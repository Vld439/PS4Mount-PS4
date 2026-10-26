/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The FUSE side of the NAS support: the kernel patch that enables fusefs on
 * 13.52, the mount (one per process: this kernel refuses to unmount a FUSE
 * filesystem from its own jail) and the read-only operations that forward to
 * whichever client is connected. */
#define FUSE_USE_VERSION 26

#include "netfs.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <ps4/kernel.h>

#include "lang.h"
#include "mem.h"

#define STATE_DIR "/data/ps4mount"
/* Inside the USB drive, not under /data or /mnt: the system installer only reads a pkg
 * from a storage it has mounted (the same reason USB drives work there). */
#define NAS_PARENT "/mnt/usb0/ps4mount"
#define NAS_TRIES 8

/* 13.52 offsets (the only firmware with the fusefs module compiled in). */
#define KBASE_OFF 0x1C0
#define KSUSER 0x21AD2AC
#define VFSCONF 0x1A7FA48
#define FUSE_LOADER 0x495C10

static struct fuse *g_fuse;
static char g_mnt[PATH_MAX];
static char g_status[192] = "off";
static int g_mounted, g_active, g_loop;
/* The boot context cannot open the fuse devices (ENXIO): the payload has to be loaded
 * again by the console's own loader, in a process of its own. */
static int g_reload;
static int g_probe_said;   /* the device errors are logged once */
static const nas_be_t *g_be;
static unsigned g_gen; /* changes with every client: old file handles are dead */
static unsigned g_ops; /* requests served: the first ones are logged, to see where a share stops */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Reading ahead: the console's Wi-Fi takes 70 ms or more for a round trip, and a read
 * waited for alone moves 128 KB in it. A window is asked in pieces all at once; while
 * the game reads one, the next is already on its way (a read that goes on doubles the
 * window, up to RA_MAX; a jump starts small again). */
#define RA_MAX (4u << 20)
#define RA_FIRST (256u << 10)
#define RA_PIECE (256u << 10)
#define RA_MIN_PIECE (64u << 10) /* below this the window is cut short */
#define RA_WAIT_MS 60000         /* the client's own timeout ends a lost piece well before this */

typedef struct {
    char *buf;   /* RA_MAX mapped bytes, NULL until needed */
    off_t off;   /* where buf[0] is in the file */
    size_t len;  /* bytes asked */
    size_t got;  /* bytes there once it came (short at the end of the file) */
    int n;       /* pieces */
    int asked;   /* sent and not waited for yet */
    nas_piece_t p[RA_MAX / RA_MIN_PIECE];
} ra_win_t;

typedef struct {
    unsigned gen;
    void *h;
    char path[PATH_MAX]; /* to open it again on a new connection */
    ra_win_t w[2];       /* w[cur] is read from; the other is the next one on its way */
    int cur;
    int lost;            /* a piece never came back: its memory may still be written */
    off_t next;          /* where a read that goes on starts */
    size_t win;          /* the last window */
    unsigned long long fetched, waits; /* for the log at close */
    long long ms;
} nas_fh_t;

/* ---- enabling fusefs on 13.52 (same patch as itemzflow) ---- */

__attribute__((no_stack_protector)) static uint64_t kbase(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000082));
    return (((uint64_t)hi << 32) | lo) - KBASE_OFF;
}

__attribute__((no_stack_protector)) static void wp_write(int enable) {
    uint64_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    if (enable)
        cr0 |= (1ull << 16);
    else
        cr0 &= ~(1ull << 16);
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

__attribute__((no_stack_protector)) static int fuse_patch(void *td) {
    (void)td;
    uint8_t *k = (uint8_t *)kbase();
    int *ksuser = (int *)(k + KSUSER);
    if (*ksuser == 1)
        return 1; /* already done (another payload, or a previous start) */

    wp_write(0);
    struct vfsconf *p = (struct vfsconf *)(k + VFSCONF);
    *ksuser = 1;
    p->vfc_flags = 0x00400000 | 0x00080000; /* VFCF_JAIL | VFCF_SYNTHETIC */

    k[0x317FE6] = 0;
    k[0x491C65] = 0x84;
    k[0x495C3E] = 0xEB;
    k[0x495C3F] = 0x1B;
    k[0x490674] = 0x0;
    k[0x490675] = 0xEB;
    k[0x490772] = 0xEB;
    k[0x490CC2] = 0x84;
    k[0x494C27] = 0x85;
    k[0x49537A] = 0x84;
    k[0x49572D] = 0xEB;
    k[0x49572E] = 0x04;
    k[0x49057E] = 0xB6;

    wp_write(1);

    int (*loader)(void *, int, void *) = (void *)(k + FUSE_LOADER);
    return loader(NULL, 0, NULL);
}

/* A mount is there, without touching the filesystem: asking a FUSE mount whose
 * owner is gone blocks until the kernel gives up. */
static int mounted_at(const char *path) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return 0;
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 8));
    if (!list)
        return 0;
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 8)), MNT_NOWAIT);
    int found = 0;
    for (int i = 0; i < n && !found; i++)
        found = !strcmp(list[i].f_mntonname, path);
    free(list);
    return found;
}

