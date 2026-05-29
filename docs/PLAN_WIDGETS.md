# PLAN: Lua widget system

A small UI widget toolkit in Lua, living in the engine so both Find5
and SOOB-Engine (and future games) can build menus / HUDs / dialog
content from declarative widget tables instead of hand-rolling
draw_quad/draw_text/draw_region calls per scene.

Scope of this plan: the **first slice**. Five widget types:

- **Label** — text in a bbox.
- **Button** — pressable, with optional icon and 3-state 9-patch bg.
- **Checkbox** — boolean toggle with text label and 4 box-state regions.
- **LineEdit** — single-line text input with 9-patch bg.
- **Slider** — value picker with 9-patch track + sprite knob.

Containers, scrollboxes, dropdowns, tabs — out of scope for the first
slice; each is a natural follow-up once the base widget contract is
proven.

## 0. Where it lives

```
SOOB-Core/scripts/engine/widget.lua
```

Single file for v1. Each widget is a constructor returning a widget
table:

```lua
local widget = require "engine.widget"

local play_btn = widget.button({
    x = 100, y = 50, width = 120, height = 32,
    text = "PLAY",
    bg_up      = "btn_up", bg_down = "btn_down",
    slice      = { x1 = 8, x2 = 24, y1 = 8, y2 = 24 },
    on_click   = function() scene.push(game_scene) end,
})
```

A scene maintains its own list of widgets and dispatches its scene
hooks (`update`/`render`/`mousedown`/`keydown`/etc.) through them. The
plan keeps the widget module *unaware* of `engine.scene` — widgets
are a pure rendering + input toolkit; scene composition stays the
caller's concern. (A small `widget.group(widgets)` helper that wraps
N widgets behind one update/render/input interface is a useful add
once the basics work.)

## 1. Engine bindings required first

The widget system needs two C-side additions in SOOB-Core's
`script.h`. Both are small and generally useful.

### 1.1 `draw_region` with source sub-rect

Today `draw_region` draws a region's full source rect, optionally
scaled and clipped via `fill_x/fill_y`. 9-patch drawing needs to take
an **arbitrary sub-rect** of the source (the middle strips of the
slice) and stretch it to an arbitrary destination size. The current
fill+scale combination cannot extract a *middle* slice — fill clips
from the edge opposite the anchor.

Proposed extension: add `src_x, src_y, src_w, src_h` (sub-rect of the
region's source pixels) and `dst_w, dst_h` (explicit destination
size). Defaults stay backward-compatible.

```lua
draw_region("btn_up", x, y, {
    src_x = 8, src_y = 0, src_w = 16, src_h = 8,   -- top edge strip
    dst_w = 100, dst_h = 8,                         -- stretched across
})
```

Implementation: small change to `scr_draw_region` to override the
source pixel rect and destination size before computing UVs. ~30
lines of C.

### 1.2 Text input + key modifiers

For `LineEdit`. Two pieces:

- **`on_textinput(char)`** Lua hook. Fires when SDL produces a
  printable character (via `event.key.keysym.unicode` in SDL 1.2 — the
  engine already enables `SDL_EnableUNICODE(1)`, so the unicode field
  is populated on KEYDOWN). C-side fires it after `scriptCallKeyDown`.
  Argument: the character as a 1-byte string (we're ASCII for v1; UTF-8
  encoding is a later refinement).
- **`key_modifiers()`** polling binding. Returns `{shift, ctrl, alt}`.
  Useful for Shift+Tab focus-backward, Ctrl+A select-all (future),
  Shift+Arrow selection (future).

`on_keydown` already fires for special keys ("backspace", "return",
"left", "right", etc.) which LineEdit consumes.

If we want to defer LineEdit to a later slice, both of these can land
with it; only #1.1 is needed for Labels/Buttons/Checkboxes/Sliders.

## 2. 9-patch primitive (pure Lua, on top of #1.1)

