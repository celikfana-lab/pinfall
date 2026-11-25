/* jsonx.c - PinFall JSON extraction: PcPin fields from Pinterest resource
 *           responses (raw pin objects and `type:"story"` wrappers).
 */
#define _POSIX_C_SOURCE 200809L

#include "jsonx.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ------------------------------------------------------------- utilities */

static void set_err(char **err, const char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    if (!err)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    *err = strdup(buf);
}

int pc_utf8_ellipsize(char *dst, size_t dstsz, const char *src)
{
    size_t n, max, keep, pos;

    if (!dst || dstsz == 0)
        return 0;
    dst[0] = '\0';
    if (!src)
        return 0;

    n   = strlen(src);
    max = dstsz - 1;
    if (n <= max) {
        memcpy(dst, src, n + 1);
        return (int)n;
    }

    /* Keep room for a U+2026 HORIZONTAL ELLIPSIS (3 UTF-8 bytes) when the
     * destination is large enough that it still carries information. */
    keep = max;
    if (dstsz >= 8)
        keep = max - 3;
    /* Never split a multi-byte UTF-8 sequence: back off continuation bytes. */
    while (keep > 0 && ((unsigned char)src[keep] & 0xC0) == 0x80)
        keep--;

    memcpy(dst, src, keep);
    pos = keep;
    if (dstsz >= 8) {
        memcpy(dst + pos, "\xE2\x80\xA6", 3);
        pos += 3;
    }
    dst[pos] = '\0';
    return (int)pos;
}

static const char *jstr(cJSON *obj, const char *key)
{
    cJSON *v;

    if (!cJSON_IsObject(obj))
        return NULL;
    v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring)
        return v->valuestring;
    return NULL;
}

/* ------------------------------------------------ ad-block filter (v5) */

static _Thread_local bool     tls_block_ads;
static _Thread_local uint64_t tls_ads_blocked;

void pc_json_ads_begin(bool block)
{
    tls_block_ads    = block;
    tls_ads_blocked  = 0;
}

uint64_t pc_json_ads_end(void)
{
    uint64_t n = tls_ads_blocked;

    tls_block_ads   = false;
    tls_ads_blocked = 0;
    return n;
}

/* JSON-ish truthiness: absent/null/false/0/""/"false" are false. */
static bool json_truthy(cJSON *v)
{
    if (!v || cJSON_IsNull(v))
        return false;
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v))
        return v->valueint != 0;
    if (cJSON_IsString(v)) {
        const char *s = v->valuestring ? v->valuestring : "";

        return *s && strcmp(s, "0") != 0 && strcmp(s, "false") != 0 &&
               strcmp(s, "False") != 0;
    }
    return true; /* present object/array */
}

/* Promoted-pin markers only.  aggregated_pin_data appears on organic pins and
 * is deliberately NOT consulted. */
static bool pin_is_promoted(cJSON *obj)
{
    if (!cJSON_IsObject(obj))
        return false;
    if (json_truthy(cJSON_GetObjectItemCaseSensitive(obj, "is_promoted")))
        return true;
    if (json_truthy(cJSON_GetObjectItemCaseSensitive(obj,
                                                     "promoted_is_lead_ad")))
        return true;
    if (json_truthy(cJSON_GetObjectItemCaseSensitive(obj,
                                                     "is_downstream_promotion")))
        return true;
    if (json_truthy(cJSON_GetObjectItemCaseSensitive(obj, "ad_match_reason")))
        return true;
    return false;
}

static void copy_field(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0)
        return;
    pc_utf8_ellipsize(dst, dstsz, src ? src : "");
}

/* URLs are truncated hard (never with an ellipsis: a mangled URL must not grow
 * a bogus U+2026 in the middle), still without splitting a UTF-8 sequence. */
static void copy_url(char *dst, size_t dstsz, const char *src)
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

