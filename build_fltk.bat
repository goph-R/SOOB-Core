@echo off
setlocal

echo === FLTK 1.3.11 static libs for the SOOB editor (Dev-C++ MinGW 3.4) ===
echo.

REM ----------------------------------------------------------------
REM  Builds libfltk.a (core) + libfltk_gl.a (GL) directly with the
REM  Dev-C++ toolchain -- no configure, no CMake, no MSYS. The exact
REM  source lists come from FLTK's own Makefile (fltk_core.list /
REM  fltk_gl.list, next to this tree) so only the cross-platform
REM  dispatcher .cxx are compiled; the _win32 backends are #included
REM  by them, not compiled standalone.
REM
REM  config.h is provided at %FLROOT%\config.h (image libs disabled).
REM  Delete raw\obj\fltk to force a full rebuild.
REM ----------------------------------------------------------------

set "GXX=C:\Dev-Cpp\bin\g++.exe"
set "GCC=C:\Dev-Cpp\bin\gcc.exe"
set "AR=C:\Dev-Cpp\bin\ar.exe"
set "FLROOT=vendor\fltk-1.3\FL"
set "LISTDIR=vendor\fltk-1.3"
set "OBJ=raw\obj\fltk"
set "LIBDIR=%FLROOT%\lib"

if not exist "%FLROOT%\FL\Fl.H" (
    echo ERROR: FLTK source not found. Expected %FLROOT%\FL\Fl.H
    goto error
)
if not exist "%FLROOT%\config.h" (
    echo ERROR: %FLROOT%\config.h missing ^(should ship with the editor^).
    goto error
)

if exist "%LIBDIR%\libfltk_gl.a" (
    echo Libraries already built ^(delete raw\obj\fltk to force a rebuild^).
    goto done
)

if not exist "%OBJ%\c" mkdir "%OBJ%\c"
if not exist "%OBJ%\g" mkdir "%OBJ%\g"
if not exist "%LIBDIR%" mkdir "%LIBDIR%"

REM  -DWIN32 selects FLTK's Windows backend. WINVER/_WIN32_WINNT = 0x0500:
REM  FLTK 1.3.11's Fl_win32.cxx references a few Win2000+ symbols at compile
REM  time (XBUTTON1/GET_XBUTTON_WPARAM for extra mouse buttons; BITMAPV5HEADER
REM  in image_to_icon for alpha window-icons/RGB cursors), so 0x0400 won't
REM  compile. Building at 0x0500 only makes those symbols DECLARED -- the win32
REM  backend has no version-gated code blocks, so no new runtime paths are
REM  enabled, and both sites are dead code at runtime for the editor (Win98
REM  never sends WM_XBUTTON*, and the editor sets no RGB icon/cursor). The
REM  functions involved (CreateDIBSection, etc.) all exist on Win98, so the
REM  binary still loads and runs there. FL_LIBRARY is set while building the lib.
set "CXXFLAGS=-O2 -w -DWIN32 -DWINVER=0x0500 -D_WIN32_WINNT=0x0500 -DFL_LIBRARY -I%FLROOT% -I%FLROOT%\src"

REM  .c files must be compiled as C (gcc), not C++ (g++): FLTK's UTF-16 code
REM  passes `unsigned short*` to the ...W() APIs, which is valid in C (where
REM  wchar_t IS unsigned short) but an invalid conversion in C++ (where wchar_t
REM  is a distinct type). Matches FLTK's own build ($(CC) for CFILES).
echo Compiling FLTK core (this takes a while)...
for /f %%f in (%LISTDIR%\fltk_core.list) do (
    if /I "%%~xf"==".c" (
        %GCC% %CXXFLAGS% -c "%FLROOT%/src/%%f" -o "%OBJ%\c\%%~nf.o"
    ) else (
        %GXX% %CXXFLAGS% -c "%FLROOT%/src/%%f" -o "%OBJ%\c\%%~nf.o"
    )
    if errorlevel 1 (echo   FAILED: %%f & goto error)
)

echo Compiling FLTK GL...
for /f %%f in (%LISTDIR%\fltk_gl.list) do (
    %GXX% %CXXFLAGS% -c "%FLROOT%/src/%%f" -o "%OBJ%\g\%%~nf.o"
    if errorlevel 1 (echo   FAILED: %%f & goto error)
)

echo Archiving libfltk.a ...
if exist "%LIBDIR%\libfltk.a" del "%LIBDIR%\libfltk.a"
for %%o in (%OBJ%\c\*.o) do %AR% rcs "%LIBDIR%\libfltk.a" "%%o"

echo Archiving libfltk_gl.a ...
if exist "%LIBDIR%\libfltk_gl.a" del "%LIBDIR%\libfltk_gl.a"
for %%o in (%OBJ%\g\*.o) do %AR% rcs "%LIBDIR%\libfltk_gl.a" "%%o"

:done
echo.
echo === FLTK ready: %LIBDIR%\libfltk.a + libfltk_gl.a ===
echo Now run the editor builds: build_editor.bat in SOOB-Engine, e98.bat in SOOB-Code.
goto end

:error
echo.
echo === FLTK BUILD FAILED ===
echo If a single file breaks on GCC 3.4, note which one -- late 1.3.x
echo patches occasionally need a small tweak on the old compiler.
pause

:end
endlocal
