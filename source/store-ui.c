#include "store-ui.h"
#include "qrcodegen.h"
#include "store-font.h"
#include "store-language.h"
#include "store-wave.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char _binary_store_logo_rgb_start[];

#define RGB(r, g, b) (UINT32_C(0xff000000) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (r))

typedef struct {
    uint32_t *pixels;
    size_t stride;
} Surface;

typedef struct {
    const StoreGlyph *glyphs;
    const unsigned char *pixels;
    int height;
} Font;

typedef struct {
    int x, y, width, height;
} Rect;

typedef struct {
    uint32_t footer;
    uint32_t glass_top, glass_bottom, trim, shadow, inset;
    uint32_t ink, muted, accent, halo, accent_surface, accent_ink;
    uint32_t warning_surface, warning_ink, error_ink, success_ink;
} Palette;

static const Font small = {store_small_glyphs, store_small_pixels, STORE_SMALL_HEIGHT};
static const Font large = {store_large_glyphs, store_large_pixels, STORE_LARGE_HEIGHT};

static const Palette day = {
    .footer = RGB(225, 235, 248),
    .glass_top = RGB(249, 252, 255), .glass_bottom = RGB(232, 241, 252),
    .trim = RGB(168, 193, 219), .shadow = RGB(104, 141, 179), .inset = RGB(213, 228, 245),
    .ink = RGB(26, 48, 76), .muted = RGB(54, 79, 113), .accent = RGB(45, 110, 173),
    .halo = RGB(166, 209, 246), .accent_surface = RGB(202, 226, 251), .accent_ink = RGB(22, 66, 120),
    .warning_surface = RGB(255, 246, 218), .warning_ink = RGB(121, 76, 16),
    .error_ink = RGB(155, 43, 65), .success_ink = RGB(12, 103, 82),
};

static const Palette night = {
    .footer = RGB(26, 42, 61),
    .glass_top = RGB(46, 65, 90), .glass_bottom = RGB(29, 45, 65),
    .trim = RGB(77, 109, 143), .shadow = RGB(8, 16, 28), .inset = RGB(25, 43, 65),
    .ink = RGB(241, 249, 255), .muted = RGB(189, 216, 239), .accent = RGB(133, 204, 255),
    .halo = RGB(56, 111, 155), .accent_surface = RGB(49, 91, 130), .accent_ink = RGB(233, 248, 255),
    .warning_surface = RGB(67, 56, 41), .warning_ink = RGB(255, 216, 145),
    .error_ink = RGB(255, 160, 171), .success_ink = RGB(148, 236, 209),
};

static const char *const categories[2][STORE_CATEGORY_COUNT] = {
    {"All", "Emulators", "Ports", "Media", "Utilities", "Development"},
    {"Todo", "Emuladores", "Ports", "Multimedia", "Utilidades", "Desarrollo"},
};

static const Rect tabs[] = {
    {56, 99, 100, 49}, {167, 99, 155, 49}, {333, 99, 100, 49},
    {444, 99, 149, 49}, {604, 99, 132, 49}, {747, 99, 140, 49},
};

static const Rect help_with_music = {707, 20, 99, 51};
static const Rect help_without_music = {847, 20, 99, 51};
static const Rect refresh_button = {539, 20, 157, 51};
static const Rect music_button = {816, 20, 130, 51};
static const Rect theme_button = {956, 20, 135, 51};
static const Rect language_button = {1101, 20, 123, 51};
static const Rect previous_button = {56, 584, 63, 55};
static const Rect next_button = {277, 584, 63, 55};
static const Rect exit_button = {1068, 653, 156, 52};
static const Rect back_button = {56, 653, 164, 52};
static const Rect install_button = {976, 653, 248, 52};
static const Rect cancel_download_button = {1068, 586, 145, 51};
static const Rect confirm_back_button = {157, 562, 186, 52};
static const Rect confirm_install_button = {885, 562, 230, 52};

static Rect help_button(const StoreView *view)
{
    return view->music_available ? help_with_music : help_without_music;
}

static bool can_refresh(const StoreView *view)
{
    return view->screen == STORE_GRID && !view->refreshing && !view->busy;
}

static Rect card_rect(unsigned index)
{
    return (Rect){56 + (int)(index % STORE_COLUMNS) * 299, 164 + (int)(index / STORE_COLUMNS) * 209, 271, 195};
}

static bool contains(Rect rect, int x, int y)
{
    return x >= rect.x && x - rect.x < rect.width && y >= rect.y && y - rect.y < rect.height;
}

static uint32_t blend(uint32_t old, uint32_t color, unsigned alpha)
{
    const unsigned background = 255u - alpha;
    const unsigned red = ((old & 255u) * background + (color & 255u) * alpha + 127u) / 255u;
    const unsigned green = (((old >> 8) & 255u) * background + ((color >> 8) & 255u) * alpha + 127u) / 255u;
    const unsigned blue = (((old >> 16) & 255u) * background + ((color >> 16) & 255u) * alpha + 127u) / 255u;
    return RGB(red, green, blue);
}

static uint32_t *row(const Surface *surface, int y)
{
    return (uint32_t *)((unsigned char *)surface->pixels + (size_t)y * surface->stride);
}

