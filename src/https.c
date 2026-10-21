/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * HTTPS client for Google Drive (see https.h). */
#include "https.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bearssl.h"
#include "google_ca.h"
#include "mem.h"
#include "public_roots.h"

#define IO_SECONDS 60     /* most a read or a write waits */
#define CONNECT_SECONDS 15
#define HEAD_MAX 16384

#ifdef __ORBIS__
/* libSceNet's resolver: the payload has no resolv.conf for libc's */
typedef struct {
    uint32_t s_addr;
} sce_in_addr_t;
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int rid, const char *host, sce_in_addr_t *addr, int timeout, int retry, int flags);
int sceNetResolverDestroy(int rid);
#endif

/* Socket buffers: the console's defaults hold about 16 KB, and over the ~30 ms to
 * Google that is ~0.5 MB/s whatever the line. 0 keeps the default. */
int g_https_rcvbuf = 4 << 20, g_https_sndbuf = 4 << 20;

static int resolve(const char *host, struct in_addr *out) {
    if (inet_pton(AF_INET, host, out) == 1)
        return 0;
#ifdef __ORBIS__
    static int pool = -1;
    if (pool < 0)
        pool = sceNetPoolCreate("ps4mount_dns", 4 * 4096, 0);
    int rid = pool < 0 ? -1 : sceNetResolverCreate("ps4mount", pool, 0);
    if (rid >= 0) {
        sce_in_addr_t a = {0};
        int rc = sceNetResolverStartNtoa(rid, host, &a, 5 * 1000 * 1000, 1, 0);
        sceNetResolverDestroy(rid);
        if (rc >= 0 && a.s_addr) {
            out->s_addr = a.s_addr;
            return 0;
        }
    }
#endif
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res)
        return -1;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

static void entropy(uint8_t *p, size_t n) {
    arc4random_buf(p, n); /* the kernel's (kern.arandom) on the console */
    /* mixed with the clock in case the source gave nothing */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    for (size_t i = 0; i < sizeof(ts) && i < n; i++)
        p[i] ^= ((uint8_t *)&ts)[i];
}

typedef struct {
    int fd, stall;
    volatile int *cancel;
    int tls, any;
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
} conn_t;

/* Waits until fd can be read (or written) for at most IO_SECONDS, a second at a time so
 * a cancel is seen. 0 when ready. */
static int wait_fd(conn_t *c, int write) {
    for (int i = 0; i < (c->stall > 0 ? c->stall : IO_SECONDS); i++) {
        if (c->cancel && *c->cancel)
            return -1;
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(c->fd, &fs);
        struct timeval tv = {1, 0};
        int r = select(c->fd + 1, write ? NULL : &fs, write ? &fs : NULL, NULL, &tv);
        if (r > 0)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
    }
    return -1;
}

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    conn_t *c = ctx;
    if (wait_fd(c, 0))
        return -1;
    ssize_t r = read(c->fd, buf, len);
    return r > 0 ? (int)r : -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    conn_t *c = ctx;
    if (wait_fd(c, 1))
        return -1;
    ssize_t w = write(c->fd, buf, len);
    return w > 0 ? (int)w : -1;
}

static const char *tls_error(int e, int any) {
    switch (e) {
    case BR_ERR_X509_EXPIRED:
        return "the server's certificate is not valid now: is the console's date right?";
    case BR_ERR_X509_NOT_TRUSTED:
        return any ? "the server's certificate is not from a known authority" : "the server's certificate is not from Google";
    case BR_ERR_X509_BAD_SERVER_NAME:
        return "the server's certificate is for another name";
    case BR_ERR_NO_RANDOM:
        return "no random source for TLS";
    case BR_ERR_IO:
        return "the connection was lost";
    default:
        return NULL;
    }
}

static void conn_err(conn_t *c, const char *what, char *err, size_t n) {
    int e = c->tls ? br_ssl_engine_last_error(&c->cc.eng) : 0;
    const char *t = tls_error(e, c->any);
    if (c->cancel && *c->cancel)
        snprintf(err, n, "cancelled");
    else if (t)
        snprintf(err, n, "%s: %s", what, t);
    else if (e)
        snprintf(err, n, "%s: TLS error %d", what, e);
    else
        snprintf(err, n, "%s: the connection was lost", what);
}

