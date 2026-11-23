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
    struct nk_color shadow;

    /* Generated once in mp_theme_init at a fixed source size and scaled to the
     * dock at draw time, so a HiDPI scale change does not regenerate it. May be
     * NULL if texture creation failed: the backdrop still paints the gradient. */
    SDL_Texture *ambient_glow;

    /* Renderer-pixel rect of the transport dock, set by mp_dock_end each frame
     * and read by mp_theme_backdrop to place the glow. Zero before the first
     * frame draws the dock. */
    struct nk_rect dock_rect;
};

/* Creates the ambient glow (needs a live renderer); mp_theme_shutdown must run
 * before SDL_DestroyRenderer. */
void mp_theme_init(struct MpTheme *th, SDL_Renderer *ren);
void mp_theme_apply(struct nk_context *ctx, float scale, struct MpTheme *th);

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

/* Dock panel. mp_dock_begin lays out one dock-height row spanning `col`, draws
 * the shadow, fill, border and top highlight, then opens a grouped layout for
 * the caller's rows (which must total `content_h`). Returns 1 when the caller
 * should lay out and draw its rows, 0 when the group could not open; either
 * way mp_dock_end must follow. All style changes are pushed in begin and popped
 * in end, so the panel leaves the shared style untouched. */
nk_bool mp_dock_begin(struct nk_context *ctx, float scale, struct nk_rect col,
    float content_h, struct MpTheme *th);
void mp_dock_end(struct nk_context *ctx);

#endif
