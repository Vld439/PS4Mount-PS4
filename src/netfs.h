/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * A network share (NFS or SMB) shown as a read-only folder through FUSE, so the
 * catalog can use it as one more drive. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <fuse.h>

enum { NAS_NFS = 1, NAS_SMB = 2 };

/* What the panel keeps in config.txt. */
typedef struct {
    int proto;           /* NAS_NFS or NAS_SMB */
    char server[64];
    int port;            /* 0: the protocol's port */
    char share[256];     /* NFS export or SMB share name */
    char sub[256];       /* folder inside it, "" for its root */
    char user[64];       /* SMB; "" is the guest account */
    char pass[64];
} nas_cfg_t;

#define NAS_NAME_MAX 128

/* A server found on the console's network: what answered, and its name when it gave one. */
typedef struct {
    char ip[16];
    char name[32];
    int smb, nfs;
} nas_host_t;

/* One part of a read sent without waiting: once done, r is the bytes read or -errno. */
typedef struct {
    char *buf;
    size_t size;
    off_t off;
    int r;
    int done;
} nas_piece_t;

/* A client behind the mount. Only one is connected at a time; changing the
 * settings swaps it without touching the mount. All the calls run serialized
 * and use FUSE paths ("/", "/game.pkg"); a failure returns -errno. */
typedef struct {
    const char *name;
    int (*connect)(const nas_cfg_t *cfg);
    void (*disconnect)(void);
    int (*stat)(const char *path, struct stat *st);
    int (*list)(const char *path, void *buf, fuse_fill_dir_t fill);
    void *(*open)(const char *path);
    int (*read)(void *fh, char *buf, size_t size, off_t off);
    /* Reads sent without waiting for them: over a link with a long round trip one
     * read at a time spends most of it waiting. submit sends the pieces (one it cannot
     * send is done at once with its error); pump moves the replies along, waiting up
     * to ms, and marks the pieces done. A piece's memory must stay until it is done. */
    void (*submit)(void *fh, nas_piece_t *p, int n);
    int (*pump)(int ms);
    size_t (*piece_max)(void); /* the largest piece the server answers in one go */
    void (*close)(void *fh);
    int (*statfs)(struct statvfs *st);
    const char *(*error)(void);
    /* The shares (SMB) or exports (NFS) of cfg's server, without connecting a client:
     * how many were put in names, or -1 with err (a text for the player); -2 when the
     * server wants a user and password (or turned down the ones given). */
    int (*shares)(const nas_cfg_t *cfg, char (*names)[NAS_NAME_MAX], int max, char *err, size_t n);
} nas_be_t;

extern const nas_be_t nas_be_nfs, nas_be_smb;

/* Mounts the filesystem once per process (it cannot be unmounted) and connects
 * the backend. The mountpoint is a new /mnt/ps4mount/nas-<n>, never a previous
 * mount: a mount whose owner is gone would fail every request. 0 on success. */
int nas_start(const nas_cfg_t *cfg, char *err, size_t n);
/* Replaces the backend of the mount that is already up (switching protocol,
 * server or folder). 0 on success; the old backend is closed either way. */
int nas_reconnect(const nas_cfg_t *cfg, char *err, size_t n);
/* Closes the backend; the mount stays, answering ENOTCONN. */
void nas_stop(void);
int nas_active(void);
int nas_mounted(void);
const char *nas_mountpoint(void);
/* "NFS 192.168.0.19:/ps4share: ready", or why it is not. */
const char *nas_status(void);
/* 1 when the fuse devices cannot be opened from this process (a payload started by
 * GoldHEN's AutoRun): it has to be loaded again through BinLoader. */
int nas_need_reload(void);
/* Looks for file servers on the console's network (the /24 of its address): the hosts
 * that take a connection on the SMB or NFS port, with their NetBIOS name. How many were
 * put in out; it takes a couple of seconds. */
int nas_scan(nas_host_t *out, int max);
/* What cfg's server shares (cfg->share is not used): as nas_be_t.shares. */
int nas_shares(const nas_cfg_t *cfg, char (*names)[NAS_NAME_MAX], int max, char *err, size_t n);
