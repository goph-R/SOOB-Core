#ifndef SCRIPT_H
#define SCRIPT_H

/*
 * Lua 5.1 scripting glue (header-only, static functions).
 *
 * Holds the lua_State and borrowed pointers to UiState, SoundSystem,
 * SoundLibrary, MusicSystem, MusicLibrary, and AssetRegistry so C
 * bindings can reach engine state.
 *
 * Bindings:
 *   uiShowMessage(text [, seconds])    -> UiState transient message
 *   soundPlay(name)                       -> SoundLibrary + SoundSystem
 *   musicPlay(name [, fade [, loop]])
 *   musicStop([fade])
 *   musicVolume(g)
 *
 * Entry points into Lua are scriptCall()'d nullary globals — engine fires
 * onStart() once after assets load.
 *
 * This header must be included after sound.h, music.h, ui.h.
 */

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "asset_registry.h"

struct ScriptSystem {
    lua_State     *L;
    UiState       *ui;
    SoundSystem   *snd;
    SoundLibrary  *sndLib;
    MusicSystem   *music;
    MusicLibrary  *musLib;
    AssetRegistry *assets;
    TexCache      *texCache;   /* shared cache for draw_sprite's lazy PNG loads */
    TexBlurCache  *blurCache;  /* downsampled color-summary textures (drawBlur) */
    const char    *optFile;    /* persistence target, e.g. "find5.dat" — set by scriptInit */
    int            quitRequested; /* requestQuit() sets this; the host loop polls it and exits */
};

/* ---- C bindings ---- */

/* uiShowMessage(text [, seconds])  — seconds defaults to 3. */
static int scrUiShowMessage(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *text = luaL_checkstring(L, 1);
    float seconds    = (float)luaL_optnumber(L, 2, 3.0);
    uiShowMessage(s->ui, text, seconds);
    return 0;
}

/* soundPlay(name) — head-relative playback. Silently warns and returns if
   the name isn't registered. */
static int scrSoundPlay(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    SoundBuffer b = sndLibPick(s->sndLib, name);
    if (!b) {
        conLogf("soundPlay: unknown sound '%s'\n", name);
        return 0;
    }
    sndPlay(s->snd, b);
    return 0;
}

/* musicPlay(name [, fadeSec [, loop]])
     fadeSec defaults to 0.5; loop defaults to true.
   Falls through to a raw path if the name isn't registered in musLib. */
static int scrMusicPlay(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    float fadeSec = (float)luaL_optnumber(L, 2, 0.5);
    int loop = (lua_gettop(L) >= 3) ? lua_toboolean(L, 3) : 1;
    musicPlay(s->music, s->musLib, name, fadeSec, loop);
    return 0;
}

/* musicStop([fadeSec]) — fadeSec defaults to 0.5. */
static int scrMusicStop(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    float fadeSec = (float)luaL_optnumber(L, 1, 0.5);
    musicStop(s->music, fadeSec);
    return 0;
}

/* musicVolume(g) — clamped to [0,1] inside musicSetVolume. */
static int scrMusicVolume(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    float g = (float)luaL_checknumber(L, 1);
    musicSetVolume(s->music, g);
    return 0;
}

/* ---- Input ----
 * Polling: keyDown(name), mousePos(), mouseDown(button).
 * Names are SDL's lowercase forms ("space", "escape", "left", "a", "1",
 * "f1"). Use print(key) inside onKeyDown to discover names you need.
 * Mouse coords are in the UI virtual canvas (center origin, Y-down,
 * ~540 units tall) — same space you draw into with uiIcon / uiText.
 * Mouse buttons: 1=left, 2=middle, 3=right, 4=wheel-up, 5=wheel-down. */

/* Linear-scan name→SDLKey. SDL 1.2 doesn't ship a reverse lookup; the
   key table is ~300 entries and keyDown calls are few per frame, so a
   strcmp loop is fine on a P4. Returns SDLK_UNKNOWN if not found. */
static SDLKey findKeyByName(const char *name)
{
    if (!name) return SDLK_UNKNOWN;
    for (int k = SDLK_FIRST; k < SDLK_LAST; k++) {
        const char *n = SDL_GetKeyName((SDLKey)k);
        if (n && strcmp(n, name) == 0) return (SDLKey)k;
    }
    return SDLK_UNKNOWN;
}

/* keyDown(name) -> bool */
static int scrKeyDown(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    SDLKey k = findKeyByName(name);
    Uint8 *keys = SDL_GetKeyState(NULL);
    lua_pushboolean(L, k != SDLK_UNKNOWN && keys[k]);
    return 1;
}

/* mousePos() -> x, y  (virtual canvas coords) */
static int scrMousePos(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    int px = 0, py = 0;
    SDL_GetMouseState(&px, &py);
    float vx = 0.0f, vy = 0.0f;
    uiMouseToVirtual(s->ui, px, py, &vx, &vy);
    lua_pushnumber(L, vx);
    lua_pushnumber(L, vy);
    return 2;
}

/* mouseDown(button) -> bool  (1=left, 2=middle, 3=right) */
static int scrMouseDown(lua_State *L)
{
    int button = (int)luaL_checkinteger(L, 1);
    Uint8 state = SDL_GetMouseState(NULL, NULL);
    lua_pushboolean(L, (state & SDL_BUTTON(button)) != 0);
    return 1;
}

/* keyModifiers() -> shift, ctrl, alt
 * Current keyboard modifier state as three booleans. Polls
 * SDL_GetModState() at the moment of the call — useful for chord
 * detection (Shift-Tab focus-prev, Ctrl-A select-all, etc.) without
 * threading state through every keyDown event. */
static int scrKeyModifiers(lua_State *L)
{
    SDLMod m = SDL_GetModState();
    lua_pushboolean(L, (m & KMOD_SHIFT) != 0);
    lua_pushboolean(L, (m & KMOD_CTRL)  != 0);
    lua_pushboolean(L, (m & KMOD_ALT)   != 0);
    return 3;
}

/* ---- Options-table helpers ----
 * Read a named numeric / integer / string field from the table at
 * `idx`, returning the default if the field is missing or nil. */
static float scrOptfieldNum(lua_State *L, int idx, const char *key, float def)
{
    lua_getfield(L, idx, key);
    float v = lua_isnil(L, -1) ? def : (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}
static int scrOptfieldInt(lua_State *L, int idx, const char *key, int def)
{
    lua_getfield(L, idx, key);
    int v = lua_isnil(L, -1) ? def : (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}
static const char *scrOptfieldStr(lua_State *L, int idx, const char *key, const char *def)
{
    lua_getfield(L, idx, key);
    const char *v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
    lua_pop(L, 1);
    return v;
}
/* Read opts.color as a 3- or 4-element array, writing into r/g/b/a.
   If `color` is absent the values are left alone. opts.alpha overrides
   color[4] (or the existing a) when present. */
static void scrOptfieldColor(lua_State *L, int idx,
                               float *r, float *g, float *b, float *a)
{
    lua_getfield(L, idx, "color");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1); if (!lua_isnil(L, -1)) *r = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 2); if (!lua_isnil(L, -1)) *g = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 3); if (!lua_isnil(L, -1)) *b = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, -1, 4); if (!lua_isnil(L, -1)) *a = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, idx, "alpha");
    if (!lua_isnil(L, -1)) *a = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
}

