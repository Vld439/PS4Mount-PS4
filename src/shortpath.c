/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The short path the system is handed for a pkg whose own path is too long. */
#include "shortpath.h"

#include <stdio.h>
#include <string.h>

int short_pkg_link(const char *dir, const char *tid, const char *kind, const char *path, char *out, size_t n) {
    if (strlen(path) <= SYS_PATH_MAX)
        return 0;
    int l = snprintf(out, n, "%s/%s-%s.pkg", dir, tid, kind);
    /* the link is short by construction; without room for it ps4mount says the real
     * path and the system cuts it, as it would have without the link */
    return l > 0 && (size_t)l < n;
}
