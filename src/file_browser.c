/* file_browser.c — a small modal directory browser built on Nuklear.
 *
 * Lists "..", subdirectories and .wav/.mp3 files in one selectable column.
 * Double-click (or select + Open) enters a directory or returns the chosen
 * file's absolute path. Click handling is deliberately "return value means
 * clicked": nk_select_label toggles the value passed in, so the stored
 * selection is owned here, never read back from Nuklear.
 */
#include "file_browser.h"
#include "nuklear_config.h"
#include "track.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>

#define FB_PATH_MAX    4096
#define FB_NAME_MAX    256
#define FB_MAX_ENTRIES 1024
#define FB_DOUBLE_CLICK_MS 400

typedef struct {
    char name[FB_NAME_MAX];
    int is_dir;
} FbEntry;

struct FileBrowser {
    int open;
    char dir[FB_PATH_MAX];
    char last_dir[FB_PATH_MAX];
    int has_last_dir;
    FbEntry entries[FB_MAX_ENTRIES];
    int count;
    int selected;
    char error[256];
    int last_click;
    Uint32 last_click_ms;
    /* Incremental scan: a bounded slice per frame, results staged so the
     * visible listing only changes when the scan commits. */
    DIR *scan_dir;
    char scan_path[FB_PATH_MAX];
    int scan_count;
    FbEntry *staging;
};

static int
fb_name_cmp_ci(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);

        if (ca != cb)
            return ca - cb;
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int
fb_entry_cmp(const void *pa, const void *pb)
{
    const FbEntry *a = (const FbEntry *)pa;
    const FbEntry *b = (const FbEntry *)pb;
    int r;

    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    r = fb_name_cmp_ci(a->name, b->name);
    if (r != 0)
        return r;
    return strcmp(a->name, b->name);
}

/* Drops any in-flight scan and its staged results. */
static void
fb_cancel_scan(FileBrowser *fb)
{
    if (fb->scan_dir) {
        closedir(fb->scan_dir);
        fb->scan_dir = NULL;
    }
    free(fb->staging);
    fb->staging = NULL;
    fb->scan_count = 0;
}

/* Starts scanning path. Returns 0 once enumeration can begin (the listing is
 * committed lazily, a slice per frame); -1 if the directory cannot be opened,
 * in which case the error row explains why and the old listing is kept. */
static int
fb_begin_scan(FileBrowser *fb, const char *path)
{
    char resolved[FB_PATH_MAX];

    fb_cancel_scan(fb);

    if (!realpath(path, resolved) || !(fb->scan_dir = opendir(resolved))) {
        snprintf(fb->error, sizeof fb->error, "cannot open directory");
        return -1;
    }

    fb->staging = (FbEntry *)malloc(sizeof *fb->staging * FB_MAX_ENTRIES);
    if (!fb->staging) {
        closedir(fb->scan_dir);
        fb->scan_dir = NULL;
        snprintf(fb->error, sizeof fb->error, "out of memory");
        return -1;
    }

    snprintf(fb->scan_path, sizeof fb->scan_path, "%s", resolved);
    snprintf(fb->staging[0].name, FB_NAME_MAX, "..");
    fb->staging[0].is_dir = 1;
    fb->scan_count = 1;
    fb->error[0] = '\0';
    return 0;
}

static void
fb_commit_scan(FileBrowser *fb)
{
    int count = fb->scan_count;

    if (!fb->staging)
        return;
    if (fb->scan_dir) {
        closedir(fb->scan_dir);
        fb->scan_dir = NULL;
    }

    /* ".." stays pinned at index 0 and is never sorted. */
    if (count > 2)
        qsort(&fb->staging[1], (size_t)(count - 1),
              sizeof fb->staging[0], fb_entry_cmp);

    memcpy(fb->entries, fb->staging, (size_t)count * sizeof fb->entries[0]);
    fb->count = count;

    free(fb->staging);
    fb->staging = NULL;
    fb->scan_count = 0;

    snprintf(fb->dir, sizeof fb->dir, "%s", fb->scan_path);
    snprintf(fb->last_dir, sizeof fb->last_dir, "%s", fb->scan_path);
    fb->has_last_dir = 1;
    fb->error[0] = '\0';
    fb->selected = -1;
    fb->last_click = -1;
}

/* Advances a running scan by a bounded slice so a large or slow directory
 * cannot stall the frame loop. Commits once every entry has been visited. */
static void
fb_advance_scan(FileBrowser *fb)
{
    int budget = 128;
    int finished = 0;

    if (!fb->scan_dir)
        return;

    while (budget-- > 0) {
        char joined[FB_PATH_MAX];
        struct dirent *de;
        struct stat sb;
        int is_dir;

        if (fb->scan_count >= FB_MAX_ENTRIES) {
            finished = 1;
            break;
        }
        de = readdir(fb->scan_dir);
        if (!de) {
            finished = 1;
            break;
        }
        if (de->d_name[0] == '.')
            continue;
        snprintf(joined, sizeof joined, "%s/%s", fb->scan_path, de->d_name);
        if (stat(joined, &sb) != 0)
            continue;

        is_dir = S_ISDIR(sb.st_mode);
        if (!is_dir) {
            if (!S_ISREG(sb.st_mode))
                continue;
            if (!track_ext_supported(de->d_name))
                continue;
        }

        snprintf(fb->staging[fb->scan_count].name, FB_NAME_MAX, "%s", de->d_name);
        fb->staging[fb->scan_count].is_dir = is_dir;
        fb->scan_count++;
    }

    if (finished)
        fb_commit_scan(fb);
}

