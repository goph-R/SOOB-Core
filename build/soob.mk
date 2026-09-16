# soob.mk — the shared Linux build for a 2D SOOB-Core game.
#
# A game's Makefile is three lines:
#
#     BIN     = mygame
#     ENGINE ?= ../SOOB-Core
#     include $(ENGINE)/build/soob.mk
#
# Optional hooks, set BEFORE the include:
#   SOOB_DEFS   extra -D flags       SOOB_INCS   extra -I flags
#   SOOB_OBJ    extra .o to link     SOOB_LIBS   extra -l flags
#   SOOB_NO_SOFTWARE=1               leave the CPU rasterizer out
#
# Requires libsdl1.2-dev and libopenal-dev.

# NOTE: not CPP/CC — those are GNU make built-ins (CPP defaults to "$(CC) -E"),
# so `?=` would silently leave the preprocessor in place instead of the
# compiler. Own names, overridable from the environment or the command line.
SOOB_CXX ?= g++
SOOB_CC  ?= gcc
ENGINE   ?= ../SOOB-Core
LUA_SRC  = $(ENGINE)/vendor/lua-5.1.5/src
LUA_CFLAGS = -I$(LUA_SRC) -Dluaall_c -DLUA_USE_POSIX

# -DSOOB_SOFTWARE_BACKEND compiles the CPU rasterizer in alongside GL; the
# active backend is picked at runtime by config.lua's display.render. It costs
# nothing when unused and is what makes a game viable on a machine with no 3D
# accelerator, so it is on by default.
ifndef SOOB_NO_SOFTWARE
SOOB_DEFS += -DSOOB_SOFTWARE_BACKEND
endif

CXXFLAGS = $(shell sdl-config --cflags) -I$(ENGINE) -I$(LUA_SRC) -O2 \
           $(SOOB_DEFS) $(SOOB_INCS)
LIBS = $(shell sdl-config --libs) -lGL -lopenal $(SOOB_LIBS)

OBJ = main.o lua.o vorbis.o $(SOOB_OBJ)

# The game's Makefile sets BIN before this include, so `all` is not otherwise
# the first target make sees.
.DEFAULT_GOAL := all

all: $(BIN) scripts/engine

# Mirror the engine's Lua modules next to the exe so shipped builds find
# `require "engine.scene"` via ./scripts/?.lua without needing the SOOB-Core
# repo on the player's machine.
scripts/engine: $(wildcard $(ENGINE)/scripts/engine/*.lua)
	mkdir -p scripts/engine
	cp $(ENGINE)/scripts/engine/*.lua scripts/engine/
	touch scripts/engine

$(BIN): $(OBJ)
	$(SOOB_CXX) $(OBJ) -o $(BIN) $(LIBS)

# soob_main.h pulls in every engine module plus the software rasterizer, so
# both header directories are dependencies of the single game TU.
main.o: main.cpp $(wildcard $(ENGINE)/*.h) $(wildcard $(ENGINE)/swrender/*.h)
	$(SOOB_CXX) -c main.cpp -o main.o $(CXXFLAGS)

# stb_vorbis (Ogg Vorbis decoder, public domain). Built as its own C TU so
# editing main.cpp doesn't pay its recompile cost. music.h includes the same
# file with STB_VORBIS_HEADER_ONLY for prototypes only.
vorbis.o: $(ENGINE)/vendor/stb/stb_vorbis.c
	$(SOOB_CC) -x c -c $< -o $@ -O2

# Lua 5.1.5 compiled as a single C TU via the unity-build aggregator.
lua.o: $(LUA_SRC)/lua_all.c
	$(SOOB_CC) -x c -c $< -o $@ $(LUA_CFLAGS) -O2

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
