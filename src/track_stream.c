/* track_stream.c — live HTTP(S) radio source.
 *
 * A stream track has no length and cannot seek. Bytes come from libcurl
 * (net.c) through a StreamIO, are decoded one block at a time (MP3 frame, Ogg
 * packet, AAC frame) into a pending buffer, and handed to the engine.
 *
 * Failure handling: when the connection drops or a read fails, the decoder
 * reconnects up to RECONNECT_TRIES times with backoff before giving up. A new
 * connection starts the stream from the beginning, so the codec state is reset
 * and rebuilt from the fresh bytes.
 *
 * Stop: track_interrupt() flags the track and aborts the socket, so a decoder
 * blocked on the network returns promptly. The next start calls track_seek(0),
 * which reconnects, because the old connection was consumed or aborted.
 */
#include "track_stream_internal.h"

#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declarations only: the implementation lives in track_mp3.c. */
#include "minimp3.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define IN_CAP          (64 * 1024)
/* minimp3 discards a short buffer whole while it hunts for a sync it cannot
 * confirm, so MP3 keeps this much in hand while the connection is alive. */
#define MP3_LOOKAHEAD   (16 * 1024)
#define PEND_CAP        (256 * 1024)
#define SNIFF_BYTES     12
#define RECONNECT_TRIES 3
#define BACKOFF_MS      500
#define AAC_PRIME       6    /* AAC frames decoded at open, so HE-AAC's final
                              * sample rate is known before playback starts */

struct Track {
    TrackBase base;
    StreamIO io;
    StreamCodec codec;

    /* Negotiated format. Written by the decoder thread, read by the UI thread
     * only at open, before the thread exists. */
    int hz;
    int channels;

    SDL_atomic_t interrupted; /* set by track_interrupt, cleared on restart */
    SDL_atomic_t dirty;       /* PCM handed out since the last (re)start */
    int failed;               /* decoder gave up; decode returns -1 */
    int opening;              /* no reconnects while the first blocks decode */
    int src_failed;           /* a read failed; reconnect once input is used up */

    Uint8 *in;                /* compressed input, IN_CAP bytes */
    int in_len;
    int in_off;

    Uint8 *pend;              /* decoded PCM not yet handed out, whole frames */
    int pend_len;
    int pend_off;

    mp3dec_t mp3;
    mp3d_sample_t mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    stb_vorbis *ogg;
    AacDec *aac;
};

static int stream_decode_chunk(Track *t, Uint8 *dst, int cap);
static int stream_seek(Track *t, Uint32 frame);
static void stream_interrupt(Track *t);
static void stream_close(Track *t);

static const TrackOps stream_ops = {
    .decode_chunk = stream_decode_chunk,
    .seek = stream_seek,
    .close = stream_close,
    .interrupt = stream_interrupt,
};

static int
interrupted(const Track *t)
{
    return SDL_AtomicGet((SDL_atomic_t *)&t->interrupted);
}

/* Sleeps for ms, waking early on interrupt so stop never waits out a backoff. */
static void
sleep_unless_interrupted(Track *t, int ms)
{
    while (ms > 0 && !interrupted(t)) {
        int step = ms < 50 ? ms : 50;

        SDL_Delay((Uint32)step);
        ms -= step;
    }
}

static int
bytes_per_frame(const Track *t)
{
    return t->channels * (t->codec == STREAM_MP3 ? 2 : 4);
}

/* Drops codec state without touching pending PCM, so the next block decodes
 * from the fresh connection. */
static void
reset_codec(Track *t)
{
    if (t->codec == STREAM_MP3)
        mp3dec_init(&t->mp3);
    if (t->ogg) {
        stb_vorbis_close(t->ogg);
        t->ogg = NULL;
    }
    if (t->aac)
        aac_dec_reset(t->aac);
    t->in_len = 0;
    t->in_off = 0;
    t->src_failed = 0;
}

/* Reconnects with backoff. Returns 0 on success. */
static int
reconnect(Track *t)
{
    int attempt;

    for (attempt = 0; attempt < RECONNECT_TRIES; attempt++) {
        if (attempt > 0)
            sleep_unless_interrupted(t, BACKOFF_MS * attempt);
        if (interrupted(t))
            return -1;
        if (t->io.reconnect(t->io.ud) == 0) {
            reset_codec(t);
            return 0;
        }
    }
    return -1;
}

/* Compacts the input buffer and reads once from the source, without
 * reconnecting. Returns 0 when bytes were added, -1 on a failed read or a full
 * buffer. */
