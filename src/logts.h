/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Log file with the time at the start of every line. */
#pragma once

#include <stddef.h>

/* Default cap per log file on internal storage (2 MiB; one .old backup kept). */
#define LOG_MAX_BYTES (2u << 20)

/* Sends stdout to path (appended, rotated to path.old past the cap), each line prefixed
 * with the date and time. 0 on success; on failure stdout is left as it was. */
int log_start(const char *path);
/* Overrides the rotation threshold in bytes (0 restores LOG_MAX_BYTES). */
void log_set_max_bytes(size_t max);
/* Writes what is still pending and waits for it; safe to call more than once. */
void log_stop(void);

/* "2026-10-14 13:52:06.123 " for the current time (local time of the console). */
void log_stamp(char *out, size_t n);
/* Writes buf to fd, putting stamp before every line; *at_start tracks whether the next
 * byte begins a line (start with 1). Returns 0, or -1 when a write failed. */
int log_write_lines(int fd, int *at_start, const char *buf, size_t len, const char *stamp);
