/* track_flac.c — FLAC source backed by dr_flac.
 *
 * dr_flac streams native FLAC from the file, takes the exact PCM frame count
 * from STREAMINFO, and decodes straight to interleaved S32: low bit depths are
 * left-shifted to full scale, so 16- and 24-bit sources stay lossless. Seeking
 * is sample-accurate through the seek table or dr_flac's binary search. Ogg
 * encapsulation is compiled out and only mono/stereo streams are accepted,
 * matching what the engine's stream conversion is guaranteed to handle.
 */
#include "track_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_FLAC_NO_OGG
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

struct Track {
    TrackBase base;
    drflac *flac;
};

static int flac_decode_chunk(Track *t, Uint8 *dst, int cap);
static int flac_seek(Track *t, Uint32 frame);
static void flac_close(Track *t);

static const TrackOps flac_ops = {
    .decode_chunk = flac_decode_chunk,
    .seek = flac_seek,
    .close = flac_close,
};

static Uint32
clamp_frames(drflac_uint64 frames)
{
    return frames > 0xffffffffu ? 0xffffffffu : (Uint32)frames;
}

Track *
track_flac_open(const char *path, TrackInfo *info, char *err, size_t errlen)
{
    Track *t;
    drflac *flac;
    FILE *probe;
    char msg[256];

    if (!path || !info) {
        if (err && errlen)
            snprintf(err, errlen, "invalid arguments");
        return NULL;
    }

    /* dr_flac returns NULL for both I/O and format failures, so touch the file
     * first to keep the two messages distinct. */
    probe = fopen(path, "rb");
    if (!probe) {
        if (err && errlen)
            snprintf(err, errlen, "cannot open '%s'", path);
        return NULL;
    }
    fclose(probe);

    flac = drflac_open_file(path, NULL);
    if (!flac) {
        if (err && errlen)
            snprintf(err, errlen, "cannot decode FLAC '%s'", path);
        return NULL;
    }

    if (flac->channels < 1 || flac->channels > 2) {
        snprintf(msg, sizeof msg,
                 "unsupported FLAC channel count %u in '%s' (mono or stereo only)",
                 (unsigned)flac->channels, path);
        goto fail;
    }
    if (flac->sampleRate == 0) {
        snprintf(msg, sizeof msg, "cannot decode FLAC '%s'", path);
        goto fail;
    }
    if (flac->totalPCMFrameCount == 0) {
        snprintf(msg, sizeof msg, "cannot determine FLAC length of '%s'", path);
        goto fail;
    }

    t = (Track *)calloc(1, sizeof *t);
    if (!t) {
        snprintf(msg, sizeof msg, "out of memory");
        goto fail;
    }

    t->flac = flac;
    t->base.ops = &flac_ops;

    memset(&info->src_spec, 0, sizeof info->src_spec);
    info->src_spec.format = AUDIO_S32SYS;
    info->src_spec.channels = flac->channels;
    info->src_spec.freq = (int)flac->sampleRate;
    info->total_frames = clamp_frames(flac->totalPCMFrameCount);
    info->codec = "FLAC";
    info->bits_per_sample = (int)flac->bitsPerSample;
    return t;

fail:
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    drflac_close(flac);
    return NULL;
}

static int
flac_decode_chunk(Track *t, Uint8 *dst, int cap)
{
    int bpf, frames;
    drflac_uint64 got;

    if (!t || !t->flac || !dst)
        return -1;
    if (cap <= 0)
        return 0;

    bpf = 4 * (int)t->flac->channels; /* bytes per interleaved S32 frame */
    frames = cap / bpf;               /* dr_flac returns only whole frames */
    if (frames <= 0)
        return -1;

    got = drflac_read_pcm_frames_s32(t->flac, (drflac_uint64)frames,
                                     (drflac_int32 *)(void *)dst);
    if (got == 0) {
        /* A damaged or truncated stream also stops here; note it, then let the
         * caller see a normal end of stream so it flushes and goes idle. */
        if (t->flac->currentPCMFrame < t->flac->totalPCMFrameCount)
            SDL_Log("FLAC stream ended early (truncated or corrupt?)");
        return 0;
    }
    return (int)(got * (drflac_uint64)bpf);
}

static int
flac_seek(Track *t, Uint32 frame)
{
    drflac_uint64 target;

    if (!t || !t->flac)
        return -1;

    /* dr_flac clamps as well; doing it here keeps the intent explicit. */
    target = frame > t->flac->totalPCMFrameCount
           ? t->flac->totalPCMFrameCount : frame;
    return drflac_seek_to_pcm_frame(t->flac, target) ? 0 : -1;
}

static void
flac_close(Track *t)
{
    if (!t)
        return;
    if (t->flac)
        drflac_close(t->flac);
    free(t);
}
