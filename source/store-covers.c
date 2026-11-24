#include "catalog.h"
#include "download.h"
#include "store-covers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"
#include <webp/decode.h>

#define STORE_ORIGIN "https://switch2homebrew.com"
#define CACHE_FOLDER "sdmc:/switch/switch2homebrew-store/covers"

enum { COVER_BYTES = STORE_COVER_WIDTH * STORE_COVER_HEIGHT * 3, IMAGE_LIMIT = 4 * 1024 * 1024 };
extern const unsigned char _binary_store_covers_rgb_start[];
static bool available_categories[STORE_CATEGORY_COUNT];

static size_t bundled_index(const char *id)
{
    for (size_t i = 0; i < store_catalog_count; ++i)
        if (strcmp(id, store_catalog[i].id) == 0)
            return i;
    return store_catalog_count;
}

void store_catalog_set_active(const StoreCatalogEntry *entries, size_t count)
{
    memset(available_categories, 0, sizeof available_categories);
    available_categories[STORE_ALL] = true;
    for (size_t i = 0; i < count; ++i)
        if ((unsigned)entries[i].category < STORE_CATEGORY_COUNT)
            available_categories[entries[i].category] = true;
}

bool store_catalog_has_category(StoreCategory category)
{
    return (unsigned)category < STORE_CATEGORY_COUNT && (category == STORE_ALL || available_categories[category]);
}

const unsigned char *store_cover_pixels(const StoreCatalogEntry *entry)
{
    if (entry->cover_pixels)
        return entry->cover_pixels;
    const size_t index = bundled_index(entry->id);
    if (index < store_catalog_count && *store_catalog[index].image_credit)
        return _binary_store_covers_rgb_start + index * COVER_BYTES;
    return NULL;
}

const char *store_cover_credit(const StoreCatalogEntry *entry)
{
    if (entry->cover_pixels)
        return entry->image_credit;
    const size_t index = bundled_index(entry->id);
    return index < store_catalog_count ? store_catalog[index].image_credit : NULL;
}

const char *store_project_source(const StoreCatalogEntry *entry)
{
    if (entry->source_url && *entry->source_url)
        return entry->source_url;
    const size_t index = bundled_index(entry->id);
    if (index < store_catalog_count)
        return store_catalog[index].source_url;
    return NULL;
}

static unsigned char *read_cache(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    unsigned char *pixels = malloc(COVER_BYTES);
    if (pixels && (fread(pixels, 1, COVER_BYTES, file) != COVER_BYTES || fgetc(file) != EOF || ferror(file))) {
        free(pixels);
        pixels = NULL;
    }
    fclose(file);
    return pixels;
}

static void save_cache(const char *path, const unsigned char *pixels)
{
    char staging[160];
    const int length = snprintf(staging, sizeof staging, "%s.part", path);
    if (length < 0 || (size_t)length >= sizeof staging)
        return;
    FILE *file = fopen(staging, "wb");
    if (!file)
        return;
    const bool complete = fwrite(pixels, 1, COVER_BYTES, file) == COVER_BYTES;
    const int closed = fclose(file);
    if (complete && !closed && rename(staging, path) == 0)
        fsdevCommitDevice("sdmc");
    else
        remove(staging);
}

static unsigned char *fit_image(const unsigned char *source, int width, int height)
{
    unsigned char *pixels = malloc(COVER_BYTES);
    if (!pixels)
        return NULL;
    int fitted_width = STORE_COVER_WIDTH, fitted_height = STORE_COVER_HEIGHT;
    if (width * STORE_COVER_HEIGHT < height * STORE_COVER_WIDTH)
        fitted_width = width * STORE_COVER_HEIGHT / height;
    else
        fitted_height = height * STORE_COVER_WIDTH / width;
    if (fitted_width < 1)
        fitted_width = 1;
    if (fitted_height < 1)
        fitted_height = 1;
    const int left = (STORE_COVER_WIDTH - fitted_width) / 2;
    const int top = (STORE_COVER_HEIGHT - fitted_height) / 2;
    for (int y = 0; y < STORE_COVER_HEIGHT; ++y) {
        for (int x = 0; x < STORE_COVER_WIDTH; ++x) {
            unsigned char *out = pixels + (y * STORE_COVER_WIDTH + x) * 3;
            if (x >= left && x < left + fitted_width && y >= top && y < top + fitted_height) {
                const int sx = (x - left) * width / fitted_width;
                const int sy = (y - top) * height / fitted_height;
                memcpy(out, source + (sy * width + sx) * 3, 3);
            } else {
                out[0] = 22;
                out[1] = 35;
                out[2] = 52;
            }
        }
    }
    return pixels;
}

static unsigned char *decode_image(const unsigned char *bytes, int length)
{
    int width, height;
    const bool webp = WebPGetInfo(bytes, (size_t)length, &width, &height);
    if ((!webp && !stbi_info_from_memory(bytes, length, &width, &height, NULL)) ||
        width <= 0 || height <= 0 || width > 4096 || height > 4096 || (uint64_t)width * height > 5000000)
        return NULL;
    unsigned char *source = webp ? WebPDecodeRGB(bytes, (size_t)length, &width, &height) :
        stbi_load_from_memory(bytes, length, &width, &height, NULL, 3);
    if (!source)
        return NULL;
    unsigned char *pixels = fit_image(source, width, height);
    if (webp)
        WebPFree(source);
    else
        stbi_image_free(source);
    return pixels;
}

unsigned char *store_cover_fetch(const char *url, const atomic_bool *cancel)
{
    if (!url || strncmp(url, STORE_ORIGIN "/", sizeof STORE_ORIGIN) != 0)
        return NULL;
    unsigned char digest[32];
    Sha256Context hash;
    sha256ContextCreate(&hash);
    sha256ContextUpdate(&hash, url, strlen(url));
    sha256ContextGetHash(&hash, digest);
    char key[33];
    for (size_t i = 0; i < 16; ++i)
        snprintf(key + i * 2, 3, "%02x", (unsigned)digest[i]);
    char path[128];
    snprintf(path, sizeof path, CACHE_FOLDER "/%s.rgb", key);
    unsigned char *pixels = read_cache(path);
    if (pixels || atomic_load(cancel))
        return pixels;

    mkdir(CACHE_FOLDER, 0777);
    const char *temporary = CACHE_FOLDER "/incoming.part";
    FILE *file = fopen(temporary, "w+b");
    if (!file)
        return NULL;
    StoreInstallError error = {0};
    int status = 0;
    if (download_store_resource(file, url + sizeof STORE_ORIGIN - 1, IMAGE_LIMIT, cancel, &status, &error) &&
        status == 200 && fflush(file) == 0 && fseek(file, 0, SEEK_END) == 0) {
        const long length = ftell(file);
        if (length > 0 && length <= IMAGE_LIMIT && fseek(file, 0, SEEK_SET) == 0 && !atomic_load(cancel)) {
            unsigned char *bytes = malloc((size_t)length);
            if (bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length)
                pixels = decode_image(bytes, (int)length);
            free(bytes);
        }
    }
    fclose(file);
    remove(temporary);
    if (pixels)
        save_cache(path, pixels);
    return pixels;
}
