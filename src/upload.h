/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pkgs sent from the panel. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build.h"

/* Bytes of a pkg's start that pkg_check is given before the rest arrives. */
#define UPLOAD_CHECK_BYTES (4u << 20)

/* Receives a pkg sent from the panel on fd (the request's socket; pre: the body bytes
 * already read) into dir/name. It is written to dir/.ps4mount-upload.part, checked once
 * its first bytes are in and again at the end, and only then renamed: the scan never
 * sees a pkg that is incomplete or unusable. Runs in its own thread, answers the request
 * and closes fd; done (may be NULL) is called with the final path. install: the pkg is
 * to be installed, so an app PS4Mount does not use from its pkg is taken too. One at a time:
 * returns -1 (fd left to the caller) when another one is running. */
int upload_start(int fd, const char *dir, const char *name, uint64_t length, const void *pre, size_t pre_len,
                 uint32_t fw, int install, void (*done)(const char *path));

/* {"state":"idle|receiving|done|failed",...} */
void upload_status_json(buf_t *out);

/* The file name to use for a pkg sent as name: no directories, printable, ending in
 * .pkg. Returns -1 when nothing usable is left. */
int upload_clean_name(const char *name, char *out, size_t n);
