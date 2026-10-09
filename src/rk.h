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
    RK_MESH_COUNT
} RkMesh;

/* baked custom models live at mesh ids [RK_MESH_BASE_MODEL .. +64) */
#define RK_MESH_BASE_MODEL 16
#define RK_MAX_MODELS     64

/* flags layout (both instanced + push-const paths):
 * bit0=unlit, bits1-2=rot90*k, bit3=no fog, bit4=iconVP (push path),
 * bits5-8=texture pattern id, bit9=use vertex color, bit10=spin in play */
#define RKIF_UNLIT  1u
#define RKIF_ROT0   0u
#define RKIF_ROT90  2u
#define RKIF_ROT180 4u
#define RKIF_ROT270 6u
#define RKIF_NOFOG  8u
#define RKF_ICON    16u
#define RKIF_TEX(id)  ((uint32_t)((id) & 15) << 5)
#define RKIF_VCOL   (1u << 9)
#define RKIF_SPIN   (1u << 10)

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

void rk_draw_grid(void);
void rk_world(RkMesh m, const RkInst *inst, uint32_t n);    /* lit, casts shadow  */
void rk_overlay(RkMesh m, const RkInst *inst, uint32_t n);  /* no shadow cast     */
void rk_ghost(RkMesh m, const RkInst *inst, uint32_t n);    /* alpha blended      */
void rk_lines(const RkInst *inst, uint32_t n);              /* unit-edge boxes    */

void rk_draw_icon(RkMesh m, int px, int py, int pw, int ph, float r, float g, float b);

/* --- baked custom-model meshes (fused blocks) ---
 * rk_gen_shape: emit a built-in unit mesh into caller memory (CPU bake).
 * rk_model_mesh_add: append baked verts, returns mesh id (RK_MESH_BASE_MODEL+i).
 * rk_models_commit: (re)uploads the model vertex buffer — call after adds. */
typedef struct { float pos[3], nrm[3], col[3]; } Vert;
int  rk_gen_shape(int shape, Vert *dst, int maxverts);
int  rk_model_mesh_add(const Vert *v, int n);
void rk_models_commit(void);
void rk_models_reset(void);     /* drop all baked verts (for rebuild) */

void rk_ui_rect(float px, float py, float pw, float ph, float r, float g, float b, float a);
void rk_ui_flush(void);

void rk_extent(int *w, int *h);

#endif /* RK_H */
