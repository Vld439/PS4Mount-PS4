/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * NFS client behind the NAS mount: libnfs, one context, read only. Tries NFSv4.2,
 * then NFSv4.0, then NFSv3, so a server with only one of them works the same. */
#define FUSE_USE_VERSION 26

#include "netfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <poll.h>
#include <unistd.h>

#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <libnfs-raw-mount.h> /* struct exportnode */

#include "lang.h"

#ifndef NFS_V4_2
#define NFS_V4_2 42
#endif

static struct nfs_context *g_nfs;
static char g_base[PATH_MAX]; /* "" for the export root, else "/sub" */
static char g_err[192] = "not connected";

static void map(const char *path, char *out, size_t n) {
    if (!strcmp(path, "/"))
        snprintf(out, n, "%s", g_base[0] ? g_base : "/");
    else
        snprintf(out, n, "%s%s", g_base, path); /* path starts with / */
}

static const char *nf_error(void) { return g_err; }

static int nf_connect(const nas_cfg_t *cfg) {
    char sub[sizeof(cfg->sub)];
    size_t l;

    snprintf(sub, sizeof(sub), "%s", cfg->sub);
    for (l = strlen(sub); l && sub[l - 1] == '/'; l--)
        sub[l - 1] = 0;
    while (sub[0] == '/')
        memmove(sub, sub + 1, strlen(sub));
    snprintf(g_base, sizeof(g_base), "%s%s", sub[0] ? "/" : "", sub); /* "" is the export root */

    int versions[] = {NFS_V4_2, 4, 3}; /* NFSv4.2, 4.0, 3: only 42 has a macro */
    for (unsigned i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        g_nfs = nfs_init_context();
        if (!g_nfs) {
            snprintf(g_err, sizeof(g_err), "%s", "out of memory");
            return -1;
        }
        if (nfs_set_version(g_nfs, versions[i])) {
            nfs_destroy_context(g_nfs);
            g_nfs = NULL;
            continue;
        }
        nfs_set_readmax(g_nfs, 262144);
        nfs_set_timeout(g_nfs, 10000);   /* an operation fails instead of waiting forever */
        nfs_set_autoreconnect(g_nfs, 0); /* the panel connects again; no silent retries */
        nfs_set_poll_timeout(g_nfs, 1000);
        if (nfs_mount(g_nfs, cfg->server, cfg->share)) {
            printf("[nas] NFS %s:%s v%d: %s\n", cfg->server, cfg->share, versions[i],
                   nfs_get_error(g_nfs));
            snprintf(g_err, sizeof(g_err), "%s", nfs_get_error(g_nfs));
            nfs_destroy_context(g_nfs);
            g_nfs = NULL;
            continue;
        }
        printf("[nas] NFS %s:%s v%d connected\n", cfg->server, cfg->share, versions[i]);
        g_err[0] = 0;
        return 0;
    }
    snprintf(g_err, sizeof(g_err), "%s", "no reply from the NFS server");
    return -1;
}

static void nf_disconnect(void) {
    if (!g_nfs)
        return;
    printf("[nas] NFS: closing the share\n");
    nfs_umount(g_nfs);
    nfs_destroy_context(g_nfs);
    g_nfs = NULL;
}

static int nf_stat(const char *path, struct stat *st) {
    struct nfs_stat_64 n;
    char p[PATH_MAX];
    map(path, p, sizeof(p));
    int r = nfs_stat64(g_nfs, p, &n);
    if (r)
        return r < 0 ? r : -EIO;
    st->st_dev = n.nfs_dev;
    st->st_ino = n.nfs_ino;
    st->st_mode = (mode_t)n.nfs_mode;
    st->st_nlink = n.nfs_nlink;
    st->st_uid = n.nfs_uid;
    st->st_gid = n.nfs_gid;
    st->st_rdev = n.nfs_rdev;
    st->st_size = (off_t)n.nfs_size;
    st->st_blksize = n.nfs_blksize;
    st->st_blocks = n.nfs_blocks;
    st->st_atime = n.nfs_atime;
    st->st_mtime = n.nfs_mtime;
    st->st_ctime = n.nfs_ctime;
    if (!S_ISDIR(st->st_mode) && !S_ISREG(st->st_mode))
        st->st_mode = S_IFREG | 0444;
    return 0;
}

