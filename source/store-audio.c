#include "store-audio.h"

#include <math.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

enum {
    SAMPLE_RATE = 48000,
    AUDIO_BUFFERS = 5,
    BUFFER_BYTES = 0x1000,
    BUFFER_FRAMES = 768,
    SINE_BITS = 10,
    SINE_SIZE = 1 << SINE_BITS,
    PCM_FRAME_BYTES = 4,
    WAVE_CACHE_FRAMES = 4096,
    LOOP_BLEND_FRAMES = 240,
    MUSIC_GAIN = 256,
    WAVE_VOLUME = 60,
    EMBEDDED_VOLUME = 287,
    AUDIO_STACK_SIZE = 64 * 1024,
    AUDIO_PRIORITY = 0x28,
    AUDIO_FALLBACK_PRIORITY = 0x2C,
    AUDIO_POLL_NS = 4000000,
};

typedef enum { AUDIO_STOPPED, AUDIO_STARTING, AUDIO_RUNNING } AudioState;

extern const unsigned char _binary_store_music_pcm_start[];
extern const unsigned char _binary_store_music_pcm_end[];

typedef struct {
    FILE *file;
    const unsigned char *embedded;
    long data_offset;
    uint32_t frames, position;
    unsigned buffered, cursor, blend_frames;
    int volume;
    unsigned char head[LOOP_BLEND_FRAMES * PCM_FRAME_BYTES];
    unsigned char pcm[WAVE_CACHE_FRAMES * PCM_FRAME_BYTES];
} MusicTrack;

typedef struct {
    unsigned frequency, start_ms, duration_ms;
    int volume;
} Tone;

static const Tone cues[][2] = {
    [STORE_SOUND_MOVE] = {{740, 0, 37, 1250}, {1110, 0, 15, 360}},
    [STORE_SOUND_CONFIRM] = {{620, 0, 49, 1350}, {830, 27, 87, 1530}},
    [STORE_SOUND_CANCEL] = {{580, 0, 43, 1180}, {410, 21, 69, 1150}},
};

typedef struct {
    uint32_t step;
    unsigned start, length;
    int volume;
} EffectVoice;

static alignas(0x1000) int16_t samples[AUDIO_BUFFERS][BUFFER_BYTES / sizeof(int16_t)];
static AudioOutBuffer buffers[AUDIO_BUFFERS];
static int16_t sine_table[SINE_SIZE];
static MusicTrack music;
static int music_gain;
static EffectVoice effects[2];
static unsigned effect_frame, effect_end;
static Thread audio_thread;
static bool thread_created;
static atomic_int audio_state = AUDIO_STOPPED;
static atomic_bool stop_requested;
static atomic_bool requested_music = true;
static atomic_bool music_available;
static atomic_int pending_sound = -1;

static uint32_t oscillator_step(double frequency)
{
    return (uint32_t)(frequency * (4294967296.0 / SAMPLE_RATE));
}

static void build_tables(void)
{
    const double turn = 6.28318530717958647693;
    for (unsigned i = 0; i < SINE_SIZE; ++i)
        sine_table[i] = (int16_t)(sin(turn * i / SINE_SIZE) * 32767.0);
}

static int sine_at(uint32_t phase)
{
    const unsigned index = phase >> (32 - SINE_BITS);
    const int fraction = (int)((phase >> (24 - SINE_BITS)) & 255u);
    const int first = sine_table[index];
    const int next = sine_table[(index + 1) & (SINE_SIZE - 1)];
    return first + (next - first) * fraction / 256;
}

