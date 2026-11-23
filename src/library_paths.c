// SPDX-License-Identifier: GPL-2.0-or-later
#include "library_paths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static const char *const system_dirs[] = {"gb", "gbc", "gba", "nes", "snes"};

enum CartridgeSystem rom_system(const char *path) {
    const char *extension = strrchr(path, '.');
    if (!extension) return SYSTEM_COUNT;
    if (!strcasecmp(extension, ".gb")) return SYSTEM_GB;
    if (!strcasecmp(extension, ".gbc")) return SYSTEM_GBC;
    if (!strcasecmp(extension, ".gba")) return SYSTEM_GBA;
    if (!strcasecmp(extension, ".nes")) return SYSTEM_NES;
    if (!strcasecmp(extension, ".sfc") || !strcasecmp(extension, ".smc")) return SYSTEM_SNES;
    return SYSTEM_COUNT;
}

bool join_path(char path[PATH_CAP], const char *directory, const char *name) {
    int length = snprintf(path, PATH_CAP, "%s/%s", directory, name);
    return length >= 0 && length < PATH_CAP;
}

static bool make_dir(const char *path) {
    struct stat64 st;
    return !mkdir(path, 0755) || (!stat64(path, &st) && S_ISDIR(st.st_mode));
}

bool library_prepare(LibraryPaths *paths, const char *data_dir) {
    if (!join_path(paths->roms, data_dir, "ROMs") || !join_path(paths->saves, data_dir, "Saves") ||
        !make_dir(data_dir) || !make_dir(paths->roms) || !make_dir(paths->saves)) return false;
    for (int i = 0; i < SYSTEM_COUNT; ++i) {
        if (!join_path(paths->system_roms[i], paths->roms, system_dirs[i]) ||
            !join_path(paths->system_saves[i], paths->saves, system_dirs[i])) return false;
        if (!make_dir(paths->system_roms[i])) fprintf(stderr, "Could not create %s\n", paths->system_roms[i]);
        if (!make_dir(paths->system_saves[i])) fprintf(stderr, "Could not create %s\n", paths->system_saves[i]);
    }
    return true;
}

static bool regular_file(const char *path) {
    struct stat64 st;
    return !lstat64(path, &st) && S_ISREG(st.st_mode);
}

static bool path_absent(const char *path) {
    struct stat64 st;
    return lstat64(path, &st) && errno == ENOENT;
}

static bool move_into_place(const char *source, const char *destination) {
    /* FAT on older Q2 firmware can lack RENAME_NOREPLACE. The fallback
     * checks for an existing entry while the migration lock is held. */
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, AT_FDCWD, source, AT_FDCWD, destination, RENAME_NOREPLACE) == 0) return true;
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return false;
#endif
    return path_absent(destination) && !rename(source, destination);
}

static bool copy_into_place(const char *source, const char *directory, const char *name) {
    char destination[PATH_CAP], temp[PATH_CAP];
    if (!join_path(destination, directory, name) || !path_absent(destination) ||
        !join_path(temp, directory, ".q2-boy-import-XXXXXX")) return false;
    FILE *input = fopen(source, "rb");
    if (!input) return false;
    struct stat64 st;
    if (fstat64(fileno(input), &st) || !S_ISREG(st.st_mode)) { fclose(input); return false; }
    int fd = mkstemp(temp);
    FILE *output = fd < 0 ? NULL : fdopen(fd, "wb");
    bool ok = output != NULL;
    if (output) {
        char buffer[8192];
        size_t count;
        while ((count = fread(buffer, 1, sizeof buffer, input)) != 0) {
            if (fwrite(buffer, 1, count, output) != count) { ok = false; break; }
        }
        ok = ok && !ferror(input) && !fflush(output);
        struct timespec times[] = {st.st_atim, st.st_mtim};
        if (ok) (void)futimens(fd, times);
        ok = ok && !fsync(fd);
        if (fclose(output)) ok = false;
    } else if (fd >= 0) close(fd);
    fclose(input);
    if (ok && move_into_place(temp, destination)) return true;
    if (fd >= 0) unlink(temp);
    return false;
}

static void migrate_roms(const LibraryPaths *paths) {
    DIR *dir = opendir(paths->roms);
    if (!dir) return;
    struct dirent64 *entry;
    while ((entry = readdir64(dir))) {
        const char *name = entry->d_name;
        enum CartridgeSystem system = rom_system(name);
        char source[PATH_CAP], destination[PATH_CAP];
        if (name[0] == '.' || system == SYSTEM_COUNT || !join_path(source, paths->roms, name) ||
            !join_path(destination, paths->system_roms[system], name) ||
            !regular_file(source) || !path_absent(destination)) continue;
        if (!move_into_place(source, destination)) fprintf(stderr, "Kept legacy ROM %s\n", name);
    }
    closedir(dir);
}

static bool matching_rom(const char *directory, const char *stem, size_t stem_length,
                         enum CartridgeSystem system) {
    DIR *dir = opendir(directory);
    if (!dir) return false;
    bool found = false;
    struct dirent64 *entry;
    while ((entry = readdir64(dir))) {
        const char *name = entry->d_name;
        enum CartridgeSystem candidate = rom_system(name);
        if (name[0] == '.' || candidate == SYSTEM_COUNT ||
            (system != SYSTEM_COUNT && candidate != system)) continue;
        if ((size_t)(strrchr(name, '.') - name) != stem_length || strncasecmp(name, stem, stem_length)) continue;
        char path[PATH_CAP];
        if (join_path(path, directory, name) && regular_file(path)) { found = true; break; }
    }
    closedir(dir);
    return found;
}

