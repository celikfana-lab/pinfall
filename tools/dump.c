/* dump.c - PinFall network-layer acceptance CLI .
 *
 *   dump [-n N] [--cookies FILE] <query...>   search page 1, one line per pin:
 *                                             id | WxH | board | title | thumb_url
 *   dump --image URL                          download + decode an image
 *   dump --detail ID                          best-effort pc_pin_detail
 *   dump --auth                               print auth state + username
 *   dump --homefeed [-n N]                    personal feed (needs cookie login)
 *   dump --boards [--user NAME]               saved boards: id | name | pins |
 *                                             privacy | url
 *   dump --user-pins [-n N] [--page P] [--user NAME]
 *                                             pins saved by a profile
 *   dump --save PIN_ID BOARD_ID               WRITE: save/repin (requires
 *                                             PC_ALLOW_WRITE=1 + logged in)
 *   dump --ads <query...>                     ad-block evidence: parse the same
 *                                             response off/on:
 *                                             pins=N ads_detected=M ads_blocked=M
 *                                             block_flag=1
 *   dump --block-ads 0|1 <query...>           set the client ad filter for
 *                                             normal search/home/saved feeds
 *   dump --related PIN_ID                     related pins (numeric close-up id)
 *   dump --comments PIN_ID [-n N]             comments (numeric close-up id)
 *   dump --like PIN_ID [--unlike]             WRITE: like/unlike (requires
 *                                             PC_ALLOW_WRITE=1 + logged in)
 *   dump --create-board "<name>" [-p public|secret]
 *                                             WRITE: create a board (requires
 *                                             PC_ALLOW_WRITE=1 + logged in)
 *   dump --video PIN_ID                       video metadata + first-frame decode
 *   dump --video-frames PIN_ID|PATH|URL [N]   decode N frames (default 5) with
 *                                             per-frame checksums
 *
 * Exit codes: 0 ok (>=1 pin for search/homefeed), 1 no pins, 2 bootstrap or
 * cookie-file failure, 3 search/homefeed error incl. "login required",
 * 4 detail failure, 5 image failure.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pc.h"
#include "http.h"
#include "media.h"

#define EXIT_OK             0
#define EXIT_NO_PINS        1
#define EXIT_BOOTSTRAP      2
#define EXIT_SEARCH         3
#define EXIT_LOGIN_REQUIRED 3
#define EXIT_DETAIL         4
#define EXIT_IMAGE          5

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-n N] [--cookies FILE] <query...>\n"
            "       %s --image URL\n"
            "       %s --detail ID\n"
            "       %s --auth\n"
            "       %s --homefeed [-n N] [--cookies FILE]\n"
            "       %s --boards [--user NAME] [--cookies FILE]\n"
            "       %s --user-pins [-n N] [--page P] [--user NAME]\n"
            "       %s --save PIN_ID BOARD_ID\n"
            "       %s --ads <query...>\n"
            "       %s --block-ads 0|1 <query...>\n"
            "       %s --related PIN_ID\n"
            "       %s --comments PIN_ID [-n N]\n"
            "       %s --like PIN_ID [--unlike]\n"
            "       %s --create-board \"<name>\" [-p public|secret]\n"
            "       %s --video PIN_ID\n"
            "       %s --video-frames PIN_ID|PATH|URL [N]\n"
            "  -n N          print at most N pins (default: all parsed)\n"
            "  --cookies F   load a Netscape cookies.txt before talking to\n"
            "                Pinterest (browser cookie login; values never printed)\n"
            "  --image URL   download + decode an image, print its size\n"
            "  --detail ID   best-effort single-pin detail lookup\n"
            "  --auth        probe auth state once, print auth=... + username=...\n"
            "  --homefeed    personal feed (guest exits 3 with 'login required')\n"
            "  --boards      list boards of --user (default: signed-in user)\n"
            "  --user-pins   list pins saved by --user (default: signed-in user)\n"
            "  --page P      for --user-pins: fetch page P via bookmark (default 1)\n"
            "  --user NAME   profile username for --boards/--user-pins\n"
            "  --save P B    WRITE: repin pin P to board B. Refused unless env\n"
            "                PC_ALLOW_WRITE=1; requires a signed-in session\n"
            "  --ads Q       parse one search response with the ad filter off\n"
            "                then on; prints pins/ads_detected/ads_blocked\n"
            "  --block-ads N enable (1) or disable (0) the parse-side ad filter\n"
            "  --related P   related pins for a canonical/numeric pin id\n"
            "  --comments P  comments for a canonical/numeric pin id\n"
            "  --like P      WRITE: like pin P (needs PC_ALLOW_WRITE=1)\n"
            "  --unlike      with --like: remove the like instead\n"
            "  --create-board N  WRITE: create board N (needs PC_ALLOW_WRITE=1)\n"
            "  -p M          privacy for --create-board: public|secret\n"
            "  --video P     video pin metadata + first-frame decode check\n"
            "  --video-frames A [N]  decode N frames of a pin id/path/URL\n"
            "env: PC_HOST overrides https://nl.pinterest.com\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
            argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static const char *auth_name(PcAuthState st)
{
    switch (st) {
    case PC_AUTH_LOGGED_IN:
        return "logged_in";
    case PC_AUTH_GUEST:
        return "guest";
    default:
        return "unknown";
    }
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static size_t print_pins(const PcPinList *list, int limit)
{
    size_t printed = 0;
    size_t i;

    for (i = 0; i < list->count; i++) {
        const PcPin *p = &list->items[i];

        if (limit > 0 && printed >= (size_t)limit)
            break;
        printf("%s | %dx%d | %s | %s | %s\n", p->id, p->img_w, p->img_h,
               p->board_name, p->title, p->thumb_url);
        printed++;
    }
    return printed;
}

static int run_image(PcClient *c, const char *url)
{
    uint8_t *pixels = NULL;
    int      w = 0, h = 0;
    char    *err = NULL;
    double   t0 = now_ms();

    if (!pc_image_get(c, url, 736, &pixels, &w, &h, &err)) {
        fprintf(stderr, "dump: image failed: %s\n",
                err ? err : pc_last_error(c));
        pc_free(err);
        return EXIT_IMAGE;
    }
    printf("image %dx%d rgba %zu bytes %.0fms %s\n", w, h,
           (size_t)w * (size_t)h * 4u, now_ms() - t0, url);
    pc_free(pixels);
    pc_free(err);
    return EXIT_OK;
}

static int run_detail(PcClient *c, const char *pin_id)
{
    PcPin  pin;
    char  *err = NULL;

    if (!pc_pin_detail(c, pin_id, &pin, &err)) {
        fprintf(stderr, "dump: detail failed: %s\n",
                err ? err : pc_last_error(c));
        pc_free(err);
        return EXIT_DETAIL;
    }
    printf("%s | %dx%d | board=%s | pinner=%s\n", pin.id, pin.img_w, pin.img_h,
           pin.board_name, pin.pinner_name);
    printf("title: %s\n", pin.title);
    printf("desc: %s\n", pin.description);
    printf("domain: %s | link: %s | color: %s\n", pin.domain, pin.link,
           pin.dominant_color);
    printf("thumb: %s\n", pin.thumb_url);
    printf("medium: %s\n", pin.medium_url);
    printf("big: %s\n", pin.big_url);
    printf("orig: %s\n", pin.orig_url);
    pc_free(err);
    return EXIT_OK;
}

static int run_auth(PcClient *c)
{
    PcAuthState st = pc_auth_check(c);

    printf("auth=%s\n", auth_name(st));
    printf("username=%s\n", pc_username(c));
    if (st == PC_AUTH_UNKNOWN) {
        fprintf(stderr, "dump: auth probe inconclusive: %s\n",
                pc_last_error(c));
        return EXIT_BOOTSTRAP;
    }
    return EXIT_OK;
}

static int run_homefeed(PcClient *c, int limit)
{
    PcPinList   list;
    char       *err = NULL;
    PcAuthState st;
    double      t0;
    size_t      printed;
    int         rc;

    st = pc_auth_check(c);
    fprintf(stderr, "dump: auth=%s username=%s\n", auth_name(st),
            pc_username(c));

    memset(&list, 0, sizeof list);
    t0 = now_ms();
    if (!pc_homefeed(c, NULL, &list, &err)) {
        const char *msg = err ? err : pc_last_error(c);

        if (strstr(msg, "login required")) {
            fprintf(stderr, "dump: %s\n", msg);
            pc_free(err);
            return EXIT_LOGIN_REQUIRED;
        }
        fprintf(stderr, "dump: homefeed failed: %s\n", msg);
        pc_free(err);
        return EXIT_SEARCH;
    }
    fprintf(stderr, "dump: %zu homefeed pins in %.0fms (bookmark %zu bytes)\n",
            list.count, now_ms() - t0, strlen(list.bookmark));

    printed = print_pins(&list, limit);
    rc = (list.count > 0) ? EXIT_OK : EXIT_NO_PINS;
    if (list.count == 0)
        fprintf(stderr, "dump: no pins parsed\n");
    (void)printed;

    pc_pinlist_free(&list);
    return rc;
}

static int run_boards(PcClient *c, const char *user)
{
    PcBoardList bl;
    char       *err = NULL;
    size_t      i;
    int         rc;

    memset(&bl, 0, sizeof bl);
    if (!pc_user_boards(c, user, &bl, &err)) {
        const char *msg = err ? err : pc_last_error(c);

        if (strstr(msg, "login required")) {
            fprintf(stderr, "dump: %s\n", msg);
            pc_free(err);
            return EXIT_LOGIN_REQUIRED;
        }
        fprintf(stderr, "dump: boards failed: %s\n", msg);
        pc_free(err);
        return EXIT_SEARCH;
    }
    for (i = 0; i < bl.count; i++) {
        const PcBoard *b = &bl.items[i];

        printf("%s | %s | %d | %s | %s\n", b->id, b->name, b->pin_count,
               b->privacy, b->url);
    }
    fprintf(stderr, "dump: %zu boards for '%s' (bookmark %zu bytes)\n", bl.count,
            user ? user : "(signed-in user)", strlen(bl.bookmark));
    rc = (bl.count > 0) ? EXIT_OK : EXIT_NO_PINS;
    if (bl.count == 0)
        fprintf(stderr, "dump: no boards parsed\n");
    pc_boardlist_free(&bl);
    return rc;
}

static int run_user_pins(PcClient *c, const char *user, int page, int limit)
{
    PcPinList list;
    char      *err = NULL;
    char       bookmark[PC_BOOKMARK_LEN];
    int        p;
    int        rc;
    double     t0;

    memset(&list, 0, sizeof list);
    bookmark[0] = '\0';
    if (page < 1)
        page = 1;

    t0 = now_ms();
    for (p = 1; p <= page; p++) {
        const char *bm = (p == 1) ? NULL : bookmark;

        if (!pc_user_pins(c, user, (bm && bm[0]) ? bm : NULL, &list, &err)) {
            const char *msg = err ? err : pc_last_error(c);

            if (strstr(msg, "login required")) {
                fprintf(stderr, "dump: %s\n", msg);
                pc_free(err);
                return EXIT_LOGIN_REQUIRED;
            }
            fprintf(stderr, "dump: user pins failed: %s\n", msg);
            pc_free(err);
            return EXIT_SEARCH;
        }
        if (p == page)
            break;
        snprintf(bookmark, sizeof bookmark, "%s", list.bookmark);
        pc_pinlist_free(&list);
        if (!bookmark[0]) {
            fprintf(stderr, "dump: only %d page(s) available\n", p);
            return EXIT_NO_PINS;
        }
    }
    fprintf(stderr, "dump: page %d: %zu pins in %.0fms (bookmark %zu bytes)\n",
            page, list.count, now_ms() - t0, strlen(list.bookmark));
    print_pins(&list, limit);
    rc = (list.count > 0) ? EXIT_OK : EXIT_NO_PINS;
    if (list.count == 0)
        fprintf(stderr, "dump: no pins parsed\n");
    pc_pinlist_free(&list);
    return rc;
}

static int run_save(PcClient *c, const char *pin_id, const char *board_id)
{
    char *err = NULL;

    if (!pc_save_pin(c, pin_id, board_id, &err)) {
        const char *msg = err ? err : pc_last_error(c);

        if (strstr(msg, "login required")) {
            fprintf(stderr, "dump: %s\n", msg);
            pc_free(err);
            return EXIT_LOGIN_REQUIRED;
        }
        fprintf(stderr, "dump: save failed: %s\n", msg);
        pc_free(err);
        return EXIT_SEARCH;
    }
    printf("saved pin %s to board %s\n", pin_id, board_id);
    pc_free(err);
    return EXIT_OK;
}

static int run_ads(PcClient *c, const char *query)
{
    int      pins = 0;
    uint64_t detected = 0;
    uint64_t blocked = 0;
    char    *err = NULL;

    if (!query || !*query) {
        fprintf(stderr, "dump: --ads needs a search query\n");
        return EXIT_SEARCH;
    }
    if (!pc_search_ads_report(c, query, &pins, &detected, &blocked, &err)) {
        /* Outputs are filled even for invariant failures: print evidence. */
        printf("pins=%d ads_detected=%llu ads_blocked=%llu block_flag=1\n", pins,
               (unsigned long long)detected, (unsigned long long)blocked);
        fprintf(stderr, "dump: ads report failed: %s\n",
                err ? err : pc_last_error(c));
        pc_free(err);
        return EXIT_SEARCH;
    }
    printf("pins=%d ads_detected=%llu ads_blocked=%llu block_flag=1\n", pins,
           (unsigned long long)detected, (unsigned long long)blocked);
    fprintf(stderr,
            "dump: ads query='%s' pins=%d detected=%llu blocked=%llu "
            "(filtered feed would show %d)\n",
            query, pins, (unsigned long long)detected,
            (unsigned long long)blocked, pins - (int)blocked);
    pc_free(err);
    return EXIT_OK;
}

