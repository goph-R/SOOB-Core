#!/bin/sh
#
# test_linux.sh -- run the fltk_ui headless tests on Linux.
#
#   tools/test_linux.sh            build FLTK if needed, then run both tests
#   tools/test_linux.sh -f         force a fresh FLTK build first
#
# WHY THIS EXISTS
#
# edit_code_model_test.cpp links FLTK, and it cannot link the SYSTEM FLTK:
# edit_code.h refers to fl_text_display_longest_line, which is one of our
# patches to vendor/fltk-1.3 and does not exist upstream. So the test needs
# our patched FLTK built for Linux -- which the repo does not ship, because
# vendor/fltk-1.3/FL/lib holds the Win98 objects and FL/config.h plus
# FL/FL/abi-version.h are the tracked WINDOWS ones.
#
# Running configure inside vendor/fltk-1.3/FL would overwrite both of those
# and break the Win98 build. So this works on a COPY, outside the repo: the
# repo is also a share the Win98 machine sees, and nothing Linux-shaped
# should land in it.
#
# The ABI is taken from the tracked abi-version.h rather than hardcoded, so
# the Linux library can never disagree with what the Windows builds use --
# that mismatch is exactly the silent-corruption trap codeedit.cpp checks for
# at startup.
set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC="$REPO/vendor/fltk-1.3/FL"
OUT=${SOOB_FLTK_LINUX:-${XDG_CACHE_HOME:-$HOME/.cache}/soob-fltk-linux}
BIN="$OUT/bin"

[ "$1" = "-f" ] && rm -rf "$OUT"

if [ ! -f "$OUT/fltk-lin/lib/libfltk.a" ]; then
    ABI=$(sed -n 's/^#define FL_ABI_VERSION  *\([0-9]*\).*/\1/p' "$SRC/FL/abi-version.h")
    [ -n "$ABI" ] || { echo "cannot read FL_ABI_VERSION from $SRC/FL/abi-version.h" >&2; exit 1; }
    echo "=== building patched FLTK for Linux (ABI $ABI) in $OUT ==="
    mkdir -p "$OUT"
    rm -rf "$OUT/fltk-lin"
    cp -a "$SRC" "$OUT/fltk-lin"
    # cp -a brings the Win98 objects and their fltkok.tag sentinel along.
    rm -rf "$OUT/fltk-lin/lib" "$OUT/fltk-lin/lib_w10"
    mkdir -p "$OUT/fltk-lin/lib"
    cd "$OUT/fltk-lin"
    # --disable-xft: no freetype/xft dev packages needed, and these tests
    # never open a display. --disable-gl: nothing here draws in 3D.
    chmod +x configure
    ./configure --with-abiversion="$ABI" --disable-gl --disable-xft \
                --enable-localzlib --enable-localjpeg --enable-localpng \
                > "$OUT/configure.log" 2>&1 \
        || { echo "configure failed -- see $OUT/configure.log" >&2; exit 1; }
    make -C src -j"$(nproc)" ../lib/libfltk.a > "$OUT/build.log" 2>&1 \
        || { echo "FLTK build failed -- see $OUT/build.log" >&2; exit 1; }
    echo "=== libfltk.a built ==="
fi

FLTK_CONFIG="$OUT/fltk-lin/fltk-config"
chmod +x "$FLTK_CONFIG"
mkdir -p "$BIN"
cd "$REPO"

echo "=== edit_code_test (lexers, no FLTK) ==="
g++ -O1 -Wall fltk_ui/edit_code_test.cpp -o "$BIN/ectest" -I. -Ifltk_ui
"$BIN/ectest"

echo "=== edit_code_model_test (buffer model, links FLTK) ==="
g++ -O1 -Wall fltk_ui/edit_code_model_test.cpp -o "$BIN/ecmtest" \
    -I. -Ifltk_ui $("$FLTK_CONFIG" --cxxflags) $("$FLTK_CONFIG" --ldflags)
# No DISPLAY: these construct buffers, never widgets. Unsetting it here means
# a test that accidentally opens a window fails loudly instead of silently
# depending on the developer's X session.
env -u DISPLAY "$BIN/ecmtest"
