# Launcher packaging: no terminal required

## Goal

`mediaplayer` is already a normal SDL2 desktop app — `main()` (src/main.c:188) never reads
stdin and only logs errors via `SDL_Log` to stderr. The terminal only exists because it is
launched by typing `./mediaplayer`, and closing that terminal SIGHUPs the app.

Ship two launchers so it starts detached, with an icon, and can open audio files from the
file manager:

- Linux (this machine, Ubuntu GNOME): `.desktop` entry + installed binary + SVG icon.
- macOS (the PRD's target platform): a `.app` bundle with `Info.plist`, `.icns`, and
  document types.

Plus one small code fix so the Open dialog starts somewhere sane when the launcher has no
useful cwd.

## Decisions (settled)

- `make install PREFIX=$HOME/.local` installs binary, `.desktop`, icon. `Exec=` gets the
  absolute installed path substituted at install time (GUI sessions do not reliably inherit
  `~/.local/bin` in `PATH`).
- File association via `MimeType=` + `%f`: "Open With mediaplayer" works, but the desktop
  entry does **not** become the default handler (no `mimeapps.list` edits, Linux; macOS uses
  `LSHandlerRank=Alternate`).
- macOS deliverable is `build/mediaplayer.app` built by `make app`; installing it (drag to
  /Applications) stays manual. No signing/notarisation — locally built bundles aren't
  quarantined.
- No DMG, no AppImage/deb/rpm, no Windows.
- Only register MIME types that actually decode today: `audio/mpeg`, `audio/wav`,
  `audio/x-wav`.

## Files

| Path | Action |
|------|--------|
| `src/file_browser.c` | edit: cwd → `$HOME` → `/` fallback |
| `packaging/mediaplayer.desktop.in` | new |
| `packaging/mediaplayer.svg` | new (128×128 scalable icon) |
| `packaging/make_icon.py` | new (stdlib-only PNG generator, feeds `.icns`) |
| `packaging/Info.plist` | new |
| `Makefile` | add `build/`, `install`, `uninstall`, `app` targets |
| `.gitignore` | add `build/` |

## Tasks

### 1. File-browser start directory (src/file_browser.c:277-280)

Finder-launched bundles and some launchers have cwd `/`, which would open the browser at the
filesystem root. Replace the tail of `fb_open()`:

```c
    const char *home = getenv("HOME");
    int cwd_usable = getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, "/") != 0;

    if (cwd_usable && fb_begin_scan(fb, cwd) == 0)
        return;
    if (home && home[0] && fb_begin_scan(fb, home) == 0)
        return;

    fb_begin_scan(fb, "/");
```

The `suggested_path` branch above it (src/file_browser.c:256-275) already handles argv[1] and
dropped files, so `%f` and "Open With" land on the right directory. No new includes beyond
what the file already has (`stdlib.h` is present for `getcwd`/`getenv`).

### 2. `packaging/mediaplayer.desktop.in`

```
[Desktop Entry]
Type=Application
Name=mediaplayer
GenericName=Audio Player
Comment=Minimal WAV/MP3 player
Exec=@BINDIR@/mediaplayer %f
Icon=mediaplayer
Terminal=false
Categories=AudioVideo;Audio;Player;
MimeType=audio/mpeg;audio/wav;audio/x-wav;
StartupNotify=true
StartupWMClass=mediaplayer
Keywords=audio;music;player;mp3;wav;
```

`StartupWMClass=mediaplayer` matches SDL2's default WM_CLASS / Wayland app_id (basename of
argv[0]).

### 3. Icons

- `packaging/mediaplayer.svg`: hand-written 128×128 icon — rounded-square background and a
  play triangle. Keep it flat and simple; no filters or embedded fonts.
- `packaging/make_icon.py`: pure-stdlib (`zlib`, `struct`) script that renders the same
  design to a 1024×1024 RGBA PNG with 4× supersampling. Usage:
  `python3 packaging/make_icon.py build/icon-1024.png`. Needed because `.icns` requires
  raster input and this box has no rsvg/inkscape/imagemagick. Verify with
  `file build/icon-1024.png` (expects "1024 x 1024").

### 4. `packaging/Info.plist`

Keys: `CFBundleName`/`CFBundleDisplayName` = mediaplayer, `CFBundleExecutable` =
mediaplayer, `CFBundleIdentifier` = `local.mediaplayer` (no claimed domain; change if you
prefer), `CFBundlePackageType` = APPL, `CFBundleShortVersionString` = 0.1,
`CFBundleIconFile` = mediaplayer.icns, `LSMinimumSystemVersion` = 11.0,
`NSHighResolutionCapable` = true, and:

