/* track_aiff.c — AIFF/AIFC source parsed and decoded into memory here.
 *
 * An AIFF file is a FORM container of big-endian chunks: COMM describes the
 * channels, sample frames, bit depth and 80-bit extended sample rate, and
 * SSND carries the PCM. AIFC adds a 4-character compression type: NONE/twos
 * mean big-endian PCM and sowt means little-endian. Only integer PCM is
 * handled. 8-bit samples are signed (unlike WAV), and 24-bit samples are
 * widened to native S32 because SDL has no 24-bit format. block_align is
 * derived from src_spec, never from the COMM/SSND headers.
 */
#include "track_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Track {
    TrackBase base;
    Uint8 *data;
    Uint32 len;
    Uint32 cursor;
    Uint32 block_align;
};

static int aiff_decode_chunk(Track *t, Uint8 *dst, int cap);
static int aiff_seek(Track *t, Uint32 frame);
static void aiff_close(Track *t);

static const TrackOps aiff_ops = {
    .decode_chunk = aiff_decode_chunk,
    .seek = aiff_seek,
    .close = aiff_close,
};

static Uint32
be16(const Uint8 *p)
{
    return ((Uint32)p[0] << 8) | (Uint32)p[1];
}

static Uint32
be32(const Uint8 *p)
{
    return ((Uint32)p[0] << 24) | ((Uint32)p[1] << 16) | ((Uint32)p[2] << 8)
         | (Uint32)p[3];
}

/* AIFF sample rates are 80-bit IEEE extended: sign, 15-bit exponent biased by
 * 16383, then a 64-bit mantissa with an explicit integer bit. */
static int
parse_extended80(const Uint8 *p, double *out)
{
    Uint32 se = be16(p);
    Uint32 hi = be32(p + 2);
    Uint32 lo = be32(p + 6);
    double mantissa, value;

    if (se & 0x8000)                    /* sample rates are never negative */
        return 0;
    if ((se & 0x7fff) == 0 && hi == 0 && lo == 0)
        return 0;                       /* zero */
    if ((se & 0x7fff) == 0x7fff)
        return 0;                       /* infinity or NaN */

    mantissa = (double)hi * 4294967296.0 + (double)lo;
    value = ldexp(mantissa, (int)(se & 0x7fff) - 16383 - 63);
    if (!(value > 0.0) || value >= 1e9) /* non-finite or absurd */
        return 0;
    *out = value;
    return 1;
}

static int
read_exact(FILE *fp, void *buf, size_t len)
{
    return len == 0 || fread(buf, 1, len, fp) == len;
}

