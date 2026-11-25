/* api.c - PinFall Pinterest guest-session + resource RPC layer.
 *
 * Implements the public API from include/pc.h (frozen) on top of http.c and
 * jsonx.c.  All entry points are thread-safe: client state is protected by a
 * single mutex, which also serialises resource requests.
 *
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "graphql.h"
#include "http.h"
#include "jsonx.h"
#include "pc.h"

struct PcClient {
    PcHttp         *http;
    pthread_mutex_t mu;     /* serialises RPCs and client state */
    pthread_mutex_t err_mu; /* guards last_error and username */
    char            last_error[512];
    char            csrf[256];
    bool            bootstrapped;

    /* v5 ad-block state (guarded by mu) */
    bool            block_ads;
    uint64_t        ads_blocked;

    /* v6 close-up cache: pin id -> aggregated_pin_data id (comments key). */
    struct {
        char pin_id[PC_ID_LEN];
        char agg_id[PC_ID_LEN];
    } closeup_ids[8];
    size_t          closeup_id_count;

    /* v2 session / auth state (guarded by mu unless noted) */
    bool            probe_done;   /* auth probe attempted (at most once) */
    PcAuthState     auth_state;
    char            username[PC_SHORT_LEN]; /* guarded by err_mu */
    PcPinList       homefeed_cache;        /* probe page 1 */
};

/* ------------------------------------------------------------- utilities */

static char *urlenc(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t            n, i, o = 0;
    char             *out;

    if (!s)
        return NULL;
    n   = strlen(s);
    out = malloc(n * 3 + 1);
    if (!out)
        return NULL;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];

        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
            ch == '~') {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 0x0F];
        }
    }
    out[o] = '\0';
    return out;
}

static void body_snippet(const char *body, size_t len, char *dst, size_t dstsz)
{
    size_t i, o = 0;

    if (!dst || dstsz == 0)
        return;
    for (i = 0; body && i < len && o + 1 < dstsz; i++) {
        unsigned char ch = (unsigned char)body[i];
        if (ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
        if (ch >= 32 || ch == ' ')
            dst[o++] = (char)ch;
    }
    dst[o] = '\0';
}

static void set_error(PcClient *c, const char *fmt, va_list ap)
{
    char buf[512];

    if (!c)
        return;
    vsnprintf(buf, sizeof buf, fmt, ap);
    pthread_mutex_lock(&c->err_mu);
    memcpy(c->last_error, buf, sizeof c->last_error);
    pthread_mutex_unlock(&c->err_mu);
    return;
}

void pc_client_set_error(PcClient *c, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    set_error(c, fmt, ap);
    va_end(ap);
}

void pc_client_clear_error(PcClient *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->err_mu);
    c->last_error[0] = '\0';
    pthread_mutex_unlock(&c->err_mu);
}

/* Record on the client and hand the caller a malloc'd copy via *err. */
static bool fail(PcClient *c, char **err, const char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (c) {
        pthread_mutex_lock(&c->err_mu);
        memcpy(c->last_error, buf, sizeof c->last_error);
        pthread_mutex_unlock(&c->err_mu);
    }
    if (err)
        *err = strdup(buf);
    return false;
}

/* Caller holds c->mu.  Bracket one feed parse so jsonx can filter promoted
 * pins and report how many it skipped. */
static void ads_parse_begin(PcClient *c)
{
    pc_json_ads_begin(c->block_ads);
}

static void ads_parse_end(PcClient *c)
{
    uint64_t n = pc_json_ads_end();

    c->ads_blocked += n;
}

/* ---------------------------------------------------------------- client */

PcClient *pc_client_new(const char *cache_dir)
{
    PcClient *c;

    (void)cache_dir; /* memory-only image cache: disk is full by design */

    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    pthread_mutex_init(&c->mu, NULL);
    pthread_mutex_init(&c->err_mu, NULL);
    c->http = pc_http_new();
    if (!c->http) {
        pthread_mutex_destroy(&c->mu);
        pthread_mutex_destroy(&c->err_mu);
        free(c);
        return NULL;
    }
    return c;
}

void pc_client_free(PcClient *c)
{
    if (!c)
        return;
    pc_pinlist_free(&c->homefeed_cache);
    pc_http_free(c->http);
    pthread_mutex_destroy(&c->mu);
    pthread_mutex_destroy(&c->err_mu);
    free(c);
}

const char *pc_last_error(PcClient *c)
{
    static _Thread_local char buf[512];

    buf[0] = '\0';
    if (!c)
        return buf;
    pthread_mutex_lock(&c->err_mu);
    memcpy(buf, c->last_error, sizeof buf);
    pthread_mutex_unlock(&c->err_mu);
    return buf;
}

/* ------------------------------------------------------- bootstrap helper */

/* Caller holds c->mu. */
static bool bootstrap_locked(PcClient *c, char **err)
{
    char        url[512];
    PcHttpResp  r;
    const char *hdrs[] = {
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
        "image/avif,image/webp,*/*;q=0.8",
        NULL
    };
    bool ok = false;

    memset(&r, 0, sizeof r);
    snprintf(url, sizeof url, "%s/", pc_http_host(c->http));

    if (!pc_http_get(c->http, url, hdrs, &r)) {
        fail(c, err, "bootstrap: %s", pc_http_error(c->http));
        return false;
    }
    if (r.status != 200) {
        fail(c, err, "bootstrap: HTTP %ld from %s", r.status, url);
        pc_http_resp_free(&r);
        return false;
    }
    if (!pc_http_cookie(c->http, "csrftoken", c->csrf, sizeof c->csrf)) {
        fail(c, err,
             "bootstrap: no csrftoken cookie (page sanity: %s)",
             (r.body && strstr(r.body, "__PWS_INITIAL_PROPS__"))
                 ? "__PWS_INITIAL_PROPS__ present"
                 : "__PWS_INITIAL_PROPS__ missing");
        pc_http_resp_free(&r);
        return false;
    }
    c->bootstrapped = true;
    ok = true;
    {
        char uname[PC_SHORT_LEN];

        /* Cheap: parse the HTML we already have; never a second request. */
        pc_json_extract_username(r.body ? r.body : "", r.body_len, uname,
                                 sizeof uname);
        pthread_mutex_lock(&c->err_mu);
        memcpy(c->username, uname, sizeof c->username);
        pthread_mutex_unlock(&c->err_mu);
    }
    pc_client_clear_error(c);
    pc_http_resp_free(&r);
    return ok;
}

