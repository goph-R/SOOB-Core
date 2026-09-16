/* soob_main.h — the shared 2D host: boot, frame loop, shutdown.
 *
 * This is a WHOLE-PROGRAM header, not a normal module header. It is the only
 * thing a 2D game's main.cpp includes, and it owns the include order that each
 * game's main.cpp used to own — which is what makes the forward-declare-conLogf
 * dance disappear from the game. A game becomes:
 *
 *     #include "soob_main.h"
 *     int main(int argc, char *argv[]) { return soobRun(argc, argv, 0); }
 *
 * Everything per-game lives in the three Lua manifests beside the exe:
 * app.lua (identity), config.lua (display), assets.lua (content). The only
 * knobs that cannot live there are compile-time by nature and are documented
 * at their use sites below: UI_VIRTUAL_H and SOOB_SOFTWARE_BACKEND.
 *
 * Scope: 2D games. SOOB-Engine (3D FPS) keeps its own main.cpp — its frame
 * loop has a mode state machine, mouse grab policy and a console that
 * intercept input ahead of any script dispatch, so sharing this loop would
 * mean seams no 2D game ever uses.
 *
 * Target span: Windows 98 (Dev-C++ / MinGW 3.4, SDL 1.2, fixed-function GL)
 * through modern Linux / Windows. No C++11.
 */

#ifndef SOOB_MAIN_H
#define SOOB_MAIN_H

#ifdef _WIN32
#include <SDL/SDL.h>
#else
#include <SDL.h>
#endif
#include <GL/gl.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <math.h>

/* Forward-declared logger used throughout the engine headers (texture.h, ui.h,
   sound.h, music.h, script.h all call conLogf). The definition follows the
   includes — `static` at namespace scope only needs a declaration before use. */
static void conLogf(const char *fmt, ...);

#include "texture.h"
#include "ui.h"
#include "sound.h"
#include "music.h"
#include "asset_registry.h"
#include "script.h"
#include "config.h"
#include "app_info.h"

/* A game that wants a real dev console (SOOB-Engine-style scrollback) defines
   SOOB_CUSTOM_CONLOG before including this header and supplies its own
   `static void conLogf(const char *, ...)` definition anywhere in the TU.
   Forgetting it is a link error, which is the right failure mode. */
#ifndef SOOB_CUSTOM_CONLOG
static void conLogf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}
#endif

static const int SOOB_SAMPLE_RATE = 44100;

/* ---- Win32 fullscreen refresh-rate workaround ----
 * SDL 1.2's fullscreen path calls ChangeDisplaySettings without a frequency,
 * dropping the monitor to 60Hz. Sample the desktop rate before SDL_Init and
 * reapply it with ChangeDisplaySettingsEx after SDL_SetVideoMode. No-op on
 * Linux. */
#ifdef _WIN32
#include <windows.h>
static int g_desktopHz = 0;
static void saveDesktopRefreshHz(void)
{
    DEVMODE dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettings(NULL, ENUM_REGISTRY_SETTINGS, &dm)) {
        g_desktopHz = (int)dm.dmDisplayFrequency;
    }
}
static void applyFullscreenRefreshHz(int width, int height)
{
    if (g_desktopHz <= 0) return;
    DEVMODE dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth        = width;
    dm.dmPelsHeight       = height;
    dm.dmBitsPerPel       = 32;
    dm.dmDisplayFrequency = g_desktopHz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsEx(NULL, &dm, NULL, CDS_FULLSCREEN, NULL);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        conLogf("refresh: ChangeDisplaySettingsEx(%dHz) failed: %ld\n", g_desktopHz, (long)r);
    }
}
#else
static void saveDesktopRefreshHz(void) {}
static void applyFullscreenRefreshHz(int, int) {}
#endif

/* DPI awareness — included after <windows.h> so its careful late ordering is
   preserved; self-guarded and a no-op on non-Win32. */
#include "dpi.h"