static void migrate_covers(const LibraryPaths *paths) {
    DIR *dir = opendir(paths->roms);
    if (!dir) return;
    struct dirent64 *entry;
    while ((entry = readdir64(dir))) {
        const char *name = entry->d_name;
        const char *extension = strrchr(name, '.');
        if (name[0] == '.' || !extension ||
            (strcasecmp(extension, ".png") && strcasecmp(extension, ".jpg") && strcasecmp(extension, ".jpeg"))) continue;
        char source[PATH_CAP], destination[PATH_CAP];
        if (!join_path(source, paths->roms, name) || !regular_file(source)) continue;

        size_t stem_length = extension - name;
        int targets[SYSTEM_COUNT], count = 0;
        for (int system = 0; system < SYSTEM_COUNT; ++system)
            if (matching_rom(paths->system_roms[system], name, stem_length, system))
                targets[count++] = system;
        if (!count) continue;
        bool still_used = matching_rom(paths->roms, name, stem_length, SYSTEM_COUNT);
        if (count == 1 && !still_used) {
            if (join_path(destination, paths->system_roms[targets[0]], name))
                (void)move_into_place(source, destination);
        } else {
            /* A shared cover can serve more than one cartridge. Keep its source. */
            for (int i = 0; i < count; ++i)
                (void)copy_into_place(source, paths->system_roms[targets[i]], name);
        }
    }
    closedir(dir);
}

static enum CartridgeSystem save_system(const char *name) {
    const char *suffix = strrchr(name, '.');
    if (!suffix || name[0] == '.') return SYSTEM_COUNT;
    bool slot = strlen(suffix) == 8 && !strncasecmp(suffix, ".state", 6) &&
                ((suffix[6] == '0' && suffix[7] >= '1' && suffix[7] <= '9') ||
                 (suffix[6] == '1' && suffix[7] == '0'));
    if (!slot && strcasecmp(suffix, ".srm") && strcasecmp(suffix, ".rtc")) return SYSTEM_COUNT;
    char rom_name[256];
    size_t length = suffix - name;
    if (length >= sizeof rom_name) return SYSTEM_COUNT;
    memcpy(rom_name, name, length);
    rom_name[length] = 0;
    return rom_system(rom_name);
}

static void migrate_saves(const LibraryPaths *paths) {
    DIR *dir = opendir(paths->saves);
    if (!dir) return;
    struct dirent64 *entry;
    while ((entry = readdir64(dir))) {
        const char *name = entry->d_name;
        enum CartridgeSystem system = save_system(name);
        char source[PATH_CAP];
        if (system == SYSTEM_COUNT || !join_path(source, paths->saves, name) || !regular_file(source)) continue;
        (void)copy_into_place(source, paths->system_saves[system], name);
    }
    closedir(dir);
}

void library_migrate(const LibraryPaths *paths) {
    int lock = open(paths->roms, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (lock < 0) return;
    if (!flock(lock, LOCK_EX | LOCK_NB)) {
        migrate_roms(paths);
        migrate_covers(paths);
        migrate_saves(paths);
    }
    close(lock);
}

static bool unlink_optional(const char *directory, const char *name) {
    char path[PATH_CAP];
    if (!join_path(path, directory, name)) return false;
    return !unlink(path) || errno == ENOENT;
}

enum RomRemoval library_remove_rom(const LibraryPaths *paths, const char *name, bool legacy,
                                   bool remove_saves, unsigned slots) {
    enum CartridgeSystem system = rom_system(name);
    if (system == SYSTEM_COUNT || name[0] == '.' || strchr(name, '/')) return ROM_REMOVE_FAILED;
    const char *directory = legacy ? paths->roms : paths->system_roms[system];
    char path[PATH_CAP];
    struct stat64 st;
    if (!join_path(path, directory, name) || lstat64(path, &st) ||
        (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) || unlink(path)) return ROM_REMOVE_FAILED;

    bool clean = true;
    size_t stem_length = strrchr(name, '.') - name;
    const char *const covers[] = {".png", ".jpg", ".jpeg"};
    char filename[300];
    if (stem_length < 256 && !matching_rom(directory, name, stem_length, SYSTEM_COUNT)) {
        for (unsigned i = 0; i < sizeof covers / sizeof covers[0]; ++i) {
            snprintf(filename, sizeof filename, "%.*s%s", (int)stem_length, name, covers[i]);
            if (!unlink_optional(directory, filename)) clean = false;
        }
    }
    if (remove_saves) {
        /* A flat-layout duplicate can appear once the per-console copy is gone. */
        const char *other = legacy ? paths->system_roms[system] : paths->roms;
        if (!join_path(path, other, name) || !path_absent(path)) return ROM_REMOVE_INCOMPLETE;
        const char *const suffixes[] = {".srm", ".rtc", ".latest-state"};
        for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; ++i) {
            snprintf(filename, sizeof filename, "%s%s", name, suffixes[i]);
            if (!unlink_optional(paths->system_saves[system], filename)) clean = false;
            if (!unlink_optional(paths->saves, filename)) clean = false;
        }
        for (unsigned slot = 1; slot <= slots; ++slot) {
            snprintf(filename, sizeof filename, "%s.state%02u", name, slot);
            if (!unlink_optional(paths->system_saves[system], filename)) clean = false;
            if (!unlink_optional(paths->saves, filename)) clean = false;
        }
    }
    return clean ? ROM_REMOVED : ROM_REMOVE_INCOMPLETE;
}
