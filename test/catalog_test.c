/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Catalog tests: PS4 pkgs found where they are, grouped per game, no rename.
 * Build and run with: bash test/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog.h"
#include "pkgmake.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

#define GAME_CID "UP0000-CUSA00001_00-TESTGAME00000000"
#define MERGED_CID "UP0000-CUSA00002_00-TESTGAME00000000"
#define NOTFAKE_CID "UP0000-CUSA00003_00-TESTGAME00000000"
#define CUT_CID "UP0000-CUSA00004_00-TESTGAME00000000"
#define LEGACY_CID "UP0000-CUSA00005_00-TESTGAME00000000"
#define ORPHAN_CID "UP0000-CUSA00009_00-TESTGAME00000000"
#define DLC_CID "UP0000-CUSA00001_00-DLCTEST00000000"
#define HIDDEN_CID "UP0000-CUSA00006_00-TESTGAME00000000"
#define SYSTEM_CID "UP0000-CUSA00007_00-TESTGAME00000000"
#define OLD_CID "UP0000-CUSA00008_00-TESTGAME00000000"

/* mkdir -p for a path below root. */
static void mkdirs(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    mkdir(tmp, 0777);
}

static void pkg_at(const char *root, const char *rel, const char *cid, const char *cat, int fake, uint64_t size,
                   uint64_t written, const char *app_ver) {
    char path[PATH_MAX], dir[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    make_pkg_ver(path, cid, cat, 0x09008000, fake, size, written, app_ver);
}

static void cleanup(const char *root) {
    char cmd[PATH_MAX + 16];
    /* test-only: remove the scratch tree */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd)) {
        /* nothing to do on failure */
    }
}

/* The game is the same title with a newer separate update: that update is used. */
static void test_update_used(const catalog_t *c) {
    const cat_game_t *g = catalog_game(c, "CUSA00001");
    CHECK(g != NULL);
    if (!g)
        return;
    const cat_pkg_t *patch = catalog_patch(c, c, g);
    CHECK(patch != NULL && !strcmp(patch->app_ver, "01.05"));
    CHECK(g->ndlc == 1);
    CHECK(g->nupdates == 1);
}

/* A merged base+update (APP_VER 01.10) with an older separate update: the game pkg
 * already has it, so the update must not be used (it would take the game back). */
static void test_merged(const catalog_t *c) {
    const cat_game_t *g = catalog_game(c, "CUSA00002");
    CHECK(g != NULL);
    if (!g)
        return;
    CHECK(g->patch == -1 && catalog_patch(c, c, g) == NULL);
    CHECK(g->nupdates == 1);
    CHECK(c->pkgs[g->updates[0]].role == CAT_ROLE_INCLUDED);
    CHECK(c->included == 1);
}

/* A game already organized in ps4/<TITLE_ID>/ is taken from there and left in place:
 * its path does not change, its old replaced update is not used, and the pkg is still
 * where it was after grouping. */
static void test_organized(const catalog_t *c, const char *root) {
    const cat_game_t *g = catalog_game(c, "CUSA00005");
    CHECK(g != NULL);
    if (!g)
        return;
    const char *app = c->pkgs[g->app].path;
    char want[PATH_MAX];
    snprintf(want, sizeof(want), "%s/ps4/CUSA00005/app.pkg", root);
    CHECK(strcmp(app, want) == 0);
    CHECK(catalog_patch(c, c, g) == NULL);
    CHECK(access(app, F_OK) == 0);
}