static conn_t *conn_open(const http_url_t *u, int any, volatile int *cancel, int stall, char *err, size_t n) {
    struct in_addr addr;
    if (resolve(u->host, &addr)) {
        snprintf(err, n, "%s could not be found: is the console online?", u->host);
        return NULL;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, n, "no socket (%s)", strerror(errno));
        return NULL;
    }
    if (g_https_rcvbuf)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &g_https_rcvbuf, sizeof(g_https_rcvbuf));
    if (g_https_sndbuf)
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &g_https_sndbuf, sizeof(g_https_sndbuf));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)u->port);
    sa.sin_addr = addr;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc && errno == EINPROGRESS) {
        /* a second at a time: a cancel (PS4Mount stopping) is not held up by it */
        rc = -1;
        for (int i = 0; i < CONNECT_SECONDS && !(cancel && *cancel); i++) {
            fd_set fs;
            FD_ZERO(&fs);
            FD_SET(fd, &fs);
            struct timeval tv = {1, 0};
            int r = select(fd + 1, NULL, &fs, NULL, &tv), soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (r == 1) {
                rc = !getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) && !soerr ? 0 : -1;
                break;
            }
            if (r < 0 && errno != EINTR)
                break;
        }
    }
    fcntl(fd, F_SETFL, fl);
    if (rc) {
        close(fd);
        snprintf(err, n, "%s did not answer: is the console online?", u->host);
        return NULL;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    conn_t *c = mem_alloc(sizeof(*c));
    if (!c) {
        close(fd);
        snprintf(err, n, "no memory for the connection");
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->stall = stall;
    c->cancel = cancel;
    c->tls = u->tls;
    c->any = any;
    if (c->tls) {
        if (any)
            br_ssl_client_init_full(&c->cc, &c->xc, PUB_TAs, PUB_TAs_NUM);
        else
            br_ssl_client_init_full(&c->cc, &c->xc, TAs, TAs_NUM);
        br_ssl_engine_set_buffer(&c->cc.eng, c->iobuf, sizeof(c->iobuf), 1);
        uint8_t seed[32];
        entropy(seed, sizeof(seed));
        br_ssl_engine_inject_entropy(&c->cc.eng, seed, sizeof(seed));
        br_ssl_client_reset(&c->cc, u->host, 0);
        br_sslio_init(&c->io, &c->cc.eng, sock_read, c, sock_write, c);
    }
    return c;
}

static void conn_close(conn_t *c) {
    if (!c)
        return;
    /* no TLS close_notify exchange: every request asks for "Connection: close", and
     * waiting for the server's would read the rest of a body nobody wants */
    close(c->fd);
    mem_free(c);
}

static int conn_write(conn_t *c, const void *p, size_t n) {
    if (c->tls)
        return br_sslio_write_all(&c->io, p, n);
    const uint8_t *b = p;
    while (n) {
        int w = sock_write(c, b, n);
        if (w <= 0)
            return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int conn_flush(conn_t *c) { return c->tls ? br_sslio_flush(&c->io) : 0; }

/* Some bytes into p; 0 at the end of the response, -1 on an error. */
static int conn_read(conn_t *c, void *p, size_t n) {
    if (c->tls) {
        int r = br_sslio_read(&c->io, p, n);
        if (r > 0)
            return r;
        /* a server that closes the connection after the body: the end, not an error */
        int e = br_ssl_engine_last_error(&c->cc.eng);
        return br_ssl_engine_current_state(&c->cc.eng) == BR_SSL_CLOSED && (e == BR_ERR_OK || e == BR_ERR_IO) ? 0 : -1;
    }
    if (wait_fd(c, 0))
        return -1;
    ssize_t r = read(c->fd, p, n);
    return r >= 0 ? (int)r : -1;
}

int http_parse_url(const char *url, http_url_t *u) {
    memset(u, 0, sizeof(*u));
    const char *h;
    if (!strncmp(url, "https://", 8)) {
        u->tls = 1;
        u->port = 443;
        h = url + 8;
    } else if (!strncmp(url, "http://", 7)) {
        u->port = 80;
        h = url + 7;
    } else
        return -1;
    size_t hl = strcspn(h, ":/");
    if (!hl || hl >= sizeof(u->host))
        return -1;
    memcpy(u->host, h, hl);
    h += hl;
    if (*h == ':') {
        u->port = atoi(h + 1);
        h += 1 + strspn(h + 1, "0123456789");
    }
    if (u->port <= 0 || u->port > 65535)
        return -1;
    u->path = *h == '/' ? h : "/";
    return 0;
}

void http_escape(buf_t *out, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.~", c))
            buf_put(out, s, 1);
        else {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            buf_put(out, e, 3);
        }
    }
}

void http_resp_free(http_resp_t *r) { buf_free(&r->body); }

/* The value of header name in the header block, copied to out. 0 when present. */
static int header(const char *head, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    for (const char *p = strchr(head, '\n'); p; p = strchr(p, '\n')) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            v += strspn(v, " \t");
            size_t l = strcspn(v, "\r\n");
            if (l >= n)
                l = n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return 0;
        }
    }
    return -1;
}

