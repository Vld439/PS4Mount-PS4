/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * RAR5 decryption on the way to libarchive (see rar5dec.h). A volume is the signature and
 * then blocks: a header (CRC32, its size, type, flags, maybe extra area and data sizes)
 * and maybe a data area. With rar -hp an encryption header (type 4) comes first and each
 * header after it is a 16-byte IV and the header in AES-256-CBC, padded to 16 bytes. The
 * data of a file with an encryption record (extra record 1) is AES-256-CBC too, its size a
 * multiple of 16; a file split over volumes has a record in each, with the IV that goes on
 * from the last one. Out: the same blocks with plain headers, without those records, and
 * without the checksums rar turned into MACs (rar -p keeps none that a reader could check
 * without the key); the data areas keep their sizes, padding included. */
#include "rar5dec.h"

#include <stdio.h>
#include <string.h>

#include "bearssl.h"
#include "mem.h"

#define SIG_LEN 8
#define HEAD_MAX (2u << 20) /* rar's own limit for a header */
#define KDF_LG_MAX 24
#ifdef RAR5DEC_FUZZ
/* the fuzz target's: cheap keys, and any password taken, so it gets past them */
#define KDF_ROUNDS(lg) (1u << ((lg) & 3))
#define CHECK_MATCHES(a, b) 1
#else
#define KDF_ROUNDS(lg) (1u << (lg))
#define CHECK_MATCHES(a, b) (!memcmp((a), (b), 8))
#endif
#define NCACHE 4

static const uint8_t k_sig[SIG_LEN] = {'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00};

enum { HEAD_MAIN = 1, HEAD_FILE = 2, HEAD_SERVICE = 3, HEAD_CRYPT = 4, HEAD_END = 5 };
enum { HFL_EXTRA = 0x01, HFL_DATA = 0x02, HFL_FROM_PREV = 0x08, HFL_TO_NEXT = 0x10, HFL_SPLIT = 0x18 };
enum { FHFL_MTIME = 0x02, FHFL_CRC = 0x04 };
enum { EX_CRYPT = 0x01, EX_HASH = 0x02, EX_LOCATOR = 0x01 };
enum { CRYPT_CHECK = 0x01, CRYPT_MAC = 0x02 };
enum { S_SIG, S_HEAD, S_DATA, S_PASS };

typedef struct {
    uint8_t salt[16], key[32], check[8];
    int lg, pw, used;
} kslot_t;

struct rar5keys {
    char pw[RAR5_MAX_PASSWORDS][RAR5_PASSWORD_MAX + 1];
    int npw, found, refused, tick;
    kslot_t cache[NCACHE];
    uint8_t next_iv[16]; /* where the CBC of a file split over volumes goes on */
    int has_next_iv;
};

struct rar5dec {
    rar5keys_t *k;
    int state, hdr_enc;
    uint8_t hkey[32];
    uint8_t *hb; /* a header being gathered (as it is in the volume) */
    size_t hlen, hcap;
    uint64_t data_left;
    int data_enc, to_next;
    uint8_t iv[16], tail[16];
    size_t ntail;
    br_aes_gen_cbcdec_keys dk, hk; /* the data's key, the headers' */
    const br_block_cbcdec_class *vt;
    uint8_t *ob, *held; /* held: the main header, until the next one */
    size_t olen, ocap, nheld;
    char err[200];
};

/* --- passwords and keys --- */

rar5keys_t *rar5keys_new(void) {
    rar5keys_t *k = mem_alloc(sizeof(*k));
    if (k) {
        memset(k, 0, sizeof(*k));
        k->found = -1;
    }
    return k;
}

void rar5keys_free(rar5keys_t *k) {
    if (k) {
        memset(k, 0, sizeof(*k));
        mem_free(k);
    }
}

void rar5keys_add(rar5keys_t *k, const char *password) {
    size_t l = strlen(password);
    if (!l || l > RAR5_PASSWORD_MAX || k->npw >= RAR5_MAX_PASSWORDS)
        return;
    for (int i = 0; i < k->npw; i++)
        if (!strcmp(k->pw[i], password))
            return;
    memcpy(k->pw[k->npw++], password, l + 1);
}

const char *rar5keys_found(const rar5keys_t *k) { return k->found >= 0 ? k->pw[k->found] : NULL; }

int rar5keys_refused(const rar5keys_t *k) { return k->refused; }

static void hmac256(const br_hmac_key_context *kc, const uint8_t *p, size_t n, uint8_t out[32]) {
    br_hmac_context hc;
    br_hmac_init(&hc, kc, 0);
    br_hmac_update(&hc, p, n);
    br_hmac_out(&hc, out);
}

/* PBKDF2-HMAC-SHA256 as rar runs it: the key after 2^lg rounds and, 32 rounds further on,
   the value its password check is folded from. */
static void kdf(const char *pw, const uint8_t salt[16], int lg, uint8_t key[32], uint8_t check[8]) {
    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha256_vtable, pw, strlen(pw));
    uint8_t s[20], u[32], f[32];
    memcpy(s, salt, 16);
    s[16] = s[17] = s[18] = 0;
    s[19] = 1;
    hmac256(&kc, s, sizeof(s), u);
    memcpy(f, u, 32);
    uint32_t rounds[3] = {KDF_ROUNDS(lg) - 1, 16, 16};
    for (int i = 0; i < 3; i++) {
        for (uint32_t j = 0; j < rounds[i]; j++) {
            hmac256(&kc, u, 32, u);
            for (int b = 0; b < 32; b++)
                f[b] ^= u[b];
        }
        if (i == 0)
            memcpy(key, f, 32);
    }
    memset(check, 0, 8);
    for (int b = 0; b < 32; b++)
        check[b % 8] ^= f[b];
}

