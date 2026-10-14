/* export.h - world geometry export formats. */
#ifndef EXPORT_H
#define EXPORT_H

/* dump every block to Wavefront OBJ: "v x y z r g b" vertex colors
 * (Blender-style extension), vn normals, o blk<id> groups. Returns 1. */
int world_export_obj(const char *path);

#endif
