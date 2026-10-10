#ifndef EDIT_PLACE_H
#define EDIT_PLACE_H

/*
 * edit_place.h -- open dialogs centred over the app window, on screen.
 *
 *   editPlaceOwner = mainWindow;          once, after creating it
 *   fl_message_place(editPlaceDialog);    fl_alert / fl_choice / fl_input too
 *   ...
 *   editPlaceDialog(dlg); dlg->show();    each dialog of your own
 *
 * Centres the dialog's whole FRAME (title bar included) over the owner's
 * client area, then pulls it back inside the work area of the screen that
 * point is on -- taskbar excluded -- so a main window half off-screen, or
 * on a second monitor, cannot push a dialog where it cannot be reached. If
 * it does not fit at all, the top-left corner wins: that is where the title
 * bar is, the only handle to drag it by.
 *
 * Call before show(), or on a hidden window: position() on a shown one moves
 * it after it has already been painted somewhere else. fl_message_place() is
 * a SOOB patch to FLTK's fl_ask.cxx -- see SOOB-Core/docs/editor-fltk-win98.md.
 *
 * The frame is not known until a window exists, so it is estimated the way
 * Fl_Window::hotspot() does: system metrics on Windows, KDE-ish defaults
 * elsewhere. A window with a resizable() child gets the sizing frame.
 */
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#ifdef _WIN32
#include <windows.h>
#endif

static Fl_Window *editPlaceOwner = 0;

static void editPlaceDialog(Fl_Window *dlg)
{
    int sx, sy, sw, sh, cx, cy, x, y;
    int left = 0, top = 0, right = 0, bottom = 0;
    Fl_Window *o = editPlaceOwner;

    if (!dlg) return;
    if (o && o->shown()) {
        cx = o->x() + o->w() / 2;
        cy = o->y() + o->h() / 2;
    } else {
        Fl::get_mouse(cx, cy);
    }
    Fl::screen_work_area(sx, sy, sw, sh, cx, cy);

    if (dlg->border()) {
#ifdef _WIN32
        int sizing = dlg->resizable() != 0;
        left = right = GetSystemMetrics(sizing ? SM_CXSIZEFRAME : SM_CXFIXEDFRAME);
        top = bottom = GetSystemMetrics(sizing ? SM_CYSIZEFRAME : SM_CYFIXEDFRAME);
        top += GetSystemMetrics(SM_CYCAPTION);
#else
        top = 20; left = right = 4; bottom = 8;
#endif
    }

    /* the frame's centre on (cx, cy), not just the client area's */
    x = cx - (left + dlg->w() + right) / 2 + left;
    y = cy - (top + dlg->h() + bottom) / 2 + top;

    if (x + dlg->w() + right > sx + sw) x = sx + sw - right - dlg->w();
    if (y + dlg->h() + bottom > sy + sh) y = sy + sh - bottom - dlg->h();
    if (x - left < sx) x = sx + left;
    if (y - top < sy) y = sy + top;
    dlg->position(x, y);
}

#endif /* EDIT_PLACE_H */
