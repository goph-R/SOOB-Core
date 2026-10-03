#ifndef EDIT_INDENT_H
#define EDIT_INDENT_H

/*
 * edit_indent.h -- the indentation model: WHAT the editor should do about
 * indentation, as buffer-only functions with no widget.
 *
 * Same split as edit_find.h and edit_match.h: everything here takes an
 * Fl_Text_Buffer plus the parallel style buffer edit_code.h maintains ('A' +
 * LEX_* per byte, so comments and strings can be ignored without re-lexing),
 * and returns a POSITION or a STRING. CodeEditor in edit_code.h does the
 * actual editing. That is what lets edit_code_model_test.cpp check every rule
 * headless, instead of by typing on the target machine.
 *
 * WHAT IS LANGUAGE-SPECIFIC
 *
 * Brackets ( [ { work in every language. Block WORDS live in one table
 * (codeBlockWordTable), split four ways:
 *
 *   open    begin case record        depth +1, and the next line is indented
 *   close   end until                depth -1, and THIS line is outdented to
 *                                    its opener
 *   mid     except finally           depth 0, but both of the above: outdented
 *                                    to the opener, next line indented
 *   hint    then do of var           depth 0, next line indented, never a
 *                                    re-indent trigger
 *
 * Only open/close move the nesting depth, which is what makes a scan back
 * from `end` walk past an `except` and land on the `try`.
 *
 * `hint` exists for Pascal and is the whole reason the fourth category is
 * needed. `then` and `do` govern ONE statement there and are closed by
 * nothing, so counting them as openers would make a later `end` align to the
 * nearest dangling `then` instead of its `begin`. Lua's `then` / `do` ARE
 * closed by `end`, so they stay openers and Lua's hint list is empty.
 *
 * `fold` matches the words case-insensitively (Pascal: BEGIN == begin), in
 * which case every table entry must be lowercase.
 *
 * Scans back from the caret are capped at CODE_INDENT_LIMIT bytes, the same
 * guard edit_match.h uses: one unbalanced keyword in a large file must not
 * stall a Pentium II.
 */

#include <FL/Fl_Text_Buffer.H>
#include <string.h>
#include <stdlib.h>
#include "edit_lex.h"

#define CODE_INDENT_LIMIT 262144

/* ---- small shared helpers ---------------------------------------------- */

static char codeIndCh(Fl_Text_Buffer *b, int p)
{
    return (p >= 0 && p < b->length()) ? b->byte_at(p) : 0;
}

/* Comment / string / directive: brackets and keywords there do not count. */
static int codeIndQuiet(Fl_Text_Buffer *sty, int p)
{
    int s = (p >= 0 && p < sty->length()) ? sty->byte_at(p) - 'A' : -1;
    return s == LEX_COMMENT || s == LEX_STRING || s == LEX_DIRECTIVE;
}

/* First non-blank at or after line start `ls`. */
static int codeIndentEnd(Fl_Text_Buffer *b, int ls)
{
    while (codeIndCh(b, ls) == ' ' || codeIndCh(b, ls) == '\t') ls++;
    return ls;
}

