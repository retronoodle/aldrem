/* Track — player-agnostic audio source interface.
 * Phase 2 ships only the WAV implementation (track_wav.c); future decoders
 * (e.g. MP3) fill in the same contract.
 */
#ifndef TRACK_H
#define TRACK_H

#include <stddef.h>
#include <SDL.h>

typedef struct Track Track; /* opaque */

typedef struct {
    SDL_AudioSpec src_spec; /* format of the PCM returned by track_decode_chunk */
    Uint32 total_frames;    /* at src_spec.freq, for duration */
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

void track_close(Track *t);

#endif
