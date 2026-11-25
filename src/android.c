/* android.c - pure-C platform layer for the SDL2 Android port.
 *
 * Desktop builds compile this file to almost nothing (PC_ANDROID is defined by
 * android/app/jni/CMakeLists.txt). Android specifics:
 *  - logical scaling (SDL_RenderSetScale) with a 432px logical width baseline
 *  - soft keyboard (IME) helpers
 *  - app-private / external storage paths for cookies + settings
 *  - finger gesture translator (tap / vertical drag / long-press)
 */
#include "android.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef PC_ANDROID
#include <SDL2/SDL_system.h>
#include <android/log.h>
#include <jni.h>

#include "pc.h"

void pf_android_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "PinFall", fmt, ap);
    va_end(ap);
}

/* ---- display insets (status bar / camera cutout / gesture bar) ---------- */
static int g_pad_top, g_pad_bottom;

static jobject jcall_obj(JNIEnv *env, jobject obj, const char *name,
                         const char *sig)
{
    jclass c;
    jmethodID m;
    jobject out = NULL;

    if (!env || !obj)
        return NULL;
    c = (*env)->GetObjectClass(env, obj);
    if (!c)
        return NULL;
    m = (*env)->GetMethodID(env, c, name, sig);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        m = NULL;
    }
    if (m)
        out = (*env)->CallObjectMethod(env, obj, m);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        out = NULL;
    }
    (*env)->DeleteLocalRef(env, c);
    return out;
}

static int jcall_int(JNIEnv *env, jobject obj, const char *name,
                     const char *sig)
{
    jclass c;
    jmethodID m;
    int out = 0;

    if (!env || !obj)
        return 0;
    c = (*env)->GetObjectClass(env, obj);
    if (!c)
        return 0;
    m = (*env)->GetMethodID(env, c, name, sig);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        m = NULL;
    }
    if (m)
        out = (*env)->CallIntMethod(env, obj, m);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        out = 0;
    }
    (*env)->DeleteLocalRef(env, c);
    return out;
}

static void read_insets(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    jobject win, decor, insets;
    int top = 0, bottom = 0;

    if (!env || !act)
        return;
    win = jcall_obj(env, act, "getWindow", "()Landroid/view/Window;");
    decor = win ? jcall_obj(env, win, "getDecorView",
                            "()Landroid/view/View;")
                : NULL;
    insets = decor ? jcall_obj(env, decor, "getRootWindowInsets",
                               "()Landroid/view/WindowInsets;")
                   : NULL;
    if (insets) {
        top = jcall_int(env, insets, "getSystemWindowInsetTop", "()I");
        bottom = jcall_int(env, insets, "getSystemWindowInsetBottom", "()I");
        {
            jobject cut = jcall_obj(env, insets, "getDisplayCutout",
                                    "()Landroid/view/DisplayCutout;");

            if (cut) {
                int safe = jcall_int(env, cut, "getSafeInsetTop", "()I");

                if (safe > top)
                    top = safe;
                safe = jcall_int(env, cut, "getSafeInsetBottom", "()I");
                if (safe > bottom)
                    bottom = safe;
                (*env)->DeleteLocalRef(env, cut);
            }
        }
        (*env)->DeleteLocalRef(env, insets);
    }
    if (decor)
        (*env)->DeleteLocalRef(env, decor);
    if (win)
        (*env)->DeleteLocalRef(env, win);
    (*env)->DeleteLocalRef(env, act);
    if (top < 0)
        top = 0;
    if (bottom < 0)
        bottom = 0;
    g_pad_top = top;
    g_pad_bottom = bottom;
}

int pf_android_top_inset_px(void)
{
    read_insets();
    return g_pad_top;
}

int pf_android_bottom_inset_px(void)
{
    read_insets();
    return g_pad_bottom;
}

void pf_android_set_pad(int top_px, int bottom_px)
{
    g_pad_top = top_px > 0 ? top_px : 0;
    g_pad_bottom = bottom_px > 0 ? bottom_px : 0;
}

static void ensure_dir(const char *path)
{
    if (path && *path)
        (void)mkdir(path, 0700);
}


/* mbedtls 3.6.2's x509_crt_parse_path hits an Android fdsan double-close while
 * walking /system/etc/security/cacerts, which aborts the app on the first TLS
 * image fetch. Build a single PEM bundle once and point libcurl at it via
 * CURL_CA_BUNDLE, so only mbedtls_x509_crt_parse_file is used. */
