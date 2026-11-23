/* playback.c — device, lock-free ring buffer, decoder thread, state machine.
 *
 * Threading model:
 *   - The audio callback reads only SDL_atomic_t values and the ring buffer.
 *     It never locks, allocates, or mutates the playback state.
 *   - One decoder thread per playback session converts source PCM through an
 *     SDL_AudioStream into the ring in the device format (float32).
 *   - The UI thread owns all lifecycle transitions (play/pause/stop/EOF).
 *   - A mutex guards only UI-facing metadata (name, duration, error).
 *
 * Deliberate departure from the PRD prose: shared audio state is atomic rather
 * than mutex-protected so the callback never blocks. Seeking follows the same
 * rule: the UI thread pauses the device, joins the decoder, rebuilds the
 * session from a new base position, then resumes. That costs a brief audible
 * gap but never touches the callback path.
 */
#include "playback.h"
#include "tags.h"
#include "track.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RB_FRAMES       (1 << 17) /* ≈2.97 s at 44.1 kHz, 1 MiB stereo float */
#define SCRATCH_BYTES   (64 * 1024)
#define FEED_SECONDS    0.5
#define LEVEL_SCALE     65536.0f  /* fixed-point scale for meter levels */

struct Playback {
    int inited;
    int dev_open;
    SDL_AudioDeviceID dev;
    SDL_AudioSpec have; /* ground truth; never hard-code 44100 after open */

    SDL_mutex *meta_mutex;
    char name[512];
    char error[512];
    char tag_text[256]; /* local file: "Artist - Title" from its tags */
    double duration;
    int has_track;
    int volume_percent;

    Track *track;
    TrackInfo info;
    int live_paused; /* a live stream was paused: stopped, but not idle */

    /* Seconds at the start of the current decoder session. Written only on the
     * UI thread (load/stop/seek/EOF); position = base + frames_played/freq. */
    double position_base;

    SDL_AudioStream *stream;
    SDL_Thread *thread;
    int thread_running;

    SDL_atomic_t state;        /* PlaybackState */
    SDL_atomic_t volume_q8;    /* 0..256 */
    SDL_atomic_t frames_played;
    SDL_atomic_t decoder_done;
    SDL_atomic_t eof;
    SDL_atomic_t quit_thread;

    /* Per-channel meter levels, written by the callback (LEVEL_SCALE fixed). */
    SDL_atomic_t lvl_peak_l;
    SDL_atomic_t lvl_peak_r;
    SDL_atomic_t lvl_rms_l;
    SDL_atomic_t lvl_rms_r;

    float *ring;               /* RB_FRAMES frames, have.channels interleaved */
    SDL_atomic_t rb_write;     /* monotonic frame counters */
    SDL_atomic_t rb_read;
    Uint8 *scratch;
};

static struct Playback g;

static int dst_bytes_per_frame(const struct Playback *p)
{
    return (int)sizeof(float) * p->have.channels;
}

/* Source frame index for a position in seconds, rounded and clamped to the
 * track length. */
static Uint32
src_frame_for(const struct Playback *p, double seconds)
{
    double frame;

    if (seconds <= 0.0 || p->info.src_spec.freq <= 0)
        return 0;
    frame = seconds * (double)p->info.src_spec.freq;
    if (frame >= (double)p->info.total_frames)
        return p->info.total_frames;
    return (Uint32)(frame + 0.5);
}

/* Playhead as reported to the UI: session start offset plus frames the
 * callback has consumed since then. */
static double
current_position(struct Playback *p)
{
    if (p->have.freq <= 0)
        return p->position_base;
    return p->position_base
        + (double)SDL_AtomicGet(&p->frames_played) / (double)p->have.freq;
}

/* Meter levels travel through SDL_atomic_t as fixed-point ints so the callback
 * never needs a lock. Values above full scale are clamped to keep ints sane. */
static int
level_to_fixed(float v)
{
    if (v < 0.0f)
        v = 0.0f;
    if (v > 4.0f)
        v = 4.0f;
    return (int)(v * LEVEL_SCALE + 0.5f);
}

