#include "catalog.h"
#include "catalog-sync.h"
#include "installer.h"
#include "store-audio.h"
#include "store-covers.h"
#include "store-language.h"
#include "store-ui.h"
#include "store-wave.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#define PREFERENCE_DIR "sdmc:/switch/switch2homebrew-store"
#define DARK_MARKER PREFERENCE_DIR "/dark.mode"
#define MUTE_MARKER PREFERENCE_DIR "/music.mute"
#define SPANISH_MARKER PREFERENCE_DIR "/spanish.lang"

typedef struct {
    bool dark, music, spanish;
} Preferences;

typedef enum { WORK_NONE, WORK_REFRESH, WORK_COVER } CatalogWorkKind;
typedef enum { REFRESH_IDLE, REFRESH_UPDATED, REFRESH_FAILED } RefreshStatus;

typedef struct {
    Thread thread;
    CatalogWorkKind kind;
    atomic_bool cancel, finished;
    StoreLiveCatalog catalog;
    StoreInstallError error;
    bool success;
    StoreCatalogEntry *cover_entry;
    unsigned char *cover_pixels;
} CatalogWork;

typedef struct {
    StoreLiveCatalog live;
    const StoreCatalogEntry *entries;
    size_t entry_count;
    const StoreCatalogEntry **filtered;
    size_t count, cursor;
    StoreCategory category;
    const StoreCatalogEntry *detail;
    const StoreCatalogEntry *job_entry;
    StoreScreen screen, help_return;
    Preferences preferences;
    InstallJob job;
    Thread worker;
    CatalogWork catalog_work;
    RefreshStatus refresh_status;
    StoreInstallError refresh_error;
    bool refresh_requested, install_requested;
    unsigned catalog_generation;
    bool running, exit_requested, audio_available, music_available, redraw;
    bool preference_error;
    char progress_label[72];
} Menu;

typedef struct {
    bool down, valid;
    u32 finger;
    int start_x, start_y, end_x, end_y;
    StoreScreen screen;
    StoreCategory category;
    size_t page;
    unsigned catalog_generation;
    StoreHit start;
} TouchGesture;

static const StoreHit no_hit = {HIT_NONE, 0};

static bool marker_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static bool ensure_directory(const char *path)
{
    if (mkdir(path, 0777) == 0)
        return true;
    struct stat info;
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool write_preference(const char *path, bool present)
{
    if (!ensure_directory("sdmc:/switch") || !ensure_directory(PREFERENCE_DIR))
        return false;
    if (!present) {
        if (!marker_exists(path))
            return true;
        if (unlink(path) != 0)
            return false;
    } else {
        if (marker_exists(path))
            return true;
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0)
            return false;
        if (close(fd) != 0)
            return false;
    }
    return R_SUCCEEDED(fsdevCommitDevice("sdmc"));
}

static void filter(Menu *menu, StoreCategory category)
{
    menu->category = category;
    menu->count = 0;
    menu->cursor = 0;
    for (size_t index = 0; index < menu->entry_count; ++index) {
        if (category == STORE_ALL || menu->entries[index].category == category)
            menu->filtered[menu->count++] = &menu->entries[index];
    }
    menu->redraw = true;
}

static void play(Menu *menu, StoreSound sound)
{
    if (menu->audio_available)
        store_audio_play(sound);
}

static StoreCatalogEntry *find_entry(const StoreLiveCatalog *live, const char *id)
{
    if (id)
        for (size_t i = 0; i < live->count; ++i)
            if (strcmp(live->entries[i].id, id) == 0)
                return &live->entries[i];
    return NULL;
}

static void release_catalog(StoreLiveCatalog *live)
{
    for (size_t i = 0; i < live->count; ++i) {
        free(live->entries[i].cover_pixels);
        live->entries[i].cover_pixels = NULL;
    }
    catalog_sync_free(live);
}

