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
#include "net.h"
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
    char **names;    /* parallel to paths; NULL entry = unnamed (a file) */
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
free_entries(char **paths, char **names, int count)
{
    int i;

    if (paths)
        for (i = 0; i < count; i++)
            free(paths[i]);
    if (names)
        for (i = 0; i < count; i++)
            free(names[i]);
    free(paths);
    free(names);
}

/* Appends one entry to the two parallel arrays, which always grow together.
 * Takes ownership of path and name (name may be NULL); frees both on failure. */
static int
entry_push(char ***paths, char ***names, int *count, int *cap,
    char *path, char *name)
{
    if (*count >= PL_MAX_ENTRIES) {
        free(path);
        free(name);
        return -1;
    }
    if (*count >= *cap) {
        int next = *cap ? *cap * 2 : 16;
        char **grown_p, **grown_n;

        if (next > PL_MAX_ENTRIES)
            next = PL_MAX_ENTRIES;
        if (next <= *cap) {
            free(path);
            free(name);
            return -1;
        }
        grown_p = (char **)realloc(*paths, sizeof *grown_p * (size_t)next);
        if (!grown_p) {
            free(path);
            free(name);
            return -1;
        }
        *paths = grown_p;
        grown_n = (char **)realloc(*names, sizeof *grown_n * (size_t)next);
        if (!grown_n) {
            free(path);
            free(name);
            return -1;
        }
        *names = grown_n;
        *cap = next;
    }
    (*paths)[*count] = path;
    (*names)[*count] = name;
    (*count)++;
    return 0;
}

/* Internet radio entries are stored as their URL. */
static int
pl_is_url(const char *s)
{
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
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
pl_read_m3u(const char *file, char ***out_paths, char ***out_names,
    int *out_count, char *err, size_t errlen)
{
    FILE *f;
    char **paths = NULL, **names = NULL;
    int count = 0, cap = 0;
    char base[PL_PATH_MAX];
    char line[PL_PATH_MAX];
    char *pending = NULL; /* name from the last #EXTINF, for the next entry */
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
        if (!*s)
            continue;
        if (*s == '#') {
            /* #EXTINF:-1,Name labels the entry on the next line. */
            if (strncmp(s, "#EXTINF:", 8) == 0) {
                const char *comma = strchr(s, ',');

                free(pending);
                pending = NULL;
                if (comma && comma[1]) {
                    pending = dup_string(comma + 1);
                    if (!pending) {
                        if (err && errlen)
                            snprintf(err, errlen, "out of memory");
                        ok = 0;
                        break;
                    }
                }
            }
            continue;
        }

        {
            char expanded[PL_PATH_MAX];
            char joined[PL_PATH_MAX];
            const char *target;
            char *copy, *name;

            if (pl_is_url(s)) {
                /* A stream URL is taken verbatim: no ~ or relative joining. */
                target = s;
            } else {
                if (pl_expand_home(expanded, sizeof expanded, s) != 0) {
                    SDL_Log("playlist: cannot expand ~ in %s", s);
                    free(pending);
                    pending = NULL;
                    continue;
                }
                if (pl_join_path(joined, sizeof joined, base, expanded) != 0) {
                    SDL_Log("playlist: path too long, skipped");
                    free(pending);
                    pending = NULL;
                    continue;
                }
                target = joined;
            }
            copy = dup_string(target);
            if (!copy) {
                if (err && errlen)
                    snprintf(err, errlen, "out of memory");
                ok = 0;
                break;
            }
            /* entry_push frees both strings on failure, so the name is handed
             * over before the call; pending is not touched again after this. */
            name = pending;
            pending = NULL;
            if (entry_push(&paths, &names, &count, &cap, copy, name) != 0) {
                if (err && errlen)
                    snprintf(err, errlen, "out of memory");
                ok = 0;
                break;
            }
        }
    }
    free(pending);

    if (ok && ferror(f)) {
        if (err && errlen)
            snprintf(err, errlen, "cannot read playlist file");
        ok = 0;
    }
    fclose(f);

    if (!ok) {
        free_entries(paths, names, count);
        return -1;
    }
    *out_paths = paths;
    *out_names = names;
    *out_count = count;
    return 0;
}

