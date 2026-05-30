-- engine/transition.lua — scene transition presets.
--
-- A transition is a description object consumed by engine.scene's
-- push / pop / replace. It carries up to two factories:
--
--   in_action_fn(scene)   -> action          (entering scene's animation)
--   out_action_fn(scene)  -> action          (leaving scene's animation)
--
-- Both are optional and both return an engine.animation action graph
-- that the scene module attaches to `scene.root.action`. Each factory
-- may mutate `scene.root` before returning to seed the starting visual
-- state (alpha=0 for a fade-in, off-screen x for a slide-in, etc.).
--
-- Transitions ARE animations on the scene's root panel — no new tween
-- machinery. Alpha cascades through children via panel:draw; move and
-- scale on the root reposition / resize the whole UI (panel scale is
-- currently no-op until GL matrix bindings land — see widget.lua).
--
-- The scene module handles the lifecycle:
--   * sets scene.transparent = true so the layer below renders during
--     the transition, then clears it on completion
--   * attaches a root.on_action_done callback that does the cleanup
--     (pop the old scene + fire its :exit, etc.)
--   * gates input + scene update while any transition is in flight
--
-- A scene without a `root` panel falls back to an instant cut — the
-- transition system needs a tween target.

local anim = require "engine.animation"

local M = {}

-- ---- cut: instant ------------------------------------------------------

-- No action functions — caller just stacks scenes. Same shape as the
-- other presets so the scene module can treat them uniformly.
function M.cut()
    return {}
end

-- ---- fade --------------------------------------------------------------

function M.fade(duration, easing)
    return {
        out_action_fn = function(s)
            return anim.fade_to(0.0, duration, easing)
        end,
        in_action_fn = function(s)
            s.root.alpha = 0
            return anim.fade_to(1.0, duration, easing)
        end,
    }
end

-- ---- slide -------------------------------------------------------------
--
-- direction = which way the OLD scene moves off-screen ("left" / "right"
-- / "up" / "down"). The new scene comes in from the opposite edge.

local SLIDE_OUT = {
    left  = {-1,  0},   right = { 1,  0},
    up    = { 0, -1},   down  = { 0,  1},
}
local SLIDE_IN = {
    left  = { 1,  0},   right = {-1,  0},
    up    = { 0,  1},   down  = { 0, -1},
}

function M.slide(direction, duration, easing)
    return {
        out_action_fn = function(s)
            local vw, vh = view_size()
            local d = SLIDE_OUT[direction]
            return anim.move_by(d[1] * vw, d[2] * vh, duration, easing)
        end,
        in_action_fn = function(s)
            local vw, vh = view_size()
            local d = SLIDE_IN[direction]
            s.root.x = d[1] * vw
            s.root.y = d[2] * vh
            return anim.move_to(0, 0, duration, easing)
        end,
    }
end

-- ---- zoom --------------------------------------------------------------
--
-- Fade combined with a slight scale-up. NOTE: panel.scale is currently
-- ignored (group transform needs GL matrix bindings) so until that
-- lands, zoom degrades visually to a plain fade when the root is a
-- panel. Kept in the preset table so call sites don't have to change
-- when group-scale becomes real.

function M.zoom(duration, easing)
    return {
        out_action_fn = function(s)
            return anim.fade_to(0.0, duration, easing)
        end,
        in_action_fn = function(s)
            s.root.alpha = 0
            s.root.scale = 0.92
            return anim.parallel{
                anim.fade_to(1.0, duration, easing),
                anim.scale_to(1.0, duration, easing or anim.ease_out_back),
            }
        end,
    }
end

return M
