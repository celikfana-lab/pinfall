/* pc.h - PinFall: stable interface contract between the network layer
 *        (src/http.c, src/api.c, src/img.c, src/jsonx.c) and the SDL2 UI
 *        (src/main.c, src/ui.c, src/render.c, src/layout.c, src/font.c).
 *
 * This header is the stable contract between the two layers. Treat it as
 * frozen: change the signatures only with a matching change on both sides.
 */
#ifndef PINFALL_PC_H
#define PINFALL_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- limits */
/* Measured against live data 2026-10-27:
 * - pin ids are 57 chars ("e1_AfqPSh..." new format) -> 64
 * - resource_response.bookmark is ~2064 chars             -> 8192 (headroom)
 * Always use sizeof(field), never assume the cursor length. */
#define PC_ID_LEN      64
#define PC_TITLE_LEN   256
#define PC_DESC_LEN    2048
#define PC_URL_LEN     640
#define PC_SHORT_LEN   160
#define PC_COLOR_LEN   16
#define PC_BOOKMARK_LEN 8192

/* ----------------------------------------------------------------- model */
typedef struct {
    char id[PC_ID_LEN];
    char title[PC_TITLE_LEN];
    char description[PC_DESC_LEN];
    char domain[PC_SHORT_LEN];       /* e.g. "ikea.com" */
    char link[PC_URL_LEN];           /* outbound source link */
    char board_name[PC_SHORT_LEN];
    char pinner_name[PC_SHORT_LEN];
    char thumb_url[PC_URL_LEN];      /* ~236px wide  (grid)   */
    char medium_url[PC_URL_LEN];     /* ~474px wide  (detail) */
    char big_url[PC_URL_LEN];        /* ~736px wide  (detail zoom) */
    char orig_url[PC_URL_LEN];       /* originals */
    char dominant_color[PC_COLOR_LEN]; /* "#rrggbb" or "" */
    int  img_w, img_h;               /* natural size of thumb */
    /* social counts (v6): filled when the payload provides them, else 0 */
    int  like_count;                 /* reaction_counts["1"]            */
    int  comment_count;              /* close-up query, 0 if unknown    */
    bool liked;                      /* viewer reacted to this pin      */
    /* video / animated pin (v8): Pinterest encodes GIFs as short videos and
     * serves HLS (.m3u8) by default, sometimes a progressive mp4. */
    bool is_video;                   /* payload had a videos{} block    */
    char video_url[PC_URL_LEN];      /* HLS or mp4 URL (best available) */
    int  video_w, video_h;           /* native video dimensions         */
    int  video_duration_ms;          /* 0 when unknown                  */
} PcPin;

typedef struct {
    PcPin *items;
    size_t count;
    size_t cap;
    char   bookmark[PC_BOOKMARK_LEN]; /* opaque next-page cursor, "" = none */
} PcPinList;

/* ---------------------------------------------------------------- client */
typedef struct PcClient PcClient;

/* Create/destroy. cache_dir may be NULL (defaults to $HOME/.cache/pinfall).
 * All pc_* functions are thread-safe: the UI may drive them from a worker
 * thread while the main thread only talks to SDL. */
PcClient *pc_client_new(const char *cache_dir);
void      pc_client_free(PcClient *c);

/* Human readable last error for this client (never NULL, "" if none). */
const char *pc_last_error(PcClient *c);

/* Step 1: guest session.
 * GET $PC_HOST/  (default https://nl.pinterest.com), cookie jar retained,
 * csrftoken extracted. Must be called once before pc_search(). */
bool pc_bootstrap(PcClient *c);

/* Step 2: search.
 *   query    : UTF-8 search terms
 *   bookmark : NULL or "" for page 1, else out->bookmark from the last call
 * On success: true, out filled (caller frees with pc_pinlist_free()).
 * On failure: false and *err set to a malloc'd string (free with pc_free()).
 * *err is optional and may be NULL. */
