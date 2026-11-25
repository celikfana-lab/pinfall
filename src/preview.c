/* preview.c - synthetic boards for the --auth-preview rendering mode. */
#include "preview.h"

#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREVIEW_BOARDS 3

static const struct {
    const char *id;
    const char *name;
    const char *privacy;
    int pin_count;
} PREVIEW_BOARD_DATA[PREVIEW_BOARDS] = {
    {"1001", "Inspiration", "public", 128},
    {"1002", "Recipes", "public", 64},
    {"1003", "Places", "secret", 23},
};

bool preview_fill_boards(PcBoardList *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof *out);
    PcBoard *items = (PcBoard *)calloc(PREVIEW_BOARDS, sizeof *items);
    if (!items)
        return false;
    for (int i = 0; i < PREVIEW_BOARDS; i++) {
        PcBoard *b = &items[i];
        snprintf(b->id, sizeof b->id, "%s", PREVIEW_BOARD_DATA[i].id);
        snprintf(b->name, sizeof b->name, "%s", PREVIEW_BOARD_DATA[i].name);
        snprintf(b->privacy, sizeof b->privacy, "%s",
                 PREVIEW_BOARD_DATA[i].privacy);
        b->pin_count = PREVIEW_BOARD_DATA[i].pin_count;
    }
    out->items = items;
    out->count = PREVIEW_BOARDS;
    out->cap = PREVIEW_BOARDS;
    return true;
}

bool preview_save_pin(const char *pin_id, const char *board_id,
                      char *board_name, size_t name_sz)
{
    (void)pin_id;
    if (board_name && name_sz)
        board_name[0] = '\0';
    if (!board_id)
        return false;
    for (int i = 0; i < PREVIEW_BOARDS; i++) {
        if (strcmp(PREVIEW_BOARD_DATA[i].id, board_id) == 0) {
            if (board_name && name_sz)
                snprintf(board_name, name_sz, "%s",
                         PREVIEW_BOARD_DATA[i].name);
            return true;
        }
    }
    return false;
}

bool preview_fill_comments(PcCommentList *out, const char *pin_id)
{
    (void)pin_id;
    if (!out)
        return false;
    memset(out, 0, sizeof *out);
    static const char *NAMES[4] = {"maya", "lin", "jules", "sam"};
    static const char *TEXTS[4] = {
        "This is exactly the palette I want for our bedroom \xF0\x9F\x98\x8D so "
        "calm and warm.",
        "Saving this \xE2\x9D\xA4\xEF\xB8\x8F for the flat renovation. Any idea where "
        "the linen throw is from?",
        "The light in this room is unreal \xF0\x9F\x98\x82. Adding a paper lantern ASAP.",
        "Minimal but still cosy, bookmarking this one.",
    };
    static const char *WHEN[4] = {"2d", "5d", "1w", "2w"};
    static const int LIKES[4] = {14, 6, 3, 0};
    PcComment *items = (PcComment *)calloc(4, sizeof *items);
    if (!items)
        return false;
    for (int i = 0; i < 4; i++) {
        PcComment *c = &items[i];
        snprintf(c->id, sizeof c->id, "pc%d", 100 + i);
        snprintf(c->user_name, sizeof c->user_name, "%s", NAMES[i]);
        snprintf(c->text, sizeof c->text, "%s", TEXTS[i]);
        snprintf(c->created_at, sizeof c->created_at, "%s", WHEN[i]);
        c->like_count = LIKES[i];
    }
    out->items = items;
    out->count = 4;
    out->cap = 4;
    return true;
}