static unsigned round_coverage(int x, int y, Rect rect, int radius)
{
    if (x < rect.x || y < rect.y || x >= rect.x + rect.width || y >= rect.y + rect.height)
        return 0;
    int dx = rect.x + radius - x;
    if (dx < 0)
        dx = x - (rect.x + rect.width - radius - 1);
    int dy = rect.y + radius - y;
    if (dy < 0)
        dy = y - (rect.y + rect.height - radius - 1);
    if (dx <= 0 || dy <= 0)
        return 255;
    const int distance = 4 * (dx * dx + dy * dy);
    const int inner = (radius * 2 - 1) * (radius * 2 - 1);
    const int outer = (radius * 2 + 1) * (radius * 2 + 1);
    if (distance <= inner)
        return 255;
    if (distance >= outer)
        return 0;
    return (unsigned)((outer - distance) * 255 / (outer - inner));
}

static Rect inset(Rect rect, int px)
{
    return (Rect){rect.x + px, rect.y + px, rect.width - 2 * px, rect.height - 2 * px};
}

static void rounded(const Surface *surface, Rect rect, int radius, int stroke, uint32_t color)
{
    const Rect inner = inset(rect, stroke);
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        uint32_t *line = row(surface, y);
        for (int x = rect.x; x < rect.x + rect.width; ++x) {
            unsigned coverage = round_coverage(x, y, rect, radius);
            if (stroke && coverage)
                coverage = coverage * (255 - round_coverage(x, y, inner, radius - stroke)) / 255;
            if (coverage == 255)
                line[x] = color;
            else if (coverage)
                line[x] = blend(line[x], color, coverage);
        }
    }
}

static void rectangle(const Surface *surface, Rect rect, uint32_t color)
{
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        uint32_t *line = row(surface, y);
        for (int x = rect.x; x < rect.x + rect.width; ++x)
            line[x] = color;
    }
}

static void gradient(const Surface *surface, Rect rect, int radius, uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < rect.height; ++j) {
        const uint32_t color = blend(top, bottom, (unsigned)(j * 255 / rect.height));
        const int y = rect.y + j;
        uint32_t *line = row(surface, y);
        for (int x = rect.x; x < rect.x + rect.width; ++x) {
            const unsigned coverage = radius ? round_coverage(x, y, rect, radius) : 255u;
            if (coverage == 255)
                line[x] = color;
            else if (coverage)
                line[x] = blend(line[x], color, coverage);
        }
    }
}

static void panel(const Surface *surface, const Palette *p, Rect rect, int radius)
{
    const Rect shadow = {rect.x, rect.y + 4, rect.width, rect.height};
    rounded(surface, shadow, radius, 0, blend(p->footer, p->shadow, 75));
    gradient(surface, rect, radius, p->glass_top, p->glass_bottom);
    rounded(surface, rect, radius, 1, p->trim);
}

static unsigned glyph_index(const unsigned char **pointer)
{
    const unsigned char *text = *pointer;
    unsigned code = *text++;
    if ((code & 0xe0) == 0xc0 && (*text & 0xc0) == 0x80) {
        code = ((code & 0x1f) << 6) | (*text++ & 0x3f);
    } else if (code >= 0x80) {
        while ((*text & 0xc0) == 0x80)
            ++text;
        code = '?';
    }
    *pointer = text;

    static const unsigned accents[] = {
        0xc1, 0xc9, 0xcd, 0xd3, 0xda, 0xdc, 0xd1,
        0xe1, 0xe9, 0xed, 0xf3, 0xfa, 0xfc, 0xf1, 0xbf, 0xa1,
    };
    if (code >= 32 && code <= 126)
        return code - 32;
    for (unsigned index = 0; index < sizeof accents / sizeof accents[0]; ++index) {
        if (accents[index] == code)
            return 95 + index;
    }
    return '?' - 32;
}

static int span_width(const Font *font, const unsigned char *begin, const unsigned char *end)
{
    int width = 0;
    while (begin < end)
        width += font->glyphs[glyph_index(&begin)].advance;
    return width;
}

static int text_width(const Font *font, const char *value)
{
    const unsigned char *begin = (const unsigned char *)value;
    return value ? span_width(font, begin, begin + strlen(value)) : 0;
}

static void draw_span(const Surface *surface, const Font *font, int x, int y, int right,
                      uint32_t color, const unsigned char *text, const unsigned char *end)
{
    while (text < end) {
        const StoreGlyph *glyph = &font->glyphs[glyph_index(&text)];
        if (x >= right)
            return;
        const int width = x + glyph->width <= right ? glyph->width : right - x;
        const unsigned char *alpha = font->pixels + glyph->offset;
        for (int j = 0; j < font->height; ++j) {
            uint32_t *line = row(surface, y + j) + x;
            const unsigned char *mask = alpha + (size_t)j * glyph->width;
            for (int i = 0; i < width; ++i) {
                if (mask[i] != 0)
                    line[i] = blend(line[i], color, mask[i]);
            }
        }
        x += glyph->advance;
    }
}

static void draw_fit(const Surface *surface, const Font *font, int x, int y, int right,
                     uint32_t color, const char *value)
{
    if (!value || right <= x)
        return;
    const unsigned char *start = (const unsigned char *)value;
    const unsigned char *end = start + strlen(value);
    if (span_width(font, start, end) <= right - x) {
        draw_span(surface, font, x, y, right, color, start, end);
        return;
    }
    static const unsigned char dots[] = "...";
    const int available = right - x - span_width(font, dots, dots + 3);
    int width = 0;
    const unsigned char *point = start;
    while (point < end) {
        const unsigned char *next = point;
        const int glyph = font->glyphs[glyph_index(&next)].advance;
        if (width + glyph > available)
            break;
        width += glyph;
        point = next;
    }
    draw_span(surface, font, x, y, right, color, start, point);
    draw_span(surface, font, x + width, y, right, color, dots, dots + 3);
}

