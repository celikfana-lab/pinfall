# PinFall — Pinterest **mobile web** UI spec (412 × 915)

Reverse-engineering brief: `task-27`. This document is the mobile counterpart of
`docs/UI.md` (desktop) and `docs/UI_CLOSEUP.md` (desktop close-up), and the source
of truth for the Android (432 logical-px) parity work.

| | |
|---|---|
| Source | `https://nl.pinterest.com/` — **signed-in** account `yourhandle` (handle redacted) |
| Capture date | 2026-10-31 |
| Browser | Google Chrome 154.0.8037.97, `--headless=new`, DevTools Protocol |
| Emulation | `Emulation.setDeviceMetricsOverride(412×915, DPR 2.625, mobile=true)`, touch 5 points, Chrome-for-Android UA on Pixel 7 (Android 14) |
| Viewport actually rendered | **412 × 915 CSS px** (`visualViewport.scale == 1`), screenshots at 1082 × 2402 device px |
| Language | nl (UI strings observed: "Home, Search, Inbox, Saved, Sponsored, Visit site, More to explore, What do you think?, Add a comment, Organize, Created") |
| Read-only | Every non-GET request was aborted at the CDP **Fetch** layer (see §10). No save / like / comment / board write was ever sent. |

Reference screenshots: **`assets/ref/mobile/`** (10 PNGs, 618 × 1372, PNG8 ≤ 192 colours):

| file | what it shows |
|---|---|
| `01-home-feed.png` | Home feed, top: sticky category chips, 2-column masonry, image-only pins, "…" pin menu, bottom nav with Inbox red dot |
| `02-home-feed-scrolled.png` | Home feed scrolled, sponsored/advertiser card block |
| `03-search-results.png` | Search results header (search field + guided colour chips) and 2-column grid |
| `04-search-video-duration-badge.png` | Video pin with white `0:11` duration pill, colour chips |
| `05-pin-closeup.png` | Pin close-up: floating back button, image, action row, red Save pill, pinner/title/description, Visit site |
| `06-pin-closeup-info.png` | Same, scrolled: info block, More to explore + related grid |
| `07-comments-sheet.png` | Comments bottom sheet (`What do you think?`, empty state, composer) |
| `08-card-options-share-sheet.png` | Card "…" share/options sheet, rows Save / Open in app / Download image / See more / See less / Report |
| `09-profile-saved.png` | Saved/profile tab: header, 120 px avatar, Created/Saved tabs, board grid |
| `10-closeup-related-backtotop.png` | Close-up related grid + floating "Back to top" pill above the bottom nav |

> **No top app bar on mobile home.** The 2026 mobile-web home is an immersive feed: only a
> 60 px sticky category-chip strip; there is **no logo, avatar or search field** on top, and
> **no Create tab** in the bottom nav (4 items only). Search lives in the bottom nav and in the
> 72 px headers of Search/Profile.

---

## 0. TL;DR — the numbers to paste (412 CSS px, 1× = logical px)

```
viewport             412 × 915, DPR 2.625, env(safe-area-inset-*) = 0 on all sides
page background      #FFFFFF     primary text #000000      secondary #62625B
brand red            #E60023     link/mention #2B48D4     input border #91918C
neutral chip/button  #E5E5E0     active underline #33332E scrim rgba(0,0,0,.50)

bottom nav           412 × 68.5  (y = 846.5)   items 79.19 × 44.5 @ x 17.91+99n
                     icon 24, label 11/700 (16.5 line), active #000, rest #62625B
                     badge 8 × 8 #E60023 r=999 at icon top-right

home chips bar       60 tall (sticky, top 0); chip 48 tall, radius 16, text 16/500, pad 12,
                     active underline 2 px #33332E (text width)

masonry              2 columns, card 201, gutter 1 px h+v, page inset 4, card pad 4
                     image 193 wide, radius 16, height = 193 × (naturalH/naturalW)
                     "…" button 24 × 24 (glyph 16) right 6, bottom 10
                     (no pin titles/captions in the feed; ads add a 12 px advertiser block)

close-up             image 388 × aspect @ (4,4), radius 16; back 48 × 48 white r16 @ (8,8)
                     action row y 408: react 32, comment/send/more 40, Save 68.53 × 48 r16
                     title 16/700 lh22.4 · desc 16/400 lh22.4 · hashtag 14/400 #2B48D4
                     Visit site 396 × 48 r16 #E5E5E0; More to explore 16/700
                     back-to-top pill 116.23 × 48 r16 @ right 8 above the nav

sheets               white, radius 32 32 0 0, scrim 50 %, grabber 37 × 5 #E5E5E0 r28,
                     close 48 × 48, comments sheet 412 × 795.25 (≈ 87 % of the viewport), composer 388 × 48 r32

profile header       412 × 72.39: search 304 × 48.39 r16, create + 48, settings 48
                     avatar 120 circle, name 36/700, username 16/400 #62625B
                     tabs 48 (underline 2), board grid 192 × 204 cards r8, gap 10 / 2
```

---

## 1. Viewport, scrolling and safe areas

* The page lays out exactly at **412 × 915**; `innerWidth/innerHeight` = 412/915,
  `devicePixelRatio` = 2.625, `visualViewport` = 412 × 915 @ scale 1.
* `env(safe-area-inset-top/right/bottom/left)` all resolve to **`0px`**. The page therefore adds
  **no** safe-area padding: the sticky header starts at `y = 0`, the bottom nav ends at
  `y = 915`. On a real Android browser the system status bar / gesture bar live *outside* the
  page viewport; the page never compensates for them. (A native SDL app must manage these
  itself — see §11.2.)
* The document scrolls (there is no inner scroll container on the mobile surfaces measured):
  `document.scrollingElement.scrollHeight` examples — home 3544 px, search 1206 px (SSR slice
  only, see §10), close-up 2186–3625 px depending on related pins.
* Sticky/fixed inventory: home category bar (`position: sticky; top: 0; z-index: 1`),
  search header (`sticky; top: 0; z-index: 2`), bottom nav (`fixed; z-index: 3`),
  close-up floating back button, close-up hidden "Visit site" slide-in bar
  (`fixed; z-index: 4; transform: translateY(64px)`), comments/save sheets (`z-index: 6`),
  "Back to top" pill (appears after scroll).
* No pull-to-refresh indicator was observed in headless; text selection was not evaluated.

---

## 2. Home feed

