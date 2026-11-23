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
#include "finder.h"
#include "net.h"
#include "playback.h"
#include "playlist.h"
#include "radio.h"
#include "radio_search.h"
#include "settings.h"
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

/* Row label: the basename without its extension. Paths stay untouched; this is
 * display only. A leading dot is part of the name, not an extension. */
static void
ui_track_label(const char *path, char *out, size_t outlen)
{
    const char *base = ui_base_name(path);
    const char *dot = strrchr(base, '.');
    size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);

    if (n >= outlen)
        n = outlen - 1;
    memcpy(out, base, n);
    out[n] = '\0';
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
    PL_ACTION_SAVE_AS   = 1 << 1,
    PL_ACTION_FIND      = 1 << 2,
    PL_ACTION_RADIO     = 1 << 3
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
/* Extensions shown as chips in the empty playlist. */
static const char *const ui_ext_chips[] = {
    ".wav", ".mp3", ".flac", ".ogg", ".aiff", ".aif", ".aifc"
};

/* Height left under the playlist buttons, for the list or the empty state. */
static float
ui_list_height(struct nk_context *ctx, float scale, float row_h)
{
    struct nk_rect content = nk_window_get_content_region(ctx);
    struct nk_rect last = nk_layout_widget_bounds(ctx);
    float list_h = (content.y + content.h) - (last.y + last.h)
        - mp_s(4.0f, scale);

    if (list_h < 3.0f * row_h)
        list_h = 3.0f * row_h;
    return list_h;
}

static int
ui_playlist_panel(Playlist *pl, struct nk_context *ctx, float scale,
    int disabled, int find_ok, int radio_ok, const struct MpTheme *th,
    int has_track)
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

    nk_layout_row_dynamic(ctx, bar_h, radio_ok ? 6 : 5);
    if (disabled)
        nk_widget_disable_begin(ctx);
    if (nk_button_label(ctx, "Open list"))
        actions |= PL_ACTION_OPEN_LIST;
    if (!disabled && !find_ok)
        nk_widget_disable_begin(ctx);
    if (nk_button_label(ctx, "Find audio"))
        actions |= PL_ACTION_FIND;
    if (!disabled && !find_ok)
        nk_widget_disable_end(ctx);
    if (radio_ok && nk_button_label(ctx, "Radio"))
        actions |= PL_ACTION_RADIO;
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
        /* With nothing loaded the band is the drop target: draw the empty
         * state. With a track playing, a single-line hint is enough. */
        if (has_track) {
            nk_layout_row_dynamic(ctx, row_h, 1);
            mp_label_muted(ctx, th, "empty - drop audio files here to add them",
                           NK_TEXT_LEFT);
        } else {
            nk_layout_row_dynamic(ctx, ui_list_height(ctx, scale, row_h), 1);
            mp_empty_state(ctx, scale, th, "Drop a track to begin",
                           ui_ext_chips,
                           (int)(sizeof ui_ext_chips / sizeof ui_ext_chips[0]));
        }
    } else {
        struct nk_list_view view;
        float list_h = ui_list_height(ctx, scale, row_h);

        nk_layout_row_dynamic(ctx, list_h, 1);

        if (disabled)
            nk_widget_disable_begin(ctx);
        if (nk_list_view_begin(ctx, &view, "tracks",
                               NK_WINDOW_BORDER, (int)row_h, count)) {
            int i;

            nk_layout_row_dynamic(ctx, row_h, 1);
            for (i = view.begin; i < view.end; i++) {
                char text[512];
                struct nk_rect bounds;
                nk_bool sel = (i == selected) ? nk_true : nk_false;

                if (playlist_label_at(pl, i))
                    snprintf(text, sizeof text, "%s", playlist_label_at(pl, i));
                else
                    ui_track_label(playlist_path_at(pl, i), text, sizeof text);

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

                /* The playing row gets a cyan tint and a left bar, drawn after
                 * the label on top of it. The tint is translucent so the text
                 * stays readable. */
                if (i == current) {
                    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);

                    nk_fill_rect(canvas, bounds, 0.0f,
                                 mp_col_alpha(th->accent_a, 26));
                    nk_fill_rect(canvas,
                                 nk_rect(bounds.x, bounds.y,
                                         mp_s(3.0f, scale), bounds.h),
                                 0.0f, th->accent_a);
                }
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
    UI_TAB_OPEN = 1,
    UI_TAB_VU,
    UI_TAB_PREV,
    UI_TAB_PLAY,
    UI_TAB_STOP,
    UI_TAB_NEXT,
    UI_TAB_LAST
};

/* Per-button press/hover state plus the focus slot for the frame. One
 * instance lives in main() for the whole run: Nuklear widgets are
 * immediate-mode, so anything that must survive a frame is held here. */
struct UiButtons {
    struct MpAnim prev, play, stop, next, open;
    /* Seek and volume easing, alongside the buttons: same lifetime, same
     * "survives a frame" rule. */
    struct MpSeekAnim seek;
    struct MpVolAnim vol;
    /* Meter style toggle and the meter's needle/peak ballistics. */
    struct MpAnim vu;
    struct MpVuAnim meter;
    struct MpButtonOpts opt; /* focus slot and dt, refilled each frame */
    /* The focused slot; UI_TAB_NONE = nobody. Tab is the only focus key the
     * app consumes: Enter is left to Nuklear so edit fields and popups keep
     * theirs, which also keeps its keyboard counters at one tick per press. */
    int focus;
};

/* Level meter pinned to the top-right corner of the frame. It is its own
 * window so it ignores the centred column; the header row reserves the same
 * width on its right (UI_VU_RESERVE) so the status text stops short of it. */
#define UI_VU_W        240.0f /* meter plus toggle, logical px */
#define UI_VU_METER_W 160.0f
#define UI_VU_H        46.0f  /* matches the header row it sits in */
#define UI_VU_MARGIN   16.0f
#define UI_VU_RESERVE (UI_VU_W + UI_VU_MARGIN)

static void
ui_vu_overlay(struct nk_context *ctx, float scale, struct MpTheme *th,
    float frame_w, float top_y, MeterStyle *style, struct UiButtons *ub,
    const PlaybackStatus *st, int enabled)
{
    float w = mp_s(UI_VU_W, scale), h = mp_s(UI_VU_H, scale);
    float margin = mp_s(UI_VU_MARGIN, scale);
    struct nk_rect r = nk_rect(frame_w - w - margin, top_y, w, h);
    struct MpVuLevels lv;
    int needle = *style == METER_NEEDLE;

    lv.peak_l = st->level_peak_l;
    lv.peak_r = st->level_peak_r;
    lv.rms_l = st->level_rms_l;
    lv.rms_r = st->level_rms_r;

    /* Transparent, unpadded window: only the meter and toggle show. Pushes
     * are popped after nk_end whether or not the window opened. */
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
                             nk_style_item_color(nk_rgba(0, 0, 0, 0)));
    nk_style_push_vec2(ctx, &ctx->style.window.padding, nk_vec2(0.0f, 0.0f));
    nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(0.0f, 0.0f));

    if (nk_begin(ctx, "vu", r, NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_template_begin(ctx, h);
        nk_layout_row_template_push_static(ctx, mp_s(UI_VU_METER_W, scale));
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_end(ctx);

        mp_vu_meter(ctx, scale, th, needle, &lv, ub->opt.dt, &ub->meter);
        if (mp_button(ctx, scale, th, needle ? "Needle" : "LED",
                      MP_BTN_NEUTRAL, MP_ICON_PLAY, UI_TAB_VU, enabled,
                      &ub->vu, &ub->opt)) {
            *style = needle ? METER_LED : METER_NEEDLE;
            if (settings_set_meter_style(*style) != 0)
                SDL_Log("settings: cannot save meter style");
        }
    }
    nk_end(ctx);

    nk_style_pop_vec2(ctx);
    nk_style_pop_vec2(ctx);
    nk_style_pop_style_item(ctx);
}

