/* test_load.c — headless decoder round-trip. Opens an audio file through the
 * real playback engine and asserts a track is loaded with a known duration.
 * Exit status is the assertion result; linked by the Makefile `test` target. */
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "playback.h"

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <audio-file>\n", argv[0]);
        return 2;
    }

    /* playback_init opens the device but does not bring audio up; the app
     * does that at startup, so the test must too. */
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init audio failed: %s\n", SDL_GetError());
        return 1;
    }

    if (playback_init() != 0) {
        fprintf(stderr, "playback_init failed\n");
        return 1;
    }

    char err[256] = "";
    if (playback_load(argv[1], err, sizeof err) != 0) {
        fprintf(stderr, "playback_load(%s) failed: %s\n", argv[1],
            err[0] ? err : "unknown error");
        playback_shutdown();
        SDL_Quit();
        return 1;
    }

    PlaybackStatus st;
    memset(&st, 0, sizeof st);
    playback_get_status(&st);

    int failed = 0;
    if (!st.has_track) {
        fprintf(stderr, "expected has_track, got 0\n");
        failed = 1;
    }
    if (!(st.duration > 0.0)) {
        fprintf(stderr, "expected duration > 0, got %.3f\n", st.duration);
        failed = 1;
    }
    if (!failed) {
        printf("loaded %s: codec=%s %.3fs %d Hz %d ch\n", argv[1], st.codec,
            st.duration, st.sample_rate, st.channels);
    }

    playback_shutdown();
    SDL_Quit();
    return failed;
}
