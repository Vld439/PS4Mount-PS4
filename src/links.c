/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Downloads from links (see links.h). Each file of a download comes in blocks, several
 * connections at a time, written in order into a hidden part file (exFAT fills a gap with
 * zeros first, which made out-of-order writes slow; see drive.c). An archive is read by
 * libarchive from those part files as they grow, and its pkgs are written beside them;
 * once all are whole they go to a folder of the scan folder in one rename. */
#include "links.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "archive.h"
#include "archive_entry.h"
#include "https.h"
#include "lang.h"
#include "mem.h"
#include "rar5dec.h"

#define TRIES 6           /* attempts of a request that fails for a passing reason */
#define WORK_DIR ".ps4mount-links"
#define PART_EXT ".dl"
#define SPACE_MARGIN (64ull << 20)
#define CHECK_BYTES (4u << 20) /* a pkg's header, entry table and param.sfo are in these */
#define MAX_THREADS 12
#define SLOTS (MAX_THREADS + 4) /* blocks that may wait in memory for an earlier one */
#define STALL 10          /* seconds without a byte before a connection is opened again */
#define REDIRECTS 10
#define URL_MAX 2048
#define XBUF (1u << 20)   /* what the archive reader takes from a part file at a time */
#define RELAY_CONNS 32    /* helper connections served at once */

size_t g_links_block = 8u << 20;
int g_links_retry_seconds = 2;
int g_links_threads = MAX_THREADS;
int g_links_relay_port = LINKS_RELAY_PORT;
int g_links_relay_claim = 60; /* seconds a helper has to deliver a block it took; then it is fetched again */
int g_links_relay_gone = 5;   /* seconds without hearing from a helper before the console goes on alone */

enum { K_UNKNOWN, K_PKG, K_RAR, K_ZIP };
enum { S_QUEUED, S_RUNNING, S_FAILED };
static const char *const k_states[] = {"queued", "running", "failed"};

typedef struct {
    char url[URL_MAX];
    char name[256];
    int named; /* the user gave the name (else it comes from the address) */
} link_t;

typedef struct {
    int id, kind, state, started;
    char dest[16], name[256], error[512];
    char password[RAR5_PASSWORD_MAX + 1]; /* the one given, for a RAR with one */
    int locked;                            /* failed for want of the right password */
    int nvol;
    link_t *vol;
} ljob_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static int g_started, g_quit;
static volatile int g_cancel;
static char g_queue_path[PATH_MAX], g_pw_path[PATH_MAX];
static links_host_t g_host;

/* the queue (all under g_mu) */
static ljob_t g_jobs[LINKS_MAX_JOBS];
static int g_njobs, g_next_id = 1;

/* the download in progress */
static int g_busy, g_cur_id;
static char g_cur_name[256], g_cur_file[256], g_cur_pkg[256];
static int g_file_i, g_file_n;
static const char *g_phase = "";
static uint64_t g_done, g_all;
static time_t g_job_start, g_retry_until;
static int g_milestone;
static char g_result[600];
static int g_result_ok = 1;
#define SAMPLES 30
static struct {
    double t;
    uint64_t b;
} g_samples[SAMPLES];
static int g_nsamples;

/* One file of the download in progress, as the downloads and the archive reader share it
 * (under g_rt.mu). */
typedef struct {
    char url[URL_MAX]; /* after redirects */
    char etag[128], fname[256];
    uint64_t size;     /* UINT64_MAX while not known */
    uint64_t have;     /* bytes in order in its part file */
    int complete, ranges;
} vol_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    vol_t *v;
    int nvol, kind;
    const ljob_t *job;
    char work[PATH_MAX];
    volatile int stop; /* a failure, a cancel, PS4Mount stopping, or the archive read to its end */
    int failed;
    char err[600];
    int x_vol;         /* the file the archive reader is on */
    uint64_t consumed; /* bytes it has read */
    int x_ok, npkgs;
    int stream;        /* little space: each file is deleted once read */
    int checked;       /* a pkg link: its start was checked */
    rar5keys_t *keys;  /* a RAR's passwords to try */
    int locked;        /* the archive wants a password none of them is */
} g_rt = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

/* --- small helpers --- */

static void set_result(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_result(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_mu);
    vsnprintf(g_result, sizeof(g_result), fmt, ap);
    g_result_ok = ok;
    pthread_mutex_unlock(&g_mu);
    va_end(ap);
    printf("[links] %s\n", g_result);
}

/* The first failure of the download in progress; everything of it stops. */
static void fail_job(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail_job(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_rt.mu);
    if (!g_rt.failed) {
        g_rt.failed = 1;
        vsnprintf(g_rt.err, sizeof(g_rt.err), fmt, ap);
    }
    g_rt.stop = 1;
    pthread_cond_broadcast(&g_rt.cv);
    pthread_mutex_unlock(&g_rt.mu);
    va_end(ap);
}

static void json_esc(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_put(b, "\\", 1);
        if ((unsigned char)*s >= 0x20)
            buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

static int halted(void) { return g_rt.stop || g_cancel || g_quit; }

/* Sleeps s seconds, a second at a time: 0, or -1 when the download stops. */
static int pause_s(int s) {
    pthread_mutex_lock(&g_mu);
    g_retry_until = time(NULL) + s;
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < s; i++) {
        if (halted())
            return -1;
        sleep(1);
    }
    return halted() ? -1 : 0;
}

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Bytes a second over the last half minute (0 while unknown). */
static double speed_locked(void) {
    if (g_nsamples < 2)
        return 0;
    double dt = g_samples[g_nsamples - 1].t - g_samples[0].t;
    return dt >= 3 ? (double)(g_samples[g_nsamples - 1].b - g_samples[0].b) / dt : 0;
}

/* "~25 min", "~1 h 40 min". */
static void fmt_left(double seconds, char *out, size_t n) {
    long m = (long)(seconds / 60 + 0.5);
    if (m < 1)
        snprintf(out, n, "%s", TR("less than a minute", "menos de un minuto"));
    else if (m < 60)
        snprintf(out, n, "~%ld min", m);
    else
        snprintf(out, n, "~%ld h %ld min", m / 60, m % 60);
}

static void fmt_size(uint64_t b, char *out, size_t n) {
    if (b >= (1ull << 30))
        snprintf(out, n, "%.1f GB", b / 1073741824.0);
    else
        snprintf(out, n, "%llu MB", (unsigned long long)((b + (1u << 20) - 1) >> 20));
}

static void set_phase(const char *p) {
    pthread_mutex_lock(&g_mu);
    g_phase = p;
    g_nsamples = 0; /* each phase has a speed of its own */
    pthread_mutex_unlock(&g_mu);
}

/* done: bytes of the whole download. A quarter more is told on the console's screen. */
static void progress(uint64_t done) {
    char note[400] = "";
    pthread_mutex_lock(&g_mu);
    g_done = done;
    double now = mono();
    if (!g_nsamples || now - g_samples[g_nsamples - 1].t >= 1) {
        if (g_nsamples == SAMPLES) {
            memmove(g_samples, g_samples + 1, sizeof(g_samples[0]) * (SAMPLES - 1));
            g_nsamples--;
        }
        g_samples[g_nsamples].t = now;
        g_samples[g_nsamples].b = done;
        g_nsamples++;
    }
    int quarter = g_all ? (int)(done * 4 / g_all) : 0;
    double sp = speed_locked();
    if (quarter > g_milestone && quarter < 4 && sp > 0 && !strcmp(g_phase, "receiving")) {
        g_milestone = quarter;
        char left[48];
        fmt_left((double)(g_all - done) / sp, left, sizeof(left));
        snprintf(note, sizeof(note), TR("%s: %d%% downloaded, %s left", "%s: %d%% descargado, quedan %s"), g_cur_name,
                 quarter * 25, left);
    }
    pthread_mutex_unlock(&g_mu);
    if (note[0] && g_host.notify)
        g_host.notify(note);
}

/* Bytes downloaded of the whole download (under g_rt.mu). */
static uint64_t have_all_locked(void) {
    uint64_t s = 0;
    for (int i = 0; i < g_rt.nvol; i++)
        s += g_rt.v[i].have;
    return s;
}

/* A name for a file or a folder: no path, no characters exFAT refuses. */
static void clean_name(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f || strchr("/\\:*?\"<>|", c))
            continue;
        out[o++] = (char)c;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.'))
        o--;
    out[o] = 0;
    while (out[0] == ' ' || out[0] == '.')
        memmove(out, out + 1, strlen(out));
}

/* strcasestr (the console's libc has none). */
static const char *find_ci(const char *s, const char *what) {
    size_t l = strlen(what);
    for (; *s; s++)
        if (!strncasecmp(s, what, l))
            return s;
    return NULL;
}

static int ends_with(const char *s, const char *tail) {
    size_t l = strlen(s), t = strlen(tail);
    return l >= t && !strcasecmp(s + l - t, tail);
}

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* %XX decoded, up to the first of stop. */
static void pct_decode(const char *in, const char *stop, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && !strchr(stop, *p) && o + 1 < n; p++) {
        int hi, lo;
        if (*p == '%' && (hi = hexval(p[1])) >= 0 && (lo = hexval(p[2])) >= 0) {
            out[o++] = (char)(hi * 16 + lo);
            p += 2;
        } else
            out[o++] = *p;
    }
    out[o] = 0;
}

/* The file name at the end of an address. */
static void url_name(const char *url, char *out, size_t n) {
    http_url_t u;
    out[0] = 0;
    if (http_parse_url(url, &u))
        return;
    const char *end = u.path + strcspn(u.path, "?#"), *s = end;
    while (s > u.path && s[-1] != '/')
        s--;
    char raw[512];
    snprintf(raw, sizeof(raw), "%.*s", (int)(end - s), s);
    char dec[512];
    pct_decode(raw, "", dec, sizeof(dec));
    clean_name(dec, out, n);
}

/* The file name of a Content-Disposition: filename*=UTF-8''name or filename="name". */
static void disposition_name(const char *cd, char *out, size_t n) {
    char raw[512] = "";
    const char *p = find_ci(cd, "filename*=");
    if (p) {
        p += 10;
        const char *q = strstr(p, "''");
        if (q)
            pct_decode(q + 2, ";", raw, sizeof(raw));
    } else if ((p = find_ci(cd, "filename="))) {
        p += 9;
        if (*p == '"') {
            p++;
            snprintf(raw, sizeof(raw), "%.*s", (int)strcspn(p, "\""), p);
        } else
            snprintf(raw, sizeof(raw), "%.*s", (int)strcspn(p, "; "), p);
    }
    clean_name(raw, out, n);
}

/* loc (a Location: header) against the address it came from. */
static int resolve_location(const char *base, const char *loc, char *out, size_t n) {
    http_url_t u;
    int w;
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8))
        w = snprintf(out, n, "%s", loc);
    else if (http_parse_url(base, &u))
        return -1;
    else if (!strncmp(loc, "//", 2))
        w = snprintf(out, n, "%s:%s", u.tls ? "https" : "http", loc);
    else {
        size_t origin = (size_t)(u.path - base);
        if (loc[0] == '/')
            w = snprintf(out, n, "%.*s%s", (int)origin, base, loc);
        else {
            /* relative to the folder of the address */
            size_t pl = strcspn(u.path, "?#");
            while (pl && u.path[pl - 1] != '/')
                pl--;
            w = snprintf(out, n, "%.*s%s", (int)(origin + pl), base, loc);
        }
    }
    return w < 0 || (size_t)w >= n ? -1 : 0; /* an address too long is not cut */
}

/* GET url, following redirects (url ends as the address that answered). */
static int get(char *url, size_t un, const char *range, http_sink_t sink, void *ctx, http_keep_t *keep, int stall,
               http_resp_t *r, char *err, size_t n) {
    for (int hop = 0;; hop++) {
        char hdr[128] = "";
        if (range)
            snprintf(hdr, sizeof(hdr), "Range: bytes=%s\r\n", range);
        http_req_t q = {0};
        q.method = "GET";
        q.url = url;
        q.headers = hdr;
        q.sink = sink;
        q.sink_ctx = ctx;
        q.cancel = &g_rt.stop;
        q.stall = stall;
        q.keep = keep;
        q.any_server = 1;
        int rc = http_request(&q, r, err, n);
        if (rc || r->status < 300 || r->status >= 400 || !r->location[0])
            return rc;
        char next[URL_MAX];
        int bad = hop >= REDIRECTS || resolve_location(url, r->location, next, sizeof(next));
        http_resp_free(r);
        if (bad) {
            snprintf(err, n, "%s", TR("the link redirects too many times", "el enlace redirige demasiadas veces"));
            return -1;
        }
        snprintf(url, un, "%s", next);
    }
}

