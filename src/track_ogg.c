/* track_ogg.c — Ogg Vorbis source backed by stb_vorbis.
 *
 * stb_vorbis streams the first logical Vorbis stream from the file, decodes
 * straight to interleaved S16 (Vorbis has no native integer depth, so the
 * label omits a bit depth) and takes the exact granule length as the frame
 * count. Seeking is sample-accurate through stb_vorbis_seek64, which lines up
 * with the stb_vorbis_get_samples_* API used here. Only mono/stereo streams are
 * accepted, matching what the engine's stream conversion is guaranteed to
 * handle. Chained Ogg files stop at the first logical stream boundary, but the
 * granule length is read from the file's final page, so such files may report a
 * short duration and stop early.
 */
#include "track_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

struct Track {
    TrackBase base;
    stb_vorbis *v;
    int channels;
    Uint32 total_frames;
    int at_end;     /* seeking to/after EOF: decode returns 0 at once */
    int logged_err; /* one SDL_Log per stream for corrupt/chained input */
};

static int ogg_decode_chunk(Track *t, Uint8 *dst, int cap);
static int ogg_seek(Track *t, Uint32 frame);
static void ogg_close(Track *t);

static const TrackOps ogg_ops = {
    .decode_chunk = ogg_decode_chunk,
    .seek = ogg_seek,
    .close = ogg_close,
};

static Uint32
clamp_frames(stb_vorbis_int64 frames)
{
    return frames > (stb_vorbis_int64)0xffffffffu ? 0xffffffffu : (Uint32)frames;
}

Track *
track_ogg_open(const char *path, TrackInfo *info, char *err, size_t errlen)
{
    Track *t;
    stb_vorbis *v;
    stb_vorbis_info vi;
    stb_vorbis_int64 len;
    int verr = 0;
    char msg[256];

    if (!path || !info) {
        if (err && errlen)
            snprintf(err, errlen, "invalid arguments");
        return NULL;
    }

    v = stb_vorbis_open_filename(path, &verr, NULL);
    if (!v) {
        if (verr == VORBIS_file_open_failure)
            snprintf(msg, sizeof msg, "cannot open '%s'", path);
        else if (verr == VORBIS_outofmem)
            snprintf(msg, sizeof msg, "out of memory");
        else
            snprintf(msg, sizeof msg,
                     "cannot decode Ogg Vorbis '%s' (Vorbis streams only)", path);
        goto fail;
    }

    vi = stb_vorbis_get_info(v);
    if (vi.channels < 1 || vi.channels > 2) {
        snprintf(msg, sizeof msg,
                 "unsupported Ogg Vorbis channel count %d in '%s' (mono or stereo only)",
                 vi.channels, path);
        goto fail;
    }
    if (vi.sample_rate == 0) {
        snprintf(msg, sizeof msg,
                 "cannot decode Ogg Vorbis '%s' (Vorbis streams only)", path);
        goto fail;
    }

    /* A broken or missing final granule means the real duration is unknown;
     * reporting a made-up one would be worse than refusing the file. */
    len = stb_vorbis_stream_length_in_samples64(v);
    if (len <= 0) {
        snprintf(msg, sizeof msg, "cannot determine Ogg Vorbis length of '%s'", path);
        goto fail;
    }

    if (!stb_vorbis_seek_start(v)) {
        snprintf(msg, sizeof msg,
                 "cannot decode Ogg Vorbis '%s' (Vorbis streams only)", path);
        goto fail;
    }

    t = (Track *)calloc(1, sizeof *t);
    if (!t) {
        snprintf(msg, sizeof msg, "out of memory");
        goto fail;
    }

    t->v = v;
    t->channels = vi.channels;
    t->total_frames = clamp_frames(len);
    t->base.ops = &ogg_ops;

    memset(&info->src_spec, 0, sizeof info->src_spec);
    info->src_spec.format = AUDIO_S16SYS;
    info->src_spec.channels = (Uint8)vi.channels;
    info->src_spec.freq = (int)vi.sample_rate;
    info->total_frames = t->total_frames;
    info->codec = "Vorbis";
    info->bits_per_sample = 0;
    return t;

fail:
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    stb_vorbis_close(v);
    return NULL;
}

static int
ogg_decode_chunk(Track *t, Uint8 *dst, int cap)
{
    int bpf, frames, n;

    if (!t || !t->v || !dst)
        return -1;
    if (cap <= 0 || t->at_end)
        return 0;

    bpf = 2 * t->channels; /* bytes per interleaved S16 frame */
    frames = cap / bpf;    /* stb_vorbis wants a whole-frame capacity */
    if (frames <= 0)
        return -1;

    /* The argument is the buffer capacity in shorts; the return value is
     * samples per channel, which is why the byte count below uses bpf. */
    n = stb_vorbis_get_samples_short_interleaved(t->v, t->channels,
                                                 (short *)(void *)dst,
                                                 frames * t->channels);
    if (n == 0) {
        /* get_error clears the latch, so only ask once a decode came back
         * empty. A latched error means corruption, truncation or a chained
         * stream boundary; either way this ends the stream cleanly. */
        if (stb_vorbis_get_error(t->v) != VORBIS__no_error && !t->logged_err) {
            SDL_Log("Ogg Vorbis stream ended early (truncated, corrupt or chained?)");
            t->logged_err = 1;
        }
        t->at_end = 1;
        return 0;
    }
    return n * bpf;
}

static int
ogg_seek(Track *t, Uint32 frame)
{
    if (!t || !t->v)
        return -1;

    /* The engine seeks to exactly total_frames when the target is at or past
     * the end, and stb_vorbis rejects seeks past EOF: end the stream instead. */
    if (frame >= t->total_frames) {
        t->at_end = 1;
        return 0;
    }

    t->at_end = 0;
    return stb_vorbis_seek64(t->v, (stb_vorbis_int64)frame) ? 0 : -1;
}

static void
ogg_close(Track *t)
{
    if (!t)
        return;
    if (t->v)
        stb_vorbis_close(t->v);
    free(t);
}
