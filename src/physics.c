/* physics.c - play-mode simulation (split from world.c): snapshot on play
 * entry, per-frame script-action pass (edge-triggered discrete acts, held
 * BOOST), gravity settle against an XZ uniform-grid broadphase, restore on
 * return to edit mode. See world.h for Block fields. */
#include "physics.h"
#include "world.h"
#include "model.h"
#include "app.h"
#include "rk.h"
#include "toast.h"
#include "particles.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

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
static int    s_baked;                   /* transient meshes added this play */
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
            && !(b->texm & (TEXM_OVR | TEXM_MAT(3)))
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
    if (s_nCls) { rk_models_commit(); s_baked += s_nCls; }
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
    if (s_baked) models_rebake_all();    /* drop transient slots: ids stay dense */
    s_baked = 0;
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