```
y=0    ┌──────────────── chips scroller (sticky, 60 px, #fff) ───────────────────┐
y=8    │  [All]  [vocaloid stickers]  [Dingen om te kopen]  [art] →  …           │  chip 48
y=56   └──────────────────────────────────────────────────────────────────────────┘
y=60   ┌──────────────────── 2-column masonry (x=4, 404 wide) ────────────────────┐
       │ ┌ card 201 ─────────────┐ 1px ┌ card 201 ─────────────┐                 │
       │ │ ┌ image 193 r16 ────┐ │ gap │ │ ┌ image 193 r16 ────┐ │                 │
       │ │ │                   │ │     │ │ │                   │ │                 │
       │ │ └───────────────────┘ │     │ │ └───────────────────┘ │                 │
       │ │                    …  │     │ │                    …  │                 │
       │ └───────────────────────┘     │ └───────────────────────┘                 │
       └──────────────────────────────────────────────────────────────────────────┘
y=846.5┌──────── bottom nav 412 × 68.5, 4 items (fixed, z3) ───────────────────────┐
y=915  └──────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Top area — category chips (no app bar)

* Container: full width, **height 60** (chips at `y = 8`, height 48, bottom gap 4). White
  background, `position: sticky`, `top: 0`, `z-index: 1`, no border/shadow.
* Chip: first at `x = 8`; height **48**; **horizontal padding 12**; **radius 16**; background
  transparent; label **16 px / 500**, black. Chip hit boxes are **adjacent (no gap)** — the
  visible 24 px between labels is the two 12 px paddings. The row scrolls horizontally
  (chips continue past the viewport).
* Active chip: **2 px underline `#33332E`** at the chip's bottom (`y = chip.y + 38`), width equal
  to the label width (19.05 px for "All"), centred under the text. Inactive: no underline.
* Category names are user/recommendation-specific (`All`, `vocaloid stickers`, `Dingen om te
  kopen`, `art`); first chip is always `All`.
* There is no logo, avatar, search field, notification bell or Create button on this bar.

### 2.2 Masonry — exact tokens

Measured on 8 SSR cards (no pagination under the read-only guard):

| token | value |
|---|---|
| columns | **2** |
| grid box | `x = 4`, `w = 404` |
| card width | **201** (column track) |
| card gap | **1 px** horizontal and vertical |
| card inner padding | **4 px** (image is inset 4 from the card box) |
| image width | **193** |
| image height | `193 × naturalH / naturalW` (observed 166.9–289.5) |
| image radius | **16** (`border-radius: 16px; overflow: hidden` on a wrapper) |
| card background | `#FFFFFF` |
| card height | `4 + imageH + 4 + 24 + 10` (image, 4 gap, "…" row 24, bottom pad 10) |
| "…" pin menu | **24 × 24**, glyph 16, transparent bg, 6 px from card right edge, 4 px under the image, 10 px card bottom |
| title/caption | **none** — feed cards are image-only |
| row pitch | previous card bottom + 1 |

First card top = **60** when no privacy banner is present. Columns: card A left = 4.5,
card B left = 206.5 (1 px gap). `object-fit: fill` with an exact aspect box is used by
Pinterest; the important part is the aspect-correct box, no letterbox.

### 2.3 Sponsored / promoted cards

Ads are normal cards plus an advertiser block under the image (no badge, no border, no tint):

* `Nike` (advertiser) — **12 px / 400**, line-height 14, colour `#000000`, 8 px below image,
  left 2 px inside the card's 4 px padding.
* `Sponsored` — **12 px / 400**, line-height 14, colour `#62625B`, 4 px under the advertiser line.
* The "…" button overlays the right side of that block (right 6, vertically centred on the
  32 px block).
* Same image width/radius; card is ~34 px taller than an image-only card with the same image.
* The advertiser line truncates with ellipsis (161 px available). The ad card has no title.

### 2.4 Video pins

* The feed image is a `<video>` (muted, autoplay, loop); there is no play button overlay.
* Duration pill, top-left **8 px inset** from the image box: **40.14 × 24** (e.g. `0:11`),
  **radius 8**, background `#FFFFFF`, padding `4px 6px`, text black (~16 px / 500 computed;
  the pill is inside a scaled overlay so it reads ≈ 11 px visually — use 12 px/500 in native).
* The "…" button and card radius are identical to image pins.

### 2.5 Bottom navigation

* Container: **412 × 68.5** at `y = 846.5`, white, `position: fixed`, `z-index: 3`,
  no border, no shadow. Internal: 8 px top pad, 44.5 px item row, 16 px bottom pad.
* 4 items, in order: **Home** (`/`), **Search** (`/search/`), **Inbox** (`/notifications/`),
  **Saved** (`/<username>/`). There is **no Create "+" item** on mobile web.
* Item box: **79.19 × 44.5**, left edges at **17.91, 116.91, 215.91, 314.91** (gap 19.81,
  side margin 17.91). Press highlight: child div radius 12.
* Icon: **24 × 24**, viewBox `0 0 24 24`. Label **11 px / 700**, line-height 16.5, 4 px under
  the icon. Active item: icon fill + label `#000000`. Inactive: `#62625B` for both.
* Inbox unread badge: **8 × 8**, `#E60023`, radius 999, at the icon's top-right
  (icon x + 24, icon y + 4); present on Inbox when unread.

### 2.6 Scroll behaviour

* The chips bar stays pinned while the masonry scrolls beneath it.
* Pins load incrementally in the real product; under the read-only guard only the SSR slice is
  present (8 cards), so the feed's `scrollHeight` is finite.
* Feed cards are `<a href="/pin/<id>/">` links, so a tap navigates (GET) to the close-up; tapping "…" opens the card options/share sheet
  (§5.3) without navigating.

---

## 3. Search

```
y=0    ┌──────────────── sticky header 412 × 150.39, #fff, z2 ─────────────────────┐
y=24   │  ┌ search field 396 × 48.39, r16, 1px #91918C ─────────────────────────┐  │
y=72   │  └──────────────────────────────────────────────────────────────────────┘  │
y=84   │  [☰ filters 48] [Modern] [Cozy] [Aesthetic] [Simple] →  colour chips 48     │
y=138  └──────────────────────────────────────────────────────────────────────────┘
y=146.4┌──────────────── 2-column grid (same card tokens as home) ─────────────────┐
```

* Header: full width, **height 150.39**, white, `position: sticky; top: 0; z-index: 2`,
  padding `12px 8px`. It contains two rows.
* Search field: **396 × 48.39** at `(8, 24)`; **radius 16**; background `#FFFFFF`;
  **1 px border `#91918C`**; text **16 / 400**, line-height 22.4; padding
  `12px 12px 12px 40px`; left magnifier icon **16 × 16** at x = 25 (17 px from the field's
  left edge), vertically centred; placeholder `Search` (when a query is present the field shows
  the query and there is no placeholder).
