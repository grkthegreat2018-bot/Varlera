/* art.c - modal pixel-art tool. Edits g_atlas through tex_px(); DONE commits
 * to the GPU and appends to textures.bw on next save. Editor is paused while
 * open (main routes all input here when A.modal == 1).
 * Tools: pen, eraser, flood fill, eyedropper, line, rect; mirror-x toggle,
 * one-level undo (per-stroke snapshot), clear, wheel zoom + RMB pan. */
#include "art.h"
#include "app.h"
#include "tex.h"
#include "panel.h"
#include "font.h"
#include "math3d.h"
#include "rk.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <GLFW/glfw3.h>

#define ART_PPX 4                 /* atlas px per art pixel */
#define MAXCELLS (TEX_TILE / ART_PPX * 8)   /* 32 * 8 = 256 max grid edge */

static int   s_cells;             /* canvas grid edge (cells) */
static int   s_tw, s_th;          /* texture dims in tiles */
static float s_cx, s_cy, s_cs;    /* canvas rect: x,y + cell size px */
static float s_zoom = 1.0f, s_panx, s_pany;
static int   s_mirror;            /* mirror strokes across x midline */
static int   s_shapeDrag;         /* line/rect preview in progress */
static int   s_d0x, s_d0y;        /* drag anchor cell */
static int   s_panDrag;           /* RMB pan in progress */
static float s_pan0x, s_pan0y, s_panOX, s_panOY;
static uint8_t *s_snap;           /* per-stroke undo snapshot (w*h*4) */
static int   s_snapW, s_snapH, s_haveSnap;

static uint8_t *tex_pix(int *w, int *h) { return tex_px(A.artTex, w, h); }

/* read/write one art pixel (ART_PPX block of atlas px) */
static void aget(int cx, int cy, uint8_t out[4]) {
    int w, h; uint8_t *p = tex_pix(&w, &h);
    int ax = cx * ART_PPX, ay = cy * ART_PPX;
    memcpy(out, p + ((size_t)ay * RK_TEX_ATLAS + ax) * 4, 4);
}
static void aset(int cx, int cy, const uint8_t c[4]) {
    int w, h; uint8_t *p = tex_pix(&w, &h);
    for (int j = 0; j < ART_PPX; j++)
        for (int i = 0; i < ART_PPX; i++) {
            int ax = cx * ART_PPX + i, ay = cy * ART_PPX + j;
            if (ax < w && ay < h)
                memcpy(p + ((size_t)ay * RK_TEX_ATLAS + ax) * 4, c, 4);
        }
}

/* ---- stroke snapshot (undo) ---- */
static void snap_take(void) {
    int w, h; uint8_t *p = tex_pix(&w, &h);
    if (!s_snap || s_snapW != w || s_snapH != h) {
        free(s_snap);
        s_snap = malloc((size_t)w * h * 4);
        s_snapW = w; s_snapH = h;
    }
    if (!s_snap) return;
    for (int y = 0; y < h; y++)
        memcpy(s_snap + (size_t)y * w * 4,
               p + (size_t)y * RK_TEX_ATLAS * 4, (size_t)w * 4);
    s_haveSnap = 1;
}
static void snap_restore(void) {
    if (!s_haveSnap || !s_snap) return;
    int w, h; uint8_t *p = tex_pix(&w, &h);
    for (int y = 0; y < h && y < s_snapH; y++)
        memcpy(p + (size_t)y * RK_TEX_ATLAS * 4,
               s_snap + (size_t)y * w * 4, (size_t)(w < s_snapW ? w : s_snapW) * 4);
}

static int canvas_cell(float x, float y, int *cx, int *cy) {
    float px = (x - s_cx - s_panx) / (s_cs * s_zoom);
    float py = (y - s_cy - s_pany) / (s_cs * s_zoom);
    *cx = (int)floorf(px); *cy = (int)floorf(py);
    return *cx >= 0 && *cy >= 0 && *cx < s_cells && *cy < s_cells;
}

