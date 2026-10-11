#ifndef EDIT_FOLDERS_H
#define EDIT_FOLDERS_H

/*
 * edit_folders.h -- the "Folders" sidebar: a lazy file tree over Fl_Tree.
 *
 * The user adds folders; the tree lists everything in them, reading a
 * directory only when it is first expanded. Double-clicking opens a file the
 * editor understands in a tab, hands anything else to the shell, and toggles
 * a directory. Files the editor will not open are drawn dimmed.
 *
 * Fl_Tree 1.3 does the drawing and the keyboard well, but three of its
 * behaviours have to be worked around, and each is a trap worth naming.
 *
 * ONE: IT SWALLOWS THE RIGHT BUTTON
 *
 * Fl_Tree::handle()'s FL_PUSH does `ret |= 1` the moment the press lands on
 * an item -- before it tests for FL_LEFT_MOUSE. So a right-click is reported
 * as handled and never reaches a parent, which means a context menu cannot be
 * hung off the widget from outside. It has to be caught here, ahead of the
 * base class.
 *
 * TWO: IT HAS NO DOUBLE-CLICK
 *
 * Fl_Tree never calls Fl::event_clicks() -- there is no double-click concept
 * in it at all. Clicking an already-selected item re-runs select_only(),
 * which returns 0 and fires nothing. So that is caught here too.
 *
 * Catching both before Fl_Tree::handle() also means running before the
 * Fl_Group::handle() that normally feeds the scrollbars, so presses inside
 * them are passed through explicitly. For the same reason FL_MOUSEWHEEL is
 * forwarded by hand: Fl_Tree does not handle it, and Fl_Group only offers it
 * to the child under the pointer, so over the tree BODY the wheel does
 * nothing at all in stock 1.3.
 *
 * THREE: IT DOES NOT OWN user_data()
 *
 * Fl_Tree_Item's destructor frees its label and nothing else, and neither
 * remove() nor clear_children() touches user_data(). Every FoldItem hung off
 * an item is ours to free, which is what freeRec() is for -- call it on a
 * subtree BEFORE destroying it. And never call Fl_Tree::clear(): it deletes
 * the root and leaves the pointer dangling for the next add().
 *
 * PATHS ARE NOT TREE PATHS
 *
 * Fl_Tree's add("a/b/c") API uses '/' as a separator and wants '/' and '\'
 * inside a name backslash-escaped -- which a Windows path violates in every
 * component. So items are added with add(parent, name), and the real path
 * lives in FoldItem::path. item_pathname() is never used.
 *
 * AND THE FILENAME ENCODING, ON 9x ONLY
 *
 * fl_filename_list() returns UTF-8 on NT, but on 9x the SOOB scandir patch
 * takes the FindFirstFileA branch and hands back raw system-ANSI bytes, by
 * design -- that platform's filesystem is codepage-based. FLTK draws labels
 * as UTF-8, so an accented name would come out as mojibake. label() converts
 * for DISPLAY only: FoldItem::path keeps the raw bytes, because those are
 * what the patched fl_fopen(), GetFileAttributesA() and ShellExecuteA() all
 * want there -- and what comdlg32 already hands addDoc() today.
 */

#include <FL/Fl.H>
#include <FL/Fl_Tree.H>
#include <FL/Fl_Tree_Item.H>
#include <FL/Fl_Pixmap.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/filename.H>
#include <FL/fl_utf8.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "edit_code.h"      /* CODE_COL_* */
#include "edit_dpi.h"       /* editDpi, editDpiImage */
#include "edit_fileio.h"    /* codeFileOpenableName, codeAnsiToUtf8 */

#if defined(WIN32) || defined(_WIN32)
#  include <windows.h>
#endif

/* ---- icons ---------------------------------------------------------------
 * Fl_Tree's built-in [+] / [-] are hardcoded #fefefe on #444444 -- a white
 * chiclet on a 0x282a36 canvas. These are the same 11x11 as the originals so
 * the collapse-icon hit box and the connector maths stay bit-identical.
 */
