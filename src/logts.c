/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Log file with the time at the start of every line: stdout goes into a pipe, and a thread
 * copies it to the log file, adding the time before each line. Every printf of the payload
 * gets its time this way. */
#include "logts.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

void log_stamp(char *out, size_t n) {
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    char day[32];
    strftime(day, sizeof(day), "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(out, n, "%s.%03ld ", day, ts.tv_nsec / 1000000);
}

int log_write_lines(int fd, int *at_start, const char *buf, size_t len, const char *stamp) {
    size_t sl = strlen(stamp);
    while (len) {
        if (*at_start && write_all(fd, stamp, sl))
            return -1;
        const char *nl = memchr(buf, '\n', len);
        size_t take = nl ? (size_t)(nl - buf) + 1 : len;
        if (write_all(fd, buf, take))
            return -1;
        *at_start = nl != NULL;
        buf += take;
        len -= take;
    }
    return 0;
}

static int g_pipe_read = -1, g_file = -1, g_saved_stdout = -1;
static pthread_t g_thread;
static int g_running;
static volatile int g_done; /* the thread saw the end of the pipe */

static void *copy_thread(void *arg) {
    (void)arg;
    char buf[4096], stamp[40];
    int at_start = 1;
    ssize_t n;
    while ((n = read(g_pipe_read, buf, sizeof(buf))) > 0) {
        log_stamp(stamp, sizeof(stamp));
        log_write_lines(g_file, &at_start, buf, (size_t)n, stamp);
    }
    g_done = 1;
    return NULL;
}

int log_start(const char *path) {
    int fds[2];
    g_file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (g_file < 0)
        return -1;
    if (pipe(fds)) {
        close(g_file);
        g_file = -1;
        return -1;
    }
    g_pipe_read = fds[0];
    if (pthread_create(&g_thread, NULL, copy_thread, NULL)) {
        /* no thread: the log keeps working, without times */
        close(fds[0]);
        close(fds[1]);
        g_pipe_read = -1;
        fflush(stdout);
        dup2(g_file, STDOUT_FILENO);
        close(g_file);
        g_file = -1;
        return 0;
    }
    g_running = 1;
    fflush(stdout);
    g_saved_stdout = dup(STDOUT_FILENO); /* given back by log_stop */
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    return 0;
}

void log_stop(void) {
    if (!g_running)
        return;
    g_running = 0;
    fflush(stdout);
    /* the thread reads the rest, then sees the end once fd 1 no longer holds the pipe.
     * The PS5 refuses close(1) (EPERM), which left the thread waiting and the payload
     * unable to exit: fd 1 gets back what it was (the PS4 process outlives the payload)
     * or /dev/null. */
    int null = g_saved_stdout >= 0 ? g_saved_stdout : open("/dev/null", O_WRONLY);
    if (null < 0 || dup2(null, STDOUT_FILENO) < 0)
        close(STDOUT_FILENO);
    if (null > STDOUT_FILENO)
        close(null);
    g_saved_stdout = -1;
    /* waited for at most 2 s: on the PS4 the end of the pipe can fail to arrive, and a
     * payload waiting here never exits (it keeps its ports, so it cannot be loaded again
     * until the console restarts) */
    for (int i = 0; i < 200 && !g_done; i++)
        usleep(10000);
    if (!g_done)
        return; /* the thread keeps its descriptors; the exit takes them */
    pthread_join(g_thread, NULL);
    close(g_pipe_read);
    close(g_file);
    g_pipe_read = g_file = -1;
}
