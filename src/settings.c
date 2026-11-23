/* settings.c — persisted preferences. See settings.h. */
#define _POSIX_C_SOURCE 200809L /* fileno, fsync */

#include "settings.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SET_PATH_MAX 1024
#define SET_LINE_MAX 128

static int
set_mkdir(const char *path)
{
    if (mkdir(path, 0700) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

/* Resolves $XDG_CONFIG_HOME/aldrem/settings.ini (or the $HOME fallback) and
 * makes the aldrem directory. Returns -1 when no config dir can be formed. */
static int
settings_path(char *out, size_t outlen, int create)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char root[SET_PATH_MAX];
    char dir[SET_PATH_MAX];
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
        set_mkdir(root);
        if (set_mkdir(dir) != 0)
            return -1;
    }

    n = snprintf(out, outlen, "%s/settings.ini", dir);
    if (n <= 0 || (size_t)n >= outlen)
        return -1;
    return 0;
}

/* Reads the value for key into out. Returns 1 when found, 0 otherwise. */
static int
kv_get(const char *key, char *out, size_t outlen)
{
    char path[SET_PATH_MAX];
    char line[SET_LINE_MAX];
    size_t klen = strlen(key);
    int found = 0;
    FILE *f;

    if (settings_path(path, sizeof path, 0) != 0)
        return 0;
    f = fopen(path, "r");
    if (!f)
        return 0;

    while (fgets(line, sizeof line, f)) {
        /* Trim the trailing newline before comparing the value. */
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            snprintf(out, outlen, "%s", line + klen + 1);
            found = 1; /* last occurrence wins */
        }
    }
    fclose(f);
    return found;
}

/* Sets key to value, keeping every other line in the file. Written through a
 * temp file and renamed, so a crash cannot leave a half-written settings file.
 * Returns 0 on success, -1 on failure. */
static int
kv_set(const char *key, const char *value)
{
    char path[SET_PATH_MAX];
    char tmp[SET_PATH_MAX + 8];
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    size_t klen = strlen(key);
    int bad = 0, wrote = 0;
    FILE *in, *f;

    if (settings_path(path, sizeof path, 1) != 0)
        return -1;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;

    f = fopen(tmp, "w");
    if (!f)
        return -1;

    /* Copy the existing file, replacing the key's line in place. */
    in = fopen(path, "r");
    if (in) {
        /* getline keeps long lines whole, so unrelated lines survive intact. */
        while ((len = getline(&line, &cap, in)) != -1) {
            if (len > 0 && line[len - 1] == '\n')
                line[len - 1] = '\0';
            if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
                if (!wrote)
                    fprintf(f, "%s=%s\n", key, value);
                wrote = 1;
            } else {
                fprintf(f, "%s\n", line);
            }
        }
        fclose(in);
    }
    free(line);
    if (!wrote)
        fprintf(f, "%s=%s\n", key, value);

    if (ferror(f))
        bad = 1;
    /* Flush to disk before the rename so a power cut cannot leave an empty
     * or truncated file under the real name. */
    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
        bad = 1;
    if (fclose(f) != 0)
        bad = 1;
    if (bad || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

MeterStyle
settings_meter_style(void)
{
    char v[SET_LINE_MAX];

    if (kv_get("meter_style", v, sizeof v)) {
        if (strcmp(v, "needle") == 0)
            return METER_NEEDLE;
    }
    return METER_LED;
}

int
settings_set_meter_style(MeterStyle style)
{
    return kv_set("meter_style", style == METER_NEEDLE ? "needle" : "led");
}

int
settings_get_int(const char *key, int def)
{
    char v[SET_LINE_MAX];
    char *end;
    long n;

    if (!kv_get(key, v, sizeof v))
        return def;
    errno = 0;
    n = strtol(v, &end, 10);
    /* Out of long or int range is not a usable value; keep the default
     * rather than truncating it. */
    if (end == v || *end != '\0' || errno == ERANGE)
        return def;
    if (n < INT_MIN || n > INT_MAX)
        return def;
    return (int)n;
}

int
settings_set_int(const char *key, int value)
{
    char v[32];

    snprintf(v, sizeof v, "%d", value);
    return kv_set(key, v);
}
