/* tex.c - texture atlas: registry, built-in pattern baking, persistence.
 * Atlas: 1024x1024 RGBA, 128px tiles -> 8x8 tile grid. tex id 0 is "none".
 * Files: textures.bw (v1) - header + used records + per-record pixel payload.
 */
#include "tex.h"
#include "font.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

Tex     g_tex[TEX_MAX];
int     g_ntex;
uint8_t g_atlas[RK_TEX_ATLAS * RK_TEX_ATLAS * 4];

#define ATW RK_TEX_ATLAS
#define TILES (ATW / TEX_TILE)          /* 8 */

static uint8_t s_used[TEX_MAX];
static uint8_t s_tiles[TILES][TILES];   /* tile allocation map */

static void put(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= ATW || y >= ATW) return;
    uint8_t *p = &g_atlas[(y * ATW + x) * 4];
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

/* tile (FONT_TX,FONT_TY) holds the baked glyph strip — permanently marked
 * used so tex_new never allocates over it. font_bake rewrites the pixels;
 * call after any atlas wipe (tex_load) and before user tiles are handed out. */
static void font_reserve(void) {
    s_tiles[FONT_TY][FONT_TX] = 1;
    font_bake(g_atlas);
}

static uint32_t s_rng = 0x9E3779B9u;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
static float rndf(void) { return (rnd() & 0xFFFFFF) / 16777216.0f; }

/* fill a tile rect with a per-pixel generator */
static void fill(int tx, int ty, int tw, int th,
                 void (*gen)(float u, float v, float s, uint8_t px[4])) {
    for (int y = 0; y < th * TEX_TILE; y++)
        for (int x = 0; x < tw * TEX_TILE; x++) {
            uint8_t px[4];
            gen(x / (float)(tw * TEX_TILE), y / (float)(th * TEX_TILE),
                (x + y * tw * TEX_TILE) / (float)(tw * TEX_TILE * th * TEX_TILE), px);
            put(tx * TEX_TILE + x, ty * TEX_TILE + y, px[0], px[1], px[2], px[3]);
        }
}

/* ---- built-in pattern generators (mid-gray = neutral under overlay x2) ---- */

static void g_check(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    int c = (int)(u * 4.0f) + (int)(v * 4.0f);
    uint8_t g = (c & 1) ? 96 : 160;
    px[0] = px[1] = px[2] = g; px[3] = 255;
}
static void g_stripe(float u, float v, float s, uint8_t px[4]) {
    (void)v; (void)s;
    uint8_t g = ((int)(u * 8.0f) & 1) ? 88 : 168;
    px[0] = px[1] = px[2] = g; px[3] = 255;
}
static void g_dots(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    float du = u * 4.0f - floorf(u * 4.0f) - 0.5f;
    float dv = v * 4.0f - floorf(v * 4.0f) - 0.5f;
    uint8_t g = (du * du + dv * dv < 0.10f) ? 80 : 168;
    px[0] = px[1] = px[2] = g; px[3] = 255;
}
static void g_grid(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    float du = fabsf(u * 4.0f - floorf(u * 4.0f) - 0.5f);
    float dv = fabsf(v * 4.0f - floorf(v * 4.0f) - 0.5f);
    uint8_t g = (du > 0.42f || dv > 0.42f) ? 84 : 160;
    px[0] = px[1] = px[2] = g; px[3] = 255;
}
static void g_studs(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    /* lego-style studs: shaded discs, light top / dark bottom */
    float du = u * 3.0f - floorf(u * 3.0f) - 0.5f;
    float dv = v * 3.0f - floorf(v * 3.0f) - 0.5f;
    float r = sqrtf(du * du + dv * dv);
    uint8_t g = 160;
    if (r < 0.32f) g = 168;
    if (r < 0.30f) g = (uint8_t)(168 + 70.0f * (-dv - du) * 1.4f);
    if (r >= 0.32f && r < 0.36f) g = 96;        /* rim shadow */
    px[0] = px[1] = px[2] = g; px[3] = 255;
}
static void g_noise(float u, float v, float s, uint8_t px[4]) {
    (void)u; (void)v; (void)s;
    uint8_t g = (uint8_t)(96 + rndf() * 96.0f);
    px[0] = px[1] = px[2] = g; px[3] = 255;
}
static void g_brick(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    float row = floorf(v * 4.0f);
    float off = ((int)row & 1) ? 0.25f : 0.0f;
    float du = fabsf(u * 2.0f + off - floorf(u * 2.0f + off) - 0.5f);
    float dv = fabsf(v * 4.0f - floorf(v * 4.0f) - 0.5f);
    uint8_t g = (du > 0.44f || dv > 0.40f) ? 76 : 150 + (uint8_t)(rndf() * 20.0f);
    px[0] = g * 13 / 10 > 255 ? 255 : (uint8_t)(g * 1.3f); px[1] = g; px[2] = (uint8_t)(g * 0.8f);
    px[3] = 255;
}
static void g_hazard(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    int d = (int)((u + v) * 6.0f);
    if (d & 1) { px[0] = 220; px[1] = 170; px[2] = 30; }
    else       { px[0] = 40;  px[1] = 40;  px[2] = 45; }
    px[3] = 255;
}
static void g_wood(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    float ring = sinf(u * 40.0f + sinf(v * 12.0f) * 1.5f) * 0.5f + 0.5f;
    uint8_t r = (uint8_t)(120 + ring * 60.0f), g = (uint8_t)(80 + ring * 40.0f);
    px[0] = r; px[1] = g; px[2] = (uint8_t)(g * 0.7f); px[3] = 255;
}
/* 3x3 stone variation sheet */
static void g_stone(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    /* coarse cells + speckle; deterministic per sub-tile region */
    int cx = (int)(u * 12.0f), cy = (int)(v * 12.0f);
    uint32_t h = (uint32_t)(cx * 73856093 ^ cy * 19349663);
    h ^= h >> 13; h *= 2654435761u;
    uint8_t g = (uint8_t)(110 + (h & 63));
    px[0] = px[1] = px[2] = g; px[3] = 255;
}
/* 3x3 metal plate variation sheet */
static void g_metal(float u, float v, float s, uint8_t px[4]) {
    (void)s;
    int cx = (int)(u * 6.0f), cy = (int)(v * 6.0f);
    uint32_t h = (uint32_t)(cx * 83492791 ^ cy * 29765797);
    h ^= h >> 15; h *= 2246822519u;
    uint8_t g = (uint8_t)(130 + (h & 40));
    /* rivet dots at cell corners */
    float du = u * 6.0f - floorf(u * 6.0f) - 0.5f;
    float dv = v * 6.0f - floorf(v * 6.0f) - 0.5f;
    if (du * du + dv * dv > 0.40f) g = 80;
    px[0] = px[1] = px[2] = g; px[3] = 255;
}