static void
level_clear(struct Playback *p)
{
    SDL_AtomicSet(&p->lvl_peak_l, 0);
    SDL_AtomicSet(&p->lvl_peak_r, 0);
    SDL_AtomicSet(&p->lvl_rms_l, 0);
    SDL_AtomicSet(&p->lvl_rms_r, 0);
}

static void SDLCALL
audio_callback(void *userdata, Uint8 *stream, int len)
{
    struct Playback *p = (struct Playback *)userdata;
    const int ch = p->have.channels;
    const int bpf = dst_bytes_per_frame(p);
    const int out_frames = bpf > 0 ? len / bpf : 0;
    int w, r, avail;

    if (len <= 0)
        return;

    /* Silence first: underruns and the paused/stopped states output zeros. */
    memset(stream, 0, (size_t)len);

    if (SDL_AtomicGet(&p->state) != MP_PLAYING || out_frames <= 0) {
        level_clear(p);
        return;
    }

    w = SDL_AtomicGet(&p->rb_write);
    SDL_MemoryBarrierAcquireFunction();
    r = SDL_AtomicGet(&p->rb_read);
    /* Counters are signed and monotonic: take the difference in unsigned
     * space so a wrap past INT_MAX cannot make the ring look negative. */
    avail = (int)((Uint32)w - (Uint32)r);

    if (avail > 0) {
        const int n = avail < out_frames ? avail : out_frames;
        const int ri = (int)((Uint32)r & (RB_FRAMES - 1));
        int first = RB_FRAMES - ri;
        float *dst = (float *)(void *)stream;

        if (first > n)
            first = n;
        memcpy(dst, p->ring + (size_t)ri * ch,
               (size_t)first * (size_t)ch * sizeof(float));
        if (n > first)
            memcpy(dst + (size_t)first * ch, p->ring,
                   (size_t)(n - first) * (size_t)ch * sizeof(float));

        SDL_MemoryBarrierReleaseFunction(); /* data consumed before we free it */
        SDL_AtomicSet(&p->rb_read, (int)((Uint32)r + (Uint32)n));

        int q8 = SDL_AtomicGet(&p->volume_q8);
        if (q8 != 256) {
            const float scale = (float)q8 / 256.0f;
            const int samples = n * (int)ch;
            int i;
            for (i = 0; i < samples; ++i)
                dst[i] *= scale;
        }

        /* Meter the post-volume output. Mono is duplicated into both sides. */
        {
            float pk[2] = { 0.0f, 0.0f }, sq[2] = { 0.0f, 0.0f };
            int f;
            for (f = 0; f < n; ++f) {
                int c;
                for (c = 0; c < 2; ++c) {
                    const float s = dst[f * ch + (ch > 1 ? c : 0)];
                    const float a = s < 0.0f ? -s : s;
                    if (a > pk[c])
                        pk[c] = a;
                    sq[c] += s * s;
                }
            }
            SDL_AtomicSet(&p->lvl_peak_l, level_to_fixed(pk[0]));
            SDL_AtomicSet(&p->lvl_peak_r, level_to_fixed(pk[1]));
            SDL_AtomicSet(&p->lvl_rms_l, level_to_fixed((float)sqrt(sq[0] / (float)n)));
            SDL_AtomicSet(&p->lvl_rms_r, level_to_fixed((float)sqrt(sq[1] / (float)n)));
        }

        /* 32-bit counter: wraps after ~13 h at 44.1 kHz. Accepted for v1. */
        SDL_AtomicAdd(&p->frames_played, n);
    } else {
        level_clear(p);
    }

    /* Only the UI thread performs the STOPPED transition; the callback just
     * raises the flag. */
    if (avail <= 0 && SDL_AtomicGet(&p->decoder_done))
        SDL_AtomicSet(&p->eof, 1);
}

