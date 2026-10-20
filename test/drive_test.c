/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Google Drive backups (src/drive.c) against a local stand-in for Google's servers, over
 * plain HTTP: signing in with a code, uploads in chunks (also cut midway), skipping what
 * Drive has, downloads that resume, a damaged download, the trash, a removed access and
 * disconnecting. Run with: bash test/run.sh */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "bearssl.h"
#include "drive.h"
#include "json.h"
#include "lang.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

/* --- the stand-in for Google --- */

#define MAXF 64
typedef struct {
    char id[16], name[128], parent[16], props[512];
    int folder, trashed;
    uint8_t *data;
    size_t size, cap, want; /* want: the size an upload session announced */
    int done;
} gfile_t;

static gfile_t g_f[MAXF];
static int g_nf;
static int g_port, g_listen;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
/* faults, each used once */
static int g_cut_upload_after;   /* close the connection at the PUT that would pass this byte */
static int g_cut_download_at;    /* send this many bytes of a download, then close */
static int g_damage_download;    /* report a wrong MD5 for downloads */
static int g_refresh_revoked;    /* the refresh token is no longer valid */
static int g_token_polls, g_revoked_calls, g_refreshes, g_ranges, g_secret_seen;

static void md5_of(const uint8_t *d, size_t n, char out[33]) {
    br_md5_context c;
    uint8_t h[16];
    br_md5_init(&c);
    br_md5_update(&c, d, n);
    br_md5_out(&c, h);
    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", h[i]);
}

static void respond(int fd, int status, const char *extra, const char *type, const void *body, size_t len) {
    char head[1024];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d X\r\nContent-Length: %zu\r\nContent-Type: %s\r\n%s\r\n", status, len,
                     type, extra ? extra : "");
    if (write(fd, head, (size_t)n) < 0)
        return;
    for (size_t o = 0; o < len;) {
        ssize_t w = write(fd, (const char *)body + o, len - o);
        if (w <= 0)
            return;
        o += (size_t)w;
    }
}

static void json(int fd, int status, const char *body) { respond(fd, status, NULL, "application/json", body, strlen(body)); }

/* Chunked, as Google answers lists. */
static void json_chunked(int fd, const char *body) {
    char head[256];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n");
    (void)!write(fd, head, (size_t)n);
    size_t len = strlen(body);
    for (size_t o = 0; o < len; o += 100) {
        size_t l = len - o < 100 ? len - o : 100;
        n = snprintf(head, sizeof(head), "%zx\r\n", l);
        (void)!write(fd, head, (size_t)n);
        (void)!write(fd, body + o, l);
        (void)!write(fd, "\r\n", 2);
    }
    (void)!write(fd, "0\r\n\r\n", 5);
}

static gfile_t *by_id(const char *id) {
    for (int i = 0; i < g_nf; i++)
        if (!strcmp(g_f[i].id, id))
            return &g_f[i];
    return NULL;
}

static gfile_t *new_file(void) {
    gfile_t *f = &g_f[g_nf];
    memset(f, 0, sizeof(*f));
    snprintf(f->id, sizeof(f->id), "id%d", g_nf);
    g_nf++;
    return f;
}

/* appProperties of a metadata body, kept as "k=v;" pairs. */
static void take_props(jv_t meta, gfile_t *f) {
    jv_t ap;
    f->props[0] = 0;
    if (jv_get(meta, "appProperties", &ap))
        return;
    static const char *keys[] = {"ps4mount", "tid", "kind", "title", "ps4mount_game"};
    for (int i = 0; i < 5; i++) {
        char v[200];
        if (!jv_get_string(ap, keys[i], v, sizeof(v))) {
            size_t l = strlen(f->props);
            snprintf(f->props + l, sizeof(f->props) - l, "%s=%s;", keys[i], v);
        }
    }
}

static int prop(const gfile_t *f, const char *k, char *v, size_t n) {
    char key[64];
    snprintf(key, sizeof(key), "%s=", k);
    const char *p = strstr(f->props, key);
    if (p && (p == f->props || p[-1] == ';')) {
        p += strlen(key);
        size_t l = strcspn(p, ";");
        snprintf(v, n, "%.*s", (int)l, p);
        return 0;
    }
    return -1;
}

