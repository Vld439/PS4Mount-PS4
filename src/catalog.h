/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Catalog of the PS4 pkgs on a drive, wherever they are, grouped per game.
 * Ported from EzioRamesha's fork (release/v0.6.1-test.1), where the pkgs stay in
 * place instead of being moved into <root>/ps4/<TITLE_ID>/. */
#pragma once

#include <limits.h>
#include <stdint.h>

#define CAT_MAX_DLC 64
#define CAT_MAX_SERVICES 7
#define CAT_MAX_DEPTH 16

#define CAT_MAX_UPDATES 8

/* CAT_SHORT: a PS4 pkg smaller than its header says, still being copied or cut off. It is
 * read again when its size or time changes, and never saved in the cache. */
enum { CAT_UNREAD, CAT_BAD, CAT_OK, CAT_SHORT };

/* What a pkg is used for, set by catalog_group. */
enum {
    CAT_ROLE_NONE,     /* not grouped: unread or unusable */
    CAT_ROLE_APP,      /* the game pkg of a game */
    CAT_ROLE_PATCH,    /* the update used */
    CAT_ROLE_DLC,      /* an add-on of a game */
    CAT_ROLE_DUPE,     /* a second copy of a game pkg */
    CAT_ROLE_ORPHAN,   /* an add-on whose game is not on the drive */
    CAT_ROLE_SPARE,    /* another update of a game, not used */
    CAT_ROLE_NOGAME,   /* an update whose game is not on the drive */
    CAT_ROLE_INCLUDED, /* an update not newer than the game pkg: a merged base+update
                        * already has it, and using it would take the game back */
};

/* One .pkg file. Its details are read once and kept while its size and time stay the same. */
typedef struct {
    char path[PATH_MAX];
    uint64_t size;
    uint64_t full_size;                   /* CAT_SHORT: the size the header gives */
    int64_t mtime;
    int state;                            /* CAT_UNREAD, CAT_BAD (not a fake PS4 pkg), CAT_OK or CAT_SHORT */
    char category[4];                     /* gd game, gp update, ac add-on */
    char title_id[10], content_id[37], app_ver[8];
    char name[64];                        /* add-ons: TITLE, for the panel */
    char services[CAT_MAX_SERVICES][20];  /* add-on content ID prefixes a game or update lists */
    int nservices;
    int seen;                             /* found by the current walk */
    int role;                             /* CAT_ROLE_* */
} cat_pkg_t;

/* A pkg of another drive: index of its catalog in the array given to catalog_link, and
 * index in that catalog's pkgs. */
typedef struct {
    int drive, pkg;
} cat_ref_t;

/* A game with its update and add-ons: indexes in catalog_t.pkgs. catalog_link may add
 * updates and add-ons of other drives (x...). Use catalog_patch, catalog_updates_of and
 * catalog_dlc_of to see both. */
typedef struct {
    char title_id[10];
    int app, patch; /* patch is -1 without an update, or when the update used is xpatch */
    int updates[CAT_MAX_UPDATES], nupdates; /* every update found, the one used included */
    int dlc[CAT_MAX_DLC], ndlc;
    cat_ref_t xupdates[CAT_MAX_UPDATES];
    int nxupdates;
    int xpatch; /* index in xupdates of the update used, -1 for none */
    cat_ref_t xdlc[CAT_MAX_DLC];
    int nxdlc;
} cat_game_t;

/* The APP_VER of the update the user chose for a title, CAT_NO_UPDATE for none, or NULL
 * for the highest version (also used when the chosen one is not on the drive). */
typedef const char *(*cat_choose_fn)(const char *title_id);
#define CAT_NO_UPDATE "none"

typedef struct {
    cat_pkg_t *pkgs; /* sorted by path */
    int npkgs, cap;
    cat_game_t *games;
    int ngames, gcap;
    int dupes;    /* game pkgs not used: an earlier copy of the same game is used */
    int orphans;  /* add-ons whose game is not on the drive */
    int included; /* updates not newer than their game (merged base+update pkgs) */
} catalog_t;

/* Returns 1 for a pkg the walk must leave out (path is absolute). */
typedef int (*cat_skip_fn)(const char *path, uint64_t size);
/* Returns 1 for a folder the walk must not enter (path absolute, rel below root). */
typedef int (*cat_skip_dir_fn)(const char *path, const char *rel);

/* Finds every .pkg under root (any depth up to CAT_MAX_DEPTH), skipping hidden and system
 * folders, the old updates PS4Mount keeps in ps4/<TID>/replaced, the folders skip_dir
 * rejects and the folders of ignore (paths below root, any case). Pkgs that are gone are
 * dropped; new or changed ones become CAT_UNREAD. Returns how many are unread. */
int catalog_walk(catalog_t *c, const char *root, cat_skip_fn skip, cat_skip_dir_fn skip_dir,
                 const char *const *ignore, int nignore);
/* Reads the details of the unread pkgs. Returns how many were read. */
int catalog_read(catalog_t *c);
/* Groups the readable pkgs into games: the first game pkg of a title (by path), its update
 * (the one choose names, else the highest APP_VER; one not newer than the game is left
 * out), and its add-ons (same title ID, or listed by the game or update in
 * SERVICE_ID_ADDCONT_ADD_n, as add-ons of another region). Sets the role of every pkg.
 * choose may be NULL. */
void catalog_group(catalog_t *c, cat_choose_fn choose);
const cat_game_t *catalog_game(const catalog_t *c, const char *title_id);
/* Groups each of the n catalogs of all again; with cross, updates and add-ons whose game
 * is not on their drive then go with that game on another drive (every copy of it), and
 * the update of each game is picked among the updates of every drive. */
void catalog_link(catalog_t *all, int n, cat_choose_fn choose, int cross);
/* Drops what catalog_link added from other drives (before walking a drive, whose pkgs
 * then move). Roles stay as they were until the next catalog_link. */
void catalog_unlink(catalog_t *all, int n);
/* The update g uses (c is the catalog of g, all the array given to catalog_link), or NULL. */
const cat_pkg_t *catalog_patch(const catalog_t *all, const catalog_t *c, const cat_game_t *g);
/* Every update (or add-on) of g, of its drive first: fills out (at most max), returns how many. */
int catalog_updates_of(const catalog_t *all, const catalog_t *c, const cat_game_t *g, const cat_pkg_t **out, int max);
int catalog_dlc_of(const catalog_t *all, const catalog_t *c, const cat_game_t *g, const cat_pkg_t **out, int max);
/* Saves what was read of each pkg, so a restart does not read them all again. 0 on success. */
int catalog_save(const catalog_t *c, const char *path);
/* Replaces c with a saved catalog (pkgs only: group it again). The next walk keeps the
 * pkgs whose size and time did not change. Returns how many were loaded, -1 without a file. */
int catalog_load(catalog_t *c, const char *path);
void catalog_free(catalog_t *c);