/* Time, seek, transport and volume, gathered inside the dock panel. Returns 1
 * when the seek bar was released, with the target (seconds) in `*seek_out`;
 * every other action is applied here through the same calls as before. */
static int
ui_transport_panel(struct nk_context *ctx, float scale, const struct MpTheme *th,
    const PlaybackStatus *st, Playlist *pl, int modal_active,
    const char *curbuf, const char *totbuf, float progress,
    const char *volbuf,
    int *seek_dragging, float *seek_preview, struct UiButtons *ub,
    double *seek_out)
{
    int released = 0;
    float volume = (float)st->volume_percent / 100.0f;
    int slider_live;

    /* The time row shows --:-- when nothing is loaded. The seek bar stays on
     * screen, disabled, so the dock keeps its shape. */
    nk_layout_row_dynamic(ctx, mp_s(35.0f, scale), 1);
    mp_time_label(ctx, th, curbuf, totbuf);

    nk_layout_row_dynamic(ctx, mp_s(35.0f, scale), 1);
    if (!st->has_track) {
        mp_label_muted(ctx, th, "no track loaded", NK_TEXT_CENTERED);
    } else if (mp_seek_bar(ctx, scale, th, progress,
                           !modal_active && !st->is_live,
                           seek_dragging, seek_preview, st->duration,
                           ub->opt.dt, &ub->seek)) {
        *seek_out = (double)*seek_preview * st->duration;
        released = 1;
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

    /* Volume slider (speaker glyph leads it) with the percentage dimmed on the
     * right; live with or without a track, disabled while a dialog is up or no
     * audio device opened. The slider takes `enabled` rather than the window
     * disable flag: nk_widget_disable_begin would set that flag for the whole
     * dock, which would take the seek bar and the transport with it. */
    nk_layout_row_template_begin(ctx, mp_s(30.0f, scale));
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, mp_s(48.0f, scale));
    nk_layout_row_template_end(ctx);
    slider_live = !modal_active && playback_available();
    if (mp_volume_slider(ctx, scale, th, &volume, slider_live, ub->opt.dt,
                         &ub->vol))
        playback_set_volume((int)lroundf(volume * 100.0f));
    nk_label_colored(ctx, volbuf, NK_TEXT_RIGHT, th->text_secondary);

    return released;
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

/* Adds every checked row, or every row when all is set, in one batch. */
static void
ui_finder_add(Playlist *pl, const Finder *fd, const unsigned char *checked,
    int all, char *msg, size_t msglen)
{
    const char **paths;
    int i, n = 0, added, full = 0, count = finder_count(fd);

    for (i = 0; i < count; i++) {
        if (all || checked[i])
            n++;
    }
    if (n == 0) {
        snprintf(msg, msglen, "Nothing selected.");
        return;
    }
    paths = (const char **)malloc(sizeof *paths * (size_t)n);
    if (!paths) {
        snprintf(msg, msglen, "Out of memory.");
        return;
    }
    for (i = 0, n = 0; i < count; i++) {
        if (all || checked[i])
            paths[n++] = finder_path_at(fd, i);
    }
    added = playlist_add_many(pl, paths, n, &full);
    free((void *)paths);
    if (full > 0)
        snprintf(msg, msglen, "Added %d new track%s; %d skipped, list is full.",
                 added, added == 1 ? "" : "s", full);
    else
        snprintf(msg, msglen, "Added %d new track%s.", added,
                 added == 1 ? "" : "s");
}

/* Expands a leading "~" or "~/" to $HOME in place, so the shorthand works in
 * the folder field. Other paths are left alone. */
static void
ui_expand_home(char *path, size_t len)
{
    const char *home = getenv("HOME");
    char tmp[4096];
    int n;

    if (path[0] != '~' || (path[1] != '/' && path[1] != '\0') ||
        !home || !home[0])
        return;
    n = snprintf(tmp, sizeof tmp, "%s%s", home, path + 1);
    if (n < 0 || (size_t)n >= sizeof tmp || (size_t)n >= len)
        return;
    memcpy(path, tmp, (size_t)n + 1);
}

/* Row text for a found file: the file name first, so it survives clipping at
 * the right edge, then its folder. */
static void
ui_finder_row_label(const char *path, char *out, size_t outlen)
{
    const char *slash = strrchr(path, '/');
    int dir_len;

    if (!slash) {
        snprintf(out, outlen, "%s", path);
        return;
    }
    dir_len = slash == path ? 1 : (int)(slash - path);
    snprintf(out, outlen, "%s   %.*s", slash + 1, dir_len, path);
}

/* Find-audio dialog. Returns 1 while it should stay open. The walk runs in
 * slices from the frame loop. Rows can be ticked only once it has finished, so
 * the sorted order cannot shift under the checkboxes. */
static int
ui_finder_modal(struct nk_context *ctx, float scale, float screen_w,
    float screen_h, Finder *fd, Playlist *pl, char *root, size_t rootlen,
    unsigned char *checked, int *focus, char *msg, size_t msglen)
{
    struct nk_rect bounds;
    float margin = mp_s(40.0f, scale);
    float w = mp_s(640.0f, scale);
    float h = mp_s(480.0f, scale);
    float row_h = mp_s(28.0f, scale);
    float bar_h = mp_s(28.0f, scale);
    int running, count;
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

    if (nk_begin(ctx, "Find audio", bounds,
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        nk_flags evt;
        int do_search = 0;
        char status[160];

        nk_layout_row_dynamic(ctx, row_h, 1);
        nk_label(ctx, "Folder to search (type / for the whole machine):",
                 NK_TEXT_LEFT);

        if (*focus) {
            nk_edit_focus(ctx, 0);
            *focus = 0;
        }
        nk_layout_row_dynamic(ctx, row_h, 1);
        evt = nk_edit_string_zero_terminated(ctx,
            NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, root, (int)rootlen,
            nk_filter_default);
        if (evt & NK_EDIT_COMMITED)
            do_search = 1;

        running = finder_running(fd);
        nk_layout_row_dynamic(ctx, bar_h, 2);
        if (running) {
            if (nk_button_label(ctx, "Stop"))
                finder_cancel(fd);
        } else if (nk_button_label(ctx, "Search")) {
            do_search = 1;
        }
        if (nk_button_label(ctx, "Close"))
            keep = 0;

        if (do_search && !running) {
            memset(checked, 0, FINDER_MAX_RESULT);
            ui_expand_home(root, rootlen);
            if (finder_start(fd, root, track_ext_supported) != 0)
                snprintf(msg, msglen, "Cannot open that folder.");
            else
                msg[0] = '\0';
        }
        running = finder_running(fd);
        count = finder_count(fd);

        if (running)
            snprintf(status, sizeof status, "Searching... %d found", count);
        else if (count == 0)
            snprintf(status, sizeof status, "No audio files found yet.");
        else if (finder_truncated(fd))
            snprintf(status, sizeof status,
                     "%d audio files found (list capped at %d)",
                     count, FINDER_MAX_RESULT);
        else if (finder_out_of_memory(fd))
            snprintf(status, sizeof status,
                     "%d audio files found (out of memory; list incomplete)",
                     count);
        else
            snprintf(status, sizeof status, "%d audio file%s found",
                     count, count == 1 ? "" : "s");
        nk_layout_row_dynamic(ctx, row_h, 1);
        nk_label(ctx, status, NK_TEXT_LEFT);
        if (msg[0]) {
            nk_layout_row_dynamic(ctx, row_h, 1);
            nk_label(ctx, msg, NK_TEXT_LEFT);
        }

        nk_layout_row_dynamic(ctx, bar_h, 4);
        if (running || count == 0)
            nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, "Select all"))
            memset(checked, 1, (size_t)count);
        if (nk_button_label(ctx, "Select none"))
            memset(checked, 0, (size_t)count);
        if (nk_button_label(ctx, "Add selected"))
            ui_finder_add(pl, fd, checked, 0, msg, msglen);
        if (nk_button_label(ctx, "Add all"))
            ui_finder_add(pl, fd, checked, 1, msg, msglen);
        if (running || count == 0)
            nk_widget_disable_end(ctx);

        if (count > 0) {
            struct nk_list_view view;
            float list_h = ui_list_height(ctx, scale, row_h);

            nk_layout_row_dynamic(ctx, list_h, 1);
            if (running)
                nk_widget_disable_begin(ctx);
            if (nk_list_view_begin(ctx, &view, "found", NK_WINDOW_BORDER,
                                   (int)row_h, count)) {
                int i;

                char label[4200];

                nk_layout_row_dynamic(ctx, row_h, 1);
                for (i = view.begin; i < view.end; i++) {
                    nk_bool on = checked[i] ? nk_true : nk_false;

                    ui_finder_row_label(finder_path_at(fd, i), label,
                                        sizeof label);
                    nk_checkbox_label(ctx, label, &on);
                    checked[i] = on ? 1 : 0;
                }
                nk_list_view_end(&view);
            }
            if (running)
                nk_widget_disable_end(ctx);
        }
    }
    nk_end(ctx);
    return keep;
}

