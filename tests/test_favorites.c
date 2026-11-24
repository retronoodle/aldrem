/* test_favorites.c — checks for src/favorites.c. The Makefile runs this with
 * XDG_CONFIG_HOME pointed at build/test-xdg, so the real user config is never
 * touched. */
#define _POSIX_C_SOURCE 200809L /* setenv, mkdir */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "favorites.h"

static int failures;
static char path[1024];

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);   \
            failures++;                                               \
        }                                                             \
    } while (0)

static RadioStation
station(const char *name, const char *url, int bitrate)
{
    RadioStation st;

    memset(&st, 0, sizeof st);
    snprintf(st.name, sizeof st.name, "%s", name);
    snprintf(st.url, sizeof st.url, "%s", url);
    snprintf(st.codec, sizeof st.codec, "MP3");
    snprintf(st.country, sizeof st.country, "Iceland");
    st.bitrate = bitrate;
    return st;
}

/* Replaces the saved file with raw text, bypassing the module. */
static void
write_raw(const char *text)
{
    FILE *f = fopen(path, "w");

    if (!f) {
        fprintf(stderr, "FAIL: cannot write %s\n", path);
        failures++;
        return;
    }
    fputs(text, f);
    fclose(f);
}

static void
reset_file(void)
{
    remove(path);
    favorites_load();
}

static void
test_empty_and_toggle(void)
{
    RadioStation a = station("Rás 2", "http://a.example/stream", 128);

    reset_file();
    CHECK(favorites_count() == 0, "empty list has no entries");
    CHECK(favorites_has(a.url) == 0, "empty list has no URL");
    CHECK(favorites_at(0) == NULL, "out of range index is NULL");
    CHECK(favorites_at(-1) == NULL, "negative index is NULL");

    CHECK(favorites_toggle(&a) == FAV_ADDED, "first toggle adds");
    CHECK(favorites_count() == 1, "one favorite after add");
    CHECK(favorites_has(a.url) == 1, "added URL is found");
    CHECK(favorites_toggle(&a) == FAV_REMOVED, "second toggle removes");
    CHECK(favorites_count() == 0, "empty again after remove");
    CHECK(favorites_has(a.url) == 0, "removed URL is gone");
}

static void
test_round_trip_and_sanitise(void)
{
    RadioStation a = station("Tab\there\nnew line", "http://a.example/1", 0);
    RadioStation b = station("Second", "http://b.example/2", 320);
    RadioStation c = station("Third", "http://c.example/3", 64);
    const RadioStation *got;

    reset_file();
    CHECK(favorites_toggle(&a) == FAV_ADDED, "add a");
    CHECK(favorites_toggle(&b) == FAV_ADDED, "add b");
    CHECK(favorites_toggle(&c) == FAV_ADDED, "add c");
    CHECK(favorites_toggle(&b) == FAV_REMOVED, "remove b keeps order");

    /* Reload from disk: the list must come back the same. */
    CHECK(favorites_load() == 2, "reload finds two favorites");
    CHECK(favorites_count() == 2, "count after reload");
    got = favorites_at(0);
    CHECK(got && strcmp(got->url, a.url) == 0, "first is a after reload");
    CHECK(got && strcmp(got->name, "Tab here new line") == 0,
          "tab and newline in name became spaces");
    CHECK(got && strcmp(got->codec, "MP3") == 0, "codec survives reload");
    CHECK(got && strcmp(got->country, "Iceland") == 0, "country survives");
    CHECK(got && got->bitrate == 0, "zero bitrate survives");
    got = favorites_at(1);
    CHECK(got && strcmp(got->url, c.url) == 0, "second is c after reload");
    CHECK(got && got->bitrate == 64, "bitrate survives reload");
}

static void
test_load_skips_bad_and_duplicate_lines(void)
{
    reset_file();
    write_raw("Good\thttp://g.example/1\tMP3\tUK\t96\n"
              "no tabs at all\n"
              "only\tthree\tfields\n"
              "Empty url\t\tMP3\tUK\t96\n"
              "Dupe\thttp://g.example/1\tOGG\tUS\t10\n"
              "Bad bitrate\thttp://g.example/2\tAAC\tFR\tfast\n"
              "Negative\thttp://g.example/3\tAAC\tFR\t-5\n");

    CHECK(favorites_load() == 3, "keeps three valid unique stations");
    CHECK(favorites_has("http://g.example/1") == 1, "first valid line kept");
    CHECK(favorites_at(0) && strcmp(favorites_at(0)->name, "Good") == 0,
          "duplicate does not replace the first entry");
    CHECK(favorites_at(1) && favorites_at(1)->bitrate == 0,
          "non-numeric bitrate becomes 0");
    CHECK(favorites_at(2) && favorites_at(2)->bitrate == 0,
          "negative bitrate becomes 0");
}

