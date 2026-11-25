# PinFall — UI reverse-engineering spec

Source: **nl.pinterest.com** guest web UI, measured 2026-10-27 with headless Chrome 154
(1920×1080 and 1440×900, DPR 1) via Chrome DevTools Protocol: `getBoundingClientRect()`,
`getComputedStyle()` on the live DOM, CSS custom properties from the inlined stylesheet,
and pixel sampling of screenshots. Machine-readable mirror: **`docs/UI_TOKENS.json`**
(parses; all numbers below are duplicated there for the C dev to paste as constants).

Reference screenshots in `assets/ref/`:

| file | what |
|---|---|
| `search-grid-1920x1080.png` | `/search/pins/?q=minimalist bedroom` at 1920×1080 — 8-column masonry, 221px cards, header + filter chips (login modal removed). Stored downscaled to 1440×810 and PNG8-quantised to fit the 400KB/asset budget. |
| `search-grid-1440x900.png` | same URL at 1440×900 → 5 columns, 267.2px cards (full resolution) |
| `search-grid-1024x768.png` | same URL at 1024×768 → 4 columns, 234px cards (full resolution) |
| `home-logged-out-1920x1080.png` | logged-out home: 80px header, 48px/900 hero, "More winning ideas" 36px/900 header + 4-up idea cards (full resolution) |

All four are PNG8 (≤128 colours) for size; total `assets/ref/` = 1.1MB.
No pin close-up screenshot exists — the surface is login-gated (see below).

Two blockers hit while capturing (documented, not worked around):
* **Pin close-up is login-gated.** `https://nl.pinterest.com/pin/<id>/` returns 302 → `/ideas/`
  for guests (desktop UA and Android mobile UA both), and clicking a gated grid pin produces a
  "You are signed out" wall without changing the URL. The **detail-view layout section below is a
  reconstruction**, explicitly marked `(est.)`.
* **Guest search grid hides pin captions and hover overlays.** Cards are image-only; a mouse-move
  over a card produced a pixel-identical screenshot (no Save button, no scrim). Card/title/hover
  metrics are taken from Pinterest's logged-in card + idea-card styling, marked `(est.)`.

---

## 1. Global layout (search results page)

```
y=0    ┌──────────────────────────────── header (fixed, 80px, #fff) ───────────────┐
y=80   ├── filter chip row (70px: 48px chips + 5px top offset + 12px bottom pad) ──┤
y=150  └──────────────────────────────── (white gap 12px) ─────────────────────────┘
y=162  ┌────────────────────────────── masonry feed ──────────────────────────────┐
       │ cards inset 20px left/right, 16px gutters, 16px vertical gap             │
       └──────────────────────────────────────────────────────────────────────────┘
```

* Header is `position: fixed; z-index: 671`, background `#ffffff`, no bottom border.
* Feed scrolls in an **inner scroll container**; the document itself does not scroll
  (`document.scrollingElement.scrollHeight == 1080` at 1080 viewport). Header and chip row stay put.
* Feed top = **162px** with chip row; **96px** (80 + 16) without a chip row.
* Horizontal page inset for cards = **20px** each side (masonry container adds 4px + 8px padding,
  column track adds 8px → first card at x=20, last card right edge at W−20).

## 2. Masonry grid — exact numbers

Measured by reading each `[data-grid-item]` inline `transform: translateX(..) translateY(..)` and width,
at 13 viewport widths. **Gutter is always 16px**, first card x=20, vertical gap 16px.

| viewport W | columns | card width | card height | gutter |
|---|---|---|---|---|
| 1920 | **8** | **221.0** | image aspect (294–420 observed) | 16 |
| 1728 | 7 | 227.4 | — | 16 |
| 1600 | 6 | 246.7 | — | 16 |
| 1440 | **5** | **267.2** | image aspect | 16 |
| 1366 | 5 | 252.4 | — | 16 |
| 1280 | 5 | 235.2 | — | 16 |
| 1200 | 4 | 278.0 | — | 16 |
| 1100 | 4 | 253.0 | — | 16 |
| 1024 | **4** | **234.0** | image aspect | 16 |
| 900 | 3 | 276.0 | — | 16 |
| 800 | 3 | 242.7 | — | 16 |
| 600 | 2 | 272.0 | — | 16 |

### Exact rules (verified against all 13 measurements)

```c
// number of columns
N = (int)ceil((W - 260.0) / 236.0);   if (N < 2) N = 2;
// card width
cardW = (W - 40.0 - (N - 1) * 16.0) / N;
// column x
x(i) = 20 + i * (cardW + 16);
```

