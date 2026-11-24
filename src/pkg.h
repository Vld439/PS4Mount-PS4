/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4 pkg reader. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A title ID at s: four capital letters and five digits. Games are CUSA; homebrew ports
 * and apps use letters of their own (SSBU00001, PCSX20007). NPXS are the system's apps,
 * PPSA the PS5's games. */
static inline int game_id_at(const char *s) {
    for (int i = 0; i < 9; i++)
        if (i < 4 ? !(s[i] >= 'A' && s[i] <= 'Z') : !(s[i] >= '0' && s[i] <= '9'))
            return 0;
    return strncmp(s, "NPXS", 4) && strncmp(s, "PPSA", 4);
}

static inline int is_game_id(const char *s) { return strlen(s) == 9 && game_id_at(s); }

#define HOMEBREW_MIN_SIZE (1ull << 30)

/* Game categories in param.sfo: "gd" (PS4 digital games and homebrew) and "gdO"/"gdo" (PS2 Classics). */
static inline int pkg_cat_is_game(const char *cat) {
    return cat && (!strcmp(cat, "gd") || !strcmp(cat, "gdO") || !strcmp(cat, "gdo"));
}

/* Official Sony title ID prefixes: PS4 (CUSA) and PS1/PS2/PSP Classics fPKGs. */
static inline int is_sony_game_prefix(const char *tid) {
    static const char *const prefixes[] = {
        "CUSA",
        "SLUS", "SLES", "SCUS", "SCES", "SLPS", "SLPM", "SCPS", "SLKA", "SCKA", "SLAJ", "SCAJ",
        "ULUS", "ULES", "UCUS", "UCES", "ULJM", "ULJS", "UCJS",
        "NPUI", "NPUJ", "NPUG", "NPEG", "NPEF", "NPJJ", "NPJG", "NPJH", "NPJI",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(tid, prefixes[i], 4))
            return 1;
    return 0;
}

/* Homebrew (not a Sony game prefix) is played from its pkg only when it is big: a port
 * carries a game's data (gigabytes), while apps weigh a few MB and are better installed
 * the usual way. */
static inline int homebrew_small(const char *tid, uint64_t pkg_size) {
    return !is_sony_game_prefix(tid) && pkg_size < HOMEBREW_MIN_SIZE;
}

/* What kind of title an ID is, for the panel's filter: "ps4" (a CUSA game), "classic" (an ID
 * of PS1, PS2 and PSP Classics: see is_sony_game_prefix) or "homebrew" (anything else). */
static inline const char *game_platform(const char *tid) {
    return !strncmp(tid, "CUSA", 4) ? "ps4" : is_sony_game_prefix(tid) ? "classic" : "homebrew";
}

#define PKG_MAX_ENTRIES 512
#define PKG_MAX_ENTRY_SIZE (64u << 20) /* largest entry pkg_entry reads */

typedef struct {
    uint32_t id, flags1, flags2, offset, size;
    uint8_t raw[32];
} pkg_entry_t;

typedef struct {
    int fd;
    uint8_t header[0x1000];
    char content_id[37];
    char title_id[10];
    uint64_t pkg_size, pfs_offset, pfs_size;
    uint8_t digest[32];
    int nentries;
    pkg_entry_t entries[PKG_MAX_ENTRIES];
    uint8_t dk3[32];
    int have_dk3, dk3_tried;
} pkg_t;

/* Opens and parses a PS4 pkg. Returns 0 on success. */
int pkg_open(pkg_t *pkg, const char *path);
void pkg_close(pkg_t *pkg);
/* 1 when the pkg uses the public fake key (unwraps it on first use). */
int pkg_fake_key(pkg_t *pkg);

/* Reads (and decrypts when needed) an entry. Caller frees. NULL when missing. */
uint8_t *pkg_entry(pkg_t *pkg, uint32_t id, size_t *len);

typedef struct {
    char key[64];
    int is_int;
    uint32_t ival;
    char sval[512];
} sfo_item_t;

typedef struct {
    int count;
    sfo_item_t items[128];
} sfo_t;

int sfo_parse(const uint8_t *data, size_t len, sfo_t *out);
const sfo_item_t *sfo_get(const sfo_t *sfo, const char *key);
