/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount daemon: scans USB drives, registers PS4 fpkgs without installing them, follows klog and serves the web panel. */
/* ps4mount: run PS4 fpkgs from external storage without installing them.
 *
 * Every scan:
 *   1. A PS4 app pkg found at the root of /mnt/usbN or /mnt/extN is moved to
 *      <root>/ps4/<TITLE_ID>/app.pkg (same disk, rename only).
 *   2. For each <root>/ps4/<TITLE_ID>/app.pkg the files a real install leaves
 *      behind are generated from the pkg into an internal folder next to a symlink
 *      to the pkg, that folder is mounted over /user/app/<TITLE_ID> (nullfs) and
 *      the title is added to app.db/appinfo.db.
 *   3. Mounts whose storage disappeared are released.
 * Titles installed the normal way are never touched.
 *
 * With organize off, step 1 is skipped: the pkgs are used where they are,
 * in any folder of the drive (the ps4/<TITLE_ID>/ folders of older versions included),
 * and the catalog (catalog.c) groups them per game from what they carry inside.
 * Nothing is renamed or moved. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <ps4/kernel.h>

#include "badpatch.h"
#include "build.h"
#include "catalog.h"
#include "check.h"
#include "upload.h"
#include "crypto.h"
#include "db.h"
#include "drive.h"
#include "links.h"
#include "fw.h"
#include "https.h"
#include "install.h"
#include "lang.h"
#include "logts.h"
#include "mem.h"
#include "netfs.h"
#include "origin.h"
#include "pkg.h"
#include "shortpath.h"
#include "titlecache.h"
#include "web.h"
#include "icon_data.h"

#define VERSION "PS4Mount 0.7.4-fix"
#define LOCK_PORT 9032
#define SCAN_SECONDS 10
/* A full walk of the drive is slower on drives with many files: the catalog is brought
 * up to date every so often, while the games already found are checked every scan. */
#define CATALOG_SECONDS 60
#define STATE_DIR "/data/ps4mount"
/* Internal folders mounted over /user/app/<TID> and /user/patch/<TID>: the files an
 * install leaves there plus app.pkg/patch.pkg as symlinks to the pkgs on the drive.
 * /user is bfs, which has no symlinks; /system_data is ufs. A delete from the
 * console only ever removes these links, never the pkgs. */
#define HOLD_DIR "/system_data/ps4mount"
#define MAX_TITLES 256
#define MAX_DLC 64
#define BATCH_QUIET 10 /* seconds without writes to the system databases between groups */
#define MAX_DLC_TIDS 8
/* Add-ons up to this size are installed with BGFT, not mounted: only the system
 * may write their record in addcont.db, which the game and its information page
 * read. Most are 1-2 MB licenses (some with a little data); copying them costs nothing. */
#define DLC_INSTALL_MAX (256ull << 20)

#define MAX_SCAN_PATHS 8
#define CONFIG_PATH STATE_DIR "/config.txt"
#define RESTORED_LIST STATE_DIR "/restored.txt" /* pkgs moved back: not picked up again */

/* Settings, kept in CONFIG_PATH and edited in the panel. */
typedef struct {
    int batch;      /* new games are added in groups */
    int batch_size; /* games per group */
    int dlc;        /* add-ons are moved and installed */
    char paths[MAX_SCAN_PATHS][128]; /* folders searched for pkgs, relative to each drive; "" is the root */
    int npaths;
    int restore_batch; /* games removed from the library per group when moving pkgs back */
    int internal;      /* pkgs in the console's /data/PS4Mount are used too */
    int organize;      /* pkgs are moved into <root>/ps4/<TID>/ (default); off: used where they are */
    int lang;          /* LANG_* of notifications and the panel, -1: the console's */
    int nas;           /* a network share is used too (firmwares with a fusefs patch, read only) */
    int nas_proto;     /* NAS_NFS or NAS_SMB */
    int nas_port;      /* 0: the protocol's port */
    char nas_server[64];
    char nas_share[256]; /* NFS export or SMB share name */
    char nas_sub[256];   /* folder inside it */
    char nas_user[64];   /* SMB; "" is the guest account */
    char nas_pass[64];
    int dlc_big;       /* add-ons over DLC_INSTALL_MAX are installed too (they take space on the console) */
    int nas_rescan;    /* minutes between looks at the share for new pkgs; 0: only when asked */
} config_t;

#define MAX_RESTORE_BATCH 10

/* The folder searched by default: one of PS4Mount's own, so nothing else on a drive is
 * moved until the user puts it there (the drive root, "", is still a choice). */
#define DEFAULT_SCAN_PATH "PS4Mount"

static config_t g_cfg = {1, 5, 1, {DEFAULT_SCAN_PATH}, 1, 5, 1, 1, -1, 0, NAS_NFS, 0, "", "", "", "", "", 1, 1};
#define NAS_RESCAN_MAX 1440 /* minutes: once a day */
static int g_setup; /* a first start: no game is added until the settings are saved (first_start) */
/* A first start with nobody at the panel for this long goes on with the default settings:
 * most never open the panel, and they would never get a game. */
#define SETUP_WAIT 120
static time_t g_setup_start;
static time_t g_setup_seen; /* the panel's last look at the list during a first start */

int sceSystemServiceParamGetInt(int param_id, int *value);
int sceSystemServicePowerTick(void);

/* The language in use: the one chosen in the settings, else the console's. */
#define REG_LANGUAGE 0x02020000 /* the system language in the registry */

/* The console's language. Asked to the system service it is always English (1) inside
 * GoldHEN's process, where AutoRun starts PS4Mount: the registry, read from the kernel,
 * gives the real one in any process. The service is the fallback. */
static int console_language(void) {
    int v = -1, reg = -1, rc = -1;
    unsigned res = 0;
    uint32_t h = 0;
    int loaded = 0;
    if (kernel_dynlib_handle(getpid(), "libSceRegMgr.sprx", &h)) { /* not in this process yet */
        int lh = sceKernelLoadStartModule("/system/common/lib/libSceRegMgr.sprx", 0, NULL, 0, NULL, &res);
        h = lh > 0 ? (uint32_t)lh : 0;
        loaded = lh;
    }
    int (*get)(long, int *) = h ? (void *)kernel_dynlib_dlsym(getpid(), h, "sceRegMgrGetInt") : NULL;
    if (get && !(rc = get(REG_LANGUAGE, &reg)) && reg >= 0)
        v = reg;
    if (v < 0 && sceSystemServiceParamGetInt(1, &v)) /* 1: the system language */
        v = -1;
    printf("[main] console language %d (registry %d: handle %u%s, call 0x%x)\n", v, reg, h,
           loaded ? " loaded" : "", (unsigned)rc);
    return v;
}

static void apply_lang(void) {
    static int console = -2;
    if (console == -2)
        console = console_language();
    g_lang = g_cfg.lang >= 0 ? g_cfg.lang : lang_from_console(console);
}

typedef struct { char useless[45]; char message[3075]; } notify_request_t;
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int32_t *);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

/* Set while the boot instance waits to be replaced by a load that can use the share:
 * what it says is only logged, so the console shows one start, the new one's. */
static int g_quiet;

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    printf("[notify]%s %s\n", g_quiet ? " (not shown)" : "", req.message);
    if (!g_quiet)
        sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* --- small fs helpers --- */

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    mkdir(tmp, mode);
}

static int write_file(const char *path, const void *data, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}

static int mounted_from(const char *mountpoint, char *from, size_t n) {
    struct statfs sf;
    if (statfs(mountpoint, &sf) || strcmp(sf.f_mntonname, mountpoint) || strcmp(sf.f_fstypename, "nullfs"))
        return 0;
    if (from)
        snprintf(from, n, "%s", sf.f_mntfromname);
    return 1;
}

/* Mounts on path from the mount table. Unlike statfs(path) this also sees a layer
 * whose source drive was disconnected (statfs on it fails). */
static int layers_on(const char *path) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT), count = 0;
    if (n <= 0)
        return 0;
    struct statfs *list = mem_alloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 0;
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].f_mntonname, path))
            count++;
    mem_free(list);
    return count;
}

/* A layer is on path but cannot be read: its drive was disconnected (and maybe
 * connected again, which gives it a new mount). */
static int dead_layer(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) && layers_on(path) > 0;
}

static int is_mountpoint(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) == 0 && !strcmp(sf.f_mntonname, path);
}

/* --- where games are looked for --- */

/* The USB drives, the extended storage, when turned on the console's own storage (pkgs
 * sent to /data/PS4Mount are registered from there, library /data/ps4, with no second
 * copy and no install to wait for; they still take internal space), and when turned on
 * a network share mounted read-only with FUSE. The share is never organized. */
#define NROOTS 12
#define NAS_ROOT 11
/* The share as PS4Mount and the system see it: a link to the live mount. The mountpoint
 * changes when PS4Mount is loaded again in the same boot (a mount cannot be removed), and
 * a path naming it, kept in the catalog, the game markers and the system's records,
 * would read the library again and register every game again on each load. */
#define NAS_ROOT_PATH STATE_DIR "/nas"
#define INTERNAL_ROOT "/data"

/* Points NAS_ROOT_PATH at the live mount when it moved. */
static void nas_link_update(void) {
    static char linked[PATH_MAX];
    const char *m = nas_mountpoint();
    if (!m[0] || !strcmp(linked, m))
        return;
    mkdir(STATE_DIR, 0777);
    unlink(NAS_ROOT_PATH);
    if (symlink(m, NAS_ROOT_PATH)) {
        printf("[nas] link %s: %s\n", NAS_ROOT_PATH, strerror(errno));
        return;
    }
    snprintf(linked, sizeof(linked), "%s", m);
    printf("[nas] %s -> %s\n", NAS_ROOT_PATH, m);
}

static void root_path(int i, char *out, size_t n) {
    if (i < 8)
        snprintf(out, n, "/mnt/usb%d", i);
    else if (i < 10)
        snprintf(out, n, "/mnt/ext%d", i - 8);
    else if (i == NAS_ROOT) {
        nas_link_update();
        snprintf(out, n, "%s", NAS_ROOT_PATH);
    }
    else
        snprintf(out, n, INTERNAL_ROOT);
}

static int is_internal(const char *root) { return !strcmp(root, INTERNAL_ROOT); }

/* A pkg path as the system writes it in its records: /data is an alias the payload can
 * use, but BGFT refuses it ("invalid content_url") and starts nothing; the installs of
 * the system say /user/data. */
static const char *sys_path(const char *path, char *out, size_t n) {
    if (!strncmp(path, INTERNAL_ROOT "/", sizeof(INTERNAL_ROOT)))
        snprintf(out, n, "/user%s", path);
    else
        snprintf(out, n, "%s", path);
    return out;
}

/* The pkg in the hold folder is a link to this: the pkg's own path when it is short, and
 * otherwise STATE_DIR/l/<TID>-<kind>.pkg, a short link to it (shortpath.h), made or fixed
 * here. The alias /data -> /user/data is not used: a link through it does not mount (the
 * system cannot build the pkg's block map, 0x8002000c). */
static const char *pkg_link(const char *tid, const char *kind, const char *path, char *out, size_t n) {
    char link[PATH_MAX];
    struct stat a, b;
    if (!short_pkg_link(STATE_DIR "/l", tid, kind, path, link, sizeof(link))) {
        /* the path fits: a link left from when the pkg was somewhere with a longer one goes */
        snprintf(link, sizeof(link), STATE_DIR "/l/%s-%s.pkg", tid, kind);
        unlink(link);
        snprintf(out, n, "%s", path);
        return out;
    }
    /* the pkg moved, or the drive it is on was mounted again: the link is made anew */
    if (stat(link, &a) || stat(path, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) {
        mkdirs(STATE_DIR "/l", 0777);
        unlink(link);
        if (symlink(path, link))
            printf("[reg] %s: short link %s: %s\n", tid, link, strerror(errno));
    }
    snprintf(out, n, "%s", link);
    return out;
}

/* The path handed to the system for a pkg in its records (app.json, BGFT): /user/data,
 * which is what the system writes and what BGFT accepts. */
static const char *pkg_url(const char *tid, const char *kind, const char *path, char *out, size_t n) {
    char p[PATH_MAX];
    return sys_path(pkg_link(tid, kind, path, p, sizeof(p)), out, n);
}

static int is_nas(const char *root) { return !strcmp(root, NAS_ROOT_PATH); }

/* Pkgs used where they are: with organizing off everywhere, and always on a share, which
 * is read-only and cannot be renamed into ps4/<TITLE_ID>/. */
static int in_place(const char *root) { return !g_cfg.organize || is_nas(root); }

static int root_present(const char *root) {
    if (is_nas(root))
        return nas_active();
    return is_internal(root) ? g_cfg.internal : is_mountpoint(root);
}

/* "usb0", "ext0", "internal storage", "NAS" */
static const char *root_label(const char *root) {
    if (is_nas(root))
        return "NAS";
    return is_internal(root) ? TR("internal storage", "memoria interna") : root + 5;
}

/* The short name of a drive in the state folder and in the panel. The share has one
 * whatever its mountpoint: its paths go through NAS_ROOT_PATH. */
static const char *root_id(const char *root, char *out, size_t n) {
    if (is_nas(root))
        snprintf(out, n, "%s", "nas");
    else if (is_internal(root))
        snprintf(out, n, "%s", "internal");
    else
        snprintf(out, n, "%s", root + 5);
    return out;
}

/* The pkgs found on each drive when they are used where they are (organize off). The
 * catalog is read by the main loop and by the Drive thread, so it is changed under a
 * lock; the scan holds it while it walks, which only delays a transfer. */
static catalog_t g_cat[NROOTS];
static int g_cat_loaded[NROOTS]; /* the saved catalog was read since the drive appeared */
static time_t g_cat_walked[NROOTS]; /* when the drive was last searched for pkgs */
static pthread_mutex_t g_cat_lock = PTHREAD_MUTEX_INITIALIZER;

static catalog_t *drive_catalog(const char *root) {
    char r[PATH_MAX];
    for (int i = 0; i < NROOTS; i++) {
        root_path(i, r, sizeof(r));
        if (!strcmp(r, root))
            return &g_cat[i];
    }
    return NULL;
}

/* 1 when the catalog of the drive at root has games to serve. */
static int catalog_has_games(const char *root) {
    const catalog_t *c = drive_catalog(root);
    return c && c->ngames > 0;
}

/* --- appmeta entries copied verbatim from the pkg --- */

typedef struct {
    uint32_t id;
    const char *name;
} meta_entry_t;

static const meta_entry_t k_user_meta[] = {
    {0x1200, "icon0.png"}, {0x1220, "pic0.png"},  {0x1006, "pic1.png"},          {0x1240, "snd0.at9"},
    {0x1280, "icon0.dds"}, {0x12a0, "pic0.dds"},  {0x1004, "pronunciation.xml"}, {0x100b, "shareparam.json"},
    {0, NULL},
};

static int write_entry(pkg_t *pkg, uint32_t id, const char *path) {
    size_t len;
    uint8_t *data = pkg_entry(pkg, id, &len);
    if (!data)
        return 1; /* absent entry is fine */
    int rc = write_file(path, data, len, 0777);
    mem_free(data);
    return rc;
}

static int write_buf(const char *path, const buf_t *b) { return write_file(path, b->data, b->len, 0777); }

/* --- registration --- */

/* An add-on: <root>/ps4/<TID>/addcont/<ADD-ON TID>/<LABEL>/ac.pkg. */
typedef struct {
    char path[PATH_MAX];
    pkg_t pkg;
    sfo_t sfo;
    int data;    /* 1: has data (mounted by the game), 0: only unlocks content */
    int install; /* 1: installed with BGFT (small, or big with dlc_big), 0: mounted */
} dlc_src_t;

/* One library folder: <root>/ps4/<TID>/app.pkg plus an optional patch.pkg and add-ons. */
typedef struct {
    char dir[PATH_MAX], app_path[PATH_MAX], patch_path[PATH_MAX];
    pkg_t app, patch;
    int has_patch;
    sfo_t app_sfo, patch_sfo;
    /* the update's param.sfo, as /system_data/priv/appmeta keeps it */
    unsigned char patch_sfo_sha[32];
    uint32_t patch_sfo_len;
    char name[128], ver[8], marker[PATH_MAX + 160];
    char update_issue[256]; /* why patch.pkg was ignored, empty when used or absent */
    dlc_src_t *dlc;
    int ndlc;
    char dlc_tids[MAX_DLC_TIDS][10]; /* add-on title IDs: /user/addcont/<TID> folders */
    int ndlc_tids;
} title_src_t;

/* title_info_t is title_src_t without the open pkgs (titlecache.h) */
_Static_assert(MAX_DLC_TIDS == TITLE_DLC_TIDS, "the cache keeps the add-on title IDs of a game");

/* sha and slen, when given, take the digest and length of the param.sfo (the launch
 * check compares the system's copy with it). */
static int read_sfo_ex(pkg_t *pkg, sfo_t *sfo, unsigned char *sha, uint32_t *slen) {
    size_t len;
    uint8_t *d = pkg_entry(pkg, 0x1000, &len);
    int rc = d ? sfo_parse(d, len, sfo) : -1;
    if (d && !rc && sha && slen && len <= 65536) {
        sha256(d, len, sha);
        *slen = (uint32_t)len;
    }
    mem_free(d);
    return rc;
}

static int read_sfo(pkg_t *pkg, sfo_t *sfo) { return read_sfo_ex(pkg, sfo, NULL, NULL); }

static void src_close(title_src_t *t) {
    pkg_close(&t->app);
    if (t->has_patch)
        pkg_close(&t->patch);
    for (int i = 0; i < t->ndlc; i++)
        pkg_close(&t->dlc[i].pkg);
    mem_free(t->dlc);
    t->dlc = NULL;
    t->ndlc = 0;
}

static int dlc_cmp(const void *a, const void *b) {
    return strcmp(((const dlc_src_t *)a)->pkg.content_id, ((const dlc_src_t *)b)->pkg.content_id);
}

/* Add-ons are used when their folder names match the pkg (title ID and label). */
static void dlc_open(title_src_t *t) {
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/addcont", t->dir);
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_game_id(e->d_name))
            continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", base, e->d_name);
        DIR *l = opendir(sub);
        if (!l)
            continue;
        struct dirent *f;
        while ((f = readdir(l))) {
            if (f->d_name[0] == '.' || strlen(f->d_name) != 16)
                continue;
            /* 6 MB: from mapped pages (zeroed), never the host's few MB of heap. A list that
             * could not be had would read as "no add-ons" and register the game again */
            if (!t->dlc && !(t->dlc = mem_alloc(MAX_DLC * sizeof(*t->dlc)))) {
                printf("[scan] %s: no memory for its add-ons\n", t->dir);
                break;
            }
            if (t->ndlc == MAX_DLC)
                break;
            dlc_src_t *a = &t->dlc[t->ndlc];
            snprintf(a->path, sizeof(a->path), "%s/%s/ac.pkg", sub, f->d_name);
            if (!exists(a->path))
                continue;
            if (pkg_open(&a->pkg, a->path) || read_sfo(&a->pkg, &a->sfo)) {
                if (a->pkg.fd > 0)
                    pkg_close(&a->pkg);
                continue;
            }
            const sfo_item_t *cat = sfo_get(&a->sfo, "CATEGORY");
            /* the fake key was checked when the pkg was moved here; checking it again on
             * every scan would cost an RSA operation per add-on */
            if (!cat || strcmp(cat->sval, "ac") || strcmp(a->pkg.title_id, e->d_name) ||
                strcmp(a->pkg.content_id + 20, f->d_name)) {
                pkg_close(&a->pkg);
                continue;
            }
            char rm[96];
            snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", a->pkg.content_id);
            if (exists(rm)) { /* deleted on the console */
                pkg_close(&a->pkg);
                continue;
            }
            a->data = a->pkg.header[0x77] == 0x1b; /* content type: 0x1b with data, 0x1c license only */
            a->install = a->pkg.pkg_size <= DLC_INSTALL_MAX || g_cfg.dlc_big;
            t->ndlc++;
            if (a->install)
                continue; /* not mounted: no /user/addcont folder of PS4Mount's */
            int known = 0;
            for (int i = 0; i < t->ndlc_tids; i++)
                known |= !strcmp(t->dlc_tids[i], e->d_name);
            if (!known && t->ndlc_tids < MAX_DLC_TIDS)
                snprintf(t->dlc_tids[t->ndlc_tids++], 10, "%s", e->d_name);
        }
        closedir(l);
    }
    closedir(d);
    if (t->ndlc)
        qsort(t->dlc, (size_t)t->ndlc, sizeof(*t->dlc), dlc_cmp);
}

static int first_time(const char *path);
static int find_root(const char *tid, char *root, size_t n);

/* Where the game is. Older versions moved the app pkg to <root>/ps4/<TID>/app.pkg and kept
 * its update and add-ons next to it; an in-place game has no folder of its own, and is the
 * pkg file itself (see the catalog for its update and add-ons). */
static void game_location(const char *root, const char *tid, const char *app_path, char *out, size_t n) {
    char lib[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4/%s/app.pkg", root, tid);
    if (!strcmp(app_path, lib))
        snprintf(out, n, "%s/ps4/%s", root, tid);
    else
        snprintf(out, n, "%s", app_path);
}

/* The add-ons the catalog grouped with the game, wherever they are. A second copy of an
 * add-on and the ones deleted on the console are left out. */
static void dlc_open_catalog(title_src_t *t, const catalog_t *c, const cat_game_t *g) {
    const cat_pkg_t *list[CAT_MAX_DLC];
    int nlist = catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC);
    for (int k = 0; k < nlist && t->ndlc < MAX_DLC; k++) {
        const cat_pkg_t *p = list[k];
        int dup = 0;
        for (int i = 0; i < t->ndlc; i++)
            dup |= !strcmp(t->dlc[i].pkg.content_id, p->content_id);
        char rm[96];
        snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", p->content_id);
        if (dup || exists(rm)) /* removed: deleted on the console */
            continue;
        /* 6 MB: from mapped pages (zeroed), never the host's few MB of heap. A list that
         * could not be had would read as "no add-ons" and register the game again */
        if (!t->dlc && !(t->dlc = mem_alloc(MAX_DLC * sizeof(*t->dlc)))) {
            printf("[scan] %s: no memory for its add-ons\n", t->dir);
            break;
        }
        dlc_src_t *a = &t->dlc[t->ndlc];
        snprintf(a->path, sizeof(a->path), "%s", p->path);
        /* the catalog checked the category and the fake key when it first read the pkg;
         * checking the key again on every scan would cost an RSA operation per add-on */
        if (pkg_open(&a->pkg, a->path) || read_sfo(&a->pkg, &a->sfo)) {
            if (a->pkg.fd >= 0)
                pkg_close(&a->pkg);
            continue;
        }
        a->data = a->pkg.header[0x77] == 0x1b; /* content type: 0x1b with data, 0x1c license only */
        a->install = a->pkg.pkg_size <= DLC_INSTALL_MAX || g_cfg.dlc_big;
        t->ndlc++;
        if (a->install)
            continue; /* not mounted: no /user/addcont folder of PS4Mount's */
        int known = 0;
        for (int i = 0; i < t->ndlc_tids; i++)
            known |= !strcmp(t->dlc_tids[i], a->pkg.title_id);
        if (!known && t->ndlc_tids < MAX_DLC_TIDS)
            snprintf(t->dlc_tids[t->ndlc_tids++], 10, "%s", a->pkg.title_id);
    }
    if (t->ndlc)
        qsort(t->dlc, (size_t)t->ndlc, sizeof(*t->dlc), dlc_cmp);
}

/* --- the updates the system turns down --- */

/* STATE_DIR/<TID>.badpatch names the update pkg the system refused (its size and time), so
 * a game the console will not start with it is served without it, and another update pkg
 * is tried again. */
static const char *badpatch_path(const char *tid, char *out, size_t n) {
    snprintf(out, n, STATE_DIR "/%s.badpatch", tid);
    return out;
}

/* 1 when the update at patch_path is the one the system turned down. */
static int patch_refused(const char *tid, const char *patch_path) {
    char p[64], text[64];
    struct stat st;
    badpatch_path(tid, p, sizeof(p));
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    text[n > 0 ? n : 0] = 0;
    if (badpatch_is_digest(text)) {
        /* a mark of 0.6.0 and older: the pkg's digest, in its 4 KB header */
        uint8_t hdr[0x1000], digest[32];
        fd = open(patch_path, O_RDONLY);
        if (fd < 0)
            return 0;
        ssize_t got = read(fd, hdr, sizeof(hdr));
        close(fd);
        if (got != (ssize_t)sizeof(hdr) || memcmp(hdr, "\x7f" "CNT", 4))
            return 0;
        memcpy(digest, hdr + 0xfe0, sizeof(digest));
        return badpatch_digest_matches(text, digest);
    }
    if (stat(patch_path, &st))
        return 0;
    return badpatch_matches(text, (uint64_t)st.st_size, (int64_t)st.st_mtime);
}

/* Writes the mark of the update at patch_path. 0 when it is written. */
static int write_badpatch(const char *tid, const char *patch_path) {
    char p[64], text[64];
    struct stat st;
    if (!patch_path[0] || stat(patch_path, &st))
        return -1;
    badpatch_path(tid, p, sizeof(p));
    badpatch_text(text, sizeof(text), (uint64_t)st.st_size, (int64_t)st.st_mtime);
    if (write_file(p, text, strlen(text), 0666)) {
        printf("[reg] %s: cannot keep the refused update: %s\n", tid, strerror(errno));
        return -1;
    }
    return 0;
}

/* Remembers that the system refused this update (CheckCumulativePatch: the console will
 * not start the game with it). The game is served without the update from now on. */
static void refuse_patch(const char *tid, const char *patch_path, const char *name) {
    if (write_badpatch(tid, patch_path))
        return;
    printf("[reg] %s: the system refuses update %s: left out until the pkg changes\n", tid, patch_path);
    notify(TR("%s: the update is left out, the system refuses it", "%s: se deja fuera la actualización, el sistema la rechaza"),
           name && name[0] ? name : tid);
}

/* The pkg of the update the drive has for tid (catalog or folder). 0 when there is none. */
static int title_patch_path(const char *root, const char *tid, char *out, size_t n) {
    out[0] = 0;
    if (in_place(root)) {
        const catalog_t *c = drive_catalog(root);
        const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
        const cat_pkg_t *p = g ? catalog_patch(g_cat, c, g) : NULL;
        if (!p)
            return 0;
        snprintf(out, n, "%s", p->path);
        return 1;
    }
    snprintf(out, n, "%s/ps4/%s/patch.pkg", root, tid);
    return exists(out);
}

/* Opens the pkgs of tid with the paths already chosen (dir, app_path and patch_path).
 * cat is NULL for the folders of older versions. */
static int src_open_paths(title_src_t *t, const char *tid, const catalog_t *cat, const cat_game_t *g) {
    if (pkg_open(&t->app, t->app_path) || read_sfo(&t->app, &t->app_sfo)) {
        if (first_time(t->app_path))
            printf("[scan] %s: unreadable app pkg\n", t->app_path);
        if (t->app.fd >= 0)
            pkg_close(&t->app);
        return -1;
    }
    if (strcmp(t->app.title_id, tid)) {
        printf("[scan] %s: the game pkg is for %s\n", t->app_path, t->app.title_id);
        pkg_close(&t->app);
        return -1;
    }
    if (t->patch_path[0] && exists(t->patch_path)) {
        const sfo_item_t *gv = sfo_get(&t->app_sfo, "VERSION");
        if (patch_refused(tid, t->patch_path)) {
            snprintf(t->update_issue, sizeof(t->update_issue), "%s",
                     TR("Update left out: the system refuses it. It is tried again when the pkg changes",
                        "Actualización descartada: el sistema la rechaza. Se vuelve a probar cuando cambie el pkg"));
        } else if (pkg_open(&t->patch, t->patch_path) ||
            read_sfo_ex(&t->patch, &t->patch_sfo, t->patch_sfo_sha, &t->patch_sfo_len)) {
            snprintf(t->update_issue, sizeof(t->update_issue), "%s",
                     TR("Update ignored: the update pkg could not be read",
                        "Actualización ignorada: no se pudo leer el pkg de la actualización"));
            if (t->patch.fd > 0)
                pkg_close(&t->patch);
        } else {
            /* An update only fits the title it was made for. (Master VERSION may differ:
             * installed games run with base 01.02 + update 01.00.) */
            const sfo_item_t *uv = sfo_get(&t->patch_sfo, "VERSION");
            const char *gver = gv ? gv->sval : "?", *uver = uv ? uv->sval : "?";
            if (strcmp(t->patch.title_id, tid)) {
                snprintf(t->update_issue, sizeof(t->update_issue),
                         TR("Update ignored: it is for %s version %s, the game is %s version %s",
                            "Actualización ignorada: es para %s versión %s, el juego es %s versión %s"),
                         t->patch.title_id, uver, tid, gver);
                pkg_close(&t->patch);
            } else {
                t->has_patch = 1;
            }
        }
        if (t->update_issue[0] && first_time(t->patch_path)) {
            const sfo_item_t *title = sfo_get(&t->app_sfo, "TITLE");
            notify("%s: %s", title ? title->sval : tid, t->update_issue);
        }
    }
    const sfo_t *eff = t->has_patch ? &t->patch_sfo : &t->app_sfo;
    const sfo_item_t *it = sfo_get(eff, "TITLE");
    snprintf(t->name, sizeof(t->name), "%s", it ? it->sval : tid);
    it = sfo_get(eff, "APP_VER");
    snprintf(t->ver, sizeof(t->ver), "%s", it && !it->is_int ? it->sval : "01.00");
    char digest[65] = "";
    if (t->has_patch)
        for (int i = 0; i < 32; i++)
            snprintf(digest + i * 2, 3, "%02x", t->patch.digest[i]);
    snprintf(t->marker, sizeof(t->marker), "%s\n%s\n", t->dir, t->has_patch ? digest : "nopatch");
    if (g_cfg.dlc) {
        if (cat)
            dlc_open_catalog(t, cat, g);
        else
            dlc_open(t);
    }
    if (t->ndlc) {
        /* one line for all add-ons, so markers of titles without add-ons stay as before */
        uint8_t h[32];
        buf_t all = {0};
        for (int i = 0; i < t->ndlc; i++)
            buf_put(&all, t->dlc[i].pkg.digest, 32);
        sha256(all.data, all.len, h);
        buf_free(&all);
        size_t n = strlen(t->marker);
        snprintf(t->marker + n, sizeof(t->marker) - n, "dlc %d ", t->ndlc);
        for (int i = 0; i < 32; i++) {
            n = strlen(t->marker);
            snprintf(t->marker + n, sizeof(t->marker) - n, "%02x", h[i]);
        }
        n = strlen(t->marker);
        snprintf(t->marker + n, sizeof(t->marker) - n, "\n");
    }
    return 0;
}

/* Opens the pkgs PS4Mount serves for tid from root: its ps4/<TID> folder (organized) or,
 * with organize off, where the catalog of the drive found them. */
static int src_open(title_src_t *t, const char *root, const char *tid) {
    memset(t, 0, sizeof(*t));
    if (in_place(root)) {
        const catalog_t *cat = drive_catalog(root);
        const cat_game_t *g = cat ? catalog_game(cat, tid) : NULL;
        if (!g)
            return -1;
        snprintf(t->app_path, sizeof(t->app_path), "%s", cat->pkgs[g->app].path);
        const cat_pkg_t *patch = catalog_patch(g_cat, cat, g);
        if (patch)
            snprintf(t->patch_path, sizeof(t->patch_path), "%s", patch->path);
        game_location(root, tid, t->app_path, t->dir, sizeof(t->dir));
        return src_open_paths(t, tid, cat, g);
    }
    snprintf(t->dir, sizeof(t->dir), "%s/ps4/%s", root, tid);
    snprintf(t->app_path, sizeof(t->app_path), "%s/app.pkg", t->dir);
    snprintf(t->patch_path, sizeof(t->patch_path), "%s/patch.pkg", t->dir);
    return src_open_paths(t, tid, NULL, NULL);
}

