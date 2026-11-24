#define _POSIX_C_SOURCE 200809L

#include "catalog-sync.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#define SD_DEVICE "sdmc:"
#define CACHE_DIR "/switch/switch2homebrew-store"
#define CACHE_FILE CACHE_DIR "/catalog-cache.sources.json"
#define PREVIOUS_CACHE_FILE CACHE_DIR "/catalog-cache.sources.previous.json"
#define CACHE_PATH SD_DEVICE CACHE_FILE
#define PREVIOUS_CACHE_PATH SD_DEVICE PREVIOUS_CACHE_FILE
#define LEGACY_CACHE_PATH SD_DEVICE CACHE_DIR "/catalog-cache.json"
#define LEGACY_PREVIOUS_CACHE_PATH SD_DEVICE CACHE_DIR "/catalog-cache.previous.json"
#define CATALOG_PATH "/api/store/v1/catalog?sources=1"
#define INSTALL_PATH "/api/store/v1/install/"

enum {
    manifest_limit = 512 * 1024, install_response_limit = 16 * 1024,
    entry_limit = 512, id_limit = 120, install_path_limit = 512, source_url_limit = 300,
};

typedef struct {
    char *cursor;
    const char *end;
} Json;

static void json_skip_space(Json *json)
{
    while (json->cursor < json->end && *json->cursor && strchr(" \t\r\n", *json->cursor))
        ++json->cursor;
}

static bool json_take(Json *json, char value)
{
    json_skip_space(json);
    if (json->cursor == json->end || *json->cursor != value)
        return false;
    ++json->cursor;
    return true;
}

static bool json_word(Json *json, const char *word)
{
    json_skip_space(json);
    const size_t length = strlen(word);
    if ((size_t)(json->end - json->cursor) < length || memcmp(json->cursor, word, length) != 0)
        return false;
    json->cursor += length;
    return true;
}

static int hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static bool json_hex_quad(Json *json, uint32_t *value)
{
    if (json->end - json->cursor < 4)
        return false;
    *value = 0;
    for (int i = 0; i < 4; ++i) {
        const int digit = hex_digit((unsigned char)*json->cursor++);
        if (digit < 0)
            return false;
        *value = *value * 16 + (unsigned)digit;
    }
    return true;
}

static bool json_escape(Json *json, uint32_t *point)
{
    if (json->cursor == json->end)
        return false;
    switch (*json->cursor++) {
    case '"': *point = '"'; return true;
    case '\\': *point = '\\'; return true;
    case '/': *point = '/'; return true;
    case 'n': *point = '\n'; return true;
    case 'r': *point = '\r'; return true;
    case 't': *point = '\t'; return true;
    case 'u':
        if (!json_hex_quad(json, point))
            return false;
        if (*point >= 0xd800 && *point <= 0xdbff) {
            uint32_t low;
            if (json->end - json->cursor < 2 || json->cursor[0] != '\\' || json->cursor[1] != 'u')
                return false;
            json->cursor += 2;
            if (!json_hex_quad(json, &low) || low < 0xdc00 || low > 0xdfff)
                return false;
            *point = 0x10000 + ((*point - 0xd800) << 10) + low - 0xdc00;
        }
        return true;
    default:
        return false;
    }
}

static bool json_utf8_tail(Json *json, unsigned leading, uint32_t *point)
{
    unsigned extra;
    uint32_t minimum;
    if (leading >= 0xc2 && leading <= 0xdf) {
        *point = leading & 0x1f;
        extra = 1;
        minimum = 0x80;
    } else if (leading >= 0xe0 && leading <= 0xef) {
        *point = leading & 0x0f;
        extra = 2;
        minimum = 0x800;
    } else if (leading >= 0xf0 && leading <= 0xf4) {
        *point = leading & 0x07;
        extra = 3;
        minimum = 0x10000;
    } else {
        return false;
    }
    if ((size_t)(json->end - json->cursor) < extra)
        return false;
    for (unsigned i = 0; i < extra; ++i) {
        const unsigned ch = (unsigned char)*json->cursor++;
        if ((ch & 0xc0) != 0x80)
            return false;
        *point = (*point << 6) | (ch & 0x3f);
    }
    return *point >= minimum && *point <= 0x10ffff;
}