static bool replace_catalog(Menu *menu, StoreLiveCatalog *next)
{
    const StoreCatalogEntry *selected = menu->cursor < menu->count ? menu->filtered[menu->cursor] : NULL;
    const size_t capacity = next->count ? next->count : 1;
    const StoreCatalogEntry **filtered = realloc(menu->filtered, capacity * sizeof *filtered);
    if (!filtered)
        return false;
    menu->filtered = filtered;

    const StoreCatalogEntry *next_selection = find_entry(next, selected ? selected->id : NULL);
    menu->detail = find_entry(next, menu->detail ? menu->detail->id : NULL);
    menu->job_entry = find_entry(next, menu->job_entry ? menu->job_entry->id : NULL);
    menu->job.package = NULL;
    if (!menu->detail && (menu->screen == STORE_DETAILS || menu->screen == STORE_CONFIRM))
        menu->screen = STORE_GRID;
    if (!menu->detail && menu->help_return == STORE_DETAILS)
        menu->help_return = STORE_GRID;
    for (size_t i = 0; i < menu->live.count; ++i) {
        StoreCatalogEntry *old = &menu->live.entries[i];
        if (!old->cover_pixels || !old->cover_url)
            continue;
        StoreCatalogEntry *same = find_entry(next, old->id);
        if (same && same->cover_url && strcmp(same->cover_url, old->cover_url) == 0) {
            same->cover_pixels = old->cover_pixels;
            same->cover_attempted = true;
            old->cover_pixels = NULL;
        }
    }
    release_catalog(&menu->live);
    menu->live = *next;
    *next = (StoreLiveCatalog){0};
    menu->entries = menu->live.entries;
    menu->entry_count = menu->live.count;
    ++menu->catalog_generation;
    store_catalog_set_active(menu->entries, menu->entry_count);
    filter(menu, store_catalog_has_category(menu->category) ? menu->category : STORE_ALL);
    for (size_t i = 0; next_selection && i < menu->count; ++i)
        if (menu->filtered[i] == next_selection) {
            menu->cursor = i;
            break;
        }
    return true;
}

static void switch_category(Menu *menu, StoreCategory category)
{
    if (category != menu->category && store_catalog_has_category(category)) {
        filter(menu, category);
        play(menu, STORE_SOUND_MOVE);
    }
}

static void cycle_category(Menu *menu, int direction)
{
    int next = menu->category;
    do {
        next = (next + direction + STORE_CATEGORY_COUNT) % STORE_CATEGORY_COUNT;
    } while (!store_catalog_has_category((StoreCategory)next));
    switch_category(menu, (StoreCategory)next);
}

static void move_cursor(Menu *menu, size_t index)
{
    if (index < menu->count && index != menu->cursor) {
        menu->cursor = index;
        menu->redraw = true;
        play(menu, STORE_SOUND_MOVE);
    }
}

static size_t page_count(const Menu *menu)
{
    return (menu->count + STORE_CARDS_PER_PAGE - 1) / STORE_CARDS_PER_PAGE;
}

static void change_page(Menu *menu, int direction)
{
    const size_t page = menu->cursor / STORE_CARDS_PER_PAGE;
    if ((direction < 0 && page == 0) || (direction > 0 && page + 1 >= page_count(menu)))
        return;
    const size_t target_page = direction > 0 ? page + 1 : page - 1;
    const size_t target = target_page * STORE_CARDS_PER_PAGE + menu->cursor % STORE_CARDS_PER_PAGE;
    move_cursor(menu, target < menu->count ? target : menu->count - 1);
}

static void move_on_grid(Menu *menu, u64 pressed)
{
    const size_t focus = menu->cursor % STORE_CARDS_PER_PAGE;
    const size_t column = focus % STORE_COLUMNS;
    if (pressed & HidNpadButton_AnyLeft) {
        if (column)
            move_cursor(menu, menu->cursor - 1);
        else
            change_page(menu, -1);
    } else if (pressed & HidNpadButton_AnyRight) {
        if (column + 1 < STORE_COLUMNS)
            move_cursor(menu, menu->cursor + 1);
        else
            change_page(menu, 1);
    } else if ((pressed & HidNpadButton_AnyUp) && focus >= STORE_COLUMNS) {
        move_cursor(menu, menu->cursor - STORE_COLUMNS);
    } else if ((pressed & HidNpadButton_AnyDown) && focus + STORE_COLUMNS < STORE_CARDS_PER_PAGE) {
        const size_t lower_row = (menu->cursor / STORE_CARDS_PER_PAGE) * STORE_CARDS_PER_PAGE + STORE_COLUMNS;
        if (lower_row < menu->count) {
            const size_t target = menu->cursor + STORE_COLUMNS;
            move_cursor(menu, target < menu->count ? target : menu->count - 1);
        }
    }
}

static bool processing(InstallPhase phase)
{
    return phase >= CHECKING && phase <= SAVING;
}

static bool refreshing(const Menu *menu)
{
    return menu->refresh_requested || menu->catalog_work.kind == WORK_REFRESH;
}

static void check_and_install(void *data)
{
    Menu *menu = data;
    InstallJob *job = &menu->job;
    if (!catalog_sync_check_project(menu->job_entry->id, job->package, &job->cancel, &job->error) ||
        atomic_load(&job->cancel)) {
        atomic_store(&job->phase, atomic_load(&job->cancel) ? CANCELLED : FAILED);
        return;
    }
    atomic_store(&job->phase, PREPARING);
    install_package(job);
}