static unsigned draw_wrap(const Surface *surface, const Font *font, int x, int y, int right,
                          int line_height, unsigned rows, uint32_t color, const char *value)
{
    if (!value || right <= x)
        return 0;
    const unsigned char *start = (const unsigned char *)value;
    unsigned line = 0;
    while (*start && line < rows) {
        while (*start == ' ' || *start == '\n')
            ++start;
        if (!*start)
            break;
        const unsigned char *point = start, *space = NULL;
        int width = 0;
        while (*point && *point != '\n') {
            const unsigned char *next = point;
            const int glyph = font->glyphs[glyph_index(&next)].advance;
            if (width + glyph > right - x)
                break;
            if (*point == ' ')
                space = point;
            width += glyph;
            point = next;
        }
        if (point == start)
            (void)glyph_index(&point);
        if (*point && *point != '\n' && space && space > start)
            point = space;
        if (line + 1 == rows && *point) {
            if (surface)
                draw_fit(surface, font, x, y, right, color, (const char *)start);
            return line + 1;
        }
        if (surface)
            draw_span(surface, font, x, y, right, color, start, point);
        y += line_height;
        ++line;
        start = point;
    }
    return line;
}

static void center(const Surface *surface, const Font *font, Rect rect, uint32_t color, const char *value)
{
    const int width = text_width(font, value);
    const int x = width >= rect.width ? rect.x : rect.x + (rect.width - width) / 2;
    draw_fit(surface, font, x, rect.y + (rect.height - font->height) / 2,
             rect.x + rect.width, color, value);
}

static void button(const Surface *surface, const Palette *p, Rect rect, const char *label,
                   bool selected, bool enabled)
{
    const uint32_t top = selected ? p->accent_surface : p->glass_top;
    const uint32_t bottom = selected ? blend(p->accent_surface, p->accent, 30) : p->glass_bottom;
    if (enabled) {
        gradient(surface, rect, 17, top, bottom);
        rounded(surface, rect, 17, selected ? 2 : 1, selected ? p->accent : p->trim);
    } else {
        rounded(surface, rect, 17, 0, p->footer);
        rounded(surface, rect, 17, 1, p->trim);
    }
    const uint32_t ink = enabled ? selected ? p->accent_ink : p->ink : p->muted;
    center(surface, &small, inset(rect, 4), ink, label);
}

static void label_chip(const Surface *surface, Rect rect, uint32_t background, uint32_t ink,
                       const char *label)
{
    rounded(surface, rect, 11, 0, background);
    center(surface, &small, rect, ink, label);
}

static void background(const Surface *surface, const Palette *p, const StoreView *view)
{
    store_wave_draw(surface->pixels, surface->stride, view->dark, view->elapsed_ms);
    rectangle(surface, (Rect){0, 646, STORE_WIDTH, 74}, p->footer);
    rectangle(surface, (Rect){0, 645, STORE_WIDTH, 1}, p->trim);
    rectangle(surface, (Rect){56, 91, 1168, 1}, p->trim);
}

static void artwork(const Surface *surface, Rect rect, int radius, const unsigned char *asset,
                    int source_width, int source_height)
{
    for (int dy = 0; dy < rect.height; ++dy) {
        const int y = rect.y + dy;
        const unsigned char *source = asset + (size_t)(dy * source_height / rect.height) * source_width * 3;
        uint32_t *output = row(surface, y) + rect.x;
        for (int dx = 0; dx < rect.width; ++dx) {
            const unsigned coverage = round_coverage(rect.x + dx, y, rect, radius);
            if (!coverage)
                continue;
            const unsigned char *pixel = source + (size_t)(dx * source_width / rect.width) * 3;
            const uint32_t color = RGB(pixel[0], pixel[1], pixel[2]);
            output[dx] = coverage == 255 ? color : blend(output[dx], color, coverage);
        }
    }
}

static void header(const Surface *surface, const Palette *p, const StoreView *view)
{
    artwork(surface, (Rect){56, 10, STORE_LOGO_SIZE, STORE_LOGO_SIZE}, 15,
            _binary_store_logo_rgb_start, STORE_LOGO_SIZE, STORE_LOGO_SIZE);
    draw_fit(surface, &large, 144, 11, refresh_button.x - 12, p->ink, "Switch2Homebrew");
    draw_fit(surface, &small, 145, 49, refresh_button.x - 12, p->muted, store_text(view->spanish, "Homebrew Store", "Tienda de homebrew"));
    if (view->screen == STORE_GRID)
        button(surface, p, refresh_button, store_text(view->spanish, "Refresh", "Actualizar"), false, can_refresh(view));
    button(surface, p, help_button(view), store_text(view->spanish, "Help", "Ayuda"), view->screen == STORE_HELP, true);
    if (view->music_available) {
        const char *label = view->music ? store_text(view->spanish, "Music: on", "Música: sí") :
            store_text(view->spanish, "Music: off", "Música: no");
        button(surface, p, music_button, label, view->music, true);
    }
    const char *theme = view->dark ? store_text(view->spanish, "Dark theme", "Tema oscuro") :
        store_text(view->spanish, "Light theme", "Tema claro");
    button(surface, p, theme_button, theme, view->dark, true);
    button(surface, p, language_button, store_text(view->spanish, "English", "Español"), false, true);
}

static void cover(const Surface *surface, const Palette *p, Rect rect, const StoreCatalogEntry *entry)
{
    const unsigned char *asset = store_cover_pixels(entry);
    if (!asset) {
        rounded(surface, rect, 13, 0, p->inset);
        return;
    }
    artwork(surface, rect, 13, asset, STORE_COVER_WIDTH, STORE_COVER_HEIGHT);
}

