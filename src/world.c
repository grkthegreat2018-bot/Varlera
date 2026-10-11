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

/* shared 16-color palette â€” panel swatches and ACT_COLOR actParam both index it */
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
    b->traction = BS_TRACT; b->bounce = BS_BOUNCE; b->weight = BS_WEIGHT;
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
#define BW_VER   5              /* v5 = v4 + 3 stat bytes in block head */
#define BW_VER1_BLKSZ 40      /* v1: no trig/act */
#define BW_VER2_BLKSZ 44      /* v2: +trig/act; no texm/alpha/id/name */
#define BW_VER3_BLKSZ 64      /* v3: raw Block */

static uint32_t s_created;    /* from v4 header; now() if absent */

#define WS_MAGIC 0x34535742u    /* 'BWS4' settings trailer */
#define MS_MAGIC 0x53574D42u    /* 'BWMS' message-table trailer */
static WSet s_wset;
char g_msgs[MSG_MAX][MSG_LEN];
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
        uint8_t head[18] = { fl };
        memcpy(head + 1, &b->id, 4);
        head[5] = b->shape; head[6] = b->rot; head[7] = b->tex;
        head[8] = b->texm;  head[9] = b->flags; head[10] = b->trig;
        head[11] = b->act;  head[12] = b->actParam; head[13] = b->tgt;
        head[14] = (uint8_t)(clampf(b->alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        head[15] = b->traction; head[16] = b->bounce; head[17] = b->weight;
        uint8_t col3[3];
        for (int c = 0; c < 3; c++)
            col3[c] = (uint8_t)(clampf(b->col[c], 0.0f, 1.0f) * 255.0f + 0.5f);
        ok = fwrite(head, 18, 1, f) == 1 && fwrite(col3, 3, 1, f) == 1;
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
    s_wset.magic = WS_MAGIC;                 /* settings trailer (v4+) */
    ok &= fwrite(&s_wset, sizeof s_wset, 1, f) == 1;
    uint32_t mh[2] = {MS_MAGIC, MSG_MAX};    /* message table trailer */
    ok &= fwrite(mh, 8, 1, f) == 1;
    ok &= fwrite(g_msgs, MSG_LEN, MSG_MAX, f) == MSG_MAX;
    fclose(f);
    return ok;
}

static void block_defaults(Block *b) {
    b->alpha = 1.0f;
    b->id = g_nextid++;
    b->traction = BS_TRACT; b->bounce = BS_BOUNCE; b->weight = BS_WEIGHT;
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
    memset(g_msgs, 0, sizeof g_msgs);
    if (ver >= 3 && fread(&g_nextid, 4, 1, f) != 1) g_nextid = 1;
    int ok = 1;
    if (ver >= 4) {                            /* v4/v5: meta header + packed */
        uint8_t rest[40];                      /* created, modified, name[32] */
        if (fread(rest, 1, 40, f) != 40) { fclose(f); g_nb = 0; return 0; }
        memcpy(&s_created, rest, 4);
        if (nameOut && rest[8]) { memcpy(nameOut, rest + 8, 32); nameOut[31] = 0; }
        int hsz = ver >= 5 ? 18 : 15;          /* v5 adds stat bytes */
        for (int i = 0; i < g_nb && ok; i++) {
            Block *b = &g_blocks[i];
            memset(b, 0, sizeof *b);
            uint8_t head[18], col3[3];
            ok = fread(head, (size_t)hsz, 1, f) == 1 && fread(col3, 3, 1, f) == 1;
            if (!ok) break;
            memcpy(&b->id, head + 1, 4);
            b->shape = head[5];  b->rot = head[6];  b->tex = head[7];
            b->texm = head[8];   b->flags = head[9]; b->trig = head[10];
            b->act = head[11];   b->actParam = head[12]; b->tgt = head[13];
            b->alpha = head[14] / 255.0f;
            if (ver >= 5) {
                b->traction = head[15]; b->bounce = head[16]; b->weight = head[17];
            } else {                           /* v4 predates stats */
                b->traction = BS_TRACT; b->bounce = BS_BOUNCE; b->weight = BS_WEIGHT;
            }
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
        /* message-table trailer (optional) */
        uint32_t mh[2];
        if (ok && fread(mh, 8, 1, f) == 1 && mh[0] == MS_MAGIC) {
            uint32_t n = mh[1] > MSG_MAX ? MSG_MAX : mh[1];
            if (fread(g_msgs, MSG_LEN, n, f) != n) ok = 0;
        }
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
            /* v3 predates stats (pad bytes were 0) -> defaults */
            g_blocks[i].traction = BS_TRACT; g_blocks[i].bounce = BS_BOUNCE;
            g_blocks[i].weight = BS_WEIGHT;
        }
    }
    fclose(f);
    if (!ok) { g_nb = 0; return 0; }
    g_un = g_re = 0;
    g_rev++;
    return 1;
}

