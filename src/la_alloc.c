/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Memory for libarchive and zlib (see la_alloc.h). */
#define LA_ALLOC_IMPL
#include "la_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

/* From this size a block is mapped: below it, the heap (libarchive makes many small ones). */
#define BIG (64u << 10)

/* Before the data: its size and where it came from. 16 bytes keep it aligned as malloc's. */
typedef struct {
    size_t size, mapped;
} head_t;

void *la_malloc(size_t n) {
    if (n > SIZE_MAX - sizeof(head_t) - 0x4000)
        return NULL;
    int big = n >= BIG;
    head_t *h = big ? mem_alloc(n + sizeof(head_t)) : malloc(n + sizeof(head_t));
    if (!h)
        return NULL;
    h->size = n;
    h->mapped = (size_t)big;
    return h + 1;
}

void *la_calloc(size_t count, size_t n) {
    if (n && count > SIZE_MAX / n)
        return NULL;
    void *p = la_malloc(count * n);
    if (p)
        memset(p, 0, count * n); /* mapped pages are zero already; heap blocks are not */
    return p;
}

void la_free(void *p) {
    if (!p)
        return;
    head_t *h = (head_t *)p - 1;
    if (h->mapped)
        mem_free(h);
    else
        free(h);
}

void *la_realloc(void *p, size_t n) {
    if (!p)
        return la_malloc(n);
    head_t *h = (head_t *)p - 1;
    if (n <= h->size && (h->mapped || n * 2 > h->size)) {
        h->size = n; /* smaller: the block stays (a mapped one keeps its pages) */
        return p;
    }
    void *q = la_malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, h->size < n ? h->size : n);
    la_free(p);
    return q;
}

char *la_strdup(const char *s) {
    size_t l = strlen(s) + 1;
    char *d = la_malloc(l);
    if (d)
        memcpy(d, s, l);
    return d;
}
