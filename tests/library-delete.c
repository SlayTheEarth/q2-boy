// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/library_paths.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static void make_file(const char *dir, const char *name) {
    char path[PATH_CAP];
    assert(join_path(path, dir, name));
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0);
    close(fd);
}

static bool exists(const char *dir, const char *name) {
    char path[PATH_CAP];
    assert(join_path(path, dir, name));
    return !access(path, F_OK);
}

int main(void) {
    char dir[] = "delete-test-XXXXXX";
    LibraryPaths paths;
    assert(mkdtemp(dir));
    assert(library_prepare(&paths, dir));
    const char *roms = paths.system_roms[SYSTEM_GBA];
    const char *saves = paths.system_saves[SYSTEM_GBA];
    make_file(roms, "Sample.gba");
    make_file(roms, "Sample.png");
    make_file(saves, "Sample.gba.srm");
    make_file(saves, "Sample.gba.rtc");
    make_file(saves, "Sample.gba.state10");
    make_file(saves, "Sample.gba.latest-state");
    make_file(paths.saves, "Sample.gba.srm");
    make_file(saves, "Other.gba.srm");
    assert(library_remove_rom(&paths, "Sample.gba", false, false, 10) == ROM_REMOVED);
    assert(!exists(roms, "Sample.gba") && !exists(roms, "Sample.png"));
    assert(exists(saves, "Sample.gba.srm") && exists(saves, "Sample.gba.state10"));
    assert(exists(paths.saves, "Sample.gba.srm"));
    assert(library_remove_rom(&paths, "Sample.gba", false, true, 10) == ROM_REMOVE_FAILED);
    assert(exists(saves, "Sample.gba.srm"));
    make_file(roms, "Sample.gba");
    assert(library_remove_rom(&paths, "Sample.gba", false, true, 10) == ROM_REMOVED);
    assert(!exists(saves, "Sample.gba.srm") && !exists(saves, "Sample.gba.rtc"));
    assert(!exists(saves, "Sample.gba.state10") && !exists(saves, "Sample.gba.latest-state"));
    assert(!exists(paths.saves, "Sample.gba.srm") && exists(saves, "Other.gba.srm"));

    make_file(paths.roms, "Legacy.gb");
    make_file(paths.roms, "Legacy.gbc");
    make_file(paths.roms, "Legacy.png");
    make_file(paths.saves, "Legacy.gb.srm");
    assert(library_remove_rom(&paths, "Legacy.gb", true, true, 10) == ROM_REMOVED);
    assert(exists(paths.roms, "Legacy.png") && exists(paths.roms, "Legacy.gbc"));
    assert(!exists(paths.saves, "Legacy.gb.srm"));

    make_file(roms, "Duplicate.gba");
    make_file(paths.roms, "Duplicate.gba");
    make_file(saves, "Duplicate.gba.srm");
    assert(library_remove_rom(&paths, "Duplicate.gba", false, true, 10) == ROM_REMOVE_INCOMPLETE);
    assert(exists(paths.roms, "Duplicate.gba") && exists(saves, "Duplicate.gba.srm"));
    assert(library_remove_rom(&paths, "../Other.gba", false, true, 10) == ROM_REMOVE_FAILED);
}
