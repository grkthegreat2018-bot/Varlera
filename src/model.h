/* model.h - fused-block custom models: bake, store, save/load, unfuse. */
#ifndef MODEL_H
#define MODEL_H

#include "world.h"
#include "rk.h"

#define MODEL_MAX_CHILDREN 128
#define MODEL_VOX_N 4096            /* 16^3 sculpted voxel field */

typedef struct {
    char name[16];
    int nch;                        /* 0 => sculpted voxel model (vox field) */
    Block ch[MODEL_MAX_CHILDREN];   /* positions relative to pivot */
    v3 size;                        /* bounding box */
    int mesh;                       /* RK_MESH_BASE_MODEL + i, or -1 */
    int vox;                        /* 1 = sculpted: bake from voxd, not ch */
    uint16_t voxd[MODEL_VOX_N];     /* rgb565|0x8000 per voxel */
} Model;

extern Model g_models[RK_MAX_MODELS];
extern int g_nmodels;

void models_init(void);                             /* load models.bw if present */
int  model_bake(const Block *children, int n);      /* returns model idx or -1 */
int  model_sculpt(const uint16_t *vox, int size);   /* voxel model -> idx or -1 */
void model_delete(int mid);   /* unfuse all world instances, then remove */
int  models_save(const char *path);
int  models_load(const char *path);

#endif
