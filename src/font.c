/* font.c - SDL2_ttf font + text texture cache, UTF-8 wrapping/ellipsizing.
 *
 * Text is the hot path in the feed (2 clamped title lines per card), so every
 * unique (string,size,bold) is rendered once into a cached texture. Cache
 * tables are open-addressed; when they get hot they are cleared wholesale
 * (strings are UI labels, so the working set is small).
 */
#include "font.h"
#include "android.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FONT_SLOTS 48
#define TEXT_CACHE_INIT 1024
#define TEXT_CACHE_SOFT_MAX 1600
#define TEXT_MAX_CACHED 158
#define EMOJI_CACHE_MAX 128
#define ELLIPSIS "\xE2\x80\xA6"

typedef struct {
    uint64_t     hash;
    SDL_Texture *tex; /* NULL == empty slot */
    int          w, h;
    int          size;
    bool         bold;
    char         text[TEXT_MAX_CACHED];
} TEntry;

struct Fonts {
    SDL_Renderer *ren;
    char          regular[600];
    char          bold_path[600];

    struct {
        int       size;
        bool      bold;
        TTF_Font *font;
    } slots[MAX_FONT_SLOTS];
    int nslots;

    TEntry *tab;
    int     tcap;
    int     tcount;

    /* color emoji fallback (NotoColorEmoji bitmap strike) */
    TTF_Font *emoji_font;
    struct {
        char        cluster[64];
        SDL_Texture *tex;
        int         nw, nh;
    } ecache[EMOJI_CACHE_MAX];
    int ecount;
};

/* --------------------------------------------------------------- helpers */

static int utf8_seq_len(const char *s)
{
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

int fonts_utf8_len(const char *s)
{
    if (!s)
        return 0;
    int n = 0;
    for (size_t i = 0; s[i];) {
        int cl = utf8_seq_len(s + i);
        i += (size_t)cl;
        n++;
    }
    return n;
}

size_t fonts_utf8_offset(const char *s, int cp)
{
    if (!s || cp <= 0)
        return 0;
    size_t i = 0;
    int n = 0;
    while (s[i] && n < cp) {
        i += (size_t)utf8_seq_len(s + i);
        n++;
    }
    return i;
}

/* DejaVu Sans has no emoji/astral glyphs: strip code points that would
 * render as tofu boxes, plus emoji joiners/variation selectors. */
static int utf8_decode(const char *s, unsigned *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((unsigned)(u[0] & 0x1F) << 6) | (unsigned)(u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 &&
        (u[2] & 0xC0) == 0x80) {
        *cp = ((unsigned)(u[0] & 0x0F) << 12) |
              ((unsigned)(u[1] & 0x3F) << 6) | (unsigned)(u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 &&
        (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((unsigned)(u[0] & 0x07) << 18) |
              ((unsigned)(u[1] & 0x3F) << 12) |
              ((unsigned)(u[2] & 0x3F) << 6) | (unsigned)(u[3] & 0x3F);
        return 4;
    }
    *cp = u[0];
    return 1;
}

static bool unsupported_cp(unsigned cp)
{
    return cp > 0xFFFF || cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

static bool needs_sanitize(const char *s)
{
    for (size_t i = 0; s[i];) {
        unsigned cp = 0;
        int n = utf8_decode(s + i, &cp);
        if (n <= 0)
            break;
        if (unsupported_cp(cp))
            return true;
        i += (size_t)n;
    }
    return false;
}

static void sanitize_utf8(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i];) {
        unsigned cp = 0;
        int n = utf8_decode(src + i, &cp);
        if (n <= 0)
            break;
        if (!unsupported_cp(cp) && o + (size_t)n < dstsz) {
            memcpy(dst + o, src + i, (size_t)n);
            o += (size_t)n;
        }
        i += (size_t)n;
    }
    dst[o] = '\0';
}

static uint64_t hash_text(const char *text, int size, bool bold)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)(unsigned)size * 0x9E3779B97F4A7C15ULL;
    h ^= bold ? 0xD6E8FEB86659FD93ULL : 0;
    return h;
}

