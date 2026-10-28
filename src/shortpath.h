/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The short path the system is handed for a pkg whose own path is too long. */
#pragma once

#include <stddef.h>

/* The system keeps the path of a pkg in 96-byte buffers (app.json, the playgo records,
 * the BGFT game task): a longer one is cut at 95, the pkg is then looked for under a
 * name that is not there and the game stays half registered. */
#define SYS_PATH_MAX 95

/* 1 and the link name in out (n bytes) when the system must be handed a link instead of
 * path, 0 when path itself fits (out untouched). The link is <dir>/<tid>-<kind>.pkg: a
 * symlink to path, made by the caller. kind tells the pkgs of one game apart: "app",
 * "patch", or "ac-<label>" for each add-on. */
int short_pkg_link(const char *dir, const char *tid, const char *kind, const char *path, char *out, size_t n);
