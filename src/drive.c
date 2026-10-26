/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Google Drive backups (see drive.h). Drive's v3 API: the device flow to sign in,
 * resumable uploads in chunks (a cut transfer goes on from the last byte Drive has, also
 * after a restart), ranged downloads into a hidden part file, and MD5 checks of both. */
#include "drive.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bearssl.h"
#include "https.h"
#include "json.h"
#include "pkg.h"
#include "lang.h"
#include "mem.h"

#define SCOPE "https://www.googleapis.com/auth/drive.file"
#define FOLDER_MIME "application/vnd.google-apps.folder"
#define MAX_JOBS 256
#define MAX_REMOTE 256
#define TRIES 6            /* attempts of a request that fails for a passing reason */
#define PART_DIR ".ps4mount-drive"
/* part files hold whole blocks in order from the start; ones from before (.part, written
 * out of order with holes) are never taken as a start */
#define PART_EXT ".dl"
#define SPACE_MARGIN (64ull << 20)

/* Each chunk is a connection of its own, and its TLS handshake costs ~1.2 s on the
 * console: 8 MB chunks lost a quarter of a 2 MB/s upload, 32 MB ones lose ~7%. */
size_t g_drive_chunk = 32u << 20;
int g_drive_retry_seconds = 2;

enum { OP_LOGIN, OP_LIST, OP_UPLOAD, OP_DOWNLOAD, OP_TRASH };
static const char *const k_op_names[] = {"login", "list", "upload", "download", "trash"};

typedef struct {
    int op;
    char tid[10], dest[16];
} job_t;

typedef struct {
    char tid[10], name[128];
    uint64_t size;
    int files, update, addons;
} remote_game_t;

typedef struct {
    char id[128], name[96], md5[33], kind[8];
    uint64_t size;
} remote_file_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static int g_started, g_quit;
static volatile int g_cancel;

static char g_state_path[PATH_MAX], g_session_path[PATH_MAX], g_client_path[PATH_MAX], g_queue_path[PATH_MAX];
/* the user's own OAuth client (Google Cloud, "TVs and limited input devices"): none ships
 * with PS4Mount; the secret never leaves the console's state folder */
static char g_client_id[256], g_client_secret[128];
static drive_endpoints_t g_ep;
static char g_ep_oauth[256], g_ep_api[256], g_ep_upload[256];
static drive_host_t g_host;

/* signed in (all under g_mu) */
static char g_refresh[512], g_email[128], g_root[128], g_revoke[512];
static char g_access[2048];
static time_t g_access_until;
static long long g_quota_limit = -1, g_quota_used = -1;

/* signing in */
static int g_login;
/* Google ended the access (an app in testing gets a week): a new code is asked for at once
 * and shown on the console's screen, and the job it cut waits for it */
static int g_expired, g_relogin;
static char g_user_code[32], g_verify_url[128], g_device_code[512], g_login_error[256];
static time_t g_login_until, g_login_next;
static int g_login_interval;

/* the queue and the transfer in progress */
static job_t g_jobs[MAX_JOBS];
static int g_njobs;
static int g_busy, g_op;
static char g_cur_tid[10], g_cur_name[128], g_cur_file[96], g_cur_dest[16];
static uint64_t g_done, g_total;
static int g_file_i, g_file_n;
static char g_result[512];
static int g_result_ok = 1;
/* the whole game: bytes of the files done (base) out of all it moves, the phase, and the
 * samples the speed and the time left come from */
static uint64_t g_base, g_all;
static const char *g_phase = "";
static time_t g_job_start, g_retry_until;
static int g_milestone;
#define SAMPLES 30
static struct {
    double t;
    uint64_t b;
} g_samples[SAMPLES];
static int g_nsamples;

/* the games in Drive, as last listed */
static remote_game_t *g_remote;
static int g_nremote, g_listed;
static char g_list_error[256];

static void queue_front_locked(int op);

/* --- small helpers --- */

static void set_result(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_result(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_mu);
    vsnprintf(g_result, sizeof(g_result), fmt, ap);
    g_result_ok = ok;
    pthread_mutex_unlock(&g_mu);
    va_end(ap);
    printf("[drive] %s\n", g_result);
}

static void json_esc(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_put(b, "\\", 1);
        if ((unsigned char)*s >= 0x20)
            buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

static void form_add(buf_t *b, const char *key, const char *value) {
    if (b->len)
        buf_put(b, "&", 1);
    buf_printf(b, "%s=", key);
    http_escape(b, value);
}

/* buf_t text as a C string (its data stays owned by b). */
static const char *text(buf_t *b) {
    buf_put(b, "", 1);
    b->len--;
    return (const char *)b->data;
}

static void save_state(void) {
    pthread_mutex_lock(&g_mu);
    buf_t b = {0};
    if (g_refresh[0])
        buf_printf(&b, "refresh=%s\nemail=%s\nroot=%s\n", g_refresh, g_email, g_root);
    pthread_mutex_unlock(&g_mu);
    if (!b.len)
        unlink(g_state_path);
    else {
        char tmp[PATH_MAX + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", g_state_path);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            int ok = write(fd, b.data, b.len) == (ssize_t)b.len;
            close(fd);
            if (ok)
                rename(tmp, g_state_path);
            else
                unlink(tmp);
        }
    }
    buf_free(&b);
}

static void load_state(void) {
    FILE *f = fopen(g_state_path, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "refresh=", 8))
            snprintf(g_refresh, sizeof(g_refresh), "%.511s", line + 8);
        else if (!strncmp(line, "email=", 6))
            snprintf(g_email, sizeof(g_email), "%.127s", line + 6);
        else if (!strncmp(line, "root=", 5))
            snprintf(g_root, sizeof(g_root), "%.127s", line + 5);
    }
    fclose(f);
}

/* Sleeps s seconds, a second at a time: 0, or -1 when cancelled or stopping. */
static int pause_s(int s) {
    pthread_mutex_lock(&g_mu);
    g_retry_until = time(NULL) + s;
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < s; i++) {
        if (g_cancel || g_quit)
            return -1;
        sleep(1);
    }
    return g_cancel || g_quit ? -1 : 0;
}

/* Google's own explanation in an error answer: {"error":{"message":..}} or
 * {"error":"..","error_description":".."}. */
static void google_error(const http_resp_t *r, char *out, size_t n) {
    jv_t v = jv_of((const char *)r->body.data, r->body.len), e;
    char msg[256] = "";
    if (!jv_get(v, "error", &e)) {
        if (jv_get_string(e, "message", msg, sizeof(msg)) && jv_string(e, msg, sizeof(msg)) == 0) {
            char d[200];
            if (!jv_get_string(v, "error_description", d, sizeof(d)) && d[0]) {
                size_t l = strlen(msg);
                snprintf(msg + l, sizeof(msg) - l, ": %s", d);
            }
        }
    }
    if (msg[0])
        snprintf(out, n, TR("Google answered %d: %s", "Google respondió %d: %s"), r->status, msg);
    else
        snprintf(out, n, TR("Google answered %d", "Google respondió %d"), r->status);
}

/* The reason of a Drive 403 ({"error":{"errors":[{"reason":..}]}}). */
static void error_reason(const http_resp_t *r, char *out, size_t n) {
    jv_t v = jv_of((const char *)r->body.data, r->body.len), e, l, f;
    out[0] = 0;
    if (!jv_get(v, "error", &e) && !jv_get(e, "errors", &l) && !jv_index(l, 0, &f))
        jv_get_string(f, "reason", out, n);
}

static int transient(int status) { return status == 429 || status >= 500; }

/* --- signing in --- */

static void disconnected(const char *why) {
    pthread_mutex_lock(&g_mu);
    g_refresh[0] = g_access[0] = g_email[0] = g_root[0] = 0;
    g_access_until = 0;
    g_nremote = 0;
    g_listed = 0;
    pthread_mutex_unlock(&g_mu);
    save_state();
    if (why) {
        set_result(0, "%s", why);
        if (g_host.notify)
            g_host.notify(why);
    }
}

/* The client's id and secret to a form. */
static void form_client(buf_t *form, int secret) {
    char id[256], sec[128];
    pthread_mutex_lock(&g_mu);
    snprintf(id, sizeof(id), "%s", g_client_id);
    snprintf(sec, sizeof(sec), "%s", g_client_secret);
    pthread_mutex_unlock(&g_mu);
    form_add(form, "client_id", id);
    if (secret)
        form_add(form, "client_secret", sec);
    memset(sec, 0, sizeof(sec));
}

/* A POST of a form to the OAuth server; the answer in r. */
static int oauth_post(const char *path, buf_t *form, http_resp_t *r, char *err, size_t n) {
    char url[512];
    snprintf(url, sizeof(url), "%s%s", g_ep.oauth, path);
    http_req_t q = {.method = "POST", .url = url,
                    .headers = "Content-Type: application/x-www-form-urlencoded\r\n",
                    .body = form->data, .body_len = form->len};
    return http_request(&q, r, err, n);
}

/* A valid access token in g_access: refreshed when it ends within 5 minutes. */
static int access_token(char *err, size_t n) {
    pthread_mutex_lock(&g_mu);
    int ok = g_access[0] && time(NULL) + 300 < g_access_until;
    char refresh[512];
    snprintf(refresh, sizeof(refresh), "%s", g_refresh);
    pthread_mutex_unlock(&g_mu);
    if (ok)
        return 0;
    if (!refresh[0]) {
        snprintf(err, n, "%s", TR("Google Drive is not connected", "Google Drive no está conectado"));
        return -1;
    }
    buf_t form = {0};
    form_client(&form, 1);
    form_add(&form, "refresh_token", refresh);
    form_add(&form, "grant_type", "refresh_token");
    http_resp_t r;
    int rc = -1;
    for (int t = 0; t < TRIES; t++) {
        rc = oauth_post("/token", &form, &r, err, n);
        if (!rc && !transient(r.status))
            break;
        if (!rc)
            google_error(&r, err, n);
        http_resp_free(&r);
        rc = -1;
        if (pause_s(g_drive_retry_seconds << t))
            break;
    }
    buf_free(&form);
    if (rc)
        return -1;
    jv_t v = jv_of((const char *)r.body.data, r.body.len);
    char tok[2048], e[64];
    if (r.status == 200 && !jv_get_string(v, "access_token", tok, sizeof(tok)) && tok[0]) {
        pthread_mutex_lock(&g_mu);
        snprintf(g_access, sizeof(g_access), "%s", tok);
        g_access_until = time(NULL) + jv_get_int(v, "expires_in", 3600);
        pthread_mutex_unlock(&g_mu);
        http_resp_free(&r);
        return 0;
    }
    if (!jv_get_string(v, "error", e, sizeof(e)) && !strcmp(e, "invalid_grant")) {
        /* revoked from the Google account, or expired: only signing in again helps */
        http_resp_free(&r);
        printf("[drive] the access ended (invalid_grant): asking for a new code\n");
        disconnected(NULL);
        pthread_mutex_lock(&g_mu);
        g_expired = g_relogin = 1;
        queue_front_locked(OP_LOGIN);
        pthread_mutex_unlock(&g_mu);
        snprintf(err, n, "%s", TR("Google Drive needs you to connect again", "Google Drive necesita que vuelvas a conectarlo"));
        return -1;
    }
    google_error(&r, err, n);
    http_resp_free(&r);
    return -1;
}

