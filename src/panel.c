/* panel.c - right-side tabbed panel: SHAPES library / PAINT (palette+picker) / WORLD. */
#include "panel.h"
#include "app.h"
#include "edit.h"
#include "model.h"
#include "tex.h"
#include "art.h"
#include "sculpt.h"
#include "font.h"
#include "math3d.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

#define PW      216.0f    /* panel width */
#define STRIPW  26.0f     /* collapsed strip width */
#define HDRH    24.0f     /* category header height */
#define CELL    64.0f     /* item cell size */
#define COLS    3
#define TOPH    56.0f     /* header + tab bar */
#define SRCHY   56.0f     /* search box y (SHAPES tab) */
#define SRCHH   22.0f
#define SHAPES_TOP (SRCHY + SRCHH + 6.0f)   /* 84: below the search box */

/* ---- preset palette ---- */
static const float PALETTE[][3] = {
    {0.90f,0.92f,0.95f},{0.55f,0.58f,0.62f},{0.24f,0.26f,0.29f},{0.95f,0.85f,0.30f},
    {0.94f,0.45f,0.20f},{0.85f,0.20f,0.25f},{0.75f,0.25f,0.60f},{0.45f,0.35f,0.85f},
    {0.25f,0.55f,0.95f},{0.20f,0.75f,0.80f},{0.25f,0.80f,0.40f},{0.55f,0.85f,0.25f},
    {0.55f,0.35f,0.20f},{0.95f,0.65f,0.45f},{0.95f,0.55f,0.70f},{1.00f,1.00f,1.00f},
};
#define N_COLORS ((int)(sizeof PALETTE / sizeof PALETTE[0]))

/* ---- assets: only shapes you can't make by resizing others ---- */
static const Asset ASSETS[] = {
    /* PRIMITIVES */
    {"Block",      "Basic cube.",                           RK_MESH_CUBE,     {1,1,1}},
    {"Sphere",     "Ball.",                                 RK_MESH_SPHERE,   {1,1,1}},
    {"Cylinder",   "Round column.",                         RK_MESH_CYLINDER, {1,1,1}},
    {"Cone",       "Pointy top.",                           RK_MESH_CONE,     {1,1,1}},
    {"Dome",       "Half sphere.",                          RK_MESH_DOME,     {1,0.55f,1}},
    {"Pyramid",    "Square pyramid.",                       RK_MESH_PYRAMID,  {1,1,1}},
    /* SLOPES */
    {"Wedge",      "45 deg ramp.",                          RK_MESH_WEDGE,    {1,1,1}},
    {"Corner",     "Corner wedge - slopes from one corner.",RK_MESH_CORNERW,  {1,1,1}},
    /* PARTS */
    {"Wheel",      "Rolls sideways.",                       RK_MESH_WHEEL,    {1,1,1}},
    {"Torus",      "Donut ring / tire.",                    RK_MESH_TORUS,    {1,1,1}},
};
#define N_ASSETS ((int)(sizeof ASSETS / sizeof ASSETS[0]))

typedef struct { const char *name; int first, count; } Cat;
static const Cat CATS[] = {
    {"PRIMITIVES", 0, 6},
    {"SLOPES",     6, 2},
    {"PARTS",      8, 2},
    {"CUSTOM",     0, 0},       /* dynamic: fused models + sculpted shapes */
};
#define N_CATS ((int)(sizeof CATS / sizeof CATS[0]))

static int          cat_count(int c) { return c < 3 ? CATS[c].count : g_nmodels; }
static int          cat_ai(int c, int i) { return c < 3 ? CATS[c].first + i : N_ASSETS + i; }

static const char *TABS[] = {"SHAPES", "PAINT", "WORLD"};

const Asset *panel_asset(int i) {
    if (i >= 0 && i < N_ASSETS) return &ASSETS[i];
    int m = i - N_ASSETS;
    if (m >= 0 && m < g_nmodels) {
        static Asset a;                      /* synthetic asset for a model */
        a.name = g_models[m].name;
        a.tip  = "Fused custom block.";
        a.mesh = g_models[m].mesh;
        a.size = g_models[m].size;
        return &a;
    }
    return NULL;
}
const float *panel_color(int i) { return PALETTE[i >= 0 && i < N_COLORS ? i : 0]; }

void panel_init(void) {
    A.panelOpen = 1; A.panelTab = 0; A.panelScroll = 0;
    for (int i = 0; i < N_CATS; i++) A.catOpen[i] = 1;
    A.curColor = 8;
    memcpy(A.paintCol, PALETTE[8], sizeof(float)*3);
    A.hsv[0] = 0.58f; A.hsv[1] = 0.7f; A.hsv[2] = 0.9f;
    A.nCustoms = 0;
    A.armedAsset = -1;
    A.paintTex = 0; A.paintTexM = TEXM_LOOP; A.paintAlpha = 1.0f;
}

/* ---- helpers ---- */

static float panel_x(void) { return (float)A.fbw - (A.panelOpen ? PW : STRIPW); }

static int in_rect(float x, float y, float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx+rw && y >= ry && y < ry+rh;
}

static void hsv2rgb(float h, float s, float v, float out[3]) {
    float i = floorf(h*6.0f), f = h*6.0f - i;
    float p = v*(1-s), q = v*(1-f*s), t = v*(1-(1-f)*s);
    switch ((int)i % 6) {
    case 0: out[0]=v; out[1]=t; out[2]=p; break;
    case 1: out[0]=q; out[1]=v; out[2]=p; break;
    case 2: out[0]=p; out[1]=v; out[2]=t; break;
    case 3: out[0]=p; out[1]=q; out[2]=v; break;
    case 4: out[0]=t; out[1]=p; out[2]=v; break;
    default:out[0]=v; out[1]=p; out[2]=q; break;
    }
}

