/* theme.h - PinFall visual design tokens (runtime-switchable theme).
 *
 * All UI code reads the single global `THEME` (defined in theme.c). Use
 * theme_set_mode(THEME_MODE_SYSTEM/THEME_MODE_LIGHT/THEME_MODE_DARK) + theme_set_system_dark()
 * to switch palettes at runtime; metrics and typography never change.
 */
#ifndef PINFALL_THEME_H
#define PINFALL_THEME_H

#include <SDL2/SDL.h>
#include <stdbool.h>

typedef struct {
    /* colors */
    SDL_Color bg;            /* page background */
    SDL_Color surface;       /* header/panels/sheets */
    SDL_Color surface_alt;   /* search field / secondary chips / buttons */
    SDL_Color surface_hover; /* hover / pressed surface */
    SDL_Color border;        /* hairlines */
    SDL_Color border_strong; /* stronger borders */
    SDL_Color text;          /* primary text */
    SDL_Color text_dim;      /* secondary text */
    SDL_Color text_faint;    /* tertiary text */
    SDL_Color brand;         /* #e60023 PinFall red */
    SDL_Color brand_hover;   /* hovered brand */
    SDL_Color brand_soft;    /* tinted brand background */
    SDL_Color on_brand;      /* foreground on brand fills (white) */
    SDL_Color link;          /* hashtags / links */
    SDL_Color pill_ink;      /* dark ink on white active pill */
    SDL_Color skeleton;      /* image placeholder */
    SDL_Color skeleton_hi;   /* shimmer highlight */
    SDL_Color scrim;         /* modal scrim */
    SDL_Color overlay;       /* card hover wash */
    SDL_Color error_bg;
    SDL_Color error_text;
    SDL_Color ok;
    SDL_Color shadow;
    /* close-up ("Pin detail") surface tokens */
    SDL_Color cu_card_border;
    SDL_Color cu_divider;
    SDL_Color cu_placeholder;
    SDL_Color cu_comment_ink;
    SDL_Color cu_overlay;

    /* metrics (logical px @ 1x) - identical for light and dark */
    float card_radius;       /* 16 */
    float card_gap;          /* 16 feed gutter */
    float page_pad;          /* 20 feed outer inset */
    float header_h;          /* 80 */
    float footer_h;          /* 28 (PinFall status line) */
    float search_h;          /* 48 */
    float search_radius;     /* 24 */
    float panel_radius;      /* 16 detail panel */
    float caption_gap;       /* 8 below image */
    float caption_pad_x;     /* 4 */
    float target_col_w;      /* 236 masonry formula constant */
    int   max_cols;          /* 12 */

    /* type (logical px @ 1x) */
    float title_size;        /* 16 caption title */
    float title_line;        /* 22 */
    float meta_size;         /* 12 */
    float logo_size;         /* 24 wordmark */
    float search_size;       /* 16 */
    float h1_size;           /* 28 detail title */
    float h2_size;           /* 20 */
    float body_size;         /* 16 detail description */
    float body_line;         /* 22 */
    float small_size;        /* 12 */
    float detail_meta_size;  /* 14 */
} Theme;

typedef enum {
    THEME_MODE_SYSTEM = 0,
    THEME_MODE_LIGHT = 1,
    THEME_MODE_DARK = 2
} ThemeMode;

extern Theme THEME;
extern Theme THEME_LIGHT;
extern Theme THEME_DARK;

void theme_set_mode(ThemeMode mode);
ThemeMode theme_mode(void);
/* Android: system night-mode flag; desktop ignores it. */
void theme_set_system_dark(bool dark);
bool theme_system_dark(void);
/* Recompute THEME from the current mode/system flag. */
void theme_apply(void);
bool theme_is_dark(void);

#endif /* PINFALL_THEME_H */
