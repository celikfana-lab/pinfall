/* settings.c - load/save PinFall settings.conf. */
#include "settings.h"
#include "android.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BUDGET_DEFAULT_MB 64
#define KEEP_DEFAULT 2

void settings_defaults(PfSettings *s)
{
    if (!s)
        return;
    s->cache_mode = PC_IMG_CACHE_JPEG;
    s->budget_mb = BUDGET_DEFAULT_MB;
    s->keep_screens = KEEP_DEFAULT;
    s->show_stats = false;
    s->block_ads = true; /* default ON: promoted pins are hidden */
    s->autoplay_video = true; /* default ON: close-up videos play */
    s->theme_mode = 0;        /* System */
}

void settings_path(char *buf, size_t n)
{
    if (!buf || n == 0)
        return;
    buf[0] = '\0';
#ifdef PC_ANDROID
    snprintf(buf, n, "%s", pf_android_settings_path());
    return;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(buf, n, "%s/pinfall/settings.conf", xdg);
    else if (home && *home)
        snprintf(buf, n, "%s/.config/pinfall/settings.conf", home);
#endif
}

const char *settings_theme_name(int mode)
{
    switch (mode) {
    case 1:
        return "light";
    case 2:
        return "dark";
    default:
        return "system";
    }
}

bool settings_parse_theme(const char *name, int *out)
{
    if (!name || !out)
        return false;
    if (strcmp(name, "system") == 0) {
        *out = 0;
        return true;
    }
    if (strcmp(name, "light") == 0) {
        *out = 1;
        return true;
    }
    if (strcmp(name, "dark") == 0) {
        *out = 2;
        return true;
    }
    return false;
}

const char *settings_cache_mode_name(PcImgCacheMode m)
{
    switch (m) {
    case PC_IMG_CACHE_OFF:
        return "off";
    case PC_IMG_CACHE_JPEG:
        return "jpeg";
    case PC_IMG_CACHE_LZ4:
        return "lz4";
    default:
        return "jpeg";
    }
}

bool settings_parse_cache_mode(const char *name, PcImgCacheMode *out)
{
    if (!name || !out)
        return false;
    if (strcmp(name, "off") == 0) {
        *out = PC_IMG_CACHE_OFF;
        return true;
    }
    if (strcmp(name, "jpeg") == 0 || strcmp(name, "jpg") == 0) {
        *out = PC_IMG_CACHE_JPEG;
        return true;
    }
    if (strcmp(name, "lz4") == 0) {
        *out = PC_IMG_CACHE_LZ4;
        return true;
    }
    return false;
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

static void clamp_settings(PfSettings *s)
{
    if (s->budget_mb != 16 && s->budget_mb != 32 && s->budget_mb != 64 &&
        s->budget_mb != 128)
        s->budget_mb = BUDGET_DEFAULT_MB;
    if (s->keep_screens != 1 && s->keep_screens != 2 &&
        s->keep_screens != 3 && s->keep_screens != 5)
        s->keep_screens = KEEP_DEFAULT;
    if (s->cache_mode != PC_IMG_CACHE_OFF &&
        s->cache_mode != PC_IMG_CACHE_JPEG &&
        s->cache_mode != PC_IMG_CACHE_LZ4)
        s->cache_mode = PC_IMG_CACHE_JPEG;
    if (s->theme_mode < 0 || s->theme_mode > 2)
        s->theme_mode = 0;
}

bool settings_load_from(PfSettings *s, const char *path)
{
    if (!s)
        return false;
    settings_defaults(s);
    if (!path || !*path) {
        clamp_settings(s);
        return false;
    }
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        while (*key == ' ' || *key == '\t')
            key++;
        char *ke = key + strlen(key);
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t'))
            *--ke = '\0';
        while (*val == ' ' || *val == '\t')
            val++;
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == '\n' || ve[-1] == '\r' ||
                            ve[-1] == ' ' || ve[-1] == '\t'))
            *--ve = '\0';

        if (strcmp(key, "img_cache") == 0) {
            PcImgCacheMode m;
            if (settings_parse_cache_mode(val, &m))
                s->cache_mode = m;
        } else if (strcmp(key, "img_budget_mb") == 0) {
            s->budget_mb = atoi(val);
        } else if (strcmp(key, "keep_screens") == 0) {
            s->keep_screens = atoi(val);
        } else if (strcmp(key, "stats") == 0) {
            s->show_stats = (atoi(val) != 0);
        } else if (strcmp(key, "block_ads") == 0) {
            s->block_ads = (atoi(val) != 0);
        } else if (strcmp(key, "autoplay_video") == 0) {
            s->autoplay_video = (atoi(val) != 0);
        } else if (strcmp(key, "theme") == 0) {
            int mode;
            if (settings_parse_theme(val, &mode))
                s->theme_mode = mode;
        }
    }
    fclose(f);
    clamp_settings(s);
    return true;
}

bool settings_load(PfSettings *s)
{
    char path[700];
    settings_path(path, sizeof path);
    return settings_load_from(s, path);
}

bool settings_save_to(const PfSettings *s, const char *path)
{
    if (!s || !path || !*path)
        return false;

    char dir[700];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (*dir && !mkdir_p(dir))
            return false;
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "# PinFall settings (written by the app, safe to edit)\n");
    fprintf(f, "img_cache=%s\n", settings_cache_mode_name(s->cache_mode));
    fprintf(f, "img_budget_mb=%d\n", s->budget_mb);
    fprintf(f, "keep_screens=%d\n", s->keep_screens);
    fprintf(f, "stats=%d\n", s->show_stats ? 1 : 0);
    fprintf(f, "block_ads=%d\n", s->block_ads ? 1 : 0);
    fprintf(f, "autoplay_video=%d\n", s->autoplay_video ? 1 : 0);
    fprintf(f, "theme=%s\n", settings_theme_name(s->theme_mode));
    bool ok = (fclose(f) == 0);
    return ok;
}

bool settings_save(const PfSettings *s)
{
    char path[700];
    settings_path(path, sizeof path);
    return settings_save_to(s, path);
}
