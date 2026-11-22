/* mediaplayer — Phase 1: SDL window + Nuklear shell
 * A 900x520 window with the real GUI layout, all controls clickable but inert.
 * Esc quits (raw SDL event, matching the Phase 6 pattern).
 */
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

int
main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

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

    int win_w = 900, win_h = 520;
    float progress = 0.0f, volume = 0.0f;
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

        if (nk_begin(ctx, "mediaplayer",
                     nk_rect(0, 0, (float)win_w, (float)win_h),
                     NK_WINDOW_NO_SCROLLBAR))
        {
            /* row 1: Open button (fixed) + status label (fills the rest) */
            nk_layout_row_template_begin(ctx, 30);
            nk_layout_row_template_push_static(ctx, 90);
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            nk_button_label(ctx, "Open");
            nk_label(ctx, "no file loaded", NK_TEXT_LEFT);

            /* row 2: time label, centered */
            nk_layout_row_dynamic(ctx, 35, 1);
            nk_label(ctx, "--:-- / --:--", NK_TEXT_CENTERED);

            /* row 3: progress slider, disabled */
            nk_layout_row_dynamic(ctx, 35, 1);
            nk_widget_disable_begin(ctx);
            nk_slider_float(ctx, 0.0f, &progress, 1.0f, 0.01f);
            nk_widget_disable_end(ctx);

            /* row 4: transport buttons (clickable, inert) */
            nk_layout_row_dynamic(ctx, 35, 3);
            nk_button_label(ctx, "Play");
            nk_button_label(ctx, "Pause");
            nk_button_label(ctx, "Stop");

            /* row 5: volume label + slider, disabled */
            nk_layout_row_template_begin(ctx, 30);
            nk_layout_row_template_push_static(ctx, 60);
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            nk_label(ctx, "vol 0", NK_TEXT_LEFT);
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

    nk_sdl_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
