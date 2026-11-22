// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform.h"
#include "library_paths.h"
#include "libretro.h"
#include <dlfcn.h>
#include "font.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define GBA_WIDTH 240
#define GBA_HEIGHT 160
#define CART_WIDTH 512
#define CART_HEIGHT 512
#define SLOTS 10
#define ROWS 5
#define ROM_LIMIT 200
#define DISPLAY_REQUEST "/tmp/q2-boy.display.req"
#define DISPLAY_DIMMED "/tmp/q2-boy.display.dimmed"
#define DISPLAY_IDLE_NS 30000000000ull
#define KEY(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)
#define RGB(value) (0xff000000u | (value))
#define ACCENT RGB(0x87e4c7)
#define WHITE RGB(0xf7faf9)
#define GRAY RGB(0xa6b9bb)
#define DARK RGB(0x0c161a)

enum Screen {
    BROWSER, GAME, MENU, STATES, SETTINGS, EDIT, CONFIRM, AUDIO, PHYSICAL, TOUCH, ASSIGN,
    BROWSER_OPTIONS, APP_MENU, COPY_TARGET, COPY_SOURCE
};
enum Scale { FIT, CROP, STRETCH, INTEGER_SCALE, SCALE_COUNT };
enum VideoFilter { NEAREST, SHARP_BILINEAR };
enum Control { L, R, B, MENU_BUTTON, SPEED, QUICK_SAVE, QUICK_LOAD, AB, SELECT_BUTTON, START_BUTTON, TOUCH_A, TOUCH_UP, TOUCH_DOWN, TOUCH_LEFT, TOUCH_RIGHT, TOUCH_X, TOUCH_Y, CONTROLS };
enum Action { SAVE = 1, LOAD };
typedef struct { int x, y, size, opacity, visible; unsigned keys; const char *label; } Button;

#define EDGE_INSET 12
#define VERTICAL_INSET 5
#define TOUCH_HIT_PADDING 12
#define DEFAULT_OPACITY 70
static const Button defaults[CONTROLS] = {
    {29, 5, 72, DEFAULT_OPACITY, 1, KEY(L), "L"},
    {274, 5, 72, DEFAULT_OPACITY, 1, KEY(R), "R"},
    {297, 244, 60, DEFAULT_OPACITY, 1, KEY(B), "B"},
    {34, 272, 62, DEFAULT_OPACITY, 1, 0, "Menu"},
    {66, 222, 44, DEFAULT_OPACITY, 0, 0, "FF"},
    {122, 208, 44, DEFAULT_OPACITY, 0, 0, "Save"},
    {174, 208, 44, DEFAULT_OPACITY, 0, 0, "Load"},
    {310, 192, 46, DEFAULT_OPACITY, 0, KEY(A) | KEY(B), "A+B"},
    {126, 281, 56, DEFAULT_OPACITY, 1, KEY(SELECT), "Select"},
    {192, 281, 56, DEFAULT_OPACITY, 1, KEY(START), "Start"},
    {253, 244, 40, DEFAULT_OPACITY, 0, KEY(A), "A"},
    {64, 127, 42, DEFAULT_OPACITY, 0, KEY(UP), "Up"},
    {64, 215, 42, DEFAULT_OPACITY, 0, KEY(DOWN), "Down"},
    {20, 171, 42, DEFAULT_OPACITY, 0, KEY(LEFT), "Left"},
    {108, 171, 42, DEFAULT_OPACITY, 0, KEY(RIGHT), "Right"},
    {304, 141, 50, DEFAULT_OPACITY, 1, KEY(X), "X"},
    {250, 192, 50, DEFAULT_OPACITY, 1, KEY(Y), "Y"}
};
static const int old_default_positions[CONTROLS][2] = {
    [L] = {18, 16}, [R] = {285, 16}, [MENU_BUTTON] = {18, 260}, [SPEED] = {88, 260},
    [SELECT_BUTTON] = {126, 268}, [START_BUTTON] = {192, 268}
};
static const int test_default_positions[CONTROLS][2] = {
    [L] = {38, 5}, [R] = {265, 5}, [MENU_BUTTON] = {43, 272}
};
static const int old_default_opacity[CONTROLS] = {
    58, 58, 58, 65, 65, 65, 65, 58, 65, 65, 62, 62, 62, 62, 62
};
enum Physical { P_RETURN, P_PLAY, P_PREVIOUS, P_NEXT, P_CENTRE, PHYSICAL_COUNT };
static const char *const physical_names[] = {"Return", "Play/Pause", "Previous", "Next", "Centre"};
static const unsigned physical_defaults[] = {KEY(UP), KEY(DOWN), KEY(LEFT), KEY(RIGHT), KEY(A)};
static unsigned physical_map[PHYSICAL_COUNT], held_buttons, gameplay_buttons;
static const struct { unsigned keys; const char *name; } actions[] = {
    {KEY(A), "A"}, {KEY(B), "B"}, {KEY(START), "Start"}, {KEY(SELECT), "Select"},
    {KEY(L), "L"}, {KEY(R), "R"}, {KEY(UP), "Up"}, {KEY(DOWN), "Down"},
    {KEY(LEFT), "Left"}, {KEY(RIGHT), "Right"}, {KEY(A) | KEY(B), "A+B"}, {0, "Unassigned"},
    {KEY(X), "X"}, {KEY(Y), "Y"}
};
static const int touch_choices[] = {TOUCH_A, B, SELECT_BUTTON, START_BUTTON, L, R,
    TOUCH_UP, TOUCH_DOWN, TOUCH_LEFT, TOUCH_RIGHT, AB, SPEED, QUICK_SAVE, QUICK_LOAD, TOUCH_X, TOUCH_Y};
static int touch_page, assign_page, assign_target;
static enum Screen assign_return;
static const char *action_name(unsigned keys) {
    for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; ++i)
        if (actions[i].keys == keys) return actions[i].name;
    return "Unassigned";
}
static bool valid_action(unsigned keys) {
    for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; ++i)
        if (actions[i].keys == keys) return true;
    return false;
}
static bool remappable_overlay(int which) { return which < MENU_BUTTON || which >= AB; }
static const char *button_label(int which, const Button *b) {
    return remappable_overlay(which) ? action_name(b->keys) : b->label;
}
static const int legacy_geometry[][3] = {
    {6, 6, 42}, {327, 6, 42}, {265, 264, 48}, {321, 264, 48}, {6, 264, 48},
    {66, 264, 44}, {122, 264, 44}, {176, 264, 44}, {320, 206, 46}
};
static const int previous_overlays[] = {L, R, -1, B, MENU_BUTTON, SPEED, QUICK_SAVE, QUICK_LOAD, AB};
static const int speeds[] = {2, 3};
static int fast_rate = 2;
#define AUDIO_REQUEST "/tmp/q2-boy.audio.req"
#define AUDIO_REPLY "/tmp/q2-boy.audio.reply"
#define AUDIO_MAGIC 0x51474131u
#ifndef Q2_RUNTIME_DIR
#define Q2_RUNTIME_DIR "/tmp"
#endif
#define AUDIO_MODE Q2_RUNTIME_DIR "/q2-boy.audio"
#define NATIVE_VIEW Q2_RUNTIME_DIR "/q2-boy.view"
#define QUIT_OPTION Q2_RUNTIME_DIR "/q2-boy.quit"
#define ADDON_CAPS "/tmp/q2-addon.caps"
enum AudioAction { AUDIO_GAME = 1, AUDIO_MUSIC, AUDIO_VOLUME, AUDIO_PREVIOUS, AUDIO_PLAY, AUDIO_NEXT, AUDIO_NOW_PLAYING };
struct AudioCommand { uint32_t magic, seq; int32_t action, value; };
struct AudioReply { uint32_t magic, seq; int32_t music, volume, limit, route, gain, playing, native_ui; };
static struct AudioReply audio_reply;
static bool audio_bridge, music_mode, native_supported, native_yielded;
static bool audio2, firmware_keys, prefer_music, remember_source, quit_off, audio_timeout;
static uint64_t native_request_time;
static char game_device[16], game_gain[8];
static int local_volume = 100, audio_route;
static uint32_t audio_seq;
static enum AudioAction pending_action;
static uint64_t audio_polled, audio_request_time;
static bool performance_log;
static bool smoke_mute;
static bool display_bridge, dimmed;
static uint64_t last_activity;
static int wake_key = -1;
static bool wake_touch;
static unsigned longest_run_us, longest_draw_us, late_frames;
static bool core_options_changed, allow_video = true;
static uint64_t next_video;
#define FAST_DISPLAY_NS 33333333ull
static double measured_speed;
static Button buttons[CONTROLS];
typedef struct { Button overlays[CONTROLS]; unsigned physical[PHYSICAL_COUNT]; } ControlProfile;
static ControlProfile inherited_controls, control_profiles[SYSTEM_COUNT];
static enum CartridgeSystem copy_target;
static bool resume_latest;
static enum Screen screen = BROWSER, confirm_from;
static enum Scale scaling;
static enum VideoFilter video_filter = SHARP_BILINEAR;
static int running, fast, dirty = 1, slot, edit_button, dragging = -1;
static int drag_x, drag_y, edit_start_x, edit_start_y, edit_moved, touch_button = -1, wheel_delta, menu_focus;
static bool game_controls_hidden;
static struct {
    int direction, x, y, button;
    uint64_t press_at;
} game_edge;

#define GAME_EDGE_WIDTH 46
#define GAME_SWIPE_DISTANCE 36
#define EDGE_TAP_SLOP 10
#define EDGE_BUTTON_HOLD_NS 120000000ull

static enum Action confirm_action;
static uint64_t wheel_time, notice_end, next_save;
static unsigned physical_keys, touch_keys, pressed_this_frame;
static char notice[95];
static const char *data_dir = "/mnt/mmc/q2-boy";
static LibraryPaths data_paths;
static char game_base[PATH_CAP], legacy_game_base[PATH_CAP], config_file[PATH_CAP];
static char active_rom[PATH_CAP];
enum RomSort { SORT_TITLE, SORT_RECENT, SORT_COUNT };
static const char *const sort_labels[] = {"Title (A-Z)", "Last played"};
static const char *const sort_short[] = {"A-Z", "Recent"};
enum { FAVORITES = SYSTEM_COUNT, RECENT, SHELF_COUNT, RECENT_LIMIT = 10 };
static const char *const shelf_names[SHELF_COUNT] = {
    "Game Boy", "Game Boy Color", "Game Boy Advance", "NES", "SNES", "Favorites", "Recently Played"
};
static const char *const system_short[] = {"GB", "GBC", "GBA", "NES", "SNES"};
static enum CartridgeSystem active_system = SYSTEM_GBA;
typedef struct {
    char name[256];
    int title_length, recent_rank;
    enum CartridgeSystem system;
    bool legacy;
} Rom;
typedef struct { int first, end, selected; } RomShelf;
typedef char RomName[sizeof ((Rom *)0)->name];
static Rom roms[ROM_LIMIT];
static RomShelf shelves[SHELF_COUNT], *shelf = &shelves[RECENT], *previous_shelf;
static RomName favorite_names[ROM_LIMIT];
static int favorite_count, favorite_roms[ROM_LIMIT], recent_roms[RECENT_LIMIT];
static int rom_count, recent_count;
static struct {
    unsigned char version, sort, list;
    char recent[ROM_LIMIT][256];
} library;
static char library_file[PATH_CAP], favorites_file[PATH_CAP];
static float flow_position, flow_start, flow_drag_origin;
static uint64_t flow_started, flow_draw_at, title_since, system_slide_started;
static uint64_t prefetch_selection, prefetch_at;
static int prefetched, last_marquee_offset;
static bool touching_flow;
static bool browser_actions_open;
static enum { TOUCH_TAP, TOUCH_HORIZONTAL, TOUCH_VERTICAL } flow_gesture;
static int flow_down_x, flow_down_y, system_slide_direction, system_slide_offset;

#define BROWSER_BUTTON_Y 12
#define BROWSER_HEADER_BOTTOM 54
#define EXIT_X 36
#define EXIT_W 84
#define APP_MENU_X 126
#define APP_MENU_W 103
#define HEART_TARGET 32
#define HEART_ICON 18
#define LIST_HEART_X (WIDTH - 64)
#define SORT_X 235
#define SORT_W 103
#define MORE_DOTS_X (WIDTH - 66)
#define MORE_TOUCH_X (WIDTH - 125)
#define MORE_TAP_SLOP 24
#define FLOW_FOOT_Y 272
#define SHELF_TRAVEL 270
#define SYSTEM_SLIDE_NS 210000000ull
#define LIST_TOP 58
#define LIST_ROW 42
#define SYSTEM_GESTURE_PX 38
static uint32_t cart[CART_WIDTH * CART_HEIGHT], color[65536];
static uint32_t *canvas;
static bool smoke, have_frame, use_dynarec;
static volatile sig_atomic_t launch_stage;
static uint64_t speed_start;
static unsigned speed_frames, video_frames;
static double fps = 59.7275;

/* gpSP remains built in. Load external cores only when their games are opened. */
#define CORE_API(X) \
    X(set_environment) X(set_video_refresh) X(set_audio_sample) X(set_audio_sample_batch) \
    X(set_input_poll) X(set_input_state) X(init) X(deinit) X(set_controller_port_device) \
    X(load_game) X(unload_game) X(get_system_info) X(get_system_av_info) X(run) X(get_memory_size) \
    X(get_memory_data) X(serialize_size) X(serialize) X(unserialize)
struct Core {
#define API_MEMBER(name) __typeof__(&retro_##name) name;
    CORE_API(API_MEMBER)
#undef API_MEMBER
};
static const struct Core gpsp = {
#define API_GPSP(name) .name = retro_##name,
    CORE_API(API_GPSP)
#undef API_GPSP
};
typedef struct {
    const char *filename, *label;
    struct Core api;
    void *library;
} SharedCore;

enum SharedCoreId { GAMBATTE, FCEUMM, SNES9X2005, SHARED_CORES };
static SharedCore shared_cores[SHARED_CORES] = {
    [GAMBATTE] = {.filename = "gambatte_libretro.so", .label = "Gambatte"},
    [FCEUMM] = {.filename = "fceumm_libretro.so", .label = "FCEUmm"},
    [SNES9X2005] = {.filename = "snes9x2005_libretro.so", .label = "Snes9x 2005"}
};
static const struct Core *core;
static char app_dir[PATH_CAP];
static unsigned cart_width = GBA_WIDTH, cart_height = GBA_HEIGHT;
static unsigned smoke_audio_frames;

static bool load_shared_core(SharedCore *module) {
    if (module->library) return true;
    char path[PATH_CAP];
    int length = snprintf(path, sizeof path, "%s/%s", app_dir, module->filename);
    if (length < 0 || length >= (int)sizeof path) return false;
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "%s unavailable: %s\n", module->label, dlerror());
        return false;
    }