/* What a file is, from its first bytes. */
static int magic_kind(const uint8_t *p, size_t n) {
    if (n >= 4 && !memcmp(p, "\x7f" "CNT", 4))
        return K_PKG;
    if (n >= 7 && !memcmp(p, "Rar!\x1a\x07", 6))
        return K_RAR;
    if (n >= 4 && !memcmp(p, "PK\x03\x04", 4))
        return K_ZIP;
    return K_UNKNOWN;
}

/* --- finding out about a file --- */

typedef struct {
    uint8_t head[16];
    size_t got;
    int stopped;
} probe_t;

static int probe_sink(void *ctx, const uint8_t *p, size_t len) {
    probe_t *pr = ctx;
    size_t take = len < sizeof(pr->head) - pr->got ? len : sizeof(pr->head) - pr->got;
    memcpy(pr->head + pr->got, p, take);
    pr->got += take;
    if (take < len)
        pr->stopped = 1; /* more than was asked: a server that sends the whole file */
    return pr->stopped;
}

static void status_error(int status, char *err, size_t n) {
    if (status == 401 || status == 403)
        snprintf(err, n, TR("the server refused the link (%d): it may have expired or need a sign-in",
                            "el servidor rechazó el enlace (%d): puede haber caducado o pedir iniciar sesión"),
                 status);
    else if (status == 404 || status == 410)
        snprintf(err, n, TR("the file is not there any more (%d)", "el archivo ya no está (%d)"), status);
    else
        snprintf(err, n, TR("the server answered %d", "el servidor respondió %d"), status);
}

/* Size, name and kind of file i, and whether it can be fetched in pieces. */
static int probe(int i, const char *link, int *kind, char *err, size_t n) {
    vol_t *v = &g_rt.v[i];
    snprintf(v->url, sizeof(v->url), "%s", link);
    for (int attempt = 0;; attempt++) {
        probe_t pr = {0};
        http_resp_t r;
        int rc = get(v->url, sizeof(v->url), "0-15", probe_sink, &pr, NULL, 30, &r, err, n);
        if (rc && !(pr.stopped && r.status)) {
            if (halted() || attempt + 1 >= TRIES || pause_s(g_links_retry_seconds << attempt))
                return -1;
            snprintf(v->url, sizeof(v->url), "%s", link);
            continue;
        }
        int st = r.status;
        if (st == 206) {
            const char *slash = strchr(r.content_range, '/');
            v->size = slash && slash[1] != '*' ? strtoull(slash + 1, NULL, 10) : UINT64_MAX;
            v->ranges = v->size != UINT64_MAX;
        } else if (st == 200) {
            v->size = r.length;
            v->ranges = 0;
        } else if (st == 416) {
            v->size = 0; /* an empty file: there is no byte 0 */
            v->ranges = 0;
        } else {
            http_resp_free(&r);
            if ((st == 429 || st >= 500) && attempt + 1 < TRIES && !pause_s(g_links_retry_seconds << attempt)) {
                snprintf(v->url, sizeof(v->url), "%s", link);
                continue;
            }
            status_error(st, err, n);
            return -1;
        }
        snprintf(v->etag, sizeof(v->etag), "%s", r.etag);
        if (r.disposition[0])
            disposition_name(r.disposition, v->fname, sizeof(v->fname));
        int html = !strncasecmp(r.type, "text/html", 9);
        http_resp_free(&r);
        *kind = magic_kind(pr.head, pr.got);
        if (*kind == K_UNKNOWN && (html || (pr.got && pr.head[0] == '<'))) {
            snprintf(err, n, "%s", TR("the link opens a web page, not the file: copy the file's direct link (the one that starts the download)",
                                      "el enlace abre una página web, no el archivo: copia el enlace directo del archivo (el que empieza la descarga)"));
            return -1;
        }
        return 0;
    }
}

/* --- downloading a file --- */

typedef struct {
    int vi;
    vol_t *v;
    const char *link, *part;
    int fd;
    uint64_t nblocks, next, written; /* next block to fetch; blocks written, in order */
    uint8_t *buf[SLOTS];
    int ready[SLOTS];
    size_t len[SLOTS];
    uint64_t inflight; /* bytes in memory not written yet */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int fails, stop, slots, active, allowed, resolves;
    char err[512];
    /* blocks a helper took (by slot), and blocks to fetch again (a helper's that never came) */
    int claimed[SLOTS], receiving[SLOTS];
    uint64_t cblock[SLOTS];
    time_t until[SLOTS];
    uint64_t redo[SLOTS];
    int nredo;
} dl_t;

/* --- a helper on the network fetching blocks for the console --- */

/* The browser extension can fetch the blocks of a download at the computer's speed (the
 * console's own HTTPS tops out near 10-15 MB/s, its network takes twice that) and hand
 * them over in plain HTTP on port g_links_relay_port: POST /claim gives it a block to
 * fetch (the address, the range), /put delivers it, /drop gives it back. The console's own
 * connections go on alongside (a browser has one connection to a server, which many hosts
 * slow down: together they go faster); a block taken and not delivered in
 * g_links_relay_claim seconds, or given back, is fetched by the console again. */
static pthread_mutex_t g_relay_mu = PTHREAD_MUTEX_INITIALIZER;
static dl_t *g_relay_dl; /* the file whose blocks can be taken */
static int g_relay_users, g_relay_conns;
static time_t g_relay_seen; /* atomic: read without g_relay_mu */
static int g_relay_fd = -1;
static volatile int g_relay_quit;
static pthread_t g_relay_thread;

static int relay_active(void) {
    time_t seen = __atomic_load_n(&g_relay_seen, __ATOMIC_RELAXED);
    return seen && time(NULL) - seen <= g_links_relay_gone;
}

/* Blocks a helper took and did not deliver in time: fetched again (with d->mu held). */
static void relay_expire_locked(dl_t *d) {
    time_t now = time(NULL);
    for (int s = 0; s < d->slots; s++)
        if (d->claimed[s] && !d->receiving[s] && now > d->until[s] && d->nredo < SLOTS) {
            d->claimed[s] = 0;
            d->redo[d->nredo++] = d->cblock[s];
            printf("[links] %s block %llu: the helper did not bring it, fetched here\n", d->part, (unsigned long long)d->cblock[s]);
        }
}

static int relay_claims_locked(const dl_t *d) {
    for (int s = 0; s < d->slots; s++)
        if (d->claimed[s])
            return 1;
    return 0;
}

/* The next block to fetch (with d->mu held): one to fetch again (always the console's: a
 * helper gave it back), or the next in the window. */
static int take_block_locked(dl_t *d, int helper, uint64_t *b) {
    if (!helper && d->nredo) {
        *b = d->redo[--d->nredo];
        return 1;
    }
    if (d->next >= d->nblocks || d->next >= d->written + (uint64_t)d->slots)
        return 0;
    *b = d->next++;
    return 1;
}

typedef struct {
    dl_t *d;
    uint8_t *buf;
    size_t at, want;
} piece_t;

static uint64_t dl_have_locked(const dl_t *d) {
    uint64_t w = d->written * g_links_block;
    return w < d->v->size ? w : d->v->size;
}

/* The bytes of file vi now in order on disk: the archive reader may go on; a pkg link's
 * start is checked once it is there. */
static void have_now(int vi, uint64_t have) {
    pthread_mutex_lock(&g_rt.mu);
    vol_t *v = &g_rt.v[vi];
    v->have = have;
    uint64_t all = have_all_locked();
    int check = g_rt.kind == K_PKG && !g_rt.checked && (have >= CHECK_BYTES || (v->size != UINT64_MAX && have >= v->size));
    if (check)
        g_rt.checked = 1;
    pthread_cond_broadcast(&g_rt.cv);
    pthread_mutex_unlock(&g_rt.mu);
    if (check && g_host.check) {
        char part[PATH_MAX], problem[400];
        snprintf(part, sizeof(part), "%.4000s/v%03d" PART_EXT, g_rt.work, vi);
        if (g_host.check(part, v->size != UINT64_MAX ? v->size : 0, problem, sizeof(problem)))
            fail_job("%s: %s", g_rt.job->vol[vi].name, problem);
    }
    progress(all);
}

static int piece_sink(void *ctx, const uint8_t *p, size_t len) {
    piece_t *pc = ctx;
    if (len > pc->want - pc->at)
        return -1; /* more than the range asked: a server that ignored it */
    memcpy(pc->buf + pc->at, p, len);
    pc->at += len;
    dl_t *d = pc->d;
    pthread_mutex_lock(&d->mu);
    d->inflight += len;
    uint64_t h = dl_have_locked(d) + d->inflight;
    pthread_mutex_unlock(&d->mu);
    pthread_mutex_lock(&g_rt.mu);
    uint64_t all = have_all_locked() - d->v->have + h;
    pthread_mutex_unlock(&g_rt.mu);
    progress(all);
    return halted() || d->stop;
}

/* Writes the blocks that are next in order (with d->mu held). */
static void dl_flush_locked(dl_t *d) {
    int wrote = 0;
    for (;;) {
        int s = (int)(d->written % (uint64_t)d->slots);
        if (d->written >= d->nblocks || !d->ready[s] || d->stop)
            break;
        size_t len = d->len[s], o = 0;
        off_t at = (off_t)(d->written * g_links_block);
        while (o < len) {
            ssize_t w = pwrite(d->fd, d->buf[s] + o, len - o, at + (off_t)o);
            if (w <= 0) {
                d->stop = 1;
                snprintf(d->err, sizeof(d->err), TR("write failed (%s): is the drive full?", "falló la escritura (%s): ¿está llena la unidad?"),
                         strerror(errno));
                pthread_cond_broadcast(&d->cv);
                return;
            }
            o += (size_t)w;
        }
        d->ready[s] = 0;
        d->inflight -= len;
        d->written++;
        wrote = 1;
        pthread_cond_broadcast(&d->cv);
    }
    if (wrote)
        have_now(d->vi, dl_have_locked(d));
}

/* The link of file vi again, for a fresh address (a host's signed one may expire): 0 when
 * there is a new one to try. */
static int dl_resolve_locked(dl_t *d) {
    if (d->resolves >= 20)
        return -1;
    d->resolves++;
    char url[URL_MAX], err[256];
    snprintf(url, sizeof(url), "%s", d->link);
    probe_t pr = {0};
    http_resp_t r;
    int rc = get(url, sizeof(url), "0-15", probe_sink, &pr, NULL, 30, &r, err, sizeof(err));
    int fresh = (!rc || (pr.stopped && r.status)) && (r.status == 206 || r.status == 200) && strcmp(url, d->v->url);
    if (fresh) {
        printf("[links] %s: the link gave a new address\n", d->part);
        snprintf(d->v->url, sizeof(d->v->url), "%s", url);
    }
    if (!rc)
        http_resp_free(&r);
    return fresh ? 0 : -1;
}

