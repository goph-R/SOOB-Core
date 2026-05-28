#ifndef TEXTURE_H
#define TEXTURE_H

#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_THREAD_SAFE
#define STBI_NO_THREAD_LOCALS
#include "vendor/stb/stb_image.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

/* Upload raw RGB or RGBA pixel data to an OpenGL texture.
   channels must be 3 (RGB) or 4 (RGBA). */
static GLuint uploadTextureN(unsigned char *pixelData, int width, int height,
                             int wrapMode, int channels)
{
    GLuint texID;
    GLenum fmt = (channels == 4) ? GL_RGBA : GL_RGB;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, width, height, 0,
                 fmt, GL_UNSIGNED_BYTE, pixelData);
    return texID;
}

static GLuint uploadTexture(unsigned char *rgbData, int width, int height, int wrapMode)
{
    return uploadTextureN(rgbData, width, height, wrapMode, 3);
}

/* Load a PNG into an OpenGL texture. Only PNG is supported since the
   asset pipeline migrated away from BMP/TGA. stb_image decodes directly
   into the layout we need (top-down, 8-bit per channel).

   keepAlpha=0 forces RGB (alpha dropped even if source has it).
   keepAlpha=1 forces RGBA (alpha filled with 255 if source is RGB).
   outW/outH (optional) receive the loaded pixel dimensions — needed by
   draw_region to compute normalized UVs from pixel-space source rects.
   Returns the GL texture ID, or 0 on failure. */
static GLuint loadTextureExA(const char *filename, int wrapMode, int keepAlpha,
                             int *outW = 0, int *outH = 0)
{
    int w = 0, h = 0, srcCh = 0;
    int desired = keepAlpha ? 4 : 3;
    unsigned char *pix = stbi_load(filename, &w, &h, &srcCh, desired);
    if (!pix) {
        conLogf("texture: cannot load %s (%s)\n", filename, stbi_failure_reason());
        if (outW) *outW = 0;
        if (outH) *outH = 0;
        return 0;
    }
    GLuint texID = uploadTextureN(pix, w, h, wrapMode, desired);
    stbi_image_free(pix);
    conLogf("texture: loaded %s (%dx%d, %d-ch)\n", filename, w, h, desired);
    if (outW) *outW = w;
    if (outH) *outH = h;
    return texID;
}

static GLuint loadTextureEx(const char *filename, int wrapMode)
{
    return loadTextureExA(filename, wrapMode, 0);
}

/* Backward-compatible wrapper: loads with GL_CLAMP_TO_EDGE. */
static GLuint loadTexture(const char *filename)
{
    return loadTextureEx(filename, GL_CLAMP_TO_EDGE);
}

/* ---- Texture Cache ---- */

#define TEX_CACHE_MAX 64

struct TexCacheEntry {
    char path[128];
    GLuint texID;
    int wrapMode;
    int keepAlpha;
    int w, h;            /* source pixel dimensions — for region UV math */
};

struct TexCache {
    TexCacheEntry entries[TEX_CACHE_MAX];
    int count;
};

static void texCacheInit(TexCache *tc)
{
    tc->count = 0;
}

/* Get or load a texture. Returns GL texture ID, or 0 if file not found.
   keepAlpha=1 uploads the texture as RGBA (for alpha-test materials or
   RGBA overlays). The flag is part of the cache key so the same file
   can coexist as RGB and RGBA if a caller ever needs both.
   outW/outH (optional) receive the source pixel dimensions — populated
   from the cache on hit, or from stb_image on miss. */
