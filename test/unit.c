/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Unit tests for the parts that run without a console: crypto, the install file
 * builders and the param.sfo parser. Build and run with: bash test/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "autorun.h"
#include "build.h"
#include "check.h"
#include "crypto.h"
#include "emu.h"
#include "lang.h"
#include "pkgmake.h"
#include "origin.h"
#include "pkg.h"
#include "stage.h"
#include "pdb_template.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static void hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++)
        sscanf(s + 2 * i, "%2hhx", &out[i]);
}

/* A pkg with only the fields the builders use. */
static void fake_pkg(pkg_t *p, const char *content_id, uint64_t size) {
    memset(p, 0, sizeof(*p));
    snprintf(p->content_id, sizeof(p->content_id), "%s", content_id);
    memcpy(p->title_id, content_id + 7, 9);
    p->pkg_size = size;
    for (int i = 0; i < 32; i++)
        p->digest[i] = (uint8_t)(0xa0 + i);
}

static void test_crypto(void) {
    printf("crypto\n");
    uint8_t h[32], want[32];
    sha256("abc", 3, h);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    CHECK(!memcmp(h, want, 32));
    sha256("", 0, h);
    hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want, 32);
    CHECK(!memcmp(h, want, 32));
    /* the same digest fed in pieces */
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, "a", 1);
    sha256_update(&c, "bc", 2);
    sha256_final(&c, h);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    CHECK(!memcmp(h, want, 32));
    CHECK(crc32c("123456789", 9) == 0xe3069283u);
}

static void test_app_json(void) {
    printf("app.json / ac.json\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTGAME00000000", 1234567);
    buf_t b = {0};
    build_app_json(&b, &p, "/mnt/usb0/ps4/CUSA00001/app.pkg");
    const char *want = "{\"numberOfSplitFiles\":1,\"originalFileSize\":1234567,\"packageDigest\":"
                       "\"A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF\",\"pieces\":[{\"fileOffset\":0,"
                       "\"fileSize\":1234567,\"url\":\"\\/mnt\\/usb0\\/ps4\\/CUSA00001\\/app.pkg\"}]}";
    CHECK(b.len == strlen(want) && !memcmp(b.data, want, b.len));
    buf_free(&b);
}

static void check_pbm(const pkg_t *p, int type) {
    buf_t b = {0};
    build_app_pbm(&b, p, "01.02", type);
    uint64_t blocks = (p->pkg_size + 0xffff) / 0x10000;
    size_t bitmap = (size_t)((blocks + 7) / 8);
    CHECK(b.len == 0x100 + bitmap + 32);
    CHECK(!memcmp(b.data, "pdbm", 4));
    CHECK(!memcmp(b.data + 4, p->title_id, 9));
    CHECK(!memcmp(b.data + 0x0e, "01.02", 5));
    CHECK(le32(b.data + 0x1e) == (uint32_t)type);
    CHECK(le64(b.data + 0x22) == blocks);
    CHECK(!memcmp(b.data + 0x60, p->digest, 32));
    CHECK(le32(b.data + 0xac) == bitmap);
    /* all blocks present; bits past the last block stay clear */
    CHECK(b.data[0x100] == 0xff);
    if (blocks % 8)
        CHECK(b.data[0x100 + bitmap - 1] == (uint8_t)(0xff << (8 - blocks % 8)));
    uint8_t h[32];
    sha256(b.data, b.len - 32, h);
    CHECK(!memcmp(b.data + b.len - 32, h, 32));
    buf_free(&b);
}

static void test_pbm(void) {
    printf("app.pbm / patch.pbm / ac.pbm\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTGAME00000000", 82706432); /* whole blocks */
    check_pbm(&p, PBM_APP);
    check_pbm(&p, PBM_PATCH);
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTADDON0000001", 524288 + 100); /* partial last block */
    check_pbm(&p, PBM_ADDCONT);
    check_pbm(&p, PBM_LICENSE);
}

/* Records of a pdb: u32 version, then (u32 key, u32 len, u32 len, data), CRC32C trailer. */
typedef struct {
    uint32_t key, len;
    const uint8_t *data;
} rec_t;

static int parse_pdb(const uint8_t *d, size_t n, rec_t *out, int max) {
    int c = 0;
    size_t off = 4;
    while (off + 12 <= n - 4 && c < max) {
        out[c].key = le32(d + off);
        out[c].len = le32(d + off + 4);
        out[c].data = d + off + 12;
        off += 12 + out[c].len;
        c++;
    }
    return off == n - 4 ? c : -1;
}

static const rec_t *find(const rec_t *r, int n, uint32_t key) {
    for (int i = 0; i < n; i++)
        if (r[i].key == key)
            return &r[i];
    return NULL;
}

