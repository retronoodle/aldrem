/* theme.c - the palette tokens and the Nuklear style built from them.
 *
 * "Midnight studio": one dark theme, cyan->violet accent, no second skin. The
 * tokens below are the source of truth; everything else here derives from them
 * or wraps them as semantic accessors so widget code never names a colour.
 */
#include "theme.h"

#include <math.h>
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

void
mp_theme_init(struct MpTheme *th, SDL_Renderer *ren)
{
    /* The renderer is unused until Phase 2 generates the ambient glow; the
     * parameter stays so the call site does not change then. */
    (void)ren;

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
    th->ambient_glow = NULL;
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
    struct nk_color bg = th->bg_bottom;

    /* Phase 1 stub: a solid bg_bottom clear. Phase 2 replaces this with the
     * banded gradient and the cached ambient glow. */
    (void)w;
    (void)h;
    SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(ren);
}

void
mp_theme_shutdown(struct MpTheme *th)
{
    if (th->ambient_glow) {
        SDL_DestroyTexture(th->ambient_glow);
        th->ambient_glow = NULL;
    }
}