static GLuint texCacheGetA(TexCache *tc, const char *path, int wrapMode, int keepAlpha,
                           int *outW = 0, int *outH = 0)
{
    if (!path || !path[0]) {
        if (outW) *outW = 0;
        if (outH) *outH = 0;
        return 0;
    }

    for (int i = 0; i < tc->count; i++) {
        if (tc->entries[i].wrapMode == wrapMode &&
            tc->entries[i].keepAlpha == keepAlpha &&
            strcmp(tc->entries[i].path, path) == 0) {
            if (outW) *outW = tc->entries[i].w;
            if (outH) *outH = tc->entries[i].h;
            return tc->entries[i].texID;
        }
    }

    int w = 0, h = 0;
    GLuint tex = loadTextureExA(path, wrapMode, keepAlpha, &w, &h);
    if (tex && tc->count < TEX_CACHE_MAX) {
        strncpy(tc->entries[tc->count].path, path, 127);
        tc->entries[tc->count].path[127] = '\0';
        tc->entries[tc->count].texID = tex;
        tc->entries[tc->count].wrapMode = wrapMode;
        tc->entries[tc->count].keepAlpha = keepAlpha;
        tc->entries[tc->count].w = w;
        tc->entries[tc->count].h = h;
        tc->count++;
    }
    if (outW) *outW = w;
    if (outH) *outH = h;
    return tex;
}

static GLuint texCacheGet(TexCache *tc, const char *path, int wrapMode)
{
    return texCacheGetA(tc, path, wrapMode, 0);
}

static void texCacheFree(TexCache *tc)
{
    for (int i = 0; i < tc->count; i++) {
        if (tc->entries[i].texID)
            glDeleteTextures(1, &tc->entries[i].texID);
    }
    tc->count = 0;
}

/* ---- Blur cache ----
 * Downsampled "color summary" textures used as a blurred-background
 * effect: re-decodes the source PNG, box-filter downsamples to a tiny
 * size (typically 16 wide, height derived from source aspect), uploads
 * with GL_LINEAR. Drawing the tiny texture stretched up produces smooth
 * color gradients between the few sample points — the visual is the
 * source image's dominant colors smeared across the view.
 *
 * Keyed by (path, downW); height is derived from source aspect so the
 * downsample isn't distorted. Cache is intentionally small — there are
 * only as many entries as you have unique (image, blur-width) pairs.
 */

#define TEX_BLUR_MAX 32

struct TexBlurEntry {
    char   path[128];
    int    sx, sy, sw, sh;   /* source rect — region of the PNG to blur */
    int    downW;
    int    downH;            /* derived from source rect aspect */
    GLuint texID;
};

struct TexBlurCache {
    TexBlurEntry entries[TEX_BLUR_MAX];
    int count;
};

static void texBlurInit(TexBlurCache *bc) { bc->count = 0; }

static void texBlurFree(TexBlurCache *bc)
{
    for (int i = 0; i < bc->count; i++) {
        if (bc->entries[i].texID) glDeleteTextures(1, &bc->entries[i].texID);
    }
    bc->count = 0;
}

/* Get or generate a tiny "color summary" texture of the (sx, sy, sw, sh)
   rect of `path`, at `downW` pixels wide. The downsampled height is
   computed from the source rect's aspect ratio so the blur isn't
   distorted. Pass sw = 0 to use the full PNG. Returns the GL texture
   ID, or 0 on failure. */
