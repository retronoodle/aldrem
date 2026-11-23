# Phase B — Ogg Vorbis support (`stb_vorbis`)

## Goal

Ogg Vorbis files (`.ogg`, `.oga`) play through the same engine as WAV/MP3/FLAC:
streaming decode, duration, pause/stop, and seek, all behind the existing
`track_*` vtable. Scope is `docs/PRD-format-support.md` Phase B only.

## Verified context

- `vendor/stb_vorbis.c` is already vendored (self-reports v1.25, public domain) and
  recorded in `vendor/VENDORED.md:28-34`, but is not wired into the build.
- The file uses the stb header/implementation split: declarations end at
  `vendor/stb_vorbis.c:450`; the implementation is guarded by
  `#ifndef STB_VORBIS_HEADER_ONLY` (`vendor/stb_vorbis.c:456` … `:5676`). So it can
  be compiled as its own translation unit and included header-only elsewhere.
- Relevant API in the vendored copy:
  - `stb_vorbis_open_filename(const char *filename, int *error, ...)` — line 306;
    sets `STBVorbisError` (enum at `:409-443`), including `VORBIS_file_open_failure`
    and `VORBIS_outofmem`.
  - `stb_vorbis_get_info` — line 172; returns `sample_rate`, `channels`,
    `max_frame_size` (`:147-157`).
  - `stb_vorbis_stream_length_in_samples64` — line 345, returns granule-based exact
    length (0 on failure).
  - `stb_vorbis_seek64` — line 333. Unlike `stb_vorbis_seek_frame`, the next
    `get_samples_*` call starts **exactly** at the requested sample.
  - `stb_vorbis_get_samples_short_interleaved(f, channels, short *buf, int num_shorts)`
    — line 396; returns samples **per channel**, may be short at EOF (`:399-403`).
  - `stb_vorbis_seek_start`, `stb_vorbis_get_error`, `stb_vorbis_close` — lines
    341, 180, 183.
- Dispatcher registration points: `track_ext_supported` `src/track.c:69`, `sniff`
  `src/track.c:78`, dispatch `src/track.c:111`, error text `src/track.c:145`.
  The browser filter already delegates to `track_ext_supported()`
  (`src/file_browser.c:195`).
- Engine is format-agnostic: `SDL_AudioStream` is built from `info.src_spec`
  (`src/playback.c:308-310`), the track is seeked before the decoder thread starts
  (`src/playback.c:304`), and chunks are requested against a 64 KiB scratch buffer
  (`src/playback.c:216`, `:25`). `src_frame_for` can return `total_frames` exactly
  (`src/playback.c:74-85`), hence the end-of-stream seek guard below.
- `TrackInfo.codec` is copied into `PlaybackStatus.codec[8]` (`src/playback.h:17`,
  `src/main.c:427`); the label "Vorbis" fits. Bit depth is printed only when > 0
  (`src/main.c:436-442`).
- Build: `SRC`/`HDR` at `Makefile:5-10`; `-Ivendor` already present; no new libs.
- The launcher-packaging plan (`.kilo/plans/1795440127682-launcher-packaging.md:117-119`)
  expected `audio/flac;audio/ogg` in `MimeType` and `org.xiph.flac`/`org.xiph.ogg-audio`
  in `Info.plist` once these formats land; Phase A never did the packaging half.

## Decisions

- **Decode to `AUDIO_S16SYS`**, as the PRD specifies and as MP3 does. Vorbis has no
  native integer depth, so `bits_per_sample = 0` (label omits the bit depth).