static void begin_install(Menu *menu)
{
    if (!menu->detail || menu->running || refreshing(menu) || menu->detail->editing ||
        menu->detail->package.kind == STORE_PACKAGE_NONE)
        return;
    if (menu->catalog_work.kind == WORK_COVER) {
        menu->install_requested = true;
        atomic_store(&menu->catalog_work.cancel, true);
        return;
    }

    menu->job_entry = menu->detail;
    menu->job.package = &menu->detail->package;
    menu->job.error = (StoreInstallError){0};
    atomic_store(&menu->job.cancel, false);
    atomic_store(&menu->job.downloaded, 0);
    atomic_store(&menu->job.total, (unsigned)menu->job.package->size);
    atomic_store(&menu->job.phase, CHECKING);
    Result result = threadCreate(&menu->worker, check_and_install, menu, NULL, 256 * 1024, 0x2C, -2);
    if (R_SUCCEEDED(result)) {
        result = threadStart(&menu->worker);
        if (R_FAILED(result))
            threadClose(&menu->worker);
    }
    if (R_FAILED(result)) {
        snprintf(menu->job.error.english, sizeof menu->job.error.english, "Could not start the download (0x%08x).", (unsigned)result);
        snprintf(menu->job.error.spanish, sizeof menu->job.error.spanish, "Error al iniciar la descarga (0x%08x).", (unsigned)result);
        atomic_store(&menu->job.phase, FAILED);
    } else {
        menu->running = true;
    }
    menu->screen = STORE_DETAILS;
    menu->redraw = true;
    play(menu, STORE_SOUND_CONFIRM);
}

static void cancel_install(Menu *menu)
{
    const InstallPhase phase = atomic_load(&menu->job.phase);
    if (menu->running && phase >= CHECKING && phase < SAVING && !atomic_load(&menu->job.cancel)) {
        atomic_store(&menu->job.cancel, true);
        menu->redraw = true;
        play(menu, STORE_SOUND_CANCEL);
    }
}

static void catalog_work(void *data)
{
    CatalogWork *work = data;
    if (work->kind == WORK_REFRESH) {
        work->success = catalog_sync_refresh(&work->catalog, &work->cancel, &work->error);
    } else {
        work->cover_pixels = store_cover_fetch(work->cover_entry->cover_url, &work->cancel);
    }
    atomic_store(&work->finished, true);
}

static bool launch_catalog_work(CatalogWork *work, CatalogWorkKind kind)
{
    work->kind = kind;
    atomic_store(&work->finished, false);
    atomic_store(&work->cancel, false);
    Result result = threadCreate(&work->thread, catalog_work, work, NULL, 256 * 1024, 0x30, -2);
    if (R_SUCCEEDED(result)) {
        result = threadStart(&work->thread);
        if (R_FAILED(result))
            threadClose(&work->thread);
    }
    if (R_SUCCEEDED(result))
        return true;
    work->kind = WORK_NONE;
    return false;
}

static void request_refresh(Menu *menu)
{
    if (menu->screen != STORE_GRID || menu->running || refreshing(menu))
        return;
    menu->refresh_requested = true;
    menu->refresh_status = REFRESH_IDLE;
    menu->refresh_error = (StoreInstallError){0};
    if (menu->catalog_work.kind == WORK_COVER)
        atomic_store(&menu->catalog_work.cancel, true);
    atomic_store(&menu->job.phase, READY);
    menu->redraw = true;
    play(menu, STORE_SOUND_CONFIRM);
}

static StoreCatalogEntry *next_cover(Menu *menu)
{
    if (!menu->live.count)
        return NULL;
    const size_t first = menu->screen == STORE_GRID ? menu->cursor / STORE_CARDS_PER_PAGE * STORE_CARDS_PER_PAGE : menu->cursor;
    const size_t end = menu->screen == STORE_GRID ? first + STORE_CARDS_PER_PAGE : first + 1;
    for (size_t i = first; i < menu->count && i < end; ++i) {
        const StoreCatalogEntry *entry = menu->filtered[i];
        if (entry->cover_url && *entry->cover_url && !entry->cover_attempted)
            return &menu->live.entries[entry - menu->live.entries];
    }
    return NULL;
}

