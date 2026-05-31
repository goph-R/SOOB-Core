-- engine/scene.lua — Lua-side scene stack.
--
-- A "scene" is an app-level slice of the game: the main menu, the
-- playing screen, a mini-game launched from inside the playing screen,
-- a credits roll, an options panel. The stack lets one scene push
-- another on top of itself (the new one runs; the old one is paused),
-- and pop back when done.
--
-- This sits ABOVE the engine's draw / input bindings and BELOW any
-- game-specific dialog system. A dialog is a modal animation within a
-- scene; switching from the menu into the game is a scene transition.
--
-- A scene is a plain table with optional fields:
--   enter(self)                      called when pushed (good place
--                                    to start music, reset timers)
--   exit(self)                       called when popped (stop music
--                                    you started, free resources)
--   update(self, dt)                 per-frame logic — top scene only
--   render(self)                     per-frame draw — see "rendering"
--   keydown(self, name)              "escape", "return", "a", etc.
--   keyup(self, name)
--   mousedown(self, x, y, button)    virtual-canvas coords
--   mouseup(self, x, y, button)
--   mousemove(self, x, y, dx, dy)
--   root (widget.panel)              auto-forward target (see below)
--   transparent (boolean)            see "rendering"
--
-- Methods are called with the scene table as `self` (use `:` syntax
-- when defining them). Missing methods are no-ops — except update /
-- render / keydown / mousedown / mouseup / mousemove, which auto-
-- forward to `self.root` when the scene defines no override. So a
-- scene that just wants to host widgets can set `root = widget.panel
-- ({...})` and skip all the per-method boilerplate; one that wants to
-- intercept (custom render with background art, say) defines the
-- method and calls `self.root:draw()` (or whatever) at the right spot.
--
-- Rendering — only the top scene receives update / input, but render
-- walks the stack from the lowest opaque scene up. A scene with
-- `transparent = true` lets the scene below it render too. Use this
-- for HUD overlays, semi-transparent menus over the game, etc.
--
--   stack: [game, pause_menu={transparent=true}]
--                       ^ top — gets update + input
--           ^ also renders behind pause_menu
--
-- Wiring up the engine hooks — option A (full control):
--   local scene = require "engine.scene"
--   function on_update(dt)    scene.dispatch_update(dt)        end
--   function on_render()       scene.dispatch_render()           end
--   function on_keydown(n)     scene.dispatch_keydown(n)         end
--   function on_keyup(n)       scene.dispatch_keyup(n)           end
--   function on_mousedown(x,y,b)   scene.dispatch_mousedown(x,y,b)    end
--   function on_mouseup(x,y,b)     scene.dispatch_mouseup(x,y,b)      end
--   function on_mousemove(x,y,dx,dy) scene.dispatch_mousemove(x,y,dx,dy) end
--
-- Option B (convenience, no extra logic in your hooks):
--   require("engine.scene").install_hooks(_G)
--
-- Then push your first scene from on_start():
--   function on_start() scene.push(main_menu) end

local anim = require "engine.animation"

local M = {}

local stack = {}

