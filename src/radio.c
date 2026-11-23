/* radio.c — Radio Browser URL building and JSON parsing. See radio.h.
 *
 * Bodies are parsed with jsmn, a token scanner that does not unescape strings.
 * The few escapes a station name can carry are decoded here; anything outside
 * ASCII becomes '?' since the UI font is ASCII-only in practice.
 */
#include "radio.h"
#include "jsmn.h" /* vendor/, on the -Ivendor path */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RADIO_MAX_TOKENS_START 256

/* ---- JSON helpers over jsmn tokens ---- */

typedef struct {
    jsmntok_t *tok;
    int n;
    const char *json;
    size_t len;
} Doc;

/* Parses json into a growing token array. Returns 0 and fills d, or -1. */
static int
doc_parse(Doc *d, const char *json, size_t len)
{
    jsmn_parser p;
    int cap = RADIO_MAX_TOKENS_START, got;

    if (!json || len == 0 || len > 0x7fffffff)
        return -1;
    for (;;) {
        jsmntok_t *tok = (jsmntok_t *)malloc(sizeof *tok * (size_t)cap);

        if (!tok)
            return -1;
        jsmn_init(&p);
        got = jsmn_parse(&p, json, len, tok, (unsigned)cap);
        if (got == JSMN_ERROR_NOMEM) {
            free(tok);
            if (cap >= (1 << 22))
                return -1;
            cap *= 2;
            continue;
        }
        if (got < 0) {
            free(tok);
            return -1;
        }
        d->tok = tok;
        d->n = got;
        d->json = json;
        d->len = len;
        return 0;
    }
}

static void
doc_free(Doc *d)
{
    free(d->tok);
    d->tok = NULL;
    d->n = 0;
}

/* Copies a JSON string token into out, decoding the common escapes. Returns 0
 * on success (truncated to fit, always NUL-terminated), -1 on a non-string. */
static int
tok_copy(const Doc *d, int idx, char *out, size_t outlen)
{
    const jsmntok_t *t = &d->tok[idx];
    size_t i, o = 0;

    if (t->type != JSMN_STRING || outlen == 0)
        return -1;
    for (i = (size_t)t->start; i < (size_t)t->end && o + 1 < outlen; i++) {
        char c = d->json[i], put;

        if (c != '\\') {
            put = c;
        } else if (i + 1 < (size_t)t->end) {
            char e = d->json[++i];

            switch (e) {
            case 'n': put = ' '; break;
            case 't': put = ' '; break;
            case 'r': put = ' '; break;
            case 'b': case 'f': put = ' '; break;
            case 'u':
                /* \uXXXX: ASCII kept, anything wider becomes '?'. The four hex
                 * digits are consumed either way. */
                {
                    unsigned v = 0;
                    int k;

                    for (k = 0; k < 4 && i + 1 < (size_t)t->end; k++) {
                        char h = d->json[++i];
                        unsigned dv;

                        if (h >= '0' && h <= '9') dv = (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') dv = (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') dv = (unsigned)(h - 'A' + 10);
                        else break;
                        v = v * 16 + dv;
                    }
                    put = v >= 0x20 && v < 0x7f ? (char)v : '?';
                }
                break;
            default: put = e; break; /* \" \\ \/ */
            }
        } else {
            break;
        }
        out[o++] = put;
    }
    out[o] = '\0';
    return 0;
}

/* jsmn gives each token a size: the number of direct children. An object's
 * children are its keys, each key has one child (its value), and so on. So
 * the subtree of token i is the run of tokens that a counter of pending
 * children covers. Iterative, so hostile nesting cannot overflow the stack. */
static int
tok_skip(const Doc *d, int i)
{
    int need = 1, j = i;

    while (need > 0 && j < d->n) {
        need--;
        need += d->tok[j].size;
        j++;
    }
    return j;
}

/* Index of the value for key inside the object at obj, or -1. */
static int
obj_value(const Doc *d, int obj, const char *key)
{
    int j = obj + 1, k;
    size_t klen = strlen(key);

    if (d->tok[obj].type != JSMN_OBJECT)
        return -1;
    for (k = 0; k < d->tok[obj].size && j < d->n; k++) {
        const jsmntok_t *kt = &d->tok[j];
        int value = j + 1;

        if (value >= d->n)
            break;
        if (kt->type == JSMN_STRING && (size_t)(kt->end - kt->start) == klen &&
            memcmp(d->json + kt->start, key, klen) == 0)
            return value;
        j = tok_skip(d, j); /* past this key and its value */
    }
    return -1;
}

/* Calls fn for each object that is a direct element of the root array. */
typedef void (*obj_fn)(const Doc *d, int obj, void *ctx);

static void
for_each_root_object(const Doc *d, obj_fn fn, void *ctx)
{
    int j = 1, k;

    if (d->n == 0 || d->tok[0].type != JSMN_ARRAY)
        return;
    for (k = 0; k < d->tok[0].size && j < d->n; k++) {
        if (d->tok[j].type == JSMN_OBJECT)
            fn(d, j, ctx);
        j = tok_skip(d, j);
    }
}

/* ---- mirrors ---- */

