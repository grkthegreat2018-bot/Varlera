/* decimate.h - quadric-error edge-collapse mesh simplification. */
#ifndef DECIMATE_H
#define DECIMATE_H
#include <stdint.h>

/* Collapse `*nvio/nfc` toward targetF faces. Vert attribute arrays (col,
 * tex, lock) follow the survivor; dead verts are compacted out and *nvio
 * is updated. Returns the new face count (<= targetF when possible;
 * degenerate faces are dropped as they form).
 * vx:  verts (float[3])   col/tex/lock: per-vert attributes or NULL
 * fc:  faces (uint16[3])  */
int mesh_decimate(float (*vx)[3], uint8_t (*col)[3], int8_t *tex,
                  uint8_t *lock, uint16_t (*fc)[3], int *nvio, int nfc,
                  int targetF);

#endif
