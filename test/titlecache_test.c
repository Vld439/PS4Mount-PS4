/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Title cache tests: a game is read once, then only when its files change, and the cache
 * left by the last run brings it back without reading anything.
 * Build and run with: bash test/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "titlecache.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

static void fill(title_info_t *t, const char *tid, const char *name) {
    snprintf(t->dir, sizeof(t->dir), "/mnt/usb0/ps4mount/nas-1/%s - GAME.pkg", name);
    snprintf(t->app_path, sizeof(t->app_path), "%s", t->dir);
    snprintf(t->name, sizeof(t->name), "%s%s", name, name[0] == 'T' ? " (with a tab\there)" : "");
    snprintf(t->ver, sizeof(t->ver), "01.03");
    snprintf(t->game_ver, sizeof(t->game_ver), "01.00");
    snprintf(t->content_id, sizeof(t->content_id), "EP0000-%s_00-TESTGAME00000000", tid);
    t->system_ver = 0x09000001;
    t->app_size = 4294967296ull;
    t->has_patch = 1;
    snprintf(t->patch_path, sizeof(t->patch_path), "/mnt/usb0/ps4mount/nas-1/%s - UPDATE.pkg", name);
    t->patch_size = 1234567;
    snprintf(t->update_issue, sizeof(t->update_issue), "Update ignored: %s", name);
    /* two lines, as src_open_paths makes it */
    snprintf(t->marker, sizeof(t->marker), "%s\n0123456789abcdef\n", t->dir);
    t->sfo_len = 1344;
    for (int i = 0; i < 32; i++)
        t->sfo_sha[i] = (unsigned char)i;
    snprintf(t->dlc_tids[0], 10, "CUSA00011");
    snprintf(t->dlc_tids[1], 10, "CUSA00012");
    t->ndlc_tids = 2;
    title_dlc_t *d = title_cache_give_dlc(t, 2);
    CHECK(d != NULL);
    if (!d)
        return;
    snprintf(d[0].title_id, sizeof(d[0].title_id), "CUSA00011");
    snprintf(d[0].content_id, sizeof(d[0].content_id), "EP0000-CUSA00011_00-ADDONAAAAAAAAAAA");
    d[0].install = 1;
    snprintf(d[1].title_id, sizeof(d[1].title_id), "CUSA00012");
    snprintf(d[1].content_id, sizeof(d[1].content_id), "EP0000-CUSA00012_00-ADDONBBBBBBBBBBB");
    d[1].install = 0;
    t->ndlc = 2;
}

int main(void) {
    /* the fingerprint: the same files hash the same, a change in any of them does not */
    uint64_t a = title_fp_add(title_fp_start(), "/mnt/usb0/Game.pkg", 100, 1700000000);
    uint64_t b = title_fp_add(title_fp_start(), "/mnt/usb0/Game.pkg", 100, 1700000000);
    uint64_t c = title_fp_add(title_fp_start(), "/mnt/usb0/Game.pkg", 100, 1700000001);
    uint64_t d = title_fp_add(title_fp_start(), "/mnt/usb0/Game.pkg", 101, 1700000000);
    uint64_t e = title_fp_add(title_fp_start(), "/mnt/usb0/Other.pkg", 100, 1700000000);
    CHECK(a == b);
    CHECK(a != c && a != d && a != e);
    CHECK(a != title_fp_start());

    /* a game that was never read is not there */
    CHECK(title_cache_find("/mnt/usb0", "CUSA00001", a) == NULL);

    title_info_t *t = title_cache_add("/mnt/usb0", "CUSA00001", a);
    CHECK(t != NULL);
    if (!t) {
        printf("%d checks, %d failed\n", g_checks, g_failed);
        return 1;
    }
    fill(t, "CUSA00001", "Terminator 2D");

    const title_info_t *got = title_cache_find("/mnt/usb0", "CUSA00001", a);
    CHECK(got != NULL && !strcmp(got->name, "Terminator 2D (with a tab\there)"));
    CHECK(got && got->ndlc == 2 && !strcmp(got->dlc[1].content_id, "EP0000-CUSA00012_00-ADDONBBBBBBBBBBB"));
    /* another fingerprint: its files changed, it has to be read again */
    CHECK(title_cache_find("/mnt/usb0", "CUSA00001", c) == NULL);
    /* another game is not served by it */
    CHECK(title_cache_find("/mnt/usb0", "CUSA00002", a) == NULL);
    /* nor the same title on another drive */
    CHECK(title_cache_find("/mnt/usb1", "CUSA00001", a) == NULL);

    /* it survives a reload of the payload: as it is, with the marker's newlines */
    const char *path = "build/test/titles-cache.txt";
    CHECK(title_cache_save(path, "/mnt/usb0") == 0);
    title_cache_forget();
    CHECK(title_cache_find("/mnt/usb0", "CUSA00001", a) == NULL);
    CHECK(title_cache_load(path) == 0);
    got = title_cache_find("/mnt/usb0", "CUSA00001", a);
    CHECK(got != NULL);
    if (got) {
        CHECK(!strcmp(got->marker, "/mnt/usb0/ps4mount/nas-1/Terminator 2D - GAME.pkg\n0123456789abcdef\n"));
        CHECK(got->has_patch && got->app_size == 4294967296ull && got->patch_size == 1234567);
        CHECK(!strcmp(got->content_id, "EP0000-CUSA00001_00-TESTGAME00000000"));
        CHECK(got->system_ver == 0x09000001 && got->sfo_len == 1344 && got->sfo_sha[31] == 31);
        CHECK(got->ndlc_tids == 2 && !strcmp(got->dlc_tids[1], "CUSA00012"));
        CHECK(got->ndlc == 2 && !strcmp(got->dlc[0].title_id, "CUSA00011") && got->dlc[0].install == 1 &&
              !strcmp(got->dlc[1].content_id, "EP0000-CUSA00012_00-ADDONBBBBBBBBBBB") && got->dlc[1].install == 0);
        CHECK(!strcmp(got->update_issue, "Update ignored: Terminator 2D"));
    }

    /* a file of another version, or a damaged one, leaves nothing behind */
    title_cache_forget();
    FILE *f = fopen("build/test/titles-bad.txt", "w");
    CHECK(f != NULL);
    if (f) {
        fprintf(f, "#titles 99\n/mnt/usb0\tCUSA00003\t1\t/x\n");
        fclose(f);
    }
    CHECK(title_cache_load("build/test/titles-bad.txt") == -1);
    CHECK(title_cache_find("/mnt/usb0", "CUSA00003", 1) == NULL);
    f = fopen("build/test/titles-bad2.txt", "w");
    CHECK(f != NULL);
    if (f) { /* right header, a line with fields missing and a line with a bad hash */
        fprintf(f, "#titles 1\n/mnt/usb0\tCUSA00004\t9\t/x\n");
        fprintf(f, "#titles 1\n");
        fclose(f);
    }
    CHECK(title_cache_load("build/test/titles-bad2.txt") == 0);
    CHECK(title_cache_find("/mnt/usb0", "CUSA00004", 9) == NULL);

    /* the "scan for games" button empties it */
    title_cache_forget();
    CHECK(title_cache_find("/mnt/usb0", "CUSA00001", a) == NULL);

    unlink(path);
    unlink("build/test/titles-bad.txt");
    unlink("build/test/titles-bad2.txt");
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
