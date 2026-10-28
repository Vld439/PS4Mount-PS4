/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The title cache: what each game is, kept between looks and between runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "titlecache.h"
#include "mem.h"

/* One entry per game of a drive (a library of 500 games fits): the array comes from
 * mapped pages when the first game is cached, never from the host's few MB of heap. */
#define MAX_ENTRIES 512
#define MAX_ROOT 128
#define VERSION "#titles 1"
#define LINE_MAX_LEN 16384

typedef struct {
    int used, dirty; /* dirty: read or changed since the file of the drive was written */
    char root[MAX_ROOT];
    uint64_t fp;
    unsigned long touched;
    title_info_t info;
} entry_t;

static entry_t *g_entries;
static int g_n;
static unsigned long g_clock;
static int g_loading; /* the entries come from the file: nothing to write back */

uint64_t title_fp_start(void) { return 0xcbf29ce484222325ull; }

/* FNV-1a over the path and the bytes of size and mtime. */
uint64_t title_fp_add(uint64_t h, const char *path, uint64_t size, int64_t mtime) {
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        h = (h ^ *p) * 0x100000001b3ull;
    h = (h ^ 0x1f) * 0x100000001b3ull;
    uint64_t v[2] = {size, (uint64_t)mtime};
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 8; i++)
            h = (h ^ ((v[k] >> (i * 8)) & 0xff)) * 0x100000001b3ull;
    return h;
}

static int same_game(const entry_t *e, const char *root, const char *tid) {
    return e->used && !strcmp(e->root, root) && !strcmp(e->info.title_id, tid);
}

const title_info_t *title_cache_find(const char *root, const char *tid, uint64_t fp) {
    for (int i = 0; i < g_n; i++) {
        entry_t *e = &g_entries[i];
        if (same_game(e, root, tid) && e->fp == fp) {
            e->touched = ++g_clock;
            return &e->info;
        }
    }
    return NULL;
}

title_info_t *title_cache_add(const char *root, const char *tid, uint64_t fp) {
    if (!g_entries && !(g_entries = mem_alloc(MAX_ENTRIES * sizeof(*g_entries))))
        return NULL;
    if (!g_n)
        g_n = MAX_ENTRIES;
    entry_t *e = NULL;
    for (int i = 0; i < g_n && !e; i++)
        if (same_game(&g_entries[i], root, tid))
            e = &g_entries[i];
    for (int i = 0; i < g_n && !e; i++)
        if (!g_entries[i].used)
            e = &g_entries[i];
    if (!e) { /* full: the one looked at longest ago goes */
        e = &g_entries[0];
        for (int i = 1; i < g_n; i++)
            if (g_entries[i].touched < e->touched)
                e = &g_entries[i];
    }
    if (e->info.dlc)
        mem_free(e->info.dlc);
    memset(&e->info, 0, sizeof(e->info));
    e->used = 1;
    e->dirty = !g_loading;
    e->fp = fp;
    e->touched = ++g_clock;
    snprintf(e->root, sizeof(e->root), "%s", root);
    snprintf(e->info.title_id, sizeof(e->info.title_id), "%s", tid);
    return &e->info;
}

title_dlc_t *title_cache_give_dlc(title_info_t *e, int n) {
    if (e->dlc || n <= 0)
        return e->dlc;
    if (n > TITLE_DLC_MAX)
        n = TITLE_DLC_MAX;
    e->dlc = mem_alloc((size_t)n * sizeof(*e->dlc));
    if (e->dlc)
        memset(e->dlc, 0, (size_t)n * sizeof(*e->dlc));
    return e->dlc;
}

void title_cache_forget(void) {
    if (!g_entries)
        return;
    for (int i = 0; i < g_n; i++) {
        if (g_entries[i].used && g_entries[i].info.dlc)
            mem_free(g_entries[i].info.dlc);
        g_entries[i].used = 0;
        g_entries[i].info.dlc = NULL;
    }
}

/* ---- the file of the last run ---- */

/* A text field as it goes in the file: a tab or a newline would break the line, and the
 * marker has newlines of its own. */
