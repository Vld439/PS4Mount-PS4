/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Reading values out of a JSON text (Google's answers) without building a tree. */
#pragma once

#include <stddef.h>

/* A JSON value: its text, p[0..n). */
typedef struct {
    const char *p;
    size_t n;
} jv_t;

jv_t jv_of(const char *text, size_t n);
/* The member key of object v. 0 when found. */
int jv_get(jv_t v, const char *key, jv_t *out);
/* Element i of array v. 0 when found. */
int jv_index(jv_t v, int i, jv_t *out);
/* The string v decoded into out (truncated to n - 1 bytes). 0 when v is a string. */
int jv_string(jv_t v, char *out, size_t n);
/* The number v (also a number in a string, as Drive gives sizes); dflt otherwise. */
long long jv_int(jv_t v, long long dflt);
/* jv_get then jv_string / jv_int. */
int jv_get_string(jv_t v, const char *key, char *out, size_t n);
long long jv_get_int(jv_t v, const char *key, long long dflt);