bool pc_search(PcClient *c, const char *query, const char *bookmark,
               PcPinList *out, char **err);

/* Step 3: single pin detail. Best effort: returns false if the resource
 * requires auth. Fills *out (fields not provided stay empty). */
bool pc_pin_detail(PcClient *c, const char *pin_id, PcPin *out, char **err);

/* Step 4: fetch + decode an image to RGBA8888 (malloc'd, *pixels).
 * Results are cached in memory (LRU, ~64 MB cap) keyed by (url,max_w).
 * max_w > 0 downscales to at most max_w pixels wide; 0 = native.
 * Thread-safe. Returns true on success. Free *pixels with pc_free(). */
bool pc_image_get(PcClient *c, const char *url, int max_w,
                  uint8_t **pixels, int *w, int *h, char **err);

/* --------------------------------------------------------------- helpers */
void pc_pinlist_free(PcPinList *l);   /* frees items, zeroes the struct */
void pc_free(void *p);                /* free err strings and pixel buffers */
int  pc_utf8_ellipsize(char *dst, size_t dstsz, const char *src);

/* ------------------------------------------- session / auth  (v2, additive)
 *
 * Login model: Pinterest has no supported third-party auth API, and its login
 * page is protected by reCAPTCHA + Arkose FunCaptcha, so PinFall NEVER handles
 * passwords. Instead the user signs in normally in their own browser (this
 * covers "Continue with Google") and imports the resulting cookie jar in
 * Netscape cookies.txt format. Those cookies are the whole session.
 */

typedef enum {
    PC_AUTH_UNKNOWN   = 0,
    PC_AUTH_GUEST     = 1,   /* cookies present or absent, not signed in */
    PC_AUTH_LOGGED_IN = 2
} PcAuthState;

/* Replace the client's cookie jar with a Netscape-format cookies.txt.
 * Returns false + err on unreadable/empty file. Never logs cookie values. */
bool pc_cookie_load(PcClient *c, const char *path, char **err);

/* Write the current jar as Netscape cookies.txt, mode 0600. Returns false+err
 * on I/O error. Caller decides whether to persist (opt-in flag in the UI). */
bool pc_cookie_save(PcClient *c, const char *path, char **err);

/* Probe auth state at most once (one UserHomefeedResource request), then
 * cache it. Returns PC_AUTH_UNKNOWN only if the probe itself failed
 * (network error) — callers should then stay in guest mode. */
PcAuthState pc_auth_check(PcClient *c);

/* Best-effort display name of the signed-in user; "" when guest/unknown.
 * Never NULL. */
const char *pc_username(PcClient *c);

/* Personal home feed — requires an authenticated jar. `bookmark` NULL/"" for
 * page 1, else out->bookmark. On 401 returns false and sets *err to a string
 * containing "login required"; callers must NOT retry. */
bool pc_homefeed(PcClient *c, const char *bookmark, PcPinList *out, char **err);

/* --------------------------------- saved pins / boards / saving  (v3) ---- */

#define PC_BOARD_NAME_LEN 200
#define PC_PRIVACY_LEN    24

typedef struct {
    char id[PC_ID_LEN];                 /* numeric board id as string */
    char name[PC_BOARD_NAME_LEN];
    char url[PC_URL_LEN];               /* "/user/board-slug/" */
    char privacy[PC_PRIVACY_LEN];       /* "public" | "secret" | ... */
    int  pin_count;
} PcBoard;

typedef struct {
    PcBoard *items;
    size_t   count, cap;
    char     bookmark[PC_BOOKMARK_LEN];
} PcBoardList;

/* Boards of `username` (NULL/"" = the signed-in user). Verified live:
 * BoardsResource/get returns data as a LIST of board objects.
 * Works for public profiles even as a guest. */
bool pc_user_boards(PcClient *c, const char *username, PcBoardList *out,
                    char **err);

