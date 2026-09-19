# SOOB-Core-Android — 2D Android player plan

A native Android app that **plays a 2D SOOB-Core game bundle** (Lua scripts +
`assets.lua` + assets), so a title that already ships on desktop and web —
Find5 first — gets an Android build with **no game-side changes**. Same
contract as [`SOOB-Core-Web`](SOOB-Core-Web.md): the host reimplements the
25-binding surface in [`SOOB-Lua.md`](SOOB-Lua.md); the Lua runs unchanged.
**3D (SOOB-Engine) is out of scope.**

The repo is the *player*, not a Find5 app: it ships as an Android **library
module** that a per-game app module (or another repo, via composite build)
consumes — see [Template / multi-game](#template--multi-game).

## Status (2026-08-23)

The repo exists — [`goph-R/SOOB-Core-Android`](https://github.com/goph-R/SOOB-Core-Android) —
with M0–M4 written and building: debug APK, R8 release APK and an AAB, arm64
libs 16 KB-aligned. What the plan below left open, and how it was settled:

- **Script loading:** option 1, the `package.loaders` asset searcher.
- **Game identity: done** — as `app.lua` beside `assets.lua`, read by all three
  hosts (`app_info.h`, `src/host/appinfo.ts`, `AppInfo.kt`). It carries `name` /
  `id` / `orientation` / `description`; see [`SOOB-Lua.md`](SOOB-Lua.md). The
  desktop no longer hardcodes its window title or save path, the web build feeds
  its `<title>` and PWA manifest from it, and the Android app module is now
  `class Find5Activity : SoobActivity()` with nothing to override. What stays
  per-game in the app module is what Play requires there: `applicationId`, the
  launcher icon and label, versionCode.
- **Display cutout:** `SHORT_EDGES` plus a viewport shrunk to the safe insets.
- **Verified so far:** a desktop harness (`tools/hosttest`) runs the unmodified
  `bridge_jni.c` behind a stub JNI table against Find5's real bundle — the
  sandbox, the asset searcher (`require`), the `assets.lua` walk, hook dispatch,
  the options round-trip in the desktop file format, and 42 value-by-value
  checks on the binding argument marshalling all pass. Plus: the 14 JNI entry
  points exported by `libsoob.so` match `Lua.kt`, all 34 host method descriptors
  match the compiled `Host` class, and BMFont parsing passes JVM unit tests
  against Find5's real `.fnt`.
- **On hardware:** first boot verified on a Redmi (Android 13, armeabi-v7a) —
  the title screen renders, touch reaches the Lua hooks, Start game runs the
  level countdown. One fix was needed to get there: FORTIFY had to be turned
  off for the vendored Lua, because a `TString` keeps its characters after the
  struct, so `__builtin_object_size(svalue(s))` is 0 and bionic's
  `__strchr_chk` aborts on `lgc.c`'s weak-table `strchr` during the first
  `luaL_openlibs`. Emscripten and MinGW have no FORTIFY, so no other host ever
  saw it. With that fixed, Find5 plays end to end on the device — rendering,
  touch, audio and the soft-keyboard bridge all confirmed by hand. The plan
  below is delivered through M5; the launcher icon and the store listing are
  what remain.

## Why this is cheaper than the web port was

The web port had to answer two questions: *what exactly is the binding
surface*, and *what geometry does each binding do*. Both are now answered in
running code:

- `src/wasm/bridge.c` (651 lines) already registers all 25 bindings on a
  `lua_State`, sandboxes `io`/`os`, walks `assets.lua`, and marshals every
  argument shape (option tables, defaults) out of `script.h`. Only its host
  imports are Emscripten-specific.
- `src/host/*.ts` (~1 000 lines) already contains the ported geometry —
  `drawRegion`'s align/fill/src-rect/dst/flip math, the BMFont layout, the
  ellipse ribbon, the blur downsample.
- The WebGL1 shaders **are GLSL ES 1.0**. They compile on GLES2 verbatim.

So this is a mechanical port of a proven host, not a new design.

```
  Find5 Lua (main/menu/dialog + engine/*)   <- unchanged
  assets.lua                                <- unchanged
        |  drawRegion / soundPlay / optGet / ...   (SOOB-Lua.md surface)
        v
  bridge_jni.c  -- same bindings, JNI instead of EM_JS --+
        |                                                |
  lua-5.1.5 (NDK)          Kotlin host: GLES2 | SoundPool+MediaPlayer
                                       | MotionEvent/KeyEvent | IMM | filesDir
```

## Lua runtime

**NDK-compile the vendored `vendor/lua-5.1.5`** — the same sources the desktop
and WASM builds use, so behaviour is identical on all three platforms. No LuaJ,
no LuaJava: they are Lua 5.1-*ish* reimplementations, and they would throw away
`bridge.c`.

`soob-player/src/main/cpp/CMakeLists.txt` points at the sibling
`../SOOB-Core/vendor/lua-5.1.5/src` with the same exclusions `build-lua.sh`
uses (`lua.c`, `luac.c`, `print.c`, `lua_all.c`) and compiles `bridge_jni.c`
beside it into `libsoob.so`.

### bridge_jni.c — bridge.c with a different host import layer

Everything from `optNum`/`optStr` down through the `scr*` wrappers and the
`assets.lua` walk is copied unchanged. Only the ~35 `EM_JS` stubs are replaced:

| Web | Android |
|---|---|
| `EM_JS(void, js_drawRegion, …)` | cached `jmethodID` → `Host.drawRegion(String, DoubleBuffer)` |
| `HEAPF64.subarray(a>>3, …)` — zero-copy view of the C scratch array | `NewDirectByteBuffer(g_args, sizeof g_args).asDoubleBuffer()` — created **once** at init, also zero-copy |
| `UTF8ToString(s)` | `NewStringUTF` (bindings are ASCII today — see risks) |
| exported `_soob_*` called via `ccall`/`cwrap` | `JNIEXPORT` `Java_net_dynart_soob_Lua_*` natives |

The JNI env is cached per thread; the Lua VM and all bindings live on **one**
thread (the GL thread — see Threading), so one cached `JNIEnv*` plus a global
ref to the host object is enough.

### Script loading — the one structural difference from web

The web host writes every `.lua` into Emscripten's MEMFS and uses
`luaL_loadfile` + `package.path`, exactly like desktop. Android assets are not
a filesystem — `AAssetManager` has no `fopen`. Two options:

1. **Custom Lua loader (recommended).** Install a searcher in `package.loaders`
   that calls back into Kotlin `readAsset(path): ByteArray` and hands the bytes
   to `luaL_loadbuffer`; `soob_doAsset("scripts/main.lua")` does the same for
   the entry points. No copy, no staleness, one `require` path.
2. Copy `assets/game/**` to `filesDir` on first run and keep `luaL_loadfile`
   verbatim. Simpler C, but adds a first-run copy, a version stamp, and a whole
   class of "stale extracted copy" bugs.

`config.lua` and `assets.lua` load through the same reader.

## The host — Kotlin, module-for-module with the web host

| `src/host/*.ts` | Android | Notes |
|---|---|---|
| `gl.ts` | `Renderer.kt` | GLES20 sprite batcher. Same `VIRTUAL_H = 480`, center origin, Y-down, same shaders, same texture-coalescing batch. Direct native-order `FloatBuffer` instead of the JS number array. |
| `text.ts` | `BmFont.kt` | Same `.fnt` key=value parser, same glyph layout by `xadvance` / offsets. |
| `assets.ts` | `Assets.kt` | `AssetManager.open` → `BitmapFactory` → `GLUtils.texImage2D`. Registries filled synchronously by the bridge; decode runs behind the loading screen. |
| `audio.ts` | `Audio.kt` | `SoundPool` for sfx (same non-repeating variant pick as the desktop `SoundLibrary`), two `MediaPlayer`s for the music crossfade. |
| `input.ts` | `GameView.kt` | `onTouchEvent` / `onKeyDown` / `onKeyUp` → the same virtual-coord transform and the same held-state sets. |
| `ime.ts` | `Ime.kt` | Hidden `EditText` + `InputMethodManager`; the value-diff → `onTextInput` logic ports as-is. |
| `loop.ts` | `Renderer.onDrawFrame` | `GLSurfaceView`, `RENDERMODE_CONTINUOUSLY`; `dt` from `System.nanoTime()`, clamped to 0.1 s like the web loop. |
| `mobile.ts` | `SoobActivity.kt` | Immersive fullscreen, landscape lock, cutout handling, back button, lifecycle. |
| `lua.ts` | `Lua.kt` | `external fun` declarations for the `soob_*` natives. |
| `bindings.ts` | `Host.kt` | The object `bridge_jni.c` calls — one method per binding, delegating to the modules above. Signatures mirror `globalThis.__SOOB` exactly. |
| `game/main.ts` | `GameView.bootStep()` | Same order: renderer → host → Lua VM → `assets.lua` → decode (sliced across frames, behind the loading bar) → `main.lua` → `onStart` → loop. |

### Threading — decide this first, it shapes everything

**Lua and every binding run on the GLSurfaceView render thread.** Rendering
bindings must, and splitting the VM across threads would need a lock on every
call. Consequences:

- Touch/key/IME events arrive on the **UI thread** and are marshalled with
  `glView.queueEvent { … }` before they touch Lua. They land between frames,
  which matches the native host's poll-events-then-update order.
- The polling state bindings read (`keyDown`, `mousePos`, `mouseDown`) is
  written from those queued runnables on the same thread — no volatiles needed
  once everything is queued. Resist writing it from the UI thread directly.
- Asset decode runs on a worker pool (bitmap decode only); the GL upload is
  queued back to the GL thread.

### Bindings — what differs from the web implementation

| Area | Android implementation |
|---|---|
| **Rendering** (`drawRegion` `drawText` `drawEllipse` `drawQuad` `drawBg` `drawBlur`) | GLES2; shaders and geometry unchanged from `gl.ts` / `bindings.ts`. `drawEllipse` stays a triangle-strip ribbon — GLES clamps `glLineWidth` the same way browsers do. `drawBlur` stays an FBO downsample (GLES2 FBOs are core, no extension needed). |
| **Queries** (`viewSize` `regionSize` `regionSlice` `textWidth`) | Pure Kotlin over the registries + font metrics; identical to web. |
| **Audio** (`soundPlay` `musicPlay` `musicStop` `musicVolume`) | sfx: `SoundPool` (`load(AssetFileDescriptor)`, ~8 streams). Music: two `MediaPlayer`s, `setVolume` ramped from a `Handler` tick over `fadeSec` — the `GainNode` crossfade in `MediaPlayer` terms. **Ogg Vorbis is natively supported**, so the `.m4a` Safari fallback the web build carries is unnecessary here. Add `AudioManager` focus handling: duck/pause on a call or another app's audio. |
| **Input** (`keyDown` `mousePos` `mouseDown` `keyModifiers`) | Single touch → button 1, same as pointer events. Physical keyboard/gamepad optional: map `KeyEvent` codes to the SDL lowercase names (`"space"`, `"escape"`, `"return"`, letters, digits) with the same table `input.ts` builds from `KeyboardEvent.key`. Touch-only play is unaffected — `keyDown()` just returns false. |
| **Options** (`optSet` `optGet` `optSave` `optLoad`) | A real file in `filesDir` (`<app.id>.dat`, e.g. `find5.dat` — see Game identity), **the same serialized-table format as desktop** — no localStorage-style translation, and a save file is portable between desktop and phone. Atomic write (tmp + rename) like `script.h`. |
| **Misc** (`print` `uiShowMessage` `requestQuit`) | `print` → `Log.i("SOOB", …)`. `uiShowMessage` → the same log, matching the web degradation (its real use is SOOB-Engine, which isn't ported). **`requestQuit` is real here** — unlike web — and calls `Activity.finish()`. |
| **IME** (`imeShow` `imeHide`) | Real: position the hidden `EditText` at the virtual→screen rect, `showSoftInput` inside the tap gesture, `hideSoftInputFromWindow` on blur; `TextWatcher` diff → `onTextInput` / `onKeyDown("backspace")` / `"return"`. |
| **Constants** (`ALIGN_*`, `FLIP_*`) | Set by the bridge — shared C, nothing to do. |

### Back button

`KEYCODE_BACK` → `onKeyDown("escape")`, so the Lua scene stack handles it like
desktop's Escape. If the game does nothing (root scene), a second press within
~2 s finishes the activity. This means the desktop-only quit path — hidden on
web because `requestQuit` is a no-op — should stay **visible** on Android, so
`platform = "android"` is worth setting for exactly that branch.

## Asset pipeline

A Gradle `syncGame` task replaces `sync-game.mjs`, wired as a `preBuild`
dependency, copying `../Find5/{scripts,assets,assets.lua,config.lua}` into
`app/src/main/assets/game/` and writing the same `manifest.json`. Find5 stays
the single source of truth; the generated folder is gitignored.

| Asset | Web | Android |
|---|---|---|
| Textures (PNG) | `createImageBitmap` | `BitmapFactory` with `inScaled = false`, `inPremultiplied = false` |
| BMFont | fetch + parse text | `AssetManager.open` + the same parser |
| Sounds (WAV) | `decodeAudioData` | `SoundPool.load(AssetFileDescriptor)` |
| Music (Ogg) | `decodeAudioData` (+ m4a fallback) | `MediaPlayer.setDataSource(AssetFileDescriptor)` — Ogg is fine |

Two build-config gotchas:

- **`noCompress`.** `SoundPool` / `MediaPlayer` need to seek an
  `AssetFileDescriptor`, which requires the file stored uncompressed in the
  APK: `androidResources { noCompress += ["wav", "ogg", "m4a"] }`.
- **Premultiplied alpha.** `GLUtils.texImage2D` uploads a premultiplied bitmap
  while the batcher blends `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, which darkens
  every sprite edge. Either decode with `inPremultiplied = false` and upload
  the raw buffer, or blend `ONE, ONE_MINUS_SRC_ALPHA`. Pick one and document it
  in the renderer — this is the classic Android sprite bug.

Find5's assets are ~10 MB (7.8 MB of it music), so the whole bundle ships
inside the APK/AAB — no asset packs, no downloads.

## Display, lifecycle, scaling

- `android:screenOrientation="sensorLandscape"`, immersive-sticky fullscreen,
  `LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES` with the cutout inset applied to
  the viewport so nothing important sits under a camera hole.
- `android:configChanges` for orientation/size/keyboard so a rotation or a
  keyboard attach doesn't recreate the activity and restart the Lua VM.
- **GL context loss** on background is the one genuinely Android-shaped risk:
  set `preserveEGLContextOnPause = true`, but still keep a `reloadTextures()`
  path that re-uploads every registry texture in `onSurfaceCreated` — some OEM
  drivers drop the context regardless.
- `onPause` → pause music, stop the loop; `onResume` → resume. The Lua VM
  survives; nothing in the game scripts needs to know.

## Project layout

The player is an **Android library module**; the shipped app is a thin module
on top of it (see [Template / multi-game](#template--multi-game) for why).

```
SOOB-Core-Android/
  settings.gradle, build.gradle, gradle/wrapper/   Gradle 9.x, Groovy DSL (CoolFox house style)

  soob-player/build.gradle                        com.android.library
                                                  compileSdk 36 / minSdk 27
                                                  externalNativeBuild { cmake }
  soob-player/src/main/cpp/CMakeLists.txt         lua-5.1.5 (from ../SOOB-Core) + bridge_jni.c -> libsoob.so
  soob-player/src/main/cpp/bridge_jni.c           bridge.c with JNI host imports
  soob-player/src/main/java/net/dynart/soob/
      SoobActivity.kt  GameView.kt  Renderer.kt  Host.kt  Lua.kt
      Assets.kt  BmFont.kt  Audio.kt  Input.kt  Ime.kt
  soob-player/syncGame.gradle                     the bundle-copy task, parameterised by game dir

  app/build.gradle                                com.android.application — identity only:
                                                  applicationId "net.dynart.find5"
                                                  targetSdk 36, versionCode/Name, signing
                                                  soobGame = "../../Find5"
  app/src/main/AndroidManifest.xml                launcher intent + SoobActivity (or a 5-line subclass)
  app/src/main/assets/game/                       generated by syncGame (gitignored)
  app/src/main/res/                               launcher + adaptive icon, app label
  README.md
```

Requires `SOOB-Core` (Lua sources) and the game bundle (`Find5`) as siblings —
the same sibling-checkout convention as the desktop and web builds.

## Template / multi-game

Nothing in the player knows what Find5 *is* — the game contract is the Lua
bundle, so the host is game-agnostic by construction. Only six things are
per-game: `applicationId`, app label, versionCode/Name, launcher icon, the
`syncGame` source path, and the options filename. The module split above is
what turns "game-agnostic" into an actual template.

**Adding a second game** then means one of two things:

1. **Another app module in this repo** (`app-find5/`, `app-next/`) or a product
   flavor — ~30 lines of Gradle, an icon set, a manifest. Everything else is
   the library. Simplest while all the games are yours.
2. **A composite build from the game's own repo** — the game repo grows an
   `android/` folder with that same thin app module and pulls the player in
   with `includeBuild '../SOOB-Core-Android'` in its `settings.gradle`. This is
   exactly how **CoolFox consumes LisaEngine**, so the workflow is already
   familiar: the game's Android identity lives beside its desktop `main.cpp`,
   and player fixes reach every game without a merge.

Either way the library is consumed, never forked. Prefer 2 once a second game
exists; 1 is fine before that.

### Game identity belongs in the bundle

**Implemented — this is `app.lua` now; the section below is why.**

The one wart was that per-game identity used to be *hardcoded per host*: the
options filename is a literal in the desktop `main.cpp` (`s->optFile`), the web
PWA manifest says "Find5", and the Android template would repeat it a third
time. Fix it while the Android host is being written — add an `app` block that
all three hosts read, either in `config.lua` (which is otherwise display-only)
or a small `app.lua` beside `assets.lua`:

```lua
return {
    app = {
        name        = "Find5",          -- window title / app label / PWA name
        id          = "find5",          -- options filename stem -> find5.dat
        orientation = "landscape",      -- manifest + PWA hint; desktop ignores it
    },
    display = { … },                    -- unchanged, desktop-only
}
```

That drops the template edit to `applicationId` + icon — both of which Play
requires to be per-app anyway — and removes the same duplication from desktop
and web. It is an addition to the shared contract, so it belongs in
[`SOOB-Lua.md`](SOOB-Lua.md) and should be **decided before M0**, not
retrofitted across three hosts later.

## Milestones

0. **Skeleton** — the `soob-player` library + `app` module split, NDK builds
   `libsoob.so`; `soob_new()` plus a `doString("print('hi')")` shows up in
   Logcat. Proves the toolchain. Agree the `app` identity block first.
1. **Boots** — `assets.lua` walked through the JNI bridge, textures uploaded,
   the Find5 title screen renders through the GLES2 batcher, touch →
   `onMouse*`. (Proves the binding surface, as on web.)
2. **Playable** — BMFont text, `SoundPool` + music crossfade, options file,
   transitions and `drawBlur`; the full Find5 loop on a device.
3. **Device integration** — lifecycle/pause, context-loss reload, audio focus,
   back button, immersive + cutouts, loading screen.
4. **Text input** — the IME bridge (hidden `EditText`, `imeShow` / `imeHide`).
5. **Ship** — icons, versionCode/Name, R8, signed AAB, Play listing (reuse the
   CoolFox signing setup), 16 KB page-size check on the native lib.
6. *(optional)* **De-duplicate the bridge** — hoist the shared ~90 % of
   `bridge.c` / `bridge_jni.c` into `SOOB-Core/hosts/bridge_common.h` with
   `SOOB_HOST_*` macros, leaving each port only its import layer.

## Risks / open questions

- **The `app` identity block touches three hosts.** Adding it to the shared
   contract means a (small) desktop and web change too — `optFile` sourced from
   the bundle, the PWA manifest fed from it. Cheap now, annoying later; either
   commit to it at M0 or accept the six-value template edit and drop it.
- **Bridge drift.** Two near-identical copies of `bridge.c` diverge the moment
  a binding is added. Milestone 6 fixes it; until then any change to the
  binding surface is a two-file change — note that in `SOOB-Lua.md`.
- **GL context loss / driver quirks** — test the texture-reload path by really
  backgrounding the app on a device, not just the emulator.
- **Premultiplied alpha** — see above; the wrong choice looks like subtly dark
  sprite edges, easy to ship by accident.
- **Soft keyboard vs immersive fullscreen** — the IME can resize or pan the
  window, and a hidden `EditText` under a GL surface can end up beneath the
  keyboard. Test on both a stock and a Samsung keyboard.
- **`SoundPool` limits** — max simultaneous streams and per-sound size. Find5's
  sfx are tiny, but if latency or polyphony disappoints, swap in **Oboe**
  (there's already a `libgdx-oboe` checkout to crib from).
- **Play policy** — targetSdk 36 and the 16 KB page-size requirement for native
  libraries; NDK 29 aligns by default, but verify with `check-elf-alignment`
  before the first upload.
- **Non-ASCII text** — JNI `NewStringUTF` is modified UTF-8. Find5 is ASCII
  today; if a game ever draws accented text, marshal as a byte array instead.
- **Aspect fragmentation** — 20:9 phones make `viewSize()` far wider than the
  4:3 desktop design rect. The same anchoring rules as web apply, but Find5's
  layout deserves one pass on a tall device.

See [`SOOB-Core-Web.md`](SOOB-Core-Web.md) for the sibling port,
[`SOOB-Lua.md`](SOOB-Lua.md) for the binding spec, and [`CLAUDE.md`](CLAUDE.md)
for naming conventions.