static const char *entry_status(const StoreCatalogEntry *entry, bool spanish)
{
    if (entry->editing)
        return store_text(spanish, "Editing", "En edición");
    if (entry->package.kind == STORE_PACKAGE_NONE)
        return "Manual";
    return store_text(spanish, "Installable", "Instalable");
}

static void grid_card(const Surface *surface, const Palette *p, Rect rect,
                      const StoreCatalogEntry *entry, bool focused, bool spanish)
{
    if (focused) {
        rounded(surface, inset(rect, -4), 23, 0, p->halo);
        rounded(surface, inset(rect, -2), 21, 3, p->accent);
    }
    panel(surface, p, rect, 20);
    cover(surface, p, (Rect){rect.x + 7, rect.y + 7, 256, 144}, entry);

    const char *label = entry_status(entry, spanish);
    const bool installable = entry->package.kind != STORE_PACKAGE_NONE;
    const uint32_t status_background = entry->editing ? p->warning_surface : installable ? p->accent_surface : p->glass_bottom;
    const uint32_t status_ink = entry->editing ? p->warning_ink : installable ? p->accent_ink : p->muted;
    label_chip(surface, (Rect){rect.x + 14, rect.y + 14, text_width(&small, label) + 20, 34},
               status_background, status_ink, label);
    if (entry->requirement && *entry->requirement) {
        label_chip(surface, (Rect){rect.x + rect.width - 47, rect.y + 14, 33, 34},
                   p->warning_surface, p->warning_ink, "!");
    }
    draw_fit(surface, &small, rect.x + 13, rect.y + 155, rect.x + rect.width - 13, p->ink, entry->name);
}

static void page_arrow(const Surface *surface, const Palette *p, Rect rect, bool next, bool enabled)
{
    button(surface, p, rect, "", false, enabled);
    const int center_x = rect.x + rect.width / 2;
    const int center_y = rect.y + rect.height / 2;
    const uint32_t ink = enabled ? p->accent_ink : p->trim;
    for (int y = -9; y <= 9; ++y) {
        const int width = y < 0 ? 9 + y : 9 - y;
        const int x = next ? center_x - 5 + width : center_x + 2 - width;
        rectangle(surface, (Rect){x, center_y + y, 4, 1}, ink);
    }
}

static bool previous_page(const StoreView *view)
{
    return view->pages > 1 && view->page > 0;
}

static bool next_page(const StoreView *view)
{
    return view->pages > 1 && view->page < view->pages - 1;
}

static void grid(const Surface *surface, const Palette *p, const StoreView *view)
{
    for (unsigned category = 0; category < STORE_CATEGORY_COUNT; ++category) {
        if (store_catalog_has_category((StoreCategory)category))
            button(surface, p, tabs[category], categories[view->spanish][category],
                   view->category == (StoreCategory)category, true);
    }
    char count[44];
    const char *count_format = view->filter_count == 1 ? store_text(view->spanish, "%zu project", "%zu proyecto") :
        store_text(view->spanish, "%zu projects", "%zu proyectos");
    snprintf(count, sizeof count, count_format, view->filter_count);
    draw_fit(surface, &small, 1221 - text_width(&small, count), 107, 1224, p->muted, count);

    for (unsigned i = 0; i < STORE_CARDS_PER_PAGE && i < view->visible_count; ++i) {
        if (view->cards[i])
            grid_card(surface, p, card_rect(i), view->cards[i], i == view->focus, view->spanish);
    }
    if (!view->visible_count) {
        panel(surface, p, (Rect){56, 165, 1168, 398}, 22);
        center(surface, &large, (Rect){65, 263, 1150, 60}, p->ink,
               store_text(view->spanish, "No projects in this category", "No hay proyectos en esta categoría"));
        center(surface, &small, (Rect){65, 320, 1150, 44}, p->muted,
               store_text(view->spanish, "Choose another category to keep browsing.", "Elige otra categoría para seguir explorando."));
    }

    page_arrow(surface, p, previous_button, false, previous_page(view));
    page_arrow(surface, p, next_button, true, next_page(view));
    char page[56];
    snprintf(page, sizeof page, store_text(view->spanish, "%zu of %zu", "%zu de %zu"),
             view->pages ? view->page + 1 : 0, view->pages);
    center(surface, &small, (Rect){125, 584, 144, 55}, p->ink, page);
}

static const char *requirement(const StoreCatalogEntry *entry, bool spanish)
{
    const char *rules = store_text(spanish, entry->requirement, entry->requirement_es);
    return rules && *rules ? rules : store_text(spanish, "Check the project page before adding extra files.",
        "Consulta la ficha del proyecto antes de añadir recursos adicionales.");
}

static bool can_install(const StoreView *view)
{
    return view->details && !view->details->editing && view->details->package.kind != STORE_PACKAGE_NONE &&
           !view->busy && !view->refreshing;
}

static void package_format(const StorePackage *package, char *label, size_t capacity, bool spanish)
{
    const char *type = package->kind == STORE_PACKAGE_ZIP ? "ZIP" : "NRO";
    if (package->size) {
        const unsigned tenths_mib = (unsigned)(package->size * 10 / (1024u * 1024u));
        if (tenths_mib)
            snprintf(label, capacity, "%s  |  %u%c%u MiB", type, tenths_mib / 10, spanish ? ',' : '.', tenths_mib % 10);
        else
            snprintf(label, capacity, "%s  |  %u KiB", type, (unsigned)((package->size + 1023) / 1024));
    } else {
        snprintf(label, capacity, "%s", type);
    }
}