static int SDLCALL
decoder_thread(void *data)
{
    struct Playback *p = (struct Playback *)data;
    const int ch = p->have.channels;
    const int bpf = dst_bytes_per_frame(p);
    const int feed_threshold = (int)(p->have.freq * FEED_SECONDS) * bpf;
    int exhausted = 0;

    if (bpf <= 0)
        return 0;

    for (;;) {
        int did_work = 0;

        if (SDL_AtomicGet(&p->quit_thread))
            break;

        /* 1. Drain converted frames from the stream into the ring. */
        for (;;) {
            int w = SDL_AtomicGet(&p->rb_write);
            int r = SDL_AtomicGet(&p->rb_read);
            int free_frames = RB_FRAMES - (int)((Uint32)w - (Uint32)r);
            int wi, contig, frames, got, got_frames;

            if (free_frames <= 0)
                break;

            wi = (int)((Uint32)w & (RB_FRAMES - 1));
            contig = RB_FRAMES - wi;
            frames = free_frames < contig ? free_frames : contig;
            got = SDL_AudioStreamGet(p->stream,
                    p->ring + (size_t)wi * ch,
                    frames * bpf);
            if (got < 0) {
                SDL_Log("audio stream get failed: %s", SDL_GetError());
                SDL_AtomicSet(&p->decoder_done, 1);
                return 0;
            }
            if (got == 0)
                break; /* starved; feed the stream below */

            got_frames = got / bpf;
            if (got_frames <= 0)
                break;

            SDL_MemoryBarrierReleaseFunction(); /* data published before index */
            SDL_AtomicSet(&p->rb_write, (int)((Uint32)w + (Uint32)got_frames));
            did_work = 1;
        }

        if (SDL_AtomicGet(&p->quit_thread))
            break;

        /* 2. Feed the track through the converter while it is shallow. */
        if (!exhausted) {
            if (SDL_AudioStreamAvailable(p->stream) < feed_threshold) {
                int got = track_decode_chunk(p->track, p->scratch, SCRATCH_BYTES);

                if (got > 0) {
                    if (SDL_AudioStreamPut(p->stream, p->scratch, got) < 0) {
                        SDL_Log("audio stream put failed: %s", SDL_GetError());
                        SDL_AtomicSet(&p->decoder_done, 1);
                        return 0;
                    }
                    did_work = 1;
                } else if (got == 0) {
                    /* Flush once so the resampler tail is not truncated. */
                    if (SDL_AudioStreamFlush(p->stream) < 0) {
                        SDL_Log("audio stream flush failed: %s", SDL_GetError());
                        SDL_AtomicSet(&p->decoder_done, 1);
                        return 0;
                    }
                    exhausted = 1;
                    did_work = 1;
                } else {
                    SDL_Log("track decode failed");
                    SDL_AtomicSet(&p->decoder_done, 1);
                    return 0;
                }
            }
        } else if (SDL_AudioStreamAvailable(p->stream) == 0) {
            /* Everything is in the ring; the callback raises EOF when it
             * drains. */
            SDL_AtomicSet(&p->decoder_done, 1);
            return 0;
        }

        if (!did_work)
            SDL_Delay(5);
    }

    return 0;
}

/* Pause the device, join the decoder thread, free the stream and reset the
 * ring. Only call on the UI thread: the callback must be stopped before the
 * ring indices are reset. */
static void
stop_session(struct Playback *p)
{
    if (p->dev_open)
        SDL_PauseAudioDevice(p->dev, 1);

    if (p->thread_running) {
        SDL_AtomicSet(&p->quit_thread, 1);
        /* A live decoder may be blocked on the socket: wake it first so the
         * join does not wait out the network timeout. */
        track_interrupt(p->track);
        SDL_WaitThread(p->thread, NULL);
        p->thread = NULL;
        p->thread_running = 0;
    }

    if (p->stream) {
        SDL_FreeAudioStream(p->stream);
        p->stream = NULL;
    }

    SDL_AtomicSet(&p->rb_read, 0);
    SDL_AtomicSet(&p->rb_write, 0);
    SDL_AtomicSet(&p->frames_played, 0);
    SDL_AtomicSet(&p->decoder_done, 0);
    SDL_AtomicSet(&p->eof, 0);
    SDL_AtomicSet(&p->state, MP_STOPPED);
    p->live_paused = 0;
    p->position_base = 0.0;
}

