#ifndef EDIT_FILEIO_H
#define EDIT_FILEIO_H

/*
 * edit_fileio.h -- read and write text files keeping their encoding and line
 * ending, for the code editor.
 *
 * Fl_Text_Buffer holds UTF-8 with LF line ends. Its own loadfile()/savefile()
 * read non-UTF-8 input as Latin-1 (wrong for a Central European ANSI file --
 * code page 1250 here) and always write UTF-8 through a text-mode stream, so
 * every file came back as UTF-8 + CRLF. Instead:
 *
 *   load: UTF-8 BOM -> CODE_ENC_UTF8_BOM; valid UTF-8 (incl. plain ASCII) ->
 *         CODE_ENC_UTF8; anything else -> CODE_ENC_ANSI, converted from the
 *         system ANSI code page. The first line break decides CRLF vs LF.
 *   save: the same encoding and line ending back out.
 *
 * ANSI conversion goes through MultiByteToWideChar / WideCharToMultiByte with
 * CP_ACP, which Windows 98 has; FLTK's own fl_utf8towc / fl_utf8fromwc do the
 * UTF-16 <-> UTF-8 half, so CP_UTF8 (missing on Win95) is never needed. Off
 * Windows, ANSI means Latin-1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <FL/fl_utf8.h>
#include <FL/Fl_Text_Buffer.H>
#ifdef _WIN32
#include <windows.h>
#endif

/* How many bytes of a line to keep when trimming trailing whitespace.
 *
 * `keepMdBreak` is for Markdown, where two trailing SPACES are a hard line
 * break rather than stray whitespace -- so a line with content whose trailing
 * run is spaces only, and at least two of them, keeps exactly two. A run that
 * contains a tab is not a line break in any Markdown implementation and is
 * trimmed away. A line that is nothing but whitespace keeps none of it: there
 * is no content for a break to attach to.
 *
 * Pure, so it is unit-tested in edit_code_model_test.cpp rather than by
 * saving files on the target. */
static int codeTrimKeep(const char *s, int n, int keepMdBreak)
{
    int e = n, i, sp = 0;
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    if (e == 0 || e == n) return e;           /* all blank, or nothing to trim */
    if (!keepMdBreak) return e;
    for (i = n; i > 0 && s[i - 1] == ' '; i--) sp++;
    return (sp >= 2 && i == e) ? e + 2 : e;
}

/* Strip trailing blanks from every line of `b`; returns the bytes removed.
 *
 * Walks from the LAST line backwards, so a removal can never shift a position
 * still to be visited -- which is what keeps this O(lines) and correct without
 * a second pass. *caret is a buffer position carried along with the text:
 * past a trimmed run it shifts back by what went, inside one it lands on the
 * new end of the line, and before one it does not move.
 *
 * Buffer-only, so the walk and that caret arithmetic are unit-tested in
 * edit_code_model_test.cpp; CodeEditor::trimTrailingBlanks() just wraps this
 * in an undo group. */
static int codeTrimBuffer(Fl_Text_Buffer *b, int keepMdBreak, int *caret)
{
    int removed = 0, ls, le, keep, pos = caret ? *caret : 0;
    if (!b) return 0;
    ls = b->line_start(b->length());
    for (;;) {
        le = b->line_end(ls);
        if (le > ls) {
            char *line = b->text_range(ls, le);
            if (line) {
                keep = codeTrimKeep(line, le - ls, keepMdBreak);
                free(line);
                if (keep < le - ls) {
                    int cut = le - (ls + keep);
                    if (pos >= le)            pos -= cut;
                    else if (pos > ls + keep) pos = ls + keep;
                    b->remove(ls + keep, le);
                    removed += cut;
                }
            }
        }
        if (ls == 0) break;
        ls = b->line_start(ls - 1);
    }
    if (caret) { if (pos > b->length()) pos = b->length(); *caret = pos; }
    return removed;
}