static void
test_cap(void)
{
    RadioStation extra = station("Extra", "http://extra.example/", 0);
    FILE *f;
    int i;

    reset_file();
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "FAIL: cannot write %s\n", path);
        failures++;
        return;
    }
    for (i = 0; i < FAV_MAX; i++)
        fprintf(f, "S%d\thttp://s.example/%d\tMP3\tUK\t0\n", i, i);
    fclose(f);

    CHECK(favorites_load() == FAV_MAX, "loads up to the cap");
    CHECK(favorites_toggle(&extra) == FAV_FULL, "add at cap is refused");
    CHECK(favorites_count() == FAV_MAX, "refused add changes nothing");
    CHECK(favorites_has(extra.url) == 0, "refused URL is not stored");
}

static void
test_save_failure_keeps_memory(void)
{
    RadioStation a = station("Keep", "http://keep.example/", 0);
    char saved[1024];
    char blocker[2048];
    FILE *f;

    reset_file();
    /* A regular file cannot hold an aldrem directory, so the save must fail
     * with no change to the in-memory list. */
    snprintf(saved, sizeof saved, "%s", getenv("XDG_CONFIG_HOME"));
    snprintf(blocker, sizeof blocker, "%s/blocker", saved);
    f = fopen(blocker, "w");
    if (f)
        fclose(f);
    setenv("XDG_CONFIG_HOME", blocker, 1);

    CHECK(favorites_toggle(&a) == FAV_IO, "save failure is reported");
    CHECK(favorites_count() == 0, "failed add leaves list unchanged");
    CHECK(favorites_has(a.url) == 0, "failed add not visible");

    setenv("XDG_CONFIG_HOME", saved, 1);
    remove(blocker);
}

static void
test_big_bitrate_reads_zero_both_ways(void)
{
    RadioStation a = station("Loud", "http://loud.example/", 150000);

    reset_file();
    CHECK(favorites_toggle(&a) == FAV_ADDED, "add over-range bitrate station");
    CHECK(favorites_at(0) && favorites_at(0)->bitrate == 0,
          "over-range bitrate is 0 in memory");
    CHECK(favorites_load() == 1, "reload finds the station");
    CHECK(favorites_at(0) && favorites_at(0)->bitrate == 0,
          "over-range bitrate is 0 after reload");
}

static void
test_unreadable_file_blocks_save(void)
{
    RadioStation a = station("New", "http://new.example/", 64);

    /* A directory where the file should be opens for reading but cannot be
     * read, which stands in for a file the user cannot read. Saving over it
     * would throw the real list away, so the toggle must refuse. */
    remove(path);
    mkdir(path, 0700);
    CHECK(favorites_load() == 0, "unreadable file loads as empty");
    CHECK(favorites_toggle(&a) == FAV_IO, "toggle refused over unreadable file");
    CHECK(favorites_count() == 0, "refused toggle changes nothing");
    CHECK(rmdir(path) == 0, "unreadable stand-in is left untouched");
}

int
main(void)
{
    const char *root = getenv("XDG_CONFIG_HOME");

    if (!root || !root[0]) {
        fprintf(stderr, "FAIL: XDG_CONFIG_HOME must be set for this test\n");
        return 1;
    }
    /* The module creates the aldrem directory on save; make sure it exists
     * before the test writes raw fixtures straight into it. */
    snprintf(path, sizeof path, "%s/aldrem", root);
    mkdir(root, 0700);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/aldrem/favorites.tsv", root);

    test_empty_and_toggle();
    test_round_trip_and_sanitise();
    test_load_skips_bad_and_duplicate_lines();
    test_cap();
    test_save_failure_keeps_memory();
    test_big_bitrate_reads_zero_both_ways();
    test_unreadable_file_blocks_save();

    remove(path);
    if (failures) {
        fprintf(stderr, "test_favorites: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_favorites: all checks passed\n");
    return 0;
}
