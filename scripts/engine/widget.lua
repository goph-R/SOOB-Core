-- engine/widget.lua — declarative UI widgets on top of the engine bindings.
--
-- A widget is a plain Lua table with x / y / width / height + a few
-- methods (draw / hit / mousedown / mouseup / mousemove / keydown /
-- keyup). Constructors live in this module:
--
--   widget.label(spec)    -- text in a bbox
--   widget.image(spec)    -- static region (logo, decoration) — alpha/
--                            scale animatable
--   widget.button(spec)   -- 3-state 9-patch bg + optional icon + text
--   widget.panel(spec)    -- container: children in local coords,
--                            owns focus across them, fans out events
--
-- A host scene keeps a list of widgets — or a single panel that owns
-- them — and dispatches its scene-hook callbacks through it. This
-- module stays unaware of engine.scene so widgets can be used inside
-- any rendering context.
--
-- Spatial cursor navigation: when the focused widget's `keydown(name)`
-- returns false for "up" / "down" / "left" / "right", the scene calls
-- widget.focusDirection(widgets, current, name) to move focus to the
-- nearest enabled focusable widget in that direction. Tab navigation
-- uses widget.focusNext / focusPrev which wrap.
--
-- 9-patch: draw9patch(name, x, y, w, h) reads the slice metadata
-- attached to the region in assets.lua via regionSlice(), then issues
-- 9 drawRegion calls (corners 1:1, edges stretched along their axis,
-- center stretched both). Regions without a slice degrade to a single
-- stretched drawRegion.

local anim = require "engine.animation"

local M = {}

-- ---- Shared helpers ----------------------------------------------------

local function rectContains(x, y, w, h, px, py)
    return px >= x and px < x + w and py >= y and py < y + h
end

-- Extract horizontal / vertical bit groups out of an ALIGN_* bitmask.
-- (Lua 5.1 has no bitwise ops; ALIGN_* values are powers of 2 in two
-- disjoint groups so arithmetic does the job.)
--   horizontal: ALIGN_LEFT(1) | ALIGN_CENTER(2) | ALIGN_RIGHT(4)
--   vertical:   ALIGN_TOP(8)  | ALIGN_MIDDLE(16)| ALIGN_BOTTOM(32)
local function splitAlign(align)
    local ah = align % 8           -- 0..7
    local av = align - ah          -- 0, 8, 16, or 32
    return ah, av
end

-- Anchor (x, y) point inside a bbox according to an ALIGN_* mask.
-- Returns the point that drawText / drawRegion should be anchored at
-- so the resulting glyph rect lands aligned within (bx, by, bw, bh).
local function anchorIn(bx, by, bw, bh, align)
    local ah, av = splitAlign(align)
    local ax, ay
    if     ah == 4 then ax = bx + bw           -- RIGHT
    elseif ah == 2 then ax = bx + bw * 0.5     -- CENTER
    else                ax = bx end            -- LEFT (or unspecified)
    if     av == 32 then ay = by + bh          -- BOTTOM
    elseif av == 16 then ay = by + bh * 0.5    -- MIDDLE
    else                 ay = by end           -- TOP (or unspecified)
    return ax, ay
end

local function dimmed(color, factor)
    return { color[1], color[2], color[3], (color[4] or 1.0) * factor }
end

-- Multiply a color's alpha component by `mul`. Returns a fresh table —
-- never mutates the caller's color. Pass 1.0 to opt out (fast path
-- returns the original).
local function withAlpha(color, mul)
    if not color then return nil end
    if mul == 1.0 then return color end
    return { color[1], color[2], color[3], (color[4] or 1.0) * mul }
end

-- Scaled bounding box for a widget that scales around its own center.
-- Returns (x, y, w, h) of the on-screen rect. scale=1 fast-pathes.
local function scaledBbox(x, y, w, h, scale)
    if scale == 1.0 then return x, y, w, h end
    local sw, sh = w * scale, h * scale
    return x + (w - sw) * 0.5, y + (h - sh) * 0.5, sw, sh
end

local function isFocusable(w)
    return w and w.focusable and not w.disabled and w.visible ~= false
end

-- ---- Defaults (consumer-overridable on the module) ---------------------

M.DISABLED_ALPHA   = 0.5
M.HOVER_OVERLAY    = { 1, 1, 1, 0.15 }   -- additive tint over hovered widgets;
                                         -- independent of focus / pressed state
M.FOCUS_COLOR      = { 1, 1, 1, 0.75 }
M.FOCUS_BORDER_PX  = 1
M.FOCUS_REGION     = nil       -- if set, a 9-patch focus indicator
                               -- replaces the thin outline everywhere
M.DEFAULT_COLOR    = { 1, 1, 1, 1 }
M.SPATIAL_PERP_K   = 2.0   -- larger = punish lateral offset more in spatial nav

-- ---- 9-patch -----------------------------------------------------------

-- Draw a 9-patch from a region whose slice metadata was declared in
-- assets.lua (slice = { x1, x2, y1, y2 }). If the region carries no
-- slice, we fall back to a stretched single-region draw — better than
-- crashing; the caller's intent (fit (x, y, w, h)) is preserved.
function M.draw9patch(regionName, x, y, w, h)
    local x1, x2, y1, y2, sw, sh = regionSlice(regionName)
    if not (x1 and x2 and y1 and y2 and sw and sh) then
        drawRegion(regionName, x, y, { dstW = w, dstH = h })
        return
    end

    -- Source-pixel sizes for the 3×3 source grid
    local lw, rw = x1, sw - x2             -- L / R corner widths
    local th_, bh = y1, sh - y2            -- T / B corner heights
    local midSw  = x2 - x1                -- center source width
    local midSh  = y2 - y1                -- center source height

    -- Destination-pixel sizes for the stretched strips
    local midDw = w - lw - rw
    local midDh = h - th_ - bh
    if midDw < 0 then midDw = 0 end
    if midDh < 0 then midDh = 0 end

    -- A small helper for each tile: takes the slice's source rect and
    -- destination rect; srcX/srcY are region-local pixels.
    local function tile(srcX, srcY, srcW, srcH, dx, dy, dw, dh)
        if srcW <= 0 or srcH <= 0 or dw <= 0 or dh <= 0 then return end
        drawRegion(regionName, dx, dy, {
            srcX = srcX, srcY = srcY, srcW = srcW, srcH = srcH,
            dstW = dw,    dstH = dh,
        })
    end

    local row1Y = y
    local row2Y = y + th_
    local row3Y = y + th_ + midDh
    local col1X = x
    local col2X = x + lw
    local col3X = x + lw + midDw

    -- Top row
    tile(0,  0,  lw,     th_, col1X, row1Y, lw,     th_)
    tile(x1, 0,  midSw, th_, col2X, row1Y, midDw, th_)
    tile(x2, 0,  rw,     th_, col3X, row1Y, rw,     th_)
    -- Middle row
    tile(0,  y1, lw,     midSh, col1X, row2Y, lw,     midDh)
    tile(x1, y1, midSw, midSh, col2X, row2Y, midDw, midDh)
    tile(x2, y1, rw,     midSh, col3X, row2Y, rw,     midDh)
    -- Bottom row
    tile(0,  y2, lw,     bh, col1X, row3Y, lw,     bh)
    tile(x1, y2, midSw, bh, col2X, row3Y, midDw, bh)
    tile(x2, y2, rw,     bh, col3X, row3Y, rw,     bh)