static void
set_error(struct Playback *p, const char *msg)
{
    SDL_LockMutex(p->meta_mutex);
    snprintf(p->error, sizeof p->error, "%s", msg);
    SDL_UnlockMutex(p->meta_mutex);
}

/* Start a decoder session at position_base. The device must be paused and no
 * decoder thread running: the ring is reset here. On failure the session stays
 * stopped and the meta error is set for the UI. */
static int
start_session(struct Playback *p)
{
    SDL_AtomicSet(&p->quit_thread, 0);
    SDL_AtomicSet(&p->decoder_done, 0);
    SDL_AtomicSet(&p->eof, 0);
    SDL_AtomicSet(&p->frames_played, 0);
    SDL_AtomicSet(&p->rb_read, 0);
    SDL_AtomicSet(&p->rb_write, 0);

    /* For a live stream this reconnects if the last connection was used. A
     * file track ignores the seek result, as it always has. */
    if (track_seek(p->track, src_frame_for(p, p->position_base)) != 0
        && p->info.is_live) {
        set_error(p, "cannot reconnect to stream");
        return -1;
    }

    /* Rebuild rather than clear: a fresh stream also resets the resampler,
     * which is what a seek wants. */
    p->stream = SDL_NewAudioStream(p->info.src_spec.format,
        p->info.src_spec.channels, p->info.src_spec.freq,
        p->have.format, p->have.channels, p->have.freq);
    if (!p->stream) {
        SDL_Log("SDL_NewAudioStream failed: %s", SDL_GetError());
        set_error(p, "cannot start audio conversion");
        return -1;
    }

    p->thread = SDL_CreateThread(decoder_thread, "decoder", p);
    if (!p->thread) {
        SDL_Log("SDL_CreateThread failed: %s", SDL_GetError());
        SDL_FreeAudioStream(p->stream);
        p->stream = NULL;
        set_error(p, "cannot start decoder thread");
        return -1;
    }
    p->thread_running = 1;
    return 0;
}

static void
release_device(struct Playback *p)
{
    if (p->dev_open) {
        SDL_CloseAudioDevice(p->dev);
        p->dev_open = 0;
        p->dev = 0;
    }
    free(p->ring);
    p->ring = NULL;
    free(p->scratch);
    p->scratch = NULL;
    if (p->meta_mutex) {
        SDL_DestroyMutex(p->meta_mutex);
        p->meta_mutex = NULL;
    }
}

