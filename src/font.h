/* font.h - SDL2_ttf font manager: font cache keyed by (size,bold), text
 * texture cache keyed by (text,size,bold), UTF-8 safe measuring, wrapping and
 * pixel-width ellipsizing. Everything draw-side lives here; layout code only
 * receives wrapped strings from ui.c. */
#ifndef PINFALL_FONT_H
#define PINFALL_FONT_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stddef.h>

#define FONT_LINE_MAX 256

typedef struct Fonts Fonts;

bool fonts_init(Fonts **out, SDL_Renderer *ren, const char *regular_path,
                const char *bold_path);
void fonts_free(Fonts **f);

/* line height for a size (TTF line skip) */
int fonts_line_h(Fonts *f, int size, bool bold);

/* measure a UTF-8 string in pixels */
void fonts_measure(Fonts *f, const char *text, int size, bool bold, int *w,
                   int *h);

/* draw a UTF-8 string at (x,y) top-left, returns height (0 on failure) */
int fonts_draw(SDL_Renderer *ren, Fonts *f, const char *text, float x, float y,
               int size, bool bold, SDL_Color c);

/* ellipsize src with U+2026 so that it fits max_w pixels; dst must hold at
 * least strlen(src)+4 bytes (UTF-8 safe). */
void fonts_ellipsize(Fonts *f, const char *src, int size, bool bold,
                     float max_w, char *dst, size_t dstsz);

/* UTF-8 safe word wrap. Returns number of lines written (<= max_lines).
 * The last line is ellipsized when the text is truncated. */
int fonts_wrap(Fonts *f, const char *text, int size, bool bold, float max_w,
               char (*lines)[FONT_LINE_MAX], int max_lines);

/* True when a color-emoji font (NotoColorEmoji) could be loaded and renders
 * glyphs through SDL2_ttf. When false, emoji are stripped as before. */
bool fonts_emoji_available(void);

/* Render a fixed emoji sample and return the number of colored pixels
 * (max-min channel > 30). Used by --emoji-test / selftest; 0 when the emoji
 * font is unavailable. */
int fonts_emoji_probe_pixels(void);

/* number of code points in a UTF-8 string (safe on malformed input) */
int fonts_utf8_len(const char *s);
/* byte offset of code point index `cp` (clamped to end) */
size_t fonts_utf8_offset(const char *s, int cp);

#endif /* PINFALL_FONT_H */