/* ---- registry ---- */

void tex_sync_rect(int id) {
    const Tex *t = &g_tex[id];
    float k = 1.0f / TILES;
    rk_tex_rect(id, t->tx * k, t->ty * k, (float)t->tw, (float)t->th);
}

static int alloc_tiles(int tw, int th, int *outx, int *outy) {
    for (int y = 0; y + th <= TILES; y++)
        for (int x = 0; x + tw <= TILES; x++) {
            int ok = 1;
            for (int j = 0; j < th && ok; j++)
                for (int i = 0; i < tw && ok; i++)
                    if (s_tiles[y + j][x + i]) ok = 0;
            if (ok) { *outx = x; *outy = y; return 1; }
        }
    return 0;
}

int tex_new(const char *name, int tw, int th) {
    if (tw < 1 || th < 1 || tw > TILES || th > TILES) return -1;
    int slot = -1;
    for (int i = 1; i < TEX_MAX; i++) if (!s_used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    int x, y;
    if (!alloc_tiles(tw, th, &x, &y)) return -1;
    for (int j = 0; j < th; j++)
        for (int i = 0; i < tw; i++) s_tiles[y + j][x + i] = 1;
    Tex *t = &g_tex[slot];
    snprintf(t->name, sizeof t->name, "%s", name ? name : "TEX");
    t->tx = (uint8_t)x; t->ty = (uint8_t)y;
    t->tw = (uint8_t)tw; t->th = (uint8_t)th;
    s_used[slot] = 1;
    if (slot >= g_ntex) g_ntex = slot + 1;
    tex_sync_rect(slot);
    return slot;
}

void tex_delete(int id) {
    if (id <= 0 || id >= g_ntex || !s_used[id]) return;
    const Tex *t = &g_tex[id];
    for (int j = 0; j < t->th; j++)
        for (int i = 0; i < t->tw; i++) s_tiles[t->ty + j][t->tx + i] = 0;
    s_used[id] = 0;
    while (g_ntex > 1 && !s_used[g_ntex - 1]) g_ntex--;
}

uint8_t *tex_px(int id, int *w, int *h) {
    if (id <= 0 || id >= g_ntex || !s_used[id]) return NULL;
    const Tex *t = &g_tex[id];
    *w = t->tw * TEX_TILE; *h = t->th * TEX_TILE;
    return &g_atlas[(t->ty * TEX_TILE * ATW + t->tx * TEX_TILE) * 4];
}

void tex_commit(int id) {
    if (id > 0 && id < TEX_MAX) tex_sync_rect(id);
    rk_tex_upload(g_atlas);
}

/* ---- persistence ---- */
#define TEX_MAGIC 0x58545742u   /* 'BWTX' */
#define TEX_VER   1

/* file layout: hdr + used-bitmap (TEX_MAX bytes) + per-used-slot {Tex, pixels}.
 * Slots keep their ids across save/load so Block.tex stays valid. */
int tex_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t hdr[3] = {TEX_MAGIC, TEX_VER, (uint32_t)g_ntex};
    fwrite(hdr, 4, 3, f);
    fwrite(s_used, 1, TEX_MAX, f);
    for (int i = 1; i < g_ntex; i++) {
        if (!s_used[i]) continue;
        const Tex *t = &g_tex[i];
        fwrite(t, sizeof(Tex), 1, f);
        const uint8_t *px = &g_atlas[(t->ty * TEX_TILE * ATW + t->tx * TEX_TILE) * 4];
        for (int y = 0; y < t->th * TEX_TILE; y++)
            fwrite(px + (size_t)y * ATW * 4, 4, (size_t)t->tw * TEX_TILE, f);
    }
    fclose(f);
    return 1;
}

