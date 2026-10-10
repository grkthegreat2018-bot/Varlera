/* sculpt.c - modal vertex-drag mesh editor (A.modal == 2). Ported from a
 * reference JS prototype: a welded triangle mesh starts as a cube inside a
 * "size marker" box; LMB grabs a vertex dot and drags it on a
 * camera-parallel plane (grid-snapped), RMB-click splits the nearest edge,
 * Del removes the selected vertex, and on release overlapping verts weld +
 * crossing edges resolve by splitting both. The marker box is a reference -
 * the mesh may exceed it, in which case a second outline shows the true
 * bounds. DONE bakes the topology into a CUSTOM model (normalized unit mesh
 * + per-vertex colors); RESCULPT reloads it for further editing. */
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
#define SNV  SC_VOX_N
#define ISO  0.5f

static float    s_vx[SC_MAX_V][3];          /* welded verts, world coords   */
static uint8_t  s_vc[SC_MAX_V][3];          /* per-vertex color             */
static uint16_t s_fc[SC_MAX_F][3];          /* triangles                    */
static int s_nv, s_nf;
static int s_selV   = -1;                   /* selected vertex              */
static int s_dragV  = -1;                   /* vertex being dragged         */
static v3  s_dragN;                         /* drag plane normal (cam fwd)  */
static v3  s_dragPt;                        /* drag plane anchor (grab pos) */
static v3  s_dragOff;                       /* grab offset on the plane     */
static int s_scale  = 1;                    /* marker cube size in blocks   */
static int s_editMid = -1;                  /* >=0: editing g_models[]      */

/* ---------- helpers ---------- */

static float snap_step(void) { return s_scale / 8.0f; }
static float merge_eps(void) { return snap_step() * 0.45f; }
static void snapv(v3 *p) {
    float s = snap_step();
    p->x = snapf(p->x, s); p->y = snapf(p->y, s); p->z = snapf(p->z, s);
}
static v3 vert_at(int i) { return v3_make(s_vx[i][0], s_vx[i][1], s_vx[i][2]); }
static void vert_set(int i, v3 p) {
    s_vx[i][0] = p.x; s_vx[i][1] = p.y; s_vx[i][2] = p.z;
}
static void vert_col(int i, const float c[3]) {
    s_vc[i][0] = (uint8_t)(c[0]*255.0f); s_vc[i][1] = (uint8_t)(c[1]*255.0f);
    s_vc[i][2] = (uint8_t)(c[2]*255.0f);
}

/* ---------- mesh ops (ported) ---------- */

static void init_cube(void) {
    float h = s_scale * 0.5f, t = (float)s_scale;
    static const float P[8][3] = {
        {-1,0, 1},{ 1,0, 1},{ 1,1, 1},{-1,1, 1},   /* front ring (z+)  */
        {-1,0,-1},{ 1,0,-1},{ 1,1,-1},{-1,1,-1}    /* back ring  (z-)  */
    };
    static const uint16_t F[12][3] = {
        {0,1,2},{0,2,3},   /* front  */
        {1,5,6},{1,6,2},   /* right  */
        {5,4,7},{5,7,6},   /* back   */
        {4,0,3},{4,3,7},   /* left   */
        {3,2,6},{3,6,7},   /* top    */
        {4,5,1},{4,1,0}    /* bottom */
    };
    s_nv = 8; s_nf = 12;
    for (int i = 0; i < 8; i++) {
        s_vx[i][0] = P[i][0] * h;
        s_vx[i][1] = P[i][1] * t;
        s_vx[i][2] = P[i][2] * h;
        vert_col(i, A.paintCol);
    }
    memcpy(s_fc, F, sizeof F);
    s_selV = s_dragV = -1;
}

/* unique undirected edges of the mesh */
static int get_edges(int (*e)[2], int maxe) {
    int n = 0;
    for (int f = 0; f < s_nf; f++) {
        for (int k = 0; k < 3; k++) {
            int a = s_fc[f][k], b = s_fc[f][(k+1) % 3];
            int lo = a < b ? a : b, hi = a < b ? b : a, dup = 0;
            for (int i = 0; i < n; i++)
                if (e[i][0] == lo && e[i][1] == hi) { dup = 1; break; }
            if (!dup && n < maxe) { e[n][0] = lo; e[n][1] = hi; n++; }
        }
    }
    return n;
}

