#ifndef SWITCH2HOMEBREW_STORE_UI_H
#define SWITCH2HOMEBREW_STORE_UI_H

#include "catalog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STORE_WIDTH = 1280, STORE_HEIGHT = 720, STORE_COLUMNS = 4, STORE_CARDS_PER_PAGE = 8 };

typedef enum { STORE_GRID, STORE_DETAILS, STORE_CONFIRM, STORE_HELP } StoreScreen;
typedef enum { STORE_NORMAL, STORE_SUCCESS, STORE_ERROR } StoreTone;
typedef enum {
    HIT_NONE, HIT_CARD, HIT_CATEGORY, HIT_PREVIOUS, HIT_NEXT, HIT_MUSIC, HIT_THEME, HIT_HELP,
    HIT_LANGUAGE, HIT_BACK, HIT_INSTALL, HIT_CONFIRM, HIT_CANCEL_DOWNLOAD, HIT_EXIT, HIT_REFRESH
} StoreHitKind;

typedef struct {
    StoreHitKind kind;
    unsigned index;
} StoreHit;

typedef struct {
    StoreScreen screen;
    StoreCategory category;
    bool dark;
    bool spanish;
    bool music;
    bool music_available;
    bool busy;
    bool can_cancel;
    bool refreshing;
    size_t page, pages, filter_count, focus, visible_count;
    const StoreCatalogEntry *cards[STORE_CARDS_PER_PAGE];
    const StoreCatalogEntry *details;
    const StoreCatalogEntry *job_entry;
    const char *status;
    const char *status_detail;
    const char *refresh_status;
    StoreTone tone;
    int progress;
    uint32_t elapsed_ms;
} StoreView;

void store_draw(uint32_t *pixels, size_t stride, const StoreView *view);
StoreHit store_hit_test(const StoreView *view, int x, int y);

#endif
