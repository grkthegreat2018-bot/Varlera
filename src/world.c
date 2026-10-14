#include "world.h"
#include "lzss.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <pthread.h>
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
#define BW_VER   7              /* v7 = v6 records LZSS-compressed as a blob */
#define BW_VER1_BLKSZ 40      /* v1: no trig/act */
#define BW_VER2_BLKSZ 44      /* v2: +trig/act; no texm/alpha/id/name */
#define BW_VER3_BLKSZ 64      /* v3: raw Block */

static uint32_t s_created;    /* from v4 header; now() if absent */

#define WS_MAGIC 0x34535742u    /* 'BWS4' settings trailer */
#define MS_MAGIC 0x53574D42u    /* 'BWMS' message-table trailer */
#define ST_MAGIC 0x54535742u    /* 'BWST' per-block-string trailer */
static WSet s_wset;
char g_msgs[MSG_MAX][MSG_LEN];
static BMsg s_bmsgs[BMSG_MAX];
static int  s_nbmsg;

const char *world_block_msg(uint32_t id) {
    for (int i = 0; i < s_nbmsg; i++)
        if (s_bmsgs[i].id == id) return s_bmsgs[i].text;
    return NULL;
}
char *world_block_msg_edit(uint32_t id) {
    for (int i = 0; i < s_nbmsg; i++)
        if (s_bmsgs[i].id == id) return s_bmsgs[i].text;
    if (s_nbmsg >= BMSG_MAX) return NULL;
    s_bmsgs[s_nbmsg].id = id;
    s_bmsgs[s_nbmsg].text[0] = 0;
    return s_bmsgs[s_nbmsg++].text;
}
static void wset_defaults(void) {
    memset(&s_wset, 0, sizeof s_wset);
    s_wset.gravMul = 8;                        /* 1.0x gravity */
}
WSet *world_wset(void)     { return &s_wset; }

/* EXPERIMENTAL render settings (advRender gate): idx 0 = auto default.
 * fog steps in world units; bloom is the composite mix strength. */
void world_fog_params(float *near_, float *far_) {
    static const float N[] = {60.0f, 30.0f, 90.0f, 150.0f};
    static const float F[] = {170.0f, 120.0f, 260.0f, 400.0f};
    *near_ = s_wset.advRender ? N[s_wset.fogNear & 3] : 60.0f;
    *far_  = s_wset.advRender ? F[s_wset.fogFar & 3]  : 170.0f;
}
float world_fog_curve(void) {
    static const float C[] = {1.0f, 1.5f, 2.0f, 3.0f};
    return s_wset.advRender ? C[s_wset.fogCurve & 3] : 1.0f;
}
float world_pcf_rad(void) {
    static const float P[] = {0.5f, 1.0f, 2.0f, 3.0f};
    return s_wset.advRender ? P[s_wset.pcfRad & 3] : 0.5f;
}
float world_bloom_amt(void) {
    static const float AMT[] = {0.55f, 0.0f, 0.30f, 1.0f};
    return s_wset.advRender ? AMT[s_wset.bloom & 3] : 0.55f;
}
int world_shadow_res(void) {
    static const int R[] = {1024, 2048, 4096};
    return s_wset.advRender ? R[s_wset.shRes % 3] : 2048;
}
void  world_wset_dirty(void) { g_rev++; }

/* ---- day/night cycle ----
 * phase in seconds-fraction [0,1); speeds: idx -> full cycle seconds. */
static float s_dayT = 0.30f;   /* runtime phase, seeded below by dayPhase */
static int   s_dayInit = 0;

static const float DAY_LEN[] = {0.0f, 600.0f, 120.0f, 30.0f};

void world_day_tick(float dt) {
    if (!s_dayInit) { s_dayT = s_wset.dayPhase / 255.99f; s_dayInit = 1; }
    int c = s_wset.dayCycle & 3;
    if (!c || DAY_LEN[c] <= 0.0f) return;
    s_dayT += dt / DAY_LEN[c];
    s_dayT -= floorf(s_dayT);
    s_wset.dayPhase = (uint8_t)(s_dayT * 255.99f);   /* persists in trailer */
}

