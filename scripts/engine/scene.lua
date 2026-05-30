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

-- ---- Inspection -------------------------------------------------------

function M.depth() return #stack end
function M.top()   return top() end
function M.empty() return #stack == 0 end

-- ---- Mutation ---------------------------------------------------------

function M.push(scene)
    if type(scene) ~= "table" then
        error("scene.push: expected a table, got " .. type(scene), 2)
    end
    stack[#stack + 1] = scene
    if scene.enter then scene:enter() end
end

function M.pop()
    local s = stack[#stack]
    if not s then return nil end
    stack[#stack] = nil
    if s.exit then s:exit() end
    return s
end

-- pop current, push new — used for transitions where you don't want
-- the previous scene to remain on the stack (e.g. menu → game).
function M.replace(scene)
    M.pop()
    M.push(scene)
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
end

function M.dispatch_keydown(name)
    local t = top()
    if not t then return end
    if t.keydown then t:keydown(name)
    elseif t.root and t.root.keydown then t.root:keydown(name) end
end

function M.dispatch_textinput(char)
    local t = top()
    if not t then return end
    if t.textinput then t:textinput(char)
    elseif t.root and t.root.textinput then t.root:textinput(char) end
end

function M.dispatch_keyup(name)
    local t = top()
    if t and t.keyup then t:keyup(name) end
end

function M.dispatch_mousedown(x, y, button)
    local t = top()
    if not t then return end
    if t.mousedown then t:mousedown(x, y, button)
    elseif t.root and t.root.mousedown then t.root:mousedown(x, y, button) end
end

function M.dispatch_mouseup(x, y, button)
    local t = top()
    if not t then return end
    if t.mouseup then t:mouseup(x, y, button)
    elseif t.root and t.root.mouseup then t.root:mouseup(x, y, button) end
end

function M.dispatch_mousemove(x, y, dx, dy)
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
