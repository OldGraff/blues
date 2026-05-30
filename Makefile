SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

MODPLUG_LIBS ?= -lmodplug

BUILD := build

# -------------------------
# SOURCES
# -------------------------

BB_SRC := bb/decode.c bb/game.c bb/level.c bb/objects.c bb/resource.c \
          bb/screen.c bb/sound.c bb/staticres.c bb/tiles.c bb/unpack.c

JA_SRC := ja/game.c ja/level.c ja/resource.c ja/screen.c ja/sound.c \
          ja/staticres.c ja/unpack.c

P2_SRC := p2/bosses.c p2/game.c p2/level.c p2/monsters.c p2/resource.c \
          p2/screen.c p2/sound.c p2/staticres.c p2/unpack.c

COMMON_SRC := main.c mixer.c sys_sdl2.c util.c

# -------------------------
# BUILD DIRS
# -------------------------

BB_BUILD := $(BUILD)/bb
JA_BUILD := $(BUILD)/ja
P2_BUILD := $(BUILD)/p2

# -------------------------
# EXECUTABLES
# -------------------------

ifeq ($(OS),Windows_NT)
    EXE := .exe
else
    EXE :=
endif

BB_EXE := $(BUILD)/bb$(EXE)
JA_EXE := $(BUILD)/ja$(EXE)
P2_EXE := $(BUILD)/p2$(EXE)

# -------------------------
# FLAGS
# -------------------------

COMMON_CFLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-Wpedantic -MMD -I. -g $(SDL_CFLAGS)

BB_CFLAGS := $(COMMON_CFLAGS) -DGAME_BB
JA_CFLAGS := $(COMMON_CFLAGS) -DGAME_JA
P2_CFLAGS := $(COMMON_CFLAGS) -DGAME_P2

# -------------------------
# OBJECTS
# -------------------------

BB_OBJS := \
	$(patsubst bb/%.c,$(BB_BUILD)/%.o,$(BB_SRC)) \
	$(patsubst %.c,$(BB_BUILD)/%.o,$(COMMON_SRC))

JA_OBJS := \
	$(patsubst ja/%.c,$(JA_BUILD)/%.o,$(JA_SRC)) \
	$(patsubst %.c,$(JA_BUILD)/%.o,$(COMMON_SRC))

P2_OBJS := \
	$(patsubst p2/%.c,$(P2_BUILD)/%.o,$(P2_SRC)) \
	$(patsubst %.c,$(P2_BUILD)/%.o,$(COMMON_SRC))

# -------------------------
# TARGETS
# -------------------------

.PHONY: all bb ja p2 clean package

all: bb ja p2 package clean

# -------------------------
# BB
# -------------------------

bb: $(BB_EXE)

$(BB_EXE): $(BB_OBJS) | $(BUILD)
	$(CC) -o $@ $^ $(SDL_LIBS) $(MODPLUG_LIBS)

$(BB_BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(BB_CFLAGS) -MMD -MP -c -o $@ $<

$(BB_BUILD)/%.o: bb/%.c
	mkdir -p $(dir $@)
	$(CC) $(BB_CFLAGS) -MMD -MP -c -o $@ $<

# -------------------------
# JA
# -------------------------

ja: $(JA_EXE)

$(JA_EXE): $(JA_OBJS) | $(BUILD)
	$(CC) -o $@ $^ $(SDL_LIBS) $(MODPLUG_LIBS)

$(JA_BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(JA_CFLAGS) -MMD -MP -c -o $@ $<

$(JA_BUILD)/%.o: ja/%.c
	mkdir -p $(dir $@)
	$(CC) $(JA_CFLAGS) -MMD -MP -c -o $@ $<

# -------------------------
# P2
# -------------------------

p2: $(P2_EXE)

$(P2_EXE): $(P2_OBJS) | $(BUILD)
	$(CC) -o $@ $^ $(SDL_LIBS) $(MODPLUG_LIBS)

$(P2_BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(P2_CFLAGS) -MMD -MP -c -o $@ $<

$(P2_BUILD)/%.o: p2/%.c
	mkdir -p $(dir $@)
	$(CC) $(P2_CFLAGS) -MMD -MP -c -o $@ $<

# -------------------------
# CLEAN
# -------------------------

clean:
	rm -rf $(BUILD)/bb $(BUILD)/ja $(BUILD)/p2

# -------------------------
# OPTIONAL PACKAGING
# -------------------------

package: 
ifeq ($(OS),Windows_NT)
	for exe in $(BB_EXE) $(JA_EXE) $(P2_EXE); do \
		ldd $$exe | grep ucrt64 | awk '{print $$3}'; \
	done | sort -u | xargs -I{} cp {} $(BUILD)/
endif
