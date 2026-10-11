/* bundle.h - .bwx portable project bundle (world + models + textures). */
#ifndef BUNDLE_H
#define BUNDLE_H

int bundle_save(const char *path, const char *worldName);
int bundle_load(const char *path, char *nameOut /*32, may be NULL*/);

#endif
