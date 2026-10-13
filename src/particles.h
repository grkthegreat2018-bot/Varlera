/* particles.h - fixed-pool world-space particles. Sim is CPU; draw packs
 * RkInst batches through rk_ghost (alpha blend, depth-tested, no shadow
 * cast) — glow presets ride the bloom chain via RKIF_MAT(TEXM_GLOW). */
#ifndef PARTICLES_H
#define PARTICLES_H

#include "math3d.h"

enum { P_SMOKE, P_FIRE, P_SPARK, P_DUST, P_PUFF, P_DEBRIS, P_N };

void particles_reset(void);
void particles_update(float dt);
void particles_draw(void);            /* queue insts — call before rk_ui_flush */

/* burst n particles of `preset` at `at`; tint NULL -> preset default */
void particles_burst(int preset, v3 at, const float tint[3], int n);
/* same, biased along dir (thruster exhaust) */
void particles_burst_dir(int preset, v3 at, v3 dir, const float tint[3], int n);

#endif
