/* physics.c - play-mode simulation (split from world.c): snapshot on play
 * entry, per-frame script-action pass (edge-triggered discrete acts, held
 * BOOST), gravity settle against an XZ uniform-grid broadphase, restore on
 * return to edit mode. See world.h for Block fields. */
#include "physics.h"
#include "world.h"
#include "toast.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static float boost_step(int i, float dt);

static float g_velY[MAX_BLOCKS];
static uint16_t s_prevHeld;           /* last frame's trigHeld -> edge detect */
static Block g_playBackup[MAX_BLOCKS];
static int g_playBackupN = -1;

/* ---- play-mode LMB drag state ---- */
static int   s_dragI = -1;            /* dragged block, -1 none */
static v3    s_dragOff;               /* grab hit point - block center */
static float s_dragT;                 /* ray distance at grab */
static v3    s_dragRO, s_dragRD;      /* latest cursor ray (world_drag_move) */

int world_drag_grab(v3 ro, v3 rd) {
    s_dragI = -1;
    if (world_wset()->dragOff) return 0;
    int idx = -1, ax = -1; float t = 0, sg = 0;
    if (!world_pick_block(ro, rd, &idx, &t, &ax, &sg)) return 0;
    Block *b = &g_blocks[idx];
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
    s_prevHeld = 0;
    s_dragI = -1;
}

void world_snapshot_play(void) {
    g_playBackupN = g_nb;
    memcpy(g_playBackup, g_blocks, sizeof(Block) * g_nb);
    world_reset_physics();
}

void world_restore_play(void) {
    if (g_playBackupN < 0) return;
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
typedef struct { int cz, cx, i; } SupCell;
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
            if (s->act == ACT_COLOR) memcpy(b->col, g_palette[s->actParam & 15], sizeof b->col);
            else if (s->act == ACT_TEX) b->tex = s->actParam;
            else if (s->act == ACT_DEL) delMark[t] = 1;
        }
    }
    int ndel = 0;
    for (int i = g_nb - 1; i >= 0; i--)     /* descending: swap-removes pull */
        if (delMark[i]) { world_delete(i); ndel++; }   /* from >i (consumed) */
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
        if (b->flags & (BF_NOPHYS | BF_LOCKED)) continue;
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
            float rest = b->bounce * 0.06f;
            g_velY[i] = (rest > 0.0f && -g_velY[i] > 1.5f)
                      ? -g_velY[i] * rest : 0.0f;
        }
    }
}

/* BOOST thrust delta for one block — weight stat resists (used in both
   phys modes so thrusters behave identically) */
static float boost_step(int i, float dt) {
    float wf = 1.5f - g_blocks[i].weight * (1.0f / 16.0f);
    return 30.0f * wf * dt;
}
