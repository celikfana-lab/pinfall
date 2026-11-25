# PinFall — Pinterest guest API map (reverse-engineered 2026-10-27)

Everything below was captured from this machine, through the **guest** (not logged-in)
web session. Nothing is guessed: each claim has a status code / byte count / field
name from a real response. Where something is *not* available for guests it is
marked **BLOCKED** with the exact evidence.

Tooling: [`tools/probe.py`](../tools/probe.py) (python3 stdlib only) implements all
of this. Raw trimmed responses live in [`fixtures/`](../fixtures/).

```sh
python3 tools/probe.py session                       # cookies + csrf + sanity RPC
python3 tools/probe.py search "minimalist bedroom"   # 25 pins: id | WxH | board | title | thumb
python3 tools/probe.py search cats --pages 3         # pagination proof (0 id overlap)
python3 tools/probe.py detail 216454325827297537     # PinResource/get (legacy numeric id)
python3 tools/probe.py feed                          # proves home feed needs login (401)
```

---

## 1. Hosts — pin yourself to `nl.pinterest.com`

| Host | `GET /` | Resource RPC | Notes |
|---|---|---|---|
| `https://nl.pinterest.com` | 200 | **200** | use this (lowest bot friction from NL/EU) |
| `https://www.pinterest.com` | 200 | **403 `No CSRF cookie`** | csrftoken cookie is host-scoped to `nl.`, so www has none |
| `https://pinterest.com` | 308 → `www` | — | redirects |
| `https://de.pinterest.com` | 200 | untested | cookies would need to be re-bootstrapped on that host |
| `https://i.pinimg.com` | image CDN, no cookies, no CSRF | — | plain GET |

Env override in the C client: `PC_HOST` (probe.py honours it too).

**Gotcha:** `csrftoken` is a **host-only** cookie for `nl.pinterest.com`; switching
to `www.` mid-session reproduces the misleading `403 No CSRF cookie`.

`GET /pin/<anything>/` always returns **200** with the ~1.19 MB React SPA shell —
there is no SEO/SSR JSON in it (even for a bogus id). Do not scrape HTML.

---

## 2. Guest session bootstrap

```
GET https://nl.pinterest.com/
User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8
Accept-Language: en-US,en;q=0.9,nl;q=0.8
```
→ HTTP 200, ~1.19 MB, then cookies are set. Observed cookie jar after bootstrap:

| cookie | domain | path | secure | lifetime (observed) | purpose |
|---|---|---|---|---|---|
| `csrftoken` | `nl.pinterest.com` (host-only!) | `/` | yes | 31535999 s ≈ 365 d | value for `X-CSRFToken` header |
| `_pinterest_sess` | `.pinterest.com` | `/` | yes | 31103999 s ≈ 360 d | guest session (opaque, 192 chars) |
| `_auth` | `.pinterest.com` | `/` | yes | 360 d | `0` = **guest**; this is fine, no login needed for search |
| `_routing_id` | `nl.pinterest.com` | `/` | no | 86399 s ≈ 1 d | datacenter routing |
| `tst` | `.pinterest.com` | `/` | yes | 172799 s ≈ 2 d | tracking/session stamp |

Notes:
- `_auth=0` is sufficient. No login, no access token, no OAuth for search/detail.
- `Value` of `csrftoken` is a 32-char lowercase hex string. Read it from the cookie
  jar, **not** from HTML (the SPA shell contains no `csrftoken` literal).
- Cookie value lengths seen: `csrftoken` 32, `_pinterest_sess` 192, `_routing_id` 38, `_auth` 1, `tst` 151 chars.
- Nothing in the bootstrap response body is needed. The
  `<script id="__PWS_INITIAL_PROPS__">` payload is only app-shell state (no pins).
- Session lifetime: verified still working after ~20 minutes of continuous use. Cookie
  expiries are long (≈1 y), but the *server-side* session can be rotated at any
  time → **on 403, re-bootstrap once and retry the request**.
- Cookie jar must be sent on every `/resource/...` call *and* on `/` re-bootstrap.
  libcurl: `CURLOPT_COOKIEFILE ""` (in-memory) + `CURLOPT_COOKIEJAR`/`COOKIEFILE`
  to persist, or store the `Cookie:` header manually.

---

## 3. Transport — the resource RPC

All data calls are a single endpoint pattern:

```
POST https://nl.pinterest.com/resource/<ResourceName>/get/?source_url=<urlencoded source path>
Content-Type: application/x-www-form-urlencoded; charset=UTF-8
X-Requested-With: XMLHttpRequest
X-CSRFToken: <csrftoken cookie value>
X-Pinterest-AppState: active
X-Pinterest-Source-Url: <same source path, unencoded>
Origin: https://nl.pinterest.com
Referer: https://nl.pinterest.com<source path>

data=<urlencoded JSON> & source_url=<urlencoded source path>
```

where the JSON is exactly `{"options":{...},"context":{}}`.

**Minimal required set, measured by deleting one header at a time** (each test got
`HTTP 200` + 2 results while the others still worked):

| variant | result |
|---|---|
| all headers | 200 |
| &minus; `X-Pinterest-AppState` | 200 |
| &minus; `X-Requested-With` | 200 |
| &minus; `Origin` + `Referer` | 200 |
| &minus; `X-Pinterest-Source-Url` header | 200 |
| **GET instead of POST** | **403 `Invalid Resource Request`** |
| **no `X-CSRFToken`** | **403 `Bad CSRF token`** |
| **no session cookies** (valid `X-CSRFToken` header only) | **200 but body is the 1.19 MB HTML app shell, not JSON** → always require `resource_response` in the body |
| raw JSON body instead of form `data=` | 200 but options are misparsed (default page size) — **don't** |

So: **POST + form-encoded `data` + `X-CSRFToken` header + cookie jar** is the hard
requirement; the rest is what the real web app sends and is safe to include
(recommended, they reduce bot score).

`source_url` conventions that work:
- search: `/search/pins/?q=<urlencoded query>` (with spaces as `%20`)
- pin detail: `/pin/<id>/`
- home feed: `/`

---

## 4. Response envelope

```json
{
  "resource_response": {
    "status": "success",
    "http_status": 200,
    "code": 0,
    "bookmark": "<opaque cursor string>",
    "data": { "results": [ ... ] },
    "metadata": {},
    "endpoint_name": "..."
  },
  "client_context": { "country": "NL", "country_from_ip": "NL", "app_version": "db4bd2c", ... },
  "resource": "BaseSearchResource",
  "request_identifier": "..."
}
```

- `data` is sometimes an object with `results`, sometimes an object with domain
  fields (e.g. `PinResource` → the pin object, `BoardResource` → board object).
  **Always type-check.**
- Errors look like:
  ```json
  {"resource_response":{"error":{"status":"failure","http_status":404,"code":50,
   "message":"Pin not found.","api_error_code":50,"extra_data":{"message":"None"}},
   "data":{"param_name":"pin","param_value":"e1_Afq..."}}}
  ```
  Note: **HTTP status == envelope status**; check the HTTP code first, then
  `resource_response.error`. An `error` object can also arrive with HTTP 200 on
  some endpoints, so check `error` unconditionally.
- **Error bodies are gzip-compressed even when the request sends
  `Accept-Encoding: identity`** (magic `1f 8b`). The C client must set
  `CURLOPT_ACCEPT_ENCODING, ""` (or gunzip when the magic matches), otherwise the
  useful message is binary garbage. probe.py sniffs the magic bytes.

---

## 5. Search — `BaseSearchResource/get`

Request (`source_url=/search/pins/?q=minimalist%20bedroom`):

```json
{"options":{"query":"minimalist bedroom","scope":"pins","page_size":25,"bookmarks":[]},
 "context":{}}
```

Options that matter:

| option | values observed | effect |
|---|---|---|
| `query` | any string | search terms; empty query returns a generic feed |
| `scope` | `"pins"` | pin grid; other scopes exist but are untested |
| `page_size` | 1–100 tested (100 accepted, 91 results) | **hint for page 1 only** (see below); larger values untested |
| `bookmarks` | `[]` on page 1, `[<bookmark>]` after | pagination cursor, see §5.1 |
| `field_set_key` | `base_pins`, `legacy_grid_pins`, `unauth_react_grid_pin`, `unauth_react_main_pin` | accepted (200) but **no observable change**: same ids, same shapes |
| others the web app sends | `rs`, `source_id`, `auto_correction_disabled`, `filters`, `query_pin_sigs`, `top_pin_ids`, `journey_depth`, `corpus`, `appliedProductFilters`, `selected_one_bar_modules` | not needed; safe to omit |

