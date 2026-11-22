# Phase 3 — Seeking and accurate position

## Goal

Make the playhead trustworthy and let the user scrub: authoritative position from
the audio callback, correct `m:ss / m:ss` display, click/drag seek on the
progress bar, and `←`/`→` seeking ±5 s. Builds on `eea3647` (Phase 2 + HiDPI).

## Verified context

- Position today is `frames_played / have.freq` (`src/playback.c:495-497`).
  `frames_played` is reset to 0 at every session start (`playback.c:244,423`), so
  it cannot survive a seek.
- Seek plumbing already exists below the engine: `track_seek(Track*, Uint32 frame)`
  (`src/track.h:27-28`), WAV implementation at `src/track_wav.c:117-129` (clamps
  to the end). No changes needed in `track.h` / `track_wav.c`.
- Duration is already derived from decoded data length:
  `info.total_frames / info.src_spec.freq` (`playback.c:387-389`). For WAV,
  `SDL_LoadWAV` gives the data-chunk length (`track_wav.c:90`), so the Phase 3
  "duration from the data chunk" task is effectively already done — verify only.
- `playback_get_status`, `playback_play/pause/stop` are called only from
  `src/main.c`; internals can change freely.
- The UI already formats `m:ss / m:ss` (`main.c:238-249`) and disables the
  progress slider (`main.c:285-289`).
- HiDPI work is committed and active: layout and mouse coords are both in
  physical pixels (`main.c:101-118`), so new hit-testing needs no extra scaling.
- Nuklear's stock `nk_slider_float` only starts a drag on the 16 px handle, so
  click-to-seek needs a small custom widget.

## Decisions

- **Custom seek bar**, click anywhere, drag shows a time preview, engine seek on
  mouse release only (no scrubbing/seeking per-frame).
- **Seeking allowed in all states.** When STOPPED, the bar sets a resume point
  and Play starts there. Stop button and natural EOF still reset to 0:00 (PRD
  requirements 2 and 6).
- **Seek engine approach:** pause the audio device, join the decoder thread,
  reset the ring/stream, `track_seek`, rebuild the stream + thread, restore the
  previous state. Simpler and safer than a callback-side seek gate. A brief
  audible gap/click on seek is accepted for v1.

## Engine changes — `src/playback.c`, `src/playback.h`

### 1. Position model

Add to `struct Playback` (`playback.c:25-56`):

```c
double position_base;   /* seconds at the start of the current session (UI thread only) */
```

- `position_base` is written only on the UI thread (`load`, `stop`, `seek`, EOF).
- Position = `position_base + frames_played / have.freq`.
- `stop_session` (`playback.c:224-248`) sets `position_base = 0.0` (covers Stop,
  EOF, new load, shutdown).
- `playback_get_status` (`playback.c:480-506`) reports
  `out->position = g.position_base + frames_played / freq` after the existing
  EOF check.

### 2. Session start helper

Factor the STOPPED branch of `playback_play` (`playback.c:415-447`) into:

```c
/* Device must be paused and no decoder thread running. Seeks the track to
 * position_base, rebuilds the converter, starts the decoder thread. */
static int start_session(struct Playback *p);
```

It does: clear `quit_thread/decoder_done/eof`, zero `frames_played` and ring
indices, `track_seek(track, frame_for_seconds(position_base))`, create a fresh
`SDL_AudioStream`, create the decoder thread. On failure set the meta error and
return -1. `playback_play` then just sets state + unpauses the device.

Add small helpers:

```c
static Uint32 src_frame_for(const struct Playback *p, double seconds); /* round, clamp to total_frames */
static double current_position(const struct Playback *p);
```

### 3. Seek API

```c
int playback_seek(double seconds);            /* absolute, clamped to [0, duration] */
int playback_seek_relative(double delta);     /* uses current_position() */
```

Both return 0 on success, -1 if not initialised / no track. Relative is
implemented as `playback_seek(current_position(&g) + delta)`.

`playback_seek` flow:

```c
if (!inited || !dev_open || !track) return -1;
clamp seconds to [0, duration] (duration > 0 guard);
prev = state;

if (prev == MP_STOPPED && !thread_running) {
    g.position_base = seconds;
    track_seek(g.track, src_frame_for(&g, seconds));   /* device stays paused */
    return 0;
}

/* Playing or PAUSED: device is running in both states. */
SDL_PauseAudioDevice(g.dev, 1);
join decoder thread (quit_thread, WaitThread), free stream;
g.position_base = seconds;
if (start_session(&g) != 0) { state = MP_STOPPED; return -1; }
SDL_AtomicSet(&g.state, prev == MP_PLAYING ? MP_PLAYING : MP_PAUSED);
SDL_PauseAudioDevice(g.dev, 0);   /* PAUSED keeps running and outputs silence */
return 0;
```

`start_session` already clears `eof`, so a seek racing the end-of-track flag
cannot be undone by `playback_get_status`'s EOF handler.

Notes:
- The decoder thread polls in 5 ms steps, so the join adds only a few ms.
- `SDL_AudioStreamClear` exists in the installed SDL 2.32, but rebuilding the
  stream also resets resampler state, which is what a seek wants.

### 4. Do not change

- `audio_callback` (`playback.c:65-124`), ring buffer, or decoder loop logic
  beyond what `start_session` resets.
- `playback_init`'s device format.

## UI changes — `src/main.c` only

### 1. Custom seek bar

Add a static helper near the existing UI helpers:

```c
/* One row-sized seek bar. `frac` is 0..1. While dragging, writes the preview
 * fraction and keeps it live; on mouse release returns 1 (caller seeks). */
static int ui_seek_bar(struct nk_context *ctx, float s, float frac,
                       int enabled, int *dragging, float *preview)
```

