#pragma once

#include <stdbool.h>

typedef enum {
    STORE_SOUND_MOVE,
    STORE_SOUND_CONFIRM,
    STORE_SOUND_CANCEL,
} StoreSound;

bool store_audio_init(void);
bool store_audio_has_music(void);
void store_audio_set_music(bool enabled);
void store_audio_play(StoreSound sound);
void store_audio_close(void);
