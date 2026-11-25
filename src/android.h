/* android.h - Android platform layer (pure C, SDL2 Android port).
 *
 * Everything here is compiled to no-ops on desktop builds; PC_ANDROID is
 * defined by android/app/jni/CMakeLists.txt. The UI keeps using logical
 * coordinates: the window is mapped through SDL_RenderSetScale so the logical
 * width is win_w/ui_scale (ui_scale = max(1, round(win_w/432, 0.5))). */
#ifndef PINFALL_ANDROID_H
#define PINFALL_ANDROID_H

#include <stdbool.h>
#include <stddef.h>

#include <SDL2/SDL.h>

#ifdef PC_ANDROID
#include <stdarg.h>
void pf_android_log(const char *fmt, ...);

#define PF_ANDROID_FONT_REG "fonts/DejaVuSans.ttf"
#define PF_ANDROID_FONT_BOLD "fonts/DejaVuSans-Bold.ttf"
#define PF_ANDROID_FONT_EMOJI "fonts/NotoColorEmoji.ttf"

/* Idempotent platform init/shutdown (currently a no-op shell). */
bool pf_android_init(void);
void pf_android_shutdown(void);

/* Display insets (physical px): status bar / camera cutout at the top,
 * gesture/nav bar at the bottom. Read via JNI from WindowInsets. */
int pf_android_top_inset_px(void);
int pf_android_bottom_inset_px(void);
/* Called by render.c after reading the insets so touch/IME mapping can
 * subtract the same padding. */
void pf_android_set_pad(int top_px, int bottom_px);

/* Logical scaling: native Android uses display density (dp baseline);
 * the 432px mobile-web baseline remains as a fallback when JNI is absent. */
float pf_android_ui_scale_for_width(int win_w);
float pf_android_density(void);
float pf_android_effective_ui_scale(int win_w);
/* 1 when the Android system is in night/dark mode. */
int pf_android_night_mode(void);
void pf_android_apply_scale(SDL_Renderer *ren, float ui_scale);

/* Soft-keyboard helpers: logical rect -> window rect; state tracked by caller. */
SDL_Rect pf_android_ime_rect(SDL_FRect logical, float ui_scale);
void pf_android_update_ime(SDL_Window *win, bool want, SDL_FRect logical,
                           bool *state);

/* Paths (static buffers owned by this module). */
const char *pf_android_internal_path(void);
const char *pf_android_cookie_path(void);   /* external files/cookies.txt */
const char *pf_android_cookie_load_path(void); /* first existing external/internal */
const char *pf_android_settings_path(void); /* internal .../pinfall/settings.conf */
bool pf_android_cookie_exists(void);
bool pf_android_reload_cookies(void *client); /* pc_cookie_load wrapper */

/* Touch gesture translator: window-pixel SDL finger events in, logical out. */
typedef struct {
    bool   active;
    bool   dragging;
    bool   longpress_done;
    float  start_wx, start_wy;
    float  last_wx, last_wy;
    Uint32 start_ms;
} PfAndroidTouch;

typedef enum {
    PF_TOUCH_NONE = 0,
    PF_TOUCH_TAP,      /* release without drag/long-press -> synthesize click */
    PF_TOUCH_DRAG,     /* vertical drag delta in *dy (logical px) */
    PF_TOUCH_LONGPRESS/* fire Save picker for the card under the finger */
} PfAndroidTouchAction;

void pf_android_touch_reset(PfAndroidTouch *t);
PfAndroidTouchAction pf_android_touch_handle(PfAndroidTouch *t,
                                             const SDL_Event *e, float win_w,
                                             float win_h, float ui_scale,
                                             float *lx, float *ly, float *dy,
                                             bool *moved);
/* Returns true once when a still finger has been held >450ms. */
bool pf_android_touch_longpress_tick(PfAndroidTouch *t, Uint32 now,
                                     float ui_scale, float *lx, float *ly);

#else /* !PC_ANDROID */
#define pf_android_log(...) ((void)0)
#endif /* PC_ANDROID */
#endif /* PINFALL_ANDROID_H */
