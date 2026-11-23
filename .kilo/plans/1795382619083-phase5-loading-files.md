# Phase 5 — Loading files: drag-and-drop + Open button

## Goal

Replace CLI-only loading with two in-app paths: SDL drag-and-drop and a minimal
Nuklear directory browser behind the Open button. A bad file must leave the
current track playing, and the window title follows the loaded file.

## Verified context

- CLI load is `src/main.c:261-265`; Open is an inert `nk_button_label`
  (`src/main.c:375-380`). The event loop is `src/main.c:284-309`; Esc currently
  quits unconditionally (`main.c:289-291`).
- Status text comes from `PlaybackStatus` (`src/playback.h:9-17`): `name` is the
  full path, `error` is the last load/start failure. Current status/title logic
  is `src/main.c:362-369`.
- **PRD conflict:** `playback_load` tears down the live session *before*
  `track_open`, and on failure clears `has_track`/`name` (`src/playback.c:432-451`).
  Phase 5 requires the previous playback state to survive a failed load, so this
  ordering must change.
- `track_open` is the single load path and already reports unsupported/corrupt
  files (`src/track.c:99-130`). No format work is needed.
- Upstream `example/file_browser.c` is a 31 KB GLFW/OpenGL app with icon sheets
  and a 4-column icon grid, not a drop-in header. User chose a custom browser.
- Build inputs: `SRC`/`HDR` in `Makefile:5-7`. Only `main.c` includes Nuklear
  with `NK_IMPLEMENTATION` today (`src/main.c:12-21`).

## Decisions

- New self-contained `src/file_browser.c` + `src/file_browser.h`; POSIX
  `dirent.h` / `stat` / `realpath`. No icons, no GLFW/OpenGL leftovers.
- `playback_load` opens the candidate track first and only then stops/swaps the
  live session. A failed open sets `error` and touches nothing else.
- The browser closes itself on file selection, Cancel, or Esc. Main then loads;
  if the load fails, the error is visible in the main status label.
- Browser start directory: last browsed dir; else the current track's directory;
  else `getcwd()`. Remembered for the session.
- No title-bar `X`: Nuklear hides a window on close and needs `nk_window_show` to
  reopen. Cancel + Esc are enough; the window stays `TITLE|MOVABLE`.
- While the browser is open, the main controls are inert and ←/→ global seeks are
  suppressed. Esc closes the browser instead of quitting.
- Keep optional `argv[1]` loading as a developer convenience; it is no longer
  required.

## Tasks

### 1. Engine: preserve playback on a failed load

`src/playback.c`, `playback_load` only:

1. Keep the audio-device guard and `err[0] = '\0'`.
2. Call `track_open(path, &info, errbuf, ...)` **before** `stop_session`.
3. On failure: `set_error(&g, errbuf[0] ? errbuf : "cannot load file")`, copy the
   same message to `err`, return -1. Do **not** touch `g.track`, `has_track`,
   `name`, `duration`, or the audio state.
4. On success: `stop_session(&g)`, close the old `g.track`, install the new
   track/info, then under `meta_mutex` set `has_track = 1`, `duration`,
   `name = path`, and clear `error`. Call `playback_play()`.

This also means a slow VBR MP3 header scan runs while the old track keeps
playing; the callback and decoder are independent of the candidate `Track`.

Update the `playback_load` comment in `src/playback.h:22-25` to say failure
leaves any current playback untouched and reports through `PlaybackStatus.error`.
No other playback functions change.

### 2. Share the Nuklear include configuration

Add `src/nuklear_config.h`:

```c
#ifndef MP_NUKLEAR_CONFIG_H
#define MP_NUKLEAR_CONFIG_H
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_VARARGS
#include "nuklear.h"
#endif
```

- `src/main.c`: keep `#define NK_IMPLEMENTATION`, replace the six `NK_INCLUDE_*`
  defines + `#include "nuklear.h"` with `#include "nuklear_config.h"`; keep
  `NK_SDL_RENDERER_IMPLEMENTATION` + renderer include after it.
- `src/file_browser.c` includes `nuklear_config.h` (declarations only).

### 3. File browser module

`src/file_browser.h` (keep Nuklear out of the header):

```c
#include <stddef.h>
struct nk_context;
typedef struct FileBrowser FileBrowser;

FileBrowser *file_browser_create(void);
void file_browser_destroy(FileBrowser *fb);
void file_browser_open(FileBrowser *fb, const char *suggested_path);
void file_browser_close(FileBrowser *fb);
int  file_browser_is_open(const FileBrowser *fb);

/* 1 = file chosen (absolute path in out, fb closed), -1 = cancelled/closed,
 * 0 = still open. Call only while is_open. */
int  file_browser_draw(FileBrowser *fb, struct nk_context *ctx, float scale,
                       float screen_w, float screen_h,
                       char *out, size_t outlen);
```

Implementation (`src/file_browser.c`; include `SDL.h` for `SDL_GetTicks`,
`dirent.h`, `sys/stat.h`, `unistd.h`, `stdlib.h`, `stdio.h`, `string.h`,
`ctype.h`, `math.h`):