static void warning(const Surface *surface, const Palette *p, const StoreCatalogEntry *entry, Rect rect, bool spanish)
{
    const char *data_rules = store_text(spanish,
        "This store carries homebrew. Use your own original games, BIOS files or other data when required.",
        "La tienda distribuye solo homebrew. Cuando corresponda, aporta tus ROM, BIOS o archivos originales.");
    const int left = rect.x + 20, right = rect.x + rect.width - 21;
    const int notice_rows = (int)draw_wrap(NULL, &small, left, 0, right, 25, 2, p->warning_ink, data_rules);
    const unsigned capacity = (unsigned)((rect.height - 71 - notice_rows * 25) / 25);
    const char *rules = requirement(entry, spanish);
    const unsigned rule_rows = draw_wrap(NULL, &small, left, 0, right, 25, capacity, p->ink, rules);
    const int notice_y = rect.y + 49 + (int)rule_rows * 25;
    rect.height = 67 + ((int)rule_rows + notice_rows) * 25;
    rounded(surface, rect, 18, 0, p->warning_surface);
    label_chip(surface, (Rect){rect.x + 18, rect.y + 9, 31, 33}, p->warning_ink, p->warning_surface, "!");
    draw_fit(surface, &small, rect.x + 61, rect.y + 7, rect.x + rect.width - 20, p->warning_ink,
             store_text(spanish, "Project requirements", "Requisitos del proyecto"));
    draw_wrap(surface, &small, left, rect.y + 43, right, 25, capacity, p->ink, rules);
    draw_wrap(surface, &small, left, notice_y, right, 25, 2, p->warning_ink, data_rules);
}

enum { QR_MAX_URL_BYTES = 300, QR_MAX_VERSION = 13, QR_QUIET_ZONE = 4, QR_AREA = 231 };

typedef struct {
    char url[QR_MAX_URL_BYTES + 1];
    uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    int modules;
} QrCache;

static bool link_qr(const Surface *surface, Rect area, QrCache *cache, const char *url)
{
    if (!url || !*url)
        return false;
    const size_t length = strlen(url);
    if (length > QR_MAX_URL_BYTES)
        return false;
    if (strcmp(cache->url, url)) {
        uint8_t workspace[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
        memcpy(cache->url, url, length + 1);
        const bool encoded = qrcodegen_encodeText(url, workspace, cache->code, qrcodegen_Ecc_MEDIUM,
            qrcodegen_VERSION_MIN, QR_MAX_VERSION, qrcodegen_Mask_AUTO, true);
        cache->modules = encoded ? qrcodegen_getSize(cache->code) : 0;
    }
    const int modules = cache->modules;
    if (!modules)
        return false;

    const int scale = area.width / (modules + 2 * QR_QUIET_ZONE);
    const int size = (modules + 2 * QR_QUIET_ZONE) * scale;
    const int left = area.x + (area.width - size) / 2;
    const int top = area.y + (area.height - size) / 2;
    const int margin = QR_QUIET_ZONE * scale;
    rectangle(surface, (Rect){left, top, size, size}, RGB(255, 255, 255));
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (qrcodegen_getModule(cache->code, x, y))
                rectangle(surface, (Rect){left + margin + x * scale, top + margin + y * scale, scale, scale},
                          RGB(13, 22, 35));
        }
    }
    return true;
}

static void project_links(const Surface *surface, const Palette *p,
                          const StoreCatalogEntry *entry, const char *source_url, bool spanish)
{
    static QrCache website, source;
    const bool manual = entry->package.kind == STORE_PACKAGE_NONE;
    const Rect website_rect = {743, 154, QR_AREA, QR_AREA};
    const Rect source_rect = {manual ? 985 : 864, 154, QR_AREA, QR_AREA};
    panel(surface, p, (Rect){735, 103, 489, 467}, 20);
    center(surface, &small, (Rect){source_rect.x, 112, source_rect.width, 36}, p->ink,
           store_text(spanish, "Official source", "Fuente oficial"));
    const char *missing = store_text(spanish, "Link unavailable", "Enlace no disponible");
    bool have_website = false;
    if (manual) {
        center(surface, &small, (Rect){website_rect.x, 112, website_rect.width, 36}, p->ink, "Switch2Homebrew");
        have_website = link_qr(surface, website_rect, &website, entry->project_url);
        if (!have_website)
            center(surface, &small, website_rect, p->muted, missing);
    }
    const bool have_source = link_qr(surface, source_rect, &source, source_url);
    if (!have_source)
        center(surface, &small, source_rect, p->muted, missing);
    if (have_website || have_source)
        center(surface, &small, (Rect){751, 385, 457, 34}, p->accent,
               store_text(spanish, "Scan with your phone", "Escanea con el móvil"));

    draw_fit(surface, &large, 756, 421, 1202, p->ink,
             manual ? store_text(spanish, "Project information", "Información del proyecto") :
             store_text(spanish, "Manual installation", "Instalación manual"));
    const char *steps;
    if (entry->editing) {
        steps = store_text(spanish,
            "This project is temporarily being edited. Check the official source for the latest instructions.",
            "Este proyecto está temporalmente en edición. Consulta la fuente oficial para ver las últimas instrucciones.");
    } else if (entry->package.kind == STORE_PACKAGE_NRO) {
        steps = store_text(spanish,
            "Get the NRO from the official source and copy it to the microSD path shown on the left.",
            "Descarga el NRO de la fuente oficial y cópialo a la ruta de la microSD que se indica a la izquierda.");
    } else if (entry->package.kind == STORE_PACKAGE_ZIP) {
        steps = store_text(spanish,
            "Download and extract the author's ZIP. Follow the official instructions to copy the files to the microSD.",
            "Descarga y descomprime el ZIP del autor. Sigue las instrucciones oficiales para copiar los archivos a la microSD.");
    } else {
        steps = store_text(spanish,
            "Direct download is unavailable. See the project page for requirements and official instructions.",
            "Sin descarga directa. Consulta la ficha web para ver los requisitos y las instrucciones oficiales.");
    }
    draw_wrap(surface, &small, 756, 466, 1202, 25, 4, p->muted, steps);
}

