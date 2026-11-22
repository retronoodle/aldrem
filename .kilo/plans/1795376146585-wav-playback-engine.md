# Phase 2 — WAV playback engine

Goal: `./mediaplayer somefile.wav` loads the file, plays it through an
SDL audio device, and supports Play / Pause / Stop correctly. No seeking, no
volume UI, no drag-and-drop — those stay in Phases 3–6.

Current state: `src/main.c` (125 lines) is the whole app — window, Nuklear
context, inert Phase 1 layout. `Makefile` compiles that one file. SDL 2.32.10
(MacPorts, via `pkg-config sdl2`). No test media in the repo.

## Decisions (locked)

| Decision | Choice |
|---|---|
| Layout | Split modules: `src/playback.{c,h}`, `src/track.h`, `src/track_wav.c`; UI stays in `main.c` |
| UI feedback this phase | Real `m:ss / m:ss` label + progress slider that *tracks* the playhead but stays non-draggable; volume slider still disabled (show 100%) |
| Pause | Do **not** pause the SDL device. Keep it running and output silence while `PAUSED` — gives instant, gapless resume |
| Position source | Device-format frames actually pulled from the ring buffer; never wall clock |
| Callback safety | Audio callback touches only `SDL_atomic_t` values + the ring buffer (no mutex, no allocation) |
| WAV loading | `SDL_LoadWAV` whole file into memory; a decoder thread replays it into the ring through `SDL_AudioStream` |
| Auto-play | A file passed on the CLI loads **and** starts playing |
| No CLI arg | App runs as in Phase 1: "no file loaded", transport buttons disabled |

Two deliberate, small departures from the PRD prose — flag in the commit body:
1. PRD Architecture says "shared state behind one mutex". We use atomics for
   everything the audio callback reads (`state`, `volume`, `frames_played`,
   `decoder_done`, `eof`) and keep a mutex only for UI-owned metadata
   (filename, duration, error string). Rationale: no locking in the callback.
2. PRD Phase 3 owns "duration from WAV data-chunk size". We compute duration in
   Phase 2 anyway (it falls out of the loaded spec) so the label isn't half
   empty. Seek/flush/keyboard remain Phase 3.

## Files

### `src/track.h` — player-agnostic source interface
```c
typedef struct Track Track;   /* opaque */

typedef struct {
    SDL_AudioSpec src_spec;   /* format of the PCM returned by decode_chunk */
    Uint32 total_frames;      /* at src_spec.freq, for duration */
} TrackInfo;

Track *track_open(const char *path, TrackInfo *info, char *err, size_t errlen);
/* Copies the next chunk of source-format PCM into dst (max cap bytes).
   Returns bytes written, 0 at end of stream, -1 on error. Advances the cursor. */
int    track_decode_chunk(Track *t, Uint8 *dst, int cap);
int    track_seek(Track *t, Uint32 frame);   /* implement now; no UI wiring until Phase 3 */
void   track_close(Track *t);
```
`track_wav.c` is the only implementation this phase. `track_seek` is trivial
(cursor = frame * block_align) but is written and unit-checked now so Phase 4's
MP3 implementation has the contract to fill in. Keep it frame-aligned.

Track owns its PCM buffer; `track_close` frees it.

### `src/playback.h` — engine API used by the UI
```c
typedef enum { MP_STOPPED, MP_PLAYING, MP_PAUSED } PlaybackState;

typedef struct {
    PlaybackState state;
    int    has_track;
    double position;      /* seconds */
    double duration;      /* seconds */
    int    volume_percent;/* 100 this phase */
} PlaybackStatus;

int  playback_init(void);          /* SDL_OpenAudioDevice, ring alloc */
void playback_shutdown(void);
int  playback_load(const char *path, char *err, size_t errlen); /* stop + open + autoplay */
void playback_play(void);
void playback_pause(void);
void playback_stop(void);
void playback_set_volume(int percent);
void playback_get_status(PlaybackStatus *out);
```
`playback_load` returns 0 on success; on failure writes a human-readable reason
into `err` (from `SDL_GetError()` or a track error), leaves any *previous*
track's state clean/stopped, and the UI shows `err` in the status label.