/* ---- search filter (SHAPES tab) ---- */
static int name_hit(const char *name) {
    if (!A.search[0]) return 1;
    char nl[64]; int i = 0;
    for (; name[i] && i < 63; i++) nl[i] = (char)tolower((unsigned char)name[i]);
    nl[i] = 0;
    char q[33]; i = 0;
    for (; A.search[i] && i < 32; i++) q[i] = (char)tolower((unsigned char)A.search[i]);
    q[i] = 0;
    return strstr(nl, q) != NULL;
}

static int match_ids(int *ids) {              /* fill filtered asset ids */
    int n = 0;
    for (int i = 0; i < N_ASSETS; i++)
        if (name_hit(ASSETS[i].name)) ids[n++] = i;
    for (int m = 0; m < g_nmodels; m++)
        if (name_hit(g_models[m].name)) ids[n++] = N_ASSETS + m;
    return n;
}

/* SHAPES tab content height */
static float shapes_h(void) {
    if (A.search[0]) {
        int ids[N_ASSETS + RK_MAX_MODELS];
        int n = match_ids(ids);
        return HDRH + ((n + COLS - 1) / COLS) * CELL + 6.0f;
    }
    float h = 0;
    for (int c = 0; c < N_CATS; c++) {
        h += HDRH;
        if (A.catOpen[c]) h += ((cat_count(c) + COLS - 1) / COLS) * CELL + 6.0f;
    }
    return h;
}

static float cy(float contentY) { return SHAPES_TOP + contentY - A.panelScroll; }

/* asset at screen pos (SHAPES tab), -1 none */
static int asset_at(float x, float y) {
    float px = panel_x();
    float yy = 0;
    if (A.search[0]) {                        /* filtered flat list */
        int ids[N_ASSETS + RK_MAX_MODELS];
        int n = match_ids(ids);
        yy += HDRH;
        for (int i = 0; i < n; i++) {
            float cx0 = px + 8 + (i % COLS) * CELL;
            float cy0 = cy(yy + (i / COLS) * CELL);
            if (in_rect(x, y, cx0, cy0, CELL-4, CELL-4)) return ids[i];
        }
        return -1;
    }
    for (int c = 0; c < N_CATS; c++) {
        yy += HDRH;
        if (A.catOpen[c]) {
            for (int i = 0; i < cat_count(c); i++) {
                float cx0 = px + 8 + (i % COLS) * CELL;
                float cy0 = cy(yy + (i / COLS) * CELL);
                if (in_rect(x, y, cx0, cy0, CELL-4, CELL-4)) return cat_ai(c, i);
            }
            yy += ((cat_count(c) + COLS - 1) / COLS) * CELL + 6.0f;
        }
    }
    return -1;
}

/* PAINT tab layout constants (presets occupy y 78..214) */
#define PV_X(px)   ((px)+12)
#define PV_Y       232.0f
#define PV_W       176.0f
#define PV_H       120.0f
#define HUE_Y      (PV_Y + PV_H + 10)
#define HUE_H      16.0f
#define CUS_Y      (HUE_Y + HUE_H + 44)

static void set_paint(const float c[3], int idx) {
    memcpy(A.paintCol, c, sizeof(float)*3);
    A.curColor = idx;
    if (A.nsel) {
        world_push_undo();
        for (int i = 0; i < g_nb; i++) if (A.sel[i])
            memcpy(g_blocks[i].col, A.paintCol, sizeof(float)*3);
    }
}

static int preset_at(float x, float y) {
    float px = panel_x();
    for (int i = 0; i < N_COLORS; i++) {
        float sx = px + 10 + (i%4)*48.0f;
        float sy = 78 + (i/4)*34.0f;
        if (in_rect(x, y, sx, sy, 40, 28)) return i;
    }
    return -1;
}

static int custom_at(float x, float y) {
    float px = panel_x();
    for (int i = 0; i < A.nCustoms; i++) {
        float sx = px + 10 + (i%4)*48.0f;
        float sy = CUS_Y + (i/4)*34.0f;
        if (in_rect(x, y, sx, sy, 40, 28)) return i;
    }
    return -1;
}

/* ---- texture swatch grid (atlas textures + mode/type/alpha) ---- */
static const char *TEXMODE_NAME[] = {"STRETCH", "LOOP", "PROC"};
static float tex_row_y(void) { return CUS_Y + ((A.nCustoms + 3) / 4) * 34.0f + 26.0f; }
#define TEXCOLS 3
#define TEXROWS 4
static float texScroll;                 /* scroll offset in tex grid rows */

/* apply current paint tex settings to selection */
static void apply_tex_sel(void) {
    if (!A.nsel) return;
    world_push_undo();
    for (int j = 0; j < g_nb; j++) if (A.sel[j]) {
        g_blocks[j].tex = (uint8_t)A.paintTex;
        g_blocks[j].texm = (uint8_t)A.paintTexM;
        g_blocks[j].alpha = A.paintAlpha;
        if (A.paintAlpha < 1.0f) g_blocks[j].flags |= BF_GLASS;
        else g_blocks[j].flags &= ~BF_GLASS;
    }
}

/* ---- input ---- */

int panel_over(double x, double y) {
    if (x >= panel_x()) return 1;
    return y < 46 && x < 244;
}

