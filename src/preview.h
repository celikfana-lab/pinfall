/* preview.h - internal, test-only fake data for --auth-preview.
 *
 * --auth-preview exists so the signed-in UI can be verified offscreen without
 * the user's real Pinterest cookies. Nothing in this file performs network
 * I/O, writes files, or touches real session state: it only fabricates the
 * data the Saved tab / board picker need for rendering.
 *
 * The fake layer is deliberately confined to this pair of files and is only
 * reachable when the App has auth_preview == true.
 */
#ifndef PINFALL_PREVIEW_H
#define PINFALL_PREVIEW_H

#include "pc.h"

#include <stdbool.h>
#include <stddef.h>

/* Fill `out` with 3 synthetic boards ("Inspiration", "Recipes", "Places").
 * On success out->items is malloc'd; free with pc_boardlist_free() (or
 * preview_boards_free()). Never called for real sessions. */
bool preview_fill_boards(PcBoardList *out);

/* Fake save: returns true and copies the board display name into board_name.
 * Only called when App.auth_preview is set (no write ever reaches Pinterest). */
bool preview_save_pin(const char *pin_id, const char *board_id, char *board_name,
                      size_t name_sz);

/* Synthetic close-up comments (4) for --auth-preview; no network. */
bool preview_fill_comments(PcCommentList *out, const char *pin_id);

#endif /* PINFALL_PREVIEW_H */
