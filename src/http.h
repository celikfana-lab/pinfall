/* http.h - PinFall internal libcurl wrapper .
 *
 * Not part of the frozen public API in include/pc.h; used by api.c, img.c and
 * tools/dump.c only.  All functions are thread-safe: a single easy handle with
 * an in-memory cookie jar is serialised by an internal mutex.
 */
#ifndef PINFALL_HTTP_H
#define PINFALL_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pc.h"

#define PC_USER_AGENT \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 " \
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

typedef struct PcHttp PcHttp;

typedef struct {
    long   status;    /* HTTP status code, 0 if the transport failed */
    char  *body;      /* NUL terminated (may be empty string), malloc'd  */
    size_t body_len;  /* bytes in body, excluding the NUL terminator     */
    char  *headers;   /* raw response headers, malloc'd, NUL terminated  */
} PcHttpResp;

/* Idempotent, thread-safe libcurl global initialisation. */
void pc_http_global_init(void);

/* Create a client.  $PC_HOST overrides the default https://nl.pinterest.com. */
PcHttp *pc_http_new(void);
void    pc_http_free(PcHttp *h);

/* Base host, no trailing slash, e.g. "https://nl.pinterest.com". */
const char *pc_http_host(PcHttp *h);

/* GET / POST.  extra_headers is a NULL terminated array of "Name: value"
 * strings (or NULL).  Return false only if the transport failed; HTTP error
 * statuses are reported in resp->status.  Retries 429/5xx twice with backoff
 * (Retry-After honoured) and enforces a polite minimum interval between
 * requests.  resp is always safe to pass to pc_http_resp_free(). */
bool pc_http_get(PcHttp *h, const char *url,
                 const char *const *extra_headers, PcHttpResp *resp);
bool pc_http_post_form(PcHttp *h, const char *url, const char *body,
                       const char *const *extra_headers, PcHttpResp *resp);

/* Same as pc_http_post_form but never retries (not even 429/5xx): used for
 * non-idempotent writes such as saving a pin. */
bool pc_http_post_form_once(PcHttp *h, const char *url, const char *body,
                            const char *const *extra_headers,
                            PcHttpResp *resp);

/* JSON-body POST (POST /_/graphql/ and similar).  Content-Type:
 * application/json is added automatically; extra_headers may be NULL.
 * pc_http_post_json follows the normal read retry policy;
 * pc_http_post_json_once is single-shot for mutations. */
bool pc_http_post_json(PcHttp *h, const char *url, const char *json_body,
                       const char *const *extra_headers, PcHttpResp *resp);
bool pc_http_post_json_once(PcHttp *h, const char *url, const char *json_body,
                            const char *const *extra_headers,
                            PcHttpResp *resp);

void pc_http_resp_free(PcHttpResp *r);

/* Last transport error for this client (thread-local copy, never NULL). */
const char *pc_http_error(PcHttp *h);

/* Copy cookie `name` (e.g. "csrftoken") from the in-memory jar into out.
 * Returns true when found. */
bool pc_http_cookie(PcHttp *h, const char *name, char *out, size_t outsz);

/* Replace the cookie jar with a Netscape-format cookies.txt file (the format
 * browsers export).  Supports host-only and ".domain" cookies plus the
 * "#HttpOnly_" prefix; skip comments/blank lines.  Never logs values.
 * Returns false + err text (NULL terminated, no cookie values) on unreadable,
 * empty, NUL-containing or cookie-less files.  *nloaded receives the number of
 * cookies applied (may be NULL). */
bool pc_http_cookie_load(PcHttp *h, const char *path, int *nloaded,
                         char *err, size_t errsz);

/* Export the current jar as a Netscape cookies.txt with mode 0600 (the file is
 * created with 0600 and fchmod'ed so an existing wider mode is tightened). */
bool pc_http_cookie_save(PcHttp *h, const char *path,
                         char *err, size_t errsz);

/* ------------------------------------------------------- cross-module glue */

/* Implemented in api.c: record / clear the human readable client error. */
struct PcClient;
void pc_client_set_error(struct PcClient *c, const char *fmt, ...);
void pc_client_clear_error(struct PcClient *c);

/* img.c: drop the shared in-memory image cache. */
void pc_img_cache_flush(void);

/* api.c internal (tools/dump --video): resolve a canonical pin id's video
 * metadata (GraphQL close-up query).  Fills id/title/is_video/video_* fields. */
struct PcClient;
bool pc_pin_video_info(struct PcClient *c, const char *pin_id, PcPin *out,
                       char **err);

/* api.c internal (tools/dump --ads): one BaseSearchResource request, then the
 * same response is parsed with the ad filter off then on.  Returns false when
 * the counts are inconsistent (outputs are still filled for evidence). */
struct PcClient;
bool pc_search_ads_report(struct PcClient *c, const char *query, int *pins,
                          uint64_t *ads_detected, uint64_t *ads_blocked,
                          char **err);

#endif /* PINFALL_HTTP_H */