static int codeIndWordCh(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int codeIndIsBlank(char c) { return c == ' ' || c == '\t'; }

/* ---- smart Home -------------------------------------------------------- */

/* Where Home should put the caret: the first non-blank character, or column 0
 * if it is already there. A blank or all-whitespace line has no first
 * non-blank, so it goes straight to column 0 -- i.e. plain Home.
 *
 * Deliberately the LOGICAL line, matching FLTK's own kf_move(FL_Home), which
 * is buffer()->line_start(). So this changes where Home lands, never which
 * kind of line it means, wrap mode included. */
static int codeHomeTarget(Fl_Text_Buffer *b, int pos)
{
    int ls, le, we;
    if (!b) return pos;
    if (pos < 0) pos = 0;
    if (pos > b->length()) pos = b->length();
    ls = b->line_start(pos);
    le = b->line_end(pos);
    we = codeIndentEnd(b, ls);
    if (we >= le) return ls;              /* nothing but blanks on the line */
    return (pos == we) ? ls : we;
}

/* ---- block words ------------------------------------------------------- */

typedef struct CodeBlockWords {
    int lang;
    const char *const *open;   int nOpen;
    const char *const *close;  int nClose;
    const char *const *mid;    int nMid;
    const char *const *hint;   int nHint;
    char fold;                 /* case-insensitive; entries must be lowercase */
} CodeBlockWords;

/* `function` and `do` and `then` and `repeat` open; `end` and `until` close.
 * `else` / `elseif` are mid words -- see the header comment. */
static const char *const codeLuaOpenWords[]  = { "function", "then", "do", "repeat" };
static const char *const codeLuaCloseWords[] = { "end", "until" };
static const char *const codeLuaMidWords[]   = { "else", "elseif" };

/* Pascal. Only genuinely paired words are openers: begin/case/record/try end
 * with `end`, repeat with `until`. `class` and `object` are left out on
 * purpose -- a forward declaration `TFoo = class;` has no `end`, so counting
 * it would strand the next `end`.
 *
 * `else` is a hint, NOT a mid word, which is the one place Pascal differs
 * from Lua. Aligning `else` with its `if` would need `if` tracked as an
 * opener, and `if` is closed by nothing. Leaving it out is better than
 * guessing: after an `end` that has just been aligned to its `begin`, plain
 * auto-indent already puts `else` on the right column, which is the common
 * begin/end form. */
static const char *const codePasOpenWords[]  = { "begin", "case", "record",
                                                 "repeat", "try" };
static const char *const codePasCloseWords[] = { "end", "until" };
static const char *const codePasMidWords[]   = { "except", "finally" };
static const char *const codePasHintWords[]  = { "const", "do", "else", "of",
                                                 "then", "type", "var" };

#define CODE_NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const CodeBlockWords codeBlockWordTable[] = {
    { LEX_LANG_LUA,
      codeLuaOpenWords,  CODE_NELEM(codeLuaOpenWords),
      codeLuaCloseWords, CODE_NELEM(codeLuaCloseWords),
      codeLuaMidWords,   CODE_NELEM(codeLuaMidWords),
      0, 0, 0 },
    { LEX_LANG_PASCAL,
      codePasOpenWords,  CODE_NELEM(codePasOpenWords),
      codePasCloseWords, CODE_NELEM(codePasCloseWords),
      codePasMidWords,   CODE_NELEM(codePasMidWords),
      codePasHintWords,  CODE_NELEM(codePasHintWords), 1 }
};

/* The block-word rules for `lang`, or 0 for a language that has none (every
 * brace language: its blocks are punctuation, handled by the bracket path). */
static const CodeBlockWords *codeBlockWords(int lang)
{
    int i;
    for (i = 0; i < CODE_NELEM(codeBlockWordTable); i++)
        if (codeBlockWordTable[i].lang == lang) return &codeBlockWordTable[i];
    return 0;
}

/* Does the buffer range [s,e) spell exactly `w`? */
static int codeIndWordIs(Fl_Text_Buffer *b, int s, int e, const char *w, int fold)
{
    int i = 0;
    while (s + i < e) {
        int c = (unsigned char)b->byte_at(s + i);
        if (fold) c = lexLower(c);
        if (!w[i] || c != (unsigned char)w[i]) return 0;
        i++;
    }
    return w[i] == '\0';
}

static int codeIndWordIn(Fl_Text_Buffer *b, int s, int e,
                         const char *const *list, int n, int fold)
{
    int i;
    for (i = 0; i < n; i++) if (codeIndWordIs(b, s, e, list[i], fold)) return 1;
    return 0;
}

/* Net block depth change contributed by the words in [from,to).
 *
 * `from` must be a word boundary (a line start, in practice) so that the
 * first word is not a tail of a longer identifier -- that is what keeps
 * `append` from reading as `end`. */
static int codeBlockWordBalance(Fl_Text_Buffer *b, Fl_Text_Buffer *sty,
                                int lang, int from, int to)
{
    const CodeBlockWords *w = codeBlockWords(lang);
    int p = from, bal = 0;
    if (!w) return 0;
    while (p < to) {
        int s;
        if (!codeIndWordCh(codeIndCh(b, p))) { p++; continue; }
        s = p;
        while (p < to && codeIndWordCh(codeIndCh(b, p))) p++;
        if (codeIndQuiet(sty, s)) continue;
        if      (codeIndWordIn(b, s, p, w->close, w->nClose, w->fold)) bal--;
        else if (codeIndWordIn(b, s, p, w->open,  w->nOpen,  w->fold)) bal++;
    }
    return bal;
}

/* ---- Enter: does the line being left open a block? --------------------- */