* Guided-search row at `y = 84.39`, height 48 (54–56 px including press padding):
  * Filter button `one-bar-pill`: **48 × 48**, **radius 16**, background **`#E5E5E0`**, icon
    (sliders) 16 × 16.
  * Colour chips: height **48**, **radius 16**, horizontal padding: a **44 × 48 image
    thumbnail** on the left + **16 px / 500** label. Observed backgrounds:
    `#FFE0CC`, `#C7F0DA`, `#83DDAD`, `#DBE1FF`, `#92A4FF`, `#FBDFFF`, `#FFD7D7`, `#FFE0CC`,
    `#C7F0DA` (cycled pastels; each chip has its own thumbnail photo and label).
  * Visual gap between chips ≈ 8 px; the row scrolls horizontally and is not clipped by the
    page (elements were measured out to x ≈ 1131).
* Results grid: starts at `y = 146.39` (the sticky header overlaps its first 4 px), same
  2-column / 201 / 1 px / radius 16 tokens as the home masonry, with the same sponsored block.
* **No back/cancel affordance** in the header. Device/browser back is the only navigation away
  from results; the bottom nav remains visible (Search item active).
* Keyboard: an IME cannot be emulated in headless Chromium, and focusing the input changed no
  layout or styling (no focus ring added beyond the persistent 1 px border). Assume the OS
  keyboard slides over the bottom nav; handle with `visualViewport`/IME insets natively.

---

## 4. Pin close-up

```
y=0    ┌ image 388 × aspect ───────────────── x 4 … 392, radius 16 ────────────────┐
y=8    │ (◀ back 48 r16 white)                                    (lens 48 circle) │
y=392  └──────────────────────────────────────────────────────────────────────────┘
y=408  ♥ 4   💬   ↗   …                                                    [ Save ]  ← 68.53×48 r16
y=465  ◎ avatar 16   Yuki_idk 12/700
y=490  Mafuyu sticker                                         H1 16/700, lh 22.4
y=516  sticker creator found in @raccoonkingpoe See more     16/400, mention 16/700 #2B48D4
y=551  [ Visit site ]                                        396×48 r16 #E5E5E0, 16/500
y=586  #mafuyu #asahina #yuki #k #kanade #mafufu              14/400 #2B48D4
y=627  More to explore                                        H2 16/700
y=658  ┌ related card 201 × 174.83 ┐ 1px ┌ related card ─────────────────────────┐
y=846.5└──────────────────── bottom nav (same 4 items, visible) ───────────────────┘
```

### 4.1 Back button

* Floating over the image at **`(8, 8)`**, hit box **48 × 48**, visual: white **rounded square,
  radius 16**, 24 × 24 chevron (`#000000`), no shadow. It stays fixed while the page scrolls.
* There is no title bar and no other page chrome.

### 4.2 Image

* Main image viewport box: **388 × 388** for a square pin, at **`(4, 4)`** (4 px side and top
  margin), **radius 16**, clipped. Height follows the real aspect ratio with
  `width = 388` (e.g. 388 × 542 for 3:4); it is not capped to a fraction of the screen.
* Image background (letterbox colour) is the pin's dominant colour (`#563980` observed), and the
  image fills the box (no object-fit letterboxing).
* Overlay button bottom-right of the image: **48 × 48 circle**, white, lens/search glyph
  24 × 24, positioned at `(348, 340)` — 8 px from the image's right edge, 4 px above its bottom
  edge. It opens visual search (not exercised).

### 4.3 Action row (`y = 408`, just 16 px under the image)

| element | box | glyph | notes |
|---|---|---|---|
| React (heart) | 32 × 32 hit at `(12, 412)` | 24 × 24 | black outline; count beside it |
| reaction count | `(48, 419)` | — | **12 px / 700**, `#000000`, gap 4 |
| Comments | 40 × 40 at `(71.58, 408)` | 24 | opens the comments sheet (§6) |
| Send / share | 40 × 40 at `(119.58, 408)` | 24 | opens the share sheet |
| More actions | 40 × 40 at `(167.58, 408)` | 24 | opens the options sheet |
| **Save** | **68.53 × 48 at `(335.47, 408)`** | — | **bg `#E60023`, radius 16**, colour `#FFFFFF`, label **16 / 500** lh 19.2, padding `6px 14px` + 2 px transparent border; right margin 8 |

Icon buttons have transparent backgrounds, no radius, and 24 px glyphs; gaps between hit boxes
are 8 px. The row is not sticky — it scrolls with the info column.

### 4.4 Pinner row

* At `y = 464–482`, left 16: circular **16 × 16 avatar** (radius 50 %), then the display name
  **12 px / 700**, lh 18, black, at `x = 36` (4 px gap). There is no Follow button for your own
  pin; for other pinners a "Follow" chip appears (not measured here).

### 4.5 Title / description / hashtags

* Title: `H1` **16 px / 700**, line-height 22.4, black, at `(16, 490)`, width 380.
* Description: **16 px / 400**, line-height 22.4, black, at `(16, 516.39)`; @mentions are
  **16 px / 700, `#2B48D4`**; the description is line-clamped (2 lines in this sample) and followed by **"See more" 14 px / 700, black**
  (lh 19.6) at the end/below.
* Hashtag row: links **14 px / 400**, line-height 19.6, colour **`#2B48D4`** (no underline),
  wrapping with 8 px gaps; row starts at `y = 586.56` when present.
* Info column side padding: **16 px** (content x = 16, width 380).

### 4.6 CTA and related content

* "Visit site": full-width minus 8 px margins — **396 × 48** at `(8, 551.39)`, **radius 16**,
  background **`#E5E5E0`**, label **16 px / 500** black, centred. (Pins without a link omit it;
  "Read it" and "Visit site" variants exist depending on target type.)
* There is also a **hidden sticky duplicate** of this CTA: a second `fixed` bar 412 × 72 at
  `y = 907` with `z-index: 4` and inline `transform: translateY(64px)` (i.e. parked 64 px below
  the viewport). In the real product it slides up over the bottom nav during scroll; programmatic
  `scrollTo` did not trigger the slide, so it never became visible in our captures.
* "More to explore": `H2` **16 px / 700**, line-height 22.4, at `(16, 627.39)`.
* Related grid: identical card metrics to the feed — 201 px cards, 4 px inner pad, image 193 wide,
  **radius 16**, 1 px gap, "…" button 24 (e.g. `05-pin-closeup.png`, `10-…png`).
* The close-up keeps the **bottom nav** visible and adds the nav's usual 68.5 px at the bottom.

### 4.7 Back to top

* After scrolling, a floating pill appears above the nav: **116.23 × 48** at
  `(287.77, 791)` — i.e. 8 px from the right edge and 7.5 px above the nav — white,
  **radius 16**, with an up-arrow glyph and label "Back to top"; text **14 px / 700**
  (est. shadow, flat white pill in the capture). Tapping scrolls to the top.

---

## 5. Save flow