/* The key for this salt: with the password that opened the archive, or else with the
   first of the given ones whose check matches (or, with no check, the first one). -1 when
   none fits. */
static int find_key(rar5keys_t *k, const uint8_t salt[16], int lg, const uint8_t *check, uint8_t key[32]) {
    for (int c = 0; c < NCACHE; c++) {
        kslot_t *s = &k->cache[c];
        if (s->used && s->lg == lg && !memcmp(s->salt, salt, 16) && (k->found < 0 || s->pw == k->found) &&
            (!check || CHECK_MATCHES(s->check, check))) {
            s->used = ++k->tick;
            memcpy(key, s->key, 32);
            k->found = s->pw;
            return 0;
        }
    }
    for (int t = 0; t < k->npw; t++) {
        int i = k->found >= 0 ? (t ? -1 : k->found) : t;
        if (i < 0)
            break;
        kslot_t *s = &k->cache[0];
        for (int c = 1; c < NCACHE; c++)
            if (k->cache[c].used < s->used)
                s = &k->cache[c];
        memcpy(s->salt, salt, 16);
        s->lg = lg;
        s->pw = i;
        kdf(k->pw[i], salt, lg, s->key, s->check);
        s->used = ++k->tick;
        if (!check || CHECK_MATCHES(s->check, check)) {
            memcpy(key, s->key, 32);
            k->found = i;
            return 0;
        }
    }
    k->refused = 1;
    return -1;
}

/* --- the decoder --- */

rar5dec_t *rar5dec_new(rar5keys_t *k) {
    rar5dec_t *d = mem_alloc(sizeof(*d));
    if (!d)
        return NULL;
    memset(d, 0, sizeof(*d));
    d->k = k;
    d->vt = br_aes_x86ni_cbcdec_get_vtable();
    if (!d->vt)
        d->vt = &br_aes_ct64_cbcdec_vtable;
    return d;
}

void rar5dec_free(rar5dec_t *d) {
    if (!d)
        return;
    mem_free(d->hb);
    mem_free(d->ob);
    mem_free(d->held);
    memset(d, 0, sizeof(*d));
    mem_free(d);
}

const char *rar5dec_error(const rar5dec_t *d) { return d->err; }

static int fail(rar5dec_t *d, const char *why) {
    snprintf(d->err, sizeof(d->err), "%s", why);
    return -1;
}

static int grow(uint8_t **b, size_t *cap, size_t need) {
    if (need <= *cap)
        return 0;
    size_t c = *cap ? *cap : 4096;
    while (c < need)
        c *= 2;
    uint8_t *n = mem_realloc(*b, c);
    if (!n)
        return -1;
    *b = n;
    *cap = c;
    return 0;
}

static int emit(rar5dec_t *d, const uint8_t *p, size_t n) {
    if (grow(&d->ob, &d->ocap, d->olen + n))
        return fail(d, "no memory");
    memcpy(d->ob + d->olen, p, n);
    d->olen += n;
    return 0;
}

static void decrypt_head(rar5dec_t *d, uint8_t *iv, uint8_t *p, size_t n) { d->vt->run(&d->hk.vtable, iv, p, n); }

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int b = 0; b < 8; b++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* A vint at *p (before end): 0 with it, -1 when it does not fit. */
static int vint(const uint8_t **p, const uint8_t *end, uint64_t *v) {
    *v = 0;
    for (int s = 0; s < 70 && *p < end; s += 7) {
        uint8_t b = *(*p)++;
        *v |= (uint64_t)(b & 0x7f) << s;
        if (!(b & 0x80))
            return 0;
    }
    return -1;
}