/* --- what each game is, kept between looks (titlecache.c) --- */

/* One file in the hash: its path, size and time. A missing one still counts (a pkg that
 * appears or goes away changes the fingerprint). */
static uint64_t fp_stat(uint64_t h, const char *path) {
    struct stat st;
    if (!path[0] || stat(path, &st))
        return title_fp_add(h, path, UINT64_MAX, 0);
    return title_fp_add(h, path, (uint64_t)st.st_size, (int64_t)st.st_mtime);
}

/* The add-ons of the game, added to h; order independent (they come from a folder or a
 * catalog, in no fixed order). */
static uint64_t fp_addons(uint64_t h, const char *root, const char *tid) {
    char x[40];
    uint64_t dh = 0;
    int nd = 0;
    if (in_place(root)) {
        const catalog_t *c = drive_catalog(root);
        const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
        const cat_pkg_t *list[CAT_MAX_DLC];
        int nl = g ? catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC) : 0;
        for (int k = 0; k < nl; k++) {
            char rm[96];
            uint64_t v = fp_stat(title_fp_start(), list[k]->path);
            snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", list[k]->content_id);
            if (exists(rm)) /* deleted on the console: src_open leaves it out too */
                v ^= 0x9e3779b97f4a7c15ull;
            dh ^= v;
            nd++;
        }
    } else {
        char base[PATH_MAX];
        snprintf(base, sizeof(base), "%s/ps4/%s/addcont", root, tid);
        DIR *d = opendir(base);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!is_game_id(e->d_name))
                continue;
            char sub[PATH_MAX];
            snprintf(sub, sizeof(sub), "%s/%s", base, e->d_name);
            DIR *l = opendir(sub);
            struct dirent *f;
            while (l && (f = readdir(l))) {
                if (f->d_name[0] == '.' || strlen(f->d_name) != 16)
                    continue;
                char p[PATH_MAX];
                snprintf(p, sizeof(p), "%s/%s/ac.pkg", sub, f->d_name);
                dh ^= fp_stat(title_fp_start(), p);
                nd++;
            }
            if (l)
                closedir(l);
        }
        if (d)
            closedir(d);
    }
    snprintf(x, sizeof(x), "addons %016llx", (unsigned long long)dh);
    return title_fp_add(h, x, (uint64_t)nd, 0);
}

/* The hash of the files of tid on root, from stats only: over a share, reading a pkg is
 * a megabyte where each of these is one getattr. */
static uint64_t title_files_fp(const char *root, const char *tid) {
    char app[PATH_MAX], patch[PATH_MAX];
    app[0] = patch[0] = 0;
    if (in_place(root)) {
        const catalog_t *c = drive_catalog(root);
        const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
        if (!g)
            return 0;
        snprintf(app, sizeof(app), "%s", c->pkgs[g->app].path);
        const cat_pkg_t *p = catalog_patch(g_cat, c, g);
        if (p)
            snprintf(patch, sizeof(patch), "%s", p->path);
    } else {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/ps4/%s", root, tid);
        snprintf(app, sizeof(app), "%s/app.pkg", dir);
        snprintf(patch, sizeof(patch), "%s/patch.pkg", dir);
    }
    uint64_t h = fp_stat(title_fp_start(), app);
    h = fp_stat(h, patch);
    /* an update left out (or tried again) changes what the game is */
    char mark[96];
    h = fp_stat(h, badpatch_path(tid, mark, sizeof(mark)));
    h = title_fp_add(h, g_cfg.dlc ? "add-ons" : "no add-ons", 0, g_cfg.dlc_big ? 1 : 0);
    return g_cfg.dlc ? fp_addons(h, root, tid) : h;
}

static void info_from_src(const title_src_t *t, title_info_t *e) {
    snprintf(e->title_id, sizeof(e->title_id), "%s", t->app.title_id);
    snprintf(e->dir, sizeof(e->dir), "%s", t->dir);
    snprintf(e->app_path, sizeof(e->app_path), "%s", t->app_path);
    snprintf(e->patch_path, sizeof(e->patch_path), "%s", t->patch_path);
    e->has_patch = t->has_patch;
    e->app_size = t->app.pkg_size;
    e->patch_size = t->has_patch ? t->patch.pkg_size : 0;
    snprintf(e->name, sizeof(e->name), "%s", t->name);
    snprintf(e->ver, sizeof(e->ver), "%s", t->ver);
    snprintf(e->content_id, sizeof(e->content_id), "%s", t->app.content_id);
    snprintf(e->update_issue, sizeof(e->update_issue), "%s", t->update_issue);
    snprintf(e->marker, sizeof(e->marker), "%s", t->marker);
    const sfo_t *eff = t->has_patch ? &t->patch_sfo : &t->app_sfo;
    const sfo_item_t *sv = sfo_get(eff, "SYSTEM_VER"), *av = sfo_get(&t->app_sfo, "APP_VER");
    e->system_ver = sv && sv->is_int ? sv->ival : 0;
    snprintf(e->game_ver, sizeof(e->game_ver), "%s", av && !av->is_int ? av->sval : "");
    if (t->has_patch) {
        memcpy(e->sfo_sha, t->patch_sfo_sha, sizeof(e->sfo_sha));
        e->sfo_len = t->patch_sfo_len;
    }
    e->ndlc_tids = t->ndlc_tids;
    memcpy(e->dlc_tids, t->dlc_tids, sizeof(e->dlc_tids));
    if (t->ndlc > 0) {
        title_dlc_t *d = title_cache_give_dlc(e, t->ndlc);
        if (d)
            for (int i = 0; i < t->ndlc; i++) {
                snprintf(d[i].title_id, sizeof(d[i].title_id), "%s", t->dlc[i].pkg.title_id);
                snprintf(d[i].content_id, sizeof(d[i].content_id), "%s", t->dlc[i].pkg.content_id);
                d[i].install = t->dlc[i].install;
            }
        e->ndlc = d ? t->ndlc : 0; /* no room: the game goes without them */
    }
}

/* What tid is, read from its pkgs only when its files changed since the last look (or it
 * was never looked at). NULL when the drive has no such game. */
static const title_info_t *title_info(const char *root, const char *tid) {
    uint64_t fp = title_files_fp(root, tid);
    if (!fp)
        return NULL;
    /* the games are kept per drive, not per path: a share mounted under another name in
     * the same boot must not be read again (its games are registered again, though) */
    char key[64];
    root_id(root, key, sizeof(key));
    const title_info_t *hit = title_cache_find(key, tid, fp);
    if (hit)
        return hit;
    title_src_t src;
    if (src_open(&src, root, tid))
        return NULL;
    title_info_t *e = title_cache_add(key, tid, fp);
    if (e)
        info_from_src(&src, e);
    src_close(&src);
    return e;
}

static void write_meta(pkg_t *pkg, const char *meta_dir, int *rc) {
    char q[PATH_MAX];
    for (const meta_entry_t *m = k_user_meta; m->name; m++) {
        snprintf(q, sizeof(q), "%s/%s", meta_dir, m->name);
        *rc |= write_entry(pkg, m->id, q) < 0;
    }
    for (uint32_t id = 0x1260; id <= 0x127f; id++) {
        size_t len;
        uint8_t *d = pkg_entry(pkg, id, &len);
        if (!d)
            continue;
        snprintf(q, sizeof(q), "%s/changeinfo", meta_dir);
        mkdirs(q, 0777);
        if (id == 0x1260)
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo.xml", meta_dir);
        else
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo_%02u.xml", meta_dir, id - 0x1261);
        *rc |= write_file(q, d, len, 0777);
        mem_free(d);
    }
}

static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode))
            rm_tree(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static void hold_path(char *out, size_t n, const char *kind, const char *tid) {
    snprintf(out, n, HOLD_DIR "/%s/%s", kind, tid);
}

/* Folder where 0.1.0-beta builds before the release mounted read-only views of the
 * libraries for the links: BGFT opens a game's pkg for writing when the game starts, and
 * through a read-only view it records EROFS in the game task and refuses the game. Links
 * ending there are written again. */
#define RO_DIR HOLD_DIR "/ro"

/* Fresh internal folder for kind ("app"/"patch") with name -> pkg_path. */
static int hold_prepare(const char *kind, const char *tid, const char *name, const char *pkg_path) {
    char dir[PATH_MAX], link[PATH_MAX];
    hold_path(dir, sizeof(dir), kind, tid);
    mkdirs(dir, 0777);
    snprintf(link, sizeof(link), "%s/%s", dir, name);
    unlink(link);
    if (symlink(pkg_path, link)) {
        printf("[reg] symlink %s: %s\n", link, strerror(errno));
        return -1;
    }
    return 0;
}

/* The link of kind reaches its pkg, not through a read-only view (see RO_DIR). readlink
 * fails for payloads here (0xffffffd1), so the link is followed instead: it must end on
 * the pkg itself. */
static int hold_ready(const char *kind, const char *tid, const char *name, const char *pkg_path) {
    char p[PATH_MAX];
    struct stat a, b;
    struct statfs sf;
    hold_path(p, sizeof(p), kind, tid);
    snprintf(p + strlen(p), sizeof(p) - strlen(p), "/%s", name);
    if (stat(p, &a) || stat(pkg_path, &b) || statfs(p, &sf))
        return 0;
    return a.st_ino == b.st_ino && strncmp(sf.f_mntonname, RO_DIR "/", sizeof(RO_DIR));
}

static uint8_t *read_all(const char *path, size_t *len) {
    struct stat st;
    *len = 0;
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > (1 << 20))
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    uint8_t *d = mem_alloc((size_t)st.st_size);
    if (d && read(fd, d, (size_t)st.st_size) != st.st_size) {
        mem_free(d);
        d = NULL;
    }
    close(fd);
    if (d)
        *len = (size_t)st.st_size;
    return d;
}

/* Adds or replaces the license slot of pkg in the rif/idx of its content ID prefix,
 * keeping the slots of the game and of other add-ons. */
static int place_license(pkg_t *pkg) {
    size_t lic_len, rl, il;
    uint8_t *lic = pkg_entry(pkg, 0x400, &lic_len);
    if (!lic || lic_len != 0x400) {
        mem_free(lic);
        printf("[reg] %s: no license.dat\n", pkg->content_id);
        return -1;
    }
    char rp[96], ip[96];
    /* the PS4 names them free<CID>, the PS5 fake<CID> */
    snprintf(rp, sizeof(rp), "/user/license/free%.19s.rif", pkg->content_id);
    snprintf(ip, sizeof(ip), "/user/license/free%.19s.idx", pkg->content_id);
    uint8_t *old_rif = read_all(rp, &rl), *old_idx = read_all(ip, &il);
    buf_t rif = {0}, idx = {0};
    build_license(&rif, &idx, old_rif, rl, old_idx, il, pkg->content_id, lic);
    int rc = write_buf(rp, &rif) | write_buf(ip, &idx);
    mem_free(lic);
    mem_free(old_rif);
    mem_free(old_idx);
    buf_free(&rif);
    buf_free(&idx);
    return rc ? -1 : 0;
}

/* Internal folder of an add-on: HOLD_DIR/addcont/<ADD-ON TID>/<LABEL>, mounted (by
 * title ID) over /user/addcont/<ADD-ON TID>. It holds ac.pkg as a symlink to the pkg on
 * the drive plus ac.json/ac.pbm. Also writes the PlayGo records, the license slot and,
 * for add-ons with data, the icon. */
static int place_dlc(dlc_src_t *a) {
    pkg_t *pkg = &a->pkg;
    const sfo_item_t *v = sfo_get(&a->sfo, "VERSION");
    const char *ver = v && !v->is_int ? v->sval : "01.00", *label = pkg->content_id + 20;
    char dir[PATH_MAX], p[PATH_MAX], q[PATH_MAX];
    snprintf(dir, sizeof(dir), HOLD_DIR "/addcont/%s/%s", pkg->title_id, label);
    mkdirs(dir, 0777);
    buf_t json = {0}, pbm = {0}, pdb = {0};
    char url[PATH_MAX + 8], kind[24];
    snprintf(kind, sizeof(kind), "ac-%.16s", label);
    pkg_url(pkg->title_id, kind, a->path, url, sizeof(url));
    snprintf(q, sizeof(q), "%s/ac.pkg", dir);
    unlink(q);
    int rc = 0;
    if (symlink(url, q)) {
        printf("[reg] symlink %s: %s\n", q, strerror(errno));
        rc = -1;
    }
    build_app_json(&json, pkg, url);
    build_app_pbm(&pbm, pkg, ver, a->data ? PBM_ADDCONT : PBM_LICENSE);
    build_ac_pdb(&pdb, pkg, url, &json);
    snprintf(q, sizeof(q), "%s/ac.json", dir);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/ac.pbm", dir);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/ac.pbm.backup", dir);
    rc |= write_buf(q, &pbm);
    snprintf(p, sizeof(p), "/user/playgo/addcont/%s/00", pkg->content_id);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/d0.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/d1.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/f0.pdb", p);
    rc |= write_file(q, "", 0, 0777);
    rc |= place_license(pkg);
    if (a->data) {
        snprintf(p, sizeof(p), "/user/appmeta/addcont/%s/%s", pkg->title_id, label);
        mkdirs(p, 0777);
        snprintf(q, sizeof(q), "%s/icon0.png", p);
        rc |= write_entry(pkg, 0x1200, q) < 0;
    }
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&pdb);
    if (rc)
        printf("[reg] add-on %s: could not write its files\n", pkg->content_id);
    return rc ? -1 : 0;
}

/* The ac.pkg link of every add-on exists and reaches its pkg. */
/* 1 when the add-on is installed the system's way: its own pkg in /user/addcont, not
 * PS4Mount's mount there. */
static void unmount_all_layers(const char *target, int force);

static int dlc_installed_ids(const char *dtid, const char *cid) {
    char p[96];
    struct stat st;
    snprintf(p, sizeof(p), "/user/addcont/%s", dtid);
    if (mounted_from(p, NULL, 0))
        /* an older PS4Mount left the add-on folder mounted over the real one: what the
         * system installed is below it, and would be installed again on every registration */
        unmount_all_layers(p, 1);
    snprintf(p, sizeof(p), "/user/addcont/%s/%s/ac.pkg", dtid, cid + 20);
    return !lstat(p, &st) && S_ISREG(st.st_mode);
}

static int dlc_installed(const dlc_src_t *a) { return dlc_installed_ids(a->pkg.title_id, a->pkg.content_id); }

/* Add-ons the system's installer would not take in this run: the game goes on without
 * them (and is not registered again for them on every scan); the next PS4Mount tries
 * again, after the restart a new game needs anyway. */
#define MAX_DLC_FAILED 64
static char g_dlc_failed[MAX_DLC_FAILED][40];
static int g_ndlc_failed;

static int dlc_failed(const char *content_id) {
    for (int i = 0; i < g_ndlc_failed; i++)
        if (!strcmp(g_dlc_failed[i], content_id))
            return 1;
    return 0;
}

/* Add-ons the installer would not take in an earlier load, with the size and digest of the
 * pkg they were: they are not tried again until that pkg changes, so a backup the system
 * refuses is not installed again at every load. Delete the file to try them all again. */
#define MAX_DLC_REFUSED 256
#define DLC_REFUSED_LIST STATE_DIR "/dlcfail.txt"
typedef struct {
    char content_id[40];
    uint64_t size;
    char digest[65];
} dlc_refused_t;
static dlc_refused_t g_dlc_refused[MAX_DLC_REFUSED];
static int g_ndlc_refused, g_dlc_refused_loaded;

static void dlc_hex(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
}

static void dlc_refused_load(void) {
    if (g_dlc_refused_loaded)
        return;
    g_dlc_refused_loaded = 1;
    FILE *f = fopen(DLC_REFUSED_LIST, "r");
    char line[160];
    if (!f)
        return;
    while (g_ndlc_refused < MAX_DLC_REFUSED && fgets(line, sizeof(line), f)) {
        dlc_refused_t *r = &g_dlc_refused[g_ndlc_refused];
        unsigned long long sz = 0;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%39s %llu %64s", r->content_id, &sz, r->digest) == 3) {
            r->size = sz;
            g_ndlc_refused++;
        }
    }
    fclose(f);
}

static int dlc_refused(const dlc_src_t *a) {
    char dig[65];
    dlc_refused_load();
    dlc_hex(a->pkg.digest, dig);
    for (int i = 0; i < g_ndlc_refused; i++)
        if (!strcmp(g_dlc_refused[i].content_id, a->pkg.content_id) && g_dlc_refused[i].size == a->pkg.pkg_size &&
            !strcmp(g_dlc_refused[i].digest, dig))
            return 1;
    return 0;
}

static void dlc_refused_add(const dlc_src_t *a) {
    dlc_refused_load();
    int i;
    for (i = 0; i < g_ndlc_refused; i++)
        if (!strcmp(g_dlc_refused[i].content_id, a->pkg.content_id))
            break;
    if (i == g_ndlc_refused) {
        if (i >= MAX_DLC_REFUSED) { /* the oldest makes room */
            memmove(&g_dlc_refused[0], &g_dlc_refused[1], sizeof(g_dlc_refused[0]) * (MAX_DLC_REFUSED - 1));
            i = MAX_DLC_REFUSED - 1;
        } else {
            g_ndlc_refused++;
        }
    }
    dlc_refused_t *r = &g_dlc_refused[i];
    snprintf(r->content_id, sizeof(r->content_id), "%s", a->pkg.content_id);
    r->size = a->pkg.pkg_size;
    dlc_hex(a->pkg.digest, r->digest);
    FILE *f = fopen(DLC_REFUSED_LIST, "w");
    if (!f)
        return;
    for (int k = 0; k < g_ndlc_refused; k++)
        fprintf(f, "%s\t%llu\t%s\n", g_dlc_refused[k].content_id, (unsigned long long)g_dlc_refused[k].size,
                g_dlc_refused[k].digest);
    fclose(f);
}

/* Big add-ons being installed: the system copies them in the background, minutes for a
 * few GB, while PS4Mount goes on. Each one is said when it is there. */
#define MAX_DLC_PENDING 64
#define DLC_SPACE_MARGIN (2ull << 30) /* left free on the console after an install */
#define DLC_PENDING_SECONDS (2 * 3600) /* an install not done by then is given up for this run */

typedef struct {
    char content_id[40], name[128];
    uint64_t size;
    time_t since;
} dlc_pending_t;

static dlc_pending_t g_dlc_pending[MAX_DLC_PENDING];
static int g_ndlc_pending;

static int dlc_pending(const char *content_id) {
    for (int i = 0; i < g_ndlc_pending; i++)
        if (!strcmp(g_dlc_pending[i].content_id, content_id))
            return 1;
    return 0;
}

/* /user/addcont/<TID>/<LABEL>/ac.pkg is there, whole, and is the system's own (not mounted). */
static int dlc_copy_done(const char *content_id, uint64_t size) {
    char tid[10], dir[64], p[96];
    struct stat st;
    snprintf(tid, sizeof(tid), "%.9s", content_id + 7);
    snprintf(dir, sizeof(dir), "/user/addcont/%s", tid);
    if (mounted_from(dir, NULL, 0))
        return 0;
    snprintf(p, sizeof(p), "%s/%s/ac.pkg", dir, content_id + 20);
    return !lstat(p, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == size;
}

static int dlc_ready(const title_info_t *t) {
    char p[PATH_MAX];
    for (int i = 0; i < t->ndlc; i++) {
        const title_dlc_t *d = &t->dlc[i];
        if (d->install && (dlc_failed(d->content_id) || dlc_pending(d->content_id)))
            continue;
        snprintf(p, sizeof(p), HOLD_DIR "/addcont/%s/%s/ac.pkg", d->title_id, d->content_id + 20);
        if (d->install ? !dlc_installed_ids(d->title_id, d->content_id) : !exists(p))
            return 0;
    }
    return 1;
}

/* --- BGFT game task (PS4) --- */

/* The PS4 starts a game only when its background transfer service (BGFT) has the
 * title's game task, /user/bgft/task/<ID>/d0.pdb and d1.pdb, and BGFT reads them at
 * boot: a game added (or whose update changed) plays after the next restart. The
 * ids used here are far above the ones the system gives its own tasks. */
#define BGFT_DIR "/user/bgft/task"
#define BGFT_FIRST 0xf001
#define BGFT_LAST 0xffff

static uint32_t g_fw; /* kernel_get_fw_version(), written in the tasks */

/* Titles whose game task was written in this run. */
static char g_reboot_tids[MAX_TITLES][10];
static int g_nreboot;

static int same_file(const char *path, const void *data, size_t size);

/* When the console booted (0 when unknown). */
/* When the console booted: the clock less the time since boot. (kern.boottime is
 * refused to payloads: it gave 0 and every restart mark was dropped as stale.) It moves
 * by a second or so between calls, and with a clock set over the network. */
static long boot_time(void) {
    struct timespec up;
    if (clock_gettime(CLOCK_MONOTONIC, &up))
        return 0;
    return (long)time(NULL) - (long)up.tv_sec;
}

#define SAME_BOOT_SECONDS 60 /* a console takes longer than this to boot again */

/* STATE_DIR/<TID>.restart holds the boot time of when its task was written: the game
 * needs a restart while the console has not booted since (a PS4Mount loaded again keeps
 * knowing it). */
static int needs_reboot(const char *tid) {
    for (int i = 0; i < g_nreboot; i++)
        if (!strcmp(g_reboot_tids[i], tid))
            return 1;
    char p[64], buf[32] = "";
    snprintf(p, sizeof(p), STATE_DIR "/%s.restart", tid);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    long boot = boot_time(), when = n > 0 ? atol(buf) : -1;
    if (boot && when > 0 && labs(when - boot) <= SAME_BOOT_SECONDS)
        return 1;
    unlink(p); /* booted since: BGFT has read the task */
    return 0;
}

static void mark_reboot(const char *tid) {
    if (!needs_reboot(tid) && g_nreboot < MAX_TITLES)
        snprintf(g_reboot_tids[g_nreboot++], 10, "%s", tid);
    char p[64], buf[32];
    snprintf(p, sizeof(p), STATE_DIR "/%s.restart", tid);
    int n = snprintf(buf, sizeof(buf), "%ld\n", boot_time());
    write_file(p, buf, (size_t)n, 0666);
}

static int contains(const uint8_t *d, size_t n, const char *s) {
    size_t l = strlen(s);
    for (size_t i = 0; i + l <= n; i++)
        if (!memcmp(d + i, s, l))
            return 1;
    return 0;
}

/* The id of the game task of tid written here, 0 when there is none. */
static uint32_t bgft_find(const char *tid) {
    char want[48], p[64];
    snprintf(want, sizeof(want), "/user/app/%s/app.pkg", tid);
    DIR *d = opendir(BGFT_DIR);
    struct dirent *e;
    uint32_t found = 0;
    while (d && !found && (e = readdir(d))) {
        char *end;
        unsigned long id = strtoul(e->d_name, &end, 16);
        if (*end || id < BGFT_FIRST || id > BGFT_LAST)
            continue;
        snprintf(p, sizeof(p), BGFT_DIR "/%s/d0.pdb", e->d_name);
        size_t len;
        uint8_t *data = read_all(p, &len);
        if (data && contains(data, len, want))
            found = (uint32_t)id;
        mem_free(data);
    }
    if (d)
        closedir(d);
    return found;
}

/* 1 when the game task of tid names the pkg at path (as the system writes it): tasks
 * written before /data paths were turned into /user/data ones are written again. */
static int bgft_names(const char *tid, const char *url) {
    uint32_t id = bgft_find(tid);
    char p[64];
    size_t len;
    snprintf(p, sizeof(p), BGFT_DIR "/%08x/d0.pdb", id);
    uint8_t *data = id ? read_all(p, &len) : NULL;
    int ok = data && contains(data, len, url);
    mem_free(data);
    return ok;
}

/* The error BGFT recorded in the game task of tid (record 0x68), 0 when none: a task
 * BGFT gave up on (it refuses the game until a restart with a clean task). */
static uint32_t bgft_task_error(const char *tid) {
    uint32_t id = bgft_find(tid), err = 0;
    char p[64];
    size_t len;
    snprintf(p, sizeof(p), BGFT_DIR "/%08x/d0.pdb", id);
    uint8_t *d = id ? read_all(p, &len) : NULL;
    /* a u32, then records: key, length, length again, data */
    for (size_t o = 4; d && o + 12 <= len;) {
        uint32_t key, l;
        memcpy(&key, d + o, 4);
        memcpy(&l, d + o + 4, 4);
        o += 12;
        if (l > len - o)
            break;
        if (key == 0x68 && l == 4)
            memcpy(&err, d + o, 4);
        o += l;
    }
    mem_free(d);
    return err;
}

/* A free odd id whose install task id (one below) is free too. */
static uint32_t bgft_free_id(void) {
    char p[64];
    /* above every task there, as BGFT gives its own: an id whose task was deleted in this
     * boot is never used again (a game given one could not start: BGFT still had the
     * deleted task's state); the lowest free one only once the ids run out */
    uint32_t top = BGFT_FIRST - 2;
    DIR *d = opendir(BGFT_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char *end;
        unsigned long id = strtoul(e->d_name, &end, 16);
        if (!*end && id >= BGFT_FIRST - 1 && id <= BGFT_LAST && id > top)
            top = (uint32_t)id;
    }
    if (d)
        closedir(d);
    uint32_t next = (top + 2) | 1;
    if (next <= BGFT_LAST)
        return next;
    for (uint32_t id = BGFT_FIRST; id <= BGFT_LAST; id += 2) {
        snprintf(p, sizeof(p), BGFT_DIR "/%08x", id);
        if (exists(p))
            continue;
        snprintf(p, sizeof(p), BGFT_DIR "/%08x", id - 1);
        if (!exists(p))
            return id;
    }
    return 0;
}

static void bgft_remove(const char *tid) {
    uint32_t id = bgft_find(tid);
    if (!id)
        return;
    char p[64];
    snprintf(p, sizeof(p), BGFT_DIR "/%08x", id);
    rm_tree(p);
    printf("[reg] %s: BGFT game task %08x removed\n", tid, id);
}

static int place_bgft(title_src_t *t, const buf_t *json, const patch_ref_t *pr) {
    const char *tid = t->app.title_id;
    uint32_t id = bgft_find(tid);
    int fresh = !id, rc = 0, changed = 0;
    if (fresh && !(id = bgft_free_id())) {
        printf("[reg] %s: no free BGFT task id\n", tid);
        return -1;
    }
    char dir[64], p[96];
    snprintf(dir, sizeof(dir), BGFT_DIR "/%08x", id);
    mkdirs(dir, 0770);
    /* creation time, microseconds since 0001-01-01; an existing task keeps its own */
    uint64_t now = ((uint64_t)time(NULL) + 62135596800ull) * 1000000ull;
    size_t len;
    snprintf(p, sizeof(p), "%s/d0.pdb", dir);
    uint8_t *old = read_all(p, &len);
    if (old && len >= 172 && old[152] == 0x76 && !old[153] && !old[154] && !old[155]) {
        now = 0;
        for (int i = 7; i >= 0; i--)
            now = now << 8 | old[164 + i];
    }
    mem_free(old);
    const char *name = strrchr(t->app_path, '/');
    name = name ? name + 1 : t->app_path;
    char url[PATH_MAX + 8];
    pkg_url(tid, "app", t->app_path, url, sizeof(url));
    for (int d1 = 0; d1 < 2; d1++) {
        bgft_task_t bt = {d1, g_fw, now, id - 1, &t->app, name, url, json, pr};
        buf_t b = {0};
        build_bgft_task(&b, &bt);
        snprintf(p, sizeof(p), "%s/d%d.pdb", dir, d1);
        if (!same_file(p, b.data, b.len)) {
            rc |= write_buf(p, &b);
            changed = 1;
        }
        buf_free(&b);
    }
    if (changed) {
        printf("[reg] %s: BGFT game task %08x %s\n", tid, id, fresh ? "written" : "updated");
        mark_reboot(tid);
    }
    return rc ? -1 : 0;
}

static void unmount_all_layers(const char *target, int force);
static void pump_events(int ms);

/* Installs a small add-on with the system's installer and waits for it (a minute at
 * most): the system copies the pkg into /user/addcont and writes its addcont.db record. */
/* 0 when installed, 1 when the installer would not take it now, 2 when it already would
 * not earlier in this run (see g_dlc_failed: said once). */
static uint64_t free_bytes(const char *dir);

/* 0 installed, 1 refused (tried again at the next load), 2 skipped, 3 started in the background. */
static int dlc_install(dlc_src_t *a) {
    if (dlc_installed(a))
        return 0;
    if (dlc_failed(a->pkg.content_id))
        return 2;
    if (dlc_refused(a)) { /* the same pkg was refused before: not again at every load */
        if (g_ndlc_failed < MAX_DLC_FAILED)
            snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", a->pkg.content_id);
        return 2;
    }
    int big = a->pkg.pkg_size > DLC_INSTALL_MAX;
    if (big && dlc_pending(a->pkg.content_id))
        return 3;
    if (big && (free_bytes("/user") < a->pkg.pkg_size + DLC_SPACE_MARGIN || g_ndlc_pending == MAX_DLC_PENDING)) {
        const sfo_item_t *n = sfo_get(&a->sfo, "TITLE");
        notify(TR("Not enough space on the console to install the add-on %s (%llu MB): free some space and load PS4Mount again",
                  "No hay espacio en la consola para instalar el DLC %s (%llu MB): libera espacio y vuelve a cargar PS4Mount"),
               n && !n->is_int ? n->sval : a->pkg.content_id, (unsigned long long)(a->pkg.pkg_size >> 20));
        if (g_ndlc_failed < MAX_DLC_FAILED)
            snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", a->pkg.content_id);
        return 2;
    }
    char target[64], url[PATH_MAX + 8], msg[PATH_MAX + 64];
    snprintf(target, sizeof(target), "/user/addcont/%s", a->pkg.title_id);
    /* a folder mounted here by an older PS4Mount: the install must reach the real one */
    if (mounted_from(target, NULL, 0))
        unmount_all_layers(target, 1);
    const sfo_item_t *n = sfo_get(&a->sfo, "TITLE");
    int32_t user = 0;
    sceUserServiceGetForegroundUser(&user);
    char kind[24];
    snprintf(kind, sizeof(kind), "ac-%.16s", a->pkg.content_id + 20);
    int task = install_pkg(pkg_url(a->pkg.title_id, kind, a->path, url, sizeof(url)),
                           n && !n->is_int ? n->sval : a->pkg.content_id, user, msg, sizeof(msg));
    printf("[dlc] %s: %s\n", a->pkg.content_id, msg);
    if (big && task >= 0) { /* minutes: followed by dlc_pending_check() */
        dlc_pending_t *p = &g_dlc_pending[g_ndlc_pending++];
        snprintf(p->content_id, sizeof(p->content_id), "%s", a->pkg.content_id);
        snprintf(p->name, sizeof(p->name), "%s", n && !n->is_int ? n->sval : a->pkg.content_id);
        p->size = a->pkg.pkg_size;
        p->since = time(NULL);
        return 3;
    }
    for (int i = 0; task >= 0 && i < 120 && !dlc_installed(a); i++)
        pump_events(500);
    if (!dlc_installed(a)) {
        if (task >= 0)
            printf("[dlc] %s: not installed after a minute\n", a->pkg.content_id);
        if (g_ndlc_failed < MAX_DLC_FAILED)
            snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", a->pkg.content_id);
        dlc_refused_add(a); /* not tried again at every load until its pkg changes */
        return 1;
    }
    printf("[dlc] %s installed\n", a->pkg.content_id);
    return 0;
}

/* Big add-ons that the system has finished installing are said; ones that never finish
 * are given up for this run. */
static void dlc_pending_check(void) {
    for (int i = 0; i < g_ndlc_pending;) {
        dlc_pending_t *p = &g_dlc_pending[i];
        int done = dlc_copy_done(p->content_id, p->size), late = time(NULL) - p->since > DLC_PENDING_SECONDS;
        if (!done && !late) {
            i++;
            continue;
        }
        if (done)
            notify(TR("Add-on installed: %s", "DLC instalado: %s"), p->name);
        else {
            printf("[dlc] %s: not installed after %d s, given up\n", p->content_id, DLC_PENDING_SECONDS);
            if (g_ndlc_failed < MAX_DLC_FAILED)
                snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", p->content_id);
        }
        printf("[dlc] %s %s\n", p->content_id, done ? "installed" : "given up");
        *p = g_dlc_pending[--g_ndlc_pending];
    }
}

/* Installs the small add-ons of a registered game. One the installer refuses does not
 * keep the game out of the library: it is said, and tried again at the next load. */
static void install_title_dlc(title_src_t *t) {
    int left = 0, started = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < t->ndlc; i++) {
        int was = dlc_pending(t->dlc[i].pkg.content_id), rc = t->dlc[i].install ? dlc_install(&t->dlc[i]) : 0;
        left += rc == 1;
        if (rc == 3 && !was)
            started++, bytes += t->dlc[i].pkg.pkg_size;
    }
    if (started)
        notify(TR("%s: installing %d big add-on(s) (%llu.%llu GB) in the background; each one is said when it is ready",
                  "%s: instalando %d DLC grande(s) (%llu,%llu GB) en segundo plano; se avisa cuando cada uno esté listo"),
               t->name, started, (unsigned long long)(bytes * 10 >> 30) / 10, (unsigned long long)(bytes * 10 >> 30) % 10);
    if (left)
        notify(TR("%s: %d add-on(s) could not be installed now; the game is added without them and they are tried again "
                  "the next time PS4Mount loads",
                  "%s: %d DLC no se pudieron instalar ahora; el juego se añade sin ellos y se vuelven a intentar la "
                  "próxima vez que cargues PS4Mount"),
               t->name, left);
}

