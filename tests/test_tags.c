/* test_tags.c — tags_read against synthetic fixtures. Each fixture is written
 * under build/, checked, then removed. Hostile inputs must come back empty
 * without crashing. Exit status is the failure count (0 = pass).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tags.h"

#define LIT(s) (s), (sizeof(s) - 1)

typedef struct {
    unsigned char b[8192];
    size_t n;
} Buf;

static int failures = 0;

static void
check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

static void
add(Buf *x, const void *p, size_t n)
{
    memcpy(x->b + x->n, p, n);
    x->n += n;
}

static void
add_u8(Buf *x, unsigned v)
{
    unsigned char c = (unsigned char)v;

    add(x, &c, 1);
}

static void
add_be32(Buf *x, unsigned long v)
{
    unsigned char c[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 8), (unsigned char)v};

    add(x, c, 4);
}

static void
add_le32(Buf *x, unsigned long v)
{
    unsigned char c[4] = {(unsigned char)v, (unsigned char)(v >> 8),
                          (unsigned char)(v >> 16), (unsigned char)(v >> 24)};

    add(x, c, 4);
}

static void
add_syncsafe(Buf *x, unsigned long v)
{
    unsigned char c[4] = {(unsigned char)((v >> 21) & 0x7f),
                          (unsigned char)((v >> 14) & 0x7f),
                          (unsigned char)((v >> 7) & 0x7f),
                          (unsigned char)(v & 0x7f)};

    add(x, c, 4);
}

/* ID3v2 frame: id, size (syncsafe in v2.4 only), two flag bytes, body. */
static void
add_frame(Buf *t, const char *id, int major, const void *data, size_t n)
{
    add(t, id, 4);
    if (major == 4)
        add_syncsafe(t, n);
    else
        add_be32(t, n);
    add_u8(t, 0);
    add_u8(t, 0);
    add(t, data, n);
}

static void
id3_header(Buf *x, int major, const Buf *body)
{
    add(x, "ID3", 3);
    add_u8(x, major);
    add_u8(x, 0);
    add_u8(x, 0);
    add_syncsafe(x, body->n);
    add(x, body->b, body->n);
}

/* RIFF / AIFF-style chunk with even padding. */
static void
add_chunk_le(Buf *x, const char *id, const void *d, size_t n)
{
    add(x, id, 4);
    add_le32(x, n);
    add(x, d, n);
    if (n & 1)
        add_u8(x, 0);
}

static void
add_chunk_be(Buf *x, const char *id, const void *d, size_t n)
{
    add(x, id, 4);
    add_be32(x, n);
    add(x, d, n);
    if (n & 1)
        add_u8(x, 0);
}

/* FLAC metadata block header plus body. */
static void
flac_block(Buf *x, int last, int type, const Buf *body)
{
    add_u8(x, (last ? 0x80 : 0) | type);
    add_u8(x, (unsigned)(body->n >> 16));
    add_u8(x, (unsigned)(body->n >> 8));
    add_u8(x, (unsigned)body->n);
    add(x, body->b, body->n);
}

/* Vorbis comment: little-endian length, then the string. */
static void
add_comment(Buf *x, const char *s)
{
    add_le32(x, strlen(s));
    add(x, s, strlen(s));
}

static void
ogg_page(Buf *x, const void *pkt, size_t n)
{
    unsigned char hd[27];

    memset(hd, 0, sizeof hd);
    memcpy(hd, "OggS", 4);
    hd[26] = 1; /* one segment: the packet itself, under 255 bytes */
    add(x, hd, sizeof hd);
    add_u8(x, (unsigned)n);
    add(x, pkt, n);
}

static int
write_file(const char *path, const Buf *x)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return 0;
    fwrite(x->b, 1, x->n, f);
    fclose(f);
    return 1;
}

/* Writes the fixture, reads its tags and compares both fields. */
static void
run(const char *name, const Buf *x, const char *want_artist,
    const char *want_title)
{
    char path[256], a[128], t[128];
    int got, ok;

    snprintf(path, sizeof path, "build/tags_%s", name);
    if (!write_file(path, x)) {
        check(name, 0);
        return;
    }
    got = tags_read(path, a, sizeof a, t, sizeof t);
    ok = strcmp(a, want_artist) == 0 && strcmp(t, want_title) == 0
      && got == (want_artist[0] || want_title[0] ? 1 : 0);
    check(name, ok);
    if (!ok)
        printf("     got artist=\"%s\" title=\"%s\" (returned %d)\n", a, t, got);
    remove(path);
}

static void
test_id3v23(void)
{
    Buf tag = {{0}, 0}, file = {{0}, 0};
    char a[128], t[5]; /* 5 bytes: "Café" needs 5 + NUL, so it must clip */
    int got;

    add_frame(&tag, "TALB", 3, LIT("\x00" "Album"));
    add_frame(&tag, "TIT2", 3, LIT("\x03" "Caf\xc3\xa9 Song"));
    add_frame(&tag, "TPE1", 3, LIT("\x00" "Bj\xf6rk"));
    id3_header(&file, 3, &tag);
    add(&file, "\xff\xfb\x90\x00", 4);
    run("v23.mp3", &file, "Bj\xc3\xb6rk", "Caf\xc3\xa9 Song");

    if (write_file("build/tags_clip.mp3", &file)) {
        got = tags_read("build/tags_clip.mp3", a, sizeof a, t, sizeof t);
        check("id3 title clipped on a character boundary",
              got == 1 && strcmp(t, "Caf") == 0);
        remove("build/tags_clip.mp3");
    }
}

