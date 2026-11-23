/* net.c — libcurl wrapper. See net.h.
 *
 * Threading: net_open and net_fetch run on the UI thread. A stream runs its
 * transfer on one SDL thread that writes into a bounded ring buffer; net_read
 * (decoder thread) and net_abort (any thread) meet it through one mutex and
 * condition variable. Nothing here touches the playback atomics.
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#define NET_RING_BYTES   (512 * 1024)
#define NET_CONNECT_S    5
#define NET_FIRST_MS     8000   /* headers must arrive within this */
#define NET_STALL_S      15     /* a stream slower than 1 B/s this long fails */
#define NET_FETCH_MAX_S  10
#define NET_CTYPE_MAX    64

#ifdef HAVE_CURL

struct NetStream {
    CURL *easy;
    struct curl_slist *hdrs;
    SDL_Thread *thread;
    SDL_mutex *mu;
    SDL_cond *cv;

    unsigned char *ring;
    size_t head;   /* read index */
    size_t count;  /* bytes buffered */

    int headers_done; /* header block complete: ctype is final */
    int ready;        /* headers done, or the transfer ended */
    int done;         /* curl_easy_perform returned */
    int aborted;
    CURLcode result;
    int joined;

    char ctype[NET_CTYPE_MAX];
};

static int curl_inited;

static int
curl_init_once(char *err, size_t errlen)
{
    if (curl_inited)
        return 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        snprintf(err, errlen, "cannot initialise networking");
        return -1;
    }
    curl_inited = 1;
    return 0;
}

int
net_available(void)
{
    return 1;
}

static void
set_common(CURL *easy, const char *url)
{
    char ua[128];

    curl_easy_setopt(easy, CURLOPT_URL, url);
    /* Only plain web schemes, including on redirect: a redirect to file:// or
     * ftp:// is refused rather than followed. */
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, (long)NET_CONNECT_S);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    /* Cookies stay off: no COOKIEFILE or COOKIEJAR is ever set. */
    snprintf(ua, sizeof ua, "Aldrem/%s",
#ifdef APP_VERSION
             APP_VERSION
#else
             "dev"
#endif
             );
    curl_easy_setopt(easy, CURLOPT_USERAGENT, ua);
}

/* Progress callback: runs about once a second even on a stalled socket, so an
 * abort ends a blocked transfer within that much time. */
static int
xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
            curl_off_t ultotal, curl_off_t ulnow)
{
    NetStream *ns = (NetStream *)clientp;
    int stop;

    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    SDL_LockMutex(ns->mu);
    stop = ns->aborted;
    SDL_UnlockMutex(ns->mu);
    return stop;
}

static size_t
header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    NetStream *ns = (NetStream *)userdata;
    size_t n = size * nitems;
    static const char key[] = "content-type:";
    size_t i, o = 0;

    /* A blank line ends one response's header block. Only a 2xx response is
     * the stream: a redirect (followed or refused) or an error status must not
     * open the stream early, so the transfer's own result decides for those. */
    if (n <= 2 && (buffer[0] == '\r' || buffer[0] == '\n')) {
        long code = 0;

        curl_easy_getinfo(ns->easy, CURLINFO_RESPONSE_CODE, &code);
        if (code < 200 || code >= 300)
            return n;
        SDL_LockMutex(ns->mu);
        ns->headers_done = 1;
        ns->ready = 1;
        SDL_CondBroadcast(ns->cv);
        SDL_UnlockMutex(ns->mu);
        return n;
    }

    if (n < sizeof key - 1 || SDL_strncasecmp(buffer, key, sizeof key - 1) != 0)
        return n;

    /* Value runs to the first ';' (parameters such as charset are dropped). */
    for (i = sizeof key - 1; i < n && (buffer[i] == ' ' || buffer[i] == '\t'); i++)
        ;
    for (; i < n && buffer[i] != ';' && buffer[i] != '\r' && buffer[i] != '\n'
           && o + 1 < sizeof ns->ctype; i++) {
        char c = buffer[i];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        ns->ctype[o++] = c;
    }
    while (o > 0 && (ns->ctype[o - 1] == ' ' || ns->ctype[o - 1] == '\t'))
        o--;
    ns->ctype[o] = '\0';
    return n;
}

static size_t
write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    NetStream *ns = (NetStream *)userdata;
    size_t total = size * nmemb, off = 0;

    SDL_LockMutex(ns->mu);
    while (off < total) {
        size_t space, tail, chunk, wi;

        while (ns->count == NET_RING_BYTES && !ns->aborted)
            SDL_CondWait(ns->cv, ns->mu);
        if (ns->aborted) {
            SDL_UnlockMutex(ns->mu);
            return 0; /* curl reports a write error and stops */
        }

        space = NET_RING_BYTES - ns->count;
        wi = (ns->head + ns->count) % NET_RING_BYTES;
        tail = NET_RING_BYTES - wi;
        chunk = total - off;
        if (chunk > space)
            chunk = space;
        if (chunk > tail)
            chunk = tail;

        memcpy(ns->ring + wi, ptr + off, chunk);
        ns->count += chunk;
        off += chunk;
        ns->ready = 1;
        SDL_CondBroadcast(ns->cv);
    }
    SDL_UnlockMutex(ns->mu);
    return total;
}

