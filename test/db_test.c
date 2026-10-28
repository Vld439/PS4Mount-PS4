/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The system database is only ever read: never written, never locked, never created.
 * Built with APP_DB pointing at a test file. Run with: bash test/run.sh */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "crypto.h"
#include "db.h"
#include "sqlite3.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

static int exists(const char *p) {
    struct stat st;
    return !stat(p, &st);
}

static void file_hash(const char *path, uint8_t out[32]) {
    memset(out, 0, 32);
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    sha256(buf, n, out);
}

static void exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        printf("  setup failed: %s: %s\n", sql, err);
        exit(2);
    }
}

/* Waits for the next second: db.c reads the list at most once a second. */
static void next_second(void) {
    time_t t = time(NULL);
    while (time(NULL) == t)
        usleep(20000);
}

static double now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(void) {
    unlink(APP_DB);
    printf("missing database\n");
    CHECK(!db_is_registered("CUSA00001"));
    CHECK(!db_ready()); /* no database read: nothing may be taken as unregistered */
    CHECK(!exists(APP_DB)); /* opening read-only never creates it */

    printf("titles\n");
    sqlite3 *w;
    CHECK(sqlite3_open(APP_DB, &w) == SQLITE_OK);
    exec(w, "CREATE TABLE tbl_appinfo(titleId TEXT, key INT, val);"
            "INSERT INTO tbl_appinfo VALUES('CUSA00001','TITLE_ID','CUSA00001'),('CUSA00001','TITLE','A'),"
            "('CUSA00001','APP_VER','01.04'),"
            "('CUSA00002','TITLE_ID','CUSA00002'),('CUSA00003','TITLE','C');"); /* CUSA00003: no TITLE_ID row */
    next_second();
    CHECK(db_is_registered("CUSA00001"));
    CHECK(db_ready());
    CHECK(db_is_registered("CUSA00002"));
    CHECK(!db_is_registered("CUSA00003"));
    CHECK(!db_is_registered(""));
    CHECK(!db_is_registered("CUSA0000")); /* no prefix match */
    CHECK(db_app_ver("CUSA00001") && !strcmp(db_app_ver("CUSA00001"), "01.04"));
    CHECK(db_app_ver("CUSA00002") && !strcmp(db_app_ver("CUSA00002"), "")); /* registered, no APP_VER */
    CHECK(!db_app_ver("CUSA00003"));

    printf("never written or locked\n");
    uint8_t before[32], after[32];
    struct stat sb, sa;
    file_hash(APP_DB, before);
    stat(APP_DB, &sb);
    for (int i = 0; i < 3; i++) {
        next_second();
        db_is_registered("CUSA00001");
    }
    file_hash(APP_DB, after);
    stat(APP_DB, &sa);
    CHECK(!memcmp(before, after, 32));
    CHECK(sa.st_mtime == sb.st_mtime && sa.st_size == sb.st_size);
    CHECK(!exists(APP_DB "-journal") && !exists(APP_DB "-wal"));
    /* the system writes right after a read: nothing of ours may be holding a lock */
    CHECK(sqlite3_exec(w, "BEGIN IMMEDIATE; INSERT INTO tbl_appinfo VALUES('CUSA00004','TITLE_ID','x'); COMMIT;", NULL, NULL,
                       NULL) == SQLITE_OK);

    printf("one read per second\n");
    /* the delete must land in the same second as the read: a slow disk can push it into the
     * next one, so the check is tried a few times */
    int cached = -1;
    for (int attempt = 0; attempt < 5 && cached < 0; attempt++) {
        exec(w, "INSERT INTO tbl_appinfo VALUES('CUSA00005','TITLE_ID','x');");
        next_second();
        time_t t = time(NULL);
        int first = db_is_registered("CUSA00005");
        exec(w, "DELETE FROM tbl_appinfo WHERE titleId='CUSA00005';");
        int second = db_is_registered("CUSA00005");
        if (time(NULL) == t) {
            CHECK(first);
            cached = second; /* same second: the cached list */
        }
    }
    CHECK(cached == 1);
    next_second();
    CHECK(!db_is_registered("CUSA00005"));

    printf("a write in progress does not block or empty the list\n");
    exec(w, "BEGIN EXCLUSIVE; DELETE FROM tbl_appinfo;");
    next_second();
    double t0 = now();
    int seen = db_is_registered("CUSA00001");
    CHECK(now() - t0 < 1.0); /* nolock: no busy wait */
    (void)seen;
    exec(w, "ROLLBACK;");
    next_second();
    CHECK(db_is_registered("CUSA00001"));
    sqlite3_close(w);

    printf("a damaged database\n");
    FILE *f = fopen(APP_DB, "r+b");
    if (f) {
        fseek(f, 0, SEEK_SET);
        fwrite("garbage garbage garbage garbage!", 1, 32, f);
        fclose(f);
    }
    file_hash(APP_DB, before);
    next_second();
    db_is_registered("CUSA00001"); /* keeps the last list, no crash */
    file_hash(APP_DB, after);
    CHECK(!memcmp(before, after, 32)); /* never "repaired" by us */

    unlink(APP_DB);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}