static uint16_t le16(const unsigned char *bytes)
{
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static uint32_t le32(const unsigned char *bytes)
{
    return (uint32_t)le16(bytes) | (uint32_t)le16(bytes + 2) << 16;
}

static bool valid_pcm_format(const unsigned char *format, uint32_t length)
{
    static const unsigned char pcm_subformat[] = {
        1, 0, 0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113,
    };
    if (length < 16 || le16(format + 2) != 2 || le32(format + 4) != SAMPLE_RATE ||
        le32(format + 8) != SAMPLE_RATE * PCM_FRAME_BYTES ||
        le16(format + 12) != PCM_FRAME_BYTES || le16(format + 14) != 16)
        return false;
    if (le16(format) == 1)
        return true;
    if (le16(format) != 0xfffe || length < 40 || le16(format + 16) < 22 ||
        le16(format + 16) > length - 18 || le16(format + 18) != 16)
        return false;
    const uint32_t channel_mask = le32(format + 20);
    return (channel_mask == 3 || channel_mask == 0) && !memcmp(format + 24, pcm_subformat, sizeof pcm_subformat);
}

static bool parse_wave(FILE *file, long *data_offset, uint32_t *frames)
{
    unsigned char riff[12];
    if (fseek(file, 0, SEEK_END) != 0)
        return false;
    const long length = ftell(file);
    if (length < (long)sizeof riff || fseek(file, 0, SEEK_SET) != 0 ||
        fread(riff, 1, sizeof riff, file) != sizeof riff ||
        memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4))
        return false;

    const uint64_t limit = 8 + (uint64_t)le32(riff + 4);
    if (limit < sizeof riff || limit > (uint64_t)length)
        return false;

    bool have_format = false, have_data = false;
    for (uint64_t position = sizeof riff; position < limit;) {
        unsigned char chunk[8];
        if (limit - position < sizeof chunk || fseek(file, (long)position, SEEK_SET) != 0 ||
            fread(chunk, 1, sizeof chunk, file) != sizeof chunk)
            return false;
        const uint32_t size = le32(chunk + 4);
        const uint64_t start = position + sizeof chunk;
        position = start + size + (size & 1u);
        if (position > limit)
            return false;

        if (!memcmp(chunk, "fmt ", 4)) {
            unsigned char format[40] = {0};
            const size_t format_bytes = size < sizeof format ? size : sizeof format;
            if (have_format || fread(format, 1, format_bytes, file) != format_bytes || !valid_pcm_format(format, size))
                return false;
            have_format = true;
        } else if (!memcmp(chunk, "data", 4)) {
            if (have_data || !size || size % PCM_FRAME_BYTES != 0)
                return false;
            *data_offset = (long)start;
            *frames = size / PCM_FRAME_BYTES;
            have_data = true;
        }
    }
    return have_format && have_data;
}

static void close_music(void)
{
    if (music.file)
        fclose(music.file);
    music.file = NULL;
    music.embedded = NULL;
    music_gain = 0;
}

static void reset_music_position(uint32_t frames, int volume)
{
    music.frames = frames;
    music.position = music.buffered = music.cursor = 0;
    music.blend_frames = frames / 4;
    if (music.blend_frames > LOOP_BLEND_FRAMES)
        music.blend_frames = LOOP_BLEND_FRAMES;
    music.volume = volume;
}

static void open_embedded_music(void)
{
    const uintptr_t start = (uintptr_t)_binary_store_music_pcm_start;
    const uintptr_t end = (uintptr_t)_binary_store_music_pcm_end;
    if (end <= start)
        return;
    const uintptr_t bytes = end - start;
    if (bytes % PCM_FRAME_BYTES != 0 || bytes / PCM_FRAME_BYTES > UINT32_MAX)
        return;
    reset_music_position((uint32_t)(bytes / PCM_FRAME_BYTES), EMBEDDED_VOLUME);
    music.embedded = _binary_store_music_pcm_start;
}

