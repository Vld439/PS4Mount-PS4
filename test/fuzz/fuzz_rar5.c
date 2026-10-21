/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Fuzz target: the RAR5 decoder (volumes downloaded from links). The input is two volumes
 * (split at a point its first byte picks) fed in pieces of sizes its second byte picks;
 * built with RAR5DEC_FUZZ, so keys are cheap and any password opens. */
#include <stddef.h>
#include <stdint.h>

#include "rar5dec.h"

static void volume(rar5keys_t *k, const uint8_t *p, size_t n, size_t piece) {
    rar5dec_t *d = rar5dec_new(k);
    if (!d)
        return;
    while (n) {
        size_t take = n < piece ? n : piece;
        const uint8_t *out;
        if (rar5dec_feed(d, p, take, &out) < 0)
            break;
        p += take;
        n -= take;
    }
    if (!n)
        rar5dec_end(d);
    rar5dec_free(d);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2)
        return 0;
    size_t split = (size - 2) * data[0] / 255, piece = 1 + (size_t)data[1] * 37;
    rar5keys_t *k = rar5keys_new();
    if (!k)
        return 0;
    rar5keys_add(k, "ps4mount");
    rar5keys_add(k, "x");
    volume(k, data + 2, split, piece);
    volume(k, data + 2 + split, size - 2 - split, piece);
    rar5keys_found(k);
    rar5keys_free(k);
    return 0;
}