#ifdef SOOB_SOFTWARE_BACKEND
/* Copy the software backbuffer to the SDL display surface and flip. On a
   SWSURFACE this Flip is the system->VRAM copy — the real bottleneck on a P166.
   Note: raw 32-bit copy; assumes the display surface is 0xAARRGGBB like the
   backbuffer (true on the Win98 target). If colors look swapped on some host,
   convert against screen->format here. */
static void swPresent(SwCanvas *c, SDL_Surface *s)
{
    int bytes = c->bpp / 8;   /* 2 (RGB565) or 4 (ARGB); matches the surface */
    if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
    for (int y = 0; y < c->h; y++)
        memcpy((unsigned char *)s->pixels + (size_t)y * s->pitch,
               (unsigned char *)c->px + (size_t)y * c->w * bytes,
               (size_t)c->w * bytes);
    if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
    SDL_Flip(s);
}
#endif

/* ---- F12 frame dump ----
 * Writes the just-presented frame to <app.id>_shot_NNN.bmp next to the exe.
 * Handy for sanity-checking software-mode colours on the real machine without a
 * camera (e.g. confirming the 16bpp present isn't channel-swapped on a given
 * display).
 *
 * Software mode: the SDL surface already holds the presented pixels, so save it
 * directly. GL mode: the GL surface has no CPU pixels, so read the back buffer
 * with glReadPixels (must be called BEFORE SwapBuffers) and flip it top-down. */
static int  g_shotReq = 0;
static int  g_shotNum = 0;
static char g_shotStem[APP_ID_MAX] = "soob";   /* set from app.id in soobRun */

static void saveScreenshot(SDL_Surface *screen, int w, int h, int softwareMode)
{
    char name[APP_ID_MAX + 32];
    sprintf(name, "%s_shot_%03d.bmp", g_shotStem, ++g_shotNum);

    if (softwareMode) {
        if (SDL_SaveBMP(screen, name) == 0) conLogf("screenshot: wrote %s\n", name);
        else conLogf("screenshot: SDL_SaveBMP failed: %s\n", SDL_GetError());
        return;
    }

    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    if (!px) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);   /* reads GL_BACK; rows bottom-up */
    /* 24bpp surface with memory byte order R,G,B (matches glReadPixels) on LE. */
    SDL_Surface *surf = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 24,
                                             0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    if (surf) {
        if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
        for (int y = 0; y < h; y++)
            memcpy((unsigned char *)surf->pixels + (size_t)y * surf->pitch,
                   px + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);  /* flip top-down */
        if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
        if (SDL_SaveBMP(surf, name) == 0) conLogf("screenshot: wrote %s\n", name);
        else conLogf("screenshot: SDL_SaveBMP failed: %s\n", SDL_GetError());
        SDL_FreeSurface(surf);
    }
    free(px);
}

/* ---- Per-game customisation ----
 *
 * Zero-config games pass 0 to soobRun and get the defaults below. Fill a
 * SoobApp only to rename a manifest or to add native Lua bindings.
 *
 * Deliberately absent: window size / clear colour (config.lua and app.lua own
 * those — a third source of truth in C would undo the point of this header) and
 * any per-frame native render hook (a 2D SOOB game draws from Lua; native frame
 * hooks are the first step toward the 3D retrofit this header does not do). */
struct SoobApp {
    const char *configFile;    /* "config.lua"       */
    const char *appFile;       /* "app.lua"          */
    const char *assetsFile;    /* "assets.lua"       */
    const char *entryScript;   /* "scripts/main.lua" */

    /* Extra lua_register() calls for games with native code. Runs after
       scriptInit and before the asset manifest loads, so the bindings exist
       by the time assets.lua and the entry script run. */
    void (*onRegister)(ScriptSystem *s);
};

static SoobApp soobAppDefaults(void)
{
    SoobApp a;
    a.configFile  = "config.lua";
    a.appFile     = "app.lua";
    a.assetsFile  = "assets.lua";
    a.entryScript = "scripts/main.lua";
    a.onRegister  = 0;
    return a;
}

