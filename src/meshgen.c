/* meshgen.c - CPU generation of the built-in unit-box shape meshes.
 * Shapes are authored centered on the origin in a unit box (y in [-0.5,0.5])
 * as flat-shaded vertex soup; world instancing scales them to block size. */
#include "meshgen.h"
#include "math3d.h"

static Vert *g_verts;
static int g_vmax;
static int g_nv;

static void mv3(float x, float y, float z, float nx, float ny, float nz) {
    if (g_nv >= g_vmax) return;
    Vert *v = &g_verts[g_nv++];
    v->pos[0]=x; v->pos[1]=y; v->pos[2]=z;
    v->nrm[0]=nx; v->nrm[1]=ny; v->nrm[2]=nz;
    v->col[0]=v->col[1]=v->col[2]=1.0f;
    v->tex=-1.0f;
}
static void tri(v3 a, v3 b, v3 c, v3 n) {
    mv3(a.x,a.y,a.z,n.x,n.y,n.z);
    mv3(b.x,b.y,b.z,n.x,n.y,n.z);
    mv3(c.x,c.y,c.z,n.x,n.y,n.z);
}
static void triN(v3 a, v3 b, v3 c, v3 na, v3 nb, v3 nc) { /* per-vertex normals */
    mv3(a.x,a.y,a.z,na.x,na.y,na.z);
    mv3(b.x,b.y,b.z,nb.x,nb.y,nb.z);
    mv3(c.x,c.y,c.z,nc.x,nc.y,nc.z);
}
static void quad(v3 a, v3 b, v3 c, v3 d, v3 n) { tri(a,b,c,n); tri(a,c,d,n); }

static int gen_cube(void) {
    int s = g_nv;
    float h = 0.5f;
    v3 v[8] = {
        {-h,-h,-h},{h,-h,-h},{h,h,-h},{-h,h,-h},
        {-h,-h, h},{h,-h, h},{h,h, h},{-h,h, h}
    };
    quad(v[4],v[5],v[6],v[7], v3_make(0,0,1));
    quad(v[1],v[0],v[3],v[2], v3_make(0,0,-1));
    quad(v[5],v[1],v[2],v[6], v3_make(1,0,0));
    quad(v[0],v[4],v[7],v[3], v3_make(-1,0,0));
    quad(v[7],v[6],v[2],v[3], v3_make(0,1,0));
    quad(v[0],v[1],v[5],v[4], v3_make(0,-1,0));
    return s;
}

static int gen_wedge(void) { /* ramp rising toward -z */
    int s = g_nv;
    float h = 0.5f;
    v3 a={-h,-h,h}, b={h,-h,h}, c={h,-h,-h}, d={-h,-h,-h};
    v3 e={h,h,-h}, f={-h,h,-h};
    quad(d,c,b,a, v3_make(0,-1,0));
    quad(c,d,f,e, v3_make(0,0,-1));
    tri(b,c,e, v3_make(1,0,0));
    tri(d,a,f, v3_make(-1,0,0));
    quad(a,b,e,f, v3_norm(v3_make(0,0.7071f,0.7071f)));
    return s;
}

static int gen_cylinder_raw(int seg, int wheel) {
    int s = g_nv;
    float h = 0.5f, r = 0.5f;
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i/seg*6.2831853f, a1 = (float)(i+1)/seg*6.2831853f;
        float c0=cosf(a0),s0=sinf(a0), c1=cosf(a1),s1=sinf(a1);
        v3 p0={r*c0,-h,r*s0}, p1={r*c1,-h,r*s1}, p2={r*c1,h,r*s1}, p3={r*c0,h,r*s0};
        v3 n0={c0,0,s0}, n1={c1,0,s1};
        triN(p0,p1,p2, n0,n1,n1);            /* smooth side wall */
        triN(p0,p2,p3, n0,n1,n0);
        v3 tc={0,h,0}, t0={r*c0,h,r*s0}, t1={r*c1,h,r*s1};
        tri(t0,tc,t1, v3_make(0,1,0));       /* top cap faces up */
        v3 bc={0,-h,0}, b0={r*c0,-h,r*s0}, b1={r*c1,-h,r*s1};
        tri(b1,b0,bc, v3_make(0,-1,0));      /* bottom cap faces down */
    }
    if (wheel) { /* rotate -90deg about Z: axis Y -> X */
        for (int i = s; i < g_nv; i++) {
            Vert *v = &g_verts[i];
            float x = v->pos[0], y = v->pos[1];
            v->pos[0] = y; v->pos[1] = -x;
            float nx = v->nrm[0], ny = v->nrm[1];
            v->nrm[0] = ny; v->nrm[1] = -nx;
        }
    }
    return s;
}