static int place_files(title_src_t *t) {
    pkg_t *app = &t->app, *pat = t->has_patch ? &t->patch : NULL;
    const char *tid = app->title_id;
    char p[PATH_MAX], q[PATH_MAX], hold[PATH_MAX];
    int rc = 0;

    snprintf(p, sizeof(p), "/user/appmeta/%s", tid);
    mkdirs(p, 0777);
    write_meta(app, p, &rc);
    if (pat)
        write_meta(pat, p, &rc); /* the update's artwork wins, as with a real install */

    snprintf(p, sizeof(p), "/system_data/priv/appmeta/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/param.sfo", p);
    rc |= write_entry(pat ? pat : app, 0x1000, q) != 0;
    snprintf(q, sizeof(q), "%s/nptitle.dat", p);
    rc |= write_entry(app, 0x402, q) < 0;
    snprintf(q, sizeof(q), "%s/npbind.dat", p);
    rc |= write_entry(app, 0x403, q) < 0;

    if (place_license(app))
        return -1;
    for (int i = 0; i < t->ndlc; i++)
        if (!t->dlc[i].install)
            rc |= place_dlc(&t->dlc[i]);
    /* the installer takes an add-on only for a game the system has: one not registered
     * yet gets them once it is (register_finish) */
    if (db_is_registered(tid))
        install_title_dlc(t);

    size_t pl = 0, ppl = 0;
    uint8_t *pg = pkg_entry(app, 0x1001, &pl), *ppg = pat ? pkg_entry(pat, 0x1001, &ppl) : NULL;
    buf_t json = {0}, pbm = {0}, xml = {0}, pjson = {0}, ppbm_patch = {0}, pxml = {0}, ppbm = {0};
    char app_url[PATH_MAX + 8], patch_url[PATH_MAX + 8], app_link[PATH_MAX], patch_link[PATH_MAX];
    build_app_json(&json, app, pkg_url(tid, "app", t->app_path, app_url, sizeof(app_url)));
    pkg_link(tid, "app", t->app_path, app_link, sizeof(app_link));
    /* the PS4 leaves no version and type 0 in app.pbm and patch.pbm (as every install
     * does on 13.52) */
    build_app_pbm(&pbm, app, "", 0);
    patch_ref_t pr = {0}, *prp = NULL;
    if (pat) {
        build_app_json(&pjson, pat, pkg_url(tid, "patch", t->patch_path, patch_url, sizeof(patch_url)));
        pkg_link(tid, "patch", t->patch_path, patch_link, sizeof(patch_link));
        build_app_pbm(&ppbm_patch, pat, "", 0);
        pr = (patch_ref_t){pat, ppg, ppl, t->ver, patch_url, &pjson};
        prp = &pr;
    }
    if (build_app_xml(&xml, pg, pl) || build_pfs_pbm(&ppbm, app, pg, pl, prp) ||
        (pat && build_app_xml(&pxml, ppg, ppl))) {
        printf("[reg] %s: playgo-chunk.dat unusable\n", tid);
        rc = -1;
        goto out;
    }
    rc |= hold_prepare("app", tid, "app.pkg", app_link);
    hold_path(hold, sizeof(hold), "app", tid);
    snprintf(q, sizeof(q), "%s/app.json", hold);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/app.pbm", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.pbm.backup", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.xml", hold);
    rc |= write_buf(q, &xml);
    snprintf(p, sizeof(p), "/system_data/playgo/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/pfs.pbm", p);
    rc |= write_buf(q, &ppbm);
    snprintf(q, sizeof(q), "%s/pfs.pbm.backup", p);
    rc |= write_buf(q, &ppbm);
    rc |= place_bgft(t, &json, prp);
    if (pat) {
        rc |= hold_prepare("patch", tid, "patch.pkg", patch_link);
        hold_path(hold, sizeof(hold), "patch", tid);
        snprintf(q, sizeof(q), "%s/patch.json", hold);
        rc |= write_buf(q, &pjson);
        snprintf(q, sizeof(q), "%s/patch.pbm", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.pbm.backup", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.xml", hold);
        rc |= write_buf(q, &pxml);
    } else {
        hold_path(hold, sizeof(hold), "patch", tid);
        rm_tree(hold);
    }
out:
    mem_free(pg);
    mem_free(ppg);
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&xml);
    buf_free(&pjson);
    buf_free(&ppbm_patch);
    buf_free(&pxml);
    buf_free(&ppbm);
    return rc ? -1 : 0;
}

#define IOV(s) {(void *)(s), strlen(s) + 1}

/* Titles this daemon serves. */
typedef struct {
    int used, has_patch, running, ndlc, ndlc_tids;
    char dlc_tids[MAX_DLC_TIDS][10];
    time_t running_since; /* launch seen in klog; the sandbox mount appears shortly after */
    char tid[10], cid[37], name[128], dir[PATH_MAX];
} managed_t;

static managed_t g_managed[MAX_TITLES];
static int g_manual_scan; /* "Scan for games" in the panel */
static int g_walk_now;    /* a drive came (the share connected): walked now, without saying so */
static int g_deleting; /* a delete screen is open: game folders are unmounted */
static time_t g_deleting_since;
static time_t g_deleting_close_at; /* set when the uninstall finished */

static managed_t *managed_by(const char *tid, const char *cid) {
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && ((tid && !strcmp(g_managed[i].tid, tid)) || (cid && !strcmp(g_managed[i].cid, cid))))
            return &g_managed[i];
    return NULL;
}

/* Top mount on target: 0 none, 1 our folder, -1 something else (like a USB
 * folder mounted by an older PS4Mount). */
static int top_layer(const char *target, const char *dir) {
    struct statfs sf;
    if (statfs(target, &sf) || strcmp(sf.f_mntonname, target))
        return 0;
    return strcmp(sf.f_fstypename, "nullfs") || strcmp(sf.f_mntfromname, dir) ? -1 : 1;
}

static int nullfs_mount(const char *dir, const char *target) {
    struct iovec iov[] = {IOV("fstype"), IOV("nullfs"), IOV("from"), IOV(dir), IOV("fspath"), IOV(target)};
    if (nmount(iov, 6, 0)) {
        printf("[mount] %s -> %s: %s\n", dir, target, strerror(errno));
        return -1;
    }
    return 0;
}



/* A running PS4 title keeps /mnt/sandbox/pfsmnt/<TID>-app0 mounted. Unlike the klog
 * events this survives a restart of the daemon, so it is the reference for "running". */
/* Sandbox mounts of running titles, read from the mount table at most once a second
 * (the table holds over a thousand entries; reading it per title would be wasteful). */
static char (*g_sandboxes)[10];
static int g_nsandboxes;
static time_t g_sandboxes_at;

static void refresh_sandboxes(void) {
    if (g_sandboxes && time(NULL) == g_sandboxes_at)
        return;
    g_sandboxes_at = time(NULL);
    g_nsandboxes = 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return;
    struct statfs *list = mem_alloc(sizeof(*list) * (size_t)(n + 16));
    char (*ids)[10] = realloc(g_sandboxes, sizeof(*ids) * (size_t)(n + 16));
    if (ids)
        g_sandboxes = ids;
    if (!list || !ids) {
        mem_free(list);
        return;
    }
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    static const char prefix[] = "/mnt/sandbox/pfsmnt/";
    for (int i = 0; i < n; i++) {
        const char *on = list[i].f_mntonname;
        if (strlen(on) != sizeof(prefix) - 1 + 9 + 5 || strncmp(on, prefix, sizeof(prefix) - 1) ||
            strcmp(on + sizeof(prefix) - 1 + 9, "-app0"))
            continue;
        snprintf(g_sandboxes[g_nsandboxes++], 10, "%.9s", on + sizeof(prefix) - 1);
    }
    mem_free(list);
}

static int title_running(const char *tid) {
    refresh_sandboxes();
    for (int i = 0; i < g_nsandboxes; i++)
        if (!strcmp(g_sandboxes[i], tid))
            return 1;
    return 0;
}

static void unmount_all_layers(const char *target, int force);

/* Set while /user/app shows only the games being registered (see mask_user_app): the
 * other game folders are hidden then, and must not be mounted or unmounted. */
static int g_masked;

static int masked_path(const char *target) {
    if (!g_masked || strncmp(target, "/user/app/", 10))
        return 0;
    printf("[mount] %s: skipped while /user/app shows only the new games\n", target);
    return 1;
}

static int mount_dir(const char *dir, const char *target) {
    if (masked_path(target))
        return 0;
    /* /user/patch and /user/addcont only exist once the console installed an update or
     * an add-on the normal way: create the parent like the system does (0770) */
    char parent[64];
    snprintf(parent, sizeof(parent), "%s", target);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (mkdir(parent, 0770) == 0)
            printf("[mount] created %s\n", parent);
    }
    mkdir(target, 0777);
    if (dead_layer(target)) {
        printf("[mount] %s: removing a layer of a disconnected drive\n", target);
        unmount_all_layers(target, 1);
    }
    int top = top_layer(target, dir);
    if (top == 1)
        return 0;
    while (top != 0) {
        if (unmount(target, 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            return -1;
        }
        top = top_layer(target, dir);
    }
    return nullfs_mount(dir, target);
}

static void unmount_all_layers(const char *target, int force) {
    if (masked_path(target))
        return;
    for (int i = 0; i < 8 && (mounted_from(target, NULL, 0) || layers_on(target) > 0); i++)
        if (unmount(target, force ? MNT_FORCE : 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            if (!force)
                return;
        }
}

/* Removes every layer of the title (forced: a system uninstall is about to run). */
static void unmount_title(const char *tid, char (*dtids)[10], int ndtids) {
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    unmount_all_layers(target, 1);
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    unmount_all_layers(target, 1);
    for (int i = 0; i < ndtids; i++) {
        snprintf(target, sizeof(target), "/user/addcont/%s", dtids[i]);
        unmount_all_layers(target, 1);
    }
}

/* Add-ons installed the normal way: their folder is never covered by a mount. */
static int addcont_installed(const char *dtid) {
    char target[64];
    snprintf(target, sizeof(target), "/user/addcont/%s", dtid);
    if (mounted_from(target, NULL, 0))
        return 0;
    DIR *d = opendir(target);
    if (!d)
        return 0;
    struct dirent *e;
    int used = 0;
    while (!used && (e = readdir(d)))
        used = e->d_name[0] != '.';
    closedir(d);
    return used;
}

/* /user/app/<TID> always, /user/patch/<TID> only with an update and
 * /user/addcont/<ADD-ON TID> for each add-on title. */
static int mount_paths(const char *tid, int has_patch, const char (*dtids)[10], int ndtids) {
    char target[64], hold[PATH_MAX];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    hold_path(hold, sizeof(hold), "app", tid);
    if (mount_dir(hold, target))
        return -1;
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    hold_path(hold, sizeof(hold), "patch", tid);
    if (has_patch ? mount_dir(hold, target) : (unmount_all_layers(target, 1), 0))
        return -1;
    for (int i = 0; i < ndtids; i++) {
        if (addcont_installed(dtids[i])) {
            printf("[mount] /user/addcont/%s holds add-ons installed the normal way, not mounted\n", dtids[i]);
            continue;
        }
        snprintf(target, sizeof(target), "/user/addcont/%s", dtids[i]);
        hold_path(hold, sizeof(hold), "addcont", dtids[i]);
        if (mount_dir(hold, target))
            return -1;
    }
    return 0;
}

static int mount_title(title_src_t *t) {
    return mount_paths(t->app.title_id, t->has_patch, t->dlc_tids, t->ndlc_tids);
}

static int mount_info(const title_info_t *t) {
    return mount_paths(t->title_id, t->has_patch, t->dlc_tids, t->ndlc_tids);
}

static void manage_fields(const char *tid, const char *cid, const char *name, const char *dir, int has_patch,
                          int ndlc, const char (*dlc_tids)[10], int ndlc_tids) {
    managed_t *m = managed_by(tid, NULL);
    for (int i = 0; !m && i < MAX_TITLES; i++)
        if (!g_managed[i].used)
            m = &g_managed[i];
    if (!m)
        return;
    /* keep the runtime state across rescans */
    managed_t keep = *m;
    memset(m, 0, sizeof(*m));
    m->used = 1;
    if (keep.used) {
        m->running = keep.running;
        m->running_since = keep.running_since;
    }
    m->has_patch = has_patch;
    m->ndlc = ndlc;
    m->ndlc_tids = ndlc_tids;
    memcpy(m->dlc_tids, dlc_tids, sizeof(m->dlc_tids));
    snprintf(m->tid, sizeof(m->tid), "%s", tid);
    snprintf(m->cid, sizeof(m->cid), "%s", cid);
    snprintf(m->name, sizeof(m->name), "%s", name);
    snprintf(m->dir, sizeof(m->dir), "%s", dir);
}

static void manage(const title_src_t *t) {
    manage_fields(t->app.title_id, t->app.content_id, t->name, t->dir, t->has_patch, t->ndlc, t->dlc_tids,
                  t->ndlc_tids);
}

static void manage_info(const title_info_t *t) {
    manage_fields(t->title_id, t->content_id, t->name, t->dir, t->has_patch, t->ndlc, t->dlc_tids, t->ndlc_tids);
}


static int removed(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), STATE_DIR "/%s.removed", tid);
    return exists(p);
}

/* Real installs keep app.pkg on the internal drive; never touch those. */
static int installed_internally(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), "/user/app/%s", tid);
    if (mounted_from(p, NULL, 0))
        return 0;
    snprintf(p, sizeof(p), "/user/app/%s/app.pkg", tid);
    return exists(p);
}

static int marker_matches(const char *tid, const char *expected) {
    char path[64], buf[PATH_MAX + 256] = {0};
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && !strcmp(buf, expected);
}

/* 1 = needs (re)registration, 0 = registered here, -1 = skip. */
/* The title is served from another connected drive: the marker names another
 * folder whose game pkg is still there. The first drive keeps it, so the same game
 * on two drives is not switched (and registered again) on every scan. */
static int served_elsewhere(const title_info_t *t) {
    char path[64], buf[PATH_MAX + 256] = {0}, pkg[PATH_MAX + 16];
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", t->title_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[strcspn(buf, "\n")] = 0;
    if (!strcmp(buf, t->dir))
        return 0;
    size_t l = strlen(buf);
    if (l > 4 && !strcasecmp(buf + l - 4, ".pkg")) /* a pkg used where it is */
        return exists(buf);
    snprintf(pkg, sizeof(pkg), "%s/app.pkg", buf);
    return exists(pkg);
}

static int title_state(const title_info_t *t) {
    const char *tid = t->title_id;
    if (installed_internally(tid) || removed(tid))
        return -1;
    if (served_elsewhere(t)) {
        if (first_time(t->app_path))
            printf("[scan] %s: %s is already served from another drive, this copy is not used\n", t->dir, tid);
        return -1;
    }
    char app_url[PATH_MAX + 8];
    pkg_url(tid, "app", t->app_path, app_url, sizeof(app_url));
    /* folders set up by 0.3.1 and older lack the internal folder: registered again */
    const char *why = !db_is_registered(tid)                ? "not registered"
                      : !bgft_names(tid, app_url)           ? "no game task naming its pkg"
                      : bgft_task_error(tid)                ? "its game task holds an error"
                      : !marker_matches(tid, t->marker)     ? "its pkgs changed"
                      : !hold_ready("app", tid, "app.pkg", t->app_path) ? "game link"
                      : t->has_patch && !hold_ready("patch", tid, "patch.pkg", t->patch_path) ? "update link"
                      : !dlc_ready(t)                        ? "add-ons"
                                                             : NULL;
    if (why)
        printf("[scan] %s: to register (%s)\n", tid, why);
    return why != NULL;
}

static void pump_events(int ms);
static void pump_now(void);

/* The launch check compares /system_data/priv/appmeta/<TID>/param.sfo with the
 * update's param.sfo byte for byte; AppInstallAll writes the game's one there. */
static void enforce_patch_sfo(const char *tid, int has_patch, const char *patch_path, uint32_t sfo_len,
                              const unsigned char *sfo_sha) {
    if (!has_patch)
        return;
    char path[96];
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/param.sfo", tid);
    int fd = open(path, O_RDONLY), same = 0;
    if (fd >= 0) {
        uint8_t cur[8192], sha[32];
        ssize_t n = read(fd, cur, sizeof(cur));
        if (n > 0 && (uint32_t)n == sfo_len && sfo_len <= sizeof(cur)) {
            sha256(cur, (size_t)n, sha);
            same = !memcmp(sha, sfo_sha, sizeof(sha));
        }
        close(fd);
    }
    if (same)
        return;
    /* not the one of the update: its own param.sfo goes in its place */
    pkg_t p;
    if (pkg_open(&p, patch_path))
        return;
    size_t len;
    uint8_t *sfo = pkg_entry(&p, 0x1000, &len);
    if (sfo) {
        printf("[reg] %s: priv param.sfo was not the update's, rewritten\n", tid);
        write_file(path, sfo, len, 0777);
        mem_free(sfo);
    }
    pkg_close(&p);
}

/* Registration method: see fw.h. */
typedef int (*title_dir_fn)(const char *title_id, const char *dir, void *reserved);

static int g_reg_mode = REG_ALL;
static title_dir_fn g_title_dir;
static char g_fw_text[16] = "unknown";

static const char *reg_mode_name(void) {
    return g_reg_mode == REG_ALL ? "AppInstallAll" : g_reg_mode == REG_TITLEDIR ? "AppInstallTitleDir" : "none";
}

static void check_firmware(void) {
    uint32_t fw = g_fw = kernel_get_fw_version();
    unsigned major, minor;
    fw_decode(fw, &major, &minor);
    fw_text(fw, g_fw_text, sizeof(g_fw_text));
    /* looked up at runtime: linking it would stop the payload from loading where it is gone */
    uint32_t h = 0;
    if (!kernel_dynlib_handle(getpid(), "libSceAppInstUtil.sprx", &h))
        g_title_dir = (title_dir_fn)kernel_dynlib_dlsym(getpid(), h, "sceAppInstUtilAppInstallTitleDir");
    g_reg_mode = fw_reg_mode(major, g_title_dir != NULL);
    printf("[main] firmware %s (0x%08x), AppInstallTitleDir %s, registration: %s\n", g_fw_text, fw,
           g_title_dir ? "present" : "missing", reg_mode_name());
}

/* The system registration runs in its own thread: after rest mode InstallAll was seen to
 * block for good, which froze the whole daemon (panel, klog, stop request). A call still
 * running is never doubled; titles keep being checked in the database. */
static volatile int g_install_busy, g_install_rc;
static char g_install_tids[MAX_TITLES][10];
static int g_install_n;

static int register_call(const char *tid) {
    if (g_reg_mode == REG_TITLEDIR) {
        int rc = g_title_dir(tid, "/user/app/", NULL);
        printf("[reg] %s: AppInstallTitleDir=0x%08x\n", tid, rc);
        return rc == (int)0x80990002 ? 0 : rc; /* 0x80990002: already known, restored */
    }
    return sceAppInstUtilAppInstallAll(NULL);
}

static void *install_thread(void *arg) {
    (void)arg;
    int rc = 0;
    if (g_reg_mode == REG_TITLEDIR) {
        for (int i = 0; i < g_install_n; i++) {
            int r = register_call(g_install_tids[i]);
            if (r)
                rc = r;
        }
    } else {
        rc = register_call(NULL);
    }
    g_install_rc = rc;
    g_install_busy = 0;
    return NULL;
}

/* Registers the titles of tids that are not registered yet. */
static int install_all_start(char (*tids)[10], int n) {
    if (g_reg_mode == REG_NONE)
        return -1; /* never where it corrupts the database */
    if (g_install_busy) {
        printf("[reg] registration still running from an earlier call\n");
        return -1;
    }
    g_install_n = 0;
    for (int i = 0; i < n && g_install_n < MAX_TITLES; i++)
        if (!db_is_registered(tids[i]))
            snprintf(g_install_tids[g_install_n++], 10, "%s", tids[i]);
    g_install_busy = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, install_thread, NULL)) {
        install_thread(NULL); /* no thread: call it here */
        return 0;
    }
    pthread_detach(th);
    return 0;
}

/* Registration runs in three steps so a group of games costs the system one
 * AppInstallAll and no write to its databases while it is still busy:
 *   1. register_prepare: files and mounts of each game of the group;
 *   2. one sceAppInstUtilAppInstallAll, then wait until ShellCore has the rows;
 *   3. register_finish: checks the title is registered, writes the marker. */
static int register_prepare(title_src_t *t) {
    if (place_files(t) || mount_title(t))
        return -1;
    manage(t);
    return 0;
}

/* Max mtime of the system databases (and their journals): ShellCore is busy while it changes. */
static time_t db_mtime(void) {
    static const char *files[] = {"/system_data/priv/mms/app.db", "/system_data/priv/mms/app.db-journal",
                                  "/system_data/priv/mms/appinfo.db", "/system_data/priv/mms/appinfo.db-journal",
                                  "/system_data/priv/mms/addcont.db", NULL};
    time_t t = 0;
    struct stat st;
    for (int i = 0; files[i]; i++)
        if (!stat(files[i], &st) && st.st_mtime > t)
            t = st.st_mtime;
    return t;
}

/* Waits until the system databases were not written for quiet seconds (max limit). */
static void wait_db_quiet(int quiet, int limit) {
    time_t start = time(NULL), last = db_mtime(), changed = time(NULL);
    int said = 0;
    while (time(NULL) - changed < quiet && time(NULL) - start < limit) {
        if (!said)
            printf("[reg] the system is writing its databases: waiting for them (up to %d s)\n", limit);
        said = 1;
        pump_events(1000);
        time_t m = db_mtime();
        if (m != last) {
            last = m;
            changed = time(NULL);
        }
    }
}

/* ShellCore registers every new title under /user/app itself; this also refreshes the
 * home screen and fetches the PSN concept. Waits while titles keep arriving. */
/* AppInstallAll walks every folder of /system_ex/app, /system_ex/rnps/apps and /user/app
 * (about 0.7 s per PS4 game, registered or not). While it runs, /user/app shows only the
 * games being added: an empty staging folder is mounted over /user/app and each new
 * game's folder on top of it. The other games stay registered (their rows are not
 * touched) and their own mounts stay in place underneath. */
#define STAGE_DIR HOLD_DIR "/stage"

static int open_games(uint32_t *ids, char (*tids)[10], int max);

static void stage_clear(void) {
    DIR *d = opendir(STAGE_DIR);
    if (!d)
        return;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof(p), STAGE_DIR "/%s", e->d_name);
            rmdir(p);
        }
    closedir(d);
}

static int user_app_masked(void) {
    struct statfs sf;
    return !statfs("/user/app", &sf) && !strcmp(sf.f_mntonname, "/user/app") && !strcmp(sf.f_mntfromname, STAGE_DIR);
}

static void unmask_user_app(char (*tids)[10], int n) {
    char target[64];
    for (int i = 0; i < n; i++) {
        snprintf(target, sizeof(target), "/user/app/%s", tids[i]);
        if (user_app_masked() && mounted_from(target, NULL, 0) && unmount(target, 0) && unmount(target, MNT_FORCE))
            printf("[reg] unmount %s: %s\n", target, strerror(errno));
    }
    /* the system keeps the folder open for a moment after AppInstallAll returned */
    for (int i = 0; i < 12 && user_app_masked(); i++) {
        if (!unmount("/user/app", 0))
            break;
        pump_events(250);
    }
    if (user_app_masked() && unmount("/user/app", MNT_FORCE))
        printf("[reg] unmount /user/app: %s\n", strerror(errno));
    if (user_app_masked())
        printf("[reg] /user/app still shows only the new games\n");
    g_masked = 0;
    stage_clear();
}

/* A daemon stopped while /user/app showed only the new games: show all of them again. */
static void unmask_leftover(void) {
    static char tids[MAX_TITLES][10];
    int n = 0;
    DIR *d = opendir(STAGE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < MAX_TITLES)
        if (strlen(e->d_name) == 9)
            snprintf(tids[n++], 10, "%s", e->d_name);
    if (d)
        closedir(d);
    printf("[reg] /user/app showed only %d game(s) from an earlier run, restoring it\n", n);
    g_masked = 1;
    unmask_user_app(tids, n);
}

/* Returns 1 when /user/app now shows only the games of tids that are not registered. */
static int mask_user_app(char (*tids)[10], int n) {
    uint32_t ids[4];
    char open[4][10];
    if (g_reg_mode != REG_ALL || open_games(ids, open, 4))
        return 0; /* an open game keeps its folder in use: plain AppInstallAll */
    if (user_app_masked())
        unmask_leftover();
    stage_clear();
    mkdirs(STAGE_DIR, 0777);
    char new_tids[MAX_TITLES][10];
    int nn = 0;
    for (int i = 0; i < n && nn < MAX_TITLES; i++)
        if (!db_is_registered(tids[i])) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), STAGE_DIR "/%s", tids[i]);
            if (mkdir(p, 0777) && errno != EEXIST)
                return 0;
            snprintf(new_tids[nn++], 10, "%s", tids[i]);
        }
    if (!nn || nullfs_mount(STAGE_DIR, "/user/app"))
        return 0;
    for (int i = 0; i < nn; i++) {
        char hold[PATH_MAX], target[64];
        hold_path(hold, sizeof(hold), "app", new_tids[i]);
        snprintf(target, sizeof(target), "/user/app/%s", new_tids[i]);
        if (nullfs_mount(hold, target)) {
            unmask_user_app(new_tids, i);
            return 0;
        }
    }
    g_masked = 1;
    printf("[reg] /user/app shows only the %d new game(s) during AppInstallAll\n", nn);
    return 1;
}

static void install_all(char (*tids)[10], int n) {
    int need = 0;
    for (int i = 0; i < n; i++)
        need += !db_is_registered(tids[i]);
    if (!need)
        return;
    int masked = mask_user_app(tids, n);
    if (install_all_start(tids, n) && masked) {
        unmask_user_app(tids, n);
        masked = 0;
    }
    int done = n - need, idle = 0;
    while (done < n && idle < 30) {
        pump_events(1000);
        int now = 0;
        for (int i = 0; i < n; i++)
            now += db_is_registered(tids[i]);
        idle = now > done ? 0 : idle + 1;
        done = now;
        if (masked && !g_install_busy) {
            unmask_user_app(tids, n);
            masked = 0;
        }
    }
    /* the rows can be there before AppInstallAll returns: the other games stay hidden
     * until it does (at most two minutes) */
    for (int i = 0; masked && g_install_busy && i < 120; i++)
        pump_events(1000);
    if (masked)
        unmask_user_app(tids, n);
    printf("[reg] %s=%s0x%08x: %d of %d title(s) registered\n", reg_mode_name(), g_install_busy ? "(still running) " : "",
           g_install_rc, done, n);
}

/* The system databases are only ever written by the system: ShellCore keeps them
 * open with its own cache, and rows written from outside make it flag app.db as
 * corrupted (and rebuild it at the next boot). A title the system did not register
 * is tried again later. */
static int register_finish(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (!db_is_registered(tid)) {
        printf("[reg] %s: not registered by the system\n", tid);
        return -1;
    }
    enforce_patch_sfo(tid, t->has_patch, t->patch_path, t->patch_sfo_len, t->patch_sfo_sha);
    install_title_dlc(t); /* now the system has the game (already installed ones are skipped) */
    char marker[64];
    snprintf(marker, sizeof(marker), STATE_DIR "/%s.src", tid);
    if (write_file(marker, t->marker, strlen(t->marker), 0666))
        return -1;
    manage(t);
    return 0;
}

/* Logs a skipped root file once per daemon run (any number of files). */
static int first_time(const char *path) {
    static char **seen;
    static size_t n, cap;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(seen[i], path))
            return 0;
    if (n == cap) {
        size_t ncap = cap ? cap * 2 : 64;
        char **grown = realloc(seen, ncap * sizeof(*seen));
        if (!grown)
            return 1;
        seen = grown;
        cap = ncap;
    }
    seen[n] = strdup(path);
    if (seen[n])
        n++;
    return 1;
}


/* 1 when sfo lists the add-on prefix (UP0000-CUSA00000_00) as one of its add-on services. */
static int sfo_serves(const sfo_t *sfo, const char *prefix) {
    char key[32];
    for (int i = 1; i <= 7; i++) {
        snprintf(key, sizeof(key), "SERVICE_ID_ADDCONT_ADD_%d", i);
        const sfo_item_t *it = sfo_get(sfo, key);
        if (it && !it->is_int && !strncmp(it->sval, prefix, 19))
            return 1;
    }
    return 0;
}

/* The game on this drive an add-on belongs to: same title ID, or a game (or its
 * update) that names the add-on's content ID prefix in SERVICE_ID_ADDCONT_ADD_n
 * (regional versions share add-ons this way). */