int panel_press(double x, double y) {
    float px = panel_x();
    if (in_rect((float)x,(float)y, 10, 10, 110, 34)) { app_toggle_mode(); return 1; }
    if (A.app == APP_EDIT &&
        in_rect((float)x,(float)y, 128, 10, 110, 34)) { sculpt_open(); return 1; }
    if (x < px) { A.textMode = 0; return 0; }
    /* clicks anywhere but the search box defocus text input */
    if (!in_rect((float)x,(float)y, px + 8, SRCHY, PW - 16, SRCHH)) A.textMode = 0;
    if (!A.panelOpen) { A.panelOpen = 1; return 1; }
    if (in_rect((float)x,(float)y, px + PW - 26, 4, 24, 22)) { A.panelOpen = 0; return 1; }

    /* tabs */
    for (int t = 0; t < 3; t++)
        if (in_rect((float)x,(float)y, px + 6 + t*66, 30, 62, 22)) { A.panelTab = t; return 1; }

    float fx = (float)x, fy = (float)y;

    if (A.panelTab == 0) {           /* SHAPES */
        /* search box */
        if (in_rect(fx, fy, px + 8, SRCHY, PW - 16, SRCHH)) {
            A.textMode = 1; return 1;
        }
        A.textMode = 0;
        /* sculpt entry button (fixed at panel bottom) */
        if (in_rect(fx, fy, px + 8, (float)A.fbh - 34.0f, PW - 16, 26)) {
            sculpt_open(); return 1;
        }
        float yy = 0;
        for (int c = 0; c < N_CATS; c++) {
            if (in_rect(fx, fy, px, cy(yy), PW, HDRH)) {
                A.catOpen[c] ^= 1;
                float ch = shapes_h();
                if (A.panelScroll > ch - (A.fbh - SHAPES_TOP))
                    A.panelScroll = fmaxf(0, ch - (A.fbh - SHAPES_TOP));
                return 1;
            }
            yy += HDRH;
            if (A.catOpen[c]) yy += ((cat_count(c) + COLS-1)/COLS) * CELL + 6.0f;
        }
        int ai = asset_at(fx, fy);
        if (ai >= 0) { A.armedAsset = ai; A.drag = M_PLACE; }
        return 1;
    }

    if (A.panelTab == 1) {           /* PAINT */
        int pi = preset_at(fx, fy);
        if (pi >= 0) { set_paint(PALETTE[pi], pi); return 1; }
        float sx = PV_X(px);
        if (in_rect(fx, fy, sx, PV_Y, PV_W, PV_H)) {          /* SV square */
            A.hsv[1] = clampf((fx - sx) / PV_W, 0, 1);
            A.hsv[2] = clampf(1.0f - (fy - PV_Y) / PV_H, 0, 1);
            hsv2rgb(A.hsv[0], A.hsv[1], A.hsv[2], A.paintCol);
            A.curColor = -1; A.pickDrag = 1;
            return 1;
        }
        if (in_rect(fx, fy, sx, HUE_Y, PV_W, HUE_H)) {        /* hue bar */
            A.hsv[0] = clampf((fx - sx) / PV_W, 0, 0.999f);
            hsv2rgb(A.hsv[0], A.hsv[1], A.hsv[2], A.paintCol);
            A.curColor = -1; A.pickDrag = 2;
            return 1;
        }
        if (in_rect(fx, fy, sx + PV_W - 44, HUE_Y + HUE_H + 8, 44, 22)) { /* ADD */
            if (A.nCustoms < 8) {
                memcpy(A.customs[A.nCustoms++], A.paintCol, sizeof(float)*3);
            } else {
                memmove(A.customs, A.customs+1, sizeof(float)*3*7);
                memcpy(A.customs[7], A.paintCol, sizeof(float)*3);
            }
            return 1;
        }
        int cu = custom_at(fx, fy);
        if (cu >= 0) { set_paint(A.customs[cu], 100 + cu); return 1; }
        /* texture header buttons: MODE / TYPE */
        float ty = tex_row_y();
        if (in_rect(fx, fy, px + 62, ty - 18, 66, 16)) {   /* MODE cycle */
            A.paintTexM = (A.paintTexM & ~3) | ((A.paintTexM + 1) % 3);
            apply_tex_sel(); return 1;
        }
        if (in_rect(fx, fy, px + 132, ty - 18, 72, 16)) {  /* TYPE toggle */
            A.paintTexM ^= TEXM_OVR;
            apply_tex_sel(); return 1;
        }
        /* texture cells (scrolled list of atlas textures incl. id 0 NONE) */
        float tgy = ty + 6;
        int first = (int)texScroll * TEXCOLS;
        for (int r = 0; r < TEXROWS; r++) for (int c = 0; c < TEXCOLS; c++) {
            int i = first + r * TEXCOLS + c;
            if (i >= g_ntex) break;
            float tx0 = px + 10 + c * 66.0f, ty0 = tgy + r * 28.0f;
            if (in_rect(fx, fy, tx0, ty0, 62, 22)) {
                A.paintTex = i;
                apply_tex_sel();
                return 1;
            }
        }
        /* alpha row */
        float ay = tgy + TEXROWS * 28.0f + 4.0f;
        static const float AL[] = {1.0f, 0.75f, 0.5f, 0.25f};
        for (int i = 0; i < 4; i++) {
            float ax = px + 10 + i * 50.0f;
            if (in_rect(fx, fy, ax, ay, 46, 20)) {
                A.paintAlpha = AL[i];
                apply_tex_sel();
                return 1;
            }
        }
        /* art tool: EDIT selected / NEW tex / NEW SHEET */
        float ny = ay + 30.0f;
        if (in_rect(fx, fy, px + 10, ny, 62, 22)) {
            if (A.paintTex > 0 && A.paintTex < g_ntex) art_open(A.paintTex);
            return 1;
        }
        if (in_rect(fx, fy, px + 76, ny, 62, 22)) {
            int id = tex_new("TEX", 1, 1);
            if (id > 0) { tex_commit(id); tex_save("textures.bw");
                          A.paintTex = id; art_open(id); }
            return 1;
        }
        if (in_rect(fx, fy, px + 142, ny, 62, 22)) {
            int id = tex_new("SHEET", 3, 3);
            if (id > 0) { tex_commit(id); tex_save("textures.bw");
                          A.paintTex = id; art_open(id); }
            return 1;
        }
        return 1;
    }

    /* WORLD tab */
    float by = 80;
    struct { float x; const char *n; } btns[] = {
        {px+8, "SAVE"}, {px+76, "LOAD"}, {px+144, "UNDO"},
        {px+8, "REDO"}, {px+76, "CLEAR"},
    };
    for (int i = 0; i < 5; i++) {
        float bx = btns[i].x, byy = by + (i/3)*30;
        if (in_rect(fx, fy, bx, byy, 62, 24)) {
            switch (i) {
            case 0: world_save("world.bw"); tex_save("textures.bw"); break;
            case 1: if (world_load("world.bw")) sel_clear(); break;
            case 2: if (world_undo()) sel_clear(); break;
            case 3: if (world_redo()) sel_clear(); break;
            case 4: world_push_undo(); g_nb = 0; sel_clear(); break;
            }
            return 1;
        }
    }
    return 1;
}

