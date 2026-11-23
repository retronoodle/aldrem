/* tags.c — see tags.h. Every length read from a file is checked against what
 * remains before it is used to read or allocate, so a hostile tag can at worst
 * yield no text.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tags.h"

#define TXT_MAX       512UL        /* one decoded field, before clipping */
#define ID3_MAX_TAG   (4UL << 20)  /* ID3v2 body read; cover art can be large */
#define BLOCK_MAX     (4UL << 20)  /* Vorbis comment block / Ogg packet cap */
#define OGG_MAX_PAGES 256
#define ID3V1_BYTES   128
#define WAV_LIST_MAX  (1UL << 20)
#define AIFF_TEXT_MAX 1024

typedef struct {
    char *artist;
    size_t acap;
    char *title;
    size_t tcap;
} Dest;

/* ---- byte order helpers ------------------------------------------------- */

static unsigned long
be24(const unsigned char *p)
{
    return ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
}

static unsigned long
be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] << 8) | p[3];
}

static unsigned long
le32(const unsigned char *p)
{
    return ((unsigned long)p[3] << 24) | ((unsigned long)p[2] << 16)
         | ((unsigned long)p[1] << 8) | p[0];
}

static unsigned long
syncsafe(const unsigned char *p)
{
    return ((unsigned long)(p[0] & 0x7f) << 21) | ((unsigned long)(p[1] & 0x7f) << 14)
         | ((unsigned long)(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static unsigned long
unit16(const unsigned char *p, int be)
{
    return be ? ((unsigned long)p[0] << 8 | p[1]) : ((unsigned long)p[1] << 8 | p[0]);
}

/* ---- text ---------------------------------------------------------------- */

/* Appends cp to out as UTF-8. Returns 0 when it would not fit with the NUL. */
static int
put_cp(char *out, size_t cap, size_t *pos, unsigned long cp)
{
    unsigned char b[4];
    size_t n, i;

    if (cp < 0x80) {
        b[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*pos + n + 1 > cap)
        return 0;
    for (i = 0; i < n; i++)
        out[*pos + i] = (char)b[i];
    *pos += n;
    out[*pos] = '\0';
    return 1;
}

/* Decodes one UTF-8 sequence at p (n bytes left, n >= 1). Returns the bytes
 * consumed; invalid input yields U+FFFD. */
static size_t
utf8_decode(const unsigned char *p, size_t n, unsigned long *cp)
{
    unsigned char c = p[0];
    size_t len, k;
    unsigned long min;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        len = 2; *cp = c & 0x1F; min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3; *cp = c & 0x0F; min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4; *cp = c & 0x07; min = 0x10000;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if (n < len) {
        *cp = 0xFFFD;
        return 1;
    }
    for (k = 1; k < len; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return k;
        }
        *cp = (*cp << 6) | (p[k] & 0x3F);
    }
    if (*cp < min || *cp > 0x10FFFF || (*cp >= 0xD800 && *cp <= 0xDFFF))
        *cp = 0xFFFD;
    return len;
}

/* Decodes an ID3-style text payload into UTF-8. enc: 0 Latin-1, 1 UTF-16 with
 * BOM, 2 UTF-16BE, 3 UTF-8 (anything else is read as Latin-1). Decoding stops
 * at the first terminator, so only the first value of a multi-value frame is
 * kept. */
static void
decode_text(int enc, const unsigned char *p, size_t n, char *out, size_t cap)
{
    size_t pos = 0, i = 0;

    out[0] = '\0';
    if (enc == 1 || enc == 2) {
        int be = enc == 2;

        if (enc == 1 && n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
            be = 1;
            i = 2;
        } else if (enc == 1 && n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
            be = 0;
            i = 2;
        }
        while (i + 2 <= n) {
            unsigned long u = unit16(p + i, be), lo;

            i += 2;
            if (u == 0)
                break;
            if (u >= 0xD800 && u <= 0xDBFF) {
                if (i + 2 <= n) {
                    lo = unit16(p + i, be);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                        i += 2;
                    } else {
                        u = 0xFFFD;
                    }
                } else {
                    u = 0xFFFD;
                }
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                u = 0xFFFD;
            }
            if (!put_cp(out, cap, &pos, u))
                break;
        }
    } else if (enc == 3) {
        while (i < n && p[i]) {
            unsigned long cp;

            i += utf8_decode(p + i, n - i, &cp);
            if (!put_cp(out, cap, &pos, cp))
                break;
        }
    } else {
        while (i < n && p[i]) {
            if (!put_cp(out, cap, &pos, p[i]))
                break;
            i++;
        }
    }
}

/* Trims surrounding whitespace in place. */
static void
trim(char *s)
{
    size_t len = strlen(s), a = 0;

    while (a < len && isspace((unsigned char)s[a]))
        a++;
    memmove(s, s + a, len - a + 1);
    len -= a;
    while (len && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

/* Stores src in dst when dst is still empty, so the first tag found wins
 * (ID3v2 before ID3v1). Clips to cap without splitting a UTF-8 sequence. */
static void
take(char *dst, size_t cap, const char *src)
{
    size_t n, k = 0;

    if (!cap || dst[0])
        return;
    snprintf(dst, cap, "%s", src);
    n = strlen(dst);
    while (k < 3 && k < n && ((unsigned char)dst[n - 1 - k] & 0xC0) == 0x80)
        k++;
    if (k < n) {
        unsigned char lead = (unsigned char)dst[n - 1 - k];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;

        if (need > k + 1)
            dst[n - 1 - k] = '\0';
    }
    trim(dst);
}

static void
text_field(char *dst, size_t cap, int enc, const unsigned char *p, size_t n)
{
    char tmp[TXT_MAX];

    decode_text(enc, p, n, tmp, sizeof tmp);
    take(dst, cap, tmp);
}

/* Reverses ID3 unsynchronisation (0xFF 0x00 -> 0xFF) in place. Returns the new
 * length. */
static size_t
unsync_decode(unsigned char *b, size_t n)
{
    size_t r, w = 0;

    for (r = 0; r < n; r++) {
        b[w++] = b[r];
        if (b[r] == 0xFF && r + 1 < n && b[r + 1] == 0x00)
            r++;
    }
    return w;
}

/* ---- ID3 (MP3) ----------------------------------------------------------- */

/* Walks the frames in [p, end). Only the title and artist frames are decoded;
 * everything else is skipped by its declared size. */
static void
id3_frames(unsigned char *p, unsigned char *end, int major, int header_unsync,
           Dest *d)
{
    size_t hlen = major == 2 ? 6 : 10;

    while ((size_t)(end - p) >= hlen) {
        char id[5];
        unsigned long fsize;
        int fflags = 0, is_title, is_artist;
        unsigned char *body;
        size_t blen;

        if (p[0] == 0)
            break; /* padding */
        if (major == 2) {
            memcpy(id, p, 3);
            id[3] = '\0';
            fsize = be24(p + 3);
        } else {
            memcpy(id, p, 4);
            id[4] = '\0';
            fsize = major == 4 ? syncsafe(p + 4) : be32(p + 4);
            fflags = p[9];
        }
        p += hlen;
        if (fsize > (unsigned long)(end - p))
            break;
        body = p;
        blen = (size_t)fsize;
        p += fsize;

        if (major == 2) {
            is_title = strcmp(id, "TT2") == 0;
            is_artist = strcmp(id, "TP1") == 0;
        } else {
            is_title = strcmp(id, "TIT2") == 0;
            is_artist = strcmp(id, "TPE1") == 0;
        }
        if (!is_title && !is_artist)
            continue;

        if (major == 4) {
            if (fflags & 0x0C)
                continue;                          /* compressed or encrypted */
            if (fflags & 0x40) {                   /* grouping identity */
                if (!blen)
                    continue;
                body++;
                blen--;
            }
            if (fflags & 0x01) {                   /* data length indicator */
                if (blen < 4)
                    continue;
                body += 4;
                blen -= 4;
            }
            if ((fflags & 0x02) && !header_unsync)
                blen = unsync_decode(body, blen);
        } else if (major == 3) {
            if (fflags & 0xC0)
                continue;                          /* compressed or encrypted */
            if (fflags & 0x20) {                   /* grouping identity */
                if (!blen)
                    continue;
                body++;
                blen--;
            }
        }

        if (blen < 1)
            continue;
        if (is_title)
            text_field(d->title, d->tcap, body[0], body + 1, blen - 1);
        else
            text_field(d->artist, d->acap, body[0], body + 1, blen - 1);
    }
}

static void
read_id3v2(FILE *fp, long file_size, Dest *d)
{
    unsigned char hdr[10], *buf, *p, *end;
    unsigned long tag_size;
    size_t len, got, avail;
    int major, flags;

    if (file_size < 10 || fseek(fp, 0, SEEK_SET) != 0
        || fread(hdr, 1, 10, fp) != 10 || memcmp(hdr, "ID3", 3) != 0)
        return;
    major = hdr[3];
    flags = hdr[5];
    if (major < 2 || major > 4)
        return;

    tag_size = syncsafe(hdr + 6);
    avail = (size_t)file_size - 10;
    len = tag_size < avail ? (size_t)tag_size : avail;
    if (len > ID3_MAX_TAG)
        len = ID3_MAX_TAG;
    if (len == 0)
        return;
    buf = malloc(len);
    if (!buf)
        return;
    got = fread(buf, 1, len, fp);
    if (flags & 0x80)
        got = unsync_decode(buf, got);

    p = buf;
    end = buf + got;
    if ((flags & 0x40) && (size_t)(end - p) >= 4) {
        size_t ext = major == 4 ? (size_t)syncsafe(p) : (size_t)be32(p) + 4;

        if (ext <= (size_t)(end - p))
            p += ext;
        else
            end = p;
    }
    id3_frames(p, end, major, (flags & 0x80) != 0, d);
    free(buf);
}

static void
read_id3v1(FILE *fp, long file_size, Dest *d)
{
    unsigned char t[ID3V1_BYTES];

    if (file_size < ID3V1_BYTES
        || fseek(fp, file_size - ID3V1_BYTES, SEEK_SET) != 0
        || fread(t, 1, ID3V1_BYTES, fp) != ID3V1_BYTES
        || memcmp(t, "TAG", 3) != 0)
        return;
    text_field(d->title, d->tcap, 0, t + 3, 30);
    text_field(d->artist, d->acap, 0, t + 33, 30);
}

/* ---- Vorbis comments (FLAC, Ogg) ----------------------------------------- */

static int
has_key(const unsigned char *c, size_t n, const char *key)
{
    size_t k = strlen(key), i;

    if (n < k)
        return 0;
    for (i = 0; i < k; i++) {
        if (toupper(c[i]) != (unsigned char)key[i])
            return 0;
    }
    return 1;
}

static void
vorbis_comment(const unsigned char *c, size_t n, Dest *d)
{
    char tmp[TXT_MAX];
    const char *key;
    char *dst;
    size_t cap, k, m;

    if (has_key(c, n, "TITLE=")) {
        key = "TITLE=";
        dst = d->title;
        cap = d->tcap;
    } else if (has_key(c, n, "ARTIST=")) {
        key = "ARTIST=";
        dst = d->artist;
        cap = d->acap;
    } else {
        return;
    }
    k = strlen(key);
    m = n - k;
    if (m > sizeof tmp - 1)
        m = sizeof tmp - 1;
    memcpy(tmp, c + k, m);
    tmp[m] = '\0';
    take(dst, cap, tmp);
}

/* Parses a Vorbis comment block: vendor string, then KEY=value entries. */
static void
parse_vorbis_comments(const unsigned char *p, size_t n, Dest *d)
{
    size_t pos, i, count;

    if (n < 4)
        return;
    if (le32(p) > n - 4)
        return;
    pos = 4 + (size_t)le32(p);
    if (n - pos < 4)
        return;
    count = (size_t)le32(p + pos);
    pos += 4;
    for (i = 0; i < count && n - pos >= 4; i++) {
        size_t clen = (size_t)le32(p + pos);

        pos += 4;
        if (clen > n - pos)
            return;
        vorbis_comment(p + pos, clen, d);
        pos += clen;
    }
}

static void
read_flac(FILE *fp, Dest *d)
{
    unsigned char h[4];

    if (fseek(fp, 0, SEEK_SET) != 0 || fread(h, 1, 4, fp) != 4
        || memcmp(h, "fLaC", 4) != 0)
        return;
    for (;;) {
        unsigned char bh[4];
        unsigned long len;
        int last, type;

        if (fread(bh, 1, 4, fp) != 4)
            return;
        last = bh[0] & 0x80;
        type = bh[0] & 0x7F;
        len = be24(bh + 1);
        if (type == 4) { /* VORBIS_COMMENT */
            unsigned char *b;

            if (len > 0 && len <= BLOCK_MAX) {
                b = malloc(len);
                if (b && fread(b, 1, len, fp) == len)
                    parse_vorbis_comments(b, len, d);
                free(b);
            }
            return;
        }
        if (last)
            return;
        if (fseek(fp, (long)len, SEEK_CUR) != 0)
            return;
    }
}

/* Grows *buf to hold at least need bytes. */
static int
pkt_reserve(unsigned char **buf, size_t *cap, size_t need)
{
    size_t nc;
    unsigned char *nb;

    if (need <= *cap)
        return 1;
    nc = *cap ? *cap : 4096;
    while (nc < need)
        nc *= 2;
    nb = realloc(*buf, nc);
    if (!nb)
        return 0;
    *buf = nb;
    *cap = nc;
    return 1;
}

/* Reassembles Ogg packets across pages. Packet 0 is the identification header;
 * packet 1 is the comment header, prefixed by "\x03vorbis". */
static void
read_ogg(FILE *fp, Dest *d)
{
    unsigned char *pkt = NULL;
    size_t plen = 0, pcap = 0;
    int packets = 0, overflow = 0, pages;

    if (fseek(fp, 0, SEEK_SET) != 0)
        return;
    for (pages = 0; pages < OGG_MAX_PAGES && packets < 2; pages++) {
        unsigned char h[27], seg[255];
        int nseg, i;

        if (fread(h, 1, 27, fp) != 27 || memcmp(h, "OggS", 4) != 0)
            break;
        nseg = h[26];
        if (fread(seg, 1, (size_t)nseg, fp) != (size_t)nseg)
            break;
        for (i = 0; i < nseg && packets < 2; i++) {
            size_t sl = seg[i];

            if (!overflow && plen + sl <= BLOCK_MAX) {
                if (sl) {
                    if (!pkt_reserve(&pkt, &pcap, plen + sl)
                        || fread(pkt + plen, 1, sl, fp) != sl)
                        goto out;
                    plen += sl;
                }
            } else {
                overflow = 1;
                if (fseek(fp, (long)sl, SEEK_CUR) != 0)
                    goto out;
            }
            if (sl < 255) { /* a lacing value under 255 ends the packet */
                if (packets == 1 && !overflow && plen >= 7
                    && memcmp(pkt, "\x03" "vorbis", 7) == 0)
                    parse_vorbis_comments(pkt + 7, plen - 7, d);
                packets++;
                plen = 0;
                overflow = 0;
            }
        }
    }
out:
    free(pkt);
}

/* ---- WAV / AIFF ---------------------------------------------------------- */

static void
wav_info(const unsigned char *p, size_t n, Dest *d)
{
    size_t q = 0;

    while (q + 8 <= n) {
        const unsigned char *id = p + q;
        size_t csz = (size_t)le32(p + q + 4);

        q += 8;
        if (csz > n - q)
            break;
        if (memcmp(id, "INAM", 4) == 0)
            text_field(d->title, d->tcap, 0, p + q, csz);
        else if (memcmp(id, "IART", 4) == 0)
            text_field(d->artist, d->acap, 0, p + q, csz);
        q += csz + (csz & 1);
    }
}

static void
read_wav(FILE *fp, long file_size, Dest *d)
{
    unsigned char h[12], c[8];
    long pos = 12;

    if (fseek(fp, 0, SEEK_SET) != 0 || fread(h, 1, 12, fp) != 12
        || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
        return;
    while (pos + 8 <= file_size) {
        unsigned long sz;

        if (fseek(fp, pos, SEEK_SET) != 0 || fread(c, 1, 8, fp) != 8)
            return;
        sz = le32(c + 4);
        if (memcmp(c, "LIST", 4) == 0 && sz >= 4 && sz <= WAV_LIST_MAX) {
            unsigned char *b = malloc(sz);

            if (b && fread(b, 1, sz, fp) == sz && memcmp(b, "INFO", 4) == 0)
                wav_info(b + 4, sz - 4, d);
            free(b);
        }
        pos += 8 + (long)sz + (long)(sz & 1);
    }
}

static void
read_aiff(FILE *fp, long file_size, Dest *d)
{
    unsigned char h[12], c[8];
    long pos = 12;

    if (fseek(fp, 0, SEEK_SET) != 0 || fread(h, 1, 12, fp) != 12
        || memcmp(h, "FORM", 4) != 0
        || (memcmp(h + 8, "AIFF", 4) != 0 && memcmp(h + 8, "AIFC", 4) != 0))
        return;
    while (pos + 8 <= file_size) {
        unsigned long sz;

        if (fseek(fp, pos, SEEK_SET) != 0 || fread(c, 1, 8, fp) != 8)
            return;
        sz = be32(c + 4);
        if (memcmp(c, "NAME", 4) == 0 || memcmp(c, "AUTH", 4) == 0) {
            unsigned char b[AIFF_TEXT_MAX];
            size_t want = sz < sizeof b ? (size_t)sz : sizeof b;
            size_t got = fread(b, 1, want, fp);

            if (c[0] == 'N')
                text_field(d->title, d->tcap, 0, b, got);
            else
                text_field(d->artist, d->acap, 0, b, got);
        }
        pos += 8 + (long)sz + (long)(sz & 1);
    }
}

/* ---- dispatch ------------------------------------------------------------ */

/* Case-insensitive test of the extension in the basename of path. */
static int
ext_is(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    size_t i;

    if (!dot || (slash && slash > dot))
        return 0;
    dot++;
    for (i = 0; ext[i] && dot[i]; i++) {
        if (tolower((unsigned char)dot[i]) != (unsigned char)ext[i])
            return 0;
    }
    return ext[i] == '\0' && dot[i] == '\0';
}

int
tags_read(const char *path, char *artist, size_t acap, char *title, size_t tcap)
{
    Dest d;
    FILE *fp;
    long size;

    if (!path || !artist || !title || !acap || !tcap)
        return 0;
    artist[0] = '\0';
    title[0] = '\0';
    d.artist = artist;
    d.acap = acap;
    d.title = title;
    d.tcap = tcap;

    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    if (fseek(fp, 0, SEEK_END) == 0)
        size = ftell(fp);
    else
        size = -1;

    if (size >= 0) {
        if (ext_is(path, "mp3")) {
            read_id3v2(fp, size, &d);
            read_id3v1(fp, size, &d);
        } else if (ext_is(path, "flac")) {
            read_flac(fp, &d);
        } else if (ext_is(path, "ogg") || ext_is(path, "oga")) {
            read_ogg(fp, &d);
        } else if (ext_is(path, "wav")) {
            read_wav(fp, size, &d);
        } else if (ext_is(path, "aif") || ext_is(path, "aiff")
                   || ext_is(path, "aifc")) {
            read_aiff(fp, size, &d);
        }
    }
    fclose(fp);
    return artist[0] || title[0];
}
