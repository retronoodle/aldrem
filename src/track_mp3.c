/* track_mp3.c — MP3 source backed by minimp3's core decoder.
 *
 * The file is streamed in 32 KiB chunks and decoded one MPEG frame at a time,
 * so every PCM buffer handed to the caller is frame-aligned. Duration comes
 * from a CBR estimate or, for VBR, a header-only frame scan; seeking maps a
 * frame index to a byte offset through the average bitrate. This follows the
 * PRD's streaming + byte-offset design and deliberately skips minimp3_ex.
 */
#include "track_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define IN_BYTES      (32 * 1024)
#define IN_HEADROOM   4096  /* > MAX_FREE_FORMAT_FRAME_SIZE (2304) */
#define SAMPLE_FRAMES 64    /* frames sampled before deciding CBR vs VBR */
#define ID3V2_HDR     10
#define ID3V1_BYTES   128

struct Track {
    TrackBase base;

    FILE *fp;
    long file_size;
    long data_start; /* first audio byte (past ID3v2) */
    long data_end;   /* past the last audio byte (before ID3v1) */

    Uint32 hz;
    int channels;
    Uint32 total_frames;

    mp3dec_t dec;

    /* Compressed input. in_off is the first undecoded byte; a frame may need
     * up to MAX_FREE_FORMAT_FRAME_SIZE contiguous bytes, hence IN_HEADROOM. */
    Uint8 in[IN_BYTES];
    int in_len;
    int in_off;
    int at_eof;

    /* One decoded frame, interleaved S16. Counts are per channel. */
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcm_valid;
    int pcm_off;
};

static int mp3_decode_chunk(Track *t, Uint8 *dst, int cap);
static int mp3_seek(Track *t, Uint32 frame);
static void mp3_close(Track *t);

static const TrackOps mp3_ops = {
    .decode_chunk = mp3_decode_chunk,
    .seek = mp3_seek,
    .close = mp3_close,
};

static Uint32
clamp_frames(Uint64 samples)
{
    return samples > 0xffffffffu ? 0xffffffffu : (Uint32)samples;
}

/* Drops the decoder and buffered input without moving the file position. */
static void
reset_io(struct Track *t)
{
    mp3dec_init(&t->dec);
    t->in_len = 0;
    t->in_off = 0;
    t->at_eof = 0;
    t->pcm_valid = 0;
    t->pcm_off = 0;
}

static int
seek_bytes(struct Track *t, long off)
{
    if (fseek(t->fp, off, SEEK_SET) != 0)
        return -1;
    reset_io(t);
    return 0;
}

/* Compacts the consumed prefix and tops up from the file. Returns the number
 * of bytes read, 0 at EOF (or when the buffer is already full), -1 on a read
 * error. */
static int
in_refill(struct Track *t)
{
    size_t want, n;
    long pos, remain;

    if (t->in_off > 0) {
        if (t->in_off < t->in_len)
            memmove(t->in, t->in + t->in_off, (size_t)(t->in_len - t->in_off));
        t->in_len -= t->in_off;
        t->in_off = 0;
    }
    if (t->at_eof || t->in_len >= IN_BYTES)
        return 0;

    /* Stop at data_end: feeding the ID3v1 trailer to the decoder makes
     * minimp3 reject the final audio frame (it needs a matching header or an
     * exact fill). */
    pos = ftell(t->fp);
    if (pos < 0)
        return -1;
    remain = t->data_end - pos;
    if (remain <= 0) {
        t->at_eof = 1;
        return 0;
    }

    want = (size_t)(IN_BYTES - t->in_len);
    if ((long)want > remain)
        want = (size_t)remain;

    n = fread(t->in + t->in_len, 1, want, t->fp);
    if (n == 0) {
        if (ferror(t->fp))
            return -1;
        t->at_eof = 1;
        return 0;
    }
    t->in_len += (int)n;
    return (int)n;
}

/* Advances to the next decodable frame. PCM is decoded only when want_pcm is
 * set, in which case pcm/pcm_valid hold it on success. On success out (when
 * non-NULL) receives the frame header info. Returns the number of samples per
 * channel (> 0) on a frame, 0 at end of stream, -1 on error. */