typedef struct {
    const http_req_t *q;
    http_resp_t *r;
    int over; /* the body went past HTTP_BODY_MAX */
} body_t;

static int deliver(body_t *b, const uint8_t *p, size_t n) {
    if (!n)
        return 0;
    if (b->q->sink && b->r->status >= 200 && b->r->status < 300)
        return b->q->sink(b->q->sink_ctx, p, n);
    if (b->r->body.len + n > HTTP_BODY_MAX) {
        b->over = 1;
        return 0; /* an error page or a list too long: the rest is dropped */
    }
    buf_put(&b->r->body, p, n);
    return 0;
}

/* Buffered reads of the body after the headers. */
typedef struct {
    conn_t *c;
    uint8_t buf[16384];
    size_t pos, len;
} reader_t;

static int rd_fill(reader_t *rd) {
    if (rd->pos < rd->len)
        return 1;
    int r = conn_read(rd->c, rd->buf, sizeof(rd->buf));
    if (r <= 0)
        return r;
    rd->pos = 0;
    rd->len = (size_t)r;
    return 1;
}

static int rd_line(reader_t *rd, char *line, size_t n) {
    size_t o = 0;
    for (;;) {
        if (rd_fill(rd) <= 0)
            return -1;
        char ch = (char)rd->buf[rd->pos++];
        if (ch == '\n')
            break;
        if (ch != '\r' && o + 1 < n)
            line[o++] = ch;
    }
    line[o] = 0;
    return 0;
}

/* The body: chunked, of a known length, or up to the end of the connection. */
static int read_body(reader_t *rd, body_t *b, int chunked, uint64_t length, char *err, size_t n) {
    if (chunked) {
        for (;;) {
            char line[64];
            if (rd_line(rd, line, sizeof(line))) {
                conn_err(rd->c, "the answer was cut", err, n);
                return -1;
            }
            uint64_t left = strtoull(line, NULL, 16);
            if (!left) {
                /* the trailers up to the empty line: the connection may carry another request */
                while (!rd_line(rd, line, sizeof(line)) && line[0])
                    ;
                return 0;
            }
            while (left) {
                if (rd_fill(rd) <= 0) {
                    conn_err(rd->c, "the answer was cut", err, n);
                    return -1;
                }
                size_t take = rd->len - rd->pos < left ? rd->len - rd->pos : (size_t)left;
                if (deliver(b, rd->buf + rd->pos, take)) {
                    snprintf(err, n, "stopped");
                    return -1;
                }
                rd->pos += take;
                left -= take;
            }
            rd_line(rd, line, sizeof(line)); /* the chunk's CRLF */
        }
    }
    uint64_t got = 0;
    while (length == UINT64_MAX || got < length) {
        int f = rd_fill(rd);
        if (f == 0 && length == UINT64_MAX)
            return 0;
        if (f <= 0) {
            conn_err(rd->c, "the answer was cut", err, n);
            return -1;
        }
        size_t take = rd->len - rd->pos;
        if (length != UINT64_MAX && take > length - got)
            take = (size_t)(length - got);
        if (deliver(b, rd->buf + rd->pos, take)) {
            snprintf(err, n, "stopped");
            return -1;
        }
        rd->pos += take;
        got += take;
    }
    return 0;
}

struct http_keep {
    conn_t *c;
    char host[256];
    int port, tls, any;
};

http_keep_t *http_keep_new(void) {
    http_keep_t *k = mem_alloc(sizeof(*k));
    if (k)
        memset(k, 0, sizeof(*k));
    return k;
}

void http_keep_free(http_keep_t *k) {
    if (!k)
        return;
    conn_close(k->c);
    mem_free(k);
}

/* One request on c. *answered: a status line arrived (a connection the server closed
 * while it was idle fails before that, and the request can go again on a new one).
 * *reusable: the answer was read to its end and the server keeps the connection. */
