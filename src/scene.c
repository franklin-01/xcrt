// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

#include "xcrt.h"

#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <stdlib.h>
#include <string.h>

static int find_index(xcrt *x, Window id)
{
    for (int i = 0; i < x->nwins; i++)
        if (x->wins[i].id == id)
            return i;
    return -1;
}

static void insert_at(xcrt *x, int pos, const win *w)
{
    if (x->nwins == x->capwins) {
        int cap = x->capwins ? x->capwins * 2 : 64;
        win *n = realloc(x->wins, sizeof(*n) * cap);
        if (!n)
            return;
        x->wins = n;
        x->capwins = cap;
    }
    if (pos < 0 || pos > x->nwins)
        pos = x->nwins;
    memmove(&x->wins[pos + 1], &x->wins[pos], sizeof(win) * (x->nwins - pos));
    x->wins[pos] = *w;
    x->nwins++;
}

/* Detaches the entry at i without releasing its resources. */
static win take_at(xcrt *x, int i)
{
    win w = x->wins[i];
    memmove(&x->wins[i], &x->wins[i + 1], sizeof(win) * (x->nwins - i - 1));
    x->nwins--;
    return w;
}

static void free_win(xcrt *x, win *w)
{
    render_release_window(x, w);
    if (w->rects)
        XFree(w->rects);
    w->rects = NULL;
}

static void remove_at(xcrt *x, int i)
{
    bool visible = x->wins[i].mapped;
    win w = take_at(x, i);
    free_win(x, &w);
    if (visible)
        x->redraw = true;
}

/* Moves wins[i] directly above `above` (None means the bottom). */
static void restack(xcrt *x, int i, Window above)
{
    win w = take_at(x, i);
    int pos = 0;
    if (above != None) {
        int a = find_index(x, above);
        pos = a >= 0 ? a + 1 : x->nwins;
    }
    insert_at(x, pos, &w);
}

static void add_window(xcrt *x, Window id)
{
    if (id == x->overlay || id == x->glwin || id == x->cm_owner || find_index(x, id) >= 0)
        return;

    XWindowAttributes a;
    if (!XGetWindowAttributes(x->dpy, id, &a))
        return; /* already gone */

    win w = {
        .id = id,
        .x = a.x, .y = a.y, .w = a.width, .h = a.height, .bw = a.border_width,
        .depth = a.depth,
        .input_only = a.class == InputOnly,
        .mapped = a.map_state == IsViewable,
        .nrects = -1,
    };
    if (!w.input_only) {
        w.damage = XDamageCreate(x->dpy, id, XDamageReportNonEmpty);
        XShapeSelectInput(x->dpy, id, ShapeNotifyMask);
        w.pixmap_stale = w.shape_stale = true;
        w.opaque = w.depth != 32;
    }
    insert_at(x, -1, &w);
    if (w.mapped)
        x->redraw = true;
}

static Pixmap read_root_pixmap(xcrt *x)
{
    static const char *names[] = { "_XROOTPMAP_ID", "ESETROOT_PMAP_ID" };
    for (int i = 0; i < 2; i++) {
        Atom prop = XInternAtom(x->dpy, names[i], True);
        if (prop == None)
            continue;
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(x->dpy, x->root, prop, 0, 1, False, XA_PIXMAP, &type, &format,
                               &n, &after, &data) == Success && data) {
            Pixmap p = (type == XA_PIXMAP && format == 32 && n == 1) ? *(Pixmap *)data : None;
            XFree(data);
            if (p)
                return p;
        }
    }
    return None;
}

static void update_cursor_image(xcrt *x)
{
    XFixesCursorImage *img = XFixesGetCursorImage(x->dpy);
    if (img) {
        render_set_cursor(x, img);
        XFree(img);
        x->redraw = true;
    }
}