static void test_ac_pdb(void) {
    printf("add-on PlayGo record\n");
    pkg_t p;
    fake_pkg(&p, "UP0000-CUSA00001_00-TESTADDON0000001", 31129600);
    const char *url = "/mnt/usb0/ps4/CUSA00001/addcont/CUSA00001/TESTADDON0000001/ac.pkg";
    buf_t json = {0}, pdb = {0};
    build_app_json(&json, &p, url);
    build_ac_pdb(&pdb, &p, url, &json);
    static rec_t got[256], tmpl[256];
    int n = parse_pdb(pdb.data, pdb.len, got, 256);
    int nt = parse_pdb(k_pdb_addcont_template, sizeof(k_pdb_addcont_template), tmpl, 256);
    CHECK(n > 0 && n == nt);
    for (int i = 0; n > 0 && i < n && i < nt; i++)
        CHECK(got[i].key == tmpl[i].key); /* same records in the same order as the installer */
    CHECK(le32(pdb.data) == le32(k_pdb_addcont_template));
    CHECK(le32(pdb.data + pdb.len - 4) == crc32c(pdb.data, pdb.len - 4));
    const rec_t *r;
    CHECK((r = find(got, n, 0xf4241)) && r->len == 36 && !memcmp(r->data, p.content_id, 36));
    CHECK((r = find(got, n, 0xf42a6)) && r->len == strlen(url) && !memcmp(r->data, url, r->len));
    CHECK((r = find(got, n, 0xf42a7)) && r->len == json.len && !memcmp(r->data, json.data, r->len));
    for (uint32_t k = 0xf4246; k <= 0xf4249; k++)
        CHECK((r = find(got, n, k)) && r->len == 8 && le64(r->data) == p.pkg_size);
    CHECK((r = find(got, n, 0xf431b)) && r->len == 53 && !memcmp(r->data, "0000000000000000-", 17) &&
          !memcmp(r->data + 17, p.content_id, 36));
    buf_free(&json);
    buf_free(&pdb);
}

/* PS4 BGFT tasks: u32 0, then records to the end of the file (no trailer). */
static int parse_task(const uint8_t *d, size_t n, rec_t *out, int max) {
    int c = 0;
    size_t off = 4;
    while (off + 12 <= n && c < max) {
        out[c].key = le32(d + off);
        out[c].len = le32(d + off + 4);
        out[c].data = d + off + 12;
        off += 12 + out[c].len;
        c++;
    }
    return off == n ? c : -1;
}

static int rec_is(const rec_t *r, int n, uint32_t key, const void *data, size_t len) {
    const rec_t *f = find(r, n, key);
    return f && f->len == len && !memcmp(f->data, data, len);
}

static void test_bgft_task(void) {
    printf("BGFT game task\n");
    pkg_t app, pat;
    fake_pkg(&app, "UP0000-CUSA00001_00-TESTGAME00000000", 271384576);
    fake_pkg(&pat, "UP0000-CUSA00001_00-TESTGAME00000000", 21430272);
    const char *url = "/mnt/usb0/ps4/CUSA00001/app.pkg", *purl = "/mnt/usb0/ps4/CUSA00001/patch.pkg";
    buf_t json = {0}, pjson = {0};
    build_app_json(&json, &app, url);
    build_app_json(&pjson, &pat, purl);
    patch_ref_t pr = {&pat, NULL, 0, "01.14", purl, &pjson};
    static rec_t r[128];
    uint8_t v[8];
    for (int with_patch = 0; with_patch < 2; with_patch++)
        for (int d1 = 0; d1 < 2; d1++) {
            bgft_task_t t = {d1, 0x13520001, 0x00e31e2713ddac1eull, 0xf092, &app, "app.pkg", url, &json,
                             with_patch ? &pr : NULL};
            buf_t b = {0};
            build_bgft_task(&b, &t);
            int n = parse_task(b.data, b.len, r, 128);
            CHECK(n == (with_patch ? 47 : 41));
            CHECK(le32(b.data) == 0);
            CHECK(r[0].key == 0x95 && le32(r[0].data) == 0x13520001);
            /* the creation time sits where place_bgft reads it back */
            CHECK(le32(b.data + 152) == 0x76 && le64(b.data + 164) == 0x00e31e2713ddac1eull);
            CHECK(rec_is(r, n, 0x74, app.content_id, 36));
            CHECK(rec_is(r, n, 0x7c, "CUSA00001", 9));
            CHECK(rec_is(r, n, 0x1388, url, strlen(url)));
            CHECK(rec_is(r, n, 0x1389, "/user/app/CUSA00001/app.pkg", 27));
            CHECK(rec_is(r, n, 0x1390, json.data, json.len));
            CHECK(find(r, n, 0x1a3) && le64(find(r, n, 0x1a3)->data) == app.pkg_size);
            uint64_t total = app.pkg_size + (with_patch ? pat.pkg_size : 0);
            CHECK(find(r, n, 0x7a) && le64(find(r, n, 0x7a)->data) == total);
            CHECK(find(r, n, 0x7b) && le64(find(r, n, 0x7b)->data) == total);
            CHECK(find(r, n, 0x1f4) && le32(find(r, n, 0x1f4)->data) == (with_patch ? 2u : 1u));
            CHECK(find(r, n, 0x1a4) && le32(find(r, n, 0x1a4)->data) == 0xf092);
            CHECK(find(r, n, 0x9a) && le64(find(r, n, 0x9a)->data) == (d1 ? 0x8000000000000000ull : 0));
            memset(v, 0, 8);
            v[7] = 0x10;
            CHECK(rec_is(r, n, 0x193, v, 8));
            CHECK(!with_patch == !find(r, n, 0x13a9));
            if (with_patch) {
                CHECK(rec_is(r, n, 0x13a8, purl, strlen(purl)));
                CHECK(rec_is(r, n, 0x13a9, "/user/patch/CUSA00001/patch.pkg", 31));
                CHECK(rec_is(r, n, 0x13b0, pjson.data, pjson.len));
                CHECK(rec_is(r, n, 0x84, "01.14", 5));
            }
            buf_free(&b);
        }
    buf_free(&json);
    buf_free(&pjson);
}