static size_t put_vint(uint8_t *o, uint64_t v) {
    size_t n = 0;
    do {
        o[n] = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v)
            o[n] |= 0x80;
        n++;
    } while (v);
    return n;
}

/* The fields of an encryption record or header, from its version on: 0 with them. */
static int crypt_fields(const uint8_t *p, const uint8_t *end, int with_iv, uint64_t *flags, int *lg,
                        const uint8_t **salt, const uint8_t **iv, const uint8_t **check) {
    uint64_t ver;
    if (vint(&p, end, &ver) || ver != 0 || vint(&p, end, flags) || p >= end)
        return -1;
    *lg = *p++;
    if (*lg > KDF_LG_MAX || end - p < 16 + (with_iv ? 16 : 0))
        return -1;
    *salt = p;
    p += 16;
    if (with_iv) {
        *iv = p;
        p += 16;
    }
    *check = NULL;
    if (*flags & CRYPT_CHECK) {
        if (end - p < 12)
            return -1;
        /* 8 bytes and the start of their SHA-256; a service header's are zeros (no check) */
        br_sha256_context sc;
        uint8_t sum[32];
        static const uint8_t zero[8];
        br_sha256_init(&sc);
        br_sha256_update(&sc, p, 8);
        br_sha256_out(&sc, sum);
        if (!memcmp(sum, p + 8, 4) && memcmp(p, zero, 8))
            *check = p;
    }
    return 0;
}

/* Header h (n bytes, plain, its CRC good) into the output: as it is, or without what the
   plain archive cannot keep. mac: a file whose CRC32 and hash no reader could check (left
   out with its encryption record); locator: the main header's offsets, wrong once headers
   got smaller. */
static int put_header(rar5dec_t *d, const uint8_t *h, size_t n, int crypt, int mac, int locator) {
    static const char *damaged = "a header of the archive is damaged";
    if (!crypt && !mac && !locator)
        return emit(d, h, n);
    const uint8_t *p = h + 4, *end = h + n;
    uint64_t size, type, flags, extra = 0, data = 0;
    if (vint(&p, end, &size) || vint(&p, end, &type) || vint(&p, end, &flags) ||
        ((flags & HFL_EXTRA) && vint(&p, end, &extra)) || ((flags & HFL_DATA) && vint(&p, end, &data)) ||
        extra > (uint64_t)(end - p))
        return fail(d, damaged);
    const uint8_t *body = p, *xa = end - extra;
    uint8_t *out = mem_alloc(n + 32);
    if (!out)
        return fail(d, "no memory");
    /* body: a file's CRC32 out */
    uint8_t bb[48];
    size_t bn = 0;
    const uint8_t *bp = body;
    if (mac) {
        uint64_t ff, usize, attr;
        if (vint(&bp, xa, &ff) || vint(&bp, xa, &usize) || vint(&bp, xa, &attr) ||
            xa - bp < ((ff & FHFL_MTIME) ? 4 : 0) + ((ff & FHFL_CRC) ? 4 : 0)) {
            mem_free(out);
            return fail(d, damaged);
        }
        bn += put_vint(bb + bn, ff & ~(uint64_t)FHFL_CRC);
        bn += put_vint(bb + bn, usize);
        bn += put_vint(bb + bn, attr);
        if (ff & FHFL_MTIME) {
            memcpy(bb + bn, bp, 4);
            bn += 4;
            bp += 4;
        }
        if (ff & FHFL_CRC)
            bp += 4;
    }
    size_t rest = (size_t)(xa - bp);
    /* the extra area without the records left out, put first at the end of out */
    uint8_t *xs = out + n + 32 - extra;
    size_t xn = 0;
    for (const uint8_t *r = xa; r < end;) {
        const uint8_t *rs = r, *q;
        uint64_t rlen, rtype;
        if (vint(&r, end, &rlen) || rlen > (uint64_t)(end - r) || (q = r, vint(&q, r + rlen, &rtype))) {
            mem_free(out);
            return fail(d, damaged);
        }
        r += rlen;
        int drop = locator ? rtype == EX_LOCATOR : (crypt && rtype == EX_CRYPT) || (mac && rtype == EX_HASH);
        if (!drop) {
            memmove(xs + xn, rs, (size_t)(r - rs));
            xn += (size_t)(r - rs);
        }
    }
    /* CRC32, size, type, flags, sizes, body, extra area */
    uint8_t mid[40];
    size_t mn = put_vint(mid, type);
    uint64_t nf = xn ? flags | HFL_EXTRA : flags & ~(uint64_t)HFL_EXTRA;
    mn += put_vint(mid + mn, nf);
    if (nf & HFL_EXTRA)
        mn += put_vint(mid + mn, xn);
    if (flags & HFL_DATA)
        mn += put_vint(mid + mn, data);
    uint8_t *w = out + 4;
    w += put_vint(w, mn + bn + rest + xn);
    memcpy(w, mid, mn), w += mn;
    memcpy(w, bb, bn), w += bn;
    memmove(w, bp, rest), w += rest;
    memmove(w, xs, xn), w += xn;
    size_t total = (size_t)(w - out);
    uint32_t c = crc32_of(out + 4, total - 4);
    out[0] = (uint8_t)c, out[1] = (uint8_t)(c >> 8), out[2] = (uint8_t)(c >> 16), out[3] = (uint8_t)(c >> 24);
    int rc = emit(d, out, total);
    mem_free(out);
    return rc;
}

