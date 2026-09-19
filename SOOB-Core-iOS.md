# SOOB-Core-iOS — 2D iOS player plan

A native iOS app that **plays a 2D SOOB-Core game bundle** (Lua scripts +
`assets.lua` + assets), so a title that already ships on desktop, web and
Android gets an iOS build with **no game-side changes**. Same contract as
[`SOOB-Core-Web`](SOOB-Core-Web.md) and
[`SOOB-Core-Android`](SOOB-Core-Android.md): the host reimplements the
25-binding surface in [`SOOB-Lua.md`](SOOB-Lua.md); the Lua runs unchanged.
**3D (SOOB-Engine) is out of scope.**

Like the Android repo, this is the *player*, not one game's app: a framework
that a per-game `ios/` folder consumes, mirroring the `android/` folder
SOOB-Core-Template already ships.

## Status (2026-09-19)

Not started. No repo, no code. This is the plan written before the work, the
way [`SOOB-Core-Android.md`](SOOB-Core-Android.md) was.

Driven by DY-Ball, which is portrait, single-pointer and iOS-shaped by
construction — but the plan is game-agnostic.

## Why this is cheaper than the Android port was

Android was the second host, so it could copy the web host module-for-module.
iOS is the third, and cheaper again for reasons specific to the platform:

| | Lines |
|---|---|
| Kotlin host — what a new platform reimplements | **2,309** across 11 files |
| `bridge_jni.c` | 950 |
| `bridge.c` (web) | 651 |
| `host_test.c` — the desktop harness | 562 |

So the job is roughly **2,300 lines of Swift plus a bridge variant**, and the
bridge should land nearer the web's 651 than Android's 950: most of that extra
300 lines is JNI plumbing — `FindClass`, `GetStaticMethodID`, marshalling
through a `jdouble[]`. None of it exists on iOS.

Three more things are already paid for:

- **Lua 5.1.5** is C89 and compiles for arm64 unchanged. Same vendored source
  in `vendor/lua-5.1.5/src`, same exclusions as every other target.
- **The shader** is one sprite batcher, already written three times and
  identical in all of them (`Renderer.kt` is "the WebGL1 shaders, verbatim").
- **`app.lua`** already carries game identity for all hosts. A fourth parser is
  ~95 lines with two working references.

## Lua runtime

### bridge_ios.c — bridge.c with C host imports

The web bridge calls out through `EM_JS` into `globalThis.__SOOB`; the Android
one through JNI into the `Host` object. iOS is the easy case: Swift exports
plain C symbols with `@_cdecl`, so the bridge calls them **directly**.

That removes the entire JNI import layer — no class lookup, no method IDs, no
boxing binding arguments into an array to cross the boundary. The host
descriptor table becomes either direct `extern` declarations or a C struct of
function pointers filled in at startup.

Prefer the struct of function pointers: it is exactly what `host_test.c`
already fakes, so the desktop harness keeps working with no special case.

### Script loading — keep the searcher, even though iOS does not need it

Android needed a `package.loaders` searcher because assets inside an APK have
no `fopen`. An iOS app bundle is a real directory on a real filesystem, so
`luaL_loadfile` plus `package.path` would work — the desktop approach.

**Do not take it.** Keeping the asset searcher keeps `bridge_ios.c` a near-copy
of `bridge_jni.c` rather than a third variant with its own module resolution,
and keeps one explanation of how `require "engine.scene"` resolves across every
port. The cost is a little indirection over a `fopen` that would have worked.

## The host — Swift, module-for-module with the Kotlin host

