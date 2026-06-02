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
--   keyDown(self, name)              "escape", "return", "a", etc.
--   keyUp(self, name)
--   mouseDown(self, x, y, button)    virtual-canvas coords
--   mouseUp(self, x, y, button)
--   mouseMove(self, x, y, dx, dy)
--   root (widget.panel)              auto-forward target (see below)
--   transparent (boolean)            see "rendering"
--
-- Methods are called with the scene table as `self` (use `:` syntax
-- when defining them). Missing methods are no-ops — except update /
-- render / keyDown / mouseDown / mouseUp / mouseMove, which auto-
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
--   function onUpdate(dt)    scene.dispatchUpdate(dt)        end
--   function onRender()       scene.dispatchRender()           end
--   function onKeyDown(n)     scene.dispatchKeyDown(n)         end
--   function onKeyUp(n)       scene.dispatchKeyUp(n)           end
--   function onMouseDown(x,y,b)   scene.dispatchMouseDown(x,y,b)    end
--   function onMouseUp(x,y,b)     scene.dispatchMouseUp(x,y,b)      end
--   function onMouseMove(x,y,dx,dy) scene.dispatchMouseMove(x,y,dx,dy) end
--
-- Option B (convenience, no extra logic in your hooks):
--   require("engine.scene").installHooks(_G)
--
-- Then push your first scene from onStart():
--   function onStart() scene.push(mainMenu) end

local anim = require "engine.animation"

local M = {}

local stack = {}

