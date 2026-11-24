/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What to do with a file in the folder where pkgs sent from the panel wait for the
 * system's installer (INSTALL_DIR). This is the only place that decides when one of
 * PS4Mount's own copies may be deleted: the timings live here so a test can hold them. */
#pragma once

#include <string.h>
#include <time.h>

/* A pkg is looked at for deletion once it is this old (seconds): the system has had time
 * to take it, and the checks in install_cleanup (registered, whole app.pkg of the same
 * size) say whether it did. */
#define INSTALL_KEEP_SECONDS 600
/* A pkg that is still there after this long was never installed (refused, cancelled,
 * paused for good). A paused install of a big game can wait days: this is long. */
#define INSTALL_ABANDON_SECONDS (3 * 86400)
/* The unfinished copy of a pkg being received: deleted only when no transfer is running. */
#define INSTALL_PART_STALE_SECONDS 7200

typedef enum {
    STAGE_KEEP = 0,       /* leave it: not ours, or too young */
    STAGE_DELETE_PART,    /* an unfinished transfer nobody is writing */
    STAGE_DELETE_ABANDONED, /* a whole pkg the system never installed */
    STAGE_VERIFY,         /* old enough: delete only if the system has the whole app */
} stage_action_t;

/* name: the file's name in INSTALL_DIR; age: seconds since it was last written;
 * receiving: 1 while a pkg is being received from the panel. */
static inline stage_action_t stage_action(const char *name, long age, int receiving) {
    size_t l = strlen(name);
    if (l >= 5 && !strcmp(name + l - 5, ".part"))
        return !receiving && age >= INSTALL_PART_STALE_SECONDS ? STAGE_DELETE_PART : STAGE_KEEP;
    if (l < 5 || strcmp(name + l - 4, ".pkg"))
        return STAGE_KEEP; /* anything else is not ours to delete */
    if (age >= INSTALL_ABANDON_SECONDS)
        return STAGE_DELETE_ABANDONED;
    return age >= INSTALL_KEEP_SECONDS ? STAGE_VERIFY : STAGE_KEEP;
}
