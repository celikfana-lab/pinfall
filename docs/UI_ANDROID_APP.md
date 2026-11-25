# Pinterest Android app (native) — measured UI reference

**App:** `com.pinterest` **14.42.0** (versionCode 14428030), Android 14
**Device:** Xiaomi 2409BRN2CY, **720 × 1640 px @ 320 dpi → 360 × 820 dp** (density scale **2.0**),
system **dark theme**, Dutch locale, **3-button navigation** (status inset 34 dp, nav inset 47 dp).
**Read-only capture:** only navigation taps (tabs, header icons, back, open/close sheets); no
save/like/comment/follow/create was submitted.

> **Correction to the earlier draft.** The first pass converted px → dp with 2.625 (that is the
> *web* DPR); this phone is 320 dpi, i.e. **px ÷ 2 = dp**. All bounds below are the corrected
> values from the native hierarchy dumps. The files that were labelled “close-up light” in the
> first pass were screenshots of **our own PinFall app**, not Pinterest, and have been removed.
> Also, the third bottom tab (“Opgeslagen”) is the profile/saved tab — there is no separate
> Profile tab.

## Where the data comes from

| source | use |
|---|---|
| Phone screenshots (720×1640) | all screens; geometry measured as px ÷ 2 |
| `assets/ref/native/01-home-dark.ui.xml` | exact native view bounds, Home |
| `assets/ref/native/02-closeup-dark.ui.xml` | exact native view bounds, close-up |
| `assets/ref/native/create-board-sheet-dark.ui.xml` | exact bounds, create-board sheet |
| APK decompiled resources (`resources/res/values/{colors,dimens,styles}.xml`, `res/font/*`) | light-theme colour tokens, type scale, component dimens, Pin Sans |
| Emulator (Android 15 x86_64, 1080×2400 @420 dpi) | login-wall / splash evidence only |

Guest browsing is **not available** in 14.42.0: the emulator lands on
`com.pinterest.identity.UnauthActivity` (email + Google login, no “Continue as guest” strings;
deep links and `NUXActivity` also hit the wall). Logged-in screens were therefore captured from
the user's phone with their authorisation. The emulator run also died three times under host
memory pressure (note for reproducibility); splash + login wall were captured before each stop.

## Screenshot & dump inventory (`assets/ref/native/`)

| file | screen |
|---|---|
| `home-dark-720x1640.png`, `01-home-dark.png`, `01-home-dark.ui.xml` | Home feed, dark mode |
| `closeup-dark-720x1640.png`, `02-closeup-dark.png`, `02-closeup-dark.ui.xml` | Pin close-up (two samples) |
| `comments-dark-720x1640.png` | Comments (full-screen page) |
| `save-board-picker-dark-720x1640.png` | Save → board picker bottom sheet |
| `create-board-sheet-dark.ui.xml` | Create-board sheet hierarchy (no separate PNG) |
| `search-dark-720x1640.png` | Search landing |
| ~~`profile-saved-dark-720x1640.png`~~ | Profile / Saved tab — *not published (personal account capture)* |
| ~~`inbox-messages-dark-720x1640.png`~~ | Inbox / Messages — *not published (personal notifications)* |
| `create-sheet-dark-720x1640.png` | Create sheet |
| `login-wall-unauth-1080x2400.png`, `login-wall-unauth.ui.xml` | Signed-out login wall (light) |
| `splash-android12-1080x2400.png` | Android 12 splash screen |

---

## 1. Home feed  (`home-dark-720x1640.png`)

Window **360 × 820 dp**. Measured hierarchy (dp = px/2):

