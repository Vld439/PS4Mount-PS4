/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The short path the system is handed for a pkg whose own path is too long. */
#include <stdio.h>
#include <string.h>

#include "../src/shortpath.h"

static int fails, checks;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static void repeat(char *out, size_t n, char c) {
    memset(out, c, n);
    out[n] = 0;
}

int main(void) {
    char path[512], link[512];

    /* a path that fits goes as it is (a share at the root, a short USB folder) */
    snprintf(path, sizeof(path), "/mnt/usb0/ps4mount/nas-1/CUSA03173.pkg");
    CHECK(short_pkg_link("/data/ps4mount/l", "CUSA03173", "app", path, link, sizeof(link)) == 0);
    snprintf(path, sizeof(path), "/mnt/usb0/ps4/ps4/%s/app.pkg", "CUSA03173");
    CHECK(short_pkg_link("/data/ps4mount/l", "CUSA03173", "app", path, link, sizeof(link)) == 0);

    /* 95 bytes still fits */
    repeat(path, SYS_PATH_MAX, 'a');
    path[0] = '/';
    CHECK(strlen(path) == SYS_PATH_MAX);
    CHECK(short_pkg_link("/data/ps4mount/l", "CUSA03173", "app", path, link, sizeof(link)) == 0);

    /* 96 does not: the system would cut it */
    repeat(path, SYS_PATH_MAX + 1, 'a');
    path[0] = '/';
    memcpy(path + SYS_PATH_MAX - 3, ".pkg", 4); /* 96 bytes, pkg name included */
    CHECK(short_pkg_link("/data/ps4mount/l", "CUSA03173", "app", path, link, sizeof(link)) == 1);
    CHECK(!strcmp(link, "/data/ps4mount/l/CUSA03173-app.pkg"));

    /* the link names the game and what of it, so app, patch and add-ons of one game do
     * not share it, and it is always short enough for the system */
    CHECK(short_pkg_link("/data/ps4mount/l", "CUSA03173", "patch", path, link, sizeof(link)) == 1);
    CHECK(!strcmp(link, "/data/ps4mount/l/CUSA03173-patch.pkg"));
    CHECK(short_pkg_link("/data/ps4mount/l", "CUSA03174", "ac-000000000000DLC1", path, link, sizeof(link)) == 1);
    CHECK(!strcmp(link, "/data/ps4mount/l/CUSA03174-ac-000000000000DLC1.pkg"));
    CHECK(strlen("/user") + strlen(link) <= SYS_PATH_MAX);


    /* the longest name a game has: a share at the deepest folder */
    repeat(path, 300, 'b');
    path[0] = '/';
    memcpy(path + 295, ".pkg", 5);
    CHECK(short_pkg_link("/data/ps4mount/l", "CUSA03173", "app", path, link, sizeof(link)) == 1);
    CHECK(strlen(link) < SYS_PATH_MAX);

    /* no room for the link: the path is left as it is (and ps4mount says so) */
    char tiny[8];
    CHECK(short_pkg_link("/data/ps4mount/l", "CUSA03173", "app", path, tiny, sizeof(tiny)) == 0);

    printf("%s: %d check(s), %d failed\n", fails ? "FAIL" : "short path", checks, fails);
    return fails ? 1 : 0;
}