void world_day_set(float t) {          /* external phase set (SET TIME) */
    s_dayT = t - floorf(t); s_dayInit = 1;
    s_wset.dayPhase = (uint8_t)(s_dayT * 255.99f);
}

void world_day_reload(void) { s_dayInit = 0; }   /* re-seed on world load */

v3 world_sun_dir(void) {
    if (!s_dayInit) { s_dayT = s_wset.dayPhase / 255.99f; s_dayInit = 1; }
    if (!(s_wset.dayCycle & 3))
        return v3_norm(v3_make(0.72f, 0.55f, 0.30f));   /* static default */
    float a = 6.2831853f * (s_dayT - 0.25f);            /* noon at t=0.5? */
    v3 d = v3_make(cosf(a) * 0.8f, sinf(a), 0.35f);
    if (d.y < 0.05f)                     /* below horizon -> moon side */
        d = v3_make(-d.x, 0.15f - d.y * 0.4f, -d.z);
    return v3_norm(d);
}

float world_day_k(void) {
    if (!(s_wset.dayCycle & 3)) return 1.0f;
    float a = 6.2831853f * (s_dayT - 0.25f);
    float u = sinf(a);                              /* raw sun height */
    /* daylight 0..1: full dark below -0.1, full light above +0.35 */
    return clampf((u + 0.1f) / 0.45f, 0.0f, 1.0f);
}

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

/* serialize+write a consistent snapshot — shared by world_save (sync) and
 * the worker thread behind world_save_async. Never touches g_blocks. */
static int save_body(FILE *f, const Block *blocks, int nb, uint32_t nextid,
                     uint32_t created, const WSet *wset,
                     const char (*msgs)[MSG_LEN],
                     const BMsg *bmsgs, int nbmsg, const char *name) {
    struct { uint32_t magic, ver, nblocks, nextid, created, modified;
             uint8_t name[32]; } hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = BW_MAGIC; hdr.ver = BW_VER;
    hdr.nblocks = (uint32_t)nb; hdr.nextid = nextid;
    hdr.created = created; hdr.modified = (uint32_t)time(NULL);
    snprintf((char *)hdr.name, 32, "%s", name && name[0] ? name : "world");
    if (fwrite(&hdr, sizeof hdr, 1, f) != 1) return 0;

    /* v7: serialize all records to memory, then write one LZSS blob.
     * Worst record = 18 head + 3 col + 24 raw pos + 12 name = 57B. */
    size_t cap = (size_t)nb * 64 + 64;
    uint8_t *rec = malloc(cap);
    if (!rec) return 0;
    size_t rn = 0;
    int ok = 1;
    for (int i = 0; i < nb && ok; i++) {
        const Block *b = &blocks[i];
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
        memcpy(rec + rn, head, 18); rn += 18;
        memcpy(rec + rn, col3, 3);  rn += 3;
        if (raw) {
            memcpy(rec + rn, b->pos.e, 24); rn += 24;   /* pos+size contiguous */
        } else {
            int16_t q[6];
            for (int c = 0; c < 3; c++) {
                q[c]   = (int16_t)floorf(b->pos.e[c] * 16.0f + 0.5f);
                q[c+3] = (int16_t)floorf(b->size.e[c] * 16.0f + 0.5f);
            }
            memcpy(rec + rn, q, 12); rn += 12;
        }
        if (fl & BF4_NAME) { memcpy(rec + rn, b->name, 12); rn += 12; }
    }
    /* {rawBytes, compBytes} + blob; compBytes 0 = stored raw (enc failure) */
    uint8_t *comp = malloc(lzss_bound(rn));
    size_t cn = comp ? lzss_encode(rec, rn, comp) : 0;
    uint32_t wh[2] = { (uint32_t)rn, (uint32_t)cn };
    ok &= fwrite(wh, 8, 1, f) == 1;
    if (cn) ok &= fwrite(comp, cn, 1, f) == 1;
    else    ok &= fwrite(rec, rn, 1, f) == 1;
    free(rec); free(comp);
    WSet ws = *wset; ws.magic = WS_MAGIC;    /* settings trailer (v4+) */
    ok &= fwrite(&ws, sizeof ws, 1, f) == 1;
    uint32_t mh[2] = {MS_MAGIC, MSG_MAX};    /* message table trailer */
    ok &= fwrite(mh, 8, 1, f) == 1;
    ok &= fwrite(msgs, MSG_LEN, MSG_MAX, f) == MSG_MAX;
    /* BWST trailer: {magic,count} + {id,len,text[len]} per block string */
    int nbm = 0;
    if (bmsgs)
        for (int i = 0; i < nbmsg; i++) if (bmsgs[i].text[0]) nbm++;
    if (nbm) {
        uint32_t sh[2] = {ST_MAGIC, (uint32_t)nbm};
        ok &= fwrite(sh, 8, 1, f) == 1;
        for (int i = 0; i < nbmsg && ok; i++) {
            if (!bmsgs[i].text[0]) continue;
            size_t tl = strlen(bmsgs[i].text);
            uint8_t rh[6]; memcpy(rh, &bmsgs[i].id, 4);
            rh[4] = (uint8_t)tl; rh[5] = 0;
            ok &= fwrite(rh, 6, 1, f) == 1;
            ok &= fwrite(bmsgs[i].text, tl, 1, f) == 1;
        }
    }
    return ok;
}

