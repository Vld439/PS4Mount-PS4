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

/* 1 when text is a mark of 0.6.0 and older: the 64 hex digits of the pkg's 32-byte
 * digest (header offset 0xfe0). They are kept working: an update the system refused
 * there must not come back on upgrade. */
int badpatch_is_digest(const char *text);
/* 1 when that mark names this digest. */
int badpatch_digest_matches(const char *text, const uint8_t digest[32]);

/* The text of the mark of a pkg of this size and time, in out (n bytes). */
void badpatch_text(char *out, size_t n, uint64_t size, int64_t mtime);
