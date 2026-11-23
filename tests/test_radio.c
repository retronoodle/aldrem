/* test_radio.c — Radio Browser parsing and URL building, from fixtures. No
 * network: the bodies below are the shapes the API documents. */
#include "radio.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
                   failures++; } \
} while (0)

static void
test_mirrors(void)
{
    static const char body[] =
        "[{\"ip\":\"91.98.4.78\",\"name\":\"de1.api.radio-browser.info\"},"
        "{\"ip\":\"2a01::1\",\"name\":\"de1.api.radio-browser.info\"},"
        "{\"ip\":\"1.2.3.4\",\"name\":\"evil.example/../x\"},"
        "{\"ip\":\"5.6.7.8\",\"name\":\"nl1.api.radio-browser.info\"}]";
    char hosts[8][RADIO_HOST_MAX];
    int n = radio_parse_mirrors(body, strlen(body), hosts, 8);

    CHECK(n == 2, "mirrors: duplicate and unsafe names dropped");
    CHECK(n >= 1 && strcmp(hosts[0], "de1.api.radio-browser.info") == 0,
          "mirrors: first host kept");
    CHECK(n >= 2 && strcmp(hosts[1], "nl1.api.radio-browser.info") == 0,
          "mirrors: second host kept");
    CHECK(radio_parse_mirrors("not json", 8, hosts, 8) == 0,
          "mirrors: malformed body yields none");
    CHECK(radio_parse_mirrors("{}", 2, hosts, 8) == 0,
          "mirrors: object (not array) yields none");
}

static void
test_search_url(void)
{
    char url[512];

    CHECK(radio_search_url("de1.api.radio-browser.info", "jazz fm", 50,
                           url, sizeof url) == 0, "url: builds");
    CHECK(strcmp(url, "https://de1.api.radio-browser.info/json/stations/search"
                      "?name=jazz%20fm&hidebroken=true&order=clickcount"
                      "&reverse=true&limit=50") == 0, "url: encoded query");

    CHECK(radio_search_url("de1.api.radio-browser.info", "a&b=c/d?", 10,
                           url, sizeof url) == 0, "url: special chars build");
    CHECK(strstr(url, "name=a%26b%3Dc%2Fd%3F&") != NULL,
          "url: & = / ? are percent-encoded");

    CHECK(radio_search_url("evil.com/x?", "q", 10, url, sizeof url) != 0,
          "url: host with path characters refused");
    CHECK(radio_search_url("de1.api.radio-browser.info", "q", 10,
                           url, 16) != 0, "url: too-small buffer refused");
}

static void
test_stations(void)
{
    static const char body[] =
        "["
        "{\"name\":\"Jazz \\\"FM\\\"\",\"url_resolved\":\"https://s.example/jazz.mp3\","
        " \"codec\":\"mp3\",\"country\":\"Germany\",\"bitrate\":128},"
        "{\"name\":\"Ogg Only\",\"url_resolved\":\"http://s.example/o.ogg\","
        " \"codec\":\"OGG\",\"country\":\"\",\"bitrate\":0},"
        "{\"name\":\"No URL\",\"url_resolved\":\"\",\"codec\":\"MP3\",\"bitrate\":64},"
        "{\"name\":\"Bad scheme\",\"url_resolved\":\"ftp://s.example/x\",\"codec\":\"MP3\"},"
        "{\"name\":\"Flac\",\"url_resolved\":\"https://s.example/f\",\"codec\":\"FLAC\"},"
        "{\"url_resolved\":\"https://s.example/anon\",\"codec\":\"MP3\",\"bitrate\":\"96\"}"
        "]";
    RadioStation s[16];
    int n = radio_parse_stations(body, strlen(body), s, 16);

    CHECK(n == 3, "stations: keeps 3 usable of 6");
    if (n >= 1) {
        CHECK(strcmp(s[0].name, "Jazz \"FM\"") == 0, "stations: escaped quotes");
        CHECK(strcmp(s[0].codec, "MP3") == 0, "stations: codec upper-cased");
        CHECK(strcmp(s[0].country, "Germany") == 0, "stations: country");
        CHECK(s[0].bitrate == 128, "stations: numeric bitrate");
        CHECK(strcmp(s[0].url, "https://s.example/jazz.mp3") == 0, "stations: url");
    }
    if (n >= 2)
        CHECK(strcmp(s[1].codec, "OGG") == 0 && s[1].bitrate == 0,
              "stations: zero bitrate, OGG kept");
    if (n >= 3) {
        CHECK(strcmp(s[2].name, "(unnamed station)") == 0,
              "stations: missing name gets a placeholder");
        CHECK(s[2].bitrate == 96, "stations: string bitrate parsed");
    }

    CHECK(radio_parse_stations("[{\"url_resolved\":", 17, s, 16) == -1,
          "stations: truncated JSON is an error");
    CHECK(radio_parse_stations("{}", 2, s, 16) == -1,
          "stations: object root is an error");
    CHECK(radio_parse_stations("[]", 2, s, 16) == 0,
          "stations: empty array is zero results");
}

static void
test_cap_and_codecs(void)
{
    static const char body[] =
        "[{\"name\":\"A\",\"url_resolved\":\"https://a/1\",\"codec\":\"MP3\"},"
        " {\"name\":\"B\",\"url_resolved\":\"https://a/2\",\"codec\":\"MP3\"},"
        " {\"name\":\"C\",\"url_resolved\":\"https://a/3\",\"codec\":\"MP3\"}]";
    RadioStation s[2];

    CHECK(radio_parse_stations(body, strlen(body), s, 2) == 2,
          "cap: max entries respected");
    CHECK(radio_codec_supported("MP3") && radio_codec_supported("OGG"),
          "codec: MP3 and OGG supported");
    CHECK(!radio_codec_supported("FLAC") && !radio_codec_supported(NULL),
          "codec: others refused");
#ifndef HAVE_AVCODEC
    CHECK(!radio_codec_supported("AAC"), "codec: AAC refused without libavcodec");
#endif
}

int
main(void)
{
    test_mirrors();
    test_search_url();
    test_stations();
    test_cap_and_codecs();

    if (failures) {
        fprintf(stderr, "test_radio: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_radio: ok\n");
    return 0;
}
