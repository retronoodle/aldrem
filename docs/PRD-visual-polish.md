# mediaplayer — PRD: Visual Polish ("make it look cool")

A styling pass over the existing player. No new audio features, no format work, no
layout reinvention: same single-window, same controls, same behaviour, but it
should look like a modern dark music app instead of an unstyled Nuklear demo.

The current UI is a fullscreen Nuklear window on a flat `#18181C` clear colour
(`src/main.c:533`), default Nuklear buttons, a flat two-colour seek bar
(`src/main.c:118`), and the default `nk_slider_float` for volume
(`src/main.c:512`). This document replaces that with one curated theme, glossy
custom controls, a real backdrop, and restrained motion.

Scope note: the main PRD lists "theming or skins" as a non-goal. This pass adds
exactly **one** designed theme; it does not add a theme system or user skins.

## Goals

- One cohesive dark theme — "midnight studio" — across the main window, transport
  dock, file browser, empty state, error state, and disabled states.
- A deliberate palette with a cyan→violet accent, used sparingly and consistently.
- Glossy, tactile buttons: gradient fills, inner highlight/shadow, visible
  hover/press/disabled states, and hand-drawn transport icons (no icon font).
- A seek bar and volume slider that look like real audio hardware: slim rounded
  track, gradient fill, soft glow, and a handle that reacts to hover and drag.
- A backdrop with depth: vertical gradient plus a soft ambient glow behind the
  transport dock, replacing the flat clear colour.
- Short, frame-rate-independent motion (hover, press, playing pulse) that never
  delays input.
- Theme centralised in one place so widget code contains no hardcoded colours.
- Stay pure C99, SDL2 + Nuklear only, no new dependencies, no new asset pipeline.

## Non-goals

- No user-selectable themes, skins, or light mode (a second theme is a separate
  project once the palette layer exists).
- No audio visualiser, waveform, spectrum, or real-time analysis — still a
  non-goal from `docs/PRD.md`.
- No album art, tag reading, playlist UI, or metadata beyond today's status line.
- No window chrome redesign (the OS title bar stays).
- No bundled custom font in this pass. The built-in Nuklear font stays; a nicer
  display face is a deferred item (see "Deferred").
- No functional changes to playback, seeking, volume, drag-and-drop, or the
  browser. Every existing behaviour and shortcut must keep working untouched.

## Design direction

Reference feel: a compact desktop player in the vein of Spotify/Apple Music dark
mode, adapted to immediate-mode constraints. The controls stay where they are;
the hierarchy comes from grouping, elevation, contrast, and one accent.

Principles:

1. **Contrast first.** Gloss must never reduce legibility. Text sits on the
   darkest part of a surface.
2. **Gloss is a highlight, not a texture.** A thin top highlight and a soft bottom
   shade imply a bevel; no noise maps, no per-pixel work.
3. **The accent earns attention.** Cyan→violet is reserved for the current
   progress, the primary action, the playing indicator, and focus rings. Not for
   static decoration.
4. **Motion is feedback, not decoration.** 60–160 ms for input response; the only
   ambient motion is a slow playing pulse.
5. **Disabled is obvious.** Desaturated, flattened, no glow, no hover response.

### Palette

One token set, defined in `src/theme.c`. RGB is the source of truth; hex is shown
for design review.

