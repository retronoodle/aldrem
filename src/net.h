/* net.h — HTTP(S) access for internet radio and the Radio Browser API.
 * Built on libcurl when HAVE_CURL is defined; otherwise every call fails with
 * a clear message and net_available() reports 0, so the rest of the app can
 * hide network features. Scheme is restricted to http/https, redirects are
 * capped, cookies are off, and no credentials are ever sent. */
#ifndef NET_H
#define NET_H

#include <stddef.h>

typedef struct NetStream NetStream;

/* 1 when this build can reach the network. */
int net_available(void);

/* Opens a streaming GET. Blocks until the response headers arrive (bounded by
 * connect and first-byte timeouts) or the request fails. Returns NULL with a
 * reason in err on failure. The body is pulled with net_read. */
NetStream *net_open(const char *url, char *err, size_t errlen);

/* Blocks until at least one byte is available, then copies up to cap bytes.
 * Returns the count (> 0), 0 at end of stream, or -1 on error or after
 * net_abort. */
int net_read(NetStream *ns, unsigned char *buf, int cap);

/* Content-Type of the response, lower-cased and without parameters, or "".
 * Valid until net_close. */
const char *net_content_type(const NetStream *ns);

/* Wakes a blocked net_read and makes the transfer stop. Safe from any thread,
 * including while net_read runs on another thread. */
void net_abort(NetStream *ns);

/* Aborts, joins the transfer thread and frees everything. */
void net_close(NetStream *ns);

/* Synchronous GET of a small body (the JSON API). Fails when the body exceeds
 * max bytes. On success the body is NUL-terminated in out and its length is
 * returned via *out_len. Returns 0 on success, -1 on failure. */
int net_fetch(const char *url, char *out, size_t outcap, size_t *out_len,
              char *err, size_t errlen);

/* Reports whether the caller wants the fetch stopped. Called from the transfer
 * thread about once a second, so it must be cheap and thread-safe. */
typedef int (*NetCancelFn)(void *ctx);

/* net_fetch that also gives up once cancelled() returns nonzero, so a stop
 * does not wait out the full timeout. cancelled may be NULL. */
int net_fetch_cancellable(const char *url, char *out, size_t outcap,
                          size_t *out_len, char *err, size_t errlen,
                          NetCancelFn cancelled, void *ctx);

#endif
