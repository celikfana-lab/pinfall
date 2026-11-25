/* jsonx.h - PinFall JSON extraction .
 *
 * Turns Pinterest resource-RPC payloads into PcPin / PcPinList.  Handles both
 * result shapes: raw pin objects and `type:"story"` wrappers whose `objects`
 * list mixes pin / board / user objects.  Not part of the frozen public API.
 */
#ifndef PINFALL_JSONX_H
#define PINFALL_JSONX_H

#include <stdbool.h>
#include <stddef.h>

#include "pc.h"

/* Parse a BaseSearchResource response into out (items + bookmark).
 * On failure returns false and, when err != NULL, sets *err to a malloc'd
 * string (free with pc_free()). */
bool pc_json_parse_search(const char *json, size_t len, PcPinList *out,
                          char **err);

/* Parse a PinResource response (best effort) into out.  If want_id is non-NULL
 * and the payload is a result list, the pin with that id is selected. */
bool pc_json_parse_pin(const char *json, size_t len, const char *want_id,
                       PcPin *out, char **err);

/* Saved-pins / boards payloads (v3).  Both accept either a bare LIST in
 * resource_response.data (what these endpoints return) or a defensive
 * data.results wrapper.  Bookmarks come from resource_response.bookmark. */
bool pc_json_parse_pins(const char *json, size_t len, PcPinList *out,
                        char **err);
bool pc_json_parse_boards(const char *json, size_t len, PcBoardList *out,
                          char **err);

/* --- internal ad-block hooks (v5) ---------------------------------------
 * The public filter flag lives on the client; api.c wraps each feed parse in
 * ads_begin/ads_end so the extractor can skip promoted pins and report how
 * many it skipped.  Per-call state is thread-local. */
void     pc_json_ads_begin(bool block);
uint64_t pc_json_ads_end(void);

/* Count promoted pins in a payload (search/home/saved shape: bare data[] or
 * data.results[]) without filtering; used by the dump --ads self-check. */
uint64_t pc_json_count_ads(const char *json, size_t len, char **err);

/* GraphQL / arbitrary-JSON extractors (v6): walk any JSON document and collect
 * pin objects (related pins) or comment objects (comments list).  Also picks up
 * the first pagination cursor found (bookmark/cursor).  The client ad filter is
 * applied to collected pins like on every other feed. */
bool pc_json_collect_pins_any(const char *json, size_t len, PcPinList *out,
                              char **err);
bool pc_json_collect_comments_any(const char *json, size_t len,
                                  PcCommentList *out, char **err);

/* Parse a payload that holds a pin with videos (GraphQL close-up data or a
 * resource pin object) into a PcPin with id/title/video_* fields filled. */
bool pc_json_parse_video_pin(const char *json, size_t len, PcPin *out,
                             char **err);

/* Best effort: pull the signed-in username out of a bootstrap HTML page's
 * <script id="__PWS_INITIAL_PROPS__"> JSON.  Writes "" when not present (guest
 * shell).  Never performs a network request. */
void pc_json_extract_username(const char *html, size_t len,
                              char *out, size_t outsz);

#endif /* PINFALL_JSONX_H */