#define API_LOAD(name) \
    do { \
        module->api.name = (__typeof__(module->api.name))dlsym(handle, "retro_" #name); \
        if (!module->api.name) { \
            fprintf(stderr, "%s lacks retro_%s\n", module->label, #name); \
            dlclose(handle); \
            return false; \
        } \
    } while (0);
    CORE_API(API_LOAD)
#undef API_LOAD
    module->library = handle;
    return true;
}

static void close_shared_cores(void) {
    for (int i = 0; i < SHARED_CORES; ++i)
        if (shared_cores[i].library) dlclose(shared_cores[i].library);
}

static void initialize_core(const struct Core *selected);
static void release_core(void) {
    if (core) { core->deinit(); core = NULL; }
}
static void locate_app(const char *argv0) {
    ssize_t n = readlink("/proc/self/exe", app_dir, sizeof app_dir - 1);
    if (n > 0) app_dir[n] = 0;
    else snprintf(app_dir, sizeof app_dir, "%s", argv0);
    char *slash = strrchr(app_dir, '/');
    if (slash == app_dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(app_dir, ".");
}
static int clamp(int x, int low, int high) { return x < low ? low : x > high ? high : x; }
/* 2x has headroom with the original path. At higher targets pace video by
 * elapsed time, leaving extra GBA frames to advance without a panel update. */
static bool paced_video(void) { return screen == GAME && fast && (fast_rate >= 3 || core != &gpsp); }
static uint64_t sooner(uint64_t a, uint64_t b) { return a < b ? a : b; }
static void schedule_video(uint64_t now) {
    allow_video = screen == GAME && (!paced_video() || !next_video || now >= next_video);
    if (paced_video() && allow_video) {
        if (!next_video || now - next_video >= FAST_DISPLAY_NS) next_video = now;
        next_video += FAST_DISPLAY_NS;
    }
}
static int button_height(int which, const Button *b) {
    if (which == MENU_BUTTON) return (b->size * 7 + 5) / 10;
    if (which == L || which == R) return (b->size * 5 + 4) / 9;
    if (which == SELECT_BUTTON || which == START_BUTTON) return (b->size * 3 + 2) / 5;
    return b->size;
}
static int button_radius(int which, const Button *b) {
    return which == B ? b->size / 2 : 8;
}
static void place_button(int which, Button *b, int x, int y) {
    b->x = clamp(x, EDGE_INSET, WIDTH - EDGE_INSET - b->size);
    b->y = clamp(y, VERTICAL_INSET, HEIGHT - VERTICAL_INSET - button_height(which, b));
}
static void move_button(int which, int x, int y) { place_button(which, &buttons[which], x, y); }
/* Outside the rounded rectangle's inner box, measure from its nearest point. */
static int shape_distance_squared(int x, int y, int w, int h, int radius) {
    int dx = x < radius ? radius - 1 - x : x >= w - radius ? x - (w - radius) : 0;
    int dy = y < radius ? radius - 1 - y : y >= h - radius ? y - (h - radius) : 0;
    return dx * dx + dy * dy;
}
static bool inside_shape(int x, int y, int w, int h, int radius) {
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    return shape_distance_squared(x, y, w, h, radius) < radius * radius;
}
static void status(const char *message) {
    snprintf(notice, sizeof notice, "%s", message);
    notice_end = clock_ns() + 3000000000ull;
    dirty = 1;
}
static void rect(int x, int y, int w, int h, uint32_t fill) {
    int left = clamp(x, 0, WIDTH), right = clamp(x + w, 0, WIDTH);
    int top = clamp(y, 0, HEIGHT), bottom = clamp(y + h, 0, HEIGHT);
    for (int row = top; row < bottom; ++row)
        for (int col = left; col < right; ++col) canvas[row * WIDTH + col] = fill;
}
static void draw_ellipsis(int x, int y) {
    for (int offset = -7; offset <= 7; offset += 7) {
        rect(x + offset - 3, y - 3, 6, 6, RGB(0x101010));
        rect(x + offset - 2, y - 2, 4, 4, GRAY);
    }
}
static inline uint32_t blend_video(uint32_t a, uint32_t b, unsigned weight);
static void draw_mask(int x, int y, int width, int height, const void *mask_bytes,
                      uint32_t ink, int left, int right) {
    const unsigned char *mask = mask_bytes;
    const int stride = (width + 1) / 2;
    int first_col = clamp(left - x, 0, width), end_col = clamp(right - x, 0, width);
    int first_row = clamp(-y, 0, height), end_row = clamp(HEIGHT - y, 0, height);
    for (int row = first_row; row < end_row; ++row) {
        for (int col = first_col; col < end_col; ++col) {
            unsigned alpha = (mask[row * stride + col / 2] >> (4 * (col % 2))) & 15u;
            if (!alpha) continue;
            uint32_t *pixel = &canvas[(y + row) * WIDTH + x + col];
            *pixel = alpha == 15 ? ink : blend_video(*pixel, ink, alpha * 17);
        }
    }
}
static void text_at(int x, int y, const char *value, uint32_t ink, int limit) {
    for (; *value; ++value) {
        unsigned ch = (unsigned char)*value;
        unsigned a = ch < 32 || ch > 126 ? '?' - 32 : ch - 32;
        if (x + advance[a] > limit) break;
        draw_mask(x, y, 16, 17, &glyphs[a], ink, 0, WIDTH);
        x += advance[a];
    }
}
static void put_button(int which, const char *label) {
    const Button *b = &buttons[which];
    bool chosen = screen == EDIT && edit_button == which;
    uint32_t tint = chosen || (touch_button == which && b->keys) ? RGB(0x296b5b) : DARK;
    int w = b->size, h = button_height(which, b), radius = button_radius(which, b);
    unsigned alpha = b->opacity * 256 / 100;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!inside_shape(x, y, w, h, radius)) continue;
            uint32_t *pixel = &canvas[(b->y + y) * WIDTH + b->x + x];
            if (chosen && !inside_shape(x - 2, y - 2, w - 4, h - 4, radius - 2)) {
                *pixel = ACCENT;
                continue;
            }
            uint32_t old = *pixel;
            unsigned rb = ((old & 0xff00ff) * (256 - alpha) + (tint & 0xff00ff) * alpha) >> 8;
            unsigned g = ((old & 0x00ff00) * (256 - alpha) + (tint & 0x00ff00) * alpha) >> 8;
            *pixel = RGB((rb & 0xff00ff) | (g & 0xff00));
        }
    }
    int label_width = 0;
    for (const char *p = label; *p; ++p) label_width += advance[*p - 32];
    text_at(b->x + (b->size - label_width) / 2, b->y + (h - 17) / 2,
            label, WHITE, b->x + b->size);
}
static bool supports_action(enum CartridgeSystem system, unsigned keys) {
    if (system != SYSTEM_SNES && (keys & (KEY(X) | KEY(Y)))) return false;
    return system == SYSTEM_GBA || system == SYSTEM_SNES || !(keys & (KEY(L) | KEY(R)));
}
static bool supports_overlay(enum CartridgeSystem system, int index) {
    return (index < TOUCH_X || system == SYSTEM_SNES) &&
           supports_action(system, defaults[index].keys);
}
static bool overlay_visible(int index) {
    const Button *button = &buttons[index];
    if (!button->visible) return false;
    return (index < TOUCH_X || active_system == SYSTEM_SNES) &&
           supports_action(active_system, button->keys);
}
static int control_choices(bool touch, int choices[CONTROLS]) {
    int count = 0;
    if (touch) {
        for (unsigned i = 0; i < sizeof touch_choices / sizeof touch_choices[0]; ++i) {
            int id = touch_choices[i];
            if (supports_overlay(active_system, id) ||
                ((id == L || id == R) && supports_action(active_system, buttons[id].keys)))
                choices[count++] = id;
        }
    } else {
        for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; ++i)
            if (supports_action(active_system, actions[i].keys)) choices[count++] = i;
    }
    return count;
}

typedef struct { int x, y, width, height; } Picture;

static Picture game_picture(void) {
    int width = WIDTH, height = HEIGHT;
    if (scaling == INTEGER_SCALE) {
        unsigned scale_x = WIDTH / cart_width, scale_y = HEIGHT / cart_height;
        unsigned scale = scale_x < scale_y ? scale_x : scale_y;
        if (scale) {
            width = scale * cart_width;
            height = scale * cart_height;
            return (Picture){(WIDTH - width) / 2, (HEIGHT - height) / 2, width, height};
        }
    }
    /* TV consoles use the 4:3 display shape. Preserve handheld pixel geometry. */
    unsigned aspect_w = active_system >= SYSTEM_NES ? 4 : cart_width;
    unsigned aspect_h = active_system >= SYSTEM_NES ? 3 : cart_height;
    bool panel_is_narrower = WIDTH * aspect_h <= HEIGHT * aspect_w;
    if (scaling != STRETCH) {
        if (panel_is_narrower == (scaling != CROP)) height = WIDTH * aspect_h / aspect_w;
        else width = HEIGHT * aspect_w / aspect_h;
    }
    return (Picture){(WIDTH - width) / 2, (HEIGHT - height) / 2, width, height};
}

static void draw_nearest(Picture p) {
    int left = clamp(p.x, 0, WIDTH), right = clamp(p.x + p.width, 0, WIDTH);
    static int columns[WIDTH], rows[HEIGHT];
    static Picture previous;
    static unsigned width, height;
    const int top = clamp(p.y, 0, HEIGHT), bottom = clamp(p.y + p.height, 0, HEIGHT);
    if (width != cart_width || height != cart_height || memcmp(&p, &previous, sizeof p)) {
        for (int x = left; x < right; ++x) columns[x] = (x - p.x) * cart_width / p.width;
        for (int y = top; y < bottom; ++y) rows[y] = (y - p.y) * cart_height / p.height;
        previous = p;
        width = cart_width;
        height = cart_height;
    }
    uint32_t *pixels = canvas;
    for (int y = top; y < bottom; ++y) {
        const uint32_t *source = cart + rows[y] * cart_width;
        uint32_t *dest = pixels + y * WIDTH;
        for (int x = left; x < right; ++x) dest[x] = source[columns[x]];
    }
}

typedef struct { uint16_t pixel, weight; } VideoSample;

static void sharp_axis(VideoSample *samples, int start, int end, int origin, int output, int input) {
    for (int i = start; i < end; ++i) {
        int offset = i - origin;
        if (output >= input) {
            /* Blend only where a destination pixel straddles a source pixel edge. */
            int edge = offset * input;
            int overlap = edge % output + input - output;
            samples[i] = (VideoSample){edge / output, overlap > 0 ? overlap * 256 / input : 0};
        } else {
            /* Use a centered bilinear sample when a high-resolution frame needs shrinking. */
            int center = (2 * offset + 1) * input - output;
            int step = 2 * output;
            samples[i] = (VideoSample){center / step, (center % step) * 256 / step};
        }
    }
}

static inline uint32_t blend_video(uint32_t a, uint32_t b, unsigned weight) {
    const uint32_t red_blue = 0x00ff00ffu, green = 0x0000ff00u;
    unsigned rest = 256 - weight;
    uint32_t rb = ((a & red_blue) * rest + (b & red_blue) * weight) >> 8;
    uint32_t g = ((a & green) * rest + (b & green) * weight) >> 8;
    return RGB((rb & red_blue) | (g & green));
}

static inline uint32_t sample_column(const uint32_t *row, VideoSample sample) {
    uint32_t pixel = row[sample.pixel];
    return sample.weight ? blend_video(pixel, row[sample.pixel + 1], sample.weight) : pixel;
}

static const uint32_t *sharp_row(uint32_t cache[2][WIDTH], int cached[2],
                                 const VideoSample columns[WIDTH], int row, int left, int right) {
    unsigned slot = row % 2;
    if (cached[slot] != row) {
        const uint32_t *source = cart + row * cart_width;
        for (int x = left; x < right; ++x) cache[slot][x] = sample_column(source, columns[x]);
        cached[slot] = row;
    }
    return cache[slot];
}

static void draw_sharp(Picture p) {
    static VideoSample columns[WIDTH], rows[HEIGHT];
    static Picture previous;
    static unsigned width, height;
    uint32_t row_cache[2][WIDTH];
    int cached[2] = {-1, -1};
    int left = clamp(p.x, 0, WIDTH), right = clamp(p.x + p.width, 0, WIDTH);
    int top = clamp(p.y, 0, HEIGHT), bottom = clamp(p.y + p.height, 0, HEIGHT);
    if (width != cart_width || height != cart_height || memcmp(&p, &previous, sizeof p)) {
        sharp_axis(columns, left, right, p.x, p.width, cart_width);
        sharp_axis(rows, top, bottom, p.y, p.height, cart_height);
        previous = p;
        width = cart_width;
        height = cart_height;
    }
    uint32_t *pixels = canvas;
    for (int y = top; y < bottom; ++y) {
        int source_y = rows[y].pixel;
        const uint32_t *row = sharp_row(row_cache, cached, columns, source_y, left, right);
        uint32_t *dest = pixels + y * WIDTH;
        if (rows[y].weight) {
            const uint32_t *next = sharp_row(row_cache, cached, columns, source_y + 1, left, right);
            for (int x = left; x < right; ++x)
                dest[x] = blend_video(row[x], next[x], rows[y].weight);
        } else {
            memcpy(dest + left, row + left, (right - left) * sizeof(*dest));
        }
    }
}

