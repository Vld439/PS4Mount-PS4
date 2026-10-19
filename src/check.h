/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What a pkg is and whether this console can use it, read from its first bytes. */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum { PKG_KIND_OTHER, PKG_KIND_GAME, PKG_KIND_UPDATE, PKG_KIND_ADDON };

typedef struct {
    int ok;               /* PS4Mount can add it */
    int kind;             /* PKG_KIND_* */
    int fake;             /* signed with the public fake key */
    char content_id[37], title_id[10];
    char title[128], app_ver[8], version[8];
    uint32_t system_ver;  /* SYSTEM_VER of param.sfo (0: absent) */
    uint64_t pkg_size;    /* the size the header declares */
    char problem[192];    /* why it cannot be used (empty when ok) */
} pkg_check_t;

/* Reads path, which may hold only the start of a pkg (the header, its entry table and
 * param.sfo are in the first few MB). fw: kernel_get_fw_version() of the console, 0 to
 * skip the firmware check. Returns 0 when it is a PS4 pkg (even one that cannot be
 * used: see ok and problem), -1 when it is not one. */
int pkg_check(const char *path, uint32_t fw, pkg_check_t *out);

/* "9.00" for a SYSTEM_VER or firmware version (BCD major and minor in the top 16 bits). */
void ver_text(uint32_t v, char *out, size_t n);

/* 1 when a pkg built for system_ver runs on firmware fw. */
int fw_runs(uint32_t system_ver, uint32_t fw);

/* "game", "update", "add-on" or "other". */
const char *pkg_kind_name(int kind);
