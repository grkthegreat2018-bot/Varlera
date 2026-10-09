/* sculpt.h - modal voxel sculpting: field -> marching-tets custom model. */
#ifndef SCULPT_H
#define SCULPT_H
#include "app.h"
#include "rk.h"
#include <stdint.h>

#define SC_N 16                    /* voxels per edge */

void sculpt_open(void);
void sculpt_draw(void);            /* call instead of panel_draw while modal */
int  sculpt_press(int mx, int my); /* LMB; 1 = consumed */
void sculpt_release(void);
void sculpt_move(int mx, int my);
void sculpt_wheel(double dy);
int  sculpt_key(int k);            /* always consumes while modal */
int  sculpt_bake_mesh(const uint16_t *vox, int size); /* -> mesh id or -1 */

#endif