static void tool_color(uint8_t c[4]) {
    if (A.artTool == 1) { memset(c, 0, 4); }          /* eraser -> alpha 0 */
    else {
        c[0] = (uint8_t)(A.paintCol[0] * 255.0f);
        c[1] = (uint8_t)(A.paintCol[1] * 255.0f);
        c[2] = (uint8_t)(A.paintCol[2] * 255.0f);
        c[3] = 255;
    }
}

/* stamp honoring the mirror toggle */
static void stamp(int cx, int cy) {
    uint8_t c[4]; tool_color(c);
    aset(cx, cy, c);
    if (s_mirror) aset(s_cells - 1 - cx, cy, c);
}

static void bline(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        stamp(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static void frect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) stamp(x, y);
}

static void flood(int cx, int cy) {
    uint8_t target[4]; aget(cx, cy, target);
    uint8_t rep[4]; tool_color(rep);
    if (!memcmp(target, rep, 4)) return;
    static int stack[65536][2];
    int sp = 0; stack[sp][0] = cx; stack[sp][1] = cy; sp++;
    while (sp) {
        sp--; int x = stack[sp][0], y = stack[sp][1];
        uint8_t c[4]; aget(x, y, c);
        if (memcmp(c, target, 4)) continue;
        aset(x, y, rep);
        if (x > 0 && sp < 65532)          { stack[sp][0]=x-1; stack[sp][1]=y; sp++; }
        if (x < s_cells-1 && sp < 65532)  { stack[sp][0]=x+1; stack[sp][1]=y; sp++; }
        if (y > 0 && sp < 65532)          { stack[sp][0]=x; stack[sp][1]=y-1; sp++; }
        if (y < s_cells-1 && sp < 65532)  { stack[sp][0]=x; stack[sp][1]=y+1; sp++; }
    }
}

static void clear_all(void) {
    int w, h; uint8_t *p = tex_pix(&w, &h);
    for (int y = 0; y < h; y++)
        memset(p + (size_t)y * RK_TEX_ATLAS * 4, 0, (size_t)w * 4);
}

void art_open(int texId) {
    if (texId <= 0 || texId >= g_ntex) return;
    A.modal = 1; A.artTex = texId; A.artTool = 0; A.artLMB = 0;
    A.drag = M_IDLE; A.armedAsset = -1;
    const Tex *t = &g_tex[texId];
    s_tw = t->tw; s_th = t->th;
    s_cells = s_tw * (TEX_TILE / ART_PPX);
    s_zoom = 1.0f; s_panx = s_pany = 0;
    s_mirror = 0; s_shapeDrag = 0; s_panDrag = 0; s_haveSnap = 0;
}

void art_close(int save) {
    if (A.modal != 1) return;
    if (save) { tex_commit(A.artTex); tex_save("textures.bw"); }
    A.modal = 0; A.artLMB = 0; s_shapeDrag = 0; s_panDrag = 0;
}

/* ---- UI layout ---- */

static void layout(float *cw, float *ch) {
    float panelx = (float)A.fbw - 216.0f;
    float aw = panelx - 150.0f, ah = (float)A.fbh - 140.0f;
    *cw = *ch = fminf(aw, ah);
    s_cx = (panelx - 90.0f - *cw) * 0.5f;
    s_cy = ((float)A.fbh - *ch) * 0.5f + 14.0f;
    s_cs = *cw / s_cells;
}

/* right column: tool buttons */
enum { T_PEN, T_ERA, T_FILL, T_LINE, T_RECT, T_PICK, T_MIRR, T_UNDO, T_CLR, NT };
static const char *TN[NT] = {"PEN","ERASE","FILL","LINE","RECT","PICK","MIRROR","UNDO","CLEAR"};
static const char *TK[NT] = {"B","E","G","L","R","I","M","Z","C"};

static float tools_x(void) {
    float cw, ch; layout(&cw, &ch);
    return s_cx + cw + 14.0f;
}

/* top bar buttons: DEL / CANCEL / DONE */
static void topbtn_rect(int i, float *x, float *y) {
    *x = (float)A.fbw - 216.0f - (3 - i) * 76.0f;
    *y = s_cy - 44.0f;
}
static const char *TBN[3] = {"DEL", "CANCEL", "DONE"};

