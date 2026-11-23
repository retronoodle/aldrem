/* track_stream_internal.h — live stream decoding shared by track_stream.c, the
 * AAC backend and the tests. Not part of the public API.
 *
 * A stream decoder reads bytes through a StreamIO, so the same MP3/Ogg/AAC code
 * runs against libcurl in the app and against an in-memory buffer in tests.
 */
#ifndef TRACK_STREAM_INTERNAL_H
#define TRACK_STREAM_INTERNAL_H

#include "track_internal.h"

typedef struct {
    /* Reads up to cap bytes. Returns >0 bytes, 0 at end of body, -1 on error. */
    int (*read)(void *ud, Uint8 *buf, int cap);
    /* Opens a fresh connection that starts from the beginning of the stream.
     * Returns 0 on success. Called on the decoder thread. */
    int (*reconnect)(void *ud);
    /* Wakes a blocked read and makes later reads fail. Any thread. */
    void (*abort)(void *ud);
    /* Clears a previous abort before a restart. UI thread, decoder not running. */
    void (*reset)(void *ud);
    /* Releases ud. Called exactly once, on failure or close. */
    void (*destroy)(void *ud);
    /* Optional: copies the current now-playing text into out. Returns 1 when
     * known, 0 when not. NULL for in-memory IO. Any thread. */
    int (*title)(void *ud, char *out, size_t cap);
    void *ud;
} StreamIO;

typedef enum { STREAM_NONE, STREAM_MP3, STREAM_OGG, STREAM_AAC } StreamCodec;

/* Maps a Content-Type (lower-cased, no parameters) to a codec. NONE when the
 * type says nothing useful, so the caller falls back to sniffing the bytes. */
StreamCodec stream_codec_from_content_type(const char *ct);

/* Identifies a codec from the first bytes: "OggS", ID3 or MPEG audio sync,
 * or an ADTS header. NONE when unrecognised. */
StreamCodec stream_codec_sniff(const Uint8 *buf, int n);

/* Opens a decoder over io. Takes ownership of io: on failure it calls
 * io->destroy. content_type may be "" to force sniffing. */
Track *track_stream_open_io(const StreamIO *io, const char *content_type,
                            TrackInfo *info, char *err, size_t errlen);

/* AAC (ADTS) backend in track_stream_aac.c. Without HAVE_AVCODEC, aac_dec_new
 * returns NULL and the other calls are no-ops, so the caller reports AAC as
 * unsupported. */
typedef struct AacDec AacDec;

AacDec *aac_dec_new(void);
void aac_dec_free(AacDec *d);
/* Forgets buffered state after a reconnect. */
void aac_dec_reset(AacDec *d);

/* Consumes ADTS bytes from in and decodes at most one frame into out, as
 * interleaved float. *used receives the bytes consumed from in.
 * Returns 1 when a frame was produced (*out_frames samples per channel,
 * *rate and *channels set), 0 when more input is needed, -1 on error with
 * the reason in msg. out_cap is in floats. */
int aac_dec_step(AacDec *d, const Uint8 *in, int in_len, int *used,
                 float *out, int out_cap, int *out_frames,
                 int *rate, int *channels, char *msg, size_t msglen);

#endif