/* ---- Rendering ----
 * drawRegion(name, x, y)
 * drawRegion(name, x, y, align [, flip [, fillX [, fillY]]])
 * drawRegion(name, x, y, {
 *     align    = ALIGN_CENTER + ALIGN_MIDDLE,
 *     flip     = FLIP_H,
 *     fillX   = 0.5, fillY = 1.0,
 *     scale    = 2.0,                  -- uniform; sets scaleX and scaleY
 *     scaleX  = 1.5, scaleY = 1.0,   -- non-uniform (overrides `scale`)
 *     alpha    = 0.5,                  -- transparency
 *     color    = { 1, 0.5, 0.5 [, 0.5] }, -- RGB tint, optional A as 4th
 *     srcX    = 8,  srcY  = 0,       -- sub-rect of the REGION's source pixels
 *     srcW    = 16, srcH  = 8,       --   (origin = region top-left). Defaults
 *                                      --   reproduce the whole region. Use for
 *                                      --   atlas frames / 9-patch slice strips.
 *     dstW    = 100, dstH = 32,      -- explicit destination size (vpx);
 *                                      --   overrides the scale-derived size.
 * })
 *
 * align:
 *   horizontal — ALIGN_LEFT(1) | ALIGN_CENTER(2) | ALIGN_RIGHT(4) — default LEFT
 *   vertical   — ALIGN_TOP(8)  | ALIGN_MIDDLE(16)| ALIGN_BOTTOM(32) — default TOP
 *   omit / 0 = TOP+LEFT.
 *
 * flip: FLIP_H(1) | FLIP_V(2), default 0.
 *
 * fillX/y: fractions in [0,1], default 1.0. <1 clips from the edge
 *   opposite the anchor (LEFT-aligned + fillX=0.5 keeps the left half).
 *
 * scale: multiplies destination size; source UVs unchanged so the sprite
 *   is enlarged, not zoomed. scale also acts on the anchor: the anchored
 *   edge stays put while the opposite edge moves out.
 *
 * src_*: override which sub-rect of the region's source pixels is sampled.
 *   Coordinates are RELATIVE to the region (srcX=0, srcY=0 is the
 *   region's top-left, not the texture's). Any subset of the four can be
 *   set; unset ones default to the region's natural box. fillX/fillY
 *   still apply on top of this rect.
 *
 * dstW/h: override the destination size in virtual-canvas pixels.
 *   scaleX/scaleY are ignored on the overridden axis. Useful when
 *   stretching an atlas chunk to a widget-sized rect (9-patch corners,
 *   edges, center).
 *
 * Region must be registered (no raw-path fallback). Must be called from
 * inside uiBegin/uiEnd (i.e., onRender). */
static int scrDrawRegion(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    float x     = (float)luaL_checknumber(L, 2);
    float y     = (float)luaL_checknumber(L, 3);

    /* Defaults */
    int   align = 0;
    int   flip  = 0;
    float fx    = 1.0f, fy = 1.0f;
    float sx_   = 1.0f, sy_ = 1.0f;
    float rot_  = 0.0f;   /* radians, about the dest-rect center */
    float cr_ = 1.0f, cg_ = 1.0f, cb_ = 1.0f, ca_ = 1.0f;

    /* Optional source sub-rect (for atlas frames / 9-patch slices) and
       explicit destination size. has_src bits: 1=srcX, 2=srcY, 4=srcW,
       8=srcH — any subset can override the region's natural box. */
    int   has_src = 0;
    int   src_x_ovr = 0, src_y_ovr = 0, src_w_ovr = 0, src_h_ovr = 0;
    int   has_dst_w = 0, has_dst_h = 0;
    float dst_w_ovr = 0.0f, dst_h_ovr = 0.0f;

    if (lua_istable(L, 4)) {
        /* Options-table form */
        align = scrOptfieldInt(L, 4, "align", 0);
        flip  = scrOptfieldInt(L, 4, "flip",  0);
        fx    = scrOptfieldNum(L, 4, "fillX", 1.0f);
        fy    = scrOptfieldNum(L, 4, "fillY", 1.0f);
        float uniform = scrOptfieldNum(L, 4, "scale", 1.0f);
        sx_   = scrOptfieldNum(L, 4, "scaleX", uniform);
        sy_   = scrOptfieldNum(L, 4, "scaleY", uniform);
        rot_  = scrOptfieldNum(L, 4, "rotation", 0.0f);
        scrOptfieldColor(L, 4, &cr_, &cg_, &cb_, &ca_);

        lua_getfield(L, 4, "srcX");
        if (!lua_isnil(L, -1)) { src_x_ovr = (int)lua_tointeger(L, -1); has_src |= 1; }
        lua_pop(L, 1);
        lua_getfield(L, 4, "srcY");
        if (!lua_isnil(L, -1)) { src_y_ovr = (int)lua_tointeger(L, -1); has_src |= 2; }
        lua_pop(L, 1);
        lua_getfield(L, 4, "srcW");
        if (!lua_isnil(L, -1)) { src_w_ovr = (int)lua_tointeger(L, -1); has_src |= 4; }
        lua_pop(L, 1);
        lua_getfield(L, 4, "srcH");
        if (!lua_isnil(L, -1)) { src_h_ovr = (int)lua_tointeger(L, -1); has_src |= 8; }
        lua_pop(L, 1);

        lua_getfield(L, 4, "dstW");
        if (!lua_isnil(L, -1)) { dst_w_ovr = (float)lua_tonumber(L, -1); has_dst_w = 1; }
        lua_pop(L, 1);
        lua_getfield(L, 4, "dstH");
        if (!lua_isnil(L, -1)) { dst_h_ovr = (float)lua_tonumber(L, -1); has_dst_h = 1; }
        lua_pop(L, 1);
    } else {
        /* Positional form (backward-compat with shipped API). */
        align = (int)luaL_optinteger(L, 4, 0);
        flip  = (int)luaL_optinteger(L, 5, 0);
        fx    = (float)luaL_optnumber(L, 6, 1.0);
        fy    = (float)luaL_optnumber(L, 7, 1.0);
    }

    const Region *rg = assetRegFindRegion(s->assets, name);
    if (!rg) {
        conLogf("drawRegion: unknown region '%s'\n", name);
        return 0;
    }

    const char *texPath = assetRegResolveTexture(s->assets, rg->texName);
    int tw = 0, th = 0;
    GLuint tex = texCacheGetA(s->texCache, texPath, GL_CLAMP_TO_EDGE, 1, &tw, &th);
    if (!tex || tw <= 0 || th <= 0) return 0;

    if (fx < 0.0f) fx = 0.0f; if (fx > 1.0f) fx = 1.0f;
    if (fy < 0.0f) fy = 0.0f; if (fy > 1.0f) fy = 1.0f;

    /* Effective source rect — defaults to the registered region, overridden
       by explicit src_* options. srcX / srcY are RELATIVE to the region
       origin, so a caller asking for "(8, 0)" gets the same pixel regardless
       of where the region sits in the texture. */
    int eff_sx = rg->sx;
    int eff_sy = rg->sy;
    int eff_sw = rg->sw;
    int eff_sh = rg->sh;
    if (has_src & 1) eff_sx = rg->sx + src_x_ovr;
    if (has_src & 2) eff_sy = rg->sy + src_y_ovr;
    if (has_src & 4) eff_sw = src_w_ovr;
    if (has_src & 8) eff_sh = src_h_ovr;

    /* Resolve align (0 in either axis falls back to TOP / LEFT). */
    int alignH = align & 7;
    int alignV = align & 56;
    if (alignH == 0) alignH = 1;   /* LEFT */
    if (alignV == 0) alignV = 8;   /* TOP */

    /* Visible source size after fill clipping (in source pixels). */
    float vis_sw = (float)eff_sw * fx;
    float vis_sh = (float)eff_sh * fy;

    /* Source clip — shrink from the edge OPPOSITE the anchor. */
    float src_x0, src_x1;
    if (alignH == 1) {            /* LEFT  — keep left edge */
        src_x0 = (float)eff_sx;
        src_x1 = (float)eff_sx + vis_sw;
    } else if (alignH == 4) {     /* RIGHT — keep right edge */
        src_x0 = (float)(eff_sx + eff_sw) - vis_sw;
        src_x1 = (float)(eff_sx + eff_sw);
    } else {                      /* CENTER — clip both sides equally */
        src_x0 = (float)eff_sx + ((float)eff_sw - vis_sw) * 0.5f;
        src_x1 = src_x0 + vis_sw;
    }

    float src_y0, src_y1;
    if (alignV == 8) {            /* TOP */
        src_y0 = (float)eff_sy;
        src_y1 = (float)eff_sy + vis_sh;
    } else if (alignV == 32) {    /* BOTTOM */
        src_y0 = (float)(eff_sy + eff_sh) - vis_sh;
        src_y1 = (float)(eff_sy + eff_sh);
    } else {                      /* MIDDLE */
        src_y0 = (float)eff_sy + ((float)eff_sh - vis_sh) * 0.5f;
        src_y1 = src_y0 + vis_sh;
    }

    /* Destination rect — visible source size × scale, or an explicit
       dstW/dstH override. Anchored at (x, y) according to align. */
    float dst_w = has_dst_w ? dst_w_ovr : vis_sw * sx_;
    float dst_h = has_dst_h ? dst_h_ovr : vis_sh * sy_;
    float dst_x;
    if (alignH == 1)      dst_x = x;
    else if (alignH == 4) dst_x = x - dst_w;
    else                  dst_x = x - dst_w * 0.5f;
    float dst_y;
    if (alignV == 8)       dst_y = y;
    else if (alignV == 32) dst_y = y - dst_h;
    else                   dst_y = y - dst_h * 0.5f;

    /* Normalize to UVs. */
    float u0 = src_x0 / (float)tw;
    float u1 = src_x1 / (float)tw;
    float v0 = src_y0 / (float)th;
    float v1 = src_y1 / (float)th;

    if (flip & 1) { float t = u0; u0 = u1; u1 = t; }
    if (flip & 2) { float t = v0; v0 = v1; v1 = t; }

    UiRect dr;
    dr.x = dst_x; dr.y = dst_y; dr.w = dst_w; dr.h = dst_h;
    if (rot_ != 0.0f) {
        uiIconUVColorRot(dr, tex, u0, v0, u1, v1, cr_, cg_, cb_, ca_, rot_);
    } else {
        uiIconUVColor(dr, tex, u0, v0, u1, v1, cr_, cg_, cb_, ca_);
    }
    return 0;
}

