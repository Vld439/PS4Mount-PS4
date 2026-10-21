/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Downloads from links (src/links.c) against a local web server: a RAR in volumes (in any
 * order, through redirects, both namings), a ZIP, a pkg (also from a server without
 * ranges, and named by the server), cuts, a server that limits connections, an address
 * that expires, a stop and a resume, little space, a cancel, and what must fail (a pkg
 * that is not fake, a web page, a password, no pkg inside, a missing file, missing parts).
 * The archives are in test/data/links (made by test/data/links/make.sh).
 * Run with: bash test/run.sh
 *   links_test --make-pkgs <dir>   writes the pkgs the archives hold */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "https.h"
#include "json.h"
#include "links.h"
#include "pkgmake.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

#define DATA "test/data/links"
#define GAME_CID "UP0000-CUSA12345_00-TESTGAME00000000"

/* --- the pkgs inside the archives --- */

/* A pkg as make_pkg writes it, with pseudo-random bytes at at (len of them) so the archives
 * hold more than zeros. */
static void write_pkg(const char *path, const char *cat, int fake, size_t size, size_t at, size_t len) {
    make_pkg(path, GAME_CID, cat, 0x09008000, fake, size, size);
    FILE *f = fopen(path, "r+b");
    uint32_t x = 2463534242u ^ (uint32_t)size;
    fseek(f, (long)at, SEEK_SET);
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        fputc((int)(x & 0xff), f);
    }
    fclose(f);
}

static void make_pkgs(const char *dir) {
    char p[512];
    snprintf(p, sizeof(p), "%s/game.pkg", dir);
    write_pkg(p, "gd", 1, 5u << 20, 0x100000, 0x10000); /* past 4 MB: checked midway */
    snprintf(p, sizeof(p), "%s/update.pkg", dir);
    write_pkg(p, "gp", 1, 300000, 0x10000, 0x8000); /* under 4 MB: checked at its end */
    snprintf(p, sizeof(p), "%s/readme.txt", dir);
    FILE *f = fopen(p, "w");
    fputs("not a pkg: left out\n", f);
    fclose(f);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l > 0 ? (size_t)l : 1);
    *n = fread(d, 1, (size_t)l, f);
    fclose(f);
    return d;
}

/* --- the web server --- */

#define MAXF 64
typedef struct {
    char name[160];
    uint8_t *data;
    size_t size;
    int no_ranges, html;
    char disp[160];
} sfile_t;

static sfile_t g_sf[MAXF];
static int g_nsf, g_port, g_listen;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
/* faults, each used once */
static int g_cut_once;    /* the next ranged answer: half of it, then the connection closes */
static int g_throttle;    /* the next ranged requests get 429 */
static int g_expire_after; /* after this many ranged requests, signed addresses so far stop working */
static int g_slow_us;     /* a pause every 16 KB sent */
static int g_sig = 1, g_sig_valid; /* /e/ hands out ?sig=g_sig; one under g_sig_valid gets 403 */
static int g_cuts, g_429s, g_resolves, g_ranged, g_403s;

static void add_file(const char *name, const uint8_t *data, size_t size) {
    sfile_t *f = &g_sf[g_nsf++];
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->data = malloc(size ? size : 1);
    memcpy(f->data, data, size);
    f->size = size;
}

static void add_path(const char *name, const char *path) {
    size_t n;
    uint8_t *d = slurp(path, &n);
    if (!d) {
        printf("  missing %s\n", path);
        exit(1);
    }
    add_file(name, d, n);
    free(d);
}

static sfile_t *find_file(const char *name) {
    for (int i = 0; i < g_nsf; i++)
        if (!strcmp(g_sf[i].name, name))
            return &g_sf[i];
    return NULL;
}

static int send_all(int fd, const void *p, size_t n, int slow) {
    for (size_t o = 0; o < n;) {
        size_t l = n - o < 16384 ? n - o : 16384;
        ssize_t w = send(fd, (const char *)p + o, l, MSG_NOSIGNAL);
        if (w <= 0)
            return -1;
        o += (size_t)w;
        if (slow && g_slow_us)
            usleep((useconds_t)g_slow_us);
    }
    return 0;
}

static void head(int fd, int status, const char *extra, size_t len) {
    char h[1024];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %d X\r\nContent-Length: %zu\r\n%s\r\n", status, len, extra ? extra : "");
    send_all(fd, h, (size_t)n, 0);
}