/* split every face containing edge (a,b) by inserting vertex v (ported) */
static void split_edge_faces(int a, int b, int v) {
    uint16_t out[SC_MAX_F][3]; int on = 0;
    for (int f = 0; f < s_nf; f++) {
        uint16_t *fc = s_fc[f];
        int ia = -1, ib = -1, io = -1;
        for (int k = 0; k < 3; k++) {
            if (fc[k] == a) ia = k; else if (fc[k] == b) ib = k; else io = k;
        }
        if (ia < 0 || ib < 0 || on + 2 > SC_MAX_F) {           /* keep unsplit */
            if (on < SC_MAX_F) memcpy(out[on++], fc, 6);
            continue;
        }
        int aPrecedesB = (ia + 1) % 3 == ib;                   /* directed edge? */
        uint16_t o = fc[io];
        if (aPrecedesB) {
            uint16_t f1[3] = {(uint16_t)a, (uint16_t)v, o};
            uint16_t f2[3] = {(uint16_t)v, (uint16_t)b, o};
            memcpy(out[on++], f1, 6); memcpy(out[on++], f2, 6);
        } else {
            uint16_t f1[3] = {(uint16_t)b, (uint16_t)v, o};
            uint16_t f2[3] = {(uint16_t)v, (uint16_t)a, o};
            memcpy(out[on++], f1, 6); memcpy(out[on++], f2, 6);
        }
    }
    memcpy(s_fc, out, on * 6); s_nf = on;
}

/* weld verts closer than eps; drop degenerate faces */
static void merge_verts(void) {
    float eps = merge_eps();
    for (int again = 1; again; ) {
        again = 0;
        for (int i = 0; i < s_nv && !again; i++) {
            for (int j = i + 1; j < s_nv && !again; j++) {
                v3 d = v3_sub(vert_at(i), vert_at(j));
                if (v3_len(d) >= eps) continue;
                for (int f = 0; f < s_nf; f++)
                    for (int k = 0; k < 3; k++)
                        if (s_fc[f][k] == j) s_fc[f][k] = (uint16_t)i;
                /* drop degenerate faces */
                int w = 0;
                for (int f = 0; f < s_nf; f++)
                    if (s_fc[f][0] != s_fc[f][1] && s_fc[f][1] != s_fc[f][2] &&
                        s_fc[f][0] != s_fc[f][2])
                        memcpy(s_fc[w++], s_fc[f], 6);
                s_nf = w;
                if (j < s_nv - 1) {
                    memmove(s_vx[j], s_vx[j+1], (s_nv-j-1) * sizeof s_vx[0]);
                    memmove(s_vc[j], s_vc[j+1], (s_nv-j-1) * sizeof s_vc[0]);
                }
                s_nv--;
                for (int f = 0; f < s_nf; f++)
                    for (int k = 0; k < 3; k++)
                        if (s_fc[f][k] > j) s_fc[f][k]--;
                if (s_selV == j) s_selV = -1;
                if (s_dragV == j) s_dragV = -1;
                if (s_selV > j) s_selV--;
                if (s_dragV > j) s_dragV--;
                again = 1;
            }
        }
    }
}

/* delete one vertex + all faces touching it (ported) */
static void delete_vert(int v) {
    int w = 0;
    for (int f = 0; f < s_nf; f++)
        if (s_fc[f][0] != v && s_fc[f][1] != v && s_fc[f][2] != v)
            memcpy(s_fc[w++], s_fc[f], 6);
    s_nf = w;
    if (v < s_nv - 1) {
        memmove(s_vx[v], s_vx[v+1], (s_nv-v-1) * sizeof s_vx[0]);
        memmove(s_vc[v], s_vc[v+1], (s_nv-v-1) * sizeof s_vc[0]);
    }
    s_nv--;
    for (int f = 0; f < s_nf; f++)
        for (int k = 0; k < 3; k++)
            if (s_fc[f][k] > v) s_fc[f][k]--;
    s_selV = s_dragV = -1;
}

/* shortest-distance segment-segment crossing test (ported) */
static int seg_cross(v3 p1, v3 p2, v3 p3, v3 p4, v3 *out) {
    v3 u = v3_sub(p2,p1), v = v3_sub(p4,p3), w = v3_sub(p1,p3);
    float a=v3_dot(u,u), b=v3_dot(u,v), c=v3_dot(v,v), d=v3_dot(u,w), e=v3_dot(v,w);
    float D=a*c-b*b, sN,sD=D, tN,tD=D;
    if (D < 1e-8f) { sN=0; sD=1; tN=e; tD=c; }
    else {
        sN = b*e - c*d; tN = a*e - b*d;
        if (sN < 0) { sN=0; tN=e; tD=c; }
        else if (sN > sD) { sN=sD; tN=e+b; tD=c; }
    }
    if (tN < 0) {
        tN = 0;
        if (-d < 0) sN = 0; else if (-d > a) sN = sD; else { sN=-d; sD=a; }
    } else if (tN > tD) {
        tN = tD;
        if (-d+b < 0) sN = 0; else if (-d+b > a) sN = sD; else { sN=-d+b; sD=a; }
    }
    float sc = fabsf(sN) < 1e-8f ? 0 : sN/sD;
    float tc = fabsf(tN) < 1e-8f ? 0 : tN/tD;
    if (sc < 0.05f || sc > 0.95f || tc < 0.05f || tc > 0.95f) return 0;
    v3 q1 = v3_add(p1, v3_scale(u, sc)), q2 = v3_add(p3, v3_scale(v, tc));
    if (v3_dist(q1, q2) < merge_eps() * 2.0f) {
        *out = v3_scale(v3_add(q1, q2), 0.5f);
        return 1;
    }
    return 0;
}

