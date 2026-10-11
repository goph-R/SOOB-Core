/*
 * edit_filedlg.h -- Open / Save dialogs that look right on Windows 98.
 *
 * FLTK offers two file dialogs and neither is quite what we want here:
 *
 *   fl_file_chooser()        FLTK's own, drawn with FLTK widgets. Works on 98
 *                            (after the scandir patch) but looks like FLTK,
 *                            not like Windows.
 *   Fl_Native_File_Chooser   wraps the platform dialog -- but on Windows it is
 *                            OPENFILENAMEW / GetOpenFileNameW / GetSaveFileNameW
 *                            / SHBrowseForFolderW throughout, and the wide API
 *                            is stubbed out on 95/98/ME. Unusable there.
 *
 * The ANSI comdlg32 entry points work perfectly well on Win98, so this calls
 * GetOpenFileNameA / GetSaveFileNameA directly and gets the genuine period
 * dialog. Off Windows it falls back to fl_file_chooser(), so the same call
 * compiles and behaves on the Linux build.
 *
 * -lcomdlg32 is already in t98.bat / c98.bat / f98.bat, so there is nothing to
 * add to the build.
 *
 * AND ONE OWNER
 *
 * hwndOwner must be the application's own window. With NULL the dialog is
 * owned by the desktop, so it is not modal to us: the main window can be
 * raised over the top of it, clicks land behind it, and on 9x the keyboard
 * focus goes with whichever window was clicked last -- the dialog looks
 * stuck. Callers pass it in once with codeFileDlgSetOwner(fl_xid(window));
 * it stays NULL (= the old behaviour) for anyone who does not.
 *
 * TWO FLAGS THAT MATTER
 *
 *   OFN_NOCHANGEDIR  -- without it the dialog leaves the process sitting in
 *                       whatever directory the user browsed to. The engine and
 *                       editor both resolve assets relative to the working
 *                       directory, so losing it breaks texture/model loading
 *                       after the first Open. Non-negotiable here.
 *   OFN_EXPLORER     -- selects the Win95+ dialog. Without it comdlg32 serves
 *                       the Windows 3.1 style one, which is not the look we are
 *                       after on 98.
 *
 * AND ONE SIZE TRAP
 *
 * OPENFILENAME grew three members (pvReserved, dwReserved, FlagsEx) at
 * _WIN32_WINNT >= 0x0500 -- which is exactly what these builds compile at, per
 * docs/editor-fltk-win98.md. Older comdlg32 versions reject the larger
 * lStructSize with CDERR_STRUCTSIZE and simply never show a dialog. Rather than
 * probe the SDK for a constant MinGW 3.4 may not define, we try the compiled
 * size and retry at the legacy 76 bytes if comdlg32 objects. Self-correcting,
 * and it reports anything else through CommDlgExtendedError().
 */
#ifndef EDIT_FILEDLG_H
#define EDIT_FILEDLG_H

#include <string.h>
#include <stdio.h>

#if defined(WIN32) || defined(_WIN32)
#  include <windows.h>
#  include <commdlg.h>
#else
#  include <FL/Fl_File_Chooser.H>
#endif

/* Owner window for the dialogs -- see "AND ONE OWNER" above. Held as void*
 * rather than HWND so this declaration needs no FLTK platform header: FL/x.H
 * drags in the X11 headers on the Linux build, where there is no HWND at all.
 * The app passes fl_xid(window); off Windows nothing reads it. */
static void *codeFileDlgOwner = 0;
static void codeFileDlgSetOwner(void *nativeWindowHandle)
{
    codeFileDlgOwner = nativeWindowHandle;
}

/* Win32 filter: pairs of NUL-terminated strings, list ends with an extra NUL.
 * The trailing "\0" in the literal supplies that second terminator. */
/* "All files" is deliberately FIRST, so nFilterIndex = 1 selects it and the
 * dialog opens unfiltered. On Windows 98 the Explorer-style comdlg32 dialog
 * stalls for about a second per file whose extension has no HKEY_CLASSES_ROOT
 * entry (a ".utf8" next to the sources is enough) while a real pattern list is
 * active -- it does an association lookup per rejected file, and the Win9x
 * registry is slow. "*.*" short-circuits that path. The bug is comdlg32's, not
 * ours: Photoshop 7 on the same machine freezes identically. The narrower
 * filters below are still there for anyone who picks one.
 *
 * "Source files" lists every extension lexLangFromPath() recognises, so it
 * covers everything the editor can highlight. Keep the two macros in step. */
