/* theme.c - the palette tokens and the Nuklear style built from them.
 *
 * "Midnight studio": one dark theme, cyan->violet accent, no second skin. The
 * tokens below are the source of truth; everything else here derives from them
 * or wraps them as semantic accessors so widget code never names a colour.
 */
#include "theme.h"

#include <math.h>
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
    /* Selected rows use the active step; Phase 6 adds the accent bar. */
    st->selectable.normal_active = nk_style_item_color(th->surface_active);
    st->selectable.hover_active = nk_style_item_color(th->surface_active);
    st->selectable.pressed_active = nk_style_item_color(th->surface_active);
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
    st->scrollh.normal = nk_style_item_color(th->surface);
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
        nk_fill_rect_multi_color(canvas, r, l, l, rr, rr);
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
    float band_h;

    inner = nk_rect(r.x + radius + border, r.y + radius + border,
                    r.w - 2.0f * (radius + border),
                    r.h - 2.0f * (radius + border));
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
     * so only Enter arrives) activates the focused button. nk_input_is_key_
     * pressed latches for as long as the key is held, so the press edge is
     * the down state crossed with the context's keyboard repeat counter --
     * which main() feeds from SDL with key repeat filtered out. */
    focused = opt && tab != 0 && tab == opt->focus_slot;
    if (focused && !disabled && nk_input_is_key_down(&ctx->input, NK_KEY_ENTER) &&
        ctx->input.keyboard.keys[NK_KEY_ENTER].clicked >= 1 && ctx->input.keyboard.keys[NK_KEY_ENTER].clicked < 2)
        clicked = 1;

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

    if (focused)
        nk_stroke_rect(canvas,
                       nk_rect(r.x - mp_s(2.0f, scale), r.y - mp_s(2.0f, scale),
                               r.w + mp_s(4.0f, scale), r.h + mp_s(4.0f, scale)),
                       radius + mp_s(1.0f, scale), mp_s(2.0f, scale),
                       mp_col_alpha(th->accent_b, MP_BTN_FOCUS_A));

    if (label) {
        struct nk_rect tr;
        float pad = mp_s(MP_BTN_LABEL_PAD, scale);
        struct nk_color ink = accent ? th->accent_ink : th->text_primary;

        tr = nk_rect(r.x + pad, r.y + shift, r.w - 2.0f * pad, r.h);
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
