-- engine/widget.lua — declarative UI widgets on top of the engine bindings.
--
-- A widget is a plain Lua table with x / y / width / height + a few
-- methods (draw / hit / mousedown / mouseup / mousemove / keydown /
-- keyup). Constructors live in this module:
--
--   widget.label(spec)    -- text in a bbox
--   widget.button(spec)   -- 3-state 9-patch bg + optional icon + text
--
-- A host scene keeps a list of widgets and dispatches its scene-hook
-- callbacks through them — this module stays unaware of engine.scene
-- so widgets can be used inside any rendering context.
--
-- Spatial cursor navigation: when the focused widget's `keydown(name)`
-- returns false for "up" / "down" / "left" / "right", the scene calls
-- widget.focus_direction(widgets, current, name) to move focus to the
-- nearest enabled focusable widget in that direction. Tab navigation
-- uses widget.focus_next / focus_prev which wrap.
--
-- 9-patch: draw_9patch(name, x, y, w, h) reads the slice metadata
-- attached to the region in assets.lua via region_slice(), then issues
-- 9 draw_region calls (corners 1:1, edges stretched along their axis,
-- center stretched both). Regions without a slice degrade to a single
-- stretched draw_region.

local M = {}

-- ---- Shared helpers ----------------------------------------------------

local function rect_contains(x, y, w, h, px, py)
    return px >= x and px < x + w and py >= y and py < y + h
end

-- Extract horizontal / vertical bit groups out of an ALIGN_* bitmask.
-- (Lua 5.1 has no bitwise ops; ALIGN_* values are powers of 2 in two
-- disjoint groups so arithmetic does the job.)
--   horizontal: ALIGN_LEFT(1) | ALIGN_CENTER(2) | ALIGN_RIGHT(4)
--   vertical:   ALIGN_TOP(8)  | ALIGN_MIDDLE(16)| ALIGN_BOTTOM(32)
local function split_align(align)
    local ah = align % 8           -- 0..7
    local av = align - ah          -- 0, 8, 16, or 32
    return ah, av
end

-- Anchor (x, y) point inside a bbox according to an ALIGN_* mask.
-- Returns the point that draw_text / draw_region should be anchored at
-- so the resulting glyph rect lands aligned within (bx, by, bw, bh).
local function anchor_in(bx, by, bw, bh, align)
    local ah, av = split_align(align)
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

-- ---- Defaults (consumer-overridable on the module) ---------------------

M.DISABLED_ALPHA   = 0.5
M.FOCUS_COLOR      = { 1, 1, 1, 0.75 }
M.FOCUS_BORDER_PX  = 1
M.DEFAULT_COLOR    = { 1, 1, 1, 1 }
M.SPATIAL_PERP_K   = 2.0   -- larger = punish lateral offset more in spatial nav

-- ---- 9-patch -----------------------------------------------------------