static int request_on(conn_t *c, const http_req_t *q, const http_url_t *u, int keep, http_resp_t *r, int *answered,
                      int *reusable, char *err, size_t n) {
    *answered = *reusable = 0;
    buf_t h = {0};
    int body = q->body_len || strcmp(q->method, "GET");
    buf_printf(&h, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PS4Mount\r\nConnection: %s\r\n", q->method, u->path,
               u->host, keep ? "keep-alive" : "close");
    if (body)
        buf_printf(&h, "Content-Length: %zu\r\n", q->body_len);
    if (q->headers)
        buf_put(&h, q->headers, strlen(q->headers));
    buf_put(&h, "\r\n", 2);
    int rc = conn_write(c, h.data, h.len);
    buf_free(&h);
    /* the body in pieces: a cancel between them is seen */
    for (size_t o = 0; !rc && o < q->body_len; o += 65536) {
        size_t l = q->body_len - o < 65536 ? q->body_len - o : 65536;
        rc = conn_write(c, (const uint8_t *)q->body + o, l);
    }
    if (!rc)
        rc = conn_flush(c);
    if (rc) {
        conn_err(c, "the request could not be sent", err, n);
        return -1;
    }
    reader_t *rd = mem_alloc(sizeof(*rd));
    char *head = mem_alloc(HEAD_MAX);
    if (!rd || !head) {
        mem_free(rd);
        mem_free(head);
        snprintf(err, n, "no memory for the answer");
        return -1;
    }
    rd->c = c;
    rd->pos = rd->len = 0;
    rc = -1;
    size_t hl = 0;
    char line[4096];
    /* the status line and the headers (an interim 100 Continue is skipped) */
    for (;;) {
        if (rd_line(rd, line, sizeof(line))) {
            conn_err(c, "no answer", err, n);
            goto out;
        }
        if (!hl && !strncmp(line, "HTTP/1.1 100", 12)) {
            while (!rd_line(rd, line, sizeof(line)) && line[0])
                ;
            continue;
        }
        if (!line[0])
            break;
        size_t ll = strlen(line);
        if (hl + ll + 2 < HEAD_MAX) {
            memcpy(head + hl, line, ll);
            head[hl + ll] = '\n';
            hl += ll + 1;
        }
    }
    head[hl] = 0;
    if (strncmp(head, "HTTP/1.", 7) || !(r->status = atoi(head + 9))) {
        snprintf(err, n, "not an HTTP answer");
        goto out;
    }
    *answered = 1;
    char v[64];
    if (!header(head, "Content-Length", v, sizeof(v)))
        r->length = strtoull(v, NULL, 10);
    header(head, "Location", r->location, sizeof(r->location));
    header(head, "Range", r->range, sizeof(r->range));
    header(head, "Content-Range", r->content_range, sizeof(r->content_range));
    header(head, "Content-Disposition", r->disposition, sizeof(r->disposition));
    header(head, "Content-Type", r->type, sizeof(r->type));
    header(head, "ETag", r->etag, sizeof(r->etag));
    int chunked = 0;
    if (!header(head, "Transfer-Encoding", v, sizeof(v))) {
        for (char *p = v; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        chunked = strstr(v, "chunked") != NULL;
    }
    int closing = !header(head, "Connection", v, sizeof(v)) && (v[0] == 'c' || v[0] == 'C');
    body_t b = {q, r, 0};
    int nobody = !strcmp(q->method, "HEAD") || r->status == 204 || r->status == 304;
    if (nobody)
        rc = 0;
    else
        rc = read_body(rd, &b, chunked, r->length, err, n);
    /* reusable: an answer of a known end, all of it read, and no close from the server */
    *reusable = !rc && !closing && (nobody || chunked || r->length != UINT64_MAX);
out:
    mem_free(rd);
    mem_free(head);
    return rc;
}

int http_request(const http_req_t *q, http_resp_t *r, char *err, size_t n) {
    memset(r, 0, sizeof(*r));
    r->length = UINT64_MAX;
    http_url_t u;
    if (http_parse_url(q->url, &u)) {
        snprintf(err, n, "bad URL");
        return -1;
    }
    http_keep_t *k = q->keep;
    for (int attempt = 0; attempt < 2; attempt++) {
        conn_t *c = NULL;
        int reused = 0;
        if (k && k->c && k->port == u.port && k->tls == u.tls && k->any == q->any_server && !strcmp(k->host, u.host)) {
            c = k->c;
            c->cancel = q->cancel;
            c->stall = q->stall;
            reused = 1;
        } else {
            if (k && k->c) {
                conn_close(k->c);
                k->c = NULL;
            }
            if (!(c = conn_open(&u, q->any_server, q->cancel, q->stall, err, n)))
                return -1;
        }
        if (k)
            k->c = NULL;
        int answered, reusable;
        int rc = request_on(c, q, &u, k != NULL, r, &answered, &reusable, err, n);
        if (!rc && k && reusable) {
            k->c = c;
            snprintf(k->host, sizeof(k->host), "%s", u.host);
            k->port = u.port;
            k->tls = u.tls;
            k->any = q->any_server;
        } else
            conn_close(c);
        /* a kept connection the server had closed fails before any answer: once more on a
         * new one (nothing of the body went to the sink) */
        if (rc && reused && !answered && !(q->cancel && *q->cancel)) {
            http_resp_free(r);
            memset(r, 0, sizeof(*r));
            r->length = UINT64_MAX;
            continue;
        }
        return rc;
    }
    return -1;
}