Measured result counts: `page_size=8 → 7` results, `12 → 9`, `25 → 25`, `50 → 43`,
`100 → 91`. Counts are **≤ page_size** because promoted/Admarkt items are dropped
server-side for guests (`is_promoted:false` on everything returned; we did not see
ad cards in 40+ guest searches).

### 5.1 Pagination — VERIFIED

`resource_response.bookmark` is a long opaque base64 string (~700 chars). Feed it
back as the **single element of `options.bookmarks`** (a list, not a string):

```json
{"options":{"query":"minimalist bedroom","scope":"pins","page_size":25,
            "bookmarks":["Y2JVSG81V2sxcmNHRlpWM1J5VFVad1ZscEhSbE5XYTNC..."]},
 "context":{}}
```

Measured proof (`minimalist bedroom`, chained bookmarks, 1.3 s between calls):

| page | results | extracted pins | pins already seen on earlier pages |
|---|---|---|---|
| 1 | 25 | 25 | — |
| 2 | 25 | 25 | **0** |
| 3 | 25 | 25 | **0** (cumulative 75 unique) |

So page 2/3 are genuinely different pins; `options.bookmarks=[bookmark]` is correct.
Notes:
- `page_size` on **continuation** pages is ignored: page 2 with `page_size=8`
  returned **20** results and with `page_size=50` also **20** — the server decides
  the page size once the cursor exists. Size your fetch buffers for ≥25 items
  regardless of the `page_size` you asked for.
- `bookmark` is present on every page of a normal search; if it ever equals
  `"-end-"` treat it as end-of-feed. Also stop when a page yields 0 new ids.
- An empty `bookmarks: []` (or absent) means "first page".

---

## 6. Result shapes — the dual shape trap

`resource_response.data.results[]` is a mixed array. Two shapes, and they nest.

### 6.1 Raw pin (the common case)

```json
{
  "type": "pin",
  "id": "e1_AfqPShzej1DH-Pkf0TZw2_vLinxJCbgi41qvC486BTdaVDgyPB1w1A",
  "node_id": "UGluOmUxX0FmcVBTaHplajFESC1Qa2YwVFp3Ml92TGlueEpDYmdpNDFxdkM0ODZCVGRhVkRneVBCMXcxQQ==",
  "title": "Warm Minimalist Japandi Bedroom Decor",
  "grid_title": "Warm Minimalist Japandi Bedroom Decor",
  "description": "I am completely obsessed with this warm minimalist bedroom setup...",
  "dominant_color": "#8c7359",
  "image_signature": "477d5cd5eb2c60bd19237f3a275dad48",
  "images": {
    "170x": {"url": "https://i.pinimg.com/236x/47/7d/5c/477d...c48.jpg", "width": 236, "height": 354},
    "236x": {"url": "https://i.pinimg.com/236x/47/7d/5c/477d...c48.jpg", "width": 236, "height": 354},
    "474x": {"url": "https://i.pinimg.com/474x/47/7d/5c/477d...c48.jpg", "width": 474, "height": 711},
    "736x": {"url": "https://i.pinimg.com/736x/47/7d/5c/477d...c48.jpg", "width": 736, "height": 1104},
    "orig": {"url": "https://i.pinimg.com/originals/47/7d/5c/477d...c48.jpg", "width": 1152, "height": 1728}
  },
  "board": {"id": "1099371027728817859", "name": "Interior Design Bedroom",
            "url": "/InteriorDesignStylesIdeas/interior-design-bedroom/", "pin_count": 26,
            "owner": {"id": "1099371096446415438", "username": "InteriorDesignStylesIdeas"},
            "images": {"236x": [ {"url": "...", "width": 236, "height": 419, "dominant_color": "#977653"} ]}},
  "pinner": {"id": "1099371096446415438", "username": "InteriorDesignStylesIdeas",
             "full_name": "Christine | ...", "follower_count": 24},
  "domain": "Uploaded by user", "link": null, "created_at": "Sun, 11 Oct 2026 11:01:14 +0000"
}
```

The search payload is **already the full pin** (title, description, all image
sizes, board, pinner, link, dominant_color) — a detail screen can be built from it
without a second request (see §7.3).

### 6.2 Story wrapper (mixed content)

`results[]` may contain story cards, e.g. `query=cats` page 1 → **18 results =
17 raw pins + 1 story**. The story holds the pin/board objects:

```json
{
  "type": "story",
  "id": "…", "story_type": "…", "title": "…",
  "objects": [
    {"type": "board", "id": "902057069054213222", "name": "…", "url": "/x/y/",
     "pin_count": 24,
     "images": {"236x": [ {"url": "https://i.pinimg.com/236x/…jpg", "width": 236,
                           "height": 419, "dominant_color": "#977653"} ]}},
    {"type": "board", "id": "902057069054216235", …}
  ]
}
```

`objects` may contain `type:"pin"`, `"board"` or `"user"`; **only pins are pins**.

### 6.3 THE trap: boards also have `id` + `images`

| | pin | board |
|---|---|---|
| `images` | dict `{size: {url,width,height}}` | dict `{size: [ {url,…} ]}` — **list** |
| `type` | `"pin"` | `"board"` |
| pin-only fields | `dominant_color`, `pinner`, `board`, `orig`, `image_signature` | `pin_count`, `url`, `owner` |

Do **not** classify by "has `id` and `images`" or by "`images.236x` exists" — a
board passes both. Require `type=="pin"`, or (when `type` is absent on older
payloads) `images` being a dict of **dicts** *and* a pin-only field present.

### 6.4 Extraction algorithm (pseudocode; mirrored in probe.py)

```
collect_pins(container):
    emit = []
    visit(container, depth=0):
        if depth > 4: return
        if list: for e in …: visit(e, depth+1); return
        if not dict: return
        if e.type == "story" or (is_list(e.objects) and not is_pin(e)):
            visit(e.objects, depth+1)          # wrapper, not an item
            return
        if is_pin(e): emit += [e]

is_pin(e):
    if e.type == "pin": return true
    if e.type in {"board","user","story"}: return false
    if "objects" in e or "content_ids" in e: return false      # story card
    imgs = e.images
    if imgs is not dict or e.id is missing: return false
    if count(v is dict for v in imgs.values()) == 0: return false
    return e.dominant_color or e.orig or e.pinner or e.board or e.image_signature

image_for(pin, wanted):
    for size in [wanted] + ["236x","474x","736x","170x","orig"]:
        v = pin.images[size]
        if v is list: v = first dict in v          # multi-image pins
        if v is dict and v.url: return v
    return none
```

Feed the grid with `236x` (10–11 KB each), the detail/lightbox with `474x`/`736x`.

### 6.5 id format warning for C

`id` is a **string** and currently looks like
`e1_AfqPShzej1DH-Pkf0TZw2_vLinxJCbgi41qvC486BTdaVDgyPB1w1A` (48+ chars, base64url,
`-`/`_` included). Legacy numeric ids (15–19 digits) still exist for older pins.
**Never store pin ids in `uint64_t`/`long`** — use `char[64]`+length or an owned
string. `node_id` is `base64("Pin:" + id)`; ignore it.

---

## 7. Pin detail — `PinResource/get`

### 7.1 It works for legacy numeric ids (guest!)

```json
{"options":{"id":"216454325827297537","field_set_key":"detailed"},"context":{}}
```
`source_url=/pin/216454325827297537/` → **HTTP 200**, 15,433 bytes, 117 fields
(`type:"pin"`): full `description`, `images` with `60x60,136x136,170x,236x,474x,564x,736x,600x315,orig`,
`dominant_color`, `board`, `pinner`, `link`, `domain`, `created_at`, `repin_count`,
`comment_count`, `video_status`, `rich_metadata`, `story_pin_data`, … as in §6.1.

`field_set_key` values that return 200 for such ids:
- `"detailed"` — canonical, 15.4 KB
- `"unauth_react_main_pin"` — what the guest web closeup asks for, 17.4 KB (a superset)
- `"unauth_ssr_lazy_load"` — accepted

### 7.2 It does NOT work for the `e1_…` ids that search returns — BLOCKED (guest)

For every `e1_` id taken from `BaseSearchResource` results (3/3 tested):

```
POST /resource/PinResource/get/?source_url=/pin/<id>/
data={"options":{"id":"<e1_…>","field_set_key":"detailed"},"context":{}}
→ HTTP 404, 2721 bytes
{"resource_response":{"error":{"status":"failure","http_status":404,"code":50,
  "message":"Pin not found.","api_error_code":50,"extra_data":{"message":"None"}},
  "data":{"param_name":"pin","param_value":"<e1_…>"}}}
```
Same with `field_set_key:"unauth_react_main_pin"` / `"unauth_ssr_lazy_load"`, and
with `source_url` = the search URL. 400 `Pin id is not provided` when the option
key is `pin`/`pin_id` instead of `id` (so `id` is the right key — the id resolves
but the pin is "not found").

