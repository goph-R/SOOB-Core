# SOOB-Core-Web — 2D browser port plan

A plan to run **2D SOOB-Core** games (Find5 first, as a free web game) in a
modern browser, desktop and mobile-landscape. **3D is out of scope** —
SOOB-Engine stays a native desktop title (Steam / GOG / itch.io), no browser
port now or later. This document covers only the 2D core.

## Why this is tractable

The game is pure Lua talking to a **narrow, documented C surface** — the 24
bindings + the lifecycle/event hooks in [`SOOB-Lua.md`](SOOB-Lua.md). Porting
means reimplementing *that surface* in JavaScript against web APIs; the Lua
scripts, `engine.scene/widget/animation/transition`, and `assets.lua` run
**unchanged**. `SOOB-Lua.md` is effectively the spec the JS host must satisfy.

```
  Find5 Lua (main/menu/dialog + engine/*)   ← unchanged
  assets.lua                                ← unchanged (run to build registries)
        │  calls drawRegion / soundPlay / optGet / …   (SOOB-Lua.md surface)
        ▼
  JS host shim   ── implements the 24 bindings + hooks ──┐
        │                                                │
   Lua-in-browser (WASM)        WebGL │ Web Audio │ DOM events │ localStorage
```

## Lua runtime

Two options; **recommend the first** for exact behavioral parity:

1. **Compile the vendored `vendor/lua-5.1.5` to WASM** (Emscripten, ~200 KB
   gz). The scripts run on the *exact* interpreter they target — no 5.1-vs-5.x
   surprises (`setfenv`/`getfenv`, `unpack`, etc.). Bindings are JS functions
   imported into the module; marshal args across the C-API the same way
   `script.h` does.
2. **Fengari** (Lua 5.3 in pure JS) for a fast MVP. Easiest JS interop, but
   audit for 5.1→5.3 gaps — notably `installHooks(_G)` (no `setfenv` in 5.2+)
   and `unpack`→`table.unpack`.

Either way the host exposes the same global functions/hooks, so Find5's Lua is
identical across desktop and web.

## The host shim — implementing the bindings

Group the [`SOOB-Lua.md`](SOOB-Lua.md) surface by subsystem:

| Area | Bindings | Web implementation |
|---|---|---|
| **Rendering** | `drawRegion` `drawText` `drawEllipse` `drawQuad` `drawBg` `drawBlur` | **WebGL1** sprite batcher: one textured-quad shader with a per-vertex color/alpha tint (matches `uiIconUVColor` exactly — UVs + RGBA). `drawText` = BMFont quads; `drawEllipse` = triangle/line strip; `drawQuad` = flat quad; `drawBg` = cover-fit quad; `drawBlur` = draw the cached downsampled texture stretched up. WebGL1 (universal) is enough; no WebGL2 needed. (Canvas2D is a viable MVP but loses easy tint/alpha parity.) |
| **Queries** | `viewSize` `regionSize` `regionSlice` `textWidth` | Pure JS off the loaded region table + BMFont metrics. `viewSize` returns the current virtual-canvas size (see scaling). |
| **Audio** | `soundPlay` `musicPlay` `musicStop` `musicVolume` | **Web Audio**. Sounds → decoded `AudioBuffer`s, played via `AudioBufferSourceNode`. Music → two `GainNode`s for crossfade (`linearRampToValueAtTime` over `fadeSec`), `musicVolume` = master gain. |
| **Input (polling)** | `keyDown` `mousePos` `mouseDown` `keyModifiers` | Held-keys `Set`, last pointer position, button state — all fed from DOM listeners. |
| **Options** | `optSet` `optGet` `optSave` `optLoad` | In-memory opts table; `optSave`/`optLoad` ↔ **localStorage** (serialize the table to a string, reload as a chunk). The `io` sandbox you already enforce maps perfectly. |
| **Misc** | `print` | → `console.log`. |
| **Constants** | `ALIGN_*`, `FLIP_*` | Set as Lua globals at init, same integer values. |

### Frame loop

`requestAnimationFrame` drives it, mirroring the native loop:

```js
function frame(now) {
  const dt = Math.min((now - last) / 1000, 0.1); last = now;
  callHook("onUpdate", dt);
  beginFrame();                 // clear + bind batcher (the uiBegin/uiEnd equivalent)
  callHook("onRender");         // all draw* calls happen here
  endFrame();                   // flush batches, swap is implicit
  requestAnimationFrame(frame);
}
```

DOM events feed the input hooks: `keydown/keyup → onKeyDown/onKeyUp`,
`pointerdown/up/move → onMouseDown/onMouseUp/onMouseMove` (coords converted to
the virtual canvas), and the held-state used by the polling bindings.

## Asset pipeline

`assets.lua` is a pure data manifest — run it through the Lua VM, read the
returned table, and build the JS-side registries. This is the literal analog of
`scriptLoadAssets`.

| Asset | Native | Web |
|---|---|---|
| Textures (PNG) | stb_image | `fetch` → `createImageBitmap` → `texImage2D` |
| BMFont (`.fnt` + page PNG) | UiFontLib | parse the `.fnt` text, load page texture, build glyph table |
| Sounds (WAV) | OpenAL | `fetch` → `decodeAudioData` |
| Music (Ogg) | stb_vorbis | `decodeAudioData` — **see Safari note** |

> **Ogg on Safari:** historically unsupported. Either ship an `.m4a`/`.webm`
> fallback per track and pick by `canPlayType`, or transcode music to a
> universally-supported codec. PNG/WAV are fine everywhere.