/* Open a TTF font: filesystem path on desktop, APK asset RW on Android. */
static TTF_Font *open_ttf_path(const char *path, int size)
{
    if (!path || !*path)
        return NULL;
#ifdef PC_ANDROID
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    if (!rw)
        return NULL;
    return TTF_OpenFontRW(rw, 1, size);
#else
    return TTF_OpenFont(path, size);
#endif
}

/* ------------------------------------------------------------ font cache */

static TTF_Font *get_font(Fonts *F, int size, bool bold)
{
    if (!F)
        return NULL;
    if (size < 6)
        size = 6;
    if (size > 220)
        size = 220;

    for (int i = 0; i < F->nslots; i++)
        if (F->slots[i].size == size && F->slots[i].bold == bold)
            return F->slots[i].font;

    if (F->nslots >= MAX_FONT_SLOTS)
        return NULL;

    const char *path = bold ? F->bold_path : F->regular;
    TTF_Font *font = open_ttf_path(path, size);
    if (!font)
        return NULL;
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    TTF_SetFontKerning(font, 1);
    F->slots[F->nslots].size = size;
    F->slots[F->nslots].bold = bold;
    F->slots[F->nslots].font = font;
    F->nslots++;
    return font;
}

/* ------------------------------------------------------- color emoji ----
 * NotoColorEmoji is a fixed 136x128 bitmap strike: TTF_SetFontSize() is a
 * no-op, so clusters are rendered natively and drawn scaled to ~1.05x the
 * text size. ZWJ sequences don't compose in SDL_ttf; we split at U+200D and
 * render the components separately. Classification is by emoji RANGES (DejaVu
 * has monochrome outlines for some emoji codepoints, so glyph coverage is not
 * a reliable signal).
 */
#define EMOJI_NATIVE_SIZE 32

static char g_emoji_path[600];
static int  g_emoji_state = -1; /* -1 unchecked, 0 missing, 1 available */

static bool emoji_file_ok(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static void emoji_probe(void)
{
    if (g_emoji_state >= 0)
        return;
    g_emoji_state = 0;
#ifdef PC_ANDROID
    {
        TTF_Font *f = open_ttf_path(PF_ANDROID_FONT_EMOJI, EMOJI_NATIVE_SIZE);
        if (f) {
            bool has = TTF_GlyphIsProvided32(f, 0x1F600) != 0;
            TTF_CloseFont(f);
            if (has) {
                snprintf(g_emoji_path, sizeof g_emoji_path, "%s",
                         PF_ANDROID_FONT_EMOJI);
                g_emoji_state = 1;
            }
        }
    }
    if (g_emoji_state == 1)
        return;
#endif
    char bufs[6][700];
    const char *cands[8];
    int n = 0;
    const char *env = getenv("PC_EMOJI_FONT");
    if (env && *env)
        cands[n++] = env;
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(bufs[0], sizeof bufs[0], "%s/fonts/NotoColorEmoji.ttf", xdg);
        cands[n++] = bufs[0];
    }
    if (home && *home) {
        snprintf(bufs[1], sizeof bufs[1],
                 "%s/.local/share/fonts/NotoColorEmoji.ttf", home);
        cands[n++] = bufs[1];
    }
    cands[n++] = "/usr/share/fonts/noto/NotoColorEmoji.ttf";
    cands[n++] = "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf";
    for (int i = 0; i < n; i++) {
        if (!emoji_file_ok(cands[i]))
            continue;
        TTF_Font *f = TTF_OpenFont(cands[i], EMOJI_NATIVE_SIZE);
        if (!f)
            continue;
        bool has = TTF_GlyphIsProvided32(f, 0x1F600) != 0;
        TTF_CloseFont(f);
        if (has) {
            snprintf(g_emoji_path, sizeof g_emoji_path, "%s", cands[i]);
            g_emoji_state = 1;
            return;
        }
    }
}

bool fonts_emoji_available(void)
{
    emoji_probe();
    return g_emoji_state == 1;
}

