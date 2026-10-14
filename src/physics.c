/* physics.c - play-mode simulation (split from world.c): snapshot on play
 * entry, per-frame script-action pass (edge-triggered discrete acts, held
 * BOOST), gravity settle against an XZ uniform-grid broadphase, restore on
 * return to edit mode. See world.h for Block fields. */
#include "physics.h"
#include "world.h"
#include "model.h"
#include "app.h"
#include "camera.h"
#include "rk.h"
#include "toast.h"
#include "particles.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static float boost_step(int i, float dt);
static int tgt_match(const Block *src, const Block *b);
typedef struct { int cz, cx, i; } SupCell;
static int cmp_cell(const void *pa, const void *pb);

static float g_velY[MAX_BLOCKS];
static uint16_t s_prevHeld;           /* last frame's trigHeld -> edge detect */
static float s_emitAcc[MAX_BLOCKS];   /* per-block particle emit accumulators */
static uint32_t s_phFrame;            /* physics frame counter (emit parity) */
static Block g_playBackup[MAX_BLOCKS];
static int g_playBackupN = -1;

/* ---- play-mode LMB drag state ---- */
static int   s_dragI = -1;            /* dragged block, -1 none */
static v3    s_dragOff;               /* grab hit point - block center */
static float s_dragT;                 /* ray distance at grab */
static v3    s_dragRO, s_dragRD;      /* latest cursor ray (world_drag_move) */

/* ---- greedy merge: settled script-free unreachable blocks bake into one
 * transient mesh per touching cluster at play entry. Members keep their
 * g_blocks slots (support/pick unchanged) but skip draw+settle via
 * BF_MERGED; a script touch / drag grab / delete dissolves the cluster. */
#define MRG_MAX_CLS 24
typedef struct { int mesh; v3 pos, size; } MCls;
static MCls   s_cls[MRG_MAX_CLS];
static int    s_nCls;
static int16_t s_blkCls[MAX_BLOCKS];     /* block idx -> cluster, -1 none */

static void merge_dissolve(int ci) {
    if (ci < 0 || ci >= s_nCls) return;
    for (int i = 0; i < g_nb; i++) {
        if (s_blkCls[i] == ci) { g_blocks[i].flags &= (uint8_t)~BF_MERGED; s_blkCls[i] = -1; }
        else if (s_blkCls[i] > ci) s_blkCls[i]--;
    }
    s_cls[ci] = s_cls[--s_nCls];
}

static int uf_find(int *uf, int i) {
    while (uf[i] != i) { uf[i] = uf[uf[i]]; i = uf[i]; }
    return i;
}

