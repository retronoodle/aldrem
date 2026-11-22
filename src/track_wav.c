/* track_wav.c — WAV source: whole file decoded into memory by SDL_LoadWAV.
 * SDL_LoadWAV already normalises 24-bit PCM to 32-bit, so src_spec.format is
 * the authoritative format; block_align is derived from it, never from the
 * file header.
 */
#include "track.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Track {
    Uint8 *data;
    Uint32 len;
    Uint32 cursor;
    Uint32 block_align;
};

static int
format_supported(SDL_AudioFormat format)
{
    switch (format) {
    case AUDIO_U8:
    case AUDIO_S8:
    case AUDIO_U16LSB:
    case AUDIO_U16MSB:
    case AUDIO_S16LSB:
    case AUDIO_S16MSB:
    case AUDIO_S32LSB:
    case AUDIO_S32MSB:
    case AUDIO_F32LSB:
    case AUDIO_F32MSB:
        return 1;
    default:
        return 0;
    }
}

Track *
track_open(const char *path, TrackInfo *info, char *err, size_t errlen)
{
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    Uint32 block_align;
    Track *t;

    if (!path || !info) {
        if (err && errlen)
            snprintf(err, errlen, "invalid arguments");
        return NULL;
    }

    SDL_ClearError();
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) {
        if (err && errlen)
            snprintf(err, errlen, "cannot load '%s': %s", path, SDL_GetError());
        return NULL;
    }

    if (spec.channels == 0 || !format_supported(spec.format)) {
        if (err && errlen)
            snprintf(err, errlen, "unsupported WAV format in '%s'", path);
        SDL_FreeWAV(buf);
        return NULL;
    }

    block_align = (Uint32)(SDL_AUDIO_BITSIZE(spec.format) / 8) * spec.channels;
    if (block_align == 0) {
        if (err && errlen)
            snprintf(err, errlen, "unsupported WAV format in '%s'", path);
        SDL_FreeWAV(buf);
        return NULL;
    }

    t = (Track *)malloc(sizeof *t);
    if (!t) {
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        SDL_FreeWAV(buf);
        return NULL;
    }

    t->data = buf;
    t->len = len;
    t->cursor = 0;
    t->block_align = block_align;

    info->src_spec = spec;
    info->total_frames = len / block_align;
    return t;
}

int
track_decode_chunk(Track *t, Uint8 *dst, int cap)
{
    Uint32 remaining, want;

    if (!t || !dst)
        return -1;
    if (t->cursor >= t->len)
        return 0;
    if (cap <= 0)
        return 0;

    remaining = t->len - t->cursor;
    want = (Uint32)cap < remaining ? (Uint32)cap : remaining;
    want -= want % t->block_align; /* always whole frames */
    if (want == 0)
        return (Uint32)cap < t->block_align ? -1 : 0;

    memcpy(dst, t->data + t->cursor, want);
    t->cursor += want;
    return (int)want;
}

int
track_seek(Track *t, Uint32 frame)
{
    Uint32 total;

    if (!t)
        return -1;
    total = t->len / t->block_align;
    if (frame > total)
        frame = total;
    t->cursor = frame * t->block_align;
    return 0;
}

void
track_close(Track *t)
{
    if (!t)
        return;
    SDL_FreeWAV(t->data);
    free(t);
}
