#ifndef EDIT_SHELL_H
#define EDIT_SHELL_H

/*
 * edit_shell.h -- hand a file to the operating system.
 *
 * The Folders tree lists every file in a folder, not just the ones the editor
 * can read, so double-clicking a .png or a .doc has to mean "ask Windows what
 * to do with this". That is ShellExecuteA, and this wraps it.
 *
 * NOT fl_open_uri(). FLTK's version takes a URI, not a path -- on Windows it
 * hands the string to ShellExecute much as this does, but a bare "C:\x\y.png"
 * is not a URI and building a file:// one means percent-escaping a path we
 * already have in the exact form the shell wants. It also has no fallback for
 * an unregistered extension, which on a Win98 box is most of them.
 *
 * ShellExecuteA returns an HINSTANCE that is really an error code: anything
 * over 32 is success. SE_ERR_NOASSOCIATION (31) means the extension has no
 * handler, and the friendly answer there is the "Open With..." dialog -- but
 * the "openas" verb is Windows 2000 and later, so on 9x it has to be raised
 * the long way round, through rundll32. Both spellings are tried, newest
 * first, because the exe is one binary for both targets.
 */

#include <string.h>
#include <stdio.h>

#if defined(WIN32) || defined(_WIN32)
#  include <windows.h>
#  include <shellapi.h>
#  ifndef SE_ERR_NOASSOCIATION
#    define SE_ERR_NOASSOCIATION 31
#  endif
#else
#  include <stdlib.h>
#endif

/* Open `path` with whatever the OS thinks owns it. 1 if the shell took it. */
static int codeShellOpen(const char *path)
{
#if defined(WIN32) || defined(_WIN32)
    HINSTANCE r;
    char arg[600];

    if (!path || !*path) return 0;
    /* lpFile is a parameter, not a command line, so it must NOT be quoted:
     * quoting would break every path that has no space in it. */
    r = ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
    if ((int)(long)r > 32) return 1;
    if ((int)(long)r != SE_ERR_NOASSOCIATION) return 0;

    /* Nothing is registered for this extension. Offer "Open With...". */
    r = ShellExecuteA(NULL, "openas", path, NULL, NULL, SW_SHOWNORMAL);
    if ((int)(long)r > 32) return 1;              /* Windows 2000 and later */
    sprintf(arg, "shell32.dll,OpenAs_RunDLL %.500s", path);
    r = ShellExecuteA(NULL, "open", "rundll32.exe", arg, NULL, SW_SHOWNORMAL);
    return (int)(long)r > 32;                     /* the 9x spelling */
#else
    /* Single-quoted for the shell, with the usual '\'' dance for a quote in
     * the name itself. Backgrounded so a slow viewer cannot wedge the editor;
     * system() then reports only whether the SHELL started, which is all the
     * caller can act on anyway. */
    char cmd[1400], esc[700];
    const char *p;
    char *o;

    if (!path || !*path) return 0;
    o = esc;
    for (p = path; *p && o < esc + sizeof(esc) - 6; p++) {
        if (*p == '\'') { memcpy(o, "'\\''", 4); o += 4; }
        else            { *o++ = *p; }
    }
    *o = '\0';
    sprintf(cmd, "xdg-open '%s' >/dev/null 2>&1 &", esc);
    return system(cmd) == 0;
#endif
}

#endif /* EDIT_SHELL_H */
