# Phase 6 — Volume, polish, and last-mile fixes

## Context

Phases 1–5 are complete on `main`; three Phase 6 items already exist or are partially built:

- **HiDPI + denser UI is already done** (commit `eea3647`, `src/main.c:20-113`, `:245-284`). No work needed; just don't regress it.
- **The audio engine already applies volume** at mix time (`playback_set_volume`, `src/playback.c:570-579`, callback gain at `:140-147`). The UI slider is drawn but inert and disabled (`src/main.c:462-470`).
- **Global shortcuts are missing.** Esc and ←/→ are handled from raw SDL events; Space/↑/↓ are not (`src/main.c:286-328`).

The remaining work is UI wiring plus surfacing decoded-format metadata. No new files, no Makefile changes.

## Decisions

- Format/sample-rate/channels **append to the existing status label**: `song.wav  |  WAV 16-bit 44100 Hz stereo`. Error text still wins over both. ASCII only (the default Nuklear font has no “·”/“—” glyph).
- Volume is adjustable with **or without a track** (it survives loads); disabled only while the file browser is modal. Keyboard steps are 5.
- Space/↑/↓ are handled from `SDL_KEYDOWN` scancodes and **not forwarded to Nuklear**, so focused widgets cannot swallow or duplicate them.
- HiDPI is out of scope (already implemented).

## Implementation tasks

### 1. Add codec metadata to the track layer

- `src/track.h:12-15` — extend `TrackInfo`:
  - `const char *codec;` (`"WAV"`/`"MP3"`, static string owned by the implementation)
  - `int bits_per_sample;` (decoded width; `0` when not meaningful, e.g. MP3)
- `src/track.c:108-113` — zero `*info` at the top of `track_open` after the argument check, so future implementations cannot leak uninitialised fields.
- `src/track_wav.c:101-103` — set `info->codec = "WAV"` and `info->bits_per_sample = (int)SDL_AUDIO_BITSIZE(spec.format)`.
- `src/track_mp3.c:371-376` — set `info->codec = "MP3"` and `info->bits_per_sample = 0`.

### 2. Surface metadata through `PlaybackStatus` and add control helpers

- `src/playback.h:9-17` — add to `PlaybackStatus`:
  - `char codec[8];`
  - `int bits_per_sample;`
  - `int sample_rate;`
  - `int channels;`
  - Update the stale `volume_percent /* 100 this phase */` comment.
- `src/playback.h:18-38` — declare:
  - `void playback_toggle(void);` — pause if playing, otherwise play (no-op from STOPPED with no track).
  - `void playback_adjust_volume(int delta);` — clamp through `playback_set_volume`.
- `src/playback.c:581-605` — in `playback_get_status`, copy the new fields from `g.info` (`src_spec.freq`, `src_spec.channels`, `bits_per_sample`, `codec`); guard a NULL codec. `g.info` is only written on the UI thread in `playback_load`, so no extra lock is needed for these fields.
- `src/playback.c:570-579` — implement the two helpers next to `playback_set_volume`:
  - `playback_toggle`: `MP_PLAYING → playback_pause()`, otherwise `playback_play()`.
  - `playback_adjust_volume`: `playback_set_volume(g.volume_percent + delta)` (existing clamp handles bounds); guard on `g.inited`.

### 3. Wire the volume slider

- `src/main.c:392-393` — change the label to include the percent sign: `"vol %d%%"`.
- `src/main.c:462-470` — remove the unconditional `nk_widget_disable_begin/end`; wrap the slider in `nk_widget_disable_begin/end` **only when `browser_open`** (matches transport modal behavior).
- Capture the `nk_slider_float` return (it is true when the value changed) and call
  `playback_set_volume((int)lroundf(volume * 100.0f));`.
- The label is laid out before the slider, so the text reflects a drag one frame late; audio is immediate. Leave this as-is (imperceptible, avoids restructuring the row).

### 4. Global keyboard shortcuts from raw SDL events

In the event loop (`src/main.c:286-328`):

