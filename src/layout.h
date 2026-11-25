/* layout.h - pure masonry/waterfall layout math (no SDL, no network).
 *
 * The feed is laid out in viewport-width-dependent columns, each pin dropped
 * into the shortest column, preserving feed order. Cards are an image plus a
 * fixed two-line title block below it (Pinterest style). Coordinates are in
 * physical pixels and absolute (including the header inset); the viewport is
 * scrolled by subtracting scroll_y at draw time.
 */
#ifndef PINFALL_LAYOUT_H
#define PINFALL_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    float x, y;             /* card origin (image top-left), absolute */
    float w;                /* column width == image width */
    float img_h;            /* image height for natural aspect ratio */
    float card_h;           /* image + title block */
    float title_x, title_y, title_w; /* title block rect */
    float aspect;           /* displayed h/w */
    int   col;
} LayoutItem;

typedef struct {
    LayoutItem *items;
    size_t      count;
    size_t      cap;
    float       total_h;    /* content height including header inset + pad */
    int         cols;
    float       col_w;
    float       gap;
    float       pad;
    int         max_cols;   /* last computed column count for change detect */
} Layout;

void layout_init(Layout *L);
void layout_free(Layout *L);

/* Recompute the whole waterfall.
 * ws/hs        : natural pixel dimensions per pin (0/hs<=0 => fallback aspect)
 * n            : pin count
 * viewport_w   : window width in physical px
 * pad/gap      : horizontal page padding and column gap in px
 * target_col_w : preferred column width in px
 * top_inset    : absolute y where content starts (header height + top pad)
 * title_block_h: height reserved under every image for the two title lines
 */
void layout_compute(Layout *L, const int *ws, const int *hs, size_t n,
                    float viewport_w, float pad, float gap,
                    float target_col_w, int max_cols, float top_inset,
                    float title_block_h);

/* Hit test in content space (pass y + scroll_y); returns pin index or -1. */
int layout_hit(const Layout *L, float x, float y_content, float scroll_y);

#endif /* PINFALL_LAYOUT_H */