/* after a drag: split both edges wherever they cross, up to 10 rounds */
static void resolve_intersections(void) {
    static int edges[SC_MAX_F * 3][2];
    int found = 1, it = 0;
    while (found && it++ < 10) {
        found = 0;
        int ne = get_edges(edges, SC_MAX_F * 3);
        for (int i = 0; i < ne && !found; i++) {
            for (int j = i + 1; j < ne && !found; j++) {
                int *e1 = edges[i], *e2 = edges[j];
                if (e1[0]==e2[0] || e1[0]==e2[1] || e1[1]==e2[0] || e1[1]==e2[1])
                    continue;
                v3 pt;
                if (seg_cross(vert_at(e1[0]), vert_at(e1[1]),
                              vert_at(e2[0]), vert_at(e2[1]), &pt)) {
                    if (s_nv >= SC_MAX_V) return;
                    snapv(&pt);
                    int nv = s_nv;
                    vert_set(s_nv, pt);
                    vert_col(s_nv, A.paintCol);
                    s_nv++;
                    split_edge_faces(e1[0], e1[1], nv);
                    split_edge_faces(e2[0], e2[1], nv);
                    found = 1;
                }
            }
        }
    }
    merge_verts();
}

/* ---------- picking ---------- */

/* distance between ray (ro+rd*t) and segment a-b; returns dist, on-seg pt */
static float ray_seg(v3 ro, v3 rd, v3 a, v3 b, v3 *onSeg, float *rt) {
    v3 u = v3_sub(b,a), w0 = v3_sub(a,ro);
    float A = v3_dot(u,u), B = v3_dot(u,rd), D = v3_dot(u,w0), E = v3_dot(rd,w0);
    float s, t;
    float den = A - B*B;                       /* |rd| = 1 */
    if (A < 1e-8f || fabsf(den) < 1e-8f) { s = 0; t = E; }
    else {
        s = (B*E - D) / den;
        if (s < 0) s = 0; else if (s > 1) s = 1;
        t = E + B*s;
    }
    *onSeg = v3_add(a, v3_scale(u, s));
    *rt = t;
    return v3_dist(*onSeg, v3_add(ro, v3_scale(rd, t)));
}

static int pick_vert(v3 ro, v3 rd) {
    int best = -1; float bt = 1e30f;
    for (int i = 0; i < s_nv; i++) {
        v3 p = vert_at(i);
        v3 w = v3_sub(p, ro);
        float t = v3_dot(w, rd);
        if (t < 0.05f) continue;
        v3 q = v3_add(ro, v3_scale(rd, t));
        if (v3_dist(p, q) < t * 0.045f && t < bt) { bt = t; best = i; }
    }
    return best;
}

/* RMB click: split the nearest edge to the ray (ported) */
static void edge_split_at(int mx, int my) {
    (void)mx; (void)my;
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    static int edges[SC_MAX_F * 3][2];
    int ne = get_edges(edges, SC_MAX_F * 3);
    int best = -1; float bd = 1e30f; v3 bp = v3_make(0,0,0);
    for (int i = 0; i < ne; i++) {
        v3 on; float rt;
        float d = ray_seg(ro, rd, vert_at(edges[i][0]), vert_at(edges[i][1]),
                          &on, &rt);
        if (rt < 0.05f) continue;              /* behind camera */
        if (d < bd) { bd = d; bp = on; best = i; }
    }
    if (best < 0) return;
    float thr = v3_dist(bp, ro) * 0.08f;       /* screen-proportional */
    if (bd >= thr) return;
    if (s_nv >= SC_MAX_V) return;
    snapv(&bp);
    int nv = s_nv;
    vert_set(s_nv, bp);
    /* new vert color = blend of the edge's endpoints */
    for (int k = 0; k < 3; k++)
        s_vc[s_nv][k] = (uint8_t)((s_vc[edges[best][0]][k] +
                                   s_vc[edges[best][1]][k]) / 2);
    s_nv++;
    split_edge_faces(edges[best][0], edges[best][1], nv);
    merge_verts();
    s_selV = -1;
}

