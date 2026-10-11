#include "world.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

Block g_blocks[MAX_BLOCKS];
int g_nb;
uint32_t g_nextid = 1;

/* ---------------- undo/redo ---------------- */

#define UNDO_DEPTH 48
typedef struct { int n; Block blocks[MAX_BLOCKS]; } Snapshot;
static Snapshot g_undo[UNDO_DEPTH], g_redo[UNDO_DEPTH];
static int g_un, g_re;

void world_push_undo(void) {
    if (g_un == UNDO_DEPTH) {        /* drop oldest */
        memmove(&g_undo[0], &g_undo[1], sizeof g_undo - sizeof g_undo[0]);
        g_un--;
    }
    g_undo[g_un].n = g_nb;
    memcpy(g_undo[g_un].blocks, g_blocks, sizeof(Block) * g_nb);
    g_un++;
    g_re = 0;                         /* new edit clears redo */
}

static int undo_apply(Snapshot *from, Snapshot *to, int *toN) {
    if (to && *toN < UNDO_DEPTH) {
        to[*toN].n = g_nb;
        memcpy(to[*toN].blocks, g_blocks, sizeof(Block) * g_nb);
        (*toN)++;
    }
    g_nb = from->n;
    memcpy(g_blocks, from->blocks, sizeof(Block) * g_nb);
    return 1;
}

int world_undo(void) {
    if (g_un == 0) return 0;
    g_un--;
    return undo_apply(&g_undo[g_un], g_redo, &g_re);
}

int world_redo(void) {
    if (g_re == 0) return 0;
    g_re--;
    return undo_apply(&g_redo[g_re], g_undo, &g_un);
}

/* ---------------- add/remove ---------------- */

int world_add(v3 pos, v3 size, int shape, int rot, const float col[3]) {
    if (g_nb >= MAX_BLOCKS) return -1;
    Block *b = &g_blocks[g_nb];
    memset(b, 0, sizeof *b);
    b->pos = pos; b->size = size; b->shape = (uint8_t)shape; b->rot = (uint8_t)rot;
    b->alpha = 1.0f;
    b->id = g_nextid++;
    memcpy(b->col, col, sizeof(float)*3);
    return g_nb++;
}

int world_addb(const Block *src) {
    if (g_nb >= MAX_BLOCKS) return -1;
    int i = g_nb++;
    g_blocks[i] = *src;
    if (g_blocks[i].alpha <= 0.0f) g_blocks[i].alpha = 1.0f;
    if (!g_blocks[i].id) g_blocks[i].id = g_nextid++;
    if (g_blocks[i].id >= g_nextid) g_nextid = g_blocks[i].id + 1;
    return i;
}

void world_delete(int i) {
    g_blocks[i] = g_blocks[g_nb-1];
    g_nb--;
}

/* ---------------- save/load ---------------- */

#define BW_MAGIC 0x42574C42u  /* 'BWLB' */
#define BW_VER   3
#define BW_VER1_BLKSZ 40      /* v1: no trig/act */
#define BW_VER2_BLKSZ 44      /* v2: +trig/act; no texm/alpha/id/name */

int world_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t hdr[4] = {BW_MAGIC, BW_VER, (uint32_t)g_nb, g_nextid};
    int ok = fwrite(hdr, 4, 4, f) == 4 &&
             fwrite(g_blocks, sizeof(Block), (size_t)g_nb, f) == (size_t)g_nb;
    fclose(f);
    return ok;
}

static void block_defaults(Block *b) {
    b->alpha = 1.0f;
    b->id = g_nextid++;
}

/* v1/v2 record: pos,size,col (36B) + shape,rot,tex,flags (4B) [+trig,act in v2] */
void world_block_migrate(Block *b, const uint8_t *rec, int ver) {
    memset(b, 0, sizeof *b);
    memcpy(b, rec, 36);
    b->shape = rec[36]; b->rot = rec[37];
    b->tex = rec[38];   b->flags = rec[39];
    if (ver >= 2) { b->trig = rec[40]; b->act = rec[41]; }
    block_defaults(b);
}