int fonts_emoji_probe_pixels(void)
{
    if (!fonts_emoji_available())
        return 0;
    TTF_Font *f = open_ttf_path(g_emoji_path, EMOJI_NATIVE_SIZE);
    if (!f)
        return 0;
    SDL_Color white = {255, 255, 255, 255};
    /* grinning face, thumbs up, red heart */
    SDL_Surface *s = TTF_RenderUTF8_Blended(
        f, "\xF0\x9F\x98\x80\xF0\x9F\x91\x8D\xE2\x9D\xA4\xEF\xB8\x8F",
        white);
    TTF_CloseFont(f);
    if (!s)
        return 0;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(s);
    if (!rgba)
        return 0;
    int colored = 0;
    for (int y = 0; y < rgba->h; y++) {
        const Uint32 *row =
            (const Uint32 *)((const Uint8 *)rgba->pixels + (size_t)y *
                                                                rgba->pitch);
        for (int x = 0; x < rgba->w; x++) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(row[x], rgba->format, &r, &g, &b, &a);
            if (a > 60) {
                Uint8 mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
                Uint8 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
                if ((int)mx - (int)mn > 30)
                    colored++;
            }
        }
    }
    SDL_FreeSurface(rgba);
    return colored;
}

static TTF_Font *get_emoji_font(Fonts *F)
{
    if (!F)
        return NULL;
    if (!F->emoji_font && fonts_emoji_available())
        F->emoji_font = open_ttf_path(g_emoji_path, EMOJI_NATIVE_SIZE);
    return F->emoji_font;
}

static SDL_Texture *emoji_tex(Fonts *F, const char *cluster, int *out_nw,
                              int *out_nh)
{
    for (int i = 0; i < F->ecount; i++)
        if (strcmp(F->ecache[i].cluster, cluster) == 0) {
            if (out_nw)
                *out_nw = F->ecache[i].nw;
            if (out_nh)
                *out_nh = F->ecache[i].nh;
            return F->ecache[i].tex;
        }
    TTF_Font *ef = get_emoji_font(F);
    if (!ef)
        return NULL;
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(ef, cluster, white);
    if (!surf)
        return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(F->ren, surf);
    if (tex)
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    int nw = surf->w, nh = surf->h;
    SDL_FreeSurface(surf);
    if (!tex)
        return NULL;
    if (F->ecount >= EMOJI_CACHE_MAX) {
        for (int i = 0; i < F->ecount; i++)
            if (F->ecache[i].tex)
                SDL_DestroyTexture(F->ecache[i].tex);
        F->ecount = 0;
    }
    int idx = F->ecount++;
    snprintf(F->ecache[idx].cluster, sizeof F->ecache[idx].cluster, "%s",
             cluster);
    F->ecache[idx].tex = tex;
    F->ecache[idx].nw = nw;
    F->ecache[idx].nh = nh;
    if (out_nw)
        *out_nw = nw;
    if (out_nh)
        *out_nh = nh;
    return tex;
}

static bool cp_is_emoji(unsigned cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) || /* faces, symbols, flags, skin */
           (cp >= 0x1F1E6 && cp <= 0x1F1FF) || /* regional indicators */
           (cp >= 0x2600 && cp <= 0x27BF) ||   /* misc symbols/dingbats */
           (cp >= 0x2B00 && cp <= 0x2BFF) ||   /* arrows/stars */
           (cp >= 0x2300 && cp <= 0x23FF) ||   /* ⌚⌛⏰ */
           (cp >= 0x25A0 && cp <= 0x25FF) ||   /* geometric shapes */
           (cp >= 0x2190 && cp <= 0x21FF);     /* arrows (VS16 emoji) */
}

/* Build the next emoji cluster at p: base + FE0F/FE0E/skin tone, or a
 * regional-indicator pair (flag). Returns byte length, 0 if not emoji. */
