/* theme.c - the palette tokens and the Nuklear style built from them.
 *
 * "Midnight studio": one dark theme, cyan->violet accent, no second skin. The
 * tokens below are the source of truth; everything else here derives from them
 * or wraps them as semantic accessors so widget code never names a colour.
 */
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Palette. RGB is authoritative; hex in comments is for design review. */
#define MP_BG_TOP           nk_rgb( 10,  12,  18) /* #0A0C12 */
#define MP_BG_BOTTOM        nk_rgb( 17,  21,  39) /* #111527 */
#define MP_AMBIENT_CYAN     nk_rgb( 34, 211, 238) /* #22D3EE */
#define MP_AMBIENT_VIOLET   nk_rgb(155, 107, 255) /* #9B6BFF */
#define MP_DOCK             nk_rgb( 21,  27,  43) /* #151B2B */
#define MP_SURFACE          nk_rgb( 26,  33,  51) /* #1A2133 */
#define MP_SURFACE_HOVER    nk_rgb( 35,  44,  66) /* #232C42 */
#define MP_SURFACE_ACTIVE   nk_rgb( 43,  54,  80) /* #2B3650 */
#define MP_BORDER           nk_rgb( 42,  52,  80) /* #2A3450 */
#define MP_BORDER_STRONG    nk_rgb( 60,  74, 107) /* #3C4A6B */
#define MP_TEXT_PRIMARY     nk_rgb(238, 242, 255) /* #EEF2FF */
#define MP_TEXT_SECONDARY   nk_rgb(167, 178, 201) /* #A7B2C9 */
#define MP_TEXT_MUTED       nk_rgb(108, 122, 150) /* #6C7A96 */
#define MP_ACCENT_A         nk_rgb( 34, 211, 238) /* #22D3EE */
#define MP_ACCENT_B         nk_rgb(155, 107, 255) /* #9B6BFF */
#define MP_ACCENT_INK       nk_rgb(  7,  16,  26) /* #07101A */
#define MP_TRACK            nk_rgb( 36,  44,  64) /* #242C40 */
#define MP_SUCCESS          nk_rgb( 52, 211, 153) /* #34D399 */
#define MP_DANGER           nk_rgb(251, 113, 133) /* #FB7185 */
#define MP_DANGER_SURFACE   nk_rgb( 42,  22,  32) /* #2A1620 */
#define MP_SHADOW           nk_rgba(  3,   5,  10, 102) /* #03060A @ 40% */
#define MP_HIGHLIGHT        nk_rgb(255, 255, 255) /* dock top sheen, alpha applied */
#define MP_INK              nk_rgb(  0,   0,   0) /* button bottom shade */

/* Backdrop: this many flat horizontal bands approximate the vertical gradient;
 * more bands means fainter steps but a pixel loop that is still one fill each. */
#define MP_GRADIENT_BANDS   32

/* Ambient glow source texture size and peak alpha (of 255) after blending. */
#define MP_GLOW_SIZE        128
#define MP_GLOW_ALPHA       38

float
mp_s(float v, float s)
{
    return lroundf(v * s);
}

struct nk_vec2
mp_sv(float x, float y, float s)
{
    return nk_vec2(mp_s(x, s), mp_s(y, s));
}

struct nk_color
mp_col_lerp(struct nk_color a, struct nk_color b, float t)
{
    if (t <= 0.0f)
        return a;
    if (t >= 1.0f)
        return b;
    return nk_rgba((int)lroundf((float)a.r + ((float)b.r - (float)a.r) * t),
                   (int)lroundf((float)a.g + ((float)b.g - (float)a.g) * t),
                   (int)lroundf((float)a.b + ((float)b.b - (float)a.b) * t),
                   (int)lroundf((float)a.a + ((float)b.a - (float)a.a) * t));
}

struct nk_color
mp_col_alpha(struct nk_color c, int a)
{
    c.a = (nk_byte)(a < 0 ? 0 : (a > 255 ? 255 : a));
    return c;
}

struct nk_color
mp_theme_track(const struct MpTheme *th)
{
    return th->track;
}

struct nk_color
mp_theme_accent(const struct MpTheme *th)
{
    return th->accent_a;
}

struct nk_color
mp_theme_text_primary(const struct MpTheme *th)
{
    return th->text_primary;
}

/* A soft two-lobe halo, cyan on the left and violet on the right: alpha falls
 * off radially from each lobe's centre so the dock sits in a glow, not under a
 * flat wash. Written once into one ARGB buffer and uploaded in a single call.
 * Returns NULL when SDL cannot make the texture; the caller logs and renders
 * the gradient alone. */
static SDL_Texture *
make_ambient_glow(SDL_Renderer *ren, struct nk_color cyan,
    struct nk_color violet)
{
    SDL_Texture *tex;
    SDL_PixelFormat *fmt;
    Uint32 *pixels;
    int x, y;

    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STATIC, MP_GLOW_SIZE, MP_GLOW_SIZE);
    if (!tex)
        return NULL;

    pixels = (Uint32 *)malloc(sizeof *pixels * MP_GLOW_SIZE * MP_GLOW_SIZE);
    fmt = SDL_AllocFormat(SDL_PIXELFORMAT_ARGB8888);
    if (!pixels || !fmt) {
        free(pixels);
        if (fmt)
            SDL_FreeFormat(fmt);
        SDL_DestroyTexture(tex);
        return NULL;
    }

    for (y = 0; y < MP_GLOW_SIZE; y++) {
        float fy = ((float)y + 0.5f) / (float)MP_GLOW_SIZE - 0.5f;

        for (x = 0; x < MP_GLOW_SIZE; x++) {
            float fx = ((float)x + 0.5f) / (float)MP_GLOW_SIZE;
            /* Distance from each lobe centre, with the vertical axis squashed
             * so the halo is wider than tall. */
            float dl = (fx - 0.25f) * (fx - 0.25f) + fy * fy * 2.0f;
            float dr = (fx - 0.75f) * (fx - 0.75f) + fy * fy * 2.0f;
            float d = dl < dr ? dl : dr;
            float fall = 1.0f - sqrtf(d / 0.0625f);
            struct nk_color c;
            int a;

            if (fall < 0.0f)
                fall = 0.0f;
            a = (int)lroundf(fall * fall * (float)MP_GLOW_ALPHA);
            /* The colour ramp runs left to right; both lobes share it so the
             * cyan stays on the left and the violet on the right. */
            c = mp_col_lerp(cyan, violet, fx);
            pixels[y * MP_GLOW_SIZE + x] = SDL_MapRGBA(fmt, c.r, c.g, c.b,
                                                       (Uint8)a);
        }
    }

    if (SDL_UpdateTexture(tex, NULL, pixels,
                          MP_GLOW_SIZE * (int)sizeof *pixels) != 0 ||
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD) != 0) {
        SDL_Log("ambient glow setup failed: %s", SDL_GetError());
        free(pixels);
        SDL_FreeFormat(fmt);
        SDL_DestroyTexture(tex);
        return NULL;
    }

    free(pixels);
    SDL_FreeFormat(fmt);
    return tex;
}

void
mp_theme_init(struct MpTheme *th, SDL_Renderer *ren)
{
    memset(th, 0, sizeof *th);
    th->scale = 1.0f;
    th->bg_top = MP_BG_TOP;
    th->bg_bottom = MP_BG_BOTTOM;
    th->ambient_cyan = MP_AMBIENT_CYAN;
    th->ambient_violet = MP_AMBIENT_VIOLET;
    th->dock = MP_DOCK;
    th->surface = MP_SURFACE;
    th->surface_hover = MP_SURFACE_HOVER;
    th->surface_active = MP_SURFACE_ACTIVE;
    th->border = MP_BORDER;
    th->border_strong = MP_BORDER_STRONG;
    th->text_primary = MP_TEXT_PRIMARY;
    th->text_secondary = MP_TEXT_SECONDARY;
    th->text_muted = MP_TEXT_MUTED;
    th->accent_a = MP_ACCENT_A;
    th->accent_b = MP_ACCENT_B;
    th->accent_ink = MP_ACCENT_INK;
    th->track = MP_TRACK;
    th->success = MP_SUCCESS;
    th->danger = MP_DANGER;
    th->danger_surface = MP_DANGER_SURFACE;
    th->shadow = MP_SHADOW;
    th->ambient_glow = ren ? make_ambient_glow(ren, th->ambient_cyan,
                                               th->ambient_violet) : NULL;
    if (ren && !th->ambient_glow)
        SDL_Log("ambient glow unavailable: %s", SDL_GetError());
}

