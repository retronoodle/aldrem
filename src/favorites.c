/* favorites.c — starred radio stations. See favorites.h. */
#define _POSIX_C_SOURCE 200809L /* getline, fileno, fsync */

#include "favorites.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FAV_PATH_MAX 1024
#define FAV_BITRATE_MAX 100000 /* larger saved values read back as 0 */

static RadioStation favs[FAV_MAX];
static RadioStation fav_next[FAV_MAX]; /* candidate list, committed on save */
static int fav_n;
static int fav_loaded;
/* Set when the saved file exists but could not be read. Saving then would
 * overwrite it with a partial list, so toggles are refused until a load works. */
static int fav_load_failed;

static int
fav_mkdir(const char *path)
{
    if (mkdir(path, 0700) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

/* Resolves $XDG_CONFIG_HOME/aldrem/favorites.tsv (or the $HOME fallback).
 * With create set, makes the aldrem directory. Returns -1 when no config dir
 * can be formed. */
static int
fav_path(char *out, size_t outlen, int create)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char root[FAV_PATH_MAX];
    char dir[FAV_PATH_MAX];
    int n;

    if (base && base[0])
        n = snprintf(root, sizeof root, "%s", base);
    else if (home && home[0])
        n = snprintf(root, sizeof root, "%s/.config", home);
    else
        return -1;
    if (n <= 0 || (size_t)n >= sizeof root)
        return -1;

    n = snprintf(dir, sizeof dir, "%s/aldrem", root);
    if (n <= 0 || (size_t)n >= sizeof dir)
        return -1;

    if (create) {
        /* The XDG root may not exist yet, e.g. under a fresh temp HOME. */
        fav_mkdir(root);
        if (fav_mkdir(dir) != 0)
            return -1;
    }

    n = snprintf(out, outlen, "%s/favorites.tsv", dir);
    if (n <= 0 || (size_t)n >= outlen)
        return -1;
    return 0;
}

/* Copies src into dst, turning tabs and line breaks into spaces so a field
 * can never split a record. Truncates to fit dst. */
static void
fav_copy(char *dst, size_t dstlen, const char *src)
{
    size_t i;

    for (i = 0; i + 1 < dstlen && src[i]; i++) {
        char c = src[i];

        dst[i] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    dst[i] = '\0';
}

static void
fav_fill(RadioStation *dst, const RadioStation *src)
{
    fav_copy(dst->name, sizeof dst->name, src->name);
    fav_copy(dst->url, sizeof dst->url, src->url);
    fav_copy(dst->codec, sizeof dst->codec, src->codec);
    fav_copy(dst->country, sizeof dst->country, src->country);
    /* Same range the parser accepts, so a saved value reads back unchanged. */
    dst->bitrate = (src->bitrate > 0 && src->bitrate <= FAV_BITRATE_MAX)
                       ? src->bitrate : 0;
}

/* Parses one saved line into st. Returns 0 on success, -1 when the line is
 * not five fields or has no URL. Modifies line. */
static int
fav_parse_line(char *line, RadioStation *st)
{
    char *fld[5];
    char *p = line;
    char *end = NULL;
    long bitrate;
    int n = 0;

    line[strcspn(line, "\r\n")] = '\0';
    while (n < 4) {
        char *t = strchr(p, '\t');

        if (!t)
            break;
        *t = '\0';
        fld[n++] = p;
        p = t + 1;
    }
    fld[n++] = p;
    if (n != 5)
        return -1;
    if (fld[1][0] == '\0')
        return -1;

    memset(st, 0, sizeof *st);
    snprintf(st->name, sizeof st->name, "%s", fld[0]);
    snprintf(st->url, sizeof st->url, "%s", fld[1]);
    snprintf(st->codec, sizeof st->codec, "%s", fld[2]);
    snprintf(st->country, sizeof st->country, "%s", fld[3]);

    errno = 0;
    bitrate = strtol(fld[4], &end, 10);
    if (end == fld[4] || errno != 0 || bitrate < 0 || bitrate > FAV_BITRATE_MAX)
        bitrate = 0;
    st->bitrate = (int)bitrate;
    return 0;
}

static int
fav_index(const char *url)
{
    int i;

    for (i = 0; i < fav_n; i++) {
        if (strcmp(favs[i].url, url) == 0)
            return i;
    }
    return -1;
}

int
favorites_load(void)
{
    char path[FAV_PATH_MAX];
    char *line = NULL;
    size_t cap = 0;
    FILE *f;

    fav_n = 0;
    fav_loaded = 1;
    fav_load_failed = 0;
    if (fav_path(path, sizeof path, 0) != 0)
        return 0;
    f = fopen(path, "r");
    if (!f) {
        /* No file (or no directory for it) just means no favorites yet. */
        if (errno != ENOENT && errno != ENOTDIR)
            fav_load_failed = 1;
        return 0;
    }

    while (getline(&line, &cap, f) != -1 && fav_n < FAV_MAX) {
        RadioStation st;

        if (fav_parse_line(line, &st) != 0)
            continue;
        if (fav_index(st.url) >= 0)
            continue;
        favs[fav_n++] = st;
    }
    if (ferror(f))
        fav_load_failed = 1;
    free(line);
    fclose(f);
    return fav_n;
}

static void
fav_ensure(void)
{
    if (!fav_loaded)
        favorites_load();
}

/* Writes list to a temp file, then renames it over the real one, so a crash
 * mid-write never damages the saved favorites. Returns 0 or -1. */
static int
fav_save(const RadioStation *list, int n)
{
    char path[FAV_PATH_MAX];
    char tmp[FAV_PATH_MAX + 32];
    FILE *f;
    int i;

    if (fav_path(path, sizeof path, 1) != 0)
        return -1;
    /* The PID keeps two running instances from writing the same temp file. */
    if (snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid()) >=
        (int)sizeof tmp)
        return -1;
    f = fopen(tmp, "w");
    if (!f)
        return -1;

    for (i = 0; i < n; i++) {
        fprintf(f, "%s\t%s\t%s\t%s\t%d\n", list[i].name, list[i].url,
                list[i].codec, list[i].country, list[i].bitrate);
    }
    if (fflush(f) != 0 || ferror(f) || fsync(fileno(f)) != 0) {
        fclose(f);
        remove(tmp);
        return -1;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int
favorites_count(void)
{
    fav_ensure();
    return fav_n;
}

const RadioStation *
favorites_at(int index)
{
    fav_ensure();
    if (index < 0 || index >= fav_n)
        return NULL;
    return &favs[index];
}

int
favorites_has(const char *url)
{
    fav_ensure();
    return url && fav_index(url) >= 0;
}

int
favorites_toggle(const RadioStation *st)
{
    int n, i, rc;

    fav_ensure();
    if (!st || st->url[0] == '\0' || fav_load_failed)
        return FAV_IO;

    n = fav_n;
    memcpy(fav_next, favs, sizeof favs[0] * (size_t)n);
    i = fav_index(st->url);
    if (i >= 0) {
        memmove(&fav_next[i], &fav_next[i + 1],
                sizeof fav_next[0] * (size_t)(n - i - 1));
        n--;
        rc = FAV_REMOVED;
    } else {
        if (n >= FAV_MAX)
            return FAV_FULL;
        fav_fill(&fav_next[n], st);
        n++;
        rc = FAV_ADDED;
    }

    if (fav_save(fav_next, n) != 0)
        return FAV_IO;
    memcpy(favs, fav_next, sizeof favs[0] * (size_t)n);
    fav_n = n;
    return rc;
}
