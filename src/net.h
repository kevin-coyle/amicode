/* net.h - AmiSSL + bsdsocket lifecycle and HTTPS transport for amicode.
 *
 * Target: AmigaOS 3.2.x / m68k, vbcc. OS4/PPC paths are intentionally absent.
 *
 * AmiSSL v5 spans TWO library bases on OS3 (AmiSSLBase + AmiSSLExtBase) because
 * OpenSSL has more public functions than one library jump table can hold.
 * Both are opened in a single OpenAmiSSLTagList() call against amisslmaster.
 *
 * Library open order is load-bearing (see README "Platform landmines"):
 *   utility -> bsdsocket -> amisslmaster -> OpenAmiSSLTagList (AmiSSL+Ext).
 * Teardown is the exact reverse.
 */
#ifndef AMICODE_NET_H
#define AMICODE_NET_H

#include <exec/types.h>

/* AmiSSL/bsdsocket library bases. The AmiSSL and bsdsocket inline headers
 * reference these by their exact symbol names, so they are real globals,
 * defined once in net.c and declared extern here. proto/amissl.h itself
 * declares AmiSSLBase/AmiSSLExtBase extern; we provide the definitions. */
extern struct Library *SocketBase;
extern struct Library *AmiSSLMasterBase;
extern struct Library *AmiSSLBase;
extern struct Library *AmiSSLExtBase;
extern struct Library *UtilityBase;

/* When TRUE, https_post() prints the negotiated TLS version + cipher per
 * request. Off by default (noisy in the REPL); enabled with amicode -v. */
extern BOOL net_verbose;

/* Bring up utility + bsdsocket + AmiSSL, seed the RNG, and build the shared
 * verified TLS context (SSL_VERIFY_PEER against AmiSSL's CA store). Returns
 * TRUE on success, FALSE otherwise (everything torn down on failure). */
BOOL net_init(void);

/* Tear down the TLS context + AmiSSL + bsdsocket in reverse order. Idempotent. */
void net_cleanup(void);

/* Parsed HTTP response from https_post(). Free body with http_response_free(). */
struct HttpResponse
{
    LONG  status;    /* parsed HTTP status code, e.g. 401, or -1 on parse fail */
    UBYTE *body;     /* de-chunked, NUL-terminated body (malloc'd), or NULL    */
    LONG  body_len;  /* length of body in bytes (excluding the NUL)            */
};

/* One-shot HTTPS POST over a freshly opened, certificate-verified TLS
 * connection to host:443.
 *
 *   path        e.g. "/v1/messages"
 *   extra_hdrs  additional header lines, each terminated with CRLF, or NULL.
 *               (Host, Content-Length and Connection are added automatically.)
 *   body        request body bytes (may be NULL if body_len == 0)
 *   body_len    length of body
 *   resp        filled in on success; caller must http_response_free() it.
 *
 * Prints the negotiated cipher on a successful handshake. Returns TRUE if a
 * response was received and parsed (any HTTP status), FALSE on a transport or
 * TLS failure. */
BOOL https_post(const char *host, const char *path,
                const char *extra_hdrs,
                const UBYTE *body, LONG body_len,
                struct HttpResponse *resp);

/* Release the body buffer held by a response (safe on a zeroed/failed resp). */
void http_response_free(struct HttpResponse *resp);

/* Callback invoked once per SSE "data:" line with the JSON payload (the text
 * after "data: "). userdata is passed through from https_post_sse. */
typedef void (*NetSseCallback)(const char *data_json, void *userdata);

/* Like https_post but with a streaming (text/event-stream) response: the caller
 * sets "stream": true in `body`, and `cb` is invoked for each SSE event as it
 * arrives. De-chunks on the fly.
 *
 * On a non-2xx response, no events are dispatched; instead the (decoded) error
 * body is returned via *out_errbody (malloc'd, caller frees) for the caller to
 * parse. *out_status receives the HTTP status (or -1).
 *
 * Returns TRUE if an HTTP response was received (check *out_status), FALSE on a
 * transport/TLS failure. */
BOOL https_post_sse(const char *host, const char *path,
                    const char *extra_hdrs,
                    const UBYTE *body, LONG body_len,
                    NetSseCallback cb, void *userdata,
                    LONG *out_status, char **out_errbody);

/* Download `url` (http:// or https://) to `dest_path` on disk. Follows up to a
 * few redirects, de-chunks, and is binary-safe (suitable for .lha archives,
 * etc.). https connections are certificate-verified via the shared context.
 *
 * Returns TRUE only on a 2xx response whose body was written to disk. On any
 * outcome, *out_status (if non-NULL) receives the final HTTP status (or -1 if
 * no response was obtained), and *out_len (if non-NULL) the bytes written. */
BOOL http_download(const char *url, const char *dest_path,
                   LONG *out_status, LONG *out_len);

#endif /* AMICODE_NET_H */
