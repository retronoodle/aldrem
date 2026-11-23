/* track_internal.h — plumbing shared by the track dispatcher and the
 * per-format implementations. Not part of the public API.
 */
#ifndef TRACK_INTERNAL_H
#define TRACK_INTERNAL_H

#include "track.h"

/* Per-format implementation entry points. Each implementation stores a
 * pointer to its ops as the first member of its own struct Track. */
typedef struct TrackOps {
    int  (*decode_chunk)(Track *, Uint8 *, int);
    int  (*seek)(Track *, Uint32);
    void (*close)(Track *);
    void (*interrupt)(Track *); /* optional; NULL for local files */
    int  (*title)(Track *, char *, size_t); /* optional; NULL unless live */
} TrackOps;

typedef struct {
    const TrackOps *ops;
} TrackBase;

Track *track_wav_open(const char *path, TrackInfo *info, char *err, size_t errlen);
Track *track_mp3_open(const char *path, TrackInfo *info, char *err, size_t errlen);
Track *track_flac_open(const char *path, TrackInfo *info, char *err, size_t errlen);
Track *track_ogg_open(const char *path, TrackInfo *info, char *err, size_t errlen);
Track *track_aiff_open(const char *path, TrackInfo *info, char *err, size_t errlen);

/* Live http(s) source in track_stream.c. */
Track *track_stream_open(const char *url, TrackInfo *info, char *err, size_t errlen);

#endif