#define CODE_FILTER_WIN32 \
    "All files (*.*)\0*.*\0" \
    "Source files\0*.lua;*.pas;*.pp;*.inc;*.dpr;*.lpr;*.md;" \
        "*.c;*.cc;*.cpp;*.cxx;*.h;*.hh;*.hpp;*.hxx;*.java;*.js;*.mjs;*.cjs;*.jsx;" \
        "*.py;*.pyw;*.css;*.htm;*.html;*.xhtml;*.php;*.phtml;*.sql;" \
        "*.sh;*.bash;*.bat;*.cmd;" \
        "*.ini;*.cfg;*.inf;*.xml;*.xsd;*.xsl;*.xslt;*.svg;*.json;" \
        "*.glsl;*.vert;*.frag;*.inl;*.incl;*.rc;*.ts;*.tsx;" \
        "*.tmx;*.qrc;*.csproj;*.props\0" \
    "Lua scripts (*.lua)\0*.lua\0" \
    "Pascal (*.pas;*.pp;*.inc;*.dpr)\0*.pas;*.pp;*.inc;*.dpr\0" \
    "Markdown (*.md)\0*.md\0" \
    "PHP (*.php;*.phtml)\0*.php;*.phtml\0" \
    "SQL (*.sql)\0*.sql\0" \
    "Shell scripts (*.sh;*.bash)\0*.sh;*.bash\0" \
    "Batch files (*.bat;*.cmd)\0*.bat;*.cmd\0" \
    "INI / config (*.ini;*.cfg;*.inf)\0*.ini;*.cfg;*.inf\0" \
    "XML (*.xml;*.xsd;*.xsl;*.xslt;*.svg)\0*.xml;*.xsd;*.xsl;*.xslt;*.svg\0" \
    "JSON (*.json)\0*.json\0"

/* FLTK's own chooser uses a different syntax for the same idea. */
#define CODE_FILTER_FLTK \
    "Source files\t*.{lua,pas,pp,inc,dpr,lpr,md,c,cc,cpp,cxx,h,hh,hpp,hxx,java,js,mjs,cjs,jsx," \
        "py,pyw,css,htm,html,xhtml,php,phtml,sql,sh,bash,bat,cmd," \
        "ini,cfg,inf,xml,xsd,xsl,xslt,svg,json," \
        "glsl,vert,frag,inl,incl,rc,ts,tsx,tmx,qrc,csproj,props}"

#if defined(WIN32) || defined(_WIN32)

/* Legacy OPENFILENAME size on 32-bit Windows, i.e. offsetof(OPENFILENAMEA,
 * pvReserved). Hard-coded because MinGW 3.4 may not declare the member at all,
 * which would make offsetof() fail to compile. Only ever used as a fallback
 * after comdlg32 has explicitly complained. */
#define CODE_OFN_SIZE_V400 76

static int codeFileDlgWin32(char *out, int outlen, int saving,
                            const char *title, const char *initialDir,
                            const char *suggestedName)
{
    OPENFILENAMEA ofn;
    char buf[MAX_PATH];
    DWORD err;

    if (!out || outlen <= 0) return 0;
    out[0] = '\0';

    buf[0] = '\0';
    if (suggestedName && *suggestedName) {
        strncpy(buf, suggestedName, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    }

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize     = sizeof(OPENFILENAMEA);
    ofn.hwndOwner       = (HWND)codeFileDlgOwner;
    ofn.lpstrFilter     = CODE_FILTER_WIN32;
    ofn.nFilterIndex    = 1;         /* "All files" -- see CODE_FILTER_WIN32 */
    ofn.lpstrFile       = buf;
    ofn.nMaxFile        = sizeof(buf);
    ofn.lpstrTitle      = title;
    ofn.lpstrInitialDir = initialDir;
    ofn.lpstrDefExt     = saving ? "lua" : NULL;
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR
              | (saving ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));

    if (saving ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)) {
        strncpy(out, buf, (size_t)(outlen - 1));
        out[outlen - 1] = '\0';
        return 1;
    }

    /* 0 means cancelled OR failed -- CommDlgExtendedError() tells them apart. */
    err = CommDlgExtendedError();
    if (err == CDERR_STRUCTSIZE) {
        ofn.lStructSize = CODE_OFN_SIZE_V400;      /* pre-0x0500 comdlg32 */
        if (saving ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)) {
            strncpy(out, buf, (size_t)(outlen - 1));
            out[outlen - 1] = '\0';
            return 1;
        }
        err = CommDlgExtendedError();
    }
    if (err != 0)
        fprintf(stderr, "codeFileDlg: CommDlgExtendedError = 0x%lx\n", (unsigned long)err);
    return 0;                                      /* err == 0 -> user cancelled */
}

#endif /* WIN32 */

/* Open dialog. Returns 1 and fills `out` with a path, or 0 if cancelled. */
static int codeFileOpenDialog(char *out, int outlen, const char *initialDir)
{
#if defined(WIN32) || defined(_WIN32)
    return codeFileDlgWin32(out, outlen, 0, "Open", initialDir, NULL);
#else
    const char *p = fl_file_chooser("Open", CODE_FILTER_FLTK, initialDir, 0);
    if (!p || !out || outlen <= 0) { if (out && outlen > 0) out[0] = '\0'; return 0; }
    strncpy(out, p, (size_t)(outlen - 1));         /* p is STATIC storage: copy now */
    out[outlen - 1] = '\0';
    return 1;
#endif
}

