/* test_stream.c — live stream decoders (MP3, Ogg Vorbis, AAC) against small
 * committed fixtures, fed through an in-memory StreamIO. No network and no
 * sound device: the track layer is driven directly.
 *
 * Usage: test_stream <fixture-dir>   (tests/data in the repo)
 */
#include "track.h"
#include "track_stream_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE     44100
#define CHANNELS 2

static int failures = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            failures++;                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);        \
            fprintf(stderr, "\n");               \
        }                                        \
    } while (0)

/* In-memory byte source. Reads come in chunks so frames straddle reads. Once
 * the bytes run out, a read fails, as a dropped connection would. Reconnect
 * restarts from the beginning while reconnects_left lasts. */
typedef struct {
    const Uint8 *buf;
    size_t len;
    size_t pos;
    size_t chunk;
    int reconnects_left;
    int reconnects;
    int aborted;
} Mem;

static int
mem_read(void *ud, Uint8 *dst, int cap)
{
    Mem *m = (Mem *)ud;
    size_t n;

    if (m->aborted || m->pos >= m->len)
        return -1;
    n = m->len - m->pos;
    if (n > m->chunk)
        n = m->chunk;
    if (n > (size_t)cap)
        n = (size_t)cap;
    memcpy(dst, m->buf + m->pos, n);
    m->pos += n;
    return (int)n;
}

static int
mem_reconnect(void *ud)
{
    Mem *m = (Mem *)ud;

    if (m->reconnects_left <= 0)
        return -1;
    m->reconnects_left--;
    m->reconnects++;
    m->pos = 0;
    return 0;
}

static void
mem_abort(void *ud)
{
    ((Mem *)ud)->aborted = 1;
}

static void
mem_reset(void *ud)
{
    ((Mem *)ud)->aborted = 0;
}

static void
mem_destroy(void *ud)
{
    (void)ud; /* the Mem lives on the test's stack */
}

static StreamIO
mem_io(Mem *m)
{
    StreamIO io;

    io.read = mem_read;
    io.reconnect = mem_reconnect;
    io.abort = mem_abort;
    io.reset = mem_reset;
    io.destroy = mem_destroy;
    io.ud = m;
    return io;
}

