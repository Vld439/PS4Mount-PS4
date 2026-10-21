/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Makes stray.rar for test/links_test.c (see make.sh): guess.rar (rar -hp) with the password
 * check of its service header (quick open) replaced by stray bytes and their SHA-256, as a
 * rar seen in the wild left it. Headers are decrypted, changed, given their CRC32 again and
 * encrypted back with the same IV. Usage: stray in.rar out.rar password */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

static uint64_t vint(const uint8_t **p) {
    uint64_t v = 0;
    for (int s = 0;; s += 7) {
        uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7f) << s;
        if (!(b & 0x80))
            return v;
    }
}

static void key_of(const char *pw, const uint8_t *salt, int lg, uint8_t key[32]) {
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, &br_sha256_vtable, pw, strlen(pw));
    uint8_t s[20], u[32];
    memcpy(s, salt, 16);
    s[16] = s[17] = s[18] = 0;
    s[19] = 1;
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, s, 20);
    br_hmac_out(&hc, u);
    memcpy(key, u, 32);
    for (uint32_t j = 1; j < (1u << lg); j++) {
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, u, 32);
        br_hmac_out(&hc, u);
        for (int b = 0; b < 32; b++)
            key[b] ^= u[b];
    }
}

int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    static uint8_t a[1 << 20];
    size_t n = fread(a, 1, sizeof(a), f);
    fclose(f);
    /* the encryption header, plain, after the signature */
    const uint8_t *p = a + 12;
    uint64_t size = vint(&p);
    const uint8_t *body = p;
    vint(&body); /* type */
    vint(&body); /* flags */
    vint(&body); /* version */
    vint(&body); /* encryption flags */
    int lg = *body++;
    uint8_t key[32];
    key_of(argv[3], body, lg, key);
    size_t at = (size_t)(p - a) + (size_t)size;
    int changed = 0;
    br_aes_ct_cbcdec_keys dk;
    br_aes_ct_cbcenc_keys ek;
    br_aes_ct_cbcdec_init(&dk, key, 32);
    br_aes_ct_cbcenc_init(&ek, key, 32);
    while (at + 32 <= n) {
        uint8_t iv[16], h[4096];
        memcpy(iv, a + at, 16);
        memcpy(h, a + at + 16, 16);
        br_aes_ct_cbcdec_run(&dk, iv, h, 16);
        const uint8_t *q = h + 4;
        uint64_t hs = vint(&q);
        size_t plain = (size_t)(q - h) + (size_t)hs, padded = (plain + 15) / 16 * 16;
        if (padded > sizeof(h) || at + 16 + padded > n)
            break;
        memcpy(iv, a + at, 16);
        memcpy(h, a + at + 16, padded);
        br_aes_ct_cbcdec_run(&dk, iv, h, padded);
        uint64_t type = vint(&q), flags = vint(&q), extra = 0, data = 0;
        if (flags & 1)
            extra = vint(&q);
        if (flags & 2)
            data = vint(&q);
        if (type == 3) {
            for (uint8_t *r = h + plain - extra; r < h + plain;) {
                const uint8_t *rq = r;
                uint64_t rlen = vint(&rq);
                uint8_t *rend = (uint8_t *)rq + rlen;
                if (vint(&rq) == 1) {
                    /* version, flags, kdf count, salt, IV, then the check */
                    uint8_t *c = (uint8_t *)rq + 3 + 16 + 16;
                    static const uint8_t stray[8] = {0, 0, 'G', 'P', 'O', 'S', 0xd0, 0xbb};
                    uint8_t sum[32];
                    br_sha256_context sc;
                    br_sha256_init(&sc);
                    br_sha256_update(&sc, stray, 8);
                    br_sha256_out(&sc, sum);
                    memcpy(c, stray, 8);
                    memcpy(c + 8, sum, 4);
                    changed++;
                }
                r = rend;
            }
            uint32_t crc = crc32_of(h + 4, plain - 4);
            h[0] = (uint8_t)crc, h[1] = (uint8_t)(crc >> 8), h[2] = (uint8_t)(crc >> 16), h[3] = (uint8_t)(crc >> 24);
            memcpy(iv, a + at, 16);
            br_aes_ct_cbcenc_run(&ek, iv, h, padded);
            memcpy(a + at + 16, h, padded);
        }
        at += 16 + padded + (size_t)data;
    }
    if (!changed)
        return 3;
    f = fopen(argv[2], "wb");
    if (!f || fwrite(a, 1, n, f) != n)
        return 1;
    fclose(f);
    return 0;
}
