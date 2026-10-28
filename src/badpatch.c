/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The update of a game the console turned down, named by its size and time so another
 * update pkg is tried again. */
#include "badpatch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

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
