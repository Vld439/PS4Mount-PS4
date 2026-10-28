/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What a game of a drive is, without opening its pkgs again. Reading one pkg is about a
 * megabyte: over a network share that is the whole library on every look, so a game is
 * read once and then only when its files change (path, size or mtime). */
#pragma once

#include <limits.h>
#include <stdint.h>

/* Add-ons kept per game, and the add-on title IDs mounted for it (MAX_DLC_TIDS in main.c). */
#define TITLE_DLC_MAX 64
#define TITLE_DLC_TIDS 8

typedef struct {
    char title_id[10], content_id[37];
    int install; /* 1: the system's installer takes it, 0: mounted */
} title_dlc_t;

typedef struct {
    char title_id[10];
    char dir[PATH_MAX], app_path[PATH_MAX], patch_path[PATH_MAX];
    int has_patch;
    /* what the panel shows */
    char name[128], ver[8], game_ver[8], content_id[37];
    uint32_t system_ver;
    uint64_t app_size, patch_size;
    char update_issue[256];
    /* the marker of the registration: folder, digest of the update (or "nopatch") and,
     * with add-ons, the digest of all of them */
    char marker[PATH_MAX + 160];
    char dlc_tids[TITLE_DLC_TIDS][10];
    int ndlc_tids;
    /* the update's param.sfo, to tell whether the system's copy is still the one */
    uint32_t sfo_len;
    unsigned char sfo_sha[32];
    int ndlc;
    title_dlc_t *dlc;
} title_info_t;

/* 64-bit hash of a game's files: starts empty and takes each path with its size and mtime. */
uint64_t title_fp_start(void);
uint64_t title_fp_add(uint64_t h, const char *path, uint64_t size, int64_t mtime);

/* The entry of tid on root, when its files hash to fp. NULL when it has to be read. */
const title_info_t *title_cache_find(const char *root, const char *tid, uint64_t fp);
/* The entry to fill (the old one is dropped). Use title_cache_give_dlc for its add-ons. */
title_info_t *title_cache_add(const char *root, const char *tid, uint64_t fp);
/* Gives the entry room for n add-ons (from mapped pages, never the host's few MB of heap). */
title_dlc_t *title_cache_give_dlc(title_info_t *e, int n);
void title_cache_forget(void);
/* Reads the file left by the last run; a damaged or older one is ignored. 0 on success. */
int title_cache_load(const char *file);
/* Writes the games of root when any of them was read or changed since the last write.
 * 0 on success (also when there was nothing to write). */
int title_cache_save(const char *file, const char *root);