- Fixed storage: `FB_PATH_MAX 4096`, `FB_NAME_MAX 256`,
  `FB_MAX_ENTRIES 1024`; `struct FileBrowser` holds `open`, `dir[FB_PATH_MAX]`,
  `last_dir[FB_PATH_MAX]`, `has_last_dir`, `FbEntry entries[FB_MAX_ENTRIES]`
  (`{ char name[FB_NAME_MAX]; int is_dir; }`), `count`, `selected`,
  `error[256]`, `last_click`, `last_click_ms`. Allocate once with `calloc`;
  destroy with `free`. `selected = last_click = -1` initially.
- `fb_load_dir(fb, path)`: `realpath` into a 4096 buffer; on failure set
  `error = "cannot open directory"` and return -1, keeping the old listing.
  On success copy to `dir` and `last_dir`, `has_last_dir = 1`, clear `error`,
  reset `selected/last_click`.
- Listing: `opendir`; entry 0 is always `".."` (is_dir = 1). For each other
  name: skip names starting with `.`; `stat(dir + "/" + name)`; include
  directories, and regular files whose extension is case-insensitively
  `.wav`/`.mp3`; skip stat failures. Stop at `FB_MAX_ENTRIES`.
  Sort entries **after index 0** dirs first, then case-insensitive name (tie
  with `strcmp`), so `..` is pinned first.
- `file_browser_open`: choose the start as `last_dir` if set, else the dirname
  of `suggested_path` (strip after the last `/`; a no-slash or NULL suggestion
  uses `getcwd`), and load it. If that fails, retry `getcwd`, then `/`.