/* ---------- modal plumbing ---------- */

static void sculpt_done(int save) {
    if (save && s_nv >= 3 && s_nf >= 1) {
        /* center verts on the AABB center; size = true bounds */
        v3 lo = v3_make(1e9f,1e9f,1e9f), hi = v3_make(-1e9f,-1e9f,-1e9f);
        for (int i = 0; i < s_nv; i++) {
            v3 p = vert_at(i);
            lo = v3_make(fminf(lo.x,p.x), fminf(lo.y,p.y), fminf(lo.z,p.z));
            hi = v3_make(fmaxf(hi.x,p.x), fmaxf(hi.y,p.y), fmaxf(hi.z,p.z));
        }
        v3 ctr = v3_scale(v3_add(lo, hi), 0.5f);
        for (int i = 0; i < s_nv; i++) {
            v3 p = v3_sub(vert_at(i), ctr);
            vert_set(i, p);
        }
        if (s_editMid >= 0 && s_editMid < g_nmodels && g_models[s_editMid].vox) {
            Model *m = &g_models[s_editMid];
            if (s_nv <= MODEL_MAX_VERTS && s_nf <= MODEL_MAX_FACES) {
                m->nvert = s_nv; m->nface = s_nf;
                memcpy(m->mv, s_vx, s_nv * sizeof s_vx[0]);
                memcpy(m->mvcol, s_vc, s_nv * sizeof s_vc[0]);
                memcpy(m->mf, s_fc, s_nf * sizeof s_fc[0]);
                v3 sz = v3_sub(hi, lo);
                m->size = v3_make(fmaxf(sz.x,0.125f), fmaxf(sz.y,0.125f),
                                  fmaxf(sz.z,0.125f));
                models_rebake_all();
                models_save(NULL);
            }
        } else if (model_sculpt_topo(s_vx, s_vc, s_fc, s_nv, s_nf) >= 0) {
            rk_models_commit();
            models_save(NULL);
        }
    }
    s_selV = s_dragV = -1; s_editMid = -1;
    A.modal = 0;
}

static void sculpt_enter(void) {
    s_selV = s_dragV = -1;
    A.modal = 2; A.drag = M_IDLE; A.armedAsset = -1;
    sel_clear();
    A.camPos = v3_make(2.6f, 2.0f, 2.6f);
    A.yaw = 0.785398f; A.pitch = -0.42f;
}

void sculpt_open(void) {
    s_scale = 1; s_editMid = -1;
    init_cube();
    sculpt_enter();
}

/* re-open an existing sculpted model for editing */
void sculpt_open_model(int mid) {
    if (mid < 0 || mid >= g_nmodels || !g_models[mid].vox) return;
    Model *m = &g_models[mid];
    if (m->nvert < 1 || m->nface < 1) return;
    s_nv = m->nvert; s_nf = m->nface;
    memcpy(s_vx, m->mv, s_nv * sizeof s_vx[0]);
    memcpy(s_vc, m->mvcol, s_nv * sizeof s_vc[0]);
    memcpy(s_fc, m->mf, s_nf * sizeof s_fc[0]);
    /* verts are centered on the AABB - lift so the mesh sits on the ground */
    for (int i = 0; i < s_nv; i++) s_vx[i][1] += m->size.y * 0.5f;
    float mx = fmaxf(m->size.x, fmaxf(m->size.y, m->size.z));
    s_scale = mx > 2.5f ? 4 : (mx > 1.25f ? 2 : 1);
    s_editMid = mid;
    sculpt_enter();
}

/* ---------- UI ---------- */

enum { SB_S1, SB_S2, SB_S4, SB_RESET, SB_DONE, SB_CANCEL, SB_N };
static const char *SBN[SB_N] = {"1X","2X","4X","RESET","DONE","EXIT"};

static float sbtn_x(int i) { return 16.0f + i * 62.0f; }
static const float SBTN_Y = 40.0f, SBTN_W = 56.0f, SBTN_H = 24.0f;