static void url_decode(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && *in != '&' && o + 1 < n; in++) {
        if (*in == '%' && in[1] && in[2]) {
            char h[3] = {in[1], in[2], 0};
            out[o++] = (char)strtol(h, NULL, 16);
            in += 2;
        } else
            out[o++] = *in == '+' ? ' ' : *in;
    }
    out[o] = 0;
}

static void param(const char *path, const char *name, char *out, size_t n) {
    char key[32];
    snprintf(key, sizeof(key), "%s=", name);
    const char *p = strstr(path, key);
    out[0] = 0;
    if (p && (p[-1] == '?' || p[-1] == '&'))
        url_decode(p + strlen(key), out, n);
}

static void file_json(char *out, size_t n, const gfile_t *f) {
    char md5[33], t[16] = "", k[16] = "", title[200] = "";
    md5_of(f->data, f->size, md5);
    if (g_damage_download)
        md5[0] = md5[0] == '0' ? '1' : '0';
    prop(f, "tid", t, sizeof(t));
    prop(f, "kind", k, sizeof(k));
    prop(f, "title", title, sizeof(title));
    snprintf(out, n,
             "{\"id\":\"%s\",\"name\":\"%s\",\"size\":\"%zu\",\"md5Checksum\":\"%s\","
             "\"appProperties\":{\"ps4mount\":\"pkg\",\"tid\":\"%s\",\"kind\":\"%s\",\"title\":\"%s\"}}",
             f->id, f->name, f->size, md5, t, k, title);
}