| Kotlin | Swift | Notes |
|---|---|---|
| `Renderer.kt` (453) | `Renderer.swift` | GLES2 via `CAEAGLLayer`, or Metal — see below |
| `Host.kt` (336) | `Host.swift` | The `@_cdecl` entry points the bridge calls |
| `Audio.kt` (322) | `Audio.swift` | `AVAudioPlayer` pool + two players for the music crossfade |
| `GameView.kt` (314) | `GameView.swift` | `CADisplayLink` frame loop, touch marshalling |
| `Assets.kt` (227) | `Assets.swift` | Bundle resources; `CGImage` decode |
| `SoobActivity.kt` (141) | `SoobViewController.swift` | Lifecycle, orientation, safe areas |
| `BmFont.kt` (130) | `BmFont.swift` | Pure parsing — near-direct port |
| `Ime.kt` (129) | `Ime.swift` | Hidden `UITextField` instead of hidden `EditText` |
| `Input.kt` (108) | `Input.swift` | `UITouch` instead of `MotionEvent` |
| `AppInfo.kt` (95) | `AppInfo.swift` | Pure parsing — near-direct port |
| `Lua.kt` (54) | `Lua.swift` | The native entry points |

### Renderer — GLES2 first, Metal later

The one real decision. OpenGL ES has been deprecated on iOS since 12 but still
works. Metal is the durable answer and Apple's direction.

**Take GLES2 first.** The renderer is a single sprite-batching shader that
already exists in GLSL ES 1.0 in `Renderer.kt`, ported verbatim from the WebGL1
original. GLES2 makes `Renderer.swift` a transcription. Metal-first means
rewriting — in MSL, with a pipeline state object, a different uniform model and
a different texture path — the one piece of work that has already been done
three times.

Ship on GLES2, migrate if Apple actually removes it. The renderer is ~450 lines
behind a stable interface and nothing else in the host touches GL, so that
migration stays cheap.

### Threading — simpler than Android

On Android the Lua VM runs on the GLSurfaceView render thread, and input
arriving on the UI thread is marshalled with `queueEvent` so it lands between
frames.

On iOS, `CADisplayLink` already fires on the main thread and UIKit is
main-thread-only, so **run the VM on the main thread** and the entire
marshalling layer disappears. Input is already ordered with respect to frames.

### requestQuit cannot work

On Android, BACK is the desktop Escape and a second press finishes the
activity. iOS has no back, and an app that terminates itself is rejected —
Apple treats programmatic exit as a crash from the user's point of view.

So `requestQuit` is a **no-op on iOS, like web**. A game with a Quit menu item
needs it hidden when `platform == "ios"` — which means this host must call
`setPlatform`, as the web and Android hosts do and the desktop host does not.

### Audio — the session category is a design decision

`AVAudioSession` has no equivalent on the other hosts. The category decides
whether the game is silenced by the hardware mute switch (`.ambient`,
`.soloAmbient`) or plays through it (`.playback`), and whether it stops or
ducks other audio. A player's podcast surviving the game launch is usually the
right default — but it is a choice to make deliberately, not one to stumble
into.

## Display, safe areas, scaling

Same virtual canvas as everywhere: fixed height, width scaling with the aspect
ratio, origin at the centre, Y down.

iOS adds the notch and the home indicator. A game that fits a fixed design box
inside the canvas — as DY-Ball does with its 720x1280 field — gets most of this
free, because every notched iPhone is *narrower* than 9:16, so the fit is
width-bound and the leftover vertical bleed exceeds both insets:

| Device | Bleed each end | Safe top | Home indicator | |
|---|---|---|---|---|
| iPhone 13 mini | 73pt | 50pt | 34pt | clears both |
| iPhone 14 | 75pt | 47pt | 34pt | clears both |
| iPhone 14 Pro Max | 84pt | 59pt | 34pt | clears both |
| iPhone 16 Pro | 80pt | 62pt | 34pt | clears both |
| **iPhone SE (2022)** | **0pt** | 20pt | 0 | **status bar overlaps** |

The SE is 375x667 — essentially 9:16 — so a 9:16 design box fills it exactly
and the 20pt status bar lands on the top edge. Keep the top ~90 design units
free of anything load-bearing.

A game that instead fills the whole canvas needs the Android treatment: report
the safe insets and shrink the viewport, as `SHORT_EDGES` plus inset-aware
sizing does there.