static int find_dlc_game(const char *root, const pkg_t *dlc, char *tid) {
    char lib[PATH_MAX], p[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, dlc->title_id);
    if (exists(p)) {
        snprintf(tid, 10, "%s", dlc->title_id);
        return 0;
    }
    DIR *d = opendir(lib);
    if (!d)
        return -1;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (!is_game_id(e->d_name))
            continue;
        static const char *names[] = {"app.pkg", "patch.pkg"};
        for (int i = 0; i < 2 && !found; i++) {
            pkg_t pkg;
            sfo_t sfo;
            snprintf(p, sizeof(p), "%s/%s/%s", lib, e->d_name, names[i]);
            if (!exists(p) || pkg_open(&pkg, p))
                continue;
            found = !read_sfo(&pkg, &sfo) && sfo_serves(&sfo, dlc->content_id);
            pkg_close(&pkg);
        }
        if (found)
            snprintf(tid, 10, "%s", e->d_name);
    }
    closedir(d);
    return found ? 0 : -1;
}

/* Add-ons left in place during one adoption run, reported in one notification. */
static int g_dlc_orphans, g_dlc_dupes;
static uint32_t g_orphans_sig; /* the add-ons waiting in this scan of a drive */

static uint32_t str_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* Add-ons waiting for their game: checked again only when the games of the drive
 * change (the check opens the pkg of every game). */
typedef struct {
    char *path;
    unsigned long sig;
} waiting_t;

static waiting_t *g_waiting;
static size_t g_nwaiting, g_capwaiting;
static unsigned long g_lib_sig; /* games of the drive being scanned */

/* Signature of the games in <root>/ps4: changes when a game is added or removed. */
static unsigned long lib_signature(const char *root) {
    char lib[PATH_MAX], p[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    if (!d)
        return 0;
    struct dirent *e;
    unsigned long sig = 5381;
    while ((e = readdir(d))) {
        if (!is_game_id(e->d_name))
            continue;
        snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, e->d_name);
        if (!exists(p))
            continue;
        unsigned long h = 0;
        for (const char *c = e->d_name; *c; c++)
            h = h * 33 + (unsigned char)*c;
        sig += h; /* order independent */
    }
    closedir(d);
    return sig;
}

static waiting_t *waiting_find(const char *path) {
    for (size_t i = 0; i < g_nwaiting; i++)
        if (!strcmp(g_waiting[i].path, path))
            return &g_waiting[i];
    return NULL;
}

static void waiting_set(const char *path, unsigned long sig) {
    waiting_t *w = waiting_find(path);
    if (w) {
        w->sig = sig;
        return;
    }
    if (g_nwaiting == g_capwaiting) {
        size_t cap = g_capwaiting ? g_capwaiting * 2 : 64;
        waiting_t *grown = realloc(g_waiting, cap * sizeof(*grown));
        if (!grown)
            return;
        g_waiting = grown;
        g_capwaiting = cap;
    }
    if ((g_waiting[g_nwaiting].path = strdup(path)))
        g_waiting[g_nwaiting++].sig = sig;
}

static void waiting_drop(const char *path) {
    waiting_t *w = waiting_find(path);
    if (!w)
        return;
    free(w->path);
    *w = g_waiting[--g_nwaiting];
}

/* Add-ons of root still waiting for their game. */
static int waiting_count(const char *root) {
    size_t l = strlen(root);
    int n = 0;
    for (size_t i = 0; i < g_nwaiting; i++)
        n += !strncmp(g_waiting[i].path, root, l) && g_waiting[i].path[l] == '/' && exists(g_waiting[i].path);
    return n;
}

/* Path of a file relative to the drive root it is on. */
static const char *drive_rel(const char *root, const char *path) {
    size_t n = strlen(root);
    return !strncmp(path, root, n) && path[n] == '/' ? path + n + 1 : path;
}

/* Records where a pkg moved into <root>/ps4/<TID>/<rel> came from. */
static void record_origin(const char *root, const char *tid, const char *rel, const char *from) {
    char game[PATH_MAX];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    if (origin_add(game, rel, drive_rel(root, from)))
        printf("[scan] %s: could not record where %s came from\n", game, rel);
}

/* Moves an add-on pkg (ac) into <root>/ps4/<GAME>/addcont/<ADD-ON TID>/<LABEL>/ac.pkg. */
static int adopt_dlc(const char *root, const char *name, const char *path, pkg_t *pkg) {
    char game[10], dst[PATH_MAX];
    if (find_dlc_game(root, pkg, game)) {
        if (first_time(path)) {
            /* a game served from another drive is mounted there too: only a real install counts */
            printf("[scan] %s: no game for add-on %s on the drive%s, waiting\n", name, pkg->content_id,
                   installed_internally(pkg->title_id) ? " (that title is installed on the console)" : "");
            g_dlc_orphans++;
        }
        g_orphans_sig += str_hash(path);
        waiting_set(path, g_lib_sig);
        return 0;
    }
    waiting_drop(path);
    snprintf(dst, sizeof(dst), "%s/ps4/%s/addcont/%s/%s", root, game, pkg->title_id, pkg->content_id + 20);
    mkdirs(dst, 0777);
    snprintf(dst, sizeof(dst), "%s/ps4/%s/addcont/%s/%s/ac.pkg", root, game, pkg->title_id, pkg->content_id + 20);
    if (exists(dst)) {
        if (first_time(path)) {
            printf("[scan] %s: %s already has this add-on, the copy was left in place\n", name, game);
            g_dlc_dupes++;
        }
        return 0;
    }
    if (rename(path, dst)) {
        printf("[scan] move %s: %s\n", path, strerror(errno));
        return 0;
    }
    printf("[scan] moved %s -> %s\n", path, dst);
    char rel[64];
    snprintf(rel, sizeof(rel), "addcont/%s/%s/ac.pkg", pkg->title_id, pkg->content_id + 20);
    record_origin(root, game, rel, path);
    return 1;
}

/* One pass over the pkgs in dir: pass 0 moves PS4 game (gd) and update (gp) pkgs into
 * <root>/ps4/<TID>/app.pkg and patch.pkg, pass 1 the add-ons (ac) next to their game. */
static int adopt_dir_pass(const char *root, const char *dir, int pass) {
    int moved = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    char path[PATH_MAX], dst[PATH_MAX];
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".pkg"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat rs;
        if (!stat(path, &rs) && restored_has(RESTORED_LIST, drive_rel(root, path), (uint64_t)rs.st_size)) {
            if (first_time(path))
                printf("[scan] skip %s (moved back from ps4/ on request)\n", path);
            continue;
        }
        waiting_t *w = waiting_find(path);
        if (w && w->sig == g_lib_sig)
            continue; /* waiting add-on, and no new game since the last check */
        pkg_t pkg;
        if (pkg_open(&pkg, path)) {
            if (pass == 0 && first_time(path))
                printf("[scan] skip %s (cannot read the pkg header: not a PS4 pkg, or unreadable)\n", path);
            continue;
        }
        if (!stat(path, &rs) && (uint64_t)rs.st_size < pkg.pkg_size) {
            /* still being copied (an internal folder lets the copy be moved midway) */
            pkg_close(&pkg);
            continue;
        }
        sfo_t sfo;
        const sfo_item_t *cat = NULL;
        if (!read_sfo(&pkg, &sfo))
            cat = sfo_get(&sfo, "CATEGORY");
        const sfo_item_t *sysver = cat ? sfo_get(&sfo, "SYSTEM_VER") : NULL;
        if (pass == 0 && sysver && sysver->is_int && g_fw && !fw_runs(sysver->ival, g_fw) && first_time(path)) {
            /* added anyway: a backport makes it run */
            char need[16], have[16];
            ver_text(sysver->ival, need, sizeof(need));
            ver_text(g_fw, have, sizeof(have));
            notify(TR("%s is built for firmware %s (this console has %s): it only runs with a backport",
                      "%s está hecho para el firmware %s (esta consola tiene %s): solo funciona con un backport"),
                   e->d_name, need, have);
        }
        int is_app = cat && !strcmp(cat->sval, "gd"), is_patch = cat && !strcmp(cat->sval, "gp");
        int is_dlc = cat && !strcmp(cat->sval, "ac");
        if (is_dlc || pass == 1) {
            /* add-ons go in the second pass: their game may be in this folder too */
            if (pass == 1 && is_dlc && !is_game_id(pkg.title_id)) {
                if (first_time(path))
                    printf("[scan] skip %s (add-on %s is not for a PS4 title)\n", path, pkg.content_id);
            } else if (pass == 1 && is_dlc) {
                if (!g_cfg.dlc) {
                    if (first_time(path))
                        printf("[scan] skip %s (add-ons are turned off)\n", path);
                } else if (pkg_fake_key(&pkg))
                    moved += adopt_dlc(root, e->d_name, path, &pkg);
                else if (first_time(path))
                    printf("[scan] skip %s (not a fake add-on pkg)\n", path);
            }
            pkg_close(&pkg);
            continue;
        }
        int ok = (is_app || is_patch) && is_game_id(pkg.title_id) && pkg_fake_key(&pkg) &&
                 !(is_app && homebrew_small(pkg.title_id, pkg.pkg_size));
        char tid[10], ver[16] = "";
        uint8_t digest[32];
        snprintf(tid, sizeof(tid), "%s", pkg.title_id);
        memcpy(digest, pkg.digest, sizeof(digest));
        const sfo_item_t *av = ok ? sfo_get(&sfo, "APP_VER") : NULL;
        if (av && !av->is_int)
            snprintf(ver, sizeof(ver), "%s", av->sval);
        pkg_close(&pkg);
        if (!ok) {
            if (first_time(path))
                printf("[scan] skip %s (not a fake PS4 game or update pkg)\n", path);
            continue;
        }
        if (installed_internally(tid)) {
            if (first_time(path))
                printf("[scan] skip %s (%s is installed internally)\n", path, tid);
            continue;
        }
        snprintf(dst, sizeof(dst), "%s/ps4/%s", root, tid);
        mkdirs(dst, 0777);
        snprintf(dst, sizeof(dst), "%s/ps4/%s/%s", root, tid, is_app ? "app.pkg" : "patch.pkg");
        if (exists(dst)) {
            pkg_t cur;
            int same = !pkg_open(&cur, dst) && !memcmp(cur.digest, digest, sizeof(digest));
            if (cur.fd > 0)
                pkg_close(&cur);
            if (same || is_app) {
                /* duplicates, and a second copy of a game, stay where they are */
                if (first_time(path))
                    notify(same ? TR("%s is already in use for %s; the copy was left where it is",
                                     "%s ya se usa para %s; la copia se deja donde está")
                                : TR("%s: %s already has a game package; the new one was left where it is",
                                     "%s: %s ya tiene un pkg de juego; el nuevo se deja donde está"),
                           e->d_name, tid);
                continue;
            }
            /* a different update: it replaces the current one, which is kept aside */
            char old[PATH_MAX];
            snprintf(old, sizeof(old), "%s/ps4/%s/replaced", root, tid);
            mkdirs(old, 0777);
            snprintf(old, sizeof(old), "%s/ps4/%s/replaced/patch-%ld.pkg", root, tid, (long)time(NULL));
            if (rename(dst, old)) {
                printf("[scan] keep %s aside: %s\n", dst, strerror(errno));
                continue;
            }
            /* the update kept aside keeps its original name */
            char game[PATH_MAX], was[PATH_MAX];
            snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
            if (!origin_find(game, "patch.pkg", was, sizeof(was)))
                origin_add(game, old + strlen(game) + 1, was);
            notify(TR("New update for %s (v%s): the previous one was moved to ps4/%s/replaced",
                      "Nueva actualización de %s (v%s): la anterior se movió a ps4/%s/replaced"),
                   tid, ver, tid);
        }
        if (rename(path, dst)) {
            printf("[scan] move %s: %s\n", path, strerror(errno));
        } else {
            printf("[scan] moved %s -> %s\n", path, dst);
            record_origin(root, tid, is_app ? "app.pkg" : "patch.pkg", path);
            moved++;
        }
    }
    closedir(d);
    return moved;
}

#define MAX_SCAN_DIRS 256

/* Folders of a scan folder that are never looked into: the library itself (ps4, the same
 * folder as the console's PS4 on exFAT), hidden ones and Windows' own. */
static int skip_subdir(const char *name) {
    return name[0] == '.' || !strcasecmp(name, "ps4") || !strcasecmp(name, "$RECYCLE.BIN") ||
           !strcasecmp(name, "System Volume Information");
}

/* Adds the subfolders of dir to dirs (n so far, max MAX_SCAN_DIRS). */
static void add_subdirs(const char *dir, char (*dirs)[PATH_MAX], int *n) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && *n < MAX_SCAN_DIRS) {
        char p[PATH_MAX];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (!skip_subdir(e->d_name) && !lstat(p, &st) && S_ISDIR(st.st_mode))
            snprintf(dirs[(*n)++], PATH_MAX, "%s", p);
    }
    if (d)
        closedir(d);
}

/* The pkgs of a scan folder and of its subfolders two levels down (Game/, Update/ and
 * DLC/ in a folder per game): games and updates of all of them first, so an add-on in
 * a sibling folder finds its game. Subfolders left empty are removed, the scan folder
 * never. */
static int adopt_dir_pkgs(const char *root, const char *dir) {
    char (*dirs)[PATH_MAX] = mem_alloc(sizeof(*dirs) * MAX_SCAN_DIRS);
    if (!dirs)
        return 0;
    int n = 0, moved = 0;
    snprintf(dirs[n++], PATH_MAX, "%s", dir);
    add_subdirs(dir, dirs, &n);
    for (int i = 1, first_level = n; i < first_level; i++)
        add_subdirs(dirs[i], dirs, &n);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1)
            g_lib_sig = lib_signature(root); /* games moved in pass 0 count */
        for (int i = 0; i < n; i++)
            moved += adopt_dir_pass(root, dirs[i], pass);
    }
    for (int i = n - 1; moved && i > 0; i--)
        rmdir(dirs[i]); /* only an empty folder goes */
    mem_free(dirs);
    return moved;
}

typedef struct {
    char tid[10], name[128];
    int state, ndlc, has_patch;
} title_t;

/* A ps4/ folder or a .pkg in the root. Other partitions (like the boot partition
 * Ventoy adds to a drive) are skipped without a notification. */
static void scan_dir(char *out, size_t n, const char *root, int i) {
    if (is_nas(root))
        /* the folder inside the share is already the root of the mount */
        snprintf(out, n, "%s", root);
    else if (is_internal(root))
        /* PS4Mount's own folder: /data/pkg holds the pkgs people install the usual way */
        snprintf(out, n, "%s", i ? "" : INTERNAL_ROOT "/" DEFAULT_SCAN_PATH); /* never /data itself */
    else if (g_cfg.paths[i][0])
        snprintf(out, n, "%s/%s", root, g_cfg.paths[i]);
    else
        snprintf(out, n, "%s", root);
}

static int has_pkg(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        found = n > 4 && !strcasecmp(e->d_name + n - 4, ".pkg");
    }
    closedir(d);
    return found;
}

/* Titles that failed to be added wait before the next try, and the failure is
 * notified once: when the system refuses registrations, every scan would retry. */
#define RETRY_SECONDS 300

typedef struct {
    char tid[10];
    time_t until;
} retry_t;

static retry_t g_retry[MAX_TITLES];

static retry_t *retry_entry(const char *tid, int create) {
    retry_t *free_slot = NULL;
    for (int i = 0; i < MAX_TITLES; i++) {
        if (!strcmp(g_retry[i].tid, tid))
            return &g_retry[i];
        if (!free_slot && !g_retry[i].tid[0])
            free_slot = &g_retry[i];
    }
    if (create && free_slot)
        snprintf(free_slot->tid, sizeof(free_slot->tid), "%s", tid);
    return create ? free_slot : NULL;
}

static int retry_waiting(const char *tid) {
    retry_t *r = retry_entry(tid, 0);
    return r && time(NULL) < r->until;
}

/* Returns 1 the first time a title fails (to notify it), 0 for repeated failures. */
static int retry_failed(const char *tid) {
    retry_t *r = retry_entry(tid, 1);
    if (!r)
        return 1;
    int first = !r->until;
    r->until = time(NULL) + RETRY_SECONDS;
    printf("[reg] %s: next try in %d seconds\n", tid, RETRY_SECONDS);
    return first;
}

static void retry_clear(const char *tid) {
    retry_t *r = retry_entry(tid, 0);
    if (r)
        memset(r, 0, sizeof(*r));
}

static int has_candidates(const char *root) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/ps4", root);
    if (exists(p))
        return 1;
    for (int i = 0; i < g_cfg.npaths; i++) {
        scan_dir(p, sizeof(p), root, i);
        if (has_pkg(p))
            return 1;
    }
    return 0;
}

static int adopt_root_pkgs(const char *root) {
    char dir[PATH_MAX];
    int moved = 0;
    g_dlc_orphans = g_dlc_dupes = 0;
    g_orphans_sig = 0;
    g_lib_sig = lib_signature(root);
    for (int i = 0; i < g_cfg.npaths; i++) {
        scan_dir(dir, sizeof(dir), root, i);
        if (dir[0] && strcmp(dir, root))
            mkdir(dir, 0777); /* there to be seen: where the pkgs go */
        moved += adopt_dir_pkgs(root, dir);
    }
    /* said again only when the waiting add-ons change, not each time the payload starts:
     * add-ons of a game that is never on the drive (themes, for one) would repeat it */
    char seen[64], sig[16];
    snprintf(seen, sizeof(seen), STATE_DIR "/waiting.%08x", str_hash(root));
    snprintf(sig, sizeof(sig), "%08x", g_orphans_sig);
    size_t sl = 0;
    uint8_t *old = read_all(seen, &sl);
    int same = old && sl == strlen(sig) && !memcmp(old, sig, sl);
    mem_free(old);
    if (g_dlc_orphans && !same)
        notify(TR("%s: %d add-on(s) wait for their game, which is not on this drive (see the log)",
                  "%s: %d DLC esperan su juego, que no está en esta unidad (ver el log)"),
               root_label(root),
               waiting_count(root));
    if (g_dlc_orphans && !same)
        write_file(seen, sig, strlen(sig), 0666);
    if (g_dlc_dupes)
        notify(TR("%s: %d add-on copy(ies) were left in place because the game already has them",
                  "%s: %d copia(s) de DLC se dejan donde están porque el juego ya las tiene"),
               root_label(root),
               g_dlc_dupes);
    return moved;
}

/* The title IDs PS4Mount serves from root: its ps4/<TID> folders when pkgs are moved
 * there, or the games the catalog of the drive found anywhere. */
static int titles_on_root(const char *root, char (*tids)[10], int max) {
    int n = 0;
    if (in_place(root)) {
        const catalog_t *c = drive_catalog(root);
        for (int i = 0; c && i < c->ngames && n < max; i++)
            snprintf(tids[n++], 10, "%s", c->games[i].title_id);
        return n;
    }
    char lib[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        if (!is_game_id(e->d_name))
            continue;
        char pkg[PATH_MAX];
        snprintf(pkg, sizeof(pkg), "%s/%s/app.pkg", lib, e->d_name);
        if (exists(pkg))
            snprintf(tids[n++], 10, "%s", e->d_name);
    }
    if (d)
        closedir(d);
    return n;
}

/* The game and update pkg paths of tid on root (empty patch without an update). */
static int title_pkgs(const char *root, const char *tid, char *patch, size_t pn, char *app, size_t an) {
    patch[0] = app[0] = 0;
    if (in_place(root)) {
        const catalog_t *c = drive_catalog(root);
        const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
        if (!g)
            return -1;
        snprintf(app, an, "%s", c->pkgs[g->app].path);
        const cat_pkg_t *p = catalog_patch(g_cat, c, g);
        if (p)
            snprintf(patch, pn, "%s", p->path);
        return 0;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/ps4/%s", root, tid);
    snprintf(app, an, "%s/app.pkg", dir);
    snprintf(patch, pn, "%s/patch.pkg", dir);
    return exists(app) ? 0 : -1;
}

/* Pkgs moved back (RESTORED_LIST) are left out of the catalog of the drive being walked. */
static const char *g_walk_root;
static int g_walk_restored;

static int walk_skip(const char *path, uint64_t size) {
    return g_walk_restored && restored_has(RESTORED_LIST, drive_rel(g_walk_root, path), size);
}

/* The console's own storage holds far more than games: only the two folders PS4Mount puts
 * pkgs in are searched there, so a scan does not walk every save and screenshot. */
static int internal_dir_skip(const char *path, const char *rel) {
    (void)path;
    size_t n = strlen(DEFAULT_SCAN_PATH);
    if (!strncmp(rel, DEFAULT_SCAN_PATH, n) && (!rel[n] || rel[n] == '/'))
        return 0;
    return strncmp(rel, "ps4", 3) || (rel[3] && rel[3] != '/');
}

/* The share is mounted inside a drive the system already trusts, so that drive's walk
 * must not find its games a second time (and the share would be organized by it).
 * The USB drive is exFAT: the name it was made with may not be the one given here. */
static int nas_dir_skip(const char *path) {
    const char *m = nas_mountpoint();
    size_t n = strlen(m);
    return m[0] && !strncasecmp(path, m, n) && (!path[n] || path[n] == '/');
}

static int root_dir_skip(const char *path, const char *rel) {
    if (is_nas(g_walk_root))
        return 0; /* the share is the root itself */
    if (nas_dir_skip(path))
        return 1;
    return is_internal(g_walk_root) && internal_dir_skip(path, rel);
}

/* Brings the catalog of the drive at root up to date: only new or changed pkgs are read,
 * also across restarts (STATE_DIR/catalog-<drive>.txt). 1 when it has games. */
static int catalog_update(catalog_t *cat, const char *root, int fresh) {
    char file[96], id[64];
    snprintf(file, sizeof(file), STATE_DIR "/catalog-%s.txt", root_id(root, id, sizeof(id)));
    int idx = (int)(cat - g_cat);
    if (!g_cat_loaded[idx]) {
        g_cat_loaded[idx] = 1;
        int n = catalog_load(cat, file);
        if (n > 0)
            printf("[scan] %s: %d pkg(s) known from the last run\n", root, n);
    }
    /* A drive with no sign of games (a music stick, the boot partition) is searched all
     * the same, but without saying so: only a ps4 folder or a pkg announces the scan. */
    int announce = fresh && g_manual_scan && (has_candidates(root) || has_pkg(root));
    if (announce)
        notify(TR("Scanning %s for PS4 games...", "Buscando juegos de PS4 en %s..."), root_label(root));
    int before = cat->npkgs;
    g_walk_root = root;
    g_walk_restored = exists(RESTORED_LIST);
    int unread = catalog_walk(cat, root, walk_skip, root_dir_skip, NULL, 0);
    if ((unread || fresh) && cat->npkgs)
        printf("[scan] %s: checking %d pkg(s), %d new or changed\n", root_label(root), cat->npkgs, unread);
    if (unread) {
        time_t t0 = time(NULL);
        int read = catalog_read(cat);
        printf("[scan] %s: %d pkg(s), %d read in %ld s\n", root_label(root), cat->npkgs, read,
               (long)(time(NULL) - t0));
    }
    if ((unread || cat->npkgs != before) && catalog_save(cat, file))
        printf("[scan] %s: could not save %s: %s\n", root, file, strerror(errno));
    for (int i = 0; i < cat->npkgs; i++) {
        const cat_pkg_t *p = &cat->pkgs[i];
        if (p->state == CAT_BAD && first_time(p->path))
            printf("[scan] skip %s (not a fake PS4 game, update or add-on pkg)\n", p->path);
        else if (p->state == CAT_SHORT && first_time(p->path))
            printf("[scan] wait %s: incomplete, %llu of %llu bytes (still copying or cut off)\n", p->path,
                   (unsigned long long)p->size, (unsigned long long)p->full_size);
    }
    catalog_group(cat, NULL);
    if (announce && !cat->ngames) /* say so, as the ps4 folder scan does */
        notify(TR("%s: no PS4 games found", "%s: no se encontraron juegos de PS4"), root_label(root));
    return cat->ngames > 0;
}

/* Add-ons deleted on the console come back with their game: drive reconnected or
 * "Add to library" in the panel. */
static void clear_removed_dlc(void) {
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d)))
        if (strlen(e->d_name) == 36 + 8 && !strcmp(e->d_name + 36, ".removed")) {
            snprintf(p, sizeof(p), STATE_DIR "/%s", e->d_name);
            unlink(p);
        }
    closedir(d);
}

static int readd_queue(const char *tid, const char *why);

static int g_stop; /* the panel asked to stop: the long loops end before the daemon does */

static void scan_root(const char *root, int fresh, int reconnected) {
    const char *label = root_label(root);
    int have = in_place(root) ? catalog_has_games(root) : has_candidates(root);
    if (!have) {
        if (fresh)
            printf("[scan] %s: no ps4 folder or pkg, skipped\n", root);
        return;
    }
    if (fresh && g_manual_scan && !in_place(root))
        notify(TR("Scanning %s for PS4 games...", "Buscando juegos de PS4 en %s..."), label);
    if (reconnected)
        clear_removed_dlc();
    int moved = in_place(root) ? 0 : adopt_root_pkgs(root);
    if (moved && !fresh)
        notify(TR("%s: %d new package(s) found", "%s: %d pkg(s) nuevo(s) encontrado(s)"), label, moved);
    static title_t titles[MAX_TITLES];
    static char tids[MAX_TITLES][10];
    int count = 0, pending = 0;
    int nt = titles_on_root(root, tids, MAX_TITLES);
    for (int k = 0; k < nt && count < MAX_TITLES; k++) {
        if (g_stop)
            break; /* the panel asked to stop */
        if (!(k % 8))
            pump_now(); /* the panel answers while a library is walked */
        const char *tid = tids[k];
        const title_info_t *info = title_info(root, tid);
        if (!info)
            continue;
        if (reconnected) {
            char rm[64];
            snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
            unlink(rm); /* reconnecting the USB drive brings removed games back */
        }
        title_t *t = &titles[count++];
        snprintf(t->tid, sizeof(t->tid), "%s", tid);
        snprintf(t->name, sizeof(t->name), "%s", info->name);
        t->ndlc = info->ndlc;
        t->has_patch = info->has_patch;
        t->state = title_state(info);
        if (t->state == 1 && retry_waiting(t->tid))
            t->state = -1; /* failed recently: tried again later */
        pending += t->state == 1;
    }
    /* the totals for a scan asked in the panel; otherwise only what changed */
    if (fresh && g_manual_scan && !count)
        notify(TR("%s: no PS4 games found", "%s: no se encontraron juegos de PS4"), label);
    else if (fresh && g_manual_scan)
        notify(TR("%s: %d PS4 game(s) found, %d new or updated", "%s: %d juego(s) de PS4 encontrado(s), %d nuevo(s) o actualizado(s)"),
               label, count, pending);
    else if (pending)
        notify(TR("%s: %d new or updated PS4 game(s)", "%s: %d juego(s) de PS4 nuevo(s) o actualizado(s)"), label, pending);
    /* games already in the library: keep them mounted */
    for (int i = 0; i < count; i++) {
        if (g_stop)
            break;
        if (!(i % 8))
            pump_now();
        title_t *t = &titles[i];
        const title_info_t *info;
        if (t->state != 0 || !(info = title_info(root, t->tid)))
            continue;
        enforce_patch_sfo(info->title_id, info->has_patch, info->patch_path, info->sfo_len, info->sfo_sha);
        manage_info(info);
        /* PS4: the system registered another version (the update came later) */
        const char *rv = db_app_ver(t->tid);
        if (rv && *rv && strcmp(rv, info->ver)) {
            char why[64];
            snprintf(why, sizeof(why), TR("registered as %s, the update is %s", "registrado como %s, la actualización es %s"), rv,
                     info->ver);
            if (readd_queue(t->tid, why))
                printf("[reg] %s: %s\n", t->tid, why);
        }
        managed_t *m = managed_by(t->tid, NULL);
        int running = title_running(t->tid);
        if (m && running && !m->running) {
            m->running = 1; /* open before this daemon started (restart or crash) */
        } else if (m && !running && m->running && time(NULL) - m->running_since > 60) {
            m->running = 0; /* closed without a klog end event */
        }
        if (mount_info(info))
            notify(TR("Failed to mount %s (%s)", "No se pudo montar %s (%s)"), info->name, t->tid);
    }
    if (!pending)
        return;

    /* New or changed games, in groups: one AppInstallAll per group and a quiet
     * period between groups, so the system is never flooded with registrations. */
    int size = g_cfg.batch && g_cfg.batch_size > 0 ? g_cfg.batch_size : pending;
    int groups = (pending + size - 1) / size, grouped = groups > 1, done = 0, ok = 0, group = 0;
    int restart = 0; /* added games that wait for a restart (also when an earlier load marked it) */
    if (grouped)
        notify(TR("%s: adding %d PS4 games in %d groups of up to %d", "%s: añadiendo %d juego(s) de PS4 en %d grupo(s) de hasta %d"),
               label, pending, groups, size);
    int *members = malloc(sizeof(int) * (size_t)size);
    char (*ids)[10] = malloc(10 * (size_t)size);
    int *prepared = malloc(sizeof(int) * (size_t)size);
    if (!members || !ids || !prepared) {
        free(members);
        free(ids);
        free(prepared);
        return;
    }
    memset(prepared, 0, sizeof(int) * (size_t)size);
    for (int i = 0; i < count;) {
        if (g_stop)
            break;
        int n = 0;
        for (; i < count && n < size; i++)
            if (titles[i].state == 1)
                members[n++] = i;
        if (!n)
            break;
        if (group++)
            wait_db_quiet(BATCH_QUIET, 120);
        if (grouped) {
            buf_t names = {0};
            for (int k = 0; k < n; k++)
                buf_printf(&names, "%s%s", k ? ", " : "", titles[members[k]].name);
            notify(TR("Group %d/%d, adding %d-%d of %d: %.400s", "Grupo %d/%d, añadiendo %d-%d de %d: %.400s"), group, groups,
                   done + 1, done + n, pending,
                   names.data ? (const char *)names.data : "");
            buf_free(&names);
        }
        int np = 0;
        for (int k = 0; k < n; k++) {
            if (g_stop)
                break;
            if (!(k % 4))
                pump_now(); /* the panel answers while the files of a group are prepared */
            title_t *t = &titles[members[k]];
            title_src_t src;
            prepared[k] = 0;
            if (!grouped) {
                char extra[64] = "";
                if (t->ndlc)
                    snprintf(extra, sizeof(extra), TR(" + %d add-on(s)", " + %d DLC"), t->ndlc);
                notify(TR("Adding %d/%d: %s%s%s (%s)", "Añadiendo %d/%d: %s%s%s (%s)"), done + k + 1, pending, t->name,
                       t->has_patch ? TR(" with update", " con actualización") : "", extra, t->tid);
            }
            if (src_open(&src, root, t->tid) || register_prepare(&src)) {
                if (retry_failed(t->tid))
                    notify(TR("Failed to add %s (%s), it is tried again every few minutes",
                              "No se pudo añadir %s (%s), se vuelve a intentar cada pocos minutos"),
                           t->name, t->tid);
            } else {
                prepared[k] = 1;
                snprintf(ids[np++], 10, "%s", t->tid);
            }
            if (src.app.fd > 0)
                src_close(&src);
        }
        printf("[reg] group %d/%d: %d of %d ready, registering\n", group, groups, np, n);
        install_all(ids, np);
        /* own rows only after ShellCore stopped writing to its databases */
        wait_db_quiet(3, 30);
        for (int k = 0; k < n; k++) {
            title_t *t = &titles[members[k]];
            title_src_t src;
            if (!prepared[k] || src_open(&src, root, t->tid))
                continue;
            if (register_finish(&src)) {
                int first = retry_failed(t->tid);
                if (!first && src.has_patch) {
                    /* twice without the system taking it: the update is what it turns down
                     * (CheckCumulativePatch); left out, it is tried again now */
                    refuse_patch(t->tid, src.patch_path, t->name);
                    retry_clear(t->tid);
                } else if (first) {
                    notify(TR("Failed to add %s (%s), it is tried again every few minutes",
                              "No se pudo añadir %s (%s), se vuelve a intentar cada pocos minutos"),
                           t->name, t->tid);
                }
            } else {
                retry_clear(t->tid);
                ok++;
                int reboot = needs_reboot(t->tid);
                restart += reboot;
                if (!grouped && reboot)
                    notify(TR("%s added. Restart the console once to play it", "%s añadido. Reinicia la consola una vez para jugarlo"),
                           t->name);
                else if (!grouped)
                    notify(TR("%s ready to play", "%s listo para jugar"), t->name);
            }
            src_close(&src);
        }
        done += n;
    }
    free(members);
    free(ids);
    free(prepared);
    if (pending > 1)
        notify(TR("%s: %d of %d game(s) added%s", "%s: %d de %d juego(s) añadido(s)%s"), label, ok, pending,
               restart           ? TR(". Restart the console once to play the new ones",
                                      ". Reinicia la consola una vez para jugar los nuevos")
               : ok == pending ? TR(", ready to play", ", listos para jugar")
                               : "");
}

