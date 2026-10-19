/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The language of notifications and of the messages the panel shows: the console's, or
 * the one chosen in the settings. */
#pragma once

#include <stddef.h>

/* Columns of TR, in this order; a language missing from a TR falls back to English. */
enum { LANG_EN, LANG_ES, LANG_COUNT };

extern int g_lang;

/* TR("English", "Español"): the text in g_lang. Both texts take the same arguments in
 * the same order (test/policy.sh checks the conversions). */
#define TR(...) lang_pick((const char *const[]){__VA_ARGS__}, sizeof((const char *const[]){__VA_ARGS__}) / sizeof(char *))
const char *lang_pick(const char *const *texts, size_t n);

/* "en", "es": the language of the panel's texts. */
const char *lang_tag(int lang);
/* LANG_* for a tag ("es", "en"), -1 for one that is not known. */
int lang_from_tag(const char *tag);
/* LANG_* for a console language code (sceSystemServiceParamGetInt, parameter 1):
 * Spanish and Spanish (Latin America) are Spanish, any other English. */
int lang_from_console(int code);
