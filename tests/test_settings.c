/* test_settings.c — round-trip checks for src/settings.c. The Makefile runs this
 * with XDG_CONFIG_HOME pointed at build/test-xdg, so the real user config is
 * never touched. */
#include <stdio.h>
#include <stdlib.h>

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

    if (failures) {
        fprintf(stderr, "test_settings: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_settings: ok\n");
    return 0;
}