-- Draw a 9-patch from a region whose slice metadata was declared in
-- assets.lua (slice = { x1, x2, y1, y2 }). If the region carries no
-- slice, we fall back to a stretched single-region draw — better than
-- crashing; the caller's intent (fit (x, y, w, h)) is preserved.
function M.draw_9patch(region_name, x, y, w, h)
    local x1, x2, y1, y2, sw, sh = region_slice(region_name)
    if not (x1 and x2 and y1 and y2 and sw and sh) then
        draw_region(region_name, x, y, { dst_w = w, dst_h = h })
        return
    end

    -- Source-pixel sizes for the 3×3 source grid
    local lw, rw = x1, sw - x2             -- L / R corner widths
    local th_, bh = y1, sh - y2            -- T / B corner heights
    local mid_sw  = x2 - x1                -- center source width
    local mid_sh  = y2 - y1                -- center source height

    -- Destination-pixel sizes for the stretched strips
    local mid_dw = w - lw - rw
    local mid_dh = h - th_ - bh
    if mid_dw < 0 then mid_dw = 0 end
    if mid_dh < 0 then mid_dh = 0 end

    -- A small helper for each tile: takes the slice's source rect and
    -- destination rect; src_x/src_y are region-local pixels.
    local function tile(src_x, src_y, src_w, src_h, dx, dy, dw, dh)
        if src_w <= 0 or src_h <= 0 or dw <= 0 or dh <= 0 then return end
        draw_region(region_name, dx, dy, {
            src_x = src_x, src_y = src_y, src_w = src_w, src_h = src_h,
            dst_w = dw,    dst_h = dh,
        })
    end

    local row1_y = y
    local row2_y = y + th_
    local row3_y = y + th_ + mid_dh
    local col1_x = x
    local col2_x = x + lw
    local col3_x = x + lw + mid_dw

    -- Top row
    tile(0,  0,  lw,     th_, col1_x, row1_y, lw,     th_)
    tile(x1, 0,  mid_sw, th_, col2_x, row1_y, mid_dw, th_)
    tile(x2, 0,  rw,     th_, col3_x, row1_y, rw,     th_)
    -- Middle row
    tile(0,  y1, lw,     mid_sh, col1_x, row2_y, lw,     mid_dh)
    tile(x1, y1, mid_sw, mid_sh, col2_x, row2_y, mid_dw, mid_dh)
    tile(x2, y1, rw,     mid_sh, col3_x, row2_y, rw,     mid_dh)
    -- Bottom row
    tile(0,  y2, lw,     bh, col1_x, row3_y, lw,     bh)
    tile(x1, y2, mid_sw, bh, col2_x, row3_y, mid_dw, bh)
    tile(x2, y2, rw,     bh, col3_x, row3_y, rw,     bh)
end

-- N-px outline at `color`, drawn inside the bbox as four edge quads.
function M.draw_outline(x, y, w, h, thickness, color)
    local opts = { color = color }
    draw_quad(x,                   y,                   w,         thickness, opts)
    draw_quad(x,                   y + h - thickness,   w,         thickness, opts)
    draw_quad(x,                   y,                   thickness, h,         opts)
    draw_quad(x + w - thickness,   y,                   thickness, h,         opts)
end

local function draw_focus_ring(x, y, w, h)
    M.draw_outline(x, y, w, h, M.FOCUS_BORDER_PX, M.FOCUS_COLOR)
end

