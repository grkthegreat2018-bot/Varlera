/* bundle.h - .bwx portable project bundle (world + models + textures). */
#ifndef BUNDLE_H
#define BUNDLE_H

#include <stdint.h>

/* provenance part record (v2 bundles): who/what produced the file */
typedef struct {
    char     world[32];
    char     creator[32];
    uint32_t created, modified;
    char     note[64];
} BxProv;

int bundle_save(const char *path, const char *worldName);
int bundle_load(const char *path, char *nameOut /*32, may be NULL*/);
const BxProv *bundle_prov(void);         /* NULL when the bundle had none */

#endif