| Token             | RGB / hex            | Use                                              |
|-------------------|----------------------|--------------------------------------------------|
| `bg_top`          | 10,12,18 `#0A0C12`   | Backdrop gradient top                            |
| `bg_bottom`       | 17,21,39 `#111527`   | Backdrop gradient bottom                         |
| `ambient_cyan`    | 34,211,238 `#22D3EE` | Ambient glow, low alpha only                     |
| `ambient_violet`  | 155,107,255 `#9B6BFF`| Ambient glow, low alpha only                     |
| `dock`            | 21,27,43 `#151B2B`   | Transport dock surface                           |
| `surface`         | 26,33,51 `#1A2133`   | Neutral button/list base                         |
| `surface_hover`   | 35,44,66 `#232C42`   | Hovered neutral surface                          |
| `surface_active`  | 43,54,80 `#2B3650`   | Pressed neutral surface                          |
| `border`          | 42,52,80 `#2A3450`   | Default 1 px border                              |
| `border_strong`   | 60,74,107 `#3C4A6B`  | Hovered/focused border                           |
| `text_primary`    | 238,242,255 `#EEF2FF`| Filename, time, button labels                    |
| `text_secondary`  | 167,178,201 `#A7B2C9`| Metadata, secondary labels                       |
| `text_muted`      | 108,122,150 `#6C7A96`| Non-essential hints                              |
| `accent_a`        | 34,211,238 `#22D3EE` | Gradient start, progress fill                    |
| `accent_b`        | 155,107,255 `#9B6BFF`| Gradient end, focus ring                         |
| `accent_ink`      | 7,16,26 `#07101A`    | Text on accent-filled buttons                    |
| `track`           | 36,44,64 `#242C40`   | Seek/volume empty track                          |
| `success`         | 52,211,153 `#34D399` | Playing indicator (optional, non-text)           |
| `danger`          | 251,113,133 `#FB7185`| Error text and border                            |
| `danger_surface`  | 42,22,32 `#2A1620`   | Error panel background                           |
| `shadow`          | 3,5,10 at ~40% alpha | Drop shadows under dock, buttons, handle         |

The primary accent gradient is `accent_a → accent_b`, left-to-right (135° feel
where a rectangle allows it). Glows are the accent colours at 10–35% alpha.

## Component specification

### Backdrop and window chrome

- Replace the flat `SDL_SetRenderDrawColor` clear (`src/main.c:533`) with a
  vertical gradient drawn as N horizontal bands (or a cached 1×N gradient
  texture). Bands are cheap and avoid per-pixel work.
- Add one soft ambient glow behind the transport dock: a small radial gradient
  texture generated once at startup (128×128 RGBA, cyan on one side, violet on
  the other), rendered with additive blending at ~15% alpha and scaled to the
  dock. Cache the texture in the theme object and destroy it on shutdown.
- The root Nuklear window (`src/main.c:458`) must not paint over the backdrop:
  set `style.window.fixed_background = nk_style_item_hide()`
  (`vendor/nuklear.h:5654`) and keep the canvas clear for the SDL backdrop.
- Introduce a centred content column: max width ~720 logical points, 24-point
  side margins, vertically centred transport dock. The window remains resizable
  and the layout must handle narrow windows (dock shrinks, never clips) and
  HiDPI scales.
- The dock is a rounded rect (`radius ≈ 14`) in `dock`, 1 px `border`, a soft
  drop shadow offset +2 px, and a faint top inner highlight. It holds the seek
  bar, transport row, and volume row; the Open/status row sits above it.

### Typography hierarchy

- Keep the default font. Create hierarchy with size, colour, and spacing only.
- Filename/window status: `text_primary`, full size.
- Time display: `text_primary`, full size, with the total time in
  `text_secondary`; the font is monospaced, so digits stay stable.
- Metadata chips (codec / bit depth / sample rate / channels): small rounded
  pills in `surface` with `text_secondary` text, 1 px `border`.
- Empty-state title: `text_primary`; supporting hint: `text_muted`.
- No text below `text_muted` contrast, and no essential information only in a
  low-contrast colour.

### Buttons

Three variants, one shared drawing helper:

| Variant   | Where                              | Fill                                   |
|-----------|------------------------------------|----------------------------------------|
| `primary` | Open (main + browser)              | Horizontal accent gradient             |
| `hero`    | Play/Pause (transport)             | Accent gradient while stopped/paused; neutral glass while playing |
| `neutral` | Stop, Cancel, list actions         | `surface` / `surface_hover`            |

Anatomy (drawn back to front):