/* One request; 0 to keep the connection. */
static int serve(int fd, const char *req) {
    char path[512] = "", extra[512] = "";
    sscanf(req, "GET %511s", path);
    const char *rg = strstr(req, "Range: bytes=");
    unsigned long long a = 0, b = 0;
    int ranged = rg && sscanf(rg + 13, "%llu-%llu", &a, &b) == 2;
    if (!strncmp(path, "/r/", 3)) {
        snprintf(extra, sizeof(extra), "Location: /f/%s\r\n", path + 3);
        head(fd, 302, extra, 0);
        return 0;
    }
    if (!strncmp(path, "/rr/", 4)) {
        snprintf(extra, sizeof(extra), "Location: http://127.0.0.1:%d/r/%s\r\n", g_port, path + 4);
        head(fd, 301, extra, 0);
        return 0;
    }
    if (!strncmp(path, "/e/", 3)) {
        pthread_mutex_lock(&g_smu);
        g_resolves++;
        snprintf(extra, sizeof(extra), "Location: /f/%s?sig=%d\r\n", path + 3, g_sig);
        pthread_mutex_unlock(&g_smu);
        head(fd, 302, extra, 0);
        return 0;
    }
    if (strncmp(path, "/f/", 3)) {
        head(fd, 404, NULL, 0);
        return 0;
    }
    char name[256];
    snprintf(name, sizeof(name), "%.*s", (int)strcspn(path + 3, "?"), path + 3);
    const char *sig = strstr(path, "?sig=");
    pthread_mutex_lock(&g_smu);
    int probe = ranged && a == 0 && b == 15;
    if (ranged && !probe) {
        g_ranged++;
        if (g_expire_after && g_ranged >= g_expire_after) {
            g_expire_after = 0;
            g_sig_valid = ++g_sig; /* every address handed out so far stops working */
        }
    }
    int expired = sig && atoi(sig + 5) < g_sig_valid;
    int throttled = ranged && !probe && !expired && g_throttle > 0;
    if (throttled)
        g_throttle--, g_429s++;
    if (expired)
        g_403s++;
    pthread_mutex_unlock(&g_smu);
    if (expired) {
        head(fd, 403, NULL, 0);
        return 0;
    }
    if (throttled) {
        head(fd, 429, NULL, 0);
        return 0;
    }
    sfile_t *f = find_file(name);
    if (!f) {
        head(fd, 404, NULL, 0);
        return 0;
    }
    if (f->html) {
        static const char page[] = "<!doctype html><html><body>Click here to download</body></html>";
        head(fd, 200, "Content-Type: text/html; charset=utf-8\r\n", sizeof(page) - 1);
        send_all(fd, page, sizeof(page) - 1, 0);
        return 0;
    }
    char disp[256] = "";
    if (f->disp[0])
        snprintf(disp, sizeof(disp), "Content-Disposition: attachment; filename=\"%s\"\r\n", f->disp);
    if (!ranged || f->no_ranges) {
        snprintf(extra, sizeof(extra), "Content-Type: application/octet-stream\r\n%s", disp);
        head(fd, 200, extra, f->size);
        return send_all(fd, f->data, f->size, 1) ? -1 : 0;
    }
    if (a >= f->size) {
        snprintf(extra, sizeof(extra), "Content-Range: bytes */%zu\r\n", f->size);
        head(fd, 416, extra, 0);
        return 0;
    }
    if (b >= f->size)
        b = f->size - 1;
    size_t len = (size_t)(b - a + 1);
    snprintf(extra, sizeof(extra), "Content-Range: bytes %llu-%llu/%zu\r\nETag: \"v1\"\r\nContent-Type: application/octet-stream\r\n%s", a,
             b, f->size, disp);
    head(fd, 206, extra, len);
    pthread_mutex_lock(&g_smu);
    int cut = g_cut_once && !probe && len > 2;
    if (cut)
        g_cut_once = 0, g_cuts++;
    pthread_mutex_unlock(&g_smu);
    if (cut) {
        send_all(fd, f->data + a, len / 2, 1);
        return -1;
    }
    return send_all(fd, f->data + a, len, 1) ? -1 : 0;
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    char req[8192];
    size_t have = 0;
    for (;;) {
        char *end;
        while (!(end = strstr(req, "\r\n\r\n"))) {
            if (have + 1 >= sizeof(req))
                goto out;
            ssize_t r = recv(fd, req + have, sizeof(req) - 1 - have, 0);
            if (r <= 0)
                goto out;
            have += (size_t)r;
            req[have] = 0;
        }
        size_t used = (size_t)(end + 4 - req);
        char one[8192];
        memcpy(one, req, used);
        one[used] = 0;
        memmove(req, req + used, have - used + 1);
        have -= used;
        if (serve(fd, one))
            break;
    }
out:
    close(fd);
    return NULL;
}

static void *accept_thread(void *arg) {
    (void)arg;
    for (;;) {
        int fd = accept(g_listen, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &at, conn_thread, (void *)(intptr_t)fd);
        pthread_attr_destroy(&at);
    }
    return NULL;
}