int world_save(const char *path, const char *name) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (!s_created) s_created = (uint32_t)time(NULL);
    int ok = save_body(f, g_blocks, g_nb, g_nextid, s_created,
                       &s_wset, g_msgs, s_bmsgs, s_nbmsg, name);
    fclose(f);
    return ok;
}

/* ---- async save: caller-snapshot world state, worker serializes+writes.
 * The serialize+LZSS+disk IO is the slow part; the memcpy is instant. */
typedef struct {
    Block *blocks;
    int nb;
    uint32_t nextid, created;
    WSet wset;
    char msgs[MSG_MAX][MSG_LEN];
    BMsg bmsgs[BMSG_MAX];
    int nbmsg;
    char path[512], name[32];
    volatile int done;                    /* 0 running, 1 ok, -1 failed */
#ifdef _WIN32
    uintptr_t th;
#else
    pthread_t th;
#endif
} SaveJob;
static SaveJob *s_sjob;

static void save_run(SaveJob *j) {
    FILE *f = fopen(j->path, "wb");
    int ok = f && save_body(f, j->blocks, j->nb, j->nextid, j->created,
                            &j->wset, (const char(*)[MSG_LEN])j->msgs,
                            j->bmsgs, j->nbmsg, j->name);
    if (f) fclose(f);
    j->done = ok ? 1 : -1;
}
#ifdef _WIN32
static unsigned __stdcall save_tramp(void *p) { save_run(p); return 0; }
#else
static void *save_tramp(void *p) { save_run(p); return NULL; }
#endif

int world_save_async(const char *path, const char *name) {
    if (s_sjob) return -1;                          /* busy */
    SaveJob *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->blocks = malloc(sizeof(Block) * (size_t)(g_nb ? g_nb : 1));
    if (!j->blocks) { free(j); return -1; }
    memcpy(j->blocks, g_blocks, sizeof(Block) * (size_t)g_nb);
    j->nb = g_nb; j->nextid = g_nextid;
    if (!s_created) s_created = (uint32_t)time(NULL);
    j->created = s_created;
    j->wset = s_wset;
    memcpy(j->msgs, g_msgs, sizeof j->msgs);
    memcpy(j->bmsgs, s_bmsgs, sizeof j->bmsgs);
    j->nbmsg = s_nbmsg;
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->name, sizeof j->name, "%s", name ? name : "world");
    s_sjob = j;
#ifdef _WIN32
    j->th = _beginthreadex(NULL, 0, save_tramp, j, 0, NULL);
    if (!j->th) { s_sjob = NULL; free(j->blocks); free(j); return -1; }
    CloseHandle((HANDLE)j->th);         /* detach: join not needed */
#else
    if (pthread_create(&j->th, NULL, save_tramp, j)) {
        s_sjob = NULL; free(j->blocks); free(j); return -1;
    }