local function top()
    return stack[#stack]
end

local function index_of(s)
    for i = 1, #stack do
        if stack[i] == s then return i end
    end
    return nil
end

-- ---- Inspection -------------------------------------------------------

function M.depth() return #stack end
function M.top()   return top() end
function M.empty() return #stack == 0 end

-- ---- Transition state -------------------------------------------------
--
-- in_transition flips true when a transition action is attached to any
-- scene's root and back to false when refresh_in_transition can't find
-- any scene with an in-flight root.action. The flag is what gates input
-- and per-scene :update during a fade / slide / zoom (see dispatch_*
-- below).

local in_transition = false

-- ---- Overlay ----------------------------------------------------------
--
-- Module-level overlay: a full-canvas quad drawn ON TOP of every scene
-- by dispatch_render whenever alpha > 0. Externally owned — transition
-- presets like fade_through_black attach an action that tweens
-- M.overlay.alpha 0 → 1 → 0 to cover a scene swap. Independent of
-- per-scene root.alpha, so it covers anything the scene draws (direct
-- draw_region calls, background art, etc.) — exactly what a fade-via-
-- root.alpha can't reach. The overlay is ticked once per frame in
-- dispatch_update (cheap when idle: no action attached).

M.overlay = {
    alpha  = 0,
    color  = { 0, 0, 0, 1 },     -- swap for a non-black fade if needed
    action = nil,
}

local function refresh_in_transition()
    if M.overlay.action then return end
    for i = 1, #stack do
        if stack[i].root and stack[i].root.action then return end
    end
    in_transition = false
end

-- Attach the "scene is appearing" action returned by transition.fade /
-- slide / zoom. The scene was already pushed; this just marks it
-- transparent so the layer below renders behind it, hangs the action
-- off root.action, and queues "clear transparent" for completion.
local function attach_in_action(scene, action)
    if not scene.root or not action then return end
    scene.transparent = true
    scene.root.action = action
    scene.root.on_action_done = function()
        scene.transparent = false
        refresh_in_transition()
    end
    in_transition = true
end

-- Attach the "scene is leaving" action. The scene STAYS in the stack
-- until its action completes — that's when we actually remove it and
-- fire :exit. Marked transparent so the layer below shows through as
-- it fades / slides off.
local function attach_out_action(scene, action)
    if not scene.root or not action then return false end
    scene.transparent = true
    scene.root.action = action
    scene.root.on_action_done = function()
        local i = index_of(scene)
        if i then table.remove(stack, i) end
        if scene.exit then scene:exit() end
        refresh_in_transition()
    end
    in_transition = true
    return true
end

-- ---- Mutation ---------------------------------------------------------
--
-- push / pop / replace take an optional transition descriptor (see
-- engine.transition). Default is an instant cut — no behavior change
-- for callers that don't pass one. A scene without a `root` panel
-- falls back to cut even when a transition is supplied (the system
-- needs a tween target).

function M.push(scene, transition)
    if type(scene) ~= "table" then
        error("scene.push: expected a table, got " .. type(scene), 2)
    end
    stack[#stack + 1] = scene
    if scene.enter then scene:enter() end
    if transition and transition.in_action_fn then
        attach_in_action(scene, transition.in_action_fn(scene))
    end
end

function M.pop(transition)
    local s = stack[#stack]
    if not s then return nil end
    if transition and transition.out_action_fn and s.root then
        if attach_out_action(s, transition.out_action_fn(s)) then
            return s    -- deferred pop — :exit fires on completion
        end
    end
    -- Instant cut: pop now.
    stack[#stack] = nil
    if s.exit then s:exit() end
    return s
end

-- Overlay-based scene swap. Only one scene is on the stack at any
-- given moment — the OLD scene renders until the overlay is fully
-- opaque (mid-transition), then we pop old, push new, fire new:enter.
-- This keeps the render loop drawing exactly one scene and means the
-- new scene's enter() (and any state mutation the caller queued) lands
-- behind a fully-black frame — no flicker of fresh state in the
-- outgoing scene before the swap.
local function overlay_replace(scene_new, transition)
    local old = stack[#stack]
    local function swap()
        if old then
            local i = index_of(old)
            if i then table.remove(stack, i) end
            if old.exit then old:exit() end
        end
        stack[#stack + 1] = scene_new
        if scene_new.enter then scene_new:enter() end
    end
    M.overlay.alpha  = 0
    M.overlay.action = transition.overlay_action_fn(swap)
    M.overlay.on_action_done = function()
        M.overlay.action = nil
        refresh_in_transition()
    end
    in_transition = true
end

-- pop current, push new. With a transition: pushes new on top, runs
-- the in-action on it, and the out-action on the old scene; the old is
-- popped (and :exit fires) when its action completes.
function M.replace(scene, transition)
    if transition and transition.overlay_action_fn then
        overlay_replace(scene, transition)
        return
    end

    local old = stack[#stack]
    stack[#stack + 1] = scene
    if scene.enter then scene:enter() end

    if transition and (transition.in_action_fn or transition.out_action_fn) then
        if transition.in_action_fn then
            attach_in_action(scene, transition.in_action_fn(scene))
        end
        if old then
            if transition.out_action_fn and old.root then
                if not attach_out_action(old, transition.out_action_fn(old)) then
                    -- root present but action factory returned nil — instant remove
                    local i = index_of(old)
                    if i then table.remove(stack, i) end
                    if old.exit then old:exit() end
                end
            else
                local i = index_of(old)
                if i then table.remove(stack, i) end
                if old.exit then old:exit() end
            end
        end
    elseif old then
        -- Instant cut: remove the old scene right away.
        local i = index_of(old)
        if i then table.remove(stack, i) end
        if old.exit then old:exit() end
    end
end

-- Pop everything. Each scene's exit() fires top-down.
function M.clear()
    while #stack > 0 do M.pop() end
end

-- ---- Dispatch ---------------------------------------------------------
-- These are called from your on_* engine hooks. The top scene receives
-- update / input. render walks bottom-to-top from the lowest opaque
-- scene.

function M.dispatch_update(dt)
    -- Tick the overlay first; it's the action target for overlay-based
    -- transitions and a no-op when no action is attached.
    anim.tick_action(M.overlay, dt)

    if in_transition then
        -- Tick every scene's root so both the leaving and entering
        -- transitions advance in lockstep. Skip the normal scene
        -- :update so per-scene timers (LineEdit cursor blink, score
        -- count-ups, gameplay clocks) don't drift mid-fade.
        for i = 1, #stack do
            local s = stack[i]
            if s.root then anim.tick_action(s.root, dt) end
        end
        return
    end
    local t = top()
    if not t then return end
    -- Tick the root panel's OWN action before delegating, so a scene
    -- that overrides :update (and never reaches root:update) still
    -- gets its root panel animated. The root's children are ticked
    -- inside panel:update via anim.tick_action.
    if t.root then anim.tick_action(t.root, dt) end
    if t.update then t:update(dt)
    elseif t.root and t.root.update then t.root:update(dt) end
end

function M.dispatch_render()
    local n = #stack
    if n == 0 then return end
    -- Find lowest scene to render: walk down past transparent scenes
    -- until we hit an opaque one (or the bottom of the stack).
    local start = n
    while start > 1 and stack[start].transparent do
        start = start - 1
    end
    for i = start, n do
        local s = stack[i]
        if s.render then s:render()
        elseif s.root and s.root.draw then s.root:draw() end
    end

    -- Module-level overlay (see M.overlay). Drawn after every scene
    -- so it covers anything any scene rendered — including direct
    -- draw_region calls outside the panel tree (menu_bg, blur layers,
    -- etc.) that a root.alpha fade wouldn't reach.
    if M.overlay.alpha > 0 then
        local vw, vh = view_size()
        local c = M.overlay.color
        local a = M.overlay.alpha
        if a > 1 then a = 1 end
        draw_quad(-vw * 0.5, -vh * 0.5, vw, vh, {
            color = { c[1], c[2], c[3], (c[4] or 1.0) * a },
        })
    end
end

-- Input dispatchers short-circuit while a transition is in flight so
-- stray clicks / keypresses don't land on half-faded buttons.

function M.dispatch_keydown(name)
    if in_transition then return end
    local t = top()
    if not t then return end
    if t.keydown then t:keydown(name)
    elseif t.root and t.root.keydown then t.root:keydown(name) end
end

function M.dispatch_textinput(char)
    if in_transition then return end
    local t = top()
    if not t then return end
    if t.textinput then t:textinput(char)
    elseif t.root and t.root.textinput then t.root:textinput(char) end
end

function M.dispatch_keyup(name)
    if in_transition then return end
    local t = top()
    if t and t.keyup then t:keyup(name) end
end

function M.dispatch_mousedown(x, y, button)
    if in_transition then return end
    local t = top()
    if not t then return end
    if t.mousedown then t:mousedown(x, y, button)
    elseif t.root and t.root.mousedown then t.root:mousedown(x, y, button) end
end

function M.dispatch_mouseup(x, y, button)
    if in_transition then return end
    local t = top()
    if not t then return end
    if t.mouseup then t:mouseup(x, y, button)
    elseif t.root and t.root.mouseup then t.root:mouseup(x, y, button) end
end

function M.dispatch_mousemove(x, y, dx, dy)
    if in_transition then return end
    local t = top()
    if not t then return end
    if t.mousemove then t:mousemove(x, y, dx, dy)
    elseif t.root and t.root.mousemove then t.root:mousemove(x, y, dx, dy) end
end

-- ---- Hook installation ------------------------------------------------
-- Convenience for consumers that don't need their own logic in the
-- engine hooks — wires every on_* global to the matching dispatcher.
-- Pass _G (or your env table) explicitly so the function signature
-- isn't surprising.

function M.install_hooks(env)
    env = env or _G
    env.on_update    = function(dt)     M.dispatch_update(dt)    end
    env.on_render    = function()        M.dispatch_render()      end
    env.on_keydown   = function(name)    M.dispatch_keydown(name) end
    env.on_keyup     = function(name)    M.dispatch_keyup(name)   end
    env.on_textinput = function(ch)      M.dispatch_textinput(ch) end
    env.on_mousedown = function(x, y, b) M.dispatch_mousedown(x, y, b) end
    env.on_mouseup   = function(x, y, b) M.dispatch_mouseup(x, y, b)   end
    env.on_mousemove = function(x, y, dx, dy)
        M.dispatch_mousemove(x, y, dx, dy)
    end
end

return M