/* Decoding never expands past the existing input bytes. */
static char *json_string(Json *json, size_t limit)
{
    if (!json_take(json, '"'))
        return NULL;
    char *const start = json->cursor;
    char *out = start;
    while (json->cursor < json->end) {
        const unsigned ch = (unsigned char)*json->cursor++;
        if (ch == '"') {
            *out = '\0';
            return start;
        }
        if (ch < 0x20)
            return NULL;
        uint32_t point = ch;
        if ((ch == '\\' && !json_escape(json, &point)) ||
            (ch >= 0x80 && !json_utf8_tail(json, ch, &point)))
            return NULL;
        if ((point < 0x20 && point != '\n' && point != '\r' && point != '\t') ||
            (point >= 0xd800 && point <= 0xdfff))
            return NULL;
        uint8_t bytes[4];
        const ssize_t width = encode_utf8(bytes, point);
        if (width < 0 || (size_t)((out - start) + width) > limit)
            return NULL;
        memcpy(out, bytes, (size_t)width);
        out += width;
    }
    return NULL;
}

static bool json_uint(Json *json, uint64_t *value)
{
    json_skip_space(json);
    const char *start = json->cursor;
    uint64_t result = 0;
    while (json->cursor < json->end && *json->cursor >= '0' && *json->cursor <= '9') {
        const unsigned digit = (unsigned)(*json->cursor++ - '0');
        if (result > (UINT64_MAX - digit) / 10u)
            return false;
        result = result * 10u + digit;
    }
    if (json->cursor == start || (*start == '0' && json->cursor != start + 1))
        return false;
    *value = result;
    return true;
}

static bool json_bool(Json *json, bool *value)
{
    if (json_word(json, "true")) {
        *value = true;
        return true;
    }
    if (json_word(json, "false")) {
        *value = false;
        return true;
    }
    return false;
}

static int json_key(Json *json, const char *const *keys, unsigned count, unsigned *seen)
{
    const char *key = json_string(json, 64);
    if (!key || !json_take(json, ':'))
        return -1;
    for (unsigned i = 0; i < count; ++i) {
        if (strcmp(key, keys[i]) == 0) {
            const unsigned bit = 1u << i;
            if (*seen & bit)
                return -1;
            *seen |= bit;
            return (int)i;
        }
    }
    return -1;
}

static bool ascii_alnum(unsigned char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9');
}

static bool valid_id(const char *id)
{
    if (!id || !*id || strlen(id) > id_limit || *id == '-')
        return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
            return false;
    }
    return true;
}

static bool valid_project_url(const char *url)
{
    static const char prefix[] = STORE_WEB_ORIGIN "/projects/";
    return url && strncmp(url, prefix, sizeof prefix - 1) == 0 && valid_id(url + sizeof prefix - 1);
}

