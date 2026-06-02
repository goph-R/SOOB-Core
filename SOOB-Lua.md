# SOOB-Lua — scripting API reference

The Lua bindings that **SOOB-Core** exposes to game scripts. All of these are
implemented in `script.h` and registered onto the `lua_State` by `scriptInit`
(`print` is swapped in separately by `scriptInstallConsolePrint`). Consuming
apps (Find5, SOOB-Engine) layer their own extra bindings on top — those are
**not** listed here (see each app's `app_ext.h` / `script_ext.h`).

Naming follows the project convention (see `CLAUDE.md`): **API names are
camelCase**, the **string IDs you pass are snake_case** (asset / sound / option
names live in `assets.lua`). Every C wrapper is `scrCamelCase` (e.g.
`scrDrawText`), but scripts only ever see the registered camelCase name.

Drawing calls must run **inside `onRender`** (the ortho virtual canvas is only
set up there). Coordinates are in the **virtual canvas**: center origin, Y
growing down, `UI_VIRTUAL_H` (480) units tall, width scaling with the window
aspect — the same space `viewSize()` reports.

---

## UI / messages

| Binding | Returns | Notes |
|---|---|---|
| `uiShowMessage(text [, seconds])` | — | Transient HUD message overlay. `seconds` defaults to `3`. |

## Audio

| Binding | Returns | Notes |
|---|---|---|
| `soundPlay(name)` | — | Head-relative one-shot of a registered sound. Warns and no-ops if `name` is unknown. |
| `musicPlay(name [, fadeSec [, loop]])` | — | `fadeSec` default `0.5`, `loop` default `true`. Falls through to a raw path if `name` isn't in the music library. |
| `musicStop([fadeSec])` | — | `fadeSec` default `0.5`. |
| `musicVolume(g)` | — | Global music gain, clamped to `[0,1]`. |

## Input (polling)

Poll any time, including from `onUpdate`. Key names are SDL's lowercase forms
(`"space"`, `"escape"`, `"left"`, `"a"`, `"1"`, `"f1"`) — `print(name)` inside
`onKeyDown` to discover them.

| Binding | Returns | Notes |
|---|---|---|
| `keyDown(name)` | `bool` | Is that key currently held. |
| `mousePos()` | `x, y` | Cursor in virtual-canvas coords. |
| `mouseDown(button)` | `bool` | `1`=left, `2`=middle, `3`=right. |
| `keyModifiers()` | `shift, ctrl, alt` | Three booleans (current modifier state). For chords like Shift-Tab. |

## Drawing — call from `onRender`

```lua
drawRegion(name, x, y [, align [, flip [, fillX [, fillY]]]])
drawRegion(name, x, y, {                 -- options-table form
    align  = ALIGN_CENTER + ALIGN_MIDDLE,
    flip   = FLIP_H,                      -- FLIP_H | FLIP_V
    fillX  = 0.5, fillY = 1.0,           -- [0,1]; <1 clips from the edge opposite the anchor
    scale  = 2.0,                         -- uniform; or scaleX / scaleY per-axis
    scaleX = 1.5, scaleY = 1.0,
    alpha  = 0.5,
    color  = { 1, 0.5, 0.5 },             -- RGB tint, optional A as 4th
    srcX = 8, srcY = 0, srcW = 16, srcH = 8,  -- sub-rect of the region's source pixels (atlas/9-patch)
    dstW = 100, dstH = 32,               -- explicit destination size in vpx (overrides scale)
})
```

| Binding | Returns | Notes |
|---|---|---|
| `drawRegion(name, x, y [, …])` | — | Draw a registered region (atlas sub-rect). Region must exist — no raw-path fallback. 1 source px = 1 vpx unless scaled. |
| `drawText(text, x, y [, scale [, font]])` | — | Positional form. `scale` multiplies the font's native line height. |
| `drawText(text, x, y, {scale, font, align, color, alpha})` | — | Options form. `font` is a name from the `fonts` table; falls back to the built-in 8×8 font if unloaded/unknown. |
| `drawEllipse(cx, cy, rx, ry [, {start, finish, segments, thickness, color, alpha}])` | — | Procedural arc/ellipse. `start`/`finish` in `[0,1]` (tween `finish` for the "drawing" animation). `segments` default 64, `thickness` default 2 (driver may clamp). |
| `drawQuad(x, y, w, h [, {color, alpha}])` | — | Flat-color quad (dim overlays, flashes, backdrops). `(x,y)` is the top-left anchor. |
| `drawBg(name)` | — | Cover-fit a region to the full view (CSS `background-size: cover`), centered, cropping the longer axis. |
| `drawBlur(name [, {width, alpha}])` | — | Blurred "color summary" backdrop: downsamples the source to `width` px (default 16, max 64) and stretches it to fill. `alpha` default `0.6`. First call per `(path,width)` pays the decode; then cached. |

`align` is a bitmask: horizontal `ALIGN_LEFT(1) | ALIGN_CENTER(2) | ALIGN_RIGHT(4)`,
vertical `ALIGN_TOP(8) | ALIGN_MIDDLE(16) | ALIGN_BOTTOM(32)`; `0` = TOP+LEFT.

## Queries / measurement

| Binding | Returns | Notes |
|---|---|---|
| `textWidth(text [, scale [, font]])` | `w` | Rendered width in vpx (same font/scale convention as `drawText`). For cursor positioning, sizing backgrounds. |
| `viewSize()` | `w, h` | Current virtual-canvas size. `h` is always 480; `w` scales with aspect. Anchor against real visible edges. |
| `regionSize(name)` | `w, h` | A region's source pixel size (from `assets.lua`). Nothing if unregistered. |
| `regionSlice(name)` | `x1, x2, y1, y2, w, h` | 9-patch cut lines (region-local px) plus source size. Nothing if the region has no slice. Drives `draw9patch` from one source of truth. |

## Options / persistence

Key-value store backed by a single file (`s->optFile`, e.g. `find5.dat`),
auto-loaded during `scriptInit`. `io` is sandboxed, so this is the supported
way for scripts to persist state.

| Binding | Returns | Notes |
|---|---|---|
| `optSet(name, value)` | — | `value` may be string / number / boolean / nested table; `nil` clears the key. |
| `optGet(name [, default])` | `value` | `default` is returned when the key is unset (forward-compat for new options). |
| `optSave()` | `bool` | Serialize the options table to disk. Atomic (write-to-tmp + rename); `false` + log on failure. |
| `optLoad()` | `bool` | Re-read from disk, discarding in-memory edits since the last save. |

## Misc

| Binding | Returns | Notes |
|---|---|---|
| `print(...)` | — | Overridden to route through `conLogf` (stdout, and the dev console where one exists). Tab-separated like stock `print`. |

---

## Constants (globals set at init)

```
ALIGN_LEFT = 1   ALIGN_CENTER = 2    ALIGN_RIGHT  = 4
ALIGN_TOP  = 8   ALIGN_MIDDLE = 16   ALIGN_BOTTOM = 32
FLIP_H = 1       FLIP_V = 2
```

---

## Lifecycle & event hooks

These are **global functions you define in Lua**; the engine calls them if
present (a missing hook is a silent no-op). They are the inbound half of the
boundary — the bindings above are the outbound half.

| Hook | When |
|---|---|
| `onStart()` | Once, after the entry script has loaded. Open music, show a welcome, etc. |
| `onUpdate(dt)` | Every frame before render. `dt` in seconds. |
| `onRender()` | Every frame; the only place the `draw*` calls are valid. |
| `onKeyDown(name)` / `onKeyUp(name)` | Key press / release. `name` is the SDL lowercase form. |
| `onTextInput(ch)` | A typed character (text entry / `lineEdit`). |
| `onMouseDown(x, y, button)` / `onMouseUp(x, y, button)` | Virtual coords; button `1`/`2`/`3` (`4`/`5` = wheel up/down). |
| `onMouseMove(x, y, dx, dy)` | Virtual coords plus deltas. |

Hook names are `on` + the CamelCase event, single- or multi-word alike. The C
side dispatches each by that exact string (`scriptBeginHook(s, "onKeyDown")`),
so the Lua name and the C string must stay in lockstep.

> The `engine.scene` Lua module (also shipped in SOOB-Core's `scripts/`) wires
> these globals to per-scene methods via `installHooks` — `scene:update(dt)`,
> `:render()`, `:mouseDown(x, y, b)`, `:keyDown(name)`, etc. (the same event
> name with the `on` dropped). That's pure Lua on top of the hooks above, not
> a C binding.

## Assets

The names passed to `soundPlay` / `musicPlay` / `drawRegion` / `drawBg` /
`drawBlur` and the font names for `drawText` are the snake_case keys declared
in the consuming app's `assets.lua`. The host loads that manifest at startup
(`scriptLoadAssets`), populating the sound / music / texture / font / region
registries; unknown names generally warn and no-op (music falls through to a
raw path).
