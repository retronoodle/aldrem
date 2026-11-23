/* Aldrem — Phase 7: a playlist panel under the player. Drops, Open and
 * multi-select drops append to it, it survives a restart via autosave under the
 * XDG config dir, and it saves to / loads from .m3u. Auto-advance is on; the
 * player rows, global shortcuts, browser and decoders are otherwise unchanged.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL.h>

#define NK_IMPLEMENTATION
#include "nuklear_config.h"
#define NK_SDL_RENDERER_IMPLEMENTATION
#include "nuklear_sdl_renderer.h"

#include "file_browser.h"
#include "playback.h"
#include "playlist.h"
#include "theme.h"
#include "track.h"

#define UI_DENSITY 0.8f
#define UI_FONT_BASE 19.0f
#define APP_DISPLAY_NAME "Aldrem"

/* nk_sdl_handle_event feeds NK_BUTTON_DOUBLE for every left click with
 * `clicks > 1`, so a triple click would look like a second double click and
 * replay the row. Hand SDL only two-click downs to Nuklear and swallow the
 * rest: the app's only double-click consumer (playlist rows) then fires once
 * per double click. Clicks above 2 would reset SDL's counter, so they are
 * dropped entirely rather than forwarded as plain left presses.
 * `forward` holds the event as Nuklear should see it. */
static int
ui_handle_event(const SDL_Event *evt)
{
    SDL_Event forward = *evt;

    if (evt->type == SDL_MOUSEBUTTONDOWN &&
        evt->button.button == SDL_BUTTON_LEFT && evt->button.clicks > 1) {
        if (evt->button.clicks > 2)
            return 0;
        forward.button.clicks = 2;
    }
    return nk_sdl_handle_event(&forward);
}

/* The window icon is a BMP baked into the binary by the Makefile (objcopy of
 * build/icon-256.bmp, rendered from packaging/aldrem.svg). SDL_LoadBMP reads
 * BMP only, which keeps SDL_image out of the dependency list. Defined only on
 * platforms where the Makefile embeds it (not macOS, which uses the bundle). */
#ifdef HAVE_EMBED_ICON
extern const unsigned char aldrem_icon_start[];
extern const unsigned char aldrem_icon_end[];
#endif

/* Supplied by the Makefile from `git describe`; a bare compiler invocation
 * (or a checkout with no tags) still builds. */
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

/* SDL_HINT_APP_NAME is newer than some SDL2 builds; the literal value is
 * stable, so fall back to it rather than failing to compile. */
#ifndef SDL_HINT_APP_NAME
#define SDL_HINT_APP_NAME "SDL_APP_NAME"
#endif

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
ui_seek_bar(struct nk_context *ctx, float s, const struct MpTheme *th,
    float frac, int enabled, int *dragging, float *preview)
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
    bar_h = mp_s(4.0f, s);
    if (bar_h < 1.0f)
        bar_h = 1.0f;
    cy = b.y + b.h * 0.5f;

    nk_fill_rect(canvas, nk_rect(b.x, cy - bar_h * 0.5f, b.w, bar_h),
        0.0f, mp_theme_track(th));
    if (shown > 0.0f)
        nk_fill_rect(canvas, nk_rect(b.x, cy - bar_h * 0.5f, b.w * shown, bar_h),
            0.0f, mp_theme_accent(th));

    if (enabled) {
        handle_w = mp_s(12.0f, s);
        if (handle_w < 4.0f)
            handle_w = 4.0f;
        cx = b.x + b.w * shown - handle_w * 0.5f;
        if (cx < b.x)
            cx = b.x;
        if (cx + handle_w > b.x + b.w)
            cx = b.x + b.w - handle_w;
        nk_fill_rect(canvas,
            nk_rect(cx, cy - handle_w * 0.5f, handle_w, handle_w),
            mp_s(2.0f, s), mp_theme_text_primary(th));
    }

    return released;
}

