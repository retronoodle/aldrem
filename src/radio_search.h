/* radio_search.h — background station search for the Radio dialog.
 *
 * A worker thread fetches the mirror list and the stations JSON with net.c,
 * then parses them with radio.c. The UI thread starts a search, polls its state
 * from the frame loop and reads the results once it has finished, the same
 * slice-from-frame pattern as the finder, except the work is off the UI thread
 * because the network can stall.
 *
 * Without HAVE_CURL, radio_search_start fails with a clear message. */
#ifndef RADIO_SEARCH_H
#define RADIO_SEARCH_H

#include "radio.h"

typedef struct RadioSearch RadioSearch;

/* Returns NULL when allocation fails. */
RadioSearch *radio_search_create(void);
/* Waits for a running search to finish, then frees everything. */
void radio_search_free(RadioSearch *s);

/* Starts a search for name. Returns 0 when started, -1 when a search is still
 * running (cancelled or not) or the network is unavailable; the reason is in
 * radio_search_error. Clears the previous results on success. */
int radio_search_start(RadioSearch *s, const char *query);
/* Drops the running search's results when it finishes. Returns at once; the
 * worker exits after its current fetch. */
void radio_search_cancel(RadioSearch *s);

/* 1 while the worker thread is alive (includes a cancelled search that has
 * not finished its current fetch). */
int radio_search_running(const RadioSearch *s);
/* 1 when the current search has been cancelled (its results will be dropped). */
int radio_search_cancelled(const RadioSearch *s);

int                 radio_search_count(const RadioSearch *s);
/* Valid until the next call, so copy what you need straight away. NULL if out
 * of range. */
const RadioStation *radio_search_at(const RadioSearch *s, int index);
/* Empty when the last search succeeded, otherwise a short human reason (no
 * mirror reachable, bad response). Valid until the next call. */
const char *radio_search_error(const RadioSearch *s);

#endif
