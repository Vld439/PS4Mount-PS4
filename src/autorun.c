/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * GoldHEN's payloads.ini AutoRun list, edited as text. */
#include "autorun.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* What may follow the payload file name on its line: the entry's separator or the
 * end of the line, so look-alike names (".bin2", ".binary", ".bin.bak") stay. */
static int after_name(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == '=';
}

int autorun_is_ours(const char *line, size_t len) {
    static const char *const names[] = {"ps4mount-ps4.bin", "ps4mount-ps4.elf"};
    size_t a = 0;
    while (a < len && (line[a] == ' ' || line[a] == '\t'))
        a++;
    if (a < len && (line[a] == '#' || line[a] == ';'))
        return 0; /* commented out: the user's line, not our entry */
    for (size_t i = 0; i < len; i++) {
        if (i && line[i - 1] != '/')
            continue;
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
            size_t nl = strlen(names[k]), e = i + nl;
            if (e <= len && !strncasecmp(line + i, names[k], nl) && (e == len || after_name(line[e])))
                return 1;
        }
    }
    return 0;
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
        buf_put(out, line, ll);
        if (e < len)
            buf_put(out, "\n", 1);
        if (on && !added) {
            size_t a = 0;
            while (a < vl && (line[a] == ' ' || line[a] == '\t'))
                a++;
            if (vl - a == 9 && line[a] == '[' && !strncasecmp(line + a + 1, "autorun]", 8)) {
                if (e == len)
                    buf_put(out, "\n", 1); /* the header had no line end: close it first */
                buf_put(out, payload_path, plen);
                buf_put(out, " = 1\n", 5);
                added = 1;
            }
        }
    }
    if (on && !added) {
        if (out->len) {
            if (out->data[out->len - 1] != '\n')
                buf_put(out, "\n", 1);
            buf_put(out, "\n", 1); /* a blank line sets the section apart */
        }
        buf_put(out, "[AutoRun]\n", 10);
        buf_put(out, payload_path, plen);
        buf_put(out, " = 1\n", 5);
    }
    return 0;
}

int autorun_write_atomic(const char *ini_path, const void *data, size_t len) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", ini_path) >= (int)sizeof(tmp))
        return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    const uint8_t *p = data;
    int rc = 0;
    while (len) {
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