static void *dl_worker(void *arg) {
    dl_t *d = arg;
    /* one connection for all the blocks of this thread: one TLS handshake, not one a block */
    http_keep_t *keep = http_keep_new();
    for (;;) {
        pthread_mutex_lock(&d->mu);
        uint64_t b = 0;
        int have = 0;
        for (;;) {
            relay_expire_locked(d);
            /* a server that limits connections: this one ends (its block was done) */
            if (d->stop || halted() || d->active > d->allowed || (have = take_block_locked(d, 0, &b)))
                break;
            if (d->next >= d->nblocks && !d->nredo && !relay_claims_locked(d))
                break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&d->cv, &d->mu, &ts);
        }
        if (!have) {
            d->active--;
            pthread_cond_broadcast(&d->cv);
            pthread_mutex_unlock(&d->mu);
            http_keep_free(keep);
            return NULL;
        }
        int s = (int)(b % (uint64_t)d->slots);
        pthread_mutex_unlock(&d->mu);
        uint64_t from = b * g_links_block, to = from + g_links_block < d->v->size ? from + g_links_block : d->v->size;
        piece_t pc = {d, d->buf[s], 0, (size_t)(to - from)};
        int ok = 0;
        while (!ok && !d->stop && !halted()) {
            char url[URL_MAX], range[64], err[512] = "";
            pthread_mutex_lock(&d->mu);
            snprintf(url, sizeof(url), "%s", d->v->url);
            pthread_mutex_unlock(&d->mu);
            snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)(from + pc.at), (unsigned long long)(to - 1));
            size_t before = pc.at;
            http_resp_t r;
            int q = get(url, sizeof(url), range, piece_sink, &pc, keep, STALL, &r, err, sizeof(err));
            int st = q ? 0 : r.status, throttled = 0, gone = 0;
            if (!q)
                http_resp_free(&r);
            if (q && pc.at < pc.want && !halted() && !d->stop)
                printf("[links] %s block %llu: %s at %zu of %zu bytes, opening it again\n", d->part, (unsigned long long)b, err,
                       pc.at, pc.want);
            if (st && st != 206 && !(st == 200 && from == 0 && to == d->v->size)) {
                if (st == 200)
                    snprintf(err, sizeof(err), "%s", TR("the server sent the whole file instead of a part", "el servidor envió el archivo entero en vez de una parte"));
                else
                    status_error(st, err, sizeof(err));
                throttled = st == 429 || st == 503;
                gone = st == 401 || st == 403 || st == 404 || st == 410;
                pc.at = before = 0; /* nothing of it was taken */
            }
            ok = pc.at == pc.want;
            if (ok || halted())
                break;
            pthread_mutex_lock(&d->mu);
            if (throttled && d->allowed > 1) {
                d->allowed = d->active - 1 > 1 ? d->active - 1 : 1;
                printf("[links] the server limits connections: %d now\n", d->allowed);
            }
            /* refused: the link again, for a new address (once for all the connections: one
             * that sees another already got one just tries it) */
            int dead = gone && !strcmp(url, d->v->url) && dl_resolve_locked(d);
            /* a piece that moved on is not a failure: the connection dropped, it goes on */
            int fails = pc.at > before || throttled || (gone && !dead) ? d->fails : ++d->fails;
            if (fails >= TRIES * 2 || dead) {
                d->stop = 1;
                snprintf(d->err, sizeof(d->err), "%s", err);
                pthread_cond_broadcast(&d->cv);
            }
            pthread_mutex_unlock(&d->mu);
            if (!d->stop && pc.at == before && !(gone && !dead) && pause_s(g_links_retry_seconds << (fails < 5 ? fails : 5)))
                break;
        }
        pthread_mutex_lock(&d->mu);
        if (ok) {
            d->len[s] = pc.want;
            d->ready[s] = 1;
            dl_flush_locked(d);
        } else {
            d->inflight -= pc.at;
            d->stop = 1; /* in order: a block that cannot come stops the file (it resumes later) */
            pthread_cond_broadcast(&d->cv);
        }
        pthread_mutex_unlock(&d->mu);
    }
}

/* --- the helper's side (see g_relay_dl) --- */

static void relay_reply(int fd, const char *status, const char *json) {
    char h[256];
    int hl = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      status, strlen(json));
    if (write(fd, h, (size_t)hl) == hl && write(fd, json, strlen(json))) {
    }
}

/* The query's value of key (a number), or -1. */
static long long query_num(const char *path, const char *key) {
    char k[16];
    snprintf(k, sizeof(k), "%s=", key);
    const char *p = strstr(path, k);
    return p ? strtoll(p + strlen(k), NULL, 10) : -1;
}

/* The file being downloaded, held for a request of the helper (NULL: none now). */
static dl_t *relay_hold(void) {
    pthread_mutex_lock(&g_relay_mu);
    dl_t *d = g_relay_dl;
    if (d)
        g_relay_users++;
    __atomic_store_n(&g_relay_seen, time(NULL), __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_relay_mu);
    return d;
}

static void relay_release(void) {
    pthread_mutex_lock(&g_relay_mu);
    g_relay_users--;
    pthread_mutex_unlock(&g_relay_mu);
}

/* POST /claim: a block to fetch, {"job","vol","block","from","to","url"} (to: past its
   last byte); {"wait":ms} when none is free yet, {"idle":true} with no download running. */
static void relay_claim(int fd) {
    dl_t *d = relay_hold();
    if (!d) {
        relay_reply(fd, "200 OK", "{\"idle\":true}");
        return;
    }
    buf_t out = {0};
    pthread_mutex_lock(&d->mu);
    relay_expire_locked(d);
    uint64_t b;
    if (d->stop || halted())
        buf_printf(&out, "{\"idle\":true}");
    else if (!take_block_locked(d, 1, &b))
        buf_printf(&out, "{\"wait\":%d}", d->next >= d->nblocks ? 2000 : 300);
    else {
        int s = (int)(b % (uint64_t)d->slots);
        d->claimed[s] = 1;
        d->receiving[s] = 0;
        d->cblock[s] = b;
        d->until[s] = time(NULL) + g_links_relay_claim;
        uint64_t from = b * g_links_block, to = from + g_links_block < d->v->size ? from + g_links_block : d->v->size;
        buf_printf(&out, "{\"job\":%d,\"vol\":%d,\"block\":%llu,\"from\":%llu,\"to\":%llu,\"url\":", g_cur_id, d->vi,
                   (unsigned long long)b, (unsigned long long)from, (unsigned long long)to);
        json_esc(&out, d->v->url);
        buf_printf(&out, "}");
    }
    pthread_mutex_unlock(&d->mu);
    relay_release();
    relay_reply(fd, "200 OK", out.data ? (const char *)out.data : "{}");
    buf_free(&out);
}

/* POST /put?job=&vol=&block= (the block, whole) or /drop?... (it could not be fetched). pre:
   the body's bytes that came with the headers. */
static void relay_deliver(int fd, const char *path, int put, const uint8_t *pre, size_t npre, unsigned long long length) {
    long long job = query_num(path, "job"), vol = query_num(path, "vol"), blk = query_num(path, "block");
    dl_t *d = relay_hold();
    if (!d) {
        relay_reply(fd, "409 Conflict", "{\"ok\":false}");
        return;
    }
    pthread_mutex_lock(&d->mu);
    int s = blk >= 0 ? (int)((uint64_t)blk % (uint64_t)d->slots) : 0;
    int mine = job == g_cur_id && vol == d->vi && blk >= 0 && d->claimed[s] && !d->receiving[s] && d->cblock[s] == (uint64_t)blk;
    uint64_t from = (uint64_t)blk * g_links_block, to = from + g_links_block < d->v->size ? from + g_links_block : d->v->size;
    size_t want = mine ? (size_t)(to - from) : 0;
    if (mine && put && length == want)
        d->receiving[s] = 1;
    else if (mine) {
        d->claimed[s] = 0; /* given back, or not whole: fetched again */
        if (d->nredo < SLOTS)
            d->redo[d->nredo++] = (uint64_t)blk;
        pthread_cond_broadcast(&d->cv);
        mine = 0;
    }
    pthread_mutex_unlock(&d->mu);
    int ok = 0;
    if (mine) {
        /* straight into its slot: no copy of 8 MB more */
        uint8_t *dst = d->buf[s];
        size_t have = npre < want ? npre : want;
        memcpy(dst, pre, have);
        while (have < want && !halted()) {
            ssize_t m = read(fd, dst + have, want - have);
            if (m <= 0)
                break;
            have += (size_t)m;
        }
        ok = have == want;
        pthread_mutex_lock(&d->mu);
        d->receiving[s] = 0;
        d->claimed[s] = 0;
        if (ok && !d->stop) {
            d->inflight += want;
            d->len[s] = want;
            d->ready[s] = 1;
            dl_flush_locked(d);
        } else if (d->nredo < SLOTS)
            d->redo[d->nredo++] = (uint64_t)blk;
        pthread_cond_broadcast(&d->cv);
        pthread_mutex_unlock(&d->mu);
    }
    relay_release();
    relay_reply(fd, ok ? "200 OK" : put ? "409 Conflict" : "200 OK", ok || !put ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void *relay_conn(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[4096];
    size_t n = 0;
    char *end = NULL;
    while (n < sizeof(req) - 1 && !end) {
        ssize_t m = read(fd, req + n, sizeof(req) - 1 - n);
        if (m <= 0)
            break;
        n += (size_t)m;
        req[n] = 0;
        end = strstr(req, "\r\n\r\n");
    }
    if (end) {
        char *body = end + 4;
        const char *cl = find_ci(req, "content-length:");
        unsigned long long length = cl ? strtoull(cl + 15, NULL, 10) : 0;
        /* the panel's header: a page on another site cannot send it */
        int ours = find_ci(req, "\r\nx-ps4mount:") != NULL;
        char path[512];
        if (!ours || sscanf(req, "POST %511s", path) != 1)
            relay_reply(fd, "403 Forbidden", "{\"ok\":false}");
        else if (!strcmp(path, "/claim"))
            relay_claim(fd);
        else if (!strncmp(path, "/put?", 5) || !strncmp(path, "/drop?", 6))
            relay_deliver(fd, path, path[1] == 'p', (const uint8_t *)body, (size_t)(req + n - body), length);
        else
            relay_reply(fd, "404 Not Found", "{\"ok\":false}");
    }
    close(fd);
    pthread_mutex_lock(&g_relay_mu);
    g_relay_conns--;
    pthread_mutex_unlock(&g_relay_mu);
    return NULL;
}

/* Until links_stop (a payload's threads outlive its main: it must end by itself). */
static void *relay_thread(void *arg) {
    (void)arg;
    while (!g_relay_quit) {
        int fd = accept(g_relay_fd, NULL, NULL);
        if (fd < 0) {
            if (!g_relay_quit)
                usleep(100000);
            continue;
        }
        pthread_mutex_lock(&g_relay_mu);
        int busy = g_relay_conns >= RELAY_CONNS;
        if (!busy)
            g_relay_conns++;
        pthread_mutex_unlock(&g_relay_mu);
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 64 << 10);
        if (busy || pthread_create(&t, &at, relay_conn, (void *)(intptr_t)fd)) {
            if (!busy) {
                pthread_mutex_lock(&g_relay_mu);
                g_relay_conns--;
                pthread_mutex_unlock(&g_relay_mu);
            }
            relay_reply(fd, "503 Service Unavailable", "{\"wait\":1000}");
            close(fd);
        }
        pthread_attr_destroy(&at);
    }
    return NULL;
}

/* Listens for a helper on g_links_relay_port or one of the next ones (taken by another
   program, or by a PS4Mount whose threads outlived it); 0: any. links_relay_port() and
   GET /api/links tell which. */
static void relay_start(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    int one = 1, bound = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    for (int i = 0; i < 10 && !bound; i++) {
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)(g_links_relay_port ? g_links_relay_port + i : 0));
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        bound = !bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    }
    if (!bound || listen(fd, 16)) {
        printf("[links] no helper port from %d (%s)\n", g_links_relay_port, strerror(errno));
        close(fd);
        return;
    }
    g_relay_fd = fd;
    g_relay_quit = 0;
    if (pthread_create(&g_relay_thread, NULL, relay_thread, NULL)) {
        close(fd);
        g_relay_fd = -1;
        return;
    }
    printf("[links] helpers on port %d\n", links_relay_port());
}

static void relay_stop(void) {
    if (g_relay_fd < 0)
        return;
    g_relay_quit = 1;
    shutdown(g_relay_fd, SHUT_RDWR);
    pthread_join(g_relay_thread, NULL);
    close(g_relay_fd);
    g_relay_fd = -1;
    /* connections still served end on their own (their download is gone, or 10 s without a
       byte): none may outlive PS4Mount */
    for (int i = 0; i < 240; i++) {
        pthread_mutex_lock(&g_relay_mu);
        int left = g_relay_conns;
        pthread_mutex_unlock(&g_relay_mu);
        if (!left)
            break;
        usleep(50000);
    }
}

int links_relay_port(void) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    if (g_relay_fd < 0 || getsockname(g_relay_fd, (struct sockaddr *)&sa, &sl))
        return 0;
    return ntohs(sa.sin_port);
}

int links_relay_active(void) { return relay_active(); }

