/* settings.h — persisted user preferences (currently just the meter style).
 * Stored as $XDG_CONFIG_HOME/aldrem/settings.ini, falling back to
 * $HOME/.config/aldrem/. Pure file I/O, no SDL, so it is unit-testable. */
#ifndef SETTINGS_H
#define SETTINGS_H

typedef enum { METER_LED = 0, METER_NEEDLE = 1 } MeterStyle;

/* Reads the saved meter style. A missing or unreadable file, or an unknown
 * value, yields METER_LED. Never fails. */
MeterStyle settings_meter_style(void);

/* Writes the meter style. Returns 0 on success, -1 on failure. The write goes
 * to a temp file first so a crash cannot leave a half-written settings file. */
int settings_set_meter_style(MeterStyle style);

#endif
