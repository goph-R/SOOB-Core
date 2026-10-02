#ifndef EDIT_MATCH_H
#define EDIT_MATCH_H

/*
 * edit_match.h -- find the pair to highlight for the caret: matching
 * brackets ( ) [ ] { }, and in HTML / PHP the matching tag name of an
 * element (<div> ... </div>).
 *
 * Works on the text buffer plus the parallel style buffer edit_code.h keeps
 * ('A' + LEX_* slot per byte), so it can tell code from comments and
 * strings without re-lexing: brackets inside comments / strings are ignored,
 * and an HTML tag name is a run of LEX_TYPE bytes right after '<' or '</'.
 * No widget, so the model test can check it headless.
 *
 * Scans are capped at CODE_MATCH_LIMIT bytes each way, so a stray bracket in
 * a large file cannot stall a slow machine.
 */

#include <FL/Fl_Text_Buffer.H>
#include "edit_lex.h"

#define CODE_MATCH_LIMIT 262144

static char codeMatchCh(Fl_Text_Buffer *b, int p)
{
    return (p >= 0 && p < b->length()) ? b->byte_at(p) : 0;
}
static int codeMatchSlot(Fl_Text_Buffer *sty, int p)
{
    return (p >= 0 && p < sty->length()) ? sty->byte_at(p) - 'A' : -1;
}
/* Comment / string / directive: brackets there do not count. */
static int codeMatchQuiet(Fl_Text_Buffer *sty, int p)
{
    int s = codeMatchSlot(sty, p);
    return s == LEX_COMMENT || s == LEX_STRING || s == LEX_DIRECTIVE;
}

/* ---- brackets ---------------------------------------------------------- */

static int codeBracketPartner(char c, int *dir)
{
    switch (c) {
    case '(': *dir =  1; return ')';
    case '[': *dir =  1; return ']';
    case '{': *dir =  1; return '}';
    case ')': *dir = -1; return '(';
    case ']': *dir = -1; return '[';
    case '}': *dir = -1; return '{';
    }
    return 0;
}

/* Bracket at p (in code): position of its partner, or -1 if unmatched. */
static int codeMatchBracket(Fl_Text_Buffer *b, Fl_Text_Buffer *sty, int p)
{
    int dir, depth = 0, i, n = b->length(), steps = 0;
    char c = codeMatchCh(b, p);
    char want = (char)codeBracketPartner(c, &dir);
    for (i = p + dir; i >= 0 && i < n && steps < CODE_MATCH_LIMIT; i += dir, steps++) {
        char d = codeMatchCh(b, i);
        if (d != c && d != want) continue;
        if (codeMatchQuiet(sty, i)) continue;
        if (d == c) depth++;
        else if (depth == 0) return i;
        else depth--;
    }
    return -1;
}

/* ---- HTML tags --------------------------------------------------------- */

static int codeTagNameCh(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == ':';
}
/* Tag-name byte: LEX_TYPE style and a name character (the lexer gives the
 * blank after a name the name's style, so the character check matters). */
static int codeTagNameAt(Fl_Text_Buffer *b, Fl_Text_Buffer *sty, int p)
{
    return codeMatchSlot(sty, p) == LEX_TYPE && codeTagNameCh(codeMatchCh(b, p));
}
static int codeTagNameEq(Fl_Text_Buffer *b, int p, int q, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        char x = codeMatchCh(b, p + i), y = codeMatchCh(b, q + i);
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;                              /* lengths are compared by the caller */
}

