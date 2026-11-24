#pragma once

#include <stdbool.h>

static inline const char *store_text(bool spanish, const char *english, const char *translated)
{
    return spanish ? translated : english;
}
