/* playlist.h — the track queue: model, .m3u persistence, and playback
 * orchestration. Nuklear stays out of this header: callers only pass an opaque
 * context. All functions run on the UI thread. */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <stddef.h>

typedef struct Playlist Playlist;

/* Loads the autosaved list; never starts playback. */
Playlist *playlist_create(void);
/* Final autosave, then free. */
void playlist_destroy(Playlist *pl);

int         playlist_count(const Playlist *pl);
const char *playlist_path_at(const Playlist *pl, int index); /* NULL if out of range */
int         playlist_current(const Playlist *pl);  /* playing row, -1 when none */
int         playlist_selected(const Playlist *pl); /* highlighted row, -1 when none */
void        playlist_select(Playlist *pl, int index);

/* Appends a supported regular file; returns 1 when added, 0 when skipped.
 * Autoplays the added entry only when playback is MP_STOPPED. */
int  playlist_add(Playlist *pl, const char *path);
void playlist_remove_selected(Playlist *pl);
void playlist_clear(Playlist *pl);

/* Manual activation: loads first, then moves current and the selection. On a
 * failed load current is left untouched and the status label shows the error. */
int  playlist_play_index(Playlist *pl, int index);
int  playlist_next(Playlist *pl); /* no-op on the last row */
int  playlist_prev(Playlist *pl); /* row 0 rewinds the current track */
/* Auto-advance after EOF: tries the following rows, skips unloadable ones and
 * stops at the end of the list. Leaves the user's selection alone. */
void playlist_track_finished(Playlist *pl);

int  playlist_save_as(Playlist *pl, const char *path, char *err, size_t errlen);
/* Replaces the list from an .m3u file. Plays nothing and stops the current
 * track; a failed read leaves both the list and playback untouched. */
int  playlist_load_file(Playlist *pl, const char *path, char *err, size_t errlen);
int  playlist_ext_supported(const char *name); /* .m3u / .m3u8 */
/* Last path saved or loaded this session, for the Save-as prefill ("" if none).
 * Deliberately not persisted. */
const char *playlist_last_file(void);

#endif