static bool valid_source_url(const char *url)
{
    static const char scheme[] = "https://";
    if (strncmp(url, scheme, sizeof scheme - 1) != 0)
        return false;
    const char *host = url + sizeof scheme - 1;
    const size_t host_size = strcspn(host, "/?#");
    if (!host_size || host_size > 253)
        return false;

    const char *const end = host + host_size;
    bool dotted = false, label_has_letter = false;
    size_t label_size = 0;
    for (const char *p = host; p < end; ++p) {
        const unsigned char ch = (unsigned char)*p;
        if (ch == '.') {
            if (!label_size || p[-1] == '-')
                return false;
            dotted = true;
            label_has_letter = false;
            label_size = 0;
        } else {
            if ((!ascii_alnum(ch) && (ch != '-' || !label_size)) || ++label_size > 63)
                return false;
            label_has_letter |= (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        }
    }
    if (!dotted || !label_size || !label_has_letter || end[-1] == '-')
        return false;

    for (const unsigned char *p = (const unsigned char *)end; *p; ++p) {
        if (*p == '%') {
            if (!p[1] || !p[2])
                return false;
            const int high = hex_digit(p[1]), low = hex_digit(p[2]);
            if (high < 0 || low < 0)
                return false;
            const int octet = high * 16 + low;
            if (octet < 0x20 || octet == 0x7f)
                return false;
            p += 2;
        } else if (!ascii_alnum(*p) && !strchr("-._~:/?#[]@!$&'()*+,;=", *p)) {
            return false;
        }
    }
    return true;
}

static bool has_suffix(const char *text, const char *suffix)
{
    const size_t text_length = strlen(text), suffix_length = strlen(suffix);
    return text_length > suffix_length && strcasecmp(text + text_length - suffix_length, suffix) == 0;
}

static bool valid_uploaded_cover_path(const char *path)
{
    if (strlen(path) != 38 || path[36] != '/' || path[37] < '1' || path[37] > '3')
        return false;
    for (size_t i = 0; i < 36; ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const char ch = path[i];
        if (dash ? ch != '-' : !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return true;
}

static bool valid_cover_url(const char *url)
{
    if (!url)
        return false;
    if (!url[0])
        return true;
    static const char media[] = STORE_WEB_ORIGIN "/media/projects/";
    static const char screenshots[] = STORE_WEB_ORIGIN "/screenshots/";
    if (strncmp(url, media, sizeof media - 1) == 0)
        return valid_uploaded_cover_path(url + sizeof media - 1);
    if (strncmp(url, screenshots, sizeof screenshots - 1) != 0)
        return false;
    const char *path = url + sizeof screenshots - 1;
    const char *segment = path;
    for (const char *p = path; ; ++p) {
        const unsigned char ch = (unsigned char)*p;
        if (ch == '/' || !ch) {
            const size_t length = (size_t)(p - segment);
            if (!length || (segment[0] == '.' &&
                (length == 1 || (length == 2 && segment[1] == '.'))))
                return false;
            if (!ch)
                break;
            segment = p + 1;
        } else if (!ascii_alnum(ch) && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return has_suffix(path, ".jpg") || has_suffix(path, ".jpeg") ||
        has_suffix(path, ".png") || has_suffix(path, ".webp");
}

static bool reserved_folder(const char *name, size_t length)
{
    static const char *const reserved[] = {
        "atmosphere", "bootloader", "kaihen", "Nintendo", "sept", "switch2homebrew-store",
    };
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; ++i) {
        if (length == strlen(reserved[i]) && strncasecmp(name, reserved[i], length) == 0)
            return true;
    }
    return false;
}

static bool safe_relative_path(const char *path)
{
    if (!path || !*path || strlen(path) >= install_path_limit)
        return false;
    unsigned depth = 0;
    for (;;) {
        const char *next = strchr(path, '/');
        const size_t length = next ? (size_t)(next - path) : strlen(path);
        if (++depth > 16 || !length || length > 120 || path[0] == ' ' ||
            path[length - 1] == '.' || path[length - 1] == ' ' || reserved_folder(path, length))
            return false;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char ch = (unsigned char)path[i];
            if (ch < 32 || ch > 126 || strchr("\\/:*?\"<>|", ch))
                return false;
        }
        if (!next)
            return true;
        path = next + 1;
    }
}

static bool safe_nro_name(const char *path)
{
    const char *file = strrchr(path, '/');
    file = file ? file + 1 : path;
    return has_suffix(file, ".nro") && strcasecmp(file, "kaihen.nro") != 0;
}

static bool valid_target(const char *id, const StorePackage *package)
{
    static const char prefix[] = "sdmc:/switch/";
    const char *target = package->target;
    if (strlen(target) >= install_path_limit - 32)
        return false;
    if (strncmp(target, prefix, sizeof prefix - 1) != 0)
        return package->kind == STORE_PACKAGE_ZIP && strcmp(id, "retroarch") == 0 &&
            strcmp(target, "sdmc:/retroarch") == 0;
    const char *relative = target + sizeof prefix - 1;
    if (!safe_relative_path(relative))
        return false;
    const char *slash = strchr(relative, '/');
    if (package->kind == STORE_PACKAGE_ZIP)
        return !slash;
    return (!slash || !strchr(slash + 1, '/')) && safe_nro_name(relative);
}

static bool valid_package(const char *id, const StorePackage *package)
{
    static const uint8_t zero[32];
    if (strcmp(id, "kaihen") == 0 || strcmp(id, "ounceshell") == 0 ||
        !package->version[0] || !download_package_url_allowed(package->url) ||
        !valid_target(id, package) || package->size > STORE_DOWNLOAD_LIMIT ||
        memcmp(package->sha256, zero, sizeof zero) == 0)
        return false;
    const char *root = package->archive_root, *secondary = package->archive_secondary_root;
    if (package->kind == STORE_PACKAGE_NRO)
        return !root[0] && !secondary[0] && !package->marker[0];
    return safe_relative_path(package->marker) && safe_nro_name(package->marker) &&
        (!root[0] || safe_relative_path(root)) &&
        (!secondary[0] || (root[0] && safe_relative_path(secondary) && strcmp(root, secondary) != 0));
}

static bool parse_sha256(const char *text, uint8_t digest[32])
{
    if (!text || strlen(text) != 64)
        return false;
    for (size_t i = 0; i < 32; ++i) {
        const int high = hex_digit((unsigned char)text[2 * i]);
        const int low = hex_digit((unsigned char)text[2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        digest[i] = (uint8_t)(high * 16 + low);
    }
    return true;
}

static bool parse_package(Json *json, StorePackage *package)
{
    static const char *const keys[] = {
        "kind", "version", "url", "target", "archive_root", "archive_secondary_root", "marker",
        "size", "sha256",
    };
    if (json_word(json, "null"))
        return true;
    if (!json_take(json, '{'))
        return false;

    unsigned seen = 0;
    do {
        switch (json_key(json, keys, 9, &seen)) {
        case 0: {
            const char *kind = json_string(json, 3);
            if (!kind)
                return false;
            if (strcmp(kind, "nro") == 0)
                package->kind = STORE_PACKAGE_NRO;
            else if (strcmp(kind, "zip") == 0)
                package->kind = STORE_PACKAGE_ZIP;
            else
                return false;
            break;
        }
        case 1: if (!(package->version = json_string(json, 120))) return false; break;
        case 2: if (!(package->url = json_string(json, 4095))) return false; break;
        case 3: if (!(package->target = json_string(json, install_path_limit - 1))) return false; break;
        case 4: if (!(package->archive_root = json_string(json, install_path_limit - 1))) return false; break;
        case 5: if (!(package->archive_secondary_root = json_string(json, install_path_limit - 1))) return false; break;
        case 6: if (!(package->marker = json_string(json, install_path_limit - 1))) return false; break;
        case 7: if (!json_uint(json, &package->size)) return false; break;
        case 8: if (!parse_sha256(json_string(json, 64), package->sha256)) return false; break;
        default: return false;
        }
    } while (json_take(json, ','));
    return seen == ((1u << 9) - 1u) && json_take(json, '}');
}

static bool parse_category(Json *json, StoreCategory *category)
{
    static const char *const values[] = {
        [STORE_EMULATION] = "emulation", [STORE_PORTS] = "ports", [STORE_MEDIA] = "media",
        [STORE_UTILITIES] = "utilities", [STORE_DEVELOPMENT] = "development",
    };
    const char *name = json_string(json, 16);
    if (!name)
        return false;
    for (StoreCategory i = STORE_EMULATION; i < STORE_CATEGORY_COUNT; ++i) {
        if (strcmp(name, values[i]) == 0) {
            *category = i;
            return true;
        }
    }
    return false;
}

static bool parse_entry(Json *json, StoreCatalogEntry *entry)
{
    static const char *const keys[] = {
        "id", "name", "category", "summary", "summary_es", "requirement", "requirement_es",
        "image_credit", "cover_url", "project_url", "editing", "package", "source_url",
    };
    if (!json_take(json, '{'))
        return false;
    unsigned seen = 0;
    do {
        switch (json_key(json, keys, 13, &seen)) {
        case 0: if (!valid_id(entry->id = json_string(json, id_limit))) return false; break;
        case 1: if (!(entry->name = json_string(json, 180)) || !entry->name[0]) return false; break;
        case 2: if (!parse_category(json, &entry->category)) return false; break;
        case 3: if (!(entry->summary = json_string(json, 2048))) return false; break;
        case 4: if (!(entry->summary_es = json_string(json, 2048))) return false; break;
        case 5: if (!(entry->requirement = json_string(json, 2048))) return false; break;
        case 6: if (!(entry->requirement_es = json_string(json, 2048))) return false; break;
        case 7: if (!(entry->image_credit = json_string(json, 384))) return false; break;
        case 8: if (!valid_cover_url(entry->cover_url = json_string(json, 2048))) return false; break;
        case 9: if (!valid_project_url(entry->project_url = json_string(json, 256))) return false; break;
        case 10: if (!json_bool(json, &entry->editing)) return false; break;
        case 11: if (!parse_package(json, &entry->package)) return false; break;
        case 12: {
            const char *source = json_string(json, source_url_limit);
            if (!source)
                return false;
            if (valid_source_url(source))
                entry->source_url = source;
            break;
        }
        default: return false;
        }
    } while (json_take(json, ','));
    const unsigned required = (1u << 12) - 1u;
    return (seen & required) == required && json_take(json, '}') &&
        (entry->package.kind == STORE_PACKAGE_NONE || valid_package(entry->id, &entry->package));
}

static bool path_conflict(const char *first, const char *second)
{
    const size_t a = strlen(first), b = strlen(second);
    const size_t len = a < b ? a : b;
    return strncasecmp(first, second, len) == 0 &&
        (a == b || (a < b && second[a] == '/') || (b < a && first[b] == '/'));
}

static bool unique_entry(const StoreLiveCatalog *catalog, const StoreCatalogEntry *entry)
{
    for (size_t i = 0; i < catalog->count; ++i) {
        const StoreCatalogEntry *other = &catalog->entries[i];
        if (strcmp(entry->id, other->id) == 0)
            return false;
        if (entry->package.kind != STORE_PACKAGE_NONE && other->package.kind != STORE_PACKAGE_NONE &&
            path_conflict(entry->package.target, other->package.target))
            return false;
    }
    return true;
}

static bool parse_entries(Json *json, StoreLiveCatalog *catalog)
{
    if (!json_take(json, '['))
        return false;
    if (json_take(json, ']'))
        return true;
    do {
        if (catalog->count >= entry_limit)
            return false;
        StoreCatalogEntry *entry = &catalog->entries[catalog->count];
        if (!parse_entry(json, entry) || !unique_entry(catalog, entry))
            return false;
        ++catalog->count;
    } while (json_take(json, ','));
    return json_take(json, ']');
}

static bool parse_catalog(Json *json, StoreLiveCatalog *catalog)
{
    static const char *const keys[] = {"schema", "revision", "entries"};
    if (!json_take(json, '{'))
        return false;
    unsigned seen = 0;
    do {
        uint64_t value;
        switch (json_key(json, keys, 3, &seen)) {
        case 0:
            if (!json_uint(json, &value) || value != 1)
                return false;
            break;
        case 1:
            if (!json_uint(json, &value) || value > UINT_MAX)
                return false;
            catalog->revision = (unsigned)value;
            break;
        case 2:
            if (!parse_entries(json, catalog))
                return false;
            break;
        default:
            return false;
        }
    } while (json_take(json, ','));
    if (seen != 7u || !json_take(json, '}'))
        return false;
    json_skip_space(json);
    return json->cursor == json->end;
}

void catalog_sync_free(StoreLiveCatalog *catalog)
{
    free(catalog->entries);
    free(catalog->storage);
    *catalog = (StoreLiveCatalog){0};
}

static bool decode_catalog(const char *raw, size_t length, StoreLiveCatalog *catalog, StoreInstallError *error)
{
    if (!length || length > manifest_limit)
        return store_install_error(error, "The online catalog has an invalid size.",
                                   "El catálogo en línea tiene un tamaño inválido.");
    catalog->storage = malloc(length);
    catalog->entries = calloc(entry_limit, sizeof *catalog->entries);
    if (!catalog->storage || !catalog->entries) {
        catalog_sync_free(catalog);
        return store_install_error(error, "There is not enough memory for the catalog.",
                                   "Memoria insuficiente para el catálogo.");
    }
    memcpy(catalog->storage, raw, length);
    Json json = {catalog->storage, catalog->storage + length};
    if (!parse_catalog(&json, catalog)) {
        catalog_sync_free(catalog);
        return store_install_error(error, "The online catalog has an invalid or unsupported format.",
                                   "El catálogo en línea tiene un formato inválido o no compatible.");
    }
    return true;
}

static bool read_cached_file(const char *path, StoreLiveCatalog *next, StoreInstallError *error)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    struct stat info;
    bool ok = false;
    if (fstat(fileno(file), &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size > 0 && (uint64_t)info.st_size <= manifest_limit) {
        const size_t size = (size_t)info.st_size;
        char *raw = malloc(size);
        if (raw) {
            ok = fread(raw, 1, size, file) == size && fgetc(file) == EOF && !ferror(file) &&
                decode_catalog(raw, size, next, error);
            free(raw);
        }
    }
    fclose(file);
    return ok;
}

bool catalog_sync_read_cache(StoreLiveCatalog *catalog, StoreInstallError *error)
{
    *error = (StoreInstallError){0};
    StoreLiveCatalog next = {0};
    if (!read_cached_file(CACHE_PATH, &next, error) &&
        !read_cached_file(PREVIOUS_CACHE_PATH, &next, error) &&
        !read_cached_file(LEGACY_CACHE_PATH, &next, error) &&
        !read_cached_file(LEGACY_PREVIOUS_CACHE_PATH, &next, error))
        return store_install_error(error, "No valid saved catalog is available.",
                                   "No hay un catálogo guardado válido.");
    *error = (StoreInstallError){0};
    catalog_sync_free(catalog);
    *catalog = next;
    return true;
}

static bool ensure_directory(const char *path)
{
    if (mkdir(path, 0777) == 0)
        return true;
    struct stat info;
    return errno == EEXIST && lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool rotate_cache(FsFileSystem *sd)
{
    struct stat info;
    if (lstat(CACHE_PATH, &info) != 0)
        return errno == ENOENT;
    if (!S_ISREG(info.st_mode))
        return false;

    StoreLiveCatalog previous = {0};
    StoreInstallError ignored;
    if (read_cached_file(CACHE_PATH, &previous, &ignored)) {
        catalog_sync_free(&previous);
        if (unlink(PREVIOUS_CACHE_PATH) != 0 && errno != ENOENT)
            return false;
    } else {
        /* Preserve the other copy if the primary could not be validated. */
        if (lstat(PREVIOUS_CACHE_PATH, &info) == 0)
            return S_ISREG(info.st_mode) && unlink(CACHE_PATH) == 0;
        if (errno != ENOENT)
            return false;
    }
    return R_SUCCEEDED(fsFsRenameFile(sd, CACHE_FILE, PREVIOUS_CACHE_FILE));
}

static bool persist_cache(const char *raw, size_t length, const atomic_bool *cancel)
{
    FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");
    if (!sd || !ensure_directory(SD_DEVICE "/switch") || !ensure_directory(SD_DEVICE CACHE_DIR))
        return false;
    char temporary[] = SD_DEVICE CACHE_DIR "/catalog-cache.sources.XXXXXX";
    const int fd = mkstemp(temporary);
    if (fd < 0)
        return false;
    FILE *file = fdopen(fd, "wb");
    bool saved = false;
    if (file) {
        saved = fwrite(raw, 1, length, file) == length && fflush(file) == 0 && fsync(fd) == 0;
        if (fclose(file) != 0)
            saved = false;
    } else {
        close(fd);
    }

    if (saved && !atomic_load(cancel) && rotate_cache(sd) &&
        R_SUCCEEDED(fsFsRenameFile(sd, temporary + sizeof(SD_DEVICE) - 1, CACHE_FILE)))
        return R_SUCCEEDED(fsdevCommitDevice("sdmc"));
    unlink(temporary);
    return false;
}

static bool fetch_json(const char *path, unsigned limit, const atomic_bool *cancel,
                       char **raw, size_t *length, int *status, StoreInstallError *error)
{
    *status = 0;
    FILE *buffer = open_memstream(raw, length);
    if (!buffer)
        return store_install_error(error, "Could not allocate memory for the store response.",
                                   "No se pudo reservar memoria para la respuesta de la tienda.");
    const bool received = download_store_resource(buffer, path, limit, cancel, status, error);
    const bool closed = fclose(buffer) == 0;
    if (!received)
        return false;
    if (!closed)
        return store_install_error(error, "Could not read the store response.",
                                   "No se pudo leer la respuesta de la tienda.");
    return true;
}

bool catalog_sync_refresh(StoreLiveCatalog *catalog, const atomic_bool *cancel, StoreInstallError *error)
{
    *error = (StoreInstallError){0};
    char *raw = NULL;
    size_t length = 0;
    int status;
    StoreLiveCatalog next = {0};
    const bool ok = fetch_json(CATALOG_PATH, manifest_limit, cancel, &raw, &length, &status, error) &&
        decode_catalog(raw, length, &next, error);
    const bool saved = ok && !atomic_load(cancel) && persist_cache(raw, length, cancel);
    free(raw);
    if (ok && !atomic_load(cancel)) {
        if (!saved)
            store_install_error(error, "The online catalog is available, but could not be saved to the microSD card.",
                                 "El catálogo en línea está disponible, pero no se pudo guardar en la microSD.");
        catalog_sync_free(catalog);
        *catalog = next;
        return true;
    }
    catalog_sync_free(&next);
    if (atomic_load(cancel))
        return store_install_error(error, "Catalog check cancelled.", "Consulta del catálogo cancelada.");
    return false;
}

static bool parse_authorization(Json *json, const char *expected_id, StorePackage *package)
{
    static const char *const keys[] = {"schema", "id", "package"};
    if (!json_take(json, '{'))
        return false;
    unsigned seen = 0;
    do {
        switch (json_key(json, keys, 3, &seen)) {
        case 0: {
            uint64_t schema;
            if (!json_uint(json, &schema) || schema != 1)
                return false;
            break;
        }
        case 1: {
            const char *id = json_string(json, id_limit);
            if (!id || strcmp(id, expected_id) != 0)
                return false;
            break;
        }
        case 2:
            if (!parse_package(json, package))
                return false;
            break;
        default:
            return false;
        }
    } while (json_take(json, ','));
    if (seen != 7u || !json_take(json, '}') ||
        package->kind == STORE_PACKAGE_NONE || !valid_package(expected_id, package))
        return false;
    json_skip_space(json);
    return json->cursor == json->end;
}

static bool same_text(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

static bool same_package(const StorePackage *a, const StorePackage *b)
{
    return a->kind == b->kind && a->size == b->size && same_text(a->version, b->version) &&
        same_text(a->url, b->url) && same_text(a->target, b->target) &&
        same_text(a->archive_root, b->archive_root) && same_text(a->archive_secondary_root, b->archive_secondary_root) &&
        same_text(a->marker, b->marker) && memcmp(a->sha256, b->sha256, sizeof a->sha256) == 0;
}

bool catalog_sync_check_project(const char *id, const StorePackage *expected,
                                const atomic_bool *cancel, StoreInstallError *error)
{
    *error = (StoreInstallError){0};
    if (!valid_id(id) || !expected || expected->kind == STORE_PACKAGE_NONE)
        return store_install_error(error, "This project has no verified package.",
                                   "Este proyecto no tiene un paquete verificado.");
    char path[sizeof INSTALL_PATH + id_limit];
    snprintf(path, sizeof path, "%s%s", INSTALL_PATH, id);
    char *raw = NULL;
    size_t length = 0;
    int status;
    const bool received = fetch_json(path, install_response_limit, cancel, &raw, &length, &status, error);
    if (atomic_load(cancel)) {
        free(raw);
        return store_install_error(error, "Download cancelled.", "Descarga cancelada.");
    }
    if (!received) {
        free(raw);
        if (status == 423)
            return store_install_error(error, "This project is being edited. Try again later.",
                                       "Este proyecto está en edición. Inténtalo más tarde.");
        if (status == 404)
            return store_install_error(error, "This project is no longer listed in the online catalog.",
                                       "Este proyecto ya no figura en el catálogo en línea.");
        if (status == 409)
            return store_install_error(error, "This project no longer has a verified package.",
                                       "Este proyecto ya no tiene un paquete verificado.");
        return false;
    }
    StorePackage fresh = {0};
    Json json = {raw, raw + length};
    const bool valid = parse_authorization(&json, id, &fresh);
    const bool matched = valid && same_package(expected, &fresh);
    free(raw);
    if (!valid)
        return store_install_error(error, "The project's verification response is invalid.",
                                   "La respuesta de verificación del proyecto no es válida.");
    if (!matched)
        return store_install_error(error, "The package changed online. Refresh the catalog and try again.",
                                   "El paquete cambió en línea. Actualiza el catálogo e inténtalo de nuevo.");
    return true;
}