static void slot(uint8_t *lic, uint8_t v) { memset(lic, v, 0x400); }

static int idx_count(const buf_t *idx) { return (int)le32(idx->data + 4); }

static const uint8_t *idx_label(const buf_t *idx, int i) { return idx->data + 0x20 + i * 0x30; }

static void test_license(void) {
    printf("license files (rif/idx)\n");
    static uint8_t a[0x400], b[0x400], c[0x400];
    slot(a, 0x11);
    slot(b, 0x22);
    slot(c, 0x33);
    const char *game = "UP0000-CUSA00001_00-TESTGAME00000000", *dlc = "UP0000-CUSA00001_00-TESTADDON0000001";

    buf_t r1 = {0}, i1 = {0};
    build_license(&r1, &i1, NULL, 0, NULL, 0, game, a);
    CHECK(r1.len == 0x800 && !memcmp(r1.data, "rifa", 4) && !memcmp(r1.data + 4, game, 19));
    CHECK(!memcmp(r1.data + 0x400, a, 0x400));
    CHECK(i1.len == 0x50 && !memcmp(i1.data, "ridx", 4) && idx_count(&i1) == 1);
    CHECK(!memcmp(i1.data + 9, game, 19) && !memcmp(idx_label(&i1, 0), game + 20, 16));
    CHECK(le64(idx_label(&i1, 0) + 0x10) == 0x400 && le64(idx_label(&i1, 0) + 0x18) == 0x400);

    /* an add-on gets a second slot, the game keeps its own */
    buf_t r2 = {0}, i2 = {0};
    build_license(&r2, &i2, r1.data, r1.len, i1.data, i1.len, dlc, b);
    CHECK(r2.len == 0xc00 && idx_count(&i2) == 2 && i2.len == 0x80);
    CHECK(!memcmp(r2.data + 0x400, a, 0x400) && !memcmp(r2.data + 0x800, b, 0x400));
    CHECK(!memcmp(idx_label(&i2, 1), dlc + 20, 16) && le64(idx_label(&i2, 1) + 0x10) == 0x800);

    /* writing the game again replaces its slot and keeps the add-on */
    buf_t r3 = {0}, i3 = {0};
    build_license(&r3, &i3, r2.data, r2.len, i2.data, i2.len, game, c);
    CHECK(r3.len == 0xc00 && idx_count(&i3) == 2);
    CHECK(!memcmp(r3.data + 0x400, c, 0x400) && !memcmp(r3.data + 0x800, b, 0x400));

    /* files of another content ID prefix, or damaged ones, are replaced */
    buf_t r4 = {0}, i4 = {0};
    build_license(&r4, &i4, r2.data, r2.len, i2.data, i2.len, "UP0000-CUSA00002_00-OTHERGAME0000000", a);
    CHECK(r4.len == 0x800 && idx_count(&i4) == 1);
    buf_t r5 = {0}, i5 = {0};
    build_license(&r5, &i5, r2.data, 0x500, i2.data, i2.len, dlc, b); /* rif shorter than its slots */
    CHECK(r5.len == 0x800 && idx_count(&i5) == 1);

    buf_t *all[] = {&r1, &i1, &r2, &i2, &r3, &i3, &r4, &i4, &r5, &i5};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        buf_free(all[i]);
}

