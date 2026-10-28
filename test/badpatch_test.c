/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Marks of the updates the system turned down. */
#include <stdio.h>
#include <string.h>

#include "../src/badpatch.h"

static int fails;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main(void) {
    char mark[64];
    const uint64_t size = 2264924160ull;
    const int64_t mtime = 1742779432;

    badpatch_text(mark, sizeof(mark), size, mtime);
    CHECK(!strcmp(mark, "2264924160 1742779432\n"));

    /* the same pkg, read back */
    CHECK(badpatch_matches(mark, size, mtime) == 1);
    /* another size (a re-download) or another time (a new update): tried again */
    CHECK(badpatch_matches(mark, size + 1, mtime) == 0);
    CHECK(badpatch_matches(mark, size, mtime + 1) == 0);
    CHECK(badpatch_matches(mark, 0, 0) == 0);

    /* the mark out of a file: spaces and no newline at the end still name it */
    CHECK(badpatch_matches("2264924160 1742779432", size, mtime) == 1);
    CHECK(badpatch_matches("  2264924160 1742779432\n\n", size, mtime) == 1);

    /* no mark: the update is used */
    CHECK(badpatch_matches(NULL, size, mtime) == 0);

    /* an empty mark names any update of the game (written when the pkg cannot be stat'ed) */
    CHECK(badpatch_matches("", size, mtime) == 1);
    CHECK(badpatch_matches(" \n", size, mtime) == 1);

    /* a mark that makes no sense is not a reason to retry it forever */
    CHECK(badpatch_matches("garbage", size, mtime) == 1);
    CHECK(badpatch_matches("2264924160", size, mtime) == 1);
    CHECK(badpatch_matches("2264924160 x", size, mtime) == 1);

    /* the size of a pkg over 4 GB (old code with a 32-bit type would name the wrong one) */
    badpatch_text(mark, sizeof(mark), 6000000000ull, 1742779432);
    CHECK(badpatch_matches(mark, 6000000000ull, 1742779432) == 1);
    CHECK(badpatch_matches(mark, 6000000000ull - (1ull << 32), 1742779432) == 0);

    /* the marks of 0.6.0 and older: the hex of the pkg digest, still honored */
    uint8_t digest[32];
    for (int i = 0; i < 32; i++)
        digest[i] = (uint8_t)(i * 7 + 1);
    char old[80];
    for (int i = 0; i < 32; i++)
        snprintf(old + i * 2, 3, "%02x", digest[i]);
    CHECK(badpatch_is_digest(old) == 1);
    CHECK(badpatch_digest_matches(old, digest) == 1);
    char upper[80];
    for (int i = 0; i < 32; i++)
        snprintf(upper + i * 2, 3, "%02X", digest[i]);
    CHECK(badpatch_digest_matches(upper, digest) == 1);
    CHECK(badpatch_digest_matches("  87264EB44AE9B9013333243F323123D15D9B3BCF9FCED1C8D716C5A8DE6C2FCC\n", digest) == 0);
    digest[31] ^= 1;
    CHECK(badpatch_digest_matches(old, digest) == 0);
    CHECK(badpatch_is_digest("2264924160 1742779432\n") == 0);
    CHECK(badpatch_is_digest(NULL) == 0);
    CHECK(badpatch_is_digest("") == 0);

    /* no room for the text: nothing is written, no mark is broken */
    char tiny[4] = "abc";
    badpatch_text(tiny, sizeof(tiny), size, mtime);
    CHECK(tiny[sizeof(tiny) - 1] == 0);
    badpatch_text(NULL, 0, size, mtime);

    printf("%s: %d check(s), %d failed\n", fails ? "FAIL" : "bad patch", 21, fails);
    return fails ? 1 : 0;
}