```lua
-- engine/widget.lua

-- Draw a region as a 9-patch: corners 1:1, edges stretched along their
-- axis, center stretched in both. `slice` carries the four cut lines in
-- source pixels: x1, x2 are the vertical cuts, y1, y2 the horizontal.
--
--   slice = { x1 = 8, x2 = 24, y1 = 8, y2 = 24 }
--   region source 32x32 -> corners 8x8, edges 16 px wide, center 16x16
--
-- (x, y, w, h) is the destination rectangle in virtual-canvas units.
local function draw_9patch(region, x, y, w, h, slice)
    local sx1, sx2, sy1, sy2 = slice.x1, slice.x2, slice.y1, slice.y2
    -- corner sizes derived from slice
    local lw, rw = sx1, slice.region_w - sx2   -- left / right corner widths
    local th, bh = sy1, slice.region_h - sy2   -- top / bottom corner heights
    local mw, mh = sx2 - sx1, sy2 - sy1        -- middle strip sizes (in source)
    -- destination strip sizes
    local dmw = w - lw - rw     -- stretched middle width
    local dmh = h - th - bh     -- stretched middle height

    -- 9 calls: corners at native size, edges stretched along their axis,
    -- center stretched both. Each call uses src_x/src_y/src_w/src_h +
    -- dst_w/dst_h on the new draw_region binding.
    -- ... (TL/T/TR / L/C/R / BL/B/BR)
end
```

The widget passes its `slice` table from its constructor; the source
region's pixel dimensions come from the asset registry (or are stored
with the slice when the widget is built — see §6).

## 3. Base widget contract

Every widget is a table with at minimum:

```
x, y, width, height    number (virtual-canvas units)
disabled               boolean (default false)
visible                boolean (default true)
focused                boolean (managed by the host; widget renders
                       a focus ring or alternate state when true)
```

Standard methods (some optional per widget type):

```
:draw()                              renders this widget
:hit(px, py) -> bool                 does (px,py) hit this widget
:mousedown(px, py, button) -> bool   returns true if consumed
:mouseup(px, py, button)  -> bool
:mousemove(px, py, dx, dy)
:keydown(name) -> bool               return true if consumed
:keyup(name)
:textinput(char)                     LineEdit only
```

Widgets do NOT subscribe themselves to events — the host scene calls
the methods. This keeps lifetime + dispatch order under the scene's
control and avoids a global event bus.

Disabled widgets render in their disabled style and return `false` from
all input methods (no consumption, no hit). Labels also return false
from `:hit()` so clicks fall through.

## 4. Per-widget API

### 4.1 Label

```lua
widget.label({
    x, y, width, height,
    text       = "HEALTH",
    font       = "default",                       -- optional
    align      = ALIGN_CENTER + ALIGN_MIDDLE,     -- text anchor in bbox
    color      = { 1, 1, 1, 1 },
    scale      = 1.0,                             -- multiplier of font lineHeight
})
```

Render: text drawn inside (x, y, width, height) at the requested align.
`disabled = true` dims color by ~50%.

No input handling.

### 4.2 Button

```lua
widget.button({
    x, y, width, height,
    text         = "PLAY",
    font         = "default",                     -- optional
    text_align   = ALIGN_CENTER + ALIGN_MIDDLE,
    text_color   = { 1, 1, 1, 1 },
    text_scale   = 1.0,

    bg_up        = "btn_up",                      -- region (9-patch)
    bg_down      = "btn_down",                    -- region (9-patch)
    bg_disabled  = "btn_disabled",                -- optional (else bg_up dimmed)
    slice        = { x1=8, x2=24, y1=8, y2=24 },

    icon         = "play_icon",                   -- optional region
    icon_align   = ALIGN_LEFT + ALIGN_MIDDLE,     -- icon anchor in bbox
    icon_padding = { l=8, r=0, t=0, b=0 },        -- inset from bbox edges

    on_click     = function(self) end,
})
```

States: `up` (idle) / `down` (mouse pressed within) / `disabled`.

Render order: bg first, then icon (anchored by `icon_align` with
`icon_padding` insetting from each edge), then text (anchored by
`text_align`). Icon and text positions are independent — caller
chooses an alignment combo that doesn't overlap (icon LEFT + text
CENTER is the typical "icon-prefixed centered text" recipe).

Input: mousedown inside bbox sets `pressed = true`. Mouseup inside bbox
fires `on_click(self)`; mouseup outside cancels (no fire). Keyboard:
Return/Space while focused fires `on_click`.

### 4.3 Checkbox