/* A server that does not send pieces: the file in one request, again from the start when
 * it is cut. */
typedef struct {
    int vi, fd;
    uint64_t at;
    int failed;
} whole_t;

static int whole_sink(void *ctx, const uint8_t *p, size_t len) {
    whole_t *w = ctx;
    size_t o = 0;
    while (o < len) {
        ssize_t k = pwrite(w->fd, p + o, len - o, (off_t)(w->at + o));
        if (k <= 0) {
            w->failed = errno ? errno : EIO;
            return -1;
        }
        o += (size_t)k;
    }
    w->at += len;
    have_now(w->vi, w->at);
    return halted();
}

static int download_whole(int vi, int fd, char *err, size_t n) {
    vol_t *v = &g_rt.v[vi];
    for (int attempt = 0; attempt < TRIES && !halted(); attempt++) {
        if (attempt && pause_s(g_links_retry_seconds << attempt))
            break;
        if (ftruncate(fd, 0)) {
        }
        have_now(vi, 0);
        whole_t w = {vi, fd, 0, 0};
        char url[URL_MAX];
        snprintf(url, sizeof(url), "%s", v->url);
        http_resp_t r;
        int q = get(url, sizeof(url), NULL, whole_sink, &w, NULL, 60, &r, err, n);
        if (w.failed) {
            snprintf(err, n, TR("write failed (%s): is the drive full?", "falló la escritura (%s): ¿está llena la unidad?"), strerror(w.failed));
            return -1;
        }
        if (q)
            continue;
        int st = r.status;
        http_resp_free(&r);
        if (st != 200) {
            status_error(st, err, n);
            if (st != 429 && st < 500)
                return -1;
            continue;
        }
        if (v->size != UINT64_MAX && w.at != v->size) {
            snprintf(err, n, TR("the file arrived with %llu of %llu bytes", "el archivo llegó con %llu de %llu bytes"),
                     (unsigned long long)w.at, (unsigned long long)v->size);
            continue;
        }
        pthread_mutex_lock(&g_rt.mu);
        v->size = w.at;
        pthread_mutex_unlock(&g_rt.mu);
        return 0;
    }
    if (halted())
        snprintf(err, n, "%s", TR("cancelled", "cancelado"));
    return -1;
}

/* What is known of a part file left by an earlier run: whose file it was. */
static void info_path(int vi, char *out, size_t n) { snprintf(out, n, "%.4000s/v%03d.info", g_rt.work, vi); }

static int same_file_as_before(int vi) {
    char p[PATH_MAX], line[256];
    info_path(vi, p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f)
        return 0;
    int same = 0;
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char *tab = strchr(line, '\t');
        const vol_t *v = &g_rt.v[vi];
        same = tab && strtoull(line, NULL, 10) == v->size && !strcmp(tab + 1, v->etag);
    }
    fclose(f);
    return same;
}

static void save_info(int vi) {
    char p[PATH_MAX];
    info_path(vi, p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "%llu\t%s\n", (unsigned long long)g_rt.v[vi].size, g_rt.v[vi].etag);
        fclose(f);
    }
}

static int download_file(int vi, const char *part, char *err, size_t n) {
    vol_t *v = &g_rt.v[vi];
    int fd = open(part, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        snprintf(err, n, TR("cannot write %s (%s)", "no se puede escribir %s (%s)"), part, strerror(errno));
        return -1;
    }
    if (!v->ranges) {
        int rc = download_whole(vi, fd, err, n);
        if (close(fd) && !rc) {
            snprintf(err, n, TR("cannot write %s (%s)", "no se puede escribir %s (%s)"), part, strerror(errno));
            rc = -1;
        }
        return rc;
    }
    dl_t *d = mem_alloc(sizeof(*d));
    if (!d) {
        close(fd);
        snprintf(err, n, "%s", TR("no memory for the download", "no hay memoria para la descarga"));
        return -1;
    }
    memset(d, 0, sizeof(*d));
    d->vi = vi;
    d->v = v;
    d->link = g_rt.job->vol[vi].url;
    d->part = part;
    d->fd = fd;
    d->nblocks = v->size ? (v->size + g_links_block - 1) / g_links_block : 0;
    for (d->slots = 0; d->slots < SLOTS && d->slots < (int)d->nblocks + 1; d->slots++)
        if (!(d->buf[d->slots] = mem_alloc(g_links_block)))
            break;
    int rc = 0;
    if (d->nblocks && d->slots < 2) {
        snprintf(err, n, "%s", TR("no memory for the download", "no hay memoria para la descarga"));
        rc = -1;
        goto out;
    }
    pthread_mutex_init(&d->mu, NULL);
    pthread_cond_init(&d->cv, NULL);
    /* a cut download: its whole blocks stay, when the file on the server is the same */
    struct stat st;
    uint64_t keep = same_file_as_before(vi) && !fstat(fd, &st) ? (uint64_t)st.st_size / g_links_block : 0;
    if (keep > d->nblocks || ftruncate(fd, (off_t)(keep * g_links_block)))
        keep = 0;
    save_info(vi);
    d->written = d->next = keep;
    have_now(vi, dl_have_locked(d));
    pthread_mutex_lock(&g_relay_mu);
    g_relay_dl = d;
    pthread_mutex_unlock(&g_relay_mu);
    pthread_t th[MAX_THREADS];
    int nth = 0, want = d->slots - 1 < g_links_threads ? d->slots - 1 : g_links_threads;
    if (want > MAX_THREADS)
        want = MAX_THREADS;
    if (want < 1)
        want = 1;
    d->allowed = want;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 << 10);
    pthread_mutex_lock(&d->mu);
    for (int i = 0; i < want && (uint64_t)i < d->nblocks - d->written; i++)
        if (!pthread_create(&th[nth], &at, dl_worker, d))
            nth++;
    d->active = nth;
    pthread_mutex_unlock(&d->mu);
    pthread_attr_destroy(&at);
    if (!nth && d->written < d->nblocks) {
        d->active = 1;
        dl_worker(d);
    }
    for (int i = 0; i < nth; i++)
        pthread_join(th[i], NULL);
    /* no helper may use it any more: wait for the ones still in it */
    pthread_mutex_lock(&g_relay_mu);
    g_relay_dl = NULL;
    while (g_relay_users) {
        pthread_mutex_unlock(&g_relay_mu);
        usleep(20000);
        pthread_mutex_lock(&g_relay_mu);
    }
    pthread_mutex_unlock(&g_relay_mu);
    if (d->written < d->nblocks) {
        rc = -1;
        snprintf(err, n, "%s", halted() ? TR("cancelled", "cancelado") : d->err[0] ? d->err : TR("the download stopped", "la descarga se detuvo"));
    }
    pthread_cond_destroy(&d->cv);
    pthread_mutex_destroy(&d->mu);
out:
    for (int i = 0; i < d->slots; i++)
        mem_free(d->buf[i]);
    mem_free(d);
    if (close(fd) && !rc) {
        snprintf(err, n, TR("cannot write %s (%s)", "no se puede escribir %s (%s)"), part, strerror(errno));
        rc = -1;
    }
    return rc;
}

/* --- reading an archive while it arrives --- */

typedef struct {
    int i, fd;
    uint64_t pos;
    uint8_t *buf;
    rar5dec_t *dec; /* a RAR's volume goes through it (plain when it had a password) */
} xvol_t;

static void part_path(int vi, char *out, size_t n) { snprintf(out, n, "%.4000s/v%03d" PART_EXT, g_rt.work, vi); }

/* The next bytes of a volume as they are in its part file (in x->buf): 0 at its end. */
static ssize_t raw_read(struct archive *a, xvol_t *x) {
    pthread_mutex_lock(&g_rt.mu);
    const vol_t *v = &g_rt.v[x->i];
    while (!g_rt.stop && !g_cancel && !g_quit && v->have <= x->pos && !v->complete) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&g_rt.cv, &g_rt.mu, &ts);
    }
    int stop = g_rt.stop || g_cancel || g_quit;
    uint64_t avail = v->have > x->pos ? v->have - x->pos : 0;
    pthread_mutex_unlock(&g_rt.mu);
    if (stop) {
        archive_set_error(a, ECANCELED, "stopped");
        return -1;
    }
    if (!avail)
        return 0; /* the end of this file */
    if (x->fd < 0) {
        char p[PATH_MAX];
        part_path(x->i, p, sizeof(p));
        if ((x->fd = open(p, O_RDONLY)) < 0) {
            archive_set_error(a, errno, "cannot open the part file");
            return -1;
        }
    }
    if (!x->buf && !(x->buf = mem_alloc(XBUF))) {
        archive_set_error(a, ENOMEM, "no memory");
        return -1;
    }
    ssize_t r = pread(x->fd, x->buf, avail < XBUF ? (size_t)avail : XBUF, (off_t)x->pos);
    if (r <= 0) {
        archive_set_error(a, errno ? errno : EIO, "cannot read the part file");
        return -1;
    }
    x->pos += (uint64_t)r;
    pthread_mutex_lock(&g_rt.mu);
    g_rt.consumed += (uint64_t)r;
    uint64_t consumed = g_rt.consumed;
    pthread_mutex_unlock(&g_rt.mu);
    pthread_mutex_lock(&g_mu);
    int extracting = !strcmp(g_phase, "extracting");
    pthread_mutex_unlock(&g_mu);
    if (extracting)
        progress(consumed);
    return r;
}

static ssize_t x_read(struct archive *a, void *cd, const void **out) {
    xvol_t *x = cd;
    for (;;) {
        ssize_t r = raw_read(a, x);
        if (r <= 0 || !x->dec) {
            *out = x->buf;
            if (!r && x->dec && rar5dec_end(x->dec)) {
                archive_set_error(a, EINVAL, "%s", rar5dec_error(x->dec));
                return -1;
            }
            return r;
        }
        const uint8_t *o;
        ssize_t m = rar5dec_feed(x->dec, x->buf, (size_t)r, &o);
        if (m < 0) {
            pthread_mutex_lock(&g_rt.mu);
            g_rt.locked |= rar5keys_refused(g_rt.keys);
            pthread_mutex_unlock(&g_rt.mu);
            archive_set_error(a, EINVAL, "%s", rar5dec_error(x->dec));
            return -1;
        }
        if (m) {
            *out = o;
            return m;
        }
    }
}

static int x_close(struct archive *a, void *cd) {
    (void)a;
    xvol_t *x = cd;
    if (x && x->fd >= 0) {
        close(x->fd);
        x->fd = -1;
    }
    if (x) {
        mem_free(x->buf);
        x->buf = NULL;
    }
    return ARCHIVE_OK;
}

/* The reader goes on to the next volume: with little space, the one it left is deleted. */
static int x_switch(struct archive *a, void *from, void *to) {
    xvol_t *p = from, *q = to;
    if (p) {
        x_close(a, p);
        if (g_rt.stream) {
            char path[PATH_MAX];
            part_path(p->i, path, sizeof(path));
            unlink(path);
            info_path(p->i, path, sizeof(path));
            unlink(path);
        }
    }
    if (q) {
        pthread_mutex_lock(&g_rt.mu);
        g_rt.x_vol = q->i;
        pthread_cond_broadcast(&g_rt.cv);
        pthread_mutex_unlock(&g_rt.mu);
    }
    return ARCHIVE_OK;
}

static void free_path(const char *dir, const char *name, char *out, size_t n) {
    struct stat st;
    snprintf(out, n, "%.3800s/%.250s", dir, name);
    size_t l = strlen(name), e = ends_with(name, ".pkg") ? 4 : 0;
    for (int i = 1; !stat(out, &st) && i < 1000; i++)
        snprintf(out, n, "%.3800s/%.*s (%d)%s", dir, (int)(l - e), name, i, name + l - e);
}

static void fail_locked(void) {
    pthread_mutex_lock(&g_rt.mu);
    g_rt.locked = 1;
    int given = g_rt.job && g_rt.job->password[0];
    pthread_mutex_unlock(&g_rt.mu);
    if (given)
        fail_job("%s", TR("the password does not open the archive: press Try again and write the right one",
                          "la contraseña no abre el archivo comprimido: pulsa Reintentar y escribe la correcta"));
    else
        fail_job("%s", TR("the archive has a password: press Try again and write it",
                          "el archivo comprimido tiene contraseña: pulsa Reintentar y escríbela"));
}

