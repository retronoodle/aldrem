/* theme.h - one curated dark palette and the Nuklear style built from it.
 *
 * Widget code asks this module for colours and never writes one itself, so a
 * restyle stays in theme.c. The struct is a plain value: main() owns it on the
 * stack and passes it in; nothing here allocates except the ambient glow, which
 * mp_theme_shutdown releases.
 */
#ifndef MP_THEME_H
#define MP_THEME_H

#include <SDL.h>

#include "nuklear_config.h"

struct MpTheme {
    /* Renderer pixels per logical point. Copied so mp_theme_apply can scale. */
    float scale;

    /* Palette mirrors. Phase 1 only reads track/accent/text_primary from
     * widget code; the rest are written every mp_theme_apply for later phases
     * (gradient backdrop, dock, glow, chips). */
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
    struct nk_color shadow;

    /* Generated once in mp_theme_init; NULL until Phase 2 creates it. */
    SDL_Texture *ambient_glow;
};

void mp_theme_init(struct MpTheme *th, SDL_Renderer *ren);
void mp_theme_apply(struct nk_context *ctx, float scale, struct MpTheme *th);

/* Paints the window backdrop. Phase 1 stub: a solid bg_bottom clear. */
void mp_theme_backdrop(SDL_Renderer *ren, int w, int h, struct MpTheme *th);
void mp_theme_shutdown(struct MpTheme *th);

/* Semantic colours: the only palette reads widget code needs this phase. */
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

#endif