/* called each frame while LMB held inside picker */
void panel_drag_update(void) {
    if (!A.pickDrag || A.panelTab != 1) return;
    float px = panel_x(), sx = PV_X(px);
    if (A.pickDrag == 1) {
        A.hsv[1] = clampf(((float)A.mx - sx) / PV_W, 0, 1);
        A.hsv[2] = clampf(1.0f - ((float)A.my - PV_Y) / PV_H, 0, 1);
    } else {
        A.hsv[0] = clampf(((float)A.mx - sx) / PV_W, 0, 0.999f);
    }
    hsv2rgb(A.hsv[0], A.hsv[1], A.hsv[2], A.paintCol);
}

void panel_scroll(double dy) {
    if (!A.panelOpen) return;
    if (A.panelTab == 0) {
        A.panelScroll = clampf(A.panelScroll - (float)dy * 42.0f,
                               0, fmaxf(0.0f, shapes_h() - (A.fbh - SHAPES_TOP)));
    } else if (A.panelTab == 1) {
        /* scroll the texture grid when hovering its area */
        float ty = tex_row_y() + 6;
        if (A.my >= ty && A.my <= ty + TEXROWS * 28.0f) {
            int rows = (g_ntex + TEXCOLS - 1) / TEXCOLS;
            texScroll = clampf(texScroll - (float)dy, 0.0f, fmaxf(0.0f, (float)(rows - TEXROWS)));
        }
    }
}

static void draw_script_menu(void);          /* defined below */

/* ---- draw ---- */

static void ui_rect_clipped(float x, float y, float w, float h,
                            float r, float g, float b, float a, float y0, float y1) {
    if (y + h <= y0 || y >= y1) return;
    float top = y < y0 ? y0 : y;
    float bot = y + h > y1 ? y1 : y + h;
    rk_ui_rect(x, top, w, bot - top, r, g, b, a);
}

static void draw_cell_row(float px, float yy, int i, int ai, float cy0, float cy1) {
    float ix = px + 8 + (i % COLS) * CELL;
    float iy = cy(yy + (i / COLS) * CELL);
    int hov = in_rect((float)A.mx,(float)A.my, ix, iy, CELL-4, CELL-4);
    if (hov) A.hoverAsset = ai;
    float bg = (A.armedAsset == ai) ? 0.30f : (hov ? 0.26f : 0.19f);
    ui_rect_clipped(ix, iy, CELL-4, CELL-4, bg, bg+0.02f, bg+0.07f, 1, cy0, cy1);
}

static void draw_icon_cell(float px, float yy, int i, int ai, float cy0, float cy1) {
    float ix = px + 8 + (i % COLS) * CELL;
    float iy = cy(yy + (i / COLS) * CELL);
    float t = iy < cy0 ? cy0 : iy;
    float b = iy + CELL-4 > cy1 ? cy1 : iy + CELL-4;
    const Asset *a = panel_asset(ai);
    if (b - t > 4 && a)
        rk_draw_icon(a->mesh, (int)ix, (int)t,
                     (int)(CELL-4), (int)(b-t),
                     A.paintCol[0], A.paintCol[1], A.paintCol[2]);
}

