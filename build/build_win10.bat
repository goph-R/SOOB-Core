@echo off
setlocal

REM ================================================================
REM  build_win10.bat - the shared Win10 build for a 2D SOOB-Core game.
REM
REM  A game's build_win10.bat is one line:
REM
REM      call ..\SOOB-Core\build\build_win10.bat
REM
REM  Optional arguments:
REM      %1  exe stem        (default: the game folder's name)
REM      %2  extra compiler options
REM      %3  extra objects to link
REM
REM  cmd.exe is guaranteed on this target, so call / setlocal / %~1 are all
REM  safe here. The Win98 build.bat is deliberately NOT shared this way --
REM  COMMAND.COM reopens a batch file per line and has none of these.
REM ================================================================

REM  ENGINE is resolved relative to THIS script, not to the caller's cwd, so
REM  the game does not have to sit in any particular place. `call` does not
REM  change cwd, so main.cpp / raw\obj\ / scripts\engine\ stay game-relative.
set "ENGINE=%~dp0.."
set "GPP=%ENGINE%\vendor_win10\mingw32\bin\g++.exe"
set "GCC=%ENGINE%\vendor_win10\mingw32\bin\gcc.exe"

REM  Exe stem: the argument if given, else this folder's name.
set "NAME=%~1"
if "%NAME%"=="" for %%I in ("%CD%") do set "NAME=%%~nxI"

echo === %NAME% Win10 MinGW Build ===
echo.

if not exist "%GPP%" (
    echo ERROR: MinGW not found at %GPP%
    echo Download WinLibs i686 and extract to %ENGINE%\vendor_win10\mingw32\
    echo https://github.com/brechtsanders/winlibs_mingw/releases
    goto error
)

if not exist "%ENGINE%\vendor_win10\include\AL\al.h" (
    echo ERROR: OpenAL headers missing.
    echo Download OpenAL Soft 1.23.x prebuilt binaries and place:
    echo   AL\al.h, AL\alc.h -^> %ENGINE%\vendor_win10\include\AL\
    echo   libOpenAL32.dll.a or libopenal.dll.a -^> %ENGINE%\vendor_win10\lib\
    echo From: https://www.openal-soft.org/#download
    goto error
)

for /f "tokens=*" %%V in ('%GPP% -dumpversion') do set "GCC_VER=%%V"
echo Found GCC %GCC_VER%
echo.

REM  The CPU rasterizer is compiled in alongside GL; config.lua's
REM  display.render picks the active backend at runtime.
set "OPTS=-O2 -DSOOB_SOFTWARE_BACKEND -I%ENGINE% -I%ENGINE%\vendor_win10\include -I%ENGINE%\vendor\lua-5.1.5\src %~2"

set "OBJDIR=raw\obj"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM ----------------------------------------------------------------
REM  Lua 5.1.5 (cached -- delete raw\obj\lua.o to force rebuild)
REM ----------------------------------------------------------------
if exist %OBJDIR%\lua.o (
    echo Skipping Lua ^(lua.o cached^)
) else (
    echo Compiling Lua...
    %GCC% -I%ENGINE%\vendor\lua-5.1.5\src -Dluaall_c -O2 -c %ENGINE%\vendor\lua-5.1.5\src\lua_all.c -o %OBJDIR%\lua.o
    if errorlevel 1 goto error
)

REM ----------------------------------------------------------------
REM  stb_vorbis (cached -- delete raw\obj\vorbis.o to force rebuild)
REM ----------------------------------------------------------------
if exist %OBJDIR%\vorbis.o (
    echo Skipping stb_vorbis ^(vorbis.o cached^)
) else (
    echo Compiling stb_vorbis...
    %GCC% -O2 -c %ENGINE%\vendor\stb\stb_vorbis.c -o %OBJDIR%\vorbis.o
    if errorlevel 1 goto error
)

REM ----------------------------------------------------------------
REM  Compile main
REM ----------------------------------------------------------------
echo Compiling SDL main stub...
%GCC% -c %ENGINE%\vendor_win10\sdl_main.c -o %OBJDIR%\sdl_main.o
if errorlevel 1 goto error

echo Compiling main...
%GPP% %OPTS% -c main.cpp -o %OBJDIR%\main.o
if errorlevel 1 goto error

REM ----------------------------------------------------------------
REM  Link
REM ----------------------------------------------------------------
echo Linking...
%GPP% %OBJDIR%\main.o %OBJDIR%\sdl_main.o %OBJDIR%\lua.o %OBJDIR%\vorbis.o %~3 -o %NAME%_w10.exe -mwindows -L%ENGINE%\vendor_win10\lib -lmingw32 -lSDL -lopengl32 -lOpenAL32 -static-libgcc -static-libstdc++
if errorlevel 1 goto error

REM ----------------------------------------------------------------
REM  Copy runtime DLLs next to the exe. They live in SOOB-Core so a new
REM  game repo does not have to carry its own ~4.7MB copy.
REM ----------------------------------------------------------------
if not exist "libwinpthread-1.dll" (
    copy %ENGINE%\vendor_win10\mingw32\bin\libwinpthread-1.dll . >nul
)
if not exist "SDL.dll" (
    if exist "%ENGINE%\vendor\SDL.dll" (
        copy %ENGINE%\vendor\SDL.dll . >nul
    ) else (
        echo NOTE: SDL.dll not found next to the exe and not in the engine.
    )
)
REM  vendor_win10\OpenAL32.dll is the optional full OpenAL Soft build (see
REM  vendor_win10\README.md); vendor\OpenAL32.dll is the small i386 runtime
REM  that ships with the engine and serves both Windows targets.
if not exist "OpenAL32.dll" (
    if exist "%ENGINE%\vendor_win10\OpenAL32.dll" (
        copy %ENGINE%\vendor_win10\OpenAL32.dll . >nul
    ) else (
        if exist "%ENGINE%\vendor\OpenAL32.dll" (
            copy %ENGINE%\vendor\OpenAL32.dll . >nul
        ) else (
            echo NOTE: OpenAL32.dll not found next to exe. Place the OpenAL Soft
            echo       runtime DLL ^(renamed from soft_oal.dll if needed^) here.
        )
    )
)

REM ----------------------------------------------------------------
REM  Mirror SOOB-Core's Lua engine modules next to the exe so
REM  require "engine.scene" resolves via ./scripts/?.lua in shipped
REM  builds without needing the SOOB-Core repo on the player's machine.
REM ----------------------------------------------------------------
if not exist scripts\engine mkdir scripts\engine
copy /Y %ENGINE%\scripts\engine\*.lua scripts\engine\ >nul

echo.
echo === Build successful! Run %NAME%_w10.exe ===
goto end

:error
echo.
echo === Build FAILED ===
pause
endlocal
exit /b 1

:end
endlocal
exit /b 0