static size_t emoji_cluster_len(const char *p, char *out, size_t outsz)
{
    unsigned cp;
    int n = utf8_decode(p, &cp);
    if (n <= 0 || !cp_is_emoji(cp))
        return 0;
    size_t off = (size_t)n;
    size_t used = (size_t)n < outsz - 1 ? (size_t)n : outsz - 1;
    memcpy(out, p, used);
    for (;;) {
        unsigned m;
        int mn = utf8_decode(p + off, &m);
        if (mn <= 0 || !p[off])
            break;
        if (m == 0xFE0F || m == 0xFE0E ||
            (m >= 0x1F3FB && m <= 0x1F3FF)) {
            if (off + (size_t)mn < outsz - 1) {
                memcpy(out + off, p + off, (size_t)mn);
                used = off + (size_t)mn;
            }
            off += (size_t)mn;
            continue;
        }
        break;
    }
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {
        unsigned m;
        int mn = utf8_decode(p + off, &m);
        if (mn > 0 && m >= 0x1F1E6 && m <= 0x1F1FF) {
            if (off + (size_t)mn < outsz - 1) {
                memcpy(out + off, p + off, (size_t)mn);
                used = off + (size_t)mn;
            }
            off += (size_t)mn;
        }
    }
    out[used] = '\0';
    return off;
}

/* Run-aware measurement (used by measure/wrap/ellipsize). */
static void measure_runs(Fonts *F, const char *text, int size, bool bold,
                         int *out_w, int *out_h)
{
    TTF_Font *font = get_font(F, size, bold);
    if (!font)
        return;
    int B = TTF_FontAscent(font);
    int D = TTF_FontDescent(font);
    if (D < 0)
        D = -D;
    int eh = (int)lroundf((float)size * 1.05f);
    int S = eh > B ? eh - B : 0;
    int width = 0;
    const char *p = text;
    while (*p) {
        unsigned cp;
        int n = utf8_decode(p, &cp);
        if (n <= 0)
            break;
        if (cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F) {
            p += n;
            continue;
        }
        char cluster[64];
        size_t cl = emoji_cluster_len(p, cluster, sizeof cluster);
        if (cl > 0) {
            int nw = 0, nh = 0;
            if (emoji_tex(F, cluster, &nw, &nh) && nh > 0) {
                width += (int)lroundf((float)nw * (float)eh / (float)nh);
            } else {
                width += eh;
            }
            p += cl;
            continue;
        }
        if (cp > 0xFFFF) { /* non-emoji astral: primary font has no glyph */
            p += n;
            continue;
        }
        size_t off = 0;
        while (p[off]) {
            unsigned c2;
            int n2 = utf8_decode(p + off, &c2);
            if (n2 <= 0)
                break;
            if (c2 == 0x200D || c2 == 0xFE0E || c2 == 0xFE0F ||
                cp_is_emoji(c2) || c2 > 0xFFFF)
                break;
            off += (size_t)n2;
        }
        if (off == 0) {
            p += n;
            continue;
        }
        char buf[4096];
        size_t bl = off < sizeof buf - 1 ? off : sizeof buf - 1;
        memcpy(buf, p, bl);
        buf[bl] = '\0';
        int tw = 0, th = 0;
        if (TTF_SizeUTF8(font, buf, &tw, &th) == 0)
            width += tw;
        p += off;
    }
    if (out_w)
        *out_w = width;
    if (out_h)
        *out_h = S + B + D;
}

bool fonts_init(Fonts **out, SDL_Renderer *ren, const char *regular_path,
                const char *bold_path)
{
    if (!out || !ren || !regular_path)
        return false;
    Fonts *F = (Fonts *)calloc(1, sizeof *F);
    if (!F)
        return false;
    F->ren = ren;
#ifdef PC_ANDROID
    (void)regular_path;
    (void)bold_path;
    snprintf(F->regular, sizeof F->regular, "%s", PF_ANDROID_FONT_REG);
    snprintf(F->bold_path, sizeof F->bold_path, "%s", PF_ANDROID_FONT_BOLD);
#else
    snprintf(F->regular, sizeof F->regular, "%s", regular_path);
    snprintf(F->bold_path, sizeof F->bold_path, "%s",
             bold_path ? bold_path : regular_path);
#endif
    F->tcap = TEXT_CACHE_INIT;
    F->tab = (TEntry *)calloc((size_t)F->tcap, sizeof *F->tab);
    if (!F->tab) {
        free(F);
        return false;
    }

    /* sanity check: both fonts must be loadable */
    if (!get_font(F, 16, false) || !get_font(F, 16, true)) {
        fonts_free(&F);
        return false;
    }
    *out = F;
    return true;
}

