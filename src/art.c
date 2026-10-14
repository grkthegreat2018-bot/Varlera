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

/* flipbook frames: a multi-tile sheet edits ONE tile at a time; the frame
 * is a row-major tile index and shifts all art-cell addressing. */
static int   s_frame;             /* current frame tile (0..tw*th-1) */
static int   s_onion = 1;         /* ghost the previous frame under canvas */
static int   s_fplay;             /* live preview: advance at tex fps */

/* select tool: marquee rect in cells, lifted/floating pixels in s_clip */
static int   s_sel[4] = {-1,-1,-1,-1};   /* x0,y0,x1,y1 inclusive */
static int   s_selDrag;           /* marquee in progress */
static int   s_moveDrag;          /* LMB drag = move lifted pixels */
static int   s_mvAx, s_mvAy;      /* move anchor cell */
static int   s_mvX, s_mvY;        /* current move offset */
static int   s_float;             /* paste mode: ghost follows mouse */
static uint8_t *s_clip; static int s_clipW, s_clipH;

static int art_frames(void) { return s_tw * s_th; }
static void frame_off(int *ox, int *oy) {
    int cpt = TEX_TILE / ART_PPX;             /* art cells per tile */
    *ox = (s_frame % s_tw) * cpt;
    *oy = (s_frame / s_tw) * cpt;
}

static uint8_t *tex_pix(int *w, int *h) { return tex_px(A.artTex, w, h); }