/* Save-as dialog. `suggestedName` may be NULL. Returns 1 and fills `out`. */
static int codeFileSaveDialog(char *out, int outlen, const char *initialDir,
                              const char *suggestedName)
{
#if defined(WIN32) || defined(_WIN32)
    return codeFileDlgWin32(out, outlen, 1, "Save As", initialDir, suggestedName);
#else
    const char *p = fl_file_chooser("Save As", CODE_FILTER_FLTK,
                                    suggestedName ? suggestedName : initialDir, 0);
    if (!p || !out || outlen <= 0) { if (out && outlen > 0) out[0] = '\0'; return 0; }
    strncpy(out, p, (size_t)(outlen - 1));
    out[outlen - 1] = '\0';
    return 1;
#endif
}

/* ---- folder picker -------------------------------------------------------
 *
 * The Folders sidebar needs a directory, not a file, and the note at the top
 * of this header is exactly why Fl_Native_File_Chooser is no help: it reaches
 * for SHBrowseForFolderW. The ANSI SHBrowseForFolderA, though, is real on
 * Windows 98 -- it is only the wide half of the shell that is stubbed -- so
 * the genuine "Browse for Folder" dialog is one call away, and the app gets
 * the same period look its Open and Save dialogs already have.
 *
 * TWO FLAGS NOT SET, DELIBERATELY
 *
 *   BIF_NEWDIALOGSTYLE  -- the resizable dialog with a New Folder button.
 *                          Needs shell32 5.0 (Windows 2000) and a
 *                          CoInitialize'd thread; Win98 gold ships 4.72 and
 *                          answers with no dialog at all.
 *   BIF_USENEWUI        -- the same thing wearing a different name.
 *
 * The starting folder is not a field but a message: the dialog asks us where
 * to begin by calling back with BFFM_INITIALIZED, and we answer with
 * BFFM_SETSELECTIONA. If that callback ever fails to compile on an old SDK,
 * dropping bi.lpfn costs only the starting position.
 */
#if defined(WIN32) || defined(_WIN32)

#include <shlobj.h>

/* MinGW 3.4's w32api predates some of these. All are fixed constants. */
#ifndef BIF_RETURNONLYFSDIRS
#  define BIF_RETURNONLYFSDIRS 0x0001
#endif
#ifndef BFFM_INITIALIZED
#  define BFFM_INITIALIZED 1
#endif
#ifndef BFFM_SETSELECTIONA
#  define BFFM_SETSELECTIONA (WM_USER + 102)
#endif

static int CALLBACK codeFolderDlgProc(HWND hw, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data)
        SendMessageA(hw, BFFM_SETSELECTIONA, (WPARAM)TRUE, data);
    return 0;
}

#endif /* WIN32 */

/* Pick a folder. Returns 1 and fills `out` with a path, or 0 if cancelled. */
static int codeFolderDialog(char *out, int outlen, const char *initialDir)
{
#if defined(WIN32) || defined(_WIN32)
    BROWSEINFOA bi;
    LPITEMIDLIST pidl;
    char name[MAX_PATH], start[MAX_PATH];
    int ok = 0;

    if (!out || outlen <= 0) return 0;
    out[0] = '\0';
    name[0] = '\0';

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner      = (HWND)codeFileDlgOwner;   /* modal to us: see above */
    bi.pszDisplayName = name;                     /* the leaf NAME, not a path */
    bi.lpszTitle      = "Add a folder to the sidebar";
    bi.ulFlags        = BIF_RETURNONLYFSDIRS;
    if (initialDir && *initialDir) {
        strncpy(start, initialDir, sizeof(start) - 1);
        start[sizeof(start) - 1] = '\0';
        bi.lpfn   = codeFolderDlgProc;
        bi.lParam = (LPARAM)start;
    }

    pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return 0;                          /* cancelled */
    /* pszDisplayName is only the leaf ("Projects"); the full path has to be
       resolved out of the item ID list. */
    if (SHGetPathFromIDListA(pidl, name)) {
        strncpy(out, name, (size_t)(outlen - 1));
        out[outlen - 1] = '\0';
        ok = out[0] != '\0';
    }
    /* The PIDL belongs to the shell allocator. CoTaskMemFree is the
       documented stand-in for SHGetMalloc()->Free() and needs no
       CoInitialize; ole32 is already on the link line. */
    CoTaskMemFree((LPVOID)pidl);
    return ok;
#else
    const char *p = fl_dir_chooser("Add a folder to the sidebar", initialDir, 0);
    if (!p || !out || outlen <= 0) { if (out && outlen > 0) out[0] = '\0'; return 0; }
    strncpy(out, p, (size_t)(outlen - 1));         /* p is STATIC storage */
    out[outlen - 1] = '\0';
    return 1;
#endif
}

#endif /* EDIT_FILEDLG_H */