/* Pins saved by `username` (NULL/"" = the signed-in user) — this is the
 * "Saved" tab. Verified live: UserPinsResource/get -> data[] of pin objects,
 * same dual shape as search, bookmark pagination. For the signed-in user this
 * requires an authenticated jar (401 -> login required). */
bool pc_user_pins(PcClient *c, const char *username, const char *bookmark,
                  PcPinList *out, char **err);

/* Save (repin) pin `pin_id` to board `board_id`. WRITE OPERATION.
 * Verified to exist: RepinResource/create -> 401 "Authorization failed." for
 * guests. Requires an authenticated jar; returns false + "login required"
 * otherwise. On success returns true (response success status is checked).
 * Callers must treat this as irreversible and only invoke it on explicit
 * user action. */
bool pc_save_pin(PcClient *c, const char *pin_id, const char *board_id,
                 char **err);

void pc_boardlist_free(PcBoardList *l);

/* ------------------------------------- in-memory image cache + pack (v4) --
 *
 * RAM policy: the UI keeps GPU textures only for the pins near the viewport;
 * everything else may be cached in RAM in one of these forms.
 *
 *   PC_IMG_CACHE_OFF   no cache: re-download + re-decode on revisit
 *   PC_IMG_CACHE_JPEG  keep the compressed source bytes (thumbnail JPEG is
 *                      ~10-15 KB; smallest RAM per pin, decode on revisit)
 *   PC_IMG_CACHE_LZ4   keep LZ4-packed decoded RGBA (restore is a ~0.1 ms
 *                      decompress, but a photo packs only ~1.1-1.4x)
 *
 * All functions below are thread-safe.
 */
typedef enum {
    PC_IMG_CACHE_OFF  = 0,
    PC_IMG_CACHE_JPEG = 1,
    PC_IMG_CACHE_LZ4  = 2
} PcImgCacheMode;

typedef struct {
    PcImgCacheMode mode;
    size_t  budget_bytes;   /* configured cap for the RAM cache       */
    size_t  used_bytes;     /* current cache occupancy                */
    uint64_t hits, misses;  /* lookups that hit / had to fetch        */
    uint64_t packs, unpacks;/* LZ4 compress / decompress operations   */
} PcImgStats;

/* Configure mode + budget (bytes). Call after pc_client_new(); may be called
 * again at runtime (e.g. from a settings screen): the new policy takes effect
 * immediately and (when shrinking) evicts LRU entries synchronously. */
void pc_image_set_cache(PcClient *c, PcImgCacheMode mode, size_t budget_bytes);
void pc_image_get_stats(PcClient *c, PcImgStats *out);

/* Raw compressed bytes fetch (no decode). Thread-safe. Free with pc_free(). */
bool pc_image_fetch_bytes(PcClient *c, const char *url, uint8_t **bytes,
                          size_t *len, char **err);

/* Decode already-downloaded bytes to RGBA8888. Thread-safe. */
bool pc_image_decode(const uint8_t *bytes, size_t len, int max_w,
                     uint8_t **pixels, int *w, int *h, char **err);

/* LZ4 pack/unpack of a decoded RGBA8888 buffer. Self-contained (no client).
 * The packed format stores w/h so unpack returns them. Free with pc_free(). */
bool pc_image_pack(const uint8_t *rgba, int w, int h, uint8_t **out,
                   size_t *out_len, char **err);
bool pc_image_unpack(const uint8_t *packed, size_t len, uint8_t **rgba,
                     int *w, int *h, char **err);

/* ------------------------------------------------- ad blocking (v5) -------
 *
 * Pinterest marks promoted/sponsored pins in the resource payload itself
 * (verified live: `is_promoted: true`, sometimes `promoted_is_lead_ad`,
 * `is_downstream_promotion`, `ad_match_reason`). This is a parse-side filter:
 * blocked pins never reach the feed, so no layout slot/network is spent on
 * them. NOTE: `aggregated_pin_data` is present on ordinary pins too and must
 * NOT be used as an ad signal.
 *
 * Thread-safe; default off, the UI turns it on from Settings.
 */