int art_press(float x, float y) {
    /* top bar */
    for (int i = 0; i < 3; i++) {
        float bx, by; topbtn_rect(i, &bx, &by);
        if (x >= bx && x < bx + 70 && y >= by && y < by + 26) {
            if (i == 0) { int id = A.artTex; art_close(0);
                          tex_delete(id); tex_commit(0); tex_save("textures.bw"); }
            else        art_close(i == 2);
            return 1;
        }
    }
    /* tool column */
    float tx = tools_x();
    for (int i = 0; i < NT; i++) {
        float ty = s_cy + i * 26.0f;
        if (x >= tx && x < tx + 76 && y >= ty && y < ty + 22) {
            switch (i) {
            case T_PEN:  A.artTool = 0; break;
            case T_ERA:  A.artTool = 1; break;
            case T_FILL: A.artTool = 2; break;
            case T_PICK: A.artTool = 3; break;
            case T_LINE: A.artTool = 4; break;
            case T_RECT: A.artTool = 5; break;
            case T_MIRR: s_mirror ^= 1; break;
            case T_UNDO: snap_restore(); break;
            case T_CLR:  snap_take(); clear_all(); break;
            }
            return 1;
        }
    }
    /* palette: 2 cols x 8 rows below tools */
    float py0 = s_cy + NT * 26.0f + 14.0f;
    for (int i = 0; i < 16; i++) {
        float sx = tx + (i % 2) * 32.0f, sy = py0 + (i / 2) * 30.0f;
        if (x >= sx && x < sx + 28 && y >= sy && y < sy + 26) {
            memcpy(A.paintCol, panel_color(i), sizeof(float)*3);
            if (A.artTool == 1) A.artTool = 0;
            return 1;
        }
    }
    /* clear (alpha) swatch */
    float cxy = py0 + 8 * 30.0f + 6.0f;
    if (x >= tx && x < tx + 28 && y >= cxy && y < cxy + 26) { A.artTool = 1; return 1; }

    int cx, cy;
    if (canvas_cell(x, y, &cx, &cy)) {
        if (A.artTool == 2)      { snap_take(); flood(cx, cy); }
        else if (A.artTool == 3) {
            uint8_t c[4]; aget(cx, cy, c);
            A.paintCol[0] = c[0] / 255.0f; A.paintCol[1] = c[1] / 255.0f;
            A.paintCol[2] = c[2] / 255.0f;
            A.artTool = 0;
        } else if (A.artTool >= 4) {
            snap_take(); s_shapeDrag = 1; s_d0x = cx; s_d0y = cy;
            stamp(cx, cy);
        } else { snap_take(); stamp(cx, cy); A.artLMB = 1; }
    }
    return 1;
}

void art_release(void) {
    if (s_shapeDrag) {
        /* finalize line/rect at release pos */
        int cx, cy;
        if (canvas_cell((float)A.mx, (float)A.my, &cx, &cy)) {
            snap_restore(); s_haveSnap = 1;
            if (A.artTool == 4) bline(s_d0x, s_d0y, cx, cy);
            else                frect(s_d0x, s_d0y, cx, cy);
        }
        s_shapeDrag = 0;
    }
    A.artLMB = 0;
}

void art_rmb(int down, float x, float y) {
    if (down) { s_panDrag = 1; s_pan0x = x; s_pan0y = y;
                s_panOX = s_panx; s_panOY = s_pany; }
    else s_panDrag = 0;
}

void art_motion(void) {
    if (s_panDrag) {
        s_panx = s_panOX + (float)A.mx - s_pan0x;
        s_pany = s_panOY + (float)A.my - s_pan0y;
        return;
    }
    if (s_shapeDrag) {
        /* live preview: restore snapshot, redraw shape */
        int cx, cy;
        if (!canvas_cell((float)A.mx, (float)A.my, &cx, &cy)) return;
        snap_restore(); s_haveSnap = 1;
        if (A.artTool == 4) bline(s_d0x, s_d0y, cx, cy);
        else                frect(s_d0x, s_d0y, cx, cy);
        return;
    }
    if (!A.artLMB || A.artTool > 1) return;
    int cx, cy;
    if (canvas_cell((float)A.mx, (float)A.my, &cx, &cy)) stamp(cx, cy);
}

