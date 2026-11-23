# Contributing to Aldrem

Thanks for wanting to help. Aldrem is a small C program on purpose, and the rules below exist to keep it that way: readable, warning-clean, and easy to build anywhere.

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md). Security bugs go through [SECURITY.md](SECURITY.md), not the issue tracker.

## Build

You need a C compiler, `make`, `pkg-config`, and SDL2 development headers.

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libsdl2-dev
# Fedora
sudo dnf install gcc make pkgconf-pkg-config SDL2-devel
# macOS
brew install sdl2

make
./aldrem
```

The build is C99 with `-Wall -Wextra` and stays warning-clean. Rebuild from scratch after a change and check that the output is free of warnings:

```sh
make clean && make
```

`CC` is set to `clang` by default; override it the same way (`make CC=cc`) if you're testing with GCC or another compiler. The project targets clang's diagnostics: GCC's extra `-Wformat-truncation` checks fire on some `snprintf` calls that clang accepts, so a GCC build may print warnings that aren't project rules.

Useful targets: `make run`, `make install` (`PREFIX=...` to relocate), `make app` (macOS `.app` bundle), `make clean`.

## Code style

- C99, no compiler extensions. No POSIX-only calls: the code builds on Linux and macOS, so keep it portable.
- 4 spaces, LF line endings, UTF-8 (`.editorconfig` covers this; the `Makefile` uses tabs).
- No new warnings, ever. A PR that adds a warning is not ready to merge.
- Match the surrounding file. Small functions, short lines, brief comments that explain *why* rather than *what*.
- No new dependencies. The decoders are vendored single-header libraries on purpose (see [`vendor/VENDORED.md`](vendor/VENDORED.md)); adding one is a separate discussion, and it must be public domain or MIT-compatible.
- If you touch vendored headers, don't. They are upstream snapshots; the license headers stay untouched.

## Architecture in brief

| File | Role |
|------|------|
| `src/main.c` | SDL window, event loop, Nuklear UI, global shortcuts |
| `src/playback.c` | audio device, decoder thread, ring buffer, volume, seeking |
| `src/playlist.c` | queue model, `.m3u` read/write, autosave, auto-advance |
| `src/vu.c` | level-meter maths (dB mapping, ballistics); pure, no SDL |
| `src/settings.c` | persisted preferences in `settings.ini`; pure file I/O |
| `src/track.c` | format detection (extension + magic bytes) and dispatch |
| `src/track_*.c` | one decoder per format |
| `src/file_browser.c` | minimal in-app directory browser |
| `src/track.h` / `src/track_internal.h` | public track API and the `TrackOps` contract |
| `vendor/` | Nuklear, minimp3, dr_flac, stb_vorbis |

Everything decodes to source-format PCM through `TrackOps`; the audio engine converts to the fixed device format (44.1 kHz, stereo, float32) via `SDL_AudioStream`. Decoders never resample or mix channels.

Shared state (position, duration, volume, status) lives behind one mutex: the UI reads it each frame, the audio callback and decoder thread write it. Keep locking discipline intact when you change playback.

## Adding a format

A new format is one file plus a line in the dispatcher:

1. **Create `src/track_<format>.c`.** Define your own `struct Track` with `TrackBase base;` as its *first* member, plus whatever state you need (file handle, decoder context, cursor).
2. **Implement three functions** and fill a static `TrackOps`:

   ```c
   static int  myfmt_decode_chunk(Track *t, Uint8 *dst, int cap);
   static int  myfmt_seek(Track *t, Uint32 frame);
   static void myfmt_close(Track *t);

   static const TrackOps myfmt_ops = {
       .decode_chunk = myfmt_decode_chunk,
       .seek = myfmt_seek,
       .close = myfmt_close,
   };
   ```

   - `decode_chunk` copies the next chunk of source-format PCM into `dst` (at most `cap` bytes), returning bytes written (always whole frames), `0` at end of stream, `-1` on error. Advances the cursor.
   - `seek` positions the cursor at `frame`, clamped to the end of the stream; return `-1` on error.
   - `close` releases everything the file owns and frees the track.
   See `src/track_aiff.c` for a compact, dependency-free example.
3. **Implement `track_<format>_open(path, info, err, errlen)`.** On success allocate the track, set `base.ops`, and fill `info`: the source `SDL_AudioSpec` (`src_spec`), `total_frames` at `src_spec.freq`, a static `codec` string, and `bits_per_sample` (`0` when not meaningful). On failure return `NULL` with a human-readable message in `err`.
4. **Declare it in `src/track_internal.h`** next to the other `track_*_open` prototypes.
5. **Wire it into `src/track.c`**: add the extension to `track_ext_supported`, route it in `track_open`, and add magic-byte detection to `sniff` if the format has a recognisable header.
6. **Add the file to `SRC` in the `Makefile`.**
7. **Update the formats table and limitations in `README.md` and [`vendor/VENDORED.md`](vendor/VENDORED.md)** as appropriate.

If the decoder is vendored, add it under `vendor/` and document its upstream URL, version and license in `vendor/VENDORED.md`. A known extension is authoritative: `track_open` does not fall back across formats, so mislabelled files should fail loudly.

## Pull requests

- One focused change per PR. If it's unrelated to the title, split it.
- Explain the *why* in the description; the diff covers the what.
- Keep the build warning-clean (see above) and say which platforms you built on.
- New user-visible behaviour should be reflected in `README.md` (Controls, Formats, Playlists, limitations).
- Don't bump versions, add CI, or reformat unrelated files in a feature PR.
- Small, readable PRs get reviewed faster than large ones. Bugfixes and tests are always welcome.

## Reporting bugs and requesting features

Use the issue templates in `.github/ISSUE_TEMPLATE/`. For bugs, include the platform, how you built it, the file format that misbehaved (a few seconds of audio that reproduces it helps), and any terminal output.

## CI and releases (maintainers)

CI runs on every push and PR: a `build` matrix (Ubuntu with gcc and clang, macOS with clang) under `-Werror`, a `test` job that generates a WAV fixture and runs `make test FIXTURE=...` headlessly (`SDL_VIDEODRIVER=dummy`, `SDL_AUDIODRIVER=dummy`), and a `release` job that only runs on a `v*` tag.

Versions are SemVer tags `vMAJOR.MINOR.PATCH` on `main`. `make` derives the version from `git describe`, so the tag is the source of truth; `packaging/Info.plist` ships a placeholder that `make app` fills in. To cut a release:

```sh
# 1. move the version in docs/version, write the notes in docs/release-notes-<tag>.md
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
# 2. wait for the release job to create a draft with aldrem-linux-x86_64 and
#    aldrem.app.zip, review the notes, then publish it
```