Also BLOCKED through the modern GraphQL path (documented here for completeness):

```
POST https://nl.pinterest.com/_/graphql/
Content-Type: application/json
X-CSRFToken, X-Requested-With: XMLHttpRequest, X-Pinterest-AppState: active,
X-Pinterest-Source-Url: /pin/<id>/, X-Pinterest-GraphQL-Name: CloseupPageQuery
body: {"queryHash":"5259cb3cb95959fbfe0ad596663c4a02e467a77f6ffbf3844df4628f60a96a1a",
       "variables":{"pinId":"<id>","isAuth":false,"isDesktop":true,"isUnauth":true,
                    "isAuthDesktop":false,"shouldPrefetchStoryPinFragment":false}}
→ HTTP 200 {"data":{"v3GetPinQueryv2":{"__typename":"PinNotFound","__isError":"PinNotFound"}}}
```
(the same call with a **numeric** id returns the full closeup payload,
`__typename:"PinResponse"`; the persisted-query hash is from the 2026-10 web bundle
of `CloseupPageQuery`).

Independent confirmation: real headless Chrome loading
`https://nl.pinterest.com/pin/<e1_…>/` renders a `PinNotFound` page (4× the string
in the DOM), while `https://nl.pinterest.com/pin/216454325827297537/` renders the
pin with its title. So this is not a UA/header issue on our side.

### 7.3 Recommended design for PinFall

1. Grid: `BaseSearchResource/get` (§5), keep the **whole pin object** in the
   in-memory cache keyed by `id` (it already has title/description/images/board/
   pinner/dominant_color). Cap the cache (e.g. 200 pins) — objects are 4–10 KB.
2. Detail view: render from the cached search object — **zero extra requests**.
3. `probe.py detail <e1_…>` therefore exits 1 with an explanation; `probe.py detail
   <numeric>` proves `PinResource/get` works if a legacy id ever appears.
4. Do not ship a home feed (§10) or any "related pins" call; both need login.

---

## 8. Images — `i.pinimg.com` ladder

URL is built from the pin's `images` dict — never hand-edit except when you want a
different size. Pattern:
`https://i.pinimg.com/<spec>/<aa>/<bb>/<cc>/<image_signature>.jpg`
where `<aa><bb><cc><sig>` = `image_signature` split into 2/2/2 chars + rest.

Measured for one article (`sig=477d5cd5…dad48`, plain GET, no cookies):

| spec | HTTP | Content-Type | bytes | notes |
|---|---|---|---|---|
| `60x60` | 200 | image/jpeg | 1,810 | avatar/cover |
| `170x` | 200 | image/jpeg | 10,543 | **byte-identical to 236x** (aliases to 236x) |
| `236x` | 200 | image/jpeg | 10,543 | **use this for the grid** |
| `474x` | 200 | image/jpeg | 35,617 | lightbox |
| `564x` | 200 | image/jpeg | 47,910 | |
| `736x` | 200 | image/jpeg | 71,799 | detail / full-screen |
| `1200x` | 200 | image/jpeg | 137,018 | |
| `originals` | 200 | image/jpeg | 124,563 | pin's `orig` may be smaller; prefer the URL in `images.orig` |
| any spec + `.webp` | **403** | `application/xml` | 243–263 | **webp is NOT available** on this path (S3 AccessDenied); do not probe it |

Rules:
- Always take the **URL from the JSON** (`images[spec].url`); it already contains
  the correct spec and path. Only substitute the spec segment if a size is missing.
- `images` in search results may lack `originals`; detail may include extras
  (`564x`, `600x315`). Order of preference: `236x` → `474x` → `736x` → `170x` → `orig`.
- All sizes are JPEG. libjpeg/stb_image both decode them; no EXIF orientation issue
  observed (dimensions in JSON match decoded image).
- `dominant_color` is a **lowercase** `"#rrggbb"` string on pins
  (`"#8c7359"`), but **uppercase** inside `board.images[...].dominant_color`
  (`"#A59275"`). Normalise before use: parse with `%x`, not by case comparison.
  Missing on some pins → fall back to a neutral grid placeholder colour.
- Aspect ratio: use `images[spec].width/height` (e.g. 236×354); heights vary a lot
  (294–423 px for a 236 width) → masonry layout needs real JSON dimensions.

---

## 9. Rate limiting / bot detection (measured)

| experiment | result |
|---|---|
| 8 × `BaseSearchResource` sequential, 1 request/s, different queries | **8/8 HTTP 200**, 0.54–0.93 s each |
| 6 × same endpoint back-to-back, **no sleep** (burst) | **6/6 HTTP 200** — no 429 |
| ~10 preceding resource calls within a few minutes | no 429/403 |
| one response | took **6.06 s** (soft throttle signal, still 200) |
| GET instead of POST / missing CSRF | 403 (protocol error, not rate limiting) |

We never observed `429` in >60 guest requests across the whole RE session. Pinterest does not advertise
`Retry-After` in the responses seen. Treat 403/429 as "slow down / re-bootstrap",
not as a hard ban.

**Recommended politeness policy for the client:**
- One in-flight resource request at a time; **≥ 0.8 s** between resource calls
  (probe.py default 0.8 s; 1.0 s for pagination loops is fine).
- Never parallelise search/detail fetches during scrolling; debounce typing
  (e.g. 500–700 ms after the last keystroke) and cancel superseded queries.
- Exponential backoff on 403/429: 2 s, 4 s, 8 s (max 3 tries), then show an error
  and keep the current results on screen. On the *first* 403, re-bootstrap the
  session once (GET `/`, new csrftoken) and retry once.
- Cache: session + parsed pins in memory (cap 64 MB per project contract, e.g.
  200 pins × 10 KB); images on-demand only for visible cells, with an LRU of
  decoded thumbnails; never prefetch originals.
- Send a normal browser UA (above). No need for other headers to pass, but send
  the full recommended set anyway.
- Do not fetch the home feed or any logged-in resource — those 401 (below) and
  only raise the bot score.

---

## 10. Home feed — BLOCKED (login required)

```
POST /resource/UserHomefeedResource/get/?source_url=/
data={"options":{"page_size":10,"bookmarks":[]},"context":{}}
→ HTTP 401 (body is gzip!), decoded:
{"resource_response":{"error":{"status":"failure","http_status":401,"code":2,
  "message":"Authentication failed.","message_detail":"Missing access token",
  "api_error_code":2,"extra_data":{"message":"Missing access token"}},"data":null}}
```
So there is no guest home feed. The app should start on a default search
(e.g. `"minimalist bedroom"`) or a saved-query list. Do not retry on 401.

`BaseSearchResource` with `query:""` returns a generic pin list (works as a
"discover" feed); use that if a landing screen is needed.

---

## 11. Error taxonomy (HTTP → meaning → action)

| HTTP | body / `error.message` | observed when | action |
|---|---|---|---|
| 200 | `status:"success"` | normal | parse `data.results` / `data` |
| 200 + `error{}` | any | rare (some resources) | treat like the listed error |
| 400 | `{"code":20,"message":"Pin id is not provided"}` | `PinResource` with `options.pin`/`pin_id` instead of `id` | fix request; don't retry |
| 401 | `Authentication failed.` / `Missing access token` (`code:2`) | `UserHomefeedResource`, any logged-in resource | not available to guests; do not retry |
| 403 | `Invalid Resource Request` | GET instead of POST | fix method |
| 403 | `Bad CSRF token` | missing/stale `X-CSRFToken` | re-bootstrap session, retry once |
| 403 | `No CSRF cookie` (14 bytes, plain) | resource call on `www.` (host-only csrftoken) | use `nl.` host |
| 404 | `{}` (empty object, ~208 bytes) | **unknown resource name** (e.g. `CloseupResource`, `PinEntityResource`) | wrong resource name |
| 404 | `{"code":50,"message":"Pin not found.","api_error_code":50}` | `PinResource` with an `e1_…` id (§7.2) | use cached search data; don't retry |
| 429 | — | not observed | back off 2/4/8 s, max 3 tries |
| 5xx | — | not observed | back off, surface a retryable error |

Also: `resource_response.data` may be `null` while HTTP is 200 (e.g. deleted pin);
guard every dereference.

---

## 12. Fixtures (offline parser tests, <200 KB total)

