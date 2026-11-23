/* test_playlist.c — the playlist's station entries: names kept in step with
 * paths through every mutator, #EXTINF round trips, and URL lines taken
 * verbatim. Runs against the real playlist.c with an isolated XDG config dir,
 * so the autosave file is exercised too. No audio device is opened.
 *
 * Usage: test_playlist <scratch-dir> <fixture-file>
 */
#define _POSIX_C_SOURCE 200112L /* setenv */
#include "net.h"
#include "playlist.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            failures++;                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                    \
            fprintf(stderr, "\n");                           \
        }                                                    \
    } while (0)

#define URL_A "https://radio.example.invalid/stream-a.mp3"
#define URL_B "http://radio.example.invalid:8000/stream-b"

static const char *
label_or_null(const Playlist *pl, int i)
{
    return playlist_label_at(pl, i);
}

static int
file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    char buf[4096];
    size_t n;
    int found = 0;

    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    found = strstr(buf, needle) != NULL;
    return found;
}

int
main(int argc, char **argv)
{
    const char *scratch = argc > 1 ? argv[1] : "build/test-playlist";
    const char *fixture = argc > 2 ? argv[2] : "tests/data/tone.mp3";
    char xdg[1024], autosave[1100], out[1100];
    Playlist *pl;
    const char *urls[2];
    const char *names[2];
    const char *files[1];
    int full = 0, added;

    snprintf(xdg, sizeof xdg, "%s/xdg", scratch);
    snprintf(autosave, sizeof autosave, "%s/aldrem/playlist.m3u", xdg);
    snprintf(out, sizeof out, "%s/saved.m3u", scratch);
    mkdir(scratch, 0700);
    mkdir(xdg, 0700);
    setenv("XDG_CONFIG_HOME", xdg, 1);
    remove(autosave);
    remove(out);

    pl = playlist_create();
    CHECK(pl != NULL, "playlist_create failed");
    if (!pl)
        return 1;
    CHECK(playlist_count(pl) == 0, "new playlist is not empty");

    if (!net_available()) {
        /* Without libcurl, stations are refused and the rest of the checks
         * still run on files. */
        fprintf(stderr, "test_playlist: no network build, station checks skipped\n");
    }

    /* A file, then two stations, named and unnamed mixes. */
    files[0] = fixture;
    added = playlist_add_many(pl, files, 1, &full);
    CHECK(added == 1, "file add: got %d", added);
    CHECK(label_or_null(pl, 0) == NULL, "file row should have no station name");

    if (net_available()) {
        urls[0] = URL_A;
        names[0] = "Alpha FM";
        urls[1] = URL_B;
        names[1] = NULL;
        added = playlist_add_stations(pl, urls, names, 2, &full);
        CHECK(added == 2, "station add: got %d", added);
        CHECK(playlist_count(pl) == 3, "count after stations: %d", playlist_count(pl));
        CHECK(strcmp(playlist_path_at(pl, 1), URL_A) == 0,
              "station URL stored as '%s'", playlist_path_at(pl, 1));
        CHECK(label_or_null(pl, 1) && strcmp(label_or_null(pl, 1), "Alpha FM") == 0,
              "station name lost");
        CHECK(label_or_null(pl, 2) == NULL, "unnamed station should have no label");

        /* Re-adding a listed station is skipped, names untouched. */
        added = playlist_add_stations(pl, urls, names, 1, &full);
        CHECK(added == 0, "duplicate station was added");
        CHECK(playlist_count(pl) == 3, "duplicate changed the count");

        /* Save, reload from disk: URL lines come back verbatim, names too. */
        CHECK(playlist_save_as(pl, out, NULL, 0) == 0, "save_as failed");
        CHECK(file_contains(out, "#EXTINF:-1,Alpha FM\n" URL_A "\n"),
              "saved file lacks the EXTINF line before the station URL");
        CHECK(file_contains(out, URL_B "\n"), "saved file lacks the second URL");

        /* Remove the named station: the names array must shift with paths. */
        playlist_select(pl, 1);
        playlist_remove_selected(pl);
        CHECK(playlist_count(pl) == 2, "count after remove: %d", playlist_count(pl));
        CHECK(strcmp(playlist_path_at(pl, 1), URL_B) == 0, "remove shifted paths wrong");
        CHECK(label_or_null(pl, 1) == NULL, "remove left a stale name behind");

        playlist_clear(pl);
        CHECK(playlist_count(pl) == 0, "clear left entries");
        CHECK(label_or_null(pl, 0) == NULL, "clear left a name");

        {
            char err[256] = "";

            CHECK(playlist_load_file(pl, out, err, sizeof err) == 0,
                  "load_file failed: %s", err);
        }
        CHECK(playlist_count(pl) == 3, "load_file count: %d", playlist_count(pl));
        CHECK(strcmp(playlist_path_at(pl, 1), URL_A) == 0,
              "load_file mangled the URL: '%s'", playlist_path_at(pl, 1));
        CHECK(label_or_null(pl, 1) && strcmp(label_or_null(pl, 1), "Alpha FM") == 0,
              "load_file lost the station name");
        CHECK(label_or_null(pl, 0) == NULL, "load_file named a file row");

        /* Destroy writes the autosave; a fresh playlist restores from it. */
        playlist_destroy(pl);
        pl = playlist_create();
        CHECK(pl != NULL, "second playlist_create failed");
        CHECK(playlist_count(pl) == 3, "autosave restore count: %d",
              playlist_count(pl));
        if (pl && playlist_count(pl) == 3) {
            CHECK(strcmp(playlist_path_at(pl, 2), URL_B) == 0,
                  "autosave restore mangled URL B");
            CHECK(label_or_null(pl, 1) && strcmp(label_or_null(pl, 1), "Alpha FM") == 0,
                  "autosave restore lost the station name");
        }
    }

    /* Files still work after all that: the same file is refused as a duplicate
     * and the list keeps its shape. */
    if (pl) {
        added = playlist_add_many(pl, files, 1, &full);
        CHECK(added == 0, "duplicate file was added");
        playlist_destroy(pl);
    }

    if (failures) {
        fprintf(stderr, "test_playlist: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_playlist: all checks passed\n");
    return 0;
}
