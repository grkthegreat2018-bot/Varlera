/* rk.c - Vulkan renderer.
 * - Dynamic rendering (core 1.3), no render-pass/framebuffer objects
 * - Reversed-Z main depth (clear 0, GREATER), 4x MSAA + resolve
 * - Directional shadow map (2048px, hardware-compare sampler + 4-tap PCF)
 * - Instanced drawing for all repeated geometry; deferred queue recording so the
 *   shadow pass can replay world draws inside the same command buffer
 * - Per-frame acquire semaphores + per-swapchain-image render-finished semaphores
 * - Manual VkDeviceMemory allocation (VMA 3.x is C++-only)
 */
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rk.h"
#include "math3d.h"

#define MAX_FRAMES   2
#define MAX_IMAGES   8
#define MAX_UI_QUADS 16384
#define MAX_VERTS    24576
#define MAX_DYN_VERTS (256*1024)
#define MAX_INST     8192
#define MAX_Q        1024
#define SHADOW_SIZE  2048

#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "VK error %d at %s:%d\n", _r, __FILE__, __LINE__); exit(1); } } while (0)

typedef struct { float pos[2]; float col[4]; } UIVert;
typedef struct { m4 model; float color[4]; uint32_t flags; uint32_t pad[3]; } Push;
typedef struct {
    m4 viewProj, iconVP, shadowVP;
    float camPos[4], lightDir[4], fogColor[4], fogParams[4];
    float texRect[RK_TEX_MAX][4];   /* u0,v0,tileW,tileH per texture id */
    float groundCol[4];             /* grid base color (world-settings tint) */
} UBO;

typedef struct { float x, y, w, h, r, g, b, a; } UIRect;

static v3 light_dir(void);

enum { K_GRID, K_WORLD, K_OVERLAY, K_GHOST, K_MESH, K_MLINES, K_LINES, K_UI, K_ICON };
typedef struct {
    int kind, mesh;
    uint32_t first, count;
    int px, py, pw, ph;
    float cr, cg, cb;
    float mm[16];                /* K_MESH model matrix */
} QEnt;

static struct {
    GLFWwindow *win;
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;

    VkSwapchainKHR swap;
    VkFormat surfFmt;
    VkColorSpaceKHR surfCS;
    VkExtent2D ext;
    uint32_t imgCount;
    VkImage imgs[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    VkImage msaaImg, depthImg;
    VkImageView msaaView, depthView;
    VkDeviceMemory msaaMem, depthMem;
    VkSampleCountFlagBits samples;

    /* shadow map (fixed size, lives forever) */
    VkImage shadowImg;
    VkImageView shadowView;
    VkDeviceMemory shadowMem;
    VkSampler shadowSamp;

    /* texture atlas (fixed 1024x1024 RGBA, lives forever) */
    VkImage texImg;
    VkImageView texView;
    VkDeviceMemory texMem;
    VkSampler texSamp;

    VkDescriptorSetLayout setLayout0, setLayout1;
    VkDescriptorPool pool;
    VkDescriptorSet set1;
    VkPipelineLayout pipeLayout;
    VkPipeline pGrid, pWorld, pWorldInst, pGhostInst, pLinesInst, pShadow, pUI;
    VkPipeline pMeshLine;            /* non-instanced LINE_LIST (wireframe) */
    VkCommandPool cmdPool;

    struct {
        VkCommandBuffer cmd;
        VkFence fence;
        VkSemaphore acq;
        VkBuffer ubo;    VkDeviceMemory uboMem;  void *uboMap;
        VkBuffer inst;   VkDeviceMemory instMem; void *instMap;
        VkBuffer uib;    VkDeviceMemory uibMem;  void *uiMap;
        VkBuffer mesh;   VkDeviceMemory meshMem; void *meshMap;
        VkDescriptorSet set0;
    } fr[MAX_FRAMES];
    VkSemaphore renderDone[MAX_IMAGES];
    uint32_t renderDoneCount;

    VkBuffer vbuf; VkDeviceMemory vbufMem;
    VkDeviceSize meshOff[RK_MESH_COUNT + 1];
    uint32_t meshCount[RK_MESH_COUNT + 1];
    VkDeviceSize lineOff; uint32_t lineCount;

    /* baked custom-model meshes: CPU store + rebuilt GPU buffer */
    VkBuffer mbuf; VkDeviceMemory mbufMem;
    Vert mverts[256 * 1024]; int mvn;     /* model vert pool */
    VkDeviceSize modelOff[RK_MAX_MODELS];
    uint32_t modelCount[RK_MAX_MODELS];
    int nmodels;

    /* per-frame queues */
    QEnt q[MAX_Q];           int qn;
    RkInst instStage[MAX_INST]; int instN;
    UIRect uiQ[MAX_UI_QUADS];   int uiN;
    UIVert uiStage[MAX_UI_QUADS * 6]; int uiVn;
    Vert meshStage[MAX_DYN_VERTS]; int meshVn;   /* K_MESH streamed verts */

    UBO ubo;
    v3 focus;
    uint32_t fi, imgIdx;
    VkCommandBuffer cmd;
    int inFrame;
    int needResize;
    double t;
} G;

/* ---------------- mesh generation ---------------- */

static Vert s_verts[MAX_VERTS];
static Vert *g_verts = s_verts;          /* redirected by rk_gen_shape */
static int g_vmax = MAX_VERTS;
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

/* sphere with circular holes on all 6 axis sides */
static int gen_cageball(int lat, int lon) {
    int s = g_nv;
    const float r = 0.5f, hole = 0.62f;   /* hole half-angle cosine */
    for (int i = 0; i < lat; i++) {
        float t0 = (float)i/lat*3.14159265f, t1 = (float)(i+1)/lat*3.14159265f;
        for (int j = 0; j < lon; j++) {
            float p0 = (float)j/lon*6.2831853f, p1 = (float)(j+1)/lon*6.2831853f;
            v3 q[4];
            float ts[4]={t0,t1,t1,t0}, ps[4]={p0,p0,p1,p1};
            for (int k = 0; k < 4; k++)
                q[k] = v3_make(r*sinf(ts[k])*cosf(ps[k]), r*cosf(ts[k]),
                               r*sinf(ts[k])*sinf(ps[k]));
            v3 c = v3_norm(v3_add(v3_add(q[0],q[1]),v3_add(q[2],q[3])));
            if (fabsf(c.x) > hole || fabsf(c.y) > hole || fabsf(c.z) > hole)
                continue;                  /* inside a hole opening */
            triN(q[0],q[1],q[2], v3_norm(q[0]),v3_norm(q[1]),v3_norm(q[2]));
            triN(q[0],q[2],q[3], v3_norm(q[0]),v3_norm(q[2]),v3_norm(q[3]));
        }
    }
    return s;
}

/* L-shaped corner staircase: two 3-step runs meeting at a mid landing */
static int gen_corstair(void) {
    int s = g_nv;
    /* landing block in the +x,-z corner at mid height */
    beam(v3_make(0.0f,-0.5f,-0.5f), v3_make(0.5f,-0.1666f,0.0f));
    /* run A: 3 steps climbing -z -> landing (along -x half) */
    for (int i = 0; i < 3; i++) {
        float z0 = 0.5f - i / 3.0f, z1 = z0 - 1.0f / 3.0f;
        float y1 = -0.5f + (i + 1) * (1.0f / 3.0f) * 0.5f;
        beam(v3_make(-0.5f,-0.5f,z1), v3_make(0.0f,y1,z0));
    }
    /* run B: 3 steps climbing -x -> landing top (along -z half) */
    for (int i = 0; i < 3; i++) {
        float x0 = 0.5f - i / 3.0f, x1 = x0 - 1.0f / 3.0f;
        float y1 = -0.1666f + (i + 1) * (1.0f / 3.0f) * 0.5f;
        beam(v3_make(x1,-0.5f,-0.5f), v3_make(x0,y1,0.0f));
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
    case RK_MESH_CAGEBALL: return gen_cageball(12, 18);
    case RK_MESH_CORSTAIR: return gen_corstair();
    }
    return g_nv;
}

int rk_gen_shape(int shape, Vert *dst, int maxverts) {
    g_verts = dst; g_vmax = maxverts; g_nv = 0;
    gen_by_index(shape);
    int n = g_nv;
    g_verts = s_verts; g_vmax = MAX_VERTS;
    return n;
}

static void build_mesh_data(void) {
    g_verts = s_verts; g_vmax = MAX_VERTS; g_nv = 0;
    G.meshOff[RK_MESH_CUBE]     = gen_cube();
    G.meshOff[RK_MESH_WEDGE]    = gen_wedge();
    G.meshOff[RK_MESH_CYLINDER] = gen_cylinder_raw(20, 0);
    G.meshOff[RK_MESH_SPHERE]   = gen_sphere(10, 16);
    G.meshOff[RK_MESH_CONE]     = gen_cone(20);
    G.meshOff[RK_MESH_WHEEL]    = gen_cylinder_raw(20, 1);
    G.meshOff[RK_MESH_TORUS]    = gen_torus(20, 10);
    G.meshOff[RK_MESH_DOME]     = gen_dome(7, 16);
    G.meshOff[RK_MESH_PYRAMID]  = gen_pyramid();
    G.meshOff[RK_MESH_CORNERW]  = gen_cornerw();
    G.meshOff[RK_MESH_STAIR]    = gen_stair();
    G.meshOff[RK_MESH_FRAME]    = gen_frame();
    G.meshOff[RK_MESH_ROUNDC]   = gen_roundc(14);
    G.meshOff[RK_MESH_CAGEBALL] = gen_cageball(12, 18);
    G.meshOff[RK_MESH_CORSTAIR] = gen_corstair();
    G.meshOff[MESH_PLANE]       = gen_plane();
    G.lineOff = gen_line_box();
    for (int i = 0; i < MESH_TOTAL; i++) {
        VkDeviceSize next = (i+1 < MESH_TOTAL) ? G.meshOff[i+1] : G.lineOff;
        G.meshCount[i] = (uint32_t)(next - G.meshOff[i]);
    }
    G.lineCount = (uint32_t)(g_nv - G.lineOff);
}

/* ---------------- memory & buffer helpers ---------------- */

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(G.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}

static VkDeviceMemory mem_alloc(VkDeviceSize size, uint32_t bits, VkMemoryPropertyFlags props) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = size;
    ai.memoryTypeIndex = mem_type(bits, props);
    VkDeviceMemory m;
    VKC(vkAllocateMemory(G.dev, &ai, NULL, &m));
    return m;
}

