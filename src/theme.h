/* theme.h - one curated dark palette and the Nuklear style built from it.
 *
 * Widget code asks this module for colours and never writes one itself, so a
 * restyle stays in theme.c. The struct is a plain value: main() owns it on the
 * stack and passes it in; nothing here allocates except the ambient glow, which
 * mp_theme_init creates and mp_theme_shutdown releases.
 */
#ifndef MP_THEME_H
#define MP_THEME_H

#include <SDL.h>

#include "nuklear_config.h"
#include "vu.h"

struct MpTheme {
    /* Renderer pixels per logical point. Copied so mp_theme_apply can scale. */
    float scale;

    /* Palette mirrors. Widget code reads track/accent/text_primary through the
     * semantic accessors below; the rest are written every mp_theme_apply for
     * later phases (chips, browser). */
    struct nk_color bg_top;
    struct nk_color bg_bottom;
    struct nk_color ambient_cyan;
    struct nk_color ambient_violet;
    struct nk_color dock;
    struct nk_color surface;
    struct nk_color surface_hover;
    struct nk_color surface_active;
    struct nk_color border;
    struct nk_color border_strong;
    struct nk_color text_primary;
    struct nk_color text_secondary;
    struct nk_color text_muted;
    struct nk_color accent_a;
    struct nk_color accent_b;
    struct nk_color accent_ink;
    struct nk_color track;
    struct nk_color success;
    struct nk_color danger;
    struct nk_color danger_surface;
    struct nk_color amber;
    struct nk_color shadow;

    /* Generated once in mp_theme_init at a fixed source size and scaled to the
     * dock at draw time, so a HiDPI scale change does not regenerate it. May be
     * NULL if texture creation failed: the backdrop still paints the gradient. */
    SDL_Texture *ambient_glow;

    /* Renderer-pixel rect of the transport dock, set by mp_dock_end each frame
     * and read by mp_theme_backdrop to place the glow. Zero before the first
     * frame draws the dock. */
    struct nk_rect dock_rect;

    /* MEDIAPLAYER_REDUCE_MOTION set at init: easing snaps to its end state and
     * the pulse and float hold still. Read once, never per frame. */
    int reduce_motion;

    /* Seconds of animation time, advanced once a frame by mp_theme_tick. The
     * pulse and float are phases of this clock. */
    float clock;
};

/* Creates the ambient glow (needs a live renderer); mp_theme_shutdown must run
 * before SDL_DestroyRenderer. */
void mp_theme_init(struct MpTheme *th, SDL_Renderer *ren);
void mp_theme_apply(struct nk_context *ctx, float scale, struct MpTheme *th);
/* Advances the animation clock by one frame's dt (seconds). */
void mp_theme_tick(struct MpTheme *th, float dt);

/* Paints the window backdrop: a vertical banded gradient plus the ambient glow
 * behind the dock rect. Called once per frame before nk_sdl_render. */
void mp_theme_backdrop(SDL_Renderer *ren, int w, int h, struct MpTheme *th);
void mp_theme_shutdown(struct MpTheme *th);

/* Semantic colours: the only palette reads widget code needs. */
struct nk_color mp_theme_track(const struct MpTheme *th);
struct nk_color mp_theme_accent(const struct MpTheme *th);
struct nk_color mp_theme_text_primary(const struct MpTheme *th);

/* Colour maths for deriving palette steps: lerp mixes a->b at t (0..1) and
 * alpha replaces the alpha channel (0..255). */
struct nk_color mp_col_lerp(struct nk_color a, struct nk_color b, float t);
struct nk_color mp_col_alpha(struct nk_color c, int a);

/* Scale a logical length or vector to renderer pixels, rounding to whole
 * pixels so borders and glyphs stay crisp. */
float mp_s(float v, float s);
struct nk_vec2 mp_sv(float x, float y, float s);

/* Buttons. Three variants share one drawing helper: primary (the accent
 * gradient, for Open), hero (Play/Pause: accent gradient only while stopped)
 * and neutral (surface glass, for everything else). */
enum mp_btn_kind { MP_BTN_PRIMARY, MP_BTN_HERO, MP_BTN_NEUTRAL };

/* Glyphs are canvas primitives, not text: the default font has no symbols. */
enum mp_icon { MP_ICON_PLAY, MP_ICON_PAUSE, MP_ICON_STOP,
               MP_ICON_PREV, MP_ICON_NEXT };

/* Caller-owned state for one button, held across frames (never a static). */
struct MpAnim {
    float hover;   /* 0..1 eased: 0 idle, 1 hovered */
    float press;   /* 0..1 eased: 0 raised, 1 held down */
    int   pressed; /* press began inside; activate on release inside */
};

/* Per-frame options for mp_button. */
struct MpButtonOpts {
    int   focus_slot; /* the slot focused this frame; 0 = none */
    float dt;         /* seconds since the last frame, for easing */
};

/* Height in points of the buttons this phase draws: transport 40 (pill
 * rounding), 34 for the header and browser footer. */
float mp_button_height(enum mp_btn_kind kind);

/* Lays out, draws and runs one button in the current row. Returns 1 when
 * activated: press inside then release inside, or Enter while focused. A NULL
 * label draws the icon alone; `enabled` 0 draws the disabled style and ignores
 * input. `tab` is only compared against opt->focus_slot to draw the ring:
 * main() owns Tab and Enter routing and uses the same slots. */
