/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * HTTPS client for Google Drive: one request per connection, TLS with BearSSL (the same
 * code on the console and in the host tests) and only Google's roots trusted. A plain
 * http:// URL is for the tests' local server. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build.h"

/* Receives the response body as it arrives; nonzero stops the transfer. */
typedef int (*http_sink_t)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    const char *method, *url;
    const char *headers;      /* more header lines, each ending in \r\n; may be NULL */
    const void *body;         /* request body, body_len bytes */
    size_t body_len;
    http_sink_t sink;         /* NULL: the body goes to http_resp_t.body (at most HTTP_BODY_MAX) */
    void *sink_ctx;
    volatile int *cancel;     /* set from another thread: the request stops (may be NULL) */
    int stall;                /* seconds without a byte before the request gives up (0: 60) */
} http_req_t;

#define HTTP_BODY_MAX (1u << 20)

typedef struct {
    int status;
    buf_t body;
    char location[2048];      /* Location: */
    char range[64];           /* Range: (a resumable upload's 308) */
    uint64_t length;          /* Content-Length:, UINT64_MAX when not given */
} http_resp_t;

/* 0 when a response arrived (any status); -1 otherwise, err says why. */
int http_request(const http_req_t *q, http_resp_t *r, char *err, size_t n);
void http_resp_free(http_resp_t *r);

/* Percent-encodes s for a query or a form value. */
void http_escape(buf_t *out, const char *s);

/* Parsed URL; -1 when it is not http(s)://host[:port]/path. */
typedef struct {
    int tls, port;
    char host[256];
    const char *path;         /* points into the URL: "/..." */
} http_url_t;
int http_parse_url(const char *url, http_url_t *u);