static int run_related(PcClient *c, const char *pin_id, int limit)
{
    PcPinList list;
    char     *err = NULL;
    size_t    printed;

    memset(&list, 0, sizeof list);
    if (!pc_pin_related(c, pin_id, NULL, &list, &err)) {
        const char *msg = err ? err : pc_last_error(c);

        fprintf(stderr, "dump: related failed: %s\n", msg);
        pc_free(err);
        return EXIT_SEARCH;
    }
    fprintf(stderr, "dump: %zu related pins (bookmark %zu bytes)\n", list.count,
            strlen(list.bookmark));
    printed = print_pins(&list, limit);
    (void)printed;
    {
        int rc = (list.count > 0) ? EXIT_OK : EXIT_NO_PINS;

        pc_pinlist_free(&list);
        return rc;
    }
}

static int run_comments(PcClient *c, const char *pin_id, int limit)
{
    PcCommentList cl;
    char         *err = NULL;
    size_t        i, printed = 0;

    memset(&cl, 0, sizeof cl);
    if (!pc_pin_comments(c, pin_id, NULL, &cl, &err)) {
        const char *msg = err ? err : pc_last_error(c);

        fprintf(stderr, "dump: comments failed: %s\n", msg);
        pc_free(err);
        return EXIT_SEARCH;
    }
    for (i = 0; i < cl.count; i++) {
        const PcComment *cm = &cl.items[i];

        if (limit > 0 && printed >= (size_t)limit)
            break;
        printf("%s | %s | %s | %d | %s\n", cm->id, cm->user_name,
               cm->created_at, cm->like_count, cm->text);
        printed++;
    }
    fprintf(stderr, "dump: %zu comments (bookmark %zu bytes)\n", cl.count,
            strlen(cl.bookmark));
    pc_commentlist_free(&cl);
    return EXIT_OK; /* a successful comments read, even when empty */
}