bool pc_bootstrap(PcClient *c)
{
    bool ok;

    if (!c)
        return false;
    pthread_mutex_lock(&c->mu);
    ok = bootstrap_locked(c, NULL);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* -------------------------------------------------- resource RPC requests */

/* Caller holds c->mu.  Fills *out with the HTTP response (caller frees).
 * allow_retry=false is used for writes: no transport retry, no 403
 * re-bootstrap retry (mutations are not idempotent). */
static bool rpc_verb_locked(PcClient *c, const char *resource, const char *verb,
                            const char *source, const char *data_json,
                            bool allow_retry, PcHttpResp *out, char **err)
{
    char  *enc_data = NULL;
    char  *enc_src  = NULL;
    char  *body     = NULL;
    char  *url      = NULL;
    char   h_csrf[300];
    char   h_src[1400];
    char   h_ref[1700];
    char   h_org[300];
    char   h_acc[200];
    size_t blen, ulen;
    bool   ok = false;
    int    attempt;
    int    max_attempts = allow_retry ? 2 : 1;

    memset(out, 0, sizeof *out);
    enc_data = urlenc(data_json);
    enc_src  = urlenc(source);
    if (!enc_data || !enc_src) {
        fail(c, err, "out of memory");
        goto done;
    }

    blen = strlen(enc_data) + strlen(enc_src) + 32;
    body = malloc(blen);
    if (!body) {
        fail(c, err, "out of memory");
        goto done;
    }
    snprintf(body, blen, "data=%s&source_url=%s", enc_data, enc_src);

    ulen = strlen(pc_http_host(c->http)) + strlen(resource) + strlen(enc_src) + 64;
    url  = malloc(ulen);
    if (!url) {
        fail(c, err, "out of memory");
        goto done;
    }
    snprintf(url, ulen, "%s/resource/%s/%s/?source_url=%s",
             pc_http_host(c->http), resource, verb, enc_src);

    for (attempt = 0; attempt < max_attempts; attempt++) {
        const char *hdrs[10];
        int         i = 0;

        snprintf(h_csrf, sizeof h_csrf, "X-CSRFToken: %s", c->csrf);
        snprintf(h_src, sizeof h_src, "X-Pinterest-Source-Url: %s", source);
        snprintf(h_ref, sizeof h_ref, "Referer: %s%s", pc_http_host(c->http),
                 source);
        snprintf(h_org, sizeof h_org, "Origin: %s", pc_http_host(c->http));
        snprintf(h_acc, sizeof h_acc,
                 "Accept: application/json, text/javascript, */*; q=0.01");

        hdrs[i++] = "X-Requested-With: XMLHttpRequest";
        hdrs[i++] = h_csrf;
        hdrs[i++] = "X-Pinterest-AppState: active";
        hdrs[i++] = h_src;
        hdrs[i++] = h_org;
        hdrs[i++] = h_ref;
        hdrs[i++] = h_acc;
        hdrs[i++] = "Content-Type: application/x-www-form-urlencoded; "
                    "charset=UTF-8";
        hdrs[i]   = NULL;

        if (allow_retry) {
            if (!pc_http_post_form(c->http, url, body, hdrs, out)) {
                fail(c, err, "%s: %s", resource, pc_http_error(c->http));
                goto done;
            }
        } else {
            if (!pc_http_post_form_once(c->http, url, body, hdrs, out)) {
                fail(c, err, "%s: %s", resource, pc_http_error(c->http));
                goto done;
            }
        }
        /* A guest cookie can expire: re-bootstrap once, then retry.  Never for
         * writes. */
        if (allow_retry && out->status == 403 && attempt == 0) {
            pc_http_resp_free(out);
            if (bootstrap_locked(c, NULL))
                continue;
        }
        break;
    }
    ok = true;

done:
    free(enc_data);
    free(enc_src);
    free(body);
    free(url);
    return ok;
}

/* Read RPC (GET verb, retries allowed). */
static bool rpc_locked(PcClient *c, const char *resource, const char *source,
                       const char *data_json, PcHttpResp *out, char **err)
{
    return rpc_verb_locked(c, resource, "get", source, data_json, true, out,
                           err);
}

/* ---------------------------------------------------------------- search */

/* Caller holds c->mu.  Build + send a BaseSearchResource request and return the
 * raw JSON body (caller frees *r). */
static bool search_rpc_locked(PcClient *c, const char *query,
                              const char *bookmark, PcHttpResp *r, char **err)
{
    cJSON *root = NULL;
    cJSON *opts;
    cJSON *bms;
    char  *data = NULL;
    char  *enc_q = NULL;
    char   source[1200];
    char   snippet[200];
    bool   ok = false;

    memset(r, 0, sizeof *r);
    enc_q = urlenc(query);
    if (!enc_q) {
        fail(c, err, "out of memory");
        return false;
    }
    snprintf(source, sizeof source, "/search/pins/?q=%s", enc_q);

    root = cJSON_CreateObject();
    opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
    bms  = opts ? cJSON_AddArrayToObject(opts, "bookmarks") : NULL;
    if (!root || !opts || !bms || !cJSON_AddStringToObject(opts, "query", query)
        || !cJSON_AddStringToObject(opts, "scope", "pins")
        || !cJSON_AddNumberToObject(opts, "page_size", 50)
        || !cJSON_AddObjectToObject(root, "context")) {
        fail(c, err, "out of memory building search request");
        goto done;
    }
    if (bookmark && *bookmark) {
        cJSON *bm = cJSON_CreateString(bookmark);

        if (!bm) {
            fail(c, err, "out of memory building bookmark");
            goto done;
        }
        cJSON_AddItemToArray(bms, bm);
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising search request");
        goto done;
    }
    if (!rpc_locked(c, "BaseSearchResource", source, data, r, err))
        goto done;
    if (r->status != 200) {
        body_snippet(r->body, r->body_len, snippet, sizeof snippet);
        fail(c, err, "search: HTTP %ld (%s)", r->status, snippet);
        pc_http_resp_free(r);
        goto done;
    }
    ok = true;

done:
    free(data);
    free(enc_q);
    if (root)
        cJSON_Delete(root);
    return ok;
}

bool pc_search(PcClient *c, const char *query, const char *bookmark,
               PcPinList *out, char **err)
{
    PcHttpResp  r;
    char       *jerr = NULL;
    bool        ok = false;

    if (err)
        *err = NULL;
    if (!out) {
        if (err)
            *err = strdup("invalid arguments: out is NULL");
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!c || !query) {
        if (err)
            *err = strdup("invalid arguments");
        return false;
    }

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);

    if (!c->bootstrapped) {
        fail(c, err, "not bootstrapped: call pc_bootstrap() first");
        goto done;
    }
    if (!search_rpc_locked(c, query, bookmark, &r, err))
        goto done;
    ads_parse_begin(c);
    ok = pc_json_parse_search(r.body, r.body_len, out, &jerr);
    ads_parse_end(c);
    if (!ok) {
        fail(c, err, "search parse: %s", jerr ? jerr : "unknown error");
        free(jerr);
        jerr = NULL;
        goto done;
    }
    ok = true;
    pc_client_clear_error(c);

done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    free(jerr);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* Internal (tools/dump --ads): one network request, then parse the SAME body
 * with the filter off (count everything) and on (count skips) and verify that
 * blocked pins explain the difference exactly.  Outputs are filled even when
 * the invariant check fails so the caller can print the evidence. */
bool pc_search_ads_report(PcClient *c, const char *query, int *pins,
                          uint64_t *ads_detected, uint64_t *ads_blocked_out,
                          char **err)
{
    PcHttpResp r;
    PcPinList  off, on;
    char      *jerr = NULL;
    char      *cerr = NULL;
    uint64_t   detected = 0;
    uint64_t   blocked = 0;
    bool       ok = false;

    if (err)
        *err = NULL;
    if (pins)
        *pins = 0;
    if (ads_detected)
        *ads_detected = 0;
    if (ads_blocked_out)
        *ads_blocked_out = 0;
    if (!c || !query || !pins || !ads_detected || !ads_blocked_out) {
        if (err)
            *err = strdup("invalid arguments");
        return false;
    }
    memset(&r, 0, sizeof r);
    memset(&off, 0, sizeof off);
    memset(&on, 0, sizeof on);

    pthread_mutex_lock(&c->mu);
    if (!c->bootstrapped) {
        fail(c, err, "not bootstrapped: call pc_bootstrap() first");
        goto done;
    }
    if (!search_rpc_locked(c, query, NULL, &r, err))
        goto done;

    detected = pc_json_count_ads(r.body, r.body_len, &cerr);
    if (cerr) {
        fail(c, err, "ads count: %s", cerr);
        goto done;
    }

    pc_json_ads_begin(false);
    if (!pc_json_parse_pins(r.body, r.body_len, &off, &jerr)) {
        pc_json_ads_end();
        fail(c, err, "ads off parse: %s", jerr ? jerr : "unknown error");
        goto done;
    }
    pc_json_ads_end();

    pc_json_ads_begin(true);
    if (!pc_json_parse_pins(r.body, r.body_len, &on, &jerr)) {
        pc_json_ads_end();
        fail(c, err, "ads on parse: %s", jerr ? jerr : "unknown error");
        goto done;
    }
    blocked = pc_json_ads_end();

    *pins          = (int)off.count;
    *ads_detected  = detected;
    *ads_blocked_out = blocked;

    if (detected != blocked ||
        off.count != on.count + (size_t)detected) {
        fail(c, err,
             "ad filter invariant failed: detected=%llu blocked=%llu "
             "pins_off=%zu pins_on=%zu",
             (unsigned long long)detected, (unsigned long long)blocked,
             off.count, on.count);
        goto done;
    }
    ok = true;
    pc_client_clear_error(c);

done:
    free(cerr);
    free(jerr);
    pc_pinlist_free(&off);
    pc_pinlist_free(&on);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* forward declarations for helpers defined later in this file */
static const char *json_obj_str(cJSON *obj, const char *key);

/* ------------------------------------------- close-up / REST helpers (v6) */

/* Fields requested from the v3 pin entity: exact reaction counts, viewer state
 * and the aggregated_pin_data id that UnifiedCommentsResource needs. */
static const char *const k_closeup_fields[] = {
    "pin.id", "pin.comment_count", "pin.reaction_counts", "pin.liked_by_me",
    "pin.repin_count", "pin.aggregated_pin_data()", "pin.images", "pin.title",
    "pin.grid_title", "pin.description", NULL
};

static bool pin_id_all_digits(const char *id)
{
    const char *p;

    if (!id || !*id)
        return false;
    for (p = id; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return true;
}

/* Reject e1_/non-canonical ids before any network traffic. */
static bool canonical_pin_or_fail(PcClient *c, const char *pin_id, char **err)
{
    if (pin_id_all_digits(pin_id))
        return true;
    if (pin_id && strncmp(pin_id, "e1_", 3) == 0)
        fail(c, err, "pin id is a search entity; close-up unavailable");
    else
        fail(c, err, "pin id is not canonical (numeric); close-up unavailable");
    return false;
}

static void copy_field_api(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0)
        return;
    pc_utf8_ellipsize(dst, dstsz, src ? src : "");
}

static void copy_url_api(char *dst, size_t dstsz, const char *src)
{
    size_t n, keep;

    if (!dst || dstsz == 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    n = strlen(src);
    if (n < dstsz) {
        memcpy(dst, src, n + 1);
        return;
    }
    keep = dstsz - 1;
    while (keep > 0 && ((unsigned char)src[keep] & 0xC0) == 0x80)
        keep--;
    memcpy(dst, src, keep);
    dst[keep] = '\0';
}

static bool obj_int(cJSON *obj, const char *key, int *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsNumber(v)) {
        *out = v->valueint;
        return true;
    }
    return false;
}

static bool obj_bool(cJSON *obj, const char *key, bool *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
        return true;
    }
    if (cJSON_IsNumber(v)) {
        *out = v->valueint != 0;
        return true;
    }
    return false;
}

/* Caller holds c->mu.  POST /resource/<res>/<verb>/ with form data. */
static bool resource_rpc_locked(PcClient *c, const char *res, const char *verb,
                                const char *source, const char *data_json,
                                bool allow_retry, PcHttpResp *r, char **err)
{
    return rpc_verb_locked(c, res, verb, source, data_json, allow_retry, r,
                           err);
}

/* Caller holds c->mu.  Fetch the v3 pin entity with the close-up field list. */
static bool closeup_entity_locked(PcClient *c, const char *pin_id,
                                  PcHttpResp *r, char **err)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *opts;
    cJSON *data;
    cJSON *fields;
    char  *json = NULL;
    char   source[700];
    char   url[256];
    bool   ok;

    memset(r, 0, sizeof *r);
    if (!root)
        return false;
    opts = cJSON_AddObjectToObject(root, "options");
    if (!opts) {
        cJSON_Delete(root);
        return false;
    }
    snprintf(url, sizeof url, "/v3/pins/%s/", pin_id);
    cJSON_AddStringToObject(opts, "url", url);
    data = cJSON_AddObjectToObject(opts, "data");
    fields = data ? cJSON_AddArrayToObject(data, "fields") : NULL;
    if (!fields) {
        cJSON_Delete(root);
        return false;
    }
    {
        int i;

        for (i = 0; k_closeup_fields[i]; i++) {
            cJSON *f = cJSON_CreateString(k_closeup_fields[i]);

            if (!f) {
                cJSON_Delete(root);
                return false;
            }
            cJSON_AddItemToArray(fields, f);
        }
    }
    cJSON_AddObjectToObject(root, "context");
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        fail(c, err, "out of memory building close-up request");
        return false;
    }
    snprintf(source, sizeof source, "/pin/%s/", pin_id);
    ok = resource_rpc_locked(c, "ApiResource", "get", source, json, true, r,
                             err);
    free(json);
    return ok;
}