end

-- N-px outline at `color`, drawn inside the bbox as four edge quads.
function M.drawOutline(x, y, w, h, thickness, color)
    local opts = { color = color }
    drawQuad(x,                   y,                   w,         thickness, opts)
    drawQuad(x,                   y + h - thickness,   w,         thickness, opts)
    drawQuad(x,                   y,                   thickness, h,         opts)
    drawQuad(x + w - thickness,   y,                   thickness, h,         opts)
end

-- Draw the focus indicator for a widget. If `region` (or M.FOCUS_REGION
-- as fallback) is set, render it as a 9-patch over the bbox — useful
-- for textured rings / glow effects. Otherwise fall back to a thin
-- outline using M.FOCUS_BORDER_PX + M.FOCUS_COLOR; if border thickness
-- is zero, nothing is drawn (silent opt-out).
function M.drawFocusIndicator(x, y, w, h, region)
    region = region or M.FOCUS_REGION
    if region then
        M.draw9patch(region, x, y, w, h)
    elseif M.FOCUS_BORDER_PX > 0 then
        M.drawOutline(x, y, w, h, M.FOCUS_BORDER_PX, M.FOCUS_COLOR)
    end
end

-- Legacy alias — kept so consumers that call drawFocusRing directly
-- keep working. New code should use drawFocusIndicator.
local function drawFocusRing(x, y, w, h)
    M.drawFocusIndicator(x, y, w, h)
end

M.rectContains   = rectContains
M.drawFocusRing = drawFocusRing

-- ---- Label -------------------------------------------------------------

function M.label(spec)
    local w = {
        x         = spec.x or 0,
        y         = spec.y or 0,
        width     = spec.width or 100,
        height    = spec.height or 20,
        disabled  = spec.disabled or false,
        visible   = spec.visible ~= false,
        focusable = false,                       -- labels never receive focus

        text       = spec.text or "",
        font       = spec.font,
        align      = spec.align or (1 + 8),       -- ALIGN_LEFT + ALIGN_TOP
        color      = spec.color or M.DEFAULT_COLOR,
        textScale = spec.textScale or 1.0,

        -- Animatable transform (see engine.animation). scale grows /
        -- shrinks the widget around its own center; alpha multiplies
        -- through every color the widget paints.
        alpha = spec.alpha or 1.0,
        scale = spec.scale or 1.0,
    }

    function w:draw()
        if not self.visible or self.alpha <= 0 then return end
        local sx, sy, sw, sh = scaledBbox(self.x, self.y, self.width,
                                           self.height, self.scale)
        local tx, ty = anchorIn(sx, sy, sw, sh, self.align)
        local color = self.disabled and dimmed(self.color, M.DISABLED_ALPHA)
                                    or self.color
        drawText(self.text, tx, ty, {
            font  = self.font,
            scale = self.textScale * self.scale,
            align = self.align,
            color = withAlpha(color, self.alpha),
        })
    end

    function w:hit() return false end
    function w:mousedown() return false end
    function w:mouseup()   return false end
    function w:mousemove() end
    function w:keydown()   return false end
    function w:keyup()     return false end

    return w
end

-- ---- Image -------------------------------------------------------------
--
-- A static, non-interactive textured rectangle. Useful when you want
-- the animation system to drive a logo, decoration, splash, or any
-- other piece of art that isn't itself a control.
--
-- spec fields:
--   x, y                anchor point in scene/panel-local coords
--   region              REQUIRED — named region from assets.lua
--   width, height       on-screen size; defaults to the region's native
--                       pixel size (queried via regionSize at construction)
--   align               anchor align mask (ALIGN_LEFT|CENTER|RIGHT +
--                       TOP|MIDDLE|BOTTOM); normalized to top-left for
--                       storage so subsequent animations move the
--                       top-left, not the original anchor
--   color               tint color (default white); animatable via tweenTo
--   flip                FLIP_H | FLIP_V bitmask
--   alpha, scale        animatable — same semantics as every other widget

function M.image(spec)
    local x, y = spec.x or 0, spec.y or 0
    local w_, h_ = spec.width, spec.height
    if spec.region and (not w_ or not h_) then
        local rw, rh = regionSize(spec.region)
        w_ = w_ or rw
        h_ = h_ or rh
    end
    w_ = w_ or 0
    h_ = h_ or 0

    -- Normalize anchor + align to top-left so the stored x/y match the
    -- bbox convention every other widget uses. Animations that tween x/y
    -- move the top-left from there on.
    if spec.align then
        local ah, av = splitAlign(spec.align)
        if     ah == 4 then x = x - w_           -- RIGHT
        elseif ah == 2 then x = x - w_ * 0.5     -- CENTER
        end
        if     av == 32 then y = y - h_          -- BOTTOM
        elseif av == 16 then y = y - h_ * 0.5    -- MIDDLE
        end
    end

    local w = {
        x         = x,
        y         = y,
        width     = w_,
        height    = h_,
        visible   = spec.visible ~= false,
        disabled  = false,
        focusable = false,                       -- images never receive focus

        region = spec.region,
        color  = spec.color or M.DEFAULT_COLOR,
        flip   = spec.flip,

        -- Optional draw-time clipping fractions in [0..1] — passed
        -- straight through to drawRegion. Useful for progress bars
        -- (timebar fill, etc.). nil = full image.
        fillX = spec.fillX,
        fillY = spec.fillY,

        -- Animatable transform (see engine.animation).
        alpha = spec.alpha or 1.0,
        scale = spec.scale or 1.0,
    }

    function w:draw()
        if not self.visible or self.alpha <= 0 or not self.region then return end
        local sx, sy, sw, sh = scaledBbox(self.x, self.y, self.width,
                                           self.height, self.scale)
        -- drawRegion's dstW/dstH overrides bypass its own fillX/y
        -- shrinkage — it still clips source UVs but draws at the
        -- override size, so a half-fill stretches to full width. Bake
        -- the fill into the dst here so both source and destination
        -- shrink in lockstep, matching drawRegion's natural behavior.
        local dw, dh = sw, sh
        if self.fillX then dw = sw * self.fillX end
        if self.fillY then dh = sh * self.fillY end
        drawRegion(self.region, sx, sy, {
            dstW  = dw, dstH = dh,
            color  = withAlpha(self.color, self.alpha),
            flip   = self.flip,
            fillX = self.fillX,
            fillY = self.fillY,
        })
    end

    function w:hit(px, py)
        return rectContains(self.x, self.y, self.width, self.height, px, py)
    end

    function w:mousedown() return false end
    function w:mouseup()   return false end
    function w:mousemove() end
    function w:keydown()   return false end
    function w:keyup()     return false end

    return w