/* drawText(text, x, y)
 * drawText(text, x, y, scale [, font])      -- positional, simple
 * drawText(text, x, y, {
 *     scale = 1.5,                           -- multiplier of native lineHeight; default 1.0
 *     font  = "default",                     -- name from assets.lua's fonts table
 *     align = ALIGN_CENTER + ALIGN_MIDDLE,   -- anchors the text rect at (x, y)
 *     color = { 1, 1, 1 [, 1] },             -- RGB(A) tint
 *     alpha = 0.5,                           -- overrides color[4] if both given
 * })
 *
 * Falls back to the built-in 8x8 ASCII font when no font is loaded or
 * the named font is unknown. Must be called from inside uiBegin/uiEnd. */
static int scrDrawText(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *text = luaL_checkstring(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);

    float scale = 1.0f;
    const char *font = NULL;
    int   align = 0;
    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;

    if (lua_istable(L, 4)) {
        scale = scrOptfieldNum(L, 4, "scale", 1.0f);
        font  = scrOptfieldStr(L, 4, "font", NULL);
        align = scrOptfieldInt(L, 4, "align", 0);
        scrOptfieldColor(L, 4, &cr, &cg, &cb, &ca);
    } else {
        scale = (float)luaL_optnumber(L, 4, 1.0);
        font  = lua_isstring(L, 5) ? lua_tostring(L, 5) : NULL;
    }

    /* Map our ALIGN_* bitfield to ui.h's UI_ALIGN_*:
       LEFT(1)→UI_LEFT(4), CENTER(2)→UI_CENTER(8), RIGHT(4)→UI_RIGHT(16),
       TOP(8)→UI_TOP(0),  MIDDLE(16)→UI_MIDDLE(1), BOTTOM(32)→UI_BOTTOM(2).
       Default horizontal = LEFT, default vertical = TOP. */
    int uiAlign = 0;
    if (align & 1)  uiAlign |= UI_ALIGN_LEFT;
    if (align & 2)  uiAlign |= UI_ALIGN_CENTER;
    if (align & 4)  uiAlign |= UI_ALIGN_RIGHT;
    if (align & 16) uiAlign |= UI_ALIGN_MIDDLE;
    if (align & 32) uiAlign |= UI_ALIGN_BOTTOM;
    if (!(align & 7)) uiAlign |= UI_ALIGN_LEFT;   /* no horizontal set → LEFT */

    UiColor c;
    c.r = cr; c.g = cg; c.b = cb; c.a = ca;
    uiText(s->ui, x, y, c, text, scale, uiAlign, font);
    return 0;
}

/* drawEllipse(cx, cy, rx, ry)
 * drawEllipse(cx, cy, rx, ry, {
 *     start     = 0.0,         -- 0..1 around the perimeter (0 = right, sweeps CCW)
 *     finish    = 1.0,         -- tween from 0 to 1 for the "drawing" animation
 *     segments  = 64,          -- vertex density (more = smoother)
 *     thickness = 2.0,         -- line width in pixels; driver may clamp
 *     color     = { 1, 1, 0 [, 1] },
 *     alpha     = 1.0,
 * })
 *
 * Used for the on-find ellipse animation. Procedural — no texture
 * needed. Must be called from inside uiBegin/uiEnd. */
static int scrDrawEllipse(lua_State *L)
{
    float cx = (float)luaL_checknumber(L, 1);
    float cy = (float)luaL_checknumber(L, 2);
    float rx = (float)luaL_checknumber(L, 3);
    float ry = (float)luaL_checknumber(L, 4);

    float startPct = 0.0f, endPct = 1.0f;
    int   segments = 64;
    float thickness = 2.0f;
    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;

    if (lua_istable(L, 5)) {
        startPct  = scrOptfieldNum(L, 5, "start",     0.0f);
        endPct    = scrOptfieldNum(L, 5, "finish",    1.0f);
        segments  = scrOptfieldInt(L, 5, "segments",  64);
        thickness = scrOptfieldNum(L, 5, "thickness", 2.0f);
        scrOptfieldColor(L, 5, &cr, &cg, &cb, &ca);
    }

    UiColor c;
    c.r = cr; c.g = cg; c.b = cb; c.a = ca;
    uiEllipse(cx, cy, rx, ry, startPct, endPct, segments, thickness, c);
    return 0;
}

/* drawQuad(x, y, w, h [, opts])
 * Flat-color quad — useful for dim overlays, flash effects, dialog
 * backdrops, anything that doesn't need a texture. (x, y) is the
 * top-left anchor in virtual-canvas coords; w / h are sizes.
 *
 *   color = { 1, 0, 0 [, 1] }   -- RGB(A) tint, defaults to opaque white
 *   alpha = 0.5                 -- overrides color[4] if both given
 */
static int scrDrawQuad(lua_State *L)
{
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float w = (float)luaL_checknumber(L, 3);
    float h = (float)luaL_checknumber(L, 4);

    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    if (lua_istable(L, 5)) {
        scrOptfieldColor(L, 5, &cr, &cg, &cb, &ca);
    }

    UiRect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    UiColor c;
    c.r = cr; c.g = cg; c.b = cb; c.a = ca;
    uiQuad(r, c);
    return 0;
}