/* Entity data object from a resource envelope. */
static cJSON *closeup_entity_data(cJSON *root)
{
    cJSON *rr;
    cJSON *data;

    if (!root)
        return NULL;
    rr = cJSON_GetObjectItemCaseSensitive(root, "resource_response");
    if (!cJSON_IsObject(rr))
        return NULL;
    data = cJSON_GetObjectItemCaseSensitive(rr, "data");
    return cJSON_IsObject(data) ? data : NULL;
}

/* Caller holds c->mu.  Resolve (and cache) the aggregated_pin_data id used by
 * UnifiedCommentsResource so comments don't refetch the entity repeatedly. */
static bool closeup_agg_id_locked(PcClient *c, const char *pin_id, char *out,
                                  size_t outsz, char **err)
{
    PcHttpResp r;
    cJSON     *root = NULL;
    cJSON     *data;
    cJSON     *agg;
    const char *agg_id;
    size_t      i;
    bool        ok = false;

    for (i = 0; i < c->closeup_id_count; i++) {
        if (strcmp(c->closeup_ids[i].pin_id, pin_id) == 0) {
            snprintf(out, outsz, "%s", c->closeup_ids[i].agg_id);
            return true;
        }
    }
    if (!closeup_entity_locked(c, pin_id, &r, err))
        return false;
    if (r.status == 401) {
        fail(c, err, "login required: close-up needs a signed-in session");
        goto done;
    }
    if (r.status != 200) {
        fail(c, err, "close-up entity: HTTP %ld", r.status);
        goto done;
    }
    root = cJSON_ParseWithLength(r.body, r.body_len);
    data = closeup_entity_data(root);
    agg = data ? cJSON_GetObjectItemCaseSensitive(data, "aggregated_pin_data")
               : NULL;
    agg_id = cJSON_IsObject(agg) ? json_obj_str(agg, "id") : NULL;
    if (!agg_id || !*agg_id) {
        fail(c, err, "close-up entity has no aggregated_pin_data id");
        goto done;
    }
    snprintf(out, outsz, "%s", agg_id);
    {
        size_t slot = c->closeup_id_count % 8;

        snprintf(c->closeup_ids[slot].pin_id, PC_ID_LEN, "%s", pin_id);
        snprintf(c->closeup_ids[slot].agg_id, PC_ID_LEN, "%s", agg_id);
        if (c->closeup_id_count < 8)
            c->closeup_id_count++;
    }
    ok = true;

done:
    if (root)
        cJSON_Delete(root);
    pc_http_resp_free(&r);
    return ok;
}

/* Caller holds c->mu.  Resource RPC envelope success check. */
static bool resource_success(const PcHttpResp *r, char **err)
{
    cJSON      *root;
    cJSON      *rr;
    cJSON      *status;
    cJSON      *eobj;
    const char *msg = NULL;
    bool        ok = false;

    if (!r || !r->body)
        return false;
    root = cJSON_ParseWithLength(r->body, r->body_len);
    if (!root)
        return false;
    rr = cJSON_GetObjectItemCaseSensitive(root, "resource_response");
    status = cJSON_GetObjectItemCaseSensitive(rr, "status");
    if (cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0)
        ok = true;
    if (!ok) {
        eobj = cJSON_GetObjectItemCaseSensitive(rr, "error");
        if (cJSON_IsObject(eobj))
            msg = json_obj_str(eobj, "message");
        if (!msg)
            msg = json_obj_str(rr, "message");
        if (err)
            *err = strdup(msg ? msg : "server did not confirm the mutation");
    }
    cJSON_Delete(root);
    return ok;
}

/* Fill a PcPin from a v3 close-up entity (best effort; keeps existing fields).
 * This entity is fetched with a restricted field list, so usually only the
 * social counts are present. */
static void closeup_fill_entity(cJSON *data, PcPin *p)
{
    cJSON *v;
    int    n;
    bool   b;

    if (!cJSON_IsObject(data))
        return;
    v = cJSON_GetObjectItemCaseSensitive(data, "reaction_counts");
    if (cJSON_IsObject(v)) {
        cJSON *one = cJSON_GetObjectItemCaseSensitive(v, "1");
        if (cJSON_IsNumber(one))
            p->like_count = one->valueint;
    }
    if (obj_int(data, "like_count", &n) && (!p->like_count || p->like_count == 0))
        p->like_count = n;
    if (obj_int(data, "comment_count", &n) && p->comment_count == 0)
        p->comment_count = n;
    if (obj_bool(data, "liked_by_me", &b) || obj_bool(data, "likedByMe", &b))
        p->liked = b;
    if (!p->title[0]) {
        const char *t = json_obj_str(data, "title");

        if (!t || !*t)
            t = json_obj_str(data, "grid_title");
        copy_field_api(p->title, sizeof p->title, t);
    }
    if (!p->description[0])
        copy_field_api(p->description, sizeof p->description,
                       json_obj_str(data, "description"));
    /* data.images uses the same dict schema as search pins (§8). */
    {
        cJSON *imgs = cJSON_GetObjectItemCaseSensitive(data, "images");

        if (cJSON_IsObject(imgs)) {
            struct {
                const char *key;
                char       *dst;
                size_t      dstsz;
            } buckets[] = {
                {"236x", p->thumb_url, sizeof p->thumb_url},
                {"474x", p->medium_url, sizeof p->medium_url},
                {"736x", p->big_url, sizeof p->big_url},
                {"orig", p->orig_url, sizeof p->orig_url},
            };
            size_t bi;

            for (bi = 0; bi < sizeof buckets / sizeof buckets[0]; bi++) {
                cJSON      *b = cJSON_GetObjectItemCaseSensitive(
                    imgs, buckets[bi].key);
                const char *u;
                int         w = 0, h = 0;

                if (!cJSON_IsObject(b))
                    continue;
                u = json_obj_str(b, "url");
                if (u && *u && !buckets[bi].dst[0])
                    copy_url_api(buckets[bi].dst, buckets[bi].dstsz, u);
                if (strcmp(buckets[bi].key, "236x") == 0) {
                    if (obj_int(b, "width", &w) && w > 0 && p->img_w == 0)
                        p->img_w = w;
                    if (obj_int(b, "height", &h) && h > 0 && p->img_h == 0)
                        p->img_h = h;
                }
            }
        }
    }
    if (!p->id[0]) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "id");

        if (cJSON_IsString(id) && id->valuestring)
            pc_utf8_ellipsize(p->id, sizeof p->id, id->valuestring);
        else if (cJSON_IsNumber(id))
            snprintf(p->id, sizeof p->id, "%.0f", id->valuedouble);
    }
}

/* Caller holds c->mu.  Enrich a detail result with exact close-up counts. */
static bool closeup_enrich_locked(PcClient *c, const char *pin_id, PcPin *out,
                                  bool *ok)
{
    PcHttpResp  r;
    cJSON      *root = NULL;
    cJSON      *data;
    bool        used = false;

    if (!closeup_entity_locked(c, pin_id, &r, NULL))
        return false;
    if (r.status == 200) {
        root = cJSON_ParseWithLength(r.body, r.body_len);
        data = closeup_entity_data(root);
        if (cJSON_IsObject(data)) {
            closeup_fill_entity(data, out);
            used = true;
        }
        cJSON_Delete(root);
    }
    pc_http_resp_free(&r);
    if (used && ok && out->id[0])
        *ok = true;
    return used;
}

