/* playlist.c — the track queue: a growable path list, .m3u read/write, atomic
 * autosave under the XDG config dir, and the playback transitions that follow
 * from editing it.
 *
 * Everything here runs on the UI thread. The model owns `current` and
 * `selected` so the Remove button always matches the drawn highlight, and every
 * entry mutation rewrites the autosave file. Playback never blocks on the list:
 * loads go through playback_load, which opens the candidate before touching the
 * live session.
 */
/* realpath() is POSIX/XSI, not ISO C: -std=c99 hides it from glibc's headers
 * unless a feature macro asks for it, which is why this is before any include. */
#define _XOPEN_SOURCE 700
#include "playlist.h"
#include "playback.h"
#include "track.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>

#define PL_PATH_MAX    4096
#define PL_MAX_ENTRIES 10000

struct Playlist {
    char **paths;
    int count;
    int cap;
    int current;
    int selected;
    char autosave[PL_PATH_MAX + 16]; /* empty when no config dir is usable */
};

/* Session-only Save-as prefill; the API takes no Playlist, and the app has one
 * instance. */
static char g_last_file[PL_PATH_MAX];

/* strdup is POSIX, not C99; this keeps -std=c99 pedantry quiet. */
static char *
dup_string(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);

    if (p)
        memcpy(p, s, n);
    return p;
}

static void
free_paths(char **paths, int count)
{
    int i;

    if (!paths)
        return;
    for (i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
}

/* Takes ownership of path; frees it on failure. */
static int
paths_push(char ***arr, int *count, int *cap, char *path)
{
    if (*count >= PL_MAX_ENTRIES) {
        free(path);
        return -1;
    }
    if (*count >= *cap) {
        int next = *cap ? *cap * 2 : 16;
        char **grown;

        if (next > PL_MAX_ENTRIES)
            next = PL_MAX_ENTRIES;
        if (next <= *cap) {
            free(path);
            return -1;
        }
        grown = (char **)realloc(*arr, sizeof *grown * (size_t)next);
        if (!grown) {
            free(path);
            return -1;
        }
        *arr = grown;
        *cap = next;
    }
    (*arr)[(*count)++] = path;
    return 0;
}

/* Case-insensitive extension test; mirrors track.c's ext_is without sharing a
 * private helper. */
static int
pl_ext_is(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    size_t i = 0;

    if (!dot)
        return 0;
    ++dot;
    while (dot[i] && ext[i]) {
        char c = dot[i];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != ext[i])
            return 0;
        ++i;
    }
    return dot[i] == '\0' && ext[i] == '\0';
}

int
playlist_ext_supported(const char *name)
{
    if (!name)
        return 0;
    return pl_ext_is(name, "m3u") || pl_ext_is(name, "m3u8");
}

/* Leading "~" and "~/" expand to $HOME; anything else is copied verbatim. */
static int
pl_expand_home(char *out, size_t outlen, const char *in)
{
    const char *home;
    int n;

    if (in[0] != '~' || (in[1] != '\0' && in[1] != '/')) {
        n = snprintf(out, outlen, "%s", in);
        return (n > 0 && (size_t)n < outlen) ? 0 : -1;
    }
    home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    n = snprintf(out, outlen, "%s%s", home, in + 1);
    return (n > 0 && (size_t)n < outlen) ? 0 : -1;
}

static int
pl_join_path(char *out, size_t outlen, const char *base, const char *name)
{
    int n;

    if (name[0] == '/')
        n = snprintf(out, outlen, "%s", name);
    else if (base[0])
        n = snprintf(out, outlen, "%s/%s", base, name);
    else
        n = snprintf(out, outlen, "%s", name);
    return (n > 0 && (size_t)n < outlen) ? 0 : -1;
}

/* Absolute directory holding file, so relative .m3u entries resolve against
 * the list itself rather than the process cwd. */
static void
pl_playlist_dir(char *out, size_t outlen, const char *file)
{
    const char *slash = strrchr(file, '/');
    char dir[PL_PATH_MAX];
    char cwd[PL_PATH_MAX];
    size_t n;

    if (!slash) {
        snprintf(dir, sizeof dir, ".");
    } else if (slash == file) {
        snprintf(dir, sizeof dir, "/");
    } else {
        n = (size_t)(slash - file);
        if (n >= sizeof dir)
            n = sizeof dir - 1;
        memcpy(dir, file, n);
        dir[n] = '\0';
    }

    if (dir[0] == '/') {
        snprintf(out, outlen, "%s", dir);
        return;
    }
    if (!getcwd(cwd, sizeof cwd)) {
        snprintf(out, outlen, "%s", dir);
        return;
    }
    if (strcmp(dir, ".") == 0)
        snprintf(out, outlen, "%s", cwd);
    else
        pl_join_path(out, outlen, cwd, dir);
}