static int run_like(PcClient *c, const char *pin_id, int unlike)
{
    char *err = NULL;

    if (!pc_set_pin_liked(c, pin_id, unlike ? false : true, &err)) {
        const char *msg = err ? err : pc_last_error(c);

        if (strstr(msg, "login required")) {
            fprintf(stderr, "dump: %s\n", msg);
            pc_free(err);
            return EXIT_LOGIN_REQUIRED;
        }
        fprintf(stderr, "dump: like failed: %s\n", msg);
        pc_free(err);
        return EXIT_SEARCH;
    }
    printf("%s pin %s\n", unlike ? "unliked" : "liked", pin_id);
    pc_free(err);
    return EXIT_OK;
}

static int run_create_board(PcClient *c, const char *name,
                            const char *privacy)
{
    PcBoard board;
    char   *err = NULL;

    memset(&board, 0, sizeof board);
    if (!pc_create_board(c, name, NULL, privacy, &board, &err)) {
        const char *msg = err ? err : pc_last_error(c);

        if (strstr(msg, "login required")) {
            fprintf(stderr, "dump: %s\n", msg);
            pc_free(err);
            return EXIT_LOGIN_REQUIRED;
        }
        fprintf(stderr, "dump: create board failed: %s\n", msg);
        pc_free(err);
        return EXIT_SEARCH;
    }
    printf("created board | %s | %s | %s | %s\n", board.id[0] ? board.id : "",
           board.name[0] ? board.name : name,
           board.privacy[0] ? board.privacy : (privacy ? privacy : "public"),
           board.url);
    pc_free(err);
    return EXIT_OK;
}