static void update_catalog_work(Menu *menu, bool graphics)
{
    CatalogWork *work = &menu->catalog_work;
    if (work->kind != WORK_NONE && atomic_load(&work->finished)) {
        threadWaitForExit(&work->thread);
        threadClose(&work->thread);
        if (work->kind == WORK_REFRESH) {
            const bool accepted = work->success && !menu->exit_requested && replace_catalog(menu, &work->catalog);
            menu->refresh_status = accepted ? REFRESH_UPDATED : REFRESH_FAILED;
            menu->refresh_error = work->error;
            release_catalog(&work->catalog);
        } else {
            work->cover_entry->cover_attempted = work->cover_pixels || !atomic_load(&work->cancel);
            work->cover_entry->cover_pixels = work->cover_pixels;
            work->cover_pixels = NULL;
        }
        work->kind = WORK_NONE;
        menu->redraw = true;
    }

    if (menu->exit_requested || menu->running || work->kind != WORK_NONE)
        return;
    if (menu->refresh_requested) {
        menu->refresh_requested = false;
        work->error = (StoreInstallError){0};
        if (!launch_catalog_work(work, WORK_REFRESH))
            menu->refresh_status = REFRESH_FAILED;
        menu->redraw = true;
    } else if (menu->install_requested) {
        menu->install_requested = false;
        if (menu->screen == STORE_CONFIRM)
            begin_install(menu);
    } else if (graphics && (menu->screen == STORE_GRID || menu->screen == STORE_DETAILS)) {
        work->cover_entry = next_cover(menu);
        if (work->cover_entry && !launch_catalog_work(work, WORK_COVER))
            work->cover_entry->cover_attempted = true;
    }
}

static void open_detail(Menu *menu, size_t index)
{
    if (index < menu->count) {
        menu->cursor = index;
        menu->detail = menu->filtered[index];
        menu->screen = STORE_DETAILS;
        menu->redraw = true;
        play(menu, STORE_SOUND_CONFIRM);
    }
}

static void back(Menu *menu)
{
    if (menu->screen == STORE_HELP)
        menu->screen = menu->help_return;
    else if (menu->screen == STORE_CONFIRM) {
        menu->install_requested = false;
        menu->screen = STORE_DETAILS;
    } else if (menu->screen == STORE_DETAILS)
        menu->screen = STORE_GRID;
    else {
        cancel_install(menu);
        return;
    }
    menu->redraw = true;
    play(menu, STORE_SOUND_CANCEL);
}

static void activate_hit(Menu *menu, StoreHit hit)
{
    switch (hit.kind) {
        case HIT_CARD:
            open_detail(menu, menu->cursor / STORE_CARDS_PER_PAGE * STORE_CARDS_PER_PAGE + hit.index);
            break;
        case HIT_CATEGORY:
            switch_category(menu, (StoreCategory)hit.index);
            break;
        case HIT_PREVIOUS: change_page(menu, -1); break;
        case HIT_NEXT: change_page(menu, 1); break;
        case HIT_REFRESH: request_refresh(menu); break;
        case HIT_MUSIC:
            if (menu->music_available) {
                menu->preferences.music = !menu->preferences.music;
                store_audio_set_music(menu->preferences.music);
                menu->preference_error = !write_preference(MUTE_MARKER, !menu->preferences.music);
                menu->redraw = true;
                play(menu, STORE_SOUND_CONFIRM);
            }
            break;
        case HIT_THEME:
            menu->preferences.dark = !menu->preferences.dark;
            menu->preference_error = !write_preference(DARK_MARKER, menu->preferences.dark);
            menu->redraw = true;
            play(menu, STORE_SOUND_CONFIRM);
            break;
        case HIT_LANGUAGE:
            menu->preferences.spanish = !menu->preferences.spanish;
            menu->preference_error = !write_preference(SPANISH_MARKER, menu->preferences.spanish);
            menu->redraw = true;
            play(menu, STORE_SOUND_CONFIRM);
            break;
        case HIT_HELP:
            if (menu->screen == STORE_GRID || menu->screen == STORE_DETAILS) {
                menu->help_return = menu->screen;
                menu->screen = STORE_HELP;
                menu->redraw = true;
                play(menu, STORE_SOUND_CONFIRM);
            }
            break;
        case HIT_BACK: back(menu); break;
        case HIT_INSTALL:
            if (menu->detail && !menu->detail->editing && menu->detail->package.kind != STORE_PACKAGE_NONE &&
                !menu->running && !refreshing(menu)) {
                menu->screen = STORE_CONFIRM;
                menu->redraw = true;
                play(menu, STORE_SOUND_CONFIRM);
            }
            break;
        case HIT_CONFIRM:
            if (menu->screen == STORE_CONFIRM)
                begin_install(menu);
            break;
        case HIT_CANCEL_DOWNLOAD: cancel_install(menu); break;
        case HIT_EXIT:
            menu->exit_requested = true;
            cancel_install(menu);
            if (menu->catalog_work.kind != WORK_NONE)
                atomic_store(&menu->catalog_work.cancel, true);
            break;
        case HIT_NONE: break;
    }
}