```
  +--------------------------------------------------+
  |  top inner highlight: white @ 10%, 45% height     |  <- rounded, inset 1px
  |                                                   |
  |   label / glyph centered, text_primary/ink        |
  |                                                   |
  |  bottom inner shade: black @ 14%, lower 55%       |
  +--------------------------------------------------+
     1px border: border -> border_strong on hover
     soft outer glow on hover/focus (accent @ 20-35%, 2-3 layers)
     drop shadow offset +1/+2 px (under neutral buttons)
```

- Geometry: transport buttons 40 pt high with pill rounding (radius = height/2);
  Open/browser buttons 34 pt high, radius 9; browser rows 28 pt.
- Interaction states:
  - normal: base fill, 1 px `border`.
  - hover: base lightens one step, border → `border_strong`, glow appears,
    label → white. Hover easing 90 ms.
  - pressed: darkens one step, content shifts down 1 px, top highlight removed,
    glow collapses. 60 ms.
  - disabled: 55% alpha, no glow, no hover, flat fill, border at 60% alpha.
  - keyboard focus: 2 px `accent_b` ring at 70% alpha outside the button; never
    rely on hover alone to indicate focus.
- Transport glyphs are drawn with canvas primitives, not text: play triangle
  (`nk_fill_triangle`, `vendor/nuklear.h:4911`), two pause bars, stop square,
  using `text_primary` or `accent_ink`. Labels are optional; if shown, glyph +
  label is centred as a unit.
- Hero behaviour: while PLAYING the Play glyph becomes Pause and a slow accent
  glow pulses (see Motion); while PAUSED the glow is static; while STOPPED the
  hero fill returns to the accent gradient.
- All interactive targets are at least 32 logical points on their shortest side.

### Seek bar

- Replace the flat drawing in `ui_seek_bar` (`src/main.c:118`) with a themed
  helper in `theme.c`; keep the existing interaction contract exactly (press
  starts a drag, live preview, release seeks, disabled while the browser is open).
- Track: 6 pt high, full pill rounding, `track` fill, 1 px darker inner edge.
- Fill: horizontal accent gradient, with the rounded right cap. Optionally a
  faint 1 px top highlight along the fill for the "wet" look.
- Handle: 14 pt circle, `text_primary` fill, 2 pt `accent_a` ring, soft accent
  glow. Hover/drag scales it to 1.15 over 90 ms. Idle scale is 1.0.
- Hover: track lightens slightly; a small time bubble above the handle shows the
  target position (reuse the existing `m:ss` formatter).
- While dragging: fill follows the preview, handle remains enlarged, glow
  strengthens; no layout shift.
- Disabled/no track: flat `track`, no handle, no hover, no bubble.

### Volume control

- Replace `nk_slider_float` (`src/main.c:512`) with a compact custom slider that
  reuses the seek-bar drawing primitives: 4 pt track, accent fill, 12 pt handle,
  optional speaker glyph drawn from rectangles/polygons.
- Keep the current behaviour: 0–100, applied immediately, ↑/↓ steps of 5, updates
  the `vol N%` label.
- Visually subordinate to the seek bar: thinner track, no time bubble.

### Status, time, and playing state

- Split the single status string (`src/main.c:473`) into:
  1. filename (`text_primary`, elided with a trailing `…` if too long),
  2. metadata chips (`text_secondary`) built from the same `PlaybackStatus`
     fields already formatted at `src/main.c:424-445`.
- Time row (`src/main.c:476`) gains hierarchy: current time `text_primary`,
  separator and total `text_secondary`. Use two labels in a row template or
  direct canvas text; no functional change to the formatting logic.
- Playing state: a small accent dot/pulse next to the filename, or a subtle
  animated accent underline on the dock. It must be visible while playing,
  static while paused, absent while stopped.

### Empty and error states