static void draw_shapes(float px) {
    float cy0 = SHAPES_TOP, cy1 = (float)A.fbh;
    float yy = 0;
    A.hoverAsset = -1;

    if (A.search[0]) {                        /* filtered flat list */
        int ids[N_ASSETS + RK_MAX_MODELS];
        int n = match_ids(ids);
        char hdr[24]; snprintf(hdr, sizeof hdr, "RESULTS %d", n);
        ui_text_clip(px + 12, cy(yy) + 5, hdr, 2, 0.85f,0.87f,0.95f,1, cy0, cy1);
        yy += HDRH;
        float cellsY = yy;
        for (int i = 0; i < n; i++) draw_cell_row(px, cellsY, i, ids[i], cy0, cy1);
        float ch = shapes_h(), vh = cy1 - cy0;
        if (ch > vh) {
            float sh = fmaxf(24.0f, vh * vh / ch);
            float sy = cy0 + (vh - sh) * (A.panelScroll / (ch - vh));
            rk_ui_rect(px + PW - 6, sy, 4, sh, 0.5f,0.55f,0.65f, 0.8f);
        }
        rk_ui_flush();
        for (int i = 0; i < n; i++) draw_icon_cell(px, cellsY, i, ids[i], cy0, cy1);
        return;
    }

    for (int c = 0; c < N_CATS; c++) {
        float hy = cy(yy);
        int hov = in_rect((float)A.mx,(float)A.my, px, hy, PW, HDRH);
        ui_rect_clipped(px+4, hy+1, PW-8, HDRH-2,
                        0.20f+hov*0.06f, 0.22f+hov*0.06f, 0.28f+hov*0.06f, 1, cy0, cy1);
        ui_text_clip(px + 12, hy + 5, A.catOpen[c] ? "v" : ">", 2,
                     0.7f,0.75f,0.85f,1, cy0, cy1);
        ui_text_clip(px + 28, hy + 5, CATS[c].name, 2,
                     0.85f,0.87f,0.95f,1, cy0, cy1);
        yy += HDRH;
        if (!A.catOpen[c]) continue;
        for (int i = 0; i < cat_count(c); i++) {
            float ix = px + 8 + (i % COLS) * CELL;
            float iy = cy(yy + (i / COLS) * CELL);
            int ai = cat_ai(c, i);
            int hov = in_rect((float)A.mx,(float)A.my, ix, iy, CELL-4, CELL-4);
            if (hov) A.hoverAsset = ai;
            float bg = (A.armedAsset == ai) ? 0.30f : (hov ? 0.26f : 0.19f);
            ui_rect_clipped(ix, iy, CELL-4, CELL-4, bg, bg+0.02f, bg+0.07f, 1, cy0, cy1);
        }
        yy += ((cat_count(c) + COLS-1)/COLS) * CELL + 6.0f;
    }
    /* scrollbar */
    float ch = shapes_h(), vh = cy1 - cy0;
    if (ch > vh) {
        float sh = fmaxf(24.0f, vh * vh / ch);
        float sy = cy0 + (vh - sh) * (A.panelScroll / (ch - vh));
        rk_ui_rect(px + PW - 6, sy, 4, sh, 0.5f,0.55f,0.65f, 0.8f);
    }
    rk_ui_flush();
    /* icons */
    yy = 0;
    for (int c = 0; c < N_CATS; c++) {
        yy += HDRH;
        if (A.catOpen[c]) {
            for (int i = 0; i < cat_count(c); i++) {
                float ix = px + 8 + (i % COLS) * CELL;
                float iy = cy(yy + (i / COLS) * CELL);
                float t = iy < cy0 ? cy0 : iy;
                float b = iy + CELL-4 > cy1 ? cy1 : iy + CELL-4;
                const Asset *a = panel_asset(cat_ai(c, i));
                if (b - t > 4 && a)
                    rk_draw_icon(a->mesh, (int)ix, (int)t,
                                 (int)(CELL-4), (int)(b-t),
                                 A.paintCol[0], A.paintCol[1], A.paintCol[2]);
            }
            yy += ((cat_count(c) + COLS-1)/COLS) * CELL + 6.0f;
        }
    }
}

static void draw_paint(float px) {
    ui_text(px+12, 62, "PRESETS", 1, 0.7f,0.72f,0.8f, 1);
    for (int i = 0; i < N_COLORS; i++) {
        float sx = px + 10 + (i%4)*48.0f;
        float sy = 78 + (i/4)*34.0f;
        if (i == A.curColor)
            rk_ui_rect(sx-2, sy-2, 44, 32, 1,1,1, 0.9f);
        const float *c = PALETTE[i];
        rk_ui_rect(sx, sy, 40, 28, c[0],c[1],c[2], 1);
    }

    /* SV square (hue = A.hsv[0]) */
    ui_text(px+12, PV_Y - 16, "PICKER", 1, 0.7f,0.72f,0.8f, 1);
    float sx = PV_X(px);
    for (int gy = 0; gy < 10; gy++)
        for (int gx = 0; gx < 12; gx++) {
            float c[3];
            hsv2rgb(A.hsv[0], (gx+0.5f)/12.0f, 1.0f - (gy+0.5f)/10.0f, c);
            rk_ui_rect(sx + gx*(PV_W/12), PV_Y + gy*(PV_H/10), PV_W/12, PV_H/10,
                       c[0],c[1],c[2], 1);
        }
    rk_ui_rect(sx, PV_Y, PV_W, 1.5f, 0,0,0, 0.5f);
    rk_ui_rect(sx, PV_Y+PV_H-1.5f, PV_W, 1.5f, 0,0,0, 0.5f);
    /* SV cursor */
    float cx0 = sx + A.hsv[1]*PV_W, cy0 = PV_Y + (1-A.hsv[2])*PV_H;
    rk_ui_rect(cx0-4, cy0-1, 8, 2, 1,1,1, 0.9f);
    rk_ui_rect(cx0-1, cy0-4, 2, 8, 1,1,1, 0.9f);

    /* hue bar */
    for (int i = 0; i < 16; i++) {
        float c[3]; hsv2rgb(i/16.0f, 1, 1, c);
        rk_ui_rect(sx + i*(PV_W/16), HUE_Y, PV_W/16, HUE_H, c[0],c[1],c[2], 1);
    }
    float hx = sx + A.hsv[0]*PV_W;
    rk_ui_rect(hx-1.5f, HUE_Y-2, 3, HUE_H+4, 1,1,1, 0.95f);

    /* preview + ADD */
    rk_ui_rect(sx, HUE_Y + HUE_H + 8, 60, 22, A.paintCol[0],A.paintCol[1],A.paintCol[2], 1);
    rk_ui_rect(sx, HUE_Y + HUE_H + 8, 60, 1, 1,1,1, 0.4f);
    rk_ui_rect(sx + PV_W - 44, HUE_Y + HUE_H + 8, 44, 22, 0.25f,0.5f,0.8f, 1);
    ui_text(sx + PV_W - 32, HUE_Y + HUE_H + 14, "ADD", 1, 1,1,1, 1);

    ui_text(px+12, CUS_Y - 14, "CUSTOM", 1, 0.7f,0.72f,0.8f, 1);
    for (int i = 0; i < A.nCustoms; i++) {
        float cx2 = px + 10 + (i%4)*48.0f;
        float cy2 = CUS_Y + (i/4)*34.0f;
        if (100 + i == A.curColor)
            rk_ui_rect(cx2-2, cy2-2, 44, 32, 1,1,1, 0.9f);
        rk_ui_rect(cx2, cy2, 40, 28, A.customs[i][0],A.customs[i][1],A.customs[i][2], 1);
    }

    /* texture section: label + MODE/TYPE buttons + scrollable tex grid + alpha */
    float ty = tex_row_y();
    ui_text(px+12, ty - 14, "TEX", 1, 0.7f,0.72f,0.8f, 1);
    rk_ui_rect(px + 62, ty - 18, 66, 16, 0.20f,0.24f,0.32f, 1);
    ui_text(px + 66, ty - 14, TEXMODE_NAME[A.paintTexM & 3], 1, 0.9f,0.9f,0.95f, 1);
    int ovr = (A.paintTexM & TEXM_OVR) != 0;
    rk_ui_rect(px + 132, ty - 18, 72, 16,
               ovr ? 0.45f : 0.20f, ovr ? 0.30f : 0.24f, ovr ? 0.22f : 0.32f, 1);
    ui_text(px + 136, ty - 14, ovr ? "OVR" : "OVL", 1, 0.95f,0.95f,1.0f, 1);

    float tgy = ty + 6;
    int first = (int)texScroll * TEXCOLS;
    for (int r = 0; r < TEXROWS; r++) for (int c = 0; c < TEXCOLS; c++) {
        int i = first + r * TEXCOLS + c;
        if (i >= g_ntex) break;
        float tx0 = px + 10 + c * 66.0f, ty0 = tgy + r * 28.0f;
        int sel = (A.paintTex == i);
        int hov = in_rect((float)A.mx,(float)A.my, tx0, ty0, 62, 22);
        rk_ui_rect(tx0, ty0, 62, 22,
                   sel?0.30f:0.19f+hov*0.07f, sel?0.36f:0.20f+hov*0.07f, sel?0.48f:0.26f+hov*0.07f, 1);
        ui_text(tx0 + (62 - ui_text_w(g_tex[i].name,1))/2, ty0 + 7, g_tex[i].name, 1,
                sel?1.0f:0.8f, sel?1.0f:0.8f, sel?1.0f:0.9f, 1);
    }
    /* alpha presets */
    float ay = tgy + TEXROWS * 28.0f + 4.0f;
    static const float AL[] = {1.0f, 0.75f, 0.5f, 0.25f};
    static const char *ALN[] = {"100%", "75%", "50%", "25%"};
    ui_text(px + 12, ay + 24, "ALPHA", 1, 0.7f,0.72f,0.8f, 1);
    for (int i = 0; i < 4; i++) {
        float ax = px + 10 + i * 50.0f;
        int sel = fabsf(A.paintAlpha - AL[i]) < 0.01f;
        rk_ui_rect(ax, ay, 46, 20,
                   sel ? 0.30f : 0.19f, sel ? 0.36f : 0.20f, sel ? 0.48f : 0.26f, AL[i]);
        ui_text(ax + (46 - ui_text_w(ALN[i],1))/2, ay + 6, ALN[i], 1, 1,1,1, 1);
    }
    /* art tool entry: EDIT / NEW / SHEET */
    float ny = ay + 30.0f;
    const char *nbn[] = {"EDIT", "NEW", "SHEET"};
    for (int i = 0; i < 3; i++) {
        float bx = px + 10 + i * 66.0f;
        int hov = in_rect((float)A.mx,(float)A.my, bx, ny, 62, 22);
        rk_ui_rect(bx, ny, 62, 22, 0.22f + hov*0.08f, 0.34f + hov*0.08f, 0.50f + hov*0.08f, 1);
        ui_text(bx + (62 - ui_text_w(nbn[i],1))/2, ny + 7, nbn[i], 1, 1,1,1, 1);
    }
    rk_ui_flush();
}