static void controller(Menu *menu, u64 pressed)
{
    if (menu->exit_requested)
        return;
    if (pressed & HidNpadButton_Plus) {
        activate_hit(menu, (StoreHit){HIT_EXIT, 0});
    } else if (pressed & HidNpadButton_StickL) {
        activate_hit(menu, (StoreHit){HIT_REFRESH, 0});
    } else if (pressed & HidNpadButton_X) {
        activate_hit(menu, (StoreHit){HIT_THEME, 0});
    } else if (pressed & HidNpadButton_Y) {
        activate_hit(menu, (StoreHit){HIT_MUSIC, 0});
    } else if (pressed & HidNpadButton_StickR) {
        activate_hit(menu, (StoreHit){HIT_LANGUAGE, 0});
    } else if (pressed & HidNpadButton_Minus) {
        activate_hit(menu, (StoreHit){HIT_HELP, 0});
    } else if (pressed & HidNpadButton_B) {
        if (menu->running && (menu->screen == STORE_GRID || menu->screen == STORE_DETAILS))
            cancel_install(menu);
        else
            back(menu);
    } else if (pressed & HidNpadButton_A) {
        if (menu->screen == STORE_GRID)
            open_detail(menu, menu->cursor);
        else if (menu->screen == STORE_DETAILS)
            activate_hit(menu, (StoreHit){HIT_INSTALL, 0});
        else if (menu->screen == STORE_CONFIRM)
            activate_hit(menu, (StoreHit){HIT_CONFIRM, 0});
        else
            back(menu);
    } else if (menu->screen == STORE_GRID) {
        if (pressed & HidNpadButton_L)
            change_page(menu, -1);
        else if (pressed & HidNpadButton_R)
            change_page(menu, 1);
        else if (pressed & HidNpadButton_ZL)
            cycle_category(menu, -1);
        else if (pressed & HidNpadButton_ZR)
            cycle_category(menu, 1);
        else
            move_on_grid(menu, pressed);
    }
}

static StoreHit touch_hit(TouchGesture *gesture, const StoreView *view, unsigned catalog_generation)
{
    HidTouchScreenState state;
    if (hidGetTouchScreenStates(&state, 1) == 0)
        return no_hit;

    if (state.count > 0) {
        const HidTouchState *point = &state.touches[0];
        if (!gesture->down) {
            *gesture = (TouchGesture){
                .down = true,
                .valid = state.count == 1 && point->x < STORE_WIDTH && point->y < STORE_HEIGHT,
                .finger = point->finger_id,
                .start_x = (int)point->x, .start_y = (int)point->y,
                .end_x = (int)point->x, .end_y = (int)point->y,
                .screen = view->screen, .category = view->category, .page = view->page,
                .catalog_generation = catalog_generation,
                .start = store_hit_test(view, (int)point->x, (int)point->y),
            };
        } else if (state.count == 1 && point->finger_id == gesture->finger) {
            gesture->end_x = (int)point->x;
            gesture->end_y = (int)point->y;
        } else {
            gesture->valid = false;
        }
        return no_hit;
    }
    if (!gesture->down)
        return no_hit;
    gesture->down = false;
    if (!gesture->valid || gesture->catalog_generation != catalog_generation || gesture->screen != view->screen ||
        gesture->category != view->category || gesture->page != view->page)
        return no_hit;

    const int dx = gesture->end_x - gesture->start_x;
    const int dy = gesture->end_y - gesture->start_y;
    const bool swipe_area = gesture->start_y >= 161 && gesture->start_y < 575;
    if (view->screen == STORE_GRID && swipe_area && abs(dx) >= 96 && abs(dx) > abs(dy) * 2)
        return (StoreHit){dx < 0 ? HIT_NEXT : HIT_PREVIOUS, 0};
    if (abs(dx) > 27 || abs(dy) > 27)
        return no_hit;
    const StoreHit end = store_hit_test(view, gesture->end_x, gesture->end_y);
    return gesture->start.kind == end.kind && gesture->start.index == end.index ? end : no_hit;
}