#define HOST_MEM (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

static VkBuffer buf_create(VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags props, VkDeviceMemory *outMem, void **mapped) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf;
    VKC(vkCreateBuffer(G.dev, &bi, NULL, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(G.dev, buf, &req);
    *outMem = mem_alloc(req.size, req.memoryTypeBits, props);
    VKC(vkBindBufferMemory(G.dev, buf, *outMem, 0));
    if (mapped) VKC(vkMapMemory(G.dev, *outMem, 0, size, 0, mapped));
    return buf;
}

static void upload_vertices(void) {
    VkDeviceSize bytes = (VkDeviceSize)g_nv * sizeof(Vert);
    void *map;
    VkDeviceMemory stagM;
    VkBuffer stag = buf_create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST_MEM, &stagM, &map);
    memcpy(map, s_verts, bytes);
    G.vbuf = buf_create(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &G.vbufMem, NULL);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = G.cmdPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer c; VKC(vkAllocateCommandBuffers(G.dev, &cai, &c));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(c, &bbi));
    VkBufferCopy cp = {0, 0, bytes};
    vkCmdCopyBuffer(c, stag, G.vbuf, 1, &cp);
    VKC(vkEndCommandBuffer(c));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &c;
    VKC(vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(G.queue));
    vkFreeCommandBuffers(G.dev, G.cmdPool, 1, &c);
    vkUnmapMemory(G.dev, stagM);
    vkDestroyBuffer(G.dev, stag, NULL);
    vkFreeMemory(G.dev, stagM, NULL);
}

/* ---------------- texture atlas ---------------- */

void rk_tex_rect(int id, float u0, float v0, float tw, float th) {
    if (id < 0 || id >= RK_TEX_MAX) return;
    G.ubo.texRect[id][0] = u0; G.ubo.texRect[id][1] = v0;
    G.ubo.texRect[id][2] = tw; G.ubo.texRect[id][3] = th;
}

static int s_texInit;

void rk_tex_upload(const uint8_t *rgba) {
    vkDeviceWaitIdle(G.dev);              /* texture edits are rare */
    VkDeviceSize bytes = (VkDeviceSize)RK_TEX_ATLAS * RK_TEX_ATLAS * 4;
    void *map; VkDeviceMemory stagM;
    VkBuffer stag = buf_create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST_MEM, &stagM, &map);
    memcpy(map, rgba, bytes);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = G.cmdPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer c; VKC(vkAllocateCommandBuffers(G.dev, &cai, &c));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(c, &bbi));
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = s_texInit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = G.texImg;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = s_texInit ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy cp = {0};
    cp.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = (VkExtent3D){RK_TEX_ATLAS, RK_TEX_ATLAS, 1};
    vkCmdCopyBufferToImage(c, stag, G.texImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VKC(vkEndCommandBuffer(c));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &c;
    VKC(vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(G.queue));
    vkFreeCommandBuffers(G.dev, G.cmdPool, 1, &c);
    vkUnmapMemory(G.dev, stagM);
    vkDestroyBuffer(G.dev, stag, NULL);
    vkFreeMemory(G.dev, stagM, NULL);
    s_texInit = 1;
}

/* ---------------- baked model meshes ---------------- */

void rk_models_reset(void) { G.nmodels = 0; G.mvn = 0; }

int rk_model_mesh_add(const Vert *v, int n) {
    if (G.nmodels >= RK_MAX_MODELS || G.mvn + n > (int)(sizeof G.mverts / sizeof(Vert)))
        return -1;
    int id = G.nmodels++;
    G.modelOff[id] = (VkDeviceSize)G.mvn;
    G.modelCount[id] = (uint32_t)n;
    memcpy(G.mverts + G.mvn, v, (size_t)n * sizeof(Vert));
    G.mvn += n;
    return RK_MESH_BASE_MODEL + id;
}