- Empty state replaces the plain label at `src/main.c:482`:
  - a large abstract disc drawn from circles (outer ring, inner hub) in
    `surface`/`border` with an accent rim,
  - "Drop a track to begin" in `text_primary`,
  - a chip row of supported extensions in `text_muted`,
  - a 2 pt vertical float animation over ~3.2 s (disabled by reduce-motion).
- Error state: rounded `danger_surface` panel, `danger` 1 px border, error text in
  `danger`/`text_primary`; no shake, no flashing. It must leave the previous
  playback state untouched, as today.

### File browser

- Restyle the existing browser (`src/file_browser.c:296`), do not rebuild it:
  - window fill `dock`, 1 px `border`, rounding 12, title header styled with
    `text_primary` and a subtle bottom divider;
  - path/loading row in `text_secondary`;
  - list rows: hover fill `surface_hover`; selected row gets an accent 3 px left
    bar plus `surface_active` fill; directories use `accent_a` for the trailing
    slash; filenames `text_primary`, secondary text `text_secondary`;
  - scrollbar restyled via `nk_style_scrollbar` colours to match the palette;
  - footer Open button uses the `primary` variant, Cancel uses `neutral`;
  - error row uses the danger styling above.
- Long entries must not paint outside the list group; truncate with `…` where the
  existing group clip does not already handle it.

## Implementation approach

### New module

Add `src/theme.h` / `src/theme.c` and wire them into `SRC`/`HDR` in the
`Makefile`.

```c
struct MpTheme {
    /* palette tokens, metrics, cached SDL_Texture *ambient_glow */
};

void mp_theme_apply(struct nk_context *ctx, float scale);
void mp_theme_set_window_style(struct nk_context *ctx, float scale);
void mp_theme_backdrop(SDL_Renderer *ren, int w, int h, struct MpTheme *th);
void mp_theme_shutdown(struct MpTheme *th);

int  mp_button(struct nk_context *ctx, const char *label,
               enum mp_btn_kind kind, int enabled, struct MpAnim *anim);
int  mp_icon_button(struct nk_context *ctx, enum mp_icon icon,
                    enum mp_btn_kind kind, int enabled, struct MpAnim *anim);
int  mp_seek_bar(struct nk_context *ctx, float scale, float frac,
                 int enabled, int *dragging, float *preview);
int  mp_volume_slider(struct nk_context *ctx, float scale, float *value,
                      int enabled);
```

- `mp_theme_apply` owns every colour assignment and extends the metrics currently
  in `ui_scale_style` (`src/main.c:61`): window, text, button, slider, scrollbar,
  group, edit. It must be safe to call repeatedly after `nk_style_default` on
  HiDPI/scale changes, matching the current re-apply path (`src/main.c:279`).
- Keep colour constants in `theme.c`; widget code calls semantic helpers. A grep
  for `nk_rgb(`/`nk_rgba(` under `src/` should only find `theme.c`.
- Move the seek-bar implementation out of `main.c` into `theme.c` and call it
  from the existing seek row (`src/main.c:480`). The drag/release contract and
  the caller-side `playback_seek` stay in `main.c`.

### Drawing gloss without new dependencies

- Nuklear has no gradient style item (style items are colour, image, or
  nine-slice; see `struct nk_style_button`, `vendor/nuklear.h:5175`), and no
  per-rect clipping for canvas primitives.
- Build gloss by layering **inset** rounded rects over a rounded base:
  1. shadow rect, offset down;
  2. base rounded fill (solid or accent midpoint);
  3. gradient band using `nk_fill_rect_multi_color`
     (`vendor/nuklear.h:4908`) inset horizontally so it stays inside the rounded
     silhouette;
  4. top highlight rounded rect (white @ 10%, upper 45%);
  5. bottom shade rounded rect (black @ 14%, lower 55%);
  6. 1 px border (`nk_stroke_rect`) and optional outer glow rings.
- Because there is no per-widget clip, every overlay must be inset by at least the
  border + rounding so it cannot bleed outside the button. This is a hard rule.
