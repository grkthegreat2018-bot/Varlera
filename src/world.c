#include "world.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

Block g_blocks[MAX_BLOCKS];
int g_nb;
uint32_t g_nextid = 1;

/* shared 16-color palette — panel swatches and ACT_COLOR actParam both index it */
const float g_palette[16][3] = {
    {0.90f,0.92f,0.95f},{0.55f,0.58f,0.62f},{0.24f,0.26f,0.29f},{0.95f,0.85f,0.30f},
    {0.94f,0.45f,0.20f},{0.85f,0.20f,0.25f},{0.75f,0.25f,0.60f},{0.45f,0.35f,0.85f},
    {0.25f,0.55f,0.95f},{0.20f,0.75f,0.80f},{0.25f,0.80f,0.40f},{0.55f,0.85f,0.25f},
    {0.55f,0.35f,0.20f},{0.95f,0.65f,0.45f},{0.95f,0.55f,0.70f},{1.00f,1.00f,1.00f},
};

/* ---------------- undo/redo ---------------- */

#define UNDO_DEPTH 48
typedef struct { int n; Block blocks[MAX_BLOCKS]; } Snapshot;
static Snapshot g_undo[UNDO_DEPTH], g_redo[UNDO_DEPTH];
static int g_un, g_re;

static uint64_t g_rev;               /* bumped on every mutating op */

uint64_t world_rev(void) { return g_rev; }

void world_push_undo(void) {
    if (g_un == UNDO_DEPTH) {        /* drop oldest */
        memmove(&g_undo[0], &g_undo[1], sizeof g_undo - sizeof g_undo[0]);
        g_un--;
    }
    g_undo[g_un].n = g_nb;
    memcpy(g_undo[g_un].blocks, g_blocks, sizeof(Block) * g_nb);
    g_un++;
    g_re = 0;                         /* new edit clears redo */
    g_rev++;
}

static int undo_apply(Snapshot *from, Snapshot *to, int *toN) {
    if (to && *toN < UNDO_DEPTH) {
        to[*toN].n = g_nb;
        memcpy(to[*toN].blocks, g_blocks, sizeof(Block) * g_nb);
        (*toN)++;
    }
    g_nb = from->n;
    memcpy(g_blocks, from->blocks, sizeof(Block) * g_nb);
    g_rev++;
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
    g_rev++;
    return g_nb++;
}

int world_addb(const Block *src) {
    if (g_nb >= MAX_BLOCKS) return -1;
    int i = g_nb++;
    g_blocks[i] = *src;
    if (g_blocks[i].alpha <= 0.0f) g_blocks[i].alpha = 1.0f;
    if (!g_blocks[i].id) g_blocks[i].id = g_nextid++;
    if (g_blocks[i].id >= g_nextid) g_nextid = g_blocks[i].id + 1;
    g_rev++;
    return i;
}

void world_delete(int i) {
    g_blocks[i] = g_blocks[g_nb-1];
    g_nb--;
    g_rev++;
}

/* ---------------- save/load ---------------- */

#define BW_MAGIC 0x42574C42u  /* 'BWLB' */
#define BW_VER   4
#define BW_VER1_BLKSZ 40      /* v1: no trig/act */
#define BW_VER2_BLKSZ 44      /* v2: +trig/act; no texm/alpha/id/name */
#define BW_VER3_BLKSZ 64      /* v3: raw Block */

static uint32_t s_created;    /* from v4 header; now() if absent */

#define WS_MAGIC 0x34535742u    /* 'BWS4' settings trailer */
static WSet s_wset;
static void wset_defaults(void) {
    memset(&s_wset, 0, sizeof s_wset);
    s_wset.gravMul = 8;                        /* 1.0x gravity */
}
WSet *world_wset(void)     { return &s_wset; }
void  world_wset_dirty(void) { g_rev++; }