static int
read_more(Track *t)
{
    int n;

    if (t->in_off > 0) {
        if (t->in_off < t->in_len)
            memmove(t->in, t->in + t->in_off, (size_t)(t->in_len - t->in_off));
        t->in_len -= t->in_off;
        t->in_off = 0;
    }
    if (t->in_len >= IN_CAP)
        return -1; /* a full window with no frame in it: not decodable */

    n = t->io.read(t->io.ud, t->in + t->in_len, IN_CAP - t->in_len);
    if (n <= 0)
        return -1;
    t->in_len += n;
    return 0;
}

/* Makes at least one more byte available, reconnecting when the connection
 * drops (unless interrupted or still opening). Returns 0 on success. */
static int
fill(Track *t)
{
    for (;;) {
        if (read_more(t) == 0)
            return 0;
        if (interrupted(t) || t->opening)
            return -1;
        if (reconnect(t) != 0)
            return -1;
    }
}

/* MP3: one frame per call. Keeps MP3_LOOKAHEAD bytes in hand while the
 * connection is alive. Once it has failed, decodes what is left, and only then
 * reconnects, so audio already received is never thrown away. Resyncs over
 * garbage like track_mp3.c. */
static int
mp3_block(Track *t)
{
    for (;;) {
        mp3dec_frame_info_t fi;
        int avail = t->in_len - t->in_off;
        int ret, bytes;

        if (avail < MP3_LOOKAHEAD && !t->src_failed) {
            if (read_more(t) != 0)
                t->src_failed = 1;
            continue;
        }

        if (avail > 0) {
            memset(&fi, 0, sizeof fi);
            ret = mp3dec_decode_frame(&t->mp3, t->in + t->in_off, avail,
                                      t->mp3_pcm, &fi);
            if (fi.frame_bytes > 0) {
                /* Frame_bytes is set even when nothing decoded: it is the
                 * number of bytes consumed or skipped. */
                t->in_off += fi.frame_bytes;
                if (ret <= 0)
                    continue; /* ID3 tag, garbage or a corrupt frame */

                if (t->hz == 0 || t->opening) {
                    if (t->hz != 0 && t->channels != fi.channels)
                        return -1;
                    t->hz = fi.hz;
                    t->channels = fi.channels;
                } else if (t->hz != fi.hz || t->channels != fi.channels) {
                    SDL_Log("MP3 stream changes sample rate or channels mid-stream");
                    return -1;
                }

                bytes = ret * t->channels * 2;
                if (t->pend_len + bytes > PEND_CAP)
                    return -1;
                memcpy(t->pend + t->pend_len, t->mp3_pcm, (size_t)bytes);
                t->pend_len += bytes;
                return bytes;
            }
        }

        /* Nothing decodable is buffered. */
        if (t->src_failed) {
            if (t->opening || interrupted(t) || reconnect(t) != 0)
                return -1;
            continue;
        }
        if (read_more(t) != 0)
            t->src_failed = 1;
    }
}

/* Ogg Vorbis via the push API. Opens the stream's headers lazily, so a
 * reconnect re-reads them from the new connection. */
static int
ogg_block(Track *t)
{
    for (;;) {
        int avail = t->in_len - t->in_off;

        if (!t->ogg) {
            stb_vorbis_info vi;
            stb_vorbis *v;
            int used = 0, err = 0;

            if (avail <= 0) {
                if (fill(t) != 0)
                    return -1;
                continue;
            }
            v = stb_vorbis_open_pushdata(t->in + t->in_off, avail, &used, &err, NULL);
            if (!v) {
                if (err == VORBIS_need_more_data) {
                    if (fill(t) != 0)
                        return -1;
                    continue;
                }
                return -1; /* not Vorbis */
            }
            t->in_off += used;
            vi = stb_vorbis_get_info(v);
            if (vi.channels < 1 || vi.channels > 2 || vi.sample_rate == 0
                || (t->hz != 0 && (t->hz != (int)vi.sample_rate
                                   || t->channels != vi.channels))) {
                stb_vorbis_close(v);
                return -1;
            }
            t->hz = (int)vi.sample_rate;
            t->channels = vi.channels;
            t->ogg = v;
            continue;
        }

        if (avail <= 0) {
            if (fill(t) != 0)
                return -1;
            continue;
        }
        {
            int nch = 0, nsamp = 0, used;
            float **out = NULL;

            used = stb_vorbis_decode_frame_pushdata(t->ogg, t->in + t->in_off,
                                                    avail, &nch, &out, &nsamp);
            if (used == 0) {
                /* Needs the rest of the packet. */
                if (fill(t) != 0)
                    return -1;
                continue;
            }
            t->in_off += used;
            if (nsamp > 0) {
                const int bytes = nsamp * t->channels * (int)sizeof(float);
                float *dst = (float *)(void *)(t->pend + t->pend_len);
                int i, c;

                if (t->pend_len + bytes > PEND_CAP)
                    return -1;
                for (i = 0; i < nsamp; i++)
                    for (c = 0; c < t->channels; c++)
                        dst[i * t->channels + c] = out[c][i];
                t->pend_len += bytes;
                return bytes;
            }
        }
    }
}

