/*
 * Unity-build aggregator for Lua 5.1.5.
 *
 * Compile this single TU and get the full Lua core + stdlib as one object
 * file (`lua.o`). Excluded: lua.c and luac.c (each has its own main()),
 * print.c (debug helper for luac, not needed by the embedded runtime).
 *
 * Build as C, not C++. The `-Dluaall_c` flag is mandatory — it's Lua's
 * own switch for unity builds, and without it loslib.c fails to find
 * LUA_TMPNAMBUFSIZE because luaconf.h gates that macro on `luaall_c`.
 *
 *     gcc -x c -c vendor/lua-5.1.5/src/lua_all.c -o lua.o \
 *         -Ivendor/lua-5.1.5/src -Dluaall_c
 *
 * On Linux add -DLUA_USE_LINUX for POSIX features (mkstemp, isatty, popen,
 * dlopen-based package loader). On Win98/MinGW 3.4 add -DLUA_ANSI so
 * loadlib.c doesn't try to use DLL or dlopen APIs the old toolchain lacks.
 */

/* stdio must come first: ldebug.h defines a `getline` macro, which once
   included would corrupt glibc's getline() function prototype declared
   by stdio.h. In normal (per-file) builds this never collides because
   the files that include stdio don't also pull in ldebug.h; the unity
   build breaks that isolation, so we preempt it here. */
#include <stdio.h>

/* Core VM */
#include "lapi.c"
#include "lcode.c"
#include "ldebug.c"
#include "ldo.c"
#include "ldump.c"
#include "lfunc.c"
#include "lgc.c"
#include "llex.c"
#include "lmem.c"
#include "lobject.c"
#include "lopcodes.c"
#include "lparser.c"
#include "lstate.c"
#include "lstring.c"
#include "ltable.c"
#include "ltm.c"
#include "lundump.c"
#include "lvm.c"
#include "lzio.c"

/* Auxiliary library */
#include "lauxlib.c"

/* Standard libraries */
#include "lbaselib.c"
#include "ldblib.c"
#include "liolib.c"
#include "lmathlib.c"
#include "loslib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "loadlib.c"

/* Opens all of the above */
#include "linit.c"