static int archive_failure(struct archive *a) {
    const char *e = archive_error_string(a);
    if (g_rt.locked || (e && find_ci(e, "passphrase") && !find_ci(e, "unsupported")))
        fail_locked();
    else if (archive_read_has_encrypted_entries(a) > 0 || (e && find_ci(e, "encrypt")))
        fail_job("%s", TR("the archive has a password of a kind PS4Mount cannot open (an older RAR, or a ZIP with AES)",
                          "el archivo comprimido tiene una contraseña de un tipo que PS4Mount no puede abrir (un RAR antiguo o un ZIP con AES)"));
    else if (!halted())
        fail_job(TR("the archive could not be read: %s", "no se pudo leer el archivo comprimido: %s"), e ? e : "?");
    return -1;
}

/* One pkg of the archive into the out folder; its start is checked once written. */
static int extract_pkg(struct archive *a, struct archive_entry *e, const char *outdir, const char *name) {
    char path[PATH_MAX];
    free_path(outdir, name, path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        fail_job(TR("cannot write %s (%s)", "no se puede escribir %s (%s)"), path, strerror(errno));
        return -1;
    }
    const char *slash = strrchr(path, '/');
    pthread_mutex_lock(&g_mu);
    snprintf(g_cur_pkg, sizeof(g_cur_pkg), "%.255s", slash ? slash + 1 : path);
    pthread_mutex_unlock(&g_mu);
    uint64_t size = archive_entry_size_is_set(e) ? (uint64_t)archive_entry_size(e) : 0, end = 0;
    printf("[links] extracting %s (%llu bytes)\n", path, (unsigned long long)size);
    int checked = 0, rc = 0;
    for (;;) {
        const void *p;
        size_t len;
        la_int64_t off;
        int r = archive_read_data_block(a, &p, &len, &off);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_WARN) {
            rc = archive_failure(a);
            break;
        }
        for (size_t o = 0; o < len;) {
            ssize_t w = pwrite(fd, (const uint8_t *)p + o, len - o, (off_t)off + (off_t)o);
            if (w <= 0) {
                fail_job(TR("write failed (%s): is the drive full?", "falló la escritura (%s): ¿está llena la unidad?"), strerror(errno));
                rc = -1;
                break;
            }
            o += (size_t)w;
        }
        if (rc)
            break;
        if ((uint64_t)off + len > end)
            end = (uint64_t)off + len;
        if (!checked && end >= CHECK_BYTES) {
            checked = 1;
            char problem[400];
            if (g_host.check && g_host.check(path, size, problem, sizeof(problem))) {
                fail_job("%s: %s", name, problem);
                rc = -1;
                break;
            }
        }
    }
    if (close(fd) && !rc) {
        fail_job(TR("cannot write %s (%s)", "no se puede escribir %s (%s)"), path, strerror(errno));
        rc = -1;
    }
    if (!rc && !checked) {
        char problem[400];
        if (g_host.check && g_host.check(path, end, problem, sizeof(problem))) {
            fail_job("%s: %s", name, problem);
            rc = -1;
        }
    }
    if (rc)
        unlink(path);
    return rc;
}

/* The passwords to try on an archive of job j: the one given, the site in brackets at the
   start of its name ([SITE.COM]-game: many put it as the password, as "[SITE.COM]",
   "SITE.COM" or "site.com"), and the ones that opened other downloads (g_pw_path, one a
   line). */
static int passwords(const ljob_t *j, char out[][RAR5_PASSWORD_MAX + 1], int max) {
    int n = 0;
    if (j->password[0] && n < max)
        snprintf(out[n++], RAR5_PASSWORD_MAX + 1, "%s", j->password);
    const char *close = j->name[0] == '[' ? strchr(j->name, ']') : NULL;
    if (close && close - j->name > 1 && close - j->name < RAR5_PASSWORD_MAX && n + 3 <= max) {
        int l = (int)(close - j->name - 1);
        snprintf(out[n++], RAR5_PASSWORD_MAX + 1, "%.*s", l + 2, j->name);
        for (int k = 0; k < 2; k++, n++) {
            snprintf(out[n], RAR5_PASSWORD_MAX + 1, "%.*s", l, j->name + 1);
            for (char *c = out[n]; k && *c; c++)
                *c = (char)tolower((unsigned char)*c);
        }
    }
    FILE *f = fopen(g_pw_path, "r");
    char line[RAR5_PASSWORD_MAX + 3];
    while (f && n < max && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0])
            snprintf(out[n++], RAR5_PASSWORD_MAX + 1, "%s", line);
    }
    if (f)
        fclose(f);
    return n;
}

/* A password that opened an archive, kept to be tried on the next ones. */
static void remember_password(const char *pw) {
    if (!pw || !pw[0] || strchr(pw, '\n') || strchr(pw, '\r'))
        return;
    FILE *f = fopen(g_pw_path, "r");
    char line[RAR5_PASSWORD_MAX + 3];
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, pw)) {
            fclose(f);
            return;
        }
    }
    if (f)
        fclose(f);
    if ((f = fopen(g_pw_path, "a"))) {
        fprintf(f, "%s\n", pw);
        fclose(f);
    }
}

static void *x_thread(void *arg) {
    xvol_t *xv = arg;
    char outdir[PATH_MAX];
    snprintf(outdir, sizeof(outdir), "%.4000s/out", g_rt.work);
    struct archive *a = archive_read_new();
    if (!a) {
        fail_job("%s", TR("no memory for the archive", "no hay memoria para el archivo comprimido"));
        return NULL;
    }
    static char pw[RAR5_MAX_PASSWORDS][RAR5_PASSWORD_MAX + 1];
    int npw = passwords(g_rt.job, pw, RAR5_MAX_PASSWORDS);
    if (g_rt.kind == K_ZIP) {
        archive_read_support_format_zip_streamable(a);
        for (int i = 0; i < npw; i++)
            archive_read_add_passphrase(a, pw[i]);
    } else {
        archive_read_support_format_rar(a);
        archive_read_support_format_rar5(a);
        if (!(g_rt.keys = rar5keys_new())) {
            archive_read_free(a);
            fail_job("%s", TR("no memory for the archive", "no hay memoria para el archivo comprimido"));
            return NULL;
        }
        for (int i = 0; i < npw; i++)
            rar5keys_add(g_rt.keys, pw[i]);
        for (int i = 0; i < g_rt.nvol; i++)
            xv[i].dec = rar5dec_new(g_rt.keys);
    }
    archive_read_set_read_callback(a, x_read);
    archive_read_set_close_callback(a, x_close);
    archive_read_set_switch_callback(a, x_switch);
    for (int i = 0; i < g_rt.nvol; i++)
        if (i)
            archive_read_append_callback_data(a, &xv[i]);
        else
            archive_read_set_callback_data(a, &xv[0]);
    struct archive_entry *e;
    int r = archive_read_open1(a);
    if (r != ARCHIVE_OK)
        archive_failure(a);
    else {
        while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
            const char *path = archive_entry_pathname(e);
            const char *base = path ? path : "";
            for (const char *s = base; *s; s++)
                if (*s == '/' || *s == '\\')
                    base = s + 1;
            char name[256];
            clean_name(base, name, sizeof(name));
            if (archive_entry_is_encrypted(e) && g_rt.kind != K_ZIP) {
                /* a RAR5's went through the decoder plain: this is an older RAR */
                fail_job("%s", TR("the archive has a password of a kind PS4Mount cannot open (an older RAR, or a ZIP with AES)",
                                  "el archivo comprimido tiene una contraseña de un tipo que PS4Mount no puede abrir (un RAR antiguo o un ZIP con AES)"));
                break;
            }
            if (archive_entry_filetype(e) != AE_IFREG || !ends_with(name, ".pkg") || strlen(name) <= 4) {
                if (archive_read_data_skip(a) < ARCHIVE_WARN) {
                    archive_failure(a);
                    break;
                }
                continue;
            }
            if (extract_pkg(a, e, outdir, name))
                break;
            pthread_mutex_lock(&g_rt.mu);
            g_rt.npkgs++;
            pthread_mutex_unlock(&g_rt.mu);
        }
        if (r == ARCHIVE_EOF) {
            pthread_mutex_lock(&g_rt.mu);
            int none = !g_rt.npkgs;
            g_rt.x_ok = !none;
            g_rt.stop = 1; /* the archive is read: nothing more is needed */
            if (!none && g_rt.keys)
                remember_password(rar5keys_found(g_rt.keys));
            pthread_cond_broadcast(&g_rt.cv);
            pthread_mutex_unlock(&g_rt.mu);
            if (none)
                fail_job("%s", TR("the archive has no pkg inside", "el archivo comprimido no tiene ningún pkg"));
        } else if (r != ARCHIVE_OK && r != ARCHIVE_WARN)
            archive_failure(a);
    }
    archive_read_free(a);
    for (int i = 0; i < g_rt.nvol; i++) {
        x_close(NULL, &xv[i]);
        rar5dec_free(xv[i].dec);
        xv[i].dec = NULL;
    }
    pthread_mutex_lock(&g_rt.mu);
    rar5keys_free(g_rt.keys);
    g_rt.keys = NULL;
    pthread_mutex_unlock(&g_rt.mu);
    return NULL;
}

/* --- a whole download --- */

/* Deletes the files of a download's work folder (only what PS4Mount puts there). */
static void wipe_work(const char *work) {
    char out[PATH_MAX];
    snprintf(out, sizeof(out), "%.4000s/out", work);
    const char *dirs[2] = {out, work};
    for (int k = 0; k < 2; k++) {
        DIR *d = opendir(dirs[k]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.' || (k == 1 && !strcmp(e->d_name, "out")))
                continue;
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%.3800s/%.250s", dirs[k], e->d_name);
            struct stat st;
            if (!lstat(p, &st) && S_ISREG(st.st_mode))
                unlink(p);
        }
        if (d)
            closedir(d);
        rmdir(dirs[k]);
    }
}

static void work_dir(const char *scan, int id, char *out, size_t n) { snprintf(out, n, "%.3900s/" WORK_DIR "/%d", scan, id); }

/* The pkgs in out go to dir in the scan folder: the folder in one rename, or file by file
 * when dir is there already. 0 when all went. */
static int move_out(const char *out, const char *dir, char *err, size_t n) {
    if (!rename(out, dir))
        return 0;
    mkdir(dir, 0777);
    DIR *d = opendir(out);
    struct dirent *e;
    int rc = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char from[PATH_MAX], to[PATH_MAX];
        snprintf(from, sizeof(from), "%.3800s/%.250s", out, e->d_name);
        free_path(dir, e->d_name, to, sizeof(to));
        if (rename(from, to)) {
            snprintf(err, n, TR("%s could not be renamed (%s)", "%s no se pudo renombrar (%s)"), from, strerror(errno));
            rc = -1;
            break;
        }
    }
    if (d)
        closedir(d);
    rmdir(out);
    return rc;
}

static ljob_t *find_locked(int id) {
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i].id == id)
            return &g_jobs[i];
    return NULL;
}

static void save_queue_locked(void);

