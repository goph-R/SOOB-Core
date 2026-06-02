#ifndef ASSET_REGISTRY_H
#define ASSET_REGISTRY_H

/*
 * Name -> path lookup for models and textures declared in assets.lua.
 *
 * The .ent parser used to take raw file paths (`mesh=assets/models/foo.obj`).
 * With this registry, .ent files can use short logical names instead
 * (`mesh=foo`), resolved against the tables populated by scriptLoadAssets.
 * Unknown names fall through as raw paths so one-off meshes still work.
 *
 * Header-only with static functions, matching the rest of the engine.
 * Flat arrays — 64 slots per kind is plenty for this scale and keeps
 * memory static.
 */

#include <string.h>

#define ASSET_REG_MAX     64
#define ASSET_REGION_MAX  256
#define ASSET_NAME_MAX    32
#define ASSET_PATH_MAX    128

/* A Region is a sub-rectangle of a named texture. Game scripts address
   it by name via drawRegion(). sx/sy/sw/sh are in source-texture
   pixels; UV normalization happens at draw time so the texture can be
   any size.

   Optional 9-patch slice: when hasSlice is set, x1/x2/y1/y2 give the
   four cut lines (vertical x1/x2, horizontal y1/y2) relative to the
   region's own top-left, in source pixels. Widgets read this via the
   regionSlice() Lua binding to drive draw9patch() without repeating
   the slice numbers at every call site. */
struct Region {
    char texName[ASSET_NAME_MAX];  /* references AssetRegistry.textureNames */
    int  sx, sy, sw, sh;
    int  hasSlice;                 /* 0 = plain region, 1 = carries slice info */
    int  x1, x2, y1, y2;           /* 9-patch slice cuts (relative to region) */
};

struct AssetRegistry {
    int  modelCount;
    char modelNames[ASSET_REG_MAX][ASSET_NAME_MAX];
    char modelPaths[ASSET_REG_MAX][ASSET_PATH_MAX];

    int  textureCount;
    char textureNames[ASSET_REG_MAX][ASSET_NAME_MAX];
    char texturePaths[ASSET_REG_MAX][ASSET_PATH_MAX];

    int    regionCount;
    char   regionNames[ASSET_REGION_MAX][ASSET_NAME_MAX];
    Region regions[ASSET_REGION_MAX];
};

static void assetRegInit(AssetRegistry *r)
{
    memset(r, 0, sizeof(*r));
}

static int assetRegAddModel(AssetRegistry *r, const char *name, const char *path)
{
    if (r->modelCount >= ASSET_REG_MAX) return 0;
    strncpy(r->modelNames[r->modelCount], name, ASSET_NAME_MAX - 1);
    r->modelNames[r->modelCount][ASSET_NAME_MAX - 1] = '\0';
    strncpy(r->modelPaths[r->modelCount], path, ASSET_PATH_MAX - 1);
    r->modelPaths[r->modelCount][ASSET_PATH_MAX - 1] = '\0';
    r->modelCount++;
    return 1;
}

static int assetRegAddTexture(AssetRegistry *r, const char *name, const char *path)
{
    if (r->textureCount >= ASSET_REG_MAX) return 0;
    strncpy(r->textureNames[r->textureCount], name, ASSET_NAME_MAX - 1);
    r->textureNames[r->textureCount][ASSET_NAME_MAX - 1] = '\0';
    strncpy(r->texturePaths[r->textureCount], path, ASSET_PATH_MAX - 1);
    r->texturePaths[r->textureCount][ASSET_PATH_MAX - 1] = '\0';
    r->textureCount++;
    return 1;
}

static const char *assetRegFindModel(const AssetRegistry *r, const char *name)
{
    if (!r || !name) return NULL;
    for (int i = 0; i < r->modelCount; i++) {
        if (strcmp(r->modelNames[i], name) == 0) return r->modelPaths[i];
    }
    return NULL;
}

static const char *assetRegFindTexture(const AssetRegistry *r, const char *name)
{
    if (!r || !name) return NULL;
    for (int i = 0; i < r->textureCount; i++) {
        if (strcmp(r->textureNames[i], name) == 0) return r->texturePaths[i];
    }
    return NULL;
}

/* Resolve a .ent value: if the token matches a registered model name, return
   its full path; otherwise return the token itself (caller is responsible for
   the lifetime of that string, which in the .ent-load case is the local
   `value` buffer). */
static const char *assetRegResolveModel(const AssetRegistry *r, const char *token)
{
    const char *p = assetRegFindModel(r, token);
    return p ? p : token;
}

static const char *assetRegResolveTexture(const AssetRegistry *r, const char *token)
{
    const char *p = assetRegFindTexture(r, token);
    return p ? p : token;
}

static int assetRegAddRegion(AssetRegistry *r, const char *name, const char *texName,
                             int sx, int sy, int sw, int sh)
{
    if (r->regionCount >= ASSET_REGION_MAX) return 0;
    int i = r->regionCount;
    strncpy(r->regionNames[i], name, ASSET_NAME_MAX - 1);
    r->regionNames[i][ASSET_NAME_MAX - 1] = '\0';
    strncpy(r->regions[i].texName, texName, ASSET_NAME_MAX - 1);
    r->regions[i].texName[ASSET_NAME_MAX - 1] = '\0';
    r->regions[i].sx = sx;
    r->regions[i].sy = sy;
    r->regions[i].sw = sw;
    r->regions[i].sh = sh;
    r->regions[i].hasSlice = 0;
    r->regions[i].x1 = 0; r->regions[i].x2 = 0;
    r->regions[i].y1 = 0; r->regions[i].y2 = 0;
    r->regionCount++;
    return 1;
}

/* Attach 9-patch slice metadata to the most recently added region. Called by
   scr_walkRegionsTable after assetRegAddRegion when the assets.lua entry has
   an optional `slice = { x1, x2, y1, y2 }` table. No-op if the region wasn't
   just added (regionCount == 0). */
static void assetRegSetLastRegionSlice(AssetRegistry *r,
                                       int x1, int x2, int y1, int y2)
{
    if (!r || r->regionCount <= 0) return;
    int i = r->regionCount - 1;
    r->regions[i].hasSlice = 1;
    r->regions[i].x1 = x1; r->regions[i].x2 = x2;
    r->regions[i].y1 = y1; r->regions[i].y2 = y2;
}

static const Region *assetRegFindRegion(const AssetRegistry *r, const char *name)
{
    if (!r || !name) return NULL;
    for (int i = 0; i < r->regionCount; i++) {
        if (strcmp(r->regionNames[i], name) == 0) return &r->regions[i];
    }
    return NULL;
}

#endif /* ASSET_REGISTRY_H */