void
mp_theme_apply(struct nk_context *ctx, float scale, struct MpTheme *th)
{
    struct nk_style *st = &ctx->style;
    struct nk_color secondary_line;

    /* Callers run nk_style_default first, so every value set here must be set
     * on every call: nothing survives from the previous scale. */
    th->scale = scale;

    /* Derived step: keeps the palette one set of tokens. */
    secondary_line = mp_col_alpha(th->text_secondary, 96);

    /* Metrics: the same numbers the app has always used. */
    st->window.padding = mp_sv(4.0f, 4.0f, scale);
    st->window.spacing = mp_sv(4.0f, 4.0f, scale);
    st->window.scrollbar_size = mp_sv(10.0f, 10.0f, scale);
    st->window.border = mp_s(2.0f, scale);
    st->window.rounding = mp_s(0.0f, scale);
    st->window.min_row_height_padding = mp_s(8.0f, scale);

    st->text.padding = mp_sv(0.0f, 0.0f, scale);

    st->button.padding = mp_sv(2.0f, 2.0f, scale);
    st->button.image_padding = mp_sv(0.0f, 0.0f, scale);
    st->button.border = mp_s(1.0f, scale);
    st->button.rounding = mp_s(4.0f, scale);

    st->slider.cursor_size = mp_sv(16.0f, 16.0f, scale);
    st->slider.padding = mp_sv(2.0f, 2.0f, scale);
    st->slider.spacing = mp_sv(2.0f, 2.0f, scale);
    st->slider.bar_height = mp_s(4.0f, scale);
    st->slider.rounding = mp_s(0.0f, scale);
    st->slider.border = mp_s(0.0f, scale);

    st->edit.border = mp_s(1.0f, scale);
    st->edit.rounding = mp_s(4.0f, scale);
    st->scrollh.rounding_cursor = mp_s(2.0f, scale);
    st->scrollv.rounding_cursor = mp_s(2.0f, scale);

    /* Window: panels and dialogs fill with the surface step. The root window
     * hides this fill only while it draws, so the backdrop shows through. */
    st->window.fixed_background = nk_style_item_color(th->surface);
    st->window.background = th->surface;
    st->window.border_color = th->border;
    st->window.popup_border_color = th->border;
    st->window.combo_border_color = th->border;
    st->window.contextual_border_color = th->border;
    st->window.menu_border_color = th->border;
    st->window.group_border_color = th->border;
    st->window.tooltip_border_color = th->border;
    st->window.header.normal = nk_style_item_color(th->dock);
    st->window.header.hover = nk_style_item_color(th->surface_hover);
    st->window.header.active = nk_style_item_color(th->surface_active);
    st->window.header.label_normal = th->text_primary;
    st->window.header.label_hover = th->text_primary;
    st->window.header.label_active = th->text_primary;

    st->text.color = th->text_primary;
    st->link.text_normal = th->text_primary;
    st->link.text_hover = th->accent_a;
    st->link.text_active = th->accent_b;

    /* Buttons, including the contextual and menu variants the popup paths use. */
    st->button.normal = nk_style_item_color(th->surface);
    st->button.hover = nk_style_item_color(th->surface_hover);
    st->button.active = nk_style_item_color(th->surface_active);
    st->button.border_color = th->border;
    st->button.text_normal = th->text_primary;
    st->button.text_hover = th->text_primary;
    st->button.text_active = th->text_primary;
    st->contextual_button = st->button;
    st->menu_button = st->button;

    st->option.normal = nk_style_item_color(th->surface);
    st->option.hover = nk_style_item_color(th->surface_hover);
    st->option.active = nk_style_item_color(th->surface_active);
    st->option.border_color = th->border;
    st->option.cursor_normal = nk_style_item_color(th->border_strong);
    st->option.cursor_hover = nk_style_item_color(th->accent_a);
    st->option.text_normal = th->text_primary;
    st->option.text_hover = th->text_primary;
    st->option.text_active = th->text_primary;
    st->checkbox = st->option;

    /* Selectable rows: the playlist and browser list entries. */
    st->selectable.normal = nk_style_item_color(th->surface);
    st->selectable.hover = nk_style_item_color(th->surface_hover);
    st->selectable.pressed = nk_style_item_color(th->surface_active);
    /* Selected rows: surface_active with a cyan tint, so they stand out from
     * the plain surface rows and the playing-row accent bar. */
    st->selectable.normal_active = nk_style_item_color(
        mp_col_lerp(th->surface_active, th->accent_a, 0.14f));
    st->selectable.hover_active = nk_style_item_color(
        mp_col_lerp(th->surface_active, th->accent_a, 0.22f));
    st->selectable.pressed_active = nk_style_item_color(
        mp_col_lerp(th->surface_active, th->accent_a, 0.22f));
    st->selectable.text_normal = th->text_primary;
    st->selectable.text_hover = th->text_primary;
    st->selectable.text_pressed = th->text_primary;
    st->selectable.text_normal_active = th->text_primary;
    st->selectable.text_hover_active = th->text_primary;
    st->selectable.text_pressed_active = th->text_primary;

    st->slider.normal = nk_style_item_color(th->track);
    st->slider.hover = nk_style_item_color(th->track);
    st->slider.active = nk_style_item_color(th->track);
    st->slider.border_color = th->border;
    st->slider.bar_normal = th->track;
    st->slider.bar_hover = th->track;
    st->slider.bar_active = th->track;
    st->slider.bar_filled = th->accent_a;
    st->slider.cursor_normal = nk_style_item_color(th->accent_a);
    st->slider.cursor_hover = nk_style_item_color(th->accent_b);
    st->slider.cursor_active = nk_style_item_color(th->accent_b);

    /* Scrollbars: the list views. */
    st->scrollh.normal = nk_style_item_color(th->dock);
    st->scrollh.hover = nk_style_item_color(th->surface_hover);
    st->scrollh.active = nk_style_item_color(th->surface_active);
    st->scrollh.border_color = th->border;
    st->scrollh.cursor_normal = nk_style_item_color(th->border_strong);
    st->scrollh.cursor_hover = nk_style_item_color(th->accent_a);
    st->scrollh.cursor_active = nk_style_item_color(th->accent_b);
    st->scrollh.cursor_border_color = th->border;
    st->scrollv.normal = st->scrollh.normal;
    st->scrollv.hover = st->scrollh.hover;
    st->scrollv.active = st->scrollh.active;
    st->scrollv.border_color = st->scrollh.border_color;
    st->scrollv.cursor_normal = st->scrollh.cursor_normal;
    st->scrollv.cursor_hover = st->scrollh.cursor_hover;
    st->scrollv.cursor_active = st->scrollh.cursor_active;
    st->scrollv.cursor_border_color = st->scrollh.cursor_border_color;

    /* Edit field: the Save-as path. */
    st->edit.normal = nk_style_item_color(th->surface);
    st->edit.hover = nk_style_item_color(th->surface_hover);
    st->edit.active = nk_style_item_color(th->surface_active);
    st->edit.border_color = th->border;
    st->edit.cursor_normal = th->accent_a;
    st->edit.cursor_hover = th->accent_b;
    st->edit.cursor_text_normal = th->accent_ink;
    st->edit.cursor_text_hover = th->accent_ink;
    st->edit.text_normal = th->text_primary;
    st->edit.text_hover = th->text_primary;
    st->edit.text_active = th->text_primary;
    st->edit.selected_normal = th->surface_active;
    st->edit.selected_hover = th->surface_active;
    st->edit.selected_text_normal = th->text_primary;
    st->edit.selected_text_hover = th->text_primary;

    /* Groups and popups share the window edge; the scaler hint is muted. */
    st->window.scaler = nk_style_item_color(secondary_line);
}

void
mp_theme_backdrop(SDL_Renderer *ren, int w, int h, struct MpTheme *th)
{
    struct nk_color top = mp_col_alpha(th->bg_top, 255);
    int k;

    /* Clear first so a partial gradient can never flash stale pixels; nothing
     * below is transparent, so this is belt and braces. */
    SDL_SetRenderDrawColor(ren, top.r, top.g, top.b, 255);
    SDL_RenderClear(ren);

    /* Flat bands with rounded edges: band k ends where band k+1 starts, and
     * the last one lands exactly on h, so there is no seam. */
    for (k = 0; k < MP_GRADIENT_BANDS; k++) {
        int y0 = (int)(((long)k * h) / MP_GRADIENT_BANDS);
        int y1 = (int)(((long)(k + 1) * h) / MP_GRADIENT_BANDS);
        struct nk_color c = mp_col_lerp(th->bg_top, th->bg_bottom,
            ((float)k + 0.5f) / (float)MP_GRADIENT_BANDS);
        SDL_Rect band = {0, y0, w, y1 - y0};

        if (band.h <= 0)
            continue;
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
        SDL_RenderFillRect(ren, &band);
    }

    /* The glow sits behind the dock, added over the gradient. Restore the
     * blend mode so nothing later inherits additive blending. */
    if (th->ambient_glow && th->dock_rect.w > 0.0f && th->dock_rect.h > 0.0f) {
        struct nk_rect d = th->dock_rect;
        SDL_BlendMode prev;
        SDL_Rect dest;
        int ex = (int)lroundf(d.w * 0.15f);
        int ey = (int)lroundf(d.h * 0.4f);

        dest.x = (int)lroundf(d.x) - ex;
        dest.y = (int)lroundf(d.y) - ey;
        dest.w = (int)lroundf(d.w) + 2 * ex;
        dest.h = (int)lroundf(d.h) + 2 * ey;

        if (SDL_GetRenderDrawBlendMode(ren, &prev) != 0)
            prev = SDL_BLENDMODE_NONE;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
        SDL_RenderCopy(ren, th->ambient_glow, NULL, &dest);
        SDL_SetRenderDrawBlendMode(ren, prev);
    }
}

void
mp_theme_shutdown(struct MpTheme *th)
{
    if (th->ambient_glow) {
        SDL_DestroyTexture(th->ambient_glow);
        th->ambient_glow = NULL;
    }
}

/* Dock geometry in logical points; scaled by mp_dock_begin. */
#define MP_DOCK_PAD        18.0f
#define MP_DOCK_RADIUS     14.0f
#define MP_DOCK_SHADOW_Y    2.0f
#define MP_DOCK_SHADOW_A  102
#define MP_DOCK_HIGHLIGHT_A 26

/* Open docked, the group's style values are the shared window ones. The group
 * pushes the input map, so nothing it sets can leak into the playlist band
 * drawn after it in the same root window. */