static int gen_cone(int seg) {
    int s = g_nv;
    float h = 0.5f, r = 0.5f;
    v3 apex = {0, h, 0}, bc = {0,-h,0};
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i/seg*6.2831853f, a1 = (float)(i+1)/seg*6.2831853f;
        v3 p0={r*cosf(a0),-h,r*sinf(a0)}, p1={r*cosf(a1),-h,r*sinf(a1)};
        /* per-vertex slant normals: n = cross(tangent, slant dir) */
        v3 t0=v3_make(-sinf(a0),0,cosf(a0)), t1=v3_make(-sinf(a1),0,cosf(a1));
        v3 n0=v3_norm(v3_cross(t0, v3_sub(apex,p0)));
        v3 n1=v3_norm(v3_cross(t1, v3_sub(apex,p1)));
        v3 na=v3_norm(v3_add(n0,n1));
        triN(p0,p1,apex, n0,n1,na);
        tri(bc,p1,p0, v3_make(0,-1,0));      /* bottom cap faces down */
    }
    return s;
}

static int gen_sphere(int lat, int lon) {
    int s = g_nv;
    for (int i = 0; i < lat; i++) {
        float t0 = (float)i/lat*3.14159265f, t1 = (float)(i+1)/lat*3.14159265f;
        for (int j = 0; j < lon; j++) {
            float p0 = (float)j/lon*6.2831853f, p1 = (float)(j+1)/lon*6.2831853f;
            float r = 0.5f;
            v3 v00={r*sinf(t0)*cosf(p0), r*cosf(t0), r*sinf(t0)*sinf(p0)};
            v3 v01={r*sinf(t0)*cosf(p1), r*cosf(t0), r*sinf(t0)*sinf(p1)};
            v3 v10={r*sinf(t1)*cosf(p0), r*cosf(t1), r*sinf(t1)*sinf(p0)};
            v3 v11={r*sinf(t1)*cosf(p1), r*cosf(t1), r*sinf(t1)*sinf(p1)};
            triN(v00,v10,v11, v3_norm(v00),v3_norm(v10),v3_norm(v11));
            triN(v00,v11,v01, v3_norm(v00),v3_norm(v11),v3_norm(v01));
        }
    }
    return s;
}

static int gen_torus(int maj, int min) { /* axis Y, centered */
    int s = g_nv;
    float R = 0.32f, r = 0.17f;
    for (int i = 0; i < maj; i++) {
        float u0 = (float)i/maj*6.2831853f, u1 = (float)(i+1)/maj*6.2831853f;
        for (int j = 0; j < min; j++) {
            float v0 = (float)j/min*6.2831853f, v1 = (float)(j+1)/min*6.2831853f;
            v3 p[4], n[4]; float us[4]={u0,u1,u1,u0}, vs[4]={v0,v0,v1,v1};
            for (int k = 0; k < 4; k++) {
                float cu=cosf(us[k]), su=sinf(us[k]), cv=cosf(vs[k]), sv=sinf(vs[k]);
                p[k] = v3_make((R+r*cv)*cu, r*sv, (R+r*cv)*su);
                n[k] = v3_norm(v3_make(cv*cu, sv, cv*su));
            }
            triN(p[0],p[1],p[2], n[0],n[1],n[2]);
            triN(p[0],p[2],p[3], n[0],n[2],n[3]);
        }
    }
    return s;
}

