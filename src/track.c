/* track.c — format detection and dispatch to the per-format implementation.
 * The public API lives here; the implementations only fill a TrackOps vtable.
 */
#include "track_internal.h"

#include <stdio.h>
#include <string.h>

enum { SNIFF_UNKNOWN, SNIFF_WAV, SNIFF_MP3, SNIFF_FLAC, SNIFF_OGG, SNIFF_AIFF,
       SNIFF_IOERR };

static const TrackOps *
ops_of(Track *t)
{
    return t ? ((TrackBase *)t)->ops : NULL;
}

int
track_decode_chunk(Track *t, Uint8 *dst, int cap)
{
    const TrackOps *ops = ops_of(t);

    if (!ops || !ops->decode_chunk)
        return -1;
    return ops->decode_chunk(t, dst, cap);
}

int
track_seek(Track *t, Uint32 frame)
{
    const TrackOps *ops = ops_of(t);

    if (!ops || !ops->seek)
        return -1;
    return ops->seek(t, frame);
}

void
track_interrupt(Track *t)
{
    const TrackOps *ops = ops_of(t);

    if (ops && ops->interrupt)
        ops->interrupt(t);
}

int
track_title(Track *t, char *out, size_t cap)
{
    const TrackOps *ops = ops_of(t);

    if (out && cap)
        out[0] = '\0';
    if (!ops || !ops->title || !out || !cap)
        return 0;
    return ops->title(t, out, cap);
}

void
track_close(Track *t)
{
    const TrackOps *ops = ops_of(t);

    if (ops && ops->close)
        ops->close(t);
}

/* Case-insensitive comparison of the part after the last '.' in the path
 * (which must be in the basename). No POSIX strcasecmp: this stays portable. */
static int
ext_is(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    size_t i;

    if (!dot || (slash && dot < slash))
        return 0;
    for (i = 0; ext[i]; i++) {
        char c = dot[i + 1];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != ext[i])
            return 0;
    }
    return dot[i + 1] == '\0';
}

int
track_ext_supported(const char *name)
{
    if (!name)
        return 0;
    return ext_is(name, "wav") || ext_is(name, "mp3") || ext_is(name, "flac")
        || ext_is(name, "ogg") || ext_is(name, "oga") || ext_is(name, "aiff")
        || ext_is(name, "aif") || ext_is(name, "aifc");
}

/* Reads the first bytes to identify a file whose extension says nothing. */
static int
sniff(const char *path)
{
    Uint8 buf[12];
    size_t n;
    int failed;
    FILE *fp = fopen(path, "rb");

    if (!fp)
        return SNIFF_IOERR;
    n = fread(buf, 1, sizeof buf, fp);
    failed = ferror(fp);
    fclose(fp);
    if (failed)
        return SNIFF_IOERR;

    if (n >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0)
        return SNIFF_WAV;

    if (n >= 12 && memcmp(buf, "FORM", 4) == 0
        && (memcmp(buf + 8, "AIFF", 4) == 0
            || memcmp(buf + 8, "AIFC", 4) == 0))
        return SNIFF_AIFF;

    if (n >= 4 && memcmp(buf, "fLaC", 4) == 0)
        return SNIFF_FLAC;

    if (n >= 4 && memcmp(buf, "OggS", 4) == 0)
        return SNIFF_OGG;

    if (n >= 3 && memcmp(buf, "ID3", 3) == 0)
        return SNIFF_MP3;

    /* MPEG audio sync: 11 set bits, no reserved version or layer. */
    if (n >= 2 && buf[0] == 0xff && (buf[1] & 0xe0) == 0xe0
        && (buf[1] & 0x18) != 0x08 && (buf[1] & 0x06) != 0x00)
        return SNIFF_MP3;

    return SNIFF_UNKNOWN;
}

Track *
track_open(const char *path, TrackInfo *info, char *err, size_t errlen)
{
    if (!path || !info) {
        if (err && errlen)
            snprintf(err, errlen, "invalid arguments");
        return NULL;
    }

    /* Zero so a decoder that sets few fields cannot leak uninitialised data. */
    memset(info, 0, sizeof *info);

    /* Network streams are identified by scheme, not extension. */
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0)
        return track_stream_open(path, info, err, errlen);

    /* A known extension is authoritative: no cross-format fallback, so a
     * mislabelled file fails loudly instead of decoding as something else. */
    if (ext_is(path, "wav"))
        return track_wav_open(path, info, err, errlen);
    if (ext_is(path, "mp3"))
        return track_mp3_open(path, info, err, errlen);
    if (ext_is(path, "flac"))
        return track_flac_open(path, info, err, errlen);
    if (ext_is(path, "ogg") || ext_is(path, "oga"))
        return track_ogg_open(path, info, err, errlen);
    if (ext_is(path, "aiff") || ext_is(path, "aif") || ext_is(path, "aifc"))
        return track_aiff_open(path, info, err, errlen);

    switch (sniff(path)) {
    case SNIFF_WAV:
        return track_wav_open(path, info, err, errlen);
    case SNIFF_MP3:
        return track_mp3_open(path, info, err, errlen);
    case SNIFF_FLAC:
        return track_flac_open(path, info, err, errlen);
    case SNIFF_OGG:
        return track_ogg_open(path, info, err, errlen);
    case SNIFF_AIFF:
        return track_aiff_open(path, info, err, errlen);
    case SNIFF_IOERR:
        if (err && errlen)
            snprintf(err, errlen, "cannot open '%s'", path);
        return NULL;
    default:
        if (err && errlen)
            snprintf(err, errlen,
                     "unsupported file type '%s': expected .wav, .mp3, .flac, .ogg, .oga, .aiff, .aif or .aifc",
                     path);
        return NULL;
    }
}