/* Reads file into a freshly allocated array; on failure err is set and the
 * caller keeps whatever it had. Blank and '#' lines are ignored, so other
 * players' #EXTINF records cost nothing. */
static int
pl_read_m3u(const char *file, char ***out_paths, int *out_count,
    char *err, size_t errlen)
{
    FILE *f;
    char **paths = NULL;
    int count = 0, cap = 0;
    char base[PL_PATH_MAX];
    char line[PL_PATH_MAX];
    int ok = 1;

    f = fopen(file, "r");
    if (!f) {
        if (err && errlen)
            snprintf(err, errlen, "cannot open playlist file: %s", strerror(errno));
        return -1;
    }

    pl_playlist_dir(base, sizeof base, file);

    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        char *s, *end;

        /* An overlong line is not a path: drop the remainder instead of
         * treating its tail as another entry. */
        if (n == sizeof line - 1 && line[n - 1] != '\n' && !feof(f)) {
            int c;

            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
            SDL_Log("playlist: skipping overlong line in %s", file);
            continue;
        }

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        s = line;
        while (*s == ' ' || *s == '\t')
            ++s;
        end = s + strlen(s);
        while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (!*s || *s == '#')
            continue;

        {
            char expanded[PL_PATH_MAX];
            char joined[PL_PATH_MAX];
            char *copy;

            if (pl_expand_home(expanded, sizeof expanded, s) != 0) {
                SDL_Log("playlist: cannot expand ~ in %s", s);
                continue;
            }
            if (pl_join_path(joined, sizeof joined, base, expanded) != 0) {
                SDL_Log("playlist: path too long, skipped");
                continue;
            }
            copy = dup_string(joined);
            if (!copy || paths_push(&paths, &count, &cap, copy) != 0) {
                if (err && errlen)
                    snprintf(err, errlen, "out of memory");
                ok = 0;
                break;
            }
        }
    }

    if (ok && ferror(f)) {
        if (err && errlen)
            snprintf(err, errlen, "cannot read playlist file");
        ok = 0;
    }
    fclose(f);

    if (!ok) {
        free_paths(paths, count);
        return -1;
    }
    *out_paths = paths;
    *out_count = count;
    return 0;
}

static int
pl_write_m3u(const char *file, char *const *paths, int count,
    char *err, size_t errlen)
{
    FILE *f = fopen(file, "w");
    int bad = 0;
    int i;

    if (!f) {
        if (err && errlen)
            snprintf(err, errlen, "cannot write playlist: %s", strerror(errno));
        return -1;
    }

    fputs("#EXTM3U\n", f);
    for (i = 0; i < count; i++)
        fprintf(f, "%s\n", paths[i]);
    if (ferror(f))
        bad = 1;
    if (fflush(f) != 0)
        bad = 1;
    if (fclose(f) != 0)
        bad = 1;

    if (bad) {
        if (err && errlen)
            snprintf(err, errlen, "cannot write playlist: %s", strerror(errno));
        remove(file);
        return -1;
    }
    return 0;
}