static void draw_game(void) {
    Picture picture = game_picture();
    int right = picture.x + picture.width, bottom = picture.y + picture.height;
    if (picture.x > 0) rect(0, 0, picture.x, HEIGHT, 0);
    if (right < WIDTH) rect(right, 0, WIDTH - right, HEIGHT, 0);
    if (picture.y > 0) rect(0, 0, WIDTH, picture.y, 0);
    if (bottom < HEIGHT) rect(0, bottom, WIDTH, HEIGHT - bottom, 0);
    if (video_filter == SHARP_BILINEAR &&
        (picture.width % cart_width || picture.height % cart_height)) draw_sharp(picture);
    else draw_nearest(picture);
    for (int i = 0; !game_controls_hidden && i < CONTROLS; ++i) {
        if (!overlay_visible(i)) continue;
        char label[12];
        const char *name = button_label(i, &buttons[i]);
        if (i == QUICK_SAVE || i == QUICK_LOAD) {
            snprintf(label, sizeof label, "%c%02d", i == QUICK_SAVE ? 'S' : 'L', slot + 1);
            name = label;
        }
        put_button(i, name);
    }
    if (fast) {
        rect(127, 7, 121, 23, DARK);
        char badge[32];
        if (measured_speed > 0)
            snprintf(badge, sizeof badge, "%.2fx / %dx", measured_speed, fast_rate);
        else snprintf(badge, sizeof badge, ">> %dx target", fast_rate);
        text_at(138, 10, badge, ACCENT, 244);
    }
}
static void header(const char *title, const char *sub) {
    rect(0, 0, WIDTH, HEIGHT, DARK);
    text_at(17, 10, title, ACCENT, WIDTH - 16);
    text_at(17, 31, sub, GRAY, WIDTH - 16);
}
static int file_path(char *path, size_t capacity, const char *base, const char *ext) {
    int n = snprintf(path, capacity, "%s%s", base, ext);
    return n >= 0 && (size_t)n < capacity;
}
static int file_write(const char *path, const void *data, size_t length) {
    char temp[PATH_CAP];
    if (!file_path(temp, sizeof temp, path, ".tmp")) return false;
    FILE *f = fopen(temp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, length, f) == length && !fflush(f) && !fsync(fileno(f));
    if (fclose(f)) ok = false;
    if (ok && !rename(temp, path)) return true;
    unlink(temp);
    return false;
}
static bool read_text(const char *path, char *buf, size_t capacity) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t n = fread(buf, 1, capacity - 1, file);
    bool ok = !ferror(file) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!ok || memchr(buf, 0, n)) { errno = EINVAL; return false; }
    buf[n] = 0;
    return true;
}
static bool has_cap(const char *caps, const char *word) {
    size_t length = strlen(word);
    for (const char *at = caps; *at;) {
        at += strspn(at, " \t\r\n");
        size_t n = strcspn(at, " \t\r\n");
        if (n == length && !strncmp(at, word, n)) return true;
        at += n;
    }
    return false;
}
static bool enable_manifest_audio(bool supported, bool *was_audio2) {
    char manifest[PATH_CAP], text[512];
    if (!join_path(manifest, app_dir, "q2app") || !read_text(manifest, text, sizeof text))
        return false;
    char *line = text;
    while (line) {
        char *end = strchr(line, '\n');
        if ((end ? end - line : (ptrdiff_t)strlen(line)) >= 7 &&
            !strncmp(line, "audio=", 6) && (line[6] == '1' || line[6] == '2') &&
            (line[7] == '\r' || line[7] == '\n' || !line[7])) {
            *was_audio2 = line[6] == '2';
            if (*was_audio2 == supported) return true;
            line[6] = supported ? '2' : '1';
            return file_write(manifest, text, strlen(text));
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}
static void store_audio_source(bool music) {
    char path[PATH_CAP];
    const char *mode = music ? "music\n" : "game\n";
    prefer_music = music;
    if (file_path(path, sizeof path, data_dir, "/audio-source.cfg"))
        (void)file_write(path, mode, strlen(mode));
}
static bool write_quit_option(bool off) {
    if (off) return file_write(QUIT_OPTION, "off", 3);
    return !unlink(QUIT_OPTION) || errno == ENOENT;
}
static bool switch_audio2(enum AudioAction action, bool remember) {
    const char *mode = action == AUDIO_MUSIC ? "music" : "game";
    bool had_audio = !music_mode;
    if (action == AUDIO_MUSIC) platform_audio_release();
    if (!file_write(AUDIO_MODE, mode, strlen(mode))) {
        if (action == AUDIO_MUSIC && had_audio && game_device[0])
            (void)platform_audio_open(game_device, game_gain);
        status("Could not request Q2 audio");
        return false;
    }
    if (action == AUDIO_MUSIC) music_mode = true;
    pending_action = action;
    remember_source = remember;
    audio_request_time = clock_ns();
    audio_timeout = false;
    dirty = 1;
    return true;
}
static void yield_player(void) {
    platform_yield_to_player();
    physical_keys = pressed_this_frame = touch_keys = 0;
    held_buttons = gameplay_buttons = 0;
    native_yielded = true;
}
static void resume_from_player(void) {
    native_yielded = false;
    last_activity = clock_ns();
    platform_resume_from_player();
    physical_keys = pressed_this_frame = touch_keys = 0;
    held_buttons = gameplay_buttons = 0;
    menu_focus = 0;
    dirty = 1;
}
static bool audio_arguments(const char *line, char device[16], char volume[8]) {
    char extra;
    int fields = sscanf(line, "%15s %7s %c", device, volume, &extra);
    if (fields == 1) { volume[0] = 0; return !strcmp(device, "plughw:0,0"); }
    if (fields != 2 || (strcmp(device, "plug:bluealsa") && strcmp(device, "usb"))) return false;
    const char *digits = volume + (!strcmp(device, "usb") && volume[0] == 'h');
    return *digits && strlen(digits) <= 3 && strspn(digits, "0123456789") == strlen(digits) &&
           atoi(digits) <= 100;
}
static void audio2_poll(uint64_t now) {
    if (pending_action) {
        char reply[48], device[16], volume[8];
        bool have_reply = read_text(AUDIO_MODE, reply, sizeof reply);
        if (pending_action == AUDIO_GAME && have_reply && audio_arguments(reply, device, volume)) {
            pending_action = 0;
            if (platform_audio_open(device, volume)) {
                strcpy(game_device, device);
                strcpy(game_gain, volume);
                music_mode = false;
                if (remember_source) store_audio_source(false);
            } else {
                (void)switch_audio2(AUDIO_MUSIC, false);
                status("Could not open game audio");
            }
            dirty = 1;
        } else if (pending_action == AUDIO_MUSIC && !have_reply && errno == ENOENT) {
            pending_action = 0;
            if (remember_source) store_audio_source(true);
            dirty = 1;
        } else if (!audio_timeout && now - audio_request_time > 5000000000ull) {
            audio_timeout = true;
            status("Waiting for Q2 audio. You can exit if it does not respond.");
        }
    }
    if (!native_yielded) return;
    char view[12];
    if (read_text(NATIVE_VIEW, view, sizeof view)) {
        if (!strcmp(view, "open")) { native_request_time = 0; return; }
        if (!native_request_time || now - native_request_time < 5000000000ull) return;
        if (unlink(NATIVE_VIEW) && errno != ENOENT) return;
        status("Could not open Q2 Now Playing");
    } else if (errno != ENOENT) return;
    resume_from_player();
    if (!prefer_music && music_mode && !pending_action) (void)switch_audio2(AUDIO_GAME, false);
}
static uint64_t display_deadline(uint64_t now) {
    if (!display_bridge || dimmed || native_yielded || screen == GAME) return UINT64_MAX;
    uint64_t deadline = last_activity + DISPLAY_IDLE_NS;
    if (now < deadline) return deadline;
    if (file_write(DISPLAY_REQUEST, "1", 1)) {
        dimmed = true;
        return UINT64_MAX;
    }
    last_activity = now; /* Try again after the next idle interval. */
    return now + DISPLAY_IDLE_NS;
}
static void request_audio(enum AudioAction action, int value) {
    if (!audio_bridge || pending_action) return;
    bool opening = action == AUDIO_NOW_PLAYING;
    if (audio2) {
        if (!opening) { (void)switch_audio2(action, true); return; }
        yield_player();
        if (!file_write(NATIVE_VIEW, "now", 3) || (!music_mode && !switch_audio2(AUDIO_MUSIC, false))) {
            unlink(NATIVE_VIEW);
            resume_from_player();
            status("Could not open Q2 Now Playing");
        } else native_request_time = clock_ns();
        return;
    }
    bool releasing = (action == AUDIO_MUSIC || opening) && !music_mode;
    if (opening) yield_player();
    if (releasing) platform_audio_release();
    struct AudioCommand command = {AUDIO_MAGIC, ++audio_seq, action, value};
    if (!file_write(AUDIO_REQUEST, &command, sizeof command)) {
        if (releasing) platform_audio_select(audio_route, audio_reply.magic ? audio_reply.gain : 100);
        if (opening) { native_yielded = false; platform_resume_from_player(); }
        status("Q2 audio controls are unavailable");
        return;
    }
    pending_action = action;
    audio_request_time = clock_ns();
    dirty = 1;
}
static uint64_t audio_deadline(void) {
    if (!audio_bridge) return UINT64_MAX;
    if (audio2 && !pending_action && !native_yielded) return UINT64_MAX;
    return audio_polled + (pending_action || native_yielded || screen == AUDIO ? 75000000ull : 500000000ull);
}
static void audio_poll(uint64_t now) {
    if (now < audio_deadline()) return;
    audio_polled = now;
    if (audio2) { audio2_poll(now); return; }
    struct AudioReply next = {0};
    FILE *file = fopen(AUDIO_REPLY, "rb");
    if (file) {
        size_t bytes = fread(&next, 1, sizeof next, file);
        if (bytes != sizeof next && (native_supported || bytes != sizeof next - sizeof next.native_ui))
            next.magic = 0;
        fclose(file);
    }
    if (next.magic == AUDIO_MAGIC) {
        bool switched = music_mode != (next.music != 0);
        if (switched) {
            music_mode = next.music != 0;
            store_audio_source(music_mode);
            if (music_mode) platform_audio_release();
        }
        if (!music_mode && (switched || audio_route != next.route)) {
            audio_route = next.route;
            platform_audio_select(audio_route, next.gain);
        }
        if (!music_mode) platform_audio_gain(next.gain);
        if (pending_action && next.seq == audio_seq) {
            pending_action = 0;
            if (screen == AUDIO) dirty = 1;
        }
        if (native_yielded && !next.native_ui && !pending_action) {
            resume_from_player();
        }
        if (memcmp(&next, &audio_reply, sizeof next)) {
            audio_reply = next;
            if (screen == AUDIO) dirty = 1;
        }
    }
    if (pending_action && now - audio_request_time > 5000000000ull) {
        if ((pending_action == AUDIO_MUSIC || pending_action == AUDIO_NOW_PLAYING) && !music_mode)
            platform_audio_select(audio_route, audio_reply.magic ? audio_reply.gain : 100);
        if (native_yielded) {
            native_yielded = false;
            last_activity = now;
            platform_resume_from_player();
        }
        pending_action = 0;
        status("Q2 audio request timed out");
    }
}
static int write_overlay(char *buffer, size_t remaining, const Button *b) {
    return snprintf(buffer, remaining, "%d %d %d %d %d %u\n",
                    b->x, b->y, b->size, b->opacity, b->visible, b->keys);
}

static bool write_config(const char *path, const ControlProfile *profile) {
    char buffer[768];
    int used = snprintf(buffer, sizeof buffer, "%d 9 %d %d %d %d %d\n",
                        scaling, fast_rate, local_volume, video_filter, resume_latest, quit_off);
    for (int i = 0; i < TOUCH_X; ++i)
        used += write_overlay(buffer + used, sizeof buffer - used, &profile->overlays[i]);
    for (int i = 0; i < PHYSICAL_COUNT; ++i)
        used += snprintf(buffer + used, sizeof buffer - used, "%u\n", profile->physical[i]);
    for (int i = TOUCH_X; i < CONTROLS; ++i)
        used += write_overlay(buffer + used, sizeof buffer - used, &profile->overlays[i]);
    return file_write(path, buffer, used);
}
static void store_config(void) {
    if (!write_config(config_file, &inherited_controls)) status("Could not save app settings");
}
static bool controls_path(char path[PATH_CAP], enum CartridgeSystem system) {
    char name[32];
    snprintf(name, sizeof name, "controls-%s.cfg", system_short[system]);
    return join_path(path, data_dir, name);
}
static bool write_controls(enum CartridgeSystem system, const ControlProfile *profile) {
    char path[PATH_CAP];
    if (controls_path(path, system) && write_config(path, profile)) return true;
    status("Could not save control layout");
    return false;
}
static void activate_controls(enum CartridgeSystem system) {
    memcpy(buttons, control_profiles[system].overlays, sizeof buttons);
    memcpy(physical_map, control_profiles[system].physical, sizeof physical_map);
}
static void store_controls(void) {
    ControlProfile *profile = &control_profiles[active_system];
    memcpy(profile->overlays, buttons, sizeof buttons);
    memcpy(profile->physical, physical_map, sizeof physical_map);
    (void)write_controls(active_system, profile);
}
static bool read_overlay(FILE *f, int version, int row, ControlProfile *profile) {
    char line[80];
    int x, y, size, alpha, show;
    unsigned keys = 0;
    if (!fgets(line, sizeof line, f) ||
        sscanf(line, "%d %d %d %d %d %u", &x, &y, &size, &alpha, &show, &keys) < (version < 6 ? 5 : 6))
        return false;
    int which = version < 3 ? previous_overlays[row] : row;
    if (which < 0) return true;
    Button *b = &profile->overlays[which];
    if (version < 2) {
        const int *old = legacy_geometry[row];
        if (x == old[0] && y == old[1]) { x = b->x; y = b->y; }
        if (size == old[2]) size = b->size;
    } else if (version == 2 && y == 260 &&
               ((which == QUICK_SAVE && x == 122) || (which == QUICK_LOAD && x == 174))) {
        y = b->y;
    }
    if (version < 4) {
        if (which == MENU_BUTTON && size == 44) size = b->size;
        if (which == SPEED && y == 260 && (x == 66 || x == 70)) x = b->x;
    }
    if (version < 7) {
        if (size == b->size && x == old_default_positions[which][0] &&
            y == old_default_positions[which][1]) { x = b->x; y = b->y; }
        if (alpha == old_default_opacity[which]) alpha = b->opacity;
    } else if (version == 7 && size == b->size &&
               x == test_default_positions[which][0] && y == test_default_positions[which][1]) {
        x = b->x;
    }
    b->size = clamp(size, 36, 88);
    place_button(which, b, x, y);
    b->opacity = clamp(alpha, 16, 100);
    b->visible = which == MENU_BUTTON || show != 0;
    if (version >= 6 && remappable_overlay(which) && valid_action(keys)) b->keys = keys;
    return true;
}
static void read_config(const char *path, ControlProfile *profile, bool app_settings) {
    FILE *f = fopen(path, "r");
    int saved_scale = FIT, version = 1, rate = 2, volume = 100, saved_filter = SHARP_BILINEAR, resume = 0;
    int saved_quit_off = 0;
    char line[80];
    if (!f) return;
    if (fgets(line, sizeof line, f))
        sscanf(line, "%d %d %d %d %d %d %d", &saved_scale, &version, &rate, &volume,
               &saved_filter, &resume, &saved_quit_off);
    if (app_settings) {
        local_volume = clamp(volume, 0, 100);
        platform_audio_attenuation(local_volume);
        if (saved_scale >= FIT && saved_scale < SCALE_COUNT) scaling = saved_scale;
        video_filter = saved_filter == NEAREST ? NEAREST : SHARP_BILINEAR;
        if (rate == 2 || rate == 3 || rate == 5) fast_rate = rate == 5 ? 3 : rate;
        resume_latest = resume == 1;
        quit_off = saved_quit_off == 1;
    }
    int rows = version < 3 ? (int)(sizeof previous_overlays / sizeof previous_overlays[0]) :
               version < 6 ? TOUCH_A : TOUCH_X;
    for (int row = 0; row < rows; ++row)
        if (!read_overlay(f, version, row, profile)) break;
    if (version >= 6) {
        for (int i = 0; i < PHYSICAL_COUNT; ++i) {
            unsigned keys;
            if (!fgets(line, sizeof line, f)) break;
            if (sscanf(line, "%u", &keys) == 1 && valid_action(keys)) profile->physical[i] = keys;
        }
    }
    if (version >= 9) {
        for (int row = TOUCH_X; row < CONTROLS; ++row)
            if (!read_overlay(f, version, row, profile)) break;
    }
    fclose(f);
}
static void load_config(void) {
    memcpy(inherited_controls.overlays, defaults, sizeof buttons);
    memcpy(inherited_controls.physical, physical_defaults, sizeof physical_map);
    read_config(config_file, &inherited_controls, true);
    for (int system = 0; system < SYSTEM_COUNT; ++system) {
        char path[PATH_CAP];
        control_profiles[system] = inherited_controls;
        if (controls_path(path, system)) read_config(path, &control_profiles[system], false);
    }
    activate_controls(active_system);
}
static void copy_controls(enum CartridgeSystem source, enum CartridgeSystem target) {
    const ControlProfile *from = &control_profiles[source];
    ControlProfile next = control_profiles[target];
    for (int i = 0; i < CONTROLS; ++i) {
        if (supports_overlay(source, i) && supports_overlay(target, i) &&
            supports_action(source, next.overlays[i].keys) && supports_action(target, from->overlays[i].keys))
            next.overlays[i] = from->overlays[i];
    }
    for (int i = 0; i < PHYSICAL_COUNT; ++i) {
        if (supports_action(source, next.physical[i]) && supports_action(target, from->physical[i]))
            next.physical[i] = from->physical[i];
    }
    if (!write_controls(target, &next)) return;
    control_profiles[target] = next;
    if (target == active_system) activate_controls(target);
    status("Shared controls copied");
}
static bool save_path(char path[PATH_CAP], const char *suffix, bool writing) {
    if (!file_path(path, PATH_CAP, game_base, suffix)) return false;
    struct stat64 st;
    if (!writing && lstat64(path, &st) && (errno == ENOENT || errno == ENOTDIR))
        return file_path(path, PATH_CAP, legacy_game_base, suffix);
    return true;
}

static bool state_path(char path[PATH_CAP], int number, bool writing) {
    char extension[24];
    snprintf(extension, sizeof extension, ".state%02d", number + 1);
    return save_path(path, extension, writing);
}
static bool state_stat(int number, struct stat64 *st) {
    char path[PATH_CAP];
    return state_path(path, number, false) && !stat64(path, st) && S_ISREG(st->st_mode);
}
static bool state_info(int number, time_t *when) {
    struct stat64 st;
    if (!state_stat(number, &st)) return false;
    if (when) *when = st.st_mtime;
    return true;
}
typedef struct { void *bytes; size_t size; } SavedMemory;
static SavedMemory saved_ram, saved_rtc;
static void battery_io(unsigned kind, const char *suffix, bool save, SavedMemory *saved) {
    char path[PATH_CAP];
    size_t size = core->get_memory_size(kind);
    void *data = core->get_memory_data(kind);
    if (!size || !data || !save_path(path, suffix, save)) return;
    if (saved->size != size) {
        free(saved->bytes);
        saved->bytes = malloc(size);
        saved->size = 0;
    }
    if (save) {
        if (saved->size == size && !memcmp(saved->bytes, data, size)) return;
        if (!file_write(path, data, size)) { status("Could not write in-game save"); return; }
    } else {
        FILE *file = fopen(path, "rb");
        if (file) { (void)fread(data, 1, size, file); fclose(file); }
    }
    if (saved->bytes) {
        memcpy(saved->bytes, data, size);
        saved->size = size;
    }
}
static void cartridge_saves(bool save) {
    battery_io(RETRO_MEMORY_SAVE_RAM, ".srm", save, &saved_ram);
    if (core == &shared_cores[GAMBATTE].api || core == &shared_cores[SNES9X2005].api)
        battery_io(RETRO_MEMORY_RTC, ".rtc", save, &saved_rtc);
}
static bool state_io(enum Action action, int number) {
    size_t length = core->serialize_size();
    char path[PATH_CAP];
    if (!length || !state_path(path, number, action == SAVE)) return false;
    void *data = malloc(length);
    if (!data) return false;
    bool ok = false;
    if (action == SAVE) {
        ok = core->serialize(data, length) && file_write(path, data, length);
        if (ok && save_path(path, ".latest-state", true)) {
            unsigned char last_saved = number;
            if (!file_write(path, &last_saved, sizeof last_saved))
                fprintf(stderr, "Could not record latest save slot\n");
        }
    } else {
        FILE *f = fopen(path, "rb");
        if (f) {
            ok = fread(data, 1, length, f) == length && fgetc(f) == EOF && !ferror(f);
            fclose(f);
            if (ok) ok = core->unserialize(data, length);
        }
    }
    free(data);
    return ok;
}
static int latest_state(void) {
    size_t bytes = core->serialize_size();
    if (!bytes) return -1;
    struct timespec latest = {0};
    int newest = -1;
    int last_saved = -1;
    char path[PATH_CAP];
    if (save_path(path, ".latest-state", false)) {
        FILE *file = fopen(path, "rb");
        if (file) {
            last_saved = fgetc(file);
            fclose(file);
        }
    }
    for (int i = 0; i < SLOTS; ++i) {
        struct stat64 st;
        if (!state_stat(i, &st) || st.st_size != (off64_t)bytes) continue;
        bool later = st.st_mtime > latest.tv_sec ||
                     (st.st_mtime == latest.tv_sec && st.st_mtim.tv_nsec > latest.tv_nsec);
        bool tied = st.st_mtime == latest.tv_sec && st.st_mtim.tv_nsec == latest.tv_nsec;
        if (newest < 0 || later || (tied && (i == last_saved || newest != last_saved))) {
            latest = st.st_mtim;
            newest = i;
        }
    }
    return newest;
}
static void set_screen(enum Screen next) {
    if (!smoke && running && (screen == GAME || next == GAME)) platform_audio_reset();
    screen = next;
    game_edge.direction = 0;
    touching_flow = false;
    browser_actions_open = false;
    flow_position = shelf->selected;
    flow_started = 0;
    previous_shelf = NULL;
    title_since = clock_ns();
    last_activity = title_since;
    physical_keys = gameplay_buttons = 0;
    menu_focus = wheel_delta = 0;
    pressed_this_frame = 0;
    speed_start = 0;
    next_video = 0;
    measured_speed = 0;
    touch_keys = 0;
    touch_button = -1;
    dirty = 1;
}
static bool do_state(enum Action a) {
    bool ok = state_io(a, slot);
    char message[60];
    snprintf(message, sizeof message, "%s slot %02d",
             ok ? (a == SAVE ? "Saved to" : "Loaded") : (a == SAVE ? "Could not save" : "Could not load"),
             slot + 1);
    status(message);
    return ok;
}
static void ask_state(enum Action a) {
    bool exists = state_info(slot, NULL);
    if (a == LOAD && !exists) { status("This slot is empty"); return; }
    if (a == LOAD || exists) {
        confirm_action = a;
        confirm_from = screen;
        set_screen(CONFIRM);
    } else if (do_state(a) && screen != GAME) set_screen(GAME);
}
static void set_fast(void) {
    fast = !fast;
    speed_start = 0;
    next_video = 0;
    core_options_changed = true;
    measured_speed = 0;
    if (fast) fprintf(stderr, "Fast-forward on (%dx target)\n", fast_rate);
    else fprintf(stderr, "Fast-forward off\n");
    platform_audio_reset();
    dirty = 1;
}
static bool emulation_active(void) {
    return running && screen == GAME;
}
static void load_library(void) {
    FILE *file = fopen(library_file, "rb");
    if (!file) return;
    size_t header_size = offsetof(__typeof__(library), recent);
    if (fread(&library, 1, header_size, file) != header_size ||
        library.version != 1 || library.sort > SORT_COUNT || library.list > 1) {
        memset(&library, 0, sizeof library);
    } else {
        /* v0.2.5 stored a separate System sort. Shelves now always group by system. */
        if (library.sort == SORT_COUNT) library.sort = SORT_TITLE;
        while (recent_count < ROM_LIMIT &&
               fread(library.recent[recent_count], sizeof library.recent[0], 1, file) == 1) {
            char *name = library.recent[recent_count];
            if (!name[0] || !memchr(name, 0, sizeof library.recent[0])) break;
            ++recent_count;
        }
    }
    fclose(file);
}

static void store_library(void) {
    library.version = 1;
    size_t length = offsetof(__typeof__(library), recent) + recent_count * sizeof library.recent[0];
    if (!file_write(library_file, &library, length)) fprintf(stderr, "Could not save ROM library settings\n");
}

static void load_favorites(void) {
    favorite_count = 0;
    FILE *file = fopen(favorites_file, "rb");
    if (!file) return;
    RomName name;
    while (favorite_count < ROM_LIMIT && fread(name, sizeof name, 1, file) == 1) {
        if (memchr(name, 0, sizeof name) && rom_system(name) != SYSTEM_COUNT)
            memcpy(favorite_names[favorite_count++], name, sizeof name);
    }
    fclose(file);
}

static int favorite_index(const char *name) {
    for (int i = 0; i < favorite_count; ++i)
        if (!strcmp(favorite_names[i], name)) return i;
    return -1;
}

static int rom_order(const Rom *a, const Rom *b) {
    if (library.sort == SORT_RECENT && a->recent_rank != b->recent_rank)
        return a->recent_rank - b->recent_rank;
    int compare = strncasecmp(a->name, b->name, a->title_length < b->title_length
                                              ? a->title_length : b->title_length);
    if (!compare) compare = a->title_length - b->title_length;
    return compare ? compare : strcmp(a->name, b->name);
}

static int rom_compare(const void *left, const void *right) {
    const Rom *a = left, *b = right;
    if (a->system != b->system) return (int)a->system - (int)b->system;
    return rom_order(a, b);
}

static int favorite_compare(const void *left, const void *right) {
    return rom_order(&roms[*(const int *)left], &roms[*(const int *)right]);
}

static const Rom *shelf_rom(const RomShelf *page, int index) {
    if (page == &shelves[FAVORITES]) index = favorite_roms[index];
    else if (page == &shelves[RECENT]) index = recent_roms[index];
    return &roms[index];
}

static bool shelf_has_games(const RomShelf *page) { return page->first < page->end; }
static bool shelf_is_console(const RomShelf *page) { return page - shelves < SYSTEM_COUNT; }
static bool shelf_available(const RomShelf *page) {
    return !shelf_is_console(page) || shelf_has_games(page);
}
static bool browser_list(void) { return library.list && shelf_is_console(shelf); }

static void remember_highlights(RomName names[SHELF_COUNT]) {
    for (int i = 0; i < SHELF_COUNT; ++i) {
        const RomShelf *page = &shelves[i];
        names[i][0] = 0;
        if (shelf_has_games(page)) strcpy(names[i], shelf_rom(page, page->selected)->name);
    }
}

static void sort_library(RomName names[SHELF_COUNT]) {
    for (int i = 0; i < rom_count; ++i) {
        int rank = 0;
        while (rank < recent_count && strcmp(roms[i].name, library.recent[rank])) ++rank;
        roms[i].recent_rank = rank;
    }
    qsort(roms, rom_count, sizeof roms[0], rom_compare);
    int i = 0;
    for (int system = 0; system < SYSTEM_COUNT; ++system) {
        RomShelf *page = &shelves[system];
        page->first = page->selected = i;
        while (i < rom_count && (int)roms[i].system == system) {
            if (!strcmp(roms[i].name, names[system])) page->selected = i;
            ++i;
        }
        page->end = i;
    }
    RomShelf *favorites = &shelves[FAVORITES];
    *favorites = (RomShelf){0};
    for (i = 0; i < rom_count; ++i)
        if (favorite_index(roms[i].name) >= 0) favorite_roms[favorites->end++] = i;
    qsort(favorite_roms, favorites->end, sizeof favorite_roms[0], favorite_compare);
    for (i = 0; i < favorites->end; ++i)
        if (!strcmp(shelf_rom(favorites, i)->name, names[FAVORITES])) favorites->selected = i;
    RomShelf *recent = &shelves[RECENT];
    *recent = (RomShelf){0};
    for (int rank = 0; rank < recent_count && recent->end < RECENT_LIMIT; ++rank) {
        for (i = 0; i < rom_count; ++i) {
            if (roms[i].recent_rank != rank) continue;
            if (!strcmp(roms[i].name, names[RECENT])) recent->selected = recent->end;
            recent_roms[recent->end++] = i;
            break;
        }
    }
    if (!shelf_available(shelf)) shelf = recent;
    flow_position = shelf->selected;
    flow_started = 0;
    previous_shelf = NULL;
    title_since = clock_ns();
    dirty = 1;
}

static void toggle_favorite(int row) {
    const char *name = shelf_rom(shelf, row)->name;
    int index = favorite_index(name);
    int selection = row;
    RomName selected[SHELF_COUNT];
    remember_highlights(selected);
    if (index >= 0) {
        --favorite_count;
        memmove(favorite_names + index, favorite_names + index + 1,
                (favorite_count - index) * sizeof favorite_names[0]);
    } else {
        if (favorite_count == ROM_LIMIT) { status("Favorites is full"); return; }
        memset(favorite_names[favorite_count], 0, sizeof favorite_names[0]);
        strcpy(favorite_names[favorite_count++], name);
    }
    if (!file_write(favorites_file, favorite_names, favorite_count * sizeof favorite_names[0])) {
        load_favorites();
        status("Could not save Favorites");
        return;
    }
    sort_library(selected);
    if (shelf == &shelves[FAVORITES] && index >= 0 && shelf_has_games(shelf)) {
        shelf->selected = clamp(selection, shelf->first, shelf->end - 1);
        flow_position = shelf->selected;
    }
}

static const char *rom_directory(const Rom *rom) {
    return rom->legacy ? data_paths.roms : data_paths.system_roms[rom->system];
}

static bool name_already_indexed(const char *name) {
    for (int i = 0; i < rom_count; ++i)
        if (!strcasecmp(roms[i].name, name)) return true;
    return false;
}

static void scan_rom_directory(const char *directory, enum CartridgeSystem expected) {
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent64 *entry;
    while (rom_count < ROM_LIMIT && (entry = readdir64(dir))) {
        const char *name = entry->d_name;
        enum CartridgeSystem system = rom_system(name);
        bool legacy = expected == SYSTEM_COUNT;
        if (name[0] == '.' || system == SYSTEM_COUNT || (!legacy && system != expected) ||
            (legacy && name_already_indexed(name))) continue;
        char path[PATH_CAP];
        struct stat64 st;
        if (!join_path(path, directory, name) || stat64(path, &st) || !S_ISREG(st.st_mode)) continue;
        Rom *rom = &roms[rom_count++];
        strcpy(rom->name, name);
        rom->title_length = (int)(strrchr(rom->name, '.') - rom->name);
        rom->system = system;
        rom->legacy = legacy;
    }
    closedir(dir);
}

static void scan_roms(void) {
    RomName selected[SHELF_COUNT];
    remember_highlights(selected);
    rom_count = 0;
    cover_clear();
    for (int system = 0; system < SYSTEM_COUNT; ++system)
        scan_rom_directory(data_paths.system_roms[system], system);
    scan_rom_directory(data_paths.roms, SYSTEM_COUNT);
    sort_library(selected);
}

static void remember_launch(void) {
    RomName selected[SHELF_COUNT];
    remember_highlights(selected);
    int rank = shelf_rom(shelf, shelf->selected)->recent_rank;
    if (!rank && recent_count) return;
    int move = clamp(rank, 0, ROM_LIMIT - 1);
    memmove(library.recent + 1, library.recent, move * sizeof library.recent[0]);
    memset(library.recent[0], 0, sizeof library.recent[0]);
    strcpy(library.recent[0], selected[shelf - shelves]);
    if (rank == recent_count && recent_count < ROM_LIMIT) ++recent_count;
    sort_library(selected);
    store_library();
}

static void close_rom(void);
static void finish_rom(bool save);
static bool load_rom(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    enum CartridgeSystem system = rom_system(path);
    char base[PATH_CAP], legacy_base[PATH_CAP];
    if (system == SYSTEM_COUNT || strlen(path) >= sizeof active_rom ||
        !join_path(base, data_paths.system_saves[system], name) ||
        !join_path(legacy_base, data_paths.saves, name)) return false;
    struct stat64 file;
    if (stat64(path, &file) || !S_ISREG(file.st_mode) || file.st_size < 192 ||
        (uint64_t)file.st_size > 32 * 1024 * 1024ull) return false;
    const struct Core *selected = &gpsp;
    const char *label = use_dynarec ? "gpSP dynarec" : "gpSP interpreter";
    if (system != SYSTEM_GBA) {
        enum SharedCoreId id = system == SYSTEM_NES ? FCEUMM : system == SYSTEM_SNES ? SNES9X2005 : GAMBATTE;
        SharedCore *module = &shared_cores[id];
        if (!load_shared_core(module)) {
            char message[75];
            snprintf(message, sizeof message, "%s core unavailable in .boy", module->label);
            status(message);
            return false;
        }
        selected = &module->api;
        label = module->label;
    }
    struct retro_system_info system_info = {0};
    selected->get_system_info(&system_info);
    struct retro_game_info info = {.path = path};
    void *rom_data = NULL;
    if (!system_info.need_fullpath) {
        rom_data = malloc(file.st_size);
        FILE *rom = rom_data ? fopen(path, "rb") : NULL;
        bool read = rom && fread(rom_data, 1, file.st_size, rom) == (size_t)file.st_size;
        if (rom) fclose(rom);
        if (!read) { free(rom_data); return false; }
        info.data = rom_data;
        info.size = file.st_size;
    }
    if (running) close_rom();
    initialize_core(selected);
    fprintf(stderr, "Opening %s (%ld bytes), core=%s\n", name, (long)file.st_size, label);
    launch_stage = 1;
    bool loaded = core->load_game(&info);
    free(rom_data);
    if (!loaded) {
        launch_stage = 0;
        release_core();
        return false;
    }
    launch_stage = 2;
    struct retro_system_av_info av = {0};
    core->get_system_av_info(&av);
    fps = av.timing.fps >= 45 && av.timing.fps <= 65 ? av.timing.fps : 59.7275;
    cart_width = av.geometry.base_width;
    cart_height = av.geometry.base_height;
    if (!cart_width || !cart_height || cart_width > CART_WIDTH || cart_height > CART_HEIGHT) {
        core->unload_game();
        release_core();
        return false;
    }
    if (!smoke && av.timing.sample_rate >= 8000 && av.timing.sample_rate <= 192000)
        platform_audio_set_rate((unsigned)(av.timing.sample_rate + 0.5));
    active_system = system;
    activate_controls(system);
    strcpy(game_base, base);
    strcpy(legacy_game_base, legacy_base);
    strcpy(active_rom, path);
    cartridge_saves(false);
    running = 1;
    game_controls_hidden = false;
    slot = 0;
    fast = 0;
    have_frame = false;
    memset(cart, 0, sizeof cart);
    next_save = clock_ns() + 20000000000ull;
    set_screen(GAME);
    fprintf(stderr, "ROM loaded: %s (%.3f fps)\n", name, fps);
    return true;
}
static void launch_selected(void) {
    char path[PATH_CAP];
    if (!shelf_has_games(shelf)) return;
    notice_end = 0;
    const Rom *rom = shelf_rom(shelf, shelf->selected);
    if (!join_path(path, rom_directory(rom), rom->name)) return;
    if (running && !strcmp(active_rom, path)) { set_screen(GAME); return; }

    char previous[PATH_CAP];
    int previous_slot = slot, previous_fast = fast;
    size_t length = running ? core->serialize_size() : 0;
    void *snapshot = length ? malloc(length) : NULL;
    if (running) {
        if (!snapshot || !core->serialize(snapshot, length)) {
            free(snapshot);
            status("Could not preserve the current game");
            return;
        }
        strcpy(previous, active_rom);
    }
    bool loaded = load_rom(path);
    if (loaded && resume_latest) {
        int newest = latest_state();
        if (newest >= 0) {
            if (state_io(LOAD, newest)) slot = newest;
            else {
                finish_rom(false);
                loaded = load_rom(path);
                if (loaded) status("Save state unreadable. Started normally.");
            }
        }
    }
    if (loaded) {
        remember_launch();
    } else {
        if (!running && snapshot && load_rom(previous)) {
            bool restored = core->unserialize(snapshot, length);
            slot = previous_slot;
            fast = previous_fast;
            core_options_changed = true;
            set_screen(BROWSER);
            if (!restored) status("Previous game restarted after ROM error");
        }
        if (notice_end <= clock_ns()) status("Could not open this ROM");
    }
    free(snapshot);
}
static void finish_rom(bool save) {
    if (running) {
        launch_stage = 5;
        if (save) cartridge_saves(true);
        core->unload_game();
    }
    free(saved_ram.bytes);
    free(saved_rtc.bytes);
    saved_ram = (SavedMemory){0};
    saved_rtc = (SavedMemory){0};
    launch_stage = 0;
    release_core();
    running = 0;
    active_rom[0] = 0;
    fast = 0;
    if (!smoke) platform_audio_reset();
}
static void close_rom(void) { finish_rom(true); }
typedef struct { int x, y, w, h; bool enabled; char label[96]; } MenuItem;
static const char *const picture_modes[SCALE_COUNT] = {
    "Original aspect", "Crop to fill", "Stretch to fill", "Integer Scale"
};
static const char *filter_label(void) {
    return video_filter == SHARP_BILINEAR ? "Filter: Sharp Bilinear" : "Filter: Nearest";
}

static int menu_items(MenuItem *items) {
    int count = 0;
    char label[96];
#define ITEM(x_, y_, w_, h_, label_, enabled_) do { \
    MenuItem *m = &items[count++]; \
    *m = (MenuItem){x_, y_, w_, h_, enabled_, {0}}; \
    snprintf(m->label, sizeof m->label, "%s", (label_)); \
} while (0)
    switch (screen) {
    case APP_MENU:
        ITEM(15, 52, 345, 40, resume_latest ? "Launch: Resume latest save state" : "Launch: Start normally", true);
        snprintf(label, sizeof label, "Picture: %s", picture_modes[scaling]);
        ITEM(15, 95, 345, 40, label, true);
        ITEM(15, 138, 345, 40, filter_label(), true);
        ITEM(15, 181, 345, 40, "Copy control layout", true);
        ITEM(15, 224, 345, 40, quit_off ? "Force close: Off" : "Force close: Hold Return + Centre", firmware_keys);
        ITEM(15, 267, 345, 40, "Back", true);
        break;
    case COPY_TARGET: case COPY_SOURCE:
        for (int system = 0; system < SYSTEM_COUNT; ++system) {
            if (screen == COPY_SOURCE && system == (int)copy_target) continue;
            int y = 51 + count * 42;
            ITEM(15, y, 345, 39, shelf_names[system], true);
        }
        ITEM(15, 267, 345, 40, "Back", true);
        break;
    case BROWSER_OPTIONS:
        for (int i = 0; i < SORT_COUNT; ++i) {
            snprintf(label, sizeof label, "%s%s", sort_labels[i], library.sort == i ? "  (current)" : "");
            ITEM(35, 52 + i * 43, 305, 39, label, true);
        }
        ITEM(35, 138, 305, 39, library.list ? "Consoles: Cover Flow" : "Consoles: List view", true);
        ITEM(35, 181, 305, 39, "Refresh ROMs and covers", true);
        break;
    case MENU:
        ITEM(15, 50, 170, 41, "Resume", true);
        ITEM(190, 50, 170, 41, "Q2 Now Playing", native_supported && !pending_action);
        ITEM(15, 94, 345, 41, "Save states", true);
        ITEM(15, 138, 170, 41, fast ? "Fast-forward: On" : "Fast-forward: Off", true);
        snprintf(label, sizeof label, "Speed: %dx", fast_rate);
        ITEM(190, 138, 170, 41, label, true);
        ITEM(15, 182, 170, 41, "Display and controls", true);
        ITEM(190, 182, 170, 41, "Audio", true);
        ITEM(15, 226, 170, 41, "ROM library", true);
        ITEM(190, 226, 170, 41, "Exit to Q2-Pod", true);
        break;
    case STATES:
        ITEM(15, 101, 168, 53, "Previous slot", true);
        ITEM(192, 101, 168, 53, "Next slot", true);
        ITEM(15, 163, 168, 57, "Save state", true);
        ITEM(192, 163, 168, 57, "Load state", state_info(slot, NULL));
        ITEM(15, 235, 345, 51, "Back", true);
        break;
    case SETTINGS:
        snprintf(label, sizeof label, "Picture: %s", picture_modes[scaling]);
        ITEM(15, 50, 345, 40, label, true);
        ITEM(15, 93, 345, 40, filter_label(), true);
        ITEM(15, 136, 345, 40, "Physical buttons", true);
        ITEM(15, 179, 345, 40, "Touch buttons", true);
        ITEM(15, 222, 345, 40, "Move, resize, or fade buttons", true);
        ITEM(15, 267, 132, 40, "Back", true);
        ITEM(152, 267, 208, 40, "Reset controls", true);
        break;
    case PHYSICAL:
        for (int i = 0; i < PHYSICAL_COUNT; ++i) {
            snprintf(label, sizeof label, "%s: %s", physical_names[i], action_name(physical_map[i]));
            ITEM(15, 49 + i * 43, 345, 39, label, true);
        }
        ITEM(15, 267, 345, 40, "Back", true);
        break;
    case TOUCH:
    case ASSIGN: {
        bool touch = screen == TOUCH;
        int page = touch ? touch_page : assign_page;
        int choices[CONTROLS], total = control_choices(touch, choices);
        for (int i = 0; i < 6; ++i) {
            int k = page * 6 + i;
            if (k >= total) break;
            if (touch) {
                int id = choices[k];
                snprintf(label, sizeof label, "%s: %s", button_label(id, &buttons[id]),
                         buttons[id].visible ? "On" : "Off");
            } else snprintf(label, sizeof label, "%s", actions[choices[k]].name);
            ITEM(15 + (i % 2) * 175, 58 + (i / 2) * 60, 170, 51, label, true);
        }
        ITEM(15, 252, 110, 49, "Previous page", page > 0);
        ITEM(132, 252, 110, 49, "Next page", (page + 1) * 6 < total);
        ITEM(249, 252, 111, 49, "Back", true);
        break;
    }
    case EDIT:
        ITEM(14, 100, 346, 42, "Choose next overlay", true);
        ITEM(10, 148, 68, 44, "Size -", true);
        ITEM(82, 148, 68, 44, "Size +", true);
        ITEM(154, 148, 68, 44, "Fade -", true);
        ITEM(226, 148, 68, 44, "Fade +", true);
        ITEM(298, 148, 67, 44, "Done", true);
        ITEM(14, 198, 182, 40, "Assign action", remappable_overlay(edit_button));
        break;
    case AUDIO:
        ITEM(15, 54, 168, 51, music_mode ? "Game audio" : "Game audio (on)", audio_bridge && music_mode && !pending_action);
        ITEM(192, 54, 168, 51, music_mode ? "Q2 music (on)" : "Q2 music", audio_bridge && !music_mode && !pending_action);
        ITEM(15, 123, 168, 55, "Game volume -", local_volume > 0);
        ITEM(192, 123, 168, 55, "Game volume +", local_volume < 100);
        ITEM(15, 235, 345, 52, "Back to emulator menu", true);
        break;
    case CONFIRM:
        ITEM(15, 98, 169, 69, "Cancel", true);
        ITEM(192, 98, 168, 69, confirm_action == SAVE ? "Overwrite" : "Load state", true);
        break;
    default: break;
    }
#undef ITEM
    return count;
}
#define CAPTION_EDGE 36
#define MARQUEE_WAIT_MS 950
#define MARQUEE_SPEED 32
#define FLOW_NS 170000000ull
#define BROWSER_FRAME_NS 33333333ull
#define SHELF_INK RGB(0x000000)

static int glyph_index(unsigned char ch) {
    return ch < 32 || ch > 126 ? '?' - 32 : ch - 32;
}

static int text_width(const char *text, const unsigned char *widths) {
    int width = 0;
    for (; *text; ++text) width += widths[glyph_index((unsigned char)*text)];
    return width;
}

static int marquee_offset(int overflow, uint64_t now) {
    if (overflow <= 0 || now <= title_since) return 0;
    uint64_t ms = (now - title_since) / 1000000;
    unsigned sweep = (overflow * 1000 + MARQUEE_SPEED - 1) / MARQUEE_SPEED;
    if (ms <= MARQUEE_WAIT_MS || ms >= 2 * (sweep + MARQUEE_WAIT_MS)) return 0;
    unsigned at = (unsigned)ms - MARQUEE_WAIT_MS;
    if (at >= 2 * sweep + MARQUEE_WAIT_MS) return 0;
    if (at > sweep + MARQUEE_WAIT_MS) at = 2 * sweep + MARQUEE_WAIT_MS - at;
    if (at > sweep) at = sweep;
    return (int)(at * MARQUEE_SPEED / 1000);
}

static void caption(int y, const char *label, uint32_t ink, int size, uint64_t now) {
    bool title = size == 24;
    const unsigned char *widths = title ? advance_24 : advance_20;
    int w = text_width(label, widths), h = title ? 28 : 24;
    int x = (WIDTH - w) / 2;
    int max_width = WIDTH - CAPTION_EDGE * 2;
    if (w > max_width) x = CAPTION_EDGE - marquee_offset(w - max_width, now);
    for (; *label && x < WIDTH - CAPTION_EDGE; ++label) {
        int glyph = glyph_index((unsigned char)*label);
        const void *mask = title ? (const void *)&glyphs_24[glyph] : &glyphs_20[glyph];
        draw_mask(x, y, 26, h, mask, ink, CAPTION_EDGE, WIDTH - CAPTION_EDGE);
        x += widths[glyph];
    }
}

static void selected_title(const RomShelf *page, int index, char title[256]) {
    const Rom *rom = shelf_rom(page, index);
    memcpy(title, rom->name, rom->title_length);
    title[rom->title_length] = 0;
}
static bool heart_position(const Rom *rom, float distance, int *x, int *y) {
    return cover_badge_position(rom_directory(rom), rom->legacy ? NULL : data_paths.roms,
                                rom->name, rom->system, distance, HEART_TARGET, x, y);
}
static void draw_heart(const Rom *rom, int x, int y) {
    const bool filled = favorite_index(rom->name) >= 0;
    rect(x + 4, y + 4, HEART_TARGET - 8, HEART_TARGET - 8, RGB(0x171717));
    const int inset = (HEART_TARGET - HEART_ICON) / 2;
    draw_mask(x + inset, y + inset, HEART_ICON, HEART_ICON, &heart_mask[filled],
              filled ? RGB(0xff4848) : WHITE, 0, WIDTH);
}
static bool heart_hit(int x, int y, int left, int top) {
    return x >= left && x < left + HEART_TARGET && y >= top && y < top + HEART_TARGET;
}

static void draw_shelf(const RomShelf *page, float position, int offset_y, uint64_t now) {
    if (!shelf_has_games(page)) {
        bool recent = page == &shelves[RECENT];
        caption(111 + offset_y, recent ? "No recently played games" : "No favorites yet", WHITE, 24, 0);
        const char *hint = rom_count ? (recent ? "Swipe down to browse your games."
                                              : "Tap a game's heart to add it here.")
                                    : "Copy ROMs to q2-boy/ROMs.";
        text_at((WIDTH - text_width(hint, advance)) / 2, 154 + offset_y, hint, GRAY, WIDTH);
        caption(282 + offset_y, shelf_names[page - shelves], GRAY, 20, 0);
        return;
    }
    int mid = (int)(position + 0.5f);
    int left = clamp(mid - 4, page->first, page->end - 1);
    int right = clamp(mid + 4, page->first, page->end - 1);
    /* Paint farther cards first so the active cover overlaps its neighbors. */
    while (left <= right) {
        int i = position - left > right - position ? left++ : right--;
        const Rom *rom = shelf_rom(page, i);
        cover_draw(canvas, rom_directory(rom), rom->legacy ? NULL : data_paths.roms,
                   rom->name, rom->system, i - position, offset_y);
    }
    const Rom *center = shelf_rom(page, mid);
    int heart_x, heart_y;
    if (heart_position(center, mid - position, &heart_x, &heart_y))
        draw_heart(center, heart_x, heart_y + offset_y);
    char title[256];
    selected_title(page, mid, title);
    if (page == shelf)
        last_marquee_offset = marquee_offset(text_width(title, advance_24) -
                                              (WIDTH - CAPTION_EDGE * 2), now);
    caption(254 + offset_y, title, RGB(0xffffff), 24, now);
    caption(282 + offset_y, shelf_names[page - shelves], RGB(0xaaaaaa), 20, now);
}

static void browser_button(int x, int y, int width, const char *label, bool enabled) {
    rect(x, y, width, 38, enabled ? RGB(0x242424) : RGB(0x141414));
    text_at(x + (width - text_width(label, advance)) / 2, y + 10, label,
            enabled ? WHITE : RGB(0x777777), x + width - 4);
}

static int list_first(void) {
    return shelf->first + ((shelf->selected - shelf->first) / ROWS) * ROWS;
}

static void draw_system_indicator(void) {
    int count = 0;
    for (int i = 0; i < SHELF_COUNT; ++i)
        if (shelf_available(&shelves[i])) ++count;
    if (count < 2) return;
    int y = HEIGHT / 2 - (count - 1) * 12;
    const int x = WIDTH - 16;
    for (int i = 0; i < SHELF_COUNT; ++i) {
        const RomShelf *page = &shelves[(RECENT + i) % SHELF_COUNT];
        if (!shelf_available(page)) continue;
        bool active = shelf == page;
        int half = active ? 7 : 3;
        rect(x - 4, y - half - 1, 8, 2 * half + 2, RGB(0x101010));
        rect(x - 2, y - half + 2, 4, 2 * half - 4, active ? WHITE : RGB(0x888888));
        rect(x - 1, y - half + 1, 2, 2 * half - 2, active ? WHITE : RGB(0x888888));
        y += 24;
    }
}

static void draw_browser(void) {
    rect(0, 0, WIDTH, HEIGHT, SHELF_INK);
    if (browser_list()) {
        int start = list_first();
        for (int i = start; i < shelf->end && i < start + ROWS; ++i) {
            int y = LIST_TOP + (i - start) * LIST_ROW;
            rect(28, y, WIDTH - 56, 39, i == shelf->selected ? RGB(0x393939) : RGB(0x181818));
            char title[256];
            selected_title(shelf, i, title);
            text_at(40, y + 11, title, WHITE, LIST_HEART_X - 5);
            draw_heart(shelf_rom(shelf, i), LIST_HEART_X, y + 3);
        }
        browser_button(37, FLOW_FOOT_Y, 89, "Previous", start > shelf->first);
        browser_button(133, FLOW_FOOT_Y, 89, "Next", start + ROWS < shelf->end);
        text_at(232, FLOW_FOOT_Y + 10, system_short[shelf - shelves], GRAY, WIDTH - 30);
    } else {
        uint64_t now = clock_ns();
        int offset = previous_shelf ? system_slide_offset : 0;
        if (previous_shelf)
            draw_shelf(previous_shelf, previous_shelf->selected,
                       offset - system_slide_direction * SHELF_TRAVEL, 0);
        draw_shelf(shelf, flow_position, offset, now);
    }
    rect(0, 0, WIDTH, BROWSER_HEADER_BOTTOM, SHELF_INK);
    draw_system_indicator();
    if (!browser_actions_open) {
        draw_ellipsis(MORE_DOTS_X, BROWSER_BUTTON_Y + 19);
        return;
    }
    char sort[26];
    snprintf(sort, sizeof sort, "Sort: %s", sort_short[library.sort]);
    browser_button(SORT_X, BROWSER_BUTTON_Y, SORT_W, sort, true);
    browser_button(EXIT_X, BROWSER_BUTTON_Y, EXIT_W, running ? "Resume" : "Exit", true);
    browser_button(APP_MENU_X, BROWSER_BUTTON_Y, APP_MENU_W, "Menu", true);
}

static uint64_t update_browser(uint64_t now) {
    if (screen != BROWSER || browser_list()) return UINT64_MAX;
    if (now < flow_draw_at) return flow_draw_at;
    flow_draw_at = now + BROWSER_FRAME_NS;
    if (previous_shelf) {
        uint64_t elapsed = now >= system_slide_started ? now - system_slide_started : 0;
        if (elapsed >= SYSTEM_SLIDE_NS) {
            previous_shelf = NULL;
            system_slide_offset = 0;
        } else {
            float remaining = 1.0f - (float)elapsed / SYSTEM_SLIDE_NS;
            system_slide_offset = system_slide_direction * SHELF_TRAVEL * remaining * remaining * remaining;
        }
        dirty = 1;
    }
    if (flow_started) {
        uint64_t elapsed = now >= flow_started ? now - flow_started : 0;
        if (elapsed >= FLOW_NS) {
            flow_position = shelf->selected;
            flow_started = 0;
        } else {
            float remaining = 1.0f - (float)elapsed / FLOW_NS;
            flow_position = shelf->selected + (flow_start - shelf->selected) * remaining * remaining * remaining;
        }
        dirty = 1;
    }
    if (previous_shelf || flow_started) return flow_draw_at;
    if (shelf_has_games(shelf)) {
        uint64_t deadline = UINT64_MAX;
        if (prefetch_selection != title_since) {
            prefetch_selection = title_since;
            prefetch_at = title_since + 220000000ull;
            prefetched = 0;
        }
        if (!touching_flow && prefetched < 4) {
            if (now >= prefetch_at) {
                int distance = 4 + prefetched / 2;
                int index = shelf->selected + (prefetched % 2 ? -distance : distance);
                if (index >= shelf->first && index < shelf->end) {
                    const Rom *rom = shelf_rom(shelf, index);
                    cover_prefetch(rom_directory(rom), rom->legacy ? NULL : data_paths.roms,
                                   rom->name, rom->system);
                }
                ++prefetched;
                prefetch_at = clock_ns() + 50000000ull;
            }
            if (prefetched < 4) deadline = prefetch_at;
        }
        char title[256];
        selected_title(shelf, shelf->selected, title);
        int overflow = text_width(title, advance_24) - (WIDTH - CAPTION_EDGE * 2);
        if (overflow > 0) {
            uint64_t start = title_since + MARQUEE_WAIT_MS * 1000000ull;
            uint64_t sweep = (overflow * 1000ull + MARQUEE_SPEED - 1) / MARQUEE_SPEED * 1000000ull;
            uint64_t returning = start + sweep + MARQUEE_WAIT_MS * 1000000ull;
            uint64_t end = returning + sweep;
            if (marquee_offset(overflow, now) != last_marquee_offset) dirty = 1;
            if (now < start) deadline = sooner(deadline, start);
            else if (now < start + sweep || (now >= returning && now < end))
                deadline = sooner(deadline, sooner(flow_draw_at, end));
            else if (now < returning) deadline = sooner(deadline, returning);
        }
        return deadline;
    }
    return UINT64_MAX;
}

static void draw_ui(void) {
    canvas = platform_canvas();
    char label[256], sub[128];
    switch (screen) {
    case GAME: draw_game(); break;
    case BROWSER: draw_browser(); break;
    case APP_MENU:
        header("Pocket Boy", "Settings for all consoles");
        break;
    case COPY_TARGET:
        header("Copy control layout", "Choose the console to update.");
        break;
    case COPY_SOURCE:
        snprintf(sub, sizeof sub, "Copy shared controls to %s.", shelf_names[copy_target]);
        header("Choose source console", sub);
        break;
    case BROWSER_OPTIONS:
        header("Library options", "Use the wheel and Centre, or tap an option.");
        break;
    case MENU:
        header("Emulator Menu", "Scroll to highlight. Press Centre to select.");
        break;
    case STATES: {
        time_t when;
        snprintf(sub, sizeof sub, "Slot %02d of %d", slot + 1, SLOTS);
        header("Save states", sub);
        if (state_info(slot, &when)) {
            struct tm *tm = localtime(&when);
            if (!tm || !strftime(label, sizeof label, "%Y-%m-%d  %H:%M:%S", tm))
                snprintf(label, sizeof label, "Saved");
        } else snprintf(label, sizeof label, "Empty slot");
        text_at(17, 64, label, WHITE, WIDTH - 15);
        break;
    }
    case SETTINGS:
        header("Display and controls", shelf_names[active_system]);
        break;
    case PHYSICAL:
        header("Physical buttons", "Choose a button to change its gameplay action.");
        break;
    case TOUCH:
        header("Touch buttons", "Choose which overlays appear during gameplay.");
        break;
    case ASSIGN:
        header("Assign action", assign_return == PHYSICAL ? physical_names[assign_target] : "Touch button");
        break;
    case EDIT:
        draw_game();
        rect(0, 55, WIDTH, 38, DARK);
        text_at(12, 57, "Drag an overlay or choose an action.", WHITE, WIDTH - 10);
        snprintf(label, sizeof label, "%s  %d px  %d%% opacity",
                 button_label(edit_button, &buttons[edit_button]), buttons[edit_button].size, buttons[edit_button].opacity);
        text_at(12, 73, label, ACCENT, WIDTH - 10);
        break;
    case AUDIO:
        header("Audio", pending_action == AUDIO_GAME || pending_action == AUDIO_MUSIC
                       ? "Switching output..." : music_mode ? "Q2 music is selected" : "Game audio is selected");
        snprintf(label, sizeof label, "Game volume: %d%%", local_volume);
        text_at(17, 194, label, WHITE, WIDTH - 16);
        break;
    case CONFIRM:
        snprintf(label, sizeof label, confirm_action == SAVE ? "Overwrite slot %02d?" : "Load slot %02d?",
                 slot + 1);
        header(label, confirm_action == SAVE ? "This replaces the saved state in that slot."
                                             : "Unsaved progress in this session will be lost.");
        break;
    }
    MenuItem items[9];
    int count = menu_items(items);
    for (int i = 0; i < count; ++i) {
        const MenuItem *m = &items[i];
        bool focused = m->enabled && i == menu_focus;
        uint32_t fill = focused ? RGB(0x245448) : RGB(0x16282b);
        rect(m->x, m->y, m->w, m->h, fill);
        if (focused) rect(m->x, m->y, 4, m->h, ACCENT);
        text_at(m->x + 11, m->y + (m->h - 17) / 2, m->label, m->enabled ? WHITE : GRAY,
                m->x + m->w - 6);
    }
    if (notice_end && clock_ns() < notice_end) {
        rect(37, HEIGHT - 29, WIDTH - 74, 23, DARK);
        text_at(45, HEIGHT - 26, notice, ACCENT, WIDTH - 42);
    }
    if (!smoke) platform_present(paced_video() || screen == BROWSER || screen == BROWSER_OPTIONS);
    dirty = 0;
}

static int hit(int x, int y) {
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return -1;
    if (screen == GAME && game_controls_hidden) return -1;
    int nearest = -1;
    float best = TOUCH_HIT_PADDING;
    for (int i = CONTROLS - 1; i >= 0; --i) {
        if (!overlay_visible(i)) continue;
        const Button *b = &buttons[i];
        int local_x = x - b->x, local_y = y - b->y;
        int height = button_height(i, b), radius = button_radius(i, b);
        int distance_squared = shape_distance_squared(local_x, local_y, b->size, height, radius);
        if (distance_squared < radius * radius) return i;
        int reach = radius + TOUCH_HIT_PADDING;
        if (screen == GAME && distance_squared < reach * reach) {
            float gap = sqrtf((float)distance_squared) - radius;
            if (gap < best) {
                best = gap;
                nearest = i;
            }
        }
    }
    return nearest;
}
static bool within(int x, int y, int left, int top, int width) {
    return x >= left && x < left + width && y >= top && y < top + 38;
}
static void go_back(void) {
    switch (screen) {
    case BROWSER_OPTIONS: case APP_MENU: set_screen(BROWSER); break;
    case COPY_TARGET: set_screen(APP_MENU); break;
    case COPY_SOURCE: set_screen(COPY_TARGET); break;
    case BROWSER:
        if (running) set_screen(GAME);
        else quitting = 1;
        break;
    case MENU: set_screen(GAME); break;
    case GAME: set_screen(MENU); break;
    case STATES: case SETTINGS: case AUDIO: set_screen(MENU); break;
    case EDIT: dragging = -1; store_controls(); set_screen(SETTINGS); break;
    case CONFIRM: set_screen(confirm_from); break;
    case PHYSICAL: case TOUCH: set_screen(SETTINGS); break;
    case ASSIGN: set_screen(assign_return); break;
    }
}
static void slot_step(int step) { slot = (slot + step + SLOTS) % SLOTS; dirty = 1; }
static void pick_rom(int step) {
    if (!shelf_has_games(shelf)) return;
    shelf->selected = clamp(shelf->selected + step, shelf->first, shelf->end - 1);
    if (browser_list()) flow_position = shelf->selected;
    else {
        if (flow_position < shelf->selected - 2) flow_position = shelf->selected - 2;
        if (flow_position > shelf->selected + 2) flow_position = shelf->selected + 2;
        flow_start = flow_position;
        flow_started = flow_start != shelf->selected ? clock_ns() : 0;
        flow_draw_at = 0;
    }
    title_since = clock_ns();
    dirty = 1;
}

static void pick_system(int direction) {
    int current = (int)(shelf - shelves);
    for (int step = 1; step < SHELF_COUNT; ++step) {
        RomShelf *page = &shelves[(current + step * direction + SHELF_COUNT) % SHELF_COUNT];
        if (!shelf_available(page)) continue;
        bool animate = !browser_list() && (!library.list || !shelf_is_console(page));
        previous_shelf = animate ? shelf : NULL;
        shelf = page;
        flow_position = shelf->selected;
        flow_started = flow_draw_at = 0;
        wheel_delta = 0;
        title_since = system_slide_started = clock_ns();
        system_slide_direction = direction;
        system_slide_offset = direction * SHELF_TRAVEL;
        dirty = 1;
        break;
    }
}

static void activate_menu(int index) {
    MenuItem items[9];
    int count = menu_items(items);
    if (index < 0 || index >= count || !items[index].enabled) return;
    menu_focus = index;
    switch (screen) {
    case APP_MENU:
        if (index == 0) { resume_latest = !resume_latest; store_config(); }
        else if (index == 1) { scaling = (scaling + 1) % SCALE_COUNT; store_config(); }
        else if (index == 2) {
            video_filter = video_filter == SHARP_BILINEAR ? NEAREST : SHARP_BILINEAR;
            store_config();
        } else if (index == 3) set_screen(COPY_TARGET);
        else if (index == 4) {
            if (write_quit_option(!quit_off)) { quit_off = !quit_off; store_config(); }
            else status("Could not set force-close shortcut");
        }
        else go_back();
        break;
    case COPY_TARGET:
        if (index == SYSTEM_COUNT) go_back();
        else { copy_target = index; set_screen(COPY_SOURCE); }
        break;
    case COPY_SOURCE:
        if (index == SYSTEM_COUNT - 1) go_back();
        else {
            copy_controls(index + (index >= (int)copy_target), copy_target);
            set_screen(COPY_TARGET);
        }
        break;
    case BROWSER_OPTIONS:
        if (index < SORT_COUNT) {
            RomName selected[SHELF_COUNT];
            remember_highlights(selected);
            library.sort = index;
            sort_library(selected);
            store_library();
        } else if (index == SORT_COUNT) {
            library.list = !library.list;
            store_library();
        } else { cover_cache_clear(); scan_roms(); }
        set_screen(BROWSER);
        break;
    case MENU:
        switch (index) {
        case 0: set_screen(GAME); break;
        case 1: request_audio(AUDIO_NOW_PLAYING, 0); break;
        case 2: set_screen(STATES); break;
        case 3: set_fast(); set_screen(GAME); break;
        case 4:
            fast_rate = fast_rate == speeds[0] ? speeds[1] : speeds[0];
            core_options_changed = true;
            store_config();
            break;
        case 5: set_screen(SETTINGS); break;
        case 6:
            set_screen(AUDIO);
            menu_focus = audio_bridge ? (music_mode ? 0 : 1) : 2;
            if (!audio_bridge && !local_volume) menu_focus = 3;
            break;
        case 7: cover_cache_clear(); scan_roms(); set_screen(BROWSER); break;
        case 8: quitting = 1; break;
        }
        break;
    case STATES:
        if (index < 2) slot_step(index == 0 ? -1 : 1);
        else if (index == 2) ask_state(SAVE);
        else if (index == 3) ask_state(LOAD);
        else go_back();
        break;
    case SETTINGS:
        if (index == 0) { scaling = (scaling + 1) % SCALE_COUNT; store_config(); }
        else if (index == 1) {
            video_filter = video_filter == SHARP_BILINEAR ? NEAREST : SHARP_BILINEAR;
            store_config();
        } else if (index == 2) set_screen(PHYSICAL);
        else if (index == 3) { touch_page = 0; set_screen(TOUCH); }
        else if (index == 4) { edit_button = MENU_BUTTON; set_screen(EDIT); }
        else if (index == 5) go_back();
        else {
            memcpy(buttons, defaults, sizeof buttons);
            memcpy(physical_map, physical_defaults, sizeof physical_map);
            store_controls();
            status("Default controls restored");
        }
        break;
    case PHYSICAL:
        if (index == PHYSICAL_COUNT) go_back();
        else { assign_target = index; assign_page = 0; assign_return = PHYSICAL; set_screen(ASSIGN); }
        break;
    case TOUCH: case ASSIGN: {
        bool touch = screen == TOUCH;
        int *page = touch ? &touch_page : &assign_page;
        if (index == count - 1) go_back();
        else if (index == count - 2 || index == count - 3) {
            *page += index == count - 2 ? 1 : -1;
            menu_focus = 0;
        } else {
            int choices[CONTROLS];
            (void)control_choices(touch, choices);
            int k = choices[*page * 6 + index];
            if (touch) buttons[k].visible ^= 1;
            else if (assign_return == PHYSICAL) physical_map[assign_target] = actions[k].keys;
            else buttons[assign_target].keys = actions[k].keys;
            store_controls();
            if (!touch) go_back();
        }
        break;
    }
    case EDIT: {
        Button *b = &buttons[edit_button];
        if (index == 0) {
            do { edit_button = (edit_button + 1) % CONTROLS; } while (!overlay_visible(edit_button));
        } else if (index <= 2) {
            b->size = clamp(b->size + (index == 1 ? -4 : 4), 36, 88);
            move_button(edit_button, b->x, b->y);
        } else if (index <= 4) {
            b->opacity = clamp(b->opacity + (index == 3 ? -8 : 8), 16, 100);
        } else if (index == 5) go_back();
        else { assign_target = edit_button; assign_page = 0; assign_return = EDIT; set_screen(ASSIGN); }
        break;
    }
    case AUDIO:
        if (index == 0 || index == 1) request_audio(index ? AUDIO_MUSIC : AUDIO_GAME, 0);
        else if (index == 2 || index == 3) {
            local_volume = clamp(local_volume + (index == 2 ? -5 : 5), 0, 100);
            platform_audio_attenuation(local_volume);
            store_config();
        } else go_back();
        break;
    case CONFIRM:
        if (index == 0 || !do_state(confirm_action)) go_back();
        else set_screen(GAME);
        break;
    default: break;
    }
    dirty = 1;
}
static void tap_menu(int x, int y) {
    MenuItem items[9];
    int count = menu_items(items);
    for (int i = 0; i < count; ++i) {
        const MenuItem *m = &items[i];
        if (x >= m->x && y >= m->y && x < m->x + m->w && y < m->y + m->h) {
            activate_menu(i);
            return;
        }
    }
}
static void step_menu(int step) {
    MenuItem items[9];
    int count = menu_items(items);
    if (!count) return;
    do { menu_focus = (menu_focus + step + count) % count; } while (!items[menu_focus].enabled);
    dirty = 1;
}
static void tap_cover(int x, int y) {
    int mid = (int)(flow_position + 0.5f);
    int heart_x, heart_y;
    if (heart_position(shelf_rom(shelf, mid), mid - flow_position, &heart_x, &heart_y) &&
        heart_hit(x, y, heart_x, heart_y)) {
        toggle_favorite(mid);
        return;
    }
    int closest = -1;
    float best = COVER_REACH;
    for (int i = clamp(mid - 4, shelf->first, shelf->end - 1);
         i < shelf->end && i <= mid + 4; ++i) {
        float offset = i - flow_position;
        float distance = offset < 0 ? -offset : offset;
        const Rom *rom = shelf_rom(shelf, i);
        if (distance < best && cover_hit(rom_directory(rom), rom->legacy ? NULL : data_paths.roms,
                                        rom->name, rom->system, offset, x, y)) {
            closest = i;
            best = distance;
        }
    }
    if (closest == mid) {
        shelf->selected = mid;
        launch_selected();
    } else if (closest >= 0) pick_rom(closest - shelf->selected);
}

static void browser_touch(char action, int x, int y) {
    if (action == 'd') {
        touching_flow = true;
        flow_down_x = x;
        flow_down_y = y;
        flow_drag_origin = flow_position;
        flow_gesture = TOUCH_TAP;
        return;
    }
    if (!touching_flow) return;
    int dx = x - flow_down_x, dy = y - flow_down_y;
    if (flow_gesture == TOUCH_TAP && flow_down_y >= BROWSER_HEADER_BOTTOM &&
        (abs(dx) >= 8 || abs(dy) >= 8)) {
        if (abs(dy) > abs(dx)) flow_gesture = TOUCH_VERTICAL;
        else if (!browser_list() && shelf_has_games(shelf)) flow_gesture = TOUCH_HORIZONTAL;
    }
    if (flow_gesture == TOUCH_HORIZONTAL) {
        previous_shelf = NULL;
        flow_started = 0;
        flow_position = flow_drag_origin - (float)dx / COVER_DRAG_PIXELS;
        if (flow_position < shelf->first) flow_position = shelf->first;
        if (flow_position > shelf->end - 1) flow_position = shelf->end - 1;
        int index = (int)(flow_position + 0.5f);
        if (index != shelf->selected) title_since = clock_ns();
        shelf->selected = index;
        dirty = 1;
    }
    if (action != 'u') return;
    touching_flow = false;
    if (flow_gesture == TOUCH_HORIZONTAL) { pick_rom(0); return; }
    if (flow_gesture == TOUCH_VERTICAL) {
        if (abs(dy) >= SYSTEM_GESTURE_PX) pick_system(dy > 0 ? 1 : -1);
        return;
    }
    if (!browser_actions_open && flow_down_x >= MORE_TOUCH_X && flow_down_x < WIDTH &&
        flow_down_y >= 0 && flow_down_y < BROWSER_HEADER_BOTTOM &&
        abs(dx) <= MORE_TAP_SLOP && abs(dy) <= MORE_TAP_SLOP) {
        browser_actions_open = true;
        dirty = 1;
        return;
    }
    if (abs(dx) > 15 || abs(dy) > 15) return;
    if (browser_actions_open) {
        browser_actions_open = false;
        dirty = 1;
        if (within(x, y, SORT_X, BROWSER_BUTTON_Y, SORT_W)) {
            set_screen(BROWSER_OPTIONS);
            menu_focus = library.sort;
        } else if (within(x, y, EXIT_X, BROWSER_BUTTON_Y, EXIT_W)) go_back();
        else if (within(x, y, APP_MENU_X, BROWSER_BUTTON_Y, APP_MENU_W)) set_screen(APP_MENU);
        return;
    }
    if (!shelf_has_games(shelf) || previous_shelf) return;
    if (!browser_list() && y >= BROWSER_HEADER_BOTTOM) tap_cover(x, y);
    else if (browser_list()) {
        int row = (y - LIST_TOP) / LIST_ROW;
        int index = list_first() + row;
        if (x >= 28 && x < WIDTH - 28 && y >= LIST_TOP && row < ROWS &&
            (y - LIST_TOP) % LIST_ROW < 39 && index < shelf->end) {
            if (heart_hit(x, y, LIST_HEART_X, LIST_TOP + row * LIST_ROW + 3)) toggle_favorite(index);
            else { shelf->selected = index; launch_selected(); }
        } else if (within(x, y, 37, FLOW_FOOT_Y, 89)) pick_rom(-ROWS);
        else if (within(x, y, 133, FLOW_FOOT_Y, 89)) pick_rom(ROWS);
    }
}

static void press_game_button(int which) {
    if (which < 0) return;
    touch_button = which;
    touch_keys = buttons[which].keys;
    pressed_this_frame |= touch_keys;
    dirty = 1;
    if (which == MENU_BUTTON) set_screen(MENU);
    else if (which == SPEED) set_fast();
    else if (which == QUICK_SAVE) ask_state(SAVE);
    else if (which == QUICK_LOAD) ask_state(LOAD);
}

static void press_held_edge_button(uint64_t now) {
    if (!game_edge.direction || game_edge.button < 0 || now < game_edge.press_at) return;
    game_edge.direction = 0;
    press_game_button(game_edge.button);
}

static void game_touch(char action, int x, int y) {
    if (action == 'd') {
        game_edge.direction = 0;
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
        int which = hit(x, y);
        if (x < GAME_EDGE_WIDTH || x >= WIDTH - GAME_EDGE_WIDTH) {
            /* Give a swipe first claim on an edge touch, even over a mapped button. */
            game_edge.direction = x < GAME_EDGE_WIDTH ? 1 : -1;
            game_edge.x = x;
            game_edge.y = y;
            game_edge.button = which;
            game_edge.press_at = which >= 0 ? clock_ns() + EDGE_BUTTON_HOLD_NS : 0;
        } else press_game_button(which);
        return;
    }
    if (game_edge.direction && (action == 'm' || action == 'u')) {
        int dx = x - game_edge.x, dy = abs(y - game_edge.y);
        int inward = dx * game_edge.direction;
        if (inward >= GAME_SWIPE_DISTANCE && inward >= dy) {
            game_edge.direction = 0;
            game_controls_hidden = !game_controls_hidden;
            dirty = 1;
        } else {
            if (abs(dx) > EDGE_TAP_SLOP || dy > EDGE_TAP_SLOP) game_edge.button = -1;
            if (action == 'u') {
                game_edge.direction = 0;
                press_game_button(game_edge.button);
            }
        }
    }
    if (action == 'u') { touch_keys = 0; touch_button = -1; dirty = 1; }
}

static void read_touch(char action, int x, int y) {
    if (screen == BROWSER) { browser_touch(action, x, y); return; }
    if (screen == GAME) { game_touch(action, x, y); return; }
    if (screen == EDIT && (action == 'm' || action == 'u')) {
        if (abs(x - edit_start_x) + abs(y - edit_start_y) >= 5) edit_moved = 1;
        if (action == 'm' && dragging >= 0 && edit_moved) {
            move_button(dragging, x - drag_x, y - drag_y);
        }
        if (action == 'u') {
            if (!edit_moved) tap_menu(x, y);
            dragging = -1;
        }
        dirty = 1;
        return;
    }
    if (action == 'u') { touch_keys = 0; touch_button = -1; dirty = 1; }
    if (action != 'd') return;
    switch (screen) {
    case GAME: case BROWSER: break; /* Handled above to distinguish taps from swipes. */
    case MENU: case STATES: case SETTINGS: case CONFIRM: case AUDIO: case PHYSICAL: case TOUCH:
    case ASSIGN: case BROWSER_OPTIONS: case APP_MENU: case COPY_TARGET: case COPY_SOURCE:
        tap_menu(x, y);
        break;
    case EDIT: {
        edit_start_x = x;
        edit_start_y = y;
        edit_moved = 0;
        int which = hit(x, y);
        if (which >= 0) {
            edit_button = dragging = which;
            drag_x = x - buttons[which].x;
            drag_y = y - buttons[which].y;
        }
        break;
    }
    }
    dirty = 1;
}
static void dismiss_browser_actions(void) {
    if (screen != BROWSER || !browser_actions_open) return;
    browser_actions_open = false;
    touching_flow = false;
    dirty = 1;
}

static void read_wheel(int delta) {
    if (!delta || screen == GAME) return;
    dismiss_browser_actions();
    uint64_t now = clock_ns();
    if (!wheel_time || now - wheel_time > 300000000ull) wheel_delta = 0;
    wheel_time = now;
    wheel_delta += clamp(delta, -84, 84);
    int tick = screen == BROWSER && !browser_list() ? 38 : 21;
    while (abs(wheel_delta) >= tick) {
        int step = wheel_delta < 0 ? 1 : -1;
        wheel_delta += step * tick;
        if (screen == BROWSER) pick_rom(step);
        else step_menu(step);
    }
}
static unsigned mapped_button(int id) {
    unsigned keys = physical_map[id];
    return supports_action(active_system, keys) ? keys : physical_defaults[id];
}

static int decode_key(Event e) {
    if (e.kind == 'g') {
        switch (e.x) { // Linux md-gpio-keys codes
        case 103: return P_RETURN;
        case 108: return P_PLAY;
        case 165: return P_PREVIOUS;
        case 163: return P_NEXT;
        case 116: return P_CENTRE;
        }
    }
    if (e.kind == 'k' && !raw_buttons) {
        switch (e.x) { // Q2-Pod card game codes
        case 170: return P_RETURN;
        case 171: return P_PLAY;
        case 222: return P_PREVIOUS;
        case 223: return P_NEXT;
        case 218: return P_CENTRE;
        }
    }
    return -1;
}
static void read_event(Event e) {
    if (display_bridge) {
        int key = decode_key(e);
        if (wake_touch && e.kind == 't') {
            if (e.action == 'u') wake_touch = false;
            return;
        }
        if (key >= 0 && key == wake_key) {
            if (e.action == 'u') wake_key = -1;
            return;
        }
        if (e.kind == 'r') wake_key = -1;
        if (e.kind == 'w' || e.kind == 't' || key >= 0) {
            last_activity = clock_ns();
            if (dimmed) {
                bool was_dimmed = !access(DISPLAY_DIMMED, F_OK);
                if (!file_write(DISPLAY_REQUEST, "0", 1)) return;
                dimmed = false;
                if (was_dimmed) {
                    wake_key = e.action == 'd' ? key : -1;
                    wake_touch = e.kind == 't' && e.action != 'u';
                    touching_flow = false;
                    game_edge.direction = 0;
                    held_buttons = gameplay_buttons = 0;
                    physical_keys = touch_keys = pressed_this_frame = 0;
                    return;
                }
            }
        }
    }
    if (e.kind == 'w') { read_wheel(e.x); return; }
    if (e.kind == 't') { read_touch(e.action, e.x, e.y); return; }
    if (e.kind == 'r') {
        physical_keys = pressed_this_frame = held_buttons = gameplay_buttons = 0;
        return;
    }
    int id = decode_key(e), down = e.action == 'd';
    if (id < 0 || (e.action != 'u' && !down)) return;
    unsigned bit = 1u << id;
    if (down) {
        if (held_buttons & bit) return;
        held_buttons |= bit;
        dismiss_browser_actions();
        if (screen == GAME) {
            gameplay_buttons |= bit;
            pressed_this_frame |= mapped_button(id);
        } else if (id == P_CENTRE) {
            if (screen == BROWSER) launch_selected();
            else activate_menu(menu_focus);
        } else if (screen == BROWSER) {
            if (!browser_list() && (id == P_RETURN || id == P_PLAY))
                pick_system(id == P_PLAY ? 1 : -1);
            else if (id == P_RETURN) go_back();
            else if (id == P_PLAY) pick_rom(1);
            else if (id == P_PREVIOUS) pick_rom(browser_list() ? -ROWS : -1);
            else if (id == P_NEXT) pick_rom(browser_list() ? ROWS : 1);
        } else if (id == P_RETURN) go_back();
    } else {
        held_buttons &= ~bit;
        gameplay_buttons &= ~bit;
    }
    physical_keys = 0;
    for (int i = 0; i < PHYSICAL_COUNT; ++i)
        if (gameplay_buttons & (1u << i)) physical_keys |= mapped_button(i);
}
static void log_core(enum retro_log_level level, const char *message, ...) {
    if (level < RETRO_LOG_INFO) return;
    va_list args;
    va_start(args, message);
    vfprintf(stderr, message, args);
    va_end(args);
}
static bool environment(unsigned command, void *data) {
    switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = data_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        if (!strcmp(v->key, "gpsp_drc")) v->value = use_dynarec ? "enabled" : "disabled";
        else if (!strcmp(v->key, "gpsp_rtc_time_source")) v->value = "system";
        else if (!strcmp(v->key, "gpsp_frameskip"))
            v->value = fast && fast_rate == 2 ? "fixed_interval" : "disabled";
        else if (!strcmp(v->key, "gpsp_frameskip_interval"))
            v->value = "1";
        else { v->value = NULL; return false; }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = ((smoke ? !smoke_mute : !music_mode && !fast && screen == GAME) ? 2 : 0) |
                       (allow_video ? 1 : 0);
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = core_options_changed;
        core_options_changed = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *(bool *)data = fast != 0; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_core;
        return true;
    default: return false;
    }
}
static void video_frame(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (screen != GAME || !data || !w || !h || w > CART_WIDTH || h > CART_HEIGHT ||
        pitch < w * sizeof(uint16_t) || (core != &gpsp && !allow_video)) return;
    cart_width = w;
    cart_height = h;
    ++video_frames;
    for (unsigned y = 0; y < h; ++y) {
        const uint16_t *line = (const uint16_t *)((const char *)data + y * pitch);
        uint32_t *dest = cart + y * w;
        for (unsigned x = 0; x < w; ++x) dest[x] = color[line[x]];
    }
    if (!have_frame) fprintf(stderr, "First cartridge video frame received\n");
    have_frame = true;
    dirty = 1;
}
static size_t audio_batch(const int16_t *data, size_t frames) {
    if (smoke) smoke_audio_frames += frames;
    else {
        sig_atomic_t previous = launch_stage;
        launch_stage = 6;
        platform_audio_write(data, frames, music_mode || fast || screen != GAME);
        launch_stage = previous;
    }
    return frames;
}
static void audio_sample(int16_t l, int16_t r) {
    int16_t pair[] = {l, r};
    audio_batch(pair, 1);
}
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port || device != RETRO_DEVICE_JOYPAD || index) return 0;
    unsigned pressed = physical_keys | touch_keys | pressed_this_frame;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)pressed;
    return id < 16 && (pressed & (1u << id)) != 0;
}
static void initialize_palette(void) {
    for (unsigned i = 0; i < 65536; ++i) {
        unsigned r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
        color[i] = RGB((r * 255 / 31 << 16) | (g * 255 / 63 << 8) | (b * 255 / 31));
    }
}
static void initialize_core(const struct Core *selected) {
    core = selected;
    allow_video = true;
    core->set_environment(environment);
    core->set_video_refresh(video_frame);
    core->set_audio_sample(audio_sample);
    core->set_audio_sample_batch(audio_batch);
    core->set_input_poll(input_poll);
    core->set_input_state(input_state);
    core->init();
    core->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
}
static bool prepare_dirs(void) {
    return join_path(config_file, data_dir, "controls.cfg") &&
           join_path(library_file, data_dir, "library.cfg") &&
           join_path(favorites_file, data_dir, "favorites.cfg") &&
           library_prepare(&data_paths, data_dir);
}
static int smoke_test(const char *path) {
    if (!load_rom(path)) return 1;
    smoke_audio_frames = 0;
    for (int i = 0; i < 15; ++i) core->run();
    if (!have_frame || !smoke_audio_frames ||
        (core == &gpsp && cart[80 * GBA_WIDTH + 120] != RGB(0xff0000)) ||
        (core == &shared_cores[GAMBATTE].api && (cart_width != 160 || cart_height != 144))) {
        close_rom();
        return 2;
    }
    if (!state_io(SAVE, 0)) { close_rom(); return 3; }
    for (int i = 0; i < 15; ++i) core->run();
    if (!state_io(SAVE, 1)) { close_rom(); return 4; }
    close_rom();
    if (!load_rom(path) || !state_io(LOAD, 0) || !state_io(LOAD, 1)) { close_rom(); return 5; }
    core->run();
    read_event((Event){'g', 'd', 103, 0});
    read_event((Event){'g', 'd', 163, 0});
    read_event((Event){'g', 'd', 116, 0});
    const Button *b = &buttons[B];
    read_touch('d', b->x + b->size / 2, b->y + b->size / 2);
    if (input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) !=
        (KEY(UP) | KEY(RIGHT) | KEY(A) | KEY(B))) { close_rom(); return 6; }
    read_event((Event){'g', 'u', 116, 0});
    read_touch('u', b->x + b->size / 2, b->y + b->size / 2);
    physical_keys = pressed_this_frame = held_buttons = gameplay_buttons = 0;
    int initial_game_volume = local_volume;
    read_wheel(200);
    if (local_volume != initial_game_volume) { close_rom(); return 18; }
    if (input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK)) {
        close_rom();
        return 7;
    }
    for (int i = SELECT_BUTTON; i <= START_BUTTON; ++i) {
        const Button *overlay = &buttons[i];
        int x = overlay->x + overlay->size / 2;
        int y = overlay->y + button_height(i, overlay) / 2;
        read_touch('d', x, y);
        if (input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) != (int16_t)overlay->keys) {
            close_rom();
            return 8;
        }
        read_touch('u', x, y);
        pressed_this_frame = 0;
    }
    enum Scale initial_scale = scaling;
    enum VideoFilter initial_filter = video_filter;
    for (int i = FIT; i < SCALE_COUNT; ++i) {
        scaling = i;
        video_filter = NEAREST;
        draw_ui();
        video_filter = SHARP_BILINEAR;
        draw_ui();
    }
    scaling = initial_scale;
    video_filter = initial_filter;
    const Button *menu = &buttons[MENU_BUTTON];
    int menu_x = menu->x + menu->size / 2, menu_y = menu->y - TOUCH_HIT_PADDING;
    Button saved_b = buttons[B];
    const int edge_x = 40, edge_y = HEIGHT / 2;
    move_button(B, EDGE_INSET, edge_y - saved_b.size / 2);
    read_touch('d', edge_x, edge_y);
    if (touch_keys || pressed_this_frame) { close_rom(); return 45; }
    read_touch('u', edge_x, edge_y);
    if (touch_keys || pressed_this_frame != KEY(B) || game_controls_hidden) { close_rom(); return 46; }
    pressed_this_frame = 0;
    read_touch('d', edge_x, edge_y);
    press_held_edge_button(game_edge.press_at - 1);
    if (touch_keys) { close_rom(); return 47; }
    press_held_edge_button(game_edge.press_at);
    if (touch_keys != KEY(B)) { close_rom(); return 50; }
    read_touch('u', edge_x, edge_y);
    pressed_this_frame = 0;
    read_touch('d', edge_x, edge_y);
    read_touch('m', edge_x + 12, edge_y);
    press_held_edge_button(game_edge.press_at);
    read_touch('m', edge_x + 38, edge_y + 30);
    read_touch('u', edge_x + 44, edge_y + 32);
    if (!game_controls_hidden || touch_keys || pressed_this_frame) { close_rom(); return 51; }
    read_touch('d', menu_x, menu_y);
    read_touch('u', menu_x, menu_y);
    read_touch('d', b->x + b->size / 2, b->y + b->size / 2);
    read_event((Event){'g', 'd', 116, 0});
    if (!game_controls_hidden || screen != GAME || touch_keys || physical_keys != KEY(A) ||
        input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) != KEY(A)) {
        close_rom(); return 48;
    }
    read_touch('u', b->x + b->size / 2, b->y + b->size / 2);
    read_event((Event){'g', 'u', 116, 0});
    pressed_this_frame = 0;
    buttons[B] = saved_b;
    read_touch('d', WIDTH - edge_x, edge_y);
    read_touch('u', WIDTH - edge_x - 38, edge_y - 30);
    if (game_controls_hidden) { close_rom(); return 49; }
    read_touch('d', edge_x, edge_y);
    read_touch('u', edge_x + 60, edge_y + 80);
    read_touch('d', WIDTH / 2, edge_y);
    read_touch('u', WIDTH / 2 + 60, edge_y);
    if (game_controls_hidden) { close_rom(); return 52; }
    int menu_right = menu->x + menu->size;
    int menu_center_y = menu->y + button_height(MENU_BUTTON, menu) / 2;
    buttons[B].x = menu_right + TOUCH_HIT_PADDING;
    buttons[B].y = menu_center_y - buttons[B].size / 2;
    if (hit(menu_right + 2, menu_center_y) != MENU_BUTTON ||
        hit(menu_right + 10, menu_center_y) != B) { close_rom(); return 34; }
    buttons[B].x = menu_right + 6;
    if (hit(menu_right + 7, menu_center_y) != B) { close_rom(); return 35; }
    buttons[B] = saved_b;
    int b_x = b->x - 6, b_y = b->y + b->size / 2;
    read_touch('d', b_x, b_y);
    if (touch_keys != KEY(B)) { close_rom(); return 36; }
    read_touch('u', b_x, b_y);
    pressed_this_frame = 0;
    read_touch('d', menu_x, menu_y - 1);
    if (screen != GAME) { close_rom(); return 32; }
    read_touch('u', menu_x, menu_y - 1);
    read_touch('d', menu_x, menu_y);
    if (screen != MENU) { close_rom(); return 33; }
    read_touch('u', menu_x, menu_y);
    set_screen(MENU);
    read_wheel(-21);
    read_event((Event){'g', 'd', 116, 0});
    read_event((Event){'g', 'u', 116, 0});
    if (screen != STATES) { close_rom(); return 9; }
    read_wheel(-42);
    read_event((Event){'g', 'd', 116, 0});
    read_event((Event){'g', 'u', 116, 0});
    if (screen != CONFIRM || confirm_action != SAVE) { close_rom(); return 10; }
    read_wheel(-21);
    read_event((Event){'g', 'd', 116, 0});
    if (screen != GAME || (physical_keys & KEY(A))) { close_rom(); return 13; }
    read_event((Event){'g', 'u', 116, 0});
    set_screen(STATES);
    read_touch('d', 240, 183);
    if (screen != CONFIRM || confirm_action != LOAD) { close_rom(); return 14; }
    read_touch('d', 240, 130);
    if (screen != GAME) { close_rom(); return 15; }
    set_screen(AUDIO);
    local_volume = 100;
    read_touch('d', 48, 150);
    if (local_volume != 95) { close_rom(); return 19; }
    load_config();
    if (local_volume != 95) { close_rom(); return 20; }
    set_screen(MENU);
    fast_rate = 2;
    read_touch('d', 238, 158);
    if (fast_rate != 3) { close_rom(); return 16; }
    fast_rate = 2;
    load_config();
    if (fast_rate != 3) { close_rom(); return 17; }
    set_screen(PHYSICAL);
    activate_menu(P_CENTRE);
    activate_menu(2); /* Start */
    activate_menu(P_NEXT);
    activate_menu(2); /* Duplicate bindings must release independently. */
    set_screen(TOUCH);
    touch_page = 0;
    activate_menu(0); /* Show A. */
    if (supports_overlay(active_system, L)) {
        activate_menu(7); /* Next page. */
        activate_menu(0); /* Show Up. */
    } else activate_menu(4); /* Show Up on the first page. */
    edit_button = TOUCH_A;
    set_screen(EDIT);
    activate_menu(6); /* Assign action. */
    activate_menu(3); /* Select */
    load_config();
    if (physical_map[P_CENTRE] != KEY(START) || !buttons[TOUCH_A].visible ||
        buttons[TOUCH_A].keys != KEY(SELECT) || !buttons[TOUCH_UP].visible) {
        close_rom();
        return 21;
    }
    set_screen(GAME);
    read_event((Event){'g', 'd', 116, 0});
    read_event((Event){'g', 'd', 163, 0});
    read_event((Event){'g', 'u', 116, 0});
    pressed_this_frame = 0;
    if (physical_keys != KEY(START)) { close_rom(); return 22; }
    read_event((Event){'g', 'u', 163, 0});
    if (physical_keys) { close_rom(); return 23; }
    set_screen(MENU);
    read_event((Event){'g', 'd', 116, 0}); /* Centre still confirms in a menu. */
    if (screen != GAME || physical_keys || pressed_this_frame) { close_rom(); return 24; }
    read_event((Event){'g', 'u', 116, 0});
    set_screen(SETTINGS);
    scaling = STRETCH;
    video_filter = SHARP_BILINEAR;
    read_touch('d', 100, 70);
    read_touch('d', 100, 113);
    if (scaling != INTEGER_SCALE || video_filter != NEAREST) { close_rom(); return 30; }
    scaling = FIT;
    video_filter = SHARP_BILINEAR;
    load_config();
    if (scaling != INTEGER_SCALE || video_filter != NEAREST) { close_rom(); return 31; }
    scaling = initial_scale;
    video_filter = initial_filter;
    activate_menu(6); /* Reset controls. */
    if (physical_map[P_CENTRE] != KEY(A) || buttons[TOUCH_A].visible ||
        !buttons[B].visible) { close_rom(); return 25; }
    for (int i = 0; i < CONTROLS; ++i)
        if (buttons[i].opacity != 70) { close_rom(); return 26; }
    set_screen(MENU);
    slot = 3;
    const struct Core *previous_core = core;
    read_touch('d', 84, 247);
    schedule_video(clock_ns());
    if (screen != BROWSER || emulation_active() || allow_video || core != previous_core ||
        slot != 3) { close_rom(); return 27; }
    const int toolbar_y = BROWSER_BUTTON_Y + 13;
    read_touch('d', EXIT_X + 25, toolbar_y);
    read_touch('u', EXIT_X + 25, toolbar_y);
    if (screen != BROWSER || browser_actions_open) { close_rom(); return 40; }
    read_touch('d', MORE_TOUCH_X + 3, 5);
    read_touch('u', MORE_TOUCH_X - 17, 25);
    if (!browser_actions_open) { close_rom(); return 41; }
    read_touch('d', WIDTH / 2, HEIGHT / 2);
    read_touch('u', WIDTH / 2, HEIGHT / 2);
    if (screen != BROWSER || browser_actions_open) { close_rom(); return 42; }
    RomShelf *previous_shelf_selection = shelf;
    shelf = &shelves[RECENT];
    RomShelf saved_recent = *shelf;
    *shelf = (RomShelf){.end = 2};
    wheel_delta = 0;
    read_touch('d', MORE_DOTS_X, toolbar_y);
    read_touch('u', MORE_DOTS_X, toolbar_y);
    read_event((Event){'w', 0, -1, 0});
    if (browser_actions_open || shelf->selected != 0) { close_rom(); return 53; }
    read_event((Event){'w', 0, -37, 0});
    if (shelf->selected != 1) { close_rom(); return 54; }
    read_touch('d', MORE_DOTS_X, toolbar_y);
    read_touch('u', MORE_DOTS_X, toolbar_y);
    read_event((Event){'w', 0, 38, 0});
    if (browser_actions_open || shelf->selected != 0) { close_rom(); return 55; }
    read_touch('d', MORE_DOTS_X, toolbar_y);
    read_touch('u', MORE_DOTS_X, toolbar_y);
    read_event((Event){'g', 'd', 163, 0});
    read_event((Event){'g', 'u', 163, 0});
    if (browser_actions_open || shelf->selected != 1) { close_rom(); return 56; }
    *shelf = saved_recent;
    read_touch('d', MORE_DOTS_X, toolbar_y);
    read_touch('u', MORE_DOTS_X, toolbar_y);
    read_event((Event){'g', 'd', 108, 0});
    read_event((Event){'g', 'u', 108, 0});
    if (screen != BROWSER || browser_actions_open || shelf == &shelves[RECENT]) { close_rom(); return 57; }
    shelf = previous_shelf_selection;
    set_screen(BROWSER);
    read_touch('d', MORE_DOTS_X, toolbar_y);
    read_touch('u', MORE_DOTS_X, toolbar_y);
    read_touch('d', SORT_X + 25, toolbar_y);
    read_touch('u', SORT_X + 25, toolbar_y);
    if (screen != BROWSER_OPTIONS || browser_actions_open) { close_rom(); return 43; }
    go_back();
    read_touch('d', MORE_DOTS_X, toolbar_y);
    read_touch('u', MORE_DOTS_X, toolbar_y);
    read_touch('d', APP_MENU_X + 25, toolbar_y);
    read_touch('u', APP_MENU_X + 25, toolbar_y);
    if (screen != APP_MENU || browser_actions_open) { close_rom(); return 44; }
    go_back();
    read_touch('d', MORE_DOTS_X, toolbar_y);
    read_touch('u', MORE_DOTS_X, toolbar_y);
    read_touch('d', EXIT_X + 25, BROWSER_BUTTON_Y + 13);
    read_touch('u', EXIT_X + 25, BROWSER_BUTTON_Y + 13);
    if (quitting || screen != GAME || !running || core != previous_core ||
        slot != 3) { close_rom(); return 29; }
    set_screen(GAME);
    fast = 1;
    for (unsigned i = 0; i < sizeof speeds / sizeof speeds[0]; ++i) {
        fast_rate = speeds[i];
        core_options_changed = true;
        next_video = 0;
        schedule_video(1000000000ull);
        core->run();
        video_frames = 0;
        unsigned expected = 0;
        uint64_t period = 1000000000ull / (60 * fast_rate);
        for (unsigned frame = 1; frame <= 30; ++frame) {
            schedule_video(1000000000ull + frame * period);
            if (allow_video) ++expected;
            core->run();
        }
        if (fast_rate == 2 && core == &gpsp) expected = 15;
        if (video_frames != expected ||
            (fast_rate >= 3 && (expected < 2 || expected > 6))) { close_rom(); return 11; }
    }
    fast = 0;
    core_options_changed = true;
    schedule_video(2000000000ull);
    core->run();
    video_frames = 0;
    for (int frame = 0; frame < 6; ++frame) core->run();
    if (video_frames != 6) { close_rom(); return 12; }
    smoke_audio_frames = 0;
    smoke_mute = true;
    for (int frame = 0; frame < 8; ++frame) core->run();
    smoke_mute = false;
    if (smoke_audio_frames) { close_rom(); return 32; }
    for (int frame = 0; frame < 8; ++frame) core->run();
    if (!smoke_audio_frames) { close_rom(); return 33; }
    close_rom();
    puts("PASS: ROM, save slots, controls and overlay toggle, library toolbar, pause and resume, speed selection, video budget, audio mute and resume");
    return 0;
}
static void write_fatal(int sig) {
    const char *names[] = {"menu/initialization", "ROM load", "save RAM load",
                           "first emulated frame", "gameplay", "save/exit", "ALSA output"};
    char line[120] = "Fatal signal ";
    unsigned n = sizeof "Fatal signal " - 1;
    line[n++] = '0' + (sig / 10) % 10;
    line[n++] = '0' + sig % 10;
    line[n++] = ' ';
    line[n++] = 'i';
    line[n++] = 'n';
    line[n++] = ' ';
    unsigned stage = launch_stage;
    if (stage >= sizeof names / sizeof names[0]) stage = 0;
    for (const char *c = names[stage]; *c; ++c) line[n++] = *c;
    line[n++] = '\n';
    (void)write(STDERR_FILENO, line, n);
    _exit(128 + sig);
}
static void log_launch(void) {
    char log[PATH_CAP], previous[PATH_CAP], flag[PATH_CAP];
    if (file_path(log, sizeof log, data_dir, "/q2-boy.log")) {
        struct stat st;
        if (!stat(log, &st) && st.st_size > 128 * 1024 &&
            file_path(previous, sizeof previous, log, ".previous"))
            (void)rename(log, previous);
        FILE *stream = freopen(log, "a", stderr);
        if (stream) setvbuf(stream, NULL, _IOLBF, 0);
    }
    if (file_path(flag, sizeof flag, data_dir, "/enable-dynarec.txt")) {
        FILE *f = fopen(flag, "r");
        if (f) { use_dynarec = fgetc(f) == '1'; fclose(f); }
    }
    if (file_path(flag, sizeof flag, data_dir, "/performance-log.txt")) {
        FILE *f = fopen(flag, "r");
        if (f) { performance_log = fgetc(f) == '1'; fclose(f); }
    }
    platform_set_diagnostics(performance_log);
    struct sigaction action = {.sa_handler = write_fatal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    fprintf(stderr, "\nq2-boy 0.3.8, engine=%s\n",
            use_dynarec ? "dynamic recompiler (opt-in)" : "interpreter");
    FILE *memory = fopen("/proc/meminfo", "r");
    if (memory) {
        char line[128];
        for (int i = 0; i < 4 && fgets(line, sizeof line, memory); ++i) fputs(line, stderr);
        fclose(memory);
    }
}
int main(int argc, char **argv) {
    smoke = argc >= 4 && (!strcmp(argv[1], "--smoke") || !strcmp(argv[1], "--smoke-dynarec"));
    use_dynarec = smoke && !strcmp(argv[1], "--smoke-dynarec");
    if (smoke) data_dir = argv[3];
    else {
        /* Move existing data only when the new data directory does not exist. */
        struct stat legacy, current;
        const char *legacy_dir = "/mnt/mmc/GBA";
        if (stat(data_dir, &current) && errno == ENOENT &&
            !stat(legacy_dir, &legacy) && S_ISDIR(legacy.st_mode) &&
            rename(legacy_dir, data_dir)) {
            fprintf(stderr, "Could not move existing cartridge data: %s\n", strerror(errno));
            return 1;
        }
    }
    if (!prepare_dirs()) return 1;
    locate_app(argv[0]);
    if (!smoke) {
        log_launch();
        const char *bridge = getenv("Q2_BOY_AUDIO_BRIDGE");
        char caps[512] = "";
        if (!read_text(ADDON_CAPS, caps, sizeof caps)) caps[0] = 0;
        bool supports_audio2 = !bridge && has_cap(caps, "audio2");
        bool was_audio2 = false;
        bool manifest_ready = !bridge && enable_manifest_audio(supports_audio2, &was_audio2);
        audio2 = supports_audio2 && argc == 1;
        firmware_keys = !bridge && has_cap(caps, "gameview");
        audio_bridge = bridge != NULL || audio2;
        native_supported = (bridge && *bridge == '2') || (audio2 && firmware_keys);
        const char *display = getenv("Q2_BOY_DISPLAY_BRIDGE");
        display_bridge = display && !strcmp(display, "1");
        music_mode = argc == 1 && (audio_bridge || was_audio2);
        audio_route = argc > 1 && !strcmp(argv[1], "plug:bluealsa") ? 1 :
                      argc > 1 && !strcmp(argv[1], "usb") ? (argc > 2 && argv[2][0] == 'h' ? 3 : 2) : 0;
        if (!platform_open(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL, music_mode, firmware_keys)) {
            fprintf(stderr, "Framebuffer or input socket initialization failed: %s\n", strerror(errno));
            platform_close();
            return 1;
        }
        if (supports_audio2 && argc > 1)
            status(manifest_ready ? "Exit Pocket Boy, back out of Games, then reopen it" : "Could not enable Q2 music on the card");
        else if (!supports_audio2 && was_audio2)
            status(manifest_ready ? "Exit Pocket Boy, back out of Games, then reopen it" : "Could not restore game audio on the card");
    }
    load_config();
    initialize_palette();
    if (smoke) {
        int result = smoke_test(argv[2]);
        for (int i = 4; !result && i < argc; ++i) result = smoke_test(argv[i]);
        release_core();
        close_shared_cores();
        return result;
    }
    if (firmware_keys && !write_quit_option(quit_off)) status("Could not set force-close shortcut");
    if (audio2) {
        char path[PATH_CAP], mode[24];
        prefer_music = file_path(path, sizeof path, data_dir, "/audio-source.cfg") &&
                       read_text(path, mode, sizeof mode) && has_cap(mode, "music");
        if (!prefer_music) (void)switch_audio2(AUDIO_GAME, false);
    }
    if (!raw_buttons && !native_supported && !firmware_keys && !notice_end)
        status("Limited input: holding Up may exit");
    load_library();
    load_favorites();
    library_migrate(&data_paths);
    cover_cache_open(data_dir);
    scan_roms();
    last_activity = clock_ns();
    uint64_t next_frame = 0;
    while (!quitting) {
        Event e;
        for (int i = 0; i < 128 && platform_event(&e); ++i) if (!native_yielded) read_event(e);
        uint64_t now = clock_ns();
        audio_poll(now);
        now = clock_ns();
        if (screen == GAME) press_held_edge_button(now);
        if (notice_end && now >= notice_end) { notice_end = 0; dirty = 1; }
        bool advance = emulation_active();
        if (advance && now >= next_frame) {
            uint64_t period = (uint64_t)(1000000000.0 / (fps * (fast ? fast_rate : 1)));
            if (performance_log && next_frame && now - next_frame >= period) ++late_frames;
            if (!next_frame || now - next_frame > period * 3) next_frame = now;
            next_frame += period;
            launch_stage = have_frame ? 4 : 3;
            if (!speed_start) { speed_start = now; speed_frames = video_frames = 0; }
            if ((fast || performance_log) && now - speed_start >= (fast ? 2000000000ull : 5000000000ull)) {
                double measured = speed_frames * 1000000000.0 / (now - speed_start);
                if (fast) { measured_speed = measured / fps; dirty = 1; }
                double pictures = video_frames * 1000000000.0 / (now - speed_start);
                if (performance_log) {
                    PlatformStats stats = platform_take_stats();
                    fprintf(stderr, "%dx: %.1f emulated fps, %.1f video fps, %u flips, %u late, "
                            "%u overwritten frames, max us: core %u draw %u flip %u, "
                            "%u underruns, %u discarded audio frames\n",
                            fast ? fast_rate : 1, measured, pictures, stats.flips, late_frames,
                            stats.dropped_frames, longest_run_us, longest_draw_us, stats.longest_flip_us,
                            stats.underruns, stats.dropped_samples);
                    late_frames = longest_run_us = longest_draw_us = 0;
                }
                speed_start = now;
                speed_frames = video_frames = 0;
            }
            schedule_video(now);
            uint64_t started = performance_log ? clock_ns() : 0;
            core->run();
            if (fast || performance_log) ++speed_frames;
            if (performance_log) {
                unsigned elapsed = (clock_ns() - started) / 1000;
                if (elapsed > longest_run_us) longest_run_us = elapsed;
            }
            pressed_this_frame = 0;
        } else if (!advance) next_frame = 0;
        if (running && now >= next_save) {
            cartridge_saves(true);
            next_save = now + 20000000000ull;
        }
        platform_audio_service();
        uint64_t browse_at = update_browser(now);
        if (dirty && !native_yielded) {
            uint64_t started = performance_log ? clock_ns() : 0;
            draw_ui();
            if (performance_log) {
                unsigned elapsed = (clock_ns() - started) / 1000;
                if (elapsed > longest_draw_us) longest_draw_us = elapsed;
            }
        }
        uint64_t deadline = advance ? next_frame : UINT64_MAX;
        deadline = sooner(deadline, sooner(browse_at, audio_deadline()));
        if (notice_end) deadline = sooner(deadline, notice_end);
        if (running) deadline = sooner(deadline, next_save);
        if (platform_audio_pending()) deadline = sooner(deadline, now + 3000000ull);
        deadline = sooner(deadline, display_deadline(now));
        platform_wait(deadline);
    }
    close_rom();
    close_shared_cores();
    platform_close();
    fprintf(stderr, "Exited cleanly\n");
    return 0;
}
