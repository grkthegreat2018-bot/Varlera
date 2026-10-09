/* sculpt.c - modal voxel sculpting (A.modal == 2). The editor world stays
 * visible but is paused; the camera keeps its normal controls. A SC_N^3 voxel
 * field sits on the ground at the origin; LMB applies the brush (ADD/SUB/
 * SMOOTH), DONE bakes the shape via marching tetrahedra into a CUSTOM model.
 * Sculpted models persist their voxel field in models.bw (ver 4) so they
 * survive restarts; unfuse explodes one back into micro blocks. */
#include "sculpt.h"
#include "edit.h"
#include "model.h"
#include "panel.h"
#include "camera.h"
#include "font.h"
#include "rk.h"
#include "world.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define SN   SC_N
#define SNV  (SN*SN*SN)

static uint16_t s_vox[SNV];       /* 0 = empty, else 0x8000|rgb565 */
static int s_tool   = 0;          /* 0 add, 1 sub, 2 smooth */
static int s_brush  = 1;          /* radius in cells (1 or 2) */
static int s_scale  = 1;          /* baked asset size in blocks (1/2/4) */
static int s_lmb    = 0;

/* ---------- voxel helpers ---------- */

static int vidx(int x, int y, int z) { return (z*SN + y)*SN + x; }
static int vsolid(int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0 || x >= SN || y >= SN || z >= SN) return 0;
    return s_vox[vidx(x,y,z)] & 0x8000;
}
static uint16_t rgb565(const float c[3]) {
    int r = (int)(c[0]*31.0f + 0.5f), g = (int)(c[1]*63.0f + 0.5f),
        b = (int)(c[2]*31.0f + 0.5f);
    if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
    return (uint16_t)(0x8000u | (r<<11) | (g<<5) | b);
}
static void rgb565f(uint16_t v, float c[3]) {
    c[0] = ((v>>11) & 31) / 31.0f;
    c[1] = ((v>>5)  & 63) / 63.0f;
    c[2] = (v & 31) / 31.0f;
}

/* grid world footprint: x,z in [-S/2, S/2], y in [0, S] */
static float cell_w(void) { return (float)s_scale / SN; }

/* ---------- ray -> voxel (Amanatides-Woo DDA) ----------
 * Returns 1 if the ray enters the grid. `hit` = first solid cell (or -1),
 * `prev` = empty cell just before hit (ADD target). `entry` = first cell
 * the ray occupies inside the bounds (used when nothing is solid yet). */
static int vox_ray(v3 ro, v3 rd,
                   int hit[3], int prev[3], int entry[3]) {
    float cs = cell_w();
    float h = s_scale * 0.5f;
    /* ray param inside bounds box */
    float tmin = -1e30f, tmax = 1e30f;
    const float lo[3] = {-h, 0.0f, -h}, hi[3] = {h, (float)s_scale, h};
    for (int a = 0; a < 3; a++) {
        float inv = rd.e[a] == 0.0f ? 1e30f : 1.0f / rd.e[a];
        float t0 = (lo[a] - ro.e[a]) * inv, t1 = (hi[a] - ro.e[a]) * inv;
        if (t0 > t1) { float t = t0; t0 = t1; t1 = t; }
        if (t0 > tmin) tmin = t0;
        if (t1 < tmax) tmax = t1;
    }
    if (tmax < tmin || tmax < 0) return 0;
    if (tmin < 0) tmin = 0;

    /* voxel-space origin */
    float ox = (ro.x + h) / cs, oy = ro.y / cs, oz = (ro.z + h) / cs;
    v3 d = v3_scale(rd, 1.0f / cs);               /* ray in voxel units */
    int c[3] = { (int)floorf(ox + d.x*tmin), (int)floorf(oy + d.y*tmin),
                 (int)floorf(oz + d.z*tmin) };
    for (int a = 0; a < 3; a++) {
        if (c[a] < 0) c[a] = 0; if (c[a] >= SN) c[a] = SN-1;
        entry[a] = c[a];
    }
    int step[3]; float tm[3], td[3];
    for (int a = 0; a < 3; a++) {
        step[a] = d.e[a] > 0 ? 1 : -1;
        float nxt = (d.e[a] > 0 ? c[a] + 1.0f : (float)c[a]);
        td[a] = fabsf(d.e[a]) > 1e-9f ? fabsf(1.0f / d.e[a]) : 1e30f;
        float dist = nxt - (a == 0 ? ox : a == 1 ? oy : oz);
        tm[a] = tmin + dist * td[a] * fabsf(d.e[a]);
        if (fabsf(d.e[a]) < 1e-9f) tm[a] = 1e30f;
    }
    hit[0] = hit[1] = hit[2] = -1;
    prev[0] = prev[1] = prev[2] = -1;
    for (int it = 0; it < 3*SN + 8; it++) {
        if (vsolid(c[0], c[1], c[2])) {
            hit[0]=c[0]; hit[1]=c[1]; hit[2]=c[2];
            return 1;
        }
        /* advance along smallest tMax */
        int a = tm[0] < tm[1] ? (tm[0] < tm[2] ? 0 : 2) : (tm[1] < tm[2] ? 1 : 2);
        prev[0]=c[0]; prev[1]=c[1]; prev[2]=c[2];
        c[a] += step[a]; tm[a] += td[a];
        if (c[a] < 0 || c[a] >= SN || tm[a] > tmax + 1.0f) break;
    }
    return 1;
}