static void login_start(void) {
    buf_t form = {0};
    form_client(&form, 0);
    form_add(&form, "scope", SCOPE);
    http_resp_t r;
    char err[256];
    int rc = oauth_post("/device/code", &form, &r, err, sizeof(err));
    buf_free(&form);
    if (!rc && r.status != 200) {
        google_error(&r, err, sizeof(err));
        rc = -1;
    }
    jv_t v = jv_of(rc ? "" : (const char *)r.body.data, rc ? 0 : r.body.len);
    pthread_mutex_lock(&g_mu);
    if (!rc && !jv_get_string(v, "device_code", g_device_code, sizeof(g_device_code)) &&
        !jv_get_string(v, "user_code", g_user_code, sizeof(g_user_code))) {
        if (jv_get_string(v, "verification_url", g_verify_url, sizeof(g_verify_url)))
            jv_get_string(v, "verification_uri", g_verify_url, sizeof(g_verify_url));
        g_login_until = time(NULL) + jv_get_int(v, "expires_in", 1800);
        g_login_interval = (int)jv_get_int(v, "interval", 5);
        g_login_next = time(NULL) + g_login_interval;
        g_login = 1;
        g_login_error[0] = 0;
    } else {
        g_login = 0;
        snprintf(g_login_error, sizeof(g_login_error), "%s", rc ? err : TR("Google did not give a code", "Google no dio ningún código"));
    }
    int relogin = g_relogin && g_login, relogin_failed = g_relogin && !g_login;
    char code[64], url[160];
    snprintf(code, sizeof(code), "%s", g_user_code);
    snprintf(url, sizeof(url), "%s", g_verify_url);
    g_relogin = 0;
    pthread_mutex_unlock(&g_mu);
    if (!rc)
        http_resp_free(&r);
    printf("[drive] sign-in %s\n", g_login ? "code shown" : g_login_error);
    if (relogin && g_host.notify) {
        /* nobody may be looking at the panel: the code goes to the screen */
        char m[300];
        snprintf(m, sizeof(m), TR("Google Drive asks to connect again: on your phone open %s and type %s",
                                  "Google Drive pide conectar de nuevo: en el móvil abre %s y escribe %s"),
                 url, code);
        g_host.notify(m);
    } else if (relogin_failed && g_host.notify)
        g_host.notify(TR("Google Drive was disconnected: connect it again in the panel",
                         "Google Drive se desconectó: vuelve a conectarlo en el panel"));
}

static void queue_locked(int op, const char *tid, const char *dest);