static int
pl_ensure_dir(const char *path)
{
    if (mkdir(path, 0700) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

/* Autosave target: $XDG_CONFIG_HOME/aldrem/playlist.m3u, else
 * $HOME/.config/... Leaves autosave empty when no config dir can be made. */
static void
playlist_resolve_autosave(Playlist *pl)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char root[PL_PATH_MAX];
    char dir[PL_PATH_MAX];
    int n;

    if (base && base[0])
        n = snprintf(root, sizeof root, "%s", base);
    else if (home && home[0])
        n = snprintf(root, sizeof root, "%s/.config", home);
    else
        return;
    if (n <= 0 || (size_t)n >= sizeof root)
        return;

    n = snprintf(dir, sizeof dir, "%s/aldrem", root);
    if (n <= 0 || (size_t)n >= sizeof dir)
        return;

    if (pl_ensure_dir(dir) != 0 && errno == ENOENT) {
        pl_ensure_dir(root); /* XDG dir may not exist yet under a temp path */
        pl_ensure_dir(dir);
    }
    if (pl_ensure_dir(dir) != 0) {
        SDL_Log("playlist: cannot create %s: %s", dir, strerror(errno));
        return;
    }

    n = snprintf(pl->autosave, sizeof pl->autosave, "%s/playlist.m3u", dir);
    if (n <= 0 || (size_t)n >= sizeof pl->autosave)
        pl->autosave[0] = '\0';
}

static void
playlist_autosave(Playlist *pl)
{
    char tmp[sizeof pl->autosave + 8];
    char err[256];

    if (!pl->autosave[0])
        return;

    /* Same directory, then rename: a crash mid-write never damages the list. */
    snprintf(tmp, sizeof tmp, "%s.tmp", pl->autosave);
    if (pl_write_m3u(tmp, pl->paths, pl->count, err, sizeof err) != 0) {
        SDL_Log("playlist autosave failed: %s", err);
        remove(tmp);
        return;
    }
    if (rename(tmp, pl->autosave) != 0) {
        SDL_Log("playlist autosave rename failed: %s", strerror(errno));
        remove(tmp);
    }
}

static void
playlist_restore(Playlist *pl)
{
    char **paths = NULL;
    int count = 0;
    char err[256];

    if (!pl->autosave[0] || access(pl->autosave, R_OK) != 0)
        return; /* absent is the normal first-run case */

    if (pl_read_m3u(pl->autosave, &paths, &count, err, sizeof err) != 0) {
        SDL_Log("playlist restore failed: %s", err);
        return;
    }

    free_paths(pl->paths, pl->count);
    pl->paths = paths;
    pl->count = count;
    pl->cap = count;
}

Playlist *
playlist_create(void)
{
    Playlist *pl = (Playlist *)calloc(1, sizeof *pl);

    if (!pl)
        return NULL;
    pl->current = -1;
    pl->selected = -1;
    playlist_resolve_autosave(pl);
    playlist_restore(pl);
    return pl;
}

void
playlist_destroy(Playlist *pl)
{
    if (!pl)
        return;
    playlist_autosave(pl);
    free_paths(pl->paths, pl->count);
    free(pl);
}

int
playlist_count(const Playlist *pl)
{
    return pl ? pl->count : 0;
}

const char *
playlist_path_at(const Playlist *pl, int index)
{
    if (!pl || index < 0 || index >= pl->count)
        return NULL;
    return pl->paths[index];
}

int
playlist_current(const Playlist *pl)
{
    return pl ? pl->current : -1;
}

int
playlist_selected(const Playlist *pl)
{
    return pl ? pl->selected : -1;
}

void
playlist_select(Playlist *pl, int index)
{
    if (!pl)
        return;
    pl->selected = (index >= 0 && index < pl->count) ? index : -1;
}

/* Shared loader. follow moves the highlight with playback (explicit user
 * actions); auto-advance leaves a selection the user made elsewhere alone. */
static int
pl_load(Playlist *pl, int index, int follow)
{
    char err[256];

    if (!pl || index < 0 || index >= pl->count)
        return -1;
    if (playback_load(pl->paths[index], err, sizeof err) != 0) {
        SDL_Log("playlist: cannot play %s: %s", pl->paths[index], err);
        return -1;
    }
    pl->current = index;
    if (follow)
        pl->selected = index;
    return 0;
}

int
playlist_play_index(Playlist *pl, int index)
{
    return pl_load(pl, index, 1);
}

int
playlist_next(Playlist *pl)
{
    int next;

    if (!pl || pl->count == 0)
        return -1;
    next = pl->current + 1;
    if (next >= pl->count)
        return -1; /* last row: Next is a no-op */
    return pl_load(pl, next, 1);
}

int
playlist_prev(Playlist *pl)
{
    if (!pl || pl->count == 0)
        return -1;
    if (pl->current <= 0) {
        if (pl->current == 0)
            return playback_seek(0.0); /* first row restarts the track */
        return pl_load(pl, 0, 1);
    }
    return pl_load(pl, pl->current - 1, 1);
}

void
playlist_track_finished(Playlist *pl)
{
    int i, start;

    if (!pl)
        return;
    start = pl->current < 0 ? 0 : pl->current + 1;
    for (i = start; i < pl->count; i++) {
        if (pl_load(pl, i, 0) == 0)
            return;
    }
    /* End of the list, or nothing loadable: stay stopped, no wrap. */
}

int
playlist_add(Playlist *pl, const char *path)
{
    PlaybackStatus st;
    struct stat sb;
    char resolved[PL_PATH_MAX];
    char *copy;
    int idle, index;

    if (!pl || !path || !path[0])
        return 0;
    if (strlen(path) >= PL_PATH_MAX) {
        SDL_Log("playlist: path too long, skipped");
        return 0;
    }
    /* A newline would corrupt the line-based autosave format. */
    if (strchr(path, '\n') || strchr(path, '\r')) {
        SDL_Log("playlist: path contains a newline, skipped");
        return 0;
    }
    if (stat(path, &sb) != 0) {
        SDL_Log("playlist: cannot add %s: %s", path, strerror(errno));
        return 0;
    }
    if (!S_ISREG(sb.st_mode)) {
        SDL_Log("playlist: not a regular file: %s", path);
        return 0;
    }
    if (!track_ext_supported(path)) {
        SDL_Log("playlist: unsupported file: %s", path);
        return 0;
    }
    /* Entries are stored absolute so the autosave restores from any cwd. */
    if (!realpath(path, resolved)) {
        SDL_Log("playlist: cannot resolve %s: %s", path, strerror(errno));
        return 0;
    }

    /* Already listed: never add it twice. An idle player still starts it, so a
     * CLI arg or drop of a known track keeps its autoplay. This runs before the
     * capacity check so a full list can still start a known track. */
    for (index = 0; index < pl->count; index++) {
        const char *have = playlist_path_at(pl, index);

        if (have && strcmp(have, resolved) == 0) {
            playback_get_status(&st);
            if (st.state == MP_STOPPED)
                playlist_play_index(pl, index);
            return 1;
        }
    }

    if (pl->count >= PL_MAX_ENTRIES) {
        SDL_Log("playlist: list is full (%d entries)", PL_MAX_ENTRIES);
        return 0;
    }

    playback_get_status(&st);
    idle = st.state == MP_STOPPED;

    copy = dup_string(resolved);
    if (!copy || paths_push(&pl->paths, &pl->count, &pl->cap, copy) != 0) {
        SDL_Log("playlist: out of memory");
        return 0;
    }
    index = pl->count - 1;
    playlist_autosave(pl);

    /* Appending during playback must never interrupt it; idle adds start the
     * new entry so a drop or CLI arg still plays. */
    if (idle)
        playlist_play_index(pl, index);
    return 1;
}

void
playlist_remove_selected(Playlist *pl)
{
    int i, index;

    if (!pl || pl->selected < 0 || pl->selected >= pl->count)
        return;

    index = pl->selected;
    free(pl->paths[index]);
    for (i = index; i < pl->count - 1; i++)
        pl->paths[i] = pl->paths[i + 1];
    pl->count--;
    pl->paths[pl->count] = NULL;

    if (pl->current == index) {
        playback_stop();
        pl->current = -1;
    } else if (pl->current > index) {
        pl->current--;
    }

    if (pl->count == 0)
        pl->selected = -1;
    else if (index >= pl->count)
        pl->selected = pl->count - 1;
    else
        pl->selected = index;

    playlist_autosave(pl);
}

void
playlist_clear(Playlist *pl)
{
    if (!pl)
        return;
    playback_stop();
    free_paths(pl->paths, pl->count);
    pl->paths = NULL;
    pl->count = 0;
    pl->cap = 0;
    pl->current = -1;
    pl->selected = -1;
    playlist_autosave(pl);
}

int
playlist_save_as(Playlist *pl, const char *path, char *err, size_t errlen)
{
    char expanded[PL_PATH_MAX];

    if (err && errlen)
        err[0] = '\0';
    if (!pl || !path || !path[0]) {
        if (err && errlen)
            snprintf(err, errlen, "no file name given");
        return -1;
    }
    if (pl_expand_home(expanded, sizeof expanded, path) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "cannot resolve path");
        return -1;
    }
    if (pl_write_m3u(expanded, pl->paths, pl->count, err, errlen) != 0)
        return -1;

    /* Save-as never moves the autosave target; it only seeds the prefill. */
    snprintf(g_last_file, sizeof g_last_file, "%s", expanded);
    return 0;
}

int
playlist_load_file(Playlist *pl, const char *path, char *err, size_t errlen)
{
    char **paths = NULL;
    int count = 0;

    if (err && errlen)
        err[0] = '\0';
    if (!pl || !path || !path[0]) {
        if (err && errlen)
            snprintf(err, errlen, "no file name given");
        return -1;
    }
    if (pl_read_m3u(path, &paths, &count, err, errlen) != 0)
        return -1;

    /* The loaded list replaces the queue, so the old current row is gone. */
    playback_stop();
    free_paths(pl->paths, pl->count);
    pl->paths = paths;
    pl->count = count;
    pl->cap = count;
    pl->current = -1;
    pl->selected = -1;

    snprintf(g_last_file, sizeof g_last_file, "%s", path);
    playlist_autosave(pl);
    return 0;
}

const char *
playlist_last_file(void)
{
    return g_last_file;
}
