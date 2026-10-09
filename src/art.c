/* art.c - modal pixel-art tool. Edits g_atlas through tex_px(); DONE commits
 * to the GPU and appends to textures.bw on next save. Editor is paused while
 * open (main routes all input here when A.modal == 1). */
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
#include <GLFW/glfw3.h>

#define ART_PPX 4                 /* atlas px per art pixel */
#define MAXCELLS (TEX_TILE / ART_PPX * 8)   /* 32 * 8 = 256 max grid edge */

static int   s_cells;             /* canvas grid edge (cells) */
static int   s_tw, s_th;          /* texture dims in tiles */
static float s_cx, s_cy, s_cs;    /* canvas rect: x,y + cell size px */
static float s_zoom = 1.0f, s_panx, s_pany;

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

static int canvas_cell(float x, float y, int *cx, int *cy) {
    float px = (x - s_cx - s_panx) / (s_cs * s_zoom);
    float py = (y - s_cy - s_pany) / (s_cs * s_zoom);
    *cx = (int)floorf(px); *cy = (int)floorf(py);
    return *cx >= 0 && *cy >= 0 && *cx < s_cells && *cy < s_cells;
}

static void paint_cell(int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= s_cells || cy >= s_cells) return;
    uint8_t c[4];
    if (A.artTool == 1) { memset(c, 0, 4); }          /* eraser -> alpha 0 */
    else {
        c[0] = (uint8_t)(A.paintCol[0] * 255.0f);
        c[1] = (uint8_t)(A.paintCol[1] * 255.0f);
        c[2] = (uint8_t)(A.paintCol[2] * 255.0f);
        c[3] = 255;
    }
    aset(cx, cy, c);
}

static void flood(int cx, int cy) {
    uint8_t target[4]; aget(cx, cy, target);
    uint8_t rep[4];
    if (A.artTool == 1) memset(rep, 0, 4);
    else { rep[0]=(uint8_t)(A.paintCol[0]*255); rep[1]=(uint8_t)(A.paintCol[1]*255);
           rep[2]=(uint8_t)(A.paintCol[2]*255); rep[3]=255; }
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

void art_open(int texId) {
    if (texId <= 0 || texId >= g_ntex) return;
    A.modal = 1; A.artTex = texId; A.artTool = 0; A.artLMB = 0;
    A.drag = M_IDLE; A.armedAsset = -1;
    const Tex *t = &g_tex[texId];
    s_tw = t->tw; s_th = t->th;
    s_cells = s_tw * (TEX_TILE / ART_PPX);
    s_zoom = 1.0f; s_panx = s_pany = 0;
}

void art_close(int save) {
    if (A.modal != 1) return;
    if (save) { tex_commit(A.artTex); tex_save("textures.bw"); }
    A.modal = 0; A.artLMB = 0;
}

/* ---- UI layout ---- */

static void layout(float *cw, float *ch) {
    float panelx = (float)A.fbw - 216.0f;
    float aw = panelx - 60.0f, ah = (float)A.fbh - 120.0f;
    *cw = *ch = fminf(aw, ah);
    s_cx = (panelx - *cw) * 0.5f;
    s_cy = ((float)A.fbh - *ch) * 0.5f;
    s_cs = *cw / s_cells;
}

enum { B_DONE, B_DEL, B_T_PEN, B_T_ERA, B_T_FILL, B_T_PICK, NB };
static const char *BN[NB] = {"DONE", "DEL", "PEN", "ERASE", "FILL", "PICK"};

static float btn_rect(int i, float *x, float *y) {
    float cw, ch; layout(&cw, &ch);
    *x = s_cx + i * 74.0f; *y = s_cy - 34.0f;
    return 0;
}

int art_press(float x, float y) {
    float bx, by;
    for (int i = 0; i < NB; i++) {
        btn_rect(i, &bx, &by);
        if (x >= bx && x < bx + 68 && y >= by && y < by + 26) {
            switch (i) {
            case B_DONE:  art_close(1); return 1;
            case B_DEL:   { int id = A.artTex; art_close(0);
                            tex_delete(id); tex_commit(0); tex_save("textures.bw");
                            return 1; }
            case B_T_PEN: A.artTool = 0; return 1;
            case B_T_ERA: A.artTool = 1; return 1;
            case B_T_FILL:A.artTool = 2; return 1;
            case B_T_PICK:A.artTool = 3; return 1;
            }
        }
    }
    /* palette: reuse panel's PALETTE via panel_color + customs */
    float cw, ch; layout(&cw, &ch);
    float pxx = s_cx + cw + 14.0f;
    for (int i = 0; i < 16; i++) {
        float py = s_cy + i * 30.0f;
        if (x >= pxx && x < pxx + 26 && y >= py && y < py + 26) {
            memcpy(A.paintCol, panel_color(i), sizeof(float)*3);
            A.artTool = 0; return 1;
        }
    }
    /* clear (alpha) swatch */
    float py = s_cy + 16 * 30.0f;
    if (x >= pxx && x < pxx + 26 && y >= py && y < py + 26) { A.artTool = 1; return 1; }

    int cx, cy;
    if (canvas_cell(x, y, &cx, &cy)) {
        if (A.artTool == 2) flood(cx, cy);
        else if (A.artTool == 3) {
            uint8_t c[4]; aget(cx, cy, c);
            A.paintCol[0] = c[0] / 255.0f; A.paintCol[1] = c[1] / 255.0f;
            A.paintCol[2] = c[2] / 255.0f;
            A.artTool = 0;
        } else { paint_cell(cx, cy); A.artLMB = 1; }
    }
    return 1;
}

void art_release(void) { A.artLMB = 0; }

void art_motion(void) {
    if (!A.artLMB || A.artTool > 1) return;
    int cx, cy;
    if (canvas_cell((float)A.mx, (float)A.my, &cx, &cy)) paint_cell(cx, cy);
}

void art_scroll(double dy) {
    /* zoom toward cursor, clamp 0.5x..8x */
    float old = s_zoom;
    s_zoom = clampf(s_zoom * (dy > 0 ? 1.15f : 0.87f), 0.5f, 8.0f);
    (void)old;
}

int art_key(int key) {
    if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_ENTER) { art_close(1); return 1; }
    if (key == GLFW_KEY_B) A.artTool = 0;
    if (key == GLFW_KEY_E) A.artTool = 1;
    if (key == GLFW_KEY_G) A.artTool = 2;
    if (key == GLFW_KEY_I) A.artTool = 3;
    return 1;
}

