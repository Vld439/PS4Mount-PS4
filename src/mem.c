/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Buffers on mapped pages: see mem.h. */
#include "mem.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

/* Before the data: the size of the mapping. 64 bytes keep the data aligned for anything. */
#define HEAD 64u

void *mem_alloc(size_t n) {
    if (n > SIZE_MAX - HEAD - 0x4000)
        return NULL;
    size_t total = (n + HEAD + 0x3fff) & ~(size_t)0x3fff; /* whole 16 KB pages */
    void *m = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED)
        return NULL;
    *(size_t *)m = total;
    return (uint8_t *)m + HEAD;
}

void *mem_realloc(void *p, size_t n) {
    if (!p)
        return mem_alloc(n);
    size_t total = *(size_t *)((uint8_t *)p - HEAD);
    if (n <= total - HEAD)
        return p;
    void *q = mem_alloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, total - HEAD);
    mem_free(p);
    return q;
}

void mem_free(void *p) {
    if (!p)
        return;
    uint8_t *m = (uint8_t *)p - HEAD;
    munmap(m, *(size_t *)m);
}