Sanity checks: 1920 → N=8, cardW=221.0 · 1440 → N=5, cardW=267.2 · 1024 → N=4, cardW=234.0.
Masonry placement: shortest column first (`y = min(column heights)`, then `column height += cardH + 16`).

Card heights: height = width × image aspect ratio (no crop). Observed aspects 1.33:1 … 1.90:1.
Aspect-while-loading placeholder is **1.5:1** (`padding-bottom: 150%`, bg `#efefef`).
Recommend capping very tall images at ~2.2:1 in PinFall.

## 3. Header — geometry

At **1920×1080** (same at 1440 except right cluster position):

| element | x | y | w | h |
|---|---|---|---|---|
| header | 0 | 0 | 1920 | **80** (padding 16) |
| logo (P mark + "Pinterest" wordmark, SVG 98×18) | 28 | 29.5 | 98 | 18 |
| "Explore" nav link | 138 | 16 | 88 | 48 |
| **search pill** | **242** | **16** | **1441.8** | **48** |
| "Log in" secondary button | 1699.8 | 16 | 76.2 | 48 |
| "Sign up" primary button | 1784 | 16 | 88 | 48 |
| chevron button | 1872 | 16 | 32 | 48 |

Right cluster is anchored to the right: `login.x = W − 220.2`, `signup.x = W − 136`,
`chevron.x = W − 48`, right margin 16. Search pill right edge = `W − 236` (16px gap to Log in).
At 1440: search pill 242→1203.8 (w 961.8), Log in at 1219.8. At 1024 left cluster widens
(search x=272, w=515.8, Log in at 803.8); "Explore" link is hidden below ~1100.

* Search pill: **radius 24** (full pill), bg `#e5e5e0`, height 48, inner padding 16 left / 8 right,
  16px magnifier icon, 8px icon→text gap, 16px text (weight 400, color `#000`), cancel ✕ 16px,
  placeholder `#62625b`.
* Search field width formula: `pillW = (W − 236) − 242`.
* Buttons: height 48, **radius 16**, label 16px/500. Primary bg `#e60023` / hover `#cc001f` /
  pressed `#b2001b`, white text; secondary bg `#e5e5e0`, black text, hover `#dadad3`.
* Header nav links ("Explore", "About", …): 16px / weight 500 / `#000`.
* Home/ideas variant of the header differs: nav links About / Businesses / Create / News sit before
  the buttons, and the button colours are swapped — **Log in is the red primary pill** (x=1731.8,
  76.2×48, `#e60023`) and **Sign up is the gray secondary** (x=1816, 88×48, `#e5e5e0`). The
  search-results header (Sign up red) is the canonical PinFall header; keep one variant.

## 4. Filter chip row (search page, optional for PinFall)

y=80, height 70; chips are 48 tall at y-offset +5, **radius 16**, gap 8, pad-x 16, label 16px/600.
First chip x=96 (an icon filter button 48×48 at x=40). Bottom 18px is a white gradient fade
(30px tall, `rgba(255,255,255,0) → #ffffff`). Chips carry pastel backgrounds from Pinterest's
palette: green `#83ddad`, blue `#92a4ff`, purple `#fbdfff`, orange `#ffb380`, red `#ff8f8f`.

## 5. Pin card

* Corner radius **16px**, applied to the image wrapper (image itself radius 0, `overflow:hidden`).
* **Card padding 0** — image is flush with the card box; no border, **no shadow** (flat).
* Image placeholder / letterbox color `#efefef`.
* Vertical gap between cards **16px** (same as horizontal gutter). Feed cards touch nothing else.
* **Title clamp 2 lines** (est. from Pinterest card/title style): 16px / weight 700 / line-height 1.4
  (22.4px), color `#211922`, ellipsis on overflow, `-webkit-line-clamp: 2`. Caption block:
  8px gap under image, 4px horizontal padding, 0 bottom padding. Guest grid shows no caption at all.
* Hover (est.): 25% black wash over image + red "Save" pill 40px tall, radius 20, pad-x 16,
  16px/700 white, top-right at 8/8; 80px top gradient `rgba(0,0,0,0.40)→transparent`; cursor pointer.
  Guest surface has no hover overlay (verified pixel-identical hover screenshot).
* Card hover scale: none (no zoom on web).

## 6. Color system

