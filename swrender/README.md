# SOOB-Core software rasterizer

A pure-CPU 2D backend for the **no-3D-accelerator** path (Pentium-class Win98).
**Wired into Find5** and selected at runtime by `config.lua`'s `display.render`
(`"opengl"` | `"software"`); the standalone benchmark here stays for measuring
raw fill-rate on real hardware.

## Selecting it (Find5)

```lua
-- config.lua
display = { width = 640, height = 480, render = "software" }
```
or pass `-software` / `-opengl` on the command line. Find5 builds with
`-DSOOB_SOFTWARE_BACKEND` so both backends are compiled in; the flag is what
makes the runtime switch available. Other consumers (SOOB-Engine, SOOB-Core-Web)
don't define the macro, so they compile exactly as before — pure GL.

## How it's wired (the seam)

Runtime branch on `g_renderMode`, set once at startup from `cfg.render`:

| Spot | GL path | Software path |
|---|---|---|
| `texture.h::uploadTextureN` | `glTexImage2D` | `swTexAlloc` (pixels stay resident) |
| `ui.h::uiInit` 8×8 atlas | GL upload | `swTexAlloc` |
| `ui.h::uiBegin/uiEnd` | ortho + GL state | publish virtual→device transform |
| `ui.h::uiQuad` | `GL_QUADS` | `swFillRect` |
| `ui.h::uiIcon/uiIconUV/uiIconUVColor` | textured `GL_QUADS` | `swBlitTex` |
| `ui.h::uiText` (per glyph) | textured `GL_QUADS` | `swBlitTex` (`uiTextSoft`) |
| `Find5/main.cpp` video init | `SDL_OPENGL` ctx | `SDL_SWSURFACE` + `SwCanvas` |
| `Find5/main.cpp` per frame | `glClear` / `SDL_GL_SwapBuffers` | `swCanvasClear` / `swPresent` |

All of `script.h`'s `drawRegion` geometry sits above these and is reused verbatim.

## Files

| File | What it is |
|---|---|
| `render_soft.h` | The rasterizer + runtime mode/backbuffer/transform globals: `swFillRect`, `swBlitTex` (nearest scale + constant RGBA modulate + SRC_ALPHA blend), `SwTexture` registry. SDL-free, GL-free, C++98. |
| `sw_demo.cpp` | Standalone benchmark. Builds a Find5-shaped frame and times raster vs present — for measuring on real hardware. |

## Build & run

```sh
# Headless — raster only, no SDL, runs anywhere. Cleanest raw fill-rate.
g++ -O2 -DHEADLESS -o sw_demo_hl sw_demo.cpp && ./sw_demo_hl

# Windowed — also measures present (the system->VRAM copy, the P166 bottleneck).
# Needs SDL 1.2 (use the same vendored SDL the engine links).
g++ -O2 -o sw_demo sw_demo.cpp `sdl-config --cflags --libs` && ./sw_demo
```

On the **target P166** (or 86Box), run the windowed build — the `raster X ms +
present Y ms` split is the number that decides the project. Tune `SPRITES` /
`GLYPHS` / `FRAMES` in `sw_demo.cpp` to match your busiest real screen.

## How the frame maps to Find5

- 1× full-screen 640×480 **opaque** blit = `drawBg` at native size → the Tier-1
  per-row `memcpy` path (no resample). This is why authoring backgrounds at
  native 640×480 matters.
- N× 64×64 **alpha** sprites = highlights / cursor / found-markers → Tier-3 blend.
- M× 8×8 **alpha** quads = text glyphs → Tier-3 blend, small.

## Coverage / what's still stubbed

| Primitive | Status in software mode |
|---|---|
| `drawQuad` (`swFillRect`) | **done** |
| `drawRegion` non-rotated, `drawBg`, glyph blit, `uiText` (`swBlitTex`) | **done** |
| `drawEllipse` (`swEllipse`) | **done** — per-pixel AA arc ribbon |
| `drawBlur` (`swBlurUpscaleGet` + `uiBlurFit`) | **done** — bilinear upscale, cached per (source, size); 1:1 alpha-blit each frame |
| 16bpp RGB565 backbuffer | **done** — `display.depth = 16`; native 565 compositing (split-channel blend), present is a straight memcpy |
| `drawRegion` rotation (`uiIconUVColorRot`) | **falls back to axis-aligned** (rotation ignored) — TODO: inverse-affine sampler |

## Remaining work

1. **`swBlitTexRot`** — inverse-affine sampler so spinning sprites actually rotate
   (the one remaining stub; deprioritized — not important for Find5).
2. **Perf:** dirty-rect (a static spot-the-diff frame then costs ~0) slots in
   behind the same `swPresent` signature.

## Reading the result (P166 extrapolation)

Per-pixel cost, not fps, is what transfers. The dominant terms per frame:
- Background: 307k px as a `memcpy` → **bandwidth-bound**. At 32bpp that's ~2.4 MB
  of traffic; on a P166's ~150–250 MB/s effective RAM bandwidth ≈ **10–16 ms**.
  16bpp roughly halves it. Dirty-rect skips it entirely on a static frame.
- Blended px (sprites + glyphs): each Tier-3 pixel is a handful of `sw_mul255`
  ops, ~15–25 P5 cycles. ~200k blended px ≈ 3–5M cycles ≈ **18–30 ms**.

So a *busy* frame (this synthetic 40 sprites + 600 glyphs) lands roughly in the
**15–25 fps** range on a P166 once you add present — and a real idle
spot-the-diff screen is far lighter than that, which is exactly why the genre is
feasible. Confirm with the windowed build on real silicon before porting the
TODO primitives.
