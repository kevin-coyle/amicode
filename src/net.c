/* net.c - AmiSSL + bsdsocket lifecycle and HTTPS transport for amicode.
 *
 * Phase 1 scope:
 *   - bring up utility/bsdsocket/AmiSSL (both OS3 bases),
 *   - seed the RNG, build a shared certificate-verifying SSL_CTX,
 *   - https_post(): open a verified TLS connection to host:443 (with SNI),
 *     send an HTTP/1.1 POST, read the whole response, de-chunk it, and parse
 *     out the status code + body.
 *
 * The platform calls mirror the AmiSSL 5.27 SDK example (Examples/https.c) and
 * its OS3 autoinit glue, which are the authoritative references for vbcc/OS3.
 */

/* vbcc cannot expand AmiSSL's GCC statement-expression stdarg macros; force the
 * plain (TagList/...A) entry points and explicit TagItem arrays. */
#define NO_INLINE_STDARG

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/socket.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>

#include <dos/dos.h>
#include <clib/alib_protos.h>   /* RangeRand() (amiga.lib) */
#include <libraries/amisslmaster.h>
#include <amissl/amissl.h>
#include <amissl/tags.h>

#include <errno.h>   /* errno is provided by vbcc's vc.lib; do not redefine. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "net.h"

/* Library bases referenced by name from the AmiSSL/bsdsocket inline headers. */
struct Library *SocketBase        = NULL;
struct Library *AmiSSLMasterBase  = NULL;
struct Library *AmiSSLBase        = NULL;
struct Library *AmiSSLExtBase     = NULL;
struct Library *UtilityBase       = NULL;

/* Shared TLS context: CA store + verify settings loaded once, reused per
 * request (cert-store setup is expensive to repeat). */
static SSL_CTX *g_ctx = NULL;

/* Per-request TLS diagnostics; off by default (set via amicode -v). */
BOOL net_verbose = FALSE;

/* TRUE once OpenAmiSSLTagList() has succeeded (AmiSSLBase is open). */
static BOOL amissl_open = FALSE;

/* ---- entropy -------------------------------------------------------------
 * Amiga entropy is poor. Fold the wall clock, a couple of live addresses and
 * utility.library's RangeRand() into the (deliberately uninitialised) seed
 * buffer. Adequate for v0; a high-res timer.device E-clock source would be a
 * worthwhile upgrade. */
static void generate_seed(UBYTE *buf, int size)
{
    struct DateStamp ds;
    ULONG mix;
    int i;

    DateStamp(&ds);
    mix  = (ULONG)ds.ds_Days * 86400UL
         + (ULONG)ds.ds_Minute * 60UL
         + (ULONG)ds.ds_Tick;
    mix ^= (ULONG)buf ^ (ULONG)&ds ^ (ULONG)&mix;

    for (i = 0; i < size; i++)
    {
        mix = mix * 1103515245UL + 12345UL;        /* LCG stir */
        buf[i] ^= (UBYTE)(mix >> 16);
        buf[i] ^= (UBYTE)RangeRand(256);           /* utility.library */
    }
}

/* Certificate verification callback: report why a chain was rejected. */
static int verify_cb(int preverify_ok, X509_STORE_CTX *ctx)
{
    if (!preverify_ok)
    {
        int err = X509_STORE_CTX_get_error(ctx);
        Printf("amicode: certificate verify failed: %s\n",
               (LONG)X509_verify_cert_error_string(err));
    }
    return preverify_ok;
}