nk_bool
mp_dock_begin(struct nk_context *ctx, float scale, struct nk_rect col,
    float content_h, struct MpTheme *th)
{
    struct nk_rect outer;
    struct nk_rect bounds;
    struct nk_rect body;
    struct nk_command_buffer *canvas;
    float pad, radius, border;
    float sheen_w, sheen_h;
    int pushed = 0;

    if (col.w <= 0.0f || content_h <= 0.0f)
        return 0;

    pad = mp_s(MP_DOCK_PAD, scale);
    radius = mp_s(MP_DOCK_RADIUS, scale);
    border = mp_s(1.0f, scale);
    if (border < 1.0f)
        border = 1.0f;

    /* The caller's rect spans the column from the top of the window; the dock
     * starts under whatever rows are already laid out (the header), which is
     * where the next row would have gone. */
    bounds = nk_layout_widget_bounds(ctx);
    col.y = bounds.y + bounds.h;

    outer = nk_rect(col.x, col.y, col.w, content_h + 2.0f * pad);
    th->dock_rect = outer;

    /* The group opens at this height, then its own rows sit inside. */
    nk_layout_row_dynamic(ctx, outer.h, 1);
    canvas = nk_window_get_canvas(ctx);
    nk_fill_rect(canvas, nk_rect(outer.x, outer.y + mp_s(MP_DOCK_SHADOW_Y, scale),
                                 outer.w, outer.h), radius,
                 mp_col_alpha(th->shadow, MP_DOCK_SHADOW_A));

    /* Only the group's edge and padding change; the values are popped in
     * mp_dock_end. Each push is counted so a failure unwinds the same number
     * of pops off the matching stacks rather than leaving a half-pushed one. */
    if (!nk_style_push_color(ctx, &ctx->style.window.group_border_color,
                             th->border))
        goto unwind;
    pushed = 1;
    if (!nk_style_push_float(ctx, &ctx->style.window.group_border, border))
        goto unwind;
    pushed = 2;
    if (!nk_style_push_vec2(ctx, &ctx->style.window.group_padding,
                            nk_vec2(pad * 0.5f, pad * 0.5f)))
        goto unwind;
    pushed = 3;

    if (!nk_group_begin(ctx, "dock", NK_WINDOW_BORDER))
        goto unwind;

    /* A group has no fill of its own; paint the dock surface, a hairline just
     * inside the border, then a top sheen over the upper part. Drawn here so
     * they sit behind the rows that follow. */
    body = nk_layout_widget_bounds(ctx);
    nk_fill_rect(canvas, body, radius > border ? radius - border : 0.0f,
                 th->dock);
    nk_stroke_rect(canvas, body, radius - border * 0.5f, border,
                   mp_col_alpha(th->border_strong, 96));
    /* The sheen sits just inside the border; a column too narrow for the two
     * borders would invert the width, so skip it rather than draw garbage. */
    sheen_w = body.w - 2.0f * border;
    sheen_h = body.h * 0.45f;
    if (sheen_w > 0.0f && sheen_h > 0.0f)
        nk_fill_rect_multi_color(canvas,
            nk_rect(body.x + border, body.y + border, sheen_w, sheen_h),
            mp_col_alpha(MP_HIGHLIGHT, MP_DOCK_HIGHLIGHT_A),
            mp_col_alpha(MP_HIGHLIGHT, 0),
            mp_col_alpha(MP_HIGHLIGHT, MP_DOCK_HIGHLIGHT_A),
            mp_col_alpha(MP_HIGHLIGHT, 0));
    return 1;

unwind:
    /* Each style property has its own stack, so unwind in reverse pushing the
     * matching pop: vec2, then float, then colour, to the depth reached. */
    if (pushed >= 3)
        nk_style_pop_vec2(ctx);
    if (pushed >= 2)
        nk_style_pop_float(ctx);
    if (pushed >= 1)
        nk_style_pop_color(ctx);
    return 0;
}

void
mp_dock_end(struct nk_context *ctx)
{
    nk_group_end(ctx);
    nk_style_pop_vec2(ctx);
    nk_style_pop_float(ctx);
    nk_style_pop_color(ctx);
}


/* Buttons. Sizes in logical points; the transport rows are pills, the text
 * buttons a softer corner. The minimum keeps a bare glyph button a legal 32 pt
 * hit target even on the smallest row. */
#define MP_BTN_H_TRANSPORT  40.0f
#define MP_BTN_H_TEXT       34.0f
#define MP_BTN_R_TEXT        9.0f
#define MP_BTN_MIN_W        32.0f
#define MP_BTN_LABEL_PAD    10.0f

/* Draw-time alphas (0..255). The sheen and shade are the dock's gloss recipe
 * applied inside the silhouette: both are inset by the rounding so they cannot
 * escape past the corners, which is the rule this whole helper is built on. */
#define MP_BTN_SHADOW_A     90
#define MP_BTN_SHEEN_A      26
#define MP_BTN_SHADE_A      36
#define MP_BTN_DISABLED_A  140
#define MP_BTN_FOCUS_A     179

/* How hard a hover or a hold lifts and darkens the face. */
#define MP_BTN_HOVER_LIFT   0.25f
#define MP_BTN_PRESS_DARK   0.12f

/* Glyph box as a fraction of the button height: about a third reads as a
 * transport glyph rather than a logo. */
#define MP_ICON_FRAC        0.36f
#define MP_ICON_BAR         3.0f

/* Easing time constants in seconds (the PRD's 60 ms hover, 30 ms press). The
 * exponential form makes a slow frame ease proportionally, not jump. */
#define MP_HOVER_TAU        0.060f
#define MP_PRESS_TAU        0.030f

float
mp_button_height(enum mp_btn_kind kind)
{
    return kind == MP_BTN_HERO ? MP_BTN_H_TRANSPORT : MP_BTN_H_TEXT;
}

/* Frame-rate independent approach to `target`; dt of 0 holds still. */
static float
mp_ease(float a, float target, float dt, float tau)
{
    if (dt <= 0.0f)
        return a;
    return a + (target - a) * (1.0f - expf(-dt / tau));
}

/* Scales a colour's RGB; keep its alpha. The pressed step of every variant. */
static struct nk_color
mp_shade_down(struct nk_color c, float f)
{
    return nk_rgba((int)lroundf((float)c.r * f), (int)lroundf((float)c.g * f),
                   (int)lroundf((float)c.b * f), (int)c.a);
}

/* The face fill. A hero at rest and the primary button run the accent
 * gradient left to right; a neutral button is glass that lifts one surface step
 * on hover and darkens on hold. */
static void
mp_btn_face(struct nk_context *ctx, struct nk_rect r, float radius, int accent,
    float hover, float press, const struct MpTheme *th)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float lift = hover * (1.0f - press);

    if (accent) {
        struct nk_color l = mp_col_lerp(th->accent_a, MP_HIGHLIGHT,
                                        lift * MP_BTN_HOVER_LIFT);
        struct nk_color rr = mp_col_lerp(th->accent_b, MP_HIGHLIGHT,
                                         lift * MP_BTN_HOVER_LIFT);

        if (press > 0.0f) {
            float k = 1.0f - MP_BTN_PRESS_DARK * press;

            l = mp_shade_down(l, k);
            rr = mp_shade_down(rr, k);
        }
        if (r.h > 2.0f * radius) {
            /* Rounded caps at the top and bottom, then a straight-sided
             * gradient body between them. The body spans only the straight
             * sides of the silhouette, so no square corner can poke out. */
            nk_fill_rect(canvas, nk_rect(r.x, r.y, r.w, 2.0f * radius), radius, l);
            nk_fill_rect(canvas, nk_rect(r.x, r.y + r.h - 2.0f * radius, r.w,
                                         2.0f * radius), radius, rr);
            nk_fill_rect_multi_color(canvas,
                nk_rect(r.x, r.y + radius, r.w, r.h - 2.0f * radius),
                l, l, rr, rr);
        } else {
            /* Pill too short for caps: one rounded rect in the mid colour. */
            nk_fill_rect(canvas, r, radius, mp_col_lerp(l, rr, 0.5f));
        }
    } else {
        /* `lift` is eased 0..1 already; mixing the whole step here and the
         * per-frame easing above would square the curve. */
        struct nk_color base = mp_col_lerp(th->surface, th->surface_hover,
                                           hover * (1.0f - press));

        if (press > 0.0f)
            base = mp_shade_down(base, 1.0f - MP_BTN_PRESS_DARK * press);
        nk_fill_rect(canvas, r, radius, base);
    }
}

/* Layered gloss over the face: a white sheen in the upper band, a black shade
 * in the lower. Both are inset corners so a rounded button cannot show them
 * hanging outside its silhouette. The sheen fades out as the button is held,
 * which with the 1 px shift is what "pressed" looks like. */
static void
mp_btn_gloss(struct nk_context *ctx, struct nk_rect r, float radius,
    float border, float press)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect inner;
    float band_h, vinset;

    /* Vertical inset is capped at 30% of the height so a pill (radius = h/2)
     * keeps a usable band; horizontal stays at the radius so it clears the
     * curved ends. Corner-radius buttons are below the cap, so unchanged. */
    vinset = (radius < 0.3f * r.h ? radius : 0.3f * r.h) + border;
    inner = nk_rect(r.x + radius + border, r.y + vinset,
                    r.w - 2.0f * (radius + border), r.h - 2.0f * vinset);
    if (inner.w <= 0.0f || inner.h <= 0.0f)
        return;

    band_h = inner.h * 0.45f;
    if (band_h > 0.0f && press < 1.0f) {
        struct nk_color hi = mp_col_alpha(MP_HIGHLIGHT,
            (int)lroundf((float)MP_BTN_SHEEN_A * (1.0f - press)));
        struct nk_color none = mp_col_alpha(MP_HIGHLIGHT, 0);

        nk_fill_rect_multi_color(canvas,
            nk_rect(inner.x, inner.y, inner.w, band_h), hi, hi, none, none);
    }

    band_h = inner.h * 0.5f;
    if (band_h > 0.0f) {
        struct nk_color lo = mp_col_alpha(MP_INK, MP_BTN_SHADE_A);
        struct nk_color none = mp_col_alpha(MP_INK, 0);

        nk_fill_rect_multi_color(canvas,
            nk_rect(inner.x, inner.y + inner.h - band_h, inner.w, band_h),
            none, none, lo, lo);
    }
}