/* textWidth(text [, scale [, font]]) -> w
 * Pixel width of the rendered text in virtual-canvas units, using the
 * same font / scale convention as drawText. Needed for cursor
 * positioning in lineEdit (measure prefix-of-text to find cursor x)
 * and for any UI that needs to size a background around dynamic text. */
static int scrTextWidth(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *text = luaL_checkstring(L, 1);
    float scale      = (float)luaL_optnumber(L, 2, 1.0);
    const char *font = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;

    lua_pushnumber(L, uiTextWidth(s->ui, text, scale, font));
    return 1;
}

/* viewSize()
 * Returns (virtual_w, virtual_h) of the current UI canvas. virtual_h is
 * always UI_VIRTUAL_H (480); virtual_w scales with the window's aspect.
 * Use when you need to anchor against the actual visible edges instead
 * of the 640×480 design rect (e.g. covering background math, edge HUD). */
static int scrViewSize(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_pushnumber(L, uiGetWidth(s->ui));
    lua_pushnumber(L, uiGetHeight(s->ui));
    return 2;
}

/* regionSlice(name)
 * Returns the 9-patch slice cuts attached to a region in assets.lua:
 *   x1, x2, y1, y2  — vertical and horizontal cut lines, in region-local
 *                     source pixels (origin = region's top-left).
 *
 * Returns nothing (i.e. nil in Lua) if the region isn't registered or
 * carries no slice. Widgets use this to drive draw9patch() from a
 * single source of truth — slice numbers live next to the region in
 * assets.lua, not at every widget call site.
 *
 * Plus the region's source width and height as 5th and 6th returns
 * for convenience (callers usually need them to derive corner sizes). */
static int scrRegionSlice(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    const Region *rg = assetRegFindRegion(s->assets, name);
    if (!rg || !rg->hasSlice) return 0;

    lua_pushinteger(L, rg->x1);
    lua_pushinteger(L, rg->x2);
    lua_pushinteger(L, rg->y1);
    lua_pushinteger(L, rg->y2);
    lua_pushinteger(L, rg->sw);
    lua_pushinteger(L, rg->sh);
    return 6;
}

/* regionSize(name) -> w, h
 * Source pixel dimensions of a registered region (the `w` / `h` fields
 * set in assets.lua). Returns nothing if the region isn't registered.
 * Convenience for widgets that need to position adjacent content
 * (Checkbox laying its text after the box; Slider centering its knob
 * on the track). */
static int scrRegionSize(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    const Region *rg = assetRegFindRegion(s->assets, name);
    if (!rg) return 0;

    lua_pushinteger(L, rg->sw);
    lua_pushinteger(L, rg->sh);
    return 2;
}

/* drawBg(name)
 * Draws a region scaled to cover the full view, centered, cropping any
 * overflow on the longer axis (CSS background-size: cover). The region's
 * source pixel size is the "natural" size; we uniformly scale it up by
 * max(view_w / source_w, view_h / source_h) so both dims are covered.
 *
 * Use this for the level / title-screen background. Other UI keeps using
 * drawRegion at design-rect coords. */
static int scrDrawBg(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    const Region *rg = assetRegFindRegion(s->assets, name);
    if (!rg) {
        conLogf("drawBg: unknown region '%s'\n", name);
        return 0;
    }
    const char *texPath = assetRegResolveTexture(s->assets, rg->texName);
    int tw = 0, th = 0;
    GLuint tex = texCacheGetA(s->texCache, texPath, GL_CLAMP_TO_EDGE, 1, &tw, &th);
    if (!tex || tw <= 0 || th <= 0 || rg->sw <= 0 || rg->sh <= 0) return 0;

    float vw = uiGetWidth(s->ui);
    float vh = uiGetHeight(s->ui);
    float bw = (float)rg->sw;
    float bh = (float)rg->sh;
    float scaleX = vw / bw;
    float scaleY = vh / bh;
    float scale  = (scaleX > scaleY) ? scaleX : scaleY;

    float dst_w = bw * scale;
    float dst_h = bh * scale;
    float dst_x = -dst_w * 0.5f;
    float dst_y = -dst_h * 0.5f;

    float u0 = (float)rg->sx                / (float)tw;
    float v0 = (float)rg->sy                / (float)th;
    float u1 = (float)(rg->sx + rg->sw)     / (float)tw;
    float v1 = (float)(rg->sy + rg->sh)     / (float)th;

    UiRect dr;
    dr.x = dst_x; dr.y = dst_y; dr.w = dst_w; dr.h = dst_h;
    uiIconUVColor(dr, tex, u0, v0, u1, v1, 1.0f, 1.0f, 1.0f, 1.0f);
    return 0;
}

/* drawBlur(name [, opts])
 * Draws a blurred "color summary" of the named region as a backdrop:
 * downsamples the source PNG to `width` × (aspect-derived height), uploads
 * it once (cached), then cover-fits the tiny texture over the whole view
 * (centered, cropping the overflow on the longer axis — same framing as
 * drawBg). Upscaling a 16×27 texture produces smooth color gradients between
 * the few sample points — the source image's dominant colors across the
 * screen, without distorting on non-4:3 aspects.
 *
 *   width  (number) downsample width in pixels (default 16, max 64).
 *                   Height is derived from the source aspect ratio.
 *   alpha  (number) draw alpha (default 0.6).
 *
 * The first call per (path, width) pair pays the decode + downsample cost
 * (~ms even on Win98). Subsequent calls hit the blur cache. */
static int scrDrawBlur(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    int   downW = 16;
    float alpha = 0.6f;
    if (lua_istable(L, 2)) {
        downW = scrOptfieldInt(L, 2, "width", 16);
        alpha = scrOptfieldNum(L, 2, "alpha", 0.6f);
    }

    const Region *rg = assetRegFindRegion(s->assets, name);
    if (!rg) {
        conLogf("drawBlur: unknown region '%s'\n", name);
        return 0;
    }
    const char *texPath = assetRegResolveTexture(s->assets, rg->texName);
    if (!texPath) return 0;
    if (rg->sw <= 0 || rg->sh <= 0) return 0;

    /* Downsample only the region's source rect — for textures that are
       POT-padded to a bigger surface than the visible image, this avoids
       averaging in the empty/transparent padding pixels. */
    GLuint tex = texBlurGet(s->blurCache, texPath,
                            rg->sx, rg->sy, rg->sw, rg->sh, downW);
    if (!tex) return 0;

    /* Cover-fit: uniformly scale the blurred summary so it fills the whole
       view, cropping the overflow on the longer axis (same math as drawBg,
       and matches the web host). Preserves the region aspect — no smearing on
       non-4:3 screens. */
    float vw = uiGetWidth(s->ui);
    float vh = uiGetHeight(s->ui);
    float bw = (float)rg->sw;
    float bh = (float)rg->sh;
    float scaleX = vw / bw;
    float scaleY = vh / bh;
    float scale  = (scaleX > scaleY) ? scaleX : scaleY;

    float dst_w = bw * scale;
    float dst_h = bh * scale;

    UiRect dr;
    dr.x = -dst_w * 0.5f;
    dr.y = -dst_h * 0.5f;
    dr.w = dst_w;
    dr.h = dst_h;
    uiBlurFit(dr, tex, 0.0f, 0.0f, 1.0f, 1.0f, alpha);
    return 0;
}