static void pf_android_build_ca_bundle(void)
{
    static const char *dirs[] = {"/system/etc/security/cacerts",
                                 "/apex/com.android.conscrypt/cacerts"};
    char out[700];
    long written = 0;
    size_t total = 0;

    snprintf(out, sizeof out, "%s/pinfall/ca-bundle.pem",
             pf_android_internal_path());
    for (size_t k = 0; k < sizeof dirs / sizeof dirs[0]; k++) {
        DIR *d = opendir(dirs[k]);
        struct dirent *de;
        if (!d)
            continue;
        FILE *o = fopen(out, written ? "ab" : "wb");
        if (!o) {
            closedir(d);
            break;
        }
        while ((de = readdir(d)) != NULL) {
            char path[900];
            char buf[8192];
            size_t n;
            FILE *i;

            if (de->d_name[0] == '.')
                continue;
            snprintf(path, sizeof path, "%s/%s", dirs[k], de->d_name);
            i = fopen(path, "rb");
            if (!i)
                continue;
            while ((n = fread(buf, 1, sizeof buf, i)) > 0) {
                fwrite(buf, 1, n, o);
                total += n;
            }
            fputc('\n', o);
            fclose(i);
            written++;
        }
        fclose(o);
        closedir(d);
    }
    if (written > 0 && total > 0)
        setenv("CURL_CA_BUNDLE", out, 1);
}
bool pf_android_init(void)
{
    {
        /* bionic fdsan aborts when libcurl closes an fd still owned by a
         * FILE* (seen in pc_image_fetch_bytes). Downgrade it to a warning
         * until the net layer is fixed; UI verification must not die. */
        typedef int (*fdsan_level_fn)(int);
        void *sym = dlsym(RTLD_DEFAULT, "android_fdsan_set_error_level");
        if (sym) {
            fdsan_level_fn set_level = (fdsan_level_fn)sym;
            (void)set_level(1); /* WARN_ONCE */
        }
    }

    (void)SDL_AndroidGetJNIEnv();
    ensure_dir(pf_android_internal_path());
    char dir[700];
    snprintf(dir, sizeof dir, "%s/pinfall", pf_android_internal_path());
    ensure_dir(dir);
    pf_android_build_ca_bundle();
    return true;
}

void pf_android_shutdown(void)
{
}

const char *pf_android_internal_path(void)
{
    static char path[700];
    const char *p = SDL_AndroidGetInternalStoragePath();
    if (!p || !*p)
        p = ".";
    snprintf(path, sizeof path, "%s", p);
    return path;
}

const char *pf_android_cookie_path(void)
{
    static char path[700];
    const char *ext = SDL_AndroidGetExternalStoragePath();
    if (ext && *ext)
        snprintf(path, sizeof path, "%s/cookies.txt", ext);
    else
        snprintf(path, sizeof path, "%s/cookies.txt", pf_android_internal_path());
    return path;
}

bool pf_android_cookie_exists(void)
{
    FILE *f = fopen(pf_android_cookie_path(), "rb");
    if (f) {
        fclose(f);
        return true;
    }
    char alt[700];
    snprintf(alt, sizeof alt, "%s/cookies.txt", pf_android_internal_path());
    f = fopen(alt, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

const char *pf_android_cookie_load_path(void)
{
    if (pf_android_cookie_exists()) {
        FILE *f = fopen(pf_android_cookie_path(), "rb");
        if (f) {
            fclose(f);
            return pf_android_cookie_path();
        }
    }
    static char alt[700];
    snprintf(alt, sizeof alt, "%s/cookies.txt", pf_android_internal_path());
    return alt;
}

const char *pf_android_settings_path(void)
{
    static char path[700];
    snprintf(path, sizeof path, "%s/pinfall/settings.conf",
             pf_android_internal_path());
    return path;
}

bool pf_android_reload_cookies(void *client)
{
    if (!client)
        return false;
    if (!pf_android_cookie_exists()) {
        /* fall back to internal storage if the external file vanished */
        char alt[700];
        snprintf(alt, sizeof alt, "%s/cookies.txt",
                 pf_android_internal_path());
        FILE *f = fopen(alt, "rb");
        if (!f)
            return false;
        fclose(f);
        char *err = NULL;
        bool ok = pc_cookie_load((PcClient *)client, alt, &err);
        if (err)
            pc_free(err);
        return ok;
    }
    char *err = NULL;
    bool ok = pc_cookie_load((PcClient *)client, pf_android_cookie_path(),
                             &err);
    if (err)
        pc_free(err);
    return ok;
}


/* Native Pinterest measures in dp. Derive the render scale from the Android
 * display density (densityDpi/160) so dp values map 1:1 to logical units,
 * instead of the old fixed 432px mobile-web baseline. */
static int g_density_milli;

float pf_android_density(void)
{
    JNIEnv *env;
    jobject act;
    jclass cls;
    jmethodID m;
    jobject res, dm;
    jfieldID fid;
    float density = 0.0f;

    if (g_density_milli > 0)
        return (float)g_density_milli / 1000.0f;
    env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act)
        return 0.0f;
    cls = (*env)->GetObjectClass(env, act);
    if (!cls) {
        (*env)->DeleteLocalRef(env, act);
        return 0.0f;
    }
    m = (*env)->GetMethodID(env, cls, "getResources",
                            "()Landroid/content/res/Resources;");
    res = m ? (*env)->CallObjectMethod(env, act, m) : NULL;
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (res) {
        jclass rcls = (*env)->GetObjectClass(env, res);
        jmethodID gm = rcls ? (*env)->GetMethodID(
                                  env, rcls, "getDisplayMetrics",
                                  "()Landroid/util/DisplayMetrics;")
                            : NULL;
        dm = gm ? (*env)->CallObjectMethod(env, res, gm) : NULL;
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        if (dm) {
            jclass dcls = (*env)->GetObjectClass(env, dm);
            fid = dcls ? (*env)->GetFieldID(env, dcls, "density", "F")
                       : NULL;
            if (fid)
                density = (*env)->GetFloatField(env, dm, fid);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                density = 0.0f;
            }
            if (dcls)
                (*env)->DeleteLocalRef(env, dcls);
            (*env)->DeleteLocalRef(env, dm);
        }
        if (rcls)
            (*env)->DeleteLocalRef(env, rcls);
        (*env)->DeleteLocalRef(env, res);
    }
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    if (density >= 1.0f && density <= 8.0f) {
        g_density_milli = (int)(density * 1000.0f + 0.5f);
        return density;
    }
    return 0.0f;
}


