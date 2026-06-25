#ifndef RENDER_SOFT_H
#define RENDER_SOFT_H
/* ---------------------------------------------------------------------------
 * SOOB-Core — software 2D rasterizer (no-3D-accelerator path)
 *
 * Pure CPU rasterizer. SDL-free and GL-free: it composites into a flat
 * backbuffer (the SwCanvas) that main.cpp presents via SDL_Flip.
 *
 * Backbuffer is 16bpp (RGB565) or 32bpp (0xAARRGGBB), chosen at init from
 * config.lua's display.depth. 16bpp halves both the composite-write and the
 * present (system->VRAM) bandwidth that bottlenecks a P166 — and because the
 * canvas IS 565, present is a straight memcpy with no per-pixel pack cost.
 * Source textures always stay 32bpp RGBA (sampled, need alpha + colour).
 *
 * Primitives:
 *   swFillRect  — flat colour rect + alpha                  (drawQuad)
 *   swBlitTex   — axis-aligned nearest blit, sub-rect, RGBA
 *                 modulate, SRC_ALPHA blend                 (drawRegion/Bg/glyphs)
 *   swEllipse   — anti-aliased arc ribbon                   (drawEllipse)
 *   swBlurUpscaleGet — bilinear upscale of a tiny texture,
 *                 cached, for the blurred backdrop          (drawBlur)
 * Not yet: rotation (uiIconUVColorRot falls back to axis-aligned).
 *
 * C++98, no C++11. Matches SOOB-Core's header-only `static` style.
 * ------------------------------------------------------------------------- */

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- pixel helpers ---- */

/* Fast (a*b)/255 for a,b in 0..255 — no divide (a real div is ~20+ cycles on a
   P5, and this runs per channel per pixel). */
static unsigned sw_mul255(unsigned a, unsigned b)
{
    unsigned t = a * b + 0x80u;
    return ((t >> 8) + t) >> 8;
}

/* 32bpp 0xAARRGGBB */
#define SW_A(p) (((p) >> 24) & 0xFFu)
#define SW_R(p) (((p) >> 16) & 0xFFu)
#define SW_G(p) (((p) >>  8) & 0xFFu)
#define SW_B(p) ( (p)        & 0xFFu)
#define SW_ARGB(a,r,g,b) \
    (((unsigned)(a) << 24) | ((unsigned)(r) << 16) | ((unsigned)(g) << 8) | (unsigned)(b))

/* 16bpp RGB565. Pack takes 8-bit channels; unpack expands back to 8-bit. */
#define SW565(r,g,b) ((unsigned short)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))
static unsigned sw565R(unsigned short p) { unsigned v = (p >> 11) & 0x1Fu; return (v << 3) | (v >> 2); }
static unsigned sw565G(unsigned short p) { unsigned v = (p >>  5) & 0x3Fu; return (v << 2) | (v >> 4); }
static unsigned sw565B(unsigned short p) { unsigned v =  p        & 0x1Fu; return (v << 3) | (v >> 2); }

/* ---- canvas (the backbuffer) ---- */

typedef struct {
    void *px;       /* w*h pixels; unsigned (32) or unsigned short (16) */
    int w, h;
    int bpp;        /* 16 or 32 */
} SwCanvas;

static int swCanvasInit(SwCanvas *c, int w, int h, int bpp)
{
    c->w = w; c->h = h; c->bpp = (bpp == 16) ? 16 : 32;
    c->px = malloc((size_t)w * h * (c->bpp / 8));
    return c->px != 0;
}
static void swCanvasFree(SwCanvas *c) { free(c->px); c->px = 0; }

static void swCanvasClear(SwCanvas *c, unsigned argb)
{
    int n = c->w * c->h;
    if (c->bpp == 16) {
        unsigned short v = SW565(SW_R(argb), SW_G(argb), SW_B(argb));
        unsigned short *p = (unsigned short *)c->px;
        for (int i = 0; i < n; i++) p[i] = v;
    } else {
        unsigned *p = (unsigned *)c->px;
        for (int i = 0; i < n; i++) p[i] = argb;
    }
}

/* ---- backend mode + the live backbuffer ----
 *
 * Runtime-selected by config.lua's display.render. Both backends are compiled
 * in (Find5 builds with -DSOOB_SOFTWARE_BACKEND); ui.h / texture.h branch on
 * g_renderMode, which main.cpp sets ONCE at startup before any draw or upload. */