/* ---- Options / persistence ----
 *
 * Key-value store backed by a single file next to the exe. Per-game
 * filename, set by scriptInit's `optFile` arg (e.g. "find5.dat",
 * "sdlfun.dat"). Designed for forward compatibility: each optGet carries
 * its own default, so adding / removing / renaming options across
 * versions never breaks an existing save file (orphan keys are quietly
 * ignored, new keys fall back to defaults).
 *
 * Lua API:
 *   optSet(name, value)        value: string | number | boolean | table
 *                               pass nil to delete
 *   optGet(name)               -> value, or nil
 *   optGet(name, default)      -> default if unset
 *   optSave()                  -> bool — writes the file atomically
 *   optLoad()                  -> bool — re-reads the file into memory
 *
 * Internally the option store is one Lua table in the registry under
 * "engine.opts". optLoad is called automatically at the end of scriptInit
 * so options are ready before the entry script runs; you only need to
 * call it manually if you want to revert to the last-saved state. */

/* Persistence filename is per-game — passed to scriptInit and stashed
   in ScriptSystem::optFile. Functions that need the .tmp variant
   build it locally with snprintf to avoid heap churn. */
#define OPT_MAX_DEPTH   16

/* Push the options table; create + register if it doesn't exist yet. */
static void opt_pushTable(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.opts");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "engine.opts");
    }
}

static int opt_isLuaKeyword(const char *s)
{
    static const char *kw[] = {
        "and","break","do","else","elseif","end","false","for","function",
        "if","in","local","nil","not","or","repeat","return","then","true",
        "until","while", NULL
    };
    for (int i = 0; kw[i]; i++) if (strcmp(s, kw[i]) == 0) return 1;
    return 0;
}

static int opt_isIdentifier(const char *s)
{
    if (!s || !*s) return 0;
    char c = *s;
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (!letter) return 0;
    for (const char *p = s + 1; *p; p++) {
        c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return 0;
    }
    return !opt_isLuaKeyword(s);
}

static void opt_writeString(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if      (c == '"')  fputs("\\\"", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 32)    fprintf(f, "\\%d", c);
        else                fputc(c, f);
    }
    fputc('"', f);
}

/* Return 1 if the table at idx is an "array" (sequential integer keys 1..N
   and nothing else). Used to emit compact { v1, v2, v3 } form instead of
   verbose { [1]=v1, [2]=v2, [3]=v3 }. */
static int opt_isArrayTable(lua_State *L, int idx)
{
    int len = (int)lua_objlen(L, idx);
    if (len <= 0) return 0;
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        count++;
        if (lua_type(L, -2) != LUA_TNUMBER) { lua_pop(L, 2); return 0; }
        double n = lua_tonumber(L, -2);
        if (n != (double)(int)n || (int)n < 1 || (int)n > len) {
            lua_pop(L, 2);
            return 0;
        }
        lua_pop(L, 1);
    }
    return count == len;
}

static void opt_writeValue(lua_State *L, FILE *f, int idx, int depth);

static void opt_writeIndent(FILE *f, int depth)
{
    for (int i = 0; i < depth; i++) fputs("    ", f);
}

static void opt_writeTable(lua_State *L, FILE *f, int idx, int depth)
{
    if (depth > OPT_MAX_DEPTH) {
        /* Cycle / too-deep — bail with nil. Won't produce nice output, but
           won't crash or infinite-loop the serializer either. */
        fputs("nil --[[ too deep ]]", f);
        return;
    }

    /* Empty table compact form */
    lua_pushnil(L);
    if (lua_next(L, idx) == 0) { fputs("{}", f); return; }
    lua_pop(L, 2);

    int isArr = opt_isArrayTable(L, idx);

    fputs("{\n", f);
    if (isArr) {
        int len = (int)lua_objlen(L, idx);
        for (int i = 1; i <= len; i++) {
            lua_rawgeti(L, idx, i);
            opt_writeIndent(f, depth + 1);
            opt_writeValue(L, f, lua_gettop(L), depth + 1);
            fputs(",\n", f);
            lua_pop(L, 1);
        }
    } else {
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            int vt = lua_type(L, -1);
            if (vt == LUA_TFUNCTION || vt == LUA_TUSERDATA ||
                vt == LUA_TTHREAD   || vt == LUA_TLIGHTUSERDATA) {
                lua_pop(L, 1);
                continue;
            }
            int kt = lua_type(L, -2);
            opt_writeIndent(f, depth + 1);
            if (kt == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                if (opt_isIdentifier(k)) {
                    fputs(k, f);
                } else {
                    fputc('[', f); opt_writeString(f, k); fputc(']', f);
                }
                fputs(" = ", f);
            } else if (kt == LUA_TNUMBER) {
                double n = lua_tonumber(L, -2);
                if (n == (double)(long)n) fprintf(f, "[%ld] = ", (long)n);
                else                      fprintf(f, "[%.17g] = ", n);
            } else {
                /* Unsupported key (table/bool/etc.) — skip silently. */
                lua_pop(L, 1);
                continue;
            }
            opt_writeValue(L, f, lua_gettop(L), depth + 1);
            fputs(",\n", f);
            lua_pop(L, 1);
        }
    }
    opt_writeIndent(f, depth);
    fputc('}', f);
}

static void opt_writeValue(lua_State *L, FILE *f, int idx, int depth)
{
    int t = lua_type(L, idx);
    if (t == LUA_TSTRING) {
        opt_writeString(f, lua_tostring(L, idx));
    } else if (t == LUA_TNUMBER) {
        double n = lua_tonumber(L, idx);
        if (n == (double)(long)n) fprintf(f, "%ld", (long)n);
        else                      fprintf(f, "%.17g", n);
    } else if (t == LUA_TBOOLEAN) {
        fputs(lua_toboolean(L, idx) ? "true" : "false", f);
    } else if (t == LUA_TTABLE) {
        opt_writeTable(L, f, idx, depth);
    } else {
        fputs("nil", f);
    }
}

/* optSet(name, value) */
static int scrOptSet(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    /* value (any type, including nil) is at index 2 */
    opt_pushTable(L);              /* push opts */
    lua_pushvalue(L, 2);           /* push value */
    lua_setfield(L, -2, name);     /* opts[name] = value */
    lua_pop(L, 1);                 /* pop opts */
    return 0;
}

/* optGet(name [, default]) */
static int scrOptGet(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int hasDefault = (lua_gettop(L) >= 2);

    opt_pushTable(L);                  /* push opts */
    lua_getfield(L, -1, name);         /* push opts[name] */
    if (lua_isnil(L, -1) && hasDefault) {
        lua_pop(L, 1);                 /* pop nil */
        lua_pushvalue(L, 2);           /* push default */
    }
    lua_remove(L, -2);                 /* drop opts table from beneath */
    return 1;
}

/* optSave() -> bool. Writes the options table to s->optFile. Returns
   true on success, false (with conLogf message) on any failure. Atomic
   via write-to-tmp + rename, so a partial write can't leave a corrupt
   save file. */