/* The name is kept between boots: the game tasks the system holds name the pkg by its
 * path, and a share that moves would have to be registered again. A mount cannot be
 * removed, so a second start in the same boot finds the saved name taken and takes a new
 * one (the games keep working, but the console has to restart to register them again). */
static int saved_name(char *out, size_t n) {
    char p[64], buf[64] = "";
    snprintf(p, sizeof(p), STATE_DIR "/nas-mnt");
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0)
        return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    if (!buf[0] || strchr(buf, '/') || strstr(buf, ".."))
        return -1;
    snprintf(out, n, "%s", buf);
    return 0;
}

static void save_name(const char *name) {
    char p[64], buf[64];
    snprintf(p, sizeof(p), STATE_DIR "/nas-mnt");
    int n = snprintf(buf, sizeof(buf), "%s\n", name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        if (write(fd, buf, (size_t)n) < 0)
            ;
        close(fd);
    }
}

static int pick_mountpoint(char *out, size_t n) {
    mkdir(STATE_DIR, 0777);
    mkdir(NAS_PARENT, 0777);
    char name[48];
    if (!saved_name(name, sizeof(name))) {
        snprintf(out, n, "%s/%s", NAS_PARENT, name);
        if (!mounted_at(out) && (!mkdir(out, 0777) || errno == EEXIST))
            return 0;
    }
    for (int i = 1; i <= NAS_TRIES; i++) {
        snprintf(name, sizeof(name), "nas-%d", i);
        snprintf(out, n, "%s/%s", NAS_PARENT, name);
        if (mounted_at(out))
            continue;
        if (!mkdir(out, 0777) || errno == EEXIST) {
            save_name(name);
            return 0;
        }
        return -1;
    }
    snprintf(out, n, "%s", "");
    return -1;
}

/* Logs the first requests and their result: a share that stops answering is found here. */
static void op(const char *what, const char *path, int r) {
    if (g_ops++ < 400 || (g_ops % 100) == 0 || r < 0)
        printf("[nas] %s %s -> %d\n", what, path, r);
}

/* ---- a link that broke ---- */

/* The console moved to another network (Wi-Fi to cable) or the server restarted: the
 * client's connection is gone and every request fails. The client is connected again
 * with the last settings (at most every REDIAL_SECONDS) and the files open are opened
 * again, so a game reading the share goes on. All of it with g_lock held. */
#define REDIAL_SECONDS 5
#define OUTAGE_MS 120000 /* a read waits this long for a server that went away */

static nas_cfg_t g_last; /* the settings of the client connected last */
static int g_have_last;
static time_t g_redial_at;

static int connect_any(const nas_cfg_t *cfg, char *err, size_t n);
static void status_set(const nas_cfg_t *cfg, const char *state);

static int link_error(int r) {
    return r == -ETIMEDOUT || r == -ENOTCONN || r == -EPIPE || r == -ECONNRESET || r == -ECONNABORTED ||
           r == -ENETDOWN || r == -ENETUNREACH || r == -EHOSTUNREACH || r == -EFAULT || r == -EIO;
}

/* 1 when a client is connected again after r. */
static int redial(int r) {
    if (!g_mounted || !g_have_last || !link_error(r))
        return 0;
    time_t now = time(NULL);
    if (now < g_redial_at)
        return 0;
    g_redial_at = now + REDIAL_SECONDS;
    printf("[nas] the link failed (%d): connecting again\n", r);
    g_active = 0;
    if (g_be) {
        g_be->disconnect();
        g_be = NULL;
    }
    g_gen++;
    char err[192];
    int ok = !connect_any(&g_last, err, sizeof(err));
    status_set(&g_last, ok ? TR("ready", "listo") : err);
    printf("[nas] connected again: %s\n", ok ? "yes" : err);
    return ok;
}

