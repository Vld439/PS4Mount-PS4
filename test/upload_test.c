/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pkgs sent from the panel (src/upload.c), over a real socket: a good pkg lands under its
 * name, a bad one is stopped and leaves nothing behind. Run with: bash test/run.sh */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pkgmake.h"
#include "upload.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

#define FW 0x13520001

static char g_dir[64], g_done_path[512];
static volatile int g_done;

static void on_done(const char *path) {
    snprintf(g_done_path, sizeof(g_done_path), "%s", path);
    g_done = 1;
}

static uint8_t *load(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*len);
    if (fread(d, 1, *len, f) != *len)
        *len = 0;
    fclose(f);
    return d;
}

typedef struct {
    int fd;
    const uint8_t *data;
    size_t len;
} sender_t;

static void *send_all(void *arg) {
    sender_t *s = arg;
    size_t off = 0;
    while (off < s->len) {
        ssize_t w = write(s->fd, s->data + off, s->len - off > 65536 ? 65536 : s->len - off);
        if (w <= 0)
            break; /* the receiver stopped it */
        off += (size_t)w;
    }
    shutdown(s->fd, SHUT_WR);
    return NULL;
}

/* Sends data (pre bytes as if read with the headers) declaring length; returns the answer. */
static const char *transfer(const char *name, const uint8_t *data, size_t len, uint64_t length, size_t pre) {
    static char resp[4096];
    int sv[2];
    resp[0] = 0;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
        return resp;
    g_done = 0;
    if (upload_start(sv[1], g_dir, name, length, data, pre, FW, on_done)) {
        close(sv[0]);
        close(sv[1]);
        return "busy";
    }
    sender_t s = {sv[0], data + pre, len - pre};
    pthread_t th;
    pthread_create(&th, NULL, send_all, &s);
    size_t n = 0;
    ssize_t r;
    while (n < sizeof(resp) - 1 && (r = read(sv[0], resp + n, sizeof(resp) - 1 - n)) > 0)
        n += (size_t)r;
    resp[n] = 0;
    pthread_join(th, NULL);
    close(sv[0]);
    /* the answer is written before the receiver lets go of the job: wait for it */
    for (int i = 0; i < 200; i++) {
        buf_t b = {0};
        upload_status_json(&b);
        buf_put(&b, "", 1);
        int running = strstr((char *)b.data, "\"receiving\"") != NULL;
        buf_free(&b);
        if (!running)
            break;
        usleep(10000);
    }
    return resp;
}

static int exists_in(const char *name) {
    char p[256];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    return !stat(p, &st);
}