static void details(const Surface *surface, const Palette *p, const StoreView *view)
{
    if (!view->details)
        return;
    const StoreCatalogEntry *entry = view->details;
    const StorePackage *package = &entry->package;
    const bool installable = !entry->editing && package->kind != STORE_PACKAGE_NONE;
    panel(surface, p, (Rect){56, 103, 666, 228}, 21);
    cover(surface, p, (Rect){68, 115, 224, 126}, entry);

    const int detail_left = 310, detail_right = 703;
    const unsigned title_rows = draw_wrap(surface, &large, detail_left, 106, detail_right, 36, 2, p->ink, entry->name);
    const int line_y = 117 + (int)title_rows * 34;
    char tag[180];
    const char *category = (unsigned)entry->category < STORE_CATEGORY_COUNT ? categories[view->spanish][entry->category] : "Homebrew";
    if (installable) {
        char format[48];
        package_format(package, format, sizeof format, view->spanish);
        snprintf(tag, sizeof tag, store_text(view->spanish, "%s  |  Version %s  |  %s", "%s  |  Versión %s  |  %s"),
                 category, package->version, format);
        if (text_width(&small, tag) + 22 > detail_right - detail_left)
            snprintf(tag, sizeof tag, "%s | %s | %s", category, package->version, format);
        if (text_width(&small, tag) + 22 > detail_right - detail_left)
            snprintf(tag, sizeof tag, store_text(view->spanish, "Version %s | %s", "Versión %s | %s"),
                     package->version, format);
    } else {
        snprintf(tag, sizeof tag, "%s  |  %s", category, entry_status(entry, view->spanish));
    }
    const Rect tag_rect = {detail_left, line_y + 1, text_width(&small, tag) + 22, 33};
    const uint32_t tag_surface = entry->editing ? p->warning_surface : installable ? p->accent_surface : p->footer;
    const uint32_t tag_ink = entry->editing ? p->warning_ink : installable ? p->accent_ink : p->muted;
    if (tag_rect.width <= detail_right - detail_left)
        label_chip(surface, tag_rect, tag_surface, tag_ink, tag);
    else
        draw_fit(surface, &small, detail_left, line_y, detail_right, tag_ink, tag);

    const int summary_y = line_y + 40;
    const unsigned summary_lines = (unsigned)((256 - summary_y) / 25 + 1);
    draw_wrap(surface, &small, detail_left, summary_y, detail_right, 25, summary_lines, p->ink,
              store_text(view->spanish, entry->summary, entry->summary_es));
    const char *credit_name = store_cover_credit(entry);
    if (credit_name && *credit_name) {
        char credit[180];
        snprintf(credit, sizeof credit, store_text(view->spanish, "Artwork: %s", "Imagen: %s"), credit_name);
        draw_fit(surface, &small, 69, 242, 296, p->muted, credit);
    }
    if (installable) {
        char target[280];
        snprintf(target, sizeof target, store_text(view->spanish, "Install to %s", "Destino: %s"), package->target);
        draw_fit(surface, &small, 69, 272, detail_right, p->muted, target);
    }
    const char *source_url = store_project_source(entry);
    const char *url = package->kind == STORE_PACKAGE_NONE ? entry->project_url : source_url;
    if (url && !strncmp(url, "https://", 8))
        url += 8;
    draw_fit(surface, &small, 69, 298, detail_right, p->accent, url);
    warning(surface, p, entry, (Rect){56, 343, 666, 227}, view->spanish);
    project_links(surface, p, entry, source_url, view->spanish);
}

static void help(const Surface *surface, const Palette *p, const StoreView *view)
{
    const bool spanish = view->spanish;
    panel(surface, p, (Rect){56, 105, 1168, 460}, 22);
    draw_fit(surface, &large, 82, 120, 1199, p->ink, store_text(spanish, "Store help", "Ayuda de la tienda"));
    const Rect touch = {83, 184, 544, 142};
    const Rect controller = {652, 184, 545, 142};
    rounded(surface, touch, 16, 0, p->inset);
    rounded(surface, controller, 16, 0, p->inset);
    draw_fit(surface, &large, touch.x + 18, touch.y + 8, touch.x + touch.width - 13, p->ink, store_text(spanish, "Touchscreen", "Pantalla táctil"));
    draw_wrap(surface, &small, touch.x + 18, touch.y + 57, touch.x + touch.width - 19, 25, 3, p->muted,
              store_text(spanish, "Tap a channel to open it. Swipe to change pages. Tap Refresh to check the website catalogue.",
                         "Toca un canal para abrirlo. Desliza para cambiar de página. Toca Actualizar para consultar el catálogo web."));
    draw_fit(surface, &large, controller.x + 18, controller.y + 8, controller.x + controller.width - 13, p->ink, store_text(spanish, "Controller", "Mando"));
    draw_wrap(surface, &small, controller.x + 18, controller.y + 57, controller.x + controller.width - 19, 25, 3, p->muted,
              store_text(spanish, "Use the D-pad or stick to move, A to open, B to go back, and L or R to change pages.",
                         "Cruceta o stick para moverte. A abre o confirma y B vuelve. L y R recorren las páginas."));

    draw_fit(surface, &large, 82, 344, 1199, p->ink, store_text(spanish, "Downloads and game data", "Descargas y recursos"));
    draw_wrap(surface, &small, 83, 392, 1176, 27, 3, p->muted,
              store_text(spanish, "Each installable channel shows its version and microSD destination. The store checks the package's SHA-256 before installing.",
                         "Las fichas instalables indican su versión y el destino en la microSD. La tienda comprueba el SHA-256 de cada paquete antes de instalar."));
    draw_wrap(surface, &small, 83, 468, 1176, 27, 3, p->muted,
              store_text(spanish, "Check each project's requirements. Use your own original copies when a port or emulator needs a ROM, BIOS or other files.",
                         "Lee los requisitos de cada ficha. Usa tus propias copias originales si un port o emulador necesita ROM, BIOS u otros datos."));
    draw_fit(surface, &small, 83, 521, 1176, p->muted,
             store_text(spanish, "X changes theme. Y toggles music. L3 refreshes the catalogue. R3 changes language.",
                        "X cambia el tema. Y alterna la música. L3 actualiza el catálogo. R3 cambia el idioma."));
}

