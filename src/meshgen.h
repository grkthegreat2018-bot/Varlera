/* meshgen.h - CPU generation of the built-in unit-box shape meshes.
 * Emits flat Vert soups (pos/nrm/col=1/tex=-1) into caller memory. */
#ifndef MESHGEN_H
#define MESHGEN_H

#include "rk.h"
#include <stdint.h>

/* internal mesh emitted after the public RK_MESH_* shapes: the grid plane */
#define MESHGEN_PLANE RK_MESH_COUNT
#define MESHGEN_TOTAL (RK_MESH_COUNT + 1)
#define MESHGEN_MAX_VERTS 24576

/* emit one built-in shape into `dst`; returns vert count (0 on bad id) */
int meshgen_shape(int shape, Vert *dst, int maxverts);

/* emit ALL built-in meshes + the unit wireframe box into `pool`.
 * Fills off[i]/count[i] for i in [0,MESHGEN_TOTAL), plus the line-box
 * span; returns total verts written. */
int meshgen_build_all(Vert *pool, int maxverts,
                      uint32_t *off, uint32_t *count,
                      uint32_t *lineOff, uint32_t *lineCount);

#endif