/* System night mode: Configuration.uiMode & UI_MODE_NIGHT_MASK == YES.
 * Read on startup and on resume/size changes for theme=system. */
int pf_android_night_mode(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    jobject res, cfg;
    int ui_mode = 0;

    if (!env || !act)
        return 0;
    res = jcall_obj(env, act, "getResources",
                    "()Landroid/content/res/Resources;");
    cfg = res ? jcall_obj(env, res, "getConfiguration",
                          "()Landroid/content/res/Configuration;")
              : NULL;
    if (cfg) {
        jclass c = (*env)->GetObjectClass(env, cfg);
        jfieldID fid = c ? (*env)->GetFieldID(env, c, "uiMode", "I") : NULL;
        if (fid)
            ui_mode = (int)(*env)->GetIntField(env, cfg, fid);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        if (c)
            (*env)->DeleteLocalRef(env, c);
        (*env)->DeleteLocalRef(env, cfg);
    }
    if (res)
        (*env)->DeleteLocalRef(env, res);
    (*env)->DeleteLocalRef(env, act);
    return ((ui_mode & 0x30) == 0x20) ? 1 : 0;
}

float pf_android_effective_ui_scale(int win_w)
{
    /* The native reference window is 274 dp wide (720 px @ 2.625 density).
     * A fixed dp target keeps the measured native bounds 1:1 even when the
     * device reports a different display-density override. */
    float ui;

    if (win_w < 1)
        win_w = 1;
    ui = (float)win_w / 274.0f;
    if (ui < 1.0f)
        ui = 1.0f;
    return ui;
}

float pf_android_ui_scale_for_width(int win_w)
{
    if (win_w < 1)
        win_w = 1;
    float ui = (float)win_w / 432.0f;
    if (ui < 1.0f)
        ui = 1.0f;
    /* keep the logical width stable across odd panel sizes */
    ui = floorf(ui * 2.0f + 0.5f) / 2.0f;
    if (ui < 1.0f)
        ui = 1.0f;
    return ui;
}

void pf_android_apply_scale(SDL_Renderer *ren, float ui_scale)
{
    if (ren && ui_scale > 0.0f)
        SDL_RenderSetScale(ren, ui_scale, ui_scale);
}

SDL_Rect pf_android_ime_rect(SDL_FRect logical, float ui_scale)
{
    SDL_Rect r;
    if (ui_scale <= 0.0f)
        ui_scale = 1.0f;
    r.x = (int)lroundf(logical.x * ui_scale);
    r.y = g_pad_top + (int)lroundf(logical.y * ui_scale);
    r.w = (int)lroundf(logical.w * ui_scale);
    r.h = (int)lroundf(logical.h * ui_scale);
    if (r.w < 1)
        r.w = 1;
    if (r.h < 1)
        r.h = 1;
    return r;
}

