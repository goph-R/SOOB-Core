#ifndef CONFIG_H
#define CONFIG_H

/*
 * config.h — display config read from config.lua before SDL/GL init.
 *
 * Precedence (low → high):
 *   built-in defaults  →  config.lua  →  command-line args.
 *
 * Uses a throwaway lua_State just for parsing config.lua and closes it
 * immediately. The main game ScriptSystem (which sandboxes io/os) is
 * created later and stays clean.
 *
 * width / height of 0 means "use desktop resolution" — only honored
 * when fullscreen is on; the caller resolves the sentinel after
 * SDL_GetVideoInfo. In windowed mode the final-safety clamp in main.cpp
 * promotes 0 to CONFIG_W_MIN / CONFIG_H_MIN.
 */

#include <cstring>
#include <cstdlib>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#define CONFIG_W_MIN  320
#define CONFIG_H_MIN  240
#define CONFIG_W_MAX  4096
#define CONFIG_H_MAX  4096

struct Config {
    int width;          /* 0 = use desktop res (fullscreen only) */
    int height;         /* 0 = use desktop res (fullscreen only) */
    int fullscreen;     /* 0 / 1 */
    int vsync;          /* 0 / 1 — requested via SDL_GL_SWAP_CONTROL */
    int wExplicitCli;   /* set by configApplyArgs if -w was passed */
    int hExplicitCli;   /* set by configApplyArgs if -h was passed */
};

static Config configLoadDefaults(void)
{
    Config c;
    c.width        = 640;
    c.height       = 480;
    c.fullscreen   = 0;
    c.vsync        = 1;   /* on by default: avoids tearing on modern stacks */
    c.wExplicitCli = 0;
    c.hExplicitCli = 0;
    return c;
}

static void configLoadFromFile(Config *c, const char *path)
{
    /* Missing file, parse error, or wrong shape → silent no-op; the
       defaults already in *c stay. */
    lua_State *L = luaL_newstate();
    if (!L) return;
    luaL_openlibs(L);

    if (luaL_loadfile(L, path) == 0 && lua_pcall(L, 0, 1, 0) == 0
        && lua_istable(L, -1)) {
        lua_getfield(L, -1, "display");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "width");
            if (lua_isnumber(L, -1)) c->width = (int)lua_tonumber(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "height");
            if (lua_isnumber(L, -1)) c->height = (int)lua_tonumber(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "fullscreen");
            if (lua_isboolean(L, -1)) c->fullscreen = lua_toboolean(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "vsync");
            if (lua_isboolean(L, -1)) c->vsync = lua_toboolean(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);  /* "display" (or nil if absent) */
    }
    lua_close(L);
}

static void configApplyArgs(Config *c, int argc, char **argv)
{
    /* Flag style matches SDLFun for muscle-memory consistency:
       -w N, -h N, -fullscreen, -windowed. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            c->width = atoi(argv[++i]);
            c->wExplicitCli = 1;
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            c->height = atoi(argv[++i]);
            c->hExplicitCli = 1;
        } else if (strcmp(argv[i], "-fullscreen") == 0) {
            c->fullscreen = 1;
        } else if (strcmp(argv[i], "-windowed") == 0) {
            c->fullscreen = 0;
        } else if (strcmp(argv[i], "-vsync") == 0) {
            c->vsync = 1;
        } else if (strcmp(argv[i], "-novsync") == 0) {
            c->vsync = 0;
        }
    }
}

static void configClamp(Config *c)
{
    /* 0 is a sentinel for "use desktop" — leave it for the caller to
       resolve after SDL_GetVideoInfo. Cap upper bound unconditionally;
       no monitor / GPU we target accepts > 4096 sensibly. */
    if (c->width  != 0 && c->width  < CONFIG_W_MIN) c->width  = CONFIG_W_MIN;
    if (c->width  > CONFIG_W_MAX)                   c->width  = CONFIG_W_MAX;
    if (c->height != 0 && c->height < CONFIG_H_MIN) c->height = CONFIG_H_MIN;
    if (c->height > CONFIG_H_MAX)                   c->height = CONFIG_H_MAX;
}

#endif
