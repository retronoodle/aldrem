/* favorites.h — starred internet radio stations. The list is keyed by stream
 * URL and persisted as $XDG_CONFIG_HOME/aldrem/favorites.tsv (falling back to
 * $HOME/.config/aldrem/): one station per line, tab-separated as name, url,
 * codec, country, bitrate. Pure file I/O, no SDL, so it is unit-testable. */
#ifndef FAVORITES_H
#define FAVORITES_H

#include "radio.h"

#define FAV_MAX 500 /* stations kept in favorites */

/* Results of favorites_toggle. */
#define FAV_REMOVED 0  /* was a favorite, now is not */
#define FAV_ADDED   1  /* was not a favorite, now is */
#define FAV_FULL   -1  /* list is at FAV_MAX; nothing changed */
#define FAV_IO     -2  /* could not save; nothing changed */

/* Reads the saved list, replacing what is in memory. Malformed lines and
 * repeated URLs are skipped. Returns the number of favorites. Calls made
 * before this are loaded on demand. */
int favorites_load(void);

int favorites_count(void);

/* Returns the favorite at index, or NULL when out of range. The pointer is
 * valid until the next favorites_toggle or favorites_load. */
const RadioStation *favorites_at(int index);

/* Nonzero when a favorite has this stream URL. */
int favorites_has(const char *url);

/* Adds st when its URL is not a favorite, removes it when it is. Saves at once
 * through a temp file and rename. Returns one of the FAV_ results above. */
int favorites_toggle(const RadioStation *st);

#endif
