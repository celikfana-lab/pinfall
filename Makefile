# PinFall build - C11 + SDL2 + SDL2_ttf + libcurl, vendored stb_image/cJSON.
# The GUI target globs src/*.c; tools/dump uses an explicit source list so it
# never depends on UI files.

CC      ?= cc
BUILD   ?= build

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null) -lSDL2_ttf

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter \
          -Iinclude -Ivendor -Ivendor/lz4 -Isrc $(SDL_CFLAGS)

NET_LIBS := -lcurl -ljpeg -lpthread -lm
GUI_LIBS := $(SDL_LIBS) $(NET_LIBS)
DUMP_LIBS := $(NET_LIBS)

NET_SRC    := src/http.c src/api.c src/img.c src/jsonx.c src/graphql.c src/media.c
VENDOR_SRC := vendor/cJSON.c
UI_SRC     := $(filter-out $(NET_SRC),$(wildcard src/*.c))
GUI_SRC    := $(wildcard src/*.c) $(VENDOR_SRC)
DUMP_SRC   := $(NET_SRC) $(VENDOR_SRC) tools/dump.c

LZ4_OBJ := $(BUILD)/vendor/lz4/lz4.o

NET_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(NET_SRC)) $(BUILD)/vendor/cJSON.o $(LZ4_OBJ)
GUI_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(wildcard src/*.c)) $(BUILD)/vendor/cJSON.o $(LZ4_OBJ)
DUMP_OBJ := $(NET_OBJ) $(BUILD)/tools/dump.o
MEMBENCH_OBJ := $(NET_OBJ) $(BUILD)/tools/membench.o

.PHONY: all clean dump run-dump membench
.DEFAULT_GOAL := all

all: tools/dump tools/membench pinfall

pinfall: $(GUI_OBJ)
	$(CC) $(CFLAGS) -o $@ $(GUI_OBJ) $(GUI_LIBS)

tools/dump: $(DUMP_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(DUMP_OBJ) $(DUMP_LIBS)

tools/membench: $(MEMBENCH_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(MEMBENCH_OBJ) $(DUMP_LIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# Third-party code: fetched verbatim, compiled quietly (still our CFLAGS).
$(BUILD)/vendor/cJSON.o: vendor/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -MMD -MP -c -o $@ $<

# LZ4: upstream sources, no edits, no hand asm.  Tuned separately (-O3
# -march=native) because the pack/unpack path is the hot part of the cache.
$(LZ4_OBJ): vendor/lz4/lz4.c vendor/lz4/lz4.h
	@mkdir -p $(dir $@)
	$(CC) -O3 -march=native -w -Ivendor/lz4 -MMD -MP -c -o $@ $<

clean:
	rm -rf $(BUILD) pinfall tools/dump tools/membench

dump: tools/dump

membench: tools/membench

run-dump: tools/dump
	./tools/dump $(ARGS)

-include $(wildcard $(BUILD)/*.d $(BUILD)/*/*.d $(BUILD)/*/*/*.d)
