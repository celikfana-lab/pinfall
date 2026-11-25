/* render.c - SDL2 drawing primitives for PinFall.
 *
 * All geometry is drawn through SDL_RenderGeometry (triangle fans / rings) so
 * rounded rectangles are one draw call each and stay cheap at 60fps. Textures
 * created from decoded pins are STATIC + BLEND and get anti-aliased corner
 * alpha masks at upload time.
 */
#include "render.h"
#include "android.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265358979323846f
#define ARC_SEGS 8
#define MAX_PATH_PTS (4 * (ARC_SEGS + 1))

/* ------------------------------------------------------------------ setup */

bool rnd_init(Rnd *rd, SDL_Window *win)
{
    memset(rd, 0, sizeof *rd);
    rd->win = win;

    rd->r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                           SDL_RENDERER_PRESENTVSYNC);
    if (!rd->r)
        rd->r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!rd->r)
        rd->r = SDL_CreateRenderer(win, -1, 0);
    if (!rd->r)
        return false;

    SDL_SetRenderDrawBlendMode(rd->r, SDL_BLENDMODE_BLEND);
    rnd_update_size(rd);
    return true;
}

void rnd_free(Rnd *rd)
{
    if (rd->r)
        SDL_DestroyRenderer(rd->r);
    memset(rd, 0, sizeof *rd);
}

void rnd_update_size(Rnd *rd)
{
    int ow = 0, oh = 0, ww = 0, wh = 0;
    SDL_GetRendererOutputSize(rd->r, &ow, &oh);
    SDL_GetWindowSize(rd->win, &ww, &wh);
#ifdef PC_ANDROID
    /* Logical UI width ~= 432px: the renderer is scaled so all layout maths
     * keeps using phone-friendly logical units. */
    (void)ow;
    (void)oh;
    rd->win_w = ww > 0 ? ww : 1;
    rd->win_h = wh > 0 ? wh : 1;
    rd->surf_w = ow > 0 ? ow : rd->win_w;
    rd->surf_h = oh > 0 ? oh : rd->win_h;
    float ui = pf_android_effective_ui_scale(rd->win_w);
    rd->out_w = (int)lroundf((float)rd->win_w / ui);
    rd->out_h = (int)lroundf((float)rd->win_h / ui);
    if (rd->out_w < 1)
        rd->out_w = 1;
    if (rd->out_h < 1)
        rd->out_h = 1;
    rd->scale = 1.0f; /* Android: draw in logical px (no extra DPI factor) */
    rd->ui_scale = ui;
    {
        int top = pf_android_top_inset_px();
        int bot = pf_android_bottom_inset_px();

        if (top < 0)
            top = 0;
        if (bot < 0)
            bot = 0;
        if (top + bot > rd->win_h / 2) {
            top = 0;
            bot = 0;
        }
        rd->pad_top_px = top;
        rd->pad_bottom_px = bot;
        pf_android_set_pad(top, bot);
        {
            float fx = (ww > 0 && ow > 0) ? (float)ow / (float)ww : 1.0f;
            float fy = (wh > 0 && oh > 0) ? (float)oh / (float)wh : 1.0f;
            SDL_Rect vp;

            rd->out_h = (int)lroundf((float)(rd->win_h - top - bot) / ui);
            if (rd->out_h < 1)
                rd->out_h = 1;
            vp.x = 0;
            vp.y = (int)lroundf((float)top * fy);
            vp.w = (int)lroundf((float)rd->win_w * fx);
            vp.h = (int)lroundf((float)(rd->win_h - top - bot) * fy);
            if (vp.h < 1)
                vp.h = 1;
            rd->viewport = vp;
            SDL_RenderSetViewport(rd->r, &rd->viewport);
        }
    }
    float out_scale = ui;
    if (ww > 0 && ow > 0)
        out_scale = ui * (float)ow / (float)ww;
    rd->draw_scale = out_scale;
    pf_android_apply_scale(rd->r, out_scale);
#else
    rd->out_w = ow > 0 ? ow : 1;
    rd->out_h = oh > 0 ? oh : 1;
    rd->win_w = ww > 0 ? ww : 1;
    rd->win_h = wh > 0 ? wh : 1;
    rd->surf_w = ow > 0 ? ow : rd->win_w;
    rd->surf_h = oh > 0 ? oh : rd->win_h;
    rd->scale = (float)rd->out_w / (float)rd->win_w;
    if (!(rd->scale > 0.0f))
        rd->scale = 1.0f;
    rd->ui_scale = rd->scale;
#endif
}

