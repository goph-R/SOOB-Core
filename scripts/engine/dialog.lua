-- engine/dialog.lua — modal dialog system (widget-based).
--
-- A single dialog at a time. Drops in from above with bounceOut (two visible
-- re-impacts before settling); shoots back up out the top on close with
-- easeInExpo. A separate dim quad behind the dialog fades in during intro and
-- snaps out faster during outro. An "open" sound plays on show(), a "close"
-- sound on close().
--
-- Tree shape:
--
--   root (panel; covers the full canvas)
--     ├─ dim       (fullscreen quad; alpha tweens 0 ↔ dimAlpha)
--     └─ dlg       (panel; y tweens between off-screen and rest)
--          ├─ decor images          (drawn first → tucked under the bg)
--          ├─ background image(s)   (see "Backgrounds" below)
--          ├─ title label
--          └─ widget.button (one per spec.buttons entry)
--
-- ---- Backgrounds ----------------------------------------------------------
--
-- What differs between games is not *which* regions the background uses but
-- **how a requested body height maps to the dialog's total height** — and the
-- total is what the drop animation, the button anchors and the panel bbox all
-- key off. So the plug point is one function:
--
--     build(cfg, dlg, dlgW, requestedH) -> dlgH
--
-- It adds whatever image widgets it likes to `dlg` and returns the total
-- height. Three are built in:
--
--   single      cfg.region            one region stretched to dlgW x dlgH
--   top_bottom  cfg.top, cfg.bottom   clippable body region above a fixed
--                                     base strip; requestedH is the BODY
--                                     height, total = body + base
--   custom      cfg.build             your own function, same signature
--
-- A game picks one once, via setDefaults; a spec may override per dialog.
--
-- ---- Theming --------------------------------------------------------------
--
-- Everything art-specific lives in one defaults table, set once at startup:
--
--   dialog.setDefaults{
--       width      = 460,
--       background = { kind = "top_bottom", top = "bg_top", bottom = "bg_bot" },
--       decor      = { { region = "chain", xFrac = 0.2, overlap = 10 } },
--       sounds     = { open = "dialog_open", close = "dialog_close" },
--       buttons    = { up = "button_up", down = "button_down",
--                      hover = "button_hover", w = 200, h = 48 },
--       title      = { font = "large", y = 40, color = { 1, 1, 1 } },
--       timing     = { open = 0.70, close = 0.40, dimIn = 0.30, dimOut = 0.20,
--                      startOffset = 720, dimAlpha = 0.88 },
--   }
--
-- The defaults below ARE working values, so a game that calls nothing gets a
-- usable dialog as soon as it registers a "dialog_bg" region. Unknown sound
-- and region names only warn (soundPlay / drawRegion are tolerant), so a
-- half-dressed game still runs.
--
-- ---- Spec fields ----------------------------------------------------------
--   title          string
--   appearSound / closeSound   override the themed sounds
--   w              dialog width (default: theme width)
--   height         body height, interpreted by the background kind
--   background     per-dialog background override
--   buttons        array of { x, y, w, h, label, action | replace, skipOutro }
--                  positions in dialog-local coords (origin = dialog CENTER).
--                  `action`   — fired AFTER the outro completes.
--                  `replace`  — spec (or factory fn) for a follow-up dialog;
--                               hands off without fading the dim. Takes
--                               precedence over `action`.
--                  `skipOutro`— fire `action` IMMEDIATELY, no outro, no dim
--                               fade; the dialog keeps rendering at rest.
--                               Pair with dialog.dismiss() at cleanup.
--   buttonsStartHidden  buttons start invisible until revealButtons()
--   drawBody       function(introDone, t, anchorX, anchorY) — called every
--                  frame after the tree draws; anchor is the dialog's CENTER
--                  on screen, including the animation offset.
--
-- Input contract: while isActive(), the host should route handleMouseDown /
-- handleMouseUp / handleMouseMove for every mouse event — buttons need both
-- press AND release to complete a click, and mouseMove drives hover. Events
-- outside STATE_OPEN are swallowed (no input during intro / outro).

local widget = require "engine.widget"
local anim   = require "engine.animation"

local M = {}

-- ---- Animation phases ----------------------------------------------------
local STATE_INTRO = "intro"
local STATE_OPEN  = "open"
local STATE_OUTRO = "outro"

-- ---- Theme ---------------------------------------------------------------