| file | bytes | contents |
|---|---|---|
| `fixtures/search_page1.json` | ~19 KB | `BaseSearchResource` page 1, 8 raw pins, includes `bookmark` |
| `fixtures/search_page2.json` | ~56 KB | page 2 fetched with `bookmarks:[page1.bookmark]` — 25 pins, **0 overlap** with page1 |
| `fixtures/search_cats_story.json` | ~19 KB | page 1 for `cats`: 8 pins + **1 story wrapper with 2 board objects** (`board.images.236x` is a **list** — the parser trap) |
| `fixtures/pin_detail_legacy.json` | ~3 KB | `PinResource/get` for numeric `216454325827297537`: 117-field pin with full image ladder |

Format: `{"_comment","captured","endpoint","query","page",
"resource_response":{"bookmark","data":{"results":[…]}}}` — trimmed copies of real
responses (descriptions truncated, board/pinner folded to the fields a client
needs) but with **real ids, real image URLs, real dominant_color values** and the
exact nesting/shape quirks. Regenerate with
`python3 tools/probe.py fixture search "q" --page N --out fixtures/x.json`.

---

## 13. C implementation checklist (libcurl + jsonx + SDL2)

**Session**
1. `curl_easy_init()` once; keep the handle's cookie engine on
   (`CURLOPT_COOKIEFILE, ""`) and a persistent `curl_share` or the same easy handle
   for all requests so cookies survive between calls.
2. Bootstrap: `GET PC_HOST "/"` with browser UA + `Accept-Language`. Ignore body.
   Find `csrftoken` via `CURLINFO_COOKIELIST` (parse the `Set-Cookie`-style lines
   `nl.pinterest.com\tTRUE\t/\tTRUE\t<expiry>\tcsrftoken\t<32 hex>`); set
   `CURLOPT_FOLLOWLOCATION, 1` (bare host 308s). Set timeouts
   (`CONNECTTIMEOUT 10`, `TIMEOUT 25`).
3. Keep `_pinterest_sess` and `_auth=0` automatically; never switch to `www.`.
4. Store `csrf[33]`. Re-bootstrap on 403 (once) and on process start if csrf empty.

**Requests**
5. Build the URL as `%s/resource/%s/get/?source_url=%s` with a **percent-encoder**
   that encodes `/`? No — encode the *path value* fully (`urllib.parse.quote(safe='')`
   equivalent): `source_url=%2Fsearch%2Fpins%2F%3Fq%3Dminimalist%2520bedroom`.
   Encode the search query *inside* the source_url (`/search/pins/?q=` + urlencode).
6. Build the body with a URL-encoder: `data=<urlencode(compact_json)>` +
   `&source_url=<same encoded path>`. Compact JSON (no spaces). Escape `"` `\` and
   control chars in the query; reject NUL/invalid UTF-8 up front.
7. Headers (static, all for one request):
   ```
   Content-Type: application/x-www-form-urlencoded; charset=UTF-8
   X-CSRFToken: <csrf>
   X-Requested-With: XMLHttpRequest
   X-Pinterest-AppState: active
   X-Pinterest-Source-Url: /search/pins/?q=<query>
   Origin: <host>
   Referer: <host><source_url>
   Accept: application/json
   ```
   plus `CURLOPT_ACCEPT_ENCODING, ""` so libcurl gunzips error bodies.
8. `CURLOPT_POST, 1`, `CURLOPT_POSTFIELDS, body`, `CURLOPT_POSTFIELDSIZE`.
9. On 403: re-bootstrap csrf, retry once. On 429: sleep 2/4/8 s. On other errors:
   map to UI message using §11.

**Parsing**
10. Parse with jsonx into a generic tree, then walk with the §6.4 algorithm —
    do not assume `results[]` items are pins. Boards and story wrappers must be
    skipped/recursed; guard depth (max 4) and object count.
11. Read `pin.id` as a **string** (copy, 64-byte buffer) — never as an integer.
12. Image URL: `images[236x].url` (string) + `width/height` ints. Fall back through
    the ladder. Do **not** append `.webp`.
13. `dominant_color`: parse `"#rrggbb"` case-insensitively → `Uint32` for the
    placeholder colour while the JPEG downloads.
14. Bookmark: keep the raw **string** (up to ~1 KB) per query; send it inside
    `options.bookmarks` as a one-element array. Reset on new query.
15. Render detail from the cached search pin object (§7.3) — no extra request.

**Images**
16. Download thumbnails through the same curl handle (they need no cookies);
    limit concurrent downloads (e.g. 2–4, only for visible cells); decode with
    vendored `stb_image` (libsdl2-image is not installed on this box).
17. Respect the 64 MB cache cap: LRU of encoded thumbs + decoded textures; free
    textures for off-screen cells.

**Politeness**
18. Serialise resource calls, ≥0.8 s apart, debounce typing 500–700 ms, cancel
    stale searches (drop late responses by comparing the query generation counter).
19. Cache pins by id; never re-search for a query already shown in this session
    unless the user explicitly refreshes.

---

## Appendix A — probe.py behaviour worth copying

- Session cache: `/tmp/pinfall-cache/session.json` (`$PC_PROBE_CACHE`), reused for
  20 min, host-checked, contains cookies + csrf (not secrets beyond the guest
  session).
- `search` prints `id | WxH | board | title | thumb_url` per pin, a `# page N`
  header line and `# N unique pins` footer; `--pages N` chains bookmarks with
  `sleep 0.8`; `--pos` numbers rows; `--raw` dumps page data.
- `detail <id>` accepts a bare id or any `…/pin/<id>/…` URL; prints the pin fields
  and exits 1 with the §7.2 explanation for `e1_…` ids.
- `feed` expects (and reports) 401.
- `fixture` writes trimmed real responses for offline parser tests.

---

## 14. Authenticated session (cookie import) — verified with a simulated server

> **Status: harness-verified, not live-verified.** Everything in this section was
> verified on 2026-10-27 against a local fake Pinterest server
> (`PC_HOST` pointed at the fake), which emulates logged-in responses. No real
> Pinterest account/credentials were available, so none of it has been confirmed
> against production. Guest behaviour from §2–§11 is unchanged. Numbering: the
> C implementation checklist is §13 above; this section was appended last.

**§10 update:** the *guest* 401 in §10 is unchanged — with an authenticated jar
`UserHomefeedResource/get` returns 200 and the home feed becomes available. This
section documents that authenticated mode; §10 remains correct for guests.

### 14.1 Why cookie import (and no password flow)

PinFall **never handles passwords**. Pinterest has no supported third-party auth
API; its `/login/` page is protected by **reCAPTCHA + Arkose FunCaptcha**, and
"Continue with Google" is browser-only Google Identity Services (GSI), which
cannot be driven from a C/SDL2 client. The only workable model:

1. The user signs in normally in their own browser (including Google sign-in).
2. They export the browser's **Netscape `cookies.txt`** for `pinterest.com`.
3. PinFall imports that jar (`--cookie-file <path>`, `pc_cookie_load`) and uses it
   for every request instead of bootstrapping a fresh guest session.

There is no OAuth token, no refresh token, no password field, and no scrape of
the login page. The session *is* the cookie jar.

### 14.2 The jar: format, required cookies, security

**Format.** Netscape `cookies.txt`, 7 TAB-separated fields:

```
#domain        flag  path  secure  expiry      name            value
.pinterest.com TRUE  /     TRUE    1824214563  _pinterest_sess  <opaque>
nl.pinterest.com TRUE /     TRUE    1824646562  csrftoken        <32 hex>
```

- Lines starting with `#` are comments; blank lines are skipped.
- Chrome/Edge cookie-export extensions emit HttpOnly cookies with a
  `#HttpOnly_` domain prefix (`#HttpOnly_nl.pinterest.com\t...`) — the loader
  must strip that prefix and **not** skip the line as a comment.
- Both host-only cookies (`nl.pinterest.com`) and `.pinterest.com` domain cookies
  must be preserved exactly; do not "clean up" domains or the RPC will 403
  (see §3: csrftoken is host-scoped to `nl.pinterest.com`).
- The import **replaces** the jar atomically (drop all existing cookies, then add
  every parsed line) and must happen *before* the bootstrap/CSRF step, or the
  client will overwrite imported cookies with a guest session.

**Cookies that matter** for an authenticated session (all present in a normal
browser export, plus others that are harmless to keep):

| cookie | domain | why it matters |
|---|---|---|
| `_pinterest_sess` | `.pinterest.com` | the account session; without it every auth resource 401s |
| `_auth` | `.pinterest.com` | `1` when signed in (guest sessions have `0`) |
| `csrftoken` | `nl.pinterest.com` | value for `X-CSRFToken` on every RPC; unchanged by auth |
| `_routing_id`, `tst` | as exported | routing/tracking; keep, harmless |