/* -------------------------------------------------------------- utilities */

static void solid(SDL_Renderer *r, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

void rnd_clear(Rnd *rd, SDL_Color c)
{
#ifdef PC_ANDROID
    SDL_Rect old;
    float old_scale = 1.0f;

    SDL_RenderGetViewport(rd->r, &old);
    SDL_RenderGetScale(rd->r, &old_scale, NULL);
    SDL_RenderSetViewport(rd->r, NULL);
    SDL_RenderSetScale(rd->r, 1.0f, 1.0f);
    SDL_RenderSetClipRect(rd->r, NULL);
    solid(rd->r, c);
    SDL_RenderClear(rd->r);
    /* ensure the inset strips are painted even if the renderer cleared only
     * part of the target */
    if (rd->pad_top_px > 0) {
        SDL_Rect top = {0, 0, rd->win_w, rd->pad_top_px};

        SDL_RenderFillRect(rd->r, &top);
    }
    if (rd->pad_bottom_px > 0) {
        SDL_Rect bot = {0, rd->win_h - rd->pad_bottom_px, rd->win_w,
                        rd->pad_bottom_px};

        SDL_RenderFillRect(rd->r, &bot);
    }
    if (rd->viewport.w > 0)
        SDL_RenderSetViewport(rd->r, &rd->viewport);
    else
        SDL_RenderSetViewport(rd->r, &old);
    SDL_RenderSetScale(rd->r, rd->draw_scale > 0.0f ? rd->draw_scale
                                                   : old_scale,
                       rd->draw_scale > 0.0f ? rd->draw_scale : old_scale);
#else
    solid(rd->r, c);
    SDL_RenderClear(rd->r);
#endif
}

void rnd_fill(Rnd *rd, float x, float y, float w, float h, SDL_Color c)
{
    if (w <= 0.0f || h <= 0.0f)
        return;
    solid(rd->r, c);
    SDL_FRect rc = {x, y, w, h};
    SDL_RenderFillRectF(rd->r, &rc);
}

void rnd_fill_rect(Rnd *rd, SDL_Rect rc, SDL_Color c)
{
    if (rc.w <= 0 || rc.h <= 0)
        return;
    solid(rd->r, c);
    SDL_RenderFillRect(rd->r, &rc);
}

/* Build a clockwise rounded-rect perimeter starting at the top-left arc. */
static int round_path(float x, float y, float w, float h, float r,
                      float *px, float *py)
{
    static const float a0[4] = {PI_F, 1.5f * PI_F, 0.0f, 0.5f * PI_F};
    int n = 0;
    if (w <= 0.0f || h <= 0.0f)
        return 0;
    if (r < 0.0f)
        r = 0.0f;
    if (r > w * 0.5f)
        r = w * 0.5f;
    if (r > h * 0.5f)
        r = h * 0.5f;

    const float cx[4] = {x + r, x + w - r, x + w - r, x + r};
    const float cy[4] = {y + r, y + r, y + h - r, y + h - r};
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= ARC_SEGS; i++) {
            float a = a0[c] + (0.5f * PI_F) * ((float)i / (float)ARC_SEGS);
            px[n] = cx[c] + r * cosf(a);
            py[n] = cy[c] + r * sinf(a);
            n++;
        }
    }
    return n;
}

void rnd_round(Rnd *rd, float x, float y, float w, float h, float radius,
               SDL_Color c)
{
    if (w <= 0.0f || h <= 0.0f)
        return;
    if (radius <= 0.6f) {
        rnd_fill(rd, x, y, w, h, c);
        return;
    }

    float px[MAX_PATH_PTS], py[MAX_PATH_PTS];
    int n = round_path(x, y, w, h, radius, px, py);
    if (n < 3) {
        rnd_fill(rd, x, y, w, h, c);
        return;
    }

    SDL_Vertex verts[MAX_PATH_PTS + 1];
    int idx[3 * MAX_PATH_PTS];
    memset(verts, 0, sizeof verts);

    verts[0].position.x = x + w * 0.5f;
    verts[0].position.y = y + h * 0.5f;
    verts[0].color = c;
    for (int i = 0; i < n; i++) {
        verts[i + 1].position.x = px[i];
        verts[i + 1].position.y = py[i];
        verts[i + 1].color = c;
    }
    for (int i = 0; i < n; i++) {
        idx[3 * i + 0] = 0;
        idx[3 * i + 1] = i + 1;
        idx[3 * i + 2] = ((i + 1) % n) + 1;
    }
    SDL_RenderGeometry(rd->r, NULL, verts, n + 1, idx, n * 3);
}