int world_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t hdr[4] = {0};
    if (fread(hdr, 4, 3, f) != 3 || hdr[0] != BW_MAGIC ||
        (hdr[1] != 1 && hdr[1] != 2 && hdr[1] != BW_VER) || hdr[2] > MAX_BLOCKS) {
        fclose(f); return 0;
    }
    int ver = (int)hdr[1];
    g_nb = (int)hdr[2];
    if (ver == BW_VER && fread(&g_nextid, 4, 1, f) != 1) g_nextid = 1;
    int ok = 1;
    if (ver < BW_VER) {                       /* legacy fixed-size records */
        int rec = ver == 1 ? BW_VER1_BLKSZ : BW_VER2_BLKSZ;
        uint8_t buf[BW_VER2_BLKSZ];
        for (int i = 0; i < g_nb; i++) {
            if (fread(buf, rec, 1, f) != 1) { ok = 0; break; }
            world_block_migrate(&g_blocks[i], buf, ver);
        }
    } else {
        ok = fread(g_blocks, sizeof(Block), g_nb, f) == (size_t)g_nb;
        for (int i = 0; i < g_nb; i++) {
            if (g_blocks[i].alpha <= 0.0f) g_blocks[i].alpha = 1.0f;
            if (g_blocks[i].id >= g_nextid) g_nextid = g_blocks[i].id + 1;
            if (!g_blocks[i].id) g_blocks[i].id = g_nextid++;
        }
    }
    fclose(f);
    if (!ok) { g_nb = 0; return 0; }
    g_un = g_re = 0;
    return 1;
}

/* ---------------- play-mode physics ---------------- */

static float g_velY[MAX_BLOCKS];
static Block g_playBackup[MAX_BLOCKS];
static int g_playBackupN = -1;

void world_reset_physics(void) {
    memset(g_velY, 0, sizeof g_velY);
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
    float sup = 0.0f;
    float myBot = b->pos.y - es.y * 0.5f;
    int cx = (int)floorf(b->pos.x * g_cellInv);
    int cz = (int)floorf(b->pos.z * g_cellInv);
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
        int tx = cx + dx, tz = cz + dz;
        int lo = 0, hi = g_cellN;              /* first entry in cell (tx,tz) */
        while (lo < hi) {
            int m = (lo + hi) >> 1;
            int d = g_cell[m].cz - tz;
            if (!d) d = g_cell[m].cx - tx;
            if (d < 0) lo = m + 1; else hi = m;
        }
        for (int k = lo; k < g_cellN &&
                        g_cell[k].cz == tz && g_cell[k].cx == tx; k++) {
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

void world_physics(float dt, uint16_t trigHeld) {
    if (dt > 0.05f) dt = 0.05f;
    support_build();
    for (int i = 0; i < g_nb; i++) {
        Block *b = &g_blocks[i];
        if (b->flags & (BF_NOPHYS | BF_LOCKED)) continue;
        v3 es = g_es[i];
        /* BOOST: thruster while its trigger is held */
        if (b->act == ACT_BOOST && ((trigHeld >> b->trig) & 1)) {
            g_velY[i] = fminf(g_velY[i] + 30.0f * dt, 10.0f);
            b->pos.y += g_velY[i] * dt;
            continue;
        }
        float sup = (b->flags & BF_NOCOLLIDE) ? es.y * 0.5f
                                            : support_y(i) + es.y * 0.5f;
        if (b->pos.y <= sup + 0.001f) { b->pos.y = sup; g_velY[i] = 0; continue; }
        g_velY[i] -= 22.0f * dt;
        b->pos.y += g_velY[i] * dt;
        if (b->pos.y <= sup) { b->pos.y = sup; g_velY[i] = 0; }
    }
}
