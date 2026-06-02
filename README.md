# SOOB-Core

Shared 2D + audio + scripting core for retro game projects targeting
**Windows 98** (Dev-C++ / MinGW 3.4, SDL 1.2, fixed-function OpenGL,
OpenAL Soft 1.9.563) through modern Linux / Windows 10.

Headers-only — no build artifacts of its own. Consumers compile their
own `main.cpp` and `#include` the engine headers as part of a single
translation unit. Two projects currently consume SOOB-Core:

- **goph-R/Find5** — 2D spot-the-difference game.
- **goph-R/SOOB-Engine** — 3D FPS engine demo (uses the 2D + audio +
  scripting parts; 3D-specific headers live in that repo).

## Layout

```
math.h            Vec2 / Vec3 type definitions
texture.h         PNG loader + TexCache + TexBlurCache
ui.h              2D virtual-canvas primitives, BMFont, alignment
sound.h           OpenAL wrapper + SoundLibrary
music.h           Streaming Ogg Vorbis + crossfade
asset_registry.h  Name -> path + Region (atlas sub-rect) lookup
script.h          Lua 5.1 glue + bindings + on* hook helpers
config.h          Config file + CLI arg parsing
vendor/           SDL 1.2 (Win98), Lua 5.1.5, stb_image, stb_vorbis
vendor_win10/     SDL/OpenAL headers + libs for the Win10 toolchain
```

## Consuming

Consumers point at this folder with `-I../SOOB-Core/` (sibling-folder
layout) so existing `#include "script.h"` paths resolve without change.

The intent is that updates to the engine apply to both games — adding
a Lua binding, fixing a texture-cache bug, etc. — without manual port
between repos.

## Lua naming

Case depends on *where the name sits*, not what it means:

- **camelCase** — code identifiers (functions, locals, table fields):
  `drawRegion`, `rectContains`, `onUpdate`.
- **snake_case** — string-literal IDs typed in quotes (asset / sound /
  region names, option keys, state tags), including every `assets.lua`
  key: `drawRegion("close_icon")`, `soundPlay("sound_id")`,
  `optGet("music_on")`.
- **UPPER_SNAKE_CASE** — constants (`ALIGN_CENTER`); **CamelCase** — classes.

So one logical name may appear in two casings by role, e.g.
`local musicOn = optGet("music_on")`. Compound event names are fully humped
(`onMouseDown`, `onKeyDown`, scene `:mouseDown`). C bindings expose a
camelCase name (`lua_register(L, "drawText", scrDrawText)`) while the C
wrapper is named `scrCamelCase`; rename both sides together. See `CLAUDE.md`
for the full rule.

## Constraints

- **No C++11.** Dev-C++ 4.x ships GCC 3.4. Use C-style `NULL`, classic
  `for (int i = 0; ...)`, `malloc`/`free` over `new`/`delete`.
- **No shaders.** Fixed-function GL only. Minimum GPU is GeForce 4 MX
  440 — DX7-class, 2 TMUs, GL 1.3 + ARB_multitexture.
- **Header-only `static` functions.** Don't split a module into .h/.cpp.
- **Assets are relative-pathed** by the consuming project. The engine
  itself owns no asset paths.