static int gen_dome(int lat, int lon) { /* upper hemisphere + bottom disc, unit box */
    int s = g_nv;
    float r = 0.5f;
    for (int i = 0; i < lat; i++) {
        float t0 = (float)i/lat*1.5707963f, t1 = (float)(i+1)/lat*1.5707963f;
        for (int j = 0; j < lon; j++) {
            float p0 = (float)j/lon*6.2831853f, p1 = (float)(j+1)/lon*6.2831853f;
            v3 v00={r*sinf(t0)*cosf(p0), r*cosf(t0), r*sinf(t0)*sinf(p0)};
            v3 v01={r*sinf(t0)*cosf(p1), r*cosf(t0), r*sinf(t0)*sinf(p1)};
            v3 v10={r*sinf(t1)*cosf(p0), r*cosf(t1), r*sinf(t1)*sinf(p0)};
            v3 v11={r*sinf(t1)*cosf(p1), r*cosf(t1), r*sinf(t1)*sinf(p1)};
            triN(v00,v10,v11, v3_norm(v00),v3_norm(v10),v3_norm(v11));
            triN(v00,v11,v01, v3_norm(v00),v3_norm(v11),v3_norm(v01));
        }
    }
    /* bottom disc at y=-0.02 (inside block bounds), facing down */
    for (int j = 0; j < lon; j++) {
        float p0 = (float)j/lon*6.2831853f, p1 = (float)(j+1)/lon*6.2831853f;
        v3 a={r*cosf(p0),-0.02f,r*sinf(p0)}, b={r*cosf(p1),-0.02f,r*sinf(p1)};
        v3 c={0,-0.02f,0};
        tri(b,a,c, v3_make(0,-1,0));         /* down-facing winding */
    }
    return s;
}

static int gen_cornerw(void) { /* apex at (-x,+y,-z) corner, crease to opposite */
    int s = g_nv;
    float h = 0.5f;
    v3 d={-h,-h,-h}, c={h,-h,-h}, b={h,-h,h}, a={-h,-h,h};
    v3 p={-h,h,-h};
    quad(d,c,b,a, v3_make(0,-1,0));                 /* base */
    tri(d,c,p, v3_make(0,0,-1));                    /* -z wall */
    tri(a,d,p, v3_make(-1,0,0));                    /* -x wall */
    tri(p,b,c, v3_norm(v3_make(1,1,0)));            /* slope toward +x */
    tri(p,a,b, v3_norm(v3_make(0,1,1)));            /* slope toward +z */
    return s;
}

static int gen_pyramid(void) { /* square pyramid, unit box */
    int s = g_nv;
    float h = 0.5f;
    v3 ap = {0, h, 0};
    v3 b0={-h,-h,-h}, b1={h,-h,-h}, b2={h,-h,h}, b3={-h,-h,h};
    v3 bs[4] = {b0,b1,b2,b3};
    for (int i = 0; i < 4; i++) {
        v3 a = bs[i], b = bs[(i+1)%4];
        v3 n = v3_norm(v3_cross(v3_sub(b,a), v3_sub(ap,a)));
        tri(a, b, ap, n);
    }
    quad(b0,b1,b2,b3, v3_make(0,-1,0));
    return s;
}