end

-- ---- Button ------------------------------------------------------------
--
-- bgUp / bgDown / bgFocused / bgDisabled are region names (slice
-- metadata comes from assets.lua via regionSlice). All but bgUp are
-- optional — missing states fall back to bgUp.
--
-- bgColor = { up = {...}, down = {...}, focused = {...}, disabled = {...} }
-- is the flat-color background — useful before art exists or for plain
-- styling. Each state's color is optional; missing states fall back
-- through .up. When both a region and a color exist for the chosen
-- state, the color draws first and the 9-patch on top.
--
-- showFocusRing defaults to FALSE on buttons — the focused state
-- already shows visually via bgColor.focused or bgFocused. Set
-- showFocusRing = true on a per-button basis (or set the widget's
-- focusRegion to render a custom 9-patch focus indicator) if you
-- want the outline back.

function M.button(spec)
    local w = {
        x         = spec.x or 0,
        y         = spec.y or 0,
        width     = spec.width or 120,
        height    = spec.height or 32,
        disabled  = spec.disabled or false,
        visible   = spec.visible ~= false,
        focused   = false,
        focusable = true,

        text       = spec.text or "",
        font       = spec.font,
        textAlign = spec.textAlign or (2 + 16),       -- CENTER + MIDDLE
        textColor = spec.textColor or M.DEFAULT_COLOR,
        textScale = spec.textScale or 1.0,

        bgUp       = spec.bgUp,
        bgDown     = spec.bgDown,
        bgHover    = spec.bgHover,
        bgFocused  = spec.bgFocused,
        bgDisabled = spec.bgDisabled,
        bgColor    = spec.bgColor,                    -- optional state map

        icon         = spec.icon,
        iconAlign   = spec.iconAlign or (1 + 16),     -- LEFT + MIDDLE
        iconPadding = spec.iconPadding or { l = 0, r = 0, t = 0, b = 0 },

        onClick = spec.onClick,

        showFocusRing = spec.showFocusRing == true, -- default OFF
        focusRegion    = spec.focusRegion,             -- per-button 9-patch

        hoverColor = spec.hoverColor or M.HOVER_OVERLAY,

        -- forceDown: external override that pins the visual state to
        -- "down" even when the mouse isn't pressed. Useful when a
        -- timer-driven press flash needs to outlive the user's actual
        -- click (Find5's joker button, e.g.).
        forceDown = spec.forceDown or false,

        -- Animatable transform (see engine.animation). scale grows /
        -- shrinks the button around its center; alpha multiplies into
        -- every color the button paints.
        alpha = spec.alpha or 1.0,
        scale = spec.scale or 1.0,

        _pressed = false,
        _hover   = false,
    }

    -- Pick state name (disabled / down / hover / focused / up). Used to
    -- index both the region map and the color map. Hover only enters
    -- the resolution when an explicit bgHover region or hover entry
    -- in the color map exists; otherwise the hoverColor overlay
    -- (drawn separately) is the hover affordance. forceDown lets a
    -- caller pin the visual to "down" independent of the mouse — sync
    -- to it each frame; the widget won't clear it for you.
    local function stateName(self)
        if self.disabled then return "disabled" end
        if self._pressed or self.forceDown then return "down" end
        if self._hover and (self.bgHover
                            or (self.bgColor and self.bgColor.hover))
            then return "hover" end
        if self.focused  then return "focused"  end
        return "up"
    end

    -- Pick a value from a state-keyed map with fallback chain
    -- (down -> up, hover -> up, focused -> up, disabled -> up). Returns
    -- nil if no entry exists for any of the fallbacks.
    local function pickState(map, state)
        if not map then return nil end
        return map[state] or map.up
    end

    -- Pick the 9-patch region name for the current state, falling
    -- back to bgUp. Returns the chosen region + whether to dim it
    -- (true only when bgUp is being used as the disabled fallback).
    local function pickRegion(self, state)
        if state == "disabled" then
            if self.bgDisabled then return self.bgDisabled, false end
            return self.bgUp, (self.bgUp ~= nil)        -- dim bgUp
        elseif state == "down" then
            return self.bgDown or self.bgUp, false
        elseif state == "hover" then
            return self.bgHover or self.bgUp, false
        elseif state == "focused" then
            return self.bgFocused or self.bgUp, false
        else
            return self.bgUp, false
        end
    end

    function w:draw()
        if not self.visible or self.alpha <= 0 then return end

        local sx, sy, sw, sh = scaledBbox(self.x, self.y, self.width,
                                           self.height, self.scale)
        local a     = self.alpha
        local state = stateName(self)

        -- Flat-color background (drawn first if present).
        local color = pickState(self.bgColor, state)
        if color then
            drawQuad(sx, sy, sw, sh, { color = withAlpha(color, a) })
        end

        -- 9-patch background (drawn over the color fill if present).
        local bg, dimBg = pickRegion(self, state)
        if bg then
            M.draw9patch(bg, sx, sy, sw, sh)
            if dimBg then
                drawQuad(sx, sy, sw, sh,
                          { color = { 0, 0, 0, (1 - M.DISABLED_ALPHA) * a } })
            end
        end

        -- Hover overlay — fallback tint for buttons without dedicated
        -- bgHover art. Skip when state is already "hover" (region
        -- handled it), or "down" (pressed art shouldn't get a hover
        -- tint on top), or "disabled".
        if self._hover and (state == "up" or state == "focused") then
            drawQuad(sx, sy, sw, sh,
                      { color = withAlpha(self.hoverColor, a) })
        end

        -- Icon (optional). Anchored within the SCALED bbox by iconAlign,
        -- inset from the matching edges by iconPadding. The opposite
        -- edges' padding is ignored (TOP padding shifts down the icon
        -- when align is TOP; ignored when align is BOTTOM). The icon
        -- glyph itself also gets scaleX/y so it grows with the bbox.
        if self.icon then
            local ah, av = splitAlign(self.iconAlign)
            local pad = self.iconPadding
            local ix
            if     ah == 4 then ix = sx + sw - (pad.r or 0)
            elseif ah == 2 then ix = sx + sw * 0.5
            else                ix = sx + (pad.l or 0) end
            local iy
            if     av == 32 then iy = sy + sh - (pad.b or 0)
            elseif av == 16 then iy = sy + sh * 0.5
            else                 iy = sy + (pad.t or 0) end
            local c = self.textColor
            if self.disabled then c = dimmed(c, M.DISABLED_ALPHA) end
            drawRegion(self.icon, ix, iy, {
                align   = self.iconAlign,
                color   = withAlpha(c, a),
                scaleX = self.scale, scaleY = self.scale,
            })
        end

        -- Text (optional — buttons can be icon-only).
        if self.text and self.text ~= "" then
            local tx, ty = anchorIn(sx, sy, sw, sh, self.textAlign)
            local c = self.disabled and dimmed(self.textColor, M.DISABLED_ALPHA)
                                    or self.textColor
            drawText(self.text, tx, ty, {
                font  = self.font,
                scale = self.textScale * self.scale,
                align = self.textAlign,
                color = withAlpha(c, a),
            })
        end

        -- Buttons default to showFocusRing=false because their focused
        -- state already shows via bgColor.focused / bgFocused.
        if self.focused and self.showFocusRing then
            M.drawFocusIndicator(sx, sy, sw, sh, self.focusRegion)
        end
    end

    function w:hit(px, py)
        return rectContains(self.x, self.y, self.width, self.height, px, py)
    end

    function w:mousedown(px, py, button)
        if self.disabled or button ~= 1 or not self:hit(px, py) then
            return false
        end
        self._pressed = true
        return true
    end

    function w:mouseup(px, py, button)
        local wasPressed = self._pressed
        self._pressed = false
        if self.disabled or button ~= 1 then return false end
        if wasPressed and self:hit(px, py) then
            if self.onClick then self:onClick() end
            return true
        end
        return false
    end

    function w:mousemove(px, py)
        if self.disabled then self._hover = false; return end
        self._hover = self:hit(px, py)
    end

    function w:keydown(name)
        if self.disabled or not self.focused then return false end
        if name == "return" or name == "space" then
            if self.onClick then self:onClick() end
            return true
        end
        return false
    end

    function w:keyup() return false end

    return w