/* Reads fixture bytes from disk. Returns NULL on failure. */
static Uint8 *
load(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    Uint8 *buf;
    long n;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (Uint8 *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Decodes until the source gives up or max bytes are produced. Returns the
 * number of PCM bytes handed out. */
static size_t
drain(Track *t, size_t max)
{
    Uint8 chunk[4096];
    size_t total = 0;

    while (total < max) {
        int n = track_decode_chunk(t, chunk, sizeof chunk);

        if (n <= 0)
            break;
        total += (size_t)n;
    }
    return total;
}

/* Bytes one second of this format takes, for the "most of the tone came out"
 * checks. The tone is 0.5 s, so a healthy decode is about half of this. */
static size_t
expected_half_second(int bytes_per_sample)
{
    return (size_t)(RATE / 2) * CHANNELS * (size_t)bytes_per_sample;
}

static void
test_sniffed_mp3_and_ogg(const char *dir)
{
    const char *names[2] = { "tone.mp3", "tone.ogg" };
    const char *labels[2] = { "MP3", "Vorbis" };
    const int sample_bytes[2] = { 2, 4 };
    int i;

    for (i = 0; i < 2; i++) {
        size_t len = 0;
        Uint8 *buf = load(dir, names[i], &len);
        Mem m;
        StreamIO io;
        TrackInfo info;
        Track *t;
        char err[256] = "";
        size_t got;

        CHECK(buf != NULL, "cannot read fixture %s", names[i]);
        if (!buf)
            continue;

        memset(&m, 0, sizeof m);
        m.buf = buf;
        m.len = len;
        m.chunk = 700; /* frames straddle reads */
        io = mem_io(&m);

        t = track_stream_open_io(&io, "", &info, err, sizeof err);
        CHECK(t != NULL, "%s: open failed: %s", names[i], err);
        if (!t) {
            free(buf);
            continue;
        }
        CHECK(info.is_live == 1, "%s: is_live not set", names[i]);
        CHECK(info.total_frames == 0, "%s: live stream reported a length", names[i]);
        CHECK(info.src_spec.freq == RATE, "%s: rate %d", names[i], info.src_spec.freq);
        CHECK(info.src_spec.channels == CHANNELS, "%s: channels %d", names[i],
              info.src_spec.channels);
        CHECK(info.codec && strcmp(info.codec, labels[i]) == 0,
              "%s: codec label '%s'", names[i], info.codec ? info.codec : "(null)");

        got = drain(t, (size_t)4 * expected_half_second(sample_bytes[i]));
        CHECK(got >= expected_half_second(sample_bytes[i]) * 8 / 10,
              "%s: decoded %lu bytes, want about %lu", names[i],
              (unsigned long)got,
              (unsigned long)expected_half_second(sample_bytes[i]));

        track_close(t);
        free(buf);
    }
}

static void
test_aac_or_unsupported(const char *dir)
{
    size_t len = 0;
    Uint8 *buf = load(dir, "tone.aac", &len);
    Mem m;
    StreamIO io;
    TrackInfo info;
    Track *t;
    char err[256] = "";

    CHECK(buf != NULL, "cannot read fixture tone.aac");
    if (!buf)
        return;

    memset(&m, 0, sizeof m);
    m.buf = buf;
    m.len = len;
    m.chunk = 512;
    io = mem_io(&m);
    t = track_stream_open_io(&io, "audio/aac", &info, err, sizeof err);

#ifdef HAVE_AVCODEC
    CHECK(t != NULL, "AAC open failed: %s", err);
    if (t) {
        size_t got;

        CHECK(info.is_live == 1, "AAC: is_live not set");
        CHECK(info.src_spec.freq == RATE, "AAC: rate %d", info.src_spec.freq);
        CHECK(info.src_spec.channels == CHANNELS, "AAC: channels %d",
              info.src_spec.channels);
        CHECK(info.codec && strcmp(info.codec, "AAC") == 0, "AAC: codec label");
        got = drain(t, (size_t)4 * expected_half_second(4));
        /* AAC has priming delay and a final partial frame, so accept less. */
        CHECK(got >= expected_half_second(4) / 2,
              "AAC: decoded %lu bytes", (unsigned long)got);
        track_close(t);
    }
#else
    CHECK(t == NULL, "AAC should fail without FFmpeg");
    CHECK(strstr(err, "FFmpeg") != NULL, "AAC error text: '%s'", err);
    if (t)
        track_close(t);
#endif
    free(buf);
}

static void
test_garbage_prefix_resyncs(const char *dir)
{
    size_t len = 0, total = 0;
    Uint8 *buf = load(dir, "tone.mp3", &len);
    Uint8 *joined;
    Mem m;
    StreamIO io;
    TrackInfo info;
    Track *t;
    char err[256] = "";
    size_t got;

    CHECK(buf != NULL, "cannot read fixture tone.mp3");
    if (!buf)
        return;

    /* Non-sync junk before the first frame, declared as audio/mpeg so the
     * sniffer does not get in the way. */
    total = 3000 + len;
    joined = (Uint8 *)malloc(total);
    if (!joined) {
        free(buf);
        return;
    }
    memset(joined, 0x55, 3000);
    memcpy(joined + 3000, buf, len);

    memset(&m, 0, sizeof m);
    m.buf = joined;
    m.len = total;
    m.chunk = 1000;
    io = mem_io(&m);
    t = track_stream_open_io(&io, "audio/mpeg", &info, err, sizeof err);
    CHECK(t != NULL, "garbage prefix: open failed: %s", err);
    if (t) {
        got = drain(t, (size_t)4 * expected_half_second(2));
        CHECK(got >= expected_half_second(2) * 8 / 10,
              "garbage prefix: decoded %lu bytes", (unsigned long)got);
        track_close(t);
    }
    free(joined);
    free(buf);
}

static void
test_unknown_type_rejected(void)
{
    static const Uint8 junk[] = "this is definitely not audio data";
    Mem m;
    StreamIO io;
    TrackInfo info;
    Track *t;
    char err[256] = "";

    memset(&m, 0, sizeof m);
    m.buf = junk;
    m.len = sizeof junk - 1;
    m.chunk = 64;
    io = mem_io(&m);
    t = track_stream_open_io(&io, "", &info, err, sizeof err);
    CHECK(t == NULL, "junk stream should not open");
    CHECK(strstr(err, "unsupported stream type") != NULL, "junk error: '%s'", err);
    if (t)
        track_close(t);
}

static void
test_reconnect_continues_then_ends(const char *dir)
{
    size_t len = 0;
    Uint8 *buf = load(dir, "tone.mp3", &len);
    Mem once, twice;
    StreamIO io;
    TrackInfo info;
    Track *t;
    char err[256] = "";
    size_t single = 0, repeated = 0;

    CHECK(buf != NULL, "cannot read fixture tone.mp3");
    if (!buf)
        return;

    /* Baseline: one connection, no retries. */
    memset(&once, 0, sizeof once);
    once.buf = buf;
    once.len = len;
    once.chunk = 900;
    io = mem_io(&once);
    t = track_stream_open_io(&io, "audio/mpeg", &info, err, sizeof err);
    CHECK(t != NULL, "reconnect baseline: open failed: %s", err);
    if (t) {
        single = drain(t, (size_t)1 << 24);
        track_close(t);
    }

    /* Two allowed reconnects: the same clip plays again on each new connection,
     * and once they are used up the stream ends. */
    memset(&twice, 0, sizeof twice);
    twice.buf = buf;
    twice.len = len;
    twice.chunk = 900;
    twice.reconnects_left = 2;
    io = mem_io(&twice);
    t = track_stream_open_io(&io, "audio/mpeg", &info, err, sizeof err);
    CHECK(t != NULL, "reconnect: open failed: %s", err);
    if (t) {
        repeated = drain(t, (size_t)1 << 24);
        track_close(t);
    }

    CHECK(twice.reconnects == 2, "reconnect: expected 2 reconnects, got %d",
          twice.reconnects);
    CHECK(repeated > single * 2 - single / 4,
          "reconnect: %lu bytes with reconnects, %lu without",
          (unsigned long)repeated, (unsigned long)single);
    free(buf);
}

static void
test_interrupt_then_restart(const char *dir)
{
    size_t len = 0;
    Uint8 *buf = load(dir, "tone.mp3", &len);
    Mem m;
    StreamIO io;
    TrackInfo info;
    Track *t;
    char err[256] = "";
    Uint8 chunk[1024];
    int n;

    CHECK(buf != NULL, "cannot read fixture tone.mp3");
    if (!buf)
        return;

    memset(&m, 0, sizeof m);
    m.buf = buf;
    m.len = len;
    m.chunk = 900;
    m.reconnects_left = 4;
    io = mem_io(&m);
    t = track_stream_open_io(&io, "audio/mpeg", &info, err, sizeof err);
    CHECK(t != NULL, "interrupt: open failed: %s", err);
    if (!t) {
        free(buf);
        return;
    }

    n = track_decode_chunk(t, chunk, sizeof chunk);
    CHECK(n > 0, "interrupt: first decode gave %d", n);

    /* Stop: the next read fails at once, not after the source runs out. */
    track_interrupt(t);
    n = track_decode_chunk(t, chunk, sizeof chunk);
    CHECK(n < 0, "interrupt: decode after interrupt gave %d", n);

    /* Play again: seek to 0 restarts the connection, and decoding resumes. */
    CHECK(track_seek(t, 0) == 0, "interrupt: restart seek failed");
    n = track_decode_chunk(t, chunk, sizeof chunk);
    CHECK(n > 0, "interrupt: decode after restart gave %d", n);

    /* A live stream has no position: any other frame is refused. */
    CHECK(track_seek(t, 1000) != 0, "interrupt: seek to a frame should fail");

    track_close(t);
    free(buf);
}

int
main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/data";

    test_sniffed_mp3_and_ogg(dir);
    test_aac_or_unsupported(dir);
    test_garbage_prefix_resyncs(dir);
    test_unknown_type_rejected();
    test_reconnect_continues_then_ends(dir);
    test_interrupt_then_restart(dir);

    if (failures) {
        fprintf(stderr, "test_stream: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_stream: all checks passed\n");
    return 0;
}