/* One poll of the token endpoint while the code is shown. */
static void login_poll(void) {
    pthread_mutex_lock(&g_mu);
    char device[512];
    snprintf(device, sizeof(device), "%s", g_device_code);
    int expired = time(NULL) > g_login_until;
    pthread_mutex_unlock(&g_mu);
    if (expired) {
        pthread_mutex_lock(&g_mu);
        g_login = 0;
        g_njobs = 0; /* what waited for the code cannot go on without it */
        snprintf(g_login_error, sizeof(g_login_error), "%s",
                 TR("The code expired: press Connect again", "El código caducó: pulsa Conectar otra vez"));
        pthread_mutex_unlock(&g_mu);
        return;
    }
    buf_t form = {0};
    form_client(&form, 1);
    form_add(&form, "device_code", device);
    form_add(&form, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    http_resp_t r;
    char err[256];
    int rc = oauth_post("/token", &form, &r, err, sizeof(err));
    buf_free(&form);
    if (rc)
        return; /* the network: next poll */
    jv_t v = jv_of((const char *)r.body.data, r.body.len);
    char e[64] = "", access[2048], refresh[512];
    jv_get_string(v, "error", e, sizeof(e));
    pthread_mutex_lock(&g_mu);
    if (r.status == 200 && !jv_get_string(v, "access_token", access, sizeof(access)) &&
        !jv_get_string(v, "refresh_token", refresh, sizeof(refresh))) {
        snprintf(g_access, sizeof(g_access), "%s", access);
        snprintf(g_refresh, sizeof(g_refresh), "%s", refresh);
        g_access_until = time(NULL) + jv_get_int(v, "expires_in", 3600);
        g_root[0] = g_email[0] = 0;
        g_login = 0;
        g_result[0] = 0; /* "disconnected" and the like belong to the account before */
        g_result_ok = 1;
        queue_locked(OP_LIST, "", "");
    } else if (!strcmp(e, "authorization_pending")) {
        ;
    } else if (!strcmp(e, "slow_down")) {
        g_login_interval += 5;
    } else {
        g_login = 0;
        if (!strcmp(e, "access_denied"))
            snprintf(g_login_error, sizeof(g_login_error), "%s",
                     TR("The access was not allowed in the Google account", "No se permitió el acceso en la cuenta de Google"));
        else if (!strcmp(e, "expired_token"))
            snprintf(g_login_error, sizeof(g_login_error), "%s",
                     TR("The code expired: press Connect again", "El código caducó: pulsa Conectar otra vez"));
        else
            google_error(&r, g_login_error, sizeof(g_login_error));
    }
    g_login_next = time(NULL) + g_login_interval;
    int done = !g_login && g_refresh[0];
    pthread_mutex_unlock(&g_mu);
    http_resp_free(&r);
    if (done) {
        save_state();
        printf("[drive] signed in\n");
    }
}

/* --- Drive requests --- */

/* A request to Drive with the access token; requests that fail for a passing reason
 * (network, 429, 5xx) are tried again, an ended token is refreshed once. 0 when an answer
 * arrived: its status is the caller's to judge. */
static int api_stall(const char *method, const char *url, const char *ctype, const void *body, size_t len,
                     const char *extra, http_sink_t sink, void *sink_ctx, int stall, http_keep_t *keep, http_resp_t *r,
                     char *err, size_t n) {
    int refreshed = 0;
    for (int t = 0; t < TRIES; t++) {
        if (g_cancel || g_quit) {
            snprintf(err, n, "%s", TR("cancelled", "cancelado"));
            return -1;
        }
        if (access_token(err, n))
            return -1;
        buf_t h = {0};
        pthread_mutex_lock(&g_mu);
        buf_printf(&h, "Authorization: Bearer %s\r\n", g_access);
        pthread_mutex_unlock(&g_mu);
        if (ctype)
            buf_printf(&h, "Content-Type: %s\r\n", ctype);
        if (extra)
            buf_put(&h, extra, strlen(extra));
        http_req_t q = {.method = method, .url = url, .headers = text(&h), .body = body, .body_len = len,
                        .sink = sink, .sink_ctx = sink_ctx, .cancel = &g_cancel, .stall = stall, .keep = keep};
        int rc = http_request(&q, r, err, n);
        buf_free(&h);
        if (!rc && r->status == 401 && !refreshed) {
            http_resp_free(r);
            pthread_mutex_lock(&g_mu);
            g_access[0] = 0;
            pthread_mutex_unlock(&g_mu);
            refreshed = 1;
            t--;
            continue;
        }
        char reason[64] = "";
        if (!rc && r->status == 403)
            error_reason(r, reason, sizeof(reason));
        int rate = !strcmp(reason, "userRateLimitExceeded") || !strcmp(reason, "rateLimitExceeded");
        if (!rc && !transient(r->status) && !rate)
            return 0;
        if (sink)
            return rc ? -1 : 0; /* a body already given to the sink: the caller resumes */
        if (!rc) {
            google_error(r, err, n);
            http_resp_free(r);
        }
        printf("[drive] %s %s: %s (try %d)\n", method, url, err, t + 1);
        if (pause_s(g_drive_retry_seconds << t))
            break;
    }
    if (g_cancel || g_quit)
        snprintf(err, n, "%s", TR("cancelled", "cancelado"));
    return -1;
}

static int api(const char *method, const char *url, const char *ctype, const void *body, size_t len, const char *extra,
               http_sink_t sink, void *sink_ctx, http_resp_t *r, char *err, size_t n) {
    return api_stall(method, url, ctype, body, len, extra, sink, sink_ctx, 0, NULL, r, err, n);
}

/* A JSON request that must answer 200; the parsed answer in r. */
static int api_json(const char *method, const char *url, buf_t *body, http_resp_t *r, char *err, size_t n) {
    if (api(method, url, body ? "application/json; charset=UTF-8" : NULL, body ? body->data : NULL, body ? body->len : 0,
            NULL, NULL, NULL, r, err, n))
        return -1;
    if (r->status != 200) {
        google_error(r, err, n);
        http_resp_free(r);
        return -1;
    }
    return 0;
}

/* GET {api}/files?q=<q>&fields=<fields>[&pageToken=..], one page. */
static int files_list(const char *q, const char *fields, const char *page, http_resp_t *r, char *err, size_t n) {
    buf_t u = {0};
    buf_printf(&u, "%s/files?pageSize=1000&spaces=drive&q=", g_ep.api);
    http_escape(&u, q);
    buf_printf(&u, "&fields=");
    http_escape(&u, fields);
    if (page && page[0]) {
        buf_printf(&u, "&pageToken=");
        http_escape(&u, page);
    }
    int rc = api_json("GET", text(&u), NULL, r, err, n);
    buf_free(&u);
    return rc;
}

/* The id of the folder with appProperties key=value (in parent, or anywhere), made when
 * missing. */
static int folder(const char *key, const char *value, const char *name, const char *parent, char *id, size_t idn,
                  char *err, size_t n) {
    char q[512];
    snprintf(q, sizeof(q), "appProperties has { key='%s' and value='%s' } and trashed=false and mimeType='" FOLDER_MIME "'",
             key, value);
    http_resp_t r;
    if (files_list(q, "files(id)", NULL, &r, err, n))
        return -1;
    jv_t files, f;
    int found = !jv_get(jv_of((const char *)r.body.data, r.body.len), "files", &files) && !jv_index(files, 0, &f) &&
                !jv_get_string(f, "id", id, idn) && id[0];
    http_resp_free(&r);
    if (found)
        return 0;
    buf_t b = {0}, u = {0};
    buf_printf(&b, "{\"name\":");
    json_esc(&b, name);
    buf_printf(&b, ",\"mimeType\":\"" FOLDER_MIME "\",\"appProperties\":{\"%s\":", key);
    json_esc(&b, value);
    buf_printf(&b, "}");
    if (parent) {
        buf_printf(&b, ",\"parents\":[");
        json_esc(&b, parent);
        buf_printf(&b, "]");
    }
    buf_printf(&b, "}");
    buf_printf(&u, "%s/files?fields=id", g_ep.api);
    int rc = api_json("POST", text(&u), &b, &r, err, n);
    buf_free(&b);
    buf_free(&u);
    if (rc)
        return -1;
    rc = jv_get_string(jv_of((const char *)r.body.data, r.body.len), "id", id, idn) || !id[0] ? -1 : 0;
    http_resp_free(&r);
    if (rc)
        snprintf(err, n, "%s", TR("Drive did not make the folder", "Drive no creó la carpeta"));
    else
        printf("[drive] folder %s made\n", name);
    return rc;
}

static int root_folder(char *id, size_t idn, char *err, size_t n) {
    pthread_mutex_lock(&g_mu);
    snprintf(id, idn, "%s", g_root);
    pthread_mutex_unlock(&g_mu);
    if (id[0])
        return 0;
    if (folder("ps4mount", "root", "PS4Mount", NULL, id, idn, err, n))
        return -1;
    pthread_mutex_lock(&g_mu);
    snprintf(g_root, sizeof(g_root), "%s", id);
    pthread_mutex_unlock(&g_mu);
    save_state();
    return 0;
}

/* The pkgs PS4Mount put in Drive for tid. -1 on an error; *count otherwise. */
static int remote_files(const char *tid, remote_file_t *out, int max, int *count, char *err, size_t n) {
    char q[256], page[512] = "";
    snprintf(q, sizeof(q), "appProperties has { key='tid' and value='%s' } and trashed=false", tid);
    *count = 0;
    do {
        http_resp_t r;
        if (files_list(q, "nextPageToken,files(id,name,size,md5Checksum,appProperties)", page, &r, err, n))
            return -1;
        jv_t v = jv_of((const char *)r.body.data, r.body.len), files, f, ap;
        page[0] = 0;
        jv_get_string(v, "nextPageToken", page, sizeof(page));
        for (int i = 0; !jv_get(v, "files", &files) && !jv_index(files, i, &f) && *count < max; i++) {
            remote_file_t *o = &out[*count];
            memset(o, 0, sizeof(*o));
            jv_get_string(f, "id", o->id, sizeof(o->id));
            jv_get_string(f, "name", o->name, sizeof(o->name));
            jv_get_string(f, "md5Checksum", o->md5, sizeof(o->md5));
            o->size = (uint64_t)jv_get_int(f, "size", 0);
            if (!jv_get(f, "appProperties", &ap))
                jv_get_string(ap, "kind", o->kind, sizeof(o->kind));
            if (o->id[0])
                (*count)++;
        }
        http_resp_free(&r);
    } while (page[0]);
    return 0;
}

static int trash_id(const char *id, char *err, size_t n) {
    buf_t u = {0}, b = {0};
    buf_printf(&u, "%s/files/%s?fields=id", g_ep.api, id);
    buf_printf(&b, "{\"trashed\":true}");
    http_resp_t r;
    int rc = api_json("PATCH", text(&u), &b, &r, err, n);
    if (!rc)
        http_resp_free(&r);
    buf_free(&u);
    buf_free(&b);
    return rc;
}

static void md5_hex(br_md5_context *c, char out[33]) {
    uint8_t d[16];
    br_md5_out(c, d);
    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
}

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Bytes a second over the last half minute (0 while unknown). */
static double speed_locked(void) {
    if (g_nsamples < 2)
        return 0;
    double dt = g_samples[g_nsamples - 1].t - g_samples[0].t;
    return dt >= 3 ? (double)(g_samples[g_nsamples - 1].b - g_samples[0].b) / dt : 0;
}

/* "~25 min", "~1 h 40 min". */
static void fmt_left(double seconds, char *out, size_t n) {
    long m = (long)(seconds / 60 + 0.5);
    if (m < 1)
        snprintf(out, n, "%s", TR("less than a minute", "menos de un minuto"));
    else if (m < 60)
        snprintf(out, n, "~%ld min", m);
    else
        snprintf(out, n, "~%ld h %ld min", m / 60, m % 60);
}

static void fmt_size(uint64_t b, char *out, size_t n) {
    /* in units of 1024, as the panel and the console's own screens count */
    if (b >= (1ull << 30))
        snprintf(out, n, "%.1f GB", b / 1073741824.0);
    else
        snprintf(out, n, "%llu MB", (unsigned long long)((b + (1u << 20) - 1) >> 20));
}

static void set_phase(const char *p) {
    pthread_mutex_lock(&g_mu);
    g_phase = p;
    pthread_mutex_unlock(&g_mu);
}

/* A new transfer of all bytes in all. */
static void begin_total(uint64_t all) {
    pthread_mutex_lock(&g_mu);
    g_all = all;
    g_base = g_done = 0;
    g_milestone = 0;
    g_nsamples = 0;
    pthread_mutex_unlock(&g_mu);
}

static void file_done(uint64_t size) {
    pthread_mutex_lock(&g_mu);
    g_base += size;
    g_done = 0;
    pthread_mutex_unlock(&g_mu);
}

/* done: bytes of the current file. A quarter more of the game is told on the console's
 * screen, with the time left: the panel may not be open. */
static void progress(uint64_t done) {
    char note[300] = "";
    pthread_mutex_lock(&g_mu);
    g_done = done;
    uint64_t all = g_base + done;
    double now = mono();
    if (!g_nsamples || now - g_samples[g_nsamples - 1].t >= 1) {
        if (g_nsamples == SAMPLES) {
            memmove(g_samples, g_samples + 1, sizeof(g_samples[0]) * (SAMPLES - 1));
            g_nsamples--;
        }
        g_samples[g_nsamples].t = now;
        g_samples[g_nsamples].b = all;
        g_nsamples++;
    }
    int quarter = g_all ? (int)(all * 4 / g_all) : 0;
    double sp = speed_locked();
    if (quarter > g_milestone && quarter < 4 && sp > 0 && (g_op == OP_UPLOAD || g_op == OP_DOWNLOAD)) {
        g_milestone = quarter;
        char left[48];
        fmt_left((double)(g_all - all) / sp, left, sizeof(left));
        snprintf(note, sizeof(note),
                 g_op == OP_UPLOAD ? TR("%s: %d%% uploaded to Google Drive, %s left", "%s: %d%% subido a Google Drive, quedan %s")
                                   : TR("%s: %d%% downloaded from Google Drive, %s left", "%s: %d%% descargado de Google Drive, quedan %s"),
                 g_cur_name, quarter * 25, left);
    }
    pthread_mutex_unlock(&g_mu);
    if (note[0] && g_host.notify)
        g_host.notify(note);
}

/* --- uploads --- */

/* The session of the upload in progress, kept so a restart goes on with it. */
static int session_load(const drive_file_t *f, char *url, size_t n) {
    FILE *s = fopen(g_session_path, "r");
    if (!s)
        return -1;
    char path[PATH_MAX], size[32];
    int ok = fgets(path, sizeof(path), s) && fgets(size, sizeof(size), s) && fgets(url, (int)n, s);
    fclose(s);
    if (!ok)
        return -1;
    path[strcspn(path, "\n")] = 0;
    url[strcspn(url, "\n")] = 0;
    return !strcmp(path, f->path) && strtoull(size, NULL, 10) == f->size && url[0] ? 0 : -1;
}

static void session_save(const drive_file_t *f, const char *url) {
    FILE *s = fopen(g_session_path, "w");
    if (s) {
        fprintf(s, "%s\n%llu\n%s\n", f->path, (unsigned long long)f->size, url);
        fclose(s);
    }
}

static int session_start(const drive_file_t *f, const char *parent, const char *tid, const char *title, char *url,
                         size_t un, char *err, size_t n) {
    buf_t b = {0}, u = {0}, h = {0};
    char short_title[100];
    /* appProperties: 124 bytes a pair; cut at a whole UTF-8 character */
    size_t tl = strlen(title) < sizeof(short_title) - 1 ? strlen(title) : sizeof(short_title) - 1;
    while (tl && tl < strlen(title) && ((unsigned char)title[tl] & 0xc0) == 0x80)
        tl--;
    memcpy(short_title, title, tl);
    short_title[tl] = 0;
    buf_printf(&b, "{\"name\":");
    json_esc(&b, f->name);
    buf_printf(&b, ",\"parents\":[");
    json_esc(&b, parent);
    buf_printf(&b, "],\"appProperties\":{\"ps4mount\":\"pkg\",\"tid\":\"%s\",\"kind\":\"%s\",\"title\":", tid, f->kind);
    json_esc(&b, short_title);
    buf_printf(&b, "}}");
    buf_printf(&u, "%s/files?uploadType=resumable&fields=id,size,md5Checksum", g_ep.upload);
    buf_printf(&h, "X-Upload-Content-Type: application/octet-stream\r\nX-Upload-Content-Length: %llu\r\n",
               (unsigned long long)f->size);
    http_resp_t r;
    int rc = api("POST", text(&u), "application/json; charset=UTF-8", b.data, b.len, text(&h), NULL, NULL, &r, err, n);
    buf_free(&b);
    buf_free(&u);
    buf_free(&h);
    if (rc)
        return -1;
    char reason[64] = "";
    if (r.status == 403)
        error_reason(&r, reason, sizeof(reason));
    if (!strcmp(reason, "storageQuotaExceeded"))
        snprintf(err, n, "%s", TR("Google Drive is full", "Google Drive está lleno"));
    else if (r.status != 200 || !r.location[0])
        google_error(&r, err, n);
    else
        snprintf(url, un, "%s", r.location);
    rc = r.status == 200 && r.location[0] && strcmp(reason, "storageQuotaExceeded") ? 0 : -1;
    http_resp_free(&r);
    if (!rc)
        session_save(f, url);
    return rc;
}

/* The bytes Drive has of the session (from a 308's Range: bytes=0-N): N + 1, or 0. */
static uint64_t range_next(const http_resp_t *r) {
    const char *d = strchr(r->range, '-');
    return d ? strtoull(d + 1, NULL, 10) + 1 : 0;
}

static void progress(uint64_t done);

/* Hashes fd's bytes [from, to) into c (report: shown as progress, for a long re-read). */
static int hash_range_r(int fd, br_md5_context *c, uint64_t from, uint64_t to, uint8_t *buf, size_t bn, int report) {
    while (from < to) {
        if (report)
            progress(from);
        size_t want = to - from < bn ? (size_t)(to - from) : bn;
        ssize_t r = pread(fd, buf, want, (off_t)from);
        if (r <= 0)
            return -1;
        br_md5_update(c, buf, (size_t)r);
        from += (uint64_t)r;
    }
    return 0;
}


static int upload_file(const drive_file_t *f, const char *parent, const char *tid, const char *title, char *err, size_t n) {
    char url[2048];
    int fd = open(f->path, O_RDONLY);
    uint8_t *buf = mem_alloc(g_drive_chunk);
    if (fd < 0 || !buf) {
        snprintf(err, n, TR("%s cannot be read (%s)", "%s no se puede leer (%s)"), f->path, fd < 0 ? strerror(errno) : "memory");
        if (fd >= 0)
            close(fd);
        mem_free(buf);
        return -1;
    }
    br_md5_context md5;
    br_md5_init(&md5);
    http_keep_t *keep = http_keep_new(); /* the chunks share one connection */
    uint64_t off = 0, hashed = 0;
    int rc = -1, fresh = session_load(f, url, sizeof(url)) != 0, restarts = 0, drop = 0;
    http_resp_t r;
    for (int fails = 0; fails < TRIES;) {
        if (fresh) {
            if (session_start(f, parent, tid, title, url, sizeof(url), err, n))
                break;
            fresh = 0;
            off = 0;
        } else {
            /* where the session is: after a cut, a retry or a restart */
            char cr[64];
            snprintf(cr, sizeof(cr), "Content-Range: bytes */%llu\r\n", (unsigned long long)f->size);
            if (api("PUT", url, NULL, NULL, 0, cr, NULL, NULL, &r, err, n)) {
                fails++;
                continue;
            }
            int st = r.status;
            off = range_next(&r);
            http_resp_free(&r);
            if (st == 200 || st == 201) {
                off = f->size;
                /* finished before the answer was lost: its MD5 is checked below */
            } else if (st == 404 || st == 410) {
                fresh = 1; /* the session ended (a week old): again from the start */
                if (++restarts > 2)
                    break;
                continue;
            } else if (st != 308) {
                snprintf(err, n, TR("Drive answered %d to the upload", "Drive respondió %d a la subida"), st);
                fails++;
                continue;
            }
        }
        if (off < hashed) {
            br_md5_init(&md5); /* Drive has less than was hashed: hash again from 0 */
            hashed = 0;
        }
        int reread = off > hashed;
        if (reread)
            set_phase("checking"); /* the part Drive has, read again for the MD5 */
        if (hash_range_r(fd, &md5, hashed, off, buf, g_drive_chunk, reread)) {
            snprintf(err, n, TR("%s cannot be read", "%s no se puede leer"), f->path);
            break;
        }
        hashed = off;
        if (reread) {
            pthread_mutex_lock(&g_mu);
            g_nsamples = 0; /* a local read: not the upload's speed */
            pthread_mutex_unlock(&g_mu);
        }
        set_phase("sending");
        progress(off);
        int done = 0;
        while (!done) {
            size_t len = f->size - off < g_drive_chunk ? (size_t)(f->size - off) : g_drive_chunk;
            ssize_t got = len ? pread(fd, buf, len, (off_t)off) : 0;
            if (got != (ssize_t)len) {
                snprintf(err, n, TR("%s cannot be read", "%s no se puede leer"), f->path);
                fails = TRIES;
                break;
            }
            char cr[96];
            if (len)
                snprintf(cr, sizeof(cr), "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)off,
                         (unsigned long long)(off + len - 1), (unsigned long long)f->size);
            else
                snprintf(cr, sizeof(cr), "Content-Range: bytes */%llu\r\n", (unsigned long long)f->size);
            if (http_request(&(http_req_t){.method = "PUT", .url = url, .headers = cr, .body = buf, .body_len = len,
                                           .cancel = &g_cancel, .keep = keep},
                             &r, err, n)) {
                fails++;
                break; /* asks the session where it is */
            }
            if (r.status == 308) {
                uint64_t next = range_next(&r);
                http_resp_free(&r);
                if (next != off + len) {
                    fails++;
                    break; /* Drive kept less: ask, resend from there */
                }
                br_md5_update(&md5, buf, len);
                off = hashed = next;
                progress(off);
                fails = 0;
                continue;
            }
            if (r.status == 200 || r.status == 201) {
                br_md5_update(&md5, buf, len);
                off = hashed = f->size;
                progress(off);
                char got_md5[33] = "", want_md5[33], id[128] = "";
                jv_t v = jv_of((const char *)r.body.data, r.body.len);
                jv_get_string(v, "md5Checksum", got_md5, sizeof(got_md5));
                jv_get_string(v, "id", id, sizeof(id));
                http_resp_free(&r);
                md5_hex(&md5, want_md5);
                if (got_md5[0] && strcmp(got_md5, want_md5)) {
                    snprintf(err, n, TR("%s arrived damaged in Drive (MD5 %s, should be %s)", "%s llegó dañado a Drive (MD5 %s, debería ser %s)"),
                             f->name, got_md5, want_md5);
                    char ignored[256];
                    if (id[0])
                        trash_id(id, ignored, sizeof(ignored));
                    fails = TRIES;
                    drop = 1;
                } else
                    rc = 0;
                done = 1;
                break;
            }
            char reason[64] = "";
            if (r.status == 403)
                error_reason(&r, reason, sizeof(reason));
            if (!strcmp(reason, "storageQuotaExceeded")) {
                snprintf(err, n, "%s", TR("Google Drive is full", "Google Drive está lleno"));
                fails = TRIES;
            } else if (r.status == 404 || r.status == 410) {
                fresh = 1;
                fails++;
            } else {
                google_error(&r, err, n);
                fails++;
            }
            http_resp_free(&r);
            break;
        }
        if (done || rc == 0 || g_cancel || g_quit)
            break;
        if (fails && fails < TRIES && pause_s(g_drive_retry_seconds << fails))
            break;
    }
    if (g_cancel || g_quit)
        snprintf(err, n, "%s", TR("cancelled", "cancelado"));
    /* kept after a cut, a cancel or a stop: the next upload of this pkg goes on from there */
    if (!rc || drop)
        unlink(g_session_path);
    http_keep_free(keep);
    close(fd);
    mem_free(buf);
    return rc;
}

static void clean_title(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c))
            continue;
        out[o++] = (char)c;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.'))
        o--;
    out[o] = 0;
    while (out[0] == ' ' || out[0] == '.')
        memmove(out, out + 1, strlen(out));
}