static void test_scan(void) {
    printf("catalog: walk, read, group\n");
    char root[] = "/tmp/ps4mount-cat-XXXXXX";
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    /* a game with its update and add-on, in the folders people use */
    pkg_at(root, "Games/Game A/Game A.pkg", GAME_CID, "gd", 1, 0x4000, 0x4000, "01.00");
    pkg_at(root, "Games/Game A/Update/v1.05.pkg", GAME_CID, "gp", 1, 0x4000, 0x4000, "01.05");
    pkg_at(root, "Games/Game A/DLC/dlc.pkg", DLC_CID, "ac", 1, 0x4000, 0x4000, "01.00");
    /* a merged base+update and an older separate update */
    pkg_at(root, "Other Game/merged.pkg", MERGED_CID, "gd", 1, 0x4000, 0x4000, "01.10");
    pkg_at(root, "Other Game/old-update.pkg", MERGED_CID, "gp", 1, 0x4000, 0x4000, "01.09");
    /* a second copy of the game, and an add-on without its game */
    pkg_at(root, "duplicate/Game A copy.pkg", GAME_CID, "gd", 1, 0x4000, 0x4000, "01.00");
    pkg_at(root, "orphan/orphan.pkg", ORPHAN_CID, "ac", 1, 0x4000, 0x4000, "01.00");
    /* unusable pkgs: not fake, and cut off (the header claims more than the file has) */
    pkg_at(root, "notfake.pkg", NOTFAKE_CID, "gd", 0, 0x4000, 0x4000, "01.00");
    pkg_at(root, "cut.pkg", CUT_CID, "gd", 1, 0x10000, 0x8000, "01.00");
    /* the layout of older PS4Mount versions keeps working; its replaced update does not */
    pkg_at(root, "ps4/CUSA00005/app.pkg", LEGACY_CID, "gd", 1, 0x4000, 0x4000, "01.04");
    /* the same, written by the helper other tests use (keeps make_pkg exercised) */
    make_pkg("/tmp/ps4mount-cat-make_pkg.pkg", LEGACY_CID, "gd", 0x09008000, 1, 0x4000, 0x4000);
    unlink("/tmp/ps4mount-cat-make_pkg.pkg");
    pkg_at(root, "ps4/CUSA00005/replaced/patch-1.pkg", OLD_CID, "gp", 1, 0x4000, 0x4000, "01.00");
    /* never searched */
    pkg_at(root, ".hidden/x.pkg", HIDDEN_CID, "gd", 1, 0x4000, 0x4000, "01.00");
    pkg_at(root, "System Volume Information/y.pkg", SYSTEM_CID, "gd", 1, 0x4000, 0x4000, "01.00");
    FILE *f = fopen("/tmp/ignored.txt", "w");
    if (f)
        fclose(f);

    catalog_t c = {0};
    int unread = catalog_walk(&c, root, NULL, NULL, NULL, 0);
    CHECK(unread == 10);
    CHECK(c.npkgs == 10);
    CHECK(catalog_read(&c) == 10);
    int ok = 0, bad = 0, short_pkgs = 0;
    for (int i = 0; i < c.npkgs; i++) {
        ok += c.pkgs[i].state == CAT_OK;
        bad += c.pkgs[i].state == CAT_BAD;
        short_pkgs += c.pkgs[i].state == CAT_SHORT;
    }
    CHECK(ok == 8 && bad == 1 && short_pkgs == 1);
    /* sorted by path */
    for (int i = 1; i < c.npkgs; i++)
        CHECK(strcmp(c.pkgs[i - 1].path, c.pkgs[i].path) < 0);

    catalog_group(&c, NULL);
    CHECK(c.ngames == 3);
    CHECK(c.dupes == 1);
    CHECK(c.orphans == 1);
    test_update_used(&c);
    test_merged(&c);
    test_organized(&c, root);

    /* a walk sees new and changed pkgs only: nothing changed, nothing to read */
    CHECK(catalog_walk(&c, root, NULL, NULL, NULL, 0) == 0);
    CHECK(catalog_read(&c) == 0);

    /* saved and loaded: same grouping without reading the headers again */
    char saved[PATH_MAX];
    snprintf(saved, sizeof(saved), "%s.txt", root);
    CHECK(catalog_save(&c, saved) == 0);
    catalog_t d = {0};
    /* the cut-off pkg is never saved: the next walk reads it again */
    CHECK(catalog_load(&d, saved) == 9);
    catalog_free(&c);
    CHECK(catalog_walk(&d, root, NULL, NULL, NULL, 0) == 1);
    CHECK(catalog_read(&d) == 1);
    catalog_group(&d, NULL);
    CHECK(d.ngames == 3 && d.dupes == 1 && d.orphans == 1);
    test_update_used(&d);
    test_merged(&d);
    test_organized(&d, root);
    catalog_free(&d);

    /* a pkg found unusable is read again when the catalog was saved with other rules */
    {
        char old[PATH_MAX];
        snprintf(old, sizeof(old), "%s-old.txt", root);
        FILE *f = fopen(old, "w");
        fprintf(f, "%d\t100\t1\t-\t-\t-\t-\t0\t-\t/x/port.pkg\n", CAT_BAD);
        fclose(f);
        catalog_t o = {0};
        CHECK(catalog_load(&o, old) == 1 && o.pkgs[0].state == CAT_UNREAD);
        o.pkgs[0].state = CAT_BAD;
        CHECK(catalog_save(&o, old) == 0);
        catalog_free(&o);
        CHECK(catalog_load(&o, old) == 1 && o.pkgs[0].state == CAT_BAD); /* the same rules: kept */
        catalog_free(&o);
        unlink(old);
    }

    /* a pkg that changes size or time is read again */
    catalog_t e = {0};
    catalog_walk(&e, root, NULL, NULL, NULL, 0);
    catalog_read(&e);
    pkg_at(root, "Other Game/merged.pkg", MERGED_CID, "gd", 1, 0x8000, 0x8000, "01.11");
    CHECK(catalog_walk(&e, root, NULL, NULL, NULL, 0) == 1);
    catalog_free(&e);

    unlink("/tmp/ignored.txt");
    unlink(saved);
    cleanup(root);
}

