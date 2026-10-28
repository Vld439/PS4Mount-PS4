/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The update of a game the console turned down, named by its size and time so another
 * update pkg is tried again. */
#include "badpatch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int badpatch_matches(const char *text, uint64_t size, int64_t mtime) {
    if (!text)
        return 0; /* no mark: the update is used */
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (!*p)
        return 1; /* an empty mark: any update of the game */
    char *end;
    errno = 0;
    unsigned long long s = strtoull(p, &end, 10);
    if (end == p || errno)
        return 1; /* a mark that makes no sense is read as "any update" */
    p = end;
    while (*p == ' ' || *p == '\t')
        p++;
    errno = 0;
    long long t = strtoll(p, &end, 10);
    if (end == p || errno)
        return 1;
    return s == size && t == mtime;
}

void badpatch_text(char *out, size_t n, uint64_t size, int64_t mtime) {
    if (n)
        snprintf(out, n, "%llu %lld\n", (unsigned long long)size, (long long)mtime);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* The 64 hex digits of a digest mark, or NULL when text is not one. */
static const char *digest_text(const char *text) {
    if (!text)
        return NULL;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    for (int i = 0; i < 64; i++)
        if (hexval(text[i]) < 0)
            return NULL;
    const char *p = text + 64;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return *p ? NULL : text;
}

int badpatch_is_digest(const char *text) { return digest_text(text) != NULL; }

int badpatch_digest_matches(const char *text, const uint8_t digest[32]) {
    const char *p = digest_text(text);
    if (!p || !digest)
        return 0;
    for (int i = 0; i < 32; i++) {
        int hi = hexval(p[i * 2]), lo = hexval(p[i * 2 + 1]);
        if ((uint8_t)(hi << 4 | lo) != digest[i])
            return 0;
    }
    return 1;
}
