/* sculpt.c - modal vertex-drag mesh editor (A.modal == 2). Ported from a
 * reference JS prototype: a welded triangle mesh starts as a cube inside a
 * "size marker" box; LMB grabs a vertex dot and drags it on a
 * camera-parallel plane (grid-snapped), RMB-click splits the nearest edge,
 * Del removes the selected vertex, and on release overlapping verts weld +
 * crossing edges resolve by splitting both. The marker box is a reference -
 * the mesh may exceed it, in which case a second outline shows the true
 * bounds. DONE bakes the topology into a CUSTOM model (normalized unit mesh
 * + per-vertex colors); RESCULPT reloads it for further editing. */
#define _CRT_SECURE_NO_WARNINGS
#include "sculpt.h"
#include "edit.h"
#include "model.h"
#include "panel.h"
#include "camera.h"
#include "font.h"
#include "rk.h"
#include "tex.h"
#include "world.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#pragma comment(lib, "comdlg32")
#endif

#define SN   SC_N
#define SNV  SC_VOX_N
#define ISO  0.5f
#define SC_LIFT 2.0f                        /* mesh floats, centered here   */

static float    s_vx[SC_MAX_V][3];          /* welded verts, world coords   */
static uint8_t  s_vc[SC_MAX_V][3];          /* per-vertex color             */
static int8_t   s_vtex[SC_MAX_V];           /* material id, -1 = inherit    */
static uint8_t  s_vlock[SC_MAX_V];          /* material layer locked        */
static uint16_t s_fc[SC_MAX_F][3];          /* triangles                    */
static int s_nv, s_nf;
static int s_selV   = -1;                   /* selected vertex              */
static int s_dragV  = -1;                   /* vertex being dragged         */
static v3  s_dragN;                         /* drag plane normal (cam fwd)  */
static v3  s_dragPt;                        /* drag plane anchor (grab pos) */
static v3  s_dragOff;                       /* grab offset on the plane     */
static float s_snap = 0.5f;                 /* grid snap step               */
static int  s_tool  = 0;                    /* 0=move 1=addvert 2=part      */
static int s_editMid = -1;                  /* >=0: editing g_models[]      */
static int s_mirror = 0;                    /* 0=off 1=X 2=4-way (X+Z)      */
static int s_density = 0;                   /* subdiv passes per SUBDIV     */
static int s_start   = 0;                   /* starting-shape preset        */
static int s_showGrid = 1;                  /* faint snap lattice           */
static int s_naming = 0;                    /* DONE->name+confirm dialog    */
static char s_name[16];                     /* asset name being typed       */
static char s_toast[80]; static double s_toastT; /* timed notice            */
static int s_part[SC_MAX_V];                /* island verts (PART tool)     */
static int s_partN  = -1;
static float s_partBase[SC_MAX_V][3];       /* island positions at grab     */
static v3  s_partAnchor;                    /* grab point on the face       */
static int s_shapePick = 0;                 /* START-shape picker open      */
static v3  s_dragFrom;                      /* vert pos at grab (delta line)*/

/* ---------- helpers ---------- */

static float seg_hit_tri(v3 p0, v3 p1, v3 a, v3 b, v3 c);   /* fwd decl */
static void  fix_outward(void);
static v3    vert_at(int i);

