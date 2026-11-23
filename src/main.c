/* mediaplayer — Phase 6: volume control, global Space/up/down shortcuts, an
 * idle hint in the seek row, and a status label that surfaces the decoded
 * format. Drag-and-drop, the in-app browser, WAV/MP3/FLAC playback, seeking
 * (arrows) and Esc (close browser or quit) from earlier phases are unchanged.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#define NK_IMPLEMENTATION
#include "nuklear_config.h"
#define NK_SDL_RENDERER_IMPLEMENTATION
#include "nuklear_sdl_renderer.h"

#include "file_browser.h"
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

/* One row-sized seek bar. `frac` is 0..1. While dragging, writes the preview
 * fraction and keeps it live; on mouse release returns 1 (caller seeks).
 * Nuklear input is already in physical pixels, matching the layout rect. */
static int
ui_seek_bar(struct nk_context *ctx, float s, float frac,
    int enabled, int *dragging, float *preview)
{
    struct nk_command_buffer *canvas;
    struct nk_rect b;
    float shown, bar_h, cy, cx, handle_w;
    int released = 0;

    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID)
        return 0;

    /* nk_input_mouse_clicked fires on release, so start the drag on press:
     * that keeps the preview live and ignores releases that began elsewhere. */
    if (enabled && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
        nk_input_is_mouse_hovering_rect(&ctx->input, b)) {
        *dragging = 1;
        *preview = (ctx->input.mouse.pos.x - b.x) / b.w;
    } else if (*dragging) {
        if (!enabled) {
            *dragging = 0;
        } else if (nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
            *preview = (ctx->input.mouse.pos.x - b.x) / b.w;
        } else {
            *dragging = 0;
            released = 1;
        }
    }

    if (*preview < 0.0f)
        *preview = 0.0f;
    if (*preview > 1.0f)
        *preview = 1.0f;

    shown = enabled ? (*dragging ? *preview : frac) : 0.0f;
    if (shown < 0.0f)
        shown = 0.0f;
    if (shown > 1.0f)
        shown = 1.0f;

    canvas = nk_window_get_canvas(ctx);
    bar_h = ui_s(4.0f, s);
    if (bar_h < 1.0f)
        bar_h = 1.0f;
    cy = b.y + b.h * 0.5f;

    nk_fill_rect(canvas, nk_rect(b.x, cy - bar_h * 0.5f, b.w, bar_h),
        0.0f, nk_rgb(60, 60, 68));
    if (shown > 0.0f)
        nk_fill_rect(canvas, nk_rect(b.x, cy - bar_h * 0.5f, b.w * shown, bar_h),
            0.0f, nk_rgb(110, 150, 230));

    if (enabled) {
        handle_w = ui_s(12.0f, s);
        if (handle_w < 4.0f)
            handle_w = 4.0f;
        cx = b.x + b.w * shown - handle_w * 0.5f;
        if (cx < b.x)
            cx = b.x;
        if (cx + handle_w > b.x + b.w)
            cx = b.x + b.w - handle_w;
        nk_fill_rect(canvas,
            nk_rect(cx, cy - handle_w * 0.5f, handle_w, handle_w),
            ui_s(2.0f, s), nk_rgb(220, 225, 235));
    }

    return released;
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

    FileBrowser *browser = file_browser_create();
    if (!browser)
        SDL_Log("file browser allocation failed; Open is disabled");

    if (argc > 1) {
        char err[256];
        if (playback_load(argv[1], err, sizeof err) != 0)
            SDL_Log("playback_load failed: %s", err);
    }

    int running = 1;
    int seek_dragging = 0;
    float seek_preview = 0.0f;
    double pending_seek = 0.0;
    char pending_drop[4096];
    int has_pending_drop = 0;
    char window_title[640] = "";

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
            int consumed = 0;

            if (evt.type == SDL_QUIT) {
                running = 0;
            } else if (evt.type == SDL_DROPFILE) {
                /* SDL owns the path string: free every one, including the
                 * extras when a multi-file selection arrives in one batch. */
                if (evt.drop.file) {
                    if (!has_pending_drop) {
                        snprintf(pending_drop, sizeof pending_drop, "%s",
                                 evt.drop.file);
                        has_pending_drop = 1;
                    }
                    SDL_free(evt.drop.file);
                }
            } else if (evt.type == SDL_KEYDOWN && !evt.key.repeat &&
                       evt.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                if (file_browser_is_open(browser))
                    file_browser_close(browser);
                else
                    running = 0;
            } else if (evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_LEFT) {
                if (!file_browser_is_open(browser))
                    pending_seek -= 5.0;
            } else if (evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_RIGHT) {
                if (!file_browser_is_open(browser))
                    pending_seek += 5.0;
            } else if (evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_SPACE) {
                /* Space/up/down are global: consumed so Nuklear never sees
                 * them. Space toggles once per press; up/down repeat so a held
                 * key ramps the volume. */
                consumed = 1;
                if (!evt.key.repeat && !file_browser_is_open(browser))
                    playback_toggle();
            } else if (evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_UP) {
                consumed = 1;
                if (!file_browser_is_open(browser))
                    playback_adjust_volume(5);
            } else if (evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_DOWN) {
                consumed = 1;
                if (!file_browser_is_open(browser))
                    playback_adjust_volume(-5);
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
            if (!consumed)
                nk_sdl_handle_event(&evt);
        }
        nk_input_end(ctx);

        /* A drop supersedes the browser; both funnel into playback_load. */
        if (has_pending_drop) {
            char err[256];

            has_pending_drop = 0;
            file_browser_close(browser);
            if (playback_load(pending_drop, err, sizeof err) != 0)
                SDL_Log("playback_load failed: %s", err);
        }

        PlaybackStatus st;
        char timebuf[64];
        char volbuf[32];
        char title[640];
        char statusbuf[768];
        char chbuf[16];
        const char *base;
        float progress, volume;
        double shown;
        int browser_open;
        int slider_live;

        browser_open = file_browser_is_open(browser);

        /* Coalesce arrow-key repeats into one decoder rebuild per frame; this
         * runs before the status fetch so the label reflects the new position
         * immediately. */
        if (pending_seek != 0.0) {
            playback_seek_relative(pending_seek);
            pending_seek = 0.0;
        }

        playback_get_status(&st);

        /* A Stop or failed load while dragging must not leave a stale preview. */
        if (!st.has_track)
            seek_dragging = 0;

        /* While dragging the label previews the target. The label is laid out
         * before the bar in the same frame, so the preview is one frame old at
         * most, which is imperceptible. */
        shown = seek_dragging && st.duration > 0.0
            ? (double)seek_preview * st.duration
            : st.position;

        if (st.has_track) {
            int cur = (int)shown;
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
        snprintf(volbuf, sizeof volbuf, "vol %d%%", st.volume_percent);

        base = "";
        if (st.has_track) {
            const char *slash = strrchr(st.name, '/');
            base = slash ? slash + 1 : st.name;
        }

        if (st.error[0]) {
            snprintf(statusbuf, sizeof statusbuf, "%s", st.error);
        } else if (st.has_track) {
            const char *codec = st.codec[0] ? st.codec : "audio";

            if (st.channels == 1)
                snprintf(chbuf, sizeof chbuf, "mono");
            else if (st.channels == 2)
                snprintf(chbuf, sizeof chbuf, "stereo");
            else
                snprintf(chbuf, sizeof chbuf, "%d ch", st.channels);

            if (st.bits_per_sample > 0)
                snprintf(statusbuf, sizeof statusbuf,
                         "%s  |  %s %d-bit %d Hz %s",
                         base, codec, st.bits_per_sample, st.sample_rate, chbuf);
            else
                snprintf(statusbuf, sizeof statusbuf, "%s  |  %s %d Hz %s",
                         base, codec, st.sample_rate, chbuf);
        } else {
            snprintf(statusbuf, sizeof statusbuf, "no file loaded");
        }

        /* The OS renders the title, so the em dash is safe here (the default
         * Nuklear font has no glyph for it). Only update on change. */
        if (base[0])
            snprintf(title, sizeof title, "%s — mediaplayer", base);
        else
            snprintf(title, sizeof title, "mediaplayer");
        if (strcmp(title, window_title) != 0) {
            SDL_SetWindowTitle(win, title);
            snprintf(window_title, sizeof window_title, "%s", title);
        }

        if (nk_begin(ctx, "mediaplayer",
                     nk_rect(0, 0, (float)px_w, (float)px_h),
                     NK_WINDOW_NO_SCROLLBAR))
        {
            /* row 1: Open button + status; inert while the browser is up */
            nk_layout_row_template_begin(ctx, ui_s(30.0f, ui_scale));
            nk_layout_row_template_push_static(ctx, ui_s(90.0f, ui_scale));
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            if (!browser || browser_open)
                nk_widget_disable_begin(ctx);
            if (nk_button_label(ctx, "Open"))
                file_browser_open(browser, st.has_track ? st.name : NULL);
            if (!browser || browser_open)
                nk_widget_disable_end(ctx);
            nk_label(ctx, statusbuf, NK_TEXT_LEFT);

            /* row 2: time label, centered */
            nk_layout_row_dynamic(ctx, ui_s(35.0f, ui_scale), 1);
            nk_label(ctx, timebuf, NK_TEXT_CENTERED);

            /* row 3: seek bar — click or drag to seek, released on mouse up */
            nk_layout_row_dynamic(ctx, ui_s(35.0f, ui_scale), 1);
            if (!st.has_track) {
                nk_label(ctx, "drop a .wav, .mp3 or .flac here", NK_TEXT_CENTERED);
            } else if (ui_seek_bar(ctx, ui_scale, progress,
                                   !browser_open,
                                   &seek_dragging, &seek_preview)) {
                playback_seek((double)seek_preview * st.duration);
            }

            /* row 4: transport buttons, disabled with no track or a dialog */
            nk_layout_row_dynamic(ctx, ui_s(35.0f, ui_scale), 3);
            if (!st.has_track || browser_open)
                nk_widget_disable_begin(ctx);
            if (nk_button_label(ctx, "Play"))
                playback_play();
            if (nk_button_label(ctx, "Pause"))
                playback_pause();
            if (nk_button_label(ctx, "Stop"))
                playback_stop();
            if (!st.has_track || browser_open)
                nk_widget_disable_end(ctx);

            /* row 5: volume label + slider; live with or without a track,
             * disabled while the browser is modal or no audio device opened */
            nk_layout_row_template_begin(ctx, ui_s(30.0f, ui_scale));
            nk_layout_row_template_push_static(ctx, ui_s(60.0f, ui_scale));
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            slider_live = !browser_open && playback_available();
            nk_label(ctx, volbuf, NK_TEXT_LEFT);
            if (!slider_live)
                nk_widget_disable_begin(ctx);
            if (nk_slider_float(ctx, 0.0f, &volume, 1.0f, 0.01f))
                playback_set_volume((int)lroundf(volume * 100.0f));
            if (!slider_live)
                nk_widget_disable_end(ctx);
        }
        nk_end(ctx);

        /* Drawn as a second window after the main layout, which is what makes
         * "disable the main controls while open" sufficient to stop clicks
         * leaking through to the seek bar and transport. */
        if (browser_open && browser) {
            char chosen[4096];
            char err[256];

            if (file_browser_draw(browser, ctx, ui_scale, (float)px_w,
                                  (float)px_h, chosen, sizeof chosen) == 1) {
                if (playback_load(chosen, err, sizeof err) != 0)
                    SDL_Log("playback_load failed: %s", err);
            }
        }

        SDL_SetRenderDrawColor(ren, 24, 24, 28, 255);
        SDL_RenderClear(ren);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        SDL_RenderPresent(ren);
    }

    /* Join the decoder thread and close the device before tearing down
     * SDL/Nuklear; otherwise macOS complains about an open audio device. */
    file_browser_destroy(browser);
    playback_shutdown();
    nk_sdl_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