/* ---- The host ----
 * Returns a process exit code: 0 on a clean run, 1 if the display or audio
 * device could not be opened. `userApp` may be 0 for the defaults. */
static int soobRun(int argc, char *argv[], const SoobApp *userApp)
{
    SoobApp opt = soobAppDefaults();
    if (userApp) opt = *userApp;

    /* Tell modern Windows we render in real pixels, before SDL touches the
       display — otherwise a non-100% display scale makes SDL_GetVideoInfo
       report a virtualised desktop and fullscreen oversizes. Safe no-op on
       old Windows (and Linux): see dpi.h. */
    dpiSetProcessAware();

    /* Display config: built-in defaults → config.lua → CLI args → clamp.
       Width/height of 0 is a sentinel for "use desktop resolution",
       resolved below once SDL knows the desktop size. */
    Config cfg = configLoadDefaults();
    configLoadFromFile(&cfg, opt.configFile);
    configApplyArgs(&cfg, argc, argv);

    /* Who we are: window title, save-file stem, screenshot stem and clear
       colour all come from app.lua — the same file the web and Android hosts
       read. */
    AppInfo app = appInfoLoadDefaults();
    appInfoLoadFromFile(&app, opt.appFile);
    snprintf(g_shotStem, sizeof(g_shotStem), "%s", app.id);

    float bgR = 0.08f, bgG = 0.08f, bgB = 0.12f;
    appInfoBackgroundRgb(&app, &bgR, &bgG, &bgB);

    configClamp(&cfg);
    int screenW    = cfg.width;
    int screenH    = cfg.height;
    int fullscreen = cfg.fullscreen;

#ifdef SOOB_SOFTWARE_BACKEND
    g_renderMode = cfg.render ? RENDER_MODE_SOFTWARE : RENDER_MODE_OPENGL;
    if (cfg.render) conLogf("Renderer: software (%d-bpp)\n", cfg.depth);
    else            conLogf("Renderer: opengl\n");
#else
    if (cfg.render)
        conLogf("config: render='software' requested but software backend not "
                "compiled in (define SOOB_SOFTWARE_BACKEND); using OpenGL\n");
#endif

    saveDesktopRefreshHz();
    srand((unsigned)time(NULL));

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        conLogf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Resolve the "0 = desktop" sentinel for fullscreen mode.
       SDL_GetVideoInfo()->current_w/h returns the desktop size BEFORE
       SDL_SetVideoMode has been called (SDL 1.2.10+). */
    if (fullscreen) {
        const SDL_VideoInfo *vi = SDL_GetVideoInfo();
        if (vi) {
            if (screenW == 0) screenW = vi->current_w;
            if (screenH == 0) screenH = vi->current_h;
        }
    }
    /* Final clamp: catches any 0-sentinel that survived (e.g. windowed
       mode with width=0 in config.lua) and any out-of-range desktop value. */
    if (screenW < CONFIG_W_MIN) screenW = CONFIG_W_MIN;
    if (screenW > CONFIG_W_MAX) screenW = CONFIG_W_MAX;
    if (screenH < CONFIG_H_MIN) screenH = CONFIG_H_MIN;
    if (screenH > CONFIG_H_MAX) screenH = CONFIG_H_MAX;
    conLogf("Resolution: %dx%d%s\n", screenW, screenH, fullscreen ? " fullscreen" : "");

    SoundSystem snd;
    if (!sndInit(&snd, SOOB_SAMPLE_RATE)) {
        SDL_Quit();
        return 1;
    }
    SoundLibrary sndLib;
    sndLibInit(&sndLib);

    MusicSystem mus;
    musicInit(&mus);
    MusicLibrary musLib;
    musicLibInit(&musLib);

    SDL_Surface *screen = 0;
#ifdef SOOB_SOFTWARE_BACKEND
    if (g_renderMode == RENDER_MODE_SOFTWARE) {
        /* No GL context: a plain shadow surface plus a CPU backbuffer. */
        Uint32 vflags = SDL_SWSURFACE;
        if (fullscreen) vflags |= SDL_FULLSCREEN;
        screen = SDL_SetVideoMode(screenW, screenH, cfg.depth, vflags);
        if (screen && !swCanvasInit(&g_swCanvas, screenW, screenH, cfg.depth)) {
            conLogf("software: backbuffer alloc failed\n");
            sndShutdown(&snd);
            SDL_Quit();
            return 1;
        }
    } else
#endif
    {
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        /* No depth buffer needed for pure 2D, but a tiny one doesn't hurt
           and keeps the SDL/GL attribute set close to typical defaults. */
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        /* Request vsync before SDL_SetVideoMode (SDL 1.2.10+). Only a request:
           some old drivers ignore it and there's no read-back in 1.2, so we just
           log what we asked for. */
        SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, cfg.vsync);
        conLogf("VSync: %s\n", cfg.vsync ? "on (requested)" : "off");

        Uint32 videoFlags = SDL_OPENGL;
        if (fullscreen) videoFlags |= SDL_FULLSCREEN;
        screen = SDL_SetVideoMode(screenW, screenH, 32, videoFlags);
    }
    if (!screen) {
        conLogf("SDL_SetVideoMode failed: %s\n", SDL_GetError());
        sndShutdown(&snd);
        SDL_Quit();
        return 1;
    }

    if (fullscreen) applyFullscreenRefreshHz(screenW, screenH);

    SDL_EnableUNICODE(1);
    SDL_WM_SetCaption(app.name, NULL);

    /* 2D GL state. uiBegin/uiEnd manages its own state per-frame, but
       set sensible defaults so non-UI draws also behave. Skipped entirely in
       software mode — there is no GL context to configure. */