static void draw_world(float px) {
    const char *names[] = {"SAVE","LOAD","UNDO","REDO","CLEAR"};
    for (int i = 0; i < 5; i++) {
        float bx = px + 8 + (i%3)*68;
        float by = 80.0f + (i/3)*30.0f;
        rk_ui_rect(bx, by, 62, 24, 0.22f,0.30f,0.45f, 1);
        ui_text(bx + (62 - ui_text_w(names[i],1))/2, by + 8, names[i], 1, 1,1,1, 1);
    }
    char buf[48];
    snprintf(buf, sizeof buf, "BLOCKS %d / %d", g_nb, MAX_BLOCKS);
    ui_text(px+12, 150, buf, 1, 0.75f,0.78f,0.85f, 1);
    ui_text(px+12, 170, "UNDO STACK OK", 1, 0.55f,0.6f,0.68f, 1);
    rk_ui_flush();
}

void panel_draw(void) {
    float px = panel_x();
    float H = (float)A.fbh;

    /* play/edit button */
    int play = (A.app == APP_EDIT);
    rk_ui_rect(10, 10, 110, 34, play?0.25f:0.90f, play?0.75f:0.45f, play?0.35f:0.20f, 1);
    rk_ui_rect(10, 10, 110, 2, 1,1,1, 0.4f);
    ui_text(24, 19, play ? "> PLAY" : "|| EDIT", 2, 1,1,1,1);
    if (play) {   /* edit mode: SCULPT button next to PLAY */
        int hov = in_rect((float)A.mx, (float)A.my, 128, 10, 110, 34);
        rk_ui_rect(128, 10, 110, 34, 0.30f+hov*0.10f, 0.42f+hov*0.10f,
                   0.62f+hov*0.08f, 1);
        rk_ui_rect(128, 10, 110, 2, 1,1,1, 0.4f);
        ui_text(128.0f + (110.0f - (float)ui_text_w("SCULPT", 2)) * 0.5f, 19,
                "SCULPT", 2, 1,1,1, 1);
    }

    if (glfwGetTime() - A.speedMsgT < 1.6f) {
        char buf[32]; snprintf(buf, sizeof buf, "SPEED %d", (int)A.speed);
        ui_text(10, 52, buf, 1, 0.9f,0.9f,0.95f, 0.8f);
    }

    if (A.app == APP_PLAY) {
        ui_text((float)A.fbw/2 - 90, (float)A.fbh - 26,
                "PLAY MODE - TAB TO EDIT", 1, 1,1,1, 0.6f);
        rk_ui_flush();
        return;
    }

    rk_ui_rect(px, 0, A.panelOpen ? PW : STRIPW, H, 0.13f,0.14f,0.17f, 0.96f);
    if (!A.panelOpen) {
        ui_text(px + 6, 14, ">", 2, 0.9f,0.9f,0.95f, 1);
        rk_ui_flush();
        return;
    }
    rk_ui_rect(px, 0, 2, H, 0.3f,0.32f,0.38f, 1);
    ui_text(px + 10, 8, "LIBRARY", 2, 0.85f,0.87f,0.95f, 1);
    ui_text(px + PW - 22, 8, "<", 2, 0.85f,0.87f,0.95f, 1);

    /* tab buttons */
    for (int t = 0; t < 3; t++) {
        float tx = px + 6 + t*66;
        int selT = (A.panelTab == t);
        rk_ui_rect(tx, 30, 62, 22, selT?0.30f:0.17f, selT?0.34f:0.18f, selT?0.42f:0.22f, 1);
        ui_text(tx + (62 - ui_text_w(TABS[t],1))/2, 37, TABS[t], 1,
                selT?1.0f:0.65f, selT?1.0f:0.65f, selT?1.0f:0.7f, 1);
    }

    if (A.panelTab == 0) {
        /* search box */
        int foc = A.textMode == 1;
        rk_ui_rect(px + 8, SRCHY, PW - 16, SRCHH,
                   foc ? 0.24f : 0.15f, foc ? 0.26f : 0.16f, foc ? 0.34f : 0.21f, 1);
        rk_ui_rect(px + 8, SRCHY, PW - 16, 1.5f, 0.4f,0.6f,0.9f, foc ? 0.9f : 0.4f);
        char sb[48];
        if (A.search[0]) snprintf(sb, sizeof sb, "SEARCH: %s", A.search);
        else snprintf(sb, sizeof sb, "SEARCH");
        ui_text(px + 14, SRCHY + 7, sb, 1,
                foc ? 1.0f : 0.55f, foc ? 1.0f : 0.55f, foc ? 1.0f : 0.6f, 1);
        draw_shapes(px);
        /* sculpt entry button (fixed at panel bottom) */
        float sy = (float)A.fbh - 34.0f;
        rk_ui_rect(px, sy - 8, PW, 42, 0.13f, 0.14f, 0.17f, 1);
        int hov = in_rect((float)A.mx, (float)A.my, px + 8, sy, PW - 16, 26);
        rk_ui_rect(px + 8, sy, PW - 16, 26,
                   0.22f + hov*0.08f, 0.32f + hov*0.08f, 0.24f + hov*0.04f, 1);
        ui_text(px + 8 + (PW - 16 - ui_text_w("SCULPT", 1)) * 0.5f, sy + 9,
                "SCULPT", 1, 1, 1, 1, 1);
    }
    else if (A.panelTab == 1) draw_paint(px);
    else                      draw_world(px);
    rk_ui_flush();

    /* tooltip (SHAPES tab) */
    if (A.hoverAsset >= 0 && A.drag == M_IDLE) {
        const Asset *a = panel_asset(A.hoverAsset);
        if (a) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s - %s", a->name, a->tip);
        float tw = (float)ui_text_w(buf, 1) + 12;
        float tx = px - tw - 10, ty = (float)A.my - 14;
        if (tx < 0) tx = 0;
        rk_ui_rect(tx, ty, tw, 22, 0.08f,0.09f,0.11f, 0.95f);
        rk_ui_rect(tx, ty, tw, 1, 0.9f,0.9f,0.95f, 0.8f);
        ui_text(tx + 6, ty + 7, buf, 1, 0.95f,0.95f,1.0f, 1);
        rk_ui_flush();
        }
    }

    /* marquee rectangle */
    if (A.drag == M_MARQUEE) {
        float x0=fminf(A.marqX0,A.marqX1), y0=fminf(A.marqY0,A.marqY1);
        float x1=fmaxf(A.marqX0,A.marqX1), y1=fmaxf(A.marqY0,A.marqY1);
        rk_ui_rect(x0,y0,x1-x0,y1-y0, 0.4f,0.8f,1.0f,0.12f);
        rk_ui_rect(x0,y0,x1-x0,1, 0.4f,0.8f,1.0f,0.7f);
        rk_ui_rect(x0,y1-1,x1-x0,1, 0.4f,0.8f,1.0f,0.7f);
        rk_ui_rect(x0,y0,1,y1-y0, 0.4f,0.8f,1.0f,0.7f);
        rk_ui_rect(x1-1,y0,1,y1-y0, 0.4f,0.8f,1.0f,0.7f);
        rk_ui_flush();
    }

    /* block script menu (BW-style) */
    draw_script_menu();
}