static int
pl_write_m3u(const char *file, char *const *paths, char *const *names,
    int count, char *err, size_t errlen)
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
    for (i = 0; i < count; i++) {
        const char *name = names ? names[i] : NULL;

        if (name && *name) {
            /* A line break in a name would split the record: flatten it. */
            fputs("#EXTINF:-1,", f);
            for (; *name; name++)
                fputc(*name == '\n' || *name == '\r' ? ' ' : *name, f);
            fputc('\n', f);
        }
        fprintf(f, "%s\n", paths[i]);
    }
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
    if (pl_write_m3u(tmp, pl->paths, pl->names, pl->count, err, sizeof err) != 0) {
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
    char **paths = NULL, **names = NULL;
    int count = 0;
    char err[256];

    if (!pl->autosave[0] || access(pl->autosave, R_OK) != 0)
        return; /* absent is the normal first-run case */

    if (pl_read_m3u(pl->autosave, &paths, &names, &count, err, sizeof err) != 0) {
        SDL_Log("playlist restore failed: %s", err);
        return;
    }

    free_entries(pl->paths, pl->names, pl->count);
    pl->paths = paths;
    pl->names = names;
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
    free_entries(pl->paths, pl->names, pl->count);
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

const char *
playlist_label_at(const Playlist *pl, int index)
{
    const char *name;

    if (!pl || index < 0 || index >= pl->count || !pl->names)
        return NULL;
    name = pl->names[index];
    return name && *name ? name : NULL;
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
        if (pl->current == 0) {
            /* A stream cannot seek: restarting it means reconnecting. Not
             * playback_get_status, which would consume a pending EOF. */
            if (playback_is_live())
                return pl_load(pl, 0, 1);
            return playback_seek(0.0); /* first row restarts the track */
        }
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
        /* A dead stream costs a blocking connect (seconds, on the UI thread),
         * so one failed station ends the advance rather than trying the rest. */
        if (pl_is_url(pl->paths[i]))
            break;
    }
    /* End of the list, or nothing loadable: stay stopped, no wrap. */
}

/* Validates a path and resolves it to the absolute form stored in the list.
 * Returns 1 with resolved filled in, 0 when rejected. Touches no list state. */