void rk_models_commit(void) {
    vkDeviceWaitIdle(G.dev);              /* fuse is rare; simplicity wins */
    if (G.mbuf) { vkDestroyBuffer(G.dev, G.mbuf, NULL); vkFreeMemory(G.dev, G.mbufMem, NULL); }
    G.mbuf = VK_NULL_HANDLE; G.mbufMem = VK_NULL_HANDLE;
    if (!G.mvn) return;
    VkDeviceSize bytes = (VkDeviceSize)G.mvn * sizeof(Vert);
    void *map; VkDeviceMemory stagM;
    VkBuffer stag = buf_create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST_MEM, &stagM, &map);
    memcpy(map, G.mverts, bytes);
    G.mbuf = buf_create(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &G.mbufMem, NULL);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = G.cmdPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer c; VKC(vkAllocateCommandBuffers(G.dev, &cai, &c));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(c, &bbi));
    VkBufferCopy cp = {0, 0, bytes};
    vkCmdCopyBuffer(c, stag, G.mbuf, 1, &cp);
    VKC(vkEndCommandBuffer(c));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &c;
    VKC(vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE));
    VKC(vkQueueWaitIdle(G.queue));
    vkFreeCommandBuffers(G.dev, G.cmdPool, 1, &c);
    vkUnmapMemory(G.dev, stagM);
    vkDestroyBuffer(G.dev, stag, NULL);
    vkFreeMemory(G.dev, stagM, NULL);
}

/* ---------------- shaders ---------------- */

static VkShaderModule load_shader(const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.spv", SHADER_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing shader %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc(n);
    if (fread(code, 1, n, f) != (size_t)n) { fclose(f); exit(1); }
    fclose(f);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = (size_t)n; ci.pCode = code;
    VkShaderModule m;
    VKC(vkCreateShaderModule(G.dev, &ci, NULL, &m));
    free(code);
    return m;
}

/* ---------------- pipelines ---------------- */

typedef struct {
    VkShaderModule vs, fs;
    VkPrimitiveTopology topo;
    int instanced;                 /* binding1 = RkInst */
    VkVertexInputAttributeDescription attrs[8];
    VkVertexInputBindingDescription binds[2];
    uint32_t attrCount, bindCount;
    int blend, depthTest, depthWrite;
    VkCompareOp cmp;
    VkCullModeFlags cull;
    int depthBias;
    VkSampleCountFlagBits samples;
} PipeDesc;

static VkPipeline make_pipeline(const PipeDesc *d) {
    VkPipelineShaderStageCreateInfo st[2] = {0};
    uint32_t sn = 0;
    st[sn].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[sn].stage = VK_SHADER_STAGE_VERTEX_BIT; st[sn].module = d->vs; st[sn].pName = "main"; sn++;
    if (d->fs) {
        st[sn].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[sn].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[sn].module = d->fs; st[sn].pName = "main"; sn++;
    }

    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = d->bindCount; vin.pVertexBindingDescriptions = d->binds;
    vin.vertexAttributeDescriptionCount = d->attrCount; vin.pVertexAttributeDescriptions = d->attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d->topo;

    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d->cull; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    if (d->depthBias) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 1.5f;
        rs.depthBiasSlopeFactor = 2.0f;
    }

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d->samples;

    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d->depthTest; ds.depthWriteEnable = d->depthWrite;
    ds.depthCompareOp = d->cmp;
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState att = {0};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    if (d->blend) {
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d->fs ? 1 : 0; cb.pAttachments = &att;

    VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;

    VkPipelineRenderingCreateInfo rd = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (d->fs) {
        rd.colorAttachmentCount = 1; rd.pColorAttachmentFormats = &G.surfFmt;
        rd.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    } else {
        rd.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    }

    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rd;
    pi.stageCount = sn; pi.pStages = st;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp; pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dyn;
    pi.layout = G.pipeLayout;
    VkPipeline p;
    VKC(vkCreateGraphicsPipelines(G.dev, VK_NULL_HANDLE, 1, &pi, NULL, &p));
    return p;
}

static void fill_mesh_vertex_input(PipeDesc *d) {
    d->binds[0] = (VkVertexInputBindingDescription){0, sizeof(Vert), VK_VERTEX_INPUT_RATE_VERTEX};
    d->attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    d->attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};
    d->attrs[2] = (VkVertexInputAttributeDescription){6, 0, VK_FORMAT_R32G32B32_SFLOAT, 24};
    d->attrs[3] = (VkVertexInputAttributeDescription){7, 0, VK_FORMAT_R32_SFLOAT, 36};
    d->attrCount = 4; d->bindCount = 1;
}

static void fill_inst_vertex_input(PipeDesc *d) {
    fill_mesh_vertex_input(d);
    d->binds[1] = (VkVertexInputBindingDescription){1, sizeof(RkInst), VK_VERTEX_INPUT_RATE_INSTANCE};
    d->attrs[4] = (VkVertexInputAttributeDescription){2, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RkInst,pos)};
    d->attrs[5] = (VkVertexInputAttributeDescription){3, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RkInst,scale)};
    d->attrs[6] = (VkVertexInputAttributeDescription){4, 1, VK_FORMAT_R8G8B8A8_UNORM, offsetof(RkInst,color)};
    d->attrs[7] = (VkVertexInputAttributeDescription){5, 1, VK_FORMAT_R32_UINT, offsetof(RkInst,flags)};
    d->attrCount = 8; d->bindCount = 2;
}

