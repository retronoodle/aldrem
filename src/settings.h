/* settings.h — persisted user preferences: the meter style plus a generic
 * key=value store for the visualizer and other UI state.
 * Stored as $XDG_CONFIG_HOME/aldrem/settings.ini, falling back to
 * $HOME/.config/aldrem/. Pure file I/O, no SDL, so it is unit-testable. */
#ifndef SETTINGS_H
#define SETTINGS_H

typedef enum { METER_LED = 0, METER_NEEDLE = 1 } MeterStyle;

/* Reads the saved meter style. A missing or unreadable file, or an unknown
 * value, yields METER_LED. Never fails. */
MeterStyle settings_meter_style(void);

/* Writes the meter style. Returns 0 on success, -1 on failure. The write goes
 * to a temp file first so a crash cannot leave a half-written settings file.
 * Other keys already in the file are preserved. */
int settings_set_meter_style(MeterStyle style);

/* Generic integer keys. Get returns def when the key is missing or not a
 * clean integer. Set keeps every other key and returns 0 or -1. */
int settings_get_int(const char *key, int def);
int settings_set_int(const char *key, int value);

#endif
