// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q2_BOY_LIBRARY_PATHS_H
#define Q2_BOY_LIBRARY_PATHS_H

#include "cover.h"

#define PATH_CAP 700

typedef struct {
    char roms[PATH_CAP], saves[PATH_CAP];
    char system_roms[SYSTEM_COUNT][PATH_CAP], system_saves[SYSTEM_COUNT][PATH_CAP];
} LibraryPaths;

enum CartridgeSystem rom_system(const char *path);
bool join_path(char path[PATH_CAP], const char *directory, const char *name);
bool library_prepare(LibraryPaths *paths, const char *data_dir);
void library_migrate(const LibraryPaths *paths);

#endif
