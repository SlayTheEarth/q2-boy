// SPDX-License-Identifier: GPL-2.0-or-later
#include "cover.h"
#include "platform.h"

#include <dlfcn.h>
#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jpeg7/jpeglib.h"

/* Q2 firmware ships libpng 1.2. Use only its public opaque-pointer ABI so the
 * frontend can still show cartridge icons if that library is unavailable. */
typedef struct png_struct_def Png;
typedef struct png_info_struct PngInfo;
typedef void (*PngMessage)(Png *, const char *);
#define PNG_API(X) \
    X(Png *, create_read_struct_2, (const char *, void *, PngMessage, PngMessage, void *, \
                                  void *(*)(Png *, uint32_t), void (*)(Png *, void *))) \
    X(PngInfo *, create_info_struct, (Png *)) \
    X(void, destroy_read_struct, (Png **, PngInfo **, PngInfo **)) \
    X(void *, get_error_ptr, (Png *)) \
    X(void *, get_mem_ptr, (Png *)) \
    X(void, init_io, (Png *, FILE *)) \
    X(void, set_user_limits, (Png *, uint32_t, uint32_t)) \
    X(void, read_info, (Png *, PngInfo *)) \
    X(uint32_t, get_IHDR, (Png *, PngInfo *, uint32_t *, uint32_t *, int *, int *, int *, int *, int *)) \
    X(void, set_strip_16, (Png *)) \
    X(void, set_expand, (Png *)) \
    X(void, set_gray_to_rgb, (Png *)) \
    X(void, set_filler, (Png *, uint32_t, int)) \
    X(int, set_interlace_handling, (Png *)) \
    X(void, read_update_info, (Png *, PngInfo *)) \
    X(uint32_t, get_rowbytes, (Png *, PngInfo *)) \
    X(void, read_image, (Png *, unsigned char **)) \
    X(void, read_end, (Png *, PngInfo *))

static struct {
#define DECLARE(result, name, args) result (*name) args;
    PNG_API(DECLARE)
#undef DECLARE
} png;

