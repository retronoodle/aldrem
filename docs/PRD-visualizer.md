# PRD: Visualizer (gh issue #3)

## Goal

Give the player a visualizer. It sits in the lower band, under the transport, in a tab that switches it with the playlist. It shows the track name, offers a few visual modes with sliders to tune them, and can go fullscreen while the artist and title stay on screen.

## Requirements

- **Placement:** a `Playlist | Visualizer` tab row sits above the lower band. Only one is shown at a time. The Visualizer tab replaces the playlist and never changes the playlist's own behaviour.
- **Modes:** `Spectrum` (log-spaced bars, theme accent gradient), `Wave` (oscilloscope trace), and `Fractal` (an audio-reactive Julia set, rendered at low resolution and stretched by the GPU). The fractal is kaleidoscoped into six mirrored wedges, rippled by the treble, and kicked by bass onsets, with its palette slowly drifting and trails holding a fading last frame. A Phoenix creature sits over it, always present and continuously reshaped by its memory parameter, which music pushes harder on swells such as vocals or guitar solos. The view drifts in and out toward the seahorse valley.
- **Track name:** the visual carries the current track name in a strip at its top-left. With nothing loaded it reads "Nothing playing". Radio shows the station and the ICY song title.
- **Controls:** three mode buttons, a `Fullscreen` button, and four sliders: `Gain` (25-400%), `Smooth` (0-100, how slowly bars fall), `Bars` (8-96), and `Hue` (0-100, rotates the gradient or the fractal palette).
- **Fullscreen:** `F11` or the button toggles it, using the desktop fullscreen mode. The visual fills the screen and keeps the track name on it. The controls float along the bottom while the pointer moves, and hide after about 3 s of stillness. `Esc` leaves fullscreen first, then behaves as before.
- **Persistence:** the mode, the four sliders and the active tab are saved to `settings.ini`. Changes are written after the control settles (about 0.5 s), and on quit. A missing or unreadable value falls back to its default.
- **Reduced motion:** under `MEDIAPLAYER_REDUCE_MOTION` the fractal's clock freezes, it ignores audio input (so the ripple, kicks and creature stay off), and it holds still. The bars still respond.
- **Idle cost:** nothing is sampled or drawn while the Visualizer tab is hidden and fullscreen is off.
- **Constraints:** C99, no new dependencies. The audio callback takes no locks and allocates nothing: the visualizer's samples travel through a ring of `SDL_atomic_t` int16 values.

## Out of scope

- Plugin or user-supplied visual modes.
- Per-mode colour pickers. Colour comes from the theme accent plus the Hue slider.
- Splitting the title into separate artist and title fields. The visual shows the same text as the status row.

## Tasks

1. Audio: a mono sample tap in `audio_callback` (`src/playback.c`), read through `playback_get_samples`.
2. Maths: `src/viz.c` (FFT, log bars, smoothing, hue, fractal constant), with `tests/test_viz.c`.
3. Settings: a generic int key/value store in `src/settings.c` that keeps unknown keys, with `tests/test_settings.c` covering it.
4. Drawing: `src/visualizer.c`, using Nuklear canvas and image commands and one streamed texture for the fractal.
5. UI: tab row, fullscreen view and controls, F11 and Esc handling in `src/main.c`.
6. Docs: this PRD, the non-goal lines in `docs/PRD.md` and `docs/PRD-visual-polish.md`, README controls and module list, CONTRIBUTING architecture table.