- `file_browser_close`: `open = 0`.
- Drawing, only when `open`:
  - `nk_begin(ctx, "Open file", centered_rect, NK_WINDOW_BORDER|NK_WINDOW_TITLE|
    NK_WINDOW_MOVABLE|NK_WINDOW_NO_SCROLLBAR)`; base size 560×460 scaled by
    `scale`, clamped to the window with a 40 px (scaled) margin, centered in
    `screen_w/screen_h`.
  - One status row: `fb->error[0] ? fb->error : fb->dir`, height `row_h`.
  - List group (`NK_WINDOW_BORDER`) height =
    `nk_window_get_content_region(ctx).h - row_h - footer_h - 3 * spacing`,
    clamped to at least one row; `nk_layout_row_dynamic(ctx, row_h, 1)` then
    `nk_select_label(ctx, text, NK_TEXT_LEFT, i == fb->selected)` per entry.
    Render dirs with a trailing `/`. A nonzero return is a click (the value
    toggles internally; never persist it).
  - Double-click: if the same index is clicked again within 400 ms of the last
    click (`SDL_GetTicks()`), set `activate = i`; otherwise select and remember
    the click. **Defer activation until after the row loop and `nk_group_end`**
    because entering a directory reloads the entry array.
  - Footer: `Open` (disabled while `selected < 0`) and `Cancel`. Open/activate:
    `..` or a directory → `fb_load_dir(join(dir, name))` and stay open; a file →
    write the joined absolute path to `out`, set `open = 0`, return 1. Cancel →
    `open = 0`, return -1.
  - Sizes/positions use a local `lroundf(v * scale)` helper (same convention as
    `main.c`'s `ui_s`). No title-bar X, so no hidden-window reopen problem.

### 4. Wire into `main.c`

Include `file_browser.h`; create the browser after `playback_init` (if creation
fails, log and leave Open inert) and destroy it during shutdown before
`playback_shutdown()`.

Event loop:

- `SDL_DROPFILE`: copy `evt.drop.file` into `char pending_drop[4096]` (first drop
  wins if several arrive), set `has_pending_drop`, then **`SDL_free(evt.drop.file)`**
  for every file event.
- Esc: if `file_browser_is_open(browser)`, `file_browser_close`; otherwise quit.
- ←/→ seek handling only when the browser is not open.
- After `nk_input_end`, process `pending_drop`: close the browser, then
  `playback_load(pending_drop, err, sizeof err)`. Success or failure both leave
  a usable app; failure keeps the old track due to task 1.

Status/title:

- Derive `basename` from `st.name` (`strrchr`).
- Keep existing status precedence (`error`, else basename, else "no file loaded").
- Build `"<basename> — mediaplayer"` (UTF-8 em dash) when `st.has_track`, else
  `"mediaplayer"`; keep the previous string in `static`/local state and call
  `SDL_SetWindowTitle` only when it changes.

UI:

- Open button: if the browser is open, draw it disabled; else on click
  `file_browser_open(browser, st.has_track ? st.name : NULL)`.
- Transport buttons disabled when `!st.has_track || browser_open`.
- Seek bar: pass `st.has_track && !browser_open` as `enabled` (it already ignores
  drags while disabled); ←/→ are suppressed while browsing, so `pending_seek`
  stays zero.
- After the main `nk_end`, if the browser is open, call `file_browser_draw(...)`
  with `ui_scale`, `px_w`, `px_h` and a 4096-byte `chosen` buffer; on return 1,
  `playback_load(chosen, err, ...)`. The main window was laid out earlier this
  frame, so status/title refresh next frame (one frame stale, invisible in
  practice).
- Update the stale file-header comment in `main.c`.

### 5. Build

- `Makefile`: add `src/file_browser.c` to `SRC`; add `src/file_browser.h` and
  `src/nuklear_config.h` to `HDR`.
- No new libraries: SDL + `-lm` already cover it. `dirent`/`stat`/`realpath` are
  libc on the supported platforms.

## Edge cases / failure modes

- **Bad/unsupported drop or pick:** `playback_load` keeps the old track exactly
  as it was (playing, paused, or stopped with position intact); `error` appears
  in the status label. The browser closes so the message is visible.
- **Dropped directory:** `track_open` fails (unsupported/cannot open) → same
  preservation path; no crash.
- **Multi-file drop:** SDL emits one `DROPFILE` per file; load the first, free all
  path strings.
- **Browser navigation error** (deleted/unreadable dir): keep the old listing,
  show the error in the status row; Cancel/Esc still work.
- **Reopening after a failed load:** start dir is remembered; no stale selection.
- **Directory reload during drawing:** activation is deferred until the row loop
  ends, avoiding a use-after-reload of entry indices.
- **Click-through:** main controls are explicitly disabled while browsing, so
  Nuklear z-order cannot leak clicks into the seek bar/transport.
- **Esc semantics:** closes the browser when open; only quits when closed. Quit
  during playback still goes through `playback_shutdown`.
- **Long paths:** browser paths use 4096-byte buffers. `PlaybackStatus.name` and
  the window title remain capped at 512 (existing limit), so very deep paths may
  display truncated but still load correctly.
- **Non-ASCII filenames:** load fine; the default Nuklear font may not have the
  glyphs (pre-existing limitation, out of scope).
- **Hidden files / non-audio files:** not listed (dotfiles skipped; only `.wav`,
  `.mp3`, and dirs shown). Drag-and-drop still relies on `track_open`'s sniffing.
- **Directories with >1024 entries:** first 1024 shown; acceptable for v1.

## Validation

1. `make clean && make` — zero warnings under `-std=c99 -Wall -Wextra`.
2. Regression: `./mediaplayer some.wav` still loads and plays; WAV/MP3 pause,
   stop, seek, arrow keys, resize, HiDPI, and quit-during-playback unchanged.
3. Drag-and-drop (manual Finder drag): drag a `.wav` then an `.mp3` onto the
   window → previous playback stops, new file starts at 0:00, title becomes
   `<basename> — mediaplayer`. The Open-browser tests exercise the same load path.
4. Failed drop: start a track, then drop a `.txt`, a folder, and a truncated
   `.mp3` → the original keeps playing/paused at the same position, the status
   label shows the error, and the app stays usable.
5. Open browser: click Open → directory listing shows `..` first, dirs before
   files, only `.wav`/`.mp3` files. Navigate via double-click and select+Open;
   load a file in a nested dir from both paths. Cancel and Esc close the browser;
   Esc with it closed still quits.
6. Browser modality: while open, clicking Play/Stop/the seek bar has no effect;
   ←/→ do not seek. After closing, controls work again.
7. Remembered location: browse into a subdir, cancel, reopen → same directory.
   Load a file there, reopen → same directory.
8. Empty dir and `/`: listing works, `..` navigates to the parent (or stays at
   `/`). Optional: `chmod 000` a temp dir → error row, app remains usable.
9. Title on failure: attempt a bad load while a track is loaded → title still
   shows the old track; a good load updates it.
10. Memory: open/close the browser and navigate repeatedly; RSS stays flat.
    Confirm every `SDL_DROPFILE` path is `SDL_free`d.

## Risks

- **Changing load-failure semantics** is the main behavioral change; it is
  required by the PRD, and `playback_load` has only one caller (`main.c`). The
  CLI startup path still ends with no track + an error, so it behaves as before.
- **Briefly holding two tracks:** the candidate is opened before the old track is
  closed. For WAV (whole file in memory) a large switch briefly doubles audio
  memory. Acceptable for a one-track player; do not add streaming complexity for
  this phase.
- **POSIX-only browser APIs** (`dirent`, `stat`, `realpath`) are fine for the
  PRD's macOS/Linux target; Windows remains out of scope.
- **Nuklear input/z-order** around an overlapping dialog is handled by disabling
  the main controls while open rather than relying on window hit-testing.
- **UI sizing** at unusual scales/small windows: clamp the dialog to the window
  and compute the list height from the content region each frame.

## Out of scope

- Native OS file dialogs (`NSOpenPanel`/GTK), playlists, and multi-file selection.
- Volume/keyboard polish (Phase 6), ID3 metadata, non-ASCII font coverage.
- A persistent on-disk "last directory", favourites, or a path text-entry field.
- Removing CLI arguments entirely (kept as a dev convenience).