static const char *const foldPlusXpm[] = {
    "11 11 3 1",
    ".  c #44475a",
    "#  c #6272a4",
    "@  c #f8f8f2",
    "###########",
    "#.........#",
    "#.........#",
    "#....@....#",
    "#....@....#",
    "#..@@@@@..#",
    "#....@....#",
    "#....@....#",
    "#.........#",
    "#.........#",
    "###########"
};
static const char *const foldMinusXpm[] = {
    "11 11 3 1",
    ".  c #44475a",
    "#  c #6272a4",
    "@  c #f8f8f2",
    "###########",
    "#.........#",
    "#.........#",
    "#.........#",
    "#.........#",
    "#..@@@@@..#",
    "#.........#",
    "#.........#",
    "#.........#",
    "#.........#",
    "###########"
};
/* 12x12, flat and outlined: anything with a gradient turns to mud at this
   size, and the row height is set by the font, not by these. */
static const char *const foldDirXpm[] = {
    "12 12 3 1",
    "   c None",
    ".  c #6272a4",
    "+  c #bd934f",
    "            ",
    " .....      ",
    " .+++.      ",
    " .+++.......",
    " .+++++++++.",
    " .+++++++++.",
    " .+++++++++.",
    " .+++++++++.",
    " .+++++++++.",
    " .+++++++++.",
    " ...........",
    "            "
};
static const char *const foldFileXpm[] = {
    "12 12 3 1",
    "   c None",
    ".  c #a0a4b0",
    "-  c #6272a4",
    "            ",
    "  ........  ",
    "  .      .  ",
    "  . ---- .  ",
    "  .      .  ",
    "  . ---- .  ",
    "  .      .  ",
    "  . ---- .  ",
    "  .      .  ",
    "  ........  ",
    "            ",
    "            "
};
static const char *const foldFileDimXpm[] = {
    "12 12 2 1",
    "   c None",
    ".  c #6272a4",
    "            ",
    "  ........  ",
    "  .      .  ",
    "  .      .  ",
    "  .      .  ",
    "  .      .  ",
    "  .      .  ",
    "  .      .  ",
    "  .      .  ",
    "  ........  ",
    "            ",
    "            "
};

/* Built once by codeFoldersTheme(). usericon() takes no copy, so these are
   owned for the life of the process and deliberately never freed. */
static Fl_Image *gFoldIconDir     = 0;
static Fl_Image *gFoldIconFile    = 0;
static Fl_Image *gFoldIconFileDim = 0;

/* ---- per-item data ------------------------------------------------------- */

typedef struct FoldItem {
    char *path;     /* malloc'd, absolute, native separators, no trailing one */
    int   isDir;
    int   loaded;   /* a directory whose children have been read */
    int   isRoot;   /* a folder the user added: what Remove Folder drops */
} FoldItem;

/* ---- small path helpers -------------------------------------------------- */

static const char *foldBaseName(const char *p)
{
    const char *a = strrchr(p, '/');
    const char *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : p;
}

static void foldJoin(char *out, int outlen, const char *dir, const char *name)
{
    int n = (int)strlen(dir);
    /* "C:\" and "/" already end in a separator; everything else needs one. */
    int sep = n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\');
#if defined(WIN32) || defined(_WIN32)
    const char *s = sep ? "" : "\\";
#else
    const char *s = sep ? "" : "/";
#endif
    if (outlen <= 0) return;
    out[0] = '\0';
    if (n + (int)strlen(s) + (int)strlen(name) >= outlen) return;  /* too long */
    sprintf(out, "%s%s%s", dir, s, name);
}

