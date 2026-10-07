#ifndef EDIT_JSON_H
#define EDIT_JSON_H

/*
 * edit_json.h -- JSON pretty-printer for the code editor's Tools menu.
 *
 * Pure text in, text out: no FLTK, so it is tested in edit_code_test.cpp.
 *
 *   char *out = codeJsonFormat(src, len, 4, 0, &errAt, &errMsg);
 *   if (!out) ... errMsg says what is wrong, errAt is the byte offset
 *   free(out);
 *
 * Validates while it formats -- a recursive-descent parse of RFC 8259 --
 * because a formatter that guesses on broken input reshuffles the text around
 * the mistake and makes it harder to find. On an error nothing is produced
 * and the caller can put the caret on errAt.
 *
 * Layout: one member / element per line, "key": value with one space after
 * the colon, empty {} and [] kept on one line. Strings, numbers and literals
 * are copied byte for byte (escapes untouched), so formatting never changes
 * what the document means. Each level indents by `indent` spaces, or by one
 * tab character when `useTabs` is set -- the editor's own Tab setting.
 *
 * Nesting is capped at CODE_JSON_MAX_DEPTH: the parser recurses, and a Win98
 * thread's stack is no place to find out how deep a hostile file goes.
 */
#include <stdlib.h>
#include <string.h>

#ifndef CODE_JSON_MAX_DEPTH
#define CODE_JSON_MAX_DEPTH 256
#endif

typedef struct CodeJson {
    const char *s;        /* source */
    int  n, p;            /* its length, read position */
    char *out;            /* growing result */
    int  len, cap;
    int  indent, useTabs;
    const char *err;      /* first error wins */
    int  errAt;
} CodeJson;

static int codeJsonFail(CodeJson *j, const char *msg)
{
    if (!j->err) { j->err = msg; j->errAt = j->p; }
    return 0;
}

static int codeJsonPut(CodeJson *j, const char *t, int n)
{
    if (j->len + n + 1 > j->cap) {
        int cap = j->cap * 2;
        char *q;
        if (cap < j->len + n + 1) cap = j->len + n + 1;
        q = (char *)realloc(j->out, (size_t)cap);
        if (!q) return codeJsonFail(j, "out of memory");
        j->out = q;
        j->cap = cap;
    }
    memcpy(j->out + j->len, t, (size_t)n);
    j->len += n;
    j->out[j->len] = '\0';
    return 1;
}

/* Newline, then `depth` levels of indent. */
static int codeJsonNewline(CodeJson *j, int depth)
{
    int k, w;
    if (!codeJsonPut(j, "\n", 1)) return 0;
    w = j->useTabs ? depth : depth * j->indent;
    for (k = 0; k < w; k++)
        if (!codeJsonPut(j, j->useTabs ? "\t" : " ", 1)) return 0;
    return 1;
}

static void codeJsonSkipWs(CodeJson *j)
{
    while (j->p < j->n) {
        char c = j->s[j->p];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        j->p++;
    }
}

static int codeJsonString(CodeJson *j)
{
    int start = j->p++;                       /* the opening quote */
    while (j->p < j->n) {
        unsigned char c = (unsigned char)j->s[j->p];
        if (c == '"') {
            j->p++;
            return codeJsonPut(j, j->s + start, j->p - start);
        }
        if (c < 0x20) break;                  /* raw newline etc.: unterminated */
        if (c == '\\') {
            j->p++;
            if (j->p >= j->n) break;
            if (!strchr("\"\\/bfnrtu", j->s[j->p]))
                return codeJsonFail(j, "invalid escape in string");
        }
        j->p++;
    }
    j->p = start;
    return codeJsonFail(j, "unterminated string");
}

static int codeJsonIsDigit(CodeJson *j)
{
    return j->p < j->n && j->s[j->p] >= '0' && j->s[j->p] <= '9';
}

static int codeJsonNumber(CodeJson *j)
{
    int start = j->p;
    if (j->s[j->p] == '-') j->p++;
    if (!codeJsonIsDigit(j)) return codeJsonFail(j, "invalid number");
    if (j->s[j->p] == '0') j->p++;
    else while (codeJsonIsDigit(j)) j->p++;
    if (j->p < j->n && j->s[j->p] == '.') {
        j->p++;
        if (!codeJsonIsDigit(j)) return codeJsonFail(j, "invalid number");
        while (codeJsonIsDigit(j)) j->p++;
    }
    if (j->p < j->n && (j->s[j->p] == 'e' || j->s[j->p] == 'E')) {
        j->p++;
        if (j->p < j->n && (j->s[j->p] == '+' || j->s[j->p] == '-')) j->p++;
        if (!codeJsonIsDigit(j)) return codeJsonFail(j, "invalid number");
        while (codeJsonIsDigit(j)) j->p++;
    }
    return codeJsonPut(j, j->s + start, j->p - start);
}

