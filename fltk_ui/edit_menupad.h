#ifndef EDIT_MENUPAD_H
#define EDIT_MENUPAD_H

/*
 * edit_menupad.h -- left "icon column" padding for drop-down menu items.
 *
 * FLTK 1.3 draws a plain menu item's text flush against the menu's left edge;
 * only toggle/radio items get a column (for their check box). Native Windows
 * menus reserve that column on every item, which reads much calmer.
 *
 * editMenuPad() switches every item INSIDE the drop-downs to a custom label
 * type that leaves exactly the toggle items' check-box column empty, so plain
 * and toggle items line up. Menu-bar titles are left alone. Toggle/radio items
 * keep the normal label type -- FLTK already offsets them.
 *
 * Checked items: editMenuCheck() swaps an item to a second label type that
 * also draws a native-style check mark (no box) in that column. Use it on
 * plain items instead of FL_MENU_TOGGLE / FL_MENU_RADIO, whose box/dot look
 * nothing like a Windows menu; the caller owns the on/off state.
 *
 * Call after the menu is fully built (menu() / add()): items added later keep
 * the default label type.
 */

#include <FL/Fl.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#define EDIT_MENUPAD_LABEL   FL_FREE_LABELTYPE
#define EDIT_MENUCHECK_LABEL ((Fl_Labeltype)(FL_FREE_LABELTYPE + 1))

/* Width of the toggle column, mirroring Fl_Menu_Item::draw(): a W-wide box
   at x+2, text at x+W+3, where d = (h - size + 1) / 2, W = h - 2d.

   `size` is the LABEL's size, not FL_NORMAL_SIZE: a menu with its own
   textsize() (editMenuBarStyle() sets 11px) would otherwise have its check
   column computed from the global 12 and drift away from its own text. */
static int editMenuPadWidth(int h, int size)
{
    int d = (h - size + 1) / 2;
    return (h - 2 * d) + 3;
}

static void editMenuPadDraw(const Fl_Label *o, int X, int Y, int W, int H,
                            Fl_Align align)
{
    int pad = editMenuPadWidth(H, o->size);
    fl_font(o->font, o->size);
    fl_color((Fl_Color)o->color);
    fl_draw(o->value, X + pad, Y, W > pad ? W - pad : 0, H, align, o->image);
}

/* Same as editMenuPadDraw, plus a check mark in the column. Item-relative,
   the label is drawn at x+3 and FLTK's toggle box at x+2, so the box would
   sit at X-1; the tick is laid out inside that square. */
static void editMenuCheckDraw(const Fl_Label *o, int X, int Y, int W, int H,
                              Fl_Align align)
{
    int d  = (H - o->size + 1) / 2;
    int s  = H - 2 * d;                 /* square side */
    int bx = X - 1, by = Y + d;
    int t  = s / 7;                     /* stroke width, grows with DPI */
    if (t < 1) t = 1;

    fl_color((Fl_Color)o->color);
    fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, t);
    fl_line(bx + s * 2 / 10, by + s * 5 / 10,
            bx + s * 4 / 10, by + s * 7 / 10,
            bx + s * 8 / 10, by + s * 3 / 10);
    fl_line_style(0);

    editMenuPadDraw(o, X, Y, W, H, align);
}

static void editMenuPadMeasure(const Fl_Label *o, int &W, int &H)
{
    fl_font(o->font, o->size);
    fl_measure(o->value, W, H);
    /* Menus measure with H = text height; the item is drawn LEADING taller,
       which only moves the column by a pixel at most -- close enough here. */
    W += editMenuPadWidth(H, o->size);
}

/* Items from `m` to the end of this (sub)menu level, recursing into submenus. */
static void editMenuPadLevel(Fl_Menu_Item *m)
{
    for (; m && m->text; m = m->next()) {
        if (!(m->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)))
            m->labeltype(EDIT_MENUPAD_LABEL);
        if (m->flags & FL_SUBMENU) editMenuPadLevel(m + 1);
    }
}

/* Show or hide an item's check mark (item must be padded, not a toggle). */
static void editMenuCheck(Fl_Menu_Item *m, int on)
{
    m->labeltype(on ? EDIT_MENUCHECK_LABEL : EDIT_MENUPAD_LABEL);
}

static void editMenuPad(Fl_Menu_ *menu)
{
    Fl::set_labeltype(EDIT_MENUPAD_LABEL, editMenuPadDraw, editMenuPadMeasure);
    Fl::set_labeltype(EDIT_MENUCHECK_LABEL, editMenuCheckDraw, editMenuPadMeasure);
    /* Top level = menu-bar titles: unpadded. Their drop-downs: padded. */
    Fl_Menu_Item *m = (Fl_Menu_Item *)menu->menu();
    for (; m && m->text; m = m->next())
        if (m->flags & FL_SUBMENU) editMenuPadLevel(m + 1);
}

/* ---- menu bar frame ----------------------------------------------------
 * FLTK's default menu bar is FL_UP_BOX: a 3D button-style frame on all four
 * sides, which reads as a row of buttons rather than a menu. IE5 (and the
 * Win9x toolbars generally) instead rule the band off with a 1px groove --
 * dark gray then white -- along the top and bottom edges only, with nothing
 * at the left and right.
 *
 * Fl_Menu_Bar::draw() lays its items out across the full y()..y()+h(), not
 * inside the box's edges, so the 2px grooves are not subtracted from the
 * label area: the text stays vertically centred in the whole band and simply
 * needs the band to be ~4px taller than the text. The dy/dh registered below
 * are therefore only for anyone who asks the boxtype about its insets.
 */
#define EDIT_MENUBAR_BOX ((Fl_Boxtype)FL_FREE_BOXTYPE)

static void editMenuBarBoxDraw(int X, int Y, int W, int H, Fl_Color c)
{
    fl_color(c);
    fl_rectf(X, Y, W, H);
    fl_color(FL_DARK3); fl_xyline(X, Y,         X + W - 1);
    fl_color(FL_WHITE); fl_xyline(X, Y + 1,     X + W - 1);
    fl_color(FL_DARK3); fl_xyline(X, Y + H - 2, X + W - 1);
    fl_color(FL_WHITE); fl_xyline(X, Y + H - 1, X + W - 1);
}

/* Apply the IE5 look to a menu bar: the groove frame above, and `fontPx` for
   both the bar titles and the drop-down items (pass an editDpi()-scaled
   value). Call after menu() / editMenuPad(). */
static void editMenuBarStyle(Fl_Menu_ *menu, int fontPx)
{
    Fl::set_boxtype(EDIT_MENUBAR_BOX, editMenuBarBoxDraw, 0, 2, 0, 4);
    menu->box(EDIT_MENUBAR_BOX);
    if (fontPx > 0) menu->textsize(fontPx);
}

#endif /* EDIT_MENUPAD_H */