void scene_update_monitors(xcrt *x)
{
    free(x->mons);
    x->mons = NULL;
    x->nmons = 0;

    if (x->has_randr) {
        int n = 0;
        XRRMonitorInfo *m = XRRGetMonitors(x->dpy, x->root, True, &n);
        if (m && n > 0 && (x->mons = calloc(n, sizeof(rect)))) {
            for (int i = 0; i < n; i++) {
                rect r = { m[i].x, m[i].y, m[i].width, m[i].height };
                /* clip to the screen */
                if (r.x < 0) { r.w += r.x; r.x = 0; }
                if (r.y < 0) { r.h += r.y; r.y = 0; }
                if (r.x + r.w > x->sw) r.w = x->sw - r.x;
                if (r.y + r.h > x->sh) r.h = x->sh - r.y;
                if (r.w > 0 && r.h > 0)
                    x->mons[x->nmons++] = r;
            }
        }
        if (m)
            XRRFreeMonitors(m);
    }
    if (x->nmons == 0) {
        free(x->mons);
        x->mons = malloc(sizeof(rect));
        x->mons[0] = (rect){ 0, 0, x->sw, x->sh };
        x->nmons = 1;
    }
    for (int i = 0; i < x->nmons; i++)
        DBG(x, "monitor %d: %dx%d+%d+%d\n", i, x->mons[i].w, x->mons[i].h, x->mons[i].x, x->mons[i].y);
}

void scene_query_pointer(xcrt *x)
{
    Window r, c;
    int rx, ry, wx, wy;
    unsigned int mask;
    x->pointer_dirty = false;
    if (XQueryPointer(x->dpy, x->root, &r, &c, &rx, &ry, &wx, &wy, &mask) &&
        (rx != x->ptr_x || ry != x->ptr_y)) {
        x->ptr_x = rx;
        x->ptr_y = ry;
        if (x->cursor_hidden)
            x->redraw = true;
    }
}

void scene_set_cursor_hidden(xcrt *x, bool hide)
{
    if (hide == x->cursor_hidden)
        return;
    if (hide)
        XFixesHideCursor(x->dpy, x->root);
    else
        XFixesShowCursor(x->dpy, x->root);
    x->cursor_hidden = hide;
    x->redraw = true;
    if (hide)
        scene_query_pointer(x);
}

void scene_init(xcrt *x)
{
    XGrabServer(x->dpy);
    XSelectInput(x->dpy, x->root, SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);
    Window r, p, *children = NULL;
    unsigned int n = 0;
    if (XQueryTree(x->dpy, x->root, &r, &p, &children, &n)) {
        for (unsigned int i = 0; i < n; i++) /* bottom to top */
            add_window(x, children[i]);
        if (children)
            XFree(children);
    }
    XUngrabServer(x->dpy);

    x->root_pixmap = read_root_pixmap(x);
    x->root_bg_stale = true;

    XFixesSelectCursorInput(x->dpy, x->root, XFixesDisplayCursorNotifyMask);
    update_cursor_image(x);

    if (x->has_xi) {
        unsigned char bits[XIMaskLen(XI_LASTEVENT)] = { 0 };
        XISetMask(bits, XI_RawMotion);
        XIEventMask m = { .deviceid = XIAllMasterDevices, .mask_len = sizeof(bits), .mask = bits };
        XISelectEvents(x->dpy, x->root, &m, 1);
    }
    if (x->has_randr)
        XRRSelectInput(x->dpy, x->root, RRScreenChangeNotifyMask);
    scene_update_monitors(x);

    DBG(x, "tracking %d top-level windows\n", x->nwins);
    x->redraw = true;
}

static void screen_resized(xcrt *x, int w, int h)
{
    if (w == x->sw && h == x->sh)
        return;
    x->sw = w;
    x->sh = h;
    scene_update_monitors(x);
    render_resize(x);
    x->redraw = true;
}