BOOL net_init(void)
{
    LONG err;
    UBYTE seed[128];

    amissl_open = FALSE;
    g_ctx = NULL;

    if (!(UtilityBase = OpenLibrary("utility.library", 39)))
    {
        Printf("amicode: couldn't open utility.library\n");
        net_cleanup();
        return FALSE;
    }

    if (!(SocketBase = OpenLibrary("bsdsocket.library", 4)))
    {
        Printf("amicode: couldn't open bsdsocket.library v4 "
               "(is Roadshow/TCP-IP running?)\n");
        net_cleanup();
        return FALSE;
    }

    if (!(AmiSSLMasterBase = OpenLibrary("amisslmaster.library",
                                         AMISSLMASTER_MIN_VERSION)))
    {
        Printf("amicode: couldn't open amisslmaster.library v%ld "
               "(is AmiSSL v5 installed?)\n", (LONG)AMISSLMASTER_MIN_VERSION);
        net_cleanup();
        return FALSE;
    }

    /* One call opens AmiSSL and fills in both library bases for OS3. */
    {
        struct TagItem tags[] = {
            { AmiSSL_UsesOpenSSLStructs, (ULONG)TRUE          },
            { AmiSSL_GetAmiSSLBase,      (ULONG)&AmiSSLBase    },
            { AmiSSL_GetAmiSSLExtBase,   (ULONG)&AmiSSLExtBase },
            { AmiSSL_SocketBase,         (ULONG)SocketBase     },
            { AmiSSL_ErrNoPtr,           (ULONG)&errno         },
            { TAG_DONE,                  0                     }
        };

        err = OpenAmiSSLTagList(AMISSL_CURRENT_VERSION, tags);
    }

    switch (err)
    {
        case 0: amissl_open = TRUE; break;
        case 1: Printf("amicode: couldn't initialise amisslmaster.library "
                       "(AmiSSL too old? need v5.27)\n"); break;
        case 2: Printf("amicode: couldn't open AmiSSL\n"); break;
        case 3: Printf("amicode: couldn't initialise AmiSSL\n"); break;
        default: Printf("amicode: OpenAmiSSL failed (code %ld)\n", err); break;
    }
    if (!amissl_open) { net_cleanup(); return FALSE; }

    /* One-time crypto init + entropy seed (must precede SSL_new). */
    OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT
                     | OPENSSL_INIT_ADD_ALL_CIPHERS
                     | OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);
    generate_seed(seed, sizeof(seed));
    RAND_seed(seed, sizeof(seed));

    /* Shared verifying context. set_default_verify_paths picks up AmiSSL's
     * bundled Mozilla CA store (installed under the AmiSSL: assign). */
    if (!(g_ctx = SSL_CTX_new(TLS_client_method())))
    {
        Printf("amicode: SSL_CTX_new failed\n");
        net_cleanup();
        return FALSE;
    }
    SSL_CTX_set_default_verify_paths(g_ctx);
    SSL_CTX_set_mode(g_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, verify_cb);

    return TRUE;
}

void net_cleanup(void)
{
    if (g_ctx)
    {
        SSL_CTX_free(g_ctx);
        g_ctx = NULL;
    }

    if (amissl_open || AmiSSLBase)
    {
        /* CloseAmiSSL() (amisslmaster) closes both AmiSSLBase and
         * AmiSSLExtBase; never CloseLibrary() those two directly. */
        CloseAmiSSL();
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
        amissl_open = FALSE;
    }

    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (SocketBase)       { CloseLibrary(SocketBase);       SocketBase = NULL; }
    if (UtilityBase)      { CloseLibrary(UtilityBase);      UtilityBase = NULL; }
}

/* ---- TCP ----------------------------------------------------------------- */

static int tcp_connect(const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in addr;
    int sock;

    if (!(he = gethostbyname((char *)host)))
    {
        Printf("amicode: host lookup failed for %s\n", (LONG)host);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((UWORD)port);
    addr.sin_len    = he->h_length;
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        Printf("amicode: socket() failed\n");
        return -1;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        Printf("amicode: connect() to %s:%ld failed\n", (LONG)host, (LONG)port);
        CloseSocket(sock);
        return -1;
    }
    return sock;
}

/* ---- TLS I/O helpers ----------------------------------------------------- */

static BOOL ssl_write_all(SSL *ssl, const UBYTE *buf, LONG len)
{
    LONG off = 0;
    while (off < len)
    {
        int n = SSL_write(ssl, buf + off, (int)(len - off));
        if (n <= 0)
            return FALSE;
        off += n;
    }
    return TRUE;
}

/* Read until the peer closes the connection (we always send Connection: close).
 * Returns a malloc'd, NUL-terminated buffer; *out_len excludes the NUL. */
