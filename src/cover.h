// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q2_BOY_COVER_H
#define Q2_BOY_COVER_H

#include <stdbool.h>
#include <stdint.h>

enum CartridgeSystem { SYSTEM_GB, SYSTEM_GBC, SYSTEM_GBA, SYSTEM_COUNT };
#define COVER_SIZE 160
#define COVER_DRAG_PIXELS 90
#define COVER_REACH 3.5f

void cover_clear(void);
void cover_draw(uint32_t *canvas, const char *directory, const char *name,
                enum CartridgeSystem system, float distance, int offset_y);
bool cover_hit(const char *directory, const char *name, enum CartridgeSystem system,
               float distance, int x, int y);

#endif