## Display & scaling

The virtual canvas (`UI_VIRTUAL_H = 480`, center origin, Y-down, width scaling
with aspect — already what `viewSize()` reports) maps straight onto a WebGL
viewport. Find5's design rect is 640×480 (4:3).

- Fit the canvas to the window, letter/pillar-boxing to keep the aspect.
- Scale by `devicePixelRatio` for crisp text on HiDPI/mobile.
- Fullscreen via the Fullscreen API (button + auto on mobile).

## Mobile (landscape)

Find5 is "nearly playable on mobile in landscape" — these close the gap:

- **Orientation.** Prompt "rotate to landscape" with a CSS overlay when
  portrait. Lock via the Screen Orientation API where allowed (only in
  fullscreen / installed PWA; **iOS Safari can't force it** — the rotate
  prompt is the fallback there).
- **Touch → mouse.** Pointer Events already cover mouse + touch; map single
  touch to button 1 and feed `onMouseDown/Move/Up` with virtual coords. The
  spot-the-difference tap *is* a click. Ensure every action is reachable by
  touch (no keyboard-only paths) — `keyDown()` polling simply returns false on
  mobile.
- **Audio unlock.** Browsers block audio until a user gesture. Defer the
  `onStart` title music to the first tap/key and `audioContext.resume()` there.
- **No hover.** Widgets must not depend on hover state for usability; tap
  affordances should be obvious.

### Text input — the one real gap (the IME bridge)

The shared `lineEdit` widget (`engine/widget.lua`) **renders its own text and
blinking caret** and only consumes `textInput(ch)` / `keyDown("backspace")`
while it is the focused widget. So a DOM `<input>` is needed **only as an IME
bridge** to summon the soft keyboard and capture characters — it stays
invisible; the Lua widget keeps drawing the field.

Wiring:

1. Overlay a transparent `<input type="text" autocapitalize=off
   autocomplete=off>` (or `inputmode` per field) positioned over the canvas.
2. Add two tiny host bindings, hooked to `lineEdit` focus changes:
   `imeShow(x, y, w, h)` on focus-in (position + `.focus()` the input → soft
   keyboard appears) and `imeHide()` on focus-out (`.blur()`). On **desktop
   these register as no-ops**, so Find5's Lua is identical everywhere; the only
   Core change is the `lineEdit` calling them when its `focused` flag flips.
3. Mirror the bridge input into the existing hooks: the input's `beforeinput` /
   `input` events → `onTextInput(ch)`; `Backspace`/`Enter` → `onKeyDown("backspace")`
   / `onKeyDown("return")`. The Lua widget edits its own buffer exactly as it
   does from a physical keyboard — no widget logic changes.
4. Enter / "Done" blurs the input → `imeHide()` → field commits via the
   existing `onSubmit`/`emitChange`.

This is the standard canvas-game text-entry pattern; here it's small because
the focus + caret + editing already live in Lua. (Find5's likely use is a
highscore-name field; the same bridge serves any `lineEdit`.)

## What changes in the shared Lua / Core

Goal: **Find5's game Lua is byte-identical desktop ↔ web.** Required changes
are tiny and host-level:

- Add `imeShow` / `imeHide` to the binding surface — real on web, no-op stubs
  on desktop (registered in each host, like any other binding). `lineEdit`
  calls them on focus change. Document them in `SOOB-Lua.md`.
- Everything else (renderer, audio, input, assets, persistence) is **new JS in
  the web host only** — no edits to `script.h` or the desktop hosts.
- Optional: a read-only `platform` global (`"web"` / `"desktop"`) for cosmetic
  branching (e.g. showing the rotate hint).

## Packaging & hosting

- Static bundle: `index.html` + JS + Lua-WASM + assets. No server needed.
- **PWA**: web app manifest (`display: fullscreen`, landscape) for
  add-to-home-screen, plus a service worker caching assets for offline play.
- Free distribution: **itch.io HTML5** (zip upload) or self-host on any static
  host. A free web build is also a funnel toward the native SOOB-Engine titles.

## Milestones

1. **Boots** — Lua-WASM up, `assets.lua` loaded, one scene renders via the
   WebGL batcher; mouse → `onMouse*`. (Proves the binding surface.)
2. **Playable desktop web** — text (BMFont), audio, options→localStorage,
   transitions; full Find5 loop in a desktop browser.
3. **Mobile landscape** — touch mapping, orientation prompt/lock, audio unlock,
   fit/scale to device.
4. **Mobile text input** — the IME bridge (`imeShow/imeHide` + hidden input).
5. **Polish & ship** — PWA/offline, HiDPI, loading screen, itch.io page.

## Risks / open questions

- **Ogg on Safari** — needs a fallback codec or transcode (see above).
- **iOS orientation lock** — not enforceable outside PWA/fullscreen; rely on the
  rotate prompt.
- **Soft-keyboard quirks** — iOS/Android differ on `input`/`beforeinput` and
  viewport resize when the keyboard opens; test the IME bridge on both.
- **Lua-WASM bundle size & startup** — acceptable (~200 KB), but measure cold
  start; precompile scripts to bytecode if needed.
- **Pointer precision** — fat-finger taps on a spot-the-difference grid; may
  want a small touch-radius tolerance on hit-testing for mobile.

See [`SOOB-Lua.md`](SOOB-Lua.md) for the binding spec and [`CLAUDE.md`](CLAUDE.md)
for naming conventions.