static void describe_status(Menu *menu, StoreView *view, InstallPhase phase, unsigned bytes, unsigned total)
{
    const bool spanish = view->spanish;
    if (phase == READY) {
        if (menu->preference_error)
            view->status = store_text(spanish, "Setting applied for this session.", "La preferencia se aplicó durante esta sesión.");
        return;
    }
    if (phase >= CHECKING && phase < SAVING && atomic_load(&menu->job.cancel)) {
        view->status = store_text(spanish, "Canceling the installation...", "Cancelando la instalación...");
        return;
    }

    switch (phase) {
        case CHECKING: view->status = store_text(spanish, "Checking the published release", "Comprobando la versión publicada"); break;
        case PREPARING: view->status = store_text(spanish, "Preparing the microSD card", "Preparando la microSD"); break;
        case DOWNLOADING: {
            view->status = store_text(spanish, "Downloading", "Descargando");
            const unsigned mb = (unsigned)((uint64_t)bytes * 10 / (1024 * 1024));
            const unsigned target_mb = (unsigned)((uint64_t)total * 10 / (1024 * 1024));
            if (total) {
                snprintf(menu->progress_label, sizeof menu->progress_label,
                         store_text(spanish, "%u.%u of %u.%u MiB", "%u,%u de %u,%u MiB"),
                         mb / 10, mb % 10, target_mb / 10, target_mb % 10);
                view->progress = (int)((uint64_t)bytes * 100 / total);
            } else {
                snprintf(menu->progress_label, sizeof menu->progress_label,
                         "%u%c%u MiB", mb / 10, spanish ? ',' : '.', mb % 10);
            }
            view->status_detail = menu->progress_label;
            break;
        }
        case VERIFYING: view->status = store_text(spanish, "Checking SHA-256", "Comprobando SHA-256"); view->progress = 100; break;
        case SAVING: view->status = store_text(spanish, "Installing files on the microSD card", "Colocando los archivos en la microSD"); view->progress = 100; break;
        case INSTALLED: {
            view->tone = STORE_SUCCESS;
            view->status = store_text(spanish, "Installation complete", "Instalación lista");
            const char *error = store_text(spanish, menu->job.error.english, menu->job.error.spanish);
            view->status_detail = *error ? error : store_text(spanish, "Open the app from hbmenu.", "Abre la aplicación desde hbmenu.");
            break;
        }
        case ALREADY_INSTALLED:
            view->tone = STORE_SUCCESS;
            view->status = store_text(spanish, "This version is already installed", "Esta versión ya está instalada");
            break;
        case CONFLICT:
            view->tone = STORE_ERROR;
            view->status = store_text(spanish, "The destination already contains files", "Ya hay archivos en el destino");
            view->status_detail = store_text(spanish, "The existing copy was kept.", "La copia existente se conserva.");
            break;
        case CANCELLED: view->status = store_text(spanish, "Download canceled", "Descarga cancelada"); break;
        case FAILED:
            view->tone = STORE_ERROR;
            view->status = store_text(spanish, "Installation failed", "Error de instalación");
            view->status_detail = store_text(spanish, menu->job.error.english, menu->job.error.spanish);
            break;
        case READY: break;
    }
}

static StoreView menu_view(Menu *menu, InstallPhase phase, unsigned bytes, unsigned total)
{
    const size_t page = menu->cursor / STORE_CARDS_PER_PAGE;
    const size_t start = page * STORE_CARDS_PER_PAGE;
    StoreView view = {
        .screen = menu->screen, .category = menu->category, .dark = menu->preferences.dark,
        .music = menu->preferences.music, .music_available = menu->music_available,
        .spanish = menu->preferences.spanish,
        .refreshing = refreshing(menu),
        .busy = menu->running, .can_cancel = menu->running && phase < SAVING && !atomic_load(&menu->job.cancel),
        .page = page, .pages = page_count(menu), .filter_count = menu->count,
        .focus = menu->cursor % STORE_CARDS_PER_PAGE, .details = menu->detail, .job_entry = menu->job_entry,
        .progress = -1,
    };
    const char *refresh_error = store_text(view.spanish, menu->refresh_error.english, menu->refresh_error.spanish);
    if (menu->refresh_status == REFRESH_FAILED) {
        view.refresh_status = *refresh_error ? refresh_error :
            store_text(view.spanish, "Refresh failed. Showing the last catalogue.", "Error al actualizar. Se muestra el último catálogo.");
        view.tone = STORE_ERROR;
    } else if (menu->refresh_status == REFRESH_UPDATED) {
        view.refresh_status = *refresh_error ? refresh_error :
            store_text(view.spanish, "Catalogue refreshed.", "Catálogo actualizado.");
        view.tone = STORE_SUCCESS;
    }
    for (size_t i = start; i < menu->count && view.visible_count < STORE_CARDS_PER_PAGE; ++i)
        view.cards[view.visible_count++] = menu->filtered[i];
    describe_status(menu, &view, phase, bytes, total);
    return view;
}