/* ---- draw ---- */

void art_draw(void) {
    float cw, ch; layout(&cw, &ch);

    /* dim the world behind */
    rk_ui_rect(0, 0, (float)A.fbw, (float)A.fbh, 0.02f, 0.02f, 0.03f, 0.55f);

    /* canvas: checkerboard backing (alpha) then cells */
    rk_ui_rect(s_cx - 2, s_cy - 2, cw + 4, ch + 4, 0.10f, 0.10f, 0.12f, 1);
    int w, h; uint8_t *px = tex_pix(&w, &h);
    float cs = s_cs * s_zoom;
    for (int cy = 0; cy < s_cells; cy++)
        for (int cx = 0; cx < s_cells; cx++) {
            float sx = s_cx + s_panx + cx * cs, sy = s_cy + s_pany + cy * cs;
            if (sx + cs < s_cx || sx > s_cx + cw || sy + cs < s_cy || sy > s_cy + ch)
                continue;
            /* checkerboard under transparent px */
            int ch_ = ((cx >> 2) + (cy >> 2)) & 1;
            rk_ui_rect(sx, sy, cs, cs, ch_ ? 0.32f : 0.22f, ch_ ? 0.32f : 0.22f,
                       ch_ ? 0.34f : 0.24f, 1);
            int ax = cx * ART_PPX, ay = cy * ART_PPX;
            const uint8_t *p = px + ((size_t)ay * RK_TEX_ATLAS + ax) * 4;
            if (p[3] > 10)
                rk_ui_rect(sx, sy, cs, cs, p[0]/255.0f, p[1]/255.0f, p[2]/255.0f,
                           p[3] / 255.0f);
        }

    /* tile guide lines (the proc-grab overlay) */
    int cellsPerTile = TEX_TILE / ART_PPX;
    for (int t = 1; t < s_tw; t++) {
        float lx = s_cx + s_panx + t * cellsPerTile * cs;
        rk_ui_rect(lx - 1, s_cy + s_pany, 2, s_cells * cs, 0.4f, 0.7f, 1.0f, 0.8f);
        float ly = s_cy + s_pany + t * cellsPerTile * cs;
        rk_ui_rect(s_cx + s_panx, ly - 1, s_cells * cs, 2, 0.4f, 0.7f, 1.0f, 0.8f);
    }
    /* canvas frame */
    rk_ui_rect(s_cx - 1, s_cy - 1, cw + 2, 1, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx - 1, s_cy + ch, cw + 2, 1, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx - 1, s_cy - 1, 1, ch + 2, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx + cw, s_cy - 1, 1, ch + 2, 0.5f, 0.6f, 0.8f, 0.8f);

    /* buttons */
    for (int i = 0; i < NB; i++) {
        float bx, by; btn_rect(i, &bx, &by);
        int tool = (i == B_T_PEN && A.artTool == 0) || (i == B_T_ERA && A.artTool == 1) ||
                   (i == B_T_FILL && A.artTool == 2) || (i == B_T_PICK && A.artTool == 3);
        int hov = A.mx >= bx && A.mx < bx + 68 && A.my >= by && A.my < by + 26;
        rk_ui_rect(bx, by, 68, 26,
                   tool ? 0.30f : 0.16f + hov * 0.06f, tool ? 0.45f : 0.18f + hov * 0.06f,
                   tool ? 0.65f : 0.24f + hov * 0.06f, 1);
        ui_text(bx + (68 - ui_text_w(BN[i], 1)) / 2, by + 9, BN[i], 1, 1, 1, 1, 1);
    }
    /* title */
    char tb[64];
    snprintf(tb, sizeof tb, "PIXEL ART - %s (%dx%d sheet)", g_tex[A.artTex].name,
             s_tw, s_th);
    ui_text(s_cx, s_cy - 56, tb, 1, 0.9f, 0.92f, 1.0f, 1);

    /* palette column */
    float pxx = s_cx + cw + 14.0f;
    for (int i = 0; i < 16; i++) {
        const float *c = panel_color(i);
        rk_ui_rect(pxx, s_cy + i * 30.0f, 26, 26, c[0], c[1], c[2], 1);
    }
    /* transparent swatch */
    rk_ui_rect(pxx, s_cy + 16 * 30.0f, 26, 26, 0.2f, 0.2f, 0.24f, 1);
    ui_text(pxx + 4, s_cy + 16 * 30.0f + 9, "X", 1, 0.9f, 0.4f, 0.4f, 1);

    /* current color preview */
    rk_ui_rect(pxx, s_cy + 17 * 30.0f + 6, 26, 26,
               A.paintCol[0], A.paintCol[1], A.paintCol[2], 1);
    ui_text(pxx - 2, s_cy + 18 * 30.0f + 14, "B E G I", 1, 0.6f, 0.65f, 0.75f, 1);

    rk_ui_flush();
}