A Google-signed-in browser session exports the same cookie names (Google is just
an identity provider; the resulting Pinterest session is identical), so the same
code path covers both.

**Security posture (non-negotiable):**

- The jar is **equivalent to full account access** — treat it like a password.
  Anyone with the file can like/save/comment as the user.
- If PinFall persists it (opt-in only), write with **mode 0600** and `fchmod`
  an existing file down to 0600 so a wider pre-existing mode is tightened.
- **Never log cookie values.** Log names/counts only ("imported 7 cookies, csrf
  present"). Error strings must never interpolate values. The implementation's
  `pc_http_cookie_load/save` API guarantees this by contract.
- Do not send the jar anywhere else, do not bundle a jar with the app, and warn in
  the UI that exporting/importing a jar grants account access.
- Reject files containing NUL bytes (binary/malformed), empty files, and files
  with zero valid cookie lines, with an error that contains no cookie data.

### 14.3 Auth probe — one request, ever, cached

Exactly **one** request decides logged-in vs guest, and the answer is cached for
the lifetime of the client; it is never retried:

```
POST /resource/UserHomefeedResource/get/?source_url=/
Content-Type: application/x-www-form-urlencoded; charset=UTF-8
X-CSRFToken: <csrf cookie>
(plus the usual headers from §3)

data={"options":{"page_size":10,"bookmarks":[]},"context":{}}
```

Decision table (simulated-server verified):

| result | state | action |
|---|---|---|
| `200` + body parses as pin results | **LOGGED_IN** | cache the parsed page 1 as the home-feed first page (no second request) |
| `401` (see §10: `Missing access token`) | **GUEST** | never retry; stay in guest mode |
| transport error / other status / JSON parse failure | **UNKNOWN** | treat as guest for the rest of the session; **no retry** |

Rules:
- The probe runs at most once per client (`probe_done` is set even when it
  fails) — do not call it from search or detail code paths, and do not loop.
- Never retry a 401. Never re-probe on a 401 mid-session; flip to guest instead.
- `401` is a normal, expected state, not an error to show as a failure.

### 14.4 Home feed (authenticated)

Same resource as the probe, paged with the same cursor convention as search
(§5.1):

```
data={"options":{"page_size":10,"bookmarks":[<cursor>]},"context":{}}
```
(`bookmarks:[]` for page 1.)

- Parse `data.results[]` with the **same dual-shape extractor as §6.4** — the
  feed mixes raw pins and `type:"story"` wrappers exactly like search results.
- `resource_response.bookmark` is the next cursor; keep it as an opaque string.
  `"-end-"` means **end of feed** — normalise it to empty/`NULL`, do not send it
  back as a cursor.
- Page 1 may be served from the cache populated by the auth probe; paging past
  page 1 issues the request above.
- A `401` while paging (session expired/revoked) flips auth state to GUEST,
  surfaces "login required", and must **not** be retried.
- Bookmarks can be ~1 KB strings: never truncate them (that yields silent
  duplicate/looping pages).

Content note: the feed is personalised server-side; ordering and items differ
per account and change over time, so UI tests must not assert on exact pins.

### 14.5 Username — free, from the bootstrap response

No extra request. The bootstrap `GET <host>/` (§2) already returns a ~1.19 MB
HTML shell containing:

```html
<script id="__PWS_INITIAL_PROPS__" type="application/json"> {…} </script>
```

JSON path:

```
initialReduxState.adminUser.username     → display name for the header
```

- Guest sessions have `"adminUser": null` → show "" / "Log in".
- Parse the JSON between that script tag and `</script>`; do not regex the whole
  HTML for `"username"` (unrelated `username` fields exist for boards/pinners).
- Defensive fallbacks the implementation also handles (harmless, since the real
  authenticated shape is only simulated-verified): if `adminUser` is a **string**
  id, look up `initialReduxState.users[<id>].username`; if that fails, a bounded
  recursive search for the first `username` under `initialReduxState`.
- The username is display-only. Never use it as an identity/auth decision — the
  auth state comes from §14.3.

### 14.6 Pin detail for `e1_…` ids — works when authenticated

Guest detail for the opaque `e1_…` ids is a hard 404 (§7.2), and that is
unchanged. With an authenticated jar the simulated server returns the full pin:

```
POST /resource/unauth_react_main_pin/get/?source_url=/pin/<e1_…>/
data={"options":{"id":"<e1_…>"},"context":{}}
→ 200, full pin object, parsed by the normal detail parser
```

Notes:
- `PinResource/get` with `field_set_key:"detailed"` **still 404s** for `e1_…`
  ids even when authenticated — do not use it as the primary authenticated path.
- Keep the guest path untouched: when the probe says GUEST/UNKNOWN, behave
  exactly as §7.2/§7.3 (render detail from the cached search payload).
- Best-effort fallback in the shipping code: after a 404 on `PinResource/get`
  while LOGGED_IN, it retries **once** with
  `field_set_key:"unauth_react_main_pin"` on `PinResource/get`. Which exact
  spelling (alternate resource name vs alternate `field_set_key`, and with or
  without `field_set_key`) live Pinterest accepts is **unverified** — see
  §14.7; keep the retry single-shot and never on the guest path.
- Detail responses can contain `is_video:true`/`story_pin_data`; the C client
  only needs `images`/`title`/`description`/`board`/`pinner`/`dominant_color`.

### 14.7 What is still UNVERIFIED (be honest in tests/docs)

1. **Live Pinterest acceptance** of every request shape in this section — only
   the local fake server was exercised. Notably the exact authenticated detail
   endpoint/resource spelling (§14.6) and the real `__PWS_INITIAL_PROPS__` value
   for a signed-in account (§14.5).
2. **Real-account home-feed content, ordering, personalisation and refresh
   behaviour** — only the response *shape* was simulated.
3. **Home-feed pagination beyond 2 pages** — cursor chaining was proven live only
   for search (`BaseSearchResource`); feed cursors are simulated only (and a
   fake `-end-` marker was used).
4. **Any write/save endpoint** — save pin / unsave / create board / follow /
   comment are **not implemented and never tested**; they need their own CSRF
   and write-specific headers and may be blocked for API-style clients.
5. **Session expiry, rotation and revocation** — cookie lifetimes from §2 are for
   a fresh guest jar; how long an imported browser session stays valid (and how
   the host reacts when the user logs out / changes password) is unknown.
   The only safe policy is the §14.3 one-shot probe + flip-to-guest on 401.
6. **Other authenticated resources** — boards, notifications, conversations,
   personalised search filters: not probed.
7. **Rate limits/bot scoring for authenticated accounts** — the §9 numbers are
   for guest sessions; an imported account may be scored differently (and is more
   valuable to protect: be *more* conservative, never exceed ~1 rps).

### 14.8 C implementation checklist (authenticated mode)

1. `--cookie-file <path>` → `pc_cookie_load()` **before** bootstrap; error text
   must not contain cookie values; count applied cookies for the log line.
2. Bootstrap still runs (GET `/`) to refresh `csrftoken` if the jar lacks it and
   to read the username from `__PWS_INITIAL_PROPS__`; imported session cookies
   must survive this GET (do not reset the jar).
3. Call `pc_auth_check()` once after bootstrap; cache `PcAuthState`.
   `UNKNOWN` ⇒ behave as guest; never auto-retry.
4. If LOGGED_IN: show home feed from the probe's cached page 1, page with
   `bookmarks:[cursor]`, stop at `""`/`-end-`; on 401 mid-paging flip to GUEST and
   show "login required".
5. Detail: guest → cached search payload (§7.3); LOGGED_IN → try the
   authenticated path (§14.6) once, then fall back to the cached payload.
6. Header shows the §14.5 username when non-empty, else a neutral guest label.
7. Optional export (`pc_cookie_save`) only on explicit user action, mode 0600,
   and a UI warning that the file is account-equivalent.
8. Log cookie **counts/names**, never values; never persist jars implicitly.

---

## 15. Saved pins, boards and saving (Repin)

> **Status.** The *read* endpoints and the repin auth-gate were verified **live as a
> guest** on 2026-10-27 (read-only discovery, `nl.pinterest.com`). The
> repin *success path* is **UNVERIFIED live** — it was only exercised against
> a local fake server. No write endpoint has ever been called against
> production by this project. Guest transport rules (§3), error taxonomy (§11) and
> the authenticated-session model (§14) all apply unchanged.

### 15.1 Boards — `BoardsResource/get`

The board list for a profile (public profiles readable as a guest):

```
POST /resource/BoardsResource/get/?source_url=%2F<user>%2F
data={"options":{"username":"<user>","page_size":25,
                 "field_set_key":"profile_grid_item"},"context":{}}
→ 200
```

**Response shape gotcha (same class as §6): `resource_response.data` is a
LIST of board objects, not an object with `results`.**

```json
{"resource_response":{
  "status":"success","http_status":200,
  "data":[
    {"id":"424605139808127452", "name":"Home Decor",
     "url":"/pinterest/home-decor/", "privacy":"public", "pin_count":128,
     "node_id":"Qm9hcmQ6NDI0NjA1MTM5ODA4MTI3NDUy"},
    {"id":"…", "name":"…", "url":"/pinterest/…/", "privacy":"public", "pin_count":…}
  ]}}
```
Verified with `username:"pinterest"` → 5 boards.

- `id` is a **numeric string** (15–19 digits, e.g. `424605139808127452`) — keep it
  as a string (`PC_ID_LEN`), it can exceed 64 bits in some ecosystems and it is
  what the repin call wants.
- Fields a client needs: `id`, `name`, `url` (`"/<user>/<slug>/"`), `privacy`
  (`"public"` observed; `"secret"`/`"protected"` exist upstream but were **not**
  exercised — see §15.7), `pin_count`.
- **Missing `username` → HTTP 400**, `resource_response.error.message` =
  `"username is required"`. `username:""`/`NULL` means "the signed-in user" in the
  client API and requires an authenticated jar (§14); do not send an empty
  username as a guest.
- Pagination: the bookmark key for this resource was **not** part of the verified
  discovery (the 5-board profile fit one page). Buffer defensively: if
  `resource_response.bookmark` appears, keep it as an opaque string and page with
  `options.bookmarks:[<cursor>]` (same shape as §5.1); unverified.
- Boards also carry `images` (cover candidates, **lists** — see §6.3) and an
  `owner`; only `id/name/url/privacy/pin_count` are needed. Do **not** feed board
  objects to the pin extractor.

### 15.2 Saved pins — `UserPinsResource/get` (the "Saved" tab)

```
POST /resource/UserPinsResource/get/?source_url=%2F<user>%2F
data={"options":{"username":"<user>","page_size":25,
                 "bookmarks":[<cursor>]},"context":{}}
→ 200, resource_response.endpoint_name == "v3_get_pins_handler"
```

- `resource_response.data` is a **LIST of pin objects** (same pin schema as
  §6.1: `id`, `title`, `grid_title`, `description`, `images`,
  `dominant_color`, `board`, `pinner`, …) — **not** an object with `results`.
- Top-level `resource_response.bookmark` is the next-page cursor (same
  `bookmarks:[cursor]` convention as search §5.1; `"-end-"`/empty ⇒ end).
- Works for **public profiles as a guest**; the signed-in user's own pins
  (username = own account) require the authenticated cookie jar (§14) — a guest
  jar gets 401 on own-account reads.
- The verified sample returned plain pin objects. Because search results mix in
  `type:"story"` wrappers (§6.2), still run the §6.4 extractor defensively rather
  than assuming every item is a pin; that mixing is **unverified** for this
  resource specifically.
- Same offline fixtures apply for parser tests: saved pins look exactly like
  search pins (§12).

### 15.3 Saving a pin — `RepinResource/create`

The write mutation ("repin to board"):

```
POST /resource/RepinResource/create/?source_url=%2Fpin%2F<pin_id>%2F
data={"options":{"pin_id":"<e1_… or numeric pin id>",
                 "board_id":"<numeric board id>"},"context":{}}
→ guest: HTTP 401, resource_response.error.message = "Authorization failed."
```

- The 401 as a guest proves the **resource exists and is auth-gated** (it is not
  a wrong-name 404 — compare §15.4).
- A write requires all of:
  1. an **authenticated cookie jar** (§14; `_pinterest_sess` + `_auth=1`),
  2. a valid **`X-CSRFToken`** header matching the jar's `csrftoken` cookie,
  3. `source_url` = `/pin/<pin_id>/` — the same pin you are saving, so it must
     match the `pin_id` in the body.
- `pin_id` accepts the opaque `e1_…` ids and legacy numeric ids (the search
  payload / detail payload id — a plain string, see §6.5).
- `PinResource/create` also exists and is likewise 401-gated as a guest, but
  **`RepinResource/create` is the one implemented**; `PinResource` is for
  create/mutate-of-pin, not "save to my board".
- **Success shape is UNVERIFIED live.** The only seen success is from the local
  fake server: `{"resource_response":{"status":"success"}}`. Treat success as
  `HTTP 200` `AND` `resource_response.error` absent `AND`
  `resource_response.status == "success"`; treat everything else as failure
  (including HTTP 200 with an `error` object, §4).
- Do not retry a save automatically: a 401/"Authorization failed." means "prompt
  the user to import/login", and an unknown outcome must not be blindly resent
  (duplicate saves — §15.7).

### 15.4 Non-routes — dead ends recorded so nobody repeats them

| Request | Result | Meaning |
|---|---|---|
| `BoardResource/get` | 200, **metadata only**; `pins[]` empty for guests | board header/owner/cover; not a pin source |
| `BoardFeedResource/get` | 403 `Invalid Resource Request` | not a usable guest feed |
| `BoardPinsResource/get` | 404, body `{}` (~208 B) | **unknown resource name** — do not use |
| `UserSavedPinsResource/get` | 404, body `{}` | unknown resource name — use `UserPinsResource` (§15.2) |
| `PinResource/save` | 404 `unsupported method` | RPC methods are `get` / `create` / `update` / `delete` only |
| `PinResource/unsave` | 404 `unsupported method` | same; unsave is a `delete`-style resource, unverified |
| `PinResource/create` | 401 `Authorization failed.` | exists, auth-gated; not the save path |

**Two distinct 404 flavours — distinguish them (§11 extension):**

| body | meaning |
|---|---|
| `{"resource_response":{"error":{"code":50,"message":"Pin not found.",…}}}` | resource known, **object** not found/visible |
| 404 `unsupported method` (message text) | resource known, **method** not valid (`save`, `unsave`, …) |
| 404 with `{}` (~208 B) | **resource name unknown** (e.g. `BoardPinsResource`) |

Never treat a 404 as "retry with the same request": each flavour needs a different
fix (fix id, fix method, fix resource name).

### 15.5 Client safety model (writes are gated, twice)

Saving is the only mutating action in the client and is off by default:

1. **CLI**: `tools/dump --save <pin> --board <id>` refuses unless the environment
   variable **`PC_ALLOW_WRITE=1`** is set; without it the command errors out
   without any network write. (Reads need no opt-in.)
2. **GUI**: the save affordance is enabled only when
   `pc_auth_check() == PC_AUTH_LOGGED_IN` **and** the GUI was not started with
   **`--no-writes`**. Guests see the button disabled with a "sign in via cookie
   import" hint (§14).
3. **Never auto-save**, never save on hover/scroll, never batch-save. Only on an
   explicit click of a specific pin into a specific board (a board chooser using
   §15.1, never an inferred default board).
4. On `401 "Authorization failed."`: surface "login required", flip to guest
   (§14.3), do **not** retry. On 403: re-bootstrap the CSRF/session once (§3),
   then fail. On other errors: show the message, no retry.
5. Writes are irreversible from the client's point of view (unsave/delete is not
   implemented, §15.7) — show the save result honestly; on an unknown outcome say
   "may not have saved" rather than claiming success.