**What tapping Save actually does on mobile web:** it is a **full-page navigation**, not a bottom
sheet. Both entry points navigate to `GET /pin/<id>/repin/`:

1. The red **Save** pill in the close-up action row.
2. The **Save** row inside the card "…" options sheet (§5.3).

The `/repin/` route is server-rendered (HTTP 200, ~386 KB HTML) but its board picker is
client-side: the page immediately issues a GraphQL **POST** for the board list. Under the
mandated read-only guard that POST is aborted, the app throws `GraphQLFetchError: Failed to
fetch`, and the route renders **blank** (verified twice, 1082 × 2402 pure white screenshot;
16 POSTs aborted). Consequently **the board-picker pixel tokens could not be measured** — they
are the only required surface this brief cannot deliver, and no write was attempted to obtain
them.

What we *did* measure that constrains the picker design:

* Mobile web always transitions **full-page** for the save picker at this viewport (no in-page
  sheet), and the picker is titled/structured by the `repin` route.
* The closest measured full-page flow is the **Organize** flow
  (`/yourhandle/organize/`, GET, renders): header **64 px** tall, title `Organize into board`
  **16 / 700** at `(120, 24.8)`, a disabled **Next** action **16 / 500** in `#91918C` at
  `(356.61, 26.41)`, and a **Select all** row **16 / 700** at `(12, 76)`.
* All Pinterest mobile sheets/buttons measured elsewhere use the same primitives: **radius 32**
  sheet tops, **48 px** touch targets, **radius 16** filled buttons in `#E5E5E0`, text
  **16 / 500**, red confirm `#E60023`, secondary text `#62625B` **12–14 px**.

### 5.1 Recommended save-picker spec for PinFall (marked **est.**, built from the above tokens)