/* A client to send a request to: the one connected, or a new one when the link broke. */
static int ready(void) { return (g_active && g_be) || redial(-ENOTCONN); }

/* The file's handle on the client connected now. */
static int fh_live(nas_fh_t *f) {
    if (f->gen == g_gen)
        return 1;
    void *h = g_be->open(f->path);
    if (!h)
        return 0;
    f->h = h; /* the old one died with its client, and its pieces with it */
    f->gen = g_gen;
    for (int i = 0; i < 2; i++)
        f->w[i].asked = 0, f->w[i].got = 0;
    f->lost = 0;
    return 1;
}

/* ---- the read-only filesystem ---- */

static int fs_getattr(const char *path, struct stat *st) {
    pthread_mutex_lock(&g_lock);
    if (!ready()) {
        pthread_mutex_unlock(&g_lock);
        return -ENOTCONN;
    }
    memset(st, 0, sizeof(*st));
    int r = g_be->stat(path, st);
    if (r < 0 && redial(r))
        r = g_be->stat(path, st);
    op("getattr", path, r);
    if (!r) {
        st->st_uid = 0;
        st->st_gid = 0;
        if (!st->st_nlink)
            st->st_nlink = S_ISDIR(st->st_mode) ? 2 : 1;
        /* the kernel reads the pkg through the buffer cache, in blocks this big: with
         * the default 4 KB a game install crawls over a share */
        st->st_blksize = 128 * 1024;
        st->st_blocks = (st->st_size + 511) / 512;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off,
                      struct fuse_file_info *fi) {
    (void)off;
    (void)fi;
    pthread_mutex_lock(&g_lock);
    int r = ready() ? g_be->list(path, buf, fill) : -ENOTCONN;
    if (r < 0 && redial(r))
        r = g_be->list(path, buf, fill);
    op("readdir", path, r);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static int fs_open(const char *path, struct fuse_file_info *fi) {
    /* Not refused: BGFT opens a game's pkg for writing when it starts it, and an EROFS
     * there is recorded in the game task and the game is refused. There is no write
     * operation: a write still fails, as it should in a read-only share. */
    if ((fi->flags & O_ACCMODE) != O_RDONLY)
        printf("[nas] open for writing: %s (0x%x)\n", path, fi->flags);
    nas_fh_t *f = calloc(1, sizeof(*f));
    if (!f)
        return -ENOMEM;
    snprintf(f->path, sizeof(f->path), "%s", path);
    pthread_mutex_lock(&g_lock);
    if (!ready()) {
        pthread_mutex_unlock(&g_lock);
        free(f);
        return -ENOTCONN;
    }
    f->h = g_be->open(path);
    if (!f->h && redial(-(errno ? errno : EIO)))
        f->h = g_be->open(path);
    op("open", path, f->h ? 0 : -(errno ? errno : EIO));
    if (!f->h) {
        int e = errno ? errno : EIO;
        pthread_mutex_unlock(&g_lock);
        free(f);
        return -e;
    }
    f->gen = g_gen;
    pthread_mutex_unlock(&g_lock);
    fi->fh = (uint64_t)(uintptr_t)f;
    fi->keep_cache = 1;
    return 0;
}

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static size_t win_size(size_t win, size_t size) {
    if (win < size)
        win = size;
    return win > RA_MAX ? RA_MAX : win;
}

/* Sends w for len bytes from off, without waiting. 0, or -ENOMEM. */
static int win_ask(nas_fh_t *f, ra_win_t *w, off_t off, size_t len) {
    if (!w->buf && !(w->buf = mem_alloc(RA_MAX)))
        return -ENOMEM;
    size_t piece = g_be->piece_max(), at = 0;
    if (!piece || piece > RA_PIECE)
        piece = RA_PIECE;
    w->n = 0;
    for (; at < len && w->n < (int)(sizeof(w->p) / sizeof(w->p[0])); at += piece, w->n++)
        w->p[w->n] = (nas_piece_t){w->buf + at, len - at < piece ? len - at : piece, off + (off_t)at, 0, 0};
    w->off = off;
    w->len = at < len ? at : len;
    w->got = 0;
    w->asked = 1;
    g_be->submit(f->h, w->p, w->n);
    return 0;
}

static int win_pending(const ra_win_t *w) {
    for (int i = 0; i < w->n; i++)
        if (!w->p[i].done)
            return 1;
    return 0;
}

/* Waits for w to come. 0 with w->got set, or -errno (the first piece failed). */
static int win_wait(nas_fh_t *f, ra_win_t *w) {
    long long t0 = now_ms();
    while (win_pending(w)) {
        if (now_ms() - t0 >= RA_WAIT_MS) {
            printf("[nas] read ahead @%lld: no reply\n", (long long)w->off);
            f->lost = 1;
            return -ETIMEDOUT;
        }
        if (g_be->pump(1000) < 0 && win_pending(w)) {
            f->lost = 1; /* until the client is dropped */
            return -EIO;
        }
    }
    f->ms += now_ms() - t0;
    f->waits++;
    w->asked = 0;
    size_t got = 0;
    for (int i = 0; i < w->n; i++) {
        if (w->p[i].r < 0) {
            if (!got)
                return w->p[i].r;
            break;
        }
        got += (size_t)w->p[i].r;
        if ((size_t)w->p[i].r < w->p[i].size)
            break; /* the end of the file */
    }
    w->got = got;
    f->fetched += got;
    return 0;
}

/* 1 when w has what a read from off needs (all of it, or up to the end of the file). */
static int win_has(const ra_win_t *w, off_t off, size_t size) {
    return !w->asked && w->got && off >= w->off && off < w->off + (off_t)w->got &&
           (off + (off_t)size <= w->off + (off_t)w->got || w->got < w->len);
}

/* A read through the windows: bytes copied, or -errno. */
static int ra_read(nas_fh_t *f, char *buf, size_t size, off_t off) {
    ra_win_t *c = &f->w[f->cur], *o = &f->w[!f->cur];
    /* goes on: from where the last read ended, or anywhere in the windows (the kernel
     * does not always ask for the blocks in order) */
    int goes_on = off == f->next || (c->got && off >= c->off && off <= c->off + (off_t)c->len) ||
                  ((o->asked || o->got) && off >= o->off && off <= o->off + (off_t)o->len);
    if (f->lost)
        return -EIO; /* its windows are in use until the client is connected again */
    if (o->asked)
        g_be->pump(0); /* the next window moves along while the game reads */
    if (!win_has(c, off, size)) {
        int r;
        if (o->asked && off >= o->off && off < o->off + (off_t)o->len) {
            if ((r = win_wait(f, o)))
                return r;
            f->cur = !f->cur;
            c = o;
            o = &f->w[!f->cur];
        } else {
            size_t win = win_size(goes_on && f->win ? f->win * 2 : RA_FIRST, size);
            if ((r = win_ask(f, c, off, win)) || (r = win_wait(f, c)))
                return r;
            f->win = win;
        }
        if (!win_has(c, off, size))
            return off >= c->off + (off_t)c->got ? 0 : -EIO; /* past the end of the file */
    }
    size_t at = (size_t)(off - c->off), n = c->got - at < size ? c->got - at : size;
    memcpy(buf, c->buf + at, n);
    /* the next window, while this one is read: only for reads that go on */
    if (goes_on && !o->asked && c->got == c->len) {
        size_t win = win_size(f->win * 2, 0);
        if (!win_ask(f, o, c->off + (off_t)c->len, win))
            f->win = win;
    }
    return (int)n;
}

static int fs_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    nas_fh_t *f = (nas_fh_t *)(uintptr_t)fi->fh;
    pthread_mutex_lock(&g_lock);
    int r;
    if (!ready() || !fh_live(f)) {
        r = -ENOTCONN;
    } else if (size > RA_MAX || (r = ra_read(f, buf, size, off)) == -ENOMEM) {
        r = g_be->read(f->h, buf, size, off); /* too big, or no memory for it: as it comes */
    }
    if (r < 0 && redial(r) && fh_live(f))
        r = ra_read(f, buf, size, off);
    /* The link broke under a game (a cable pulled, the server restarted): the read waits,
     * and the game with it, until the server is back. An error would be a hole in its
     * data that it never gets over (it falls through the floor of a map it could not
     * load); a pause it does. */
    if (r < 0 && link_error(r) && g_have_last) {
        printf("[nas] read %s @%lld -> %d: held until the server is back\n", path, (long long)off, r);
        for (long long until = now_ms() + OUTAGE_MS; r < 0 && link_error(r) && g_have_last && now_ms() < until;) {
            pthread_mutex_unlock(&g_lock);
            usleep(1000000);
            pthread_mutex_lock(&g_lock);
            g_redial_at = 0; /* waited: try again now */
            if (redial(r) && fh_live(f))
                r = size > RA_MAX ? g_be->read(f->h, buf, size, off) : ra_read(f, buf, size, off);
        }
    }
    if (r > 0)
        f->next = off + r;
    if (r < 0)
        printf("[nas] read %s @%lld -> %d\n", path, (long long)off, r);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static int fs_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    nas_fh_t *f = (nas_fh_t *)(uintptr_t)fi->fh;
    pthread_mutex_lock(&g_lock);
    if (f->gen == g_gen && g_be) {
        for (int i = 0; i < 2; i++) /* a window on its way writes into f until it comes */
            if (f->w[i].asked && win_wait(f, &f->w[i]))
                break;
        g_be->close(f->h); /* a handle of a client already gone is dead: just drop it */
    }
    pthread_mutex_unlock(&g_lock);
    if (f->fetched)
        printf("[nas] %s: %llu MB read in %llu go(es), %lld ms waiting (%lld KB/s)\n", path, f->fetched >> 20, f->waits,
               f->ms, f->ms ? (long long)(f->fetched / 1024 * 1000 / (unsigned long long)f->ms) : 0);
    if (f->lost && f->gen == g_gen)
        return 0; /* pieces may still land in it: left as it is */
    for (int i = 0; i < 2; i++)
        mem_free(f->w[i].buf);
    free(f);
    return 0;
}

static int fs_statfs(const char *path, struct statvfs *st) {
    (void)path;
    memset(st, 0, sizeof(*st));
    pthread_mutex_lock(&g_lock);
    int r = ready() ? g_be->statfs(st) : -ENOTCONN;
    if (r < 0 && redial(r))
        r = g_be->statfs(st);
    op("statfs", path, r);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static int fs_flush(const char *path, struct fuse_file_info *fi) {
    (void)path;
    (void)fi;
    return 0;
}

static int fs_access(const char *path, int mode) {
    struct stat st;
    (void)mode;
    pthread_mutex_lock(&g_lock);
    int r = ready() ? g_be->stat(path, &st) : -ENOTCONN;
    if (r < 0 && redial(r))
        r = g_be->stat(path, &st);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void *fs_init(struct fuse_conn_info *ci) {
    (void)ci;
    printf("[nas] %s: mounted, the kernel is talking to it\n", g_mnt);
    return NULL;
}

static void fs_destroy(void *p) {
    (void)p;
    printf("[nas] %s: the kernel closed the mount\n", g_mnt);
}

static struct fuse_operations fs_ops = {
    .init = fs_init,
    .destroy = fs_destroy,
    .getattr = fs_getattr,
    .readdir = fs_readdir,
    .open = fs_open,
    .read = fs_read,
    .release = fs_release,
    .statfs = fs_statfs,
    .flush = fs_flush,
    .access = fs_access,
};

static void *fuse_thread(void *arg) {
    (void)arg;
    printf("[nas] %s: serving\n", g_mnt);
    int r = fuse_loop(g_fuse);
    printf("[nas] fuse_loop returned %d\n", r);
    pthread_mutex_lock(&g_lock);
    g_active = 0;
    g_loop = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* ---- connecting and swapping the client ---- */

static const nas_be_t *be_for(int proto) { return proto == NAS_SMB ? &nas_be_smb : &nas_be_nfs; }

#define PROBE_MS 3000

/* 0 when the server takes a connection on the protocol's port within PROBE_MS. A server
 * that drops it (a firewall, an address nobody has) otherwise keeps a client waiting for
 * the system's TCP timeout, over a minute per protocol version, with everything stopped. */
static int probe(const nas_cfg_t *cfg) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg->port ? cfg->port : cfg->proto == NAS_SMB ? 445 : 2049);
    if (inet_pton(AF_INET, cfg->server, &a.sin_addr) != 1)
        return 0; /* a name: the client resolves it */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return 0;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&a, sizeof(a));
    if (r && errno == EINPROGRESS) {
        struct pollfd p = {s, POLLOUT, 0};
        int e = 0;
        socklen_t el = sizeof(e);
        r = poll(&p, 1, PROBE_MS) == 1 && !getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) && !e ? 0 : -1;
    }
    close(s);
    return r;
}