int sculpt_press(int mx, int my) {
    float x = (float)mx, y = (float)my;
    for (int i = 0; i < SB_N; i++) {
        float bx = sbtn_x(i);
        if (x >= bx && x < bx + SBTN_W && y >= SBTN_Y && y < SBTN_Y + SBTN_H) {
            switch (i) {
            case SB_S1: s_scale = 1; break;
            case SB_S2: s_scale = 2; break;
            case SB_S4: s_scale = 4; break;
            case SB_RESET: init_cube(); break;
            case SB_DONE:  sculpt_done(1); break;
            case SB_CANCEL:sculpt_done(0); break;
            }
            return 1;
        }
    }
    /* palette column: recolor selected vertex (or whole mesh if none) */
    float pxx = (float)A.fbw - 44.0f;
    for (int i = 0; i < 16; i++) {
        float py = SBTN_Y + i * 30.0f;
        if (x >= pxx && x < pxx + 26 && y >= py && y < py + 26) {
            const float *c = panel_color(i);
            memcpy(A.paintCol, c, sizeof(float)*3);
            if (s_selV >= 0) vert_col(s_selV, c);
            else for (int k = 0; k < s_nv; k++) vert_col(k, c);
            return 1;
        }
    }
    /* vertex pick -> drag on camera-parallel plane */
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    int v = pick_vert(ro, rd);
    s_selV = v;
    if (v >= 0) {
        s_dragV = v;
        s_dragN = cam_basis().f;
        s_dragPt = vert_at(v);
        /* plane through the vertex, facing the camera */
        float denom = v3_dot(s_dragN, rd);
        if (fabsf(denom) > 1e-6f) {
            float t = v3_dot(s_dragN, v3_sub(s_dragPt, ro)) / denom;
            v3 onPlane = v3_add(ro, v3_scale(rd, t));
            s_dragOff = v3_sub(onPlane, s_dragPt);
        } else s_dragOff = v3_make(0,0,0);
    }
    return 1;
}

/* RMB click (< few px of movement): split nearest edge */
void sculpt_rmb_click(int mx, int my) {
    (void)mx; (void)my;
    edge_split_at(mx, my);
}

void sculpt_release(void) {
    if (s_dragV >= 0) { merge_verts(); resolve_intersections(); }
    s_dragV = -1;
}

void sculpt_move(int mx, int my) {
    A.mx = mx; A.my = my;
    if (s_dragV < 0) return;
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    float denom = v3_dot(s_dragN, rd);
    if (fabsf(denom) < 1e-6f) return;
    float t = v3_dot(s_dragN, v3_sub(s_dragPt, ro)) / denom;
    v3 p = v3_sub(v3_add(ro, v3_scale(rd, t)), s_dragOff);
    snapv(&p);
    vert_set(s_dragV, p);
}

void sculpt_wheel(double dy) {
    cam_zoom_cursor((float)dy, A.mx, A.my);
}

int sculpt_key(int k) {
    if (k == GLFW_KEY_ESCAPE) {
        if (s_selV >= 0 || s_dragV >= 0) { s_selV = s_dragV = -1; return 1; }
        sculpt_done(0); return 1;
    }
    if (k == GLFW_KEY_ENTER)  { sculpt_done(1); return 1; }
    if (k == GLFW_KEY_DELETE || k == GLFW_KEY_BACKSPACE) {
        if (s_selV >= 0) delete_vert(s_selV);
        return 1;
    }
    return 1;
}

/* ---------- draw ---------- */

static void mesh_aabb(v3 *lo, v3 *hi) {
    *lo = v3_make(1e9f,1e9f,1e9f); *hi = v3_make(-1e9f,-1e9f,-1e9f);
    for (int i = 0; i < s_nv; i++) {
        v3 p = vert_at(i);
        lo->x = fminf(lo->x,p.x); lo->y = fminf(lo->y,p.y); lo->z = fminf(lo->z,p.z);
        hi->x = fmaxf(hi->x,p.x); hi->y = fmaxf(hi->y,p.y); hi->z = fmaxf(hi->z,p.z);
    }
}