static int scrOptSave(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *optFile = s->optFile ? s->optFile : "options.dat";
    char tmpFile[256];
    snprintf(tmpFile, sizeof(tmpFile), "%s.tmp", optFile);

    FILE *f = fopen(tmpFile, "wb");
    if (!f) {
        conLogf("optSave: cannot open %s for writing\n", tmpFile);
        lua_pushboolean(L, 0);
        return 1;
    }
    fputs("return ", f);
    opt_pushTable(L);
    opt_writeValue(L, f, lua_gettop(L), 0);
    lua_pop(L, 1);
    fputs("\n", f);
    fclose(f);

    /* Windows rename() fails if the target already exists; remove first. */
    remove(optFile);
    if (rename(tmpFile, optFile) != 0) {
        conLogf("optSave: rename %s -> %s failed\n", tmpFile, optFile);
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Internal: load the options file (or reset to empty on missing/corrupt).
   Used by both scrOptLoad and scriptInit. Returns 1 on success, 0 on
   any failure — but the registry always ends up holding a usable table. */
static int opt_loadInternal(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    const char *optFile = s->optFile ? s->optFile : "options.dat";

    if (luaL_loadfile(L, optFile) != 0) {
        /* File missing or unreadable — fine on first run. Quiet log. */
        conLogf("optLoad: %s (starting with empty options)\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        goto resetEmpty;
    }
    if (lua_pcall(L, 0, 1, 0) != 0) {
        conLogf("optLoad: parse error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        goto resetEmpty;
    }
    if (!lua_istable(L, -1)) {
        conLogf("optLoad: file did not return a table\n");
        lua_pop(L, 1);
        goto resetEmpty;
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "engine.opts");
    return 1;

resetEmpty:
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "engine.opts");
    return 0;
}

/* optLoad() -> bool. Re-reads s->optFile from disk into the options
   table. Useful to revert in-memory edits to the last saved state. */
static int scrOptLoad(lua_State *L)
{
    lua_pushboolean(L, opt_loadInternal(L));
    return 1;
}

/* ---- Sandboxing ----
 * Default Lua opens os/io/package — a bad script could delete files or
 * load arbitrary DLLs via package.loadlib. Nil out the dangerous stuff,
 * but keep `require` available so game scripts can split into modules
 * (require "tween" -> scripts/tween.lua). package.cpath is cleared so
 * require can't reach any .dll/.so. */
static void scriptSandbox(lua_State *L)
{
    const char *banned[] = {
        "os", "io",
        "dofile", "loadfile", "load", "loadstring",
        "module",
        NULL
    };
    for (int i = 0; banned[i]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, banned[i]);
    }

    /* Restrict require: look in the consuming game's ./scripts/ first,
       then the engine's shared modules under ../SOOB-Core/scripts/.
       Convention: engine modules live in `engine/` so consumers write
       `require "engine.scene"`, sidestepping any name clash with the
       game's own modules. Never look for C libs. */
    luaL_dostring(L,
        "package.path = './scripts/?.lua;./scripts/?/init.lua"
                       ";../SOOB-Core/scripts/?.lua;../SOOB-Core/scripts/?/init.lua'\n"
        "package.cpath = ''\n"
        "package.loadlib = nil\n"
    );
}

/* ---- Error reporting ---- */
static int scrTraceback(lua_State *L)
{
    if (!lua_isstring(L, 1)) return 1;
    lua_getfield(L, LUA_GLOBALSINDEX, "debug");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 1; }
    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return 1; }
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 2);
    lua_call(L, 2, 1);
    return 1;
}

/* ---- Lifecycle ---- */

/* Make a directory if it doesn't already exist. Returns 0 on success
   (path now exists and is a directory) or non-zero otherwise. */
static int scriptEnsureDir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return (st.st_mode & S_IFDIR) ? 0 : -1;
    }
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

/* Resolve the per-user persistence path for `filename` under app subdir
   `app_name`. Writes the full path into out (capacity out_size) and
   creates the app subdirectory if needed.

   Unix: $XDG_CONFIG_HOME/<app>/<file>, or $HOME/.config/<app>/<file>.
   Modern Windows: %APPDATA%\<app>\<file>.
   Win98 (no APPDATA) or no env found: falls back to <file> next to the
   exe. Win98 has full write access everywhere so this is fine; modern
   OSes without the env vars are essentially unsupported anyway. */
static void scriptResolveConfigPath(const char *app_name, const char *filename,
                                    char *out, int out_size)
{
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (base && *base) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s\\%s", base, app_name);
        if (scriptEnsureDir(dir) == 0) {
            snprintf(out, out_size, "%s\\%s", dir, filename);
            conLogf("opt: persistence at %s\n", out);
            return;
        }
    }
    snprintf(out, out_size, "%s", filename);
    conLogf("opt: persistence at %s (relative — APPDATA unavailable)\n", out);
#else
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char dir[512];
    if (xdg && *xdg) {
        snprintf(dir, sizeof(dir), "%s/%s", xdg, app_name);
    } else if (home && *home) {
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.config", home);
        scriptEnsureDir(parent);   /* may exist already, ignore result */
        snprintf(dir, sizeof(dir), "%s/%s", parent, app_name);
    } else {
        snprintf(out, out_size, "%s", filename);
        conLogf("opt: persistence at %s (relative — no HOME)\n", out);
        return;
    }
    if (scriptEnsureDir(dir) == 0) {
        snprintf(out, out_size, "%s/%s", dir, filename);
    } else {
        snprintf(out, out_size, "%s", filename);
    }
    conLogf("opt: persistence at %s\n", out);
#endif
}

/* requestQuit()  — ask the host to exit after the current frame. Sets a
   flag the C main loop polls; the normal shutdown path (scriptShutdown,
   audio / UI teardown, SDL_Quit) still runs, so it's a graceful exit,
   not an abort. Safe to call more than once. Esc remains the hardcoded
   kill-switch in the host loop; this is the script-driven counterpart. */
static int scrRequestQuit(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "engine.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (s) s->quitRequested = 1;
    return 0;
}

/* imeShow(x, y, w, h) / imeHide() — soft-keyboard bridge hooks. The lineEdit
 * widget calls these when it gains / loses focus. On native there is a real
 * keyboard, so they are no-ops; the web host overrides them to summon / hide
 * a hidden <input> (see SOOB-Core-Web). Registered here so the shared
 * widget.lua can call them unconditionally on every consumer. */
static int scrImeShow(lua_State *L) { (void)L; return 0; }
static int scrImeHide(lua_State *L) { (void)L; return 0; }