static int connect_any(const nas_cfg_t *cfg, char *err, size_t n) {
    const nas_be_t *be = be_for(cfg->proto);
    if (probe(cfg)) {
        snprintf(err, n, "%s", TR("the server does not answer (check its address, that it is on, and its firewall)",
                                  "el servidor no contesta (revisa su dirección, que esté encendido y su cortafuegos)"));
        return -1;
    }
    if (be->connect(cfg)) {
        snprintf(err, n, "%s", be->error());
        return -1;
    }
    g_be = be;
    g_active = 1;
    g_gen++;
    if (cfg != &g_last)
        g_last = *cfg;
    g_have_last = 1;
    return 0;
}

static void status_set(const nas_cfg_t *cfg, const char *state) {
    if (cfg->proto == NAS_SMB)
        snprintf(g_status, sizeof(g_status), "SMB //%s/%s%s%s: %s", cfg->server, cfg->share,
                 cfg->sub[0] ? "/" : "", cfg->sub, state);
    else
        snprintf(g_status, sizeof(g_status), "NFS %s:%s%s%s: %s", cfg->server, cfg->share,
                 cfg->sub[0] ? "/" : "", cfg->sub, state);
}

int nas_mounted(void) { return g_mounted; }

const char *nas_mountpoint(void) { return g_mnt; }

