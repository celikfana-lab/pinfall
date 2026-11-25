# PinFall — Pinterest close-up ("Pin detail") UI spec

Measured live from **nl.pinterest.com**, authenticated (imported Netscape session cookies),
headless Chrome 154 via Chrome DevTools Protocol, DPR 1, at **1920×1080** and **1440×900**.
Reference pin (numeric id): `878272364837237440` ("Shitpost / Memes 🗿", 2 comments, story-pin
variant with comments + bordered card) and `1023372715313207733` (regular pin with title,
43 comments). Numbers below are `getBoundingClientRect()` / `getComputedStyle()` of the live DOM.

The close-up is a **full page route**, not a modal: the vertical nav rail and top search bar are
fixed, the whole rest of the page scrolls, the close-up card is the first item of one page-wide
masonry, and recommendation pins fill the columns to the right of and below the card.

Reference images in `assets/ref/`:

| file | what |
|---|---|
| `closeup-1920x1080.png` | close-up page, 1920×1080 (downscaled 0.75, PNG8) |
| `closeup-1920x1080-annotated.png` | same with measured boxes + labels |
| `closeup-1440x900.png` | close-up page, 1440×900 (PNG8) |
| `closeup-1440x900-annotated.png` | same with measured boxes + labels |
| `closeup-card-1920.png` | card crop at native resolution (image + info detail) |
| `closeup-info-title-1920.png` | info-pane crop of the title/description variant |
| `closeup-tall-image-1920x1080.png` | tall image: image pane capped at `90vh - 80px`, letterboxed |
| `pinfall-closeup-1440x900.png` | PinFall's rebuilt close-up (preview data) at 1440×900, for A/B comparison |

Method: cookies loaded with `Network.setCookies`; `Runtime.evaluate` probes of
`[data-test-id]` nodes + computed styles; screenshots via `Page.captureScreenshot`;
`Emulation.setDeviceMetricsOverride` sweep of 82 viewport widths to derive the column formula.
Webapp chunks (`https://s.pinimg.com/webapp/*.mjs`) were downloaded gzip-compressed, inflated,
string-searched, then the relevant ones unpacked with

```sh
npx --yes webcrack@2.16.0 /tmp/pinjs/62759-*.mjs.js -o /tmp/wc_62759
npx --yes webcrack@2.16.0 /tmp/pinjs/app-common-react-components-auth-desktop-web-closeup-\
  CloseupBody-*.mjs.js -o /tmp/wc_closeup
```

(the `*.mjs` files had to be inflated first: webcrack cannot parse the raw gzip stream).
CSS constants quoted below are from those deobfuscated chunks. No relevant `.wasm` module was
involved in this surface, so the installed wasm tooling (`wasm2wat`/`wasm-decompile`) was not
needed.

---

## 1. App shell

Both viewports: page background `#ffffff`, no borders on header/rail (they sit on the white
page); header and rail are `position: fixed`, the document scrolls underneath.

| element | 1920×1080 | 1440×900 |
|---|---|---|
| nav rail (fixed, z672) | x0 y0 **72×1080** | x0 y0 72×900 |
| rail logo button | 11.5,16 48×48 | 11.5,16 48×48 |
| rail items (home, boards, create, bell, messages, tune) | 11.5, **88 + 72·i** 48×48 (i=0…5) | same y |
| rail settings (bottom) | 11.5, **H−64** 48×48 | 11.5, 836 |
| header (fixed, z671) | 72,0 **W−72 × 80** | 72,0 1368×80 |
| search pill | 88,16 **1736×48**, radius **12**, bg **#e5e5e0** | 88,16 1256×48 |
| search magnifier | 104,32 16×16 | 104,32 16×16 |
| search text/input | 128,16 1584×48, 16px/400 | 128,16 1104×48 |
| visual-search (lens) button | 1724,16 48×48 (24px icon) | 1244,16 48×48 |
| voice-search button | 1772,16 48×48 | 1292,16 48×48 |
| avatar | 1840,24 32×32 (in 48×48 profile box at 1832,16) | 1360,24 32×32 |
| account chevron | 1880,28 24×24 | 1400,28 24×24 |

Rules: search pill left = rail+16; pill right edge = W−96; right cluster = avatar 32 + chevron 24
anchored 16 from the right edge. Header content vertical center = 40. Rail item step is 72
(48px button + 24px gap); 24px icons inside 48px hover circles.