/* 1 if the line after `pos` should get one extra indent level.
 *
 * Two independent reasons, either is enough, and the answer is one level
 * rather than a count -- `foo(bar, {` indents once, not twice:
 *
 *   - the last thing before the caret is an opening bracket;
 *   - the line's block words open more than they close, or its last word is
 *     a mid word (a bare `else` has balance 0 but is still followed by an
 *     indented body).
 *
 * `if x then return end` opens and closes on one line, so it balances to 0
 * and the next line is NOT indented -- which is the whole point of counting
 * rather than looking only at the last word. */
static int codeOpensBlock(Fl_Text_Buffer *b, Fl_Text_Buffer *sty,
                          int lang, int pos)
{
    const CodeBlockWords *w;
    int ls, p, s;
    char c;

    if (!b || !sty) return 0;
    ls = b->line_start(pos);
    p  = pos;
    while (p > ls && codeIndIsBlank(codeIndCh(b, p - 1))) p--;

    if (p > ls) {
        c = codeIndCh(b, p - 1);
        if ((c == '{' || c == '[' || c == '(') && !codeIndQuiet(sty, p - 1))
            return 1;
    }
    if (codeBlockWordBalance(b, sty, lang, ls, p) > 0) return 1;

    /* A trailing mid or hint word: `else` on its own line in Lua, or Pascal's
     * `if x then` / `for i := 1 to n do` / a bare `var`. Both indent what
     * follows; only the last word on the line counts, which is what keeps
     * `procedure Foo(var x: Integer);` from indenting. */
    w = codeBlockWords(lang);
    if (w && p > ls && codeIndWordCh(codeIndCh(b, p - 1))) {
        s = p;
        while (s > ls && codeIndWordCh(codeIndCh(b, s - 1))) s--;
        if (!codeIndQuiet(sty, s) &&
            (codeIndWordIn(b, s, p, w->mid,  w->nMid,  w->fold) ||
             codeIndWordIn(b, s, p, w->hint, w->nHint, w->fold)))
            return 1;
    }
    return 0;
}

/* ---- closing a block: where should this line line up? ------------------ */

static char codeIndOpenPartner(char close)
{
    switch (close) {
    case '}': return '{';
    case ']': return '[';
    case ')': return '(';
    }
    return 0;
}

/* `close` is about to be typed at `pos`, whose line is blank so far: the
 * line start of the line holding the matching opener, or -1.
 *
 * The blank-line requirement is what makes this safe for ( and ) -- a `)`
 * closing a half-finished expression is never alone on its line, so this
 * does not fire on it. */
static int codeCloseBracketOpener(Fl_Text_Buffer *b, Fl_Text_Buffer *sty,
                                  int pos, char close)
{
    char open = codeIndOpenPartner(close);
    int ls, le, p, depth = 0, steps = 0;

    if (!b || !sty || !open) return -1;
    ls = b->line_start(pos);
    le = b->line_end(pos);
    for (p = ls; p < le; p++)
        if (!codeIndIsBlank(codeIndCh(b, p))) return -1;

    for (p = ls - 1; p >= 0 && steps < CODE_INDENT_LIMIT; p--, steps++) {
        char c = codeIndCh(b, p);
        if ((c != open && c != close) || codeIndQuiet(sty, p)) continue;
        if (c == close) depth++;
        else if (depth == 0) return b->line_start(p);
        else depth--;
    }
    return -1;
}

/* A closing or mid block word is being completed on an otherwise-blank line:
 * the line start of the line holding its opener, or -1.
 *
 * `typed` is the character about to be inserted, so this is answered BEFORE
 * the edit and the caller can re-indent and insert inside one undo group --
 * the same shape the bracket path has always used. Pass 0 if the word in the
 * buffer is already complete.
 *
 * The backwards walk counts open/close words only, so it steps over an
 * `else` on the way from an `end` to its `if`. */