Track *
track_aiff_open(const char *path, TrackInfo *info, char *err, size_t errlen)
{
    FILE *fp = NULL;
    Track *t = NULL;
    Uint8 *buf = NULL;
    Uint8 hdr[12];
    long fsize = 0, pos, data_pos = 0, data_declared = 0, data_avail;
    Uint32 num_frames = 0, frames;
    unsigned channels = 0, sample_size = 0, frame_bytes_src;
    double rate = 0.0;
    char comp[5] = {0};
    int is_aifc = 0, is_le = 0, have_comm = 0, have_ssnd = 0;
    int rate_i;
    SDL_AudioFormat fmt;
    Uint32 block_align;
    char msg[256];

    if (!path || !info) {
        if (err && errlen)
            snprintf(err, errlen, "invalid arguments");
        return NULL;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        if (err && errlen)
            snprintf(err, errlen, "cannot open '%s'", path);
        return NULL;
    }

    /* The chunk table is walked by offset, so the file must be seekable. */
    if (fseek(fp, 0, SEEK_END) != 0 || (fsize = ftell(fp)) < 0
        || fseek(fp, 0, SEEK_SET) != 0) {
        snprintf(msg, sizeof msg, "cannot decode AIFF '%s'", path);
        goto fail;
    }

    if (!read_exact(fp, hdr, sizeof hdr) || memcmp(hdr, "FORM", 4) != 0) {
        snprintf(msg, sizeof msg, "cannot decode AIFF '%s'", path);
        goto fail;
    }
    if (memcmp(hdr + 8, "AIFF", 4) == 0)
        is_aifc = 0;
    else if (memcmp(hdr + 8, "AIFC", 4) == 0)
        is_aifc = 1;
    else {
        snprintf(msg, sizeof msg, "cannot decode AIFF '%s'", path);
        goto fail;
    }

    /* Pass 1: walk the chunk headers and record where COMM and SSND live, so
     * chunk order does not matter and FORM's size field is never trusted. */
    pos = 12;
    while (pos + 8 <= fsize) {
        Uint8 chdr[8];
        Uint32 csize;
        long payload, avail;

        if (fseek(fp, pos, SEEK_SET) != 0 || !read_exact(fp, chdr, 8))
            break;
        csize = be32(chdr + 4);
        payload = pos + 8;
        avail = fsize - payload;

        if (memcmp(chdr, "COMM", 4) == 0 && !have_comm) {
            Uint8 c[22];
            size_t want;

            if (csize < 18 || (is_aifc && csize < 22)) {
                snprintf(msg, sizeof msg,
                         "cannot decode AIFF '%s' (short COMM chunk)", path);
                goto fail;
            }
            want = csize < 22 ? (size_t)csize : sizeof c;
            if (!read_exact(fp, c, want)) {
                snprintf(msg, sizeof msg,
                         "cannot decode AIFF '%s' (short COMM chunk)", path);
                goto fail;
            }
            channels = (unsigned)be16(c);
            num_frames = be32(c + 2);
            sample_size = (unsigned)be16(c + 6);
            if (!parse_extended80(c + 8, &rate)) {
                snprintf(msg, sizeof msg, "cannot decode AIFF '%s'", path);
                goto fail;
            }
            if (is_aifc) {
                memcpy(comp, c + 18, 4);
                comp[4] = '\0';
            }
            have_comm = 1;
        } else if (memcmp(chdr, "SSND", 4) == 0 && !have_ssnd) {
            Uint8 s[8];
            Uint32 off;

            if (csize < 8) {
                snprintf(msg, sizeof msg,
                         "cannot decode AIFF '%s' (short SSND chunk)", path);
                goto fail;
            }
            if (!read_exact(fp, s, 8)) {
                snprintf(msg, sizeof msg,
                         "cannot decode AIFF '%s' (short SSND chunk)", path);
                goto fail;
            }
            off = be32(s);
            if (off > csize - 8) {
                snprintf(msg, sizeof msg,
                         "cannot decode AIFF '%s' (bad SSND offset)", path);
                goto fail;
            }
            data_pos = payload + 8 + (long)off;
            data_declared = (long)(csize - 8 - off);
            have_ssnd = 1;
        }

        /* A chunk that claims more than the file holds cannot be followed; an
         * overrunning SSND is reported as truncation below. Always advance at
         * least the 8-byte header, so zero-sized chunks cannot loop. */
        if ((unsigned long)csize > (unsigned long)avail)
            break;
        pos = payload + (long)csize + (long)(csize & 1u);
    }

    if (!have_comm || !have_ssnd) {
        snprintf(msg, sizeof msg, "cannot decode AIFF '%s' (%s chunk missing)", path,
                 have_comm ? "SSND" : "COMM");
        goto fail;
    }

    if (is_aifc) {
        if (memcmp(comp, "NONE", 4) == 0 || memcmp(comp, "twos", 4) == 0)
            is_le = 0;
        else if (memcmp(comp, "sowt", 4) == 0)
            is_le = 1;
        else {
            snprintf(msg, sizeof msg,
                     "unsupported AIFC compression '%s' in '%s'", comp, path);
            goto fail;
        }
    }

    switch (sample_size) {
    case 8:
        fmt = AUDIO_S8; /* AIFF 8-bit PCM is signed, unlike WAV's U8 */
        break;
    case 16:
        fmt = is_le ? AUDIO_S16LSB : AUDIO_S16MSB;
        break;
    case 24:
        fmt = AUDIO_S32SYS; /* widened to full-scale S32 below */
        break;
    case 32:
        fmt = is_le ? AUDIO_S32LSB : AUDIO_S32MSB;
        break;
    default:
        snprintf(msg, sizeof msg,
                 "unsupported AIFF bit depth %u in '%s'", sample_size, path);
        goto fail;
    }

    if (channels < 1 || channels > 2) {
        snprintf(msg, sizeof msg,
                 "unsupported AIFF channel count %u in '%s' (mono or stereo only)",
                 channels, path);
        goto fail;
    }

    rate_i = (int)(rate + 0.5);
    if (rate_i < 1 || rate_i > 384000) {
        snprintf(msg, sizeof msg, "unsupported AIFF format in '%s'", path);
        goto fail;
    }

    if (data_pos < 0 || data_pos > fsize) {
        snprintf(msg, sizeof msg, "truncated AIFF '%s'", path);
        goto fail;
    }
    data_avail = fsize - data_pos;
    if (data_declared > data_avail) {
        snprintf(msg, sizeof msg, "truncated AIFF '%s'", path);
        goto fail;
    }

    block_align = (Uint32)(SDL_AUDIO_BITSIZE(fmt) / 8) * channels;
    frame_bytes_src = (sample_size / 8u) * channels;

    {
        unsigned long long derived =
            (unsigned long long)data_declared / frame_bytes_src;

        frames = derived > 0xffffffffull ? 0xffffffffu : (Uint32)derived;
    }
    if (frames > 0xffffffffu / block_align)
        frames = 0xffffffffu / block_align; /* len is Uint32 */
    if (num_frames != 0) {
        if (num_frames > frames)
            SDL_Log("AIFF '%s' declares %u frames but SSND holds %u; using the data",
                    path, (unsigned)num_frames, (unsigned)frames);
        else
            frames = num_frames;
    }
    if (frames == 0) {
        snprintf(msg, sizeof msg, "cannot determine AIFF length of '%s'", path);
        goto fail;
    }

    buf = (Uint8 *)malloc((size_t)frames * block_align);
    if (!buf) {
        snprintf(msg, sizeof msg, "out of memory");
        goto fail;
    }

    if (fseek(fp, data_pos, SEEK_SET) != 0) {
        snprintf(msg, sizeof msg, "cannot decode AIFF '%s'", path);
        goto fail;
    }

    if (sample_size == 24) {
        Uint8 tmp[4096 * 3 * 2];
        Uint32 done = 0;

        while (done < frames) {
            Uint32 n = frames - done;
            Uint32 i, j;

            if (n > 4096u)
                n = 4096u;
            if (!read_exact(fp, tmp, (size_t)n * frame_bytes_src)) {
                snprintf(msg, sizeof msg, "truncated AIFF '%s'", path);
                goto fail;
            }
            for (i = 0; i < n; i++) {
                for (j = 0; j < channels; j++) {
                    const Uint8 *p = tmp + ((size_t)i * channels + j) * 3;
                    Uint32 v;

                    if (is_le)
                        v = (Uint32)p[0] | ((Uint32)p[1] << 8)
                          | ((Uint32)p[2] << 16);
                    else
                        v = ((Uint32)p[0] << 16) | ((Uint32)p[1] << 8)
                          | (Uint32)p[2];
                    if (v & 0x800000u) /* unsigned sign extension */
                        v |= 0xff000000u;
                    v <<= 8;           /* full scale in S32 */
                    memcpy(buf + ((size_t)done + i) * block_align
                             + (size_t)j * 4, &v, sizeof v);
                }
            }
            done += n;
        }
    } else {
        size_t remaining = (size_t)frames * block_align;
        size_t done = 0;

        while (remaining > 0) {
            size_t want = remaining < 65536u ? remaining : 65536u;

            if (!read_exact(fp, buf + done, want)) {
                snprintf(msg, sizeof msg, "truncated AIFF '%s'", path);
                goto fail;
            }
            done += want;
            remaining -= want;
        }
    }

    t = (Track *)calloc(1, sizeof *t);
    if (!t) {
        snprintf(msg, sizeof msg, "out of memory");
        goto fail;
    }

    t->data = buf;
    t->len = frames * block_align;
    t->cursor = 0;
    t->block_align = block_align;
    t->base.ops = &aiff_ops;

    memset(&info->src_spec, 0, sizeof info->src_spec);
    info->src_spec.format = fmt;
    info->src_spec.channels = (Uint8)channels;
    info->src_spec.freq = rate_i;
    info->total_frames = frames;
    info->codec = is_aifc ? "AIFC" : "AIFF";
    info->bits_per_sample = (int)sample_size;

    fclose(fp);
    return t;

fail:
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    free(buf);
    free(t);
    if (fp)
        fclose(fp);
    return NULL;
}

static int
aiff_decode_chunk(Track *t, Uint8 *dst, int cap)
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

static int
aiff_seek(Track *t, Uint32 frame)
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

static void
aiff_close(Track *t)
{
    if (!t)
        return;
    free(t->data);
    free(t);
}
