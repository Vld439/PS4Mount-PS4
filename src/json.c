/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Reading values out of a JSON text (see json.h). */
#include "json.h"

#include <stdlib.h>
#include <string.h>

jv_t jv_of(const char *text, size_t n) {
    jv_t v = {text, n};
    return v;
}

static size_t skip_ws(const char *p, size_t i, size_t n) {
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r'))
        i++;
    return i;
}

/* The end of the value at p[i]; n when it is not complete. Depth is bounded so a hostile
 * answer cannot exhaust the stack (it is iterative anyway). */
static size_t skip_value(const char *p, size_t i, size_t n) {
    i = skip_ws(p, i, n);
    if (i >= n)
        return n;
    if (p[i] == '"') {
        for (i++; i < n; i++) {
            if (p[i] == '\\')
                i++;
            else if (p[i] == '"')
                return i + 1;
        }
        return n;
    }
    if (p[i] == '{' || p[i] == '[') {
        int depth = 0;
        for (; i < n; i++) {
            char c = p[i];
            if (c == '"') {
                for (i++; i < n && p[i] != '"'; i++)
                    if (p[i] == '\\')
                        i++;
            } else if (c == '{' || c == '[')
                depth++;
            else if ((c == '}' || c == ']') && --depth == 0)
                return i + 1;
        }
        return n;
    }
    while (i < n && !strchr(",}] \t\r\n", p[i]))
        i++;
    return i;
}

static jv_t trimmed(jv_t v) {
    size_t i = skip_ws(v.p, 0, v.n);
    jv_t t = {v.p + i, v.n - i};
    while (t.n && strchr(" \t\r\n", t.p[t.n - 1]))
        t.n--;
    return t;
}

int jv_get(jv_t v, const char *key, jv_t *out) {
    v = trimmed(v);
    if (!v.n || v.p[0] != '{')
        return -1;
    size_t i = 1, kl = strlen(key);
    for (;;) {
        i = skip_ws(v.p, i, v.n);
        if (i >= v.n || v.p[i] != '"')
            return -1;
        size_t ks = i + 1, ke = skip_value(v.p, i, v.n);
        if (ke >= v.n)
            return -1;
        i = skip_ws(v.p, ke, v.n);
        if (i >= v.n || v.p[i] != ':')
            return -1;
        size_t vs = skip_ws(v.p, i + 1, v.n), ve = skip_value(v.p, vs, v.n);
        if (ve > v.n || vs >= v.n)
            return -1;
        if (ke - 1 - ks == kl && !memcmp(v.p + ks, key, kl)) {
            out->p = v.p + vs;
            out->n = ve - vs;
            return 0;
        }
        i = skip_ws(v.p, ve, v.n);
        if (i >= v.n || v.p[i] != ',')
            return -1;
        i++;
    }
}

int jv_index(jv_t v, int idx, jv_t *out) {
    v = trimmed(v);
    if (!v.n || v.p[0] != '[' || idx < 0)
        return -1;
    size_t i = skip_ws(v.p, 1, v.n);
    if (i < v.n && v.p[i] == ']')
        return -1;
    for (int k = 0;; k++) {
        size_t vs = skip_ws(v.p, i, v.n), ve = skip_value(v.p, vs, v.n);
        if (vs >= v.n || ve > v.n)
            return -1;
        if (k == idx) {
            out->p = v.p + vs;
            out->n = ve - vs;
            return 0;
        }
        i = skip_ws(v.p, ve, v.n);
        if (i >= v.n || v.p[i] != ',')
            return -1;
        i++;
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int jv_string(jv_t v, char *out, size_t n) {
    v = trimmed(v);
    if (!n)
        return -1;
    out[0] = 0;
    if (v.n < 2 || v.p[0] != '"' || v.p[v.n - 1] != '"')
        return -1;
    size_t o = 0;
#define PUT(ch)                                                                                                        \
    do {                                                                                                               \
        if (o + 1 < n)                                                                                                 \
            out[o++] = (char)(ch);                                                                                     \
    } while (0)
    for (size_t i = 1; i + 1 < v.n; i++) {
        char c = v.p[i];
        if (c != '\\') {
            PUT(c);
            continue;
        }
        if (++i + 1 >= v.n)
            break;
        switch (v.p[i]) {
        case 'n': PUT('\n'); break;
        case 't': PUT('\t'); break;
        case 'r': PUT('\r'); break;
        case 'b': PUT('\b'); break;
        case 'f': PUT('\f'); break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 1; k <= 4; k++) {
                int h = i + k + 1 < v.n ? hexval(v.p[i + k]) : -1;
                if (h < 0) {
                    cp = '?';
                    break;
                }
                cp = cp * 16 + (unsigned)h;
            }
            i += 4;
            /* UTF-8; a surrogate pair half becomes '?' (names here are titles) */
            if (cp >= 0xd800 && cp < 0xe000)
                PUT('?');
            else if (cp < 0x80)
                PUT(cp);
            else if (cp < 0x800) {
                PUT(0xc0 | (cp >> 6));
                PUT(0x80 | (cp & 0x3f));
            } else {
                PUT(0xe0 | (cp >> 12));
                PUT(0x80 | ((cp >> 6) & 0x3f));
                PUT(0x80 | (cp & 0x3f));
            }
            break;
        }
        default: PUT(v.p[i]);
        }
    }
#undef PUT
    out[o] = 0;
    return 0;
}

long long jv_int(jv_t v, long long dflt) {
    v = trimmed(v);
    char tmp[32];
    if (v.n && v.p[0] == '"') {
        if (jv_string(v, tmp, sizeof(tmp)))
            return dflt;
    } else {
        if (!v.n || v.n >= sizeof(tmp))
            return dflt;
        memcpy(tmp, v.p, v.n);
        tmp[v.n] = 0;
    }
    char *end;
    long long r = strtoll(tmp, &end, 10);
    return end == tmp ? dflt : r;
}

int jv_get_string(jv_t v, const char *key, char *out, size_t n) {
    jv_t m;
    if (n)
        out[0] = 0;
    return jv_get(v, key, &m) ? -1 : jv_string(m, out, n);
}

long long jv_get_int(jv_t v, const char *key, long long dflt) {
    jv_t m;
    return jv_get(v, key, &m) ? dflt : jv_int(m, dflt);
}