local function top()
    return stack[#stack]
end

local function indexOf(s)
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
-- inTransition flips true when a transition action is attached to any
-- scene's root and back to false when refreshInTransition can't find
-- any scene with an in-flight root.action. The flag is what gates input
-- and per-scene :update during a fade / slide / zoom (see dispatch_*
-- below).

local inTransition = false

-- ---- Overlay ----------------------------------------------------------
--
-- Module-level overlay: a full-canvas quad drawn ON TOP of every scene
-- by dispatchRender whenever alpha > 0. Externally owned — transition
-- presets like fadeThroughBlack attach an action that tweens
-- M.overlay.alpha 0 → 1 → 0 to cover a scene swap. Independent of
-- per-scene root.alpha, so it covers anything the scene draws (direct
-- drawRegion calls, background art, etc.) — exactly what a fade-via-
-- root.alpha can't reach. The overlay is ticked once per frame in
-- dispatchUpdate (cheap when idle: no action attached).

M.overlay = {
    alpha  = 0,
    color  = { 0, 0, 0, 1 },     -- swap for a non-black fade if needed
    action = nil,
}

local function refreshInTransition()
    if M.overlay.action then return end
    for i = 1, #stack do
        if stack[i].root and stack[i].root.action then return end
    end
    inTransition = false
end

-- Attach the "scene is appearing" action returned by transition.fade /
-- slide / zoom. The scene was already pushed; this just marks it
-- transparent so the layer below renders behind it, hangs the action
-- off root.action, and queues "clear transparent" for completion.
local function attachInAction(scene, action)
    if not scene.root or not action then return end
    scene.transparent = true
    scene.root.action = action
    scene.root.onActionDone = function()
        scene.transparent = false
        refreshInTransition()
    end
    inTransition = true
end

-- Attach the "scene is leaving" action. The scene STAYS in the stack
-- until its action completes — that's when we actually remove it and
-- fire :exit. Marked transparent so the layer below shows through as
-- it fades / slides off.
local function attachOutAction(scene, action)
    if not scene.root or not action then return false end
    scene.transparent = true
    scene.root.action = action
    scene.root.onActionDone = function()
        local i = indexOf(scene)
        if i then table.remove(stack, i) end
        if scene.exit then scene:exit() end
        refreshInTransition()
    end
    inTransition = true
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
    if transition and transition.inActionFn then
        attachInAction(scene, transition.inActionFn(scene))
    end
end

function M.pop(transition)
    local s = stack[#stack]
    if not s then return nil end
    if transition and transition.outActionFn and s.root then
        if attachOutAction(s, transition.outActionFn(s)) then
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
local function overlayReplace(sceneNew, transition)
    local old = stack[#stack]
    local function swap()
        if old then
            local i = indexOf(old)
            if i then table.remove(stack, i) end
            if old.exit then old:exit() end
        end
        stack[#stack + 1] = sceneNew
        if sceneNew.enter then sceneNew:enter() end
    end
    M.overlay.alpha  = 0
    M.overlay.action = transition.overlayActionFn(swap)
    M.overlay.onActionDone = function()
        M.overlay.action = nil
        refreshInTransition()
    end
    inTransition = true
end

-- pop current, push new. With a transition: pushes new on top, runs
-- the in-action on it, and the out-action on the old scene; the old is
-- popped (and :exit fires) when its action completes.
function M.replace(scene, transition)
    if transition and transition.overlayActionFn then
        overlayReplace(scene, transition)
        return
    end

    local old = stack[#stack]
    stack[#stack + 1] = scene
    if scene.enter then scene:enter() end

    if transition and (transition.inActionFn or transition.outActionFn) then
        if transition.inActionFn then
            attachInAction(scene, transition.inActionFn(scene))
        end
        if old then
            if transition.outActionFn and old.root then
                if not attachOutAction(old, transition.outActionFn(old)) then
                    -- root present but action factory returned nil — instant remove
                    local i = indexOf(old)
                    if i then table.remove(stack, i) end
                    if old.exit then old:exit() end
                end
            else
                local i = indexOf(old)
                if i then table.remove(stack, i) end
                if old.exit then old:exit() end
            end
        end
    elseif old then
        -- Instant cut: remove the old scene right away.
        local i = indexOf(old)
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

function M.dispatchUpdate(dt)
    -- Tick the overlay first; it's the action target for overlay-based
    -- transitions and a no-op when no action is attached.
    anim.tickAction(M.overlay, dt)

    if inTransition then
        -- Tick every scene's root so both the leaving and entering
        -- transitions advance in lockstep. Skip the normal scene
        -- :update so per-scene timers (LineEdit cursor blink, score
        -- count-ups, gameplay clocks) don't drift mid-fade.
        for i = 1, #stack do
            local s = stack[i]
            if s.root then anim.tickAction(s.root, dt) end
        end
        return
    end
    local t = top()
    if not t then return end
    -- Tick the root panel's OWN action before delegating, so a scene
    -- that overrides :update (and never reaches root:update) still
    -- gets its root panel animated. The root's children are ticked
    -- inside panel:update via anim.tickAction.
    if t.root then anim.tickAction(t.root, dt) end
    if t.update then t:update(dt)
    elseif t.root and t.root.update then t.root:update(dt) end
end

function M.dispatchRender()
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
    -- drawRegion calls outside the panel tree (menu_bg, blur layers,
    -- etc.) that a root.alpha fade wouldn't reach.
    if M.overlay.alpha > 0 then
        local vw, vh = viewSize()
        local c = M.overlay.color
        local a = M.overlay.alpha
        if a > 1 then a = 1 end
        drawQuad(-vw * 0.5, -vh * 0.5, vw, vh, {
            color = { c[1], c[2], c[3], (c[4] or 1.0) * a },
        })
    end
end

-- Input dispatchers short-circuit while a transition is in flight so
-- stray clicks / keypresses don't land on half-faded buttons.

function M.dispatchKeyDown(name)
    if inTransition then return end
    local t = top()
    if not t then return end
    if t.keyDown then t:keyDown(name)
    elseif t.root and t.root.keyDown then t.root:keyDown(name) end
end

function M.dispatchTextInput(char)
    if inTransition then return end
    local t = top()
    if not t then return end
    if t.textInput then t:textInput(char)
    elseif t.root and t.root.textInput then t.root:textInput(char) end
end

function M.dispatchKeyUp(name)
    if inTransition then return end
    local t = top()
    if t and t.keyUp then t:keyUp(name) end
end

function M.dispatchMouseDown(x, y, button)
    if inTransition then return end
    local t = top()
    if not t then return end
    if t.mouseDown then t:mouseDown(x, y, button)
    elseif t.root and t.root.mouseDown then t.root:mouseDown(x, y, button) end
end

function M.dispatchMouseUp(x, y, button)
    if inTransition then return end
    local t = top()
    if not t then return end
    if t.mouseUp then t:mouseUp(x, y, button)
    elseif t.root and t.root.mouseUp then t.root:mouseUp(x, y, button) end
end

function M.dispatchMouseMove(x, y, dx, dy)
    if inTransition then return end
    local t = top()
    if not t then return end
    if t.mouseMove then t:mouseMove(x, y, dx, dy)
    elseif t.root and t.root.mouseMove then t.root:mouseMove(x, y, dx, dy) end
end

-- ---- Hook installation ------------------------------------------------
-- Convenience for consumers that don't need their own logic in the
-- engine hooks — wires every on_* global to the matching dispatcher.
-- Pass _G (or your env table) explicitly so the function signature
-- isn't surprising.

function M.installHooks(env)
    env = env or _G
    env.onUpdate    = function(dt)     M.dispatchUpdate(dt)    end
    env.onRender    = function()        M.dispatchRender()      end
    env.onKeyDown   = function(name)    M.dispatchKeyDown(name) end
    env.onKeyUp     = function(name)    M.dispatchKeyUp(name)   end
    env.onTextInput = function(ch)      M.dispatchTextInput(ch) end
    env.onMouseDown = function(x, y, b) M.dispatchMouseDown(x, y, b) end
    env.onMouseUp   = function(x, y, b) M.dispatchMouseUp(x, y, b)   end
    env.onMouseMove = function(x, y, dx, dy)
        M.dispatchMouseMove(x, y, dx, dy)
    end
end

return M
