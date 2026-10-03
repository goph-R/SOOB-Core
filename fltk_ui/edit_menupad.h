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
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#define EDIT_MENUPAD_LABEL   FL_FREE_LABELTYPE
#define EDIT_MENUCHECK_LABEL ((Fl_Labeltype)(FL_FREE_LABELTYPE + 1))
#define EDIT_MENUTITLE_LABEL ((Fl_Labeltype)(FL_FREE_LABELTYPE + 2))

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

/* ---- Office 97 menu bar -----------------------------------------------
 * Three looks, and FLTK routes all three through the menu-bar widget:
 *
 *   the band      1px white along the top, 1px dark gray along the bottom.
 *   a pressed     the open title: a 1px SUNKEN border (dark gray top/left,
 *   title         white bottom/right) on plain button face, black text.
 *   a drop-down   a 3D button frame -- FL_UP_BOX, i.e. the 2px Win95 raised
 *                 border -- with items highlighting in solid blue.
 *
 * The awkward part is that menuwindow takes its frame from the BAR:
 *
 *     box(button->box());
 *     if (box() == FL_NO_BOX || box() == FL_FLAT_BOX) box(FL_UP_BOX);
 *
 * so a custom boxtype on the bar (what the old IE5 look used) is inherited by
 * every drop-down, which is why the popups came out ruled top and bottom
 * instead of framed. The way out is in the second line: leave the bar
 * FL_FLAT_BOX and the drop-downs fall back to FL_UP_BOX on their own. The
 * band's two rules are then drawn by EditMenuBar::draw() rather than by a
 * boxtype -- the one thing a boxtype cannot be here.
 *
 * FL_FLAT_BOX pays off twice, because menuwindow also sizes the floating
 * title window from the bar's box:
 *
 *     int dy = Fl::box_dy(button->box()) + 1;
 *     int ht = button->h() - dy * 2;
 *
 * With box_dy == 0 that is a 1px inset, so the pressed title lands exactly
 * between the band's two rules. A 2px boxtype would have floated it 3px in.
 *
 * WHY THE PRESSED TITLE IS DRAWN BY A LABELTYPE
 *
 * Fl_Menu_Item::draw() fills the highlight with down_box() (or FL_FLAT_BOX)
 * in selection_color() for BOTH a selected drop-down item and a pressed
 * title -- one setting, two looks wanted, so it cannot be set. Leaving
 * down_box unset gives the drop-down items their blue fill, and the title is
 * repainted over that blue by its own labeltype. A labeltype can tell the two
 * apart where a boxtype cannot: the title is drawn into the little borderless
 * menutitle WINDOW that FLTK floats over the bar, so Fl_Window::current()
 * answers "am I the pressed title, or the bar itself?".
 *
 * That also fixes the text colour. The fill is blue, so FLTK contrasts the
 * label to white against it; on button face it has to be black, and drawing
 * the label ourselves is what lets us ignore the colour FLTK worked out.
 *
 * Nothing moves between the two states, which is worth knowing before
 * "correcting" the asymmetry in FLTK: a title cell is laid out at x+6 with
 * width measure+16 and its label at +3, while the menutitle window sits at
 * titlex() == x+3 with width measure+12 and draws its label at +3+3. Both
 * land the text at the same pixel, and the 1px border is drawn inside the
 * 3px that the narrower window leaves. An Office 97 push does not shove its
 * label down-right, so this is the behaviour we want.
 */

/* Label for the menu-bar titles. Draws the pressed border when it is being
   drawn into the floating title window, and always forces the text colour --
   see "WHY THE PRESSED TITLE IS DRAWN BY A LABELTYPE" above. */
static void editMenuTitleDraw(const Fl_Label *o, int X, int Y, int W, int H,
                              Fl_Align align)
{
    Fl_Window *win = Fl_Window::current();
    if (win && win->menu_window()) {
        /* The pressed title. The window IS the item, so its own size is the
           border rect -- no need to undo the label's insets. */
        int w = win->w(), h = win->h();
        fl_color(FL_BACKGROUND_COLOR);
        fl_rectf(0, 0, w, h);                    /* over FLTK's blue fill */
        fl_color(FL_DARK3);
        fl_xyline(0, 0, w - 1);                  /* shadow: top, left     */
        fl_yxline(0, 0, h - 1);
        fl_color(FL_WHITE);
        fl_xyline(0, h - 1, w - 1);              /* highlight: bottom, right */
        fl_yxline(w - 1, 0, h - 1);
    }
    fl_font(o->font, o->size);
    fl_color(FL_FOREGROUND_COLOR);
    fl_draw(o->value, X, Y, W, H, align, o->image);
}

/* Plain text metrics, as FL_NORMAL_LABEL would give: the titles must measure
   the same either way or titlex() and the drawn cells would disagree. */
static void editMenuTitleMeasure(const Fl_Label *o, int &W, int &H)
{
    fl_font(o->font, o->size);
    fl_measure(o->value, W, H);
}

/* The band. Ruled after Fl_Menu_Bar::draw() so the rules win over a title
   cell or an FL_MENU_DIVIDER reaching into the edge rows; the labels are
   vertically centred, so nothing legible is covered. */
class EditMenuBar : public Fl_Menu_Bar {
public:
    EditMenuBar(int X, int Y, int W, int H, const char *L = 0)
        : Fl_Menu_Bar(X, Y, W, H, L) { box(FL_FLAT_BOX); }

    void draw()
    {
        Fl_Menu_Bar::draw();
        fl_color(FL_WHITE); fl_xyline(x(), y(), x() + w() - 1);
        fl_color(FL_DARK3); fl_xyline(x(), y() + h() - 1, x() + w() - 1);
    }
};

/* Apply the Office 97 look to a menu bar, and `fontPx` to both the bar titles
   and the drop-down items (pass an editDpi()-scaled value). Call after
   menu() / editMenuPad(). The bar wants to be an EditMenuBar for the band;
   everything else here works on a plain Fl_Menu_Bar too. */
static void editMenuBarStyle(Fl_Menu_ *menu, int fontPx)
{
    Fl::set_labeltype(EDIT_MENUTITLE_LABEL, editMenuTitleDraw,
                      editMenuTitleMeasure);
    menu->box(FL_FLAT_BOX);          /* => FL_UP_BOX drop-downs; see above */
    /* Top level only: these are the bar titles. editMenuPad() has already
       taken the items inside the drop-downs. */
    Fl_Menu_Item *m = (Fl_Menu_Item *)menu->menu();
    for (; m && m->text; m = m->next())
        m->labeltype(EDIT_MENUTITLE_LABEL);
    if (fontPx > 0) menu->textsize(fontPx);
}

#endif /* EDIT_MENUPAD_H */
