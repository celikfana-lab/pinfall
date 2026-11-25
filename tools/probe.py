#!/usr/bin/env python3
"""probe.py — reverse-engineering / sanity-check CLI for Pinterest's guest API.

Pure Python 3 standard library (no pip installs).  See docs/API.md for the spec.

Usage
-----
  python3 tools/probe.py session [--refresh]
  python3 tools/probe.py search "minimalist bedroom" [--pages N] [--page-size N]
                                     [--pos N] [--raw]
  python3 tools/probe.py detail <pin_id> [--raw]
  python3 tools/probe.py feed [--raw]          # proves home feed needs auth (401)
  python3 tools/probe.py fixture search "q" --out fixtures/search_p1.json

The guest session (cookies + csrftoken) is bootstrapped once from
``GET /`` and cached in /tmp/pinfall-cache/session.json (override with
$PC_PROBE_CACHE).  Every resource RPC is POST + form-encoded `data`.

Exit codes: 0 ok, 1 network/HTTP error, 2 usage error.
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import http.cookiejar

HOST_DEFAULT = "https://nl.pinterest.com"          # www. returns 403 from many IPs
UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36")
CACHE_DIR = os.environ.get("PC_PROBE_CACHE", "/tmp/pinfall-cache")
SESSION_FILE = os.path.join(CACHE_DIR, "session.json")
SESSION_MAX_AGE = 20 * 60          # guest session is fine for hours; refresh hourly-ish
POLITE_DELAY = float(os.environ.get("PC_PROBE_DELAY", "0.8"))

PIN_SIZES = ["236x", "474x", "736x", "170x", "orig"]


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------
def eprint(*a):
    print(*a, file=sys.stderr)


def gunzip(body):
    """Pinterest gzips *error* bodies (e.g. 403 'Bad CSRF token') even when the
    request says `Accept-Encoding: identity`; always sniff the magic bytes."""
    if body[:2] == b"\x1f\x8b":
        import gzip
        try:
            return gzip.decompress(body)
        except OSError:
            return body
    return body


def host():
    return (os.environ.get("PC_HOST") or HOST_DEFAULT).rstrip("/")


class Session:
    def __init__(self, cookies=None, csrf=None):
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar))
        self.csrf = csrf
        self.created = 0
        if cookies:
            for name, meta in cookies.items():
                c = http.cookiejar.Cookie(
                    0, name, meta["value"], None, False, meta["domain"], False,
                    False, meta["path"], meta["secure"], False, None, False, None,
                    None, {})
                self.jar.set_cookie(c)
        if csrf is None:
            self.csrf = self._cookie("csrftoken")

    # -- cookies -----------------------------------------------------------
    def _cookie(self, name):
        for c in self.jar:
            if c.name == name:
                return c.value
        return None

    def cookie_rows(self):
        now = time.time()
        rows = []
        for c in self.jar:
            rows.append({
                "name": c.name, "value_len": len(c.value),
                "domain": c.domain, "path": c.path, "secure": bool(c.secure),
                "expires_in_s": (int(c.expires - now) if c.expires else None),
            })
        return rows

    def to_dict(self):
        return {
            "host": host(), "created": self.created, "csrf": self.csrf,
            "cookies": {c.name: {"value": c.value, "domain": c.domain,
                                 "path": c.path, "secure": bool(c.secure)}
                        for c in self.jar},
        }

    # -- persistence -------------------------------------------------------
    def save(self):
        try:
            os.makedirs(CACHE_DIR, exist_ok=True)
            tmp = SESSION_FILE + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self.to_dict(), f)
            os.replace(tmp, SESSION_FILE)
        except OSError as exc:                     # disk full etc. — non-fatal
            eprint("probe: could not cache session:", exc)

    @classmethod
    def load_cached(cls):
        try:
            with open(SESSION_FILE) as f:
                d = json.load(f)
        except (OSError, ValueError):
            return None
        if d.get("host") != host():
            return None
        if time.time() - d.get("created", 0) > SESSION_MAX_AGE:
            return None
        if not d.get("cookies"):
            return None
        s = cls(cookies=d["cookies"], csrf=d.get("csrf"))
        s.created = d.get("created", 0)
        return s

    # -- network -----------------------------------------------------------
    def get(self, url, extra_headers=None, body=None):
        headers = {
            "User-Agent": UA,
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            "Accept-Language": "en-US,en;q=0.9,nl;q=0.8",
            "Accept-Encoding": "identity",
        }
        if extra_headers:
            headers.update(extra_headers)
        req = urllib.request.Request(url, data=body, headers=headers,
                                     method="POST" if body is not None else "GET")
        try:
            r = self.opener.open(req, timeout=30)
            return r.getcode(), gunzip(r.read())
        except urllib.error.HTTPError as e:
            return e.code, gunzip(e.read())

    def post_form(self, url, fields, extra_headers):
        body = urllib.parse.urlencode(fields).encode()
        return self.get(url, extra_headers=extra_headers, body=body)


def bootstrap(session=None, force=False):
    """GET / -> guest cookies (`_pinterest_sess`, `csrftoken`, `_auth=0`, ...)."""
    if session and not force:
        return session
    if not force:
        cached = Session.load_cached()
        if cached:
            return cached
    s = Session()
    status, body = s.get(host() + "/")
    if status != 200:
        raise SystemExit(f"probe: bootstrap GET / failed with HTTP {status}")
    csrf = s._cookie("csrftoken")
    if not csrf:                       # fall back to HTML/JSON on the page
        m = re.search(rb'\\?"csrftoken\\?"\s*:\s*\\?"([0-9a-f]{16,})\\?"', body)
        if m:
            csrf = m.group(1).decode()
            s.csrf = csrf
    if not csrf:
        raise SystemExit("probe: no csrftoken cookie after bootstrap")
    s.csrf = csrf
    s.created = int(time.time())
    s.save()
    return s


def resource_url(name, source_url):
    return (f"{host()}/resource/{name}/get/"
            f"?source_url={urllib.parse.quote(source_url, safe='')}")


def post_resource(session, name, options, source_url, extra_headers=None):
    """The whole API: POST /resource/<Name>/get/ with form-encoded options JSON."""
    payload = json.dumps({"options": options, "context": {}},
                         separators=(",", ":"))
    headers = {
        "User-Agent": UA,
        "Accept": "application/json, text/javascript, */*, q=0.01",
        "Accept-Language": "en-US,en;q=0.9,nl;q=0.8",
        "Accept-Encoding": "identity",
        "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
        "X-Requested-With": "XMLHttpRequest",
        "X-CSRFToken": session.csrf,
        "X-Pinterest-AppState": "active",
        "X-Pinterest-Source-Url": source_url,
        "Origin": host(),
        "Referer": host() + source_url,
    }
    if extra_headers:
        headers.update(extra_headers)
    status, body = session.post_form(
        resource_url(name, source_url),
        {"data": payload, "source_url": source_url}, headers)
    return status, body


def parse_rpc(status, body):
    """Return (resource_response dict, error dict|None). Raises on non-JSON."""
    try:
        doc = json.loads(body)
    except ValueError:
        hint = ""
        if body[:15].lstrip().lower().startswith(b"<!doctype"):
            hint = ("\n  (got the HTML app shell: the session cookie jar was not sent "
                    "or was rejected — the RPC needs the full guest cookie jar)")
        raise SystemExit(f"probe: HTTP {status}, non-JSON body: {body[:200]!r}{hint}")
    rr = doc.get("resource_response") or {}
    err = rr.get("error")
    if status >= 400 and not err:
        err = {"http_status": status, "message": doc.get("message", "HTTP error")}
    return rr, err


def fmt_error(rr, err):
    if not err:
        return "unknown error"
    code = err.get("code", err.get("api_error_code"))
    return f"HTTP {err.get('http_status')} {code} {err.get('message')}"


# --------------------------------------------------------------------------
# extraction — the dual-shape trap, robust version
# --------------------------------------------------------------------------
def _is_pin(node):
    """Pin predicate that also rejects BOARD objects (they have id+images too).

    pin.images is {size: {url,width,height}}; board.images is {size: [ ... ]}.
    Boards never have dominant_color/pinner/board, pins (almost) always do.
    """
    if not isinstance(node, dict):
        return False
    t = node.get("type")
    if t == "pin":
        return True
    if t in ("board", "user", "story"):
        return False
    if "objects" in node or "content_ids" in node:      # story wrapper
        return False
    imgs = node.get("images")
    if not isinstance(imgs, dict) or not node.get("id"):
        return False
    dict_sizes = sum(1 for v in imgs.values() if isinstance(v, dict))
    if dict_sizes == 0:
        return False
    return ("dominant_color" in node or "orig" in node or "pinner" in node
            or "board" in node or "image_signature" in node)


def iter_pins(node, depth=0):
    """Walk results[] and yield pin dicts, recursing through story wrappers."""
    if depth > 4:
        return
    if isinstance(node, list):
        for item in node:
            yield from iter_pins(item, depth + 1)
        return
    if not isinstance(node, dict):
        return
    if node.get("type") == "story" or (
            isinstance(node.get("objects"), list) and not _is_pin(node)):
        yield from iter_pins(node["objects"], depth + 1)
        return
    if _is_pin(node):
        yield node


def image_info(pin, size=None):
    """Return (url, width, height) using the 236x->474x->736x->170x->orig ladder."""
    imgs = pin.get("images") or {}
    for key in ([size] if size else []) + PIN_SIZES:
        v = imgs.get(key)
        if isinstance(v, list):            # multi-image pins: first candidate
            v = next((x for x in v if isinstance(x, dict)), None)
        if isinstance(v, dict) and v.get("url"):
            return v["url"], v.get("width"), v.get("height")
    return None, None, None


def pin_row(pin):
    url, w, h = image_info(pin)
    board = (pin.get("board") or {}).get("name") or ""
    title = (pin.get("title") or pin.get("grid_title") or
             pin.get("description") or "").strip().replace("\n", " ")
    return {
        "id": pin.get("id", ""),
        "width": w, "height": h,
        "size": f"{w}x{h}" if w and h else "?x?",
        "board": board,
        "title": title[:70],
        "thumb_url": url,
        "dominant_color": pin.get("dominant_color"),
    }


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------
def cmd_session(args):
    s = bootstrap(force=args.refresh)
    print(f"host       : {host()}")
    print(f"csrf cookie: {'yes' if s.csrf else 'NO'}  ({len(s.csrf or '')} chars)")
    print(f"cached at  : {SESSION_FILE}")
    print(f"{'cookie':<18}{'dom':<18}{'path':<6}{'sec':<5}expires_in")
    for row in s.cookie_rows():
        exp = row["expires_in_s"]
        print(f"{row['name']:<18}{row['domain']:<18}{row['path']:<6}"
              f"{str(row['secure']):<5}{exp if exp is None else f'{exp}s'}")
    # sanity probe: a tiny search proves the session is usable
    st, body = post_resource(s, "BaseSearchResource",
                             {"query": "cats", "scope": "pins", "page_size": 3,
                              "bookmarks": []}, "/search/pins/?q=cats")
    rr, err = parse_rpc(st, body)
    n = len((rr.get("data") or {}).get("results") or [])
    print(f"sanity     : BaseSearchResource -> HTTP {st}, {n} results, "
          f"bookmark={'yes' if rr.get('bookmark') else 'no'}"
          + (f", error={fmt_error(rr, err)}" if err else ""))
    return 0 if not err else 1


def cmd_search(args):
    s = bootstrap()
    quote = args.query
    source_url = "/search/pins/?q=" + urllib.parse.quote(quote)
    bookmark = None
    seen = set()
    total = 0
    for page in range(1, args.pages + 1):
        options = {"query": quote, "scope": "pins",
                   "page_size": args.page_size,
                   "bookmarks": [bookmark] if bookmark else []}
        st, body = post_resource(s, "BaseSearchResource", options, source_url)
        rr, err = parse_rpc(st, body)
        if err:
            eprint(f"probe: page {page}: {fmt_error(rr, err)}")
            return 1
        results = (rr.get("data") or {}).get("results") or []
        pins = [p for p in iter_pins(results)]
        print(f"# page {page}: {len(results)} results -> {len(pins)} pins "
              f"(bookmark={'yes' if rr.get('bookmark') else 'NO'})")
        for pin in pins:
            row = pin_row(pin)
            if row["id"] in seen:
                continue
            seen.add(row["id"])
            total += 1
            if args.pos:
                print(f'[{total:02d}] | {row["size"]:<9} | {row["board"][:22]:<22} | '
                      f'{row["title"]:<40} | {row["thumb_url"]}')
            else:
                print(f'{row["id"]} | {row["size"]} | {row["board"][:24]} | '
                      f'{row["title"]} | {row["thumb_url"]}')
        bookmark = rr.get("bookmark")
        if args.raw:
            print(json.dumps(rr.get("data"), indent=1)[:4000])
        if not bookmark or bookmark == "-end-":
            break
        if page < args.pages:
            time.sleep(POLITE_DELAY)
    print(f"# {total} unique pins from {args.pages} page(s)")
    return 0


def cmd_detail(args):
    s = bootstrap()
    pid = args.pin_id.strip()
    m = re.search(r"/pin/([^/?#]+)", pid)
    if m:
        pid = m.group(1)
    source_url = f"/pin/{pid}/"
    st, body = post_resource(s, "PinResource",
                             {"id": pid, "field_set_key": "detailed"}, source_url)
    rr, err = parse_rpc(st, body)
    if err:
        eprint(f"probe: PinResource/get id={pid} -> {fmt_error(rr, err)}")
        if pid.startswith("e1_"):
            eprint(
                "\nprobe: this is a 2026 'ngapi' opaque pin id (BaseSearchResource ids).\n"
                "  Guest access to its closeup is NOT available: PinResource/get and\n"
                "  POST /_/graphql/ (CloseupPageQuery) both answer PinNotFound, and the\n"
                "  real web page /pin/<id>/ renders 'Pin not found' too.\n"
                "  The search payload already contains title/description/images/board/\n"
                "  pinner/dominant_color — render detail from that (see docs/API.md).\n"
                "  Legacy *numeric* ids DO work: probe.py detail 216454325827297537\n")
        return 1
    data = rr.get("data") or {}
    if args.raw:
        print(json.dumps(data, indent=1))
        return 0
    url, w, h = image_info(data)
    print(f"id          : {data.get('id')}")
    print(f"title       : {data.get('title') or data.get('grid_title')}")
    print(f"description : {(data.get('description') or '')[:160]!r}")
    print(f"board       : {(data.get('board') or {}).get('name')} "
          f"({(data.get('board') or {}).get('url')})")
    print(f"pinner      : {(data.get('pinner') or {}).get('username')}")
    print(f"link/domain : {data.get('link')} / {data.get('domain')}")
    print(f"dominant    : {data.get('dominant_color')}")
    print(f"size        : {w}x{h}")
    print(f"image sizes : {sorted((data.get('images') or {}).keys())}")
    print(f"thumb_url   : {url}")
    print(f"orig_url    : {image_info(data, 'orig')[0]}")
    print(f"created_at  : {data.get('created_at')}")
    return 0


def cmd_feed(args):
    s = bootstrap()
    st, body = post_resource(s, "UserHomefeedResource",
                             {"page_size": 10, "bookmarks": []}, "/")
    try:
        rr, err = parse_rpc(st, body)
    except SystemExit:
        print(f"UserHomefeedResource -> HTTP {st} (non-JSON, {len(body)} bytes) "
              f"=> login required")
        return 1
    if err:
        print(f"UserHomefeedResource -> HTTP {st}: {fmt_error(rr, err)} "
              f"=> login required")
        if args.raw:
            print(json.dumps(err, indent=1))
        return 1
    results = (rr.get("data") or {}).get("results") or []
    print(f"UserHomefeedResource -> HTTP {st}, {len(results)} results "
          f"(unexpected: guest feed works)")
    for pin in iter_pins(results):
        row = pin_row(pin)
        print(f'{row["id"]} | {row["size"]} | {row["board"]} | {row["title"]}')
    return 0


def cmd_fixture(args):
    """Save a trimmed, parser-testable JSON slice of a real response."""
    s = bootstrap()
    keep = set("id type title grid_title description dominant_color link domain "
               "created_at images image_signature orig alt_text board pinner "
               "objects story_type node_id".split())
    if args.what == "search":
        q = args.query
        source_url = "/search/pins/?q=" + urllib.parse.quote(q)
        bookmark = None
        for page in range(1, args.page + 1):
            st, body = post_resource(s, "BaseSearchResource",
                                     {"query": q, "scope": "pins",
                                      "page_size": args.page_size,
                                      "bookmarks": [bookmark] if bookmark else []},
                                     source_url)
            rr, err = parse_rpc(st, body)
            if err:
                raise SystemExit(f"probe: {fmt_error(rr, err)}")
            if page < args.page:
                bookmark = rr.get("bookmark")
                if not bookmark or bookmark == "-end-":
                    raise SystemExit("probe: no further pages")
                time.sleep(POLITE_DELAY)
        results = (rr.get("data") or {}).get("results") or []
    else:
        st, body = post_resource(s, "PinResource",
                                 {"id": args.pin_id, "field_set_key": "detailed"},
                                 f"/pin/{args.pin_id}/")
        rr, err = parse_rpc(st, body)
        if err:
            raise SystemExit(f"probe: {fmt_error(rr, err)}")
        results = [rr.get("data") or {}]

    def trim(node, depth=0):
        if depth > 3:
            return None
        if isinstance(node, list):
            return [trim(x, depth + 1) for x in node]
        if not isinstance(node, dict):
            return node
        if node.get("type") == "board" or "pin_count" in node:
            # keep the *shape* of board.images (236x is a LIST) — parser trap bait
            out = {k: node[k] for k in ("id", "name", "url", "type", "pin_count")
                   if k in node}
            if isinstance(node.get("images"), dict):
                out["images"] = {size: ([{"url": (val[0] or {}).get("url")}]
                                        if isinstance(val, list) and val else val)
                                 for size, val in node["images"].items()}
            return out
        if node.get("type") == "user" or "username" in node:
            return {k: node[k] for k in ("id", "username", "full_name", "type")
                    if k in node}
        out = {}
        for k, v in node.items():
            if k not in keep:
                continue
            if k == "images" and isinstance(v, dict):
                out[k] = {}
                for size, val in v.items():
                    if isinstance(val, list):
                        val = [x for x in val if isinstance(x, dict)][:1]
                    if isinstance(val, list):
                        out[k][size] = [{"url": x.get("url"), "width": x.get("width"),
                                         "height": x.get("height"),
                                         "dominant_color": x.get("dominant_color")}
                                        for x in val]
                    elif isinstance(val, dict):
                        out[k][size] = {"url": val.get("url"),
                                        "width": val.get("width"),
                                        "height": val.get("height"),
                                        "dominant_color": val.get("dominant_color")}
            elif k in ("board", "pinner"):
                out[k] = trim(v, depth + 1)
            elif isinstance(v, (dict, list)):
                out[k] = trim(v, depth + 1)
            else:
                out[k] = v
        return out

    doc = {"_comment": "trimmed real Pinterest guest response, parser fixture",
           "captured": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "endpoint": args.what, "query": args.query if args.what == "search" else None,
           "page": args.page if args.what == "search" else None,
           "resource_response": {"bookmark": rr.get("bookmark") if args.what == "search" else None,
                                 "data": {"results": [trim(r) for r in results]}}}
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes, "
          f"{len(results)} results)")
    return 0


# --------------------------------------------------------------------------
def build_parser():
    p = argparse.ArgumentParser(description="Pinterest guest-API probe")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("session", help="bootstrap and show cookies/csrf")
    sp.add_argument("--refresh", action="store_true")
    sp.set_defaults(func=cmd_session)

    sp = sub.add_parser("search", help="BaseSearchResource/get")
    sp.add_argument("query")
    sp.add_argument("--pages", type=int, default=1)
    sp.add_argument("--page-size", type=int, default=25)
    sp.add_argument("--pos", action="store_true", help="numbered rows")
    sp.add_argument("--raw", action="store_true")
    sp.set_defaults(func=cmd_search)

    sp = sub.add_parser("detail", help="PinResource/get by pin id")
    sp.add_argument("pin_id")
    sp.add_argument("--raw", action="store_true")
    sp.set_defaults(func=cmd_detail)

    sp = sub.add_parser("feed", help="UserHomefeedResource (expects 401)")
    sp.add_argument("--raw", action="store_true")
    sp.set_defaults(func=cmd_feed)

    sp = sub.add_parser("fixture", help="write a trimmed parser fixture")
    sp.add_argument("what", choices=["search", "pin"])
    sp.add_argument("query", nargs="?", default="minimalist bedroom")
    sp.add_argument("--pin-id", default="216454325827297537")
    sp.add_argument("--page-size", type=int, default=12)
    sp.add_argument("--page", type=int, default=1)
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_fixture)
    return p


def main(argv):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