/* 3-step staircase ascending toward -z (sits in the unit box) */
static int gen_stair(void) {
    int s = g_nv;
    float h = 0.5f;
    for (int i = 0; i < 3; i++) {
        float z0 = 0.5f - i / 3.0f, z1 = z0 - 1.0f / 3.0f;
        float y1 = -0.5f + (i + 1) / 3.0f;
        v3 f0={-h,-h,z0}, f1={h,-h,z0}, f2={h,y1,z0}, f3={-h,y1,z0};
        quad(f0,f1,f2,f3, v3_make(0,0,1));                    /* riser   */
        v3 t0={-h,y1,z1}, t1={h,y1,z1}, t2={h,y1,z0}, t3={-h,y1,z0};
        quad(t0,t1,t2,t3, v3_make(0,1,0));                    /* tread   */
        v3 l0={-h,-h,z0}, l1={-h,-h,z1}, l2={-h,y1,z1}, l3={-h,y1,z0};
        quad(l0,l1,l2,l3, v3_make(-1,0,0));                   /* -x side */
        v3 r0={h,-h,z1}, r1={h,-h,z0}, r2={h,y1,z0}, r3={h,y1,z1};
        quad(r0,r1,r2,r3, v3_make(1,0,0));                    /* +x side */
    }
    v3 d0={-h,-h,-h}, d1={h,-h,-h}, d2={h,h,-h}, d3={-h,h,-h};
    quad(d1,d0,d3,d2, v3_make(0,0,-1));                       /* back    */
    v3 g0={-h,-h,0.5f}, g1={h,-h,0.5f};
    quad(g0,g1,d1,d0, v3_make(0,-1,0));                       /* bottom  */
    return s;
}

