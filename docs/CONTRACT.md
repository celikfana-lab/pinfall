# PinFall — project contract

An alternative desktop Pinterest client: **C11 + SDL2 + SDL2_ttf + libcurl + libjpeg
(vendored stb_image for decode)**.

Module map:

| Area | Files |
|---|---|
| Contract, README, integration, build | `include/pc.h`, `README.md`, `docs/CONTRACT.md` |
| API reverse engineering | `docs/API.md`, `tools/probe.py`, `fixtures/*` |
| UI/UX reverse engineering | `docs/UI.md`, `docs/UI_TOKENS.json`, `assets/ref/*` |
| Network / API / JSON / image layer + build | `Makefile`, `vendor/*`, `src/http.[ch]`, `src/jsonx.[ch]`, `src/api.c`, `src/img.c`, `tools/dump.c` |
| SDL2 UI | `src/main.c`, `src/ui.c`, `src/render.[ch]`, `src/layout.[ch]`, `src/font.[ch]`, `src/theme.h` |

Rules:
- `include/pc.h` is **frozen**. A change must land on both sides at once.
- `src/*.c` sets are disjoint by design: the network layer and the UI never share
  a translation unit.
- The Makefile globs `src/*.c` for the GUI binary and uses an explicit list for
  `tools/dump.c`.

## Verified API facts (2026-10-27)

Base host: `https://nl.pinterest.com` (env `PC_HOST` overrides). Using
`www.pinterest.com` from this machine returned 403 — stay on `nl.`.

### Guest session bootstrap
```
GET https://nl.pinterest.com/     (browser UA, Accept: text/html, Accept-Language)
```
Sets cookies `_pinterest_sess`, `_routing_id`, `csrftoken`, `_auth=0`.
Response also contains `<script id="__PWS_INITIAL_PROPS__" type="application/json">`.

### Resource RPC (this is the real API)
`POST https://nl.pinterest.com/resource/<ResourceName>/get/?source_url=<enc>` with
**form-encoded body** `data=<urlencoded JSON>&source_url=<urlencoded path>`.

Required headers (verified):
```
X-Requested-With: XMLHttpRequest
X-CSRFToken: <csrftoken cookie value>
X-Pinterest-AppState: active
X-Pinterest-Source-Url: /search/pins/?q=cats
Origin: https://nl.pinterest.com
Referer: https://nl.pinterest.com/search/pins/?q=cats
Content-Type: application/x-www-form-urlencoded; charset=UTF-8
```
Failure modes observed:
- GET instead of POST → `403 Invalid Resource Request`
- missing `X-CSRFToken` → `403 Bad CSRF token`
- `data` as raw JSON body (not form-encoded) → `400`

Working example (search):
```json
data = {"options":{"query":"minimalist bedroom","scope":"pins","page_size":25,"bookmarks":[]},
        "context":{}}
```
→ HTTP 200, ~25 pins in `resource_response.data.results`,
`resource_response.bookmark` = next-page cursor.

### Response shape — IMPORTANT gotcha
`results[]` items are **not uniform**:
1. **Pin-shaped** object: keys `id`, `images{170x,236x,474x,736x,orig}`, `title`,
   `description`, `board{name,images}`, `pinner`, `domain`, `link`,
   `dominant_color`, `orig`.
2. **Story wrapper**: `type:"story"` with `objects: [ {type:"pin"|"board"|"user"}, ... ]`.
   Recurse into `objects`, ignore non-pin types.

Image CDN: `https://i.pinimg.com/{236x,474x,736x,originals}/<aa>/<bb>/<cc>/<hash>.jpg`
(served as JPEG; 736x ≈ 72 KB).

## Environment (already verified by Lead)
- gcc 15.2, `pkg-config sdl2` 2.32.4, `SDL2_ttf` (lib + header), libjpeg, libpng,
  libcurl, pthread — all present and linkable.
- `libsdl2-image-dev` is **NOT** installed and there is **no sudo**.
  → decode with vendored `stb_image.h` (`vendor/`), fallback libjpeg if needed.
- Fonts available under `/usr/share/fonts/truetype/dejavu/`.
- X display `:0` at 1920x1080 exists, so the GUI can be run and screenshotted
  with ImageMagick `import -window root`.
- **Disk is 99% full (1.8 GB free)** — keep caches in memory (64 MB cap), cap
  fixture/asset sizes, no bulk image dumps.

## Acceptance (Lead runs these)
```
make -C pinfall            # clean build, no warnings with -Wall -Wextra
./pinfall/tools/dump cats  # prints >= 20 pins with title + thumb url
./pinfall/pinfall          # window opens, grid shows thumbnails, scroll works,
                           # typing a query + Enter shows new results,
                           # click opens detail view, Esc returns
```
