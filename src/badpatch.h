/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The update of a game the console turned down, named by its size and time so another
 * update pkg is tried again. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* 1 when the text of a mark names this pkg (same size and time). An empty mark names any
 * update of the game, and so does one that makes no sense. A missing mark (NULL) names
 * none. */
int badpatch_matches(const char *text, uint64_t size, int64_t mtime);

/* The text of the mark of a pkg of this size and time, in out (n bytes). */
void badpatch_text(char *out, size_t n, uint64_t size, int64_t mtime);