FileBrowser *
file_browser_create(void)
{
    FileBrowser *fb = (FileBrowser *)calloc(1, sizeof *fb);

    if (fb) {
        fb->selected = -1;
        fb->last_click = -1;
    }
    return fb;
}

void
file_browser_destroy(FileBrowser *fb)
{
    if (!fb)
        return;
    fb_cancel_scan(fb);
    free(fb);
}

void
file_browser_close(FileBrowser *fb)
{
    if (fb) {
        fb_cancel_scan(fb);
        fb->open = 0;
    }
}

int
file_browser_is_open(const FileBrowser *fb)
{
    return fb && fb->open;
}

void
file_browser_open(FileBrowser *fb, const char *suggested_path)
{
    char cwd[FB_PATH_MAX];

    if (!fb)
        return;
    fb->open = 1;

    if (fb->has_last_dir) {
        if (fb_begin_scan(fb, fb->last_dir) == 0)
            return;
    } else if (suggested_path && suggested_path[0]) {
        const char *slash = strrchr(suggested_path, '/');

        if (slash) {
            size_t n = (size_t)(slash - suggested_path);
            char dir[FB_PATH_MAX];

            if (n == 0) {
                dir[0] = '/';
                dir[1] = '\0';
            } else if (n < sizeof dir) {
                memcpy(dir, suggested_path, n);
                dir[n] = '\0';
            } else {
                dir[0] = '\0';
            }
            if (dir[0] && fb_begin_scan(fb, dir) == 0)
                return;
        }
    }

    const char *home = getenv("HOME");
    int cwd_usable = getcwd(cwd, sizeof cwd) != NULL && strcmp(cwd, "/") != 0;

    if (cwd_usable && fb_begin_scan(fb, cwd) == 0)
        return;
    if (home && home[0] && fb_begin_scan(fb, home) == 0)
        return;

    fb_begin_scan(fb, "/");
}

/* Physical-pixel sizing, matching main.c's ui_s convention. */
static float
fb_s(float v, float scale)
{
    return lroundf(v * scale);
}

int
file_browser_draw(FileBrowser *fb, struct nk_context *ctx, float scale,
    float screen_w, float screen_h, char *out, size_t outlen)
{
    struct nk_rect bounds;
    float row_h, footer_h, spacing, margin, list_h;
    float w, h;
    int result = 0;
    int chosen = -1;
    int cancelled = 0;
    int loading;

    if (!fb || !fb->open)
        return -1;

    /* Keep enumeration off the critical path: a bounded slice per frame. */
    fb_advance_scan(fb);
    loading = fb->scan_dir != NULL;

    row_h = fb_s(28.0f, scale);
    footer_h = fb_s(30.0f, scale);
    spacing = fb_s(4.0f, scale);
    margin = fb_s(40.0f, scale);
    w = fb_s(560.0f, scale);
    h = fb_s(460.0f, scale);

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

    if (nk_begin(ctx, "Open file", bounds,
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE |
                 NK_WINDOW_NO_SCROLLBAR)) {
        int activate = -1;
        int i;

        nk_layout_row_dynamic(ctx, row_h, 1);
        if (fb->error[0])
            nk_label(ctx, fb->error, NK_TEXT_LEFT);
        else
            nk_label(ctx, loading ? fb->scan_path : fb->dir, NK_TEXT_LEFT);

        list_h = nk_window_get_content_region(ctx).h
            - row_h - footer_h - 3.0f * spacing;
        if (list_h < row_h)
            list_h = row_h;

        nk_layout_row_dynamic(ctx, list_h, 1);
        if (loading) {
            nk_label(ctx, "loading...", NK_TEXT_LEFT);
        } else if (nk_group_begin(ctx, "files", NK_WINDOW_BORDER)) {
            nk_layout_row_dynamic(ctx, row_h, 1);
            for (i = 0; i < fb->count; i++) {
                char text[FB_NAME_MAX + 2];

                if (fb->entries[i].is_dir)
                    snprintf(text, sizeof text, "%s/", fb->entries[i].name);
                else
                    snprintf(text, sizeof text, "%s", fb->entries[i].name);

                /* Nonzero return means clicked; the value is only a render
                 * flag, so the borrowed truth is fb->selected. */
                if (nk_select_label(ctx, text, NK_TEXT_LEFT,
                                    i == fb->selected)) {
                    Uint32 now = SDL_GetTicks();

                    if (fb->last_click == i &&
                        now - fb->last_click_ms < FB_DOUBLE_CLICK_MS) {
                        activate = i;
                    } else {
                        fb->selected = i;
                        fb->last_click = i;
                        fb->last_click_ms = now;
                    }
                }
            }
            nk_group_end(ctx);
        }

        /* Deferred: activation starts a new scan after the row loop, so the
         * index being iterated above stays valid. */
        if (activate >= 0)
            chosen = activate;

        nk_layout_row_dynamic(ctx, footer_h, 2);
        if (fb->selected < 0 || loading)
            nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, "Open"))
            chosen = fb->selected;
        if (fb->selected < 0 || loading)
            nk_widget_disable_end(ctx);
        if (nk_button_label(ctx, "Cancel"))
            cancelled = 1;
    }
    nk_end(ctx);

    if (cancelled) {
        fb_cancel_scan(fb);
        fb->open = 0;
        return -1;
    }

    if (chosen >= 0 && chosen < fb->count) {
        char joined[FB_PATH_MAX];

        snprintf(joined, sizeof joined, "%s/%s",
                 fb->dir, fb->entries[chosen].name);
        if (fb->entries[chosen].is_dir) {
            fb_begin_scan(fb, joined);
        } else {
            snprintf(out, outlen, "%s", joined);
            fb->open = 0;
            result = 1;
        }
    }

    return result;
}