static void serve(int fd) {
    static char req[1 << 16];
    size_t got = 0;
    char *end = NULL;
    while (!end && got < sizeof(req) - 1) {
        ssize_t r = read(fd, req + got, sizeof(req) - 1 - got);
        if (r <= 0)
            return;
        got += (size_t)r;
        req[got] = 0;
        end = strstr(req, "\r\n\r\n");
    }
    if (!end)
        return;
    *end = 0;
    char method[16], path[4096];
    sscanf(req, "%15s %4095s", method, path);
    const char *cl = strcasestr(req, "Content-Length:");
    size_t len = cl ? strtoul(cl + 15, NULL, 10) : 0, have = got - (size_t)(end + 4 - req);
    uint8_t *body = malloc(len + 1);
    memcpy(body, end + 4, have < len ? have : len);
    while (have < len) {
        ssize_t r = read(fd, body + have, len - have);
        if (r <= 0) {
            free(body);
            return;
        }
        have += (size_t)r;
    }
    body[len] = 0;
    static char headers[1 << 16];
    snprintf(headers, sizeof(headers), "%s", req);
    const char *auth = strcasestr(headers, "Authorization: Bearer ");
    int authed = auth && !strncmp(auth + 22, "access-", 7);
    jv_t meta = jv_of((char *)body, len);
    char tmp[1 << 15];

    pthread_mutex_lock(&g_smu);
    if (!strcmp(path, "/oauth/device/code")) {
        json(fd, 200, "{\"device_code\":\"dev-1\",\"user_code\":\"ABCD-EFGH\",\"verification_url\":"
                      "\"https://www.google.com/device\",\"expires_in\":1800,\"interval\":1}");
    } else if (!strcmp(path, "/oauth/token")) {
        g_secret_seen |= strstr((char *)body, "client_secret=GOCSPX-secret_1") != NULL &&
                         strstr((char *)body, "client_id=123-abc.apps.googleusercontent.com") != NULL;
        if (strstr((char *)body, "grant_type=urn")) {
            if (++g_token_polls < 2)
                json(fd, 428, "{\"error\":\"authorization_pending\"}");
            else
                json(fd, 200, "{\"access_token\":\"access-1\",\"refresh_token\":\"refresh-1\",\"expires_in\":3599}");
        } else if (g_refresh_revoked) {
            json(fd, 400, "{\"error\":\"invalid_grant\",\"error_description\":\"Token has been expired or revoked.\"}");
        } else {
            g_refreshes++;
            json(fd, 200, "{\"access_token\":\"access-2\",\"expires_in\":3599}");
        }
    } else if (!strcmp(path, "/oauth/revoke")) {
        g_revoked_calls++;
        json(fd, 200, "{}");
    } else if (!authed && strncmp(path, "/upload/session/", 16)) {
        json(fd, 401, "{\"error\":{\"code\":401,\"message\":\"Invalid Credentials\"}}");
    } else if (!strncmp(path, "/drive/about", 12)) {
        json(fd, 200, "{\"user\":{\"emailAddress\":\"someone@example.com\"},\"storageQuota\":{\"limit\":\"16106127360\","
                      "\"usage\":\"1024\"}}");
    } else if (!strncmp(path, "/drive/files?", 13) && !strcmp(method, "GET")) {
        char q[1024], key[64] = "", value[64] = "";
        param(path, "q", q, sizeof(q));
        const char *k = strstr(q, "key='"), *v = strstr(q, "value='");
        if (k)
            sscanf(k + 5, "%63[^']", key);
        if (v)
            sscanf(v + 7, "%63[^']", value);
        int want_folder = strstr(q, "mimeType") != NULL;
        size_t o = (size_t)snprintf(tmp, sizeof(tmp), "{\"files\":[");
        int first = 1;
        for (int i = 0; i < g_nf; i++) {
            gfile_t *f = &g_f[i];
            char pv[200];
            if (f->trashed || !f->done || (want_folder && !f->folder) || prop(f, key, pv, sizeof(pv)) || strcmp(pv, value))
                continue;
            char one[1024];
            file_json(one, sizeof(one), f);
            o += (size_t)snprintf(tmp + o, sizeof(tmp) - o, "%s%s", first ? "" : ",", one);
            first = 0;
        }
        snprintf(tmp + o, sizeof(tmp) - o, "]}");
        json_chunked(fd, tmp);
    } else if (!strncmp(path, "/drive/files?", 13) && !strcmp(method, "POST")) {
        gfile_t *f = new_file();
        jv_get_string(meta, "name", f->name, sizeof(f->name));
        jv_t ps, p0;
        if (!jv_get(meta, "parents", &ps) && !jv_index(ps, 0, &p0))
            jv_string(p0, f->parent, sizeof(f->parent));
        f->folder = 1;
        f->done = 1;
        take_props(meta, f);
        snprintf(tmp, sizeof(tmp), "{\"id\":\"%s\"}", f->id);
        json(fd, 200, tmp);
    } else if (!strncmp(path, "/drive/files/", 13) && !strcmp(method, "PATCH")) {
        char id[16];
        sscanf(path + 13, "%15[^?]", id);
        gfile_t *f = by_id(id);
        if (f)
            f->trashed = 1;
        json(fd, f ? 200 : 404, "{}");
    } else if (!strncmp(path, "/drive/files/", 13) && strstr(path, "alt=media")) {
        char id[16];
        sscanf(path + 13, "%15[^?]", id);
        gfile_t *f = by_id(id);
        const char *rg = strcasestr(headers, "Range: bytes=");
        size_t from = rg ? strtoul(rg + 13, NULL, 10) : 0, last = 0;
        const char *dash = rg ? strchr(rg + 13, '-') : NULL;
        if (dash && dash[1] >= '0' && dash[1] <= '9')
            last = strtoul(dash + 1, NULL, 10);
        if (!f || f->trashed) {
            json(fd, 404, "{\"error\":{\"code\":404,\"message\":\"File not found\"}}");
        } else {
            if (!last || last >= f->size)
                last = f->size - 1;
            size_t n = last + 1 - from;
            char extra[128];
            snprintf(extra, sizeof(extra), "Content-Range: bytes %zu-%zu/%zu\r\n", from, last, f->size);
            g_ranges++;
            if (g_cut_download_at && (size_t)g_cut_download_at > from && (size_t)g_cut_download_at <= last) {
                /* the announced length, then less: a connection that drops */
                char head[256];
                int hn = snprintf(head, sizeof(head), "HTTP/1.1 206 X\r\nContent-Length: %zu\r\n%s\r\n", n, extra);
                (void)!write(fd, head, (size_t)hn);
                (void)!write(fd, f->data + from, (size_t)g_cut_download_at - from);
                g_cut_download_at = 0;
            } else
                respond(fd, rg ? 206 : 200, extra, "application/octet-stream", f->data + from, n);
        }
    } else if (!strncmp(path, "/upload/files?", 14) && strstr(path, "uploadType=resumable")) {
        gfile_t *f = new_file();
        jv_get_string(meta, "name", f->name, sizeof(f->name));
        jv_t ps, p0;
        if (!jv_get(meta, "parents", &ps) && !jv_index(ps, 0, &p0))
            jv_string(p0, f->parent, sizeof(f->parent));
        take_props(meta, f);
        const char *xl = strcasestr(headers, "X-Upload-Content-Length:");
        f->want = xl ? strtoul(xl + 24, NULL, 10) : 0;
        f->cap = f->want ? f->want : 1;
        f->data = malloc(f->cap);
        char extra[256];
        snprintf(extra, sizeof(extra), "Location: http://127.0.0.1:%d/upload/session/%s\r\n", g_port, f->id);
        respond(fd, 200, extra, "application/json", "", 0);
    } else if (!strncmp(path, "/upload/session/", 16) && !strcmp(method, "PUT")) {
        gfile_t *f = by_id(path + 16);
        const char *cr = strcasestr(headers, "Content-Range: bytes ");
        size_t a = 0, b = 0, total = 0;
        int query = cr && cr[21] == '*';
        if (cr && !query)
            sscanf(cr + 21, "%zu-%zu/%zu", &a, &b, &total);
        if (!f) {
            json(fd, 404, "{}");
        } else if (!query && g_cut_upload_after && b + 1 > (size_t)g_cut_upload_after) {
            g_cut_upload_after = 0; /* the chunk is lost: nothing answers */
        } else {
            if (!query && a == f->size && len == b - a + 1) {
                memcpy(f->data + a, body, len);
                f->size += len;
            }
            if (f->size == f->want) {
                f->done = 1;
                char one[1024];
                file_json(one, sizeof(one), f);
                json(fd, 200, one);
            } else {
                char extra[64] = "";
                if (f->size)
                    snprintf(extra, sizeof(extra), "Range: bytes=0-%zu\r\n", f->size - 1);
                respond(fd, 308, extra, "text/plain", "", 0);
            }
        }
    } else {
        json(fd, 404, "{\"error\":{\"code\":404,\"message\":\"no such path\"}}");
    }
    pthread_mutex_unlock(&g_smu);
    free(body);
}

