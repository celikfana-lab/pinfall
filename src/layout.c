/* layout.c - masonry layout: shortest-column waterfall in absolute pixels. */
#include "layout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ASPECT_MIN 0.55f /* h/w lower clamp: very wide pins */
#ifdef PC_ANDROID
/* Native masonry keeps very tall pins at their natural aspect. */
#define ASPECT_MAX 4.00f
#else
#define ASPECT_MAX 1.90f /* h/w upper clamp: very tall pins */
#endif
#define FALLBACK_ASPECT 1.20f

void layout_init(Layout *L)
{
    memset(L, 0, sizeof *L);
    L->items = NULL;
    L->count = 0;
    L->cap = 0;
}

void layout_free(Layout *L)
{
    free(L->items);
    memset(L, 0, sizeof *L);
}

static bool ensure_cap(Layout *L, size_t need)
{
    if (need <= L->cap)
        return true;
    size_t cap = L->cap ? L->cap : 32;
    while (cap < need)
        cap *= 2;
    LayoutItem *items = (LayoutItem *)realloc(L->items, cap * sizeof *items);
    if (!items)
        return false;
    L->items = items;
    L->cap = cap;
    return true;
}

static int column_count(float viewport_w, float pad, float gap,
                        float target_col_w, int max_cols)
{
    /* docs/UI_TOKENS.json: N = max(2, ceil((W - 260) / 236)).
     * 260 = 2*outer_inset(20) + 220 reserved; scale it with the DPI factor
     * derived from the scaled target column width. */
    float s = target_col_w > 1.0f ? target_col_w / 236.0f : 1.0f;
    float reserve = 260.0f * s;
    float avail = viewport_w - reserve;
    (void)gap;
    (void)pad;
    if (avail < 1.0f)
        return 1;
    int cols = (int)ceilf(avail / target_col_w);
    if (cols < 2)
        cols = 2;
    if (cols > max_cols)
        cols = max_cols;
    return cols;
}

void layout_compute(Layout *L, const int *ws, const int *hs, size_t n,
                    float viewport_w, float pad, float gap,
                    float target_col_w, int max_cols, float top_inset,
                    float title_block_h)
{
    if (!L || !ws || !hs)
        return;

    int cols = column_count(viewport_w, pad, gap, target_col_w, max_cols);
    float avail = viewport_w - 2.0f * pad;
    if (avail < 1.0f)
        avail = 1.0f;
    float col_w = (avail - gap * (float)(cols - 1)) / (float)cols;
    if (col_w < 24.0f)
        col_w = 24.0f;

    L->cols = cols;
    L->col_w = col_w;
    L->gap = gap;
    L->pad = pad;

    if (!ensure_cap(L, n)) {
        L->count = 0;
        L->total_h = top_inset;
        return;
    }

    float cursors[32];
    if (cols > (int)(sizeof(cursors) / sizeof(cursors[0])))
        cols = (int)(sizeof(cursors) / sizeof(cursors[0]));
    for (int c = 0; c < cols; c++)
        cursors[c] = 0.0f;

    for (size_t i = 0; i < n; i++) {
        LayoutItem *it = &L->items[i];
        memset(it, 0, sizeof *it);

        float aspect = FALLBACK_ASPECT;
        if (ws[i] > 0 && hs[i] > 0)
            aspect = (float)hs[i] / (float)ws[i];
        if (!(aspect > 0.0f))
            aspect = FALLBACK_ASPECT;
        if (aspect < ASPECT_MIN)
            aspect = ASPECT_MIN;
        if (aspect > ASPECT_MAX)
            aspect = ASPECT_MAX;

        int col = 0;
        for (int c = 1; c < cols; c++)
            if (cursors[c] < cursors[col] - 0.01f)
                col = c;

        float img_h = col_w * aspect;
        it->x = pad + (float)col * (col_w + gap);
        it->y = top_inset + cursors[col];
        it->w = col_w;
        it->img_h = img_h;
        it->card_h = img_h + title_block_h;
        it->aspect = aspect;
        it->col = col;
        it->title_x = it->x + gap * 0.25f;
        it->title_y = it->y + img_h + gap * 0.5f;
        it->title_w = col_w - gap * 0.5f;

        cursors[col] += it->card_h + gap;
    }

    float max_cursor = 0.0f;
    for (int c = 0; c < cols; c++)
        if (cursors[c] > max_cursor)
            max_cursor = cursors[c];
    L->count = n;
    L->total_h = top_inset + (max_cursor > gap ? max_cursor - gap : 0.0f) + pad;
}

int layout_hit(const Layout *L, float x, float y_screen, float scroll_y)
{
    if (!L)
        return -1;
    float y = y_screen + scroll_y;
    for (size_t i = 0; i < L->count; i++) {
        const LayoutItem *it = &L->items[i];
        if (x >= it->x && x <= it->x + it->w && y >= it->y &&
            y <= it->y + it->card_h)
            return (int)i;
    }
    return -1;
}
