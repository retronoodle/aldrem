/* Track — player-agnostic audio source interface.
 * Implementations (track_wav.c, track_mp3.c, track_flac.c, track_ogg.c,
 * track_aiff.c) fill in the same contract behind an opaque vtable.
 */
#ifndef TRACK_H
#define TRACK_H

#include <stddef.h>
#include <SDL.h>

typedef struct Track Track; /* opaque */

typedef struct {
    SDL_AudioSpec src_spec; /* format of the PCM returned by track_decode_chunk */
    Uint32 total_frames;    /* at src_spec.freq, for duration */
    const char *codec;      /* static string ("WAV"/"MP3"), owned by impl */
    int bits_per_sample;    /* decoded width, 0 when not meaningful (MP3) */
    int is_live;            /* 1 for a network stream: no length, no seeking */
} TrackInfo;

/* Opens path and fills info on success. Returns NULL on failure with a
 * human-readable reason in err. */
Track *track_open(const char *path, TrackInfo *info, char *err, size_t errlen);

/* Nonzero when name carries a supported audio extension. The single source of
 * truth for extension-based filtering, shared by the loader and the browser. */
int track_ext_supported(const char *name);

/* Copies the next chunk of source-format PCM into dst (max cap bytes).
 * Returns bytes written (always a whole number of frames), 0 at end of
 * stream, -1 on error. Advances the cursor. */
int track_decode_chunk(Track *t, Uint8 *dst, int cap);

/* Positions the cursor at frame (clamped to the end of the stream). */
int track_seek(Track *t, Uint32 frame);

/* Asks a decoder that may be blocked on the network to stop. Safe from any
 * thread. No-op for local files. */
void track_interrupt(Track *t);

/* Copies the current now-playing text of a live stream into out. Returns 1
 * when known, 0 when not (always 0 for local files). Safe from any thread. */
int track_title(Track *t, char *out, size_t cap);

void track_close(Track *t);

#endif
