#ifndef SDLFUN_MATH_H
#define SDLFUN_MATH_H

/* Shared math types for the engine. Just type definitions — no operator
   helpers yet. Tight inner loops (rasterizer, lightmap update) keep
   their raw float math; Vec3 is for API boundaries where bundling x/y/z
   makes the call sites readable. Add helpers (sub, length, normalize)
   when a second caller needs them — not preemptively. */

struct Vec2 {
    float u, v;
};

struct Vec3 {
    float x, y, z;
};

#endif
