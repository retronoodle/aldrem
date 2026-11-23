# Phase A — FLAC support (`dr_flac`)

## Goal

FLAC files play through the same engine as WAV/MP3: streaming decode, exact duration,
pause/stop, and seek, all behind the existing `track_*` vtable. Scope is
`docs/PRD-format-support.md` Phase A only (`.flac`, native FLAC container).

## Verified context

- `vendor/dr_flac.h` is already vendored (v0.13.5, public domain/MIT-0) and recorded in
  `vendor/VENDORED.md:20-26`, but not wired into the build.
- The dispatcher is the only place formats are registered: extension check
  `src/track.c:69`, magic sniff `src/track.c:78`, dispatch `src/track.c:107`, error string
  `src/track.c:137`. File browser filtering already delegates to `track_ext_supported()`
  (`src/file_browser.c:195`).
- Implementations expose one `track_<fmt>_open` symbol declared in `src/track_internal.h:21-22`
  and fill a `TrackOps` vtable as the first member of their `Track` struct.
- `TrackInfo` (`src/track.h:13-18`) needs `src_spec`, `total_frames`, `codec`,
  `bits_per_sample`. The status label prints bit depth only when > 0 (`src/main.c:436-442`).
- Playback is format-agnostic: it creates `SDL_AudioStream` from `info.src_spec`
  (`src/playback.c:308-310`), seeks the track before starting the decoder thread
  (`src/playback.c:304`), and requests chunks with a 64 KiB scratch buffer
  (`src/playback.c:216`, `src/playback.c:25`). No playback changes are expected.
- `dr_flac` skips leading ID3v2 tags (`vendor/dr_flac.h:8397`), reports exact
  `totalPCMFrameCount` from STREAMINFO, and its S32 reader left-shifts low bit depths to full
  32-bit range (`vendor/dr_flac.h:10604`), so S32 is lossless for 16- and 24-bit sources.
- Build: `SRC`/`HDR` at `Makefile:5-10`; `-Ivendor` is already present; no new link libs.
- `ffmpeg` is available for generating fixtures; there are no audio fixtures in the repo.

## Decisions

- **Always decode to `AUDIO_S32SYS`**, independent of source depth. The engine converts
  S32→F32 for the device; 24-bit stays intact. `bits_per_sample` still reports the source
  depth from STREAMINFO (`drflac->bitsPerSample`, e.g. 16 or 24), not 32.
- **Mono/stereo only.** Reject >2 channels at open with a clear error. SDL2's stream
  conversion is only guaranteed for mono↔stereo; multichannel FLAC is out of scope.
- **Known length is required.** If `totalPCMFrameCount == 0` (STREAMINFO says unknown),
  fail open rather than report a 0-second track with broken seeking.
- **Native FLAC only.** Define `DR_FLAC_NO_OGG` to drop the Ogg-FLAC mapping; `.ogg`/`.oga`
  stay Phase B's business.
- **Extension is authoritative** (unchanged rule): `.flac` always goes to `dr_flac`, so a
  mislabelled file fails loudly. Unknown extensions sniff the 4-byte `fLaC` magic.
  A known limitation: an extensionless FLAC with an ID3v2 prefix sniffs as MP3; with the
  `.flac` extension it opens correctly.
- **Truncated audio body** is treated as a clean early EOF with one `SDL_Log` warning, no
  crash (same acceptance as truncated MP3). A missing/invalid header fails `track_open`.

## Tasks

### 1. Vendor record

`vendor/dr_flac.h` is present and unmodified. Update only the last line of its
`vendor/VENDORED.md` entry to state it is now wired into `src/track_flac.c` with
`DR_FLAC_NO_OGG`, normalised to interleaved S32, and that Ogg-encapsulated FLAC / multichannel
FLAC are not supported. Do not touch the vendored header.

### 2. Declare the entry point

`src/track_internal.h`: add next to the existing declarations

```c
Track *track_flac_open(const char *path, TrackInfo *info, char *err, size_t errlen);
```

### 3. New `src/track_flac.c`

Include the implementation in this TU only:

```c
#define DR_FLAC_NO_OGG
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
```

If the vendored header emits warnings under `-Wall -Wextra`, wrap the include in
`#pragma clang diagnostic push/ignored` at this site only; never edit `vendor/`.
The target is zero warnings.

State and vtable, mirroring `track_mp3.c`:

```c
struct Track { TrackBase base; drflac *flac; };
static const TrackOps flac_ops = { flac_decode_chunk, flac_seek, flac_close };
```

`track_flac_open`:

1. Validate args (same "invalid arguments" shape as the other implementations).
2. Probe readability with `fopen(path, "rb")`; on failure → `cannot open '<path>'`.
   Close the probe handle. This gives a distinct I/O message because `dr_flac` returns NULL
   for both I/O and format errors.
3. `drflac_open_file(path, NULL)`; on NULL → `cannot decode FLAC '<path>'`.
4. Reject `channels` outside 1..2 → e.g. `unsupported FLAC channel count %u in '<path>'
   (mono or stereo only)`.
5. Reject `sampleRate == 0` or `totalPCMFrameCount == 0` → `cannot determine FLAC length of
   '<path>'` / `cannot decode FLAC '<path>'`.
6. `calloc` the Track (free the `drflac` and report `out of memory` on failure), store
   `flac`, set `base.ops`.
7. Fill info: `src_spec.format = AUDIO_S32SYS`, `channels`, `freq = (int)sampleRate`, all
   other spec fields zeroed; `total_frames = totalPCMFrameCount` clamped to
   `0xffffffffu` (same 32-bit limit as the other formats); `codec = "FLAC"`;
   `bits_per_sample = (int)flac->bitsPerSample` (0 is not possible for valid FLAC, but the
   UI handles it).

