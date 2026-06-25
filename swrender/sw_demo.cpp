/* ---------------------------------------------------------------------------
 * SOOB-Core software rasterizer — benchmark harness (throwaway prototype)
 *
 * Builds a Find5-shaped frame and times it, so you can measure real fill-rate on
 * actual P166 hardware (or 86Box) BEFORE committing to porting rotation / ellipse
 * / full text. Two builds from this one file:
 *
 *   Full (windowed, measures raster + present/VRAM cost), needs SDL 1.2:
 *     g++ -O2 -o sw_demo sw_demo.cpp `sdl-config --cflags --libs`
 *     (Win98/DJGPP-or-mingw: link the vendored SDL 1.2 the engine already uses.)
 *
 *   Headless (raster only, no SDL — runs anywhere, cleanest raw fill-rate):
 *     g++ -O2 -DHEADLESS -o sw_demo_hl sw_demo.cpp
 *
 * The frame mimics a spot-the-difference screen:
 *   - 1 full-screen 640x480 OPAQUE background blit (drawBg at native size = the
 *     Tier-1 memcpy path),
 *   - SPRITES alpha-blended 64x64 accents (highlights / cursor / found-markers),
 *   - GLYPHS 8x8 alpha quads (text).
 * Tune the counts to your busiest real screen and read the numbers.
 * ------------------------------------------------------------------------- */

#include "render_soft.h"
#include <stdio.h>
#include <math.h>
#include <time.h>

#ifndef HEADLESS
#include "SDL.h"     /* SDL 1.2 — same vendored headers the engine builds against */
#endif

#define SCRW 640
#define SCRH 480
#define SPRITES 40
#define GLYPHS  600
#define FRAMES  300

/* ---- synthetic assets (so the demo needs no PNG files) ---- */

static unsigned makeBg(void)        /* 640x480 opaque gradient+checker */
{
    int w = SCRW, h = SCRH;
    unsigned char *p = (unsigned char *)malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *q = &p[((size_t)y * w + x) * 3];
            int c = ((x >> 5) ^ (y >> 5)) & 1;
            q[0] = (unsigned char)(x * 255 / w);
            q[1] = (unsigned char)(y * 255 / h);
            q[2] = (unsigned char)(c ? 80 : 160);
        }
    unsigned id = swTexAlloc(p, w, h, 3);
    free(p);
    return id;
}

static unsigned makeSprite(void)    /* 64x64 RGBA soft disc (alpha falls to 0 at rim) */
{
    int s = 64;
    unsigned char *p = (unsigned char *)malloc((size_t)s * s * 4);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            unsigned char *q = &p[((size_t)y * s + x) * 4];
            float dx = x - 31.5f, dy = y - 31.5f;
            float d = sqrtf(dx * dx + dy * dy) / 32.0f;
            int a = d >= 1.0f ? 0 : (int)((1.0f - d) * 255);
            q[0] = 255; q[1] = 220; q[2] = 40; q[3] = (unsigned char)a;
        }
    unsigned id = swTexAlloc(p, s, s, 4);
    free(p);
    return id;
}

static unsigned makeGlyph(void)     /* 8x8 RGBA blob standing in for a font texel */
{
    int s = 8;
    unsigned char *p = (unsigned char *)malloc((size_t)s * s * 4);
    for (int i = 0; i < s * s; i++) {
        p[i*4+0] = 240; p[i*4+1] = 240; p[i*4+2] = 240;
        p[i*4+3] = ((i * 37) & 0xFF) > 90 ? 255 : 0;   /* ~irregular coverage */
    }
    unsigned id = swTexAlloc(p, s, s, 4);
    free(p);
    return id;
}

/* Cheap deterministic scatter (no rand — reproducible frame to frame). */
static int scatterX(int i) { return (i * 137 + 17) % (SCRW - 64); }
static int scatterY(int i) { return (i * 91  + 53) % (SCRH - 64); }

static void renderFrame(SwCanvas *c, unsigned bg, unsigned spr, unsigned gly, int f)
{
    swBlitTex(c, bg, 0, 0, SCRW, SCRH, 0,0,1,1, 255,255,255,255);   /* Tier 1 */

    for (int i = 0; i < SPRITES; i++) {
        int x = scatterX(i) + (int)(8 * sinf((f + i) * 0.1f));      /* a little motion */
        int y = scatterY(i);
        swBlitTex(c, spr, x, y, 64, 64, 0,0,1,1, 255,255,255, 160); /* Tier 3 alpha */
    }
    for (int i = 0; i < GLYPHS; i++) {
        int x = (i * 11) % (SCRW - 8);
        int y = ((i / 70) * 12 + 8) % (SCRH - 8);
        swBlitTex(c, gly, x, y, 8, 8, 0,0,1,1, 255,255,255, 255);   /* Tier 3, small */
    }
}

#ifndef HEADLESS
static void present(SwCanvas *c, SDL_Surface *s)
{
    if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
    for (int y = 0; y < c->h; y++)
        memcpy((unsigned char *)s->pixels + (size_t)y * s->pitch,
               (unsigned char *)c->px + (size_t)y * c->w * 4, (size_t)c->w * 4);
    if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
    SDL_Flip(s);   /* on a SWSURFACE this is the system->VRAM copy = the P166 bottleneck */
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    SwCanvas canvas;
    if (!swCanvasInit(&canvas, SCRW, SCRH, 32)) { printf("alloc failed\n"); return 1; }
    g_swTex.count = 0;
    unsigned bg = makeBg(), spr = makeSprite(), gly = makeGlyph();

#ifdef HEADLESS
    clock_t c0 = clock();
    for (int f = 0; f < FRAMES; f++) renderFrame(&canvas, bg, spr, gly, f);
    clock_t c1 = clock();
    double ms = 1000.0 * (double)(c1 - c0) / CLOCKS_PER_SEC / FRAMES;
    printf("HEADLESS raster-only: %.3f ms/frame  (%.1f fps raster ceiling)\n",
           ms, ms > 0 ? 1000.0 / ms : 0.0);
#else
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { printf("SDL init: %s\n", SDL_GetError()); return 1; }
    SDL_Surface *screen = SDL_SetVideoMode(SCRW, SCRH, 32, SDL_SWSURFACE);
    if (!screen) { printf("video: %s\n", SDL_GetError()); return 1; }

    unsigned rasterMs = 0, presentMs = 0;
    for (int f = 0; f < FRAMES; f++) {
        unsigned a = SDL_GetTicks();
        renderFrame(&canvas, bg, spr, gly, f);
        unsigned b = SDL_GetTicks();
        present(&canvas, screen);
        unsigned d = SDL_GetTicks();
        rasterMs += (b - a); presentMs += (d - b);
        SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) f = FRAMES;
    }
    double r = (double)rasterMs / FRAMES, p = (double)presentMs / FRAMES;
    printf("raster %.2f ms  + present %.2f ms = %.2f ms/frame  (%.1f fps)\n",
           r, p, r + p, (r + p) > 0 ? 1000.0 / (r + p) : 0.0);
    SDL_Quit();
#endif

    swCanvasFree(&canvas);
    return 0;
}