- Glows are 2–3 concentric rounded rects with decreasing alpha, or a thicker
  translucent stroke. No blur passes.
- Use `nk_fill_circle` (`vendor/nuklear.h:4909`) for handles/discs and
  `nk_fill_triangle` / `nk_fill_polygon` for glyphs.

### Custom widget interaction

- `mp_button` follows the same pattern as the existing custom seek bar
  (`src/main.c:118-185`): claim the layout with `nk_widget(&bounds, ctx)`, test
  `nk_input_is_mouse_hovering_rect`, track press/release, and treat activation as
  release-inside to match Nuklear button semantics.
- Press state is stored in a small `struct MpAnim` owned by the caller (one per
  button) so releases that began outside the button never activate it. Do not use
  static locals; the UI may grow more buttons later.
- While a widget is disabled (`nk_widget_disable_begin` is active or `enabled` is
  false), skip hover/press handling entirely and draw the disabled style.

### Motion

- Drive animations from `SDL_GetPerformanceCounter`, one `dt` per frame, clamped
  to 100 ms so a dragged/focused window does not jump.
- Use exponential smoothing: `a += (target - a) * (1 - expf(-dt / tau))`, with
  `tau` ≈ 30 ms for press, 60 ms for hover, 300 ms for glow.
- Ambient playing pulse is a sine over ~2.4 s, alpha 0.12→0.30; empty-state float
  is ±2 pt over ~3.2 s. Both are decorative and never block input.
- Honour `MEDIAPLAYER_REDUCE_MOTION=1`: snap hover/press to their end states and
  disable pulse/float. Read the env once at startup.
- No per-frame heap allocation, no wall-clock sleeps, no more than one animated
  sine per visible state.

## Functional requirements

1. A single theme source sets all colours and metrics; no hardcoded colours
   remain in `main.c` or `file_browser.c`.
2. The root backdrop renders a vertical gradient plus ambient glow with no seams
   at any window size or HiDPI scale, and no flat-colour flash on startup.
3. All buttons implement normal, hover, pressed, disabled, and keyboard-focus
   states with the specified colours and geometry.
4. Transport controls render glyphs (play/pause/stop) with at least 32 pt hit
   targets and correct state icons while playing/paused/stopped.
5. The seek bar keeps its exact current behaviour (click/drag preview, release
   seeks, disabled while the browser is open) while gaining the themed track,
   gradient fill, glow handle, hover scale, and hover time bubble.
6. The volume slider keeps 0–100 behaviour, immediate application, keyboard
   steps, and numeric label, with the themed appearance.
7. Empty, error, disabled, and no-track states are visually distinct and
   preserve the existing playback/error semantics.
8. The file browser uses the same theme, with clear hover/selected rows and
   matching footer buttons.
9. Motion is frame-rate independent, allocation-free per frame, under the 3 Hz
   flash threshold, and fully disabled by `MEDIAPLAYER_REDUCE_MOTION=1`.
10. Text contrast: primary text ≥ 7:1 and secondary text ≥ 4.5:1 against its
    surface; accent-filled button text ≥ 4.5:1 against the fill.
11. Rendering adds no per-pixel full-screen work per frame: the gradient is a
    handful of rects and the ambient glow is a pre-generated texture.
12. `make` builds with zero warnings under `-Wall -Wextra`, the binary remains a
    single executable, and no new dependency or asset file is required.
13. Every existing functional requirement in `docs/PRD.md` (items 1–9) still
    passes; no behavioural or keyboard regression.

## Success criteria

- Side by side with the old build, the new UI reads as one designed product:
  coherent palette, confident buttons, depth without clutter.
- All controls remain legible at 900×520 and when maximised on a 4K display.
- Resizing while playing produces no visual glitches, no handle jumps, and no
  backdrop seams.
- Hover, press, seek, pause, and stop all respond within one frame; no animation
  delays an action.
- `make clean && make` is warning-clean; the only new files are `theme.c/h`.
- Both the empty state and a loaded track look intentional; the browser looks
  like part of the same app.

