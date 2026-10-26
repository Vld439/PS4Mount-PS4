/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * SMB client behind the NAS mount: libsmb2, one context, read only. Its paths are
 * relative to the share ("", "games/x.pkg"), never start with a slash. */
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

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h> /* smb2_free_data */
#include <smb2/libsmb2-share-enum.h>

#include "lang.h"

static struct smb2_context *g_smb;
static char g_base[PATH_MAX]; /* "" for the share root, else "sub" */
static char g_err[192] = "not connected";

static void map(const char *path, char *out, size_t n) {
    if (!strcmp(path, "/"))
        snprintf(out, n, "%s", g_base);
    else if (g_base[0])
        snprintf(out, n, "%s%s", g_base, path); /* path starts with / */
    else
        snprintf(out, n, "%s", path + 1);
}

static const char *sm_error(void) { return g_err; }

static int sm_connect(const nas_cfg_t *cfg) {
    char server[96], sub[sizeof(cfg->sub)];
    size_t l;

    if (cfg->port)
        snprintf(server, sizeof(server), "%s:%d", cfg->server, cfg->port);
    else
        snprintf(server, sizeof(server), "%s", cfg->server);

    snprintf(sub, sizeof(sub), "%s", cfg->sub);
    for (l = strlen(sub); l && (sub[l - 1] == '/' || sub[l - 1] == '\\'); l--)
        sub[l - 1] = 0;
    while (sub[0] == '/' || sub[0] == '\\')
        memmove(sub, sub + 1, strlen(sub));
    snprintf(g_base, sizeof(g_base), "%s", sub);

    g_smb = smb2_init_context();
    if (!g_smb) {
        snprintf(g_err, sizeof(g_err), "%s", "out of memory");
        return -1;
    }
    smb2_set_timeout(g_smb, 20);
    smb2_set_security_mode(g_smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (cfg->user[0]) {
        smb2_set_user(g_smb, cfg->user);
        smb2_set_password(g_smb, cfg->pass);
    }
    if (smb2_connect_share(g_smb, server, cfg->share, cfg->user[0] ? cfg->user : NULL)) {
        snprintf(g_err, sizeof(g_err), "%s", smb2_get_error(g_smb));
        smb2_destroy_context(g_smb);
        g_smb = NULL;
        return -1;
    }
    printf("[nas] SMB //%s/%s connected as %s\n", server, cfg->share,
           cfg->user[0] ? cfg->user : "guest");
    g_err[0] = 0;
    return 0;
}

static void sm_disconnect(void) {
    if (!g_smb)
        return;
    smb2_disconnect_share(g_smb);
    smb2_destroy_context(g_smb);
    g_smb = NULL;
}

static int sm_stat(const char *path, struct stat *st) {
    char p[PATH_MAX];

    if (!strcmp(path, "/")) { /* the server need not describe the share root */
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }
    map(path, p, sizeof(p));
    struct smb2_stat_64 n;
    int r = smb2_stat(g_smb, p, &n);
    if (r)
        return r < 0 ? r : -EIO;
    int dir = n.smb2_type == SMB2_TYPE_DIRECTORY;
    st->st_ino = n.smb2_ino;
    st->st_mode = dir ? S_IFDIR | 0555 : S_IFREG | 0444;
    st->st_nlink = dir ? 2 : 1;
    st->st_size = (off_t)n.smb2_size;
    st->st_atime = (time_t)n.smb2_atime;
    st->st_mtime = (time_t)n.smb2_mtime;
    st->st_ctime = (time_t)n.smb2_ctime;
    return 0;
}

static int sm_list(const char *path, void *buf, fuse_fill_dir_t fill) {
    char p[PATH_MAX];
    map(path, p, sizeof(p));
    struct smb2dir *dir = smb2_opendir(g_smb, p);
    if (!dir) {
        printf("[nas] SMB opendir %s: %s\n", p, smb2_get_error(g_smb));
        return -EIO;
    }
    struct smb2dirent *de;
    while ((de = smb2_readdir(g_smb, dir))) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        int d = de->st.smb2_type == SMB2_TYPE_DIRECTORY;
        st.st_ino = de->st.smb2_ino;
        st.st_mode = d ? S_IFDIR | 0555 : S_IFREG | 0444;
        st.st_nlink = d ? 2 : 1;
        st.st_size = (off_t)de->st.smb2_size;
        if (fill(buf, de->name, &st, 0))
            break;
    }
    smb2_closedir(g_smb, dir);
    return 0;
}

static void *sm_open(const char *path) {
    char p[PATH_MAX];
    map(path, p, sizeof(p));
    struct smb2fh *fh = smb2_open(g_smb, p, O_RDONLY);
    if (!fh) {
        printf("[nas] SMB open %s: %s\n", p, smb2_get_error(g_smb));
        errno = EIO;
    }
    return fh;
}

static int sm_read(void *fh, char *buf, size_t size, off_t off) {
    return smb2_pread(g_smb, fh, (uint8_t *)buf, size, off);
}