static int foldPathEq(const char *a, const char *b)
{
#if defined(WIN32) || defined(_WIN32)
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

/* Hidden, system, and the version-control directories nobody wants to page
   through. On Windows "hidden" is the ATTRIBUTE, not a leading dot --
   ".gitignore" is an ordinary visible file there and should be listed. */
static int foldSkip(const char *full, const char *name)
{
#if defined(WIN32) || defined(_WIN32)
    DWORD a = GetFileAttributesA(full);
    if (a != 0xFFFFFFFFUL && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
        return 1;
#else
    (void)full;
    if (name[0] == '.' && strcmp(name, ".") && strcmp(name, "..")) return 1;
#endif
    return !strcmp(name, ".git") || !strcmp(name, ".svn") || !strcmp(name, ".hg");
}

/* ---- the widget ---------------------------------------------------------- */

/* Called with the event still current, so the owner can popup() at
   Fl::event_x() / event_y(). `item` is 0 when the click missed every row. */
typedef void (*CodeFoldersFn)(Fl_Tree_Item *item, void *data);

class CodeFolders : public Fl_Tree {
public:
    CodeFolders(int X, int Y, int W, int H)
        : Fl_Tree(X, Y, W, H),
          mMenuFn(0), mActFn(0), mMenuData(0), mActData(0), mSwallow(0) {}

    ~CodeFolders() { freeRec(root()); }

    void menuCallback(CodeFoldersFn fn, void *d)     { mMenuFn = fn; mMenuData = d; }
    void activateCallback(CodeFoldersFn fn, void *d) { mActFn = fn;  mActData = d; }

    static const char *pathOf(Fl_Tree_Item *it)
    {
        FoldItem *f = it ? (FoldItem *)it->user_data() : 0;
        return f ? f->path : 0;
    }
    static int isDirOf(Fl_Tree_Item *it)
    {
        FoldItem *f = it ? (FoldItem *)it->user_data() : 0;
        return f ? f->isDir : 0;
    }

    /* _vscroll / _hscroll are protected on Fl_Tree, so this can only be done
       from a subclass -- there is no public accessor. The look itself comes
       from codeThemeScrollbar() in edit_code.h, which the editor uses too:
       these two sit side by side, so they must not drift. */
    void themeScrollbars(void)
    {
        codeThemeScrollbar(_vscroll);
        codeThemeScrollbar(_hscroll);
    }

    /* Free every FoldItem in a subtree, the item's OWN included. MUST run
       before remove() -- Fl_Tree frees the items but never their user data.
       Stub children carry a null, so they cost nothing here. */
    static void freeRec(Fl_Tree_Item *it)
    {
        FoldItem *f;
        int k;
        if (!it) return;
        for (k = 0; k < it->children(); k++) freeRec(it->child(k));
        f = (FoldItem *)it->user_data();
        if (f) { free(f->path); free(f); it->user_data(0); }
    }

    /* Children only. populate() needs this and NOT freeRec(): the item's own
       FoldItem is the one holding the path it is about to read. */
    static void freeChildren(Fl_Tree_Item *it)
    {
        int k;
        if (!it) return;
        for (k = 0; k < it->children(); k++) freeRec(it->child(k));
    }

    /* Add a folder the user picked. Re-adding one just selects it. */
    Fl_Tree_Item *addRoot(const char *path)
    {
        Fl_Tree_Item *r = root(), *it;
        FoldItem *f;
        const char *lab;
        int k;

        if (!r || !path || !*path) return 0;
        for (k = 0; k < r->children(); k++) {
            f = (FoldItem *)r->child(k)->user_data();
            if (f && foldPathEq(f->path, path)) {
                select_only(r->child(k), 0);
                return r->child(k);
            }
        }
        lab = foldBaseName(path);           /* "C:\" has no base name */
        it = addEntry(r, path, (lab && *lab) ? lab : path, 1);
        if (!it) return 0;
        f = (FoldItem *)it->user_data();
        if (f) f->isRoot = 1;
        return it;
    }

    /* The root that contains `it` -- what Remove Folder acts on, so a
       right-click deep inside a tree still has something to remove. */
    Fl_Tree_Item *rootOf(Fl_Tree_Item *it)
    {
        Fl_Tree_Item *r = root();
        while (it && it->parent() && it->parent() != r) it = it->parent();
        return (it && it->parent() == r) ? it : 0;
    }

    /* Drop a listed folder. The disk is not touched. */
    void removeRoot(Fl_Tree_Item *it)
    {
        Fl_Tree_Item *r = rootOf(it);
        if (!r) return;
        freeRec(r);
        remove(r);                 /* never clear(): it deletes root() itself */
        redraw();
    }

    /* Read a directory in, the first time it is opened. */
    void populate(Fl_Tree_Item *it)
    {
        FoldItem *f = it ? (FoldItem *)it->user_data() : 0;
        struct dirent **list = 0;
        char name[300], child[600];
        int n, pass, k, len, isDir;

        if (!f || !f->isDir || f->loaded) return;
        f->loaded = 1;             /* set FIRST: an unreadable directory must
                                      not be retried on every single open */
        freeChildren(it);          /* just the stub here -- and NOT freeRec,
                                      which would free the very FoldItem
                                      whose path we are about to read */
        clear_children(it);

        n = fl_filename_list(f->path, &list, fl_casenumericsort);
        if (n < 0) return;                 /* unreadable: nothing allocated */

        /* Folders first, then files. fl_casenumericsort has already ordered
           the list case-insensitively and numerically ("f2" before "f10"), so
           two passes over it is the whole sort -- which is why sortorder()
           stays NONE: its ASCENDING is a case-SENSITIVE strcmp with no
           folders-first. */
        for (pass = 0; pass < 2; pass++) {
            for (k = 0; k < n; k++) {
                len = (int)strlen(list[k]->d_name);
                /* The patched scandir_win32.c appends '/' to directories, so
                   "." and ".." arrive as "./" and "../". */
                isDir = len > 0 && list[k]->d_name[len - 1] == '/';
                if (isDir != (pass == 0)) continue;
                if (len >= (int)sizeof(name)) continue;
                strcpy(name, list[k]->d_name);
                if (isDir) name[len - 1] = '\0';
                if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) continue;
                foldJoin(child, (int)sizeof(child), f->path, name);
                if (!child[0] || foldSkip(child, name)) continue;
                addEntry(it, child, name, isDir);
            }
        }
        fl_filename_free_list(&list, n);   /* n == 0 still frees the array */
    }

    int handle(int e)
    {
        Fl_Tree_Item *it;

        /* Fl_Tree does not handle the wheel, and Fl_Group only offers it to
           the child under the pointer, so without this it does nothing over
           the tree body. */
        if (e == FL_MOUSEWHEEL && _vscroll && _vscroll->visible())
            return _vscroll->handle(e);

        /* A press we took owns its drag and release too, or the base class
           re-resolves them against a selection it never saw change. */
        if (mSwallow && (e == FL_DRAG || e == FL_RELEASE)) {
            if (e == FL_RELEASE) mSwallow = 0;
            return 1;
        }

        if (e == FL_PUSH) {
            mSwallow = 0;      /* a popup() may have eaten the last release */
            /* The scrollbars are real children and the base class gives them
               first refusal; running ahead of it means doing so by hand. */
            if ((_vscroll && _vscroll->visible() && Fl::event_inside(_vscroll)) ||
                (_hscroll && _hscroll->visible() && Fl::event_inside(_hscroll)))
                return Fl_Tree::handle(e);

            it = find_clicked(0);

            if (Fl::event_button() == FL_RIGHT_MOUSE) {
                if (it) select_only(it, 0);    /* 0: no callback re-entry */
                mSwallow = 1;
                if (mMenuFn) mMenuFn(it, mMenuData);
                return 1;      /* touch nothing after: the menu may have
                                  removed the very item `it` points at */
            }

            if (Fl::event_button() == FL_LEFT_MOUSE && Fl::event_clicks() > 0) {
                Fl::event_clicks(0);   /* consume, or a third click reads as
                                          another pair */
                mSwallow = 1;
                /* On the [+] the first click of the pair already toggled; a
                   second would undo it. */
                if (it && !it->event_on_collapse_icon(prefs())) {
                    if (isDirOf(it)) open_toggle(it);   /* fires OPENED */
                    else if (mActFn) mActFn(it, mActData);
                }
                return 1;
            }
        }
        return Fl_Tree::handle(e);
    }

private:
    /* One entry. add(parent, name) rather than the path form -- see the
       header comment on why a Windows path cannot be an Fl_Tree path. */
    Fl_Tree_Item *addEntry(Fl_Tree_Item *parent, const char *path,
                           const char *name, int isDir)
    {
        Fl_Tree_Item *it;
        FoldItem *f;
        char shown[400];

        displayLabel(shown, (int)sizeof(shown), name);
        it = add(parent, shown);
        if (!it) return 0;

        f = (FoldItem *)malloc(sizeof(FoldItem));
        if (!f) return it;
        f->path = (char *)malloc(strlen(path) + 1);
        if (!f->path) { free(f); return it; }
        strcpy(f->path, path);
        f->isDir  = isDir;
        f->loaded = 0;
        f->isRoot = 0;
        it->user_data(f);

        if (isDir) {
            it->usericon(gFoldIconDir);
            close(it, 0);
            /* A directory with no children draws no [+] and reports no hit on
               one, so an unexpanded folder needs a stub to stand on. It is
               gone before it can ever be drawn: populate() runs from the
               OPENED callback, before the next flush. */
            add(it, "");
        } else if (codeFileOpenableName(name)) {
            it->usericon(gFoldIconFile);
        } else {
            it->usericon(gFoldIconFileDim);
            it->labelfgcolor(CODE_COL_COMMENT);
        }
        return it;
    }

    /* Display text for a raw filesystem name -- see the encoding note in the
       header. fl_utf8test() returns 0 for bytes that are not valid UTF-8,
       which ANSI text with accents never is; pure ASCII passes either way. */
    static void displayLabel(char *out, int outlen, const char *name)
    {
        int n = (int)strlen(name);
        char *u;
        out[0] = '\0';
        if (n >= outlen) n = outlen - 1;
        memcpy(out, name, (size_t)n);
        out[n] = '\0';
        if (n > 0 && !fl_utf8test(name, (unsigned)strlen(name))) {
            u = codeAnsiToUtf8(name, (int)strlen(name));
            if (u) {
                int m = (int)strlen(u);
                if (m >= outlen) m = outlen - 1;
                memcpy(out, u, (size_t)m);
                out[m] = '\0';
                free(u);
            }
        }
    }

    CodeFoldersFn mMenuFn, mActFn;
    void *mMenuData, *mActData;
    int   mSwallow;
};

/* ---- theme ---------------------------------------------------------------
 * Call once, immediately after construction and BEFORE the first addRoot():
 * item_labelfgcolor() writes the prefs that every item copies at
 * construction, so items made earlier would keep FLTK's black.
 */
static void codeFoldersTheme(CodeFolders *t)
{
    t->box(FL_FLAT_BOX);                  /* the default FL_DOWN_BOX is a
                                             light two-pixel bevel */
    t->color(CODE_COL_BG);
    t->selection_color(CODE_COL_SEL);
    t->selectbox(FL_FLAT_BOX);
    t->connectorcolor(CODE_COL_GUTTER);
    t->connectorstyle(FL_TREE_CONNECTOR_DOTTED);

    t->item_labelfgcolor(CODE_COL_FG);
    t->item_labelbgcolor(0xffffffff);     /* the "transparent" sentinel: rows
                                             take the tree's own color() */
    t->item_labelsize(FL_NORMAL_SIZE);    /* editDpiInit() already scaled it */

    t->showroot(1);                       /* the root IS the sidebar's title */
    t->root_label("Folders");
    t->showcollapse(1);
    t->selectmode(FL_TREE_SELECT_SINGLE);
    t->sortorder(FL_TREE_SORT_NONE);      /* populate() sorts, folders first */
    t->when(FL_WHEN_CHANGED);             /* the OPENED callback needs it */

    /* Fl_Tree_Prefs' metrics are hard 96-DPI pixels and nothing scales them:
       the tree is built after editDpiScaleTree(), and that walk only moves
       geometry anyway. */
    t->marginleft(editDpi(6));
    t->margintop(editDpi(3));
    t->marginbottom(editDpi(4));          /* the 20px default is dead space */
    t->usericonmarginleft(editDpi(3));
    t->labelmarginleft(editDpi(4));
    t->widgetmarginleft(editDpi(3));
    t->connectorwidth(editDpi(14));
    t->linespacing(editDpi(2));

    /* The open icon must never be null: the layout reads openicon()->w(). */
    t->openicon(editDpiImage(new Fl_Pixmap(foldPlusXpm)));
    t->closeicon(editDpiImage(new Fl_Pixmap(foldMinusXpm)));
    gFoldIconDir     = editDpiImage(new Fl_Pixmap(foldDirXpm));
    gFoldIconFile    = editDpiImage(new Fl_Pixmap(foldFileXpm));
    gFoldIconFileDim = editDpiImage(new Fl_Pixmap(foldFileDimXpm));

    t->themeScrollbars();

    /* The root was constructed by Fl_Tree's own constructor, before any of
       the above, so it still carries FLTK's defaults. */
    if (t->root()) {
        t->root()->labelfgcolor(CODE_COL_FG);
        t->root()->labelbgcolor(0xffffffff);
        t->root()->open();
    }
}

/* Selected-row text is NOT ours: Fl_Tree_Item::drawfgcolor() forces it to
   fl_contrast(labelfgcolor, selection_color). Against CODE_COL_SEL
   (luminance 72) a normal CODE_COL_FG label clears the threshold and is kept
   as-is, and a dimmed one becomes white -- both legible, so no
   draw_item_content() override is needed. If CODE_COL_SEL is ever lightened
   past luminance 127 that flips to black and this stops being true. */

#endif /* EDIT_FOLDERS_H */