enum {
    CODE_ENC_UTF8 = 0,
    CODE_ENC_UTF8_BOM,
    CODE_ENC_ANSI
};

/* Line ending for files that have no line break to go by. */
#ifdef _WIN32
#define CODE_DEFAULT_CRLF 1
#else
#define CODE_DEFAULT_CRLF 0
#endif

static const char *codeEncName(int enc)
{
    if (enc == CODE_ENC_UTF8_BOM) return "UTF-8 BOM";
    if (enc == CODE_ENC_ANSI)     return "ANSI";
    return "UTF-8";
}

/* ANSI -> UTF-8. Returns a malloc'd, NUL-terminated string. */
static char *codeAnsiToUtf8(const char *src, int n)
{
    char *out;
#ifdef _WIN32
    int wn = n ? MultiByteToWideChar(CP_ACP, 0, src, n, NULL, 0) : 0;
    wchar_t *w = (wchar_t *)malloc((size_t)(wn + 1) * sizeof(wchar_t));
    unsigned un;
    if (!w) return NULL;
    if (wn) MultiByteToWideChar(CP_ACP, 0, src, n, w, wn);
    un = fl_utf8fromwc(NULL, 0, w, (unsigned)wn);
    out = (char *)malloc(un + 1);
    if (out) fl_utf8fromwc(out, un + 1, w, (unsigned)wn);
    free(w);
#else
    unsigned un = fl_utf8froma(NULL, 0, src, (unsigned)n);
    out = (char *)malloc(un + 1);
    if (out) fl_utf8froma(out, un + 1, src, (unsigned)n);
#endif
    return out;
}

/* UTF-8 -> ANSI. Returns a malloc'd buffer, its length in *outLen, and in
 * *lossy whether some character had no ANSI equivalent (written as '?'). */
static char *codeUtf8ToAnsi(const char *src, int n, int *outLen, int *lossy)
{
    char *out;
    *lossy = 0;
#ifdef _WIN32
    {
        unsigned wn = fl_utf8towc(src, (unsigned)n, NULL, 0);
        wchar_t *w = (wchar_t *)malloc((wn + 1) * sizeof(wchar_t));
        int mn;
        BOOL usedDefault = FALSE;
        if (!w) return NULL;
        fl_utf8towc(src, (unsigned)n, w, wn + 1);
        mn = wn ? WideCharToMultiByte(CP_ACP, 0, w, (int)wn, NULL, 0, NULL, NULL) : 0;
        out = (char *)malloc((size_t)mn + 1);
        if (out && mn)
            WideCharToMultiByte(CP_ACP, 0, w, (int)wn, out, mn, NULL, &usedDefault);
        free(w);
        *lossy = usedDefault ? 1 : 0;
        *outLen = mn;
    }
#else
    {
        unsigned mn = fl_utf8toa(src, (unsigned)n, NULL, 0);
        out = (char *)malloc(mn + 1);
        if (out) fl_utf8toa(src, (unsigned)n, out, mn + 1);
        *outLen = (int)mn;
    }
#endif
    return out;
}

/* Would saving this UTF-8 text as ANSI lose characters? */
static int codeAnsiIsLossy(const char *text, int n)
{
    int len, lossy;
    char *a = codeUtf8ToAnsi(text, n, &len, &lossy);
    free(a);
    return lossy;
}

/* Read a whole file. Returns malloc'd UTF-8 text with LF line ends (caller
 * frees), or NULL if it cannot be read. Sets *enc and *crlf. */
