/* test_settings.c — round-trip checks for src/settings.c. The Makefile runs this
 * with XDG_CONFIG_HOME pointed at build/test-xdg, so the real user config is
 * never touched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

int
main(void)
{
    const char *root = getenv("XDG_CONFIG_HOME");
    char path[1024];
    int failures = 0;

    if (!root || !root[0]) {
        fprintf(stderr, "test_settings: XDG_CONFIG_HOME must be set\n");
        return 1;
    }

    /* Nothing saved yet: default is LED. Clear the file under the same root the
     * code reads, so a stray XDG_CONFIG_HOME can never touch the real config. */
    snprintf(path, sizeof path, "%s/aldrem/settings.ini", root);
    remove(path);
    if (settings_meter_style() != METER_LED) {
        fprintf(stderr, "FAIL: default should be LED\n");
        failures++;
    }

    if (settings_set_meter_style(METER_NEEDLE) != 0) {
        fprintf(stderr, "FAIL: save needle\n");
        failures++;
    }
    if (settings_meter_style() != METER_NEEDLE) {
        fprintf(stderr, "FAIL: needle did not round-trip\n");
        failures++;
    }

    if (settings_set_meter_style(METER_LED) != 0) {
        fprintf(stderr, "FAIL: save led\n");
        failures++;
    }
    if (settings_meter_style() != METER_LED) {
        fprintf(stderr, "FAIL: led did not round-trip\n");
        failures++;
    }

    /* Integer keys: defaults when missing, round-trip when saved. */
    if (settings_get_int("viz_bars", 42) != 42) {
        fprintf(stderr, "FAIL: missing int key should give default\n");
        failures++;
    }
    if (settings_set_int("viz_bars", 64) != 0 || settings_get_int("viz_bars", 0) != 64) {
        fprintf(stderr, "FAIL: int key did not round-trip\n");
        failures++;
    }
    if (settings_set_int("viz_gain", -20) != 0 || settings_get_int("viz_gain", 0) != -20) {
        fprintf(stderr, "FAIL: negative int did not round-trip\n");
        failures++;
    }

    /* Setting one key must not drop another, in either direction. */
    if (settings_set_meter_style(METER_NEEDLE) != 0 || settings_set_int("viz_hue", 30) != 0) {
        fprintf(stderr, "FAIL: could not write two keys\n");
        failures++;
    }
    if (settings_meter_style() != METER_NEEDLE || settings_get_int("viz_hue", 0) != 30) {
        fprintf(stderr, "FAIL: second key write dropped the first\n");
        failures++;
    }
    if (settings_set_meter_style(METER_LED) != 0 || settings_get_int("viz_hue", 0) != 30) {
        fprintf(stderr, "FAIL: meter write dropped the int key\n");
        failures++;
    }

    /* A hand-edited, non-numeric value falls back to the default. */
    if (settings_set_int("viz_mode", 1) != 0) {
        fprintf(stderr, "FAIL: save mode\n");
        failures++;
    }
    {
        FILE *f = fopen(path, "a");
        if (!f) {
            fprintf(stderr, "FAIL: open settings for append\n");
            failures++;
        } else {
            fprintf(f, "viz_smooth=lots\n");
            fclose(f);
        }
    }
    if (settings_get_int("viz_smooth", 7) != 7) {
        fprintf(stderr, "FAIL: non-numeric value should give default\n");
        failures++;
    }

    /* A value past int range keeps the default instead of being truncated. */
    {
        FILE *f = fopen(path, "a");
        if (!f) {
            fprintf(stderr, "FAIL: open settings for append\n");
            failures++;
        } else {
            fprintf(f, "viz_hue=4294967300\n");
            fclose(f);
        }
    }
    if (settings_get_int("viz_hue", 9) != 9) {
        fprintf(stderr, "FAIL: out-of-range value should give default\n");
        failures++;
    }

    /* A line longer than the read buffer survives a save untouched. */
    {
        char longline[300];
        char back[512];
        FILE *f;
        int found = 0;

        memset(longline, 'x', sizeof longline - 1);
        longline[sizeof longline - 1] = '\0';
        memcpy(longline, "# ", 2);
        f = fopen(path, "a");
        if (!f) {
            fprintf(stderr, "FAIL: open settings for append\n");
            failures++;
        } else {
            fprintf(f, "%s\n", longline);
            fclose(f);
        }
        if (settings_set_int("viz_bars", 16) != 0) {
            fprintf(stderr, "FAIL: save after long line\n");
            failures++;
        }
        f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "FAIL: reopen settings\n");
            failures++;
        } else {
            while (fgets(back, sizeof back, f)) {
                back[strcspn(back, "\n")] = '\0';
                if (strcmp(back, longline) == 0)
                    found = 1;
            }
            fclose(f);
            if (!found) {
                fprintf(stderr, "FAIL: long line was altered by save\n");
                failures++;
            }
        }
    }

    if (failures) {
        fprintf(stderr, "test_settings: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_settings: ok\n");
    return 0;
}
