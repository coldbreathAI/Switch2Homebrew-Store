#pragma once

#include "catalog.h"
#include "download.h"

typedef struct {
    StoreCatalogEntry *entries;
    size_t count;
    unsigned revision;
    char *storage;
} StoreLiveCatalog;

/* Start with a zeroed catalog. A failed read or refresh preserves the current value. */
bool catalog_sync_read_cache(StoreLiveCatalog *catalog, StoreInstallError *error);
/* A successful refresh can return a warning in error if saving to the SD card fails. */
bool catalog_sync_refresh(StoreLiveCatalog *catalog, const atomic_bool *cancel, StoreInstallError *error);
bool catalog_sync_check_project(const char *id, const StorePackage *expected,
                                const atomic_bool *cancel, StoreInstallError *error);
void catalog_sync_free(StoreLiveCatalog *catalog);