| element | bounds (dp) |
|---|---|
| status bar | 0–**34** (`status_bar_background`) |
| header bar | `home_feed_greeting_header_view` x0 **y34 w360 h48** |
| logo + wordmark | `home_feed_greeting_header_logotype` **x16 y45 142×26** |
| create button | **x252 y34 48×48**, content-desc “Maken” |
| messages button | **x300 y34 48×48**, content-desc “Berichten” |
| messages badge | red dot **6×6** at (334, 42) |
| category chip row | container x0 **y82 h48**; horizontal scroller h44 |
| chip “Alles” | **x16 y82 40×34**, label y92 h22 (**16 sp**), active 2 dp underline |
| chip “vocaloid stickers” | x80 y82 **134×34** (24 dp gap) |
| chip “Dingen om te kopen” | x238 y82 **122×34** |
| feed container | x0 **y130 h643** (ends at nav bar 773) |
| pin cards | 2 columns, **174 dp** wide at **x4 / x182** |
| page inset / gutter | **4 dp** inset, **4 dp** gutter, **4 dp** vertical gap |
| first row | y132 |
| pin overflow “…” | `pin_overflow_action_id` **16×8** dots at x156 (right −22), near card bottom |
| captions | 12 sp grey text under some pins (1–2 lines), drawn inside the pin cell |
| video badge | white pill top-left, ≈**40×24 dp**, radius 8 (e.g. “0:19”) |
| bottom nav bar | `bottom_nav_bar` x0 **y697 w360 h76** |
| nav tiles | 3 × **60×60 dp** at **x82 / x150 / x218** (8 dp gaps), radius ≈20 dp, fill **#2C2C30** |
| nav icon / label | icon **20×20** in 24 box (y706); label **12 sp** (h16, y732) |
| nav labels | **Startpagina · Zoeken · Opgeslagen** |

Rendered colours: page **#18181B**, elevated surfaces/sheets **#222226**, nav tiles **#2C2C30**,
text **#FFFFFF**, brand **#E60023**. No full-width bottom bar: the nav is a floating group of
three rounded tiles centred over the feed. Create lives in the header; Inbox is the header
messages icon (red badge).

Feed details seen in the capture: variable-height masonry with 16 dp image corners; video pins
with a white duration pill; “…” dot menu bottom-right on cards; some cards show a 1–2 line
caption (e.g. “I forgot What I Was Gonna Say”, “Idek anymore bro.”); no sponsored marker was
observed on this feed slice.

---

## 2. Search  (`search-dark-720x1640.png`)

Root tab from the middle nav tile; full screen, no back button.

* Search field: full width minus **16 dp** margins (≈328 dp), **48 dp** tall, **pill radius 24**,
  dark elevated fill, magnifier 24 dp, placeholder “Ideeën zoeken”, camera icon right.
* Hero card: full-width image card with a large overlaid title (~24 sp/700, e.g. “Pumpkin spice
  recipes”) and carousel dots at the card bottom.
* Sections: “Ideeën voor jou” (16 sp/700 header + trailing search icon) with horizontal topic
  cards/tiles (“Angry birds”, “Monster energ…”).
* Floating 3-tile nav at bottom.
* No personal search history was shown (field empty); the field was intentionally not typed into
  (search history is account data).

`uiautomator` returned only the Compose shell for this screen, so these are screenshot
measurements, not node bounds.

---

## 3. Pin close-up  (`closeup-dark-720x1640.png`, `02-closeup-dark.png`)

Two hierarchy samples; the close-up is an edge-to-edge scroll with a floating top bar and **no
bottom nav** (system nav bar remains).

| element | bounds (dp) |
|---|---|
| content container | y34 h739 (`lego_closeup_container`) |
| back button | **48×48** at **(8, 38)** — white rounded square, radius 16 (visual), chevron 24 |
| floating top bar | x0 y34 w360 h52 (transparent) |
| main image | **x4 y34 352×320** (sample A; sample B 352×282), radius **8 dp** (resource) |
| visual-search button | **48×48** at **(304, 302)** (8 dp from right, 4 dp above image bottom), glyph 24 |
| action row | y354 h64 |
| react | **48×48** at x4; count “207” **12 sp** at x46 |
| comments | **48×64** at x78 (glyph 20 at x92) |
| share | **48×64** at x126 (glyph 20 at x140) |
| more (…) | **48×48** at x174 (container 62×48) |
| **Save** | **104×48** at x248 (right margin 8), radius 16, **#E60023**, label **16 sp**; the Dutch label “Bewaren” can wrap at this width |
| pinner row | avatar **16×16** at (16, 419); name **12 sp** (h18) at x38 |
| title/description | **16 sp** at (16, 446) h19; expand chevron 24 at (320, 444); translation link 14 sp at y472 |
| “More to explore” | **16 sp/700** at (16, 518) h22 (“Meer om te verkennen”) |
| related grid | 2 columns **174 dp** at x4/x182 from y547; 4 dp gutter; image radius 16; “…” 16×8 right −6 |
| video/idea pin (sample B) | media 344×520 at x8 with a 48 dp progress scrubber and gradient overlays; same action row below |