static char *codeReadText(const char *path, int *enc, int *crlf)
{
    FILE *f = fl_fopen(path, "rb");
    long n;
    char *raw, *data, *text;
    int dn, i, o;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    raw = (char *)malloc((size_t)n + 1);
    if (!raw) { fclose(f); return NULL; }
    if (n && fread(raw, 1, (size_t)n, f) != (size_t)n) { free(raw); fclose(f); return NULL; }
    fclose(f);
    raw[n] = '\0';

    data = raw;
    dn = (int)n;
    if (dn >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        *enc = CODE_ENC_UTF8_BOM;
        data += 3;
        dn -= 3;
    } else if (dn == 0 || fl_utf8test(data, (unsigned)dn)) {
        *enc = CODE_ENC_UTF8;
    } else {
        *enc = CODE_ENC_ANSI;
    }

    if (*enc == CODE_ENC_ANSI) {
        text = codeAnsiToUtf8(data, dn);
        free(raw);
        if (!text) return NULL;
    } else {
        memmove(raw, data, (size_t)dn);
        raw[dn] = '\0';
        text = raw;
    }

    /* line ending from the first break; then CRLF -> LF in place */
    *crlf = CODE_DEFAULT_CRLF;
    for (i = 0; text[i]; i++)
        if (text[i] == '\n') { *crlf = (i > 0 && text[i - 1] == '\r'); break; }
    for (i = 0, o = 0; text[i]; i++) {
        if (text[i] == '\r' && text[i + 1] == '\n') continue;
        text[o++] = text[i];
    }
    text[o] = '\0';
    return text;
}

/* Write UTF-8 text (LF line ends) out in the given encoding and line ending.
 * Returns 0 on success, 1 if the file cannot be written. */
static int codeWriteText(const char *path, const char *text, int n, int enc, int crlf)
{
    FILE *f;
    char *buf, *ansi = 0;
    const char *out;
    int i, o = 0, lines = 0, outLen, lossy, ok;

    for (i = 0; i < n; i++) if (text[i] == '\n') lines++;
    buf = (char *)malloc((size_t)n + (crlf ? lines : 0) + 1);
    if (!buf) return 1;
    for (i = 0; i < n; i++) {
        if (crlf && text[i] == '\n') buf[o++] = '\r';
        buf[o++] = text[i];
    }
    buf[o] = '\0';

    out = buf;
    outLen = o;
    if (enc == CODE_ENC_ANSI) {
        ansi = codeUtf8ToAnsi(buf, o, &outLen, &lossy);
        if (!ansi) { free(buf); return 1; }
        out = ansi;
    }

    f = fl_fopen(path, "wb");
    if (!f) { free(buf); free(ansi); return 1; }
    ok = 1;
    if (enc == CODE_ENC_UTF8_BOM && fwrite("\xEF\xBB\xBF", 1, 3, f) != 3) ok = 0;
    if (ok && outLen && fwrite(out, 1, (size_t)outLen, f) != (size_t)outLen) ok = 0;
    if (fclose(f) != 0) ok = 0;
    free(buf);
    free(ansi);
    return ok ? 0 : 1;
}

/* ---- will the editor take this file? -------------------------------------
 *
 * The Folders tree lists every entry in a folder, so it has to decide per
 * file whether a double-click opens a tab or goes to the shell -- and draw
 * the ones it will not open in a dimmed colour.
 *
 * lexLangFromPath() cannot answer this. It picks a HIGHLIGHTER, and its
 * LEX_LANG_TEXT fallback covers both ".txt", which opens perfectly well, and
 * "kernel32.dll", which does not. The two questions are unrelated.
 *
 * NAME ONLY, no I/O: this runs once per directory entry while the tree is
 * being built, and an fopen() per entry would make a 400-file folder on a
 * Win98 share painful. So no sniffing for NUL bytes, no size check.
 *
 * DENY WINS, AND THE DEFAULT IS NO. An unknown extension is reported as not
 * openable, because the cost of being wrong is asymmetric: refusing a text
 * file is a shrug and a trip through File > Open, while accepting a 40 MB
 * .iso means codeReadText() mallocs all of it and Fl_Text_Buffer lays out
 * several million lines of binary. Neither it nor CodeEditor::loadFile() has
 * a size cap or a binary check -- this list is the gate.
 *
 * To teach the editor a new extension, add it to codeOpenExt and nowhere
 * else. Keep CODE_FILTER_WIN32 in edit_filedlg.h in step if the Open dialog
 * should offer it too.
 */