/* AAC (ADTS): one decoded frame per call, through the libavcodec backend. */
static int
aac_block(Track *t)
{
    for (;;) {
        int avail = t->in_len - t->in_off;
        int used = 0, frames = 0, rate = 0, nch = 0, r;
        char msg[160] = "";
        float *out;

        if (!t->aac)
            return -1;
        if (avail <= 0) {
            if (fill(t) != 0)
                return -1;
            continue;
        }

        out = (float *)(void *)(t->pend + t->pend_len);
        r = aac_dec_step(t->aac, t->in + t->in_off, avail, &used,
                         out, (PEND_CAP - t->pend_len) / (int)sizeof(float),
                         &frames, &rate, &nch, msg, sizeof msg);
        t->in_off += used;
        if (r < 0) {
            SDL_Log("AAC stream failed: %s", msg);
            return -1;
        }
        if (r == 0) {
            if (fill(t) != 0)
                return -1;
            continue;
        }

        /* While opening, the last frame's rate wins: HE-AAC can report its
         * final rate only after the first frames. Mid-stream changes fail. */
        if (t->hz == 0 || t->opening) {
            if (t->hz != 0 && t->channels != nch)
                return -1;
            t->hz = rate;
            t->channels = nch;
        } else if (t->hz != rate || t->channels != nch) {
            SDL_Log("AAC stream changes sample rate or channels mid-stream");
            return -1;
        }

        {
            const int bytes = frames * nch * (int)sizeof(float);
            t->pend_len += bytes;
            return bytes;
        }
    }
}

static int
block(Track *t)
{
    switch (t->codec) {
    case STREAM_MP3:
        return mp3_block(t);
    case STREAM_OGG:
        return ogg_block(t);
    case STREAM_AAC:
        return aac_block(t);
    default:
        return -1;
    }
}

/* Decodes the first block(s) so the format is known before the engine starts.
 * Returns the number of blocks decoded (0 means failure). */
static int
prime(Track *t)
{
    const int want = t->codec == STREAM_AAC ? AAC_PRIME : 1;
    int got = 0;

    t->opening = 1;
    while (got < want) {
        if (block(t) <= 0)
            break;
        got++;
    }
    t->opening = 0;
    return got;
}

StreamCodec
stream_codec_from_content_type(const char *ct)
{
    if (!ct || !*ct)
        return STREAM_NONE;
    if (strcmp(ct, "audio/mpeg") == 0 || strcmp(ct, "audio/mp3") == 0
        || strcmp(ct, "audio/mpeg3") == 0 || strcmp(ct, "audio/x-mpeg") == 0)
        return STREAM_MP3;
    if (strcmp(ct, "audio/ogg") == 0 || strcmp(ct, "application/ogg") == 0
        || strcmp(ct, "audio/vorbis") == 0)
        return STREAM_OGG;
    if (strcmp(ct, "audio/aac") == 0 || strcmp(ct, "audio/aacp") == 0
        || strcmp(ct, "audio/x-aac") == 0)
        return STREAM_AAC;
    return STREAM_NONE;
}

StreamCodec
stream_codec_sniff(const Uint8 *buf, int n)
{
    if (n >= 4 && memcmp(buf, "OggS", 4) == 0)
        return STREAM_OGG;
    if (n >= 3 && memcmp(buf, "ID3", 3) == 0)
        return STREAM_MP3;
    if (n >= 2 && buf[0] == 0xff) {
        /* ADTS: 12-bit sync, layer bits 00. MPEG audio: layer I-III. */
        if ((buf[1] & 0xf6) == 0xf0)
            return STREAM_AAC;
        if ((buf[1] & 0xe0) == 0xe0 && (buf[1] & 0x06) != 0x00)
            return STREAM_MP3;
    }
    return STREAM_NONE;
}

