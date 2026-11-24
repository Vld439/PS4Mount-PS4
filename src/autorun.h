/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * GoldHEN's payloads.ini AutoRun list, edited as text. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build.h"

/* 1 when the line names the payload file (ps4mount-ps4.bin or .elf, any path, any
 * case) as an active entry: a commented-out line is the user's and does not count. */
int autorun_is_ours(const char *line, size_t len);

/* Appends to out the ini text of raw[0..len] without our entries and, when on, with
 * "<payload_path> = 1" once under [AutoRun] (the section is added at the end when
 * there is none). raw may be NULL. 0 on success, -1 to turn on without a path. */
int autorun_edit_ini(const uint8_t *raw, size_t len, const char *payload_path, int on, buf_t *out);

/* Writes data[0..len] to "<ini_path>.tmp" and renames it over ini_path: the ini is
 * never seen half written. 0 on success, -1 on failure. */
int autorun_write_atomic(const char *ini_path, const void *data, size_t len);