static void merge_build(void) {
    static uint8_t reach[MAX_BLOCKS], elig[MAX_BLOCKS];
    static int     uf[MAX_BLOCKS];
    s_nCls = 0;
    for (int i = 0; i < g_nb; i++) s_blkCls[i] = -1;

    /* reachable = targetable by a mutating discrete act (or self-acting) */
    memset(reach, 0, (size_t)g_nb);
    for (int s = 0; s < g_nb; s++) {
        const Block *sb = &g_blocks[s];
        if (sb->act != ACT_COLOR && sb->act != ACT_TEX && sb->act != ACT_DEL) continue;
        if ((sb->tgt & 3) == TGT_SELF) { reach[s] = 1; continue; }
        for (int t = 0; t < g_nb; t++)
            if (tgt_match(sb, &g_blocks[t])) reach[t] = 1;
    }
    int ne = 0;
    for (int i = 0; i < g_nb; i++) {
        const Block *b = &g_blocks[i];
        /* ineligible: scripted/reachable, translucent, material-finished,
           spinning, locked, or non-STRETCH textured (bake can't carry it) */
        elig[i] = (uint8_t)(b->act == ACT_NONE && !reach[i]
            && !(b->flags & (BF_LOCKED | BF_GLASS | BF_SPIN))
            && b->alpha >= 0.999f
            && !(b->texm & (TEXM_OVR | TEXM_MAT(3) |
                            TEXM_HAZARD | TEXM_PICKUP))
            && !(b->tex && (b->texm & 3) != TEXM_STRETCH));
        uf[i] = i;
        ne += elig[i];
    }
    if (ne < 2) return;

    /* XZ cell broadphase over the eligible set -> union touching AABBs.
     * Cell size = max eligible XZ extent: touching pairs always share or
     * neighbor cells. */
    static SupCell ec[MAX_BLOCKS];
    float cs = 1.0f;
    int en = 0;
    for (int i = 0; i < g_nb; i++) if (elig[i]) {
        v3 es = block_eff_size(&g_blocks[i]);
        if (es.x > cs) cs = es.x;
        if (es.z > cs) cs = es.z;
    }
    if (cs < 4.0f) cs = 4.0f;
    float ci1 = 1.0f / cs;
    for (int i = 0; i < g_nb; i++) if (elig[i]) {
        ec[en].cx = (int)floorf(g_blocks[i].pos.x * ci1);
        ec[en].cz = (int)floorf(g_blocks[i].pos.z * ci1);
        ec[en].i = i; en++;
    }
    qsort(ec, (size_t)en, sizeof ec[0], cmp_cell);
    for (int i = 0; i < g_nb; i++) {
        if (!elig[i]) continue;
        v3 es = block_eff_size(&g_blocks[i]);
        int tx = (int)floorf(g_blocks[i].pos.x * ci1);
        int tz = (int)floorf(g_blocks[i].pos.z * ci1);
        for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            int lo = 0, hi = en, kz = tz + dz, kx = tx + dx;
            while (lo < hi) { int m = (lo + hi) >> 1;
                if (ec[m].cz < kz || (ec[m].cz == kz && ec[m].cx < kx))
                    lo = m + 1;
                else hi = m;
            }
            for (int k = lo; k < en && ec[k].cz == kz && ec[k].cx == kx; k++) {
                int j = ec[k].i;
                if (j <= i) continue;
                v3 eb = block_eff_size(&g_blocks[j]);
                float dxo = fabsf(g_blocks[i].pos.x - g_blocks[j].pos.x)
                          - (es.x + eb.x) * 0.5f;
                float dzo = fabsf(g_blocks[i].pos.z - g_blocks[j].pos.z)
                          - (es.z + eb.z) * 0.5f;
                float dyo = fabsf(g_blocks[i].pos.y - g_blocks[j].pos.y)
                          - (es.y + eb.y) * 0.5f;
                if (dxo < 0.06f && dzo < 0.06f && dyo < 0.06f) {
                    int ra = uf_find(uf, i), rb = uf_find(uf, j);
                    if (ra != rb) uf[rb] = ra;
                }
            }
        }
    }

    /* cluster roots -> transient bakes (skip oversized/singleton groups) */
    static Block memb[MODEL_MAX_CHILDREN];
    for (int i = 0; i < g_nb && s_nCls < MRG_MAX_CLS; i++) {
        if (!elig[i] || uf_find(uf, i) != i) continue;
        int nm = 0;
        for (int j = i; j < g_nb; j++)
            if (elig[j] && uf_find(uf, j) == i) nm++;
        if (nm < 2 || nm > MODEL_MAX_CHILDREN) continue;
        nm = 0;
        for (int j = i; j < g_nb; j++)
            if (elig[j] && uf_find(uf, j) == i) memb[nm++] = g_blocks[j];
        v3 pivot, size;
        int mesh = model_bake_transient(memb, nm, &pivot, &size);
        if (mesh < 0) continue;                        /* vert/slot cap hit */
        s_cls[s_nCls].mesh = mesh;
        s_cls[s_nCls].pos = pivot; s_cls[s_nCls].size = size;
        int ci = s_nCls++;
        for (int j = 0; j < g_nb; j++)
            if (elig[j] && uf_find(uf, j) == i) {
                g_blocks[j].flags |= BF_MERGED;
                s_blkCls[j] = (int16_t)ci;
            }
    }
    if (s_nCls) rk_models_commit();
}

int world_merged_count(void) { return s_nCls; }