static int
transfer_thread(void *data)
{
    NetStream *ns = (NetStream *)data;
    CURLcode res = curl_easy_perform(ns->easy);

    SDL_LockMutex(ns->mu);
    ns->result = res;
    ns->done = 1;
    ns->ready = 1;
    SDL_CondBroadcast(ns->cv);
    SDL_UnlockMutex(ns->mu);
    return 0;
}

static void
stream_free(NetStream *ns)
{
    if (!ns)
        return;
    if (ns->thread && !ns->joined)
        SDL_WaitThread(ns->thread, NULL);
    if (ns->easy)
        curl_easy_cleanup(ns->easy);
    if (ns->hdrs)
        curl_slist_free_all(ns->hdrs);
    if (ns->cv)
        SDL_DestroyCond(ns->cv);
    if (ns->mu)
        SDL_DestroyMutex(ns->mu);
    free(ns->ring);
    free(ns);
}

NetStream *
net_open(const char *url, char *err, size_t errlen)
{
    NetStream *ns;
    int waited = 0;
    Uint32 start;

    if (err && errlen)
        err[0] = '\0';
    if (!url || !url[0]) {
        snprintf(err, errlen, "no stream address");
        return NULL;
    }
    if (curl_init_once(err, errlen) != 0)
        return NULL;

    ns = (NetStream *)calloc(1, sizeof *ns);
    if (!ns) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    ns->ring = (unsigned char *)malloc(NET_RING_BYTES);
    ns->mu = SDL_CreateMutex();
    ns->cv = SDL_CreateCond();
    ns->easy = curl_easy_init();
    if (!ns->ring || !ns->mu || !ns->cv || !ns->easy) {
        snprintf(err, errlen, "out of memory");
        stream_free(ns);
        return NULL;
    }

    set_common(ns->easy, url);
    /* Live streams never end, so no total timeout; a stall is caught below. */
    curl_easy_setopt(ns->easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(ns->easy, CURLOPT_LOW_SPEED_TIME, (long)NET_STALL_S);
    curl_easy_setopt(ns->easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(ns->easy, CURLOPT_WRITEDATA, ns);
    curl_easy_setopt(ns->easy, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(ns->easy, CURLOPT_HEADERDATA, ns);
    curl_easy_setopt(ns->easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(ns->easy, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(ns->easy, CURLOPT_XFERINFODATA, ns);
    /* Ask for no in-stream metadata: this player does not parse ICY blocks. */
    ns->hdrs = curl_slist_append(NULL, "Icy-MetaData: 0");
    curl_easy_setopt(ns->easy, CURLOPT_HTTPHEADER, ns->hdrs);

    ns->thread = SDL_CreateThread(transfer_thread, "net", ns);
    if (!ns->thread) {
        snprintf(err, errlen, "cannot start network thread");
        stream_free(ns);
        return NULL;
    }

    /* Wait for the headers (or the first body bytes, or a failure). The
     * transfer thread signals every change through ns->cv. */
    start = SDL_GetTicks();
    SDL_LockMutex(ns->mu);
    while (!ns->ready) {
        Uint32 el = SDL_GetTicks() - start;

        if (el >= NET_FIRST_MS) {
            waited = 1;
            break;
        }
        SDL_CondWaitTimeout(ns->cv, ns->mu, NET_FIRST_MS - el);
    }
    SDL_UnlockMutex(ns->mu);

    if (waited) {
        snprintf(err, errlen, "no response from the station");
        net_close(ns);
        return NULL;
    }
    SDL_LockMutex(ns->mu);
    if (ns->done && ns->result != CURLE_OK) {
        CURLcode res = ns->result;

        SDL_UnlockMutex(ns->mu);
        snprintf(err, errlen, "cannot open stream: %s", curl_easy_strerror(res));
        net_close(ns);
        return NULL;
    }
    SDL_UnlockMutex(ns->mu);
    return ns;
}

/* Synchronous fetch: accumulates into the caller's buffer and refuses to
 * overflow it. */
typedef struct {
    char *out;
    size_t cap;
    size_t len;
    int overflow;
} FetchBuf;

static size_t
fetch_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    FetchBuf *fb = (FetchBuf *)userdata;
    size_t n = size * nmemb;

    if (n > fb->cap - 1 - fb->len) {
        fb->overflow = 1;
        return 0;
    }
    memcpy(fb->out + fb->len, ptr, n);
    fb->len += n;
    return n;
}

typedef struct {
    NetCancelFn cancelled;
    void *ctx;
} FetchCancel;

static int
fetch_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t ultotal, curl_off_t ulnow)
{
    FetchCancel *fc = (FetchCancel *)clientp;

    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return fc->cancelled(fc->ctx) ? 1 : 0;
}

int
net_fetch_cancellable(const char *url, char *out, size_t outcap,
                      size_t *out_len, char *err, size_t errlen,
                      NetCancelFn cancelled, void *ctx)
{
    CURL *easy;
    FetchBuf fb;
    FetchCancel fc;
    CURLcode res;

    if (err && errlen)
        err[0] = '\0';
    if (out_len)
        *out_len = 0;
    if (!url || !out || outcap < 2) {
        snprintf(err, errlen, "invalid request");
        return -1;
    }
    if (curl_init_once(err, errlen) != 0)
        return -1;

    easy = curl_easy_init();
    if (!easy) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    fb.out = out;
    fb.cap = outcap;
    fb.len = 0;
    fb.overflow = 0;

    set_common(easy, url);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, (long)NET_FETCH_MAX_S);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, fetch_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &fb);
    if (cancelled) {
        fc.cancelled = cancelled;
        fc.ctx = ctx;
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, fetch_progress_cb);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &fc);
    }
    res = curl_easy_perform(easy);
    curl_easy_cleanup(easy);

    if (fb.overflow) {
        snprintf(err, errlen, "response too large");
        return -1;
    }
    if (res != CURLE_OK) {
        snprintf(err, errlen, "request failed: %s", curl_easy_strerror(res));
        return -1;
    }
    out[fb.len] = '\0';
    if (out_len)
        *out_len = fb.len;
    return 0;
}