end

-- ---- Checkbox ----------------------------------------------------------
--
-- Fixed layout: the box sits ALIGN_LEFT + ALIGN_MIDDLE inside the
-- widget bbox; the label follows after `boxGap` pixels. The box's
-- on-screen width is read from the unchecked region's native size via
-- regionSize(). Click anywhere in the bbox toggles the value.
--
-- spec fields:
--   x, y, width, height
--   text                    (label drawn after the box)
--   font, color, scale      (label font + style)
--   boxUnchecked           region name (REQUIRED for the box to render)
--   boxChecked             region name (REQUIRED — same source size as
--                                       boxUnchecked is recommended)
--   boxUncheckedDisabled  optional dimmed-state region
--   boxCheckedDisabled    optional dimmed-state region
--   boxGap                 px between box and label (default 6)
--   value                   initial bool (default false)
--   onChange               function(self, new_value)
--
-- Without box regions the widget falls back to a 16-px outlined square
-- — handy during asset bring-up; replace with real art via the box_*
-- fields once it exists.

function M.checkbox(spec)
    local w = {
        x         = spec.x or 0,
        y         = spec.y or 0,
        width     = spec.width or 100,
        height    = spec.height or 20,
        disabled  = spec.disabled or false,
        visible   = spec.visible ~= false,
        focused   = false,
        focusable = true,

        text       = spec.text or "",
        font       = spec.font,
        color      = spec.color or M.DEFAULT_COLOR,
        textScale = spec.textScale or 1.0,

        boxUnchecked          = spec.boxUnchecked,
        boxChecked            = spec.boxChecked,
        boxUncheckedDisabled = spec.boxUncheckedDisabled,
        boxCheckedDisabled   = spec.boxCheckedDisabled,
        boxGap                = spec.boxGap or 6,

        value     = spec.value or false,
        onChange = spec.onChange,

        showFocusRing = spec.showFocusRing ~= false,   -- default ON
        focusRegion    = spec.focusRegion,

        -- Animatable transform (see engine.animation).
        alpha = spec.alpha or 1.0,
        scale = spec.scale or 1.0,

        _box_w    = 0,
        _box_h    = 0,
        _pressed  = false,
    }

    -- Resolve box source size at construction. Both states are
    -- expected to share width; if not, the unchecked region wins (it's
    -- what we measure against for the text offset).
    if w.boxUnchecked then
        local bw, bh = regionSize(w.boxUnchecked)
        w._box_w = bw or 0
        w._box_h = bh or 0
    end
    -- Fallback when no region: 16-px square so the widget is still
    -- visible during bring-up.
    if w._box_w == 0 then w._box_w = 16; w._box_h = 16 end

    local function pickBox(self)
        if self.value then
            return (self.disabled and self.boxCheckedDisabled) or self.boxChecked
        else
            return (self.disabled and self.boxUncheckedDisabled) or self.boxUnchecked
        end
    end

    function w:draw()
        if not self.visible or self.alpha <= 0 then return end

        local sx, sy, sw, sh = scaledBbox(self.x, self.y, self.width,
                                           self.height, self.scale)
        local a     = self.alpha
        local s     = self.scale
        local boxW = self._box_w * s
        local boxH = self._box_h * s
        local midY = sy + sh * 0.5

        local region = pickBox(self)
        if region then
            drawRegion(region, sx, midY, {
                align   = 1 + 16,                -- ALIGN_LEFT + ALIGN_MIDDLE
                color   = withAlpha(M.DEFAULT_COLOR, a),
                scaleX = s, scaleY = s,
            })
        else
            -- No region — render a flat outlined square at _box_w size.
            local boxY = midY - boxH * 0.5
            if self.value then
                drawQuad(sx, boxY, boxW, boxH,
                          { color = withAlpha({ 0.55, 0.55, 0.85, 1.0 }, a) })
            end
            M.drawOutline(sx, boxY, boxW, boxH,
                           1, withAlpha({ 0.9, 0.9, 0.95, 0.8 }, a))
        end

        if self.text and self.text ~= "" then
            local tx = sx + boxW + self.boxGap * s
            local color = self.disabled and dimmed(self.color, M.DISABLED_ALPHA)
                                        or self.color
            drawText(self.text, tx, midY, {
                font  = self.font,
                scale = self.textScale * s,
                align = 1 + 16,                  -- ALIGN_LEFT + ALIGN_MIDDLE
                color = withAlpha(color, a),
            })
        end

        if self.focused and self.showFocusRing then
            M.drawFocusIndicator(sx, sy, sw, sh, self.focusRegion)
        end
    end

    function w:hit(px, py)
        return rectContains(self.x, self.y, self.width, self.height, px, py)
    end

    local function toggle(self)
        self.value = not self.value
        if self.onChange then self:onChange(self.value) end
    end

    function w:mousedown(px, py, button)
        if self.disabled or button ~= 1 or not self:hit(px, py) then return false end
        self._pressed = true
        return true
    end

    function w:mouseup(px, py, button)
        local was = self._pressed
        self._pressed = false
        if self.disabled or button ~= 1 then return false end
        if was and self:hit(px, py) then
            toggle(self)
            return true
        end
        return false
    end

    function w:mousemove() end

    function w:keydown(name)
        if self.disabled or not self.focused then return false end
        if name == "space" or name == "return" then
            toggle(self)
            return true
        end
        return false
    end

    function w:keyup() return false end

    return w