static void transfer(const Surface *surface, const Palette *p, const StoreView *view)
{
    const bool is_grid = view->screen == STORE_GRID;
    const bool is_refresh = is_grid && !view->busy && (view->refreshing || view->refresh_status);
    const char *status = is_refresh ? view->refresh_status : view->status;
    const int left = is_grid ? 364 : 56;
    if (!is_refresh && !status && !view->can_cancel) {
        if (is_grid)
            center(surface, &small, (Rect){left, 584, 1224 - left, 55}, p->muted,
                   store_text(view->spanish, "Open a channel for details and requirements.", "Abre un canal para ver su ficha y sus requisitos."));
        return;
    }

    panel(surface, p, (Rect){left, 580, 1224 - left, 62}, 17);
    if (is_refresh && view->refreshing) {
        const Rect bar = {1022, 605, 184, 12};
        const int extent = bar.width - 48;
        const int phase = (int)(view->elapsed_ms / 6u % (unsigned)(extent * 2));
        const int moving = phase <= extent ? phase : extent * 2 - phase;
        rounded(surface, bar, 6, 0, p->inset);
        rounded(surface, (Rect){bar.x + moving, bar.y, 48, bar.height}, 6, 0, p->accent);
        draw_fit(surface, &small, left + 17, 593, bar.x - 18, p->ink, status ? status :
                 store_text(view->spanish, "Checking the website catalogue…", "Consultando el catálogo web…"));
        return;
    }
    const uint32_t color = view->tone == STORE_SUCCESS ? p->success_ink :
        view->tone == STORE_ERROR ? p->error_ink : p->ink;
    if (is_refresh) {
        draw_fit(surface, &small, left + 17, 593, 1206, color, status);
        return;
    }
    const int end = view->can_cancel ? cancel_download_button.x - 13 : 1206;
    const bool has_detail = view->status_detail && *view->status_detail;
    char heading[208];
    snprintf(heading, sizeof heading, "%s%s%s", status ? status : store_text(view->spanish, "Download", "Descarga"),
             view->job_entry ? "  |  " : "", view->job_entry ? view->job_entry->name : "");
    draw_fit(surface, &small, left + 17, has_detail ? 581 : 593, end - (view->progress >= 0 ? 238 : 0), color, heading);
    if (has_detail)
        draw_fit(surface, &small, left + 17, 606, end - (view->progress >= 0 ? 238 : 0), p->muted, view->status_detail);

    if (view->progress >= 0) {
        const unsigned percent = view->progress <= 100 ? (unsigned)view->progress : 100u;
        const Rect bar = {end - 186, 607, 186, 12};
        const int width = (int)(percent * (unsigned)bar.width / 100u);
        rounded(surface, bar, 6, 0, p->inset);
        if (width)
            rounded(surface, (Rect){bar.x, bar.y, width, bar.height}, width < 12 ? width / 2 : 6, 0, p->accent);
        char progress[8];
        snprintf(progress, sizeof progress, "%u%%", percent);
        draw_fit(surface, &small, bar.x - text_width(&small, progress) - 12, 596, bar.x - 8, p->ink, progress);
    }
    if (view->can_cancel)
        button(surface, p, cancel_download_button,
               view->screen == STORE_HELP ? store_text(view->spanish, "Cancel", "Cancelar") :
               store_text(view->spanish, "B  Cancel", "B  Cancelar"), false, true);
}

static void shortcut(const Surface *surface, const Palette *p, int x, const char *key, const char *label)
{
    const int width = text_width(&small, key) + 16;
    label_chip(surface, (Rect){x, 662, width, 33}, p->inset, p->ink, key);
    draw_fit(surface, &small, x + width + 9, 660, 1040, p->muted, label);
}