#ifdef SOOB_SOFTWARE_BACKEND
    if (g_renderMode != RENDER_MODE_SOFTWARE)
#endif
    {
        glViewport(0, 0, screenW, screenH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(bgR, bgG, bgB, 1.0f);
    }

    UiState ui;
    uiInit(&ui, screenW, screenH);

    AssetRegistry assetReg;
    assetRegInit(&assetReg);
    TexCache texCache;
    texCacheInit(&texCache);
    TexBlurCache blurCache;
    texBlurInit(&blurCache);
    ScriptSystem script;
    /* Per-user persistence path, named from app.lua: AppData\<Name>\<id>.dat
       (Windows) or ~/.config/<Name>/<id>.dat (Unix). Falls back to "<id>.dat"
       next to the exe when no user-config dir is reachable (e.g. Win98).
       optPath is static so the buffer outlives ScriptSystem's borrowed
       pointer. */
    static char optPath[512];
    char optName[APP_ID_MAX + 8];
    appInfoOptFileName(&app, optName, sizeof(optName));
    scriptResolveConfigPath(app.name, optName, optPath, sizeof(optPath));
    scriptInit(&script, &ui, &snd, &sndLib, &mus, &musLib, &assetReg,
               &texCache, &blurCache, optPath);

    /* Native bindings, if this game has any, before anything Lua-side runs. */
    if (opt.onRegister) opt.onRegister(&script);

    scriptLoadAssets(&script, opt.assetsFile);
    scriptInstallConsolePrint(&script);

    /* Run the entry script, then fire onStart. Scripts can call
       musicPlay, soundPlay, uiShowMessage at this point. */
    scriptRunFile(&script, opt.entryScript);
    scriptCall(&script, "onStart");

    int running = 1;
    Uint32 lastTime = SDL_GetTicks();

    SDL_Event event;
    while (running) {
        Uint32 now = SDL_GetTicks();
        float dt = (now - lastTime) / 1000.0f;
        if (dt > 0.15f) dt = 0.15f;
        lastTime = now;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
                break;
            }
            if (event.type == SDL_KEYDOWN) {
                SDLKey sym = event.key.keysym.sym;
                if (sym == SDLK_F12) g_shotReq = 1;   /* dump current frame */
                const char *name = SDL_GetKeyName(sym);
                scriptCallKeyDown(&script, name ? name : "");
                /* Fire onTextInput after onKeyDown when the key
                   produced a printable ASCII character. Editor widgets
                   handle character insertion here; navigation keys
                   stay in onKeyDown. ASCII only for v1. */
                Uint16 uni = event.key.keysym.unicode;
                if (uni >= 0x20 && uni <= 0x7E) {
                    char buf[2] = { (char)uni, '\0' };
                    scriptCallTextInput(&script, buf);
                }
            }
            if (event.type == SDL_KEYUP) {
                const char *name = SDL_GetKeyName(event.key.keysym.sym);
                scriptCallKeyUp(&script, name ? name : "");
            }
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                float vx = 0.0f, vy = 0.0f;
                uiMouseToVirtual(&ui, event.button.x, event.button.y, &vx, &vy);
                scriptCallMouseDown(&script, vx, vy, event.button.button);
            }
            if (event.type == SDL_MOUSEBUTTONUP) {
                float vx = 0.0f, vy = 0.0f;
                uiMouseToVirtual(&ui, event.button.x, event.button.y, &vx, &vy);
                scriptCallMouseUp(&script, vx, vy, event.button.button);
            }
            if (event.type == SDL_MOUSEMOTION) {
                /* xrel/yrel are pixels; convert by scaling against the
                   virtual canvas (no offset, since they're deltas). */
                float vx = 0.0f, vy = 0.0f;
                uiMouseToVirtual(&ui, event.motion.x, event.motion.y, &vx, &vy);
                float scale = (ui.virtualH > 0) ? (ui.virtualH / (float)screenH) : 1.0f;
                float dvx = event.motion.xrel * scale;
                float dvy = event.motion.yrel * scale;
                scriptCallMouseMove(&script, vx, vy, dvx, dvy);
            }
        }

        scriptCallUpdate(&script, dt);
        /* A script (e.g. the menu's "Exit to system?" dialog) can ask to
           quit via requestQuit(); honor it after the frame's update so
           any dialog outro / action that set it has already run. */
        if (script.quitRequested) running = 0;
        musicUpdate(&mus, dt);
        uiUpdateMessage(&ui, dt);