static const char *
codec_label(StreamCodec c)
{
    switch (c) {
    case STREAM_MP3:
        return "MP3";
    case STREAM_OGG:
        return "Vorbis";
    case STREAM_AAC:
        return "AAC";
    default:
        return "";
    }
}

Track *
track_stream_open_io(const StreamIO *io, const char *content_type,
                     TrackInfo *info, char *err, size_t errlen)
{
    Track *t;
    char msg[256];

    if (!io || !info) {
        if (err && errlen)
            snprintf(err, errlen, "invalid arguments");
        if (io && io->destroy)
            io->destroy(io->ud);
        return NULL;
    }

    t = (Track *)calloc(1, sizeof *t);
    if (!t) {
        if (io->destroy)
            io->destroy(io->ud);
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        return NULL;
    }
    t->io = *io;
    t->base.ops = &stream_ops;
    SDL_AtomicSet(&t->interrupted, 0);
    SDL_AtomicSet(&t->dirty, 0);
    t->in = (Uint8 *)malloc(IN_CAP);
    t->pend = (Uint8 *)malloc(PEND_CAP);
    if (!t->in || !t->pend) {
        snprintf(msg, sizeof msg, "out of memory");
        goto fail;
    }

    t->codec = stream_codec_from_content_type(content_type);
    if (t->codec == STREAM_NONE) {
        /* Content-Type missing or generic: sniff the first bytes. */
        t->opening = 1;
        while (t->in_len - t->in_off < SNIFF_BYTES) {
            if (fill(t) != 0) {
                t->opening = 0;
                snprintf(msg, sizeof msg, "stream ended before any audio data");
                goto fail;
            }
        }
        t->opening = 0;
        t->codec = stream_codec_sniff(t->in + t->in_off, t->in_len - t->in_off);
        if (t->codec == STREAM_NONE) {
            snprintf(msg, sizeof msg,
                     "unsupported stream type '%s' (expected MP3, Ogg Vorbis or AAC)",
                     content_type && *content_type ? content_type : "unknown");
            goto fail;
        }
    }

    if (t->codec == STREAM_AAC) {
        t->aac = aac_dec_new();
        if (!t->aac) {
            snprintf(msg, sizeof msg,
                     "AAC streams need a build with FFmpeg (AAC=1)");
            goto fail;
        }
    }

    if (prime(t) == 0) {
        if (t->codec == STREAM_OGG)
            snprintf(msg, sizeof msg,
                     "cannot decode Ogg stream (only Vorbis is supported)");
        else
            snprintf(msg, sizeof msg, "cannot decode %s stream",
                     codec_label(t->codec));
        goto fail;
    }
    if (t->hz <= 0 || t->channels < 1 || t->channels > 2) {
        snprintf(msg, sizeof msg, "unsupported %s stream format",
                 codec_label(t->codec));
        goto fail;
    }

    /* Priming output counts as the first session's PCM, so it is not dirty. */
    memset(info, 0, sizeof *info);
    info->src_spec.format = t->codec == STREAM_MP3 ? AUDIO_S16SYS : AUDIO_F32SYS;
    info->src_spec.channels = (Uint8)t->channels;
    info->src_spec.freq = t->hz;
    info->total_frames = 0;
    info->codec = codec_label(t->codec);
    info->bits_per_sample = 0;
    info->is_live = 1;
    return (Track *)t;

fail:
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
    stream_close(t);
    return NULL;
}

static int
stream_decode_chunk(Track *t, Uint8 *dst, int cap)
{
    int bpf, avail, n;

    if (!t || !dst)
        return -1;
    if (t->failed || interrupted(t))
        return -1;

    bpf = bytes_per_frame(t);
    if (bpf <= 0 || cap < bpf)
        return -1;

    if (t->pend_off >= t->pend_len) {
        t->pend_off = 0;
        t->pend_len = 0;
        if (block(t) <= 0) {
            t->failed = 1;
            return -1;
        }
    }

    avail = t->pend_len - t->pend_off;
    n = avail < cap ? avail : cap - cap % bpf; /* whole frames only */
    memcpy(dst, t->pend + t->pend_off, (size_t)n);
    t->pend_off += n;
    SDL_AtomicSet(&t->dirty, 1);
    return n;
}