static int scriptInit(ScriptSystem *s, UiState *ui, SoundSystem *snd,
                      SoundLibrary *sndLib, MusicSystem *music,
                      MusicLibrary *musLib, AssetRegistry *reg,
                      TexCache *texCache, TexBlurCache *blurCache,
                      const char *optFile)
{
    s->ui        = ui;
    s->snd       = snd;
    s->sndLib    = sndLib;
    s->music     = music;
    s->musLib    = musLib;
    s->assets    = reg;
    s->texCache  = texCache;
    s->blurCache = blurCache;
    s->optFile   = optFile;
    s->quitRequested = 0;

    s->L = luaL_newstate();
    if (!s->L) {
        conLogf("script: luaL_newstate failed\n");
        return 0;
    }
    luaL_openlibs(s->L);
    scriptSandbox(s->L);

    lua_pushlightuserdata(s->L, s);
    lua_setfield(s->L, LUA_REGISTRYINDEX, "engine.sys");

    lua_register(s->L, "uiShowMessage", scrUiShowMessage);
    lua_register(s->L, "soundPlay",        scrSoundPlay);
    lua_register(s->L, "musicPlay",      scrMusicPlay);
    lua_register(s->L, "musicStop",      scrMusicStop);
    lua_register(s->L, "musicVolume",    scrMusicVolume);
    lua_register(s->L, "keyDown",        scrKeyDown);
    lua_register(s->L, "mousePos",       scrMousePos);
    lua_register(s->L, "mouseDown",      scrMouseDown);
    lua_register(s->L, "keyModifiers",   scrKeyModifiers);
    lua_register(s->L, "drawRegion",     scrDrawRegion);
    lua_register(s->L, "drawText",       scrDrawText);
    lua_register(s->L, "textWidth",      scrTextWidth);
    lua_register(s->L, "drawEllipse",    scrDrawEllipse);
    lua_register(s->L, "drawQuad",       scrDrawQuad);
    lua_register(s->L, "drawBg",         scrDrawBg);
    lua_register(s->L, "drawBlur",       scrDrawBlur);
    lua_register(s->L, "viewSize",       scrViewSize);
    lua_register(s->L, "regionSlice",    scrRegionSlice);
    lua_register(s->L, "regionSize",     scrRegionSize);
    lua_register(s->L, "optSet",         scrOptSet);
    lua_register(s->L, "optGet",         scrOptGet);
    lua_register(s->L, "optSave",        scrOptSave);
    lua_register(s->L, "optLoad",        scrOptLoad);
    lua_register(s->L, "requestQuit",    scrRequestQuit);
    lua_register(s->L, "imeShow",        scrImeShow);
    lua_register(s->L, "imeHide",        scrImeHide);

    /* Auto-load s->optFile on init so options are ready before the
       entry script runs. Lua code can call optLoad() again later if
       it wants to revert to last-saved state. */
    opt_loadInternal(s->L);

    /* Align / flip constants exposed as Lua globals — see scrDrawRegion
       for the bitfield layout. */
    lua_pushinteger(s->L, 1);  lua_setglobal(s->L, "ALIGN_LEFT");
    lua_pushinteger(s->L, 2);  lua_setglobal(s->L, "ALIGN_CENTER");
    lua_pushinteger(s->L, 4);  lua_setglobal(s->L, "ALIGN_RIGHT");
    lua_pushinteger(s->L, 8);  lua_setglobal(s->L, "ALIGN_TOP");
    lua_pushinteger(s->L, 16); lua_setglobal(s->L, "ALIGN_MIDDLE");
    lua_pushinteger(s->L, 32); lua_setglobal(s->L, "ALIGN_BOTTOM");
    lua_pushinteger(s->L, 1);  lua_setglobal(s->L, "FLIP_H");
    lua_pushinteger(s->L, 2);  lua_setglobal(s->L, "FLIP_V");

    conLogf("script: Lua %s initialised\n", LUA_VERSION);
    return 1;
}

static void scriptShutdown(ScriptSystem *s)
{
    if (s->L) { lua_close(s->L); s->L = NULL; }
}

static int scriptRunFile(ScriptSystem *s, const char *path)
{
    lua_pushcfunction(s->L, scrTraceback);
    int tbidx = lua_gettop(s->L);

    if (luaL_loadfile(s->L, path) != 0) {
        conLogf("script: load %s: %s\n", path, lua_tostring(s->L, -1));
        lua_pop(s->L, 2);
        return 0;
    }
    if (lua_pcall(s->L, 0, 0, tbidx) != 0) {
        conLogf("script: run %s: %s\n", path, lua_tostring(s->L, -1));
        lua_pop(s->L, 2);
        return 0;
    }
    lua_pop(s->L, 1);
    return 1;
}

/* Walk a name=path subtable and dispatch each pair to onPair. Safely
   duplicates keys before lua_tostring per the Lua 5.1 docs. */
static int scrWalkStringTable(lua_State *L, ScriptSystem *s,
                               void (*onPair)(ScriptSystem *, const char *, const char *))
{
    if (!lua_istable(L, -1)) return 0;
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        lua_pushvalue(L, -2);
        const char *name = lua_tostring(L, -1);
        const char *val  = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
        if (name && val) { onPair(s, name, val); count++; }
        lua_pop(L, 2);
    }
    return count;
}

static void scrOnSound(ScriptSystem *s, const char *name, const char *path)
{
    sndLibAdd(s->sndLib, name, sndLoad(path));
}

/* Walk the manifest's `sounds` subtable. A value can be either a single
   path string or an array of paths (variants of the same group, picked
   randomly at play time). */
static int scrWalkSoundsTable(lua_State *L, ScriptSystem *s)
{
    if (!lua_istable(L, -1)) return 0;
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        lua_pushvalue(L, -2);
        const char *name = lua_tostring(L, -1);

        if (name && lua_isstring(L, -2)) {
            scrOnSound(s, name, lua_tostring(L, -2));
            count++;
        } else if (name && lua_istable(L, -2)) {
            int n = (int)lua_objlen(L, -2);
            for (int i = 1; i <= n; i++) {
                lua_rawgeti(L, -2, i);
                if (lua_isstring(L, -1)) {
                    scrOnSound(s, name, lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
            if (n > 0) count++;
        } else if (name) {
            conLogf("script: sounds.%s must be a path string or array of paths\n", name);
        }
        lua_pop(L, 2);
    }
    return count;
}
static void scrOnMusic(ScriptSystem *s, const char *name, const char *path)
{
    musicLibAdd(s->musLib, name, path);
}
static void scrOnModel(ScriptSystem *s, const char *name, const char *path)
{
    assetRegAddModel(s->assets, name, path);
}
static void scrOnTexture(ScriptSystem *s, const char *name, const char *path)
{
    assetRegAddTexture(s->assets, name, path);
}
static void scrOnFont(ScriptSystem *s, const char *name, const char *path)
{
    uiFontLoad(&s->ui->fonts, name, path);
}

/* Walk a `regions` subtable: each value is a sub-table with fields
   { tex = "name", x = N, y = N, w = N, h = N
     [, slice = { x1 = N, x2 = N, y1 = N, y2 = N }] }.
   Missing or non-positive w/h is a config error. The optional `slice`
   sub-table is read out into the Region's 9-patch metadata so widgets
   can call regionSlice(name) at runtime to drive draw9patch without
   repeating the slice numbers at every call site. */
static int scrWalkRegionsTable(lua_State *L, ScriptSystem *s)
{
    if (!lua_istable(L, -1)) return 0;
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        /* stack: ... regionsTable, key, value */
        lua_pushvalue(L, -2);            /* dup key */
        const char *name = lua_tostring(L, -1);

        if (name && lua_istable(L, -2)) {
            lua_getfield(L, -2, "tex");
            const char *tex = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
            lua_pop(L, 1);

            lua_getfield(L, -2, "x");
            int rx = (int)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -2, "y");
            int ry = (int)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -2, "w");
            int rw = (int)lua_tointeger(L, -1); lua_pop(L, 1);
            lua_getfield(L, -2, "h");
            int rh = (int)lua_tointeger(L, -1); lua_pop(L, 1);

            if (tex && rw > 0 && rh > 0) {
                assetRegAddRegion(s->assets, name, tex, rx, ry, rw, rh);

                /* Optional 9-patch slice: { x1, x2, y1, y2 } */
                lua_getfield(L, -2, "slice");
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, "x1");
                    int x1 = (int)lua_tointeger(L, -1); lua_pop(L, 1);
                    lua_getfield(L, -1, "x2");
                    int x2 = (int)lua_tointeger(L, -1); lua_pop(L, 1);
                    lua_getfield(L, -1, "y1");
                    int y1 = (int)lua_tointeger(L, -1); lua_pop(L, 1);
                    lua_getfield(L, -1, "y2");
                    int y2 = (int)lua_tointeger(L, -1); lua_pop(L, 1);
                    if (x1 >= 0 && x2 > x1 && x2 <= rw &&
                        y1 >= 0 && y2 > y1 && y2 <= rh) {
                        assetRegSetLastRegionSlice(s->assets, x1, x2, y1, y2);
                    } else {
                        conLogf("script: region '%s' slice out of range "
                                "(x1=%d x2=%d y1=%d y2=%d, region w=%d h=%d)\n",
                                name, x1, x2, y1, y2, rw, rh);
                    }
                }
                lua_pop(L, 1);   /* slice table or nil */

                count++;
            } else {
                conLogf("script: region '%s' missing tex/w/h\n", name);
            }
        }
        lua_pop(L, 2);                   /* key copy + value */
    }
    return count;
}

