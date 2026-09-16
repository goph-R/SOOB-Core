/* app_info.h — the game's identity, loaded from app.lua.
 *
 * A game names itself once, in app.lua beside assets.lua, and every host reads
 * that same file: this header on the desktop, src/host/appinfo.ts on the web,
 * AppInfo.kt in the Android player. See SOOB-Lua.md for the field list.
 *
 * Kept separate from config.h on purpose: config.lua is display settings the
 * player may override per machine (and per command line), while this is what
 * the game *is* — the window title, the save-file stem. Same parsing style as
 * configLoadFromFile: a throwaway lua_State, and any missing file, parse error
 * or wrong shape silently leaves the defaults in place.
 */

#ifndef SOOB_APP_INFO_H
#define SOOB_APP_INFO_H

#include <string.h>
#include <stdio.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#define APP_NAME_MAX  64
#define APP_ID_MAX    64
#define APP_ORIENT_MAX 16
#define APP_DESC_MAX  192
#define APP_BG_MAX    8    /* "#rrggbb" + NUL */

struct AppInfo {
    char name[APP_NAME_MAX];          /* window title / app label */
    char id[APP_ID_MAX];              /* persistence stem: <id>.dat */
    char orientation[APP_ORIENT_MAX]; /* "landscape" | "portrait" (mobile hint) */
    char description[APP_DESC_MAX];   /* one line, for store / manifest use */
    char background[APP_BG_MAX];      /* "#rrggbb" clear colour, every host */
};

static AppInfo appInfoLoadDefaults(void)
{
    AppInfo a;
    /* Deliberately generic: a game that ships without app.lua still runs and
       still persists, it just isn't named. */
    snprintf(a.name, sizeof(a.name), "%s", "SOOB");
    snprintf(a.id, sizeof(a.id), "%s", "soob");
    snprintf(a.orientation, sizeof(a.orientation), "%s", "landscape");
    a.description[0] = '\0';
    /* The colour every host cleared to before app.lua owned it. */
    snprintf(a.background, sizeof(a.background), "%s", "#14141f");
    return a;
}

static void appInfoCopyField(lua_State *L, const char *key, char *out, int out_size)
{
    lua_getfield(L, -1, key);
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if (s && *s) snprintf(out, out_size, "%s", s);
    }
    lua_pop(L, 1);
}

static void appInfoLoadFromFile(AppInfo *a, const char *path)
{
    lua_State *L = luaL_newstate();
    if (!L) return;
    luaL_openlibs(L);

    if (luaL_loadfile(L, path) == 0 && lua_pcall(L, 0, 1, 0) == 0
        && lua_istable(L, -1)) {
        appInfoCopyField(L, "name", a->name, sizeof(a->name));
        appInfoCopyField(L, "id", a->id, sizeof(a->id));
        appInfoCopyField(L, "orientation", a->orientation, sizeof(a->orientation));
        appInfoCopyField(L, "description", a->description, sizeof(a->description));
        appInfoCopyField(L, "background", a->background, sizeof(a->background));
    }
    lua_close(L);
}

/* Parse the "#rrggbb" background into 0..1 floats. Anything malformed leaves
   the outputs untouched, so a caller that pre-seeds them with its own default
   keeps that default rather than going black. Case-insensitive; a leading '#'
   is optional. */
static void appInfoBackgroundRgb(const AppInfo *a, float *r, float *g, float *b)
{
    const char *s = a->background;
    if (*s == '#') s++;
    if (strlen(s) != 6) return;

    int v[6];
    for (int i = 0; i < 6; i++) {
        char c = s[i];
        if      (c >= '0' && c <= '9') v[i] = c - '0';
        else if (c >= 'a' && c <= 'f') v[i] = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v[i] = c - 'A' + 10;
        else return;                  /* not hex — leave the defaults alone */
    }
    *r = (v[0] * 16 + v[1]) / 255.0f;
    *g = (v[2] * 16 + v[3]) / 255.0f;
    *b = (v[4] * 16 + v[5]) / 255.0f;
}

/* Convenience for the usual desktop call site: fill `out` with "<id>.dat". */
static void appInfoOptFileName(const AppInfo *a, char *out, int out_size)
{
    snprintf(out, out_size, "%s.dat", a->id);
}

#endif /* SOOB_APP_INFO_H */