int tex_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t hdr[3];
    if (fread(hdr, 4, 3, f) != 3 || hdr[0] != TEX_MAGIC || hdr[1] > TEX_VER) {
        fclose(f); return 0;
    }
    memset(s_tiles, 0, sizeof s_tiles);
    memset(s_used, 0, sizeof s_used);
    memset(g_atlas, 0, sizeof g_atlas);
    if (fread(s_used, 1, TEX_MAX, f) != TEX_MAX) { fclose(f); return 0; }
    g_ntex = (int)hdr[2];
    for (int i = 1; i < g_ntex && i < TEX_MAX; i++) {
        if (!s_used[i]) continue;
        Tex *t = &g_tex[i];
        if (fread(t, sizeof(Tex), 1, f) != 1) { fclose(f); return 0; }
        if (t->tx + t->tw > TILES || t->ty + t->th > TILES) { s_used[i] = 0; continue; }
        /* records overlapping the reserved font tile get dropped (payload
           is still read to keep the stream aligned; font wins the pixels) */
        int fonthit = t->tx <= FONT_TX && t->tx + t->tw > FONT_TX &&
                      t->ty <= FONT_TY && t->ty + t->th > FONT_TY;
        for (int j = 0; j < t->th; j++)
            for (int k = 0; k < t->tw; k++) s_tiles[t->ty + j][t->tx + k] = 1;
        uint8_t *dst = &g_atlas[(t->ty * TEX_TILE * ATW + t->tx * TEX_TILE) * 4];
        for (int y = 0; y < t->th * TEX_TILE; y++)
            if (fread(dst + (size_t)y * ATW * 4, 4, (size_t)t->tw * TEX_TILE, f)
                != (size_t)t->tw * TEX_TILE) { fclose(f); return 0; }
        if (fonthit) {
            for (int j = 0; j < t->th; j++)
                for (int k = 0; k < t->tw; k++) s_tiles[t->ty + j][t->tx + k] = 0;
            s_used[i] = 0; continue;
        }
        tex_sync_rect(i);
    }
    font_reserve();
    fclose(f);
    return 1;
}

/* ---- init: bake built-ins ---- */

void tex_init(void) {
    memset(g_atlas, 0, sizeof g_atlas);
    memset(s_used, 0, sizeof s_used);
    memset(s_tiles, 0, sizeof s_tiles);
    font_reserve();                     /* mark+bake before tile handout */
    g_ntex = 1;
    snprintf(g_tex[0].name, 16, "NONE");

    struct { const char *n; int tw, th; void (*g)(float,float,float,uint8_t[4]); } b[] = {
        {"CHECK",  1, 1, g_check },
        {"STRIPE", 1, 1, g_stripe},
        {"DOTS",   1, 1, g_dots  },
        {"GRID",   1, 1, g_grid  },
        {"STUDS",  1, 1, g_studs },
        {"NOISE",  1, 1, g_noise },
        {"BRICK",  1, 1, g_brick },
        {"HAZARD", 1, 1, g_hazard},
        {"WOOD",   1, 1, g_wood  },
        {"STONE3", 3, 3, g_stone },   /* proc sheet: 3x3 variants */
        {"METAL3", 3, 3, g_metal },
    };
    for (int i = 0; i < (int)(sizeof b / sizeof b[0]); i++) {
        int id = tex_new(b[i].n, b[i].tw, b[i].th);
        if (id > 0) {
            const Tex *t = &g_tex[id];
            fill(t->tx, t->ty, t->tw, t->th, b[i].g);
        }
    }
    tex_load("textures.bw");            /* user textures override/add */
    tex_commit(0);
}