void world_merged_draw(void) {
    /* cluster inst: unit-normalized mesh scaled back to the group AABB */
    float pl[6][4]; frustum_planes(A.viewProj, pl);
    for (int i = 0; i < s_nCls; i++) {
        const MCls *c = &s_cls[i];
        RkInst in;
        memcpy(in.pos, c->pos.e, 12);
        memcpy(in.scale, c->size.e, 12);
        in.color = 0xFFFFFFFFu; in.flags = RKIF_VCOL;
        v3 half = v3_scale(c->size, 0.5f);
        if (aabb_in_frustum(pl, c->pos, half)) rk_world(c->mesh, &in, 1);
        else if (rk_can_shadow_cast(c->pos, half)) rk_caster(c->mesh, &in, 1);
    }
}

int world_drag_grab(v3 ro, v3 rd) {
    s_dragI = -1;
    if (world_wset()->dragOff) return 0;
    int idx = -1, ax = -1; float t = 0, sg = 0;
    if (!world_pick_block(ro, rd, &idx, &t, &ax, &sg)) return 0;
    Block *b = &g_blocks[idx];
    if (b->flags & BF_MERGED) {             /* grabbing pops the cluster */
        merge_dissolve(s_blkCls[idx]);
        b = &g_blocks[idx];
    }
    if (b->flags & BF_NOPHYS) return 0;
    s_dragI = idx;
    s_dragT = t;
    s_dragOff = v3_sub(v3_make(ro.x + rd.x*t, ro.y + rd.y*t, ro.z + rd.z*t),
                       b->pos);
    return 1;
}
void world_drag_move(v3 ro, v3 rd) { s_dragRO = ro; s_dragRD = rd; }
void world_drag_release(void) {
    if (s_dragI >= 0) g_velY[s_dragI] = 0.0f;
    s_dragI = -1;
}
int world_dragging(void) { return s_dragI >= 0; }

void world_reset_physics(void) {
    memset(g_velY, 0, sizeof g_velY);
    memset(s_emitAcc, 0, sizeof s_emitAcc);
    s_prevHeld = 0;
    s_dragI = -1;
    particles_reset();
    s_nCls = 0;
    rk_models_drop_transient();          /* cluster slots die here */
    for (int i = 0; i < g_nb; i++) s_blkCls[i] = -1;
}

/* held-trigger emitters (BOOST exhaust / SPIN sparks) — runs for every
   block each physics frame in both phys modes; cosmetic, ignores NOPHYS */
static void emit_continuous(int i, float dt, uint16_t trigHeld) {
    Block *b = &g_blocks[i];
    if (!((trigHeld >> (b->trig & 15)) & 1) ||
        (b->act != ACT_BOOST && b->act != ACT_SPIN)) {
        s_emitAcc[i] = 0.0f;
        return;
    }
    s_emitAcc[i] += dt;
    v3 es = block_eff_size(b);
    if (b->act == ACT_BOOST) {
        if (s_emitAcc[i] < 0.028f) return;
        s_emitAcc[i] = 0.0f;
        v3 at = b->pos; at.y -= es.y * 0.5f;
        particles_burst_dir(P_FIRE, at, v3_make(0, -1, 0), NULL, 1);
        if (((s_phFrame + (uint32_t)i) & 3) == 0)
            particles_burst(P_SMOKE, at, NULL, 1);
    } else {                        /* ACT_SPIN: occasional top sparks */
        if (s_emitAcc[i] < 0.14f) return;
        s_emitAcc[i] = 0.0f;
        v3 at = b->pos; at.y += es.y * 0.5f;
        particles_burst(P_SPARK, at, NULL, 1);
    }
}

void world_snapshot_play(void) {
    g_playBackupN = g_nb;
    memcpy(g_playBackup, g_blocks, sizeof(Block) * g_nb);
    world_reset_physics();
    merge_build();                       /* greedy cluster meshes -> GPU */
}