local D = {
    width      = 460,
    background = { kind = "single", region = "dialog_bg" },
    decor      = {},
    sounds     = { open = "dialog_open", close = "dialog_close" },
    buttons    = { up = "button_up", down = "button_down",
                   hover = "button_hover", w = 200, h = 48 },
    title      = { font = "large", y = 40, color = { 1, 1, 1 } },
    timing     = { open        = 0.70,   -- drop-in until rest
                   close       = 0.40,   -- shoot-up until off-screen
                   dimIn       = 0.30,   -- dim ramps up alongside the drop
                   dimOut      = 0.20,   -- dim snaps out faster than the dialog
                   startOffset = 720,    -- px above rest where the drop starts
                   dimAlpha    = 0.88 }, -- target dim once intro settles
}

-- One level of merge: setDefaults{ timing = { open = 1.0 } } overrides only
-- `open` and leaves the rest of the timing block alone. `decor` is replaced
-- wholesale — it is a list, so merging keys would be meaningless.
function M.setDefaults(t)
    for k, v in pairs(t) do
        if type(v) == "table" and type(D[k]) == "table" and k ~= "decor" then
            for k2, v2 in pairs(v) do D[k][k2] = v2 end
        else
            D[k] = v
        end
    end
end

-- ---- Backgrounds ---------------------------------------------------------

local BACKGROUNDS = {}

-- One region stretched to the full dialog rect.
BACKGROUNDS.single = function(cfg, dlg, dlgW, requestedH)
    local _, nativeH = regionSize(cfg.region)
    local dlgH = requestedH or nativeH
    dlg:add(widget.image{
        x = 0, y = 0, region = cfg.region,
        width = dlgW, height = dlgH,
    })
    return dlgH
end

-- A clippable body region above a fixed base strip. `requestedH` is the BODY
-- height: capped at the body region's native height (above that there is no
-- pattern to extend) and clipped from the bottom via fillY, so both source
-- UVs and destination pixels shrink in lockstep — the same mechanism a
-- progress bar uses for fillX. The base strip is never clipped, so the total
-- is body + base.
BACKGROUNDS.top_bottom = function(cfg, dlg, dlgW, requestedH)
    local _, topNativeH    = regionSize(cfg.top)
    local _, bottomNativeH = regionSize(cfg.bottom)
    local topH = requestedH or topNativeH
    if topH > topNativeH then topH = topNativeH end

    dlg:add(widget.image{
        x = 0, y = 0, region = cfg.top,
        width = dlgW, height = topNativeH,
        fillY = topH / topNativeH,
    })
    dlg:add(widget.image{
        x = 0, y = topH, region = cfg.bottom,
        width = dlgW, height = bottomNativeH,
    })
    return topH + bottomNativeH
end

-- Escape hatch for anything the two built-ins don't cover (a nine-patch, a
-- tiled fill, a parallax stack): supply cfg.build with the same signature.
BACKGROUNDS.custom = function(cfg, dlg, dlgW, requestedH)
    return cfg.build(dlg, dlgW, requestedH)
end

-- ---- Internal state ------------------------------------------------------
local current        = nil    -- { spec, state, t, root, dlg, dim, buttons }
local pendingAction  = nil    -- latched in onClick, fires after outro
local pendingReplace = nil    -- spec to show next; dim stays at full

-- ---- Tree construction ---------------------------------------------------