int
playback_init(void)
{
    SDL_AudioSpec want;

    if (g.inited)
        return 0;

    memset(&g, 0, sizeof g);
    SDL_AtomicSet(&g.state, MP_STOPPED);
    SDL_AtomicSet(&g.volume_q8, 256);
    g.volume_percent = 100;

    g.meta_mutex = SDL_CreateMutex();
    if (!g.meta_mutex) {
        SDL_Log("SDL_CreateMutex failed: %s", SDL_GetError());
        return -1;
    }

    SDL_memset(&want, 0, sizeof want);
    want.freq = 44100;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = &g;

    /* allowed_changes = 0: take our spec or fail; `have` is ground truth. */
    g.dev = SDL_OpenAudioDevice(NULL, 0, &want, &g.have, 0);
    if (g.dev == 0) {
        SDL_Log("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        release_device(&g);
        return -1;
    }
    g.dev_open = 1;

    if (g.have.format != AUDIO_F32SYS || g.have.channels == 0) {
        SDL_Log("audio device returned an unexpected format");
        release_device(&g);
        return -1;
    }

    g.ring = (float *)malloc((size_t)RB_FRAMES * g.have.channels * sizeof(float));
    g.scratch = (Uint8 *)malloc(SCRATCH_BYTES);
    if (!g.ring || !g.scratch) {
        SDL_Log("out of memory allocating playback buffers");
        release_device(&g);
        return -1;
    }

    g.inited = 1;
    return 0;
}

int
playback_available(void)
{
    return g.inited && g.dev_open;
}

void
playback_shutdown(void)
{
    if (!g.inited)
        return;

    stop_session(&g);
    if (g.track) {
        track_close(g.track);
        g.track = NULL;
    }
    release_device(&g);
    g.inited = 0;
}

int
playback_load(const char *path, char *err, size_t errlen)
{
    TrackInfo info;
    Track *t;
    char errbuf[256] = "";
    char tag_line[256] = "";

    if (err && errlen)
        err[0] = '\0';

    if (!g.inited || !g.dev_open) {
        if (err && errlen)
            snprintf(err, errlen, "audio device is not available");
        return -1;
    }

    /* Open the candidate before touching the live session: a bad file must
     * leave the current track playing exactly as it was. A slow open (VBR MP3
     * header scan, whole-file WAV) runs while the old track keeps playing. */
    t = track_open(path, &info, errbuf, sizeof errbuf);
    if (!t) {
        const char *msg = errbuf[0] ? errbuf : "cannot load file";

        set_error(&g, msg);
        if (err && errlen)
            snprintf(err, errlen, "%s", msg);
        return -1;
    }

    /* Only now stop and replace the live session. */
    stop_session(&g);
    if (g.track) {
        track_close(g.track);
        g.track = NULL;
    }

    /* Local files show their embedded tags in place of the filename. Read
     * before taking the lock so status reads on the UI thread never wait on
     * disk. */
    if (!info.is_live) {
        char artist[120], title[120];

        if (tags_read(path, artist, sizeof artist, title, sizeof title)) {
            if (artist[0] && title[0])
                snprintf(tag_line, sizeof tag_line, "%s - %s", artist, title);
            else
                snprintf(tag_line, sizeof tag_line, "%s",
                         title[0] ? title : artist);
        }
    }

    g.track = t;
    g.info = info;

    SDL_LockMutex(g.meta_mutex);
    snprintf(g.tag_text, sizeof g.tag_text, "%s", tag_line);
    g.has_track = 1;
    g.duration = info.src_spec.freq > 0
        ? (double)info.total_frames / (double)info.src_spec.freq
        : 0.0;
    snprintf(g.name, sizeof g.name, "%s", path);
    g.error[0] = '\0';
    SDL_UnlockMutex(g.meta_mutex);

    playback_play();
    return 0;
}

void
playback_play(void)
{
    PlaybackState st;

    if (!g.inited || !g.dev_open || !g.track)
        return;

    st = (PlaybackState)SDL_AtomicGet(&g.state);

    if (st == MP_PLAYING)
        return;
    if (st == MP_PAUSED) {
        SDL_AtomicSet(&g.state, MP_PLAYING);
        return;
    }

    /* STOPPED: (re)build the session from position_base, which a seek made
     * while stopped may have moved. The device is paused here, so it is safe
     * to reset the ring. */
    if (g.thread_running) {
        double resume_at = g.position_base;
        stop_session(&g);
        g.position_base = resume_at;
    }

    if (start_session(&g) != 0)
        return;

    g.live_paused = 0;
    SDL_AtomicSet(&g.state, MP_PLAYING);
    SDL_PauseAudioDevice(g.dev, 0);
}

void
playback_pause(void)
{
    if (!g.inited)
        return;
    if (SDL_AtomicGet(&g.state) != MP_PLAYING)
        return;
    /* A live stream cannot resume where it left off, so pause stops it: Play
     * reconnects instead of playing stale buffered audio. */
    if (g.info.is_live) {
        stop_session(&g);
        g.live_paused = 1;
        return;
    }
    /* Keep the device running: the callback outputs silence while paused, so
     * resume is gapless. */
    SDL_AtomicSet(&g.state, MP_PAUSED);
}

void
playback_stop(void)
{
    if (!g.inited)
        return;
    stop_session(&g);
}

int
playback_seek(double seconds)
{
    PlaybackState prev;
    double dur;

    if (!g.inited || !g.dev_open || !g.track || g.info.is_live)
        return -1;

    SDL_LockMutex(g.meta_mutex);
    dur = g.duration;
    SDL_UnlockMutex(g.meta_mutex);

    if (seconds < 0.0)
        seconds = 0.0;
    if (seconds > dur)
        seconds = dur;

    prev = (PlaybackState)SDL_AtomicGet(&g.state);

    if (prev == MP_STOPPED && !g.thread_running) {
        /* The device stays paused: this only moves the point Play starts at. */
        g.position_base = seconds;
        track_seek(g.track, src_frame_for(&g, seconds));
        return 0;
    }

    /* Playing and paused both run the device. Stop the session, move the
     * start offset and rebuild it; a brief audible gap is the accepted cost of
     * seeking without a callback-side gate. */
    stop_session(&g);
    g.position_base = seconds;
    if (start_session(&g) != 0) {
        SDL_AtomicSet(&g.state, MP_STOPPED);
        return -1;
    }

    SDL_AtomicSet(&g.state, prev == MP_PLAYING ? MP_PLAYING : MP_PAUSED);
    SDL_PauseAudioDevice(g.dev, 0);
    return 0;
}

int
playback_seek_relative(double delta)
{
    if (!g.inited || !g.dev_open || !g.track)
        return -1;
    return playback_seek(current_position(&g) + delta);
}

void
playback_set_volume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    g.volume_percent = percent;
    SDL_AtomicSet(&g.volume_q8, percent * 256 / 100);
}