static void sm_close(void *fh) { smb2_close(g_smb, fh); }

static void sm_piece(struct smb2_context *smb2, int status, void *data, void *priv) {
    (void)smb2;
    (void)data;
    nas_piece_t *p = priv;
    p->r = status;
    p->done = 1;
}

static void sm_submit(void *fh, nas_piece_t *p, int n) {
    for (int i = 0; i < n; i++) {
        p[i].done = 0;
        int e = smb2_pread_async(g_smb, fh, (uint8_t *)p[i].buf, (uint32_t)p[i].size, (uint64_t)p[i].off, sm_piece, &p[i]);
        if (e) {
            p[i].r = e < 0 ? e : -EIO;
            p[i].done = 1;
        }
    }
}

static int sm_pump(int ms) {
    struct pollfd pfd = {smb2_get_fd(g_smb), (short)smb2_which_events(g_smb), 0};
    int k = poll(&pfd, 1, ms);
    /* also without events: libsmb2 ends the requests that timed out here */
    return smb2_service(g_smb, k > 0 ? pfd.revents : 0) < 0 ? -EIO : 0;
}

static size_t sm_piece_max(void) { return smb2_get_max_read_size(g_smb); }

static int sm_statfs(struct statvfs *st) {
    struct smb2_statvfs sv;
    int r = smb2_statvfs(g_smb, g_base, &sv);
    if (r)
        return r < 0 ? r : -EIO;
    st->f_bsize = sv.f_bsize;
    st->f_frsize = sv.f_frsize;
    st->f_blocks = sv.f_blocks;
    st->f_bfree = sv.f_bfree;
    st->f_bavail = sv.f_bavail;
    st->f_files = sv.f_files;
    st->f_ffree = sv.f_ffree;
    st->f_favail = sv.f_favail;
    st->f_fsid = sv.f_fsid;
    st->f_flag = sv.f_flag;
    st->f_namemax = sv.f_namemax;
    return 0;
}

#define SHARE_DISK 0 /* the low bits of a share's type: a folder (not a printer or IPC$) */

static int sm_shares(const nas_cfg_t *cfg, char (*names)[NAS_NAME_MAX], int max, char *err, size_t n) {
    char server[96];
    if (cfg->port)
        snprintf(server, sizeof(server), "%s:%d", cfg->server, cfg->port);
    else
        snprintf(server, sizeof(server), "%s", cfg->server);
    struct smb2_context *s = smb2_init_context();
    if (!s) {
        snprintf(err, n, "%s", TR("out of memory", "sin memoria"));
        return -1;
    }
    smb2_set_timeout(s, 10);
    smb2_set_security_mode(s, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (cfg->user[0]) {
        smb2_set_user(s, cfg->user);
        smb2_set_password(s, cfg->pass);
    }
    int k = -1; /* -2: the server wants (other) credentials */
    struct smb2_share_enum_reply *r = NULL;
    int rc = smb2_connect_share(s, server, "IPC$", cfg->user[0] ? cfg->user : NULL);
    if (rc || !(r = smb2_share_enum_sync(s, SMB2_SHARE_INFO_1))) {
        const char *e = smb2_get_error(s);
        printf("[nas] SMB shares of %s: %d %s\n", server, rc, e);
        /* a logon failure is ECONNREFUSED here (the port answered the probe), a refused
         * account EACCES; the text is often a later socket error */
        if (rc == -ECONNREFUSED || rc == -EACCES || strstr(e, "LOGON_FAILURE") || strstr(e, "ACCESS_DENIED"))
            k = -2, snprintf(err, n, "%s", cfg->user[0] ? TR("the server turned down that user and password",
                                                     "el servidor rechazó ese usuario y contraseña")
                                                : TR("the server asks for a user and password",
                                                     "el servidor pide usuario y contraseña"));
        else
            snprintf(err, n, TR("its shares could not be listed: %s", "no se pudieron listar sus recursos: %s"), e);
    } else {
        k = 0;
        for (uint32_t i = 0; i < r->entries_read && k < max; i++) {
            const struct smb2_share_info_1 *e = &r->share_info.info_1[i];
            size_t l = e->netname ? strlen(e->netname) : 0;
            /* folders only; a name ending in $ is hidden (C$, ADMIN$, print$) */
            if (!l || (e->type & 0x3) != SHARE_DISK || e->netname[l - 1] == '$')
                continue;
            snprintf(names[k++], NAS_NAME_MAX, "%s", e->netname);
        }
        smb2_free_data(s, r);
    }
    if (smb2_get_fd(s) >= 0)
        smb2_disconnect_share(s);
    smb2_destroy_context(s);
    return k;
}

const nas_be_t nas_be_smb = {
    "SMB", sm_connect, sm_disconnect, sm_stat, sm_list,
    sm_open, sm_read, sm_submit, sm_pump, sm_piece_max, sm_close, sm_statfs, sm_error, sm_shares,
};