end

-- ---- Slider ------------------------------------------------------------
--
-- Horizontal or vertical drag-to-set value picker. Track is a 9-patch
-- (optional — falls back to a flat-color quad); knob is a single region
-- (also optional — flat-color fallback). Mouse drag on track or knob
-- updates value continuously. Keyboard: left/right (horizontal) or
-- up/down (vertical) adjust by `step`, or by 5% of (max - min) when
-- step == 0.
--
-- spec fields:
--   x, y, width, height
--   trackBg          region name (9-patch); optional
--   knob              region name (sprite);  optional
--   knobW, knobH    explicit knob size; defaults to knob region's
--                     native size, or 16 if no knob region
--   min, max          numeric range (default 0..1)
--   value             current value (default min)
--   step              snap step; 0 (default) = continuous
--   orientation       "horizontal" (default) or "vertical"
--   onChange         function(self, new_value); fires while dragging

function M.slider(spec)
    local w = {
        x         = spec.x or 0,
        y         = spec.y or 0,
        width     = spec.width or 200,
        height    = spec.height or 24,
        disabled  = spec.disabled or false,
        visible   = spec.visible ~= false,
        focused   = false,
        focusable = true,

        trackBg = spec.trackBg,
        knob     = spec.knob,
        knobW   = spec.knobW,
        knobH   = spec.knobH,

        min   = spec.min or 0.0,
        max   = spec.max or 1.0,
        value = spec.value or spec.min or 0.0,
        step  = spec.step or 0.0,

        orientation = spec.orientation or "horizontal",
        onChange   = spec.onChange,

        showFocusRing = spec.showFocusRing ~= false,   -- default ON
        focusRegion    = spec.focusRegion,

        -- Animatable transform (see engine.animation).
        alpha = spec.alpha or 1.0,
        scale = spec.scale or 1.0,

        _dragging = false,
    }

    -- Knob source size: explicit override, else region's native size,
    -- else 16-px fallback.
    if w.knob and (not w.knobW or not w.knobH) then
        local kw, kh = regionSize(w.knob)
        w.knobW = w.knobW or kw
        w.knobH = w.knobH or kh
    end
    w.knobW = w.knobW or 16
    w.knobH = w.knobH or 16

    local function clampSnap(self, v)
        if v < self.min then v = self.min end
        if v > self.max then v = self.max end
        if self.step and self.step > 0 then
            v = math.floor((v - self.min) / self.step + 0.5) * self.step + self.min
            if v > self.max then v = self.max end
        end
        return v
    end

    -- Convert mouse position to a value along the slider's long axis.
    -- Centered on the knob (the value 0 puts the knob's center at the
    -- start; value max puts it at end) — knobW / knobH reduce the
    -- effective travel.
    local function posToValue(self, px, py)
        local pct
        if self.orientation == "vertical" then
            local travel = self.height - self.knobH
            pct = travel > 0 and (py - self.y - self.knobH * 0.5) / travel or 0
        else
            local travel = self.width - self.knobW
            pct = travel > 0 and (px - self.x - self.knobW * 0.5) / travel or 0
        end
        if pct < 0 then pct = 0 end
        if pct > 1 then pct = 1 end
        return clampSnap(self, self.min + pct * (self.max - self.min))
    end

    local function setValue(self, v)
        v = clampSnap(self, v)
        if v ~= self.value then
            self.value = v
            if self.onChange then self:onChange(v) end
        end
    end

    function w:draw()
        if not self.visible or self.alpha <= 0 then return end

        local sx, sy, sw, sh = scaledBbox(self.x, self.y, self.width,
                                           self.height, self.scale)
        local a  = self.alpha
        local s  = self.scale
        local kw = self.knobW * s
        local kh = self.knobH * s

        -- Track
        if self.trackBg then
            M.draw9patch(self.trackBg, sx, sy, sw, sh)
        else
            drawQuad(sx, sy, sw, sh,
                      { color = withAlpha({ 0.18, 0.18, 0.22, 0.85 }, a) })
        end

        -- Knob position
        local pct = 0
        if self.max > self.min then
            pct = (self.value - self.min) / (self.max - self.min)
            if pct < 0 then pct = 0 end
            if pct > 1 then pct = 1 end
        end
        local kx, ky
        if self.orientation == "vertical" then
            kx = sx + (sw - kw) * 0.5
            ky = sy + pct * (sh - kh)
        else
            kx = sx + pct * (sw - kw)
            ky = sy + (sh - kh) * 0.5
        end

        if self.knob then
            drawRegion(self.knob, kx, ky, {
                dstW = kw, dstH = kh,
                color = withAlpha(M.DEFAULT_COLOR, a),
            })
        else
            drawQuad(kx, ky, kw, kh,
                      { color = withAlpha({ 0.75, 0.75, 0.85, 1.0 }, a) })
        end

        if self.disabled then
            drawQuad(sx, sy, sw, sh,
                      { color = { 0, 0, 0, (1 - M.DISABLED_ALPHA) * a } })
        end

        if self.focused and self.showFocusRing then
            M.drawFocusIndicator(sx, sy, sw, sh, self.focusRegion)
        end
    end

    function w:hit(px, py)
        return rectContains(self.x, self.y, self.width, self.height, px, py)
    end

    function w:mousedown(px, py, button)
        if self.disabled or button ~= 1 or not self:hit(px, py) then return false end
        self._dragging = true
        setValue(self, posToValue(self, px, py))
        return true
    end

    function w:mouseup(px, py, button)
        if button ~= 1 then return false end
        local was = self._dragging
        self._dragging = false
        return was
    end

    function w:mousemove(px, py)
        if self._dragging then
            setValue(self, posToValue(self, px, py))
        end
    end

    function w:keydown(name)
        if self.disabled or not self.focused then return false end
        local sign
        if self.orientation == "vertical" then
            if     name == "up"   then sign = -1
            elseif name == "down" then sign =  1 end
        else
            if     name == "left"  then sign = -1
            elseif name == "right" then sign =  1 end
        end
        if not sign then return false end
        local delta = (self.step and self.step > 0) and self.step
                                                    or (self.max - self.min) * 0.05
        setValue(self, self.value + sign * delta)
        return true
    end

    function w:keyup() return false end

    return w
