/* model.h - custom models: fused block bakes + sculpted vertex meshes. */
#ifndef MODEL_H
#define MODEL_H

#include "world.h"
#include "rk.h"
#include <stdint.h>

#define MODEL_MAX_CHILDREN 128
#define MODEL_MAX_VERTS  512        /* sculpt mesh caps */
#define MODEL_MAX_FACES  1024

typedef struct {
    char name[16];
    int nch;                        /* >0: fused children; 0+ */
    Block ch[MODEL_MAX_CHILDREN];   /* positions relative to pivot */
    v3 size;                        /* bounding box in blocks */
    int mesh;                       /* RK_MESH_BASE_MODEL + i, or -1 */
    int vox;                        /* 1 = sculpted topo mesh (editable) */
    int adv;                        /* 1 = premade asset (PROPS tab) */
    /* sculpted topology (verts centered on bbox center, block units) */
    int      nvert, nface;
    float    mv[MODEL_MAX_VERTS][3];
    uint8_t  mvcol[MODEL_MAX_VERTS][3];
    int8_t   mvtex[MODEL_MAX_VERTS];  /* per-vert material id, -1 = inherit */
    uint8_t  mvlock[MODEL_MAX_VERTS]; /* material layer locked in sculpt */
    uint16_t mf[MODEL_MAX_FACES][3];
} Model;

extern Model g_models[RK_MAX_MODELS];
extern int g_nmodels;

void models_init(void);                                  /* load models.bw if present */
int  model_bake(const Block *children, int n);           /* returns model idx or -1 */
/* transient mesh bake — no Model record (play-mode greedy clusters);
   abs -> pivot/size derived from the group AABB. returns mesh id or -1 */
int  model_bake_transient(const Block *abs, int n, v3 *pivot, v3 *size);
int  model_sculpt_topo(const float (*vx)[3], const uint8_t (*vc)[3],
                       const int8_t *vt, const uint8_t *vl,
                       const uint16_t (*fc)[3], int nv, int nf,
                       const char *name);
void models_bake_builtins(void);    /* premade PROPS-tab assets (first run) */
void model_delete(int mid);   /* unfuse all world instances, then remove */
int  models_save(const char *path);
int  models_load(const char *path);
void models_rebake_all(void);       /* rebuild all GPU meshes (after edits) */

#endif
