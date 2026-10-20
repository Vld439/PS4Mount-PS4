/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Web panel interface. */
#pragma once

#include "build.h"

#define WEB_PORT 8770

/* Implemented by main.c for the web panel. */
void app_games_json(buf_t *out);
int app_add(const char *tid, char *msg, size_t cap);
int app_remove(const char *tid, char *msg, size_t cap);
void app_rescan(void);
void app_stop(void);
/* Closes the open game and moves the pkgs of the games in tids (comma separated title ids)
 * or of all games (NULL) back to where they came from. */
int app_restore(const char *tids, char *msg, size_t cap);
/* Forgets the pkgs moved back, so the scan picks them up again. */
int app_restored_forget(char *msg, size_t cap);
void app_config_json(buf_t *out);
/* body: form-encoded settings; 0 when saved, msg says why not. */
int app_config_set(const char *body, char *msg, size_t cap);
/* Fills an icon0.png (pic0.png: the background) for tid; 0 on success. */
int app_icon(const char *tid, buf_t *out);
int app_pic(const char *tid, buf_t *out);
/* /api/check: data holds the first bytes of a pkg the panel is about to send, size is the
 * size of the whole file. Fills out (JSON) with what it is, whether it can be used and
 * where it can go. */
void app_check(const void *data, size_t len, uint64_t size, buf_t *out);
/* /api/upload?dest=<id>&name=<file name>: starts receiving the pkg (pre: body bytes already
 * read, length: the whole body) and takes fd, which it answers. Returns -1 when it did not
 * start: msg says why and fd is the caller's. */
int app_upload(int fd, const char *query, const void *pre, size_t pre_len, uint64_t length, char *msg, size_t cap);
/* /api/dests: the drives a pkg can go to, with their free space (as in /api/check). */
void app_dests_json(buf_t *out);
/* /api/uploads: the state of the upload. */
void app_uploads_json(buf_t *out);

int web_listen(void);
/* Serves one connection on the listening socket. */
void web_serve(int listen_fd);
/* Answers the request on a connected socket and closes it. */
void web_handle(int fd);
