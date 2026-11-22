/* The station's sound: 44.1 kHz, 16-bit stereo, as the game's Groove player takes it. */
#ifndef GROOVE_PCM_H
#define GROOVE_PCM_H

#define PCM_RATE 44100
/* A source hands out at most this many frames per read; readers pass room for at least this many. */
#define PCM_MAX_FRAMES 8192

typedef struct {
    short l, r;
} Frame; /* one stereo sample pair */

#endif