6. Log only public identifiers (pin id, board id) and the HTTP status; **never**
   log cookie values or CSRF tokens (§14.2).
7. Keep write spacing conservative (≥1 s between writes, one in flight); no
   retry loops — writes are account-visible and more expensive than reads (§9
   numbers are guest-read numbers).

### 15.6 C implementation checklist (boards / saved / save)

1. `pc_user_boards(c, username, …)`: POST `BoardsResource/get`,
   `options={username, page_size:25, field_set_key:"profile_grid_item"}`,
   `source_url="/<user>/"`. Parse `resource_response.data` as a **list**; map each
   board to `{id, name, url, privacy, pin_count}`. On 400 `"username is required"`
   show "enter a username / sign in" (no retry).
2. `pc_user_pins(c, username, bookmark, …)`: POST `UserPinsResource/get`,
   `options={username, page_size:25, bookmarks:[cursor]?}`, `source_url="/<user>/"`.
   Parse `data` (list) with the §6.4 extractor; take `resource_response.bookmark`
   (empty/`-end-` ⇒ done). Own account requires §14 auth.
3. `pc_save_pin(c, pin_id, board_id, …)`: hard-gate on `PC_ALLOW_WRITE` at the
   call site (CLI/GUI), then POST `RepinResource/create`, `source_url=
   "/pin/<pin_id>/"`, `options={pin_id, board_id}`. One request, no retry. Success
   = 200 + `status=="success"` + no `error`. `401 "Authorization failed."` ⇒
   "login required". Return false + err otherwise.
