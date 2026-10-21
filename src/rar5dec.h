/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * RAR5 volumes with a password made plain on their way to libarchive, which reads no
 * encrypted RAR. Each volume goes through a decoder as it arrives: its headers (encrypted
 * with rar -hp, or not) come out plain, without the encryption records, and the data of
 * each encrypted file comes out decrypted (AES-256-CBC, key by PBKDF2-HMAC-SHA256, as in
 * RARLAB's RAR 5.0 archive format notes). Anything else (a RAR4, a RAR5 without a
 * password) goes through as it is. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define RAR5_MAX_PASSWORDS 32
#define RAR5_PASSWORD_MAX 127 /* bytes of UTF-8, as rar takes them */

/* The passwords to try and what came of them, shared by the volumes of one archive. */
typedef struct rar5keys rar5keys_t;

rar5keys_t *rar5keys_new(void);
void rar5keys_free(rar5keys_t *k);
/* One more password to try (in the order given; empty or repeated ones are left out). */
void rar5keys_add(rar5keys_t *k, const char *password);
/* The password that opened it (NULL until one did). */
const char *rar5keys_found(const rar5keys_t *k);
/* 1 when the archive asked for a password none of the given opened (or none was given). */
int rar5keys_refused(const rar5keys_t *k);

typedef struct rar5dec rar5dec_t;

/* A decoder for one volume. */
rar5dec_t *rar5dec_new(rar5keys_t *k);
void rar5dec_free(rar5dec_t *d);
/* n bytes of the volume in: *out the plain bytes they give (maybe none, while a header is
   still coming), their count returned; -1 with why in rar5dec_error. */
ssize_t rar5dec_feed(rar5dec_t *d, const uint8_t *in, size_t n, const uint8_t **out);
/* The end of the volume: -1 when it was cut in the middle of something. */
int rar5dec_end(rar5dec_t *d);
const char *rar5dec_error(const rar5dec_t *d);
