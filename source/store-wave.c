#include "store-wave.h"
#include "store-ui.h"

#include <math.h>

#define WAVE_COLOR(r, g, b) (UINT32_C(0xff000000) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (r))

typedef struct {
    uint32_t sky_top, sky_bottom;
    uint32_t distant_top, distant_bottom;
    uint32_t water_top, water_bottom;
} WaveColors;

static const WaveColors light = {
    WAVE_COLOR(232, 238, 246), WAVE_COLOR(211, 225, 239),
    WAVE_COLOR(211, 226, 242), WAVE_COLOR(201, 218, 234),
    WAVE_COLOR(186, 216, 234), WAVE_COLOR(199, 222, 237),
};

static const WaveColors dark_colors = {
    WAVE_COLOR(39, 51, 75), WAVE_COLOR(29, 46, 67),
    WAVE_COLOR(51, 69, 94), WAVE_COLOR(44, 69, 95),
    WAVE_COLOR(56, 87, 112), WAVE_COLOR(40, 66, 89),
};

enum {
    COORD_SCALE = 16,
    NEAR_SOFTNESS = 2 * COORD_SCALE,
    FAR_SOFTNESS = 4 * COORD_SCALE,
};

typedef struct {
    int16_t distant, near;
} Crest;

static uint32_t mix(uint32_t back, uint32_t front, unsigned coverage)
{
    const unsigned rest = 255 - coverage;
    const unsigned red = ((back & 255u) * rest + (front & 255u) * coverage + 127) / 255;
    const unsigned green = (((back >> 8) & 255u) * rest + ((front >> 8) & 255u) * coverage + 127) / 255;
    const unsigned blue = (((back >> 16) & 255u) * rest + ((front >> 16) & 255u) * coverage + 127) / 255;
    return WAVE_COLOR(red, green, blue);
}

static double ease(double position)
{
    if (position <= 0.0)
        return 0.0;
    if (position >= 1.0)
        return 1.0;
    return position * position * (3.0 - 2.0 * position);
}

static unsigned edge_coverage(int distance, int softness)
{
    if (distance <= -softness)
        return 0;
    if (distance >= softness)
        return 255;
    return (unsigned)((distance + softness) * 255 / (softness * 2));
}

void store_wave_draw(uint32_t *pixels, size_t stride, bool dark, uint32_t elapsed_ms)
{
    if (!pixels || stride < STORE_WIDTH * sizeof(*pixels) || stride % sizeof(*pixels) != 0)
        return;

    const WaveColors *colors = dark ? &dark_colors : &light;
    const double period = 6.28318530717958647693;
    const double phase = period * (elapsed_ms % STORE_WAVE_PERIOD_MS) / STORE_WAVE_PERIOD_MS;
    const double lateral_shift = 92.0 * sin(phase + 0.42);
    Crest crests[STORE_WIDTH];
    int topmost = STORE_HEIGHT * COORD_SCALE, bottommost = 0;

    for (int x = 0; x < STORE_WIDTH; ++x) {
        const double across = (double)x / (STORE_WIDTH - 1);
        const double rise = ease(((double)x + lateral_shift - 116.0) / 1164.0);
        const double surface = 645.0 - 169.0 * rise
                             + 15.5 * sin(phase - across * 4.6)
                             + 5.0 * sin(2.0 * phase - across * 6.0 + 0.58);
        const double distant = surface - 33.0 - 11.5 * sin(phase - across * 2.6 + 1.05);
        crests[x] = (Crest){(int16_t)(distant * COORD_SCALE), (int16_t)(surface * COORD_SCALE)};
        if (crests[x].distant < topmost)
            topmost = crests[x].distant;
        if (crests[x].near > bottommost)
            bottommost = crests[x].near;
    }

    for (int y = 0; y < STORE_HEIGHT; ++y) {
        uint32_t *output = (uint32_t *)((unsigned char *)pixels + (size_t)y * stride);
        const int row = y * COORD_SCALE;
        const unsigned depth = (unsigned)(255 * y / (STORE_HEIGHT - 1));
        const uint32_t sky = mix(colors->sky_top, colors->sky_bottom, depth);
        if (row < topmost - FAR_SOFTNESS) {
            for (int x = 0; x < STORE_WIDTH; ++x)
                output[x] = sky;
            continue;
        }

        const unsigned water_depth = y > 407 ? (unsigned)((y - 407) * 255 / (STORE_HEIGHT - 408)) : 0;
        const uint32_t water = mix(colors->water_top, colors->water_bottom, water_depth);
        if (row >= bottommost + NEAR_SOFTNESS) {
            for (int x = 0; x < STORE_WIDTH; ++x)
                output[x] = water;
            continue;
        }

        const uint32_t distant_water = mix(colors->distant_top, colors->distant_bottom, water_depth);
        for (int x = 0; x < STORE_WIDTH; ++x) {
            const unsigned near = edge_coverage(row - crests[x].near, NEAR_SOFTNESS);
            if (near == 255) {
                output[x] = water;
                continue;
            }
            const unsigned far = edge_coverage(row - crests[x].distant, FAR_SOFTNESS);
            const uint32_t backdrop = far == 255 ? distant_water : far ? mix(sky, distant_water, far) : sky;
            output[x] = near ? mix(backdrop, water, near) : backdrop;
        }
    }
}