/* Adds one station and starts it, connecting at most once. The add itself
 * autoplays a new row when playback was idle (and not a paused live stream),
 * so in that case it has already tried: a failure there is reported, not
 * retried. */
static void
ui_radio_play(Playlist *pl, const RadioStation *st, char *msg, size_t msglen)
{
    const char *urls[1], *names[1];
    int full = 0, i, index = -1, added;
    PlaybackStatus before;

    urls[0] = st->url;
    names[0] = st->name;
    playback_get_status(&before);
    added = playlist_add_stations(pl, urls, names, 1, &full);
    for (i = 0; i < playlist_count(pl); i++) {
        const char *have = playlist_path_at(pl, i);

        if (have && strcmp(have, st->url) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        snprintf(msg, msglen, "The list is full.");
        return;
    }
    if (added > 0 && before.state == MP_STOPPED && !before.live_paused) {
        if (playlist_current(pl) != index)
            snprintf(msg, msglen, "Could not connect to that station.");
        else
            msg[0] = '\0';
        return;
    }
    /* Already playing this one: nothing to do. Anything else (idle, paused,
     * or another row) starts it. */
    if (playlist_current(pl) == index && before.state == MP_PLAYING) {
        msg[0] = '\0';
        return;
    }
    if (playlist_play_index(pl, index) != 0) {
        snprintf(msg, msglen, "Could not connect to that station.");
        return;
    }
    msg[0] = '\0';
}

/* Plays the search result at row index (copies it out first: radio_search_at
 * reuses one buffer). */
static void
ui_radio_play_row(Playlist *pl, RadioSearch *rs, int index, char *msg,
    size_t msglen)
{
    const RadioStation *st = radio_search_at(rs, index);

    if (st) {
        RadioStation copy = *st;

        ui_radio_play(pl, &copy, msg, msglen);
    }
}

/* Adds the checked stations (all = every result). Copies them out first:
 * radio_search_at hands back one station at a time. */
static void
ui_radio_add(Playlist *pl, RadioSearch *rs, const unsigned char *checked,
    int all, char *msg, size_t msglen)
{
    const int count = radio_search_count(rs);
    RadioStation *picked;
    const char **urls, **names;
    int i, n = 0, added, full = 0;

    for (i = 0; i < count; i++) {
        if (all || checked[i])
            n++;
    }
    if (n == 0) {
        snprintf(msg, msglen, "Nothing selected.");
        return;
    }
    picked = (RadioStation *)malloc(sizeof *picked * (size_t)n);
    urls = (const char **)malloc(sizeof *urls * (size_t)n);
    names = (const char **)malloc(sizeof *names * (size_t)n);
    if (!picked || !urls || !names) {
        free(picked);
        free(urls);
        free(names);
        snprintf(msg, msglen, "Out of memory.");
        return;
    }
    for (i = 0, n = 0; i < count; i++) {
        const RadioStation *st;

        if (!all && !checked[i])
            continue;
        st = radio_search_at(rs, i);
        if (!st)
            continue;
        picked[n] = *st;
        urls[n] = picked[n].url;
        names[n] = picked[n].name;
        n++;
    }
    added = playlist_add_stations(pl, urls, names, n, &full);
    free(picked);
    free((void *)urls);
    free((void *)names);

    if (full > 0)
        snprintf(msg, msglen, "Added %d station%s; %d skipped, list is full.",
                 added, added == 1 ? "" : "s", full);
    else
        snprintf(msg, msglen, "Added %d station%s.", added,
                 added == 1 ? "" : "s");
}

/* Row text: name first so it survives clipping, then country and codec. */
static void
ui_radio_row_label(const RadioStation *st, char *out, size_t outlen)
{
    const char *country = st->country[0] ? st->country : "?";

    if (st->bitrate > 0)
        snprintf(out, outlen, "%s  -  %s  -  %s %d kbit/s", st->name, country,
                 st->codec, st->bitrate);
    else
        snprintf(out, outlen, "%s  -  %s  -  %s", st->name, country, st->codec);
}

#define RADIO_DOUBLE_CLICK_MS 400

/* Radio dialog. Returns 1 while it should stay open. The search runs on its
 * own thread; each frame only reads its state. Stations are played from a row
 * click plus Play, or added by checkbox. */
static int
ui_radio_modal(struct nk_context *ctx, float scale, float screen_w,
    float screen_h, RadioSearch *rs, Playlist *pl, char *query, size_t querylen,
    unsigned char *checked, int *selected, int *searched, int *focus,
    char *msg, size_t msglen, int *last_click, Uint32 *last_click_ms)
{
    struct nk_rect bounds;
    float margin = mp_s(40.0f, scale);
    float w = mp_s(640.0f, scale);
    float h = mp_s(480.0f, scale);
    float row_h = mp_s(28.0f, scale);
    float bar_h = mp_s(28.0f, scale);
    int running, count;
    int keep = 1;
    int play_row = -1;

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

    if (nk_begin(ctx, "Radio", bounds,
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        nk_flags evt;
        int do_search = 0;
        char status[200];
        const char *err;

        nk_layout_row_dynamic(ctx, row_h, 1);
        nk_label(ctx, "Station name (internet radio via Radio Browser):",
                 NK_TEXT_LEFT);

        if (*focus) {
            nk_edit_focus(ctx, 0);
            *focus = 0;
        }
        nk_layout_row_dynamic(ctx, row_h, 1);
        evt = nk_edit_string_zero_terminated(ctx,
            NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, query, (int)querylen,
            nk_filter_default);
        if (evt & NK_EDIT_COMMITED)
            do_search = 1;

        running = radio_search_running(rs);
        nk_layout_row_dynamic(ctx, bar_h, 2);
        if (running) {
            if (nk_button_label(ctx, "Stop"))
                radio_search_cancel(rs);
        } else if (nk_button_label(ctx, "Search")) {
            do_search = 1;
        }
        if (nk_button_label(ctx, "Close"))
            keep = 0;

        if (do_search && !running && query[0]) {
            memset(checked, 0, RADIO_MAX_RESULT);
            *selected = -1;
            *searched = 1;
            if (radio_search_start(rs, query) == 0)
                msg[0] = '\0';
        }
        running = radio_search_running(rs);
        count = radio_search_count(rs);
        err = radio_search_error(rs);

        if (running)
            snprintf(status, sizeof status, "%s",
                     radio_search_cancelled(rs) ? "Stopping..." : "Searching...");
        else if (err[0])
            snprintf(status, sizeof status, "%s", err);
        else if (radio_search_cancelled(rs))
            snprintf(status, sizeof status, "Search stopped.");
        else if (!*searched)
            snprintf(status, sizeof status,
                     "Type a station name and press Search.");
        else if (count == 0)
            snprintf(status, sizeof status, "No stations found.");
        else
            snprintf(status, sizeof status,
                     "%d station%s found, most popular first.",
                     count, count == 1 ? "" : "s");
        nk_layout_row_dynamic(ctx, row_h, 1);
        nk_label(ctx, status, NK_TEXT_LEFT);
        if (msg[0]) {
            nk_layout_row_dynamic(ctx, row_h, 1);
            nk_label(ctx, msg, NK_TEXT_LEFT);
        }

        if (count > 0) {
            struct nk_list_view view;
            float list_h = ui_list_height(ctx, scale, row_h);

            nk_layout_row_dynamic(ctx, list_h, 1);
            if (running)
                nk_widget_disable_begin(ctx);
            if (nk_list_view_begin(ctx, &view, "stations", NK_WINDOW_BORDER,
                                   (int)row_h, count)) {
                int i;

                for (i = view.begin; i < view.end; i++) {
                    const RadioStation *st = radio_search_at(rs, i);
                    RadioStation copy;
                    char label[600];
                    nk_bool on = checked[i] ? nk_true : nk_false;

                    if (!st)
                        continue;
                    copy = *st; /* radio_search_at reuses one buffer */
                    ui_radio_row_label(&copy, label, sizeof label);

                    nk_layout_row_template_begin(ctx, row_h);
                    nk_layout_row_template_push_static(ctx, row_h);
                    nk_layout_row_template_push_dynamic(ctx);
                    nk_layout_row_template_end(ctx);

                    nk_checkbox_label(ctx, "", &on);
                    checked[i] = on ? 1 : 0;
                    if (nk_select_label(ctx, label, NK_TEXT_LEFT, *selected == i)) {
                        Uint32 now = SDL_GetTicks();

                        *selected = i;
                        if (*last_click == i &&
                            now - *last_click_ms < RADIO_DOUBLE_CLICK_MS) {
                            play_row = i;
                            *last_click = -1;
                        } else {
                            *last_click = i;
                            *last_click_ms = now;
                        }
                    }
                }
                nk_list_view_end(&view);
            }
            if (running)
                nk_widget_disable_end(ctx);
        }
        /* Deferred until after the list: playing connects, and the list is
         * already laid out for this frame. */
        if (play_row >= 0)
            ui_radio_play_row(pl, rs, play_row, msg, msglen);

        nk_layout_row_dynamic(ctx, bar_h, 3);
        if (running || *selected < 0)
            nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, "Play"))
            ui_radio_play_row(pl, rs, *selected, msg, msglen);
        if (running || *selected < 0)
            nk_widget_disable_end(ctx);

        if (running || count == 0)
            nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, "Add selected"))
            ui_radio_add(pl, rs, checked, 0, msg, msglen);
        if (nk_button_label(ctx, "Add all"))
            ui_radio_add(pl, rs, checked, 1, msg, msglen);
        if (running || count == 0)
            nk_widget_disable_end(ctx);
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
    /* Below about 280 pt the header's fixed cells (Open + meter reserve) cover
     * the whole row and the meter overlay sits over the Open button. Keep the
     * window wide enough that they never meet. */
    SDL_SetWindowMinimumSize(win, 360, 0);

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
    MeterStyle meter_style = settings_meter_style();
    nk_style_default(ctx);
    mp_theme_apply(ctx, ui_scale, &theme);
    ui_select_font(ctx, ratio, font_1x, font_2x);
    float style_scale = ui_scale;

    if (playback_init() != 0)
        SDL_Log("playback_init failed: %s", SDL_GetError());

    FileBrowser *browser = file_browser_create();
    if (!browser)
        SDL_Log("file browser allocation failed; Open is disabled");

    Finder *finder = finder_create();
    if (!finder)
        SDL_Log("finder allocation failed; Find audio is disabled");

    RadioSearch *radio = radio_search_create();
    if (!radio)
        SDL_Log("radio allocation failed; Radio is disabled");

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
    int finder_open = 0;
    int finder_focus = 0;
    char finder_root[4096] = "";
    char finder_msg[256] = "";
    unsigned char finder_checked[FINDER_MAX_RESULT];
    int radio_open = 0;
    int radio_focus = 0;
    int radio_searched = 0;
    int radio_sel = -1;
    int radio_last_click = -1;
    Uint32 radio_last_click_ms = 0;
    char radio_query[256] = "";
    char radio_msg[256] = "";
    unsigned char radio_checked[RADIO_MAX_RESULT];

    struct UiButtons buttons;
    memset(&buttons, 0, sizeof buttons);
    /* The handles sit at their resting scale from the first frame; only hover
     * and glow ease in from zero. */
    buttons.seek.handle = 1.0f;
    buttons.vol.handle = 1.0f;

    /* Per-frame dt comes from the high-resolution counter (see the loop). */
    Uint64 last_ticks = SDL_GetPerformanceCounter();

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
            modal_active = save_modal_open || finder_open || radio_open ||
                file_browser_is_open(browser);

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
                } else if (finder_open) {
                    finder_cancel(finder);
                    finder_open = 0;
                    SDL_StopTextInput();
                } else if (radio_open) {
                    radio_search_cancel(radio);
                    radio_open = 0;
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
                    buttons.focus = UI_TAB_OPEN;
            } else if (!modal_active && evt.type == SDL_KEYDOWN &&
                       evt.key.repeat &&
                       (evt.key.keysym.scancode == SDL_SCANCODE_RETURN ||
                        evt.key.keysym.scancode == SDL_SCANCODE_KP_ENTER)) {
                /* Each Enter repeat adds a key edge in Nuklear, so a held Enter
                 * would re-fire the focused transport button. Dropped here; the
                 * modal gate keeps repeats flowing into a dialog's text entry. */
                consumed = 1;
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
        char curbuf[16];
        char totbuf[16];
        char volbuf[32];
        char title[640];
        char chbuf[16];
        char bitsbuf[16];
        char ratebuf[24];
        const char *chips[4];
        int nchips = 0;
        enum mp_play_state play_ps;
        const char *base;
        float progress;
        double shown;
        int browser_open;
        int panel_actions = PL_ACTION_NONE;
        int dock_open;

        browser_open = file_browser_is_open(browser);
        modal_active = save_modal_open || finder_open || radio_open ||
            browser_open;

        /* One dt per frame for the easing and the theme clock, clamped so a
         * stalled window (a drag, a resize) cannot jump an animation to its
         * end. */
        {
            Uint64 now = SDL_GetPerformanceCounter();
            double span = (double)(now - last_ticks) /
                          (double)SDL_GetPerformanceFrequency();

            last_ticks = now;
            if (span > 0.1)
                span = 0.1;
            buttons.opt.dt = (float)span;
            mp_theme_tick(&theme, buttons.opt.dt);
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

        if (st.has_track && st.is_live) {
            /* No length: elapsed time since the stream opened, and LIVE. */
            int cur = (int)st.position;

            if (cur < 0)
                cur = 0;
            snprintf(curbuf, sizeof curbuf, "%d:%02d", cur / 60, cur % 60);
            snprintf(totbuf, sizeof totbuf, "LIVE");
        } else if (st.has_track) {
            int cur = (int)shown;
            int dur = (int)st.duration;
            if (cur < 0)
                cur = 0;
            if (cur > dur)
                cur = dur;
            snprintf(curbuf, sizeof curbuf, "%d:%02d", cur / 60, cur % 60);
            snprintf(totbuf, sizeof totbuf, "%d:%02d", dur / 60, dur % 60);
        } else {
            snprintf(curbuf, sizeof curbuf, "--:--");
            snprintf(totbuf, sizeof totbuf, "--:--");
        }

        progress = st.duration > 0.0 ? (float)(st.position / st.duration) : 0.0f;
        if (progress < 0.0f)
            progress = 0.0f;
        if (progress > 1.0f)
            progress = 1.0f;

        snprintf(volbuf, sizeof volbuf, "%d%%", st.volume_percent);

        base = "";
        if (st.has_track) {
            const char *slash = strrchr(st.name, '/');
            base = slash ? slash + 1 : st.name;
            if (st.is_live) {
                /* A stream's URL means little to a reader: show the station. */
                const char *label = playlist_label_at(pl, playlist_current(pl));

                base = label ? label : st.name;
            }
        }

        /* Header chips: the same decoded fields as before, one chip each. */
        if (st.has_track) {
            if (st.codec[0])
                chips[nchips++] = st.codec;
            if (st.bits_per_sample > 0) {
                snprintf(bitsbuf, sizeof bitsbuf, "%d-bit",
                         st.bits_per_sample);
                chips[nchips++] = bitsbuf;
            }
            snprintf(ratebuf, sizeof ratebuf, "%d Hz", st.sample_rate);
            chips[nchips++] = ratebuf;

            if (st.channels == 1)
                snprintf(chbuf, sizeof chbuf, "mono");
            else if (st.channels == 2)
                snprintf(chbuf, sizeof chbuf, "stereo");
            else
                snprintf(chbuf, sizeof chbuf, "%d ch", st.channels);
            chips[nchips++] = chbuf;
        }

        if (st.state == MP_PLAYING)
            play_ps = MP_PS_PLAYING;
        else if (st.state == MP_PAUSED)
            play_ps = MP_PS_PAUSED;
        else
            play_ps = MP_PS_STOPPED;

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
        float header_y = 0.0f;

        if (nk_begin(ctx, "aldrem",
                     nk_rect(0, 0, (float)px_w, (float)px_h),
                     NK_WINDOW_NO_SCROLLBAR))
        {
            /* Everything lays out inside the centred column; the window and
             * the backdrop still cover the whole frame. */
            struct nk_rect col = ui_column(nk_window_get_content_region(ctx),
                                           ui_scale);

            /* header: Open button + status; inert while a dialog is up. The
             * right cell is left blank for the level meter overlay. */
            header_y = nk_window_get_content_region(ctx).y;
            nk_layout_row_template_begin(ctx, mp_s(UI_VU_H, ui_scale));
            nk_layout_row_template_push_static(ctx, mp_s(90.0f, ui_scale));
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_push_static(ctx,
                                               mp_s(UI_VU_RESERVE, ui_scale));
            nk_layout_row_template_end(ctx);
            if (mp_button(ctx, ui_scale, &theme, "Open", MP_BTN_PRIMARY,
                          MP_ICON_PLAY, UI_TAB_OPEN,
                          browser && !modal_active, &buttons.open,
                          &buttons.opt)) {
                browser_list_mode = 0;
                file_browser_open(browser,
                                  st.has_track && !st.is_live ? st.name : NULL);
            }
            if (st.error[0])
                mp_error_panel(ctx, ui_scale, &theme, st.error);
            else
                mp_status_row(ctx, ui_scale, &theme,
                              st.has_track ? base : NULL, chips, nchips,
                              play_ps);

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
                double seek = 0.0;

                if (ui_transport_panel(ctx, ui_scale, &theme, &st, pl,
                                       modal_active, curbuf, totbuf, progress,
                                       volbuf, &seek_dragging,
                                       &seek_preview, &buttons, &seek))
                    playback_seek(seek);
            }

            /* The playlist band sits below the dock, in the same column, and
             * takes whatever height is left. Only touched once the dock
             * closed: mp_dock_end restores the shared style before it draws. */
            if (dock_open)
                mp_dock_end(ctx);
            nk_layout_row_dynamic(ctx, mp_s(4.0f, ui_scale), 1);
            panel_actions = ui_playlist_panel(pl, ctx, ui_scale, modal_active,
                                              finder != NULL,
                                              radio != NULL && net_available(),
                                              &theme, st.has_track);
        }
        nk_end(ctx);
        nk_style_pop_style_item(ctx);

        /* Meter overlay sits above the main window, pinned to the frame's top
         * right. It stays live while a dialog is up; only its toggle is gated. */
        ui_vu_overlay(ctx, ui_scale, &theme, (float)px_w, header_y,
                      &meter_style, &buttons, &st, !modal_active);

        /* Toolbar consequences run after the main window closes so dialogs are
         * never opened from inside another window's draw. */
        if (panel_actions & PL_ACTION_OPEN_LIST) {
            browser_list_mode = 1;
            file_browser_open_for(browser,
                                  st.has_track && !st.is_live ? st.name : NULL,
                                  playlist_ext_supported, "Open playlist");
        }
        if (panel_actions & PL_ACTION_SAVE_AS) {
            ui_save_path_default(save_path, sizeof save_path);
            save_err[0] = '\0';
            save_focus = 1;
            save_modal_open = 1;
            SDL_StartTextInput();
        }
        if (panel_actions & PL_ACTION_RADIO) {
            radio_msg[0] = '\0';
            radio_focus = 1;
            radio_open = 1;
            SDL_StartTextInput();
        }
        if (panel_actions & PL_ACTION_FIND) {
            const char *home = getenv("HOME");

            /* Remember the last folder for the session; default to $HOME. */
            if (finder_root[0] == '\0')
                snprintf(finder_root, sizeof finder_root, "%s",
                         home && home[0] ? home : "/");
            finder_msg[0] = '\0';
            finder_focus = 1;
            finder_open = 1;
            SDL_StartTextInput();
        }

        /* Drawn as a second window after the main layout, which is what makes
         * "disable the main controls while open" sufficient to stop clicks
         * leaking through to the seek bar and transport. */
        if (browser_open && browser) {
            char chosen[4096];

            if (file_browser_draw(browser, ctx, ui_scale, &theme,
                                  buttons.opt.dt, (float)px_w, (float)px_h,
                                  chosen, sizeof chosen) == 1) {
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

        /* Find-audio scans a slice per frame, then draws over the main window
         * like the other dialogs. Closing cancels the walk. */
        if (finder_running(finder))
            finder_step(finder, 256);
        if (finder_open) {
            if (!ui_finder_modal(ctx, ui_scale, (float)px_w, (float)px_h,
                                 finder, pl, finder_root, sizeof finder_root,
                                 finder_checked, &finder_focus,
                                 finder_msg, sizeof finder_msg)) {
                finder_cancel(finder);
                finder_open = 0;
                SDL_StopTextInput();
            }
        }

        /* Radio polls its worker thread each frame; the dialog only reads. */
        if (radio_open) {
            if (!ui_radio_modal(ctx, ui_scale, (float)px_w, (float)px_h, radio,
                                pl, radio_query, sizeof radio_query,
                                radio_checked, &radio_sel, &radio_searched,
                                &radio_focus, radio_msg, sizeof radio_msg,
                                &radio_last_click, &radio_last_click_ms)) {
                radio_search_cancel(radio);
                radio_open = 0;
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
    radio_search_free(radio);
    finder_free(finder);
    file_browser_destroy(browser);
    playback_shutdown();
    nk_sdl_shutdown();
    mp_theme_shutdown(&theme);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
