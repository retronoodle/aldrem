/* mediaplayer — Phase 2: WAV playback engine wired to the Phase 1 layout.
 * A 900x520 window with Open (inert) + status, time label, read-only progress
 * slider, transport buttons and a disabled volume slider.
 * Esc quits (raw SDL event, matching the Phase 6 pattern).
 */
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
        SDL_WINDOW_RESIZABLE);
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
    struct nk_font *font;
    nk_sdl_font_stash_begin(&atlas);
    font = nk_font_atlas_add_default(atlas, 19.0f, 0);
    nk_sdl_font_stash_end();
    nk_style_set_font(ctx, &font->handle);

    if (playback_init() != 0)
        SDL_Log("playback_init failed: %s", SDL_GetError());

    if (argc > 1) {
        char err[256];
        if (playback_load(argv[1], err, sizeof err) != 0)
            SDL_Log("playback_load failed: %s", err);
    }

    int win_w = 900, win_h = 520;
    int running = 1;
    while (running) {
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
                        evt.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
                win_w = evt.window.data1;
                win_h = evt.window.data2;
            }
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
                     nk_rect(0, 0, (float)win_w, (float)win_h),
                     NK_WINDOW_NO_SCROLLBAR))
        {
            /* row 1: Open button (fixed; file dialog is a later phase) + status */
            nk_layout_row_template_begin(ctx, 30);
            nk_layout_row_template_push_static(ctx, 90);
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            nk_button_label(ctx, "Open");
            nk_label(ctx, status_text, NK_TEXT_LEFT);

            /* row 2: time label, centered */
            nk_layout_row_dynamic(ctx, 35, 1);
            nk_label(ctx, timebuf, NK_TEXT_CENTERED);

            /* row 3: progress slider — tracks the playhead, non-draggable */
            nk_layout_row_dynamic(ctx, 35, 1);
            nk_widget_disable_begin(ctx);
            nk_slider_float(ctx, 0.0f, &progress, 1.0f, 0.01f);
            nk_widget_disable_end(ctx);

            /* row 4: transport buttons, disabled until a track is loaded */
            nk_layout_row_dynamic(ctx, 35, 3);
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
            nk_layout_row_template_begin(ctx, 30);
            nk_layout_row_template_push_static(ctx, 60);
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