static int codeJsonWord(CodeJson *j, const char *w)
{
    int n = (int)strlen(w);
    if (j->n - j->p < n || memcmp(j->s + j->p, w, (size_t)n) != 0)
        return codeJsonFail(j, "unexpected character");
    j->p += n;
    return codeJsonPut(j, w, n);
}

static int codeJsonValue(CodeJson *j, int depth);

/* An object or an array; the two differ only in the "key": prefix. */
static int codeJsonContainer(CodeJson *j, int depth)
{
    char open = j->s[j->p];
    char close = open == '{' ? '}' : ']';

    if (depth >= CODE_JSON_MAX_DEPTH) return codeJsonFail(j, "nested too deeply");
    j->p++;
    codeJsonSkipWs(j);
    if (j->p < j->n && j->s[j->p] == close) {         /* {} and [] stay put */
        j->p++;
        return codeJsonPut(j, open == '{' ? "{}" : "[]", 2);
    }
    if (!codeJsonPut(j, &open, 1)) return 0;
    for (;;) {
        if (!codeJsonNewline(j, depth + 1)) return 0;
        codeJsonSkipWs(j);
        if (open == '{') {
            if (j->p >= j->n || j->s[j->p] != '"')
                return codeJsonFail(j, "expected a \"key\"");
            if (!codeJsonString(j)) return 0;
            codeJsonSkipWs(j);
            if (j->p >= j->n || j->s[j->p] != ':')
                return codeJsonFail(j, "expected ':'");
            j->p++;
            if (!codeJsonPut(j, ": ", 2)) return 0;
        }
        if (!codeJsonValue(j, depth + 1)) return 0;
        codeJsonSkipWs(j);
        if (j->p < j->n && j->s[j->p] == ',') {
            j->p++;
            if (!codeJsonPut(j, ",", 1)) return 0;
            continue;
        }
        if (j->p < j->n && j->s[j->p] == close) {
            j->p++;
            return codeJsonNewline(j, depth) && codeJsonPut(j, &close, 1);
        }
        return codeJsonFail(j, open == '{' ? "expected ',' or '}'"
                                           : "expected ',' or ']'");
    }
}

static int codeJsonValue(CodeJson *j, int depth)
{
    char c;
    codeJsonSkipWs(j);
    if (j->p >= j->n) return codeJsonFail(j, "unexpected end of text");
    c = j->s[j->p];
    if (c == '{' || c == '[') return codeJsonContainer(j, depth);
    if (c == '"') return codeJsonString(j);
    if (c == '-' || (c >= '0' && c <= '9')) return codeJsonNumber(j);
    if (c == 't') return codeJsonWord(j, "true");
    if (c == 'f') return codeJsonWord(j, "false");
    if (c == 'n') return codeJsonWord(j, "null");
    return codeJsonFail(j, "unexpected character");
}

/* Format `len` bytes of `src`. Returns the malloc()ed result, ending in a
 * newline only if src did; or 0 with *errAt (byte offset into src) and
 * *errMsg set. Either out-pointer may be 0. */
static char *codeJsonFormat(const char *src, int len, int indent, int useTabs,
                            int *errAt, const char **errMsg)
{
    CodeJson j;
    memset(&j, 0, sizeof(j));
    j.s = src;
    j.n = len;
    j.indent = indent > 0 ? indent : 1;
    j.useTabs = useTabs;
    j.cap = len + len / 2 + 64;
    j.out = (char *)malloc((size_t)j.cap);
    if (!j.out) { codeJsonFail(&j, "out of memory"); goto fail; }
    j.out[0] = '\0';

    if (!codeJsonValue(&j, 0)) goto fail;
    codeJsonSkipWs(&j);
    if (j.p < j.n) { codeJsonFail(&j, "unexpected text after the JSON value"); goto fail; }
    if (len > 0 && src[len - 1] == '\n' && !codeJsonPut(&j, "\n", 1)) goto fail;
    return j.out;

fail:
    free(j.out);
    if (errAt)  *errAt  = j.errAt;
    if (errMsg) *errMsg = j.err;
    return 0;
}

#endif /* EDIT_JSON_H */