/* Releases mounts over /user/app/<TID> of older versions, which mounted the drive
 * folder itself, once that folder is gone. The internal folders mounted now stay
 * while the drive is away (rest mode powers drives off): their pkg links simply
 * point to nothing until the drive is back. */
static void release_stale(void) {
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n != 13 || strcmp(e->d_name + 9, ".src"))
            continue;
        char tid[10], target[64], from[PATH_MAX], pkg_path[PATH_MAX + 16];
        snprintf(tid, sizeof(tid), "%.9s", e->d_name);
        snprintf(target, sizeof(target), "/user/app/%s", tid);
        if (!mounted_from(target, from, sizeof(from)) || strncmp(from, "/mnt/", 5))
            continue;
        snprintf(pkg_path, sizeof(pkg_path), "%s/app.pkg", from);
        if (!exists(pkg_path)) {
            printf("[scan] releasing %s (source gone)\n", target);
            managed_t *m = managed_by(tid, NULL);
            if (m) {
                unmount_title(tid, m->dlc_tids, m->ndlc_tids);
                m->used = 0;
            } else {
                unmount_title(tid, NULL, 0);
            }
        }
    }
    closedir(d);
}

/* The share is looked at for new or changed pkgs every nas_rescan minutes (each look is a
 * walk of the whole share over the network); with 0 only when asked ("Scan for games")
 * or when it connects. */
static int nas_due(time_t now, time_t last) {
    return g_cfg.nas_rescan > 0 && now - last >= (time_t)g_cfg.nas_rescan * 60;
}

/* What each game of the drive is, as the last run left it: a reload of the payload does
 * not read the whole library again. */
static int g_titles_loaded[NROOTS];

static void titles_file_name(const char *root, char *out, size_t n) {
    char id[64];
    root_id(root, id, sizeof(id));
    snprintf(out, n, STATE_DIR "/titles-%s.txt", id);
}

static void titles_file_load(int i, const char *root) {
    if (g_titles_loaded[i])
        return;
    g_titles_loaded[i] = 1;
    char p[PATH_MAX];
    titles_file_name(root, p, sizeof(p));
    title_cache_load(p);
}

static void titles_file_save(const char *root) {
    char p[PATH_MAX], key[64];
    titles_file_name(root, p, sizeof(p));
    title_cache_save(p, root_id(root, key, sizeof(key)));
}

static void scan_all(void) {
    static int seen[NROOTS], first = 1;
    static time_t nas_scanned;
    char root[PATH_MAX];
    if (!db_ready()) {
        /* every game would look unregistered and be added again: wait for a good read */
        printf("[scan] the system's app.db could not be read: the scan waits\n");
        return;
    }
    release_stale();
    if (!g_cfg.organize || nas_active()) {
        /* Pkgs are used where they are: bring every catalog up to date first, then group
         * the pkgs of each drive into games (catalog_link). */
        int ready[NROOTS] = {0};
        time_t now = time(NULL);
        pthread_mutex_lock(&g_cat_lock);
        catalog_unlink(g_cat, NROOTS); /* pkgs move when a drive is walked: drop cross refs */
        for (int i = 0; i < NROOTS; i++) {
            root_path(i, root, sizeof(root));
            if (root_present(root)) {
                int due = g_manual_scan || g_walk_now || !g_cat_loaded[i] ||
                          (is_nas(root) ? nas_due(now, g_cat_walked[i]) : now - g_cat_walked[i] >= CATALOG_SECONDS);
                if (due) {
                    ready[i] = catalog_update(&g_cat[i], root, !seen[i] || g_manual_scan || g_walk_now);
                    g_cat_walked[i] = now;
                } else {
                    ready[i] = catalog_has_games(root);
                }
            } else if (g_cat_loaded[i]) {
                catalog_free(&g_cat[i]); /* checked again when the drive is back: it may have changed */
                g_cat_loaded[i] = 0;
                g_cat_walked[i] = 0;
            }
        }
        /* Games are grouped per drive: an update or add-on of another drive is not used,
         * so unplugging one drive never breaks a game of another. */
        catalog_link(g_cat, NROOTS, NULL, 0);
        pthread_mutex_unlock(&g_cat_lock);
        for (int i = 0; i < NROOTS; i++) {
            root_path(i, root, sizeof(root));
            int present = root_present(root);
            /* every pkg of a game is opened in a scan: over the share that is a round trip
             * per pkg, so it is looked at less often */
            int fresh = !seen[i] || g_manual_scan || g_walk_now;
            if (ready[i] && is_nas(root) && !fresh && !nas_due(now, nas_scanned))
                ready[i] = 0;
            else if (ready[i] && is_nas(root))
                nas_scanned = now;
            if (ready[i]) {
                titles_file_load(i, root);
                scan_root(root, fresh, !seen[i] && !first);
                titles_file_save(root);
            }
            seen[i] = present;
        }
    } else {
        /* Back to moving pkgs: the catalogs are not used any more. */
        int any = 0;
        for (int i = 0; i < NROOTS; i++)
            any |= g_cat_loaded[i];
        if (any) {
            pthread_mutex_lock(&g_cat_lock);
            for (int i = 0; i < NROOTS; i++) {
                catalog_free(&g_cat[i]);
                g_cat_loaded[i] = 0;
                g_cat_walked[i] = 0;
            }
            pthread_mutex_unlock(&g_cat_lock);
        }
        for (int i = 0; i < NROOTS; i++) {
            root_path(i, root, sizeof(root));
            int present = root_present(root);
            if (present) {
                titles_file_load(i, root);
                scan_root(root, !seen[i] || g_manual_scan, !seen[i] && !first);
                titles_file_save(root);
            }
            seen[i] = present;
        }
    }
    first = 0;
    g_manual_scan = 0;
    g_walk_now = 0;
}

/* --- klog events --- */

static int klog_connect(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(3232);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a))) {
        printf("[klog] klogsrv\n");
        return s;
    }
    if (s >= 0)
        close(s);
    int fd = open("/dev/klog", O_RDONLY);
    printf("[klog] /dev/klog fd=%d\n", fd);
    return fd;
}

static const char *after(const char *line, const char *tag) {
    const char *p = strstr(line, tag);
    return p ? p + strlen(tag) : NULL;
}

static void end_delete_window(void) {
    g_deleting = 0;
    g_deleting_close_at = 0;
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used)
            mount_paths(g_managed[i].tid, g_managed[i].has_patch, g_managed[i].dlc_tids, g_managed[i].ndlc_tids);
    printf("[delete] window closed, game folders mounted again\n");
}

/* Screens of the two delete flows: the home screen (Options > Delete) and
 * Settings > Storage. From the first of them until focus leaves both flows the game
 * folders are unmounted, so a delete removes /user/app/<TID> cleanly (a mount point
 * cannot be removed). Returns 2 for a screen that opens the window, 1 for a screen
 * that keeps it open (menus and dialogs on the way), 0 otherwise. */
static int delete_scene(const char *name) {
    /* PS5 names, then the PS4's. Options > Delete asks with ConfirmContentDeleteScene and
     * deletes under Parent_ContentDeleteScene. Settings > Storage lists the games in
     * storage_data#application (opened there: its menu, confirmation and delete pages,
     * OptionMenu, Parent_NormalStack and storage_data#application_delete, follow fast) */
    static const char *opens[] = {"AppDeleteConfirmScene", "console storage delete content", "DeleteDialogScreen",
                                  "ConfirmContentDeleteScene", "storage_data#application", NULL};
    static const char *keeps[] = {"AppDeletingScene", "ReactPopupMenu", "Noname", "Render.NPXS40021",
                                  "storage management", "ContentDeleteScene", "OptionMenu :", "Parent_NormalStack",
                                  NULL};
    for (int i = 0; opens[i]; i++)
        if (strstr(name, opens[i]))
            return 2;
    for (int i = 0; keeps[i]; i++)
        if (strstr(name, keeps[i]))
            return 1;
    return 0;
}

static void start_delete_window(void) {
    g_deleting_since = time(NULL);
    g_deleting_close_at = 0;
    if (g_deleting)
        return;
    g_deleting = 1;
    for (int i = 0; i < MAX_TITLES; i++) {
        managed_t *m = &g_managed[i];
        if (!m->used)
            continue;
        if (m->running || title_running(m->tid))
            continue; /* still open: unmounted when it closes (see the End event) */
        unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
    }
    printf("[delete] delete screen open, game folders unmounted\n");
}

/* Copies klog lines about our titles (and crash/mount errors) into the log; every
 * line while STATE_DIR/klog_all exists (for diagnosis: the kernel log has one reader). */
static void trace_klog(const char *line) {
    static time_t checked;
    static int all;
    if (time(NULL) != checked) {
        checked = time(NULL);
        all = exists(STATE_DIR "/klog_all");
    }
    if (all && !strstr(line, "[REGMGR]")) {
        printf("[klog] %.300s\n", line);
        return;
    }
    static const char *keys[] = {"CRASH", "Trophy:Register", "_pfs_mount", "mountpfs", "PrepareProcessLaunch",
                                 "preLaunchCheck", "scePlayGoCore", "process_mount", "process_check", "sqlite",
                                 "SQLITE", "corrupt", "Corrupt", "CORRUPT", "dbr", "DBR", NULL};
    for (int i = 0; keys[i]; i++)
        if (strstr(line, keys[i])) {
            printf("[klog] %.300s\n", line);
            return;
        }
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && strstr(line, g_managed[i].tid) && !strstr(line, "BGLayer") && !strstr(line, "PSM.UI")) {
            printf("[klog] %.300s\n", line);
            return;
        }
}

/* "system file disagree [<TID>]": the system refused to start a game with an update.
 * Removing the game from the library and adding it again made it start; this is done
 * once per title and daemon run, so a game that still fails is not removed again. */
static char g_readd_tid[10], g_readd_why[64];
/* The readd of a game whose update was refused: removing it clears the mark (clean_holds),
 * so it is written again before the game is added back. */
static int g_readd_badpatch;
static char g_readd_patch[PATH_MAX];
static char g_readd_done[MAX_TITLES][10];
static int g_nreadd_done;
static char g_last_launch[10]; /* the PS4's "APP_VER disagree" line names no title */

/* Asks the main loop to remove tid from the library and add it again (once per title
 * and daemon run). Returns 1 when queued. */
static int readd_queue(const char *tid, const char *why) {
    for (int i = 0; i < g_nreadd_done; i++)
        if (!strcmp(g_readd_done[i], tid))
            return 0; /* once per title and daemon run: the scan runs again and again */
    if (g_readd_tid[0])
        return 0;
    snprintf(g_readd_tid, sizeof(g_readd_tid), "%s", tid);
    snprintf(g_readd_why, sizeof(g_readd_why), "%s", why);
    return 1;
}

/* A game added from a drive that is not connected now: 1 with its folder (from its
 * marker), the drive's name and the name the system registered. */
static int offline_game(const char *tid, char *dir, size_t dn, char *drive, size_t vn, char *name, size_t nn) {
    char p[96];
    size_t len = 0;
    snprintf(p, sizeof(p), STATE_DIR "/%s.src", tid);
    uint8_t *m = read_all(p, &len);
    if (!m)
        return 0;
    snprintf(dir, dn, "%.*s", (int)strcspn((char *)m, "\n"), (char *)m);
    mem_free(m);
    /* only drives: /mnt/usb0/ps4/<TID>, or the pkg itself when it is used where it is */
    if (strncmp(dir, "/mnt/", 5) || !db_is_registered(tid))
        return 0;
    size_t l = strlen(dir);
    int file = l > 4 && !strcasecmp(dir + l - 4, ".pkg");
    if (file)
        snprintf(p, sizeof(p), "%s", dir);
    else
        snprintf(p, sizeof(p), "%s/app.pkg", dir);
    if (exists(p))
        return 0;
    snprintf(drive, vn, "%.*s", (int)strcspn(dir + 5, "/"), dir + 5);
    snprintf(name, nn, "%s", tid);
    snprintf(p, sizeof(p), "/system_data/priv/appmeta/%s/param.sfo", tid);
    uint8_t *d = read_all(p, &len);
    sfo_t *sfo = mem_alloc(sizeof(*sfo));
    if (d && sfo && !sfo_parse(d, len, sfo)) {
        const sfo_item_t *t = sfo_get(sfo, "TITLE");
        if (t && !t->is_int && t->sval[0])
            snprintf(name, nn, "%s", t->sval);
    }
    mem_free(sfo);
    mem_free(d);
    return 1;
}

/* The system cannot open the pkg of a game being started: when its drive is away, say
 * so (the system's own error names no reason). */
static void open_failed(const char *tid) {
    char dir[PATH_MAX], drive[32], name[128];
    if (offline_game(tid, dir, sizeof(dir), drive, sizeof(drive), name, sizeof(name)))
        notify(TR("%s is on %s, which is not connected. Connect it and start the game again",
                  "%s está en %s, que no está conectado. Conéctalo y vuelve a abrir el juego"),
               name, drive);
}

static void disagree_seen(const char *tid) {
    managed_t *m = managed_by(tid, NULL);
    if (!m)
        return;
    m->running = 0; /* the launch was refused */
    if (!m->has_patch)
        return;
    /* The system holds one version of the game and the update is not it (it refuses the
     * launch with CheckCumulativePatch): the game is added again without the update. */
    char root[PATH_MAX], patch[PATH_MAX] = "";
    if (!find_root(tid, root, sizeof(root)))
        title_patch_path(root, tid, patch, sizeof(patch));
    if (patch[0])
        refuse_patch(tid, patch, m->name);
    if (readd_queue(tid, TR("the system found another version", "el sistema encontró otra versión"))) {
        g_readd_badpatch = patch[0] != 0;
        snprintf(g_readd_patch, sizeof(g_readd_patch), "%s", patch);
    }
}

/* PS4Mount's own folders of a game that left the library (the pkgs are not in them).
 * The mark of a refused update is kept: it names the pkg, and the pkg did not change (it
 * also has to survive taking the game out and adding it again). */
static void clean_holds(const char *tid, char (*dtids)[10], int ndtids) {
    char hold[PATH_MAX], link[PATH_MAX];
    snprintf(link, sizeof(link), STATE_DIR "/l/%s-app.pkg", tid);
    unlink(link);
    snprintf(link, sizeof(link), STATE_DIR "/l/%s-patch.pkg", tid);
    unlink(link);
    /* one link per add-on, <add-on TID>-ac-<label>.pkg (and -ac.pkg of the 0.7.4 test builds) */
    DIR *ld = ndtids ? opendir(STATE_DIR "/l") : NULL;
    struct dirent *le;
    while (ld && (le = readdir(ld)))
        for (int i = 0; i < ndtids; i++)
            if (!strncmp(le->d_name, dtids[i], 9) && !strncmp(le->d_name + 9, "-ac", 3)) {
                snprintf(link, sizeof(link), STATE_DIR "/l/%s", le->d_name);
                unlink(link);
                break;
            }
    if (ld)
        closedir(ld);
    hold_path(hold, sizeof(hold), "app", tid);
    rm_tree(hold);
    hold_path(hold, sizeof(hold), "patch", tid);
    rm_tree(hold);
    for (int i = 0; i < ndtids; i++) {
        snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s", dtids[i]);
        rm_tree(hold);
    }
}

/* Bytes of the files under path. */
static unsigned long long tree_size(const char *path) {
    struct stat st;
    if (lstat(path, &st))
        return 0;
    if (!S_ISDIR(st.st_mode))
        return (unsigned long long)st.st_size;
    unsigned long long n = 0;
    DIR *d = opendir(path);
    struct dirent *e;
    char p[PATH_MAX];
    while (d && (e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
            n += tree_size(p);
        }
    }
    if (d)
        closedir(d);
    return n;
}

/* Deletes the library folder of tid, <root>/ps4/<tid> (its game, update and add-ons) and
 * nothing else: any other path is refused. The system only reaches the pkgs read-only;
 * deleting them is PS4Mount's own job. 1 deleted, 0 refused. */
static int delete_game_files(const char *dir, const char *tid, const char *name) {
    for (int i = 0; i < NROOTS; i++) {
        char root[PATH_MAX], want[64];
        root_path(i, root, sizeof(root));
        snprintf(want, sizeof(want), "%s/ps4/%s", root, tid);
        if (!is_game_id(tid) || strcmp(dir, want))
            continue;
        unsigned long long n = tree_size(dir);
        rm_tree(dir);
        printf("[delete] %s: %s deleted (%llu bytes)\n", tid, dir, n);
        /* GB of 1024 MB, as the panel and the console's own screens count */
        notify(TR("%s deleted: %.1f GB freed on %s", "%s borrado: %.1f GB liberados en %s"), name,
               (double)n / 1073741824.0, root_label(root));
        return 1;
    }
    printf("[delete] %s: %s is not its library folder, nothing deleted\n", tid, dir);
    return 0;
}

/* The console delete kept the pkgs (they are used where they are): the game must not be
 * added again by the scan that finishes the uninstall. */
static int g_delete_kept;

/* A game of ours deleted on the console's delete screens: deleted for real, as an
 * installed game would be, files included (Remove from library on the panel is the way
 * to take a game off the console and keep its pkgs). In-place pkgs are the user's files:
 * the game leaves the library and they stay. */
static void title_deleted(managed_t *m) {
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", m->tid);
    unlink(path);
    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", m->tid);
    unlink(path);
    clean_holds(m->tid, m->dlc_tids, m->ndlc_tids);
    bgft_remove(m->tid);
    g_delete_kept = !delete_game_files(m->dir, m->tid, m->name);
    if (g_delete_kept) {
        write_file(path, m->tid, strlen(m->tid), 0666); /* not added again until asked */
        printf("[delete] %s: its pkg stays where it is: %s\n", m->tid, m->dir);
    }
    m->used = 0;
}

/* PS4: "GameWillDelete(<TID>)" names the game the system is deleting (only that line: a
 * delete that works writes nothing after it). Inside a delete window it is the user's
 * delete, and the game leaves the library there and then; PS4Mount's own uninstalls
 * (removing on the panel, adding a game again) happen outside those windows. A delete
 * that failed, "AppUnInstallMainExecute ret = [<code>]" (the folder was still mounted:
 * the screens came too fast), is finished by the main loop as a removal on the panel. */
static char g_will_delete[10], g_delete_tid[10];

static void handle_klog_line(const char *line) {
    const char *p;
    char id[40];
    trace_klog(line);
    if ((p = after(line, "OnFocusActiveSceneChanged ["))) {
        /* "[from : type] -> [to : type]": follow the delete screens */
        const char *to = strstr(p, "] -> [");
        if (g_deleting)
            printf("[delete] screen %.200s\n", p);
        if (to) {
            to += 6;
            int kind = delete_scene(to);
            if (kind == 2 || (kind == 1 && g_deleting))
                start_delete_window();
            else if (!kind && g_deleting)
                end_delete_window();
        }
    } else if ((p = after(line, "launchApp("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        snprintf(g_last_launch, sizeof(g_last_launch), "%s", id);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 1;
            m->running_since = time(NULL);
        }
    } else if ((p = after(line, "PrepareProcessLaunchPkg() ret = ")) && strtoul(p, NULL, 16) && g_last_launch[0]) {
        /* the system turned the launch down (80990019: no game task yet): it never ran */
        managed_t *m = managed_by(g_last_launch, NULL);
        if (m)
            m->running = 0;
    } else if ((p = after(line, "notifyAppCrash() titleId={"))) {
        /* a crash, or a game the system killed because it did not suspend in time: it
         * ends without the PlayGo end event */
        snprintf(id, sizeof(id), "%.9s", p);
        printf("[game] %s ended with an application error\n", id);
        managed_t *m = managed_by(id, NULL);
        if (m)
            m->running = 0;
    } else if ((p = after(line, "Suspending the app has been timed out. ("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        printf("[game] %s did not suspend in time: the system closes it (unsaved progress is lost)\n", id);
    } else if ((p = after(line, "sceKernelOpen(/user/app/")) && strstr(p, "/app.pkg) failed")) {
        snprintf(id, sizeof(id), "%.9s", p);
        open_failed(id);
    } else if ((p = after(line, "GameWillDelete("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = g_deleting ? managed_by(id, NULL) : NULL;
        g_will_delete[0] = 0; /* a delete that worked wrote nothing more: forget the last one */
        if (m) {
            printf("[delete] %s deleted on the console\n", id);
            snprintf(g_will_delete, sizeof(g_will_delete), "%s", id);
            title_deleted(m);
        }
    } else if ((p = after(line, "AppUnInstallMainExecute ret = [")) && g_will_delete[0]) {
        unsigned long rc = strtoul(p, NULL, 16);
        printf("[delete] %s: the system's delete returned 0x%08lx\n", g_will_delete, rc);
        if (rc) /* out of the library already: finish the uninstall */
            snprintf(g_delete_tid, sizeof(g_delete_tid), "%s", g_will_delete);
        else
            g_delete_kept = 0; /* the delete finished: a removal on the panel next time */
        g_will_delete[0] = 0;
        if (g_deleting)
            g_deleting_close_at = time(NULL) + 3;
    } else if ((p = after(line, "system file disagree ["))) {
        snprintf(id, sizeof(id), "%.9s", p);
        disagree_seen(id);
    } else if (after(line, "APP_VER disagree [") && g_last_launch[0]) {
        disagree_seen(g_last_launch); /* PS4: registered and update versions differ */
    } else if ((p = after(line, "[PlayGoService][End] end ("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 0;
            if (g_deleting) {
                /* closed by a delete: take the folder away before the uninstall runs */
                unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
                printf("[delete] %s closed, game folder unmounted\n", m->tid);
            }
        }
    } else if ((p = after(line, "[PlayGoCore][Uninstall] end ("))) {
        snprintf(id, sizeof(id), "%.36s", p);
        managed_t *m = managed_by(NULL, id);
        if (m) {
            title_deleted(m);
        } else if (strlen(id) == 36 && g_deleting) {
            /* an add-on of one of our games deleted on a delete screen: keep it out until the
             * drive is reconnected. Outside those screens the uninstall is the system's own:
             * registering a game again uninstalls its add-ons' PlayGo data, and taking that
             * for a delete removed the add-ons of the game every time a game was added. */
            for (int i = 0; i < MAX_TITLES; i++) {
                managed_t *g = &g_managed[i];
                for (int j = 0; g->used && j < g->ndlc_tids; j++) {
                    if (strncmp(id + 7, g->dlc_tids[j], 9))
                        continue;
                    char path[96], hold[PATH_MAX];
                    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", id);
                    write_file(path, id, strlen(id), 0666);
                    snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%.9s/%s", id + 7, id + 20);
                    rm_tree(hold);
                    printf("[delete] add-on %s removed\n", id);
                    i = MAX_TITLES;
                    break;
                }
            }
        }
        /* the delete is done: mount the other games again shortly after */
        if (g_deleting)
            g_deleting_close_at = time(NULL) + 3;
    }
}

/* --- web panel --- */

static time_t g_next_scan;

void app_rescan(void) {
    g_next_scan = 0;
    g_manual_scan = 1;
}

/* The panel's own "scan for games": every pkg is read again, even when its file kept the
 * same size and time (a share that replaces a pkg in place looks unchanged otherwise). */
void app_rescan_hard(void) {
    title_cache_forget();
    /* the updates left out are tried again, too */
    DIR *d = opendir(STATE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 8 && !strcmp(e->d_name + l - 8, ".badpatch")) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), STATE_DIR "/%s", e->d_name);
            unlink(p);
        }
    }
    if (d)
        closedir(d);
    app_rescan();
}
void app_stop(void) { g_stop = 1; }

static void json_str(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_put(b, "\\", 1);
        if ((unsigned char)*s >= 0x20)
            buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

/* Finds the drive tid is served from: the ps4/<TID> folder of older versions, or the
 * game the catalog found where it is. */
static int find_root(const char *tid, char *root, size_t n) {
    for (int i = 0; i < NROOTS; i++) {
        char r[PATH_MAX], p[PATH_MAX];
        root_path(i, r, sizeof(r));
        if (!root_present(r))
            continue;
        if (in_place(r)) {
            const catalog_t *c = drive_catalog(r);
            if (c && catalog_game(c, tid)) {
                snprintf(root, n, "%s", r);
                return 0;
            }
            continue;
        }
        snprintf(p, sizeof(p), "%s/ps4/%s/app.pkg", r, tid);
        if (exists(p)) {
            snprintf(root, n, "%s", r);
            return 0;
        }
    }
    return -1;
}

void app_games_json(buf_t *out) {
    if (g_setup)
        g_setup_seen = time(NULL); /* someone is at the panel: the defaults wait */
    buf_printf(out, "{\"lang\":\"%s\",\"setup\":%s,\"version\":", lang_tag(g_lang), g_setup ? "true" : "false");
    json_str(out, VERSION);
    buf_printf(out, ",\"games\":[");
    int first = 1, nlisted = 0;
    static char listed[MAX_TITLES][10];
    for (int i = 0; i < NROOTS; i++) {
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!root_present(root))
            continue;
        static char tids[MAX_TITLES][10];
        int nt = titles_on_root(root, tids, MAX_TITLES);
        for (int k = 0; k < nt; k++) {
            const char *tid = tids[k];
            const title_info_t *t = title_info(root, tid);
            if (!t)
                continue;
            const char *state, *mount = "";
            char target[64], from[PATH_MAX];
            snprintf(target, sizeof(target), "/user/app/%s", tid);
            int mounted = mounted_from(target, from, sizeof(from));
            if (mounted) {
                struct statfs sf;
                statfs(target, &sf);
                mount = sf.f_flags & MNT_RDONLY ? "ro" : "rw";
            }
            managed_t *m = managed_by(tid, NULL);
            if (installed_internally(tid))
                state = "internal";
            else if (served_elsewhere(t))
                state = "duplicate";
            else if (removed(tid))
                state = "removed";
            else if (m && m->running)
                state = "running";
            else if (mounted && db_is_registered(tid))
                state = needs_reboot(tid) ? "restart" : "ready";
            else
                state = "pending";
            if (nlisted < MAX_TITLES)
                snprintf(listed[nlisted++], 10, "%s", tid);
            buf_printf(out, "%s{\"title_id\":\"%s\",\"name\":", first ? "" : ",", tid);
            json_str(out, t->name);
            buf_printf(out, ",\"version\":");
            json_str(out, t->ver);
            buf_printf(out, ",\"has_patch\":%s,\"size\":%llu,\"state\":\"%s\",\"mount\":\"%s\",\"source\":",
                       t->has_patch ? "true" : "false",
                       (unsigned long long)(t->app_size + (t->has_patch ? t->patch_size : 0)), state, mount);
            json_str(out, t->dir);
            buf_printf(out, ",\"dlc\":%d,\"update_issue\":", t->ndlc);
            json_str(out, t->update_issue);
            /* used where they are: nothing was moved or renamed, so there are no original
             * files to move back (also for games found in the old ps4/<TID> folders) */
            int inpl = !g_cfg.organize || !strncmp(t->dir, NAS_ROOT_PATH "/", sizeof(NAS_ROOT_PATH));
            buf_printf(out, ",\"in_place\":%s", inpl ? "true" : "false");
            char svt[16] = "";
            if (t->system_ver)
                ver_text(t->system_ver, svt, sizeof(svt));
            buf_printf(out, ",\"content_id\":");
            json_str(out, t->content_id);
            buf_printf(out, ",\"game_version\":");
            json_str(out, t->game_ver);
            buf_printf(out, ",\"system_ver\":");
            json_str(out, svt);
            buf_printf(out, ",\"storage\":");
            json_str(out, root_label(root));
            buf_printf(out, "}");
            first = 0;
        }
    }
    /* games of drives that are not connected: still on the home screen, not playable */
    DIR *sd = opendir(STATE_DIR);
    struct dirent *e;
    while (sd && (e = readdir(sd))) {
        size_t n = strlen(e->d_name);
        if (n != 13 || strcmp(e->d_name + 9, ".src") || !game_id_at(e->d_name))
            continue;
        char tid[10], dir[PATH_MAX], drive[32], name[128];
        snprintf(tid, sizeof(tid), "%.9s", e->d_name);
        int seen = 0;
        for (int i = 0; i < nlisted && !seen; i++)
            seen = !strcmp(listed[i], tid);
        if (seen || !offline_game(tid, dir, sizeof(dir), drive, sizeof(drive), name, sizeof(name)))
            continue;
        buf_printf(out, "%s{\"title_id\":\"%s\",\"name\":", first ? "" : ",", tid);
        json_str(out, name);
        buf_printf(out, ",\"version\":\"\",\"has_patch\":false,\"size\":0,\"state\":\"offline\",\"mount\":\"\","
                        "\"source\":");
        json_str(out, dir);
        buf_printf(out, ",\"dlc\":0,\"update_issue\":\"\",\"in_place\":false,\"content_id\":\"\",\"game_version\":\"\","
                        "\"system_ver\":\"\",\"storage\":");
        json_str(out, drive);
        buf_printf(out, "}");
        first = 0;
    }
    if (sd)
        closedir(sd);
    buf_printf(out, "]}");
}

static int app_art(const char *tid, const char *name, uint32_t id, buf_t *out);

int app_icon(const char *tid, buf_t *out) { return app_art(tid, "icon0.png", 0x1200, out); }

int app_pic(const char *tid, buf_t *out) { return app_art(tid, "pic0.png", 0x1220, out); }

/* Artwork of a game: the copy written into /user/appmeta when it was added, or the entry
 * id of its pkgs (the update's first: its artwork wins, as with a real install). */
static int app_art(const char *tid, const char *name, uint32_t id, buf_t *out) {
    char p[PATH_MAX], root[PATH_MAX];
    snprintf(p, sizeof(p), "/user/appmeta/%s/%s", tid, name);
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        char tmp[65536];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof(tmp))) > 0)
            buf_put(out, tmp, (size_t)n);
        close(fd);
        return out->len ? 0 : -1;
    }
    if (find_root(tid, root, sizeof(root)))
        return -1;
    char patch[PATH_MAX], app[PATH_MAX];
    if (title_pkgs(root, tid, patch, sizeof(patch), app, sizeof(app)))
        return -1;
    const char *files[] = {patch, app}; /* the update's artwork wins, as with a real install */
    for (int i = 0; i < 2; i++) {
        if (!files[i][0])
            continue;
        pkg_t *pkg = mem_alloc(sizeof(*pkg));
        if (!pkg || pkg_open(pkg, files[i])) {
            mem_free(pkg);
            continue;
        }
        size_t len;
        uint8_t *png = pkg_entry(pkg, id, &len);
        pkg_close(pkg);
        mem_free(pkg);
        if (png) {
            buf_put(out, png, len);
            mem_free(png);
            return 0;
        }
    }
    return -1;
}