void world_restore_play(void) {
    if (g_playBackupN < 0) return;
    /* transient slots drop in world_reset_physics below — free, no GPU
     * wait and no persistent-model rebake */
    g_nb = g_playBackupN;
    memcpy(g_blocks, g_playBackup, sizeof(Block) * g_nb);
    g_playBackupN = -1;
    world_reset_physics();
}

/* XZ broadphase for support_y: cell size = max XZ extent, so any pair that
 * can overlap has centers at most one cell apart (checked 3x3 window). */
static v3     g_es[MAX_BLOCKS];
static float  g_cellInv;
static int    g_cellN;
static SupCell g_cell[MAX_BLOCKS];

static int cmp_cell(const void *pa, const void *pb) {
    const SupCell *a = pa, *b = pb;
    return a->cz != b->cz ? a->cz - b->cz : a->cx - b->cx;
}

static void support_build(void) {
    float cs = 1.0f;
    for (int i = 0; i < g_nb; i++) {
        g_es[i] = block_eff_size(&g_blocks[i]);
        if (g_es[i].x > cs) cs = g_es[i].x;
        if (g_es[i].z > cs) cs = g_es[i].z;
    }
    if (cs < 4.0f) cs = 4.0f;
    g_cellInv = 1.0f / cs;
    for (int i = 0; i < g_nb; i++) {
        g_cell[i].cx = (int)floorf(g_blocks[i].pos.x * g_cellInv);
        g_cell[i].cz = (int)floorf(g_blocks[i].pos.z * g_cellInv);
        g_cell[i].i = i;
    }
    g_cellN = g_nb;
    qsort(g_cell, (size_t)g_cellN, sizeof g_cell[0], cmp_cell);
}

/* support height under block i: ground or top face of any overlapping block */
static float support_y(int i) {
    Block *b = &g_blocks[i];
    v3 es = g_es[i];
    float sup = 0.0f, myBot = b->pos.y - es.y * 0.5f;
    int tx = (int)floorf(b->pos.x * g_cellInv);
    int tz = (int)floorf(b->pos.z * g_cellInv);
    for (int dz = -1; dz <= 1; dz++)
    for (int dx = -1; dx <= 1; dx++) {
        /* binary-search cell start in sorted (cz,cx) order */
        int lo = 0, hi = g_cellN;
        int kz = tz + dz, kx = tx + dx;
        while (lo < hi) { int m = (lo + hi) >> 1;
            if (g_cell[m].cz < kz || (g_cell[m].cz == kz && g_cell[m].cx < kx))
                lo = m + 1;
            else hi = m;
        }
        for (int k = lo; k < g_cellN &&
                        g_cell[k].cz == kz && g_cell[k].cx == kx; k++) {
            int j = g_cell[k].i;
            if (j == i) continue;
            Block *o = &g_blocks[j];
            if (o->flags & BF_NOCOLLIDE) continue;   /* can't support anything */
            v3 oe = g_es[j];
            if (!aabb_overlap_xz(b->pos, es, o->pos, oe, 0.0f)) continue;
            float top = o->pos.y + oe.y * 0.5f;
            if (top <= myBot + 0.6f && top > sup) sup = top; /* must be below us */
        }
    }
    return sup;
}

/* does script action on `src` reach block `b`? (tgt scoping) */
static int tgt_match(const Block *src, const Block *b) {
    switch (src->tgt & 3) {
    case TGT_ASSET: return b->shape == src->shape;
    case TGT_NAME:  return src->name[0] && !strcmp(b->name, src->name);
    case TGT_ALL:   return 1;
    default:        return b == src;
    }
}