/* 0 done; 1 to be kept for later (PS4Mount stopping); -1 failed (g_rt.err says why). */
static int run_job(ljob_t *j) {
    char scan[PATH_MAX], out[PATH_MAX], err[600] = "", part[PATH_MAX];
    if (g_host.dest_dir(j->dest, scan, sizeof(scan))) {
        fail_job(TR("%s is not available", "%s no está disponible"), j->dest);
        return -1;
    }
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%.3900s/" WORK_DIR, scan);
    mkdir(parent, 0777);
    work_dir(scan, j->id, g_rt.work, sizeof(g_rt.work));
    if (!j->started) {
        /* a new download: nothing of an older one with the same number */
        wipe_work(g_rt.work);
        pthread_mutex_lock(&g_mu);
        j->started = 1;
        ljob_t *q = find_locked(j->id);
        if (q)
            q->started = 1;
        save_queue_locked();
        pthread_mutex_unlock(&g_mu);
    }
    mkdir(g_rt.work, 0777);
    snprintf(out, sizeof(out), "%.4000s/out", g_rt.work);
    /* the archive is read again from its start: its pkgs are written again */
    {
        DIR *d = opendir(out);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%.3800s/%.250s", out, e->d_name);
            if (e->d_name[0] != '.')
                unlink(p);
        }
        if (d)
            closedir(d);
    }
    mkdir(out, 0777);

    set_phase("probing");
    int kind = j->kind;
    for (int i = 0; i < j->nvol; i++) {
        pthread_mutex_lock(&g_mu);
        snprintf(g_cur_file, sizeof(g_cur_file), "%s", j->vol[i].name);
        g_file_i = i + 1;
        g_file_n = j->nvol;
        pthread_mutex_unlock(&g_mu);
        int k;
        if (probe(i, j->vol[i].url, &k, err, sizeof(err))) {
            if (halted())
                return 1;
            if (j->nvol > 1)
                fail_job("%s: %s", j->vol[i].name, err);
            else
                fail_job("%s", err);
            return -1;
        }
        if (i)
            continue;
        if (kind == K_UNKNOWN)
            kind = k;
        if (k != kind) {
            fail_job(TR("%s is not a pkg, RAR or ZIP file", "%s no es un archivo pkg, RAR ni ZIP"), j->vol[0].name);
            return -1;
        }
        if (j->kind == K_UNKNOWN && !j->vol[0].named && g_rt.v[0].fname[0]) {
            /* the server's name for it, also the download's */
            pthread_mutex_lock(&g_mu);
            snprintf(j->vol[0].name, sizeof(j->vol[0].name), "%s", g_rt.v[0].fname);
            snprintf(j->name, sizeof(j->name), "%s", g_rt.v[0].fname);
            if (ends_with(j->name, ".pkg") || ends_with(j->name, ".zip") || ends_with(j->name, ".rar"))
                j->name[strlen(j->name) - 4] = 0;
            snprintf(g_cur_name, sizeof(g_cur_name), "%s", j->name);
            ljob_t *q = find_locked(j->id);
            if (q)
                snprintf(q->name, sizeof(q->name), "%s", j->name);
            pthread_mutex_unlock(&g_mu);
        }
    }
    if (kind == K_UNKNOWN) {
        fail_job(TR("%s is not a pkg, RAR or ZIP file", "%s no es un archivo pkg, RAR ni ZIP"), j->vol[0].name);
        return -1;
    }
    g_rt.kind = kind;

    /* the space it needs: the files (part files of a cut download count as used already)
     * and, for an archive, its pkgs too, about as large; with less, each file is deleted
     * once read (a cut download then starts again) */
    uint64_t total = 0, held = 0, largest = 0;
    int unknown = 0;
    for (int i = 0; i < j->nvol; i++) {
        struct stat st;
        part_path(i, part, sizeof(part));
        held += !stat(part, &st) ? (uint64_t)st.st_blocks * 512 : 0;
        if (g_rt.v[i].size == UINT64_MAX)
            unknown = 1;
        else {
            total += g_rt.v[i].size;
            if (g_rt.v[i].size > largest)
                largest = g_rt.v[i].size;
        }
    }
    char stream_mark[PATH_MAX];
    snprintf(stream_mark, sizeof(stream_mark), "%.4000s/stream", g_rt.work);
    struct stat ms;
    if (!stat(stream_mark, &ms)) {
        /* the last run deleted files it had read: they would be needed again */
        part_path(0, part, sizeof(part));
        if (stat(part, &ms)) {
            printf("[links] %s: the files read last time were deleted, starting again\n", j->name);
            wipe_work(g_rt.work);
            mkdir(g_rt.work, 0777);
            mkdir(out, 0777);
            held = 0;
        }
        unlink(stream_mark);
    }
    uint64_t fb = g_host.free_bytes(scan), left = total > held ? total - held : 0;
    if (!unknown) {
        uint64_t keep_all = left + (kind == K_PKG ? 0 : total) + SPACE_MARGIN;
        uint64_t streaming = 2 * largest + total + SPACE_MARGIN;
        if (fb < keep_all && (kind == K_PKG || j->nvol < 3 || fb < streaming)) {
            fail_job(TR("not enough space: %llu MB free, %llu MB needed", "no hay espacio: %llu MB libres, %llu MB necesarios"),
                     (unsigned long long)(fb >> 20), (unsigned long long)(keep_all >> 20));
            return -1;
        }
        if (fb < keep_all) {
            g_rt.stream = 1;
            FILE *f = fopen(stream_mark, "w");
            if (f)
                fclose(f);
            printf("[links] %s: little space, each part is deleted once extracted\n", j->name);
        }
    }

    pthread_mutex_lock(&g_mu);
    g_all = unknown ? 0 : total;
    g_milestone = 0;
    pthread_mutex_unlock(&g_mu);
    set_phase("receiving");
    char size_text[32];
    fmt_size(total, size_text, sizeof(size_text));
    if (g_host.notify) {
        char m[400];
        if (unknown)
            snprintf(m, sizeof(m), TR("Downloading %s", "Descargando %s"), j->name);
        else
            snprintf(m, sizeof(m), TR("Downloading %s (%s)", "Descargando %s (%s)"), j->name, size_text);
        g_host.notify(m);
    }

    xvol_t *xv = NULL;
    pthread_t xt;
    int reading = 0;
    if (kind != K_PKG) {
        xv = mem_alloc(sizeof(*xv) * (size_t)j->nvol);
        if (!xv) {
            fail_job("%s", TR("no memory for the download", "no hay memoria para la descarga"));
            return -1;
        }
        for (int i = 0; i < j->nvol; i++) {
            xv[i].i = i;
            xv[i].fd = -1;
            xv[i].pos = 0;
            xv[i].buf = NULL;
            xv[i].dec = NULL;
        }
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1 << 20);
        reading = !pthread_create(&xt, &at, x_thread, xv);
        pthread_attr_destroy(&at);
        if (!reading) {
            mem_free(xv);
            fail_job("%s", TR("the archive reader could not start", "no se pudo empezar a leer el archivo comprimido"));
            return -1;
        }
    }
    for (int i = 0; i < j->nvol && !halted(); i++) {
        /* with little space, at most the file being read and the next one are here */
        pthread_mutex_lock(&g_rt.mu);
        while (g_rt.stream && i > g_rt.x_vol + 1 && !halted()) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_rt.cv, &g_rt.mu, &ts);
        }
        pthread_mutex_unlock(&g_rt.mu);
        if (halted())
            break;
        pthread_mutex_lock(&g_mu);
        snprintf(g_cur_file, sizeof(g_cur_file), "%s", j->vol[i].name);
        g_file_i = i + 1;
        pthread_mutex_unlock(&g_mu);
        part_path(i, part, sizeof(part));
        if (g_rt.v[i].size == UINT64_MAX)
            printf("[links] %s: downloading %s (size unknown) into %s\n", j->name, j->vol[i].url, part);
        else
            printf("[links] %s: downloading %s (%llu bytes) into %s\n", j->name, j->vol[i].url,
                   (unsigned long long)g_rt.v[i].size, part);
        if (download_file(i, part, err, sizeof(err))) {
            if ((!halted() || g_rt.failed) && j->nvol > 1)
                fail_job("%s: %s", j->vol[i].name, err);
            else if (!halted() || g_rt.failed)
                fail_job("%s", err);
            break;
        }
        pthread_mutex_lock(&g_rt.mu);
        g_rt.v[i].complete = 1;
        pthread_cond_broadcast(&g_rt.cv);
        pthread_mutex_unlock(&g_rt.mu);
    }
    if (reading) {
        if (!halted()) {
            pthread_mutex_lock(&g_rt.mu);
            uint64_t consumed = g_rt.consumed;
            pthread_mutex_unlock(&g_rt.mu);
            set_phase("extracting");
            progress(consumed);
        }
        pthread_join(xt, NULL);
        mem_free(xv);
    }
    if (g_rt.failed)
        return -1;
    if (g_cancel || g_quit)
        return 1;
    if (kind != K_PKG && !g_rt.x_ok) {
        fail_job("%s", TR("the archive ended before its end", "el archivo comprimido se cortó antes de su final"));
        return -1;
    }

    /* all here: a pkg link's file takes its name, the pkgs go to the scan folder */
    if (kind == K_PKG) {
        char name[256], dst[PATH_MAX];
        char base[250];
        clean_name(j->vol[0].name, base, sizeof(base));
        snprintf(name, sizeof(name), "%s%s", base, ends_with(base, ".pkg") ? "" : ".pkg");
        part_path(0, part, sizeof(part));
        free_path(out, name, dst, sizeof(dst));
        if (rename(part, dst)) {
            fail_job(TR("%s could not be renamed (%s)", "%s no se pudo renombrar (%s)"), part, strerror(errno));
            return -1;
        }
        g_rt.npkgs = 1;
    }
    char folder[256], dir[PATH_MAX];
    clean_name(j->name, folder, sizeof(folder));
    if (ends_with(folder, ".pkg"))
        folder[strlen(folder) - 4] = 0;
    snprintf(dir, sizeof(dir), "%.3800s/%.250s", scan, folder[0] ? folder : "download");
    if (move_out(out, dir, err, sizeof(err))) {
        fail_job("%s", err);
        return -1;
    }
    wipe_work(g_rt.work);
    rmdir(parent);
    char took[48];
    fmt_left((double)(time(NULL) - g_job_start), took, sizeof(took));
    set_result(1, TR("%s was downloaded: %d pkg(s), %s in %s", "%s se descargó: %d pkg(s), %s en %s"), j->name, g_rt.npkgs,
               size_text, took + (took[0] == '~'));
    if (g_host.notify)
        g_host.notify(g_result);
    if (g_host.arrived)
        g_host.arrived(dir);
    return 0;
}

/* --- the queue --- */

/* links_queue.txt: a "job" line per download and a "link" line per file of it, tabs
 * between the fields (none of them holds a tab or a line break). */
static void save_queue_locked(void) {
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_queue_path);
    if (!g_njobs) {
        unlink(g_queue_path);
        return;
    }
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (int i = 0; i < g_njobs; i++) {
        const ljob_t *j = &g_jobs[i];
        fprintf(f, "job\t%d\t%s\t%d\t%d\t%s\t%s\t%s\t%d\n", j->id, k_states[j->state == S_RUNNING ? S_QUEUED : j->state],
                j->started, j->kind, j->dest, j->name, j->error, j->locked);
        if (j->password[0]) {
            fprintf(f, "password\t");
            for (const char *c = j->password; *c; c++)
                fprintf(f, "%02x", (unsigned char)*c);
            fprintf(f, "\n");
        }
        for (int k = 0; k < j->nvol; k++)
            fprintf(f, "link\t%d\t%s\t%s\n", j->vol[k].named, j->vol[k].name, j->vol[k].url);
    }
    int ok = !ferror(f);
    if (fclose(f) || !ok)
        unlink(tmp);
    else
        rename(tmp, g_queue_path);
}

/* The next field of a line (tabs between them). */
static char *field(char **p) {
    char *s = *p, *t = s ? strchr(s, '\t') : NULL;
    if (t)
        *t = 0;
    *p = t ? t + 1 : NULL;
    return s ? s : (char *)"";
}

static void free_job(ljob_t *j) {
    mem_free(j->vol);
    j->vol = NULL;
    j->nvol = 0;
}