static float snap_step(void) { return s_snap; }
static float merge_eps(void) { return s_snap * 0.1f; }
static void toast(const char *msg) {
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toastT = glfwGetTime();
}
static void toastf(const char *fmt, int n) {
    snprintf(s_toast, sizeof s_toast, fmt, n);
    s_toastT = glfwGetTime();
}
/* first existing vert within eps of p, or -1 (redundant-add guard) */
static int vert_near(v3 p, float eps) {
    for (int i = 0; i < s_nv; i++)
        if (v3_dist(vert_at(i), p) < eps) return i;
    return -1;
}
/* drop faces that are degenerate: zero-area or out-of-range indices */
static void cull_degenerate(void) {
    int w = 0;
    for (int f = 0; f < s_nf; f++) {
        uint16_t *t = s_fc[f];
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        if (t[0] >= s_nv || t[1] >= s_nv || t[2] >= s_nv) continue;
        v3 a = vert_at(t[0]), b = vert_at(t[1]), c = vert_at(t[2]);
        v3 n = v3_cross(v3_sub(b, a), v3_sub(c, a));
        if (v3_dot(n, n) < 1e-12f) continue;
        if (w != f) memcpy(s_fc[w], s_fc[f], sizeof s_fc[0]);
        w++;
    }
    s_nf = w;
}
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
    const float h = 0.5f;
    static const float P[8][3] = {
        {-1,-1, 1},{ 1,-1, 1},{ 1,1, 1},{-1,1, 1},  /* front ring (z+)  */
        {-1,-1,-1},{ 1,-1,-1},{ 1,1,-1},{-1,1,-1}   /* back ring  (z-)  */
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
        s_vx[i][1] = SC_LIFT + P[i][1] * h;
        s_vx[i][2] = P[i][2] * h;
        vert_col(i, A.paintCol);
        s_vtex[i] = -1; s_vlock[i] = 0;
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
static void merge_verts_e(float eps) {
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
                    memmove(&s_vtex[j], &s_vtex[j+1], s_nv-j-1);
                    memmove(&s_vlock[j], &s_vlock[j+1], s_nv-j-1);
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
    /* drop exact-duplicate faces left by merges/splits */
    {
        int w = 0;
        for (int f = 0; f < s_nf; f++) {
            int dup = 0;
            for (int g = 0; g < w && !dup; g++) {
                uint16_t *x = s_fc[f], *y = s_fc[g];
                if ((x[0]==y[0]||x[0]==y[1]||x[0]==y[2]) &&
                    (x[1]==y[0]||x[1]==y[1]||x[1]==y[2]) &&
                    (x[2]==y[0]||x[2]==y[1]||x[2]==y[2])) dup = 1;
            }
            if (!dup) { if (w != f) memcpy(s_fc[w], s_fc[f], 6); w++; }
        }
        s_nf = w;
    }
}
static void merge_verts(void) { merge_verts_e(merge_eps()); }
static void delete_vert(int v) {
    int w = 0;
    for (int f = 0; f < s_nf; f++)
        if (s_fc[f][0] != v && s_fc[f][1] != v && s_fc[f][2] != v)
            memcpy(s_fc[w++], s_fc[f], 6);
    s_nf = w;
    if (v < s_nv - 1) {
        memmove(s_vx[v], s_vx[v+1], (s_nv-v-1) * sizeof s_vx[0]);
        memmove(s_vc[v], s_vc[v+1], (s_nv-v-1) * sizeof s_vc[0]);
        memmove(&s_vtex[v], &s_vtex[v+1], s_nv-v-1);
        memmove(&s_vlock[v], &s_vlock[v+1], s_nv-v-1);
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
                    s_vtex[s_nv] = s_vtex[e1[0]];
                    s_vlock[s_nv] = s_vlock[e1[0]];
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

/* nearest edge under the cursor: returns edge idx + split point, or -1 */
static int nearest_edge(v3 ro, v3 rd, int (*edges)[2], int ne,
                        v3 *outPt, int *outIdx) {
    int best = -1; float bd = 1e30f; v3 bp = v3_make(0,0,0);
    for (int i = 0; i < ne; i++) {
        v3 on; float rt;
        float d = ray_seg(ro, rd, vert_at(edges[i][0]), vert_at(edges[i][1]),
                          &on, &rt);
        if (rt < 0.05f) continue;              /* behind camera */
        if (d < bd) { bd = d; bp = on; best = i; }
    }
    if (best < 0) return 0;
    float thr = v3_dist(bp, ro) * 0.08f;       /* screen-proportional */
    if (bd >= thr) return 0;
    *outPt = bp; *outIdx = best;
    return 1;
}

/* split the nearest edge to the ray (ported) */
static void edge_split_at(int mx, int my) {
    v3 ro, rd; cam_ray(mx, my, &ro, &rd);
    static int edges[SC_MAX_F * 3][2];
    int ne = get_edges(edges, SC_MAX_F * 3);
    int best = -1; v3 bp;
    if (!nearest_edge(ro, rd, edges, ne, &bp, &best)) return;
    if (s_nv >= SC_MAX_V) return;
    snapv(&bp);
    /* no-op guard: don't add a dot that lands on an existing vert */
    if (vert_near(bp, merge_eps() * 2.0f) >= 0) { toast("DOT ALREADY THERE"); return; }
    int nv = s_nv;
    vert_set(s_nv, bp);
    /* new vert color = blend of the edge's endpoints */
    for (int k = 0; k < 3; k++)
        s_vc[s_nv][k] = (uint8_t)((s_vc[edges[best][0]][k] +
                                   s_vc[edges[best][1]][k]) / 2);
    s_vtex[s_nv] = s_vtex[edges[best][0]];
    s_vlock[s_nv] = s_vlock[edges[best][0]];
    s_nv++;
    split_edge_faces(edges[best][0], edges[best][1], nv);
    merge_verts();
    s_selV = -1;
}

/* ---------- tool helpers ---------- */

/* starting-shape presets: CUBE / PYRAMID / BALL / WEDGE / CYL / SHEET */
static const char *START_NAMES[] = {"CUBE","PYRA","BALL","WEDGE","CYL","SHEET"};
static void push_f(uint16_t a, uint16_t b, uint16_t c) {
    if (s_nf < SC_MAX_F) { s_fc[s_nf][0]=a; s_fc[s_nf][1]=b; s_fc[s_nf][2]=c; s_nf++; }
}
static void push_v(float x, float y, float z) {
    if (s_nv < SC_MAX_V) {
        vert_set(s_nv, v3_make(x,y,z)); vert_col(s_nv, A.paintCol);
        s_vtex[s_nv] = -1; s_vlock[s_nv] = 0;
        s_nv++;
    }
}
static void init_preset(int p) {
    const float h = 0.5f;
    s_nv = s_nf = 0; s_selV = s_dragV = -1; s_partN = -1;
    switch (p) {
    case 1: /* pyramid */
        push_v(-h,SC_LIFT-h,-h); push_v(h,SC_LIFT-h,-h);
        push_v(h,SC_LIFT-h,h);   push_v(-h,SC_LIFT-h,h);
        push_v(0,SC_LIFT+h,0);
        push_f(0,3,4); push_f(3,2,4); push_f(2,1,4); push_f(1,0,4);
        push_f(0,1,2); push_f(0,2,3);
        break;
    case 2: /* icosahedron */
        {
        const float t = 0.80901699f;      /* phi-ish scaled to ~unit */
        const float IV[12][3] = {
            {0,t,1},{0,t,-1},{0,-t,1},{0,-t,-1},
            {1,0,t},{-1,0,t},{1,0,-t},{-1,0,-t},
            {t,1,0},{-t,1,0},{t,-1,0},{-t,-1,0}
        };
        static const uint16_t IF[20][3] = {
            {0,4,2},{0,2,5},{0,5,9},{0,9,8},{0,8,4},
            {3,1,6},{3,6,10},{3,10,11},{3,11,7},{3,7,1},
            {1,8,9},{1,9,7},{1,6,8},{8,6,4},{4,6,10},
            {4,10,2},{2,10,11},{2,11,5},{5,11,7},{7,11,9}
        };
        for (int i = 0; i < 12; i++) push_v(IV[i][0]*h, SC_LIFT+IV[i][1]*h, IV[i][2]*h);
        for (int i = 0; i < 20; i++) push_f(IF[i][0], IF[i][1], IF[i][2]);
        break; }
    case 3: /* wedge (triangular prism, ramp up to -z) */
        push_v(-h,SC_LIFT-h,h);  push_v(h,SC_LIFT-h,h);
        push_v(-h,SC_LIFT-h,-h); push_v(h,SC_LIFT-h,-h);
        push_v(-h,SC_LIFT+h,-h); push_v(h,SC_LIFT+h,-h);
        push_f(0,2,3); push_f(0,3,1);      /* bottom */
        push_f(2,5,3); push_f(2,4,5);      /* back */
        push_f(0,1,5); push_f(0,5,4);      /* slope */
        push_f(1,3,5); push_f(0,4,2);      /* sides */
        break;
    case 4: /* octagonal prism */
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7853982f;
            push_v(cosf(a)*h, SC_LIFT-h, sinf(a)*h);
            push_v(cosf(a)*h, SC_LIFT+h, sinf(a)*h);
        }
        for (int i = 0; i < 8; i++) {
            int a = i*2, b = (i+1)%8*2;
            push_f(a, b, b+1); push_f(a, b+1, a+1);         /* side */
        }
        for (int i = 1; i < 7; i++) { push_f(0, i*2+2, i*2); push_f(1, i*2+1, i*2+3); }
        break;
    case 5: /* 3x3 sheet on XZ */
        for (int z = -1; z <= 1; z++) for (int x = -1; x <= 1; x++)
            push_v(x*h, SC_LIFT, z*h);
        for (int z = 0; z < 2; z++) for (int x = 0; x < 2; x++) {
            int i = z*3 + x;
            push_f(i, i+1, i+4); push_f(i, i+4, i+3);
        }
        break;
    default: init_cube(); return;
    }
    fix_outward();
}

/* flip face windings so every normal points away from the centroid */
static void fix_outward(void) {
    v3 ctr = v3_make(0,0,0);
    for (int i = 0; i < s_nv; i++) ctr = v3_add(ctr, vert_at(i));
    if (s_nv) ctr = v3_scale(ctr, 1.0f / s_nv);
    for (int f = 0; f < s_nf; f++) {
        v3 a = vert_at(s_fc[f][0]), b = vert_at(s_fc[f][1]), c = vert_at(s_fc[f][2]);
        v3 n = v3_cross(v3_sub(b,a), v3_sub(c,a));
        v3 cc = v3_scale(v3_add(v3_add(a,b),c), 1.0f/3.0f);
        if (v3_dot(n, v3_sub(cc, ctr)) < 0) {
            uint16_t t = s_fc[f][1]; s_fc[f][1] = s_fc[f][2]; s_fc[f][2] = t;
        }
    }
}
static void subdivide(void) {
    if (s_density < 1) { toast("SET DIV > 0 FIRST"); return; }
    for (int p = 0; p < s_density; p++) {
        static int edges[SC_MAX_F * 3][2];
        int ne = get_edges(edges, SC_MAX_F * 3), did = 0;
        for (int i = 0; i < ne && s_nv < SC_MAX_V; i++) {
            v3 a = vert_at(edges[i][0]), b = vert_at(edges[i][1]);
            if (v3_dist(a, b) < s_snap * 1.5f) continue;
            v3 mid = v3_scale(v3_add(a, b), 0.5f);
            snapv(&mid);
            /* skip if the midpoint would land on an existing vert -
               a redundant dot with no effect */
            if (vert_near(mid, merge_eps() * 2.0f) >= 0) continue;
            int nv = s_nv;
            vert_set(s_nv, mid);
            for (int k = 0; k < 3; k++)
                s_vc[s_nv][k] = (uint8_t)((s_vc[edges[i][0]][k] +
                                           s_vc[edges[i][1]][k]) / 2);
            s_vtex[s_nv] = s_vtex[edges[i][0]];
            s_vlock[s_nv] = s_vlock[edges[i][0]];
            s_nv++;
            split_edge_faces(edges[i][0], edges[i][1], nv);
            did++;
        }
        if (!did) break;
        merge_verts();
    }
    cull_degenerate();
    toastf("SUBDIVIDED (DIV %d)", s_density);
}

/* laplacian smooth: pull each vertex toward its edge-neighbors' centroid */
static void smooth_mesh(void) {
    static float acc[SC_MAX_V][3]; static int cnt[SC_MAX_V];
    memset(acc, 0, sizeof acc); memset(cnt, 0, sizeof cnt);
    static int edges[SC_MAX_F * 3][2];
    int ne = get_edges(edges, SC_MAX_F * 3);
    for (int i = 0; i < ne; i++) {
        int a = edges[i][0], b = edges[i][1];
        for (int k = 0; k < 3; k++) {
            acc[a][k] += s_vx[b][k]; acc[b][k] += s_vx[a][k];
        }
        cnt[a]++; cnt[b]++;
    }
    for (int i = 0; i < s_nv; i++) {
        if (!cnt[i]) continue;
        for (int k = 0; k < 3; k++)
            s_vx[i][k] += (acc[i][k] / cnt[i] - s_vx[i][k]) * 0.5f;
    }
    merge_verts();
    cull_degenerate();
    toast("SMOOTHED");
}

/* verts sitting at mirrored positions of v (X and/or Z flip) */
static int mirror_partners(int v, int *out) {
    if (!s_mirror || v < 0 || v >= s_nv) return 0;
    v3 p = vert_at(v);
    float eps = merge_eps() * 2.5f + 0.02f;
    int n = 0;
    for (int i = 0; i < s_nv && n < 3; i++) {
        if (i == v) continue;
        v3 q = vert_at(i);
        int ok = 0;
        if (s_mirror >= 1 && fabsf(q.x + p.x) < eps &&
            fabsf(q.y - p.y) < eps && fabsf(q.z - p.z) < eps) ok = 1;
        if (!ok && s_mirror == 2) {
            if (fabsf(q.x - p.x) < eps && fabsf(q.y - p.y) < eps &&
                fabsf(q.z + p.z) < eps) ok = 1;
            if (!ok && fabsf(q.x + p.x) < eps && fabsf(q.y - p.y) < eps &&
                fabsf(q.z + p.z) < eps) ok = 1;
        }
        if (ok) out[n++] = i;
    }
    return n;
}
/* apply mirror transforms of v's position to its partners.
   Partners + transform are recorded once at grab time (s_mirV/s_mirT) -
   re-searching every frame loses them because the dragged vert snaps in
   0.5 steps while the partner still sits at the old mirrored spot. */
static int s_mirN = 0;
static int s_mirV[4];                         /* partner vert indices       */
static int s_mirT[4];                         /* 1 = -x | 2 = -z | 3 = -xz  */
static void mirror_grab(int v) {
    s_mirN = 0;
    if (!s_mirror || v < 0 || v >= s_nv) return;
    v3 p = vert_at(v);
    float eps = merge_eps() * 2.5f + 0.02f;
    for (int i = 0; i < s_nv && s_mirN < 4; i++) {
        if (i == v) continue;
        v3 q = vert_at(i);
        int t = 0;
        if (fabsf(q.x + p.x) < eps && fabsf(q.y - p.y) < eps &&
            fabsf(q.z - p.z) < eps) t = 1;
        else if (s_mirror == 2) {
            if (fabsf(q.x - p.x) < eps && fabsf(q.y - p.y) < eps &&
                fabsf(q.z + p.z) < eps) t = 2;
            else if (fabsf(q.x + p.x) < eps && fabsf(q.y - p.y) < eps &&
                     fabsf(q.z + p.z) < eps) t = 3;
        }
        if (t) { s_mirV[s_mirN] = i; s_mirT[s_mirN] = t; s_mirN++; }
    }
}
static void mirror_apply(int v) {
    if (v < 0) return;
    v3 p = vert_at(v);
    for (int i = 0; i < s_mirN; i++) {
        v3 t = p;
        if (s_mirT[i] & 1) t.x = -t.x;
        if (s_mirT[i] & 2) t.z = -t.z;
        vert_set(s_mirV[i], t);
    }
}

/* island (connected component) containing face fi -> s_part[] */
static void island_from_face(int fi) {
    static uint8_t in[SC_MAX_V];
    memset(in, 0, (size_t)s_nv);
    s_partN = 0;
    for (int k = 0; k < 3; k++) { in[s_fc[fi][k]] = 1; }
    for (int grow = 1; grow; ) {
        grow = 0;
        for (int f = 0; f < s_nf; f++) {
            int has = in[s_fc[f][0]] || in[s_fc[f][1]] || in[s_fc[f][2]];
            if (!has) continue;
            for (int k = 0; k < 3; k++)
                if (!in[s_fc[f][k]]) { in[s_fc[f][k]] = 1; grow = 1; }
        }
    }
    for (int i = 0; i < s_nv; i++) if (in[i]) s_part[s_partN++] = i;
}

/* island (connected-component) id per vert via union-find over faces.
   Islands are numbered in order of first appearance -> stable dot colors. */
static int island_ids(int *ids) {
    static int par[SC_MAX_V], remap[SC_MAX_V];
    for (int i = 0; i < s_nv; i++) { par[i] = i; remap[i] = -1; }
    for (int f = 0; f < s_nf; f++) {
        int a = s_fc[f][0];
        while (par[a] != a) a = par[a];
        for (int k = 1; k < 3; k++) {
            int b = s_fc[f][k];
            while (par[b] != b) b = par[b];
            par[b] = a;
        }
    }
    int ni = 0;
    for (int i = 0; i < s_nv; i++) {
        int r = i;
        while (par[r] != r) r = par[r];
        if (remap[r] < 0) remap[r] = ni++;
        ids[i] = remap[r];
    }
    return ni;
}

/* dot tint per island - disconnected objects read as separate pieces */
static const float ISLCOL[8][3] = {
    {1.00f,0.32f,0.32f}, {0.30f,0.85f,1.00f}, {1.00f,0.85f,0.30f},
    {0.95f,0.45f,1.00f}, {0.40f,1.00f,0.50f}, {1.00f,0.60f,0.25f},
    {0.65f,0.55f,1.00f}, {0.45f,1.00f,0.85f}
};

/* the verts material buttons act on: the PART-tool island, else all */
static int mat_target(int *out) {
    if (s_tool == 2 && s_partN > 0) {
        memcpy(out, s_part, (size_t)s_partN * sizeof(int));
        return s_partN;
    }
    for (int i = 0; i < s_nv; i++) out[i] = i;
    return s_nv;
}
/* apply color to the target set, skipping locked material layers */
static int mat_apply_col(const float c[3]) {
    static int tg[SC_MAX_V];
    int n = mat_target(tg), skip = 0;
    for (int i = 0; i < n; i++)
        if (s_vlock[tg[i]]) skip++; else vert_col(tg[i], c);
    return skip;
}
static int mat_apply_tex(int8_t t) {
    static int tg[SC_MAX_V];
    int n = mat_target(tg), skip = 0;
    for (int i = 0; i < n; i++)
        if (s_vlock[tg[i]]) skip++; else s_vtex[tg[i]] = t;
    return skip;
}
/* current layer of the target set (first vert), for the button label */
static int mat_cur_tex(void) {
    static int tg[SC_MAX_V];
    int n = mat_target(tg);
    return n > 0 ? s_vtex[tg[0]] : -1;
}
static int mat_cur_lock(void) {
    static int tg[SC_MAX_V];
    int n = mat_target(tg);
    return n > 0 && s_vlock[tg[0]];
}

/* first face hit by the ray, or -1 */
static int pick_face(v3 ro, v3 rd) {
    int best = -1; float bt = 1e30f;
    for (int f = 0; f < s_nf; f++) {
        v3 a = vert_at(s_fc[f][0]), b = vert_at(s_fc[f][1]), c = vert_at(s_fc[f][2]);
        float t = seg_hit_tri(ro, v3_add(ro, v3_scale(rd, 300.0f)), a, b, c);
        if (t >= 0.0f && t < bt) { bt = t; best = f; }
    }
    return best;
}

/* remove verts no ray can reach (fully enclosed by other faces) */
static void cull_buried(void) {
    if (s_nv < 12) return;
    float maxd = 0;
    for (int i = 0; i < s_nv; i++)
        for (int j = i + 1; j < s_nv; j++) {
            float d = v3_dist(vert_at(i), vert_at(j));
            if (d > maxd) maxd = d;
        }
    maxd = maxd * 1.5f + 1.0f;
    static const v3 AX[6] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    int killed = 0;
    for (int v = s_nv - 1; v >= 0; v--) {
        v3 p = vert_at(v);
        int buried = 1;
        for (int d = 0; d < 6 && buried; d++) {
            v3 fp = v3_add(p, v3_scale(AX[d], maxd));
            int hit = 0;
            for (int f = 0; f < s_nf && !hit; f++) {
                if (s_fc[f][0] == v || s_fc[f][1] == v || s_fc[f][2] == v)
                    continue;
                if (seg_hit_tri(p, fp, vert_at(s_fc[f][0]),
                                vert_at(s_fc[f][1]), vert_at(s_fc[f][2])) >= 0.0f)
                    hit = 1;
            }
            if (!hit) buried = 0;
        }
        if (buried) { delete_vert(v); killed++; }
    }
    if (killed) toastf("CULLED %d BURIED VERTS", killed);
}

/* append a tri-soup as a separate island at `off` (weld-adds verts) */
static int append_topo(const Vert *soup, int n, v3 off) {
    int added = 0;
    for (int i = 0; i + 2 < n; i += 3) {
        uint16_t ix[3]; int ok = 1;
        for (int k = 0; k < 3 && ok; k++) {
            const Vert *s = &soup[i + k];
            v3 p = v3_make(s->pos[0] + off.x, s->pos[1] + off.y,
                           s->pos[2] + off.z);
            int f = -1;
            for (int j = 0; j < s_nv; j++) {
                if (fabsf(s_vx[j][0]-p.x) < 1e-4f && fabsf(s_vx[j][1]-p.y) < 1e-4f &&
                    fabsf(s_vx[j][2]-p.z) < 1e-4f) { f = j; break; }
            }
            if (f < 0) {
                if (s_nv >= SC_MAX_V) { ok = 0; break; }
                f = s_nv++;
                vert_set(f, p);
                for (int c = 0; c < 3; c++)
                    s_vc[f][c] = (uint8_t)(s->col[c] * 255.0f);
                s_vtex[f] = (int8_t)s->tex;
                s_vlock[f] = 0;
                added++;
            }
            ix[k] = (uint16_t)f;
        }
        if (!ok || s_nf >= SC_MAX_F) break;
        if (ix[0]==ix[1] || ix[1]==ix[2] || ix[0]==ix[2]) continue;
        push_f(ix[0], ix[1], ix[2]);
    }
    return added;
}

/* paste clipboard blocks (or a model) as a new island beside the mesh */
static void paste_clip(void) {
    if (A.nClip < 1) { toast("NOTHING COPIED (CTRL+C FIRST)"); return; }
    /* spawn point: just right of the current mesh bounds */
    v3 hi = v3_make(-1e9f,-1e9f,-1e9f), lo = v3_make(1e9f,1e9f,1e9f);
    for (int i = 0; i < s_nv; i++) for (int a = 0; a < 3; a++) {
        if (s_vx[i][a] > hi.e[a]) hi.e[a] = s_vx[i][a];
        if (s_vx[i][a] < lo.e[a]) lo.e[a] = s_vx[i][a];
    }
    v3 spawn = s_nv ? v3_make(hi.x + 1.5f, SC_LIFT, (lo.z + hi.z) * 0.5f)
                    : v3_make(0, SC_LIFT, 0);
    /* clip AABB center as local origin */
    v3 clo = v3_make(1e9f,1e9f,1e9f), chi = v3_make(-1e9f,-1e9f,-1e9f);
    for (int i = 0; i < A.nClip; i++) {
        for (int a = 0; a < 3; a++) {
            float l = A.clip[i].pos.e[a] - A.clip[i].size.e[a] * 0.5f;
            float h2 = A.clip[i].pos.e[a] + A.clip[i].size.e[a] * 0.5f;
            if (l < clo.e[a]) clo.e[a] = l;
            if (h2 > chi.e[a]) chi.e[a] = h2;
        }
    }
    v3 cctr = v3_scale(v3_add(clo, chi), 0.5f);
    int added = 0;
    static Vert soup[4096];
    for (int i = 0; i < A.nClip; i++) {
        Block *b = &A.clip[i];
        v3 base = v3_sub(b->pos, cctr);
        if (b->shape >= RK_MESH_BASE_MODEL) {
            int mid = b->shape - RK_MESH_BASE_MODEL;
            if (mid < g_nmodels && g_models[mid].vox) {
                Model *m = &g_models[mid];
                v3 sc = v3_make(b->size.x/m->size.x, b->size.y/m->size.y,
                                b->size.z/m->size.z);
                int ns = 0;
                for (int f = 0; f < m->nface && ns + 3 <= 4096; f++) {
                    for (int k = 0; k < 3; k++) {
                        Vert *v = &soup[ns++];
                        int vi = m->mf[f][k];
                        v->pos[0] = m->mv[vi][0]*sc.x + base.x + spawn.x;
                        v->pos[1] = m->mv[vi][1]*sc.y + base.y + spawn.y;
                        v->pos[2] = m->mv[vi][2]*sc.z + base.z + spawn.z;
                        for (int c = 0; c < 3; c++)
                            v->col[c] = m->mvcol[vi][c]/255.0f;
                    }
                }
                added += append_topo(soup, ns, v3_make(0,0,0));
                continue;
            }
        }
        /* generic block -> unit shape baked then welded in */
        int tn = rk_gen_shape(b->shape, soup, 4096);
        for (int k = 0; k < tn; k++) {
            soup[k].pos[0] = soup[k].pos[0]*b->size.x + base.x + spawn.x;
            soup[k].pos[1] = soup[k].pos[1]*b->size.y + base.y + spawn.y;
            soup[k].pos[2] = soup[k].pos[2]*b->size.z + base.z + spawn.z;
            memcpy(soup[k].col, b->col, 12);
        }
        added += append_topo(soup, tn, v3_make(0,0,0));
    }
    s_partN = -1;
    cull_degenerate();
    toastf("PASTED %d VERTS", added);
}

/* ---- OBJ import (Wavefront: v + f, polygons fan-triangulated) ---- */
static int import_obj(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { toast("IMPORT FAILED - FILE NOT FOUND"); return 0; }
    static float (*tv)[3] = NULL; static int tcap = 0;
    if (!tv) { tcap = 1 << 18; tv = malloc((size_t)tcap * sizeof *tv); }
    if (!tv) { fclose(f); return 0; }
    int ntv = 0;
    static Vert soup[SC_MAX_F * 3];
    int ns = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            if (ntv < tcap)
                sscanf(line + 2, "%f %f %f", &tv[ntv][0], &tv[ntv][1],
                       &tv[ntv][2]), ntv++;
        } else if (line[0] == 'f' && line[1] == ' ') {
            int idx[64], n = 0;
            char *tok = strtok(line + 2, " \t\r\n");
            while (tok && n < 64) {
                int vi = atoi(tok);
                if (vi < 0) vi = ntv + vi; else vi--;
                if (vi >= 0 && vi < ntv) idx[n++] = vi;
                tok = strtok(NULL, " \t\r\n");
            }
            for (int k = 1; k + 1 < n && ns + 3 <= SC_MAX_F * 3; k++) {
                int tri[3] = {idx[0], idx[k], idx[k+1]};
                for (int j = 0; j < 3; j++) {
                    Vert *v = &soup[ns++];
                    memcpy(v->pos, tv[tri[j]], 12);
                    memcpy(v->col, A.paintCol, 12);
                }
            }
        }
    }
    fclose(f);
    if (!ns) { toast("IMPORT FAILED - NO FACES"); return 0; }
    /* normalize: recenter on AABB, uniform-fit to ~1.6 units */
    v3 lo = v3_make(1e9f,1e9f,1e9f), hi = v3_make(-1e9f,-1e9f,-1e9f);
    for (int i = 0; i < ns; i++) for (int a = 0; a < 3; a++) {
        if (soup[i].pos[a] < lo.e[a]) lo.e[a] = soup[i].pos[a];
        if (soup[i].pos[a] > hi.e[a]) hi.e[a] = soup[i].pos[a];
    }
    v3 c = v3_scale(v3_add(lo,hi), 0.5f);
    float mx = fmaxf(fmaxf(hi.x-lo.x, hi.y-lo.y), hi.z-lo.z);
    float s = mx > 1e-6f ? 1.6f / mx : 1.0f;
    s_nv = s_nf = 0; s_selV = s_dragV = -1; s_partN = -1;
    for (int i = 0; i < ns; i++) {
        soup[i].pos[0] = (soup[i].pos[0]-c.x)*s;
        soup[i].pos[1] = (soup[i].pos[1]-c.y)*s + SC_LIFT;
        soup[i].pos[2] = (soup[i].pos[2]-c.z)*s;
    }
    int added = append_topo(soup, ns, v3_make(0,0,0));
    /* decimate if the cap overflowed: weld at growing eps until it fits */
    for (int it = 0; it < 3 && s_nv >= SC_MAX_V - 1; it++) {
        s_nv = s_nf = 0;
        float e = 0.02f * (1 << it);
        for (int i = 0; i + 2 < ns; i += 3) {
            uint16_t ix[3]; int ok = 1;
            for (int k = 0; k < 3; k++) {
                v3 p = v3_make(soup[i+k].pos[0], soup[i+k].pos[1],
                               soup[i+k].pos[2]);
                int f2 = -1;
                for (int j = 0; j < s_nv; j++)
                    if (v3_dist(vert_at(j), p) < e) { f2 = j; break; }
                if (f2 < 0 && s_nv < SC_MAX_V) { f2 = s_nv++; vert_set(f2, p);
                    for (int c2 = 0; c2 < 3; c2++)
                        s_vc[f2][c2] = (uint8_t)(soup[i+k].col[c2]*255.0f);
                    s_vtex[f2] = (int8_t)soup[i+k].tex; s_vlock[f2] = 0; }
                if (f2 < 0) { ok = 0; break; }
                ix[k] = (uint16_t)f2;
            }
            if (ok && ix[0]!=ix[1] && ix[1]!=ix[2] && ix[0]!=ix[2] &&
                s_nf < SC_MAX_F)
                push_f(ix[0], ix[1], ix[2]);
        }
        (void)added;
    }
    merge_verts();
    cull_degenerate();
    toastf("IMPORTED %d VERTS", s_nv);
    return 1;
}

