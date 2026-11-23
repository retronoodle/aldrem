/* file_browser.h — minimal in-app directory browser for choosing a track.
 * Nuklear stays out of this header: callers only pass an opaque context. */
#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include <stddef.h>

struct nk_context;
struct MpTheme;

typedef struct FileBrowser FileBrowser;

/* Nonzero when the browser should list name. NULL keeps the audio default. */
typedef int (*fb_filter_fn)(const char *name);

FileBrowser *file_browser_create(void);
void file_browser_destroy(FileBrowser *fb);

/* Opens the browser at last_dir, else the directory of suggested_path, else
 * the current working directory, else "/". */
void file_browser_open(FileBrowser *fb, const char *suggested_path);
/* Same, but lists files matching filter (NULL = supported audio files) and
 * titles the window. Used by playlist mode for .m3u/.m3u8 lists. */
void file_browser_open_for(FileBrowser *fb, const char *suggested_path,
                           fb_filter_fn filter, const char *title);
void file_browser_close(FileBrowser *fb);
int  file_browser_is_open(const FileBrowser *fb);

/* 1 = file chosen (absolute path written to out, fb closed),
 * -1 = cancelled/closed, 0 = still open. Call only while is_open.
 * theme may be NULL, which keeps the stock footer buttons; a theme draws the
 * footer with the shared button helper (Phase 6 passes it). dt is the frame
 * time in seconds for the footer's hover and press easing. */
int  file_browser_draw(FileBrowser *fb, struct nk_context *ctx, float scale,
                       struct MpTheme *theme, float dt, float screen_w,
                       float screen_h, char *out, size_t outlen);

#endif