/* A transport glyph centred in `box`, drawn from primitives. */
static void
mp_draw_icon(struct nk_context *ctx, struct nk_rect box, enum mp_icon icon,
    struct nk_color col, float scale)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float cx = box.x + box.w * 0.5f;
    float cy = box.y + box.h * 0.5f;
    float h = box.h, w = box.w;
    float bar = mp_s(MP_ICON_BAR, scale);
    float gap = mp_s(2.0f, scale);

    switch (icon) {
    case MP_ICON_PLAY:
        /* Nudged right: a triangle's mass sits left of its bounding box. */
        nk_fill_triangle(canvas,
            cx - w * 0.30f + mp_s(1.0f, scale), cy - h * 0.42f,
            cx - w * 0.30f + mp_s(1.0f, scale), cy + h * 0.42f,
            cx + w * 0.40f, cy, col);
        break;
    case MP_ICON_PAUSE:
        nk_fill_rect(canvas, nk_rect(cx - gap * 0.5f - bar, cy - h * 0.42f,
                                     bar, h * 0.84f), 0.0f, col);
        nk_fill_rect(canvas, nk_rect(cx + gap * 0.5f, cy - h * 0.42f,
                                     bar, h * 0.84f), 0.0f, col);
        break;
    case MP_ICON_STOP:
        nk_fill_rect(canvas, nk_rect(cx - w * 0.34f, cy - h * 0.34f,
                                     w * 0.68f, h * 0.68f),
                     mp_s(1.0f, scale), col);
        break;
    case MP_ICON_PREV:
    case MP_ICON_NEXT: {
        /* Triangle pointing in the travel direction, bar on the leading edge. */
        int prev = icon == MP_ICON_PREV;
        float dir = prev ? -1.0f : 1.0f;
        float tx = cx + dir * w * 0.10f;
        float bx = cx - dir * w * 0.30f;

        nk_fill_triangle(canvas,
            tx + dir * w * 0.20f, cy,
            tx - dir * w * 0.20f, cy - h * 0.40f,
            tx - dir * w * 0.20f, cy + h * 0.40f, col);
        if (prev)
            nk_fill_rect(canvas, nk_rect(bx - bar, cy - h * 0.40f, bar,
                                         h * 0.80f), 0.0f, col);
        else
            nk_fill_rect(canvas, nk_rect(bx + w * 0.20f, cy - h * 0.40f, bar,
                                         h * 0.80f), 0.0f, col);
        break;
    }
    }
}

int
mp_button(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *label, enum mp_btn_kind kind, enum mp_icon icon, int tab,
    int enabled, struct MpAnim *anim, const struct MpButtonOpts *opt)
{
    struct nk_command_buffer *canvas;
    struct nk_rect r;
    enum nk_widget_layout_states state;
    float radius, border, shift;
    int accent, hovered = 0, focused, clicked = 0, disabled;
    float dt = opt ? opt->dt : 0.0f;

    state = nk_widget(&r, ctx);
    if (state == NK_WIDGET_INVALID)
        return 0;

    /* `nk_widget` reports the window's disable flag as its own state; the
     * caller's flag covers "no track"/"modal up" without pushing a style. */
    disabled = !enabled || state == NK_WIDGET_DISABLED;
    /* The accent face belongs to the primary button and the hero; a hero while
     * playing or paused still gets the gradient, only its glyph changes. */
    accent = kind == MP_BTN_PRIMARY || kind == MP_BTN_HERO;

    /* Transport rows may already be a pill; the text rows get their corner. */
    radius = kind == MP_BTN_HERO ? r.h * 0.5f : mp_s(MP_BTN_R_TEXT, scale);
    if (r.w < mp_s(MP_BTN_MIN_W, scale))
        r.w = mp_s(MP_BTN_MIN_W, scale);
    border = mp_s(1.0f, scale);
    if (border < 1.0f)
        border = 1.0f;

    /* Interaction. ROM still draws; it just cannot be pressed. The `pressed`
     * flag is what makes a release outside a cancel and a press that began
     * elsewhere inert, matching NK_BUTTON_TRIGGER_ON_RELEASE. Rect tests beat
     * nk_widget_is_hovered here: that helper also refuses while another widget
     * owns `ctx->active`, which is exactly the state a held button is in, so it
     * would drop the press it just started. The browser modal cannot steal this
     * either way, because the caller passes enabled = 0 while it is open. */
    if (!disabled && state != NK_WIDGET_ROM) {
        hovered = nk_input_is_mouse_hovering_rect(&ctx->input, r) ? 1 : 0;
        if (hovered && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
            anim->pressed = 1;
        else if (hovered && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT) &&
                 ctx->input.mouse.buttons[NK_BUTTON_LEFT].clicked >= 2)
            /* Press and release both landed within one frame: Nuklear counts
             * each edge, so the held state was never seen. Still a click. */
            clicked = 1;
        if (anim->pressed) {
            if (nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT)) {
                anim->pressed = 0;
                if (hovered)
                    clicked = 1;
            } else if (!nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
                anim->pressed = 0;
            }
        }
    } else {
        anim->pressed = 0;
    }

    /* Enter (or Space, which main() consumes before it reaches this context,
     * so only Enter arrives) activates the focused button. Nuklear counts each
     * key edge in the frame, so one tap is either "down with one edge" (press
     * this frame) or "up with two edges" (press and release this frame). main()
     * drops Enter key-repeat events before they reach this context, so a held
     * key does not add edges. */
    focused = opt && tab != 0 && tab == opt->focus_slot;
    if (focused && !disabled) {
        int down = nk_input_is_key_down(&ctx->input, NK_KEY_ENTER);
        int edges = ctx->input.keyboard.keys[NK_KEY_ENTER].clicked;

        if ((down && edges == 1) || (!down && edges == 2))
            clicked = 1;
    }

    /* Hover dims while the pointer is down elsewhere (a drag over the button
     * is not a hover), but only a press that began here sinks the face. */
    if (hovered && nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
        hovered = 0;
    anim->hover = mp_ease(anim->hover, hovered ? 1.0f : 0.0f, dt, MP_HOVER_TAU);
    anim->press = mp_ease(anim->press, anim->pressed ? 1.0f : 0.0f, dt,
                          MP_PRESS_TAU);

    canvas = nk_window_get_canvas(ctx);
    shift = anim->press * mp_s(1.0f, scale);

    /* Shadow, face, gloss, border, in that order: the shadow sits under the
     * raised face and is not drawn on a disabled or already-flat button. */
    if (!disabled)
        nk_fill_rect(canvas, nk_rect(r.x, r.y + mp_s(2.0f, scale) - shift,
                                     r.w, r.h), radius,
                     mp_col_alpha(th->shadow, MP_BTN_SHADOW_A));
    mp_btn_face(ctx, nk_rect(r.x, r.y + shift, r.w, r.h), radius, accent,
                anim->hover, anim->press, th);
    mp_btn_gloss(ctx, nk_rect(r.x, r.y + shift, r.w, r.h), radius, border,
                 anim->press);
    nk_stroke_rect(canvas, nk_rect(r.x, r.y + shift, r.w, r.h),
                   radius - border * 0.5f, border,
                   disabled ? mp_col_alpha(th->border, MP_BTN_DISABLED_A)
                            : mp_col_lerp(th->border, th->border_strong,
                                          anim->hover));

    if (focused && !disabled)
        nk_stroke_rect(canvas,
                       nk_rect(r.x - mp_s(2.0f, scale), r.y - mp_s(2.0f, scale),
                               r.w + mp_s(4.0f, scale), r.h + mp_s(4.0f, scale)),
                       radius + mp_s(1.0f, scale), mp_s(2.0f, scale),
                       mp_col_alpha(th->accent_b, MP_BTN_FOCUS_A));

    if (label) {
        struct nk_rect tr;
        float pad = mp_s(MP_BTN_LABEL_PAD, scale);
        struct nk_color ink = accent ? th->accent_ink : th->text_primary;

        /* nk_draw_text top-aligns in its rect, so centre the line vertically
         * by hand. */
        tr = nk_rect(r.x + pad,
                     r.y + shift + (r.h - ctx->style.font->height) * 0.5f,
                     r.w - 2.0f * pad, ctx->style.font->height);
        /* A fully transparent background: the text draws its own glyphs and
         * the face beneath it is already painted. */
        nk_draw_text(canvas, tr, label, (int)strlen(label), ctx->style.font,
                     mp_col_alpha(MP_INK, 0),
                     disabled ? mp_col_alpha(ink, MP_BTN_DISABLED_A) : ink);
    } else {
        float ih = r.h * MP_ICON_FRAC;
        struct nk_color ink = accent ? th->accent_ink : th->text_primary;

        mp_draw_icon(ctx, nk_rect(r.x + (r.w - ih) * 0.5f,
                                  r.y + shift + (r.h - ih) * 0.5f, ih, ih),
                     icon, disabled ? mp_col_alpha(ink, MP_BTN_DISABLED_A)
                                    : ink, scale);
    }

    return clicked;
}