```lua
widget.checkbox({
    x, y, width, height,
    text                  = "Enable shadows",
    font                  = "default",
    color                 = { 1, 1, 1, 1 },
    scale                 = 1.0,

    box_unchecked         = "chk_off",            -- region (sprite, not 9-patch)
    box_checked           = "chk_on",
    box_unchecked_disabled = "chk_off_dim",       -- optional fallback
    box_checked_disabled   = "chk_on_dim",        -- optional fallback

    box_gap               = 6,                    -- px between box and text
    value                 = false,                -- current state
    on_change             = function(self, v) end,
})
```

Layout is fixed: box anchored `ALIGN_LEFT + ALIGN_MIDDLE` inside the
bbox at (x, y), text anchored `ALIGN_LEFT + ALIGN_MIDDLE` starting at
`x + box_width + box_gap`. Box width = the unchecked region's source
width.

Input: mousedown anywhere in bbox toggles `value` on mouseup-inside;
Space toggles when focused. `on_change(self, new_value)` fires after
the flip.

### 4.4 LineEdit

```lua
widget.line_edit({
    x, y, width, height,
    text          = "",
    font          = "default",
    color         = { 1, 1, 1, 1 },
    scale         = 1.0,

    bg_enabled    = "input_bg",                   -- 9-patch
    bg_disabled   = "input_bg_dim",               -- 9-patch
    slice         = { x1=4, x2=12, y1=4, y2=12 },

    padding       = { l=8, r=8, t=4, b=4 },       -- text inset from bbox
    max_length    = 64,                           -- char cap; nil = unlimited
    placeholder   = "",                           -- shown when text == "" and not focused

    on_change     = function(self, text) end,
    on_submit     = function(self, text) end,     -- Enter pressed
})
```