Implementation:
- `struct nk_rect b; if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) return 0;`
  (consumes the layout row; no `nk_slider_float`).
- Input when `enabled`:
  - `nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, b)` → `*dragging = 1`,
    set `*preview` from `(mouse.x - b.x) / b.w` clamped to 0..1.
  - While `*dragging` and left mouse down → refresh `*preview` (clamped; allow
    pointer outside the bar).
  - While `*dragging` and mouse up → return 1, `*dragging = 0`.
  - When dragging, use `*preview` as the drawn fraction.
- Draw with `nk_window_get_canvas(ctx)` + `nk_fill_rect`:
  - track: 4 px * `s` tall, vertically centered, `nk_rgb(60, 60, 68)`;
  - played fill: same height, width `b.w * frac`, `nk_rgb(110, 150, 230)`;
  - handle: ~12 px * `s` rounded square at `frac`, `nk_rgb(220, 225, 235)`;
    omit the handle when `!enabled`.
- No `nk_widget_disable_*`; `enabled` is `st.has_track` and only gates input.

In the main loop keep drag state across frames:
`static int seek_dragging = 0; static float seek_preview = 0.0f;` (reset to 0
when `!st.has_track` to avoid a stale drag).

Replace the disabled `nk_slider_float` row (`main.c:285-289`) with a
`nk_layout_row_dynamic(ctx, ui_s(35.0f, ui_scale), 1)` row calling
`ui_seek_bar`; on return 1 call `playback_seek((double)seek_preview * st.duration)`.

### 2. Time label preview

While `seek_dragging`, show the preview time in the label (the label is drawn
one row above, so it uses the previous frame's preview — imperceptible):
`double shown = seek_dragging ? seek_preview * st.duration : st.position;`
Keep the existing floor-to-int `m:ss / m:ss` formatting (`main.c:238-249`).

### 3. Keyboard seek

In the raw SDL event loop (`main.c:208-228`, next to the Esc case):

```c
double pending_seek = 0.0;   /* declared before the loop */
...
else if (SDL_KEYDOWN && scancode == SDL_SCANCODE_LEFT)  pending_seek -= 5.0;
else if (SDL_KEYDOWN && scancode == SDL_SCANCODE_RIGHT) pending_seek += 5.0;
```

After `SDL_PollEvent` finishes for the frame, if `pending_seek != 0.0` call
`playback_seek_relative(pending_seek)` once and reset it. Coalescing per frame
prevents one decoder rebuild per key-repeat event; repeats are intentionally
honoured so holding an arrow keeps seeking. No-op when nothing is loaded.

### 4. Leave alone

Transport buttons, Open button, volume slider (still disabled until Phase 6),
DPI helpers, and the HiDPI style scaling.

## Edge cases

- Seek to the very end: session starts, immediately hits EOF, existing EOF path
  returns to STOPPED at 0:00 (PRD requirement 6).
- Seek while PAUSED: position updates via `position_base`; resume plays from
  there. Seeking while STOPPED sets the Play start point.
- Stop / new load / shutdown reset `position_base` to 0.
- `duration == 0` clamps every seek to 0.
- Seek failure (`start_session` alloc/thread failure): state STOPPED, error set,
  no crash.
- No track: bar draws empty, arrows no-op.

## Validation

1. `make clean && make` — zero warnings under `-Wall -Wextra`.
2. Generate a 10-minute marker WAV in `/tmp` (python3 stdlib, no ffmpeg/sox on
   this machine). Each 10 s block starts with a 0.1 s 1 kHz beep:
   ```python
   import wave, struct, math
   rate, secs = 44100, 600
   n = int(rate * 0.1)
   beep = b''.join(struct.pack('<hh', v, v) for v in
       (int(12000 * math.sin(2 * math.pi * 1000 * i / rate)) for i in range(n)))
   gap = b'\x00' * (int(rate * 9.9) * 4)
   w = wave.open('/tmp/seektest.wav', 'wb')
   w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
   for _ in range(secs // 10): w.writeframes(beep + gap)
   w.close()
   ```
   `25% / 50% / 90%` of 600 s = 150/300/540 s, all exact 10 s boundaries, so a
   correct seek lands on an immediate beep.
3. Play, click the bar at 25/50/90%: label jumps within ~0.2 s of the target,
   an audible beep starts immediately, playback continues.
4. Drag with button held: label previews, no audio jump until release; then it
   seeks once. Click without dragging also seeks.
5. Pause, seek to 50%: label updates, audio silent, resume plays from there.
6. Stop → `0:00`; seek to 25% while stopped; Play starts at 25%.
7. Hold `←`/`→`: clamps at 0 and at duration, no crash or runaway seek rate.
8. Seek near the end → auto-returns to STOPPED at `0:00`.
9. Compare a few timestamps against QuickTime Player (external reference) —
   within ~0.5 s.
10. 3-minute soak: repeated seeks while playing/paused, watch RSS, Esc quit with
    no SDL/audio warnings.

## Risks

- **Audible gap/click on seek** from pausing/restarting the device. Accepted for
  Phase 3; a callback-side seek gate is a later option if it bothers.
- **Key-repeat seek rate**: coalesced to one rebuild per frame; each rebuild is
  a join + alloc, measured in single-digit ms. If it stutters, add a ~100 ms
  coalescing window rather than per-frame.
- **Custom widget hit-testing**: coordinates are physical pixels end to end, so
  no HiDPI conversion is needed; test clicks at both ends and while resizing.
- **Stale drag state** after loading/stopping: reset `seek_dragging` when
  `!st.has_track`.

## Out of scope

- MP3 seeking/duration (Phase 4), volume wiring (Phase 6), file loading (Phase 5).
- Scrub-audio while dragging, waveform display, or gapless seek.
- Any change to `vendor/` or the Makefile.