void fonts_free(Fonts **fp)
{
    if (!fp || !*fp)
        return;
    Fonts *F = *fp;
    for (int i = 0; i < F->nslots; i++)
        if (F->slots[i].font)
            TTF_CloseFont(F->slots[i].font);
    if (F->emoji_font)
        TTF_CloseFont(F->emoji_font);
    for (int i = 0; i < F->ecount; i++)
        if (F->ecache[i].tex)
            SDL_DestroyTexture(F->ecache[i].tex);
    if (F->tab) {
        for (int i = 0; i < F->tcap; i++)
            if (F->tab[i].tex)
                SDL_DestroyTexture(F->tab[i].tex);
        free(F->tab);
    }
    free(F);
    *fp = NULL;
}

int fonts_line_h(Fonts *F, int size, bool bold)
{
    TTF_Font *font = get_font(F, size, bold);
    if (!font)
        return size + 2;
    return TTF_FontLineSkip(font);
}

void fonts_measure(Fonts *F, const char *text, int size, bool bold, int *w,
                   int *h)
{
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    if (!text || !*text || !F)
        return;
    if (fonts_emoji_available()) {
        measure_runs(F, text, size, bold, w, h);
        return;
    }
    char sbuf[4096];
    if (!fonts_emoji_available() && needs_sanitize(text)) {
        sanitize_utf8(text, sbuf, sizeof sbuf);
        text = sbuf;
        if (!*text)
            return;
    }
    TTF_Font *font = get_font(F, size, bold);
    if (!font)
        return;
    int tw = 0, th = 0;
    if (TTF_SizeUTF8(font, text, &tw, &th) == 0) {
        if (w)
            *w = tw;
        if (h)
            *h = th;
    }
}

/* ------------------------------------------------------------ text cache */

static void text_cache_clear(Fonts *F)
{
    for (int i = 0; i < F->tcap; i++) {
        if (F->tab[i].tex)
            SDL_DestroyTexture(F->tab[i].tex);
        memset(&F->tab[i], 0, sizeof F->tab[i]);
    }
    F->tcount = 0;
}

static bool text_cache_grow(Fonts *F)
{
    int ncap = F->tcap * 2;
    TEntry *ntab = (TEntry *)calloc((size_t)ncap, sizeof *ntab);
    if (!ntab)
        return false;
    for (int i = 0; i < F->tcap; i++) {
        TEntry *e = &F->tab[i];
        if (!e->tex)
            continue;
        int idx = (int)(e->hash & (uint64_t)(ncap - 1));
        while (ntab[idx].tex)
            idx = (idx + 1) & (ncap - 1);
        ntab[idx] = *e;
    }
    free(F->tab);
    F->tab = ntab;
    F->tcap = ncap;
    return true;
}

static SDL_Texture *render_text_texture(Fonts *F, const char *text, int size,
                                        bool bold, int *out_w, int *out_h)
{
    TTF_Font *font = get_font(F, size, bold);
    if (!font)
        return NULL;
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, white);
    if (!surf)
        return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(F->ren, surf);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        if (out_w)
            *out_w = surf->w;
        if (out_h)
            *out_h = surf->h;
    }
    SDL_FreeSurface(surf);
    return tex;
}

static TEntry *text_cache_get(Fonts *F, const char *text, int size, bool bold)
{
    uint64_t h = hash_text(text, size, bold);
    int mask = F->tcap - 1;
    int idx = (int)(h & (uint64_t)mask);
    while (F->tab[idx].tex) {
        TEntry *e = &F->tab[idx];
        if (e->hash == h && e->size == size && e->bold == bold &&
            strcmp(e->text, text) == 0)
            return e;
        idx = (idx + 1) & mask;
    }

    if (F->tcount + 1 > TEXT_CACHE_SOFT_MAX) {
        /* Bounded working set. Previously this branch grew the table forever,
         * so dynamic strings (e.g. the live stats overlay: fps/hits/bytes
         * change every frame) leaked textures without bound. Clear instead. */
        text_cache_clear(F);
        mask = F->tcap - 1;
        idx = (int)(h & (uint64_t)mask);
    } else if ((F->tcount + 1) * 4 > F->tcap * 3) {
        if (!text_cache_grow(F)) {
            text_cache_clear(F);
            mask = F->tcap - 1;
            idx = (int)(h & (uint64_t)mask);
        }
    }

    int w = 0, hh = 0;
    SDL_Texture *tex = render_text_texture(F, text, size, bold, &w, &hh);
    if (!tex)
        return NULL;

    TEntry *e = &F->tab[idx];
    e->hash = h;
    e->tex = tex;
    e->w = w;
    e->h = hh;
    e->size = size;
    e->bold = bold;
    size_t tl = strlen(text);
    if (tl >= sizeof e->text)
        tl = sizeof e->text - 1;
    memcpy(e->text, text, tl);
    e->text[tl] = '\0';
    F->tcount++;
    return e;
}