local function buildTree(spec)
    local vw, vh = viewSize()
    local dlgW   = spec.w or D.width

    local root = widget.panel{ x = 0, y = 0, width = vw, height = vh }

    -- Dim: black fullscreen quad. alpha tweens 0 → dimAlpha on intro, back to
    -- 0 on outro. Independent of the dlg panel's transform.
    local dim = widget.quad{
        x = -vw * 0.5, y = -vh * 0.5,
        width = vw, height = vh,
        color = { 0, 0, 0, 1 },
    }
    dim.alpha = 0
    root:add(dim)

    -- The dialog panel uses TOP-LEFT origin so its bbox encloses the buttons
    -- (click-to-focus needs the bbox to contain hit children). Decor hanging
    -- above the bbox at negative local y is draw-only, so the bbox doesn't
    -- matter for it. y is set once dlgH is known, below.
    local dlg = widget.panel{ x = -dlgW * 0.5, y = 0, width = dlgW, height = 0 }

    -- Decor — added FIRST so the background occludes whatever overlaps the
    -- dialog's top edge (z-order does the tuck). Each entry is centred at
    -- `xFrac` across the dialog and hangs above it, with `overlap` px of its
    -- bottom tucked under the top edge so it visibly anchors into the
    -- background instead of just touching.
    for _, d in ipairs(spec.decor or D.decor) do
        local dw, dh = regionSize(d.region)
        dlg:add(widget.image{
            x = dlgW * (d.xFrac or 0.5) - dw * 0.5,
            y = (d.overlap or 0) - dh,
            region = d.region,
        })
    end

    local bg   = spec.background or D.background
    local kind = BACKGROUNDS[bg.kind] or BACKGROUNDS.single
    local dlgH = kind(bg, dlg, dlgW, spec.height)

    dlg.height = dlgH
    local restY  = -dlgH * 0.5
    local startY = restY - D.timing.startOffset
    dlg.y = startY

    -- Title — visible from frame 1 of the intro.
    if spec.title then
        dlg:add(widget.label{
            x = dlgW * 0.5, y = D.title.y,
            width = 0, height = 0,
            text  = spec.title,
            font  = D.title.font,
            align = ALIGN_CENTER + ALIGN_MIDDLE,
            color = D.title.color,
        })
    end

    -- Buttons. spec.buttons[i].x/y are CENTER offsets from the dialog center;
    -- translate to panel-local top-left coords. The onClick closure either:
    --   * `replace` set → hand off to M.replace, no dim fade between dialogs
    --     (a function-form replace resolves lazily so two specs can
    --     cross-reference each other without an init cycle);
    --   * else → latch `action` into pendingAction and start a normal close;
    --     dlg.onActionDone fires the action once the outro lands.
    -- Collected so M.revealButtons can animate them in later.
    local buttons = {}
    for _, btn in ipairs(spec.buttons or {}) do
        local bw    = btn.w or D.buttons.w
        local bh    = btn.h or D.buttons.h
        local cx    = btn.x or 0
        local cy    = btn.y or 0
        local act   = btn.action
        local repl  = btn.replace
        local skipo = btn.skipOutro
        local b = widget.button{
            x = dlgW * 0.5 + cx - bw * 0.5,
            y = dlgH * 0.5 + cy - bh * 0.5,
            width = bw, height = bh,
            bgUp      = D.buttons.up,
            bgDown    = D.buttons.down,
            bgHover   = D.buttons.hover,
            text      = btn.label,
            textAlign = ALIGN_CENTER + ALIGN_MIDDLE,
            onClick   = function()
                if repl then
                    local nextSpec = type(repl) == "function"
                                      and repl() or repl
                    M.replace(nextSpec)
                elseif skipo and act then
                    -- Skip the outro: fire the action right now. The dialog
                    -- stays rendered at rest until the action's downstream
                    -- effect (typically a scene fade) covers it; the host
                    -- scene should call dialog.dismiss() from its :exit so
                    -- the dialog state doesn't leak.
                    act()
                else
                    pendingAction = act
                    M.close()
                end
            end,
        }
        -- Dialogs are mouse-driven; pressing Enter / Space anywhere shouldn't
        -- fire arbitrary dialog buttons.
        b.focusable = false
        if spec.buttonsStartHidden then b.visible = false end
        buttons[#buttons + 1] = b
        dlg:add(b)
    end

    root:add(dlg)
    return root, dlg, dim, buttons
end

-- ---- Animation kickoff ---------------------------------------------------

-- skipDimFade = true when handing off from a previous dialog. The dim already
-- sits at dimAlpha from the outgoing dialog and we want it to stay there —
-- fading it back up would look like a flicker.
local function startIntro(c, skipDimFade)
    if not skipDimFade then
        c.dim.action = anim.fadeTo(D.timing.dimAlpha, D.timing.dimIn)
    end
    c.dlg.action = anim.moveTo(c.dlg.x, -(c.dlg.height * 0.5),
                               D.timing.open, anim.bounceOut)
    c.dlg.onActionDone = function(self)
        c.state = STATE_OPEN
        c.t     = 0
    end
end

-- When pendingReplace is set we leave the dim at full alpha — the next
-- dialog's intro will be told to skip its own fade-in so the dim stays
-- visually continuous across the hand-off.
local function startOutro(c)
    local offY = -(c.dlg.height * 0.5) - D.timing.startOffset
    if not pendingReplace then
        c.dim.action = anim.fadeTo(0.0, D.timing.dimOut)
    end
    c.dlg.action = anim.moveTo(c.dlg.x, offY,
                               D.timing.close, anim.easeInExpo)
    c.dlg.onActionDone = function(self)
        if pendingReplace then
            local nextSpec = pendingReplace
            pendingReplace = nil
            M.show(nextSpec, { skipDimFade = true })
        else
            current = nil
            if pendingAction then
                local a = pendingAction
                pendingAction = nil
                a()
            end
        end
    end
end

-- ---- Public API ----------------------------------------------------------

function M.isActive()
    return current ~= nil
end

-- opts.skipDimFade — start the dim at full alpha and skip the intro fade.
-- Used by the replace path so the dim doesn't flicker between dialogs.
function M.show(spec, opts)
    opts = opts or {}
    local root, dlg, dim, buttons = buildTree(spec)
    if opts.skipDimFade then dim.alpha = D.timing.dimAlpha end
    current = {
        spec    = spec,
        state   = STATE_INTRO,
        t       = 0,
        root    = root,
        dlg     = dlg,
        dim     = dim,
        buttons = buttons,
    }
    startIntro(current, opts.skipDimFade)
    soundPlay(spec.appearSound or D.sounds.open)
end

function M.close()
    if not current or current.state == STATE_OUTRO then return end
    current.state = STATE_OUTRO
    current.t     = 0
    startOutro(current)
    soundPlay(current.spec.closeSound or D.sounds.close)
end

-- Swap the current dialog for nextSpec without dropping the dim. The current
-- dialog runs its normal outro, then on completion startOutro's onActionDone
-- detects pendingReplace and shows the new spec with skipDimFade. If no
-- dialog is active, falls back to a plain show().
function M.replace(nextSpec)
    if not current then
        M.show(nextSpec)
        return
    end
    pendingReplace = nextSpec
    M.close()
end

-- Clear all dialog state with no animation. Used when something else is
-- taking over the visual hand-off (a scene fade, e.g.) and we want the dialog
-- to vanish without playing its outro under the new effect. Pair with a
-- button's `skipOutro = true`: skipOutro fires the action right away and
-- leaves the dialog visible; the action's downstream (scene transition)
-- covers it, and the host calls dismiss() at the right cleanup moment —
-- usually scene:exit on the leaving scene.
function M.dismiss()
    current        = nil
    pendingAction  = nil
    pendingReplace = nil
end

-- Animate the dialog's buttons in (alpha 0→1, scale 0.7→1.0 with a small
-- overshoot). For specs built with buttonsStartHidden = true that defer the
-- button until a body animation finishes — e.g. a "COMPLETED" dialog that
-- only offers "Next level >" once the score breakdown has counted up.
-- Idempotent-ish: safe to gate behind a one-shot flag in the caller.
function M.revealButtons()
    if not current or not current.buttons then return end
    for _, b in ipairs(current.buttons) do
        b.visible = true
        b.alpha   = 0
        b.scale   = 0.7
        b.action  = anim.parallel{
            anim.fadeIn(0.25),
            anim.scaleTo(1.0, 0.35, anim.easeOutBack),
        }
    end
