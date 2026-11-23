/* finder.c — iterative, cancellable directory walk for audio files.
 *
 * Uses an explicit stack of open directories instead of recursion so a scan can
 * pause between frames. Symlinked directories are never followed (no loops),
 * hidden entries are skipped, and results are sorted once the walk ends.
 */
/* opendir/readdir/lstat are POSIX, not ISO C: -std=c99 hides them from glibc
 * unless a feature macro asks for them, so this goes before any include. */
#define _XOPEN_SOURCE 700
#include "finder.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FINDER_PATH_MAX   4096
#define FINDER_MAX_DEPTH  64

typedef struct {
    DIR *dh;
    char path[FINDER_PATH_MAX];
} FinderDir;

struct Finder {
    FinderDir *stack; /* FINDER_MAX_DEPTH slots, calloc'd */
    int depth;        /* open directories on the stack */
    char **paths;
    int count;
    int cap;
    int truncated;
    int out_of_memory; /* a result could not be stored; the list is incomplete */
    int running;
    finder_filter_fn filter;
};

static int
finder_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void
finder_close_stack(Finder *f)
{
    while (f->depth > 0) {
        f->depth--;
        if (f->stack[f->depth].dh)
            closedir(f->stack[f->depth].dh);
        f->stack[f->depth].dh = NULL;
    }
}

static void
finder_clear_results(Finder *f)
{
    int i;

    for (i = 0; i < f->count; i++)
        free(f->paths[i]);
    f->count = 0;
    f->truncated = 0;
    f->out_of_memory = 0;
}

/* Ends the walk: closes handles and sorts results into path order. */
static void
finder_finish(Finder *f)
{
    finder_close_stack(f);
    f->running = 0;
    if (f->count > 1)
        qsort(f->paths, (size_t)f->count, sizeof *f->paths, finder_cmp);
}

/* Pushes a directory onto the walk stack. Returns 0 on success. */
static int
finder_push_dir(Finder *f, const char *path)
{
    FinderDir *top;

    if (f->depth >= FINDER_MAX_DEPTH)
        return -1;
    top = &f->stack[f->depth];
    if (strlen(path) >= sizeof top->path)
        return -1;
    top->dh = opendir(path);
    if (!top->dh)
        return -1;
    strcpy(top->path, path);
    f->depth++;
    return 0;
}

/* Appends a copy of path to the results. Returns 0 on success. */
static int
finder_add_result(Finder *f, const char *path)
{
    char *copy;
    char **grown;
    int next;

    if (f->count >= FINDER_MAX_RESULT) {
        f->truncated = 1;
        return -1;
    }
    if (f->count >= f->cap) {
        next = f->cap ? f->cap * 2 : 64;
        if (next > FINDER_MAX_RESULT)
            next = FINDER_MAX_RESULT;
        grown = (char **)realloc(f->paths, (size_t)next * sizeof *grown);
        if (!grown) {
            f->out_of_memory = 1;
            return -1;
        }
        f->paths = grown;
        f->cap = next;
    }
    copy = (char *)malloc(strlen(path) + 1);
    if (!copy) {
        f->out_of_memory = 1;
        return -1;
    }
    strcpy(copy, path);
    f->paths[f->count++] = copy;
    return 0;
}

Finder *
finder_create(void)
{
    Finder *f = (Finder *)calloc(1, sizeof *f);

    if (f) {
        f->stack = (FinderDir *)calloc(FINDER_MAX_DEPTH, sizeof *f->stack);
        if (!f->stack) {
            free(f);
            return NULL;
        }
    }
    return f;
}

void
finder_free(Finder *f)
{
    if (!f)
        return;
    finder_close_stack(f);
    finder_clear_results(f);
    free(f->paths);
    free(f->stack);
    free(f);
}

int
finder_start(Finder *f, const char *root, finder_filter_fn filter)
{
    if (!f || !root || !root[0] || !filter)
        return -1;

    finder_close_stack(f);
    finder_clear_results(f);
    f->filter = filter;
    f->running = 0;
    if (finder_push_dir(f, root) != 0)
        return -1;
    f->running = 1;
    return 0;
}

int
finder_step(Finder *f, int budget)
{
    if (!f || !f->running)
        return 0;

    while (budget-- > 0) {
        FinderDir *top;
        struct dirent *de;
        char joined[FINDER_PATH_MAX];
        struct stat sb;

        if (f->depth == 0) {
            finder_finish(f);
            return 0;
        }
        top = &f->stack[f->depth - 1];
        errno = 0;
        de = readdir(top->dh);
        if (!de) {
            closedir(top->dh);
            top->dh = NULL;
            f->depth--;
            continue;
        }
        if (de->d_name[0] == '.')
            continue;
        if (strlen(top->path) + 1 + strlen(de->d_name) >= sizeof joined)
            continue;
        strcpy(joined, top->path);
        if (strcmp(top->path, "/") != 0)
            strcat(joined, "/");
        strcat(joined, de->d_name);

        /* lstat: a symlink to a directory is seen as a link and skipped. */
        if (lstat(joined, &sb) != 0)
            continue;
        if (S_ISDIR(sb.st_mode)) {
            finder_push_dir(f, joined); /* unreadable or too deep: skip */
            continue;
        }
        /* A symlink to an audio file still counts, as playlist_add follows it. */
        if (stat(joined, &sb) != 0 || !S_ISREG(sb.st_mode))
            continue;
        if (!f->filter(de->d_name))
            continue;
        /* Stops at the cap or when memory runs out; either way the list is
         * incomplete and finder_truncated / finder_out_of_memory say which. */
        if (finder_add_result(f, joined) != 0) {
            finder_finish(f);
            return 0;
        }
    }
    return f->running;
}

void
finder_cancel(Finder *f)
{
    if (!f || !f->running)
        return;
    finder_finish(f);
}

int
finder_running(const Finder *f)
{
    return f ? f->running : 0;
}

int
finder_count(const Finder *f)
{
    return f ? f->count : 0;
}

const char *
finder_path_at(const Finder *f, int index)
{
    if (!f || index < 0 || index >= f->count)
        return NULL;
    return f->paths[index];
}

int
finder_truncated(const Finder *f)
{
    return f ? f->truncated : 0;
}

int
finder_out_of_memory(const Finder *f)
{
    return f ? f->out_of_memory : 0;
}
