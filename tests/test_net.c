/* test_net.c — net.c against a loopback HTTP server on 127.0.0.1. No outside
 * network. Built only when libcurl is available (see Makefile NET). */
#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <SDL.h>

#define BODY_BYTES (200 * 1024)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
                   failures++; } \
} while (0)

static int srv_fd;
static char srv_base[64];

static unsigned char
pattern(size_t i)
{
    return (unsigned char)((i * 7u) & 0xff);
}

static void
send_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, s, n, 0);

        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

static void
handle(int fd, const char *path)
{
    char head[512];
    size_t i;

    if (strcmp(path, "/ok") == 0) {
        int n = snprintf(head, sizeof head,
            "HTTP/1.1 200 OK\r\nContent-Type: Audio/MPEG; charset=x\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n", BODY_BYTES);
        unsigned char *body = malloc(BODY_BYTES);

        send_all(fd, head, (size_t)n);
        for (i = 0; i < BODY_BYTES; i++)
            body[i] = pattern(i);
        send_all(fd, (const char *)body, BODY_BYTES);
        free(body);
    } else if (strcmp(path, "/redir") == 0) {
        /* Must not be followed: file:// is outside the allowed schemes. */
        const char *r = "HTTP/1.1 302 Found\r\nLocation: file:///etc/hostname\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, r, strlen(r));
    } else if (strcmp(path, "/404") == 0) {
        const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
        send_all(fd, r, strlen(r));
    } else if (strcmp(path, "/slow") == 0) {
        /* Headers, then silence for 3 s: a stalled station. */
        const char *r = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n"
                        "Connection: close\r\n\r\n";
        send_all(fd, r, strlen(r));
        SDL_Delay(3000);
    } else if (strcmp(path, "/json") == 0) {
        const char *r = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: 11\r\nConnection: close\r\n\r\n"
                        "[{\"a\":\"b\"}]";
        send_all(fd, r, strlen(r));
    } else if (strcmp(path, "/big") == 0) {
        char *r = malloc(65536 + 512);
        size_t hl;

        hl = (size_t)snprintf(r, 512,
            "HTTP/1.1 200 OK\r\nContent-Length: 65536\r\nConnection: close\r\n\r\n");
        memset(r + hl, 'x', 65536);
        send_all(fd, r, hl + 65536);
        free(r);
    } else {
        const char *r = "HTTP/1.1 500 Oops\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, r, strlen(r));
    }
}

static void *
server_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = accept(srv_fd, NULL, NULL);
        char req[1024];
        ssize_t n, total = 0;
        char path[256] = "";

        if (fd < 0)
            break;
        while (total < (ssize_t)sizeof req - 1) {
            n = recv(fd, req + total, sizeof req - 1 - (size_t)total, 0);
            if (n <= 0)
                break;
            total += n;
            req[total] = '\0';
            if (strstr(req, "\r\n\r\n"))
                break;
        }
        if (total > 0 && sscanf(req, "GET %255s", path) == 1)
            handle(fd, path);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    return NULL;
}

static int
start_server(void)
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    pthread_t th;

    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(srv_fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(srv_fd, 8) != 0 ||
        getsockname(srv_fd, (struct sockaddr *)&a, &len) != 0)
        return -1;
    snprintf(srv_base, sizeof srv_base, "http://127.0.0.1:%d", ntohs(a.sin_port));
    if (pthread_create(&th, NULL, server_thread, NULL) != 0)
        return -1;
    pthread_detach(th);
    return 0;
}

static void
test_stream_ok(void)
{
    char url[128], err[160];
    NetStream *ns;
    unsigned char buf[8192];
    size_t got = 0, bad = 0;
    int n;

    snprintf(url, sizeof url, "%s/ok", srv_base);
    ns = net_open(url, err, sizeof err);
    CHECK(ns != NULL, "stream: opens");
    if (!ns) {
        fprintf(stderr, "  %s\n", err);
        return;
    }
    CHECK(strcmp(net_content_type(ns), "audio/mpeg") == 0,
          "stream: content type lower-cased, parameters dropped");
    while ((n = net_read(ns, buf, sizeof buf)) > 0) {
        int i;

        for (i = 0; i < n; i++) {
            if (buf[i] != pattern(got + (size_t)i))
                bad++;
        }
        got += (size_t)n;
    }
    CHECK(n == 0, "stream: clean end of stream");
    CHECK(got == BODY_BYTES, "stream: every byte delivered");
    CHECK(bad == 0, "stream: bytes match in order (ring wrap included)");
    net_close(ns);
}

static void
test_refusals(void)
{
    char url[128], err[160];
    NetStream *ns;

    snprintf(url, sizeof url, "%s/redir", srv_base);
    ns = net_open(url, err, sizeof err);
    CHECK(ns == NULL, "redirect to file:// is refused");

    snprintf(url, sizeof url, "%s/404", srv_base);
    ns = net_open(url, err, sizeof err);
    CHECK(ns == NULL && err[0], "HTTP 404 fails with a reason");

    ns = net_open("file:///etc/hostname", err, sizeof err);
    CHECK(ns == NULL, "file:// URL refused at open");
}

static void *
aborter(void *arg)
{
    SDL_Delay(300);
    net_abort((NetStream *)arg);
    return NULL;
}

static void
test_abort(void)
{
    char url[128], err[160];
    NetStream *ns;
    unsigned char buf[256];
    pthread_t th;
    Uint32 t0;
    int n;

    snprintf(url, sizeof url, "%s/slow", srv_base);
    ns = net_open(url, err, sizeof err);
    CHECK(ns != NULL, "slow: headers arrive, stream opens");
    if (!ns)
        return;
    pthread_create(&th, NULL, aborter, ns);
    t0 = SDL_GetTicks();
    n = net_read(ns, buf, sizeof buf); /* blocks until the abort */
    CHECK(n == -1, "abort: blocked read returns -1");
    CHECK(SDL_GetTicks() - t0 < 2500, "abort: read wakes promptly");
    pthread_join(th, NULL);
    t0 = SDL_GetTicks();
    net_close(ns);
    CHECK(SDL_GetTicks() - t0 < 2500, "abort: close joins promptly");
}

static void
test_fetch(void)
{
    char out[256], err[160];
    char small[1024];
    size_t len = 0;
    char url[128];

    snprintf(url, sizeof url, "%s/json", srv_base);
    CHECK(net_fetch(url, out, sizeof out, &len, err, sizeof err) == 0,
          "fetch: small body");
    CHECK(len == 11 && strcmp(out, "[{\"a\":\"b\"}]") == 0,
          "fetch: body and length");

    snprintf(url, sizeof url, "%s/big", srv_base);
    CHECK(net_fetch(url, small, sizeof small, &len, err, sizeof err) != 0,
          "fetch: body over the cap is refused");
    CHECK(strstr(err, "too large") != NULL, "fetch: reason says too large");

    CHECK(net_fetch("file:///etc/hostname", out, sizeof out, &len, err,
                    sizeof err) != 0, "fetch: file:// refused");
}

int
main(void)
{
    if (!net_available()) {
        printf("test_net: skipped (no network support in this build)\n");
        return 0;
    }
    if (start_server() != 0) {
        fprintf(stderr, "test_net: cannot start loopback server\n");
        return 1;
    }

    test_stream_ok();
    test_refusals();
    test_abort();
    test_fetch();

    if (failures) {
        fprintf(stderr, "test_net: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_net: ok\n");
    return 0;
}
