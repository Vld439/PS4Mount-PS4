/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pkgs sent from the panel: received in a thread of their own, checked as soon as their
 * first bytes are in, and renamed into the scan folder only when complete. */
#include "upload.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "check.h"
#include "lang.h"
#include "mem.h"

#define PART_NAME ".ps4mount-upload.part"
#define CHUNK (1u << 20)

enum { ST_IDLE, ST_RECEIVING, ST_DONE, ST_FAILED };

typedef struct {
    int fd;
    char part[PATH_MAX], final[PATH_MAX], name[256];
    uint64_t length;
    uint8_t *pre;
    size_t pre_len;
    uint32_t fw;
    void (*done)(const char *path);
} job_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_state = ST_IDLE;
static uint64_t g_length, g_received;
static char g_name[256], g_title[128], g_message[512];

static void set_state(int state, const char *message) {
    pthread_mutex_lock(&g_mu);
    g_state = state;
    snprintf(g_message, sizeof(g_message), "%s", message ? message : "");
    pthread_mutex_unlock(&g_mu);
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

void upload_status_json(buf_t *out) {
    static const char *names[] = {"idle", "receiving", "done", "failed"};
    pthread_mutex_lock(&g_mu);
    buf_printf(out, "{\"state\":\"%s\",\"name\":", names[g_state]);
    json_esc(out, g_name);
    buf_printf(out, ",\"title\":");
    json_esc(out, g_title);
    buf_printf(out, ",\"size\":%llu,\"received\":%llu,\"message\":", (unsigned long long)g_length,
               (unsigned long long)g_received);
    json_esc(out, g_message);
    buf_printf(out, "}");
    pthread_mutex_unlock(&g_mu);
}

int upload_clean_name(const char *name, char *out, size_t n) {
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    size_t o = 0;
    for (const char *p = base; *p && o + 5 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            continue;
        out[o++] = (char)c;
    }
    out[o] = 0;
    while (o && (out[0] == '.' || out[0] == ' ')) /* no hidden files, no "..": a name, not a path */
        memmove(out, out + 1, o--);
    if (!o)
        return -1;
    if (o < 4 || strcasecmp(out + o - 4, ".pkg"))
        snprintf(out + o, n - o, ".pkg");
    return 0;
}

static void answer(int fd, int ok, const char *msg) {
    buf_t b = {0};
    buf_printf(&b, "{\"ok\":%s,\"%s\":", ok ? "true" : "false", ok ? "message" : "error");
    json_esc(&b, msg);
    buf_printf(&b, "}");
    char head[200];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     ok ? "200 OK" : "400 Bad Request", b.len);
    if (write(fd, head, (size_t)n) > 0)
        (void)!write(fd, b.data, b.len);
    buf_free(&b);
}

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* The checks on the start of the pkg (and again on the whole file): msg says why not. */
static int check_part(const job_t *j, char *msg, size_t n) {
    pkg_check_t c;
    if (pkg_check(j->part, j->fw, &c)) {
        snprintf(msg, n, TR("%s is not a PS4 pkg", "%s no es un pkg de PS4"), j->name);
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    snprintf(g_title, sizeof(g_title), "%s", c.title);
    pthread_mutex_unlock(&g_mu);
    if (!c.ok) {
        snprintf(msg, n, "%s: %s", j->name, c.problem);
        return -1;
    }
    if (c.pkg_size != j->length) {
        snprintf(msg, n,
                 TR("%s is %llu bytes but its header says %llu: the file is incomplete or damaged",
                    "%s tiene %llu bytes pero su cabecera dice %llu: el archivo está incompleto o dañado"),
                 j->name,
                 (unsigned long long)j->length, (unsigned long long)c.pkg_size);
        return -1;
    }
    return 0;
}

/* A name in dir not taken yet: "Game.pkg", then "Game (1).pkg"... */
static void free_name(const char *dir, const char *name, char *out, size_t n) {
    struct stat st;
    snprintf(out, n, "%.3800s/%.250s", dir, name);
    size_t l = strlen(name) - 4;
    for (int i = 1; !stat(out, &st) && i < 1000; i++)
        snprintf(out, n, "%.3800s/%.*s (%d).pkg", dir, (int)(l > 240 ? 240 : l), name, i);
}

static void *receive(void *arg) {
    job_t *j = arg;
    char msg[512] = "";
    int out = open(j->part, O_WRONLY | O_CREAT | O_TRUNC, 0666), checked = 0, rc = 0;
    uint64_t got = 0;
    uint8_t *buf = mem_alloc(CHUNK);
    if (out < 0 || !buf) {
        snprintf(msg, sizeof(msg), TR("cannot write %.300s: %s", "no se puede escribir %.300s: %s"), j->part, strerror(errno));
        rc = -1;
    }
    if (!rc && j->pre_len) {
        size_t take = j->pre_len < j->length ? j->pre_len : (size_t)j->length;
        rc = write_all(out, j->pre, take);
        got = take;
    }
    while (!rc && got < j->length) {
        size_t want = j->length - got < CHUNK ? (size_t)(j->length - got) : CHUNK;
        ssize_t r = read(j->fd, buf, want);
        if (r <= 0) {
            snprintf(msg, sizeof(msg),
                     TR("%s: the transfer stopped after %llu of %llu bytes", "%s: la transferencia se cortó tras %llu de %llu bytes"),
                     j->name,
                     (unsigned long long)got, (unsigned long long)j->length);
            rc = -1;
            break;
        }
        if (write_all(out, buf, (size_t)r)) {
            snprintf(msg, sizeof(msg), TR("%s: write failed (%s): is the drive full?", "%s: falló la escritura (%s): ¿está llena la unidad?"),
                     j->name, strerror(errno));
            rc = -1;
            break;
        }
        got += (uint64_t)r;
        pthread_mutex_lock(&g_mu);
        g_received = got;
        pthread_mutex_unlock(&g_mu);
        if (!checked && got >= UPLOAD_CHECK_BYTES) {
            checked = 1;
            rc = check_part(j, msg, sizeof(msg)); /* stops a doomed transfer early */
        }
    }
    mem_free(buf);
    if (out >= 0 && close(out) && !rc) {
        snprintf(msg, sizeof(msg), TR("%s: write failed (%s)", "%s: falló la escritura (%s)"), j->name, strerror(errno));
        rc = -1;
    }
    if (!rc)
        rc = check_part(j, msg, sizeof(msg));
    if (!rc) {
        char final[PATH_MAX];
        free_name(j->final, j->name, final, sizeof(final)); /* j->final holds the folder until now */
        snprintf(j->final, sizeof(j->final), "%s", final);
        if (rename(j->part, j->final)) {
            snprintf(msg, sizeof(msg), TR("%s: rename failed (%s)", "%s: no se pudo renombrar (%s)"), j->name, strerror(errno));
            rc = -1;
        }
    }
    if (rc) {
        unlink(j->part);
        printf("[upload] %s\n", msg);
        set_state(ST_FAILED, msg);
        answer(j->fd, 0, msg);
    } else {
        snprintf(msg, sizeof(msg), TR("%s received (%llu bytes)", "%s recibido (%llu bytes)"), j->name, (unsigned long long)got);
        printf("[upload] %s -> %s\n", msg, j->final);
        set_state(ST_DONE, msg);
        answer(j->fd, 1, msg);
        if (j->done)
            j->done(j->final);
    }
    close(j->fd);
    mem_free(j->pre);
    free(j);
    return NULL;
}

int upload_start(int fd, const char *dir, const char *name, uint64_t length, const void *pre, size_t pre_len,
                 uint32_t fw, void (*done)(const char *path)) {
    pthread_mutex_lock(&g_mu);
    int busy = g_state == ST_RECEIVING;
    if (!busy) {
        g_state = ST_RECEIVING;
        g_length = length;
        g_received = 0;
        snprintf(g_name, sizeof(g_name), "%s", name);
        g_title[0] = g_message[0] = 0;
    }
    pthread_mutex_unlock(&g_mu);
    if (busy)
        return -1;
    job_t *j = calloc(1, sizeof(*j));
    j->fd = fd;
    j->length = length;
    j->fw = fw;
    j->done = done;
    snprintf(j->name, sizeof(j->name), "%s", name);
    snprintf(j->part, sizeof(j->part), "%s/" PART_NAME, dir);
    snprintf(j->final, sizeof(j->final), "%s", dir);
    if (pre_len) {
        j->pre = mem_alloc(pre_len);
        memcpy(j->pre, pre, pre_len);
        j->pre_len = pre_len;
    }
    /* a stalled sender ends the transfer instead of holding it forever */
    struct timeval tv = {60, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    printf("[upload] receiving %s (%llu bytes) into %s\n", name, (unsigned long long)length, dir);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &at, receive, j);
    pthread_attr_destroy(&at);
    if (rc)
        receive(j); /* no thread: here (the panel waits for it) */
    return 0;
}