static int status_has(const char *s) {
    buf_t b = {0};
    upload_status_json(&b);
    buf_put(&b, "", 1);
    int r = strstr((char *)b.data, s) != NULL;
    buf_free(&b);
    return r;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN); /* as the daemon: a refused sender writes into a closed socket */
    snprintf(g_dir, sizeof(g_dir), "/tmp/ps4mount-upload-XXXXXX");
    if (!mkdtemp(g_dir))
        return 1;
    char src[128];
    snprintf(src, sizeof(src), "%s/src.bin", g_dir);
    const char *cid = "UP0000-CUSA00001_00-TESTGAME00000000";
    size_t len;
    uint8_t *data;
    const char *r;

    printf("a good pkg\n");
    /* bigger than UPLOAD_CHECK_BYTES: checked midway and at the end */
    make_pkg(src, cid, "gd", 0x09008000, 1, UPLOAD_CHECK_BYTES + 3000000, UPLOAD_CHECK_BYTES + 3000000);
    data = load(src, &len);
    r = transfer("Game.pkg", data, len, len, 5000);
    CHECK(strstr(r, "200 OK") && strstr(r, "\"ok\":true"));
    CHECK(g_done && strstr(g_done_path, "/Game.pkg"));
    CHECK(exists_in("Game.pkg") && !exists_in(".ps4mount-upload.part"));
    CHECK(status_has("\"done\"") && status_has("\"title\":\"Test Game\""));
    struct stat st;
    char p[256];
    snprintf(p, sizeof(p), "%s/Game.pkg", g_dir);
    CHECK(!stat(p, &st) && (size_t)st.st_size == len);

    printf("the same name again\n");
    r = transfer("Game.pkg", data, len, len, 0);
    CHECK(strstr(r, "\"ok\":true") && exists_in("Game (1).pkg"));
    free(data);

    printf("a small pkg (checked only at the end)\n");
    make_pkg(src, cid, "gp", 0x09008000, 1, 0x4000, 0x4000);
    data = load(src, &len);
    r = transfer("Update.pkg", data, len, len, 100);
    CHECK(strstr(r, "\"ok\":true") && exists_in("Update.pkg"));
    free(data);

    printf("not a fake pkg: stopped once its start is in\n");
    make_pkg(src, cid, "gd", 0x09008000, 0, UPLOAD_CHECK_BYTES + 8000000, UPLOAD_CHECK_BYTES + 8000000);
    data = load(src, &len);
    g_done = 0;
    r = transfer("Retail.pkg", data, len, len, 0);
    CHECK(strstr(r, "400") && strstr(r, "fake") && !g_done);
    CHECK(!exists_in("Retail.pkg") && !exists_in(".ps4mount-upload.part") && status_has("\"failed\""));
    free(data);

    printf("needs a newer firmware\n");
    make_pkg(src, cid, "gd", 0x14000000, 1, 0x4000, 0x4000);
    data = load(src, &len);
    r = transfer("New.pkg", data, len, len, 0);
    CHECK(strstr(r, "400") && strstr(r, "needs firmware 14.00") && !exists_in("New.pkg"));
    free(data);

    printf("the header says another size\n");
    make_pkg(src, cid, "gd", 0x09008000, 1, 0x8000, 0x4000); /* file shorter than declared */
    data = load(src, &len);
    r = transfer("Short.pkg", data, len, len, 0);
    CHECK(strstr(r, "400") && strstr(r, "incomplete") && !exists_in("Short.pkg"));
    free(data);

    printf("the sender stops midway\n");
    make_pkg(src, cid, "gd", 0x09008000, 1, 0x40000, 0x40000);
    data = load(src, &len);
    r = transfer("Cut.pkg", data, len / 2, len, 0); /* declares all of it, sends half */
    CHECK(strstr(r, "400") && strstr(r, "stopped after") && !exists_in("Cut.pkg") && !exists_in(".ps4mount-upload.part"));
    free(data);

    printf("not a pkg\n");
    static const uint8_t junk[] = "this is not a pkg at all, just some bytes";
    r = transfer("Junk.pkg", junk, sizeof(junk), sizeof(junk), 0);
    CHECK(strstr(r, "400") && strstr(r, "not a PS4 pkg") && !exists_in("Junk.pkg"));

    printf("file names\n");
    char n[256];
    CHECK(!upload_clean_name("Game.pkg", n, sizeof(n)) && !strcmp(n, "Game.pkg"));
    CHECK(!upload_clean_name("../../etc/x.pkg", n, sizeof(n)) && !strcmp(n, "x.pkg"));
    CHECK(!upload_clean_name("C:\\games\\y.PKG", n, sizeof(n)) && !strcmp(n, "y.PKG"));
    CHECK(!upload_clean_name("..hidden", n, sizeof(n)) && !strcmp(n, "hidden.pkg"));
    CHECK(!upload_clean_name("name", n, sizeof(n)) && !strcmp(n, "name.pkg"));
    CHECK(!upload_clean_name("a\tb\x01" "c.pkg", n, sizeof(n)) && !strcmp(n, "abc.pkg"));
    CHECK(!upload_clean_name("Juego – Edición [v1.00].pkg", n, sizeof(n)) && !strcmp(n, "Juego – Edición [v1.00].pkg"));
    CHECK(upload_clean_name("", n, sizeof(n)) == -1);
    CHECK(upload_clean_name("../..", n, sizeof(n)) == -1);
    CHECK(upload_clean_name("dir/", n, sizeof(n)) == -1);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd))
        printf("could not remove %s\n", g_dir);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}