static bool png_available(void) {
    static bool attempted, ready;
    if (attempted) return ready;
    attempted = true;
    void *lib = dlopen("libpng12.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
#define LOAD(result, name, args) \
    png.name = (result (*) args)dlsym(lib, "png_" #name); \
    if (!png.name) { dlclose(lib); return false; }
    PNG_API(LOAD)
#undef LOAD
    return ready = true;
}

#define PNG_SOURCE_LIMIT 1024
#define JPEG_SOURCE_LIMIT 2048
#define COVER_FILE_LIMIT (8 * 1024 * 1024)
#define CACHE_COUNT 14 /* Seven covers can be visible on each shelf during a system change. */
#define DISK_CACHE_COUNT 64
#define THUMBNAIL_FORMAT 0x51435431u
#define PNG_MEMORY_LIMIT (4u * 1024u * 1024u)
#define REFLECTION 46
#define COVER_TOP 76
#define COVER_HORIZON 64
#define CAMERA_DEPTH 256.0f
typedef struct {
    char name[256];
    unsigned width, height, age;
    uint32_t pixels[COVER_SIZE * COVER_SIZE];
} Cover;

typedef struct {
    jmp_buf error;
    Png *decoder;
    PngInfo *info;
    size_t allocated;
    unsigned char *pixels;
    unsigned char *rows[PNG_SOURCE_LIMIT];
} Decode;

static Cover covers[CACHE_COUNT];
static unsigned age;
static char cache_dir[768];

typedef struct {
    uint64_t key;
    uint32_t format, width, height;
} ThumbnailHeader;

void cover_cache_open(const char *data_dir) {
    int length = snprintf(cache_dir, sizeof cache_dir, "%s/.cover-cache", data_dir);
    struct stat st;
    if (length < 0 || (size_t)length >= sizeof cache_dir ||
        (mkdir(cache_dir, 0755) && (stat(cache_dir, &st) || !S_ISDIR(st.st_mode))))
        cache_dir[0] = 0;
}

void cover_cache_clear(void) {
    if (!cache_dir[0]) return;
    char path[800];
    for (unsigned i = 0; i < DISK_CACHE_COUNT; ++i) {
        snprintf(path, sizeof path, "%s/%02x", cache_dir, i);
        unlink(path);
    }
    snprintf(path, sizeof path, "%s/pending", cache_dir);
    unlink(path);
}

static uint64_t thumbnail_key(const char *path, const struct stat64 *st) {
    uint64_t hash = 14695981039346656037ull;
    for (; *path; ++path) {
        hash ^= (unsigned char)*path;
        hash *= 1099511628211ull;
    }
    const uint64_t identity[] = {st->st_size, st->st_mtime, st->st_mtim.tv_nsec,
                                 st->st_ctime, st->st_ctim.tv_nsec};
    for (unsigned i = 0; i < sizeof identity / sizeof identity[0]; ++i) {
        hash ^= identity[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool load_thumbnail(const char *path, uint64_t key, Cover *cover) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    ThumbnailHeader header;
    bool valid = fread(&header, sizeof header, 1, file) == 1 &&
                 header.format == THUMBNAIL_FORMAT && header.key == key &&
                 header.width && header.height &&
                 header.width <= COVER_SIZE && header.height <= COVER_SIZE;
    if (valid) {
        size_t count = header.width * header.height;
        valid = fread(cover->pixels, sizeof cover->pixels[0], count, file) == count;
        if (valid) {
            cover->width = header.width;
            cover->height = header.height;
        }
    }
    fclose(file);
    return valid;
}

static void store_thumbnail(const char *path, uint64_t key, const Cover *cover) {
    char temporary[800];
    snprintf(temporary, sizeof temporary, "%s/pending", cache_dir);
    FILE *file = fopen(temporary, "wb");
    if (!file) return;
    ThumbnailHeader header = {key, THUMBNAIL_FORMAT, cover->width, cover->height};
    size_t count = cover->width * cover->height;
    bool written = fwrite(&header, sizeof header, 1, file) == 1 &&
                   fwrite(cover->pixels, sizeof cover->pixels[0], count, file) == count;
    if (fclose(file) || !written || rename(temporary, path)) unlink(temporary);
}

static void png_fail(Png *decoder, const char *message) {
    (void)message;
    Decode *decode = png.get_error_ptr(decoder);
    longjmp(decode->error, 1);
}

static void png_warn(Png *decoder, const char *message) {
    (void)decoder;
    (void)message;
}

typedef union {
    size_t size;
    max_align_t alignment;
} Allocation;

static void *png_allocate(Png *decoder, uint32_t bytes) {
    Decode *decode = png.get_mem_ptr(decoder);
    if (bytes > PNG_MEMORY_LIMIT - decode->allocated) return NULL;
    Allocation *block = malloc(sizeof *block + bytes);
    if (!block) return NULL;
    block->size = bytes;
    decode->allocated += bytes;
    return block + 1;
}

static void png_free(Png *decoder, void *pointer) {
    if (!pointer) return;
    Decode *decode = png.get_mem_ptr(decoder);
    Allocation *block = (Allocation *)pointer - 1;
    decode->allocated -= block->size;
    free(block);
}

static void size_cover(Cover *cover, unsigned w, unsigned h) {
    cover->width = w >= h ? COVER_SIZE : (w * COVER_SIZE + h - 1) / h;
    cover->height = h >= w ? COVER_SIZE : (h * COVER_SIZE + w - 1) / w;
}

static bool png_pixels(FILE *file, Decode *decode, Cover *cover) {
    if (setjmp(decode->error)) return false;
    Png *decoder = decode->decoder =
        png.create_read_struct_2("1.2.0", decode, png_fail, png_warn, decode, png_allocate, png_free);
    if (!decoder) return false;
    PngInfo *info = decode->info = png.create_info_struct(decoder);
    if (!info) return false;
    png.init_io(decoder, file);
    png.set_user_limits(decoder, PNG_SOURCE_LIMIT, PNG_SOURCE_LIMIT);
    png.read_info(decoder, info);
    uint32_t w, h;
    int depth, type, interlace, compression, filter;
    if (!png.get_IHDR(decoder, info, &w, &h, &depth, &type, &interlace, &compression, &filter) ||
        !w || !h || w > PNG_SOURCE_LIMIT || h > PNG_SOURCE_LIMIT) return false;
    if (depth == 16) png.set_strip_16(decoder);
    png.set_expand(decoder);
    png.set_gray_to_rgb(decoder);
    png.set_filler(decoder, 255, 1); /* PNG_FILLER_AFTER */
    png.set_interlace_handling(decoder);
    png.read_update_info(decoder, info);
    if (png.get_rowbytes(decoder, info) != w * 4) return false;
    decode->pixels = calloc(h, w * 4);
    if (!decode->pixels) return false;
    for (unsigned y = 0; y < h; ++y) decode->rows[y] = decode->pixels + y * w * 4;
    png.read_image(decoder, decode->rows);
    png.read_end(decoder, NULL);

    size_cover(cover, w, h);
    for (unsigned y = 0; y < cover->height; ++y) {
        const unsigned char *row = decode->rows[(2 * y + 1) * h / (2 * cover->height)];
        for (unsigned x = 0; x < cover->width; ++x) {
            const unsigned char *p = row + 4 * ((2 * x + 1) * w / (2 * cover->width));
            cover->pixels[y * cover->width + x] = (uint32_t)p[3] << 24 |
                                                (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    }
    return true;
}

static bool read_png(FILE *file, Cover *cover) {
    if (!png_available()) return false;
    Decode *decode = calloc(1, sizeof *decode);
    bool ok = decode && png_pixels(file, decode, cover);
    if (decode) {
        if (decode->decoder) png.destroy_read_struct(&decode->decoder, &decode->info, NULL);
        free(decode->pixels);
        free(decode);
    }
    return ok;
}

#define JPEG_API(X) \
    X(std_error) \
    X(CreateDecompress) \
    X(stdio_src) \
    X(read_header) \
    X(start_decompress) \
    X(read_scanlines) \
    X(finish_decompress) \
    X(destroy_decompress)

static struct {
#define DECLARE(name) __typeof__(&jpeg_##name) name;
    JPEG_API(DECLARE)
#undef DECLARE
} jpeg;

static bool jpeg_available(void) {
    static bool attempted, ready;
    if (attempted) return ready;
    attempted = true;
    void *lib = dlopen("libjpeg.so.7", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
#define LOAD(name) \
    jpeg.name = (__typeof__(jpeg.name))dlsym(lib, "jpeg_" #name); \
    if (!jpeg.name) { dlclose(lib); return false; }
    JPEG_API(LOAD)
#undef LOAD
    return ready = true;
}

typedef struct {
    struct jpeg_decompress_struct decoder;
    struct jpeg_error_mgr error;
    jmp_buf jump;
} JpegDecode;

static void jpeg_fail(j_common_ptr decoder) {
    JpegDecode *decode = (JpegDecode *)decoder;
    longjmp(decode->jump, 1);
}

static void jpeg_quiet(j_common_ptr decoder) {
    (void)decoder;
}

static uint32_t jpeg_color(const JSAMPLE *p, bool cmyk, bool adobe) {
    if (!cmyk) return 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    /* Adobe CMYK stores inverted components. */
    unsigned k = adobe ? p[3] : 255 - p[3];
    unsigned r = (adobe ? p[0] : 255 - p[0]) * k / 255;
    unsigned g = (adobe ? p[1] : 255 - p[1]) * k / 255;
    unsigned b = (adobe ? p[2] : 255 - p[2]) * k / 255;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static bool jpeg_pixels(FILE *file, JpegDecode *decode, Cover *cover) {
    if (setjmp(decode->jump)) return false;
    j_decompress_ptr decoder = &decode->decoder;
    decoder->err = jpeg.std_error(&decode->error);
    decode->error.error_exit = jpeg_fail;
    decode->error.output_message = jpeg_quiet;
    jpeg.CreateDecompress(decoder, JPEG_LIB_VERSION, sizeof *decoder);
    decoder->mem->max_memory_to_use = 16 * 1024 * 1024;
    jpeg.stdio_src(decoder, file);
    if (jpeg.read_header(decoder, TRUE) != JPEG_HEADER_OK) return false;
    unsigned w = decoder->image_width, h = decoder->image_height;
    if (!w || !h || w > JPEG_SOURCE_LIMIT || h > JPEG_SOURCE_LIMIT) return false;

    size_cover(cover, w, h);
    unsigned largest = w > h ? w : h;
    decoder->scale_num = 1;
    decoder->scale_denom = largest >= 8 * COVER_SIZE ? 8 :
                           largest >= 4 * COVER_SIZE ? 4 : largest >= 2 * COVER_SIZE ? 2 : 1;
    bool cmyk = decoder->jpeg_color_space == JCS_CMYK || decoder->jpeg_color_space == JCS_YCCK;
    decoder->out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    if (!jpeg.start_decompress(decoder)) return false;

    unsigned width = decoder->output_width, height = decoder->output_height;
    unsigned components = cmyk ? 4 : 3;
    if (!width || width > JPEG_SOURCE_LIMIT || !height || height > JPEG_SOURCE_LIMIT ||
        decoder->output_components != (int)components) return false;
    JSAMPROW row = (*decoder->mem->alloc_sarray)((j_common_ptr)decoder, JPOOL_IMAGE, width * components, 1)[0];
    unsigned y = 0;
    while (decoder->output_scanline < height) {
        unsigned source_y = decoder->output_scanline;
        if (jpeg.read_scanlines(decoder, &row, 1) != 1) return false;
        while (y < cover->height && (2 * y + 1) * height / (2 * cover->height) == source_y) {
            for (unsigned x = 0; x < cover->width; ++x) {
                unsigned sample = (2 * x + 1) * width / (2 * cover->width);
                cover->pixels[y * cover->width + x] =
                    jpeg_color(row + sample * components, cmyk, decoder->saw_Adobe_marker);
            }
            ++y;
        }
    }
    return jpeg.finish_decompress(decoder) && !decode->error.num_warnings;
}

static bool read_jpeg(FILE *file, Cover *cover) {
    if (!jpeg_available()) return false;
    JpegDecode *decode = calloc(1, sizeof *decode);
    if (!decode) return false;
    bool ok = jpeg_pixels(file, decode, cover);
    if (decode->decoder.mem) jpeg.destroy_decompress(&decode->decoder);
    free(decode);
    return ok;
}

static bool read_cover(const char *path, Cover *cover, bool is_png) {
    struct stat64 st;
    if (stat64(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 8 ||
        st.st_size > COVER_FILE_LIMIT) return false;
    char thumb[800];
    uint64_t key = 0;
    if (cache_dir[0]) {
        key = thumbnail_key(path, &st);
        snprintf(thumb, sizeof thumb, "%s/%02x", cache_dir, (unsigned)(key % DISK_CACHE_COUNT));
        if (load_thumbnail(thumb, key, cover)) return true;
    }
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = is_png ? read_png(file, cover) : read_jpeg(file, cover);
    fclose(file);
    if (ok && cache_dir[0]) store_thumbnail(thumb, key, cover);
    return ok;
}

static void cartridge_icon(Cover *cover, enum CartridgeSystem system) {
    const uint32_t shells[] = {0xffb2b4a6, 0xff886bb0, 0xff626f8e, 0xffa6a7a8, 0xffa4a3ad};
    const uint32_t labels[] = {0xff737d60, 0xffb65981, 0xff387d97, 0xffae5443, 0xff665099};
    bool wide = system == SYSTEM_GBA || system == SYSTEM_SNES;
    cover->width = wide ? 150 : 116;
    cover->height = wide ? 104 : 136;
    unsigned w = cover->width, h = cover->height;
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            uint32_t color = shells[system];
            if ((y < 7 && (x < 7 - y || x >= w - (7 - y))) ||
                (y >= h - 8 && (x < y - h + 8 || x >= w - (y - h + 8)))) color = 0;
            else if (x < 3 || x >= w - 3 || y >= h - 4) color = 0xff42454e;
            else if (x >= 13 && x < w - 13 && y >= 24 && y < h - 21) {
                color = labels[system];
                if ((x >= 24 && x < w - 24 && y >= 37 && y < 43) ||
                    (x >= 24 && x < w - 43 && y >= 49 && y < 53)) color = 0xffd8dbd7;
            } else if (y >= h - 14 && y < h - 10 && x >= 14 && x < w - 14) color = 0xff42454e;
            cover->pixels[y * w + x] = color;
        }
    }
}

void cover_clear(void) {
    for (unsigned i = 0; i < CACHE_COUNT; ++i) covers[i].name[0] = 0;
    age = 0;
}

static bool artwork_in(const char *directory, const char *name, Cover *cover) {
    static const char *const extensions[] = {".png", ".PNG", ".jpg", ".JPG", ".jpeg", ".JPEG"};
    char path[1024];
    int stem = (int)(strrchr(name, '.') - name);
    for (unsigned i = 0; i < sizeof extensions / sizeof extensions[0]; ++i) {
        int length = snprintf(path, sizeof path, "%s/%.*s%s", directory, stem, name, extensions[i]);
        if (length > 0 && (size_t)length < sizeof path && read_cover(path, cover, i < 2)) return true;
    }
    return false;
}

static const Cover *find_cover(const char *directory, const char *fallback, const char *name,
                               enum CartridgeSystem system) {
    Cover *oldest = covers;
    for (unsigned i = 0; i < CACHE_COUNT; ++i) {
        Cover *cover = &covers[i];
        if (!strcmp(cover->name, name)) {
            cover->age = ++age;
            return cover;
        }
        if (!cover->name[0] || (oldest->name[0] && cover->age < oldest->age)) oldest = cover;
    }
    snprintf(oldest->name, sizeof oldest->name, "%s", name);
    oldest->age = ++age;

    if (!artwork_in(directory, name, oldest) && (!fallback || !artwork_in(fallback, name, oldest)))
        cartridge_icon(oldest, system);
    return oldest;
}

void cover_prefetch(const char *directory, const char *fallback, const char *name,
                    enum CartridgeSystem system) {
    (void)find_cover(directory, fallback, name, system);
}

static uint32_t composite(uint32_t color, uint32_t background, unsigned alpha) {
    unsigned rb = ((color & 0xff00ff) * alpha + (background & 0xff00ff) * (256 - alpha)) >> 8;
    unsigned g = ((color & 0x00ff00) * alpha + (background & 0x00ff00) * (256 - alpha)) >> 8;
    return 0xff000000 | (rb & 0xff00ff) | (g & 0xff00);
}

/* Q2-Pod Minimal's Cover Flow uses a 90-degree turn, a 128 px first
 * offset, 35 px side spacing, 77 px of depth and a shared 64% horizon.
 * Project the actual image rectangle so non-square game covers stay intact. */
typedef struct {
    float x, depth, cosine, sine;
    unsigned brightness;
    bool mirrored;
} CoverPose;

static CoverPose cover_pose(float distance) {
    float t = fabsf(distance);
    float turn = t < 1.0f ? t : 1.0f;
    float angle = turn * 1.57079632679f;
    float brightness = t <= 2.0f ? 256.0f - 64.0f * t : 128.0f;
    if (t > 3.0f) brightness *= (COVER_REACH - t) * 2.0f;
    return (CoverPose){
        128.0f * turn + 35.0f * (t - turn),
        CAMERA_DEPTH + 77.0f * turn,
        turn == 1.0f ? 0.0f : cosf(angle),
        turn == 1.0f ? 1.0f : sinf(angle),
        (unsigned)(brightness + 0.5f),
        distance < 0
    };
}

static bool cover_column(const Cover *cover, CoverPose pose, int x,
                         unsigned *source, int *top, int *height) {
    const float center = WIDTH / 2.0f;
    float dx = x + 0.5f - center;
    if (pose.mirrored) dx = -dx;
    float denominator = CAMERA_DEPTH * pose.cosine + dx * pose.sine;
    if (denominator <= 0.0f) return false;
    float u = (dx * pose.depth - CAMERA_DEPTH * pose.x) / denominator;
    float width = cover->width;
    float sample = width * 0.5f + (pose.mirrored ? -u : u);
    if (sample < 0.0f || sample >= width) return false;
    *source = (unsigned)sample;
    *height = (int)(cover->height * CAMERA_DEPTH / (pose.depth - u * pose.sine) + 0.5f);
    int horizon = COVER_TOP + COVER_SIZE * COVER_HORIZON / 100;
    *top = horizon - *height * COVER_HORIZON / 100;
    return *height > 0;
}

bool cover_hit(const char *directory, const char *fallback, const char *name,
               enum CartridgeSystem system,
               float distance, int x, int y) {
    if (fabsf(distance) >= COVER_REACH) return false;
    const Cover *cover = find_cover(directory, fallback, name, system);
    unsigned source;
    int top, height;
    return cover_column(cover, cover_pose(distance), x, &source, &top, &height) &&
           y >= top && y < top + height;
}

static void cover_span(const Cover *cover, CoverPose pose, int *start, int *end) {
    float half = cover->width * 0.5f;
    float left = CAMERA_DEPTH * (pose.x - half * pose.cosine) / (pose.depth + half * pose.sine);
    float right = CAMERA_DEPTH * (pose.x + half * pose.cosine) / (pose.depth - half * pose.sine);
    float center = WIDTH / 2.0f;
    *start = (int)(center + (pose.mirrored ? -right : left));
    *end = (int)(center + (pose.mirrored ? -left : right) + 0.5f);
    if (*start < 0) *start = 0;
    if (*end > WIDTH) *end = WIDTH;
}

bool cover_badge_position(const char *directory, const char *fallback, const char *name,
                          enum CartridgeSystem system, float distance, int size, int *x, int *y) {
    if (fabsf(distance) >= COVER_REACH) return false;
    const Cover *cover = find_cover(directory, fallback, name, system);
    CoverPose pose = cover_pose(distance);
    int start, end, height;
    unsigned source;
    cover_span(cover, pose, &start, &end);
    if (end <= start) return false;
    int anchor = end - (end - start > size + 6 ? 4 : 0);
    int sample = anchor - 1;
    while (sample >= start && !cover_column(cover, pose, sample, &source, y, &height)) --sample;
    if (sample < start) return false;
    *x = anchor - size;
    if (*x < 0) *x = 0;
    *y += 4;
    return true;
}

void cover_draw(uint32_t *canvas, const char *directory, const char *fallback, const char *name,
                enum CartridgeSystem system, float distance, int offset_y) {
    if (fabsf(distance) >= COVER_REACH) return;
    const Cover *cover = find_cover(directory, fallback, name, system);
    CoverPose pose = cover_pose(distance);
    int start, end;
    cover_span(cover, pose, &start, &end);
    for (int x = start; x < end; ++x) {
        unsigned source_x;
        int top, height;
        if (!cover_column(cover, pose, x, &source_x, &top, &height)) continue;
        top += offset_y;
        unsigned step = (cover->height << 16) / height;
        unsigned sample = 0;
        for (int y = 0; y < height; ++y, sample += step) {
            if ((unsigned)(top + y) >= HEIGHT) continue;
            uint32_t color = cover->pixels[(sample >> 16) * cover->width + source_x];
            unsigned alpha = (color >> 24) + (color >> 31);
            if (!alpha) continue;
            uint32_t *pixel = canvas + (top + y) * WIDTH + x;
            if (pose.brightness != 256) color = composite(color, 0, pose.brightness);
            *pixel = alpha == 256 ? color : composite(color, *pixel, alpha);
        }

        int depth = height * REFLECTION / COVER_SIZE;
        int bottom = top + height;
        for (int y = 0; y < depth; ++y) {
            if ((unsigned)(bottom + y) >= HEIGHT) continue;
            unsigned source_y = cover->height - 1 - (((unsigned)y * step) >> 16);
            uint32_t color = cover->pixels[source_y * cover->width + source_x];
            unsigned opacity = (color >> 24) + (color >> 31);
            unsigned fade = (depth - y) * 90 * pose.brightness / (depth * 256);
            uint32_t *pixel = canvas + (bottom + y) * WIDTH + x;
            *pixel = composite(color, *pixel, fade * opacity / 256);
        }
    }
}
