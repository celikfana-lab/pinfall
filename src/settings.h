/* settings.h - persisted PinFall UI settings (image cache + residency).
 *
 * Stored as key=value in $XDG_CONFIG_HOME/pinfall/settings.conf
 * (fallback: ~/.config/pinfall/settings.conf). Only the Settings overlay
 * writes this file; CLI overrides are applied at runtime without persisting.
 */
#ifndef PINFALL_SETTINGS_H
#define PINFALL_SETTINGS_H

#include "pc.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    PcImgCacheMode cache_mode;   /* off / jpeg / lz4 */
    int            budget_mb;    /* 16 / 32 / 64 / 128 */
    int            keep_screens; /* 1 / 2 / 3 / 5 */
    bool           show_stats;   /* F3 overlay */
    bool           block_ads;    /* hide promoted/sponsored pins */
    bool           autoplay_video; /* close-up video autoplay */
    int            theme_mode;     /* 0=System 1=Light 2=Dark */
} PfSettings;

void settings_defaults(PfSettings *s);

/* Resolve the config path (empty string if neither XDG_CONFIG_HOME nor HOME
 * is set). */
void settings_path(char *buf, size_t n);

/* Load defaults and overlay values from disk. Returns true when the file was
 * read (missing/corrupt files keep defaults and return false). */
bool settings_load(PfSettings *s);

/* Create parent directories and write the file atomically-ish. */
bool settings_save(const PfSettings *s);

/* Explicit-path variants (used by tests so verification never touches the
 * user's real settings file). */
bool settings_load_from(PfSettings *s, const char *path);
bool settings_save_to(const PfSettings *s, const char *path);

const char *settings_cache_mode_name(PcImgCacheMode m);
bool settings_parse_cache_mode(const char *name, PcImgCacheMode *out);
const char *settings_theme_name(int mode);
bool settings_parse_theme(const char *name, int *out);

#endif /* PINFALL_SETTINGS_H */