* Bottom sheet (in-app; the native app need not reproduce the web's full-page navigation), white,
  **radius 32 32 0 0**, scrim `rgba(0,0,0,.50)`, grabber **37 × 5** `#E5E5E0` r28 centred 16 px
  below the top edge, close **48 × 48** at `(16, 12)` from the sheet top.
* Title `Save to board` **16 / 700** at left 72 (after the close button) — same geometry as the
  comments sheet title (`What do you think?` at `x = 80`).
* Board rows: **56 px** tall (mobile web action rows measure 38.39 px tall with 16/700 labels;
  56 px is the app's safer touch size), 40 × 40 preview thumb radius 8 on the left, board name
  **16 / 500**, pin count **12 / 400 `#62625B`**, chevron/right optional; row radius 12 on press.
* "Create board" row pinned as the first row: `+` icon 24 in a 40 × 40 `#E5E5E0` r8 tile, label
  **16 / 500**.
* Confirm: a sticky bottom bar **48 px** tall, full-width minus 16 px margins, **radius 16**,
  bg `#E60023`, label **16 / 500** white, disabled state bg `#E5E5E0` + text `#91918C` until a
  board is selected. Do **not** navigate away during save.
* Success feedback (not captured — saving is forbidden for this research; marked est.):
  toast pill **44 px** tall, **radius 16**, bg `#111111`, text **14 / 500** white,
  "Saved to <board>" + **Undo** (14 / 700 white), 3–4 s, bottom-offset above the nav.
  The web shows no modal confirmation before saving when a board is preselected.

### 5.2 Board-row semantics observed in desktop docs (for parity only)

`docs/UI.md`/`UI_TOKENS.json` already define desktop board-picker tokens; the mobile sheet must
reuse the row metrics above and the same board fields (name, pin count, cover, private flag).

### 5.3 Card "…" options / share sheet (measured, safe)

Tapping the feed card's "…" opens a white bottom sheet over a `rgba(0,0,0,.5)` scrim:

* Sheet top at **y = 345.08**, full width, height 569.92, **radius 32 32 0 0**, white.
* Share row: title `Share` (16/700) + 5 round targets (WhatsApp, Messenger, Message, Facebook, X)
  with 48 px circles and small labels (est. 11/700); 1 px `#EFEFEF` divider below.
* `Options` header **16 / 700** at `y = 557.08`.
* Rows (Save, Open in app, Download image, See more, See less, Report): **386 × 38.39** at
  `x = 13`, label **16 / 700** at `x = 43` (24 px icon at `x ≈ 17`); no dividers, no chevrons.
  `Report` carries the sub-label `This goes against the community guidelines` **14 / 400**
  (lh 19.6).
* Footer note `This Pin was inspired by your recent activity` **14 / 400** at `y = 866.41`.
* The `Save` row navigates to `/pin/<id>/repin/` (§5), which is why the picker itself was not
  observable in-page.

---

## 6. Comments

The close-up's comment action opens a **bottom sheet** (web: `role = dialog`), not a new page.

```
y=119.75 ┌ sheet 412 × 795.25, white, radius 32 32 0 0 ─────────────────────────────┐
y=135.75 │                     grabber 37 × 5 r28 #E5E5E0                          │
y=148.75 │ (X 48 × 48)                                                            │
y=161.55 │ What do you think?                          H1 16/700 (x = 80)            │
y=220.75 ├── modal content 412 × 686.25 ──────────────────────────────────────────────┤
         │                                                                          │
         │   (empty state, 16/400, 380 × 44.78 at (16, 513.48))                    │
         │   Write a comment, share feedback, or give a high-five to get the        │
         │   conversation going                                                     │
         │                                                                          │
y=847    │  ┌ composer 388 × 48, r32 ───────────────────────────────────────────┐    │
         │  │ Add a comment (16/400, x=30)                                (send) │    │
y=895    │  └──────────────────────────────────────────────────────────────────┘    │
y=915    └──────────────────────────────────────────────────────────────────────────┘
```

* Sheet: **412 × 795.25** at `y = 119.75` = **≈ 87 % of the 915 px viewport**, white,
  **`border-radius: 32px 32px 0 0`**, over a full-screen scrim (**50 % black**).
* Grabber: **37 × 5** at `(187.5, sheetTop + 16)`, radius **28** (pill), `#E5E5E0`
  (`aria-label="Grabber"`).
* Close: **48 × 48** at `(16, sheetTop + 29)` — 24 × 24 X; `aria-label="Dismiss bottom sheet"`.
* Title: `What do you think?` **16 / 700**, lh 22.4, at `(80, sheetTop + 41.8)` — i.e. left of the
  title aligns with the close button's right edge + 16.
* Content region starts at `y = 220.75` (100 px below the sheet top).
* Empty state: 380 × 44.78 at `(16, 513.48)`, **16 px / 400**, black, two lines:
  *"Write a comment, share feedback, or give a high-five to get the conversation going."*
* Composer (always visible, pinned to the sheet bottom, not sticky relative to the page):
  **388 × 48** at `(12, 847)` — 12 px side margins, **radius 32** (full pill), white,
  editor text **16 / 400** at `x = 30` (18 px cell inset), placeholder `Add a comment`,
  send icon **24 × 24** at the right end of the pill.
* Comment rows (avatar/name/body/like layout) could **not** be measured: all three pins probed
  showed the empty state (comments hydrate via a POST that the read-only guard aborts, and the
  sampled pins may genuinely have none). For parity, use the tokens from the rest of the app:
  avatar **32 × 32** circle, name **12 / 700**, body **14 / 400** lh 19.6, like count
  **12 / 400 `#62625B`**, 8 px vertical gaps — marked **est.**
* Dismissal: drag down on the grabber or tap the X; tapping the scrim also dismisses
  (`Dismiss bottom sheet` semantics).

---

## 7. Saved / Profile tab

Reached from the bottom nav's 4th item (`/yourhandle/`); the URL is the plain profile URL and
the **Saved** tab is selected by default for your own profile.

```
y=0    ┌────────────── own-profile header 412 × 72.39 (fixed) ────────────────────┐
y=12   │ [🔍 Search your Pins 304 × 48.39 r16]        [+] 48        [⚙] 48        │
y=72   ├──────────────────── profile header 364 × 300.38 ───────────────────────────┤
y=72   │                          (avatar 120 × 120 circle)                       │
y=200  │                     Your Name Here        36/700                         │
y=257  │                     yourhandle  16/400 #62625B                           │
y=286  │                    0 followers · 0 following  16/700 · 14/400             │
y=325  │                    [ Share 75.47×48 ]  [ Edit Profile 111.56×48 ]         │
y=401  ├──────── tabs container (24, 400.77) 364 × 56 ─────────────────────────────┤
y=405  │                     Created 84.31×48     Saved 70.56×48 (active)          │
y=457  ├──────── board grid (x=8, 396 wide, 2 columns) ────────────────────────────┤
       │ [All Pins 192×199.67]  [meme 192×204]  …                                  │
y=1098 │                             [ Organize 82.91×36 ]                        │
y=1146 ├──────── MobileFeed (regular 2-column masonry, same as home) ──────────────┤
```

### 7.1 Header and identity

* Header: fixed, **412 × 72.39**, white, contains:
  * Search field **304 × 48.39** at `(8, 12)`, **radius 16**, placeholder `Search your Pins`
    (the fill is a light neutral; the measured wrapper is transparent and the fill lives one
    level deeper — assume `#E5E5E0`/`#F1F1F1` with a 16 radius, marked **est.**), magnifier 16.
  * Create "+" button **48 × 48** at `(312, 12.19)` (opens create menu — not exercised).
  * Profile settings gear **48 × 48** at `(360, 12.19)`; tap target 48 circle.
* Avatar: **120 × 120**, circle, centred at x = 146, top y = 72.39 (i.e. flush under the header).
* Display name: **36 px / 700**, line-height 39.6, centred, at `y = 200.39` (content 352.89 wide
  at x = 29.55). Truncates with ellipsis on overflow.
* Username: **16 px / 400**, `#62625B`, lh 22.4, centred, at `y = 256.98`.
* Stats: `0 followers` / `0 following` **16 px / 700** black, separator `·` **14 / 400**;
  row at `y = 286.38`.
* Buttons at `y = 324.77`, **48 px** tall, **radius 16**, bg **`#E5E5E0`**, label **16 px / 500**:
  * `Share` **75.47 × 48** at x = 108.48
  * `Edit Profile` **111.56 × 48** at x = 191.95
  (Share sits left of Edit; both centred as a pair.)

### 7.2 Tabs

* Container **364 × 56** at `(24, 400.77)` (8 px inner top pad).
* `Created` **84.31 × 48** at `(128.56, 404.77)`, `Saved` **70.56 × 48** at `(212.88, 404.77)` —
  boxes adjacent (0 px between hit boxes; the visible label gap comes from their padding), **radius 16**, label **16 / 500** black. The pair is centred in the 364-wide container.
* Active tab: **2 px underline `#33332E`** at the tab bottom, width = label width (same pattern as
  the home chips; the underline is visible under "Saved" in `09-profile-saved.png`).
  Inactive: no underline, same black label (no grey-out).

### 7.3 Board grid

* 2 columns: **192** wide at `x = 8` and `x = 210` (10 px horizontal gap, 2 px vertical gap).
* Board card: **192 × 204**, **radius 8**, white. Content:
  * Stacked cover group starting 12 px below the card top: 3 staggered rounded thumbnails,
    the first **127 × 126** at `(x + 1, y + 13)` with **radius 8**; the group occupies 128 px.
  * `All Pins` card is **192 × 199.67** and uses the stacked-pins artwork instead of photos.
  * Board name: **16 px / 700**, lh 22.4, at `x = card.x + 8`, `y = coverGroupBottom + 8`
    (e.g. y = 604.77 for the second row); single line, ellipsis.
  * Meta: `<n> Pins · <age>` **12 px / 400**, lh 18, colour `#62625B` (e.g. "357 Pins · 44m").
* Each card has an "Edit" affordance (**28 × 28** hit) at the top-right of the card
  (x = card right − 36, y = card top + 104) on your own boards.

### 7.4 Organize section

* Section title `Unorganized ideas` **20 px / 700**, lh 24, at `(8, 1103.77)`.
* `Organize` button **82.91 × 36** at `(321.09, 1097.77)` (radius 16, bg `#E5E5E0`,
  label **14 / 500** — est. from the 36 px height); it opens the full-page Organize flow
  (`/yourhandle/organize/`, §5).
* Below it, the profile's own pin feed renders with the standard masonry tokens (§2.2).

---

## 8. Colour, radius, typography and elevation tokens

### 8.1 Colours (exact computed values)

| token | hex / rgb | used for |
|---|---|---|
| background | `#FFFFFF` / `rgb(255,255,255)` | page, cards, sheets, nav, search field |
| text | `#000000` | titles, labels, body, icons (mobile uses pure black, *not* the desktop `#211922`) |
| text secondary | `#62625B` / `rgb(98,98,91)` | Sponsored, inactive nav, username, meta, composer placeholder-adjacent text |
| brand red | `#E60023` / `rgb(230,0,35)` | Save pill, Inbox dot |
| link / mention | `#2B48D4` / `rgb(43,72,212)` | @mentions, hashtags |
| input border | `#91918C` / `rgb(145,145,140)` | search field 1 px border; disabled "Next" label |
| neutral fill | `#E5E5E0` / `rgb(229,229,224)` | filter pill, Visit site, Share/Edit Profile, sheet grabber |
| active underline | `#33332E` / `rgb(51,51,46)` | chip + tab underline 2 px |
| surface hover | `#F6F6F3` | (press/hover wash — from desktop token set, not re-measured) |
| divider | `#EFEFEF` | sheet dividers (share/options) |
| scrim | `rgba(0,0,0,0.50)` | sheet backdrop |
| pastel chips | `#FFE0CC #C7F0DA #83DDAD #DBE1FF #92A4FF #FBDFFF #FFD7D7` | guided search chips |
| image background | pin dominant colour (e.g. `#563980`, `rgb(200,142,103)`) | close-up/news image boxes |

**No shadows** were measured on cards, buttons, nav, chips or the close-up image
(`box-shadow: none`). Sheet elevation is communicated by the scrim + 32 px radius, not a shadow.

### 8.2 Radii

| radius | applied to |
|---|---|
| **8 px** | board cards (profile), board cover images, video duration pill, small tiles |
| **12 px** | bottom-nav item press highlight |
| **16 px** | feed/related images and cards, close-up image, back button, all buttons (Save, Visit site, Share, Edit, filters), search input, tabs, chips, header press targets |
| **28 px** | sheet grabber (pill) |
| **32 px** | bottom-sheet top corners, comment composer pill |
| **50 %** | avatars (16, 120), lens image-search button |
| **999 px** | Inbox unread dot |

### 8.3 Typography

Font stack: `"Pin Sans", -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, …`, i.e. Pin
Sans with a Roboto/Helvetica fallback. Sizes and line-heights below are computed CSS values
(logical px at 412 width):

| role | size / weight | line-height | colour |
|---|---|---|---|
| profile display name | 36 / 700 | 39.6 | #000 |
| section title (`Unorganized ideas`) | 20 / 700 | 24 | #000 |
| pin title, section H2 (`More to explore`), sheet title, options header | 16 / 700 | 22.4 | #000 |
| close-up description / body | 16 / 400 | 22.4 | #000 |
| buttons (Save, Visit site, Share, Edit, tabs, chips) | 16 / 500 | 19.2 | #000 / #FFF |
| search input | 16 / 400 | 22.4 | #000 |
| sheet action rows (Save/Open in app/…) | 16 / 700 | 22.4 | #000 |
| hashtags | 14 / 400 | 19.6 | #2B48D4 |
| "See more", "Back to top" | 14 / 700 | 19.6 | #000 |
| sheet footer note, comment meta | 14 / 400 | 19.6 | #000 / #62625B |
| guided-chip label | 16 / 500 | 19.2 | #000 |
| Sponsored / advertiser line | 12 / 400 | 14 | #62625B / #000 |
| reaction count, pinner name | 12 / 700 | 18 | #000 |
| board meta (`357 Pins · 44m`) | 12 / 400 | 18 | #62625B |
| bottom-nav labels | 11 / 700 | 16.5 | #000 / #62625B |

### 8.4 Touch-target sizes observed

* 24 px glyph is the mobile **icon** size everywhere (nav, action row, compose send, sheet rows).
* 40 px hit boxes for the close-up comment/send/more actions; 32 px for the react button;
  48 px for Save, back, close, sheet rows, search field height, tabs, card "…" (24 px hit).
* Nav items are 79.19 × 44.5 hit boxes (plus 8/16 px padding).
* Minimum spacing between adjacent action targets: 8 px.

---

## 9. Behaviour notes (what is sticky, what scrolls, gestures)

* **Home:** chips bar is `sticky; top: 0` and remains while the feed scrolls; bottom nav fixed;
  feed scrolls the document (no inner scroll container). Tapping a card = open close-up; "…"
  opens the options sheet (§5.3) — no long-press menu was observed on cards.
* **Search:** header is `sticky; top: 0; z-index: 2`; it covers the first 4 px of the grid at
  rest. The guided chip row scrolls horizontally; the page scrolls vertically. Tapping the field
  focuses it (OS keyboard); no in-page cancel button.
* **Close-up:** floating back button is fixed at `(8, 8)` and stays while scrolling; the image,
  action row, pinner, title/desc, CTA and related grid all scroll; bottom nav stays fixed; a
  "Back to top" pill fades in after scrolling; a hidden sticky Visit-site bar is parked 64 px
  below the viewport and slides up on scroll in the real product.
* **Sheets:** drag-handle + close button + scrim tap are the three dismissal affordances; sheets
  open over a 50 % scrim and own the bottom ≈ 87 % of the screen; content behind is inert.
* **Tap vs long-press:** all interactions measured are single taps; the web build exposes no
  long-press menu (long-press may invoke the browser's link context menu, which is not app UI).
* **Transitions:** the only measurable motion is the sheet slide-up and the nav/CTA slide
  (`translateY(64px) → 0`); durations are not observable from static CDP measurements. Use
  ~200–250 ms ease-out for the native equivalents.
* **No infinite-scroll spinner** was observed in the SSR slice; pagination is a POST resource
  call (blocked here), so treat loading rows as skeleton cards at the bottom of the feed.

---

## 10. Read-only method & limitations (reproducibility)

* Dedicated headless Chrome profile (`/tmp/pf-mob-profile`), `--headless=new --no-sandbox
  --disable-gpu --remote-debugging-port=9224`, cookies loaded from
  `.session-cookies.txt` via `Network.setCookies` (signed-in session; `_auth` present).
* Device emulation: `Emulation.setDeviceMetricsOverride(412, 915, 2.625, mobile=true,
  screenOrientation=portraitPrimary)` + `Emulation.setTouchEmulationEnabled(5)` +
  Pixel 7 / Chrome 154 UA override (`navigator.userAgentData.mobile == true`).
* **Safety:** `Fetch.enable` with a catch-all pattern; the handler continues `GET`, `HEAD`,
  `OPTIONS` and **fails every other method with `Aborted`**. Counts on the measured pages:
  136 GETs allowed / 14 POSTs aborted in the initial home load; 16–23 non-GET requests aborted on later loads
  (analytics `storage_report`, `trace/trace`, `graphql`, `resource/*`, reCAPTCHA). No request
  with a body ever reached Pinterest from the page.
* No like, save, comment, board creation or follow was executed. The Save sheet and comments
  sheet were **opened** (read-only UI) but no board or post was ever chosen/submitted; the
  `/repin/` blank page confirms the board-list POST never left the browser.
* Data collected via `getBoundingClientRect()`, `getComputedStyle()`, hit-testing
  (`elementFromPoint`), and pixel sampling of the screenshots (the red Save bbox in the PNG
  matched the DOM rect to sub-pixel: `335.24, 408.38, 403.05, 456` CSS).
* Screenshots were taken at DPR 2.625 (1082 × 2402), downscaled to 618 × 1372 and quantised to
  PNG8 (≤ 192 colours) for the repository (10 files, 1.6 MB total).
* Limitations caused by the safety guard (documented, not worked around):
  * Infinite scroll / pagination does not load → finite SSR feed slices.
  * Save/board picker cannot hydrate (blank `/repin/`) → **board-picker tokens not measured**.
  * Comments list did not load on any probed pin → comment-row tokens marked **est.**
  * No IME/keyboard; no hover; no long-press context menu; no video playback observation
    beyond the DOM (the `<video>` element and duration pill were measured, not frames).
  * nl locale strings appear in a few places (e.g. `Dingen om te kopen`); UI structure is
    locale-independent.

---

## 11. Mapping to PinFall Android (432 logical px)

Our Android renderer uses a **432 logical-px** design width (`pf_android_ui_scale_for_width`:
`ui = win_w / 432`, `SDL_RenderSetScale`; logical px ≈ dp). Pinterest's mobile web is measured
at **412**. Recommendation: **keep Pinterest's fixed dimensions (heights, icon sizes, radii,
font sizes, paddings) as logical px** — they are already tuned for a phone — and **stretch only
widths** by 432/412 = **1.0485**. Do not scale font sizes up by 4.8 %; the extra 20 px should go
into card width and nav spacing.

### 11.1 Token mapping (Pinterest mobile → PinFall 432)

| token | Pinterest @412 | PinFall @432 | action |
|---|---|---|---|
| page side inset | 4 | **4** | set `page_pad = 4` (was 20 desktop) |
| feed gutter | 1 | **1** | `card_gap = 1` (was 16) |
| card width | 201 | **211.5** → render 211/212 | compute `(W − 2·4 − 1)/2` |
| image inset in card | 4 | **4** | image width = card − 8 = 203.5 |
| image / card radius | 16 | **16** | already `card_radius = 16` ✔ |
| card title block | none (image-only) | **0** for feed | caption block 0; ads add 34 |
| "…" pin menu | 24 hit / 16 glyph, right 6, bottom 10 | **24 / 16** | new component |
| home header | none (chips bar 60) | **chips bar 60 + status inset** | replace 80 px header on Home |
| chips bar | 60 h, chip 48 r16, 16/500 (12 pad, adjacent boxes), underline 2 #33332E | **60 h, 48 r16, 16/500, 12 pad, 2 px underline** | new `home_chips` bar |
| search/profile header | 72.39 h (search 48.39) | **72 h** | new header variant |
| search results header | 150.39 h (field + guided chips) | **150 h** | new search variant |
| search field | 48.39 h, r16, 1 px #91918C | **48 h, r16, #91918C** | `search_radius 16` (was 24), `search_h 48` ✔ |
| bottom nav | 68.5 h, items 79.19 × 44.5, gaps 19.81, side 17.91 | **69 h**, items **83 × 44.5**, gap **21**, side **18** | replaces 28 px status footer |
| nav icons / labels | 24 / 11·700 | **24 / 11·700** | new |
| nav badge | 8 × 8 #E60023 r999 | **8 × 8** | new |
| close-up back | 48 × 48 r16 white @ (8,8) | **48 × 48 r16 @ (8, 8+statusInset)** | replaces 44 px back below 56 px header |
| close-up image | 388 w (4 px margins), r16, natural height | **424 w**, r16, natural height | no 42 % height cap |
| close-up action hit | 40 (32 react), glyph 24 | **40 / 24** | already `CU_ACTION_H 48` → reduce to 40 |
| close-up title | 16/700 lh22.4 | **16/700 lh22.4** | `h1_size 28` → 16 |
| close-up desc / hashtags | 16/400 lh22.4 / 14/400 #2B48D4 | same | new link colour token |
| Save pill | 68.53 × 48 r16 #E60023, right 8 | **68.5 × 48 r16**, right 8 | already `CU_SAVE_W 68.5` ✔ |
| Visit site | 396 × 48 r16 #E5E5E0 | **416 × 48 r16** | keep 16/500 label |
| profile avatar | 120 circle | **120** | new profile tab |
| profile name | 36/700 lh39.6 | **36/700** | new |
| profile board card | 192 × 204 r8, cover 127 r8, gap 10/2 | **203 wide**, r8, cover 134, gap 10/2 | new |
| tabs | 48 h, underline 2 #33332E, 16/500 | **48 h** | new |
| sheet radius / scrim | 32 top / 50 % black | **32 / 50 %** | new sheet component |
| grabber / close | 37 × 5 r28 #E5E5E0 / 48 | **37 × 5 / 48** | new |
| sheet rows | 38.39 h, 16/700, 24 icon | **38.4 h** (or 56 for save board rows) | new |
| composer | 388 × 48 r32, 16/400, send 24 | **408 × 48 r32** | new |
| toast | not captured (est.) | **44 h r16 #111, 14/500 white** | new |
| text colour | #000000 | **#000000** | was #211922 |
| border strong | #91918C | **#91918C** | was #CDCDCD |
| active underline | #33332E | **#33332E** | new |
| link/mention | #2B48D4 | **#2B48D4** | new |
| background / secondary / brand / fill | #FFFFFF / #62625B / #E60023 / #E5E5E0 | unchanged | ✔ already in theme |

### 11.2 Safe areas in the native app (important — the web shows 0)

The mobile web relies on the browser chrome for status/gesture areas; its CSS insets are `0`.
Our SDL app is edge-to-edge (Android `pf_android_*` already tracks a top pad), so add:

* **Top:** status-bar inset (typically **24 dp**; use the real `WindowInsets`) above the chips
  bar / headers. Close-up's floating back button moves to `8 + topInset`.
* **Bottom:** gesture/navigation-bar inset (typically **24 dp**; use the real inset) below the
  bottom nav. Nav container 69 + inset = e.g. 93 dp, white, so content never hides under the
  gesture bar.
* Keep the nav *icons* at 24 dp and 11 sp labels inside that taller container.

### 11.3 Concrete component specs @432 (drop-in)

* **Bottom nav** (`footer_h 69`): white; 8 dp top pad; 4 items, item w = `(432 − 2·18 − 3·21)/4
  = 83.25` → 83; item h 44.5; icon 24; label 11/700 lh 16.5; active #000, inactive #62625B;
  press pill radius 12; Inbox dot 8 at icon top-right; hit the full item box.
* **Home chips scroller** (height 60, sticky): chips 48 tall, radius 16, text 16/500, 12 dp padding (adjacent hit boxes),
  active underline 2 dp #33332E centred under the label, horizontal scroll with edge fade.
  No logo/avatar/search on Home.
* **Feed card:** w = 211.5, image w = 203.5, image h = `203.5 × naturalH/naturalW`, radius 16,
  image inset 4, "…" 24 at right 6 / bottom 10; no caption; sponsored block: advertiser 12/400
  #000 at y = imageBottom+8, `Sponsored` 12/400 #62625B at +18, block 34, card bottom pad 16.
* **Close-up:** back 48 r16 white at (8, 8+inset); image 424 wide r16 with 4 dp top margin;
  action row 16 dp below image, hit 40 (react 32) with 24 glyphs; count 12/700 4 dp right of
  the heart; Save 68.5 × 48 r16 #E60023 at right 8; pinner avatar 16 + name 12/700; title
  16/700 lh 22.4; description 16/400 lh 22.4 (line clamp + "See more" 14/700); hashtags
  14/400 #2B48D4; Visit site 416 × 48 r16 #E5E5E0; "More to explore" 16/700; related grid =
  feed grid; Back-to-top pill 116 × 48 r16 at right 8, 7.5 dp above the nav.
* **Comments sheet:** height ≈ 87 % of the screen; radius 32 top; grabber 37 × 5 r28
  #E5E5E0; close 48 at (16, 12); title 16/700 at x 80; empty state 16/400; composer pill
  408 × 48 r32 at bottom 12, placeholder `Add a comment` 16/400 at x 30, send 24 right;
  scrim 50 %.
* **Save sheet:** as §5.1 (radius 32, grabber, close, board rows 56, create-board row,
  sticky red confirm 48 r16, toast + undo).
* **Profile/Saved:** header 72 (search 304 + create 48 + settings 48), avatar 120, name 36/700,
  username 16/400 #62625B, stats 16/700, Share/Edit 48 r16 #E5E5E0, tabs 48 underline 2,
  board grid 2 × 203 r8 with cover ~134, names 16/700, meta 12/400 #62625B, Organize 20/700
  title + 36 dp pill.

SDL note: all of the above are plain SDL_FRect math in logical px, so they compose with the
existing `SDL_RenderSetScale` baseline — see the SDL Android README for the platform glue:
<https://wiki.libsdl.org/SDL2/README-android>.

---

## 12. Parity gaps vs the current PinFall Android UI (top 8)

Ground-truthed against `src/theme.h` (desktop tokens: `header_h 80`, `footer_h 28`, `page_pad 20`,
`card_gap 16`, `search_radius 24`, `h1 28`, text `#211922`, border `#CDCDCD`) and `src/ui.c`
(portrait close-up: 56 dp header, 44 dp back below header, media capped at `0.42·H`, 48 dp action
targets; feed cards render a 2-line caption block; footer is a 28 dp status line).

1. **No bottom navigation.** PinFall renders a 28 px status footer; Pinterest mobile has a
   68.5 px fixed 4-tab nav (Home/Search/Inbox/Saved) with 24 px icons, 11/700 labels, active
   #000, Inbox red dot. This is the single biggest structural difference — the app currently has
   no persistent, thumb-reachable navigation and no notifications/profile surfaces.
2. **Feed density and card anatomy.** Desktop feed uses 20 px page inset, 16 px gutters and a
   two-line caption (`title_block_h`); Pinterest mobile uses 4 px inset, **1 px** gutter, 4 px
   inner card padding, and **image-only** cards (no caption anywhere in the feed). Sponsored
   cards only add the 12 px advertiser/Sponsored block.
3. **Home top bar.** PinFall draws an 80 px logo/search/avatar header on Home; mobile Pinterest
   has **no top bar** on the feed — only a 60 px sticky category-chip strip (no logo/search/
   avatar). Search lives in the bottom nav and in the 72 px Search/Profile headers.
4. **Close-up chrome and image.** PinFall portrait close-up keeps a 56 dp header (search+avatar
   +chevron), puts the back button below it (44 dp) and caps the image at 42 % of the screen
   height inside a 1 px bordered card. Mobile Pinterest has **no header**, a floating 48 × 48
   white rounded-square back at (8,8) **over** the image, a 388 (→424) px-wide radius-16 image
   with natural aspect height, and no card border.
5. **Close-up typography and action row.** Current detail uses `h1 = 28 px`, 48 dp action
   targets and a desktop-style action cluster; mobile uses **16/700 title / 16/400 body /
   14/400 blue hashtags**, 40 dp action hits with 24 px glyphs, a 12/700 reaction count, and the
   red Save pill 68.5 × 48 r16 flush at the right edge of the action row.
6. **No bottom-sheet component.** Mobile Pinterest's comments, card options/share and save
   pickers are bottom sheets: white, **radius 32 top**, **50 % scrim**, grabber **37 × 5**
   `#E5E5E0`, 48 px close, 38.4 px rows with 24 px icons, 48 px pill composer. PinFall has
   modal/panel primitives only (desktop `panel_radius 16`).
7. **Save flow.** Current app exposes a desktop-style picker modal; Pinterest mobile navigates to
   a full-page `/pin/<id>/repin/` picker (the web's only entry point), i.e. it needs a
   dedicated board-picker screen/sheet with board rows, a "Create board" affordance, a sticky
   red confirm (disabled until selection), and a saved-toast with Undo. Nothing of that flow
   exists natively yet.
8. **Touch platform chrome and details.** Missing on Android today: safe-area insets (status
   24 / gesture 24) around the nav and headers, press-highlight pills (radius 12) on nav items,
   video duration pill (40 × 24 r8 white, 12 px/500 text, top-left 8), Inbox unread badge, floating
   "Back to top" pill, and the mobile colour corrections (`#000` text → not `#211922`,
   `#91918C` input border, `#33332E` active underline, `#2B48D4` links, `search_radius 16`).

Secondary drift worth fixing while in there: profile/saved (120 avatar, 36/700 name, 48 dp
Share/Edit, 48 dp tabs with 2 px underline, 192→203 board cards radius 8 with stacked covers,
12/400 `n Pins · age`) has no native equivalent; and the theme's `max_cols`/`target_col_w`
formula is desktop-only (Pinterest mobile is always exactly 2 columns at this width).

---

## 13. Files

```
docs/UI_MOBILE.md                                  ← this spec
assets/ref/mobile/01-home-feed.png
assets/ref/mobile/02-home-feed-scrolled.png
assets/ref/mobile/03-search-results.png
assets/ref/mobile/04-search-video-duration-badge.png
assets/ref/mobile/05-pin-closeup.png
assets/ref/mobile/06-pin-closeup-info.png
assets/ref/mobile/07-comments-sheet.png
assets/ref/mobile/08-card-options-share-sheet.png
assets/ref/mobile/09-profile-saved.png
assets/ref/mobile/10-closeup-related-backtotop.png
```

Related: `docs/UI.md` (desktop feed), `docs/UI_CLOSEUP.md` (desktop close-up),
`docs/UI_TOKENS.json` (desktop token mirror). This document is measured at 412 CSS px; all
numbers are reproducible with the emulation and Fetch-guard recipe in §10.