`header` z-index 671, `nav` z-index 672, one sticky spacer at top 80 with z 670/101 (search is
not sticky-scrolled; it is simply fixed). The **back button** is also fixed: 48×48 circle at
(96, 88) — 24px from the rail, 8px below the header — with a white background and a soft shadow;
it stays in place while the page scrolls.

## 2. Close-up card (first masonry item)

Card dimensions are derived from the masonry column grid (section 4) and from two CSS constants
found in Pinterest's JS: **min height `calc(55vh + 80px)`** and **image max height
`calc(90vh − 80px)`**. Measured result:

| element | 1920×1080 | 1440×900 |
|---|---|---|
| card (border-box) | 88,80 **1030.84×676** | 88,80 **1065.59×577** |
| card style | bg #fff, **1px solid #c8c8c1**, **radius 16**, shadow `0 2px 5px rgba(0,0,0,.06)` | same |
| image pane (`closeup-media-container`) | 89,163.1 **506.30×509.75** | 89,104.9 **523.67×527.25** |
| image pane x/width rule | x = card+1; **w = (card_inner − 16)/2** (card_inner = card_w − 2) | 523.67 = (1063.59−16)/2 |
| image vertical placement | centered: y = card.y + (card_h − media_h)/2 | 104.9 = 80+(577−527.25)/2 |
| info column | 595.3,81 **522.55×674** (= card_inner − media_w) | 612.7,81 **539.90×575** |
| info content box | x **611.3**, w **490.55** (16px horizontal padding) | x 628.7, w 507.9 |
| card bottom | 756 | 657 |

Image fit: width = media pane width, height = width × natural aspect, capped at
`min(natural_h, calc(90vh − 80px))`. When capped (tall pins) the image is **letterboxed
horizontally and centered** inside the pane (`object-fit: contain`); see
`closeup-tall-image-1920x1080.png` (natural 1920×3693 → displayed 463.9×892.5 inside the
506.3×892 pane). A tall image grows the card past the `55vh` floor: card height becomes
`max(55vh + 80, media_h) + 2`.

Computed rules (logical px, DPR scale applied by PinFall):

```
card_h      = max( 0.55*H + 80, media_h ) + 2
media_h     = min( media_w * img_h/img_w, 0.90*H - 80 )
media_w     = (card_w - 2 - 16) / 2
info_w      = card_w - 2 - media_w          /* media_w + 16 */
```

### 2.1 Info column internals (1920 / 1440)

| element | 1920 | 1440 |
|---|---|---|
| sticky top block | 611.3,81 490.55×64, padding 8 0 | 628.7,81 507.9×64 |
| action bar | 599.3,89 **502.55×48** (margin-left −12) | 616.7,89 519.9×48 |
| action items | 599.3,89 **242.42×48** | 616.7,89 244.3×48 |
| react button | 599.3,89 48×48, 24px heart | 616.7,89 48×48 |
| reaction count | 647.3,101.8 18.4×22.4, **16px/700 #000** | 664.7,101.8 |
| comment button | 681.7,89 48×48 (16px gap after count) | 701,89 |
| share button | 737.7,89 48×48 (8px gap) | 757,89 |
| more button | 793.7,89 48×48 (8px gap) | 813,89 |
| save cluster | 841.7,89 **260.13×48**, right-aligned | 859.1,89 277.5×48 |
| board dropdown | 841.7,89 187.6×48, label 16px/700 truncated + 24px chevron | 859.1,89 ~205×48 |
| Save button | 1033.3,89 **68.53×48**, radius **24**, bg **#e60023**, label 16px/500 #fff | 1068.1,89 68.53×48 |
| creator row | 607.3,153 498.55×24 (−4px outer, 4px avatar padding) | 624.7,153 515.9×24 |
| creator avatar | 24×24 circle at 611.3,153 | 628.7,153 |
| creator name | 643.3,155.2, **14px/700 #000**, lh 19.6 | 660.7,155.2 |
| divider | 611.3,201 **490.55×1 #e9e9e9** (24px below creator) | 628.7,201 507.9×1 |
| comments heading | 611.3,218 **16px/700 #000**, lh 22.4 ("4 Comments") + 16px chevron right-aligned | 628.7,218 |
| first comment avatar | 611.3,256.4 32×32 circle | 628.7,256.4 |
| comment author | 16px/700 #000 inline, then comment text 16px/400 lh≈19 **#494943** | same |
| composer container | 611.3,696 **490.55×59** (= card bottom −59 −1) | 628.7,624 507.9×59 |
| composer pill | 611.3,696 **490.55×47, radius 24, bg #fff, 1px border #c8c8c1** | same geometry |
| composer placeholder | 628.3,710, **16px/400 #919BA3** ("Add a comment") | 645.7,638 |
| composer right icons | 3× 32px circle buttons (emoji / note / image), right inset 16, gap 8 | same |