static bool all_digits(const char *s)
{
    if (!s || !*s)
        return false;
    while (*s) {
        if (*s < '0' || *s > '9')
            return false;
        s++;
    }
    return true;
}

static int even_dim(int v)
{
    if (v < 2)
        v = 2;
    if (v & 1)
        v--;
    return v;
}

static uint32_t frame_checksum(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t   i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Fit (sw,sh) into a max width/height box, keeping the aspect ratio. */
static void fit_box(int sw, int sh, int maxw, int maxh, int *ow, int *oh)
{
    if (sw <= 0 || sh <= 0) {
        *ow = maxw;
        *oh = maxh;
        return;
    }
    if (sw <= maxw && sh <= maxh) {
        *ow = sw;
        *oh = sh;
    } else if ((long long)maxw * sh <= (long long)maxh * sw) {
        *ow = maxw;
        *oh = (int)((long long)sh * maxw / sw);
    } else {
        *oh = maxh;
        *ow = (int)((long long)sw * maxh / sh);
    }
    *ow = even_dim(*ow);
    *oh = even_dim(*oh);
    if (*ow <= 0)
        *ow = 2;
    if (*oh <= 0)
        *oh = 2;
}

static int run_video(PcClient *c, const char *pin_id)
{
    PcPin    pin;
    char    *err = NULL;
    PcVideo *v;
    uint8_t *frame;
    int      ow = 236, oh = 236;

    memset(&pin, 0, sizeof pin);
    if (!pc_pin_video_info(c, pin_id, &pin, &err)) {
        fprintf(stderr, "dump: video info failed: %s\n",
                err ? err : pc_last_error(c));
        pc_free(err);
        return EXIT_SEARCH;
    }
    printf("video id=%s is_video=%d w=%d h=%d duration_ms=%d\n", pin.id,
           pin.is_video, pin.video_w, pin.video_h, pin.video_duration_ms);
    printf("url=%s\n", pin.video_url);
    if (!pin.is_video || !pin.video_url[0]) {
        fprintf(stderr, "dump: pin has no video\n");
        return EXIT_NO_PINS;
    }
    fit_box(pin.video_w, pin.video_h, 236, 320, &ow, &oh);
    v = pc_video_open(pin.video_url, ow, oh, 24, &err);
    if (!v) {
        fprintf(stderr, "dump: video decode open failed: %s\n",
                err ? err : "unknown");
        pc_free(err);
        return EXIT_SEARCH;
    }
    frame = malloc((size_t)ow * oh * 4u);
    if (!frame) {
        pc_video_close(v);
        return EXIT_SEARCH;
    }
    if (!pc_video_read_frame(v, frame)) {
        fprintf(stderr, "dump: first frame failed: %s\n", pc_video_error(v));
        free(frame);
        pc_video_close(v);
        return EXIT_SEARCH;
    }
    {
        int aw = 0, ah = 0;

        pc_video_size(v, &aw, &ah);
        printf("frame=%dx%d bytes=%zu\n", aw, ah,
               (size_t)aw * (size_t)ah * 4u);
    }
    free(frame);
    pc_video_close(v);
    pc_free(err);
    return EXIT_OK;
}

static int run_video_frames(PcClient *c, const char *arg, int count)
{
    char     url[PC_URL_LEN];
    char    *err = NULL;
    PcVideo *v;
    uint8_t *frame = NULL;
    int      ow = 320, oh = 240;
    int      fps;
    int      i, decoded = 0;

    if (count < 1)
        count = 5;
    if (count > 120)
        count = 120;
    url[0] = '\0';
    if (strncmp(arg, "http://", 7) == 0 || strncmp(arg, "https://", 8) == 0) {
        snprintf(url, sizeof url, "%s", arg);
    } else if (strchr(arg, '/') != NULL || access(arg, R_OK) == 0) {
        snprintf(url, sizeof url, "%s", arg);
    } else {
        PcPin pin;

        memset(&pin, 0, sizeof pin);
        if (!pc_pin_video_info(c, arg, &pin, &err)) {
            fprintf(stderr, "dump: video info failed: %s\n",
                    err ? err : pc_last_error(c));
            pc_free(err);
            return EXIT_SEARCH;
        }
        if (!pin.is_video || !pin.video_url[0]) {
            fprintf(stderr, "dump: pin %s has no video\n", arg);
            return EXIT_NO_PINS;
        }
        snprintf(url, sizeof url, "%s", pin.video_url);
        fit_box(pin.video_w, pin.video_h, 320, 320, &ow, &oh);
    }
    fps = (count > 1) ? 10 : 24;
    v = pc_video_open(url, ow, oh, fps, &err);
    if (!v) {
        fprintf(stderr, "dump: video open failed: %s\n",
                err ? err : "unknown");
        pc_free(err);
        return EXIT_SEARCH;
    }
    {
        int aw = 0, ah = 0;

        pc_video_size(v, &aw, &ah);
        ow = aw;
        oh = ah;
    }
    frame = malloc((size_t)ow * oh * 4u);
    if (!frame) {
        pc_video_close(v);
        return EXIT_SEARCH;
    }
    for (i = 0; i < count; i++) {
        uint32_t sum;

        if (!pc_video_read_frame(v, frame)) {
            fprintf(stderr, "dump: frame %d read failed: %s\n", i,
                    pc_video_error(v));
            break;
        }
        sum = frame_checksum(frame, (size_t)ow * oh * 4u);
        printf("frame=%d size=%dx%d bytes=%zu checksum=0x%08x\n", i, ow, oh,
               (size_t)ow * oh * 4u, sum);
        decoded++;
    }
    fprintf(stderr, "dump: decoded %d/%d frames from %s\n", decoded, count,
            url);
    free(frame);
    pc_video_close(v);
    pc_free(err);
    return (decoded == count) ? EXIT_OK : EXIT_SEARCH;
}

int main(int argc, char **argv)
{
    PcClient  *client;
    PcPinList  list;
    char      *err = NULL;
    char       query[1024];
    char       image_url[PC_URL_LEN];
    char       detail_id[PC_ID_LEN];
    char       cookies_path[1024];
    char       user_name[PC_SHORT_LEN];
    char       save_pin[PC_ID_LEN];
    char       save_board[PC_ID_LEN];
    int        limit = 0;
    int        mode_auth = 0;
    int        mode_homefeed = 0;
    int        mode_boards = 0;
    int        mode_user_pins = 0;
    int        mode_save = 0;
    int        mode_ads = 0;
    int        mode_related = 0;
    int        mode_comments = 0;
    int        mode_like = 0;
    int        mode_create_board = 0;
    int        mode_video = 0;
    int        mode_video_frames = 0;
    int        video_count = 5;
    int        unlike = 0;
    char       board_name[PC_BOARD_NAME_LEN];
    char       board_privacy[PC_PRIVACY_LEN];
    char       video_arg[PC_URL_LEN];
    char       closeup_id[PC_ID_LEN];
    int        block_ads_opt = -1;
    int        page = 1;
    int        i, rc;
    size_t     printed;
    double     t0;

    query[0]        = '\0';
    image_url[0]    = '\0';
    detail_id[0]    = '\0';
    cookies_path[0] = '\0';
    user_name[0]    = '\0';
    save_pin[0]     = '\0';
    save_board[0]   = '\0';
    closeup_id[0]   = '\0';
    board_name[0]   = '\0';
    board_privacy[0] = '\0';
    video_arg[0]    = '\0';

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
            if (limit < 0)
                limit = 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return EXIT_OK;
        } else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            snprintf(image_url, sizeof image_url, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--detail") == 0 && i + 1 < argc) {
            snprintf(detail_id, sizeof detail_id, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--cookies") == 0 && i + 1 < argc) {
            snprintf(cookies_path, sizeof cookies_path, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--auth") == 0) {
            mode_auth = 1;
        } else if (strcmp(argv[i], "--homefeed") == 0) {
            mode_homefeed = 1;
        } else if (strcmp(argv[i], "--boards") == 0) {
            mode_boards = 1;
        } else if (strcmp(argv[i], "--user-pins") == 0) {
            mode_user_pins = 1;
        } else if (strcmp(argv[i], "--ads") == 0) {
            mode_ads = 1;
        } else if (strcmp(argv[i], "--related") == 0 && i + 1 < argc) {
            snprintf(closeup_id, sizeof closeup_id, "%s", argv[++i]);
            mode_related = 1;
        } else if (strcmp(argv[i], "--comments") == 0 && i + 1 < argc) {
            snprintf(closeup_id, sizeof closeup_id, "%s", argv[++i]);
            mode_comments = 1;
        } else if (strcmp(argv[i], "--like") == 0 && i + 1 < argc) {
            snprintf(closeup_id, sizeof closeup_id, "%s", argv[++i]);
            mode_like = 1;
        } else if (strcmp(argv[i], "--unlike") == 0) {
            unlike = 1;
        } else if (strcmp(argv[i], "--create-board") == 0 && i + 1 < argc) {
            snprintf(board_name, sizeof board_name, "%s", argv[++i]);
            mode_create_board = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            snprintf(board_privacy, sizeof board_privacy, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) {
            snprintf(video_arg, sizeof video_arg, "%s", argv[++i]);
            mode_video = 1;
        } else if (strcmp(argv[i], "--video-frames") == 0 && i + 1 < argc) {
            snprintf(video_arg, sizeof video_arg, "%s", argv[++i]);
            mode_video_frames = 1;
            if (i + 1 < argc && all_digits(argv[i + 1]))
                video_count = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--block-ads") == 0 && i + 1 < argc) {
            block_ads_opt = atoi(argv[++i]) ? 1 : 0;
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            snprintf(user_name, sizeof user_name, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            page = atoi(argv[++i]);
            if (page < 1)
                page = 1;
        } else if (strcmp(argv[i], "--save") == 0 && i + 2 < argc) {
            snprintf(save_pin, sizeof save_pin, "%s", argv[++i]);
            snprintf(save_board, sizeof save_board, "%s", argv[++i]);
            mode_save = 1;
        } else {
            size_t used = strlen(query);
            size_t need = strlen(argv[i]);

            if (used + need + 2 >= sizeof query) {
                fprintf(stderr, "dump: query too long\n");
                return EXIT_SEARCH;
            }
            if (used)
                query[used++] = ' ';
            memcpy(query + used, argv[i], need + 1);
        }
    }

    if (!image_url[0] && !detail_id[0] && !query[0] && !mode_auth &&
        !mode_homefeed && !mode_boards && !mode_user_pins && !mode_save &&
        !mode_ads && !mode_related && !mode_comments && !mode_like &&
        !mode_create_board && !mode_video && !mode_video_frames) {
        usage(argv[0]);
        return EXIT_SEARCH;
    }

    /* WRITE INTERLOCK: no mutation may happen unless the operator explicitly
     * opts in.  Checked before any client/network is created. */
    if (mode_save || mode_like || mode_create_board) {
        const char *allow = getenv("PC_ALLOW_WRITE");

        if (!allow || strcmp(allow, "1") != 0) {
            fprintf(stderr,
                    "dump: refusing: set PC_ALLOW_WRITE=1 to permit a write\n");
            return EXIT_BOOTSTRAP;
        }
    }

    client = pc_client_new(NULL);
    if (!client) {
        fprintf(stderr, "dump: could not create client\n");
        return EXIT_BOOTSTRAP;
    }

    if (block_ads_opt >= 0)
        pc_set_block_ads(client, block_ads_opt != 0);

    /* Browser cookie login (optional).  Values are never printed. */
    if (cookies_path[0]) {
        if (!pc_cookie_load(client, cookies_path, &err)) {
            fprintf(stderr, "dump: cookie load failed: %s\n",
                    err ? err : pc_last_error(client));
            pc_free(err);
            pc_client_free(client);
            return EXIT_BOOTSTRAP;
        }
        fprintf(stderr, "dump: cookie jar loaded from %s (values not printed)\n",
                cookies_path);
    }

    if (image_url[0]) {
        rc = run_image(client, image_url);
        pc_client_free(client);
        return rc;
    }

    if (!pc_bootstrap(client)) {
        fprintf(stderr, "dump: bootstrap failed: %s\n", pc_last_error(client));
        pc_client_free(client);
        return EXIT_BOOTSTRAP;
    }

    if (mode_related) {
        rc = run_related(client, closeup_id, limit);
        pc_client_free(client);
        return rc;
    }

    if (mode_comments) {
        rc = run_comments(client, closeup_id, limit);
        pc_client_free(client);
        return rc;
    }

    if (mode_like) {
        rc = run_like(client, closeup_id, unlike);
        pc_client_free(client);
        return rc;
    }

    if (mode_ads) {
        rc = run_ads(client, query);
        pc_client_free(client);
        return rc;
    }

    if (mode_video) {
        rc = run_video(client, video_arg);
        pc_client_free(client);
        return rc;
    }

    if (mode_video_frames) {
        rc = run_video_frames(client, video_arg, video_count);
        pc_client_free(client);
        return rc;
    }

    if (mode_create_board) {
        rc = run_create_board(client, board_name,
                              board_privacy[0] ? board_privacy : NULL);
        pc_client_free(client);
        return rc;
    }

    if (mode_save) {
        rc = run_save(client, save_pin, save_board);
        pc_client_free(client);
        return rc;
    }

    if (mode_boards) {
        rc = run_boards(client, user_name[0] ? user_name : NULL);
        pc_client_free(client);
        return rc;
    }

    if (mode_user_pins) {
        rc = run_user_pins(client, user_name[0] ? user_name : NULL, page,
                           limit);
        pc_client_free(client);
        return rc;
    }

    if (mode_auth) {
        rc = run_auth(client);
        pc_client_free(client);
        return rc;
    }

    if (mode_homefeed) {
        rc = run_homefeed(client, limit);
        pc_client_free(client);
        return rc;
    }

    if (detail_id[0]) {
        rc = run_detail(client, detail_id);
        pc_client_free(client);
        return rc;
    }

    memset(&list, 0, sizeof list);
    t0 = now_ms();
    if (!pc_search(client, query, NULL, &list, &err)) {
        fprintf(stderr, "dump: search failed: %s\n",
                err ? err : pc_last_error(client));
        pc_free(err);
        pc_client_free(client);
        return EXIT_SEARCH;
    }
    fprintf(stderr, "dump: %zu pins in %.0fms (bookmark %zu bytes)\n",
            list.count, now_ms() - t0, strlen(list.bookmark));
    if (pc_get_block_ads(client))
        fprintf(stderr, "dump: block_ads=1 ads_blocked_total=%llu\n",
                (unsigned long long)pc_ads_blocked(client));

    printed = print_pins(&list, limit);
    rc = (list.count > 0) ? EXIT_OK : EXIT_NO_PINS;
    if (list.count == 0)
        fprintf(stderr, "dump: no pins parsed\n");
    (void)printed;

    pc_pinlist_free(&list);
    pc_client_free(client);
    return rc;
}
