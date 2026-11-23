# Aldrem v0.1.0

The first release: a tiny desktop audio player in pure C99. One binary, SDL2 for
the window and audio, Nuklear for the GUI, vendored single-header decoders for
everything else.

## What it does

- Plays **WAV, MP3, FLAC, Ogg Vorbis and AIFF/AIFC**, picked by extension with
  magic-byte sniffing as a fallback.
- Decoder thread fills a ring buffer; the SDL audio callback drains it. Seek,
  volume and progress are live, with a sample-accurate playhead.
- Drag and drop, a built-in file browser, and a playlist panel that autosaves to
  `~/.config/aldrem/playlist.m3u` and loads `.m3u` files via `Open list`.
- Keyboard control: `Space`, arrows, `Esc`.

## Downloads

| Asset | Platform |
|-------|----------|
| `aldrem-linux-x86_64` | Linux (x86_64), dynamically linked against SDL2 |
| `aldrem.app.zip` | macOS 11+, unzip and drag to Applications |

Linux users can also build it from source (`make`), or `make install` for the
binary, desktop entry and icon. macOS users can `brew install sdl2 && make`,
then `make app`.

The Linux binary needs `libsdl2` at runtime, e.g. `sudo apt install libsdl2-2.0-0`.

## Known limitations

- No reordering, repeat or shuffle in the playlist.
- Ogg is Vorbis only; Opus, Ogg FLAC and Speex are not decoded, and chained Ogg
  stops at the first stream.
- Mono and stereo only; multichannel FLAC is not supported.
- Embedded tags are not read; names come from the file path.

See the [README](https://github.com/retronoodle/mediaplayer#known-limitations)
for the full list.

## Building from source

```sh
sudo apt install build-essential pkg-config libsdl2-dev   # or: brew install sdl2
make
./aldrem --version
```

The build is plain `clang -std=c99 -Wall -Wextra` and is warning-clean under
gcc and clang in CI.

## Licensing

MIT. The vendored decoders keep their own upstream licenses (all public
domain/MIT-0); SDL2 is zlib and linked dynamically.