static int load_queue_locked(void) {
    FILE *f = fopen(g_queue_path, "r");
    if (!f)
        return 0;
    char *line = mem_alloc(URL_MAX + 1024);
    ljob_t *j = NULL;
    while (line && fgets(line, URL_MAX + 1024, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line, *kind = field(&p);
        if (!strcmp(kind, "job") && g_njobs < LINKS_MAX_JOBS) {
            j = &g_jobs[g_njobs++];
            memset(j, 0, sizeof(*j));
            j->id = atoi(field(&p));
            const char *st = field(&p);
            j->state = !strcmp(st, "failed") ? S_FAILED : S_QUEUED;
            j->started = atoi(field(&p));
            j->kind = atoi(field(&p));
            snprintf(j->dest, sizeof(j->dest), "%s", field(&p));
            snprintf(j->name, sizeof(j->name), "%s", field(&p));
            snprintf(j->error, sizeof(j->error), "%s", field(&p));
            j->locked = atoi(field(&p));
            if (j->kind < K_UNKNOWN || j->kind > K_ZIP)
                j->kind = K_UNKNOWN;
            if (!(j->vol = mem_alloc(sizeof(link_t) * LINKS_MAX_FILES)))
                g_njobs--, j = NULL;
            else if (j->id >= g_next_id)
                g_next_id = j->id + 1;
        } else if (!strcmp(kind, "password") && j) {
            const char *h = field(&p);
            size_t o = 0;
            for (unsigned b; o < RAR5_PASSWORD_MAX && sscanf(h, "%2x", &b) == 1 && b; h += 2)
                j->password[o++] = (char)b;
            j->password[o] = 0;
        } else if (!strcmp(kind, "link") && j && j->nvol < LINKS_MAX_FILES) {
            link_t *l = &j->vol[j->nvol];
            l->named = atoi(field(&p));
            snprintf(l->name, sizeof(l->name), "%s", field(&p));
            snprintf(l->url, sizeof(l->url), "%s", field(&p));
            if (!strncmp(l->url, "http://", 7) || !strncmp(l->url, "https://", 8))
                j->nvol++;
        }
    }
    mem_free(line);
    fclose(f);
    /* a download without files is dropped */
    int resumed = 0;
    for (int i = 0; i < g_njobs;) {
        if (!g_jobs[i].nvol) {
            free_job(&g_jobs[i]);
            memmove(&g_jobs[i], &g_jobs[i + 1], sizeof(g_jobs[0]) * (size_t)(g_njobs - i - 1));
            g_njobs--;
            continue;
        }
        resumed += g_jobs[i].state == S_QUEUED;
        i++;
    }
    return resumed;
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    while (!g_quit) {
        ljob_t *j = NULL;
        for (int i = 0; i < g_njobs && !j; i++)
            if (g_jobs[i].state == S_QUEUED)
                j = &g_jobs[i];
        if (!j) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
            continue;
        }
        j->state = S_RUNNING;
        j->error[0] = 0;
        /* a copy runs: the queue may move while it does (the files stay where they are) */
        ljob_t run = *j;
        g_busy = 1;
        g_cur_id = run.id;
        g_cancel = 0;
        snprintf(g_cur_name, sizeof(g_cur_name), "%s", run.name);
        g_cur_file[0] = g_cur_pkg[0] = 0;
        g_file_i = 0;
        g_file_n = run.nvol;
        g_done = g_all = 0;
        g_nsamples = 0;
        g_milestone = 0;
        g_retry_until = 0;
        g_job_start = time(NULL);
        g_phase = "probing";
        pthread_mutex_unlock(&g_mu);

        vol_t *v = mem_alloc(sizeof(vol_t) * (size_t)run.nvol);
        pthread_mutex_lock(&g_rt.mu);
        g_rt.v = v;
        g_rt.nvol = run.nvol;
        g_rt.kind = run.kind;
        g_rt.job = &run;
        g_rt.stop = g_rt.failed = g_rt.x_ok = g_rt.npkgs = g_rt.stream = g_rt.checked = g_rt.locked = 0;
        g_rt.x_vol = 0;
        g_rt.consumed = 0;
        g_rt.err[0] = 0;
        g_rt.work[0] = 0;
        pthread_mutex_unlock(&g_rt.mu);
        int rc;
        if (!v) {
            fail_job("%s", TR("no memory for the download", "no hay memoria para la descarga"));
            rc = -1;
        } else {
            memset(v, 0, sizeof(vol_t) * (size_t)run.nvol);
            rc = run_job(&run);
        }
        if (g_cancel && !g_quit) {
            set_result(0, TR("%s: cancelled", "%s: cancelado"), run.name);
            if (g_rt.work[0]) {
                wipe_work(g_rt.work);
                char *slash = strrchr(g_rt.work, '/');
                if (slash) {
                    *slash = 0;
                    rmdir(g_rt.work); /* the hidden folder, when no other download is in it */
                }
            }
        } else if (rc < 0) {
            set_result(0, TR("%s was not downloaded: %s", "%s no se descargó: %s"), run.name, g_rt.err);
            if (g_host.notify)
                g_host.notify(g_result);
        }
        pthread_mutex_lock(&g_rt.mu);
        g_rt.v = NULL;
        g_rt.job = NULL;
        pthread_mutex_unlock(&g_rt.mu);
        mem_free(v);

        pthread_mutex_lock(&g_mu);
        ljob_t *now = find_locked(run.id);
        if (now) {
            /* cut by PS4Mount stopping: it stays, for the next start */
            if (g_quit || (rc > 0 && !g_cancel))
                now->state = S_QUEUED;
            else if (rc < 0 && !g_cancel) {
                now->state = S_FAILED;
                now->locked = g_rt.locked;
                snprintf(now->error, sizeof(now->error), "%.511s", g_rt.err);
            } else {
                free_job(now);
                int i = (int)(now - g_jobs);
                memmove(&g_jobs[i], &g_jobs[i + 1], sizeof(g_jobs[0]) * (size_t)(g_njobs - i - 1));
                g_njobs--;
            }
            /* PS4Mount stopping saves the queue as it was */
            if (!g_quit)
                save_queue_locked();
        }
        g_busy = 0;
        g_cur_id = 0;
        g_cancel = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

void links_start(const char *state_dir, const links_host_t *host) {
    g_host = *host;
    snprintf(g_queue_path, sizeof(g_queue_path), "%s/links_queue.txt", state_dir);
    snprintf(g_pw_path, sizeof(g_pw_path), "%s/passwords.txt", state_dir);
    if (g_started)
        return;
    g_quit = 0;
    pthread_mutex_lock(&g_mu);
    int resumed = load_queue_locked();
    pthread_mutex_unlock(&g_mu);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    if (!pthread_create(&g_thread, &at, worker, NULL)) {
        g_started = 1;
        if (g_relay_fd < 0)
            relay_start();
        if (resumed) {
            printf("[links] %d download(s) left by the last run: going on with them\n", resumed);
            if (g_host.notify) {
                char m[200];
                snprintf(m, sizeof(m), TR("Going on with %d download(s) from links left unfinished",
                                          "Continuando %d descarga(s) de enlaces que quedaron a medias"),
                         resumed);
                g_host.notify(m);
            }
        }
    }
    pthread_attr_destroy(&at);
}

void links_stop(void) {
    if (!g_started)
        return;
    pthread_mutex_lock(&g_mu);
    g_quit = 1;
    g_cancel = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    g_rt.stop = 1;
    pthread_join(g_thread, NULL);
    relay_stop();
    g_started = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_njobs; i++)
        free_job(&g_jobs[i]);
    g_njobs = 0;
    g_result[0] = 0;
    g_result_ok = 1;
    pthread_mutex_unlock(&g_mu);
}

int links_active(void) {
    pthread_mutex_lock(&g_mu);
    int a = g_busy;
    for (int i = 0; i < g_njobs && !a; i++)
        a = g_jobs[i].state == S_QUEUED;
    pthread_mutex_unlock(&g_mu);
    return a;
}

void links_status_json(buf_t *out) {
    pthread_mutex_lock(&g_mu);
    time_t now = time(NULL);
    buf_printf(out, "{\"configured\":%s,\"relay_port\":%d,\"jobs\":[", g_started ? "true" : "false", links_relay_port());
    for (int i = 0; i < g_njobs; i++) {
        const ljob_t *j = &g_jobs[i];
        buf_printf(out, "%s{\"id\":%d,\"name\":", i ? "," : "", j->id);
        json_esc(out, j->name);
        buf_printf(out, ",\"dest\":");
        json_esc(out, j->dest);
        buf_printf(out, ",\"files\":%d,\"state\":\"%s\",\"locked\":%s,\"error\":", j->nvol, k_states[j->state],
                   j->locked ? "true" : "false");
        json_esc(out, j->error);
        buf_printf(out, "}");
    }
    long retry_in = g_busy && g_retry_until > now ? (long)(g_retry_until - now) : 0;
    const char *phase = !g_busy ? "" : retry_in ? "retrying" : g_phase;
    buf_printf(out, "],\"job\":{\"busy\":%s,\"relay\":%s,\"id\":%d,\"name\":", g_busy ? "true" : "false",
               g_busy && relay_active() ? "true" : "false", g_busy ? g_cur_id : 0);
    json_esc(out, g_busy ? g_cur_name : "");
    buf_printf(out, ",\"file\":");
    json_esc(out, g_busy ? g_cur_file : "");
    buf_printf(out, ",\"pkg\":");
    json_esc(out, g_busy ? g_cur_pkg : "");
    buf_printf(out, ",\"file_index\":%d,\"files\":%d,\"done\":%llu,\"total\":%llu,\"speed\":%.0f,\"elapsed\":%lld,\"retry_in\":%ld,\"phase\":\"%s\"}",
               g_file_i, g_file_n, (unsigned long long)(g_busy ? g_done : 0), (unsigned long long)(g_busy ? g_all : 0),
               g_busy ? speed_locked() : 0.0, (long long)(g_busy ? now - g_job_start : 0), retry_in, phase);
    buf_printf(out, ",\"result\":");
    json_esc(out, g_result);
    buf_printf(out, ",\"ok\":%s}", g_result_ok ? "true" : "false");
    pthread_mutex_unlock(&g_mu);
}

/* --- adding links --- */

typedef struct {
    link_t l;
    int kind, style, idx; /* style: 0 one file, 1 name.partN.rar, 2 name.rar + name.rNN */
    char base[256];       /* the archive's name without the volume part (lowercase) */
    int boff;             /* where base starts in the name */
    int used;
} item_t;

/* What a file name says it is: its kind, and for a volume its archive and number. */
static void classify(item_t *it) {
    const char *n = it->l.name;
    size_t l = strlen(n);
    it->kind = K_UNKNOWN;
    it->style = it->idx = 0;
    snprintf(it->base, sizeof(it->base), "%s", n);
    if (ends_with(n, ".pkg"))
        it->kind = K_PKG;
    else if (ends_with(n, ".zip"))
        it->kind = K_ZIP;
    else if (ends_with(n, ".rar")) {
        it->kind = K_RAR;
        /* name.part01.rar */
        const char *d = n + l - 4, *s = d;
        while (s > n && isdigit((unsigned char)s[-1]))
            s--;
        if (s < d && s - n >= 5 && !strncasecmp(s - 5, ".part", 5)) {
            it->style = 1;
            it->idx = atoi(s);
            snprintf(it->base, sizeof(it->base), "%.*s", (int)(s - 5 - n), n);
        } else {
            it->style = 2; /* the first volume of name.rar, name.r00...; or a single file */
            snprintf(it->base, sizeof(it->base), "%.*s", (int)(l - 4), n);
        }
    } else if (l > 4 && (n[l - 4] == '.' || n[l - 3] == '.')) {
        /* name.r00 (or .r000) */
        const char *dot = n[l - 4] == '.' ? n + l - 4 : n + l - 3;
        if (tolower((unsigned char)dot[1]) == 'r' && isdigit((unsigned char)dot[2]) && isdigit((unsigned char)dot[3]) &&
            (!dot[4] || (isdigit((unsigned char)dot[4]) && !dot[5]))) {
            it->kind = K_RAR;
            it->style = 2;
            it->idx = atoi(dot + 2) + 1;
            snprintf(it->base, sizeof(it->base), "%.*s", (int)(dot - n), n);
        }
    }
    for (char *c = it->base; *c; c++)
        *c = (char)tolower((unsigned char)*c);
}

static int item_cmp(const void *a, const void *b) {
    const item_t *x = a, *y = b;
    return x->idx - y->idx;
}

/* name.partN.rar after a host's own code (code-name.partN.rar), or NULL. */
static const char *coded_base(const item_t *it) {
    const char *d = it->style == 1 ? strchr(it->base, '-') : NULL;
    return d && d[1] ? d + 1 : NULL;
}

/* Whether the name.partN.rar volumes with this base (or, coded, base after the code) are
   parts 1 to N, each once. */
static int whole_archive(const item_t *items, int ni, const char *base, int coded) {
    unsigned char seen[LINKS_MAX_FILES + 1] = {0};
    int count = 0, top = 0;
    for (int k = 0; k < ni; k++) {
        const char *b = coded ? coded_base(&items[k]) : items[k].style == 1 ? items[k].base : NULL;
        if (!b || strcmp(b, base))
            continue;
        int x = items[k].idx;
        if (x < 1 || x > LINKS_MAX_FILES || seen[x]++)
            return 0;
        count++;
        top = x > top ? x : top;
    }
    return count && top == count;
}

/* Some hosts put a code of their own before each file's name, a different one for each part
   (covFB...-game.part1.rar, coBCQ...-game.part2.rar): parts that are no whole archive by
   their names become one by the names after the code, when those are. */
static void join_coded(item_t *items, int ni) {
    for (int i = 0; i < ni; i++) {
        const char *t = coded_base(&items[i]);
        if (!t || whole_archive(items, ni, items[i].base, 0) || !whole_archive(items, ni, t, 1))
            continue;
        char key[256];
        snprintf(key, sizeof(key), "%s", t);
        for (int k = 0; k < ni; k++) {
            const char *tk = coded_base(&items[k]);
            if (tk && !strcmp(tk, key)) {
                items[k].boff += (int)(tk - items[k].base);
                memmove(items[k].base, tk, strlen(tk) + 1);
            }
        }
    }
}

/* One line: "link" or "link | name". 0 with it in it, -1 with why not in err. */
static int parse_line(const char *line, item_t *it, char *err, size_t n) {
    memset(it, 0, sizeof(*it));
    const char *bar = strrchr(line, '|');
    char url[URL_MAX + 1], given[256] = "";
    size_t ul = bar ? (size_t)(bar - line) : strlen(line);
    if (ul > URL_MAX) {
        snprintf(err, n, "%s", TR("a link is too long", "un enlace es demasiado largo"));
        return -1;
    }
    memcpy(url, line, ul);
    url[ul] = 0;
    while (ul && isspace((unsigned char)url[ul - 1]))
        url[--ul] = 0;
    if (bar) {
        const char *g = bar + 1;
        while (isspace((unsigned char)*g))
            g++;
        clean_name(g, given, sizeof(given));
    }
    /* spaces inside a pasted address are %20 */
    size_t o = 0;
    for (const char *p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            snprintf(err, n, "%s", TR("a link has characters that do not belong in an address", "un enlace tiene caracteres que no van en una dirección"));
            return -1;
        }
        if (c == ' ') {
            if (o + 3 >= sizeof(it->l.url))
                break;
            memcpy(it->l.url + o, "%20", 3);
            o += 3;
        } else if (o + 1 < sizeof(it->l.url))
            it->l.url[o++] = (char)c;
    }
    it->l.url[o] = 0;
    http_url_t u;
    if (http_parse_url(it->l.url, &u)) {
        snprintf(err, n, TR("not an http:// or https:// link: %.80s", "no es un enlace http:// ni https://: %.80s"), it->l.url);
        return -1;
    }
    if (given[0]) {
        snprintf(it->l.name, sizeof(it->l.name), "%s", given);
        it->l.named = 1;
    } else
        url_name(it->l.url, it->l.name, sizeof(it->l.name));
    if (!it->l.name[0])
        snprintf(it->l.name, sizeof(it->l.name), "%s", u.host);
    classify(it);
    return 0;
}

