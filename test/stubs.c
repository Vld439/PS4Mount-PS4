/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The app_* calls of the web panel, recorded instead of done, for the host tests. */
#include <stdio.h>
#include <string.h>

#include "drive.h"
#include "links.h"
#include "stubs.h"
#include "web.h"

char g_stub_call[64], g_stub_arg[4096];
int g_stub_calls;

static int record(const char *call, const char *arg, char *msg, size_t cap) {
    snprintf(g_stub_call, sizeof(g_stub_call), "%s", call);
    snprintf(g_stub_arg, sizeof(g_stub_arg), "%s", arg ? arg : "(null)");
    g_stub_calls++;
    snprintf(msg, cap, "done \"%s\"", call);
    return 0;
}

void app_games_json(buf_t *out) { buf_printf(out, "{\"version\":\"test\",\"games\":[]}"); }
int app_add(const char *tid, char *msg, size_t cap) { return record("add", tid, msg, cap); }
int app_remove(const char *tid, char *msg, size_t cap) { return record("remove", tid, msg, cap); }
void app_rescan(void) {
    char m[8];
    record("rescan", NULL, m, sizeof(m));
}
void app_rescan_hard(void) {
    char m[8];
    record("rescan-hard", NULL, m, sizeof(m));
}
void app_panel_close(void) {
}

void app_stop(void) {
    char m[8];
    record("stop", NULL, m, sizeof(m));
}
int app_restore(const char *tids, char *msg, size_t cap) { return record("restore", tids, msg, cap); }
int app_restored_forget(char *msg, size_t cap) { return record("forget", NULL, msg, cap); }
void app_config_json(buf_t *out) { buf_printf(out, "{\"batch\":true}"); }
int app_config_set(const char *body, char *msg, size_t cap) { return record("config", body, msg, cap); }
int app_nas_reload(char *msg, size_t cap) { return record("nas_reload", NULL, msg, cap); }
void app_nas_scan_json(buf_t *out) { buf_printf(out, "{\"hosts\":[{\"ip\":\"192.168.0.19\",\"name\":\"PC\",\"smb\":true,\"nfs\":false}]}"); }
void app_nas_shares_json(const char *body, buf_t *out) {
    record("nas_shares", body, NULL, 0);
    buf_printf(out, "{\"ok\":true,\"auth\":false,\"message\":\"\",\"shares\":[\"games\"]}");
}
int app_icon(const char *tid, buf_t *out) {
    char m[8];
    record("icon", tid, m, sizeof(m));
    buf_put(out, "PNG", 3);
    return 0;
}
int app_pic(const char *tid, buf_t *out) {
    char m[8];
    record("pic", tid, m, sizeof(m));
    buf_put(out, "PNG", 3);
    return 0;
}
void app_check(const void *data, size_t len, uint64_t size, buf_t *out) {
    char arg[64], m[8];
    (void)data;
    snprintf(arg, sizeof(arg), "%zu %llu", len, (unsigned long long)size);
    record("check", arg, m, sizeof(m));
    buf_printf(out, "{\"ok\":true}");
}
int app_upload(int fd, const char *query, const void *pre, size_t pre_len, uint64_t length, char *msg, size_t cap) {
    (void)fd, (void)pre, (void)pre_len, (void)length;
    record("upload", query, msg, cap);
    return -1; /* refused: the panel answers with msg */
}
void app_uploads_json(buf_t *out) { buf_printf(out, "{\"state\":\"idle\"}"); }
void app_dests_json(buf_t *out) { buf_printf(out, "{\"dests\":[]}"); }

void drive_status_json(buf_t *out) { buf_printf(out, "{\"configured\":true}"); }
int drive_connect(char *msg, size_t n) { return record("drive_connect", NULL, msg, n); }
int drive_disconnect(char *msg, size_t n) { return record("drive_disconnect", NULL, msg, n); }
int drive_upload(const char *tid, char *msg, size_t n) { return record("drive_upload", tid, msg, n); }
int drive_download(const char *tid, const char *dest, char *msg, size_t n) {
    char arg[64];
    snprintf(arg, sizeof(arg), "%s %s", tid, dest);
    return record("drive_download", arg, msg, n);
}
int drive_trash(const char *tid, char *msg, size_t n) { return record("drive_trash", tid, msg, n); }
int drive_refresh(char *msg, size_t n) { return record("drive_refresh", NULL, msg, n); }
int drive_cancel(char *msg, size_t n) { return record("drive_cancel", NULL, msg, n); }
int drive_set_client(const char *id, const char *secret, char *msg, size_t n) {
    char arg[512];
    snprintf(arg, sizeof(arg), "%s|%s", id, secret);
    return record("drive_client", arg, msg, n);
}
int drive_forget_client(char *msg, size_t n) { return record("drive_forget_client", NULL, msg, n); }

void links_status_json(buf_t *out) { buf_printf(out, "{\"configured\":true,\"jobs\":[]}"); }
int links_add(const char *text, const char *dest, const char *password, char *msg, size_t n) {
    char arg[4096];
    snprintf(arg, sizeof(arg), "%s|%zu|%.40s|%s", dest, strlen(text), text, password ? password : "");
    return record("links_add", arg, msg, n);
}
int links_cancel(int id, char *msg, size_t n) {
    char arg[16];
    snprintf(arg, sizeof(arg), "%d", id);
    return record("links_cancel", arg, msg, n);
}
int links_retry(int id, const char *password, char *msg, size_t n) {
    char arg[160];
    snprintf(arg, sizeof(arg), "%d|%s", id, password ? password : "");
    return record("links_retry", arg, msg, n);
}