int app_add(const char *tid, char *msg, size_t cap) {
    char rm[64], root[PATH_MAX];
    if (find_root(tid, root, sizeof(root))) {
        snprintf(msg, cap, TR("%s is not on any connected drive", "%s no está en ninguna unidad conectada"), tid);
        return -1;
    }
    snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
    unlink(rm);
    clear_removed_dlc();
    g_next_scan = 0;
    snprintf(msg, cap, TR("Adding %s to the library", "Añadiendo %s a la biblioteca"), tid);
    return 0;
}

/* sceAppInstUtilAppUnInstall in its own thread, waited for at most UNINSTALL_WAIT seconds:
 * a call that never returns (as InstallAll did after rest mode) must not freeze PS4Mount.
 * The wait does not serve the panel (this also runs from a panel request). */
#define UNINSTALL_WAIT 60

static volatile int g_uninstall_busy, g_uninstall_rc;
static char g_uninstall_tid[10];

static void *uninstall_thread(void *arg) {
    (void)arg;
    g_uninstall_rc = sceAppInstUtilAppUnInstall(g_uninstall_tid);
    g_uninstall_busy = 0;
    return NULL;
}

/* 0 or the system error; -2 while an earlier call is still running or this one timed out. */
static int uninstall_title(const char *tid) {
    if (g_uninstall_busy) {
        printf("[remove] %s: an earlier uninstall (%s) is still running\n", tid, g_uninstall_tid);
        return -2;
    }
    snprintf(g_uninstall_tid, sizeof(g_uninstall_tid), "%s", tid);
    g_uninstall_busy = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, uninstall_thread, NULL)) {
        uninstall_thread(NULL);
        return g_uninstall_rc;
    }
    pthread_detach(th);
    for (int i = 0; i < UNINSTALL_WAIT * 10 && g_uninstall_busy; i++)
        usleep(100000);
    if (g_uninstall_busy) {
        printf("[remove] %s: uninstall did not return within %d s\n", tid, UNINSTALL_WAIT);
        return -2;
    }
    return g_uninstall_rc;
}

int app_remove(const char *tid, char *msg, size_t cap) {
    managed_t *m = managed_by(tid, NULL);
    if ((m && m->running) || title_running(tid)) {
        printf("[web] remove %s refused: game running\n", tid);
        snprintf(msg, cap, "%s", TR("Close the game before removing it", "Cierra el juego antes de quitarlo"));
        return -1;
    }
    if (m)
        unmount_title(tid, m->dlc_tids, m->ndlc_tids);
    else
        unmount_title(tid, NULL, 0);
    char a[64], b[64];
    snprintf(a, sizeof(a), "/user/app/%s", tid);
    snprintf(b, sizeof(b), "/user/patch/%s", tid);
    if (mounted_from(a, NULL, 0) || mounted_from(b, NULL, 0)) {
        /* never let the system uninstall run while the USB folder is reachable */
        snprintf(msg, cap, TR("Could not unmount %s; nothing was removed", "No se pudo desmontar %s; no se quitó nada"), tid);
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", tid);
    write_file(path, tid, strlen(tid), 0666);
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    unlink(path);
    int rc = db_is_registered(tid) ? uninstall_title(tid) : 0;
    if (!rc) {
        clean_holds(tid, m ? m->dlc_tids : NULL, m ? m->ndlc_tids : 0);
        /* the task goes with it: BGFT keeps the state of an id for the whole boot, and a
         * game added again under the same one could not start */
        bgft_remove(tid);
    }
    if (m)
        m->used = 0;
    printf("[web] remove %s: uninstall=0x%08x\n", tid, rc);
    if (rc == -2)
        snprintf(msg, cap,
                 TR("The system did not finish removing %s yet; try again in a moment",
                    "El sistema aún no terminó de quitar %s; inténtalo de nuevo en un momento"),
                 tid);
    else
        snprintf(msg, cap,
                 rc ? TR("Failed to remove %s (0x%08x)", "No se pudo quitar %s (0x%08x)")
                    : TR("%s removed from the library. Its pkgs stay where they are.",
                         "%s quitado de la biblioteca. Sus pkgs se quedan donde están."),
                 tid, rc);
    return rc ? -1 : 0;
}

/* Removes a game refused with "system file disagree" from the library and adds it again. */
static void readd_title(const char *tid) {
    char msg[256], name[128];
    managed_t *m = managed_by(tid, NULL);
    snprintf(name, sizeof(name), "%s", m ? m->name : tid);
    if (g_nreadd_done < MAX_TITLES)
        snprintf(g_readd_done[g_nreadd_done++], 10, "%s", tid);
    pump_events(5000); /* let the system finish the refused launch */
    notify(TR("%s (%s): adding it to the library again", "%s (%s): volviendo a añadirlo a la biblioteca"), name,
           g_readd_why);
    int rc = app_remove(tid, msg, sizeof(msg));
    printf("[reg] %s: removed to add it again: %s\n", tid, msg);
    if (g_readd_badpatch) {
        /* the removal cleared it: the game comes back without the update */
        g_readd_badpatch = 0;
        write_badpatch(tid, g_readd_patch);
        g_readd_patch[0] = 0;
    }
    if (!rc)
        wait_db_quiet(3, 30);
    app_add(tid, msg, sizeof(msg));
}

/* --- moving pkgs back where they came from --- */

static void clean_name(char *s) {
    for (; *s; s++)
        if (strchr("\\:*?\"<>|", *s) || (unsigned char)*s < 0x20)
            *s = '_';
}

/* Moves <game_dir>/<rel> to its recorded original path (or fallback, relative to the
 * drive root, for pkgs moved before origins were recorded) and adds it to the list of
 * restored pkgs when remember is set (moved back on request: the scan leaves them alone).
 * 1 moved, 0 nothing there, -1 failed. */
static int restore_pkg(const char *root, const char *game_dir, const char *rel, const char *fallback, int remember) {
    char src[PATH_MAX], orig[PATH_MAX], dst[PATH_MAX], dir[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s", game_dir, rel);
    struct stat st;
    if (stat(src, &st))
        return 0;
    if (origin_find(game_dir, rel, orig, sizeof(orig))) {
        snprintf(orig, sizeof(orig), "%s", fallback);
        clean_name(orig);
    }
    snprintf(dir, sizeof(dir), "%s/%s", root, orig);
    char *slash = strrchr(dir, '/');
    if (slash && slash > dir + strlen(root))
        *slash = 0, mkdirs(dir, 0777); /* the original folder may be gone */
    if (origin_free_path(root, orig, dst, sizeof(dst)) || rename(src, dst)) {
        printf("[restore] %s: %s\n", src, strerror(errno));
        return -1;
    }
    if (remember)
        restored_add(RESTORED_LIST, drive_rel(root, dst), (uint64_t)st.st_size);
    printf("[restore] %s -> %s\n", src, dst);
    return 1;
}

static void unlink_in(const char *dir, const char *const *names) {
    char p[PATH_MAX];
    for (; *names; names++) {
        snprintf(p, sizeof(p), "%s/%s", dir, *names);
        unlink(p);
    }
}

/* Moves every pkg of one game folder back and removes what PS4Mount created for it.
 * remember: they were moved back on request, so the scan leaves them alone; clear for
 * the move back that organize off does on its own, whose pkgs are used where they are.
 * Folders are only removed when empty, so nothing else is ever deleted. */
static void restore_files(const char *root, const char *tid, const char *name, int *moved, int *failed, int remember) {
    static const char *const generated[] = {"app.json", "app.pbm", "app.pbm.backup", "app.xml", "patch.json",
                                            "patch.pbm", "patch.pbm.backup", "patch.xml", NULL};
    static const char *const ac_generated[] = {"ac.json", "ac.pbm", "ac.pbm.backup", NULL};
    char game[PATH_MAX], rel[PATH_MAX], fb[PATH_MAX], sub[PATH_MAX];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    int rc;
#define RESTORE(r, f)                                                                                                  \
    do {                                                                                                               \
        rc = restore_pkg(root, game, r, f, remember);                                                                  \
        *moved += rc > 0;                                                                                              \
        *failed += rc < 0;                                                                                             \
    } while (0)
    snprintf(fb, sizeof(fb), "%s - %s - game.pkg", tid, name);
    RESTORE("app.pkg", fb);
    snprintf(fb, sizeof(fb), "%s - %s - update.pkg", tid, name);
    RESTORE("patch.pkg", fb);
    snprintf(sub, sizeof(sub), "%s/replaced", game);
    DIR *d = opendir(sub);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(rel, sizeof(rel), "replaced/%s", e->d_name);
        snprintf(fb, sizeof(fb), "%s - %s - older update %s", tid, name, e->d_name);
        RESTORE(rel, fb);
    }
    if (d)
        closedir(d);
    rmdir(sub);
    snprintf(sub, sizeof(sub), "%s/addcont", game);
    d = opendir(sub);
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char tdir[PATH_MAX], hold[PATH_MAX];
        snprintf(tdir, sizeof(tdir), "%s/%s", sub, e->d_name);
        DIR *l = opendir(tdir);
        struct dirent *f;
        while (l && (f = readdir(l))) {
            if (f->d_name[0] == '.')
                continue;
            char ldir[PATH_MAX + 64];
            snprintf(ldir, sizeof(ldir), "%s/%s", tdir, f->d_name);
            snprintf(rel, sizeof(rel), "addcont/%s/%s/ac.pkg", e->d_name, f->d_name);
            snprintf(fb, sizeof(fb), "%s - %s - %s.pkg", e->d_name, name, f->d_name);
            RESTORE(rel, fb);
            unlink_in(ldir, ac_generated);
            rmdir(ldir);
            snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s/%s", e->d_name, f->d_name);
            rm_tree(hold);
        }
        if (l)
            closedir(l);
        rmdir(tdir);
        snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s", e->d_name);
        rmdir(hold); /* kept while other games still use add-ons of this title */
    }
    if (d)
        closedir(d);
#undef RESTORE
    rmdir(sub);
    unlink_in(game, generated); /* written next to the pkgs by versions before 0.3.2 */
    char marker[64];
    snprintf(marker, sizeof(marker), "%s/" ORIGIN_FILE, game);
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/app.pkg", game);
    if (!exists(probe)) /* keep the record while a pkg could not be moved */
        unlink(marker);
    rmdir(game);
    snprintf(probe, sizeof(probe), HOLD_DIR "/app/%s", tid);
    rm_tree(probe);
    snprintf(probe, sizeof(probe), HOLD_DIR "/patch/%s", tid);
    rm_tree(probe);
}

/* Add-on title IDs of a game folder (its /user/addcont mounts). */
static int folder_dlc_tids(const char *game, char (*tids)[10], int max) {
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/addcont", game);
    DIR *d = opendir(sub);
    int n = 0;
    struct dirent *e;
    while (d && n < max && (e = readdir(d)))
        if (is_game_id(e->d_name))
            snprintf(tids[n++], 10, "%s", e->d_name);
    if (d)
        closedir(d);
    return n;
}

/* Takes one game out of the library (through the system) and unmounts it. */
static int restore_unregister(const char *root, const char *tid) {
    char game[PATH_MAX], dtids[MAX_DLC_TIDS][10];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    int nd = folder_dlc_tids(game, dtids, MAX_DLC_TIDS);
    unmount_title(tid, dtids, nd);
    char msg[256];
    if (!installed_internally(tid) && app_remove(tid, msg, sizeof(msg))) {
        printf("[restore] %s kept: %s\n", tid, msg);
        return -1;
    }
    return 0;
}

static void restore_clear_state(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), STATE_DIR "/%s.removed", tid);
    unlink(p);
    snprintf(p, sizeof(p), STATE_DIR "/%s.src", tid);
    unlink(p);
    retry_clear(tid);
    bgft_remove(tid);
}

/* Requests from the panel, run by the main loop: some titles, or all of them. */
static char g_restore_tids[MAX_TITLES][10];
static int g_restore_n, g_restore_all;

/* organize off: the games of each drive PS4Mount had moved into ps4/ are sent back to
 * where they were once (and then used there). */
static int g_migrated[NROOTS];

/* --- closing the open game --- */

typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[10];
    uint32_t app_type;
    char unknown2[0x36];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);
int sceSystemServiceKillApp(uint32_t app_id, int how, int reason, int core_dump);

/* The panel app (panel-app/): a home screen tile that shows the panel. It is no game. */
#define PANEL_APP_TID "PSMT00001"

/* Open apps: processes named eboot.bin (PS4 and PS5 games alike), the games or only the
 * panel app. Fills up to max app ids and title ids, returns how many are open. */
static int open_apps(uint32_t *ids, char (*tids)[10], int max, int panel) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}, n = 0;
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) || !len)
        return 0;
    len += len / 4; /* processes started meanwhile */
    uint8_t *buf = mem_alloc(len); /* ~100 KB: off the libc heap */
    if (!buf || sysctl(mib, 4, buf, &len, NULL, 0)) {
        mem_free(buf);
        return 0;
    }
    for (size_t off = 0; off + sizeof(struct kinfo_proc) <= len;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)(buf + off);
        if (ki->ki_structsize < (int)sizeof(*ki) || (size_t)ki->ki_structsize > len - off)
            break;
        app_info_t info;
        memset(&info, 0, sizeof(info));
        if (!strcmp(ki->ki_comm, "eboot.bin") && !sceKernelGetAppInfo(ki->ki_pid, &info) && info.app_id &&
            (strncmp(info.title_id, PANEL_APP_TID, 9) == 0) == panel) {
            if (n < max) {
                ids[n] = info.app_id;
                snprintf(tids[n], 10, "%.9s", info.title_id);
            }
            n++;
        }
        off += (size_t)ki->ki_structsize;
    }
    mem_free(buf);
    return n;
}

static int open_games(uint32_t *ids, char (*tids)[10], int max) {
    return open_apps(ids, tids, max, 0);
}

/* Back on the panel's first page inside the panel app: the app cannot close itself while
 * it shows the page. */
void app_panel_close(void) {
    uint32_t ids[2];
    char tids[2][10];
    int n = open_apps(ids, tids, 2, 1);
    for (int i = 0; i < n && i < 2; i++)
        printf("[main] panel app closed: 0x%x\n", (unsigned)sceSystemServiceKillApp(ids[i], -1, 0, 0));
}

/* Every pkg operation that takes games out of the library closes the open game first,
 * whichever it is: a game left open keeps reading pkgs that are being moved. 0 once no
 * game is open; -1 when one did not close within 30 seconds. */
static int close_open_games(void) {
    uint32_t ids[8];
    char tids[8][10];
    int n = open_games(ids, tids, 8);
    if (!n)
        return 0;
    for (int i = 0; i < n && i < 8; i++) {
        managed_t *m = managed_by(tids[i], NULL);
        notify(TR("Closing %s to restore its original files", "Cerrando %s para restaurar sus archivos originales"), m ? m->name : tids[i]);
        int rc = sceSystemServiceKillApp(ids[i], -1, 0, 0);
        printf("[restore] close %s (app 0x%08x): 0x%08x\n", tids[i], ids[i], rc);
    }
    char closing[8][10];
    int nc = n < 8 ? n : 8;
    memcpy(closing, tids, sizeof(closing[0]) * (size_t)nc);
    for (int s = 0; s < 30; s++) {
        pump_events(1000);
        int mounted = 0;
        for (int i = 0; i < nc; i++)
            mounted |= title_running(closing[i]);
        if (!open_games(ids, tids, 8) && !mounted) {
            pump_events(2000); /* let the system finish unmounting the game */
            for (int i = 0; i < MAX_TITLES; i++)
                g_managed[i].running = 0;
            return 0;
        }
    }
    return -1;
}

/* Folders of games on every connected drive (or only tid), with their name. */
typedef struct {
    char root[PATH_MAX], tid[10], name[128];
} restore_item_t;

/* only: n title ids to keep, or NULL for every game. */
static int restore_collect(char (*only)[10], int nonly, restore_item_t *items, int max) {
    int n = 0;
    for (int i = 0; i < NROOTS && n < max; i++) {
        char root[PATH_MAX], lib[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!root_present(root))
            continue;
        snprintf(lib, sizeof(lib), "%s/ps4", root);
        DIR *d = opendir(lib);
        struct dirent *e;
        while (d && n < max && (e = readdir(d))) {
            if (!is_game_id(e->d_name))
                continue;
            int wanted = !only;
            for (int k = 0; k < nonly && !wanted; k++)
                wanted = !strcmp(only[k], e->d_name);
            if (!wanted)
                continue;
            restore_item_t *it = &items[n++];
            snprintf(it->root, sizeof(it->root), "%s", root);
            snprintf(it->tid, sizeof(it->tid), "%s", e->d_name);
            snprintf(it->name, sizeof(it->name), "%s", e->d_name);
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, e->d_name);
            pkg_t pkg;
            sfo_t sfo;
            if (exists(p) && !pkg_open(&pkg, p)) {
                const sfo_item_t *t = read_sfo(&pkg, &sfo) ? NULL : sfo_get(&sfo, "TITLE");
                if (t) {
                    snprintf(it->name, sizeof(it->name), "%s", t->sval);
                    clean_name(it->name);
                }
                pkg_close(&pkg);
            }
        }
        if (d)
            closedir(d);
    }
    return n;
}

/* Moves the pkgs of the requested games (only, or all when NULL) back, in groups of the
 * restore setting: the games of a group leave the library one at a time, their pkgs are
 * moved, and the next group waits until the system databases are idle. remember: see
 * restore_files (0 for the move back organize off does by itself). */
static void run_restore(char (*only)[10], int nonly, int remember) {
    static restore_item_t items[MAX_TITLES];
    int n = restore_collect(only, nonly, items, MAX_TITLES), moved = 0, failed = 0, done = 0, kept = 0;
    if (!n) {
        notify("%s", TR("No PS4Mount games to restore", "No hay juegos de PS4Mount que restaurar"));
        return;
    }
    if (close_open_games()) {
        notify("%s", TR("The open game did not close; no files were restored", "El juego abierto no se cerró; no se restauró ningún archivo"));
        return;
    }
    int size = g_cfg.restore_batch;
    if (n == 1)
        notify(TR("Restoring the original files of %s...", "Restaurando los archivos originales de %s..."), items[0].name);
    else
        notify(TR("Restoring the original files of %d game(s), %d at a time...",
                  "Restaurando los archivos originales de %d juego(s), en grupos de %d..."),
               n, size);
    for (int i = 0; i < n; i += size) {
        if (i)
            wait_db_quiet(BATCH_QUIET, 120);
        int end = i + size < n ? i + size : n;
        for (int k = i; k < end; k++) {
            if (restore_unregister(items[k].root, items[k].tid)) {
                kept++;
                continue;
            }
            /* one removal at a time: the next waits until the system databases are idle */
            wait_db_quiet(3, 30);
            restore_files(items[k].root, items[k].tid, items[k].name, &moved, &failed, remember);
            done++;
        }
    }
    /* the uninstall events arrive late and mark the games as removed: clear that, so a
     * game added again later is not taken for one removed on the console */
    pump_events(3000);
    for (int i = 0; i < n; i++)
        restore_clear_state(items[i].tid);
    clear_removed_dlc();
    g_next_scan = 0;
    if (n == 1 && done)
        notify(TR("%s: %d pkg file(s) restored%s", "%s: %d pkg(s) restaurado(s)%s"), items[0].name, moved,
               failed ? TR(", some could not be moved (see the log)", ", algunos no se pudieron mover (ver el log)") : "");
    else if (n > 1)
        notify(TR("%d pkg file(s) of %d game(s) restored%s%s", "%d pkg(s) de %d juego(s) restaurado(s)%s%s"), moved, done,
               failed ? TR(", some could not be moved (see the log)", ", algunos no se pudieron mover (ver el log)") : "",
               kept ? TR(", some games could not leave the library (see the log)",
                         ", algunos juegos no pudieron salir de la biblioteca (ver el log)")
                    : "");
}

/* Games on root that PS4Mount moved into ps4/<TITLE_ID>/: their app.pkg has a recorded
 * origin, so they go back to where they were and are used there. Games removed on the
 * console and titles installed internally are left alone. */
static int organized_on_root(const char *root, char (*tids)[10], int max) {
    char lib[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (!is_game_id(e->d_name) || removed(e->d_name) ||
            installed_internally(e->d_name))
            continue;
        char game[PATH_MAX], app[PATH_MAX + 8], orig[PATH_MAX];
        snprintf(game, sizeof(game), "%s/%s", lib, e->d_name);
        snprintf(app, sizeof(app), "%s/app.pkg", game);
        if (exists(app) && !origin_find(game, "app.pkg", orig, sizeof(orig)))
            snprintf(tids[n++], 10, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

/* organize off: gives back the pkgs of the games PS4Mount had organized, once per drive.
 * A game left open is not closed behind the user's back: the next scan tries again. */
static void migrate_organized(void) {
    if (g_cfg.organize)
        return;
    int pending = 0;
    for (int i = 0; i < NROOTS && !pending; i++) {
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        pending = !g_migrated[i] && root_present(root);
    }
    if (!pending)
        return;
    uint32_t ids[8];
    char opened[8][10];
    if (open_games(ids, opened, 8))
        return;
    char tids[MAX_TITLES][10];
    int n = 0;
    for (int i = 0; i < NROOTS && n < MAX_TITLES; i++) {
        if (g_migrated[i])
            continue;
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!root_present(root))
            continue; /* plugged in later: tried then */
        g_migrated[i] = 1;
        n += organized_on_root(root, tids + n, MAX_TITLES - n);
    }
    if (!n)
        return;
    notify(TR("Organize is off: moving the pkgs of %d game(s) back where they were",
              "Organizar está apagado: devolviendo los pkgs de %d juego(s) a donde estaban"),
           n);
    run_restore(tids, n, 0);
}

int app_restore(const char *tids, char *msg, size_t cap) {
    if (!g_cfg.organize) {
        /* Nothing was moved or renamed: the pkgs are the user's, where they are. */
        snprintf(msg, cap, "%s",
                 TR("Pkgs are used where they are, with their own names: there are no original files to restore",
                    "Los pkg se usan donde están, con sus propios nombres: no hay archivos originales que restaurar"));
        return -1;
    }
    if (g_restore_all || g_restore_n) {
        snprintf(msg, cap, "%s", TR("Already restoring files, wait for it to finish", "Ya se están restaurando archivos, espera a que termine"));
        return -1;
    }
    if (!tids) {
        g_restore_all = 1;
        snprintf(msg, cap, "%s",
                 TR("Closing the open game and restoring the original files of all games",
                    "Cerrando el juego abierto y restaurando los archivos originales de todos los juegos"));
        return 0;
    }
    int n = 0;
    for (const char *p = tids; *p;) {
        size_t len = strcspn(p, ",");
        char tid[10], root[PATH_MAX];
        if (len != 9 || !game_id_at(p)) {
            snprintf(msg, cap, "invalid title id: %.20s", p);
            return -1;
        }
        snprintf(tid, sizeof(tid), "%.9s", p);
        if (find_root(tid, root, sizeof(root))) {
            snprintf(msg, cap, TR("%s is not on any connected drive", "%s no está en ninguna unidad conectada"), tid);
            return -1;
        }
        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= !strcmp(g_restore_tids[i], tid);
        if (!dup && n < MAX_TITLES)
            snprintf(g_restore_tids[n++], 10, "%s", tid);
        p += len;
        if (*p == ',')
            p++;
    }
    if (!n) {
        snprintf(msg, cap, "%s", TR("No games selected", "No hay juegos seleccionados"));
        return -1;
    }
    g_restore_n = n;
    if (n == 1)
        snprintf(msg, cap,
                 TR("Closing the open game and restoring the original files of %s",
                    "Cerrando el juego abierto y restaurando los archivos originales de %s"),
                 g_restore_tids[0]);
    else
        snprintf(msg, cap,
                 TR("Closing the open game and restoring the original files of %d games, %d at a time",
                    "Cerrando el juego abierto y restaurando los archivos originales de %d juego(s), en grupos de %d"),
                 n, g_cfg.restore_batch);
    return 0;
}

int app_restored_forget(char *msg, size_t cap) {
    unlink(RESTORED_LIST);
    g_next_scan = 0;
    snprintf(msg, cap, "%s", TR("Restored pkgs will be used again on the next scan", "Los pkgs restaurados se volverán a usar en la próxima búsqueda"));
    return 0;
}

/* --- settings --- */

/* A scan folder: relative to the drive, "" for the root. Folders inside ps4/ (where
 * PS4Mount keeps the games) and anything leaving the drive are refused. */
static int clean_scan_path(const char *in, char *out, size_t n, char *why, size_t wn) {
    while (*in == ' ' || *in == '/')
        in++;
    snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '/' || out[l - 1] == '\r'))
        out[--l] = 0;
    if (l >= 120) {
        snprintf(why, wn, TR("folder name too long: %.40s...", "nombre de carpeta demasiado largo: %.40s..."), out);
        return -1;
    }
    if (strchr(out, '\\') || strchr(out, ':') || strstr(out, "//")) {
        snprintf(why, wn, TR("invalid folder: %s", "carpeta no válida: %s"), out);
        return -1;
    }
    for (const char *c = out; *c; c = strchr(c, '/') ? strchr(c, '/') + 1 : c + strlen(c))
        if (!strncmp(c, "..", 2) && (c[2] == '/' || !c[2])) {
            snprintf(why, wn, TR("folders outside the drive are not allowed: %s", "no se permiten carpetas fuera de la unidad: %s"), out);
            return -1;
        }
    if (!strncasecmp(out, "ps4", 3) && (!out[3] || out[3] == '/')) {
        snprintf(why, wn, "%s",
                 TR("the ps4 folder holds the games PS4Mount already moved; choose another folder",
                    "la carpeta ps4 guarda los juegos que PS4Mount ya movió; elige otra carpeta"));
        return -1;
    }
    return 0;
}

/* --- the network share --- */

/* The FUSE mount needs a firmware whose fusefs patch is known (5.05 to 13.52, itemzflow's;
 * tried on a console on 13.52 and 6.72). */
static int nas_supported(void) { return nas_fw_ok(g_fw); }

static void nas_cfg_from(nas_cfg_t *n) {
    memset(n, 0, sizeof(*n));
    n->proto = g_cfg.nas_proto == NAS_SMB ? NAS_SMB : NAS_NFS;
    n->port = g_cfg.nas_port;
    snprintf(n->server, sizeof(n->server), "%s", g_cfg.nas_server);
    snprintf(n->share, sizeof(n->share), "%s", g_cfg.nas_share);
    snprintf(n->sub, sizeof(n->sub), "%s", g_cfg.nas_sub);
    snprintf(n->user, sizeof(n->user), "%s", g_cfg.nas_user);
    snprintf(n->pass, sizeof(n->pass), "%s", g_cfg.nas_pass);
}

static time_t g_nas_next;      /* when to try again after a failure */
static time_t g_nas_fail_at;   /* when the failures started */
static char g_nas_last[192];   /* last failure, to not repeat it every 20 s */
static int g_nas_told;         /* that failure was already shown to the player */
static int g_nas_on;           /* connected when the last call looked */

/* Mounts the share or replaces its client, only when the settings say so. force: the
 * panel just saved and the address may have changed. */
static void nas_apply(int force) {
    if (!g_cfg.nas || !nas_supported()) {
        if (nas_mounted())
            nas_stop();
        if (g_nas_on)
            printf("[nas] off\n");
        g_nas_on = 0;
        return;
    }
    if (!force && nas_active())
        return;
    if (!force && nas_need_reload()) /* this process cannot open the devices: nas_ask_reload() */
        return;
    time_t now = time(NULL);
    if (!force && now < g_nas_next)
        return;

    nas_cfg_t n;
    nas_cfg_from(&n);
    char err[160] = "";
    int r = nas_mounted() ? nas_reconnect(&n, err, sizeof(err)) : nas_start(&n, err, sizeof(err));
    if (r) {
        g_nas_next = now + 20;
        if (!g_nas_fail_at)
            g_nas_fail_at = now;
        if (strcmp(err, g_nas_last)) {
            printf("[nas] %s: %s\n", nas_status(), err);
            snprintf(g_nas_last, sizeof(g_nas_last), "%s", err);
            g_nas_told = 0;
        }
        /* The first seconds after a boot the share can fail before the console
         * is ready: only tell the player when it keeps failing. */
        if (!g_nas_told && now - g_nas_fail_at >= 120) {
            notify(TR("NAS: %s", "NAS: %s"), err);
            g_nas_told = 1;
        }
        g_nas_on = 0;
        return;
    }
    g_nas_last[0] = 0;
    g_nas_told = 0;
    g_nas_fail_at = 0;
    g_nas_next = now + 3600; /* connected: no need to ask again */
    if (!g_nas_on)
        printf("[nas] ready: %s\n", nas_status());
    g_nas_on = 1;
    g_walk_now = 1; /* a new drive to walk */
}

/* A first start: nothing of PS4Mount on the console yet. No game is added until the settings
 * are saved once, so a drive plugged in already is not added whole with the defaults. A
 * console that ran an older version without ever saving them has catalogs or games. */
static int first_start(void) {
    if (exists(CONFIG_PATH))
        return 0;
    DIR *d = opendir(STATE_DIR);
    struct dirent *e;
    int found = 0;
    while (d && !found && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        found = !strncmp(e->d_name, "catalog-", 8) || (n > 4 && !strcmp(e->d_name + n - 4, ".src"));
    }
    if (d)
        closedir(d);
    return !found;
}