#ifdef SOOB_SOFTWARE_BACKEND
        if (g_renderMode == RENDER_MODE_SOFTWARE)
            swCanvasClear(&g_swCanvas, SW_ARGB(255, (int)(bgR * 255.0f + 0.5f),
                                                    (int)(bgG * 255.0f + 0.5f),
                                                    (int)(bgB * 255.0f + 0.5f)));
        else
#endif
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        uiBegin(&ui);

        /* Game-side rendering — scripts call drawRegion etc. from here. */
        scriptCallRender(&script);

        /* Engine HUD overlay — drawn after game so it always sits on top. */
        uiDrawMessage(&ui);

        uiEnd(&ui);

#ifdef SOOB_SOFTWARE_BACKEND
        if (g_renderMode == RENDER_MODE_SOFTWARE) {
            swPresent(&g_swCanvas, screen);
            if (g_shotReq) { saveScreenshot(screen, screenW, screenH, 1); g_shotReq = 0; }
        } else
#endif
        {
            /* GL: read the back buffer before it's swapped away. */
            if (g_shotReq) { saveScreenshot(screen, screenW, screenH, 0); g_shotReq = 0; }
            SDL_GL_SwapBuffers();
        }
        SDL_Delay(1);
    }

    /* Teardown, innermost first. texCacheFree deletes GL texture names, so it
       has to run while the context is still alive — i.e. before SDL_Quit. */
    scriptShutdown(&script);
    texBlurFree(&blurCache);
    texCacheFree(&texCache);
    uiShutdown(&ui);
    musicShutdown(&mus);
    sndLibShutdown(&sndLib);
    sndShutdown(&snd);
#ifdef SOOB_SOFTWARE_BACKEND
    if (g_renderMode == RENDER_MODE_SOFTWARE) swCanvasFree(&g_swCanvas);
#endif
    SDL_Quit();
    return 0;
}

#endif /* SOOB_MAIN_H */