static int
pl_resolve(const char *path, char *resolved)
{
    struct stat sb;

    if (!path || !path[0])
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
    if (pl_is_url(path)) {
        /* Stations are not files: no stat or realpath, just the URL. */
        if (!net_available()) {
            SDL_Log("playlist: network streams are not available in this build");
            return 0;
        }
        snprintf(resolved, PL_PATH_MAX, "%s", path);
        return 1;
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
    return 1;
}

/* Validates and appends one path with no autosave or autoplay. Returns 0 when
 * rejected, 1 when added (*out_index set), 2 when already listed (*out_index
 * set). Callers decide persistence and playback. */
static int
pl_admit(Playlist *pl, const char *path, int *out_index)
{
    char resolved[PL_PATH_MAX];
    char *copy;
    int index;

    if (!pl || !pl_resolve(path, resolved))
        return 0;

    /* Already listed: never add it twice. An idle player still starts it, so a
     * CLI arg or drop of a known track keeps its autoplay. This runs before the
     * capacity check so a full list can still start a known track. */
    for (index = 0; index < pl->count; index++) {
        const char *have = playlist_path_at(pl, index);

        if (have && strcmp(have, resolved) == 0) {
            *out_index = index;
            return 2;
        }
    }

    if (pl->count >= PL_MAX_ENTRIES) {
        SDL_Log("playlist: list is full (%d entries)", PL_MAX_ENTRIES);
        return 0;
    }

    copy = dup_string(resolved);
    if (!copy || entry_push(&pl->paths, &pl->names, &pl->count, &pl->cap,
                            copy, NULL) != 0) {
        SDL_Log("playlist: out of memory");
        return 0;
    }
    *out_index = pl->count - 1;
    return 1;
}

/* Open-addressing set of path strings, used only by a batch add so dedupe is
 * one hash lookup per file instead of a scan of the whole list. Slots point at
 * strings the playlist owns; the set never copies or frees them. */
typedef struct {
    const char **slots;
    size_t cap; /* power of two, always at least twice the entries */
} PathSet;

static size_t
path_hash(const char *s)
{
    size_t h = 2166136261u;

    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static int
pathset_init(PathSet *set, size_t want)
{
    size_t cap = 16;

    while (cap < want * 2)
        cap <<= 1;
    set->slots = (const char **)calloc(cap, sizeof *set->slots);
    if (!set->slots)
        return -1;
    set->cap = cap;
    return 0;
}

/* Returns the slot holding path, or the empty slot it belongs in. */
static const char **
pathset_slot(const PathSet *set, const char *path)
{
    size_t i = path_hash(path) & (set->cap - 1);

    while (set->slots[i] && strcmp(set->slots[i], path) != 0)
        i = (i + 1) & (set->cap - 1);
    return &set->slots[i];
}

int
playlist_add(Playlist *pl, const char *path)
{
    PlaybackStatus st;
    int index, r;

    r = pl_admit(pl, path, &index);
    if (r == 0)
        return 0;
    if (r == 1)
        playlist_autosave(pl);

    /* Appending during playback must never interrupt it; idle adds start the
     * entry so a drop or CLI arg still plays, and a known track is restarted
     * the same way. */
    playback_get_status(&st);
    if (st.state == MP_STOPPED && !st.live_paused)
        playlist_play_index(pl, index);
    return 1;
}

/* Shared by the file and station batch adds. names may be NULL. */
static int
pl_add_batch(Playlist *pl, const char *const *paths, const char *const *names,
    int n, int *full)
{
    PlaybackStatus st;
    PathSet set;
    char resolved[PL_PATH_MAX];
    const char **slot;
    char *copy, *label;
    int i, index, added = 0, first = -1, dropped = 0;

    if (full)
        *full = 0;
    if (!pl || !paths || n <= 0)
        return 0;
    if (pathset_init(&set, (size_t)pl->count + (size_t)n) != 0) {
        SDL_Log("playlist: out of memory");
        return 0;
    }
    for (i = 0; i < pl->count; i++) {
        slot = pathset_slot(&set, pl->paths[i]);
        if (!*slot)
            *slot = pl->paths[i];
    }
    for (i = 0; i < n; i++) {
        if (!pl_resolve(paths[i], resolved))
            continue;
        slot = pathset_slot(&set, resolved);
        if (*slot)
            continue; /* already listed: skipped silently */
        if (pl->count >= PL_MAX_ENTRIES) {
            dropped++;
            continue;
        }
        copy = dup_string(resolved);
        label = names && names[i] ? dup_string(names[i]) : NULL;
        if (!copy || (names && names[i] && !label)) {
            free(copy);
            free(label);
            SDL_Log("playlist: out of memory");
            continue;
        }
        if (entry_push(&pl->paths, &pl->names, &pl->count, &pl->cap,
                       copy, label) != 0) {
            SDL_Log("playlist: out of memory");
            continue;
        }
        index = pl->count - 1;
        *slot = pl->paths[index];
        if (first < 0)
            first = index;
        added++;
    }
    free(set.slots);

    if (dropped > 0)
        SDL_Log("playlist: list is full (%d entries), %d skipped", PL_MAX_ENTRIES,
                dropped);
    if (full)
        *full = dropped;
    if (added == 0)
        return 0;

    /* One autosave for the whole batch, not one per file. */
    playlist_autosave(pl);
    playback_get_status(&st);
    if (st.state == MP_STOPPED && !st.live_paused)
        playlist_play_index(pl, first);
    return added;
}

int
playlist_add_many(Playlist *pl, const char *const *paths, int n, int *full)
{
    return pl_add_batch(pl, paths, NULL, n, full);
}

int
playlist_add_stations(Playlist *pl, const char *const *urls,
    const char *const *names, int n, int *full)
{
    return pl_add_batch(pl, urls, names, n, full);
}

void
playlist_remove_selected(Playlist *pl)
{
    int i, index;

    if (!pl || pl->selected < 0 || pl->selected >= pl->count)
        return;

    index = pl->selected;
    free(pl->paths[index]);
    free(pl->names[index]);
    for (i = index; i < pl->count - 1; i++) {
        pl->paths[i] = pl->paths[i + 1];
        pl->names[i] = pl->names[i + 1];
    }
    pl->count--;
    pl->paths[pl->count] = NULL;
    pl->names[pl->count] = NULL;

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
    free_entries(pl->paths, pl->names, pl->count);
    pl->paths = NULL;
    pl->names = NULL;
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
    if (pl_write_m3u(expanded, pl->paths, pl->names, pl->count, err, errlen) != 0)
        return -1;

    /* Save-as never moves the autosave target; it only seeds the prefill. */
    snprintf(g_last_file, sizeof g_last_file, "%s", expanded);
    return 0;
}

int
playlist_load_file(Playlist *pl, const char *path, char *err, size_t errlen)
{
    char **paths = NULL, **names = NULL;
    int count = 0;

    if (err && errlen)
        err[0] = '\0';
    if (!pl || !path || !path[0]) {
        if (err && errlen)
            snprintf(err, errlen, "no file name given");
        return -1;
    }
    if (pl_read_m3u(path, &paths, &names, &count, err, errlen) != 0)
        return -1;

    /* The loaded list replaces the queue, so the old current row is gone. */
    playback_stop();
    free_entries(pl->paths, pl->names, pl->count);
    pl->paths = paths;
    pl->names = names;
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
