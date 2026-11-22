/* mediaplayer — Phase 2: WAV playback engine wired to the Phase 1 layout.
 * A 900x520 window with Open (inert) + status, time label, read-only progress
 * slider, transport buttons and a disabled volume slider.
 * Esc quits (raw SDL event, matching the Phase 6 pattern).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_IMPLEMENTATION
#include "nuklear.h"
#define NK_SDL_RENDERER_IMPLEMENTATION
#include "nuklear_sdl_renderer.h"

#include "playback.h"

#define UI_DENSITY 0.8f
#define UI_FONT_BASE 19.0f

/* Renderer output in physical pixels and the backing scale ratio (points to
 * pixels). Nuklear lays out in renderer pixels, SDL delivers input in points. */
static float
ui_get_pixel_size(SDL_Window *win, SDL_Renderer *ren, int *px_w, int *px_h)
{
    int point_w = 0, point_h = 0;
    int out_w = 0, out_h = 0;

    SDL_GetWindowSize(win, &point_w, &point_h);
    if (SDL_GetRendererOutputSize(ren, &out_w, &out_h) != 0 ||
        out_w <= 0 || out_h <= 0) {
        out_w = point_w;
        out_h = point_h;
    }
    if (px_w)
        *px_w = out_w;
    if (px_h)
        *px_h = out_h;
    if (point_w <= 0)
        return 1.0f;
    return (float)out_w / (float)point_w;
}

static float
ui_s(float v, float s)
{
    return lroundf(v * s);
}

static struct nk_vec2
ui_sv(float x, float y, float s)
{
    return nk_vec2(ui_s(x, s), ui_s(y, s));
}

/* Scale the widget metrics this app uses from the Nuklear defaults. Callers
 * must call nk_style_default first so values are not compounded. */
static void
ui_scale_style(struct nk_context *ctx, float s)
{
    struct nk_style *st = &ctx->style;

    st->window.padding = ui_sv(4.0f, 4.0f, s);
    st->window.spacing = ui_sv(4.0f, 4.0f, s);
    st->window.scrollbar_size = ui_sv(10.0f, 10.0f, s);
    st->window.border = ui_s(2.0f, s);
    st->window.rounding = ui_s(0.0f, s);
    st->window.min_row_height_padding = ui_s(8.0f, s);

    st->text.padding = ui_sv(0.0f, 0.0f, s);

    st->button.padding = ui_sv(2.0f, 2.0f, s);
    st->button.image_padding = ui_sv(0.0f, 0.0f, s);
    st->button.border = ui_s(1.0f, s);
    st->button.rounding = ui_s(4.0f, s);

    st->slider.cursor_size = ui_sv(16.0f, 16.0f, s);
    st->slider.padding = ui_sv(2.0f, 2.0f, s);
    st->slider.spacing = ui_sv(2.0f, 2.0f, s);
    st->slider.bar_height = ui_s(4.0f, s);
    st->slider.rounding = ui_s(0.0f, s);
    st->slider.border = ui_s(0.0f, s);
}

static void
ui_select_font(struct nk_context *ctx, float ratio,
    struct nk_font *font_1x, struct nk_font *font_2x)
{
    struct nk_font *font = (ratio >= 1.5f) ? font_2x : font_1x;
    if (font)
        nk_style_set_font(ctx, &font->handle);
}

/* Nuklear receives physical-pixel coordinates; SDL reports points. */
static void
ui_scale_mouse_event(SDL_Event *evt, float ratio)
{
    switch (evt->type) {
    case SDL_MOUSEMOTION:
        evt->motion.x = (int)lroundf((float)evt->motion.x * ratio);
        evt->motion.y = (int)lroundf((float)evt->motion.y * ratio);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        evt->button.x = (int)lroundf((float)evt->button.x * ratio);
        evt->button.y = (int)lroundf((float)evt->button.y * ratio);
        break;
    default:
        break;
    }
}

