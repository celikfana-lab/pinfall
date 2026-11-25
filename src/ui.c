/* ui.c - PinFall application: state machine, threaded networking, input,
 * masonry feed, detail overlay and headless snapshot mode.
 *
 * Threading model
 * ---------------
 *   main thread  : SDL events, layout, textures, drawing (the only thread that
 *                  ever touches SDL).
 *   net thread   : pc_bootstrap / pc_search / pc_pin_detail (single in-flight
 *                  command slot; latest request wins, stale results dropped).
 *   image pool   : 3 threads calling pc_image_get. Decoded RGBA is published
 *                  back to the main thread which uploads SDL textures.
 * Nothing on the main thread ever blocks on the network.
 */
#include "pc.h"

#include "font.h"
#include "layout.h"
#include "preview.h"
#include "render.h"
#include "settings.h"
#include "android.h"

#include "dsh_badge_png.h"
#include "stb_image.h"
#include "theme.h"

#include <SDL2/SDL.h>

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- constants */
#define APP_NAME "PinFall"
#define DEFAULT_QUERY "minimalist bedroom"
#define DEFAULT_WIN_W 1440
#define DEFAULT_WIN_H 900
#define MAX_PINS 1200       /* residency is bounded by texture eviction, so a
                             * large saved/home collection is fine to keep */
#define MAX_REQS 512
#define MAX_RESULTS 512
#define IMG_THREADS 3
#define SNAP_MIN_IMAGES 12
#define SNAP_DEFAULT_TIMEOUT_MS 20000
#define MAX_PAGES 48
#define NET_QUEUE 8   /* worker command ring: comments+related are queued together */

#define FONT_REG "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#define FONT_BOLD "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

/* Close-up ("Pin detail") metrics in logical px @1x, measured from the live
 * Pinterest page - see docs/UI_CLOSEUP.md. Multiply by App.rnd.scale. */
#define CU_RAIL_W         72.0f   /* fixed vertical icon rail */
#define CU_HEADER_H       80.0f   /* fixed search header */
#define CU_PAGE_PAD       16.0f   /* inset from rail / right edge */
#define CU_GAP            16.0f   /* masonry gutter (h+v) */
#define CU_SEARCH_RADIUS  12.0f
#define CU_CARD_RADIUS    16.0f
#define CU_CARD_BORDER     1.0f
#define CU_ACTION_H       48.0f
#define CU_ICON           24.0f
#define CU_SAVE_W         68.5f
#define CU_DROPDOWN_MAX  270.0f
#define CU_COMPOSER_H     47.0f
#define CU_COMPOSER_SLOT  59.0f
#define CU_AVATAR         24.0f
#define CU_COMMENT_AVATAR 32.0f
#define CU_MIN_CARD_F     0.55f   /* card min height = 55vh + 80 */
#define CU_MAX_MEDIA_F    0.90f   /* image max height = 90vh - 80 */
#define CU_IDEAS_RATIO    0.42f   /* ideas compact tile height / col width */
#define CU_TITLE_SIZE     24
#define CU_TITLE_LINE     26
#define CU_SECTION_SIZE   16
#define CU_SECTION_LINE   22
#define CU_CREATOR_SIZE   14
#define CU_BODY_SIZE      16
#define CU_COMMENT_LINE   20
#define CU_SMALL_SIZE     12
#define CU_MIN_REGION     90.0f   /* minimum comments viewport height */
#define CU_MAX_COLS       64

/* ------------------------------------------------------------- data types */
typedef enum {
    VIEW_BOOT,
    VIEW_LOADING,
    VIEW_FEED,
    VIEW_EMPTY, /* friendly empty state, e.g. "No saved pins yet" */
    VIEW_ERROR
} ViewState;

typedef enum {
    FEED_SEARCH,
    FEED_HOME,
    FEED_SAVED
} FeedMode;

typedef enum {
    NET_START,
    NET_HOME,
    NET_SAVED,
    NET_BOARDS,
    NET_SAVE,
    NET_SEARCH,
    NET_DETAIL,
    NET_COMMENTS,
    NET_RELATED,
    NET_LIKE,
    NET_ADD_COMMENT,
    NET_CREATE_BOARD
} NetCmdKind;

typedef enum {
    NRES_START,
    NRES_HOME,
    NRES_SAVED,
    NRES_BOARDS,
    NRES_SAVE,
    NRES_SEARCH,
    NRES_DETAIL,
    NRES_COMMENTS,
    NRES_RELATED,
    NRES_LIKE,
    NRES_ADD_COMMENT,
    NRES_CREATE_BOARD
} NetResKind;

typedef struct {
    NetCmdKind kind;
    uint64_t   seq;
    char       query[PC_TITLE_LEN];
    char       bookmark[PC_BOOKMARK_LEN];
    bool       append;
    bool       preview;      /* --auth-preview: skip probe/homefeed */
    bool       save_session;
    char       session_path[700];
    char       pin_id[PC_ID_LEN];
    char       board_id[PC_ID_LEN];
    char       board_name[PC_BOARD_NAME_LEN];
    bool       liked;
    bool       secret;
    char       text[PC_COMMENT_TEXT_LEN];
} NetCmd;

typedef struct {
    NetResKind kind;
    uint64_t   seq;
    bool       ok;
    bool       append;
    PcPinList  list;
    PcPin      pin;
    char       err[512];
    PcAuthState auth;
    char       username[PC_SHORT_LEN];
    bool       home;         /* list is the personal home feed */
    bool       home_failed;  /* pc_homefeed said login required */
    char       home_msg[512];
    bool       session_saved;
    bool       save_failed;
    char       save_msg[256];
    char       session_path[700];
    PcBoardList boards;
    char       board_name[PC_BOARD_NAME_LEN];
    PcCommentList comments;
    PcPinList     related;
    bool          liked;
    PcBoard       board;
    bool          has_board;
} NetRes;

#define MAX_REL 96
enum { IMG_GRID = 0, IMG_DETAIL = 1, IMG_RELATED = 2 };

typedef struct {
    int  target;
    int  gen;
    int  idx;
    char url[PC_URL_LEN];
    int  max_w;
} ImgReq;

typedef struct {
    int      target;
    int      gen;
    int      idx;
    uint8_t *pixels;
    int      w, h;
    bool     ok;
} ImgRes;

typedef struct {
    PcPin       pin;
    SDL_Texture *tex;
    int         tw, th;
    uint8_t     status; /* 0 pending, 1 in flight, 2 ready, 3 failed */
    float       hover;
    char        line[2][FONT_LINE_MAX];
    int         nlines;
} PinView;

typedef struct {
    SDL_Texture *tex;
    int          tw, th;
    uint8_t      status;
} DetailView;

/* Board-picker modal: async board list + async save, never blocks the frame. */
typedef struct {
    bool        open;
    bool        loading;      /* boards request in flight */
    bool        boards_ready;
    bool        saving;       /* pc_save_pin in flight */
    int         saving_row;   /* board row currently saving, -1 none */
    int         hover_row;
    int         selected_row; /* mobile sheet: highlighted board */
    int         pin_idx;      /* feed pin being saved */
    char        pin_id[PC_ID_LEN];
    char        saving_board_id[PC_ID_LEN];
    char        error[512];   /* shown inside the picker */
    bool        creating;     /* pc_create_board in flight */
    bool        create_focus;
    char        new_name[PC_BOARD_NAME_LEN];
    int         new_name_len;
    bool        new_secret;
    char        create_error[256];
    char        created_name[PC_BOARD_NAME_LEN];
    SDL_FRect   create_box, create_btn, privacy_btn;
    PcBoardList boards;
    SDL_FRect   panel;
    SDL_FRect   close;
    float       list_y;     /* top of the scrollable board list */
    float       list_h;     /* visible list height */
    float       row_h;
    float       scroll;
    float       max_scroll;
    int         rows_visible;
} Picker;

struct App;

typedef struct {
    SDL_Window   *win;
    Rnd           rnd;
    Fonts        *fonts;
    PcClient     *client;

    ViewState     state;
    char          query[PC_TITLE_LEN];
    char          error[512];
    char          host[PC_SHORT_LEN];
    bool          booted;
    bool          running;

    /* auth / session */
    PcAuthState   auth;
    char          username[PC_SHORT_LEN];
    FeedMode      feed_mode;     /* what the masonry is currently showing */
    bool          auth_preview;  /* rendering-only signed-in preview */
    bool          query_explicit;/* --query given: even signed-in, open SEARCH */
    bool          pending_query_search; /* bootstrap first, then NET_SEARCH */
    bool          save_session;
    bool          no_writes;     /* --no-writes: hide all save affordances */
    char          session_path[700];
    SDL_Rect      home_btn;      /* Home tab hit rect (w==0 -> guest) */
    SDL_Rect      saved_btn;     /* Saved tab hit rect (w==0 -> guest) */

    /* board picker modal */
    Picker        picker;
    char          saved_board_name[PC_BOARD_NAME_LEN];
    SDL_Rect      card_save_rect; /* hover Save pill hit rect (0 w == none) */
    SDL_Rect      d_save;         /* detail Save button hit rect */

    PinView       pins[MAX_PINS];
    size_t        npins;
    int           gen;
    char          bookmark[PC_BOOKMARK_LEN];
    int           pages;
    bool          loading_more;
    Uint32        load_more_retry_at; /* backoff after a failed page */

    Layout        layout;
    float         scroll, scroll_target;
    /* Android inertial scrolling */
    float         fling_v;
    bool          fling_active;
    int           fling_axis;    /* 0 feed, 1 detail/page, 2 picker */
    float         drag_v;
    Uint32        drag_last_ms;
    bool          suppress_click;
    bool          drag_moved;
    bool          dragging_scrollbar;
    float         drag_y, drag_scroll;

    /* search field */
    char          input[PC_TITLE_LEN];
    int           input_len; /* bytes */
    int           caret;     /* code point index */
    bool          search_focus;
    Uint32        caret_last;
    bool          caret_on;

    /* mouse */
    int           mx, my;
    bool          mouse_down;
    int           hover;
    bool          hover_in_img;
    SDL_Rect      search_clear;
    SDL_Rect      banner_rect;

    /* detail overlay (Pinterest-parity close-up) */
    bool          show_detail;
    int           detail_idx;        /* feed index, -1 when external related pin */
    int           detail_serial;
    DetailView    detail;
    PcPin         detail_pin;        /* pin currently shown (copy) */
    bool          detail_external;
    int           detail_feed_idx;
    char          d_title[3][FONT_LINE_MAX];
    int           d_title_n;
    char          d_desc[24][FONT_LINE_MAX];
    int           d_desc_n;
    SDL_FRect     d_panel, d_img, d_close, d_btn;
    float         d_right_x, d_right_w;
    int           d_text_y;

    /* close-up social: comments / related / reactions (v6) */
    PcCommentList comments;
    bool          comments_loading, comments_ready, comments_unavailable;
    char          comments_err[256];
    uint64_t      comments_seq;
    PcPinList     related;
    bool          related_loading, related_ready, related_unavailable;
    char          related_err[256];
    uint64_t      related_seq;
    SDL_Texture  *rel_tex[MAX_REL];
    int           rel_tw[MAX_REL], rel_th[MAX_REL];
    uint8_t       rel_status[MAX_REL];
    int           rel_n;
    int           rel_img_serial;
    bool          detail_liked;
    int           detail_like_count;
    bool          like_inflight;
    bool          like_pending_value;
    bool          like_revert_liked;
    int           like_revert_count;
    uint64_t      like_seq;
    uint64_t      create_seq;
    char          comment_draft[PC_COMMENT_TEXT_LEN];
    int           comment_draft_len;
    bool          composer_focus, comment_posting;
    char          pending_comment[PC_COMMENT_TEXT_LEN];
    char          comment_post_err[256];
    uint64_t      comment_seq;
    int           detail_menu;       /* 0 none, 1 share menu, 2 more menu */
    bool          desc_expanded;
    float         detail_scroll, detail_scroll_target;
    float         detail_content_h, detail_view_h;
    bool          detail_drag_sb;
    float         detail_drag_y, detail_drag_scroll;
    SDL_FRect     d_like_btn, d_comment_btn, d_share_btn, d_more_btn;
    SDL_FRect     d_desc_more, d_viewmore_btn, d_composer, d_post_btn;
    SDL_FRect     d_rel_tiles[MAX_REL];
    int           d_rel_cols;
    SDL_FRect     d_menu;
    SDL_FRect     d_menu_items[3];
    int           d_menu_n;
    SDL_FRect     d_sb_track, d_sb_thumb;
    float         d_content_y, d_comments_y, d_related_y, d_composer_y;
    /* full-screen close-up (v6.1) */
    SDL_FRect     d_topbar, d_back_btn;
    float         d_left_x, d_left_w;
    float         left_scroll, left_scroll_target;
    float         left_content_h, left_view_h;
    float         d_img_h, d_rel_y;
    bool          left_drag_sb, right_drag_sb;
    float         left_drag_y, left_drag_scroll;
    SDL_FRect     d_left_sb_track, d_left_sb_thumb;
    SDL_FRect     d_right_sb_track, d_right_sb_thumb;
    SDL_FRect     d_rel_viewmore;
    bool          rel_loading_more;
    uint64_t      rel_more_seq;
    bool          snap_empty_comments;

    /* close-up video playback (v8) */
    char          video_file_override[700]; /* --video-file test hook */
    pthread_t     vid_thr;
    pthread_mutex_t vid_mtx;
    pthread_cond_t  vid_cv;
    bool          vid_thread_started;
    bool          vid_quit;
    bool          vid_has_cmd;
    bool          vid_stop;
    uint64_t      vid_gen;
    char          vid_src[700];
    int           vid_out_w, vid_out_h, vid_fps;
    uint8_t      *vid_pixels;
    int           vid_fw, vid_fh;
    uint64_t      vid_frame_seq;
    bool          vid_decode_failed;
    bool          vid_available;
    bool          video_active;
    bool          video_playing;
    bool          video_paused;
    bool          video_failed;
    char          video_note[128];
    SDL_Texture  *video_tex;
    int           video_w, video_h;
    uint64_t      video_frames;
    uint64_t      video_uploaded_seq;
    uint64_t      video_gen;
    char          video_src[700];
    int           video_target_w, video_target_h;
    SDL_FRect     d_video_btn;
    char          snap_video_file[700];
    int           snap_video_frame;
    bool          snap_video_pause;
    int           detail_focus_side; /* 0 = comments pane, 1 = page */

    /* ---- close-up page (UI_CLOSEUP.md, v7) ----------------------------
     * Everything above is shared with the feed; these fields hold the
     * Pinterest-parity close-up shell: fixed rail+header, close-up card,
     * page masonry of recommendations and the "Ideas you might love" unit.
     * Geometry is split in two coordinate spaces:
     *   - fixed chrome (rail/header/back button): screen px, unscrolled;
     *   - page content (card, info pane, recs, ideas): content px, y=0 at
     *     the header bottom, scrolled by left_scroll.
     * app_draw_detail()/hit tests convert with cu_screen(). */
    SDL_FRect d_header, d_rail, d_search_rect;
    SDL_FRect d_nav_btns[8];        /* rail: logo,home,boards,create,bell,chat,tune,settings */
    SDL_FRect d_header_avatar, d_header_chev;
    SDL_FRect d_card, d_media, d_info;
    SDL_FRect d_dropdown, d_save_btn, d_like_count, d_creator;
    SDL_FRect d_ideas, d_idea_rows[2], d_overlay_btn[2];
    float d_rail_w, d_card_w, d_card_h, d_media_w, d_media_h;
    float d_content_x, d_content_w, d_act_y, d_divider_y, d_title_y;
    float d_desc_y, d_creator_y, d_comments_top, d_comments_bot;
    int   d_cols, d_span, d_rec_n, d_hover_rec;
    float d_col_w, d_idea_row_h;
    char  d_idea_labels[2][PC_SHORT_LEN];
    int   d_idea_pin[2];

    /* image cache / texture residency settings + live stats */
    PfSettings    settings;
    char          settings_path[700];
    PcImgStats    img_stats;
    int           tex_count;     /* grid textures currently uploaded */
    size_t        tex_bytes;     /* approximate w*h*4 of those textures */
    int           tex_peak;
    bool          show_stats;    /* F3 overlay visible */
    bool          show_settings; /* settings modal open */
    int           settings_row;  /* keyboard focus row */
    SDL_Rect      gear_btn;      /* header gear hit rect */
    SDL_FRect     settings_panel;/* settings modal panel rect */
    SDL_FRect     settings_close;
    SDL_FRect     settings_rows[6];
    SDL_FRect     settings_opt[6][4];
    int           settings_opt_n[6];
    SDL_FRect     settings_toggle;      /* stats overlay (row 4) */
    SDL_FRect     settings_ads_toggle;  /* block promoted pins (row 3) */
    SDL_FRect     settings_autoplay_toggle; /* autoplay videos (row 5) */
    SDL_FRect     settings_theme_rect;      /* theme cycle row */
    bool          stats_arg;     /* --stats: overlay + machine line at exit */
    bool          snap_open_settings;
    bool          snap_open_about;
    bool          snap_create_board;
    int           snap_hover;
    bool          about_selected;
    SDL_Texture  *badge_tex;
    int           badge_w, badge_h;
    SDL_FRect     settings_about_badge;
    SDL_FRect     settings_about_url;
    float         fps;
    Uint32        fps_window_start;
    int           fps_frames;

    /* toast */
    char          toast[128];
    Uint32        toast_until;

    /* scaled metrics */
    struct {
        float header_h, footer_h, pad, gap, card_radius, search_h,
            search_radius, panel_radius, title_block_h, caption_gap,
            body_line_f;
        int   title_size, title_line, meta_size, logo_size, search_size,
            h1_size, h2_size, body_size, body_line, small_size,
            detail_meta_size;
        float target_col_w;
        int   max_cols;
    } M;

    /* networking */
    pthread_t       net_thr;
    pthread_mutex_t net_mtx;
    pthread_cond_t  net_cv;
    bool            net_quit, net_pending, net_busy;
    /* Ring buffer: the detail view queues comments + related back-to-back;
     * a single slot silently dropped the first command. */
    NetCmd          net_q[NET_QUEUE];
    int             net_head, net_tail;
    bool            net_res_ready;
    NetRes          net_res;
    uint64_t        net_search_seq;
    uint64_t        net_detail_seq;

    uint64_t        net_boards_seq;
    uint64_t        net_save_seq;

    /* image pool */
    pthread_t       img_thr[IMG_THREADS];
    int             n_img_thr;
    pthread_mutex_t img_mtx;
    pthread_cond_t  img_cv;
    bool            img_quit;
    ImgReq          reqs[MAX_REQS];
    int             req_head, req_count;
    ImgRes          results[MAX_RESULTS];
    int             nresults;

    /* misc */
    Uint32          last_tick;
    float           dt;
    float           anim_t;
    SDL_Cursor     *cur_arrow, *cur_hand, *cur_ibeam;
    int             cur_kind;

    /* snapshot */
    bool            snapshot;
    char            snap_path[512];
    int             snap_timeout;
    Uint32          snap_start;
    Uint32          snap_settle;
    bool            snap_saved;
    float           snap_scroll;
    int             snap_detail;
    int             snap_detail_tab;   /* 0 default, 1 comments, 2 related */
    bool            snap_like;
    char            snap_comment_draft[PC_COMMENT_TEXT_LEN];
    int             snap_tab;        /* 0 none, 1 home, 2 saved */
    bool            snap_open_picker;

    /* mobile platform state (no-ops on desktop) */
    bool            ime_active;
    bool            android_backgrounded;
    bool            mobile_search;   /* Search tab / mobile search header */
    SDL_FRect       mob_navbar, mob_nav[4];
    SDL_FRect       mob_logo, mob_top_plus, mob_top_msg, mob_top_avatar;
    SDL_FRect       mob_chip_active;
    SDL_FRect       mob_search_field;
    SDL_FRect       mob_back, mob_like, mob_comment, mob_send, mob_more,
                    mob_save, mob_visit, mob_backtotop;
    SDL_FRect       mob_sheet, mob_sheet_close, mob_sheet_grab,
                    mob_sheet_composer, mob_sheet_send;
    SDL_FRect       mob_save_rows[8], mob_save_confirm, mob_save_create;
    SDL_FRect       mob_seemore;
    float           mob_sheet_scroll;
    int             mob_save_n;
    float           mob_media_h;
    float           d_pinner_y;
    int             d_desc_shown;
    bool            comments_sheet;
#ifdef PC_ANDROID
    PfAndroidTouch  touch;
    char            device_cookie_path[700];
    SDL_FRect       settings_cookie_btn;
#endif
} App;

/* --------------------------------------------------------------- forward */
static void app_metrics(App *a);
static void app_layout(App *a);
static void app_draw(App *a);
static void app_update(App *a);
#ifdef PC_ANDROID
static void app_draw_mobile(App *a);
static bool app_mobile_click(App *a, float x, float y);
static void app_mobile_measure(App *a);
#endif
static void app_update_hover(App *a);
static void app_consume_net(App *a);
static void app_consume_images(App *a);
static void app_request_images(App *a);
static void app_search(App *a, const char *query);
static void app_retry(App *a);
static void app_go_home(App *a);
static void app_go_saved(App *a);
static void app_open_picker(App *a, int pin_idx);
static void app_close_picker(App *a);
static void app_picker_choose(App *a, int row);
static void app_picker_create(App *a);
static bool picker_row_rect(App *a, int row, SDL_FRect *out);
static bool writes_enabled(App *a);
static void app_apply_settings(App *a);
static void app_load_badge(App *a);
static void tex_clear(App *a, PinView *pv);
static void tex_uploaded(App *a, PinView *pv, int w, int h);
static void app_texture_band(App *a, float *top, float *bot);
static void app_evict_textures(App *a);
static void app_update_stats(App *a);
static void app_print_stats_line(App *a);
static void app_open_settings(App *a);
static void app_apply_theme(App *a);
static void app_close_settings(App *a);
static void app_settings_geom(App *a);
static void app_draw_settings(App *a);
static void app_draw_stats(App *a);
static void draw_gear_icon(App *a, float cx, float cy, float r, SDL_Color c);
static void app_install_pins(App *a, PcPinList *list, bool append);
static void app_clear_feed(App *a);
static void app_push_net(App *a, const NetCmd *cmd);
static void app_open_detail(App *a, int idx);
static void app_close_detail(App *a);
static void app_detail_geom(App *a);
static void app_detail_measure(App *a);
static SDL_FRect cu_screen(App *a, SDL_FRect r);
static SDL_FRect cu_search_lens(App *a);
static SDL_FRect cu_search_mic(App *a);
static SDL_FRect cu_search_clear(App *a);
static void app_draw_cu_header(App *a);
static float app_detail_image_h(App *a);
static void app_detail_wrap(App *a);
static void app_detail_load_social(App *a);
static void app_detail_clear_social(App *a);
static void app_request_related_images(App *a);
static void app_detail_like_toggle(App *a);
static void app_detail_post_comment(App *a);
static bool app_detail_canonical_id(const char *id);
static int app_detail_comment_count(App *a);
static float app_detail_image_h(App *a);
static void app_open_related_pin(App *a, const PcPin *pin);
static void app_video_begin(App *a);
static void app_video_stop(App *a);
static void app_video_toggle_play(App *a);
static void app_update_video(App *a);
static void *vid_thread_fn(void *arg);
static void app_handle_event(App *a, const SDL_Event *e);
static void app_update_cursor(App *a);
static int app_count_ready_images(App *a);
static SDL_FRect search_rect(App *a);

/* ---------------------------------------------------------------- helpers */
static SDL_Color col_a(SDL_Color c, Uint8 alpha)
{
    c.a = alpha;
    return c;
}

static SDL_Color col_lerp(SDL_Color a, SDL_Color b, float t)
{
    SDL_Color c;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    c.r = (Uint8)((float)a.r + ((float)b.r - (float)a.r) * t);
    c.g = (Uint8)((float)a.g + ((float)b.g - (float)a.g) * t);
    c.b = (Uint8)((float)a.b + ((float)b.b - (float)a.b) * t);
    c.a = (Uint8)((float)a.a + ((float)b.a - (float)a.a) * t);
    return c;
}

static bool dbg_events(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("PINFALL_DEBUG_EVENTS") ? 1 : 0;
    return enabled != 0;
}

static bool point_in(SDL_Rect r, float x, float y)
{
    return x >= (float)r.x && x <= (float)(r.x + r.w) && y >= (float)r.y &&
           y <= (float)(r.y + r.h);
}

static bool point_in_f(SDL_FRect r, float x, float y)
{
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

static SDL_FRect fr_from(SDL_Rect r)
{
    SDL_FRect f = {(float)r.x, (float)r.y, (float)r.w, (float)r.h};
    return f;
}

static float content_top(App *a)
{
#ifdef PC_ANDROID
    (void)a;
    return 85.0f; /* native top bar (37) + category chips (48) */
#else
    return a->M.header_h;
#endif
}

static float content_bottom(App *a)
{
#ifdef PC_ANDROID
    /* The native bottom nav floats above the feed, so the grid runs full
     * height and simply scrolls underneath it. */
    return (float)a->rnd.out_h;
#else
    return (float)a->rnd.out_h - a->M.footer_h;
#endif
}

static float content_h(App *a)
{
    return content_bottom(a) - content_top(a);
}

static float max_scroll(App *a)
{
    float ms = a->layout.total_h - content_bottom(a);
    return ms > 0.0f ? ms : 0.0f;
}

static const char *host_label(void)
{
    const char *env = getenv("PC_HOST");
    if (env && *env) {
        if (strncmp(env, "https://", 8) == 0)
            env += 8;
        else if (strncmp(env, "http://", 7) == 0)
            env += 7;
        return env;
    }
    return "nl.pinterest.com";
}

/* $XDG_CONFIG_HOME/pinfall/session.txt, falling back to ~/.config/... */
static void default_session_path(char *buf, size_t n)
{
    buf[0] = '\0';
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(buf, n, "%s/pinfall/session.txt", xdg);
    else if (home && *home)
        snprintf(buf, n, "%s/.config/pinfall/session.txt", home);
}

static bool file_exists(const char *path)
{
    if (!path || !*path)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static bool writes_enabled(App *a)
{
    return a->auth == PC_AUTH_LOGGED_IN && !a->no_writes;
}

/* Push the current settings into the image cache + ad filter + UI toggles. */
static void app_apply_settings(App *a)
{
    size_t budget = (size_t)a->settings.budget_mb * 1024u * 1024u;
    if (a->client) {
        pc_image_set_cache(a->client, a->settings.cache_mode, budget);
        pc_set_block_ads(a->client, a->settings.block_ads);
    }
    a->show_stats = a->settings.show_stats;
}

static bool mkdir_p(const char *path)
{
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return false;
    return true;
}

/* Detect a "login required" / 401 style error without retrying in a loop. */
static bool err_login_required(const char *msg)
{
    if (!msg || !*msg)
        return false;
    char buf[512];
    size_t n = strlen(msg);
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    for (size_t i = 0; i < n; i++) {
        char c = msg[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[n] = '\0';
    return strstr(buf, "login") != NULL || strstr(buf, "401") != NULL ||
           strstr(buf, "unauthor") != NULL;
}

static void draw_text_center(App *a, const char *text, int size, bool bold,
                             float cx, float y, SDL_Color c)
{
    int w = 0;
    fonts_measure(a->fonts, text, size, bold, &w, NULL);
    fonts_draw(a->rnd.r, a->fonts, text, cx - (float)w * 0.5f, y, size, bold,
               c);
}

static void draw_text_right(App *a, const char *text, int size, bool bold,
                            float right_x, float y, SDL_Color c)
{
    int w = 0;
    fonts_measure(a->fonts, text, size, bold, &w, NULL);
    fonts_draw(a->rnd.r, a->fonts, text, right_x - (float)w, y, size, bold, c);
}

static void draw_search_icon(App *a, float cx, float cy, float r,
                             SDL_Color c)
{
    SDL_FRect cbox = {cx - r, cy - r, 2.0f * r, 2.0f * r};
    rnd_round_outline(&a->rnd, cbox.x, cbox.y, cbox.w, cbox.h, r,
                      fmaxf(1.0f, 1.6f * a->rnd.scale), c);
    float hx = cx + r * 0.72f, hy = cy + r * 0.72f;
    rnd_line(&a->rnd, hx, hy, hx + r * 0.7f, hy + r * 0.7f,
             fmaxf(1.0f, 1.8f * a->rnd.scale), c);
}

static void draw_close_icon(App *a, SDL_FRect box, SDL_Color c)
{
    float m = box.w * 0.32f;
    float t = fmaxf(1.0f, 1.8f * a->rnd.scale);
    rnd_line(&a->rnd, box.x + m, box.y + m, box.x + box.w - m,
             box.y + box.h - m, t, c);
    rnd_line(&a->rnd, box.x + box.w - m, box.y + m, box.x + m,
             box.y + box.h - m, t, c);
}

/* --------------------------------------------------------------- metrics */
static void app_metrics(App *a)
{
    float s = a->rnd.scale;
#ifdef PC_ANDROID
    /* Native Pinterest Android tokens, in dp (ui_scale = display density) */
    a->M.header_h = 85.0f * s;      /* top app bar 37 + category chips 48 */
    a->M.footer_h = 0.0f;           /* floating nav overlays the feed */
    a->M.pad = 3.0f * s;
    a->M.gap = 3.0f * s;
    a->M.card_radius = 16.0f * s;
    a->M.search_h = 36.6f * s;
    a->M.search_radius = 18.3f * s;
    a->M.panel_radius = 16.0f * s;
    a->M.target_col_w = 140.0f * s;
    a->M.max_cols = 2;
    a->M.title_size = 12;
    a->M.title_line = 16;
    a->M.meta_size = 12;
    a->M.logo_size = 20;
    a->M.search_size = 16;
    a->M.h1_size = 18;
    a->M.h2_size = 16;
    a->M.body_size = 16;
    a->M.body_line = 22;
    a->M.small_size = 12;
    a->M.detail_meta_size = 12;
    a->M.caption_gap = 4.0f;
    a->M.title_block_h = 4.0f + 2.0f * 16.0f;
    return;
#else
    a->M.header_h = THEME.header_h * s;
    a->M.footer_h = THEME.footer_h * s;
    a->M.pad = THEME.page_pad * s;
    a->M.gap = THEME.card_gap * s;
    a->M.card_radius = THEME.card_radius * s;
    a->M.search_h = THEME.search_h * s;
    a->M.search_radius = THEME.search_radius * s;
    a->M.panel_radius = THEME.panel_radius * s;
    a->M.target_col_w = THEME.target_col_w * s;
    a->M.max_cols = THEME.max_cols;

    a->M.title_size = (int)lroundf(THEME.title_size * s);
    a->M.title_line = (int)lroundf(THEME.title_line * s);
    a->M.meta_size = (int)lroundf(THEME.meta_size * s);
    a->M.logo_size = (int)lroundf(THEME.logo_size * s);
    a->M.search_size = (int)lroundf(THEME.search_size * s);
    a->M.h1_size = (int)lroundf(THEME.h1_size * s);
    a->M.h2_size = (int)lroundf(THEME.h2_size * s);
    a->M.body_size = (int)lroundf(THEME.body_size * s);
    a->M.body_line = (int)lroundf(THEME.body_line * s);
    a->M.small_size = (int)lroundf(THEME.small_size * s);
    a->M.detail_meta_size = (int)lroundf(THEME.detail_meta_size * s);
    a->M.caption_gap = THEME.caption_gap * s;
    a->M.title_block_h =
        a->M.caption_gap + (float)(2 * a->M.title_line);

    int min_size = 8;
    if (a->M.title_size < min_size)
        a->M.title_size = min_size;
    if (a->M.meta_size < min_size)
        a->M.meta_size = min_size;
    if (a->M.small_size < min_size)
        a->M.small_size = min_size;
    if (a->M.body_size < min_size)
        a->M.body_size = min_size;
    if (a->M.h1_size < min_size)
        a->M.h1_size = min_size;
#endif
}

/* ---------------------------------------------------------------- layout */
static void app_layout(App *a)
{
    int ws[MAX_PINS] = {0}, hs[MAX_PINS] = {0};
    size_t n = a->npins;
    if (n > MAX_PINS)
        n = MAX_PINS;
    for (size_t i = 0; i < n; i++) {
        ws[i] = a->pins[i].pin.img_w;
        hs[i] = a->pins[i].pin.img_h;
    }

#ifdef PC_ANDROID
    float top_inset = content_top(a);
#else
    float top_inset = content_top(a) + 16.0f * a->rnd.scale;
#endif
    layout_compute(&a->layout, ws, hs, n, (float)a->rnd.out_w, a->M.pad,
                   a->M.gap, a->M.target_col_w, a->M.max_cols, top_inset,
                   a->M.title_block_h);

    for (size_t i = 0; i < n && i < a->layout.count; i++) {
        PinView *pv = &a->pins[i];
        const LayoutItem *it = &a->layout.items[i];
        const char *title = pv->pin.title;
        if (!title[0])
            title = pv->pin.board_name[0]
                        ? pv->pin.board_name
                        : (pv->pin.domain[0] ? pv->pin.domain : "Untitled");
        pv->nlines = fonts_wrap(a->fonts, title, a->M.title_size, true,
                                it->title_w, pv->line, 2);
    }
}

/* -------------------------------------------------------------- feed load */
static void app_clear_feed(App *a)
{
    for (size_t i = 0; i < a->npins; i++)
        tex_clear(a, &a->pins[i]);
    a->tex_count = 0;
    a->tex_bytes = 0;
    memset(a->pins, 0, sizeof a->pins);
    a->npins = 0;
    a->pages = 0;
    a->bookmark[0] = '\0';
    a->hover = -1;
    a->scroll = 0.0f;
    a->scroll_target = 0.0f;
    a->loading_more = false;
    a->error[0] = '\0';

    pthread_mutex_lock(&a->img_mtx);
    a->gen++;
    a->req_head = 0;
    a->req_count = 0;
    pthread_mutex_unlock(&a->img_mtx);
}

static void app_install_pins(App *a, PcPinList *list, bool append)
{
    a->load_more_retry_at = 0;
    if (!append) {
        app_clear_feed(a);
    } else {
        a->loading_more = false;
    }
    a->error[0] = '\0';

    size_t added = 0;
    for (size_t i = 0; i < list->count && a->npins < MAX_PINS; i++) {
        PinView *pv = &a->pins[a->npins];
        memset(pv, 0, sizeof *pv);
        pv->pin = list->items[i];
        pv->status = 0;
        a->npins++;
        added++;
    }

    if (a->auth_preview) {
        /* mark a few preview pins as videos so grid badges/playback are
         * verifiable offscreen without depending on the live feed */
        for (size_t i = 0; i < a->npins; i++) {
            if (i % 3 == 2) {
                a->pins[i].pin.is_video = true;
                if (a->pins[i].pin.video_duration_ms <= 0)
                    a->pins[i].pin.video_duration_ms =
                        9000 + (int)(i % 3) * 3000;
                if (a->pins[i].pin.video_w <= 0) {
                    a->pins[i].pin.video_w = 720;
                    a->pins[i].pin.video_h = 1280;
                }
            }
        }
    }

    snprintf(a->bookmark, sizeof a->bookmark, "%s", list->bookmark);
    a->pages++;

    if (a->npins == 0) {
        a->state = VIEW_ERROR;
        snprintf(a->error, sizeof a->error, "No pins found for \"%s\"",
                 a->query);
    } else {
        a->state = VIEW_FEED;
        if (
            /* keep the user anchored when appending a page */
            !append) {
            a->scroll = 0.0f;
            a->scroll_target = 0.0f;
        }
    }
    (void)added;

    app_layout(a);
    app_request_images(a);
}


/* Pinterest occasionally double-escapes comment text ("\\u200d", "&amp;"), so
 * unescape common JSON escapes and HTML entities before rendering. Handles
 * \uXXXX including surrogate pairs and numeric entities. */
static int app_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int app_utf8_put(char *dst, size_t di, size_t dstsz, unsigned cp)
{
    if (cp < 0x80) {
        if (di + 1 >= dstsz) return -1;
        dst[di++] = (char)cp;
    } else if (cp < 0x800) {
        if (di + 2 >= dstsz) return -1;
        dst[di++] = (char)(0xC0 | (cp >> 6));
        dst[di++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (di + 3 >= dstsz) return -1;
        dst[di++] = (char)(0xE0 | (cp >> 12));
        dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[di++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (di + 4 >= dstsz) return -1;
        dst[di++] = (char)(0xF0 | (cp >> 18));
        dst[di++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[di++] = (char)(0x80 | (cp & 0x3F));
    }
    return (int)di;
}

static void app_unescape_text(char *dst, size_t dstsz, const char *src)
{
    size_t di = 0;
    if (!dst || dstsz == 0)
        return;
    while (src && *src && di + 5 < dstsz) {
        if (*src == '\\' && src[1] == 'u' && app_hexval(src[2]) >= 0 &&
            app_hexval(src[3]) >= 0 && app_hexval(src[4]) >= 0 &&
            app_hexval(src[5]) >= 0) {
            unsigned cp = (unsigned)(app_hexval(src[2]) << 12 |
                                     app_hexval(src[3]) << 8 |
                                     app_hexval(src[4]) << 4 |
                                     app_hexval(src[5]));
            const char *adv = src + 6;
            if (cp >= 0xD800 && cp <= 0xDBFF && adv[0] == '\\' &&
                adv[1] == 'u' && app_hexval(adv[2]) >= 0 &&
                app_hexval(adv[3]) >= 0 && app_hexval(adv[4]) >= 0 &&
                app_hexval(adv[5]) >= 0) {
                unsigned lo = (unsigned)(app_hexval(adv[2]) << 12 |
                                         app_hexval(adv[3]) << 8 |
                                         app_hexval(adv[4]) << 4 |
                                         app_hexval(adv[5]));
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    adv += 6;
                }
            }
            int ni = app_utf8_put(dst, di, dstsz, cp);
            if (ni < 0)
                break;
            di = (size_t)ni;
            src = adv;
            continue;
        }
        if (*src == '\\' && src[1]) {
            char c = 0;
            switch (src[1]) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            }
            if (c) {
                dst[di++] = c;
                src += 2;
                continue;
            }
        }
        if (*src == '&') {
            static const struct { const char *ent; char ch; } ents[] = {
                {"&amp;", '&'}, {"&quot;", '"'}, {"&#39;", '\''},
                {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'},
                {"&nbsp;", ' '},
            };
            bool done = false;
            for (size_t k = 0; k < sizeof ents / sizeof ents[0]; k++) {
                size_t l = strlen(ents[k].ent);
                if (strncmp(src, ents[k].ent, l) == 0) {
                    dst[di++] = ents[k].ch;
                    src += l;
                    done = true;
                    break;
                }
            }
            if (done)
                continue;
            if (src[1] == '#') {
                unsigned cp = 0;
                const char *q = src + 2;
                int base = 10;
                if (*q == 'x' || *q == 'X') {
                    base = 16;
                    q++;
                }
                const char *q0 = q;
                while (*q) {
                    int v = base == 16
                                ? app_hexval(*q)
                                : (*q >= '0' && *q <= '9' ? *q - '0' : -1);
                    if (v < 0)
                        break;
                    cp = cp * (unsigned)base + (unsigned)v;
                    q++;
                }
                if (q > q0 && *q == ';') {
                    int ni = app_utf8_put(dst, di, dstsz, cp);
                    if (ni < 0)
                        break;
                    di = (size_t)ni;
                    src = q + 1;
                    continue;
                }
            }
        }
        dst[di++] = *src++;
    }
    dst[di] = '\0';
}

static void app_unescape_comment(PcComment *c)
{
    char tmp[PC_COMMENT_TEXT_LEN * 4 + 8];
    size_t n;

    app_unescape_text(tmp, sizeof tmp, c->text);
    n = strlen(tmp);
    if (n >= sizeof c->text)
        n = sizeof c->text - 1;
    memcpy(c->text, tmp, n);
    c->text[n] = '\0';

    app_unescape_text(tmp, sizeof tmp, c->user_name);
    n = strlen(tmp);
    if (n >= sizeof c->user_name)
        n = sizeof c->user_name - 1;
    memcpy(c->user_name, tmp, n);
    c->user_name[n] = '\0';
}

static void app_consume_net(App *a)
{
    NetRes r;
    bool has = false;

    pthread_mutex_lock(&a->net_mtx);
    if (a->net_res_ready) {
        r = a->net_res;
        memset(&a->net_res, 0, sizeof a->net_res);
        a->net_res_ready = false;
        has = true;
    }
    pthread_mutex_unlock(&a->net_mtx);
    if (!has)
        return;

    switch (r.kind) {
    case NRES_START: {
        bool stale = (r.seq != a->net_search_seq);
        if (stale) {
            pc_pinlist_free(&r.list);
            return;
        }
        a->auth = r.auth;
        snprintf(a->username, sizeof a->username, "%s", r.username);
        a->feed_mode = r.home ? FEED_HOME : FEED_SEARCH;
        if (r.session_saved)
            printf("pinfall: session saved to %s\n", r.session_path);
        if (r.save_failed)
            fprintf(stderr, "pinfall: could not save session: %s\n",
                    r.save_msg);
        if (r.home_failed) {
            snprintf(a->toast, sizeof a->toast,
                     "Session expired \xE2\x80\x94 export cookies again");
            a->toast_until = SDL_GetTicks() + 5200;
        }
        if (!r.ok) {
            pc_pinlist_free(&r.list);
            a->booted = false;
            if (a->npins > 0 && r.append) {
                a->loading_more = false;
                a->load_more_retry_at = SDL_GetTicks() + 1500;
                snprintf(a->error, sizeof a->error, "%s",
                         r.err[0] ? r.err : "page load failed, retrying");
            } else {
                a->state = VIEW_ERROR;
                snprintf(a->error, sizeof a->error, "%s", r.err);
            }
            return;
        }
        a->booted = true;
        if (a->pending_query_search) {
            /* --query needs a real bootstrap before /search/pins/ */
            a->pending_query_search = false;
            pc_pinlist_free(&r.list);
            char qbuf[PC_TITLE_LEN];
            snprintf(qbuf, sizeof qbuf, "%s", a->query);
            app_search(a, qbuf);
            break;
        }
        app_install_pins(a, &r.list, r.append);
        pc_pinlist_free(&r.list);
        break;
    }
    case NRES_HOME: {
        bool stale = (r.seq != a->net_search_seq);
        if (stale) {
            pc_pinlist_free(&r.list);
            return;
        }
        if (!r.ok) {
            pc_pinlist_free(&r.list);
            if (r.home_failed) {
                /* session expired mid-run: drop to guest search ONCE, no loop */
                a->auth = PC_AUTH_GUEST;
                a->feed_mode = FEED_SEARCH;
                snprintf(a->toast, sizeof a->toast,
                         "Session expired \xE2\x80\x94 export cookies again");
                a->toast_until = SDL_GetTicks() + 5200;
                app_search(a, DEFAULT_QUERY);
            } else if (a->npins > 0 && r.append) {
                a->loading_more = false;
                a->load_more_retry_at = SDL_GetTicks() + 1500;
                snprintf(a->error, sizeof a->error, "%s",
                         r.err[0] ? r.err : "page load failed, retrying");
            } else {
                a->state = VIEW_ERROR;
                snprintf(a->error, sizeof a->error, "%s", r.err);
            }
            return;
        }
        a->feed_mode = FEED_HOME;
        app_install_pins(a, &r.list, r.append);
        pc_pinlist_free(&r.list);
        break;
    }
    case NRES_SAVED: {
        bool stale = (r.seq != a->net_search_seq);
        if (stale) {
            pc_pinlist_free(&r.list);
            return;
        }
        if (!r.ok) {
            pc_pinlist_free(&r.list);
            if (err_login_required(r.err)) {
                a->auth = PC_AUTH_GUEST;
                a->feed_mode = FEED_SEARCH;
                snprintf(a->toast, sizeof a->toast,
                         "Session expired \xE2\x80\x94 export cookies again");
                a->toast_until = SDL_GetTicks() + 5200;
                app_search(a, DEFAULT_QUERY);
            } else if (a->npins > 0 && r.append) {
                a->loading_more = false;
                a->load_more_retry_at = SDL_GetTicks() + 1500;
                snprintf(a->error, sizeof a->error, "%s",
                         r.err[0] ? r.err : "page load failed, retrying");
            } else {
                a->state = VIEW_ERROR;
                snprintf(a->error, sizeof a->error, "%s", r.err);
            }
            return;
        }
        a->feed_mode = FEED_SAVED;
        if (r.list.count == 0 && !r.append) {
            /* friendly empty state, not an error banner */
            pc_pinlist_free(&r.list);
            app_clear_feed(a);
            a->feed_mode = FEED_SAVED;
            a->state = VIEW_EMPTY;
            return;
        }
        a->booted = true;
        app_install_pins(a, &r.list, r.append);
        pc_pinlist_free(&r.list);
        break;
    }
    case NRES_CREATE_BOARD: {
        if (r.seq != a->create_seq)
            return;
        a->picker.creating = false;
        if (!r.ok) {
            snprintf(a->picker.create_error,
                     sizeof a->picker.create_error, "%.200s", r.err);
            break;
        }
        snprintf(a->picker.created_name,
                 sizeof a->picker.created_name, "%s",
                 a->picker.new_name);
        a->picker.create_error[0] = '\0';
        a->picker.new_name[0] = '\0';
        a->picker.new_name_len = 0;
        a->picker.create_focus = false;
        /* refresh the list, then select + save into the new board */
        a->picker.loading = true;
        a->picker.boards_ready = false;
        NetCmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = NET_BOARDS;
        cmd.seq = ++a->net_boards_seq;
        app_push_net(a, &cmd);
        break;
    }
    case NRES_BOARDS: {
        if (r.seq != a->net_boards_seq) {
            pc_boardlist_free(&r.boards);
            return;
        }
        pc_boardlist_free(&a->picker.boards);
        a->picker.boards = r.boards;
        memset(&r.boards, 0, sizeof r.boards);
        a->picker.boards_ready = true;
        a->picker.loading = false;
        if (!r.ok && r.err[0])
            snprintf(a->picker.error, sizeof a->picker.error, "%s", r.err);
        if (a->picker.open && a->picker.created_name[0] && r.ok) {
            int found = -1;
            for (size_t i = 0; i < a->picker.boards.count; i++) {
                if (strcmp(a->picker.boards.items[i].name,
                           a->picker.created_name) == 0) {
                    found = (int)i;
                    break;
                }
            }
            if (found >= 0) {
                app_picker_choose(a, found);
            } else {
                snprintf(a->picker.error, sizeof a->picker.error,
                         "Board created, but it is not in the list yet");
                a->picker.created_name[0] = '\0';
            }
        }
        break;
    }
    case NRES_SAVE: {
        if (r.seq != a->net_save_seq)
            return;
        a->picker.saving = false;
        a->picker.saving_row = -1;
        if (r.ok) {
            snprintf(a->saved_board_name, sizeof a->saved_board_name, "%s",
                     r.board_name);
            /* bump the chosen board's local pin count */
            for (size_t i = 0; i < a->picker.boards.count; i++) {
                if (strcmp(a->picker.boards.items[i].id,
                           a->picker.saving_board_id) == 0) {
                    a->picker.boards.items[i].pin_count++;
                    break;
                }
            }
            if (a->picker.created_name[0])
                snprintf(a->toast, sizeof a->toast, "Created %.80s and saved",
                         a->picker.created_name);
            else
                snprintf(a->toast, sizeof a->toast, "Saved to %.100s",
                         r.board_name);
            a->toast_until = SDL_GetTicks() + 2800;
            app_close_picker(a);
        } else if (a->picker.open) {
            snprintf(a->picker.error, sizeof a->picker.error, "%s", r.err);
        }
        break;
    }
    case NRES_SEARCH: {
        bool stale = (r.seq != a->net_search_seq);
        if (stale) {
            pc_pinlist_free(&r.list);
            return;
        }
        if (!r.ok) {
            pc_pinlist_free(&r.list);
            if (a->npins > 0 && r.append) {
                a->loading_more = false;
                a->load_more_retry_at = SDL_GetTicks() + 1500;
                snprintf(a->error, sizeof a->error, "%s",
                         r.err[0] ? r.err : "page load failed, retrying");
            } else {
                a->state = VIEW_ERROR;
                snprintf(a->error, sizeof a->error, "%s", r.err);
            }
            return;
        }
        a->booted = true;
        app_install_pins(a, &r.list, r.append);
        pc_pinlist_free(&r.list);
        break;
    }
    case NRES_COMMENTS: {
        if (r.seq != a->comments_seq || !a->show_detail) {
            pc_commentlist_free(&r.comments);
            return;
        }
        if (r.append) {
            /* Load-more must NEVER destroy what is already on screen. The old
             * code fell into the replace branch when an appended page was
             * empty or failed, which made every "View more comments" click
             * wipe the list. */
            if (r.ok && r.comments.count > 0) {
                size_t total = a->comments.count + r.comments.count;
                PcComment *items = (PcComment *)realloc(
                    a->comments.items, total * sizeof(PcComment));

                if (items) {
                    memcpy(items + a->comments.count, r.comments.items,
                           r.comments.count * sizeof(PcComment));
                    for (size_t ci = a->comments.count; ci < total; ci++)
                        app_unescape_comment(&items[ci]);
                    a->comments.items = items;
                    a->comments.count = total;
                    a->comments.cap = total;
                    /* advance the cursor, otherwise "View more" repeats the
                     * same page forever */
                    snprintf(a->comments.bookmark,
                             sizeof a->comments.bookmark, "%s",
                             r.comments.bookmark);
                } else {
                    snprintf(a->comments_err, sizeof a->comments_err,
                             "out of memory loading more comments");
                }
            } else if (r.ok) {
                /* server had no more rows for this cursor */
                a->comments.bookmark[0] = '\0';
            }
            pc_commentlist_free(&r.comments);
        } else {
            pc_commentlist_free(&a->comments);
            a->comments = r.comments;
            for (size_t ci = 0; ci < a->comments.count; ci++)
                app_unescape_comment(&a->comments.items[ci]);
            memset(&r.comments, 0, sizeof r.comments);
        }
        a->comments_loading = false;
        a->comments_ready = true;
        /* The feed payload's comment_count can be stale/0; trust what we
         * actually loaded so the action-bar bubble matches the list. */
        if (r.ok && a->detail_pin.comment_count < (int)a->comments.count)
            a->detail_pin.comment_count = (int)a->comments.count;
        if (r.ok)
            a->comments_err[0] = '\0';
        else
            snprintf(a->comments_err, sizeof a->comments_err, "%.200s",
                     r.err);
        if (getenv("PC_DEBUG_DETAIL"))
            fprintf(stderr, "DBG comments ok=%d n=%zu err=%.120s\n",
                    r.ok ? 1 : 0, a->comments.count, r.ok ? "" : r.err);
        app_detail_measure(a);
        break;
    }
    case NRES_RELATED: {
        if (r.seq != a->related_seq || !a->show_detail) {
            pc_pinlist_free(&r.related);
            return;
        }
        a->rel_loading_more = false;
        if (r.append) {
            /* Same rule as comments: appending must not wipe the grid. */
            if (r.ok && r.related.count > 0) {
                size_t total = a->related.count + r.related.count;
                PcPin *items = (PcPin *)realloc(a->related.items,
                                                total * sizeof(PcPin));
                if (items) {
                    memcpy(items + a->related.count, r.related.items,
                           r.related.count * sizeof(PcPin));
                    a->related.items = items;
                    a->related.count = total;
                    a->related.cap = total;
                    snprintf(a->related.bookmark,
                             sizeof a->related.bookmark, "%s",
                             r.related.bookmark);
                }
            } else if (r.ok) {
                a->related.bookmark[0] = '\0';
            }
            pc_pinlist_free(&r.related);
        } else {
            pc_pinlist_free(&a->related);
            a->related = r.related;
            memset(&r.related, 0, sizeof r.related);
        }
        a->related_loading = false;
        a->related_ready = true;
        a->related_unavailable = false;
        if (r.ok)
            a->related_err[0] = '\0';
        else
            snprintf(a->related_err, sizeof a->related_err, "%.200s",
                     r.err);
        if (getenv("PC_DEBUG_DETAIL"))
            fprintf(stderr, "DBG related ok=%d n=%zu err=%.120s\n",
                    r.ok ? 1 : 0, a->related.count, r.ok ? "" : r.err);
        app_request_related_images(a);
        app_detail_measure(a);
        break;
    }
    case NRES_LIKE: {
        if (r.seq != a->like_seq)
            return;
        a->like_inflight = false;
        if (r.ok) {
            a->detail_pin.liked = a->detail_liked;
            a->detail_pin.like_count = a->detail_like_count;
        } else {
            /* revert optimistic update */
            a->detail_liked = a->like_revert_liked;
            a->detail_like_count = a->like_revert_count;
            snprintf(a->toast, sizeof a->toast, "%.100s",
                     r.err[0] ? r.err : "Could not update like");
            a->toast_until = SDL_GetTicks() + 2600;
        }
        break;
    }
    case NRES_ADD_COMMENT: {
        if (r.seq != a->comment_seq)
            return;
        a->comment_posting = false;
        if (!r.ok) {
            snprintf(a->comment_post_err, sizeof a->comment_post_err,
                     "%.200s",
                     r.err[0] ? r.err : "Could not post comment");
            break;
        }
        PcComment *items = (PcComment *)realloc(
            a->comments.items,
            (a->comments.count + 1) * sizeof(PcComment));
        if (items) {
            PcComment *c = &items[a->comments.count];
            memset(c, 0, sizeof *c);
            snprintf(c->id, sizeof c->id, "local%zu", a->comments.count);
            snprintf(c->user_name, sizeof c->user_name, "you");
            snprintf(c->text, sizeof c->text, "%s", a->pending_comment);
            snprintf(c->created_at, sizeof c->created_at, "now");
            a->comments.items = items;
            a->comments.count++;
            a->comments.cap = a->comments.count;
        }
        a->detail_pin.comment_count++;
        a->comment_draft[0] = '\0';
        a->comment_draft_len = 0;
        a->composer_focus = false;
        a->comment_post_err[0] = '\0';
        snprintf(a->toast, sizeof a->toast, "Comment posted");
        a->toast_until = SDL_GetTicks() + 2400;
        app_detail_measure(a);
        break;
    }
    case NRES_DETAIL: {
        if (r.seq != a->net_detail_seq || !r.ok)
            break;
        if (!a->show_detail)
            break;
        if (strcmp(a->detail_pin.id, r.pin.id) != 0)
            break;
        bool changed = false;
        if (r.pin.description[0] &&
            strcmp(a->detail_pin.description, r.pin.description) != 0) {
            snprintf(a->detail_pin.description,
                     sizeof a->detail_pin.description, "%s",
                     r.pin.description);
            changed = true;
        }
        if (!a->detail_pin.link[0] && r.pin.link[0]) {
            snprintf(a->detail_pin.link, sizeof a->detail_pin.link, "%s",
                     r.pin.link);
            changed = true;
        }
        if (!a->detail_pin.board_name[0] && r.pin.board_name[0]) {
            snprintf(a->detail_pin.board_name,
                     sizeof a->detail_pin.board_name, "%s",
                     r.pin.board_name);
            changed = true;
        }
        if (!a->detail_pin.domain[0] && r.pin.domain[0]) {
            snprintf(a->detail_pin.domain, sizeof a->detail_pin.domain, "%s",
                     r.pin.domain);
            changed = true;
        }
        /* Story pins (e.g. 906419862523865948) are only detected as videos by
         * the REST detail enrichment, not by the feed payload. Adopt the video
         * metadata as soon as it arrives and start playing. */
        bool became_video = false;
        if (!a->detail_pin.is_video && r.pin.is_video &&
            r.pin.video_url[0]) {
            a->detail_pin.is_video = true;
            snprintf(a->detail_pin.video_url,
                     sizeof a->detail_pin.video_url, "%s", r.pin.video_url);
            a->detail_pin.video_w = r.pin.video_w;
            a->detail_pin.video_h = r.pin.video_h;
            a->detail_pin.video_duration_ms = r.pin.video_duration_ms;
            became_video = true;
            changed = true;
        } else if (a->detail_pin.is_video && !a->detail_pin.video_url[0] &&
                   r.pin.video_url[0]) {
            snprintf(a->detail_pin.video_url,
                     sizeof a->detail_pin.video_url, "%s", r.pin.video_url);
            a->detail_pin.video_w = r.pin.video_w;
            a->detail_pin.video_h = r.pin.video_h;
            a->detail_pin.video_duration_ms = r.pin.video_duration_ms;
            became_video = true;
            changed = true;
        }
        if (changed) {
            if (a->detail_feed_idx >= 0 &&
                (size_t)a->detail_feed_idx < a->npins)
                a->pins[a->detail_feed_idx].pin = a->detail_pin;
            app_detail_measure(a);
            if (became_video)
                app_video_begin(a);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------- image loading */
/* Uploaded-texture accounting (approximate bytes = w*h*4 RGBA). */
static void tex_clear(App *a, PinView *pv)
{
    if (pv->tex) {
        size_t bytes = (size_t)pv->tw * (size_t)pv->th * 4u;
        SDL_DestroyTexture(pv->tex);
        pv->tex = NULL;
        if (a->tex_count > 0)
            a->tex_count--;
        if (a->tex_bytes >= bytes)
            a->tex_bytes -= bytes;
        else
            a->tex_bytes = 0;
    }
    pv->tw = 0;
    pv->th = 0;
}

static void tex_uploaded(App *a, PinView *pv, int w, int h)
{
    pv->tw = w;
    pv->th = h;
    a->tex_count++;
    a->tex_bytes += (size_t)w * (size_t)h * 4u;
    if (a->tex_count > a->tex_peak)
        a->tex_peak = a->tex_count;
}

/* Keep textures for pins intersecting the viewport expanded by keep_screens
 * viewports above and below (default +/-2). */
static void app_texture_band(App *a, float *top, float *bot)
{
    float ch = content_h(a);
    if (ch < 1.0f)
        ch = 1.0f;
    int keep = a->settings.keep_screens;
    if (keep < 1)
        keep = 1;
    if (top)
        *top = a->scroll - (float)keep * ch;
    if (bot)
        *bot = a->scroll + (float)(keep + 1) * ch;
}

/* Destroy textures that left the keep band and mark them "wants image" so
 * they are re-requested (and served from the RAM cache) when they come back. */
static void app_evict_textures(App *a)
{
    if (a->npins == 0 || a->layout.count == 0)
        return;
    float top, bot;
    app_texture_band(a, &top, &bot);
    size_t n = a->npins < a->layout.count ? a->npins : a->layout.count;
    for (size_t i = 0; i < n; i++) {
        PinView *pv = &a->pins[i];
        if (!pv->tex)
            continue;
        const LayoutItem *it = &a->layout.items[i];
        bool in_band = (it->y + it->card_h >= top) && (it->y <= bot);
        if (in_band)
            continue;
        tex_clear(a, pv);
        pv->status = 0; /* wants image again (RAM-cache hit on reload) */
    }
}

/* Request images only for pins inside the keep band; visible pins get the
 * request queue first. Keeps outstanding work bounded regardless of how many
 * pins are in the collection. */
static void app_request_images(App *a)
{
    if (!a->client || a->npins == 0 || a->layout.count == 0)
        return;

    float band_top, band_bot;
    app_texture_band(a, &band_top, &band_bot);
    float vis_top = a->scroll;
    float vis_bot = a->scroll + content_h(a) + 200.0f;
    size_t n = a->npins < a->layout.count ? a->npins : a->layout.count;

    pthread_mutex_lock(&a->img_mtx);
    int space = MAX_REQS - a->req_count;
    for (int pass = 0; pass < 2 && space > 0; pass++) {
        for (size_t i = 0; i < n && space > 0; i++) {
            PinView *pv = &a->pins[i];
            if (pv->status != 0)
                continue;
            const LayoutItem *it = &a->layout.items[i];
            bool in_band = (it->y + it->card_h >= band_top) &&
                           (it->y <= band_bot);
            if (!in_band)
                continue;
            bool visible = it->y + it->card_h > vis_top && it->y < vis_bot;
            if (pass == 0 && !visible)
                continue;
            if (pass == 1 && visible)
                continue;
            if (!pv->pin.thumb_url[0]) {
                pv->status = 3;
                continue;
            }
            ImgReq *r = &a->reqs[(a->req_head + a->req_count) % MAX_REQS];
            memset(r, 0, sizeof *r);
            r->target = IMG_GRID;
            r->gen = a->gen;
            r->idx = (int)i;
            snprintf(r->url, sizeof r->url, "%s", pv->pin.thumb_url);
            r->max_w = (int)(it->w + 0.5f);
            a->req_count++;
            pv->status = 1;
            space--;
        }
    }
    bool wake = a->req_count > 0;
    pthread_mutex_unlock(&a->img_mtx);
    if (wake)
        pthread_cond_broadcast(&a->img_cv);
}

/* Recompute residency from the pin array. The incremental counters are only
 * for cheap peak tracking; this is the source of truth used for stats and the
 * residency bound (keeps the numbers exact across eviction races). */
static void app_recount_textures(App *a)
{
    int count = 0;
    size_t bytes = 0;
    for (size_t i = 0; i < a->npins; i++) {
        PinView *pv = &a->pins[i];
        if (!pv->tex)
            continue;
        count++;
        bytes += (size_t)pv->tw * (size_t)pv->th * 4u;
    }
    a->tex_count = count;
    a->tex_bytes = bytes;
    if (count > a->tex_peak)
        a->tex_peak = count;
}

/* Live cache stats + FPS for the stats overlay / machine-readable line. */
static void app_update_stats(App *a)
{
    if (a->client)
        pc_image_get_stats(a->client, &a->img_stats);

    Uint32 now = SDL_GetTicks();
    if (a->fps_window_start == 0)
        a->fps_window_start = now;
    a->fps_frames++;
    Uint32 elapsed = now - a->fps_window_start;
    if (elapsed >= 250) {
        float fps = (float)a->fps_frames * 1000.0f / (float)elapsed;
        a->fps = a->fps > 0.0f ? a->fps * 0.5f + fps * 0.5f : fps;
        a->fps_frames = 0;
        a->fps_window_start = now;
    }
}

/* One machine-readable line on stderr (used by --stats). */
static void app_print_stats_line(App *a)
{
    PcImgStats st;
    memset(&st, 0, sizeof st);
    app_recount_textures(a);
    if (a->client)
        pc_image_get_stats(a->client, &st);
    double used_mb = (double)st.used_bytes / (1024.0 * 1024.0);
    double budget_mb = (double)st.budget_bytes / (1024.0 * 1024.0);
    double tex_mb = (double)a->tex_bytes / (1024.0 * 1024.0);
    uint64_t ads_blocked = 0;
    int block_ads = a->settings.block_ads ? 1 : 0;
    if (a->client) {
        ads_blocked = pc_ads_blocked(a->client);
        block_ads = pc_get_block_ads(a->client) ? 1 : 0;
    }
    fprintf(stderr,
            "pinfall-stats: cache_mode=%s budget_mb=%.1f used_mb=%.3f "
            "hits=%llu misses=%llu packs=%llu unpacks=%llu pins=%zu "
            "textures=%d texture_mb=%.2f tex_peak=%d keep_screens=%d "
            "fps=%.1f ads_blocked=%llu block_ads=%d\n",
            settings_cache_mode_name(st.mode), budget_mb, used_mb,
            (unsigned long long)st.hits, (unsigned long long)st.misses,
            (unsigned long long)st.packs, (unsigned long long)st.unpacks,
            a->npins, a->tex_count, tex_mb, a->tex_peak,
            a->settings.keep_screens, (double)a->fps,
            (unsigned long long)ads_blocked, block_ads);
}

static void app_queue_detail_image(App *a)
{
    if (!a->client || !a->show_detail)
        return;
    const char *url = a->detail_pin.medium_url;
    if (!url[0])
        url = a->detail_pin.big_url;
    if (!url[0])
        url = a->detail_pin.thumb_url;
    if (!url[0]) {
        a->detail.status = 3;
        return;
    }

    pthread_mutex_lock(&a->img_mtx);
    if (a->req_count < MAX_REQS) {
        ImgReq *r = &a->reqs[(a->req_head + a->req_count) % MAX_REQS];
        memset(r, 0, sizeof *r);
        r->target = IMG_DETAIL;
        r->gen = a->detail_serial;
        r->idx = 0;
        snprintf(r->url, sizeof r->url, "%s", url);
        r->max_w = (int)(a->d_media_w + 0.5f);
        a->req_count++;
    }
    pthread_mutex_unlock(&a->img_mtx);
    pthread_cond_broadcast(&a->img_cv);
    a->detail.status = 1;
}

static void app_consume_images(App *a)
{
    ImgRes batch[MAX_RESULTS];
    int n = 0;

    pthread_mutex_lock(&a->img_mtx);
    n = a->nresults;
    if (n > 0) {
        memcpy(batch, a->results, (size_t)n * sizeof(ImgRes));
        a->nresults = 0;
    }
    pthread_mutex_unlock(&a->img_mtx);

    for (int i = 0; i < n; i++) {
        ImgRes *r = &batch[i];
        if (r->target == IMG_GRID) {
            if (r->gen != a->gen || r->idx < 0 ||
                (size_t)r->idx >= a->npins) {
            } else {
                PinView *pv = &a->pins[r->idx];
                if (r->ok && r->pixels && r->w > 0 && r->h > 0) {
                    SDL_Texture *tex = rnd_texture_rgba(
                        &a->rnd, r->pixels, r->w, r->h, a->M.card_radius);
                    if (tex) {
                        tex_clear(a, pv);
                        pv->tex = tex;
                        tex_uploaded(a, pv, r->w, r->h);
                        pv->status = 2;
                    } else {
                        pv->status = 3;
                    }
                } else {
                    pv->status = 3;
                }
            }
        } else if (r->target == IMG_RELATED) {
            if (r->gen == a->rel_img_serial && a->show_detail &&
                r->idx >= 0 && r->idx < MAX_REL) {
                if (r->ok && r->pixels && r->w > 0 && r->h > 0) {
                    SDL_Texture *tex = rnd_texture_rgba(
                        &a->rnd, r->pixels, r->w, r->h,
                        CU_CARD_RADIUS * a->rnd.scale);
                    if (tex) {
                        if (a->rel_tex[r->idx])
                            SDL_DestroyTexture(a->rel_tex[r->idx]);
                        a->rel_tex[r->idx] = tex;
                        a->rel_tw[r->idx] = r->w;
                        a->rel_th[r->idx] = r->h;
                        a->rel_status[r->idx] = 2;
                    } else {
                        a->rel_status[r->idx] = 3;
                    }
                } else {
                    a->rel_status[r->idx] = 3;
                }
            }
        } else {
            if (r->gen == a->detail_serial && a->show_detail) {
                if (r->ok && r->pixels && r->w > 0 && r->h > 0) {
                    SDL_Texture *tex = rnd_texture_rgba(
                        &a->rnd, r->pixels, r->w, r->h,
                        a->M.panel_radius);
                    if (tex) {
                        if (a->detail.tex)
                            SDL_DestroyTexture(a->detail.tex);
                        a->detail.tex = tex;
                        a->detail.tw = r->w;
                        a->detail.th = r->h;
                        a->detail.status = 2;
                    } else {
                        a->detail.status = 3;
                    }
                } else {
                    a->detail.status = 3;
                }
            }
        }
        if (r->pixels)
            pc_free(r->pixels);
    }
}

/* ----------------------------------------------------------- net worker */
static bool worker_search(App *a, const char *query, const char *bookmark,
                          PcPinList *out, char *errbuf, size_t errsz)
{
    memset(out, 0, sizeof *out);
    char *err = NULL;
    bool ok = pc_search(a->client, query,
                        (bookmark && bookmark[0]) ? bookmark : NULL, out,
                        &err);
    if (!ok) {
        if (err && *err)
            snprintf(errbuf, errsz, "%s", err);
        else
            snprintf(errbuf, errsz, "%s", pc_last_error(a->client));
        pc_pinlist_free(out);
    }
    if (err)
        pc_free(err);
    return ok;
}

static bool worker_home(App *a, const char *bookmark, PcPinList *out,
                        char *errbuf, size_t errsz)
{
    memset(out, 0, sizeof *out);
    char *err = NULL;
    bool ok = pc_homefeed(a->client,
                          (bookmark && bookmark[0]) ? bookmark : NULL, out,
                          &err);
    if (!ok) {
        if (err && *err)
            snprintf(errbuf, errsz, "%s", err);
        else
            snprintf(errbuf, errsz, "%s", pc_last_error(a->client));
        pc_pinlist_free(out);
    }
    if (err)
        pc_free(err);
    return ok;
}

static bool session_save(App *a, const char *path, char *errbuf, size_t errsz)
{
    char dir[700];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (*dir && !mkdir_p(dir)) {
            snprintf(errbuf, errsz, "cannot create directory %.180s", dir);
            return false;
        }
    }
    char *err = NULL;
    bool ok = pc_cookie_save(a->client, path, &err);
    if (!ok) {
        if (err && *err)
            snprintf(errbuf, errsz, "%s", err);
        else
            snprintf(errbuf, errsz, "%s", pc_last_error(a->client));
    }
    if (err)
        pc_free(err);
    return ok;
}

static void net_res_free(NetRes *r)
{
    if (r->kind == NRES_BOARDS) {
        pc_boardlist_free(&r->boards);
        return;
    }
    if (r->kind == NRES_COMMENTS) {
        pc_commentlist_free(&r->comments);
        return;
    }
    if (r->kind == NRES_RELATED) {
        pc_pinlist_free(&r->related);
        return;
    }
    if (r->kind == NRES_CREATE_BOARD)
        return;
    if (r->kind != NRES_DETAIL)
        pc_pinlist_free(&r->list);
}

static void *net_thread_fn(void *arg)
{
    App *a = (App *)arg;
    for (;;) {
        NetCmd cmd;
        memset(&cmd, 0, sizeof cmd);

        pthread_mutex_lock(&a->net_mtx);
        while (!a->net_pending && !a->net_quit)
            pthread_cond_wait(&a->net_cv, &a->net_mtx);
        if (a->net_quit) {
            pthread_mutex_unlock(&a->net_mtx);
            break;
        }
        cmd = a->net_q[a->net_head];
        a->net_head = (a->net_head + 1) % NET_QUEUE;
        a->net_pending = (a->net_head != a->net_tail);
        a->net_busy = true;
        pthread_mutex_unlock(&a->net_mtx);

        NetRes res;
        memset(&res, 0, sizeof res);
        res.seq = cmd.seq;
        res.append = cmd.append;
        res.ok = true;

        switch (cmd.kind) {
        case NET_START: {
            /* ordering: bootstrap -> auth probe -> home feed or guest search */
            res.kind = NRES_START;
            res.auth = PC_AUTH_UNKNOWN;
            PcPinList list;
            if (!pc_bootstrap(a->client)) {
                res.ok = false;
                snprintf(res.err, sizeof res.err, "%s",
                         pc_last_error(a->client));
                break;
            }
            if (cmd.preview) {
                res.auth = PC_AUTH_LOGGED_IN;
                snprintf(res.username, sizeof res.username, "%s", "preview");
            } else {
                res.auth = pc_auth_check(a->client);
                if (res.auth == PC_AUTH_LOGGED_IN)
                    snprintf(res.username, sizeof res.username, "%s",
                             pc_username(a->client));
            }
            bool authed = (res.auth == PC_AUTH_LOGGED_IN);
            if (authed && !cmd.preview) {
                if (worker_home(a, NULL, &list, res.home_msg,
                                sizeof res.home_msg)) {
                    res.list = list;
                    res.home = true;
                } else {
                    bool lr = err_login_required(res.home_msg);
                    res.home_failed = lr;
                    if (lr)
                        res.auth = PC_AUTH_GUEST;
                    res.home = false;
                    if (!worker_search(a, cmd.query, NULL, &list, res.err,
                                       sizeof res.err)) {
                        res.ok = false;
                        break;
                    }
                    res.list = list;
                }
            } else if (authed && cmd.preview) {
                /* rendering preview: reuse search results as a home-style
                 * feed because a real home feed needs a real cookie jar */
                if (!worker_search(a, cmd.query, NULL, &list, res.err,
                                   sizeof res.err)) {
                    res.ok = false;
                    break;
                }
                res.list = list;
                res.home = true;
            } else {
                if (!worker_search(a, cmd.query, NULL, &list, res.err,
                                   sizeof res.err)) {
                    res.ok = false;
                    break;
                }
                res.list = list;
                res.home = false;
            }
            if (res.ok && res.auth == PC_AUTH_LOGGED_IN && cmd.save_session &&
                !cmd.preview && cmd.session_path[0]) {
                if (session_save(a, cmd.session_path, res.save_msg,
                                 sizeof res.save_msg)) {
                    res.session_saved = true;
                    snprintf(res.session_path, sizeof res.session_path, "%s",
                             cmd.session_path);
                } else {
                    res.save_failed = true;
                }
            }
            break;
        }
        case NET_HOME: {
            res.kind = NRES_HOME;
            res.home = true;
            PcPinList list;
            if (worker_home(a, cmd.bookmark, &list, res.err,
                            sizeof res.err)) {
                res.list = list;
            } else {
                res.ok = false;
                res.home_failed = err_login_required(res.err);
                if (res.home_failed)
                    res.auth = PC_AUTH_GUEST;
            }
            break;
        }
        case NET_SAVED: {
            res.kind = NRES_SAVED;
            res.auth = a->auth;
            PcPinList list;
            if (a->auth_preview || cmd.preview) {
                /* preview mode: current search results stand in for saved pins */
                if (!worker_search(a, cmd.query, cmd.bookmark, &list,
                                   res.err, sizeof res.err)) {
                    res.ok = false;
                    break;
                }
            } else {
                char *err = NULL;
                bool ok = pc_user_pins(a->client, NULL,
                                       cmd.bookmark[0] ? cmd.bookmark : NULL,
                                       &list, &err);
                if (!ok) {
                    if (err && *err)
                        snprintf(res.err, sizeof res.err, "%s", err);
                    else
                        snprintf(res.err, sizeof res.err, "%s",
                                 pc_last_error(a->client));
                    pc_pinlist_free(&list);
                    if (err)
                        pc_free(err);
                    res.ok = false;
                    break;
                }
                if (err)
                    pc_free(err);
            }
            res.list = list;
            break;
        }
        case NET_BOARDS: {
            res.kind = NRES_BOARDS;
            if (a->auth_preview) {
                if (!preview_fill_boards(&res.boards)) {
                    res.ok = false;
                    snprintf(res.err, sizeof res.err,
                             "could not build preview boards");
                }
            } else {
                char *err = NULL;
                res.ok = pc_user_boards(a->client, NULL, &res.boards, &err);
                if (!res.ok) {
                    if (err && *err)
                        snprintf(res.err, sizeof res.err, "%s", err);
                    else
                        snprintf(res.err, sizeof res.err, "%s",
                                 pc_last_error(a->client));
                }
                if (err)
                    pc_free(err);
            }
            break;
        }
        case NET_SAVE: {
            res.kind = NRES_SAVE;
            snprintf(res.board_name, sizeof res.board_name, "%s",
                     cmd.board_name);
            if (a->no_writes) {
                res.ok = false;
                snprintf(res.err, sizeof res.err,
                         "saving is disabled (--no-writes)");
            } else if (a->auth != PC_AUTH_LOGGED_IN) {
                res.ok = false;
                snprintf(res.err, sizeof res.err, "Sign in to save");
            } else if (a->auth_preview) {
                char nm[PC_BOARD_NAME_LEN] = "";
                res.ok = preview_save_pin(cmd.pin_id, cmd.board_id, nm,
                                          sizeof nm);
                if (res.ok) {
                    if (nm[0])
                        snprintf(res.board_name, sizeof res.board_name, "%s",
                                 nm);
                } else {
                    snprintf(res.err, sizeof res.err,
                             "preview save failed");
                }
            } else {
                char *err = NULL;
                res.ok = pc_save_pin(a->client, cmd.pin_id, cmd.board_id,
                                     &err);
                if (!res.ok) {
                    if (err && *err)
                        snprintf(res.err, sizeof res.err, "%s", err);
                    else
                        snprintf(res.err, sizeof res.err, "%s",
                                 pc_last_error(a->client));
                }
                if (err)
                    pc_free(err);
            }
            break;
        }
        case NET_SEARCH: {
            res.kind = NRES_SEARCH;
            res.auth = a->auth;
            char *err = NULL;
            bool ok = pc_search(a->client, cmd.query,
                                cmd.bookmark[0] ? cmd.bookmark : NULL,
                                &res.list, &err);
            if (!ok) {
                res.ok = false;
                if (err && *err)
                    snprintf(res.err, sizeof res.err, "%s", err);
                else
                    snprintf(res.err, sizeof res.err, "%s",
                             pc_last_error(a->client));
                if (err)
                    pc_free(err);
                pc_pinlist_free(&res.list);
            }
            break;
        }
        case NET_COMMENTS: {
            res.kind = NRES_COMMENTS;
            char *err = NULL;
            if (getenv("PC_DEBUG_DETAIL"))
                fprintf(stderr, "DBG worker comments start id=%.40s\n",
                        cmd.pin_id);
            res.ok = pc_pin_comments(a->client, cmd.pin_id,
                                     cmd.bookmark[0] ? cmd.bookmark : NULL,
                                     &res.comments, &err);
            if (getenv("PC_DEBUG_DETAIL"))
                fprintf(stderr, "DBG worker comments end ok=%d n=%zu err=%.120s\n",
                        res.ok ? 1 : 0, res.comments.count,
                        err ? err : "");
            if (!res.ok) {
                if (err && *err)
                    snprintf(res.err, sizeof res.err, "%s", err);
                else
                    snprintf(res.err, sizeof res.err, "%s",
                             pc_last_error(a->client));
                pc_commentlist_free(&res.comments);
            }
            if (err)
                pc_free(err);
            break;
        }
        case NET_RELATED: {
            res.kind = NRES_RELATED;
            char *err = NULL;
            res.ok = pc_pin_related(a->client, cmd.pin_id,
                                    cmd.bookmark[0] ? cmd.bookmark : NULL,
                                    &res.related, &err);
            if (!res.ok) {
                if (err && *err)
                    snprintf(res.err, sizeof res.err, "%s", err);
                else
                    snprintf(res.err, sizeof res.err, "%s",
                             pc_last_error(a->client));
                pc_pinlist_free(&res.related);
            }
            if (err)
                pc_free(err);
            break;
        }
        case NET_LIKE: {
            res.kind = NRES_LIKE;
            res.liked = cmd.liked;
            if (a->no_writes) {
                res.ok = false;
                snprintf(res.err, sizeof res.err,
                         "Likes are disabled (--no-writes)");
            } else if (a->auth != PC_AUTH_LOGGED_IN) {
                res.ok = false;
                snprintf(res.err, sizeof res.err, "Sign in to like");
            } else {
                char *err = NULL;
                res.ok = pc_set_pin_liked(a->client, cmd.pin_id, cmd.liked,
                                          &err);
                if (!res.ok) {
                    if (err && *err)
                        snprintf(res.err, sizeof res.err, "%s", err);
                    else
                        snprintf(res.err, sizeof res.err, "%s",
                                 pc_last_error(a->client));
                }
                if (err)
                    pc_free(err);
            }
            break;
        }
        case NET_ADD_COMMENT: {
            res.kind = NRES_ADD_COMMENT;
            if (a->no_writes) {
                res.ok = false;
                snprintf(res.err, sizeof res.err,
                         "Comments are disabled (--no-writes)");
            } else if (a->auth != PC_AUTH_LOGGED_IN) {
                res.ok = false;
                snprintf(res.err, sizeof res.err, "Sign in to comment");
            } else {
                char *err = NULL;
                res.ok = pc_add_comment(a->client, cmd.pin_id, cmd.text,
                                        &err);
                if (!res.ok) {
                    if (err && *err)
                        snprintf(res.err, sizeof res.err, "%s", err);
                    else
                        snprintf(res.err, sizeof res.err, "%s",
                                 pc_last_error(a->client));
                }
                if (err)
                    pc_free(err);
            }
            break;
        }
        case NET_CREATE_BOARD: {
            res.kind = NRES_CREATE_BOARD;
            if (a->no_writes) {
                res.ok = false;
                snprintf(res.err, sizeof res.err,
                         "Boards are read-only (--no-writes)");
            } else if (a->auth != PC_AUTH_LOGGED_IN) {
                res.ok = false;
                snprintf(res.err, sizeof res.err,
                         "Sign in to create boards");
            } else {
                char *err = NULL;
                res.ok = pc_create_board(a->client, cmd.board_name, "",
                                         cmd.secret ? "secret" : "public",
                                         &res.board, &err);
                res.has_board = res.ok;
                if (!res.ok) {
                    if (err && *err)
                        snprintf(res.err, sizeof res.err, "%s", err);
                    else
                        snprintf(res.err, sizeof res.err, "%s",
                                 pc_last_error(a->client));
                }
                if (err)
                    pc_free(err);
            }
            break;
        }
        case NET_DETAIL: {
            res.kind = NRES_DETAIL;
            res.ok = pc_pin_detail(a->client, cmd.pin_id, &res.pin, NULL);
            break;
        }
        default:
            res.ok = false;
            break;
        }

        pthread_mutex_lock(&a->net_mtx);
        if (a->net_res_ready)
            net_res_free(&a->net_res);
        a->net_res = res;
        a->net_res_ready = true;
        a->net_busy = false;
        pthread_mutex_unlock(&a->net_mtx);
    }
    return NULL;
}

/* --------------------------------------------------------- image workers */
static void *img_thread_fn(void *arg)
{
    App *a = (App *)arg;

    for (;;) {
        ImgReq req;
        memset(&req, 0, sizeof req);

        pthread_mutex_lock(&a->img_mtx);
        for (;;) {
            if (a->img_quit) {
                pthread_mutex_unlock(&a->img_mtx);
                return NULL;
            }
            if (a->req_count == 0) {
                pthread_cond_wait(&a->img_cv, &a->img_mtx);
                continue;
            }
            req = a->reqs[a->req_head];
            a->req_head = (a->req_head + 1) % MAX_REQS;
            a->req_count--;

            bool stale = (req.target == IMG_GRID && req.gen != a->gen) ||
                         (req.target == IMG_DETAIL &&
                          req.gen != a->detail_serial) ||
                         (req.target == IMG_RELATED &&
                          req.gen != a->rel_img_serial);
            if (stale)
                continue;
            break;
        }
        pthread_mutex_unlock(&a->img_mtx);

        uint8_t *pixels = NULL;
        int w = 0, h = 0;
        char *err = NULL;
        bool ok = pc_image_get(a->client, req.url, req.max_w, &pixels, &w, &h,
                               &err);
        if (err)
            pc_free(err);
        if (!ok && pixels) {
            pc_free(pixels);
            pixels = NULL;
        }

        pthread_mutex_lock(&a->img_mtx);
        if (a->nresults < MAX_RESULTS) {
            ImgRes *r = &a->results[a->nresults++];
            r->target = req.target;
            r->gen = req.gen;
            r->idx = req.idx;
            r->pixels = pixels;
            r->w = w;
            r->h = h;
            r->ok = ok;
        } else if (pixels) {
            pc_free(pixels);
        }
        pthread_mutex_unlock(&a->img_mtx);
    }
    return NULL;
}

/* ---------------------------------------------------------- search/retry */
static void app_push_net(App *a, const NetCmd *cmd)
{
    pthread_mutex_lock(&a->net_mtx);
    int next = (a->net_tail + 1) % NET_QUEUE;
    if (next == a->net_head) {
        /* Ring full: drop the OLDEST pending command (stale request beats a
         * stalled one). */
        a->net_head = (a->net_head + 1) % NET_QUEUE;
    }
    a->net_q[a->net_tail] = *cmd;
    a->net_tail = next;
    a->net_pending = true;
    pthread_cond_signal(&a->net_cv);
    pthread_mutex_unlock(&a->net_mtx);
}

static void app_search(App *a, const char *query)
{
    if (!query || !*query)
        return;
    app_close_detail(a);
    app_close_picker(a);
    snprintf(a->query, sizeof a->query, "%s", query);
    a->feed_mode = FEED_SEARCH;
    a->state = VIEW_LOADING;
    a->error[0] = '\0';
    app_clear_feed(a);

    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = a->booted ? NET_SEARCH : NET_START;
    cmd.seq = ++a->net_search_seq;
    snprintf(cmd.query, sizeof cmd.query, "%s", query);
    cmd.preview = a->auth_preview;
    if (cmd.kind == NET_START) {
        cmd.save_session = a->save_session;
        snprintf(cmd.session_path, sizeof cmd.session_path, "%s",
                 a->session_path);
    }
    app_push_net(a, &cmd);

    snprintf(a->toast, sizeof a->toast, "Searching \xE2\x80\x9C%s\xE2\x80\x9D",
             query);
    a->toast_until = SDL_GetTicks() + 1800;
}

static void app_retry(App *a)
{
    a->booted = false;
    char qbuf[PC_TITLE_LEN];
    snprintf(qbuf, sizeof qbuf, "%s", a->query);
    app_search(a, qbuf);
}

#ifdef PC_ANDROID
static void app_reload_cookies(App *a)
{
    if (!a->client)
        return;
    if (!pf_android_reload_cookies(a->client)) {
        snprintf(a->toast, sizeof a->toast,
                 "No cookies.txt found on the device");
        a->toast_until = SDL_GetTicks() + 2600;
        return;
    }
    a->booted = false;
    app_close_settings(a);
    char qbuf[PC_TITLE_LEN];
    snprintf(qbuf, sizeof qbuf, "%s", a->query);
    app_search(a, qbuf);
    snprintf(a->toast, sizeof a->toast, "Cookies reloaded");
    a->toast_until = SDL_GetTicks() + 2200;
}
#endif

static void app_go_home(App *a)
{
    if (a->auth != PC_AUTH_LOGGED_IN)
        return;
    app_close_detail(a);
    app_close_picker(a);
    a->feed_mode = FEED_HOME;
    a->state = VIEW_LOADING;
    a->error[0] = '\0';
    app_clear_feed(a);

    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.seq = ++a->net_search_seq;
    if (a->auth_preview) {
        /* no real jar: home tab previews the personal feed with search data */
        cmd.kind = a->booted ? NET_SEARCH : NET_START;
        snprintf(cmd.query, sizeof cmd.query, "%s", a->query);
        cmd.preview = true;
    } else {
        cmd.kind = a->booted ? NET_HOME : NET_START;
    }
    app_push_net(a, &cmd);

    snprintf(a->toast, sizeof a->toast, "Opening Home");
    a->toast_until = SDL_GetTicks() + 1200;
}

static void app_go_saved(App *a)
{
    if (a->auth != PC_AUTH_LOGGED_IN)
        return;
    app_close_detail(a);
    app_close_picker(a);
    a->feed_mode = FEED_SAVED;
    a->state = VIEW_LOADING;
    a->error[0] = '\0';
    app_clear_feed(a);
    a->feed_mode = FEED_SAVED;

    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = NET_SAVED;
    cmd.seq = ++a->net_search_seq;
    cmd.preview = a->auth_preview;
    snprintf(cmd.query, sizeof cmd.query, "%s", a->query);
    app_push_net(a, &cmd);

    snprintf(a->toast, sizeof a->toast, "Opening Saved");
    a->toast_until = SDL_GetTicks() + 1200;
}

static void app_maybe_load_more(App *a)
{
    if (getenv("PC_DEBUG_LM")) {
        static Uint32 lm_last;
        Uint32 lm_now = SDL_GetTicks();

        if (lm_now - lm_last > 1000) {
            lm_last = lm_now;
            fprintf(stderr,
                    "LM state=%d busy=%d bm=%zu pages=%d npins=%zu snap=%d "
                    "scroll=%.0f ch=%.0f total=%.0f need=%.0f\n",
                    (int)a->state, a->loading_more ? 1 : 0,
                    strlen(a->bookmark), a->pages, a->npins,
                    a->snapshot ? 1 : 0, (double)a->scroll,
                    (double)content_h(a), (double)a->layout.total_h,
                    (double)(a->layout.total_h - 260.0f * a->rnd.scale));
        }
    }
    if (a->state != VIEW_FEED || a->loading_more || !a->bookmark[0] ||
        a->pages >= MAX_PAGES || a->npins >= MAX_PINS || a->snapshot)
        return;
    /* Backoff after a failed page so a throttled API is not hammered every
     * frame (that used to trip rate limits and stall deep pagination). */
    if (a->load_more_retry_at && SDL_GetTicks() < a->load_more_retry_at)
        return;
    if (a->scroll + content_h(a) < a->layout.total_h - 260.0f * a->rnd.scale)
        return;

    a->loading_more = true;
    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    if (a->feed_mode == FEED_SAVED && a->auth == PC_AUTH_LOGGED_IN)
        cmd.kind = NET_SAVED;
    else if (a->feed_mode == FEED_HOME && a->auth == PC_AUTH_LOGGED_IN &&
             !a->auth_preview)
        cmd.kind = NET_HOME;
    else
        cmd.kind = a->booted ? NET_SEARCH : NET_START;
    cmd.seq = ++a->net_search_seq;
    cmd.append = true;
    if (cmd.kind == NET_HOME || cmd.kind == NET_SAVED) {
        snprintf(cmd.bookmark, sizeof cmd.bookmark, "%s", a->bookmark);
        if (cmd.kind == NET_SAVED) {
            snprintf(cmd.query, sizeof cmd.query, "%s", a->query);
            cmd.preview = a->auth_preview;
        }
    } else {
        snprintf(cmd.query, sizeof cmd.query, "%s", a->query);
        snprintf(cmd.bookmark, sizeof cmd.bookmark, "%s", a->bookmark);
        if (cmd.kind == NET_START) {
            cmd.preview = a->auth_preview;
            cmd.save_session = a->save_session;
            snprintf(cmd.session_path, sizeof cmd.session_path, "%s",
                     a->session_path);
        }
    }
    app_push_net(a, &cmd);
}

/* ------------------------------------------------------- video playback --
 * One decoder thread per app (ffmpeg subprocess via pc_video_*). The worker
 * owns the decoder and writes the newest frame into vid_pixels; the UI thread
 * uploads it to an SDL texture. Loops by reopening at EOF.
 */

static void *vid_thread_fn(void *arg)
{
    App *a = (App *)arg;
    PcVideo *v = NULL;
    uint8_t *buf = NULL;
    uint64_t gen = 0;
    char src[700];
    int ow = 0, oh = 0, fps = 24;

    for (;;) {
        pthread_mutex_lock(&a->vid_mtx);
        while (!a->vid_quit && !a->vid_has_cmd)
            pthread_cond_wait(&a->vid_cv, &a->vid_mtx);
        if (a->vid_quit) {
            pthread_mutex_unlock(&a->vid_mtx);
            break;
        }
        gen = a->vid_gen;
        snprintf(src, sizeof src, "%s", a->vid_src);
        ow = a->vid_out_w;
        oh = a->vid_out_h;
        fps = a->vid_fps > 0 ? a->vid_fps : 24;
        a->vid_has_cmd = false;
        a->vid_stop = false;
        a->vid_decode_failed = false;
        pthread_mutex_unlock(&a->vid_mtx);

        char *err = NULL;
        v = pc_video_open(src, ow, oh, fps, &err);
        if (err)
            pc_free(err);
        if (!v) {
            pthread_mutex_lock(&a->vid_mtx);
            if (gen == a->vid_gen)
                a->vid_decode_failed = true;
            pthread_mutex_unlock(&a->vid_mtx);
            continue;
        }
        buf = (uint8_t *)malloc((size_t)ow * (size_t)oh * 4u);
        if (!buf) {
            pc_video_close(v);
            v = NULL;
            pthread_mutex_lock(&a->vid_mtx);
            if (gen == a->vid_gen)
                a->vid_decode_failed = true;
            pthread_mutex_unlock(&a->vid_mtx);
            continue;
        }

        for (;;) {
            pthread_mutex_lock(&a->vid_mtx);
            while (a->video_paused && !a->vid_quit && !a->vid_stop &&
                   gen == a->vid_gen)
                pthread_cond_wait(&a->vid_cv, &a->vid_mtx);
            bool stop = a->vid_quit || a->vid_stop || gen != a->vid_gen;
            pthread_mutex_unlock(&a->vid_mtx);
            if (stop)
                break;

            if (!pc_video_read_frame(v, buf)) {
                /* EOF: reopen for a seamless loop (unless stopped) */
                pc_video_close(v);
                v = pc_video_open(src, ow, oh, fps, &err);
                if (err)
                    pc_free(err);
                if (!v) {
                    pthread_mutex_lock(&a->vid_mtx);
                    if (gen == a->vid_gen)
                        a->vid_decode_failed = true;
                    pthread_mutex_unlock(&a->vid_mtx);
                    break;
                }
                continue;
            }

            pthread_mutex_lock(&a->vid_mtx);
            if (gen == a->vid_gen && !a->vid_stop && a->vid_pixels) {
                memcpy(a->vid_pixels, buf, (size_t)ow * (size_t)oh * 4u);
                a->vid_fw = ow;
                a->vid_fh = oh;
                a->vid_frame_seq++;
            }
            pthread_mutex_unlock(&a->vid_mtx);
            SDL_Delay((Uint32)(1000 / (fps > 0 ? fps : 24)));
        }
        free(buf);
        buf = NULL;
        pc_video_close(v);
        v = NULL;
    }
    free(buf);
    return NULL;
}

static void app_video_clear_ui(App *a)
{
    a->video_active = false;
    a->video_playing = false;
    a->video_paused = false;
    a->video_failed = false;
    a->video_note[0] = '\0';
    a->video_frames = 0;
    a->video_uploaded_seq = 0;
    a->video_src[0] = '\0';
    if (a->video_tex) {
        SDL_DestroyTexture(a->video_tex);
        a->video_tex = NULL;
    }
    a->video_w = 0;
    a->video_h = 0;
}

static void app_video_stop(App *a)
{
    if (!a->vid_thread_started)
        return;
    pthread_mutex_lock(&a->vid_mtx);
    a->vid_stop = true;
    a->vid_gen++;
    a->video_paused = false;
    pthread_cond_broadcast(&a->vid_cv);
    pthread_mutex_unlock(&a->vid_mtx);
    app_video_clear_ui(a);
}

static void app_video_start_decode(App *a)
{
    if (!a->video_active || a->video_failed || !a->video_src[0])
        return;
    int w = a->video_target_w > 0 ? a->video_target_w : 720;
    int h = a->video_target_h > 0 ? a->video_target_h : 1280;
    if (w % 2)
        w--;
    if (h % 2)
        h--;
    pthread_mutex_lock(&a->vid_mtx);
    free(a->vid_pixels);
    a->vid_pixels = (uint8_t *)calloc((size_t)w * (size_t)h * 4u, 1);
    if (!a->vid_pixels) {
        pthread_mutex_unlock(&a->vid_mtx);
        a->video_failed = true;
        snprintf(a->video_note, sizeof a->video_note, "Video unavailable");
        return;
    }
    a->vid_fw = w;
    a->vid_fh = h;
    a->vid_frame_seq = 0;
    a->vid_decode_failed = false;
    snprintf(a->vid_src, sizeof a->vid_src, "%s", a->video_src);
    a->vid_out_w = w;
    a->vid_out_h = h;
    a->vid_fps = 24;
    a->vid_has_cmd = true;
    a->vid_stop = false;
    a->vid_gen++;
    pthread_cond_broadcast(&a->vid_cv);
    pthread_mutex_unlock(&a->vid_mtx);
}

static void app_video_begin(App *a)
{
    app_video_clear_ui(a);
    if (!a->show_detail)
        return;
#ifdef PC_ANDROID
    /* poster only: no ffmpeg decoder on the device */
    return;
#endif
    const PcPin *pin = &a->detail_pin;
    const char *src = NULL;
    if (a->video_file_override[0])
        src = a->video_file_override;
    else if (pin->is_video && pin->video_url[0])
        src = pin->video_url;
    else if (pin->is_video && !pin->video_url[0])
        src = "";
    if (!src)
        return;
    a->video_active = true;
    snprintf(a->video_src, sizeof a->video_src, "%s", src);
    /* aspect-fit decode size inside the media pane, <=720px wide, even dims */
    float pane_w = a->d_media_w > 40.0f ? a->d_media_w : 720.0f;
    float pane_h = a->d_media_h > 40.0f ? a->d_media_h : pane_w * 1.6f;
    int ow = (int)(pane_w - 2.0f);
    if (ow > 720)
        ow = 720;
    if (ow < 240)
        ow = 240;
    int vw = pin->video_w > 0 ? pin->video_w : 720;
    int vh = pin->video_h > 0 ? pin->video_h : 1280;
    int oh = (int)lroundf((float)ow * (float)vh / (float)vw);
    if (oh > pane_h) {
        oh = (int)pane_h;
        ow = (int)lroundf((float)oh * (float)vw / (float)vh);
    }
    if (ow < 2)
        ow = 2;
    if (oh < 2)
        oh = 2;
    if (ow > 720)
        ow = 720;
    if (ow % 2)
        ow--;
    if (oh % 2)
        oh--;
    a->video_target_w = ow;
    a->video_target_h = oh;
    a->video_gen = a->detail_serial;
    a->video_paused = false;
    if (!a->vid_available) {
        a->video_failed = true;
        snprintf(a->video_note, sizeof a->video_note, "Video unavailable");
        return;
    }
    if (!src[0]) {
        a->video_failed = true;
        snprintf(a->video_note, sizeof a->video_note, "Video unavailable");
        return;
    }
    if (a->settings.autoplay_video) {
        a->video_playing = true;
        app_video_start_decode(a);
    } else {
        a->video_playing = false;
    }
}

static void app_video_toggle_play(App *a)
{
#ifdef PC_ANDROID
    return;
#endif
    if (!a->video_active || a->video_failed)
        return;
    if (!a->video_playing) {
        a->video_playing = true;
        a->video_paused = false;
        app_video_start_decode(a);
        pthread_mutex_lock(&a->vid_mtx);
        pthread_cond_broadcast(&a->vid_cv);
        pthread_mutex_unlock(&a->vid_mtx);
        return;
    }
    pthread_mutex_lock(&a->vid_mtx);
    a->video_paused = !a->video_paused;
    pthread_cond_broadcast(&a->vid_cv);
    pthread_mutex_unlock(&a->vid_mtx);
}

static void app_update_video(App *a)
{
#ifdef PC_ANDROID
    return;
#endif
    if (!a->show_detail || !a->video_active || a->video_failed)
        return;
    if (!a->video_playing || a->video_paused)
        return;
    bool failed = false;
    uint64_t seq = 0;
    int fw = 0, fh = 0;
    uint8_t *copy = NULL;
    pthread_mutex_lock(&a->vid_mtx);
    failed = a->vid_decode_failed;
    seq = a->vid_frame_seq;
    fw = a->vid_fw;
    fh = a->vid_fh;
    if (seq > a->video_uploaded_seq && a->vid_pixels && fw > 0 && fh > 0) {
        size_t bytes = (size_t)fw * (size_t)fh * 4u;
        copy = (uint8_t *)malloc(bytes);
        if (copy)
            memcpy(copy, a->vid_pixels, bytes);
    }
    pthread_mutex_unlock(&a->vid_mtx);
    if (failed && a->video_frames == 0) {
        a->video_failed = true;
        snprintf(a->video_note, sizeof a->video_note, "Video unavailable");
    }
    if (!copy)
        return;
    if (!a->video_tex || a->video_w != fw || a->video_h != fh) {
        if (a->video_tex)
            SDL_DestroyTexture(a->video_tex);
        a->video_tex = SDL_CreateTexture(a->rnd.r, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, fw, fh);
        if (a->video_tex) {
            SDL_SetTextureBlendMode(a->video_tex, SDL_BLENDMODE_BLEND);
            a->video_w = fw;
            a->video_h = fh;
        }
    }
    if (a->video_tex && SDL_UpdateTexture(a->video_tex, NULL, copy,
                                          fw * 4) == 0) {
        a->video_frames++;
        a->video_uploaded_seq = seq;
    }
    free(copy);
}

/* --------------------------------------------------------------- detail */
/* Pinterest-parity close-up: contained image on the left, a scrollable info
 * column on the right (action bar, title/description, comments, related). */

static bool app_detail_canonical_id(const char *id)
{
    return id && id[0] >= '0' && id[0] <= '9';
}

static bool text_has_visible(const char *s)
{
    if (!s)
        return false;
    for (; *s; s++) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
            return true;
    }
    return false;
}

/* Displayed comment count: the loaded list is authoritative once ready; the
 * feed payload's count can be stale (0 rows + no bookmark => "No comments"). */
static int app_detail_comment_count(App *a)
{
    if (a->comments_ready && !a->comments_unavailable &&
        !a->comments_err[0])
        return (int)a->comments.count;
    return a->detail_pin.comment_count;
}

static void app_detail_clear_social(App *a)
{
    pc_commentlist_free(&a->comments);
    pc_pinlist_free(&a->related);
    for (int i = 0; i < MAX_REL; i++) {
        if (a->rel_tex[i])
            SDL_DestroyTexture(a->rel_tex[i]);
        a->rel_tex[i] = NULL;
        a->rel_tw[i] = 0;
        a->rel_th[i] = 0;
        a->rel_status[i] = 0;
    }
    a->rel_n = 0;
    a->rel_img_serial++;
    a->d_rel_cols = 0;
    a->comments_loading = false;
    a->comments_ready = false;
    a->comments_unavailable = false;
    a->comments_err[0] = '\0';
    a->related_loading = false;
    a->related_ready = false;
    a->related_unavailable = false;
    a->related_err[0] = '\0';
    a->composer_focus = false;
    a->comment_posting = false;
    a->comment_post_err[0] = '\0';
    a->comment_draft[0] = '\0';
    a->comment_draft_len = 0;
    a->detail_menu = 0;
}

/* Build the synthetic preview related list from the current feed slice. */
static void app_preview_related(App *a)
{
    size_t n = 0;
    size_t cap = 24; /* matches the net layer's 24/page */
    PcPin *items = (PcPin *)calloc(cap, sizeof(PcPin));
    if (!items)
        return;
    for (size_t i = 0; i < a->npins && n < cap; i++) {
        const PcPin *p = &a->pins[i].pin;
        if (strcmp(p->id, a->detail_pin.id) == 0)
            continue;
        items[n++] = *p;
    }
    a->related.items = items;
    a->related.count = n;
    a->related.cap = cap;
    a->related.bookmark[0] = '\0';
}

static void app_detail_load_social(App *a)
{
    if (!a->show_detail)
        return;
    bool canonical = app_detail_canonical_id(a->detail_pin.id);
    if (getenv("PC_DEBUG_DETAIL"))
        fprintf(stderr,
                "DBG load_social show=%d canonical=%d client=%d auth=%d "
                "preview=%d id=%.40s\n",
                a->show_detail ? 1 : 0, canonical ? 1 : 0,
                a->client ? 1 : 0, (int)a->auth, a->auth_preview ? 1 : 0,
                a->detail_pin.id);

    if (a->auth_preview) {
        if (a->snap_empty_comments) {
            /* simulate the stale-payload bug: count says 3, zero rows */
            a->detail_pin.comment_count = 3;
            a->comments.bookmark[0] = '\0';
        } else {
            preview_fill_comments(&a->comments, a->detail_pin.id);
            for (size_t ci = 0; ci < a->comments.count; ci++)
                app_unescape_comment(&a->comments.items[ci]);
            /* preview metadata so the action bar counts look real */
            if (a->detail_pin.comment_count < (int)a->comments.count)
                a->detail_pin.comment_count = (int)a->comments.count;
        }
        if (a->detail_pin.like_count <= 0)
            a->detail_pin.like_count = 128;
        a->detail_like_count = a->detail_pin.like_count;
        a->comments_ready = true;
        a->comments_loading = false;
        app_preview_related(a);
        a->related_ready = true;
        a->related_loading = false;
        app_request_related_images(a);
        app_detail_measure(a);
        return;
    }
    if (!canonical) {
        /* Pinterest itself cannot resolve e1_... ids on /pin/ */
        a->comments_unavailable = true;
        a->comments_ready = true;
        a->related_unavailable = true;
        a->related_ready = true;
        app_detail_measure(a);
        return;
    }
    if (!a->client) {
        a->comments_unavailable = true;
        a->related_unavailable = true;
        a->comments_ready = true;
        a->related_ready = true;
        app_detail_measure(a);
        return;
    }
    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = NET_COMMENTS;
    cmd.seq = ++a->comments_seq;
    snprintf(cmd.pin_id, sizeof cmd.pin_id, "%s", a->detail_pin.id);
    a->comments_loading = true;
    app_push_net(a, &cmd);

    memset(&cmd, 0, sizeof cmd);
    cmd.kind = NET_RELATED;
    cmd.seq = ++a->related_seq;
    snprintf(cmd.pin_id, sizeof cmd.pin_id, "%s", a->detail_pin.id);
    a->related_loading = true;
    app_push_net(a, &cmd);
    app_detail_measure(a);
}

/* Natural image height for the close-up image pane: the image scales to the
 * pane width and is capped at calc(90vh - 80px) (tall images are letterboxed
 * horizontally / scroll within the pane). */
static float app_detail_image_h(App *a)
{
    float mw = a->d_media_w > 0.0f ? a->d_media_w : 1.0f;
    float s = a->rnd.scale;
    float aspect = 1.25f;
    if (a->detail.status == 2 && a->detail.tw > 0 && a->detail.th > 0)
        aspect = (float)a->detail.th / (float)a->detail.tw;
    else if (a->detail_pin.img_w > 0 && a->detail_pin.img_h > 0)
        aspect = (float)a->detail_pin.img_h / (float)a->detail_pin.img_w;
    if (a->video_active) {
        int vw = a->detail_pin.video_w;
        int vh = a->detail_pin.video_h;
        if (a->video_w > 0 && a->video_h > 0) {
            vw = a->video_w;
            vh = a->video_h;
        }
        if (vw > 0 && vh > 0)
            aspect = (float)vh / (float)vw;
    }
    float nat = mw * aspect;
    float cap = CU_MAX_MEDIA_F * (float)a->rnd.out_h - CU_HEADER_H * s;
    if (cap < 120.0f * s)
        cap = 120.0f * s;
    return nat < cap ? nat : cap;
}

/* Related-pin aspect for the recommendation masonry (stable: pin metadata
 * first, decoded texture as a fallback; default portrait-ish 1:1.3). */
static float cu_rec_aspect(App *a, int i)
{
    int w = a->related.items[i].img_w;
    int h = a->related.items[i].img_h;
    if (w <= 0 || h <= 0) {
        w = a->rel_tw[i];
        h = a->rel_th[i];
    }
    if (w <= 0 || h <= 0)
        return 1.3f;
    float asp = (float)h / (float)w;
    if (asp < 0.4f)
        asp = 0.4f;
    if (asp > 4.0f)
        asp = 4.0f;
    return asp;
}

/* Page-content coordinates (y=0 at the header bottom) -> screen px. */
static SDL_FRect cu_screen(App *a, SDL_FRect r)
{
    r.y += a->d_header.h - a->left_scroll;
    return r;
}

static void app_detail_geom(App *a)
{
    float s = a->rnd.scale;
    float W = (float)a->rnd.out_w, H = (float)a->rnd.out_h;
    float rail = CU_RAIL_W * s, header = CU_HEADER_H * s;
    float gap = CU_GAP * s;
    float pad = CU_PAGE_PAD * s;
    const PcPin *pin = &a->detail_pin;

    a->rel_n = (int)a->related.count;
    if (a->rel_n > MAX_REL)
        a->rel_n = MAX_REL;

    int fs_section = (int)lroundf((float)CU_SECTION_SIZE * s);
    if (fs_section < 10)
        fs_section = 10;

    a->d_rail_w = rail;
    a->d_rail = (SDL_FRect){0.0f, 0.0f, rail, H};
    a->d_header = (SDL_FRect){rail, 0.0f, W - rail, header};

    /* fixed left rail: logo, 6 nav glyphs, settings at the bottom */
    {
        float bx = 12.0f * s;
        a->d_nav_btns[0] = (SDL_FRect){bx, 16.0f * s, 48.0f * s, 48.0f * s};
        for (int i = 0; i < 6; i++)
            a->d_nav_btns[i + 1] = (SDL_FRect){
                bx, (88.0f + 72.0f * (float)i) * s, 48.0f * s, 48.0f * s};
        a->d_nav_btns[7] =
            (SDL_FRect){bx, H - 64.0f * s, 48.0f * s, 48.0f * s};
    }

    /* fixed top bar: search pill + right cluster */
    a->d_search_rect =
        (SDL_FRect){rail + pad, 16.0f * s, W - rail - 112.0f * s, 48.0f * s};
    if (a->d_search_rect.w < 120.0f * s)
        a->d_search_rect.w = 120.0f * s;
    a->d_header_avatar =
        (SDL_FRect){W - 80.0f * s, 24.0f * s, 32.0f * s, 32.0f * s};
    a->d_header_chev =
        (SDL_FRect){W - 40.0f * s, 28.0f * s, 24.0f * s, 24.0f * s};

    /* fixed back button overlapping the card's top-left corner */
    a->d_back_btn = (SDL_FRect){rail + 24.0f * s, header + 8.0f * s, 48.0f * s,
                                48.0f * s};

    /* masonry column grid: N = max(3, floor((W-80)/240)), 16px gutters */
    int cols = (int)floorf((W - 80.0f * s) / (240.0f * s));
    if (cols < 3)
        cols = 3;
    if (cols > CU_MAX_COLS)
        cols = CU_MAX_COLS;
    float usable = W - rail - 2.0f * pad;
    float colw = (usable - (float)(cols - 1) * gap) / (float)cols;
    if (colw < 60.0f * s)
        colw = 60.0f * s;
    int span = cols <= 3 ? cols : (cols <= 8 ? 4 : 5);
    if (span > cols)
        span = cols;
    a->d_cols = cols;
    a->d_col_w = colw;
    a->d_span = span;

    /* close-up card: spans 4 columns (5 on very wide viewports) */
    float card_x = rail + pad;
    float card_w = (float)span * colw + (float)(span - 1) * gap;
    float inner = card_w - 2.0f * CU_CARD_BORDER * s;
    float media_w = (inner - gap) * 0.5f;
    if (media_w < 100.0f * s)
        media_w = 100.0f * s;
    a->d_media_w = media_w;
    float media_h = app_detail_image_h(a);
    a->d_media_h = media_h;
    float card_min = CU_MIN_CARD_F * H + header;
    float card_h =
        (card_min > media_h ? card_min : media_h) + 2.0f * CU_CARD_BORDER * s;
    a->d_card_w = card_w;
    a->d_card_h = card_h;
    a->d_card = (SDL_FRect){card_x, 0.0f, card_w, card_h};
    a->d_media = (SDL_FRect){
        card_x + CU_CARD_BORDER * s,
        CU_CARD_BORDER * s +
            (card_h - 2.0f * CU_CARD_BORDER * s - media_h) * 0.5f,
        media_w, media_h};
    float info_w = inner - media_w;
    a->d_info = (SDL_FRect){card_x + CU_CARD_BORDER * s + media_w,
                            CU_CARD_BORDER * s, info_w,
                            card_h - 2.0f * CU_CARD_BORDER * s};
    a->d_content_x = a->d_info.x + pad;
    a->d_content_w = a->d_info.w - 2.0f * pad;
    a->d_img_h = media_h;

#ifdef PC_ANDROID
    if (W < 760.0f * s) {
        /* portrait phone: single stacked card (media on top, info below),
         * no left rail, 44px+ action targets */
        a->d_rail_w = 0.0f;
        a->d_rail = (SDL_FRect){0.0f, 0.0f, 0.0f, H};
        for (int i = 0; i < 8; i++)
            a->d_nav_btns[i] = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
        float hh = 56.0f * s;
        a->d_header = (SDL_FRect){0.0f, 0.0f, W, hh};
        a->d_search_rect = (SDL_FRect){8.0f * s, 8.0f * s, W - 96.0f * s,
                                       40.0f * s};
        a->d_header_avatar = (SDL_FRect){W - 68.0f * s, 12.0f * s,
                                         32.0f * s, 32.0f * s};
        a->d_header_chev = (SDL_FRect){W - 36.0f * s, 16.0f * s, 24.0f * s,
                                       24.0f * s};
        a->d_back_btn = (SDL_FRect){8.0f * s, hh + 8.0f * s, 44.0f * s,
                                    44.0f * s};
        float cx = 8.0f * s;
        float cw = W - 16.0f * s;
        float iw = cw - 2.0f * CU_CARD_BORDER * s;
        float mh = media_h;
        float max_mh = H * 0.42f;
        if (mh > max_mh)
            mh = max_mh;
        if (mh < 120.0f * s)
            mh = 120.0f * s;
        float ch = H - hh;
        a->d_card_w = cw;
        a->d_card_h = ch;
        a->d_card = (SDL_FRect){cx, 0.0f, cw, ch};
        a->d_media = (SDL_FRect){cx + CU_CARD_BORDER * s,
                                 CU_CARD_BORDER * s, iw, mh};
        a->d_media_w = iw;
        a->d_media_h = mh;
        a->d_img_h = mh;
        a->d_info = (SDL_FRect){cx + CU_CARD_BORDER * s,
                                CU_CARD_BORDER * s + mh, iw,
                                ch - 2.0f * CU_CARD_BORDER * s - mh};
        a->d_content_x = a->d_info.x + pad;
        a->d_content_w = a->d_info.w - 2.0f * pad;
        a->d_act_y = a->d_media.y + a->d_media.h + 8.0f * s;
        card_h = ch;
        media_h = mh;
    }
#endif

    /* action bar: 48px round icon buttons + right-aligned board/Save cluster */
    a->d_act_y = CU_CARD_BORDER * s + 8.0f * s;
    {
        float abx = a->d_content_x - 12.0f * s;
        char cnt[32];
        int cw = 0;
        snprintf(cnt, sizeof cnt, "%d", a->detail_like_count);
        fonts_measure(a->fonts, cnt, fs_section, true, &cw, NULL);
        int clh = fonts_line_h(a->fonts, fs_section, true);
        a->d_like_btn =
            (SDL_FRect){abx, a->d_act_y, CU_ACTION_H * s, CU_ACTION_H * s};
        a->d_like_count = (SDL_FRect){
            abx + CU_ACTION_H * s, a->d_act_y +
                                       (CU_ACTION_H * s - (float)clh) * 0.5f,
            (float)cw, (float)clh};
        float bx = a->d_like_count.x + (float)cw + 16.0f * s;
        a->d_comment_btn =
            (SDL_FRect){bx, a->d_act_y, CU_ACTION_H * s, CU_ACTION_H * s};
        bx += CU_ACTION_H * s + 8.0f * s;
        a->d_share_btn =
            (SDL_FRect){bx, a->d_act_y, CU_ACTION_H * s, CU_ACTION_H * s};
        bx += CU_ACTION_H * s + 8.0f * s;
        a->d_more_btn =
            (SDL_FRect){bx, a->d_act_y, CU_ACTION_H * s, CU_ACTION_H * s};

        /* Save pill: 16px/500 label + 16px horizontal padding, min 68.5px */
        int fsv = (int)lroundf(16.0f * s);
        int sw = 0;
        fonts_measure(a->fonts, "Save", fsv, false, &sw, NULL);
        float save_w = (float)sw + 32.0f * s;
        if (save_w < CU_SAVE_W * s)
            save_w = CU_SAVE_W * s;
        float save_r = a->d_content_x + a->d_content_w;
        a->d_save_btn =
            (SDL_FRect){save_r - save_w, a->d_act_y, save_w, CU_ACTION_H * s};
        a->d_save.x = (int)a->d_save_btn.x;
        a->d_save.y = (int)a->d_save_btn.y;
        a->d_save.w = (int)a->d_save_btn.w;
        a->d_save.h = (int)a->d_save_btn.h;

        /* board dropdown: board name (or author/domain) + chevron, right
         * aligned against the Save pill (Pinterest: plain 16px/700 label). */
        const char *board = pin->board_name[0]
                                ? pin->board_name
                                : (pin->pinner_name[0]
                                       ? pin->pinner_name
                                       : (pin->domain[0] ? pin->domain
                                                         : "Profile"));
        /* width is capped by the room left between the action icons and the
         * Save pill, so a long board name never overlaps the icon row */
        float action_end = a->d_more_btn.x + CU_ACTION_H * s;
        float avail = a->d_save_btn.x - 4.0f * s - action_end;
        if (avail < 60.0f * s)
            avail = 60.0f * s;
        float maxw = CU_DROPDOWN_MAX * s;
        if (maxw > avail)
            maxw = avail;
        char label[PC_BOARD_NAME_LEN + 8];
        fonts_ellipsize(a->fonts, board, fsv, true, maxw - 40.0f * s, label,
                        sizeof label);
        int lw = 0;
        fonts_measure(a->fonts, label, fsv, true, &lw, NULL);
        float dw = (float)lw + 40.0f * s;
        if (dw > maxw)
            dw = maxw;
        if (dw < 60.0f * s)
            dw = 60.0f * s;
        a->d_dropdown = (SDL_FRect){a->d_save_btn.x - 4.0f * s - dw,
                                    a->d_act_y, dw, CU_ACTION_H * s};
    }

    /* creator row + (in measure) title/description/divider */
    a->d_creator_y = a->d_act_y + CU_ACTION_H * s + 16.0f * s;
    a->d_creator = (SDL_FRect){a->d_content_x, a->d_creator_y, a->d_content_w,
                               CU_AVATAR * s};

    /* sticky composer pill (47px tall, 12px above the card bottom edge) */
    float comp_y =
        card_h - CU_CARD_BORDER * s - 12.0f * s - CU_COMPOSER_H * s;
    a->d_composer = (SDL_FRect){a->d_content_x, comp_y, a->d_content_w,
                                CU_COMPOSER_H * s};
    a->d_composer_y = comp_y;
    {
        int fsp = (int)lroundf(13.0f * s);
        if (a->comment_draft_len > 0) {
            int pw = 0;
            fonts_measure(a->fonts, "Post", fsp, true, &pw, NULL);
            float pbw = (float)pw + 24.0f * s;
            if (pbw < 52.0f * s)
                pbw = 52.0f * s;
            a->d_post_btn =
                (SDL_FRect){a->d_composer.x + a->d_composer.w - 8.0f * s - pbw,
                            comp_y + (CU_COMPOSER_H * s - 33.0f * s) * 0.5f,
                            pbw, 33.0f * s};
        } else {
            a->d_post_btn = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
        }
    }

    /* two circular overlay buttons at the image's bottom-right corner */
    a->d_overlay_btn[0] = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    a->d_overlay_btn[1] = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    a->d_video_btn = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    if (a->video_active) {
        a->d_video_btn = (SDL_FRect){
            a->d_media.x + 12.0f * s,
            a->d_media.y + a->d_media.h - 12.0f * s - 48.0f * s,
            48.0f * s, 48.0f * s};
    }
    if (media_h > 200.0f * s) {
        float bs = 48.0f * s;
        float bx = a->d_media.x + a->d_media.w - 12.0f * s - bs;
        float by = a->d_media.y + a->d_media.h - 12.0f * s - bs;
        a->d_overlay_btn[0] = (SDL_FRect){bx, by, bs, bs};
        a->d_overlay_btn[1] = (SDL_FRect){bx, by - bs - 12.0f * s, bs, bs};
    }

    /* ---- page masonry: card (span cols) + related recs + ideas tile ---- */
    {
        float col_h[CU_MAX_COLS];
        for (int c = 0; c < cols; c++)
            col_h[c] = 0.0f;
        for (int c = 0; c < span; c++)
            col_h[c] = card_h + gap;

        int rel_n = a->related_unavailable ? 0 : a->rel_n;
        if (rel_n > MAX_REL)
            rel_n = MAX_REL;
        float idea_row_h = colw * CU_IDEAS_RATIO;
        float ideas_h = (float)fonts_line_h(a->fonts, fs_section, true) +
                        4.0f * s + 2.0f * (idea_row_h + 8.0f * s);
        a->d_idea_row_h = idea_row_h;
        a->d_ideas = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
        a->d_rec_n = 0;
        bool ideas_done = false;
        for (int i = 0; i < rel_n; i++) {
            int c = 0;
            for (int k = 1; k < cols; k++)
                if (col_h[k] < col_h[c])
                    c = k;
            float th = colw * cu_rec_aspect(a, i);
            if (th < 60.0f * s)
                th = 60.0f * s;
            a->d_rel_tiles[i] = (SDL_FRect){
                card_x + (float)c * (colw + gap), col_h[c], colw, th};
            col_h[c] += th + gap;
            a->d_rec_n++;
            if (!ideas_done && rel_n >= 2 && i == 4) {
                int ic = 0;
                for (int k = 1; k < cols; k++)
                    if (col_h[k] < col_h[ic])
                        ic = k;
                a->d_ideas = (SDL_FRect){card_x + (float)ic * (colw + gap),
                                         col_h[ic], colw, ideas_h};
                col_h[ic] += ideas_h + gap;
                ideas_done = true;
            }
        }
        if (!ideas_done && rel_n >= 2) {
            int ic = 0;
            for (int k = 1; k < cols; k++)
                if (col_h[k] < col_h[ic])
                    ic = k;
            a->d_ideas = (SDL_FRect){card_x + (float)ic * (colw + gap),
                                     col_h[ic], colw, ideas_h};
            col_h[ic] += ideas_h + gap;
        }
        if (a->d_ideas.w > 0.0f) {
            float line = (float)fonts_line_h(a->fonts, fs_section, true);
            for (int r = 0; r < 2; r++) {
                a->d_idea_rows[r] =
                    (SDL_FRect){a->d_ideas.x,
                                a->d_ideas.y + line + 4.0f * s +
                                    (float)r * (idea_row_h + 8.0f * s) +
                                    4.0f * s,
                                colw, idea_row_h};
                int idx = rel_n - 1 - r;
                if (idx < 0)
                    idx = 0;
                a->d_idea_pin[r] = idx;
                const PcPin *rp = &a->related.items[idx];
                const char *lab = rp->title[0]
                                      ? rp->title
                                      : (rp->board_name[0] ? rp->board_name
                                                           : "Ideas for you");
                fonts_ellipsize(a->fonts, lab, fs_section, true,
                                colw - 24.0f * s, a->d_idea_labels[r],
                                sizeof a->d_idea_labels[r]);
            }
        }

        float bottom = 0.0f;
        for (int c = 0; c < cols; c++)
            if (col_h[c] > bottom)
                bottom = col_h[c];
        if (bottom > gap)
            bottom -= gap;
        a->left_content_h = bottom + 32.0f * s;
        a->left_view_h = H - header;
        if (a->left_content_h < a->left_view_h)
            a->left_content_h = a->left_view_h;
        a->d_rel_y = card_h + gap;

        /* page scrollbar track (fixed, below the header) */
        a->d_left_sb_track =
            (SDL_FRect){W - 12.0f * s, header, 6.0f * s, H - header};
    }
}

static void app_detail_measure(App *a)
{
#ifdef PC_ANDROID
    app_mobile_measure(a);
    return;
#endif
    if (!a->show_detail) {
        a->detail_content_h = 0.0f;
        a->left_content_h = 0.0f;
        return;
    }
    app_detail_geom(a);

    float s = a->rnd.scale;
    const PcPin *pin = &a->detail_pin;
    int fs_title = (int)lroundf((float)CU_TITLE_SIZE * s);
    int fs_section = (int)lroundf((float)CU_SECTION_SIZE * s);
    int fs_body = (int)lroundf((float)CU_BODY_SIZE * s);
    if (fs_title < 12)
        fs_title = 12;
    if (fs_section < 10)
        fs_section = 10;
    if (fs_body < 10)
        fs_body = 10;
    float title_line = (float)CU_TITLE_LINE * s;
    float section_line = (float)CU_SECTION_LINE * s;
    float body_line = 22.0f * s;
    float w = a->d_content_w;
    if (w < 60.0f * s)
        w = 60.0f * s;

    /* ---- title (24px/700, 2 lines max) ---- */
    const char *title = pin->title[0]
                            ? pin->title
                            : (pin->board_name[0] ? pin->board_name
                                                  : "Untitled pin");
    a->d_title_n = fonts_wrap(a->fonts, title, fs_title, true, w, a->d_title,
                              2);
    a->d_title_y = a->d_creator_y + CU_AVATAR * s + 23.0f * s;
    float y = a->d_title_y + (float)a->d_title_n * title_line;

    /* ---- description: "Description" h4 + 16px/400 body, 6 lines shown --- */
    const char *desc = pin->description;
    bool has_desc = text_has_visible(desc);
    a->d_desc_n = 0;
    a->d_desc_more = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    a->d_desc_y = y + 20.0f * s;
    if (has_desc) {
        a->d_desc_n = fonts_wrap(a->fonts, desc, fs_body, false, w, a->d_desc,
                                 20);
        float text_top = a->d_desc_y + section_line + 4.0f * s;
        float avail =
            a->d_composer.y - 8.0f * s - text_top - 24.0f * s - section_line;
        int max_show = (int)(avail / body_line);
        if (max_show < 1)
            max_show = 1;
        if (max_show > 6)
            max_show = 6;
        int show = a->desc_expanded ? a->d_desc_n : max_show;
        if (show > a->d_desc_n)
            show = a->d_desc_n;
        y = text_top + (float)show * body_line;
        if (a->d_desc_n > show) {
            int sz = a->M.small_size;
            int lh = fonts_line_h(a->fonts, sz, true);
            const char *label = a->desc_expanded ? "less" : "more";
            int tw = 0;
            fonts_measure(a->fonts, label, sz, true, &tw, NULL);
            a->d_desc_more = (SDL_FRect){a->d_content_x, y + 2.0f * s,
                                         (float)tw, (float)lh};
            y += (float)lh + 4.0f * s;
        }
        y += 8.0f * s;
    }
    a->d_divider_y = y + 24.0f * s;

    /* ---- comments region (independent scroll inside the card) ---- */
    a->d_comments_top = a->d_divider_y + 16.0f * s;
    a->d_comments_bot = a->d_composer.y - 8.0f * s;
    a->detail_view_h = a->d_comments_bot - a->d_comments_top;
    if (a->detail_view_h < CU_MIN_REGION * s)
        a->detail_view_h = CU_MIN_REGION * s;
    if (a->detail_scroll < 0.0f)
        a->detail_scroll = 0.0f;

    int fs_meta = a->M.small_size;
    float cy = section_line + 16.0f * s; /* heading + gap before first row */
    if (a->comments_unavailable) {
        cy += (float)fonts_line_h(a->fonts, fs_meta, false) + 8.0f * s;
    } else if (a->comments_loading) {
        cy += 44.0f * s;
    } else if (a->comments_err[0]) {
        char lines[2][FONT_LINE_MAX];
        int nl = fonts_wrap(a->fonts, a->comments_err, fs_meta, false, w,
                            lines, 2);
        cy += (float)nl * (float)fonts_line_h(a->fonts, fs_meta, false) +
              8.0f * s;
    } else if (a->comments.count == 0) {
        cy += (float)fonts_line_h(a->fonts, fs_meta, false) + 8.0f * s;
    } else {
        int fs_author = fs_section;
        for (size_t i = 0; i < a->comments.count; i++) {
            PcComment *cm = &a->comments.items[i];
            char lines[3][FONT_LINE_MAX];
            int nl = fonts_wrap(a->fonts, cm->text, fs_body, false,
                                w - 40.0f * s, lines, 3);
            int nlines = fonts_line_h(a->fonts, fs_author, true);
            float name_w = 0.0f;
            int nw = 0;
            fonts_measure(a->fonts, cm->user_name, fs_author, true, &nw, NULL);
            name_w = (float)nw;
            bool inline_one =
                nl == 1 && name_w + 6.0f * s +
                               (float)fonts_line_h(a->fonts, fs_body, false) <
                               w - 40.0f * s;
            float text_h = (float)nl * (float)CU_COMMENT_LINE * s;
            float row_h = (float)nlines + text_h + 12.0f * s +
                          (float)fonts_line_h(a->fonts, fs_meta, false) +
                          6.0f * s;
            if (!inline_one)
                row_h += 0.0f;
            if (row_h < 56.0f * s)
                row_h = 56.0f * s;
            cy += row_h;
        }
        if (a->comments.bookmark[0] && a->comments_ready) {
            cy += 8.0f * s;
            a->d_viewmore_btn = (SDL_FRect){
                a->d_content_x, a->d_comments_top + cy, 200.0f * s, 32.0f * s};
            cy += 40.0f * s;
        } else {
            a->d_viewmore_btn = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
        }
    }
    a->detail_content_h = cy + 12.0f * s;
    a->d_comments_y = a->d_comments_top;
    {
        float W = (float)a->rnd.out_w;
        (void)W;
        a->d_sb_track = (SDL_FRect){a->d_info.x + a->d_info.w - 10.0f * s,
                                    a->d_comments_top, 6.0f * s,
                                    a->detail_view_h};
    }
    app_detail_geom(a);
}

static void app_detail_wrap(App *a)
{
    app_detail_measure(a);
}

static void app_request_related_images(App *a)
{
    if (!a->client || !a->show_detail || a->detail_menu)
        return;
    app_detail_measure(a);
    float s = a->rnd.scale;
    int max_w = (int)(a->d_col_w + 0.5f);
    if (max_w < (int)(120.0f * s))
        max_w = (int)(120.0f * s);
    pthread_mutex_lock(&a->img_mtx);
    int space = MAX_REQS - a->req_count;
    for (int i = 0; i < a->rel_n && i < MAX_REL && space > 0; i++) {
        if (a->rel_status[i] != 0)
            continue;
        const char *url = a->related.items[i].thumb_url;
        if (!url[0]) {
            a->rel_status[i] = 3;
            continue;
        }
        ImgReq *r = &a->reqs[(a->req_head + a->req_count) % MAX_REQS];
        memset(r, 0, sizeof *r);
        r->target = IMG_RELATED;
        r->gen = a->rel_img_serial;
        r->idx = i;
        snprintf(r->url, sizeof r->url, "%s", url);
        r->max_w = max_w;
        a->req_count++;
        a->rel_status[i] = 1;
        space--;
    }
    bool wake = a->req_count > 0;
    pthread_mutex_unlock(&a->img_mtx);
    if (wake)
        pthread_cond_broadcast(&a->img_cv);
}

static void app_detail_reset_for_pin(App *a, const PcPin *pin, bool external,
                                     int feed_idx)
{
    app_video_stop(a);
    if (a->detail.tex)
        SDL_DestroyTexture(a->detail.tex);
    memset(&a->detail, 0, sizeof a->detail);
    a->detail.status = 0;
    a->detail_serial++;
    a->detail_pin = *pin;
    a->detail_external = external;
    a->detail_feed_idx = feed_idx;
    a->detail_idx = feed_idx;
    a->desc_expanded = false;
    a->detail_scroll = 0.0f;
    a->detail_scroll_target = 0.0f;
    a->left_scroll = 0.0f;
    a->left_scroll_target = 0.0f;
    a->d_rec_n = 0;
    a->d_hover_rec = -1;
    a->d_ideas = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    a->d_idea_rows[0] = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    a->d_idea_rows[1] = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    a->d_card_h = 0.0f;
    a->d_media_h = 0.0f;
    a->d_left_sb_thumb = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    a->d_sb_thumb = (SDL_FRect){0.0f, 0.0f, 0.0f, 0.0f};
    a->rel_loading_more = false;
    a->detail_focus_side = 0;
    a->detail_menu = 0;
    a->detail_liked = pin->liked;
    a->detail_like_count = pin->like_count;
    a->like_inflight = false;
    a->detail_drag_sb = false;
    app_detail_clear_social(a);
    app_detail_geom(a);
    app_detail_measure(a);
    app_queue_detail_image(a);
    app_detail_load_social(a);
    app_video_begin(a);
}

static void app_open_detail(App *a, int idx)
{
    if (idx < 0 || (size_t)idx >= a->npins)
        return;
    bool external = a->detail_external;
    (void)external;
    a->show_detail = true;
    a->search_focus = false;
    app_detail_reset_for_pin(a, &a->pins[idx].pin, false, idx);

    /* opportunistic enrichment (guest sessions usually cannot fetch detail) */
    if (a->client && app_detail_canonical_id(a->detail_pin.id)) {
        NetCmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = NET_DETAIL;
        cmd.seq = ++a->net_detail_seq;
        snprintf(cmd.pin_id, sizeof cmd.pin_id, "%s", a->detail_pin.id);
        app_push_net(a, &cmd);
    }
}

static void app_open_related_pin(App *a, const PcPin *pin)
{
    if (!pin || !a->show_detail)
        return;
    app_detail_reset_for_pin(a, pin, true, -1);
}

static void app_close_detail(App *a)
{
    if (!a->show_detail)
        return;
    app_video_stop(a);
    a->comments_sheet = false;
    a->show_detail = false;
    a->detail_serial++;
    a->rel_img_serial++;
    if (a->detail.tex)
        SDL_DestroyTexture(a->detail.tex);
    memset(&a->detail, 0, sizeof a->detail);
    app_detail_clear_social(a);
    a->detail_idx = -1;
    a->detail_external = false;
    a->detail_feed_idx = -1;
    a->detail_menu = 0;
}

static void app_detail_like_toggle(App *a)
{
    if (!a->show_detail)
        return;
    if (!a->auth_preview &&
        !app_detail_canonical_id(a->detail_pin.id)) {
        snprintf(a->toast, sizeof a->toast,
                 "Likes unavailable for this pin");
        a->toast_until = SDL_GetTicks() + 2200;
        return;
    }
    if (!writes_enabled(a)) {
        snprintf(a->toast, sizeof a->toast, "Sign in to like");
        a->toast_until = SDL_GetTicks() + 2000;
        return;
    }
    if (a->like_inflight)
        return;
    a->like_revert_liked = a->detail_liked;
    a->like_revert_count = a->detail_like_count;
    a->detail_liked = !a->detail_liked;
    if (a->detail_liked)
        a->detail_like_count++;
    else if (a->detail_like_count > 0)
        a->detail_like_count--;

    if (a->auth_preview) {
        snprintf(a->toast, sizeof a->toast, "%s",
                 a->detail_liked ? "Liked (preview)" : "Like removed (preview)");
        a->toast_until = SDL_GetTicks() + 1600;
        return;
    }
    if (!a->client)
        return;
    a->like_inflight = true;
    a->like_pending_value = a->detail_liked;
    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = NET_LIKE;
    cmd.seq = ++a->like_seq;
    cmd.liked = a->detail_liked;
    snprintf(cmd.pin_id, sizeof cmd.pin_id, "%s", a->detail_pin.id);
    app_push_net(a, &cmd);
}

static void app_detail_append_comment(App *a, const char *text)
{
    PcComment *items = (PcComment *)realloc(
        a->comments.items, (a->comments.count + 1) * sizeof(PcComment));
    if (!items)
        return;
    PcComment *c = &items[a->comments.count];
    memset(c, 0, sizeof *c);
    snprintf(c->id, sizeof c->id, "local%zu", a->comments.count);
    snprintf(c->user_name, sizeof c->user_name, "you");
    snprintf(c->text, sizeof c->text, "%s", text);
    snprintf(c->created_at, sizeof c->created_at, "now");
    a->comments.items = items;
    a->comments.count++;
    a->comments.cap = a->comments.count;
}

static void app_detail_post_comment(App *a)
{
    if (!a->show_detail)
        return;
    if (a->comment_posting)
        return;
    a->comment_post_err[0] = '\0';
    char text[PC_COMMENT_TEXT_LEN];
    snprintf(text, sizeof text, "%s", a->comment_draft);
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\n' ||
                     text[n - 1] == '\t'))
        text[--n] = '\0';
    size_t st = 0;
    while (text[st] == ' ' || text[st] == '\t')
        st++;
    if (!text[st]) {
        snprintf(a->comment_post_err, sizeof a->comment_post_err,
                 "Write something first");
        return;
    }
    if (!a->auth_preview &&
        !app_detail_canonical_id(a->detail_pin.id)) {
        snprintf(a->comment_post_err, sizeof a->comment_post_err,
                 "Comments unavailable for this pin");
        return;
    }
    if (!writes_enabled(a)) {
        snprintf(a->comment_post_err, sizeof a->comment_post_err,
                 "Sign in to comment");
        return;
    }
    if (a->auth_preview) {
        a->detail_pin.comment_count++;
        app_detail_append_comment(a, text + st);
        a->comment_draft[0] = '\0';
        a->comment_draft_len = 0;
        a->composer_focus = false;
        snprintf(a->toast, sizeof a->toast, "Comment posted (preview)");
        a->toast_until = SDL_GetTicks() + 2000;
        app_detail_measure(a);
        return;
    }
    snprintf(a->pending_comment, sizeof a->pending_comment, "%s",
             text + st);
    a->comment_posting = true;
    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = NET_ADD_COMMENT;
    cmd.seq = ++a->comment_seq;
    snprintf(cmd.pin_id, sizeof cmd.pin_id, "%s", a->detail_pin.id);
    snprintf(cmd.text, sizeof cmd.text, "%s", text + st);
    app_push_net(a, &cmd);
}

/* ---------------------------------------------------------------- frame */
static void app_update_hover(App *a)
{
    a->hover = -1;
    a->hover_in_img = false;
    if (a->show_detail)
        return;
    if (a->my < content_top(a) || a->my > content_bottom(a))
        return;
    int idx = layout_hit(&a->layout, (float)a->mx, (float)a->my, a->scroll);
    if (idx < 0 || (size_t)idx >= a->npins || (size_t)idx >= a->layout.count)
        return;
    a->hover = idx;
    const LayoutItem *it = &a->layout.items[idx];
    a->hover_in_img =
        ((float)a->my + a->scroll) <= it->y + it->img_h;
}

static void app_update(App *a)
{
#ifdef PC_ANDROID
    if (a->touch.active) {
        float ui_scale = a->rnd.ui_scale > 0.0f ? a->rnd.ui_scale : 1.0f;
        float tlx = 0.0f, tly = 0.0f;
        if (pf_android_touch_longpress_tick(&a->touch, SDL_GetTicks(),
                                            ui_scale, &tlx, &tly)) {
            a->mx = (int)tlx;
            a->my = (int)tly;
            app_update_hover(a);
            if (!a->show_detail && !a->picker.open && !a->show_settings &&
                a->hover >= 0 && (size_t)a->hover < a->npins)
                app_open_picker(a, a->hover);
        }
    }
    {
        bool want = a->search_focus || a->composer_focus ||
                    (a->picker.open && a->picker.create_focus);
        SDL_FRect lr = {0.0f, 0.0f, 10.0f, 10.0f};
        if (a->search_focus) {
            lr = a->mobile_search ? a->mob_search_field : search_rect(a);
        } else if (a->picker.open && a->picker.create_focus) {
            lr = a->mob_save_create.w > 0 ? a->mob_save_create
                                          : a->picker.create_box;
        } else if (a->comments_sheet) {
            lr = a->mob_sheet_composer;
        }
        pf_android_update_ime(a->win, want, lr, &a->ime_active);
    }
    if (a->fling_active) {
        float dt = a->dt;
        if (dt > 0.2f)
            dt = 0.2f;
        if (dt > 0.0f) {
            float decay = powf(0.95f, dt * 60.0f);
            a->fling_v *= decay;
            float step = a->fling_v * dt;
            bool stop = false;
            if (a->fling_axis == 1) {
                float lmax = a->left_content_h - a->left_view_h;
                if (lmax < 0.0f)
                    lmax = 0.0f;
                a->left_scroll += step;
                if (a->left_scroll < 0.0f) {
                    a->left_scroll = 0.0f;
                    stop = true;
                } else if (a->left_scroll > lmax) {
                    a->left_scroll = lmax;
                    stop = true;
                }
                a->left_scroll_target = a->left_scroll;
            } else if (a->fling_axis == 0) {
                float ms = max_scroll(a);
                a->scroll += step;
                if (a->scroll < 0.0f) {
                    a->scroll = 0.0f;
                    stop = true;
                } else if (a->scroll > ms) {
                    a->scroll = ms;
                    stop = true;
                }
                a->scroll_target = a->scroll;
            } else {
                stop = true;
            }
            if (fabsf(a->fling_v) < 35.0f)
                stop = true;
            if (stop) {
                a->fling_active = false;
                a->fling_v = 0.0f;
                SDL_Log("[fling] stop");
            }
        }
    }
#endif
    if (a->show_detail && a->video_active)
        app_update_video(a);
    if (a->show_detail) {
        float dmax = a->detail_content_h - a->detail_view_h;
        if (dmax < 0.0f)
            dmax = 0.0f;
        if (a->detail_scroll_target < 0.0f)
            a->detail_scroll_target = 0.0f;
        if (a->detail_scroll_target > dmax)
            a->detail_scroll_target = dmax;
        if (!a->detail_drag_sb) {
            float k = 1.0f - expf(-16.0f * a->dt);
            a->detail_scroll +=
                (a->detail_scroll_target - a->detail_scroll) * k;
            if (fabsf(a->detail_scroll_target - a->detail_scroll) < 0.3f)
                a->detail_scroll = a->detail_scroll_target;
        }
        if (a->detail_scroll < 0.0f)
            a->detail_scroll = 0.0f;
        if (a->detail_scroll > dmax)
            a->detail_scroll = dmax;

        /* left column scrolls independently */
        float lmax = a->left_content_h - a->left_view_h;
        if (lmax < 0.0f)
            lmax = 0.0f;
        if (a->left_scroll_target < 0.0f)
            a->left_scroll_target = 0.0f;
        if (a->left_scroll_target > lmax)
            a->left_scroll_target = lmax;
        if (!a->left_drag_sb) {
            float k2 = 1.0f - expf(-16.0f * a->dt);
            a->left_scroll +=
                (a->left_scroll_target - a->left_scroll) * k2;
            if (fabsf(a->left_scroll_target - a->left_scroll) < 0.3f)
                a->left_scroll = a->left_scroll_target;
        }
        if (a->left_scroll < 0.0f)
            a->left_scroll = 0.0f;
        if (a->left_scroll > lmax)
            a->left_scroll = lmax;

        /* related "View more" / infinite load near the bottom */
        if (!a->snapshot && !a->auth_preview && a->client &&
            a->related_ready && a->related.bookmark[0] &&
            !a->rel_loading_more && a->detail_menu == 0 &&
            lmax > 0.0f &&
            a->left_scroll > lmax - 320.0f * a->rnd.scale) {
            NetCmd cmd;
            memset(&cmd, 0, sizeof cmd);
            cmd.kind = NET_RELATED;
            cmd.seq = ++a->related_seq;
            cmd.append = true;
            snprintf(cmd.pin_id, sizeof cmd.pin_id, "%s",
                     a->detail_pin.id);
            snprintf(cmd.bookmark, sizeof cmd.bookmark, "%s",
                     a->related.bookmark);
            a->rel_loading_more = true;
            app_push_net(a, &cmd);
        }
        if (!a->auth_preview && a->rel_n > 0) {
            for (int i = 0; i < a->rel_n; i++)
                if (a->rel_status[i] == 0) {
                    app_request_related_images(a);
                    break;
                }
        }
    }

    float ms = max_scroll(a);
    if (a->scroll_target > ms)
        a->scroll_target = ms;
    if (a->scroll_target < 0.0f)
        a->scroll_target = 0.0f;

    if (!a->dragging_scrollbar) {
        float k = 1.0f - expf(-15.0f * a->dt);
        a->scroll += (a->scroll_target - a->scroll) * k;
        if (fabsf(a->scroll_target - a->scroll) < 0.35f)
            a->scroll = a->scroll_target;
    }
    if (a->scroll < 0.0f)
        a->scroll = 0.0f;
    if (a->scroll > ms)
        a->scroll = ms;

    app_evict_textures(a);
    app_request_images(a);
    app_recount_textures(a);
    app_update_stats(a);

    app_update_hover(a);

    /* eased hover overlay */
    for (size_t i = 0; i < a->npins; i++) {
        float target =
            (a->hover == (int)i && a->hover_in_img && !a->show_detail) ? 1.0f
                                                                       : 0.0f;
        float k = 1.0f - expf(-18.0f * a->dt);
        a->pins[i].hover += (target - a->pins[i].hover) * k;
        if (a->pins[i].hover < 0.002f)
            a->pins[i].hover = 0.0f;
    }

    /* caret blink */
    Uint32 now = SDL_GetTicks();
    if (a->search_focus) {
        if (now - a->caret_last >= 520) {
            a->caret_last = now;
            a->caret_on = !a->caret_on;
        }
    } else {
        a->caret_on = false;
    }

    app_maybe_load_more(a);
    app_update_cursor(a);
}

/* -------------------------------------------------------------- pointers */
static void app_update_cursor(App *a)
{
    int want = SDL_SYSTEM_CURSOR_ARROW;
    if (a->show_settings) {
        app_settings_geom(a);
        bool hit = point_in_f(a->settings_close, (float)a->mx,
                              (float)a->my) ||
                   point_in_f(a->settings_toggle, (float)a->mx,
                              (float)a->my) ||
                   point_in_f(a->settings_ads_toggle, (float)a->mx,
                              (float)a->my);
        for (int row = 0; row < 3 && !hit; row++)
            for (int i = 0; i < a->settings_opt_n[row] && !hit; i++)
                if (point_in_f(a->settings_opt[row][i], (float)a->mx,
                               (float)a->my))
                    hit = true;
        want = hit ? SDL_SYSTEM_CURSOR_HAND : SDL_SYSTEM_CURSOR_ARROW;
    } else if (a->picker.open) {
        want = SDL_SYSTEM_CURSOR_ARROW;
    } else if (a->show_detail) {
        float mx = (float)a->mx, my = (float)a->my;
        bool hand = point_in_f(a->d_back_btn, mx, my) ||
                    point_in_f(a->d_header_avatar, mx, my) ||
                    point_in_f(a->d_header_chev, mx, my) ||
                    point_in_f(a->d_search_rect, mx, my) ||
                    point_in_f(cu_search_lens(a), mx, my) ||
                    point_in_f(cu_search_mic(a), mx, my);
        for (int i = 0; i < 8 && !hand; i++)
            hand = a->d_nav_btns[i].w > 0.0f &&
                   point_in_f(a->d_nav_btns[i], mx, my);
        if (!hand && a->d_left_sb_thumb.h > 0.0f)
            hand = point_in_f(a->d_left_sb_thumb, mx, my);
        if (!hand && a->d_sb_thumb.h > 0.0f)
            hand = point_in_f(a->d_sb_thumb, mx, my);
        if (!hand)
            hand = point_in_f(cu_screen(a, a->d_like_btn), mx, my) ||
                   point_in_f(cu_screen(a, a->d_comment_btn), mx, my) ||
                   point_in_f(cu_screen(a, a->d_share_btn), mx, my) ||
                   point_in_f(cu_screen(a, a->d_more_btn), mx, my) ||
                   point_in_f(cu_screen(a, a->d_dropdown), mx, my) ||
                   (a->d_save.w > 0 &&
                    point_in_f(cu_screen(a, a->d_save_btn), mx, my)) ||
                   (a->d_desc_more.w > 0.0f &&
                    point_in_f(cu_screen(a, a->d_desc_more), mx, my)) ||
                   (a->d_post_btn.w > 0.0f &&
                    point_in_f(cu_screen(a, a->d_post_btn), mx, my));
        if (!hand && a->video_active && a->d_video_btn.w > 0.0f)
            hand = point_in_f(cu_screen(a, a->d_video_btn), mx, my);
        if (!hand && a->d_viewmore_btn.w > 0.0f) {
            SDL_FRect vm = a->d_viewmore_btn;
            vm.y += a->d_header.h - a->left_scroll - a->detail_scroll;
            hand = point_in_f(vm, mx, my);
        }
        if (!hand)
            for (int k = 0; k < 2 && a->d_ideas.w > 0.0f; k++)
                hand = point_in_f(cu_screen(a, a->d_idea_rows[k]), mx, my);
        if (!hand)
            for (int i = 0; i < a->d_rec_n && i < MAX_REL; i++)
                if (point_in_f(cu_screen(a, a->d_rel_tiles[i]), mx, my)) {
                    hand = true;
                    break;
                }
        if (hand)
            want = SDL_SYSTEM_CURSOR_HAND;
        else if (point_in_f(a->d_search_rect, mx, my) ||
                 point_in_f(cu_screen(a, a->d_composer), mx, my))
            want = SDL_SYSTEM_CURSOR_IBEAM;
    } else if (a->state == VIEW_ERROR) {
    } else if (a->state == VIEW_ERROR) {    } else if (a->state == VIEW_ERROR) {
        want = SDL_SYSTEM_CURSOR_HAND;
    } else {
        SDL_FRect sr = search_rect(a);
        if (point_in_f(sr, (float)a->mx, (float)a->my))
            want = SDL_SYSTEM_CURSOR_IBEAM;
        else if (a->gear_btn.w > 0 &&
                 point_in(a->gear_btn, (float)a->mx, (float)a->my))
            want = SDL_SYSTEM_CURSOR_HAND;
        else if (a->home_btn.w > 0 &&
                 point_in(a->home_btn, (float)a->mx, (float)a->my))
            want = SDL_SYSTEM_CURSOR_HAND;
        else if (a->hover >= 0)
            want = SDL_SYSTEM_CURSOR_HAND;
        else if (a->error[0] && point_in(a->banner_rect, (float)a->mx,
                                         (float)a->my))
            want = SDL_SYSTEM_CURSOR_HAND;
    }
    if (want == a->cur_kind)
        return;
    a->cur_kind = want;
    SDL_Cursor *c = a->cur_arrow;
    if (want == SDL_SYSTEM_CURSOR_HAND)
        c = a->cur_hand;
    else if (want == SDL_SYSTEM_CURSOR_IBEAM)
        c = a->cur_ibeam;
    if (c)
        SDL_SetCursor(c);
}

/* ---------------------------------------------------------- settings ---
 * Modal panel (scrim + white rounded panel) for the image-cache policy,
 * RAM budget, texture keep band and the stats overlay toggle. Applies live
 * through pc_image_set_cache() and persists to settings.conf.
 */
static const int SETTINGS_BUDGETS[4] = {16, 32, 64, 128};
static const int SETTINGS_KEEPS[4] = {1, 2, 3, 5};

static int budget_index(int mb)
{
    for (int i = 0; i < 4; i++)
        if (SETTINGS_BUDGETS[i] == mb)
            return i;
    return 2;
}

static int keep_index(int k)
{
    for (int i = 0; i < 4; i++)
        if (SETTINGS_KEEPS[i] == k)
            return i;
    return 1;
}

static void app_settings_geom(App *a)
{
    float s = a->rnd.scale;
    float W = (float)a->rnd.out_w, H = (float)a->rnd.out_h;
    float pw = fminf(620.0f * s, W - 80.0f * s);
    if (pw < 380.0f * s)
        pw = W - 40.0f * s;
    if (pw < 240.0f * s)
        pw = 240.0f * s;
#ifdef PC_ANDROID
    float ph = 880.0f * s;
#else
    float ph = 780.0f * s;
#endif
    if (ph > H - 40.0f * s)
        ph = H - 40.0f * s;
    a->settings_panel.x = (W - pw) * 0.5f;
    a->settings_panel.y = (H - ph) * 0.5f;
    a->settings_panel.w = pw;
    a->settings_panel.h = ph;

    float x0 = a->settings_panel.x + 24.0f * s;
    float cw = pw - 48.0f * s;
    float gap = 8.0f * s;
    a->settings_close.x = a->settings_panel.x + pw - 42.0f * s;
    a->settings_close.y = a->settings_panel.y + 16.0f * s;
    a->settings_close.w = 30.0f * s;
    a->settings_close.h = 30.0f * s;

    int counts[6] = {3, 4, 4, 0, 0, 0};
    float tops[6] = {88.0f, 200.0f, 292.0f, 384.0f, 452.0f, 520.0f};
    for (int row = 0; row < 6; row++) {
        float ry = a->settings_panel.y + tops[row] * s;
        a->settings_rows[row].x = x0;
        a->settings_rows[row].y = ry;
        a->settings_rows[row].w = cw;
        a->settings_rows[row].h = 22.0f * s;
        a->settings_opt_n[row] = counts[row];
        if (counts[row] > 0) {
            float bw = (cw - gap * (float)(counts[row] - 1)) /
                       (float)counts[row];
            float oy = ry + 26.0f * s;
            for (int i = 0; i < counts[row]; i++) {
                a->settings_opt[row][i].x = x0 + (float)i * (bw + gap);
                a->settings_opt[row][i].y = oy;
                a->settings_opt[row][i].w = bw;
                a->settings_opt[row][i].h = 38.0f * s;
            }
        }
    }
    a->settings_ads_toggle.x = x0 + cw - 52.0f * s;
    a->settings_ads_toggle.y = a->settings_panel.y + 381.0f * s;
    a->settings_ads_toggle.w = 52.0f * s;
    a->settings_ads_toggle.h = 28.0f * s;
    a->settings_toggle.x = x0 + cw - 52.0f * s;
    a->settings_toggle.y = a->settings_panel.y + 449.0f * s;
    a->settings_toggle.w = 52.0f * s;
    a->settings_toggle.h = 28.0f * s;
    a->settings_autoplay_toggle.x = x0 + cw - 52.0f * s;
    a->settings_autoplay_toggle.y = a->settings_panel.y + 517.0f * s;
    a->settings_autoplay_toggle.w = 52.0f * s;
    a->settings_autoplay_toggle.h = 28.0f * s;
    a->settings_theme_rect = (SDL_FRect){x0, a->settings_panel.y +
                                                  66.0f * s,
                                        cw, 20.0f * s};
}

static void app_open_settings(App *a)
{
    app_close_picker(a);
    app_close_detail(a);
    a->show_settings = true;
    a->settings_row = 0;
    a->about_selected = false;
    app_settings_geom(a);
}

static void app_open_about(App *a)
{
    app_open_settings(a);
    a->about_selected = true;
}

static void app_close_settings(App *a)
{
    a->show_settings = false;
}

/* Live-apply + persist whenever the user changes something. */
static void app_settings_changed(App *a)
{
    app_apply_settings(a);
    settings_save(&a->settings);
    snprintf(a->toast, sizeof a->toast, "Settings saved");
    a->toast_until = SDL_GetTicks() + 1400;
}

static void app_settings_set(App *a, int row, int idx)
{
    if (row == 0) {
        static const PcImgCacheMode modes[3] = {PC_IMG_CACHE_OFF,
                                                PC_IMG_CACHE_JPEG,
                                                PC_IMG_CACHE_LZ4};
        if (idx < 0 || idx > 2)
            return;
        a->settings.cache_mode = modes[idx];
    } else if (row == 1) {
        if (idx < 0 || idx > 3)
            return;
        a->settings.budget_mb = SETTINGS_BUDGETS[idx];
    } else if (row == 2) {
        if (idx < 0 || idx > 3)
            return;
        a->settings.keep_screens = SETTINGS_KEEPS[idx];
    } else if (row == 3) {
        a->settings.block_ads = !a->settings.block_ads;
    } else if (row == 4) {
        a->settings.show_stats = !a->settings.show_stats;
    } else if (row == 5) {
        a->settings.autoplay_video = !a->settings.autoplay_video;
    }
    app_settings_changed(a);
}

static void app_settings_adjust(App *a, int dir)
{
    int row = a->settings_row;
    if (row == 0) {
        int idx = (int)a->settings.cache_mode + dir;
        if (idx < 0)
            idx = 2;
        if (idx > 2)
            idx = 0;
        app_settings_set(a, 0, idx);
    } else if (row == 1) {
        int idx = budget_index(a->settings.budget_mb) + dir;
        if (idx < 0)
            idx = 3;
        if (idx > 3)
            idx = 0;
        app_settings_set(a, 1, idx);
    } else if (row == 2) {
        int idx = keep_index(a->settings.keep_screens) + dir;
        if (idx < 0)
            idx = 3;
        if (idx > 3)
            idx = 0;
        app_settings_set(a, 2, idx);
    } else if (row == 3) {
        app_settings_set(a, 3, 0);
    } else if (row == 4) {
        app_settings_set(a, 4, 0);
    } else {
        app_settings_set(a, 5, 0);
    }
}

static void draw_gear_icon(App *a, float cx, float cy, float r, SDL_Color c)
{
    float thick = fmaxf(1.4f, 1.8f * a->rnd.scale);
    for (int i = 0; i < 8; i++) {
        float ang = (float)i * 0.7853981634f;
        float x1 = cx + cosf(ang) * r * 0.78f;
        float y1 = cy + sinf(ang) * r * 0.78f;
        float x2 = cx + cosf(ang) * r * 1.12f;
        float y2 = cy + sinf(ang) * r * 1.12f;
        rnd_line(&a->rnd, x1, y1, x2, y2, thick, c);
    }
    rnd_round_outline(&a->rnd, cx - r * 0.82f, cy - r * 0.82f, r * 1.64f,
                      r * 1.64f, r * 0.82f, thick, c);
    rnd_round(&a->rnd, cx - r * 0.26f, cy - r * 0.26f, r * 0.52f, r * 0.52f,
              r * 0.26f, c);
}

static void app_draw_settings(App *a)
{
    if (!a->show_settings)
        return;
    app_settings_geom(a);
    Rnd *r = &a->rnd;
    float s = r->scale;
    float W = (float)r->out_w, H = (float)r->out_h;
    SDL_FRect pan = a->settings_panel;

    rnd_fill(r, 0, 0, W, H, THEME.scrim);
    rnd_shadow(r, pan.x, pan.y + 4.0f * s, pan.w, pan.h, a->M.panel_radius,
               22.0f * s, 90);
    rnd_round(r, pan.x, pan.y, pan.w, pan.h, a->M.panel_radius,
              THEME.surface);

    float x0 = pan.x + 24.0f * s;
    float cw = pan.w - 48.0f * s;
    int tsz = (int)lroundf(20.0f * s);
    fonts_draw(r->r, a->fonts, "Settings", x0, pan.y + 20.0f * s, tsz, true,
               THEME.text);
    fonts_draw(r->r, a->fonts, "Image cache & memory", x0,
               pan.y + 48.0f * s, a->M.small_size, false, THEME.text_dim);
    /* Theme cycle row (System -> Light -> Dark) */
    {
        SDL_FRect tr = a->settings_theme_rect;
        bool hov = point_in_f(tr, (float)a->mx, (float)a->my);
        const char *tname = settings_theme_name(a->settings.theme_mode);
        char cap[32];
        cap[0] = (char)toupper((unsigned char)tname[0]);
        snprintf(cap + 1, sizeof cap - 1, "%s", tname + 1);
        fonts_draw(r->r, a->fonts, "Theme", x0,
                   tr.y + (tr.h - (float)fonts_line_h(
                                       a->fonts, a->M.small_size, true)) *
                              0.5f,
                   a->M.small_size, true, THEME.text);
        SDL_FRect val = {tr.x + tr.w - 104.0f * s, tr.y, 104.0f * s,
                         tr.h};
        rnd_round(r, val.x, val.y, val.w, val.h, val.h * 0.5f,
                  hov ? THEME.surface_hover : THEME.surface_alt);
        int vw = 0;
        fonts_measure(a->fonts, cap, a->M.small_size, true, &vw, NULL);
        int vh = fonts_line_h(a->fonts, a->M.small_size, true);
        fonts_draw(r->r, a->fonts, cap,
                   val.x + (val.w - (float)vw) * 0.5f,
                   val.y + (val.h - (float)vh) * 0.5f, a->M.small_size,
                   true, THEME.text);
    }

    rnd_round(r, a->settings_close.x, a->settings_close.y,
              a->settings_close.w, a->settings_close.h,
              a->settings_close.w * 0.5f,
              point_in_f(a->settings_close, (float)a->mx, (float)a->my)
                  ? THEME.surface_hover
                  : THEME.surface_alt);
    draw_close_icon(a, a->settings_close, THEME.text_dim);

    static const char *CACHE_LABELS[3] = {"Off", "JPEG bytes", "LZ4 RGBA"};
    static const char *BUDGET_LABELS[4] = {"16 MB", "32 MB", "64 MB",
                                           "128 MB"};
    static const char *KEEP_LABELS[4] = {"\xC2\xB1" "1", "\xC2\xB1" "2",
                                         "\xC2\xB1" "3",
                                         "\xC2\xB1" "5"};
    const char *labels[6][4] = {{NULL, NULL, NULL, NULL},
                                {NULL, NULL, NULL, NULL},
                                {NULL, NULL, NULL, NULL},
                                {NULL, NULL, NULL, NULL},
                                {NULL, NULL, NULL, NULL},
                                {NULL, NULL, NULL, NULL}};
    int counts[6] = {3, 4, 4, 0, 0, 0};
    int sel[6] = {(int)a->settings.cache_mode,
                  budget_index(a->settings.budget_mb),
                  keep_index(a->settings.keep_screens),
                  a->settings.block_ads ? 1 : 0,
                  a->settings.show_stats ? 1 : 0,
                  a->settings.autoplay_video ? 1 : 0};
    for (int i = 0; i < 3; i++)
        labels[0][i] = CACHE_LABELS[i];
    for (int i = 0; i < 4; i++) {
        labels[1][i] = BUDGET_LABELS[i];
        labels[2][i] = KEEP_LABELS[i];
    }
    static const char *ROW_TITLES[6] = {
        "Image cache", "RAM budget", "Keep textures",
        "Block promoted pins (ads)", "Stats overlay (F3)",
        "Autoplay videos in close-up"};

    for (int row = 0; row < 6; row++) {
        SDL_FRect rr = a->settings_rows[row];
        bool focus = (row == a->settings_row);
        fonts_draw(r->r, a->fonts, ROW_TITLES[row], rr.x, rr.y,
                   a->M.body_size, true,
                   focus ? THEME.text : THEME.text_dim);
        if (counts[row] == 0)
            continue;
        for (int i = 0; i < counts[row]; i++) {
            SDL_FRect o = a->settings_opt[row][i];
            bool active = (sel[row] == i);
            bool hov = point_in_f(o, (float)a->mx, (float)a->my);
            rnd_round(r, o.x, o.y, o.w, o.h, o.h * 0.5f,
                      active ? THEME.brand
                             : (hov ? THEME.surface_hover
                                    : THEME.surface_alt));
            int lsz = (int)lroundf(12.5f * s);
            int tw = 0;
            fonts_measure(a->fonts, labels[row][i], lsz, true, &tw, NULL);
            int th = fonts_line_h(a->fonts, lsz, true);
            fonts_draw(r->r, a->fonts, labels[row][i],
                       o.x + (o.w - (float)tw) * 0.5f,
                       o.y + (o.h - (float)th) * 0.5f, lsz, true,
                       active ? THEME.on_brand : THEME.text_dim);
        }
    }

    /* one-line tradeoff / explanation for the selected row */
    const char *explain = "";
    if (a->settings_row == 0) {
        switch (a->settings.cache_mode) {
        case PC_IMG_CACHE_OFF:
            explain = "Off: no RAM cache - every revisit re-downloads the "
                      "thumbnail.";
            break;
        case PC_IMG_CACHE_LZ4:
            explain = "LZ4: keeps packed RGBA (~1.1-1.4x smaller than raw); "
                      "revisits need one fast decompress.";
            break;
        default:
            explain = "JPEG bytes: keeps the ~10-15 KB thumbnail source; "
                      "smallest RAM, decode on revisit.";
            break;
        }
    } else if (a->settings_row == 1) {
        explain = "Cap for the in-memory image cache; shrinking applies "
                  "immediately (LRU eviction).";
    } else if (a->settings_row == 2) {
        explain = "Uploaded textures kept around the viewport; outside this "
                  "band they are freed and reloaded from the RAM cache.";
    } else if (a->settings_row == 3) {
        static char ads_explain[256];
        snprintf(ads_explain, sizeof ads_explain,
                 "Promoted/sponsored pins are filtered before layout (no "
                 "gaps). %llu blocked so far.",
                 a->client ? (unsigned long long)pc_ads_blocked(a->client)
                           : 0ULL);
        explain = ads_explain;
    } else if (a->settings_row == 4) {
        explain = "Performance overlay (also toggled with F3).";
    } else {
        explain = "Start videos automatically when a close-up opens; "
                  "when off, press play on the poster.";
    }
#ifndef PC_ANDROID
    fonts_draw(r->r, a->fonts, explain, x0,
               pan.y + 500.0f * s, a->M.small_size, false, THEME.text_dim);
#else
    (void)explain;
#endif

    /* ads toggle (row 3) */
    bool ads_on = a->settings.block_ads;
    rnd_round(r, a->settings_ads_toggle.x, a->settings_ads_toggle.y,
              a->settings_ads_toggle.w, a->settings_ads_toggle.h,
              a->settings_ads_toggle.h * 0.5f,
              ads_on ? THEME.brand : THEME.border);
    float knob = a->settings_ads_toggle.h - 6.0f * s;
    float kx = ads_on
                   ? a->settings_ads_toggle.x + a->settings_ads_toggle.w -
                         knob - 3.0f * s
                   : a->settings_ads_toggle.x + 3.0f * s;
    rnd_round(r, kx, a->settings_ads_toggle.y + 3.0f * s, knob, knob,
              knob * 0.5f, THEME.on_brand);

    /* stats toggle switch (row 4) */
    bool on = a->settings.show_stats;
    rnd_round(r, a->settings_toggle.x, a->settings_toggle.y,
              a->settings_toggle.w, a->settings_toggle.h,
              a->settings_toggle.h * 0.5f,
              on ? THEME.brand : THEME.border);
    kx = on ? a->settings_toggle.x + a->settings_toggle.w - knob -
                  3.0f * s
            : a->settings_toggle.x + 3.0f * s;
    rnd_round(r, kx, a->settings_toggle.y + 3.0f * s, knob, knob,
              knob * 0.5f, THEME.on_brand);

    /* autoplay toggle switch (row 5) */
    {
        bool aon = a->settings.autoplay_video;
        rnd_round(r, a->settings_autoplay_toggle.x,
                  a->settings_autoplay_toggle.y,
                  a->settings_autoplay_toggle.w,
                  a->settings_autoplay_toggle.h,
                  a->settings_autoplay_toggle.h * 0.5f,
                  aon ? THEME.brand : THEME.border);
        float aknob = a->settings_autoplay_toggle.h - 6.0f * s;
        float akx = aon ? a->settings_autoplay_toggle.x +
                              a->settings_autoplay_toggle.w - aknob -
                              3.0f * s
                        : a->settings_autoplay_toggle.x + 3.0f * s;
        rnd_round(r, akx, a->settings_autoplay_toggle.y + 3.0f * s, aknob,
                  aknob, aknob * 0.5f, THEME.on_brand);
    }

    /* ---- About: PinFall / DeepSeek Harness badge / repo URL ---- */
    {
        float ay = pan.y + 590.0f * s;
        rnd_fill(r, x0, ay, cw, 1.0f * s, THEME.border);
        ay += 12.0f * s;
        fonts_draw(r->r, a->fonts, "About", x0, ay, a->M.h2_size, true,
                   THEME.text);
        ay += (float)fonts_line_h(a->fonts, a->M.h2_size, true) + 6.0f * s;
        fonts_draw(r->r, a->fonts, "PinFall 0.1.0 \xC2\xB7 C11 + SDL2", x0,
                   ay, a->M.meta_size, true, THEME.text);
        ay += (float)fonts_line_h(a->fonts, a->M.meta_size, true) + 2.0f * s;
        fonts_draw(r->r, a->fonts, "Built with DeepSeek Harness", x0, ay,
                   a->M.small_size, false, THEME.text_dim);
        ay += (float)fonts_line_h(a->fonts, a->M.small_size, false) +
              8.0f * s;

        int mult = a->rnd.scale >= 1.5f ? 2 : 1; /* 121x20 (x2 HiDPI) */
        float bw = 121.0f * (float)mult;
        float bh = 20.0f * (float)mult;
        a->settings_about_badge = (SDL_FRect){x0, ay, bw, bh};
        if (a->badge_tex) {
            rnd_texture(r, a->badge_tex, x0, ay, bw, bh);
        } else {
            fonts_draw(r->r, a->fonts, "[dsh badge unavailable]", x0, ay,
                       a->M.small_size, false, THEME.error_text);
        }
        int url_w = 0;
        fonts_measure(a->fonts, "https://github.com/deepseek-ai/deepseek-harness",
                      a->M.small_size, false, &url_w, NULL);
        a->settings_about_url = (SDL_FRect){x0, ay + bh + 8.0f * s,
                                            (float)url_w,
                                            (float)fonts_line_h(
                                                a->fonts, a->M.small_size,
                                                false)};
        bool url_hov = point_in_f(a->settings_about_url, (float)a->mx,
                                  (float)a->my) ||
                       point_in_f(a->settings_about_badge, (float)a->mx,
                                  (float)a->my);
        fonts_draw(r->r, a->fonts,
                   "https://github.com/deepseek-ai/deepseek-harness", x0,
                   a->settings_about_url.y, a->M.small_size, false,
                   url_hov ? THEME.brand : THEME.text_dim);
        if (a->about_selected) {
            rnd_round_outline(r, x0 - 8.0f * s, pan.y + 578.0f * s,
                              cw + 16.0f * s,
                              ay + bh + 30.0f * s -
                                  (pan.y + 578.0f * s),
                              10.0f * s, 1.5f * s, THEME.brand);
        }
    }

#ifdef PC_ANDROID
    /* cookies source + reload (phone login path) */
    {
        float cy = pan.y + pan.h - 104.0f * s;
        rnd_fill(r, x0, cy - 12.0f * s, cw, 1.0f * s, THEME.border);
        fonts_draw(r->r, a->fonts, "Pinterest cookies", x0, cy,
                   a->M.small_size, true, THEME.text);
        char pbuf[FONT_LINE_MAX];
        fonts_ellipsize(a->fonts,
                        a->device_cookie_path[0]
                            ? a->device_cookie_path
                            : pf_android_cookie_path(),
                        a->M.small_size, false, cw, pbuf, sizeof pbuf);
        fonts_draw(r->r, a->fonts, pbuf, x0,
                   cy + (float)fonts_line_h(a->fonts, a->M.small_size,
                                            false) +
                       2.0f * s,
                   a->M.small_size, false, THEME.text_faint);
        a->settings_cookie_btn =
            (SDL_FRect){x0, cy + 40.0f * s, 168.0f * s, 32.0f * s};
        bool hov = point_in_f(a->settings_cookie_btn, (float)a->mx,
                              (float)a->my);
        rnd_round(r, a->settings_cookie_btn.x, a->settings_cookie_btn.y,
                  a->settings_cookie_btn.w, a->settings_cookie_btn.h,
                  a->settings_cookie_btn.h * 0.5f,
                  hov ? THEME.brand_hover : THEME.brand);
        int bsz = (int)lroundf(12.5f * s);
        int blh = fonts_line_h(a->fonts, bsz, true);
        draw_text_center(a, "Reload cookies",
                         bsz, true,
                         a->settings_cookie_btn.x +
                             a->settings_cookie_btn.w * 0.5f,
                         a->settings_cookie_btn.y +
                             (a->settings_cookie_btn.h - (float)blh) * 0.5f,
                         THEME.on_brand);
    }
#endif

    fonts_draw(r->r, a->fonts,
               "Saved automatically \xC2\xB7 Esc or , closes",
               x0, pan.y + pan.h - 24.0f * s, a->M.small_size, false,
               THEME.text_faint);
}

static void app_draw_stats(App *a)
{
    if (!a->show_stats)
        return;
    Rnd *r = &a->rnd;
    float s = r->scale;
    float W = (float)r->out_w;
    PcImgStats st = a->img_stats;
    char lines[8][200];
    int n = 0;
    uint64_t ads_blocked = a->client ? pc_ads_blocked(a->client) : 0;
    double tex_mb = (double)a->tex_bytes / (1024.0 * 1024.0);
    double used_mb = (double)st.used_bytes / (1024.0 * 1024.0);
    double budget_mb = (double)st.budget_bytes / (1024.0 * 1024.0);
    snprintf(lines[n++], sizeof lines[0],
             "pins %zu   pins loaded %d   gpu textures %d (peak %d)",
             a->npins, app_count_ready_images(a), a->tex_count, a->tex_peak);
    snprintf(lines[n++], sizeof lines[0],
             "texture MB %.1f   keep \xC2\xB1%d screens", tex_mb,
             a->settings.keep_screens);
    snprintf(lines[n++], sizeof lines[0],
             "cache %s  %.2f / %.2f MB", settings_cache_mode_name(st.mode),
             used_mb, budget_mb);
    snprintf(lines[n++], sizeof lines[0],
             "hits %llu   misses %llu", (unsigned long long)st.hits,
             (unsigned long long)st.misses);
    snprintf(lines[n++], sizeof lines[0],
             "lz4 packs %llu   unpacks %llu",
             (unsigned long long)st.packs, (unsigned long long)st.unpacks);
    snprintf(lines[n++], sizeof lines[0], "fps %.0f   evict outside \xC2\xB1%d",
             (double)a->fps, a->settings.keep_screens);
    snprintf(lines[n++], sizeof lines[0],
             "ads blocked %llu   block_ads %s",
             (unsigned long long)ads_blocked,
             a->settings.block_ads ? "on" : "off");
    int lsz = (int)lroundf(12.0f * s);
    if (lsz < 9)
        lsz = 9;
    int lh = (int)lroundf(18.0f * s);
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = 0;
        fonts_measure(a->fonts, lines[i], lsz, false, &w, NULL);
        if (w > maxw)
            maxw = w;
    }
    int titlew = 0;
    fonts_measure(a->fonts, "STATS  (F3)", lsz, true, &titlew, NULL);
    if (titlew > maxw)
        maxw = titlew;
    float pw = (float)maxw + 28.0f * s;
    float ph = 34.0f * s + (float)n * (float)lh + 12.0f * s;
    float px = W - 16.0f * s - pw;
    float py = a->M.header_h + 12.0f * s;
    if (px < 8.0f * s)
        px = 8.0f * s;
    rnd_round(r, px, py, pw, ph, 10.0f * s, col_a(THEME.text, 226));
    fonts_draw(r->r, a->fonts, "STATS  (F3)", px + 14.0f * s,
               py + 8.0f * s, lsz, true, THEME.brand_soft);
    for (int i = 0; i < n; i++)
        fonts_draw(r->r, a->fonts, lines[i], px + 14.0f * s,
                   py + 28.0f * s + (float)i * (float)lh, lsz, false,
                   THEME.surface);
}

/* ------------------------------------------------------- board picker ---
 * Modal with async board list + async save; mouse wheel scrolls the list.
 * Choosing a board farms the write out to the worker thread (NET_SAVE). */
static void picker_geom(App *a)
{
    float s = a->rnd.scale;
    float W = (float)a->rnd.out_w, H = (float)a->rnd.out_h;
    Picker *p = &a->picker;
    float pw = fminf(420.0f * s, W - 60.0f * s);
    float err_h = p->error[0] ? 46.0f * s : 0.0f;
    float create_h = 108.0f * s;
    int n = (int)p->boards.count;
    float row_h = 64.0f * s;
    float list_h;
    if (p->loading || (!p->boards_ready && !p->error[0]))
        list_h = 96.0f * s;
    else if (n == 0)
        list_h = 96.0f * s;
    else
        list_h = (float)n * row_h;
    float ph = 64.0f * s + err_h + create_h + list_h + 20.0f * s;
    float max_ph = H - 80.0f * s;
    if (ph > max_ph) {
        ph = max_ph;
        list_h = ph - 64.0f * s - err_h - create_h - 20.0f * s;
        if (list_h < 0.0f)
            list_h = 0.0f;
    }
    if (pw < 200.0f * s)
        pw = 200.0f * s;
    p->panel.x = (W - pw) * 0.5f;
    p->panel.y = (H - ph) * 0.5f;
    p->panel.w = pw;
    p->panel.h = ph;
    p->close.w = 30.0f * s;
    p->close.h = 30.0f * s;
    p->close.x = p->panel.x + p->panel.w - p->close.w - 12.0f * s;
    p->close.y = p->panel.y + 14.0f * s;
    p->list_y = p->panel.y + 64.0f * s + err_h + create_h;
    p->create_box = (SDL_FRect){p->panel.x + 20.0f * s,
                                p->panel.y + 64.0f * s + err_h,
                                p->panel.w - 40.0f * s - 96.0f * s,
                                40.0f * s};
    p->create_btn = (SDL_FRect){p->panel.x + p->panel.w - 20.0f * s -
                                    88.0f * s,
                                p->create_box.y, 88.0f * s, 40.0f * s};
    p->privacy_btn = (SDL_FRect){p->panel.x + 20.0f * s,
                                 p->create_box.y + 46.0f * s, 150.0f * s,
                                 26.0f * s};
    p->row_h = row_h;
    p->list_h = list_h;
    float msc = (float)n * row_h - list_h;
    p->max_scroll = msc > 0.0f ? msc : 0.0f;
    if (p->scroll < 0.0f)
        p->scroll = 0.0f;
    if (p->scroll > p->max_scroll)
        p->scroll = p->max_scroll;
    p->rows_visible = n;
}

static bool picker_row_rect(App *a, int row, SDL_FRect *out)
{
    Picker *p = &a->picker;
    if (!p->open)
        return false;
    picker_geom(a);
    if (row < 0 || (size_t)row >= p->boards.count)
        return false;
    float s = a->rnd.scale;
    float y = p->list_y + (float)row * p->row_h - p->scroll;
    if (y + p->row_h <= p->list_y || y >= p->list_y + p->list_h)
        return false;
    out->x = p->panel.x + 12.0f * s;
    out->y = y;
    out->w = p->panel.w - 24.0f * s;
    out->h = p->row_h;
    return true;
}

static void app_open_picker(App *a, int pin_idx)
{
    if (a->no_writes)
        return;
    if (a->auth != PC_AUTH_LOGGED_IN) {
        snprintf(a->toast, sizeof a->toast, "Sign in to save");
        a->toast_until = SDL_GetTicks() + 1800;
        return;
    }
    const char *pid = NULL;
    if (pin_idx >= 0 && (size_t)pin_idx < a->npins)
        pid = a->pins[pin_idx].pin.id;
    else if (a->show_detail && a->detail_pin.id[0])
        pid = a->detail_pin.id;
    if (!pid || !pid[0])
        return;
    app_close_picker(a);
    Picker *p = &a->picker;
    p->open = true;
    p->pin_idx = pin_idx;
    p->saving_row = -1;
    p->hover_row = -1;
    p->selected_row = -1;
    p->scroll = 0.0f;
    p->error[0] = '\0';
    p->creating = false;
    p->create_focus = false;
    p->new_name[0] = '\0';
    p->new_name_len = 0;
    p->new_secret = false;
    p->create_error[0] = '\0';
    p->created_name[0] = '\0';
    snprintf(p->pin_id, sizeof p->pin_id, "%s", pid);

    if (a->auth_preview) {
        if (!preview_fill_boards(&p->boards)) {
            snprintf(p->error, sizeof p->error,
                     "could not build preview boards");
            p->boards_ready = false;
            p->loading = false;
            return;
        }
        p->boards_ready = true;
        p->loading = false;
    } else {
        p->loading = true;
        p->boards_ready = false;
        NetCmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = NET_BOARDS;
        cmd.seq = ++a->net_boards_seq;
        app_push_net(a, &cmd);
    }
    picker_geom(a);
}

static void app_close_picker(App *a)
{
    Picker *p = &a->picker;
    p->open = false;
    p->loading = false;
    p->saving = false;
    p->saving_row = -1;
    p->hover_row = -1;
    p->selected_row = -1;
    p->error[0] = '\0';
    p->scroll = 0.0f;
    p->creating = false;
    p->create_focus = false;
    p->new_name[0] = '\0';
    p->new_name_len = 0;
    p->new_secret = false;
    p->create_error[0] = '\0';
    p->created_name[0] = '\0';
    pc_boardlist_free(&p->boards);
}

static void app_picker_choose(App *a, int row)
{
    Picker *p = &a->picker;
    if (!p->open || p->loading || p->saving)
        return;
    if (row < 0 || (size_t)row >= p->boards.count)
        return;
    if (a->no_writes)
        return;
    if (a->auth != PC_AUTH_LOGGED_IN) {
        snprintf(a->toast, sizeof a->toast, "Sign in to save");
        a->toast_until = SDL_GetTicks() + 1800;
        return;
    }
    PcBoard *b = &p->boards.items[row];
    if (a->auth_preview) {
        char nm[PC_BOARD_NAME_LEN] = "";
        if (preview_save_pin(p->pin_id, b->id, nm, sizeof nm)) {
            if (b->pin_count >= 0)
                b->pin_count++;
            snprintf(a->saved_board_name, sizeof a->saved_board_name, "%s",
                     nm[0] ? nm : b->name);
            if (p->created_name[0])
                snprintf(a->toast, sizeof a->toast,
                         "Created %.80s and saved", p->created_name);
            else
                snprintf(a->toast, sizeof a->toast, "Saved to %.100s",
                         a->saved_board_name);
            a->toast_until = SDL_GetTicks() + 2800;
            app_close_picker(a);
        } else {
            snprintf(p->error, sizeof p->error, "preview save failed");
        }
        return;
    }
    p->saving = true;
    p->saving_row = row;
    p->error[0] = '\0';
    snprintf(p->saving_board_id, sizeof p->saving_board_id, "%s", b->id);
    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = NET_SAVE;
    cmd.seq = ++a->net_save_seq;
    snprintf(cmd.pin_id, sizeof cmd.pin_id, "%s", p->pin_id);
    snprintf(cmd.board_id, sizeof cmd.board_id, "%s", b->id);
    snprintf(cmd.board_name, sizeof cmd.board_name, "%s", b->name);
    app_push_net(a, &cmd);
}


static void app_picker_create(App *a)
{
    Picker *p = &a->picker;
    if (!p->open || p->creating || p->loading || p->saving)
        return;
    p->create_error[0] = '\0';
    if (a->no_writes || a->auth != PC_AUTH_LOGGED_IN) {
        snprintf(p->create_error, sizeof p->create_error,
                 "Sign in to create boards");
        return;
    }
    char name[PC_BOARD_NAME_LEN];
    snprintf(name, sizeof name, "%s", p->new_name);
    size_t st = 0;
    while (name[st] == ' ' || name[st] == '\t')
        st++;
    size_t n = strlen(name);
    while (n > st && (name[n - 1] == ' ' || name[n - 1] == '\t' ||
                      name[n - 1] == '\n'))
        n--;
    memmove(name, name + st, n - st);
    name[n - st] = '\0';
    if (!name[0]) {
        snprintf(p->create_error, sizeof p->create_error,
                 "Board name is required");
        return;
    }
    if (strlen(name) > 60)
        name[60] = '\0';
    if (a->auth_preview) {
        /* fake a successful create locally; never writes */
        size_t count = p->boards.count;
        PcBoard *items = (PcBoard *)realloc(
            p->boards.items, (count + 1) * sizeof(PcBoard));
        if (!items) {
            snprintf(p->create_error, sizeof p->create_error,
                     "Out of memory");
            return;
        }
        memset(&items[count], 0, sizeof items[count]);
        snprintf(items[count].id, sizeof items[count].id, "preview_new_%zu",
                 count);
        snprintf(items[count].name, sizeof items[count].name, "%s", name);
        snprintf(items[count].privacy, sizeof items[count].privacy, "%s",
                 p->new_secret ? "secret" : "public");
        items[count].pin_count = 0;
        p->boards.items = items;
        p->boards.count = count + 1;
        p->boards.cap = count + 1;
        snprintf(p->created_name, sizeof p->created_name, "%s", name);
        p->new_name[0] = '\0';
        p->new_name_len = 0;
        p->create_focus = false;
        /* synthetic preview boards are not known to preview_save_pin() */
        snprintf(a->saved_board_name, sizeof a->saved_board_name, "%s",
                 name);
        if (items[count].pin_count >= 0)
            items[count].pin_count++;
        snprintf(a->toast, sizeof a->toast, "Created %.80s and saved", name);
        a->toast_until = SDL_GetTicks() + 2800;
        app_close_picker(a);
        return;
    }
    p->creating = true;
    NetCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = NET_CREATE_BOARD;
    cmd.seq = ++a->create_seq;
    cmd.secret = p->new_secret;
    snprintf(cmd.board_name, sizeof cmd.board_name, "%s", name);
    app_push_net(a, &cmd);
}

static void draw_tooltip(App *a, const char *text, float cx, float y)
{
    int sz = a->M.small_size;
    int tw = 0;
    fonts_measure(a->fonts, text, sz, false, &tw, NULL);
    int lh = fonts_line_h(a->fonts, sz, false);
    float s = a->rnd.scale;
    float pw = (float)tw + 20.0f * s;
    float ph = (float)lh + 12.0f * s;
    float px = cx - pw * 0.5f;
    if (px < 8.0f * s)
        px = 8.0f * s;
    if (px + pw > (float)a->rnd.out_w - 8.0f * s)
        px = (float)a->rnd.out_w - 8.0f * s - pw;
    rnd_round(&a->rnd, px, y, pw, ph, 6.0f * s, col_a(THEME.text, 238));
    draw_text_center(a, text, sz, false, px + pw * 0.5f,
                     y + (ph - (float)lh) * 0.5f, THEME.surface);
}

static void app_draw_picker(App *a)
{
    Picker *p = &a->picker;
    if (!p->open)
        return;
    picker_geom(a);
    Rnd *r = &a->rnd;
    float s = r->scale;
    float W = (float)r->out_w, H = (float)r->out_h;

    rnd_fill(r, 0, 0, W, H, THEME.scrim);
    rnd_shadow(r, p->panel.x, p->panel.y + 4.0f * s, p->panel.w, p->panel.h,
               a->M.panel_radius, 22.0f * s, 90);
    rnd_round(r, p->panel.x, p->panel.y, p->panel.w, p->panel.h,
              a->M.panel_radius, THEME.surface);

    int tsz = (int)lroundf(18.0f * s);
    fonts_draw(r->r, a->fonts, "Save to board",
               p->panel.x + 20.0f * s, p->panel.y + 16.0f * s, tsz, true,
               THEME.text);

    rnd_round(r, p->close.x, p->close.y, p->close.w, p->close.h,
              p->close.w * 0.5f,
              point_in_f(p->close, (float)a->mx, (float)a->my)
                  ? THEME.surface_hover
                  : THEME.surface_alt);
    draw_close_icon(a, p->close, THEME.text_dim);

    if (p->error[0]) {
        char lines[3][FONT_LINE_MAX];
        int nl = fonts_wrap(a->fonts, p->error, a->M.small_size, false,
                            p->panel.w - 40.0f * s, lines, 2);
        for (int i = 0; i < nl; i++)
            fonts_draw(r->r, a->fonts, lines[i],
                       p->panel.x + 20.0f * s,
                       p->panel.y + 50.0f * s +
                           (float)i * (float)(a->M.small_size + 4),
                       a->M.small_size, false, THEME.error_text);
    }

    /* create-board row */
    if (!writes_enabled(a)) {
        int hint_lh = fonts_line_h(a->fonts, a->M.small_size, false);
        fonts_draw(r->r, a->fonts, "Sign in to create boards",
                   p->create_box.x,
                   p->create_box.y + (40.0f * s - (float)hint_lh) * 0.5f,
                   a->M.small_size, false, THEME.text_faint);
    } else {
        bool focus = p->create_focus;
        rnd_round(r, p->create_box.x, p->create_box.y, p->create_box.w,
                  p->create_box.h, p->create_box.h * 0.5f,
                  focus ? THEME.surface : THEME.surface_alt);
        if (focus)
            rnd_round_outline(r, p->create_box.x, p->create_box.y,
                              p->create_box.w, p->create_box.h,
                              p->create_box.h * 0.5f,
                              fmaxf(1.5f, 1.8f * s), THEME.brand);
        int isz = a->M.body_size;
        int ilh = fonts_line_h(a->fonts, isz, false);
        const char *shown = p->new_name;
        SDL_Color tcol = THEME.text;
        if (!shown[0]) {
            shown = "New board name";
            tcol = THEME.text_faint;
        }
        float tx = p->create_box.x + 16.0f * s;
        rnd_push_clip(r, tx - 2.0f * s, p->create_box.y,
                      p->create_box.w - 24.0f * s, p->create_box.h);
        fonts_draw(r->r, a->fonts, shown, tx,
                   p->create_box.y + (p->create_box.h - (float)ilh) * 0.5f,
                   isz, false, tcol);
        if (focus && (SDL_GetTicks() / 500) % 2 == 0) {
            int pw = 0;
            fonts_measure(a->fonts, p->new_name, isz, false, &pw, NULL);
            rnd_fill(r, tx + (float)pw, p->create_box.y + 10.0f * s,
                     fmaxf(1.5f, 1.8f * s), p->create_box.h - 20.0f * s,
                     THEME.brand);
        }
        rnd_pop_clip(r);

        bool phov = point_in_f(p->privacy_btn, (float)a->mx, (float)a->my);
        rnd_round(r, p->privacy_btn.x, p->privacy_btn.y,
                  p->privacy_btn.w, p->privacy_btn.h,
                  p->privacy_btn.h * 0.5f,
                  phov ? THEME.surface_hover : THEME.surface_alt);
        int psz = (int)lroundf(12.0f * s);
        int plh = fonts_line_h(a->fonts, psz, true);
        draw_text_center(a, p->new_secret ? "Secret" : "Public",
                         psz, true,
                         p->privacy_btn.x + p->privacy_btn.w * 0.5f,
                         p->privacy_btn.y +
                             (p->privacy_btn.h - (float)plh) * 0.5f,
                         THEME.text_dim);

        bool create_ready = p->new_name_len > 0 && !p->creating;
        bool cbhov = point_in_f(p->create_btn, (float)a->mx, (float)a->my);
        rnd_round(r, p->create_btn.x, p->create_btn.y, p->create_btn.w,
                  p->create_btn.h, p->create_btn.h * 0.5f,
                  create_ready ? (cbhov ? THEME.brand_hover : THEME.brand)
                               : THEME.surface_alt);
        int bsz = (int)lroundf(13.0f * s);
        int blh = fonts_line_h(a->fonts, bsz, true);
        draw_text_center(a, p->creating ? "Creating\xE2\x80\xA6" : "Create",
                         bsz, true,
                         p->create_btn.x + p->create_btn.w * 0.5f,
                         p->create_btn.y +
                             (p->create_btn.h - (float)blh) * 0.5f,
                         create_ready ? THEME.on_brand : THEME.text_faint);
        if (p->create_error[0]) {
            char el[2][FONT_LINE_MAX];
            int en = fonts_wrap(a->fonts, p->create_error,
                                a->M.small_size, false,
                                p->panel.w - 40.0f * s, el, 2);
            for (int i = 0; i < en; i++)
                fonts_draw(r->r, a->fonts, el[i], p->create_box.x,
                           p->create_box.y + 76.0f * s +
                               (float)i * (float)(a->M.small_size + 3),
                           a->M.small_size, false, THEME.error_text);
        }
    }

    rnd_push_clip(r, p->panel.x + 8.0f * s, p->list_y,
                  p->panel.w - 16.0f * s, p->list_h);
    if (p->loading) {
        float cx = p->panel.x + p->panel.w * 0.5f;
        float cy = p->list_y + p->list_h * 0.5f;
        for (int i = 0; i < 8; i++) {
            float ang = a->anim_t * 3.0f + (float)i * 0.785f;
            float rr = 2.6f * s;
            float dx = cosf(ang) * 14.0f * s;
            float dy = sinf(ang) * 14.0f * s;
            Uint8 al = (Uint8)(60 + 170 * (0.5f + 0.5f * sinf(ang)));
            rnd_round(r, cx + dx - rr, cy + dy - rr, rr * 2.0f, rr * 2.0f,
                      rr, col_a(THEME.brand, al));
        }
        draw_text_center(a, "Loading boards\xE2\x80\xA6", a->M.small_size,
                         false, cx, cy + 26.0f * s, THEME.text_dim);
    } else if (p->boards.count == 0) {
        draw_text_center(a, "No boards found", a->M.body_size, false,
                         p->panel.x + p->panel.w * 0.5f,
                         p->list_y + p->list_h * 0.5f - 10.0f * s,
                         THEME.text_dim);
        draw_text_center(a, "Create a board on Pinterest first",
                         a->M.small_size, false,
                         p->panel.x + p->panel.w * 0.5f,
                         p->list_y + p->list_h * 0.5f + 16.0f * s,
                         THEME.text_faint);
    } else {
        for (size_t i = 0; i < p->boards.count; i++) {
            SDL_FRect row;
            if (!picker_row_rect(a, (int)i, &row))
                continue;
            bool hov = point_in_f(row, (float)a->mx, (float)a->my) &&
                       !p->saving;
            if (hov)
                rnd_round(r, row.x, row.y, row.w, row.h, 10.0f * s,
                          THEME.surface_hover);
            PcBoard *b = &p->boards.items[i];
            fonts_draw(r->r, a->fonts, b->name, row.x + 14.0f * s,
                       row.y + 10.0f * s, (int)lroundf(15.0f * s), true,
                       THEME.text);
            char meta[192];
            snprintf(meta, sizeof meta, "%d pins \xC2\xB7 %s", b->pin_count,
                     b->privacy[0] ? b->privacy : "public");
            fonts_draw(r->r, a->fonts, meta, row.x + 14.0f * s,
                       row.y + 34.0f * s, a->M.small_size, false,
                       THEME.text_dim);
            if (p->saving && p->saving_row == (int)i) {
                draw_text_right(a, "Saving\xE2\x80\xA6", a->M.small_size, true,
                                row.x + row.w - 14.0f * s,
                                row.y + row.h * 0.5f -
                                    (float)a->M.small_size * 0.5f,
                                THEME.brand);
            } else if (!p->saving) {
                draw_text_right(a, "Save", a->M.small_size, true,
                                row.x + row.w - 14.0f * s,
                                row.y + row.h * 0.5f -
                                    (float)a->M.small_size * 0.5f,
                                THEME.text_faint);
            }
        }
    }
    rnd_pop_clip(r);
}

static void app_draw_empty(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w;
    float s = r->scale;
    float top = content_top(a);
    float bot = content_bottom(a);
    float pw = fminf(540.0f * s, W - 80.0f * s);
    float ph = fminf(300.0f * s, (bot - top) - 60.0f * s);
    float px = (W - pw) * 0.5f;
    float py = top + ((bot - top) - ph) * 0.5f;

    rnd_shadow(r, px, py + 2.0f * s, pw, ph, a->M.panel_radius, 16.0f * s,
               34);
    rnd_round(r, px, py, pw, ph, a->M.panel_radius, THEME.surface);

    float cx = px + pw * 0.5f;
    float icon = 64.0f * s;
    rnd_round(r, cx - icon * 0.5f, py + 34.0f * s, icon, icon, icon * 0.5f,
              THEME.brand_soft);
    /* simple pin glyph */
    float pin_w = 14.0f * s, pin_h = 20.0f * s;
    float pin_top = py + 34.0f * s + icon * 0.5f - pin_h * 0.5f;
    rnd_round(r, cx - pin_w * 0.5f, pin_top + 5.0f * s, pin_w, pin_h,
              5.0f * s, THEME.brand);
    rnd_round(r, cx - 5.0f * s, pin_top, 10.0f * s, 10.0f * s, 5.0f * s,
              THEME.brand);

    draw_text_center(a, "No saved pins yet", a->M.h2_size, true, cx,
                     py + 118.0f * s, THEME.text);
    draw_text_center(a, "Save pins from search to find them here.",
                     a->M.body_size, false, cx, py + 154.0f * s,
                     THEME.text_dim);
    draw_text_center(a, "Use Save on a pin card or in its detail view.",
                     a->M.small_size, false, cx, py + 196.0f * s,
                     THEME.text_faint);
}

/* --------------------------------------------------------------- drawing */
static SDL_FRect search_rect(App *a)
{
    float s = a->rnd.scale;
    float W = (float)a->rnd.out_w;
    int tw = 0;
    fonts_measure(a->fonts, APP_NAME, a->M.logo_size, true, &tw, NULL);
    float logo_end = 16.0f * s + 32.0f * s + 10.0f * s + (float)tw;

    /* docs/UI_TOKENS.json: pill starts at 242, right edge is W-236. */
    float x = 242.0f * s;
    if (x < logo_end + 24.0f * s)
        x = logo_end + 24.0f * s;
    float w = W - 236.0f * s - x;
    if (w < 240.0f * s) {
        x = logo_end + 16.0f * s;
        w = W - 16.0f * s - x;
    }
    if (w < 120.0f * s)
        w = 120.0f * s;

    SDL_FRect r;
    r.x = x;
    r.y = (a->M.header_h - a->M.search_h) * 0.5f;
    r.w = w;
    r.h = a->M.search_h;
    return r;
}

static void app_draw_header(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w;
    float s = r->scale;

    rnd_fill(r, 0, 0, W, a->M.header_h, THEME.surface);
    rnd_fill(r, 0, a->M.header_h - 1.0f, W, 1.0f, THEME.border);

    /* logo cluster: red P mark + wordmark, cluster starts at 16px */
    float mark = 32.0f * s;
    float mx = 16.0f * s;
    float my = (a->M.header_h - mark) * 0.5f;
    rnd_round(r, mx, my, mark, mark, 8.0f * s, THEME.brand);
    int lh = fonts_line_h(a->fonts, a->M.logo_size, true);
    draw_text_center(a, "P", a->M.logo_size, true, mx + mark * 0.5f,
                     my + (mark - (float)lh) * 0.5f, THEME.on_brand);

    int logoh = fonts_line_h(a->fonts, a->M.logo_size, true);
    fonts_draw(r->r, a->fonts, APP_NAME, mx + mark + 10.0f * s,
               (a->M.header_h - (float)logoh) * 0.5f, a->M.logo_size, true,
               THEME.text);

    /* search pill: h48 r24 #e5e5e0, icon 16 with 16px left padding */
    SDL_FRect sr = search_rect(a);
    float rad = a->M.search_radius;
    rnd_round(r, sr.x, sr.y, sr.w, sr.h, rad, THEME.surface_alt);
    if (a->search_focus)
        rnd_round_outline(r, sr.x, sr.y, sr.w, sr.h, rad,
                          fmaxf(2.0f, 2.0f * s), THEME.brand);

    float icon_sz = 16.0f * s;
    float icon_cx = sr.x + 16.0f * s + icon_sz * 0.5f;
    float icon_cy = sr.y + sr.h * 0.5f;
    draw_search_icon(a, icon_cx, icon_cy, icon_sz * 0.5f, THEME.text);

    float text_x = sr.x + 40.0f * s; /* 16 pad + 16 icon + 8 gap */
    float clear_w = a->input[0] ? 40.0f * s : 8.0f * s;
    float text_avail = sr.w - 40.0f * s - clear_w;
    if (text_avail < 30.0f * s)
        text_avail = 30.0f * s;
    int line_h = fonts_line_h(a->fonts, a->M.search_size, false);
    float text_y = sr.y + (sr.h - (float)line_h) * 0.5f;

    rnd_push_clip(r, text_x - 2.0f * s, sr.y, text_avail + 4.0f * s, sr.h);

    const char *shown = a->input;
    SDL_Color tcol = THEME.text;
    if (!a->input[0]) {
        shown = "Search for ideas";
        tcol = THEME.text_dim;
    }

    size_t caret_off = fonts_utf8_offset(a->input, a->caret);
    int caret_x = 0;
    {
        char prefix[PC_TITLE_LEN];
        size_t cp = caret_off < sizeof prefix ? caret_off : sizeof prefix - 1;
        memcpy(prefix, a->input, cp);
        prefix[cp] = '\0';
        fonts_measure(a->fonts, prefix, a->M.search_size, false, &caret_x,
                      NULL);
    }
    float offset = 0.0f;
    if ((float)caret_x > text_avail - 2.0f * s)
        offset = (float)caret_x - (text_avail - 2.0f * s);

    fonts_draw(r->r, a->fonts, shown, text_x - offset, text_y,
               a->M.search_size, false, tcol);
    if (a->search_focus && a->caret_on) {
        float cxp = text_x - offset + (float)caret_x;
        rnd_fill(r, cxp, sr.y + 12.0f * s, fmaxf(1.5f, 2.0f * s),
                 sr.h - 24.0f * s, THEME.brand);
    }
    rnd_pop_clip(r);

    /* cancel icon (16px) on the right */
    a->search_clear.w = 0;
    a->search_clear.h = 0;
    if (a->input[0]) {
        float cb = 32.0f * s;
        SDL_FRect clr = {sr.x + sr.w - cb - 8.0f * s,
                         sr.y + (sr.h - cb) * 0.5f, cb, cb};
        bool hov = point_in_f(clr, (float)a->mx, (float)a->my);
        if (hov)
            rnd_round(r, clr.x, clr.y, clr.w, clr.h, cb * 0.5f,
                      THEME.surface);
        SDL_FRect ic = {clr.x + (cb - 16.0f * s) * 0.5f,
                        clr.y + (cb - 16.0f * s) * 0.5f, 16.0f * s,
                        16.0f * s};
        draw_close_icon(a, ic, THEME.text);
        a->search_clear.x = (int)clr.x;
        a->search_clear.y = (int)clr.y;
        a->search_clear.w = (int)clr.w;
        a->search_clear.h = (int)clr.h;
    }

    /* right cluster: a fixed 220px budget for both auth variants so the
     * header never shifts when the session state changes. */
    float right_x = W - 16.0f * s;
    float cluster_w = 220.0f * s;
    float cluster_x = right_x - cluster_w;
    a->home_btn.w = 0;
    a->home_btn.h = 0;
    a->saved_btn.w = 0;
    a->saved_btn.h = 0;

    /* settings gear (left edge of the right cluster; both auth variants) */
    a->gear_btn.w = 0;
    a->gear_btn.h = 0;
    {
        float gs = 32.0f * s;
        SDL_FRect gear = {cluster_x, (a->M.header_h - gs) * 0.5f, gs, gs};
        bool ghov = point_in_f(gear, (float)a->mx, (float)a->my);
        if (ghov)
            rnd_round(r, gear.x, gear.y, gear.w, gear.h, 8.0f * s,
                      THEME.surface_hover);
        draw_gear_icon(a, gear.x + gs * 0.5f, gear.y + gs * 0.5f,
                       gs * 0.29f, THEME.text_dim);
        a->gear_btn.x = (int)gear.x;
        a->gear_btn.y = (int)gear.y;
        a->gear_btn.w = (int)gear.w;
        a->gear_btn.h = (int)gear.h;
    }

    if (a->auth == PC_AUTH_LOGGED_IN) {
        /* Home + Saved tabs share the cluster with the username chip */
        const char *labels[2] = {"Home", "Saved"};
        FeedMode modes[2] = {FEED_HOME, FEED_SAVED};
        SDL_Rect *rects[2] = {&a->home_btn, &a->saved_btn};
        int tsz = (int)lroundf(12.5f * s);
        if (tsz < 10)
            tsz = 10;
        int tth = fonts_line_h(a->fonts, tsz, true);
        float tab_h = 38.0f * s;
        float tab_y = (a->M.header_h - tab_h) * 0.5f;
        float tx = cluster_x + 40.0f * s;
        for (int i = 0; i < 2; i++) {
            int tw = 0;
            fonts_measure(a->fonts, labels[i], tsz, true, &tw, NULL);
            float tab_w = (float)tw + 18.0f * s;
            SDL_FRect tab = {tx, tab_y, tab_w, tab_h};
            bool active = (a->feed_mode == modes[i]);
            bool hov = point_in_f(tab, (float)a->mx, (float)a->my);
            if (active || hov)
                rnd_round(r, tab.x, tab.y, tab.w, tab.h, tab_h * 0.5f,
                          active ? THEME.surface_alt : THEME.surface_hover);
            fonts_draw(r->r, a->fonts, labels[i],
                       tab.x + (tab.w - (float)tw) * 0.5f,
                       tab.y + (tab.h - (float)tth) * 0.5f, tsz, true,
                       active ? THEME.text : THEME.text_dim);
            rects[i]->x = (int)tab.x;
            rects[i]->y = (int)tab.y;
            rects[i]->w = (int)tab.w;
            rects[i]->h = (int)tab.h;
            tx += tab_w + 3.0f * s;
        }

        /* username chip: avatar + display name, right aligned */
        float av = 32.0f * s;
        SDL_FRect avatar = {right_x - av, (a->M.header_h - av) * 0.5f, av,
                            av};
        bool av_hov = point_in_f(avatar, (float)a->mx, (float)a->my);
        rnd_round(r, avatar.x, avatar.y, avatar.w, avatar.h, av * 0.5f,
                  av_hov ? THEME.surface_hover : THEME.surface_alt);
        char initial[8] = {0};
        if (a->username[0]) {
            unsigned char c0 = (unsigned char)a->username[0];
            size_t cl = c0 >= 0xF0 ? 4u : c0 >= 0xE0 ? 3u
                        : c0 >= 0xC0            ? 2u
                                                : 1u;
            if (cl > sizeof initial - 1)
                cl = 1;
            memcpy(initial, a->username, cl);
            initial[cl] = '\0';
        }
        int isz = (int)lroundf(15.0f * s);
        int ith = fonts_line_h(a->fonts, isz, true);
        draw_text_center(a, initial[0] ? initial : "P", isz, true,
                         avatar.x + av * 0.5f, avatar.y + (av - (float)ith) * 0.5f,
                         THEME.text_dim);

        const char *name = a->username[0] ? a->username : "Signed in";
        char shown[PC_SHORT_LEN];
        float name_max = avatar.x - 6.0f * s - tx;
        if (name_max < 24.0f * s)
            name_max = 24.0f * s;
        int nsz = (int)lroundf(11.0f * s);
        int nth = fonts_line_h(a->fonts, nsz, true);
        fonts_ellipsize(a->fonts, name, nsz, true, name_max, shown,
                        sizeof shown);
        draw_text_right(a, shown, nsz, true, avatar.x - 6.0f * s,
                        (a->M.header_h - (float)nth) * 0.5f, THEME.text);
    } else {
        /* guest: existing badge + a subtle cookie-import hint */
        const char *status = "Guest session";
        SDL_Color scol = THEME.text_dim;
        SDL_Color dotc = THEME.ok;
        if (a->state == VIEW_ERROR) {
            status = "Offline";
            scol = THEME.error_text;
            dotc = THEME.error_text;
        } else if (a->state == VIEW_LOADING || a->state == VIEW_BOOT) {
            status = "Loading\xE2\x80\xA6";
            dotc = THEME.brand;
        } else if (a->loading_more) {
            status = "Loading more\xE2\x80\xA6";
            dotc = THEME.brand;
        }
        int status_w = 0;
        fonts_measure(a->fonts, status, a->M.small_size, false, &status_w,
                      NULL);
        int sh = fonts_line_h(a->fonts, a->M.small_size, false);
        float sx = right_x - (float)status_w;
        float l1y = a->M.header_h * 0.5f - (float)sh - 2.0f * s;
        fonts_draw(r->r, a->fonts, status, sx, l1y, a->M.small_size, false,
                   scol);
        float dot_r = 3.5f * s;
        float phase = 0.5f + 0.5f * sinf(a->anim_t * 3.0f);
        if (a->state == VIEW_LOADING || a->loading_more)
            dotc = col_lerp(col_a(THEME.brand, 110), THEME.brand, phase);
        rnd_round(r, sx - dot_r * 2.0f - 8.0f * s,
                  l1y + (float)sh * 0.5f - dot_r, dot_r * 2.0f,
                  dot_r * 2.0f, dot_r, dotc);
        const char *hint = "Import cookies for Home";
        int hsz = (int)lroundf(11.0f * s);
        draw_text_right(a, hint, hsz, false, right_x,
                        a->M.header_h * 0.5f + 4.0f * s, THEME.text_faint);
    }
}

static void app_draw_skeleton_card(App *a, float x, float y, float w,
                                   float h)
{
    float s = a->rnd.scale;
    float phase =
        0.5f + 0.5f * sinf(a->anim_t * 2.0f + (x + y) * 0.013f);
    SDL_Color sk = col_lerp(THEME.skeleton, THEME.skeleton_hi, phase);
    rnd_round(&a->rnd, x, y, w, h, a->M.card_radius, sk);

    /* faint image glyph */
    float cw = 34.0f * s, ch = 26.0f * s;
    SDL_FRect g = {x + (w - cw) * 0.5f, y + (h - ch) * 0.5f, cw, ch};
    rnd_round_outline(&a->rnd, g.x, g.y, g.w, g.h, 6.0f * s,
                      fmaxf(1.0f, 1.4f * s), col_a(THEME.skeleton_hi, 210));
    rnd_round(&a->rnd, g.x + 5.0f * s, g.y + 5.0f * s, 7.0f * s, 7.0f * s,
              3.0f * s, col_a(THEME.skeleton_hi, 210));
}

static void app_draw_loading_grid(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w;
    float y0 = content_top(a) + 16.0f * a->rnd.scale;
    float y1 = content_bottom(a);
    float avail = W - 2.0f * a->M.pad;
    int cols = (int)ceilf((W - 260.0f * a->rnd.scale) /
                          (236.0f * a->rnd.scale));
    if (cols < 2)
        cols = 2;
    if (cols > a->M.max_cols)
        cols = a->M.max_cols;
    float col_w = (avail - a->M.gap * (float)(cols - 1)) / (float)cols;

    static const float aspects[12] = {1.30f, 1.00f, 1.55f, 1.20f, 1.75f,
                                      0.85f, 1.40f, 1.10f, 1.60f, 1.25f,
                                      0.95f, 1.50f};
    float cursor[8] = {0};
    for (int i = 0; i < 12; i++) {
        int c = i % cols;
        float x = a->M.pad + (float)c * (col_w + a->M.gap);
        float y = y0 + cursor[c];
        float h = col_w * aspects[i];
        if (y > y1)
            break;
        app_draw_skeleton_card(a, x, y, col_w, h);
        /* title placeholder bars */
        float bar_w = col_w * (0.72f + 0.18f * (float)((i * 37) % 10) / 10.0f);
        rnd_round(r, x, y + h + 9.0f * a->rnd.scale,
                  bar_w, 8.0f * a->rnd.scale, 4.0f * a->rnd.scale,
                  THEME.skeleton);
        rnd_round(r, x, y + h + 22.0f * a->rnd.scale,
                  col_w * 0.45f, 8.0f * a->rnd.scale,
                  4.0f * a->rnd.scale, THEME.skeleton);
        cursor[c] += h + a->M.title_block_h + a->M.gap;
    }

    /* status pill */
    char msg[PC_TITLE_LEN + 32];
    snprintf(msg, sizeof msg, "Searching \xE2\x80\x9C%s\xE2\x80\x9D",
             a->query);
    int tw = 0, th = 0;
    fonts_measure(a->fonts, msg, a->M.body_size, false, &tw, &th);
    float padx = 20.0f * a->rnd.scale;
    float pillw = (float)tw + padx * 2.0f;
    float pillh = (float)fonts_line_h(a->fonts, a->M.body_size, false) +
                  18.0f * a->rnd.scale;
    float px = (W - pillw) * 0.5f;
    float py = y1 - pillh - 26.0f * a->rnd.scale;
    rnd_round(r, px, py, pillw, pillh, pillh * 0.5f, THEME.surface);
    rnd_round_outline(r, px, py, pillw, pillh, pillh * 0.5f, 1.0f,
                      THEME.border);
    draw_text_center(a, msg, a->M.body_size, false, W * 0.5f,
                     py + 9.0f * a->rnd.scale, THEME.text_dim);
}

static void app_draw_card(App *a, size_t i)
{
    Rnd *r = &a->rnd;
    const LayoutItem *it = &a->layout.items[i];
    PinView *pv = &a->pins[i];
    float s = a->rnd.scale;
    float y = it->y - a->scroll;
    SDL_FRect img = {it->x, y, it->w, it->img_h};

    if (pv->status == 2 && pv->tex) {
        /* cards are flat in the reference: no shadow, no border */
        rnd_texture(r, pv->tex, img.x, img.y, img.w, img.h);
    } else if (pv->status == 3) {
        rnd_round(r, img.x, img.y, img.w, img.h, a->M.card_radius,
                  THEME.skeleton);
        const char *msg = "Image unavailable";
        draw_text_center(a, msg, a->M.small_size, false,
                         img.x + img.w * 0.5f,
                         img.y + img.h * 0.5f -
                             (float)fonts_line_h(a->fonts, a->M.small_size,
                                                 false) *
                                 0.5f,
                         THEME.text_faint);
    } else {
        app_draw_skeleton_card(a, img.x, img.y, img.w, img.h);
    }

    /* video pins: subtle play affordance (white circle + triangle) */
#ifndef PC_ANDROID
    if (pv->pin.is_video && pv->status == 2) {
        float br = 13.0f * s;
        float bx = img.x + 10.0f * s + br;
        float by = img.y + img.h - 10.0f * s - br;
        rnd_round(r, bx - br, by - br, br * 2.0f, br * 2.0f, br,
                  col_a(THEME.on_brand, 235));
        SDL_Vertex v[3];
        int ind[3] = {0, 1, 2};
        memset(v, 0, sizeof v);
        v[0].position.x = bx - br * 0.30f;
        v[0].position.y = by - br * 0.42f;
        v[1].position.x = bx - br * 0.30f;
        v[1].position.y = by + br * 0.42f;
        v[2].position.x = bx + br * 0.52f;
        v[2].position.y = by;
        for (int k = 0; k < 3; k++)
            v[k].color = THEME.text;
        SDL_RenderGeometry(r->r, NULL, v, 3, ind, 3);
    }
#endif

    /* hover: rgba(0,0,0,.25) wash + Open hint + Save pill.
     * Phones have no hover; long-press opens the Save picker instead. */
#ifdef PC_ANDROID
    if (0) {
#else
    if (pv->hover > 0.01f && !a->show_detail && !a->picker.open) {
#endif
        Uint8 alpha = (Uint8)((float)THEME.overlay.a * pv->hover);
        rnd_round(r, img.x, img.y, img.w, img.h, a->M.card_radius,
                  col_a(THEME.overlay, alpha));
        Uint8 pa = (Uint8)(255.0f * pv->hover);

        /* centered "Open" hint */
        {
            const char *label = "Open";
            int tw = 0;
            fonts_measure(a->fonts, label, a->M.title_size, true, &tw, NULL);
            float pillh = 36.0f * s;
            float padx = 16.0f * s;
            float pillw = (float)tw + padx * 2.0f;
            float px = img.x + (img.w - pillw) * 0.5f;
            float py = img.y + (img.h - pillh) * 0.5f;
            int th = fonts_line_h(a->fonts, a->M.title_size, true);
            rnd_round(r, px, py, pillw, pillh, pillh * 0.5f,
                      col_a(THEME.surface, (Uint8)(238.0f * pv->hover)));
            fonts_draw(r->r, a->fonts, label,
                       px + (pillw - (float)tw) * 0.5f,
                       py + (pillh - (float)th) * 0.5f, a->M.title_size,
                       true, col_a(THEME.text, pa));
        }

        /* Save pill, top-right (tokens save_button: h40, r20, red) */
        if (!a->no_writes) {
            bool enabled = writes_enabled(a);
            const char *label = "Save";
            int sz = (int)lroundf(15.0f * s);
            int tw = 0, th = fonts_line_h(a->fonts, sz, true);
            fonts_measure(a->fonts, label, sz, true, &tw, NULL);
            float pillw = (float)tw + 32.0f * s;
            float pillh = 40.0f * s;
            SDL_FRect sp = {img.x + img.w - 8.0f * s - pillw,
                            img.y + 8.0f * s, pillw, pillh};
            rnd_round(r, sp.x, sp.y, sp.w, sp.h, pillh * 0.5f,
                      enabled ? col_a(THEME.brand, pa)
                              : col_a(THEME.surface,
                                      (Uint8)(228.0f * pv->hover)));
            fonts_draw(r->r, a->fonts, label,
                       sp.x + (sp.w - (float)tw) * 0.5f,
                       sp.y + (sp.h - (float)th) * 0.5f, sz, true,
                       enabled ? col_a(THEME.on_brand, pa)
                               : col_a(THEME.text_faint, pa));
            a->card_save_rect.x = (int)sp.x;
            a->card_save_rect.y = (int)sp.y;
            a->card_save_rect.w = (int)sp.w;
            a->card_save_rect.h = (int)sp.h;
            if (!enabled && point_in_f(sp, (float)a->mx, (float)a->my))
                draw_tooltip(a, "Sign in to save", sp.x + sp.w * 0.5f,
                             sp.y + sp.h + 8.0f * s);
        }
    }

    /* caption: 16px/700, two-line clamp (tokens) */
    float ty = it->title_y - a->scroll;
    for (int l = 0; l < pv->nlines; l++) {
        fonts_draw(r->r, a->fonts, pv->line[l], it->title_x,
                   ty + (float)l * (float)a->M.title_line, a->M.title_size,
                   true, THEME.text);
    }
}

static void app_draw_feed(App *a)
{
    Rnd *r = &a->rnd;
    float top = content_top(a);
    float bot = content_bottom(a);

    rnd_push_clip(r, 0, top, (float)r->out_w, bot - top);
    a->card_save_rect.w = 0;
    a->card_save_rect.h = 0;
    for (size_t i = 0; i < a->layout.count && i < a->npins; i++) {
        const LayoutItem *it = &a->layout.items[i];
        float y = it->y - a->scroll;
        if (y + it->card_h < top || y > bot)
            continue;
        app_draw_card(a, i);
    }

    /* scrollbar */
    float total = a->layout.total_h;
    float ch = bot - top;
    if (total > ch + 1.0f && a->npins > 0) {
        float track_w = 8.0f * r->scale;
        float track_x = (float)r->out_w - 2.0f * r->scale - track_w;
        float track_y = top + 8.0f * r->scale;
        float track_h = ch - 16.0f * r->scale;
        float thumb_h = track_h * ch / total;
        if (thumb_h < 40.0f * r->scale)
            thumb_h = 40.0f * r->scale;
        if (thumb_h > track_h)
            thumb_h = track_h;
        float ms = max_scroll(a);
        float t = ms > 0.0f ? a->scroll / ms : 0.0f;
        float ty2 = track_y + t * (track_h - thumb_h);
        Uint8 ta = a->dragging_scrollbar ? 89 : 51;
        rnd_round(r, track_x, ty2, track_w, thumb_h, track_w * 0.5f,
                  (SDL_Color){0, 0, 0, ta});
    }
    rnd_pop_clip(r);
}

static void app_draw_footer(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w;
    float H = (float)r->out_h;
    float y = H - a->M.footer_h;
    rnd_fill(r, 0, y, W, a->M.footer_h, THEME.surface);
    rnd_fill(r, 0, y, W, 1.0f, THEME.border);

    char left[1024];
    SDL_Color lc = THEME.text_dim;
    if (a->state == VIEW_ERROR) {
        snprintf(left, sizeof left, "Offline \xC2\xB7 %s", a->error);
        lc = THEME.error_text;
    } else if (a->state == VIEW_LOADING || a->state == VIEW_BOOT) {
        if (a->feed_mode == FEED_SAVED)
            snprintf(left, sizeof left, "Loading \xC2\xB7 Saved");
        else if (a->feed_mode == FEED_HOME)
            snprintf(left, sizeof left, "Loading \xC2\xB7 Home");
        else
            snprintf(left, sizeof left,
                     "Loading \xC2\xB7 \xE2\x80\x9C%s\xE2\x80\x9D",
                     a->query);
    } else if (a->feed_mode == FEED_SAVED) {
        snprintf(left, sizeof left, "Saved \xC2\xB7 %zu pins", a->npins);
    } else if (a->feed_mode == FEED_HOME) {
        snprintf(left, sizeof left, "Home \xC2\xB7 %zu pins", a->npins);
    } else {
        snprintf(left, sizeof left,
                 "\xE2\x80\x9C%s\xE2\x80\x9D \xC2\xB7 %zu pins", a->query,
                 a->npins);
    }
    int lh = fonts_line_h(a->fonts, a->M.small_size, false);
    float ty = y + (a->M.footer_h - (float)lh) * 0.5f;
    fonts_draw(r->r, a->fonts, left, a->M.pad, ty, a->M.small_size, false,
               lc);

    char right[PC_SHORT_LEN + 64];
    snprintf(right, sizeof right, "%s \xC2\xB7 %s", a->host, APP_NAME);
    draw_text_right(a, right, a->M.small_size, false, W - a->M.pad, ty,
                    THEME.text_faint);
}

static void app_draw_error(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w;
    float top = content_top(a);
    float bot = content_bottom(a);
    float s = r->scale;

    float pw = fminf(560.0f * s, W - 80.0f * s);
    float ph = fminf(330.0f * s, (bot - top) - 60.0f * s);
    float px = (W - pw) * 0.5f;
    float py = top + ((bot - top) - ph) * 0.5f;

    rnd_shadow(r, px, py + 2.0f * s, pw, ph, a->M.panel_radius, 18.0f * s,
               40);
    rnd_round(r, px, py, pw, ph, a->M.panel_radius, THEME.surface);

    float cx = px + pw * 0.5f;
    float icon = 62.0f * s;
    rnd_round(r, cx - icon * 0.5f, py + 34.0f * s, icon, icon, icon * 0.5f,
              THEME.error_bg);
    int ih = fonts_line_h(a->fonts, a->M.h1_size, true);
    draw_text_center(a, "!", a->M.h1_size, true, cx,
                     py + 34.0f * s + (icon - (float)ih) * 0.5f,
                     THEME.error_text);

    draw_text_center(a, "Can't reach Pinterest", a->M.h2_size, true, cx,
                     py + 112.0f * s, THEME.text);

    char lines[4][FONT_LINE_MAX];
    int nl = fonts_wrap(a->fonts, a->error[0] ? a->error : "Unknown error",
                        a->M.body_size, false, pw - 80.0f * s, lines, 3);
    for (int i = 0; i < nl; i++)
        draw_text_center(a, lines[i], a->M.body_size, false, cx,
                         py + 148.0f * s +
                             (float)i * (float)(a->M.body_line + 2),
                         THEME.text_dim);

    float bw = 148.0f * s, bh = 42.0f * s;
    bool hov = point_in_f(
        (SDL_FRect){cx - bw * 0.5f, py + ph - bh - 34.0f * s, bw, bh},
        (float)a->mx, (float)a->my);
    rnd_round(r, cx - bw * 0.5f, py + ph - bh - 34.0f * s, bw, bh, bh * 0.5f,
              hov ? col_lerp(THEME.brand, (SDL_Color){0, 0, 0, 255}, 0.12f)
                  : THEME.brand);
    int bth = fonts_line_h(a->fonts, a->M.body_size, true);
    draw_text_center(a, "Try again", a->M.body_size, true, cx,
                     py + ph - bh - 34.0f * s + (bh - (float)bth) * 0.5f,
                     THEME.on_brand);

    int nh = fonts_line_h(a->fonts, a->M.small_size, false);
    draw_text_center(a, "Check your connection or search again",
                     a->M.small_size, false, cx,
                     py + ph - 22.0f * s - (float)nh, THEME.text_faint);
}

static void app_draw_banner(App *a)
{
    if (!a->error[0] || a->state != VIEW_FEED)
        return;
    Rnd *r = &a->rnd;
    float W = (float)r->out_w;
    float s = r->scale;
    float bw = W - 2.0f * a->M.pad;
    float bh = 46.0f * s;
    float bx = a->M.pad, by = content_top(a) + 10.0f * s;
    a->banner_rect.x = (int)bx;
    a->banner_rect.y = (int)by;
    a->banner_rect.w = (int)bw;
    a->banner_rect.h = (int)bh;

    rnd_round(r, bx, by, bw, bh, 10.0f * s, THEME.error_bg);
    int lh = fonts_line_h(a->fonts, a->M.small_size, false);
    float ty = by + (bh - (float)lh) * 0.5f;
    char msg[600];
    snprintf(msg, sizeof msg, "%s", a->error);
    rnd_push_clip(r, bx + 14.0f * s, by, bw - 210.0f * s, bh);
    fonts_draw(r->r, a->fonts, msg, bx + 14.0f * s, ty, a->M.small_size,
               false, THEME.error_text);
    rnd_pop_clip(r);
    draw_text_right(a, "Click to retry", a->M.small_size, true,
                    bx + bw - 14.0f * s, ty, THEME.error_text);
} 
static void app_draw_toast(App *a)
{
    if (!a->toast[0] || SDL_GetTicks() >= a->toast_until)
        return;
    Rnd *r = &a->rnd;
    float s = r->scale;
    int tw = 0;
    fonts_measure(a->fonts, a->toast, a->M.small_size, false, &tw, NULL);
    float padx = 18.0f * s;
    float pw = (float)tw + padx * 2.0f;
    float ph = (float)fonts_line_h(a->fonts, a->M.small_size, false) +
               14.0f * s;
    float px = ((float)r->out_w - pw) * 0.5f;
    float py = content_bottom(a) - ph - 22.0f * s;
    float age = 1.0f;
    Uint32 now = SDL_GetTicks();
    Uint32 left = a->toast_until > now ? a->toast_until - now : 0;
    if (left < 350)
        age = (float)left / 350.0f;
    Uint8 a1 = (Uint8)(225.0f * age);
    rnd_round(r, px, py, pw, ph, ph * 0.5f, col_a(THEME.text, a1));
    draw_text_center(a, a->toast, a->M.small_size, false,
                     (float)r->out_w * 0.5f, py + 7.0f * s,
                     col_a(THEME.bg, a1));
}

static void draw_heart_icon(App *a, float cx, float cy, float r,
                            SDL_Color c, bool filled)
{
    if (filled) {
        float lw = r * 1.02f, lh = r * 0.95f;
        rnd_round(&a->rnd, cx - r * 1.02f, cy - r * 0.92f, lw, lh, lh * 0.5f,
                  c);
        rnd_round(&a->rnd, cx - r * 0.02f, cy - r * 0.92f, lw, lh, lh * 0.5f,
                  c);
        SDL_Vertex v[3];
        Uint16 idx[3] = {0, 1, 2};
        (void)idx;
        int ind[3] = {0, 1, 2};
        memset(v, 0, sizeof v);
        v[0].position.x = cx - r * 1.0f;
        v[0].position.y = cy - r * 0.15f;
        v[1].position.x = cx + r * 1.0f;
        v[1].position.y = cy - r * 0.15f;
        v[2].position.x = cx;
        v[2].position.y = cy + r * 1.05f;
        for (int i = 0; i < 3; i++)
            v[i].color = c;
        SDL_RenderGeometry(a->rnd.r, NULL, v, 3, ind, 3);
    } else {
        float t = fmaxf(1.3f, 1.8f * a->rnd.scale);
        rnd_round_outline(&a->rnd, cx - r * 1.02f, cy - r * 0.92f,
                          r * 1.02f, r * 0.95f, r * 0.48f, t, c);
        rnd_round_outline(&a->rnd, cx - r * 0.02f, cy - r * 0.92f,
                          r * 1.02f, r * 0.95f, r * 0.48f, t, c);
        rnd_line(&a->rnd, cx - r * 1.0f, cy - r * 0.05f, cx,
                 cy + r * 1.02f, t, c);
        rnd_line(&a->rnd, cx + r * 1.0f, cy - r * 0.05f, cx,
                 cy + r * 1.02f, t, c);
    }
}

static void draw_bubble_icon(App *a, float cx, float cy, float r, SDL_Color c)
{
    float t = fmaxf(1.2f, 1.6f * a->rnd.scale);
    SDL_FRect b = {cx - r, cy - r * 0.8f, r * 2.0f, r * 1.45f};
    rnd_round_outline(&a->rnd, b.x, b.y, b.w, b.h, r * 0.55f, t, c);
    rnd_line(&a->rnd, cx - r * 0.45f, cy + r * 0.7f, cx - r * 0.1f,
             cy + r * 1.15f, t, c);
    rnd_line(&a->rnd, cx - r * 0.1f, cy + r * 1.15f, cx + r * 0.35f,
             cy + r * 0.66f, t, c);
}

static void draw_share_icon(App *a, float cx, float cy, float r, SDL_Color c)
{
    float t = fmaxf(1.4f, 1.8f * a->rnd.scale);
    rnd_line(&a->rnd, cx - r * 0.85f, cy + r * 0.25f, cx + r * 0.55f,
             cy - r * 0.85f, t, c);
    rnd_line(&a->rnd, cx + r * 0.05f, cy - r * 0.9f, cx + r * 0.7f,
             cy - r * 0.9f, t, c);
    rnd_line(&a->rnd, cx + r * 0.65f, cy - r * 0.9f, cx + r * 0.65f,
             cy - r * 0.25f, t, c);
}

static void draw_more_icon(App *a, float cx, float cy, float r, SDL_Color c)
{
    float d = r * 0.42f;
    for (int i = -1; i <= 1; i++)
        rnd_round(&a->rnd, cx + (float)i * d * 1.9f - d * 0.5f,
                  cy - d * 0.5f, d, d, d * 0.5f, c);
}

/* ------------------------------------------------ close-up page drawing
 * Pinterest-parity close-up: fixed icon rail + search header, close-up card
 * (image left / info right), a page masonry of recommendation pins and the
 * "Ideas you might love" module. Numbers: docs/UI_CLOSEUP.md.
 */

/* ---- 24px line icons (hand-drawn with SDL primitives) ----------------- */
typedef void (*CuIconFn)(App *a, float cx, float cy, float sz, SDL_Color c);

static void cu_icon_home(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    rnd_line(r, cx - h * 0.72f, cy - h * 0.02f, cx, cy - h * 0.78f, t, c);
    rnd_line(r, cx, cy - h * 0.78f, cx + h * 0.72f, cy - h * 0.02f, t, c);
    rnd_line(r, cx - h * 0.52f, cy - h * 0.12f, cx - h * 0.52f, cy + h * 0.72f,
             t, c);
    rnd_line(r, cx + h * 0.52f, cy - h * 0.12f, cx + h * 0.52f, cy + h * 0.72f,
             t, c);
    rnd_line(r, cx - h * 0.52f, cy + h * 0.72f, cx + h * 0.52f, cy + h * 0.72f,
             t, c);
}

static void cu_icon_boards(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    rnd_round_outline(r, cx - h * 0.78f, cy - h * 0.72f, h * 1.56f, h * 1.44f,
                      3.0f * a->rnd.scale, t, c);
    rnd_line(r, cx - h * 0.05f, cy - h * 0.66f, cx - h * 0.05f, cy + h * 0.66f,
             t, c);
    rnd_line(r, cx - h * 0.72f, cy - h * 0.12f, cx + h * 0.72f, cy - h * 0.12f,
             t, c);
}

static void cu_icon_create(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    rnd_round_outline(r, cx - h * 0.78f, cy - h * 0.78f, h * 1.56f, h * 1.56f,
                      3.0f * a->rnd.scale, t, c);
    rnd_line(r, cx - h * 0.34f, cy, cx + h * 0.34f, cy, t, c);
    rnd_line(r, cx, cy - h * 0.34f, cx, cy + h * 0.34f, t, c);
}

static void cu_icon_bell(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    SDL_FRect dome = {cx - h * 0.62f, cy - h * 0.62f, h * 1.24f, h * 1.5f};
    rnd_round_outline(r, dome.x, dome.y, dome.w, dome.h, h * 0.6f, t, c);
    rnd_line(r, cx - h * 0.82f, cy + h * 0.34f, cx + h * 0.82f, cy + h * 0.34f,
             t, c);
    rnd_line(r, cx, cy + h * 0.34f, cx, cy + h * 0.62f, t, c);
}

static void cu_icon_chat(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    rnd_round_outline(r, cx - h * 0.8f, cy - h * 0.66f, h * 1.6f, h * 1.2f,
                      h * 0.5f, t, c);
    rnd_line(r, cx - h * 0.4f, cy + h * 0.5f, cx - h * 0.2f, cy + h * 0.82f,
             t, c);
    rnd_line(r, cx - h * 0.2f, cy + h * 0.82f, cx + h * 0.1f, cy + h * 0.5f,
             t, c);
    for (int i = -1; i <= 1; i++)
        rnd_round(r, cx + (float)i * h * 0.34f - t * 0.9f,
                  cy + h * 0.02f - t * 0.9f, t * 1.8f, t * 1.8f, t, c);
}

static void cu_icon_tune(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    for (int i = -1; i <= 1; i++) {
        float x = cx + (float)i * h * 0.52f;
        rnd_line(r, x, cy - h * 0.8f, x, cy + h * 0.8f, t, c);
        float ky = cy + ((i == 0) ? h * 0.3f : (i < 0 ? -h * 0.3f : h * 0.05f));
        rnd_line(r, x - h * 0.2f, ky, x + h * 0.2f, ky, t, c);
    }
}

static void cu_icon_chevron(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    rnd_line(r, cx - h * 0.45f, cy - h * 0.2f, cx, cy + h * 0.28f, t, c);
    rnd_line(r, cx, cy + h * 0.28f, cx + h * 0.45f, cy - h * 0.2f, t, c);
}

static void cu_icon_lens(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    draw_search_icon(a, cx - h * 0.12f, cy - h * 0.1f, h * 0.52f, c);
    /* corner brackets around the lens */
    float b = h * 0.86f;
    float len = h * 0.34f;
    rnd_line(r, cx - b, cy - b + len, cx - b, cy - b, t, c);
    rnd_line(r, cx - b, cy - b, cx - b + len, cy - b, t, c);
    rnd_line(r, cx + b - len, cy - b, cx + b, cy - b, t, c);
    rnd_line(r, cx + b, cy - b, cx + b, cy - b + len, t, c);
}

static void cu_icon_mic(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    rnd_round_outline(r, cx - h * 0.28f, cy - h * 0.82f, h * 0.56f, h * 1.06f,
                      h * 0.28f, t, c);
    rnd_line(r, cx - h * 0.5f, cy - h * 0.06f, cx - h * 0.5f, cy + h * 0.18f,
             t, c);
    rnd_line(r, cx + h * 0.5f, cy - h * 0.06f, cx + h * 0.5f, cy + h * 0.18f,
             t, c);
    rnd_line(r, cx - h * 0.5f, cy + h * 0.18f, cx + h * 0.5f, cy + h * 0.18f,
             t, c);
    rnd_line(r, cx, cy + h * 0.18f, cx, cy + h * 0.78f, t, c);
}

static void cu_icon_expand(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.4f, 2.0f * a->rnd.scale);
    rnd_line(r, cx - h * 0.7f, cy + h * 0.7f, cx - h * 0.2f, cy + h * 0.2f, t,
             c);
    rnd_line(r, cx - h * 0.2f, cy + h * 0.2f, cx - h * 0.62f, cy + h * 0.2f, t,
             c);
    rnd_line(r, cx - h * 0.2f, cy + h * 0.2f, cx - h * 0.2f, cy + h * 0.62f, t,
             c);
    rnd_line(r, cx + h * 0.7f, cy - h * 0.7f, cx + h * 0.2f, cy - h * 0.2f, t,
             c);
    rnd_line(r, cx + h * 0.2f, cy - h * 0.2f, cx + h * 0.62f, cy - h * 0.2f, t,
             c);
    rnd_line(r, cx + h * 0.2f, cy - h * 0.2f, cx + h * 0.2f, cy - h * 0.62f, t,
             c);
}

static void cu_icon_smiley(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.3f, 1.8f * a->rnd.scale);
    rnd_round_outline(r, cx - h * 0.85f, cy - h * 0.85f, h * 1.7f, h * 1.7f,
                      h * 0.85f, t, c);
    rnd_round(r, cx - h * 0.34f - t, cy - h * 0.2f - t, t * 2.0f, t * 2.0f, t,
              c);
    rnd_round(r, cx + h * 0.34f - t, cy - h * 0.2f - t, t * 2.0f, t * 2.0f, t,
              c);
    rnd_line(r, cx - h * 0.34f, cy + h * 0.3f, cx + h * 0.34f, cy + h * 0.3f,
             t, c);
}

static void cu_icon_note(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.3f, 1.8f * a->rnd.scale);
    rnd_round_outline(r, cx - h * 0.8f, cy - h * 0.74f, h * 1.6f, h * 1.48f,
                      4.0f * a->rnd.scale, t, c);
    rnd_line(r, cx - h * 0.48f, cy - h * 0.28f, cx + h * 0.48f, cy - h * 0.28f,
             t, c);
    rnd_line(r, cx - h * 0.48f, cy + h * 0.12f, cx + h * 0.16f, cy + h * 0.12f,
             t, c);
}

static void cu_icon_image(App *a, float cx, float cy, float sz, SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.3f, 1.8f * a->rnd.scale);
    rnd_round_outline(r, cx - h * 0.85f, cy - h * 0.72f, h * 1.7f, h * 1.44f,
                      3.0f * a->rnd.scale, t, c);
    rnd_round(r, cx - h * 0.38f - t, cy - h * 0.34f - t, t * 2.4f, t * 2.4f,
              t, c);
    rnd_line(r, cx - h * 0.7f, cy + h * 0.5f, cx - h * 0.1f, cy - h * 0.06f, t,
             c);
    rnd_line(r, cx - h * 0.1f, cy - h * 0.06f, cx + h * 0.3f, cy + h * 0.28f,
             t, c);
    rnd_line(r, cx + h * 0.3f, cy + h * 0.28f, cx + h * 0.68f, cy - h * 0.1f,
             t, c);
}

static void cu_icon_arrow_left(App *a, float cx, float cy, float sz,
                               SDL_Color c)
{
    Rnd *r = &a->rnd;
    float h = sz * 0.5f;
    float t = fmaxf(1.6f, 2.2f * a->rnd.scale);
    rnd_line(r, cx - h * 0.7f, cy, cx + h * 0.6f, cy, t, c);
    rnd_line(r, cx - h * 0.7f, cy, cx - h * 0.16f, cy - h * 0.5f, t, c);
    rnd_line(r, cx - h * 0.7f, cy, cx - h * 0.16f, cy + h * 0.5f, t, c);
}

/* circular avatar with the first UTF-8 code point of `name` */
static void cu_avatar(App *a, float cx, float cy, float d, const char *name)
{
    Rnd *r = &a->rnd;
    rnd_round(r, cx - d * 0.5f, cy - d * 0.5f, d, d, d * 0.5f,
              THEME.brand_soft);
    char initial[6] = {0};
    if (name && name[0]) {
        unsigned char c0 = (unsigned char)name[0];
        size_t cl = c0 >= 0xF0 ? 4u : c0 >= 0xE0 ? 3u : c0 >= 0xC0 ? 2u : 1u;
        if (cl > sizeof initial - 1)
            cl = 1;
        memcpy(initial, name, cl);
        initial[cl] = '\0';
    }
    int sz = (int)lroundf(d * 0.5f);
    if (sz < 8)
        sz = 8;
    int lh = fonts_line_h(a->fonts, sz, true);
    draw_text_center(a, initial[0] ? initial : "?", sz, true, cx,
                     cy - (float)lh * 0.5f, THEME.error_text);
}

/* Search field sub-rects (fixed header chrome, screen coordinates). */
static SDL_FRect cu_search_lens(App *a)
{
    SDL_FRect sr = a->d_search_rect;
    return (SDL_FRect){sr.x + sr.w - 100.0f * a->rnd.scale, sr.y,
                       48.0f * a->rnd.scale, sr.h};
}

static SDL_FRect cu_search_mic(App *a)
{
    SDL_FRect b = cu_search_lens(a);
    b.x += 48.0f * a->rnd.scale;
    return b;
}

static SDL_FRect cu_search_clear(App *a)
{
    SDL_FRect b = cu_search_lens(a);
    float s = a->rnd.scale;
    return (SDL_FRect){b.x - 40.0f * s, b.y + 8.0f * s, 32.0f * s, 32.0f * s};
}

static void app_draw_cu_rail(App *a)
{
    Rnd *r = &a->rnd;
    float s = r->scale;
    float H = (float)r->out_h;
    rnd_fill(r, 0.0f, 0.0f, a->d_rail_w, H, THEME.surface);
    for (int i = 0; i < 8; i++) {
        SDL_FRect b = a->d_nav_btns[i];
        if (b.w <= 0.0f)
            continue;
        bool hov = point_in_f(b, (float)a->mx, (float)a->my);
        if (hov)
            rnd_round(r, b.x, b.y, b.w, b.h, b.w * 0.5f, THEME.surface_hover);
        float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
        SDL_Color c = THEME.text;
        switch (i) {
        case 0: {
            int sz = (int)lroundf(26.0f * s);
            int lh = fonts_line_h(a->fonts, sz, true);
            draw_text_center(a, "P", sz, true, cx, cy - (float)lh * 0.5f,
                             THEME.brand);
            break;
        }
        case 1:
            cu_icon_home(a, cx, cy, CU_ICON * s, c);
            break;
        case 2:
            cu_icon_boards(a, cx, cy, CU_ICON * s, c);
            break;
        case 3:
            cu_icon_create(a, cx, cy, CU_ICON * s, c);
            break;
        case 4:
            cu_icon_bell(a, cx, cy, CU_ICON * s, c);
            rnd_round(r, cx + 7.0f * s - 4.0f * s, cy - 9.0f * s - 4.0f * s,
                      8.0f * s, 8.0f * s, 4.0f * s, THEME.brand);
            break;
        case 5:
            cu_icon_chat(a, cx, cy, CU_ICON * s, c);
            break;
        case 6:
            cu_icon_tune(a, cx, cy, CU_ICON * s, c);
            break;
        default:
            draw_gear_icon(a, cx, cy, 12.0f * s, c);
            break;
        }
        if (hov) {
            const char *tips[8] = {"Home", "Home", "Your boards", "Create",
                                   "Updates", "Messages",
                                   "Refine your recommendations",
                                   "Settings & Support"};
            draw_tooltip(a, tips[i], cx, b.y + b.h + 4.0f * s);
        }
    }
}

static void app_draw_cu_header(App *a)
{
    Rnd *r = &a->rnd;
    float s = r->scale;
    float W = (float)r->out_w;
    SDL_FRect hd = a->d_header;
    rnd_fill(r, hd.x, 0.0f, hd.w, hd.h, THEME.surface);

    SDL_FRect sr = a->d_search_rect;
    rnd_round(r, sr.x, sr.y, sr.w, sr.h, CU_SEARCH_RADIUS * s,
              THEME.surface_alt);
    if (a->search_focus)
        rnd_round_outline(r, sr.x, sr.y, sr.w, sr.h, CU_SEARCH_RADIUS * s,
                          fmaxf(1.5f, 2.0f * s), THEME.brand);

    float icon_sz = 16.0f * s;
    draw_search_icon(a, sr.x + 16.0f * s + icon_sz * 0.5f,
                     sr.y + sr.h * 0.5f, icon_sz * 0.5f, THEME.text);

    SDL_FRect lens = cu_search_lens(a);
    SDL_FRect mic = cu_search_mic(a);
    float text_x = sr.x + 40.0f * s;
    float text_avail = lens.x - 8.0f * s - text_x;
    if (text_avail < 40.0f * s)
        text_avail = 40.0f * s;
    int line_h = fonts_line_h(a->fonts, a->M.search_size, false);
    float text_y = sr.y + (sr.h - (float)line_h) * 0.5f;
    rnd_push_clip(r, text_x - 2.0f * s, sr.y, text_avail + 4.0f * s, sr.h);
    const char *shown = a->input;
    SDL_Color tcol = THEME.text;
    if (!a->input[0]) {
        shown = "Search";
        tcol = THEME.text_dim;
    }
    size_t caret_off = fonts_utf8_offset(a->input, a->caret);
    int caret_x = 0;
    {
        char prefix[PC_TITLE_LEN];
        size_t cp = caret_off < sizeof prefix ? caret_off : sizeof prefix - 1;
        memcpy(prefix, a->input, cp);
        prefix[cp] = '\0';
        fonts_measure(a->fonts, prefix, a->M.search_size, false, &caret_x,
                      NULL);
    }
    float offset = 0.0f;
    if ((float)caret_x > text_avail - 2.0f * s)
        offset = (float)caret_x - (text_avail - 2.0f * s);
    fonts_draw(r->r, a->fonts, shown, text_x - offset, text_y,
               a->M.search_size, false, tcol);
    if (a->search_focus && a->caret_on)
        rnd_fill(r, text_x - offset + (float)caret_x, sr.y + 12.0f * s,
                 fmaxf(1.5f, 2.0f * s), sr.h - 24.0f * s, THEME.brand);
    rnd_pop_clip(r);

    a->search_clear.w = 0;
    a->search_clear.h = 0;
    if (a->input[0]) {
        SDL_FRect clr = cu_search_clear(a);
        bool hov = point_in_f(clr, (float)a->mx, (float)a->my);
        if (hov)
            rnd_round(r, clr.x, clr.y, clr.w, clr.h, clr.w * 0.5f,
                      THEME.surface);
        SDL_FRect ic = {clr.x + (clr.w - 16.0f * s) * 0.5f,
                        clr.y + (clr.h - 16.0f * s) * 0.5f, 16.0f * s,
                        16.0f * s};
        draw_close_icon(a, ic, THEME.text);
        a->search_clear.x = (int)clr.x;
        a->search_clear.y = (int)clr.y;
        a->search_clear.w = (int)clr.w;
        a->search_clear.h = (int)clr.h;
    }

    bool lhov = point_in_f(lens, (float)a->mx, (float)a->my);
    if (lhov)
        rnd_round(r, lens.x, lens.y, lens.w, lens.h, lens.w * 0.5f,
                  THEME.surface);
    cu_icon_lens(a, lens.x + lens.w * 0.5f, lens.y + lens.h * 0.5f,
                 CU_ICON * s, THEME.text);
    bool mhov = point_in_f(mic, (float)a->mx, (float)a->my);
    if (mhov)
        rnd_round(r, mic.x, mic.y, mic.w, mic.h, mic.w * 0.5f,
                  THEME.surface);
    cu_icon_mic(a, mic.x + mic.w * 0.5f, mic.y + mic.h * 0.5f, CU_ICON * s,
                THEME.text);

    /* profile avatar + account chevron */
    SDL_FRect av = a->d_header_avatar;
    bool ahov = point_in_f(av, (float)a->mx, (float)a->my);
    if (ahov)
        rnd_round(r, av.x - 8.0f * s, av.y - 8.0f * s, av.w + 16.0f * s,
                  av.h + 16.0f * s, (av.w + 16.0f * s) * 0.5f,
                  THEME.surface_alt);
    {
        float d = av.w;
        rnd_round(r, av.x, av.y, d, d, d * 0.5f,
                  a->auth == PC_AUTH_LOGGED_IN ? THEME.brand_soft
                                               : THEME.surface_alt);
        char initial[6] = {0};
        const char *nm = a->username[0] ? a->username : "?";
        unsigned char c0 = (unsigned char)nm[0];
        size_t cl = c0 >= 0xF0 ? 4u : c0 >= 0xE0 ? 3u : c0 >= 0xC0 ? 2u : 1u;
        if (cl > sizeof initial - 1)
            cl = 1;
        memcpy(initial, nm, cl);
        initial[cl] = '\0';
        int isz = (int)lroundf(15.0f * s);
        int ith = fonts_line_h(a->fonts, isz, true);
        draw_text_center(a, initial[0] ? initial : "?",
                         a->auth == PC_AUTH_LOGGED_IN ? isz : 14, true,
                         av.x + d * 0.5f, av.y + (d - (float)ith) * 0.5f,
                         a->auth == PC_AUTH_LOGGED_IN ? THEME.error_text
                                                      : THEME.text_faint);
    }
    SDL_FRect chev = a->d_header_chev;
    bool chov = point_in_f(chev, (float)a->mx, (float)a->my);
    if (chov)
        rnd_round(r, chev.x, chev.y, chev.w, chev.h, chev.w * 0.5f,
                  THEME.surface_hover);
    cu_icon_chevron(a, chev.x + chev.w * 0.5f, chev.y + chev.h * 0.5f,
                    18.0f * s, THEME.text);
    (void)W;
}

static void app_draw_cu_back(App *a)
{
    Rnd *r = &a->rnd;
    float s = r->scale;
    SDL_FRect b = a->d_back_btn;
    bool hov = point_in_f(b, (float)a->mx, (float)a->my);
    rnd_shadow(r, b.x, b.y + 1.0f * s, b.w, b.h, b.w * 0.5f, 6.0f * s, 42);
    rnd_round(r, b.x, b.y, b.w, b.h, b.w * 0.5f,
              hov ? THEME.surface_hover : THEME.surface);
    cu_icon_arrow_left(a, b.x + b.w * 0.5f, b.y + b.h * 0.5f, CU_ICON * s,
                       THEME.text);
    if (hov)
        draw_tooltip(a, "Back to feed", b.x + b.w * 0.5f, b.y + b.h + 6.0f * s);
}

static void app_draw_detail_menu(App *a)
{
    if (!a->detail_menu)
        return;
    Rnd *r = &a->rnd;
    float s = r->scale;
    const char *items[2];
    if (a->detail_menu == 1) {
        items[0] = "Copy link";
        items[1] = "Open on Pinterest";
    } else {
        items[0] = "Open in browser";
        items[1] = "Copy pin id";
    }
    float item_h = 38.0f * s;
    float mw = 208.0f * s;
    float mh = item_h * 2.0f + 10.0f * s;
    SDL_FRect anchor = cu_screen(a, a->detail_menu == 1 ? a->d_share_btn
                                                        : a->d_more_btn);
    float mx = anchor.x;
    if (mx + mw > (float)a->rnd.out_w - 8.0f * s)
        mx = (float)a->rnd.out_w - 8.0f * s - mw;
    float my = anchor.y + anchor.h + 6.0f * s;
    if (my + mh > (float)a->rnd.out_h - 8.0f * s)
        my = anchor.y - mh - 6.0f * s;
    a->d_menu = (SDL_FRect){mx, my, mw, mh};
    rnd_shadow(r, mx, my + 2.0f * s, mw, mh, 10.0f * s, 10.0f * s, 60);
    rnd_round(r, mx, my, mw, mh, 10.0f * s, THEME.surface);
    for (int i = 0; i < 2; i++) {
        SDL_FRect ir = {mx + 4.0f * s, my + 5.0f * s + (float)i * item_h,
                        mw - 8.0f * s, item_h};
        a->d_menu_items[i] = ir;
        bool hov = point_in_f(ir, (float)a->mx, (float)a->my);
        if (hov)
            rnd_round(r, ir.x, ir.y, ir.w, ir.h, 8.0f * s,
                      THEME.surface_hover);
        int sz = (int)lroundf(13.0f * s);
        int th = fonts_line_h(a->fonts, sz, false);
        fonts_draw(r->r, a->fonts, items[i], ir.x + 12.0f * s,
                   ir.y + (ir.h - (float)th) * 0.5f, sz, false, THEME.text);
    }
    a->d_menu_n = 2;
}

static void app_draw_detail(App *a)
{
    if (!a->show_detail)
        return;
    Rnd *r = &a->rnd;
    float s = r->scale;
    float W = (float)r->out_w, H = (float)r->out_h;
    const PcPin *pin = &a->detail_pin;

    app_detail_measure(a);
    app_detail_geom(a);
    rnd_clear(r, THEME.bg);

    float sy = a->d_header.h - a->left_scroll;

    int fs_section = (int)lroundf((float)CU_SECTION_SIZE * s);
    if (fs_section < 10)
        fs_section = 10;
    int fs_title = (int)lroundf((float)CU_TITLE_SIZE * s);
    if (fs_title < 12)
        fs_title = 12;
    int fs_body = (int)lroundf((float)CU_BODY_SIZE * s);
    if (fs_body < 10)
        fs_body = 10;
    int fs_creator = (int)lroundf((float)CU_CREATOR_SIZE * s);
    if (fs_creator < 9)
        fs_creator = 9;
    int fs_meta = a->M.small_size;

    app_draw_cu_rail(a);
    app_draw_cu_header(a);

    /* ================= page content (scrolls under the header) ========= */
    rnd_push_clip(r, a->d_rail_w, a->d_header.h, W - a->d_rail_w,
                  H - a->d_header.h);

    /* ---- recommendation masonry tiles ---- */
    if (a->related_loading && a->d_rec_n == 0) {
        for (int i = 0; i < a->d_cols && i < 9; i++) {
            float cw = a->d_col_w;
            float x = a->d_card.x + (float)(i % a->d_cols) * (cw + CU_GAP * s);
            float yy = a->d_header.h + (float)(i / a->d_cols) * 220.0f * s;
            float phase = 0.5f + 0.5f * sinf(a->anim_t * 2.0f + (float)i);
            rnd_round(r, x, yy, cw, 200.0f * s, CU_CARD_RADIUS * s,
                      col_lerp(THEME.skeleton, THEME.skeleton_hi, phase));
        }
    }
    for (int i = 0; i < a->d_rec_n && i < MAX_REL; i++) {
        SDL_FRect t = a->d_rel_tiles[i];
        t.y += sy;
        if (t.y > H || t.y + t.h < a->d_header.h)
            continue;
        if (a->rel_tex[i])
            rnd_texture(r, a->rel_tex[i], t.x, t.y, t.w, t.h);
        else {
            float phase = 0.5f + 0.5f * sinf(a->anim_t * 2.0f + (float)i);
            rnd_round(r, t.x, t.y, t.w, t.h, CU_CARD_RADIUS * s,
                      col_lerp(THEME.skeleton, THEME.skeleton_hi, phase));
        }
    }

    /* degraded recommendation surface: guest / e1_… pins cannot resolve
     * related pins on Pinterest itself (see docs/UI_CLOSEUP.md). */
    if (a->related_unavailable) {
        SDL_FRect msg = {a->d_card.x, a->d_card_h + sy + 24.0f * s,
                         (float)r->out_w - a->d_card.x - 16.0f * s,
                         80.0f * s};
        if (msg.y + msg.h > a->d_header.h && msg.y < H) {
            fonts_draw(r->r, a->fonts,
                       "Related pins unavailable for this pin",
                       a->d_card.x, msg.y, fs_section, true, THEME.text_dim);
            fonts_draw(r->r, a->fonts,
                       "Sign in with a Pinterest session to load "
                       "recommendations here.",
                       a->d_card.x, msg.y + (float)fs_section + 10.0f * s,
                       fs_meta, false, THEME.text_faint);
        }
    }

    /* ---- "Ideas you might love" module ---- */
    if (a->d_ideas.w > 0.0f) {
        SDL_FRect it = a->d_ideas;
        it.y += sy;
        if (it.y + it.h > a->d_header.h && it.y < H) {
            float line = (float)fonts_line_h(a->fonts, fs_section, true);
            draw_text_center(a, "Ideas you might love", fs_section, true,
                             it.x + it.w * 0.5f, it.y,
                             point_in_f(it, (float)a->mx, (float)a->my)
                                 ? THEME.text
                                 : THEME.text);
            for (int k = 0; k < 2; k++) {
                SDL_FRect row = a->d_idea_rows[k];
                row.y += sy;
                if (row.y > H || row.y + row.h < a->d_header.h)
                    continue;
                int idx = a->d_idea_pin[k];
                if (idx >= 0 && idx < MAX_REL && a->rel_tex[idx])
                    rnd_texture(r, a->rel_tex[idx], row.x, row.y, row.w,
                                row.h);
                else
                    rnd_round(r, row.x, row.y, row.w, row.h,
                              CU_CARD_RADIUS * s, THEME.skeleton);
                SDL_FRect ov = row;
                rnd_round(r, ov.x, ov.y, ov.w, ov.h, CU_CARD_RADIUS * s,
                          THEME.cu_overlay);
                int lh = fonts_line_h(a->fonts, fs_section, true);
                draw_text_center(a, a->d_idea_labels[k], fs_section, true,
                                 row.x + row.w * 0.5f,
                                 row.y + (row.h - (float)lh) * 0.5f,
                                 THEME.on_brand);
                (void)line;
            }
        }
    }

    /* ---- close-up card ---- */
    SDL_FRect card = a->d_card;
    card.y += sy;
    if (card.y + card.h > a->d_header.h && card.y < H) {
        rnd_shadow(r, card.x + 2.0f * s, card.y + 3.0f * s, card.w - 4.0f * s,
                   card.h, CU_CARD_RADIUS * s, 16.0f * s, 34);
        rnd_round(r, card.x, card.y, card.w, card.h, CU_CARD_RADIUS * s,
                  THEME.surface);
        rnd_round_outline(r, card.x, card.y, card.w, card.h,
                          CU_CARD_RADIUS * s, CU_CARD_BORDER * s,
                          THEME.cu_card_border);

        /* image pane */
        SDL_FRect med = a->d_media;
        med.y += sy;
        rnd_push_clip(r, med.x, med.y, med.w, med.h);
        if (a->video_active && a->video_tex && a->video_frames > 0 &&
            a->video_w > 0 && a->video_h > 0) {
            float aspect = (float)a->video_h / (float)a->video_w;
            float iw = med.w, ih = med.w * aspect;
            if (ih > med.h + 0.5f) {
                ih = med.h;
                iw = med.h / aspect;
            }
            rnd_texture(r, a->video_tex, med.x + (med.w - iw) * 0.5f,
                        med.y + (med.h - ih) * 0.5f, iw, ih);
        } else if (a->detail.status == 2 && a->detail.tex &&
                   a->detail.tw > 0 && a->detail.th > 0) {
            float aspect = (float)a->detail.th / (float)a->detail.tw;
            float iw = med.w, ih = med.w * aspect;
            if (ih > med.h + 0.5f) {
                ih = med.h;
                iw = med.h / aspect;
            }
            rnd_texture(r, a->detail.tex, med.x + (med.w - iw) * 0.5f,
                        med.y + (med.h - ih) * 0.5f, iw, ih);
        } else {
            float phase = 0.5f + 0.5f * sinf(a->anim_t * 2.0f);
            rnd_round(r, med.x, med.y, med.w, med.h, CU_CARD_RADIUS * s,
                      col_lerp(THEME.skeleton, THEME.skeleton_hi, phase));
            const char *msg = a->detail.status == 3 ? "Image unavailable"
                                                    : "Loading image\xE2\x80\xA6";
            int lh = fonts_line_h(a->fonts, fs_body, false);
            draw_text_center(a, msg, fs_body, false, med.x + med.w * 0.5f,
                             med.y + med.h * 0.5f - (float)lh * 0.5f,
                             THEME.text_faint);
        }
        rnd_pop_clip(r);

        /* video controls / notes */
        if (a->video_active) {
            if (a->video_failed) {
                const char *msg = a->video_note[0] ? a->video_note
                                                  : "Video unavailable";
                int lh = fonts_line_h(a->fonts, fs_meta, false);
                int tw2 = 0;
                fonts_measure(a->fonts, msg, fs_meta, false, &tw2, NULL);
                SDL_FRect nb = {med.x + 12.0f * s,
                                med.y + med.h - 12.0f * s -
                                    (float)lh - 12.0f * s,
                                (float)tw2 + 20.0f * s,
                                (float)lh + 12.0f * s};
                rnd_round(r, nb.x, nb.y, nb.w, nb.h, 8.0f * s,
                          col_a(THEME.text, 200));
                draw_text_center(a, msg, fs_meta, false,
                                 nb.x + nb.w * 0.5f,
                                 nb.y + (nb.h - (float)lh) * 0.5f,
                                 THEME.on_brand);
            } else {
                SDL_FRect vb = a->d_video_btn;
                vb.y += sy;
                bool hov = point_in_f(vb, (float)a->mx, (float)a->my);
                rnd_round(r, vb.x, vb.y, vb.w, vb.h, vb.w * 0.5f,
                          col_a(THEME.text, hov ? 225 : 190));
                float icx = vb.x + vb.w * 0.5f;
                float icy = vb.y + vb.h * 0.5f;
                if (a->video_playing && !a->video_paused) {
                    float bar_w = 4.0f * s, bar_h = 16.0f * s;
                    rnd_round(r, icx - bar_w - 2.0f * s,
                              icy - bar_h * 0.5f, bar_w, bar_h,
                              bar_w * 0.5f, THEME.on_brand);
                    rnd_round(r, icx + 2.0f * s, icy - bar_h * 0.5f, bar_w,
                              bar_h, bar_w * 0.5f, THEME.on_brand);
                } else {
                    SDL_Vertex v[3];
                    int ind[3] = {0, 1, 2};
                    memset(v, 0, sizeof v);
                    v[0].position.x = icx - 5.0f * s;
                    v[0].position.y = icy - 8.0f * s;
                    v[1].position.x = icx - 5.0f * s;
                    v[1].position.y = icy + 8.0f * s;
                    v[2].position.x = icx + 8.0f * s;
                    v[2].position.y = icy;
                    for (int k = 0; k < 3; k++)
                        v[k].color = THEME.on_brand;
                    SDL_RenderGeometry(r->r, NULL, v, 3, ind, 3);
                }
                /* "Video · MM:SS" label */
                int dur = a->detail_pin.video_duration_ms;
                if (dur < 0)
                    dur = 0;
                char lab[64];
                if (a->video_paused)
                    snprintf(lab, sizeof lab, "Paused \xC2\xB7 %02d:%02d",
                             dur / 60000, (dur / 1000) % 60);
                else
                    snprintf(lab, sizeof lab, "Video \xC2\xB7 %02d:%02d",
                             dur / 60000, (dur / 1000) % 60);
                int lh = fonts_line_h(a->fonts, fs_meta, false);
                int tw2 = 0;
                fonts_measure(a->fonts, lab, fs_meta, false, &tw2, NULL);
                float lx = vb.x + vb.w + 10.0f * s;
                SDL_FRect lb = {lx, vb.y + (vb.h - (float)lh) * 0.5f -
                                       6.0f * s,
                                (float)tw2 + 20.0f * s,
                                (float)lh + 12.0f * s};
                rnd_round(r, lb.x, lb.y, lb.w, lb.h, 8.0f * s,
                          col_a(THEME.text, 170));
                draw_text_center(a, lab, fs_meta, false,
                                 lb.x + lb.w * 0.5f,
                                 lb.y + (lb.h - (float)lh) * 0.5f,
                                 THEME.surface);
            }
        }

        /* image overlay buttons (view larger / search image) */
        for (int i = 0; i < 2; i++) {
            SDL_FRect ob = a->d_overlay_btn[i];
            if (ob.w <= 0.0f)
                continue;
            ob.y += sy;
            bool hov = point_in_f(ob, (float)a->mx, (float)a->my);
            rnd_shadow(r, ob.x, ob.y + 1.0f * s, ob.w, ob.h, ob.w * 0.5f,
                       5.0f * s, 36);
            rnd_round(r, ob.x, ob.y, ob.w, ob.h, ob.w * 0.5f,
                      hov ? THEME.surface_hover : THEME.surface);
            if (i == 0)
                cu_icon_lens(a, ob.x + ob.w * 0.5f, ob.y + ob.h * 0.5f,
                             18.0f * s, THEME.text);
            else
                cu_icon_expand(a, ob.x + ob.w * 0.5f, ob.y + ob.h * 0.5f,
                               18.0f * s, THEME.text);
        }

        /* ---- info column ---- */
        float ix = a->d_content_x;

        /* action bar */
        {
            SDL_FRect b = a->d_like_btn;
            b.y += sy;
            bool hov = point_in_f(b, (float)a->mx, (float)a->my);
            bool like_enabled = (a->auth_preview || writes_enabled(a)) &&
                                (a->auth_preview ||
                                 app_detail_canonical_id(pin->id));
            if (hov)
                rnd_round(r, b.x, b.y, b.w, b.h, b.w * 0.5f,
                          THEME.surface_hover);
            draw_heart_icon(a, b.x + b.w * 0.5f, b.y + b.h * 0.5f, 11.0f * s,
                            !like_enabled
                                ? THEME.text_faint
                                : (a->detail_liked ? THEME.brand
                                                   : THEME.text),
                            a->detail_liked);
            char cnt[32];
            snprintf(cnt, sizeof cnt, "%d", a->detail_like_count);
            fonts_draw(r->r, a->fonts, cnt, a->d_like_count.x,
                       a->d_like_count.y + sy, fs_section, true,
                       like_enabled ? THEME.text : THEME.text_faint);
        }
        {
            SDL_FRect b = a->d_comment_btn;
            b.y += sy;
            bool hov = point_in_f(b, (float)a->mx, (float)a->my);
            if (hov)
                rnd_round(r, b.x, b.y, b.w, b.h, b.w * 0.5f,
                          THEME.surface_hover);
            draw_bubble_icon(a, b.x + b.w * 0.5f, b.y + b.h * 0.5f,
                             11.0f * s, THEME.text);
        }
        {
            SDL_FRect b = a->d_share_btn;
            b.y += sy;
            bool hov = point_in_f(b, (float)a->mx, (float)a->my);
            if (hov)
                rnd_round(r, b.x, b.y, b.w, b.h, b.w * 0.5f,
                          THEME.surface_hover);
            draw_share_icon(a, b.x + b.w * 0.5f, b.y + b.h * 0.5f,
                            11.0f * s, THEME.text);
            SDL_FRect b2 = a->d_more_btn;
            b2.y += sy;
            bool hov2 = point_in_f(b2, (float)a->mx, (float)a->my);
            if (hov2)
                rnd_round(r, b2.x, b2.y, b2.w, b2.h, b2.w * 0.5f,
                          THEME.surface_hover);
            draw_more_icon(a, b2.x + b2.w * 0.5f, b2.y + b2.h * 0.5f,
                           5.0f * s, THEME.text);
        }

        /* board dropdown (plain bold label + chevron) */
        {
            SDL_FRect d = a->d_dropdown;
            d.y += sy;
            bool hov = point_in_f(d, (float)a->mx, (float)a->my);
            if (hov)
                rnd_round(r, d.x, d.y, d.w, d.h, d.h * 0.5f,
                          THEME.surface_hover);
            const char *board = pin->board_name[0]
                                    ? pin->board_name
                                    : (pin->pinner_name[0]
                                           ? pin->pinner_name
                                           : (pin->domain[0] ? pin->domain
                                                             : "Profile"));
            int fsv = (int)lroundf(16.0f * s);
            int lh = fonts_line_h(a->fonts, fsv, true);
            char label[PC_BOARD_NAME_LEN + 8];
            fonts_ellipsize(a->fonts, board, fsv, true, d.w - 40.0f * s,
                            label, sizeof label);
            fonts_draw(r->r, a->fonts, label, d.x, d.y +
                           (d.h - (float)lh) * 0.5f, fsv, true, THEME.text);
            cu_icon_chevron(a, d.x + d.w - 20.0f * s, d.y + d.h * 0.5f,
                            16.0f * s, THEME.text);
        }
        /* Save pill */
        {
            SDL_FRect b = a->d_save_btn;
            b.y += sy;
            bool en = writes_enabled(a);
            bool hov = point_in_f(b, (float)a->mx, (float)a->my);
            rnd_round(r, b.x, b.y, b.w, b.h, b.h * 0.5f,
                      en ? (hov ? THEME.brand_hover : THEME.brand)
                         : THEME.surface_alt);
            int fsv = (int)lroundf(16.0f * s);
            int lh = fonts_line_h(a->fonts, fsv, false);
            draw_text_center(a, "Save", fsv, false, b.x + b.w * 0.5f,
                             b.y + (b.h - (float)lh) * 0.5f,
                             en ? THEME.on_brand : THEME.text_faint);
            if (!en && hov)
                draw_tooltip(a, "Sign in to save", b.x + b.w * 0.5f,
                             b.y + b.h + 6.0f * s);
        }

        /* creator row */
        {
            SDL_FRect cr = a->d_creator;
            cr.y += sy;
            cu_avatar(a, cr.x + 4.0f * s + CU_AVATAR * s * 0.5f,
                      cr.y + cr.h * 0.5f, CU_AVATAR * s, pin->pinner_name[0]
                                                          ? pin->pinner_name
                                                          : pin->domain);
            const char *who = pin->pinner_name[0]
                                  ? pin->pinner_name
                                  : (pin->domain[0] ? pin->domain : "Pinterest");
            char shown[PC_SHORT_LEN];
            fonts_ellipsize(a->fonts, who, fs_creator, true,
                            a->d_content_w - 40.0f * s, shown, sizeof shown);
            int lh = fonts_line_h(a->fonts, fs_creator, true);
            fonts_draw(r->r, a->fonts, shown, cr.x + 32.0f * s,
                       cr.y + (cr.h - (float)lh) * 0.5f, fs_creator, true,
                       THEME.text);
        }

        /* title */
        {
            float ty = a->d_title_y + sy;
            float line = (float)CU_TITLE_LINE * s;
            for (int i = 0; i < a->d_title_n; i++)
                fonts_draw(r->r, a->fonts, a->d_title[i], ix,
                           ty + (float)i * line, fs_title, true, THEME.text);
        }

        /* description */
        if (a->d_desc_n > 0) {
            float dy = a->d_desc_y + sy;
            float sec_line = (float)CU_SECTION_LINE * s;
            float body_line = 22.0f * s;
            fonts_draw(r->r, a->fonts, "Description", ix, dy, fs_section, true,
                       THEME.text);
            int shown = a->desc_expanded ? a->d_desc_n : a->d_desc_n;
            if (!a->desc_expanded) {
                float text_top = dy + sec_line + 4.0f * s;
                float avail = a->d_composer.y - 8.0f * s - text_top -
                              24.0f * s - sec_line;
                int max_show = (int)(avail / body_line);
                if (max_show < 1)
                    max_show = 1;
                if (max_show > 6)
                    max_show = 6;
                shown = a->d_desc_n < max_show ? a->d_desc_n : max_show;
            }
            for (int i = 0; i < shown && i < 20; i++)
                fonts_draw(r->r, a->fonts, a->d_desc[i], ix,
                           dy + sec_line + 4.0f * s +
                               (float)i * body_line,
                           fs_body, false, THEME.text);
            if (a->d_desc_more.w > 0.0f) {
                SDL_FRect m = a->d_desc_more;
                m.y += sy;
                bool hov = point_in_f(m, (float)a->mx, (float)a->my);
                fonts_draw(r->r, a->fonts,
                           a->desc_expanded ? "less" : "more", m.x, m.y,
                           a->M.small_size, true,
                           hov ? THEME.brand : THEME.text_dim);
            }
        }

        /* divider */
        rnd_fill(r, ix, a->d_divider_y + sy, a->d_content_w, 1.0f * s,
                 THEME.cu_divider);

        /* ---- comments region (independent scroll) ---- */
        float vtop = a->d_comments_top + sy;
        rnd_push_clip(r, ix - 2.0f * s, vtop, a->d_content_w + 6.0f * s,
                      a->detail_view_h);
        {
            float cline = (float)CU_SECTION_LINE * s;
            char head[64];
            int count = app_detail_comment_count(a);
            snprintf(head, sizeof head, "%d %s", count,
                     count == 1 ? "Comment" : "Comments");
            float hy = vtop - a->detail_scroll;
            int hlh = fonts_line_h(a->fonts, fs_section, true);
            fonts_draw(r->r, a->fonts, head, ix, hy, fs_section, true,
                       THEME.text);
            cu_icon_chevron(a, ix + a->d_content_w - 16.0f * s,
                            hy + (float)hlh * 0.5f, 16.0f * s, THEME.text);
            float cy = hy + cline + 16.0f * s;
            if (a->comments_unavailable) {
                fonts_draw(r->r, a->fonts,
                           "Comments unavailable for this pin", ix, cy,
                           fs_meta, false, THEME.text_faint);
            } else if (a->comments_loading) {
                fonts_draw(r->r, a->fonts, "Loading comments\xE2\x80\xA6", ix,
                           cy, fs_meta, false, THEME.text_dim);
            } else if (a->comments_err[0]) {
                char lines[2][FONT_LINE_MAX];
                int nl = fonts_wrap(a->fonts, a->comments_err, fs_meta, false,
                                    a->d_content_w, lines, 2);
                for (int i = 0; i < nl; i++)
                    fonts_draw(r->r, a->fonts, lines[i], ix,
                               cy + (float)i * (float)(fs_meta + 4), fs_meta,
                               false, THEME.error_text);
            } else if (a->comments.count == 0) {
                fonts_draw(r->r, a->fonts, "No comments yet", ix, cy, fs_meta,
                           false, THEME.text_faint);
            } else {
                for (size_t i = 0; i < a->comments.count; i++) {
                    PcComment *cm = &a->comments.items[i];
                    float name_size_f = (float)fs_section;
                    (void)name_size_f;
                    float name_w = 0.0f;
                    int nw = 0;
                    fonts_measure(a->fonts, cm->user_name, fs_section, true,
                                  &nw, NULL);
                    name_w = (float)nw;
                    /* inline for one-line comments, stacked otherwise */
                    char lines[3][FONT_LINE_MAX];
                    int nl = fonts_wrap(a->fonts, cm->text, fs_body, false,
                                        a->d_content_w - 40.0f * s, lines, 3);
                    bool inline_one = nl == 1;
                    float av = CU_COMMENT_AVATAR * s;
                    cu_avatar(a, ix + av * 0.5f, cy + av * 0.5f, av,
                              cm->user_name);
                    int alh = fonts_line_h(a->fonts, fs_section, true);
                    fonts_draw(r->r, a->fonts, cm->user_name,
                               ix + av + 8.0f * s,
                               cy + (av - (float)alh) * 0.5f, fs_section,
                               true, THEME.text);
                    float text_x = ix + av + 8.0f * s;
                    if (inline_one) {
                        float nw2 = 0.0f;
                        fonts_measure(a->fonts, cm->user_name, fs_section,
                                      true, NULL, NULL);
                        (void)nw2;
                        fonts_draw(r->r, a->fonts, lines[0],
                                   text_x + name_w + 6.0f * s,
                                   cy + (av - (float)alh) * 0.5f, fs_body,
                                   false, THEME.cu_comment_ink);
                        cy += av + 8.0f * s;
                    } else {
                        cy += av + 4.0f * s;
                        for (int k = 0; k < nl; k++)
                            fonts_draw(r->r, a->fonts, lines[k], text_x,
                                       cy + (float)k * (float)CU_COMMENT_LINE *
                                                s,
                                       fs_body, false,
                                       THEME.cu_comment_ink);
                        cy += (float)nl * (float)CU_COMMENT_LINE * s + 4.0f * s;
                    }
                    char meta2[96];
                    snprintf(meta2, sizeof meta2, "%s \xC2\xB7 \xE2\x99\xA5 %d",
                             cm->created_at[0] ? cm->created_at : "now",
                             cm->like_count);
                    fonts_draw(r->r, a->fonts, meta2, text_x, cy, fs_meta,
                               false, THEME.text_faint);
                    cy += (float)fonts_line_h(a->fonts, fs_meta, false) +
                          8.0f * s;
                }
                if (a->d_viewmore_btn.w > 0.0f) {
                    SDL_FRect vm = a->d_viewmore_btn;
                    vm.y += sy - a->detail_scroll;
                    bool hov = point_in_f(vm, (float)a->mx, (float)a->my);
                    rnd_round(r, vm.x, vm.y, vm.w, vm.h, vm.h * 0.5f,
                              hov ? THEME.surface_hover : THEME.surface_alt);
                    int bh2 = fonts_line_h(a->fonts, a->M.small_size, true);
                    draw_text_center(a, a->rel_loading_more
                                             ? "Loading\xE2\x80\xA6"
                                             : "View more comments",
                                     a->M.small_size, true,
                                     vm.x + vm.w * 0.5f,
                                     vm.y + (vm.h - (float)bh2) * 0.5f,
                                     THEME.text);
                }
            }
        }
        rnd_pop_clip(r);

        /* comments scrollbar */
        a->d_sb_thumb.w = 0.0f;
        a->d_sb_thumb.h = 0.0f;
        if (a->detail_content_h > a->detail_view_h + 1.0f) {
            float vh = a->detail_view_h;
            float maxs = a->detail_content_h - vh;
            float th = vh * vh / a->detail_content_h;
            if (th < 36.0f * s)
                th = 36.0f * s;
            float t = a->detail_scroll / maxs;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            float track_y = a->d_sb_track.y + sy;
            a->d_sb_thumb =
                (SDL_FRect){a->d_sb_track.x, track_y + t * (vh - th),
                            a->d_sb_track.w, th};
            rnd_round(r, a->d_sb_thumb.x, a->d_sb_thumb.y, a->d_sb_thumb.w,
                      a->d_sb_thumb.h, a->d_sb_thumb.w * 0.5f,
                      col_a(a->detail_drag_sb ? THEME.text_dim
                                              : THEME.text_faint,
                            170));
        }

        /* ---- composer pill ---- */
        {
            SDL_FRect c = a->d_composer;
            c.y += sy;
            bool enabled = (a->auth_preview || writes_enabled(a)) &&
                           (a->auth_preview ||
                            app_detail_canonical_id(pin->id));
            bool focus = a->composer_focus && enabled;
            rnd_round(r, c.x, c.y, c.w, c.h, 24.0f * s, THEME.surface);
            rnd_round_outline(r, c.x, c.y, c.w, c.h, 24.0f * s,
                              fmaxf(1.0f, CU_CARD_BORDER * s),
                              focus ? THEME.brand : THEME.cu_card_border);
            if (focus)
                rnd_round_outline(r, c.x - 1.0f * s, c.y - 1.0f * s,
                                  c.w + 2.0f * s, c.h + 2.0f * s,
                                  25.0f * s, fmaxf(1.0f, 2.0f * s),
                                  THEME.brand);
            int lh = fonts_line_h(a->fonts, fs_body, false);
            float tx = c.x + 17.0f * s;
            float ty = c.y + (c.h - (float)lh) * 0.5f;
            const char *shown = a->comment_draft;
            SDL_Color tcol = THEME.text;
            if (!shown[0]) {
                shown = enabled ? "Add a comment" : "Sign in to comment";
                tcol = THEME.cu_placeholder;
            }
            float icons_w = 3.0f * 32.0f * s + 2.0f * 8.0f * s + 16.0f * s;
            float avail = c.w - 17.0f * s - icons_w;
            if (a->comment_draft_len > 0)
                avail = a->d_post_btn.w > 0.0f
                            ? a->d_post_btn.x - 8.0f * s - tx
                            : c.w - 34.0f * s;
            if (avail < 30.0f * s)
                avail = 30.0f * s;
            rnd_push_clip(r, tx - 2.0f * s, c.y, avail, c.h);
            fonts_draw(r->r, a->fonts, shown, tx, ty, fs_body, false, tcol);
            if (focus && (SDL_GetTicks() / 500) % 2 == 0) {
                int pw = 0;
                fonts_measure(a->fonts, a->comment_draft, fs_body, false, &pw,
                              NULL);
                rnd_fill(r, tx + (float)pw, c.y + 12.0f * s,
                         fmaxf(1.5f, 2.0f * s), c.h - 24.0f * s, THEME.brand);
            }
            rnd_pop_clip(r);
            if (a->comment_draft_len > 0 && a->d_post_btn.w > 0.0f) {
                SDL_FRect pb = a->d_post_btn;
                pb.y += sy;
                bool post_enabled = enabled && !a->comment_posting;
                rnd_round(r, pb.x, pb.y, pb.w, pb.h, pb.h * 0.5f,
                          post_enabled ? THEME.brand : THEME.surface_alt);
                int bsz = (int)lroundf(13.0f * s);
                int blh = fonts_line_h(a->fonts, bsz, true);
                draw_text_center(a, a->comment_posting ? "\xE2\x80\xA6"
                                                       : "Post",
                                 bsz, true, pb.x + pb.w * 0.5f,
                                 pb.y + (pb.h - (float)blh) * 0.5f,
                                 post_enabled ? THEME.on_brand
                                              : THEME.text_faint);
            } else {
                /* decorative composer action icons: emoji / note / image */
                float bx = c.x + c.w - 16.0f * s - 32.0f * s;
                for (int i = 2; i >= 0; i--) {
                    SDL_FRect ib = {bx, c.y + (c.h - 32.0f * s) * 0.5f,
                                    32.0f * s, 32.0f * s};
                    if (i == 0)
                        cu_icon_smiley(a, ib.x + 16.0f * s,
                                       ib.y + 16.0f * s, 20.0f * s,
                                       THEME.text);
                    else if (i == 1)
                        cu_icon_note(a, ib.x + 16.0f * s, ib.y + 16.0f * s,
                                     20.0f * s, THEME.text);
                    else
                        cu_icon_image(a, ib.x + 16.0f * s, ib.y + 16.0f * s,
                                      20.0f * s, THEME.text);
                    if (i > 0)
                        bx -= 32.0f * s + 8.0f * s;
                }
            }
            if (a->comment_post_err[0])
                fonts_draw(r->r, a->fonts, a->comment_post_err, c.x,
                           c.y - (float)fonts_line_h(a->fonts, fs_meta,
                                                     false) -
                               6.0f * s,
                           fs_meta, false, THEME.error_text);
        }
    }

    rnd_pop_clip(r);

    /* page scrollbar (fixed) */
    a->d_left_sb_thumb.w = 0.0f;
    a->d_left_sb_thumb.h = 0.0f;
    if (a->left_content_h > a->left_view_h + 1.0f) {
        float vh = a->left_view_h;
        float maxs = a->left_content_h - vh;
        float th = vh * vh / a->left_content_h;
        if (th < 36.0f * s)
            th = 36.0f * s;
        float t = a->left_scroll / maxs;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        a->d_left_sb_thumb =
            (SDL_FRect){a->d_left_sb_track.x,
                        a->d_left_sb_track.y + t * (vh - th),
                        a->d_left_sb_track.w, th};
        rnd_round(r, a->d_left_sb_thumb.x, a->d_left_sb_thumb.y,
                  a->d_left_sb_thumb.w, a->d_left_sb_thumb.h,
                  a->d_left_sb_thumb.w * 0.5f,
                  col_a(a->left_drag_sb ? THEME.text_dim : THEME.text_faint,
                        170));
    }

    app_draw_cu_back(a);
    app_draw_detail_menu(a);
}


/* =================================================== Android mobile UI ==
 * A compact Pinterest-mobile layout drawn only on Android (PC_ANDROID). The
 * desktop renderer below is untouched. All coordinates are logical px at the
 * 432px baseline.
 */
#ifdef PC_ANDROID
static void app_draw_mobile(App *a);
static bool app_mobile_click(App *a, float x, float y);
static void app_mobile_measure(App *a);

static void mob_round_line(App *a, float x1, float y1, float x2, float y2,
                           float thick, SDL_Color c)
{
    rnd_line(&a->rnd, x1, y1, x2, y2, thick, c);
    float r = thick * 0.5f;
    rnd_round(&a->rnd, x1 - r, y1 - r, thick, thick, r, c);
    rnd_round(&a->rnd, x2 - r, y2 - r, thick, thick, r, c);
}

static void mob_icon_home(App *a, float cx, float cy, SDL_Color c)
{
    const float t = 2.0f;
    mob_round_line(a, cx - 10, cy - 1, cx, cy - 10, t, c);
    mob_round_line(a, cx, cy - 10, cx + 10, cy - 1, t, c);
    mob_round_line(a, cx - 7, cy - 2, cx - 7, cy + 9, t, c);
    mob_round_line(a, cx + 7, cy - 2, cx + 7, cy + 9, t, c);
    mob_round_line(a, cx - 7, cy + 9, cx + 7, cy + 9, t, c);
}

static void mob_icon_search(App *a, float cx, float cy, SDL_Color c)
{
    rnd_round_outline(&a->rnd, cx - 10, cy - 10, 15, 15, 7.5f, 2.0f, c);
    mob_round_line(a, cx + 4, cy + 4, cx + 10, cy + 10, 2.0f, c);
}

static void mob_icon_saved(App *a, float cx, float cy, SDL_Color c)
{
    const float t = 2.0f;
    mob_round_line(a, cx - 8, cy - 9, cx + 8, cy - 9, t, c);
    mob_round_line(a, cx - 8, cy - 9, cx - 8, cy + 9, t, c);
    mob_round_line(a, cx + 8, cy - 9, cx + 8, cy + 9, t, c);
    mob_round_line(a, cx - 8, cy + 9, cx, cy + 2, t, c);
    mob_round_line(a, cx, cy + 2, cx + 8, cy + 9, t, c);
}

static void mob_icon_profile(App *a, float cx, float cy, SDL_Color c)
{
    rnd_round_outline(&a->rnd, cx - 5, cy - 9, 10, 10, 5, 2.0f, c);
    mob_round_line(a, cx - 9, cy + 9, cx - 6, cy + 3, 2.0f, c);
    mob_round_line(a, cx - 6, cy + 3, cx + 6, cy + 3, 2.0f, c);
    mob_round_line(a, cx + 6, cy + 3, cx + 9, cy + 9, 2.0f, c);
}

static void mob_heart_points(float cx, float cy, float r, float *xs,
                             float *ys, int n)
{
    for (int i = 0; i <= n; i++) {
        float t = (float)i * 6.2831853f / (float)n;
        float st = sinf(t);
        float x = 16.0f * st * st * st;
        float y = 13.0f * cosf(t) - 5.0f * cosf(2 * t) -
                  2.0f * cosf(3 * t) - cosf(4 * t);
        float k = r / 16.0f;
        xs[i] = cx + x * k;
        ys[i] = cy - y * k + r * 0.10f;
    }
}

static void mob_heart(App *a, float cx, float cy, float r, SDL_Color c,
                      bool filled)
{
    float xs[65], ys[65];
    int n = 48;
    mob_heart_points(cx, cy, r, xs, ys, n);
    if (filled) {
        SDL_Vertex v[66];
        int idx[3 * 64];
        memset(v, 0, sizeof v);
        v[0].position.x = cx;
        v[0].position.y = cy + r * 0.05f;
        v[0].color = c;
        for (int i = 0; i <= n; i++) {
            v[i + 1].position.x = xs[i];
            v[i + 1].position.y = ys[i];
            v[i + 1].color = c;
        }
        for (int i = 0; i < n; i++) {
            idx[i * 3] = 0;
            idx[i * 3 + 1] = i + 1;
            idx[i * 3 + 2] = i + 2;
        }
        SDL_RenderGeometry(a->rnd.r, NULL, v, n + 1, idx, n * 3);
    } else {
        for (int i = 0; i < n; i++)
            mob_round_line(a, xs[i], ys[i], xs[i + 1], ys[i + 1], 2.0f, c);
    }
}

static void mob_comment_icon(App *a, float cx, float cy, float r, SDL_Color c)
{
    float w = r * 1.9f, h = r * 1.55f;
    rnd_round_outline(&a->rnd, cx - w * 0.5f, cy - h * 0.55f, w, h,
                      r * 0.55f, 2.0f, c);
    mob_round_line(a, cx - r * 0.35f, cy + h * 0.45f, cx - r * 0.05f,
                   cy + h * 0.85f, 2.0f, c);
    mob_round_line(a, cx - r * 0.05f, cy + h * 0.85f, cx + r * 0.45f,
                   cy + h * 0.38f, 2.0f, c);
}

static void mob_send_icon(App *a, float cx, float cy, float r, SDL_Color c)
{
    float t = 2.0f;
    mob_round_line(a, cx - r, cy + r * 0.35f, cx + r, cy - r * 0.85f, t, c);
    mob_round_line(a, cx + r, cy - r * 0.85f, cx + r * 0.20f,
                   cy + r * 0.85f, t, c);
    mob_round_line(a, cx + r * 0.20f, cy + r * 0.85f, cx, cy + r * 0.15f, t,
                   c);
    mob_round_line(a, cx - r * 0.55f, cy + r * 0.02f, cx + r * 0.55f,
                   cy - r * 0.05f, t * 0.8f, c);
}

static void mob_more_icon(App *a, float cx, float cy, float r, SDL_Color c)
{
    float dot = r * 0.85f;
    float step = dot * 2.6f;
    for (int i = -1; i <= 1; i++)
        rnd_round(&a->rnd, cx + (float)i * step - dot, cy - dot, dot * 2,
                  dot * 2, dot, c);
}

static void app_mobile_nav_geom(App *a)
{
    float W = (float)a->rnd.out_w, H = (float)a->rnd.out_h;
    const float item = 45.7f;
    const float gap = 6.1f;
    float total = 3.0f * item + 2.0f * gap;
    float x0 = (W - total) * 0.5f;
    float y = H - 12.0f - item;

    /* native 14.42: three floating items, no solid bar background */
    a->mob_navbar = (SDL_FRect){x0 - 6.0f, y - 4.0f, total + 12.0f,
                                item + 8.0f};
    for (int i = 0; i < 3; i++)
        a->mob_nav[i] = (SDL_FRect){x0 + (float)i * (item + gap), y, item,
                                    item};
    a->mob_nav[3] = (SDL_FRect){0, 0, 0, 0};

    /* top app bar: logo left; + / messages / avatar right */
    a->mob_logo = (SDL_FRect){6.0f, 5.5f, 26.0f, 26.0f};
    a->mob_top_avatar = (SDL_FRect){W - 6.0f - 26.0f, 5.5f, 26.0f, 26.0f};
    a->mob_top_msg = (SDL_FRect){a->mob_top_avatar.x - 4.0f - 36.6f, 5.5f,
                                 36.6f, 36.6f};
    a->mob_top_plus = (SDL_FRect){a->mob_top_msg.x - 36.6f, 5.5f, 36.6f,
                                  36.6f};

    /* search tab: full-width field in the top bar slot */
    a->mob_search_field = (SDL_FRect){6.0f, 0.2f, W - 12.0f, 36.6f};
}

static void app_mobile_nav_tap(App *a, int i)
{
    switch (i) {
    case 0: /* Home */
        a->mobile_search = false;
        a->search_focus = false;
        if (a->feed_mode == FEED_SAVED) {
            if (a->auth == PC_AUTH_LOGGED_IN)
                app_go_home(a);
            else {
                char qbuf[PC_TITLE_LEN];
                snprintf(qbuf, sizeof qbuf, "%s", DEFAULT_QUERY);
                app_search(a, qbuf);
            }
        }
        a->scroll_target = 0.0f;
        break;
    case 1: /* Search */
        a->mobile_search = true;
        a->search_focus = true;
        break;
    case 2: /* Saved */
        a->mobile_search = false;
        if (a->auth == PC_AUTH_LOGGED_IN)
            app_go_saved(a);
        else {
            snprintf(a->toast, sizeof a->toast,
                     "Sign in with cookies to see Saved");
            a->toast_until = SDL_GetTicks() + 2400;
        }
        break;
    }
    app_layout(a);
}

static void app_mobile_measure(App *a)
{
    float W = (float)a->rnd.out_w, H = (float)a->rnd.out_h;
    app_mobile_nav_geom(a);
    a->d_header = (SDL_FRect){0, 0, W, 0};
    a->d_rail_w = 0;
    a->d_rail = (SDL_FRect){0, 0, 0, H};
    a->left_view_h = H;
    a->detail_view_h = H;
    a->detail_content_h = H;

    a->mob_back = (SDL_FRect){6.0f, 6.0f, 36.6f, 36.6f};

    /* save bottom-sheet geometry (can open from feed or close-up) */
    float save_h = H * 0.88f;
    a->mob_save_n = (int)a->picker.boards.count;
    {
        int fit = (int)((save_h - 76.0f - 56.0f - 58.0f) / 56.0f);
        if (fit < 1)
            fit = 1;
        if (a->mob_save_n > fit)
            a->mob_save_n = fit;
    }
    float row_y = H - save_h + 76.0f;
    for (int i = 0; i < a->mob_save_n; i++)
        a->mob_save_rows[i] = (SDL_FRect){8, row_y + (float)i * 56.0f,
                                          W - 16.0f, 56.0f};
    a->mob_save_create = (SDL_FRect){8,
                                     row_y + (float)a->mob_save_n * 56.0f,
                                     W - 16.0f, 56.0f};
    a->mob_save_confirm = (SDL_FRect){8, H - 10.0f - 48.0f, W - 16.0f, 48};

    if (!a->show_detail) {
        a->left_content_h = H;
        return;
    }
    const PcPin *pin = &a->detail_pin;
    float aspect = 1.25f;
    if (a->detail.status == 2 && a->detail.tw > 0 && a->detail.th > 0)
        aspect = (float)a->detail.th / (float)a->detail.tw;
    else if (pin->img_w > 0 && pin->img_h > 0)
        aspect = (float)pin->img_h / (float)pin->img_w;
    float img_w = W - 4.0f;
    float img_h = img_w * aspect;
    a->mob_media_h = img_h;
    a->d_media = (SDL_FRect){2, 0, img_w, img_h};
    a->d_media_w = img_w;
    a->d_media_h = img_h;
    a->d_img_h = img_h;

    /* native action row: 48.8 dp tall */
    float y = img_h + 8.0f;
    const float act_h = 48.8f;
    a->mob_like = (SDL_FRect){3, y, 56.8f, act_h};
    a->mob_comment = (SDL_FRect){59.8f, y, 36.6f, act_h};
    a->mob_send = (SDL_FRect){96.4f, y, 36.6f, act_h};
    a->mob_more = (SDL_FRect){133.0f, y, 47.2f, act_h};
    a->mob_save = (SDL_FRect){W - 6.0f - 78.9f, y + (act_h - 36.6f) * 0.5f,
                              78.9f, 36.6f};
    y += act_h + 6.0f;

    /* pinner row (13.7 dp avatar) */
    a->d_pinner_y = y;
    y += 20.0f + 6.0f;

    a->d_title_n = fonts_wrap(a->fonts,
                              pin->title[0] ? pin->title : "Untitled pin",
                              a->M.h1_size, true, W - 12.0f, a->d_title, 2);
    y += (float)a->d_title_n * 24.0f + 4.0f;

    const char *desc = pin->description;
    {
        bool has = false;
        for (const char *p = desc; *p; p++)
            if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                has = true;
                break;
            }
        if (!has)
            desc = "This pin has no description.";
    }
    a->d_desc_n = fonts_wrap(a->fonts, desc, 16, false, W - 12.0f, a->d_desc,
                             6);
    int show = a->d_desc_n > 3 ? 3 : a->d_desc_n;
    if (a->desc_expanded)
        show = a->d_desc_n > 3 ? a->d_desc_n : show;
    if (a->desc_expanded && show > 6)
        show = 6;
    a->d_desc_shown = show;
    a->mob_seemore = (SDL_FRect){0, 0, 0, 0};
    y += (float)show * 22.0f;
    if (a->d_desc_n > 3) {
        a->mob_seemore = (SDL_FRect){6, y + 2, 80, 20};
        y += 22.0f;
    } else {
        y += 2.0f;
    }

    a->mob_visit.w = 0;
    if (pin->link[0] || pin->orig_url[0] || pin->domain[0]) {
        a->mob_visit = (SDL_FRect){6, y, W - 12.0f, 36.6f};
        y += 36.6f + 12.0f;
    }

    y += 16.0f + 10.0f; /* "More to explore" header */
    a->rel_n = (int)a->related.count;
    if (a->rel_n > 8)
        a->rel_n = 8;
    float tile_w = (W - 2.0f * 3.0f - 3.0f) / 2.0f;
    float tile_h = tile_w * 0.82f;
    for (int i = 0; i < a->rel_n; i++) {
        int c = i % 2, r = i / 2;
        a->d_rel_tiles[i] = (SDL_FRect){3.0f + (float)c * (tile_w + 3.0f),
                                        y + (float)r * (tile_h + 3.0f),
                                        tile_w, tile_h};
    }
    if (a->rel_n > 0)
        y += (float)((a->rel_n + 1) / 2) * (tile_h + 3.0f);
    y += 24.0f;
    a->left_content_h = y;

    /* comments bottom sheet geometry */
    float sheet_h = H * 0.87f;
    a->mob_sheet = (SDL_FRect){0, H - sheet_h, W, sheet_h};
    a->mob_sheet_grab = (SDL_FRect){W * 0.5f - 16.0f, H - sheet_h + 8.0f,
                                    32, 4};
    a->mob_sheet_close = (SDL_FRect){12, H - sheet_h + 20.0f, 36, 36};
    a->mob_sheet_composer = (SDL_FRect){8, H - 8.0f - 48.0f, W - 16.0f, 48};
    a->mob_sheet_send = (SDL_FRect){W - 8.0f - 40.0f,
                                    H - 8.0f - 48.0f, 40, 48};
    a->d_composer = a->mob_sheet_composer;
    a->d_comments_top = H - sheet_h + 80.0f;
}

static void mob_icon_plus(App *a, float cx, float cy, SDL_Color c)
{
    float t = 2.0f;
    rnd_line(&a->rnd, cx - 7, cy, cx + 7, cy, t, c);
    rnd_line(&a->rnd, cx, cy - 7, cx, cy + 7, t, c);
}

static void app_draw_mobile_topbar(App *a, bool search_mode)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w;
    rnd_fill(r, 0, 0, W, 37.0f, THEME.surface);

    if (search_mode) {
        SDL_FRect f = a->mob_search_field;
        rnd_round(r, f.x, f.y, f.w, f.h, 18.3f, THEME.surface_alt);
        mob_icon_search(a, f.x + 18, f.y + f.h * 0.5f, THEME.text_dim);
        const char *txt = a->input[0] ? a->input : "Search";
        int th = fonts_line_h(a->fonts, 16, false);
        fonts_draw(r->r, a->fonts, txt, f.x + 32,
                   f.y + (f.h - (float)th) * 0.5f, 16, false,
                   a->input[0] ? THEME.text : THEME.text_dim);
        return;
    }

    /* Pinterest logo mark + wordmark */
    SDL_FRect lg = a->mob_logo;
    rnd_round(r, lg.x, lg.y, lg.w, lg.h, 13.0f, THEME.brand);
    {
        int th = fonts_line_h(a->fonts, 17, true);
        int tw = 0;
        fonts_measure(a->fonts, "P", 17, true, &tw, NULL);
        fonts_draw(r->r, a->fonts, "P", lg.x + (lg.w - (float)tw) * 0.5f,
                   lg.y + (lg.h - (float)th) * 0.5f, 17, true,
                   THEME.on_brand);
    }
    {
        int th = fonts_line_h(a->fonts, 20, true);
        fonts_draw(r->r, a->fonts, "Pinterest", lg.x + lg.w + 5.0f,
                   (37.0f - (float)th) * 0.5f, 20, true, THEME.text);
    }

    /* + create */
    mob_icon_plus(a, a->mob_top_plus.x + 18.3f,
                  a->mob_top_plus.y + 18.3f, THEME.text);
    /* messages bubble + red badge */
    {
        float cx = a->mob_top_msg.x + 18.3f;
        float cy = a->mob_top_msg.y + 18.3f;
        rnd_round_outline(r, cx - 10, cy - 8, 20, 15, 7.0f, 1.8f,
                          THEME.text);
        rnd_line(r, cx - 4, cy + 6, cx - 1, cy + 10, 1.8f, THEME.text);
        rnd_line(r, cx - 1, cy + 10, cx + 3, cy + 6, 1.8f, THEME.text);
        rnd_round(r, cx + 5, cy - 12, 6, 6, 3.0f, THEME.brand);
    }
    /* avatar / profile */
    {
        SDL_FRect av = a->mob_top_avatar;
        rnd_round(r, av.x, av.y, av.w, av.h, av.w * 0.5f,
                  THEME.surface_alt);
        rnd_round_outline(r, av.x + 8, av.y + 6, 10, 10, 5.0f, 1.6f,
                          THEME.text_dim);
        rnd_line(r, av.x + 6, av.y + 21, av.x + 9, av.y + 16, 1.6f,
                 THEME.text_dim);
        rnd_line(r, av.x + 9, av.y + 16, av.x + 17, av.y + 16, 1.6f,
                 THEME.text_dim);
        rnd_line(r, av.x + 17, av.y + 16, av.x + 20, av.y + 21, 1.6f,
                 THEME.text_dim);
    }
}

static void app_draw_mobile_chips(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w;
    static const char *chips[5] = {"All", "minimalist", "decor", "art",
                                   "ideas"};
    rnd_fill(r, 0, 37.0f, W, 48.0f, THEME.surface);
    float x = 11.0f;
    int th = fonts_line_h(a->fonts, 16, true);
    for (int i = 0; i < 5; i++) {
        int tw = 0;
        fonts_measure(a->fonts, chips[i], 16, true, &tw, NULL);
        if (x > W - 8)
            break;
        fonts_draw(r->r, a->fonts, chips[i], x,
                   37.0f + (48.0f - (float)th) * 0.5f + 2.0f, 16, true,
                   i == 0 ? THEME.text : THEME.text_dim);
        if (i == 0) {
            rnd_fill(r, x, 77.0f, (float)tw, 3.0f, THEME.text);
            a->mob_chip_active = (SDL_FRect){x - 4, 37, (float)tw + 8, 48};
        }
        x += (float)tw + 18.0f;
    }
}

static void app_draw_mobile_search_header(App *a)
{
    float W = (float)a->rnd.out_w;
    app_draw_mobile_topbar(a, true);
    /* guided chips (native search suggestions row) */
    rnd_fill(&a->rnd, 0, 37.0f, W, 48.0f, THEME.surface);
    float x = 11.0f;
    int th = fonts_line_h(a->fonts, 16, true);
    for (int i = 0; i < 5; i++) {
        static const char *labs[5] = {"Modern", "Cozy", "Aesthetic", "Simple",
                                      "Ideas"};
        int tw = 0;
        fonts_measure(a->fonts, labs[i], 16, true, &tw, NULL);
        if (x > W - 8)
            break;
        fonts_draw(a->rnd.r, a->fonts, labs[i], x,
                   37.0f + (48.0f - (float)th) * 0.5f + 2.0f, 16, true,
                   THEME.text_dim);
        x += (float)tw + 18.0f;
    }
}

static void app_draw_mobile_nav(App *a)
{
    Rnd *r = &a->rnd;
    float H = (float)r->out_h;
    app_mobile_nav_geom(a);
    static const char *labels[3] = {"Home", "Search", "Saved"};
    bool active[3];
    active[0] = !a->mobile_search && a->feed_mode != FEED_SAVED &&
                !a->show_settings;
    active[1] = a->mobile_search;
    active[2] = a->feed_mode == FEED_SAVED;
    for (int i = 0; i < 3; i++) {
        SDL_FRect it = a->mob_nav[i];
        if (active[i]) {
            /* floating white active pill (native) */
            rnd_shadow(r, it.x, it.y + 1.0f, it.w, it.h, 20.0f, 6.0f, 55);
            rnd_round(r, it.x, it.y, it.w, it.h, 20.0f, THEME.on_brand);
        }
        SDL_Color c = active[i] ? THEME.pill_ink : THEME.text;
        float ix = it.x + it.w * 0.5f;
        float iy = it.y + 9.0f;
        switch (i) {
        case 0: mob_icon_home(a, ix, iy, c); break;
        case 1: mob_icon_search(a, ix, iy, c); break;
        default: mob_icon_saved(a, ix, iy, c); break;
        }
        int lsz = 10;
        int lw = 0;
        char lab[32];
        fonts_ellipsize(a->fonts, labels[i], lsz, true, it.w - 6.0f, lab,
                        sizeof lab);
        fonts_measure(a->fonts, lab, lsz, true, &lw, NULL);
        fonts_draw(r->r, a->fonts, lab, ix - (float)lw * 0.5f,
                   it.y + 28.0f, lsz, true, c);
    }
    (void)H;
}

/* draw a 16sp line, link-blue for #hashtags */
static void mob_draw_desc_line(App *a, const char *line, float x, float y)
{
    char tmp[FONT_LINE_MAX];
    snprintf(tmp, sizeof tmp, "%s", line);
    char *p = tmp;
    while (*p) {
        char *sp = p;
        while (*sp && *sp != ' ')
            sp++;
        char save = *sp;
        *sp = '\0';
        SDL_Color c = (p[0] == '#') ? THEME.link
                                    : THEME.text;
        fonts_draw(a->rnd.r, a->fonts, p, x, y, 16, false, c);
        int w = 0;
        fonts_measure(a->fonts, p, 16, false, &w, NULL);
        x += (float)w;
        *sp = save;
        if (!save)
            break;
        fonts_draw(a->rnd.r, a->fonts, " ", x, y, 16, false, THEME.text);
        fonts_measure(a->fonts, " ", 16, false, &w, NULL);
        x += (float)w;
        p = sp + 1;
    }
}

static void app_draw_mobile_detail(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w, H = (float)r->out_h;
    const PcPin *pin = &a->detail_pin;
    app_mobile_measure(a);
    rnd_fill(r, 0, 0, W, H, THEME.surface);
    float sy = -a->left_scroll;
    SDL_FRect img = a->d_media;
    img.y += sy;
    rnd_push_clip(r, 0, 0, W, H);
    if (a->detail.status == 2 && a->detail.tex) {
        rnd_texture(r, a->detail.tex, img.x, img.y, img.w, img.h);
    } else {
        rnd_round(r, img.x, img.y, img.w, img.h, 16, THEME.skeleton);
    }

    /* native 48.8 dp action row */
    SDL_FRect like = a->mob_like, cmt = a->mob_comment, snd = a->mob_send,
              more = a->mob_more, save = a->mob_save;
    like.y += sy;
    cmt.y += sy;
    snd.y += sy;
    more.y += sy;
    save.y += sy;
    mob_heart(a, like.x + 13, like.y + 22, 12, THEME.text, a->detail_liked);
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%d", a->detail_like_count);
    fonts_draw(r->r, a->fonts, cnt, like.x + 30, like.y + 16, 12, true,
               THEME.text);
    mob_comment_icon(a, cmt.x + 18.3f, cmt.y + 22, 11, THEME.text);
    mob_send_icon(a, snd.x + 18.3f, snd.y + 22, 11, THEME.text);
    mob_more_icon(a, more.x + 23.6f, more.y + 22, 1.7f, THEME.text);
    rnd_round(r, save.x, save.y, save.w, save.h, 18.3f, THEME.brand);
    {
        int th = fonts_line_h(a->fonts, 16, true);
        int tw = 0;
        fonts_measure(a->fonts, "Save", 16, true, &tw, NULL);
        fonts_draw(r->r, a->fonts, "Save", save.x + (save.w - (float)tw) / 2,
                   save.y + (save.h - (float)th) / 2, 16, true,
                   THEME.on_brand);
    }

    /* pinner row: 13.7 dp avatar + 12 sp name */
    float px = 6.0f, py = a->d_pinner_y + sy;
    rnd_round(r, px, py + 2, 14, 14, 7, THEME.brand_soft);
    fonts_draw(r->r, a->fonts,
               pin->pinner_name[0] ? pin->pinner_name : "PinFall", px + 20,
               py + 1, 12, true, THEME.text);

    /* title 18 sp bold, description 16 sp */
    float ty = py + 20.0f + 6.0f;
    for (int i = 0; i < a->d_title_n; i++)
        fonts_draw(r->r, a->fonts, a->d_title[i], 6,
                   ty + (float)i * 24.0f, 18, true, THEME.text);
    ty += (float)a->d_title_n * 24.0f + 4.0f;
    for (int i = 0; i < a->d_desc_shown; i++)
        mob_draw_desc_line(a, a->d_desc[i], 6, ty + (float)i * 22.0f);
    ty += (float)a->d_desc_shown * 22.0f;
    if (a->d_desc_n > 3) {
        SDL_FRect sm = a->mob_seemore;
        sm.y += sy;
        fonts_draw(r->r, a->fonts,
                   a->desc_expanded ? "See less" : "See more", sm.x, sm.y,
                   14, true, THEME.text);
        ty += 22;
    }
    ty += 2;

    /* Visit site: full width, 36.6 dp tall */
    if (a->mob_visit.w > 0) {
        SDL_FRect v = a->mob_visit;
        v.y += sy;
        rnd_round(r, v.x, v.y, v.w, v.h, 18.3f, THEME.surface_alt);
        int th = fonts_line_h(a->fonts, 16, true);
        int tw = 0;
        fonts_measure(a->fonts, "Visit site", 16, true, &tw, NULL);
        fonts_draw(r->r, a->fonts, "Visit site",
                   v.x + (v.w - (float)tw) / 2,
                   v.y + (v.h - (float)th) / 2, 16, true, THEME.text);
    }

    /* More to explore header + 2-col related grid */
    float ry = a->mob_visit.w > 0
                   ? a->mob_visit.y + a->mob_visit.h + 12.0f
                   : ty;
    ry += sy;
    fonts_draw(r->r, a->fonts, "More to explore", 6, ry, 16, true,
               THEME.text);
    if (a->related_unavailable) {
        fonts_draw(r->r, a->fonts, "Related pins unavailable for this pin",
                   6, ry + 22, 12, false, THEME.text_dim);
    }
    for (int i = 0; i < a->rel_n; i++) {
        SDL_FRect t = a->d_rel_tiles[i];
        t.y += sy;
        if (a->rel_tex[i])
            rnd_texture_cover(r, a->rel_tex[i], t.x, t.y, t.w, t.h);
        else
            rnd_round(r, t.x, t.y, t.w, t.h, 16, THEME.skeleton);
        mob_more_icon(a, t.x + t.w - 10, t.y + t.h - 10, 2.0f,
                      THEME.on_brand);
    }
    rnd_pop_clip(r);

    /* edge-to-edge media back button: 36.6 dp white circle */
    rnd_shadow(r, a->mob_back.x, a->mob_back.y + 2.0f, a->mob_back.w,
               a->mob_back.h, 18.3f, 8.0f, 70);
    rnd_round(r, a->mob_back.x, a->mob_back.y, a->mob_back.w,
              a->mob_back.h, 18.3f, THEME.surface);
    float cx = a->mob_back.x + 18.3f, cy = a->mob_back.y + 18.3f;
    rnd_line(&a->rnd, cx + 5, cy - 7, cx - 5, cy, 2.2f, THEME.text);
    rnd_line(&a->rnd, cx - 5, cy, cx + 5, cy + 7, 2.2f, THEME.text);

    if (a->left_scroll > 180.0f) {
        SDL_FRect bt = {W - 8 - 106, H - 20 - 36.6f, 106, 36.6f};
        a->mob_backtotop = bt;
        rnd_shadow(r, bt.x, bt.y + 1, bt.w, bt.h, 18.3f, 6, 55);
        rnd_round(r, bt.x, bt.y, bt.w, bt.h, 18.3f, THEME.surface);
        fonts_draw(r->r, a->fonts, "\xE2\x86\x91 Back to top", bt.x + 12,
                   bt.y + 10, 12, true, THEME.text);
    } else {
        a->mob_backtotop.w = 0;
    }
}



static void app_draw_mobile_save_sheet(App *a)
{
    Picker *p = &a->picker;
    Rnd *r = &a->rnd;
    float W = (float)r->out_w, H = (float)r->out_h;
    float sheet_h = H * 0.88f;
    rnd_fill(r, 0, 0, W, H, THEME.scrim);
    rnd_round(r, 0, H - sheet_h, W, sheet_h, 32, THEME.surface);
    rnd_round(r, W * 0.5f - 16.0f, H - sheet_h + 8.0f, 32, 4, 2,
              THEME.surface_alt);
    rnd_round(r, 12, H - sheet_h + 20.0f, 36, 36, 18, THEME.surface_alt);
    float cx = 30, cy = H - sheet_h + 38.0f;
    rnd_line(r, cx - 6, cy - 6, cx + 6, cy + 6, 1.8f, THEME.text);
    rnd_line(r, cx + 6, cy - 6, cx - 6, cy + 6, 1.8f, THEME.text);
    fonts_draw(r->r, a->fonts, "Save to board", 60, H - sheet_h + 30.0f, 16,
               true, THEME.text);

    app_mobile_measure(a);
    for (int i = 0; i < a->mob_save_n; i++) {
        SDL_FRect row = a->mob_save_rows[i];
        bool sel = (p->selected_row == i);
        if (sel)
            rnd_round(r, row.x, row.y, row.w, row.h, 12, THEME.surface_alt);
        rnd_round(r, row.x + 8, row.y + 8, 40, 40, 8, THEME.surface_alt);
        char bname[FONT_LINE_MAX];
        fonts_ellipsize(a->fonts, p->boards.items[i].name, 16, true,
                        row.w - 76.0f, bname, sizeof bname);
        fonts_draw(r->r, a->fonts, bname, row.x + 60, row.y + 10, 16, true,
                   THEME.text);
        char meta[64];
        snprintf(meta, sizeof meta, "%d Pins", p->boards.items[i].pin_count);
        fonts_draw(r->r, a->fonts, meta, row.x + 60, row.y + 32, 12, false,
                   THEME.text_dim);
    }
    SDL_FRect cr = a->mob_save_create;
    rnd_round(r, cr.x + 8, cr.y + 8, 40, 40, 8, THEME.surface_alt);
    fonts_draw(r->r, a->fonts, "+", cr.x + 20, cr.y + 12, 24, true,
               THEME.text_dim);
    fonts_draw(r->r, a->fonts,
               p->new_name[0] ? p->new_name : "Create board", cr.x + 60,
               cr.y + 18, 16, true, THEME.text);
    /* confirm */
    bool ready = p->selected_row >= 0 && (size_t)p->selected_row <
                                             p->boards.count;
    SDL_FRect cf = a->mob_save_confirm;
    rnd_round(r, cf.x, cf.y, cf.w, cf.h, 18.3f,
              ready ? THEME.brand : THEME.surface_alt);
    int th = fonts_line_h(a->fonts, 16, true);
    int tw = 0;
    fonts_measure(a->fonts, "Save", 16, true, &tw, NULL);
    fonts_draw(r->r, a->fonts, "Save", cf.x + (cf.w - (float)tw) / 2,
               cf.y + (cf.h - (float)th) / 2, 16, true,
               ready ? THEME.on_brand : THEME.text_dim);
    if (p->create_error[0])
        fonts_draw(r->r, a->fonts, p->create_error, 16, cr.y + cr.h + 4, 12,
                   false, THEME.error_text);
}

static void app_draw_mobile_comments_sheet(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w, H = (float)r->out_h;
    float sheet_h = H * 0.87f;
    rnd_fill(r, 0, 0, W, H, THEME.scrim);
    rnd_round(r, 0, H - sheet_h, W, sheet_h, 32, THEME.surface);
    rnd_round(r, a->mob_sheet_grab.x, a->mob_sheet_grab.y,
              a->mob_sheet_grab.w, a->mob_sheet_grab.h, 2,
              THEME.surface_alt);
    rnd_round(r, a->mob_sheet_close.x, a->mob_sheet_close.y,
              a->mob_sheet_close.w, a->mob_sheet_close.h, 18,
              THEME.surface_alt);
    float cx = a->mob_sheet_close.x + a->mob_sheet_close.w * 0.5f;
    float cy = a->mob_sheet_close.y + a->mob_sheet_close.h * 0.5f;
    rnd_line(r, cx - 6, cy - 6, cx + 6, cy + 6, 1.8f, THEME.text);
    rnd_line(r, cx + 6, cy - 6, cx - 6, cy + 6, 1.8f, THEME.text);
    fonts_draw(r->r, a->fonts, "What do you think?",
               a->mob_sheet_close.x + a->mob_sheet_close.w + 12.0f,
               a->mob_sheet_close.y + 9.0f, 16, true, THEME.text);
    float top = a->mob_sheet_close.y + a->mob_sheet_close.h + 8.0f;
    float bottom = a->mob_sheet_composer.y - 8.0f;
    rnd_push_clip(r, 0, top, W, bottom - top);
    float y = top - a->mob_sheet_scroll;
    if (a->comments_loading) {
        fonts_draw(r->r, a->fonts, "Loading comments\xE2\x80\xA6", 16, y, 16,
                   false, THEME.text_dim);
    } else if (a->comments_unavailable) {
        fonts_draw(r->r, a->fonts, "Comments unavailable for this pin", 16,
                   y, 16, false, THEME.text_dim);
    } else if (a->comments.count == 0) {
        char lines[4][FONT_LINE_MAX];
        int nl = fonts_wrap(a->fonts,
                            "Write a comment, share feedback, or give a "
                            "high-five to get the conversation going",
                            16, false, W - 32, lines, 4);
        for (int k = 0; k < nl; k++)
            fonts_draw(r->r, a->fonts, lines[k], 16,
                       y + (float)k * 22.0f, 16, false, THEME.text);
    } else {
        for (size_t i = 0; i < a->comments.count; i++) {
            PcComment *c = &a->comments.items[i];
            rnd_round(r, 16, y, 32, 32, 16, THEME.brand_soft);
            char ini[2] = {0};
            if (c->user_name[0])
                ini[0] = c->user_name[0];
            fonts_draw(r->r, a->fonts, ini, 16 + 11, y + 6, 14, true,
                       THEME.error_text);
            fonts_draw(r->r, a->fonts, c->user_name, 52, y, 12, true,
                       THEME.text);
            char lines[3][FONT_LINE_MAX];
            int nl = fonts_wrap(a->fonts, c->text, 14, false, W - 68, lines,
                                3);
            for (int k = 0; k < nl; k++)
                fonts_draw(r->r, a->fonts, lines[k], 52,
                           y + 18 + (float)k * 19.6f, 14, false, THEME.text);
            char meta[64], mshown[FONT_LINE_MAX];
            snprintf(meta, sizeof meta, "%s \xC2\xB7 \xE2\x99\xA5 %d",
                     c->created_at[0] ? c->created_at : "now",
                     c->like_count);
            fonts_ellipsize(a->fonts, meta, 11, false, W - 64.0f, mshown,
                            sizeof mshown);
            fonts_draw(r->r, a->fonts, mshown, 52,
                       y + 18 + (float)nl * 19.6f + 2, 11, false,
                       THEME.text_dim);
            y += 18 + (float)nl * 19.6f + 22 + 8;
        }
    }
    rnd_pop_clip(r);
    /* composer pill */
    SDL_FRect cbox = a->mob_sheet_composer;
    rnd_round(r, cbox.x, cbox.y, cbox.w, cbox.h, 24, THEME.surface_alt);
    int th2 = fonts_line_h(a->fonts, 16, false);
    const char *ph = a->comment_draft[0] ? a->comment_draft : "Add a comment";
    fonts_draw(r->r, a->fonts, ph, cbox.x + 18,
               cbox.y + (cbox.h - (float)th2) / 2, 16, false,
               a->comment_draft[0] ? THEME.text : THEME.text_dim);
    mob_send_icon(a, a->mob_sheet_send.x + 22, cbox.y + 24, 11, THEME.text);
}

static void app_draw_mobile_feed(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w, H = (float)r->out_h;
    rnd_fill(r, 0, 0, W, H, THEME.bg);
    app_draw_mobile_topbar(a, a->mobile_search);
    if (a->mobile_search)
        app_draw_mobile_search_header(a);
    else
        app_draw_mobile_chips(a);
    float top = content_top(a), bot = content_bottom(a);
    rnd_push_clip(r, 0, top, W, bot - top);
    if (a->state == VIEW_LOADING && a->npins == 0) {
        fonts_draw(r->r, a->fonts, "Loading\xE2\x80\xA6", 11, top + 28, 16,
                   true, THEME.text_dim);
    } else if (a->state == VIEW_ERROR) {
        fonts_draw(r->r, a->fonts, a->error, 11, top + 28, 14, false,
                   THEME.error_text);
    } else if (a->state == VIEW_EMPTY) {
        fonts_draw(r->r, a->fonts, "Nothing here yet", 11, top + 28, 14,
                   false, THEME.text_dim);
    }
    for (size_t i = 0; i < a->layout.count && i < a->npins; i++) {
        const LayoutItem *it = &a->layout.items[i];
        PinView *pv = &a->pins[i];
        float y = it->y - a->scroll;
        if (y + it->card_h < top || y > bot)
            continue;
        SDL_FRect box = {it->x, y, it->w, it->img_h};
        if (pv->status == 2 && pv->tex) {
            rnd_texture_cover(r, pv->tex, box.x, box.y, box.w, box.h);
        } else {
            rnd_round(r, box.x, box.y, box.w, box.h, 16, THEME.skeleton);
        }
        if (pv->pin.video_duration_ms > 0) {
            int secs = pv->pin.video_duration_ms / 1000;
            char dur[16];
            snprintf(dur, sizeof dur, "%d:%02d", secs / 60, secs % 60);
            SDL_FRect db = {box.x + 8, box.y + 8, 40, 24};
            rnd_round(r, db.x, db.y, db.w, db.h, 8, THEME.surface);
            int th = fonts_line_h(a->fonts, 12, true);
            int tw = 0;
            fonts_measure(a->fonts, dur, 12, true, &tw, NULL);
            fonts_draw(r->r, a->fonts, dur, db.x + (db.w - (float)tw) / 2,
                       db.y + (db.h - (float)th) / 2, 12, true, THEME.text);
        }
        /* native "…" affordance: bottom-right of the image, 24 dp box */
        mob_more_icon(a, box.x + box.w - 10.0f, box.y + box.h - 12.0f,
                      1.7f, THEME.text);

        /* native 12 sp caption under the pin (2-line clamp, secondary) */
        if (pv->nlines > 0) {
            float cy = box.y + box.h + 4.0f;
            for (int li = 0; li < pv->nlines && li < 2; li++)
                fonts_draw(r->r, a->fonts, pv->line[li], box.x,
                           cy + (float)li * 16.0f, 12, false, THEME.text_dim);
        }
    }
    rnd_pop_clip(r);
}

static void app_draw_mobile(App *a)
{
    Rnd *r = &a->rnd;
    float W = (float)r->out_w, H = (float)r->out_h;
    app_mobile_nav_geom(a);
    if (a->picker.open) {
        app_draw_mobile_save_sheet(a);
    } else if (a->show_settings) {
        app_draw_settings(a);
    } else if (a->show_detail) {
        app_draw_mobile_detail(a);
        if (a->comments_sheet)
            app_draw_mobile_comments_sheet(a);
    } else {
        app_draw_mobile_feed(a);
    }
    if (!a->show_settings && !a->comments_sheet && !a->picker.open &&
        !a->show_detail)
        app_draw_mobile_nav(a);
    (void)W;
    (void)H;
}

static bool app_mobile_click(App *a, float x, float y)
{
    float W = (float)a->rnd.out_w, H = (float)a->rnd.out_h;
    app_mobile_nav_geom(a);
    app_mobile_measure(a);
    if (a->picker.open) {
        float save_h = H * 0.78f;
        if (y < H - save_h) {
            app_close_picker(a);
            return true;
        }
        if (x < 72 && y < H - save_h + 80) {
            app_close_picker(a);
            return true;
        }
        for (int i = 0; i < a->mob_save_n; i++)
            if (point_in_f(a->mob_save_rows[i], x, y)) {
                a->picker.selected_row = i;
                return true;
            }
        if (point_in_f(a->mob_save_create, x, y)) {
            if (a->picker.create_focus && a->picker.new_name_len > 0)
                app_picker_create(a);
            else
                a->picker.create_focus = true;
            return true;
        }
        if (point_in_f(a->mob_save_confirm, x, y)) {
            if (a->picker.selected_row >= 0)
                app_picker_choose(a, a->picker.selected_row);
            return true;
        }
        return true;
    }
    if (a->show_settings) {
        return false; /* desktop settings panel handles its own clicks */
    }
    if (a->comments_sheet) {
        float sheet_h = H * 0.87f;
        if (y < H - sheet_h || (x < 72 && y < H - sheet_h + 90)) {
            a->comments_sheet = false;
            a->composer_focus = false;
            return true;
        }
        if (point_in_f(a->mob_sheet_composer, x, y)) {
            a->composer_focus = true;
            return true;
        }
        if (point_in_f(a->mob_sheet_send, x, y)) {
            app_detail_post_comment(a);
            return true;
        }
        return true;
    }
    /* top app bar actions (native: logo, +, messages, avatar/settings) */
    if (!a->show_detail && !a->mobile_search) {
        if (point_in_f(a->mob_logo, x, y)) {
            a->mobile_search = false;
            a->search_focus = false;
            app_layout(a);
            return true;
        }
        if (point_in_f(a->mob_top_plus, x, y)) {
            snprintf(a->toast, sizeof a->toast,
                     "Creating pins is not available in this build");
            a->toast_until = SDL_GetTicks() + 2000;
            return true;
        }
        if (point_in_f(a->mob_top_msg, x, y)) {
            snprintf(a->toast, sizeof a->toast,
                     "Messages need a Pinterest inbox API");
            a->toast_until = SDL_GetTicks() + 2000;
            return true;
        }
        if (point_in_f(a->mob_top_avatar, x, y)) {
            a->search_focus = false;
            app_open_settings(a);
            return true;
        }
    }
    /* floating bottom navigation (3 tabs, native) */
    if (!a->show_detail) {
        for (int i = 0; i < 3; i++) {
            SDL_FRect hit = {a->mob_nav[i].x - 3.0f,
                             a->mob_nav[i].y - 4.0f,
                             a->mob_nav[i].w + 6.0f,
                             a->mob_nav[i].h + 8.0f};
            if (point_in_f(hit, x, y)) {
                app_mobile_nav_tap(a, i);
                return true;
            }
        }
    }
    if (a->show_detail) {
        float scroll = a->left_scroll;
        if (point_in_f(a->mob_back, x, y)) {
            app_close_detail(a);
            return true;
        }
        if (point_in_f((SDL_FRect){a->mob_like.x, a->mob_like.y + scroll,
                                   a->mob_like.w, a->mob_like.h}, x, y)) {
            app_detail_like_toggle(a);
            return true;
        }
        if (point_in_f((SDL_FRect){a->mob_comment.x, a->mob_comment.y + scroll,
                                   a->mob_comment.w, a->mob_comment.h}, x, y)) {
            a->comments_sheet = true;
            a->composer_focus = false;
            return true;
        }
        if (point_in_f((SDL_FRect){a->mob_send.x, a->mob_send.y + scroll,
                                   a->mob_send.w, a->mob_send.h}, x, y) ||
            point_in_f((SDL_FRect){a->mob_more.x, a->mob_more.y + scroll,
                                   a->mob_more.w, a->mob_more.h}, x, y)) {
            const char *link = a->detail_pin.link[0] ? a->detail_pin.link
                                                     : a->detail_pin.orig_url;
            if (link[0]) {
                SDL_SetClipboardText(link);
                snprintf(a->toast, sizeof a->toast, "Link copied");
                a->toast_until = SDL_GetTicks() + 1600;
            }
            return true;
        }
        if (point_in_f((SDL_FRect){a->mob_save.x, a->mob_save.y + scroll,
                                   a->mob_save.w, a->mob_save.h}, x, y)) {
            if (writes_enabled(a))
                app_open_picker(a, a->detail_idx);
            else {
                snprintf(a->toast, sizeof a->toast, "Sign in to save");
                a->toast_until = SDL_GetTicks() + 1800;
            }
            return true;
        }
        if (a->d_desc_n > 3 && a->mob_seemore.w > 0 &&
            point_in_f((SDL_FRect){a->mob_seemore.x,
                                   a->mob_seemore.y + a->left_scroll,
                                   a->mob_seemore.w, a->mob_seemore.h},
                       x, y)) {
            a->desc_expanded = !a->desc_expanded;
            app_mobile_measure(a);
            return true;
        }
        if (a->mob_visit.w > 0 &&
            point_in_f(a->mob_visit, x, y + a->left_scroll)) {
            const char *link = a->detail_pin.link[0] ? a->detail_pin.link
                                                     : a->detail_pin.orig_url;
            if (link[0]) {
                SDL_SetClipboardText(link);
                snprintf(a->toast, sizeof a->toast, "Link copied");
                a->toast_until = SDL_GetTicks() + 1600;
            }
            return true;
        }
        if (a->mob_backtotop.w > 0 &&
            point_in_f(a->mob_backtotop, x, y)) {
            a->left_scroll_target = 0;
            return true;
        }
        /* related tiles */
        for (int i = 0; i < a->rel_n; i++) {
            SDL_FRect t = a->d_rel_tiles[i];
            t.y += -a->left_scroll;
            if (point_in_f(t, x, y)) {
                app_open_related_pin(a, &a->related.items[i]);
                return true;
            }
        }
        return true;
    }
    /* feed */
    if (a->mobile_search) {
        if (point_in_f(a->mob_search_field, x, y)) {
            a->search_focus = true;
            return true;
        }
        if (y >= 85.0f)
            a->search_focus = false;
    }
    if (y >= H - 12.0f)
        return true;
    int idx = layout_hit(&a->layout, x, y, a->scroll);
    if (idx >= 0 && (size_t)idx < a->npins) {
        const LayoutItem *it = &a->layout.items[idx];
        float iy = it->y - a->scroll;
        /* "…" affordance area (bottom-right of the image) */
        if (x > it->x + it->w - 30 && y > iy + it->img_h - 34) {
            if (writes_enabled(a))
                app_open_picker(a, idx);
            else {
                snprintf(a->toast, sizeof a->toast, "Sign in to save");
                a->toast_until = SDL_GetTicks() + 1800;
            }
            return true;
        }
        a->search_focus = false;
        app_open_detail(a, idx);
        return true;
    }
    if (a->search_focus)
        a->search_focus = false;
    return true;
}
#endif /* PC_ANDROID */

static void app_draw(App *a)
{
    rnd_clear(&a->rnd, THEME.bg);

#ifdef PC_ANDROID
    app_draw_mobile(a);
    app_draw_stats(a);
    app_draw_toast(a);
    return;
#endif

    if (a->show_detail) {
        /* full-page close-up route: app header/feed are hidden */
        app_draw_detail(a);
        if (a->picker.open)
            app_draw_picker(a);
        app_draw_stats(a);
        app_draw_toast(a);
        return;
    }

    app_draw_header(a);
    if (a->state == VIEW_ERROR && a->npins == 0) {
        app_draw_error(a);
    } else if (a->state == VIEW_EMPTY) {
        app_draw_empty(a);
    } else if (a->npins == 0) {
        app_draw_loading_grid(a);
    } else {
        app_draw_feed(a);
    }
    app_draw_footer(a);
    app_draw_banner(a);
    if (a->picker.open)
        app_draw_picker(a);
    if (a->show_settings)
        app_draw_settings(a);
    app_draw_stats(a);
    app_draw_toast(a);
}

/* ------------------------------------------------------------- events */
static void app_input_insert(App *a, const char *text)
{
    if (!a->search_focus || !text || !*text)
        return;
    size_t tlen = strlen(text);
    if (a->input_len + tlen >= sizeof a->input)
        return;
    size_t off = fonts_utf8_offset(a->input, a->caret);
    memmove(a->input + off + tlen, a->input + off,
            (size_t)a->input_len - off + 1);
    memcpy(a->input + off, text, tlen);
    a->input_len += (int)tlen;
    a->caret += fonts_utf8_len(text);
    a->caret_last = SDL_GetTicks();
    a->caret_on = true;
}

static void app_input_backspace(App *a)
{
    if (!a->search_focus || a->caret <= 0)
        return;
    size_t start = fonts_utf8_offset(a->input, a->caret - 1);
    size_t end = fonts_utf8_offset(a->input, a->caret);
    memmove(a->input + start, a->input + end,
            (size_t)a->input_len - end + 1);
    a->input_len -= (int)(end - start);
    a->caret--;
    a->caret_last = SDL_GetTicks();
    a->caret_on = true;
}

static void app_submit_search(App *a)
{
    char q[PC_TITLE_LEN];
    snprintf(q, sizeof q, "%s", a->input);
    size_t n = strlen(q);
    while (n > 0 && (q[n - 1] == ' ' || q[n - 1] == '\t'))
        q[--n] = '\0';
    size_t start = 0;
    while (q[start] == ' ' || q[start] == '\t')
        start++;
    if (!q[start])
        return;
    app_search(a, q + start);
    a->search_focus = false;
}

static void app_scrollbar_drag(App *a, int my)
{
    float top = content_top(a);
    float ch = content_h(a);
    float track_y = top + 8.0f * a->rnd.scale;
    float track_h = ch - 16.0f * a->rnd.scale;
    float total = a->layout.total_h;
    float thumb_h = track_h * ch / total;
    if (thumb_h < 40.0f * a->rnd.scale)
        thumb_h = 40.0f * a->rnd.scale;
    float ms = max_scroll(a);
    float span = track_h - thumb_h;
    if (span <= 0.0f)
        return;
    float t = ((float)my - track_y - thumb_h * 0.5f) / span;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    a->scroll_target = t * ms;
    a->scroll = a->scroll_target;
}

static void app_click(App *a, float x, float y)
{
#ifdef PC_ANDROID
    if (app_mobile_click(a, x, y))
        return;
#endif
    /* settings modal owns input while open */
    if (a->show_settings) {
        app_settings_geom(a);
        if (point_in_f(a->settings_close, x, y) ||
            !point_in_f(a->settings_panel, x, y)) {
            app_close_settings(a);
            return;
        }
#ifdef PC_ANDROID
        if (a->settings_cookie_btn.w > 0.0f &&
            point_in_f(a->settings_cookie_btn, x, y)) {
            app_reload_cookies(a);
            return;
        }
#endif
        if (point_in_f(a->settings_theme_rect, x, y)) {
            a->settings.theme_mode = (a->settings.theme_mode + 1) % 3;
            settings_save(&a->settings);
            app_apply_theme(a);
            snprintf(a->toast, sizeof a->toast, "Theme: %s",
                     settings_theme_name(a->settings.theme_mode));
            a->toast_until = SDL_GetTicks() + 1600;
            return;
        }
        if (point_in_f(a->settings_about_badge, x, y) ||
            (a->settings_about_url.w > 0 &&
             point_in_f(a->settings_about_url, x, y))) {
            SDL_SetClipboardText(
                "https://github.com/deepseek-ai/deepseek-harness");
            snprintf(a->toast, sizeof a->toast,
                     "Repo link copied to clipboard");
            a->toast_until = SDL_GetTicks() + 1800;
            a->about_selected = true;
            return;
        }
        if (y > a->settings_panel.y + 575.0f * a->rnd.scale &&
            point_in_f(a->settings_panel, x, y)) {
            a->about_selected = true;
        }
        if (point_in_f(a->settings_autoplay_toggle, x, y)) {
            a->settings_row = 5;
            app_settings_set(a, 5, 0);
            return;
        }
        if (point_in_f(a->settings_ads_toggle, x, y)) {
            a->settings_row = 3;
            app_settings_set(a, 3, 0);
            return;
        }
        if (point_in_f(a->settings_toggle, x, y)) {
            a->settings_row = 4;
            app_settings_set(a, 4, 0);
            return;
        }
        for (int row = 0; row < 3; row++) {
            for (int i = 0; i < a->settings_opt_n[row]; i++) {
                if (point_in_f(a->settings_opt[row][i], x, y)) {
                    a->settings_row = row;
                    app_settings_set(a, row, i);
                    return;
                }
            }
            if (point_in_f(a->settings_rows[row], x, y)) {
                a->settings_row = row;
                return;
            }
            if (row == 5 && point_in_f(a->settings_rows[row], x, y)) {
                a->settings_row = row;
                return;
            }
        }
        return;
    }

    /* board-picker modal owns input while open */
    if (a->picker.open) {
        Picker *p = &a->picker;
        if (point_in_f(p->close, x, y))
            app_close_picker(a);
        else if (!point_in_f(p->panel, x, y))
            app_close_picker(a);
        else if (writes_enabled(a) && !p->creating &&
                 point_in_f(p->create_box, x, y)) {
            p->create_focus = true;
        } else if (writes_enabled(a) && !p->creating &&
                   point_in_f(p->privacy_btn, x, y)) {
            p->new_secret = !p->new_secret;
        } else if (writes_enabled(a) && !p->creating &&
                   point_in_f(p->create_btn, x, y)) {
            app_picker_create(a);
        } else if (!p->loading && !p->saving) {
            p->create_focus = false;
            for (size_t i = 0; i < p->boards.count; i++) {
                SDL_FRect row;
                if (picker_row_rect(a, (int)i, &row) && point_in_f(row, x, y)) {
                    app_picker_choose(a, (int)i);
                    break;
                }
            }
        }
        return;
    }

    if (a->show_detail) {
        /* context menus own the click while open */
        if (a->detail_menu) {
            for (int i = 0; i < a->d_menu_n; i++) {
                if (!point_in_f(a->d_menu_items[i], x, y))
                    continue;
                const char *link = a->detail_pin.link[0]
                                       ? a->detail_pin.link
                                       : (a->detail_pin.orig_url[0]
                                              ? a->detail_pin.orig_url
                                              : a->detail_pin.domain);
                if (a->detail_menu == 1) {
                    if (i == 0 && link[0]) {
                        SDL_SetClipboardText(link);
                        snprintf(a->toast, sizeof a->toast,
                                 "Link copied to clipboard");
                        a->toast_until = SDL_GetTicks() + 1600;
                    } else if (i == 1 && link[0]) {
                        SDL_SetClipboardText(link);
                        snprintf(a->toast, sizeof a->toast,
                                 "Link copied \xE2\x80\x94 open it in your "
                                 "browser");
                        a->toast_until = SDL_GetTicks() + 2200;
                    }
                } else {
                    if (i == 0 && link[0]) {
                        SDL_SetClipboardText(link);
                        snprintf(a->toast, sizeof a->toast,
                                 "Link copied \xE2\x80\x94 open it in your "
                                 "browser");
                        a->toast_until = SDL_GetTicks() + 2200;
                    } else if (i == 1 && a->detail_pin.id[0]) {
                        SDL_SetClipboardText(a->detail_pin.id);
                        snprintf(a->toast, sizeof a->toast, "Pin id copied");
                        a->toast_until = SDL_GetTicks() + 1600;
                    }
                }
                a->detail_menu = 0;
                return;
            }
            if (!point_in_f(a->d_menu, x, y))
                a->detail_menu = 0;
            return;
        }
        /* ---- fixed chrome: icon rail ---- */
        for (int i = 0; i < 8; i++) {
            if (a->d_nav_btns[i].w <= 0.0f ||
                !point_in_f(a->d_nav_btns[i], x, y))
                continue;
            a->detail_menu = 0;
            switch (i) {
            case 0:
            case 1:
                app_close_detail(a);
                app_go_home(a);
                break;
            case 2:
                app_close_detail(a);
                app_go_saved(a);
                break;
            case 7:
                app_open_settings(a);
                break;
            default:
                snprintf(a->toast, sizeof a->toast,
                         "Not available in PinFall");
                a->toast_until = SDL_GetTicks() + 1800;
                break;
            }
            return;
        }
        /* ---- fixed chrome: search header ---- */
        if (a->search_clear.w > 0 &&
            point_in_f(fr_from(a->search_clear), x, y)) {
            a->input[0] = '\0';
            a->input_len = 0;
            a->caret = 0;
            a->search_focus = true;
            a->composer_focus = false;
            return;
        }
        if (point_in_f(a->d_search_rect, x, y)) {
            a->search_focus = true;
            a->composer_focus = false;
            a->caret = fonts_utf8_len(a->input);
            a->caret_last = SDL_GetTicks();
            a->caret_on = true;
            return;
        }
        if (point_in_f(cu_search_lens(a), x, y)) {
            snprintf(a->toast, sizeof a->toast,
                     "Visual search is not available in PinFall");
            a->toast_until = SDL_GetTicks() + 2000;
            return;
        }
        if (point_in_f(cu_search_mic(a), x, y)) {
            snprintf(a->toast, sizeof a->toast,
                     "Voice search is not available in PinFall");
            a->toast_until = SDL_GetTicks() + 2000;
            return;
        }
        if (point_in_f(a->d_header_avatar, x, y)) {
            if (a->auth == PC_AUTH_LOGGED_IN)
                snprintf(a->toast, sizeof a->toast, "Signed in as %.90s",
                         a->username[0] ? a->username : "Pinterest");
            else
                snprintf(a->toast, sizeof a->toast, "Guest session");
            a->toast_until = SDL_GetTicks() + 2000;
            return;
        }
        if (point_in_f(a->d_header_chev, x, y)) {
            app_open_settings(a);
            return;
        }

        /* back arrow returns to the feed */
        if (point_in_f(a->d_back_btn, x, y)) {
            app_close_detail(a);
            return;
        }
        /* page scrollbar */
        if (a->d_left_sb_thumb.h > 0 &&
            point_in_f(a->d_left_sb_thumb, x, y)) {
            a->left_drag_sb = true;
            a->left_drag_y = y;
            a->left_drag_scroll = a->left_scroll;
            return;
        }
        /* comments scrollbar */
        if (a->d_sb_thumb.h > 0 && point_in_f(a->d_sb_thumb, x, y)) {
            a->detail_drag_sb = true;
            a->detail_drag_y = y;
            a->detail_drag_scroll = a->detail_scroll;
            return;
        }
        /* image overlay buttons */
        if (a->video_active && !a->video_failed &&
            a->d_video_btn.w > 0.0f &&
            point_in_f(cu_screen(a, a->d_video_btn), x, y)) {
            app_video_toggle_play(a);
            return;
        }
        if (a->d_overlay_btn[0].w > 0.0f &&
            point_in_f(cu_screen(a, a->d_overlay_btn[0]), x, y)) {
            snprintf(a->toast, sizeof a->toast,
                     "Visual search is not available in PinFall");
            a->toast_until = SDL_GetTicks() + 2000;
            return;
        }
        if (a->d_overlay_btn[1].w > 0.0f &&
            point_in_f(cu_screen(a, a->d_overlay_btn[1]), x, y)) {
            snprintf(a->toast, sizeof a->toast,
                     "Full-screen view is not available in PinFall");
            a->toast_until = SDL_GetTicks() + 2000;
            return;
        }
        /* action bar */
        if (point_in_f(cu_screen(a, a->d_like_btn), x, y)) {
            app_detail_like_toggle(a);
            return;
        }
        if (point_in_f(cu_screen(a, a->d_comment_btn), x, y)) {
            a->detail_focus_side = 0;
            a->detail_scroll_target = 0.0f;
            return;
        }
        if (point_in_f(cu_screen(a, a->d_share_btn), x, y)) {
            a->detail_menu = (a->detail_menu == 1) ? 0 : 1;
            return;
        }
        if (point_in_f(cu_screen(a, a->d_more_btn), x, y)) {
            a->detail_menu = (a->detail_menu == 2) ? 0 : 2;
            return;
        }
        if (point_in_f(cu_screen(a, a->d_dropdown), x, y) ||
            (a->d_save.w > 0 &&
             point_in_f(cu_screen(a, a->d_save_btn), x, y))) {
            if (!writes_enabled(a)) {
                snprintf(a->toast, sizeof a->toast, "Sign in to save");
                a->toast_until = SDL_GetTicks() + 1800;
            } else {
                app_open_picker(a, a->detail_idx);
            }
            return;
        }
        if (a->d_desc_more.w > 0 &&
            point_in_f(cu_screen(a, a->d_desc_more), x, y)) {
            a->desc_expanded = !a->desc_expanded;
            app_detail_measure(a);
            return;
        }
        if (a->d_viewmore_btn.w > 0) {
            SDL_FRect vm = a->d_viewmore_btn;
            vm.y += a->d_header.h - a->left_scroll - a->detail_scroll;
            if (point_in_f(vm, x, y)) {
                if (!a->auth_preview && a->client &&
                    app_detail_canonical_id(a->detail_pin.id) &&
                    a->comments.bookmark[0]) {
                    NetCmd cmd;
                    memset(&cmd, 0, sizeof cmd);
                    cmd.kind = NET_COMMENTS;
                    cmd.seq = ++a->comments_seq;
                    cmd.append = true;
                    snprintf(cmd.pin_id, sizeof cmd.pin_id, "%s",
                             a->detail_pin.id);
                    snprintf(cmd.bookmark, sizeof cmd.bookmark, "%s",
                             a->comments.bookmark);
                    a->comments_loading = true;
                    app_push_net(a, &cmd);
                }
                return;
            }
        }
        /* the Post pill sits inside the composer, so test it first */
        if (a->d_post_btn.w > 0.0f &&
            point_in_f(cu_screen(a, a->d_post_btn), x, y)) {
            app_detail_post_comment(a);
            return;
        }
        if (point_in_f(cu_screen(a, a->d_composer), x, y)) {
            bool en = (a->auth_preview || writes_enabled(a)) &&
                      (a->auth_preview ||
                       app_detail_canonical_id(a->detail_pin.id));
            if (en) {
                a->composer_focus = true;
                a->search_focus = false;
            } else {
                snprintf(a->toast, sizeof a->toast, "Sign in to comment");
                a->toast_until = SDL_GetTicks() + 1800;
            }
            return;
        }
        /* recommendation tiles open the related pin */
        for (int i = 0; i < a->d_rec_n && i < MAX_REL; i++) {
            if (point_in_f(cu_screen(a, a->d_rel_tiles[i]), x, y)) {
                a->detail_menu = 0;
                app_open_related_pin(a, &a->related.items[i]);
                return;
            }
        }
        /* "Ideas you might love" search-idea rows */
        for (int k = 0; k < 2 && a->d_ideas.w > 0.0f; k++) {
            if (!point_in_f(cu_screen(a, a->d_idea_rows[k]), x, y))
                continue;
            char q[PC_TITLE_LEN];
            snprintf(q, sizeof q, "%s", a->d_idea_labels[k]);
            a->search_focus = false;
            app_close_detail(a);
            if (q[0])
                app_search(a, q);
            return;
        }
        a->composer_focus = false;
        a->search_focus = false;
        return;
    }

    if (a->auth == PC_AUTH_LOGGED_IN) {
        if (a->home_btn.w > 0 && point_in(a->home_btn, x, y)) {
            app_go_home(a);
            return;
        }
        if (a->saved_btn.w > 0 && point_in(a->saved_btn, x, y)) {
            app_go_saved(a);
            return;
        }
    }

    SDL_FRect sr = search_rect(a);
    if (point_in_f(sr, x, y)) {
        if (a->search_clear.w > 0 && point_in(a->search_clear, x, y)) {
            a->input[0] = '\0';
            a->input_len = 0;
            a->caret = 0;
            a->search_focus = true;
            return;
        }
        a->search_focus = true;
        /* place caret closest to click */
        int best = 0;
        int bestd = 0x7fffffff;
        int nc = fonts_utf8_len(a->input);
        for (int cp = 0; cp <= nc; cp++) {
            char prefix[PC_TITLE_LEN];
            size_t off = fonts_utf8_offset(a->input, cp);
            memcpy(prefix, a->input, off);
            prefix[off] = '\0';
            int w = 0;
            fonts_measure(a->fonts, prefix, a->M.search_size, false, &w,
                          NULL);
            int d = (int)fabsf((float)w - (x - (sr.x + 40.0f * a->rnd.scale)));
            if (d < bestd) {
                bestd = d;
                best = cp;
            }
        }
        a->caret = best;
        a->caret_last = SDL_GetTicks();
        a->caret_on = true;
        return;
    }

    if (a->state == VIEW_ERROR) {
        app_retry(a);
        return;
    }
    if (a->banner_rect.w > 0 && a->error[0] &&
        point_in(a->banner_rect, x, y)) {
        app_retry(a);
        return;
    }

    /* scrollbar */
    float total = a->layout.total_h;
    if (total > content_h(a) + 1.0f) {
        float s = a->rnd.scale;
        float track_w = 8.0f * s;
        float track_x = (float)a->rnd.out_w - 2.0f * s - track_w;
        if (x >= track_x - 6.0f * s && x <= track_x + track_w + 6.0f * s &&
            y >= content_top(a) && y <= content_bottom(a)) {
            a->dragging_scrollbar = true;
            a->mouse_down = true;
            app_scrollbar_drag(a, (int)y);
            return;
        }
    }

    if (a->card_save_rect.w > 0 && a->hover >= 0 &&
        point_in(a->card_save_rect, x, y)) {
        if (!writes_enabled(a)) {
            snprintf(a->toast, sizeof a->toast, "Sign in to save");
            a->toast_until = SDL_GetTicks() + 1800;
        } else {
            app_open_picker(a, a->hover);
        }
        return;
    }

    int idx = layout_hit(&a->layout, x, y, a->scroll);
    if (idx >= 0 && (size_t)idx < a->npins && (size_t)idx < a->layout.count &&
        y >= content_top(a) && y <= content_bottom(a)) {
        const LayoutItem *it = &a->layout.items[idx];
        if (y + a->scroll <= it->y + it->img_h + a->M.card_radius) {
            app_open_detail(a, idx);
            return;
        }
    }
    if (a->search_focus)
        a->search_focus = false;
}

static void app_handle_event(App *a, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT:
        a->running = false;
        break;
#ifdef PC_ANDROID
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP: {
        float ui_scale = a->rnd.ui_scale > 0.0f ? a->rnd.ui_scale : 1.0f;
        float lx = 0.0f, ly = 0.0f, dy = 0.0f;
        bool moved = false;
        PfAndroidTouchAction act = pf_android_touch_handle(
            &a->touch, e, (float)a->rnd.win_w,
            (float)(a->rnd.win_h - a->rnd.pad_bottom_px), ui_scale,
            &lx, &ly, &dy, &moved);
        if (e->type != SDL_FINGERUP || act == PF_TOUCH_TAP) {
            a->mx = (int)lx;
            a->my = (int)ly;
        }
        if (e->type == SDL_FINGERDOWN) {
            a->suppress_click = a->fling_active;
            a->fling_active = false;
            a->fling_v = 0.0f;
            a->drag_v = 0.0f;
            a->drag_last_ms = SDL_GetTicks();
        }
        if (act == PF_TOUCH_DRAG && dy != 0.0f) {
            Uint32 now = SDL_GetTicks();
            float dt = (float)(now - a->drag_last_ms) / 1000.0f;
            a->mouse_down = false;
            a->scroll_target = a->scroll;
            a->left_scroll_target = a->left_scroll;
            if (a->picker.open) {
                a->picker.scroll -= dy;
                picker_geom(a);
                a->fling_axis = 2;
            } else if (a->show_settings) {
                /* modal: no scrolling behind it */
                a->fling_axis = 2;
            } else if (a->show_detail) {
                /* native close-up: one page scroll (no column split) */
                float lmax = a->left_content_h - a->left_view_h;
                if (lmax < 0.0f)
                    lmax = 0.0f;
                a->left_scroll -= dy;
                if (a->left_scroll < 0.0f)
                    a->left_scroll = 0.0f;
                if (a->left_scroll > lmax)
                    a->left_scroll = lmax;
                a->left_scroll_target = a->left_scroll;
                a->fling_axis = 1;
                if (a->detail_menu)
                    a->detail_menu = 0;
            } else {
                float ms = max_scroll(a);
                a->scroll -= dy;
                if (a->scroll < 0.0f)
                    a->scroll = 0.0f;
                if (a->scroll > ms)
                    a->scroll = ms;
                a->scroll_target = a->scroll;
                a->fling_axis = 0;
            }
            a->drag_moved = true;
            if (dt > 0.0001f && dt < 0.25f) {
                float inst = dy / dt;
                a->drag_v = a->drag_v * 0.65f + inst * 0.35f;
                a->drag_last_ms = now;
            } else {
                a->drag_last_ms = now;
            }
        }
        if (e->type == SDL_FINGERUP) {
            a->mouse_down = false;
            a->dragging_scrollbar = false;
            a->detail_drag_sb = false;
            a->left_drag_sb = false;
            if (a->drag_moved && fabsf(a->drag_v) > 220.0f &&
                !a->picker.open && !a->show_settings) {
                a->fling_v = -a->drag_v;
                a->fling_active = true;
                SDL_Log("[fling] start v=%.0f axis=%d", a->fling_v,
                        a->fling_axis);
            }
            bool do_click = (act == PF_TOUCH_TAP) && !a->suppress_click;
            a->suppress_click = false;
            a->drag_moved = false;
            a->drag_v = 0.0f;
            if (do_click)
                app_click(a, lx, ly);
            pf_android_touch_reset(&a->touch);
        }
        break;
    }
    case SDL_APP_WILLENTERBACKGROUND:
        a->android_backgrounded = true;
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        a->android_backgrounded = false;
        if (a->settings.theme_mode == 0)
            app_apply_theme(a);
        break;
    case SDL_RENDER_DEVICE_RESET: {
        /* GL context lost (backgrounded): all textures are invalid */
        rnd_update_size(&a->rnd);
        app_metrics(a);
        app_layout(a);
        for (size_t i = 0; i < a->npins; i++)
            tex_clear(a, &a->pins[i]);
        a->tex_count = 0;
        a->tex_bytes = 0;
        if (a->detail.tex) {
            SDL_DestroyTexture(a->detail.tex);
            a->detail.tex = NULL;
            a->detail.status = 0;
        }
        if (a->video_tex) {
            SDL_DestroyTexture(a->video_tex);
            a->video_tex = NULL;
            a->video_frames = 0;
        }
        if (!a->badge_tex)
            app_load_badge(a);
        fonts_free(&a->fonts);
        if (!fonts_init(&a->fonts, a->rnd.r, "", "")) {
            a->state = VIEW_ERROR;
            snprintf(a->error, sizeof a->error,
                     "Could not reload fonts after context reset");
        }
        app_request_images(a);
        if (a->show_detail && a->detail.status == 0)
            app_queue_detail_image(a);
        break;
    }
#endif
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_RESIZED) {
#ifdef PC_ANDROID
            if (a->settings.theme_mode == 0)
                app_apply_theme(a);
#endif
            rnd_update_size(&a->rnd);
            app_metrics(a);
            app_layout(a);
            if (a->show_detail) {
                app_detail_wrap(a);
                if (a->detail.status == 0)
                    app_queue_detail_image(a);
            }
        }
        break;
    case SDL_MOUSEWHEEL:
        if (dbg_events())
            fprintf(stderr, "[ev] wheel y=%d dir=%d\n", e->wheel.y,
                    e->wheel.direction);
        if (a->show_settings) {
            /* modal: no feed scrolling behind it */
        } else if (a->picker.open) {
            float amount = (float)e->wheel.y * 48.0f * a->rnd.scale;
            if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                amount = -amount;
            a->picker.scroll -= amount;
            picker_geom(a);
        } else if (a->show_detail) {
            float s = a->rnd.scale;
            float amount = (float)e->wheel.y * 120.0f * s;
            if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                amount = -amount;
            SDL_FRect comments = {a->d_info.x, a->d_comments_top + a->d_header.h -
                                                 a->left_scroll,
                                  a->d_info.w, a->detail_view_h};
            bool over_comments =
                point_in_f(comments, (float)a->mx, (float)a->my) &&
                a->detail_content_h > a->detail_view_h + 1.0f;
            if (over_comments) {
                a->detail_focus_side = 0;
                a->detail_scroll_target -= amount;
            } else {
                a->detail_focus_side = 1;
                a->left_scroll_target -= amount;
            }
            if (a->detail_menu)
                a->detail_menu = 0;
        } else {
            float amount = (float)e->wheel.y * 130.0f * a->rnd.scale;
            if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                amount = -amount;
            a->scroll_target -= amount;
            if (dbg_events())
                fprintf(stderr, "[ev] scroll target=%.1f\n",
                        a->scroll_target);
        }
        break;
    case SDL_MOUSEMOTION:
#ifdef PC_ANDROID
        a->mx = (int)lroundf((float)e->motion.x /
                             (a->rnd.ui_scale > 0 ? a->rnd.ui_scale : 1.0f));
        a->my = (int)lroundf(((float)e->motion.y - (float)a->rnd.pad_top_px) /
                             (a->rnd.ui_scale > 0 ? a->rnd.ui_scale : 1.0f));
#else
        a->mx = (int)((float)e->motion.x * a->rnd.scale);
        a->my = (int)(((float)e->motion.y - (float)a->rnd.pad_top_px) *
                      a->rnd.scale);
#endif
        if (a->dragging_scrollbar)
            app_scrollbar_drag(a, a->my);
        if (a->left_drag_sb) {
            float track_h = a->d_left_sb_track.h - a->d_left_sb_thumb.h;
            float maxs = a->left_content_h - a->left_view_h;
            if (track_h > 1.0f && maxs > 0.0f) {
                float t = ((float)a->my - a->d_left_sb_track.y -
                           a->d_left_sb_thumb.h * 0.5f) /
                          track_h;
                if (t < 0.0f)
                    t = 0.0f;
                if (t > 1.0f)
                    t = 1.0f;
                a->left_scroll_target = t * maxs;
                a->left_scroll = a->left_scroll_target;
            }
        }
        if (a->detail_drag_sb) {
            float track_h = a->d_sb_track.h - a->d_sb_thumb.h;
            float maxs = a->detail_content_h - a->detail_view_h;
            float track_y = a->d_sb_track.y + a->d_header.h - a->left_scroll;
            if (track_h > 1.0f && maxs > 0.0f) {
                float t = ((float)a->my - track_y -
                           a->d_sb_thumb.h * 0.5f) /
                          track_h;
                if (t < 0.0f)
                    t = 0.0f;
                if (t > 1.0f)
                    t = 1.0f;
                a->detail_scroll_target = t * maxs;
                a->detail_scroll = a->detail_scroll_target;
            }
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
#ifdef PC_ANDROID
            a->mx = (int)lroundf((float)e->button.x /
                                 (a->rnd.ui_scale > 0 ? a->rnd.ui_scale
                                                      : 1.0f));
            a->my = (int)lroundf(((float)e->button.y -
                                  (float)a->rnd.pad_top_px) /
                                 (a->rnd.ui_scale > 0 ? a->rnd.ui_scale
                                                      : 1.0f));
#else
            a->mx = (int)((float)e->button.x * a->rnd.scale);
            a->my = (int)(((float)e->button.y - (float)a->rnd.pad_top_px) *
                          a->rnd.scale);
#endif
            a->mouse_down = true;
            if (dbg_events())
                fprintf(stderr, "[ev] click %d,%d\n", a->mx, a->my);
            app_click(a, (float)a->mx, (float)a->my);
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) {
            a->mouse_down = false;
            a->dragging_scrollbar = false;
            a->detail_drag_sb = false;
            a->left_drag_sb = false;
        }
        break;
    case SDL_TEXTINPUT:
        if (a->picker.open) {
            if (a->picker.create_focus && !a->picker.creating) {
                size_t add = strlen(e->text.text);
                if (a->picker.new_name_len + (int)add <
                    (int)sizeof a->picker.new_name - 1 &&
                    a->picker.new_name_len + (int)add <= 60) {
                    memcpy(a->picker.new_name + a->picker.new_name_len,
                           e->text.text, add + 1);
                    a->picker.new_name_len += (int)add;
                }
            }
        } else if (a->show_detail) {
            if (a->composer_focus) {
                size_t add = strlen(e->text.text);
                if (a->comment_draft_len + (int)add <
                    (int)sizeof a->comment_draft - 1) {
                    memcpy(a->comment_draft + a->comment_draft_len,
                           e->text.text, add + 1);
                    a->comment_draft_len += (int)add;
                }
            } else if (a->search_focus) {
                app_input_insert(a, e->text.text);
            }
        } else if (!a->show_settings) {
            app_input_insert(a, e->text.text);
        }
        break;
    case SDL_KEYDOWN: {
        SDL_Keymod mod = SDL_GetModState();
        SDL_Keycode sym = e->key.keysym.sym;
#ifdef PC_ANDROID
        if (sym == SDLK_AC_BACK) {
            if (a->comments_sheet) {
                a->comments_sheet = false;
                a->composer_focus = false;
            } else if (a->detail_menu) {
                a->detail_menu = 0;
            } else if (a->show_settings) {
                app_close_settings(a);
            } else if (a->picker.open) {
                app_close_picker(a);
            } else if (a->show_detail) {
                app_close_detail(a);
            } else if (a->search_focus) {
                a->search_focus = false;
            } else {
                a->running = false;
            }
            break;
        }
#endif
        if (a->show_settings) {
            switch (sym) {
            case SDLK_ESCAPE:
            case SDLK_COMMA:
                app_close_settings(a);
                break;
            case SDLK_UP:
                a->settings_row = (a->settings_row + 5) % 6;
                break;
            case SDLK_DOWN:
                a->settings_row = (a->settings_row + 1) % 6;
                break;
            case SDLK_LEFT:
                app_settings_adjust(a, -1);
                break;
            case SDLK_RIGHT:
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                app_settings_adjust(a, 1);
                break;
            case SDLK_F3:
                a->settings.show_stats = !a->settings.show_stats;
                app_settings_changed(a);
                break;
            default:
                break;
            }
            break;
        }
        if (sym == SDLK_F3) {
            a->settings.show_stats = !a->settings.show_stats;
            app_settings_changed(a);
            break;
        }
        if (sym == SDLK_COMMA && !a->search_focus) {
            app_open_settings(a);
            break;
        }
        if (a->picker.open) {
            if (sym == SDLK_ESCAPE) {
                app_close_picker(a);
                break;
            }
            if (a->picker.create_focus) {
                if (sym == SDLK_BACKSPACE &&
                    a->picker.new_name_len > 0) {
                    a->picker.new_name_len--;
                    a->picker.new_name[a->picker.new_name_len] = '\0';
                } else if (sym == SDLK_RETURN ||
                           sym == SDLK_KP_ENTER) {
                    app_picker_create(a);
                } else if (sym == SDLK_TAB) {
                    a->picker.create_focus = false;
                }
            }
            break;
        }
        if (a->show_detail && !a->picker.open) {
            if (a->search_focus) {
                switch (sym) {
                case SDLK_ESCAPE:
                    a->search_focus = false;
                    break;
                case SDLK_BACKSPACE:
                    app_input_backspace(a);
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    a->search_focus = false;
                    app_close_detail(a);
                    app_submit_search(a);
                    break;
                default:
                    break;
                }
                break;
            }
            switch (sym) {
            case SDLK_ESCAPE:
                if (a->detail_menu)
                    a->detail_menu = 0;
                else
                    app_close_detail(a);
                break;
            case SDLK_BACKSPACE:
                if (a->composer_focus && a->comment_draft_len > 0) {
                    a->comment_draft_len--;
                    a->comment_draft[a->comment_draft_len] = '\0';
                }
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (a->composer_focus)
                    app_detail_post_comment(a);
                break;
            case SDLK_PAGEUP:
                a->detail_focus_side = 1;
                a->left_scroll_target -= a->left_view_h * 0.85f;
                break;
            case SDLK_PAGEDOWN:
                a->detail_focus_side = 1;
                a->left_scroll_target += a->left_view_h * 0.85f;
                break;
            case SDLK_HOME:
                a->detail_focus_side = 1;
                a->left_scroll_target = 0.0f;
                break;
            case SDLK_END:
                a->detail_focus_side = 1;
                a->left_scroll_target = a->left_content_h;
                break;
            default:
                break;
            }
            break;
        }
        if (dbg_events())
            fprintf(stderr, "[ev] key %d\n", (int)sym);
        if ((mod & KMOD_CTRL) && (sym == SDLK_f || sym == SDLK_k)) {
            a->search_focus = true;
            a->caret = fonts_utf8_len(a->input);
            a->caret_last = SDL_GetTicks();
            a->caret_on = true;
            break;
        }
        if ((mod & KMOD_CTRL) && sym == SDLK_v) {
            if (a->search_focus) {
                char *clip = SDL_GetClipboardText();
                if (clip) {
                    app_input_insert(a, clip);
                    SDL_free(clip);
                }
            }
            break;
        }
        switch (sym) {
        case SDLK_ESCAPE:
            if (a->picker.open)
                app_close_picker(a);
            else if (a->show_detail)
                app_close_detail(a);
            else if (a->search_focus)
                a->search_focus = false;
            else if (a->scroll_target > 1.0f) {
                a->scroll_target = 0.0f;
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (a->show_detail)
                app_close_detail(a);
            else if (a->search_focus || a->state == VIEW_ERROR)
                app_submit_search(a);
            break;
        case SDLK_BACKSPACE:
            app_input_backspace(a);
            break;
        case SDLK_DELETE: {
            if (a->search_focus && a->caret < fonts_utf8_len(a->input)) {
                size_t s0 = fonts_utf8_offset(a->input, a->caret);
                size_t s1 = fonts_utf8_offset(a->input, a->caret + 1);
                memmove(a->input + s0, a->input + s1,
                        (size_t)a->input_len - s1 + 1);
                a->input_len -= (int)(s1 - s0);
            }
            break;
        }
        case SDLK_LEFT:
            if (a->search_focus && a->caret > 0) {
                a->caret--;
                a->caret_last = SDL_GetTicks();
                a->caret_on = true;
            }
            break;
        case SDLK_RIGHT:
            if (a->search_focus &&
                a->caret < fonts_utf8_len(a->input)) {
                a->caret++;
                a->caret_last = SDL_GetTicks();
                a->caret_on = true;
            }
            break;
        case SDLK_HOME:
            if (a->search_focus) {
                a->caret = 0;
            } else {
                a->scroll_target = 0.0f;
            }
            break;
        case SDLK_END:
            if (a->search_focus) {
                a->caret = fonts_utf8_len(a->input);
            } else {
                a->scroll_target = max_scroll(a);
            }
            break;
        case SDLK_PAGEUP:
            if (!a->search_focus)
                a->scroll_target -= content_h(a) * 0.88f;
            break;
        case SDLK_PAGEDOWN:
            if (!a->search_focus)
                a->scroll_target += content_h(a) * 0.88f;
            break;
        case SDLK_SPACE:
            if (a->search_focus && e->key.repeat == 0) {
                /* TEXTINPUT handles the space itself; nothing to do */
            }
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

/* --------------------------------------------------------------- snapshot */
static int app_count_ready_images(App *a)
{
    int n = 0;
    for (size_t i = 0; i < a->npins; i++) {
        if (a->pins[i].status == 2 && a->pins[i].tex)
            n++;
    }
    return n;
}

static int app_band_pending(App *a)
{
    int pending = 0;
    float top, bot;
    app_texture_band(a, &top, &bot);
    size_t n = a->npins < a->layout.count ? a->npins : a->layout.count;
    for (size_t i = 0; i < n; i++) {
        const LayoutItem *it = &a->layout.items[i];
        if (it->y + it->card_h < top || it->y > bot)
            continue;
        if (a->pins[i].status == 0 || a->pins[i].status == 1)
            pending++;
    }
    return pending;
}

static int app_visible_pending(App *a)
{
    int pending = 0;
    float top = a->scroll;
    float bot = a->scroll + content_h(a);
    size_t n = a->npins < a->layout.count ? a->npins : a->layout.count;
    for (size_t i = 0; i < n; i++) {
        const LayoutItem *it = &a->layout.items[i];
        if (it->y + it->card_h < top || it->y > bot)
            continue;
        if (a->pins[i].status == 0 || a->pins[i].status == 1)
            pending++;
    }
    return pending;
}

static void app_stop_threads(App *a)
{
    if (a->vid_thread_started) {
        app_video_stop(a);
        pthread_mutex_lock(&a->vid_mtx);
        a->vid_quit = true;
        pthread_cond_broadcast(&a->vid_cv);
        pthread_mutex_unlock(&a->vid_mtx);
        pthread_join(a->vid_thr, NULL);
        a->vid_thread_started = false;
    }
    pthread_mutex_lock(&a->img_mtx);
    a->img_quit = true;
    pthread_cond_broadcast(&a->img_cv);
    pthread_mutex_unlock(&a->img_mtx);
    for (int i = 0; i < a->n_img_thr; i++)
        pthread_join(a->img_thr[i], NULL);

    pthread_mutex_lock(&a->net_mtx);
    a->net_quit = true;
    pthread_cond_signal(&a->net_cv);
    pthread_mutex_unlock(&a->net_mtx);
    pthread_join(a->net_thr, NULL);
}

static void app_free_queues(App *a)
{
    pthread_mutex_lock(&a->net_mtx);
    if (a->net_res_ready)
        net_res_free(&a->net_res);
    memset(&a->net_res, 0, sizeof a->net_res);
    a->net_res_ready = false;
    pthread_mutex_unlock(&a->net_mtx);

    pthread_mutex_lock(&a->img_mtx);
    for (int i = 0; i < a->nresults; i++) {
        if (a->results[i].pixels)
            pc_free(a->results[i].pixels);
    }
    a->nresults = 0;
    a->req_count = 0;
    a->req_head = 0;
    pthread_mutex_unlock(&a->img_mtx);
}

/* Decode the embedded official dsh badge once (do not regenerate it). */
static void app_load_badge(App *a)
{
    int w = 0, h = 0, n = 0;
    unsigned char *pix = stbi_load_from_memory(
        dsh_badge_png, (int)dsh_badge_png_len, &w, &h, &n, 4);
    if (!pix)
        return;
    SDL_Texture *tex = rnd_texture_rgba(&a->rnd, pix, w, h, 0.0f);
    stbi_image_free(pix);
    if (!tex)
        return;
    a->badge_tex = tex;
    a->badge_w = w;
    a->badge_h = h;
}

static void app_apply_theme(App *a)
{
    int mode = a->settings.theme_mode;
    if (mode < 0 || mode > 2)
        mode = 0;
    if (mode == 0) {
#ifdef PC_ANDROID
        theme_set_system_dark(pf_android_night_mode() != 0);
#else
        const char *gtk = getenv("GTK_THEME");
        const char *cs = getenv("COLORSCHEME");
        bool dark = (gtk && strstr(gtk, "dark")) ||
                    (cs && strstr(cs, "dark"));
        theme_set_system_dark(dark);
#endif
    } else {
        theme_set_system_dark(false);
    }
    theme_set_mode((ThemeMode)mode);
}

static bool app_init(App *a, SDL_Window *win, bool snapshot_mode,
                     const char *snap_path, int snap_timeout,
                     const char *initial_query, const char *cookie_file,
                     bool save_session, bool auth_preview,
                     bool query_explicit)
{
    memset(a, 0, sizeof *a);
    a->win = win;
    a->hover = -1;
    a->detail_idx = -1;
    a->snapshot = snapshot_mode;
    a->snap_timeout = snap_timeout;
    a->cur_kind = -2;
    a->auth = PC_AUTH_UNKNOWN;
    a->auth_preview = auth_preview;
    a->query_explicit = query_explicit;
    a->save_session = save_session;
    default_session_path(a->session_path, sizeof a->session_path);
    settings_defaults(&a->settings);
    settings_load(&a->settings);
    {
        const char *env_theme = getenv("PINFALL_THEME");
        int forced;
        if (env_theme && settings_parse_theme(env_theme, &forced))
            a->settings.theme_mode = forced;
    }
    app_apply_theme(a);
    settings_path(a->settings_path, sizeof a->settings_path);
    a->settings_row = 0;
    a->show_stats = a->settings.show_stats;
    a->fps_window_start = SDL_GetTicks();
    if (snap_path)
        snprintf(a->snap_path, sizeof a->snap_path, "%s", snap_path);
    a->snap_hover = -1;
    snprintf(a->query, sizeof a->query, "%s",
             initial_query && *initial_query ? initial_query : DEFAULT_QUERY);
    snprintf(a->input, sizeof a->input, "%s", a->query);
    a->input_len = (int)strlen(a->input);
    a->caret = fonts_utf8_len(a->input);
    snprintf(a->host, sizeof a->host, "%s", host_label());

    pthread_mutex_init(&a->net_mtx, NULL);
    pthread_cond_init(&a->net_cv, NULL);
    pthread_mutex_init(&a->img_mtx, NULL);
    pthread_cond_init(&a->img_cv, NULL);
    pthread_mutex_init(&a->vid_mtx, NULL);
    pthread_cond_init(&a->vid_cv, NULL);

    if (!rnd_init(&a->rnd, win)) {
        pf_android_log("app: rnd_init FAILED: %s", SDL_GetError());
        fprintf(stderr, "pinfall: failed to create SDL renderer: %s\n",
                SDL_GetError());
        return false;
    }
    pf_android_log("app: renderer ok");
    if (!fonts_init(&a->fonts, a->rnd.r, FONT_REG, FONT_BOLD)) {
        pf_android_log("app: fonts_init FAILED");
        fprintf(stderr, "pinfall: failed to load DejaVu fonts from "
                        "/usr/share/fonts/truetype/dejavu\n");
        return false;
    }
    pf_android_log("app: fonts ok");
    layout_init(&a->layout);
    app_metrics(a);
    app_load_badge(a);

    a->cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    a->cur_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    a->cur_ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);

    a->client = pc_client_new(NULL);
    if (!a->client) {
        a->state = VIEW_ERROR;
        snprintf(a->error, sizeof a->error,
                 "Could not initialise the network client");
    }
    app_apply_settings(a);

    /* Cookie import: explicit --cookie-file, else the saved default session.
     * --auth-preview is rendering-only, so it never touches cookies. */
    if (a->client && !auth_preview) {
        const char *load_path = cookie_file;
#ifdef PC_ANDROID
        snprintf(a->device_cookie_path, sizeof a->device_cookie_path, "%s",
                 pf_android_cookie_load_path());
        if (!load_path && pf_android_cookie_exists())
            load_path = a->device_cookie_path;
#endif
        if (!load_path && a->session_path[0] &&
            file_exists(a->session_path))
            load_path = a->session_path;
        if (load_path) {
            char *err = NULL;
            if (pc_cookie_load(a->client, load_path, &err)) {
                fprintf(stderr, "pinfall: loaded cookies from %s\n",
                        load_path);
            } else {
                fprintf(stderr,
                        "pinfall: could not load cookies from %s: %s "
                        "(continuing as guest)\n",
                        load_path, (err && *err) ? err
                                                 : pc_last_error(a->client));
            }
            if (err)
                pc_free(err);
        }
    }

    if (pthread_create(&a->net_thr, NULL, net_thread_fn, a) != 0) {
        pf_android_log("app: net thread FAILED");
        fprintf(stderr, "pinfall: could not start network thread\n");
        return false;
    }
    pf_android_log("app: net thread ok");
    for (int i = 0; i < IMG_THREADS; i++) {
        if (pthread_create(&a->img_thr[i], NULL, img_thread_fn, a) != 0)
            break;
        a->n_img_thr++;
    }
#ifdef PC_ANDROID
    a->vid_available = false; /* no ffmpeg on the device */
#else
    a->vid_available = pc_video_available();
    if (pthread_create(&a->vid_thr, NULL, vid_thread_fn, a) == 0)
        a->vid_thread_started = true;
#endif

    a->state = VIEW_BOOT;
    a->last_tick = SDL_GetTicks();
    if (a->client) {
        a->state = VIEW_LOADING;
        NetCmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = NET_START;
        a->pending_query_search = a->query_explicit;
        cmd.seq = ++a->net_search_seq;
        snprintf(cmd.query, sizeof cmd.query, "%s", a->query);
        cmd.preview = auth_preview;
        cmd.save_session = save_session;
        snprintf(cmd.session_path, sizeof cmd.session_path, "%s",
                 a->session_path);
        app_push_net(a, &cmd);
    } else {
        a->state = VIEW_ERROR;
    }
    return true;
}

static void app_shutdown(App *a)
{
    app_stop_threads(a);
    app_free_queues(a);

    for (size_t i = 0; i < a->npins; i++)
        tex_clear(a, &a->pins[i]);
    a->tex_count = 0;
    a->tex_bytes = 0;
    if (a->detail.tex)
        SDL_DestroyTexture(a->detail.tex);
    a->detail.tex = NULL;
    if (a->badge_tex)
        SDL_DestroyTexture(a->badge_tex);
    a->badge_tex = NULL;
    app_video_clear_ui(a);
    free(a->vid_pixels);
    a->vid_pixels = NULL;
    pthread_mutex_destroy(&a->vid_mtx);
    pthread_cond_destroy(&a->vid_cv);
    pc_boardlist_free(&a->picker.boards);

    if (a->cur_arrow)
        SDL_FreeCursor(a->cur_arrow);
    if (a->cur_hand)
        SDL_FreeCursor(a->cur_hand);
    if (a->cur_ibeam)
        SDL_FreeCursor(a->cur_ibeam);
    if (a->client)
        pc_client_free(a->client);
    a->client = NULL;

    layout_free(&a->layout);
    fonts_free(&a->fonts);
    rnd_free(&a->rnd);

    pthread_mutex_destroy(&a->net_mtx);
    pthread_cond_destroy(&a->net_cv);
    pthread_mutex_destroy(&a->img_mtx);
    pthread_cond_destroy(&a->img_cv);
}

static int app_pump_once(App *a, bool process_input)
{
#ifdef PC_ANDROID
    if (a->android_backgrounded) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                a->running = false;
            else
                app_handle_event(a, &ev);
        }
        SDL_Delay(50);
        return 0;
    }
#endif
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (!process_input && e.type != SDL_QUIT &&
            e.type != SDL_WINDOWEVENT)
            continue;
        app_handle_event(a, &e);
    }
    app_consume_net(a);
    app_consume_images(a);

    Uint32 now = SDL_GetTicks();
    float dt = (float)(now - a->last_tick) / 1000.0f;
    if (dt < 0.0f || dt > 0.1f)
        dt = 1.0f / 60.0f;
    if (dt < 0.001f)
        dt = 0.001f;
    a->last_tick = now;
    a->dt = dt;
    a->anim_t += dt;

    app_update(a);
    app_draw(a);
    SDL_RenderPresent(a->rnd.r);
    return 0;
}

static int app_run_interactive(App *a)
{
    a->running = true;
    while (a->running) {
        app_pump_once(a, true);
    }
    return 0;
}

static bool snap_home_ready(App *a)
{
    return a->feed_mode == FEED_HOME && a->state == VIEW_FEED;
}

static bool snap_saved_ready(App *a)
{
    return a->feed_mode == FEED_SAVED &&
           (a->state == VIEW_FEED || a->state == VIEW_EMPTY);
}

static bool snap_vis_ready(App *a)
{
    return app_visible_pending(a) == 0;
}

static bool snap_picker_ready(App *a)
{
    return a->picker.open &&
           (a->picker.boards_ready || !a->picker.loading);
}

typedef bool (*SnapPred)(App *a);

static void snap_pump(App *a)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            return;
        if (e.type == SDL_WINDOWEVENT)
            app_handle_event(a, &e);
    }
    app_consume_net(a);
    app_consume_images(a);
    app_update(a);
    app_draw(a);
    SDL_RenderPresent(a->rnd.r);
    SDL_Delay(8);
}

static bool snap_wait(App *a, SnapPred pred, Uint32 deadline)
{
    while (!pred(a)) {
        if (SDL_GetTicks() > deadline)
            return false;
        snap_pump(a);
    }
    return true;
}

static int app_run_emoji_test(App *a, const char *path)
{
    Rnd *r = &a->rnd;
    float s = r->scale;
    rnd_clear(r, THEME.surface);

    static const char *samples[4] = {
        "Hello \xF0\x9F\x98\x80 world",
        "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD \xE2\x9D\xA4\xEF\xB8\x8F "
        "\xF0\x9F\x98\x82 \xF0\x9F\x8E\x89",
        "family \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7",
        "flag \xF0\x9F\x87\xB3\xF0\x9F\x87\xB1 and \xE2\x98\x80\xEF\xB8\x8F "
        "text",
    };
    int sizes[4] = {14, 16, 24, 28};
    float y = 32.0f * s;
    fonts_draw(r->r, a->fonts, "PinFall color emoji test (NotoColorEmoji)",
               24.0f * s, y, 16, true, THEME.text_dim);
    y += (float)fonts_line_h(a->fonts, 16, true) + 14.0f * s;
    for (int si = 0; si < 4; si++) {
        for (int k = 0; k < 4; k++) {
            int sz = sizes[si];
            char line[256];
            snprintf(line, sizeof line, "%2dpx  %s", sz, samples[k]);
            int h = fonts_draw(r->r, a->fonts, line, 32.0f * s, y, sz,
                               false, THEME.text);
            y += (float)(h > 0 ? h : fonts_line_h(a->fonts, sz, false)) +
                 6.0f * s;
            if (y > (float)r->out_h - 40.0f * s)
                break;
        }
    }

    bool saved = rnd_save_bmp(r->r, path);
    int colored = 0;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
        0, r->out_w, r->out_h, 32, SDL_PIXELFORMAT_RGBA32);
    if (surf && SDL_RenderReadPixels(r->r, NULL, SDL_PIXELFORMAT_RGBA32,
                                     surf->pixels, surf->pitch) == 0) {
        for (int yy = 0; yy < surf->h; yy++) {
            const Uint32 *row =
                (const Uint32 *)((const Uint8 *)surf->pixels +
                                 (size_t)yy * surf->pitch);
            for (int xx = 0; xx < surf->w; xx++) {
                Uint8 cr, cg, cb, ca;
                SDL_GetRGBA(row[xx], surf->format, &cr, &cg, &cb, &ca);
                if (ca > 60) {
                    Uint8 mx = cr > cg ? (cr > cb ? cr : cb)
                                       : (cg > cb ? cg : cb);
                    Uint8 mn = cr < cg ? (cr < cb ? cr : cb)
                                       : (cg < cb ? cg : cb);
                    if ((int)mx - (int)mn > 30)
                        colored++;
                }
            }
        }
    }
    if (surf)
        SDL_FreeSurface(surf);
    printf("pinfall-emoji: available=%d colored_pixels=%d path=%s\n",
           fonts_emoji_available() ? 1 : 0, colored, path);
    if (!saved)
        fprintf(stderr, "pinfall: emoji-test could not write %s\n", path);
    return saved ? 0 : 1;
}

static int app_run_snapshot(App *a)
{
    a->snap_start = SDL_GetTicks();
    a->snap_settle = 0;
    int ready = 0;
    bool timed_out = false;

    for (;;) {
        app_pump_once(a, false);

        ready = app_count_ready_images(a);
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - a->snap_start;

        bool enough = (a->state == VIEW_FEED && ready >= SNAP_MIN_IMAGES &&
                       app_visible_pending(a) == 0);
        bool failed = (a->state == VIEW_ERROR);
        if (enough || failed) {
            if (a->snap_settle == 0)
                a->snap_settle = now + 300;
            if (now >= a->snap_settle)
                break;
        } else {
            a->snap_settle = 0;
        }
        if (elapsed > (Uint32)a->snap_timeout) {
            timed_out = true;
            break;
        }
        SDL_Delay(10);
    }

    /* optional verification hooks: Saved/Home tab + board picker modal */
    Uint32 hook_dl = a->snap_start + (Uint32)a->snap_timeout;
    if (a->auth == PC_AUTH_LOGGED_IN && a->snap_tab == 1) {
        app_go_home(a);
        snap_wait(a, snap_home_ready, hook_dl);
    } else if (a->auth == PC_AUTH_LOGGED_IN && a->snap_tab == 2) {
        app_go_saved(a);
        snap_wait(a, snap_saved_ready, hook_dl);
    }
    if (a->snap_tab != 0) {
        Uint32 vdl = SDL_GetTicks() + 9000;
        if (vdl > hook_dl)
            vdl = hook_dl;
        snap_wait(a, snap_vis_ready, vdl);
        for (int i = 0; i < 20; i++)
            snap_pump(a);
    }

    /* optional verification hooks: scroll offset and detail overlay */
    if (a->snap_scroll > 0.0f && a->state == VIEW_FEED) {
        float ms = max_scroll(a);
        a->scroll = a->snap_scroll < ms ? a->snap_scroll : ms;
        a->scroll_target = a->scroll;
        app_update(a);
        app_draw(a);
        SDL_RenderPresent(a->rnd.r);
        Uint32 dl = a->snap_start + (Uint32)a->snap_timeout;
        while (app_visible_pending(a) > 0 && SDL_GetTicks() < dl) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT)
                    goto scroll_wait_done;
                if (e.type == SDL_WINDOWEVENT)
                    app_handle_event(a, &e);
            }
            app_consume_images(a);
            app_update(a);
            app_draw(a);
            SDL_RenderPresent(a->rnd.r);
            SDL_Delay(10);
        }
    scroll_wait_done:
        SDL_Delay(80);
    }
    if (a->state == VIEW_FEED && a->npins > 0 &&
        (a->snap_detail >= 0 || getenv("PC_SNAP_DETAIL_ID"))) {
        int didx = a->snap_detail;
        const char *did_env = getenv("PC_SNAP_DETAIL_ID");

        if (getenv("PC_DEBUG_DETAIL")) {
            size_t nv = 0;
            for (size_t i = 0; i < a->npins; i++)
                if (a->pins[i].pin.is_video)
                    nv++;
            fprintf(stderr,
                    "DBG feed mode=%d npins=%zu videos=%zu q=%.20s "
                    "ids=%.20s,%.20s,%.20s\n",
                    (int)a->feed_mode, a->npins, nv, a->query,
                    a->npins > 0 ? a->pins[0].pin.id : "-",
                    a->npins > 1 ? a->pins[1].pin.id : "-",
                    a->npins > 2 ? a->pins[2].pin.id : "-");
        }

        if (did_env && *did_env) {
            didx = -1;
            if (strcmp(did_env, "first-video") == 0) {
                for (size_t i = 0; i < a->npins; i++)
                    if (a->pins[i].pin.is_video) {
                        didx = (int)i;
                        break;
                    }
            } else {
                for (size_t i = 0; i < a->npins; i++)
                    if (strcmp(a->pins[i].pin.id, did_env) == 0) {
                        didx = (int)i;
                        break;
                    }
            }
        }
        if (didx < 0 || (size_t)didx >= a->npins)
            didx = a->snap_detail;
        if (didx < 0 || (size_t)didx >= a->npins)
            goto snap_no_detail;
        app_open_detail(a, didx);
        Uint32 d0 = SDL_GetTicks();
        while (a->detail.status != 2 && SDL_GetTicks() - d0 < 9000) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT)
                    break;
            }
            app_consume_images(a);
            app_update(a);
            app_draw(a);
            SDL_RenderPresent(a->rnd.r);
            SDL_Delay(10);
        }
        /* close-up v6 verification hooks */
        if (a->snap_like)
            app_detail_like_toggle(a);
        if (a->snap_comment_draft[0]) {
            snprintf(a->comment_draft, sizeof a->comment_draft, "%s",
                     a->snap_comment_draft);
            a->comment_draft_len = (int)strlen(a->comment_draft);
            a->composer_focus = true;
        }
        Uint32 s0 = SDL_GetTicks();
        while ((!a->comments_ready || !a->related_ready) &&
               SDL_GetTicks() - s0 < 5000) {
            snap_pump(a);
        }
        /* Verification hook: enqueue exactly the append command the
         * "View more comments" click sends, then wait for the list to grow.
         * PC_SNAP_MORE_COMMENTS=1 ./pinfall --snapshot x.bmp --detail N ... */
        if (getenv("PC_SNAP_MORE_COMMENTS") && a->comments.bookmark[0] &&
            a->comments_ready) {
            size_t before = a->comments.count;
            NetCmd lm;

            memset(&lm, 0, sizeof lm);
            lm.kind = NET_COMMENTS;
            lm.seq = ++a->comments_seq;
            lm.append = true;
            snprintf(lm.pin_id, sizeof lm.pin_id, "%s", a->detail_pin.id);
            snprintf(lm.bookmark, sizeof lm.bookmark, "%s",
                     a->comments.bookmark);
            a->comments_loading = true;
            app_push_net(a, &lm);

            Uint32 m0 = SDL_GetTicks();
            while ((a->comments.count == before || a->comments_loading) &&
                   SDL_GetTicks() - m0 < 15000)
                snap_pump(a);
            if (getenv("PC_DEBUG_DETAIL"))
                fprintf(stderr,
                        "DBG loadmore comments before=%zu after=%zu bm=%zu\n",
                        before, a->comments.count,
                        strlen(a->comments.bookmark));
        }
        /* video hook: wait for decoded frames, optionally pause */
        if (a->video_active) {
            if (a->video_playing) {
                int want = a->snap_video_frame > 0 ? a->snap_video_frame : 3;
                if (want < 3)
                    want = 3;
                Uint32 vdl = SDL_GetTicks() + 8000;
                while ((int)a->video_frames < want &&
                       SDL_GetTicks() < vdl)
                    snap_pump(a);
            }
            if (a->snap_video_pause && a->video_playing &&
                !a->video_paused)
                app_video_toggle_play(a);
            for (int i = 0; i < 12; i++)
                snap_pump(a);
        }

        if (a->snap_detail_tab == 1 || a->snap_detail_tab == 2) {
            app_detail_measure(a);
            if (a->snap_detail_tab == 2) {
                float lmax = a->left_content_h - a->left_view_h;
                if (lmax < 0.0f)
                    lmax = 0.0f;
                float lt = a->d_rel_y > lmax ? lmax : a->d_rel_y;
                a->left_scroll = lt;
                a->left_scroll_target = lt;
            } else {
                a->detail_scroll = 0.0f;
                a->detail_scroll_target = 0.0f;
            }
            app_update(a);
        }
        Uint32 h0 = SDL_GetTicks();
        while (SDL_GetTicks() - h0 < 1200)
            snap_pump(a);
        for (int i = 0; i < 20; i++)
            snap_pump(a);
    }

    /* board-picker modal hook (after the feed is populated) */
    if (a->snap_open_picker && a->npins > 0 && a->state == VIEW_FEED) {
        app_open_picker(a, 0);
        snap_wait(a, snap_picker_ready, SDL_GetTicks() + 9000);
        for (int i = 0; i < 20; i++)
            snap_pump(a);
    }

#ifdef PC_ANDROID
    /* mobile-state hook (Android offscreen verification only): shows a mobile
     * overlay so the sheets/search/profile can be captured without a device. */
    {
        const char *mstate = getenv("PINFALL_MOBILE_STATE");
        if (mstate && *mstate) {
            if (strcmp(mstate, "search") == 0) {
                a->mobile_search = true;
                a->search_focus = false;
                app_layout(a);
                for (int i = 0; i < 10; i++)
                    snap_pump(a);
            } else if (strcmp(mstate, "profile") == 0) {
                app_open_settings(a);
                for (int i = 0; i < 10; i++)
                    snap_pump(a);
            } else if (a->show_detail) {
                if (strcmp(mstate, "comments") == 0) {
                    a->comments_sheet = true;
                } else if (strcmp(mstate, "save") == 0) {
                    app_open_picker(a, a->detail_idx);
                    snap_wait(a, snap_picker_ready, SDL_GetTicks() + 9000);
                }
                for (int i = 0; i < 12; i++)
                    snap_pump(a);
            }
        }
    }
#endif

    /* create-board row hook: picker open with the name field primed */
    if (a->snap_create_board && a->npins > 0 && a->state == VIEW_FEED) {
        app_open_picker(a, 0);
        snap_wait(a, snap_picker_ready, SDL_GetTicks() + 9000);
        snprintf(a->picker.new_name, sizeof a->picker.new_name,
                 "Reading list");
        a->picker.new_name_len = (int)strlen(a->picker.new_name);
        a->picker.create_focus = true;
        for (int i = 0; i < 16; i++)
            snap_pump(a);
    }

    /* settings / about modal hooks */
    if (a->snap_open_about) {
        app_open_about(a);
        for (int i = 0; i < 12; i++)
            snap_pump(a);
    } else if (a->snap_open_settings) {
        app_open_settings(a);
        for (int i = 0; i < 12; i++)
            snap_pump(a);
    }

    /* hover hook: show the grid Save pill on pin <n> */
    if (a->snap_hover >= 0) {
        a->hover = a->snap_hover;
        a->mx = a->layout.items[a->snap_hover].x +
                a->layout.items[a->snap_hover].w * 0.5f;
        a->my = a->layout.items[a->snap_hover].y - a->scroll +
                a->layout.items[a->snap_hover].img_h * 0.5f;
        for (int i = 0; i < 40; i++)
            snap_pump(a);
    }
snap_no_detail:;

    /* final frame, then read back */
    app_draw(a);
    bool saved = rnd_save_bmp(a->rnd.r, a->snap_path);
    SDL_RenderPresent(a->rnd.r);
    a->snap_saved = saved;

    ready = app_count_ready_images(a);
    Uint32 elapsed = SDL_GetTicks() - a->snap_start;
    if (a->video_active)
        fprintf(stderr,
                "pinfall-video: frames=%llu playing=%d paused=%d failed=%d "
                "duration_ms=%d\n",
                (unsigned long long)a->video_frames,
                a->video_playing ? 1 : 0, a->video_paused ? 1 : 0,
                a->video_failed ? 1 : 0, a->detail_pin.video_duration_ms);
    printf("pinfall: snapshot query=\"%s\" state=%s pins=%zu images=%d/%d "
           "-> %s (%dx%d) in %u ms%s\n",
           a->query,
           a->state == VIEW_ERROR
               ? "error"
               : (a->state == VIEW_FEED ? "feed" : "loading"),
           a->npins, ready, (int)a->npins, a->snap_path, a->rnd.out_w,
           a->rnd.out_h, (unsigned)elapsed,
           timed_out ? " [timeout]" : "");
    if (a->state == VIEW_ERROR)
        printf("pinfall: error state: %s\n", a->error);

    if (!saved) {
        fprintf(stderr, "pinfall: failed to save snapshot to %s: %s\n",
                a->snap_path, SDL_GetError());
        return 1;
    }
    if (a->state != VIEW_FEED || ready < SNAP_MIN_IMAGES) {
        fprintf(stderr,
                "pinfall: warning: snapshot is degraded (%d/%d images ready, "
                "state %s)\n",
                ready, (int)a->npins,
                a->state == VIEW_ERROR ? "error" : "loading");
        return 2;
    }
    return 0;
}


/* --------------------------------------------------------------- selftest
 * Headless interaction proof: drives the REAL event handlers by pushing
 * synthetic SDL events into the queue (no X server, no window on the user's
 * desktop when SDL_VIDEODRIVER=offscreen). Writes a BMP after each step.
 */
typedef bool (*StPred)(App *a);

static bool st_pump(App *a, int frames)
{
    for (int f = 0; f < frames; f++) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            app_handle_event(a, &e);
        app_consume_net(a);
        app_consume_images(a);

        Uint32 now = SDL_GetTicks();
        float dt = (float)(now - a->last_tick) / 1000.0f;
        if (dt <= 0.0f || dt > 0.1f)
            dt = 1.0f / 60.0f;
        if (dt < 0.001f)
            dt = 0.001f;
        a->last_tick = now;
        a->dt = dt;
        a->anim_t += dt;

        app_update(a);
        app_draw(a);
        SDL_RenderPresent(a->rnd.r);
    }
    return true;
}

static bool st_wait(App *a, StPred pred, Uint32 timeout_ms)
{
    Uint32 deadline = SDL_GetTicks() + timeout_ms;
    while (!pred(a)) {
        if (SDL_GetTicks() > deadline)
            return false;
        st_pump(a, 1);
        SDL_Delay(4);
    }
    return true;
}

static void st_push_key(App *a, SDL_Keycode sym)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.keysym.sym = sym;
    e.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
    e.key.windowID = SDL_GetWindowID(a->win);
    SDL_PushEvent(&e);
}

static void st_push_text(const char *text)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_TEXTINPUT;
    snprintf(e.text.text, sizeof e.text.text, "%s", text);
    SDL_PushEvent(&e);
}

static void st_push_move(App *a, float x, float y)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEMOTION;
    e.motion.windowID = SDL_GetWindowID(a->win);
    e.motion.x = (Sint32)x;
    e.motion.y = (Sint32)y;
    SDL_PushEvent(&e);
}

static void st_push_motion(App *a, float x, float y)
{
    (void)a;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_MOUSEMOTION;
    e.motion.x = (int)x;
    e.motion.y = (int)y;
    SDL_PushEvent(&e);
}

static void st_push_click(App *a, float x, float y)
{
    st_push_move(a, x, y);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.windowID = SDL_GetWindowID(a->win);
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_PRESSED;
    e.button.clicks = 1;
    e.button.x = (Sint32)x;
    e.button.y = (Sint32)y;
    SDL_PushEvent(&e);

    SDL_zero(e);
    e.type = SDL_MOUSEBUTTONUP;
    e.button.windowID = SDL_GetWindowID(a->win);
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_RELEASED;
    e.button.clicks = 1;
    e.button.x = (Sint32)x;
    e.button.y = (Sint32)y;
    SDL_PushEvent(&e);
}

static void st_push_wheel(int y)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_MOUSEWHEEL;
    e.wheel.y = y;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&e);
}

static bool st_feed_ready(App *a)
{
    return a->state == VIEW_FEED && app_count_ready_images(a) >= SNAP_MIN_IMAGES;
}

static bool st_at_top(App *a)
{
    return a->scroll < 2.0f;
}

static bool st_saved_ready(App *a)
{
    return a->feed_mode == FEED_SAVED &&
           (a->state == VIEW_FEED || a->state == VIEW_EMPTY);
}

static bool st_home_ready(App *a)
{
    return a->feed_mode == FEED_HOME && a->state == VIEW_FEED;
}

static bool st_picker_ready(App *a)
{
    return a->picker.open && a->picker.boards_ready;
}

static bool st_anime_ready(App *a)
{
    return a->state == VIEW_FEED && strcmp(a->query, "anime") == 0 &&
           a->npins > 0;
}

static bool st_search_done(App *a)
{
    return a->state == VIEW_FEED && strcmp(a->query, "cats") == 0 &&
           a->npins > 0;
}

typedef struct {
    bool ok;
    int tex_top, tex_bottom, tex_back, evicted, band_pins, bound;
    unsigned long long hits_top, hits_bottom, hits_after;
    unsigned long long misses_top, misses_bottom, misses_after;
    unsigned long long packs, unpacks;
} EvictResult;

/* Deterministic residency proof:
 *   1. go to the top and force the top band through LZ4 (cache warm),
 *   2. jump to the ABSOLUTE bottom, count how many previously-loaded pins had
 *      their texture freed, and check texture count is bounded by the band,
 *   3. jump straight back to the top: reloads must be cache hits and must not
 *      cause any new network misses. */
static bool st_eviction_case(App *a, const char *prefix, EvictResult *res)
{
    memset(res, 0, sizeof *res);
    if (!a->client || a->npins < 30 || a->state != VIEW_FEED)
        return false;
    char path_stats[700];
    snprintf(path_stats, sizeof path_stats, "%s_stats.bmp", prefix);

    PfSettings saved_settings = a->settings;
    bool saved_snapshot = a->snapshot;
    a->snapshot = true; /* freeze pagination: measure a fixed feed */
    a->settings.cache_mode = PC_IMG_CACHE_LZ4;
    a->settings.budget_mb = 64;
    a->settings.keep_screens = 1;
    a->settings.show_stats = true;
    app_apply_settings(a);

    /* 1. top of the feed, all textures dropped so they warm through LZ4 */
    a->scroll = 0.0f;
    a->scroll_target = 0.0f;
    app_update(a);
    Uint32 wdl = SDL_GetTicks() + 12000;
    while ((app_band_pending(a) > 0 || app_count_ready_images(a) < 8) &&
           SDL_GetTicks() < wdl) {
        st_pump(a, 1);
        SDL_Delay(2);
    }
    {
        float t, b;
        app_texture_band(a, &t, &b);
        size_t nn = a->npins < a->layout.count ? a->npins
                                               : a->layout.count;
        for (size_t i = 0; i < nn; i++) {
            const LayoutItem *it = &a->layout.items[i];
            bool in_band = it->y + it->card_h >= t && it->y <= b;
            if (in_band && a->pins[i].tex) {
                tex_clear(a, &a->pins[i]);
                a->pins[i].status = 0;
            }
        }
        wdl = SDL_GetTicks() + 12000;
        while ((app_band_pending(a) > 0 ||
                app_count_ready_images(a) < 8) &&
               SDL_GetTicks() < wdl) {
            st_pump(a, 1);
            SDL_Delay(2);
        }
        st_pump(a, 10);
    }
    app_update_stats(a);
    app_recount_textures(a);
    res->hits_top = a->img_stats.hits;
    res->misses_top = a->img_stats.misses;
    res->tex_top = a->tex_count;
    int loaded_idx[256];
    int n_loaded = 0;
    size_t nn = a->npins < a->layout.count ? a->npins : a->layout.count;
    for (size_t i = 0; i < nn && n_loaded < 256; i++)
        if (a->pins[i].tex)
            loaded_idx[n_loaded++] = (int)i;

    /* 2. jump to the absolute bottom (not a fraction: paging/layout changes
     * move max_scroll, which made a fractional jump non-deterministic). */
    float bottom = max_scroll(a);
    a->scroll = bottom;
    a->scroll_target = bottom;
    app_update(a);
    Uint32 bdl = SDL_GetTicks() + 12000;
    while ((app_band_pending(a) > 0 || fabsf(a->scroll - bottom) > 2.0f) &&
           SDL_GetTicks() < bdl) {
        st_pump(a, 1);
        SDL_Delay(2);
    }
    st_pump(a, 6);
    app_update_stats(a);
    app_recount_textures(a);
    res->tex_bottom = a->tex_count;
    res->hits_bottom = a->img_stats.hits;
    res->misses_bottom = a->img_stats.misses;
    /* Feed length may have changed since the warm step; recount against the
     * current layout so band/texture accounting cannot go stale. */
    nn = a->npins < a->layout.count ? a->npins : a->layout.count;
    float btop, bbot;
    app_texture_band(a, &btop, &bbot);
    for (size_t i = 0; i < nn; i++) {
        const LayoutItem *it = &a->layout.items[i];
        if (it->y + it->card_h >= btop && it->y <= bbot)
            res->band_pins++;
    }
    for (int k = 0; k < n_loaded; k++) {
        const LayoutItem *it = &a->layout.items[loaded_idx[k]];
        bool in_band = it->y + it->card_h >= btop && it->y <= bbot;
        if (!in_band && !a->pins[loaded_idx[k]].tex)
            res->evicted++;
    }
    int outside_band_tex = 0;
    int textured_pins = 0;
    for (size_t i = 0; i < nn; i++) {
        const LayoutItem *it = &a->layout.items[i];
        bool in_band = it->y + it->card_h >= btop && it->y <= bbot;
        if (a->pins[i].tex)
            textured_pins++;
        if (!in_band && a->pins[i].tex)
            outside_band_tex++;
    }
    res->bound = res->band_pins + 15; /* in-flight/just-arrived slack */
    rnd_save_bmp(a->rnd.r, path_stats);

    /* 3. straight back to the top: cached pins must reload without fetches */
    a->scroll = 0.0f;
    a->scroll_target = 0.0f;
    app_update(a);
    Uint32 rdl = SDL_GetTicks() + 12000;
    while (app_band_pending(a) > 0 && SDL_GetTicks() < rdl) {
        st_pump(a, 1);
        SDL_Delay(2);
    }
    st_pump(a, 10);
    app_update_stats(a);
    app_recount_textures(a);
    res->tex_back = a->tex_count;
    res->hits_after = a->img_stats.hits;
    res->misses_after = a->img_stats.misses;
    res->packs = a->img_stats.packs;
    res->unpacks = a->img_stats.unpacks;
    bool reloaded = app_count_ready_images(a) >= 8;
    res->ok = (res->evicted >= 5) &&
              (res->tex_bottom <= res->bound) &&
              (res->tex_bottom <= res->tex_top * 3 + 40) &&
              (res->tex_back > 0) && reloaded &&
              (res->hits_after > res->hits_bottom) &&
              (res->misses_after == res->misses_bottom) &&
              (res->packs > 0) && (res->unpacks > 0);
    printf("selftest: eviction tex top=%d bottom=%d back=%d (band %d, bound "
           "%d), evicted %d loaded pins, hits %llu->%llu->%llu, misses "
           "%llu->%llu->%llu, packs %llu unpacks %llu\n",
           res->tex_top, res->tex_bottom, res->tex_back, res->band_pins,
           res->bound, res->evicted, res->hits_top, res->hits_bottom,
           res->hits_after, res->misses_top, res->misses_bottom,
           res->misses_after, res->packs, res->unpacks);
    printf("selftest: eviction details nn=%zu npins=%zu layout=%zu "
           "outside_band_tex=%d textured_pins=%d tex_bottom=%d band=%d\n",
           nn, a->npins, a->layout.count, outside_band_tex, textured_pins,
           res->tex_bottom, res->band_pins);

    a->settings = saved_settings;
    a->snapshot = saved_snapshot;
    app_apply_settings(a);
    st_pump(a, 10);
    return true;
}

static int app_run_selftest(App *a, const char *prefix)
{
    char path_scroll[700], path_search[700], path_detail[700];
    char path_saved[700], path_picker[700], path_stats[700];
    char path_search_paging[700];
    snprintf(path_scroll, sizeof path_scroll, "%s_scroll.bmp", prefix);
    snprintf(path_search, sizeof path_search, "%s_search.bmp", prefix);
    snprintf(path_detail, sizeof path_detail, "%s_detail.bmp", prefix);
    snprintf(path_saved, sizeof path_saved, "%s_saved.bmp", prefix);
    snprintf(path_picker, sizeof path_picker, "%s_picker.bmp", prefix);
    snprintf(path_stats, sizeof path_stats, "%s_stats.bmp", prefix);
    snprintf(path_search_paging, sizeof path_search_paging,
             "%s_search_paging.bmp", prefix);

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("pinfall: selftest starting (driver=%s, synthetic SDL events)\n",
           SDL_GetCurrentVideoDriver());

    /* 0a. comment unescape fixture (\uXXXX incl. surrogate pair + HTML). */
    {
        char ubuf[256];
        app_unescape_text(ubuf, sizeof ubuf,
                          "A\\u200dB &amp; &#39; \\uD83D\\uDE00 end");
        if (strstr(ubuf, "\\u") || strstr(ubuf, "&amp;") ||
            !strstr(ubuf, "\xE2\x80\x8D") ||
            !strstr(ubuf, "\xF0\x9F\x98\x80") || !strstr(ubuf, "'")) {
            fprintf(stderr, "selftest: unescape FAILED (\"%s\")\n", ubuf);
            return 1;
        }
        printf("selftest: unescape PASS (\"%s\")\n", ubuf);
    }

    /* theme palette + settings persistence fixture */
    {
        ThemeMode prev = theme_mode();
        theme_apply();
        SDL_Color lbg = THEME.bg;
        SDL_Color ltx = THEME.text;
        theme_set_mode(THEME_MODE_DARK);
        theme_apply();
        bool differ = (THEME.bg.r != lbg.r || THEME.bg.g != lbg.g ||
                       THEME.bg.b != lbg.b) &&
                      (THEME.text.r != ltx.r || THEME.text.g != ltx.g ||
                       THEME.text.b != ltx.b);
        int mode = -1;
        bool parsed = settings_parse_theme("dark", &mode) && mode == 2 &&
                      settings_parse_theme("system", &mode) && mode == 0 &&
                      strcmp(settings_theme_name(2), "dark") == 0;
        theme_set_mode(prev);
        theme_apply();
        if (!differ || !parsed) {
            fprintf(stderr, "selftest: theme FAILED (differ=%d parsed=%d)\n",
                    (int)differ, (int)parsed);
            return 1;
        }
        printf("selftest: theme PASS (dark bg=%u,%u,%u)\n",
               THEME_DARK.bg.r, THEME_DARK.bg.g, THEME_DARK.bg.b);
    }

    bool ok_feed = st_wait(a, st_feed_ready, 30000);
    if (!ok_feed) {
        fprintf(stderr, "selftest: FAIL feed did not load (%s)\n",
                a->error[0] ? a->error : "timeout");
        return 1;
    }
    st_pump(a, 15);

    /* 0. cookie-login rendering path: --auth-preview must show the signed-in
     * header (Home + Saved tabs, username chip) and the tabs must reload. */
    bool ok_auth = true;
    bool ok_saved = true, ok_picker = true, ok_save = true;
    bool ok_picker_esc = true;
    bool auth_case = a->auth_preview;
    if (auth_case) {
        ok_auth = (a->auth == PC_AUTH_LOGGED_IN) &&
                  a->feed_mode == FEED_HOME && a->username[0] != '\0' &&
                  a->home_btn.w > 0 && a->saved_btn.w > 0;
        if (ok_auth) {
            float hx = (float)a->home_btn.x + (float)a->home_btn.w * 0.5f;
            float hy = (float)a->home_btn.y + (float)a->home_btn.h * 0.5f;
            st_push_click(a, hx, hy);
            st_pump(a, 6);
            ok_auth = st_wait(a, st_feed_ready, 25000) &&
                      a->feed_mode == FEED_HOME &&
                      a->auth == PC_AUTH_LOGGED_IN &&
                      a->username[0] != '\0';
        }
        if (!ok_auth)
            fprintf(stderr, "selftest: auth-preview header/Home-tab FAILED\n");
        if (ok_auth && a->saved_btn.w > 0) {
            float sx = (float)a->saved_btn.x + (float)a->saved_btn.w * 0.5f;
            float sy = (float)a->saved_btn.y + (float)a->saved_btn.h * 0.5f;
            st_push_click(a, sx, sy);
            st_pump(a, 6);
            ok_saved = st_wait(a, st_saved_ready, 25000) &&
                       a->feed_mode == FEED_SAVED;
            st_pump(a, 8);
            rnd_save_bmp(a->rnd.r, path_saved);
            /* back to Home for the remaining interaction asserts */
            float hx = (float)a->home_btn.x + (float)a->home_btn.w * 0.5f;
            float hy = (float)a->home_btn.y + (float)a->home_btn.h * 0.5f;
            st_push_click(a, hx, hy);
            st_wait(a, st_home_ready, 25000);
            st_pump(a, 8);
        } else if (ok_auth) {
            ok_saved = false;
        }
        if (!ok_saved)
            fprintf(stderr, "selftest: Saved tab FAILED\n");
    }

    /* 0b. texture residency + RAM cache: deep scroll evicts, scroll back
     * reloads from the compressed cache. On a 25-pin first page there may be
     * too little content to evict, so the case also runs after paging. */
    bool ok_evict = true, ok_settings = true;
    bool eviction_done = false;
    EvictResult ev;
    memset(&ev, 0, sizeof ev);

    if (a->settings_path[0]) {
        char probe_path[700];
        snprintf(probe_path, sizeof probe_path, "%s_settingsprobe.conf",
                 prefix);
        PfSettings probe = a->settings;
        ok_settings = settings_save_to(&probe, probe_path);
        PfSettings round;
        memset(&round, 0, sizeof round);
        ok_settings = ok_settings && settings_load_from(&round, probe_path) &&
                      round.cache_mode == probe.cache_mode &&
                      round.budget_mb == probe.budget_mb &&
                      round.keep_screens == probe.keep_screens &&
                      round.show_stats == probe.show_stats &&
                      round.block_ads == probe.block_ads;
        remove(probe_path);
    }
    if (a->npins >= 40 && a->state == VIEW_FEED && a->client) {
        eviction_done = st_eviction_case(a, prefix, &ev);
        ok_evict = ev.ok;
    }

    /* 1. mouse wheel -> scroll offset must change (smooth animation). */
    float scroll_before = a->scroll;
    for (int i = 0; i < 3; i++) {
        st_push_wheel(-1);
        st_pump(a, 2);
    }
    Uint32 dl = SDL_GetTicks() + 2000;
    while (a->scroll < scroll_before + 60.0f && SDL_GetTicks() < dl)
        st_pump(a, 1);
    float scroll_after = a->scroll;
    bool ok_scroll = scroll_after > scroll_before + 60.0f;
    st_pump(a, 2);
    bool saved_scroll = rnd_save_bmp(a->rnd.r, path_scroll);

    /* 2. Home key -> back to top (keyboard scroll path). */
    st_push_key(a, SDLK_HOME);
    bool ok_home = st_wait(a, st_at_top, 3000);
    st_pump(a, 8);

    /* hover a card: the Save pill must appear and open the picker for it */
    bool ok_hover = true, ok_hover_click = true;
    if (a->layout.count > 0 && (size_t)a->npins > 0) {
        const LayoutItem *it = &a->layout.items[0];
        float hx = it->x + it->w * 0.5f;
        float hy = it->y - a->scroll + it->img_h * 0.5f;
        st_push_motion(a, hx, hy);
        st_pump(a, 24);
        bool pill = a->card_save_rect.w > 0 && a->hover == 0;
        ok_hover = pill;
        if (pill) {
            float bx = (float)a->card_save_rect.x +
                       (float)a->card_save_rect.w * 0.5f;
            float by = (float)a->card_save_rect.y +
                       (float)a->card_save_rect.h * 0.5f;
            st_push_click(a, bx, by);
            st_pump(a, 10);
            if (writes_enabled(a)) {
                ok_hover_click = a->picker.open &&
                                 a->picker.pin_idx == a->hover;
                if (a->picker.open) {
                    st_push_key(a, SDLK_ESCAPE);
                    st_pump(a, 8);
                }
            } else {
                ok_hover_click = !a->picker.open &&
                                 strstr(a->toast, "Sign in") != NULL;
            }
        }
        if (!ok_hover || !ok_hover_click)
            fprintf(stderr, "selftest: hover save FAILED (pill=%d)\n",
                    pill ? 1 : 0);
    }

    /* 3. click a card -> detail overlay (initial query has rich captions);
     *    Esc closes it. This runs before the search test so the detail BMP
     *    shows title + board + description + copy-link. */
    bool ok_detail = false, saved_detail = false, ok_close = false;
    bool ok_actions = true, ok_comments = true, ok_related = true;
    bool ok_like = true, ok_composer = true, ok_guest_ok = true;
    bool ok_fullscreen = true, ok_image_full = true, ok_scrolls = true;
    bool ok_create = true, ok_about = true, ok_video = true;
    bool ok_emoji = true;
    bool ok_empty = true;
    int detail_status = -1;
    if (a->layout.count > 0 && (size_t)a->npins > 0) {
        const LayoutItem *it = &a->layout.items[0];
        float cx = it->x + it->w * 0.5f;
        float cy = it->y - a->scroll + it->img_h * 0.5f;
        st_push_click(a, cx, cy);
        st_pump(a, 8);
        ok_detail = a->show_detail && a->detail_idx == 0;
        if (ok_detail) {
            Uint32 ddl = SDL_GetTicks() + 5000;
            while (a->detail.status != 2 && SDL_GetTicks() < ddl)
                st_pump(a, 1);
            detail_status = a->detail.status;
            st_pump(a, 8);

            /* close-up v6: action bar / comments / related / like / composer */
            {
                Uint32 sdl = SDL_GetTicks() + 8000;
                while ((!a->comments_ready || !a->related_ready) &&
                       SDL_GetTicks() < sdl) {
                    st_pump(a, 1);
                    SDL_Delay(2);
                }
                st_pump(a, 10);
                ok_actions = a->d_like_btn.w > 0 &&
                             a->d_comment_btn.w > 0 &&
                             a->d_share_btn.w > 0 &&
                             a->d_more_btn.w > 0 && a->d_save.w > 0;
                /* full-page route: fixed rail + header, close-up card in
                 * the page masonry, image pane fit to the pane width capped
                 * at 90vh - 80px. */
                ok_fullscreen = a->d_rail_w > 0.0f &&
                                a->d_header.h > 0.0f &&
                                a->d_card.w > 0.0f &&
                                a->d_card.x >= a->d_rail_w &&
                                a->d_info.w > 0.0f && a->d_img.w <= 0.0f;
                float want_media_h =
                    a->d_media_w * (float)a->detail.th / (float)a->detail.tw;
                float cap_media_h = 0.90f * (float)a->rnd.out_h -
                                    80.0f * a->rnd.scale;
                if (want_media_h > cap_media_h)
                    want_media_h = cap_media_h;
                ok_image_full = a->detail.status == 2 &&
                                a->detail.tw > 0 && a->detail.th > 0 &&
                                fabsf(a->d_img_h - want_media_h) < 2.0f;
                if (auth_case) {
                    ok_comments = a->comments.count >= 3;
                    ok_related = a->related.count >= 20 && a->rel_n >= 20;
                    bool liked0 = a->detail_liked;
                    int likes0 = a->detail_like_count;
                    float lx = a->d_like_btn.x + a->d_like_btn.w * 0.5f;
                    float ly = a->d_header.h - a->left_scroll +
                               a->d_like_btn.y + a->d_like_btn.h * 0.5f;
                    st_push_click(a, lx, ly);
                    st_pump(a, 8);
                    bool liked1 = a->detail_liked;
                    int likes1 = a->detail_like_count;
                    st_push_click(a, lx, ly);
                    st_pump(a, 8);
                    ok_like = (liked1 == !liked0) &&
                              (likes1 == likes0 + (liked1 ? 1 : -1)) &&
                              (a->detail_liked == liked0);
                    /* composer accepts text and mock-posts (preview only) */
                    st_push_click(a,
                                  a->d_composer.x + a->d_composer.w * 0.5f,
                                  a->d_header.h - a->left_scroll +
                                      a->d_composer.y +
                                      a->d_composer.h * 0.5f);
                    st_pump(a, 4);
                    st_push_text("nice pin");
                    st_pump(a, 4);
                    bool typed = a->comment_draft_len > 0;
                    size_t cc0 = a->comments.count;
                    st_push_click(a,
                                  a->d_post_btn.x + a->d_post_btn.w * 0.5f,
                                  a->d_header.h - a->left_scroll +
                                      a->d_post_btn.y +
                                      a->d_post_btn.h * 0.5f);
                    st_pump(a, 10);
                    ok_composer = typed && a->comments.count == cc0 + 1 &&
                                  a->comment_draft_len == 0;

                    /* independent left/right scrolling */
                    a->left_scroll = 0.0f;
                    a->left_scroll_target = 0.0f;
                    a->detail_scroll = 0.0f;
                    a->detail_scroll_target = 0.0f;
                    st_pump(a, 4);
                    a->left_scroll_target = 200.0f;
                    st_pump(a, 30);
                    bool left_moved = a->left_scroll > 1.0f &&
                                      a->detail_scroll < 1.0f;
                    a->left_scroll = 0.0f;
                    a->left_scroll_target = 0.0f;
                    st_pump(a, 4);
                    a->detail_scroll_target = 160.0f;
                    st_pump(a, 30);
                    /* short right content (preview) may not need scrolling */
                    bool right_scrollable =
                        a->detail_content_h > a->detail_view_h + 4.0f;
                    bool right_moved = (a->detail_scroll > 1.0f ||
                                        !right_scrollable) &&
                                       a->left_scroll < 1.0f;
                    a->detail_scroll = 0.0f;
                    a->detail_scroll_target = 0.0f;
                    st_pump(a, 4);
                    ok_scrolls = left_moved && right_moved;

                    /* stale-count empty state: header/bubble use loaded rows */
                    a->snap_empty_comments = true;
                    app_detail_clear_social(a);
                    app_detail_load_social(a);
                    Uint32 edl = SDL_GetTicks() + 6000;
                    while ((!a->comments_ready || !a->related_ready) &&
                           SDL_GetTicks() < edl)
                        st_pump(a, 1);
                    st_pump(a, 8);
                    ok_empty = a->comments_ready && a->comments.count == 0 &&
                               app_detail_comment_count(a) == 0 &&
                               a->detail_pin.comment_count == 3 &&
                               a->d_viewmore_btn.w == 0.0f;
                    a->snap_empty_comments = false;
                    app_detail_clear_social(a);
                    app_detail_load_social(a);
                    Uint32 edl2 = SDL_GetTicks() + 6000;
                    while ((!a->comments_ready || !a->related_ready) &&
                           SDL_GetTicks() < edl2)
                        st_pump(a, 1);
                    st_pump(a, 8);

                    /* related tile click swaps the close-up + resets scroll */
                    if (a->rel_n > 0 && a->related.count > 0) {
                        a->left_scroll = 0.0f;
                        a->left_scroll_target = 0.0f;
                        a->detail_scroll = 0.0f;
                        a->detail_scroll_target = 0.0f;
                        st_pump(a, 8);
                        char want_id[PC_ID_LEN];
                        snprintf(want_id, sizeof want_id, "%s",
                                 a->related.items[0].id);
                        SDL_FRect t = a->d_rel_tiles[0];
                        float tx = t.x + t.w * 0.5f;
                        float ty = a->d_header.h - a->left_scroll + t.y +
                                   t.h * 0.5f;
                        st_push_click(a, tx, ty);
                        st_pump(a, 12);
                        bool swapped = a->show_detail &&
                                       strcmp(a->detail_pin.id, want_id) ==
                                           0 &&
                                       a->left_scroll < 2.0f &&
                                       a->detail_scroll < 2.0f;
                        ok_related = ok_related && swapped;
                        if (!swapped)
                            fprintf(stderr,
                                    "selftest: related tile swap FAILED\n");
                    }
                } else if (a->auth != PC_AUTH_LOGGED_IN) {
                    /* guest: like is disabled and never writes */
                    bool liked0 = a->detail_liked;
                    int likes0 = a->detail_like_count;
                    st_push_click(a,
                                  a->d_like_btn.x + a->d_like_btn.w * 0.5f,
                                  a->d_header.h - a->left_scroll +
                                      a->d_like_btn.y +
                                      a->d_like_btn.h * 0.5f);
                    st_pump(a, 6);
                    ok_guest_ok = !writes_enabled(a) &&
                                  a->detail_liked == liked0 &&
                                  a->detail_like_count == likes0;
                }
            }

            saved_detail = rnd_save_bmp(a->rnd.r, path_detail);

            /* Save button -> board picker -> board click -> mock save */
            if (auth_case && a->d_save.w > 0) {
                SDL_FRect sbtn = cu_screen(a, fr_from(a->d_save));
                float bx = sbtn.x + sbtn.w * 0.5f;
                float by = sbtn.y + sbtn.h * 0.5f;
                st_push_click(a, bx, by);
                st_pump(a, 6);
                ok_picker = st_wait(a, st_picker_ready, 10000) &&
                            a->picker.boards.count == 3 &&
                            a->picker.boards.items[0].pin_count >= 0;
                st_pump(a, 8);
                rnd_save_bmp(a->rnd.r, path_picker);
                if (ok_picker) {
                    SDL_FRect row;
                    if (picker_row_rect(a, 0, &row))
                        st_push_click(a, row.x + row.w * 0.5f,
                                      row.y + row.h * 0.5f);
                    st_pump(a, 10);
                    ok_save = !a->picker.open &&
                              strncmp(a->toast, "Saved to ", 9) == 0;
                    if (!ok_save)
                        fprintf(stderr,
                                "selftest: save flow FAILED (toast=\"%s\")\n",
                                a->toast);
                    /* reopen, then Esc must close the picker */
                    st_push_click(a, bx, by);
                    st_pump(a, 6);
                    bool reopened = a->picker.open;
                    st_push_key(a, SDLK_ESCAPE);
                    st_pump(a, 8);
                    ok_picker_esc = reopened && !a->picker.open;
                }
                if (!ok_picker)
                    fprintf(stderr, "selftest: board picker FAILED\n");
                /* create-board row: preview fake-create -> select -> save */
                if (auth_case && a->d_save.w > 0) {
                    SDL_FRect sb2 = cu_screen(a, fr_from(a->d_save));
                    st_push_click(a, sb2.x + sb2.w * 0.5f,
                                  sb2.y + sb2.h * 0.5f);
                    st_pump(a, 8);
                    ok_create = a->picker.open &&
                                a->picker.create_box.w > 0.0f &&
                                a->picker.boards.count == 3;
                    if (ok_create) {
                        size_t before = a->picker.boards.count;
                        st_push_click(a, a->picker.create_box.x + 20.0f,
                                      a->picker.create_box.y + 20.0f);
                        st_pump(a, 4);
                        st_push_text("Selftest Board");
                        st_pump(a, 4);
                        st_push_click(
                            a, a->picker.create_btn.x +
                                   a->picker.create_btn.w * 0.5f,
                            a->picker.create_btn.y +
                                   a->picker.create_btn.h * 0.5f);
                        st_pump(a, 14);
                        (void)before;
                        ok_create = strncmp(a->toast, "Created ", 8) == 0 &&
                                    strcmp(a->saved_board_name,
                                           "Selftest Board") == 0 &&
                                    !a->picker.open;
                        if (!ok_create)
                            fprintf(stderr,
                                    "selftest: create board FAILED "
                                    "(count=%zu toast=\"%s\")\n",
                                    a->picker.boards.count, a->toast);
                        if (a->picker.open)
                            app_close_picker(a);
                    } else if (a->picker.open) {
                        app_close_picker(a);
                    }
                }
            }
            /* related tile click swaps the close-up (preview data) */
            if (auth_case && a->rel_n > 0 && a->related.count > 0) {
                a->left_scroll = 0.0f;
                a->left_scroll_target = 0.0f;
                st_pump(a, 6);
                char want_id[PC_ID_LEN];
                snprintf(want_id, sizeof want_id, "%s",
                         a->related.items[0].id);
                float tx = a->d_rel_tiles[0].x + a->d_rel_tiles[0].w * 0.5f;
                float ty = a->d_header.h - a->left_scroll +
                           a->d_rel_tiles[0].y + a->d_rel_tiles[0].h * 0.5f;
                st_push_click(a, tx, ty);
                st_pump(a, 12);
                bool swapped = a->show_detail &&
                               strcmp(a->detail_pin.id, want_id) == 0;
                ok_related = ok_related && swapped;
                if (!swapped)
                    fprintf(stderr, "selftest: related tile swap FAILED\n");
            }
            st_push_key(a, SDLK_ESCAPE);
            st_pump(a, 8);
            ok_close = !a->show_detail;
        }
    }

    /* 4. search: click the cancel icon, type "cats", press Enter. */
    bool cleared = false;
    if (a->search_clear.w > 0) {
        st_push_click(a, (float)a->search_clear.x +
                             (float)a->search_clear.w * 0.5f,
                      (float)a->search_clear.y +
                             (float)a->search_clear.h * 0.5f);
        st_pump(a, 6);
        cleared = (a->input[0] == '\0');
    }
    if (!cleared) {
        SDL_FRect sr = search_rect(a);
        st_push_click(a, sr.x + sr.w * 0.5f, sr.y + sr.h * 0.5f);
        st_pump(a, 3);
        for (int i = 0; i < a->input_len + 2; i++) {
            st_push_key(a, SDLK_BACKSPACE);
            st_pump(a, 1);
        }
    }
    st_push_text("cats");
    st_pump(a, 4);
    st_push_key(a, SDLK_RETURN);
    bool ok_search = st_wait(a, st_search_done, 30000);
    Uint32 img_dl = SDL_GetTicks() + 8000;
    while (ok_search && app_count_ready_images(a) < 10 &&
           SDL_GetTicks() < img_dl) {
        st_pump(a, 1);
        SDL_Delay(10);
    }
    st_pump(a, 10);
    bool saved_search = rnd_save_bmp(a->rnd.r, path_search);

    printf("selftest: scroll=%s (%.0f -> %.0f px) | home=%s | "
           "search=%s (query=\"%s\", %zu pins) | detail=%s (pin 0, image "
           "status=%d) | close=%s | auth-preview=%s | saved=%s | picker=%s | "
           "save=%s | picker-esc=%s\n",
           ok_scroll ? "PASS" : "FAIL", scroll_before, scroll_after,
           ok_home ? "PASS" : "FAIL", ok_search ? "PASS" : "FAIL", a->query,
           a->npins, ok_detail ? "PASS" : "FAIL", detail_status,
           ok_close ? "PASS" : "FAIL",
           auth_case ? (ok_auth ? "PASS" : "FAIL") : "skip(guest)",
           auth_case ? (ok_saved ? "PASS" : "FAIL") : "skip(guest)",
           auth_case ? (ok_picker ? "PASS" : "FAIL") : "skip(guest)",
           auth_case ? (ok_save ? "PASS" : "FAIL") : "skip(guest)",
           auth_case ? (ok_picker_esc ? "PASS" : "FAIL") : "skip(guest)");
    printf("selftest: evict=%s settings=%s closeup=%s\n",
           ok_evict ? "PASS" : "FAIL", ok_settings ? "PASS" : "FAIL",
           (ok_actions && ok_fullscreen && ok_image_full && ok_scrolls &&
            ok_comments && ok_related && ok_like && ok_composer &&
            ok_empty && ok_guest_ok)
               ? "PASS"
               : "FAIL");
    /* About section: badge texture decoded at 121x20 + clickable URL row */
    if (a->badge_tex) {
        app_open_about(a);
        st_pump(a, 10);
        ok_about = a->show_settings && a->about_selected &&
                   a->settings_about_badge.w >= 120.0f &&
                   a->settings_about_badge.h >= 20.0f &&
                   a->settings_about_url.w > 0.0f &&
                   a->badge_tex != NULL &&
                   a->settings_about_badge.w == 121.0f &&
                   a->settings_about_badge.h == 20.0f;
        if (!ok_about)
            fprintf(stderr,
                    "selftest: about FAILED (tex=%p badge=%dx%d rect=%.0fx%.0f "
                    "url=%.0f open=%d sel=%d)\n",
                    (void *)a->badge_tex, a->badge_w, a->badge_h,
                    (double)a->settings_about_badge.w,
                    (double)a->settings_about_badge.h,
                    (double)a->settings_about_url.w, a->show_settings ? 1 : 0,
                    a->about_selected ? 1 : 0);
        app_close_settings(a);
        st_pump(a, 4);
    } else {
        ok_about = false;
    }

    /* color emoji: font available + mixed string actually renders color */
    {
        bool emoji_avail = fonts_emoji_available();
        if (emoji_avail) {
            int probe = fonts_emoji_probe_pixels();
            int wa = 0, wb = 0;
            fonts_measure(a->fonts, "A", 16, false, &wa, NULL);
            fonts_measure(a->fonts, "Að", 16, false, &wb,
                          NULL);
            ok_emoji = probe > 100 && wb > wa;
            if (!ok_emoji)
                fprintf(stderr,
                        "selftest: emoji FAILED probe=%d wa=%d wb=%d\n",
                        probe, wa, wb);
        } else {
            fprintf(stderr,
                    "selftest: emoji font unavailable, skipping\n");
        }
    }

    /* video: real decoder through the synthetic clip (preview only) */
    if (auth_case && a->vid_available && a->npins > 0) {
        const char *clip = getenv("PC_SELFTEST_VIDEO");
        static char clippath[512];
        if (!clip || !*clip) {
            snprintf(clippath, sizeof clippath,
                     "/tmp/pinfall_selftest_clip.mp4");
            FILE *cf = fopen(clippath, "rb");
            if (!cf) {
                char cmd[900];
                snprintf(cmd, sizeof cmd,
                         "ffmpeg -y -f lavfi -i "
                         "testsrc=size=320x568:rate=24:duration=3 -pix_fmt "
                         "yuv420p -c:v libx264 -preset ultrafast -loglevel "
                         "error '%s' >/dev/null 2>&1",
                         clippath);
#ifdef PC_ANDROID
                /* no ffmpeg / no desktop selftest sample generation */
                (void)cmd;
#else
                int frc = system(cmd);
                (void)frc;
#endif
            } else {
                fclose(cf);
            }
            clip = clippath;
        }
        if (clip && *clip) {
            bool saved_is = a->pins[0].pin.is_video;
            int saved_dur = a->pins[0].pin.video_duration_ms;
            int saved_vw = a->pins[0].pin.video_w;
            int saved_vh = a->pins[0].pin.video_h;
            a->pins[0].pin.is_video = true;
            a->pins[0].pin.video_duration_ms = 10000;
            a->pins[0].pin.video_w = 720;
            a->pins[0].pin.video_h = 1280;
            snprintf(a->video_file_override,
                     sizeof a->video_file_override, "%s", clip);
            app_open_detail(a, 0);
            st_pump(a, 4);
            Uint32 vdl = SDL_GetTicks() + 10000;
            while (a->video_frames < 3 && SDL_GetTicks() < vdl) {
                st_pump(a, 1);
                SDL_Delay(2);
            }
            bool badge = a->pins[0].pin.is_video;
            uint64_t f0 = a->video_frames;
            ok_video = badge && a->video_active &&
                       a->video_tex != NULL && !a->video_failed && f0 >= 3;
            if (!ok_video)
                fprintf(stderr,
                        "selftest: video FAILED (active=%d frames=%llu "
                        "tex=%p failed=%d avail=%d)\n",
                        a->video_active ? 1 : 0,
                        (unsigned long long)f0, (void *)a->video_tex,
                        a->video_failed ? 1 : 0, a->vid_available ? 1 : 0);
            SDL_FRect vb = cu_screen(a, a->d_video_btn);
            st_push_click(a, vb.x + vb.w * 0.5f, vb.y + vb.h * 0.5f);
            st_pump(a, 6);
            bool paused = a->video_paused;
            uint64_t f1 = a->video_frames;
            SDL_Delay(250);
            st_pump(a, 10);
            uint64_t f2 = a->video_frames;
            ok_video = ok_video && paused && f2 <= f1 + 1;
            st_push_click(a, vb.x + vb.w * 0.5f, vb.y + vb.h * 0.5f);
            st_pump(a, 6);
            SDL_Delay(150);
            st_pump(a, 10);
            uint64_t f3 = a->video_frames;
            ok_video = ok_video && !a->video_paused && f3 > f2;
            if (!ok_video)
                fprintf(stderr,
                        "selftest: video FAILED (paused=%d f1=%llu f2=%llu "
                        "f3=%llu)\n",
                        paused ? 1 : 0, (unsigned long long)f1,
                        (unsigned long long)f2, (unsigned long long)f3);
            st_push_key(a, SDLK_ESCAPE);
            st_pump(a, 12);
            ok_video = ok_video && !a->show_detail && !a->video_active &&
                       a->video_tex == NULL;
            a->pins[0].pin.is_video = saved_is;
            a->pins[0].pin.video_duration_ms = saved_dur;
            a->pins[0].pin.video_w = saved_vw;
            a->pins[0].pin.video_h = saved_vh;
            a->video_file_override[0] = '\0';
        } else {
            ok_video = false;
        }
    } else if (!a->vid_available) {
        fprintf(stderr, "selftest: video decoder unavailable, skipping\n");
    }

    printf("selftest: hover=%s picker-create=%s about=%s video=%s "
           "emoji=%s\n",
           (ok_hover && ok_hover_click) ? "PASS" : "FAIL",
           ok_create ? "PASS" : "FAIL", ok_about ? "PASS" : "FAIL",
           ok_video ? "PASS" : "FAIL", ok_emoji ? "PASS" : "FAIL");
    printf("selftest: close-up actions=%s fullscreen=%s image=%s "
           "scrolls=%s comments=%s related=%s like=%s composer=%s "
           "empty=%s guest=%s\n",
           ok_actions ? "PASS" : "FAIL", ok_fullscreen ? "PASS" : "FAIL",
           ok_image_full ? "PASS" : "FAIL", ok_scrolls ? "PASS" : "FAIL",
           ok_comments ? "PASS" : "FAIL", ok_related ? "PASS" : "FAIL",
           ok_like ? "PASS" : "FAIL", ok_composer ? "PASS" : "FAIL",
           ok_empty ? "PASS" : "FAIL", ok_guest_ok ? "PASS" : "FAIL");
    printf("selftest: bmps %s %s %s %s %s %s\n", path_scroll, path_search,
           path_detail, path_saved, path_picker, path_stats);

    /* 5. real-session paging (opt-in via PC_SELFTEST_PAGING=1): the Saved tab
     * must page past the old 3-page / 75-pin cap (target via
     * PC_SELFTEST_PAGING_TARGET, default 100). */
    bool ok_paging = true;
    bool paging_case = (a->auth == PC_AUTH_LOGGED_IN && !a->auth_preview &&
                        getenv("PC_SELFTEST_PAGING") != NULL);
    if (paging_case) {
        char path_paging[700];
        int paging_target = 100;
        const char *pt_env = getenv("PC_SELFTEST_PAGING_TARGET");
        if (pt_env && atoi(pt_env) > 0)
            paging_target = atoi(pt_env);

        snprintf(path_paging, sizeof path_paging, "%s_paging.bmp", prefix);
        ok_paging = false;
        if (a->saved_btn.w > 0) {
            st_push_click(a,
                          (float)a->saved_btn.x + (float)a->saved_btn.w * .5f,
                          (float)a->saved_btn.y + (float)a->saved_btn.h * .5f);
            st_pump(a, 6);
            if (st_wait(a, st_saved_ready, 25000)) {
                size_t start = a->npins;
                Uint32 dl_all = SDL_GetTicks() + 180000;
                bool target_hit = ((int)a->npins >= paging_target);
                bool exhausted = (a->bookmark[0] == '\0' && a->npins > 0);
                int no_growth_streak = 0;

                while (!target_hit && !exhausted &&
                       SDL_GetTicks() < dl_all) {
                    size_t before = a->npins;
                    Uint32 dl = SDL_GetTicks() + 30000;
                    /* Drive BOTH scroll and scroll_target to the bottom: the
                     * smooth-scroll animation pulls scroll back toward the
                     * target, so setting only `scroll` keeps it below the
                     * load-more threshold (260 px) and paging never fires. */
                    a->scroll = max_scroll(a);
                    a->scroll_target = a->scroll;
                    while (a->npins == before && !exhausted &&
                           SDL_GetTicks() < dl) {
                        st_pump(a, 2);
                        SDL_Delay(2);
                        a->scroll = max_scroll(a);
                        a->scroll_target = a->scroll;
                        if (a->bookmark[0] == '\0' && a->npins > 0)
                            exhausted = true;
                    }
                    if (a->npins == before) {
                        no_growth_streak++;
                        printf("selftest: paging stalled at %zu pins "
                               "(attempt %d, target %d, bm=%zu, pages=%d, "
                               "busy=%d, err=\"%s\")%s\n",
                               a->npins, no_growth_streak, paging_target,
                               strlen(a->bookmark), a->pages,
                               a->loading_more ? 1 : 0, a->error,
                               a->bookmark[0] ? "" : " [end of feed]");
                        fflush(stdout);
                        if (a->bookmark[0] == '\0') {
                            exhausted = true;
                        } else {
                            /* slow or throttled page: keep trying until the
                             * overall deadline instead of failing early */
                            st_pump(a, 20);
                            SDL_Delay(200);
                            continue;
                        }
                    } else {
                        no_growth_streak = 0;
                        printf("selftest: paging page -> %zu pins "
                               "(target %d)\n", a->npins, paging_target);
                        fflush(stdout);
                    }
                    if (a->bookmark[0] == '\0' && a->npins > 0)
                        exhausted = true;
                    target_hit = ((int)a->npins >= paging_target);
                }
                if (a->bookmark[0] == '\0' && a->npins > 0)
                    exhausted = true;
                ok_paging = target_hit || exhausted;
                rnd_save_bmp(a->rnd.r, path_paging);
                printf("selftest: paging saved=%zu pins (start %zu target %d) "
                       "-> %s (%s)\n",
                       a->npins, start, paging_target,
                       ok_paging ? "PASS" : "FAIL",
                       target_hit ? "target reached"
                                  : (exhausted ? "end of feed"
                                               : "timeout"));
            }
        }
        if (!ok_paging)
            fprintf(stderr, "selftest: real-session paging FAILED\n");
    }

    /* 6. search pagination regression (opt-in via PC_SELFTEST_SEARCH_PAGING=1):
     * the user bug "when I search something and scroll down I can't go far".
     * Types "anime", Enter, then pages the SEARCH feed with both scroll and
     * scroll_target at max_scroll(). PASS if target reached or feed ended. */
    bool ok_search_paging = true;
    bool search_paging_case = (getenv("PC_SELFTEST_SEARCH_PAGING") != NULL);
    if (search_paging_case) {
        int search_target = 150;
        const char *st_env = getenv("PC_SELFTEST_SEARCH_TARGET");
        if (st_env && atoi(st_env) > 0)
            search_target = atoi(st_env);
        ok_search_paging = false;

        bool cleared = false;
        if (a->search_clear.w > 0) {
            st_push_click(a, (float)a->search_clear.x +
                                 (float)a->search_clear.w * 0.5f,
                          (float)a->search_clear.y +
                                 (float)a->search_clear.h * 0.5f);
            st_pump(a, 6);
            cleared = (a->input[0] == '\0');
        }
        if (!cleared) {
            SDL_FRect sr = search_rect(a);
            st_push_click(a, sr.x + sr.w * 0.5f, sr.y + sr.h * 0.5f);
            st_pump(a, 3);
            for (int i = 0; i < a->input_len + 2; i++) {
                st_push_key(a, SDLK_BACKSPACE);
                st_pump(a, 1);
            }
        }
        st_push_text("anime");
        st_pump(a, 4);
        st_push_key(a, SDLK_RETURN);
        bool anime_ready = st_wait(a, st_anime_ready, 30000);
        if (anime_ready) {
            size_t start = a->npins;
            Uint32 dl_all = SDL_GetTicks() + 180000;
            bool target_hit = ((int)a->npins >= search_target);
            bool exhausted = (a->bookmark[0] == '\0' && a->npins > 0);
            int streak = 0;
            while (!target_hit && !exhausted &&
                   SDL_GetTicks() < dl_all) {
                size_t before = a->npins;
                Uint32 dl = SDL_GetTicks() + 30000;
                /* BOTH scroll and scroll_target: the smooth-scroll animation
                 * otherwise pulls scroll below the load-more trigger. */
                a->scroll = max_scroll(a);
                a->scroll_target = a->scroll;
                while (a->npins == before && !exhausted &&
                       SDL_GetTicks() < dl) {
                    st_pump(a, 2);
                    SDL_Delay(2);
                    a->scroll = max_scroll(a);
                    a->scroll_target = a->scroll;
                    if (a->bookmark[0] == '\0' && a->npins > 0)
                        exhausted = true;
                }
                if (a->npins == before) {
                    streak++;
                    printf("selftest: search paging stalled at %zu pins "
                           "(attempt %d, target %d, bm=%zu, pages=%d, "
                           "busy=%d, err=\"%s\")%s\n",
                           a->npins, streak, search_target,
                           strlen(a->bookmark), a->pages,
                           a->loading_more ? 1 : 0, a->error,
                           a->bookmark[0] ? "" : " [end of feed]");
                    fflush(stdout);
                    if (a->bookmark[0] == '\0') {
                        exhausted = true;
                    } else {
                        st_pump(a, 20);
                        SDL_Delay(200);
                        continue;
                    }
                } else {
                    streak = 0;
                    printf("selftest: search paging page -> %zu pins "
                           "(target %d)\n", a->npins, search_target);
                    fflush(stdout);
                }
                if (a->bookmark[0] == '\0' && a->npins > 0)
                    exhausted = true;
                target_hit = ((int)a->npins >= search_target);
            }
            if (a->bookmark[0] == '\0' && a->npins > 0)
                exhausted = true;
            ok_search_paging = target_hit || exhausted;
            rnd_save_bmp(a->rnd.r, path_search_paging);
            printf("selftest: search paging saved=%zu pins (start %zu target "
                   "%d) -> %s (%s)\n", a->npins, start, search_target,
                   ok_search_paging ? "PASS" : "FAIL",
                   target_hit ? "target reached"
                              : (exhausted ? "end of feed" : "timeout"));
        } else {
            fprintf(stderr,
                    "selftest: search paging: \"anime\" feed did not load\n");
        }
        if (!ok_search_paging)
            fprintf(stderr, "selftest: search paging FAILED\n");
    }

    if (!eviction_done && a->npins >= 60 && a->state == VIEW_FEED) {
        eviction_done = st_eviction_case(a, prefix, &ev);
        ok_evict = ev.ok;
    }

    /* settings overlay BMP (when --open-settings is passed with --selftest) */
    if (a->snap_open_settings) {
        char path_settings[700];
        snprintf(path_settings, sizeof path_settings, "%s_settings.bmp",
                 prefix);
        app_open_settings(a);
        st_pump(a, 12);
        rnd_save_bmp(a->rnd.r, path_settings);
        app_close_settings(a);
        printf("selftest: settings bmp %s\n", path_settings);
    }

    bool all = ok_scroll && ok_home && ok_search && ok_detail && ok_close &&
               ok_auth && ok_saved && ok_picker && ok_save && ok_picker_esc &&
               ok_evict && ok_settings && ok_paging && ok_search_paging &&
               ok_actions && ok_fullscreen && ok_image_full && ok_scrolls &&
               ok_comments && ok_related && ok_like && ok_composer &&
               ok_empty && ok_guest_ok && ok_hover && ok_hover_click &&
               ok_create && ok_about && ok_video && ok_emoji &&
               saved_scroll && saved_search && saved_detail;
    printf("selftest: %s\n", all ? "PASS" : "FAIL");
    return all ? 0 : 1;
}

/* ------------------------------------------------------------------ main */
static void print_usage(const char *argv0)
{
    printf("%s - an alternative Pinterest client (C11 + SDL2)\n\n", APP_NAME);
    printf("usage: %s [options]\n\n", argv0);
    printf("  --query <text>      initial search query "
           "(default: \"%s\")\n", DEFAULT_QUERY);
    printf("  --snapshot <path>   headless-ish verification mode: load the feed,\n"
           "                      wait for >= %d images (max --timeout ms), then\n"
           "                      write a BMP screenshot to <path> and exit\n",
           SNAP_MIN_IMAGES);
    printf("  --timeout <ms>      snapshot wait budget (default %d)\n",
           SNAP_DEFAULT_TIMEOUT_MS);
    printf("  --size <WxH>        window size (default %dx%d, resizable)\n",
           DEFAULT_WIN_W, DEFAULT_WIN_H);
    printf("  --scroll <px>       with --snapshot: scroll before capture "
           "(verifies lower rows)\n");
    printf("  --detail <index>    with --snapshot: open detail overlay for "
           "pin <index>\n");
    printf("  --cookie-file <p>   import a Netscape cookies.txt session "
           "before bootstrap\n"
           "                      (failure prints an error and continues as "
           "guest)\n");
    printf("  --save-session      after a successful login check, persist the "
           "cookie jar\n"
           "                      to $XDG_CONFIG_HOME/pinfall/session.txt "
           "(mode 0600)\n");
    printf("  --auth-preview      rendering-only signed-in preview (username "
           "\"preview\");\n"
           "                      never combined with --save-session\n");
    printf("  --no-writes         hide/disable all save affordances "
           "(read-only mode)\n");
    printf("  --tab <home|saved>  with --snapshot: capture that feed tab\n");
    printf("  --open-picker       with --snapshot: open the board-picker "
           "modal\n");
    printf("  --open-settings     with --snapshot: open the Settings panel\n");
    printf("  --img-cache <m>     override image cache: off | jpeg | lz4\n");
    printf("  --img-budget-mb <N> override RAM cache budget: "
           "16 | 32 | 64 | 128\n");
    printf("  --keep-screens <N>  override texture keep band: 1 | 2 | 3 | 5\n");
    printf("  --stats             show the F3 stats overlay and print one\n"
           "                      machine-readable pinfall-stats line at "
           "exit\n");
    printf("  --block-ads <0|1>   block promoted/sponsored pins (runtime "
           "override;\n"
           "                      default on, persisted in settings.conf)\n");
    printf("  --emoji-test [bmp]  render a color-emoji sample sheet to BMP\n");
    printf("  --video-file <p>    play this local clip in the close-up "
           "(test hook)\n");
    printf("  --video-frame <n>   with --snapshot: capture after frame n\n");
    printf("  --video-pause       with --snapshot: pause before capture\n");
    printf("  --autoplay-video <0|1>  close-up video autoplay override\n");
    printf("  --open-about        open Settings on the About section\n");
    printf("  --hover <index>     hover a grid pin (shows the Save pill)\n");
    printf("  --create-board      open the picker with the Create-board row\n");
    printf("  --detail-tab <t>    with --detail/--snapshot: comments | related\n");
    printf("  --like              with --detail/--snapshot: toggle like first\n"
           "                      (preview mock, never a live write)\n");
    printf("  --comment-draft <s> with --detail/--snapshot: prefill the comment "
           "box\n");
    printf("  --selftest          headless interaction test: pushes synthetic\n"
           "                      wheel/key/click events, writes BMPs, exits "
           "0 on PASS\n");
    printf("  --prefix <path>     selftest BMP prefix (default "
           "/tmp/pinfall_selftest)\n");
    printf("  -h, --help          show this help\n\n");
    printf("interactive controls: type + Enter to search, mouse wheel / "
           "PgUp/PgDn / Home/End to scroll,\nclick a pin for detail, Esc to "
           "close, Ctrl+F to focus search, Ctrl+V to paste.\n");
}

int pinfall_ui_main(int argc, char **argv)
{
    const char *query = DEFAULT_QUERY;
    const char *snap_path = NULL;
    int timeout_ms = SNAP_DEFAULT_TIMEOUT_MS;
    int win_w = DEFAULT_WIN_W, win_h = DEFAULT_WIN_H;
    int snap_scroll = 0, snap_detail = -1;
    bool selftest = false;
    const char *prefix = "/tmp/pinfall_selftest";
    const char *cookie_file = NULL;
    bool save_session = false;
    bool auth_preview = false;
    bool no_writes = false;
    int snap_tab = 0;
    bool snap_open_picker = false;
    const char *img_cache_opt = NULL;
    int img_budget_opt = 0, keep_opt = 0;
    bool stats_flag = false;
    bool open_settings = false;
    int block_ads_opt = -1; /* -1 = not specified */
    int detail_tab = 0;
    bool snap_like = false;
    bool comments_empty = false;
    bool open_about = false;
    bool create_board_flag = false;
    int hover_opt = -1;
    const char *video_file = NULL;
    int video_frame = 0;
    bool video_pause = false;
    int autoplay_opt = -1;
    bool emoji_test = false;
    const char *emoji_path = NULL;
    const char *comment_draft = NULL;
    bool query_set = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snap_path = argv[++i];
        } else if (strcmp(argv[i], "--query") == 0 && i + 1 < argc) {
            query = argv[++i];
            query_set = true;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
            if (timeout_ms < 1000)
                timeout_ms = 1000;
            if (timeout_ms > 120000)
                timeout_ms = 120000;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else if (strcmp(argv[i], "--cookie-file") == 0 && i + 1 < argc) {
            cookie_file = argv[++i];
        } else if (strcmp(argv[i], "--save-session") == 0) {
            save_session = true;
        } else if (strcmp(argv[i], "--auth-preview") == 0) {
            auth_preview = true;
        } else if (strcmp(argv[i], "--no-writes") == 0) {
            no_writes = true;
        } else if (strcmp(argv[i], "--img-cache") == 0 && i + 1 < argc) {
            img_cache_opt = argv[++i];
        } else if (strcmp(argv[i], "--img-budget-mb") == 0 && i + 1 < argc) {
            img_budget_opt = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--keep-screens") == 0 && i + 1 < argc) {
            keep_opt = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stats") == 0) {
            stats_flag = true;
        } else if (strcmp(argv[i], "--open-settings") == 0) {
            open_settings = true;
        } else if (strcmp(argv[i], "--detail-tab") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "comments") == 0)
                detail_tab = 1;
            else if (strcmp(v, "related") == 0)
                detail_tab = 2;
            else
                fprintf(stderr,
                        "pinfall: --detail-tab expects comments|related\n");
        } else if (strcmp(argv[i], "--like") == 0) {
            snap_like = true;
        } else if (strcmp(argv[i], "--comments-empty") == 0) {
            comments_empty = true;
        } else if (strcmp(argv[i], "--open-about") == 0) {
            open_about = true;
        } else if (strcmp(argv[i], "--create-board") == 0) {
            create_board_flag = true;
        } else if (strcmp(argv[i], "--hover") == 0 && i + 1 < argc) {
            hover_opt = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--emoji-test") == 0) {
            emoji_test = true;
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0)
                emoji_path = argv[++i];
        } else if (strcmp(argv[i], "--video-file") == 0 && i + 1 < argc) {
            video_file = argv[++i];
        } else if (strcmp(argv[i], "--video-frame") == 0 && i + 1 < argc) {
            video_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--video-pause") == 0) {
            video_pause = true;
        } else if (strcmp(argv[i], "--autoplay-video") == 0 &&
                   i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v == 0 || v == 1)
                autoplay_opt = v;
        } else if (strcmp(argv[i], "--comment-draft") == 0 && i + 1 < argc) {
            comment_draft = argv[++i];
        } else if (strcmp(argv[i], "--block-ads") == 0 && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v == 0 || v == 1)
                block_ads_opt = v;
            else
                fprintf(stderr,
                        "pinfall: --block-ads expects 0 or 1\n");
        } else if (strcmp(argv[i], "--open-picker") == 0) {
            snap_open_picker = true;
        } else if (strcmp(argv[i], "--tab") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "home") == 0) {
                snap_tab = 1;
            } else if (strcmp(v, "saved") == 0) {
                snap_tab = 2;
            } else {
                fprintf(stderr,
                        "pinfall: unknown --tab '%s' (expected home|saved)\n",
                        v);
            }
        } else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--scroll") == 0 && i + 1 < argc) {
            snap_scroll = atoi(argv[++i]);
            if (snap_scroll < 0)
                snap_scroll = 0;
        } else if (strcmp(argv[i], "--detail") == 0 && i + 1 < argc) {
            snap_detail = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 640 &&
                h >= 480) {
                win_w = w;
                win_h = h;
            } else {
                fprintf(stderr, "pinfall: bad --size (expected WxH)\n");
            }
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "pinfall: ignoring unknown argument '%s' "
                            "(see --help)\n", argv[i]);
        }
    }

    if (snap_path && !*snap_path) {
        fprintf(stderr, "pinfall: --snapshot needs a path\n");
        return 1;
    }
    if (auth_preview && save_session) {
        fprintf(stderr, "pinfall: --auth-preview is rendering-only and "
                        "cannot be combined with --save-session\n");
        return 2;
    }
    if (!snap_path)
        snap_path = NULL;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");

#ifdef PC_ANDROID
    pf_android_log("ui: creating fullscreen window %dx%d", win_w, win_h);
    Uint32 flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window *win = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, win_w, win_h,
                                       flags);
    if (!win) {
        pf_android_log("ui: SDL_CreateWindow FAILED: %s", SDL_GetError());
        fprintf(stderr, "pinfall: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        return 1;
    }
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        pf_android_log("ui: window ok %dx%d", ww, wh);
    }
#else
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window *win = SDL_CreateWindow(APP_NAME, SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, win_w, win_h,
                                       flags);
    if (!win) {
        fprintf(stderr, "pinfall: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 640, 480);
#endif

    App *app = (App *)calloc(1, sizeof *app);
    if (!app) {
        SDL_DestroyWindow(win);
        return 1;
    }

    int rc = 1;
    bool inited = app_init(app, win, snap_path != NULL, snap_path, timeout_ms,
                           query, cookie_file, save_session, auth_preview,
                           query_set);
    pf_android_log("ui: app_init -> %d", inited ? 1 : 0);
    if (inited) {
        /* CLI overrides: runtime only, never persisted to settings.conf */
        if (img_cache_opt) {
            PcImgCacheMode m;
            if (settings_parse_cache_mode(img_cache_opt, &m))
                app->settings.cache_mode = m;
            else
                fprintf(stderr,
                        "pinfall: invalid --img-cache '%s' (off|jpeg|lz4)\n",
                        img_cache_opt);
        }
        if (img_budget_opt > 0) {
            if (img_budget_opt == 16 || img_budget_opt == 32 ||
                img_budget_opt == 64 || img_budget_opt == 128)
                app->settings.budget_mb = img_budget_opt;
            else
                fprintf(stderr, "pinfall: --img-budget-mb must be "
                                "16, 32, 64 or 128\n");
        }
        if (keep_opt > 0) {
            if (keep_opt == 1 || keep_opt == 2 || keep_opt == 3 ||
                keep_opt == 5)
                app->settings.keep_screens = keep_opt;
            else
                fprintf(stderr, "pinfall: --keep-screens must be "
                                "1, 2, 3 or 5\n");
        }
        if (stats_flag) {
            app->settings.show_stats = true;
            app->stats_arg = true;
        }
        if (block_ads_opt >= 0)
            app->settings.block_ads = (block_ads_opt != 0);
        app->snap_detail_tab = detail_tab;
        app->snap_like = snap_like;
        app->snap_empty_comments = comments_empty;
        app->snap_open_about = open_about;
        app->snap_create_board = create_board_flag;
        app->snap_hover = hover_opt;
        if (video_file)
            snprintf(app->video_file_override,
                     sizeof app->video_file_override, "%s", video_file);
        app->snap_video_frame = video_frame;
        app->snap_video_pause = video_pause;
        if (autoplay_opt >= 0)
            app->settings.autoplay_video = (autoplay_opt != 0);
        if (comment_draft)
            snprintf(app->snap_comment_draft,
                     sizeof app->snap_comment_draft, "%s", comment_draft);
        app->snap_open_settings = open_settings;
        app_apply_settings(app);
    }

    if (!inited) {
        rc = 1;
    } else if (emoji_test) {
        rc = app_run_emoji_test(
            app, emoji_path && *emoji_path ? emoji_path
                                           : "/tmp/pinfall_emoji_test.bmp");
    } else if (selftest) {
        app->no_writes = no_writes;
        app->snap_tab = snap_tab;
        app->snap_open_picker = snap_open_picker;
        if (no_writes)
            fprintf(stderr, "pinfall: --no-writes enabled: save buttons "
                            "are hidden\n");
        rc = app_run_selftest(app, prefix);
    } else if (snap_path) {
        app->snap_scroll = (float)snap_scroll;
        app->snap_detail = snap_detail;
        app->snap_tab = snap_tab;
        app->snap_open_picker = snap_open_picker;
        app->no_writes = no_writes;
        if (no_writes)
            fprintf(stderr, "pinfall: --no-writes enabled: save buttons "
                            "are hidden\n");
        rc = app_run_snapshot(app);
    } else {
        app->no_writes = no_writes;
        if (no_writes)
            fprintf(stderr, "pinfall: --no-writes enabled: save buttons "
                            "are hidden\n");
        pf_android_log("ui: entering interactive loop");
        rc = app_run_interactive(app);
        pf_android_log("ui: interactive loop returned %d", rc);
    }

    if (app->stats_arg)
        app_print_stats_line(app);

    app_shutdown(app);
    free(app);
    SDL_DestroyWindow(win);
    return rc;
}