/* OS file picker -> OBJ import */
static void sculpt_import(void) {
#ifdef _WIN32
    char path[512] = {0};
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = glfwGetWin32Window(A.win);
    ofn.lpstrFile = path; ofn.nMaxFile = sizeof path;
    ofn.lpstrFilter = "3D Models\0*.obj\0All Files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "Import model (.obj)";
    if (!GetOpenFileNameA(&ofn)) return;
    import_obj(path);
#else
    /* Linux: try zenity's picker; else look for import.obj next to the exe */
    char path[512] = {0};
    FILE *pp = popen("zenity --file-selection "
                     "--file-filter='3D models (obj) | *.obj' 2>/dev/null", "r");
    if (pp) {
        if (!fgets(path, sizeof path, pp)) path[0] = 0;
        if (pclose(pp) != 0) path[0] = 0;
        path[strcspn(path, "\r\n")] = 0;
    }
    if (path[0]) { import_obj(path); return; }
    if (import_obj("import.obj")) return;
    toast("DROP import.obj NEXT TO THE EXE");
#endif
}

/* ---------- modal plumbing ---------- */

/* bake current topology into a custom model (creates or edits in place) */
static void sculpt_finish(void) {
    if (s_nv >= 3 && s_nf >= 1) {
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
                memcpy(m->mvtex, s_vtex, s_nv);
                memcpy(m->mvlock, s_vlock, s_nv);
                memcpy(m->mf, s_fc, s_nf * sizeof s_fc[0]);
                v3 sz = v3_sub(hi, lo);
                m->size = v3_make(fmaxf(sz.x,0.125f), fmaxf(sz.y,0.125f),
                                  fmaxf(sz.z,0.125f));
                snprintf(m->name, sizeof m->name, "%.11s", s_name);
                models_rebake_all();
                models_save(NULL);
            }
        } else if (model_sculpt_topo(s_vx, s_vc, s_vtex, s_vlock,
                                     s_fc, s_nv, s_nf, s_name) >= 0) {
            rk_models_commit();
            models_save(NULL);
        }
    }
    s_selV = s_dragV = -1; s_editMid = -1; s_naming = 0;
    A.modal = 0;
}