void art_scroll(double dy) {
    /* zoom toward canvas center, clamp 0.5x..8x */
    s_zoom = clampf(s_zoom * (dy > 0 ? 1.15f : 0.87f), 0.5f, 8.0f);
}

int art_key(int key) {
    if (key == GLFW_KEY_ENTER)  { art_close(1); return 1; }
    if (key == GLFW_KEY_ESCAPE) { art_close(0); return 1; }
    if (key == GLFW_KEY_B) A.artTool = 0;
    if (key == GLFW_KEY_E) A.artTool = 1;
    if (key == GLFW_KEY_G) A.artTool = 2;
    if (key == GLFW_KEY_I) A.artTool = 3;
    if (key == GLFW_KEY_L) A.artTool = 4;
    if (key == GLFW_KEY_R) A.artTool = 5;
    if (key == GLFW_KEY_M) s_mirror ^= 1;
    if (key == GLFW_KEY_Z || key == GLFW_KEY_U) snap_restore();
    if (key == GLFW_KEY_C) { snap_take(); clear_all(); }
    return 1;
}

/* ---- draw ---- */

void art_draw(void) {
    float cw, ch; layout(&cw, &ch);

    /* dim the world behind */
    rk_ui_rect(0, 0, (float)A.fbw, (float)A.fbh, 0.02f, 0.02f, 0.03f, 0.55f);

    /* canvas: backing + cells */
    rk_ui_rect(s_cx - 2, s_cy - 2, cw + 4, ch + 4, 0.10f, 0.10f, 0.12f, 1);
    int w, h; uint8_t *px = tex_pix(&w, &h);
    float cs = s_cs * s_zoom;
    for (int cy = 0; cy < s_cells; cy++)
        for (int cx = 0; cx < s_cells; cx++) {
            float sx = s_cx + s_panx + cx * cs, sy = s_cy + s_pany + cy * cs;
            if (sx + cs < s_cx || sx > s_cx + cw || sy + cs < s_cy || sy > s_cy + ch)
                continue;
            int ch_ = ((cx >> 2) + (cy >> 2)) & 1;
            rk_ui_rect(sx, sy, cs, cs, ch_ ? 0.32f : 0.22f, ch_ ? 0.32f : 0.22f,
                       ch_ ? 0.34f : 0.24f, 1);
            int ax = cx * ART_PPX, ay = cy * ART_PPX;
            const uint8_t *p = px + ((size_t)ay * RK_TEX_ATLAS + ax) * 4;
            if (p[3] > 10)
                rk_ui_rect(sx, sy, cs, cs, p[0]/255.0f, p[1]/255.0f, p[2]/255.0f,
                           p[3] / 255.0f);
        }

    /* cell grid lines when zoomed enough to matter */
    if (cs >= 7.0f) {
        for (int g = 0; g <= s_cells; g++) {
            float gx = s_cx + s_panx + g * cs;
            float gy = s_cy + s_pany + g * cs;
            if (gx >= s_cx && gx <= s_cx + cw)
                rk_ui_rect(gx, fmaxf(s_cy, s_cy + s_pany), 0.5f,
                           fminf(ch, s_cells * cs), 0,0,0, 0.12f);
            if (gy >= s_cy && gy <= s_cy + ch)
                rk_ui_rect(fmaxf(s_cx, s_cx + s_panx), gy,
                           fminf(cw, s_cells * cs), 0.5f, 0,0,0, 0.12f);
        }
    }

    /* tile guide lines (proc sheet) */
    int cellsPerTile = TEX_TILE / ART_PPX;
    for (int t = 1; t < s_tw; t++) {
        float lx = s_cx + s_panx + t * cellsPerTile * cs;
        rk_ui_rect(lx - 1, s_cy + s_pany, 2, s_cells * cs, 0.4f, 0.7f, 1.0f, 0.8f);
        float ly = s_cy + s_pany + t * cellsPerTile * cs;
        rk_ui_rect(s_cx + s_panx, ly - 1, s_cells * cs, 2, 0.4f, 0.7f, 1.0f, 0.8f);
    }
    /* mirror midline */
    if (s_mirror) {
        float mx = s_cx + s_panx + s_cells * 0.5f * cs;
        rk_ui_rect(mx - 1, s_cy + s_pany, 2, s_cells * cs, 0.9f, 0.6f, 0.2f, 0.7f);
    }
    /* canvas frame */
    rk_ui_rect(s_cx - 1, s_cy - 1, cw + 2, 1, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx - 1, s_cy + ch, cw + 2, 1, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx - 1, s_cy - 1, 1, ch + 2, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx + cw, s_cy - 1, 1, ch + 2, 0.5f, 0.6f, 0.8f, 0.8f);

    /* title + top bar buttons */
    char tb[64];
    snprintf(tb, sizeof tb, "PIXEL ART - %s (%dx%d)", g_tex[A.artTex].name,
             s_tw, s_th);
    ui_text(s_cx, s_cy - 38, tb, 2, 0.9f, 0.92f, 1.0f, 1);
    ui_text(s_cx, s_cy - 16, "WHEEL ZOOM - RMB PAN", 1, 0.55f, 0.6f, 0.7f, 1);
    for (int i = 0; i < 3; i++) {
        float bx, by; topbtn_rect(i, &bx, &by);
        int hov = A.mx >= bx && A.mx < bx + 70 && A.my >= by && A.my < by + 26;
        if (i == 0)
            rk_ui_rect(bx, by, 70, 26, 0.45f + hov*0.10f, 0.18f, 0.16f, 1);
        else if (i == 2)
            rk_ui_rect(bx, by, 70, 26, 0.20f + hov*0.10f, 0.55f + hov*0.10f, 0.30f, 1);
        else
            rk_ui_rect(bx, by, 70, 26, 0.20f + hov*0.06f, 0.22f + hov*0.06f, 0.30f + hov*0.06f, 1);
        ui_text(bx + (70 - ui_text_w(TBN[i],1)) * 0.5f, by + 9, TBN[i], 1, 1,1,1, 1);
    }

    /* tool column */
    float tx = tools_x();
    ui_text(tx, s_cy - 16, "TOOLS", 1, 0.6f, 0.65f, 0.75f, 1);
    for (int i = 0; i < NT; i++) {
        float ty = s_cy + i * 26.0f;
        int act = (i == T_PEN && A.artTool == 0) || (i == T_ERA && A.artTool == 1) ||
                  (i == T_FILL && A.artTool == 2) || (i == T_PICK && A.artTool == 3) ||
                  (i == T_LINE && A.artTool == 4) || (i == T_RECT && A.artTool == 5) ||
                  (i == T_MIRR && s_mirror);
        int hov = A.mx >= tx && A.mx < tx + 76 && A.my >= ty && A.my < ty + 22;
        rk_ui_rect(tx, ty, 76, 22,
                   act ? 0.30f : 0.16f + hov*0.06f, act ? 0.45f : 0.18f + hov*0.06f,
                   act ? 0.65f : 0.24f + hov*0.06f, 1);
        char lb[16]; snprintf(lb, sizeof lb, "%s %s", TN[i], TK[i]);
        ui_text(tx + 6, ty + 7, lb, 1, act ? 1.0f : 0.85f, act ? 1.0f : 0.85f,
                act ? 1.0f : 0.9f, 1);
    }

    /* palette 2x8 below tools */
    float py0 = s_cy + NT * 26.0f + 14.0f;
    for (int i = 0; i < 16; i++) {
        const float *c = panel_color(i);
        float sx = tx + (i % 2) * 32.0f, sy = py0 + (i / 2) * 30.0f;
        rk_ui_rect(sx, sy, 28, 26, c[0], c[1], c[2], 1);
    }
    /* transparent swatch + current color */
    float cxy = py0 + 8 * 30.0f + 6.0f;
    rk_ui_rect(tx, cxy, 28, 26, 0.2f, 0.2f, 0.24f, 1);
    ui_text(tx + 9, cxy + 9, "X", 1, 0.9f, 0.4f, 0.4f, 1);
    rk_ui_rect(tx + 32, cxy, 28, 26, A.paintCol[0], A.paintCol[1], A.paintCol[2], 1);
    rk_ui_rect(tx + 32, cxy, 28, 1.5f, 1,1,1, 0.5f);

    rk_ui_flush();
}