static GLuint texBlurGet(TexBlurCache *bc, const char *path,
                         int sx, int sy, int sw, int sh, int downW)
{
    if (!path || !path[0] || downW <= 0) return 0;
    if (downW > 64) downW = 64;   /* sanity cap — anything bigger isn't a "blur" */

    for (int i = 0; i < bc->count; i++) {
        TexBlurEntry *e = &bc->entries[i];
        if (e->downW == downW
            && e->sx == sx && e->sy == sy && e->sw == sw && e->sh == sh
            && strcmp(e->path, path) == 0) {
            return e->texID;
        }
    }
    if (bc->count >= TEX_BLUR_MAX) {
        conLogf("texBlur: cache full, dropping '%s'\n", path);
        return 0;
    }

    int imgW = 0, imgH = 0, srcCh = 0;
    unsigned char *src = stbi_load(path, &imgW, &imgH, &srcCh, 4);
    if (!src) {
        conLogf("texBlur: cannot load %s (%s)\n", path, stbi_failure_reason());
        return 0;
    }

    /* Resolve sub-rect — sw/sh of 0 means "full image". Clamp to image bounds
       so out-of-range rects don't read past the buffer. */
    int rx = sx, ry = sy, rw = (sw > 0 ? sw : imgW), rh = (sh > 0 ? sh : imgH);
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > imgW) rw = imgW - rx;
    if (ry + rh > imgH) rh = imgH - ry;
    if (rw <= 0 || rh <= 0) {
        stbi_image_free(src);
        conLogf("texBlur: %s: empty rect (%d,%d %dx%d)\n", path, sx, sy, sw, sh);
        return 0;
    }

    int downH = (int)((float)downW * (float)rh / (float)rw + 0.5f);
    if (downH < 1) downH = 1;

    /* Round both dims up to the next power-of-two. GeForce 4 MX (and other
       DX7-class hardware on the Win98 target) doesn't support NPOT
       textures in hardware — uploading a 16×23 texture there silently
       fails and leaves the bound texture undefined, which reads back as
       the GL clear color. Rounding to 16×32 (etc.) costs ~zero pixels at
       this scale and keeps the result drivable on every target. The mild
       aspect bias is invisible after the cover-stretch to view width. */
    { int p; for (p = 1; p < downW; p <<= 1) {} downW = p; }
    { int p; for (p = 1; p < downH; p <<= 1) {} downH = p; }

    unsigned char *dst = (unsigned char *)malloc((size_t)downW * downH * 4);
    if (!dst) {
        stbi_image_free(src);
        return 0;
    }

    /* Box-filter downsample over the sub-rect: for each dst pixel, average
       the source rect that maps to it (shifted by rx/ry into the PNG).
       Integer accumulators — downW × downH is tiny (≤ 64 × 64). */
    for (int dy = 0; dy < downH; dy++) {
        int y0 = ry + ( dy      * rh) / downH;
        int y1 = ry + ((dy + 1) * rh) / downH;
        if (y1 == y0) y1 = y0 + 1;
        for (int dx = 0; dx < downW; dx++) {
            int x0 = rx + ( dx      * rw) / downW;
            int x1 = rx + ((dx + 1) * rw) / downW;
            if (x1 == x0) x1 = x0 + 1;

            unsigned long ar = 0, ag = 0, ab = 0, aa = 0;
            for (int py = y0; py < y1; py++) {
                const unsigned char *row = &src[((size_t)py * imgW + x0) * 4];
                for (int px = x0; px < x1; px++) {
                    ar += row[0];
                    ag += row[1];
                    ab += row[2];
                    aa += row[3];
                    row += 4;
                }
            }
            unsigned long n = (unsigned long)(x1 - x0) * (unsigned long)(y1 - y0);
            unsigned char *o = &dst[((size_t)dy * downW + dx) * 4];
            o[0] = (unsigned char)(ar / n);
            o[1] = (unsigned char)(ag / n);
            o[2] = (unsigned char)(ab / n);
            o[3] = (unsigned char)(aa / n);
        }
    }

    GLuint texID = uploadTextureN(dst, downW, downH, GL_CLAMP_TO_EDGE, 4);
    free(dst);
    stbi_image_free(src);
    if (!texID) return 0;

    TexBlurEntry *e = &bc->entries[bc->count++];
    strncpy(e->path, path, 127);
    e->path[127] = '\0';
    e->sx = sx; e->sy = sy; e->sw = sw; e->sh = sh;
    e->downW = downW;
    e->downH = downH;
    e->texID = texID;
    conLogf("texBlur: generated %dx%d for %s [%d,%d %dx%d]\n",
            downW, downH, path, rx, ry, rw, rh);
    return texID;
}

#endif
