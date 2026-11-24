/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Emulators: a folder of a drive, <drive>/PS4Mount/emulators/<App>/<Folder> (the ROMs, the
 * BIOS), is shown inside the console as /data/<App>/<Folder>, where an emulator app looks
 * for it, so gigabytes of games stay on the drive. No setting: the folder names are the
 * setting. What may be named is decided here, so a test holds it: a name from a drive is
 * input from outside, and the result is a mount over a folder of the console. */
#pragma once

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define EMU_DIR "PS4Mount/emulators"

/* A name PS4Mount may use for a folder: letters, digits, space, dot, dash and underscore,
 * 1 to 48 bytes, not starting with a dot (so never "." or ".."), and no slash. */
static inline int emu_name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 48 || s[0] == '.')
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
                 c == '-' || c == '_';
        if (!ok)
            return 0;
    }
    return 1;
}

/* The folders of /data that are PS4Mount's, the system installer's or a payload's: never the
 * app side of an emulator mount (PS4Mount's own state, its internal library, the pkgs of the
 * installer, GoldHEN and the payloads). */
static inline int emu_app_reserved(const char *app) {
    static const char *const reserved[] = {"ps4mount", "ps4", "pkg", "goldhen", "payloads"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        if (!strcasecmp(app, reserved[i]))
            return 1;
    return 0;
}

/* Where <App>/<Folder> goes: "/data/<App>/<Folder>" in out. 0 when it may be mounted, -1 when
 * a name is not allowed or the app is reserved. */
static inline int emu_target(const char *app, const char *folder, char *out, size_t n) {
    if (!emu_name_ok(app) || !emu_name_ok(folder) || emu_app_reserved(app))
        return -1;
    return snprintf(out, n, "/data/%s/%s", app, folder) < (int)n ? 0 : -1;
}

/* A path that is a drive's emulators folder, "<drive>/PS4Mount/emulators". It is the drive's
 * ROMs and BIOS, not games: the searches for pkgs and the tidying of the scan folder leave it
 * alone (an empty ROM folder would be taken for a game folder left empty). */
static inline int emu_is_dir(const char *path) {
    size_t n = strlen(path), m = strlen("/" EMU_DIR);
    return n >= m && !strcasecmp(path + n - m, "/" EMU_DIR);
}
