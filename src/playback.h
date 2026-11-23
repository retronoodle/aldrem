/* playback.h — audio engine API used by the UI. */
#ifndef PLAYBACK_H
#define PLAYBACK_H

#include <stddef.h>

typedef enum { MP_STOPPED, MP_PLAYING, MP_PAUSED } PlaybackState;

typedef struct {
    PlaybackState state;
    int    has_track;
    double position;       /* seconds */
    double duration;       /* seconds */
    int    volume_percent; /* 0..100 */
    char   name[512];      /* loaded path, empty when nothing is loaded */
    char   error[512];     /* last load/start error, empty on success */
    char   codec[8];       /* decoded format label ("WAV"/"MP3") */
    int    bits_per_sample; /* decoded width, 0 when not meaningful (MP3) */
    int    sample_rate;    /* source sample rate, Hz */
    int    channels;       /* source channel count */
} PlaybackStatus;

int  playback_init(void); /* SDL_OpenAudioDevice, ring alloc */
void playback_shutdown(void);

/* 1 when a device is open and volume/transport changes take effect. The UI
 * disables the volume row when this is 0 (no audio device). */
int  playback_available(void);

/* Opens path and, on success, replaces any current session with it and starts
 * playing from the beginning. Returns 0 on success. On failure writes a
 * human-readable reason into err, reports it through PlaybackStatus.error, and
 * leaves any current playback untouched. */
int  playback_load(const char *path, char *err, size_t errlen);

void playback_play(void);
void playback_pause(void);
void playback_stop(void);
/* Pause when playing, otherwise play. No-op with no loaded track. */
void playback_toggle(void);

/* Moves the playhead to seconds, clamped to [0, duration]. Seeking while
 * playing or paused rebuilds the session; while stopped it only sets the point
 * Play will start at. Returns 0 on success, -1 if there is no track. */
int  playback_seek(double seconds);
/* Seek from the current position; see playback_seek. */
int  playback_seek_relative(double delta);

void playback_set_volume(int percent);
/* Adds delta to the volume percentage, clamped to [0, 100]. */
void playback_adjust_volume(int delta);
void playback_get_status(PlaybackStatus *out);

#endif