void world_physics(float dt, uint16_t trigHeld) {
    if (dt > 0.05f) dt = 0.05f;
    /* discrete actions (COLOR/TEX/DEL) fire on the trigger edge; continuous
       acts (SPIN/GLOW render-side, BOOST below) key off the held bit */
    uint16_t edge = trigHeld & ~s_prevHeld;
    s_prevHeld = trigHeld;
    s_phFrame++;
    static uint8_t delMark[MAX_BLOCKS];
    memset(delMark, 0, (size_t)g_nb);
    for (int i = 0; i < g_nb; i++) {
        Block *s = &g_blocks[i];
        if (s->act < ACT_COLOR || !((edge >> s->trig) & 1)) continue;
        if (s->act >= ACT_SAY) {                 /* text acts: global */
            const char *m = g_msgs[s->actParam & (MSG_MAX - 1)];
            if (*m) toast_say(m);              /* TITLE styling later */
            continue;
        }
        for (int t = 0; t < g_nb; t++) {
            Block *b = &g_blocks[t];
            if (!tgt_match(s, b)) continue;
            if (b->flags & BF_MERGED) merge_dissolve(s_blkCls[t]);
            if (s->act == ACT_COLOR) {
                memcpy(b->col, g_palette[s->actParam & 15], sizeof b->col);
                particles_burst(P_PUFF, b->pos, NULL, 1);
            } else if (s->act == ACT_TEX) {
                b->tex = s->actParam;
                particles_burst(P_PUFF, b->pos, NULL, 1);
            } else if (s->act == ACT_DEL) delMark[t] = 1;
        }
    }
    int ndel = 0;
    for (int i = g_nb - 1; i >= 0; i--)     /* descending: swap-removes pull */
        if (delMark[i]) {                     /* debris burst, then delete */
            if (g_blocks[i].flags & BF_MERGED) merge_dissolve(s_blkCls[i]);
            particles_burst(P_DEBRIS, g_blocks[i].pos, g_blocks[i].col, 6);
            particles_burst(P_PUFF, g_blocks[i].pos, NULL, 2);
            world_delete(i); ndel++;
            s_blkCls[i] = s_blkCls[g_nb];     /* swapped-in tail's cluster id */
        }
    if (ndel) { memset(g_velY, 0, sizeof(float) * g_nb); s_dragI = -1; }

    /* drag target along the stored ray; Y never sinks below support */
    v3 dragT = s_dragI >= 0 ? v3_sub(v3_make(s_dragRO.x + s_dragRD.x*s_dragT,
                                             s_dragRO.y + s_dragRD.y*s_dragT,
                                             s_dragRO.z + s_dragRD.z*s_dragT),
                                     s_dragOff)
                            : v3_make(0,0,0);

    /* RIGID world setting: joints are perfect — script actions still fire
       (incl. BOOST below) but nothing settles under gravity */
    const WSet *ws = world_wset();
    if (ws->phys == WS_PHYS_RIGID) {
        for (int i = 0; i < g_nb; i++) {
            Block *b = &g_blocks[i];
            emit_continuous(i, dt, trigHeld);
            if (i == s_dragI) {                 /* kinematic drag */
                v3 es = block_eff_size(b);
                b->pos.x = dragT.x; b->pos.z = dragT.z;
                b->pos.y = fmaxf(dragT.y, es.y * 0.5f);
                continue;
            }
            if (b->act == ACT_BOOST && !(b->flags & (BF_NOPHYS | BF_LOCKED)) &&
                ((trigHeld >> b->trig) & 1))
                b->pos.y += boost_step(i, dt);
        }
        return;
    }

    /* SETTLE: gravity scale comes from the world settings (gravMul/8) */
    const float grav = 22.0f * (ws->gravMul ? ws->gravMul : 8) / 8.0f;
    support_build();
    for (int i = 0; i < g_nb; i++) {
        Block *b = &g_blocks[i];
        emit_continuous(i, dt, trigHeld);   /* thrusters/sparks run always */
        if (b->flags & (BF_NOPHYS | BF_LOCKED | BF_MERGED)) continue;
        v3 es = g_es[i];
        /* kinematic drag: follow cursor ray, can't sink through support */
        if (i == s_dragI) {
            float sup = (b->flags & BF_NOCOLLIDE) ? es.y * 0.5f
                                                : support_y(i) + es.y * 0.5f;
            b->pos.x = dragT.x; b->pos.z = dragT.z;
            b->pos.y = fmaxf(dragT.y, sup);
            g_velY[i] = 0;
            continue;
        }
        /* BOOST: thruster while its trigger is held; weight resists lift
           (w0 -> 1.5x, w8 -> 1.0x, w15 -> 0.56x) */
        if (b->act == ACT_BOOST && ((trigHeld >> b->trig) & 1)) {
            g_velY[i] = fminf(g_velY[i] + boost_step(i, dt), 10.0f);
            b->pos.y += g_velY[i] * dt;
            continue;
        }
        float sup = (b->flags & BF_NOCOLLIDE) ? es.y * 0.5f
                                            : support_y(i) + es.y * 0.5f;
        if (b->pos.y <= sup + 0.001f) { b->pos.y = sup; g_velY[i] = 0; continue; }
        g_velY[i] -= grav * dt;
        b->pos.y += g_velY[i] * dt;
        if (b->pos.y <= sup) {          /* landing: bounce restitution 0..0.9 */
            b->pos.y = sup;
            float imp = -g_velY[i];
            if (imp > 2.5f) {           /* hard landing -> dust at the base */
                v3 at = b->pos; at.y -= es.y * 0.5f;
                particles_burst(P_DUST, at, NULL,
                                (int)clampf(imp * 0.9f, 3.0f, 12.0f));
            }
            float rest = b->bounce * 0.06f;
            g_velY[i] = (rest > 0.0f && imp > 1.5f)
                      ? imp * rest : 0.0f;
        }
    }
}