M.rect_contains   = rect_contains
M.draw_focus_ring = draw_focus_ring

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

        text  = spec.text or "",
        font  = spec.font,
        align = spec.align or (1 + 8),           -- ALIGN_LEFT + ALIGN_TOP
        color = spec.color or M.DEFAULT_COLOR,
        scale = spec.scale or 1.0,
    }

    function w:draw()
        if not self.visible then return end
        local tx, ty = anchor_in(self.x, self.y, self.width, self.height, self.align)
        local color = self.disabled and dimmed(self.color, M.DISABLED_ALPHA)
                                    or self.color
        draw_text(self.text, tx, ty, {
            font  = self.font,
            scale = self.scale,
            align = self.align,
            color = color,
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

-- ---- Button ------------------------------------------------------------
--
-- bg_up / bg_down / bg_disabled are region names (their slice metadata
-- comes from assets.lua via region_slice). bg_disabled is optional —
-- when missing, the disabled state renders bg_up dimmed.
--
-- bg_color = { up = {...}, down = {...}, focused = {...}, disabled = {...} }
-- is the flat-color background — useful before art exists or for plain
-- styling. Each state's color is optional; missing states fall back
-- through focused -> up (priority disabled > pressed > focused > up).
-- When both a region and a color exist for the chosen state, the color
-- draws first and the 9-patch on top.

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
        text_align = spec.text_align or (2 + 16),       -- CENTER + MIDDLE
        text_color = spec.text_color or M.DEFAULT_COLOR,
        text_scale = spec.text_scale or 1.0,

        bg_up       = spec.bg_up,
        bg_down     = spec.bg_down,
        bg_disabled = spec.bg_disabled,
        bg_color    = spec.bg_color,                    -- optional state map

        icon         = spec.icon,
        icon_align   = spec.icon_align or (1 + 16),     -- LEFT + MIDDLE
        icon_padding = spec.icon_padding or { l = 0, r = 0, t = 0, b = 0 },

        on_click = spec.on_click,

        _pressed = false,
    }

    -- Pick state name (disabled / down / focused / up). Used to index
    -- both the region map and the color map.
    local function state_name(self)
        if self.disabled then return "disabled" end
        if self._pressed then return "down"     end
        if self.focused  then return "focused"  end
        return "up"
    end

    -- Pick a value from a state-keyed map with fallback chain
    -- (down -> up, focused -> up, disabled -> up). Returns nil if no
    -- entry exists for any of the fallbacks.
    local function pick_state(map, state)
        if not map then return nil end
        return map[state] or map.up
    end

    function w:draw()
        if not self.visible then return end

        local state = state_name(self)

        -- Flat-color background (drawn first if present).
        local color = pick_state(self.bg_color, state)
        if color then
            draw_quad(self.x, self.y, self.width, self.height, { color = color })
        end

        -- 9-patch background (drawn over the color fill if present).
        local bg, dim_bg
        if state == "disabled" then
            bg = self.bg_disabled or self.bg_up
            dim_bg = (self.bg_disabled == nil) and (self.bg_up ~= nil)
        elseif state == "down" then
            bg = self.bg_down or self.bg_up
            dim_bg = false
        else
            bg = self.bg_up
            dim_bg = false
        end
        if bg then
            M.draw_9patch(bg, self.x, self.y, self.width, self.height)
            if dim_bg then
                draw_quad(self.x, self.y, self.width, self.height,
                          { color = { 0, 0, 0, 1 - M.DISABLED_ALPHA } })
            end
        end

        -- Icon (optional). Anchored within the bbox by icon_align, inset
        -- from the matching edges by icon_padding. The opposite edges'
        -- padding is ignored (TOP padding shifts down the icon when
        -- align is TOP; ignored when align is BOTTOM).
        if self.icon then
            local ah, av = split_align(self.icon_align)
            local pad = self.icon_padding
            local ix
            if     ah == 4 then ix = self.x + self.width - (pad.r or 0)
            elseif ah == 2 then ix = self.x + self.width * 0.5
            else                ix = self.x + (pad.l or 0) end
            local iy
            if     av == 32 then iy = self.y + self.height - (pad.b or 0)
            elseif av == 16 then iy = self.y + self.height * 0.5
            else                 iy = self.y + (pad.t or 0) end
            local c = self.text_color
            if self.disabled then c = dimmed(c, M.DISABLED_ALPHA) end
            draw_region(self.icon, ix, iy, { align = self.icon_align, color = c })
        end

        -- Text (optional — buttons can be icon-only).
        if self.text and self.text ~= "" then
            local tx, ty = anchor_in(self.x, self.y, self.width, self.height,
                                     self.text_align)
            local c = self.disabled and dimmed(self.text_color, M.DISABLED_ALPHA)
                                    or self.text_color
            draw_text(self.text, tx, ty, {
                font  = self.font,
                scale = self.text_scale,
                align = self.text_align,
                color = c,
            })
        end

        if self.focused then
            draw_focus_ring(self.x, self.y, self.width, self.height)
        end
    end

    function w:hit(px, py)
        return rect_contains(self.x, self.y, self.width, self.height, px, py)
    end

    function w:mousedown(px, py, button)
        if self.disabled or button ~= 1 or not self:hit(px, py) then
            return false
        end
        self._pressed = true
        return true
    end

    function w:mouseup(px, py, button)
        local was_pressed = self._pressed
        self._pressed = false
        if self.disabled or button ~= 1 then return false end
        if was_pressed and self:hit(px, py) then
            if self.on_click then self:on_click() end
            return true
        end
        return false
    end

    function w:mousemove() end

    function w:keydown(name)
        if self.disabled or not self.focused then return false end
        if name == "return" or name == "space" then
            if self.on_click then self:on_click() end
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
-- widget bbox; the label follows after `box_gap` pixels. The box's
-- on-screen width is read from the unchecked region's native size via
-- region_size(). Click anywhere in the bbox toggles the value.
--
-- spec fields:
--   x, y, width, height
--   text                    (label drawn after the box)
--   font, color, scale      (label font + style)
--   box_unchecked           region name (REQUIRED for the box to render)
--   box_checked             region name (REQUIRED — same source size as
--                                       box_unchecked is recommended)
--   box_unchecked_disabled  optional dimmed-state region
--   box_checked_disabled    optional dimmed-state region
--   box_gap                 px between box and label (default 6)
--   value                   initial bool (default false)
--   on_change               function(self, new_value)
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

        text  = spec.text or "",
        font  = spec.font,
        color = spec.color or M.DEFAULT_COLOR,
        scale = spec.scale or 1.0,

        box_unchecked          = spec.box_unchecked,
        box_checked            = spec.box_checked,
        box_unchecked_disabled = spec.box_unchecked_disabled,
        box_checked_disabled   = spec.box_checked_disabled,
        box_gap                = spec.box_gap or 6,

        value     = spec.value or false,
        on_change = spec.on_change,

        _box_w    = 0,
        _box_h    = 0,
        _pressed  = false,
    }

    -- Resolve box source size at construction. Both states are
    -- expected to share width; if not, the unchecked region wins (it's
    -- what we measure against for the text offset).
    if w.box_unchecked then
        local bw, bh = region_size(w.box_unchecked)
        w._box_w = bw or 0
        w._box_h = bh or 0
    end
    -- Fallback when no region: 16-px square so the widget is still
    -- visible during bring-up.
    if w._box_w == 0 then w._box_w = 16; w._box_h = 16 end

    local function pick_box(self)
        if self.value then
            return (self.disabled and self.box_checked_disabled) or self.box_checked
        else
            return (self.disabled and self.box_unchecked_disabled) or self.box_unchecked
        end
    end

    function w:draw()
        if not self.visible then return end

        local region = pick_box(self)
        local mid_y  = self.y + self.height * 0.5

        if region then
            draw_region(region, self.x, mid_y, {
                align = 1 + 16,    -- ALIGN_LEFT + ALIGN_MIDDLE
            })
        else
            -- No region — render a flat outlined square at _box_w size.
            local box_y = mid_y - self._box_h * 0.5
            if self.value then
                draw_quad(self.x, box_y, self._box_w, self._box_h,
                          { color = { 0.55, 0.55, 0.85, 1.0 } })
            end
            M.draw_outline(self.x, box_y, self._box_w, self._box_h,
                           1, { 0.9, 0.9, 0.95, 0.8 })
        end

        if self.text and self.text ~= "" then
            local tx = self.x + self._box_w + self.box_gap
            local color = self.disabled and dimmed(self.color, M.DISABLED_ALPHA)
                                        or self.color
            draw_text(self.text, tx, mid_y, {
                font  = self.font,
                scale = self.scale,
                align = 1 + 16,    -- ALIGN_LEFT + ALIGN_MIDDLE
                color = color,
            })
        end

        if self.focused then
            draw_focus_ring(self.x, self.y, self.width, self.height)
        end
    end

    function w:hit(px, py)
        return rect_contains(self.x, self.y, self.width, self.height, px, py)
    end

    local function toggle(self)
        self.value = not self.value
        if self.on_change then self:on_change(self.value) end
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
--   track_bg          region name (9-patch); optional
--   knob              region name (sprite);  optional
--   knob_w, knob_h    explicit knob size; defaults to knob region's
--                     native size, or 16 if no knob region
--   min, max          numeric range (default 0..1)
--   value             current value (default min)
--   step              snap step; 0 (default) = continuous
--   orientation       "horizontal" (default) or "vertical"
--   on_change         function(self, new_value); fires while dragging

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

        track_bg = spec.track_bg,
        knob     = spec.knob,
        knob_w   = spec.knob_w,
        knob_h   = spec.knob_h,

        min   = spec.min or 0.0,
        max   = spec.max or 1.0,
        value = spec.value or spec.min or 0.0,
        step  = spec.step or 0.0,

        orientation = spec.orientation or "horizontal",
        on_change   = spec.on_change,

        _dragging = false,
    }

    -- Knob source size: explicit override, else region's native size,
    -- else 16-px fallback.
    if w.knob and (not w.knob_w or not w.knob_h) then
        local kw, kh = region_size(w.knob)
        w.knob_w = w.knob_w or kw
        w.knob_h = w.knob_h or kh
    end
    w.knob_w = w.knob_w or 16
    w.knob_h = w.knob_h or 16

    local function clamp_snap(self, v)
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
    -- start; value max puts it at end) — knob_w / knob_h reduce the
    -- effective travel.
    local function pos_to_value(self, px, py)
        local pct
        if self.orientation == "vertical" then
            local travel = self.height - self.knob_h
            pct = travel > 0 and (py - self.y - self.knob_h * 0.5) / travel or 0
        else
            local travel = self.width - self.knob_w
            pct = travel > 0 and (px - self.x - self.knob_w * 0.5) / travel or 0
        end
        if pct < 0 then pct = 0 end
        if pct > 1 then pct = 1 end
        return clamp_snap(self, self.min + pct * (self.max - self.min))
    end

    local function set_value(self, v)
        v = clamp_snap(self, v)
        if v ~= self.value then
            self.value = v
            if self.on_change then self:on_change(v) end
        end
    end

    function w:draw()
        if not self.visible then return end

        -- Track
        if self.track_bg then
            M.draw_9patch(self.track_bg, self.x, self.y, self.width, self.height)
        else
            draw_quad(self.x, self.y, self.width, self.height,
                      { color = { 0.18, 0.18, 0.22, 0.85 } })
        end

        -- Knob position
        local pct = 0
        if self.max > self.min then
            pct = (self.value - self.min) / (self.max - self.min)
            if pct < 0 then pct = 0 end
            if pct > 1 then pct = 1 end
        end
        local kw, kh = self.knob_w, self.knob_h
        local kx, ky
        if self.orientation == "vertical" then
            kx = self.x + (self.width - kw) * 0.5
            ky = self.y + pct * (self.height - kh)
        else
            kx = self.x + pct * (self.width - kw)
            ky = self.y + (self.height - kh) * 0.5
        end

        if self.knob then
            draw_region(self.knob, kx, ky, { dst_w = kw, dst_h = kh })
        else
            draw_quad(kx, ky, kw, kh, { color = { 0.75, 0.75, 0.85, 1.0 } })
        end

        if self.disabled then
            draw_quad(self.x, self.y, self.width, self.height,
                      { color = { 0, 0, 0, 1 - M.DISABLED_ALPHA } })
        end

        if self.focused then
            draw_focus_ring(self.x, self.y, self.width, self.height)
        end
    end

    function w:hit(px, py)
        return rect_contains(self.x, self.y, self.width, self.height, px, py)
    end

    function w:mousedown(px, py, button)
        if self.disabled or button ~= 1 or not self:hit(px, py) then return false end
        self._dragging = true
        set_value(self, pos_to_value(self, px, py))
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
            set_value(self, pos_to_value(self, px, py))
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
        set_value(self, self.value + sign * delta)
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
--   bg_enabled            region name (9-patch); optional
--   bg_disabled           region name (9-patch); optional
--   bg_color = { enabled = {...}, disabled = {...} }  optional flat fills
--   padding = { l, r, t, b }    text inset from bbox (default {6,6,0,0})
--   max_length            char cap (nil = unlimited)
--   placeholder           text shown when value is empty and not focused
--   placeholder_color     color for the placeholder text
--   on_change             function(self, text)  fires after every edit
--   on_submit             function(self, text)  fires on Return
--
-- Cursor blink runs at 2 Hz (visible for the first 0.25 s of each
-- 0.5 s cycle). The widget exposes :update(dt) for the host scene to
-- tick the blink phase; widget.dispatch_update(widgets, dt) ticks
-- every widget in a list.
--
-- Convention (per scripts/engine/scene.lua's on_textinput hook):
-- navigation keys (Left/Right/Home/End/Backspace/Delete/Return)
-- handle in keydown; character insertion is in textinput.

local function le_pad(spec_pad)
    local p = spec_pad or {}
    return {
        l = p.l or 6, r = p.r or 6,
        t = p.t or 0, b = p.b or 0,
    }
end

function M.line_edit(spec)
    local w = {
        x         = spec.x or 0,
        y         = spec.y or 0,
        width     = spec.width or 200,
        height    = spec.height or 28,
        disabled  = spec.disabled or false,
        visible   = spec.visible ~= false,
        focused   = false,
        focusable = true,

        text  = spec.text or "",
        font  = spec.font,
        color = spec.color or M.DEFAULT_COLOR,
        scale = spec.scale or 1.0,

        bg_enabled  = spec.bg_enabled,
        bg_disabled = spec.bg_disabled,
        bg_color    = spec.bg_color,

        padding           = le_pad(spec.padding),
        max_length        = spec.max_length,
        placeholder       = spec.placeholder or "",
        placeholder_color = spec.placeholder_color or { 0.6, 0.6, 0.65, 0.8 },

        on_change = spec.on_change,
        on_submit = spec.on_submit,

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
    -- input lengths.
    local function cursor_from_x(self, click_x)
        local text_x = self.x + self.padding.l
        local target = click_x - text_x
        if target <= 0 then return 0 end
        local best_pos  = 0
        local best_diff = math.huge
        for i = 0, #self.text do
            local w_ = text_width(self.text:sub(1, i), self.scale, self.font)
            local diff = w_ - target
            if diff < 0 then diff = -diff end
            if diff < best_diff then best_pos, best_diff = i, diff end
        end
        return best_pos
    end

    local function cursor_is_visible(self)
        if not self.focused or self.disabled then return false end
        return (self._blink_t % 0.5) < 0.25
    end

    local function emit_change(self)
        if self.on_change then self:on_change(self.text) end
    end

    function w:update(dt)
        if self.focused and not self.disabled then
            self._blink_t = self._blink_t + dt
        else
            self._blink_t = 0
        end
    end

    function w:draw()
        if not self.visible then return end

        -- Flat background fill.
        local state = self.disabled and "disabled" or "enabled"
        if self.bg_color then
            local c = self.bg_color[state] or self.bg_color.enabled
            if c then
                draw_quad(self.x, self.y, self.width, self.height, { color = c })
            end
        end

        -- 9-patch on top of the fill.
        local bg = self.disabled and (self.bg_disabled or self.bg_enabled)
                                 or self.bg_enabled
        if bg then
            M.draw_9patch(bg, self.x, self.y, self.width, self.height)
        end

        local ix, iy, iw, ih = inner(self)
        local mid_y = self.y + self.height * 0.5

        -- Text or placeholder.
        local shown = self.text
        local color
        if shown == "" and not self.focused and self.placeholder ~= "" then
            shown = self.placeholder
            color = self.placeholder_color
        else
            color = self.disabled and dimmed(self.color, M.DISABLED_ALPHA)
                                  or self.color
        end
        if shown ~= "" then
            draw_text(shown, ix, mid_y, {
                font  = self.font,
                scale = self.scale,
                align = 1 + 16,   -- ALIGN_LEFT + ALIGN_MIDDLE
                color = color,
            })
        end

        -- Blinking cursor at the prefix width.
        if cursor_is_visible(self) then
            local prefix_w = text_width(self.text:sub(1, self.cursor),
                                        self.scale, self.font)
            local cx = ix + prefix_w
            -- Clamp to inner rect so an over-long string doesn't paint
            -- the cursor off-screen indefinitely.
            if cx > ix + iw then cx = ix + iw end
            draw_quad(cx, iy, 1, ih, { color = self.color })
        end

        if self.focused then
            draw_focus_ring(self.x, self.y, self.width, self.height)
        end
    end

    function w:hit(px, py)
        return rect_contains(self.x, self.y, self.width, self.height, px, py)
    end

    function w:mousedown(px, py, button)
        if self.disabled or button ~= 1 or not self:hit(px, py) then return false end
        self.cursor = cursor_from_x(self, px)
        self._blink_t = 0   -- re-show cursor immediately on click
        return true
    end

    function w:mouseup() return false end
    function w:mousemove() end

    function w:textinput(ch)
        if self.disabled or not self.focused then return end
        if not ch or ch == "" then return end
        if self.max_length and #self.text >= self.max_length then return end
        local before = self.text:sub(1, self.cursor)
        local after  = self.text:sub(self.cursor + 1)
        self.text   = before .. ch .. after
        self.cursor = self.cursor + #ch
        self._blink_t = 0
        emit_change(self)
    end

    function w:keydown(name)
        if self.disabled or not self.focused then return false end

        if name == "backspace" then
            if self.cursor > 0 then
                self.text   = self.text:sub(1, self.cursor - 1)
                            .. self.text:sub(self.cursor + 1)
                self.cursor = self.cursor - 1
                self._blink_t = 0
                emit_change(self)
            end
            return true
        end

        if name == "delete" then
            if self.cursor < #self.text then
                self.text = self.text:sub(1, self.cursor)
                          .. self.text:sub(self.cursor + 2)
                self._blink_t = 0
                emit_change(self)
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
            if self.on_submit then self:on_submit(self.text) end
            return true
        end

        return false   -- Tab / Esc / other keys fall through to scene
    end

    function w:keyup() return false end

    return w
end

-- ---- Update dispatch ---------------------------------------------------
--
-- Per-frame tick for widgets that need it (LineEdit's cursor blink).
-- Most widgets don't define :update; the helper just skips them.
function M.dispatch_update(widgets, dt)
    for _, w in ipairs(widgets) do
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
--   self.focused_widget = widget.focus_next(self.widgets, self.focused_widget)
--                         or self.focused_widget
--   self.focused_widget.focused = true

local function is_focusable(w)
    return w and w.focusable and not w.disabled and w.visible ~= false
end

-- Linear Tab navigation — wraps. Returns the next focusable widget or
-- nil if none exist at all. Pass nil for `current` to get the first
-- focusable widget.
function M.focus_next(widgets, current)
    if #widgets == 0 then return nil end
    local start
    if current then
        for i, w in ipairs(widgets) do if w == current then start = i; break end end
    end
    start = start or 0
    for k = 1, #widgets do
        local i = ((start - 1 + k) % #widgets) + 1
        if is_focusable(widgets[i]) and widgets[i] ~= current then
            return widgets[i]
        end
    end
    -- Only one focusable widget — return it (or current if current is it).
    if is_focusable(current) then return current end
    return nil
end

function M.focus_prev(widgets, current)
    if #widgets == 0 then return nil end
    local start
    if current then
        for i, w in ipairs(widgets) do if w == current then start = i; break end end
    end
    start = start or (#widgets + 1)
    for k = 1, #widgets do
        local i = ((start - 1 - k) % #widgets) + 1
        if is_focusable(widgets[i]) and widgets[i] ~= current then
            return widgets[i]
        end
    end
    if is_focusable(current) then return current end
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
function M.focus_direction(widgets, current, dir)
    if not current then
        -- No current focus — fall back to the first focusable widget.
        return M.focus_next(widgets, nil)
    end
    local cx = current.x + current.width  * 0.5
    local cy = current.y + current.height * 0.5

    -- Direction vector: (axial unit) and a perp extractor.
    local axial_x, axial_y = 0, 0
    if     dir == "up"    then axial_y = -1
    elseif dir == "down"  then axial_y =  1
    elseif dir == "left"  then axial_x = -1
    elseif dir == "right" then axial_x =  1
    else return nil end

    local best, best_score
    for _, w in ipairs(widgets) do
        if is_focusable(w) and w ~= current then
            local wx = w.x + w.width  * 0.5
            local wy = w.y + w.height * 0.5
            local dx = wx - cx
            local dy = wy - cy

            local axial = dx * axial_x + dy * axial_y
            if axial > 0 then
                -- perpendicular magnitude is just the other axis here
                local perp
                if axial_x ~= 0 then perp = dy < 0 and -dy or dy
                else                 perp = dx < 0 and -dx or dx end
                local score = axial + M.SPATIAL_PERP_K * perp
                if not best_score or score < best_score then
                    best, best_score = w, score
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
--   self.focused_widget = widget.dispatch_keydown(self.widgets,
--                                                 self.focused_widget, name)
function M.dispatch_keydown(widgets, current, name)
    if current and current.keydown and current:keydown(name) then
        return current
    end
    local next_w
    if name == "tab" then
        next_w = M.focus_next(widgets, current)
    elseif name == "up" or name == "down" or name == "left" or name == "right" then
        next_w = M.focus_direction(widgets, current, name)
    end
    if next_w and next_w ~= current then
        if current then current.focused = false end
        next_w.focused = true
        return next_w
    end
    return current
end

return M