### `src/playback.c` — device, ring buffer, thread, state machine
- Init: `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)` moves to `main.c`'s init
  call; engine calls `playback_init()` after renderer setup.
- Device request: `freq 44100`, `AUDIO_F32SYS`, 2 channels, `samples 1024`,
  callback, allowed changes = 0. Store the returned `have` spec and treat it as
  ground truth everywhere (never hard-code 44100 after open).
- Ring buffer: single-producer/single-consumer, frame-granular.
  `RB_FRAMES = 1 << 17` (131072 frames ≈ 2.97 s at 44.1 kHz, 1 MiB).
  Monotonic `SDL_atomic_t` write/read counters masked on index; producer writes
  data then sets write counter with `SDL_MemoryBarrierReleaseFunction()`,
  consumer reads counter then data with the acquire barrier. No mutex.
- Audio callback:
  - read `state`; if not `PLAYING`, zero the output and return (do not advance).
  - else copy `min(avail, out_frames)` frames, zero the remainder (underrun),
    scale each float by `volume_q8/256.0f`, `SDL_AtomicAdd(&frames_played, n)`.
  - if ring was empty and `decoder_done` is set, set `eof = 1` and leave the rest
    silent (UI thread performs the actual transition; callback never mutates state).
- Decoder thread (SDL_CreateThread), one per playback session:
  1. while free frames < target: `SDL_AudioStreamGet` converted frames → ring write.
  2. if stream has < ~0.5 s queued and track not exhausted: `track_decode_chunk`
     into a fixed scratch buffer (allocated once), `SDL_AudioStreamPut`.
  3. on track exhaustion: `SDL_AudioStreamFlush` once, keep draining the stream
     into the ring, then set `decoder_done` and exit.
  4. check a `quit_thread` atomic at the top of each loop; cap sleeps so it
     exits well under a second (e.g. `SDL_Delay(5)` when the ring is full).
- `SDL_AudioStream` is created per playback session with src = track spec,
  dst = `have` spec; freed when the thread joins.
- Lifecycle rules (all on the UI thread):
  - `playback_play`: if STOPPED and a track is loaded → reset ring + `frames_played`,
    `track_seek(0)`, create stream, spawn thread, `SDL_PauseAudioDevice(dev, 0)`,
    state = PLAYING. If PAUSED → state = PLAYING only.
  - `playback_pause`: PLAYING → PAUSED only (device keeps running).
  - `playback_stop`: pause the *device*, set `quit_thread`, join, free stream,
    reset ring and `frames_played`, state = STOPPED. Track stays loaded.
  - EOF: UI thread sees `eof`, calls the same teardown path but also resets
    `frames_played` so the label shows `0:00 / …` and state is STOPPED.
  - `playback_shutdown`: stop, then `track_close`, `SDL_CloseAudioDevice`.
- Position: `frames_played / have.freq`. `SDL_atomic_t` is 32-bit → wraps after
  ~13 h at 44.1 kHz; accepted for v1 (note in a comment).
- Duration: `track_info.total_frames / src_spec.freq` (exact for WAV).
- Mutex (`SDL_CreateMutex`) guards only the loaded path/name, duration and error
  string, so `playback_get_status` can copy them safely.

### `src/main.c` — UI wiring (Phase 1 layout preserved)
- `int main(int argc, char **argv)`; after `playback_init()`, if `argc > 1`
  call `playback_load(argv[1], err, sizeof err)` and surface failure in the
  status label.
- Each frame: `playback_get_status()` before building widgets; format
  `"%d:%02d / %d:%02d"` into a buffer and display it.
- Set `progress = duration > 0 ? position / duration : 0` **before**
  `nk_slider_float`; keep it inside `nk_widget_disable_begin/end` so it can't be
  dragged (Phase 3 removes that).
- Play / Pause / Stop: use the `nk_button_label` return value to call the engine.
  Disable all three (existing `nk_widget_disable_begin/end` pattern) only when
  no track is loaded.
- Volume slider: stays disabled; drive its value to 1.0 and label `vol 100` so it
  doesn't lie about the engine's volume. Phase 6 wires it.