4. Board chooser must come from §15.1 (`board_id` is the numeric string id, not
   the board `node_id` — see §15.7), and must not silently pick a board.
5. Response buffers: board lists and saved lists are plain arrays — handle
   `data` being `null` (no boards / hidden profile) as "empty", not as an error.

### 15.7 What is still UNVERIFIED (do not claim otherwise)

1. **Actual repin success payload** — only the fake server's
   `{"resource_response":{"status":"success"}}` was seen; the real envelope may
   carry the new pin/board ids and counters. Client must not depend on extra
   fields.
2. **`board_id` numeric vs `node_id`** — the fake server accepted the numeric
   string; whether production requires numeric or the base64 `node_id`
   (`Qm9hcmQ6…`) is untested. Send the numeric `id` from §15.1.
3. **Secret / protected boards** — `privacy` values other than `"public"`, and
   whether saving to them works or needs extra flags, unverified.
4. **Save-to-new-board flow** — the board must already exist; creating a board
   inline (`BoardResource/create`) is unimplemented and untested.
5. **Unsave / delete** — unimplemented; the delete-style resource name/options are
   unknown; `PinResource/unsave` is not a method (§15.4).
6. **Duplicate-save idempotency** — whether repinning the same pin to the same
   board twice errors, no-ops or duplicates is unknown; do not auto-retry.
7. **Write rate limits / bot scoring** — zero write samples against production;
   §9 numbers are guest reads. Assume writes are watched more closely.
8. **Session expiry during a write** — a 401 mid-save (expired jar) is
   indistinguishable from "not signed in"; only the §14.3 probe + user re-import
   resolves it.
9. **Boards pagination / own-profile read as guest** — the 5-board sample fit one
   page; bookmark behaviour for >25 boards, and the exact error for a guest
   reading their own (empty) username, are unverified.

---

## 16. Close-up GraphQL: comments, related pins, reactions

> **Status.** Captured on a **live, authenticated session** (imported cookie jar,
> headless Chrome + CDP, no visible window, 2026-10-31) and **replayed outside the
> browser with `curl`** (commands below are paste-ready; all reads verified read-only).
> **No write/mutation was ever executed** — every write below is marked
> **UNVERIFIED** and was derived from the webapp JS bundles only.

### 16.1 What the close-up page actually calls (GraphQL is mostly a red herring)

61 network captures of `/resource/…` + `/\/graphql/` on a numeric pin close-up page
(`https://nl.pinterest.com/pin/1049549888178381639/`) contained **zero requests to
`/_/graphql/`**. Every close-up read is a normal resource RPC:

| UI element | Resource (method `get`) | Response `endpoint_name` |
|---|---|---|
| pin entity | `ApiResource` → `options.url = /v3/pins/<id>/` | `v3_get_pin` |
| comments list | `UnifiedCommentsResource` | `v3_get_aggregated_pin_data_unified_comments` |
| comment replies | `AggregatedCommentReplyFeedResource` | `v3_get_aggregated_comment_replies` |
| related pins ("More to explore") | `RelatedModulesResource` | `v3_related_modules_for_pin` |

The GraphQL endpoint **is still usable** for related pins (`GatedCloseupRelatedPinsFeedQuery`,
§16.4) but this build's UI does not call it on the close-up page. **Replay caveat:**
the browser issues these resources as **GET with `?data=`**, but a plain `curl` GET
is rejected with `403 Invalid Resource Request`; **always POST the form body**
(`source_url=<enc>&data=<enc>`) — the server accepts POST for all `get` resources.
`ApiResource` is special: it is POSTed (its RPC method is `get`).

Common replay headers (all verified):

```
-b .session-cookies.txt                 # Netscape jar from the browser
-H "X-CSRFToken: <csrftoken cookie value>"
-H "X-Requested-With: XMLHttpRequest"
-H "X-Pinterest-AppState: active"
-H "X-Pinterest-Source-Url: /pin/<pin_id>/"
-H "Origin: https://nl.pinterest.com"
-H "Referer: https://nl.pinterest.com/pin/<pin_id>/"
-H "Accept: application/json"
```
plus `--compressed` (bodies are gzip) and `Content-Type: application/x-www-form-urlencoded; charset=UTF-8`
for POST bodies.

### 16.2 Pin entity — `ApiResource` → `/v3/pins/<id>/`

`options.url` alone returns a 252-key pin but **without** `aggregated_pin_data`,
`reaction_counts` and `liked_by_me`. Request them explicitly via `options.data.fields`:

```
POST /resource/ApiResource/get/?source_url=%2Fpin%2F1049549888178381639%2F
data={"options":{"url":"/v3/pins/1049549888178381639/",
                 "data":{"fields":["pin.id","pin.comment_count","pin.reaction_counts",
                                   "pin.liked_by_me","pin.repin_count",
                                   "pin.aggregated_pin_data()","pin.images",
                                   "pin.title","pin.grid_title","pin.description"]}},
      "context":{}}
→ HTTP 200, 9 862 B, endpoint_name "v3_get_pin"
```

JSON paths (`resource_response.data`):

| path | sample | meaning |
|---|---|---|
| `data.id` | `"1049549888178381639"` | canonical numeric pin id (string) |
| `data.comment_count` | `0` | **stale/unused** on modern pins — do not trust for the comments badge |
| `data.reaction_counts` | `{"1": 339}` | map `reaction_type` → count; key `"1"` is the Like reaction |
| `data.liked_by_me` | `false` | viewer-liked flag (needs fields request) |
| `data.repin_count` | `1245` | saves/repins |
| `data.aggregated_pin_data.id` | `"3507559680119543872"` | **the key the comments resource needs** (§16.3) |
| `data.aggregated_pin_data.aggregated_stats` | `{"saves":1720,"done":4}` | aggregated stats |
| `data.images` | dict of `{url,width,height}` | image ladder, same schema as search pins (§8) |

The comments badge count shown by the UI comes from the comments resource itself /
aggregated comment data, not `data.comment_count`.

### 16.3 Comments — `UnifiedCommentsResource/get`

The comments list is **not GraphQL**. It takes the *aggregated pin id* (from
§16.2 `aggregated_pin_data.id`), **not** the pin id:

```
POST /resource/UnifiedCommentsResource/get/?source_url=%2Fpin%2F1049549888178381639%2F
data={"options":{"aggregated_pin_id":"3507559680119543872",
                 "comment_featured_ids":[],"page_size":10,
                 "redux_normalize_feed":true,"is_reversed":false},
      "context":{}}
→ HTTP 200, 14 380 B, endpoint "v3_get_aggregated_pin_data_unified_comments"
   data[] = 10 comments, bookmark = 588-char cursor
```

Pagination (verified page 2/3): add `"bookmarks":["<resource_response.bookmark>"]`
to `options`; page 2 returned 10 *different* comments and a new bookmark.
Stop when `bookmark` is absent/`-end-`.

JSON paths per item in `resource_response.data[]`:

| path | meaning |
|---|---|
| `id` | comment id (numeric string) |
| `type` | `"userdiditdata"` for comments |
| `details` | comment text (HTML-ish; `text` is `null` for these) |
| `user.{id,username,full_name,image_small_url}` | author |
| `like_count` | comment likes |
| `comment_count` | number of replies (`0` for top-level here) |
| `done_at` | timestamp (`created_at` is null for this type) |
| `images`, `videos`, `tagged_users`, `tags` | attachments/tags |
| `replies` | `null` — replies are a separate resource ↓ |

Replies: `AggregatedCommentReplyFeedResource/get` with
`options={"isUnifiedComment":true,"objectId":"<comment id>","redux_normalize_feed":true}`
(same source_url) → endpoint `v3_get_aggregated_comment_replies`, `data[]` of replies,
code points: `.id`, `.text`, `.user`, `.reaction_counts`, `.comment_count`.

**Add a comment** (shape verified; creation itself deliberately never executed):
`POST /resource/AggregatedCommentResource/create/` with
`options={"force":true,"objectId":"<aggregated_pin_data.id>","pinId":"<pin id>","tags":"[]","text":"<text>","replyToCommentId":null}`
(the `tags` field is a JSON *string*; reply-to uses the parent comment id; the app's
helper is `Resource.create("AggregatedCommentResource", …).callCreate()`).

