/* render.h - low level SDL2 drawing helpers (rounded rects, shadows, textures,
 * clipping, framebuffer save). Pure SDL2, no network or layout knowledge. */
#ifndef PINFALL_RENDER_H
#define PINFALL_RENDER_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *r;
    int   out_w, out_h;   /* renderer output size in physical pixels */
    int   win_w, win_h;   /* window size in window units */
    int   surf_w, surf_h; /* renderer output (touch coordinate space) */
    float scale;          /* out_w / win_w (DPI scale factor; 1.0 on Android) */
    float ui_scale;       /* logical -> window px factor (Android; 1 otherwise) */
    int   pad_top_px;     /* Android status bar / cutout inset (window px) */
    int   pad_bottom_px;  /* Android navigation / gesture inset (window px) */
    SDL_Rect viewport;    /* render viewport (excludes the insets) */
    float draw_scale;     /* scale currently applied to the renderer */
    SDL_Rect clip_stack[8];
    int      clip_n;
} Rnd;

bool  rnd_init(Rnd *rd, SDL_Window *win);
void  rnd_free(Rnd *rd);
void  rnd_update_size(Rnd *rd);

void  rnd_clear(Rnd *rd, SDL_Color c);
void  rnd_fill(Rnd *rd, float x, float y, float w, float h, SDL_Color c);
void  rnd_fill_rect(Rnd *rd, SDL_Rect rc, SDL_Color c);
void  rnd_round(Rnd *rd, float x, float y, float w, float h, float radius,
                SDL_Color c);
void  rnd_round_outline(Rnd *rd, float x, float y, float w, float h,
                        float radius, float thick, SDL_Color c);
void  rnd_shadow(Rnd *rd, float x, float y, float w, float h, float radius,
                 float spread, Uint8 alpha);
void  rnd_line(Rnd *rd, float x1, float y1, float x2, float y2, float thick,
               SDL_Color c);

void  rnd_push_clip(Rnd *rd, float x, float y, float w, float h);
void  rnd_pop_clip(Rnd *rd);

/* Upload an RGBA8888 (byte order R,G,B,A) pixel buffer as a STATIC texture,
 * optionally masking the four corners to an anti-aliased rounded rectangle. */
SDL_Texture *rnd_texture_rgba(Rnd *rd, const uint8_t *rgba, int w, int h,
                              float radius);
void  rnd_texture(Rnd *rd, SDL_Texture *tex, float x, float y, float w,
                  float h);
/* Aspect-preserving cover draw (centre crop); never leaves filler bands. */
void  rnd_texture_cover(Rnd *rd, SDL_Texture *tex, float x, float y,
                        float w, float h);

/* Read the current backbuffer and write a .bmp (24/32bpp BMP via SDL_SaveBMP).
 * Returns true on success. */
bool  rnd_save_bmp(SDL_Renderer *r, const char *path);

#endif /* PINFALL_RENDER_H */