/* ---- block script menu: floats above the selection ---- */
enum { SM_GRAV, SM_COLL, SM_LOCK, SM_TRIG, SM_ACT, SM_UNFUSE, SM_FUSE, SM_EDIT };
static const char *SM_LABEL[] = {"GRAVITY","COLLIDE","LOCKED","TRIG","ACT","UNFUSE","FUSE ALL","RESCULPT"};
static const char *TRIG_NAME[N_TRIGS] = {"ALWAYS","SPACE","W","A","S","D","E","Q","CLICK"};
static const char *ACT_NAME[N_ACTS] = {"NONE","SPIN","GLOW","BOOST"};
#define SM_W 152.0f

static int script_layout(float *bx, float *by, int acts[8]) {
    if (A.app != APP_EDIT || !A.nsel || A.drag != M_IDLE) return 0;
    int nr = 0;
    if (A.nsel == 1) {
        acts[nr++] = SM_GRAV; acts[nr++] = SM_COLL; acts[nr++] = SM_LOCK;
        acts[nr++] = SM_TRIG; acts[nr++] = SM_ACT;
        int si0 = sel_first();
        int sh = g_blocks[si0].shape;
        if (sh >= RK_MESH_BASE_MODEL) {
            int mid = sh - RK_MESH_BASE_MODEL;
            if (mid < g_nmodels && g_models[mid].vox) acts[nr++] = SM_EDIT;
            acts[nr++] = SM_UNFUSE;
        }
    } else {
        acts[nr++] = SM_FUSE; acts[nr++] = SM_LOCK;
    }
    float h = 26.0f + nr * 22.0f;
    float pw = A.panelOpen ? PW : STRIPW;
    *bx = (float)A.fbw - pw - SM_W - 10.0f;
    *by = clampf((float)A.fbh * 0.5f - h * 0.5f, 8.0f, (float)A.fbh - h - 8.0f);
    return nr;
}