void rnd_round_outline(Rnd *rd, float x, float y, float w, float h,
                       float radius, float thick, SDL_Color c)
{
    if (w <= 0.0f || h <= 0.0f || thick <= 0.0f)
        return;
    if (thick > w * 0.5f)
        thick = w * 0.5f;
    if (thick > h * 0.5f)
        thick = h * 0.5f;

    float opx[MAX_PATH_PTS], opy[MAX_PATH_PTS];
    float ipx[MAX_PATH_PTS], ipy[MAX_PATH_PTS];
    int n = round_path(x, y, w, h, radius, opx, opy);
    int m = round_path(x + thick, y + thick, w - 2.0f * thick,
                       h - 2.0f * thick, radius - thick, ipx, ipy);
    if (n < 3 || m != n)
        return;

    SDL_Vertex verts[2 * MAX_PATH_PTS];
    int idx[6 * MAX_PATH_PTS];
    memset(verts, 0, sizeof verts);

    for (int i = 0; i < n; i++) {
        verts[i].position.x = opx[i];
        verts[i].position.y = opy[i];
        verts[i].color = c;
        verts[n + i].position.x = ipx[i];
        verts[n + i].position.y = ipy[i];
        verts[n + i].color = c;
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int o0 = i, o1 = j;
        int i0 = n + i, i1 = n + j;
        idx[6 * i + 0] = o0;
        idx[6 * i + 1] = o1;
        idx[6 * i + 2] = i1;
        idx[6 * i + 3] = o0;
        idx[6 * i + 4] = i1;
        idx[6 * i + 5] = i0;
    }
    SDL_RenderGeometry(rd->r, NULL, verts, 2 * n, idx, 6 * n);
}

void rnd_shadow(Rnd *rd, float x, float y, float w, float h, float radius,
                float spread, Uint8 alpha)
{
    if (spread <= 0.0f || alpha == 0)
        return;
    const int layers = 4;
    for (int k = 0; k < layers; k++) {
        float grow = spread * (1.0f - (float)k / (float)layers);
        SDL_Color c = {0, 0, 0, (Uint8)((alpha + layers - 1 - k) / layers)};
        rnd_round(rd, x - grow, y - grow + spread * 0.35f, w + 2.0f * grow,
                  h + 2.0f * grow, radius + grow, c);
    }
}

void rnd_line(Rnd *rd, float x1, float y1, float x2, float y2, float thick,
              SDL_Color c)
{
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f || thick <= 0.0f)
        return;
    float nx = -dy / len * thick * 0.5f;
    float ny = dx / len * thick * 0.5f;

    SDL_Vertex verts[4];
    int idx[6] = {0, 1, 2, 0, 2, 3};
    memset(verts, 0, sizeof verts);
    verts[0].position.x = x1 + nx;
    verts[0].position.y = y1 + ny;
    verts[1].position.x = x1 - nx;
    verts[1].position.y = y1 - ny;
    verts[2].position.x = x2 - nx;
    verts[2].position.y = y2 - ny;
    verts[3].position.x = x2 + nx;
    verts[3].position.y = y2 + ny;
    for (int i = 0; i < 4; i++)
        verts[i].color = c;
    SDL_RenderGeometry(rd->r, NULL, verts, 4, idx, 6);
}

/* ------------------------------------------------------------------- clip */

void rnd_push_clip(Rnd *rd, float x, float y, float w, float h)
{
    SDL_Rect want;
    want.x = (int)floorf(x);
    want.y = (int)floorf(y);
    want.w = (int)ceilf(w);
    want.h = (int)ceilf(h);
    if (want.w < 0)
        want.w = 0;
    if (want.h < 0)
        want.h = 0;

    if (rd->clip_n > 0) {
        SDL_Rect cur = rd->clip_stack[rd->clip_n - 1];
        int x0 = want.x > cur.x ? want.x : cur.x;
        int y0 = want.y > cur.y ? want.y : cur.y;
        int x1 = (want.x + want.w) < (cur.x + cur.w)
                     ? (want.x + want.w)
                     : (cur.x + cur.w);
        int y1 = (want.y + want.h) < (cur.y + cur.h)
                     ? (want.y + want.h)
                     : (cur.y + cur.h);
        if (x1 < x0)
            x1 = x0;
        if (y1 < y0)
            y1 = y0;
        want.x = x0;
        want.y = y0;
        want.w = x1 - x0;
        want.h = y1 - y0;
    }
    if (rd->clip_n < (int)(sizeof rd->clip_stack / sizeof rd->clip_stack[0]))
        rd->clip_stack[rd->clip_n++] = want;
    SDL_RenderSetClipRect(rd->r, &want);
}