static void esc_text(char *out, size_t n, const char *in) {
    size_t o = 0;
    for (; *in && o + 3 < n; in++) {
        const char *e = *in == '\\' ? "\\\\" : *in == '\n' ? "\\n" : *in == '\t' ? "\\t" : *in == '\r' ? "\\r" : NULL;
        if (e)
            while (*e && o + 1 < n)
                out[o++] = *e++;
        else
            out[o++] = *in;
    }
    out[o] = 0;
}

/* The other way round, in place. */
static void unesc_text(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            *o++ = *s;
            continue;
        }
        s++;
        *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
    }
    *o = 0;
}

static void sha_hex(char *out, const unsigned char *sha, size_t n) {
    for (size_t i = 0; i < n; i++)
        snprintf(out + i * 2, 3, "%02x", sha[i]);
}

static int unhex(const char *s, unsigned char *out, size_t n) {
    if (strlen(s) != n * 2)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + i * 2, "%2x", &v) != 1)
            return -1;
        out[i] = (unsigned char)v;
    }
    return 0;
}

int title_cache_save(const char *file, const char *root) {
    if (!g_entries)
        return 0;
    int dirty = 0;
    for (int i = 0; i < g_n; i++)
        dirty |= g_entries[i].used && g_entries[i].dirty && !strcmp(g_entries[i].root, root);
    if (!dirty)
        return 0;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "%s\n", VERSION);
    for (int i = 0; i < g_n; i++) {
        entry_t *e = &g_entries[i];
        if (!e->used || strcmp(e->root, root))
            continue;
        title_info_t *t = &e->info;
        char dir[PATH_MAX * 2], app[PATH_MAX * 2], patch[PATH_MAX * 2], issue[512];
        char marker[(PATH_MAX + 160) * 2], name[256];
        esc_text(dir, sizeof(dir), t->dir);
        esc_text(app, sizeof(app), t->app_path);
        esc_text(patch, sizeof(patch), t->patch_path);
        esc_text(issue, sizeof(issue), t->update_issue);
        esc_text(marker, sizeof(marker), t->marker);
        esc_text(name, sizeof(name), t->name);
        char tids[1 + TITLE_DLC_TIDS * 11], sha[65], dlcs[TITLE_DLC_MAX * 64];
        tids[0] = 0;
        for (int k = 0; k < t->ndlc_tids && k < TITLE_DLC_TIDS; k++)
            snprintf(tids + strlen(tids), sizeof(tids) - strlen(tids), "%s%s", k ? "," : "", t->dlc_tids[k]);
        sha_hex(sha, t->sfo_sha, sizeof(t->sfo_sha));
        dlcs[0] = 0;
        for (int k = 0; k < t->ndlc && k < TITLE_DLC_MAX; k++)
            snprintf(dlcs + strlen(dlcs), sizeof(dlcs) - strlen(dlcs), "%s%s:%s:%d", k ? "," : "",
                     t->dlc[k].title_id, t->dlc[k].content_id, t->dlc[k].install);
        fprintf(f, "%s\t%s\t%llx\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%u\t%llu\t%llu\t%s\t%s\t%u\t%s\t%d\t%s\t%d\t%s\n",
                e->root, t->title_id, (unsigned long long)e->fp, dir, app, patch, t->has_patch, name, t->ver,
                t->game_ver, t->content_id, t->system_ver, (unsigned long long)t->app_size,
                (unsigned long long)t->patch_size, issue, marker, t->sfo_len, sha, t->ndlc_tids, tids, t->ndlc, dlcs);
    }
    int rc = fclose(f);
    if (rc || rename(tmp, file)) {
        unlink(tmp);
        return -1;
    }
    for (int i = 0; i < g_n; i++)
        if (g_entries[i].used && !strcmp(g_entries[i].root, root))
            g_entries[i].dirty = 0;
    return 0;
}

/* The next tab-separated field of *p (empty ones included), NULL when there is none. */
static char *field(char **p) {
    if (!*p)
        return NULL;
    char *s = *p, *t = strchr(s, '\t');
    if (t) {
        *t = 0;
        *p = t + 1;
    } else {
        *p = NULL;
    }
    return s;
}