static void *server(void *arg) {
    (void)arg;
    for (;;) {
        int fd = accept(g_listen, NULL, NULL);
        if (fd < 0)
            return NULL;
        serve(fd);
        close(fd);
    }
}

/* --- what PS4Mount gives the Drive thread --- */

static char g_root[64], g_lib[256], g_dest[256];
static int g_in_library = 1, g_downloaded_n;
static char g_notified[1024];

static int t_game_files(const char *tid, drive_file_t *files, int max, char *name, size_t nn) {
    static const char *names[] = {"game.pkg", "update.pkg", "addon-CUSA00001-DLC1.pkg"};
    static const char *kinds[] = {"game", "update", "addon"};
    static const char *local[] = {"app.pkg", "patch.pkg", "ac.pkg"};
    if (!g_in_library || strcmp(tid, "CUSA00001"))
        return 0;
    int n = 0;
    for (int i = 0; i < 3 && n < max; i++) {
        snprintf(files[n].path, sizeof(files[n].path), "%s/%s", g_lib, local[i]);
        snprintf(files[n].name, sizeof(files[n].name), "%s", names[i]);
        snprintf(files[n].kind, sizeof(files[n].kind), "%s", kinds[i]);
        struct stat st;
        stat(files[n].path, &st);
        files[n].size = (uint64_t)st.st_size;
        n++;
    }
    snprintf(name, nn, "Test: Game");
    return n;
}