static void run_upload(const char *tid) {
    drive_file_t *files = mem_alloc(sizeof(*files) * DRIVE_MAX_FILES);
    remote_file_t *rem = mem_alloc(sizeof(*rem) * DRIVE_MAX_FILES);
    char title[128] = "", err[512] = "", root[128], dir[128], fname[192];
    int nf = files && rem ? g_host.game_files(tid, files, DRIVE_MAX_FILES, title, sizeof(title)) : 0, nr = 0, rc = -1;
    pthread_mutex_lock(&g_mu);
    snprintf(g_cur_name, sizeof(g_cur_name), "%s", title[0] ? title : tid);
    pthread_mutex_unlock(&g_mu);
    if (!nf) {
        set_result(0, TR("%s is not in the library", "%s no está en la biblioteca"), tid);
        goto out;
    }
    clean_title(title, fname, sizeof(fname));
    char folder_name[256];
    snprintf(folder_name, sizeof(folder_name), "%s [%s]", fname[0] ? fname : tid, tid);
    if (root_folder(root, sizeof(root), err, sizeof(err)) ||
        folder("ps4mount_game", tid, folder_name, root, dir, sizeof(dir), err, sizeof(err)) ||
        remote_files(tid, rem, DRIVE_MAX_FILES, &nr, err, sizeof(err)))
        goto fail;
    /* what Drive already has is skipped; an older pkg with the same name goes to the trash */
    uint8_t have[DRIVE_MAX_FILES] = {0};
    uint64_t all = 0;
    int nsend = 0;
    for (int i = 0; i < nf; i++) {
        for (int k = 0; k < nr; k++) {
            if (strcmp(rem[k].name, files[i].name))
                continue;
            if (rem[k].size == files[i].size)
                have[i] = 1;
            else if (trash_id(rem[k].id, err, sizeof(err)))
                goto fail;
        }
        if (!have[i]) {
            all += files[i].size;
            nsend++;
        }
    }
    begin_total(all);
    char size_text[32];
    fmt_size(all, size_text, sizeof(size_text));
    if (nsend && g_host.notify) {
        char m[300];
        snprintf(m, sizeof(m), TR("Uploading %s to Google Drive (%s)", "Subiendo %s a Google Drive (%s)"), title, size_text);
        g_host.notify(m);
    }
    for (int i = 0, k = 0; i < nf; i++) {
        drive_file_t *f = &files[i];
        if (have[i])
            continue;
        pthread_mutex_lock(&g_mu);
        snprintf(g_cur_file, sizeof(g_cur_file), "%s", f->name);
        g_file_i = ++k;
        g_file_n = nsend;
        g_done = 0;
        g_total = f->size;
        pthread_mutex_unlock(&g_mu);
        printf("[drive] %s: uploading %s (%llu bytes) as %s\n", tid, f->path, (unsigned long long)f->size, f->name);
        if (upload_file(f, dir, tid, title, err, sizeof(err)))
            goto fail;
        file_done(f->size);
    }
    rc = 0;
    if (nsend) {
        char took[48];
        fmt_left((double)(time(NULL) - g_job_start), took, sizeof(took));
        set_result(1, TR("%s is in Google Drive (%s in %s)", "%s está en Google Drive (%s en %s)"), title, size_text,
                   took + (took[0] == '~'));
        if (g_host.notify)
            g_host.notify(g_result);
    } else
        set_result(1, TR("%s was already in Google Drive", "%s ya estaba en Google Drive"), title);
fail:
    if (rc) {
        set_result(0, TR("%s was not uploaded: %s", "%s no se subió: %s"), title, err);
        if (g_host.notify && !g_cancel)
            g_host.notify(g_result);
    }
out:
    mem_free(files);
    mem_free(rem);
}

/* --- downloads --- */

/* The console's network gives one connection about 1.2-1.6 MB/s from the internet whatever
 * the line, and several together add up to a ceiling of about 15 MB/s (a 13.52 PS4 on
 * a 35 MB/s line: 8 connections 10.5, 12 15.0, 16 14.0, 24 14.1): a file comes in blocks,
 * DL_THREADS at a time. Each block arrives in memory and the blocks are written in order:
 * written out of order, exFAT fills the gap before a block with zeros first, which held a
 * download at 0.8 MB/s. In order, the MD5 is made as it goes and a cut download goes on
 * from the end of its part file. */
size_t g_drive_block = 8u << 20;
#define DL_THREADS 12
#define DL_SLOTS (DL_THREADS + 4) /* blocks that may wait in memory for an earlier one */
#define DL_STALL 10

typedef struct {
    const remote_file_t *f;
    int fd;
    uint64_t nblocks, next, written; /* next block to fetch; blocks written, in order */
    uint8_t *buf[DL_SLOTS];
    int ready[DL_SLOTS];
    size_t len[DL_SLOTS];
    uint64_t inflight; /* bytes in memory not written yet */
    br_md5_context md5;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int fails, stop, slots;
    char err[512];
} dl_t;

typedef struct {
    dl_t *d;
    uint8_t *buf;
    size_t at, want;
} piece_t;

static uint64_t dl_have_locked(const dl_t *d) {
    uint64_t w = d->written * g_drive_block;
    return (w < d->f->size ? w : d->f->size) + d->inflight;
}

static int piece_sink(void *ctx, const uint8_t *p, size_t len) {
    piece_t *pc = ctx;
    if (len > pc->want - pc->at)
        return -1; /* more than the range asked: a server that ignored it */
    memcpy(pc->buf + pc->at, p, len);
    pc->at += len;
    dl_t *d = pc->d;
    pthread_mutex_lock(&d->mu);
    d->inflight += len;
    uint64_t h = dl_have_locked(d);
    pthread_mutex_unlock(&d->mu);
    progress(h);
    return g_cancel || g_quit || d->stop;
}

