#ifndef EDIT_GREP_H
#define EDIT_GREP_H

/*
 * edit_grep.h -- find (and replace) a string across a folder tree.
 *
 * The model half of Find in Folder: it walks a directory, reads every file
 * the editor would open, and records each match as path + line number + the
 * text of that line. No widgets, so it is unit-testable headless, and the
 * dialog in codeedit.cpp is only a list box over the results.
 *
 * ONE ENTRY PER MATCH, NOT PER LINE
 *
 * A line with three hits produces three entries. That makes the count in
 * "47 matches in 9 files" mean what it says, and it is what the replace
 * actually does -- a per-line list would under-report and then surprise
 * you.
 *
 * THE CAP IS NOT OPTIONAL
 *
 * Searching for "e" across a source tree on a 192 MB Pentium II will find
 * hundreds of thousands of matches, and every one of them is two mallocs
 * and a row in a browser widget. codeGrepRun() stops at maxHits and sets
 * `truncated`, which the caller must say out loud -- a silently short list
 * would read as "that is all of them" and a Replace All driven from it
 * would be wrong.
 *
 * READING IS codeReadText(), WHICH MATTERS
 *
 * It gives back UTF-8 with LF endings whatever the file really is, and
 * reports the encoding and line ending it found. So the search sees one
 * consistent form, and a replace can write the file back in its ORIGINAL
 * encoding and line ending rather than quietly converting every file it
 * touches to UTF-8 LF.
 *
 * WHAT IS SKIPPED, AND WHY IT IS THE SAME RULE AS THE TREE
 *
 * Hidden and system files, the version-control directories, and anything
 * codeFileOpenableName() refuses -- the same two tests the Folders tree
 * uses to decide what to list and what to dim. If the tree will not open
 * it, searching it would only produce hits you cannot act on.
 */

#include <FL/filename.H>
#include <stdlib.h>
#include <string.h>

#include "edit_fileio.h"    /* codeReadText, codeWriteText, openable, hidden */
#include "edit_find.h"      /* codeNCaseEq */

/* A file open in the editor may differ from the file on disk. Set this and
 * the search asks for the open text first, so it finds what you can SEE --
 * both the edits you have not saved yet and, just as importantly, not the
 * matches you have already edited away. Return malloc'd text (the search
 * frees it) or 0 to fall back to reading the file. */
static char *(*codeGrepOpenText)(const char *path) = 0;

#define CODE_GREP_MAXDEPTH  16     /* a symlink loop must not be fatal */
#define CODE_GREP_LINEMAX  200     /* of a matching line, for display */

typedef struct CodeGrepHit {
    char *path;        /* malloc'd, absolute */
    char *line;        /* malloc'd, the matching line, trimmed for display */
    int   lineNo;      /* 1-based */
} CodeGrepHit;

typedef struct CodeGrep {
    CodeGrepHit *hit;
    int n, cap;
    int files;         /* files with at least one match */
    int scanned;       /* files actually read */
    int truncated;     /* stopped at maxHits: the list is NOT all of them */
} CodeGrep;

static void codeGrepInit(CodeGrep *g)
{
    g->hit = 0;
    g->n = g->cap = 0;
    g->files = g->scanned = g->truncated = 0;
}

static void codeGrepFree(CodeGrep *g)
{
    int i;
    for (i = 0; i < g->n; i++) { free(g->hit[i].path); free(g->hit[i].line); }
    free(g->hit);
    codeGrepInit(g);
}

/* Offset of the next match at or after `from`, or -1. */
static int codeGrepFind(const char *text, int len, const char *needle,
                        int nlen, int from, int matchCase)
{
    int i;
    if (nlen <= 0) return -1;
    for (i = from; i + nlen <= len; i++) {
        if (matchCase) { if (memcmp(text + i, needle, (size_t)nlen) == 0) return i; }
        else           { if (codeNCaseEq(text + i, needle, nlen))        return i; }
    }
    return -1;
}

/* How many times `needle` occurs. Non-overlapping, which is what replacing
   does too, so the count and the replacement can never disagree. */
static int codeGrepCount(const char *text, const char *needle, int matchCase)
{
    int len = (int)strlen(text), nlen = (int)strlen(needle), at = 0, n = 0;
    while ((at = codeGrepFind(text, len, needle, nlen, at, matchCase)) >= 0) {
        n++;
        at += nlen;
    }
    return n;
}

/* Every occurrence replaced. Returns malloc'd text and sets *count, or 0 if
   there was nothing to do or no memory. Scans forward past each replacement,
   so a replacement containing the needle cannot loop. */
static char *codeGrepReplace(const char *text, const char *needle,
                             const char *repl, int matchCase, int *count)
{
    int len = (int)strlen(text), nlen = (int)strlen(needle);
    int rlen = (int)strlen(repl), n, at, from, outlen;
    char *out, *o;

    if (count) *count = 0;
    n = codeGrepCount(text, needle, matchCase);
    if (n <= 0) return 0;

    outlen = len + n * (rlen - nlen);
    out = (char *)malloc((size_t)outlen + 1);
    if (!out) return 0;

    o = out;
    from = 0;
    while ((at = codeGrepFind(text, len, needle, nlen, from, matchCase)) >= 0) {
        memcpy(o, text + from, (size_t)(at - from)); o += at - from;
        memcpy(o, repl, (size_t)rlen);               o += rlen;
        from = at + nlen;
    }
    memcpy(o, text + from, (size_t)(len - from));
    o += len - from;
    *o = '\0';
    if (count) *count = n;
    return out;
}

