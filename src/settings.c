/* settings.c — persisted preferences. See settings.h. */
#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

MeterStyle
settings_meter_style(void)
{
    char path[SET_PATH_MAX];
    char line[SET_LINE_MAX];
    MeterStyle style = METER_LED;
    FILE *f;

    if (settings_path(path, sizeof path, 0) != 0)
        return style;
    f = fopen(path, "r");
    if (!f)
        return style;

    while (fgets(line, sizeof line, f)) {
        /* Trim the trailing newline before comparing the value. */
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "meter_style=needle") == 0)
            style = METER_NEEDLE;
        else if (strcmp(line, "meter_style=led") == 0)
            style = METER_LED;
    }
    fclose(f);
    return style;
}

int
settings_set_meter_style(MeterStyle style)
{
    char path[SET_PATH_MAX];
    char tmp[SET_PATH_MAX + 8];
    FILE *f;
    int bad = 0;

    if (settings_path(path, sizeof path, 1) != 0)
        return -1;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;

    f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "meter_style=%s\n", style == METER_NEEDLE ? "needle" : "led");
    if (ferror(f))
        bad = 1;
    if (fclose(f) != 0)
        bad = 1;
    if (bad || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}