/* Optional GraphQL fallback for related pins (REST is primary). */
static char *gql_related_vars(const char *pin_id)
{
    cJSON *root = cJSON_CreateObject();
    char  *s;

    if (!root)
        return NULL;
    cJSON_AddStringToObject(root, "pinId", pin_id);
    cJSON_AddNumberToObject(root, "count", 12);
    s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* ------------------------------------------------------------ pin detail */

/* Build a PinResource request body for one id / field_set_key. */
static char *detail_data(const char *pin_id, const char *field_set_key)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *opts;
    char  *data;

    if (!root)
        return NULL;
    opts = cJSON_AddObjectToObject(root, "options");
    if (!opts
        || !cJSON_AddStringToObject(opts, "id", pin_id)
        || !cJSON_AddStringToObject(opts, "field_set_key", field_set_key)
        || !cJSON_AddObjectToObject(root, "context")) {
        cJSON_Delete(root);
        return NULL;
    }
    data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return data;
}

bool pc_pin_detail(PcClient *c, const char *pin_id, PcPin *out, char **err)
{
    char       *data = NULL;
    char       *enc_id = NULL;
    char        source[600];
    char        snippet[200];
    PcHttpResp  r;
    char       *jerr = NULL;
    bool        ok = false;

    if (err)
        *err = NULL;
    if (!out) {
        if (err)
            *err = strdup("invalid arguments: out is NULL");
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!c || !pin_id || !*pin_id) {
        if (err)
            *err = strdup("invalid arguments");
        return false;
    }

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);

    if (!c->bootstrapped) {
        fail(c, err, "not bootstrapped: call pc_bootstrap() first");
        goto done;
    }
    enc_id = urlenc(pin_id);
    if (!enc_id) {
        fail(c, err, "out of memory");
        goto done;
    }
    snprintf(source, sizeof source, "/pin/%s/", enc_id);

    data = detail_data(pin_id, "detailed");
    if (!data) {
        fail(c, err, "out of memory building detail request");
        goto done;
    }

    if (!rpc_locked(c, "PinResource", source, data, &r, err))
        goto done;
    /* Best effort.  Guest sessions cannot resolve unified `e1_...` ids (404);
     * an authenticated session is untested, so try one alternate
     * field_set_key exactly once.  Never probe auth from here (non-blocking)
     * and never regress the guest path. */
    if (r.status == 404 && c->probe_done &&
        c->auth_state == PC_AUTH_LOGGED_IN) {
        char *alt = detail_data(pin_id, "unauth_react_main_pin");

        if (alt) {
            free(data);
            data = alt;
            pc_http_resp_free(&r);
            if (!rpc_locked(c, "PinResource", source, data, &r, err))
                goto done;
        }
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "pin detail unavailable: HTTP %ld (%s)", r.status,
             snippet);
        goto done;
    }
    if (!pc_json_parse_pin(r.body, r.body_len, pin_id, out, &jerr)) {
        fail(c, err, "pin detail parse: %s", jerr ? jerr : "unknown error");
        free(jerr);
        jerr = NULL;
        goto done;
    }
    ok = true;
    pc_client_clear_error(c);

done:
    /* Authenticated numeric ids: enrich with the v3 close-up entity (social
     * counts, and basic fields when PinResource had nothing).  Best effort. */
    if (c->probe_done && c->auth_state == PC_AUTH_LOGGED_IN &&
        pin_id_all_digits(pin_id))
        closeup_enrich_locked(c, pin_id, out, &ok);
    free(jerr);
    free(data);
    free(enc_id);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* ------------------------------------------------------- session cookie I/O */

/* Caller holds c->mu.  Drop all session-derived state after a jar swap. */
static void reset_session_locked(PcClient *c)
{
    c->bootstrapped = false;
    c->csrf[0]      = '\0';
    c->probe_done   = false;
    c->auth_state   = PC_AUTH_UNKNOWN;
    pthread_mutex_lock(&c->err_mu);
    c->last_error[0] = '\0';
    c->username[0]   = '\0';
    pthread_mutex_unlock(&c->err_mu);
    pc_pinlist_free(&c->homefeed_cache);
}

bool pc_cookie_load(PcClient *c, const char *path, char **err)
{
    char ebuf[512];
    int  n = 0;
    bool ok;

    if (err)
        *err = NULL;
    if (!c || !path || !*path) {
        if (err)
            *err = strdup("cookie load: invalid cookie file path");
        return false;
    }
    ebuf[0] = '\0';
    pthread_mutex_lock(&c->mu);
    ok = pc_http_cookie_load(c->http, path, &n, ebuf, sizeof ebuf);
    if (ok) {
        reset_session_locked(c);
        if (pc_http_cookie(c->http, "csrftoken", c->csrf, sizeof c->csrf))
            c->bootstrapped = true; /* jar may already carry a usable session */
        pc_client_clear_error(c);
        if (getenv("PC_DEBUG_COOKIES")) /* counts only: never log values */
            fprintf(stderr, "pinfall: loaded %d cookies from %s\n", n, path);
    } else {
        pc_client_set_error(c, "%s", ebuf[0] ? ebuf : "cookie load failed");
        if (err)
            *err = strdup(ebuf[0] ? ebuf : "cookie load failed");
    }
    pthread_mutex_unlock(&c->mu);
    return ok;
}

