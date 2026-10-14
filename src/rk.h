/* rk.h - Vulkan renderer for Varlera.
 * Per-frame: rk_begin_frame() -> rk_set_camera() -> push draws -> rk_end_frame().
 * Draw calls are queued and recorded on end_frame (shadow pass replays world draws).
 */
#ifndef RK_H
#define RK_H

#include <stdint.h>
#include <GLFW/glfw3.h>
#include "math3d.h"

typedef enum {
    RK_MESH_CUBE = 0,
    RK_MESH_WEDGE,
    RK_MESH_CYLINDER,
    RK_MESH_SPHERE,
    RK_MESH_CONE,
    RK_MESH_WHEEL,      /* cylinder lying on its side */
    RK_MESH_TORUS,
    RK_MESH_DOME,       /* hemisphere on a base disc */
    RK_MESH_PYRAMID,    /* 4-sided cone, apex up */
    RK_MESH_CORNERW,    /* corner wedge: slopes down from one high corner */
    RK_MESH_STAIR,      /* 3-step staircase, ascends toward -z */
    RK_MESH_FRAME,      /* hollow cube: 12 edge beams only */
    RK_MESH_ROUNDC,     /* rounded-edge cube (superellipsoid) */
    RK_MESH_CAGEBALL,   /* birdcage sphere: meridian rib tubes + lat rings */
    RK_MESH_CORSTAIR,   /* L-shaped corner staircase */
    RK_MESH_ARCH,       /* box arch: two legs + lintel, opening in z */
    RK_MESH_RING,       /* standing torus (ring in the XY plane) */
    RK_MESH_CAP,        /* capsule: cylinder with hemispherical caps */
    RK_MESH_TUBE,       /* hollow pipe, open ends */
    RK_MESH_ELBOW,      /* quarter-bend pipe (XY plane) */
    RK_MESH_GABLE,      /* triangular roof prism */
    RK_MESH_CROSS,      /* plus-shaped prism */
    RK_MESH_HWEDGE,     /* half-height wedge */
    RK_MESH_IWEDGE,     /* inverted (ceiling) wedge */
    RK_MESH_COUNT
} RkMesh;

/* baked custom models live at mesh ids [RK_MESH_BASE_MODEL .. +64).
 * 32 (not RK_MESH_COUNT): leaves 17-31 free for future built-ins without
 * shifting model ids — v<6 world saves remap old ids 16-79 up by 16. */
#define RK_MESH_BASE_MODEL 32
#define RK_MAX_MODELS     64

/* flags layout (both instanced + push-const paths):
 * bit0=unlit, bits1-2=rot90*k, bit3=no fog, bit4=iconVP (push path),
 * bits5-10=texture id (0=none), bits11-12=tex mode, bit13=tex override,
 * bit14=use vertex color, bit15=spin in play */
#define RKIF_UNLIT  1u
#define RKIF_ROT0   0u
#define RKIF_ROT90  2u
#define RKIF_ROT180 4u
#define RKIF_ROT270 6u
#define RKIF_NOFOG  8u
#define RKF_ICON    16u
#define RKIF_TEX(id)  ((uint32_t)((id) & 63u) << 5)
#define RKIF_TEXM(m)  ((uint32_t)((m) & 3u) << 11)
#define RKIF_TEXOVR   (1u << 13)
#define RKIF_VCOL   (1u << 14)
#define RKIF_SPIN   (1u << 15)
#define RKIF_MAT(m) ((uint32_t)((m) & 3u) << 17)

/* Block.texm packing: bits0-1 mode, bit2 override, bits3-4 material */
#define TEXM_STRETCH 0
#define TEXM_LOOP    1
#define TEXM_PROC    2
#define TEXM_ANIM    3              /* sheet tiles = flipbook frames */
#define TEXM_OVR     4
#define TEXM_MAT(m)  ((uint8_t)((m) & 3) << 3)
#define TEXM_MATTE   0
#define TEXM_GLOSS   1
#define TEXM_METAL   2
#define TEXM_GLOW    3

typedef struct {
    float pos[3];
    float scale[3];
    uint32_t color;             /* RGBA8 packed: r|g<<8|b<<16|a<<24 */
    uint32_t flags;
} RkInst;

static inline uint32_t rk_rgba(float r, float g, float b, float a) {
    return (uint32_t)(r*255.0f) | ((uint32_t)(g*255.0f)<<8) |
           ((uint32_t)(b*255.0f)<<16) | ((uint32_t)(a*255.0f)<<24);
}

int  rk_init(GLFWwindow *win);
void rk_shutdown(void);

int  rk_begin_frame(void);            /* 0 = skip frame (minimized/resized) */
void rk_end_frame(void);

/* vp: camera view-proj, eye: camera pos, focus: point shadows/light center on */
void rk_set_camera(const float vp[16], const float eye[3], const float focus[3]);