Action order: **React → Comments → Share → More → Save** (Save flush right). All targets are
48 dp tall; glyphs 20 dp (visual search 24 dp). The image’s top edge sits directly under the
34 dp status bar; side margins are 4 dp.

---

## 4. Comments  (`comments-dark-720x1640.png`)

Comments are a **full-screen page**, not a bottom sheet (no drag handle):

* App bar **56 dp**: back 48 dp hit (chevron), centred title `Pin van <pinner>` **16 sp/700**,
  share icon right.
* Heading **“3 opmerkingen”** **20 sp/700** at x16.
* Rows: avatar **40 dp** circle; author **13 sp/700** + timestamp **12 sp** grey; body
  **14–16 sp**; some comments carry attached pin thumbnails (stacked, radius 8).
* Per-comment actions: **“Beantwoorden”** (Reply) **13 sp/700**, heart (like) icon, “…” 24 dp.
* Emoji reaction bar (~48 dp) above the composer: emoji chips ~40 dp + “Geweldig! ❤️” pill.
* Composer: pill **328×52 dp** at x16 (16 dp margins), radius 26, placeholder
  “Voeg een opmerking toe” **16 sp**, emoji + image icons 24 dp right.
* System nav bar 47 dp at bottom.

`sizes are screenshot-measured` (Compose shell only from uiautomator).

---

## 5. Save flow

### 5.1 Board picker  (`save-board-picker-dark-720x1640.png`)

Tapping **Save** opens a bottom sheet over the dimmed close-up:

* Sheet top ≈ **y323 dp**, bottom **773 dp** → **450 dp** tall; top corners **24 dp**;
  fill **#222226**; scrim behind.
* Header **64 dp**: close **X 48×48** at left 16, centred title **“Bewaren op bord” 16 sp/700**.
* Board rows **≈64 dp**: cover thumb **48×48** (radius 8) at x16; board name **16 sp** at x72.
  Sampled: art, Dingen om te kopen, how_to_make_that_one_friend…, meme, vocaloid stickers.
* Last row **“+ Bord maken”** with a 48 dp plus tile.
* No confirm button: tapping a board saves immediately (not exercised read-only).

### 5.2 Create-board sheet  (`create-board-sheet-dark.ui.xml`)

Full-height sheet (y34–773) with header 64 dp:

* title “Een bord maken” **16 sp** at (72, 55); close **48×48** at (8, 42);
* cover preview **215×145** at (72, 126), images 142×144 + 2×72×72;
* **board-name field 328×66 dp** at (16, 295) with a 32 dp floating label;
* **private toggle row 328×120** at y370 with a **48×24 switch** at x296;
* group-board row **328×42** at y506 with a **36×36** add button;
* sticky **“Bord maken” 328×48 dp** at y701 (16 dp side margins).

---

## 6. Profile / Saved  (`profile-saved-dark-720x1640.png` — not published)

Reached from the right-hand floating nav tile (“Opgeslagen”):

* Header: avatar tile top-left; **3 tabs: Pins · Borden · Collages** (16 sp, active indicated);
  settings gear right; search field “Je pins doorzoeken” (~48 dp pill) with a **+** create button.
* Filter chips: “1↓” (sort) and “Groep” (Group).
* Board grid **2 columns**: stacked cover thumbnails, board name **~14 sp**, meta
  “8 pins · 3u” **12 sp** grey.
* Floating nav remains (“Opgeslagen” active).
* `uiautomator` not available (Compose shell); screenshot-measured.

---

## 7. Inbox / Messages  (`inbox-messages-dark-720x1640.png` — not published) ⚠

**Contains the user's personal notifications — do not publish/reuse the content.**