/* ---- save dir paths: <dir>/<san>.<ext>; san = [a-z0-9_-] ---- */
static char s_dir[384] = "worlds";   /* user-chosen via WORLD tab DIR */

void worlds_set_dir(const char *d) {
    snprintf(s_dir, sizeof s_dir, "%s", (d && *d) ? d : "worlds");
}
const char *worlds_dir(void) { return s_dir; }

const char *worlds_file(const char *name, const char *ext) {
    static char s_path[512];
    char san[25]; int n = 0;
    for (const char *p = name; *p && n < 24; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += (char)('a' - 'A');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) c = '_';
        san[n++] = c;
    }
    if (!n) { san[0] = 'w'; san[1] = 0; } else san[n] = 0;
    snprintf(s_path, sizeof s_path, "%s/%s.%s", s_dir, san, ext);
    return s_path;
}

void worlds_ensure(void) {
#ifdef _WIN32
    _mkdir(s_dir);
#else
    mkdir(s_dir, 0755);
#endif
}

/* v4 per-block record: flag byte + id + 9 meta bytes + u8 col + u8 alpha
   + 6xi16 quantized pos/size (or 6xf32 when out of the 1/16 grid/range)
   + optional name[12]. ~28B vs the v3 64B dump. */
#define BF4_RAWPOS  1
#define BF4_NAME    2

static int quant_ok(float v) {
    float q = v * 16.0f;
    return fabsf(q - floorf(q + 0.5f)) < 0.02f && fabsf(q) < 32700.0f;
}

