#ifndef DPI_H
#define DPI_H

/*
 * dpi.h — opt the process into DPI awareness before any window/video init.
 *
 * Why: on Windows 8.1+ with a display scale other than 100% (e.g. 125%), a
 * DPI-unaware process is lied to. GetSystemMetrics and SDL 1.2's
 * SDL_GetVideoInfo report the *virtualised* (scaled-down) desktop size — a
 * physical 1920x1080 monitor at 125% comes back as 1536x864 — and the desktop
 * compositor bitmap-stretches the output. Fullscreen at an explicit 1920x1080
 * then lands on that 1536x864 virtual desktop and gets blurrily upscaled /
 * oversized. Declaring the process DPI-aware makes the OS report real pixels,
 * so config.h's "0 = use desktop" resolves to the true resolution and an
 * explicit 1920x1080 fits the panel 1:1. No scale factor to detect or divide
 * out — we just stop the OS from lying.
 *
 * Old-OS safety: every entry point is resolved at runtime via GetProcAddress,
 * never linked as an import. On Win98 / 2000 / XP the symbols are simply
 * absent, the cascade falls through to a no-op, and the exe still loads. The
 * new DPI constants are passed as raw casts so the ancient MinGW headers need
 * not define them.
 *
 * Call once, as early as possible in main(), BEFORE SDL_Init — DPI awareness
 * is locked in at the first window / DPI query and cannot be changed after.
 * No-op on non-Win32.
 */

#ifdef _WIN32
#include <windows.h>

static void dpiSetProcessAware(void)
{
    /* Newest API first; stop at the first the running OS actually provides. */

    /* Win10 1703+: per-monitor v2 — correct on mixed-DPI setups and when the
       window moves between monitors. Lives in user32, which an SDL app has
       already loaded via its import table. */
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        SetCtxFn setCtx =
            (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
            if (setCtx((HANDLE)-4)) return;
            /* -4 is unknown on 1607; fall back to per-monitor v1 == (HANDLE)-3. */
            if (setCtx((HANDLE)-3)) return;
        }
    }

    /* Win8.1+: shcore!SetProcessDpiAwareness. Not normally loaded, so pull it
       in explicitly. 2 == PROCESS_PER_MONITOR_DPI_AWARE. */
    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *SetAwarenessFn)(int);
        SetAwarenessFn setAwareness =
            (SetAwarenessFn)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (setAwareness) {
            HRESULT hr = setAwareness(2);   /* PROCESS_PER_MONITOR_DPI_AWARE */
            FreeLibrary(shcore);
            /* S_OK = we set it; E_ACCESSDENIED = already set (e.g. a manifest)
               — either way the process is now aware, so we are done. */
            if (hr == S_OK || hr == E_ACCESSDENIED) return;
        } else {
            FreeLibrary(shcore);
        }
    }

    /* Vista..Win8: user32!SetProcessDPIAware — coarse system-DPI awareness,
       the universally-effective fallback for a single-monitor fullscreen game. */
    if (user32) {
        typedef BOOL (WINAPI *SetDpiAwareFn)(void);
        SetDpiAwareFn setDpiAware =
            (SetDpiAwareFn)GetProcAddress(user32, "SetProcessDPIAware");
        if (setDpiAware) setDpiAware();
    }
}

#else
static void dpiSetProcessAware(void) {}
#endif

#endif /* DPI_H */