static void config_save(void) {
    g_setup = 0; /* the settings were chosen: games are added from now on */
    buf_t b = {0};
    buf_printf(&b, "ver=2\nbatch=%d\nbatch_size=%d\ndlc=%d\ndlc_big=%d\nrestore_batch=%d\ninternal=%d\norganize=%d\nlang=%s\n",
               g_cfg.batch, g_cfg.batch_size, g_cfg.dlc, g_cfg.dlc_big, g_cfg.restore_batch, g_cfg.internal, g_cfg.organize,
               g_cfg.lang >= 0 ? lang_tag(g_cfg.lang) : "auto");
    buf_printf(&b, "nas=%d\nnas_proto=%d\nnas_port=%d\nnas_server=%s\nnas_share=%s\nnas_sub=%s\nnas_user=%s\nnas_pass=%s\n",
               g_cfg.nas, g_cfg.nas_proto, g_cfg.nas_port, g_cfg.nas_server, g_cfg.nas_share,
               g_cfg.nas_sub, g_cfg.nas_user, g_cfg.nas_pass);
    buf_printf(&b, "nas_rescan=%d\n", g_cfg.nas_rescan);
    for (int i = 0; i < g_cfg.npaths; i++)
        buf_printf(&b, "path=%s\n", g_cfg.paths[i]);
    write_file(CONFIG_PATH, b.data, b.len, 0666);
    buf_free(&b);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return;
    config_t c = g_cfg;
    c.npaths = 0;
    char line[768], why[160], path[128];
    int ver = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "ver=", 4)) {
            ver = atoi(line + 4);
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "batch=", 6))
            c.batch = atoi(line + 6) != 0;
        else if (!strncmp(line, "batch_size=", 11))
            c.batch_size = atoi(line + 11);
        else if (!strncmp(line, "dlc=", 4))
            c.dlc = atoi(line + 4) != 0;
        else if (!strncmp(line, "dlc_big=", 8))
            c.dlc_big = atoi(line + 8) != 0;
        else if (!strncmp(line, "restore_batch=", 14))
            c.restore_batch = atoi(line + 14);
        else if (!strncmp(line, "internal=", 9))
            c.internal = atoi(line + 9) != 0;
        else if (!strncmp(line, "organize=", 9))
            c.organize = atoi(line + 9) != 0;
        else if (!strncmp(line, "lang=", 5))
            c.lang = lang_from_tag(line + 5);
        else if (!strncmp(line, "path=", 5) && c.npaths < MAX_SCAN_PATHS &&
                 !clean_scan_path(line + 5, path, sizeof(path), why, sizeof(why)))
            snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
        else if (!strncmp(line, "nas=", 4))
            c.nas = atoi(line + 4) != 0;
        else if (!strncmp(line, "nas_proto=", 10))
            c.nas_proto = atoi(line + 10) == NAS_SMB ? NAS_SMB : NAS_NFS;
        else if (!strncmp(line, "nas_port=", 9))
            c.nas_port = atoi(line + 9);
        else if (!strncmp(line, "nas_rescan=", 11)) {
            c.nas_rescan = atoi(line + 11);
            if (c.nas_rescan < 0 || c.nas_rescan > NAS_RESCAN_MAX)
                c.nas_rescan = 1;
        }
        else if (!strncmp(line, "nas_server=", 11))
            snprintf(c.nas_server, sizeof(c.nas_server), "%s", line + 11);
        else if (!strncmp(line, "nas_share=", 10))
            snprintf(c.nas_share, sizeof(c.nas_share), "%s", line + 10);
        else if (!strncmp(line, "nas_sub=", 8))
            snprintf(c.nas_sub, sizeof(c.nas_sub), "%s", line + 8);
        else if (!strncmp(line, "nas_user=", 9))
            snprintf(c.nas_user, sizeof(c.nas_user), "%s", line + 9);
        else if (!strncmp(line, "nas_pass=", 9))
            snprintf(c.nas_pass, sizeof(c.nas_pass), "%s", line + 9);
    }
    fclose(f);
    if (c.batch_size < 1 || c.batch_size > 50)
        c.batch_size = 5;
    if (c.restore_batch < 1 || c.restore_batch > MAX_RESTORE_BATCH)
        c.restore_batch = 5;
    if (!c.npaths) {
        snprintf(c.paths[0], sizeof(c.paths[0]), DEFAULT_SCAN_PATH);
        c.npaths = 1;
    }
    /* Before 0.7.5 the settings had no version and organize was off: the pkgs stayed where
     * they were, the console's installer found them again and a game could end up installed
     * on both drives (issue #4). The ps4 folder is the default again; turning it off is kept
     * from the next save on. */
    if (ver < 2)
        c.organize = 1;
    g_cfg = c;
    printf("[config] batch=%d batch_size=%d dlc=%d restore_batch=%d organize=%d folders=%d\n", g_cfg.batch, g_cfg.batch_size,
           g_cfg.dlc, g_cfg.restore_batch, g_cfg.organize, g_cfg.npaths);
}

void app_config_json(buf_t *out) {
    buf_printf(out,
               "{\"batch\":%s,\"batch_size\":%d,\"dlc\":%s,\"dlc_big\":%s,\"internal\":%s,\"organize\":%s,\"restore_batch\":%d,\"max_restore_batch\":%d,"
               "\"lang\":\"%s\",\"ui_lang\":\"%s\",\"nas_supported\":%s,\"nas\":%s,\"nas_proto\":\"%s\",\"nas_port\":%d,"
               "\"nas_rescan\":%d,\"nas_tested\":%s,\"nas_server\":",
               g_cfg.batch ? "true" : "false", g_cfg.batch_size, g_cfg.dlc ? "true" : "false", g_cfg.dlc_big ? "true" : "false",
               g_cfg.internal ? "true" : "false", g_cfg.organize ? "true" : "false", g_cfg.restore_batch, MAX_RESTORE_BATCH,
               g_cfg.lang >= 0 ? lang_tag(g_cfg.lang) : "auto", lang_tag(g_lang),
               nas_supported() ? "true" : "false", g_cfg.nas ? "true" : "false",
               g_cfg.nas_proto == NAS_SMB ? "smb" : "nfs", g_cfg.nas_port, g_cfg.nas_rescan,
               nas_fw_tested(g_fw) ? "true" : "false");
    json_str(out, g_cfg.nas_server);
    buf_printf(out, ",\"nas_share\":");
    json_str(out, g_cfg.nas_share);
    buf_printf(out, ",\"nas_sub\":");
    json_str(out, g_cfg.nas_sub);
    buf_printf(out, ",\"nas_user\":");
    json_str(out, g_cfg.nas_user);
    buf_printf(out, ",\"nas_has_pass\":%s,\"nas_reload\":%s,\"nas_status\":",
               g_cfg.nas_pass[0] ? "true" : "false", nas_need_reload() ? "true" : "false");
    json_str(out, nas_status());
    buf_printf(out, ",\"paths\":[");
    for (int i = 0; i < g_cfg.npaths; i++) {
        char shown[130];
        snprintf(shown, sizeof(shown), "/%s", g_cfg.paths[i]);
        buf_printf(out, "%s", i ? "," : "");
        json_str(out, shown);
    }
    buf_printf(out, "]}");
}

static void url_decode(const char *in, size_t len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        if (in[i] == '+')
            out[o++] = ' ';
        else if (in[i] == '%' && i + 2 < len) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else
            out[o++] = in[i];
    }
    out[o] = 0;
}

/* The value of key in a form-encoded body ("" when it is not there). */
static void form_field(const char *body, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && p[kl] == '=' && !strncmp(p, key, kl)) {
            url_decode(p + kl + 1, len - kl - 1, out, n);
            return;
        }
        p = amp ? amp + 1 : NULL;
    }
}

/* /api/nas/scan: the file servers on the console's network. */
void app_nas_scan_json(buf_t *out) {
    static nas_host_t h[64];
    int n = nas_supported() ? nas_scan(h, 64) : 0;
    buf_printf(out, "{\"hosts\":[");
    for (int i = 0; i < n; i++) {
        buf_printf(out, "%s{\"ip\":", i ? "," : "");
        json_str(out, h[i].ip);
        buf_printf(out, ",\"name\":");
        json_str(out, h[i].name);
        buf_printf(out, ",\"smb\":%s,\"nfs\":%s}", h[i].smb ? "true" : "false", h[i].nfs ? "true" : "false");
    }
    buf_printf(out, "]}");
}

/* /api/nas/shares, body proto=smb|nfs&server=&port=&user=&pass=: what that server shares.
 * An empty password is the saved one when server and user are the saved ones. */
void app_nas_shares_json(const char *body, buf_t *out) {
    nas_cfg_t c;
    char v[256];
    memset(&c, 0, sizeof(c));
    form_field(body, "proto", v, sizeof(v));
    c.proto = !strcmp(v, "smb") ? NAS_SMB : NAS_NFS;
    form_field(body, "port", v, sizeof(v));
    c.port = atoi(v);
    form_field(body, "server", c.server, sizeof(c.server));
    form_field(body, "user", c.user, sizeof(c.user));
    form_field(body, "pass", c.pass, sizeof(c.pass));
    if (!c.pass[0] && !strcmp(c.server, g_cfg.nas_server) && !strcmp(c.user, g_cfg.nas_user))
        snprintf(c.pass, sizeof(c.pass), "%s", g_cfg.nas_pass);
    static char names[64][NAS_NAME_MAX];
    char err[192] = "";
    int k = !nas_supported() ? -1 : c.server[0] ? nas_shares(&c, names, 64, err, sizeof(err)) : -1;
    if (k < 0 && !err[0])
        snprintf(err, sizeof(err), "%s", TR("no server given", "falta el servidor"));
    buf_printf(out, "{\"ok\":%s,\"auth\":%s,\"message\":", k >= 0 ? "true" : "false", k == -2 ? "true" : "false");
    json_str(out, k >= 0 ? "" : err);
    buf_printf(out, ",\"shares\":[");
    for (int i = 0; i < k; i++) {
        buf_printf(out, "%s", i ? "," : "");
        json_str(out, names[i]);
    }
    buf_printf(out, "]}");
}

/* body: batch=0|1&batch_size=N&dlc=0|1&restore_batch=N&paths=<folders, one per line> (form
 * encoded); keys left out keep their value. */
int app_config_set(const char *body, char *msg, size_t cap) {
    config_t c = g_cfg;
    int was_organize = c.organize;
    char value[1200], why[160] = "";
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&'), *eq = strchr(p, '=');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (eq && eq < p + len) {
            url_decode(eq + 1, (size_t)(p + len - eq - 1), value, sizeof(value));
            size_t k = (size_t)(eq - p);
            if (k == 5 && !strncmp(p, "batch", 5))
                c.batch = atoi(value) != 0;
            else if (k == 10 && !strncmp(p, "batch_size", 10))
                c.batch_size = atoi(value);
            else if (k == 3 && !strncmp(p, "dlc", 3))
                c.dlc = atoi(value) != 0;
            else if (k == 7 && !strncmp(p, "dlc_big", 7))
                c.dlc_big = atoi(value) != 0;
            else if (k == 8 && !strncmp(p, "internal", 8))
                c.internal = atoi(value) != 0;
            else if (k == 8 && !strncmp(p, "organize", 8))
                c.organize = atoi(value) != 0;
            else if (k == 4 && !strncmp(p, "lang", 4))
                c.lang = lang_from_tag(value); /* "auto" and anything else: the console's */
            else if (k == 13 && !strncmp(p, "restore_batch", 13)) {
                char *end;
                long v = strtol(value, &end, 10);
                c.restore_batch = *value && !*end ? (int)v : 0;
            } else if (k == 3 && !strncmp(p, "nas", 3))
                c.nas = atoi(value) != 0;
            else if (k == 9 && !strncmp(p, "nas_proto", 9))
                c.nas_proto = !strcmp(value, "smb") ? NAS_SMB : NAS_NFS;
            else if (k == 8 && !strncmp(p, "nas_port", 8))
                c.nas_port = atoi(value);
            else if (k == 10 && !strncmp(p, "nas_rescan", 10))
                c.nas_rescan = atoi(value);
            else if (k == 10 && !strncmp(p, "nas_server", 10))
                snprintf(c.nas_server, sizeof(c.nas_server), "%s", value);
            else if (k == 9 && !strncmp(p, "nas_share", 9))
                snprintf(c.nas_share, sizeof(c.nas_share), "%s", value);
            else if (k == 7 && !strncmp(p, "nas_sub", 7))
                snprintf(c.nas_sub, sizeof(c.nas_sub), "%s", value);
            else if (k == 8 && !strncmp(p, "nas_user", 8))
                snprintf(c.nas_user, sizeof(c.nas_user), "%s", value);
            else if (k == 8 && !strncmp(p, "nas_pass", 8)) { /* empty: keep the saved one */
                if (value[0])
                    snprintf(c.nas_pass, sizeof(c.nas_pass), "%s", value);
            } else if (k == 5 && !strncmp(p, "paths", 5)) {
                c.npaths = 0;
                for (char *line = strtok(value, "\n"); line; line = strtok(NULL, "\n")) {
                    char path[128];
                    if (clean_scan_path(line, path, sizeof(path), why, sizeof(why))) {
                        snprintf(msg, cap, TR("Not saved: %s", "No se guardó: %s"), why);
                        return -1;
                    }
                    int dup = 0;
                    for (int i = 0; i < c.npaths; i++)
                        dup |= !strcmp(c.paths[i], path);
                    if (dup)
                        continue;
                    if (c.npaths == MAX_SCAN_PATHS) {
                        snprintf(msg, cap, TR("Not saved: at most %d folders", "No se guardó: como máximo %d carpetas"), MAX_SCAN_PATHS);
                        return -1;
                    }
                    snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
                }
            }
        }
        p = amp ? amp + 1 : NULL;
    }
    if (c.batch_size < 1 || c.batch_size > 50) {
        snprintf(msg, cap, "%s", TR("Not saved: games per group must be between 1 and 50", "No se guardó: los juegos por grupo deben estar entre 1 y 50"));
        return -1;
    }
    if (c.restore_batch < 1 || c.restore_batch > MAX_RESTORE_BATCH) {
        snprintf(msg, cap,
                 TR("Not saved: games per group when moving back must be a number from 1 to %d",
                    "No se guardó: los juegos por grupo al devolver deben ser un número de 1 a %d"),
                 MAX_RESTORE_BATCH);
        return -1;
    }
    if (!c.npaths) {
        snprintf(msg, cap, "%s", TR("Not saved: add at least one folder (/ is the drive root)", "No se guardó: añade al menos una carpeta (/ es la raíz de la unidad)"));
        return -1;
    }
    /* NAS: only where the fusefs patch is known, read only, and never with the organizer. */
    char *sub = c.nas_sub;
    while (sub[0] == '/' || sub[0] == '\\')
        memmove(sub, sub + 1, strlen(sub));
    for (size_t l = strlen(sub); l && (sub[l - 1] == '/' || sub[l - 1] == '\\'); l--)
        sub[l - 1] = 0;
    if (c.nas && !nas_supported()) {
        snprintf(msg, cap, "%s", TR("Not saved: there is no NAS on this firmware", "No se guardó: no hay NAS en este firmware"));
        return -1;
    }
    if (c.nas && (!c.nas_server[0] || !c.nas_share[0])) {
        snprintf(msg, cap, "%s", TR("Not saved: the NAS needs a server and a share",
                                    "No se guardó: el NAS necesita un servidor y un recurso"));
        return -1;
    }
    if (c.nas_rescan < 0 || c.nas_rescan > NAS_RESCAN_MAX) {
        snprintf(msg, cap, "%s", TR("Not saved: the share check interval is out of range",
                                    "No se guardó: el intervalo de revisión del NAS no es válido"));
        return -1;
    }
    if (c.nas_port < 0 || c.nas_port > 65535) {
        snprintf(msg, cap, "%s", TR("Not saved: the port must be between 1 and 65535",
                                    "No se guardó: el puerto debe estar entre 1 y 65535"));
        return -1;
    }
    int nas_changed = c.nas != g_cfg.nas || c.nas_proto != g_cfg.nas_proto ||
                      c.nas_port != g_cfg.nas_port || strcmp(c.nas_server, g_cfg.nas_server) ||
                      strcmp(c.nas_share, g_cfg.nas_share) || strcmp(c.nas_sub, g_cfg.nas_sub) ||
                      strcmp(c.nas_user, g_cfg.nas_user) || strcmp(c.nas_pass, g_cfg.nas_pass);
    g_cfg = c;
    apply_lang();
    config_save();
    printf("[config] saved: batch=%d batch_size=%d dlc=%d restore_batch=%d organize=%d folders=%d\n", c.batch,
           c.batch_size, c.dlc, c.restore_batch, c.organize, c.npaths);
    if (was_organize && !c.organize) /* turned off: send the organized games back to where they were */
        for (int i = 0; i < NROOTS; i++)
            g_migrated[i] = 0;
    if (nas_changed)
        nas_apply(1);
    g_next_scan = 0;
    if (nas_changed && c.nas && !nas_active())
        snprintf(msg, cap, TR("Saved, but the NAS did not connect: %s", "Guardado, pero el NAS no conectó: %s"),
                 nas_status());
    else
        snprintf(msg, cap, "%s", TR("Settings saved", "Ajustes guardados"));
    return 0;
}

/* --- pkgs sent from the panel --- */

/* Free bytes for a new file in dir (or its drive when dir does not exist yet). */
static uint64_t free_bytes(const char *dir) {
    struct statfs sf;
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s", dir);
    while (statfs(p, &sf)) {
        char *slash = strrchr(p, '/');
        if (!slash || slash == p)
            return 0;
        *slash = 0;
    }
    return (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize;
}

/* The folder a pkg sent to dest ("usb0".."usb7", "ext0", "ext1", "internal") goes to: the
 * first scan folder of the drive, or /data/PS4Mount. -1 when dest is not usable now. */
static int upload_dir(const char *dest, char *dir, size_t n) {
    char root[PATH_MAX], id[64];
    for (int i = 0; i < NROOTS; i++) {
        root_path(i, root, sizeof(root));
        if (is_nas(root)) /* read only: nothing is sent there */
            continue;
        if (!strcmp(dest, root_id(root, id, sizeof(id))) && root_present(root)) {
            scan_dir(dir, n, root, 0);
            return 0;
        }
    }
    return -1;
}

static void dests_json(buf_t *out, uint64_t need) {
    char root[PATH_MAX], dir[PATH_MAX];
    int first = 1;
    buf_printf(out, "[");
    for (int i = 0; i < NROOTS; i++) {
        char id[64];
        root_path(i, root, sizeof(root));
        if (!root_present(root) || is_nas(root)) /* read only: nothing is sent there */
            continue;
        scan_dir(dir, sizeof(dir), root, 0);
        uint64_t fb = free_bytes(dir);
        buf_printf(out, "%s{\"id\":", first ? "" : ",");
        json_str(out, root_id(root, id, sizeof(id)));
        buf_printf(out, ",\"label\":");
        json_str(out, root_label(root));
        buf_printf(out, ",\"folder\":");
        json_str(out, dir);
        buf_printf(out, ",\"free\":%llu,\"fits\":%s}", (unsigned long long)fb, fb > need ? "true" : "false");
        first = 0;
    }
    buf_printf(out, "]");
}

/* Why PS4Mount cannot use a checked pkg of size bytes (0: not known); empty when it can. */
static void check_problem(const pkg_check_t *c, uint64_t size, char *problem, size_t n) {
    snprintf(problem, n, "%s", c->problem);
    if (!problem[0] && size && size != c->pkg_size)
        snprintf(problem, n,
                 TR("the file is %llu bytes but its header says %llu: it is incomplete or damaged",
                    "el archivo tiene %llu bytes pero su cabecera dice %llu: está incompleto o dañado"),
                 (unsigned long long)size, (unsigned long long)c->pkg_size);
    if (!problem[0] && c->kind != PKG_KIND_ADDON && installed_internally(c->title_id))
        snprintf(problem, n,
                 TR("%s is installed on the console the normal way: PS4Mount leaves it alone",
                    "%s está instalado en la consola de la forma normal: PS4Mount no lo toca"),
                 c->title_id);
}

void app_check(const void *data, size_t len, uint64_t size, buf_t *out) {
    static const char *tmp = STATE_DIR "/check.part";
    pkg_check_t c;
    mkdirs(STATE_DIR, 0777);
    int rc = write_file(tmp, data, len, 0666) ? -1 : pkg_check(tmp, g_fw, &c);
    unlink(tmp);
    if (rc) {
        buf_printf(out, "{\"ok\":false,\"problem\":\"%s\"}", TR("not a PS4 pkg", "no es un pkg de PS4"));
        return;
    }
    char problem[256], notes[3][200], sv[16], fw[16], where[32];
    int nnotes = 0;
    check_problem(&c, size, problem, sizeof(problem));
    if (c.warning[0])
        snprintf(notes[nnotes++], sizeof(notes[0]), "%s", c.warning);
    if (c.kind == PKG_KIND_ADDON && !g_cfg.dlc)
        snprintf(notes[nnotes++], sizeof(notes[0]), "%s",
                 TR("add-ons are turned off in the settings: it will wait in the folder",
                    "los DLC están desactivados en los ajustes: esperará en la carpeta"));
    if (c.kind == PKG_KIND_UPDATE && find_root(c.title_id, where, sizeof(where)) && !db_is_registered(c.title_id))
        snprintf(notes[nnotes++], sizeof(notes[0]), "%s",
                 TR("the game of this update is not on a drive yet: send it too",
                    "el juego de esta actualización aún no está en ninguna unidad: envíalo también"));
    if (c.kind == PKG_KIND_GAME && !find_root(c.title_id, where, sizeof(where)))
        snprintf(notes[nnotes++], sizeof(notes[0]),
                 TR("%s already has a game in %s: this copy will be left where it is",
                    "%s ya tiene un juego en %s: esta copia se dejará donde está"),
                 c.title_id, root_label(where));
    ver_text(c.system_ver, sv, sizeof(sv));
    ver_text(g_fw, fw, sizeof(fw));
    buf_printf(out, "{\"ok\":%s,\"install\":%s,\"problem\":", problem[0] ? "false" : "true",
               c.installable && !installed_internally(c.title_id) ? "true" : "false");
    json_str(out, problem);
    buf_printf(out, ",\"kind\":\"%s\",\"title\":", pkg_kind_name(c.kind));
    json_str(out, c.title);
    buf_printf(out, ",\"title_id\":");
    json_str(out, c.title_id);
    buf_printf(out, ",\"content_id\":");
    json_str(out, c.content_id);
    buf_printf(out, ",\"app_ver\":");
    json_str(out, c.app_ver);
    buf_printf(out, ",\"version\":");
    json_str(out, c.version);
    buf_printf(out, ",\"system_ver\":");
    json_str(out, c.system_ver ? sv : "");
    buf_printf(out, ",\"console_fw\":");
    json_str(out, fw);
    buf_printf(out, ",\"pkg_size\":%llu,\"notes\":[", (unsigned long long)c.pkg_size);
    for (int i = 0; i < nnotes; i++) {
        buf_printf(out, "%s", i ? "," : "");
        json_str(out, notes[i]);
    }
    buf_printf(out, "],\"dests\":");
    dests_json(out, c.pkg_size);
    buf_printf(out, "}");
}

static void upload_done(const char *path) {
    (void)path;
    app_rescan(); /* the scan picks it up now */
}

/* --- homebrew apps sent to be installed --- */
/* A small homebrew app is not used from its pkg: sent from the panel with "Send and
 * install", it lands in INSTALL_DIR and goes to the system's installer, like a pkg
 * installed by hand. The copy is deleted once the system has the app. */
#define INSTALL_DIR STATE_DIR "/install"
#define INSTALL_KEEP_SECONDS 600 /* a copy is only deleted this long after it arrived */

static pthread_mutex_t g_inst_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_inst_path[PATH_MAX]; /* received, for the main loop to install */

static void upload_install_done(const char *path) {
    pthread_mutex_lock(&g_inst_mu);
    snprintf(g_inst_path, sizeof(g_inst_path), "%s", path);
    pthread_mutex_unlock(&g_inst_mu);
}

/* The title ID and title of the pkg at path; 0 when read. */
static int pkg_id_title(const char *path, char *tid, char *title, size_t n) {
    pkg_t *pkg = mem_alloc(sizeof(*pkg));
    sfo_t *sfo = mem_alloc(sizeof(*sfo));
    int rc = -1;
    if (pkg && sfo && !pkg_open(pkg, path)) {
        snprintf(tid, 10, "%s", pkg->title_id);
        const sfo_item_t *t = read_sfo(pkg, sfo) ? NULL : sfo_get(sfo, "TITLE");
        snprintf(title, n, "%s", t && !t->is_int ? t->sval : pkg->title_id);
        pkg_close(pkg);
        rc = 0;
    }
    mem_free(pkg);
    mem_free(sfo);
    return rc;
}

/* The installer service is used from the main loop only (the upload has a thread of its own). */
static void install_received(void) {
    char path[PATH_MAX];
    pthread_mutex_lock(&g_inst_mu);
    snprintf(path, sizeof(path), "%s", g_inst_path);
    g_inst_path[0] = 0;
    pthread_mutex_unlock(&g_inst_mu);
    if (!path[0])
        return;
    char tid[10] = "", title[128] = "", url[PATH_MAX + 8], msg[PATH_MAX + 64];
    if (pkg_id_title(path, tid, title, sizeof(title)))
        snprintf(title, sizeof(title), "%s", path);
    int32_t user = 0;
    sceUserServiceGetForegroundUser(&user);
    int task = install_pkg(pkg_url(tid[0] ? tid : "install", "up", path, url, sizeof(url)), title, user, msg,
                           sizeof(msg));
    printf("[install] %s: %s\n", path, msg);
    if (task < 0)
        notify(TR("%s could not be installed: the system's installer turned it down (see the log)",
                  "%s no se pudo instalar: el instalador del sistema lo rechazó (mira el log)"),
               title);
    else
        notify(TR("Installing %s: it shows on the home screen when it is done", "Instalando %s: aparece en la pantalla de inicio cuando termine"),
               title);
}

/* Copies whose app the system has installed by now (whole, and a while ago) are deleted. */
static void install_cleanup(void) {
    DIR *d = opendir(INSTALL_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".pkg"))
            continue; /* the part being received, or anything else */
        char path[PATH_MAX], tid[10] = "", title[128], app[64];
        struct stat st, ast;
        snprintf(path, sizeof(path), INSTALL_DIR "/%s", e->d_name);
        if (stat(path, &st) || time(NULL) - st.st_mtime < INSTALL_KEEP_SECONDS || pkg_id_title(path, tid, title, sizeof(title)))
            continue;
        snprintf(app, sizeof(app), "/user/app/%s/app.pkg", tid);
        if (!installed_internally(tid) || !db_is_registered(tid) || stat(app, &ast) || ast.st_size != st.st_size)
            continue;
        unlink(path);
        printf("[install] %s installed: %s deleted\n", tid, path);
    }
    if (d)
        closedir(d);
}

/* --- the panel app --- */
/* A home screen tile that shows the panel (panel-app/): few find the panel from a notice.
 * When the console does not have it, its pkg is downloaded from this version's release and
 * installed like an app sent with "Send and install"; a first start opens it. Once it was
 * installed, a tile the player deleted is not put back (PANEL_APP_DONE). */
#define PANEL_APP_URL "https://github.com/Vld439/PS4Mount-PS4/releases/download/v%s/ps4mount-panel.pkg"
#define PANEL_APP_DONE STATE_DIR "/panel-app"
#define PANEL_APP_DELAY 15 /* seconds after the start: the network is up by then */

typedef struct { uint32_t sz, user_id, app_opt; uint64_t crash_report, check_flag; } lnc_param_t;
int sceLncUtilLaunchApp(const char *tid, const char *argv[], lnc_param_t *param);

static int g_panel_fetching; /* the download thread runs; done in this load either way */

static int panel_app_sink(void *ctx, const uint8_t *data, size_t len) {
    return fwrite(data, 1, len, ctx) != len;
}

static void *panel_app_fetch(void *arg) {
    (void)arg;
    char url[2048], err[192] = "";
    const char *part = INSTALL_DIR "/ps4mount-panel.part", *pkg = INSTALL_DIR "/ps4mount-panel.pkg";
    snprintf(url, sizeof(url), PANEL_APP_URL, &VERSION[sizeof("PS4Mount ") - 1]);
    mkdir(INSTALL_DIR, 0777);
    FILE *f = fopen(part, "wb");
    int status = 0;
    for (int hop = 0; f && hop < 5; hop++) { /* GitHub answers with a redirect to its file server */
        http_req_t q = {.method = "GET", .url = url, .sink = panel_app_sink, .sink_ctx = f, .any_server = 1};
        http_resp_t r;
        memset(&r, 0, sizeof(r));
        if (http_request(&q, &r, err, sizeof(err))) {
            http_resp_free(&r);
            break;
        }
        status = r.status;
        if (status / 100 == 3 && r.location[0])
            snprintf(url, sizeof(url), "%s", r.location);
        http_resp_free(&r);
        if (status / 100 != 3)
            break;
        rewind(f); /* the redirect's own body is not the file */
        if (ftruncate(fileno(f), 0))
            break;
    }
    int ok = f && status == 200 && !fflush(f) && ftell(f) > 0;
    if (f)
        fclose(f);
    if (ok && !rename(part, pkg)) {
        printf("[panel-app] downloaded %s\n", pkg);
        upload_install_done(pkg);
    } else {
        unlink(part);
        printf("[panel-app] not downloaded (%d %s): %s\n", status, err, url);
    }
    __atomic_store_n(&g_panel_fetching, 2, __ATOMIC_RELEASE);
    return NULL;
}

/* The tile, from the main loop: downloaded once per load when missing, and opened by a
 * first start (not over a game). */
static void panel_app_step(void) {
    static time_t start;
    static int opened;
    if (!start)
        start = time(NULL);
    int have = installed_internally(PANEL_APP_TID);
    if (have && !exists(PANEL_APP_DONE)) {
        int fd = open(PANEL_APP_DONE, O_WRONLY | O_CREAT, 0666);
        if (fd >= 0)
            close(fd);
    }
    if (have && g_setup && !opened && db_is_registered(PANEL_APP_TID)) {
        uint32_t ids[1];
        char tids[1][10];
        opened = 1;
        if (!open_games(ids, tids, 1)) {
            int32_t user = 0;
            sceUserServiceGetForegroundUser(&user);
            lnc_param_t lp = {sizeof(lp), (uint32_t)user, 0, 0, 0};
            printf("[panel-app] opened for the first start: 0x%x\n", (unsigned)sceLncUtilLaunchApp(PANEL_APP_TID, NULL, &lp));
        }
    }
    if (have || exists(PANEL_APP_DONE) || g_panel_fetching || time(NULL) - start < PANEL_APP_DELAY)
        return;
    pthread_t th;
    g_panel_fetching = 1;
    if (!pthread_create(&th, NULL, panel_app_fetch, NULL))
        pthread_detach(th);
}

int app_upload(int fd, const char *query, const void *pre, size_t pre_len, uint64_t length, char *msg, size_t cap) {
    char dest[16] = "", raw[512] = "", name[256], dir[PATH_MAX], inst[4] = "";
    const char *q = strchr(query, '?');
    for (const char *p = q ? q + 1 : ""; *p;) {
        const char *amp = strchr(p, '&'), *eq = strchr(p, '=');
        size_t l = amp ? (size_t)(amp - p) : strlen(p);
        if (eq && eq < p + l) {
            size_t k = (size_t)(eq - p);
            if (k == 4 && !strncmp(p, "dest", 4))
                url_decode(eq + 1, (size_t)(p + l - eq - 1), dest, sizeof(dest));
            else if (k == 4 && !strncmp(p, "name", 4))
                url_decode(eq + 1, (size_t)(p + l - eq - 1), raw, sizeof(raw));
            else if (k == 7 && !strncmp(p, "install", 7))
                url_decode(eq + 1, (size_t)(p + l - eq - 1), inst, sizeof(inst));
        }
        p = amp ? amp + 1 : p + l;
    }
    int install = !strcmp(inst, "1");
    if (upload_clean_name(raw, name, sizeof(name))) {
        snprintf(msg, cap, "%s", TR("no file name", "falta el nombre del archivo"));
        return -1;
    }
    if (install)
        snprintf(dir, sizeof(dir), "%s", INSTALL_DIR); /* on the console: installed from there */
    else if (upload_dir(dest, dir, sizeof(dir))) {
        snprintf(msg, cap, TR("%s is not available", "%s no está disponible"), dest[0] ? dest : TR("the destination", "el destino"));
        return -1;
    }
    mkdirs(dir, 0777);
    uint64_t fb = free_bytes(dir), margin = 64ull << 20;
    if (fb < length + margin) {
        snprintf(msg, cap, TR("not enough space in %s: %llu MB free, %llu MB needed", "no hay espacio en %s: %llu MB libres, %llu MB necesarios"), dir,
                 (unsigned long long)(fb >> 20), (unsigned long long)((length + margin) >> 20));
        return -1;
    }
    if (upload_start(fd, dir, name, length, pre, pre_len, g_fw, install, install ? upload_install_done : upload_done)) {
        snprintf(msg, cap, "%s", TR("another pkg is being received: wait for it to finish", "ya se está recibiendo otro pkg: espera a que termine"));
        return -1;
    }
    return 0;
}

void app_uploads_json(buf_t *out) { upload_status_json(out); }

void app_dests_json(buf_t *out) {
    buf_printf(out, "{\"dests\":");
    dests_json(out, 0);
    buf_printf(out, "}");
}

/* --- Google Drive backups --- */