static int nf_list(const char *path, void *buf, fuse_fill_dir_t fill) {
    struct nfsdir *dir = NULL;
    struct nfsdirent *de;
    char p[PATH_MAX];
    map(path, p, sizeof(p));
    int r = nfs_opendir(g_nfs, p, &dir);
    if (r)
        return r < 0 ? r : -EIO;
    while ((de = nfs_readdir(g_nfs, dir))) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->inode;
        st.st_mode = de->type == 2 /* NF3DIR, also of NFSv4 */ ? S_IFDIR | 0555 : S_IFREG | 0444;
        st.st_nlink = S_ISDIR(st.st_mode) ? 2 : 1;
        st.st_size = (off_t)de->size;
        if (fill(buf, de->name, &st, 0))
            break;
    }
    nfs_closedir(g_nfs, dir);
    return 0;
}

static void *nf_open(const char *path) {
    struct nfsfh *fh = NULL;
    char p[PATH_MAX];
    map(path, p, sizeof(p));
    int r = nfs_open(g_nfs, p, O_RDONLY, &fh);
    if (r) {
        errno = r < 0 ? -r : EIO;
        return NULL;
    }
    return fh;
}

static int nf_read(void *fh, char *buf, size_t size, off_t off) {
    return (int)nfs_pread(g_nfs, fh, buf, size, off);
}

static void nf_close(void *fh) { nfs_close(g_nfs, fh); }

static void nf_piece(int status, struct nfs_context *nfs, void *data, void *priv) {
    (void)nfs;
    (void)data;
    nas_piece_t *p = priv;
    p->r = status;
    p->done = 1;
}

static void nf_submit(void *fh, nas_piece_t *p, int n) {
    for (int i = 0; i < n; i++) {
        p[i].done = 0;
        int e = nfs_pread_async(g_nfs, fh, p[i].buf, p[i].size, (uint64_t)p[i].off, nf_piece, &p[i]);
        if (e) {
            p[i].r = e < 0 ? e : -EIO;
            p[i].done = 1;
        }
    }
}

static int nf_pump(int ms) {
    struct pollfd pfd = {nfs_get_fd(g_nfs), (short)nfs_which_events(g_nfs), 0};
    int k = poll(&pfd, 1, ms);
    /* also without events: libnfs ends the requests that timed out here */
    return nfs_service(g_nfs, k > 0 ? pfd.revents : 0) < 0 ? -EIO : 0;
}

static size_t nf_piece_max(void) { return nfs_get_readmax(g_nfs); }

static int nf_statfs(struct statvfs *st) {
    struct nfs_statvfs_64 sv;
    int r = nfs_statvfs64(g_nfs, g_base[0] ? g_base : "/", &sv);
    if (r)
        return r < 0 ? r : -EIO;
    st->f_bsize = (unsigned long)sv.f_bsize;
    st->f_frsize = (unsigned long)sv.f_frsize;
    st->f_blocks = (fsblkcnt_t)sv.f_blocks;
    st->f_bfree = (fsblkcnt_t)sv.f_bfree;
    st->f_bavail = (fsblkcnt_t)sv.f_bavail;
    st->f_files = (fsfilcnt_t)sv.f_files;
    st->f_ffree = (fsfilcnt_t)sv.f_ffree;
    st->f_favail = (fsfilcnt_t)sv.f_favail;
    st->f_fsid = (unsigned long)sv.f_fsid;
    st->f_flag = (unsigned long)sv.f_flag;
    st->f_namemax = (unsigned long)sv.f_namemax;
    return 0;
}

static int nf_shares(const nas_cfg_t *cfg, char (*names)[NAS_NAME_MAX], int max, char *err, size_t n) {
    /* asked to its mountd through rpcbind (port 111): an NFSv4-only server has neither */
    struct exportnode *ex = mount_getexports_timeout(cfg->server, 4000);
    if (!ex) {
        printf("[nas] NFS exports of %s: none or no reply\n", cfg->server);
        snprintf(err, n, "%s", TR("it lists no exports (type the export by hand)",
                                  "no da la lista de exportaciones (escribe la exportación a mano)"));
        return -1;
    }
    int k = 0;
    for (struct exportnode *e = ex; e && k < max; e = e->ex_next)
        if (e->ex_dir && e->ex_dir[0])
            snprintf(names[k++], NAS_NAME_MAX, "%s", e->ex_dir);
    mount_free_export_list(ex);
    return k;
}

const nas_be_t nas_be_nfs = {
    "NFS", nf_connect, nf_disconnect, nf_stat, nf_list,
    nf_open, nf_read, nf_submit, nf_pump, nf_piece_max, nf_close, nf_statfs, nf_error, nf_shares,
};
