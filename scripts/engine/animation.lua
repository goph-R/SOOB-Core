-- engine/animation.lua — composable per-widget action graphs.
--
-- Every widget can have ONE action attached via `widget.action`. The
-- ticker (anim.tickAction) walks the action each frame; when the
-- action completes, widget.action is cleared and widget:onActionDone()
-- fires if defined.
--
-- Actions are plain tables with one method:
--
--   action:update(widget, dt) -> leftover_dt | nil
--     nil = still running
--     N   = completed this tick; N is the leftover dt (elapsed -
--           duration). Sequences pass that leftover into the next
--           child so timing doesn't drift across boundaries — a 0.7s
--           tick across a 0.5s leaf hands 0.2s to the next leaf
--           instead of missing a frame.
--
-- Composition:
--   anim.sequence{a, b, c, ...}   -- one after another
--   anim.parallel{a, b, c, ...}   -- together; done when last finishes
--
-- Leaves capture their start values on the first tick, so moveBy /
-- fadeTo / scaleTo read whatever the widget's state was when the
-- action ACTUALLY started — after any preceding delay or earlier
-- sequence step has run.
--
-- One-shot: action state lives on the action table itself, so don't
-- share a single action between widgets. Build a fresh action per
-- widget (factories are cheap — they just allocate a table).
--
-- Cancel by writing `widget.action = nil`; onActionDone does NOT
-- fire for cancellations (it's a "naturally completed" signal).

local M = {}

-- ---- Easings -----------------------------------------------------------
--
-- easing(t) where t and return value are both in [0, 1]. Pass any of
-- these as the `easing` argument of moveTo / fadeTo / scaleTo /
-- tweenTo / etc.; omit for linear.

M.linear      = function(t) return t end
M.easeIn     = function(t) return t * t end
M.easeOut    = function(t) local u = 1 - t; return 1 - u * u end
M.easeInOut = function(t)
    if t < 0.5 then return 2 * t * t end
    local u = 1 - t
    return 1 - 2 * u * u
end

-- Overshoots past 1, then settles — classic "pop in" feel.
local BACK_S = 1.70158
M.easeOutBack = function(t)
    local u = t - 1
    return u * u * ((BACK_S + 1) * u + BACK_S) + 1
end

-- Penner-style bounce on the way OUT (lands and bounces).
M.bounceOut = function(t)
    if t < 1 / 2.75 then
        return 7.5625 * t * t
    elseif t < 2 / 2.75 then
        t = t - 1.5 / 2.75
        return 7.5625 * t * t + 0.75
    elseif t < 2.5 / 2.75 then
        t = t - 2.25 / 2.75
        return 7.5625 * t * t + 0.9375
    else
        t = t - 2.625 / 2.75
        return 7.5625 * t * t + 0.984375
    end
end

-- Custom-curve factories — t^n. Use for cubic / quart / quint / etc.
-- without us shipping a named function for each.
function M.easeInPow(n)
    return function(t) return t ^ n end
end
function M.easeOutPow(n)
    return function(t) local u = 1 - t; return 1 - u ^ n end
end
function M.easeInOutPow(n)
    return function(t)
        if t < 0.5 then return (2 ^ (n - 1)) * (t ^ n) end
        local u = 1 - t
        return 1 - (2 ^ (n - 1)) * (u ^ n)
    end
end

-- ---- Tick driver -------------------------------------------------------
--
-- Hosts (widget.panel, engine.scene) call this each frame. Advances
-- widget.action by `dt`; when the action returns a leftover, it's
-- considered complete — widget.action is cleared and onActionDone
-- fires.

function M.tickAction(widget, dt)
    local a = widget.action
    if not a then return end
    local leftover = a:update(widget, dt)
    if leftover ~= nil then
        widget.action = nil
        if widget.onActionDone then widget:onActionDone() end
    end
end

-- ---- Tween helper ------------------------------------------------------
--
-- Most leaves share the same shape: capture start state on tick 1,
-- accumulate elapsed, lerp from start toward target via an easing,
-- return leftover on completion. makeTween factors that out so a
-- leaf is one capture / apply / finish triple.

local function lerp(a, b, t) return a + (b - a) * t end

-- spec.duration  : seconds
-- spec.easing    : optional easing fn (default linear)
-- spec.capture   : fn(widget) -> arbitrary "start" table for this run
-- spec.apply     : fn(widget, start, k) where k is eased [0..1]
-- spec.finish    : fn(widget, start) — called once at completion to
--                  snap to the exact target (avoids float drift)
local function makeTween(spec)
    local t = {
        duration = spec.duration,
        easing   = spec.easing or M.linear,
        _elapsed = 0,
        _start   = nil,
        _capture = spec.capture,
        _apply   = spec.apply,
        _finish  = spec.finish,
    }
    function t:update(widget, dt)
        if not self._start then self._start = self._capture(widget) end
        self._elapsed = self._elapsed + dt
        local progress = self._elapsed / self.duration
        if progress >= 1 then
            self._finish(widget, self._start)
            return self._elapsed - self.duration
        end
        self._apply(widget, self._start, self.easing(progress))
        return nil
    end
    return t
end

-- ---- Leaves: position / opacity / scale --------------------------------

function M.moveTo(x, y, duration, easing)
    return makeTween{
        duration = duration, easing = easing,
        capture = function(w) return { x = w.x, y = w.y } end,
        apply   = function(w, s, k)
            w.x = lerp(s.x, x, k)
            w.y = lerp(s.y, y, k)
        end,
        finish  = function(w) w.x = x; w.y = y end,
    }
end

function M.moveBy(dx, dy, duration, easing)
    return makeTween{
        duration = duration, easing = easing,
        capture = function(w) return { x = w.x, y = w.y } end,
        apply   = function(w, s, k)
            w.x = s.x + dx * k
            w.y = s.y + dy * k
        end,
        finish  = function(w, s) w.x = s.x + dx; w.y = s.y + dy end,
    }
end

function M.fadeTo(alpha, duration, easing)
    return makeTween{
        duration = duration, easing = easing,
        capture = function(w) return { a = w.alpha or 1.0 } end,
        apply   = function(w, s, k) w.alpha = lerp(s.a, alpha, k) end,
        finish  = function(w) w.alpha = alpha end,
    }
end

function M.fadeIn (duration, easing) return M.fadeTo(1.0, duration, easing) end
function M.fadeOut(duration, easing) return M.fadeTo(0.0, duration, easing) end

function M.scaleTo(scale, duration, easing)
    return makeTween{
        duration = duration, easing = easing,
        capture = function(w) return { s = w.scale or 1.0 } end,
        apply   = function(w, s, k) w.scale = lerp(s.s, scale, k) end,
        finish  = function(w) w.scale = scale end,
    }
end

function M.scaleBy(factor, duration, easing)
    return makeTween{
        duration = duration, easing = easing,
        capture = function(w) return { s = w.scale or 1.0 } end,
        apply   = function(w, s, k) w.scale = s.s * (1 + (factor - 1) * k) end,
        finish  = function(w, s) w.scale = s.s * factor end,
    }
end

-- Generic numeric property tween — for less common fields not covered
-- by the named leaves (textScale, custom widget fields, etc.).
function M.tweenTo(prop, value, duration, easing)
    return makeTween{
        duration = duration, easing = easing,
        capture = function(w) return { v = w[prop] or 0 } end,
        apply   = function(w, s, k) w[prop] = lerp(s.v, value, k) end,
        finish  = function(w) w[prop] = value end,
    }
end

-- ---- Leaves: control ---------------------------------------------------

function M.delay(seconds)
    return {
        duration = seconds,
        _elapsed = 0,
        update = function(self, widget, dt)
            self._elapsed = self._elapsed + dt
            if self._elapsed >= self.duration then
                return self._elapsed - self.duration
            end
            return nil
        end,
    }
end

-- Fires fn(widget) once, completes instantly with the full dt as
-- leftover so a sequence's next leaf doesn't miss the frame.
function M.call(fn)
    return {
        update = function(self, widget, dt)
            fn(widget)
            return dt
        end,
    }
end

-- Snaps a set of properties at once. Instantaneous.
function M.set(props)
    return {
        update = function(self, widget, dt)
            for k, v in pairs(props) do widget[k] = v end
            return dt
        end,
    }
end

-- ---- Composites --------------------------------------------------------

function M.sequence(actions)
    return {
        _actions = actions,
        _index   = 1,
        update = function(self, widget, dt)
            while self._index <= #self._actions do
                local leftover = self._actions[self._index]:update(widget, dt)
                if leftover == nil then return nil end
                self._index = self._index + 1
                dt = leftover
            end
            return dt
        end,
    }
end

function M.parallel(actions)
    -- _running[i] starts nil (still running), set to false on
    -- completion. Parallel finishes when EVERY child has finished;
    -- leftover dt is the smallest of the children's completion
    -- leftovers (corresponding to the child that finished last).
    return {
        _actions  = actions,
        _running  = {},
        _min_left = nil,
        update = function(self, widget, dt)
            local anyRunning = false
            for i, a in ipairs(self._actions) do
                if self._running[i] ~= false then
                    local leftover = a:update(widget, dt)
                    if leftover == nil then
                        anyRunning = true
                    else
                        self._running[i] = false
                        if self._min_left == nil or leftover < self._min_left then
                            self._min_left = leftover
                        end
                    end
                end
            end
            if anyRunning then return nil end
            return self._min_left or 0
        end,
    }
end

return M