- Status label: filename when loaded, error text on failure, "no file loaded" otherwise.
- Shutdown order: `playback_shutdown()` → `nk_sdl_shutdown()` → destroy renderer/
  window → `SDL_Quit()`. The thread must be joined before `SDL_Quit` or macOS
  prints leak warnings.
- Leave Esc/quit handling, resize handling and all Nuklear setup untouched.

### `Makefile`
Compile objects instead of one translation unit:
```make
SRC := src/main.c src/playback.c src/track_wav.c
OBJ := $(SRC:.c=.o)
HDR := src/playback.h src/track.h vendor/nuklear.h vendor/nuklear_sdl_renderer.h
mediaplayer: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)
$(OBJ): $(HDR)
clean: rm -f $(OBJ) mediaplayer
```
Keep `-std=c99 -Wall -Wextra`, `-Ivendor`, pkg-config flags, and `run` target.

## Task order

1. `Makefile` multi-object support + stub `playback.c`/`track.c` equivalents so the
   tree still links (or do 1–4 in one pass).
2. `track.h`, `track_wav.c`: open/load/spec/total_frames/decode_chunk/seek/close.
3. `playback.c`: device open, ring buffer, callback with volume + position, silence
   when not playing. Verify by loading a WAV and hearing audio (no UI wiring yet).
4. Decoder thread + `SDL_AudioStream` conversion + flush-at-EOF + `decoder_done`.
5. State machine: play/pause/stop/EOF teardown, thread join safety.
6. `main.c`: CLI arg, autoplay, buttons, time label, read-only progress bar,
   disabled volume slider, status/error label.
7. Cleanup: `playback_shutdown` ordering, `make clean && make` warning-free.

## Validation

Build/lint:
- `make clean && make` → zero warnings under `-Wall -Wextra`.
- `make run` with no args → Phase 1 behavior intact, buttons disabled.

Generate fixtures (no media in repo; use `python3` + stdlib `wave`, write to
`/tmp`, e.g. `/var/folders/.../T/kilo` tmpdir):
- 60 s sine @ 44100/16-bit/stereo (the happy path).
- 10 s sine @ 22050/mono/16-bit and @ 48000/stereo/24-bit (exercise
  `SDL_AudioStream` resampling + channel + format conversion).
- a 3 s file (quick EOF test) and a truncated/garbage `.wav` (error path).

Manual checks (all must pass):
- `./mediaplayer /tmp/x.wav` auto-plays; label ticks; slider advances without dragging.
- Pause: output stops within a frame or two, no click; resume has no gap; position frozen while paused.
- Stop: audio stops, position resets to 0; Play restarts from the top.
- Play to EOF: auto-stops, position back to 0, buttons still usable, Play replays.
- Pause *at* EOF and resume: still ends cleanly in STOPPED (no stuck PLAYING).
- Converted-format files play at correct pitch/speed (22050 and 48000 fixtures).
- Bad file: error text in the status label, app stays usable, quit is clean.
- Esc during playback: exits < 1 s, no SDL/leak warnings, no hanging thread.
- Volume is audible at full level while the slider reads 100.

## Risks / watch-outs

- **Flush at EOF** — forgetting `SDL_AudioStreamFlush` truncates the last few ms
  and can leave `decoder_done` true with samples still queued. Drain, then mark done.
- **EOF must be one-shot** — transition on the UI thread off an `eof` flag, or the
  callback and UI thread will race on the state.
- **Ring reset while the callback is live** — only reset indices when the device
  is paused (stop path and pre-play path); never mid-playback.
- **`have` ≠ requested spec** — use `have.freq` for all time math; hard-coded
  44100 would misreport position if CoreAudio ever resamples.
- **Memory** — whole-file WAV is fine for Phase 2 but a 1 h stereo WAV is ~1.2 GB.
  Do not "fix" it here; the Phase 6 RSS check covers stability only.
- **`track_decode_chunk` frame alignment** — always return whole frames
  (multiples of block_align) so Phase 3 seeking stays exact.
- Don't touch `vendor/` or `docs/PRD.md`; don't add keyboard shortcuts or seek
  wiring (Phase 3/6 own those).