static void make_pipelines(void) {
    VkShaderModule wv = load_shader("world.vert");
    VkShaderModule wi = load_shader("world_i.vert");
    VkShaderModule sv = load_shader("shadow.vert");
    VkShaderModule wf = load_shader("world.frag");
    VkShaderModule gf = load_shader("grid.frag");
    VkShaderModule uv = load_shader("ui.vert");
    VkShaderModule uf = load_shader("ui.frag");

    PipeDesc d = {0};

    /* grid: non-instanced, no cull, opaque */
    d.vs=wv; d.fs=gf; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    fill_mesh_vertex_input(&d);
    d.cull=VK_CULL_MODE_NONE; d.cmp=VK_COMPARE_OP_GREATER;
    d.depthTest=1; d.depthWrite=1; d.samples=G.samples;
    G.pGrid = make_pipeline(&d);

    /* icons / non-instanced lit */
    d.fs=wf; d.cull=VK_CULL_MODE_NONE;
    G.pWorld = make_pipeline(&d);

    /* instanced world blocks + overlays (no cull: closed shapes, immune to
       winding slips on generated meshes) */
    d.vs=wi; fill_inst_vertex_input(&d); d.cull=VK_CULL_MODE_NONE;
    G.pWorldInst = make_pipeline(&d);

    /* instanced ghost (blend) */
    d.blend=1; d.depthWrite=0;
    G.pGhostInst = make_pipeline(&d);

    /* instanced line boxes */
    d.topo=VK_PRIMITIVE_TOPOLOGY_LINE_LIST; d.blend=1; d.depthWrite=0; d.cull=VK_CULL_MODE_NONE;
    G.pLinesInst = make_pipeline(&d);

    /* non-instanced dynamic lines (sculpt wireframe) */
    d.vs=wv; fill_mesh_vertex_input(&d);
    G.pMeshLine = make_pipeline(&d);

    /* shadow: depth-only, instanced, front-cull + depth bias, 1x samples */
    memset(&d, 0, sizeof d);
    d.vs=sv; d.fs=VK_NULL_HANDLE; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    fill_inst_vertex_input(&d);
    d.cull=VK_CULL_MODE_FRONT_BIT;
    d.cmp=VK_COMPARE_OP_LESS; d.depthTest=1; d.depthWrite=1;
    d.depthBias=1; d.samples=VK_SAMPLE_COUNT_1_BIT;
    G.pShadow = make_pipeline(&d);

    /* UI: NDC quads, blend, depth ALWAYS + write (resets to far) */
    memset(&d, 0, sizeof d);
    d.vs=uv; d.fs=uf; d.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.binds[0] = (VkVertexInputBindingDescription){0, sizeof(UIVert), VK_VERTEX_INPUT_RATE_VERTEX};
    d.attrs[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    d.attrs[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8};
    d.attrCount=2; d.bindCount=1;
    d.blend=1; d.depthTest=1; d.depthWrite=1; d.cmp=VK_COMPARE_OP_ALWAYS;
    d.cull=VK_CULL_MODE_NONE; d.samples=G.samples;
    G.pUI = make_pipeline(&d);

    vkDestroyShaderModule(G.dev, wv, NULL); vkDestroyShaderModule(G.dev, wi, NULL);
    vkDestroyShaderModule(G.dev, sv, NULL); vkDestroyShaderModule(G.dev, wf, NULL);
    vkDestroyShaderModule(G.dev, gf, NULL); vkDestroyShaderModule(G.dev, uv, NULL);
    vkDestroyShaderModule(G.dev, uf, NULL);
}

/* ---------------- swapchain ---------------- */

static VkSurfaceFormatKHR pick_surface_format(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(G.phys, G.surf, &n, NULL);
    VkSurfaceFormatKHR *f = malloc(n * sizeof *f);
    vkGetPhysicalDeviceSurfaceFormatsKHR(G.phys, G.surf, &n, f);
    VkSurfaceFormatKHR pick = f[0];
    for (uint32_t i = 0; i < n; i++)
        if (f[i].format == VK_FORMAT_B8G8R8A8_SRGB) { pick = f[i]; break; }
    free(f);
    return pick;
}

static VkPresentModeKHR pick_present_mode(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(G.phys, G.surf, &n, NULL);
    VkPresentModeKHR *m = malloc(n * sizeof *m);
    vkGetPhysicalDeviceSurfacePresentModesKHR(G.phys, G.surf, &n, m);
    VkPresentModeKHR pick = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++)
        if (m[i] == VK_PRESENT_MODE_MAILBOX_KHR) { pick = m[i]; break; }
    free(m);
    return pick;
}

