# mediaplayer

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**A tiny desktop audio player, written in pure C.**

No Electron. No webview. No package manager tail. One small C99 codebase, SDL2 for the window and audio, Nuklear for the GUI, and vendored single-header decoders for everything else. It builds into a single binary and plays your music.

```
┌──────────────────────────────────────────────┐
│  Open  track.flac | FLAC 44100 Hz stereo     │
│               1:23 / 4:07                    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  │
│  Prev   Play   Pause   Stop   Next           │
│  vol 70%  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━    │
│  Playlist - 3 tracks                         │
│  Open list  Save as  Remove  Clear           │
│  > 01 - first.flac                           │
│    second.flac                               │
│    third.flac                                │
└──────────────────────────────────────────────┘
```

## Why

Most audio players are 200 MB of framework wrapped around `Play`. This one isn't. It's a readable program you can actually finish reading: a decoder thread fills a ring buffer, the SDL audio callback drains it, and the UI draws the controls. That's it. That's the whole trick.

- **Pure C99.** No C++, no Rust bindings, no runtime. If you can read C, you can read this.
- **One binary.** `make` and you're done. Vendored decoders mean no dependency hell at build time.
- **Actually plays your files.** WAV, MP3, FLAC, Ogg Vorbis, AIFF/AIFC — five formats, one interface.
- **Instant and lightweight.** Decode-ahead ring buffer, sample-accurate playhead, no dropouts.
- **Zero-fuss loading.** Drag a file onto the window, or use the built-in file browser.
- **A queue that survives a restart.** Everything you add is kept in a playlist, autosaved as plain `.m3u`, and waiting for you next time.

## Formats

| Format        | Extensions                     | Decoder        |
|---------------|--------------------------------|----------------|
| WAV           | `.wav`                         | SDL2 built-in  |
| MP3           | `.mp3`                         | minimp3        |
| FLAC          | `.flac`                        | dr_flac        |
| Ogg Vorbis    | `.ogg`, `.oga`                 | stb_vorbis     |
| AIFF / AIFC   | `.aiff`, `.aif`, `.aifc`       | custom reader  |

Format detection works by extension, with magic-byte sniffing as a fallback — rename the file and it still plays.

## Controls

| Input           | Action                     |
|-----------------|----------------------------|
| `Space`         | Play / pause               |
| `←` / `→`       | Seek back / forward 5 s    |
| `↑` / `↓`       | Volume up / down 5%        |
| `Esc`           | Close a dialog, then quit  |
| Drag & drop     | Append; play when idle     |
| Click a row     | Select it (Remove targets it) |
| Double-click    | Play that row immediately  |
| Prev / Next     | Previous / next entry      |

The progress and volume sliders are live: seek mid-track, adjust volume, and it takes effect immediately. Keys are global except while a dialog or the file browser is open, so typing a path works.

## Playlists

Anything you drop or open is appended to the playlist under the player. The track that is already playing keeps playing; when the player is idle, the first added file starts. Double-click a row to jump to it, and the end of a track auto-advances to the next one — playback stops at the end of the list, with no wrap.

The live list is autosaved to `$XDG_CONFIG_HOME/cside-mediaplayer/playlist.m3u` (usually `~/.config/cside-mediaplayer/playlist.m3u`; the same path on macOS) on every change and on quit, and restored at launch. `Save as` writes a standard `.m3u` file you can share or open elsewhere; `Open list` loads one, replacing the current queue. There is no reordering, repeat or shuffle in this version.

## Build

Requires a C compiler, `make`, `pkg-config`, and SDL2 development headers.

**macOS**

```sh
brew install sdl2
make
./mediaplayer
```

**Debian / Ubuntu**

```sh
sudo apt install build-essential pkg-config libsdl2-dev
make
./mediaplayer
```

**Fedora**

```sh
sudo dnf install gcc make pkgconf-pkg-config SDL2-devel
make
./mediaplayer
```

The build is plain `clang -std=c99 -Wall -Wextra`, and it stays warning-clean.

## Install

```sh
make install              # binary + .desktop entry + icon under ~/.local
make install PREFIX=/usr  # system-wide (with sudo)
```

On macOS, `make app` wraps the binary into a proper `mediaplayer.app` bundle with an `.icns` icon.

## Under the hood

```
main.c            SDL window, event loop, Nuklear UI, global shortcuts
playback.c        audio device, decoder thread, ring buffer, volume, seeking
playlist.c        queue model, .m3u read/write, autosave, auto-advance
track.c           format detection + dispatch to the right decoder
track_*.c         one small implementation per format
file_browser.c    minimal in-app directory browser
vendor/           Nuklear, minimp3, dr_flac, stb_vorbis
```

Every decoder implements the same tiny `TrackOps` vtable — open, decode a chunk, seek, close — so adding a format is one new file and a line in the dispatcher. The audio engine always converts to one fixed device format (44.1 kHz, stereo, float32) via `SDL_AudioStream`, so decoders never worry about resampling or channel mixing.

Shared state (position, duration, volume, status) sits behind a single mutex; the UI reads it every frame while the audio callback and decoder thread write it. Seeking flushes the ring buffer and repositions the decoder, so there's no stale audio left to hear.

## Dependencies

- [SDL2](https://www.libsdl.org/) — windowing, rendering, audio
- [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) — public domain immediate-mode GUI (vendored)
- [minimp3](https://github.com/lieff/minimp3) — public domain MP3 decoder (vendored)
- [dr_flac](https://github.com/mackron/dr_libs) — public domain FLAC decoder (vendored)
- [stb_vorbis](https://github.com/nothings/stb) — public domain Ogg Vorbis decoder (vendored)

Licenses for the vendored libraries live alongside them in [`vendor/`](vendor/); each vendored header keeps its own upstream license (Nuklear, minimp3, dr_flac and stb_vorbis are public domain/MIT-0, SDL2 is zlib).

## License

[MIT](LICENSE) © 2026 Tim Monaghan. This includes the app icon (`packaging/`).