int nas_active(void) { return g_mounted && g_active && g_be; }

const char *nas_status(void) { return g_status; }

int nas_need_reload(void) { return g_reload; }

int nas_start(const nas_cfg_t *cfg, char *err, size_t n) {
    pthread_mutex_lock(&g_lock);
    if (g_mounted) {
        /* The mount is not going away; only its client changes. */
        g_active = 0;
        if (g_be) {
            g_be->disconnect();
            g_be = NULL;
        }
        g_gen++;
        int r = connect_any(cfg, err, n);
        status_set(cfg, r ? err : TR("ready", "listo"));
        pthread_mutex_unlock(&g_lock);
        return r;
    }

    snprintf(g_status, sizeof(g_status), "%s", TR("starting", "arrancando"));
    if (access("/dev/fuse0", F_OK)) {
        errno = 0;
        long pr = syscall(11, fuse_patch);
        printf("[nas] fusefs patch: %ld (errno %d)\n", pr, errno);
        if (access("/dev/fuse0", F_OK)) {
            snprintf(err, n, "%s", TR("the kernel has no fusefs (patch failed)", "el kernel no tiene fusefs (falló el parche)"));
            status_set(cfg, err);
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
    }

    /* Every device, as the mount does: a payload started by GoldHEN's AutoRun runs inside
     * GoldHEN's own process, where they all answer ENXIO (6). One loaded by BinLoader has
     * a process of its own and can; main asks for that load when this flag is set. */
    {
        int ok = 0, enxio = 1;
        for (int i = 0; i < 8 && !ok; i++) {
            char dev[32];
            snprintf(dev, sizeof(dev), "/dev/fuse%d", i);
            int fd = open(dev, O_RDWR);
            if (fd >= 0) {
                close(fd);
                ok = 1;
                break;
            }
            if (!g_probe_said)
                printf("[nas] open %s: %d\n", dev, errno);
            if (errno != ENXIO)
                enxio = 0;
        }
        g_probe_said = 1;
        if (!ok) {
            g_reload = enxio;
            snprintf(err, n, "%s", g_reload ? TR("this start cannot use fusefs: the payload has to be loaded again", "este arranque no puede usar fusefs: hay que volver a cargar el payload")
                                            : TR("the fuse devices cannot be opened", "no se pueden abrir los dispositivos fuse"));
            status_set(cfg, err);
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
    }

    char mnt[PATH_MAX];
    if (pick_mountpoint(mnt, sizeof(mnt))) {
        snprintf(err, n, "%s", TR("the share is mounted by an earlier start of this boot: restart the console", "el recurso lo montó un arranque anterior de esta sesión: reinicia la consola"));
        status_set(cfg, err);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    /* Connect before mounting: a wrong address must not use up a fuse device. */
    if (connect_any(cfg, err, n)) {
        status_set(cfg, err);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_active = 0; /* the loop has not started yet */

    char *argv[] = {"ps4mount", mnt, "-o", "allow_other", "-o", "rw", "-f", NULL};
    char *mountpoint = NULL;
    int mt = 0;
    g_fuse = fuse_setup(6, argv, &fs_ops, sizeof(fs_ops), &mountpoint, &mt, NULL);
    if (!g_fuse) {
        g_be->disconnect();
        g_be = NULL;
        /* GoldHEN's process can open a device once and then no more: the mount found every
         * one closed (ENXIO), which needs the load of its own as well */
        int enxio = 1;
        for (int i = 0; i < 8 && enxio; i++) {
            char dev[32];
            snprintf(dev, sizeof(dev), "/dev/fuse%d", i);
            int fd = open(dev, O_RDWR);
            if (fd >= 0)
                close(fd);
            enxio = fd < 0 && errno == ENXIO;
        }
        if (enxio) {
            g_reload = 1;
            snprintf(err, n, "%s", TR("this start cannot use fusefs: the payload has to be loaded again", "este arranque no puede usar fusefs: hay que volver a cargar el payload"));
            status_set(cfg, err);
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
        snprintf(err, n, "%s", TR("the kernel refused the mount (see the log)", "el kernel rechazó el montaje (mira el log)"));
        status_set(cfg, err);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    snprintf(g_mnt, sizeof(g_mnt), "%s", mnt);
    g_mounted = 1;
    g_active = 1;
    status_set(cfg, TR("ready", "listo"));
    g_loop = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, fuse_thread, NULL)) {
        g_loop = 0;
        snprintf(err, n, "%s", TR("could not start the FUSE thread", "no se pudo arrancar el hilo de FUSE"));
        status_set(cfg, err);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    pthread_detach(th);
    printf("[nas] mounted on %s\n", g_mnt);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int nas_reconnect(const nas_cfg_t *cfg, char *err, size_t n) {
    pthread_mutex_lock(&g_lock);
    g_active = 0;
    if (g_be) {
        g_be->disconnect();
        g_be = NULL;
    }
    g_gen++;
    int r = connect_any(cfg, err, n);
    status_set(cfg, r ? err : TR("ready", "listo"));
    pthread_mutex_unlock(&g_lock);
    return r;
}

void nas_stop(void) {
    pthread_mutex_lock(&g_lock);
    g_have_last = 0; /* off on purpose: not connected again */
    g_active = 0;
    if (g_be) {
        g_be->disconnect();
        g_be = NULL;
    }
    g_gen++;
    snprintf(g_status, sizeof(g_status), "%s", TR("off", "apagado"));
    pthread_mutex_unlock(&g_lock);
}

/* ---- finding servers ---- */

#define SCAN_BATCH 127 /* hosts tried at once (two sockets each) */
#define SCAN_MS 1500   /* a host that has not answered by then is not there */
#define NAME_MS 700

/* The console's address on its network: the one a packet to the internet would leave from. */
static int own_addr(struct in_addr *out) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0}, me;
    socklen_t n = sizeof(me);
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    a.sin_addr.s_addr = htonl(0x08080808);
    int ok = s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a)) && !getsockname(s, (struct sockaddr *)&me, &n);
    if (s >= 0)
        close(s);
    if (ok)
        *out = me.sin_addr;
    return ok;
}

static nas_host_t *host_of(nas_host_t *out, int *found, int max, uint32_t ip) {
    char t[16];
    struct in_addr a = {htonl(ip)};
    inet_ntop(AF_INET, &a, t, sizeof(t));
    for (int i = 0; i < *found; i++)
        if (!strcmp(out[i].ip, t))
            return &out[i];
    if (*found == max)
        return NULL;
    nas_host_t *h = &out[(*found)++];
    memset(h, 0, sizeof(*h));
    snprintf(h->ip, sizeof(h->ip), "%s", t);
    return h;
}

/* NetBIOS node status ("*"): Windows, Samba and most NAS boxes answer with their name. */
static void name_hosts(nas_host_t *out, int found) {
    static const uint8_t ask[] = {0x50, 0x53, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x20, 'C', 'K',
                                  'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A',
                                  'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A',
                                  'A', 'A', 0, 0, 0x21, 0, 1};
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;
    for (int i = 0; i < found; i++) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(137);
        if (inet_pton(AF_INET, out[i].ip, &a.sin_addr) == 1)
            sendto(s, ask, sizeof(ask), 0, (struct sockaddr *)&a, sizeof(a));
    }
    long long end = now_ms() + NAME_MS;
    for (long long left; (left = end - now_ms()) > 0;) {
        struct pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, (int)left) != 1)
            break;
        uint8_t r[1024];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t len = recvfrom(s, r, sizeof(r), 0, (struct sockaddr *)&from, &fl);
        if (len < 57)
            continue;
        size_t o = 12; /* the name asked, then type, class, ttl and length */
        if (r[o] >= 0xC0)
            o += 2;
        else
            while (o < (size_t)len && r[o])
                o += r[o] + 1;
        o += 1 + 10;
        if (o >= (size_t)len)
            continue;
        int names = r[o++];
        char ip[16];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        for (int k = 0; k < names && o + 18 <= (size_t)len; k++, o += 18) {
            /* the machine's own name: suffix 0x00, not a group name */
            if (r[o + 15] != 0 || (r[o + 16] & 0x80))
                continue;
            char name[16];
            memcpy(name, r + o, 15);
            name[15] = 0;
            for (int e = 14; e >= 0 && name[e] == ' '; e--)
                name[e] = 0;
            for (int i = 0; i < found; i++)
                if (!strcmp(out[i].ip, ip) && !out[i].name[0])
                    snprintf(out[i].name, sizeof(out[i].name), "%s", name);
            break;
        }
    }
    close(s);
}

