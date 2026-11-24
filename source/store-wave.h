#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STORE_WAVE_PERIOD_MS = 26000 };

void store_wave_draw(uint32_t *pixels, size_t stride, bool dark, uint32_t elapsed_ms);