`flac_decode_chunk(t, dst, cap)`:

- `bpf = 4 * channels` (interleaved `drflac_int32`). `frames = cap / bpf`; if 0 return `-1`.
- `got = drflac_read_pcm_frames_s32(t->flac, frames, (drflac_int32 *)(void *)dst)`.
- `got == 0`: if `t->flac->currentPCMFrame < t->flac->totalPCMFrameCount`, `SDL_Log` once
  that the stream ended early (truncated/corrupt). Return 0 — the decoder thread flushes and
  the track stops cleanly. Otherwise it is a normal EOF.
- Return `(int)(got * bpf)`; `dr_flac` only returns short reads at end of stream.

`flac_seek(t, frame)`:

- Clamp `frame` to `totalPCMFrameCount` (dr_flac clamps too).
- Return `0` when `drflac_seek_to_pcm_frame(t->flac, frame)` is true, `-1` otherwise.
  Seeks are sample-accurate via the seek table / binary search, satisfying the ~1 s PRD
  requirement for valid files.

`flac_close`: `drflac_close(t->flac)` then `free(t)`; NULL-safe.

### 4. Register in the dispatcher — `src/track.c`

- `enum { ..., SNIFF_FLAC, SNIFF_IOERR }`.
- `track_ext_supported()`: `|| ext_is(name, "flac")`.
- `sniff()`: after the RIFF check, add
  `if (n >= 4 && memcmp(buf, "fLaC", 4) == 0) return SNIFF_FLAC;`.
  The MP3 sync check cannot collide (`'f' != 0xff`).
- `track_open()`: extension branch `if (ext_is(path, "flac")) return track_flac_open(...)`
  and a `case SNIFF_FLAC:` dispatch.
- Unsupported-type message: `expected .wav, .mp3 or .flac`.

### 5. UI strings — `src/main.c`

- Empty-state hint (`src/main.c:482`): `drop a .wav, .mp3 or .flac here`.
- Leave the status/label layout alone; FLAC renders through the existing
  `codec bit-depth sample-rate channels` path.
- No other main.c/playback.c changes.

### 6. Build — `Makefile`

- Add `src/track_flac.c` to `SRC` (`Makefile:5`).
- Add `vendor/dr_flac.h` to `HDR` (`Makefile:8-10`).
- No flag or library changes.

## Edge cases / failure modes

- **Corrupt/missing header, non-FLAC .flac:** open fails with the human-readable message;
  `playback_load` keeps the current track untouched (`src/playback.c:438-449`).
- **Truncated audio body:** plays up to the damage, logs once, stops at EOF; no hang/crash.
- **Seek to/after end:** position clamps to `total_frames`; session immediately EOFs and the
  normal STOPPED-at-0:00 path runs.
- **24-bit / 96 kHz / mono:** S32 source → SDL resamples/converts to the float32 stereo
  device; status label shows the source depth/rate/channel count.
- **`.flac` containing an MP3** (or garbage): dr_flac rejects it; no cross-format fallback.
- **ID3v2-prefixed FLAC with `.flac` extension:** works (dr_flac skips ID3).
- **Multichannel FLAC:** refused at open with a specific message rather than silently
  downmixing wrong.
- **> ~27 h at 44.1 kHz:** `Uint32 total_frames` saturation, same limit as WAV/MP3.
- **Track length unknown in STREAMINFO:** refused at open.
- **Threading:** the `drflac` handle is touched only by `start_session`'s seek (UI thread,
  before the thread exists), the decoder thread, and close after join — no concurrent use.
- **S32 scratch cost:** 64 KiB chunk = 8192 stereo frames (~0.19 s at 44.1 kHz); decoder
  thread simply feeds more often. No playback tuning needed.

## Validation

1. `make clean && make` — zero warnings under `-std=c99 -Wall -Wextra` (after adding the
   pragma guard only if the header actually warns).
2. Generate fixtures under `~/.cache/claude-tmp/flac-fixtures/` with ffmpeg:
   stereo 16-bit/44.1 kHz, stereo 24-bit/96 kHz (`-sample_fmt s32 -bits_per_raw_sample 24`),
   mono 48 kHz, a short file truncated mid-audio, a copy with no extension, and an MP3
   renamed `.flac`. Confirm durations with `ffprobe`.
3. Launch with each fixture as `argv[1]`: app starts, status shows `FLAC <bits>-bit <rate>
   <channels>`, time label matches `ffprobe` within a second, seeking/stopping leaves the app
   responsive, truncated file stops cleanly and the app stays usable.
4. Back-to-back `WAV → MP3 → FLAC` loads in one session: no device reopen, no errors.
5. Regression: existing WAV/MP3 playback, seeking, pause/stop unchanged.
6. Hand off listening/seek-accuracy checks to Tim; he does manual testing on his own schedule.

## Risks

- **dr_flac warnings:** contain at the include site; never edit `vendor/`.
- **Untested SDL channel conversion:** avoided by restricting to mono/stereo.
- **Seek failure leaves cursor undefined:** `track_seek` reports `-1`, but `playback.c`
  ignores the return; on a damaged file playback may start from an arbitrary point. Accepted
  for Phase A; only affects corrupt streams.
- **Unknown-length FLACs** (streaming-encoded) are refused rather than mis-reported.

## Out of scope

- Ogg-encapsulated FLAC, multichannel FLAC, metadata/tag display, ReplayGain, gapless.
- Vorbis (Phase B), AIFF (Phase C), Opus/AAC.
- Changes to `playback.c`, the vtable signature, or the device setup.
- Tests/harness beyond the fixtures above.