void sculpt_draw(void) {
    float h = s_scale * 0.5f;
    m4 idm; m4_identity(idm);

    /* solid mesh: flat normals, per-vertex colors */
    static Vert tbuf[SC_MAX_F * 3];
    int tn = 0;
    for (int f = 0; f < s_nf && tn + 3 <= SC_MAX_F * 3; f++) {
        v3 a = vert_at(s_fc[f][0]), b = vert_at(s_fc[f][1]), c = vert_at(s_fc[f][2]);
        v3 n = v3_norm(v3_cross(v3_sub(b,a), v3_sub(c,a)));
        for (int k = 0; k < 3; k++) {
            v3 p = k == 0 ? a : (k == 1 ? b : c);
            Vert *v = &tbuf[tn++];
            memcpy(v->pos, p.e, 12);
            memcpy(v->nrm, n.e, 12);
            int vi = s_fc[f][k];
            v->col[0] = s_vc[vi][0]/255.0f; v->col[1] = s_vc[vi][1]/255.0f;
            v->col[2] = s_vc[vi][2]/255.0f;
        }
    }
    if (tn) rk_mesh(tbuf, (uint32_t)tn, idm);

    /* wireframe: unique edges as line pairs */
    static Vert lbuf[SC_MAX_F * 6];
    static int edges[SC_MAX_F * 3][2];
    int ne = get_edges(edges, SC_MAX_F * 3);
    int ln = 0;
    for (int i = 0; i < ne && ln + 2 <= SC_MAX_F * 6; i++) {
        v3 a = vert_at(edges[i][0]), b = vert_at(edges[i][1]);
        memcpy(lbuf[ln].pos, a.e, 12); ln++;
        memcpy(lbuf[ln].pos, b.e, 12); ln++;
    }
    if (ln) rk_meshlines(lbuf, (uint32_t)ln, idm, 0.25f, 0.75f, 1.0f, 0.5f);

    /* marker box (size reference, always the 1X/2X/4X cube) */
    RkInst box;
    memset(&box, 0, sizeof box);
    box.pos[0] = 0; box.pos[1] = h; box.pos[2] = 0;
    box.scale[0] = box.scale[1] = box.scale[2] = (float)s_scale;
    box.color = rk_rgba(0.4f, 0.6f, 0.9f, 0.8f);
    rk_lines(&box, 1);

    /* second outline: true mesh bounds when they exceed the marker */
    if (s_nv) {
        v3 lo, hi; mesh_aabb(&lo, &hi);
        if (lo.x < -h-1e-3f || hi.x > h+1e-3f || lo.z < -h-1e-3f ||
            hi.z > h+1e-3f || lo.y < -1e-3f || hi.y > s_scale+1e-3f) {
            RkInst bb;
            memset(&bb, 0, sizeof bb);
            v3 c = v3_scale(v3_add(lo,hi), 0.5f), s = v3_sub(hi,lo);
            bb.pos[0]=c.x; bb.pos[1]=c.y; bb.pos[2]=c.z;
            bb.scale[0]=s.x; bb.scale[1]=s.y; bb.scale[2]=s.z;
            bb.color = rk_rgba(0.95f, 0.95f, 1.0f, 1.0f);
            rk_lines(&bb, 1);
        }
    }

    /* vertex dots */
    static RkInst dots[SC_MAX_V];
    int dn = 0;
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    int hov = pick_vert(ro, rd);
    for (int i = 0; i < s_nv; i++) {
        float d = v3_dist(vert_at(i), ro);
        float r = d * 0.028f;
        if (i == s_selV) r *= 1.4f; else if (i == hov) r *= 1.25f;
        /* pull toward the camera so the dot sits proud of the surface */
        v3 p = v3_add(vert_at(i),
                      v3_scale(v3_norm(v3_sub(ro, vert_at(i))), r * 0.6f));
        dots[dn].pos[0]=p.x; dots[dn].pos[1]=p.y; dots[dn].pos[2]=p.z;
        dots[dn].scale[0]=dots[dn].scale[1]=dots[dn].scale[2]=r;
        int sel = (i == s_selV);
        dots[dn].color = rk_rgba(1.0f, sel ? 1.0f : 0.30f,
                                 sel ? 1.0f : 0.30f, 1.0f);
        dots[dn].flags = RKIF_UNLIT | RKIF_NOFOG;
        dn++;
    }
    if (dn) rk_overlay(RK_MESH_CUBE, dots, (uint32_t)dn);

    /* toolbar */
    rk_ui_rect(0, 0, (float)A.fbw, 72, 0.05f, 0.06f, 0.08f, 0.85f);
    ui_text(16, 16, s_editMid >= 0 ? "SCULPT MODE - EDITING ASSET"
                                   : "SCULPT MODE - NEW ASSET",
            1, 0.9f, 0.92f, 1.0f, 1);
    for (int i = 0; i < SB_N; i++) {
        float bx = sbtn_x(i);
        int act = (i == SB_S1 && s_scale == 1) || (i == SB_S2 && s_scale == 2) ||
                  (i == SB_S4 && s_scale == 4);
        int hov = A.mx >= bx && A.mx < bx + SBTN_W &&
                  A.my >= SBTN_Y && A.my < SBTN_Y + SBTN_H;
        rk_ui_rect(bx, SBTN_Y, SBTN_W, SBTN_H,
                   act ? 0.30f : 0.16f + hov*0.06f, act ? 0.45f : 0.18f + hov*0.06f,
                   act ? 0.65f : 0.24f + hov*0.06f, 1);
        ui_text(bx + (SBTN_W - ui_text_w(SBN[i], 1)) * 0.5f, SBTN_Y + 8,
                SBN[i], 1, 1, 1, 1, 1);
    }
    /* verts/faces counter */
    char cnt[64]; snprintf(cnt, sizeof cnt, "VERTS %d/%d   TRIS %d/%d",
                           s_nv, SC_MAX_V, s_nf, SC_MAX_F);
    ui_text(sbtn_x(SB_N) + 16, SBTN_Y + 8, cnt, 1, 0.6f, 0.7f, 0.8f, 1);

    /* palette column */
    float pxx = (float)A.fbw - 44.0f;
    for (int i = 0; i < 16; i++) {
        const float *c = panel_color(i);
        rk_ui_rect(pxx, SBTN_Y + i * 30.0f, 26, 26, c[0], c[1], c[2], 1);
    }
    rk_ui_rect(pxx, SBTN_Y + 16 * 30.0f + 6, 26, 26,
               A.paintCol[0], A.paintCol[1], A.paintCol[2], 1);
    ui_text(16, (float)A.fbh - 40,
            "LMB DRAG VERTEX   RMB CLICK EDGE = SPLIT   DEL VERTEX", 1,
            0.55f, 0.6f, 0.7f, 1);
    ui_text(16, (float)A.fbh - 24,
            "RMB DRAG = LOOK   WHEEL ZOOM   ENTER DONE   ESC EXIT", 1,
            0.55f, 0.6f, 0.7f, 1);
    rk_ui_flush();
}

