/* camera.h - free-fly editor camera. */
#ifndef CAMERA_H
#define CAMERA_H

#include "math3d.h"

typedef struct { v3 f, r, u; } CamBasis;

void cam_init(void);
void cam_frame(float dt);          /* WASD/QE movement via held keys */
void cam_look(float dx, float dy); /* RMB drag look */
void cam_pan(float dx, float dy);  /* MMB drag pan */
void cam_speed_step(float dy);     /* ctrl+wheel: fly speed */
void cam_zoom_cursor(float dy, double mx, double my); /* wheel: zoom to cursor */
void cam_focus_sel(void);          /* F: fly to selection centroid */
void cam_update_vp(void);          /* recompute A.viewProj */
CamBasis cam_basis(void);
void cam_ray(double mx, double my, v3 *ro, v3 *rd); /* world ray through pixel */

#endif
