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
 *   ui_show_message(text [, seconds])    -> UiState transient message
 *   snd_play(name)                       -> SoundLibrary + SoundSystem
 *   music_play(name [, fade [, loop]])
 *   music_stop([fade])
 *   music_volume(g)
 *
 * Entry points into Lua are scriptCall()'d nullary globals — engine fires
 * on_start() once after assets load.
 *
 * This header must be included after sound.h, music.h, ui.h.
 */

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

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
    TexBlurCache  *blurCache;  /* downsampled color-summary textures (draw_blur) */
};

/* ---- C bindings ---- */

/* ui_show_message(text [, seconds])  — seconds defaults to 3. */
static int scr_ui_show_message(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *text = luaL_checkstring(L, 1);
    float seconds    = (float)luaL_optnumber(L, 2, 3.0);
    uiShowMessage(s->ui, text, seconds);
    return 0;
}

/* snd_play(name) — head-relative playback. Silently warns and returns if
   the name isn't registered. */
static int scr_snd_play(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    SoundBuffer b = sndLibPick(s->sndLib, name);
    if (!b) {
        conLogf("snd_play: unknown sound '%s'\n", name);
        return 0;
    }
    sndPlay(s->snd, b);
    return 0;
}

/* music_play(name [, fadeSec [, loop]])
     fadeSec defaults to 0.5; loop defaults to true.
   Falls through to a raw path if the name isn't registered in musLib. */
static int scr_music_play(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    float fadeSec = (float)luaL_optnumber(L, 2, 0.5);
    int loop = (lua_gettop(L) >= 3) ? lua_toboolean(L, 3) : 1;
    musicPlay(s->music, s->musLib, name, fadeSec, loop);
    return 0;
}

/* music_stop([fadeSec]) — fadeSec defaults to 0.5. */
static int scr_music_stop(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    float fadeSec = (float)luaL_optnumber(L, 1, 0.5);
    musicStop(s->music, fadeSec);
    return 0;
}

/* music_volume(g) — clamped to [0,1] inside musicSetVolume. */
static int scr_music_volume(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    float g = (float)luaL_checknumber(L, 1);
    musicSetVolume(s->music, g);
    return 0;
}

/* ---- Input ----
 * Polling: key_down(name), mouse_pos(), mouse_down(button).
 * Names are SDL's lowercase forms ("space", "escape", "left", "a", "1",
 * "f1"). Use print(key) inside on_keydown to discover names you need.
 * Mouse coords are in the UI virtual canvas (center origin, Y-down,
 * ~540 units tall) — same space you draw into with uiIcon / uiText.
 * Mouse buttons: 1=left, 2=middle, 3=right, 4=wheel-up, 5=wheel-down. */

/* Linear-scan name→SDLKey. SDL 1.2 doesn't ship a reverse lookup; the
   key table is ~300 entries and key_down calls are few per frame, so a
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

/* key_down(name) -> bool */
static int scr_key_down(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    SDLKey k = findKeyByName(name);
    Uint8 *keys = SDL_GetKeyState(NULL);
    lua_pushboolean(L, k != SDLK_UNKNOWN && keys[k]);
    return 1;
}