static int valid_dest(const char *d) {
    if (!d[0] || strlen(d) >= 16)
        return 0;
    for (const char *p = d; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            return 0;
    return 1;
}

int links_add(const char *text, const char *dest, const char *password, char *msg, size_t n) {
    if (!g_started) {
        snprintf(msg, n, "%s", TR("downloads from links are not running", "las descargas de enlaces no están en marcha"));
        return -1;
    }
    if (!valid_dest(dest)) {
        snprintf(msg, n, "%s", TR("choose where to save it", "elige dónde guardarlo"));
        return -1;
    }
    if (password && strlen(password) > RAR5_PASSWORD_MAX) {
        snprintf(msg, n, TR("the password is too long (at most %d characters)", "la contraseña es demasiado larga (como máximo %d caracteres)"),
                 RAR5_PASSWORD_MAX);
        return -1;
    }
    item_t *items = mem_alloc(sizeof(item_t) * LINKS_MAX_FILES);
    if (!items) {
        snprintf(msg, n, "%s", "no memory");
        return -1;
    }
    int ni = 0, rc = 0;
    char err[300] = "";
    for (const char *p = text; *p && !rc;) {
        size_t l = strcspn(p, "\r\n");
        char *line = mem_alloc(l + 1);
        if (!line) {
            rc = -1;
            snprintf(err, sizeof(err), "no memory");
            break;
        }
        memcpy(line, p, l);
        line[l] = 0;
        p += l;
        p += strspn(p, "\r\n");
        char *s = line;
        while (isspace((unsigned char)*s))
            s++;
        if (*s && *s != '#') {
            if (ni >= LINKS_MAX_FILES) {
                snprintf(err, sizeof(err), TR("at most %d links at a time", "como máximo %d enlaces a la vez"), LINKS_MAX_FILES);
                rc = -1;
            } else if (parse_line(s, &items[ni], err, sizeof(err)))
                rc = -1;
            else
                ni++;
        }
        mem_free(line);
    }
    if (!rc && !ni) {
        snprintf(err, sizeof(err), "%s", TR("paste at least one link", "pega al menos un enlace"));
        rc = -1;
    }
    if (!rc)
        join_coded(items, ni);
    /* the volumes of each archive together, in order, all of them there */
    ljob_t add[LINKS_MAX_JOBS];
    int na = 0;
    memset(add, 0, sizeof(add));
    for (int i = 0; i < ni && !rc; i++) {
        if (items[i].used)
            continue;
        int first = i, count = 0;
        /* the items of the same archive go to the front of the rest, sorted */
        if (items[i].kind == K_RAR) {
            for (int k = i; k < ni; k++) {
                if (!items[k].used && items[k].kind == K_RAR && items[k].style == items[i].style &&
                    !strcmp(items[k].base, items[i].base)) {
                    item_t t = items[k];
                    memmove(&items[first + count + 1], &items[first + count], sizeof(item_t) * (size_t)(k - first - count));
                    items[first + count] = t;
                    items[first + count].used = 1;
                    count++;
                }
            }
            qsort(&items[first], (size_t)count, sizeof(item_t), item_cmp);
            int start = items[first].style == 1 ? 1 : 0;
            for (int k = 0; k < count && !rc; k++) {
                if (items[first + k].idx == start + k)
                    continue;
                const char *what = items[first + k].idx < start + k ? TR("is there twice", "está dos veces") : TR("is missing", "falta");
                int part = items[first + k].idx < start + k ? items[first + k].idx : start + k;
                if (items[first].style == 1)
                    snprintf(err, sizeof(err), TR("%s: part %d %s", "%s: la parte %d %s"), items[first].l.name, part, what);
                else if (part == 0)
                    snprintf(err, sizeof(err), TR("%s: the first volume (.rar) %s", "%s: el primer volumen (.rar) %s"), items[first].l.name, what);
                else
                    snprintf(err, sizeof(err), TR("%s: volume .r%02d %s", "%s: el volumen .r%02d %s"), items[first].l.name, part - 1, what);
                rc = -1;
            }
        } else {
            items[i].used = 1;
            count = 1;
        }
        if (rc)
            break;
        if (g_njobs + na >= LINKS_MAX_JOBS) {
            snprintf(err, sizeof(err), TR("at most %d downloads in the list", "como máximo %d descargas en la lista"), LINKS_MAX_JOBS);
            rc = -1;
            break;
        }
        ljob_t *j = &add[na++];
        j->kind = items[first].kind;
        snprintf(j->dest, sizeof(j->dest), "%s", dest);
        snprintf(j->password, sizeof(j->password), "%s", password ? password : "");
        if (!(j->vol = mem_alloc(sizeof(link_t) * LINKS_MAX_FILES))) {
            snprintf(err, sizeof(err), "no memory");
            rc = -1;
            break;
        }
        for (int k = 0; k < count; k++)
            j->vol[j->nvol++] = items[first + k].l;
        /* the download's name: the archive's, without its volume part */
        const item_t *f = &items[first];
        if (f->kind == K_RAR) {
            size_t bl = strlen(f->base);
            snprintf(j->name, sizeof(j->name), "%.*s", (int)bl, f->l.name + f->boff);
        } else
            snprintf(j->name, sizeof(j->name), "%s", f->l.name);
        if (ends_with(j->name, ".zip") || ends_with(j->name, ".pkg"))
            j->name[strlen(j->name) - 4] = 0;
        i = first + count - 1;
    }
    mem_free(items);
    if (rc) {
        for (int i = 0; i < na; i++)
            free_job(&add[i]);
        snprintf(msg, n, "%s", err);
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    int added = 0, same = 0;
    for (int i = 0; i < na; i++) {
        /* the same first link already in the list: not twice */
        int dup = 0;
        for (int k = 0; k < g_njobs && !dup; k++)
            dup = !strcmp(g_jobs[k].vol[0].url, add[i].vol[0].url);
        if (dup || g_njobs >= LINKS_MAX_JOBS) {
            free_job(&add[i]);
            same++;
            continue;
        }
        add[i].id = g_next_id++;
        add[i].state = S_QUEUED;
        g_jobs[g_njobs++] = add[i];
        added++;
    }
    save_queue_locked();
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    if (!added)
        snprintf(msg, n, "%s", TR("those links are already in the list", "esos enlaces ya están en la lista"));
    else if (same)
        snprintf(msg, n, TR("%d download(s) added (%d were already in the list)", "%d descarga(s) añadida(s) (%d ya estaban en la lista)"), added, same);
    else
        snprintf(msg, n, TR("%d download(s) added", "%d descarga(s) añadida(s)"), added);
    return added ? 0 : -1;
}

int links_cancel(int id, char *msg, size_t n) {
    pthread_mutex_lock(&g_mu);
    ljob_t *j = find_locked(id);
    if (!j) {
        pthread_mutex_unlock(&g_mu);
        snprintf(msg, n, "%s", TR("that download is not in the list", "esa descarga no está en la lista"));
        return -1;
    }
    if (g_busy && g_cur_id == id) {
        g_cancel = 1;
        g_rt.stop = 1;
        pthread_mutex_unlock(&g_mu);
        pthread_mutex_lock(&g_rt.mu);
        pthread_cond_broadcast(&g_rt.cv);
        pthread_mutex_unlock(&g_rt.mu);
        snprintf(msg, n, TR("%s will stop and its files will be deleted", "%s se detendrá y se borrarán sus archivos"), j->name);
        return 0;
    }
    char name[256], dest[16], scan[PATH_MAX], work[PATH_MAX];
    snprintf(name, sizeof(name), "%s", j->name);
    snprintf(dest, sizeof(dest), "%s", j->dest);
    int id_ = j->id, started = j->started;
    free_job(j);
    int i = (int)(j - g_jobs);
    memmove(&g_jobs[i], &g_jobs[i + 1], sizeof(g_jobs[0]) * (size_t)(g_njobs - i - 1));
    g_njobs--;
    save_queue_locked();
    pthread_mutex_unlock(&g_mu);
    /* what a failed or cut download left */
    if (started && !g_host.dest_dir(dest, scan, sizeof(scan))) {
        work_dir(scan, id_, work, sizeof(work));
        wipe_work(work);
        snprintf(work, sizeof(work), "%.3900s/" WORK_DIR, scan);
        rmdir(work);
    }
    snprintf(msg, n, TR("%s was removed from the list", "%s se quitó de la lista"), name);
    return 0;
}

int links_retry(int id, const char *password, char *msg, size_t n) {
    pthread_mutex_lock(&g_mu);
    ljob_t *j = find_locked(id);
    int rc = -1;
    if (!j)
        snprintf(msg, n, "%s", TR("that download is not in the list", "esa descarga no está en la lista"));
    else if (j->state != S_FAILED)
        snprintf(msg, n, "%s", TR("that download has not failed", "esa descarga no ha fallado"));
    else if (password && strlen(password) > RAR5_PASSWORD_MAX)
        snprintf(msg, n, TR("the password is too long (at most %d characters)", "la contraseña es demasiado larga (como máximo %d caracteres)"),
                 RAR5_PASSWORD_MAX);
    else {
        if (password && password[0])
            snprintf(j->password, sizeof(j->password), "%s", password);
        j->state = S_QUEUED;
        j->locked = 0;
        j->error[0] = 0;
        save_queue_locked();
        pthread_cond_signal(&g_cv);
        snprintf(msg, n, TR("%s will be downloaded again from where it was", "%s se descargará de nuevo desde donde iba"), j->name);
        rc = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}
