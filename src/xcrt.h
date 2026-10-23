// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

#pragma once

#include <stdbool.h>
#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>

#include "config.h"

typedef struct {
    Window  id;
    int     x, y;        /* outer corner (including border) */
    int     w, h, bw;    /* interior size and border width */
    int     depth;
    bool    input_only;
    bool    mapped;
    Damage  damage;

    bool    pixmap_stale; /* must re-name the window pixmap (map/resize) */
    bool    damaged;      /* contents changed since the texture was bound */
    bool    shape_stale;

    Pixmap      pixmap;
    GLXPixmap   glxpixmap;
    GLuint      tex;
    bool        y_inverted;
    bool        opaque;   /* depth-24 window: ignore alpha */

    XRectangle *rects;    /* bounding shape, relative to the interior origin */
    int         nrects;
} win;

typedef struct {
    int x, y, w, h;
} rect;

typedef struct xcrt {
    Display *dpy;
    int      screen;
    Window   root;
    int      sw, sh;
    Window   overlay, glwin, cm_owner;
    bool     debug;

    int  damage_event, damage_error;
    int  xfixes_event, xfixes_error;
    int  shape_event, shape_error;
    int  randr_event, randr_error;
    int  xi_opcode;
    bool has_randr, has_xi;

    win *wins; /* stacking order, bottom to top */
    int  nwins, capwins;

    rect *mons;
    int   nmons;

    /* software cursor: drawn into the scene so it follows the curvature */
    bool cursor_hidden;
    bool pointer_dirty;
    int  ptr_x, ptr_y;

    /* desktop wallpaper (_XROOTPMAP_ID) */
    Pixmap root_pixmap;
    bool   root_bg_stale;

    xcrt_config cfg;             /* live presets, edited by `set` */
    xcrt_config saved;           /* presets as stored on disk, for `reset` */
    char        cfg_path[4096];
    bool        enabled;
    float       curvature_scale; /* live value, starts at cfg.curvature_scale */
    Atom        cmd_atom;        /* _XCRT_COMMAND, see xcrt --send */
    Atom        state_atom;      /* _XCRT_STATE, read by xcrt --status and xcrt-gui */
    bool        state_dirty;
    bool        redraw;
    int         settle_frames; /* extra frames to render while phosphors decay */
} xcrt;

#define LOG(...) fprintf(stderr, "xcrt: " __VA_ARGS__)
#define DBG(x, ...) do { if ((x)->debug) fprintf(stderr, "xcrt[dbg]: " __VA_ARGS__); } while (0)

/* main.c */
void trap_errors(xcrt *x);
int  untrap_errors(xcrt *x);
static inline const crt_preset *current_preset(const xcrt *x)
{
    return &x->cfg.presets[x->cfg.current];
}

/* wmcomp.c: replaces a running xfwm4, picom, compton or xcompmgr compositor
 * while xcrt runs and restores it afterwards. Returns false if that failed. */
bool wmcomp_pause(xcrt *x, const char *selection);

/* scene.c */
void scene_init(xcrt *x);
void scene_handle_event(xcrt *x, XEvent *ev);
void scene_update_monitors(xcrt *x);
void scene_query_pointer(xcrt *x);
void scene_set_cursor_hidden(xcrt *x, bool hide);
void scene_free(xcrt *x);

/* render.c */
bool render_init(xcrt *x);
void render_resize(xcrt *x);
void render_frame(xcrt *x);
void render_release_window(xcrt *x, win *w);
void render_set_cursor(xcrt *x, XFixesCursorImage *img);
void render_set_vsync(xcrt *x, bool on);
void render_cleanup(xcrt *x);