void pf_android_update_ime(SDL_Window *win, bool want, SDL_FRect logical,
                           bool *state)
{
    if (!state)
        return;
    if (want && !*state) {
        SDL_StartTextInput();
        if (win)
            SDL_SetTextInputRect(&(SDL_Rect){0, 0, 1, 1});
        *state = true;
        if (win) {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(win, &ww, &wh);
            (void)wh;
            float ui = pf_android_effective_ui_scale(ww);
            SDL_Rect r = pf_android_ime_rect(logical, ui);
            SDL_SetTextInputRect(&r);
        }
    } else if (want && *state) {
        if (win) {
            int ww = 0;
            SDL_GetWindowSize(win, &ww, NULL);
            float ui = pf_android_effective_ui_scale(ww);
            SDL_Rect r = pf_android_ime_rect(logical, ui);
            SDL_SetTextInputRect(&r);
        }
    } else if (!want && *state) {
        SDL_StopTextInput();
        *state = false;
    }
}

static void touch_begin(PfAndroidTouch *t, float wx, float wy, Uint32 now)
{
    t->active = true;
    t->dragging = false;
    t->longpress_done = false;
    t->start_wx = wx;
    t->start_wy = wy;
    t->last_wx = wx;
    t->last_wy = wy;
    t->start_ms = now;
}

void pf_android_touch_reset(PfAndroidTouch *t)
{
    if (t)
        memset(t, 0, sizeof *t);
}

PfAndroidTouchAction pf_android_touch_handle(PfAndroidTouch *t,
                                             const SDL_Event *e, float win_w,
                                             float win_h, float ui_scale,
                                             float *lx, float *ly, float *dy,
                                             bool *moved)
{
    if (moved)
        *moved = false;
    if (dy)
        *dy = 0.0f;
    if (!t || !e || ui_scale <= 0.0f)
        return PF_TOUCH_NONE;
    if (lx)
        *lx = t->last_wx / ui_scale;
    if (ly)
        *ly = (t->last_wy - (float)g_pad_top) / ui_scale;
    switch (e->type) {
    case SDL_FINGERDOWN: {
        float wx = e->tfinger.x * win_w;
        float wy = e->tfinger.y * win_h;
        touch_begin(t, wx, wy, SDL_GetTicks());
        if (lx)
            *lx = wx / ui_scale;
        if (ly)
            *ly = (wy - (float)g_pad_top) / ui_scale;
        return PF_TOUCH_NONE;
    }
    case SDL_FINGERMOTION: {
        if (!t->active)
            return PF_TOUCH_NONE;
        float wx = e->tfinger.x * win_w;
        float wy = e->tfinger.y * win_h;
        if (dy)
            *dy = (wy - t->last_wy) / ui_scale;
        t->last_wx = wx;
        t->last_wy = wy;
        if (lx)
            *lx = wx / ui_scale;
        if (ly)
            *ly = (wy - (float)g_pad_top) / ui_scale;
        if (moved)
            *moved = true;
        float ddx = wx - t->start_wx;
        float ddy = wy - t->start_wy;
        float threshold = 12.0f * ui_scale;
        if (!t->dragging && ddx * ddx + ddy * ddy > threshold * threshold)
            t->dragging = true;
        return t->dragging ? PF_TOUCH_DRAG : PF_TOUCH_NONE;
    }
    case SDL_FINGERUP: {
        if (!t->active)
            return PF_TOUCH_NONE;
        bool was_long = t->longpress_done;
        bool was_drag = t->dragging;
        if (lx)
            *lx = t->start_wx / ui_scale;
        if (ly)
            *ly = (t->start_wy - (float)g_pad_top) / ui_scale;
        t->active = false;
        t->dragging = false;
        if (was_long)
            return PF_TOUCH_NONE;
        if (was_drag)
            return PF_TOUCH_NONE;
        return PF_TOUCH_TAP;
    }
    default:
        break;
    }
    return PF_TOUCH_NONE;
}

bool pf_android_touch_longpress_tick(PfAndroidTouch *t, Uint32 now,
                                     float ui_scale, float *lx, float *ly)
{
    if (!t || !t->active || t->dragging || t->longpress_done)
        return false;
    if (now - t->start_ms < 450)
        return false;
    t->longpress_done = true;
    if (lx)
        *lx = t->start_wx / ui_scale;
    if (ly)
        *ly = t->start_wy / ui_scale;
    return true;
}

#else /* desktop build: the platform layer compiles to nothing */

bool pf_android_init(void) { return true; }
void pf_android_shutdown(void) {}

#endif /* PC_ANDROID */