/* mouse_pos() -> x, y  (virtual canvas coords) */
static int scr_mouse_pos(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
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

/* mouse_down(button) -> bool  (1=left, 2=middle, 3=right) */
static int scr_mouse_down(lua_State *L)
{
    int button = (int)luaL_checkinteger(L, 1);
    Uint8 state = SDL_GetMouseState(NULL, NULL);
    lua_pushboolean(L, (state & SDL_BUTTON(button)) != 0);
    return 1;
}

/* ---- Options-table helpers ----
 * Read a named numeric / integer / string field from the table at
 * `idx`, returning the default if the field is missing or nil. */
static float scr_optfield_num(lua_State *L, int idx, const char *key, float def)
{
    lua_getfield(L, idx, key);
    float v = lua_isnil(L, -1) ? def : (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}
static int scr_optfield_int(lua_State *L, int idx, const char *key, int def)
{
    lua_getfield(L, idx, key);
    int v = lua_isnil(L, -1) ? def : (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}
static const char *scr_optfield_str(lua_State *L, int idx, const char *key, const char *def)
{
    lua_getfield(L, idx, key);
    const char *v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
    lua_pop(L, 1);
    return v;
}
/* Read opts.color as a 3- or 4-element array, writing into r/g/b/a.
   If `color` is absent the values are left alone. opts.alpha overrides
   color[4] (or the existing a) when present. */
static void scr_optfield_color(lua_State *L, int idx,
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
 * draw_region(name, x, y)
 * draw_region(name, x, y, align [, flip [, fill_x [, fill_y]]])
 * draw_region(name, x, y, {
 *     align    = ALIGN_CENTER + ALIGN_MIDDLE,
 *     flip     = FLIP_H,
 *     fill_x   = 0.5, fill_y = 1.0,
 *     scale    = 2.0,                  -- uniform; sets scale_x and scale_y
 *     scale_x  = 1.5, scale_y = 1.0,   -- non-uniform (overrides `scale`)
 *     alpha    = 0.5,                  -- transparency
 *     color    = { 1, 0.5, 0.5 [, 0.5] }  -- RGB tint, optional A as 4th
 * })
 *
 * align:
 *   horizontal — ALIGN_LEFT(1) | ALIGN_CENTER(2) | ALIGN_RIGHT(4) — default LEFT
 *   vertical   — ALIGN_TOP(8)  | ALIGN_MIDDLE(16)| ALIGN_BOTTOM(32) — default TOP
 *   omit / 0 = TOP+LEFT.
 *
 * flip: FLIP_H(1) | FLIP_V(2), default 0.
 *
 * fill_x/y: fractions in [0,1], default 1.0. <1 clips from the edge
 *   opposite the anchor (LEFT-aligned + fill_x=0.5 keeps the left half).
 *
 * scale: multiplies destination size; source UVs unchanged so the sprite
 *   is enlarged, not zoomed. scale also acts on the anchor: the anchored
 *   edge stays put while the opposite edge moves out.
 *
 * Region must be registered (no raw-path fallback). Must be called from
 * inside uiBegin/uiEnd (i.e., on_render). */
static int scr_draw_region(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
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
    float cr_ = 1.0f, cg_ = 1.0f, cb_ = 1.0f, ca_ = 1.0f;

    if (lua_istable(L, 4)) {
        /* Options-table form */
        align = scr_optfield_int(L, 4, "align", 0);
        flip  = scr_optfield_int(L, 4, "flip",  0);
        fx    = scr_optfield_num(L, 4, "fill_x", 1.0f);
        fy    = scr_optfield_num(L, 4, "fill_y", 1.0f);
        float uniform = scr_optfield_num(L, 4, "scale", 1.0f);
        sx_   = scr_optfield_num(L, 4, "scale_x", uniform);
        sy_   = scr_optfield_num(L, 4, "scale_y", uniform);
        scr_optfield_color(L, 4, &cr_, &cg_, &cb_, &ca_);
    } else {
        /* Positional form (backward-compat with shipped API). */
        align = (int)luaL_optinteger(L, 4, 0);
        flip  = (int)luaL_optinteger(L, 5, 0);
        fx    = (float)luaL_optnumber(L, 6, 1.0);
        fy    = (float)luaL_optnumber(L, 7, 1.0);
    }

    const Region *rg = assetRegFindRegion(s->assets, name);
    if (!rg) {
        conLogf("draw_region: unknown region '%s'\n", name);
        return 0;
    }

    const char *texPath = assetRegResolveTexture(s->assets, rg->texName);
    int tw = 0, th = 0;
    GLuint tex = texCacheGetA(s->texCache, texPath, GL_CLAMP_TO_EDGE, 1, &tw, &th);
    if (!tex || tw <= 0 || th <= 0) return 0;

    if (fx < 0.0f) fx = 0.0f; if (fx > 1.0f) fx = 1.0f;
    if (fy < 0.0f) fy = 0.0f; if (fy > 1.0f) fy = 1.0f;

    /* Resolve align (0 in either axis falls back to TOP / LEFT). */
    int alignH = align & 7;
    int alignV = align & 56;
    if (alignH == 0) alignH = 1;   /* LEFT */
    if (alignV == 0) alignV = 8;   /* TOP */

    /* Visible source size after fill clipping (in source pixels). */
    float vis_sw = (float)rg->sw * fx;
    float vis_sh = (float)rg->sh * fy;

    /* Source clip — shrink from the edge OPPOSITE the anchor. */
    float src_x0, src_x1;
    if (alignH == 1) {            /* LEFT  — keep left edge */
        src_x0 = (float)rg->sx;
        src_x1 = (float)rg->sx + vis_sw;
    } else if (alignH == 4) {     /* RIGHT — keep right edge */
        src_x0 = (float)(rg->sx + rg->sw) - vis_sw;
        src_x1 = (float)(rg->sx + rg->sw);
    } else {                      /* CENTER — clip both sides equally */
        src_x0 = (float)rg->sx + ((float)rg->sw - vis_sw) * 0.5f;
        src_x1 = src_x0 + vis_sw;
    }

    float src_y0, src_y1;
    if (alignV == 8) {            /* TOP */
        src_y0 = (float)rg->sy;
        src_y1 = (float)rg->sy + vis_sh;
    } else if (alignV == 32) {    /* BOTTOM */
        src_y0 = (float)(rg->sy + rg->sh) - vis_sh;
        src_y1 = (float)(rg->sy + rg->sh);
    } else {                      /* MIDDLE */
        src_y0 = (float)rg->sy + ((float)rg->sh - vis_sh) * 0.5f;
        src_y1 = src_y0 + vis_sh;
    }

    /* Destination rect — visible source size, multiplied by scale,
       anchored at (x, y) according to align. */
    float dst_w = vis_sw * sx_;
    float dst_h = vis_sh * sy_;
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
    uiIconUVColor(dr, tex, u0, v0, u1, v1, cr_, cg_, cb_, ca_);
    return 0;
}

/* draw_text(text, x, y)
 * draw_text(text, x, y, scale [, font])      -- positional, simple
 * draw_text(text, x, y, {
 *     scale = 1.5,                           -- multiplier of native lineHeight; default 1.0
 *     font  = "default",                     -- name from assets.lua's fonts table
 *     align = ALIGN_CENTER + ALIGN_MIDDLE,   -- anchors the text rect at (x, y)
 *     color = { 1, 1, 1 [, 1] },             -- RGB(A) tint
 *     alpha = 0.5,                           -- overrides color[4] if both given
 * })
 *
 * Falls back to the built-in 8x8 ASCII font when no font is loaded or
 * the named font is unknown. Must be called from inside uiBegin/uiEnd. */
static int scr_draw_text(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
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
        scale = scr_optfield_num(L, 4, "scale", 1.0f);
        font  = scr_optfield_str(L, 4, "font", NULL);
        align = scr_optfield_int(L, 4, "align", 0);
        scr_optfield_color(L, 4, &cr, &cg, &cb, &ca);
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

/* draw_ellipse(cx, cy, rx, ry)
 * draw_ellipse(cx, cy, rx, ry, {
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
static int scr_draw_ellipse(lua_State *L)
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
        startPct  = scr_optfield_num(L, 5, "start",     0.0f);
        endPct    = scr_optfield_num(L, 5, "finish",    1.0f);
        segments  = scr_optfield_int(L, 5, "segments",  64);
        thickness = scr_optfield_num(L, 5, "thickness", 2.0f);
        scr_optfield_color(L, 5, &cr, &cg, &cb, &ca);
    }

    UiColor c;
    c.r = cr; c.g = cg; c.b = cb; c.a = ca;
    uiEllipse(cx, cy, rx, ry, startPct, endPct, segments, thickness, c);
    return 0;
}

/* draw_quad(x, y, w, h [, opts])
 * Flat-color quad — useful for dim overlays, flash effects, dialog
 * backdrops, anything that doesn't need a texture. (x, y) is the
 * top-left anchor in virtual-canvas coords; w / h are sizes.
 *
 *   color = { 1, 0, 0 [, 1] }   -- RGB(A) tint, defaults to opaque white
 *   alpha = 0.5                 -- overrides color[4] if both given
 */
static int scr_draw_quad(lua_State *L)
{
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float w = (float)luaL_checknumber(L, 3);
    float h = (float)luaL_checknumber(L, 4);

    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    if (lua_istable(L, 5)) {
        scr_optfield_color(L, 5, &cr, &cg, &cb, &ca);
    }

    UiRect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    UiColor c;
    c.r = cr; c.g = cg; c.b = cb; c.a = ca;
    uiQuad(r, c);
    return 0;
}

/* view_size()
 * Returns (virtual_w, virtual_h) of the current UI canvas. virtual_h is
 * always UI_VIRTUAL_H (480); virtual_w scales with the window's aspect.
 * Use when you need to anchor against the actual visible edges instead
 * of the 640×480 design rect (e.g. covering background math, edge HUD). */
static int scr_view_size(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_pushnumber(L, uiGetWidth(s->ui));
    lua_pushnumber(L, uiGetHeight(s->ui));
    return 2;
}

/* draw_bg(name)
 * Draws a region scaled to cover the full view, centered, cropping any
 * overflow on the longer axis (CSS background-size: cover). The region's
 * source pixel size is the "natural" size; we uniformly scale it up by
 * max(view_w / source_w, view_h / source_h) so both dims are covered.
 *
 * Use this for the level / title-screen background. Other UI keeps using
 * draw_region at design-rect coords. */
static int scr_draw_bg(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    const Region *rg = assetRegFindRegion(s->assets, name);
    if (!rg) {
        conLogf("draw_bg: unknown region '%s'\n", name);
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

/* draw_blur(name [, opts])
 * Draws a blurred "color summary" of the named region as a backdrop:
 * downsamples the source PNG to `width` × (aspect-derived height), uploads
 * it once (cached), then stretches the tiny texture to fill the view width
 * and centers it vertically. Stretching a 16×27 texture up to ~640 px wide
 * produces smooth color gradients between the few sample points — the
 * source image's dominant colors smeared across the screen.
 *
 *   width  (number) downsample width in pixels (default 16, max 64).
 *                   Height is derived from the source aspect ratio.
 *   alpha  (number) draw alpha (default 0.6).
 *
 * The first call per (path, width) pair pays the decode + downsample cost
 * (~ms even on Win98). Subsequent calls hit the blur cache. */
static int scr_draw_blur(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.sys");
    ScriptSystem *s = (ScriptSystem *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    const char *name = luaL_checkstring(L, 1);
    int   downW = 16;
    float alpha = 0.6f;
    if (lua_istable(L, 2)) {
        downW = scr_optfield_int(L, 2, "width", 16);
        alpha = scr_optfield_num(L, 2, "alpha", 0.6f);
    }

    const Region *rg = assetRegFindRegion(s->assets, name);
    if (!rg) {
        conLogf("draw_blur: unknown region '%s'\n", name);
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

    /* Stretch to view width, preserve the region's aspect, center vertically.
       For a taller-than-wide portrait the height overruns the canvas —
       exactly what we want, the blurred color fills top-to-bottom. */
    float vw = uiGetWidth(s->ui);
    float dst_w = vw;
    float dst_h = vw * (float)rg->sh / (float)rg->sw;

    UiRect dr;
    dr.x = -dst_w * 0.5f;
    dr.y = -dst_h * 0.5f;
    dr.w = dst_w;
    dr.h = dst_h;
    uiIconUVColor(dr, tex, 0.0f, 0.0f, 1.0f, 1.0f,
                  1.0f, 1.0f, 1.0f, alpha);
    return 0;
}

/* ---- Options / persistence ----
 *
 * Key-value store backed by a single file `find5.dat` next to the exe.
 * Designed for forward compatibility: each opt_get carries its own default,
 * so adding/removing/renaming options across versions never breaks an
 * existing save file (orphan keys are quietly ignored, new keys fall back
 * to defaults).
 *
 * Lua API:
 *   opt_set(name, value)        value: string | number | boolean | table
 *                               pass nil to delete
 *   opt_get(name)               -> value, or nil
 *   opt_get(name, default)      -> default if unset
 *   opt_save()                  -> bool — writes find5.dat atomically
 *   opt_load()                  -> bool — re-reads find5.dat into memory
 *
 * Internally the option store is one Lua table in the registry under
 * "find5.opts". opt_load is called automatically at the end of scriptInit
 * so options are ready before the entry script runs; you only need to
 * call it manually if you want to revert to the last-saved state. */

#define OPT_FILE        "find5.dat"
#define OPT_FILE_TMP    "find5.dat.tmp"
#define OPT_MAX_DEPTH   16

/* Push the options table; create + register if it doesn't exist yet. */
static void opt_pushTable(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "find5.opts");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "find5.opts");
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

/* opt_set(name, value) */
static int scr_opt_set(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    /* value (any type, including nil) is at index 2 */
    opt_pushTable(L);              /* push opts */
    lua_pushvalue(L, 2);           /* push value */
    lua_setfield(L, -2, name);     /* opts[name] = value */
    lua_pop(L, 1);                 /* pop opts */
    return 0;
}

/* opt_get(name [, default]) */
static int scr_opt_get(lua_State *L)
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

/* opt_save() -> bool. Writes the options table to find5.dat. Returns
   true on success, false (with conLogf message) on any failure. Atomic
   via write-to-tmp + rename, so a partial write can't leave a corrupt
   save file. */
static int scr_opt_save(lua_State *L)
{
    FILE *f = fopen(OPT_FILE_TMP, "wb");
    if (!f) {
        conLogf("opt_save: cannot open %s for writing\n", OPT_FILE_TMP);
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
    remove(OPT_FILE);
    if (rename(OPT_FILE_TMP, OPT_FILE) != 0) {
        conLogf("opt_save: rename %s -> %s failed\n", OPT_FILE_TMP, OPT_FILE);
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Internal: load the options file (or reset to empty on missing/corrupt).
   Used by both scr_opt_load and scriptInit. Returns 1 on success, 0 on
   any failure — but the registry always ends up holding a usable table. */
static int opt_loadInternal(lua_State *L)
{
    if (luaL_loadfile(L, OPT_FILE) != 0) {
        /* File missing or unreadable — fine on first run. Quiet log. */
        conLogf("opt_load: %s (starting with empty options)\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        goto resetEmpty;
    }
    if (lua_pcall(L, 0, 1, 0) != 0) {
        conLogf("opt_load: parse error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        goto resetEmpty;
    }
    if (!lua_istable(L, -1)) {
        conLogf("opt_load: file did not return a table\n");
        lua_pop(L, 1);
        goto resetEmpty;
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "find5.opts");
    return 1;

resetEmpty:
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "find5.opts");
    return 0;
}

/* opt_load() -> bool. Re-reads find5.dat from disk into the options
   table. Useful to revert in-memory edits to the last saved state. */
static int scr_opt_load(lua_State *L)
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

    /* Restrict require: search only scripts/?.lua, never look for C libs. */
    luaL_dostring(L,
        "package.path = './scripts/?.lua;./scripts/?/init.lua'\n"
        "package.cpath = ''\n"
        "package.loadlib = nil\n"
    );
}

/* ---- Error reporting ---- */
static int scr_traceback(lua_State *L)
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

static int scriptInit(ScriptSystem *s, UiState *ui, SoundSystem *snd,
                      SoundLibrary *sndLib, MusicSystem *music,
                      MusicLibrary *musLib, AssetRegistry *reg,
                      TexCache *texCache, TexBlurCache *blurCache)
{
    s->ui        = ui;
    s->snd       = snd;
    s->sndLib    = sndLib;
    s->music     = music;
    s->musLib    = musLib;
    s->assets    = reg;
    s->texCache  = texCache;
    s->blurCache = blurCache;

    s->L = luaL_newstate();
    if (!s->L) {
        conLogf("script: luaL_newstate failed\n");
        return 0;
    }
    luaL_openlibs(s->L);
    scriptSandbox(s->L);

    lua_pushlightuserdata(s->L, s);
    lua_setfield(s->L, LUA_REGISTRYINDEX, "find5.sys");

    lua_register(s->L, "ui_show_message", scr_ui_show_message);
    lua_register(s->L, "snd_play",        scr_snd_play);
    lua_register(s->L, "music_play",      scr_music_play);
    lua_register(s->L, "music_stop",      scr_music_stop);
    lua_register(s->L, "music_volume",    scr_music_volume);
    lua_register(s->L, "key_down",        scr_key_down);
    lua_register(s->L, "mouse_pos",       scr_mouse_pos);
    lua_register(s->L, "mouse_down",      scr_mouse_down);
    lua_register(s->L, "draw_region",     scr_draw_region);
    lua_register(s->L, "draw_text",       scr_draw_text);
    lua_register(s->L, "draw_ellipse",    scr_draw_ellipse);
    lua_register(s->L, "draw_quad",       scr_draw_quad);
    lua_register(s->L, "draw_bg",         scr_draw_bg);
    lua_register(s->L, "draw_blur",       scr_draw_blur);
    lua_register(s->L, "view_size",       scr_view_size);
    lua_register(s->L, "opt_set",         scr_opt_set);
    lua_register(s->L, "opt_get",         scr_opt_get);
    lua_register(s->L, "opt_save",        scr_opt_save);
    lua_register(s->L, "opt_load",        scr_opt_load);

    /* Auto-load find5.dat on init so options are ready before the entry
       script runs. Lua code can call opt_load() again later if it wants
       to revert to last-saved state. */
    opt_loadInternal(s->L);

    /* Align / flip constants exposed as Lua globals — see scr_draw_region
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
    lua_pushcfunction(s->L, scr_traceback);
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
static int scr_walkStringTable(lua_State *L, ScriptSystem *s,
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

static void scr_onSound(ScriptSystem *s, const char *name, const char *path)
{
    sndLibAdd(s->sndLib, name, sndLoad(path));
}

/* Walk the manifest's `sounds` subtable. A value can be either a single
   path string or an array of paths (variants of the same group, picked
   randomly at play time). */
static int scr_walkSoundsTable(lua_State *L, ScriptSystem *s)
{
    if (!lua_istable(L, -1)) return 0;
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        lua_pushvalue(L, -2);
        const char *name = lua_tostring(L, -1);

        if (name && lua_isstring(L, -2)) {
            scr_onSound(s, name, lua_tostring(L, -2));
            count++;
        } else if (name && lua_istable(L, -2)) {
            int n = (int)lua_objlen(L, -2);
            for (int i = 1; i <= n; i++) {
                lua_rawgeti(L, -2, i);
                if (lua_isstring(L, -1)) {
                    scr_onSound(s, name, lua_tostring(L, -1));
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
static void scr_onMusic(ScriptSystem *s, const char *name, const char *path)
{
    musicLibAdd(s->musLib, name, path);
}
static void scr_onTexture(ScriptSystem *s, const char *name, const char *path)
{
    assetRegAddTexture(s->assets, name, path);
}
static void scr_onFont(ScriptSystem *s, const char *name, const char *path)
{
    uiFontLoad(&s->ui->fonts, name, path);
}

/* Walk a `regions` subtable: each value is a sub-table with fields
   { tex = "name", x = N, y = N, w = N, h = N }. Missing or non-positive
   w/h is treated as a config error and logged. */
static int scr_walkRegionsTable(lua_State *L, ScriptSystem *s)
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
    lua_pushcfunction(L, scr_traceback);
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
    int sounds = scr_walkSoundsTable(L, s);
    lua_pop(L, 1);

    lua_getfield(L, -1, "music");
    int music = scr_walkStringTable(L, s, scr_onMusic);
    lua_pop(L, 1);

    lua_getfield(L, -1, "textures");
    int textures = scr_walkStringTable(L, s, scr_onTexture);
    lua_pop(L, 1);

    lua_getfield(L, -1, "fonts");
    int fonts = scr_walkStringTable(L, s, scr_onFont);
    lua_pop(L, 1);

    lua_getfield(L, -1, "regions");
    int regions = scr_walkRegionsTable(L, s);
    lua_pop(L, 1);

    lua_pop(L, 2);
    conLogf("assets: %d sound(s), %d music, %d texture(s), %d font(s), %d region(s) registered from %s\n",
           sounds, music, textures, fonts, regions, path);
    return 1;
}

/* ---- Lua print → conLogf override ---- */

static int scr_print(lua_State *L)
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
    lua_register(s->L, "print", scr_print);
}

/* Call a nullary global function if it exists. Missing function is a
   no-op, not an error. */
static int scriptCall(ScriptSystem *s, const char *fn)
{
    lua_pushcfunction(s->L, scr_traceback);
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
 *   if (scriptBeginHook(s, "on_keydown")) {
 *       lua_pushstring(s->L, name);
 *       scriptEndHook(s, "on_keydown", 1);
 *   }
 * Begin returns 0 (and leaves the stack untouched) if the hook isn't
 * defined; the caller skips End in that case. */
static int scriptBeginHook(ScriptSystem *s, const char *fn)
{
    lua_pushcfunction(s->L, scr_traceback);
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
    if (!scriptBeginHook(s, "on_update")) return;
    lua_pushnumber(s->L, dt);
    scriptEndHook(s, "on_update", 1);
}

static void scriptCallKeyDown(ScriptSystem *s, const char *name)
{
    if (!scriptBeginHook(s, "on_keydown")) return;
    lua_pushstring(s->L, name);
    scriptEndHook(s, "on_keydown", 1);
}

static void scriptCallKeyUp(ScriptSystem *s, const char *name)
{
    if (!scriptBeginHook(s, "on_keyup")) return;
    lua_pushstring(s->L, name);
    scriptEndHook(s, "on_keyup", 1);
}

static void scriptCallMouseDown(ScriptSystem *s, float x, float y, int button)
{
    if (!scriptBeginHook(s, "on_mousedown")) return;
    lua_pushnumber(s->L, x);
    lua_pushnumber(s->L, y);
    lua_pushinteger(s->L, button);
    scriptEndHook(s, "on_mousedown", 3);
}

static void scriptCallMouseUp(ScriptSystem *s, float x, float y, int button)
{
    if (!scriptBeginHook(s, "on_mouseup")) return;
    lua_pushnumber(s->L, x);
    lua_pushnumber(s->L, y);
    lua_pushinteger(s->L, button);
    scriptEndHook(s, "on_mouseup", 3);
}

static void scriptCallMouseMove(ScriptSystem *s, float x, float y, float dx, float dy)
{
    if (!scriptBeginHook(s, "on_mousemove")) return;
    lua_pushnumber(s->L, x);
    lua_pushnumber(s->L, y);
    lua_pushnumber(s->L, dx);
    lua_pushnumber(s->L, dy);
    scriptEndHook(s, "on_mousemove", 4);
}

/* Called inside uiBegin/uiEnd — game scripts use draw_sprite, uiText
   (via future bindings), etc. to render. */
static void scriptCallRender(ScriptSystem *s)
{
    if (!scriptBeginHook(s, "on_render")) return;
    scriptEndHook(s, "on_render", 0);
}

#endif /* SCRIPT_H */
