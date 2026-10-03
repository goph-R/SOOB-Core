#ifndef EDIT_MENUPAD_H
#define EDIT_MENUPAD_H

/*
 * edit_menupad.h -- Office 97 / Word 97 menus for an Fl_Menu_Bar.
 *
 * Drop-down items: FLTK 1.3 draws a plain item's text flush against the
 * menu's left edge and reserves the icon column only for toggle/radio items.
 * Word reserves it on every item, rules its separators as an etched line
 * inset from both edges, and marks a checked item with the same square it
 * uses for a pressed toolbar button. editMenuPad() puts all three on a menu,
 * measured off Word 97 itself:
 *
 *   row          19px for an 11px font -- the text height plus three.
 *   icon column  a square the height of the row against the left edge, the
 *                text five pixels past it (25px in from the edge at 19px).
 *   separator    128-gray over white, inset 3px from each content edge.
 *   check        a 1px SUNKEN square filled with a white / face
 *                checkerboard, with the Win95 tick centred on it.
 *
 * ONE LABELTYPE, FOUR STATES
 *
 * An item can be checked, ruled (a separator above it), both or neither, so
 * the four are FL_FREE_LABELTYPE + a two-bit code rather than four draw
 * functions. Fl_Label carries the labeltype in `type`, so one draw function
 * reads its own bits back out. editMenuCheck() flips the check bit and
 * leaves the rule bit alone.
 *
 * Call after the menu is fully built (menu() / add()): items added later
 * keep the default label type. Toggle/radio items are left to FLTK -- use
 * editMenuCheck() on a plain item instead, whose box and dot look nothing
 * like a Windows menu; the caller owns the on/off state.
 *
 * WHY THE RULE BELONGS TO THE ITEM UNDER IT
 *
 * FLTK draws FL_MENU_DIVIDER itself, in menuwindow::drawentry(), after the
 * label and in colours of its own -- so a labeltype cannot restyle it: the
 * label has already been drawn by then. Overdrawing it from the NEXT item
 * does not work either, because a hover redraws just two entries, and a lone
 * redraw of the item above would put FLTK's line back with nothing to cover
 * it. So editMenuPad() clears FL_MENU_DIVIDER and hands the rule to the item
 * below, which draws it in the leading above its own cell.
 *
 * Those two rows are the only ones safe to draw in. drawentry() erases an
 * entry under
 *
 *     fl_push_clip(xx+1, yy-(LEADING-2)/2, ww-2, hh+(LEADING-2));
 *
 * which for the item above reaches exactly to our top row minus one, and for
 * the item itself starts exactly one row below our bottom one. A rule drawn
 * any higher would be erased by the neighbour above on every hover.
 *
 * The one thing here that is not Word's is the air around a separator, and
 * it cannot be: Word gives one a row of its own, 10px, where FLTK has only
 * the 4px LEADING between two rows -- every entry in a menu is one height.
 * Measured text-to-rule, Word is 7px above and 10 below; this is 5 and 4.
 * The alternative, a dummy item, buys a full 19px row and lands at 12 and
 * 12 -- no closer, and 9px taller per separator. Left as is.
 */

#include <FL/Fl.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

/* The item labeltypes: a base plus a two-bit state. Keep them contiguous --
   editMenuCheck() and editMenuPad() do arithmetic on them. */
#define EDIT_MENU_CHECKED    1
#define EDIT_MENU_RULED      2
#define EDIT_MENUPAD_LABEL      FL_FREE_LABELTYPE                      /* +0 */
#define EDIT_MENUCHECK_LABEL    ((Fl_Labeltype)(FL_FREE_LABELTYPE + 1))
#define EDIT_MENUSEP_LABEL      ((Fl_Labeltype)(FL_FREE_LABELTYPE + 2))
#define EDIT_MENUSEPCHECK_LABEL ((Fl_Labeltype)(FL_FREE_LABELTYPE + 3))
#define EDIT_MENUTITLE_LABEL    ((Fl_Labeltype)(FL_FREE_LABELTYPE + 4))

/* ButtonShadow in the Win9x default scheme, which is the palette
   FL_BACKGROUND_COLOR's 192-gray already assumes. FL_DARK3 is 85 -- too dark
   for an etched line next to white. */
#define EDIT_MENU_SHADOW fl_rgb_color(128, 128, 128)

/* Added to the MEASURED text height to reach Word's row. FLTK builds the row
   out of the tallest label plus LEADING (4), and every item in these menus
   shares one font, so the height handed back to editMenuItemDraw() is this
   same number -- measure and draw agree without having to guess. */
static int editMenuRowPad(int size)
{
    return (size + 2) / 4;              /* 3 at the 11px menu font */
}

/* Text offset inside an item: the icon square (the row height) plus Word's
   five-pixel gap, less the 1px the square is inset by. */
static int editMenuPadWidth(int h)
{
    return h + 7;
}

/* Word's checked-toolbar-button square: 1px sunken, a white / face
   checkerboard inside it, the Win95 tick centred on top. The checkerboard is
   a point per pixel -- about 110 of them at 19px, which is nothing next to
   the rest of a menu redraw even on a PII, and there is no stipple in FLTK
   that would survive both platforms. */