end

function M.update(dt)
    if not current then return end
    current.t = current.t + dt
    current.root:update(dt)
end

function M.render()
    if not current then return end
    current.root:draw()
    -- Body content — the spec drives whatever else appears. introDone lets
    -- bodies stagger sub-animations after the dialog settles. The anchor
    -- passed in is the dialog's animated CENTER (not the TL).
    if current.spec.drawBody then
        local ax = 0
        local ay = current.dlg.y + current.dlg.height * 0.5
        current.spec.drawBody(current.state ~= STATE_INTRO,
                              current.t, ax, ay)
    end
end

-- ---- Input forwarding ----------------------------------------------------
--
-- Modal: every mouse event is swallowed. Buttons only respond while the
-- dialog is at rest (STATE_OPEN); intro / outro frames freeze input so a
-- click during the drop doesn't fire a button before the player can see it
-- land.

local function inputAlive()
    return current and current.state == STATE_OPEN
end

function M.handleMouseDown(x, y, button)
    if not current then return false end
    if inputAlive() then
        current.root:mouseDown(x, y, button)
    end
    return true   -- swallow either way
end

function M.handleMouseUp(x, y, button)
    if not current then return false end
    if inputAlive() then
        current.root:mouseUp(x, y, button)
    end
    return true
end

function M.handleMouseMove(x, y, dx, dy)
    if not current then return false end
    if inputAlive() then
        current.root:mouseMove(x, y, dx, dy)
    end
    return true
end

return M
