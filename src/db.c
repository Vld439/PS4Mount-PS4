/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Read-only lookups in the system databases. */
/* The system databases are written by the system alone: ShellCore keeps them open
 * with its own cache, and rows written from outside make it flag them as corrupted. */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sqlite3.h"

#ifndef APP_DB
#define APP_DB "/system_data/priv/mms/app.db"
#endif

/* Titles registered in app.db, read in one query at most once a second. The file is
 * opened without locks (nolock=1): a shared lock held here, even for a moment, can make
 * a write of ShellCore fail, and ShellCore then flags the database as corrupted. */
typedef struct {
    char tid[10];
    char ver[8]; /* APP_VER the system registered ("" when it has none) */
} title_row_t;

static title_row_t *g_titles;
static int g_ntitles, g_cap;
static time_t g_read_at;
static int g_read_ok;

static void refresh(void) {
    if (g_read_ok && time(NULL) == g_read_at)
        return;
    g_read_at = time(NULL);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2("file:" APP_DB "?mode=ro&nolock=1", &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return; /* keep the last list */
    }
    sqlite3_stmt *st;
    int n = 0, ok = 0;
    /* the PS4 keeps one row per title and key in tbl_appinfo (the PS5 has tbl_contentinfo) */
    if (sqlite3_prepare_v2(db, "SELECT a.titleId, b.val FROM tbl_appinfo a LEFT JOIN tbl_appinfo b "
                               "ON b.titleId = a.titleId AND b.key = 'APP_VER' WHERE a.key = 'TITLE_ID'",
                           -1, &st, NULL) == SQLITE_OK) {
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(st, 0), *v = (const char *)sqlite3_column_text(st, 1);
            if (!t)
                continue;
            if (n == g_cap) {
                int cap = g_cap ? g_cap * 2 : 256;
                title_row_t *grown = realloc(g_titles, sizeof(*grown) * (size_t)cap);
                if (!grown)
                    break;
                g_titles = grown;
                g_cap = cap;
            }
            snprintf(g_titles[n].tid, sizeof(g_titles[n].tid), "%s", t);
            snprintf(g_titles[n].ver, sizeof(g_titles[n].ver), "%s", v ? v : "");
            n++;
        }
        ok = rc == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (ok) { /* a read that met a write in progress keeps the last list */
        g_ntitles = n;
        g_read_ok = 1;
    }
}

int db_is_registered(const char *title_id) {
    return db_app_ver(title_id) != NULL;
}

const char *db_app_ver(const char *title_id) {
    refresh();
    for (int i = 0; i < g_ntitles; i++)
        if (!strcmp(g_titles[i].tid, title_id))
            return g_titles[i].ver;
    return NULL;
}