* App bar “Berichten” **~20 sp/700**, back arrow, edit/compose icon.
* Sections: **Activiteit** (with “Alles bekijken”), **Berichten** (messages),
  **Meldingen** (notifications); rows have avatar ~40 dp, text 14–16 sp, metadata 12 sp grey.
* No dividers; 16 dp paddings; no dump (Compose).

---

## 8. Create sheet  (`create-sheet-dark-720x1640.png`)

Header **+** opens a small bottom sheet (top ≈ y465 dp, radius 24):

* Title **“Begin meteen met maken” 16 sp/700** left, close **X 48×48**.
* Three options: **Pin · Collage · Bord** — ~92 dp rounded tiles (radius ~20) with 32 dp icons
  and 14 sp labels.
* No drag handle; content behind dimmed; no bottom nav.

---

## 9. Pre-login (emulator, light)  (`login-wall-unauth-1080x2400.png`)

Android-12 splash: white, centred red Pinterest logo. Then `UnauthActivity`:

* photo-collage hero (5 rounded images), Pinterest logo **40 dp**;
* heading “Create a life / you love” **28 sp/700**;
* email field **379×64 dp** (1 dp **#91918C** border, radius ~12–16, label 12 sp, text 16 sp);
* **Continue 379×48 dp**, radius 16, disabled **#E5E5E0** fill + **#91918C** label;
* “Or”; **Continue with Google 379×48 dp**, white with 1 dp **#91918C** border;
* terms **12 sp #62625B** with underlined links;
* 16 dp side margins; content insets 48.8 dp top / 24 dp bottom (capture shows a white top band;
  no system-bar glyphs were drawn).

No guest mode exists in this version (no `guest`/`continue as guest` strings; `unauth_*` copy
only), so this is the only pre-login surface.

---

## 10. Tokens from the APK resources  **[res]**

Pin Sans ships in the APK (`raw_pin_sans_{regular,medium,bold,heavy}[_italic].{ttf,otf}` plus
Roboto fallbacks).

### 10.1 Light-theme colours (`sema_color_*`, resolved)

| token | value |
|---|---|
| background default / elevated opaque | **#FFFFFF** |
| elevated opaque secondary | **#E5E5E0** |
| elevated opaque subtle | **#F6F6F3** |
| text default / icon default | **#000000** |
| text primary / icon primary / brand | **#E60023** |
| text subtle | **#62625B** |
| text link | **#2B48D4** |
| text inverse | #FFFFFF |
| border interactive | **#91918C** |
| border decorative | **#C8C8C1** |
| background selected | **#33332E** |
| background checked | #435EE5 |
| background error | #B60C0C |
| scrim (translucent) | `#66757570` (40 % neutral) |
| scrim (opaque) | `#CCF5F4ED` (80 % warm white) |
| greys | 25 #FBFBF9 · 50 #F6F6F3 · 100 #E5E5E0 · 150 #C8C8C1 · 200 #91918C · 250 #74746C · 300 #62625B · 350 #494943 · 400 #33332E · 450 #242421 · 475 #181816 · 500 #000000 |
| blues | 50 #EBEEFF · 100 #DBE1FF · 200 #92A4FF · 300 #677EEE · 400 #435EE5 · 500 #2B48D4 · 600 #273EB0 · 700 #17287D · 800 #0D1645 |
| reds | 50 #FFEBEB · 100 #FFD7D7 · 200 #FF8F8F · 300 #F84F4F · 400 #DD0E0E · 500 #B60C0C · 600 #900909 · 700 #6B0000 |

### 10.2 Dark theme (measured on the phone)

page **#18181B**, elevated surfaces **#222226**, nav tiles **#2C2C30**, text **#FFFFFF**,
captions/metadata grey ≈ #A0A0A5, brand **#E60023**, sheet scrim ~50 % black.
(These rendered dark values differ slightly from the resource grey scale; treat them as
Compose/Material dark tokens.)

### 10.3 Type scale & usage

sp: **12 / 14 / 16 / 20 / 24 / 28 / 36 / 48 / 60 / 72**, line-height 1.1–1.6.
Observed: login heading 28/700; section headers 20/700; sheet/action titles, pin title and
button labels 16; chips 16; comment body 14–16; captions/counts/meta/nav labels **12**.

### 10.4 Spacing, radii, components

* Spacing: 0, 1, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64 dp.
* Radii: 0, 4, 6, 8, 12, 16, 20, 24, 28, 32; pill 999; circle 50 %.
* Bottom nav: `bottom_nav_height = 54 dp` (legacy full-width), `bottomnav_icon_dimen = 28 dp`
  (floating), avatar bg 30 dp, empty badge **6 dp**, border 1 dp. Live floating variant:
  3 × 60 dp tiles, 20 dp icons, 12 sp labels.
* Buttons: large min height **48 dp**, radius **16 dp**, label **16 sp**, h-pad 16 / v-pad 8,
  icon 16 (+6 padding); small min height **36 dp**, label 14 sp, radius 12 dp; shadow 8 dp
  (blur 12–16, opacity 0.12–0.2).
* Sheets: top radius **24 dp**, bottom 0; header min height **64 dp**; outer padding **16 dp**;
  button group top pad 8 dp; blur 24.
* Comments: grabber **64×4 dp**; composer padding 12 dp; comment text top margin 6 dp.
* Close-up: rounded image **8 dp**; image max height **564 dp**; unified action bar **80 dp**;
  action button **44 dp**; overlay button **56 dp** (8 dp padding); drawer collapsed 64/80 dp;
  spacing 5/10/15 dp; avatar margin 6; back top margin 4; composer text padding 6.
* Board picker: content vertical margin 16 dp; padding 8 dp.
* Tags: sm radius 8, lg 12. Badges: radius 4. Toast: radius **16 dp**.
* Search field: border **1 dp** default / **1.5 dp** tapped; text **16 sp**; helper 12 sp;
  elevation 0; container blur 0 light / 24 dark.
* Icon buttons: lg 48 dp box / 24 dp icon; md 36 dp; sm 24 dp.
* Grid cells: inner padding 4 dp (compact 3, small 2), text padding 6 dp, no-card padding 6 dp;
  CTA radius 16, heights 26/32/36.
* Avatars: profile xxl **120 dp**, nav avatar 30 dp.
* Notifications: action icon 32 dp, text 13 sp, large icon 64 dp.

---

## 11. Mapping to PinFall Android @432 logical px

Native is authored at **360 dp**. Keep component dimensions and stretch horizontal widths; do
not scale fonts by 1.2.

| element | native (360 dp) | PinFall @432 |
|---|---|---|
| insets | 34 top / 47 bottom (3-button) | use real WindowInsets, edge-to-edge |
| home header | 48 dp: logo 142×26, create 48, messages 48 + 6 dp badge | 48 dp bar, 26 dp logo, 48 dp targets, 6 dp red badge |
| chips | 48 dp row, 16 sp labels, 24 dp gaps, 2 dp underline | same, stretched width |
| feed | 2 cols, card **174**, inset **4**, gutter **4**, radius 16, 12 sp captions | 2 cols, card **(432−2·4−4)/2 = 210**, keep 4/4, radius 16, **captions 12 sp** |
| video badge | ~40×24 dp white pill r8, top-left | same |
| floating nav | 3 × 60 dp tiles, 8 dp gaps, 20 dp icons, 12 sp labels, 76 dp bar | 60–66 dp tiles centred, 8–10 dp gaps, 20–24 dp icons, 12 sp labels + bottom inset; **3 tabs** |
| close-up | image 352 wide @4 dp, radius 8; back 48 at (8,4); action row 48 targets, Save **104×48** r16 | width 424, radius **8**, 4 dp margins; back 48 r16; Save ~104×48 r16 |
| pinner/title | avatar 16, name 12, title 16/700, more 16/700 | same |
| comments | full screen: bar 56, heading 20/700, avatars 40, composer 52, emoji bar | build as a page, not a sheet |
| save | sheet r24, header 64, rows 64, thumbs 48 r8, create-board row | same; immediate save on board tap + create-board entry |
| create-board | header 64, field 66, switch 48×24, group row 42, confirm 48 | same |
| profile | avatar + Pins/Borden/Collages tabs + search/plus + board grid | add profile tabs and board cards |
| theme | dark #18181B / #222226 / #2C2C30; light #FFF/#E5E5E0 | add dark palette + system theme |
| font | Pin Sans (bundled) | bundle Pin Sans if licensing allows, else Roboto |

### 11.1 Top deltas vs our current app (mobile-web based)

1. **Nav model** — native is a **floating 3-tile bar** (60 dp tiles, 20 dp icons, 12 sp labels,
   no Create tab, no full-width bar); ours is 4 full-width tabs. Create moves to the header;
   Inbox is a header icon with a 6 dp badge.
2. **Home chrome** — native has a **48 dp header** (logo/wordmark + create + messages) above a
   48 dp chip row; ours has only the chips strip.
3. **Feed anatomy** — native cards are **174 dp with 4 dp inset/gutter and captions (12 sp)
   under pins**; ours are web-derived image-only 210 dp cards with 1 px gutters.
4. **Close-up** — native image is **4 dp-inset with 8 dp radius**, back is 48 dp at 4 dp below
   the status bar, Save is **104×48**, action glyphs are 20 dp in 48 dp boxes; ours used 16 dp
   radius, 40 dp hits and a 68.5×48 Save.
5. **Comments** — native is a **full-screen page** (56 dp bar, 20 sp heading, 40 dp avatars,
   reply, emoji reaction bar, 52 dp composer), not a bottom sheet.
6. **Save flow** — native sheet **radius 24**, header **64**, rows **64**, thumbs **48**,
   immediate save; separate create-board sheet (66 dp field, 48×24 switch, 48 dp confirm).
   Ours planned 32-radius sheets and a 37×5 grabber.
7. **Profile** — native has **Pins / Borden / Collages** tabs, search+plus header and board
   cards; ours has none of that.
8. **Theme/type** — native supports dark (#18181B/#222226/#2C2C30) and uses Pin Sans with
   **12 sp nav labels** and 16 sp chips; ours is light-only with #211922/#CDCDCD and no dark
   palette.

Secondary: the app reads system insets (34 dp top / 47 dp bottom on this device) instead of a
fixed footer, and the floating nav overlays feed content rather than reserving a band.

---

## 12. Method & limitations

* Read-only: only navigation taps were injected; the save picker/comments/create sheets were
  opened but no board, comment, like or publish was submitted.
* `uiautomator` exposes real bounds for Home, Close-up and the create-board sheet; Search,
  Comments, Save picker, Profile/Saved, Inbox and Create sheet return only the Compose shell
  (or time out with “could not get idle state”), so those are screenshot-measured.
* Phone is in **dark mode + Dutch**; light-theme values are from the APK resources and the
  emulator login wall. To capture light-mode in-app screens, change the phone/app theme (not
  done — that alters user settings).
* `inbox-messages-dark-720x1640.png` contained **personal notifications** and is therefore not published.
* Not observed/measured: native toast/snackbar after a save (would be a write), ad/sponsored
  markers in the sampled feed, hover/press states, exact caption font weight.
* Emulator instability: the 14.42 APK installs and runs on Android 15 x86_64, but the host
  killed the emulator three times under memory pressure (1–2 GB RAM); the login wall and splash
  were captured before each stop.

## 13. Files

```
docs/UI_ANDROID_APP.md
assets/ref/native/
  01-home-dark.png / 01-home-dark.ui.xml
  02-closeup-dark.png / 02-closeup-dark.ui.xml
  home-dark-720x1640.png
  closeup-dark-720x1640.png
  comments-dark-720x1640.png
  save-board-picker-dark-720x1640.png
  create-board-sheet-dark.ui.xml
  search-dark-720x1640.png
  (profile-saved / inbox-messages captures withheld — personal account)
  create-sheet-dark-720x1640.png
  login-wall-unauth-1080x2400.png / .ui.xml
  splash-android12-1080x2400.png
```

Related: `docs/UI_MOBILE.md` (mobile-web spec), `docs/UI.md`, `docs/UI_CLOSEUP.md`,
`docs/UI_TOKENS.json`.

## Note on withheld captures

`profile-saved-dark-720x1640.png`, `inbox-messages-dark-720x1640.png` and
`assets/ref/mobile/09-profile-saved.png` are captures of a personal signed-in
account (boards, notifications, handle) and are intentionally **not** part of
this repository.
