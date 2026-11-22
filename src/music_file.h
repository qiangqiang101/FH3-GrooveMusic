/* Songs from the music folder (MP3, FLAC, WAV), read as the station's PCM (pcm.h). */
#ifndef GROOVE_MUSIC_FILE_H
#define GROOVE_MUSIC_FILE_H

#include <stddef.h>
#include <wchar.h>

#include "pcm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MusicFile MusicFile;

/* NULL when the file can't be opened or isn't MP3, FLAC or WAV (8 to 384 kHz, up to 8 channels). */
MusicFile *music_open(const wchar_t *path);
/* The next frames of the song, at most `room` (callers pass at least PCM_MAX_FRAMES); 0 at its end. */
size_t music_read(MusicFile *file, Frame *out, size_t room);
void music_close(MusicFile *file);

#ifdef __cplusplus
}
#endif
#endif