/* Writes the blocks that are next in order (with d->mu held). */
static void dl_flush_locked(dl_t *d) {
    for (;;) {
        int s = (int)(d->written % (uint64_t)d->slots);
        if (d->written >= d->nblocks || !d->ready[s] || d->stop)
            return;
        size_t len = d->len[s], o = 0;
        off_t at = (off_t)(d->written * g_drive_block);
        while (o < len) {
            ssize_t w = pwrite(d->fd, d->buf[s] + o, len - o, at + (off_t)o);
            if (w <= 0) {
                d->stop = 1;
                snprintf(d->err, sizeof(d->err), TR("write failed (%s): is the drive full?", "falló la escritura (%s): ¿está llena la unidad?"),
                         strerror(errno));
                pthread_cond_broadcast(&d->cv);
                return;
            }
            o += (size_t)w;
        }
        br_md5_update(&d->md5, d->buf[s], len);
        d->ready[s] = 0;
        d->inflight -= len;
        d->written++;
        pthread_cond_broadcast(&d->cv);
    }
}

static void *dl_worker(void *arg) {
    dl_t *d = arg;
    /* one connection for all the blocks of this thread: one TLS handshake, not one a block */
    http_keep_t *keep = http_keep_new();
    for (;;) {
        pthread_mutex_lock(&d->mu);
        /* no block further ahead than the slots can hold */
        while (!d->stop && !g_cancel && !g_quit && d->next < d->nblocks && d->next >= d->written + (uint64_t)d->slots) {
            /* a second at a time: a cancel or a stop sets a flag, it does not signal */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&d->cv, &d->mu, &ts);
        }
        if (d->stop || g_cancel || g_quit || d->next >= d->nblocks) {
            pthread_mutex_unlock(&d->mu);
            http_keep_free(keep);
            return NULL;
        }
        uint64_t b = d->next++;
        int s = (int)(b % (uint64_t)d->slots);
        pthread_mutex_unlock(&d->mu);
        uint64_t from = b * g_drive_block, to = from + g_drive_block < d->f->size ? from + g_drive_block : d->f->size;
        piece_t pc = {d, d->buf[s], 0, (size_t)(to - from)};
        char url[512], range[80], err[512] = "";
        snprintf(url, sizeof(url), "%s/files/%s?alt=media", g_ep.api, d->f->id);
        http_resp_t r;
        int ok = 0, gone = 0;
        while (!ok && !d->stop && !g_cancel && !g_quit) {
            snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n", (unsigned long long)(from + pc.at),
                     (unsigned long long)(to - 1));
            size_t before = pc.at;
            /* a connection that stops sending holds every block after it (they are written in
             * order): after DL_STALL seconds without a byte it is opened again from there */
            int q = api_stall("GET", url, NULL, NULL, 0, range, piece_sink, &pc, DL_STALL, keep, &r, err, sizeof(err));
            if (q && pc.at < pc.want && !g_cancel && !g_quit && !d->stop)
                printf("[drive] block %llu: %s at %zu of %zu bytes, opening it again\n", (unsigned long long)b, err, pc.at, pc.want);
            if (!q && r.status != 206 && !(r.status == 200 && from == 0 && to == d->f->size)) {
                gone = r.status == 404;
                if (r.status == 200)
                    snprintf(err, sizeof(err), "%s", TR("Drive sent the whole file instead of a part", "Drive envió el archivo entero en vez de una parte"));
                else
                    google_error(&r, err, sizeof(err));
            }
            if (!q)
                http_resp_free(&r);
            ok = pc.at == pc.want;
            if (ok || g_cancel || g_quit)
                break;
            pthread_mutex_lock(&d->mu);
            /* a piece that moved on is not a failure: the connection dropped, it goes on */
            int fails = pc.at > before ? d->fails : ++d->fails;
            if (gone || fails >= TRIES * 2) {
                d->stop = 1;
                snprintf(d->err, sizeof(d->err), "%s", gone ? TR("the file is no longer in Drive", "el archivo ya no está en Drive") : err);
                pthread_cond_broadcast(&d->cv);
            }
            pthread_mutex_unlock(&d->mu);
            if (!d->stop && pc.at == before && pause_s(g_drive_retry_seconds << (fails < 5 ? fails : 5)))
                break;
        }
        pthread_mutex_lock(&d->mu);
        if (ok) {
            d->len[s] = pc.want;
            d->ready[s] = 1;
            dl_flush_locked(d);
        } else {
            d->inflight -= pc.at;
            d->stop = 1; /* in order: a block that cannot come stops the file (it resumes later) */
            pthread_cond_broadcast(&d->cv);
        }
        pthread_mutex_unlock(&d->mu);
    }
}

static int download_file(const remote_file_t *f, const char *part, char *err, size_t n) {
    dl_t d;
    memset(&d, 0, sizeof(d));
    d.f = f;
    d.nblocks = f->size ? (f->size + g_drive_block - 1) / g_drive_block : 0;
    d.fd = open(part, O_RDWR | O_CREAT, 0666);
    if (d.fd < 0) {
        snprintf(err, n, TR("cannot write %s (%s)", "no se puede escribir %s (%s)"), part, strerror(errno));
        return -1;
    }
    /* as many slots as memory gives, at least one a connection plus one */
    for (d.slots = 0; d.slots < DL_SLOTS && d.slots < (int)d.nblocks + 1; d.slots++)
        if (!(d.buf[d.slots] = mem_alloc(g_drive_block)))
            break;
    if (d.nblocks && d.slots < 2) {
        for (int i = 0; i < d.slots; i++)
            mem_free(d.buf[i]);
        close(d.fd);
        snprintf(err, n, "%s", TR("no memory for the download", "no hay memoria para la descarga"));
        return -1;
    }
    pthread_mutex_init(&d.mu, NULL);
    pthread_cond_init(&d.cv, NULL);
    br_md5_init(&d.md5);
    int rc = 0;
    /* a cut download: its whole blocks stay, read again for the MD5 */
    struct stat st;
    uint64_t keep = !fstat(d.fd, &st) ? (uint64_t)st.st_size / g_drive_block : 0;
    if (keep > d.nblocks)
        keep = 0;
    if (ftruncate(d.fd, (off_t)(keep * g_drive_block)))
        keep = 0;
    if (keep) {
        set_phase("checking");
        if (hash_range_r(d.fd, &d.md5, 0, keep * g_drive_block, d.buf[0], g_drive_block, 1)) {
            br_md5_init(&d.md5);
            keep = 0;
            if (ftruncate(d.fd, 0)) {
            }
        }
        pthread_mutex_lock(&g_mu);
        g_nsamples = 0; /* a local read: not the download's speed */
        pthread_mutex_unlock(&g_mu);
        set_phase("receiving");
    }
    d.written = d.next = keep;
    progress(dl_have_locked(&d));
    pthread_t th[DL_THREADS];
    int nth = 0, want = d.slots - 1 < DL_THREADS ? d.slots - 1 : DL_THREADS;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 << 10);
    for (int i = 0; i < want && (uint64_t)i < d.nblocks - d.written; i++)
        if (!pthread_create(&th[nth], &at, dl_worker, &d))
            nth++;
    pthread_attr_destroy(&at);
    if (!nth && d.written < d.nblocks)
        dl_worker(&d);
    for (int i = 0; i < nth; i++)
        pthread_join(th[i], NULL);
    if (d.written < d.nblocks)
        rc = -1;
    if (g_cancel || g_quit)
        snprintf(err, n, "%s", TR("cancelled", "cancelado"));
    else if (rc)
        snprintf(err, n, "%s", d.err[0] ? d.err : TR("the download stopped", "la descarga se detuvo"));
    if (!rc && fstat(d.fd, &st) == 0 && (uint64_t)st.st_size != f->size) {
        snprintf(err, n, TR("%s arrived with %llu of %llu bytes", "%s llegó con %llu de %llu bytes"), f->name,
                 (unsigned long long)st.st_size, (unsigned long long)f->size);
        rc = -1;
    }
    if (!rc && f->md5[0]) {
        char got[33];
        md5_hex(&d.md5, got);
        if (strcmp(got, f->md5)) {
            snprintf(err, n, TR("%s arrived damaged (MD5 %s, should be %s)", "%s llegó dañado (MD5 %s, debería ser %s)"),
                     f->name, got, f->md5);
            if (ftruncate(d.fd, 0)) {
            }
            rc = -2;
        }
    }
    if (close(d.fd) && !rc) {
        snprintf(err, n, TR("cannot write %s (%s)", "no se puede escribir %s (%s)"), part, strerror(errno));
        rc = -1;
    }
    for (int i = 0; i < d.slots; i++)
        mem_free(d.buf[i]);
    pthread_cond_destroy(&d.cv);
    pthread_mutex_destroy(&d.mu);
    return rc;
}

static void free_path(const char *dir, const char *name, char *out, size_t n) {
    struct stat st;
    snprintf(out, n, "%.3800s/%.90s", dir, name);
    size_t l = strlen(name) > 4 ? strlen(name) - 4 : strlen(name);
    for (int i = 1; !stat(out, &st) && i < 1000; i++)
        snprintf(out, n, "%.3800s/%.*s (%d).pkg", dir, (int)l, name, i);
}

