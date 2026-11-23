# PRD: VU meters (gh issue #7)

## Goal

Show stereo output levels in the top-right corner of the window, in one of two styles the user chooses: an analog needle dial, or a digital LED bar. The choice persists across launches.

## Requirements

- **Placement:** pinned to the top-right corner of the window, as its own overlay window (not inside the centred column). The header row reserves matching space on its right so the status text stops short of it.
- **Styles:** `Needle` (twin dials, red warning zone above 85%) and `LED` (two stacked 20-segment bars, green/amber/red by scale position, with a peak-hold marker). A single chip in the meter's row toggles between them.
- **Signal:** per-channel RMS (needle, LED body) and peak (LED peak marker), measured in the audio callback after volume. Mono is duplicated to both channels. Silence when not playing.
- **Ballistics:** fast attack (~10 ms), slow release (~300 ms), peak hold ~1 s then fall. Under `MEDIAPLAYER_REDUCE_MOTION` the meter snaps instead of easing.
- **Persistence:** style saved to `$XDG_CONFIG_HOME/aldrem/settings.ini` (fallback `~/.config/aldrem/`). A missing or unknown file means LED.
- **Constraints:** C99, no new dependencies, no locks or allocations in the audio callback (levels travel as fixed-point `SDL_atomic_t`).

## Out of scope

- Position choice (top vs right). The meter has one fixed place.
- dB scale labels and calibration to a true VU standard.

## Tasks

1. Audio: per-channel RMS and peak in `audio_callback`; expose through `PlaybackStatus`; zero when not playing.
2. Maths: `src/vu.c` mapping and ballistics, with `tests/test_vu.c`.
3. Settings: `src/settings.c` with atomic save, with `tests/test_settings.c`.
4. Widgets: `mp_vu_meter` in `src/theme.c`, with an `amber` palette token.
5. UI: overlay window and toggle in `src/main.c`; header row reserves space.
6. Docs: README controls and module list, CONTRIBUTING architecture table.
