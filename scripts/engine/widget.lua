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