/* ---------- legacy density-field bake (models.bw <= v5 migration) ---------- */

static int vidx(int x, int y, int z) { return (z*SN + y)*SN + x; }
static int inb(int x, int y, int z) {
    return x >= 0 && y >= 0 && z >= 0 && x < SN && y < SN && z < SN;
}
static const int TETS[6][4] = {
    {0,1,3,7},{0,1,5,7},{0,2,3,7},{0,2,6,7},{0,4,5,7},{0,4,6,7}
};
static v3 ngrad(const float *den, int x, int y, int z) {
    #define DN(a,b,c) (inb(a,b,c) ? den[vidx(a,b,c)] : 0.0f)
    return v3_make(DN(x+1,y,z) - DN(x-1,y,z),
                   DN(x,y+1,z) - DN(x,y-1,z),
                   DN(x,y,z+1) - DN(x,y,z-1));
    #undef DN
}
typedef struct { v3 p; float c[3]; v3 n; } MV;
static MV ex(const float *den, const uint8_t (*col)[3],
             int ax, int ay, int az, int bx, int by, int bz,
             v3 pa, v3 pb, v3 ga, v3 gb, v3 faceN) {
    float da = den[vidx(ax,ay,az)], db = den[vidx(bx,by,bz)];
    float t = (ISO - da) / (db - da);
    if (t < 0) t = 0; if (t > 1) t = 1;
    MV r;
    r.p = v3_add(pa, v3_scale(v3_sub(pb, pa), t));
    for (int i = 0; i < 3; i++)
        r.c[i] = (col[vidx(ax,ay,az)][i] +
                  (col[vidx(bx,by,bz)][i] - col[vidx(ax,ay,az)][i]) * t) / 255.0f;
    v3 g = v3_scale(v3_add(ga, v3_scale(v3_sub(gb, ga), t)), -1.0f);
    r.n = v3_dot(g, g) > 1e-8f ? v3_norm(g) : faceN;
    return r;
}
static void etri(Vert *o, int *n, int max, MV a, MV b, MV c, v3 emptySide) {
    if (*n + 3 > max) return;
    v3 fn = v3_cross(v3_sub(b.p, a.p), v3_sub(c.p, a.p));
    v3 cc = v3_scale(v3_add(v3_add(a.p, b.p), c.p), 1.0f/3.0f);
    if (v3_dot(fn, v3_sub(emptySide, cc)) < 0) { MV t = b; b = c; c = t; }
    MV vs[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        Vert *v = &o[(*n)++];
        memcpy(v->pos, vs[i].p.e, 12);
        memcpy(v->nrm, vs[i].n.e, 12);
        memcpy(v->col, vs[i].c, 12);
    }
}
static void mtet(Vert *o, int *n, int max,
                 const int nd[4][3], v3 cp[4], v3 cg[4],
                 const float *den, const uint8_t (*col)[3]) {
    int m = 0;
    for (int i = 0; i < 4; i++)
        if (den[vidx(nd[i][0], nd[i][1], nd[i][2])] >= ISO) m |= 1 << i;
    if (!m || m == 15) return;
    int sol[4], emp[4], ns = 0, ne = 0;
    for (int i = 0; i < 4; i++)
        (m & (1<<i)) ? (void)(sol[ns++]=i) : (void)(emp[ne++]=i);
    v3 eside = v3_make(0,0,0);
    for (int i = 0; i < ne; i++) eside = v3_add(eside, cp[emp[i]]);
    eside = v3_scale(eside, 1.0f/ne);
    v3 fn = v3_norm(v3_cross(v3_sub(cp[1],cp[0]), v3_sub(cp[2],cp[0])));
    if (ns == 1 || ns == 3) {
        int x  = ns == 1 ? sol[0] : emp[0];
        int o1 = ns == 1 ? emp[0] : sol[0];
        int o2 = ns == 1 ? emp[1] : sol[1];
        int o3 = ns == 1 ? emp[2] : sol[2];
        MV a = ex(den, col, nd[x][0],nd[x][1],nd[x][2],
                  nd[o1][0],nd[o1][1],nd[o1][2], cp[x], cp[o1], cg[x], cg[o1], fn);
        MV b = ex(den, col, nd[x][0],nd[x][1],nd[x][2],
                  nd[o2][0],nd[o2][1],nd[o2][2], cp[x], cp[o2], cg[x], cg[o2], fn);
        MV c = ex(den, col, nd[x][0],nd[x][1],nd[x][2],
                  nd[o3][0],nd[o3][1],nd[o3][2], cp[x], cp[o3], cg[x], cg[o3], fn);
        etri(o, n, max, a, b, c, eside);
    } else {
        int a = sol[0], b = sol[1], c = emp[0], d = emp[1];
        MV vac = ex(den, col, nd[a][0],nd[a][1],nd[a][2],
                    nd[c][0],nd[c][1],nd[c][2], cp[a], cp[c], cg[a], cg[c], fn);
        MV vad = ex(den, col, nd[a][0],nd[a][1],nd[a][2],
                    nd[d][0],nd[d][1],nd[d][2], cp[a], cp[d], cg[a], cg[d], fn);
        MV vbc = ex(den, col, nd[b][0],nd[b][1],nd[b][2],
                    nd[c][0],nd[c][1],nd[c][2], cp[b], cp[c], cg[b], cg[c], fn);
        MV vbd = ex(den, col, nd[b][0],nd[b][1],nd[b][2],
                    nd[d][0],nd[d][1],nd[d][2], cp[b], cp[d], cg[b], cg[d], fn);
        etri(o, n, max, vac, vad, vbd, eside);
        etri(o, n, max, vac, vbd, vbc, eside);
    }
}