/* The console's own storage holds saves, screenshots and the system: PS4Mount only
 * searches /data/PS4Mount and the ps4 folder there (as main.c's internal_dir_skip). */
static int skip_internal(const char *path, const char *rel) {
    (void)path;
    if (!strncmp(rel, "PS4Mount", 8) && (!rel[8] || rel[8] == '/'))
        return 0;
    return strncmp(rel, "ps4", 3) || (rel[3] && rel[3] != '/');
}

static void test_skip_dir(void) {
    printf("catalog: folders left out of the walk\n");
    char root[] = "/tmp/ps4mount-cat-XXXXXX";
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    pkg_at(root, "PS4Mount/game.pkg", "UP0000-CUSA00021_00-SKIP000000000000", "gd", 1, 0x4000, 0x4000, "01.00");
    pkg_at(root, "PS4Mount/Overcooked/deep.pkg", "UP0000-CUSA00022_00-SKIP000000000000", "gd", 1, 0x4000, 0x4000,
           "01.00");
    pkg_at(root, "ps4/CUSA00023/app.pkg", "UP0000-CUSA00023_00-SKIP000000000000", "gd", 1, 0x4000, 0x4000, "01.00");
    /* never searched: the rest of /data, at any depth */
    pkg_at(root, "user/savedata/x.pkg", "UP0000-CUSA00024_00-SKIP000000000000", "gd", 1, 0x4000, 0x4000, "01.00");
    pkg_at(root, "system/y.pkg", "UP0000-CUSA00025_00-SKIP000000000000", "gd", 1, 0x4000, 0x4000, "01.00");

    catalog_t c = {0};
    CHECK(catalog_walk(&c, root, NULL, skip_internal, NULL, 0) == 3);
    CHECK(c.npkgs == 3);
    int found_ps4mount = 0, found_legacy = 0, found_deep = 0;
    for (int i = 0; i < c.npkgs; i++) {
        const char *p = c.pkgs[i].path;
        found_deep |= !!strstr(p, "/PS4Mount/Overcooked/deep.pkg");
        found_ps4mount |= !!strstr(p, "/PS4Mount/game.pkg");
        found_legacy |= !!strstr(p, "/ps4/CUSA00023/app.pkg");
        CHECK(!strstr(p, "/user/") && !strstr(p, "/system/"));
    }
    CHECK(found_ps4mount && found_deep && found_legacy);
    CHECK(catalog_read(&c) == 3);
    catalog_group(&c, NULL);
    CHECK(c.ngames == 3);
    catalog_free(&c);

    /* without the filter the whole tree is searched, as on a USB drive */
    catalog_t d = {0};
    CHECK(catalog_walk(&d, root, NULL, NULL, NULL, 0) == 5);
    catalog_free(&d);
    cleanup(root);
}