bool pc_cookie_save(PcClient *c, const char *path, char **err)
{
    char ebuf[512];
    bool ok;

    if (err)
        *err = NULL;
    if (!c || !path || !*path) {
        if (err)
            *err = strdup("cookie save: invalid cookie file path");
        return false;
    }
    ebuf[0] = '\0';
    pthread_mutex_lock(&c->mu);
    ok = pc_http_cookie_save(c->http, path, ebuf, sizeof ebuf);
    if (ok) {
        pc_client_clear_error(c);
    } else {
        pc_client_set_error(c, "%s", ebuf[0] ? ebuf : "cookie save failed");
        if (err)
            *err = strdup(ebuf[0] ? ebuf : "cookie save failed");
    }
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* --------------------------------------------------------- auth / homefeed */

static bool pinlist_copy(const PcPinList *src, PcPinList *dst)
{
    memset(dst, 0, sizeof *dst);
    if (src->count) {
        dst->items = malloc(src->count * sizeof *dst->items);
        if (!dst->items)
            return false;
        memcpy(dst->items, src->items, src->count * sizeof *dst->items);
        dst->count = src->count;
        dst->cap   = src->count;
    }
    memcpy(dst->bookmark, src->bookmark, sizeof dst->bookmark);
    return true;
}

/* Caller holds c->mu.  POST UserHomefeedResource/get (personal feed; guests get
 * HTTP 401).  Returns false only on transport failure. */
static bool homefeed_rpc_locked(PcClient *c, const char *bookmark,
                                PcHttpResp *r, char **err)
{
    cJSON *root = NULL;
    cJSON *opts;
    cJSON *bms;
    char  *data = NULL;
    bool   ok = false;

    memset(r, 0, sizeof *r);
    root = cJSON_CreateObject();
    opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
    bms  = opts ? cJSON_AddArrayToObject(opts, "bookmarks") : NULL;
    if (!root || !opts || !bms
        || !cJSON_AddNumberToObject(opts, "page_size", 25)
        || !cJSON_AddObjectToObject(root, "context")) {
        fail(c, err, "out of memory building homefeed request");
        goto done;
    }
    if (bookmark && *bookmark) {
        cJSON *bm = cJSON_CreateString(bookmark);

        if (!bm) {
            fail(c, err, "out of memory building bookmark");
            goto done;
        }
        cJSON_AddItemToArray(bms, bm);
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising homefeed request");
        goto done;
    }
    ok = rpc_locked(c, "UserHomefeedResource", "/", data, r, err);

done:
    free(data);
    if (root)
        cJSON_Delete(root);
    return ok;
}

/* Caller holds c->mu.  At most one probe per client, ever. */
static PcAuthState auth_check_locked(PcClient *c)
{
    PcHttpResp r;

    if (c->probe_done)
        return c->auth_state;
    c->probe_done = true; /* one probe, even when it fails */
    c->auth_state = PC_AUTH_UNKNOWN;

    if (!c->bootstrapped && !bootstrap_locked(c, NULL))
        return c->auth_state;

    memset(&r, 0, sizeof r);
    if (!homefeed_rpc_locked(c, NULL, &r, NULL)) {
        pc_http_resp_free(&r);
        return c->auth_state; /* transport failure -> UNKNOWN, no retry */
    }
    if (r.status == 200) {
        PcPinList list;
        char     *jerr = NULL;

        bool parsed;

        memset(&list, 0, sizeof list);
        /* Homefeed payloads can be data.results or a bare data[] list. */
        ads_parse_begin(c);
        parsed = pc_json_parse_pins(r.body, r.body_len, &list, &jerr);
        ads_parse_end(c);
        if (parsed) {
            c->auth_state = PC_AUTH_LOGGED_IN;
            pc_pinlist_free(&c->homefeed_cache);
            c->homefeed_cache = list; /* probe results double as feed page 1 */
            memset(&list, 0, sizeof list);
            pc_client_clear_error(c);
        }
        pc_pinlist_free(&list);
        free(jerr);
    } else if (r.status == 401) {
        c->auth_state = PC_AUTH_GUEST;
        pc_pinlist_free(&c->homefeed_cache);
    }
    pc_http_resp_free(&r);
    return c->auth_state;
}

PcAuthState pc_auth_check(PcClient *c)
{
    PcAuthState st;

    if (!c)
        return PC_AUTH_UNKNOWN;
    pthread_mutex_lock(&c->mu);
    st = auth_check_locked(c);
    pthread_mutex_unlock(&c->mu);
    return st;
}

const char *pc_username(PcClient *c)
{
    static _Thread_local char buf[PC_SHORT_LEN];

    buf[0] = '\0';
    if (!c)
        return buf;
    pthread_mutex_lock(&c->err_mu);
    memcpy(buf, c->username, sizeof buf);
    pthread_mutex_unlock(&c->err_mu);
    return buf;
}

bool pc_homefeed(PcClient *c, const char *bookmark, PcPinList *out, char **err)
{
    PcHttpResp  r;
    char       *jerr = NULL;
    char        snippet[200];
    bool        ok = false;

    if (err)
        *err = NULL;
    if (!out) {
        if (err)
            *err = strdup("invalid arguments: out is NULL");
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!c) {
        if (err)
            *err = strdup("invalid arguments");
        return false;
    }

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);

    if (!c->bootstrapped && !bootstrap_locked(c, err))
        goto done;

    {
        PcAuthState st = auth_check_locked(c);

        if (st == PC_AUTH_GUEST) {
            /* Remembered guest: fail fast, no second network call. */
            fail(c, err,
                 "login required: not signed in. Import your browser's "
                 "Pinterest cookies (cookies.txt) and retry.");
            goto done;
        }
        if (st == PC_AUTH_LOGGED_IN && (!bookmark || !*bookmark) &&
            c->homefeed_cache.count > 0) {
            if (!pinlist_copy(&c->homefeed_cache, out)) {
                fail(c, err, "out of memory");
                goto done;
            }
            ok = true;
            pc_client_clear_error(c);
            goto done;
        }
    }

    if (!homefeed_rpc_locked(c, bookmark, &r, err))
        goto done;
    if (r.status == 401) {
        c->probe_done = true;
        c->auth_state = PC_AUTH_GUEST;
        pc_pinlist_free(&c->homefeed_cache);
        fail(c, err,
             "login required: home feed needs a signed-in session (HTTP 401)");
        goto done;
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "homefeed: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }
    /* Homefeed payloads are data.results OR a bare data[] list (verified live
     * on an authenticated session), so use the list-aware parser. */
    ads_parse_begin(c);
    ok = pc_json_parse_pins(r.body, r.body_len, out, &jerr);
    ads_parse_end(c);
    if (!ok) {
        fail(c, err, "homefeed parse: %s", jerr ? jerr : "unknown error");
        ok = false;
        goto done;
    }
    /* A successful feed proves the session works (even if the probe was
     * inconclusive earlier). */
    if (c->auth_state != PC_AUTH_LOGGED_IN) {
        c->auth_state = PC_AUTH_LOGGED_IN;
        c->probe_done = true;
    }
    ok = true;
    pc_client_clear_error(c);

done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    free(jerr);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* ------------------------------------------------ saved pins / boards (v3) */

/* Caller holds c->mu.  NULL/"" means "the signed-in user".  Returns false when
 * no username is known (guest, no session). */
static bool resolve_username_locked(PcClient *c, const char *username,
                                    char *out, size_t outsz)
{
    if (username && *username) {
        snprintf(out, outsz, "%s", username);
        return true;
    }
    pthread_mutex_lock(&c->err_mu);
    snprintf(out, outsz, "%s", c->username);
    pthread_mutex_unlock(&c->err_mu);
    return out[0] != '\0';
}

static const char *json_obj_str(cJSON *obj, const char *key)
{
    cJSON *v;

    if (!cJSON_IsObject(obj))
        return NULL;
    v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

bool pc_user_boards(PcClient *c, const char *username, PcBoardList *out,
                    char **err)
{
    cJSON      *root = NULL;
    cJSON      *opts;
    char       *data = NULL;
    char        user[PC_SHORT_LEN];
    char        source[1600];
    char        snippet[200];
    PcHttpResp  r;
    char       *jerr = NULL;
    bool        have_user;
    bool        ok = false;

    if (err)
        *err = NULL;
    if (!out) {
        if (err)
            *err = strdup("invalid arguments: out is NULL");
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!c) {
        if (err)
            *err = strdup("invalid arguments");
        return false;
    }

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);

    if (!c->bootstrapped && !bootstrap_locked(c, err))
        goto done;
    have_user = resolve_username_locked(c, username, user, sizeof user);
    snprintf(source, sizeof source, "%s%s%s", "/", have_user ? user : "",
             have_user ? "/" : "");

    root = cJSON_CreateObject();
    opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
    if (!root || !opts || !cJSON_AddNumberToObject(opts, "page_size", 25)
        || !cJSON_AddStringToObject(opts, "field_set_key", "profile_grid_item")
        || !cJSON_AddObjectToObject(root, "context")
        || (have_user && !cJSON_AddStringToObject(opts, "username", user))) {
        fail(c, err, "out of memory building boards request");
        goto done;
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising boards request");
        goto done;
    }

    if (!rpc_locked(c, "BoardsResource", source, data, &r, err))
        goto done;
    if (r.status == 401) {
        fail(c, err,
             "login required: boards of the signed-in user (HTTP 401)");
        goto done;
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        if (!have_user)
            fail(c, err,
                 "boards: HTTP %ld (%s); no signed-in user known - pass a "
                 "username", r.status, snippet);
        else
            fail(c, err, "boards: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }
    if (!pc_json_parse_boards(r.body, r.body_len, out, &jerr)) {
        if (!have_user)
            fail(c, err,
                 "boards: no signed-in user known; pass a username (%s)",
                 jerr ? jerr : "parse failed");
        else
            fail(c, err, "boards parse: %s", jerr ? jerr : "unknown error");
        goto done;
    }
    ok = true;
    pc_client_clear_error(c);

done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    free(jerr);
    free(data);
    if (root)
        cJSON_Delete(root);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

bool pc_user_pins(PcClient *c, const char *username, const char *bookmark,
                  PcPinList *out, char **err)
{
    cJSON      *root = NULL;
    cJSON      *opts;
    cJSON      *bms;
    char       *data = NULL;
    char        user[PC_SHORT_LEN];
    char        source[1600];
    char        snippet[200];
    PcHttpResp  r;
    char       *jerr = NULL;
    bool        have_user;
    bool        ok = false;

    if (err)
        *err = NULL;
    if (!out) {
        if (err)
            *err = strdup("invalid arguments: out is NULL");
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!c) {
        if (err)
            *err = strdup("invalid arguments");
        return false;
    }

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);

    if (!c->bootstrapped && !bootstrap_locked(c, err))
        goto done;
    have_user = resolve_username_locked(c, username, user, sizeof user);
    snprintf(source, sizeof source, "%s%s%s", "/", have_user ? user : "",
             have_user ? "/" : "");

    root = cJSON_CreateObject();
    opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
    bms  = opts ? cJSON_AddArrayToObject(opts, "bookmarks") : NULL;
    if (!root || !opts || !bms || !cJSON_AddNumberToObject(opts, "page_size", 25)
        || !cJSON_AddObjectToObject(root, "context")
        || (have_user && !cJSON_AddStringToObject(opts, "username", user))) {
        fail(c, err, "out of memory building user-pins request");
        goto done;
    }
    if (bookmark && *bookmark) {
        cJSON *bm = cJSON_CreateString(bookmark);

        if (!bm) {
            fail(c, err, "out of memory building bookmark");
            goto done;
        }
        cJSON_AddItemToArray(bms, bm);
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising user-pins request");
        goto done;
    }

    if (!rpc_locked(c, "UserPinsResource", source, data, &r, err))
        goto done;
    if (r.status == 401) {
        fail(c, err,
             "login required: saved pins of the signed-in user (HTTP 401)");
        goto done;
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        if (!have_user)
            fail(c, err,
                 "user pins: HTTP %ld (%s); no signed-in user known - pass a "
                 "username", r.status, snippet);
        else
            fail(c, err, "user pins: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }
    ads_parse_begin(c);
    ok = pc_json_parse_pins(r.body, r.body_len, out, &jerr);
    ads_parse_end(c);
    if (!ok) {
        ok = false;
        if (!have_user)
            fail(c, err,
                 "user pins: no signed-in user known; pass a username (%s)",
                 jerr ? jerr : "parse failed");
        else
            fail(c, err, "user pins parse: %s", jerr ? jerr : "unknown error");
        goto done;
    }
    ok = true;
    pc_client_clear_error(c);

done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    free(jerr);
    free(data);
    if (root)
        cJSON_Delete(root);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

bool pc_save_pin(PcClient *c, const char *pin_id, const char *board_id,
                 char **err)
{
    cJSON      *root = NULL;
    cJSON      *opts;
    cJSON      *resp_root = NULL;
    char       *data = NULL;
    char       *enc_pin = NULL;
    char        source[700];
    char        snippet[200];
    PcHttpResp  r;
    PcAuthState st;
    bool        ok = false;

    if (err)
        *err = NULL;
    if (!c || !pin_id || !*pin_id || !board_id || !*board_id) {
        if (err)
            *err = strdup("invalid arguments: pin_id and board_id are required");
        return false;
    }

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);

    /* Short-circuit: never send the mutation unless a real session exists.
     * auth_check_locked() may run the single cached auth probe, but no write
     * request leaves the machine on failure. */
    st = auth_check_locked(c);
    if (st != PC_AUTH_LOGGED_IN) {
        fail(c, err, "login required: saving a pin needs a signed-in session");
        goto done;
    }
    enc_pin = urlenc(pin_id);
    if (!enc_pin) {
        fail(c, err, "out of memory");
        goto done;
    }
    snprintf(source, sizeof source, "/pin/%s/", enc_pin);

    root = cJSON_CreateObject();
    opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
    if (!root || !opts || !cJSON_AddStringToObject(opts, "pin_id", pin_id)
        || !cJSON_AddStringToObject(opts, "board_id", board_id)
        || !cJSON_AddObjectToObject(root, "context")) {
        fail(c, err, "out of memory building save request");
        goto done;
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising save request");
        goto done;
    }

    /* WRITE: exactly one attempt.  No transport retry, no 403 re-bootstrap. */
    if (!rpc_verb_locked(c, "RepinResource", "create", source, data, false, &r,
                         err))
        goto done;
    if (r.status == 401 || r.status == 403) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "login required: saving failed (HTTP %ld) (%s)", r.status,
             snippet);
        goto done;
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "save: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }

    /* Success requires the envelope to say status=="success". */
    resp_root = cJSON_ParseWithLength(r.body, r.body_len);
    {
        cJSON      *rr = resp_root
                            ? cJSON_GetObjectItemCaseSensitive(resp_root,
                                                               "resource_response")
                            : NULL;
        const char *status = json_obj_str(rr, "status");

        if (status && strcmp(status, "success") == 0) {
            ok = true;
            pc_client_clear_error(c);
        } else {
            cJSON      *eobj = cJSON_GetObjectItemCaseSensitive(rr, "error");
            const char *msg = cJSON_IsObject(eobj) ? json_obj_str(eobj, "message")
                                                   : NULL;

            if (!msg)
                msg = json_obj_str(rr, "message");
            fail(c, err, "save not confirmed by server (status=%s%s%s)",
                 status ? status : "missing", msg ? ": " : "", msg ? msg : "");
        }
    }

done:
    if (resp_root)
        cJSON_Delete(resp_root);
    free(data);
    free(enc_pin);
    if (root)
        cJSON_Delete(root);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

void pc_boardlist_free(PcBoardList *l)
{
    if (!l)
        return;
    free(l->items);
    memset(l, 0, sizeof *l);
}

/* ----------------------------------------------------------- ad blocking (v5) */

void pc_set_block_ads(PcClient *c, bool block)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->mu);
    if (c->block_ads != block) {
        c->block_ads = block;
        /* Cached feed page was parsed under the old policy: refetch/filter. */
        pc_pinlist_free(&c->homefeed_cache);
    }
    pthread_mutex_unlock(&c->mu);
}

bool pc_get_block_ads(PcClient *c)
{
    bool block;

    if (!c)
        return false;
    pthread_mutex_lock(&c->mu);
    block = c->block_ads;
    pthread_mutex_unlock(&c->mu);
    return block;
}

uint64_t pc_ads_blocked(PcClient *c)
{
    uint64_t n;

    if (!c)
        return 0;
    pthread_mutex_lock(&c->mu);
    n = c->ads_blocked;
    pthread_mutex_unlock(&c->mu);
    return n;
}

/* --------------------------------------- close-up API: related/comments (v6) */

bool pc_pin_related(PcClient *c, const char *pin_id, const char *bookmark,
                    PcPinList *out, char **err)
{
    PcHttpResp r;
    cJSON     *root = NULL;
    cJSON     *groot = NULL;
    char      *data = NULL;
    char      *vars = NULL;
    char      *data_json = NULL;
    char      *jerr = NULL;
    char       source[700];
    bool       ok = false;
    bool       rest_parsed = false;

    if (err)
        *err = NULL;
    if (!out) {
        if (err)
            *err = strdup("invalid arguments: out is NULL");
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!c)
        return false;

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);
    if (!canonical_pin_or_fail(c, pin_id, err))
        goto done;
    if (!c->bootstrapped && !bootstrap_locked(c, err))
        goto done;

    /* Primary: REST RelatedModulesResource (verified live on nl.pinterest.com). */
    root = cJSON_CreateObject();
    {
        cJSON *opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
        cJSON *fields = NULL;

        if (opts) {
            cJSON_AddStringToObject(opts, "pin_id", pin_id);
            cJSON_AddNumberToObject(opts, "page_size", 24);
            cJSON_AddStringToObject(opts, "source", "deep_linking");
            cJSON_AddStringToObject(opts, "top_level_source", "deep_linking");
            cJSON_AddNumberToObject(opts, "top_level_source_depth", 1);
            cJSON_AddBoolToObject(opts, "is_pdp", false);
            cJSON_AddStringToObject(opts, "search_query", "");
            cJSON_AddArrayToObject(opts, "context_pin_ids");
            cJSON_AddArrayToObject(opts, "context_near_dup_image_sigs");
            cJSON_AddNullToObject(opts, "homefeed_source_sig");
            fields = cJSON_AddArrayToObject(opts, "additional_fields");
            if (fields) {
                cJSON *f = cJSON_CreateString("pin.gen_ai_topics");

                if (f)
                    cJSON_AddItemToArray(fields, f);
            }
            if (bookmark && *bookmark) {
                cJSON *bms = cJSON_AddArrayToObject(opts, "bookmarks");
                cJSON *bm = bms ? cJSON_CreateString(bookmark) : NULL;

                if (bm)
                    cJSON_AddItemToArray(bms, bm);
            }
        }
        if (!root || !opts || !fields
            || !cJSON_AddObjectToObject(root, "context")) {
            fail(c, err, "out of memory building related request");
            goto done;
        }
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising related request");
        goto done;
    }
    snprintf(source, sizeof source, "/pin/%s/", pin_id);
    if (!rpc_locked(c, "RelatedModulesResource", source, data, &r, err))
        goto done;
    if (r.status == 401) {
        fail(c, err, "login required: related pins need a signed-in session");
        goto done;
    }
    if (r.status == 200) {
        ads_parse_begin(c);
        rest_parsed = pc_json_parse_pins(r.body, r.body_len, out, &jerr);
        ads_parse_end(c);
        if (rest_parsed) {
            ok = true;
            pc_client_clear_error(c);
            goto done;
        }
    }

    /* Optional GraphQL fallback (queryHash in graphql.c). */
    pc_http_resp_free(&r);
    free(jerr);
    jerr = NULL;
    free(data);
    data = NULL;
    vars = gql_related_vars(pin_id);
    if (!vars) {
        fail(c, err, "out of memory");
        goto done;
    }
    if (!pc_gql_post(c->http, PC_GQL_OP_RELATED, source, vars, PC_GQL_READ,
                     &r, &jerr)) {
        fail(c, err, "related: %s", jerr ? jerr : pc_last_error(c));
        goto done;
    }
    if (r.status == 401) {
        fail(c, err, "login required: related pins need a signed-in session");
        goto done;
    }
    groot = pc_gql_parse(&r, &jerr);
    if (!groot) {
        fail(c, err, "related: %s", jerr ? jerr : "graphql error");
        goto done;
    }
    {
        cJSON *gdata = cJSON_GetObjectItemCaseSensitive(groot, "data");

        data_json = cJSON_PrintUnformatted(gdata);
    }
    if (!data_json) {
        fail(c, err, "out of memory");
        goto done;
    }
    ads_parse_begin(c);
    ok = pc_json_collect_pins_any(data_json, strlen(data_json), out, &jerr);
    ads_parse_end(c);
    if (!ok) {
        fail(c, err, "related parse: %s", jerr ? jerr : "unknown error");
        goto done;
    }
    pc_client_clear_error(c);

done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    free(jerr);
    free(data_json);
    free(vars);
    free(data);
    if (root)
        cJSON_Delete(root);
    if (groot)
        cJSON_Delete(groot);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

bool pc_pin_comments(PcClient *c, const char *pin_id, const char *bookmark,
                     PcCommentList *out, char **err)
{
    PcHttpResp r;
    cJSON     *root = NULL;
    char      *data = NULL;
    char      *jerr = NULL;
    char       source[700];
    char       agg[PC_ID_LEN];
    char       snippet[200];
    bool       ok = false;

    if (err)
        *err = NULL;
    if (!out) {
        if (err)
            *err = strdup("invalid arguments: out is NULL");
        return false;
    }
    memset(out, 0, sizeof *out);
    if (!c)
        return false;

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);
    if (!canonical_pin_or_fail(c, pin_id, err))
        goto done;
    if (!c->bootstrapped && !bootstrap_locked(c, err))
        goto done;

    /* Comments are keyed by aggregated_pin_data.id (cached per pin). */
    if (!closeup_agg_id_locked(c, pin_id, agg, sizeof agg, err))
        goto done;

    root = cJSON_CreateObject();
    {
        cJSON *opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
        cJSON *ids = NULL;

        if (opts) {
            cJSON_AddStringToObject(opts, "aggregated_pin_id", agg);
            ids = cJSON_AddArrayToObject(opts, "comment_featured_ids");
            cJSON_AddNumberToObject(opts, "page_size", 20);
            cJSON_AddBoolToObject(opts, "redux_normalize_feed", true);
            cJSON_AddBoolToObject(opts, "is_reversed", false);
            if (bookmark && *bookmark) {
                cJSON *bms = cJSON_AddArrayToObject(opts, "bookmarks");
                cJSON *bm = bms ? cJSON_CreateString(bookmark) : NULL;

                if (bm)
                    cJSON_AddItemToArray(bms, bm);
            }
        }
        if (!root || !opts || !ids
            || !cJSON_AddObjectToObject(root, "context")) {
            fail(c, err, "out of memory building comments request");
            goto done;
        }
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising comments request");
        goto done;
    }
    snprintf(source, sizeof source, "/pin/%s/", pin_id);
    if (!rpc_locked(c, "UnifiedCommentsResource", source, data, &r, err))
        goto done;
    if (r.status == 401) {
        fail(c, err, "login required: comments need a signed-in session");
        goto done;
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "comments: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }
    ok = pc_json_collect_comments_any(r.body, r.body_len, out, &jerr);
    if (!ok)
        fail(c, err, "comments parse: %s", jerr ? jerr : "unknown error");
    else
        pc_client_clear_error(c);

done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    free(jerr);
    free(data);
    if (root)
        cJSON_Delete(root);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

bool pc_add_comment(PcClient *c, const char *pin_id, const char *text,
                    char **err)
{
    PcHttpResp r;
    cJSON     *root = NULL;
    char      *data = NULL;
    char       source[700];
    char       snippet[200];
    char       agg[PC_ID_LEN];
    char      *jerr = NULL;
    bool       ok = false;

    if (err)
        *err = NULL;
    if (!c || !pin_id || !text || !*text) {
        if (err)
            *err = strdup("invalid arguments: pin_id and text are required");
        return false;
    }
    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);
    if (!canonical_pin_or_fail(c, pin_id, err))
        goto done;
    if (auth_check_locked(c) != PC_AUTH_LOGGED_IN) {
        fail(c, err, "login required: adding a comment needs a signed-in "
                     "session");
        goto done;
    }
    if (!c->bootstrapped && !bootstrap_locked(c, err))
        goto done;
    /* WRITE payload quirk (verified with an empty-text probe, which the server
     * rejects at validation and therefore creates nothing): objectId must be
     * the pin's aggregated_pin_data.id, not the pin id — otherwise Pinterest
     * answers HTTP 404 / code 880 "Aggregated pin data could not be found". */
    if (!closeup_agg_id_locked(c, pin_id, agg, sizeof agg, err))
        goto done;
    root = cJSON_CreateObject();
    {
        cJSON *opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;

        if (opts) {
            cJSON_AddBoolToObject(opts, "force", true);
            cJSON_AddStringToObject(opts, "objectId", agg);
            cJSON_AddStringToObject(opts, "pinId", pin_id);
            cJSON_AddStringToObject(opts, "tags", "[]");
            cJSON_AddStringToObject(opts, "text", text);
            cJSON_AddNullToObject(opts, "replyToCommentId");
        }
        if (!root || !opts || !cJSON_AddObjectToObject(root, "context")) {
            fail(c, err, "out of memory building comment request");
            goto done;
        }
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising comment request");
        goto done;
    }
    snprintf(source, sizeof source, "/pin/%s/", pin_id);
    if (getenv("PC_ADD_COMMENT_DRYRUN")) {
        /* Test hook: show the exact payload without writing anything. */
        fprintf(stderr, "pinfall-comment-dryrun: %s\n", data);
        fail(c, err, "dry-run: comment not sent");
        goto done;
    }
    /* WRITE: single-shot, never retried (verb create). */
    if (!resource_rpc_locked(c, "AggregatedCommentResource", "create", source,
                             data, false, &r, err))
        goto done;
    if (r.status == 401 || r.status == 403) {
        fail(c, err, "login required: add comment refused (HTTP %ld)", r.status);
        goto done;
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "add comment: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }
    if (!resource_success(&r, &jerr)) {
        fail(c, err, "add comment: %s", jerr ? jerr : "not confirmed");
        goto done;
    }
    ok = true;
    pc_client_clear_error(c);

done:
    free(jerr);
    free(data);
    if (root)
        cJSON_Delete(root);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

bool pc_set_pin_liked(PcClient *c, const char *pin_id, bool liked, char **err)
{
    PcHttpResp r;
    cJSON     *root = NULL;
    char      *data = NULL;
    char       source[700];
    char       url[300];
    char       snippet[200];
    char      *jerr = NULL;
    bool       ok = false;

    if (err)
        *err = NULL;
    if (!c || !pin_id || !*pin_id) {
        if (err)
            *err = strdup("invalid arguments");
        return false;
    }
    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);
    if (!canonical_pin_or_fail(c, pin_id, err))
        goto done;
    if (auth_check_locked(c) != PC_AUTH_LOGGED_IN) {
        fail(c, err, "login required: liking a pin needs a signed-in session");
        goto done;
    }
    snprintf(url, sizeof url, "/v3/pins/%s/react/", pin_id);
    root = cJSON_CreateObject();
    {
        cJSON *opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
        cJSON *rdata = opts ? cJSON_AddObjectToObject(opts, "data") : NULL;

        if (opts)
            cJSON_AddStringToObject(opts, "url", url);
        if (rdata && liked) /* create carries reaction_type; delete does not */
            cJSON_AddNumberToObject(rdata, "reaction_type", 1);
        if (!root || !opts || !rdata
            || !cJSON_AddObjectToObject(root, "context")) {
            fail(c, err, "out of memory building reaction request");
            goto done;
        }
    }
    data = cJSON_PrintUnformatted(root);
    if (!data) {
        fail(c, err, "out of memory serialising reaction request");
        goto done;
    }
    snprintf(source, sizeof source, "/pin/%s/", pin_id);
    /* WRITE: like = ApiResource/create; unlike = ApiResource/delete.
     * Single-shot, never retried. */
    if (!resource_rpc_locked(c, "ApiResource", liked ? "create" : "delete",
                             source, data, false, &r, err))
        goto done;
    if (r.status == 401 || r.status == 403) {
        fail(c, err, "login required: like refused (HTTP %ld)", r.status);
        goto done;
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "like: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }
    if (!resource_success(&r, &jerr)) {
        fail(c, err, "like: %s", jerr ? jerr : "not confirmed");
        goto done;
    }
    ok = true;
    pc_client_clear_error(c);

done:
    free(jerr);
    free(data);
    if (root)
        cJSON_Delete(root);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

void pc_commentlist_free(PcCommentList *l)
{
    if (!l)
        return;
    free(l->items);
    memset(l, 0, sizeof *l);
}

/* --------------------------------------------------- board creation (v7) -- */

static void fill_created_board(cJSON *obj, PcBoard *b)
{
    cJSON      *v;
    const char *s;
    int         n;

    if (!cJSON_IsObject(obj))
        return;
    s = json_obj_str(obj, "id");
    if (s && *s) {
        copy_field_api(b->id, sizeof b->id, s);
    } else {
        v = cJSON_GetObjectItemCaseSensitive(obj, "id");
        if (cJSON_IsNumber(v))
            snprintf(b->id, sizeof b->id, "%.0f", v->valuedouble);
    }
    copy_field_api(b->name, sizeof b->name, json_obj_str(obj, "name"));
    copy_url_api(b->url, sizeof b->url, json_obj_str(obj, "url"));
    copy_field_api(b->privacy, sizeof b->privacy,
                   json_obj_str(obj, "privacy"));
    if (obj_int(obj, "pin_count", &n) && n > 0)
        b->pin_count = n;
}

/* Defensive: board object can be resource_response.data, data.board, or absent
 * (some create responses only echo the new id as a string). */
static void parse_created_board(const PcHttpResp *r, PcBoard *out)
{
    cJSON *root;
    cJSON *rr;
    cJSON *data;
    cJSON *board = NULL;

    if (!r || !r->body)
        return;
    root = cJSON_ParseWithLength(r->body, r->body_len);
    if (!root)
        return;
    rr = cJSON_GetObjectItemCaseSensitive(root, "resource_response");
    data = cJSON_IsObject(rr)
               ? cJSON_GetObjectItemCaseSensitive(rr, "data")
               : NULL;
    if (cJSON_IsObject(data)) {
        board = cJSON_GetObjectItemCaseSensitive(data, "board");
        if (!cJSON_IsObject(board))
            board = data;
        if (cJSON_IsObject(board))
            fill_created_board(board, out);
    }
    /* Any other data shape (string/list/null) is ignored: success is enough,
     * the UI refreshes pc_user_boards(). */
    cJSON_Delete(root);
}

static void normalize_privacy(const char *privacy, char *dst, size_t dstsz)
{
    if (!privacy || !*privacy) {
        snprintf(dst, dstsz, "%s", "public");
        return;
    }
    if (strcasecmp(privacy, "secret") == 0 ||
        strcasecmp(privacy, "private") == 0) {
        snprintf(dst, dstsz, "%s", "secret");
        return;
    }
    snprintf(dst, dstsz, "%s", "public");
}

bool pc_create_board(PcClient *c, const char *name, const char *description,
                     const char *privacy, PcBoard *out, char **err)
{
    char        priv[PC_PRIVACY_LEN];
    char        user[PC_SHORT_LEN];
    char        source[PC_URL_LEN];
    char        snippet[200] = "";
    PcHttpResp  r;
    bool        ok = false;
    int         attempt;

    if (err)
        *err = NULL;
    if (out)
        memset(out, 0, sizeof *out);
    if (!c || !name || !*name) {
        if (err)
            *err = strdup("invalid arguments: a board name is required");
        return false;
    }
    if (!out) {
        if (err)
            *err = strdup("invalid arguments: out is NULL");
        return false;
    }
    normalize_privacy(privacy, priv, sizeof priv);

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);
    if (auth_check_locked(c) != PC_AUTH_LOGGED_IN) {
        fail(c, err,
             "login required: creating a board needs a signed-in session");
        goto done;
    }
    pthread_mutex_lock(&c->err_mu);
    snprintf(user, sizeof user, "%s", c->username);
    pthread_mutex_unlock(&c->err_mu);
    if (user[0])
        snprintf(source, sizeof source, "/%s/", user);
    else
        snprintf(source, sizeof source, "%s", "/");

    /* Candidate option shapes, in order.  Only a 400 ("Invalid parameters")
     * advances to the next shape; success or any other failure stops.  Each
     * attempt is a single-shot POST (writes are not idempotent). */
    for (attempt = 0; attempt < 3; attempt++) {
        cJSON *root = cJSON_CreateObject();
        cJSON *opts = root ? cJSON_AddObjectToObject(root, "options") : NULL;
        char  *data = NULL;
        int    nkeys = 3 - attempt;

        if (!root || !opts)
            break;
        cJSON_AddStringToObject(opts, "name", name);
        if (nkeys >= 2)
            cJSON_AddStringToObject(opts, "description",
                                    description ? description : "");
        if (nkeys >= 3)
            cJSON_AddStringToObject(opts, "privacy", priv);
        cJSON_AddObjectToObject(root, "context");
        data = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!data) {
            fail(c, err, "out of memory building create-board request");
            goto done;
        }
        if (!resource_rpc_locked(c, "BoardResource", "create", source, data,
                                 false, &r, err)) {
            free(data);
            goto done; /* transport failure: no retry, no other shapes */
        }
        free(data);

        if (r.status == 200) {
            char *jerr = NULL;

            if (!resource_success(&r, &jerr)) {
                fail(c, err, "create board: %s",
                     jerr ? jerr : "server rejected the request");
                free(jerr);
                goto done;
            }
            parse_created_board(&r, out); /* best effort; may stay zeroed */
            ok = true;
            pc_client_clear_error(c);
            goto done;
        }
        if (r.status == 401 || r.status == 403) {
            fail(c, err,
                 "login required: create board refused (HTTP %ld)", r.status);
            goto done;
        }
        if (r.status == 400) {
            body_snippet(r.body, r.body_len, snippet, sizeof snippet);
            pc_http_resp_free(&r);
            continue; /* invalid parameter shape -> try the next one */
        }
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "create board: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }
    fail(c, err, "create board: rejected for all parameter shapes (%s)",
         snippet[0] ? snippet : "HTTP 400");

done:
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* ---------------------------------------------- video metadata (v8) ------- */

/* GraphQL close-up is the only live source of a video pin's video URLs for a
 * numeric id (the REST v3 entity only exposes videos.id).  Read-only. */
bool pc_pin_video_info(PcClient *c, const char *pin_id, PcPin *out, char **err)
{
    PcHttpResp  r;
    cJSON      *root = NULL;
    cJSON      *data;
    cJSON      *q;
    cJSON      *qd;
    char       *vars = NULL;
    char       *json = NULL;
    char       *jerr = NULL;
    char        source[700];
    char        snippet[200];
    bool        ok = false;

    if (err)
        *err = NULL;
    if (out)
        memset(out, 0, sizeof *out);
    if (!c || !pin_id || !*pin_id || !out) {
        if (err)
            *err = strdup("invalid arguments");
        return false;
    }

    pthread_mutex_lock(&c->mu);
    memset(&r, 0, sizeof r);
    if (!canonical_pin_or_fail(c, pin_id, err))
        goto done;
    if (!c->bootstrapped && !bootstrap_locked(c, err))
        goto done;

    {
        cJSON *vr = cJSON_CreateObject();

        if (!vr) {
            fail(c, err, "out of memory");
            goto done;
        }
        cJSON_AddStringToObject(vr, "pinId", pin_id);
        cJSON_AddBoolToObject(vr, "isAuth", true);
        cJSON_AddBoolToObject(vr, "isDesktop", true);
        cJSON_AddBoolToObject(vr, "isUnauth", false);
        cJSON_AddBoolToObject(vr, "isAuthDesktop", true);
        cJSON_AddBoolToObject(vr, "shouldPrefetchStoryPinFragment", false);
        vars = cJSON_PrintUnformatted(vr);
        cJSON_Delete(vr);
    }
    if (!vars) {
        fail(c, err, "out of memory");
        goto done;
    }
    snprintf(source, sizeof source, "/pin/%s/", pin_id);
    if (!pc_gql_post(c->http, PC_GQL_OP_CLOSEUP, source, vars, PC_GQL_READ,
                     &r, &jerr)) {
        fail(c, err, "video: %s", jerr ? jerr : pc_last_error(c));
        goto done;
    }
    if (r.status == 401) {
        fail(c, err, "login required: video metadata needs a signed-in session");
        goto done;
    }
    if (r.status == 404) {
        fail(c, err, "pin not found for close-up (numeric id required)");
        goto done;
    }
    if (r.status != 200) {
        body_snippet(r.body, r.body_len, snippet, sizeof snippet);
        fail(c, err, "video: HTTP %ld (%s)", r.status, snippet);
        goto done;
    }
    root = pc_gql_parse(&r, &jerr);
    if (!root) {
        fail(c, err, "video: %s", jerr ? jerr : "graphql error");
        goto done;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    q = cJSON_IsObject(data)
            ? cJSON_GetObjectItemCaseSensitive(data, "v3GetPinQueryv2")
            : NULL;
    qd = cJSON_IsObject(q)
             ? cJSON_GetObjectItemCaseSensitive(q, "data")
             : NULL;
    if (!cJSON_IsObject(qd)) {
        fail(c, err, "video: close-up payload has no pin data");
        goto done;
    }
    json = cJSON_PrintUnformatted(qd);
    if (!json) {
        fail(c, err, "out of memory");
        goto done;
    }
    {
        bool vok = pc_json_parse_video_pin(json, strlen(json), out, &jerr);
        bool usable = vok && out->is_video && out->video_url[0] != '\0';

        if (getenv("PC_DEBUG_DETAIL"))
            fprintf(stderr,
                    "DBG gqlparse ok=%d isv=%d url=%.40s jerr=%.60s\n",
                    vok ? 1 : 0, out->is_video, out->video_url,
                    jerr ? jerr : "");
        if (!usable) {
            /* Story pins (multi-page/idea pins) keep their video under
             * story_pin_data.pages[].video; the close-up GraphQL query can
             * return an empty videos{} for them, while the REST PinResource
             * entity carries the story data. (Verified with pin
             * 906419862523865948.) */
            char  *dd = detail_data(pin_id, "detailed");
            bool   got = false;

            if (dd) {
                PcHttpResp r2;
                PcPin      tmp;
                char      *j2 = NULL;

                memset(&r2, 0, sizeof r2);
                memset(&tmp, 0, sizeof tmp);
                if (rpc_locked(c, "PinResource", source, dd, &r2, NULL) &&
                    r2.status == 200 &&
                    pc_json_parse_pin(r2.body, r2.body_len, pin_id, &tmp,
                                      &j2) &&
                    tmp.is_video && tmp.video_url[0]) {
                    *out = tmp;
                    got  = true;
                }
                if (getenv("PC_DEBUG_DETAIL"))
                    fprintf(stderr,
                            "DBG video fallback dd=%d status=%ld parse=%d "
                            "isv=%d url=%.40s\n",
                            dd != NULL, r2.status, tmp.id[0] ? 1 : 0,
                            tmp.is_video, tmp.video_url);
                free(j2);
                pc_http_resp_free(&r2);
                free(dd);
            }
            if (!got) {
                /* Ordinary (non-video) pin: success with is_video == false. */
                cJSON *idv =
                    cJSON_GetObjectItemCaseSensitive(qd, "entityId");

                if (cJSON_IsString(idv) && idv->valuestring)
                    copy_field_api(out->id, sizeof out->id, idv->valuestring);
                if (!out->id[0])
                    copy_field_api(out->id, sizeof out->id, pin_id);
                copy_field_api(out->title, sizeof out->title,
                               json_obj_str(qd, "title"));
                out->is_video = false;
                out->video_url[0] = '\0';
            }
            free(jerr);
            jerr = NULL;
            ok = true;
            pc_client_clear_error(c);
            goto done;
        }
        free(jerr);
        jerr = NULL;
    }
    if (!out->id[0])
        copy_field_api(out->id, sizeof out->id, pin_id);
    ok = true;
    pc_client_clear_error(c);

done:
    if (!ok && out)
        memset(out, 0, sizeof *out);
    free(jerr);
    free(json);
    free(vars);
    if (root)
        cJSON_Delete(root);
    pc_http_resp_free(&r);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* --------------------------------------------------------------- helpers */

void pc_pinlist_free(PcPinList *l)
{
    if (!l)
        return;
    free(l->items);
    memset(l, 0, sizeof *l);
}

void pc_free(void *p)
{
    free(p);
}
