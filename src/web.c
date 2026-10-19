/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Minimal HTTP server for the PS4Mount panel. */
/* Minimal HTTP server for the PS4Mount panel (one request per connection). */
#include "web.h"

#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "page_data.h"

#define LOG_PATH "/data/ps4mount/log.txt"
#define CHECK_MAX (16u << 20) /* most a pkg check is given */

int web_listen(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 8)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    return s;
}

static void send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w <= 0)
            return;
        c += w;
        n -= (size_t)w;
    }
}

static void reply(int fd, const char *status, const char *type, const void *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, type, len);
    send_all(fd, head, (size_t)n);
    send_all(fd, body, len);
}

static void json_msg(int fd, int ok, const char *msg) {
    buf_t b = {0};
    buf_printf(&b, "{\"ok\":%s,\"%s\":\"", ok ? "true" : "false", ok ? "message" : "error");
    for (const char *p = msg; *p; p++) {
        if (*p == '"' || *p == '\\')
            buf_put(&b, "\\", 1);
        if ((unsigned char)*p >= 0x20)
            buf_put(&b, p, 1);
    }
    buf_printf(&b, "\"}");
    reply(fd, ok ? "200 OK" : "400 Bad Request", "application/json; charset=utf-8", b.data, b.len);
    buf_free(&b);
}

/* A title id (CUSA00000) at p, copied to tid. 0 when valid. */
static int title_at(const char *p, char *tid) {
    for (int i = 0; i < 9; i++) {
        char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return -1;
        tid[i] = c;
    }
    tid[9] = 0;
    return strncmp(tid, "CUSA", 4) ? -1 : 0;
}

static int title_param(const char *path, char *tid) {
    const char *p = strstr(path, "title_id=");
    return p ? title_at(p + 9, tid) : -1;
}

static void send_log(int fd) {
    buf_t b = {0}, j = {0};
    int f = open(LOG_PATH, O_RDONLY);
    if (f >= 0) {
        off_t end = lseek(f, 0, SEEK_END), start = end > 16384 ? end - 16384 : 0;
        lseek(f, start, SEEK_SET);
        char tmp[4096];
        ssize_t n;
        while ((n = read(f, tmp, sizeof(tmp))) > 0)
            buf_put(&b, tmp, (size_t)n);
        close(f);
    }
    buf_printf(&j, "{\"log\":\"");
    for (size_t i = 0; i < b.len; i++) {
        char c = (char)b.data[i];
        if (c == '\n')
            buf_put(&j, "\\n", 2);
        else if (c == '"' || c == '\\') {
            buf_put(&j, "\\", 1);
            buf_put(&j, &c, 1);
        } else if ((unsigned char)c >= 0x20)
            buf_put(&j, &c, 1);
    }
    buf_printf(&j, "\"}");
    reply(fd, "200 OK", "application/json; charset=utf-8", j.data, j.len);
    buf_free(&b);
    buf_free(&j);
}

/* Requests that write files: from the local network only, and with the panel's own
 * header, which a page of another site cannot send without the panel agreeing (CORS
 * preflight): no site the browser opens can push files to the console. */
static int may_write(int fd, const char *req) {
    const char *h = req;
    int header = 0;
    while ((h = strchr(h, '\n'))) {
        h++;
        if (!strncasecmp(h, "X-PS4Mount:", 11)) {
            header = 1;
            break;
        }
    }
    if (!header)
        return 0;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) || ss.ss_family != AF_INET)
        return 1; /* a local socket (the host tests) */
    uint32_t a = ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
    return (a >> 24) == 10 || (a >> 24) == 127 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8 || (a >> 16) == 0xa9fe;
}

/* The body after what was read with the headers, up to len bytes. NULL on a short read. */
static uint8_t *read_body(int fd, const char *pre, size_t pre_len, size_t len) {
    uint8_t *b = malloc(len ? len : 1);
    if (!b)
        return NULL;
    size_t have = pre_len < len ? pre_len : len;
    memcpy(b, pre, have);
    while (have < len) {
        ssize_t m = read(fd, b + have, len - have);
        if (m <= 0) {
            free(b);
            return NULL;
        }
        have += (size_t)m;
    }
    return b;
}

void web_serve(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd >= 0)
        web_handle(fd);
}

