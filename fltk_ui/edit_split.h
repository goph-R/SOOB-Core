#ifndef EDIT_SPLIT_H
#define EDIT_SPLIT_H

/*
 * edit_split.h -- a draggable bar between two widgets.
 *
 * Fl_Tile is FLTK's answer and it is the wrong one here. It needs the two
 * panes wrapped in a group of their own, which means an extra level in a
 * window laid out as flat children -- and that group would then be walked by
 * editDpiScaleTree(), the one thing edit_dpi.h says must not happen to a
 * group that lays itself out. Beyond that: there is no bar to grab, only the
 * 2-3 px child border, so the resize cursor appears almost nowhere; Fl_Tile
 * shares space proportionally, where a sidebar wants a fixed pixel width as
 * the window grows; and it cannot hide a pane, which a "show the sidebar"
 * toggle needs.
 *
 * So: a plain widget that draws a bar, shows the resize cursor over it, and
 * reports where the user has dragged it. It moves nothing itself.
 *
 * WHY IT REPORTS INSTEAD OF RESIZING
 *
 * Only the owner knows what else is on that row and what the minimum widths
 * are, so the callback hands over a RAW left-hand width and the owner clamps
 * it and lays out. That also keeps this widget free of any assumption about
 * what it sits between.
 *
 * FL_ENTER MUST RETURN 1, or FLTK never sends the matching FL_LEAVE and the
 * resize cursor stays on after the pointer has gone.
 */

#include <FL/Fl.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

/* Called with the new width for whatever is to the LEFT of the bar, on every
   drag and once more on release. Unclamped: the owner decides. */
typedef void (*CodeSplitFn)(int leftW, void *data);

class CodeSplitter : public Fl_Widget {
public:
    CodeSplitter(int X, int Y, int W, int H)
        : Fl_Widget(X, Y, W, H), mFn(0), mData(0), mGrab(0), mHot(0) {}

    void splitCallback(CodeSplitFn fn, void *data) { mFn = fn; mData = data; }

    int handle(int e)
    {
        switch (e) {
        case FL_ENTER:
            mHot = 1;
            redraw();
            if (window()) window()->cursor(FL_CURSOR_WE);
            return 1;                      /* or no FL_LEAVE ever arrives */
        case FL_LEAVE:
            mHot = 0;
            redraw();
            if (window()) window()->cursor(FL_CURSOR_DEFAULT);
            return 1;
        case FL_PUSH:
            if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
            /* Where in the bar it was grabbed, so it does not jump under the
               pointer on the first drag event. */
            mGrab = Fl::event_x() - x();
            return 1;
        case FL_DRAG:
        case FL_RELEASE:
            if (mFn) mFn(Fl::event_x() - mGrab, mData);
            return 1;
        }
        return Fl_Widget::handle(e);
    }

protected:
    void draw()
    {
        fl_color(mHot ? selection_color() : color());
        fl_rectf(x(), y(), w(), h());
    }

private:
    CodeSplitFn mFn;
    void       *mData;
    int         mGrab;      /* pointer offset inside the bar while dragging */
    int         mHot;       /* pointer is over it */
};

#endif /* EDIT_SPLIT_H */
