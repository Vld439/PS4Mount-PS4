/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * BGFT installs: the interface flatz documented (ps4_stub_lib_maker_v2, bgft.h), as
 * open installers such as ps4-ipi use it. libSceBgft is loaded the first time. */
#include "install.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ps4/kernel.h>

#include "mem.h"

typedef struct {
    void *heap;
    size_t heap_size;
} bgft_init_t;

typedef struct {
    int32_t user_id, entitlement_type;
    const char *id, *content_url, *content_ex_url, *content_name, *icon_path, *sku_id;
    int option;
    const char *playgo_scenario_id, *release_date, *package_type, *package_sub_type;
    uint32_t package_size;
} bgft_param_t;

typedef struct {
    bgft_param_t params;
    uint32_t slot;
} bgft_param_ex_t;

#define BGFT_OPT_DISABLE_CDN_QUERY_PARAM 0x10000
#define BGFT_HEAP (1u << 20)

int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, int *);

static int (*g_register)(bgft_param_ex_t *, int *);
static int (*g_start)(int);
static int (*g_term)(void);
static void *g_heap;

static int bgft_open(char *msg, unsigned long n) {
    if (g_register)
        return 0;
    int res = 0;
    sceKernelLoadStartModule("/system/common/lib/libSceBgft.sprx", 0, NULL, 0, NULL, &res);
    uint32_t h = 0;
    if (kernel_dynlib_handle(getpid(), "libSceBgft.sprx", &h)) {
        snprintf(msg, n, "the system installer (libSceBgft) could not be loaded");
        return -1;
    }
    int (*init)(bgft_init_t *) = (void *)kernel_dynlib_dlsym(getpid(), h, "sceBgftServiceIntInit");
    int (*reg)(bgft_param_ex_t *, int *) =
        (void *)kernel_dynlib_dlsym(getpid(), h, "sceBgftServiceIntDownloadRegisterTaskByStorageEx");
    int (*start)(int) = (void *)kernel_dynlib_dlsym(getpid(), h, "sceBgftServiceDownloadStartTask");
    int (*term)(void) = (void *)kernel_dynlib_dlsym(getpid(), h, "sceBgftServiceIntTerm");
    if (!init || !reg || !start || !term) {
        snprintf(msg, n, "the system installer lacks the functions PS4Mount uses");
        return -1;
    }
    /* the service keeps its heap for as long as the payload runs */
    static bgft_init_t ip;
    ip.heap_size = BGFT_HEAP;
    ip.heap = mem_alloc(BGFT_HEAP);
    if (!ip.heap) {
        snprintf(msg, n, "no memory for the system installer");
        return -1;
    }
    memset(ip.heap, 0, BGFT_HEAP);
    int rc = init(&ip);
    if (rc) {
        /* the payload runs in a borrowed process: another payload there may have left the
         * service open (0x80990001), and it serves one client */
        int t = term(), again = init(&ip);
        printf("[dlc] installer init 0x%08x, after closing it (0x%08x): 0x%08x\n", rc, t, again);
        rc = again;
    }
    if (rc) {
        snprintf(msg, n, "the system installer did not start (0x%08x)", rc);
        mem_free(ip.heap);
        return -1;
    }
    g_register = reg;
    g_start = start;
    g_term = term;
    g_heap = ip.heap;
    return 0;
}

int install_pkg(const char *path, const char *name, int user, char *msg, unsigned long n) {
    if (bgft_open(msg, n))
        return -1;
    bgft_param_ex_t p;
    memset(&p, 0, sizeof(p));
    p.params.user_id = user;
    p.params.entitlement_type = 5;
    p.params.id = "";
    p.params.content_url = path;
    p.params.content_name = name;
    p.params.icon_path = "";
    p.params.playgo_scenario_id = "0";
    p.params.option = BGFT_OPT_DISABLE_CDN_QUERY_PARAM;
    int task = -1, rc = g_register(&p, &task);
    if (rc) {
        snprintf(msg, n, "the system installer refused %s (0x%08x)", path, rc);
        return -1;
    }
    rc = g_start(task);
    if (rc) {
        snprintf(msg, n, "the install of %s did not start (0x%08x)", path, rc);
        return -1;
    }
    snprintf(msg, n, "installing %s (task %08x)", path, task);
    return task;
}

void install_close(void) {
    if (!g_register)
        return;
    g_term();
    mem_free(g_heap);
    g_register = NULL;
}