/* Seek bar and volume slider. Sizes in logical points. The seek track is the
 * dock's thickest line and the volume track thinner, and the volume handle is
 * smaller than the seek handle, so the two read as one family with volume
 * deliberately subordinate. */
#define MP_SEEK_TRACK_H      6.0f
#define MP_SEEK_HANDLE_D    14.0f
#define MP_VOL_TRACK_H       4.0f
#define MP_VOL_HANDLE_D     12.0f

/* The handle's growth on hover/drag and its easing tau (the PRD's 90 ms).
 * Hover itself eases on MP_HOVER_TAU above, so both widgets warm up at the
 * same rate as the buttons. */
#define MP_HANDLE_GROW       1.15f
#define MP_HANDLE_TAU        0.090f

/* Alphas (0..255) of the handle: the halo at full strength (a drag) and the
 * ring width that separates the accent ring from the text_primary face. */
#define MP_SEEK_GLOW_A       102
#define MP_SEEK_RING_PT        2.0f

/* Fill alphas: the track's inner edge, the "wet" top band, and the muted fill
 * the disabled volume shows at its current value. */
#define MP_TRACK_EDGE_A       64
#define MP_TRACK_SHEEN_A      46
#define MP_FILL_MUTED_A      110

/* Track lift on hover: a tenth of the way toward surface_hover, so the bar
 * brightens slightly without competing with the buttons. */
#define MP_TRACK_HOVER_LIFT  0.10f

/* Bubble padding, the gap that floats it above the track, and the fill alpha
 * of its surface. The bubble is sized from the font and floats over the row
 * above the bar: an 18 pt bubble and a 14 pt handle cannot both fit in a
 * 35 pt row, and the seek link is the dock's most important control. */
#define MP_BUBBLE_PAD_X        7.0f
#define MP_BUBBLE_PAD_Y        3.0f
#define MP_BUBBLE_GAP          4.0f
#define MP_BUBBLE_SURFACE_A  235
#define MP_BUBBLE_BORDER_A   204

/* The speaker glyph's box, and the narrowest row that still gets one: below
 * it the glyph would squeeze the track to nothing. */
#define MP_VOL_GLYPH_W        16.0f
#define MP_VOL_GLYPH_H        12.0f
#define MP_VOL_GLYPH_MIN_ROW 120.0f

/* Text width through the font callback: this Nuklear build exposes no
 * nk_text_width helper, only the user font's `width` hook. */
static float
mp_text_w(const struct nk_user_font *font, const char *s)
{
    return font->width(font->userdata, font->height, s, (int)strlen(s));
}

/* m:ss of `secs`, rounded to the nearest second and never negative. */
static void
mp_clock_text(char *out, size_t outlen, double secs)
{
    int t;

    if (secs < 0.0)
        secs = 0.0;
    t = (int)(secs + 0.5);
    snprintf(out, outlen, "%d:%02d", t / 60, t % 60);
}

/* The track floats in its row, centred vertically, so the row keeps its dock
 * height and the handle has room to grow. */
static struct nk_rect
mp_track_rect(struct nk_rect row, float bar_h)
{
    return nk_rect(row.x, row.y + (row.h - bar_h) * 0.5f, row.w, bar_h);
}

/* Pointer x as a 0..1 fraction of `sub`. Off-track values clamp, so a drag
 * that leaves the bar still previews 0 or 1 rather than wrapping. */
static float
mp_pointer_frac(const struct nk_input *in, struct nk_rect sub)
{
    float f;

    if (sub.w <= 0.0f)
        return 0.0f;
    f = (in->mouse.pos.x - sub.x) / sub.w;
    if (f < 0.0f)
        return 0.0f;
    if (f > 1.0f)
        return 1.0f;
    return f;
}

/* The track pill plus a faint inner edge, which stops a thin bar reading as a
 * flat stripe. `lift` brightens the fill on hover. */
static void
mp_track_draw(struct nk_context *ctx, struct nk_rect tr,
    const struct MpTheme *th, float scale, float lift)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float radius = tr.h * 0.5f;
    float edge = mp_s(1.0f, scale);
    struct nk_rect inner = nk_rect(tr.x, tr.y + edge, tr.w, tr.h - 2.0f * edge);

    nk_fill_rect(canvas, tr, radius,
                 mp_col_lerp(th->track, th->surface_hover, lift));
    if (inner.w > 0.0f && inner.h > 0.0f)
        nk_fill_rect(canvas, inner, inner.h * 0.5f,
                     mp_col_alpha(MP_INK, MP_TRACK_EDGE_A));
}

/* The accent gradient fill, drawn only up to `frac` of the whole track.
 *
 * Two constraints shape this. The gradient is anchored to the *track*: the
 * ramp's colour at an x is a function of x/track.w only, so colours do not
 * slide as the handle is dragged -- Nuklear's multi-colour rect interpolates
 * across whatever rect it is handed, so the band's endpoint colours are
 * sampled from the full-track ramp rather than being the ramp's ends.
 *
 * And the fill has to be a pill, but a rect cannot round only two corners. So
 * the shape is a band between two disc centres plus a disc at each end: the
 * union is exactly the rounded fill, and the discs are drawn last so their
 * rims sit over the band's square edges. `muted` drains the alpha for the
 * disabled volume. */
static void
mp_fill_draw(struct nk_context *ctx, struct nk_rect track, float frac,
    const struct MpTheme *th, float scale, int muted)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_color a = muted ? mp_col_alpha(th->accent_a, MP_FILL_MUTED_A)
                              : th->accent_a;
    struct nk_color b = muted ? mp_col_alpha(th->accent_b, MP_FILL_MUTED_A)
                              : th->accent_b;
    float w = track.w * frac;
    float r = track.h * 0.5f;
    float cy = track.y + r;
    float sheen = mp_s(1.0f, scale);
    struct nk_color c0, c1, hi_a, hi_b;

    if (w <= 0.0f || r <= 0.0f || track.w <= 0.0f)
        return;

    /* Narrower than the two caps need: one disc the fill's own width, so the
     * colour never reaches past the value. */
    if (w < 2.0f * r) {
        nk_fill_circle(canvas, nk_rect(track.x, cy - w * 0.5f, w, w),
                       mp_col_lerp(a, b, w * 0.5f / track.w));
        return;
    }

    /* The ramp sampled at the two cap centres. */
    c0 = mp_col_lerp(a, b, r / track.w);
    c1 = mp_col_lerp(a, b, (w - r) / track.w);

    /* Band between the cap centres, then the caps over its square ends. */
    nk_fill_rect_multi_color(canvas,
        nk_rect(track.x + r, cy - r, w - 2.0f * r, 2.0f * r),
        c0, c0, c1, c1);
    nk_fill_circle(canvas, nk_rect(track.x, cy - r, 2.0f * r, 2.0f * r), a);
    nk_fill_circle(canvas, nk_rect(track.x + w - 2.0f * r, cy - r,
                                   2.0f * r, 2.0f * r), c1);

    /* The wet band: a hairline along the top of the fill, paler at the fill's
     * end. Only the seek track is tall enough to carry it. */
    if (track.h >= mp_s(5.0f, scale) && w > 2.0f * r) {
        hi_a = mp_col_alpha(mp_col_lerp(c0, MP_HIGHLIGHT,
                                        (float)MP_TRACK_SHEEN_A / 255.0f),
                            MP_TRACK_SHEEN_A);
        hi_b = mp_col_alpha(mp_col_lerp(c1, MP_HIGHLIGHT,
                                        (float)MP_TRACK_SHEEN_A / 255.0f),
                            MP_TRACK_SHEEN_A);
        nk_fill_rect_multi_color(canvas,
            nk_rect(track.x + r, track.y + sheen, w - 2.0f * r, sheen),
            hi_a, hi_a, hi_b, hi_b);
    }
}

/* The handle, as the PRD specifies it: a 2 pt accent ring around a
 * text_primary face, and a soft accent glow. `size` already carries the eased
 * hover/drag growth; `glow` (0..1) sizes a halo that grows with the ring. */
static void
mp_handle_draw(struct nk_context *ctx, float cx, float cy, float size,
    const struct MpTheme *th, float scale, float glow)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float half = size * 0.5f;
    float ring = mp_s(MP_SEEK_RING_PT, scale);
    float inner = size - 2.0f * ring;

    if (inner < 1.0f)
        inner = 1.0f;

    if (glow > 0.0f) {
        /* Three concentric discs rather than one: alpha blends over the
         * overlaps, so the stack reads as a falloff instead of a flat coin.
         * Drawn outermost first so the centre is the brightest. */
        struct nk_color glowc = mp_col_alpha(th->accent_a,
            (int)lroundf((float)MP_SEEK_GLOW_A * glow / 3.0f));
        int i;

        for (i = 3; i >= 1; i--) {
            float d = 2.0f * (half + half * 0.35f * glow * (float)i);

            nk_fill_circle(canvas, nk_rect(cx - d * 0.5f, cy - d * 0.5f, d, d),
                           glowc);
        }
    }
    nk_fill_circle(canvas, nk_rect(cx - half, cy - half, size, size),
                   th->accent_a);
    nk_fill_circle(canvas,
        nk_rect(cx - inner * 0.5f, cy - inner * 0.5f, inner, inner),
        th->text_primary);
}

/* The hover time bubble: a surface pill with the target m:ss centred in it.
 * `bubble` is already positioned inside the dock; this only paints and centres
 * the text, with a transparent text background so the pill shows through. */