int mp_button(struct nk_context *ctx, float scale, const struct MpTheme *th,
              const char *label, enum mp_btn_kind kind, enum mp_icon icon,
              int tab, int enabled, struct MpAnim *anim,
              const struct MpButtonOpts *opt);

/* Favourite star, drawn from primitives (the default font has no symbols).
 * Filled in the accent colour when `filled`, outlined otherwise. Returns 1 when
 * a left click is released inside it this frame. Sized to the layout slot. */
int mp_star(struct nk_context *ctx, float scale, const struct MpTheme *th,
            int filled);

/* Seek bar and volume slider: the same glass-and-accent recipe as the buttons,
 * with the seek row's drag contract unchanged from the old main.c bar. Both
 * structs are caller-owned per-widget state, held across frames (never a
 * static). */
struct MpSeekAnim {
    float hover;  /* 0..1 eased: 0 idle, 1 hovered */
    float handle; /* 1..~1.15 eased: handle scale on hover/drag */
    float glow;   /* 0..1 eased: 0 idle, 0.5 hover, 1 dragging */
};

struct MpVolAnim {
    float hover;  /* 0..1 eased: 0 idle, 1 hovered */
    float handle; /* 1..~1.15 eased: handle scale on hover/drag */
};

/* Lays out, draws and runs the seek bar in the current row. `frac` is 0..1.
 * Press inside starts a drag that writes `*preview` (0..1) each frame until
 * the button is released; the release returns 1 so the caller seeks to
 * `*preview * duration`, and a release anywhere counts, matching the previous
 * bar. `duration` (seconds) only feeds the hover bubble's m:ss text; pass 0
 * to suppress it. `enabled` 0 draws the flat disabled track and cancels a
 * drag in progress. `dt` drives the easing. */
int mp_seek_bar(struct nk_context *ctx, float scale, const struct MpTheme *th,
                float frac, int enabled, int *dragging, float *preview,
                double duration, float dt, struct MpSeekAnim *anim);

/* The compact volume slider: 0..1 in `*value`, clamped. A press or drag
 * anywhere on the row sets it; returns 1 on a frame where it wrote a new
 * value (the caller then applies it to the engine), 0 otherwise. `enabled` 0
 * draws the muted track and fill with no handle and ignores input, so the
 * numeric label beside it still reads true. */
int mp_volume_slider(struct nk_context *ctx, float scale,
                     const struct MpTheme *th, float *value, int enabled,
                     float dt, struct MpVolAnim *anim);

/* Stereo level meter, in two styles: twin needle dials, or LED bars. Output
 * levels are linear and post-volume (see playback.h). Ballistics live in the
 * caller-owned MpVuAnim, held across frames like the other widget states. */
struct MpVuLevels {
    float peak_l, peak_r;
    float rms_l, rms_r;
};

struct MpVuAnim {
    VuBallistics l, r;
};

/* Lays out and draws the meter in the current row, then advances its
 * ballistics by dt. `needle` 1 draws the dials side by side; 0 stacks the LED
 * bars, left channel above right. */
void mp_vu_meter(struct nk_context *ctx, float scale, const struct MpTheme *th,
    int needle, const struct MpVuLevels *lv, float dt, struct MpVuAnim *anim);

/* Dock panel. mp_dock_begin lays out one dock-height row spanning `col`, draws
 * the shadow, fill, border and top highlight, then opens a grouped layout for
 * the caller's rows (which must total `content_h`). Returns 1 when the caller
 * should lay out and draw its rows, 0 when the group could not open; either
 * way mp_dock_end must follow. All style changes are pushed in begin and popped
 * in end, so the panel leaves the shared style untouched. */
nk_bool mp_dock_begin(struct nk_context *ctx, float scale, struct nk_rect col,
    float content_h, struct MpTheme *th);
void mp_dock_end(struct nk_context *ctx);

/* Content hierarchy and states. The caller maps the engine state to this
 * enum, so theme code never depends on the playback module. */
enum mp_play_state { MP_PS_STOPPED, MP_PS_PLAYING, MP_PS_PAUSED };

/* Header cell: a state dot and metadata chips on the right. `name` is only the
 * loaded flag: NULL draws "no file loaded". Each lays out one row cell. */
void mp_status_row(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *name, const char *const *chips, int nchips,
    enum mp_play_state ps);

/* Error text in a danger panel; the full text shows as a tooltip if elided. */
void mp_error_panel(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *text);

/* Name (elided) on top, "cur / total" below. The name is optional (NULL or
 * empty centres the time). */
void mp_time_label(struct nk_context *ctx, const struct MpTheme *th,
    const char *name, const char *cur, const char *total);

/* A label in text_muted, for hints. `align` is an NK_TEXT_* flag. */
void mp_label_muted(struct nk_context *ctx, const struct MpTheme *th,
    const char *text, nk_flags align);

/* One file-browser row, returning 1 on click (nk_select_label's contract, so
 * the caller keeps owning the selection). `selected` draws the accent bar and
 * fill; `is_dir` draws the name in the accent colour. */
int mp_list_row(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *text, int is_dir, int selected);

/* Empty-playlist illustration: a disc, a headline and a chip row. The caller
 * sets a one-row layout at the band height first. */
void mp_empty_state(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *headline, const char *const *chips, int nchips);

#endif