static void run_download(const char *tid, const char *dest) {
    remote_file_t *rem = mem_alloc(sizeof(*rem) * DRIVE_MAX_FILES);
    drive_file_t *local = mem_alloc(sizeof(*local) * 1);
    char err[512] = "", scan[PATH_MAX], title[128] = "", fname[192];
    int nr = 0, rc = -1;
    for (int i = 0; i < g_nremote; i++)
        if (!strcmp(g_remote[i].tid, tid))
            snprintf(title, sizeof(title), "%s", g_remote[i].name);
    if (!title[0])
        snprintf(title, sizeof(title), "%s", tid);
    pthread_mutex_lock(&g_mu);
    snprintf(g_cur_name, sizeof(g_cur_name), "%s", title);
    pthread_mutex_unlock(&g_mu);
    if (!rem || !local) {
        snprintf(err, sizeof(err), "no memory");
        goto fail;
    }
    char ignored[128];
    if (g_host.game_files(tid, local, 1, ignored, sizeof(ignored))) {
        snprintf(err, sizeof(err), "%s", TR("it is already in the library", "ya está en la biblioteca"));
        goto fail;
    }
    if (g_host.dest_dir(dest, scan, sizeof(scan))) {
        snprintf(err, sizeof(err), TR("%s is not available", "%s no está disponible"), dest);
        goto fail;
    }
    if (remote_files(tid, rem, DRIVE_MAX_FILES, &nr, err, sizeof(err)))
        goto fail;
    if (!nr) {
        snprintf(err, sizeof(err), "%s", TR("it has no pkgs in Drive", "no tiene pkgs en Drive"));
        goto fail;
    }
    char parts[PATH_MAX], final_dir[PATH_MAX], part[PATH_MAX];
    snprintf(parts, sizeof(parts), "%.4000s/" PART_DIR, scan);
    mkdir(scan, 0777);
    mkdir(parts, 0777);
    /* the space it needs: all of it (part files of a cut download are already counted
     * as used, so a resumed one may ask for more than is left to get) */
    uint64_t need = SPACE_MARGIN;
    for (int i = 0; i < nr; i++) {
        struct stat st;
        snprintf(part, sizeof(part), "%.3900s/%.127s" PART_EXT, parts, rem[i].id);
        uint64_t held = !stat(part, &st) ? (uint64_t)st.st_blocks * 512 : 0;
        need += rem[i].size > held ? rem[i].size - held : 0;
    }
    uint64_t fb = g_host.free_bytes(scan);
    if (fb < need) {
        snprintf(err, sizeof(err), TR("not enough space: %llu MB free, %llu MB needed", "no hay espacio: %llu MB libres, %llu MB necesarios"),
                 (unsigned long long)(fb >> 20), (unsigned long long)(need >> 20));
        goto fail;
    }
    uint64_t all = 0;
    for (int i = 0; i < nr; i++)
        all += rem[i].size;
    begin_total(all);
    set_phase("receiving");
    char size_text[32];
    fmt_size(all, size_text, sizeof(size_text));
    if (g_host.notify) {
        char m[300];
        snprintf(m, sizeof(m), TR("Downloading %s from Google Drive (%s)", "Descargando %s de Google Drive (%s)"), title, size_text);
        g_host.notify(m);
    }
    clean_title(title, fname, sizeof(fname));
    snprintf(final_dir, sizeof(final_dir), "%.3800s/%.191s [%s]", scan, fname[0] ? fname : tid, tid);
    /* Every pkg first comes whole into the hidden part folder; then all of them go to the
     * game's folder at once. One at a time, the scan added the game while its update was
     * still on the way, and had to add it again when it came (the add-on install that
     * re-add asks for can stall, and a registration during the download slowed it). */
    char staged[PATH_MAX];
    snprintf(staged, sizeof(staged), "%.3800s/.%.191s [%s]", scan, fname[0] ? fname : tid, tid);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nr; i++) {
            if ((pass == 0) != !strcmp(rem[i].kind, "game"))
                continue;
            pthread_mutex_lock(&g_mu);
            snprintf(g_cur_file, sizeof(g_cur_file), "%s", rem[i].name);
            g_file_i++;
            g_file_n = nr;
            g_done = 0;
            g_total = rem[i].size;
            pthread_mutex_unlock(&g_mu);
            snprintf(part, sizeof(part), "%.3900s/%.127s" PART_EXT, parts, rem[i].id);
            /* already here whole (a download asked for twice, before the scan took it) */
            char here[PATH_MAX];
            struct stat hs;
            snprintf(here, sizeof(here), "%.3800s/%.90s", final_dir, rem[i].name);
            if (!stat(here, &hs) && (uint64_t)hs.st_size == rem[i].size) {
                printf("[drive] %s: %s is already in %s\n", tid, rem[i].name, final_dir);
                file_done(rem[i].size);
                continue;
            }
            printf("[drive] %s: downloading %s (%llu bytes) into %s\n", tid, rem[i].name, (unsigned long long)rem[i].size, part);
            {
                /* the old kind of part file of this pkg, if any, is not a start */
                char old[PATH_MAX + 8];
                snprintf(old, sizeof(old), "%.3900s/%.127s.part", parts, rem[i].id);
                unlink(old);
                snprintf(old, sizeof(old), "%.3900s/%.127s.part.map", parts, rem[i].id);
                unlink(old);
            }
            int drc = download_file(&rem[i], part, err, sizeof(err));
            if (drc == -2) {
                /* damaged (the part file emptied): once more from the start, on its own */
                printf("[drive] %s: %s; downloading it again\n", tid, err);
                drc = download_file(&rem[i], part, err, sizeof(err));
            }
            if (drc)
                goto fail;
            file_done(rem[i].size);
            set_phase("receiving");
        }
    /* all of them whole: into a hidden folder (the scan skips hidden ones), which then
     * takes the game folder's name in one rename */
    mkdir(staged, 0777);
    for (int i = 0; i < nr; i++) {
        char here[PATH_MAX], dst[PATH_MAX];
        struct stat hs;
        snprintf(here, sizeof(here), "%.3800s/%.90s", final_dir, rem[i].name);
        if (!stat(here, &hs) && (uint64_t)hs.st_size == rem[i].size)
            continue; /* already in the game's folder */
        snprintf(part, sizeof(part), "%.3900s/%.127s" PART_EXT, parts, rem[i].id);
        free_path(staged, rem[i].name, dst, sizeof(dst));
        if (rename(part, dst)) {
            snprintf(err, sizeof(err), TR("%s could not be renamed (%s)", "%s no se pudo renombrar (%s)"), part, strerror(errno));
            goto fail;
        }
    }
    if (rename(staged, final_dir)) {
        /* the game's folder is there already (a download asked for again): file by file */
        DIR *sd = opendir(staged);
        struct dirent *e;
        while (sd && (e = readdir(sd))) {
            char from[PATH_MAX], dst[PATH_MAX];
            if (e->d_name[0] == '.')
                continue;
            snprintf(from, sizeof(from), "%.3800s/%.250s", staged, e->d_name);
            free_path(final_dir, e->d_name, dst, sizeof(dst));
            if (rename(from, dst)) {
                snprintf(err, sizeof(err), TR("%s could not be renamed (%s)", "%s no se pudo renombrar (%s)"), from, strerror(errno));
                closedir(sd);
                goto fail;
            }
        }
        if (sd)
            closedir(sd);
        rmdir(staged);
    }
    rmdir(parts);
    rc = 0;
    char took[48];
    fmt_left((double)(time(NULL) - g_job_start), took, sizeof(took));
    set_result(1, TR("%s was downloaded from Google Drive (%s in %s)", "%s se descargó de Google Drive (%s en %s)"), title,
               size_text, took + (took[0] == '~'));
    if (g_host.notify)
        g_host.notify(g_result);
    if (g_host.downloaded)
        g_host.downloaded(tid);
fail:
    if (rc) {
        set_result(0, TR("%s was not downloaded: %s", "%s no se descargó: %s"), title, err);
        if (g_host.notify && !g_cancel)
            g_host.notify(g_result);
    }
    mem_free(rem);
    mem_free(local);
}

static void run_trash(const char *tid) {
    remote_file_t *rem = mem_alloc(sizeof(*rem) * DRIVE_MAX_FILES);
    char err[512] = "", q[256], title[128] = "";
    int nr = 0, rc = -1;
    for (int i = 0; i < g_nremote; i++)
        if (!strcmp(g_remote[i].tid, tid))
            snprintf(title, sizeof(title), "%s", g_remote[i].name);
    if (!title[0])
        snprintf(title, sizeof(title), "%s", tid);
    pthread_mutex_lock(&g_mu);
    snprintf(g_cur_name, sizeof(g_cur_name), "%s", title);
    pthread_mutex_unlock(&g_mu);
    if (!rem || remote_files(tid, rem, DRIVE_MAX_FILES, &nr, err, sizeof(err)))
        goto out;
    for (int i = 0; i < nr; i++)
        if (trash_id(rem[i].id, err, sizeof(err)))
            goto out;
    /* the game's folder too */
    snprintf(q, sizeof(q), "appProperties has { key='ps4mount_game' and value='%s' } and trashed=false", tid);
    http_resp_t r;
    if (files_list(q, "files(id)", NULL, &r, err, sizeof(err)))
        goto out;
    jv_t files, f;
    char id[128];
    for (int i = 0; !jv_get(jv_of((const char *)r.body.data, r.body.len), "files", &files) && !jv_index(files, i, &f); i++)
        if (!jv_get_string(f, "id", id, sizeof(id)))
            trash_id(id, err, sizeof(err));
    http_resp_free(&r);
    rc = 0;
out:
    if (rc)
        set_result(0, TR("%s was not deleted from Drive: %s", "%s no se borró de Drive: %s"), title, err);
    else
        set_result(1, TR("%s was moved to the Drive trash", "%s se movió a la papelera de Drive"), title);
    mem_free(rem);
}

/* The account, its space and the games in Drive. */
static void run_list(void) {
    char err[512] = "", url[512], page[512] = "";
    http_resp_t r;
    snprintf(url, sizeof(url), "%s/about?fields=user(emailAddress),storageQuota(limit,usage)", g_ep.api);
    if (api_json("GET", url, NULL, &r, err, sizeof(err)))
        goto fail;
    jv_t v = jv_of((const char *)r.body.data, r.body.len), u, sq;
    pthread_mutex_lock(&g_mu);
    if (!jv_get(v, "user", &u))
        jv_get_string(u, "emailAddress", g_email, sizeof(g_email));
    if (!jv_get(v, "storageQuota", &sq)) {
        g_quota_limit = jv_get_int(sq, "limit", -1);
        g_quota_used = jv_get_int(sq, "usage", -1);
    }
    pthread_mutex_unlock(&g_mu);
    http_resp_free(&r);
    save_state();
    remote_game_t *games = mem_alloc(sizeof(*games) * MAX_REMOTE);
    int ng = 0;
    if (!games) {
        snprintf(err, sizeof(err), "no memory");
        goto fail;
    }
    do {
        if (files_list("appProperties has { key='ps4mount' and value='pkg' } and trashed=false",
                       "nextPageToken,files(size,appProperties)", page, &r, err, sizeof(err))) {
            mem_free(games);
            goto fail;
        }
        jv_t files, f, ap;
        v = jv_of((const char *)r.body.data, r.body.len);
        page[0] = 0;
        jv_get_string(v, "nextPageToken", page, sizeof(page));
        for (int i = 0; !jv_get(v, "files", &files) && !jv_index(files, i, &f); i++) {
            char tid[16] = "", kind[16] = "", title[128] = "";
            if (jv_get(f, "appProperties", &ap) || jv_get_string(ap, "tid", tid, sizeof(tid)) || strlen(tid) != 9)
                continue;
            jv_get_string(ap, "kind", kind, sizeof(kind));
            jv_get_string(ap, "title", title, sizeof(title));
            int k = 0;
            while (k < ng && strcmp(games[k].tid, tid))
                k++;
            if (k == ng) {
                if (ng == MAX_REMOTE)
                    continue;
                memset(&games[ng], 0, sizeof(games[0]));
                snprintf(games[ng].tid, sizeof(games[0].tid), "%.9s", tid);
                ng++;
            }
            remote_game_t *g = &games[k];
            if (title[0] && (!g->name[0] || !strcmp(kind, "game")))
                snprintf(g->name, sizeof(g->name), "%s", title);
            g->size += (uint64_t)jv_get_int(f, "size", 0);
            g->files++;
            g->update |= !strcmp(kind, "update");
            g->addons += !strcmp(kind, "addon");
        }
        http_resp_free(&r);
    } while (page[0]);
    pthread_mutex_lock(&g_mu);
    mem_free(g_remote);
    g_remote = games;
    g_nremote = ng;
    g_listed = 1;
    g_list_error[0] = 0;
    pthread_mutex_unlock(&g_mu);
    printf("[drive] %s: %d game(s) in Drive\n", g_email, ng);
    return;
fail:
    pthread_mutex_lock(&g_mu);
    snprintf(g_list_error, sizeof(g_list_error), "%.255s", err);
    pthread_mutex_unlock(&g_mu);
    printf("[drive] list: %s\n", err);
}

/* --- the thread --- */

/* The transfers of the queue (the running one first) to <state>/drive_queue.txt, so the
 * next PS4Mount goes on with them: lines "upload CUSAxxxxx" and "download CUSAxxxxx usb0". */
