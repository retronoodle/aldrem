# mediaplayer — PRD: Additional File Formats

Extend the existing one-track player beyond WAV and MP3. Same constraints as the
main PRD: pure C99, SDL2 + Nuklear, vendored single-header decoders where
possible, and a codebase small enough to hold in your head.

## Goals

- Add FLAC, then Ogg Vorbis, behind the existing `TrackOps` interface.

- Keep every format interchangeable with WAV/MP3: decode, seek, duration.

- Prefer public-domain / MIT single-header decoders so the build stays
  dependency-free (`make` + `pkg-config sdl2`).

## Non-goals

- Playlists, metadata/tag display, gapless playback.
- Opus and AAC/ALAC in this pass (see "Deferred").
- RF64/W64 or other >4 GB WAV variants; AC3/DTS.

## Format priority

| Priority | Format   | Extensions        | Decoder           | Effort | Notes |
|----------|----------|-------------------|-------------------|--------|-------|
| 1        | FLAC     | `.flac`           | `dr_flac`         | Low    | Lossless, seekable, outputs up to 24-bit |
| 2        | Vorbis   | `.ogg`, `.oga`    | `stb_vorbis`      | Low    | Most-requested lossy after MP3 |
| 3        | AIFF     | `.aiff`, `.aif`, `.aifc` | custom reader | Low | macOS-native; `SDL_LoadWAV` does not handle it |
| Deferred | Opus     | `.opus`           | libopus + libogg  | Medium | Breaks single-header rule |
| Deferred | AAC/ALAC | `.m4a`            | external + MP4 demux | High | Container demux is the real cost |

## Architecture

Each format is a new `src/track_<fmt>.c` implementing the existing vtable in
`src/track_internal.h`:

```c
typedef struct TrackOps {
    int  (*decode_chunk)(Track *, Uint8 *, int);
    int  (*seek)(Track *, Uint32);
    void (*close)(Track *);
} TrackOps;
```

Decoders must emit the source PCM format in `TrackInfo.src_spec`; the playback
engine's `SDL_AudioStream` handles conversion to the fixed device format, so
implementations need no resampling or channel mixing of their own.

Integration checklist (same for every format):

1. Add `track_<fmt>_open` to `src/track_internal.h`.
2. Add the new `.c` to `SRC` in `Makefile` and its vendor header to `HDR`.
3. Extend `track_ext_supported()` — `src/track.c:69` (the single source of truth
   for the browser filter at `src/file_browser.c:195`).
4. Extend `sniff()` magic-byte detection — `src/track.c:78`.
5. Update the "unsupported file type" message (`src/track.c:137`) and the
   empty-state hint (`src/main.c:481`).

## Functional requirements

1. Extension is authoritative, as today: a known extension dispatches straight
   to its decoder with no cross-format fallback; mislabelled files fail loudly.
2. Unknown extensions fall back to magic-byte sniffing for the new formats.
3. A new format plays through the existing transport: play/pause/stop, seek,
   volume, end-of-track reset to STOPPED.
4. `TrackInfo` is filled correctly: `src_spec` (format/freq/channels),
   `total_frames`, `codec` label, `bits_per_sample` (0 when not meaningful).
5. Corrupt or truncated files fail `track_open` with a human-readable error and
   leave playback state clean (PRD functional req 7).
6. Seeking lands within ~1 s of the requested position.
7. Duration is exact for streamed/lossless formats (no VBR-style estimate).

## Phased tasks

### Phase A — FLAC (`dr_flac`)

- Vendor `dr_flac.h` (single header, public domain) into `vendor/`; record it in
  `vendor/VENDORED.md`.
- `src/track_flac.c`: `drflac_open_file`, stream PCM via
  `drflac_read_pcm_frames_*`, seek via `drflac_seek_to_pcm_frame`.
- Normalise output to a fixed `src_spec` format (S16 or S32) regardless of the
  file's bit depth; set `bits_per_sample` from the stream info.
- Duration from `drflac` total sample count (exact).
- Register in `track_internal.h`, `Makefile`, `track_ext_supported()`, `sniff()`
  (`fLaC` magic), and the UI strings.

**Done when:** a FLAC file plays, seeks, and reports duration through the same
pipeline as MP3; `make` is warning-clean.

### Phase B — Ogg Vorbis (`stb_vorbis`)

- Vendor `stb_vorbis.c` (single header, public domain) into `vendor/`.
- `src/track_ogg.c`: open with `stb_vorbis_open_filename`, decode with
  `stb_vorbis_get_samples_short_interleaved`, seek with
  `stb_vorbis_seek_frame`, close with `stb_vorbis_close`.
- Chained-stream caveat: treat as a single logical stream (documented limit).
- Register everywhere and add `OggS` sniffing.

**Done when:** a Vorbis file plays/seeks alongside WAV, MP3 and FLAC in one
session without reopening the audio device.

### Phase C — AIFF

- `src/track_aiff.c`: small custom reader for the AIFF/AIFC container
  (FORM/COMM/SSND chunks, big-endian PCM, `sowt` little-endian variant).
- Reuse the WAV approach: decode whole file into memory, `block_align` derived
  from the resolved `src_spec`, not the header.
- Register extensions (`.aiff`, `.aif`, `.aifc`) and `FORM`/`AIFF` sniffing.

**Done when:** AIFF and AIFC (including 24-bit) play and seek.

## Success criteria

- `make` builds with zero warnings under `-Wall -Wextra`.
- FLAC, Vorbis and AIFF files are listed in the file browser and play via
  drag-and-drop and the Open dialog, identical to WAV/MP3.
- Format switching mid-session does not reopen the audio device.
- Unsupported/corrupt files surface an error and never crash.

## Deferred

Opus and AAC/ALAC need external libraries and (for M4A) an MP4 demuxer, which
breaks the vendored single-header model. Revisit as a separate phase with an
explicit dependency decision.