void rk_draw_sky(void);               /* gradient dome + sun (draw first) */
v3   rk_light_dir(void);              /* normalized direction toward sun */
void rk_draw_grid(void);
void rk_world(RkMesh m, const RkInst *inst, uint32_t n);    /* lit, casts shadow  */
void rk_caster(RkMesh m, const RkInst *inst, uint32_t n);   /* shadow pass only  */
/* conservative: can this world AABB (center, half-extents) cast a shadow
   into the current shadow map? (ortho rect extruded along the light dir) */
int  rk_can_shadow_cast(v3 c, v3 half);
void rk_overlay(RkMesh m, const RkInst *inst, uint32_t n);  /* no shadow cast     */
void rk_ghost(RkMesh m, const RkInst *inst, uint32_t n);    /* alpha blended      */
void rk_lines(const RkInst *inst, uint32_t n);              /* unit-edge boxes    */

void rk_draw_icon(RkMesh m, int px, int py, int pw, int ph,
                  float r, float g, float b, const float size[3]);

/* --- baked custom-model meshes (fused blocks) ---
 * meshgen_shape (meshgen.h): emit a built-in unit mesh into caller memory.
 * rk_model_mesh_add: append baked verts, returns mesh id (RK_MESH_BASE_MODEL+i).
 * rk_models_commit: (re)uploads the model vertex buffer — call after adds. */
/* tex: per-vertex atlas material id (-1 = use the instance/block tex).
   verts sharing a tex id shade seamless across touching faces (box
   projection is continuous where verts coincide). */
typedef struct { float pos[3], nrm[3], col[3], tex; } Vert;
int  rk_model_mesh_add(const Vert *v, int n);
int  rk_model_mesh_add_top(const Vert *v, int n);   /* transient slot (tail) */
void rk_models_drop_transient(void);                /* free tail slots, no GPU work */
void rk_models_commit(void);
void rk_models_reset(void);     /* drop all baked verts (for rebuild) */

/* stream an arbitrary vertex-colored mesh for this frame only (sculpt
   preview); drawn lit with `model` transform, no shadow cast */
void rk_mesh(const Vert *v, uint32_t n, const float model[16]);
/* same, but a line list with a uniform color (wireframe overlay) */
void rk_meshlines(const Vert *v, uint32_t n, const float model[16],
                  float r, float g, float b, float a);

/* --- texture atlas (tex.c owns contents) --- */
#define RK_TEX_ATLAS 1024          /* atlas is 1024x1024 RGBA */
#define RK_TEX_TILE  128           /* tile edge in px */
#define RK_TEX_MAX   64            /* texture records (UBO table size) */
void rk_tex_upload(const uint8_t *rgba);           /* push whole atlas */
void rk_tex_rect(int id, float u0, float v0, float tw, float th); /* tiles */
void rk_tex_fps(int id, float fps);   /* flipbook rate for TEXM_ANIM */

void rk_ui_rect(float px, float py, float pw, float ph, float r, float g, float b, float a);
/* textured UI quad sampling atlas region [u0,v0]-[u1,v1] */
void rk_ui_texrect(float px, float py, float pw, float ph,
                   float u0, float v0, float u1, float v1);
/* same, tinted (texture rgb*a multiplied by r,g,b,a) */
void rk_ui_texrect_col(float px, float py, float pw, float ph,
                       float r, float g, float b, float a,
                       float u0, float v0, float u1, float v1);
void rk_ui_flush(void);

/* environment look: sky tints clear+fog, ground tints the grid base color.
 * gstyle: 0 grid, 1 checker, 2 dots, 3 plain (grid.frag pattern select) */
void rk_set_env(const float sky[3], const float ground[3], int gstyle);
void rk_set_fog(float near_, float far_);   /* world-set override (60/170 def) */
void rk_set_bloom(float amt);               /* bloom mix strength (def 0.55) */
void rk_set_light(v3 d);                    /* dynamic sun/moon direction */
void rk_set_day(float k);                   /* 0..1 daylight (env color mul) */
int  rk_queue_insts(int kind);              /* per-kind insts last frame */
void rk_pool_usage(int *inst, int *instCap, int *dynVerts, int *dynCap,
                   int *mverts, int *mvCap);

void rk_extent(int *w, int *h);
void rk_screenshot(const char *path);       /* .bmp captured after present */
int  rk_reload_shaders(void);               /* F8: glslc + pipeline rebuild */

/* --- profiling --- */
typedef struct { int draws, insts, uiVerts, meshVerts, models; } RkStats;
void rk_query_stats(RkStats *s);
int  rk_gpu_times(float out[4]);   /* ms: shadow, main, post, total. 0 = none */

#endif /* RK_H */