static int t_library(char (*tids)[10], int max) {
    if (!g_in_library || max < 1)
        return 0;
    snprintf(tids[0], 10, "CUSA00001");
    return 1;
}

static int t_dest_dir(const char *dest, char *dir, size_t n) {
    if (strcmp(dest, "usb0"))
        return -1;
    snprintf(dir, n, "%s", g_dest);
    return 0;
}

static uint64_t t_free_bytes(const char *dir) {
    (void)dir;
    return 1ull << 40;
}

static void t_downloaded(const char *tid) {
    (void)tid;
    g_downloaded_n++;
}

static char g_notes[8192]; /* every notification, one a line */
static void t_notify(const char *msg) {
    snprintf(g_notified, sizeof(g_notified), "%s", msg);
    size_t l = strlen(g_notes);
    snprintf(g_notes + l, sizeof(g_notes) - l, "%s\n", msg);
}

/* --- helpers --- */

static buf_t status(void) {
    buf_t b = {0};
    drive_status_json(&b);
    buf_put(&b, "", 1);
    return b;
}

/* Waits until the thread is idle with nothing queued (at most 20 s). */
static void wait_idle(void) {
    for (int i = 0; i < 400; i++) {
        usleep(50000);
        buf_t b = status();
        jv_t v = jv_of((char *)b.data, b.len - 1), job, q, x;
        int busy = !jv_get(v, "job", &job) && !jv_get(job, "busy", &x) && x.n == 4;
        int queued = !jv_get(job, "queued", &q) && !jv_index(q, 0, &x);
        int login = 0;
        jv_t l;
        if (!jv_get(v, "login", &l) && !jv_get(l, "active", &x))
            login = x.n == 4;
        buf_free(&b);
        if (!busy && !queued && !login)
            return;
    }
    printf("  (timed out waiting for the Drive thread)\n");
}

static void result(char *out, size_t n, int *ok) {
    buf_t b = status();
    jv_t v = jv_of((char *)b.data, b.len - 1), job, x;
    jv_get(v, "job", &job);
    jv_get_string(job, "result", out, n);
    *ok = !jv_get(job, "ok", &x) && x.n == 4;
    buf_free(&b);
}

static void write_bytes(const char *path, size_t n, int seed) {
    FILE *f = fopen(path, "wb");
    for (size_t i = 0; i < n; i++)
        fputc((int)((i * 31 + (size_t)seed * 7 + i / 977) & 0xff), f);
    fclose(f);
}

static int same(const char *path, const gfile_t *g) {
    FILE *f = fopen(path, "rb");
    if (!f || !g)
        return 0;
    uint8_t *d = malloc(g->size + 1);
    size_t r = fread(d, 1, g->size + 1, f);
    fclose(f);
    int eq = r == g->size && !memcmp(d, g->data, g->size);
    free(d);
    return eq;
}

static gfile_t *remote(const char *name) {
    for (int i = 0; i < g_nf; i++)
        if (!g_f[i].folder && !g_f[i].trashed && g_f[i].done && !strcmp(g_f[i].name, name))
            return &g_f[i];
    return NULL;
}