static bool initialize_graphics(Framebuffer *framebuffer)
{
    const Result created = framebufferCreate(framebuffer, nwindowGetDefault(),
                                             STORE_WIDTH, STORE_HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(created))
        return false;
    if (R_SUCCEEDED(framebufferMakeLinear(framebuffer)))
        return true;
    framebufferClose(framebuffer);
    return false;
}

static void draw_console(const StoreView *view)
{
    const bool spanish = view->spanish;
    consoleClear();
    printf("\n SWITCH2HOMEBREW | %s 0.5.2\n\n", store_text(spanish, "STORE", "TIENDA"));
    if (view->screen == STORE_GRID) {
        printf(store_text(spanish, " Page %lu/%lu  |  %lu projects\n\n", " Pagina %lu/%lu  |  %lu proyectos\n\n"), (unsigned long)(view->page + 1),
               (unsigned long)view->pages, (unsigned long)view->filter_count);
        for (size_t index = 0; index < view->visible_count; ++index) {
            const bool installable = view->cards[index]->package.kind != STORE_PACKAGE_NONE;
            printf(" %c %-42s %s\n", index == view->focus ? '>' : ' ', view->cards[index]->name,
                   installable ? store_text(spanish, "Installable", "Instalable") : store_text(spanish, "Info", "Ficha"));
        }
    } else if (view->screen == STORE_HELP) {
        puts(store_text(spanish, " A opens or confirms. B goes back or cancels.", " A abre la ficha o confirma. B vuelve o cancela."));
        puts(store_text(spanish, " L/R change pages. ZL/ZR change filters.", " L/R cambian pagina. ZL/ZR cambian categoria."));
        puts(store_text(spanish, " X switches theme. Y toggles music. Right stick changes language.",
                       " X cambia el tema. Y alterna la musica. El stick derecho cambia el idioma."));
    } else if (view->details) {
        const StoreCatalogEntry *item = view->details;
        printf(" %s\n\n %s\n\n", item->name, store_text(spanish, item->summary, item->summary_es));
        const char *rules = store_text(spanish, item->requirement, item->requirement_es);
        if (*rules)
            printf(" %s\n %s\n\n", store_text(spanish, "REQUIREMENTS", "REQUISITOS"), rules);
        puts(store_text(spanish, " The store carries homebrew. Use your own original files when required.",
                       " La tienda distribuye archivos homebrew. Usa tus originales cuando corresponda."));
        if (item->editing)
            puts(store_text(spanish, " Project temporarily being edited.", " Proyecto temporalmente en edicion."));
        else if (item->package.kind == STORE_PACKAGE_NONE)
            puts(store_text(spanish, " Direct install unavailable. See the project page.", " Sin instalacion directa. Consulta la ficha web."));
        else if (view->screen == STORE_CONFIRM)
            printf(store_text(spanish, " A confirms installation in %s\n", " A confirma la instalacion en %s\n"), item->package.target);
        else
            puts(store_text(spanish, " A opens installation confirmation.", " A abre la confirmacion de instalacion."));
        if (item->package.kind == STORE_PACKAGE_NONE && item->project_url)
            printf(" %s\n", item->project_url);
        const char *source = store_project_source(item);
        if (source)
            printf(" %s\n", source);
    }
    if (view->status)
        printf("\n %s %s\n %s\n", view->status, view->job_entry ? view->job_entry->name : "",
               view->status_detail ? view->status_detail : "");
    if (view->screen == STORE_GRID && (view->refreshing || view->refresh_status))
        puts(view->refreshing ? store_text(spanish, "Refreshing catalogue...", "Actualizando catálogo...") : view->refresh_status);
    if (view->screen == STORE_GRID)
        puts(store_text(spanish, " Press the left stick to refresh.", " Pulsa el stick izquierdo para actualizar."));
    puts(store_text(spanish, "\n A Continue | B Back or cancel | + Exit", "\n A Continuar | B Volver o cancelar | + Salir"));
}

