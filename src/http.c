/* http.c - PinFall libcurl wrapper: in-memory cookies, gzip, retries,
 *          polite rate limiting, PC_HOST override.  Thread-safe.
 */
#define _POSIX_C_SOURCE 200809L

#include "http.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PC_MIN_INTERVAL_NS 250000000L /* ~250 ms between requests */
#define PC_MAX_RETRIES     2
#define PC_MAX_BODY        (48u * 1024u * 1024u)

/* ------------------------------------------------------------------ state */

struct PcHttp {
    CURL             *curl;
    pthread_mutex_t   mu;
    char              host[256];
    char              last_err[256];
    struct timespec   last_req;
    bool              have_last;
};

/* ------------------------------------------------------------- byte buffer */

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} Buf;

static bool buf_reserve(Buf *b, size_t extra)
{
    size_t need;
    char  *np;

    if (b->len + extra + 1 <= b->cap)
        return true;
    need = b->cap ? b->cap : 8192;
    while (need < b->len + extra + 1)
        need *= 2;
    np = realloc(b->p, need);
    if (!np)
        return false;
    b->p   = np;
    b->cap = need;
    return true;
}

static size_t buf_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Buf   *b = ud;
    size_t n = size * nmemb;

    if (b->len + n > PC_MAX_BODY)
        return 0; /* abort the transfer, body too large */
    if (!buf_reserve(b, n))
        return 0;
    memcpy(b->p + b->len, ptr, n);
    b->len += n;
    b->p[b->len] = '\0';
    return n;
}

/* ------------------------------------------------------------ misc helpers */

static void sleep_ms(long ms)
{
    struct timespec ts;

    if (ms <= 0)
        return;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ci_has_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (ascii_lower(*s) != ascii_lower(*prefix))
            return false;
        s++;
        prefix++;
    }
    return true;
}

/* Look for "Name: value" at the start of a line in a raw header block.
 * Copies the value into out (trimmed) and returns true when found. */
static bool header_value(const char *headers, const char *name,
                         char *out, size_t outsz)
{
    size_t nlen = strlen(name);
    const char *p;

    if (!headers || !name || nlen == 0)
        return false;
    for (p = headers; p && *p; p = strchr(p, '\n')) {
        if (*p == '\n')
            p++;
        if (!ci_has_prefix(p, name))
            continue;
        p += nlen;
        if (*p != ':')
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        {
            size_t i = 0;
            while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < outsz) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            return true;
        }
    }
    return false;
}

static long retry_after_secs(const char *headers)
{
    char  v[64];
    char *end = NULL;
    long  secs;

    if (!header_value(headers, "Retry-After", v, sizeof v))
        return -1;
    secs = strtol(v, &end, 10);
    if (end == v || secs < 0)
        return -1;
    if (secs > 15)
        secs = 15;
    return secs;
}