**Home indicator vs. touch.** The system reserves the bottom edge for the
swipe-up gesture. Horizontal dragging there is mostly fine, since the system
gestures are vertical, and `preferredScreenEdgesDeferringSystemGestures` is the
mitigation — but it only *defers* the first swipe. A control scheme whose live
area reaches the bottom of the screen (DY-Ball's steering band does) should be
tested on hardware early rather than assumed.

## Project layout

```
SOOB-Core-iOS/
  SoobPlayer/            the framework — everything that is not game identity
    bridge_ios.c         bridge.c with C host imports
    CMakeLists.txt       lua-5.1.5 (from ../SOOB-Core) + the bridge
    Renderer.swift       …and the rest of the module list above
  Example/               a neutral demo app target, the twin of android's app/
```

And in a game repo, mirroring the `android/` folder:

```
DY-Ball/
  ios/
    DYBall.xcodeproj
    Info.plist           bundle id, orientation, display name
    Assets.xcassets      app icon
```

`app.lua` supplies the rest — display name, orientation, save file, background
colour — exactly as for Android. The bundle is copied in by a build phase, the
twin of `syncGame.gradle` and `sync-game.mjs`, and it must **also copy
SOOB-Core's `scripts/engine`**, which both of those learned to do: a game repo
gitignores that directory, so without the copy the app builds cleanly and dies
at boot on `require "engine.scene"`.

## Toolchain

A Mac mini M1 and an Apple Developer account cover everything. Xcode on Apple
Silicon runs an arm64 iOS Simulator, so simulator iteration is fast and native,
and TestFlight is available for beta the way Play's closed track already is for
CoolFox.

Two notes that still matter:

- **M0 does not need the Mac.** `tools/hosttest` boots the real bridge against a
  stub host table in plain C — that is how the Android C half was verified
  before it ever ran on a phone, including 42 argument-marshalling checks. The
  same harness validates `bridge_ios.c` on the Windows box, and the C half is
  where the subtle bugs live.
- **GLES2 works in the Simulator**, so the renderer can be brought up without
  device round-trips. Audio session behaviour and the home-indicator gesture
  cannot — those need hardware.

Separately, **iOS is not blocked while this is unwritten**: SOOB-Core-Web runs
on iOS Safari today (WASM, WebGL1, PWA, add-to-home-screen), so a game is
playable on an iPhone now. The known iOS-Safari trap is already handled — pointer
deltas must be computed from the absolute position, because
`PointerEvent.movementX` is not populated for touch pointers there.

(A WKWebView wrapper for the App Store is a different matter: Apple's
minimum-functionality rule makes a pure web wrapper a gamble. The PWA route has
no such problem.)

## Milestones

- **M0** — `bridge_ios.c` + the host-descriptor struct, validated entirely in
  `tools/hosttest`. No Swift, no Mac.
- **M1** — Swift host skeleton: `Lua.swift`, `Host.swift`, `AppInfo.swift`,
  `Assets.swift`. Boots a bundle and logs what it registered.
- **M2** — `Renderer.swift` on GLES2 + `BmFont.swift`. First frame, in the
  Simulator.
- **M3** — `Input.swift`, `GameView.swift`, `SoobViewController.swift`.
  Playable on device.
- **M4** — `Audio.swift`, `Ime.swift`, session category, lifecycle.
- **M5** — packaging: the `ios/` folder convention, icon, TestFlight.

## Risks / open questions

1. **GLES2 removal.** Deprecated since iOS 12 and still present, but Apple
   could drop it. Mitigated by the renderer being ~450 lines behind a stable
   interface, with nothing else in the host touching GL.
2. **An embedded interpreter in the store.** The vendored Lua runs only bundled
   scripts, never downloaded code, which is the distinction Apple's rule
   actually turns on. Worth knowing before review, not worth worrying about.
3. **`requestQuit`.** A no-op here; games with a Quit item need it hidden.
4. **Audio session category.** Mute switch and other-audio behaviour both
   follow from it.
5. **Home indicator vs. bottom-edge controls.** Hardware only. Test early.
6. **Who owns `tools/hosttest`.** It lives in the Android repo and is not
   Android-specific. Either share it from SOOB-Core or accept a second copy.