void web_handle(int fd) {
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[8192];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) {
        close(fd);
        return;
    }
    req[n] = 0;
    /* the body of a POST may arrive after the headers */
    char *body = strstr(req, "\r\n\r\n");
    const char *cl = strstr(req, "Content-Length:");
    if (!cl)
        cl = strstr(req, "content-length:");
    unsigned long long length = cl ? strtoull(cl + 15, NULL, 10) : 0;
    long want = length > sizeof(req) ? (long)sizeof(req) : (long)length;
    while (want > 0 && (size_t)n < sizeof(req) - 1 && (!body || (long)(n - (body + 4 - req)) < want)) {
        ssize_t m = read(fd, req + n, sizeof(req) - 1 - (size_t)n);
        if (m <= 0)
            break;
        n += m;
        req[n] = 0;
        body = strstr(req, "\r\n\r\n");
    }
    body = body ? body + 4 : req + n;
    char method[8] = "", path[1024] = "", tid[10], msg[256];
    sscanf(req, "%7s %1023s", method, path);
    int post = !strcmp(method, "POST");

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        reply(fd, "200 OK", "text/html; charset=utf-8", index_html, sizeof(index_html));
    } else if (!strcmp(path, "/legacy") || !strcmp(path, "/legacy.html")) {
        reply(fd, "200 OK", "text/html; charset=utf-8", legacy_html, sizeof(legacy_html));
    } else if (!strcmp(path, "/api/games")) {
        buf_t b = {0};
        app_games_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (!strncmp(path, "/icon/", 6)) {
        buf_t b = {0};
        /* the id becomes part of file paths: nothing but a title id */
        if (strlen(path + 6) == 9 && !title_at(path + 6, tid) && !app_icon(tid, &b))
            reply(fd, "200 OK", "image/png", b.data, b.len);
        else
            reply(fd, "404 Not Found", "text/plain", "no icon\n", 8);
        buf_free(&b);
    } else if (post && !strncmp(path, "/api/mount", 10)) {
        if (title_param(path, tid))
            json_msg(fd, 0, "invalid title_id");
        else {
            int rc = app_add(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/unmount", 12)) {
        if (title_param(path, tid))
            json_msg(fd, 0, "invalid title_id");
        else {
            int rc = app_remove(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strcmp(path, "/api/rescan")) {
        app_rescan();
        json_msg(fd, 1, "Scanning USB drives for games");
    } else if (post && !strncmp(path, "/api/restore", 12) && (path[12] == 0 || path[12] == '?')) {
        /* ?title_id=<one game>, body title_ids=<games, comma separated>, or neither for all */
        int rc;
        const char *list = strstr(body, "title_ids=");
        if (strstr(path, "title_id="))
            rc = title_param(path, tid) ? (snprintf(msg, sizeof(msg), "invalid title_id"), -1)
                                        : app_restore(tid, msg, sizeof(msg));
        else if (list) {
            char tids[4096];
            size_t o = 0;
            for (const char *c = list + 10; *c && *c != '&' && o + 1 < sizeof(tids); c++) {
                if (!strncmp(c, "%2C", 3) || !strncmp(c, "%2c", 3)) {
                    tids[o++] = ',';
                    c += 2;
                } else
                    tids[o++] = *c;
            }
            tids[o] = 0;
            rc = app_restore(tids, msg, sizeof(msg));
        } else
            rc = app_restore(NULL, msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (post && !strcmp(path, "/api/restored/forget")) {
        int rc = app_restored_forget(msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (!strcmp(path, "/api/config") && !post) {
        buf_t b = {0};
        app_config_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (post && !strcmp(path, "/api/config")) {
        int rc = app_config_set(body, msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (post && !strncmp(path, "/api/check", 10) && (path[10] == 0 || path[10] == '?')) {
        const char *sz = strstr(path, "size=");
        uint8_t *data = NULL;
        if (!may_write(fd, req))
            reply(fd, "403 Forbidden", "text/plain", "forbidden\n", 10);
        else if (!length || length > CHECK_MAX)
            json_msg(fd, 0, "send the first MB of the pkg (at most 16 MB)");
        else if (!(data = read_body(fd, body, (size_t)(req + n - body), (size_t)length)))
            json_msg(fd, 0, "the pkg start did not arrive whole");
        else {
            buf_t b = {0};
            app_check(data, (size_t)length, sz ? strtoull(sz + 5, NULL, 10) : 0, &b);
            reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
            buf_free(&b);
        }
        free(data);
    } else if (post && !strncmp(path, "/api/upload", 11) && (path[11] == 0 || path[11] == '?')) {
        if (!may_write(fd, req))
            reply(fd, "403 Forbidden", "text/plain", "forbidden\n", 10);
        else if (!length)
            json_msg(fd, 0, "nothing was sent");
        else if (!app_upload(fd, path, body, (size_t)(req + n - body), length, msg, sizeof(msg)))
            return; /* the upload answers and closes it */
        else
            json_msg(fd, 0, msg);
    } else if (!strcmp(path, "/api/uploads")) {
        buf_t b = {0};
        app_uploads_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (!strcmp(path, "/api/log")) {
        send_log(fd);
    } else if (post && !strcmp(path, "/api/stop")) {
        json_msg(fd, 1, "PS4Mount is stopping");
        app_stop();
    } else {
        reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
    }
    close(fd);
}
