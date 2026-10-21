/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The memory of libarchive and zlib, which are built with this header included first
 * (build.sh, test/run.sh): small blocks from the libc heap, large ones (a RAR5 window is
 * tens of MB) on mapped pages, as mem.h explains. */
#pragma once

#include <stddef.h>

void *la_malloc(size_t n);
void *la_calloc(size_t count, size_t n);
void *la_realloc(void *p, size_t n);
void la_free(void *p);
char *la_strdup(const char *s);

#ifndef LA_ALLOC_IMPL
#define malloc la_malloc
#define calloc la_calloc
#define realloc la_realloc
#define free la_free
#define strdup la_strdup
#endif
