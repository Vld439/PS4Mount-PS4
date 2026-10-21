/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Downloads from links pasted in the panel: a pkg, a ZIP or a RAR (also in volumes,
 * name.part1.rar... or name.rar, name.r00...). The console downloads them itself, several
 * connections at a time, and an archive is extracted while it arrives: its pkgs go to a
 * scan folder, where the scan adds them. All in one thread of its own; the panel only
 * reads its state. A download cut by stopping PS4Mount goes on when it loads again. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build.h"

#define LINKS_MAX_JOBS 32
#define LINKS_MAX_FILES 256 /* volumes of one archive */

/* What PS4Mount gives the links thread (main.c; the tests give their own). */
typedef struct {
    /* The scan folder downloads to dest go to (made); -1 when dest is not usable now. */
    int (*dest_dir)(const char *dest, char *dir, size_t n);
    uint64_t (*free_bytes)(const char *dir);
    /* The pkg whose start (or all of it) is at path, size bytes in all (0: not known):
     * 0 when PS4Mount can use it, -1 with the reason in problem. */
    int (*check)(const char *path, uint64_t size, char *problem, size_t n);
    /* New pkgs are in dir (a folder in a scan folder). */
    void (*arrived)(const char *dir);
    void (*notify)(const char *msg);
} links_host_t;

void links_start(const char *state_dir, const links_host_t *host);
/* Ends the thread; the download in progress goes on at the next start. */
void links_stop(void);

/* 1 while a download runs or waits (the console is kept out of rest mode). */
int links_active(void);

/* {"jobs":[..],"job":{..},"result":..,"ok":..} */
void links_status_json(buf_t *out);

/* Panel actions; 0 when accepted, msg says what happens or why not. */
/* text: one link a line, "link | name" to give it a name; the volumes of an archive
 * become one download. dest: usb0..usb7, ext0, ext1 or internal. password: an archive's
 * (NULL or empty: the ones that opened others are tried). */
int links_add(const char *text, const char *dest, const char *password, char *msg, size_t n);
/* Stops the download id (or drops it from the queue), its partial files deleted. */
int links_cancel(int id, char *msg, size_t n);
/* A download that failed, once more from where it was (with this password, when given). */
int links_retry(int id, const char *password, char *msg, size_t n);

/* Blocks of a download, fetched several at a time; the tests make them small. */
extern size_t g_links_block;
/* First wait before a request is tried again (it doubles each time). */
extern int g_links_retry_seconds;
/* Connections at a time for one file. */
extern int g_links_threads;