```
CFBundleDocumentTypes = [ { CFBundleTypeName = Audio;
    CFBundleTypeRole = Viewer;
    LSHandlerRank = Alternate;
    LSItemContentTypes = (public.mp3, com.microsoft.waveform-audio,
                          public.aiff-audio); } ]
```

`public.aiff-audio` is listed deliberately: the file association should describe what the
player is *for*, and `track_open` already fails loudly on unsupported input. When FLAC/OGG
land (docs/PRD-format-support.md), add `org.xiph.flac`, `org.xiph.ogg-audio` here and
`audio/flac;audio/ogg` to `MimeType=`.

### 5. Makefile

```make
PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin
APPDIR   ?= $(PREFIX)/share/applications
ICONDIR  ?= $(PREFIX)/share/icons/hicolor/scalable/apps
DESKTOP  := build/mediaplayer.desktop
APP      := build/mediaplayer.app
```

- `build`: `mkdir -p build`.
- `$(DESKTOP): packaging/mediaplayer.desktop.in mediaplayer | build` →
  `sed 's|@BINDIR@|$(BINDIR)|g' $< > $@`. Pipe delimiter matters — the paths contain `/`.
- `install: mediaplayer $(DESKTOP)` →
  `install -Dm755 mediaplayer $(DESTDIR)$(BINDIR)/mediaplayer`,
  `install -Dm644 $(DESKTOP) $(DESTDIR)$(APPDIR)/mediaplayer.desktop`,
  `install -Dm644 packaging/mediaplayer.svg $(DESTDIR)$(ICONDIR)/mediaplayer.svg`,
  then `-update-desktop-database $(DESTDIR)$(APPDIR)` (leading `-` so a missing tool is not
  fatal).
- `uninstall`: remove those three paths, re-run `update-desktop-database`.
- `app: mediaplayer`, first line guards the platform:
  `@if [ "$$(uname -s)" != "Darwin" ]; then echo "app: macOS only (needs sips/iconutil)" >&2; exit 1; fi`
  then create `$(APP)/Contents/{MacOS,Resources}`, copy the binary and `Info.plist`, run
  `make_icon.py` into `build/`, `sips -z` the ten `icon_NxN[@2x].png` variants into
  `build/mediaplayer.iconset/`, and `iconutil -c icns build/mediaplayer.iconset -o
  $(APP)/Contents/Resources/mediaplayer.icns`.
- Add `build install uninstall app` to `.PHONY`. `make run` and `make clean` unchanged
  (`clean` does not delete `build/`, so `uninstall` still works after a clean; document that
  `rm -rf build` is the manual sweep).

### 6. `.gitignore`

Add `build/`.

## Out of scope

- Becoming the default audio handler; Windows; AppImage/flatpak/deb/rpm packaging.
- Ad-hoc codesigning / notarisation.
- Redirecting `SDL_Log` to a log file when no tty is attached (errors are already surfaced in
  the status label; revisit only if a launch failure is ever invisible).
- Relocating the file browser's "last directory" to XDG state.

## Risks

- `Exec=` with a substituted absolute path breaks if the user relocates `~/.local`; re-run
  `make install` after changing `PREFIX`. Acceptable.
- `install -D` is GNU coreutils; that target is Linux-only by design (macOS uses `app`).
- `%f` with a relative path from some file managers would resolve against the launcher cwd.
  GNOME passes absolute paths; the `suggested_path` branch handles both.
- MIME lists drift from `track_ext_supported()` (src/track.c:69) as formats land — the
  comment in task 4 is the reminder.

## Validation

- `make clean && make` — still warning-free under `-Wall -Wextra`.
- `make install PREFIX=$HOME/.local`, then:
  - `desktop-file-validate build/mediaplayer.desktop` — no output.
  - `grep '^Exec=' build/mediaplayer.desktop` — absolute path under `$HOME/.local/bin`.
  - `file $HOME/.local/share/icons/hicolor/scalable/apps/mediaplayer.svg` and
    `python3 packaging/make_icon.py build/icon-1024.png && file build/icon-1024.png`.
  - `update-desktop-database` exit 0.
- On macOS only: `make app`, `plutil -lint build/mediaplayer.app/Contents/Info.plist`,
  `ls build/mediaplayer.app/Contents/Resources/mediaplayer.icns`.
