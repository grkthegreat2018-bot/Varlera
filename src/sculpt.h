/* sculpt.h - modal vertex-drag mesh editor -> welded topo custom model. */
#ifndef SCULPT_H
#define SCULPT_H
#include "app.h"
#include "rk.h"
#include <stdint.h>

#define SC_N 32                     /* legacy density grid (save migration) */
#define SC_VOX_N (SC_N*SC_N*SC_N)
#define SC_MAX_VERTS (192*1024)     /* legacy field bake cap */
#define SC_MAX_V 512                /* sculpt mesh caps (keep = model.h) */
#define SC_MAX_F 1024

void sculpt_open(void);           /* new cube mesh */
void sculpt_open_model(int mid);  /* re-edit an existing sculpted model */
void sculpt_draw(void);            /* call instead of panel_draw while modal */
int  sculpt_press(int mx, int my); /* LMB; 1 = consumed */
void sculpt_rmb_click(int mx, int my); /* RMB click: split nearest edge */
void sculpt_release(void);
void sculpt_move(int mx, int my);
void sculpt_wheel(double dy);
int  sculpt_key(int k);            /* always consumes while modal */
void sculpt_char(unsigned cp);     /* name-field typing */

/* legacy field <-> persisted voxel payloads (models.bw <= v5) */
void sculpt_vox_unpack(const uint8_t (*vox4)[4], float *den, uint8_t (*col)[3]);
void sculpt_vox_unpack16(const uint16_t *vox2, float *den, uint8_t (*col)[3]);
int  sculpt_bake_verts(const float *den, const uint8_t (*col)[3], int size,
                       Vert *out, int max);           /* -> vert count */

#endif