/* A param.sfo with one string and one integer. */
static size_t make_sfo(uint8_t *d) {
    memset(d, 0, 256);
    const char keys[] = "TITLE\0ATTRIBUTE\0";
    uint32_t ko = 20 + 2 * 16, dof = ko + (uint32_t)sizeof(keys);
    memcpy(d, "\0PSF", 4);
    d[4] = 1, d[5] = 1;
    d[8] = (uint8_t)ko, d[12] = (uint8_t)dof, d[16] = 2;
    uint8_t *e = d + 20;
    e[0] = 0, e[2] = 0x04, e[3] = 0x02, e[4] = 6, e[8] = 8, e[12] = 0; /* TITLE: utf8, "Title\0" */
    e += 16;
    e[0] = 6, e[2] = 0x04, e[3] = 0x04, e[4] = 4, e[8] = 4, e[12] = 8; /* ATTRIBUTE: int32 at 8 */
    memcpy(d + ko, keys, sizeof(keys));
    memcpy(d + dof, "Title", 6);
    d[dof + 8] = 0x34, d[dof + 9] = 0x12;
    return dof + 12;
}

static void test_sfo(void) {
    printf("param.sfo\n");
    uint8_t d[256];
    size_t n = make_sfo(d);
    sfo_t s;
    CHECK(sfo_parse(d, n, &s) == 0 && s.count == 2);
    const sfo_item_t *t = sfo_get(&s, "TITLE"), *a = sfo_get(&s, "ATTRIBUTE");
    CHECK(t && !t->is_int && !strcmp(t->sval, "Title"));
    CHECK(a && a->is_int && a->ival == 0x1234);
    CHECK(!sfo_get(&s, "CATEGORY"));
    d[1] = 'X'; /* not an SFO */
    CHECK(sfo_parse(d, n, &s) != 0);
}

static void touch(const char *path) {
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

static void test_check(void) {
    printf("pkg check\n");
    char root[] = "/tmp/ps4mount-test-XXXXXX", p[256];
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    snprintf(p, sizeof(p), "%s/x.pkg", root);
    const char *cid = "UP0000-CUSA00001_00-TESTGAME00000000";
    pkg_check_t c;

    make_pkg(p, cid, "gd", 0x09008000, 1, 0x100000, 0x100000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && c.ok && c.fake && c.kind == PKG_KIND_GAME && !c.problem[0]);
    CHECK(!strcmp(c.content_id, cid) && !strcmp(c.title_id, "CUSA00001") && !strcmp(c.title, "Test Game"));
    CHECK(!strcmp(c.app_ver, "01.04") && !strcmp(c.version, "01.00") && c.system_ver == 0x09008000);
    CHECK(c.pkg_size == 0x100000);

    /* only the start of a big pkg: everything is known already */
    make_pkg(p, cid, "gp", 0x09008000, 1, 30ull << 30, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && c.ok && c.kind == PKG_KIND_UPDATE && c.pkg_size == 30ull << 30);

    make_pkg(p, cid, "ac", 0, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && c.ok && c.kind == PKG_KIND_ADDON);

    /* built for a newer firmware than the console's */
    make_pkg(p, cid, "gd", 0x13500000, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x12520001, &c) == 0 && c.ok && strstr(c.warning, "firmware 13.50") &&
          strstr(c.warning, "12.52") && strstr(c.warning, "backport"));
    CHECK(pkg_check(p, 0x13500001, &c) == 0 && c.ok && !c.warning[0]); /* same firmware */
    CHECK(pkg_check(p, 0, &c) == 0 && c.ok && !c.warning[0]);          /* no firmware check */

    make_pkg(p, cid, "gd", 0x09008000, 0, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && !c.fake && strstr(c.problem, "fake"));
    CHECK(!c.installable); /* not fake: nothing to offer */

    /* homebrew ports and apps have title IDs of their own */
    make_pkg(p, "IV0000-SSBU00001_00-SSBURECOMPPS4000", "gd", 0, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && c.kind == PKG_KIND_GAME && strstr(c.problem, "homebrew app") &&
          strstr(c.problem, "SSBU00001") && c.installable); /* a few KB: an app, for the installer */
    CHECK(homebrew_small("SSBU00001", 600ull << 20) && !homebrew_small("SSBU00001", 5ull << 30) &&
          !homebrew_small("CUSA00001", 1 << 20)); /* a port is big; CUSA games are never held back */
    /* PS1/PS2/PSP Classic fPKGs (Sony prefixes) under 1 GB are games, not small homebrew apps */
    CHECK(!homebrew_small("SLUS20071", 600ull << 20) && !homebrew_small("SCUS94163", 400ull << 20) &&
          !homebrew_small("ULUS10041", 300ull << 20) && !homebrew_small("NPUJ00001", 200ull << 20));
    CHECK(homebrew_small("PCSX20007", 50ull << 20)); /* emulator app: installable homebrew */
    /* GET /api/games "platform" (idea 10: the panel's filter): public, the panel reads it */
    CHECK(!strcmp(game_platform("CUSA00001"), "ps4") && !strcmp(game_platform("SLUS20071"), "classic") &&
          !strcmp(game_platform("NPUJ00001"), "classic") && !strcmp(game_platform("SMWR00001"), "homebrew") &&
          !strcmp(game_platform("PCSX20007"), "homebrew"));
    CHECK(pkg_cat_is_game("gd") && pkg_cat_is_game("gdO") && pkg_cat_is_game("gdo") && !pkg_cat_is_game("gdc") &&
          !pkg_cat_is_game("gp") && !pkg_cat_is_game("ac"));
    make_pkg(p, "UP9000-SLUS20071_00-SLUS200710000001", "gdO", 0x05050000, 1, 600ull << 20, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && c.ok && c.kind == PKG_KIND_GAME && !strcmp(c.title_id, "SLUS20071"));
    /* multi-chunk bitmap (> 4096 bytes) in build_app_pbm */
    {
        pkg_t big = {0};
        snprintf(big.title_id, sizeof(big.title_id), "CUSA00001");
        big.pkg_size = (4ull << 30) + 0x10000ull; /* 65537 blocks -> 8193 bitmap bytes, last byte 0x80 */
        buf_t b = {0};
        build_app_pbm(&b, &big, "", 0);
        CHECK(b.len == 0x100 + 8193 + 32);
        CHECK(b.data[0x100] == 0xff && b.data[0x100 + 4096] == 0xff && b.data[0x100 + 8191] == 0xff &&
              b.data[0x100 + 8192] == 0x80);
        buf_free(&b);
    }
    CHECK(is_game_id("CUSA00001") && is_game_id("SSBU00001") && is_game_id("PCSX20007"));
    CHECK(!is_game_id("NPXS00001") && !is_game_id("PPSA00001") && !is_game_id("cusa00001") && !is_game_id("CUSA0001") &&
          !is_game_id("CUSA000011") && !is_game_id("CUS100001") && !is_game_id("CUSA0000X") && !is_game_id(""));
    CHECK(game_id_at("SSBU00001.src") && !game_id_at("SSBU0001"));

    make_pkg(p, "UP0000-NPXS00001_00-TESTGAME00000000", "gd", 0, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && strstr(c.problem, "not a game") && strstr(c.problem, "NPXS00001"));
    /* the same refusal in Spanish, with the same argument in it */
    g_lang = LANG_ES;
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && strstr(c.problem, "no un juego") && strstr(c.problem, "NPXS00001"));
    g_lang = LANG_EN;

    make_pkg(p, cid, "gdc", 0, 1, 0x4000, 0x4000);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && c.kind == PKG_KIND_OTHER);

    /* cut before param.sfo */
    make_pkg(p, cid, "gd", 0, 1, 0x100000, 0x1800);
    CHECK(pkg_check(p, 0x13520001, &c) == 0 && !c.ok && strstr(c.problem, "param.sfo"));

    FILE *o = fopen(p, "wb");
    fputs("not a pkg at all", o);
    fclose(o);
    CHECK(pkg_check(p, 0x13520001, &c) == -1);

    char v[16];
    ver_text(0x13520001, v, sizeof(v));
    CHECK(!strcmp(v, "13.52"));
    ver_text(0x05050000, v, sizeof(v));
    CHECK(!strcmp(v, "5.05"));
    CHECK(fw_runs(0x09008000, 0x13520001) && !fw_runs(0x13500000, 0x13020001) && fw_runs(0x13500000, 0x13520001));

    unlink(p);
    rmdir(root);
}