| role | hex | source |
|---|---|---|
| background / header | `#ffffff` | computed |
| search field, secondary btn | `#e5e5e0` | computed (search pill) |
| hover surface | `#f6f6f3` | token `--sema-color-hover-background-default` |
| pressed surface | `#efefeb` | token |
| image placeholder | `#efefef` | computed `rgb(239,239,239)` |
| text primary | `#211922` | computed body color (classic Pinterest) |
| text strong / headings | `#000000` | computed headings/buttons |
| text secondary / muted | `#62625b` | token `--sema-color-text-subtle` |
| legacy muted | `#767676` | classic Pinterest gray |
| border light (dividers) | `#efefef` | classic |
| border | `#cdcdcd` | classic |
| brand red | **`#e60023`** | `--base-color-pinterest` |
| brand red hover | `#cc001f` | `--base-color-hover-pinterest` |
| brand red pressed | `#b2001b` | `--base-color-pressed-pinterest` |
| link blue | `#2b48d4` | `--sema-color-text-link` |
| **scrim (modal behind)** | **`rgba(0,0,0,0.60)`** | derived: white 255 → 102 in dimmed screenshot |
| hover image overlay | `rgba(0,0,0,0.25)` | est. |
| grayscale ramp | `#fbfbf9 #f6f6f3 #e5e5e0 #c8c8c1 #91918c #74746c #62625b #494943 #33332e #242421 #181816 #000000` | `--base-color-grayscale-25…500` |
| focus ring | `#0074e8` | accessibility outline (est.) |

Pinterest's full token sheet (2642 custom properties) lives in the inlined `<style>`; the above are
the only ones the guest UI actually paints.

## 7. Typography

* Family: **"Pin Sans"** (Pinterest custom; `Pin-Sans-MacOS-{Regular,Medium,Bold,Heavy}.woff2`).
  Fallback stack measured: `-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen-Sans,
  "Helvetica Neue", Helvetica, Arial, sans-serif`.
* Available locally for SDL2_ttf: **Liberation Sans** (`/usr/share/fonts/truetype/liberation/`)
  is metrically closest; **DejaVu Sans** is the contract-guaranteed fallback but ~8% wider.
  Use Liberation Sans regular + bold; emulate weights 700/900 with bold.

| role | px | weight | line-height | notes |
|---|---|---|---|---|
| header nav / buttons | 16 | 500 | 19.2 | measured |
| search input | 16 | 400 | 19.2 | measured |
| filter chip | 16 | 600 | — | measured-ish |
| pin card title | 16 | 700 | 22.4 | est. (idea cards measured 16/700) |
| pin description | 16 | 400 | 1.4 | measured body style |
| section header ("More winning ideas") | **36** | **900** | 39.6, ls −0.5 | **measured** |
| home hero title | 48 | 900 | 52.8, ls −0.5 | measured |
| close-up pin title | 28 | 700 | 30.8, ls −0.5 | est. |
| meta ("59 Pins · 3mo", board name) | 12 | 400 | 18 | measured |
| meta bold variant | 14 | 600 | — | est. |
| body default | 12 | 400 | 1.33 | computed |

Note Pinterest has a true **weight 900 (Heavy)** for big headings; SDL2_ttf only has regular/bold
here, so bold at the same sizes is the right approximation.

## 8. Detail / pin close-up layout — RECONSTRUCTED (est.)

Not reachable as a guest (see blockers). Reconstruction from Pinterest's standard desktop close-up,
consistent with the measured surface language (radius 16, red primary, scrim 0.6, title style):

```
viewport
┌─────────────────────── scrim rgba(0,0,0,0.60) ──────────────────────────┐
│  (×) 48px white circle, at 16,16                                        │
│              ┌──────────────── panel, max-width 1016, radius 16 ───────┐│
│              │  image pane 508px (50%)     │  info column 508px (50%)  ││
│              │  radius 16 left corners     │  padding 32               ││
│              │  bg #efefef, object-fit     │  title 28/700, ls −0.5    ││
│              │  contain, max-height 90vh   │  desc 16/400 #211922      ││
│              │                             │  board row 14/600         ││
│              │                             │  Save pill 48h r24 #e60023││
│              │                             │  icon row 48×48 hover #f6f6f3│
│              └─────────────────────────────┴───────────────────────────┘│
└──────────────────────────────────────────────────────────────────────────┘
```

* Proportions: **image pane ≈ 50% / info column ≈ 50%** of a 1016px-max panel; panel is vertically
  centered with ≥16px margins; image pane never taller than 90vh (972px at 1080).
* Close button 48×48, radius 24, white bg, black ✕; Esc also closes.
* Info column order: title → description → board/pinner attribution → primary red Save → circular
  action buttons (share, more). Source/domain link: 14px/600 `#211922`, underline on hover.
* PinFall may simplify to a two-pane full-screen view: left image letterboxed on `#efefef`,
  right 480–520px info column on `#fff`, red "Open" button — the same proportions read correctly.

## 9. Interaction affordances worth cloning

