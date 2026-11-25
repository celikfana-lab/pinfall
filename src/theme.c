/* theme.c - the single runtime-switchable PinFall theme instance.
 *
 * Light palette mirrors docs/UI_TOKENS.json; dark palette follows the native
 * Pinterest Android app (#18181B page / #222226 surface / #2C2C30 raised).
 * Metrics/typography are identical in both modes (copied from light).
 */
#include "theme.h"

#include <string.h>

Theme THEME;

static Theme g_light;
static Theme g_dark;
static bool g_built;
static ThemeMode g_mode = THEME_MODE_SYSTEM;
static bool g_system_dark;

static void theme_metrics(Theme *t)
{
    t->card_radius = 16.0f;
    t->card_gap = 16.0f;
    t->page_pad = 20.0f;
    t->header_h = 80.0f;
    t->footer_h = 28.0f;
    t->search_h = 48.0f;
    t->search_radius = 24.0f;
    t->panel_radius = 16.0f;
    t->caption_gap = 8.0f;
    t->caption_pad_x = 4.0f;
    t->target_col_w = 236.0f;
    t->max_cols = 12;

    t->title_size = 16.0f;
    t->title_line = 22.0f;
    t->meta_size = 12.0f;
    t->logo_size = 24.0f;
    t->search_size = 16.0f;
    t->h1_size = 28.0f;
    t->h2_size = 20.0f;
    t->body_size = 16.0f;
    t->body_line = 22.0f;
    t->small_size = 12.0f;
    t->detail_meta_size = 14.0f;
}

static void theme_build(void)
{
    Theme l;
    Theme d;

    memset(&l, 0, sizeof l);
    l.bg = (SDL_Color){255, 255, 255, 255};
    l.surface = (SDL_Color){255, 255, 255, 255};
    l.surface_alt = (SDL_Color){229, 229, 224, 255};
    l.surface_hover = (SDL_Color){246, 246, 243, 255};
    l.border = (SDL_Color){239, 239, 239, 255};
    l.border_strong = (SDL_Color){205, 205, 205, 255};
    l.text = (SDL_Color){33, 25, 34, 255};
    l.text_dim = (SDL_Color){98, 98, 91, 255};
    l.text_faint = (SDL_Color){118, 118, 118, 255};
    l.brand = (SDL_Color){230, 0, 35, 255};
    l.brand_hover = (SDL_Color){204, 0, 31, 255};
    l.brand_soft = (SDL_Color){253, 236, 238, 255};
    l.on_brand = (SDL_Color){255, 255, 255, 255};
    l.link = (SDL_Color){43, 72, 212, 255};
    l.pill_ink = (SDL_Color){17, 17, 17, 255};
    l.skeleton = (SDL_Color){239, 239, 239, 255};
    l.skeleton_hi = (SDL_Color){246, 246, 243, 255};
    l.scrim = (SDL_Color){0, 0, 0, 153};
    l.overlay = (SDL_Color){0, 0, 0, 64};
    l.error_bg = (SDL_Color){253, 236, 234, 255};
    l.error_text = (SDL_Color){152, 43, 37, 255};
    l.ok = (SDL_Color){46, 125, 50, 255};
    l.shadow = (SDL_Color){0, 0, 0, 255};
    l.cu_card_border = (SDL_Color){200, 200, 193, 255};
    l.cu_divider = (SDL_Color){233, 233, 233, 255};
    l.cu_placeholder = (SDL_Color){145, 151, 163, 255};
    l.cu_comment_ink = (SDL_Color){73, 73, 67, 255};
    l.cu_overlay = (SDL_Color){0, 0, 0, 102};
    theme_metrics(&l);

    d = l;
    d.bg = (SDL_Color){24, 24, 27, 255};          /* #18181B */
    d.surface = (SDL_Color){34, 34, 38, 255};     /* #222226 */
    d.surface_alt = (SDL_Color){44, 44, 48, 255}; /* #2C2C30 */
    d.surface_hover = (SDL_Color){58, 58, 64, 255};
    d.border = (SDL_Color){48, 48, 54, 255};
    d.border_strong = (SDL_Color){90, 90, 98, 255};
    d.text = (SDL_Color){255, 255, 255, 255};
    d.text_dim = (SDL_Color){199, 199, 201, 255}; /* #C7C7C9 */
    d.text_faint = (SDL_Color){154, 154, 160, 255};
    d.brand = (SDL_Color){230, 0, 35, 255};
    d.brand_hover = (SDL_Color){255, 26, 60, 255};
    d.brand_soft = (SDL_Color){58, 26, 32, 255};
    d.on_brand = (SDL_Color){255, 255, 255, 255};
    d.link = (SDL_Color){138, 180, 248, 255}; /* #8AB4F8 */
    d.pill_ink = (SDL_Color){17, 17, 17, 255};
    d.skeleton = (SDL_Color){44, 44, 48, 255};
    d.skeleton_hi = (SDL_Color){53, 53, 58, 255};
    d.scrim = (SDL_Color){0, 0, 0, 179}; /* ~70% */
    d.overlay = (SDL_Color){0, 0, 0, 90};
    d.error_bg = (SDL_Color){58, 30, 28, 255};
    d.error_text = (SDL_Color){255, 138, 128, 255};
    d.ok = (SDL_Color){123, 216, 143, 255};
    d.shadow = (SDL_Color){0, 0, 0, 255};
    d.cu_card_border = (SDL_Color){58, 58, 64, 255};
    d.cu_divider = (SDL_Color){53, 53, 58, 255};
    d.cu_placeholder = (SDL_Color){154, 154, 160, 255};
    d.cu_comment_ink = (SDL_Color){215, 215, 218, 255};
    d.cu_overlay = (SDL_Color){0, 0, 0, 140};

    g_light = l;
    g_dark = d;
    THEME_LIGHT = l;
    THEME_DARK = d;
    g_built = true;
}

Theme THEME_LIGHT = {0};
Theme THEME_DARK = {0};

ThemeMode theme_mode(void)
{
    return g_mode;
}

void theme_set_system_dark(bool dark)
{
    g_system_dark = dark;
}

bool theme_system_dark(void)
{
    return g_system_dark;
}

bool theme_is_dark(void)
{
    if (g_mode == THEME_MODE_DARK)
        return true;
    if (g_mode == THEME_MODE_LIGHT)
        return false;
    return g_system_dark;
}

void theme_apply(void)
{
    if (!g_built)
        theme_build();
    THEME = theme_is_dark() ? g_dark : g_light;
}

void theme_set_mode(ThemeMode mode)
{
    if (mode != THEME_MODE_LIGHT && mode != THEME_MODE_DARK && mode != THEME_MODE_SYSTEM)
        mode = THEME_MODE_SYSTEM;
    g_mode = mode;
    theme_apply();
}