int
net_fetch(const char *url, char *out, size_t outcap, size_t *out_len,
          char *err, size_t errlen)
{
    return net_fetch_cancellable(url, out, outcap, out_len, err, errlen,
                                 NULL, NULL);
}

int
net_read(NetStream *ns, unsigned char *buf, int cap)
{
    size_t n, first;

    if (!ns || !buf || cap <= 0)
        return -1;

    SDL_LockMutex(ns->mu);
    while (ns->count == 0 && !ns->done && !ns->aborted)
        SDL_CondWait(ns->cv, ns->mu);
    if (ns->aborted) {
        SDL_UnlockMutex(ns->mu);
        return -1;
    }
    if (ns->count == 0) {
        int ok = ns->result == CURLE_OK;

        SDL_UnlockMutex(ns->mu);
        return ok ? 0 : -1;
    }

    n = ns->count < (size_t)cap ? ns->count : (size_t)cap;
    first = NET_RING_BYTES - ns->head;
    if (first > n)
        first = n;
    memcpy(buf, ns->ring + ns->head, first);
    memcpy(buf + first, ns->ring, n - first);
    ns->head = (ns->head + n) % NET_RING_BYTES;
    ns->count -= n;
    SDL_CondBroadcast(ns->cv); /* the writer may be waiting for room */
    SDL_UnlockMutex(ns->mu);
    return (int)n;
}

const char *
net_content_type(const NetStream *ns)
{
    return ns ? ns->ctype : "";
}

void
net_abort(NetStream *ns)
{
    if (!ns)
        return;
    SDL_LockMutex(ns->mu);
    ns->aborted = 1;
    SDL_CondBroadcast(ns->cv);
    SDL_UnlockMutex(ns->mu);
}

void
net_close(NetStream *ns)
{
    if (!ns)
        return;
    net_abort(ns);
    if (ns->thread) {
        SDL_WaitThread(ns->thread, NULL);
        ns->joined = 1;
    }
    stream_free(ns);
}

#else /* !HAVE_CURL */

struct NetStream {
    int unused;
};

int
net_available(void)
{
    return 0;
}

NetStream *
net_open(const char *url, char *err, size_t errlen)
{
    (void)url;
    if (err && errlen)
        snprintf(err, errlen, "this build has no network support");
    return NULL;
}

int
net_read(NetStream *ns, unsigned char *buf, int cap)
{
    (void)ns; (void)buf; (void)cap;
    return -1;
}

const char *
net_content_type(const NetStream *ns)
{
    (void)ns;
    return "";
}

void
net_abort(NetStream *ns)
{
    (void)ns;
}

void
net_close(NetStream *ns)
{
    (void)ns;
}

int
net_fetch(const char *url, char *out, size_t outcap, size_t *out_len,
          char *err, size_t errlen)
{
    (void)url; (void)out; (void)outcap; (void)out_len;
    if (err && errlen)
        snprintf(err, errlen, "this build has no network support");
    return -1;
}

#endif