static void save_queue_locked(void) {
    buf_t b = {0};
    if (g_busy && (g_op == OP_UPLOAD || g_op == OP_DOWNLOAD))
        buf_printf(&b, "%s %s %s\n", k_op_names[g_op], g_cur_tid, g_op == OP_DOWNLOAD ? g_cur_dest : "-");
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i].op == OP_UPLOAD || g_jobs[i].op == OP_DOWNLOAD)
            buf_printf(&b, "%s %s %s\n", k_op_names[g_jobs[i].op], g_jobs[i].tid,
                       g_jobs[i].op == OP_DOWNLOAD ? g_jobs[i].dest : "-");
    if (!b.len)
        unlink(g_queue_path);
    else {
        int fd = open(g_queue_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            (void)!write(fd, b.data, b.len);
            close(fd);
        }
    }
    buf_free(&b);
}

static int valid_tid(const char *t) {
    if (!is_game_id(t))
        return 0;
    for (int i = 4; i < 9; i++)
        if (t[i] < '0' || t[i] > '9')
            return 0;
    return 1;
}

static int valid_dest(const char *d) {
    if (!d[0] || strlen(d) > 12)
        return 0;
    for (; *d; d++)
        if (!((*d >= 'a' && *d <= 'z') || (*d >= '0' && *d <= '9')))
            return 0;
    return 1;
}

/* What the last PS4Mount left in its queue, back in this one's (with g_mu held). */
static int load_queue_locked(void) {
    FILE *f = fopen(g_queue_path, "r");
    char line[128], op[16], tid[16], dest[32];
    int n = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%15s %15s %31s", op, tid, dest) != 3 || !valid_tid(tid))
            continue;
        if (!strcmp(op, "upload")) {
            queue_locked(OP_UPLOAD, tid, "");
            n++;
        } else if (!strcmp(op, "download") && valid_dest(dest)) {
            queue_locked(OP_DOWNLOAD, tid, dest);
            n++;
        }
    }
    if (f)
        fclose(f);
    return n;
}

static void queue_locked(int op, const char *tid, const char *dest) {
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i].op == op && !strcmp(g_jobs[i].tid, tid) && !strcmp(g_jobs[i].dest, dest))
            return; /* already waiting */
    if (tid[0] && g_busy && g_op == op && !strcmp(g_cur_tid, tid) && op != OP_LIST)
        return; /* that one is running now (a second press of the same button) */
    if (g_njobs == MAX_JOBS)
        return;
    job_t *j = &g_jobs[g_njobs++];
    j->op = op;
    snprintf(j->tid, sizeof(j->tid), "%s", tid);
    snprintf(j->dest, sizeof(j->dest), "%s", dest);
    if (op == OP_UPLOAD || op == OP_DOWNLOAD)
        save_queue_locked();
    pthread_cond_signal(&g_cv);
}

/* Before everything waiting (no duplicate of it stays behind). */
static void queue_front_locked(int op) {
    int k = 0;
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i].op != op)
            g_jobs[k++] = g_jobs[i];
    g_njobs = k < MAX_JOBS ? k : MAX_JOBS - 1;
    memmove(g_jobs + 1, g_jobs, sizeof(g_jobs[0]) * (size_t)g_njobs);
    memset(&g_jobs[0], 0, sizeof(g_jobs[0]));
    g_jobs[0].op = op;
    g_njobs++;
    pthread_cond_signal(&g_cv);
}

static void revoke_token(const char *token) {
    buf_t form = {0};
    form_add(&form, "token", token);
    http_resp_t r;
    char err[256];
    if (!oauth_post("/revoke", &form, &r, err, sizeof(err))) {
        printf("[drive] access revoked (%d)\n", r.status);
        http_resp_free(&r);
    } else
        printf("[drive] revoke: %s\n", err);
    buf_free(&form);
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    while (!g_quit) {
        if (g_revoke[0]) {
            char t[512];
            snprintf(t, sizeof(t), "%s", g_revoke);
            g_revoke[0] = 0;
            pthread_mutex_unlock(&g_mu);
            revoke_token(t);
            pthread_mutex_lock(&g_mu);
            continue;
        }
        if (g_login && time(NULL) >= g_login_next) {
            pthread_mutex_unlock(&g_mu);
            login_poll();
            pthread_mutex_lock(&g_mu);
            continue;
        }
        /* while a code is shown, only the sign-in itself runs */
        if (!g_njobs || (g_login && g_jobs[0].op != OP_LOGIN)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
            continue;
        }
        job_t j = g_jobs[0];
        memmove(g_jobs, g_jobs + 1, sizeof(g_jobs[0]) * (size_t)--g_njobs);
        g_busy = 1;
        g_op = j.op;
        g_cancel = 0;
        snprintf(g_cur_tid, sizeof(g_cur_tid), "%s", j.tid);
        snprintf(g_cur_dest, sizeof(g_cur_dest), "%s", j.dest);
        g_cur_name[0] = g_cur_file[0] = 0;
        g_done = g_total = 0;
        g_file_i = g_file_n = 0;
        g_base = g_all = 0;
        g_nsamples = 0;
        g_milestone = 0;
        g_retry_until = 0;
        g_job_start = time(NULL);
        g_phase = j.op == OP_UPLOAD ? "sending" : j.op == OP_DOWNLOAD ? "receiving" : "";
        int list_after = j.op == OP_UPLOAD || j.op == OP_DOWNLOAD || j.op == OP_TRASH;
        pthread_mutex_unlock(&g_mu);
        switch (j.op) {
        case OP_LOGIN: login_start(); break;
        case OP_LIST: run_list(); break;
        case OP_UPLOAD: run_upload(j.tid); break;
        case OP_DOWNLOAD: run_download(j.tid, j.dest); break;
        case OP_TRASH: run_trash(j.tid); break;
        }
        pthread_mutex_lock(&g_mu);
        if (g_expired && j.op != OP_LOGIN) {
            /* cut by the end of the access: again once signed in (an upload goes on from
             * its session) */
            g_expired = 0;
            if (g_njobs < MAX_JOBS && j.op != OP_LIST) {
                int at = g_njobs && g_jobs[0].op == OP_LOGIN ? 1 : 0;
                memmove(g_jobs + at + 1, g_jobs + at, sizeof(g_jobs[0]) * (size_t)(g_njobs - at));
                g_jobs[at] = j;
                g_njobs++;
            }
        }
        int more = 0;
        for (int i = 0; i < g_njobs; i++)
            more |= g_jobs[i].op != OP_LOGIN && g_jobs[i].op != OP_LIST;
        /* the list once a batch is over: the panel shows what Drive has now */
        if (list_after && !more && g_refresh[0] && !g_quit)
            queue_locked(OP_LIST, "", "");
        g_busy = 0;
        g_cancel = 0;
        /* done, failed for good or cancelled: out of the saved queue; cut by PS4Mount
         * stopping: it stays there, for the next one */
        if (!g_quit && (j.op == OP_UPLOAD || j.op == OP_DOWNLOAD))
            save_queue_locked();
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static void load_client(void) {
    FILE *f = fopen(g_client_path, "r");
    char line[512];
    g_client_id[0] = g_client_secret[0] = 0;
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "client_id=", 10))
            snprintf(g_client_id, sizeof(g_client_id), "%.255s", line + 10);
        else if (!strncmp(line, "client_secret=", 14))
            snprintf(g_client_secret, sizeof(g_client_secret), "%.127s", line + 14);
    }
    if (f)
        fclose(f);
}

void drive_start(const char *state_dir, const drive_endpoints_t *ep, const drive_host_t *host) {
    snprintf(g_ep_oauth, sizeof(g_ep_oauth), "%s", ep && ep->oauth ? ep->oauth : "https://oauth2.googleapis.com");
    snprintf(g_ep_api, sizeof(g_ep_api), "%s", ep && ep->api ? ep->api : "https://www.googleapis.com/drive/v3");
    snprintf(g_ep_upload, sizeof(g_ep_upload), "%s",
             ep && ep->upload ? ep->upload : "https://www.googleapis.com/upload/drive/v3");
    g_ep.oauth = g_ep_oauth;
    g_ep.api = g_ep_api;
    g_ep.upload = g_ep_upload;
    g_host = *host;
    snprintf(g_state_path, sizeof(g_state_path), "%s/drive.txt", state_dir);
    snprintf(g_session_path, sizeof(g_session_path), "%s/drive_upload.txt", state_dir);
    snprintf(g_client_path, sizeof(g_client_path), "%s/drive_client.txt", state_dir);
    snprintf(g_queue_path, sizeof(g_queue_path), "%s/drive_queue.txt", state_dir);
    if (g_started)
        return;
    pthread_mutex_lock(&g_mu);
    load_client();
    if (g_client_id[0])
        load_state();
    pthread_mutex_unlock(&g_mu);
    g_quit = 0;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    if (!pthread_create(&g_thread, &at, worker, NULL)) {
        g_started = 1;
        pthread_mutex_lock(&g_mu);
        int resumed = 0;
        if (g_refresh[0]) {
            queue_locked(OP_LIST, "", "");
            resumed = load_queue_locked();
        }
        if (resumed) {
            printf("[drive] %d transfer(s) left by the last run: going on with them\n", resumed);
            if (g_host.notify) {
                char m[200];
                snprintf(m, sizeof(m), TR("Google Drive: going on with %d transfer(s) left unfinished",
                                          "Google Drive: continuando %d transferencia(s) que quedaron a medias"),
                         resumed);
                pthread_mutex_unlock(&g_mu);
                g_host.notify(m);
                pthread_mutex_lock(&g_mu);
            }
        }
        pthread_mutex_unlock(&g_mu);
    }
    pthread_attr_destroy(&at);
}

void drive_stop(void) {
    if (!g_started)
        return;
    pthread_mutex_lock(&g_mu);
    g_quit = 1;
    g_cancel = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_thread, NULL);
    g_started = 0;
    pthread_mutex_lock(&g_mu);
    g_njobs = 0;
    g_login = 0;
    mem_free(g_remote);
    g_remote = NULL;
    g_nremote = 0;
    g_listed = 0;
    /* as a new start: the account is read again from the state file */
    g_access[0] = g_refresh[0] = g_email[0] = g_root[0] = 0;
    g_access_until = 0;
    g_quota_limit = g_quota_used = -1;
    pthread_mutex_unlock(&g_mu);
}

int drive_active(void) {
    pthread_mutex_lock(&g_mu);
    int a = g_busy && (g_op == OP_UPLOAD || g_op == OP_DOWNLOAD);
    for (int i = 0; i < g_njobs && !a; i++)
        a = g_jobs[i].op == OP_UPLOAD || g_jobs[i].op == OP_DOWNLOAD;
    pthread_mutex_unlock(&g_mu);
    return a;
}