static int count_remote(void) {
    int n = 0;
    for (int i = 0; i < g_nf; i++)
        n += !g_f[i].folder && !g_f[i].trashed && g_f[i].done;
    return n;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    g_lang = LANG_EN;
    g_drive_chunk = 256u << 10;
    g_drive_block = 256u << 10;
    g_drive_retry_seconds = 0;
    char tmpl[] = "/tmp/drive_test.XXXXXX", state[300], p[512];
    snprintf(g_root, sizeof(g_root), "%s", mkdtemp(tmpl));
    snprintf(state, sizeof(state), "%s/state", g_root);
    snprintf(g_lib, sizeof(g_lib), "%s/lib", g_root);
    snprintf(g_dest, sizeof(g_dest), "%s/usb0/PS4Mount", g_root);
    mkdir(state, 0777);
    mkdir(g_lib, 0777);
    snprintf(p, sizeof(p), "%s/usb0", g_root);
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/app.pkg", g_lib);
    write_bytes(p, (1300u << 10) + 17, 1); /* five chunks and a bit */
    snprintf(p, sizeof(p), "%s/patch.pkg", g_lib);
    write_bytes(p, 256u << 10, 2); /* exactly one chunk */
    snprintf(p, sizeof(p), "%s/ac.pkg", g_lib);
    write_bytes(p, 1000, 3);

    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(g_listen, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(g_listen, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    listen(g_listen, 8);
    pthread_t th;
    pthread_create(&th, NULL, server, NULL);

    char oauth[64], api[64], up[64], msg[512], res[512];
    int ok;
    snprintf(oauth, sizeof(oauth), "http://127.0.0.1:%d/oauth", g_port);
    snprintf(api, sizeof(api), "http://127.0.0.1:%d/drive", g_port);
    snprintf(up, sizeof(up), "http://127.0.0.1:%d/upload", g_port);
    drive_endpoints_t ep = {oauth, api, up};
    drive_host_t host = {t_game_files, t_library, t_dest_dir, t_free_bytes, t_downloaded, t_notify};

    printf("== no client yet: the user brings their own\n");
    drive_start(state, &ep, &host);
    buf_t b = status();
    CHECK(strstr((char *)b.data, "\"configured\":true,\"client\":false"));
    buf_free(&b);
    CHECK(drive_connect(msg, sizeof(msg)) == -1 && strstr(msg, "client ID"));
    CHECK(drive_set_client("not-an-id", "s3cret", msg, sizeof(msg)) == -1);
    CHECK(drive_set_client("123-abc.apps.googleusercontent.com", "bad secret!", msg, sizeof(msg)) == -1);
    CHECK(drive_set_client("123-abc.apps.googleusercontent.com", "GOCSPX-secret_1", msg, sizeof(msg)) == 0);
    b = status();
    CHECK(strstr((char *)b.data, "\"client\":true,\"client_id\":\"123-abc.apps.googleusercontent.com\""));
    CHECK(!strstr((char *)b.data, "GOCSPX")); /* the secret never goes back to the panel */
    buf_free(&b);
    snprintf(p, sizeof(p), "%s/drive_client.txt", state);
    struct stat cst;
    CHECK(!stat(p, &cst) && (cst.st_mode & 077) == 0);
    drive_stop(); /* read back from the console's file on the next start */
    g_secret_seen = 0;

    printf("== signing in with a code\n");
    drive_start(state, &ep, &host);
    CHECK(drive_upload("CUSA00001", msg, sizeof(msg)) == -1); /* not connected yet */
    CHECK(strstr(msg, "Connect Google Drive first"));
    CHECK(drive_connect(msg, sizeof(msg)) == 0);
    int saw_code = 0;
    for (int i = 0; i < 100 && !saw_code; i++) {
        usleep(20000);
        b = status();
        saw_code = strstr((char *)b.data, "\"code\":\"ABCD-EFGH\"") && strstr((char *)b.data, "google.com/device");
        buf_free(&b);
    }
    CHECK(saw_code);
    wait_idle();
    b = status();
    CHECK(strstr((char *)b.data, "\"connected\":true"));
    CHECK(strstr((char *)b.data, "\"email\":\"someone@example.com\""));
    CHECK(strstr((char *)b.data, "\"quota_limit\":16106127360"));
    CHECK(strstr((char *)b.data, "\"listed\":true"));
    buf_free(&b);
    CHECK(g_token_polls >= 2); /* pending first, then the tokens */
    CHECK(g_secret_seen);      /* with the user's client, from the console's file */
    snprintf(p, sizeof(p), "%s/drive.txt", state);
    FILE *sf = fopen(p, "r");
    char line[256] = "";
    CHECK(sf && fgets(line, sizeof(line), sf) && !strcmp(line, "refresh=refresh-1\n"));
    if (sf)
        fclose(sf);

    printf("== uploading a game in chunks\n");
    g_notes[0] = 0;
    CHECK(drive_upload("CUSA00001", msg, sizeof(msg)) == 0);
    int saw_progress = 0;
    for (int i = 0; i < 400 && !saw_progress; i++) {
        usleep(5000);
        b = status();
        saw_progress = strstr((char *)b.data, "\"phase\":\"sending\"") && strstr((char *)b.data, "\"game_total\":1594361");
        buf_free(&b);
    }
    CHECK(saw_progress); /* the whole game (1331217 + 262144 + 1000), not one file */
    wait_idle();
    result(res, sizeof(res), &ok);
    CHECK(ok && strstr(res, "Test: Game is in Google Drive (2 MB in "));
    /* on the console's screen: when it starts and when it ends */
    CHECK(strstr(g_notes, "Uploading Test: Game to Google Drive (2 MB)\n"));
    CHECK(strstr(g_notes, "Test: Game is in Google Drive (2 MB in "));
    snprintf(p, sizeof(p), "%s/app.pkg", g_lib);
    CHECK(same(p, remote("game.pkg")));
    snprintf(p, sizeof(p), "%s/patch.pkg", g_lib);
    CHECK(same(p, remote("update.pkg")));
    snprintf(p, sizeof(p), "%s/ac.pkg", g_lib);
    CHECK(same(p, remote("addon-CUSA00001-DLC1.pkg")));
    CHECK(count_remote() == 3);
    int folders = 0;
    for (int i = 0; i < g_nf; i++)
        if (g_f[i].folder) {
            folders++;
            if (strstr(g_f[i].props, "ps4mount_game"))
                CHECK(!strcmp(g_f[i].name, "Test Game [CUSA00001]")); /* no ':' in a folder name */
        }
    CHECK(folders == 2); /* PS4Mount/ and the game's */
    snprintf(p, sizeof(p), "%s/drive_upload.txt", state);
    CHECK(access(p, F_OK) != 0); /* the session is gone once done */
    b = status();
    CHECK(strstr((char *)b.data, "\"games\":[{\"title_id\":\"CUSA00001\",\"name\":\"Test: Game\""));
    CHECK(strstr((char *)b.data, "\"files\":3,\"update\":true,\"addons\":1"));
    buf_free(&b);

    printf("== the whole library again: what Drive has is skipped\n");
    int before = g_nf;
    CHECK(drive_upload(NULL, msg, sizeof(msg)) == 0);
    CHECK(strstr(msg, "1 game(s)"));
    wait_idle();
    result(res, sizeof(res), &ok);
    CHECK(ok && strstr(res, "was already in Google Drive"));
    CHECK(g_nf == before);

    printf("== an upload cut midway goes on from what Drive has\n");
    snprintf(p, sizeof(p), "%s/patch.pkg", g_lib);
    write_bytes(p, (600u << 10) + 5, 4); /* a new update: another size */
    g_cut_upload_after = (300u << 10);
    CHECK(drive_upload("CUSA00001", msg, sizeof(msg)) == 0);
    wait_idle();
    result(res, sizeof(res), &ok);
    CHECK(ok);
    CHECK(same(p, remote("update.pkg")));
    CHECK(count_remote() == 3); /* the old update went to the trash */
    CHECK(g_cut_upload_after == 0);

    printf("== downloading a game into a scan folder\n");
    g_in_library = 0;
    g_cut_download_at = 100000; /* the first download drops after 100000 bytes */
    CHECK(drive_download("CUSA00001", "usb1", msg, sizeof(msg)) == -1);
    CHECK(drive_download("CUSA00001", "usb0", msg, sizeof(msg)) == 0);
    wait_idle();
    result(res, sizeof(res), &ok);
    CHECK(ok && strstr(res, "was downloaded from Google Drive"));
    CHECK(g_downloaded_n == 1);
    CHECK(g_cut_download_at == 0);
    CHECK(g_ranges >= 6 + 3 + 1 + 1); /* in blocks: 6 for the game, 3 the update, 1 the add-on, 1 again */
    snprintf(p, sizeof(p), "%s/Test Game [CUSA00001]/game.pkg", g_dest);
    CHECK(same(p, remote("game.pkg")));
    snprintf(p, sizeof(p), "%s/Test Game [CUSA00001]/update.pkg", g_dest);
    CHECK(same(p, remote("update.pkg")));
    snprintf(p, sizeof(p), "%s/Test Game [CUSA00001]/addon-CUSA00001-DLC1.pkg", g_dest);
    CHECK(same(p, remote("addon-CUSA00001-DLC1.pkg")));
    snprintf(p, sizeof(p), "%s/.ps4mount-drive", g_dest);
    CHECK(access(p, F_OK) != 0); /* no part files left */
    CHECK(strstr(g_notified, "downloaded"));

    printf("== a download that arrives damaged is not kept\n");
    g_damage_download = 1;
    snprintf(p, sizeof(p), "%s/other", g_root);
    mkdir(p, 0777);
    snprintf(g_dest, sizeof(g_dest), "%s/other", g_root);
    CHECK(drive_download("CUSA00001", "usb0", msg, sizeof(msg)) == 0);
    wait_idle();
    result(res, sizeof(res), &ok);
    CHECK(!ok && strstr(res, "arrived damaged"));
    snprintf(p, sizeof(p), "%s/Test Game [CUSA00001]/game.pkg", g_dest);
    CHECK(access(p, F_OK) != 0);
    g_damage_download = 0;

    printf("== a game already in the library is not downloaded over itself\n");
    g_in_library = 1;
    CHECK(drive_download("CUSA00001", "usb0", msg, sizeof(msg)) == 0);
    wait_idle();
    result(res, sizeof(res), &ok);
    CHECK(!ok && strstr(res, "already in the library"));

    printf("== moving a game to the Drive trash\n");
    CHECK(drive_trash("CUSA00001", msg, sizeof(msg)) == 0);
    wait_idle();
    result(res, sizeof(res), &ok);
    CHECK(ok && strstr(res, "trash"));
    CHECK(count_remote() == 0);
    b = status();
    CHECK(strstr((char *)b.data, "\"games\":[]"));
    buf_free(&b);

    printf("== an access that ends asks for a new code on the screen, and the job goes on\n");
    g_in_library = 1;
    g_notified[0] = 0;
    pthread_mutex_lock(&g_smu);
    g_refresh_revoked = 1;
    pthread_mutex_unlock(&g_smu);
    drive_stop();
    drive_start(state, &ep, &host); /* a new start: the access token is asked for again */
    CHECK(drive_upload("CUSA00001", msg, sizeof(msg)) == 0);
    int saw = 0;
    for (int i = 0; i < 200 && !saw; i++) {
        usleep(20000);
        saw = strstr(g_notified, "google.com/device") && strstr(g_notified, "ABCD-EFGH");
    }
    CHECK(saw); /* the code on the console's screen */
    wait_idle();
    result(res, sizeof(res), &ok);
    CHECK(ok && strstr(res, "is in Google Drive")); /* after signing in, the cut upload ran */
    b = status();
    CHECK(strstr((char *)b.data, "\"connected\":true"));
    buf_free(&b);
    CHECK(count_remote() == 3);
    pthread_mutex_lock(&g_smu);
    g_refresh_revoked = 0;
    pthread_mutex_unlock(&g_smu);

    printf("== disconnecting revokes the access\n");
    CHECK(drive_disconnect(msg, sizeof(msg)) == 0);
    wait_idle();
    usleep(200000);
    CHECK(g_revoked_calls == 1);
    b = status();
    CHECK(strstr((char *)b.data, "\"connected\":false"));
    buf_free(&b);
    snprintf(p, sizeof(p), "%s/drive.txt", state);
    CHECK(access(p, F_OK) != 0);

    printf("== forgetting the client\n");
    CHECK(drive_forget_client(msg, sizeof(msg)) == 0);
    snprintf(p, sizeof(p), "%s/drive_client.txt", state);
    CHECK(access(p, F_OK) != 0);
    b = status();
    CHECK(strstr((char *)b.data, "\"client\":false"));
    buf_free(&b);

    drive_stop();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
