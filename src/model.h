/* model.h - fused-block custom models: bake, store, save/load, unfuse. */
#ifndef MODEL_H
#define MODEL_H

#include "world.h"
#include "rk.h"

#define MODEL_MAX_CHILDREN 128

typedef struct {
    char name[16];
    int nch;
    Block ch[MODEL_MAX_CHILDREN];   /* positions relative to pivot */
    v3 size;                        /* bounding box */
    int mesh;                       /* RK_MESH_BASE_MODEL + i, or -1 */
} Model;

extern Model g_models[RK_MAX_MODELS];
extern int g_nmodels;

void models_init(void);                             /* load models.bw if present */
int  model_bake(const Block *children, int n);      /* returns model idx or -1 */
void model_delete(int mid);   /* unfuse all world instances, then remove */
int  models_save(const char *path);
int  models_load(const char *path);

#endif