void drive_status_json(buf_t *out) {
    pthread_mutex_lock(&g_mu);
    time_t now = time(NULL);
    /* the client id, never its secret */
    buf_printf(out, "{\"configured\":%s,\"client\":%s,\"client_id\":", g_started ? "true" : "false",
               g_client_id[0] ? "true" : "false");
    json_esc(out, g_client_id);
    buf_printf(out, ",\"connected\":%s,\"email\":", g_refresh[0] ? "true" : "false");
    json_esc(out, g_email);
    buf_printf(out, ",\"quota_limit\":%lld,\"quota_used\":%lld,\"login\":{\"active\":%s,\"code\":", g_quota_limit,
               g_quota_used, g_login ? "true" : "false");
    json_esc(out, g_login ? g_user_code : "");
    buf_printf(out, ",\"url\":");
    json_esc(out, g_login ? g_verify_url : "");
    buf_printf(out, ",\"expires\":%lld,\"error\":", (long long)(g_login && g_login_until > now ? g_login_until - now : 0));
    json_esc(out, g_login_error);
    buf_printf(out, "},\"job\":{\"busy\":%s,\"op\":\"%s\",\"title_id\":\"%s\",\"name\":", g_busy ? "true" : "false",
               g_busy ? k_op_names[g_op] : "", g_busy ? g_cur_tid : "");
    json_esc(out, g_busy ? g_cur_name : "");
    buf_printf(out, ",\"file\":");
    json_esc(out, g_busy ? g_cur_file : "");
    buf_printf(out, ",\"file_index\":%d,\"files\":%d,\"done\":%llu,\"total\":%llu", g_file_i, g_file_n,
               (unsigned long long)g_done, (unsigned long long)g_total);
    /* the whole game, its speed (bytes a second, 0 while unknown) and the phase: sending,
     * receiving, checking (MD5), retrying (retry_in seconds), login (a new code is waited for) */
    long retry_in = g_busy && g_retry_until > now ? (long)(g_retry_until - now) : 0;
    const char *phase = !g_busy ? "" : g_login ? "login" : retry_in ? "retrying" : g_phase;
    buf_printf(out, ",\"game_done\":%llu,\"game_total\":%llu,\"speed\":%.0f,\"elapsed\":%lld,\"retry_in\":%ld,\"phase\":\"%s\",\"queued\":[",
               (unsigned long long)(g_base + g_done), (unsigned long long)g_all, g_busy ? speed_locked() : 0.0,
               (long long)(g_busy ? now - g_job_start : 0), retry_in, phase);
    int first = 1;
    for (int i = 0; i < g_njobs; i++) {
        if (g_jobs[i].op != OP_UPLOAD && g_jobs[i].op != OP_DOWNLOAD && g_jobs[i].op != OP_TRASH)
            continue;
        buf_printf(out, "%s{\"op\":\"%s\",\"title_id\":\"%s\"}", first ? "" : ",", k_op_names[g_jobs[i].op], g_jobs[i].tid);
        first = 0;
    }
    buf_printf(out, "],\"result\":");
    json_esc(out, g_result);
    buf_printf(out, ",\"ok\":%s},\"listed\":%s,\"list_error\":", g_result_ok ? "true" : "false", g_listed ? "true" : "false");
    json_esc(out, g_list_error);
    buf_printf(out, ",\"games\":[");
    for (int i = 0; i < g_nremote; i++) {
        const remote_game_t *g = &g_remote[i];
        buf_printf(out, "%s{\"title_id\":\"%s\",\"name\":", i ? "," : "", g->tid);
        json_esc(out, g->name);
        buf_printf(out, ",\"size\":%llu,\"files\":%d,\"update\":%s,\"addons\":%d}", (unsigned long long)g->size, g->files,
                   g->update ? "true" : "false", g->addons);
    }
    buf_printf(out, "]}");
    pthread_mutex_unlock(&g_mu);
}

/* --- panel actions --- */

static int ready(char *msg, size_t n, int need_account) {
    if (!g_started) {
        snprintf(msg, n, "%s", TR("Google Drive is not running", "Google Drive no está en marcha"));
        return -1;
    }
    if (!g_client_id[0]) {
        snprintf(msg, n, "%s", TR("Enter your Google client ID and secret first", "Pon primero tu ID de cliente y tu secreto de Google"));
        return -1;
    }
    if (need_account && !g_refresh[0]) {
        snprintf(msg, n, "%s", TR("Connect Google Drive first", "Conecta Google Drive primero"));
        return -1;
    }
    return 0;
}

int drive_connect(char *msg, size_t n) {
    if (ready(msg, n, 0))
        return -1;
    pthread_mutex_lock(&g_mu);
    g_login = 0;
    g_login_error[0] = 0;
    queue_locked(OP_LOGIN, "", "");
    pthread_mutex_unlock(&g_mu);
    snprintf(msg, n, "%s", TR("Asking Google for a code", "Pidiendo un código a Google"));
    return 0;
}

int drive_disconnect(char *msg, size_t n) {
    if (ready(msg, n, 0))
        return -1;
    pthread_mutex_lock(&g_mu);
    g_cancel = 1;
    g_njobs = 0;
    g_login = 0;
    snprintf(g_revoke, sizeof(g_revoke), "%s", g_refresh);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    disconnected(NULL);
    unlink(g_session_path);
    unlink(g_queue_path);
    snprintf(msg, n, "%s", TR("Google Drive disconnected", "Google Drive desconectado"));
    set_result(1, "%s", msg);
    return 0;
}

int drive_upload(const char *tid, char *msg, size_t n) {
    if (ready(msg, n, 1))
        return -1;
    char (*tids)[10] = NULL;
    int count = 1;
    if (!tid) {
        tids = mem_alloc(sizeof(*tids) * MAX_JOBS);
        count = tids ? g_host.library(tids, MAX_JOBS) : 0;
        if (!count) {
            mem_free(tids);
            snprintf(msg, n, "%s", TR("There are no games in the library", "No hay juegos en la biblioteca"));
            return -1;
        }
    }
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < count; i++)
        queue_locked(OP_UPLOAD, tid ? tid : tids[i], "");
    pthread_mutex_unlock(&g_mu);
    mem_free(tids);
    if (tid)
        snprintf(msg, n, TR("%s will be uploaded to Google Drive", "%s se subirá a Google Drive"), tid);
    else
        snprintf(msg, n, TR("%d game(s) will be uploaded to Google Drive (the ones already there are skipped)",
                            "Se subirán %d juego(s) a Google Drive (los que ya están se saltan)"),
                 count);
    return 0;
}

int drive_download(const char *tid, const char *dest, char *msg, size_t n) {
    if (ready(msg, n, 1))
        return -1;
    char dir[PATH_MAX];
    if (g_host.dest_dir(dest, dir, sizeof(dir))) {
        snprintf(msg, n, TR("%s is not available", "%s no está disponible"), dest);
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    queue_locked(OP_DOWNLOAD, tid, dest);
    pthread_mutex_unlock(&g_mu);
    snprintf(msg, n, TR("%s will be downloaded from Google Drive", "%s se descargará de Google Drive"), tid);
    return 0;
}

int drive_trash(const char *tid, char *msg, size_t n) {
    if (ready(msg, n, 1))
        return -1;
    pthread_mutex_lock(&g_mu);
    queue_locked(OP_TRASH, tid, "");
    pthread_mutex_unlock(&g_mu);
    snprintf(msg, n, TR("%s will be moved to the Drive trash", "%s se moverá a la papelera de Drive"), tid);
    return 0;
}

int drive_refresh(char *msg, size_t n) {
    if (ready(msg, n, 1))
        return -1;
    pthread_mutex_lock(&g_mu);
    queue_locked(OP_LIST, "", "");
    pthread_mutex_unlock(&g_mu);
    snprintf(msg, n, "%s", TR("Reading Google Drive", "Leyendo Google Drive"));
    return 0;
}

int drive_cancel(char *msg, size_t n) {
    if (ready(msg, n, 0))
        return -1;
    pthread_mutex_lock(&g_mu);
    /* the queue goes too; the list stays */
    int k = 0;
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i].op == OP_LIST || g_jobs[i].op == OP_LOGIN)
            g_jobs[k++] = g_jobs[i];
    g_njobs = k;
    g_cancel = g_busy && g_op != OP_LIST;
    save_queue_locked(); /* the running one goes from it when it ends */
    pthread_mutex_unlock(&g_mu);
    snprintf(msg, n, "%s", TR("Cancelled", "Cancelado"));
    return 0;
}

/* An id as Google gives them: <digits>-<letters and digits>.apps.googleusercontent.com. */
static int client_id_ok(const char *id) {
    static const char suffix[] = ".apps.googleusercontent.com";
    size_t l = strlen(id), sl = sizeof(suffix) - 1;
    if (l <= sl || l >= sizeof(g_client_id) || strcmp(id + l - sl, suffix))
        return 0;
    for (size_t i = 0; i < l - sl; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'z') || id[i] == '-'))
            return 0;
    return 1;
}

static int secret_ok(const char *s) {
    size_t l = strlen(s);
    if (!l || l >= sizeof(g_client_secret))
        return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-' || *s == '_'))
            return 0;
    return 1;
}

int drive_set_client(const char *id, const char *secret, char *msg, size_t n) {
    if (!g_started) {
        snprintf(msg, n, "%s", TR("Google Drive is not running", "Google Drive no está en marcha"));
        return -1;
    }
    if (!client_id_ok(id)) {
        snprintf(msg, n, "%s", TR("That is not a client ID: it ends in .apps.googleusercontent.com",
                                  "Eso no es un ID de cliente: termina en .apps.googleusercontent.com"));
        return -1;
    }
    if (!secret_ok(secret)) {
        snprintf(msg, n, "%s", TR("That is not a client secret (letters, digits, - and _)",
                                  "Eso no es un secreto de cliente (letras, números, - y _)"));
        return -1;
    }
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_client_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    buf_t b = {0};
    buf_printf(&b, "client_id=%s\nclient_secret=%s\n", id, secret);
    int ok = fd >= 0 && write(fd, b.data, b.len) == (ssize_t)b.len;
    memset(b.data, 0, b.len);
    buf_free(&b);
    if (fd >= 0)
        close(fd);
    if (!ok || rename(tmp, g_client_path)) {
        unlink(tmp);
        snprintf(msg, n, TR("cannot write %s (%s)", "no se puede escribir %s (%s)"), g_client_path, strerror(errno));
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    int changed = strcmp(id, g_client_id) != 0;
    snprintf(g_client_id, sizeof(g_client_id), "%s", id);
    snprintf(g_client_secret, sizeof(g_client_secret), "%s", secret);
    if (changed) {
        /* an account signed in with another client does not go with this one */
        g_cancel = 1;
        g_njobs = 0;
        g_login = 0;
        g_login_error[0] = 0;
    }
    pthread_mutex_unlock(&g_mu);
    if (changed)
        disconnected(NULL);
    printf("[drive] client set (%s)\n", id);
    snprintf(msg, n, "%s", TR("Google client saved on the console", "Cliente de Google guardado en la consola"));
    return 0;
}

int drive_forget_client(char *msg, size_t n) {
    if (!g_started) {
        snprintf(msg, n, "%s", TR("Google Drive is not running", "Google Drive no está en marcha"));
        return -1;
    }
    if (g_refresh[0])
        drive_disconnect(msg, n);
    pthread_mutex_lock(&g_mu);
    g_client_id[0] = 0;
    memset(g_client_secret, 0, sizeof(g_client_secret));
    g_login = 0;
    g_login_error[0] = 0;
    pthread_mutex_unlock(&g_mu);
    unlink(g_client_path);
    snprintf(msg, n, "%s", TR("Google client removed from the console", "Cliente de Google borrado de la consola"));
    return 0;
}
