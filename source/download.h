#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define STORE_DOWNLOAD_LIMIT (1536u * 1024u * 1024u)
#define STORE_WEB_ORIGIN "https://switch2homebrew.com"

typedef struct {
    char english[192];
    char spanish[192];
} StoreInstallError;

__attribute__((format(printf, 2, 4), format(printf, 3, 4)))
bool store_install_error(StoreInstallError *error, const char *english, const char *spanish, ...);

bool download_asset(FILE *out, const char *url, uint64_t expected_size, atomic_uint *downloaded,
                    atomic_uint *total, const atomic_bool *cancel, StoreInstallError *error);
bool download_package_url_allowed(const char *url);
bool download_store_resource(FILE *out, const char *path, unsigned limit, const atomic_bool *cancel,
                             int *http_status, StoreInstallError *error);