/* BOOST thrust delta for one block — weight stat resists (used in both
   phys modes so thrusters behave identically) */
static float boost_step(int i, float dt) {
    float wf = 1.5f - g_blocks[i].weight * (1.0f / 16.0f);
    return 30.0f * wf * dt;
}

/* ---- walk-mode player controller (WSet.playerMode) ----
 * First-person kinematic box (feet-anchored) vs block AABBs: integrate
 * per-axis, push back out on overlap. y=0 is the world floor. */
static v3   s_plPos, s_plVel, s_plSpawn, s_plSavedCam;
static int  s_plGround, s_plActive, s_goalsTotal, s_plStandI = -1;
static int  s_hp, s_score; static float s_hurtT;
static float s_coyote, s_jumpBuf;
static uint8_t s_goalHit[MAX_BLOCKS], s_taken[MAX_BLOCKS];
#define PL_R   0.32f
#define PL_H   1.75f
#define PL_EYE 1.62f

int world_player_active(void) { return s_plActive; }
int world_player_hp(void)     { return s_hp; }
int world_player_score(void)  { return s_score; }
int world_player_taken(int i) { return s_plActive && s_taken[i]; }

void world_player_enter(void) {
    s_plActive = world_wset()->playerMode & 1;
    s_plSavedCam = A.camPos;
    s_plSpawn = v3_make(A.camPos.x, A.camPos.y - PL_EYE, A.camPos.z);
    if (s_plSpawn.y < 0.5f) s_plSpawn.y = 0.5f;
    /* designated spawn blocks override the camera drop point */
    for (int i = 0; s_plActive && i < g_nb; i++)
        if (g_blocks[i].flags & BF_SPAWN) {
            s_plSpawn = g_blocks[i].pos;
            s_plSpawn.y += block_eff_size(&g_blocks[i]).y * 0.5f + 0.05f;
            break;
        }
    memset(s_goalHit, 0, sizeof s_goalHit);
    memset(s_taken, 0, sizeof s_taken);
    s_hp = 3; s_score = 0; s_hurtT = 0.0f;
    s_goalsTotal = 0;
    for (int i = 0; i < g_nb; i++)
        if (g_blocks[i].flags & BF_GOAL) s_goalsTotal++;
    s_plPos = s_plSpawn; s_plVel = v3_make(0, 0, 0); s_plGround = 0;
    s_plStandI = -1;
}

void world_player_exit(void) {
    if (s_plActive) { A.camPos = s_plSavedCam; A.camVel = v3_make(0, 0, 0); }
    s_plActive = 0;
}