enum { RENDER_MODE_OPENGL = 0, RENDER_MODE_SOFTWARE = 1 };
static int      g_renderMode = RENDER_MODE_OPENGL;
static SwCanvas g_swCanvas = { 0, 0, 0, 0 };   /* the frame backbuffer, owned by main */

/* Virtual-canvas -> device-pixel transform. The engine draws in a center-origin,
   Y-down virtual canvas (ui.h uiBegin: glOrtho(-halfW,+halfW,+halfH,-halfH)).
   uiBegin sets these each frame: device = (virtual + half) * scale. scale == 1 at
   the fixed 640x480 / no-stretch target. */
static float g_swHalfW = 0.0f, g_swHalfH = 0.0f, g_swScale = 1.0f;

static int swDevX(float vx)  { return (int)((vx + g_swHalfW) * g_swScale + 0.5f); }
static int swDevY(float vy)  { return (int)((vy + g_swHalfH) * g_swScale + 0.5f); }
static int swDevLen(float n) { return (int)(n * g_swScale + 0.5f); }
static unsigned swByte(float f)
{
    int v = (int)(f * 255.0f + 0.5f);
    return (unsigned)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* ---- software texture registry ----
 *
 * The engine threads a `GLuint` texture handle everywhere; the software backend
 * keeps that type but reinterprets it as a 1-based index into this table. In the
 * real port, texture.h::uploadTextureN copies decoded RGBA here (and the pixels
 * stay resident) instead of calling glTexImage2D. Always 32bpp RGBA. */

#define SW_TEX_MAX 256
typedef struct {
    unsigned *px;     /* w*h, straight RGBA in 0xAARRGGBB */
    int w, h;
    int opaque;       /* 1 if every alpha == 255 -> enables the memcpy fast path */
    int used;
} SwTexture;

typedef struct { SwTexture t[SW_TEX_MAX]; int count; } SwTexReg;
static SwTexReg g_swTex;

/* Register an RGBA8 image (channels: 3 or 4). Returns handle >=1 (0 = invalid,
   matching GL's "0 is no texture"). `src` is copied; caller keeps ownership. */
static unsigned swTexAlloc(const unsigned char *src, int w, int h, int channels)
{
    if (g_swTex.count >= SW_TEX_MAX) return 0;
    int id = ++g_swTex.count;
    SwTexture *t = &g_swTex.t[id - 1];
    t->px = (unsigned *)malloc((size_t)w * h * sizeof(unsigned));
    if (!t->px) { g_swTex.count--; return 0; }
    t->w = w; t->h = h; t->used = 1; t->opaque = 1;
    int n = w * h;
    for (int i = 0; i < n; i++) {
        unsigned r = src[(size_t)i * channels + 0];
        unsigned g = src[(size_t)i * channels + 1];
        unsigned b = src[(size_t)i * channels + 2];
        unsigned a = (channels == 4) ? src[(size_t)i * channels + 3] : 255u;
        if (a != 255u) t->opaque = 0;
        t->px[i] = SW_ARGB(a, r, g, b);
    }
    return (unsigned)id;
}
static SwTexture *swTexGet(unsigned id)
{
    if (id == 0 || id > (unsigned)g_swTex.count) return 0;
    return &g_swTex.t[id - 1];
}

/* ---- single-pixel blend (used by swEllipse; canvas-format aware) ---- */
static void swBlendPixel(SwCanvas *c, int x, int y,
                         unsigned r, unsigned g, unsigned b, unsigned a)
{
    if (a == 0 || x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    if (c->bpp == 16) {
        unsigned short *d = (unsigned short *)c->px + (size_t)y * c->w + x;
        if (a == 255) { *d = SW565(r, g, b); return; }
        unsigned short dp = *d; unsigned ia = 255u - a;
        unsigned dr = sw565R(dp), dg = sw565G(dp), db = sw565B(dp);
        *d = SW565(sw_mul255(r, a) + sw_mul255(dr, ia),
                   sw_mul255(g, a) + sw_mul255(dg, ia),
                   sw_mul255(b, a) + sw_mul255(db, ia));
    } else {
        unsigned *d = (unsigned *)c->px + (size_t)y * c->w + x;
        if (a == 255) { *d = SW_ARGB(255, r, g, b); return; }
        unsigned dp = *d; unsigned ia = 255u - a;
        *d = SW_ARGB(255, sw_mul255(r, a) + sw_mul255(SW_R(dp), ia),
                          sw_mul255(g, a) + sw_mul255(SW_G(dp), ia),
                          sw_mul255(b, a) + sw_mul255(SW_B(dp), ia));
    }
}

/* ---- flat rect (drawQuad) ---- */
static void swFillRect(SwCanvas *c, int x, int y, int w, int h,
                       unsigned cr, unsigned cg, unsigned cb, unsigned ca)
{
    if (ca == 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return;
    unsigned ia = 255u - ca;

    for (int row = 0; row < h; row++) {
        if (c->bpp == 16) {
            unsigned short *d = (unsigned short *)c->px + (size_t)(y + row) * c->w + x;
            if (ca == 255) {
                unsigned short v = SW565(cr, cg, cb);
                for (int i = 0; i < w; i++) d[i] = v;
            } else {
                for (int i = 0; i < w; i++) {
                    unsigned short dp = d[i];
                    d[i] = SW565(sw_mul255(cr, ca) + sw_mul255(sw565R(dp), ia),
                                 sw_mul255(cg, ca) + sw_mul255(sw565G(dp), ia),
                                 sw_mul255(cb, ca) + sw_mul255(sw565B(dp), ia));
                }
            }
        } else {
            unsigned *d = (unsigned *)c->px + (size_t)(y + row) * c->w + x;
            if (ca == 255) {
                unsigned v = SW_ARGB(255, cr, cg, cb);
                for (int i = 0; i < w; i++) d[i] = v;
            } else {
                for (int i = 0; i < w; i++) {
                    unsigned dp = d[i];
                    d[i] = SW_ARGB(255, sw_mul255(cr, ca) + sw_mul255(SW_R(dp), ia),
                                        sw_mul255(cg, ca) + sw_mul255(SW_G(dp), ia),
                                        sw_mul255(cb, ca) + sw_mul255(SW_B(dp), ia));
                }
            }
        }
    }
}

/* ---- textured blit (drawRegion / drawBg / glyphs) ----
 * Axis-aligned, nearest. Source span as UVs (as uiIconUVColor gets them); dest
 * rect in integer pixels; (cr,cg,cb,ca) = glColor4f under GL_MODULATE in 0..255. */
static void swBlitTex(SwCanvas *c, unsigned texId,
                      int dx, int dy, int dw, int dh,
                      float u0, float v0, float u1, float v1,
                      unsigned cr, unsigned cg, unsigned cb, unsigned ca)
{
    SwTexture *t = swTexGet(texId);
    if (!t || dw <= 0 || dh <= 0 || ca == 0) return;

    int su0 = (int)(u0 * t->w * 65536.0f);
    int sv0 = (int)(v0 * t->h * 65536.0f);
    int sustep = (int)(((u1 - u0) * t->w * 65536.0f) / dw);
    int svstep = (int)(((v1 - v0) * t->h * 65536.0f) / dh);

    if (dx < 0) { su0 += sustep * (-dx); dw += dx; dx = 0; }
    if (dy < 0) { sv0 += svstep * (-dy); dh += dy; dy = 0; }
    if (dx + dw > c->w) dw = c->w - dx;
    if (dy + dh > c->h) dh = c->h - dy;
    if (dw <= 0 || dh <= 0) return;

    int white    = (cr == 255 && cg == 255 && cb == 255);
    int oneToOne = (sustep == 65536 && svstep == 65536);
    int bpp16    = (c->bpp == 16);

    int sv = sv0;
    for (int row = 0; row < dh; row++) {
        int syi = sv >> 16;
        if (syi < 0) syi = 0; else if (syi >= t->h) syi = t->h - 1;
        const unsigned *srow = &t->px[(size_t)syi * t->w];
        int su = su0;

        if (!bpp16) {
            unsigned *d = (unsigned *)c->px + (size_t)(dy + row) * c->w + dx;
            if (t->opaque && ca == 255 && white && oneToOne) {
                int sxi = su0 >> 16; if (sxi < 0) sxi = 0;
                memcpy(d, &srow[sxi], (size_t)dw * sizeof(unsigned));   /* fast path */
            } else {
                for (int col = 0; col < dw; col++) {
                    int sxi = su >> 16;
                    if (sxi < 0) sxi = 0; else if (sxi >= t->w) sxi = t->w - 1;
                    unsigned sp = srow[sxi]; su += sustep;
                    unsigned sa = SW_A(sp);
                    if (ca != 255) sa = sw_mul255(sa, ca);
                    if (sa == 0) continue;
                    unsigned sr = SW_R(sp), sg = SW_G(sp), sb = SW_B(sp);
                    if (!white) { sr = sw_mul255(sr, cr); sg = sw_mul255(sg, cg); sb = sw_mul255(sb, cb); }
                    if (sa == 255) { d[col] = SW_ARGB(255, sr, sg, sb); }
                    else {
                        unsigned dp = d[col], ia = 255u - sa;
                        d[col] = SW_ARGB(255, sw_mul255(sr, sa) + sw_mul255(SW_R(dp), ia),
                                              sw_mul255(sg, sa) + sw_mul255(SW_G(dp), ia),
                                              sw_mul255(sb, sa) + sw_mul255(SW_B(dp), ia));
                    }
                }
            }
        } else {
            unsigned short *d = (unsigned short *)c->px + (size_t)(dy + row) * c->w + dx;
            for (int col = 0; col < dw; col++) {
                int sxi = su >> 16;
                if (sxi < 0) sxi = 0; else if (sxi >= t->w) sxi = t->w - 1;
                unsigned sp = srow[sxi]; su += sustep;
                unsigned sa = SW_A(sp);
                if (ca != 255) sa = sw_mul255(sa, ca);
                if (sa == 0) continue;
                unsigned sr = SW_R(sp), sg = SW_G(sp), sb = SW_B(sp);
                if (!white) { sr = sw_mul255(sr, cr); sg = sw_mul255(sg, cg); sb = sw_mul255(sb, cb); }
                if (sa == 255) { d[col] = SW565(sr, sg, sb); }
                else {
                    unsigned short dp = d[col]; unsigned ia = 255u - sa;
                    unsigned dr = sw565R(dp), dg = sw565G(dp), db = sw565B(dp);
                    d[col] = SW565(sw_mul255(sr, sa) + sw_mul255(dr, ia),
                                   sw_mul255(sg, sa) + sw_mul255(dg, ia),
                                   sw_mul255(sb, sa) + sw_mul255(db, ia));
                }
            }
        }
        sv += svstep;
    }
}

/* ---- anti-aliased arc ribbon (drawEllipse) ----
 *
 * Per-pixel implicit-ellipse rasterizer matching ui.h's GL ribbon: a stroke of
 * half-width `half` (device px) centred on the perimeter (x/rx)^2+(y/ry)^2 = 1,
 * spanning the perimeter fraction [startPct, endPct] (0 = +X, CCW). Distance to
 * the perimeter uses the gradient of the implicit function, so it tracks the same
 * (cos/rx, sin/ry) normal the GL path uses. atan2 (the angular test) is gated
 * behind the coverage test, so only the thin ribbon band pays for it. */
static void swEllipse(SwCanvas *c, float cx, float cy, float rx, float ry,
                      float startPct, float endPct, float half,
                      unsigned cr, unsigned cg, unsigned cb, unsigned ca)
{
    if (rx < 0.5f || ry < 0.5f || ca == 0) return;
    const float TAU = 6.28318530717958647692f;
    float minR = (rx < ry) ? rx : ry;
    /* normalized-radius band that can possibly be covered (+1px AA margin) */
    float margin = (half + 1.0f) / minR;
    float gLo = 1.0f - margin, gHi = 1.0f + margin;
    if (gLo < 0.0f) gLo = 0.0f;

    int x0 = (int)(cx - rx - half - 1.0f), x1 = (int)(cx + rx + half + 1.0f);
    int y0 = (int)(cy - ry - half - 1.0f), y1 = (int)(cy + ry + half + 1.0f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    if (y1 >= c->h) y1 = c->h - 1;

    for (int py = y0; py <= y1; py++) {
        float uy = (py + 0.5f - cy) / ry;
        for (int px = x0; px <= x1; px++) {
            float ux = (px + 0.5f - cx) / rx;
            float f = ux * ux + uy * uy;
            float g = sqrtf(f);
            if (g < gLo || g > gHi || g < 1e-4f) continue;   /* cheap reject */

            /* perpendicular distance to the perimeter, in device px */
            float gx = ux / rx, gy = uy / ry;
            float gradLen = sqrtf(gx * gx + gy * gy) / g;     /* |grad of g| */
            if (gradLen < 1e-6f) continue;
            float dist = (g - 1.0f) / gradLen;
            float cov = half - (dist < 0.0f ? -dist : dist) + 0.5f;  /* 1px AA ramp */
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            /* now (and only now) the angular range test */
            float ang = atan2f(uy, ux) / TAU;   /* (-0.5, 0.5] */
            if (ang < 0.0f) ang += 1.0f;          /* [0, 1) */
            if (ang < startPct || ang > endPct) continue;

            unsigned a = sw_mul255(ca, (unsigned)(cov * 255.0f + 0.5f));
            swBlendPixel(c, px, py, cr, cg, cb, a);
        }
    }
}

/* ---- blur upscale cache (drawBlur) ----
 *
 * Bilinearly upscales a tiny "colour summary" texture (built by texture.h's blur
 * cache) to the draw size, ONCE, into a resident 32bpp texture keyed by
 * (source handle, dst w, dst h). drawBlur then alpha-blits that 1:1 each frame —
 * so the per-frame cost is one cheap blit, not a full-screen bilinear resample. */
#define SW_BLUR_MAX 8
typedef struct { unsigned src; int w, h; unsigned cache; } SwBlurEntry;
typedef struct { SwBlurEntry e[SW_BLUR_MAX]; int count; } SwBlurCache;
static SwBlurCache g_swBlur;

static unsigned swBlurUpscaleGet(unsigned srcId, int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    for (int i = 0; i < g_swBlur.count; i++)
        if (g_swBlur.e[i].src == srcId && g_swBlur.e[i].w == w && g_swBlur.e[i].h == h)
            return g_swBlur.e[i].cache;

    SwTexture *src = swTexGet(srcId);
    if (!src || src->w <= 0 || src->h <= 0) return 0;

    unsigned char *buf = (unsigned char *)malloc((size_t)w * h * 4);
    if (!buf) return 0;

    for (int dyi = 0; dyi < h; dyi++) {
        float sy = ((dyi + 0.5f) / (float)h) * src->h - 0.5f;
        int y0 = (int)floorf(sy); float fy = sy - y0; int y1 = y0 + 1;
        if (y0 < 0) y0 = 0; else if (y0 >= src->h) y0 = src->h - 1;
        if (y1 < 0) y1 = 0; else if (y1 >= src->h) y1 = src->h - 1;
        for (int dxi = 0; dxi < w; dxi++) {
            float sx = ((dxi + 0.5f) / (float)w) * src->w - 0.5f;
            int x0 = (int)floorf(sx); float fx = sx - x0; int x1 = x0 + 1;
            if (x0 < 0) x0 = 0; else if (x0 >= src->w) x0 = src->w - 1;
            if (x1 < 0) x1 = 0; else if (x1 >= src->w) x1 = src->w - 1;

            unsigned p00 = src->px[(size_t)y0 * src->w + x0];
            unsigned p10 = src->px[(size_t)y0 * src->w + x1];
            unsigned p01 = src->px[(size_t)y1 * src->w + x0];
            unsigned p11 = src->px[(size_t)y1 * src->w + x1];
            unsigned char *o = &buf[((size_t)dyi * w + dxi) * 4];
            /* bilerp each channel */
            #define SW_LERP4(GET) \
                ( (GET(p00)*(1.0f-fx)+GET(p10)*fx)*(1.0f-fy) \
                + (GET(p01)*(1.0f-fx)+GET(p11)*fx)*fy )
            o[0] = (unsigned char)(SW_LERP4(SW_R) + 0.5f);
            o[1] = (unsigned char)(SW_LERP4(SW_G) + 0.5f);
            o[2] = (unsigned char)(SW_LERP4(SW_B) + 0.5f);
            o[3] = (unsigned char)(SW_LERP4(SW_A) + 0.5f);
            #undef SW_LERP4
        }
    }

    unsigned id = swTexAlloc(buf, w, h, 4);
    free(buf);
    if (id && g_swBlur.count < SW_BLUR_MAX) {
        SwBlurEntry *e = &g_swBlur.e[g_swBlur.count++];
        e->src = srcId; e->w = w; e->h = h; e->cache = id;
    }
    return id;
}

#endif /* RENDER_SOFT_H */