void     pc_set_block_ads(PcClient *c, bool block);
bool     pc_get_block_ads(PcClient *c);
/* Cumulative number of promoted pins skipped by the parser. */
uint64_t pc_ads_blocked(PcClient *c);

/* ------------------- close-up: related pins, comments, reactions (v6) ----
 *
 * Pinterest's close-up data is served by POSTing to /_/graphql/ with a
 * *persisted query hash* + variables, so these functions encapsulate that
 * transport. They require a canonical (numeric) pin id; search results often
 * carry "e1_..." ids that Pinterest itself does not resolve on /pin/<id>/.
 *
 * All reads are thread-safe. Writes are explicit-only, never retried, and
 * return false + "login required" for guests.
 */

#define PC_COMMENT_TEXT_LEN 1024
#define PC_COMMENT_NAME_LEN 120

typedef struct {
    char id[PC_ID_LEN];
    char user_name[PC_COMMENT_NAME_LEN];
    char text[PC_COMMENT_TEXT_LEN];
    char created_at[48];             /* display string, may be "" */
    int  like_count;
} PcComment;

typedef struct {
    PcComment *items;
    size_t     count, cap;
    char       bookmark[PC_BOOKMARK_LEN];
} PcCommentList;

/* Related pins for a canonical pin id (Pinterest's "More like this"). */
bool pc_pin_related(PcClient *c, const char *pin_id, const char *bookmark,
                    PcPinList *out, char **err);

/* Comments for a canonical pin id, newest-first, bookmark paginated. */
bool pc_pin_comments(PcClient *c, const char *pin_id, const char *bookmark,
                     PcCommentList *out, char **err);

/* WRITE: add a comment. Explicit user action only; no retry. */
bool pc_add_comment(PcClient *c, const char *pin_id, const char *text,
                    char **err);

/* WRITE: like/unlike a pin. Explicit user action only; no retry. */
bool pc_set_pin_liked(PcClient *c, const char *pin_id, bool liked, char **err);

void pc_commentlist_free(PcCommentList *l);

/* ------------------------------------------- board creation (v7) ---------
 *
 * WRITE: create a new board for the signed-in user. Explicit user action
 * only; no retry. `privacy` is "public" or "secret" (NULL -> "public").
 * On success fills *out (id/name/url/privacy) when the API returns a board
 * object; if the response omits it, the caller can refresh pc_user_boards().
 */
bool pc_create_board(PcClient *c, const char *name, const char *description,
                     const char *privacy, PcBoard *out, char **err);

/* --------------------------------------- video / animated decode (v8) ----
 *
 * Backend: the ffmpeg CLI (present on this system) is spawned as a decoder
 * subprocess and streams raw RGBA frames over a pipe. That covers H.264/HLS
 * (Pinterest's default for videos and GIF pins), animated GIF/WebP, etc.,
 * without linking libav* and without buffering whole files in RAM.
 * Local file paths are accepted too (used by tests with synthetic clips).
 *
 * Per-instance objects; pc_video_read_frame blocks until a frame is ready.
 */
typedef struct PcVideo PcVideo;

/* out_w/out_h: requested frame size (caller computes aspect fit).
 * fps: output frame-rate cap (Pinterest videos are usually 24-30). */
PcVideo *pc_video_open(const char *url_or_path, int out_w, int out_h, int fps,
                       char **err);

/* Read one frame as exactly out_w*out_h*4 bytes RGBA. false on EOF/error. */
bool pc_video_read_frame(PcVideo *v, uint8_t *rgba_out);

/* Close the decoder and reap/kill the subprocess. Safe on NULL. */
void pc_video_close(PcVideo *v);

void pc_video_size(PcVideo *v, int *w, int *h);

/* True when an ffmpeg binary is usable (friendly error when it is not). */
bool pc_video_available(void);

#ifdef __cplusplus
}
#endif
#endif /* PINFALL_PC_H */