static void pl_move_axis(float dt, int axis) {
    s_plPos.e[axis] += s_plVel.e[axis] * dt;
    const float ph[3] = {PL_R, PL_H * 0.5f, PL_R};
    v3 c = v3_make(s_plPos.x, s_plPos.y + PL_H * 0.5f, s_plPos.z);
    for (int i = 0; i < g_nb; i++) {
        const Block *b = &g_blocks[i];
        if (b->flags & BF_NOCOLLIDE) continue;
        if (b->texm & TEXM_PICKUP) continue;    /* walk through pickups */
        v3 bh = v3_scale(block_eff_size(b), 0.5f);
        v3 d = v3_sub(c, b->pos);
        float ox = ph[0] + bh.x - fabsf(d.x);
        float oy = ph[1] + bh.y - fabsf(d.y);
        float oz = ph[2] + bh.z - fabsf(d.z);
        if (ox <= 0.0f || oy <= 0.0f || oz <= 0.0f) continue;
        float pen = axis == 0 ? ox : axis == 1 ? oy : oz;
        float dir = d.e[axis] >= 0.0f ? 1.0f : -1.0f;
        s_plPos.e[axis] += pen * dir;
        c.e[axis] += pen * dir;
        if (axis == 1) { if (dir > 0.0f) { s_plGround = 1; s_plStandI = i; }
                         s_plVel.y = 0.0f; }
        else           s_plVel.e[axis] = 0.0f;
    }
}

