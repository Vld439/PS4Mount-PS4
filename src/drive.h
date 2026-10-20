/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Google Drive backups: the console signs in with a code shown in the panel (OAuth device
 * flow, scope drive.file: PS4Mount sees only the files it made), uploads the pkgs of a
 * game into PS4Mount/<Name> [CUSAxxxxx]/ and downloads them back into a scan folder. All
 * the work is done in one thread of its own; the panel only reads its state. */
#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "build.h"

#define DRIVE_MAX_FILES 72 /* game, update and add-ons of one game */

typedef struct {
    char path[PATH_MAX];
    char name[96];   /* in Drive: game.pkg, update.pkg, addon-<content id>.pkg */
    char kind[8];    /* game, update, addon */
    uint64_t size;
} drive_file_t;

/* What PS4Mount gives the Drive thread (main.c; the tests give their own). */
typedef struct {
    /* The pkgs of a library game; their count (0: not in the library). name: its title. */
    int (*game_files)(const char *tid, drive_file_t *files, int max, char *name, size_t nn);
    /* The title ids of the library's games. */
    int (*library)(char (*tids)[10], int max);
    /* The scan folder downloads to dest go to; -1 when dest is not usable now. */
    int (*dest_dir)(const char *dest, char *dir, size_t n);
    /* Free bytes in dir. */
    uint64_t (*free_bytes)(const char *dir);
    /* A game's pkgs arrived in the scan folder. */
    void (*downloaded)(const char *tid);
    void (*notify)(const char *msg);
} drive_host_t;

typedef struct {
    const char *oauth;  /* https://oauth2.googleapis.com */
    const char *api;    /* https://www.googleapis.com/drive/v3 */
    const char *upload; /* https://www.googleapis.com/upload/drive/v3 */
} drive_endpoints_t;

/* Starts the thread. client_id empty: Drive is not available in this build. ep NULL: Google. */
void drive_start(const char *state_dir, const char *client_id, const char *client_secret,
                 const drive_endpoints_t *ep, const drive_host_t *host);
/* Ends the thread (the transfer in progress stops; uploads resume where they were). */
void drive_stop(void);

/* {"configured":..,"connected":..,"email":..,"login":{..},"job":{..},"games":[..]} */
void drive_status_json(buf_t *out);

/* Panel actions; 0 when accepted, msg says what happens or why not. */
int drive_connect(char *msg, size_t n);
int drive_disconnect(char *msg, size_t n);
/* tid NULL: the whole library. */
int drive_upload(const char *tid, char *msg, size_t n);
int drive_download(const char *tid, const char *dest, char *msg, size_t n);
int drive_trash(const char *tid, char *msg, size_t n);
int drive_refresh(char *msg, size_t n);
int drive_cancel(char *msg, size_t n);

/* Chunk size of uploads (a multiple of 256 KiB, as Drive asks). The tests make it small. */
extern size_t g_drive_chunk;
/* Blocks of a download, fetched several at a time. */
extern size_t g_drive_block;
/* First wait before a request is tried again (it doubles each time). */
extern int g_drive_retry_seconds;