static void sculpt_leave(void) {           /* discard + close */
    s_selV = s_dragV = -1; s_editMid = -1; s_naming = 0;
    A.modal = 0;
}

/* open the name+confirm dialog (DONE/EXIT/ENTER all funnel here) */
static void sculpt_ask_name(void) {
    if (s_nv < 3 || s_nf < 1) { sculpt_leave(); return; }
    s_naming = 1;
    if (s_editMid >= 0 && s_editMid < g_nmodels)
        snprintf(s_name, sizeof s_name, "%.11s", g_models[s_editMid].name);
    else s_name[0] = 0;
}

static void sculpt_enter(void) {
    s_selV = s_dragV = -1; s_partN = -1; s_naming = 0; s_toast[0] = 0;
    A.modal = 2; A.drag = M_IDLE; A.armedAsset = -1;
    sel_clear();
    A.camPos = v3_make(2.6f, SC_LIFT + 1.2f, 2.6f);
    A.yaw = 0.785398f; A.pitch = -0.38f;
}

void sculpt_open(void) {
    s_editMid = -1;
    init_preset(s_start);
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
    memcpy(s_vtex, m->mvtex, s_nv);
    memcpy(s_vlock, m->mvlock, s_nv);
    memcpy(s_fc, m->mf, s_nf * sizeof s_fc[0]);
    /* verts are centered on the AABB - float them at the sculpt height */
    for (int i = 0; i < s_nv; i++) s_vx[i][1] += SC_LIFT;
    s_editMid = mid;
    sculpt_enter();
}