static void make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect, VkSampleCountFlagBits samples,
                       VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = (VkExtent3D){w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = samples; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(G.dev, &ii, NULL, img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(G.dev, *img, &req);
    *mem = mem_alloc(req.size, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkBindImageMemory(G.dev, *img, *mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    VKC(vkCreateImageView(G.dev, &vi, NULL, view));
}

static void swapchain_destroy(void) {
    for (uint32_t i = 0; i < G.imgCount; i++)
        vkDestroyImageView(G.dev, G.views[i], NULL);
    if (G.msaaView) vkDestroyImageView(G.dev, G.msaaView, NULL);
    if (G.depthView) vkDestroyImageView(G.dev, G.depthView, NULL);
    if (G.msaaImg) { vkDestroyImage(G.dev, G.msaaImg, NULL); vkFreeMemory(G.dev, G.msaaMem, NULL); }
    if (G.depthImg) { vkDestroyImage(G.dev, G.depthImg, NULL); vkFreeMemory(G.dev, G.depthMem, NULL); }
    G.msaaView = G.depthView = VK_NULL_HANDLE;
    G.msaaImg = G.depthImg = VK_NULL_HANDLE;
    if (G.swap) vkDestroySwapchainKHR(G.dev, G.swap, NULL);
    G.swap = VK_NULL_HANDLE;
}

static void swapchain_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(G.phys, G.surf, &caps));
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(G.win, &fw, &fh);
    VkExtent2D ext = { (uint32_t)fw, (uint32_t)fh };
    if (caps.currentExtent.width != UINT32_MAX) ext = caps.currentExtent;
    if (ext.width == 0 || ext.height == 0) { G.ext = ext; return; }
    ext.width  = (uint32_t)clampf((float)ext.width,  (float)caps.minImageExtent.width,  (float)caps.maxImageExtent.width);
    ext.height = (uint32_t)clampf((float)ext.height, (float)caps.minImageExtent.height, (float)caps.maxImageExtent.height);

    VkSurfaceFormatKHR sf = pick_surface_format();
    G.surfFmt = sf.format; G.surfCS = sf.colorSpace;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = G.surf; ci.minImageCount = count;
    ci.imageFormat = G.surfFmt; ci.imageColorSpace = G.surfCS;
    ci.imageExtent = ext; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = pick_present_mode();
    ci.clipped = VK_TRUE;
    VKC(vkCreateSwapchainKHR(G.dev, &ci, NULL, &G.swap));

    vkGetSwapchainImagesKHR(G.dev, G.swap, &G.imgCount, NULL);
    if (G.imgCount > MAX_IMAGES) G.imgCount = MAX_IMAGES;
    vkGetSwapchainImagesKHR(G.dev, G.swap, &G.imgCount, G.imgs);
    for (uint32_t i = 0; i < G.imgCount; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = G.imgs[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = G.surfFmt;
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKC(vkCreateImageView(G.dev, &vi, NULL, &G.views[i]));
    }
    if (G.samples != VK_SAMPLE_COUNT_1_BIT)
        make_image(ext.width, ext.height, G.surfFmt,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, G.samples, &G.msaaImg, &G.msaaMem, &G.msaaView);
    make_image(ext.width, ext.height, VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, G.samples, &G.depthImg, &G.depthMem, &G.depthView);
    G.ext = ext;

    if (G.imgCount > G.renderDoneCount) {
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (uint32_t i = G.renderDoneCount; i < G.imgCount; i++)
            VKC(vkCreateSemaphore(G.dev, &si, NULL, &G.renderDone[i]));
        G.renderDoneCount = G.imgCount;
    }
}

static void swapchain_recreate(void) {
    vkDeviceWaitIdle(G.dev);
    swapchain_destroy();
    swapchain_create();
    G.needResize = 0;
}

/* ---------------- init ---------------- */

static void pick_physical_device(void) {
    uint32_t n = 0;
    VKC(vkEnumeratePhysicalDevices(G.inst, &n, NULL));
    if (!n) { fprintf(stderr, "no vulkan devices\n"); exit(1); }
    VkPhysicalDevice *ds = malloc(n * sizeof *ds);
    VKC(vkEnumeratePhysicalDevices(G.inst, &n, ds));
    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(ds[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ds[i], &qn, NULL);
        VkQueueFamilyProperties *qp = malloc(qn * sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(ds[i], &qn, qp);
        int found = -1;
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 pres = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(ds[i], q, G.surf, &pres);
            if (pres && (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { found = (int)q; break; }
        }
        free(qp);
        if (found < 0) continue;
        int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 10 : 1;
        if (score > bestScore) { bestScore = score; best = ds[i]; G.qfam = (uint32_t)found; }
    }
    free(ds);
    if (!best) { fprintf(stderr, "no suitable device\n"); exit(1); }
    G.phys = best;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(best, &p);
    fprintf(stderr, "GPU: %s\n", p.deviceName);
}

int rk_init(GLFWwindow *win) {
    memset(&G, 0, sizeof G);
    G.win = win;

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Varlera";
    app.apiVersion = VK_API_VERSION_1_3;

    uint32_t extCount = 0;
    const char **exts = glfwGetRequiredInstanceExtensions(&extCount);
    const char *extra[16];
    uint32_t total = extCount;
    for (uint32_t i = 0; i < extCount; i++) extra[i] = exts[i];
#ifndef NDEBUG
    extra[total++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
#endif
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = total; ii.ppEnabledExtensionNames = extra;
#ifndef NDEBUG
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    uint32_t layerN = 0, availN = 0;
    vkEnumerateInstanceLayerProperties(&availN, NULL);
    VkLayerProperties *avail = malloc(availN * sizeof *avail);
    vkEnumerateInstanceLayerProperties(&availN, avail);
    for (uint32_t i = 0; i < availN; i++)
        if (!strcmp(avail[i].layerName, layers[0])) layerN = 1;
    free(avail);
    ii.enabledLayerCount = layerN; ii.ppEnabledLayerNames = layers;
#endif
    VKC(vkCreateInstance(&ii, NULL, &G.inst));
    VKC(glfwCreateWindowSurface(G.inst, win, NULL, &G.surf));
    pick_physical_device();

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = G.qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    const char *devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &f13;
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = devExts;
    VKC(vkCreateDevice(G.phys, &di, NULL, &G.dev));
    vkGetDeviceQueue(G.dev, G.qfam, 0, &G.queue);

    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(G.phys, &pp);
    VkSampleCountFlags sc = pp.limits.framebufferColorSampleCounts & pp.limits.framebufferDepthSampleCounts;
    G.samples = (sc & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;

    swapchain_create();

    /* shadow map + compare sampler */
    make_image(SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_1_BIT,
               &G.shadowImg, &G.shadowMem, &G.shadowView);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VKC(vkCreateSampler(G.dev, &sci, NULL, &G.shadowSamp));

    /* texture atlas image + pixel-art sampler (nearest, clamp) */
    make_image(RK_TEX_ATLAS, RK_TEX_ATLAS, VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT,
               &G.texImg, &G.texMem, &G.texView);
    VkSamplerCreateInfo tsi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    tsi.magFilter = tsi.minFilter = VK_FILTER_NEAREST;
    tsi.addressModeU = tsi.addressModeV = tsi.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(G.dev, &tsi, NULL, &G.texSamp));

    /* descriptors: set0 = ubo/frame, set1 = shadow sampler + tex atlas */
    VkDescriptorSetLayoutBinding b0 = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1; li.pBindings = &b0;
    VKC(vkCreateDescriptorSetLayout(G.dev, &li, NULL, &G.setLayout0));
    VkDescriptorSetLayoutBinding b1[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    li.bindingCount = 2; li.pBindings = b1;
    VKC(vkCreateDescriptorSetLayout(G.dev, &li, NULL, &G.setLayout1));

    VkDescriptorPoolSize psz[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}
    };
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = MAX_FRAMES + 1; pci.poolSizeCount = 2; pci.pPoolSizes = psz;
    VKC(vkCreateDescriptorPool(G.dev, &pci, NULL, &G.pool));

    {
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = G.pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &G.setLayout1;
        VKC(vkAllocateDescriptorSets(G.dev, &dai, &G.set1));
        VkDescriptorImageInfo dii[2] = {
            {G.shadowSamp, G.shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {G.texSamp, G.texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet w[2];
        memset(w, 0, sizeof w);
        for (int i = 0; i < 2; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = G.set1; w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo = &dii[i];
        }
        vkUpdateDescriptorSets(G.dev, 2, w, 0, NULL);
    }

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkDescriptorSetLayout lset[2] = {G.setLayout0, G.setLayout1};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2; pli.pSetLayouts = lset;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(G.dev, &pli, NULL, &G.pipeLayout));

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = G.qfam;
    VKC(vkCreateCommandPool(G.dev, &cpi, NULL, &G.cmdPool));

    for (int i = 0; i < MAX_FRAMES; i++) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = G.cmdPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(G.dev, &cai, &G.fr[i].cmd));
        VkFenceCreateInfo fi2 = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi2.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKC(vkCreateFence(G.dev, &fi2, NULL, &G.fr[i].fence));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(G.dev, &si, NULL, &G.fr[i].acq));
        G.fr[i].ubo = buf_create(sizeof(UBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HOST_MEM,
            &G.fr[i].uboMem, &G.fr[i].uboMap);
        G.fr[i].inst = buf_create(MAX_INST * sizeof(RkInst), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HOST_MEM,
            &G.fr[i].instMem, &G.fr[i].instMap);
        G.fr[i].uib = buf_create(MAX_UI_QUADS * 6 * sizeof(UIVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HOST_MEM,
            &G.fr[i].uibMem, &G.fr[i].uiMap);
        G.fr[i].mesh = buf_create(MAX_DYN_VERTS * sizeof(Vert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HOST_MEM,
            &G.fr[i].meshMem, &G.fr[i].meshMap);
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = G.pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &G.setLayout0;
        VKC(vkAllocateDescriptorSets(G.dev, &dai, &G.fr[i].set0));
        VkDescriptorBufferInfo dbi = {G.fr[i].ubo, 0, sizeof(UBO)};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = G.fr[i].set0; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(G.dev, 1, &w, 0, NULL);
    }

    build_mesh_data();
    upload_vertices();
    make_pipelines();

    /* icon view-projection (reversed-Z ortho to match GREATER compare) */
    {
        m4 v, p;
        v3 eye = {2.2f, 1.9f, 2.2f}, at = {0,0,0}, up = {0,1,0};
        m4_look_at(v, eye, at, up);
        m4_ortho_rz(p, -0.95f, 0.95f, -0.95f, 0.95f, 0.1f, 10.0f);
        m4_mul(G.ubo.iconVP, p, v);
    }
    m4_identity(G.ubo.viewProj);
    m4_identity(G.ubo.shadowVP);
    G.ubo.camPos[0]=0; G.ubo.camPos[1]=10; G.ubo.camPos[2]=10;
    { v3 l = light_dir(); memcpy(G.ubo.lightDir, l.e, sizeof l); }
    G.ubo.fogColor[0]=0.56f; G.ubo.fogColor[1]=0.66f; G.ubo.fogColor[2]=0.82f; G.ubo.fogColor[3]=1;
    G.ubo.fogParams[0]=60.0f; G.ubo.fogParams[1]=170.0f;
    G.ubo.groundCol[0]=0.58f; G.ubo.groundCol[1]=0.60f;
    G.ubo.groundCol[2]=0.64f; G.ubo.groundCol[3]=1;
    return 1;
}

void rk_set_env(const float sky[3], const float ground[3]) {
    if (sky) memcpy(G.ubo.fogColor, sky, 12);
    if (ground) memcpy(G.ubo.groundCol, ground, 12);
}

void rk_shutdown(void) {
    vkDeviceWaitIdle(G.dev);
    for (int i = 0; i < MAX_FRAMES; i++) {
        vkUnmapMemory(G.dev, G.fr[i].uboMem);
        vkUnmapMemory(G.dev, G.fr[i].instMem);
        vkUnmapMemory(G.dev, G.fr[i].uibMem);
        vkDestroyBuffer(G.dev, G.fr[i].ubo, NULL);
        vkDestroyBuffer(G.dev, G.fr[i].inst, NULL);
        vkDestroyBuffer(G.dev, G.fr[i].uib, NULL);
        vkFreeMemory(G.dev, G.fr[i].uboMem, NULL);
        vkFreeMemory(G.dev, G.fr[i].instMem, NULL);
        vkFreeMemory(G.dev, G.fr[i].uibMem, NULL);
        vkUnmapMemory(G.dev, G.fr[i].meshMem);
        vkDestroyBuffer(G.dev, G.fr[i].mesh, NULL);
        vkFreeMemory(G.dev, G.fr[i].meshMem, NULL);
        vkDestroyFence(G.dev, G.fr[i].fence, NULL);
        vkDestroySemaphore(G.dev, G.fr[i].acq, NULL);
    }
    for (uint32_t i = 0; i < G.renderDoneCount; i++)
        vkDestroySemaphore(G.dev, G.renderDone[i], NULL);
    vkDestroyBuffer(G.dev, G.vbuf, NULL);
    vkFreeMemory(G.dev, G.vbufMem, NULL);
    vkDestroyImageView(G.dev, G.shadowView, NULL);
    vkDestroyImage(G.dev, G.shadowImg, NULL);
    vkFreeMemory(G.dev, G.shadowMem, NULL);
    vkDestroySampler(G.dev, G.shadowSamp, NULL);
    vkDestroyImageView(G.dev, G.texView, NULL);
    vkDestroyImage(G.dev, G.texImg, NULL);
    vkFreeMemory(G.dev, G.texMem, NULL);
    vkDestroySampler(G.dev, G.texSamp, NULL);
    vkDestroyPipeline(G.dev, G.pGrid, NULL);
    vkDestroyPipeline(G.dev, G.pWorld, NULL);
    vkDestroyPipeline(G.dev, G.pWorldInst, NULL);
    vkDestroyPipeline(G.dev, G.pGhostInst, NULL);
    vkDestroyPipeline(G.dev, G.pLinesInst, NULL);
    vkDestroyPipeline(G.dev, G.pMeshLine, NULL);
    vkDestroyPipeline(G.dev, G.pShadow, NULL);
    vkDestroyPipeline(G.dev, G.pUI, NULL);
    vkDestroyPipelineLayout(G.dev, G.pipeLayout, NULL);
    vkDestroyDescriptorPool(G.dev, G.pool, NULL);
    vkDestroyDescriptorSetLayout(G.dev, G.setLayout0, NULL);
    vkDestroyDescriptorSetLayout(G.dev, G.setLayout1, NULL);
    vkDestroyCommandPool(G.dev, G.cmdPool, NULL);
    swapchain_destroy();
    vkDestroyDevice(G.dev, NULL);
    vkDestroySurfaceKHR(G.inst, G.surf, NULL);
    vkDestroyInstance(G.inst, NULL);
}

/* ---------------- draw queue ---------------- */

static void qu(int kind, int mesh, uint32_t first, uint32_t count) {
    if (G.qn < MAX_Q)
        G.q[G.qn++] = (QEnt){kind, mesh, first, count, 0,0,0,0, 0,0,0};
}

void rk_draw_grid(void) { qu(K_GRID, 0, 0, 0); }

static uint32_t push_insts(const RkInst *inst, uint32_t n) {
    uint32_t first = (uint32_t)G.instN;
    for (uint32_t i = 0; i < n && G.instN < MAX_INST; i++)
        G.instStage[G.instN++] = inst[i];
    return first;
}

void rk_world(RkMesh m, const RkInst *inst, uint32_t n) {
    qu(K_WORLD, m, push_insts(inst, n), n);
}
void rk_overlay(RkMesh m, const RkInst *inst, uint32_t n) {
    qu(K_OVERLAY, m, push_insts(inst, n), n);
}
void rk_ghost(RkMesh m, const RkInst *inst, uint32_t n) {
    qu(K_GHOST, m, push_insts(inst, n), n);
}
void rk_lines(const RkInst *inst, uint32_t n) {
    qu(K_LINES, 0, push_insts(inst, n), n);
}
void rk_mesh(const Vert *v, uint32_t n, const float model[16]) {
    uint32_t first = (uint32_t)G.meshVn;
    uint32_t room = MAX_DYN_VERTS - (uint32_t)G.meshVn;
    if (n > room) n = room;
    if (!n) return;
    memcpy(G.meshStage + G.meshVn, v, (size_t)n * sizeof(Vert));
    G.meshVn += (int)n;
    if (G.qn < MAX_Q) {
        QEnt *e = &G.q[G.qn++];
        *e = (QEnt){K_MESH, 0, first, n, 0,0,0,0, 0,0,0};
        memcpy(e->mm, model, sizeof e->mm);
    }
}
void rk_meshlines(const Vert *v, uint32_t n, const float model[16],
                  float r, float g, float b, float a) {
    uint32_t first = (uint32_t)G.meshVn;
    uint32_t room = MAX_DYN_VERTS - (uint32_t)G.meshVn;
    if (n > room) n = room;
    if (!n) return;
    memcpy(G.meshStage + G.meshVn, v, (size_t)n * sizeof(Vert));
    G.meshVn += (int)n;
    if (G.qn < MAX_Q) {
        QEnt *e = &G.q[G.qn++];
        *e = (QEnt){K_MLINES, 0, first, n, 0,0,0,0, r,g,b};
        memcpy(e->mm, model, sizeof e->mm);
        e->pw = (int)(a * 1000.0f);      /* alpha rides in pw */
    }
}
void rk_draw_icon(RkMesh m, int px, int py, int pw, int ph, float r, float g, float b) {
    if (G.qn < MAX_Q) {
        QEnt *e = &G.q[G.qn++];
        *e = (QEnt){K_ICON, m, 0, 0, px, py, pw, ph, r, g, b};
    }
}

void rk_ui_rect(float px, float py, float pw, float ph, float r, float g, float b, float a) {
    if (G.uiN < MAX_UI_QUADS)
        G.uiQ[G.uiN++] = (UIRect){px, py, pw, ph, r, g, b, a};
}

void rk_ui_flush(void) {
    /* convert queued rects to staged verts, record a UI queue entry */
    uint32_t first = (uint32_t)G.uiVn;
    int n = 0;
    for (int i = 0; i < G.uiN && G.uiVn + 6 <= MAX_UI_QUADS*6; i++) {
        UIRect *q = &G.uiQ[i];
        float x0 = q->x / G.ext.width * 2.0f - 1.0f;
        float x1 = (q->x + q->w) / G.ext.width * 2.0f - 1.0f;
        float y0 = q->y / G.ext.height * 2.0f - 1.0f;
        float y1 = (q->y + q->h) / G.ext.height * 2.0f - 1.0f;
        float c[4] = {q->r, q->g, q->b, q->a};
        UIVert *o = G.uiStage + G.uiVn;
        o[0].pos[0]=x0; o[0].pos[1]=y0; memcpy(o[0].col,c,16);
        o[1].pos[0]=x1; o[1].pos[1]=y0; memcpy(o[1].col,c,16);
        o[2].pos[0]=x1; o[2].pos[1]=y1; memcpy(o[2].col,c,16);
        o[3].pos[0]=x0; o[3].pos[1]=y0; memcpy(o[3].col,c,16);
        o[4].pos[0]=x1; o[4].pos[1]=y1; memcpy(o[4].col,c,16);
        o[5].pos[0]=x0; o[5].pos[1]=y1; memcpy(o[5].col,c,16);
        G.uiVn += 6; n += 6;
    }
    G.uiN = 0;
    if (n) qu(K_UI, 0, first, (uint32_t)n);
}

/* ---------------- frame ---------------- */

static void img_barrier(VkImage img, VkImageLayout oldL, VkImageLayout newL,
                        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAcc,
                        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAcc,
                        VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAcc;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAcc;
    b.oldLayout = oldL; b.newLayout = newL;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(G.cmd, &dep);
}

int rk_begin_frame(void) {
    if (G.needResize) swapchain_recreate();
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(G.win, &fw, &fh);
    if (fw == 0 || fh == 0) return 0;
    if ((uint32_t)fw != G.ext.width || (uint32_t)fh != G.ext.height)
        swapchain_recreate();
    if (G.ext.width == 0) return 0;

    VKC(vkWaitForFences(G.dev, 1, &G.fr[G.fi].fence, VK_TRUE, UINT64_MAX));
    VKC(vkResetFences(G.dev, 1, &G.fr[G.fi].fence));

    VkResult r = vkAcquireNextImageKHR(G.dev, G.swap, UINT64_MAX, G.fr[G.fi].acq, VK_NULL_HANDLE, &G.imgIdx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { swapchain_recreate(); return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VKC(r);

    G.cmd = G.fr[G.fi].cmd;
    VKC(vkResetCommandBuffer(G.cmd, 0));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(G.cmd, &bbi));

    G.qn = 0; G.instN = 0; G.uiN = 0; G.uiVn = 0; G.meshVn = 0;
    G.t = glfwGetTime();
    G.inFrame = 1;
    return 1;
}

static void bind_sets_and_mesh(void) {
    VkDescriptorSet ds[2] = {G.fr[G.fi].set0, G.set1};
    vkCmdBindDescriptorSets(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipeLayout,
        0, 2, ds, 0, NULL);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(G.cmd, 0, 1, &G.vbuf, &off);
}

/* model meshes live in a separate rebuilt buffer */
static int mesh_span(int mesh, VkDeviceSize *off, uint32_t *cnt) {
    if (mesh >= RK_MESH_BASE_MODEL) {
        int m = mesh - RK_MESH_BASE_MODEL;
        if (m >= G.nmodels) return 0;
        *off = G.modelOff[m]; *cnt = G.modelCount[m];
        return G.mbuf != VK_NULL_HANDLE;
    }
    *off = G.meshOff[mesh]; *cnt = G.meshCount[mesh];
    return 1;
}
static void bind_mesh_buf(int mesh) {
    VkBuffer b = (mesh >= RK_MESH_BASE_MODEL) ? G.mbuf : G.vbuf;
    VkDeviceSize off = 0;
    if (b) vkCmdBindVertexBuffers(G.cmd, 0, 1, &b, &off);
}

static void bind_inst(void) {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(G.cmd, 1, 1, &G.fr[G.fi].inst, &off);
}

static void push_pc(const m4 model, const float color[4], uint32_t flags) {
    Push pc; memcpy(pc.model, model, sizeof(m4));
    memcpy(pc.color, color, sizeof(float)*4); pc.flags = flags;
    vkCmdPushConstants(G.cmd, G.pipeLayout,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
}

void rk_end_frame(void) {
    /* stage instance + ui data for this frame */
    if (G.instN)
        memcpy(G.fr[G.fi].instMap, G.instStage, (size_t)G.instN * sizeof(RkInst));
    if (G.uiVn)
        memcpy(G.fr[G.fi].uiMap, G.uiStage, (size_t)G.uiVn * sizeof(UIVert));
    if (G.meshVn)
        memcpy(G.fr[G.fi].meshMap, G.meshStage, (size_t)G.meshVn * sizeof(Vert));

    /* ---- shadow pass (always runs so the map is in READ layout for sampling) ---- */
    {
        img_barrier(G.shadowImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo dep = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        dep.imageView = G.shadowView; dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        dep.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = (VkRect2D){{0,0},{SHADOW_SIZE, SHADOW_SIZE}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &dep;
        vkCmdBeginRendering(G.cmd, &ri);
        VkViewport vp = {0, 0, SHADOW_SIZE, SHADOW_SIZE, 0.0f, 1.0f};
        vkCmdSetViewport(G.cmd, 0, 1, &vp);
        VkRect2D sc = {{0,0},{SHADOW_SIZE, SHADOW_SIZE}};
        vkCmdSetScissor(G.cmd, 0, 1, &sc);
        vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pShadow);
        bind_sets_and_mesh();
        bind_inst();
        for (int i = 0; i < G.qn; i++) {
            QEnt *e = &G.q[i];
            if (e->kind != K_WORLD) continue;
            VkDeviceSize off; uint32_t cnt;
            if (!mesh_span(e->mesh, &off, &cnt)) continue;
            bind_mesh_buf(e->mesh);
            vkCmdDraw(G.cmd, cnt, e->count, (uint32_t)off, e->first);
        }
        vkCmdEndRendering(G.cmd);
        img_barrier(G.shadowImg, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    /* ---- main pass ---- */
    img_barrier(G.imgs[G.imgIdx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    if (G.msaaImg)
        img_barrier(G.msaaImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    img_barrier(G.depthImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    int msaa = G.samples != VK_SAMPLE_COUNT_1_BIT;
    VkRenderingAttachmentInfo col = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = msaa ? G.msaaView : G.views[G.imgIdx];
    col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (msaa) {
        col.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        col.resolveImageView = G.views[G.imgIdx];
        col.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    col.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    col.clearValue.color.float32[0] = G.ubo.fogColor[0];
    col.clearValue.color.float32[1] = G.ubo.fogColor[1];
    col.clearValue.color.float32[2] = G.ubo.fogColor[2];
    col.clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo dep = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    dep.imageView = G.depthView; dep.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    dep.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; dep.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    dep.clearValue.depthStencil.depth = 0.0f;   /* reversed-Z far */

    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = (VkRect2D){{0,0}, G.ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &col;
    ri.pDepthAttachment = &dep;
    vkCmdBeginRendering(G.cmd, &ri);

    VkViewport fullvp = {0, 0, (float)G.ext.width, (float)G.ext.height, 0.0f, 1.0f};
    VkRect2D fullsc = {{0,0}, G.ext};
    vkCmdSetViewport(G.cmd, 0, 1, &fullvp);
    vkCmdSetScissor(G.cmd, 0, 1, &fullsc);

    for (int i = 0; i < G.qn; i++) {
        QEnt *e = &G.q[i];
        switch (e->kind) {
        case K_GRID: {
            m4 m;
            v3 t = v3_make(snapf(G.ubo.camPos[0], 1.0f), -0.02f, snapf(G.ubo.camPos[2], 1.0f));
            m4_trs(m, t, v3_make(360, 1, 360), 0, 0, 0);
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pGrid);
            bind_sets_and_mesh();
            float white[4] = {1,1,1,1};
            push_pc(m, white, 0);
            vkCmdDraw(G.cmd, G.meshCount[MESH_PLANE], 1, (uint32_t)G.meshOff[MESH_PLANE], 0);
            break; }
        case K_WORLD:
        case K_OVERLAY: {
            VkDeviceSize off; uint32_t cnt;
            if (!mesh_span(e->mesh, &off, &cnt)) break;
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pWorldInst);
            bind_sets_and_mesh();
            bind_mesh_buf(e->mesh);
            bind_inst();
            vkCmdDraw(G.cmd, cnt, e->count, (uint32_t)off, e->first);
            break; }
        case K_GHOST: {
            VkDeviceSize off; uint32_t cnt;
            if (!mesh_span(e->mesh, &off, &cnt)) break;
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pGhostInst);
            bind_sets_and_mesh();
            bind_mesh_buf(e->mesh);
            bind_inst();
            vkCmdDraw(G.cmd, cnt, e->count, (uint32_t)off, e->first);
            break; }
        case K_MESH:
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pWorld);
            bind_sets_and_mesh();
            vkCmdBindVertexBuffers(G.cmd, 0, 1, &G.fr[G.fi].mesh, &(VkDeviceSize){0});
            {   float c[4] = {1,1,1,1};
    push_pc(e->mm, c, RKIF_VCOL | RKIF_TEXM(1)); }
            vkCmdDraw(G.cmd, e->count, 1, e->first, 0);
            break;
        case K_MLINES:
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pMeshLine);
            bind_sets_and_mesh();
            vkCmdBindVertexBuffers(G.cmd, 0, 1, &G.fr[G.fi].mesh, &(VkDeviceSize){0});
            {   float c[4] = {e->cr, e->cg, e->cb, (float)e->pw / 1000.0f};
                push_pc(e->mm, c, RKIF_UNLIT | RKIF_NOFOG); }
            vkCmdDraw(G.cmd, e->count, 1, e->first, 0);
            break;
        case K_LINES:
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pLinesInst);
            bind_sets_and_mesh();
            bind_mesh_buf(0);
            bind_inst();
            vkCmdDraw(G.cmd, G.lineCount, e->count, (uint32_t)G.lineOff, e->first);
            break;
        case K_UI:
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pUI);
            bind_sets_and_mesh();
            vkCmdBindVertexBuffers(G.cmd, 0, 1, &G.fr[G.fi].uib, &(VkDeviceSize){0});
            vkCmdDraw(G.cmd, e->count, 1, e->first, 0);
            break;
        case K_ICON: {
            VkViewport vp = {(float)e->px, (float)e->py, (float)e->pw, (float)e->ph, 0, 1};
            vkCmdSetViewport(G.cmd, 0, 1, &vp);
            VkRect2D sc = {{e->px, e->py}, {(uint32_t)e->pw, (uint32_t)e->ph}};
            vkCmdSetScissor(G.cmd, 0, 1, &sc);
            m4 m;
            m4_trs(m, v3_make(0,0,0), v3_make(0.62f,0.62f,0.62f), -0.55f, (float)G.t*0.6f, 0);
            vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pWorld);
            bind_sets_and_mesh();
            bind_mesh_buf(e->mesh);
            float c[4] = {e->cr, e->cg, e->cb, 1};
            uint32_t icol = e->mesh >= RK_MESH_BASE_MODEL ? RKF_ICON|RKIF_VCOL : RKF_ICON;
            push_pc(m, c, icol);
            VkDeviceSize io; uint32_t ic;
            if (mesh_span(e->mesh, &io, &ic)) vkCmdDraw(G.cmd, ic, 1, (uint32_t)io, 0);
            vkCmdSetViewport(G.cmd, 0, 1, &fullvp);
            vkCmdSetScissor(G.cmd, 0, 1, &fullsc);
            break; }
        }
    }

    vkCmdEndRendering(G.cmd);
    img_barrier(G.imgs[G.imgIdx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT);
    VKC(vkEndCommandBuffer(G.cmd));

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &G.fr[G.fi].acq;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &G.cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &G.renderDone[G.imgIdx];
    VKC(vkQueueSubmit(G.queue, 1, &si, G.fr[G.fi].fence));

    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &G.renderDone[G.imgIdx];
    pi.swapchainCount = 1; pi.pSwapchains = &G.swap;
    pi.pImageIndices = &G.imgIdx;
    VkResult r = vkQueuePresentKHR(G.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) G.needResize = 1;
    else if (r != VK_SUCCESS) VKC(r);

    G.fi = (G.fi + 1) % MAX_FRAMES;
    G.inFrame = 0;
}

static v3 light_dir(void) { return v3_norm(v3_make(0.72f, 0.55f, 0.30f)); }

void rk_set_camera(const float vp[16], const float eye[3], const float focus[3]) {
    memcpy(G.ubo.viewProj, vp, sizeof(m4));
    memcpy(G.ubo.camPos, eye, sizeof(float)*3);
    G.ubo.camPos[3] = (float)G.t;          /* w = time (anim/spin) */
    memcpy(&G.focus, focus, sizeof(v3));

    /* directional shadow ortho, centered on snapped focus for stability */
    v3 f = v3_make(snapf(focus[0], 2.0f), focus[1], snapf(focus[2], 2.0f));
    v3 l = light_dir();
    v3 le = v3_add(f, v3_scale(l, 100.0f));   /* eye on the light side, above scene */
    m4 v, p;
    m4_look_at(v, le, f, v3_make(0, 1, 0));
    float R = 60.0f;
    m4_ortho(p, -R, R, -R, R, 1.0f, 220.0f);
    m4_mul(G.ubo.shadowVP, p, v);

    memcpy(G.fr[G.fi].uboMap, &G.ubo, sizeof(UBO));
}

void rk_extent(int *w, int *h) { *w = (int)G.ext.width; *h = (int)G.ext.height; }