static int
decode_next_frame(struct Track *t, int want_pcm, mp3dec_frame_info_t *out)
{
    for (;;) {
        mp3dec_frame_info_t fi;
        int avail, ret;

        /* Keep a full frame's worth of contiguous input so a frame is never
         * split across a refill boundary. */
        if (!t->at_eof && t->in_len - t->in_off < IN_HEADROOM) {
            if (in_refill(t) < 0)
                return -1;
        }

        avail = t->in_len - t->in_off;
        if (avail <= 0)
            return 0;

        memset(&fi, 0, sizeof fi);
        ret = mp3dec_decode_frame(&t->dec, t->in + t->in_off, avail,
                                  want_pcm ? t->pcm : NULL, &fi);

        /* frame_bytes is set even when nothing decoded: it is the number of
         * bytes skipped. Always advance or this loops forever. */
        if (fi.frame_bytes > 0) {
            t->in_off += fi.frame_bytes;
        } else {
            /* Truncated frame at EOF, or a header that cannot be completed
             * from what is buffered. */
            int got;

            if (t->at_eof)
                return 0;
            got = in_refill(t);
            if (got < 0)
                return -1;
            if (got == 0)
                return t->at_eof ? 0 : -1;
            continue;
        }

        if (ret <= 0)
            continue; /* garbage or a corrupt frame: resync */

        if (t->hz == 0) {
            t->hz = (Uint32)fi.hz;
            t->channels = fi.channels;
        } else if ((int)t->hz != fi.hz || t->channels != fi.channels) {
            SDL_Log("MP3 changes sample rate or channels mid-stream");
            return -1;
        }

        if (want_pcm) {
            t->pcm_valid = ret;
            t->pcm_off = 0;
        }
        if (out)
            *out = fi;
        return ret;
    }
}

typedef struct {
    Uint32 frames;
    Uint64 samples; /* samples per channel */
    int bitrate_min;
    int bitrate_max;
} HeaderScan;

/* Walks frames from the current position with header-only decodes, stopping
 * after limit frames (0 = until EOF). Returns 0 on success, -1 on error. */
static int
header_scan(struct Track *t, Uint32 limit, HeaderScan *out)
{
    memset(out, 0, sizeof *out);

    while (limit == 0 || out->frames < limit) {
        mp3dec_frame_info_t fi;
        int ret = decode_next_frame(t, 0, &fi);

        if (ret < 0)
            return -1;
        if (ret == 0)
            break;

        out->samples += (Uint32)ret;
        if (out->frames == 0) {
            out->bitrate_min = fi.bitrate_kbps;
            out->bitrate_max = fi.bitrate_kbps;
        } else {
            if (fi.bitrate_kbps < out->bitrate_min)
                out->bitrate_min = fi.bitrate_kbps;
            if (fi.bitrate_kbps > out->bitrate_max)
                out->bitrate_max = fi.bitrate_kbps;
        }
        out->frames++;
    }
    return 0;
}