int world_save(const char *path, const char *name) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (!s_created) s_created = (uint32_t)time(NULL);
    struct { uint32_t magic, ver, nblocks, nextid, created, modified;
             uint8_t name[32]; } hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = BW_MAGIC; hdr.ver = BW_VER;
    hdr.nblocks = (uint32_t)g_nb; hdr.nextid = g_nextid;
    hdr.created = s_created; hdr.modified = (uint32_t)time(NULL);
    snprintf((char *)hdr.name, 32, "%s", name && name[0] ? name : "world");
    if (fwrite(&hdr, sizeof hdr, 1, f) != 1) { fclose(f); return 0; }
    int ok = 1;
    for (int i = 0; i < g_nb && ok; i++) {
        const Block *b = &g_blocks[i];
        uint8_t fl = b->name[0] ? BF4_NAME : 0;
        int raw = !(quant_ok(b->pos.x) && quant_ok(b->pos.y) && quant_ok(b->pos.z) &&
                    quant_ok(b->size.x) && quant_ok(b->size.y) && quant_ok(b->size.z));
        if (raw) fl |= BF4_RAWPOS;
        uint8_t head[16] = { fl };
        memcpy(head + 1, &b->id, 4);
        head[5] = b->shape; head[6] = b->rot; head[7] = b->tex;
        head[8] = b->texm;  head[9] = b->flags; head[10] = b->trig;
        head[11] = b->act;  head[12] = b->actParam; head[13] = b->tgt;
        head[14] = (uint8_t)(clampf(b->alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t col3[3];
        for (int c = 0; c < 3; c++)
            col3[c] = (uint8_t)(clampf(b->col[c], 0.0f, 1.0f) * 255.0f + 0.5f);
        ok = fwrite(head, 15, 1, f) == 1 && fwrite(col3, 3, 1, f) == 1;
        if (raw) ok &= fwrite(b->pos.e, 4, 6, f) == 6;      /* pos+size are contiguous */
        else {
            int16_t q[6];
            for (int c = 0; c < 3; c++) {
                q[c]   = (int16_t)floorf(b->pos.e[c] * 16.0f + 0.5f);
                q[c+3] = (int16_t)floorf(b->size.e[c] * 16.0f + 0.5f);
            }
            ok &= fwrite(q, 12, 1, f) == 1;
        }
        if (fl & BF4_NAME) ok &= fwrite(b->name, 12, 1, f) == 1;
    }
    s_wset.magic = WS_MAGIC;                 /* settings trailer (v4) */
    ok &= fwrite(&s_wset, sizeof s_wset, 1, f) == 1;
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

int world_load(const char *path, char *nameOut) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t hdr[4] = {0};
    if (fread(hdr, 4, 3, f) != 3 || hdr[0] != BW_MAGIC ||
        hdr[1] < 1 || hdr[1] > BW_VER || hdr[2] > MAX_BLOCKS) {
        fclose(f); return 0;
    }
    int ver = (int)hdr[1];
    g_nb = (int)hdr[2];
    s_created = 0;
    wset_defaults();
    if (ver >= 3 && fread(&g_nextid, 4, 1, f) != 1) g_nextid = 1;
    int ok = 1;
    if (ver == BW_VER) {                       /* v4: meta header + packed */
        uint8_t rest[40];                      /* created, modified, name[32] */
        if (fread(rest, 1, 40, f) != 40) { fclose(f); g_nb = 0; return 0; }
        memcpy(&s_created, rest, 4);
        if (nameOut && rest[8]) { memcpy(nameOut, rest + 8, 32); nameOut[31] = 0; }
        for (int i = 0; i < g_nb && ok; i++) {
            Block *b = &g_blocks[i];
            memset(b, 0, sizeof *b);
            uint8_t head[15], col3[3];
            ok = fread(head, 15, 1, f) == 1 && fread(col3, 3, 1, f) == 1;
            if (!ok) break;
            memcpy(&b->id, head + 1, 4);
            b->shape = head[5];  b->rot = head[6];  b->tex = head[7];
            b->texm = head[8];   b->flags = head[9]; b->trig = head[10];
            b->act = head[11];   b->actParam = head[12]; b->tgt = head[13];
            b->alpha = head[14] / 255.0f;
            for (int c = 0; c < 3; c++) b->col[c] = col3[c] / 255.0f;
            if (head[0] & BF4_RAWPOS) ok = fread(b->pos.e, 4, 6, f) == 6;
            else {
                int16_t q[6];
                ok = fread(q, 12, 1, f) == 1;
                for (int c = 0; c < 3; c++) {
                    b->pos.e[c]  = q[c]   / 16.0f;
                    b->size.e[c] = q[c+3] / 16.0f;
                }
            }
            if (head[0] & BF4_NAME) ok &= fread(b->name, 12, 1, f) == 1;
            if (b->alpha <= 0.0f) b->alpha = 1.0f;
            if (b->id >= g_nextid) g_nextid = b->id + 1;
            if (!b->id) b->id = g_nextid++;
        }
        /* settings trailer is optional: absent on pre-trailer v4 files */
        WSet ws;
        if (ok && fread(&ws, sizeof ws, 1, f) == 1 && ws.magic == WS_MAGIC)
            s_wset = ws;
    } else if (ver < 3) {                      /* v1/v2 fixed-size records */
        int rec = ver == 1 ? BW_VER1_BLKSZ : BW_VER2_BLKSZ;
        uint8_t buf[BW_VER2_BLKSZ];
        for (int i = 0; i < g_nb; i++) {
            if (fread(buf, rec, 1, f) != 1) { ok = 0; break; }
            world_block_migrate(&g_blocks[i], buf, ver);
        }
    } else {                                   /* v3: raw Block dump */
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
    g_rev++;
    return 1;
}

/* ---------------- play-mode physics ---------------- */

static float g_velY[MAX_BLOCKS];
static uint16_t s_prevHeld;           /* last frame's trigHeld -> edge detect */
static Block g_playBackup[MAX_BLOCKS];
static int g_playBackupN = -1;

void world_reset_physics(void) {
    memset(g_velY, 0, sizeof g_velY);
    s_prevHeld = 0;
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
    if (ndel) memset(g_velY, 0, sizeof(float) * g_nb);

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