int main(void)
{
    Menu menu = {
        .preferences = {
            .dark = marker_exists(DARK_MARKER),
            .music = !marker_exists(MUTE_MARKER),
            .spanish = marker_exists(SPANISH_MARKER),
        },
        .job = {.phase = READY, .downloaded = 0, .total = 0, .cancel = false},
        .screen = STORE_GRID,
    };
    menu.filtered = calloc(store_catalog_count, sizeof *menu.filtered);
    if (!menu.filtered)
        return 1;
    menu.entries = store_catalog;
    menu.entry_count = store_catalog_count;
    store_catalog_set_active(menu.entries, menu.entry_count);
    filter(&menu, STORE_ALL);
    if (ensure_directory("sdmc:/switch") && ensure_directory(PREFERENCE_DIR)) {
        StoreLiveCatalog cached = {0};
        StoreInstallError error = {0};
        if (catalog_sync_read_cache(&cached, &error))
            replace_catalog(&menu, &cached);
        release_catalog(&cached);
    }
    menu.refresh_requested = true;

    Framebuffer framebuffer = {0};
    bool graphics = initialize_graphics(&framebuffer);
    if (!graphics)
        consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    if (graphics) {
        if (hosversionAtLeast(17, 0, 0))
            hidSetTouchScreenOutputRanges(STORE_WIDTH, STORE_HEIGHT);
        hidInitializeTouchScreen();
    }
    TouchGesture touch = {0};
    store_audio_set_music(menu.preferences.music);
    menu.audio_available = store_audio_init();
    if (menu.audio_available)
        menu.music_available = store_audio_has_music();

    int previous_phase = -1;
    unsigned previous_progress = 0, previous_total = 0;
    const u64 animation_start = armGetSystemTick();
    const u64 wave_interval = armNsToTicks(UINT64_C(50000000));
    u64 next_wave_frame = animation_start;
    while (appletMainLoop()) {
        InstallPhase phase = atomic_load(&menu.job.phase);
        if (menu.running && !processing(phase)) {
            threadWaitForExit(&menu.worker);
            threadClose(&menu.worker);
            menu.running = false;
            menu.redraw = true;
        }
        update_catalog_work(&menu, graphics);
        if (menu.exit_requested && !menu.running && menu.catalog_work.kind == WORK_NONE)
            break;

        padUpdate(&pad);
        const u64 pressed = padGetButtonsDown(&pad);
        const unsigned bytes_before = atomic_load(&menu.job.downloaded);
        const unsigned total_before = atomic_load(&menu.job.total);
        if (!menu.exit_requested && graphics) {
            StoreView view = menu_view(&menu, phase, bytes_before, total_before);
            const StoreHit hit = touch_hit(&touch, &view, menu.catalog_generation);
            if (!pressed && hit.kind != HIT_NONE)
                activate_hit(&menu, hit);
        }
        controller(&menu, pressed);

        phase = atomic_load(&menu.job.phase);
        const unsigned bytes = atomic_load(&menu.job.downloaded);
        const unsigned total = atomic_load(&menu.job.total);
        const unsigned progress = total ? (unsigned)((uint64_t)bytes * 100 / total) : bytes / (256 * 1024);
        if (menu.audio_available) {
            const bool music_available = store_audio_has_music();
            if (menu.music_available != music_available) {
                menu.music_available = music_available;
                menu.redraw = true;
            }
        }
        const u64 frame_tick = armGetSystemTick();
        if (menu.redraw || (int)phase != previous_phase || progress != previous_progress || total != previous_total ||
            (graphics && frame_tick >= next_wave_frame)) {
            StoreView view = menu_view(&menu, phase, bytes, total);
            if (graphics) {
                view.elapsed_ms = (uint32_t)(armTicksToNs(frame_tick - animation_start) / 1000000u % STORE_WAVE_PERIOD_MS);
                next_wave_frame = frame_tick + wave_interval;
                u32 stride;
                uint32_t *pixels = framebufferBegin(&framebuffer, &stride);
                if (pixels) {
                    store_draw(pixels, stride, &view);
                    framebufferEnd(&framebuffer);
                } else {
                    framebufferClose(&framebuffer);
                    graphics = false;
                    consoleInit(NULL);
                }
            }
            if (!graphics)
                draw_console(&view);
            menu.redraw = false;
            previous_phase = (int)phase;
            previous_progress = progress;
            previous_total = total;
        }

        if (graphics)
            svcSleepThread(8000000);
        else
            consoleUpdate(NULL);
    }

    if (menu.running) {
        atomic_store(&menu.job.cancel, true);
        threadWaitForExit(&menu.worker);
        threadClose(&menu.worker);
    }
    if (menu.catalog_work.kind != WORK_NONE) {
        atomic_store(&menu.catalog_work.cancel, true);
        threadWaitForExit(&menu.catalog_work.thread);
        threadClose(&menu.catalog_work.thread);
        free(menu.catalog_work.cover_pixels);
        release_catalog(&menu.catalog_work.catalog);
    }
    if (menu.audio_available)
        store_audio_close();
    if (graphics)
        framebufferClose(&framebuffer);
    else
        consoleExit(NULL);
    release_catalog(&menu.live);
    free(menu.filtered);
    return 0;
}
