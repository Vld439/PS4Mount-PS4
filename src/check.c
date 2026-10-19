/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What a pkg is and whether this console can use it, read from its first bytes. Sending a
 * pkg of tens of GB takes hours: what makes it unusable is known from the start. */
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lang.h"
#include "pkg.h"
#include "mem.h"

void ver_text(uint32_t v, char *out, size_t n) {
    unsigned major = ((v >> 28) & 0xf) * 10 + ((v >> 24) & 0xf), minor = ((v >> 20) & 0xf) * 10 + ((v >> 16) & 0xf);
    snprintf(out, n, "%u.%02u", major, minor);
}

int fw_runs(uint32_t system_ver, uint32_t fw) { return (system_ver >> 16) <= (fw >> 16); }

const char *pkg_kind_name(int kind) {
    return kind == PKG_KIND_GAME ? "game" : kind == PKG_KIND_UPDATE ? "update" : kind == PKG_KIND_ADDON ? "add-on"
                                                                                                         : "other";
}

static void str_item(const sfo_t *sfo, const char *key, char *out, size_t n) {
    const sfo_item_t *it = sfo_get(sfo, key);
    snprintf(out, n, "%s", it && !it->is_int ? it->sval : "");
}

int pkg_check(const char *path, uint32_t fw, pkg_check_t *out) {
    memset(out, 0, sizeof(*out));
    pkg_t *pkg = mem_alloc(sizeof(*pkg)); /* large, and the panel's uploads check from their own thread */
    if (!pkg || pkg_open(pkg, path)) {
        mem_free(pkg);
        return -1;
    }
    snprintf(out->content_id, sizeof(out->content_id), "%s", pkg->content_id);
    snprintf(out->title_id, sizeof(out->title_id), "%s", pkg->title_id);
    out->pkg_size = pkg->pkg_size;
    out->fake = pkg_fake_key(pkg);
    size_t len;
    uint8_t *d = pkg_entry(pkg, 0x1000, &len);
    sfo_t *sfo = mem_alloc(sizeof(*sfo));
    int have_sfo = d && sfo && !sfo_parse(d, len, sfo);
    mem_free(d);
    pkg_close(pkg);
    mem_free(pkg);
    if (!have_sfo) {
        mem_free(sfo);
        snprintf(out->problem, sizeof(out->problem), "%s",
                 TR("param.sfo cannot be read: the pkg is damaged", "no se puede leer param.sfo: el pkg está dañado"));
        return 0;
    }
    char cat[8];
    str_item(sfo, "CATEGORY", cat, sizeof(cat));
    out->kind = !strcmp(cat, "gd") ? PKG_KIND_GAME : !strcmp(cat, "gp") ? PKG_KIND_UPDATE
                                                    : !strcmp(cat, "ac") ? PKG_KIND_ADDON
                                                                         : PKG_KIND_OTHER;
    str_item(sfo, "TITLE", out->title, sizeof(out->title));
    str_item(sfo, "APP_VER", out->app_ver, sizeof(out->app_ver));
    str_item(sfo, "VERSION", out->version, sizeof(out->version));
    const sfo_item_t *sv = sfo_get(sfo, "SYSTEM_VER");
    out->system_ver = sv && sv->is_int ? sv->ival : 0;
    mem_free(sfo);

    char need[16], have[16];
    if (out->kind == PKG_KIND_OTHER)
        snprintf(out->problem, sizeof(out->problem),
                 TR("not a game, an update or an add-on (category \"%s\")",
                    "no es un juego, una actualización ni un DLC (categoría \"%s\")"),
                 cat);
    else if (strncmp(out->title_id, "CUSA", 4))
        snprintf(out->problem, sizeof(out->problem),
                 TR("%s is an app (homebrew), not a game: PS4Mount plays CUSA games; install it the usual way",
                    "%s es una app (homebrew), no un juego: PS4Mount juega juegos CUSA; instálala de la forma normal"),
                 out->title_id);
    else if (!out->fake)
        snprintf(out->problem, sizeof(out->problem), "%s",
                 TR("not a fake pkg: only fake pkgs can be used", "no es un pkg fake: solo se pueden usar pkgs fake"));
    else if (fw && out->system_ver && !fw_runs(out->system_ver, fw)) {
        ver_text(out->system_ver, need, sizeof(need));
        ver_text(fw, have, sizeof(have));
        snprintf(out->problem, sizeof(out->problem),
                 TR("needs firmware %s or newer; this console has %s", "necesita el firmware %s o superior; esta consola tiene %s"),
                 need, have);
    }
    out->ok = !out->problem[0];
    return 0;
}
