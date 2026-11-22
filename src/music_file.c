/* Songs from the music folder as the station's PCM. dr_libs (third_party, compiled in dr_libs.c)
 * decode MP3, FLAC and WAV to 16-bit samples; here they become stereo (a mono song is copied to both
 * sides; past two channels, the first two are kept) at 44.1 kHz.
 *
 * The rate is converted by linear interpolation. `phase` says, in 32.32 fixed point, where the next
 * output frame falls after the source frame `before` (0 = on it, 1.0 = on the one after); every output
 * frame moves it on by `step` = source rate / 44100, and every source frame read moves it back by 1.0. */
#include "music_file.h"

#include <stdint.h>
#include <stdlib.h>

#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

enum { SOURCE_FRAMES = 512, MAX_CHANNELS = 8 };
#define ONE ((uint64_t)1 << 32)

typedef enum { KIND_MP3, KIND_FLAC, KIND_WAV } Kind;

struct MusicFile {
    Kind kind;
    drmp3 mp3;
    drwav wav;
    drflac *flac;
    unsigned channels;
    uint64_t step, phase;
    Frame before;
    int primed; /* `before` holds the song's first frame or a later one */
    int16_t source[SOURCE_FRAMES * MAX_CHANNELS];
};

static int named(const wchar_t *path, const wchar_t *extension) {
    size_t length = wcslen(path), tail = wcslen(extension);
    return length >= tail && !_wcsicmp(path + length - tail, extension);
}

MusicFile *music_open(const wchar_t *path) {
    MusicFile *file = calloc(1, sizeof(*file));
    if (!file || !path) {
        free(file);
        return NULL;
    }
    unsigned rate = 0;
    int opened = 0;
    if (named(path, L".mp3") && drmp3_init_file_w(&file->mp3, path, NULL)) {
        file->kind = KIND_MP3;
        file->channels = file->mp3.channels;
        rate = file->mp3.sampleRate;
        opened = 1;
    } else if (named(path, L".flac") && (file->flac = drflac_open_file_w(path, NULL))) {
        file->kind = KIND_FLAC;
        file->channels = file->flac->channels;
        rate = file->flac->sampleRate;
        opened = 1;
    } else if (named(path, L".wav") && drwav_init_file_w(&file->wav, path, NULL)) {
        file->kind = KIND_WAV;
        file->channels = file->wav.channels;
        rate = file->wav.sampleRate;
        opened = 1;
    }
    if (!opened) {
        free(file);
        return NULL;
    }
    if (!file->channels || file->channels > MAX_CHANNELS || rate < 8000 || rate > 384000) {
        music_close(file);
        return NULL;
    }
    file->step = ((uint64_t)rate << 32) / PCM_RATE;
    return file;
}

static size_t decode(MusicFile *file) {
    switch (file->kind) {
    case KIND_MP3: return (size_t)drmp3_read_pcm_frames_s16(&file->mp3, SOURCE_FRAMES, file->source);
    case KIND_FLAC: return (size_t)drflac_read_pcm_frames_s16(file->flac, SOURCE_FRAMES, file->source);
    case KIND_WAV: return (size_t)drwav_read_pcm_frames_s16(&file->wav, SOURCE_FRAMES, file->source);
    }
    return 0;
}

/* Between a and b, `phase` (below 1.0) of the way. */
static short between(short a, short b, uint64_t phase) {
    return (short)(a + (((int64_t)(b - a) * (int64_t)(phase >> 16)) >> 16));
}

size_t music_read(MusicFile *file, Frame *out, size_t room) {
    size_t made = 0;
    /* A read that only primes the first frame makes nothing: read on, as 0 means the song has ended. */
    for (size_t got = 1; !made && got;) {
        got = decode(file);
        for (size_t i = 0; i < got; ++i) {
            const int16_t *sample = file->source + i * file->channels;
            Frame after = {sample[0], file->channels > 1 ? sample[1] : sample[0]};
            if (!file->primed) {
                file->before = after;
                file->primed = 1;
                continue;
            }
            for (; file->phase < ONE && made < room; file->phase += file->step) {
                out[made].l = between(file->before.l, after.l, file->phase);
                out[made].r = between(file->before.r, after.r, file->phase);
                ++made;
            }
            /* Out of room (never with PCM_MAX_FRAMES: 512 frames at 8 kHz make 2823) drops the rest. */
            file->phase = file->phase >= ONE ? file->phase - ONE : 0;
            file->before = after;
        }
    }
    return made;
}

void music_close(MusicFile *file) {
    if (!file) return;
    switch (file->kind) {
    case KIND_MP3: drmp3_uninit(&file->mp3); break;
    case KIND_FLAC: drflac_close(file->flac); break;
    case KIND_WAV: drwav_uninit(&file->wav); break;
    }
    free(file);
}
