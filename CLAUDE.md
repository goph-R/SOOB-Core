# SOOB-Core — agent notes

See `README.md` for the architecture and the Win98 build constraints (no
C++11, fixed-function GL only, header-only `static` functions). This file
covers coding conventions for SOOB-Core and its consumers (Find5,
SOOB-Engine).

## Lua naming convention

A **syntactic split** — the case depends on *where the name sits*, not what
it means:

- **camelCase** — code identifiers: functions, locals, table fields.
  `drawRegion`, `rectContains`, `focusedWidget`, `onUpdate`.
- **snake_case** — string-literal IDs the author types in quotes: asset /
  sound / texture / region / model names, option keys, state tags. This
  includes every key in a consumer's `assets.lua`.
  `drawRegion("close_icon")`, `soundPlay("sound_id")`, `optGet("music_on")`,
  `local STATE_GAME_OVER = "game_over"`.
- **UPPER_SNAKE_CASE** — constants: `ALIGN_CENTER`, `STATE_PLAYING`.
- **CamelCase** — classes, if any.

The same logical name may wear two casings by role — that is intended, not
drift:

```lua
local musicOn = optGet("music_on")   -- camelCase local, snake_case option key
```

`assets.lua` is a pure data manifest: keys (asset IDs) and value strings
(paths) stay snake_case; only its comments use camelCase API names.

**Compound event names are fully humped** — the SDL input events are
camelCased all the way through, not left as one lump. The global hooks are
`onMouseDown` / `onMouseUp` / `onMouseMove` / `onKeyDown` / `onKeyUp` /
`onTextInput` (not `onMousedown`); their scene-method counterparts are
`scene:mouseDown(x, y, b)` / `:keyDown(name)` / `:mouseMove(...)`; the
dispatchers are `dispatchMouseMove` etc. The C hook-dispatch strings
(`scriptBeginHook(s, "onKeyDown")`) move in lockstep with these Lua globals.
(`onUpdate` / `onRender` / `onClick` are single words — already trivially
camelCase.)

## Lua <-> C binding (keep in lockstep)

A C function is exposed to Lua under a **camelCase** name; the C wrapper
itself is named `scrCamelCase` — the registered stem with an `scr` prefix
(`scrDrawText`, `scrKeyDown`, `scrWalkRegionsTable`):

```c
lua_register(L, "drawText", scrDrawText);
```

Renaming a bound name means changing the `lua_register` / `lua_getfield`
string **and** every Lua call site in the same change. Table fields that C
reads back from Lua (`srcX`, `onUpdate`, `scaleX`, the `on*` hooks) follow the
same rule. Asset IDs are *not* hardcoded in C — they are registered from the
Lua table via `lua_next` — so adding an asset is a Lua-only edit.

Leave as-is: vendored code under `vendor/` (Lua 5.1, stb_*) and `bullet3-*`
in SOOB-Engine. The internal C audio library in `sound.h` (`sndPlay`,
`sndLib*`, …) is not Lua-visible and keeps its own names — only the Lua-facing
`soundPlay` API matters.
