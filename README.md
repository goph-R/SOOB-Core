# SOOB-Core

Shared 2D + audio + scripting core for retro game projects targeting
**Windows 98** (Dev-C++ / MinGW 3.4, SDL 1.2, fixed-function OpenGL,
OpenAL Soft 1.9.563) through modern Linux / Windows 10.

Headers-only — no build artifacts of its own. Consumers compile their
own `main.cpp` and `#include` the engine headers as part of a single
translation unit. Three projects currently consume SOOB-Core:

- **goph-R/SOOB-Template** — the clone-and-rename starter for a new 2D game.
- **goph-R/Find5** — 2D spot-the-difference game.
- **goph-R/SOOB-Engine** — 3D FPS engine demo (uses the 2D + audio +
  scripting parts; 3D-specific headers live in that repo). It keeps its own
  `main.cpp` — `soob_main.h` below is for 2D games.

## Layout

```
soob_main.h       The whole 2D host: soobRun() — boot, frame loop, shutdown
math.h            Vec2 / Vec3 type definitions
texture.h         PNG loader + TexCache + TexBlurCache
ui.h              2D virtual-canvas primitives, BMFont, alignment
sound.h           OpenAL wrapper + SoundLibrary
music.h           Streaming Ogg Vorbis + crossfade
asset_registry.h  Name -> path + Region (atlas sub-rect) lookup
app_info.h        Game identity from app.lua (name / id / orientation / background)
script.h          Lua 5.1 glue + bindings + on* hook helpers
config.h          Config file + CLI arg parsing
build/            Shared build fragments: soob.mk, soob.cmake, build_win10.bat
scripts/engine/   Lua modules mirrored next to each game's exe at build time:
                  scene, widget, animation, transition, dialog
vendor/           SDL 1.2 (Win98), Lua 5.1.5, stb_image, stb_vorbis,
                  and the runtime SDL.dll / OpenAL32.dll the Windows builds copy
vendor_win10/     SDL/OpenAL headers + libs for the Win10 toolchain
```

## Consuming

Consumers point at this folder with `-I../SOOB-Core/` (sibling-folder
layout) so existing `#include "script.h"` paths resolve without change.

A 2D game includes exactly one header and writes no host code:

```cpp
#include "soob_main.h"
int main(int argc, char *argv[]) { return soobRun(argc, argv, 0); }
```

`soob_main.h` owns the include order, defines `conLogf`, and runs the whole
frame loop. A game with native bindings fills a `SoobApp` and sets
`onRegister`; a game with its own dev console defines `SOOB_CUSTOM_CONLOG` and
supplies its own `conLogf`. Its four build systems reduce to stubs over
`build/soob.mk`, `build/soob.cmake` and `build/build_win10.bat` — see
SOOB-Template.

The Win98 `build.bat` is deliberately **not** shared: COMMAND.COM reopens a
batch file per line and cannot parse `setlocal`, quoted `set` or parenthesised
if-blocks, so each game keeps a goto-only copy with a single `set NAME=` line.

The intent is that updates to the engine apply to every game — adding
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
`local musicOn = optGet("music_on")`. Event hooks are `on` + CamelCase event,
single- or multi-word alike (`onUpdate`, `onMouseDown`, `onKeyDown`); scene
methods drop the `on` (`:update`, `:mouseDown`). C bindings expose a camelCase
name (`lua_register(L, "drawText", scrDrawText)`) while the C wrapper is named
`scrCamelCase`; rename both sides together. See `CLAUDE.md` for the full rule.

## Constraints

- **No C++11.** Dev-C++ 4.x ships GCC 3.4. Use C-style `NULL`, classic
  `for (int i = 0; ...)`, `malloc`/`free` over `new`/`delete`.
- **No shaders.** Fixed-function GL only. Minimum GPU is GeForce 4 MX
  440 — DX7-class, 2 TMUs, GL 1.3 + ARB_multitexture.
- **Header-only `static` functions.** Don't split a module into .h/.cpp.
- **Assets are relative-pathed** by the consuming project. The engine
  itself owns no asset paths.