static void editMenuCheckBox(int x, int y, int side, int size)
{
    /* The tick as Word draws it: seven 2px column runs, down three and up
       four. A stroked line cannot reproduce it at this size. */
    static const int tickDY[7] = { 2, 3, 4, 3, 2, 1, 0 };
    int s = size / 11;                  /* 1 at 96 DPI, 2 at 192, ... */
    int gx, gy, i, px, py;
    if (s < 1) s = 1;

    fl_color(FL_BACKGROUND_COLOR);
    fl_rectf(x + 1, y + 1, side - 2, side - 2);
    fl_color(FL_WHITE);
    for (py = 1; py < side - 1; py++)
        for (px = 1; px < side - 1; px++)
            if ((px + py) & 1) fl_point(x + px, y + py);

    fl_color(EDIT_MENU_SHADOW);
    fl_xyline(x, y, x + side - 2);                  /* shadow: top, left   */
    fl_yxline(x, y, y + side - 2);
    fl_color(FL_WHITE);
    fl_xyline(x, y + side - 1, x + side - 1);       /* highlight: bottom,  */
    fl_yxline(x + side - 1, y, y + side - 1);       /* right               */

    gx = x + (side - 7 * s) / 2;
    gy = y + (side - 6 * s + 1) / 2;
    fl_color(FL_FOREGROUND_COLOR);
    for (i = 0; i < 7; i++)
        fl_rectf(gx + i * s, gy + tickDY[i] * s, s, 2 * s);
}

/* One draw for all four item states; which one is in the labeltype itself.
   Leaves fl_color() on the label colour, because drawentry() draws the
   submenu arrow and the shortcut text with whatever this left set. */
static void editMenuItemDraw(const Fl_Label *o, int X, int Y, int W, int H,
                             Fl_Align align)
{
    int bits = (int)o->type - (int)FL_FREE_LABELTYPE;
    int pad  = editMenuPadWidth(H);

    if (bits & EDIT_MENU_RULED) {
        /* X is the content edge plus 3 (FLTK's own item inset), which is
           exactly Word's left inset; the right end mirrors it off the
           window, whose width is the only honest source for it here. */
        Fl_Window *win = Fl_Window::current();
        int right = win ? win->w() - 6 : X + W;
        fl_color(EDIT_MENU_SHADOW); fl_xyline(X, Y - 3, right);
        fl_color(FL_WHITE);         fl_xyline(X, Y - 2, right);
    }
    /* Square the height of the HIGHLIGHT, one pixel in from the edge.
       Word's fills the whole 19px row, but ours cannot: the bottom two rows
       of a cell are where the next item draws its rule, so a full-height
       square under a separator would be struck through. The highlight is
       what reads as the row anyway. */
    if (bits & EDIT_MENU_CHECKED)
        editMenuCheckBox(X - 2, Y - 1, H + 2, o->size);

    fl_font(o->font, o->size);
    fl_color((Fl_Color)o->color);
    fl_draw(o->value, X + pad, Y, W > pad ? W - pad : 0, H, align, o->image);
}

static void editMenuItemMeasure(const Fl_Label *o, int &W, int &H)
{
    fl_font(o->font, o->size);
    fl_measure(o->value, W, H);
    H += editMenuRowPad(o->size);
    W += editMenuPadWidth(H);
}

/* Show or hide an item's check mark (item must be one of ours, not a
   toggle). Preserves the rule bit. */
static void editMenuCheck(Fl_Menu_Item *m, int on)
{
    int bits = (int)m->labeltype() - (int)FL_FREE_LABELTYPE;
    if (bits < 0 || bits > 3) return;
    bits = on ? (bits | EDIT_MENU_CHECKED) : (bits & ~EDIT_MENU_CHECKED);
    m->labeltype((Fl_Labeltype)(FL_FREE_LABELTYPE + bits));
}

/* Items from `m` to the end of this (sub)menu level, recursing into
   submenus. FL_MENU_DIVIDER is consumed here and re-expressed as the rule
   bit on the FOLLOWING item -- see the header comment. */
static void editMenuPadLevel(Fl_Menu_Item *m)
{
    int carry = 0;
    for (; m && m->text; m = m->next()) {
        int bits = carry;
        carry = 0;
        if (m->flags & FL_MENU_DIVIDER) {
            m->flags &= ~FL_MENU_DIVIDER;
            carry = EDIT_MENU_RULED;
        }
        if (m->flags & FL_SUBMENU) editMenuPadLevel(m + 1);
        /* A toggle/radio keeps FLTK's own label type, so it cannot carry a
           rule. Nothing here uses them; editMenuCheck() is the way. */
        if (!(m->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)))
            m->labeltype((Fl_Labeltype)(FL_FREE_LABELTYPE + bits));
    }
}

static void editMenuPad(Fl_Menu_ *menu)
{
    Fl_Menu_Item *m;
    int t;
    for (t = 0; t <= EDIT_MENU_CHECKED + EDIT_MENU_RULED; t++)
        Fl::set_labeltype((Fl_Labeltype)(FL_FREE_LABELTYPE + t),
                          editMenuItemDraw, editMenuItemMeasure);
    /* Top level = menu-bar titles: left to editMenuBarStyle(). Their
       drop-downs: ours. */
    m = (Fl_Menu_Item *)menu->menu();
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
           border rect -- no need to undo the label's insets.

           Held off the band's two rules by a row top and bottom: the title
           window is inset 1px into the bar, which would otherwise put this
           border immediately against them with no face colour between. The
           margin rows are filled, not skipped, because FLTK has already
           flooded the window with selection_color(). Nothing is inset
           horizontally -- the band is ruled along the top and bottom only,
           so there is nothing to clear at the sides. */
        int w = win->w(), h = win->h();
        fl_color(FL_BACKGROUND_COLOR);
        fl_rectf(0, 0, w, h);                    /* over FLTK's blue fill */
        fl_color(FL_DARK3);
        fl_xyline(0, 1, w - 1);                  /* shadow: top, left     */
        fl_yxline(0, 1, h - 2);
        fl_color(FL_WHITE);
        fl_xyline(0, h - 2, w - 1);              /* highlight: bottom, right */
        fl_yxline(w - 1, 1, h - 2);
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