static int
stream_seek(Track *t, Uint32 frame)
{
    if (!t || !t->io.reconnect)
        return -1;
    /* Live streams have no position. The engine only asks for frame 0 when a
     * session starts, which restarts the connection if the last one was used. */
    if (frame != 0)
        return -1;
    if (!SDL_AtomicGet(&t->dirty) && !t->failed && !interrupted(t))
        return 0;

    SDL_AtomicSet(&t->interrupted, 0);
    t->failed = 0;
    t->pend_len = 0;
    t->pend_off = 0;
    if (t->io.reset)
        t->io.reset(t->io.ud);
    if (t->io.reconnect(t->io.ud) != 0) {
        t->failed = 1;
        return -1;
    }
    reset_codec(t);
    SDL_AtomicSet(&t->dirty, 0);
    return 0;
}

static void
stream_interrupt(Track *t)
{
    if (!t)
        return;
    SDL_AtomicSet(&t->interrupted, 1);
    if (t->io.abort)
        t->io.abort(t->io.ud);
}

static void
stream_close(Track *t)
{
    if (!t)
        return;
    if (t->ogg)
        stb_vorbis_close(t->ogg);
    if (t->aac)
        aac_dec_free(t->aac);
    if (t->io.destroy)
        t->io.destroy(t->io.ud);
    free(t->in);
    free(t->pend);
    free(t);
}

/* ---- Network wiring: StreamIO over net.c ---- */

typedef struct {
    char *url;
    NetStream *ns;     /* NULL between a close and the next open */
    SDL_mutex *mu;     /* guards ns, so abort never races a swap */
    SDL_atomic_t aborted;
} NetIO;

static int
netio_read(void *ud, Uint8 *buf, int cap)
{
    NetIO *n = (NetIO *)ud;
    NetStream *ns;

    SDL_LockMutex(n->mu);
    ns = n->ns;
    SDL_UnlockMutex(n->mu);
    if (!ns)
        return -1;
    /* Not under the lock: net_read blocks, and abort must get through. */
    return net_read(ns, buf, cap);
}

static int
netio_reconnect(void *ud)
{
    NetIO *n = (NetIO *)ud;
    NetStream *old, *fresh;
    char err[256] = "";
    int aborted;

    SDL_LockMutex(n->mu);
    old = n->ns;
    n->ns = NULL;
    SDL_UnlockMutex(n->mu);
    if (old)
        net_close(old);

    fresh = net_open(n->url, err, sizeof err);
    if (!fresh) {
        SDL_Log("stream reconnect failed: %s", err);
        return -1;
    }

    SDL_LockMutex(n->mu);
    n->ns = fresh;
    /* An abort may have landed while net_open ran, before ns was set. */
    aborted = SDL_AtomicGet(&n->aborted);
    if (aborted)
        net_abort(fresh);
    SDL_UnlockMutex(n->mu);
    return 0;
}

static void
netio_abort(void *ud)
{
    NetIO *n = (NetIO *)ud;

    SDL_AtomicSet(&n->aborted, 1);
    SDL_LockMutex(n->mu);
    if (n->ns)
        net_abort(n->ns);
    SDL_UnlockMutex(n->mu);
}

static void
netio_reset(void *ud)
{
    SDL_AtomicSet(&((NetIO *)ud)->aborted, 0);
}

static void
netio_destroy(void *ud)
{
    NetIO *n = (NetIO *)ud;
    NetStream *ns;

    if (!n)
        return;
    SDL_LockMutex(n->mu);
    ns = n->ns;
    n->ns = NULL;
    SDL_UnlockMutex(n->mu);
    if (ns)
        net_close(ns);
    SDL_DestroyMutex(n->mu);
    free(n->url);
    free(n);
}

Track *
track_stream_open(const char *url, TrackInfo *info, char *err, size_t errlen)
{
    NetIO *n;
    StreamIO io;
    NetStream *ns;

    if (!url || !info) {
        if (err && errlen)
            snprintf(err, errlen, "invalid arguments");
        return NULL;
    }

    n = (NetIO *)calloc(1, sizeof *n);
    if (!n) {
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        return NULL;
    }
    n->url = (char *)malloc(strlen(url) + 1);
    n->mu = SDL_CreateMutex();
    if (!n->url || !n->mu) {
        netio_destroy(n);
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        return NULL;
    }
    strcpy(n->url, url);
    SDL_AtomicSet(&n->aborted, 0);

    ns = net_open(url, err, errlen);
    if (!ns) {
        netio_destroy(n);
        return NULL;
    }
    n->ns = ns;

    memset(&io, 0, sizeof io);
    io.read = netio_read;
    io.reconnect = netio_reconnect;
    io.abort = netio_abort;
    io.reset = netio_reset;
    io.destroy = netio_destroy;
    io.ud = n;
    return track_stream_open_io(&io, net_content_type(ns), info, err, errlen);
}
