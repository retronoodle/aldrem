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