end

-- ---- LineEdit ----------------------------------------------------------
--
-- Single-line text input. State: text (string), cursor (int, 0..#text),
-- _blink_t (cursor blink phase in seconds).
--
-- spec fields:
--   x, y, width, height
--   text                  initial value (default "")
--   font, color, scale    text styling
--   bgEnabled            region name (9-patch); optional
--   bgDisabled           region name (9-patch); optional
--   bgColor = { enabled = {...}, disabled = {...} }  optional flat fills
--   padding = { l, r, t, b }    text inset from bbox (default {6,6,0,0})
--   maxLength            char cap (nil = unlimited)
--   placeholder           text shown when value is empty and not focused
--   placeholderColor     color for the placeholder text
--   onChange             function(self, text)  fires after every edit
--   onSubmit             function(self, text)  fires on Return
--
-- Cursor blink runs at 2 Hz (visible for the first 0.25 s of each
-- 0.5 s cycle). The widget exposes :update(dt) for the host scene to
-- tick the blink phase; widget.dispatchUpdate(widgets, dt) ticks
-- every widget in a list.
--
-- Convention (per scripts/engine/scene.lua's onTextinput hook):
-- navigation keys (Left/Right/Home/End/Backspace/Delete/Return)
-- handle in keydown; character insertion is in textinput.

local function lePad(specPad)
    local p = specPad or {}
    return {
        l = p.l or 6, r = p.r or 6,
        t = p.t or 0, b = p.b or 0,
    }
end

function M.lineEdit(spec)
    local w = {
        x         = spec.x or 0,
        y         = spec.y or 0,
        width     = spec.width or 200,
        height    = spec.height or 28,
        disabled  = spec.disabled or false,
        visible   = spec.visible ~= false,
        focused   = false,
        focusable = true,

        text       = spec.text or "",
        font       = spec.font,
        color      = spec.color or M.DEFAULT_COLOR,
        textScale = spec.textScale or 1.0,

        bgEnabled  = spec.bgEnabled,
        bgDisabled = spec.bgDisabled,
        bgColor    = spec.bgColor,

        padding           = lePad(spec.padding),
        maxLength        = spec.maxLength,
        placeholder       = spec.placeholder or "",
        placeholderColor = spec.placeholderColor or { 0.6, 0.6, 0.65, 0.8 },

        onChange = spec.onChange,
        onSubmit = spec.onSubmit,

        showFocusRing = spec.showFocusRing ~= false,   -- default ON
        focusRegion    = spec.focusRegion,

        -- Animatable transform (see engine.animation). NB: hit-testing
        -- and cursorFromX measure the LOGICAL (un-scaled) bbox so a
        -- click during a scale animation still maps to a sensible
        -- character index. Visual rendering uses the scaled bbox.
        alpha = spec.alpha or 1.0,
        scale = spec.scale or 1.0,

        cursor   = #(spec.text or ""),
        _blink_t = 0,
    }

    -- Inner content rect (after padding).
    local function inner(self)
        local p = self.padding
        return self.x + p.l,
               self.y + p.t,
               self.width  - p.l - p.r,
               self.height - p.t - p.b
    end

    -- 0..#text — the closest cursor index for a click at virtual x.
    -- Linear scan over prefix widths; O(N) per click is fine at typical
    -- input lengths. Measured against the LOGICAL text scale (the
    -- textScale field) ignoring the widget-transform scale.
    local function cursorFromX(self, clickX)
        local textX = self.x + self.padding.l
        local target = clickX - textX
        if target <= 0 then return 0 end
        local bestPos  = 0
        local bestDiff = math.huge
        for i = 0, #self.text do
            local w_ = textWidth(self.text:sub(1, i), self.textScale, self.font)
            local diff = w_ - target
            if diff < 0 then diff = -diff end
            if diff < bestDiff then bestPos, bestDiff = i, diff end
        end
        return bestPos
    end

    local function cursorIsVisible(self)
        if not self.focused or self.disabled then return false end
        return (self._blink_t % 0.5) < 0.25
    end

    local function emitChange(self)
        if self.onChange then self:onChange(self.text) end
    end

    function w:update(dt)
        if self.focused and not self.disabled then
            self._blink_t = self._blink_t + dt
        else
            self._blink_t = 0
        end
    end

    function w:draw()
        if not self.visible or self.alpha <= 0 then return end

        local sx, sy, sw, sh = scaledBbox(self.x, self.y, self.width,
                                           self.height, self.scale)
        local a   = self.alpha
        local s   = self.scale
        local pad = self.padding
        local ix  = sx + pad.l * s
        local iy  = sy + pad.t * s
        local iw  = sw - (pad.l + pad.r) * s
        local ih  = sh - (pad.t + pad.b) * s
        local midY = sy + sh * 0.5

        -- Flat background fill.
        local state = self.disabled and "disabled" or "enabled"
        if self.bgColor then
            local c = self.bgColor[state] or self.bgColor.enabled
            if c then
                drawQuad(sx, sy, sw, sh, { color = withAlpha(c, a) })
            end
        end

        -- 9-patch on top of the fill.
        local bg = self.disabled and (self.bgDisabled or self.bgEnabled)
                                 or self.bgEnabled
        if bg then
            M.draw9patch(bg, sx, sy, sw, sh)
        end

        -- Text or placeholder.
        local shown = self.text
        local color
        if shown == "" and not self.focused and self.placeholder ~= "" then
            shown = self.placeholder
            color = self.placeholderColor
        else
            color = self.disabled and dimmed(self.color, M.DISABLED_ALPHA)
                                  or self.color
        end
        if shown ~= "" then
            drawText(shown, ix, midY, {
                font  = self.font,
                scale = self.textScale * s,
                align = 1 + 16,                  -- ALIGN_LEFT + ALIGN_MIDDLE
                color = withAlpha(color, a),
            })
        end

        -- Blinking cursor at the prefix width.
        if cursorIsVisible(self) then
            local prefixW = textWidth(self.text:sub(1, self.cursor),
                                        self.textScale * s, self.font)
            local cx = ix + prefixW
            -- Clamp to inner rect so an over-long string doesn't paint
            -- the cursor off-screen indefinitely.
            if cx > ix + iw then cx = ix + iw end
            drawQuad(cx, iy, 1, ih, { color = withAlpha(self.color, a) })
        end

        if self.focused and self.showFocusRing then
            M.drawFocusIndicator(sx, sy, sw, sh, self.focusRegion)
        end
    end

    function w:hit(px, py)
        return rectContains(self.x, self.y, self.width, self.height, px, py)
    end

    function w:mousedown(px, py, button)
        if self.disabled or button ~= 1 or not self:hit(px, py) then return false end
        self.cursor = cursorFromX(self, px)
        self._blink_t = 0   -- re-show cursor immediately on click
        return true
    end

    function w:mouseup() return false end
    function w:mousemove() end

    function w:textinput(ch)
        if self.disabled or not self.focused then return end
        if not ch or ch == "" then return end
        if self.maxLength and #self.text >= self.maxLength then return end
        local before = self.text:sub(1, self.cursor)
        local after  = self.text:sub(self.cursor + 1)
        self.text   = before .. ch .. after
        self.cursor = self.cursor + #ch
        self._blink_t = 0
        emitChange(self)
    end

    function w:keydown(name)
        if self.disabled or not self.focused then return false end

        if name == "backspace" then
            if self.cursor > 0 then
                self.text   = self.text:sub(1, self.cursor - 1)
                            .. self.text:sub(self.cursor + 1)
                self.cursor = self.cursor - 1
                self._blink_t = 0
                emitChange(self)
            end
            return true
        end

        if name == "delete" then
            if self.cursor < #self.text then
                self.text = self.text:sub(1, self.cursor)
                          .. self.text:sub(self.cursor + 2)
                self._blink_t = 0
                emitChange(self)
            end
            return true
        end

        if name == "left" then
            if self.cursor > 0 then self.cursor = self.cursor - 1 end
            self._blink_t = 0
            return true
        end
        if name == "right" then
            if self.cursor < #self.text then self.cursor = self.cursor + 1 end
            self._blink_t = 0
            return true
        end
        if name == "home" then self.cursor = 0; self._blink_t = 0; return true end
        if name == "end"  then self.cursor = #self.text; self._blink_t = 0; return true end

        if name == "return" then
            if self.onSubmit then self:onSubmit(self.text) end
            return true
        end

        return false   -- Tab / Esc / other keys fall through to scene
    end

    function w:keyup() return false end

    return w
end

-- ---- Panel -------------------------------------------------------------
--
-- Container widget that owns a flat list of children laid out in its
-- own local coordinate space: the panel's (x, y) is the origin and
-- children's (x, y) are offsets from it. A host scene's per-frame
-- boilerplate collapses to "build panel, point scene methods at it".
--
-- spec fields:
--   x, y, width, height     bbox in the parent's coord space
--   visible                 default true
--
-- Methods:
--   :add(child)             append a child; if it's the first focusable
--                           child, it claims focus
--   :draw                   offsets children by (x, y) then delegates
--   :hit(px, py)            panel's own bbox (used when nested)
--   :mousedown/up/move      translate to local coords, fan out;
--                           mousedown also runs click-to-focus
--   :update / :keydown      fan out; keydown uses dispatchKeydown
--                           over children (focused-child first, then
--                           Tab / spatial nav within this panel)
--
-- Nesting: a child may itself be a panel. Mouse events keep translating
-- as they descend, draw recurses. Keyboard focus is owned per-panel:
-- when a click lands inside a child panel, this panel's focusedChild
-- points at that child panel so keydown forwards into it.
--
-- alpha multiplies into every child's alpha during draw, so a fade on
-- the panel cascades through nested panels and leaf widgets uniformly.
-- scale on a panel is currently ignored (group-scale needs GL matrix
-- push/pop, which isn't bound from Lua yet) — animate children
-- individually for now.
function M.panel(spec)
    spec = spec or {}
    local p = {
        x         = spec.x or 0,
        y         = spec.y or 0,
        width     = spec.width  or 0,
        height    = spec.height or 0,
        visible   = spec.visible ~= false,
        focusable = false,
        focused   = false,
        disabled  = false,

        alpha = spec.alpha or 1.0,
        scale = spec.scale or 1.0,    -- accepted but no-op for v1

        isPanel      = true,
        children      = {},
        focusedChild = nil,
    }

    function p:add(child)
        table.insert(self.children, child)
        if not self.focusedChild and isFocusable(child) then
            self.focusedChild = child
            child.focused = true
        end
        return child
    end

    function p:draw()
        if not self.visible or self.alpha <= 0 then return end
        for _, c in ipairs(self.children) do
            if c.visible ~= false and c.draw then
                local lx, ly = c.x, c.y
                local la     = c.alpha or 1.0
                c.x = self.x + lx
                c.y = self.y + ly
                c.alpha = la * self.alpha
                c:draw()
                c.x, c.y = lx, ly
                c.alpha = la
            end
        end
    end

    function p:hit(px, py)
        return rectContains(self.x, self.y, self.width, self.height, px, py)
    end

    -- Click-to-focus walks children in reverse so the topmost-drawn
    -- child wins overlap. Either claims focus directly (focusable leaf)
    -- or redirects this panel's focus into a hit child panel.
    local function claimFocus(self, c)
        if self.focusedChild and self.focusedChild ~= c then
            self.focusedChild.focused = false
        end
        self.focusedChild = c
    end

    function p:mousedown(px, py, button)
        if not self.visible then return end
        local lx, ly = px - self.x, py - self.y
        if button == 1 then
            for i = #self.children, 1, -1 do
                local c = self.children[i]
                if c.visible ~= false and c.hit and c:hit(lx, ly) then
                    if c.focusable and not c.disabled then
                        claimFocus(self, c)
                        c.focused = true
                        break
                    elseif c.isPanel then
                        claimFocus(self, c)
                        break
                    end
                end
            end
        end
        for _, c in ipairs(self.children) do
            if c.visible ~= false and c.mousedown then
                c:mousedown(lx, ly, button)
            end
        end
    end

    function p:mouseup(px, py, button)
        if not self.visible then return end
        local lx, ly = px - self.x, py - self.y
        for _, c in ipairs(self.children) do
            if c.visible ~= false and c.mouseup then
                c:mouseup(lx, ly, button)
            end
        end
    end

    function p:mousemove(px, py, dx, dy)
        if not self.visible then return end
        local lx, ly = px - self.x, py - self.y
        for _, c in ipairs(self.children) do
            if c.visible ~= false and c.mousemove then
                c:mousemove(lx, ly, dx, dy)
            end
        end
    end

    function p:update(dt)
        if not self.visible then return end
        for _, c in ipairs(self.children) do
            anim.tickAction(c, dt)
            if c.update then c:update(dt) end
        end
    end

    function p:keydown(name)
        if not self.visible or self.disabled then return false end
        local nextFocus = M.dispatchKeydown(
            self.children, self.focusedChild, name)
        if nextFocus then self.focusedChild = nextFocus end
        return false
    end

    function p:keyup() return false end

    -- Only the focused leaf cares about textinput. Fan-out doesn't make
    -- sense here (a single character would land in every lineEdit).
    function p:textinput(ch)
        if not self.visible then return end
        local fc = self.focusedChild
        if fc and fc.textinput then fc:textinput(ch) end
    end

    return p
end

-- ---- Update dispatch ---------------------------------------------------
--
-- Per-frame tick for widgets that need it (LineEdit's cursor blink).
-- Most widgets don't define :update; the helper just skips them.
function M.dispatchUpdate(widgets, dt)
    for _, w in ipairs(widgets) do
        anim.tickAction(w, dt)
        if w.update then w:update(dt) end
    end
end

-- ---- Focus management --------------------------------------------------
--
-- The host scene owns the focused widget reference. These helpers walk
-- a list of widgets and return the next focus target (or nil if none
-- exists in that direction). Callers do:
--
--   self.focused_widget.focused = false
--   self.focused_widget = widget.focusNext(self.widgets, self.focused_widget)
--                         or self.focused_widget
--   self.focused_widget.focused = true

-- Linear Tab navigation — wraps. Returns the next focusable widget or
-- nil if none exist at all. Pass nil for `current` to get the first
-- focusable widget.
function M.focusNext(widgets, current)
    if #widgets == 0 then return nil end
    local start
    if current then
        for i, w in ipairs(widgets) do if w == current then start = i; break end end
    end
    start = start or 0
    for k = 1, #widgets do
        local i = ((start - 1 + k) % #widgets) + 1
        if isFocusable(widgets[i]) and widgets[i] ~= current then
            return widgets[i]
        end
    end
    -- Only one focusable widget — return it (or current if current is it).
    if isFocusable(current) then return current end
    return nil
end

function M.focusPrev(widgets, current)
    if #widgets == 0 then return nil end
    local start
    if current then
        for i, w in ipairs(widgets) do if w == current then start = i; break end end
    end
    start = start or (#widgets + 1)
    for k = 1, #widgets do
        local i = ((start - 1 - k) % #widgets) + 1
        if isFocusable(widgets[i]) and widgets[i] ~= current then
            return widgets[i]
        end
    end
    if isFocusable(current) then return current end
    return nil
end

-- Spatial navigation — finds the nearest focusable widget in the given
-- direction relative to `current`'s center. dir = "up" / "down" /
-- "left" / "right". Returns nil if no candidate exists in that
-- direction (no wrap-around; spatial nav is the cursor-key job, Tab is
-- the wrap-around job).
--
-- Scoring: each candidate gets `axial + K * perp` where axial is its
-- distance along the direction and perp is its perpendicular offset
-- from `current`. K (M.SPATIAL_PERP_K) controls how much the
-- heuristic prefers candidates that align with `current` over ones
-- that are merely closer but off to the side. Lowest score wins.
function M.focusDirection(widgets, current, dir)
    if not current then
        -- No current focus — fall back to the first focusable widget.
        return M.focusNext(widgets, nil)
    end
    local cx = current.x + current.width  * 0.5
    local cy = current.y + current.height * 0.5

    -- Direction vector: (axial unit) and a perp extractor.
    local axialX, axialY = 0, 0
    if     dir == "up"    then axialY = -1
    elseif dir == "down"  then axialY =  1
    elseif dir == "left"  then axialX = -1
    elseif dir == "right" then axialX =  1
    else return nil end

    local best, bestScore
    for _, w in ipairs(widgets) do
        if isFocusable(w) and w ~= current then
            local wx = w.x + w.width  * 0.5
            local wy = w.y + w.height * 0.5
            local dx = wx - cx
            local dy = wy - cy

            local axial = dx * axialX + dy * axialY
            if axial > 0 then
                -- perpendicular magnitude is just the other axis here
                local perp
                if axialX ~= 0 then perp = dy < 0 and -dy or dy
                else                 perp = dx < 0 and -dx or dx end
                local score = axial + M.SPATIAL_PERP_K * perp
                if not bestScore or score < bestScore then
                    best, bestScore = w, score
                end
            end
        end
    end
    return best
end

-- One-stop dispatch: forward `name` to the focused widget; if the
-- widget didn't consume it, try Tab / cursor-key navigation. Returns
-- the (possibly-updated) focused widget. Pattern:
--
--   self.focused_widget = widget.dispatchKeydown(self.widgets,
--                                                 self.focused_widget, name)
function M.dispatchKeydown(widgets, current, name)
    if current and current.keydown and current:keydown(name) then
        return current
    end
    local nextW
    if name == "tab" then
        nextW = M.focusNext(widgets, current)
    elseif name == "up" or name == "down" or name == "left" or name == "right" then
        nextW = M.focusDirection(widgets, current, name)
    end
    if nextW and nextW ~= current then
        if current then current.focused = false end
        nextW.focused = true
        return nextW
    end
    return current
end

return M