/* A whole plain header h (n bytes): taken in (encryption, the main header until the next
   tells whether the archive is encrypted) or put out. */
static int header(rar5dec_t *d, const uint8_t *h, size_t n) {
    static const char *damaged = "a header of the archive is damaged";
    if (crc32_of(h + 4, n - 4) != ((uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24))
        return fail(d, damaged);
    const uint8_t *p = h + 4, *end = h + n;
    uint64_t size, type, flags, extra = 0, data = 0;
    if (vint(&p, end, &size) || vint(&p, end, &type) || vint(&p, end, &flags) ||
        ((flags & HFL_EXTRA) && vint(&p, end, &extra)) || ((flags & HFL_DATA) && vint(&p, end, &data)) ||
        extra > (uint64_t)(end - p))
        return fail(d, damaged);
    const uint8_t *body = p, *xa = end - extra;
    d->data_left = data;
    d->data_enc = 0;
    d->to_next = (flags & HFL_TO_NEXT) != 0;
    uint64_t cf;
    int lg;
    const uint8_t *salt, *iv, *check;
    if (type == HEAD_CRYPT) {
        if (crypt_fields(body, xa, 0, &cf, &lg, &salt, &iv, &check))
            return fail(d, "the archive's encryption is of a kind PS4Mount does not know");
        if (find_key(d->k, salt, lg, check, d->hkey))
            return fail(d, "password");
        d->vt->init(&d->hk.vtable, d->hkey, 32);
        d->hdr_enc = 1;
        return 0;
    }
    if (type == HEAD_MAIN && !d->held) {
        if (!(d->held = mem_alloc(n)))
            return fail(d, "no memory");
        memcpy(d->held, h, n);
        d->nheld = n;
        d->data_left = 0;
        return 0;
    }
    int file = type == HEAD_FILE || type == HEAD_SERVICE, mac = 0;
    /* the encryption record, if any: the key and IV of the data */
    for (const uint8_t *r = xa; file && r < end;) {
        uint64_t rlen, rtype;
        if (vint(&r, end, &rlen) || rlen > (uint64_t)(end - r))
            return fail(d, damaged);
        const uint8_t *rend = r + rlen, *q = r;
        if (vint(&q, rend, &rtype))
            return fail(d, damaged);
        if (rtype == EX_CRYPT) {
            uint8_t key[32];
            if (crypt_fields(q, rend, 1, &cf, &lg, &salt, &iv, &check))
                return fail(d, "the archive's encryption is of a kind PS4Mount does not know");
            if (find_key(d->k, salt, lg, check, key))
                return fail(d, "password");
            d->vt->init(&d->dk.vtable, key, 32);
            /* a part after the first goes on from the last block of the one before */
            memcpy(d->iv, (flags & HFL_FROM_PREV) && d->k->has_next_iv ? d->k->next_iv : iv, 16);
            d->ntail = 0;
            d->data_enc = 1;
            /* checksums that only the key could check: a MAC, or a split part's, which is of
               the encrypted data in its volume */
            mac = (cf & CRYPT_MAC) || (flags & HFL_SPLIT);
        }
        r = rend;
    }
    if (d->data_enc && data % 16)
        return fail(d, "an encrypted file of the archive is damaged");
    if (d->held) {
        int rc = put_header(d, d->held, d->nheld, 0, 0, d->hdr_enc || d->data_enc);
        mem_free(d->held);
        d->held = NULL;
        if (rc)
            return -1;
    }
    if (put_header(d, h, n, d->data_enc, mac, 0))
        return -1;
    if (type == HEAD_END)
        d->state = S_PASS;
    return 0;
}

/* Takes what it can of a header from in; 1 when the header is whole (and done). */
static int gather(rar5dec_t *d, const uint8_t **in, size_t *n) {
    size_t want;
    for (;;) {
        if (d->hdr_enc) {
            /* IV, then the first block tells the size */
            if (d->hlen < 32)
                want = 32;
            else {
                uint8_t first[16], iv[16];
                memcpy(first, d->hb + 16, 16);
                memcpy(iv, d->hb, 16);
                decrypt_head(d, iv, first, 16);
                const uint8_t *p = first + 4;
                uint64_t size;
                if (vint(&p, first + 16, &size) || size > HEAD_MAX)
                    return fail(d, "a header of the archive is damaged");
                size_t plain = (size_t)(p - first) + (size_t)size;
                want = 16 + (plain + 15) / 16 * 16;
                if (d->hlen >= want) {
                    uint8_t iv2[16];
                    memcpy(iv2, d->hb, 16);
                    decrypt_head(d, iv2, d->hb + 16, want - 16);
                    d->hlen = 0;
                    return header(d, d->hb + 16, plain) ? -1 : 1;
                }
            }
        } else {
            if (d->hlen < 5)
                want = 5;
            else {
                const uint8_t *p = d->hb + 4;
                uint64_t size;
                if (vint(&p, d->hb + d->hlen, &size)) {
                    if (d->hlen >= 14)
                        return fail(d, "a header of the archive is damaged");
                    want = d->hlen + 1;
                } else {
                    if (size > HEAD_MAX)
                        return fail(d, "a header of the archive is damaged");
                    want = (size_t)(p - d->hb) + (size_t)size;
                    if (d->hlen >= want) {
                        d->hlen = 0;
                        return header(d, d->hb, want) ? -1 : 1;
                    }
                }
            }
        }
        if (!*n)
            return 0;
        size_t take = want - d->hlen < *n ? want - d->hlen : *n;
        if (grow(&d->hb, &d->hcap, d->hlen + take))
            return fail(d, "no memory");
        memcpy(d->hb + d->hlen, *in, take);
        d->hlen += take;
        *in += take;
        *n -= take;
    }
}

ssize_t rar5dec_feed(rar5dec_t *d, const uint8_t *in, size_t n, const uint8_t **out) {
    d->olen = 0;
    while (n) {
        if (d->state == S_PASS) {
            if (emit(d, in, n))
                return -1;
            break;
        }
        if (d->state == S_SIG) {
            size_t take = SIG_LEN - d->hlen < n ? SIG_LEN - d->hlen : n;
            if (grow(&d->hb, &d->hcap, SIG_LEN))
                return fail(d, "no memory");
            memcpy(d->hb + d->hlen, in, take);
            d->hlen += take;
            in += take;
            n -= take;
            if (d->hlen < SIG_LEN)
                break;
            d->state = memcmp(d->hb, k_sig, SIG_LEN) ? S_PASS : S_HEAD;
            d->hlen = 0;
            if (emit(d, d->hb, SIG_LEN))
                return -1;
            continue;
        }
        if (d->state == S_HEAD) {
            int r = gather(d, &in, &n);
            if (r < 0)
                return -1;
            if (r && d->state == S_HEAD && d->data_left)
                d->state = S_DATA;
            continue;
        }
        /* a data area */
        size_t take = d->data_left < n ? (size_t)d->data_left : n;
        if (!d->data_enc) {
            if (emit(d, in, take))
                return -1;
        } else {
            size_t start = d->olen;
            if (emit(d, d->tail, d->ntail) || emit(d, in, take))
                return -1;
            size_t have = d->olen - start, whole = have / 16 * 16;
            d->vt->run(&d->dk.vtable, d->iv, d->ob + start, whole);
            d->ntail = have - whole;
            memcpy(d->tail, d->ob + start + whole, d->ntail);
            d->olen = start + whole;
        }
        in += take;
        n -= take;
        d->data_left -= take;
        if (!d->data_left) {
            d->state = S_HEAD;
            if (d->data_enc && d->to_next) {
                memcpy(d->k->next_iv, d->iv, 16);
                d->k->has_next_iv = 1;
            }
        }
    }
    *out = d->ob;
    return (ssize_t)d->olen;
}

int rar5dec_end(rar5dec_t *d) {
    if (d->state == S_PASS || (d->state == S_HEAD && !d->hlen && !d->held))
        return 0;
    return fail(d, "the volume ends in the middle of the archive");
}