#endif
    return 1;
}

/* 1 = still running, 0 = finished ok (reaped), -1 = finished failed,
 * 2 = nothing in flight */
int world_save_poll(void) {
    if (!s_sjob) return 2;
    if (!s_sjob->done) return 1;
    int r = s_sjob->done;
#ifndef _WIN32
    pthread_join(s_sjob->th, NULL);
#endif
    free(s_sjob->blocks); free(s_sjob); s_sjob = NULL;
    return r < 0 ? -1 : 0;
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
    s_nbmsg = 0;
    if (ver >= 3 && fread(&g_nextid, 4, 1, f) != 1) g_nextid = 1;
    int ok = 1;
    if (ver >= 4) {                            /* v4+: meta header + packed */
        uint8_t rest[40];                      /* created, modified, name[32] */
        if (fread(rest, 1, 40, f) != 40) { fclose(f); g_nb = 0; return 0; }
        memcpy(&s_created, rest, 4);
        if (nameOut && rest[8]) { memcpy(nameOut, rest + 8, 32); nameOut[31] = 0; }
        int hsz = ver >= 5 ? 18 : 15;          /* v5 adds stat bytes */
        /* v7: records arrive as one LZSS blob {rawBytes, compBytes} */
        uint8_t *blob = NULL; const uint8_t *bp = NULL, *bend = NULL;
        if (ver >= 7) {
            uint32_t wh[2];
            if (fread(wh, 8, 1, f) != 1) { fclose(f); g_nb = 0; return 0; }
            blob = malloc(wh[0] ? wh[0] : 1);
            if (!blob) { fclose(f); g_nb = 0; return 0; }
            if (wh[1]) {
                uint8_t *comp = malloc(wh[1]);
                if (!comp || fread(comp, wh[1], 1, f) != 1 ||
                    lzss_decode(comp, wh[1], blob, wh[0]) != wh[0]) {
                    free(comp); free(blob); fclose(f); g_nb = 0; return 0;
                }
                free(comp);
            } else if (fread(blob, wh[0], 1, f) != 1 && wh[0]) {
                free(blob); fclose(f); g_nb = 0; return 0;
            }
            bp = blob; bend = blob + wh[0];
        }
        for (int i = 0; i < g_nb && ok; i++) {
            Block *b = &g_blocks[i];
            memset(b, 0, sizeof *b);
            uint8_t head[18], col3[3];
            /* record source: decompressed blob (v7) or the file (v4-v6) */
            if (bp) {
                ok = (size_t)(bend - bp) >= (size_t)hsz + 3;
                if (ok) { memcpy(head, bp, hsz); memcpy(col3, bp + hsz, 3); bp += hsz + 3; }
            } else {
                ok = fread(head, (size_t)hsz, 1, f) == 1 && fread(col3, 3, 1, f) == 1;
            }
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
            size_t pn = (head[0] & BF4_RAWPOS) ? 24 : 12;
            if (bp) {
                ok = (size_t)(bend - bp) >= pn;
                if (ok) {
                    if (pn == 24) memcpy(b->pos.e, bp, 24);
                    else {
                        int16_t q[6]; memcpy(q, bp, 12);
                        for (int c = 0; c < 3; c++) {
                            b->pos.e[c]  = q[c]   / 16.0f;
                            b->size.e[c] = q[c+3] / 16.0f;
                        }
                    }
                    bp += pn;
                }
            } else if (head[0] & BF4_RAWPOS) ok = fread(b->pos.e, 4, 6, f) == 6;
            else {
                int16_t q[6];
                ok = fread(q, 12, 1, f) == 1;
                for (int c = 0; c < 3; c++) {
                    b->pos.e[c]  = q[c]   / 16.0f;
                    b->size.e[c] = q[c+3] / 16.0f;
                }
            }
            if (ok && (head[0] & BF4_NAME)) {
                if (bp) { ok = (size_t)(bend - bp) >= 12; if (ok) { memcpy(b->name, bp, 12); bp += 12; } }
                else    ok = fread(b->name, 12, 1, f) == 1;
            }
            if (b->alpha <= 0.0f) b->alpha = 1.0f;
            if (b->id >= g_nextid) g_nextid = b->id + 1;
            if (!b->id) b->id = g_nextid++;
        }
        free(blob);
        /* settings trailer is optional: absent on pre-trailer v4 files */
        WSet ws;
        if (ok && fread(&ws, sizeof ws, 1, f) == 1 && ws.magic == WS_MAGIC)
            { s_wset = ws; world_day_reload(); }   /* re-seed day phase */
        /* message-table trailer (optional) */
        uint32_t mh[2];
        if (ok && fread(mh, 8, 1, f) == 1 && mh[0] == MS_MAGIC) {
            uint32_t n = mh[1] > MSG_MAX ? MSG_MAX : mh[1];
            if (fread(g_msgs, MSG_LEN, n, f) != n) ok = 0;
            /* per-block-string trailer (optional, follows BWMS) */
            uint32_t sh[2];
            if (ok && fread(sh, 8, 1, f) == 1 && sh[0] == ST_MAGIC) {
                for (uint32_t k = 0; k < sh[1] && ok; k++) {
                    uint8_t rh[6];
                    ok = fread(rh, 6, 1, f) == 1; if (!ok) break;
                    uint32_t id; memcpy(&id, rh, 4);
                    uint8_t tl = rh[4];
                    if (tl >= BMSG_LEN) { ok = fseek(f, tl, SEEK_CUR) == 0; continue; }
                    char *m = world_block_msg_edit(id);
                    if (!m) { ok = fseek(f, tl, SEEK_CUR) == 0; continue; }
                    ok = fread(m, tl, 1, f) == 1;
                    if (ok) m[tl] = 0;
                }
            }
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
    /* v<6 predates RK_MESH_BASE_MODEL 16->32 (built-ins grew to 17 ids);
       saved fused-model refs shift up to the new base */
    if (ver < 6 && ver >= 4)
        for (int i = 0; i < g_nb; i++)
            if (g_blocks[i].shape >= 16) g_blocks[i].shape += 16;
    g_un = g_re = 0;
    g_rev++;
    return 1;
}


/* ---- ray pick (shared by edit.c and physics.c play-drag) ---- */

int ray_unit_box(v3 ro, v3 rd, float *tOut, int *axisOut, float *signOut) {
    float tmin = -1e30f, tmax = 1e30f;
    int axis = -1; float sign = 0;
    for (int i = 0; i < 3; i++) {
        float inv = 1.0f / rd.e[i];
        float t0 = (-0.5f - ro.e[i]) * inv;
        float t1 = ( 0.5f - ro.e[i]) * inv;
        float s = -1.0f;
        if (t0 > t1) { float tt=t0; t0=t1; t1=tt; s = 1.0f; }
        if (t0 > tmin) { tmin = t0; axis = i; sign = s; }
        if (t1 < tmax) tmax = t1;
    }
    if (tmax < tmin || tmax < 0) return 0;
    if (tmin < 0) { tmin = tmax; axis = -1; }
    *tOut = tmin; *axisOut = axis; *signOut = sign;
    return 1;
}

int world_pick_block(v3 ro, v3 rd, int *idx, float *tOut,
                     int *axisOut, float *signOut) {
    int best = -1; float bt = 1e30f; int ba = -1; float bs = 0;
    for (int i = 0; i < g_nb; i++) {
        Block *b = &g_blocks[i];
        if (b->flags & BF_LOCKED) continue;
        v3 es = block_eff_size(b);
        v3 lro = v3_make((ro.x-b->pos.x)/es.x, (ro.y-b->pos.y)/es.y,
                         (ro.z-b->pos.z)/es.z);
        v3 lrd = v3_make(rd.x/es.x, rd.y/es.y, rd.z/es.z);
        float t; int ax; float sg;
        if (ray_unit_box(lro, lrd, &t, &ax, &sg) && t < bt) {
            bt = t; best = i; ba = ax; bs = sg;
        }
    }
    *idx = best; *tOut = bt; *axisOut = ba; *signOut = bs;
    return best >= 0;
}