void rnd_pop_clip(Rnd *rd)
{
    if (rd->clip_n > 0)
        rd->clip_n--;
    if (rd->clip_n > 0) {
        SDL_Rect *cur = &rd->clip_stack[rd->clip_n - 1];
        SDL_RenderSetClipRect(rd->r, cur);
    } else {
        SDL_RenderSetClipRect(rd->r, NULL);
    }
}

/* --------------------------------------------------------------- textures */

SDL_Texture *rnd_texture_rgba(Rnd *rd, const uint8_t *rgba, int w, int h,
                              float radius)
{
    if (!rgba || w <= 0 || h <= 0)
        return NULL;

    uint8_t *tmp = NULL;
    int r = (int)lroundf(radius);
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;

    if (r > 0) {
        tmp = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
        if (!tmp)
            return NULL;
        memcpy(tmp, rgba, (size_t)w * (size_t)h * 4u);

        /* anti-aliased rounded-corner alpha mask */
        for (int cy = 0; cy < r; cy++) {
            for (int cx = 0; cx < r; cx++) {
                float fx = (float)cx + 0.5f - (float)r;
                float fy = (float)cy + 0.5f - (float)r;
                float d = sqrtf(fx * fx + fy * fy);
                float cov = (float)r + 0.5f - d;
                if (cov < 0.0f)
                    cov = 0.0f;
                if (cov > 1.0f)
                    cov = 1.0f;
                Uint8 a = (Uint8)(cov * 255.0f + 0.5f);
                if (a == 255)
                    continue;

                size_t offs[4] = {
                    ((size_t)cy * (size_t)w + (size_t)cx) * 4u,
                    ((size_t)cy * (size_t)w + (size_t)(w - 1 - cx)) * 4u,
                    ((size_t)(h - 1 - cy) * (size_t)w + (size_t)cx) * 4u,
                    ((size_t)(h - 1 - cy) * (size_t)w +
                     (size_t)(w - 1 - cx)) * 4u,
                };
                for (int k = 0; k < 4; k++) {
                    uint8_t *px = tmp + offs[k];
                    px[3] = (uint8_t)(((unsigned)px[3] * (unsigned)a) / 255u);
                }
            }
        }
    }

    SDL_Texture *tex = SDL_CreateTexture(rd->r, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        if (SDL_UpdateTexture(tex, NULL, tmp ? tmp : rgba, w * 4) != 0) {
            SDL_DestroyTexture(tex);
            tex = NULL;
        }
    }
    free(tmp);
    return tex;
}

void rnd_texture(Rnd *rd, SDL_Texture *tex, float x, float y, float w,
                 float h)
{
    if (!tex || w <= 0.0f || h <= 0.0f)
        return;
    SDL_FRect dst = {x, y, w, h};
    SDL_RenderCopyF(rd->r, tex, NULL, &dst);
}

void rnd_texture_cover(Rnd *rd, SDL_Texture *tex, float x, float y, float w,
                       float h)
{
    int tw = 0, th = 0;
    if (!tex || w <= 0.0f || h <= 0.0f)
        return;
    if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) != 0 || tw <= 0 ||
        th <= 0) {
        rnd_texture(rd, tex, x, y, w, h);
        return;
    }
    SDL_Rect src = {0, 0, tw, th};
    SDL_FRect dst = {x, y, w, h};
    float src_aspect = (float)tw / (float)th;
    float dst_aspect = w / h;
    if (src_aspect > dst_aspect) {
        src.w = (int)lroundf((float)th * dst_aspect);
        src.x = (tw - src.w) / 2;
    } else {
        src.h = (int)lroundf((float)tw / dst_aspect);
        src.y = (th - src.h) / 2;
    }
    SDL_RenderCopyF(rd->r, tex, &src, &dst);
}

/* ------------------------------------------------------------ screenshot */

bool rnd_save_bmp(SDL_Renderer *r, const char *path)
{
    if (!r || !path)
        return false;
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(r, &w, &h) != 0 || w <= 0 || h <= 0)
        return false;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
        0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf)
        return false;

    bool ok = false;
    if (SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels,
                             surf->pitch) == 0)
        ok = SDL_SaveBMP(surf, path) == 0;
    SDL_FreeSurface(surf);
    return ok;
}