**Important:** `objectId` must be the pin's **`aggregated_pin_data.id`**, *not*
the pin id. With the pin id Pinterest answers
`HTTP 404 / code 880 "Aggregated pin data could not be found."` (this is what the
first live attempt hit). With the aggregated id, an **empty-text** probe (which
cannot create a comment) advances to validation:
`HTTP 400 / code 4906 "Text cannot be empty if no media is provided"`. The
client resolves the aggregated id through the same cached close-up entity lookup
used for `UnifiedCommentsResource`. `PC_ADD_COMMENT_DRYRUN=1` prints the exact
request payload without sending it.

Response `resource_response.data` is the new comment (client reads `.id`/`.type`).

### 16.4 Related pins — two working paths

**(a) UI path, bookmark-paged (verified):** `RelatedModulesResource/get`

```
POST /resource/RelatedModulesResource/get/?source_url=%2Fpin%2F934285885217653703%2F
data={"options":{"additional_fields":["pin.gen_ai_topics"],"pin_id":"934285885217653703",
                 "context_pin_ids":[],"context_near_dup_image_sigs":[],
                 "homefeed_source_sig":null,"page_size":12,"search_query":"",
                 "source":"deep_linking","top_level_source":"deep_linking",
                 "top_level_source_depth":1,"is_pdp":false},
      "context":{}}
→ HTTP 200, endpoint "v3_related_modules_for_pin"
   data[] = 12 modules (1 story + 11 pins), bookmark = 1020-char cursor
```
Page 2: add `"bookmarks":["<cursor>"]` to `options` (13 modules, new cursor).
Each module is either a **pin object** (`type:"pin"`, same schema as §6.1, use the
§6.4 extractor) or a **story wrapper** (`type:"story"`, `objects[]` may hold pins —
recurse; some story modules legitimately have `objects: []`). This is the path that
supports "load more related".

**(b) GraphQL query (verified read-only, no pagination):**

```
POST https://nl.pinterest.com/_/graphql/
Content-Type: application/json
X-CSRFToken: <csrftoken>
X-Requested-With: XMLHttpRequest
X-Pinterest-AppState: active
X-Pinterest-GraphQL-Name: GatedCloseupRelatedPinsFeedQuery
X-Pinterest-Source-Url: /pin/<pin_id>/
Origin/Referer: https://nl.pinterest.com…

{"queryHash":"22d4b5f01f07c1be71875fc3555a98a630cb701ea7090b818d4dc456325515e6",
 "variables":{"pinId":"934285885217653703","count":12}}
→ HTTP 200, 9 836 B
```

JSON path: `data.v3RelatedPinsForPinSeoQuery.data.connection.edges[].node` with
`node.__typename:"Pin"`, `node.entityId` (numeric pin id), `node.imageSignature`,
`node.images_236x / images_474x / images_736x / images_orig` (`{url,width,height,dominantColor}`),
`node.videos`, `node.trackingParams`. There is **no** `pageInfo`/cursor field in the
response and the operation has only `pinId` + `count` variables → **one page only**.

Also in the bundle: `CloseupStoryPinQuery`
hash `fe63d9041933722e5ee1b2315a5e72e739f6d049c9a0b8597b262a9882b644b3` (story pins;
variables not exercised — **UNVERIFIED**).

### 16.5 Write mutations — hashes/specs, NONE EXECUTED (all UNVERIFIED)

`ApiResource` maps HTTP verbs to RPC methods (from the bundle):
`POST → create`, `PUT/PATCH → update`, `DELETE → delete`, else `get`. So writes are
**REST resources too** — GraphQL is only an alternate for comment likes. Nothing
below was performed against the account; treat every line as unverified.

| Action | Request | Notes |
|---|---|---|
| **Like pin** | `POST /resource/ApiResource/create/?source_url=%2Fpin%2F<pin>%2F` with `options={"url":"/v3/pins/<pin>/react/","data":{"reaction_type":1}}` | `reaction_type:1` = Like (`reaction_counts` key `"1"`); add `client_tracking_params` optionally |
| **Unlike pin** | `POST /resource/ApiResource/delete/?…` with `options={"url":"/v3/pins/<pin>/react/","data":{"client_tracking_params":…}}` | bundle calls `method:"DELETE"` on `/v3/pins/{pin}/react/` |
| **Like/unlike comment** | `ApiResource/create` or `/delete` (same URL) with `options={"url":"/v3/aggregated_comments/<comment_id>/react/","data":{"reaction_type":1}}` | REST path used by current bundle |
| Add comment | see §16.3 (AggregatedCommentResource/create) | — |
| Highlight comment (pin owner) | `PUT /v3/aggregated_comments/pin/highlight/` body `{aggregated_comment, pin, highlight}` | via `ApiResource/update`; bundle-only |

GraphQL alternates found in the persisted-query map (hashes **verbatim**, variables
**unverified** — do not use without live testing):

```
CommentThreadContainerLikeMutation    0e45555e33d7c8c17de577cb4e5213ff3568b1b3f3e6694041407c0dad67674d
CommentThreadContainerUnLikeMutation  8f06f078d5b50aa7a3b5e86afda41af9f9ff89d14d2620f3f830abe11ce26f6e
CloseupStoryPinQuery                  fe63d9041933722e5ee1b2315a5e72e739f6d049c9a0b8597b262a9882b644b3
GatedCloseupRelatedPinsFeedQuery      22d4b5f01f07c1be71875fc3555a98a630cb701ea7090b818d4dc456325515e6
```
How these were obtained without executing anything: downloaded the close-up page's
94 JS chunks from `s.pinimg.com`, then grepped the Relay persisted-query map for
`params:{id:"<64-hex>",…,name:"<Op>",operationKind:"mutation"}`. Re-run the same
grep after a hash rotation (see §16.6).

### 16.6 Failure modes and hash rotation

| Symptom | Meaning | Action |
|---|---|---|
| `403` body `Invalid Resource Request` | resource RPC sent as GET (or missing CSRF) | POST form body; send `X-CSRFToken` |
| `403` body `Bad CSRF token` / `No CSRF cookie` | stale/host-scoped csrftoken (§2/§3) | re-bootstrap host `nl.`, retry once |
| `401` `Authentication failed.` | comments/related/reactions need the authenticated jar | import cookies (§14); no retry |
| `404` body `{}` (~208 B) | unknown resource name | check spelling; see §15.4 |
| `404` code 50 `Pin not found.` | `e1_…` id or deleted pin | use numeric ids from feed (§7.2) |
| GraphQL `200` `{"data":null,"errors":[{"message":"CLIENT GRAPHQL ERROR",…}]}` | bad/rotated `queryHash`, or missing/extra variables | re-derive hash from bundles |
| GraphQL `200` `data.<root>.__typename:"…NotFound"` | query accepted, object not found | id/visibility problem, not a hash problem |
| `429` | rate limit | backoff 2/4/8 s (not observed in this session; §9) |

**Detecting hash rotation:** any GraphQL read that starts returning
`CLIENT GRAPHQL ERROR` (HTTP 200) means the persisted query changed. Recovery: fetch
`https://nl.pinterest.com/pin/<numeric-id>/` HTML, download its `.mjs` chunks from
`s.pinimg.com`, grep for the operation name next to a 64-hex `params:{id:"…"}` and
replace the hash. Bundle URLs are listed in `document.scripts` (94 chunks ≈ 14 MB).

### 16.7 C implementation checklist (close-up parity)

1. Open close-up for a **numeric** pin id (§7.2: `e1_…` ids still 404).
2. One `ApiResource/get` to `/v3/pins/<id>/` **with the `fields` list** from §16.2 →
   gives `liked_by_me`, `reaction_counts`, `aggregated_pin_data.id`, images, title.
3. Comments: `UnifiedCommentsResource/get` with
   `aggregated_pin_id = aggregated_pin_data.id`, `page_size:10`; page with
   `bookmarks:[cursor]`; render `details`/`user`/`like_count`/`done_at`. Fetch
   replies only on expand (`AggregatedCommentReplyFeedResource/get`, `objectId` = comment id).
4. Related: `RelatedModulesResource/get` (bookmark-paged) — flatten with the §6.4
   extractor; skip empty story modules. GraphQL `GatedCloseupRelatedPinsFeedQuery` is
   an optional single-page fallback (no cursor).
5. Reactions: render `reaction_counts["1"]` (Like) and `liked_by_me`; toggling =
   §16.5 (gate writes behind `PC_ALLOW_WRITE`, no auto-retry, mark unverified path).
6. Politeness: one in-flight request, ≥0.8 s spacing, cache pin+comments per open
   close-up, never parallelise comment paging with related paging.
7. Never send writes until §16.5 is verified live — the project's rule is that
   unverified writes stay behind the explicit write gate.
