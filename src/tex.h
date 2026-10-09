/* tex.h - texture atlas registry. Textures are rects of 128px tiles inside a
 * 1024px RGBA atlas. A 1x1 texture is a plain image; a 3x3 texture is a
 * procedural-variation sheet (PROC mode picks a random sub-tile per unit cell).
 * tex.c owns the CPU copy of the pixels; rk_tex_upload() pushes to the GPU.
 */
#ifndef TEX_H
#define TEX_H

#include <stdint.h>
#include "rk.h"

#define TEX_MAX    RK_TEX_MAX
#define TEX_TILE   RK_TEX_TILE

typedef struct {
    char     name[16];
    uint8_t  tx, ty;        /* atlas tile coords */
    uint8_t  tw, th;        /* size in tiles (1 = plain, 3 = proc sheet) */
} Tex;

extern Tex     g_tex[TEX_MAX];
extern int     g_ntex;                          /* slot 0 = "none" */
extern uint8_t g_atlas[RK_TEX_ATLAS * RK_TEX_ATLAS * 4];

void     tex_init(void);                        /* bakes built-ins, loads file */
int      tex_new(const char *name, int tw, int th); /* -> id or -1 (atlas full) */
void     tex_delete(int id);
uint8_t *tex_px(int id, int *w, int *h);        /* edit pointer (atlas stride!) */
void     tex_commit(int id);                    /* push rect data to GPU table + upload */
int      tex_save(const char *path);
int      tex_load(const char *path);

static inline int tex_span(const Tex *t) { return t->tw; }   /* square sheets */

#endif
