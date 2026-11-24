/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * GoldHEN's payloads.ini AutoRun list, edited as text. */
#include "autorun.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int autorun_is_ours(const char *line, size_t len) {
    static const char *const names[] = {"ps4mount-ps4.bin", "ps4mount-ps4.elf"};
    size_t a = 0;
    while (a < len && (line[a] == ' ' || line[a] == '\t'))
        a++;
    if (a < len && (line[a] == '#' || line[a] == ';'))
        return 0; /* commented out: the user's line, not our entry */
    size_t base = a, end = a;
    while (end < len && line[end] != '=') {
        if (line[end] == '/')
            base = end + 1;
        end++;
    }
    while (end > base && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                          line[end - 1] == '\r' || line[end - 1] == '\n'))
        end--;
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
        if (strlen(names[k]) == end - base && !strncasecmp(line + base, names[k], end - base))
            return 1;
    return 0;
}

static int append(buf_t *out, const void *p, size_t len) {
    size_t before = out->len;
    buf_put(out, p, len);
    return out->len - before == len ? 0 : -1;
}

int autorun_edit_ini(const uint8_t *raw, size_t len, const char *payload_path, int on, buf_t *out) {
    if (on && (!payload_path || !payload_path[0]))
        return -1;
    size_t plen = on ? strlen(payload_path) : 0;
    int added = 0;
    for (size_t pos = 0; raw && pos < len;) {
        size_t e = pos;
        while (e < len && raw[e] != '\n')
            e++;
        const char *line = (const char *)raw + pos;
        size_t ll = e - pos, vl = ll;
        while (vl && (line[vl - 1] == '\r' || line[vl - 1] == ' ' || line[vl - 1] == '\t'))
            vl--;
        pos = e < len ? e + 1 : e;
        if (autorun_is_ours(line, vl))
            continue; /* ours, wherever it was: dropped, added back below */
        if (append(out, line, ll) || (e < len && append(out, "\n", 1)))
            return -1;
        if (on && !added) {
            size_t a = 0;
            while (a < vl && (line[a] == ' ' || line[a] == '\t'))
                a++;
            if (vl - a == 9 && line[a] == '[' && !strncasecmp(line + a + 1, "autorun]", 8)) {
                if ((e == len && append(out, "\n", 1)) ||
                    append(out, payload_path, plen) || append(out, " = 1\n", 5))
                    return -1;
                added = 1;
            }
        }
    }
    if (on && !added) {
        if (out->len) {
            if ((out->data[out->len - 1] != '\n' && append(out, "\n", 1)) || append(out, "\n", 1))
                return -1;
        }
        if (append(out, "[AutoRun]\n", 10) || append(out, payload_path, plen) || append(out, " = 1\n", 5))
            return -1;
    }
    return 0;
}

int autorun_write_atomic(const char *ini_path, const void *data, size_t len) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", ini_path) >= (int)sizeof(tmp))
        return -1;
    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    const uint8_t *p = data;
    int rc = 0;
    struct stat st;
    if (stat(ini_path, &st) == 0 && fchmod(fd, st.st_mode & 0777))
        rc = -1;
    while (!rc && len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) { /* 0 would loop forever */
            rc = -1;
            break;
        }
        p += n;
        len -= (size_t)n;
    }
    /* A filesystem that cannot fsync (EINVAL/ENOTSUP) is not fatal; other errors are. */
    if (!rc && fsync(fd) && errno != EINVAL && errno != ENOTSUP)
        rc = -1;
    if (close(fd))
        rc = -1;
    if (!rc && rename(tmp, ini_path))
        rc = -1;
    if (rc)
        unlink(tmp);
    return rc;
}