/* Load the asset manifest (expected to `return` a table). Walks
   manifest.sounds / manifest.music / manifest.textures / manifest.fonts.
   Missing tables are fine — nothing gets registered. */
static int scriptLoadAssets(ScriptSystem *s, const char *path)
{
    lua_State *L = s->L;
    lua_pushcfunction(L, scrTraceback);
    int tbidx = lua_gettop(L);

    if (luaL_loadfile(L, path) != 0) {
        conLogf("script: load %s: %s\n", path, lua_tostring(L, -1));
        lua_pop(L, 2);
        return 0;
    }
    if (lua_pcall(L, 0, 1, tbidx) != 0) {
        conLogf("script: run %s: %s\n", path, lua_tostring(L, -1));
        lua_pop(L, 2);
        return 0;
    }
    if (!lua_istable(L, -1)) {
        conLogf("script: %s must return a table\n", path);
        lua_pop(L, 2);
        return 0;
    }

    lua_getfield(L, -1, "sounds");
    int sounds = scrWalkSoundsTable(L, s);
    lua_pop(L, 1);

    lua_getfield(L, -1, "music");
    int music = scrWalkStringTable(L, s, scrOnMusic);
    lua_pop(L, 1);

    lua_getfield(L, -1, "models");
    int models = scrWalkStringTable(L, s, scrOnModel);
    lua_pop(L, 1);

    lua_getfield(L, -1, "textures");
    int textures = scrWalkStringTable(L, s, scrOnTexture);
    lua_pop(L, 1);

    lua_getfield(L, -1, "fonts");
    int fonts = scrWalkStringTable(L, s, scrOnFont);
    lua_pop(L, 1);

    lua_getfield(L, -1, "regions");
    int regions = scrWalkRegionsTable(L, s);
    lua_pop(L, 1);

    lua_pop(L, 2);
    conLogf("assets: %d sound(s), %d music, %d model(s), %d texture(s), %d font(s), %d region(s) registered from %s\n",
           sounds, music, models, textures, fonts, regions, path);
    return 1;
}

/* ---- Lua print → conLogf override ---- */

static int scrPrint(lua_State *L)
{
    int n = lua_gettop(L);
    char buf[512];
    int pos = 0;
    lua_getglobal(L, "tostring");
    for (int i = 1; i <= n; i++) {
        lua_pushvalue(L, -1);
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char *s = lua_tostring(L, -1);
        if (!s) s = "(nil)";
        if (pos > 0 && pos < (int)sizeof(buf) - 1) buf[pos++] = '\t';
        while (*s && pos < (int)sizeof(buf) - 1) buf[pos++] = *s++;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    buf[pos] = '\0';
    conLogf("%s\n", buf);
    return 0;
}

static void scriptInstallConsolePrint(ScriptSystem *s)
{
    lua_register(s->L, "print", scrPrint);
}

/* Call a nullary global function if it exists. Missing function is a
   no-op, not an error. */
static int scriptCall(ScriptSystem *s, const char *fn)
{
    lua_pushcfunction(s->L, scrTraceback);
    int tbidx = lua_gettop(s->L);

    lua_getglobal(s->L, fn);
    if (!lua_isfunction(s->L, -1)) {
        lua_pop(s->L, 2);
        return 0;
    }
    if (lua_pcall(s->L, 0, 0, tbidx) != 0) {
        conLogf("script: %s(): %s\n", fn, lua_tostring(s->L, -1));
        lua_pop(s->L, 2);
        return 0;
    }
    lua_pop(s->L, 1);
    return 1;
}

/* ---- Hook helpers ----
 * Two-phase API so callers can push the right argument types between
 * Begin and End without scripCall having to know any signatures.
 *   if (scriptBeginHook(s, "onKeyDown")) {
 *       lua_pushstring(s->L, name);
 *       scriptEndHook(s, "onKeyDown", 1);
 *   }
 * Begin returns 0 (and leaves the stack untouched) if the hook isn't
 * defined; the caller skips End in that case. */
static int scriptBeginHook(ScriptSystem *s, const char *fn)
{
    lua_pushcfunction(s->L, scrTraceback);
    lua_getglobal(s->L, fn);
    if (!lua_isfunction(s->L, -1)) {
        lua_pop(s->L, 2);
        return 0;
    }
    return 1;
}

static void scriptEndHook(ScriptSystem *s, const char *fn, int nargs)
{
    int tbidx = lua_gettop(s->L) - nargs - 1;
    if (lua_pcall(s->L, nargs, 0, tbidx) != 0) {
        conLogf("script: %s(): %s\n", fn, lua_tostring(s->L, -1));
        lua_pop(s->L, 2);
        return;
    }
    lua_pop(s->L, 1); /* traceback */
}

static void scriptCallUpdate(ScriptSystem *s, float dt)
{
    if (!scriptBeginHook(s, "onUpdate")) return;
    lua_pushnumber(s->L, dt);
    scriptEndHook(s, "onUpdate", 1);
}

static void scriptCallKeyDown(ScriptSystem *s, const char *name)
{
    if (!scriptBeginHook(s, "onKeyDown")) return;
    lua_pushstring(s->L, name);
    scriptEndHook(s, "onKeyDown", 1);
}

static void scriptCallKeyUp(ScriptSystem *s, const char *name)
{
    if (!scriptBeginHook(s, "onKeyUp")) return;
    lua_pushstring(s->L, name);
    scriptEndHook(s, "onKeyUp", 1);
}

/* onTextInput(char) — printable character produced by a keypress.
 * Fired AFTER onKeyDown for the same event so nav-key widgets handle
 * arrows / backspace / enter in keyDown, and editor widgets handle
 * character insertion here. `char` is a single-byte Lua string (ASCII
 * only for v1; non-ASCII unicode is dropped). Use SDL 1.2's
 * event.key.keysym.unicode field; SDL_EnableUNICODE(1) must have been
 * called at SDL init (both Find5 and SDLFun already do this). */
static void scriptCallTextInput(ScriptSystem *s, const char *ch)
{
    if (!scriptBeginHook(s, "onTextInput")) return;
    lua_pushstring(s->L, ch);
    scriptEndHook(s, "onTextInput", 1);
}

static void scriptCallMouseDown(ScriptSystem *s, float x, float y, int button)
{
    if (!scriptBeginHook(s, "onMouseDown")) return;
    lua_pushnumber(s->L, x);
    lua_pushnumber(s->L, y);
    lua_pushinteger(s->L, button);
    scriptEndHook(s, "onMouseDown", 3);
}

static void scriptCallMouseUp(ScriptSystem *s, float x, float y, int button)
{
    if (!scriptBeginHook(s, "onMouseUp")) return;
    lua_pushnumber(s->L, x);
    lua_pushnumber(s->L, y);
    lua_pushinteger(s->L, button);
    scriptEndHook(s, "onMouseUp", 3);
}

static void scriptCallMouseMove(ScriptSystem *s, float x, float y, float dx, float dy)
{
    if (!scriptBeginHook(s, "onMouseMove")) return;
    lua_pushnumber(s->L, x);
    lua_pushnumber(s->L, y);
    lua_pushnumber(s->L, dx);
    lua_pushnumber(s->L, dy);
    scriptEndHook(s, "onMouseMove", 4);
}

/* Called inside uiBegin/uiEnd — game scripts use draw_sprite, uiText
   (via future bindings), etc. to render. */
static void scriptCallRender(ScriptSystem *s)
{
    if (!scriptBeginHook(s, "onRender")) return;
    scriptEndHook(s, "onRender", 0);
}

#endif /* SCRIPT_H */