1. **Click card → close-up modal** over 60% scrim; URL becomes `/pin/<id>/`; Esc/× returns.
   Preserve scroll position of the grid behind the modal.
2. **Hover card**: 25% dark wash + red Save pill (est.). Even without saving, the wash communicates
   interactivity; PinFall should show a scrim + "Open" pill on hover.
3. **Header search**: pill field, Enter submits, cancel ✕ clears; results replace the grid and
   scroll resets to top. Typing shows inline ✕; focus ring on the pill.
4. **Infinite scroll**: fetch next page when scrollY > contentHeight − 2×viewportHeight. No pager.
   Append with masonry shortest-column placement, never re-flow existing cards (keeps scroll stable).
5. **Skeleton/placeholder**: while a thumbnail loads, a `#efefef` rounded rect at 1.5:1 holds the
   slot (Pinterest animates a shimmer; a static fill is fine at our scale).
6. **No image zoom on web** — don't implement zoom-on-hover.
7. Keyboard: Esc closes detail, ↑/↓ not used in grid; Ctrl+F style browser find irrelevant.

## 10. Scroll behavior & scrollbar

* Inner scroll container under the fixed header; header + chip row do not scroll.
* Headless Chrome uses **overlay scrollbars → 0 layout px** (measured 0 at every width, with and
  without `--hide-scrollbars`). All widths above are therefore pure content widths. A normal Linux
  desktop Chrome draws a ~15px classic scrollbar and would subtract it from the viewport.
* Pinterest CSS sets **no** `::-webkit-scrollbar` rules (grep: 0 matches) → default OS scrollbar.
* Recommendation for PinFall (SDL2 has no native scrollbar): draw a thin custom one:
  8px wide, track transparent, thumb `rgba(0,0,0,0.20)` (hover 0.35), radius 4, 2px from right edge,
  min thumb height 40px, fade out after ~1s idle; full-height minus header. Wheel step ~120px,
  momentum optional.

## 11. SDL2 implementation guidance

**Cheap, do these:**
* Solid `SDL_RenderFillRect` fills for header, pill, buttons, placeholders (`#efefef`).
* Rounded rects via 4 corner alpha masks or pre-baked corner sprites: **r=16** cards/buttons,
  **r=24** search pill and detail close button. Cache masks; never per-frame per-pixel rounding.
* SDL2_ttf text with 2 fonts (regular + bold); sizes 12/14/16/28/36.
* 1–2px dividers as thin filled rects `#efefef`.
* Detail scrim: `SDL_SetRenderDrawBlendMode(BLENDMODE_BLEND)` + fill `rgba(0,0,0,153)`.
* Hover overlay: flat 25% black wash (fill over the thumbnail texture region) + solid red Save pill.
  No shadows are needed anywhere — Pinterest cards are flat.
* Red logo "P": filled circle `#e60023` + white "P" glyph, or just the circle; wordmark optional.

**Skip or fake:**
* backdrop blur / frosted glass → flat scrim or opaque white panel.
* box-shadows (cards have none) → nothing.
* shimmer skeletons → static `#efefef` fill.
* vector logo/icon fonts → 2–3 hand-drawn primitives or tiny PNG sprites.
* smooth corner antialiasing on every card at 16px radius is fine with masks; don't do supersampling.

**Performance notes:**
* One `SDL_Texture` per thumbnail, decode at ≤236px width (API already gives 236x/474x).
  Cap the cache at 64MB; LRU-evict textures for cards scrolled far outside the viewport.
* Lazy upload: only create the texture when the card first enters (viewport + 1 screen of margin).
* Cache the scaled texture at the current card width (221/267/234) — don't scale per frame.
* Prefetch the next API page when scrollY > contentH − 2×viewportH; keep 25 pins/page.
* Recompute masonry layout only on resize/append, not per frame; draw back-to-front with one
  target texture or direct blits, target 60fps at 1920×1080.

---

## Appendix — measurement provenance

* Grid: `[data-grid-item="true"]` inline `transform:translateX() translateY()` + `width`, container
  `padding: 0px 8px`, role=list width `W−24`.
* Header/search/chips/buttons: `getBoundingClientRect()` + `getComputedStyle()` (font, radius, bg).
* Scrim alpha: same pixel sampled in dimmed vs undimmed screenshots; `255·(1−α)=102 ⇒ α=0.60`
  (confirmed on a second pixel: `221 → 88.4`).
* Colors: `--base-color-*` / `--sema-color-*` custom properties resolved to hex, plus computed
  `backgroundColor` / `color` values.
* Full raw metrics: `/tmp/uir/search-metrics.json`, `deep-metrics.json`, `home-extra.json`
  (ephemeral; numbers are all inline above).