static UBYTE *ssl_read_all(SSL *ssl, LONG *out_len)
{
    LONG cap = 8192, len = 0;
    UBYTE *buf = malloc(cap);

    if (!buf)
        return NULL;

    for (;;)
    {
        int n;
        if (len + 4096 + 1 > cap)
        {
            UBYTE *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap *= 2;
        }
        n = SSL_read(ssl, buf + len, 4096);
        if (n <= 0)
            break;              /* clean close or error: response is complete */
        len += n;
    }

    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* ---- HTTP parsing -------------------------------------------------------- */

/* Case-insensitive search of a NUL-free region [hay, hay+n) for needle. */
static const UBYTE *mem_ci_find(const UBYTE *hay, LONG n, const char *needle)
{
    LONG nl = (LONG)strlen(needle);
    LONG i;
    if (nl == 0 || nl > n)
        return NULL;
    for (i = 0; i + nl <= n; i++)
    {
        LONG j;
        for (j = 0; j < nl; j++)
        {
            UBYTE a = hay[i + j];
            UBYTE b = (UBYTE)needle[j];
            if (a >= 'A' && a <= 'Z') a = (UBYTE)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (UBYTE)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nl)
            return hay + i;
    }
    return NULL;
}

/* Decode chunked transfer encoding in place. Returns the decoded length. */
static LONG dechunk(UBYTE *body, LONG len)
{
    UBYTE *src = body, *dst = body, *end = body + len;

    for (;;)
    {
        LONG sz = 0;

        /* hex chunk size, up to CR or a ';' chunk-extension */
        while (src < end && *src != '\r' && *src != ';')
        {
            UBYTE c = *src++;
            if      (c >= '0' && c <= '9') sz = sz * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') sz = sz * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') sz = sz * 16 + (c - 'A' + 10);
            else break;
        }
        /* advance past the size line's CRLF */
        while (src < end && *src != '\n') src++;
        if (src < end) src++;

        if (sz <= 0)
            break;                          /* terminating 0-length chunk */
        if (src + sz > end)
            sz = end - src;                 /* clamp against truncation */

        memmove(dst, src, sz);
        dst += sz;
        src += sz;

        while (src < end && (*src == '\r' || *src == '\n')) src++;  /* CRLF */
    }

    return (LONG)(dst - body);
}

/* ---- public: one HTTPS POST --------------------------------------------- */

BOOL https_post(const char *host, const char *path,
                const char *extra_hdrs,
                const UBYTE *body, LONG body_len,
                struct HttpResponse *resp)
{
    SSL  *ssl  = NULL;
    int   sock = -1;
    BOOL  ok   = FALSE;
    char *head = NULL;
    LONG  head_len;
    UBYTE *raw = NULL;
    LONG  raw_len = 0;

    if (resp)
    {
        resp->status   = -1;
        resp->body     = NULL;
        resp->body_len = 0;
    }
    if (!g_ctx || !resp)
        return FALSE;

    if (!(ssl = SSL_new(g_ctx)))
    {
        Printf("amicode: SSL_new failed\n");
        return FALSE;
    }

    if ((sock = tcp_connect(host, 443)) < 0)
        goto done;

    SSL_set_fd(ssl, sock);
    SSL_set_tlsext_host_name(ssl, (char *)host);   /* SNI - mandatory */

    if (SSL_connect(ssl) != 1)
    {
        Printf("amicode: TLS handshake with %s failed\n", (LONG)host);
        goto done;
    }
    if (net_verbose)
        Printf("TLS established: %s, cipher %s\n",
               (LONG)SSL_get_version(ssl), (LONG)SSL_get_cipher(ssl));

    /* Build the request head. extra_hdrs (if any) already ends each line with
     * CRLF; we append Content-Length + Connection and the blank separator. */
    {
        LONG cap = (LONG)strlen(path) + (LONG)strlen(host)
                 + (extra_hdrs ? (LONG)strlen(extra_hdrs) : 0) + 160;
        head = malloc(cap);
        if (!head)
            goto done;
        head_len = (LONG)sprintf(head,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "%s"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, extra_hdrs ? extra_hdrs : "", (long)body_len);
    }

    if (!ssl_write_all(ssl, (UBYTE *)head, head_len))
    {
        Printf("amicode: failed sending request headers\n");
        goto done;
    }
    if (body_len > 0 && !ssl_write_all(ssl, body, body_len))
    {
        Printf("amicode: failed sending request body\n");
        goto done;
    }

    if (!(raw = ssl_read_all(ssl, &raw_len)))
    {
        Printf("amicode: failed reading response (out of memory?)\n");
        goto done;
    }