static bool open_wave_music(void)
{
    FILE *file = fopen("sdmc:/switch/switch2homebrew-store/music.wav", "rb");
    if (!file)
        return false;
    long data_offset;
    uint32_t frames;
    if (!parse_wave(file, &data_offset, &frames)) {
        fclose(file);
        return false;
    }
    reset_music_position(frames, WAVE_VOLUME);
    if (fseek(file, data_offset, SEEK_SET) != 0 ||
        fread(music.head, PCM_FRAME_BYTES, music.blend_frames, file) != music.blend_frames ||
        fseek(file, data_offset, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    music.data_offset = data_offset;
    music.file = file;
    return true;
}

static bool refill_wave_music(void)
{
    if (music.position == music.frames) {
        music.position = music.blend_frames;
        if (music.frames <= WAVE_CACHE_FRAMES && music.buffered == music.frames) {
            music.cursor = music.blend_frames;
            return true;
        }
        if (fseek(music.file, music.data_offset + music.blend_frames * PCM_FRAME_BYTES, SEEK_SET) != 0)
            return false;
    }
    const uint32_t remaining = music.frames - music.position;
    music.buffered = remaining < WAVE_CACHE_FRAMES ? (unsigned)remaining : WAVE_CACHE_FRAMES;
    music.cursor = 0;
    return fread(music.pcm, PCM_FRAME_BYTES, music.buffered, music.file) == music.buffered;
}

static void render_music(int *left, int *right, bool enabled)
{
    if ((!music.file && !music.embedded) || (!enabled && !music_gain))
        return;
    if (enabled && music_gain < MUSIC_GAIN)
        ++music_gain;
    else if (!enabled)
        --music_gain;

    const unsigned char *frame, *head_frames;
    if (music.embedded) {
        if (music.position == music.frames)
            music.position = music.blend_frames;
        frame = music.embedded + (size_t)music.position * PCM_FRAME_BYTES;
        head_frames = music.embedded;
    } else {
        if (music.cursor == music.buffered && !refill_wave_music()) {
            close_music();
            open_embedded_music();
            atomic_store_explicit(&music_available, music.embedded != NULL, memory_order_relaxed);
            return;
        }
        frame = music.pcm + music.cursor++ * PCM_FRAME_BYTES;
        head_frames = music.head;
    }
    int l = (int16_t)le16(frame), r = (int16_t)le16(frame + 2);
    const uint32_t remaining = music.frames - music.position++;
    if (remaining <= music.blend_frames) {
        const unsigned head_index = music.blend_frames - remaining;
        const unsigned char *head = head_frames + head_index * PCM_FRAME_BYTES;
        const int head_weight = (int)(head_index + 1);
        const int tail_weight = (int)music.blend_frames - head_weight;
        l = (l * tail_weight + (int16_t)le16(head) * head_weight) / (int)music.blend_frames;
        r = (r * tail_weight + (int16_t)le16(head + 2) * head_weight) / (int)music.blend_frames;
    }
    *left += l * music.volume / 1024 * music_gain / MUSIC_GAIN;
    *right += r * music.volume / 1024 * music_gain / MUSIC_GAIN;
}

static int render_effect(void)
{
    if (effect_frame >= effect_end)
        return 0;

    enum { ATTACK = 175 };
    int output = 0;
    for (unsigned i = 0; i < sizeof(effects) / sizeof(effects[0]); ++i) {
        const EffectVoice *voice = &effects[i];
        if (effect_frame < voice->start || effect_frame - voice->start >= voice->length)
            continue;

        const unsigned age = effect_frame - voice->start;
        int envelope = (int)((voice->length - age) * 256u / voice->length);
        envelope = envelope * envelope / 256;
        if (age < ATTACK)
            envelope = envelope * (int)age / ATTACK;
        const uint32_t phase = (uint32_t)age * voice->step;
        const int tone = sine_at(phase) + sine_at(phase * 2u) / 8;
        output += (tone * voice->volume / 32768) * envelope / 256;
    }
    ++effect_frame;
    return output;
}

static void start_effect(StoreSound sound)
{
    effect_frame = effect_end = 0;
    for (unsigned i = 0; i < sizeof(effects) / sizeof(effects[0]); ++i) {
        const Tone tone = cues[sound][i];
        const unsigned start = tone.start_ms * (SAMPLE_RATE / 1000);
        const unsigned length = tone.duration_ms * (SAMPLE_RATE / 1000);
        effects[i] = (EffectVoice){oscillator_step(tone.frequency), start, length, tone.volume};
        if (start + length > effect_end)
            effect_end = start + length;
    }
}

static void fill_buffer(AudioOutBuffer *buffer)
{
    const bool enabled = atomic_load_explicit(&requested_music, memory_order_relaxed);
    const int sound = atomic_exchange_explicit(&pending_sound, -1, memory_order_relaxed);
    if (sound >= 0)
        start_effect((StoreSound)sound);
    int16_t *output = buffer->buffer;
    for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) {
        int left = 0, right = 0;
        render_music(&left, &right, enabled);
        const int click = render_effect();
        output[2 * frame] = (int16_t)(left + click);
        output[2 * frame + 1] = (int16_t)(right + click);
    }
}

static bool queue_initial_buffers(void)
{
    /* Five 16 ms buffers give playback an 80 ms initial margin. */
    for (unsigned i = 0; i < AUDIO_BUFFERS; ++i) {
        buffers[i] = (AudioOutBuffer){
            .buffer = samples[i],
            .buffer_size = BUFFER_BYTES,
            .data_size = BUFFER_FRAMES * 2 * sizeof(int16_t),
        };
        fill_buffer(&buffers[i]);
        if (R_FAILED(audoutAppendAudioOutBuffer(&buffers[i])))
            return false;
    }
    return true;
}

static bool refill_released_buffers(void)
{
    for (unsigned i = 0; i < AUDIO_BUFFERS; ++i) {
        AudioOutBuffer *released = NULL;
        u32 count = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)))
            return false;
        if (!count)
            break;
        bool known = false;
        for (unsigned j = 0; j < AUDIO_BUFFERS; ++j) {
            if (released == &buffers[j]) {
                known = true;
                break;
            }
        }
        if (!known)
            return false;
        fill_buffer(released);
        if (R_FAILED(audoutAppendAudioOutBuffer(released)))
            return false;
    }
    return true;
}

