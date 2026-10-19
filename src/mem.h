/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Buffers that can be large (artwork, pkg entries, request bodies): mapped pages, not the
 * libc heap. The loader runs the payload inside a system process whose heap can be a few
 * MB in all, and a request it cannot serve ends that process. */
#pragma once

#include <stddef.h>

/* NULL when the system has no memory left. Free with mem_free only. */
void *mem_alloc(size_t n);
/* As realloc: p may be NULL; on failure p is left as it was and NULL is returned. */
void *mem_realloc(void *p, size_t n);
void mem_free(void *p);