static int script_state(int act, const Block *b) {
    switch (act) {
    case SM_GRAV: return !(b->flags & BF_NOPHYS);
    case SM_COLL: return !(b->flags & BF_NOCOLLIDE);
    case SM_LOCK: return  (b->flags & BF_LOCKED);
    }
    return 0;
}

static void draw_script_menu(void) {
    int acts[8]; float bx, by;
    int nr = script_layout(&bx, &by, acts);
    if (!nr) return;
    float h = 26.0f + nr * 22.0f;
    rk_ui_rect(bx, by, SM_W, h, 0.10f,0.11f,0.14f, 0.96f);
    rk_ui_rect(bx, by, SM_W, 1.5f, 0.40f,0.70f,1.0f, 0.9f);
    char buf[24];
    snprintf(buf, sizeof buf, A.nsel == 1 ? "BLOCK" : "%d BLOCKS", A.nsel);
    ui_text(bx + 8, by + 8, buf, 1, 0.85f,0.87f,0.95f, 1);
    int si = sel_first();
    const Block *b = A.nsel == 1 && si >= 0 ? &g_blocks[si] : NULL;
    for (int r = 0; r < nr; r++) {
        float ry = by + 26.0f + r * 22.0f;
        int hov = in_rect((float)A.mx,(float)A.my, bx+4, ry, SM_W-8, 19);
        int btn = acts[r] == SM_FUSE || acts[r] == SM_UNFUSE ||
                  acts[r] == SM_TRIG || acts[r] == SM_ACT || acts[r] == SM_EDIT;
        rk_ui_rect(bx+4, ry, SM_W-8, 19,
                   btn ? 0.25f : 0.16f+hov*0.08f,
                   btn ? 0.42f : 0.17f+hov*0.08f,
                   btn ? 0.65f : 0.22f+hov*0.08f, 1);
        char lab[32];
        const char *txt = SM_LABEL[acts[r]];
        if (b && acts[r] == SM_TRIG) { snprintf(lab, sizeof lab, "TRIG: %s", TRIG_NAME[b->trig]); txt = lab; }
        else if (b && acts[r] == SM_ACT) { snprintf(lab, sizeof lab, "ACT: %s", ACT_NAME[b->act]); txt = lab; }
        ui_text(bx + 12, ry + 6, txt, 1, 0.9f,0.9f,0.95f, 1);
        if (!btn && b) {
            int on = script_state(acts[r], b);
            ui_text(bx + SM_W - 46, ry + 6, on ? "ON" : "OFF", 1,
                    on?0.4f:0.5f, on?0.9f:0.5f, on?0.5f:0.55f, 1);
        }
    }
    rk_ui_flush();
}

/* RMB on a CUSTOM cell deletes that model (its world instances get unfused) */
int panel_rmb(double x, double y) {
    if (A.panelTab != 0 || !A.panelOpen || x < panel_x()) return 0;
    int ai = asset_at((float)x, (float)y);
    if (ai < N_ASSETS) return 0;
    model_delete(ai - N_ASSETS);
    if (A.armedAsset == ai) { A.armedAsset = -1; A.drag = M_IDLE; }
    if (A.hoverAsset >= ai) A.hoverAsset = -1;
    return 1;
}

int panel_script_click(double x, double y) {
    int acts[8]; float bx, by;
    int nr = script_layout(&bx, &by, acts);
    if (!nr) return 0;
    for (int r = 0; r < nr; r++) {
        float ry = by + 26.0f + r * 22.0f;
        if (!in_rect((float)x,(float)y, bx+4, ry, SM_W-8, 19)) continue;
        int i = sel_first();
        world_push_undo();
        switch (acts[r]) {
        case SM_GRAV: g_blocks[i].flags ^= BF_NOPHYS; break;
        case SM_COLL: g_blocks[i].flags ^= BF_NOCOLLIDE; break;
        case SM_TRIG: g_blocks[i].trig = (g_blocks[i].trig + 1) % N_TRIGS; break;
        case SM_ACT:
            g_blocks[i].act = (g_blocks[i].act + 1) % N_ACTS;
            g_blocks[i].flags &= ~BF_SPIN;   /* migrated to trig/act */
            break;
        case SM_LOCK:
            for (int j = 0; j < g_nb; j++) if (A.sel[j]) {
                g_blocks[j].flags ^= BF_LOCKED;
                if (g_blocks[j].flags & BF_LOCKED) { A.sel[j] = 0; A.nsel--; }
            }
            break;
        case SM_UNFUSE: unfuse_block(i); break;
        case SM_FUSE:   fuse_selection(); break;
        case SM_EDIT: {
            int mid = g_blocks[i].shape - RK_MESH_BASE_MODEL;
            if (mid >= 0 && mid < g_nmodels && g_models[mid].vox)
                sculpt_open_model(mid);
            break;
        }
        }
        return 1;
    }
    /* swallow clicks on the menu body */
    return in_rect((float)x,(float)y, bx, by, SM_W, 26.0f + nr*22.0f);
}