void
playback_adjust_volume(int delta)
{
    if (!g.inited)
        return;
    playback_set_volume(g.volume_percent + delta);
}

void
playback_toggle(void)
{
    if (!g.inited)
        return;
    if ((PlaybackState)SDL_AtomicGet(&g.state) == MP_PLAYING)
        playback_pause();
    else
        playback_play();
}

int
playback_is_live(void)
{
    return g.inited && g.info.is_live;
}

void
playback_get_status(PlaybackStatus *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof *out);
    if (!g.inited)
        return;

    /* EOF is handled here, on the UI thread, exactly once. stop_session clears
     * the atomic, which makes this flag inherently one-shot. */
    if (SDL_AtomicGet(&g.eof)) {
        stop_session(&g);
        out->finished = 1;
    }

    out->state = (PlaybackState)SDL_AtomicGet(&g.state);
    out->position = current_position(&g);
    out->volume_percent = g.volume_percent;

    /* A paused device stops calling the callback, so stale levels would freeze
     * on screen. Report silence unless audio is actually flowing. */
    if (out->state == MP_PLAYING) {
        out->level_peak_l = (float)SDL_AtomicGet(&g.lvl_peak_l) / LEVEL_SCALE;
        out->level_peak_r = (float)SDL_AtomicGet(&g.lvl_peak_r) / LEVEL_SCALE;
        out->level_rms_l = (float)SDL_AtomicGet(&g.lvl_rms_l) / LEVEL_SCALE;
        out->level_rms_r = (float)SDL_AtomicGet(&g.lvl_rms_r) / LEVEL_SCALE;
    }

    /* g.info is written only on this thread (playback_load). */
    out->bits_per_sample = g.info.bits_per_sample;
    out->is_live = g.info.is_live;
    out->live_paused = g.live_paused;
    if (g.track && g.info.is_live)
        track_title(g.track, out->now_playing, sizeof out->now_playing);
    out->sample_rate = g.info.src_spec.freq;
    out->channels = g.info.src_spec.channels;
    if (g.info.codec)
        snprintf(out->codec, sizeof out->codec, "%s", g.info.codec);

    SDL_LockMutex(g.meta_mutex);
    out->has_track = g.has_track;
    out->duration = g.duration;
    if (!g.info.is_live)
        snprintf(out->now_playing, sizeof out->now_playing, "%s", g.tag_text);
    snprintf(out->name, sizeof out->name, "%s", g.name);
    snprintf(out->error, sizeof out->error, "%s", g.error);
    SDL_UnlockMutex(g.meta_mutex);
}
