#pragma once

#include <stdatomic.h>

unsigned char *store_cover_fetch(const char *url, const atomic_bool *cancel);