/* The title of the pkg at path (the update's wins, as on the console); 0 when read. */
static int pkg_title(const char *path, char *out, size_t n) {
    pkg_t *pkg = mem_alloc(sizeof(*pkg));
    sfo_t *sfo = mem_alloc(sizeof(*sfo));
    int rc = -1;
    if (pkg && sfo && !pkg_open(pkg, path)) {
        const sfo_item_t *t = read_sfo(pkg, sfo) ? NULL : sfo_get(sfo, "TITLE");
        if (t && !t->is_int) {
            snprintf(out, n, "%s", t->sval);
            rc = 0;
        }
        pkg_close(pkg);
    }
    mem_free(pkg);
    mem_free(sfo);
    return rc;
}

static void drive_add_file(drive_file_t *files, int *nf, int max, const char *path, const char *name, const char *kind) {
    struct stat st;
    if (*nf >= max || stat(path, &st) || !S_ISREG(st.st_mode))
        return;
    drive_file_t *f = &files[(*nf)++];
    snprintf(f->path, sizeof(f->path), "%s", path);
    snprintf(f->name, sizeof(f->name), "%s", name);
    snprintf(f->kind, sizeof(f->kind), "%s", kind);
    f->size = (uint64_t)st.st_size;
}

/* The pkgs of a library game: its app.pkg, update and add-ons, in <root>/ps4/<TID> or
 * wherever the catalog found them when pkgs are used where they are. Runs in the Drive
 * thread: it only reads (the catalog under its lock). */
static int drive_game_files(const char *tid, drive_file_t *files, int max, char *name, size_t nn) {
    char root[PATH_MAX], p[PATH_MAX];
    int nf = 0;
    if (!g_cfg.organize || nas_active()) {
        char app[PATH_MAX] = "", patch[PATH_MAX] = "";
        pthread_mutex_lock(&g_cat_lock);
        for (int i = 0; i < NROOTS && !app[0]; i++) {
            root_path(i, root, sizeof(root));
            if (!root_present(root) || !in_place(root))
                continue;
            const catalog_t *c = drive_catalog(root);
            const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
            if (!g)
                continue;
            snprintf(app, sizeof(app), "%s", c->pkgs[g->app].path);
            const cat_pkg_t *up = catalog_patch(g_cat, c, g);
            if (up)
                snprintf(patch, sizeof(patch), "%s", up->path);
            drive_add_file(files, &nf, max, app, "game.pkg", "game");
            if (patch[0])
                drive_add_file(files, &nf, max, patch, "update.pkg", "update");
            const cat_pkg_t *list[CAT_MAX_DLC];
            int nd = catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC);
            for (int k = 0; k < nd && nf < max; k++) {
                char fname[96];
                snprintf(fname, sizeof(fname), "addon-%.9s-%.40s.pkg", list[k]->title_id, list[k]->content_id + 20);
                drive_add_file(files, &nf, max, list[k]->path, fname, "addon");
            }
        }
        pthread_mutex_unlock(&g_cat_lock);
        if (!nf)
            return 0;
        if (pkg_title(app, name, nn))
            snprintf(name, nn, "%s", tid);
        if (patch[0])
            pkg_title(patch, name, nn);
        return nf;
    }
    if (find_root(tid, root, sizeof(root)))
        return 0;
    char dir[PATH_MAX], fname[96];
    snprintf(dir, sizeof(dir), "%s/ps4/%s", root, tid);
    snprintf(p, sizeof(p), "%s/app.pkg", dir);
    drive_add_file(files, &nf, max, p, "game.pkg", "game");
    if (!nf)
        return 0;
    if (pkg_title(p, name, nn))
        snprintf(name, nn, "%s", tid);
    snprintf(p, sizeof(p), "%s/patch.pkg", dir);
    if (exists(p)) {
        drive_add_file(files, &nf, max, p, "update.pkg", "update");
        pkg_title(p, name, nn);
    }
    snprintf(p, sizeof(p), "%s/addcont", dir);
    DIR *d = opendir(p);
    struct dirent *e, *l;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/addcont/%s", dir, e->d_name);
        DIR *ld = opendir(sub);
        while (ld && (l = readdir(ld))) {
            if (l->d_name[0] == '.')
                continue;
            snprintf(p, sizeof(p), "%s/%s/ac.pkg", sub, l->d_name);
            snprintf(fname, sizeof(fname), "addon-%.9s-%.40s.pkg", e->d_name, l->d_name);
            drive_add_file(files, &nf, max, p, fname, "addon");
        }
        if (ld)
            closedir(ld);
    }
    if (d)
        closedir(d);
    return nf;
}

static int drive_library(char (*tids)[10], int max) {
    int n = 0;
    static char found[MAX_TITLES][10];
    pthread_mutex_lock(&g_cat_lock);
    for (int i = 0; i < NROOTS; i++) {
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!root_present(root))
            continue;
        int nf = titles_on_root(root, found, MAX_TITLES);
        for (int k = 0; k < nf && n < max; k++) {
            int seen = 0;
            for (int j = 0; j < n; j++)
                seen |= !strcmp(tids[j], found[k]);
            if (!seen)
                snprintf(tids[n++], 10, "%s", found[k]);
        }
    }
    pthread_mutex_unlock(&g_cat_lock);
    return n;
}

static int drive_dest_dir(const char *dest, char *dir, size_t n) {
    if (upload_dir(dest, dir, n))
        return -1;
    mkdirs(dir, 0777);
    return 0;
}

static void drive_downloaded(const char *tid) {
    (void)tid;
    app_rescan(); /* the scan adds it now */
}

static void drive_notify(const char *msg) { notify("%s", msg); }

static void drive_begin(void) {
    static const drive_host_t host = {drive_game_files, drive_library, drive_dest_dir, free_bytes, drive_downloaded,
                                      drive_notify};
    drive_start(STATE_DIR, NULL, &host);
}

/* --- downloads from links --- */

static int links_check(const char *path, uint64_t size, char *problem, size_t n) {
    pkg_check_t c;
    if (pkg_check(path, g_fw, &c)) {
        snprintf(problem, n, "%s", TR("not a PS4 pkg", "no es un pkg de PS4"));
        return -1;
    }
    check_problem(&c, size, problem, n);
    return problem[0] ? -1 : 0;
}

static void links_arrived(const char *dir) {
    (void)dir;
    app_rescan(); /* the scan adds them now */
}

static void links_begin(void) {
    static const links_host_t host = {drive_dest_dir, free_bytes, links_check, links_arrived, drive_notify};
    links_start(STATE_DIR, &host);
}

/* --- Media tab shortcut (category 65536 + deeplinkUri, as other PS5 web panels) --- */

#define PANEL_ID "PMNT00001"
static const char k_panel_param[] =
    "{\n"
    "    \"titleId\": \"" PANEL_ID "\",\n"
    "    \"applicationCategoryType\": 65536,\n"
    "    \"deeplinkUri\": \"http://127.0.0.1:8770/\",\n"
    "    \"localizedParameters\": {\n"
    "        \"defaultLanguage\": \"en-US\",\n"
    "        \"en-US\": {\n"
    "            \"titleName\": \"PS4Mount\"\n"
    "        }\n"
    "    }\n"
    "}\n";

static int same_file(const char *path, const void *data, size_t size) {
    struct stat st;
    if (stat(path, &st) || (size_t)st.st_size != size)
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t *tmp = mem_alloc(size);
    int same = tmp && read(fd, tmp, size) == (ssize_t)size && !memcmp(tmp, data, size);
    mem_free(tmp);
    close(fd);
    return same;
}

/* Installs the shortcut when it is missing or out of date (deleting it from the
 * home screen makes the next start install it again). */
__attribute__((unused)) static void install_panel_shortcut(void) {
    const char *param = "/user/app/" PANEL_ID "/sce_sys/param.json", *icon = "/user/app/" PANEL_ID "/sce_sys/icon0.png";
    /* sceAppInstUtilAppExists reports 0 for this shortcut on fw 13.20; app.db is reliable */
    int present = db_is_registered(PANEL_ID), rc = 0;
    int same_param = same_file(param, k_panel_param, sizeof(k_panel_param) - 1);
    int same_icon = same_file(icon, icon0_png, sizeof(icon0_png));
    printf("[panel] exists rc=0x%08x present=%d param=%d icon=%d\n", rc, present, same_param, same_icon);
    if (!rc && present && same_param && same_icon)
        return;
    mkdirs("/user/app/" PANEL_ID "/sce_sys", 0755);
    if (write_file(param, k_panel_param, sizeof(k_panel_param) - 1, 0644) ||
        write_file(icon, icon0_png, sizeof(icon0_png), 0644)) {
        printf("[panel] shortcut files: %s\n", strerror(errno));
        return;
    }
    rc = register_call(PANEL_ID);
    printf("[panel] shortcut install rc=0x%08x\n", rc);
    if (!rc)
        notify("%s", TR("PS4Mount added to the Media tab", "PS4Mount añadido a la pestaña Multimedia"));
}

static int g_klog = -1, g_web = -1;

/* Closes the ports and the log before main returns: the process stays (it is the
 * loader's) and anything left open in it would keep the next PS4Mount out. */
static void shutdown_daemon(int lock) {
    nas_release();
    links_stop();
    drive_stop();
    install_close();
    if (g_web >= 0)
        close(g_web);
    if (g_klog >= 0)
        close(g_klog);
    if (lock >= 0)
        close(lock);
    g_web = g_klog = -1;
    log_stop();
}

static int single_instance(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LOCK_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    return s;
}

/* ---- the share and GoldHEN's AutoRun ---- */
/* A payload started at boot by GoldHEN's AutoRun runs inside GoldHEN's own process, where
 * the fuse devices cannot be opened. Loaded through BinLoader it gets a process of its
 * own, where they can: the boot instance asks for that load, and the new one asks this
 * one to step aside. Nothing is closed before the new process is known to be running, so
 * a load that does not come leaves the panel as it was. */

#define NAS_WAIT STATE_DIR "/nas-wait" /* written before asking: the new one takes over */

static int send_all(int s, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        ssize_t w = send(s, p, len, 0);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* A payload file that the console's loader takes: the wrapped .bin (or the ELF). Only the
 * first bytes are read: the file is 3 MB and this runs where the heap is small. */
static const char *payload_file(void) {
    static const char *cand[] = {"/data/payloads/PS4Mount-PS4.bin", "/user/data/payloads/PS4Mount-PS4.bin",
                                 "/data/ps4mount/PS4Mount-PS4.bin", "/data/payloads/ps4mount-ps4.bin", NULL};
    static char found[PATH_MAX];
    for (int i = 0; cand[i]; i++) {
        struct stat st;
        if (stat(cand[i], &st) || st.st_size < 4096)
            continue;
        uint8_t d[8] = {0};
        int fd = open(cand[i], O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, d, sizeof(d));
        close(fd);
        if (n < 4)
            continue;
        if (!memcmp(d, "\177ELF", 4) || (d[0] == 'U' && d[1] == 'H')) { /* UH: jumps to main */
            snprintf(found, sizeof(found), "%s", cand[i]);
            return found;
        }
    }
    return NULL;
}

/* One line (without its CRLF) within ms; 1 when the socket said something. */
static int recv_line(int s, char *out, size_t n, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t i = 0;
    int any = 0;
    for (;;) {
        char c;
        ssize_t r = recv(s, &c, 1, 0);
        if (r <= 0)
            break;
        any = 1;
        if (c == '\n')
            break;
        if (c != '\r' && i + 1 < n)
            out[i++] = c;
    }
    out[i] = 0;
    return any;
}

/* The console's own address, to reach a loader that is not bound to loopback. */
static int lan_ip(struct in_addr *out) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0}, me;
    socklen_t n = sizeof(me);
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    a.sin_addr.s_addr = htonl(0x08080808);
    int ok = s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a)) &&
             !getsockname(s, (struct sockaddr *)&me, &n);
    if (s >= 0)
        close(s);
    if (ok)
        *out = me.sin_addr;
    return ok;
}

/* Sent in pieces: the file does not fit in memory here. The headers, and the wait for the
 * "100 Continue" before the body, are curl's: GoldHEN's payloader reads the request with a
 * small parser that turns down anything else ("Error handling payload") and wants the body
 * only after the go-ahead. 0 taken, 1 nothing listening, -1 turned down. */
static int post_payload(const char *path, struct in_addr ip) {
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(9090);
    a.sin_addr = ip;
    if (s < 0) {
        close(fd);
        return 1;
    }
    struct timeval to = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) {
        close(s);
        close(fd);
        return 1;
    }
    int on = 1;
    setsockopt(s, IPPROTO_TCP, 0x01 /* TCP_NODELAY */, &on, sizeof(on));
    char head[256];
    int h = snprintf(head, sizeof(head),
                     "POST / HTTP/1.1\r\nHost: %s:9090\r\nUser-Agent: curl/8.5.0\r\nAccept: */*"
                     "\r\nContent-Length: %lld\r\nContent-Type: application/x-www-form-urlencoded"
                     "\r\nExpect: 100-continue\r\n\r\n",
                     inet_ntoa(ip), (long long)st.st_size);
    int rc = 0;
    char line[160];
    if (send_all(s, head, (size_t)h)) {
        printf("[nas] BinLoader: headers not sent (%d)\n", errno);
        rc = -1;
    } else if (recv_line(s, line, sizeof(line), 2500) && line[0]) {
        printf("[nas] BinLoader: %s\n", line);
        if (!strncmp(line, "HTTP/1.1 4", 10) || !strncmp(line, "HTTP/1.1 5", 10))
            rc = -1;
    }
    if (!rc) {
        static char buf[65536];
        ssize_t r;
        while ((r = read(fd, buf, sizeof(buf))) > 0)
            if (send_all(s, buf, (size_t)r)) {
                printf("[nas] BinLoader: body cut (%d)\n", errno);
                rc = -1;
                break;
            }
        if (r < 0)
            rc = -1;
    }
    if (!rc && recv_line(s, line, sizeof(line), 20000) && line[0]) {
        printf("[nas] BinLoader: %s\n", line);
        if (!strncmp(line, "HTTP/1.1 4", 10) || !strncmp(line, "HTTP/1.1 5", 10))
            rc = -1;
    }
    close(s);
    close(fd);
    return rc;
}

/* Seconds since the console booted. */
static long uptime_secs(void) {
    struct timespec up;
    if (clock_gettime(CLOCK_MONOTONIC, &up))
        return 0;
    return (long)up.tv_sec;
}

#define NAS_TAKEOVER_AFTER 30 /* GoldHEN is still starting: its loader does not work yet */

/* The boot instance cannot open the fuse devices, so it asks BinLoader for a load of its
 * own file. BinLoader is only listening once it is switched on in GoldHEN's menu, which can
 * be any time after the console starts: keep asking (every 10 s) until it takes it once. */
static time_t g_nas_ask_at;
static int g_nas_said; /* the reason was already logged */
static time_t g_quiet_until; /* the load that was taken should have taken over by then */
static int nas_waiting(void);
static void notify_running(void);

#define NAS_TAKEOVER_WAIT 30

/* No load is taking over: this instance stays and says so. */
static void nas_speak(void) {
    if (!g_quiet)
        return;
    g_quiet = 0;
    notify_running();
}

static void nas_ask_reload(void) {
    if (!nas_need_reload())
        return;
    if (uptime_secs() < NAS_TAKEOVER_AFTER)
        return;
    time_t now = time(NULL);
    /* once per boot: a load that was taken but never took over must not be sent again
     * and again (GoldHEN's loader and the Shell UI stopped answering under that) */
    if (nas_waiting()) {
        if (!g_quiet_until)
            g_quiet_until = now + NAS_TAKEOVER_WAIT;
        if (g_quiet && now >= g_quiet_until) {
            printf("[nas] the load did not take over: this instance stays\n");
            nas_speak();
        }
        if (!g_nas_said) {
            printf("[nas] BinLoader took the payload already this boot: not sent again\n");
            g_nas_said = 1;
        }
        return;
    }
    if (now < g_nas_ask_at)
        return;
    g_nas_ask_at = now + 10;
    const char *file = payload_file();
    if (!file) {
        printf("[nas] no payload file to load again\n");
        nas_speak();
        return;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%ld\n", boot_time());
    write_file(NAS_WAIT, buf, (size_t)n, 0666);
    struct in_addr lan;
    int have_lan = lan_ip(&lan);
    for (int i = 0; i < 3; i++) {
        struct in_addr lo;
        lo.s_addr = htonl(INADDR_LOOPBACK);
        if (!post_payload(file, lo)) {
            g_nas_said = 0;
            g_quiet_until = now + NAS_TAKEOVER_WAIT;
            printf("[nas] BinLoader took %s: loading again\n", file);
            return;
        }
        if (have_lan && !post_payload(file, lan)) {
            g_nas_said = 0;
            g_quiet_until = now + NAS_TAKEOVER_WAIT;
            printf("[nas] BinLoader took %s: loading again\n", file);
            return;
        }
        usleep(1500000);
    }
    unlink(NAS_WAIT);
    if (!g_nas_said) {
        printf("[nas] BinLoader (:9090) did not take the payload\n");
        g_nas_said = 1;
    }
    nas_speak();
}

/* The panel: the same request the retry makes, now. */
int app_nas_reload(char *msg, size_t cap) {
    const char *file = payload_file();
    if (!file) {
        snprintf(msg, cap, "%s", TR("the payload is not where the panel expects it",
                                    "el payload no está donde el panel lo espera"));
        return 1;
    }
    char buf[32];
    int k = snprintf(buf, sizeof(buf), "%ld\n", boot_time());
    write_file(NAS_WAIT, buf, (size_t)k, 0666);
    struct in_addr lo, lan;
    lo.s_addr = htonl(INADDR_LOOPBACK);
    int where = post_payload(file, lo);
    if (where == 1 && lan_ip(&lan))
        where = post_payload(file, lan);
    if (where) {
        unlink(NAS_WAIT);
        snprintf(msg, cap, "%s", where == 1
                     ? TR("BinLoader is not listening on port 9090",
                          "BinLoader no está escuchando en el puerto 9090")
                     : TR("BinLoader turned the load down", "BinLoader rechazó la carga"));
        return 1;
    }
    snprintf(msg, cap, "%s", TR("BinLoader took the payload: loading it again",
                                "BinLoader aceptó el payload: cargándolo otra vez"));
    return 0;
}

/* 1 when the instance running asked for this one to replace it. */
static int nas_waiting(void) {
    char buf[32] = "";
    int fd = open(NAS_WAIT, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && atol(buf) == boot_time();
}

/* 1 when this process can open the fuse devices: the one started at boot cannot, and a
 * load that cannot either must not take the panel away from it. */
static int fuse_ok(void) {
    for (int i = 0; i < 8; i++) {
        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/fuse%d", i);
        int fd = open(dev, O_RDWR);
        if (fd >= 0) {
            close(fd);
            return 1;
        }
    }
    return 0;
}

/* 1 when the instance on the panel says it needs a load again (it runs inside GoldHEN's
 * process: this one, with a process of its own, can take the share). */
static int running_needs_reload(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static const char req[] = "GET /api/config HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    static const char want[] = "\"nas_reload\":true";
    static char buf[8192]; /* the stack of a payload can be small */
    ssize_t n = -1;
    int found = 0;
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a))) {
        if (!send_all(s, req, sizeof(req) - 1)) {
            n = recv(s, buf, sizeof(buf) - 1, 0);
            while (n > 0 && !found) {
                buf[n] = 0;
                found = strstr(buf, want) != NULL;
                if (found || n >= (ssize_t)sizeof(buf) - 1)
                    break;
                ssize_t more = recv(s, buf + n, sizeof(buf) - 1 - (size_t)n, 0);
                if (more <= 0)
                    break;
                n += more;
            }
        }
    }
    if (s >= 0)
        close(s);
    return found;
}

/* Asks the instance that asked for us to stop. */
static int stop_running(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    static const char req[] = "POST /api/stop HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
    int rc = -1;
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a)))
        rc = send_all(s, req, sizeof(req) - 1);
    if (s >= 0)
        close(s);
    return rc;
}

/* The one that was running asked to be replaced: it goes, this one stays. */
static int nas_takeover(int *lock) {
    if (!nas_waiting() && !running_needs_reload())
        return 0;
    if (!fuse_ok()) { /* loaded again inside the same process: nothing to gain */
        printf("[main] this load cannot use fusefs either: the running one stays\n");
        return 0;
    }
    if (stop_running()) {
        printf("[main] the running instance did not take the stop\n");
        return 0;
    }
    /* it may take a while to close: a Drive or download request has to end first */
    for (int i = 0; i < 240; i++) {
        usleep(250000);
        *lock = single_instance();
        if (*lock >= 0) {
            unlink(NAS_WAIT);
            printf("[main] replaced the instance of the boot\n");
            return 1;
        }
    }
    printf("[main] the running instance is still there\n");
    return 0;
}

/* Rest mode closes the network sockets of payloads: a listening socket that stopped
 * listening is found here so it can be opened again. */
static int still_listening(int s) {
    int on = 0;
    socklen_t n = sizeof(on);
    return s >= 0 && !getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &on, &n) && on;
}

static int g_live; /* klogsrv replays its backlog first: live after the first quiet 100 ms */

/* Rest mode would cut a Drive transfer or a download (and close the payload's sockets): while one runs,
 * the system is told every few seconds that the console is in use, as when a pad is
 * pressed. Once it ends the usual rest settings apply again. */
static void keep_awake(void) {
    static time_t last;
    static int on;
    int active = drive_active() || links_active();
    if (active != on) {
        on = active;
        printf("[power] %s\n", on ? "keeping the console out of rest mode while it transfers" : "rest mode allowed again");
    }
    if (on && time(NULL) - last >= 10) {
        last = time(NULL);
        sceSystemServicePowerTick();
    }
}

/* Serves the panel and handles klog lines for up to ms milliseconds. */
static void pump_events(int ms) {
    static char buf[16384], line[4096];
    static size_t ll;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += ms / 1000;
    end.tv_nsec += (long)(ms % 1000) * 1000000;
    for (;;) {
        if (g_klog < 0) {
            g_klog = klog_connect();
            g_live = 0;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = (end.tv_sec - now.tv_sec) * 1000 + (end.tv_nsec - now.tv_nsec) / 1000000;
        if (left <= 0)
            return;
        long wait = g_live || g_klog < 0 ? left : (left < 100 ? left : 100);
        struct timeval tv = {wait / 1000, (wait % 1000) * 1000};
        fd_set fs;
        FD_ZERO(&fs);
        int maxfd = -1;
        if (g_klog >= 0) {
            FD_SET(g_klog, &fs);
            maxfd = g_klog;
        }
        if (g_web >= 0 && !g_masked) { /* panel requests wait until all games show again */
            FD_SET(g_web, &fs);
            if (g_web > maxfd)
                maxfd = g_web;
        }
        if (maxfd < 0) {
            usleep((useconds_t)left * 1000);
            return;
        }
        int ready = select(maxfd + 1, &fs, NULL, NULL, &tv);
        if (ready == 0 && !g_live && g_klog >= 0) {
            g_live = 1;
            printf("[klog] backlog done, following live\n");
        }
        if (ready <= 0)
            continue;
        if (g_web >= 0 && FD_ISSET(g_web, &fs))
            web_serve(g_web);
        if (g_klog < 0 || !FD_ISSET(g_klog, &fs))
            continue;
        ssize_t n = read(g_klog, buf, sizeof(buf));
        if (n <= 0) {
            close(g_klog);
            g_klog = -1;
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (ll < sizeof(line) - 1)
                    line[ll++] = buf[i];
                continue;
            }
            line[ll] = 0;
            ll = 0;
            if (g_live)
                handle_klog_line(line);
        }
    }
}

/* Serves the panel and the klog for a moment, from inside a long scan: the daemon has no
 * thread of its own for them (catalog.h cat_pump and the scan loops call this). */
static void pump_now(void) { pump_events(1); }

/* The address of the panel as the console's notice shows it: with the console's own
 * address when it has one (no network yet: the placeholder of the README). */
static const char *panel_url(char *out, size_t n) {
    struct in_addr ip;
    char t[16];
    if (lan_ip(&ip) && inet_ntop(AF_INET, &ip, t, sizeof(t)))
        snprintf(out, n, "http://%s:%d", t, WEB_PORT);
    else
        snprintf(out, n, "%s", TR("http://<PS4 IP>:8770", "http://<IP de la PS4>:8770"));
    return out;
}

/* The start, as the console shows it. */
static void notify_running(void) {
    char url[48];
    if (g_setup && g_reg_mode != REG_NONE && g_web >= 0)
        notify(TR(VERSION ": first start. Choose the settings in the panel (%s) and press Save. Without it, the games "
                          "are added with the default settings in 2 minutes.",
                  VERSION ": primera vez. Elige los ajustes en el panel (%s) y pulsa Guardar. Si no, en 2 minutos se "
                          "añaden los juegos con los ajustes por defecto."),
               panel_url(url, sizeof(url)));
    else if (g_reg_mode == REG_ALL && g_web >= 0)
        notify(TR(VERSION " running. Panel at %s", VERSION " en marcha. Panel en %s"), panel_url(url, sizeof(url)));
    else if (g_reg_mode == REG_ALL)
        notify("%s", TR(VERSION " running (panel unavailable)", VERSION " en marcha (panel no disponible)"));
    else if (g_reg_mode == REG_TITLEDIR)
        notify(TR(VERSION " running (firmware %s: games are added one at a time). Panel at %s",
                  VERSION " en marcha (firmware %s: los juegos se añaden de uno en uno). Panel en %s"),
               g_fw_text, panel_url(url, sizeof(url)));
    else
        notify(TR(VERSION ": firmware %s is not supported yet. To protect the system database, no games are added.",
                  VERSION ": el firmware %s aún no es compatible. Para proteger la base de datos del sistema, no se "
                          "añaden juegos."),
               g_fw_text);
}

int main(void) {
    mkdirs(STATE_DIR, 0777);
    /* the log goes to a file, every line with its time; when it cannot be opened, stdout
     * stays as it was. Opened first of all: a load that ends before the panel is up is
     * only seen here */
    log_start(STATE_DIR "/log.txt");
    setvbuf(stdout, NULL, _IOLBF, 0);
    int lock = single_instance();
    if (lock < 0 && !nas_takeover(&lock)) {
        apply_lang(); /* the settings are the running one's: the console's language */
        printf("[main] already running: this load ends here\n");
        char url[48];
        notify(TR(VERSION " is already running. Panel at %s", VERSION " ya está en marcha. Panel en %s"),
               panel_url(url, sizeof(url)));
        log_stop();
        return 0;
    }
    sceUserServiceInitialize(0);
    sceAppInstUtilInitialize();
    /* the PS4 loader runs the payload in a process of its own and returning from main
     * goes back to it: no exit, no atexit. What the payload opened is closed by
     * shutdown() before main returns */
    printf("[main] start (lock %d, pid %d)\n", lock, (int)getpid());
    cat_pump = pump_now; /* the panel answers during a walk of a share */
    g_setup = first_start();
    g_setup_start = time(NULL);
    config_load();
    apply_lang();
    if (g_setup)
        printf("[main] first start: no game is added until the settings are saved\n");
    signal(SIGPIPE, SIG_IGN);
    check_firmware();
    nas_apply(1); /* the share, when the settings have one */
    g_quiet = nas_need_reload(); /* a load of its own takes over in a few seconds */
    if (user_app_masked())
        unmask_leftover();
    /* no Media tab shortcut: it is a PS5 web app (param.json); the panel is opened from
     * a browser */
    g_web = web_listen();
    drive_begin();
    links_begin();
    notify_running();
    unlink(STATE_DIR "/stop");
    for (;;) {
        if (g_stop || exists(STATE_DIR "/stop")) {
            unlink(STATE_DIR "/stop");
            notify("%s", TR(VERSION " stopped", VERSION " detenido"));
            shutdown_daemon(lock);
            return 0;
        }
        if (!still_listening(lock)) {
            /* lost in rest mode: take it again, or stop if another PS4Mount has it now */
            close(lock);
            if ((lock = single_instance()) < 0) {
                notify("%s", TR(VERSION " stopped: another PS4Mount is running", VERSION " detenido: hay otro PS4Mount en marcha"));
                shutdown_daemon(-1);
                return 0;
            }
            printf("[main] instance lock opened again\n");
        }
        if (!still_listening(g_web)) {
            int was = g_web >= 0;
            if (g_web >= 0)
                close(g_web);
            g_web = web_listen();
            if (was || g_web >= 0) /* log changes only, not every retry */
                printf("[main] web panel %s\n", g_web >= 0 ? "listening again" : "unavailable");
        }
        if (g_deleting && g_deleting_close_at && time(NULL) >= g_deleting_close_at)
            end_delete_window();
        if (g_deleting && time(NULL) - g_deleting_since > 600)
            end_delete_window(); /* the line of the screen change was missed */
        if (g_restore_n || g_restore_all) {
            run_restore(g_restore_all ? NULL : g_restore_tids, g_restore_n, 1);
            g_restore_n = 0;
            g_restore_all = 0;
            continue;
        }
        if (g_delete_tid[0] && !g_deleting) {
            char tid[10], msg[256];
            int kept = g_delete_kept;
            snprintf(tid, sizeof(tid), "%s", g_delete_tid);
            g_delete_tid[0] = 0;
            g_delete_kept = 0;
            pump_events(2000); /* let the system close its error dialog */
            /* the files went when the delete started: finish taking it off the console */
            if (app_remove(tid, msg, sizeof(msg)))
                notify("%s", msg);
            if (!kept) { /* a pkg used where it is stayed: keep the game out of the library */
                char rm[64];
                snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
                unlink(rm);
            }
            continue;
        }
        if (g_readd_tid[0] && !g_deleting) {
            char tid[10];
            snprintf(tid, sizeof(tid), "%s", g_readd_tid);
            g_readd_tid[0] = 0;
            readd_title(tid);
            continue;
        }
        nas_apply(0); /* retries a share that was not reached, or the settings changed */
        nas_ask_reload(); /* the boot instance cannot open the fuse devices: ask for a load */
        install_received(); /* an app sent with "Send and install" */
        panel_app_step();
        if (time(NULL) >= g_next_scan && !g_deleting) {
            if (g_setup && time(NULL) >= (g_setup_seen > g_setup_start ? g_setup_seen : g_setup_start) + SETUP_WAIT) {
                printf("[main] first start: nobody at the panel, the default settings are saved\n");
                config_save();
            }
            if (g_setup) {
                if (g_manual_scan)
                    notify("%s", TR("Save the settings in the panel first: no game is added until then",
                                    "Guarda primero los ajustes en el panel: hasta entonces no se añade ningún juego"));
            } else if (g_reg_mode != REG_NONE) {
                migrate_organized(); /* organize off: the games it had organized, back in place */
                scan_all();
                /* After a boot the games are mounted by this first scan: one started before
                 * says its data is corrupted. Saying when they are there tells when to start. */
                static int ready_said;
                if (!ready_said) {
                    ready_said = 1;
                    int playable = 0;
                    for (int i = 0; i < MAX_TITLES; i++)
                        playable += g_managed[i].used && !needs_reboot(g_managed[i].tid);
                    if (playable)
                        notify(TR("%d game(s) ready to play", "%d juego(s) listo(s) para jugar"), playable);
                }
            } else if (g_manual_scan)
                notify(TR("Firmware %s is not supported yet: no games are added",
                          "El firmware %s aún no es compatible: no se añaden juegos"),
                       g_fw_text);
            g_manual_scan = 0;
            g_next_scan = time(NULL) + SCAN_SECONDS;
            install_cleanup();
            dlc_pending_check();
        }
        keep_awake();
        pump_events(1000);
    }
}