static void footer(const Surface *surface, const Palette *p, const StoreView *view)
{
    const bool spanish = view->spanish;
    if (view->screen == STORE_GRID) {
        rounded(surface, (Rect){56, 662, 36, 33}, 11, 0, p->inset);
        rounded(surface, (Rect){71, 669, 6, 19}, 2, 0, p->ink);
        rounded(surface, (Rect){65, 675, 18, 6}, 2, 0, p->ink);
        draw_fit(surface, &small, 101, 660, 174, p->muted, store_text(spanish, "Move", "Mover"));
        shortcut(surface, p, 181, "A", store_text(spanish, "Open", "Ficha"));
        shortcut(surface, p, 293, "L/R", store_text(spanish, "Page", "Página"));
        shortcut(surface, p, 446, "ZL/ZR", store_text(spanish, "Filter", "Filtro"));
        shortcut(surface, p, 616, "-", store_text(spanish, "Help", "Ayuda"));
        shortcut(surface, p, 761, "L3", store_text(spanish, "Refresh", "Actualizar"));
        button(surface, p, exit_button, store_text(spanish, "+  Exit", "+  Salir"), false, true);
        return;
    }
    button(surface, p, back_button, view->busy && view->screen == STORE_DETAILS ? store_text(spanish, "Back", "Volver") :
           store_text(spanish, "B  Back", "B  Volver"), false, true);
    if (view->screen == STORE_DETAILS && view->details && !view->details->editing &&
        view->details->package.kind != STORE_PACKAGE_NONE)
        button(surface, p, install_button, view->busy ? store_text(spanish, "Downloading", "Descarga en curso") :
               store_text(spanish, "A  Install", "A  Instalar"), can_install(view), can_install(view));
}

static void confirmation(const Surface *surface, const Palette *p, const StoreView *view)
{
    for (int y = 0; y < STORE_HEIGHT; ++y) {
        uint32_t *line = row(surface, y);
        for (int x = 0; x < STORE_WIDTH; ++x)
            line[x] = blend(line[x], RGB(13, 27, 47), 138);
    }
    panel(surface, p, (Rect){130, 112, 1020, 516}, 23);
    draw_fit(surface, &large, 155, 130, 1114, p->ink, store_text(view->spanish, "Confirm installation", "Confirmar instalación"));
    if (view->details) {
        const StoreCatalogEntry *entry = view->details;
        char title[280], format[48];
        package_format(&entry->package, format, sizeof format, view->spanish);
        snprintf(title, sizeof title, "%s  |  %s  |  %s", entry->name,
                 entry->package.version ? entry->package.version : "", format);
        draw_fit(surface, &small, 155, 175, 1114, p->ink, title);
        char target[280];
        snprintf(target, sizeof target, "microSD: %s", entry->package.target ? entry->package.target : "");
        draw_fit(surface, &small, 155, 206, 1114, p->muted, target);
        warning(surface, p, entry, (Rect){155, 246, 970, 294}, view->spanish);
    }
    button(surface, p, confirm_back_button, store_text(view->spanish, "B  Cancel", "B  Cancelar"), false, true);
    button(surface, p, confirm_install_button, store_text(view->spanish, "A  Install", "A  Instalar"), can_install(view), can_install(view));
}

void store_draw(uint32_t *pixels, size_t stride, const StoreView *view)
{
    if (!pixels || !view || stride < STORE_WIDTH * sizeof(*pixels))
        return;
    const Surface surface = {pixels, stride};
    const Palette *p = view->dark ? &night : &day;
    background(&surface, p, view);
    header(&surface, p, view);
    switch (view->screen) {
        case STORE_GRID: grid(&surface, p, view); break;
        case STORE_DETAILS:
        case STORE_CONFIRM: details(&surface, p, view); break;
        case STORE_HELP: help(&surface, p, view); break;
    }
    transfer(&surface, p, view);
    footer(&surface, p, view);
    if (view->screen == STORE_CONFIRM)
        confirmation(&surface, p, view);
}

StoreHit store_hit_test(const StoreView *view, int x, int y)
{
    if (!view || x < 0 || x >= STORE_WIDTH || y < 0 || y >= STORE_HEIGHT)
        return (StoreHit){HIT_NONE, 0};

    if (view->screen == STORE_CONFIRM) {
        if (contains(confirm_back_button, x, y))
            return (StoreHit){HIT_BACK, 0};
        if (can_install(view) && contains(confirm_install_button, x, y))
            return (StoreHit){HIT_CONFIRM, 0};
        return (StoreHit){HIT_NONE, 0};
    }

    if (view->screen != STORE_HELP && contains(help_button(view), x, y))
        return (StoreHit){HIT_HELP, 0};
    if (view->music_available && contains(music_button, x, y))
        return (StoreHit){HIT_MUSIC, 0};
    if (contains(theme_button, x, y))
        return (StoreHit){HIT_THEME, 0};
    if (contains(language_button, x, y))
        return (StoreHit){HIT_LANGUAGE, 0};
    if (view->can_cancel && contains(cancel_download_button, x, y))
        return (StoreHit){HIT_CANCEL_DOWNLOAD, 0};

    if (view->screen == STORE_GRID) {
        if (can_refresh(view) && contains(refresh_button, x, y))
            return (StoreHit){HIT_REFRESH, 0};
        if (contains(exit_button, x, y))
            return (StoreHit){HIT_EXIT, 0};
        if (previous_page(view) && contains(previous_button, x, y))
            return (StoreHit){HIT_PREVIOUS, 0};
        if (next_page(view) && contains(next_button, x, y))
            return (StoreHit){HIT_NEXT, 0};
        for (unsigned category = 0; category < STORE_CATEGORY_COUNT; ++category) {
            if (contains(tabs[category], x, y) && store_catalog_has_category((StoreCategory)category))
                return (StoreHit){HIT_CATEGORY, category};
        }
        for (unsigned i = 0; i < STORE_CARDS_PER_PAGE && i < view->visible_count; ++i) {
            if (view->cards[i] && contains(card_rect(i), x, y))
                return (StoreHit){HIT_CARD, i};
        }
    } else {
        if (contains(back_button, x, y))
            return (StoreHit){HIT_BACK, 0};
        if (view->screen == STORE_DETAILS && can_install(view) && contains(install_button, x, y))
            return (StoreHit){HIT_INSTALL, 0};
    }
    return (StoreHit){HIT_NONE, 0};
}
