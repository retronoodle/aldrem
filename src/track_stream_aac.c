/* track_stream_aac.c — AAC (ADTS) decoding for live streams, backed by
 * libavcodec from FFmpeg. libavcodec is LGPL and linked dynamically (AAC=1);
 * this file is the only place that touches it.
 *
 * The ADTS parser splits the byte stream into frames, the decoder turns each
 * frame into planar float, and this file interleaves it by hand. Without
 * HAVE_AVCODEC every entry point is a stub, so the rest of the engine just sees
 * AAC as unsupported.
 */
#include "track_stream_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_AVCODEC

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>

struct AacDec {
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
};

AacDec *
aac_dec_new(void)
{
    AacDec *d = (AacDec *)calloc(1, sizeof *d);
    const AVCodec *codec;

    if (!d)
        return NULL;
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec)
        d->ctx = avcodec_alloc_context3(codec);
    if (d->ctx && avcodec_open2(d->ctx, codec, NULL) < 0) {
        avcodec_free_context(&d->ctx);
    }
    d->parser = av_parser_init(AV_CODEC_ID_AAC);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->ctx || !d->parser || !d->pkt || !d->frame) {
        aac_dec_free(d);
        return NULL;
    }
    return d;
}

void
aac_dec_free(AacDec *d)
{
    if (!d)
        return;
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    if (d->parser)
        av_parser_close(d->parser);
    avcodec_free_context(&d->ctx);
    free(d);
}

void
aac_dec_reset(AacDec *d)
{
    if (!d)
        return;
    avcodec_flush_buffers(d->ctx);
    if (d->parser)
        av_parser_close(d->parser);
    d->parser = av_parser_init(AV_CODEC_ID_AAC);
}

int
aac_dec_step(AacDec *d, const Uint8 *in, int in_len, int *used,
             float *out, int out_cap, int *out_frames,
             int *rate, int *channels, char *msg, size_t msglen)
{
    int consumed = 0;

    *used = 0;
    if (!d || !d->parser || !d->ctx) {
        snprintf(msg, msglen, "AAC decoder not available");
        return -1;
    }

    for (;;) {
        int r;

        r = avcodec_receive_frame(d->ctx, d->frame);
        if (r == 0) {
            AVFrame *f = d->frame;
            int nch = f->ch_layout.nb_channels;
            int ns = f->nb_samples;
            int c, i;

            if (f->format != AV_SAMPLE_FMT_FLTP) {
                snprintf(msg, msglen, "unsupported AAC sample format %d",
                         (int)f->format);
                av_frame_unref(f);
                return -1;
            }
            if (nch < 1 || nch > 2) {
                snprintf(msg, msglen, "unsupported AAC channel count %d", nch);
                av_frame_unref(f);
                return -1;
            }
            if (ns * nch > out_cap) {
                snprintf(msg, msglen, "AAC frame too large");
                av_frame_unref(f);
                return -1;
            }
            for (c = 0; c < nch; c++) {
                const float *src = (const float *)(const void *)f->data[c];

                for (i = 0; i < ns; i++)
                    out[i * nch + c] = src[i];
            }
            *out_frames = ns;
            *rate = f->sample_rate > 0 ? f->sample_rate : d->ctx->sample_rate;
            *channels = nch;
            *used = consumed;
            av_frame_unref(f);
            return 1;
        }
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
            snprintf(msg, msglen, "AAC decode error %d", r);
            return -1;
        }

        if (consumed >= in_len) {
            *used = consumed;
            return 0;
        }

        {
            uint8_t *pdata = NULL;
            int psize = 0, n;

            n = av_parser_parse2(d->parser, d->ctx, &pdata, &psize,
                                 in + consumed, in_len - consumed,
                                 AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (n < 0) {
                snprintf(msg, msglen, "AAC framing error %d", n);
                return -1;
            }
            if (n == 0 && psize == 0) {
                /* Parser wants more input than we have. */
                *used = consumed;
                return 0;
            }
            consumed += n;
            *used = consumed;

            if (psize > 0) {
                int sr;

                if (av_new_packet(d->pkt, psize) < 0) {
                    snprintf(msg, msglen, "out of memory");
                    return -1;
                }
                memcpy(d->pkt->data, pdata, (size_t)psize);
                sr = avcodec_send_packet(d->ctx, d->pkt);
                av_packet_unref(d->pkt);
                if (sr == AVERROR_INVALIDDATA) {
                    /* One corrupt frame: drop it and resync, as MP3 does,
                     * rather than ending the stream. */
                    avcodec_flush_buffers(d->ctx);
                } else if (sr < 0 && sr != AVERROR_EOF) {
                    snprintf(msg, msglen, "AAC decode error %d", sr);
                    return -1;
                }
            }
        }
    }
}

#else /* !HAVE_AVCODEC */

struct AacDec {
    int unused;
};

AacDec *
aac_dec_new(void)
{
    return NULL;
}

void
aac_dec_free(AacDec *d)
{
    (void)d;
}

void
aac_dec_reset(AacDec *d)
{
    (void)d;
}

int
aac_dec_step(AacDec *d, const Uint8 *in, int in_len, int *used,
             float *out, int out_cap, int *out_frames,
             int *rate, int *channels, char *msg, size_t msglen)
{
    (void)d;
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_cap;
    (void)out_frames;
    (void)rate;
    (void)channels;
    *used = 0;
    snprintf(msg, msglen, "AAC is not built in");
    return -1;
}

#endif
