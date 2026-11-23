/* file_browser.h — minimal in-app directory browser for choosing a track.
 * Nuklear stays out of this header: callers only pass an opaque context. */
#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include <stddef.h>

struct nk_context;

typedef struct FileBrowser FileBrowser;

FileBrowser *file_browser_create(void);
void file_browser_destroy(FileBrowser *fb);

/* Opens the browser at last_dir, else the directory of suggested_path, else
 * the current working directory, else "/". */
void file_browser_open(FileBrowser *fb, const char *suggested_path);
void file_browser_close(FileBrowser *fb);
int  file_browser_is_open(const FileBrowser *fb);

/* 1 = file chosen (absolute path written to out, fb closed),
 * -1 = cancelled/closed, 0 = still open. Call only while is_open. */
int  file_browser_draw(FileBrowser *fb, struct nk_context *ctx, float scale,
                       float screen_w, float screen_h,
                       char *out, size_t outlen);

#endif