static void audio_worker(void *argument)
{
    (void)argument;
    const bool initialized = R_SUCCEEDED(audoutInitialize());
    bool started = false;
    if (initialized && audoutGetSampleRate() == SAMPLE_RATE && audoutGetChannelCount() == 2 &&
        audoutGetPcmFormat() == PcmFormat_Int16) {
        build_tables();
        if (!open_wave_music())
            open_embedded_music();
        effect_frame = effect_end = 0;
        started = R_SUCCEEDED(audoutStartAudioOut());
        if (started && queue_initial_buffers()) {
            atomic_store_explicit(&music_available, music.file != NULL || music.embedded != NULL,
                                  memory_order_relaxed);
            atomic_store(&audio_state, AUDIO_RUNNING);
            while (!atomic_load_explicit(&stop_requested, memory_order_relaxed) &&
                   refill_released_buffers())
                svcSleepThread(AUDIO_POLL_NS);
        }
    }

    if (started)
        audoutStopAudioOut();
    if (initialized)
        audoutExit();
    close_music();
    atomic_store_explicit(&music_available, false, memory_order_relaxed);
    atomic_store(&audio_state, AUDIO_STOPPED);
}

bool store_audio_init(void)
{
    if (atomic_load(&audio_state) == AUDIO_RUNNING)
        return true;
    store_audio_close();
    atomic_store_explicit(&stop_requested, false, memory_order_relaxed);
    atomic_store_explicit(&pending_sound, -1, memory_order_relaxed);
    atomic_store(&audio_state, AUDIO_STARTING);
    Result result = threadCreate(&audio_thread, audio_worker, NULL, NULL, AUDIO_STACK_SIZE, AUDIO_PRIORITY, -2);
    if (R_FAILED(result))
        result = threadCreate(&audio_thread, audio_worker, NULL, NULL, AUDIO_STACK_SIZE, AUDIO_FALLBACK_PRIORITY, -2);
    if (R_SUCCEEDED(result)) {
        result = threadStart(&audio_thread);
        if (R_SUCCEEDED(result))
            thread_created = true;
        else
            threadClose(&audio_thread);
    }
    if (R_FAILED(result)) {
        atomic_store(&audio_state, AUDIO_STOPPED);
        return false;
    }

    int state;
    while ((state = atomic_load(&audio_state)) == AUDIO_STARTING)
        svcSleepThread(1000000);
    if (state == AUDIO_RUNNING)
        return true;
    store_audio_close();
    return false;
}

bool store_audio_has_music(void)
{
    return atomic_load(&audio_state) == AUDIO_RUNNING &&
        atomic_load_explicit(&music_available, memory_order_relaxed);
}

void store_audio_set_music(bool enabled)
{
    atomic_store_explicit(&requested_music, enabled, memory_order_relaxed);
}

void store_audio_play(StoreSound sound)
{
    if ((unsigned)sound < sizeof(cues) / sizeof(cues[0]) && atomic_load(&audio_state) == AUDIO_RUNNING)
        atomic_store_explicit(&pending_sound, (int)sound, memory_order_relaxed);
}

void store_audio_close(void)
{
    if (!thread_created)
        return;
    atomic_store_explicit(&stop_requested, true, memory_order_relaxed);
    threadWaitForExit(&audio_thread);
    threadClose(&audio_thread);
    thread_created = false;
}