/* read/write one art pixel (ART_PPX block of atlas px) */
static void aget(int cx, int cy, uint8_t out[4]) {
    int w, h; uint8_t *p = tex_pix(&w, &h);
    int ox, oy; frame_off(&ox, &oy);
    int ax = (cx + ox) * ART_PPX, ay = (cy + oy) * ART_PPX;
    memcpy(out, p + ((size_t)ay * RK_TEX_ATLAS + ax) * 4, 4);
}
static void aset(int cx, int cy, const uint8_t c[4]) {
    int w, h; uint8_t *p = tex_pix(&w, &h);
    int ox, oy; frame_off(&ox, &oy);
    for (int j = 0; j < ART_PPX; j++)
        for (int i = 0; i < ART_PPX; i++) {
            int ax = (cx + ox) * ART_PPX + i, ay = (cy + oy) * ART_PPX + j;
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

/* ---- select tool ops (cell-space, frame-aware via aget/aset) ---- */

static int sel_have(void) { return s_sel[2] >= s_sel[0] && s_sel[3] >= s_sel[1]; }
static int sel_w(void) { return s_sel[2] - s_sel[0] + 1; }
static int sel_h(void) { return s_sel[3] - s_sel[1] + 1; }
static int sel_contains(int cx, int cy) {
    return sel_have() && cx >= s_sel[0] && cx <= s_sel[2] &&
           cy >= s_sel[1] && cy <= s_sel[3];
}

static void clip_ensure(int w, int h) {
    if ((size_t)w * h * 4 > (size_t)s_clipW * s_clipH * 4) {
        free(s_clip); s_clip = NULL;
    }
    if (!s_clip) s_clip = malloc((size_t)w * h * 4);
    s_clipW = w; s_clipH = h;
}

static void sel_copy(void) {                    /* selection -> s_clip */
    if (!sel_have()) return;
    int w = sel_w(), h = sel_h();
    clip_ensure(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            aget(s_sel[0] + x, s_sel[1] + y, s_clip + ((size_t)y * w + x) * 4);
}

static void sel_erase(void) {                   /* clear selection cells */
    static const uint8_t Z[4] = {0,0,0,0};
    for (int y = s_sel[1]; y <= s_sel[3]; y++)
        for (int x = s_sel[0]; x <= s_sel[2]; x++) aset(x, y, Z);
}

static void clip_stamp(int bx, int by) {        /* s_clip -> canvas at base */
    for (int y = 0; y < s_clipH; y++)
        for (int x = 0; x < s_clipW; x++) {
            int cx = bx + x, cy = by + y;
            if (cx < 0 || cy < 0 || cx >= s_cells || cy >= s_cells) continue;
            aset(cx, cy, s_clip + ((size_t)y * s_clipW + x) * 4);
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
    s_cells = TEX_TILE / ART_PPX;           /* one frame tile per canvas */
    s_zoom = 1.0f; s_panx = s_pany = 0;
    s_mirror = 0; s_shapeDrag = 0; s_panDrag = 0; s_haveSnap = 0;
    s_frame = 0; s_fplay = 0;
    s_sel[0] = s_sel[1] = s_sel[2] = s_sel[3] = -1;
    s_selDrag = s_moveDrag = s_float = 0;
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
enum { T_PEN, T_ERA, T_FILL, T_LINE, T_RECT, T_PICK, T_SEL, T_MIRR, T_UNDO,
       T_CLR, T_ONION, T_PLAY, NT };
static const char *TN[NT] = {"PEN","ERASE","FILL","LINE","RECT","PICK","SEL","MIRROR","UNDO","CLEAR","ONION","PLAY"};
static const char *TK[NT] = {"B","E","G","L","R","I","S","M","Z","C","O","P"};

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
            case T_SEL:  A.artTool = 6; break;
            case T_MIRR: s_mirror ^= 1; break;
            case T_UNDO: snap_restore(); break;
            case T_CLR:  snap_take(); clear_all(); break;
            case T_ONION: s_onion ^= 1; break;
            case T_PLAY:  s_fplay ^= 1; break;
            }
            return 1;
        }
    }
    /* frame strip (sheet anims): click a tile to edit that frame */
    if (art_frames() > 1) {
        float cw2, ch2; layout(&cw2, &ch2);
        float fy = s_cy + ch2 + 12.0f;
        for (int f = 0; f < art_frames(); f++) {
            float fx = s_cx + f * 34.0f;
            if (fx + 30.0f > s_cx + cw2) break;
            if (x >= fx && x < fx + 30 && y >= fy && y < fy + 30) {
                s_frame = f; s_fplay = 0;
                return 1;
            }
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
    if (canvas_cell(x, y, &cx, &cy) && !s_fplay) {
        if (s_float) {                        /* paste mode: stamp at click */
            snap_take(); clip_stamp(cx, cy); s_float = 0;
            return 1;
        }
        if (A.artTool == 2)      { snap_take(); flood(cx, cy); }
        else if (A.artTool == 3) {
            uint8_t c[4]; aget(cx, cy, c);
            A.paintCol[0] = c[0] / 255.0f; A.paintCol[1] = c[1] / 255.0f;
            A.paintCol[2] = c[2] / 255.0f;
            A.artTool = 0;
        } else if (A.artTool == 6) {
            if (sel_contains(cx, cy)) {       /* lift + move */
                snap_take(); sel_copy(); sel_erase();
                s_moveDrag = 1; s_mvAx = cx; s_mvAy = cy;
                s_mvX = s_mvY = 0;
            } else {                          /* new marquee */
                s_selDrag = 1; s_d0x = cx; s_d0y = cy;
                s_sel[0] = s_sel[2] = cx; s_sel[1] = s_sel[3] = cy;
            }
        } else if (A.artTool >= 4) {
            snap_take(); s_shapeDrag = 1; s_d0x = cx; s_d0y = cy;
            stamp(cx, cy);
        } else { snap_take(); stamp(cx, cy); A.artLMB = 1; }
    }
    return 1;
}

void art_release(void) {
    if (s_selDrag) s_selDrag = 0;
    if (s_moveDrag) {
        /* commit lifted pixels at the final offset */
        clip_stamp(s_sel[0] + s_mvX, s_sel[1] + s_mvY);
        s_sel[0] += s_mvX; s_sel[2] += s_mvX;
        s_sel[1] += s_mvY; s_sel[3] += s_mvY;
        s_moveDrag = 0;
    }
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
    if (s_fplay) return;                    /* no edits while previewing */
    if (s_selDrag) {
        int cx, cy;
        if (canvas_cell((float)A.mx, (float)A.my, &cx, &cy)) {
            s_sel[0] = s_d0x < cx ? s_d0x : cx; s_sel[2] = s_d0x < cx ? cx : s_d0x;
            s_sel[1] = s_d0y < cy ? s_d0y : cy; s_sel[3] = s_d0y < cy ? cy : s_d0y;
        }
        return;
    }
    if (s_moveDrag) {
        int cx, cy;
        if (canvas_cell((float)A.mx, (float)A.my, &cx, &cy)) {
            s_mvX = cx - s_mvAx; s_mvY = cy - s_mvAy;
        }
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

int art_key(int key, int mods) {
    if (key == GLFW_KEY_ENTER)  { art_close(1); return 1; }
    if (key == GLFW_KEY_ESCAPE) {
        if (s_float) { s_float = 0; return 1; }
        if (sel_have()) { s_sel[0] = -1; s_sel[2] = -1; return 1; }
        art_close(0); return 1;
    }
    int ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    if (ctrl && key == GLFW_KEY_C && sel_have()) { sel_copy(); return 1; }
    if (ctrl && key == GLFW_KEY_X && sel_have()) {
        snap_take(); sel_copy(); sel_erase();
        s_sel[0] = -1; s_sel[2] = -1; return 1;
    }
    if (ctrl && key == GLFW_KEY_V && s_clip) { s_float = 1; return 1; }
    if (key == GLFW_KEY_DELETE && sel_have()) {
        snap_take(); sel_erase(); s_sel[0] = -1; s_sel[2] = -1; return 1;
    }
    if (key == GLFW_KEY_B) A.artTool = 0;
    if (key == GLFW_KEY_E) A.artTool = 1;
    if (key == GLFW_KEY_G) A.artTool = 2;
    if (key == GLFW_KEY_I) A.artTool = 3;
    if (key == GLFW_KEY_L) A.artTool = 4;
    if (key == GLFW_KEY_R) A.artTool = 5;
    if (key == GLFW_KEY_S) A.artTool = 6;
    if (key == GLFW_KEY_M) s_mirror ^= 1;
    if (key == GLFW_KEY_Z || key == GLFW_KEY_U) snap_restore();
    if (key == GLFW_KEY_C && !ctrl) { snap_take(); clear_all(); }
    if (key == GLFW_KEY_O) s_onion ^= 1;
    if (key == GLFW_KEY_P) s_fplay ^= 1;
    if (key == GLFW_KEY_COMMA)
        { s_frame = (s_frame + art_frames() - 1) % art_frames(); s_fplay = 0; }
    if (key == GLFW_KEY_PERIOD)
        { s_frame = (s_frame + 1) % art_frames(); s_fplay = 0; }
    return 1;
}

/* ---- draw ---- */

void art_draw(void) {
    float cw, ch; layout(&cw, &ch);

    /* dim the world behind */
    rk_ui_rect(0, 0, (float)A.fbw, (float)A.fbh, 0.02f, 0.02f, 0.03f, 0.55f);

    /* canvas: backing + cells (one frame tile; sheet anims pick s_frame) */
    rk_ui_rect(s_cx - 2, s_cy - 2, cw + 4, ch + 4, 0.10f, 0.10f, 0.12f, 1);
    int w, h; uint8_t *px = tex_pix(&w, &h);
    int nf = art_frames();
    /* live preview plays the frames at the texture's rate */
    int df = s_frame;
    if (s_fplay && nf > 1) {
        float fps = tex_fps(A.artTex); if (fps <= 0.0f) fps = 8.0f;
        df = (int)(glfwGetTime() * fps) % nf;
    }
    int cpt = TEX_TILE / ART_PPX;
    int ox = (df % s_tw) * cpt, oy = (df / s_tw) * cpt;
    int pox = (((df + nf - 1) % nf) % s_tw) * cpt,   /* onion: prev frame */
        poy = (((df + nf - 1) % nf) / s_tw) * cpt;
    float cs = s_cs * s_zoom;
    for (int cy = 0; cy < s_cells; cy++)
        for (int cx = 0; cx < s_cells; cx++) {
            float sx = s_cx + s_panx + cx * cs, sy = s_cy + s_pany + cy * cs;
            if (sx + cs < s_cx || sx > s_cx + cw || sy + cs < s_cy || sy > s_cy + ch)
                continue;
            int ch_ = ((cx >> 2) + (cy >> 2)) & 1;
            rk_ui_rect(sx, sy, cs, cs, ch_ ? 0.32f : 0.22f, ch_ ? 0.32f : 0.22f,
                       ch_ ? 0.34f : 0.24f, 1);
            if (s_onion && nf > 1) {           /* previous frame ghost */
                int qx = (cx + pox) * ART_PPX, qy = (cy + poy) * ART_PPX;
                const uint8_t *q = px + ((size_t)qy * RK_TEX_ATLAS + qx) * 4;
                if (q[3] > 10)
                    rk_ui_rect(sx, sy, cs, cs, q[0]/255.0f, q[1]/255.0f,
                               q[2]/255.0f, q[3] / 255.0f * 0.30f);
            }
            int ax = (cx + ox) * ART_PPX, ay = (cy + oy) * ART_PPX;
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

    /* mirror midline */
    if (s_mirror) {
        float mx = s_cx + s_panx + s_cells * 0.5f * cs;
        rk_ui_rect(mx - 1, s_cy + s_pany, 2, s_cells * cs, 0.9f, 0.6f, 0.2f, 0.7f);
    }

    /* select: marquee outline + lifted/paste ghost */
    if (sel_have() || s_moveDrag) {
        int sox = s_moveDrag ? s_mvX : 0, soy = s_moveDrag ? s_mvY : 0;
        float rx = s_cx + s_panx + (s_sel[0] + sox) * cs;
        float ry = s_cy + s_pany + (s_sel[1] + soy) * cs;
        float rw = (s_sel[2] - s_sel[0] + 1) * cs;
        float rh = (s_sel[3] - s_sel[1] + 1) * cs;
        rk_ui_rect(rx - 1, ry - 1, rw + 2, 1, 1,1,1, 0.9f);
        rk_ui_rect(rx - 1, ry + rh, rw + 2, 1, 1,1,1, 0.9f);
        rk_ui_rect(rx - 1, ry - 1, 1, rh + 2, 1,1,1, 0.9f);
        rk_ui_rect(rx + rw, ry - 1, 1, rh + 2, 1,1,1, 0.9f);
        if (s_moveDrag && s_clip) {          /* lifted pixels preview */
            for (int y = 0; y < s_clipH; y++)
                for (int x = 0; x < s_clipW; x++) {
                    const uint8_t *p = s_clip + ((size_t)y * s_clipW + x) * 4;
                    if (p[3] < 10) continue;
                    int cx = s_sel[0] + s_mvX + x, cy = s_sel[1] + s_mvY + y;
                    if (cx < 0 || cy < 0 || cx >= s_cells || cy >= s_cells)
                        continue;
                    rk_ui_rect(s_cx + s_panx + cx * cs, s_cy + s_pany + cy * cs,
                               cs, cs, p[0]/255.0f, p[1]/255.0f, p[2]/255.0f,
                               p[3] / 255.0f);
                }
        }
    }
    if (s_float && s_clip) {                 /* ctrl+V ghost follows mouse */
        int bx, by;
        if (canvas_cell((float)A.mx, (float)A.my, &bx, &by)) {
            for (int y = 0; y < s_clipH; y++)
                for (int x = 0; x < s_clipW; x++) {
                    const uint8_t *p = s_clip + ((size_t)y * s_clipW + x) * 4;
                    if (p[3] < 10) continue;
                    int cx = bx + x, cy = by + y;
                    if (cx < 0 || cy < 0 || cx >= s_cells || cy >= s_cells)
                        continue;
                    rk_ui_rect(s_cx + s_panx + cx * cs, s_cy + s_pany + cy * cs,
                               cs, cs, p[0]/255.0f, p[1]/255.0f, p[2]/255.0f,
                               p[3] / 255.0f * 0.8f);
                }
        }
    }
    /* canvas frame */
    rk_ui_rect(s_cx - 1, s_cy - 1, cw + 2, 1, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx - 1, s_cy + ch, cw + 2, 1, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx - 1, s_cy - 1, 1, ch + 2, 0.5f, 0.6f, 0.8f, 0.8f);
    rk_ui_rect(s_cx + cw, s_cy - 1, 1, ch + 2, 0.5f, 0.6f, 0.8f, 0.8f);

    /* frame strip (sheet anims): one atlas-thumbnail per tile */
    if (nf > 1) {
        const Tex *t = &g_tex[A.artTex];
        float fy = s_cy + ch + 12.0f;
        for (int f = 0; f < nf; f++) {
            float fx = s_cx + f * 34.0f;
            if (fx + 30.0f > s_cx + cw) break;
            int sel = (f == df);
            rk_ui_rect(fx, fy, 30, 30, sel ? 0.30f : 0.16f,
                       sel ? 0.45f : 0.18f, sel ? 0.65f : 0.24f, 1);
            float u0 = (t->tx + f % s_tw) / 8.0f, u1 = u0 + 0.125f;
            float v0 = (t->ty + f / s_tw) / 8.0f, v1 = v0 + 0.125f;
            rk_ui_texrect(fx + 2, fy + 2, 26, 26, u0, v0, u1, v1);
        }
    }

    /* title + top bar buttons */
    char tb[80];
    if (nf > 1)
        snprintf(tb, sizeof tb, "PIXEL ART - %s (%dx%d)  FRAME %d/%d",
                 g_tex[A.artTex].name, s_tw, s_th, df + 1, nf);
    else
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
                  (i == T_SEL && A.artTool == 6) ||
                  (i == T_MIRR && s_mirror) ||
                  (i == T_ONION && s_onion && art_frames() > 1) ||
                  (i == T_PLAY && s_fplay);
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