/* ------------------------------------------------------------ init / free */

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void global_init_once(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void pc_http_global_init(void)
{
    pthread_once(&g_once, global_init_once);
}

PcHttp *pc_http_new(void)
{
    PcHttp     *h;
    const char *env;
    const char *host;
    size_t      n;

    pc_http_global_init();
    h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    pthread_mutex_init(&h->mu, NULL);

    env  = getenv("PC_HOST");
    host = (env && *env) ? env : "https://nl.pinterest.com";
    snprintf(h->host, sizeof h->host, "%s", host);
    n = strlen(h->host);
    while (n > 0 && h->host[n - 1] == '/')
        h->host[--n] = '\0';

    h->curl = curl_easy_init();
    if (!h->curl) {
        pthread_mutex_destroy(&h->mu);
        free(h);
        return NULL;
    }
    curl_easy_setopt(h->curl, CURLOPT_USERAGENT, PC_USER_AGENT);
    curl_easy_setopt(h->curl, CURLOPT_COOKIEFILE, ""); /* in-memory jar */
    curl_easy_setopt(h->curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h->curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h->curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(h->curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(h->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h->curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h->curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h->curl, CURLOPT_TCP_KEEPALIVE, 1L);
    if (getenv("PC_CURL_VERBOSE"))
        curl_easy_setopt(h->curl, CURLOPT_VERBOSE, 1L);
    return h;
}

void pc_http_free(PcHttp *h)
{
    if (!h)
        return;
    if (h->curl)
        curl_easy_cleanup(h->curl);
    pthread_mutex_destroy(&h->mu);
    free(h);
}

const char *pc_http_host(PcHttp *h)
{
    return h ? h->host : "";
}

const char *pc_http_error(PcHttp *h)
{
    static _Thread_local char buf[256];

    buf[0] = '\0';
    if (!h)
        return buf;
    pthread_mutex_lock(&h->mu);
    memcpy(buf, h->last_err, sizeof buf);
    pthread_mutex_unlock(&h->mu);
    return buf;
}

/* ----------------------------------------------------------- cookie access */

static bool cookie_lookup_locked(PcHttp *h, const char *name,
                                 char *out, size_t outsz)
{
    struct curl_slist *list = NULL;
    struct curl_slist *it;
    bool found = false;

    if (outsz)
        out[0] = '\0';
    if (curl_easy_getinfo(h->curl, CURLINFO_COOKIELIST, &list) != CURLE_OK)
        return false;
    for (it = list; it && !found; it = it->next) {
        /* Netscape jar line: domain \t flag \t path \t secure \t expiry \t
         * name \t value */
        char  *copy = strdup(it->data);
        char  *fields[7];
        char  *p;
        int    nf = 0;

        if (!copy)
            break;
        p = copy;
        while (nf < 7 && p) {
            fields[nf++] = p;
            p = strchr(p, '\t');
            if (p) {
                *p = '\0';
                p++;
            }
        }
        if (nf >= 7 && strcmp(fields[5], name) == 0) {
            snprintf(out, outsz, "%s", fields[6]);
            found = true;
        }
        free(copy);
    }
    if (list)
        curl_slist_free_all(list);
    return found;
}

bool pc_http_cookie(PcHttp *h, const char *name, char *out, size_t outsz)
{
    bool ok;

    if (!h || !name || !out || outsz == 0)
        return false;
    pthread_mutex_lock(&h->mu);
    ok = cookie_lookup_locked(h, name, out, outsz);
    pthread_mutex_unlock(&h->mu);
    return ok;
}

/* ----------------------------------------------------- cookie jar file I/O */

#define PC_COOKIE_FILE_MAX (4u * 1024u * 1024u)

typedef struct {
    char  **lines;
    size_t  count;
    size_t  cap;
} CookieLines;

static void cookie_lines_free(CookieLines *cl)
{
    size_t i;

    for (i = 0; i < cl->count; i++)
        free(cl->lines[i]);
    free(cl->lines);
    cl->lines = NULL;
    cl->count = 0;
    cl->cap   = 0;
}

static bool cookie_lines_push(CookieLines *cl, char *line)
{
    if (cl->count == cl->cap) {
        size_t  nc = cl->cap ? cl->cap * 2 : 32;
        char  **nl = realloc(cl->lines, nc * sizeof *nl);

        if (!nl)
            return false;
        cl->lines = nl;
        cl->cap   = nc;
    }
    cl->lines[cl->count++] = line;
    return true;
}

/* Parse a Netscape cookies.txt buffer into canonical 7-field lines.
 * "#HttpOnly_" lines are cookies; other '#' lines and blank lines are skipped.
 * Values are copied, never printed.  Returns false when no cookie line exists. */
static bool cookie_parse(const char *buf, size_t len, CookieLines *out,
                         char *err, size_t errsz)
{
    size_t pos = 0;

    while (pos < len) {
        size_t end = pos;
        size_t llen;
        char  *line;
        char  *fields[7];
        char  *canon;
        char  *p;
        size_t need;
        int    nf = 0;

        while (end < len && buf[end] != '\n')
            end++;
        llen = end - pos;
        line = malloc(llen + 1);
        if (!line)
            goto oom;
        memcpy(line, buf + pos, llen);
        line[llen] = '\0';
        pos = (end < len) ? end + 1 : len;

        while (llen > 0 &&
               (line[llen - 1] == '\r' || line[llen - 1] == ' '))
            line[--llen] = '\0';
        if (llen == 0) {
            free(line);
            continue;
        }
        if (line[0] == '#' && strncmp(line, "#HttpOnly_", 10) != 0) {
            free(line); /* comment */
            continue;
        }
        p = line;
        while (nf < 7) {
            fields[nf++] = p;
            p = strchr(p, '\t');
            if (!p)
                break;
            *p = '\0';
            p++;
        }
        if (nf < 7 || !fields[0][0] || !fields[5][0]) {
            free(line); /* not a Netscape cookie line */
            continue;
        }
        need = 1;
        for (nf = 0; nf < 7; nf++)
            need += strlen(fields[nf]) + 1;
        canon = malloc(need);
        if (!canon) {
            free(line);
            goto oom;
        }
        canon[0] = '\0';
        for (nf = 0; nf < 7; nf++) {
            if (nf)
                strcat(canon, "\t");
            strcat(canon, fields[nf]);
        }
        free(line);
        if (!cookie_lines_push(out, canon)) {
            free(canon);
            goto oom;
        }
    }
    if (out->count == 0) {
        if (err && errsz)
            snprintf(err, errsz, "no Netscape cookie lines found");
        return false;
    }
    return true;

oom:
    if (err && errsz)
        snprintf(err, errsz, "out of memory");
    return false;
}

bool pc_http_cookie_load(PcHttp *h, const char *path, int *nloaded,
                         char *err, size_t errsz)
{
    FILE       *f;
    long        sz;
    char       *buf = NULL;
    size_t      nread;
    CookieLines cl = {0};
    bool        ok = false;
    size_t      i;

    if (err && errsz)
        err[0] = '\0';
    if (nloaded)
        *nloaded = 0;
    if (!h || !path || !*path) {
        if (err && errsz)
            snprintf(err, errsz, "invalid cookie file path");
        return false;
    }
    f = fopen(path, "rb");
    if (!f) {
        if (err && errsz)
            snprintf(err, errsz, "cookie file not found: %s", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        if (err && errsz)
            snprintf(err, errsz, "cannot read cookie file: %s", path);
        fclose(f);
        return false;
    }
    if (sz == 0) {
        if (err && errsz)
            snprintf(err, errsz, "cookie file is empty: %s", path);
        fclose(f);
        return false;
    }
    if ((unsigned long)sz > PC_COOKIE_FILE_MAX) {
        if (err && errsz)
            snprintf(err, errsz, "cookie file too large (%ld bytes)", sz);
        fclose(f);
        return false;
    }
    rewind(f);
    buf = malloc((size_t)sz);
    if (!buf) {
        if (err && errsz)
            snprintf(err, errsz, "out of memory");
        fclose(f);
        return false;
    }
    nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nread != (size_t)sz) {
        if (err && errsz)
            snprintf(err, errsz, "short read from cookie file: %s", path);
        free(buf);
        return false;
    }
    if (memchr(buf, '\0', nread)) {
        if (err && errsz)
            snprintf(err, errsz,
                     "cookie file contains NUL bytes (not a Netscape cookies.txt)");
        free(buf);
        return false;
    }
    if (!cookie_parse(buf, nread, &cl, err, errsz)) {
        free(buf);
        cookie_lines_free(&cl);
        return false;
    }
    free(buf);

    pthread_mutex_lock(&h->mu);
    curl_easy_setopt(h->curl, CURLOPT_COOKIELIST, "ALL"); /* replace jar */
    for (i = 0; i < cl.count; i++) {
        if (curl_easy_setopt(h->curl, CURLOPT_COOKIELIST, cl.lines[i]) !=
            CURLE_OK) {
            if (err && errsz)
                snprintf(err, errsz, "libcurl rejected a cookie line");
            curl_easy_setopt(h->curl, CURLOPT_COOKIELIST, "ALL");
            break;
        }
    }
    if (i == cl.count)
        ok = true;
    pthread_mutex_unlock(&h->mu);

    if (ok && nloaded)
        *nloaded = (int)cl.count;
    cookie_lines_free(&cl);
    return ok;
}

bool pc_http_cookie_save(PcHttp *h, const char *path,
                         char *err, size_t errsz)
{
    struct curl_slist *list = NULL;
    struct curl_slist *it;
    int                fd;
    FILE              *f;
    bool               ok = true;

    if (err && errsz)
        err[0] = '\0';
    if (!h || !path || !*path) {
        if (err && errsz)
            snprintf(err, errsz, "invalid cookie file path");
        return false;
    }
    pthread_mutex_lock(&h->mu);
    if (curl_easy_getinfo(h->curl, CURLINFO_COOKIELIST, &list) != CURLE_OK) {
        if (err && errsz)
            snprintf(err, errsz, "cannot read cookie jar");
        ok = false;
    }
    if (ok) {
        /* Create with 0600 and force it: an existing file may be wider. */
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            if (err && errsz)
                snprintf(err, errsz, "cannot create %s: %s", path,
                         strerror(errno));
            ok = false;
        } else {
            (void)fchmod(fd, 0600);
            f = fdopen(fd, "w");
            if (!f) {
                if (err && errsz)
                    snprintf(err, errsz, "cannot write %s: %s", path,
                             strerror(errno));
                close(fd);
                ok = false;
            } else {
                fputs("# Netscape HTTP Cookie File\n"
                      "# PinFall exported session - keep this file private\n\n",
                      f);
                for (it = list; it; it = it->next) {
                    fputs(it->data, f);
                    fputc('\n', f);
                }
                if (ferror(f)) {
                    if (err && errsz)
                        snprintf(err, errsz, "write failed for %s", path);
                    ok = false;
                }
                if (fclose(f) != 0) {
                    if (err && errsz)
                        snprintf(err, errsz, "write failed for %s", path);
                    ok = false;
                }
            }
        }
    }
    if (list)
        curl_slist_free_all(list);
    pthread_mutex_unlock(&h->mu);
    return ok;
}

/* --------------------------------------------------------------- transfer */

static CURLcode perform_once(PcHttp *h, const char *url, const char *body,
                             struct curl_slist *hdrs, PcHttpResp *resp)
{
    Buf         bb = {0}, hb = {0};
    CURLcode    rc;
    long        status = 0;

    if (!buf_reserve(&bb, 0) || !buf_reserve(&hb, 0)) {
        free(bb.p);
        free(hb.p);
        return CURLE_OUT_OF_MEMORY;
    }
    bb.p[0] = '\0';
    hb.p[0] = '\0';

    curl_easy_setopt(h->curl, CURLOPT_URL, url);
    if (body) {
        curl_easy_setopt(h->curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h->curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        curl_easy_setopt(h->curl, CURLOPT_POST, 1L);
    } else {
        curl_easy_setopt(h->curl, CURLOPT_POSTFIELDS, NULL);
        curl_easy_setopt(h->curl, CURLOPT_POSTFIELDSIZE, (long)-1);
        curl_easy_setopt(h->curl, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(h->curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h->curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(h->curl, CURLOPT_WRITEDATA, &bb);
    curl_easy_setopt(h->curl, CURLOPT_HEADERFUNCTION, buf_write);
    curl_easy_setopt(h->curl, CURLOPT_HEADERDATA, &hb);

    rc = curl_easy_perform(h->curl);
    if (rc == CURLE_OK)
        curl_easy_getinfo(h->curl, CURLINFO_RESPONSE_CODE, &status);

    resp->status   = (rc == CURLE_OK) ? status : 0;
    resp->body     = bb.p;
    resp->body_len = bb.len;
    resp->headers  = hb.p;
    return rc;
}

void pc_http_resp_free(PcHttpResp *r)
{
    if (!r)
        return;
    free(r->body);
    free(r->headers);
    r->body     = NULL;
    r->headers  = NULL;
    r->body_len = 0;
    r->status   = 0;
}

/* Held with h->mu locked.  allow_retry=false sends the request exactly once
 * (used for writes: 429/5xx and transport retries could duplicate a mutation). */
static bool request_locked(PcHttp *h, const char *url, const char *body,
                           const char *const *extra, PcHttpResp *resp,
                           bool allow_retry, const char *content_type)
{
    struct curl_slist *hdrs = NULL;
    bool                ok = false;
    int                 attempt;
    int                 max_attempts = allow_retry ? PC_MAX_RETRIES + 1 : 1;

    if (!h || !url || !resp)
        return false;
    memset(resp, 0, sizeof *resp);

    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
    if (content_type)
        hdrs = curl_slist_append(hdrs, content_type);
    if (extra) {
        const char *const *p;
        for (p = extra; *p; p++)
            hdrs = curl_slist_append(hdrs, *p);
    }

    for (attempt = 0; attempt < max_attempts; attempt++) {
        CURLcode rc;

        /* polite minimum interval between requests */
        if (h->have_last) {
            struct timespec now;
            double elapsed;

            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed = (double)(now.tv_sec - h->last_req.tv_sec) +
                      (double)(now.tv_nsec - h->last_req.tv_nsec) / 1e9;
            if (elapsed < 0.25) {
                long wait_ms = (long)((0.25 - elapsed) * 1000.0);
                sleep_ms(wait_ms > 0 ? wait_ms : 1);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &h->last_req);
        h->have_last = true;

        pc_http_resp_free(resp);
        rc = perform_once(h, url, body, hdrs, resp);
        if (rc == CURLE_OK) {
            bool retryable = (resp->status == 429 || resp->status >= 500);
            if (retryable && allow_retry && attempt < PC_MAX_RETRIES) {
                long ra = retry_after_secs(resp->headers);
                long delay_ms = (ra >= 0) ? ra * 1000 : (long)(1000 << attempt);
                sleep_ms(delay_ms);
                continue;
            }
            ok = true;
            break;
        }
        if (allow_retry && attempt < PC_MAX_RETRIES) {
            sleep_ms(500 * (attempt + 1));
            continue;
        }
        snprintf(h->last_err, sizeof h->last_err, "curl: %s",
                 curl_easy_strerror(rc));
        break;
    }
    curl_slist_free_all(hdrs);
    if (!ok)
        pc_http_resp_free(resp);
    else
        h->last_err[0] = '\0';
    return ok;
}

bool pc_http_get(PcHttp *h, const char *url,
                 const char *const *extra_headers, PcHttpResp *resp)
{
    bool ok;

    if (!h)
        return false;
    pthread_mutex_lock(&h->mu);
    ok = request_locked(h, url, NULL, extra_headers, resp, true, NULL);
    pthread_mutex_unlock(&h->mu);
    return ok;
}

bool pc_http_post_form(PcHttp *h, const char *url, const char *body,
                       const char *const *extra_headers, PcHttpResp *resp)
{
    bool ok;

    if (!h || !body)
        return false;
    pthread_mutex_lock(&h->mu);
    ok = request_locked(h, url, body, extra_headers, resp, true, NULL);
    pthread_mutex_unlock(&h->mu);
    return ok;
}

bool pc_http_post_form_once(PcHttp *h, const char *url, const char *body,
                            const char *const *extra_headers, PcHttpResp *resp)
{
    bool ok;

    if (!h || !body)
        return false;
    pthread_mutex_lock(&h->mu);
    ok = request_locked(h, url, body, extra_headers, resp, false, NULL);
    pthread_mutex_unlock(&h->mu);
    return ok;
}

bool pc_http_post_json(PcHttp *h, const char *url, const char *json_body,
                       const char *const *extra_headers, PcHttpResp *resp)
{
    bool ok;

    if (!h || !json_body)
        return false;
    pthread_mutex_lock(&h->mu);
    ok = request_locked(h, url, json_body, extra_headers, resp, true,
                        "Content-Type: application/json");
    pthread_mutex_unlock(&h->mu);
    return ok;
}

bool pc_http_post_json_once(PcHttp *h, const char *url, const char *json_body,
                            const char *const *extra_headers,
                            PcHttpResp *resp)
{
    bool ok;

    if (!h || !json_body)
        return false;
    pthread_mutex_lock(&h->mu);
    ok = request_locked(h, url, json_body, extra_headers, resp, false,
                        "Content-Type: application/json");
    pthread_mutex_unlock(&h->mu);
    return ok;
}