int sculpt_bake_verts(const float *den, const uint8_t (*col)[3], int size,
                      Vert *out, int max) {
    int nv = 0;
    float p = (float)size / (SN - 1), h = size * 0.5f;
    for (int z = 0; z < SN-1; z++)
    for (int y = 0; y < SN-1; y++)
    for (int x = 0; x < SN-1; x++) {
        int inside = 0, allin = 1;
        for (int c = 0; c < 8; c++)
            if (den[vidx(x+(c&1), y+((c>>1)&1), z+((c>>2)&1))] >= ISO)
                inside = 1;
            else allin = 0;
        if (!inside || allin) continue;
        for (int t = 0; t < 6; t++) {
            int nd[4][3]; v3 tp[4], tg[4];
            for (int i = 0; i < 4; i++) {
                int c = TETS[t][i];
                nd[i][0] = x + (c&1);
                nd[i][1] = y + ((c>>1)&1);
                nd[i][2] = z + ((c>>2)&1);
                tp[i] = v3_make(nd[i][0]*p - h, nd[i][1]*p - h,
                                nd[i][2]*p - h);
                tg[i] = ngrad(den, nd[i][0], nd[i][1], nd[i][2]);
            }
            mtet(out, &nv, max, nd, tp, tg, den, col);
        }
    }
    return nv;
}

void sculpt_vox_unpack(const uint8_t (*vox4)[4], float *den, uint8_t (*col)[3]) {
    for (int i = 0; i < SNV; i++) {
        den[i] = vox4[i][3] / 255.0f;
        col[i][0] = vox4[i][0]; col[i][1] = vox4[i][1]; col[i][2] = vox4[i][2];
    }
}
void sculpt_vox_unpack16(const uint16_t *vox2, float *den, uint8_t (*col)[3]) {
    for (int z = 0; z < SN; z++)
    for (int y = 0; y < SN; y++)
    for (int x = 0; x < SN; x++) {
        int ox = (int)(x * 15.0f / (SN-1) + 0.5f);
        int oy = (int)(y * 15.0f / (SN-1) + 0.5f);
        int oz = (int)(z * 15.0f / (SN-1) + 0.5f);
        uint16_t v = vox2[(oz*16 + oy)*16 + ox];
        int i = vidx(x,y,z);
        den[i] = (v & 0x8000) ? 1.0f : 0.0f;
        col[i][0] = (uint8_t)(((v>>11) & 31) * 255 / 31);
        col[i][1] = (uint8_t)(((v>>5)  & 63) * 255 / 63);
        col[i][2] = (uint8_t)((v & 31) * 255 / 31);
    }
}
