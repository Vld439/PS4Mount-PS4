/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Synthetic pkgs for the host tests: what pkg_check reads, and nothing else. */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crypto.h"
#include "keys.h"

static void put_be32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (24 - 8 * i));
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

/* param.sfo with CATEGORY, TITLE, APP_VER, VERSION (strings) and SYSTEM_VER (int). */
static size_t make_check_sfo(uint8_t *d, const char *cat, uint32_t sysver) {
    static const char *keys[] = {"APP_VER", "CATEGORY", "SYSTEM_VER", "TITLE", "VERSION"};
    const char *vals[] = {"01.04", cat, NULL, "Test Game", "01.00"};
    enum { N = 5 };
    memset(d, 0, 1024);
    uint32_t ko = 20 + N * 16, kl = 0;
    for (int i = 0; i < N; i++)
        kl += (uint32_t)strlen(keys[i]) + 1;
    uint32_t dof = (ko + kl + 3) & ~3u, koff = 0, voff = 0;
    memcpy(d, "\0PSF", 4);
    d[4] = 1, d[5] = 1;
    memcpy(d + 8, &ko, 4);
    memcpy(d + 12, &dof, 4);
    d[16] = N;
    for (int i = 0; i < N; i++) {
        uint8_t *e = d + 20 + i * 16;
        uint16_t k16 = (uint16_t)koff, fmt = vals[i] ? 0x0204 : 0x0404;
        uint32_t len = vals[i] ? (uint32_t)strlen(vals[i]) + 1 : 4, max = vals[i] ? 16 : 4;
        memcpy(e, &k16, 2);
        memcpy(e + 2, &fmt, 2);
        memcpy(e + 4, &len, 4);
        memcpy(e + 8, &max, 4);
        memcpy(e + 12, &voff, 4);
        strcpy((char *)d + ko + koff, keys[i]);
        koff += (uint32_t)strlen(keys[i]) + 1;
        if (vals[i])
            strcpy((char *)d + dof + voff, vals[i]);
        else
            memcpy(d + dof + voff, &sysver, 4);
        voff += max;
    }
    return dof + voff;
}

/* A pkg with what pkg_check reads: header, entry table, the keys entry (with entry key 3
 * wrapped with the public fake key when fake) and param.sfo. The header declares size
 * bytes; the file holds the first written bytes (all of it when written >= size). */
static void make_pkg(const char *path, const char *cid, const char *cat, uint32_t sysver, int fake, uint64_t size,
                     uint64_t written) {
    static uint8_t f[0x4000];
    memset(f, 0, sizeof(f));
    put_be32(f, 0x7f434e54);
    put_be32(f + 0x10, 2);      /* entries */
    put_be32(f + 0x18, 0x1000); /* entry table */
    memcpy(f + 0x40, cid, 36);
    put_be64(f + 0x430, size);
    /* keys entry 0x10 at 0x1100 (0x520 bytes), param.sfo 0x1000 at 0x2000 */
    put_be32(f + 0x1000, 0x10);
    put_be32(f + 0x1000 + 16, 0x1100);
    put_be32(f + 0x1000 + 20, 0x20 + 7 * 0x20 + 4 * 0x100);
    put_be32(f + 0x1020, 0x1000);
    put_be32(f + 0x1020 + 16, 0x2000);
    put_be32(f + 0x1020 + 20, (uint32_t)make_check_sfo(f + 0x2000, cat, sysver));
    uint8_t block[256], e[256] = {0};
    memset(block, 0x5a, sizeof(block)); /* PKCS#1 v1.5 type 2 padding (no zero bytes) */
    block[0] = 0, block[1] = 2;
    block[256 - 33] = 0;
    for (int i = 0; i < 32; i++)
        block[256 - 32 + i] = (uint8_t)i;
    e[253] = 1, e[255] = 1; /* 65537 */
    if (fake)
        rsa2048_modexp(block, e, k_pkg_key3_n, f + 0x1100 + 0x20 + 7 * 0x20 + 3 * 0x100);
    FILE *o = fopen(path, "wb");
    uint64_t n = written < size ? written : size;
    fwrite(f, 1, n < sizeof(f) ? (size_t)n : sizeof(f), o);
    for (uint64_t i = sizeof(f); i < n; i++)
        fputc(0, o);
    fclose(o);
}