/* axis-aligned box helper for FRAME/CORSTAIR builders */
static void beam(v3 lo, v3 hi) {
    v3 c[8] = {
        {lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{lo.x,hi.y,lo.z},
        {lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z}
    };
    quad(c[4],c[5],c[6],c[7], v3_make(0,0,1));
    quad(c[1],c[0],c[3],c[2], v3_make(0,0,-1));
    quad(c[5],c[1],c[2],c[6], v3_make(1,0,0));
    quad(c[0],c[4],c[7],c[3], v3_make(-1,0,0));
    quad(c[7],c[6],c[2],c[3], v3_make(0,1,0));
    quad(c[0],c[1],c[5],c[4], v3_make(0,-1,0));
}

/* hollow cube: 12 edge beams -> see-through frame */
static int gen_frame(void) {
    int s = g_nv;
    float h = 0.5f, t = 0.14f;
    for (int a = 0; a < 3; a++)            /* 4 beams per axis */
        for (int u = -1; u <= 1; u += 2)
            for (int v = -1; v <= 1; v += 2) {
                v3 lo = {-h,-h,-h}, hi = {h,h,h};
                lo.e[a] = -h; hi.e[a] = h;
                int b1 = (a + 1) % 3, b2 = (a + 2) % 3;
                lo.e[b1] = u > 0 ? h - t : -h;
                hi.e[b1] = u > 0 ? h : -h + t;
                lo.e[b2] = v > 0 ? h - t : -h;
                hi.e[b2] = v > 0 ? h : -h + t;
                beam(lo, hi);
            }
    return s;
}

/* rounded-edge cube: superellipsoid (exponent ~0.35 reads as a soft box) */
static int gen_roundc(int n) {
    int s = g_nv;
    const float e = 0.35f, r = 0.5f;
    for (int i = 0; i < n; i++) {
        float u0 = -1.5707963f + (float)i / n * 3.14159265f;
        float u1 = -1.5707963f + (float)(i + 1) / n * 3.14159265f;
        for (int j = 0; j < 2 * n; j++) {
            float v0 = (float)j / (2 * n) * 6.2831853f;
            float v1 = (float)(j + 1) / (2 * n) * 6.2831853f;
            v3 p[4], nm[4]; float us[4]={u0,u0,u1,u1}, vs[4]={v0,v1,v1,v0};
            for (int k = 0; k < 4; k++) {
                float cu = cosf(us[k]), su = sinf(us[k]);
                float cv = cosf(vs[k]), sv = sinf(vs[k]);
                float pu = fabsf(cu) < 1e-6f ? 0 : powf(fabsf(cu), e);
                float pv = fabsf(cv) < 1e-6f ? 0 : powf(fabsf(cv), e);
                float qw = fabsf(su) < 1e-6f ? 0 : powf(fabsf(su), e);
                float rv = fabsf(sv) < 1e-6f ? 0 : powf(fabsf(sv), e);
                float x = r * pu * (cu < 0 ? -1 : 1) * pv * (cv < 0 ? -1 : 1);
                float y = r * qw * (su < 0 ? -1 : 1);
                float z = r * pu * (cu < 0 ? -1 : 1) * rv * (sv < 0 ? -1 : 1);
                p[k] = v3_make(x, y, z);
                /* superellipsoid normal ~ (x^(2-e), y^(2-e), z^(2-e)) */
                float ne = 2.0f - e;
                v3 nn = v3_make(fabsf(x) < 1e-6f ? 0 : (x < 0 ? -1 : 1) * powf(fabsf(x) / r, ne),
                                fabsf(y) < 1e-6f ? 0 : (y < 0 ? -1 : 1) * powf(fabsf(y) / r, ne),
                                fabsf(z) < 1e-6f ? 0 : (z < 0 ? -1 : 1) * powf(fabsf(z) / r, ne));
                nm[k] = v3_len(nn) < 1e-5f ? v3_make(0,1,0) : v3_norm(nn);
            }
            triN(p[0],p[2],p[3], nm[0],nm[2],nm[3]);
            triN(p[0],p[1],p[2], nm[0],nm[1],nm[2]);
        }
    }
    return s;
}

/* swept tube along a circle arc: (u,v) span the plane, ax = plane normal,
   off = circle center, a0..a1 = arc sweep. 6-gon cross-section, radius tr,
   per-vertex normals for smooth shading. */
static void tube_arc(v3 u, v3 v, v3 ax, v3 off, float r, float a0, float a1,
                     int segs, float tr) {
    for (int i = 0; i < segs; i++) {
        float us[2] = {a0 + (a1-a0)*(float)i/segs,
                       a0 + (a1-a0)*(float)(i+1)/segs};
        v3 c[2], rd[2];
        for (int k = 0; k < 2; k++) {
            float ca = cosf(us[k]), sa = sinf(us[k]);
            rd[k] = v3_add(v3_scale(u,ca), v3_scale(v,sa));   /* unit radial */
            c[k] = v3_add(off, v3_scale(rd[k], r));
        }
        for (int k = 0; k < 6; k++) {
            float w0 = (float)k/6*6.2831853f, w1 = (float)(k+1)/6*6.2831853f;
            v3 d00 = v3_add(v3_scale(ax,cosf(w0)), v3_scale(rd[0],sinf(w0)));
            v3 d10 = v3_add(v3_scale(ax,cosf(w0)), v3_scale(rd[1],sinf(w0)));
            v3 d01 = v3_add(v3_scale(ax,cosf(w1)), v3_scale(rd[0],sinf(w1)));
            v3 d11 = v3_add(v3_scale(ax,cosf(w1)), v3_scale(rd[1],sinf(w1)));
            triN(v3_add(c[0],v3_scale(d00,tr)),
                 v3_add(c[1],v3_scale(d10,tr)),
                 v3_add(c[1],v3_scale(d11,tr)), d00,d10,d11);
            triN(v3_add(c[0],v3_scale(d00,tr)),
                 v3_add(c[1],v3_scale(d11,tr)),
                 v3_add(c[0],v3_scale(d01,tr)), d00,d11,d01);
        }
    }
}

/* cage sphere (birdcage): 4 meridian rib tubes end on small polar-cap rings;
   latitude rings at 0/+-38deg brace the ribs. ~3.7k verts fits the 4k
   sculpt-preset buffer and the 8k fuse-bake buffer. */
static int gen_cageball(void) {
    int s = g_nv;
    const float R = 0.44f, TR = 0.055f;   /* outer extent R+TR ~= 0.5 */
    const float T75 = 1.3090f;
    for (int k = 0; k < 4; k++) {         /* meridian ribs, 45deg apart */
        float ph = (float)k * 0.7853982f;
        tube_arc(v3_make(cosf(ph),0,sinf(ph)), v3_make(0,1,0),
                 v3_make(-sinf(ph),0,cosf(ph)), v3_make(0,0,0),
                 R, -T75, T75, 12, TR);
    }
    static const float LAT[5] = {-1.3090f,-0.6632f,0.0f,0.6632f,1.3090f};
    static const int   LSG[5] = {8,12,16,12,8};
    for (int i = 0; i < 5; i++) {
        float y = R*sinf(LAT[i]), rr = R*cosf(LAT[i]);
        tube_arc(v3_make(1,0,0), v3_make(0,0,1), v3_make(0,1,0),
                 v3_make(0,y,0), rr, 0.0f, 6.2831853f, LSG[i], TR);
    }
    return s;
}

/* L-shaped corner staircase on a 3x3 cell grid: climb up the -x column
   to the back-left corner, turn, then along the back row to back-right.
   Each cell is a solid column whose top is (i+1)/5 of the height, so the
   5 steps climb monotonically from the front-left corner to back-right. */
static int gen_corstair(void) {
    int s = g_nv;
    const float h0 = -0.5f, st = 1.0f / 5.0f;
    static const float C[5][4] = {          /* x0, x1, z0, z1 per cell */
        {-0.5f,-0.16667f, 0.16667f, 0.5f},  /* front-left (lowest)  */
        {-0.5f,-0.16667f,-0.16667f, 0.16667f},/* mid-left           */
        {-0.5f,-0.16667f,-0.5f,-0.16667f},  /* back-left (corner)   */
        {-0.16667f, 0.16667f,-0.5f,-0.16667f},/* back-mid           */
        { 0.16667f, 0.5f,  -0.5f,-0.16667f},/* back-right (top)     */
    };
    for (int i = 0; i < 5; i++)
        beam(v3_make(C[i][0], h0, C[i][2]),
             v3_make(C[i][1], h0 + (i + 1) * st, C[i][3]));
    return s;
}

/* box arch: two legs + a lintel across the top, opening along z */
static int gen_arch(void) {
    int s = g_nv;
    const float h = 0.5f, tw = 0.16f, tz = 0.30f;
    beam(v3_make(-h, -h, -tz), v3_make(-h + tw, h, tz));   /* -x leg   */
    beam(v3_make(h - tw, -h, -tz), v3_make(h, h, tz));     /* +x leg   */
    beam(v3_make(-h, 0.30f, -tz), v3_make(h, h, tz));      /* lintel   */
    return s;
}

/* standing ring: the Y-axis torus tipped 90deg about X so it stands in XY */
static int gen_ring(void) {
    int s = g_nv;
    gen_torus(20, 10);
    for (int i = s; i < g_nv; i++) {
        Vert *v = &g_verts[i];
        float y = v->pos[1], z = v->pos[2];
        v->pos[1] = -z; v->pos[2] = y;
        float ny = v->nrm[1], nz = v->nrm[2];
        v->nrm[1] = -nz; v->nrm[2] = ny;
    }
    return s;
}

static int gen_line_box(void) {
    int s = g_nv;
    float h = 0.5f;
    v3 v[8] = {
        {-h,-h,-h},{h,-h,-h},{h,h,-h},{-h,h,-h},
        {-h,-h, h},{h,-h, h},{h,h, h},{-h,h, h}
    };
    int e[24] = {0,1,1,2,2,3,3,0, 4,5,5,6,6,7,7,4, 0,4,1,5,2,6,3,7};
    for (int i = 0; i < 24; i++)
        mv3(v[e[i]].x,v[e[i]].y,v[e[i]].z, 0,1,0);
    return s;
}

enum { MESH_PLANE = RK_MESH_COUNT };
#define MESH_TOTAL (RK_MESH_COUNT + 1)

static int gen_plane(void) {
    int s = g_nv;
    v3 a={-0.5f,0,-0.5f}, b={0.5f,0,-0.5f}, c={0.5f,0,0.5f}, d={-0.5f,0,0.5f};
    quad(a,b,c,d, v3_make(0,1,0));
    return s;
}

static int gen_by_index(int shape) {
    switch (shape) {
    case RK_MESH_CUBE:     return gen_cube();
    case RK_MESH_WEDGE:    return gen_wedge();
    case RK_MESH_CYLINDER: return gen_cylinder_raw(20, 0);
    case RK_MESH_SPHERE:   return gen_sphere(10, 16);
    case RK_MESH_CONE:     return gen_cone(20);
    case RK_MESH_WHEEL:    return gen_cylinder_raw(20, 1);
    case RK_MESH_TORUS:    return gen_torus(20, 10);
    case RK_MESH_DOME:     return gen_dome(7, 16);
    case RK_MESH_PYRAMID:  return gen_pyramid();
    case RK_MESH_CORNERW:  return gen_cornerw();
    case RK_MESH_STAIR:    return gen_stair();
    case RK_MESH_FRAME:    return gen_frame();
    case RK_MESH_ROUNDC:   return gen_roundc(14);
    case RK_MESH_CAGEBALL: return gen_cageball();
    case RK_MESH_CORSTAIR: return gen_corstair();
    case RK_MESH_ARCH:     return gen_arch();
    case RK_MESH_RING:     return gen_ring();
    }
    return g_nv;
}
int meshgen_shape(int shape, Vert *dst, int maxverts) {
    g_verts = dst; g_vmax = maxverts; g_nv = 0;
    gen_by_index(shape);
    return g_nv;
}

int meshgen_build_all(Vert *pool, int maxverts,
                      uint32_t *off, uint32_t *count,
                      uint32_t *lineOff, uint32_t *lineCount) {
    g_verts = pool; g_vmax = maxverts; g_nv = 0;
    off[RK_MESH_CUBE]     = (uint32_t)gen_cube();
    off[RK_MESH_WEDGE]    = (uint32_t)gen_wedge();
    off[RK_MESH_CYLINDER] = (uint32_t)gen_cylinder_raw(20, 0);
    off[RK_MESH_SPHERE]   = (uint32_t)gen_sphere(10, 16);
    off[RK_MESH_CONE]     = (uint32_t)gen_cone(20);
    off[RK_MESH_WHEEL]    = (uint32_t)gen_cylinder_raw(20, 1);
    off[RK_MESH_TORUS]    = (uint32_t)gen_torus(20, 10);
    off[RK_MESH_DOME]     = (uint32_t)gen_dome(7, 16);
    off[RK_MESH_PYRAMID]  = (uint32_t)gen_pyramid();
    off[RK_MESH_CORNERW]  = (uint32_t)gen_cornerw();
    off[RK_MESH_STAIR]    = (uint32_t)gen_stair();
    off[RK_MESH_FRAME]    = (uint32_t)gen_frame();
    off[RK_MESH_ROUNDC]   = (uint32_t)gen_roundc(14);
    off[RK_MESH_CAGEBALL] = (uint32_t)gen_cageball();
    off[RK_MESH_CORSTAIR] = (uint32_t)gen_corstair();
    off[RK_MESH_ARCH]     = (uint32_t)gen_arch();
    off[RK_MESH_RING]     = (uint32_t)gen_ring();
    off[MESHGEN_PLANE]    = (uint32_t)gen_plane();
    *lineOff = (uint32_t)gen_line_box();
    for (int i = 0; i < MESHGEN_TOTAL; i++) {
        uint32_t next = (i+1 < MESHGEN_TOTAL) ? off[i+1] : *lineOff;
        count[i] = next - off[i];
    }
    *lineCount = (uint32_t)g_nv - *lineOff;
    return g_nv;
}