static char ascii_lower_hex(char c)
{
    return (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
}

static bool valid_color(const char *s)
{
    size_t i;

    if (!s || s[0] != '#' || strlen(s) != 7)
        return false;
    for (i = 1; i <= 6; i++) {
        if (!isxdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------ image buckets */

/* Pinterest image buckets are normally {"url","width","height"} dicts, but
 * board objects use a LIST of such dicts under images["236x"].  Accept both and
 * take the first usable entry. */
static bool img_bucket(cJSON *images, const char *key, char *url, size_t urlsz,
                       int *w, int *h)
{
    cJSON      *bucket;
    cJSON      *node = NULL;
    const char *u;

    if (w)
        *w = 0;
    if (h)
        *h = 0;
    if (!cJSON_IsObject(images))
        return false;

    bucket = cJSON_GetObjectItemCaseSensitive(images, key);
    if (!bucket)
        return false;

    if (cJSON_IsObject(bucket)) {
        node = bucket;
    } else if (cJSON_IsArray(bucket)) {
        cJSON *it;
        cJSON_ArrayForEach(it, bucket) {
            if (cJSON_IsObject(it) && jstr(it, "url")) {
                node = it;
                break;
            }
        }
    }
    if (!node)
        return false;

    u = jstr(node, "url");
    if (!u || !*u)
        return false;
    copy_url(url, urlsz, u);

    if (w) {
        cJSON *vw = cJSON_GetObjectItemCaseSensitive(node, "width");
        *w = cJSON_IsNumber(vw) ? vw->valueint : 0;
    }
    if (h) {
        cJSON *vh = cJSON_GetObjectItemCaseSensitive(node, "height");
        *h = cJSON_IsNumber(vh) ? vh->valueint : 0;
    }
    return true;
}

/* -------------------------------------------------------------- pin object */

/* Board / user objects also carry id+images, so require either type=="pin" or a
 * pin-only field before treating an object as a pin. */
static bool is_pin_object(cJSON *obj)
{
    const char *type;
    cJSON      *images;

    if (!cJSON_IsObject(obj))
        return false;
    type = jstr(obj, "type");
    if (type && strcmp(type, "pin") != 0)
        return false;
    if (!type && (cJSON_GetObjectItemCaseSensitive(obj, "objects") ||
                  cJSON_GetObjectItemCaseSensitive(obj, "content_ids")))
        return false; /* story card without an explicit type */
    if (!jstr(obj, "id"))
        return false;
    images = cJSON_GetObjectItemCaseSensitive(obj, "images");
    if (!cJSON_IsObject(images))
        return false;
    if (cJSON_GetObjectItemCaseSensitive(obj, "dominant_color") ||
        cJSON_GetObjectItemCaseSensitive(obj, "orig") ||
        cJSON_GetObjectItemCaseSensitive(obj, "pinner"))
        return true;
    /* Fallback: a pin's 236x bucket is a dict, a board's is a list. */
    return cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(images, "236x"));
}

static int json_field_int(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static bool json_field_bool(cJSON *obj, const char *key, bool *out)
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

/* ---------------------------------------------------------- videos (v8) -- */

static bool url_ends_ci(const char *url, const char *suffix)
{
    size_t ul, sl, i;

    if (!url || !suffix)
        return false;
    ul = strlen(url);
    sl = strlen(suffix);
    if (ul < sl)
        return false;
    for (i = 0; i < sl; i++) {
        char a = url[ul - sl + i];
        char b = suffix[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

static bool key_contains_ci(const char *key, const char *needle)
{
    size_t kl, nl, i, j;

    if (!key || !needle)
        return false;
    kl = strlen(key);
    nl = strlen(needle);
    for (i = 0; i + nl <= kl; i++) {
        for (j = 0; j < nl; j++) {
            char a = key[i + j];
            char b = needle[j];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (a != b)
                break;
        }
        if (j == nl)
            return true;
    }
    return false;
}

/* Fill PcPin from a container object that holds a video_list (or videoUrls).
 * Returns true when the object was recognised as a video container. */
static bool parse_video_container(cJSON *videos, PcPin *p)
{
    cJSON      *list;
    cJSON      *entry;
    cJSON      *best = NULL;
    const char *url = NULL;
    int         pass;

    if (!cJSON_IsObject(videos))
        return false;
    list = cJSON_GetObjectItemCaseSensitive(videos, "video_list");
    if (!cJSON_IsObject(list))
        list = cJSON_GetObjectItemCaseSensitive(videos, "videoList");

    if (!cJSON_IsObject(list)) {
        cJSON *urls = cJSON_GetObjectItemCaseSensitive(videos, "videoUrls");

        if (!cJSON_IsArray(urls))
            return false;
    }
    p->is_video = true;

    if (cJSON_IsObject(list)) {
        for (pass = 0; pass < 4 && !best; pass++) {
            cJSON_ArrayForEach(entry, list) {
                const char *u = jstr(entry, "url");

                if (!cJSON_IsObject(entry) || !u || !*u)
                    continue;
                if (pass == 0) {
                    if (url_ends_ci(u, ".mp4") ||
                        key_contains_ci(entry->string, "720p") ||
                        key_contains_ci(entry->string, "480p") ||
                        key_contains_ci(entry->string, "exp"))
                        best = entry;
                } else if (pass == 1) {
                    if (key_contains_ci(entry->string, "hlsv4"))
                        best = entry;
                } else if (pass == 2) {
                    if (key_contains_ci(entry->string, "hlsv3"))
                        best = entry;
                } else {
                    best = entry;
                }
                if (best)
                    break;
            }
        }
    }
    if (best)
        url = jstr(best, "url");
    if (!url) {
        cJSON *urls = cJSON_GetObjectItemCaseSensitive(videos, "videoUrls");

        if (cJSON_IsArray(urls)) {
            cJSON *u;

            cJSON_ArrayForEach(u, urls) {
                if (!cJSON_IsString(u) || !u->valuestring)
                    continue;
                if (!url)
                    url = u->valuestring;
                if (url_ends_ci(u->valuestring, ".mp4")) {
                    url = u->valuestring;
                    break;
                }
            }
        }
    }
    if (url && *url)
        copy_url(p->video_url, sizeof p->video_url, url);
    if (cJSON_IsObject(best)) {
        cJSON *v;

        v = cJSON_GetObjectItemCaseSensitive(best, "width");
        if (cJSON_IsNumber(v))
            p->video_w = v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(best, "height");
        if (cJSON_IsNumber(v))
            p->video_h = v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(best, "duration");
        if (cJSON_IsNumber(v))
            p->video_duration_ms = (int)(v->valuedouble + 0.5);
    }
    return true;
}

/* Story pins (multi-page / idea pins) keep their video under
 * story_pin_data.pages[].video or pages[].blocks[].video — the close-up REST
 * entity exposes it even when the pin's own `videos` field is null. */
static bool parse_story_videos(cJSON *story, PcPin *p)
{
    cJSON *pages = cJSON_GetObjectItemCaseSensitive(story, "pages");
    cJSON *page;

    if (cJSON_IsArray(pages)) {
        cJSON_ArrayForEach(page, pages) {
            cJSON *v;

            if (!cJSON_IsObject(page))
                continue;
            v = cJSON_GetObjectItemCaseSensitive(page, "video");
            if (parse_video_container(v, p))
                return true;
            {
                cJSON *blocks = cJSON_GetObjectItemCaseSensitive(page, "blocks");

                if (cJSON_IsArray(blocks)) {
                    cJSON *b;

                    cJSON_ArrayForEach(b, blocks) {
                        cJSON *bv;

                        if (!cJSON_IsObject(b))
                            continue;
                        bv = cJSON_GetObjectItemCaseSensitive(b, "video");
                        if (parse_video_container(bv, p))
                            return true;
                    }
                }
            }
        }
    }
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(story, "video");

        if (parse_video_container(v, p))
            return true;
    }
    return false;
}

/* Choose the best video entry for a pin: `videos`, a bare video_list on the
 * pin, or a story_pin_data page/block video (verified with pin 906419862523865948,
 * a video pin whose `videos` is null). */
static void parse_videos(cJSON *obj, PcPin *p)
{
    cJSON *story;

    if (parse_video_container(
            cJSON_GetObjectItemCaseSensitive(obj, "videos"), p))
        return;
    if (parse_video_container(obj, p))
        return;
    story = cJSON_GetObjectItemCaseSensitive(obj, "story_pin_data");
    if (!cJSON_IsObject(story))
        story = cJSON_GetObjectItemCaseSensitive(obj, "storyPinData");
    if (cJSON_IsObject(story) && parse_story_videos(story, p))
        return;
    story = cJSON_GetObjectItemCaseSensitive(obj, "story");
    if (cJSON_IsObject(story)) {
        cJSON *pd = cJSON_GetObjectItemCaseSensitive(story, "pin_data");

        if (!cJSON_IsObject(pd))
            pd = cJSON_GetObjectItemCaseSensitive(story, "pinData");
        if (cJSON_IsObject(pd))
            parse_story_videos(pd, p);
        else
            parse_story_videos(story, p);
    }
}

static bool fill_pin(cJSON *obj, PcPin *p)
{
    cJSON      *images;
    cJSON      *sub;
    const char *title;
    const char *domain;
    const char *color;
    char        norm_color[PC_COLOR_LEN];

    if (!is_pin_object(obj))
        return false;
    memset(p, 0, sizeof *p);

    copy_field(p->id, sizeof p->id, jstr(obj, "id"));

    title = jstr(obj, "title");
    if (!title || !*title)
        title = jstr(obj, "grid_title");
    if (!title || !*title)
        title = jstr(obj, "alt_text");
    copy_field(p->title, sizeof p->title, title);
    copy_field(p->description, sizeof p->description, jstr(obj, "description"));
    copy_url(p->link, sizeof p->link, jstr(obj, "link"));

    domain = jstr(obj, "domain");
    if (domain && strcmp(domain, "Uploaded by user") == 0)
        domain = NULL;
    copy_field(p->domain, sizeof p->domain, domain);

    color = jstr(obj, "dominant_color");
    if (!valid_color(color)) {
        color = NULL;
    } else {
        size_t ci;

        for (ci = 0; ci < 7; ci++)
            norm_color[ci] = ascii_lower_hex(color[ci]);
        norm_color[7] = '\0';
        color         = norm_color;
    }
    copy_field(p->dominant_color, sizeof p->dominant_color, color);

    sub = cJSON_GetObjectItemCaseSensitive(obj, "board");
    if (cJSON_IsObject(sub))
        copy_field(p->board_name, sizeof p->board_name, jstr(sub, "name"));

    sub = cJSON_GetObjectItemCaseSensitive(obj, "pinner");
    if (cJSON_IsObject(sub)) {
        const char *name = jstr(sub, "username");
        if (!name || !*name)
            name = jstr(sub, "full_name");
        copy_field(p->pinner_name, sizeof p->pinner_name, name);
    }

    /* v6 social counts: resource payloads use reaction_counts["1"], close-up
     * entities use like_count/comment_count/liked_by_me. */
    p->like_count = json_field_int(obj, "like_count");
    if (p->like_count == 0)
        p->like_count = json_field_int(obj, "likeCount");
    if (p->like_count == 0) {
        cJSON *rc = cJSON_GetObjectItemCaseSensitive(obj, "reaction_counts");

        if (cJSON_IsObject(rc)) {
            cJSON *one = cJSON_GetObjectItemCaseSensitive(rc, "1");
            if (cJSON_IsNumber(one))
                p->like_count = one->valueint;
        }
    }
    p->comment_count = json_field_int(obj, "comment_count");
    if (p->comment_count == 0)
        p->comment_count = json_field_int(obj, "commentCount");
    {
        bool liked = false;

        if (json_field_bool(obj, "liked_by_me", &liked) ||
            json_field_bool(obj, "likedByMe", &liked) ||
            json_field_bool(obj, "liked", &liked) ||
            json_field_bool(obj, "viewer_has_liked", &liked)) {
            p->liked = liked;
        } else {
            cJSON *vr = cJSON_GetObjectItemCaseSensitive(obj,
                                                         "viewer_reaction");
            if (cJSON_IsString(vr) && vr->valuestring &&
                strcmp(vr->valuestring, "1") == 0)
                p->liked = true;
        }
    }

    images = cJSON_GetObjectItemCaseSensitive(obj, "images");
    if (!img_bucket(images, "236x", p->thumb_url, sizeof p->thumb_url,
                    &p->img_w, &p->img_h))
        return false; /* no grid thumbnail: not usable */
    img_bucket(images, "474x", p->medium_url, sizeof p->medium_url, NULL, NULL);
    img_bucket(images, "736x", p->big_url, sizeof p->big_url, NULL, NULL);
    img_bucket(images, "orig", p->orig_url, sizeof p->orig_url, NULL, NULL);

    if (!p->medium_url[0])
        copy_url(p->medium_url, sizeof p->medium_url, p->thumb_url);
    if (!p->big_url[0])
        copy_url(p->big_url, sizeof p->big_url, p->medium_url);
    if (!p->orig_url[0])
        copy_url(p->orig_url, sizeof p->orig_url, p->big_url);
    parse_videos(obj, p);
    return true;
}

/* ------------------------------------------------------------- pin list ops */

static bool list_push(PcPinList *l, const PcPin *p)
{
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 32;
        PcPin *ni = realloc(l->items, nc * sizeof *ni);
        if (!ni)
            return false;
        l->items = ni;
        l->cap   = nc;
    }
    l->items[l->count++] = *p;
    return true;
}

static bool copy_bookmark_str(char *dst, size_t dstsz, cJSON *rr, char **err)
{
    const char *bm = jstr(rr, "bookmark");
    size_t      n;

    if (!dst || dstsz == 0)
        return false;
    dst[0] = '\0';
    if (!bm || !*bm)
        return true;
    if (strcmp(bm, "-end-") == 0)
        return true; /* explicit end-of-feed marker */
    n = strlen(bm);
    if (n >= dstsz) {
        /* Never silently truncate a pagination cursor. */
        set_err(err,
                "bookmark too long (%zu bytes, buffer is %zu): refusing to "
                "truncate pagination cursor", n, dstsz);
        return false;
    }
    memcpy(dst, bm, n + 1);
    return true;
}

static bool copy_bookmark(PcPinList *out, cJSON *rr, char **err)
{
    return copy_bookmark_str(out->bookmark, sizeof out->bookmark, rr, err);
}

static cJSON *parse_root(const char *json, size_t len, char **err)
{
    cJSON *root = cJSON_ParseWithLength(json, len);

    if (!root)
        set_err(err, "JSON parse failed");
    return root;
}

static cJSON *rpc_response(cJSON *root, char **err)
{
    cJSON *rr = cJSON_GetObjectItemCaseSensitive(root, "resource_response");
    cJSON *e;

    if (!cJSON_IsObject(rr)) {
        set_err(err, "response is missing resource_response");
        return NULL;
    }
    e = cJSON_GetObjectItemCaseSensitive(rr, "error");
    if (cJSON_IsObject(e)) {
        const char *msg = jstr(e, "message");
        set_err(err, "API error: %s", msg ? msg : "unknown");
        return NULL;
    }
    return rr;
}

/* Collect pins from an array of mixed items (raw pins, story wrappers).
 * Returns false only on OOM. */
static bool collect_pins_array(cJSON *results, PcPinList *out, char **err)
{
    cJSON *item;

    cJSON_ArrayForEach(item, results) {
        const char *type;

        if (!cJSON_IsObject(item))
            continue;
        type = jstr(item, "type");
        if (type && strcmp(type, "story") == 0) {
            /* Story wrapper: recurse one level into typed objects. */
            cJSON *objects = cJSON_GetObjectItemCaseSensitive(item, "objects");
            cJSON *obj;

            if (!cJSON_IsArray(objects))
                continue;
            cJSON_ArrayForEach(obj, objects) {
                PcPin pin;
                if (!fill_pin(obj, &pin))
                    continue; /* board / user / unknown object */
                if (tls_block_ads && pin_is_promoted(obj)) {
                    tls_ads_blocked++;
                    continue;
                }
                if (!list_push(out, &pin)) {
                    set_err(err, "out of memory");
                    return false;
                }
            }
        } else {
            PcPin pin;
            if (!fill_pin(item, &pin))
                continue;
            if (tls_block_ads && pin_is_promoted(item)) {
                tls_ads_blocked++;
                continue;
            }
            if (!list_push(out, &pin)) {
                set_err(err, "out of memory");
                return false;
            }
        }
    }
    return true;
}

/* -------------------------------------------------------------- public API */

bool pc_json_parse_search(const char *json, size_t len, PcPinList *out,
                          char **err)
{
    cJSON *root;
    cJSON *rr;
    cJSON *data;
    cJSON *results;
    bool   ok = false;

    if (err)
        *err = NULL;
    if (!json || !out) {
        set_err(err, "invalid arguments");
        return false;
    }
    memset(out, 0, sizeof *out);

    root = parse_root(json, len, err);
    if (!root)
        return false;

    rr = rpc_response(root, err);
    if (!rr)
        goto done;
    data = cJSON_GetObjectItemCaseSensitive(rr, "data");
    results = cJSON_IsObject(data)
                  ? cJSON_GetObjectItemCaseSensitive(data, "results")
                  : NULL;
    if (!cJSON_IsArray(results)) {
        set_err(err, "response is missing resource_response.data.results");
        goto done;
    }

    if (!collect_pins_array(results, out, err))
        goto done;

    ok = copy_bookmark(out, rr, err);
done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    cJSON_Delete(root);
    return ok;
}

static bool find_pin_rec(cJSON *node, const char *want_id, PcPin *out,
                         int depth)
{
    cJSON *child;

    if (!node || depth > 8)
        return false;
    if (cJSON_IsObject(node)) {
        PcPin p;
        if (fill_pin(node, &p) && (!want_id || !*want_id ||
                                   strcmp(p.id, want_id) == 0)) {
            *out = p;
            return true;
        }
        cJSON_ArrayForEach(child, node) {
            if (find_pin_rec(child, want_id, out, depth + 1))
                return true;
        }
    } else if (cJSON_IsArray(node)) {
        cJSON_ArrayForEach(child, node) {
            if (find_pin_rec(child, want_id, out, depth + 1))
                return true;
        }
    }
    return false;
}

bool pc_json_parse_pin(const char *json, size_t len, const char *want_id,
                       PcPin *out, char **err)
{
    cJSON *root;
    cJSON *rr;
    cJSON *data;
    bool   ok = false;

    if (err)
        *err = NULL;
    if (!json || !out) {
        set_err(err, "invalid arguments");
        return false;
    }
    memset(out, 0, sizeof *out);

    root = parse_root(json, len, err);
    if (!root)
        return false;

    rr = rpc_response(root, err);
    if (!rr) {
        cJSON_Delete(root);
        return false;
    }
    data = cJSON_GetObjectItemCaseSensitive(rr, "data");
    if (cJSON_IsObject(data) && fill_pin(data, out))
        ok = true;
    if (!ok)
        ok = find_pin_rec(data, want_id, out, 0);
    if (!ok)
        ok = find_pin_rec(root, want_id, out, 0);
    if (!ok)
        set_err(err, "no pin object in response");

    cJSON_Delete(root);
    return ok;
}

/* --------------------------------------------------- saved pins / boards (v3) */

/* UserPinsResource returns data as a bare LIST of pins (no .results), so this
 * mirrors pc_json_parse_search but accepts both shapes defensively. */
bool pc_json_parse_pins(const char *json, size_t len, PcPinList *out,
                        char **err)
{
    cJSON *root;
    cJSON *rr;
    cJSON *data;
    cJSON *results;
    bool   ok = false;

    if (err)
        *err = NULL;
    if (!json || !out) {
        set_err(err, "invalid arguments");
        return false;
    }
    memset(out, 0, sizeof *out);

    root = parse_root(json, len, err);
    if (!root)
        return false;
    rr = rpc_response(root, err);
    if (!rr)
        goto done;
    data = cJSON_GetObjectItemCaseSensitive(rr, "data");
    if (cJSON_IsArray(data)) {
        results = data;
    } else if (cJSON_IsObject(data)) {
        results = cJSON_GetObjectItemCaseSensitive(data, "results");
    } else {
        results = NULL;
    }
    if (!cJSON_IsArray(results)) {
        set_err(err,
                "response is missing a pin list (data[] or data.results[])");
        goto done;
    }
    if (!collect_pins_array(results, out, err))
        goto done;
    ok = copy_bookmark(out, rr, err);

done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    cJSON_Delete(root);
    return ok;
}

static int json_int(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsNumber(v) ? v->valueint : 0;
}

static bool fill_board(cJSON *obj, PcBoard *b)
{
    const char *type;
    const char *id;

    if (!cJSON_IsObject(obj))
        return false;
    type = jstr(obj, "type");
    if (type && strcmp(type, "board") != 0)
        return false;
    id = jstr(obj, "id");
    if (!id || !*id)
        return false;
    memset(b, 0, sizeof *b);
    copy_field(b->id, sizeof b->id, id);
    copy_field(b->name, sizeof b->name, jstr(obj, "name"));
    copy_url(b->url, sizeof b->url, jstr(obj, "url"));
    copy_field(b->privacy, sizeof b->privacy, jstr(obj, "privacy"));
    b->pin_count = json_int(obj, "pin_count");
    return true;
}

static bool board_push(PcBoardList *l, const PcBoard *b)
{
    if (l->count == l->cap) {
        size_t   nc = l->cap ? l->cap * 2 : 16;
        PcBoard *ni = realloc(l->items, nc * sizeof *ni);

        if (!ni)
            return false;
        l->items = ni;
        l->cap   = nc;
    }
    l->items[l->count++] = *b;
    return true;
}

bool pc_json_parse_boards(const char *json, size_t len, PcBoardList *out,
                          char **err)
{
    cJSON *root;
    cJSON *rr;
    cJSON *data;
    cJSON *results;
    cJSON *item;
    bool   ok = false;

    if (err)
        *err = NULL;
    if (!json || !out) {
        set_err(err, "invalid arguments");
        return false;
    }
    memset(out, 0, sizeof *out);

    root = parse_root(json, len, err);
    if (!root)
        return false;
    rr = rpc_response(root, err);
    if (!rr)
        goto done;
    data = cJSON_GetObjectItemCaseSensitive(rr, "data");
    if (cJSON_IsArray(data)) {
        results = data;
    } else if (cJSON_IsObject(data)) {
        results = cJSON_GetObjectItemCaseSensitive(data, "results");
    } else {
        results = NULL;
    }
    if (!cJSON_IsArray(results)) {
        set_err(err,
                "response is missing a board list (data[] or data.results[])");
        goto done;
    }
    cJSON_ArrayForEach(item, results) {
        PcBoard b;

        if (!fill_board(item, &b))
            continue;
        if (!board_push(out, &b)) {
            set_err(err, "out of memory");
            goto done;
        }
    }
    if (!copy_bookmark_str(out->bookmark, sizeof out->bookmark, rr, err))
        goto done;
    ok = true;

done:
    if (!ok) {
        free(out->items);
        memset(out, 0, sizeof *out);
    }
    cJSON_Delete(root);
    return ok;
}

/* Count promoted pins in a payload, no filtering (dump --ads self-check). */
static void count_ads_rec(cJSON *node, int depth, uint64_t *count)
{
    if (!node || depth > 3)
        return;
    if (cJSON_IsObject(node)) {
        if (is_pin_object(node) && pin_is_promoted(node))
            (*count)++;
        return;
    }
    if (cJSON_IsArray(node)) {
        cJSON *it;

        cJSON_ArrayForEach(it, node) {
            if (!cJSON_IsObject(it))
                continue;
            {
                const char *type = jstr(it, "type");

                if (type && strcmp(type, "story") == 0) {
                    cJSON *objects = cJSON_GetObjectItemCaseSensitive(it,
                                                                     "objects");

                    count_ads_rec(objects, depth + 1, count);
                } else if (is_pin_object(it) && pin_is_promoted(it)) {
                    (*count)++;
                }
            }
        }
    }
}

uint64_t pc_json_count_ads(const char *json, size_t len, char **err)
{
    cJSON   *root;
    cJSON   *rr;
    cJSON   *data;
    cJSON   *results;
    uint64_t count = 0;

    if (err)
        *err = NULL;
    if (!json) {
        set_err(err, "invalid arguments");
        return 0;
    }
    root = parse_root(json, len, err);
    if (!root)
        return 0;
    rr = rpc_response(root, err);
    if (!rr) {
        cJSON_Delete(root);
        return 0;
    }
    data = cJSON_GetObjectItemCaseSensitive(rr, "data");
    if (cJSON_IsArray(data)) {
        results = data;
    } else if (cJSON_IsObject(data)) {
        results = cJSON_GetObjectItemCaseSensitive(data, "results");
    } else {
        results = NULL;
    }
    if (cJSON_IsArray(results))
        count_ads_rec(results, 0, &count);
    else
        set_err(err, "response is missing a pin list");
    cJSON_Delete(root);
    return count;
}

/* ------------------------------------- GraphQL / arbitrary JSON (v6) ------- */

static bool pinlist_push_unique(PcPinList *out, const PcPin *pin)
{
    size_t i;

    for (i = 0; i < out->count; i++) {
        if (strcmp(out->items[i].id, pin->id) == 0)
            return true; /* duplicate (GraphQL often echoes the entity) */
    }
    return list_push(out, pin);
}

static void collect_pins_walk(cJSON *node, PcPinList *out, int depth,
                              bool *oom)
{
    if (!node || depth > 12 || *oom)
        return;
    if (cJSON_IsObject(node)) {
        PcPin pin;

        if (fill_pin(node, &pin)) {
            if (tls_block_ads && pin_is_promoted(node)) {
                tls_ads_blocked++;
            } else if (!pinlist_push_unique(out, &pin)) {
                *oom = true;
            }
            return; /* never descend into a pin object */
        }
        {
            cJSON *child;

            cJSON_ArrayForEach(child, node)
                collect_pins_walk(child, out, depth + 1, oom);
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *child;

        cJSON_ArrayForEach(child, node)
            collect_pins_walk(child, out, depth + 1, oom);
    }
}

static bool looks_like_bookmark_key(const char *k)
{
    return strcmp(k, "bookmark") == 0 || strcmp(k, "next_bookmark") == 0 ||
           strcmp(k, "cursor") == 0 || strcmp(k, "end_cursor") == 0 ||
           strcmp(k, "nextCursor") == 0 || strcmp(k, "next_bookmark") == 0;
}

static void find_bookmark_walk(cJSON *node, char *dst, size_t dstsz, int depth,
                               bool *found)
{
    if (!node || depth > 12 || *found)
        return;
    if (cJSON_IsObject(node)) {
        cJSON *child;

        cJSON_ArrayForEach(child, node) {
            if (child->string && cJSON_IsString(child) && child->valuestring &&
                child->valuestring[0] && looks_like_bookmark_key(child->string)) {
                pc_utf8_ellipsize(dst, dstsz, child->valuestring);
                *found = true;
                return;
            }
            find_bookmark_walk(child, dst, dstsz, depth + 1, found);
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *child;

        cJSON_ArrayForEach(child, node)
            find_bookmark_walk(child, dst, dstsz, depth + 1, found);
    }
}

bool pc_json_collect_pins_any(const char *json, size_t len, PcPinList *out,
                              char **err)
{
    cJSON *root;
    bool   oom = false;
    bool   found_bm = false;

    if (err)
        *err = NULL;
    if (!json || !out) {
        set_err(err, "invalid arguments");
        return false;
    }
    memset(out, 0, sizeof *out);
    root = parse_root(json, len, err);
    if (!root)
        return false;
    collect_pins_walk(root, out, 0, &oom);
    if (oom) {
        set_err(err, "out of memory");
        free(out->items);
        memset(out, 0, sizeof *out);
        cJSON_Delete(root);
        return false;
    }
    find_bookmark_walk(root, out->bookmark, sizeof out->bookmark, 0,
                       &found_bm);
    cJSON_Delete(root);
    return true;
}

static bool comment_id_str(cJSON *id, char *dst, size_t dstsz)
{
    if (cJSON_IsString(id) && id->valuestring) {
        pc_utf8_ellipsize(dst, dstsz, id->valuestring);
        return dst[0] != '\0';
    }
    if (cJSON_IsNumber(id)) {
        snprintf(dst, dstsz, "%.0f", id->valuedouble);
        return true;
    }
    return false;
}

static bool fill_comment(cJSON *obj, PcComment *cm)
{
    cJSON      *text;
    cJSON      *user;
    cJSON      *id;
    const char *name = NULL;

    if (!cJSON_IsObject(obj))
        return false;
    /* Some close-up payloads use type userdiditdata with `details` for the
     * comment text; older ones use `text`.  Accept either. */
    text = cJSON_GetObjectItemCaseSensitive(obj, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
        cJSON *details = cJSON_GetObjectItemCaseSensitive(obj, "details");

        if (cJSON_IsString(details) && details->valuestring &&
            details->valuestring[0])
            text = details;
        else
            return false;
    }
    id = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (!id)
        return false;
    user = cJSON_GetObjectItemCaseSensitive(obj, "user");
    if (!cJSON_IsObject(user))
        user = cJSON_GetObjectItemCaseSensitive(obj, "pinner");
    if (cJSON_IsObject(user)) {
        name = jstr(user, "username");
        if (!name || !*name)
            name = jstr(user, "full_name");
        if (!name || !*name)
            name = jstr(user, "fullName");
    }
    if (!name || !*name)
        name = jstr(obj, "user_name");
    if (!name || !*name)
        name = jstr(obj, "username");

    memset(cm, 0, sizeof *cm);
    comment_id_str(id, cm->id, sizeof cm->id);
    copy_field(cm->user_name, sizeof cm->user_name, name);
    copy_field(cm->text, sizeof cm->text, text->valuestring);
    copy_field(cm->created_at, sizeof cm->created_at, jstr(obj, "created_at"));
    if (!cm->created_at[0])
        copy_field(cm->created_at, sizeof cm->created_at,
                   jstr(obj, "createdAt"));
    if (!cm->created_at[0])
        copy_field(cm->created_at, sizeof cm->created_at, jstr(obj, "done_at"));
    cm->like_count = json_field_int(obj, "like_count");
    if (cm->like_count == 0) {
        cJSON *rc = cJSON_GetObjectItemCaseSensitive(obj, "reaction_counts");

        if (cJSON_IsObject(rc)) {
            cJSON *one = cJSON_GetObjectItemCaseSensitive(rc, "1");
            if (cJSON_IsNumber(one))
                cm->like_count = one->valueint;
        }
    }
    return cm->text[0] != '\0';
}

static void collect_comments_walk(cJSON *node, PcCommentList *out, int depth,
                                  bool *oom)
{
    if (!node || depth > 12 || *oom)
        return;
    if (cJSON_IsObject(node)) {
        PcComment cm;
        cJSON    *child;

        if (fill_comment(node, &cm)) {
            if (out->count == out->cap) {
                size_t    nc = out->cap ? out->cap * 2 : 16;
                PcComment *ni = realloc(out->items, nc * sizeof *ni);

                if (!ni) {
                    *oom = true;
                    return;
                }
                out->items = ni;
                out->cap   = nc;
            }
            out->items[out->count++] = cm;
            return;
        }
        cJSON_ArrayForEach(child, node)
            collect_comments_walk(child, out, depth + 1, oom);
    } else if (cJSON_IsArray(node)) {
        cJSON *child;

        cJSON_ArrayForEach(child, node)
            collect_comments_walk(child, out, depth + 1, oom);
    }
}

bool pc_json_collect_comments_any(const char *json, size_t len,
                                  PcCommentList *out, char **err)
{
    cJSON *root;
    bool   oom = false;
    bool   found_bm = false;

    if (err)
        *err = NULL;
    if (!json || !out) {
        set_err(err, "invalid arguments");
        return false;
    }
    memset(out, 0, sizeof *out);
    root = parse_root(json, len, err);
    if (!root)
        return false;
    collect_comments_walk(root, out, 0, &oom);
    if (oom) {
        set_err(err, "out of memory");
        free(out->items);
        memset(out, 0, sizeof *out);
        cJSON_Delete(root);
        return false;
    }
    find_bookmark_walk(root, out->bookmark, sizeof out->bookmark, 0,
                       &found_bm);
    cJSON_Delete(root);
    return true;
}

/* Find the first object under `node` that has a videos object. */
static cJSON *find_videos_holder(cJSON *node, int depth)
{
    if (!node || depth > 10)
        return NULL;
    if (cJSON_IsObject(node)) {
        cJSON *child;
        cJSON *videos = cJSON_GetObjectItemCaseSensitive(node, "videos");

        if (cJSON_IsObject(videos))
            return node;
        if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(node,
                                                           "story_pin_data")))
            return node;
        if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(node,
                                                           "storyPinData")))
            return node;
        cJSON_ArrayForEach(child, node) {
            cJSON *found = find_videos_holder(child, depth + 1);

            if (found)
                return found;
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *child;

        cJSON_ArrayForEach(child, node) {
            cJSON *found = find_videos_holder(child, depth + 1);

            if (found)
                return found;
        }
    }
    return NULL;
}

/* Parse a GraphQL close-up data object (or any JSON document holding a pin with
 * videos) into a PcPin carrying at least id/title/video_* fields. */
bool pc_json_parse_video_pin(const char *json, size_t len, PcPin *out,
                             char **err)
{
    cJSON *root;
    cJSON *holder;
    cJSON *v;

    if (err)
        *err = NULL;
    if (!json || !out) {
        set_err(err, "invalid arguments");
        return false;
    }
    memset(out, 0, sizeof *out);
    root = parse_root(json, len, err);
    if (!root)
        return false;
    holder = find_videos_holder(root, 0);
    if (!holder) {
        cJSON_Delete(root);
        set_err(err, "no video metadata in payload");
        return false;
    }
    v = cJSON_GetObjectItemCaseSensitive(holder, "entityId");
    if (cJSON_IsString(v) && v->valuestring)
        copy_field(out->id, sizeof out->id, v->valuestring);
    else if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(holder, "id")))
        copy_field(out->id, sizeof out->id,
                   jstr(holder, "id"));
    copy_field(out->title, sizeof out->title, jstr(holder, "title"));
    if (!out->title[0])
        copy_field(out->title, sizeof out->title, jstr(holder, "grid_title"));
    parse_videos(holder, out);
    cJSON_Delete(root);
    return true;
}

/* -------------------------------------------------- signed-in username (v2) */

void pc_json_extract_username(const char *html, size_t len,
                              char *out, size_t outsz)
{
    static const char tag[] = "<script id=\"__PWS_INITIAL_PROPS__\"";
    static const char close_tag[] = "</script>";
    const char *p;
    const char *end;
    cJSON      *root;

    if (out && outsz)
        out[0] = '\0';
    if (!html || !out || outsz == 0)
        return;

    p = strstr(html, tag);
    if (!p)
        return;
    p = strchr(p, '>');
    if (!p)
        return;
    p++;
    end = strstr(p, close_tag);
    if (!end)
        return;
    root = cJSON_ParseWithLength(p, (size_t)(end - p));
    if (!root)
        return;

    {
        cJSON *state = cJSON_GetObjectItemCaseSensitive(root,
                                                        "initialReduxState");
        if (cJSON_IsObject(state)) {
            cJSON *admin   = cJSON_GetObjectItemCaseSensitive(state, "adminUser");
            cJSON *users   = cJSON_GetObjectItemCaseSensitive(state, "users");
            cJSON *session = cJSON_GetObjectItemCaseSensitive(state, "session");

            /* Preferred: session.userId (string id) -> users[userId].username.
             * Verified live: on an authenticated page adminUser is null, so
             * this is the only reliable identity source. */
            if (!out[0] && cJSON_IsObject(session) && cJSON_IsObject(users)) {
                cJSON *uid = cJSON_GetObjectItemCaseSensitive(session, "userId");
                cJSON *me  = NULL;

                if (cJSON_IsString(uid) && uid->valuestring && *uid->valuestring)
                    me = cJSON_GetObjectItemCaseSensitive(users, uid->valuestring);
                if (cJSON_IsObject(me)) {
                    const char *u = jstr(me, "username");

                    if (u && *u)
                        copy_field(out, outsz, u);
                }
            }
            /* Legacy layout: adminUser object, or an id string into users[]. */
            if (!out[0]) {
                if (cJSON_IsObject(admin)) {
                    const char *u = jstr(admin, "username");

                    if (u && *u)
                        copy_field(out, outsz, u);
                } else if (cJSON_IsString(admin) && admin->valuestring
                           && *admin->valuestring && cJSON_IsObject(users)) {
                    cJSON *me = cJSON_GetObjectItemCaseSensitive(users,
                                                                 admin->valuestring);

                    if (cJSON_IsObject(me)) {
                        const char *u = jstr(me, "username");

                        if (u && *u)
                            copy_field(out, outsz, u);
                    }
                }
            }
            /* Last resort: a users map containing exactly one user. Never scan
             * the whole state — that picks random suggested accounts. */
            if (!out[0] && cJSON_IsObject(users)) {
                cJSON *it;
                cJSON *only = NULL;
                int    n = 0;

                cJSON_ArrayForEach(it, users) {
                    if (!cJSON_IsObject(it))
                        continue;
                    n++;
                    only = it;
                }
                if (n == 1) {
                    const char *u = jstr(only, "username");

                    if (u && *u)
                        copy_field(out, outsz, u);
                }
            }
        }
    }
    cJSON_Delete(root);
}