/* One line of the file into the cache; -1 when it is damaged (the game is read again). */
static int load_line(char *line) {
    char *p = line, *f[22];
    for (int i = 0; i < 22; i++)
        if (!(f[i] = field(&p)))
            return -1;
    if (p)
        return -1; /* more fields than a game has: another version, or a damaged line */
    if (!f[0][0] || !f[1][0] || strlen(f[1]) > 9)
        return -1;
    for (int i = 3; i <= 5; i++)
        unesc_text(f[i]);
    for (int i = 7; i <= 10; i++)
        unesc_text(f[i]);
    unesc_text(f[14]);
    unesc_text(f[15]);
    char *end = NULL;
    unsigned long long fp = strtoull(f[2], &end, 16);
    if (!end || *end || !fp)
        return -1;
    long n = atol(f[20]);
    long ntids = atol(f[18]);
    long sfo = atol(f[16]);
    if (n < 0 || n > TITLE_DLC_MAX || ntids < 0 || ntids > TITLE_DLC_TIDS || sfo < 0 || sfo > 65536)
        return -1;
    unsigned char sha[32];
    if (unhex(f[17], sha, sizeof(sha)))
        return -1;
    title_info_t *t = title_cache_add(f[0], f[1], (uint64_t)fp);
    if (!t)
        return -1;
    snprintf(t->dir, sizeof(t->dir), "%s", f[3]);
    snprintf(t->app_path, sizeof(t->app_path), "%s", f[4]);
    snprintf(t->patch_path, sizeof(t->patch_path), "%s", f[5]);
    t->has_patch = atoi(f[6]) != 0;
    snprintf(t->name, sizeof(t->name), "%s", f[7]);
    snprintf(t->ver, sizeof(t->ver), "%s", f[8]);
    snprintf(t->game_ver, sizeof(t->game_ver), "%s", f[9]);
    snprintf(t->content_id, sizeof(t->content_id), "%s", f[10]);
    t->system_ver = (uint32_t)strtoul(f[11], NULL, 10);
    t->app_size = strtoull(f[12], NULL, 10);
    t->patch_size = strtoull(f[13], NULL, 10);
    snprintf(t->update_issue, sizeof(t->update_issue), "%s", f[14]);
    snprintf(t->marker, sizeof(t->marker), "%s", f[15]);
    t->sfo_len = (uint32_t)sfo;
    memcpy(t->sfo_sha, sha, sizeof(sha));
    for (int k = 0; k < (int)ntids; k++) {
        char *comma = strchr(f[19], ',');
        size_t l = comma ? (size_t)(comma - f[19]) : strlen(f[19]);
        if (!l || l >= sizeof(t->dlc_tids[0]))
            return 0; /* the rest is dropped: the game is checked again by its files */
        memcpy(t->dlc_tids[k], f[19], l);
        t->dlc_tids[k][l] = 0;
        f[19] = comma ? comma + 1 : f[19] + l;
    }
    t->ndlc_tids = (int)ntids;
    if (n) {
        title_dlc_t *d = title_cache_give_dlc(t, (int)n);
        if (!d)
            return -1;
        char *s = f[21];
        for (int k = 0; k < (int)n; k++) {
            char *comma = strchr(s, ','), *c1, *c2;
            size_t l = comma ? (size_t)(comma - s) : strlen(s);
            char item[64];
            if (!l || l >= sizeof(item))
                return 0;
            memcpy(item, s, l);
            item[l] = 0;
            c1 = strchr(item, ':');
            c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (!c1 || !c2)
                return 0;
            *c1 = *c2 = 0;
            size_t tl = strlen(item), cl = strlen(c1 + 1);
            if (tl > 9 || cl > 36)
                return 0;
            memcpy(d[k].title_id, item, tl + 1);
            memcpy(d[k].content_id, c1 + 1, cl + 1);
            d[k].install = atoi(c2 + 1) != 0;
            s = comma ? comma + 1 : s + l;
        }
        t->ndlc = (int)n;
    }
    return 0;
}

int title_cache_load(const char *file) {
    FILE *f = fopen(file, "r");
    if (!f)
        return -1;
    static char line[LINE_MAX_LEN];
    int seen_version = 0;
    g_loading = 1;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (!l)
            continue;
        if (line[0] == '#') {
            if (strcmp(line, VERSION))
                break; /* another version: the whole file is ignored */
            seen_version = 1;
            continue;
        }
        /* a damaged line is left out: the game is read again when it is asked for */
        if (seen_version)
            load_line(line);
    }
    fclose(f);
    g_loading = 0;
    return seen_version ? 0 : -1;
}