Title variant (regular pins, e.g. `1023372715313207733`): `<h1>` at 611.3,200.2,
**24px/700, line-height 26.4 (1.1), #000**, up to 3 lines; creator name 14px/700 at 643.3,153.
Description variant: `<h4>Description</h4>` 16px/700 lh 22.4 followed by 16px/400 lh 22.4 body
text; description region `max-height: calc(75vh − 80px)`.

Comments are collapsed by default to the header + the first comment row
(`canonical-card` 490.55×78.4). The collapse chevron sits at the right edge of the heading
(x=1085.8 = content right − 16).

## 3. Recommendations ("more like this") masonry

The close-up card is `data-grid-item[0]` of one page-wide masonry; recommendation tiles fill the
columns to its right and then the full width below it. Nothing is a separate panel: the same
grid, one scroll.

| property | 1920×1080 | 1440×900 |
|---|---|---|
| masonry container | data-test-id `masonry-container`: 72,80 1848×3190, `padding: 0 8px` | 72,80 1368×3322 |
| grid columns area | x **88 … 1904** (1816 wide) | x 88 … 1424 (1336 wide) |
| column count **N** | **7** | **5** |
| column width | **245.71** | **254.4** |
| gutter (h+v) | **16** | 16 |
| card column span | **4** (card 88,80 1030.84×676) | **4** (1065.59×577) |
| rec tile geometry | e.g. 1134.9,80 245.7×302 (aspect height), radius 16 | 1170,80 254.4×313 |
| page content height | 3190 (document scrollHeight 3270) | 3322 |
| first rec row y | 80 (aligned with card top) | 80 |
| recs below card start | ≈772 | ≈673 |

Rec tile: image only (organic pins have no caption/board line in this surface), rounded
`16px`, height = column width × natural image aspect. Sponsored tiles add a 64–78px footer
(brand 14px/400 + "Sponsored" 12px grey + "…"), which PinFall has no data for.

### 3.1 Column formula (measured over 82 viewport widths)

`N` changes in a strict pattern, one column per 240px of viewport:

```
N  = max(3, floor((W - 80) / 240))          /* W = viewport width */
usable = W - 104                            /* minus rail 72, minus 16px page inset x2 */
colw   = (usable - (N-1)*16) / N
```

This matches Pinterest's generic masonry helper found in `gestalt-*.mjs`
(`columnCount = max(floor(width/(columnWidth+gutter)), minCols)`, defaults
`columnWidth 236`, `minCols 3`, gutter 16) with the close-up's own width/gutter inputs.
Measured sweep (`W → N, colw, card_w, card_span`):

| W | N | colw | card w | span |
|---|---|---|---|---|
| 1000 | 3 | 288.0 | 896.0 | 3 |
| 1040 | 4 | 222.0 | 936.0 | 4 |
| 1280 | 5 | 222.4 | 937.6 | 4 |
| 1366 | 5 | 238.4 | 1001.6 | 4 |
| **1440** | **5** | **254.4** | **1065.6** | **4** |
| 1600 | 6 | 236.0 | 992.0 | 4 |
| 1728 | 6 | 257.3 | 1077.3 | 4 |
| **1920** | **7** | **245.7** | **1030.8** | **4** |
| 2000 | 8 | 223.0 | 940.0 | 4 |
| 2200 | 8 | 248.0 | 1040.0 | 4 |
| 2240 | 9 | 223.1 | 1179.5 | **5** |
| 2560 | 10 | 231.2 | 1220.0 | 5 |
| 2800 | 11 | 230.5 | 1216.7 | 5 |
| 3000 | 12 | 226.7 | 1197.3 | 5 |

Card span rule (measured): `span = N` when `N ≤ 3`; `span = 4` for `4 ≤ N ≤ 8`; `span = 5` for
`N ≥ 9` (through N=12). This corresponds to Pinterest's closeup module span config
`{md:4, lg:4, lg1:4, xl:5}` found in the chunk (xl takes over around the 2240px viewport
threshold, i.e. exactly where the measurement flips to 5).