/* Last path component; the playlist shows basenames, not full paths. */
static const char *
ui_base_name(const char *path)
{
    const char *slash;

    if (!path)
        return "";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* SDL frees evt.drop.file when the event is done, so each path is copied.
 * Drops arrive one event per file; this keeps them together for one batch. */
static int
ui_drop_push(char ***queue, int *count, int *cap, const char *path)
{
    char *copy;
    size_t n = strlen(path) + 1;

    if (*count >= *cap) {
        int next = *cap ? *cap * 2 : 8;
        char **grown = (char **)realloc(*queue, sizeof *grown * (size_t)next);

        if (!grown)
            return -1;
        *queue = grown;
        *cap = next;
    }
    copy = (char *)malloc(n);
    if (!copy)
        return -1;
    memcpy(copy, path, n);
    (*queue)[(*count)++] = copy;
    return 0;
}

static void
ui_drop_free_all(char **queue, int count)
{
    int i;

    for (i = 0; i < count; i++)
        free(queue[i]);
    free(queue);
}

/* Save-as prefill: the session's last list path, else $HOME/Music/playlist.m3u
 * when that directory exists, else $HOME/playlist.m3u. */
static void
ui_save_path_default(char *out, size_t outlen)
{
    const char *last = playlist_last_file();
    const char *home = getenv("HOME");
    char probe[4096];
    struct stat sb;

    if (last && last[0]) {
        snprintf(out, outlen, "%s", last);
        return;
    }
    if (!home || !home[0]) {
        snprintf(out, outlen, "playlist.m3u");
        return;
    }
    snprintf(probe, sizeof probe, "%s/Music", home);
    if (stat(probe, &sb) == 0 && S_ISDIR(sb.st_mode))
        snprintf(out, outlen, "%s/Music/playlist.m3u", home);
    else
        snprintf(out, outlen, "%s/playlist.m3u", home);
}

#define PL_ROW_H           24.0f

enum {
    PL_ACTION_NONE      = 0,
    PL_ACTION_OPEN_LIST = 1 << 0,
    PL_ACTION_SAVE_AS   = 1 << 1
};

/* The content column inside the root window: 720 pt wide at most, centred,
 * with a 24 pt margin either side that shrinks so a narrow window still fills
 * the column without clipping the dock. `frame` is the window's content
 * region, which already has padding applied. */
static struct nk_rect
ui_column(struct nk_rect frame, float scale)
{
    float margin = mp_s(24.0f, scale);
    float max_w = mp_s(720.0f, scale);
    float w;

    if (margin > frame.w * 0.5f)
        margin = frame.w * 0.5f;
    w = frame.w - 2.0f * margin;
    if (w < 0.0f)
        w = 0.0f;
    if (w > max_w)
        w = max_w;
    return nk_rect(frame.x + (frame.w - w) * 0.5f, frame.y, w, frame.h);
}

/* The whole playlist band: heading, toolbar, scrolling rows. Returns toolbar
 * actions whose consequences live in main() (browser/modal state). Remove,
 * Clear and double-click are applied after the row loop so the array that
 * nk_list_view indexed cannot change mid-draw.
 *
 * Deliberately one function: issue #3 will move this panel under a tab without
 * touching the layout maths. */
static int
ui_playlist_panel(Playlist *pl, struct nk_context *ctx, float scale,
    int disabled)
{
    char header[64];
    float row_h = mp_s(PL_ROW_H, scale);
    float bar_h = mp_s(28.0f, scale);
    int count = playlist_count(pl);
    int current = playlist_current(pl);
    int selected = playlist_selected(pl);
    int activate = -1;
    int want_remove = 0, want_clear = 0;
    int actions = PL_ACTION_NONE;

    if (count > 0)
        snprintf(header, sizeof header, "Playlist - %d track%s",
                 count, count == 1 ? "" : "s");
    else
        snprintf(header, sizeof header, "Playlist - empty");

    nk_layout_row_dynamic(ctx, bar_h, 1);
    nk_label(ctx, header, NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, bar_h, 4);
    if (disabled)
        nk_widget_disable_begin(ctx);
    if (nk_button_label(ctx, "Open list"))
        actions |= PL_ACTION_OPEN_LIST;
    if (nk_button_label(ctx, "Save as"))
        actions |= PL_ACTION_SAVE_AS;
    if (disabled)
        nk_widget_disable_end(ctx);

    if (disabled || selected < 0)
        nk_widget_disable_begin(ctx);
    if (nk_button_label(ctx, "Remove"))
        want_remove = 1;
    if (disabled || selected < 0)
        nk_widget_disable_end(ctx);

    if (disabled || count == 0)
        nk_widget_disable_begin(ctx);
    if (nk_button_label(ctx, "Clear"))
        want_clear = 1;
    if (disabled || count == 0)
        nk_widget_disable_end(ctx);

    if (count == 0) {
        nk_layout_row_dynamic(ctx, row_h, 1);
        nk_label(ctx, "empty - drop audio files here to add them",
                 NK_TEXT_LEFT);
    } else {
        struct nk_list_view view;
        struct nk_rect content = nk_window_get_content_region(ctx);
        struct nk_rect last = nk_layout_widget_bounds(ctx);
        float list_h = (content.y + content.h) - (last.y + last.h)
            - mp_s(4.0f, scale);

        if (list_h < 3.0f * row_h)
            list_h = 3.0f * row_h;
        nk_layout_row_dynamic(ctx, list_h, 1);

        if (disabled)
            nk_widget_disable_begin(ctx);
        if (nk_list_view_begin(ctx, &view, "tracks",
                               NK_WINDOW_BORDER, (int)row_h, count)) {
            int i;

            nk_layout_row_dynamic(ctx, row_h, 1);
            for (i = view.begin; i < view.end; i++) {
                const char *base = ui_base_name(playlist_path_at(pl, i));
                char text[4100];
                struct nk_rect bounds;
                nk_bool sel = (i == selected) ? nk_true : nk_false;

                /* ">" marks the playing row: the default Nuklear font has no
                 * glyph for a play triangle. */
                if (i == current)
                    snprintf(text, sizeof text, "> %s", base);
                else
                    snprintf(text, sizeof text, "%s", base);

                /* Read the row rect before drawing it: nk_widget_bounds peeks
                 * at the next slot, so asking after the label names the row
                 * below. Double click plays the row; NK_BUTTON_DOUBLE comes
                 * from the input pump, which forwards only two-click downs,
                 * so this fires once per double click. ROM mode already
                 * blocks input, but keep the guard so rows stay inert behind
                 * a modal. */
                bounds = nk_widget_bounds(ctx);
                if (!disabled &&
                    nk_input_mouse_clicked(&ctx->input, NK_BUTTON_DOUBLE,
                                           bounds))
                    activate = i;

                /* Single click selects a row. The return value is "was
                 * clicked", so it needs a local selection flag: the truth
                 * stays in the Playlist struct (see playlist_select). */
                if (nk_selectable_label(ctx, text, NK_TEXT_LEFT, &sel))
                    playlist_select(pl, i);
            }
            nk_list_view_end(&view);
        }
        if (disabled)
            nk_widget_disable_end(ctx);
    }

    /* Deferred mutations: the row loop above indexed this list this frame. */
    if (activate >= 0)
        playlist_play_index(pl, activate);
    if (want_remove)
        playlist_remove_selected(pl);
    if (want_clear)
        playlist_clear(pl);
    return actions;
}

/* Tab order slots, shared by the frame and the key handler so both agree on
 * which button Enter fires. Slots are fixed numbers rather than an array so a
 * button added later cannot silently renumber the others. */
enum {
    UI_TAB_NONE = 0,
    UI_TAB_PREV = 1,
    UI_TAB_PLAY,
    UI_TAB_STOP,
    UI_TAB_NEXT,
    UI_TAB_OPEN,
    UI_TAB_LAST
};

/* Per-button press/hover state plus the focus slot for the frame. One
 * instance lives in main() for the whole run: Nuklear widgets are
 * immediate-mode, so anything that must survive a frame is held here. */
struct UiButtons {
    struct MpAnim prev, play, stop, next, open;
    struct MpButtonOpts opt; /* focus slot and dt, refilled each frame */
    /* The focused slot; UI_TAB_NONE = nobody. Tab is the only focus key the
     * app consumes: Enter is left to Nuklear so edit fields and popups keep
     * theirs, which also keeps its keyboard counters at one tick per press. */
    int focus;
};

/* Time, seek, transport and volume, gathered inside the dock panel. Returns
 * the pending seek (seconds) when the seek bar was released, else 0; every
 * other action is applied here through the same calls as before. */
static double
ui_transport_panel(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const PlaybackStatus *st, Playlist *pl, int modal_active,
    const char *timebuf, float progress, const char *volbuf,
    int *seek_dragging, float *seek_preview, struct UiButtons *ub)
{
    double seek = 0.0;
    float volume = (float)st->volume_percent / 100.0f;
    int slider_live;

    nk_layout_row_dynamic(ctx, mp_s(35.0f, scale), 1);
    nk_label(ctx, timebuf, NK_TEXT_CENTERED);

    nk_layout_row_dynamic(ctx, mp_s(35.0f, scale), 1);
    if (!st->has_track) {
        nk_label(ctx, "drop a .wav, .mp3, .flac, .ogg, .aiff, .aif or .aifc here", NK_TEXT_CENTERED);
    } else if (ui_seek_bar(ctx, scale, th, progress, !modal_active,
                           seek_dragging, seek_preview)) {
        seek = (double)*seek_preview * st->duration;
    }

    /* Prev | [hero Play/Pause] | Stop | Next. Prev/Next work from list entries
     * alone, so they stay live with no loaded track; the hero and Stop need
     * one. The hero is one toggle: its glyph follows the state read at the
     * top of the frame, never a second query. */
    nk_layout_row_dynamic(ctx, mp_s(40.0f, scale), 4);
    if (mp_button(ctx, scale, th, NULL, MP_BTN_NEUTRAL, MP_ICON_PREV,
                  UI_TAB_PREV, playlist_count(pl) > 0 && !modal_active,
                  &ub->prev, &ub->opt))
        playlist_prev(pl);
    if (mp_button(ctx, scale, th, NULL, MP_BTN_HERO,
                  st->state == MP_PLAYING ? MP_ICON_PAUSE : MP_ICON_PLAY,
                  UI_TAB_PLAY, st->has_track && !modal_active, &ub->play,
                  &ub->opt))
        playback_toggle();
    if (mp_button(ctx, scale, th, NULL, MP_BTN_NEUTRAL, MP_ICON_STOP,
                  UI_TAB_STOP, st->has_track && !modal_active, &ub->stop,
                  &ub->opt))
        playback_stop();
    if (mp_button(ctx, scale, th, NULL, MP_BTN_NEUTRAL, MP_ICON_NEXT,
                  UI_TAB_NEXT, playlist_count(pl) > 0 && !modal_active,
                  &ub->next, &ub->opt))
        playlist_next(pl);

    /* Volume label + slider; live with or without a track, disabled while a
     * dialog is up or no audio device opened. */
    nk_layout_row_template_begin(ctx, mp_s(30.0f, scale));
    nk_layout_row_template_push_static(ctx, mp_s(60.0f, scale));
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    slider_live = !modal_active && playback_available();
    nk_label(ctx, volbuf, NK_TEXT_LEFT);
    if (!slider_live)
        nk_widget_disable_begin(ctx);
    if (nk_slider_float(ctx, 0.0f, &volume, 1.0f, 0.01f))
        playback_set_volume((int)lroundf(volume * 100.0f));
    if (!slider_live)
        nk_widget_disable_end(ctx);

    return seek;
}

/* Small centred Save-as dialog. Returns 1 while it should stay open. Enter (in
 * the field) and Save both write; a failure keeps the dialog and its error up.
 * Esc is handled in the event loop, before the browser/quit fallbacks. */
static int
ui_save_modal(struct nk_context *ctx, float scale, float screen_w,
    float screen_h, Playlist *pl, char *path, size_t pathlen,
    char *err, size_t errlen, int *focus)
{
    struct nk_rect bounds;
    float margin = mp_s(40.0f, scale);
    float w = mp_s(560.0f, scale);
    float h = mp_s(200.0f, scale);
    float row_h = mp_s(28.0f, scale);
    int keep = 1;

    if (w > screen_w - 2.0f * margin)
        w = screen_w - 2.0f * margin;
    if (h > screen_h - 2.0f * margin)
        h = screen_h - 2.0f * margin;
    if (w < 1.0f)
        w = screen_w;
    if (h < 1.0f)
        h = screen_h;
    bounds = nk_rect(lroundf((screen_w - w) * 0.5f),
                     lroundf((screen_h - h) * 0.5f),
                     lroundf(w), lroundf(h));

    if (nk_begin(ctx, "Save playlist", bounds,
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        nk_flags evt;
        int do_save = 0;

        nk_layout_row_dynamic(ctx, row_h, 1);
        nk_label(ctx, "Save the playlist as an .m3u file:", NK_TEXT_LEFT);

        if (*focus) {
            nk_edit_focus(ctx, 0);
            *focus = 0;
        }
        nk_layout_row_dynamic(ctx, row_h, 1);
        evt = nk_edit_string_zero_terminated(ctx,
            NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, path, (int)pathlen,
            nk_filter_default);
        if (evt & NK_EDIT_COMMITED)
            do_save = 1;

        if (err[0]) {
            nk_layout_row_dynamic(ctx, row_h, 1);
            nk_label(ctx, err, NK_TEXT_LEFT);
        }

        nk_layout_row_dynamic(ctx, row_h, 2);
        if (nk_button_label(ctx, "Save"))
            do_save = 1;
        if (nk_button_label(ctx, "Cancel"))
            keep = 0;

        if (do_save && playlist_save_as(pl, path, err, errlen) == 0)
            keep = 0;
    }
    nk_end(ctx);
    return keep;
}

int
main(int argc, char **argv)
{
    /* Answer --version before touching SDL: it needs no window, no audio and
     * no display, so it works in a bare CI container. */
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 ||
                     strcmp(argv[1], "-V") == 0)) {
        printf("%s %s\n", "aldrem", APP_VERSION); /* matches packaging/TARGET */
        return 0;
    }

    /* Name the app for volume mixers and screensaver inhibitors. Set before
     * the first SDL_Init so the audio backend picks it up on device open. */
    SDL_SetHint(SDL_HINT_APP_NAME, APP_DISPLAY_NAME);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    /* Audio is optional: without it the UI still runs with transport disabled. */
    if (SDL_Init(SDL_INIT_AUDIO) != 0)
        SDL_Log("SDL audio init failed: %s", SDL_GetError());

    SDL_Window *win = SDL_CreateWindow(APP_DISPLAY_NAME,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 900, 640,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* Set the window icon from the bundled BMP so the title bar and task
     * switcher show the app icon even when it is run outside a desktop
     * entry (e.g. `make run`). Failure is non-fatal: the UI is unaffected. */
#ifdef HAVE_EMBED_ICON
    SDL_RWops *icon_rw = SDL_RWFromConstMem(aldrem_icon_start,
        (int)(aldrem_icon_end - aldrem_icon_start));
    if (icon_rw) {
        SDL_Surface *icon = SDL_LoadBMP_RW(icon_rw, 1);
        if (icon) {
            SDL_SetWindowIcon(win, icon);
            SDL_FreeSurface(icon);
        } else {
            SDL_Log("icon load failed: %s", SDL_GetError());
        }
    }
#endif

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        /* Headless CI (SDL_VIDEODRIVER=dummy) and machines without a GL
         * driver have no accelerated renderer; the software one still runs
         * the full event loop. */
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
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
    struct MpTheme theme;

    mp_theme_init(&theme, ren);
    nk_style_default(ctx);
    mp_theme_apply(ctx, ui_scale, &theme);
    ui_select_font(ctx, ratio, font_1x, font_2x);
    float style_scale = ui_scale;

    if (playback_init() != 0)
        SDL_Log("playback_init failed: %s", SDL_GetError());

    FileBrowser *browser = file_browser_create();
    if (!browser)
        SDL_Log("file browser allocation failed; Open is disabled");

    /* Restores the autosaved queue (never plays) before anything is added. */
    Playlist *pl = playlist_create();
    if (!pl)
        SDL_Log("playlist allocation failed; the queue is disabled");

    if (argc > 1) {
        /* Appending is the load path now; it autoplays because the player is
         * idle, and the entry stays in the list. */
        if (!playlist_add(pl, argv[1]))
            SDL_Log("cannot add %s to the playlist", argv[1]);
    }

    int running = 1;
    int seek_dragging = 0;
    float seek_preview = 0.0f;
    double pending_seek = 0.0;
    char window_title[640] = "";
    char **drops = NULL;
    int drop_count = 0, drop_cap = 0;
    int browser_list_mode = 0;
    int modal_active = 0;
    int save_modal_open = 0;
    int save_focus = 0;
    char save_path[4096] = "";
    char save_err[256] = "";

    struct UiButtons buttons;
    memset(&buttons, 0, sizeof buttons);

    /* Phase 7 placeholder: a real per-frame dt replaces this clamped millisecond
     * delta, and the widgets' easing call sites stay the same. */
    Uint32 last_ticks = SDL_GetTicks();

    while (running) {
        /* Derive sizes fresh each frame: the resize event values are points,
         * while Nuklear lays out in physical pixels. */
        ratio = ui_get_pixel_size(win, ren, &px_w, &px_h);
        ui_scale = ratio * UI_DENSITY;
        if (ui_scale != style_scale) {
            nk_style_default(ctx);
            mp_theme_apply(ctx, ui_scale, &theme);
            ui_select_font(ctx, ratio, font_1x, font_2x);
            style_scale = ui_scale;
        }

        SDL_Event evt;
        nk_input_begin(ctx);
        while (SDL_PollEvent(&evt)) {
            int consumed = 0;

            /* Recomputed per event: Esc can close a modal mid-batch. */
            modal_active = save_modal_open || file_browser_is_open(browser);

            /* A click is a pointer gesture, so it drops keyboard focus: the
             * ring should not linger on a button the mouse did not pick. */
            if (evt.type == SDL_MOUSEBUTTONDOWN)
                buttons.focus = UI_TAB_NONE;

            if (evt.type == SDL_QUIT) {
                running = 0;
            } else if (evt.type == SDL_DROPFILE) {
                /* SDL owns the path string: copy each one, then free it. */
                if (evt.drop.file) {
                    if (ui_drop_push(&drops, &drop_count, &drop_cap,
                                     evt.drop.file) != 0)
                        SDL_Log("drop queue allocation failed; file skipped");
                    SDL_free(evt.drop.file);
                }
            } else if (evt.type == SDL_KEYDOWN && !evt.key.repeat &&
                       evt.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                /* Esc closes the newest modal first, then the browser, then
                 * the window. */
                if (save_modal_open) {
                    save_modal_open = 0;
                    SDL_StopTextInput();
                } else if (file_browser_is_open(browser))
                    file_browser_close(browser);
                else
                    running = 0;
            } else if (!modal_active && evt.type == SDL_KEYDOWN &&
                       !evt.key.repeat &&
                       evt.key.keysym.scancode == SDL_SCANCODE_TAB) {
                /* Tab walks the converted buttons in reading order and wraps.
                 * Consumed so Nuklear's edit fields never see it; the modal
                 * gate above keeps Tab in text entry while a dialog is up. */
                consumed = 1;
                buttons.focus++;
                if (buttons.focus >= UI_TAB_LAST)
                    buttons.focus = UI_TAB_PREV;
            } else if (!modal_active && evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_LEFT) {
                pending_seek -= 5.0;
            } else if (!modal_active && evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_RIGHT) {
                pending_seek += 5.0;
            } else if (!modal_active && evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_SPACE) {
                /* Space/up/down are global: consumed so Nuklear never sees
                 * them. Space toggles once per press; up/down repeat so a held
                 * key ramps the volume. A text field needs these keys, so the
                 * modal gate above skips the whole block. */
                consumed = 1;
                if (!evt.key.repeat)
                    playback_toggle();
            } else if (!modal_active && evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_UP) {
                consumed = 1;
                playback_adjust_volume(5);
            } else if (!modal_active && evt.type == SDL_KEYDOWN &&
                       evt.key.keysym.scancode == SDL_SCANCODE_DOWN) {
                consumed = 1;
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
                ui_handle_event(&evt);
        }
        nk_input_end(ctx);

        /* A drop supersedes the browser and appends in arrival order. */
        if (drop_count > 0) {
            int i;

            file_browser_close(browser);
            for (i = 0; i < drop_count; i++)
                playlist_add(pl, drops[i]);
            ui_drop_free_all(drops, drop_count);
            drops = NULL;
            drop_count = 0;
            drop_cap = 0;
        }

        PlaybackStatus st;
        char timebuf[64];
        char volbuf[32];
        char title[640];
        char statusbuf[768];
        char chbuf[16];
        const char *base;
        float progress;
        double shown;
        int browser_open;
        int panel_actions = PL_ACTION_NONE;
        int dock_open;

        browser_open = file_browser_is_open(browser);
        modal_active = save_modal_open || browser_open;

        /* One dt per frame for the hover/press easing, clamped so a stalled
         * window (a drag, a resize) cannot jump an animation to its end. */
        {
            Uint32 now = SDL_GetTicks();
            Uint32 span = now - last_ticks;

            last_ticks = now;
            if (span > 100)
                span = 100;
            buttons.opt.dt = (float)span / 1000.0f;
        }
        buttons.opt.focus_slot = buttons.focus;

        /* Coalesce arrow-key repeats into one decoder rebuild per frame; this
         * runs before the status fetch so the label reflects the new position
         * immediately. */
        if (pending_seek != 0.0) {
            playback_seek_relative(pending_seek);
            pending_seek = 0.0;
        }

        playback_get_status(&st);

        /* EOF is consumed by this read; advance before the label is built so
         * the frame shows the next track rather than the finished one. */
        if (st.finished) {
            playlist_track_finished(pl);
            playback_get_status(&st);
        }

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
            snprintf(title, sizeof title, "%s — %s", base, APP_DISPLAY_NAME);
        else
            snprintf(title, sizeof title, "%s", APP_DISPLAY_NAME);
        if (strcmp(title, window_title) != 0) {
            SDL_SetWindowTitle(win, title);
            snprintf(window_title, sizeof window_title, "%s", title);
        }

        /* The root window is the frame itself, so hide its fill here (not in
         * the shared style) to let the backdrop show through. Panels and
         * dialogs drawn inside keep their surface. */
        nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                                 nk_style_item_hide());
        if (nk_begin(ctx, "aldrem",
                     nk_rect(0, 0, (float)px_w, (float)px_h),
                     NK_WINDOW_NO_SCROLLBAR))
        {
            /* Everything lays out inside the centred column; the window and
             * the backdrop still cover the whole frame. */
            struct nk_rect col = ui_column(nk_window_get_content_region(ctx),
                                           ui_scale);

            /* header: Open button + status; inert while a dialog is up */
            nk_layout_row_template_begin(ctx, mp_s(34.0f, ui_scale));
            nk_layout_row_template_push_static(ctx, mp_s(90.0f, ui_scale));
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_end(ctx);
            if (mp_button(ctx, ui_scale, &theme, "Open", MP_BTN_PRIMARY,
                          MP_ICON_PLAY, UI_TAB_OPEN,
                          browser && !modal_active, &buttons.open,
                          &buttons.opt)) {
                browser_list_mode = 0;
                file_browser_open(browser, st.has_track ? st.name : NULL);
            }
            nk_label(ctx, statusbuf, NK_TEXT_LEFT);

            /* The dock holds time, seek, transport and volume; its content
             * height is the four rows plus the three 4 pt gaps between them.
             * The rows only draw when the group opened, and mp_dock_end must
             * follow only when it did: a skipped begin never opened the group
             * or pushed the group styles, so an unconditional end would pop
             * an empty stack and close a group that was never begun. */
            dock_open = mp_dock_begin(ctx, ui_scale, col,
                mp_s(140.0f, ui_scale) + 3.0f * mp_s(4.0f, ui_scale),
                &theme);
            if (dock_open) {
                double seek = ui_transport_panel(ctx, ui_scale, &theme, &st,
                                                 pl, modal_active, timebuf,
                                                 progress, volbuf,
                                                 &seek_dragging,
                                                 &seek_preview, &buttons);

                if (seek != 0.0)
                    playback_seek(seek);
            }

            /* The playlist band sits below the dock, in the same column, and
             * takes whatever height is left. Only touched once the dock
             * closed: mp_dock_end restores the shared style before it draws. */
            if (dock_open)
                mp_dock_end(ctx);
            nk_layout_row_dynamic(ctx, mp_s(4.0f, ui_scale), 1);
            panel_actions = ui_playlist_panel(pl, ctx, ui_scale, modal_active);
        }
        nk_end(ctx);
        nk_style_pop_style_item(ctx);

        /* Toolbar consequences run after the main window closes so dialogs are
         * never opened from inside another window's draw. */
        if (panel_actions & PL_ACTION_OPEN_LIST) {
            browser_list_mode = 1;
            file_browser_open_for(browser, st.has_track ? st.name : NULL,
                                  playlist_ext_supported, "Open playlist");
        }
        if (panel_actions & PL_ACTION_SAVE_AS) {
            ui_save_path_default(save_path, sizeof save_path);
            save_err[0] = '\0';
            save_focus = 1;
            save_modal_open = 1;
            SDL_StartTextInput();
        }

        /* Drawn as a second window after the main layout, which is what makes
         * "disable the main controls while open" sufficient to stop clicks
         * leaking through to the seek bar and transport. */
        if (browser_open && browser) {
            char chosen[4096];

            if (file_browser_draw(browser, ctx, ui_scale, NULL, (float)px_w,
                                  (float)px_h, chosen, sizeof chosen) == 1) {
                if (browser_list_mode) {
                    char err[256];

                    /* Open list replaces the queue and plays nothing. */
                    if (playlist_load_file(pl, chosen, err, sizeof err) != 0)
                        SDL_Log("playlist_load_file failed: %s", err);
                } else {
                    playlist_add(pl, chosen);
                }
            }
        }

        /* Same layering rule for the Save-as dialog: it is drawn last, above
         * the main window and the playlist. */
        if (save_modal_open) {
            if (!ui_save_modal(ctx, ui_scale, (float)px_w, (float)px_h, pl,
                               save_path, sizeof save_path,
                               save_err, sizeof save_err, &save_focus)) {
                save_modal_open = 0;
                SDL_StopTextInput();
            }
        }

        mp_theme_backdrop(ren, px_w, px_h, &theme);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        SDL_RenderPresent(ren);
    }

    /* Final autosave before the engine and SDL go away; the playlist only
     * touches playback_stop/load, both of which need a live engine. */
    ui_drop_free_all(drops, drop_count);
    playlist_destroy(pl);

    /* Join the decoder thread and close the device before tearing down
     * SDL/Nuklear; otherwise macOS complains about an open audio device. */
    file_browser_destroy(browser);
    playback_shutdown();
    nk_sdl_shutdown();
    mp_theme_shutdown(&theme);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