int nas_scan(nas_host_t *out, int max) {
    struct in_addr me;
    if (!own_addr(&me))
        return 0;
    uint32_t self = ntohl(me.s_addr), net = self & 0xFFFFFF00u;
    static const int ports[2] = {445, 2049};
    int found = 0;
    for (int first = 1; first < 255; first += SCAN_BATCH) {
        struct pollfd p[SCAN_BATCH * 2];
        uint32_t ip[SCAN_BATCH * 2];
        int which[SCAN_BATCH * 2], np = 0;
        for (int h = first; h < first + SCAN_BATCH && h < 255; h++) {
            if (net + (uint32_t)h == self)
                continue;
            for (int k = 0; k < 2; k++) {
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0)
                    break;
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                struct sockaddr_in a = {0};
                a.sin_family = AF_INET;
                a.sin_port = htons(ports[k]);
                a.sin_addr.s_addr = htonl(net + (uint32_t)h);
                if (connect(fd, (struct sockaddr *)&a, sizeof(a)) && errno != EINPROGRESS) {
                    close(fd);
                    continue;
                }
                p[np] = (struct pollfd){fd, POLLOUT, 0};
                ip[np] = net + (uint32_t)h;
                which[np++] = k;
            }
        }
        long long end = now_ms() + SCAN_MS;
        for (int left = np; left > 0;) {
            long long wait = end - now_ms();
            if (wait <= 0 || poll(p, (nfds_t)np, (int)wait) <= 0)
                break;
            for (int i = 0; i < np; i++) {
                if (p[i].fd < 0 || !p[i].revents)
                    continue;
                int e = 0;
                socklen_t el = sizeof(e);
                if (!getsockopt(p[i].fd, SOL_SOCKET, SO_ERROR, &e, &el) && !e) {
                    nas_host_t *h = host_of(out, &found, max, ip[i]);
                    if (h && which[i] == 0)
                        h->smb = 1;
                    else if (h)
                        h->nfs = 1;
                }
                close(p[i].fd);
                p[i].fd = -1; /* poll leaves it out from now on */
                left--;
            }
        }
        for (int i = 0; i < np; i++)
            if (p[i].fd >= 0)
                close(p[i].fd);
    }
    if (found)
        name_hosts(out, found);
    printf("[nas] scan of %s/24: %d server(s)\n", inet_ntoa((struct in_addr){htonl(net)}), found);
    return found;
}

int nas_shares(const nas_cfg_t *cfg, char (*names)[NAS_NAME_MAX], int max, char *err, size_t n) {
    if (probe(cfg)) {
        snprintf(err, n, "%s", TR("the server does not answer (check its address, that it is on, and its firewall)",
                                  "el servidor no contesta (revisa su dirección, que esté encendido y su cortafuegos)"));
        return -1;
    }
    return be_for(cfg->proto)->shares(cfg, names, max, err, n);
}

/* helper.c of the itemzflow port prints through this */
int sceKernelDebugOutText(int channel, const char *text) {
    (void)channel;
    printf("[fuse] %s\n", text);
    return 0;
}
