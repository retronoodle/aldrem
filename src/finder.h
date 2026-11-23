/* finder.h — recursive audio-file search, run in slices from the UI loop.
 * No SDL or Nuklear dependency. The caller supplies the extension filter, so
 * the format list stays in track.c. All functions run on the UI thread. */
#ifndef FINDER_H
#define FINDER_H

typedef struct Finder Finder;

/* Results beyond this are dropped and finder_truncated() reports it. Matches
 * the playlist cap, so every result fits in one batch add. */
#define FINDER_MAX_RESULT 10000

/* Returns 1 when a file name (no directory part) is a wanted audio file. */
typedef int (*finder_filter_fn)(const char *name);

Finder *finder_create(void);
void    finder_free(Finder *f);

/* Starts a fresh search under root, discarding earlier results. Returns 0 on
 * success, -1 when root cannot be opened as a directory. */
int finder_start(Finder *f, const char *root, finder_filter_fn filter);
/* Scans up to budget directory entries. Returns 1 while more work remains. */
int finder_step(Finder *f, int budget);
/* Stops the scan and keeps what was found so far. */
void finder_cancel(Finder *f);

int         finder_running(const Finder *f);
int         finder_count(const Finder *f);
const char *finder_path_at(const Finder *f, int index); /* NULL if out of range */
int         finder_truncated(const Finder *f); /* hit the result cap */
int         finder_out_of_memory(const Finder *f); /* a result was lost to allocation failure */

#endif
