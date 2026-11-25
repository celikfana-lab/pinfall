/* img.c - PinFall image pipeline (v4).
 *
 *   HTTP fetch (libcurl, per-call handle, thread-safe)
 *     -> decode (stb_image) -> optional box downscale -> RGBA8888
 *     -> RAM cache in one of three modes:
 *          OFF  : nothing cached
 *          JPEG : the compressed source bytes are cached (small, decode on hit)
 *          LZ4  : packed RGBA is cached (fast restore, poor ratio on photos)
 *
 * Cache is a global, mutex-protected LRU with a byte budget; mode+budget are
 * runtime-changeable via pc_image_set_cache() (shrinking evicts synchronously).
 * No disk cache: the project constraint is "disk is 99% full".
 *
 */
#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http.h"
#include "lz4.h"
#include "pc.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define PC_IMG_CACHE_DEFAULT (64u * 1024u * 1024u) /* default budget          */
#define PC_IMG_FETCH_CAP     (8u * 1024u * 1024u)  /* per-image download cap  */
#define PC_IMG_MAX_RETRIES   2
#define PC_IMG_MAX_PIXELS    (64u * 1024u * 1024u) /* decode sanity bound      */
#define PC_PACK_HDR          16u                   /* magic + w + h + raw_len  */
#define PC_PACK_MAX_RAW      (256u * 1024u * 1024u)
#define PC_PACK_MAGIC        "PF41"

/* ------------------------------------------------------------------- state */

typedef struct ImgEnt {
    char          *key;
    unsigned char *data;
    size_t         len;
    struct ImgEnt *prev; /* towards MRU */
    struct ImgEnt *next; /* towards LRU */
} ImgEnt;

static ImgEnt         *g_head; /* MRU */
static ImgEnt         *g_tail; /* LRU */
static size_t          g_used;
static PcImgCacheMode  g_mode  = PC_IMG_CACHE_LZ4;
static size_t          g_budget = PC_IMG_CACHE_DEFAULT;
static uint64_t        g_hits, g_misses, g_packs, g_unpacks;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------ LRU helpers */

static void lru_unlink(ImgEnt *e)
{
    if (e->prev)
        e->prev->next = e->next;
    else
        g_head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        g_tail = e->prev;
    e->prev = NULL;
    e->next = NULL;
}

static void lru_push_front(ImgEnt *e)
{
    e->prev = NULL;
    e->next = g_head;
    if (g_head)
        g_head->prev = e;
    else
        g_tail = e;
    g_head = e;
}

/* Caller holds g_mu. */
static void entry_free(ImgEnt *e)
{
    free(e->key);
    free(e->data);
    free(e);
}

/* Caller holds g_mu. */
static void cache_clear_locked(void)
{
    ImgEnt *e = g_head;

    while (e) {
        ImgEnt *n = e->next;
        entry_free(e);
        e = n;
    }
    g_head = NULL;
    g_tail = NULL;
    g_used = 0;
}

/* Caller holds g_mu.  Evict LRU entries until within budget. */
static void cache_trim_locked(void)
{
    while (g_tail) {
        ImgEnt *e = g_tail;

        if (g_used <= g_budget)
            break;
        lru_unlink(e);
        g_used -= e->len;
        entry_free(e);
    }
}

static pthread_once_t g_exit_once = PTHREAD_ONCE_INIT;

static void flush_at_exit(void)
{
    pc_img_cache_flush();
}

static void register_atexit(void)
{
    atexit(flush_at_exit);
}

/* Drop the shared cache (called at exit; also usable by tests). */
void pc_img_cache_flush(void)
{
    pthread_mutex_lock(&g_mu);
    cache_clear_locked();
    pthread_mutex_unlock(&g_mu);
}