Track *
track_mp3_open(const char *path, TrackInfo *info, char *err, size_t errlen)
{
    Track *t;
    HeaderScan scan;
    char msg[256];
    Uint8 hdr[ID3V2_HDR];
    Uint8 tail[ID3V1_BYTES];
    size_t n;
    int ret;

    if (!path || !info) {
        if (err && errlen)
            snprintf(err, errlen, "invalid arguments");
        return NULL;
    }

    t = (Track *)calloc(1, sizeof *t);
    if (!t) {
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        return NULL;
    }

    t->fp = fopen(path, "rb");
    if (!t->fp) {
        snprintf(msg, sizeof msg, "cannot open '%s'", path);
        goto fail;
    }

    if (fseek(t->fp, 0, SEEK_END) != 0) {
        snprintf(msg, sizeof msg, "cannot read '%s'", path);
        goto fail;
    }
    t->file_size = ftell(t->fp);
    if (t->file_size < 0 || fseek(t->fp, 0, SEEK_SET) != 0) {
        snprintf(msg, sizeof msg, "cannot read '%s'", path);
        goto fail;
    }

    /* ID3v2: syncsafe size in header bytes 6..9, optional 10-byte footer. */
    t->data_start = 0;
    n = fread(hdr, 1, sizeof hdr, t->fp);
    if (n == sizeof hdr && memcmp(hdr, "ID3", 3) == 0) {
        long size = ((long)(hdr[6] & 0x7f) << 21)
                  | ((long)(hdr[7] & 0x7f) << 14)
                  | ((long)(hdr[8] & 0x7f) << 7)
                  |  (long)(hdr[9] & 0x7f);

        t->data_start = ID3V2_HDR + size;
        if (hdr[5] & 0x10)
            t->data_start += ID3V2_HDR;
        if (t->data_start > t->file_size)
            t->data_start = t->file_size;
    } else if (ferror(t->fp)) {
        snprintf(msg, sizeof msg, "cannot read '%s'", path);
        goto fail;
    }

    /* ID3v1 is a fixed 128-byte trailer. APEv2 is out of scope. */
    t->data_end = t->file_size;
    if (t->file_size >= ID3V1_BYTES) {
        if (fseek(t->fp, t->file_size - ID3V1_BYTES, SEEK_SET) != 0) {
            snprintf(msg, sizeof msg, "cannot read '%s'", path);
            goto fail;
        }
        n = fread(tail, 1, sizeof tail, t->fp);
        if (n == sizeof tail && memcmp(tail, "TAG", 3) == 0)
            t->data_end = t->file_size - ID3V1_BYTES;
        else if (ferror(t->fp)) {
            snprintf(msg, sizeof msg, "cannot read '%s'", path);
            goto fail;
        }
    }

    if (t->data_end <= t->data_start) {
        snprintf(msg, sizeof msg, "cannot decode MP3 '%s'", path);
        goto fail;
    }

    /* First frame establishes the source format. */
    if (seek_bytes(t, t->data_start) != 0) {
        snprintf(msg, sizeof msg, "cannot read '%s'", path);
        goto fail;
    }
    ret = decode_next_frame(t, 1, NULL);
    if (ret <= 0 || (t->channels != 1 && t->channels != 2)
        || t->hz < 8000 || t->hz > 48000) {
        snprintf(msg, sizeof msg, "cannot decode MP3 '%s'", path);
        goto fail;
    }

    /* Duration: sample a few frames. A short file is already fully counted,
     * constant bitrate gives a byte-length estimate, otherwise scan it all. */
    if (seek_bytes(t, t->data_start) != 0) {
        snprintf(msg, sizeof msg, "cannot read '%s'", path);
        goto fail;
    }
    if (header_scan(t, SAMPLE_FRAMES, &scan) != 0) {
        snprintf(msg, sizeof msg, "cannot decode MP3 '%s'", path);
        goto fail;
    }

    if (scan.frames < SAMPLE_FRAMES) {
        t->total_frames = clamp_frames(scan.samples);
    } else if (scan.bitrate_min == scan.bitrate_max && scan.bitrate_min > 0) {
        double seconds = (double)(t->data_end - t->data_start) * 8.0
                       / ((double)scan.bitrate_min * 1000.0);
        double frames = seconds * (double)t->hz + 0.5;

        t->total_frames = frames > 4294967295.0 ? 0xffffffffu : (Uint32)frames;
    } else {
        if (seek_bytes(t, t->data_start) != 0) {
            snprintf(msg, sizeof msg, "cannot read '%s'", path);
            goto fail;
        }
        if (header_scan(t, 0, &scan) != 0) {
            snprintf(msg, sizeof msg, "cannot decode MP3 '%s'", path);
            goto fail;
        }
        t->total_frames = clamp_frames(scan.samples);
    }

    if (t->total_frames == 0) {
        snprintf(msg, sizeof msg, "cannot decode MP3 '%s'", path);
        goto fail;
    }

    /* Leave the stream ready for the first decode. */
    if (seek_bytes(t, t->data_start) != 0) {
        snprintf(msg, sizeof msg, "cannot read '%s'", path);
        goto fail;
    }

    t->base.ops = &mp3_ops;
    memset(&info->src_spec, 0, sizeof info->src_spec);
    info->src_spec.format = AUDIO_S16SYS;
    info->src_spec.channels = (Uint8)t->channels;
    info->src_spec.freq = (int)t->hz;
    info->total_frames = t->total_frames;
    return t;

fail:
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    if (t->fp)
        fclose(t->fp);
    free(t);
    return NULL;
}

static int
mp3_decode_chunk(Track *t, Uint8 *dst, int cap)
{
    int bpf, written = 0; /* bpf: bytes per S16 frame, interleaved */

    if (!t || !dst || t->channels <= 0)
        return -1;
    if (cap <= 0)
        return 0;

    bpf = 2 * t->channels;

    while (written < cap) {
        int want, avail, frames;

        if (t->pcm_off >= t->pcm_valid) {
            int ret = decode_next_frame(t, 1, NULL);

            if (ret < 0)
                return -1;
            if (ret == 0)
                return written;
        }

        want = (cap - written) / bpf; /* whole frames that still fit */
        if (want <= 0)
            return written > 0 ? written : -1;

        avail = t->pcm_valid - t->pcm_off;
        frames = avail < want ? avail : want;
        memcpy(dst + written,
               t->pcm + (size_t)t->pcm_off * (size_t)t->channels,
               (size_t)frames * (size_t)bpf);
        t->pcm_off += frames;
        written += frames * bpf;
    }
    return written;
}

static int
mp3_seek(Track *t, Uint32 frame)
{
    long offset;

    if (!t)
        return -1;

    if (t->total_frames == 0 || frame >= t->total_frames) {
        offset = t->data_end - 1; /* end of stream: the next decode EOFs */
    } else {
        long span = t->data_end - t->data_start;

        offset = t->data_start
               + (long)((double)span * (double)frame / (double)t->total_frames);
        if (offset < t->data_start)
            offset = t->data_start;
        if (offset >= t->data_end)
            offset = t->data_end - 1;
    }
    return seek_bytes(t, offset);
}

static void
mp3_close(Track *t)
{
    if (!t)
        return;
    if (t->fp)
        fclose(t->fp);
    free(t);
}