/* ---------- sculpting sidebar (replaces the build panel) ---------- */

#define SB_W 240.0f                      /* sidebar width                  */
#define SB_H 30.0f                       /* button row height              */
static float sbx(void) { return (float)A.fbw - SB_W; }

/* rows inside the sidebar (30px tall; group labels ride 14px above) */
enum {
    SROW_TOOL,      /* MOVE | ADDV | PART (thirds)                      */
    SROW_EDIT,      /* DEL V | SMOOTH | WELD (thirds)                   */
    SROW_DIV,       /* DOTS+n (halves) | SUBDIV                         */
    SROW_SNAP,      /* 0.125 | 0.25 | 0.5 (thirds)                      */
    SROW_MIRROR,    /* OFF | X | 4-WAY (thirds)                         */
    SROW_START,     /* START: name (full, opens shape picker)           */
    SROW_INS,       /* PASTE | IMPORT | GRID (thirds)                   */
    SROW_MAT,       /* TEX: name (2/3) | LOCK (1/3) - material layer    */
    SROW_RESET,     /* RESET                                            */
    SROW_DONE,      /* DONE                                             */
    SROW_EXIT,      /* EXIT                                             */
};
static float srow_y(int r) {
    static const float Y[] = {48, 104, 160, 216, 272, 328, 384,
                              548, 606, 646, 686};
    return Y[r];
}
static int sbtn_hit(float x, float y, int r) {
    return x >= sbx() + 8 && x < sbx() + SB_W - 8 &&
           y >= srow_y(r) && y < srow_y(r) + SB_H;
}
/* 0/1/2 index of the third-row button at x, or -1 */
static int row3_hit(float x) {
    float w3 = (SB_W - 16 - 8) / 3;
    for (int i = 0; i < 3; i++)
        if (x >= sbx() + 8 + i * (w3 + 4) && x < sbx() + 8 + i * (w3 + 4) + w3)
            return i;
    return -1;
}
static int half_hit(float x) {                 /* 0 = left half, 1 = right */
    return x < sbx() + 8 + (SB_W - 16) * 0.5f ? 0 : 1;
}

/* naming dialog button rects (shared by press + draw) */
static void naming_btns(float *bx, float *bw) {
    float cx = ((float)A.fbw - 340.0f) * 0.5f;
    bx[0] = cx + 16;  bw[0] = 92;              /* SAVE   */
    bx[1] = cx + 118; bw[1] = 118;             /* EDIT   */
    bx[2] = cx + 246; bw[2] = 78;              /* DISCARD*/
}
static float naming_by(void) { return (float)A.fbh * 0.5f + 28.0f; }

/* delete vertex + its mirror partners (descending idx = safe) */
static void del_sel_mirrored(void) {
    if (s_selV < 0) return;
    int kill[4]; int nk = 1;
    kill[0] = s_selV;
    nk += mirror_partners(s_selV, kill + 1);
    for (int i = 0; i < nk; i++)               /* insertion sort desc */
        for (int j = i + 1; j < nk; j++)
            if (kill[j] > kill[i]) { int t = kill[i]; kill[i] = kill[j]; kill[j] = t; }
    for (int i = 0; i < nk; i++) delete_vert(kill[i]);
}

/* shape-picker cell rects (shared by press + draw): 2 cols x 3 rows */
static void picker_cell(int i, float *x, float *y) {
    float x0 = ((float)A.fbw - 260.0f) * 0.5f;
    float y0 = ((float)A.fbh - 400.0f) * 0.5f;
    *x = x0 + 12 + (i % 2) * 122;
    *y = y0 + 48 + (i / 2) * 112;
}
#define PICK_CELL_W 110.0f

int sculpt_press(int mx, int my) {
    float x = (float)mx, y = (float)my;
    /* start-shape picker popup */
    if (s_shapePick) {
        for (int i = 0; i < 6; i++) {
            float px, py; picker_cell(i, &px, &py);
            if (x >= px && x < px + PICK_CELL_W && y >= py && y < py + 106) {
                s_start = i; init_preset(s_start); s_shapePick = 0;
                snprintf(s_toast, sizeof s_toast, "START: %s",
                         START_NAMES[s_start]);
                s_toastT = glfwGetTime();
                return 1;
            }
        }
        s_shapePick = 0;                     /* click outside = close */
        return 1;
    }
    /* naming dialog captures every click */
    if (s_naming) {
        float bx[3], bw[3]; naming_btns(bx, bw);
        float by = naming_by();
        for (int i = 0; i < 3; i++)
            if (x >= bx[i] && x < bx[i] + bw[i] && y >= by && y < by + 24) {
                if (i == 0) { if (!s_name[0]) snprintf(s_name, sizeof s_name,
                                                       "CUSTOM");
                              sculpt_finish(); }
                else if (i == 1) s_naming = 0;
                else sculpt_leave();
                return 1;
            }
        return 1;                            /* swallow all other clicks */
    }
    /* sidebar swallow + controls */
    if (x >= sbx()) {
        if (sbtn_hit(x, y, SROW_TOOL)) {
            int h = row3_hit(x); if (h >= 0) s_tool = h;
        } else if (sbtn_hit(x, y, SROW_EDIT)) {
            int h = row3_hit(x);
            if (h == 0) del_sel_mirrored();
            else if (h == 1) smooth_mesh();
            else if (h == 2) { merge_verts_e(s_snap * 0.75f); toast("WELDED"); }
        } else if (sbtn_hit(x, y, SROW_DIV)) {
            if (half_hit(x) == 0) { s_density = (s_density + 1) % 4;
                                    toastf("DOTS +%d", s_density); }
            else subdivide();
        } else if (sbtn_hit(x, y, SROW_SNAP)) {
            int h = row3_hit(x);
            if (h >= 0) s_snap = h == 0 ? 0.125f : (h == 1 ? 0.25f : 0.5f);
        } else if (sbtn_hit(x, y, SROW_MIRROR)) {
            int h = row3_hit(x); if (h >= 0) s_mirror = h;
        } else if (sbtn_hit(x, y, SROW_START)) {
            s_shapePick = 1;                 /* visual shape picker */
        } else if (sbtn_hit(x, y, SROW_INS)) {
            int h = row3_hit(x);
            if (h == 0) paste_clip();
            else if (h == 1) sculpt_import();
            else if (h == 2) s_showGrid ^= 1;
        } else if (sbtn_hit(x, y, SROW_MAT)) {
            /* left ~2/3 cycles the texture layer; right third toggles lock */
            float third = sbx() + 8 + (SB_W - 16) * 0.66f;
            if (x < third) {
                int cur = mat_cur_tex();
                int nt = cur < 0 ? 1 : (cur + 1 >= g_ntex ? -1 : cur + 1);
                int skip = mat_apply_tex((int8_t)nt);
                if (skip) toast("LAYER LOCKED");
                else snprintf(s_toast, sizeof s_toast, "TEX: %s",
                              nt < 0 ? "NONE" : g_tex[nt].name),
                     s_toastT = glfwGetTime();
            } else {
                static int tg[SC_MAX_V];
                int n = mat_target(tg);
                int lock = !mat_cur_lock();
                for (int i = 0; i < n; i++) s_vlock[tg[i]] = (uint8_t)lock;
                toast(lock ? "LAYER LOCKED" : "LAYER UNLOCKED");
            }
        } else if (sbtn_hit(x, y, SROW_RESET)) init_preset(s_start);
        else if (sbtn_hit(x, y, SROW_DONE) || sbtn_hit(x, y, SROW_EXIT))
            sculpt_ask_name();               /* exiting applies the mesh */
        else {
            /* palette grid 4x4 below the insert row */
            float gx = sbx() + 10, gy = srow_y(SROW_INS) + 44;
            for (int i = 0; i < 16; i++) {
                float px = gx + (i % 4) * 55.0f, py = gy + (i / 4) * 26.0f;
                if (x >= px && x < px + 50 && y >= py && y < py + 24) {
                    const float *c = panel_color(i);
                    memcpy(A.paintCol, c, sizeof(float)*3);
                    if (s_selV >= 0 && !(s_tool == 2 && s_partN > 0)) {
                        if (!s_vlock[s_selV]) vert_col(s_selV, c);
                        else toast("LAYER LOCKED");
                    } else if (mat_apply_col(c))
                        toast("PARTLY LOCKED");
                    break;
                }
            }
        }
        return 1;
    }
    /* LMB action depends on the active tool */
    if (s_tool == 1) { edge_split_at(mx, my); return 1; }
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    if (s_tool == 2) {
        /* PART: click a face -> grab the whole island */
        int fi = pick_face(ro, rd);
        if (fi < 0) { s_partN = -1; return 1; }
        island_from_face(fi);
        for (int i = 0; i < s_partN; i++) {
            v3 p = vert_at(s_part[i]);
            memcpy(s_partBase[i], p.e, 12);
        }
        v3 a = vert_at(s_fc[fi][0]), b = vert_at(s_fc[fi][1]),
           c = vert_at(s_fc[fi][2]);
        float t = seg_hit_tri(ro, v3_add(ro, v3_scale(rd, 300.0f)), a, b, c);
        s_partAnchor = v3_add(ro, v3_scale(rd, t));
        s_dragPt = s_partAnchor;
        s_dragN = cam_basis().f;
        s_dragV = -2;                        /* sentinel: island drag */
        s_mirN = 0;
        s_selV = -1;
        return 1;
    }
    /* TOOL move: vertex pick -> drag on camera-parallel plane */
    int v = pick_vert(ro, rd);
    s_selV = v;
    if (v >= 0) {
        s_dragV = v;
        s_dragN = cam_basis().f;
        s_dragPt = vert_at(v);
        s_dragFrom = s_dragPt;
        mirror_grab(v);                      /* record partners once */
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
    if (s_dragV == -2) {                     /* island drag ended */
        merge_verts(); resolve_intersections(); cull_degenerate(); cull_buried();
    } else if (s_dragV >= 0) {
        merge_verts(); resolve_intersections(); cull_degenerate(); cull_buried();
    }
    s_dragV = -1;
}

/* segment p0->p1 vs triangle: t of first hit in (0,1) or -1 (Moller) */
static float seg_hit_tri(v3 p0, v3 p1, v3 a, v3 b, v3 c) {
    v3 e1 = v3_sub(b,a), e2 = v3_sub(c,a);
    v3 dir = v3_sub(p1,p0);
    v3 pv = v3_cross(dir, e2);
    float det = v3_dot(e1, pv);
    if (fabsf(det) < 1e-9f) return -1.0f;          /* parallel */
    float inv = 1.0f/det;
    v3 tv = v3_sub(p0,a);
    float u = v3_dot(tv,pv)*inv;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    v3 qv = v3_cross(tv, e1);
    float v = v3_dot(dir,qv)*inv;
    if (v < 0.0f || u+v > 1.0f) return -1.0f;
    float t = v3_dot(e2,qv)*inv;
    return (t > 1e-4f && t < 1.0f) ? t : -1.0f;
}

void sculpt_move(int mx, int my) {
    A.mx = mx; A.my = my;
    if (s_dragV < 0 && s_dragV != -2) return;
    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);
    float denom = v3_dot(s_dragN, rd);
    if (fabsf(denom) < 1e-6f) return;
    float t = v3_dot(s_dragN, v3_sub(s_dragPt, ro)) / denom;
    if (s_dragV == -2) {                          /* whole-island drag */
        v3 d = v3_sub(v3_add(ro, v3_scale(rd, t)), s_dragPt);
        snapv(&d);
        float mny = 1e9f;
        for (int i = 0; i < s_partN; i++) {
            float y2 = s_partBase[i][1] + d.y;
            if (y2 < mny) mny = y2;
        }
        if (mny < 0.02f) d.y += 0.02f - mny;      /* keep above ground */
        for (int i = 0; i < s_partN; i++)
            vert_set(s_part[i], v3_make(s_partBase[i][0] + d.x,
                                        s_partBase[i][1] + d.y,
                                        s_partBase[i][2] + d.z));
        return;
    }
    v3 p = v3_sub(v3_add(ro, v3_scale(rd, t)), s_dragOff);
    snapv(&p);
    /* don't let the vertex tunnel through faces it isn't part of:
       clamp to just before the earliest crossed triangle */
    v3 cur = vert_at(s_dragV);
    float tb = 1.0f;
    for (int f = 0; f < s_nf; f++) {
        if (s_fc[f][0] == s_dragV || s_fc[f][1] == s_dragV ||
            s_fc[f][2] == s_dragV) continue;
        float th = seg_hit_tri(cur, p, vert_at(s_fc[f][0]),
                               vert_at(s_fc[f][1]), vert_at(s_fc[f][2]));
        if (th >= 0.0f && th < tb) tb = th;
    }
    if (tb < 1.0f) {
        float tt = tb - 0.02f; if (tt < 0.0f) tt = 0.0f;
        p = v3_add(cur, v3_scale(v3_sub(p, cur), tt));
    }
    if (p.y < 0.02f) p.y = 0.02f;                  /* never underground */
    vert_set(s_dragV, p);
    mirror_apply(s_dragV);
}