/* Copy a cache entry out (own copy: decode/unpack happens outside the lock). */
static bool cache_lookup(const char *key, unsigned char **data, size_t *len,
                         PcImgCacheMode *mode)
{
    ImgEnt *e;
    bool    hit = false;

    *data = NULL;
    *len  = 0;
    pthread_mutex_lock(&g_mu);
    *mode = g_mode;
    if (g_mode != PC_IMG_CACHE_OFF && g_budget > 0) {
        for (e = g_head; e; e = e->next) {
            if (strcmp(e->key, key) == 0)
                break;
        }
        if (e) {
            unsigned char *copy = malloc(e->len ? e->len : 1u);

            if (copy) {
                memcpy(copy, e->data, e->len);
                *data = copy;
                *len  = e->len;
                hit   = true;
                g_hits++;
                if (e != g_head) {
                    lru_unlink(e);
                    lru_push_front(e);
                }
            }
        }
    }
    if (!hit)
        g_misses++;
    pthread_mutex_unlock(&g_mu);
    return hit;
}

static void cache_drop(const char *key)
{
    ImgEnt *e;

    pthread_mutex_lock(&g_mu);
    for (e = g_head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            lru_unlink(e);
            g_used -= e->len;
            entry_free(e);
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

/* Store a compressed payload; cost is its byte length. */
static void cache_store(const char *key, const unsigned char *data, size_t len)
{
    ImgEnt *e;
    ImgEnt *it;

    if (!key || !data || len == 0)
        return;
    pthread_once(&g_exit_once, register_atexit);
    pthread_mutex_lock(&g_mu);
    if (g_mode == PC_IMG_CACHE_OFF || g_budget == 0 || len > g_budget) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    for (it = g_head; it; it = it->next) {
        if (strcmp(it->key, key) == 0) {
            lru_unlink(it);
            g_used -= it->len;
            entry_free(it);
            break;
        }
    }
    while (g_tail && (g_used > g_budget - len)) {
        it = g_tail;
        lru_unlink(it);
        g_used -= it->len;
        entry_free(it);
    }
    e = calloc(1, sizeof *e);
    if (!e) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    e->key = strdup(key);
    e->data = malloc(len);
    if (!e->key || !e->data) {
        entry_free(e);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    memcpy(e->data, data, len);
    e->len = len;
    lru_push_front(e);
    g_used += len;
    pthread_mutex_unlock(&g_mu);
}

static PcImgCacheMode current_mode(void)
{
    PcImgCacheMode m;

    pthread_mutex_lock(&g_mu);
    m = g_mode;
    pthread_mutex_unlock(&g_mu);
    return m;
}

/* ------------------------------------------------------- public cache API */

void pc_image_set_cache(PcClient *c, PcImgCacheMode mode, size_t budget_bytes)
{
    (void)c; /* cache is process-wide: single-client application */
    if (mode != PC_IMG_CACHE_OFF && mode != PC_IMG_CACHE_JPEG &&
        mode != PC_IMG_CACHE_LZ4)
        mode = PC_IMG_CACHE_OFF;
    pthread_once(&g_exit_once, register_atexit);
    pthread_mutex_lock(&g_mu);
    if (mode != g_mode && g_mode != PC_IMG_CACHE_OFF)
        cache_clear_locked(); /* payloads are mode-specific: never mix */
    g_mode   = mode;
    g_budget = budget_bytes;
    cache_trim_locked(); /* shrinking the budget evicts LRU synchronously */
    pthread_mutex_unlock(&g_mu);
}

void pc_image_get_stats(PcClient *c, PcImgStats *out)
{
    (void)c;
    if (!out)
        return;
    pthread_mutex_lock(&g_mu);
    out->mode         = g_mode;
    out->budget_bytes = g_budget;
    out->used_bytes   = g_used;
    out->hits         = g_hits;
    out->misses       = g_misses;
    out->packs        = g_packs;
    out->unpacks      = g_unpacks;
    pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------- download */

typedef struct {
    unsigned char *p;
    size_t         len;
    size_t         cap;
} Raw;

static size_t raw_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Raw   *r = ud;
    size_t n = size * nmemb;

    if (r->len + n > PC_IMG_FETCH_CAP)
        return 0; /* abort: image larger than the cap */
    if (r->len + n > r->cap) {
        size_t         nc = r->cap ? r->cap : 65536u;
        unsigned char *np;

        while (nc < r->len + n)
            nc *= 2;
        np = realloc(r->p, nc);
        if (!np)
            return 0;
        r->p   = np;
        r->cap = nc;
    }
    memcpy(r->p + r->len, ptr, n);
    r->len += n;
    return n;
}

static void sleep_ms(long ms)
{
    struct timespec ts;

    if (ms <= 0)
        return;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static bool img_fail(PcClient *c, char **err, const char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (err)
        *err = strdup(buf);
    if (c)
        pc_client_set_error(c, "%s", buf);
    return false;
}

bool pc_image_fetch_bytes(PcClient *c, const char *url, uint8_t **bytes,
                          size_t *len, char **err)
{
    int attempt;

    if (err)
        *err = NULL;
    if (bytes)
        *bytes = NULL;
    if (len)
        *len = 0;
    if (!c || !url || !*url || !bytes || !len) {
        if (err)
            *err = strdup("image: invalid arguments");
        return false;
    }

    pc_http_global_init();
    for (attempt = 0; attempt <= PC_IMG_MAX_RETRIES; attempt++) {
        CURL              *curl = NULL;
        struct curl_slist *hdrs = NULL;
        Raw                raw = {0, 0, 0};
        CURLcode           rc;
        long               status = 0;

        curl = curl_easy_init();
        if (!curl)
            return img_fail(c, err, "image: curl init failed");
        hdrs = curl_slist_append(hdrs, "Accept: image/avif,image/webp,"
                                        "image/jpeg,image/png,image/*;q=0.9,"
                                        "*/*;q=0.8");
        hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
        hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, PC_USER_AGENT);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, raw_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &raw);

        rc = curl_easy_perform(curl);
        if (rc == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc == CURLE_OK && status == 200 && raw.len > 0) {
            *bytes = raw.p;
            *len   = raw.len;
            pc_client_clear_error(c);
            return true;
        }
        free(raw.p);
        if (attempt < PC_IMG_MAX_RETRIES) {
            sleep_ms(500 * (attempt + 1));
            continue;
        }
        if (rc != CURLE_OK)
            return img_fail(c, err, "image: %s", curl_easy_strerror(rc));
        return img_fail(c, err, "image: HTTP %ld for %s", status, url);
    }
    return false;
}

/* --------------------------------------------------------------- downscale */

static uint8_t *downscale(const uint8_t *src, int sw, int sh, int nw, int nh)
{
    uint8_t *dst = malloc((size_t)nw * (size_t)nh * 4u);
    int      x, y;

    if (!dst)
        return NULL;
    for (y = 0; y < nh; y++) {
        int y0 = (int)((long)y * sh / nh);
        int y1 = (int)((long)(y + 1) * sh / nh);

        if (y1 <= y0)
            y1 = y0 + 1;
        if (y1 > sh)
            y1 = sh;
        for (x = 0; x < nw; x++) {
            int            x0 = (int)((long)x * sw / nw);
            int            x1 = (int)((long)(x + 1) * sw / nw);
            unsigned       sum[4] = {0, 0, 0, 0};
            unsigned       cnt = 0;
            int            xx, yy;
            uint8_t       *o;

            if (x1 <= x0)
                x1 = x0 + 1;
            if (x1 > sw)
                x1 = sw;
            for (yy = y0; yy < y1; yy++) {
                const uint8_t *row = src + ((size_t)yy * sw + x0) * 4u;
                for (xx = x0; xx < x1; xx++, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                    cnt++;
                }
            }
            if (!cnt)
                cnt = 1;
            o    = dst + ((size_t)y * nw + x) * 4u;
            o[0] = (uint8_t)(sum[0] / cnt);
            o[1] = (uint8_t)(sum[1] / cnt);
            o[2] = (uint8_t)(sum[2] / cnt);
            o[3] = (uint8_t)(sum[3] / cnt);
        }
    }
    return dst;
}

/* ------------------------------------------------------------- decode API */

bool pc_image_decode(const uint8_t *bytes, size_t len, int max_w,
                     uint8_t **pixels, int *w, int *h, char **err)
{
    int            iw = 0, ih = 0, n = 0;
    unsigned char *decoded;
    uint8_t       *final;
    int            fw, fh;

    if (err)
        *err = NULL;
    if (pixels)
        *pixels = NULL;
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    if (!bytes || len == 0 || !pixels || !w || !h || max_w < 0) {
        if (err)
            *err = strdup("image: invalid decode arguments");
        return false;
    }
    if (len > (size_t)0x7FFFFFFF || len > PC_IMG_FETCH_CAP) {
        if (err)
            *err = strdup("image: compressed data too large");
        return false;
    }

    decoded = stbi_load_from_memory(bytes, (int)len, &iw, &ih, &n, 4);
    if (!decoded) {
        if (err)
            *err = strdup("image: decode failed (unsupported or corrupt data)");
        return false;
    }
    if (iw <= 0 || ih <= 0 ||
        (unsigned long long)iw * (unsigned long long)ih >
            (unsigned long long)PC_IMG_MAX_PIXELS / 4u) {
        STBI_FREE(decoded);
        if (err)
            *err = strdup("image: bad or excessive dimensions");
        return false;
    }

    final = decoded;
    fw    = iw;
    fh    = ih;
    if (max_w > 0 && iw > max_w) {
        int      nw = max_w;
        int      nh = (int)((long long)ih * max_w / iw);
        uint8_t *ds;

        if (nh < 1)
            nh = 1;
        ds = downscale(decoded, iw, ih, nw, nh);
        if (ds) {
            STBI_FREE(decoded);
            final = ds;
            fw    = nw;
            fh    = nh;
        }
    }
    *pixels = final;
    *w      = fw;
    *h      = fh;
    return true;
}

/* --------------------------------------------------------- LZ4 pack format */

static void put_u32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool pc_image_pack(const uint8_t *rgba, int w, int h, uint8_t **out,
                   size_t *out_len, char **err)
{
    size_t         raw_len;
    int            bound;
    unsigned char *buf;
    int            clen;

    if (err)
        *err = NULL;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!rgba || !out || !out_len || w <= 0 || h <= 0) {
        if (err)
            *err = strdup("image: invalid pack arguments");
        return false;
    }
    if ((unsigned long long)w * (unsigned long long)h >
        (unsigned long long)PC_PACK_MAX_RAW / 4u) {
        if (err)
            *err = strdup("image: pack buffer too large");
        return false;
    }
    raw_len = (size_t)w * (size_t)h * 4u;
    if (raw_len > (size_t)0x7FFFFFFF) {
        if (err)
            *err = strdup("image: pack buffer too large");
        return false;
    }
    bound = LZ4_compressBound((int)raw_len);
    if (bound <= 0) {
        if (err)
            *err = strdup("image: lz4 compress bound failed");
        return false;
    }
    buf = malloc(PC_PACK_HDR + (size_t)bound);
    if (!buf) {
        if (err)
            *err = strdup("image: out of memory");
        return false;
    }
    memcpy(buf, PC_PACK_MAGIC, 4);
    put_u32le(buf + 4, (uint32_t)w);
    put_u32le(buf + 8, (uint32_t)h);
    put_u32le(buf + 12, (uint32_t)raw_len);
    clen = LZ4_compress_default((const char *)rgba, (char *)buf + PC_PACK_HDR,
                                (int)raw_len, bound);
    if (clen <= 0) {
        free(buf);
        if (err)
            *err = strdup("image: lz4 compress failed");
        return false;
    }
    *out     = buf;
    *out_len = PC_PACK_HDR + (size_t)clen;
    return true;
}

bool pc_image_unpack(const uint8_t *packed, size_t len, uint8_t **rgba,
                     int *w, int *h, char **err)
{
    uint32_t    uw, uh, raw_len;
    uint64_t    expect;
    unsigned char *buf;
    int         rc;

    if (err)
        *err = NULL;
    if (rgba)
        *rgba = NULL;
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    if (!packed || !rgba || !w || !h || len < PC_PACK_HDR) {
        if (err)
            *err = strdup("image: truncated packed buffer");
        return false;
    }
    if (memcmp(packed, PC_PACK_MAGIC, 4) != 0) {
        if (err)
            *err = strdup("image: bad pack magic");
        return false;
    }
    uw      = get_u32le(packed + 4);
    uh      = get_u32le(packed + 8);
    raw_len = get_u32le(packed + 12);
    expect  = (uint64_t)uw * (uint64_t)uh * 4u;
    if (uw == 0 || uh == 0 || expect != (uint64_t)raw_len ||
        expect > (uint64_t)PC_PACK_MAX_RAW || expect > (uint64_t)0x7FFFFFFF ||
        len - PC_PACK_HDR > (size_t)0x7FFFFFFF) {
        if (err)
            *err = strdup("image: inconsistent pack header");
        return false;
    }
    buf = malloc(raw_len ? raw_len : 1u);
    if (!buf) {
        if (err)
            *err = strdup("image: out of memory");
        return false;
    }
    rc = LZ4_decompress_safe((const char *)packed + PC_PACK_HDR, (char *)buf,
                             (int)(len - PC_PACK_HDR), (int)raw_len);
    if (rc < 0 || (uint32_t)rc != raw_len) {
        free(buf);
        if (err)
            *err = strdup("image: corrupt lz4 payload");
        return false;
    }
    *rgba = buf;
    *w    = (int)uw;
    *h    = (int)uh;
    return true;
}

/* -------------------------------------------------------------- get + cache */

bool pc_image_get(PcClient *c, const char *url, int max_w, uint8_t **pixels,
                  int *w, int *h, char **err)
{
    char           key[800];
    unsigned char *cached = NULL;
    size_t         clen = 0;
    PcImgCacheMode mode = PC_IMG_CACHE_OFF;
    uint8_t       *bytes = NULL;
    size_t         blen = 0;
    uint8_t       *rgba = NULL;
    int            rw = 0, rh = 0;

    if (err)
        *err = NULL;
    if (pixels)
        *pixels = NULL;
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    if (!c || !url || !*url) {
        if (err)
            *err = strdup("image: invalid arguments");
        pc_client_set_error(c, "image: invalid arguments");
        return false;
    }
    if (!pixels || !w || !h || max_w < 0)
        return img_fail(c, err, "image: invalid output pointers");

    snprintf(key, sizeof key, "%s|%d", url, max_w > 0 ? max_w : 0);

    if (cache_lookup(key, &cached, &clen, &mode)) {
        bool ok = false;

        if (mode == PC_IMG_CACHE_JPEG) {
            /* Revisit: decode the small cached source bytes again. */
            ok = pc_image_decode(cached, clen, max_w, pixels, w, h, err);
        } else if (mode == PC_IMG_CACHE_LZ4) {
            ok = pc_image_unpack(cached, clen, pixels, w, h, err);
            if (ok) {
                pthread_mutex_lock(&g_mu);
                g_unpacks++;
                pthread_mutex_unlock(&g_mu);
            }
        }
        free(cached);
        if (ok) {
            pc_client_clear_error(c);
            return true;
        }
        /* Corrupt/undecodable cache payload: drop it and re-fetch below. */
        cache_drop(key);
        pthread_mutex_lock(&g_mu);
        g_misses++;
        pthread_mutex_unlock(&g_mu);
        mode = current_mode();
    } else {
        mode = current_mode();
    }

    if (!pc_image_fetch_bytes(c, url, &bytes, &blen, err))
        return false;
    if (!pc_image_decode(bytes, blen, max_w, &rgba, &rw, &rh, err)) {
        free(bytes);
        return false;
    }

    if (mode == PC_IMG_CACHE_JPEG) {
        cache_store(key, bytes, blen); /* small: source bytes */
    } else if (mode == PC_IMG_CACHE_LZ4) {
        uint8_t *packed = NULL;
        size_t   plen = 0;
        char    *perr = NULL;

        if (pc_image_pack(rgba, rw, rh, &packed, &plen, &perr)) {
            pthread_mutex_lock(&g_mu);
            g_packs++;
            pthread_mutex_unlock(&g_mu);
            cache_store(key, packed, plen);
        }
        pc_free(perr);
        pc_free(packed);
    }
    free(bytes);

    *pixels = rgba;
    *w      = rw;
    *h      = rh;
    pc_client_clear_error(c);
    return true;
}