static void test_origin(void) {
    printf("origin records and restored list\n");
    char root[] = "/tmp/ps4mount-test-XXXXXX", game[128], list[128], out[256], p[256];
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    snprintf(game, sizeof(game), "%s/ps4", root);
    mkdir(game, 0777);
    snprintf(game, sizeof(game), "%s/ps4/CUSA00001", root);
    mkdir(game, 0777);

    CHECK(origin_find(game, "app.pkg", out, sizeof(out)) != 0); /* nothing recorded yet */
    CHECK(origin_add(game, "app.pkg", "games/My Game [v1.00].pkg") == 0);
    CHECK(origin_add(game, "patch.pkg", "Update 1.01.pkg") == 0);
    CHECK(origin_add(game, "addcont/CUSA00001/TESTADDON0000001/ac.pkg", "dlc/Pack.pkg") == 0);
    CHECK(origin_find(game, "app.pkg", out, sizeof(out)) == 0 && !strcmp(out, "games/My Game [v1.00].pkg"));
    CHECK(origin_find(game, "addcont/CUSA00001/TESTADDON0000001/ac.pkg", out, sizeof(out)) == 0 &&
          !strcmp(out, "dlc/Pack.pkg"));
    /* a newer update in the same place: the last record wins, the old one is kept aside */
    CHECK(origin_add(game, "replaced/patch-1.pkg", "Update 1.01.pkg") == 0);
    CHECK(origin_add(game, "patch.pkg", "Update 1.02.pkg") == 0);
    CHECK(origin_find(game, "patch.pkg", out, sizeof(out)) == 0 && !strcmp(out, "Update 1.02.pkg"));
    CHECK(origin_find(game, "replaced/patch-1.pkg", out, sizeof(out)) == 0 && !strcmp(out, "Update 1.01.pkg"));
    CHECK(origin_find(game, "patch", out, sizeof(out)) != 0); /* whole names only */
    CHECK(origin_add(game, "bad\tname", "x.pkg") != 0);

    /* free names in the original folder */
    snprintf(p, sizeof(p), "%s/Game.pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    touch(p);
    snprintf(p, sizeof(p), "%s/Game (1).pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    touch(p);
    snprintf(p, sizeof(p), "%s/Game (2).pkg", root);
    CHECK(origin_free_path(root, "Game.pkg", out, sizeof(out)) == 0 && !strcmp(out, p));
    snprintf(p, sizeof(p), "%s/v1.0/noext", root);
    CHECK(origin_free_path(root, "v1.0/noext", out, sizeof(out)) == 0 && !strcmp(out, p));

    /* restored pkgs are matched by path and size */
    snprintf(list, sizeof(list), "%s/restored.txt", root);
    CHECK(!restored_has(list, "Game.pkg", 100));
    CHECK(restored_add(list, "Game.pkg", 100) == 0);
    CHECK(restored_add(list, "games/Other.pkg", 5000000000ull) == 0);
    CHECK(restored_has(list, "Game.pkg", 100));
    CHECK(restored_has(list, "games/Other.pkg", 5000000000ull));
    CHECK(!restored_has(list, "Game.pkg", 101));    /* changed */
    CHECK(!restored_has(list, "Game2.pkg", 100));   /* renamed */
    CHECK(!restored_has(list, "x/Game.pkg", 100));  /* moved */

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    CHECK(system(cmd) == 0);
}

static int ini_ours(const char *s) { return autorun_is_ours(s, strlen(s)); }

/* autorun_edit_ini(raw, on) must give want, byte for byte (raw NULL: empty). */
static void ini_result(const char *raw, const char *path, int on, const char *want) {
    buf_t b = {0};
    CHECK(autorun_edit_ini((const uint8_t *)raw, raw ? strlen(raw) : 0, path, on, &b) == 0);
    size_t wl = strlen(want);
    int same = b.len == wl && !memcmp(b.data ? b.data : (const uint8_t *)"", want, wl);
    CHECK(same);
    if (!same)
        printf("    wanted %zu bytes \"%s\", got %zu\n", wl, want, b.len);
    buf_free(&b);
}

static size_t slurp(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return n;
}

static void test_autorun(void) {
    printf("autorun ini\n");
    const char *me = "/data/payloads/PS4Mount-PS4.bin";

    /* the matcher: our file, any path and case; not comments nor look-alikes */
    CHECK(ini_ours("/data/payloads/ps4mount-ps4.bin = 1"));
    CHECK(ini_ours("/data/GoldHEN/payloads/ps4mount-ps4.elf=1"));
    CHECK(ini_ours("ps4mount-ps4.bin"));
    CHECK(ini_ours("/x/PS4MOUNT-PS4.BIN\r"));
    CHECK(ini_ours("/x/ps4mount-ps4.bin\t= 1"));
    CHECK(!ini_ours("# /data/payloads/ps4mount-ps4.bin = 1"));
    CHECK(!ini_ours("   ; ps4mount-ps4.elf was here"));
    CHECK(!ini_ours("/x/myps4mount-ps4.bin = 1"));
    CHECK(!ini_ours("/x/ps4mount-ps4.binfoo = 1"));
    CHECK(!ini_ours("/x/ps4mount-ps4.bin.bak = 1"));
    CHECK(!ini_ours("/x/other.bin = 1"));
    CHECK(!ini_ours("[AutoRun]"));

    /* on with nothing there: a new section; off: nothing */
    ini_result(NULL, me, 1, "[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n");
    ini_result("", me, 1, "[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n");
    ini_result(NULL, NULL, 0, "");
    ini_result("", NULL, 0, "");
    {
        buf_t b = {0};
        CHECK(autorun_edit_ini(NULL, 0, NULL, 1, &b) == -1);
        CHECK(autorun_edit_ini(NULL, 0, "", 1, &b) == -1);
        CHECK(b.len == 0);
        buf_free(&b);
    }

    /* an existing [AutoRun]: ours goes right under the header, the rest stays */
    ini_result("[AutoRun]\n/data/payloads/other.bin = 1\n", me, 1,
               "[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n/data/payloads/other.bin = 1\n");

    /* [autorun] any case, spaces round it, CRLF endings */
    ini_result("  [AuToRun]  \r\n/data/other/x.bin = 1\r\n", me, 1,
               "  [AuToRun]  \r\n/data/payloads/PS4Mount-PS4.bin = 1\n/data/other/x.bin = 1\r\n");

    /* an older entry, any path, case or extension: replaced, not duplicated */
    ini_result("[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n"
               "/data/GoldHEN/payloads/ps4mount-ps4.elf = 1\n"
               "/data/payloads/other.bin = 1\n",
               me, 1,
               "[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n/data/payloads/other.bin = 1\n");

    /* other sections and comments stay verbatim; ours goes even from another section */
    ini_result("# mine\n[Other]\n/foo.bin = 1\n/old/ps4mount-ps4.bin = 1\n"
               "[AutoRun]\n# /data/ps4mount-ps4.bin = 1\n   ; ps4mount-ps4.elf\n/x.bin = 1\n",
               me, 1,
               "# mine\n[Other]\n/foo.bin = 1\n[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n"
               "# /data/ps4mount-ps4.bin = 1\n   ; ps4mount-ps4.elf\n/x.bin = 1\n");

    /* off: ours goes, everything else stays */
    ini_result("[AutoRun]\n/data/payloads/ps4mount-ps4.bin = 1\n"
               "/data/GoldHEN/payloads/PS4MOUNT-PS4.ELF = 1\n"
               "/data/payloads/other.bin = 1\n[Other]\n# c\n",
               NULL, 0,
               "[AutoRun]\n/data/payloads/other.bin = 1\n[Other]\n# c\n");

    /* a last line without a line end stays without one; a bare header gets one */
    ini_result("[AutoRun]\n/x.bin = 1", NULL, 0, "[AutoRun]\n/x.bin = 1");
    ini_result("[AutoRun]", me, 1, "[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n");
    ini_result("tail", me, 1, "tail\n\n[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n");
    ini_result("[X]\n", me, 1, "[X]\n\n[AutoRun]\n/data/payloads/PS4Mount-PS4.bin = 1\n");

    /* applying it twice changes nothing the second time */
    {
        static const char *const raws[] = {"# c\n[Other]\n/o.bin = 1\n[AutoRun]\n/old/ps4mount-ps4.bin = 1\n/p.bin = 1\n",
                                           "/x.bin = 1\n"};
        for (size_t i = 0; i < sizeof(raws) / sizeof(raws[0]); i++) {
            buf_t a = {0}, b = {0};
            CHECK(autorun_edit_ini((const uint8_t *)raws[i], strlen(raws[i]), me, 1, &a) == 0);
            CHECK(autorun_edit_ini(a.data, a.len, me, 1, &b) == 0);
            CHECK(a.len == b.len && !memcmp(a.data, b.data, a.len));
            buf_free(&a);
            buf_free(&b);
        }
    }

    /* the file is replaced whole: tmp + rename, nothing left behind */
    {
        char root[] = "/tmp/ps4mount-autorun-XXXXXX", ini[256], tmp[256], got[64];
        if (!mkdtemp(root)) {
            CHECK(0);
            return;
        }
        snprintf(ini, sizeof(ini), "%s/payloads.ini", root);
        snprintf(tmp, sizeof(tmp), "%s/payloads.ini.tmp", root);
        CHECK(autorun_write_atomic(ini, "first\n", 6) == 0);
        CHECK(access(tmp, F_OK) != 0);
        CHECK(slurp(ini, got, sizeof(got)) == 6 && !memcmp(got, "first\n", 6));
        CHECK(autorun_write_atomic(ini, "second, longer\n", 15) == 0);
        CHECK(access(tmp, F_OK) != 0);
        CHECK(slurp(ini, got, sizeof(got)) == 15 && !memcmp(got, "second, longer\n", 15));
        CHECK(autorun_write_atomic("/nonexistent-ps4mount-dir/f.ini", "x", 1) == -1);
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        CHECK(system(cmd) == 0);
    }
}

static void test_lang(void) {
    printf("languages\n");
    CHECK(g_lang == LANG_EN && !strcmp(TR("Yes", "Sí"), "Yes"));
    g_lang = LANG_ES;
    CHECK(!strcmp(TR("Yes", "Sí"), "Sí"));
    CHECK(!strcmp(TR("Only English"), "Only English")); /* a language a text lacks: English */
    g_lang = LANG_COUNT;                                 /* one with no column anywhere */
    CHECK(!strcmp(TR("Yes", "Sí"), "Yes"));
    g_lang = LANG_EN;
    CHECK(lang_from_console(3) == LANG_ES && lang_from_console(20) == LANG_ES);
    CHECK(lang_from_console(1) == LANG_EN && lang_from_console(7) == LANG_EN && lang_from_console(-1) == LANG_EN);
    CHECK(lang_from_tag("es") == LANG_ES && lang_from_tag("en") == LANG_EN && lang_from_tag("auto") == -1);
    CHECK(!strcmp(lang_tag(LANG_ES), "es") && !strcmp(lang_tag(-1), "en"));
}

/* STORE-1, STORE-3, STORE-4, STORE-5 (specs/storage.md): when PS4Mount may delete its own copy of a
 * pkg sent from the panel. A wrong row here deletes a file the system still needs, or leaves
 * gigabytes on the console's drive. */
static void test_stage(void) {
    printf("storage: when a staged copy may be deleted\n");
    static const struct {
        const char *name;
        long age;
        int receiving;
        stage_action_t want;
    } rows[] = {
        /* a whole pkg: left alone for 10 minutes, then deleted only if the system has the app, and
         * deleted whatever the system says once it is 3 days old */
        {"game.pkg", 0, 0, STAGE_KEEP},
        {"game.pkg", 599, 0, STAGE_KEEP},
        {"game.pkg", 600, 0, STAGE_VERIFY},
        {"game.pkg", 3 * 86400 - 1, 0, STAGE_VERIFY},
        {"game.pkg", 3 * 86400, 0, STAGE_DELETE_ABANDONED},
        {"game.pkg", 3 * 86400, 1, STAGE_DELETE_ABANDONED}, /* another pkg arriving does not protect it */
        {"game.pkg", -5, 0, STAGE_KEEP},                     /* the clock went back: never deleted */
        /* the part being received: only when nothing is being received, and not at once */
        {"game.pkg.part", 7199, 0, STAGE_KEEP},
        {"game.pkg.part", 7200, 0, STAGE_DELETE_PART},
        {"game.pkg.part", 7200, 1, STAGE_KEEP},
        {"game.pkg.part", 100 * 86400, 1, STAGE_KEEP},
        /* anything else in the folder is not PS4Mount's to delete */
        {"notes.txt", 100 * 86400, 0, STAGE_KEEP},
        {"game.pkg.bak", 100 * 86400, 0, STAGE_KEEP},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        stage_action_t got = stage_action(rows[i].name, rows[i].age, rows[i].receiving);
        if (got != rows[i].want)
            printf("  \"%s\" age %ld receiving %d: %d, wanted %d\n", rows[i].name, rows[i].age, rows[i].receiving, got,
                   rows[i].want);
        CHECK(got == rows[i].want);
    }
}

static void test_emu(void) {
    printf("emulators: which names are mounted over the console's folders\n");
    static const struct {
        const char *name;
        int ok;
    } names[] = {
        {"PS4SX2", 1}, {"roms", 1}, {"my roms_2.0-b", 1}, {"a", 1},
        {"", 0}, {".", 0}, {"..", 0}, {".hidden", 0},
        {"a/b", 0}, {"../x", 0}, {"/etc", 0}, {"a\\b", 0}, {"a\nb", 0}, {"caf\xc3\xa9", 0},
        {"123456789012345678901234567890123456789012345678", 1},  /* 48 */
        {"1234567890123456789012345678901234567890123456789", 0}, /* 49 */
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (emu_name_ok(names[i].name) != names[i].ok)
            printf("  name \"%s\" wanted %d\n", names[i].name, names[i].ok);
        CHECK(emu_name_ok(names[i].name) == names[i].ok);
    }
    char out[128];
    CHECK(emu_target("PS4SX2", "roms", out, sizeof(out)) == 0 && !strcmp(out, "/data/PS4SX2/roms"));
    CHECK(emu_target("PS4SX2", "..", out, sizeof(out)) == -1);
    CHECK(emu_target("..", "roms", out, sizeof(out)) == -1);
    /* the folders PS4Mount and the system use, in any case */
    const char *reserved[] = {"PS4Mount", "ps4mount", "PS4", "pkg", "GoldHEN", "goldhen", "payloads", "PAYLOADS"};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
        CHECK(emu_target(reserved[i], "roms", out, sizeof(out)) == -1);
    CHECK(emu_target("PS4Mounts", "roms", out, sizeof(out)) == 0); /* only the exact names */
    char tiny[16];
    CHECK(emu_target("PS4SX2", "roms", tiny, sizeof(tiny)) == -1); /* does not fit: not cut */
    /* the folder the games searches leave alone, and nothing else */
    CHECK(emu_is_dir("/mnt/usb0/PS4Mount/emulators"));
    CHECK(emu_is_dir("/mnt/usb0/ps4mount/Emulators"));
    CHECK(!emu_is_dir("/mnt/usb0/PS4Mount/emulators2"));
    CHECK(!emu_is_dir("/mnt/usb0/PS4Mount/Games/emulators"));
    CHECK(!emu_is_dir("PS4Mount/emulators/PS4SX2"));
    CHECK(!emu_is_dir(""));
}

int main(void) {
    test_crypto();
    test_app_json();
    test_pbm();
    test_ac_pdb();
    test_bgft_task();
    test_check();
    test_lang();
    test_license();
    test_sfo();
    test_origin();
    test_autorun();
    test_stage();
    test_emu();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