static void
test_id3v24(void)
{
    Buf tag = {{0}, 0}, file = {{0}, 0};

    add_frame(&tag, "TIT2", 4, LIT("\x02" "\0W\0i\0d\0e"));
    add_frame(&tag, "TPE1", 4, LIT("\x01\xff\xfeO\0k\0\0\0"));
    id3_header(&file, 4, &tag);
    run("v24.mp3", &file, "Ok", "Wide");
}

static void
test_id3v1(void)
{
    Buf file = {{0}, 0};
    unsigned char tr[128];

    memset(tr, 0, sizeof tr);
    memcpy(tr, "TAG", 3);
    memcpy(tr + 3, "V1 Title", 8);
    memcpy(tr + 33, "V1 Artist", 9);
    add(&file, "\xff\xfb\x90\x00", 4);
    add(&file, tr, sizeof tr);
    run("v1.mp3", &file, "V1 Artist", "V1 Title");
}

static void
test_flac(void)
{
    Buf si = {{0}, 0}, vc = {{0}, 0}, file = {{0}, 0};

    memset(si.b, 0, 34);
    si.n = 34;
    add_le32(&vc, 1);
    add(&vc, "x", 1);
    add_le32(&vc, 2);
    add_comment(&vc, "TITLE=Flac Title");
    add_comment(&vc, "ARTIST=Flac Artist");
    add(&file, "fLaC", 4);
    flac_block(&file, 0, 0, &si);
    flac_block(&file, 1, 4, &vc);
    run("a.flac", &file, "Flac Artist", "Flac Title");
}

static void
test_wav(void)
{
    Buf info = {{0}, 0}, file = {{0}, 0};

    add(&info, "INFO", 4);
    add_chunk_le(&info, "INAM", LIT("Wav Title"));
    add_chunk_le(&info, "IART", LIT("Wav Artist"));
    add(&file, "RIFF", 4);
    add_le32(&file, 0);
    add(&file, "WAVE", 4);
    add_chunk_le(&file, "LIST", info.b, info.n);
    run("a.wav", &file, "Wav Artist", "Wav Title");
}

static void
test_aiff(void)
{
    Buf file = {{0}, 0};

    add(&file, "FORM", 4);
    add_be32(&file, 0);
    add(&file, "AIFF", 4);
    add_chunk_be(&file, "NAME", LIT("Aiff Title"));
    add_chunk_be(&file, "AUTH", LIT("Aiff Artist"));
    run("a.aiff", &file, "Aiff Artist", "Aiff Title");
}

static void
test_ogg(void)
{
    Buf id = {{0}, 0}, cm = {{0}, 0}, file = {{0}, 0};
    unsigned char zeros[23];

    memset(zeros, 0, sizeof zeros);
    add(&id, "\x01" "vorbis", 7);
    add(&id, zeros, sizeof zeros);
    add(&cm, "\x03" "vorbis", 7);
    add_le32(&cm, 0);
    add_le32(&cm, 2);
    add_comment(&cm, "TITLE=Ogg Title");
    add_comment(&cm, "ARTIST=Ogg Artist");
    add_u8(&cm, 1);
    ogg_page(&file, id.b, id.n);
    ogg_page(&file, cm.b, cm.n);
    run("a.ogg", &file, "Ogg Artist", "Ogg Title");
}

/* Hostile inputs: lengths that overrun the file. Expect nothing, no crash. */
static void
test_hostile(void)
{
    Buf tag = {{0}, 0}, file = {{0}, 0}, flac = {{0}, 0}, ogg = {{0}, 0};
    unsigned char zeros[23];

    add(&tag, "TIT2", 4);
    add_be32(&tag, 0xFFFFFF);
    add_u8(&tag, 0);
    add_u8(&tag, 0);
    add(&tag, "\x03" "abc", 4);
    id3_header(&file, 3, &tag);
    run("bad_id3.mp3", &file, "", "");

    memset(zeros, 0, sizeof zeros);
    add(&flac, "fLaC", 4);
    add_u8(&flac, 0x84);
    add_u8(&flac, 0xFF);
    add_u8(&flac, 0xFF);
    add_u8(&flac, 0xFF);
    add(&flac, zeros, 10);
    run("bad.flac", &flac, "", "");

    memset(zeros, 0, sizeof zeros);
    add(&ogg, "OggS", 4);
    add(&ogg, zeros, 23);
    ogg.b[26] = 255; /* claims 255 segments, provides none */
    add(&ogg, zeros, 10);
    run("bad.ogg", &ogg, "", "");
}

/* The decoders' own test fixtures carry no guaranteed tags; they must simply
 * come back without crashing. */
static void
test_real_files(void)
{
    const char *files[] = {"tests/data/tone.mp3", "tests/data/tone.ogg",
                           "tests/data/tone.aac"};
    size_t i;

    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        char a[128], t[128];
        int got = tags_read(files[i], a, sizeof a, t, sizeof t);

        check(files[i], got == 0 || got == 1);
    }
}

int
main(void)
{
    test_id3v23();
    test_id3v24();
    test_id3v1();
    test_flac();
    test_wav();
    test_aiff();
    test_ogg();
    test_hostile();
    test_real_files();

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("all tag tests passed\n");
    return failures ? 1 : 0;
}
