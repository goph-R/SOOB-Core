/*
 * config.h for FLTK 1.3.11 — SOOB level editor build (Dev-C++ MinGW 3.4, Win98).
 *
 * Hand-authored (no configure/CMake run) from ide/VisualC6/config.h — the
 * Win9x-era MSVC config — with the bundled image libraries (libpng / zlib /
 * libjpeg) turned OFF: build_fltk.bat builds only libfltk.a (core) and
 * libfltk_gl.a (GL), NOT libfltk_images.a. The editor loads its own textures
 * via the engine's stb-based texture.h, so FLTK never needs image codecs.
 *
 * Consumed via -I<fltk root> so the FLTK sources' `#include <config.h>`
 * resolve here. See build_fltk.bat.
 */

#define FLTK_DATADIR "C:/FLTK"
#define FLTK_DOCDIR "C:/FLTK/DOC"
#define BORDER_WIDTH 2

/* OpenGL — required by the editor's Fl_Gl_Window viewport. */
#define HAVE_GL 1
#define HAVE_GL_GLU_H 1

#define USE_COLORMAP 1

/* X11 double-buffer / overlay — not applicable on Windows. */
#define HAVE_XDBE 0
#define USE_XDBE HAVE_XDBE
#define HAVE_OVERLAY 0
#define HAVE_GL_OVERLAY 1

#define WORDS_BIGENDIAN 0
#define U16 unsigned short
#define U32 unsigned
#undef  U64

/* FLTK supplies its own vsnprintf/snprintf when these are undefined. */
#undef  HAVE_VSNPRINTF
#undef  HAVE_SNPRINTF

#define HAVE_STRCASECMP 1
#define HAVE_LOCALE_H 1
#define HAVE_LOCALECONV 1
#define HAVE_POLL 0

/*
 * Bundled image libraries DISABLED. Fl_PNG_Image / Fl_JPEG_Image live in the
 * separate fltk_images library (not built here); with these undefined nothing
 * in the core pulls <png.h> / <zlib.h> / <jpeglib.h>, so the editor links
 * against libfltk + libfltk_gl alone.
 */
#undef  HAVE_LIBPNG
#undef  HAVE_LIBZ
#undef  HAVE_LIBJPEG
#undef  HAVE_PNG_H
#undef  HAVE_LIBPNG_PNG_H
#undef  HAVE_PNG_GET_VALID
#undef  HAVE_PNG_SET_TRNS_TO_ALPHA