static void start_server(void) {
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(g_listen, (struct sockaddr *)&sa, sizeof(sa));
    socklen_t sl = sizeof(sa);
    getsockname(g_listen, (struct sockaddr *)&sa, &sl);
    g_port = ntohs(sa.sin_port);
    listen(g_listen, 64);
    pthread_t t;
    pthread_create(&t, NULL, accept_thread, NULL);
}

/* --- PS4Mount's side --- */

static char g_scan[512], g_state[512];
static uint64_t g_free = UINT64_MAX / 4;
static int g_arrived, g_notes;
static char g_arrived_dir[1024], g_last_note[600];
static pthread_mutex_t g_hmu = PTHREAD_MUTEX_INITIALIZER;

static int h_dest(const char *dest, char *dir, size_t n) {
    if (strcmp(dest, "usb0"))
        return -1;
    snprintf(dir, n, "%s", g_scan);
    mkdir(dir, 0777);
    return 0;
}

static uint64_t h_free(const char *dir) {
    (void)dir;
    return g_free;
}

static int h_check(const char *path, uint64_t size, char *problem, size_t n) {
    pkg_check_t c;
    if (pkg_check(path, 0, &c)) {
        snprintf(problem, n, "not a PS4 pkg");
        return -1;
    }
    if (c.problem[0]) {
        snprintf(problem, n, "%s", c.problem);
        return -1;
    }
    if (size && size != c.pkg_size) {
        snprintf(problem, n, "incomplete");
        return -1;
    }
    return 0;
}

static void h_arrived(const char *dir) {
    pthread_mutex_lock(&g_hmu);
    g_arrived++;
    snprintf(g_arrived_dir, sizeof(g_arrived_dir), "%s", dir);
    pthread_mutex_unlock(&g_hmu);
}

static void h_notify(const char *m) {
    pthread_mutex_lock(&g_hmu);
    g_notes++;
    snprintf(g_last_note, sizeof(g_last_note), "%s", m);
    pthread_mutex_unlock(&g_hmu);
    printf("    [notify] %s\n", m);
}

static const links_host_t k_host = {h_dest, h_free, h_check, h_arrived, h_notify};

/* --- helpers --- */

static char g_status[65536];

static void status(void) {
    buf_t b = {0};
    links_status_json(&b);
    snprintf(g_status, sizeof(g_status), "%.*s", (int)b.len, (const char *)b.data);
    buf_free(&b);
}

static int njobs(void) {
    jv_t v = jv_of(g_status, strlen(g_status)), jobs, j;
    int n = 0;
    if (!jv_get(v, "jobs", &jobs))
        while (!jv_index(jobs, n, &j))
            n++;
    return n;
}

static void job_field(int i, const char *key, char *out, size_t n) {
    jv_t v = jv_of(g_status, strlen(g_status)), jobs, j;
    out[0] = 0;
    if (!jv_get(v, "jobs", &jobs) && !jv_index(jobs, i, &j))
        jv_get_string(j, key, out, n);
}

static int busy(void) {
    jv_t v = jv_of(g_status, strlen(g_status)), job, b;
    return !jv_get(v, "job", &job) && !jv_get(job, "busy", &b) && b.n == 4 && !memcmp(b.p, "true", 4);
}

static void result(char *out, size_t n) { jv_get_string(jv_of(g_status, strlen(g_status)), "result", out, n); }

/* Waits until nothing runs or waits (a failed download stays in the list). */
static int wait_idle(int seconds) {
    for (int i = 0; i < seconds * 20; i++) {
        status();
        int waiting = 0;
        for (int k = 0; k < njobs(); k++) {
            char st[16];
            job_field(k, "state", st, sizeof(st));
            waiting |= strcmp(st, "failed") != 0;
        }
        if (!busy() && !waiting)
            return 0;
        usleep(50000);
    }
    printf("  still busy: %s\n", g_status);
    return -1;
}

static long long job_num(const char *key) {
    status();
    jv_t v = jv_of(g_status, strlen(g_status)), job;
    return jv_get(v, "job", &job) ? -1 : jv_get_int(job, key, -1);
}

static int same_as(const char *path, const char *expected) {
    size_t a, b;
    uint8_t *x = slurp(path, &a), *y = slurp(expected, &b);
    int same = x && y && a == b && !memcmp(x, y, a);
    if (!same)
        printf("  %s differs from %s (%zu / %zu bytes)\n", path, expected, x ? a : 0, y ? b : 0);
    free(x);
    free(y);
    return same;
}

static int exists(const char *p) {
    struct stat st;
    return !stat(p, &st);
}

static char g_pkgs[512];

static int add(const char *text, char *msg, size_t n) { return links_add(text, "usb0", NULL, msg, n); }

/* --- a helper like the browser extension: takes blocks, fetches them, hands them over --- */