    /* Split status/headers from body at the first CRLFCRLF. */
    {
        const UBYTE *sep = mem_ci_find(raw, raw_len, "\r\n\r\n");
        LONG hdr_len = sep ? (LONG)(sep - raw) : raw_len;
        UBYTE *bstart = sep ? (UBYTE *)sep + 4 : raw + raw_len;
        LONG blen = (LONG)(raw + raw_len - bstart);

        /* Status code: "HTTP/1.1 NNN ..." */
        {
            const UBYTE *sp = mem_ci_find(raw, hdr_len, " ");
            resp->status = sp ? atol((const char *)sp + 1) : -1;
        }

        /* De-chunk if the headers say so. */
        if (mem_ci_find(raw, hdr_len, "transfer-encoding:") &&
            mem_ci_find(raw, hdr_len, "chunked"))
        {
            blen = dechunk(bstart, blen);
        }

        resp->body = malloc(blen + 1);
        if (resp->body)
        {
            memcpy(resp->body, bstart, blen);
            resp->body[blen] = '\0';
            resp->body_len = blen;
            ok = TRUE;
        }
    }

done:
    if (raw)  free(raw);
    if (head) free(head);
    if (ssl)
    {
        if (sock >= 0)
            SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (sock >= 0)
        CloseSocket(sock);

    return ok;
}

void http_response_free(struct HttpResponse *resp)
{
    if (resp && resp->body)
    {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
}

/* ---- download: HTTP/HTTPS GET to a file --------------------------------- */

/* A transport that is either TLS (ssl != NULL) or a plain bsdsocket. */
struct Conn { int sock; SSL *ssl; };

struct dl_url { BOOL tls; char host[256]; int port; char path[1200]; };

static BOOL parse_url(const char *url, struct dl_url *u)
{
    const char *p = url, *slash, *colon, *hostend;
    LONG hlen;

    u->tls = FALSE;
    u->port = 80;
    if (strncmp(p, "https://", 8) == 0)      { u->tls = TRUE; u->port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0)  { p += 7; }
    else return FALSE;

    slash = strchr(p, '/');
    colon = strchr(p, ':');

    hostend = slash;
    if (colon && (!slash || colon < slash))
        hostend = colon;
    if (!hostend)
        hostend = p + strlen(p);

    hlen = (LONG)(hostend - p);
    if (hlen <= 0 || hlen >= (LONG)sizeof(u->host))
        return FALSE;
    memcpy(u->host, p, hlen);
    u->host[hlen] = '\0';

    if (colon && (!slash || colon < slash))
        u->port = (int)atol(colon + 1);

    if (slash)
    {
        strncpy(u->path, slash, sizeof(u->path) - 1);
        u->path[sizeof(u->path) - 1] = '\0';
    }
    else
    {
        strcpy(u->path, "/");
    }
    return TRUE;
}

static BOOL conn_open(struct Conn *c, const struct dl_url *u)
{
    c->sock = -1;
    c->ssl  = NULL;

    if ((c->sock = tcp_connect(u->host, u->port)) < 0)
        return FALSE;

    if (u->tls)
    {
        if (!(c->ssl = SSL_new(g_ctx)))
        {
            CloseSocket(c->sock); c->sock = -1;
            return FALSE;
        }
        SSL_set_fd(c->ssl, c->sock);
        SSL_set_tlsext_host_name(c->ssl, (char *)u->host);
        if (SSL_connect(c->ssl) != 1)
        {
            Printf("amicode: TLS handshake with %s failed\n", (LONG)u->host);
            SSL_free(c->ssl); c->ssl = NULL;
            CloseSocket(c->sock); c->sock = -1;
            return FALSE;
        }
    }
    return TRUE;
}

static void conn_close(struct Conn *c)
{
    if (c->ssl)  { SSL_shutdown(c->ssl); SSL_free(c->ssl); c->ssl = NULL; }
    if (c->sock >= 0) { CloseSocket(c->sock); c->sock = -1; }
}

static BOOL conn_write_all(struct Conn *c, const UBYTE *buf, LONG len)
{
    LONG off = 0;
    while (off < len)
    {
        int n = c->ssl ? SSL_write(c->ssl, buf + off, (int)(len - off))
                       : (int)send(c->sock, (UBYTE *)buf + off, len - off, 0);
        if (n <= 0)
            return FALSE;
        off += n;
    }
    return TRUE;
}

/* Read until the peer closes. Binary-safe; NUL-terminates for header parsing. */
static UBYTE *conn_read_all(struct Conn *c, LONG *out_len)
{
    LONG cap = 16384, len = 0;
    UBYTE *buf = malloc(cap);

    if (!buf)
        return NULL;

    for (;;)
    {
        int n;
        if (len + 8192 + 1 > cap)
        {
            UBYTE *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb; cap *= 2;
        }
        n = c->ssl ? SSL_read(c->ssl, buf + len, 8192)
                   : (int)recv(c->sock, buf + len, 8192, 0);
        if (n <= 0)
            break;
        len += n;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

BOOL http_download(const char *url, const char *dest_path,
                   LONG *out_status, LONG *out_len)
{
    char current[1400];
    int  redirects;

    if (out_status) *out_status = -1;
    if (out_len)    *out_len = 0;
    if (!g_ctx)
        return FALSE;

    strncpy(current, url, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    for (redirects = 0; redirects < 6; redirects++)
    {
        struct dl_url u;
        struct Conn   conn;
        char  hosthdr[300];
        char *req;
        UBYTE *raw = NULL;
        LONG  raw_len = 0, hdr_len, blen, status;
        const UBYTE *sep;
        UBYTE *bstart;

        if (!parse_url(current, &u))
        {
            Printf("amicode: bad URL: %s\n", (LONG)current);
            return FALSE;
        }

        if (!conn_open(&conn, &u))
            return FALSE;

        if ((u.tls && u.port == 443) || (!u.tls && u.port == 80))
            sprintf(hosthdr, "%s", u.host);
        else
            sprintf(hosthdr, "%s:%d", u.host, u.port);

        req = malloc((LONG)strlen(u.path) + (LONG)strlen(hosthdr) + 160);
        if (!req) { conn_close(&conn); return FALSE; }
        sprintf(req,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: amicode/0.1 (AmigaOS)\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            u.path, hosthdr);

        if (!conn_write_all(&conn, (UBYTE *)req, (LONG)strlen(req)))
        {
            free(req); conn_close(&conn);
            Printf("amicode: failed sending download request\n");
            return FALSE;
        }
        free(req);

        raw = conn_read_all(&conn, &raw_len);
        conn_close(&conn);
        if (!raw)
        {
            Printf("amicode: download read failed (out of memory?)\n");
            return FALSE;
        }

        sep     = mem_ci_find(raw, raw_len, "\r\n\r\n");
        hdr_len = sep ? (LONG)(sep - raw) : raw_len;
        bstart  = sep ? (UBYTE *)sep + 4 : raw + raw_len;
        blen    = (LONG)(raw + raw_len - bstart);

        {
            const UBYTE *spc = mem_ci_find(raw, hdr_len, " ");
            status = spc ? atol((const char *)spc + 1) : -1;
        }
        if (out_status) *out_status = status;

        /* Follow redirects via the Location header. */
        if (status >= 300 && status < 400)
        {
            const UBYTE *loc = mem_ci_find(raw, hdr_len, "\nlocation:");
            BOOL followed = FALSE;
            if (loc)
            {
                const UBYTE *v = loc + 10, *end;
                LONG vlen;
                char next[1400];

                while (v < raw + hdr_len && (*v == ' ' || *v == '\t')) v++;
                end = v;
                while (end < raw + hdr_len && *end != '\r' && *end != '\n') end++;
                vlen = (LONG)(end - v);

                if (vlen > 0 && vlen < (LONG)sizeof(next))
                {
                    memcpy(next, v, vlen);
                    next[vlen] = '\0';
                    if (strncmp(next, "http://", 7) == 0 ||
                        strncmp(next, "https://", 8) == 0)
                        sprintf(current, "%.1399s", next);
                    else if (next[0] == '/')
                        sprintf(current, "%s://%s%.1200s",
                                u.tls ? "https" : "http", u.host, next);
                    else
                        sprintf(current, "%s://%s/%.1200s",
                                u.tls ? "https" : "http", u.host, next);
                    Printf("  -> redirect to %s\n", (LONG)current);
                    followed = TRUE;
                }
            }
            free(raw);
            if (followed)
                continue;
            Printf("amicode: redirect (%ld) without usable Location\n", status);
            return FALSE;
        }

        if (status < 200 || status >= 300)
        {
            free(raw);
            Printf("amicode: download HTTP status %ld\n", status);
            return FALSE;
        }

        if (mem_ci_find(raw, hdr_len, "transfer-encoding:") &&
            mem_ci_find(raw, hdr_len, "chunked"))
            blen = dechunk(bstart, blen);

        {
            BPTR fh = Open((STRPTR)dest_path, MODE_NEWFILE);
            LONG wrote;
            if (!fh)
            {
                free(raw);
                Printf("amicode: couldn't open %s for writing\n", (LONG)dest_path);
                return FALSE;
            }
            wrote = Write(fh, bstart, blen);
            Close(fh);
            free(raw);
            if (wrote != blen)
            {
                Printf("amicode: short write to %s (disk full?)\n", (LONG)dest_path);
                return FALSE;
            }
            if (out_len) *out_len = blen;
            return TRUE;
        }
    }

    Printf("amicode: too many redirects\n");
    return FALSE;
}

/* ---- streaming SSE POST --------------------------------------------------
 * Reads a text/event-stream response, de-chunking on the fly and assembling
 * "data:" lines, invoking the callback per event. Non-2xx bodies are returned
 * whole for error parsing. */

struct sse_state {
    NetSseCallback cb;
    void          *ud;
    char          *line;     /* current SSE line buffer        */
    LONG           ll;        /* bytes in line                  */
    LONG           linecap;
    BOOL           is_error;  /* status != 200: accumulate body */
    char          *err;       /* error-body accumulator         */
    LONG           errlen, errcap;
};

static void sse_emit_line(struct sse_state *s)
{
    char *L = s->line;

    if (s->ll > 0 && L[s->ll - 1] == '\r')
        s->ll--;
    L[s->ll] = '\0';

    if (strncmp(L, "data:", 5) == 0)
    {
        char *d = L + 5;
        if (*d == ' ')
            d++;
        if (*d && strcmp(d, "[DONE]") != 0)
            s->cb(d, s->ud);
    }
    s->ll = 0;
}

/* Consume one fully-decoded body byte. */
static void sse_decoded_byte(struct sse_state *s, UBYTE b)
{
    if (s->is_error)
    {
        if (s->errlen + 2 > s->errcap)
        {
            LONG ncap = s->errcap ? s->errcap * 2 : 1024;
            char *nb = realloc(s->err, ncap);
            if (!nb) return;
            s->err = nb; s->errcap = ncap;
        }
        s->err[s->errlen++] = (char)b;
        s->err[s->errlen]   = '\0';
        return;
    }

    if (b == '\n')
        sse_emit_line(s);
    else if (s->ll < s->linecap - 1)
        s->line[s->ll++] = (char)b;
    /* else: drop the overflowing tail of an absurdly long line */
}

BOOL https_post_sse(const char *host, const char *path,
                    const char *extra_hdrs,
                    const UBYTE *body, LONG body_len,
                    NetSseCallback cb, void *userdata,
                    LONG *out_status, char **out_errbody)
{
    SSL  *ssl  = NULL;
    int   sock = -1;
    BOOL  ok   = FALSE;
    char *head = NULL;
    LONG  head_len, status = -1;
    struct sse_state s;

    /* chunked transfer-decoder state */
    int   cst = 0;           /* 0 size, 1 data, 2 trailCRLF, 3 done, 4 ext */
    long  crem = 0;
    char  csz[24];
    int   cszl = 0;
    BOOL  chunked = FALSE;

    /* header accumulation */
    UBYTE *hdr = NULL;
    LONG   hdrcap = 8192, hdrlen = 0;
    BOOL   headers_done = FALSE;
    LONG   body_start = 0;

    UBYTE rbuf[4096];
    int   n;

    if (out_status)  *out_status = -1;
    if (out_errbody) *out_errbody = NULL;
    if (!g_ctx)
        return FALSE;

    memset(&s, 0, sizeof(s));
    s.cb = cb; s.ud = userdata;
    s.linecap = 16384;
    s.line = malloc(s.linecap);
    if (!s.line)
        return FALSE;

    if (!(ssl = SSL_new(g_ctx))) { free(s.line); return FALSE; }
    if ((sock = tcp_connect(host, 443)) < 0) { SSL_free(ssl); free(s.line); return FALSE; }
    SSL_set_fd(ssl, sock);
    SSL_set_tlsext_host_name(ssl, (char *)host);
    if (SSL_connect(ssl) != 1)
    {
        Printf("amicode: TLS handshake with %s failed\n", (LONG)host);
        goto done;
    }

    {
        LONG cap = (LONG)strlen(path) + (LONG)strlen(host)
                 + (extra_hdrs ? (LONG)strlen(extra_hdrs) : 0) + 160;
        head = malloc(cap);
        if (!head) goto done;
        head_len = (LONG)sprintf(head,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "%s"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, extra_hdrs ? extra_hdrs : "", (long)body_len);
    }
    if (!ssl_write_all(ssl, (UBYTE *)head, head_len)) goto done;
    if (body_len > 0 && !ssl_write_all(ssl, body, body_len)) goto done;

    /* Phase A: read headers. */
    if (!(hdr = malloc(hdrcap))) goto done;
    while (!headers_done && (n = SSL_read(ssl, rbuf, sizeof(rbuf))) > 0)
    {
        if (hdrlen + n + 1 > hdrcap)
        {
            UBYTE *nb;
            while (hdrlen + n + 1 > hdrcap) hdrcap *= 2;
            nb = realloc(hdr, hdrcap);
            if (!nb) goto done;
            hdr = nb;
        }
        memcpy(hdr + hdrlen, rbuf, n);
        hdrlen += n;
        hdr[hdrlen] = '\0';

        {
            const UBYTE *sep = mem_ci_find(hdr, hdrlen, "\r\n\r\n");
            if (sep)
            {
                headers_done = TRUE;
                body_start = (LONG)(sep - hdr) + 4;
                {
                    const UBYTE *spc = mem_ci_find(hdr, (LONG)(sep - hdr), " ");
                    status = spc ? atol((const char *)spc + 1) : -1;
                }
                chunked = (mem_ci_find(hdr, (LONG)(sep - hdr), "transfer-encoding:") &&
                           mem_ci_find(hdr, (LONG)(sep - hdr), "chunked")) ? TRUE : FALSE;
                break;
            }
        }
    }

    if (!headers_done)
        goto done;

    if (out_status) *out_status = status;
    s.is_error = (status != 200);

    /* Phase B: feed body bytes (leftover after headers, then more reads) through
     * the chunk decoder (if chunked) into the SSE/error consumer. */
    {
        LONG pos = body_start;
        UBYTE *src = hdr;
        LONG   srclen = hdrlen;
        BOOL   first = TRUE;

        for (;;)
        {
            LONG i;
            if (!first)
            {
                n = SSL_read(ssl, rbuf, sizeof(rbuf));
                if (n <= 0) break;
                src = rbuf; srclen = n; pos = 0;
            }
            first = FALSE;

            for (i = pos; i < srclen; i++)
            {
                UBYTE b = src[i];
                if (!chunked)
                {
                    sse_decoded_byte(&s, b);
                    continue;
                }
                switch (cst)
                {
                    case 0:
                        if (b == '\r') ;
                        else if (b == '\n')
                        { csz[cszl] = 0; crem = strtol(csz, NULL, 16); cszl = 0;
                          cst = (crem == 0) ? 3 : 1; }
                        else if (b == ';') cst = 4;
                        else if (cszl < 23) csz[cszl++] = (char)b;
                        break;
                    case 4:
                        if (b == '\n')
                        { csz[cszl] = 0; crem = strtol(csz, NULL, 16); cszl = 0;
                          cst = (crem == 0) ? 3 : 1; }
                        break;
                    case 1:
                        sse_decoded_byte(&s, b);
                        if (--crem == 0) cst = 2;
                        break;
                    case 2:
                        if (b == '\n') cst = 0;
                        break;
                    case 3:
                    default:
                        break;
                }
            }
            if (chunked && cst == 3)
                break;
        }
    }

    ok = TRUE;
    if (s.is_error && out_errbody)
    {
        *out_errbody = s.err;
        s.err = NULL;
    }

done:
    if (s.err)  free(s.err);
    if (s.line) free(s.line);
    if (hdr)    free(hdr);
    if (head)   free(head);
    if (ssl)
    {
        if (sock >= 0) SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (sock >= 0) CloseSocket(sock);
    return ok;
}