### 3.2 "Ideas you might love" tile

A masonry tile of one column width (`245.71×248` at 1920, placed at x=873.1, y=772):
heading **"Ideas you might love"** 16px/700 lh 22.4, horizontally centered, 8px bottom margin;
then a `role=list` of 2 compact "search idea" rows. Each row: 4px vertical padding, full-column
image tile **103px** tall (≈`0.42 × colw`), radius 16, `object-fit: cover`, a **rgba(0,0,0,0.4)**
overlay, and the search phrase centered in white 16px/700 (`Weird photo`, `Goofy ahh` in the
measured case). Clicking a row opens `/search/pins/?q=<phrase>`.

## 4. Type & color tokens (computed, DPR1)

| token | value |
|---|---|
| font family | `"Pin Sans", -apple-system, Segoe UI, Roboto, … Arial, sans-serif` |
| title `<h1>` | 24px / 700 / line-height 26.4 (1.1) / #000 |
| section headings (Comments, Description, Ideas…) | 16px / 700 / lh 22.4 (1.4) / #000 |
| action counts | 16px / 700 / lh 22.4 / #000 |
| creator name | 14px / 700 / lh 19.6 (1.4) / #000 |
| board dropdown label | 16px / 700 / #000 (ellipsized with … + chevron) |
| Save label | 16px / 500 / #fff |
| comment author | 16px / 700 / lh 22.4 / #000 |
| comment text | 16px / 400 / lh≈19 / **#494943** |
| description body | 16px / 400 / lh 22.4 / #000 |
| search field + placeholder | 16px / 400 / #000 / placeholder #62625b |
| composer placeholder | 16px / 400 / **#919BA3** |
| sponsored meta / small | 12px / 400 / #62625b |
| icons | 24px line icons (stroke ≈2px) in 48px hover circles; search magnifier 16px |

Colors: page/rail/header/card `#ffffff`; card border `#c8c8c1`; search field `#e5e5e0`;
composer border `#c8c8c1`; divider `#e9e9e9`; Save/brand `#e60023` (hover `#cc001f`);
primary text `#000000` (Pinterest body ink `#211922`), comment ink `#494943`; placeholder
`#919BA3`; skeleton `#efefef`; card shadow `0 2px 5px rgba(0,0,0,0.06)`; idea-tile overlay
`rgba(0,0,0,0.4)`.

## 5. DOM / component hierarchy (trimmed)

```
nav (fixed 72)                      [pinterest-logo-home-button, home-tab, boards-tab,
                                     create-tab, bell-icon, notifications-button, tuner-tab,
                                     vertical-nav-settings-button]
header (fixed 80)                   [header-background, search-box-container > search-box-input,
                                     Lens, Voice search, header-profile, accounts-options-button]
closeup-container
└─ max-width-container
   └─ masonry-container (padding: 0 8px)
      └─ Masonry_O (role=list, 1832×3190)
         ├─ Masonry__Item idx0 (span 4, 1030.86×676)
         │  └─ gradient → closeup-body → closeup-body-style
         │     (border 1px #c8c8c1, radius 16, shadow 0 2px 5px rgba(0,0,0,.06))
         │     ├─ closeup-media-container (max-height calc(-80px + 90vh))
         │     │  └─ closeup-visual-container > img (object-fit contain, radius 16)
         │     └─ description-content-container
         │        (paddingX 16, min-height calc(80px + 55vh), max-height calc(-80px + 90vh))
         │        ├─ closeup-body-sticky-content (padding 8 0, radius 0 32 32 0)
         │        │  └─ closeupActionBar (margin-left -12)
         │        │     ├─ closeup-action-items: react-button + reactions-count,
         │        │     │   Comments, closeup-share-button, closeup-more-options
         │        │     └─ pin-better-save-button
         │        │        ├─ board-selection-item-selected > PinBetterSaveDropdown
         │        │        └─ PinBetterSaveButton
         │        ├─ creator-card-profile > creator-avatar + creator-profile-name
         │        ├─ closeup-metadata-details-divider
         │        ├─ canonical-card (collapsible-header "N Comments", comment rows)
         │        └─ inline-comment-composer-container (sticky bottom)
         │           └─ editor-with-mentions + emoji-selector
         ├─ Masonry__Item rec tiles (image only, radius 16)
         └─ Masonry__Item "Ideas you might love" (h2 + 2 search-idea image rows)
```