- **`codec = "Vorbis"`** — fits the 8-byte status field; "Ogg Vorbis" would truncate.
- **Mono/stereo only**: reject >2 channels at open with a clear error (confirmed
  choice; matches `track_flac.c` and the engine's guaranteed conversion range).
- **Exact seeking via `stb_vorbis_seek64`**, not the PRD's `seek_frame`. PRD req 6
  allows ~1 s, but `seek64` matches the `get_samples_*` API we already use and
  removes up to one Vorbis block (~93 ms) of drift at no cost. Documented deviation.
- **Explicit end-of-stream flag**: because the engine may seek to `total_frames`
  exactly, and `stb_vorbis_seek64` rejects seeks past EOF, `at_end` short-circuits
  `decode_chunk` to a clean 0.
- **Length is required**: if the granule length is unknown/broken, fail open rather
  than report a wrong duration (PRD req 7).
- **Mid-stream decode errors end the stream cleanly**: log once, then report 0 (same
  acceptance as truncated FLAC/MP3). This is also how chained Ogg files terminate:
  only the first logical stream is played (documented limit).
- **Extension stays authoritative**; `OggS` is added to sniffing, so an
  extensionless Ogg reaches the Vorbis opener and fails loudly if it is Opus/FLAC/
  Speex-in-Ogg.
- **Vendored file compiled as its own TU**, included header-only in `track_ogg.c`.
  This keeps any third-party warnings isolated and is stb's intended usage.

## Tasks

### 1. Build wiring — `Makefile`

- `SRC`: add `vendor/stb_vorbis.c` (own object) and `src/track_ogg.c`.
- `HDR`: add `vendor/stb_vorbis.c` so dependent objects rebuild when it changes.
- No new flags or libraries; keep default stb config (do **not** define
  `STB_VORBIS_NO_STDIO` or `STB_VORBIS_NO_INTEGER_CONVERSION` — both APIs are used).
- After the first build, if `vendor/stb_vorbis.o` warns, add only the observed
  suppressions as a target-specific variable, e.g.
  `vendor/stb_vorbis.o: CFLAGS += -Wno-sign-compare -Wno-unused-function`,
  and note it in `VENDORED.md`. Never edit `vendor/stb_vorbis.c`.

### 2. New `src/track_ogg.c`

Include pattern (matches stb's split; `-Ivendor` is already set):

```c
#include "track_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
```

State and vtable, mirroring `track_flac.c`:

```c
struct Track {
    TrackBase base;
    stb_vorbis *v;
    Uint32 hz;
    int channels;
    Uint32 total_frames;
    int at_end;      /* seeking to/after EOF: decode returns 0 at once */
    int logged_err;  /* one SDL_Log per stream for corrupt/chained input */
};
static const TrackOps ogg_ops = { ogg_decode_chunk, ogg_seek, ogg_close };
```

`track_ogg_open`:

1. Validate args (`invalid arguments`), `calloc` the Track (`out of memory`).
2. `int verr = 0; v = stb_vorbis_open_filename(path, &verr, NULL);` — on NULL map:
   `VORBIS_file_open_failure` → `cannot open '<path>'`; `VORBIS_outofmem` →
   `out of memory`; otherwise → `cannot decode Ogg Vorbis '<path>' (Vorbis streams only)`.
3. `stb_vorbis_get_info(v)`: reject `channels < 1 || channels > 2` →
   `unsupported Ogg Vorbis channel count %d in '<path>' (mono or stereo only)`;
   reject `sample_rate == 0` → the decode message above.
4. `stb_vorbis_stream_length_in_samples64(v)`; if `<= 0` →
   `cannot determine Ogg Vorbis length of '<path>'`. Clamp to `0xffffffffu`
   (same `Uint32 total_frames` limit as the other formats).
5. `stb_vorbis_seek_start(v)`; on failure treat as a decode failure. (The engine
   seeks again before the first session; this just leaves the handle defined.)
6. Fill info: zero `src_spec`; `format = AUDIO_S16SYS`, `channels`, `freq =
   (int)sample_rate`; `total_frames`; `codec = "Vorbis"`; `bits_per_sample = 0`.
   Store `v`, `hz`, `channels`, `total_frames`; set `base.ops`. On any failure path:
   `stb_vorbis_close(v)` + `free(t)`.

`ogg_decode_chunk(t, dst, cap)`:

- Guards as elsewhere; if `at_end` return 0; if `cap <= 0` return 0.
- `bpf = 2 * channels`; `frames = cap / bpf`; if `frames <= 0` return -1.
- `n = stb_vorbis_get_samples_short_interleaved(t->v, t->channels,
  (short *)(void *)dst, frames * t->channels);` — request in shorts, return is
  frames per channel.
- If `n == 0`: `int e = stb_vorbis_get_error(t->v);` a nonzero `e` means corruption,
  truncation, or a chained-stream boundary (and `get_error` clears it). Log once
  (`SDL_Log("Ogg Vorbis stream ended early (truncated, corrupt or chained?)")`),
  set `at_end`, return 0. `e == 0` is a normal EOF.
- Return `n * bpf`.

`ogg_seek(t, frame)`:

- If `!v` return -1. If `frame >= total_frames`: `at_end = 1; return 0;`
  (decode then EOFs cleanly; see `src/playback.c:82-83`).
- `at_end = 0;` then `stb_vorbis_seek64(v, frame)`, returning 0 on success,
  -1 on failure. Valid-file seeks land exactly; PRD req 6 is comfortably met.

`ogg_close`: `stb_vorbis_close(v)` then `free(t)`; NULL-safe.

### 3. Dispatcher — `src/track.c` and `src/track_internal.h`

- `src/track_internal.h`: add
  `Track *track_ogg_open(const char *path, TrackInfo *info, char *err, size_t errlen);`
- `src/track.c`:
  - enum: add `SNIFF_OGG` before `SNIFF_IOERR`.
  - `track_ext_supported`: add `|| ext_is(name, "ogg") || ext_is(name, "oga")`.
  - `sniff`: next to the `fLaC` check, add
    `if (n >= 4 && memcmp(buf, "OggS", 4) == 0) return SNIFF_OGG;`
    (no collision with RIFF/fLaC/ID3 or the MPEG sync test).
  - `track_open`: extension branches for `ogg` and `oga`, plus `case SNIFF_OGG:`,
    both calling `track_ogg_open`.
  - Unsupported message → `expected .wav, .mp3, .flac, .ogg or .oga`.

### 4. UI copy and stale comments

- `src/main.c:482`: `drop a .wav, .mp3, .flac or .ogg here`.
- `src/main.c:3`, `src/file_browser.c:3`: mention Ogg Vorbis / use “supported audio
  files” so the browser comment stops enumerating a partial list.
- `src/track.h:2`: implementation list gains `track_ogg.c`.
- `src/playback.h:17`: label comment gains `"Vorbis"`.
- No layout changes: Vorbis renders through the existing `codec sample-rate
  channels` path (no bit depth).

### 5. Vendor record, packaging and docs

- `vendor/VENDORED.md:28-34`: replace “Not wired into the build yet — see …” with:
  wired via `src/track_ogg.c`, compiled as its own TU, decoded to interleaved S16;
  limits = mono/stereo only, first logical stream of a chained file only, Ogg
  Opus/FLAC/Speex not supported. Record any compile suppressions if step 1 needed
  them.
- `packaging/mediaplayer.desktop.in`: `Comment=Minimal WAV/MP3/FLAC/Ogg player`;
  append `audio/flac;audio/ogg;audio/x-vorbis+ogg;` to `MimeType` and `ogg;vorbis;`
  to `Keywords` (also closes the FLAC packaging gap left by Phase A).
- `packaging/Info.plist`: add `org.xiph.flac` and `org.xiph.ogg-audio` to the
  existing `LSItemContentTypes` array (per the launcher-packaging plan).
- `docs/PRD.md:17`: the “no FLAC/OGG yet” non-goal is now stale; reword to name the
  still-deferred formats (Opus, AAC/ALAC, AIFF until Phase C). One line only.

## Edge cases / failure modes

- **Opus/FLAC/Speex inside Ogg** (`.ogg`, `.oga`, or sniffed): Vorbis opener fails
  with the “(Vorbis streams only)” message; the current track is untouched
  (`src/playback.c:438-449`).
- **`.flac` containing Ogg FLAC**: unchanged, still rejected by dr_flac
  (`DR_FLAC_NO_OGG`), as documented in Phase A.
- **Chained Ogg**: first logical stream plays; the serial-number change surfaces as a
  decode error, which is logged once and treated as EOF. Duration covers the first
  stream only.
- **Truncated/corrupt body after a valid header**: plays up to the damage, logs once,
  stops cleanly; no hang, no crash.
- **Seek to/after end**: `at_end` forces immediate EOF → normal STOPPED-at-0:00 path.
- **Seek failure on a damaged file**: `track_seek` returns -1, but `playback.c`
  ignores it (same accepted risk as Phase A); the stream then ends early rather than
  inventing audio.
- **No final granule / broken last page**: open fails with “cannot determine Ogg
  Vorbis length” instead of a bogus duration.
- **Granule length vs decoded samples**: a few encoders trim a handful of samples at
  the end; the final chunk may come back short. Position stops marginally before the
  reported duration. Accepted.
- **Multichannel**: refused at open, matching FLAC.
- **> ~27 h at 44.1 kHz**: `Uint32 total_frames` saturation, same as other formats.
- **Threading**: the `stb_vorbis` handle is used by the UI thread only at open/seek
  (seek happens with the decoder joined, `src/playback.c:295-304`) and by the decoder
  thread for reads. No concurrent access.
- **Memory**: stb allocates its setup memory per open (tens–hundreds of KiB); freed
  by `stb_vorbis_close`. No per-chunk allocation.

## Validation

1. `make clean && make` — zero warnings under `-std=c99 -Wall -Wextra`, with
   suppressions (if any) confined to `vendor/stb_vorbis.o`.
2. `pkg-config --libs sdl2` unchanged: no new dependency, no link changes.
3. Code review checks: `track.c` dispatch order, `sniff` thresholds, and that
   `playback.c` is untouched (no device/session changes).
4. Optional fixture check with `ffmpeg`/`ffprobe` under
   `~/.cache/claude-tmp/ogg-fixtures/`: stereo 44.1 kHz and mono 48 kHz files;
   compare reported duration/rate/channels against `ffprobe`, plus a truncated copy
   and a `.ogg`-named Opus file to confirm the error paths. Playback/seek feel is
   Tim's own manual test.

## Risks

- **Third-party warnings**: isolate in the separate TU; never modify `vendor/`.
- **`get_error` semantics**: it clears the latched error, so call it only when a
  decode returns 0; otherwise a transient condition could be missed.
- **Seek-return ignored by the engine**: unchanged, pre-existing behaviour.
- **PRD deviation**: exact `stb_vorbis_seek64` instead of `seek_frame`; strictly
  better for our `get_samples_*` usage and still within the stated tolerance.

## Out of scope

- Opus, Ogg FLAC/Speex, chained-stream concatenation, metadata/tag display, gapless.
- AIFF (Phase C) and any changes to `playback.c`, the vtable, or device setup.
- Tests/harness beyond the build and optional fixtures above.