static int
host_ok(const char *h)
{
    size_t i, n = strlen(h);

    if (n == 0 || n >= RADIO_HOST_MAX)
        return 0;
    for (i = 0; i < n; i++) {
        char c = h[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

typedef struct {
    char (*hosts)[RADIO_HOST_MAX];
    int max;
    int count;
} MirrorCtx;

static void
mirror_visit(const Doc *d, int obj, void *vctx)
{
    MirrorCtx *c = (MirrorCtx *)vctx;
    int v, k;
    char host[RADIO_HOST_MAX];

    if (c->count >= c->max)
        return;
    v = obj_value(d, obj, "name");
    if (v < 0 || tok_copy(d, v, host, sizeof host) != 0 || !host_ok(host))
        return;
    /* Mirrors can be listed twice (IPv4 and IPv6 records). Keep one. */
    for (k = 0; k < c->count; k++) {
        if (strcmp(c->hosts[k], host) == 0)
            return;
    }
    snprintf(c->hosts[c->count], RADIO_HOST_MAX, "%s", host);
    c->count++;
}

int
radio_parse_mirrors(const char *json, size_t len,
                    char hosts[][RADIO_HOST_MAX], int max)
{
    Doc d;
    MirrorCtx c = { hosts, max, 0 };

    if (!hosts || max <= 0)
        return 0;
    if (doc_parse(&d, json, len) != 0)
        return 0;
    for_each_root_object(&d, mirror_visit, &c);
    doc_free(&d);
    return c.count;
}

/* ---- URL building ---- */

static int
is_unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

int
radio_search_url(const char *host, const char *query, int limit,
                 char *out, size_t outlen)
{
    size_t o = 0;
    const char *p;
    int n;

    if (!host || !host_ok(host) || !query || !out || outlen == 0)
        return -1;

    n = snprintf(out, outlen, "https://%s/json/stations/search?name=", host);
    if (n < 0 || (size_t)n >= outlen)
        return -1;
    o = (size_t)n;

    for (p = query; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char enc[4];

        if (is_unreserved(c)) {
            enc[0] = (char)c;
            enc[1] = '\0';
        } else {
            snprintf(enc, sizeof enc, "%%%02X", (unsigned)c);
        }
        n = (int)strlen(enc);
        if (o + (size_t)n + 1 >= outlen)
            return -1;
        memcpy(out + o, enc, (size_t)n);
        o += (size_t)n;
    }
    out[o] = '\0';

    n = snprintf(out + o, outlen - o,
                 "&hidebroken=true&order=clickcount&reverse=true&limit=%d",
                 limit);
    if (n < 0 || (size_t)n >= outlen - o)
        return -1;
    return 0;
}

/* ---- codec filter ---- */

int
radio_codec_supported(const char *codec)
{
    if (!codec)
        return 0;
    if (strcmp(codec, "MP3") == 0 || strcmp(codec, "OGG") == 0)
        return 1;
#ifdef HAVE_AVCODEC
    if (strcmp(codec, "AAC") == 0 || strcmp(codec, "AAC+") == 0)
        return 1;
#endif
    return 0;
}

/* ---- stations ---- */

typedef struct {
    RadioStation *out;
    int max;
    int count;
} StationCtx;

static void
station_visit(const Doc *d, int obj, void *vctx)
{
    StationCtx *c = (StationCtx *)vctx;
    RadioStation s;
    int v, k;

    if (c->count >= c->max)
        return;
    memset(&s, 0, sizeof s);

    v = obj_value(d, obj, "url_resolved");
    if (v < 0 || tok_copy(d, v, s.url, sizeof s.url) != 0)
        return;
    if (strncmp(s.url, "http://", 7) != 0 && strncmp(s.url, "https://", 8) != 0)
        return;

    v = obj_value(d, obj, "codec");
    if (v < 0 || tok_copy(d, v, s.codec, sizeof s.codec) != 0)
        return;
    for (k = 0; s.codec[k]; k++) {
        if (s.codec[k] >= 'a' && s.codec[k] <= 'z')
            s.codec[k] = (char)(s.codec[k] - 'a' + 'A');
    }
    if (!radio_codec_supported(s.codec))
        return;

    v = obj_value(d, obj, "name");
    if (v < 0 || tok_copy(d, v, s.name, sizeof s.name) != 0 || !s.name[0])
        snprintf(s.name, sizeof s.name, "%s", "(unnamed station)");

    v = obj_value(d, obj, "country");
    if (v >= 0)
        tok_copy(d, v, s.country, sizeof s.country);

    v = obj_value(d, obj, "bitrate");
    if (v >= 0 && d->tok[v].type == JSMN_PRIMITIVE) {
        char num[16];
        int len = d->tok[v].end - d->tok[v].start;

        if (len > 0 && len < (int)sizeof num) {
            memcpy(num, d->json + d->tok[v].start, (size_t)len);
            num[len] = '\0';
            s.bitrate = atoi(num);
        }
    } else if (v >= 0) {
        char num[16];

        if (tok_copy(d, v, num, sizeof num) == 0)
            s.bitrate = atoi(num);
    }

    c->out[c->count++] = s;
}

int
radio_parse_stations(const char *json, size_t len, RadioStation *out, int max)
{
    Doc d;
    StationCtx c = { out, max, 0 };

    if (!out || max <= 0)
        return -1;
    if (doc_parse(&d, json, len) != 0)
        return -1;
    if (d.n == 0 || d.tok[0].type != JSMN_ARRAY) {
        doc_free(&d);
        return -1;
    }
    for_each_root_object(&d, station_visit, &c);
    doc_free(&d);
    return c.count;
}
