/* PS4Mount: replaces the <orbis/libkernel.h> that itemzflow's libfuse port
 * includes, which only provides the two module-loading calls used by
 * fuse_dlopen(). PS4Mount links everything it needs into the payload and never
 * loads a FUSE module at run time, so both report "not supported" and dlopen
 * keeps failing the way it did before the port. */
#ifndef PS4MOUNT_ORBIS_LIBKERNEL_H
#define PS4MOUNT_ORBIS_LIBKERNEL_H

#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>

typedef int SceKernelModule;

/* scePthread* is the system's pthread API: the payload SDK ships plain
 * pthreads, so these are the same calls. */
typedef pthread_t ScePthread;

static inline int scePthreadCreate(ScePthread *t, const pthread_attr_t *attr,
                                   void *(*fn)(void *), void *arg, const char *name) {
    (void)name;
    return pthread_create(t, attr, fn, arg);
}

static inline int scePthreadJoin(ScePthread t, void **ret) {
    return pthread_join(t, ret);
}

static inline int scePthreadCancel(ScePthread t) {
    return pthread_cancel(t);
}

#define sceKernelOpen open
#define sceKernelWrite write
#define sceKernelClose close

static inline SceKernelModule sceKernelLoadStartModule(const char *name, size_t argc,
                                                       const void *argv, unsigned int flags,
                                                       const void *opt, unsigned int *res) {
    (void)name; (void)argc; (void)argv; (void)flags; (void)opt;
    if (res)
        *res = 0;
    return -1;
}

static inline int sceKernelStopUnloadModule(SceKernelModule handle, size_t argc,
                                            const void *argv, unsigned int flags,
                                            const void *opt, unsigned int *res) {
    (void)handle; (void)argc; (void)argv; (void)flags; (void)opt;
    if (res)
        *res = 0;
    return -1;
}

#endif