- Add a per-event `int consumed = 0;`.
- `SDL_KEYDOWN` + `SDL_SCANCODE_SPACE` + `!evt.key.repeat`: when the browser is closed, `playback_toggle()`. Mark consumed.
- `SDL_KEYDOWN` + `SDL_SCANCODE_UP`: when the browser is closed, `playback_adjust_volume(5)`. Allow key repeat so holding the key ramps smoothly. Mark consumed.
- `SDL_KEYDOWN` + `SDL_SCANCODE_DOWN`: same with `-5`.
- Consume these three keys even while the browser is open (do not act) so Nuklear never sees them.
- Guard the existing `nk_sdl_handle_event(&evt)` call with `if (!consumed)`. Leave Esc, ←/→, drop, and resize handling unchanged.
- Do not forward these keys to Nuklear; there are no text widgets in the app today, so this is safe.

### 5. Idle/empty hint

- `src/main.c:441-447` — in the seek-bar row, when `!st.has_track`, draw
  `nk_label(ctx, "drop a .wav or .mp3 here", NK_TEXT_CENTERED)` instead of the bar.
- Keep the row height identical so the layout does not jump when a track loads. Keep the `--:-- / --:--` time row and the disabled transport as they are.

### 6. Status label: filename + format

Replace the `const char *status_text` selection at `src/main.c:395-407` with a local
`char statusbuf[768];`:

- Error present → error text (unchanged priority).
- Track loaded → `basename  |  <format>` where `<format>` is:
  - channels: `mono`, `stereo`, or `"%d ch"` (>2);
  - if `bits_per_sample > 0`: `"%s %d-bit %d Hz %s"` → `WAV 16-bit 44100 Hz stereo`;
  - otherwise: `"%s %d Hz %s"` → `MP3 44100 Hz stereo`.
- Nothing loaded → `"no file loaded"`.

Keep the window title (`filename — mediaplayer`) unchanged.

### 7. Comment clean-up

- `src/main.c:1-5` — describe Phase 6 (volume, shortcuts, idle hint, format label).
- `src/playback.h` — remove the phase-specific volume comment.

### 8. Build

- `make clean && make` — must be zero warnings under the existing `-std=c99 -Wall -Wextra`.

## Functional requirement coverage after this phase

| PRD requirement | Status after Phase 6 |
|---|---|
| 1 opening replaces track at 0:00 | already done (`playback_load`) |
| 2 play/pause/stop | already done |
| 3 seek by drag | already done |
| 4 `m:ss / m:ss` | already done |
| 5 volume 0–100, immediate | task 3 (+ existing callback gain) |
| 6 natural end → STOPPED/0 | already done |
| 7 corrupt file → error label, clean state | already done |
| 8 Space/←→/↑↓/Esc | task 4 |
| 9 resizable progress bar | already done |

## Validation

- `make clean && make` with zero warnings.
- Headless startup/shutdown smoke if useful: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./mediaplayer`, then terminate; expect no crash and no SDL warnings. (The dummy backend may not invoke the audio callback, so this only proves lifecycle cleanliness.)
- Deferred to the user's usual manual pass: the PRD's 9-requirement check and 15-minute session, including resize-while-playing and quit-during-playback.
- Long-file memory: use an MP3 for the growth test (streaming path). Sample RSS while playing and seeking repeatedly; expect a flat line. A 1-hour WAV has a large *baseline* (~600 MB) because `SDL_LoadWAV` decodes the whole file by design (`src/track_wav.c:1-5`); that is not a leak. There is no loop mode, so “looping” means replaying/seeking a long file.

## Risks / notes

- **Long filenames** can clip the combined status label on narrow windows. Accepted: the chosen append layout keeps one row.
- **WAV bit depth** reflects what SDL delivered: 24-bit PCM is normalised to 32-bit by `SDL_LoadWAV`, so it may read `32-bit`. Avoids reading the original WAV header (out of scope).
- **Consuming Space/↑/↓** from Nuklear is safe because the app has no text-input widgets. Revisit if one is added.
- **Volume label** trails the drag by one frame; audio gain is immediate.
- **HiDPI** already works; if a specific display scale looks wrong, treat it as a separate fix.