int
main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    /* Audio is optional: without it the UI still runs with transport disabled. */
    if (SDL_Init(SDL_INIT_AUDIO) != 0)
        SDL_Log("SDL audio init failed: %s", SDL_GetError());

    SDL_Window *win = SDL_CreateWindow("mediaplayer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 900, 520,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    struct nk_context *ctx = nk_sdl_init(win, ren);
    struct nk_font_atlas *atlas;
    struct nk_font *font_1x, *font_2x;
    struct nk_font_config cfg = nk_font_config(0);
    char prev_quality[16];
    const char *quality = SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
    int had_quality = 0;

    /* Nearest filtering keeps 1:1 glyphs crisp; the font texture is created
     * inside nk_sdl_font_stash_end, so set it now and restore afterwards. */
    if (quality) {
        snprintf(prev_quality, sizeof prev_quality, "%s", quality);
        had_quality = 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    cfg.pixel_snap = 1;
    nk_sdl_font_stash_begin(&atlas);
    font_1x = nk_font_atlas_add_default(atlas, UI_FONT_BASE * UI_DENSITY, &cfg);
    font_2x = nk_font_atlas_add_default(atlas,
        UI_FONT_BASE * UI_DENSITY * 2.0f, &cfg);
    nk_sdl_font_stash_end();

    if (had_quality)
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, prev_quality);
    else
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, NULL);

    int px_w, px_h;
    float ratio = ui_get_pixel_size(win, ren, &px_w, &px_h);
    float ui_scale = ratio * UI_DENSITY;
    nk_style_default(ctx);
    ui_scale_style(ctx, ui_scale);
    ui_select_font(ctx, ratio, font_1x, font_2x);
    float style_scale = ui_scale;

    if (playback_init() != 0)
        SDL_Log("playback_init failed: %s", SDL_GetError());

    if (argc > 1) {
        char err[256];
        if (playback_load(argv[1], err, sizeof err) != 0)
            SDL_Log("playback_load failed: %s", err);
    }

    int running = 1;
    while (running) {
        /* Derive sizes fresh each frame: the resize event values are points,
         * while Nuklear lays out in physical pixels. */
        ratio = ui_get_pixel_size(win, ren, &px_w, &px_h);
        ui_scale = ratio * UI_DENSITY;
        if (ui_scale != style_scale) {
            nk_style_default(ctx);
            ui_scale_style(ctx, ui_scale);
            ui_select_font(ctx, ratio, font_1x, font_2x);
            style_scale = ui_scale;
        }

        SDL_Event evt;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT) {
                running = 0;
            } else if (evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                running = 0;
            } else if (evt.type == SDL_WINDOWEVENT &&
                       (evt.window.event == SDL_WINDOWEVENT_RESIZED ||
                        evt.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        evt.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)) {
                /* Refresh now so later events in this batch are scaled with
                 * the new backing ratio; the top of the next frame re-derives
                 * ui_scale and reapplies the style. */
                ratio = ui_get_pixel_size(win, ren, &px_w, &px_h);
            }
            ui_scale_mouse_event(&evt, ratio);
            nk_sdl_handle_event(&evt);
        }
        nk_input_end(ctx);

        PlaybackStatus st;
        char timebuf[64];
        char volbuf[32];
        const char *status_text;
        float progress, volume;

        playback_get_status(&st);

        if (st.has_track) {
            int cur = (int)st.position;
            int dur = (int)st.duration;
            if (cur < 0)
                cur = 0;
            if (cur > dur)
                cur = dur;
            snprintf(timebuf, sizeof timebuf, "%d:%02d / %d:%02d",
                     cur / 60, cur % 60, dur / 60, dur % 60);
        } else {
            snprintf(timebuf, sizeof timebuf, "--:-- / --:--");
        }

        progress = st.duration > 0.0 ? (float)(st.position / st.duration) : 0.0f;
        if (progress < 0.0f)
            progress = 0.0f;
        if (progress > 1.0f)
            progress = 1.0f;

        volume = (float)st.volume_percent / 100.0f;
        snprintf(volbuf, sizeof volbuf, "vol %d", st.volume_percent);

        if (st.error[0]) {
            status_text = st.error;
        } else if (st.has_track) {
            const char *slash = strrchr(st.name, '/');
            status_text = slash ? slash + 1 : st.name;
        } else {
            status_text = "no file loaded";
        }

        if (nk_begin(ctx, "mediaplayer",
                     nk_rect(0, 0, (float)px_w, (float)px_h),
                     NK_WINDOW_NO_SCROLLBAR))
        {
            /* row 1: Open button (fixed; file dialog is a later phase) + status */
            nk_layout_row_template_begin(ctx, ui_s(30.0f, ui_scale));
            nk_layout_row_template_push_static(ctx, ui_s(90.0f, ui_scale));
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            nk_button_label(ctx, "Open");
            nk_label(ctx, status_text, NK_TEXT_LEFT);

            /* row 2: time label, centered */
            nk_layout_row_dynamic(ctx, ui_s(35.0f, ui_scale), 1);
            nk_label(ctx, timebuf, NK_TEXT_CENTERED);

            /* row 3: progress slider — tracks the playhead, non-draggable */
            nk_layout_row_dynamic(ctx, ui_s(35.0f, ui_scale), 1);
            nk_widget_disable_begin(ctx);
            nk_slider_float(ctx, 0.0f, &progress, 1.0f, 0.01f);
            nk_widget_disable_end(ctx);

            /* row 4: transport buttons, disabled until a track is loaded */
            nk_layout_row_dynamic(ctx, ui_s(35.0f, ui_scale), 3);
            if (!st.has_track)
                nk_widget_disable_begin(ctx);
            if (nk_button_label(ctx, "Play"))
                playback_play();
            if (nk_button_label(ctx, "Pause"))
                playback_pause();
            if (nk_button_label(ctx, "Stop"))
                playback_stop();
            if (!st.has_track)
                nk_widget_disable_end(ctx);

            /* row 5: volume label + slider, disabled (Phase 6 wires it) */
            nk_layout_row_template_begin(ctx, ui_s(30.0f, ui_scale));
            nk_layout_row_template_push_static(ctx, ui_s(60.0f, ui_scale));
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            nk_label(ctx, volbuf, NK_TEXT_LEFT);
            nk_widget_disable_begin(ctx);
            nk_slider_float(ctx, 0.0f, &volume, 1.0f, 0.01f);
            nk_widget_disable_end(ctx);
        }
        nk_end(ctx);

        SDL_SetRenderDrawColor(ren, 24, 24, 28, 255);
        SDL_RenderClear(ren);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        SDL_RenderPresent(ren);
    }

    /* Join the decoder thread and close the device before tearing down
     * SDL/Nuklear; otherwise macOS complains about an open audio device. */
    playback_shutdown();
    nk_sdl_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
