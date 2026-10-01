// fl_win98_compat.h — SOOB-editor build shim (NOT upstream FLTK).
//
// Dev-C++'s bundled MinGW 3.4 w32api predates a handful of Win2000-era GDI
// declarations that FLTK 1.3.11 references unconditionally, so they aren't
// present at any _WIN32_WINNT / WINVER. This header supplies just those,
// guarded so a newer SDK is left untouched.
//
// Every symbol here is DEAD CODE at runtime on Win98:
//   * WM_XBUTTON* messages are never sent (extra mouse buttons are Win2000+);
//   * image_to_icon() only runs when an app sets an Fl_RGB_Image icon / RGB
//     cursor, which the SOOB editor never does;
//   * GetGlyphIndicesW is loaded via GetProcAddress and is NULL on Win98, so
//     the GGI_* path is skipped for a GetCharacterPlacementW fallback.
// The functions actually called (CreateDIBSection, CreateIconIndirect, …) all
// exist on Win98, so nothing becomes an unresolvable import.
//
// MUST be included AFTER <windows.h> (both include sites are #included into a
// dispatcher .cxx that has already pulled it in via FL/x.H), because the
// BITMAPV5HEADER definition uses DWORD/LONG/WORD/CIEXYZTRIPLE.

#ifndef FL_WIN98_COMPAT_H
#define FL_WIN98_COMPAT_H

// Extra mouse-button constants (winuser.h, _WIN32_WINNT >= 0x0500).
#ifndef XBUTTON1
#  define XBUTTON1 0x0001
#endif
#ifndef XBUTTON2
#  define XBUTTON2 0x0002
#endif
#ifndef GET_XBUTTON_WPARAM
#  define GET_XBUTTON_WPARAM(wParam) ((WORD)(HIWORD(wParam)))
#endif

// GetGlyphIndicesW / GetGlyphOutlineW flags (wingdi.h, WINVER >= 0x0500). Only
// reached after GetGlyphIndicesW loads, which it can't on Win98, so dead there.
#ifndef GGI_MARK_NONEXISTING_GLYPHS
#  define GGI_MARK_NONEXISTING_GLYPHS 0x0001
#endif
#ifndef GGO_GLYPH_INDEX
#  define GGO_GLYPH_INDEX 0x0080
#endif

// BITMAPV5HEADER (wingdi.h, WINVER >= 0x0500). PROFILE_LINKED is defined in the
// same header block, so its absence is a reliable proxy for "no V5 header".
#ifndef PROFILE_LINKED
typedef struct {
  DWORD        bV5Size;
  LONG         bV5Width;
  LONG         bV5Height;
  WORD         bV5Planes;
  WORD         bV5BitCount;
  DWORD        bV5Compression;
  DWORD        bV5SizeImage;
  LONG         bV5XPelsPerMeter;
  LONG         bV5YPelsPerMeter;
  DWORD        bV5ClrUsed;
  DWORD        bV5ClrImportant;
  DWORD        bV5RedMask;
  DWORD        bV5GreenMask;
  DWORD        bV5BlueMask;
  DWORD        bV5AlphaMask;
  DWORD        bV5CSType;
  CIEXYZTRIPLE bV5Endpoints;
  DWORD        bV5GammaRed;
  DWORD        bV5GammaGreen;
  DWORD        bV5GammaBlue;
  DWORD        bV5Intent;
  DWORD        bV5ProfileData;
  DWORD        bV5ProfileSize;
  DWORD        bV5Reserved;
} BITMAPV5HEADER;
#endif

#endif // FL_WIN98_COMPAT_H