void scene_handle_event(xcrt *x, XEvent *ev)
{
    int i;
    switch (ev->type) {
    case CreateNotify:
        if (ev->xcreatewindow.parent == x->root)
            add_window(x, ev->xcreatewindow.window);
        return;

    case DestroyNotify:
        if ((i = find_index(x, ev->xdestroywindow.window)) >= 0) {
            x->wins[i].damage = None; /* destroyed together with the window */
            remove_at(x, i);
        }
        return;

    case MapNotify:
        if ((i = find_index(x, ev->xmap.window)) >= 0) {
            win *w = &x->wins[i];
            w->mapped = true;
            w->pixmap_stale = w->shape_stale = !w->input_only;
            x->redraw = true;
        }
        return;

    case UnmapNotify:
        if ((i = find_index(x, ev->xunmap.window)) >= 0) {
            x->wins[i].mapped = false;
            render_release_window(x, &x->wins[i]);
            x->redraw = true;
        }
        return;

    case ReparentNotify:
        if (ev->xreparent.parent == x->root)
            add_window(x, ev->xreparent.window);
        else if ((i = find_index(x, ev->xreparent.window)) >= 0)
            remove_at(x, i);
        return;

    case ConfigureNotify: {
        XConfigureEvent *c = &ev->xconfigure;
        if (c->window == x->root) {
            screen_resized(x, c->width, c->height);
            return;
        }
        if ((i = find_index(x, c->window)) < 0)
            return;
        win *w = &x->wins[i];
        if (w->w != c->width || w->h != c->height || w->bw != c->border_width)
            w->pixmap_stale = w->shape_stale = !w->input_only;
        w->x = c->x;
        w->y = c->y;
        w->w = c->width;
        w->h = c->height;
        w->bw = c->border_width;
        if (w->mapped)
            x->redraw = true;
        restack(x, i, c->above);
        return;
    }

    case CirculateNotify:
        if ((i = find_index(x, ev->xcirculate.window)) >= 0) {
            win w = take_at(x, i);
            insert_at(x, ev->xcirculate.place == PlaceOnTop ? -1 : 0, &w);
            x->redraw = true;
        }
        return;

    case PropertyNotify:
        if (ev->xproperty.window == x->root) {
            char *name = XGetAtomName(x->dpy, ev->xproperty.atom);
            if (name && (!strcmp(name, "_XROOTPMAP_ID") || !strcmp(name, "ESETROOT_PMAP_ID"))) {
                x->root_pixmap = read_root_pixmap(x);
                x->root_bg_stale = true;
                x->redraw = true;
            }
            if (name)
                XFree(name);
        }
        return;

    case GenericEvent:
        if (x->has_xi && ev->xcookie.extension == x->xi_opcode && ev->xcookie.evtype == XI_RawMotion)
            x->pointer_dirty = true;
        return;
    }

    if (ev->type == x->damage_event + XDamageNotify) {
        XDamageNotifyEvent *d = (XDamageNotifyEvent *)ev;
        XDamageSubtract(x->dpy, d->damage, None, None);
        if ((i = find_index(x, d->drawable)) >= 0) {
            x->wins[i].damaged = true;
            if (x->wins[i].mapped)
                x->redraw = true;
        }
    } else if (ev->type == x->shape_event + ShapeNotify) {
        XShapeEvent *s = (XShapeEvent *)ev;
        if (s->kind == ShapeBounding && (i = find_index(x, s->window)) >= 0) {
            x->wins[i].shape_stale = true;
            x->redraw = true;
        }
    } else if (ev->type == x->xfixes_event + XFixesCursorNotify) {
        update_cursor_image(x);
    } else if (x->has_randr && ev->type == x->randr_event + RRScreenChangeNotify) {
        XRRUpdateConfiguration(ev);
        int w = DisplayWidth(x->dpy, x->screen);
        int h = DisplayHeight(x->dpy, x->screen);
        if (w == x->sw && h == x->sh) {
            /* same size, but the monitor layout may have changed */
            scene_update_monitors(x);
            x->redraw = true;
        } else {
            screen_resized(x, w, h);
        }
    }
}

void scene_free(xcrt *x)
{
    for (int i = 0; i < x->nwins; i++)
        free_win(x, &x->wins[i]);
    free(x->wins);
    x->wins = NULL;
    x->nwins = x->capwins = 0;
    free(x->mons);
    x->mons = NULL;
    x->nmons = 0;
}
