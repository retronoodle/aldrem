/* radio.h — Radio Browser (api.radio-browser.info) client, pure half: URL
 * building and JSON parsing. No network, SDL or Nuklear dependency, so it is
 * unit-testable from a fixture. Fetching is the caller's job (net.c). */
#ifndef RADIO_H
#define RADIO_H

#include <stddef.h>

#define RADIO_NAME_MAX   256
#define RADIO_URL_MAX    1024
#define RADIO_CODEC_MAX  16
#define RADIO_COUNTRY_MAX 64
#define RADIO_HOST_MAX   256
#define RADIO_MAX_RESULT 200  /* stations kept per search */

typedef struct {
    char name[RADIO_NAME_MAX];
    char url[RADIO_URL_MAX];     /* url_resolved: a direct stream URL */
    char codec[RADIO_CODEC_MAX]; /* upper-cased: "MP3", "OGG", "AAC", "AAC+" */
    char country[RADIO_COUNTRY_MAX];
    int  bitrate;                /* kbit/s, 0 when unknown */
} RadioStation;

/* Writes the mirror list's host names from the /json/servers body. Returns
 * the number written (0 on a bad body). Hosts are taken from the "name"
 * field, which is what the API documents for mirror selection. */
int radio_parse_mirrors(const char *json, size_t len,
                        char hosts[][RADIO_HOST_MAX], int max);

/* Builds the stations search URL on host, for a name query. Free text is
 * percent-encoded. Returns 0 on success, -1 when outlen is too small. */
int radio_search_url(const char *host, const char *query, int limit,
                     char *out, size_t outlen);

/* Nonzero when a station codec can be decoded in this build. MP3 and OGG
 * always; AAC variants only when built with HAVE_AVCODEC. */
int radio_codec_supported(const char *codec);

/* Parses a /json/stations/... body into out (at most max entries). Entries
 * without a usable http(s) url_resolved, or with an unsupported codec, are
 * skipped. Long strings are truncated to their buffers. Returns the number of
 * stations written, or -1 on malformed JSON. */
int radio_parse_stations(const char *json, size_t len,
                         RadioStation *out, int max);

#endif