static void
mp_bubble_draw(struct nk_context *ctx, struct nk_rect bubble, const char *text,
    const struct MpTheme *th, float scale, int dragging)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float radius = bubble.h * 0.5f;
    float border = mp_s(1.0f, scale);
    struct nk_color edge = dragging ? th->border_strong : th->border;
    const struct nk_user_font *font = ctx->style.font;
    float w = mp_text_w(font, text);

    nk_fill_rect(canvas, bubble, radius,
                 mp_col_alpha(th->surface, MP_BUBBLE_SURFACE_A));
    nk_stroke_rect(canvas, bubble, radius, border,
                   mp_col_alpha(edge, MP_BUBBLE_BORDER_A));
    /* nk_draw_text lays the line out from its rect's top-left with the font's
     * own height, so centre both axes by hand. */
    nk_draw_text(canvas,
                 nk_rect(bubble.x + (bubble.w - w) * 0.5f,
                         bubble.y + (bubble.h - font->height) * 0.5f,
                         w, font->height),
                 text, (int)strlen(text), font,
                 mp_col_alpha(MP_INK, 0), th->text_primary);
}

int
mp_seek_bar(struct nk_context *ctx, float scale, const struct MpTheme *th,
    float frac, int enabled, int *dragging, float *preview, double duration,
    float dt, struct MpSeekAnim *anim)
{
    struct nk_rect row, track;
    enum nk_widget_layout_states state;
    float bar_h, shown, hx, size;
    int disabled, hovered, release = 0;

    state = nk_widget(&row, ctx);
    if (state == NK_WIDGET_INVALID)
        return 0;

    /* The drag interaction, ported from the old main.c bar: nk_input_mouse_
     * clicked fires on release, so start on press and seed the preview from
     * the press point; once a drag is running it owns the pointer even
     * outside `row`, so a release anywhere ends it. Clamp after the update so
     * a pointer that leaves the bar previews 0 or 1, never an out-of-range
     * seek. */
    if (enabled && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
        nk_input_is_mouse_hovering_rect(&ctx->input, row)) {
        *dragging = 1;
        *preview = mp_pointer_frac(&ctx->input, row);
    } else if (*dragging) {
        if (!enabled) {
            *dragging = 0;
        } else if (nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
            *preview = mp_pointer_frac(&ctx->input, row);
        } else {
            *dragging = 0;
            release = 1;
        }
    }

    if (*preview < 0.0f)
        *preview = 0.0f;
    if (*preview > 1.0f)
        *preview = 1.0f;

    /* `nk_widget` reports the window's disable flag as its own state; the
     * caller's flag covers "modal up" without pushing a style. */
    disabled = !enabled || state == NK_WIDGET_DISABLED;
    hovered = !disabled &&
        nk_input_is_mouse_hovering_rect(&ctx->input, row) ? 1 : 0;

    /* Hover dims while a drag runs elsewhere (the pointer is holding this
     * bar, not hovering it), matching the buttons. */
    if (hovered && nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
        hovered = 0;
    anim->hover = mp_ease(anim->hover, hovered ? 1.0f : 0.0f, dt, MP_HOVER_TAU);
    anim->handle = mp_ease(anim->handle,
        (hovered || *dragging) ? MP_HANDLE_GROW : 1.0f, dt, MP_HANDLE_TAU);
    anim->glow = mp_ease(anim->glow, *dragging ? 1.0f : hovered ? 0.5f : 0.0f,
                         dt, MP_HANDLE_TAU);

    /* While dragging the fill follows the preview, as the time label beside it
     * does; a disabled bar shows the flat track and nothing else. */
    if (disabled)
        shown = 0.0f;
    else
        shown = *dragging ? *preview : frac;
    if (shown < 0.0f)
        shown = 0.0f;
    if (shown > 1.0f)
        shown = 1.0f;

    bar_h = mp_s(MP_SEEK_TRACK_H, scale);
    if (bar_h < 1.0f)
        bar_h = 1.0f;
    track = mp_track_rect(row, bar_h);

    mp_track_draw(ctx, track, th, scale, anim->hover * MP_TRACK_HOVER_LIFT);
    if (disabled)
        return release;
    mp_fill_draw(ctx, track, shown, th, scale, 0);

    /* Inset the centre by the handle's own radius so the disc never pokes out
     * of the track at 0 or 1, where the glow would clip against the dock. */
    size = mp_s(MP_SEEK_HANDLE_D, scale) * anim->handle;
    if (size < 1.0f)
        size = 1.0f;
    hx = track.x + track.w * shown;
    if (hx < track.x + size * 0.5f)
        hx = track.x + size * 0.5f;
    if (track.w > size && hx > track.x + track.w - size * 0.5f)
        hx = track.x + track.w - size * 0.5f;
    mp_handle_draw(ctx, hx, track.y + track.h * 0.5f, size, th, scale,
                   anim->glow);

    /* The bubble previews where a click would land, so its target is the
     * pointer as a fraction of the track; while dragging that is the preview.
     * It is centred on the handle and floats above the track, over the row
     * above -- the only placement with room for it, and the reason it is drawn
     * last. The horizontal clamp keeps it inside the bar's width. */
    if (duration > 0.0 && (hovered || *dragging)) {
        char buf[16];
        const struct nk_user_font *font = ctx->style.font;
        float bh = font->height + mp_s(MP_BUBBLE_PAD_Y, scale) * 2.0f;
        float bw, bx, by;
        float target = *dragging ? shown : mp_pointer_frac(&ctx->input, track);

        mp_clock_text(buf, sizeof buf, (double)target * duration);
        bw = mp_text_w(font, buf) + mp_s(MP_BUBBLE_PAD_X, scale) * 2.0f;
        if (bw > row.w)
            bw = row.w;
        bx = hx - bw * 0.5f;
        if (bx < row.x)
            bx = row.x;
        if (bx + bw > row.x + row.w)
            bx = row.x + row.w - bw;
        by = track.y - mp_s(MP_BUBBLE_GAP, scale) - bh;
        if (by < 0.0f)
            by = 0.0f;
        mp_bubble_draw(ctx, nk_rect(bx, by, bw, bh), buf, th, scale, *dragging);
    }

    return release;
}

/* The speaker glyph: a box, a trapezoid cone drawn as two triangles off the
 * box's right edge, and two arcs of sound to the right of the cone's mouth.
 * The arcs are stroked, not filled, so they read as thin waves. */
static void
mp_vol_speaker(struct nk_context *ctx, struct nk_rect box, struct nk_color col,
    float scale)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float cy = box.y + box.h * 0.5f;
    float bx = box.x + box.w * 0.08f;
    float bw = box.w * 0.18f;
    float body = box.h * 0.24f;
    float mouth = box.h * 0.44f;
    float tip = bx + box.w * 0.44f;
    float ring = mp_s(1.0f, scale);

    /* Body, then the cone as a trapezoid: one triangle from the body's top
     * edge out to the mouth, one closing the bottom. */
    nk_fill_rect(canvas, nk_rect(bx, cy - body, bw, 2.0f * body), 0.0f, col);
    nk_fill_triangle(canvas, bx, cy - body, tip, cy - mouth, tip, cy + mouth,
                     col);
    nk_fill_triangle(canvas, bx, cy - body, bx, cy + body, tip, cy + mouth,
                     col);

    /* Two arcs of sound to the right of the mouth, the outer one dimmer so it
     * reads as a falloff rather than a second shape. */
    nk_stroke_arc(canvas, tip, cy, box.w * 0.14f, -0.8f, 0.8f, ring, col);
    nk_stroke_arc(canvas, tip, cy, box.w * 0.26f, -0.8f, 0.8f, ring,
                  mp_col_alpha(col, 140));
}