## Risk notes

- **Custom widgets and input semantics.** Re-implementing button behaviour can
  introduce double-activations or missed releases. Reuse the release-inside
  pattern already proven in `ui_seek_bar`, and keep one pressed flag per button.
- **No per-rect clipping.** Overlays that are not inset will bleed past rounded
  corners. Enforce the inset rule in `mp_button` so callers cannot get it wrong.
- **HiDPI scale churn.** The style is re-applied when `ui_scale` changes
  (`src/main.c:279`); theme metrics must be derived from the passed scale every
  time, never accumulated, or controls will grow on each monitor move.
- **Glow cost.** Keep it to a few rects and one cached texture; do not add a blur
  or per-pixel loop. If the ambient texture fails to create, fall back to the
  gradient alone.
- **Scope creep.** This is a styling pass. If a layout idea requires touching
  playback, seek maths, or the browser's scanning logic, defer it.

## Deferred

- Bundled display font (e.g. an embedded geometric sans) via
  `nk_font_atlas_add_from_memory`; improves type personality but adds a binary
  asset and licensing decision.
- A second theme (light, or a warm "ember" variant) behind the token layer.
- Real artwork panel with a blurred cover backdrop, once tag reading exists.

# Phased task list

Each phase ends with a runnable program and a visible change. Build them in
order. No functional/audio work belongs in any phase.

## Phase 1 — Theme foundation and palette

**Goal:** Replace scattered colours and the flat clear colour with one central
theme, with no custom widgets yet.

**Tasks**
- Add `src/theme.h` / `src/theme.c`; add to `SRC`/`HDR` in `Makefile`.
- Define the palette tokens, semantic helpers (`lerp`, `alpha`), and the
  `MpTheme` struct.
- Implement `mp_theme_apply(ctx, scale)` and move all colour assignments plus the
  existing metrics from `ui_scale_style` (`src/main.c:61`) into it; keep the
  scale re-apply path working.
- Set button, slider, scrollbar, group, edit, and text colours to the palette.
- Replace the root window background with `nk_style_item_hide()` and draw a
  temporary solid `bg_bottom` clear so the app stays runnable.
- Replace the hardcoded colours in `ui_seek_bar` (`src/main.c:118`) with theme
  tokens as a first step; full restyle comes later.

**Done when:** `make` is warning-clean, the app runs, and all visible colours come
from `theme.c`; `grep -R "nk_rgb" src` only matches `theme.c`.

## Phase 2 — Backdrop, dock, and content column

**Goal:** Give the UI depth: gradient backdrop, ambient glow, and a centred dock
that groups the transport controls.

**Tasks**
- Implement `mp_theme_backdrop`: cached vertical gradient (banded rects or a 1×N
  texture) plus the pre-generated ambient glow texture with additive blending;
  destroy the texture in `mp_theme_shutdown`.
- Add the centred content column (max ~720 pt, 24 pt margins) inside the root
  window and handle narrow/resized windows without clipping.
- Draw the transport dock rounded panel (shadow, `dock` fill, `border`, top inner
  highlight) around the seek, transport, and volume rows.
- Keep the Open/status row above the dock; move rows into the column layout.
- Verify the layout at 100% and HiDPI scales and on resize.

**Done when:** The window shows a gradient backdrop and glow with no seams, all
controls sit in a centred dock, and playback shortcuts/seek still behave as
before.

## Phase 3 — Glossy buttons and transport glyphs

**Goal:** Replace default Nuklear buttons with the three themed variants and
canvas-drawn transport icons.

**Tasks**
- Implement `mp_button` (shared interaction + layered gloss recipe) with
  `primary`, `hero`, and `neutral` variants and all five states.
- Implement `mp_icon_button` and draw play/pause/stop glyphs with
  `nk_fill_triangle`/`nk_fill_rect`; keep accessible labels for any text buttons.
