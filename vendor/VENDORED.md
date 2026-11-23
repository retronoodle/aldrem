# Vendored files

## Nuklear

Source repo: https://github.com/Immediate-Mode-UI/Nuklear — tag **v4.13.4** — fetched 2026-11-22.

- `nuklear.h` — single-header build generated from the official v4.13.4 tarball (`src/nuklear.h` alone is API-only since the repo split; the single header is produced by running the repo's own `src/build.py` with `src/paq.sh` arguments at that tag).
- `nuklear_sdl_renderer.h` — https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/v4.13.4/demo/sdl_renderer/nuklear_sdl_renderer.h

No modifications to either file. Backend in use: `demo/sdl_renderer` (SDL_Renderer API).

## minimp3

Source repo: https://github.com/lieff/minimp3 — commit **48e3a085f0e1a99446b34614e1e7f17f09dc2e82** — fetched 2026-11-22.

- `minimp3.h` — https://raw.githubusercontent.com/lieff/minimp3/48e3a085f0e1a99446b34614e1e7f17f09dc2e82/minimp3.h

No modifications. Only the core header is vendored (`minimp3_ex.h` is not). Public domain (CC0).

## dr_flac

Source repo: https://github.com/mackron/dr_libs — `master`, header self-reports **v0.13.5** — fetched 2026-11-23.

- `dr_flac.h` — https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h

No modifications. Choice of public domain or MIT-0; license statement at the end of the file. Wired into the build via `src/track_flac.c` with `DR_FLAC_NO_OGG`, normalised to interleaved S32. Ogg-encapsulated FLAC and multichannel FLAC are not supported.

## stb_vorbis

Source repo: https://github.com/nothings/stb — `master`, header self-reports **v1.25** — fetched 2026-11-23.

- `stb_vorbis.c` — https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c

No modifications. Public domain; license statement at the end of the file. Wired into the build via `src/track_ogg.c`, compiled as its own TU and decoded to interleaved S16. Limits: mono/stereo only; Ogg Opus/FLAC/Speex are not supported. For chained files, decoding stops at the first logical stream boundary while the length is taken from the file's final page, so duration and playback may be shorter than the first stream.