/* Elements that never have a closing tag. */
static int codeTagIsVoid(Fl_Text_Buffer *b, int p, int len)
{
    static const char *const v[] = {
        "area","base","br","col","embed","hr","img","input","link","meta",
        "param","source","track","wbr"
    };
    char name[8];
    int i;
    if (len >= (int)sizeof(name)) return 0;
    for (i = 0; i < len; i++) {
        char c = codeMatchCh(b, p + i);
        name[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    name[len] = '\0';
    for (i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++)
        if (strcmp(name, v[i]) == 0) return 1;
    return 0;
}

/* Does the tag whose name ends at e close itself ("/>")? Looks for the
 * tag's '>' outside attribute strings. */
static int codeTagSelfCloses(Fl_Text_Buffer *b, Fl_Text_Buffer *sty, int e)
{
    int i, n = b->length(), steps = 0;
    for (i = e; i < n && steps < 4096; i++, steps++) {
        char c = codeMatchCh(b, i);
        if (codeMatchSlot(sty, i) == LEX_STRING) continue;
        if (c == '>') return codeMatchCh(b, i - 1) == '/';
        if (c == '<') return 0;                          /* malformed */
    }
    return 0;
}

/* If p is a tag name start (preceded by '<' or '</'), its length and
 * whether it is a closing tag; 0 otherwise. */
static int codeTagNameStart(Fl_Text_Buffer *b, Fl_Text_Buffer *sty, int p, int *closing)
{
    int e = p;
    if (!codeTagNameAt(b, sty, p) || codeTagNameAt(b, sty, p - 1)) return 0;
    if (codeMatchCh(b, p - 1) == '<') *closing = 0;
    else if (codeMatchCh(b, p - 1) == '/' && codeMatchCh(b, p - 2) == '<') *closing = 1;
    else return 0;
    while (codeTagNameAt(b, sty, e)) e++;
    return e - p;
}

/* Tag name under / next to the caret at pos: its start, length, closing. */
static int codeTagAtCaret(Fl_Text_Buffer *b, Fl_Text_Buffer *sty, int pos,
                          int *start, int *closing)
{
    int q = pos;
    if (codeMatchCh(b, q) == '<') q++;                         /* on '<'   */
    if (codeMatchCh(b, q) == '/' && codeMatchCh(b, q - 1) == '<') q++;  /* on '/' of '</' */
    if (!codeTagNameAt(b, sty, q) && codeTagNameAt(b, sty, q - 1)) q--; /* just after it */
    if (!codeTagNameAt(b, sty, q)) return 0;
    while (codeTagNameAt(b, sty, q - 1)) q--;
    *start = q;
    return codeTagNameStart(b, sty, q, closing);
}

/* Matching tag name for the one at s (length len): its start, or -1. */
static int codeMatchTag(Fl_Text_Buffer *b, Fl_Text_Buffer *sty, int s, int len, int closing)
{
    int n = b->length(), depth = 0, steps = 0, i;
    if (!closing) {
        for (i = s + len; i < n && steps < CODE_MATCH_LIMIT; i++, steps++) {
            int cl, l;
            if (codeMatchCh(b, i) != '<') continue;
            l = codeTagNameStart(b, sty, i + 1, &cl);
            if (!l) l = codeTagNameStart(b, sty, i + 2, &cl);
            if (l != len) continue;
            if (!codeTagNameEq(b, s, cl ? i + 2 : i + 1, len)) continue;
            if (!cl) {
                if (!codeTagSelfCloses(b, sty, i + 1 + len)) depth++;
            } else if (depth == 0) {
                return i + 2;
            } else {
                depth--;
            }
        }
    } else {
        for (i = s - 3; i >= 0 && steps < CODE_MATCH_LIMIT; i--, steps++) {   /* before its own "</" */
            int cl, l;
            if (codeMatchCh(b, i) != '<') continue;
            l = codeTagNameStart(b, sty, i + 1, &cl);
            if (!l) l = codeTagNameStart(b, sty, i + 2, &cl);
            if (l != len) continue;
            if (!codeTagNameEq(b, s, cl ? i + 2 : i + 1, len)) continue;
            if (cl) {
                depth++;
            } else if (!codeTagSelfCloses(b, sty, i + 1 + len)) {
                if (depth == 0) return i + 1;
                depth--;
            }
        }
    }
    return -1;
}

/* ---- entry point ------------------------------------------------------- */

/* What to highlight for the caret at pos. Fills start[k] / len[k] for up to
 * two ranges and returns how many. *bad = 1 means an unmatched bracket (one
 * range, shown as an error). Brackets win over tags; a bracket just before
 * the caret wins over one under it. */
static int codeFindPair(Fl_Text_Buffer *b, Fl_Text_Buffer *sty, int lang, int pos,
                        int *start, int *len, int *bad)
{
    int k, closing, s, l;
    *bad = 0;
    for (k = 0; k < 2; k++) {
        int p = k == 0 ? pos - 1 : pos, dir, m;
        char c = codeMatchCh(b, p);
        if (!codeBracketPartner(c, &dir) || codeMatchQuiet(sty, p)) continue;
        m = codeMatchBracket(b, sty, p);
        start[0] = p; len[0] = 1;
        if (m < 0) { *bad = 1; return 1; }
        start[1] = m; len[1] = 1;
        return 2;
    }
    if (lang != LEX_LANG_HTML && lang != LEX_LANG_PHP) return 0;
    l = codeTagAtCaret(b, sty, pos, &s, &closing);
    if (!l || (!closing && codeTagIsVoid(b, s, l))) return 0;
    if (!closing && codeTagSelfCloses(b, sty, s + l)) return 0;
    {
        int m = codeMatchTag(b, sty, s, l, closing);
        if (m < 0) return 0;                    /* unclosed <p> / <li> is legal HTML */
        start[0] = s; len[0] = l;
        start[1] = m; len[1] = l;
        return 2;
    }
}

#endif /* EDIT_MATCH_H */