/* The line `at` falls on, trimmed of leading blanks and clipped, for the
   results list. Also yields the 1-based line number. */
static char *codeGrepLineAt(const char *text, int at, int *lineNo)
{
    int a, b, i, n;
    char *s;

    *lineNo = 1;
    for (i = 0; i < at; i++) if (text[i] == '\n') (*lineNo)++;
    a = at;
    while (a > 0 && text[a - 1] != '\n') a--;
    while (text[a] == ' ' || text[a] == '\t') a++;   /* indentation is noise */
    b = at;
    while (text[b] && text[b] != '\n') b++;
    if (b < a) b = a;
    n = b - a;
    if (n > CODE_GREP_LINEMAX) n = CODE_GREP_LINEMAX;
    s = (char *)malloc((size_t)n + 1);
    if (!s) return 0;
    memcpy(s, text + a, (size_t)n);
    s[n] = '\0';
    return s;
}

static int codeGrepPush(CodeGrep *g, const char *path, const char *line,
                        int lineNo, int maxHits)
{
    CodeGrepHit *grown;
    char *p, *l;

    if (g->n >= maxHits) { g->truncated = 1; return 0; }
    if (g->n == g->cap) {
        int cap = g->cap ? g->cap * 2 : 64;
        grown = (CodeGrepHit *)realloc(g->hit, (size_t)cap * sizeof(*grown));
        if (!grown) { g->truncated = 1; return 0; }
        g->hit = grown;
        g->cap = cap;
    }
    p = (char *)malloc(strlen(path) + 1);
    l = (char *)malloc(strlen(line) + 1);
    if (!p || !l) { free(p); free(l); g->truncated = 1; return 0; }
    strcpy(p, path);
    strcpy(l, line);
    g->hit[g->n].path   = p;
    g->hit[g->n].line   = l;
    g->hit[g->n].lineNo = lineNo;
    g->n++;
    return 1;
}

/* One file. Returns 0 if the cap stopped it. */
static int codeGrepFile(CodeGrep *g, const char *path, const char *needle,
                        int matchCase, int maxHits)
{
    int enc, crlf, len, nlen, at, lineNo, hits = 0, ok = 1;
    char *text, *line;

    text = codeGrepOpenText ? codeGrepOpenText(path) : 0;
    /* No trunc out-param: a file that stops at a NUL is still searchable up
       to there, and this only ever READS. The on-disk replace does ask. */
    if (!text) text = codeReadText(path, &enc, &crlf, 0);
    if (!text) return 1;                       /* unreadable: not an error */
    g->scanned++;
    len  = (int)strlen(text);
    nlen = (int)strlen(needle);
    at   = 0;
    while ((at = codeGrepFind(text, len, needle, nlen, at, matchCase)) >= 0) {
        line = codeGrepLineAt(text, at, &lineNo);
        if (line) {
            ok = codeGrepPush(g, path, line, lineNo, maxHits);
            free(line);
        }
        hits++;
        if (!ok) break;
        at += nlen;
    }
    if (hits) g->files++;
    free(text);
    return ok;
}

static int codeGrepWalk(CodeGrep *g, const char *dir, const char *needle,
                        int matchCase, int recurse, int maxHits, int depth)
{
    struct dirent **list = 0;
    char name[300], child[600];
    int n, k, len, isDir, ok = 1;

    if (depth > CODE_GREP_MAXDEPTH) return 1;
    n = fl_filename_list(dir, &list, fl_casenumericsort);
    if (n < 0) return 1;

    for (k = 0; k < n && ok; k++) {
        len = (int)strlen(list[k]->d_name);
        isDir = len > 0 && list[k]->d_name[len - 1] == '/';
        if (len >= (int)sizeof(name)) continue;
        strcpy(name, list[k]->d_name);
        if (isDir) name[len - 1] = '\0';
        if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        codeFileJoin(child, (int)sizeof(child), dir, name);
        if (!child[0] || codeFileHidden(child, name)) continue;
        if (isDir) {
            if (recurse)
                ok = codeGrepWalk(g, child, needle, matchCase, recurse,
                                  maxHits, depth + 1);
        } else if (codeFileOpenableName(name)) {
            ok = codeGrepFile(g, child, needle, matchCase, maxHits);
        }
    }
    fl_filename_free_list(&list, n);
    return ok;
}

/* Search `dir` for `needle`. The caller owns `g` and must codeGrepFree it. */
static void codeGrepRun(CodeGrep *g, const char *dir, const char *needle,
                        int matchCase, int recurse, int maxHits)
{
    codeGrepFree(g);                   /* also leaves it initialised */
    if (!dir || !*dir || !needle || !*needle) return;
    codeGrepWalk(g, dir, needle, matchCase, recurse, maxHits, 0);
}

#endif /* EDIT_GREP_H */