#define CODE_NLIST(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Case-insensitive whole-string compare. ASCII folding only, which is all an
   extension or a bare filename can portably be. */
static int codeNameEqI(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* Linear, not a lexInTable() binary search: that would tie this header to
   edit_lex.h and impose a sorted-table invariant on three lists people will
   want to edit. A few dozen compares of three-character strings per file is
   nothing next to the directory read that produced the name. */
static int codeNameInList(const char *const *tab, int n, const char *s)
{
    int i;
    for (i = 0; i < n; i++) if (codeNameEqI(tab[i], s)) return 1;
    return 0;
}

/* Known binary. Checked first, so nothing below can resurrect one. */
static const char *const codeBinExt[] = {
    "7z", "a", "avi", "bin", "bmp", "bz2", "cab", "chm", "class", "com",
    "cur", "dat", "db", "dll", "doc", "docx", "dylib", "exe", "flac", "fon",
    "gif", "gz", "ico", "img", "iso", "jar", "jpeg", "jpg", "lib", "mdb",
    "mid", "mkv", "mov", "mp3", "mp4", "mpg", "msi", "o", "obj", "ocx",
    "odt", "ogg", "otf", "pcx", "pdb", "pdf", "png", "ppt", "pptx", "psd",
    "pyc", "pyo", "rar", "res", "so", "swf", "sys", "tar", "tga", "tif",
    "tiff", "ttf", "wav", "webp", "wmv", "xcf", "xls", "xlsx", "zip"
};

/* Text. Everything lexLangFromPath() knows, plus the plain-text extensions
   that legitimately land on its LEX_LANG_TEXT fallback. */
static const char *const codeOpenExt[] = {
    "ash", "asc", "bash", "bat", "c", "cc", "cfg", "cjs", "cmake", "cmd",
    "conf", "cpp", "csproj", "css", "csv", "cxx", "diff", "dpr", "frag",
    "glsl", "h", "hh", "hpp", "htm", "html", "hxx", "inc", "incl", "inf",
    "ini", "inl", "java", "js", "json", "jsx", "log", "lpr", "lua",
    "markdown", "md", "mdown", "mjs", "mkd", "nfo", "pas", "patch", "php",
    "phtml", "pp", "props", "py", "pyw", "qrc", "rc", "rst", "sh", "sql",
    "svg", "tcl", "tex", "tmx", "toml", "ts", "tsv", "tsx", "txt", "vert",
    "xhtml", "xml", "xsd", "xsl", "xslt", "yaml", "yml"
};

/* No extension at all, but text by convention. Matched against the WHOLE
   name, which is also how the leading-dot names below are caught. */
static const char *const codeOpenBare[] = {
    ".bashrc", ".editorconfig", ".gitattributes", ".gitignore",
    ".gitmodules", "authors", "changelog", "changes", "copying", "install",
    "licence", "license", "makefile", "news", "readme", "todo"
};

/* 1 if a double-click should open `name` in a tab. Pass the base name, not
   a path -- a directory called "x.exe" is the caller's problem, not ours. */
static int codeFileOpenableName(const char *name)
{
    const char *dot;

    if (!name || !*name) return 0;
    dot = strrchr(name, '.');
    /* No dot, or a dot only at the front (".gitignore" is a NAME, not an
       extension): the whole thing has to be in the bare list. */
    if (!dot || dot == name)
        return codeNameInList(codeOpenBare, CODE_NLIST(codeOpenBare), name);
    dot++;
    if (codeNameInList(codeBinExt,  CODE_NLIST(codeBinExt),  dot)) return 0;
    if (codeNameInList(codeOpenExt, CODE_NLIST(codeOpenExt), dot)) return 1;
    /* "CMakeLists.txt" has an extension AND is in the bare list; so would
       any future "Something.cfg"-shaped convention name. */
    return codeNameInList(codeOpenBare, CODE_NLIST(codeOpenBare), name);
}

#endif /* EDIT_FILEIO_H */