static const char *choose_105(const char *tid) {
    (void)tid;
    return "01.05";
}

static const char *choose_none(const char *tid) {
    (void)tid;
    return CAT_NO_UPDATE;
}

static void test_choose(void) {
    printf("catalog: update chosen and cross-drive\n");
    char root[] = "/tmp/ps4mount-cat-XXXXXX";
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    pkg_at(root, "A/game.pkg", GAME_CID, "gd", 1, 0x4000, 0x4000, "01.00");
    pkg_at(root, "A/u1.pkg", GAME_CID, "gp", 1, 0x4000, 0x4000, "01.05");
    pkg_at(root, "A/u2.pkg", GAME_CID, "gp", 1, 0x4000, 0x4000, "01.07");
    catalog_t c = {0};
    catalog_walk(&c, root, NULL, NULL, NULL, 0);
    catalog_read(&c);

    /* highest by default */
    catalog_group(&c, NULL);
    const cat_game_t *g = catalog_game(&c, "CUSA00001");
    CHECK(g && g->patch >= 0 && !strcmp(c.pkgs[g->patch].app_ver, "01.07"));

    /* a chosen update, and none */
    catalog_group(&c, choose_105);
    g = catalog_game(&c, "CUSA00001");
    CHECK(g && g->patch >= 0 && !strcmp(c.pkgs[g->patch].app_ver, "01.05"));
    catalog_group(&c, choose_none);
    g = catalog_game(&c, "CUSA00001");
    CHECK(g && g->patch == -1 && catalog_patch(&c, &c, g) == NULL);
    catalog_free(&c);
    cleanup(root);

    /* the update on another drive goes with the game when linking */
    char other[] = "/tmp/ps4mount-cat-XXXXXX";
    if (!mkdtemp(other)) {
        CHECK(0);
        cleanup(root);
        return;
    }
    pkg_at(root, "A/game.pkg", GAME_CID, "gd", 1, 0x4000, 0x4000, "01.00");
    pkg_at(other, "B/u.pkg", GAME_CID, "gp", 1, 0x4000, 0x4000, "01.06");
    catalog_t all[2] = {{0}};
    catalog_walk(&all[0], root, NULL, NULL, NULL, 0);
    catalog_walk(&all[1], other, NULL, NULL, NULL, 0);
    catalog_read(&all[0]);
    catalog_read(&all[1]);
    catalog_group(&all[0], NULL);
    catalog_group(&all[1], NULL);
    g = catalog_game(&all[0], "CUSA00001");
    CHECK(g && g->patch == -1);
    CHECK(all[1].pkgs[0].role == CAT_ROLE_NOGAME);
    catalog_link(all, 2, NULL, 1);
    g = catalog_game(&all[0], "CUSA00001");
    const cat_pkg_t *patch = g ? catalog_patch(all, &all[0], g) : NULL;
    CHECK(patch != NULL && !strcmp(patch->app_ver, "01.06"));
    CHECK(all[1].pkgs[0].role == CAT_ROLE_PATCH);
    catalog_free(&all[0]);
    catalog_free(&all[1]);
    cleanup(root);
    cleanup(other);
}

int main(void) {
    test_scan();
    test_skip_dir();
    test_choose();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
