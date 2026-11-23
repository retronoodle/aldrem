/* test_finder.c — checks the recursive audio finder against a fixture tree.
 * Usage: test_finder <empty-dir>. The directory is populated here. No device. */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "finder.h"

static int failures;

#define CHECK(cond, msg)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            fprintf(stderr, "FAIL: %s\n", msg);          \
            failures++;                                  \
        }                                                \
    } while (0)

static char root[1024];

static int
make_file(const char *rel)
{
    char path[1100];
    FILE *fp;

    snprintf(path, sizeof path, "%s/%s", root, rel);
    fp = fopen(path, "w");
    if (!fp)
        return -1;
    fputs("x", fp);
    fclose(fp);
    return 0;
}

static int
make_dir(const char *rel)
{
    char path[1100];

    snprintf(path, sizeof path, "%s/%s", root, rel);
    return mkdir(path, 0700);
}

/* Links live directly under root, so target_rel is already relative to them. */
static int
make_link(const char *target_rel, const char *link_rel)
{
    char link[1100];

    snprintf(link, sizeof link, "%s/%s", root, link_rel);
    return symlink(target_rel, link);
}

/* Extension filter standing in for track_ext_supported(). */
static int
is_audio(const char *name)
{
    size_t n = strlen(name);

    return (n > 4 && strcmp(name + n - 4, ".mp3") == 0) ||
           (n > 4 && strcmp(name + n - 4, ".wav") == 0) ||
           (n > 5 && strcmp(name + n - 5, ".flac") == 0) ||
           (n > 4 && strcmp(name + n - 4, ".ogg") == 0);
}

static int
ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s), tl = strlen(suffix);

    return sl >= tl && strcmp(s + sl - tl, suffix) == 0;
}

static int
has_suffix(const Finder *f, const char *suffix)
{
    int i;

    for (i = 0; i < finder_count(f); i++) {
        if (ends_with(finder_path_at(f, i), suffix))
            return 1;
    }
    return 0;
}

static void
run_to_end(Finder *f)
{
    while (finder_step(f, 4))
        ;
}

int
main(int argc, char **argv)
{
    Finder *f;
    int i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <empty-dir>\n", argv[0]);
        return 2;
    }
    snprintf(root, sizeof root, "%s", argv[1]);

    /* Fixture: audio at the top, nested, plus hidden entries, a symlinked
     * directory, a loop back to the root and a symlinked file. */
    CHECK(make_file("a.mp3") == 0, "create a.mp3");
    CHECK(make_file("b.txt") == 0, "create b.txt");
    CHECK(make_file(".hidden.mp3") == 0, "create .hidden.mp3");
    CHECK(make_dir(".hdir") == 0, "create .hdir");
    CHECK(make_file(".hdir/x.wav") == 0, "create .hdir/x.wav");
    CHECK(make_dir("sub") == 0, "create sub");
    CHECK(make_file("sub/c.flac") == 0, "create sub/c.flac");
    CHECK(make_dir("sub/deep") == 0, "create sub/deep");
    CHECK(make_file("sub/deep/d.ogg") == 0, "create sub/deep/d.ogg");
    CHECK(make_file("sub/notes.md") == 0, "create sub/notes.md");
    CHECK(make_link("sub", "sub_link") == 0, "symlink sub_link");
    CHECK(make_link(".", "loop") == 0, "symlink loop");
    CHECK(make_link("a.mp3", "alias.wav") == 0, "symlink alias.wav");

    /* A missing root is rejected without touching results. */
    f = finder_create();
    CHECK(f != NULL, "finder_create");
    CHECK(finder_start(f, "/nonexistent/aldrem-finder", is_audio) != 0,
          "missing root rejected");
    CHECK(!finder_running(f), "missing root leaves finder idle");

    /* Full walk: the four audio files, each exactly once, sorted. */
    CHECK(finder_start(f, root, is_audio) == 0, "start on fixture root");
    CHECK(finder_running(f), "running after start");
    run_to_end(f);
    CHECK(!finder_running(f), "walk finishes");
    CHECK(finder_count(f) == 4, "four audio files found");
    CHECK(has_suffix(f, "/a.mp3"), "finds a.mp3");
    CHECK(has_suffix(f, "/alias.wav"), "finds symlinked audio file");
    CHECK(has_suffix(f, "/sub/c.flac"), "finds sub/c.flac");
    CHECK(has_suffix(f, "/sub/deep/d.ogg"), "finds sub/deep/d.ogg");
    CHECK(!has_suffix(f, ".hidden.mp3"), "skips hidden file");
    CHECK(!has_suffix(f, "/.hdir/x.wav"), "skips hidden directory");
    CHECK(!has_suffix(f, "/b.txt"), "skips non-audio file");
    for (i = 1; i < finder_count(f); i++) {
        CHECK(strcmp(finder_path_at(f, i - 1), finder_path_at(f, i)) < 0,
              "results sorted and unique");
    }
    CHECK(finder_path_at(f, finder_count(f)) == NULL, "out-of-range is NULL");
    CHECK(!finder_truncated(f), "not truncated");

    /* Cancel mid-walk stops work and keeps what was found so far. */
    CHECK(finder_start(f, root, is_audio) == 0, "restart");
    finder_step(f, 2);
    finder_cancel(f);
    CHECK(!finder_running(f), "cancel stops the walk");
    CHECK(finder_step(f, 100) == 0, "step after cancel does nothing");

    /* Cap: a folder with more audio than FINDER_MAX_RESULT stops at the cap and
     * reports it. Runs last so the fixture counts above stay untouched. */
    {
        char flood[1100];

        snprintf(flood, sizeof flood, "%s/flood", root);
        CHECK(make_dir("flood") == 0, "create flood dir");
        for (i = 0; i <= FINDER_MAX_RESULT; i++) {
            char name[64];

            snprintf(name, sizeof name, "flood/f%05d.mp3", i);
            CHECK(make_file(name) == 0, "create flood file");
        }
        CHECK(finder_start(f, flood, is_audio) == 0, "start on flood dir");
        run_to_end(f);
        CHECK(!finder_running(f), "flood walk finishes");
        CHECK(finder_count(f) == FINDER_MAX_RESULT, "result capped at max");
        CHECK(finder_truncated(f), "cap reported as truncated");
        CHECK(!finder_out_of_memory(f), "cap is not reported as out of memory");
    }

    finder_free(f);
    if (failures) {
        fprintf(stderr, "test_finder: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_finder: all checks passed\n");
    return 0;
}
