# vendor_win10 — Win10 toolchain and optional runtime

Headers and import libraries for the Windows 10 build (portable WinLibs MinGW).
Two things here are **not** tracked and must be fetched separately.

## `mingw32/` — the compiler (~250 MB)

Download WinLibs i686 and extract so that
`vendor_win10/mingw32/bin/g++.exe` exists.
<https://github.com/brechtsanders/winlibs_mingw/releases>

## `OpenAL32.dll` — optional full OpenAL Soft (~4 MB)

**You usually do not need this.** The engine tracks a small 32-bit OpenAL
runtime at `vendor/OpenAL32.dll`, and `build/build_win10.bat` copies that next
to the exe when no `vendor_win10/OpenAL32.dll` is present. Drop the full build
here only if you need its extra backends or HRTF tables; it then takes
precedence.

Get it from <https://www.openal-soft.org/#download> and rename
`bin/Win32/soft_oal.dll` to `OpenAL32.dll`. **Take the Win32 build** — a 64-bit
DLL will not load into these 32-bit executables.

> Note: a file named `OpenAL32-win10-small.dll` circulated in the game repos and
> is **not** usable — it is a 64-bit Creative Labs router DLL, not OpenAL Soft.

## Win98 target

OpenAL Soft 1.9.563 is the only release tested working on Windows 98. Put it
next to the exe as `OpenAL32.dll` on that machine.