static int g_helper_stop, g_helper_vanish = -1, g_helper_drop_every; /* set before it runs, but stop */
static int g_helper_put, g_helper_dropped;

static int relay_post(const char *path, const void *body, size_t len, char *out, size_t n) {
    char url[256], err[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", links_relay_port(), path);
    http_req_t q = {0};
    q.method = "POST";
    q.url = url;
    q.headers = "X-PS4Mount: 1\r\n";
    q.body = body;
    q.body_len = len;
    q.stall = 10;
    http_resp_t r;
    if (http_request(&q, &r, err, sizeof(err)))
        return -1;
    snprintf(out, n, "%.*s", (int)r.body.len, r.body.data ? (const char *)r.body.data : "");
    int st = r.status;
    http_resp_free(&r);
    return st;
}

static void *helper(void *arg) {
    (void)arg;
    int taken = 0;
    while (!__atomic_load_n(&g_helper_stop, __ATOMIC_RELAXED)) {
        char j[4096], u[2048], q[128], range[96], err[256], ack[256];
        if (relay_post("/claim", NULL, 0, j, sizeof(j)) != 200 || strstr(j, "\"idle\"") || strstr(j, "\"wait\"")) {
            usleep(20000);
            continue;
        }
        jv_t v = jv_of(j, strlen(j));
        long long from = jv_get_int(v, "from", -1), to = jv_get_int(v, "to", -1);
        jv_get_string(v, "url", u, sizeof(u));
        snprintf(q, sizeof(q), "?job=%lld&vol=%lld&block=%lld", jv_get_int(v, "job", -1), jv_get_int(v, "vol", -1),
                 jv_get_int(v, "block", -1));
        if (++taken == g_helper_vanish)
            break; /* gone with a block taken */
        char path[160];
        if (g_helper_drop_every && taken % g_helper_drop_every == 0) {
            snprintf(path, sizeof(path), "/drop%s", q);
            relay_post(path, NULL, 0, ack, sizeof(ack));
            g_helper_dropped++;
            continue;
        }
        snprintf(range, sizeof(range), "Range: bytes=%lld-%lld\r\n", from, to - 1);
        http_req_t g = {0};
        g.method = "GET";
        g.url = u;
        g.headers = range;
        g.stall = 10;
        http_resp_t r;
        if (http_request(&g, &r, err, sizeof(err)))
            continue;
        snprintf(path, sizeof(path), "/put%s", q);
        if (r.status == 206 && (long long)r.body.len == to - from && relay_post(path, r.body.data, r.body.len, ack, sizeof(ack)) == 200)
            g_helper_put++;
        http_resp_free(&r);
    }
    return NULL;
}

static void helper_run(pthread_t *t, int vanish, int drop_every) {
    g_helper_stop = 0;
    g_helper_vanish = vanish;
    g_helper_drop_every = drop_every;
    g_helper_put = g_helper_dropped = 0;
    pthread_create(t, NULL, helper, NULL);
}

static void helper_end(pthread_t t) {
    __atomic_store_n(&g_helper_stop, 1, __ATOMIC_RELAXED);
    pthread_join(t, NULL);
}


static void url(char *out, size_t n, const char *kind, const char *name) {
    snprintf(out, n, "http://127.0.0.1:%d/%s/%s", g_port, kind, name);
}

static void reset_scan(void) {
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", g_scan, g_scan);
    (void)!system(cmd);
    g_arrived = 0;
    g_arrived_dir[0] = 0;
}

static int has_game(const char *folder, int update) {
    char p[1200], e[1200];
    snprintf(p, sizeof(p), "%s/%s/game.pkg", g_scan, folder);
    snprintf(e, sizeof(e), "%s/game.pkg", g_pkgs);
    int ok = same_as(p, e);
    if (update) {
        snprintf(p, sizeof(p), "%s/%s/update.pkg", g_scan, folder);
        snprintf(e, sizeof(e), "%s/update.pkg", g_pkgs);
        ok &= same_as(p, e);
    }
    snprintf(p, sizeof(p), "%s/%s/readme.txt", g_scan, folder);
    return ok && !exists(p);
}

static int work_gone(void) {
    char p[1200];
    snprintf(p, sizeof(p), "%s/.ps4mount-links", g_scan);
    return !exists(p);
}

static int g_nvol;

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--make-pkgs")) {
        make_pkgs(argv[2]);
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    char base[] = "/tmp/links_test.XXXXXX";
    if (!mkdtemp(base))
        return 1;
    snprintf(g_scan, sizeof(g_scan), "%s/scan", base);
    snprintf(g_state, sizeof(g_state), "%s/state", base);
    snprintf(g_pkgs, sizeof(g_pkgs), "%s/pkgs", base);
    mkdir(g_state, 0777);
    mkdir(g_pkgs, 0777);
    make_pkgs(g_pkgs);

    /* the files the server has */
    char p[1200];
    DIR *d = opendir(DATA);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strstr(e->d_name, ".sh") || strstr(e->d_name, ".txt"))
            continue;
        snprintf(p, sizeof(p), DATA "/%s", e->d_name);
        add_path(e->d_name, p);
        if (!strncmp(e->d_name, "game.part", 9)) {
            g_nvol++;
            /* the same volumes with the older naming: old.rar, old.r00, old.r01... */
            char old[64];
            int k = atoi(e->d_name + 9);
            if (k == 1)
                snprintf(old, sizeof(old), "old.rar");
            else
                snprintf(old, sizeof(old), "old.r%02d", k - 2);
            add_path(old, p);
            /* and with a host's code before each name, a different one for each part */
            snprintf(old, sizeof(old), "c%dX%d-dl-Coded.part%03d.rar", k * 7, k, k);
            add_path(old, p);
        }
    }
    if (d)
        closedir(d);
    printf("  %d files served, game.rar in %d volumes\n", g_nsf, g_nvol);
    snprintf(p, sizeof(p), "%s/game.pkg", g_pkgs);
    add_path("Direct%20Game.pkg", p);
    add_path("getfile", p);
    snprintf(find_file("getfile")->disp, sizeof(find_file("getfile")->disp), "Cool Game.pkg");
    add_path("plain.pkg", p);
    find_file("plain.pkg")->no_ranges = 1;
    add_path("page.pkg", p);
    find_file("page.pkg")->html = 1;
    snprintf(p, sizeof(p), "%s/real.pkg", g_pkgs);
    make_pkg(p, GAME_CID, "gd", 0x09008000, 0, 0x8000, 0x8000); /* not fake */
    add_path("real.pkg", p);
    start_server();

    g_links_block = 8192;
    g_links_threads = 4;
    g_links_retry_seconds = 0;
    g_links_relay_port = 0;
    g_links_relay_claim = 1;
    g_links_relay_gone = 1;
    reset_scan();
    links_start(g_state, &k_host);

    char text[8192], msg[512], u[600], res[700];

    printf("== links that are refused\n");
    CHECK(add("", msg, sizeof(msg)) && strstr(msg, "paste"));
    CHECK(add("ftp://example.com/game.pkg", msg, sizeof(msg)) && strstr(msg, "http"));
    CHECK(links_add("http://example.com/a.pkg", "../x", NULL, msg, sizeof(msg)) && strstr(msg, "where"));
    CHECK(add("http://example.com/a.part1.rar\nhttp://example.com/a.part3.rar", msg, sizeof(msg)) && strstr(msg, "part 2 is missing"));
    CHECK(add("http://example.com/a.part2.rar\nhttp://example.com/a.part1.rar\nhttp://example.com/a.part2.rar", msg, sizeof(msg)) &&
          strstr(msg, "part 2 is there twice"));
    CHECK(add("http://example.com/b.r00\nhttp://example.com/b.r01", msg, sizeof(msg)) && strstr(msg, "first volume"));
    status();
    CHECK(njobs() == 0);

    printf("== a RAR in %d volumes, pasted out of order, through redirects\n", g_nvol);
    text[0] = 0;
    for (int k = g_nvol; k >= 1; k--) {
        char name[64];
        snprintf(name, sizeof(name), "game.part%03d.rar", k);
        url(u, sizeof(u), k == 2 ? "rr" : "r", name);
        size_t l = strlen(text);
        snprintf(text + l, sizeof(text) - l, "  %s  \r\n", u);
    }
    CHECK(!add(text, msg, sizeof(msg)) && strstr(msg, "1 download"));
    status();
    char name[256];
    job_field(0, "name", name, sizeof(name));
    CHECK(!strcmp(name, "game"));
    CHECK(add(text, msg, sizeof(msg)) && strstr(msg, "already"));
    CHECK(!wait_idle(60));
    result(res, sizeof(res));
    CHECK(strstr(res, "game was downloaded: 2 pkg(s)") != NULL);
    CHECK(has_game("game", 1));
    CHECK(work_gone());
    CHECK(g_arrived == 1 && strstr(g_arrived_dir, "/scan/game"));
    CHECK(strstr(g_last_note, "was downloaded") != NULL);
    snprintf(p, sizeof(p), "%s/links_queue.txt", g_state);
    CHECK(!exists(p));


    printf("== a helper on the network fetches the blocks (the browser extension)\n");
    {
        pthread_t ht;
        CHECK(links_relay_port() > 0);
        status();
        CHECK(jv_get_int(jv_of(g_status, strlen(g_status)), "relay_port", 0) == links_relay_port());
        /* it brings blocks alongside the console's own connection (one here, so it gets some) */
        g_links_threads = 1;
        reset_scan();
        helper_run(&ht, -1, 0);
        text[0] = 0;
        for (int k = 1; k <= g_nvol; k++) {
            char nm[64];
            snprintf(nm, sizeof(nm), "game.part%03d.rar", k);
            url(u, sizeof(u), "f", nm);
            snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
        }
        usleep(100000); /* heard from before the download starts */
        CHECK(!add(text, msg, sizeof(msg)));
        CHECK(!wait_idle(60));
        helper_end(ht);
        result(res, sizeof(res));
        CHECK(strstr(res, "game was downloaded: 2 pkg(s)") != NULL);
        CHECK(has_game("game", 1));
        printf("  the helper brought %d blocks\n", g_helper_put);
        CHECK(g_helper_put >= 3);
        CHECK(work_gone());
        /* it gives some back: the console fetches those */
        reset_scan();
        helper_run(&ht, -1, 2);
        usleep(100000);
        CHECK(!add(text, msg, sizeof(msg)));
        CHECK(!wait_idle(60));
        helper_end(ht);
        result(res, sizeof(res));
        CHECK(strstr(res, "game was downloaded: 2 pkg(s)") != NULL && g_helper_dropped > 0);
        CHECK(has_game("game", 1));
        CHECK(work_gone());
        /* it goes away holding a block: the console takes it again and goes on alone */
        reset_scan();
        helper_run(&ht, 4, 0);
        usleep(100000);
        CHECK(!add(text, msg, sizeof(msg)));
        CHECK(!wait_idle(60));
        helper_end(ht);
        result(res, sizeof(res));
        CHECK(strstr(res, "game was downloaded: 2 pkg(s)") != NULL);
        CHECK(has_game("game", 1));
        CHECK(work_gone());
        /* a request without the panel's header is refused */
        char url2[128], err[200];
        snprintf(url2, sizeof(url2), "http://127.0.0.1:%d/claim", links_relay_port());
        http_req_t q = {0};
        q.method = "POST";
        q.url = url2;
        http_resp_t r;
        CHECK(!http_request(&q, &r, err, sizeof(err)) && r.status == 403);
        http_resp_free(&r);
        g_links_threads = 4;
    }

    printf("== the same volumes named old.rar, old.r00...\n");
    reset_scan();
    text[0] = 0;
    for (int k = 0; k < g_nvol; k++) {
        char nm[64];
        if (k)
            snprintf(nm, sizeof(nm), "old.r%02d", k - 1);
        else
            snprintf(nm, sizeof(nm), "old.rar");
        url(u, sizeof(u), "f", nm);
        size_t l = strlen(text);
        snprintf(text + l, sizeof(text) - l, "%s\n", u);
    }
    CHECK(!add(text, msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    CHECK(has_game("old", 1));

    printf("== the same volumes with a host's code before each name\n");
    reset_scan();
    text[0] = 0;
    for (int k = 1; k <= g_nvol; k++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "c%dX%d-dl-Coded.part%03d.rar", k * 7, k, k);
        url(u, sizeof(u), "f", nm);
        size_t l = strlen(text);
        snprintf(text + l, sizeof(text) - l, "%s\n", u);
    }
    CHECK(!add(text, msg, sizeof(msg)) && strstr(msg, "1 download"));
    status();
    job_field(0, "name", name, sizeof(name));
    CHECK(!strcmp(name, "dl-Coded"));
    CHECK(!wait_idle(60));
    CHECK(has_game("dl-Coded", 1));
    CHECK(add("http://example.com/c1-a.part1.rar\nhttp://example.com/c2-a.part3.rar", msg, sizeof(msg)) &&
          strstr(msg, "is missing"));

    printf("== a ZIP, a pkg, a pkg named by the server, a server without ranges\n");
    reset_scan();
    url(u, sizeof(u), "f", "bundle.zip");
    snprintf(text, sizeof(text), "%s\n", u);
    url(u, sizeof(u), "f", "Direct%20Game.pkg");
    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    url(u, sizeof(u), "f", "getfile");
    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    url(u, sizeof(u), "f", "plain.pkg");
    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s | My Plain Game.pkg\n", u);
    CHECK(!add(text, msg, sizeof(msg)) && strstr(msg, "4 download"));
    CHECK(!wait_idle(60));
    CHECK(has_game("bundle", 1));
    snprintf(p, sizeof(p), "%s/Direct Game/Direct Game.pkg", g_scan);
    snprintf(u, sizeof(u), "%s/game.pkg", g_pkgs);
    CHECK(same_as(p, u));
    snprintf(p, sizeof(p), "%s/Cool Game/Cool Game.pkg", g_scan);
    CHECK(same_as(p, u));
    snprintf(p, sizeof(p), "%s/My Plain Game/My Plain Game.pkg", g_scan);
    CHECK(same_as(p, u));
    CHECK(g_arrived == 4);
    CHECK(work_gone());

    printf("== what fails, and stays in the list\n");
    reset_scan();
    url(u, sizeof(u), "f", "real.pkg");
    snprintf(text, sizeof(text), "%s\n", u);
    url(u, sizeof(u), "f", "page.pkg");
    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    url(u, sizeof(u), "f", "locked.rar");
    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    url(u, sizeof(u), "f", "missing.pkg");
    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    text[strlen(text)] = 0;
    for (int k = 1; k <= 3; k++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "rar4.part%d.rar", k);
        url(u, sizeof(u), "f", nm);
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    }
    CHECK(!add(text, msg, sizeof(msg)) && strstr(msg, "5 download"));
    CHECK(!wait_idle(60));
    CHECK(njobs() == 5);
    char err[600], st[16];
    for (int k = 0; k < njobs(); k++) {
        job_field(k, "name", name, sizeof(name));
        job_field(k, "error", err, sizeof(err));
        job_field(k, "state", st, sizeof(st));
        printf("  %s: %s\n", name, err);
        CHECK(!strcmp(st, "failed"));
        if (!strcmp(name, "real"))
            CHECK(strstr(err, "real.pkg: ") && strstr(err, "fake"));
        else if (!strcmp(name, "page"))
            CHECK(strstr(err, "web page") != NULL);
        else if (!strcmp(name, "locked"))
            CHECK(strstr(err, "password") != NULL);
        else if (!strcmp(name, "missing"))
            CHECK(strstr(err, "not there any more (404)") != NULL);
        else if (!strcmp(name, "rar4"))
            CHECK(strstr(err, "no pkg inside") != NULL);
        else
            CHECK(!"an unexpected download");
    }
    CHECK(g_arrived == 0);
    /* once more: it fails the same way; then it goes, with what it left */
    jv_t v = jv_of(g_status, strlen(g_status)), jobs, j0;
    jv_get(v, "jobs", &jobs);
    jv_index(jobs, 0, &j0);
    int id0 = (int)jv_get_int(j0, "id", 0);
    CHECK(!links_retry(id0, NULL, msg, sizeof(msg)));
    CHECK(links_retry(id0 + 1000, NULL, msg, sizeof(msg)) && strstr(msg, "not in the list"));
    CHECK(!wait_idle(60));
    job_field(0, "error", err, sizeof(err));
    CHECK(strstr(err, "fake") != NULL);
    snprintf(p, sizeof(p), "%s/.ps4mount-links/%d", g_scan, id0);
    CHECK(exists(p));
    while (njobs()) {
        jv_get(jv_of(g_status, strlen(g_status)), "jobs", &jobs);
        jv_index(jobs, 0, &j0);
        CHECK(!links_cancel((int)jv_get_int(j0, "id", 0), msg, sizeof(msg)) && strstr(msg, "removed"));
        status();
    }
    CHECK(work_gone());

    printf("== archives with a password\n");
    reset_scan();
    snprintf(p, sizeof(p), "%s/passwords.txt", g_state);
    unlink(p);
    /* none known: it stops at once, and says to give it */
    url(u, sizeof(u), "f", "locked.rar");
    CHECK(!add(u, msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    job_field(0, "error", err, sizeof(err));
    CHECK(strstr(err, "has a password") && strstr(err, "Try again"));
    CHECK(strstr(g_status, "\"locked\":true") != NULL);
    jv_get(jv_of(g_status, strlen(g_status)), "jobs", &jobs);
    jv_index(jobs, 0, &j0);
    id0 = (int)jv_get_int(j0, "id", 0);
    /* a wrong one, then the right one: it opens, and is kept for the next archives */
    CHECK(!links_retry(id0, "nope", msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    job_field(0, "error", err, sizeof(err));
    CHECK(strstr(err, "does not open") != NULL);
    CHECK(!links_retry(id0, "ps4mount", msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    CHECK(njobs() == 0);
    result(res, sizeof(res));
    CHECK(strstr(res, "locked was downloaded: 1 pkg(s)") != NULL);
    snprintf(p, sizeof(p), "%s/locked/update.pkg", g_scan);
    CHECK(exists(p));
    snprintf(p, sizeof(p), "%s/passwords.txt", g_state);
    {
        size_t pl;
        char *kept = (char *)slurp(p, &pl);
        CHECK(kept && pl == 9 && !memcmp(kept, "ps4mount\n", 9));
        free(kept);
    }
    CHECK(work_gone());
    /* headers encrypted too, in volumes: the kept password opens it by itself */
    reset_scan();
    text[0] = 0;
    for (int k = 1; k <= 6; k++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "secret.part%03d.rar", k);
        url(u, sizeof(u), "f", nm);
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    }
    CHECK(!add(text, msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    result(res, sizeof(res));
    CHECK(strstr(res, "secret was downloaded: 2 pkg(s)") != NULL);
    CHECK(has_game("secret", 1));
    CHECK(work_gone());
    /* the site in brackets at the start of the name ([SITE]-game) is tried too */
    reset_scan();
    unlink(p);
    url(u, sizeof(u), "f", "guess.rar");
    snprintf(text, sizeof(text), "%s | [GUESS.SITE]-guess.rar\n", u);
    CHECK(!add(text, msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    result(res, sizeof(res));
    CHECK(strstr(res, "was downloaded: 1 pkg(s)") != NULL);
    unlink(p);
    CHECK(work_gone());
    /* a service header whose password check is stray bytes (seen in the wild): not a check */
    reset_scan();
    url(u, sizeof(u), "f", "stray.rar");
    CHECK(!links_add(u, "usb0", "guess.site", msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    result(res, sizeof(res));
    CHECK(strstr(res, "stray was downloaded: 1 pkg(s)") != NULL);
    unlink(p);
    CHECK(work_gone());

    printf("== a cut, a server that limits connections, an address that expires\n");
    reset_scan();
    g_cut_once = 1;
    g_throttle = 3;
    g_expire_after = 4;
    text[0] = 0;
    for (int k = 1; k <= g_nvol; k++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "game.part%03d.rar", k);
        url(u, sizeof(u), "e", nm);
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    }
    int resolves = g_resolves;
    CHECK(!add(text, msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    CHECK(has_game("game", 1));
    CHECK(g_cuts == 1 && g_429s == 3 && g_403s >= 1);
    CHECK(g_resolves > resolves + g_nvol); /* the expired address was asked for again */

    printf("== PS4Mount stops midway, and goes on at its next start\n");
    reset_scan();
    g_slow_us = 40000;
    text[0] = 0;
    for (int k = 1; k <= g_nvol; k++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "game.part%03d.rar", k);
        url(u, sizeof(u), "f", nm);
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s\n", u);
    }
    CHECK(!add(text, msg, sizeof(msg)));
    long long total = 0, done = 0;
    for (int i = 0; i < 400 && (done = job_num("done")) * 3 < (total = job_num("total")) * 1 + 1; i++)
        usleep(25000);
    printf("  stopping at %lld of %lld bytes\n", done, total);
    CHECK(done > 0 && done < total);
    links_stop();
    snprintf(p, sizeof(p), "%s/links_queue.txt", g_state);
    CHECK(exists(p));
    g_slow_us = 0;
    int notes = g_notes;
    links_start(g_state, &k_host);
    CHECK(g_notes == notes + 1 && strstr(g_last_note, "Going on with 1 download"));
    CHECK(!wait_idle(60));
    CHECK(has_game("game", 1));
    CHECK(work_gone());

    printf("== little space: each volume goes once read\n");
    reset_scan();
    uint64_t sum = 0, largest = 0;
    for (int k = 1; k <= g_nvol; k++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "game.part%03d.rar", k);
        sum += find_file(nm)->size;
        if (find_file(nm)->size > largest)
            largest = find_file(nm)->size;
    }
    g_free = sum + 2 * largest + (64ull << 20) + 4096;
    CHECK(!add(text, msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    CHECK(has_game("game", 1));
    printf("== not enough space\n");
    reset_scan();
    g_free = 1 << 20;
    CHECK(!add(text, msg, sizeof(msg)));
    CHECK(!wait_idle(60));
    job_field(0, "error", err, sizeof(err));
    CHECK(strstr(err, "not enough space") != NULL);
    jv_get(jv_of(g_status, strlen(g_status)), "jobs", &jobs);
    jv_index(jobs, 0, &j0);
    CHECK(!links_cancel((int)jv_get_int(j0, "id", 0), msg, sizeof(msg)));
    g_free = UINT64_MAX / 4;

    printf("== a download cancelled while it runs\n");
    reset_scan();
    g_slow_us = 40000;
    CHECK(!add(text, msg, sizeof(msg)));
    for (int i = 0; i < 400 && job_num("done") <= 0; i++)
        usleep(25000);
    status();
    jv_get(jv_of(g_status, strlen(g_status)), "jobs", &jobs);
    jv_index(jobs, 0, &j0);
    CHECK(!links_cancel((int)jv_get_int(j0, "id", 0), msg, sizeof(msg)) && strstr(msg, "will stop"));
    g_slow_us = 0;
    CHECK(!wait_idle(60));
    CHECK(njobs() == 0);
    result(res, sizeof(res));
    CHECK(strstr(res, "cancelled") != NULL);
    CHECK(work_gone());
    CHECK(!links_active());

    links_stop();
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    (void)!system(cmd);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