State: `text` (string), `cursor` (int, 0..#text), `focused` (bool).

Render: bg 9-patch first, then either `text` or (if empty + not
focused) `placeholder` at a dimmer color, then a blinking cursor at
the cursor position when focused. Cursor blink: 2 Hz, tracked by the
widget via a private `:_tick(dt)` method called from the scene's
update.

Input:
- `mousedown` inside bbox: focuses widget, sets cursor to the closest
  char index for the click x.
- `textinput(char)`: inserts char at cursor (respecting `max_length`).
  Fires `on_change`.
- `keydown("backspace")`: deletes char left of cursor; fires `on_change`.
- `keydown("delete")`: deletes char right of cursor; fires `on_change`.
- `keydown("left")`/`"right"`: moves cursor.
- `keydown("home")`/`"end"`: jumps to ends.
- `keydown("return")`: fires `on_submit`.

Character measurement uses `uiTextWidth` (already in `ui.h`); for v1
we measure char-by-char from the start of the string each frame —
expensive on long inputs but acceptable for typical UI text. A cached
prefix-width array is a follow-up.

### 4.5 Slider

```lua
widget.slider({
    x, y, width, height,
    track_bg    = "slider_track",                 -- 9-patch
    slice       = { x1=4, x2=12, y1=4, y2=12 },

    knob        = "slider_knob",                  -- region (sprite)
    knob_w      = nil,                            -- nil = use knob's native width
    knob_h      = nil,

    min         = 0.0,
    max         = 100.0,
    value       = 50.0,
    step        = 1.0,                            -- snap; 0 = continuous

    orientation = "horizontal",                   -- or "vertical"

    on_change   = function(self, v) end,          -- fires while dragging
})
```

Render: track 9-patch fills bbox. Knob centered on the track,
positioned along the long axis by `(value - min) / (max - min)`.

Input:
- `mousedown` inside knob bbox: start drag.
- `mousedown` inside track-but-not-knob: jump value to that position
  then start drag.
- `mousemove` during drag: update value from mouse position, clamped
  to [min, max] and snapped to nearest multiple of `step` if step > 0.
  Fires `on_change`.
- `mouseup`: end drag.
- `keydown("left")`/`"right"` (horizontal): adjust by `step` (or
  `(max-min) * 0.05` if `step == 0`).
- Vertical: `"up"`/`"down"`.

## 5. Focus + tab navigation

Focus tracking is per-scene, not global — the scene that holds the
widgets owns a `focused_widget` reference. The widget module provides
a tiny helper to walk a list:

```lua
function widget.focus_next(widgets, current)  -- returns next focusable
function widget.focus_prev(widgets, current)
```

The scene's `keydown("tab")` calls one of these and sets `current.focused
= false; new.focused = true`. Tab order = widget list order; an
explicit `focus_order` field on the widget can override (future).

When focused, a widget draws a 1-px outline at `FOCUS_COLOR` inset by
1 px from the bbox. Standard across all widget types so users see a
consistent focus ring.

## 6. Asset declarations + slice metadata

A 9-patch needs the source region's pixel dimensions plus the slice
edges. Two ergonomic options:

**Option A — slice in the widget constructor.** Caller passes both
the region name and the slice. Engine looks up the region's source
size at draw time. Pro: keeps `assets.lua` simple (no slice info).
Con: every consumer of a 9-patch repeats the slice numbers.

**Option B — slice in assets.lua.** Region entries carry an optional
`slice` field. Widget reads it. Pro: one source of truth per asset.
Con: extends the region schema; engine has to walk `slice` in
`scr_walkRegionsTable`.

Recommend **Option B**. Schema:

```lua
regions = {
    btn_up = {
        tex = "ui_atlas", x = 0, y = 0, w = 32, h = 32,
        slice = { x1 = 8, x2 = 24, y1 = 8, y2 = 24 },   -- new
    },
    ...
}
```

`scr_walkRegionsTable` reads the optional `slice` table when present
and stores it on the Region struct. A new `region_slice(name)` Lua
binding returns `x1, x2, y1, y2` (or nil if not a 9-patch). Widgets
look it up at construction; falls through to the constructor's slice
arg if the region didn't declare one (for ad-hoc cases).

## 7. Implementation order

Each step is independently buildable and testable.

1. **Engine: extend `draw_region` with source sub-rect + dst size.**
   `script.h` change. Smoke-test by rendering a slice manually from
   menu.lua.
2. **Engine: register `slice` on regions + `region_slice(name)` binding.**
   `asset_registry.h` Region struct grows four optional ints +
   `has_slice` flag. `assets.lua` walker reads optional table.
3. **`engine/widget.lua` — base + Label + Button.** Validate the
   pattern end-to-end. Replace SDLFun's hand-rolled menu button code
   in `scripts/menu.lua` with `widget.button` calls — proves
   coexistence with `engine.scene`.
4. **Checkbox + Slider.** Smaller surface; build them next to verify
   the asset declaration pattern works for non-button widgets.
5. **Engine: `on_textinput` hook + `key_modifiers()` polling.** C-side
   binding additions. Smoke-test by printing typed chars to console.
6. **LineEdit.** Cursor render, blink tick, backspace/arrows, focus.
7. **Focus navigation helpers + visible focus ring.** Tab through
   widgets in a sample scene.

## 8. Open questions to settle as we go

- **9-patch alpha**: do the corner / edge / center quads share an
  alpha multiplier? Probably yes — widget background color/alpha
  applies to all 9 quads uniformly.
- **Hover state for Button**: do we add a `bg_hover` slot? Old C menu
  used a "focused" tint that doubled as hover. For v1 we don't have
  hover (just focus + down + disabled) — easy to add later.
- **Slider keyboard step when step=0**: pick a sane default. The plan
  proposes `(max-min) * 0.05` which is 20 keypress increments.
- **LineEdit text overflow**: if text is wider than the widget's
  inner width, do we scroll horizontally? Yes — track a `scroll_x`
  derived from cursor position. Defer to a refinement after the basic
  insertion/deletion works.
- **Container widgets**: `widget.group({...})` that wraps N widgets
  in a single update/draw/input interface. Likely the v1.5 follow-up
  once these five are landed and used in a real scene.

## 9. What this unlocks downstream

- SDLFun's Lua menu (`scripts/menu.lua`) collapses from ~300 lines to
  widget-based declarations: each menu screen is just a list of
  widgets in a scene.
- Find5's options screen + future highscore / credits screens are
  widget compositions.
- The Bioshock-style mini-games sketched in
  `SOOB-Engine/docs/add-mini-game.md` (PIN keypads, hacking
  terminals) become widget assemblies — no more ad-hoc draw_quad +
  hit-test math per puzzle.
