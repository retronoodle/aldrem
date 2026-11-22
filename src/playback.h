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
    int    volume_percent; /* 100 this phase */
    char   name[512];      /* loaded path, empty when nothing is loaded */
    char   error[512];     /* last load/start error, empty on success */
} PlaybackStatus;

int  playback_init(void); /* SDL_OpenAudioDevice, ring alloc */
void playback_shutdown(void);

/* Stops any current session, opens path and starts playing it.
 * Returns 0 on success; on failure writes a human-readable reason into err
 * and leaves the engine stopped with no track loaded. */
int  playback_load(const char *path, char *err, size_t errlen);

void playback_play(void);
void playback_pause(void);
void playback_stop(void);

/* Moves the playhead to seconds, clamped to [0, duration]. Seeking while
 * playing or paused rebuilds the session; while stopped it only sets the point
 * Play will start at. Returns 0 on success, -1 if there is no track. */
int  playback_seek(double seconds);
/* Seek from the current position; see playback_seek. */
int  playback_seek_relative(double delta);

void playback_set_volume(int percent);
void playback_get_status(PlaybackStatus *out);

#endif