Card inline style (measured, abridged):

```html
<div data-test-id="closeup-body-style" class="box_k default_k paddingXNaN_1t"
     style="min-height: calc(80px + 55vh); max-height: calc(-80px + 90vh)">
  <div data-test-id="closeup-media-container" style="overflow: hidden auto;
       max-height: calc(-80px + 90vh); width: 100%">
    <div data-test-id="closeup-visual-container"><img class="img_F" …></div>
  </div>
  <div data-test-id="description-content-container"
       style="container-type: size; height: 100%; width: 100%;
              min-height: calc(80px + 55vh); max-height: calc(-80px + 90vh);
              padding: 0 16px">
    <div data-test-id="closeup-body-sticky-content"
         style="padding: 8px 0; border-radius: 0 32px 32px 0; width: 100%">
      <div data-test-id="closeupActionBar" class="box_k marginStartN300_1t">…</div>
    </div>
    …creator, divider, comments, composer…
  </div>
</div>
```

Masonry item inline style (card): `transform: translateX(8px) translateY(0px);
width: 1030.86px; height: 676px;` inside a `Masonry_O` of width 1832 (`padding: 0 8px`),
i.e. first column at x = 72 + 8 + 8 = 88.

## 6. Evidence from Pinterest's own JS (webcrack)

Chunks are ES modules under `https://s.pinimg.com/webapp/`. Decompressed with gzip, the
close-up layout constants live in chunk `62759-1d40526ab2b7adf4.mjs` (and duplicates
`85552-*`, `29618-*`); `CloseupBody`/`CloseupActionBar`/`DefaultMetadata` in
`app-common-react-components-auth-desktop-web-closeup-*`; `CloseupTitle` in
`app-www-closeup-duplo-CloseupTitle-*`.

```js
// 62759-1d40526ab2b7adf4.mjs (deobfuscated)
Ii: "calc(55vh + 80px)",        // close-up card min height
gr: "calc(90vh - 80px)",        // image pane max height
t4: "calc(75vh - 80px)",        // description max height
En: { boxShadow: "0 2px 5px rgba(0, 0, 0, 0.06)",
      transition: "box-shadow 200ms ease-in-out" },
CD: { md:4, lg:4, lg1:4, xl:5 },                 // closeup module column span
IO: { columnSpan:{sm:2,md:2,_lg1:2,lg:2,xl:2} }, // inclusive-filters module
BQ: ({verticalNavWidth:e}) => ({ BREAKPOINT: 1056+e,
      BACKBUTTON_WITH_LABEL_WIDE_BREAKPOINT: 1340+e,
      SM_BREAKPOINT: 680+e, EXP_SM_BREAKPOINT: 814+e })
```

```js
// gestalt-*.mjs — responsive breakpoints and masonry column math
{ sm:576, md:768, lg:1312, xl:1440, xxl:1680, xxxl:2048 }
function columnCount({gutter, columnWidth, width, minCols}) {
  return Math.max(Math.floor(width / ((columnWidth ?? 236) + (gutter ?? 14))), minCols);
}
// Masonry.defaultProps = { columnWidth: 236, minCols: 3, layout: "basic" }
```

`outerPaddingSize: 16`, `closeupActionBarMaxWidth: 488` (action-bar/title pane cap), and
`descriptionPaneWidth` are `CloseupBody` props; the rendered DOM shows the min/max-height pair
above and `masonry-container { padding: 0 8px }`, matching the measurements.

i18n/UI strings confirmed from the rendered DOM and bundles: **"Save"**, **"Description"**,
**"Ideas you might love"**, **"Add a comment"**, **"N Comments"**, **"Select a board to save
to: …"**, **"View larger"**, **"Search image"**, **"Visit site"**, **"AI modified"**,
**"Sponsored"**. No separate close-up layout language beyond the constants above: the surface
is composed from the generic masonry + the closeup module span/minheight/shadow tokens.

## 7. What PinFall implements 1:1 vs approximated

1:1 (same numbers at 1920 and 1440, DPI-scaled):
* fixed 72px rail + 80px header shell, search pill geometry/radius/color, right cluster,
  fixed 48px back button at (rail+24, header+8);
* card border-box geometry, radius 16, 1px `#c8c8c1` border, soft shadow, image/info split
  `(inner−16)/2`, image vertical centering, contain-fit with `90vh−80px` cap, card height
  `max(55vh+80, media_h)+2`;