int
mp_volume_slider(struct nk_context *ctx, float scale, const struct MpTheme *th,
    float *value, int enabled, float dt, struct MpVolAnim *anim)
{
    struct nk_rect row, track;
    enum nk_widget_layout_states state;
    float bar_h, shown, hx, size, glyph_w = 0.0f, newv;
    int disabled, hovered, dragging = 0, changed = 0;

    state = nk_widget(&row, ctx);
    if (state == NK_WIDGET_INVALID)
        return 0;

    /* The caller recomputes `*value` from the engine every frame, so this is a
     * controlled widget: a drag writes it back and the next frame's status
     * fetch drives the handle. Clamp defensively in case the engine reports an
     * out-of-range value. */
    if (*value < 0.0f)
        *value = 0.0f;
    if (*value > 1.0f)
        *value = 1.0f;
    disabled = !enabled || state == NK_WIDGET_DISABLED;

    bar_h = mp_s(MP_VOL_TRACK_H, scale);
    if (bar_h < 1.0f)
        bar_h = 1.0f;

    /* A speaker glyph leads the track when the row has the width for it; a
     * narrow row drops it rather than squeezing the track to nothing. */
    if (row.w >= mp_s(MP_VOL_GLYPH_MIN_ROW, scale)) {
        struct nk_rect glyph;

        glyph_w = mp_s(MP_VOL_GLYPH_W, scale);
        glyph = nk_rect(row.x, row.y + (row.h - mp_s(MP_VOL_GLYPH_H, scale))
                                * 0.5f,
                        glyph_w, mp_s(MP_VOL_GLYPH_H, scale));
        mp_vol_speaker(ctx, glyph,
                       mp_col_alpha(th->text_secondary,
                                    disabled ? MP_FILL_MUTED_A : 200), scale);
    }
    track = mp_track_rect(row, bar_h);
    track.x += glyph_w;
    track.w -= glyph_w;
    if (track.w < 0.0f)
        track.w = 0.0f;

    /* Interaction mirrors a slider: a press on the row claims the drag until
     * the button comes up, even past the row's edges. `clicked_pos` is where
     * the press landed and it outlives the frame, so this needs no per-widget
     * state: `is_mouse_down` alone would also drag when a press began on the
     * seek bar or the transport. */
    hovered = 0;
    if (!disabled) {
        hovered = nk_input_is_mouse_hovering_rect(&ctx->input, row) ? 1 : 0;
        dragging = nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT) &&
            nk_input_has_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, row);
        if (dragging) {
            newv = mp_pointer_frac(&ctx->input, track);
            /* Report every change, not just big ones: the caller re-derives
             * the value from the engine next frame, so a skipped sub-pixel
             * update would snap the handle back. The engine call is two stores
             * and an atomic set, and the caller's integer percent makes repeat
             * calls free. */
            if (newv != *value) {
                *value = newv;
                changed = 1;
            }
        }
    }

    anim->hover = mp_ease(anim->hover, hovered ? 1.0f : 0.0f, dt, MP_HOVER_TAU);
    anim->handle = mp_ease(anim->handle,
        (hovered || dragging) ? MP_HANDLE_GROW : 1.0f, dt, MP_HANDLE_TAU);

    shown = *value;
    if (shown < 0.0f)
        shown = 0.0f;
    if (shown > 1.0f)
        shown = 1.0f;

    mp_track_draw(ctx, track, th, scale, anim->hover * MP_TRACK_HOVER_LIFT);
    mp_fill_draw(ctx, track, shown, th, scale, disabled ? 1 : 0);

    if (!disabled) {
        size = mp_s(MP_VOL_HANDLE_D, scale) * anim->handle;
        if (size < 1.0f)
            size = 1.0f;
        hx = track.x + track.w * shown;
        if (hx < track.x + size * 0.5f)
            hx = track.x + size * 0.5f;
        if (track.w > size && hx > track.x + track.w - size * 0.5f)
            hx = track.x + track.w - size * 0.5f;
        mp_handle_draw(ctx, hx, track.y + track.h * 0.5f, size, th, scale,
                       dragging ? 1.0f : anim->hover * 0.5f);
    }

    return changed;
}

/* ---- Phase 5: content hierarchy and states ------------------------------ */

#define MP_CHIP_PAD_X       8.0f  /* logical pt inside a chip, each side */
#define MP_CHIP_H_EXTRA     8.0f  /* chip height above the font height */
#define MP_CHIP_GAP         6.0f
#define MP_CHIP_ROUND       9.0f
#define MP_DOT_D            8.0f
#define MP_DOT_SLOT         16.0f /* width reserved for the dot beside the name */
#define MP_STATUS_NAME_MIN  120.0f /* chips drop before the name gets this short */
#define MP_ERR_PAD_X        12.0f
#define MP_ERR_ROUND        9.0f
#define MP_ROW_ROUND        4.0f  /* browser row fill corner */
#define MP_ROW_BAR          3.0f  /* selected row's accent bar width */
#define MP_ROW_PAD_X        10.0f
#define MP_EMPTY_DISC_MAX   64.0f
#define MP_EMPTY_DISC_MIN   24.0f
#define MP_EMPTY_GAP        10.0f
#define MP_GLOW_RING        3.0f  /* logical pt the playing dot's halo extends */

/* Text height and colour helpers for the new rows. */
static void
mp_draw_line(struct nk_command_buffer *canvas, const struct nk_user_font *font,
    struct nk_rect r, const char *s, struct nk_color fg)
{
    nk_draw_text(canvas, r, s, (int)strlen(s), font,
                 mp_col_alpha(MP_INK, 0), fg);
}

/* Shrinks `in` to fit max_w, ending in "..." (the default font has no
 * ellipsis glyph). Cuts on UTF-8 boundaries so a multibyte sequence is never
 * split. Binary search: ~log(n) width calls, not one per dropped byte. */
static void
mp_elide(const struct nk_user_font *font, const char *in, float max_w,
    char *out, size_t outlen)
{
    size_t lo = 0, hi, len;

    if (outlen == 0)
        return;
    len = strlen(in);
    if (mp_text_w(font, in) <= max_w) {
        snprintf(out, outlen, "%s", in);
        return;
    }
    hi = len;
    /* Invariant: prefix of length lo fits (lo=0 is "..."), hi does not. The
     * probe may end mid-character; only the final cut is snapped, which can
     * only shorten it, so it still fits. */
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        char probe[600];

        snprintf(probe, sizeof probe, "%.*s...", (int)mid, in);
        if (mp_text_w(font, probe) <= max_w)
            lo = mid;
        else
            hi = mid - 1;
    }
    /* Back off to a UTF-8 lead byte so the cut is a whole character. */
    while (lo > 0 && ((unsigned char)in[lo] & 0xC0) == 0x80)
        lo--;
    snprintf(out, outlen, "%.*s...", (int)lo, in);
}

/* A rounded chip: surface pill with a 1 px border. The text is centred in it;
 * the caller has already sized `r` from the text width. */
static void
mp_chip_draw(struct nk_command_buffer *canvas, const struct nk_user_font *font,
    const struct MpTheme *th, struct nk_rect r, const char *s,
    struct nk_color fg, float scale)
{
    float tw = mp_text_w(font, s);

    nk_fill_rect(canvas, r, r.h * 0.5f, th->surface);
    nk_stroke_rect(canvas, r, r.h * 0.5f, mp_s(1.0f, scale), th->border);
    mp_draw_line(canvas, font,
                 nk_rect(r.x + (r.w - tw) * 0.5f,
                         r.y + (r.h - font->height) * 0.5f, tw, font->height),
                 s, fg);
}

static float
mp_chip_w(const struct nk_user_font *font, const char *s, float scale)
{
    return mp_text_w(font, s) + 2.0f * mp_s(MP_CHIP_PAD_X, scale);
}

/* Chip row from the right edge of `r`, first chip rightmost. Returns the
 * left edge reached so the caller knows what space is left. */
static float
mp_chips_right(struct nk_command_buffer *canvas, const struct nk_user_font *font,
    const struct MpTheme *th, struct nk_rect r, const char *const *chips,
    int nchips, float scale)
{
    float right = r.x + r.w;
    float h = font->height + mp_s(MP_CHIP_H_EXTRA, scale);
    float y = r.y + (r.h - h) * 0.5f;
    int i;

    for (i = nchips - 1; i >= 0; i--) {
        float w = mp_chip_w(font, chips[i], scale);
        mp_chip_draw(canvas, font, th, nk_rect(right - w, y, w, h), chips[i],
                     th->text_secondary, scale);
        right -= w + mp_s(MP_CHIP_GAP, scale);
    }
    return right;
}

/* Filename, dot, and chips in one header cell. The dot slot is reserved
 * whenever a track is loaded, so the name does not shift on play/pause.
 * Chips drop from the end of the list (the last ones first) when the name
 * would get shorter than MP_STATUS_NAME_MIN. */
void
mp_status_row(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *name, const char *const *chips, int nchips,
    enum mp_play_state ps)
{
    struct nk_rect row;
    struct nk_command_buffer *canvas;
    const struct nk_user_font *font = ctx->style.font;
    enum nk_widget_layout_states state;
    float dot_slot, name_x, name_w, chips_w = 0.0f, min_w;
    int shown = nchips;
    char elided[600];

    state = nk_widget(&row, ctx);
    if (state == NK_WIDGET_INVALID)
        return;
    canvas = nk_window_get_canvas(ctx);

    if (!name) {
        mp_draw_line(canvas, font,
                     nk_rect(row.x, row.y + (row.h - font->height) * 0.5f,
                             row.w, font->height),
                     "no file loaded", th->text_secondary);
        return;
    }

    dot_slot = mp_s(MP_DOT_SLOT, scale);
    min_w = mp_s(MP_STATUS_NAME_MIN, scale);
    for (;;) {
        float right_w = 0.0f;
        int k;

        for (k = 0; k < shown; k++)
            right_w += mp_chip_w(font, chips[k], scale) +
                mp_s(MP_CHIP_GAP, scale);
        if (row.w - dot_slot - right_w >= min_w || shown == 0) {
            chips_w = right_w;
            break;
        }
        shown--;
    }

    name_x = row.x + dot_slot;
    name_w = row.w - dot_slot - chips_w;
    if (name_w < 1.0f)
        name_w = 1.0f;

    /* The dot: a solid accent with a halo while playing, a hollow ring while
     * paused, nothing when stopped. The accent is a state signal here, not
     * decoration. */
    if (ps != MP_PS_STOPPED) {
        float cy = row.y + row.h * 0.5f;
        float d = mp_s(MP_DOT_D, scale);
        float cx = row.x + dot_slot * 0.5f;
        struct nk_rect dr = nk_rect(cx - d * 0.5f, cy - d * 0.5f, d, d);

        if (ps == MP_PS_PLAYING) {
            float g = mp_s(MP_GLOW_RING, scale);
            nk_fill_circle(canvas,
                nk_rect(dr.x - g, dr.y - g, d + 2.0f * g, d + 2.0f * g),
                mp_col_alpha(th->accent_a, 56));
            nk_fill_circle(canvas, dr, th->accent_a);
        } else {
            nk_stroke_circle(canvas, dr, mp_s(1.5f, scale),
                             mp_col_alpha(th->accent_a, 180));
        }
    }