static int codeCloseWordOpener(Fl_Text_Buffer *b, Fl_Text_Buffer *sty,
                               int lang, int pos, char typed)
{
    const CodeBlockWords *w = codeBlockWords(lang);
    int ls, le, ws, p, depth = 0, steps = 0, hit;

    if (!b || !sty || !w) return -1;
    ls = b->line_start(pos);
    le = b->line_end(pos);
    if (pos != le) return -1;                 /* caret must be at the end */
    ws = codeIndentEnd(b, ls);

    /* [ws,le) plus `typed` must spell a closing or mid word, with nothing
     * but blanks in front of it. */
    for (p = ws; p < le; p++)
        if (!codeIndWordCh(codeIndCh(b, p))) return -1;
    {
        char cand[16];
        int n = le - ws;
        if (n < 0 || n >= (int)sizeof(cand) - 1) return -1;
        for (p = 0; p < n; p++) cand[p] = codeIndCh(b, ws + p);
        if (typed) cand[n++] = typed;
        cand[n] = '\0';
        if (w->fold) for (p = 0; cand[p]; p++) cand[p] = (char)lexLower((unsigned char)cand[p]);
        hit = 0;
        for (p = 0; p < w->nClose && !hit; p++) if (!strcmp(cand, w->close[p])) hit = 1;
        for (p = 0; p < w->nMid   && !hit; p++) if (!strcmp(cand, w->mid[p]))   hit = 1;
        if (!hit) return -1;
    }

    for (p = ls - 1; p >= 0 && steps < CODE_INDENT_LIMIT; steps++) {
        int s, e;
        while (p >= 0 && !codeIndWordCh(codeIndCh(b, p))) { p--; steps++; }
        if (p < 0) break;
        e = p + 1;
        s = e;
        while (s > 0 && codeIndWordCh(codeIndCh(b, s - 1))) s--;
        if (!codeIndQuiet(sty, s)) {
            if (codeIndWordIn(b, s, e, w->close, w->nClose, w->fold)) depth++;
            else if (codeIndWordIn(b, s, e, w->open, w->nOpen, w->fold)) {
                if (depth == 0) return b->line_start(s);
                depth--;
            }
        }
        p = s - 1;
    }
    return -1;
}

/* ---- paste ------------------------------------------------------------- */

/* Re-indent a multi-line clipboard block for a caret that sits in the
 * leading whitespace of its line, where `indent` is the text between the
 * line start and the caret.
 *
 * The block's own COMMON indent is removed and `indent` put in its place, so
 * code copied out of a deeply nested file lands at the depth it is pasted
 * into while keeping its INTERNAL shape. Line 1 is stripped but not
 * prefixed: `indent` is already in the buffer ahead of the caret. Blank
 * lines stay empty rather than being padded, so a paste introduces no
 * trailing whitespace.
 *
 * Returns a malloc'd string for the caller to free(), or 0 when `src` should
 * go in untouched -- a single line, or nothing that would change. The caller
 * is responsible for only calling this when the caret is in the indent: a
 * mid-line paste is a continuation and must stay verbatim.
 *
 * The clipboard is LF-only by the time it gets here; FLTK folds CRLF on the
 * way in (Fl_win32.cxx). A stray CR is therefore ordinary content.
 */
static char *codeReindentPaste(const char *src, const char *indent)
{
    const char *p, *nl;
    char *out, *o;
    int ilen, common = -1, lines = 0, total, first;

    if (!src || !*src || !strchr(src, '\n')) return 0;
    if (!indent) indent = "";
    ilen = (int)strlen(indent);

    /* Pass 1: the shallowest leading blank run among the non-blank lines.
     * Entirely blank lines say nothing about the block's depth, so they must
     * not drag it to zero. */
    for (p = src;; p = nl + 1) {
        int len, ws = 0;
        nl = strchr(p, '\n');
        len = nl ? (int)(nl - p) : (int)strlen(p);
        while (ws < len && codeIndIsBlank(p[ws])) ws++;
        if (ws < len && (common < 0 || ws < common)) common = ws;
        if (!nl) break;
        lines++;
    }
    if (common < 0) common = 0;
    if (common == 0 && ilen == 0) return 0;          /* would be a no-op */

    total = (int)strlen(src) + lines * (ilen + 1) + 1;
    out = (char *)malloc((size_t)total);
    if (!out) return 0;
    o = out;

    first = 1;
    for (p = src;; p = nl + 1) {
        int len, ws = 0, cut;
        nl = strchr(p, '\n');
        len = nl ? (int)(nl - p) : (int)strlen(p);
        while (ws < len && codeIndIsBlank(p[ws])) ws++;
        if (ws < len) {
            cut = ws < common ? ws : common;
            if (!first) { memcpy(o, indent, (size_t)ilen); o += ilen; }
            memcpy(o, p + cut, (size_t)(len - cut));
            o += len - cut;
        }
        first = 0;
        if (!nl) break;
        *o++ = '\n';
    }
    *o = '\0';
    return out;
}

#endif /* EDIT_INDENT_H */