* info column: action bar (48px buttons, 24px icons, 16px/700 counts), board dropdown with
  ellipsized 16px/700 label + chevron, Save pill 68.5×48 r24 `#e60023`, creator row, divider,
  comments heading/rows, composer pill 47px r24 white with 1px border and `#919BA3`
  placeholder;
* masonry column count/width/gutter/radius, 4-column card span (5 from 2240px), recs to the
  right of and below the card, whole page scrolling under fixed header/rail;
* "Ideas you might love" tile: centered 16px/700 heading + two 103px-high overlay tiles;
* type sizes/weights/line-heights and the color tokens above.

Approximated (documented deviations):
* **Font**: Pinterest ships the proprietary "Pin Sans"; PinFall uses bundled DejaVu Sans. Sizes
  and weights are matched, glyph widths differ slightly, so wrapped line counts can differ.
* **Icons**: 24px outline glyphs are hand-drawn with SDL primitives (home/boards/create/bell/
  chat/tune/settings/search/lens/mic/chevron/heart/comment/share/more/expand) — same size,
  stroke and color, not pixel-identical SVG paths.
* **Avatars**: initial-letter circles (brand-soft fill); no avatar image is fetched by the net
  layer, so creator/comment avatars are monogram stand-ins.
* **Comments**: rendered as an independently scrollable region inside the card (heading +
  rows) instead of Pinterest's collapse/expand animation; no reply/like-on-comment actions.
* **Recommendations**: reuse the existing `related` feed (up to 24 pins/page) — a fixed set, no
  infinite recs beyond that page; no sponsored tiles (no ad data). Guest/`e1_…` sessions get a
  graceful "related pins unavailable" note instead of the rec grid.
* **Extras not implemented**: visual-search/lens, "View larger", comments emoji picker, pin
  reactions menu beyond like, share sheet (share still copies the link), AI-modified/Visit-site
  pills, carousel/product modules, A/B variants. Decorative equivalents toast a message.
* **Page scrollbar**: thin custom scrollbar at the right edge instead of the browser overlay
  scrollbar.
* Pinterest serves several close-up variants (story-pin card with comments + bordered card,
  product-pin card without border, etc.). PinFall implements the story-pin variant (the one in
  the user's reference and the task description: bordered card, comments, composer, Save) with
  one deliberate merge: the story-pin variant hides the inline title (it puts the pin title in
  the Save dropdown); PinFall always renders the regular-pin 24px/700 title block because the
  user's spec asks for "author, title, Description, composer".
* Multi-line comments stack the wrapped text under the author name; a comment that fits on one
  line is drawn inline after the bold author (`**Samuel** que lindo`), as Pinterest does.

## 8. Files & verification

* `src/theme.h` — shared tokens (feed) plus close-up surface tokens (`cu_*`).
* `src/ui.c` — `app_detail_geom()`, `app_detail_measure()`, `app_draw_detail()`, the `cu_*`
  drawing/hit-test helpers and the close-up event routing implement this spec; the feed and
  its masonry shell are unchanged (only the close-up route is replaced).
* `docs/UI_CLOSEUP.md` — this document.
* `assets/ref/closeup-*.png` — Pinterest references listed at the top; plus
  `pinfall-closeup-1440x900.png`, PinFall's rebuilt surface.

Verification performed for this spec's implementation:

```sh
make -B all                                   # warning-clean
SDL_VIDEODRIVER=offscreen ./pinfall --selftest --auth-preview --prefix /tmp/pf_st
SDL_VIDEODRIVER=offscreen ./pinfall --selftest --prefix /tmp/pf_guest
SDL_VIDEODRIVER=offscreen ./pinfall --selftest --cookie-file .session-cookies.txt \
    --prefix /tmp/pf_real
# all three: PASS (close-up actions/fullscreen/image/scrolls/comments/related/
#             like/composer/empty/guest)
SDL_VIDEODRIVER=offscreen ./pinfall --snapshot /tmp/v.bmp --detail 2 --auth-preview \
    --size 1440x900 --detail-tab related
# offscreen BMPs rendered at 1440x900 and 1920x1080 for default / comments /
# related / guest close-up states (all 0-warning builds)
```

Deviations are listed in section 7; the most visible are the bundled DejaVu font instead of
Pinterest's proprietary "Pin Sans", hand-drawn 24px icons, and monogram avatars.
