#ifndef SWITCH2HOMEBREW_STORE_CATALOG_H
#define SWITCH2HOMEBREW_STORE_CATALOG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STORE_PACKAGE_NONE, STORE_PACKAGE_NRO, STORE_PACKAGE_ZIP
} StorePackageKind;

typedef struct StorePackage {
    StorePackageKind kind;
    const char *version, *url, *target, *archive_root, *archive_secondary_root, *marker;
    uint64_t size;
    uint8_t sha256[32];
} StorePackage;

typedef enum {
    STORE_ALL, STORE_EMULATION, STORE_PORTS, STORE_MEDIA, STORE_UTILITIES, STORE_DEVELOPMENT, STORE_CATEGORY_COUNT
} StoreCategory;

typedef struct {
    const char *id;
    const char *name;
    StoreCategory category;
    const char *summary;
    const char *requirement;
    const char *summary_es;
    const char *requirement_es;
    const char *image_credit;
    StorePackage package;
    const char *cover_url;
    const char *project_url;
    const char *source_url;
    unsigned char *cover_pixels;
    bool editing;
    bool cover_attempted;
} StoreCatalogEntry;

enum { STORE_COVER_WIDTH = 384, STORE_COVER_HEIGHT = 216, STORE_LOGO_SIZE = 72 };

extern const StoreCatalogEntry store_catalog[];
extern const size_t store_catalog_count;
extern const size_t store_installable_count;
extern const unsigned store_catalog_revision;

void store_catalog_set_active(const StoreCatalogEntry *entries, size_t count);
bool store_catalog_has_category(StoreCategory category);
const unsigned char *store_cover_pixels(const StoreCatalogEntry *entry);
const char *store_cover_credit(const StoreCatalogEntry *entry);
const char *store_project_source(const StoreCatalogEntry *entry);

#endif