void sculpt_wheel(double dy) {
    cam_zoom_cursor((float)dy, A.mx, A.my);
}

int sculpt_key(int k) {
    if (s_naming) {
        if (k == GLFW_KEY_BACKSPACE) {
            int n = (int)strlen(s_name); if (n) s_name[n-1] = 0;
        } else if (k == GLFW_KEY_ENTER) {
            if (!s_name[0]) snprintf(s_name, sizeof s_name, "CUSTOM");
            sculpt_finish();
        } else if (k == GLFW_KEY_ESCAPE) s_naming = 0;
        return 1;
    }
    if (k == GLFW_KEY_ESCAPE) {
        if (s_selV >= 0 || s_dragV >= 0) { s_selV = s_dragV = -1; return 1; }
        sculpt_ask_name(); return 1;         /* exiting applies the mesh */
    }
    if (k == GLFW_KEY_ENTER)  { sculpt_ask_name(); return 1; }
    if (k == GLFW_KEY_DELETE || k == GLFW_KEY_BACKSPACE) {
        del_sel_mirrored();
        return 1;
    }
    return 1;
}

/* name-field typing (called from glfw char callback while modal==2) */
void sculpt_char(unsigned cp) {
    if (!s_naming || cp < 32 || cp >= 127) return;
    int n = (int)strlen(s_name);
    if (n < 15) { s_name[n] = (char)cp; s_name[n+1] = 0; }
}

/* ---------- draw ---------- */

/* count how many faces use each unique edge (1 = boundary/hole edge) */
static int edge_face_count(int a, int b) {
    int n = 0;
    for (int f = 0; f < s_nf; f++) {
        uint16_t *x = s_fc[f];
        int ha = (x[0]==a||x[1]==a||x[2]==a), hb = (x[0]==b||x[1]==b||x[2]==b);
        if (ha && hb) n++;
    }
    return n;
}

static void sb_btn(float x, float y, float w, const char *label,
                   int active, int accent) {
    int hov = A.mx >= x && A.mx < x + w && A.my >= y && A.my < y + SB_H;
    float r = accent ? 0.25f : (active ? 0.32f : 0.15f) + hov*0.05f;
    float g = accent ? 0.45f : (active ? 0.48f : 0.17f) + hov*0.05f;
    float b = accent ? 0.30f : (active ? 0.72f : 0.22f) + hov*0.05f;
    rk_ui_rect(x, y, w, SB_H, r, g, b, 1);
    int sc = ui_text_w(label, 2) <= (int)w - 6 ? 2 : 1;
    ui_text(x + (w - ui_text_w(label, sc)) * 0.5f, y + (SB_H - 8*sc) * 0.5f,
            label, sc, 1, 1, 1, 1);
}