- Convert Open (`src/main.c:469`), Play/Pause/Stop (`src/main.c:493-498`), and the
  browser footer buttons (`src/file_browser.c:391-396`) to the new helpers.
- Implement per-button `MpAnim` hover/press smoothing and the keyboard-focus ring.
- Make the hero button reflect PLAYING/PAUSED/STOPPED state, including the icon
  swap and static/pulsing glow (pulse may be wired in Phase 6).
- Ensure disabled styling during browser-modal/no-track states.

**Done when:** All buttons use the new variants, glyphs are crisp at every scale,
hover/press/disabled/focus are clearly distinct, and clicking behaves exactly as
the previous buttons did.

## Phase 4 — Seek bar and volume slider

**Goal:** Make progress and volume look like premium controls while preserving
their behaviour.

**Tasks**
- Move `ui_seek_bar` logic into `mp_seek_bar` in `theme.c`; keep the drag/release
  contract and the caller in `main.c`.
- Implement the themed track, accent gradient fill, glow handle with hover/drag
  scaling, hover track brightening, and the target-time bubble.
- Implement `mp_volume_slider` with the slimmer track, accent fill, small handle,
  and optional speaker glyph; replace `nk_slider_float` (`src/main.c:512`).
- Preserve numeric volume label, immediate apply, and ↑/↓ steps.
- Handle no-track and browser-modal disabled states in both widgets.

**Done when:** Seek and volume look consistent with the buttons, dragging still
previews and seeks correctly, and volume still applies immediately.

## Phase 5 — Content hierarchy and states

**Goal:** Polish the information layer: time, status, chips, empty and error
states, and the playing indicator.

**Tasks**
- Split the status line (`src/main.c:424-445`, rendered at `src/main.c:473`) into
  the filename and metadata chips; elide overlong filenames.
- Give the time row (`src/main.c:476`) current/total hierarchy with two colours.
- Add the playing indicator (accent dot/pulse or dock underline) driven by
  `PlaybackStatus`.
- Replace the empty label (`src/main.c:482`) with the drawn disc, headline,
  extension chips, and (Phase 6) float animation.
- Replace the raw error string with the danger panel styling; keep the error
  semantics and state preservation unchanged.

**Done when:** Loaded, empty, playing, paused, and error states are visually
distinct and legible, with no change to status text content or playback state.

## Phase 6 — File browser restyle

**Goal:** Make the Open dialog look like part of the same product.

**Tasks**
- Apply the theme to the browser window (`src/file_browser.c:296-398`): fill,
  border, rounding, title header divider, path/loading text colours.
- Restyle list rows (`src/file_browser.c:354-381`): hover fill, selected accent
  bar and fill, directory accent, and long-name truncation.
- Restyle the scrollbar via the theme's `nk_style_scrollbar` colours.
- Convert footer buttons to `primary`/`neutral` variants and style the error row
  with the danger panel.

**Done when:** The browser matches the main window, hover/selection are obvious,
and opening/cancelling files works exactly as before.

## Phase 7 — Motion and final polish

**Goal:** Add the small amount of life that makes the UI feel finished, then audit.

**Tasks**
- Add the frame `dt` helper and per-widget smoothing; wire hover/press easing into
  `MpAnim`.
- Implement the playing pulse and empty-state float; gate both behind
  `MEDIAPLAYER_REDUCE_MOTION`.
- Add the outer glow rings on hover/focus and the accent progress cap highlight.
- Audit contrast ratios against the palette table; adjust tokens if any pair
  fails the stated ratios.
- Audit for per-frame allocations, per-pixel work, repeated texture creation, and
  leaks in `mp_theme_shutdown`.
- Full regression pass: build warnings, resizing, HiDPI, long filenames, browser
  modal, error files, and all keyboard shortcuts from `docs/PRD.md`.

**Done when:** Motion is subtle and responsive, reduce-motion disables it
completely, `make clean && make` is warning-clean, and every existing functional
requirement still passes.