/* Run-aware draw: text runs use the cached primary-font textures (tinted with
 * c); emoji runs use cached color textures (never tinted, so colors survive).
 */
static int draw_runs(SDL_Renderer *ren, Fonts *F, const char *text, int size,
                     bool bold, SDL_Color c, float x, float y)
{
    TTF_Font *font = get_font(F, size, bold);
    if (!font)
        return 0;
    int B = TTF_FontAscent(font);
    int D = TTF_FontDescent(font);
    if (D < 0)
        D = -D;
    int eh = (int)lroundf((float)size * 1.05f);
    int S = eh > B ? eh - B : 0;
    float pen = x;
    const char *p = text;
    while (*p) {
        unsigned cp;
        int n = utf8_decode(p, &cp);
        if (n <= 0)
            break;
        if (cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F) {
            p += n;
            continue;
        }
        char cluster[64];
        size_t cl = emoji_cluster_len(p, cluster, sizeof cluster);
        if (cl > 0) {
            int nw = 0, nh = 0;
            SDL_Texture *tex = emoji_tex(F, cluster, &nw, &nh);
            if (tex && nh > 0) {
                float h = (float)eh;
                float w = h * (float)nw / (float)nh;
                SDL_SetTextureColorMod(tex, 255, 255, 255);
                SDL_SetTextureAlphaMod(tex, c.a);
                SDL_FRect dst = {pen, y + (float)S + (float)(B - eh), w, h};
                SDL_RenderCopyF(ren, tex, NULL, &dst);
                pen += w;
            } else {
                pen += (float)eh;
            }
            p += cl;
            continue;
        }
        if (cp > 0xFFFF) {
            p += n;
            continue;
        }
        size_t off = 0;
        while (p[off]) {
            unsigned c2;
            int n2 = utf8_decode(p + off, &c2);
            if (n2 <= 0)
                break;
            if (c2 == 0x200D || c2 == 0xFE0E || c2 == 0xFE0F ||
                cp_is_emoji(c2) || c2 > 0xFFFF)
                break;
            off += (size_t)n2;
        }
        if (off == 0) {
            p += n;
            continue;
        }
        char buf[4096];
        size_t bl = off < sizeof buf - 1 ? off : sizeof buf - 1;
        memcpy(buf, p, bl);
        buf[bl] = '\0';
        SDL_Texture *tex = NULL;
        int tw = 0, th = 0;
        if (strlen(buf) < TEXT_MAX_CACHED) {
            TEntry *e = text_cache_get(F, buf, size, bold);
            if (e) {
                tex = e->tex;
                tw = e->w;
                th = e->h;
            }
        } else {
            tex = render_text_texture(F, buf, size, bold, &tw, &th);
            if (tex)
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        }
        if (tex) {
            SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(tex, c.a);
            SDL_FRect dst = {pen, y + (float)S, (float)tw, (float)th};
            SDL_RenderCopyF(ren, tex, NULL, &dst);
            pen += (float)tw;
            if (strlen(buf) >= TEXT_MAX_CACHED)
                SDL_DestroyTexture(tex);
        }
        p += off;
    }
    return S + B + D;
}