    mp_elide(font, name, name_w, elided, sizeof elided);
    mp_draw_line(canvas, font,
                 nk_rect(name_x, row.y + (row.h - font->height) * 0.5f,
                         name_w, font->height),
                 elided, th->text_primary);

    if (shown > 0)
        mp_chips_right(canvas, font, th, row, chips, shown, scale);
}

/* Error message in a danger panel. Long messages are elided; the full text
 * comes up as a tooltip while the pointer is over the panel. */
void
mp_error_panel(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *text)
{
    struct nk_rect row, panel;
    struct nk_command_buffer *canvas;
    const struct nk_user_font *font = ctx->style.font;
    enum nk_widget_layout_states state;
    float pad, avail, border;
    char elided[600];

    state = nk_widget(&row, ctx);
    if (state == NK_WIDGET_INVALID)
        return;
    canvas = nk_window_get_canvas(ctx);

    /* A 2 px inset keeps the rounded panel inside the row (no per-rect clip
     * in Nuklear, so overlays stay inside by construction). */
    panel = nk_rect(row.x, row.y + mp_s(2.0f, scale), row.w,
                    row.h - mp_s(4.0f, scale));
    border = mp_s(1.0f, scale);
    nk_fill_rect(canvas, panel, mp_s(MP_ERR_ROUND, scale), th->danger_surface);
    nk_stroke_rect(canvas, panel, mp_s(MP_ERR_ROUND, scale), border,
                   th->danger);

    pad = mp_s(MP_ERR_PAD_X, scale);
    avail = panel.w - 2.0f * pad;
    if (avail < 1.0f)
        avail = 1.0f;
    mp_elide(font, text, avail, elided, sizeof elided);
    mp_draw_line(canvas, font,
                 nk_rect(panel.x + pad,
                         panel.y + (panel.h - font->height) * 0.5f,
                         avail, font->height),
                 elided, th->danger);

    if (strcmp(elided, text) != 0 &&
        nk_input_is_mouse_hovering_rect(&ctx->input, panel))
        nk_tooltip(ctx, text);
}

/* "cur / total": the current time in text_primary, the total in
 * text_secondary. The font is monospaced, so the centring maths is stable. */
void
mp_time_label(struct nk_context *ctx, const struct MpTheme *th,
    const char *cur, const char *total)
{
    struct nk_rect row;
    struct nk_command_buffer *canvas;
    const struct nk_user_font *font = ctx->style.font;
    enum nk_widget_layout_states state;
    float wc, wsep, wt, x;
    float y;

    state = nk_widget(&row, ctx);
    if (state == NK_WIDGET_INVALID)
        return;
    canvas = nk_window_get_canvas(ctx);

    wc = mp_text_w(font, cur);
    wsep = mp_text_w(font, " / ");
    wt = mp_text_w(font, total);
    x = row.x + (row.w - (wc + wsep + wt)) * 0.5f;
    y = row.y + (row.h - font->height) * 0.5f;

    mp_draw_line(canvas, font, nk_rect(x, y, wc, font->height), cur,
                 th->text_primary);
    mp_draw_line(canvas, font, nk_rect(x + wc, y, wsep + wt, font->height),
                 " / ", th->text_secondary);
    mp_draw_line(canvas, font, nk_rect(x + wc + wsep, y, wt, font->height),
                 total, th->text_secondary);
}

/* Hint text in text_muted. Lives here so main.c names no colour. */
void
mp_label_muted(struct nk_context *ctx, const struct MpTheme *th,
    const char *text, nk_flags align)
{
    nk_label_colored(ctx, text, align, th->text_muted);
}

/* One file-browser row. Same contract as nk_select_label: returns 1 on a
 * click, and the caller owns the selection. Selected rows get a tinted fill and
 * an accent bar; directories are drawn in the accent. Long names elide with the
 * full name as a tooltip. */
int
mp_list_row(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *text, int is_dir, int selected)
{
    struct nk_rect row, r;
    struct nk_command_buffer *canvas;
    const struct nk_user_font *font = ctx->style.font;
    enum nk_widget_layout_states state;
    struct nk_color fg;
    float inset, pad, avail, round;
    int hovered, clicked;
    char elided[600];

    state = nk_widget(&row, ctx);
    if (state == NK_WIDGET_INVALID)
        return 0;
    canvas = nk_window_get_canvas(ctx);

    /* Inset so the rounded fills stay inside the row cell. */
    inset = mp_s(1.0f, scale);
    r = nk_rect(row.x + inset, row.y + inset,
                row.w - 2.0f * inset, row.h - 2.0f * inset);
    round = mp_s(MP_ROW_ROUND, scale);

    hovered = nk_input_is_mouse_hovering_rect(&ctx->input, row) ? 1 : 0;
    clicked = nk_input_is_mouse_click_in_rect(&ctx->input, NK_BUTTON_LEFT, row);

    if (selected) {
        nk_fill_rect(canvas, r, round, mp_col_alpha(th->accent_a, 48));
        nk_fill_rect(canvas,
                     nk_rect(r.x, r.y + mp_s(4.0f, scale),
                             mp_s(MP_ROW_BAR, scale), r.h - mp_s(8.0f, scale)),
                     0.0f, th->accent_a);
    } else if (hovered) {
        nk_fill_rect(canvas, r, round, th->surface_hover);
    }

    fg = is_dir ? th->accent_a : th->text_primary;
    pad = mp_s(MP_ROW_PAD_X, scale);
    avail = r.w - 2.0f * pad;
    if (avail < 1.0f)
        avail = 1.0f;
    mp_elide(font, text, avail, elided, sizeof elided);
    mp_draw_line(canvas, font,
                 nk_rect(r.x + pad, r.y + (r.h - font->height) * 0.5f,
                         avail, font->height),
                 elided, fg);

    if (strcmp(elided, text) != 0 && hovered)
        nk_tooltip(ctx, text);
    return clicked;
}

/* Empty playlist with nothing loaded: a drawn disc above a headline and a row
 * of supported extensions. The caller sets the row layout (one row, the band
 * height). The disc is dropped first when the band is too short for it. */
void
mp_empty_state(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const char *headline, const char *const *chips, int nchips)
{
    struct nk_rect row;
    struct nk_command_buffer *canvas;
    const struct nk_user_font *font = ctx->style.font;
    enum nk_widget_layout_states state;
    float gap = mp_s(MP_EMPTY_GAP, scale);
    float chip_h = font->height + mp_s(MP_CHIP_H_EXTRA, scale);
    float text_h = font->height;
    float fixed, disc = 0.0f, total, y, cx;
    int shown = nchips, i;

    state = nk_widget(&row, ctx);
    if (state == NK_WIDGET_INVALID)
        return;
    canvas = nk_window_get_canvas(ctx);

    fixed = text_h + gap + chip_h;
    if (row.h - fixed - gap >= mp_s(MP_EMPTY_DISC_MIN, scale)) {
        disc = row.h - fixed - gap;
        if (disc > mp_s(MP_EMPTY_DISC_MAX, scale))
            disc = mp_s(MP_EMPTY_DISC_MAX, scale);
    }
    total = fixed + (disc > 0.0f ? disc + gap : 0.0f);
    y = row.y + (row.h - total) * 0.5f;
    if (y < row.y)
        y = row.y;
    cx = row.x + row.w * 0.5f;

    if (disc > 0.0f) {
        struct nk_rect dr = nk_rect(cx - disc * 0.5f, y, disc, disc);
        float hub = disc * 0.22f;
        struct nk_rect hr = nk_rect(cx - hub * 0.5f, y + (disc - hub) * 0.5f,
                                    hub, hub);

        nk_fill_circle(canvas, dr, th->surface);
        nk_stroke_circle(canvas, dr, mp_s(1.0f, scale), th->border);
        /* A groove ring inside the disc, then the accent rim on top. */
        nk_stroke_circle(canvas,
            nk_rect(cx - disc * 0.36f, y + disc * 0.14f,
                    disc * 0.72f, disc * 0.72f),
            mp_s(1.0f, scale), mp_col_alpha(th->border_strong, 140));
        nk_stroke_circle(canvas,
            nk_rect(dr.x + mp_s(3.0f, scale), dr.y + mp_s(3.0f, scale),
                    disc - mp_s(6.0f, scale), disc - mp_s(6.0f, scale)),
            mp_s(1.5f, scale), mp_col_alpha(th->accent_a, 150));
        nk_fill_circle(canvas, hr, th->surface_active);
        y += disc + gap;
    }

    {
        float tw = mp_text_w(font, headline);
        mp_draw_line(canvas, font,
                     nk_rect(cx - tw * 0.5f, y, tw, text_h), headline,
                     th->text_primary);
    }
    y += text_h + gap;

    /* Extension chips: shed from the end until the row fits. */
    for (;;) {
        float w = 0.0f;
        for (i = 0; i < shown; i++)
            w += mp_chip_w(font, chips[i], scale) + mp_s(MP_CHIP_GAP, scale);
        if (w - mp_s(MP_CHIP_GAP, scale) <= row.w || shown == 0)
            break;
        shown--;
    }
    {
        float cw = 0.0f, x;
        float gap_x = mp_s(MP_CHIP_GAP, scale);

        for (i = 0; i < shown; i++)
            cw += mp_chip_w(font, chips[i], scale) + (i ? gap_x : 0.0f);
        x = cx - cw * 0.5f;
        for (i = 0; i < shown; i++) {
            float w = mp_chip_w(font, chips[i], scale);
            mp_chip_draw(canvas, font, th, nk_rect(x, y, w, chip_h),
                         chips[i], th->text_muted, scale);
            x += w + gap_x;
        }
    }
}