void world_player_tick(float dt) {
    if (!s_plActive) return;
    CamBasis cb = cam_basis();
    v3 f = v3_make(cb.f.x, 0, cb.f.z);
    v3 r = v3_make(cb.r.x, 0, cb.r.z);
    if (v3_len(f) > 1e-4f) f = v3_norm(f);
    if (v3_len(r) > 1e-4f) r = v3_norm(r);
    v3 wish = v3_make(0, 0, 0);
    if (glfwGetKey(A.win, GLFW_KEY_W) == GLFW_PRESS) wish = v3_add(wish, f);
    if (glfwGetKey(A.win, GLFW_KEY_S) == GLFW_PRESS) wish = v3_sub(wish, f);
    if (glfwGetKey(A.win, GLFW_KEY_D) == GLFW_PRESS) wish = v3_add(wish, r);
    if (glfwGetKey(A.win, GLFW_KEY_A) == GLFW_PRESS) wish = v3_sub(wish, r);
    if (v3_len(wish) > 0.0f) wish = v3_norm(wish);
    float spd = glfwGetKey(A.win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
              ? 3.2f : 6.5f;                              /* shift = walk */
    /* ground accel: the standing block's traction stat is the grip —
       0 = ice slide, 15 = instant; bare floor = full; air = weak control */
    float rate = !s_plGround ? 3.5f
               : s_plStandI >= 0 ? 6.0f + g_blocks[s_plStandI].traction * (34.0f/15.0f)
               : 40.0f;
    float kf = 1.0f - expf(-rate * dt);
    s_plVel.x += (wish.x * spd - s_plVel.x) * kf;
    s_plVel.z += (wish.z * spd - s_plVel.z) * kf;
    /* jump feel: SPACE buffers 0.12s; ground grace (coyote) 0.12s */
    if (glfwGetKey(A.win, GLFW_KEY_SPACE) == GLFW_PRESS) s_jumpBuf = 0.12f;
    else if (s_jumpBuf > 0.0f) s_jumpBuf -= dt;
    if (s_plGround) s_coyote = 0.12f; else if (s_coyote > 0.0f) s_coyote -= dt;
    if (s_jumpBuf > 0.0f && s_coyote > 0.0f) {
        s_plVel.y = 9.0f; s_plGround = 0;
        s_coyote = 0.0f; s_jumpBuf = 0.0f;
    }
    s_plVel.y -= 26.0f * dt;
    s_plGround = 0; s_plStandI = -1;
    pl_move_axis(dt, 0); pl_move_axis(dt, 2); pl_move_axis(dt, 1);
    if (s_plPos.y < 0.0f) { s_plPos.y = 0.0f; s_plVel.y = 0.0f; s_plGround = 1; }
    if (s_plPos.y < -40.0f) { s_plPos = s_plSpawn; s_plVel = v3_make(0,0,0); }
    /* goal blocks: first touch fires a burst + toast (per play session) */
    if (s_goalsTotal) {
        v3 c = v3_make(s_plPos.x, s_plPos.y + PL_H * 0.5f, s_plPos.z);
        const float ph[3] = {PL_R, PL_H * 0.5f, PL_R};
        int hit = 0;
        for (int i = 0; i < g_nb; i++) {
            const Block *b = &g_blocks[i];
            if (!(b->flags & BF_GOAL) || s_goalHit[i]) continue;
            v3 bh = v3_scale(block_eff_size(b), 0.5f);
            v3 d = v3_sub(c, b->pos);
            if (ph[0] + bh.x - fabsf(d.x) <= 0.0f) continue;
            if (ph[1] + bh.y - fabsf(d.y) <= 0.0f) continue;
            if (ph[2] + bh.z - fabsf(d.z) <= 0.0f) continue;
            s_goalHit[i] = 1; hit++;
            particles_burst(P_SPARK, b->pos, NULL, 14);
        }
        if (hit) {
            int done = 0;
            for (int i = 0; i < g_nb; i++) done += s_goalHit[i] != 0;
            char msg[24];
            snprintf(msg, sizeof msg, done >= s_goalsTotal
                     ? "ALL GOALS!" : "GOAL %d/%d", done, s_goalsTotal);
            toast_say(msg);
        }
    }
    /* hazard + pickup sweep: same player-AABB test as goals */
    if (s_hurtT > 0.0f) s_hurtT -= dt;
    for (int i = 0; i < g_nb; i++) {
        const Block *b = &g_blocks[i];
        int t = b->texm & (TEXM_HAZARD | TEXM_PICKUP);
        if (!(t || (b->flags & BF_SPAWN)) || s_taken[i]) continue;
        v3 c = v3_make(s_plPos.x, s_plPos.y + PL_H * 0.5f, s_plPos.z);
        const float ph[3] = {PL_R, PL_H * 0.5f, PL_R};
        v3 bh = v3_scale(block_eff_size(b), 0.5f);
        v3 d = v3_sub(c, b->pos);
        if (ph[0] + bh.x - fabsf(d.x) <= 0.0f) continue;
        if (ph[1] + bh.y - fabsf(d.y) <= 0.0f) continue;
        if (ph[2] + bh.z - fabsf(d.z) <= 0.0f) continue;
        if (b->flags & BF_SPAWN) {              /* checkpoint: touch updates */
            v3 np = b->pos;
            np.y += bh.y + 0.05f;
            if (v3_dist(np, s_plSpawn) > 0.25f) {
                s_plSpawn = np;
                particles_burst(P_SPARK, b->pos, NULL, 8);
                toast_say("CHECKPOINT");
            }
            continue;
        }
        if (t & TEXM_PICKUP) {
            s_taken[i] = 1; s_score++;
            particles_burst(P_SPARK, b->pos, NULL, 10);
            char msg[20]; snprintf(msg, sizeof msg, "+1  (%d)", s_score);
            toast_say(msg);
        } else if (s_hurtT <= 0.0f) {
            s_hurtT = 1.0f; s_hp--;
            v3 k = v3_norm(v3_make(d.x, 0, d.z));
            s_plVel = v3_add(s_plVel,
                             v3_add(v3_scale(k, 6.0f), v3_make(0, 4.0f, 0)));
            particles_burst(P_DEBRIS, b->pos, NULL, 8);
            if (s_hp <= 0) {
                s_plPos = s_plSpawn; s_plVel = v3_make(0, 0, 0);
                s_hp = 3; toast_say("RESPAWNED");
            } else {
                char msg[20]; snprintf(msg, sizeof msg, "OUCH! HP %d", s_hp);
                toast_say(msg);
            }
        }
    }
    A.camPos = v3_make(s_plPos.x, s_plPos.y + PL_EYE, s_plPos.z);
    A.camVel = v3_make(0, 0, 0);
}