int fonts_draw(SDL_Renderer *ren, Fonts *F, const char *text, float x, float y,
               int size, bool bold, SDL_Color c)
{
    if (!ren || !F || !text || !*text)
        return 0;
    if (fonts_emoji_available())
        return draw_runs(ren, F, text, size, bold, c, x, y);
    char sbuf[4096];
    if (!fonts_emoji_available() && needs_sanitize(text)) {
        sanitize_utf8(text, sbuf, sizeof sbuf);
        text = sbuf;
        if (!*text)
            return 0;
    }

    if (strlen(text) < TEXT_MAX_CACHED) {
        TEntry *e = text_cache_get(F, text, size, bold);
        if (!e)
            return 0;
        SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(e->tex, c.a);
        SDL_FRect dst = {x, y, (float)e->w, (float)e->h};
        SDL_RenderCopyF(ren, e->tex, NULL, &dst);
        return e->h;
    }

    int w = 0, h = 0;
    SDL_Texture *tex = render_text_texture(F, text, size, bold, &w, &h);
    if (!tex)
        return 0;
    SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(tex, c.a);
    SDL_FRect dst = {x, y, (float)w, (float)h};
    SDL_RenderCopyF(ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    return h;
}

/* -------------------------------------------------------- wrap/ellipsis */

void fonts_ellipsize(Fonts *F, const char *src, int size, bool bold,
                     float max_w, char *dst, size_t dstsz)
{
    if (!dst || dstsz == 0)
        return;
    dst[0] = '\0';
    if (!src || !*src || !F) {
        return;
    }
    char sbuf[4096];
    if (!fonts_emoji_available() && needs_sanitize(src)) {
        sanitize_utf8(src, sbuf, sizeof sbuf);
        src = sbuf;
        if (!*src)
            return;
    }
    if (max_w < 1.0f)
        max_w = 1.0f;

    int w = 0;
    fonts_measure(F, src, size, bold, &w, NULL);
    if ((float)w <= max_w) {
        snprintf(dst, dstsz, "%s", src);
        return;
    }

    size_t offs[FONT_LINE_MAX + 2];
    int ncp = 0;
    size_t slen = strlen(src);
    for (size_t i = 0; src[i] && ncp < (int)(sizeof offs / sizeof offs[0]) - 1;) {
        offs[ncp++] = i;
        int cl = utf8_seq_len(src + i);
        i += (size_t)cl;
    }
    offs[ncp] = slen;

    char cand[FONT_LINE_MAX * 2 + 8];
    for (int k = ncp; k >= 0; k--) {
        size_t plen = offs[k];
        if (plen + sizeof ELLIPSIS >= sizeof cand)
            continue;
        memcpy(cand, src, plen);
        memcpy(cand + plen, ELLIPSIS, sizeof ELLIPSIS);
        int cw = 0;
        fonts_measure(F, cand, size, bold, &cw, NULL);
        if ((float)cw <= max_w || k == 0) {
            snprintf(dst, dstsz, "%s", cand);
            return;
        }
    }
    snprintf(dst, dstsz, "%s", ELLIPSIS);
}

int fonts_wrap(Fonts *F, const char *text, int size, bool bold, float max_w,
               char (*lines)[FONT_LINE_MAX], int max_lines)
{
    if (!F || !text || !*text || !lines || max_lines <= 0)
        return 0;
    char sbuf[4096];
    if (!fonts_emoji_available() && needs_sanitize(text)) {
        sanitize_utf8(text, sbuf, sizeof sbuf);
        text = sbuf;
        if (!*text)
            return 0;
    }
    if (max_w < 1.0f)
        max_w = 1.0f;

    int n = 0;
    char cur[FONT_LINE_MAX];
    cur[0] = '\0';
    size_t curlen = 0;
    int curw = 0;
    int spacew = 0;
    fonts_measure(F, " ", size, bold, &spacew, NULL);
    if (spacew <= 0)
        spacew = size / 3;

    const char *p = text;
    bool truncated = false;

    while (*p && n < max_lines) {
        if (*p == '\n') {
            snprintf(lines[n], FONT_LINE_MAX, "%s", cur);
            n++;
            cur[0] = '\0';
            curlen = 0;
            curw = 0;
            p++;
            if (n >= max_lines) {
                truncated = (*p != '\0');
                break;
            }
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }

        /* tokenize one word */
        const char *w0 = p;
        size_t wl = 0;
        while (p[wl] && p[wl] != ' ' && p[wl] != '\t' && p[wl] != '\n' &&
               p[wl] != '\r')
            wl++;
        char word[FONT_LINE_MAX];
        size_t take = wl < sizeof word - 1 ? wl : sizeof word - 1;
        memcpy(word, w0, take);
        word[take] = '\0';
        p += take;

        int ww = 0;
        fonts_measure(F, word, size, bold, &ww, NULL);

        if (curlen > 0 && curw + spacew + ww > max_w) {
            snprintf(lines[n], FONT_LINE_MAX, "%s", cur);
            n++;
            cur[0] = '\0';
            curlen = 0;
            curw = 0;
            if (n >= max_lines) {
                truncated = true;
                break;
            }
        }

        if (ww > max_w) {
            /* hard break an over-wide word by code points */
            const char *q = word;
            while (*q) {
                int cl = utf8_seq_len(q);
                size_t left = strlen(q);
                if ((size_t)cl > left)
                    cl = (int)left;
                char cand[FONT_LINE_MAX];
                if (curlen + (size_t)cl + 1 >= FONT_LINE_MAX) {
                    snprintf(lines[n], FONT_LINE_MAX, "%s", cur);
                    n++;
                    cur[0] = '\0';
                    curlen = 0;
                    curw = 0;
                    if (n >= max_lines) {
                        truncated = true;
                        break;
                    }
                }
                memcpy(cand, cur, curlen);
                memcpy(cand + curlen, q, (size_t)cl);
                cand[curlen + (size_t)cl] = '\0';
                int cw = 0;
                fonts_measure(F, cand, size, bold, &cw, NULL);
                if ((float)cw > max_w && curlen > 0) {
                    snprintf(lines[n], FONT_LINE_MAX, "%s", cur);
                    n++;
                    cur[0] = '\0';
                    curlen = 0;
                    curw = 0;
                    if (n >= max_lines) {
                        truncated = true;
                        break;
                    }
                    continue; /* retry this code point on the new line */
                }
                memcpy(cur, cand, curlen + (size_t)cl + 1);
                curlen += (size_t)cl;
                curw = cw;
                q += cl;
            }
            if (truncated)
                break;
            continue;
        }

        if (curlen > 0) {
            if (curlen + 1 >= FONT_LINE_MAX) {
                snprintf(lines[n], FONT_LINE_MAX, "%s", cur);
                n++;
                cur[0] = '\0';
                curlen = 0;
                curw = 0;
                if (n >= max_lines) {
                    truncated = true;
                    break;
                }
            } else {
                cur[curlen++] = ' ';
                cur[curlen] = '\0';
            }
        }
        if (curlen + strlen(word) + 1 >= FONT_LINE_MAX) {
            snprintf(lines[n], FONT_LINE_MAX, "%s", cur);
            n++;
            cur[0] = '\0';
            curlen = 0;
            curw = 0;
            if (n >= max_lines) {
                truncated = true;
                break;
            }
        }
        memcpy(cur + curlen, word, strlen(word) + 1);
        curlen += strlen(word);
        fonts_measure(F, cur, size, bold, &curw, NULL);
    }

    if (n < max_lines && (curlen > 0 || n == 0)) {
        snprintf(lines[n], FONT_LINE_MAX, "%s", cur);
        n++;
    }

    if (truncated || (*p != '\0' && n > 0)) {
        if (n > 0) {
            char cand[FONT_LINE_MAX];
            char tmp[FONT_LINE_MAX];
            snprintf(cand, sizeof cand, "%s%s", lines[n - 1], ELLIPSIS);
            int cw = 0;
            fonts_measure(F, cand, size, bold, &cw, NULL);
            if ((float)cw <= max_w) {
                snprintf(lines[n - 1], FONT_LINE_MAX, "%s", cand);
            } else {
                fonts_ellipsize(F, lines[n - 1], size, bold, max_w, tmp,
                                sizeof tmp);
                snprintf(lines[n - 1], FONT_LINE_MAX, "%s", tmp);
            }
        }
    }
    return n;
}