/* ---------- tools ---------- */

static int brush_r(void) { return s_brush == 1 ? 1 : 2; }

static void brush_apply(int cx, int cy, int cz) {
    if (cx < 0) return;
    int r = brush_r();
    float rr = (float)r + 0.35f;
    if (s_tool == 2) {
        /* smooth: erode isolated voxels, fill cavities with neighbor color */
        static uint16_t cp[SNV];
        memcpy(cp, s_vox, sizeof cp);
        for (int z = cz-r; z <= cz+r; z++)
        for (int y = cy-r; y <= cy+r; y++)
        for (int x = cx-r; x <= cx+r; x++) {
            if (x<0||y<0||z<0||x>=SN||y>=SN||z>=SN) continue;
            float dx=(float)(x-cx), dy=(float)(y-cy), dz=(float)(z-cz);
            if (dx*dx+dy*dy+dz*dz > rr*rr) continue;
            int n6 = 0; uint16_t nc = 0;
            const int NB[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
            for (int q = 0; q < 6; q++) {
                int nx=x+NB[q][0], ny=y+NB[q][1], nz=z+NB[q][2];
                if (nx>=0&&ny>=0&&nz>=0&&nx<SN&&ny<SN&&nz<SN &&
                    (cp[vidx(nx,ny,nz)] & 0x8000)) { n6++; if (!nc) nc = cp[vidx(nx,ny,nz)]; }
            }
            int i = vidx(x,y,z);
            if ((cp[i] & 0x8000) && n6 < 3) s_vox[i] = 0;         /* erode bump */
            else if (!(cp[i] & 0x8000) && n6 >= 5) s_vox[i] = nc;/* fill pit  */
        }
        return;
    }
    uint16_t v = rgb565(A.paintCol);
    for (int z = cz-r; z <= cz+r; z++)
    for (int y = cy-r; y <= cy+r; y++)
    for (int x = cx-r; x <= cx+r; x++) {
        if (x<0||y<0||z<0||x>=SN||y>=SN||z>=SN) continue;
        float dx=(float)(x-cx), dy=(float)(y-cy), dz=(float)(z-cz);
        if (dx*dx+dy*dy+dz*dz > rr*rr) continue;
        s_vox[vidx(x,y,z)] = (s_tool == 0) ? v : 0;
    }
}

static void apply_at_cursor(void) {
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    int hit[3], prev[3], entry[3];
    if (!vox_ray(ro, rd, hit, prev, entry)) return;
    if (s_tool == 0) {
        int t[3];
        if (hit[0] >= 0 && prev[0] >= 0) memcpy(t, prev, sizeof t);
        else memcpy(t, entry, sizeof t);      /* empty grid: paint surface */
        brush_apply(t[0], t[1], t[2]);
    } else if (hit[0] >= 0) {
        brush_apply(hit[0], hit[1], hit[2]);
    }
}

/* ---------- marching tetrahedra bake ---------- */
/* Voxels are treated as node densities (solid=1). Each node-cell is split
 * into 6 tets around the 0-7 body diagonal; boundary triangles are emitted
 * with outward normals and the solid corner's color per vertex. */

static const int TETS[6][4] = {
    {0,1,3,7},{0,1,5,7},{0,2,3,7},{0,2,6,7},{0,4,5,7},{0,4,6,7}
};

static v3 mid(v3 a, v3 b) { return v3_scale(v3_add(a,b), 0.5f); }

/* emit one tri with normal facing away from `inside` (empty side) */
static void etri(Vert *o, int *n, int max, v3 a, v3 b, v3 c,
                 const float ca[3], const float cb[3], const float cc[3], v3 inside) {
    if (*n + 3 > max) return;
    v3 nrm = v3_cross(v3_sub(b,a), v3_sub(c,a));
    v3 cc3 = v3_scale(v3_add(v3_add(a,b),c), 1.0f/3.0f);
    if (v3_dot(nrm, v3_sub(inside, cc3)) < 0) { v3 t = b; b = c; c = t;
        const float *tc = cb; cb = cc; cc = tc; }
    nrm = v3_norm(v3_cross(v3_sub(b,a), v3_sub(c,a)));
    const float *cols[3] = {ca, cb, cc}; v3 ps[3] = {a,b,c};
    for (int i = 0; i < 3; i++) {
        memcpy(o[*n].pos, ps[i].e, 12);
        memcpy(o[*n].nrm, nrm.e, 12);
        memcpy(o[*n].col, cols[i], 12);
        (*n)++;
    }
}

/* march one tet; p[4]=corners, cl[4]=corner colors, m=4bit solid mask */
static void mtet(Vert *o, int *n, int max, v3 p[4], float cl[4][3], int m) {
    if (!m || m == 15) return;
    int sol[4], emp[4], ns = 0, ne = 0;
    for (int i = 0; i < 4; i++) (m & (1<<i)) ? (void)(sol[ns++]=i) : (void)(emp[ne++]=i);
    v3 ec = v3_make(0,0,0);
    for (int i = 0; i < ne; i++) ec = v3_add(ec, p[emp[i]]);
    ec = v3_scale(ec, 1.0f/ne);
    if (ns == 1 || ns == 3) {
        int x = ns == 1 ? sol[0] : emp[0];       /* the lone corner */
        int o1 = ns == 1 ? emp[0] : sol[0];
        int o2 = ns == 1 ? emp[1] : sol[1];
        int o3 = ns == 1 ? emp[2] : sol[2];
        etri(o,n,max, mid(p[x],p[o1]), mid(p[x],p[o2]), mid(p[x],p[o3]),
             cl[x], cl[x], cl[x], ec);
    } else {
        int a = sol[0], b = sol[1], c = emp[0], d = emp[1];
        v3 mac = mid(p[a],p[c]), mad = mid(p[a],p[d]);
        v3 mbc = mid(p[b],p[c]), mbd = mid(p[b],p[d]);
        etri(o,n,max, mac, mad, mbd, cl[a], cl[a], cl[b], ec);
        etri(o,n,max, mac, mbd, mbc, cl[a], cl[b], cl[b], ec);
    }
}

/* bake the voxel field to a mesh (centered at origin, spans s_size blocks).
 * returns mesh id via rk_model_mesh_add, or -1. */
int sculpt_bake_mesh(const uint16_t *vox, int size) {
    static Vert out[64 * 1024];
    int nv = 0;
    float csn = (float)size / (SN - 1), h = size * 0.5f;
    /* node grid: node(x,y,z) = voxel(x,y,z); cells between nodes */
    for (int z = 0; z < SN-1; z++)
    for (int y = 0; y < SN-1; y++)
    for (int x = 0; x < SN-1; x++) {
        int mask8 = 0; float cols[8][3];
        for (int c = 0; c < 8; c++) {
            int cx = x + (c&1), cy = y + ((c>>1)&1), cz = z + ((c>>2)&1);
            uint16_t v = vox[vidx(cx,cy,cz)];
            if (v & 0x8000) mask8 |= 1 << c;
            rgb565f(v, cols[c]);
        }
        if (!mask8 || mask8 == 0xFF) continue;
        v3 cp[8];
        for (int c = 0; c < 8; c++)
            cp[c] = v3_make((x+(c&1))*csn - h, (y+((c>>1)&1))*csn - h,
                            (z+((c>>2)&1))*csn - h);
        for (int t = 0; t < 6; t++) {
            v3 tp[4]; float tc[4][3]; int tm = 0;
            for (int i = 0; i < 4; i++) {
                tp[i] = cp[TETS[t][i]];
                memcpy(tc[i], cols[TETS[t][i]], 12);
                if (mask8 & (1 << TETS[t][i])) tm |= 1 << i;
            }
            mtet(out, &nv, (int)(sizeof out/sizeof(Vert)), tp, tc, tm);
        }
    }
    if (!nv) return -1;
    return rk_model_mesh_add(out, nv);
}

/* ---------- model glue ---------- */

static void sculpt_done(int save) {
    if (save) {
        if (model_sculpt(s_vox, s_scale) >= 0) {
            rk_models_commit();
            models_save(NULL);
        }
    }
    memset(s_vox, 0, sizeof s_vox);
    A.modal = 0; s_lmb = 0;
}

void sculpt_open(void) {
    memset(s_vox, 0, sizeof s_vox);
    s_tool = 0; s_brush = 1; s_scale = 1; s_lmb = 0;
    A.modal = 2; A.drag = M_IDLE; A.armedAsset = -1;
    sel_clear();
    /* park the camera looking at the grid */
    A.camPos = v3_make(2.4f, 1.8f, 2.4f);
    A.yaw = 0.785398f; A.pitch = -0.42f;
}

/* ---------- UI ---------- */

enum { SB_ADD, SB_SUB, SB_SMO, SB_BR1, SB_BR2, SB_S1, SB_S2, SB_S4,
       SB_CLEAR, SB_DONE, SB_CANCEL, SB_N };
static const char *SBN[SB_N] = {
    "ADD","SUB","SMO","BR1","BR2","1X","2X","4X","CLEAR","DONE","EXIT"
};

static float sbtn_x(int i) { return 16.0f + i * 58.0f; }
static const float SBTN_Y = 40.0f, SBTN_W = 52.0f, SBTN_H = 24.0f;

int sculpt_press(int mx, int my) {
    float x = (float)mx, y = (float)my;
    for (int i = 0; i < SB_N; i++) {
        float bx = sbtn_x(i);
        if (x >= bx && x < bx + SBTN_W && y >= SBTN_Y && y < SBTN_Y + SBTN_H) {
            switch (i) {
            case SB_ADD: s_tool = 0; break;
            case SB_SUB: s_tool = 1; break;
            case SB_SMO: s_tool = 2; break;
            case SB_BR1: s_brush = 1; break;
            case SB_BR2: s_brush = 2; break;
            case SB_S1:  s_scale = 1; break;
            case SB_S2:  s_scale = 2; break;
            case SB_S4:  s_scale = 4; break;
            case SB_CLEAR: memset(s_vox, 0, sizeof s_vox); break;
            case SB_DONE:  sculpt_done(1); break;
            case SB_CANCEL:sculpt_done(0); break;
            }
            return 1;
        }
    }
    /* palette column on the right (same 16 presets as the art tool) */
    float pxx = (float)A.fbw - 44.0f;
    for (int i = 0; i < 16; i++) {
        float py = SBTN_Y + i * 30.0f;
        if (x >= pxx && x < pxx + 26 && y >= py && y < py + 26) {
            memcpy(A.paintCol, panel_color(i), sizeof(float)*3);
            return 1;
        }
    }
    apply_at_cursor();
    s_lmb = 1;
    return 1;
}

void sculpt_release(void) { s_lmb = 0; }

void sculpt_move(int mx, int my) {
    (void)mx; (void)my;
    if (s_lmb) apply_at_cursor();
}

void sculpt_wheel(double dy) {
    cam_zoom_cursor((float)dy, A.mx, A.my);
}

int sculpt_key(int k) {
    if (k == GLFW_KEY_ESCAPE) { sculpt_done(0); return 1; }
    if (k == GLFW_KEY_ENTER)  { sculpt_done(1); return 1; }
    if (k == GLFW_KEY_1) s_tool = 0;
    if (k == GLFW_KEY_2) s_tool = 1;
    if (k == GLFW_KEY_3) s_tool = 2;
    if (k == GLFW_KEY_LEFT_BRACKET)  s_brush = 1;
    if (k == GLFW_KEY_RIGHT_BRACKET) s_brush = 2;
    return 1;
}

/* ---------- draw ---------- */

void sculpt_draw(void) {
    float cs = cell_w(), h = s_scale * 0.5f;

    /* voxels: surface cells only */
    static RkInst inst[SNV];
    int n = 0;
    for (int z = 0; z < SN; z++)
    for (int y = 0; y < SN; y++)
    for (int x = 0; x < SN; x++) {
        uint16_t v = s_vox[vidx(x,y,z)];
        if (!(v & 0x8000)) continue;
        if (vsolid(x+1,y,z) && vsolid(x-1,y,z) && vsolid(x,y+1,z) &&
            vsolid(x,y-1,z) && vsolid(x,y,z+1) && vsolid(x,y,z-1)) continue;
        float c[3]; rgb565f(v, c);
        inst[n].pos[0] = -h + (x + 0.5f) * cs;
        inst[n].pos[1] = (y + 0.5f) * cs;
        inst[n].pos[2] = -h + (z + 0.5f) * cs;
        inst[n].scale[0] = inst[n].scale[1] = inst[n].scale[2] = cs;
        inst[n].color = rk_rgba(c[0], c[1], c[2], 1.0f);
        inst[n].flags = RKIF_NOFOG;
        n++;
    }
    if (n) rk_world(RK_MESH_CUBE, inst, (uint32_t)n);

    /* bounds wire box */
    RkInst box;
    memset(&box, 0, sizeof box);
    box.pos[0] = 0; box.pos[1] = h; box.pos[2] = 0;
    box.scale[0] = box.scale[1] = box.scale[2] = (float)s_scale;
    box.color = rk_rgba(0.4f, 0.6f, 0.9f, 1.0f);
    rk_lines(&box, 1);

    /* hover ghost: target cell outline */
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    int hit[3], pv[3], en[3];
    if (vox_ray(ro, rd, hit, pv, en)) {
        int t[3];
        if (s_tool == 0) {
            if (hit[0] >= 0 && pv[0] >= 0) memcpy(t, pv, sizeof t);
            else memcpy(t, en, sizeof t);
        } else if (hit[0] >= 0) memcpy(t, hit, sizeof t);
        else t[0] = -1;
        if (t[0] >= 0) {
            RkInst g;
            memset(&g, 0, sizeof g);
            g.pos[0] = -h + (t[0] + 0.5f) * cs;
            g.pos[1] = (t[1] + 0.5f) * cs;
            g.pos[2] = -h + (t[2] + 0.5f) * cs;
            float r = brush_r() * 2.0f + 0.4f;
            g.scale[0] = g.scale[1] = g.scale[2] = cs * r;
            g.color = rk_rgba(1.0f, 1.0f, 1.0f, 1.0f);
            rk_lines(&g, 1);
        }
    }

    /* toolbar */
    rk_ui_rect(0, 0, (float)A.fbw, 72, 0.05f, 0.06f, 0.08f, 0.85f);
    ui_text(16, 16, "SCULPT MODE - SHAPE 16^3 VOXELS", 1, 0.9f, 0.92f, 1.0f, 1);
    for (int i = 0; i < SB_N; i++) {
        float bx = sbtn_x(i);
        int act = (i == SB_ADD && s_tool == 0) || (i == SB_SUB && s_tool == 1) ||
                  (i == SB_SMO && s_tool == 2) ||
                  (i == SB_BR1 && s_brush == 1) || (i == SB_BR2 && s_brush == 2) ||
                  (i == SB_S1 && s_scale == 1) || (i == SB_S2 && s_scale == 2) ||
                  (i == SB_S4 && s_scale == 4);
        int hov = A.mx >= bx && A.mx < bx + SBTN_W &&
                  A.my >= SBTN_Y && A.my < SBTN_Y + SBTN_H;
        rk_ui_rect(bx, SBTN_Y, SBTN_W, SBTN_H,
                   act ? 0.30f : 0.16f + hov*0.06f, act ? 0.45f : 0.18f + hov*0.06f,
                   act ? 0.65f : 0.24f + hov*0.06f, 1);
        ui_text(bx + (SBTN_W - ui_text_w(SBN[i], 1)) * 0.5f, SBTN_Y + 8,
                SBN[i], 1, 1, 1, 1, 1);
    }

    /* palette column */
    float pxx = (float)A.fbw - 44.0f;
    for (int i = 0; i < 16; i++) {
        const float *c = panel_color(i);
        rk_ui_rect(pxx, SBTN_Y + i * 30.0f, 26, 26, c[0], c[1], c[2], 1);
    }
    /* current color */
    rk_ui_rect(pxx, SBTN_Y + 16 * 30.0f + 6, 26, 26,
               A.paintCol[0], A.paintCol[1], A.paintCol[2], 1);
    ui_text(16, (float)A.fbh - 24,
            "1/2/3 TOOL  [/] BRUSH  LMB PAINT  ENTER DONE  ESC EXIT", 1,
            0.55f, 0.6f, 0.7f, 1);
    rk_ui_flush();
}