void sculpt_draw(void) {
    m4 idm; m4_identity(idm);

    /* mesh AABB + centroid (lattice bounds, line puffing) */
    v3 lo = v3_make(1e9f,1e9f,1e9f), hi = v3_make(-1e9f,-1e9f,-1e9f);
    v3 gc = v3_make(0,0,0);
    for (int i = 0; i < s_nv; i++) {
        for (int a = 0; a < 3; a++) {
            if (s_vx[i][a] < lo.e[a]) lo.e[a] = s_vx[i][a];
            if (s_vx[i][a] > hi.e[a]) hi.e[a] = s_vx[i][a];
        }
        gc = v3_add(gc, vert_at(i));
    }
    if (s_nv) gc = v3_scale(gc, 1.0f / s_nv);

    /* faint snap lattice: where dots will land (3 mid-planes thru the AABB,
       or just the XZ plane if that's already too many points) */
    if (s_showGrid && s_nv) {
        float m = s_snap * 1.01f;
        int nx = (int)((hi.x - lo.x + 2*m) / s_snap) + 1;
        int ny = (int)((hi.y - lo.y + 2*m) / s_snap) + 1;
        int nz = (int)((hi.z - lo.z + 2*m) / s_snap) + 1;
        int planes = (nx*ny + nx*nz + ny*nz <= 1600) ? 7 : 1;
        static RkInst pts[1700];
        int pn = 0;
        v3 c = v3_scale(v3_add(lo, hi), 0.5f);
        float sz = s_snap * 0.07f;
        for (int ix = 0; ix < nx && pn < 1650; ix++)
            for (int iy = 0; iy < ny && pn < 1650; iy++)
                for (int iz = 0; iz < nz && pn < 1650; iz++) {
                    v3 p = v3_make(lo.x - m + ix*s_snap,
                                   lo.y - m + iy*s_snap,
                                   lo.z - m + iz*s_snap);
                    int on = ((fabsf(p.z - c.z) < s_snap*0.5f) ? 1 : 0) |
                             ((fabsf(p.y - c.y) < s_snap*0.5f) ? 2 : 0) |
                             ((fabsf(p.x - c.x) < s_snap*0.5f) ? 4 : 0);
                    if (!(on & planes)) continue;
                    pts[pn].pos[0]=p.x; pts[pn].pos[1]=p.y; pts[pn].pos[2]=p.z;
                    pts[pn].scale[0]=pts[pn].scale[1]=pts[pn].scale[2]=sz;
                    pts[pn].color = rk_rgba(0.55f,0.7f,0.9f,0.13f);
                    pts[pn].flags = RKIF_UNLIT | RKIF_NOFOG;
                    pn++;
                }
        if (pn) rk_ghost(RK_MESH_CUBE, pts, (uint32_t)pn);
    }

    /* solid mesh: flat normals, per-vertex colors */
    static Vert tbuf[SC_MAX_F * 3];
    int tn = 0;
    for (int f = 0; f < s_nf && tn + 3 <= SC_MAX_F * 3; f++) {
        uint16_t *t = s_fc[f];
        if (t[0] >= s_nv || t[1] >= s_nv || t[2] >= s_nv) continue;
        v3 a = vert_at(t[0]), b = vert_at(t[1]), c = vert_at(t[2]);
        v3 cr = v3_cross(v3_sub(b,a), v3_sub(c,a));
        if (v3_dot(cr, cr) < 1e-12f) continue;    /* degenerate -> no NaN */
        v3 n = v3_norm(cr);
        for (int k = 0; k < 3; k++) {
            v3 p = k == 0 ? a : (k == 1 ? b : c);
            Vert *v = &tbuf[tn++];
            memcpy(v->pos, p.e, 12);
            memcpy(v->nrm, n.e, 12);
            int vi = s_fc[f][k];
            v->col[0] = s_vc[vi][0]/255.0f; v->col[1] = s_vc[vi][1]/255.0f;
            v->col[2] = s_vc[vi][2]/255.0f;
            v->tex = (float)s_vtex[s_fc[f][0]];   /* face's material layer */
        }
    }
    if (tn) rk_mesh(tbuf, (uint32_t)tn, idm);

    /* wireframe: the mesh's own edges in white - "part borders", not a box.
       edges shared by exactly one face are mesh borders/holes -> orange,
       so unintended gaps are visible immediately */
    static Vert lbuf[SC_MAX_F * 6];
    static Vert bbuf[SC_MAX_F * 6];
    static int edges[SC_MAX_F * 3][2];
    int ne = get_edges(edges, SC_MAX_F * 3);
    int ln = 0, bn = 0;
    for (int i = 0; i < ne; i++) {
        /* puff edges 0.6% off the centroid -> lines float clear of the fill */
        v3 a = v3_add(gc, v3_scale(v3_sub(vert_at(edges[i][0]), gc), 1.006f));
        v3 b = v3_add(gc, v3_scale(v3_sub(vert_at(edges[i][1]), gc), 1.006f));
        Vert *dst = (edge_face_count(edges[i][0], edges[i][1]) == 1)
                    ? bbuf : lbuf;
        int *cnt = dst == bbuf ? &bn : &ln;
        memcpy(dst[*cnt].pos, a.e, 12);
        memcpy(dst[*cnt + 1].pos, b.e, 12);
        *cnt += 2;
    }
    if (ln) rk_meshlines(lbuf, (uint32_t)ln, idm, 1.0f, 1.0f, 1.0f, 0.9f);
    if (bn) rk_meshlines(bbuf, (uint32_t)bn, idm, 1.0f, 0.55f, 0.15f, 1.0f);

    /* PART tool: hover/drag island highlighted in cyan */
    if (s_tool == 2) {
        v3 ro2, rd2; cam_ray(A.mx, A.my, &ro2, &rd2);
        if (s_dragV != -2) {
            int fi = pick_face(ro2, rd2);
            if (fi >= 0) island_from_face(fi); else s_partN = -1;
        }
        if (s_partN > 0) {
            static uint8_t inm[SC_MAX_V];
            memset(inm, 0, (size_t)s_nv);
            for (int i = 0; i < s_partN; i++) inm[s_part[i]] = 1;
            static Vert ibuf[SC_MAX_F * 6];
            int in = 0;
            for (int i = 0; i < ne && in + 2 <= SC_MAX_F * 6; i++)
                if (inm[edges[i][0]] && inm[edges[i][1]]) {
                    memcpy(ibuf[in].pos,   vert_at(edges[i][0]).e, 12);
                    memcpy(ibuf[in+1].pos, vert_at(edges[i][1]).e, 12);
                    in += 2;
                }
            if (in) rk_meshlines(ibuf, (uint32_t)in, idm,
                                 0.2f, 0.9f, 1.0f, 0.95f);
        }
    }

    v3 ro, rd; cam_ray(A.mx, A.my, &ro, &rd);

    /* ADD tool: preview the vertex that a click would insert */
    if (s_tool == 1 && A.mx < sbx() && s_nv < SC_MAX_V) {
        v3 bp; int bi;
        if (nearest_edge(ro, rd, edges, ne, &bp, &bi)) {
            snapv(&bp);
            RkInst pv; memset(&pv, 0, sizeof pv);
            float d = v3_dist(bp, ro), r = d * 0.035f;
            v3 q = v3_add(bp, v3_scale(v3_norm(v3_sub(ro, bp)), r*0.6f));
            pv.pos[0]=q.x; pv.pos[1]=q.y; pv.pos[2]=q.z;
            pv.scale[0]=pv.scale[1]=pv.scale[2]=r;
            pv.color = rk_rgba(0.35f, 1.0f, 0.5f, 0.9f);
            pv.flags = RKIF_UNLIT | RKIF_NOFOG;
            rk_overlay(RK_MESH_CUBE, &pv, 1);
        }
    }

    /* drag indicators: reachable snap spots on the drag plane + delta
       line + landing marker so you can see where the dot will go */
    if (s_dragV >= 0 && s_dragV != -2) {
        v3 cur = vert_at(s_dragV);
        /* reachable spots: plane grid filtered to world-snap positions */
        v3 u = v3_cross(s_dragN, v3_make(0,1,0));
        u = v3_len(u) < 0.01f ? v3_make(1,0,0) : v3_norm(u);
        v3 w2 = v3_cross(s_dragN, u);
        static RkInst spots[200];
        int sn2 = 0;
        for (int i = -6; i <= 6; i++)
            for (int j = -6; j <= 6 && sn2 < 200; j++) {
                v3 q = v3_add(s_dragFrom,
                              v3_add(v3_scale(u, i * s_snap),
                                     v3_scale(w2, j * s_snap)));
                v3 q2 = q; snapv(&q2);
                if (v3_dist(q2, q) > s_snap * 0.3f) continue;  /* off-plane */
                if (q2.y < 0.02f) continue;
                spots[sn2].pos[0]=q2.x; spots[sn2].pos[1]=q2.y;
                spots[sn2].pos[2]=q2.z;
                float sz = s_snap * 0.09f;
                spots[sn2].scale[0]=spots[sn2].scale[1]=spots[sn2].scale[2]=sz;
                spots[sn2].color = rk_rgba(0.4f, 1.0f, 0.55f, 0.35f);
                spots[sn2].flags = RKIF_UNLIT | RKIF_NOFOG;
                sn2++;
            }
        if (sn2) rk_ghost(RK_MESH_CUBE, spots, (uint32_t)sn2);
        /* delta line grab->current */
        Vert dl[2];
        memcpy(dl[0].pos, s_dragFrom.e, 12);
        memcpy(dl[1].pos, cur.e, 12);
        rk_meshlines(dl, 2, idm, 1.0f, 0.9f, 0.3f, 0.9f);
        /* landing marker: bright green box at the snapped target */
        RkInst mk; memset(&mk, 0, sizeof mk);
        float d = v3_dist(cur, ro), r = d * 0.045f;
        mk.pos[0]=cur.x; mk.pos[1]=cur.y; mk.pos[2]=cur.z;
        mk.scale[0]=mk.scale[1]=mk.scale[2]=r;
        mk.color = rk_rgba(0.3f, 1.0f, 0.5f, 0.85f);
        mk.flags = RKIF_UNLIT | RKIF_NOFOG;
        rk_overlay(RK_MESH_CUBE, &mk, 1);
    }

    /* vertex dots: separate islands get separate colors */
    static RkInst dots[SC_MAX_V];
    static int isl[SC_MAX_V];
    int ni = island_ids(isl);
    int dn = 0;
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
        int sel = (i == s_selV), inP = 0;
        if (s_tool == 2 && s_partN > 0)
            for (int k = 0; k < s_partN; k++)
                if (s_part[k] == i) { inP = 1; break; }
        if (inP)      dots[dn].color = rk_rgba(0.3f, 0.9f, 1.0f, 1.0f);
        else if (sel) dots[dn].color = rk_rgba(1.0f, 1.0f, 1.0f, 1.0f);
        else if (ni > 1) {
            const float *ic = ISLCOL[isl[i] & 7];
            dots[dn].color = rk_rgba(ic[0], ic[1], ic[2], 1.0f);
        } else        dots[dn].color = rk_rgba(1.0f, 0.30f, 0.30f, 1.0f);
        dots[dn].flags = RKIF_UNLIT | RKIF_NOFOG;
        dn++;
    }
    if (dn) rk_overlay(RK_MESH_CUBE, dots, (uint32_t)dn);

    /* ---------- sculpting sidebar ---------- */
    rk_ui_rect(sbx(), 0, SB_W, (float)A.fbh, 0.10f, 0.11f, 0.14f, 0.94f);
    rk_ui_rect(sbx(), 0, 1.5f, (float)A.fbh, 0.25f, 0.28f, 0.35f, 1);
    ui_text(sbx() + 10, 12,
            s_editMid >= 0 ? "SCULPT - EDITING" : "SCULPT - NEW",
            1, 0.9f, 0.92f, 1.0f, 1);
    {
        float w3 = (SB_W - 16 - 8) / 3;
        float hw = (SB_W - 16) * 0.5f - 2;
        const char *tn[3] = {"MOVE", "ADDV", "PART"};
        const char *en[3] = {"DEL V", "SMOOTH", "WELD"};
        const char *sn[3] = {"0.125", "0.25", "0.5"};
        const float sv[3] = {0.125f, 0.25f, 0.5f};
        const char *mn[3] = {"OFF", "X", "4-WAY"};
        const char *in[3] = {"PASTE", "IMPORT", "GRID"};
        const char *lab[] = {"TOOLS", "EDIT", "DOT DENSITY", "SNAP",
                             "MIRROR", "START SHAPE", "INSERT / VIEW"};
        int srow[] = {SROW_TOOL, SROW_EDIT, SROW_DIV, SROW_SNAP,
                      SROW_MIRROR, SROW_START, SROW_INS};
        for (int i = 0; i < 7; i++)
            ui_text(sbx() + 10, srow_y(srow[i]) - 14, lab[i], 1,
                    0.55f, 0.6f, 0.7f, 1);
        for (int i = 0; i < 3; i++) {
            sb_btn(sbx() + 8 + i * (w3 + 4), srow_y(SROW_TOOL), w3,
                   tn[i], s_tool == i, 0);
            sb_btn(sbx() + 8 + i * (w3 + 4), srow_y(SROW_EDIT), w3,
                   en[i], 0, 0);
            sb_btn(sbx() + 8 + i * (w3 + 4), srow_y(SROW_SNAP), w3,
                   sn[i], s_snap == sv[i], 0);
            sb_btn(sbx() + 8 + i * (w3 + 4), srow_y(SROW_MIRROR), w3,
                   mn[i], s_mirror == i, 0);
            sb_btn(sbx() + 8 + i * (w3 + 4), srow_y(SROW_INS), w3,
                   in[i], i == 2 && s_showGrid, 0);
        }
        char lbl[32];
        snprintf(lbl, sizeof lbl, "DOTS +%d", s_density);
        sb_btn(sbx() + 8, srow_y(SROW_DIV), hw, lbl, s_density > 0, 0);
        sb_btn(sbx() + 12 + hw, srow_y(SROW_DIV), hw, "SUBDIV", 0, 0);
        snprintf(lbl, sizeof lbl, "SHAPE: %s", START_NAMES[s_start]);
        sb_btn(sbx() + 8, srow_y(SROW_START), SB_W - 16, lbl, 0, 0);
        /* palette 4x4 (compact pitch so the material row fits below) */
        ui_text(sbx() + 10, srow_y(SROW_INS) + 32, "COLOR", 1,
                0.55f, 0.6f, 0.7f, 1);
        /* current color swatch rides next to the label */
        rk_ui_rect(sbx() + 66, srow_y(SROW_INS) + 30, 64, 14,
                   A.paintCol[0], A.paintCol[1], A.paintCol[2], 1);
        float gx = sbx() + 10, gy = srow_y(SROW_INS) + 44;
        for (int i = 0; i < 16; i++) {
            const float *c = panel_color(i);
            float px = gx + (i % 4) * 55.0f, py = gy + (i / 4) * 26.0f;
            rk_ui_rect(px, py, 50, 24, c[0], c[1], c[2], 1);
        }
        /* MATERIAL row: texture layer + lock, acting on the island /
           whole mesh when nothing is selected */
        ui_text(sbx() + 10, srow_y(SROW_MAT) - 14, "MATERIAL", 1,
                0.55f, 0.6f, 0.7f, 1);
        int ct = mat_cur_tex();
        char ml[32];
        snprintf(ml, sizeof ml, "TEX: %s",
                 ct < 0 ? "NONE" : g_tex[ct].name);
        float mw = (SB_W - 16) * 0.66f;
        sb_btn(sbx() + 8, srow_y(SROW_MAT), mw, ml, ct >= 0, 0);
        sb_btn(sbx() + 8 + mw + 4, srow_y(SROW_MAT), SB_W - 16 - mw - 4,
               mat_cur_lock() ? "LOCKED" : "LOCK", mat_cur_lock(), 0);
    }
    sb_btn(sbx() + 8, srow_y(SROW_RESET), SB_W - 16, "RESET", 0, 0);
    sb_btn(sbx() + 8, srow_y(SROW_DONE), SB_W - 16, "DONE", 0, 1);
    sb_btn(sbx() + 8, srow_y(SROW_EXIT), SB_W - 16, "EXIT", 0, 0);
    /* counters */
    char cnt[64]; snprintf(cnt, sizeof cnt, "VERTS %d/%d", s_nv, SC_MAX_V);
    ui_text(sbx() + 10, srow_y(SROW_EXIT) + 40, cnt, 1, 0.6f, 0.7f, 0.8f, 1);
    snprintf(cnt, sizeof cnt, "TRIS %d/%d", s_nf, SC_MAX_F);
    ui_text(sbx() + 10, srow_y(SROW_EXIT) + 56, cnt, 1, 0.6f, 0.7f, 0.8f, 1);
    int bn2 = 0;
    for (int i = 0; i < ne; i++)
        if (edge_face_count(edges[i][0], edges[i][1]) == 1) bn2++;
    if (bn2) {
        snprintf(cnt, sizeof cnt, "OPEN EDGES %d", bn2);
        ui_text(sbx() + 10, srow_y(SROW_EXIT) + 76, cnt, 1, 1.0f, 0.6f, 0.3f, 1);
    }
    /* toast */
    if (s_toast[0] && glfwGetTime() - s_toastT < 2.6) {
        float tw = (float)ui_text_w(s_toast, 1) + 20.0f;
        float tx = ((float)A.fbw - tw) * 0.5f, ty = (float)A.fbh - 56;
        rk_ui_rect(tx, ty, tw, 24, 0.12f, 0.5f, 0.3f, 0.92f);
        ui_text(tx + 10, ty + 8, s_toast, 1, 1, 1, 1, 1);
    }
    ui_text(16, (float)A.fbh - 24,
            "MOVE DRAGS   ADDV CLICKS EDGE   PART MOVES ISLAND   RMB DRAG=LOOK",
            1, 0.55f, 0.6f, 0.7f, 1);
    /* ---------- start-shape picker ---------- */
    if (s_shapePick) {
        static const int PM[6] = {RK_MESH_CUBE, RK_MESH_PYRAMID,
                                  RK_MESH_SPHERE, RK_MESH_WEDGE,
                                  RK_MESH_CYLINDER, RK_MESH_CUBE};
        float x0 = ((float)A.fbw - 260.0f) * 0.5f;
        float y0 = ((float)A.fbh - 400.0f) * 0.5f;
        rk_ui_rect(0, 0, (float)A.fbw, (float)A.fbh, 0, 0, 0, 0.45f);
        rk_ui_rect(x0, y0, 260, 400, 0.13f, 0.14f, 0.17f, 1);
        rk_ui_rect(x0, y0, 260, 2, 0.3f, 0.6f, 0.9f, 1);
        ui_text(x0 + 16, y0 + 14, "START SHAPE", 2, 1, 1, 1, 1);
        for (int i = 0; i < 6; i++) {
            float px, py; picker_cell(i, &px, &py);
            int hov = A.mx >= px && A.mx < px + PICK_CELL_W &&
                      A.my >= py && A.my < py + 106;
            rk_ui_rect(px, py, PICK_CELL_W, 106,
                       0.19f + hov*0.08f, 0.21f + hov*0.08f,
                       0.27f + hov*0.08f, 1);
            ui_text(px + (PICK_CELL_W - ui_text_w(START_NAMES[i], 2)) * 0.5f,
                    py + 82, START_NAMES[i], 2, 1, 1, 1, 1);
        }
        ui_text(x0 + 16, y0 + 376, "CLICK = BUILD THIS SHAPE", 1,
                0.55f, 0.6f, 0.7f, 1);
        rk_ui_flush();
        for (int i = 0; i < 6; i++) {
            float px, py; picker_cell(i, &px, &py);
            rk_draw_icon(PM[i], (int)px + 14, (int)py + 6, 82, 72,
                         0.55f, 0.75f, 1.0f, NULL);
        }
    }

    /* ---------- name+confirm dialog ---------- */
    if (s_naming) {
        float cx = ((float)A.fbw - 340.0f) * 0.5f, cy = (float)A.fbh * 0.5f - 90;
        rk_ui_rect(0, 0, (float)A.fbw, (float)A.fbh, 0, 0, 0, 0.45f);
        rk_ui_rect(cx, cy, 340, 172, 0.13f, 0.14f, 0.17f, 1);
        rk_ui_rect(cx, cy, 340, 2, 0.3f, 0.6f, 0.9f, 1);
        ui_text(cx + 16, cy + 14, "SAVE ASSET - NAME IT:", 2, 1, 1, 1, 1);
        rk_ui_rect(cx + 16, cy + 42, 308, 30, 0.05f, 0.06f, 0.08f, 1);
        rk_ui_rect(cx + 16, cy + 42, 2, 30, 0.3f, 0.6f, 0.9f, 1);
        char disp[24]; snprintf(disp, sizeof disp, "%s_", s_name);
        ui_text(cx + 24, cy + 52, disp, 1, 0.9f, 0.95f, 1, 1);
        ui_text(cx + 16, cy + 76, "EMPTY NAME = \"CUSTOM\"", 1, 0.5f, 0.55f, 0.65f, 1);
        float bx[3], bw[3]; naming_btns(bx, bw);
        const char *bt[3] = {"SAVE", "KEEP EDITING", "DISCARD"};
        for (int i = 0; i < 3; i++) {
            float by = naming_by();
            rk_ui_rect(bx[i], by, bw[i], 24,
                       i == 0 ? 0.2f : 0.16f, i == 0 ? 0.45f : 0.18f,
                       i == 0 ? 0.28f : 0.22f, 1);
            ui_text(bx[i] + (bw[i] - ui_text_w(bt[i], 1)) * 0.5f, by + 8,
                    bt[i], 1, 1, 1, 1, 1);
        }
    }
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
