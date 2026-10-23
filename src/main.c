// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

#include "xcrt.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t sig_quit, sig_toggle, sig_next, sig_reload;
static int sig_pipe[2] = { -1, -1 };

static xcrt *the_xcrt;
static int   last_error;
static bool  trapping;

static int on_x_error(Display *dpy, XErrorEvent *e)
{
    last_error = e->error_code;
    if (the_xcrt && the_xcrt->debug && !trapping) {
        char buf[128];
        XGetErrorText(dpy, e->error_code, buf, sizeof(buf));
        DBG(the_xcrt, "X error: %s (request %d.%d, resource 0x%lx)\n", buf, e->request_code,
            e->minor_code, e->resourceid);
    }
    return 0; /* windows vanish all the time; never abort */
}

void trap_errors(xcrt *x)
{
    XSync(x->dpy, False);
    last_error = 0;
    trapping = true;
}

int untrap_errors(xcrt *x)
{
    XSync(x->dpy, False);
    trapping = false;
    int e = last_error;
    last_error = 0;
    return e;
}

static void on_signal(int s)
{
    switch (s) {
    case SIGINT:
    case SIGTERM: sig_quit = 1; break;
    case SIGUSR1: sig_toggle = 1; break;
    case SIGUSR2: sig_next = 1; break;
    case SIGHUP:  sig_reload = 1; break;
    }
    int saved = errno;
    if (sig_pipe[1] >= 0)
        (void)!write(sig_pipe[1], "", 1);
    errno = saved;
}

/* ------------------------------------------------------------------ */
/* hotkeys                                                             */

static const unsigned int lock_masks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };

static void grab_key(xcrt *x, const hotkey *k, bool grab)
{
    if (!k->keysym)
        return;
    KeyCode kc = XKeysymToKeycode(x->dpy, k->keysym);
    if (!kc)
        return;
    for (int i = 0; i < 4; i++) {
        if (grab)
            XGrabKey(x->dpy, kc, k->mods | lock_masks[i], x->root, True, GrabModeAsync, GrabModeAsync);
        else
            XUngrabKey(x->dpy, kc, k->mods | lock_masks[i], x->root);
    }
}

static void grab_keys(xcrt *x, bool grab)
{
    const hotkey *keys[] = {
        &x->cfg.toggle_key, &x->cfg.next_key, &x->cfg.prev_key,
        &x->cfg.curv_up_key, &x->cfg.curv_down_key, &x->cfg.curv_reset_key,
    };
    trap_errors(x);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        grab_key(x, keys[i], grab);
    if (untrap_errors(x) && grab)
        LOG("warning: some hotkeys are already taken by another program\n");
}

static bool key_matches(xcrt *x, const XKeyEvent *e, const hotkey *k)
{
    unsigned int mods = e->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
    return k->keysym && e->keycode == XKeysymToKeycode(x->dpy, k->keysym) && mods == k->mods;
}

/* ------------------------------------------------------------------ */
/* actions                                                             */

/* Every visible change also refreshes the state published for xcrt-gui. */
static void changed(xcrt *x)
{
    x->redraw = true;
    x->state_dirty = true;
}

static void set_enabled(xcrt *x, bool on)
{
    x->enabled = on;
    /* the software cursor is only needed to follow the curvature */
    scene_set_cursor_hidden(x, on && x->cfg.draw_cursor);
    changed(x);
    LOG("effect %s\n", on ? "on" : "off");
}

static void select_preset(xcrt *x, int idx)
{
    int n = x->cfg.npresets;
    x->cfg.current = ((idx % n) + n) % n;
    changed(x);
    LOG("preset: %s\n", current_preset(x)->name);
}

static void set_curvature(xcrt *x, float scale)
{
    if (scale < 0)
        scale = 0;
    if (scale > CURVATURE_SCALE_MAX)
        scale = CURVATURE_SCALE_MAX;
    scale = roundf(scale * 100.0f) / 100.0f; /* repeated steps must not drift */
    x->curvature_scale = scale;
    changed(x);
    const crt_preset *p = current_preset(x);
    LOG("curvature: %.0f%% (x %.3f, y %.3f)\n", scale * 100.0f, p->curvature_x * scale,
        p->curvature_y * scale);
}

/* Edits one parameter of the current preset; a leading +/- on a number is relative. */
static void set_param(xcrt *x, const char *key, const char *val)
{
    crt_preset *p = &x->cfg.presets[x->cfg.current];
    float cur, v;
    char num[32];
    if (preset_get_float(p, key, &cur)) {
        if (!config_parse_scale(val, &v)) {
            LOG("invalid value '%s' for %s\n", val, key);
            return;
        }
        if (val[0] == '+' || val[0] == '-')
            v += cur;
        if (v < 0 && strncmp(key, "curvature", 9)) /* negative curvature is pincushion */
            v = 0;
        v = roundf(v * 10000.0f) / 10000.0f;
        snprintf(num, sizeof(num), "%g", v);
        val = num;
    }
    if (!preset_set_value(&x->cfg, p, key, val)) {
        LOG("invalid setting '%s = %s'\n", key, val);
        return;
    }
    DBG(x, "%s: %s = %s\n", p->name, key, val);
    changed(x);
}

/* Drops unsaved edits of the current preset. */
static void reset_preset(xcrt *x)
{
    crt_preset *p = &x->cfg.presets[x->cfg.current];
    int i = config_find_preset(&x->saved, p->name);
    if (i < 0) {
        LOG("preset '%s' has no saved version\n", p->name);
        return;
    }
    *p = x->saved.presets[i];
    changed(x);
    LOG("preset '%s' restored\n", p->name);
}

static bool valid_preset_name(const char *s)
{
    if (!*s || strlen(s) >= sizeof(((crt_preset *)0)->name))
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("-_.", *s))
            return false;
    return true;
}

/* Writes the current preset (optionally under a new name) to the config file. */
static void save_preset(xcrt *x, const char *name)
{
    crt_preset *cur = &x->cfg.presets[x->cfg.current];
    if (name && *name && strcmp(name, cur->name)) {
        if (!valid_preset_name(name)) {
            LOG("invalid preset name '%s' (use letters, digits, '-', '_' and '.')\n", name);
            return;
        }
        crt_preset copy = *cur;
        int src = config_find_preset(&x->saved, cur->name);
        if (src >= 0)
            *cur = x->saved.presets[src]; /* the original keeps its saved values */
        crt_preset *dst = config_get_or_add_preset(&x->cfg, name); /* may move cur */
        if (!dst)
            return;
        snprintf(copy.name, sizeof(copy.name), "%s", name);
        *dst = copy;
        x->cfg.current = config_find_preset(&x->cfg, name);
    }

    const crt_preset *p = current_preset(x);
    if (!config_save(x->cfg_path, p, x->curvature_scale)) {
        LOG("could not write %s: %s\n", x->cfg_path, strerror(errno));
        return;
    }
    crt_preset *s = config_get_or_add_preset(&x->saved, p->name);
    if (s)
        *s = *p;
    x->cfg.curvature_scale = x->saved.curvature_scale = x->curvature_scale;
    snprintf(x->cfg.start_preset, sizeof(x->cfg.start_preset), "%s", p->name);
    snprintf(x->saved.start_preset, sizeof(x->saved.start_preset), "%s", p->name);
    changed(x);
    LOG("saved preset '%s' to %s\n", p->name, x->cfg_path);
}

/* _XCRT_STATE on the selection owner window, in config syntax, read by xcrt-gui. */
static void publish_state(xcrt *x)
{
    x->state_dirty = false;
    const crt_preset *p = current_preset(x);
    int si = config_find_preset(&x->saved, p->name);
    bool modified = si < 0 || !preset_equal(p, &x->saved.presets[si]);

    size_t cap = 8192 + (size_t)x->cfg.npresets * 64, n;
    char *buf = malloc(cap);
    if (!buf)
        return;
    n = snprintf(buf, cap,
                 "version = 1\nenabled = %d\npreset = %s\nmodified = %d\ncurvature_scale = %g\nconfig = %s\n",
                 x->enabled, p->name, modified, x->curvature_scale, x->cfg_path);
    for (int i = 0; i < x->cfg.npresets && n < cap; i++)
        n += snprintf(buf + n, cap - n, "available = %s\n", x->cfg.presets[i].name);
    if (n < cap)
        n += preset_format(p, buf + n, cap - n);
    if (n >= cap)
        n = cap - 1;
    XChangeProperty(x->dpy, x->cm_owner, x->state_atom, XA_STRING, 8, PropModeReplace,
                    (unsigned char *)buf, (int)n);
    free(buf);
}

static void reload_config(xcrt *x);

static char *trim_ws(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

/* Splits "word rest of line"; returns the rest (possibly empty). */
static char *split_word(char *s)
{
    char *rest = s + strcspn(s, " \t");
    if (*rest)
        *rest++ = '\0';
    return trim_ws(rest);
}

/* Commands arrive from `xcrt --send`, one per line. */
static void run_command(xcrt *x, char *line)
{
    char *cmd = trim_ws(line);
    char *arg = split_word(cmd);
    if (!*cmd)
        return;

    if (!strcmp(cmd, "toggle")) {
        set_enabled(x, !x->enabled);
    } else if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {
        set_enabled(x, cmd[1] == 'n');
    } else if (!strcmp(cmd, "next")) {
        select_preset(x, x->cfg.current + 1);
    } else if (!strcmp(cmd, "prev")) {
        select_preset(x, x->cfg.current - 1);
    } else if (!strcmp(cmd, "preset") && *arg) {
        int i = config_find_preset(&x->cfg, arg);
        if (i >= 0)
            select_preset(x, i);
        else
            LOG("unknown preset '%s'\n", arg);
    } else if (!strcmp(cmd, "reload")) {
        reload_config(x);
    } else if (!strcmp(cmd, "curvature")) {
        float v;
        if (!*arg)
            set_curvature(x, x->curvature_scale); /* just report it */
        else if (!strcmp(arg, "reset"))
            set_curvature(x, x->cfg.curvature_scale);
        else if (config_parse_scale(arg, &v))
            set_curvature(x, (arg[0] == '+' || arg[0] == '-') ? x->curvature_scale + v : v);
        else
            LOG("invalid curvature '%s' (use e.g. 1.5, 150%%, +10%% or reset)\n", arg);
    } else if (!strcmp(cmd, "set")) {
        char *val = split_word(arg);
        if (*arg && *val)
            set_param(x, arg, val);
        else
            LOG("usage: set PARAMETER VALUE\n");
    } else if (!strcmp(cmd, "reset")) {
        reset_preset(x);
    } else if (!strcmp(cmd, "save")) {
        save_preset(x, arg);
    } else {
        LOG("unknown command '%s'\n", cmd);
    }
}

static void read_commands(xcrt *x)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    /* read-and-delete is atomic, so appends from other clients are never lost */
    if (XGetWindowProperty(x->dpy, x->cm_owner, x->cmd_atom, 0, 65536, True, XA_STRING, &type, &format,
                           &n, &after, &data) != Success || !data)
        return;
    if (type == XA_STRING && format == 8) {
        char *save = NULL;
        for (char *line = strtok_r((char *)data, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
            run_command(x, line);
    }
    XFree(data);
}

static void reload_config(xcrt *x)
{
    xcrt_config n, saved;
    config_init(&n);
    if (!config_load_file(&n, x->cfg_path))
        LOG("could not read %s, using built-in presets\n", x->cfg_path);
    config_init(&saved);
    config_load_file(&saved, x->cfg_path);

    int idx = config_find_preset(&n, current_preset(x)->name);
    if (idx < 0)
        idx = config_find_preset(&n, n.start_preset);
    n.current = idx < 0 ? 0 : idx;

    /* keep a live curvature tweak unless the file itself changed the value */
    if (n.curvature_scale != x->cfg.curvature_scale)
        x->curvature_scale = n.curvature_scale;

    grab_keys(x, false);
    config_free(&x->cfg);
    config_free(&x->saved);
    x->cfg = n;
    x->saved = saved;
    grab_keys(x, true);
    render_set_vsync(x, x->cfg.vsync);
    scene_set_cursor_hidden(x, x->enabled && x->cfg.draw_cursor);
    changed(x);
    LOG("configuration reloaded, preset: %s\n", current_preset(x)->name);
}

static void handle_key(xcrt *x, const XKeyEvent *e)
{
    if (key_matches(x, e, &x->cfg.toggle_key))
        set_enabled(x, !x->enabled);
    else if (key_matches(x, e, &x->cfg.next_key))
        select_preset(x, x->cfg.current + 1);
    else if (key_matches(x, e, &x->cfg.prev_key))
        select_preset(x, x->cfg.current - 1);
    else if (key_matches(x, e, &x->cfg.curv_up_key))
        set_curvature(x, x->curvature_scale + x->cfg.curvature_step);
    else if (key_matches(x, e, &x->cfg.curv_down_key))
        set_curvature(x, x->curvature_scale - x->cfg.curvature_step);
    else if (key_matches(x, e, &x->cfg.curv_reset_key))
        set_curvature(x, x->cfg.curvature_scale);
}

/* Frames needed for a phosphor at full brightness to fade below 1/256. */
static int settle_frames(float decay)
{
    if (decay <= 0)
        return 0;
    if (decay > 0.98f)
        decay = 0.98f;
    return (int)ceilf(logf(1.0f / 256.0f) / logf(decay));
}

static void count_frame(xcrt *x)
{
    static struct timespec start;
    static int frames;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (frames++ == 0)
        start = now;
    double dt = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) * 1e-9;
    if (dt >= 5.0) {
        DBG(x, "%.1f fps (%d frames in %.1fs)\n", (frames - 1) / dt, frames - 1, dt);
        frames = 0;
    }
}

/* ------------------------------------------------------------------ */
/* setup                                                               */

static bool check_extensions(xcrt *x)
{
    int ev, err, major = 0, minor = 0;
    if (!XCompositeQueryExtension(x->dpy, &ev, &err)) {
        LOG("the X server lacks the Composite extension\n");
        return false;
    }
    XCompositeQueryVersion(x->dpy, &major, &minor);
    if (major == 0 && minor < 3) {
        LOG("Composite 0.3 or newer is required (server has %d.%d)\n", major, minor);
        return false;
    }
    if (!XDamageQueryExtension(x->dpy, &x->damage_event, &x->damage_error)) {
        LOG("the X server lacks the DAMAGE extension\n");
        return false;
    }
    if (!XFixesQueryExtension(x->dpy, &x->xfixes_event, &x->xfixes_error)) {
        LOG("the X server lacks the XFIXES extension\n");
        return false;
    }
    major = 5;
    minor = 0;
    XFixesQueryVersion(x->dpy, &major, &minor);
    if (major < 4) {
        LOG("XFIXES 4.0 or newer is required\n");
        return false;
    }
    if (!XShapeQueryExtension(x->dpy, &x->shape_event, &x->shape_error)) {
        LOG("the X server lacks the SHAPE extension\n");
        return false;
    }
    x->has_randr = XRRQueryExtension(x->dpy, &x->randr_event, &x->randr_error);

    int xi_ev, xi_err;
    x->has_xi = XQueryExtension(x->dpy, "XInputExtension", &x->xi_opcode, &xi_ev, &xi_err);
    if (x->has_xi) {
        major = 2;
        minor = 0;
        x->has_xi = XIQueryVersion(x->dpy, &major, &minor) == Success;
    }
    DBG(x, "RandR: %s, XInput2: %s\n", x->has_randr ? "yes" : "no", x->has_xi ? "yes" : "no");
    return true;
}

static bool become_compositor(xcrt *x)
{
    char name[32];
    snprintf(name, sizeof(name), "_NET_WM_CM_S%d", x->screen);
    Atom sel = XInternAtom(x->dpy, name, False);
    if (!wmcomp_pause(x, name))
        return false;
    if (XGetSelectionOwner(x->dpy, sel) != None) {
        LOG("another compositor is already running on this screen.\n"
            "      xcrt replaces xfwm4, picom, compton and xcompmgr by itself, but not this one;\n"
            "      turn its compositing off first. Cinnamon, GNOME and KDE always composite\n"
            "      (Cinnamon has the xcrt@xcrt extension instead).\n");
        return false;
    }
    x->cm_owner = XCreateSimpleWindow(x->dpy, x->root, 0, 0, 1, 1, 0, 0, 0);
    Xutf8SetWMProperties(x->dpy, x->cm_owner, "xcrt", "xcrt", NULL, 0, NULL, NULL, NULL);
    XSetSelectionOwner(x->dpy, sel, x->cm_owner, CurrentTime);
    /* `xcrt --send` appends commands to a property on this window */
    x->cmd_atom = XInternAtom(x->dpy, "_XCRT_COMMAND", False);
    x->state_atom = XInternAtom(x->dpy, "_XCRT_STATE", False);
    XSelectInput(x->dpy, x->cm_owner, PropertyChangeMask);

    trap_errors(x);
    XCompositeRedirectSubwindows(x->dpy, x->root, CompositeRedirectManual);
    if (untrap_errors(x)) {
        LOG("could not redirect windows; is another compositor running?\n");
        return false;
    }

    x->overlay = XCompositeGetOverlayWindow(x->dpy, x->root);
    XserverRegion empty = XFixesCreateRegion(x->dpy, NULL, 0);
    XFixesSetWindowShapeRegion(x->dpy, x->overlay, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(x->dpy, empty);
    return true;
}

static void shutdown_xcrt(xcrt *x)
{
    if (!x->dpy)
        return;
    grab_keys(x, false);
    scene_set_cursor_hidden(x, false);
    scene_free(x); /* needs the GL context */
    render_cleanup(x);
    if (x->overlay) {
        XCompositeUnredirectSubwindows(x->dpy, x->root, CompositeRedirectManual);
        XCompositeReleaseOverlayWindow(x->dpy, x->root);
    }
    if (x->cm_owner)
        XDestroyWindow(x->dpy, x->cm_owner);
    XCloseDisplay(x->dpy);
    x->dpy = NULL;
}

/* Client side: finds the selection-owner window of a running xcrt. */
static Display *connect_xcrt(Window *owner)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        LOG("cannot open display '%s'\n", XDisplayName(NULL));
        return NULL;
    }
    char name[32];
    snprintf(name, sizeof(name), "_NET_WM_CM_S%d", DefaultScreen(dpy));
    *owner = XGetSelectionOwner(dpy, XInternAtom(dpy, name, False));
    char *wm_name = NULL;
    bool ours = *owner && XFetchName(dpy, *owner, &wm_name) && wm_name && !strcmp(wm_name, "xcrt");
    if (wm_name)
        XFree(wm_name);
    if (!ours) {
        LOG("xcrt is not running on display '%s'\n", XDisplayName(NULL));
        XCloseDisplay(dpy);
        return NULL;
    }
    return dpy;
}

/* --send: appends one command line to the running instance. */
static int send_command(int argc, char **argv)
{
    char cmd[512] = "";
    for (int i = 0; i < argc; i++) {
        if (i)
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 2);
    }
    strcat(cmd, "\n");

    Window owner;
    Display *dpy = connect_xcrt(&owner);
    if (!dpy)
        return 1;
    XChangeProperty(dpy, owner, XInternAtom(dpy, "_XCRT_COMMAND", False), XA_STRING, 8, PropModeAppend,
                    (unsigned char *)cmd, strlen(cmd));
    XCloseDisplay(dpy);
    return 0;
}

/* --status: prints the state the running instance publishes. */
static int print_status(void)
{
    Window owner;
    Display *dpy = connect_xcrt(&owner);
    if (!dpy)
        return 1;
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    int rc = 1;
    if (XGetWindowProperty(dpy, owner, XInternAtom(dpy, "_XCRT_STATE", False), 0, 65536, False, XA_STRING,
                           &type, &format, &n, &after, &data) == Success && data) {
        fwrite(data, 1, n, stdout);
        rc = 0;
    }
    if (data)
        XFree(data);
    XCloseDisplay(dpy);
    return rc;
}

static void usage(FILE *f)
{
    fprintf(f,
            "usage: xcrt [options]\n"
            "       xcrt -s COMMAND [ARG...]\n"
            "       xcrt --status\n"
            "\n"
            "CRT filter compositor for X11 / XLibre window managers.\n"
            "\n"
            "  -c, --config FILE      config file (default: ~/.config/xcrt/xcrt.conf)\n"
            "  -p, --preset NAME      start with this preset\n"
            "  -k, --curvature SCALE  curvature intensity: 0 (flat) to 3, or 0%%..300%% (default 100%%)\n"
            "  -l, --list-presets     list available presets and exit\n"
            "      --no-cursor        keep the hardware cursor (it will not follow curvature)\n"
            "      --no-vsync         render without vertical sync\n"
            "      --disabled         start with the effect turned off\n"
            "  -d, --debug            verbose logging\n"
            "  -h, --help             show this help\n"
            "\n"
            "Controlling a running xcrt (-s, --send must come first):\n"
            "  xcrt -s toggle | on | off | next | prev | reload\n"
            "  xcrt -s preset NAME\n"
            "  xcrt -s curvature 150%%      intensity of every preset's curvature (+10%%, -0.1, reset)\n"
            "  xcrt -s set PARAM VALUE     edit the current preset: set bloom 0.5, set bloom +0.1,\n"
            "                              set mask slot, set tint #33ff66 (names as in xcrt.conf)\n"
            "  xcrt -s reset               drop unsaved edits of the current preset\n"
            "  xcrt -s save [NAME]         write the current preset (or a copy named NAME) to the config\n"
            "  xcrt --status               print the current state and parameters\n"
            "\n"
            "signals: SIGUSR1 toggles the effect, SIGUSR2 cycles presets, SIGHUP reloads the config.\n"
            "\n"
            "Copyright (C) 2026 Franklin Cordeiro. Free software under the GPL-3.0-or-later,\n"
            "with NO WARRANTY. See the LICENSE file for details.\n");
}

int main(int argc, char **argv)
{
    static xcrt x;
    the_xcrt = &x;

    /* handled before getopt so "curvature -10%" is not taken for options */
    if (argc >= 2 && (!strcmp(argv[1], "-s") || !strcmp(argv[1], "--send"))) {
        if (argc < 3) {
            usage(stderr);
            return 2;
        }
        return send_command(argc - 2, argv + 2);
    }
    if (argc == 2 && !strcmp(argv[1], "--status"))
        return print_status();

    const char *cfg_arg = NULL, *preset_arg = NULL;
    bool list = false, no_cursor = false, no_vsync = false, disabled = false;
    float curvature_arg = -1;
    enum { OPT_NO_CURSOR = 256, OPT_NO_VSYNC, OPT_DISABLED };
    static const struct option opts[] = {
        { "config", required_argument, NULL, 'c' },
        { "preset", required_argument, NULL, 'p' },
        { "curvature", required_argument, NULL, 'k' },
        { "list-presets", no_argument, NULL, 'l' },
        { "no-cursor", no_argument, NULL, OPT_NO_CURSOR },
        { "no-vsync", no_argument, NULL, OPT_NO_VSYNC },
        { "disabled", no_argument, NULL, OPT_DISABLED },
        { "debug", no_argument, NULL, 'd' },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    int o;
    while ((o = getopt_long(argc, argv, "c:p:k:ldh", opts, NULL)) != -1) {
        switch (o) {
        case 'c': cfg_arg = optarg; break;
        case 'p': preset_arg = optarg; break;
        case 'k':
            if (!config_parse_scale(optarg, &curvature_arg) || curvature_arg < 0 ||
                curvature_arg > CURVATURE_SCALE_MAX) {
                LOG("--curvature takes a value from 0 to %.0f (or 0%% to %.0f%%)\n", CURVATURE_SCALE_MAX,
                    CURVATURE_SCALE_MAX * 100);
                return 2;
            }
            break;
        case 'l': list = true; break;
        case 'd': x.debug = true; break;
        case 'h': usage(stdout); return 0;
        case OPT_NO_CURSOR: no_cursor = true; break;
        case OPT_NO_VSYNC: no_vsync = true; break;
        case OPT_DISABLED: disabled = true; break;
        default: usage(stderr); return 2;
        }
    }

    config_init(&x.cfg);
    if (cfg_arg)
        snprintf(x.cfg_path, sizeof(x.cfg_path), "%s", cfg_arg);
    else
        config_default_path(x.cfg_path, sizeof(x.cfg_path));
    if (!config_load_file(&x.cfg, x.cfg_path)) {
        if (cfg_arg) {
            LOG("cannot read config file %s\n", cfg_arg);
            return 1;
        }
        DBG(&x, "no config at %s, using built-in presets\n", x.cfg_path);
    }

    config_init(&x.saved);
    config_load_file(&x.saved, x.cfg_path);

    const char *start = preset_arg ? preset_arg : x.cfg.start_preset;
    int idx = config_find_preset(&x.cfg, start);
    if (list) {
        for (int i = 0; i < x.cfg.npresets; i++)
            printf("%s%s\n", x.cfg.presets[i].name, i == idx ? " (default)" : "");
        return 0;
    }
    if (idx < 0) {
        LOG("unknown preset '%s' (see --list-presets)\n", start);
        if (preset_arg)
            return 1;
        idx = 0;
    }
    x.cfg.current = idx;
    if (no_cursor)
        x.cfg.draw_cursor = false;
    if (no_vsync)
        x.cfg.vsync = false;
    if (disabled)
        x.cfg.start_enabled = false;
    if (curvature_arg >= 0)
        x.cfg.curvature_scale = curvature_arg;
    x.curvature_scale = x.cfg.curvature_scale;

    if (!(x.dpy = XOpenDisplay(NULL))) {
        LOG("cannot open display '%s'\n", XDisplayName(NULL));
        return 1;
    }
    XSetErrorHandler(on_x_error);
    x.screen = DefaultScreen(x.dpy);
    x.root = RootWindow(x.dpy, x.screen);
    x.sw = DisplayWidth(x.dpy, x.screen);
    x.sh = DisplayHeight(x.dpy, x.screen);

    if (!check_extensions(&x) || !become_compositor(&x) || !render_init(&x)) {
        shutdown_xcrt(&x);
        return 1;
    }
    scene_init(&x);
    set_enabled(&x, x.cfg.start_enabled);
    grab_keys(&x, true);

    if (pipe2(sig_pipe, O_NONBLOCK | O_CLOEXEC) < 0)
        sig_pipe[0] = sig_pipe[1] = -1;
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGINT, SIGTERM, SIGUSR1, SIGUSR2, SIGHUP };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);

    LOG("running on %dx%d (%d monitor%s), preset '%s', curvature %.0f%%\n", x.sw, x.sh, x.nmons,
        x.nmons == 1 ? "" : "s", current_preset(&x)->name, x.curvature_scale * 100.0f);

    struct pollfd fds[2] = {
        { .fd = ConnectionNumber(x.dpy), .events = POLLIN },
        { .fd = sig_pipe[0], .events = POLLIN },
    };
    while (!sig_quit) {
        if (sig_toggle) {
            sig_toggle = 0;
            set_enabled(&x, !x.enabled);
        }
        if (sig_next) {
            sig_next = 0;
            select_preset(&x, x.cfg.current + 1);
        }
        if (sig_reload) {
            sig_reload = 0;
            reload_config(&x);
        }

        while (XPending(x.dpy)) {
            XEvent ev;
            XNextEvent(x.dpy, &ev);
            if (ev.type == KeyPress)
                handle_key(&x, &ev.xkey);
            else if (ev.type == PropertyNotify && ev.xproperty.window == x.cm_owner) {
                if (ev.xproperty.atom == x.cmd_atom && ev.xproperty.state == PropertyNewValue)
                    read_commands(&x);
            } else
                scene_handle_event(&x, &ev);
        }
        if (x.state_dirty)
            publish_state(&x);
        /* without XInput2 there are no motion events on the root; poll instead */
        if (x.pointer_dirty || (!x.has_xi && x.cursor_hidden))
            scene_query_pointer(&x);

        const crt_preset *p = current_preset(&x);
        bool animated = x.enabled && preset_is_animated(p);
        if (x.redraw && x.enabled)
            x.settle_frames = settle_frames(p->persistence);

        if (x.redraw || animated || x.settle_frames > 0) {
            if (!x.redraw && x.settle_frames > 0)
                x.settle_frames--;
            x.redraw = false;
            render_frame(&x);
            if (x.debug)
                count_frame(&x);
            continue;
        }

        if (XPending(x.dpy))
            continue;
        int timeout = (!x.has_xi && x.cursor_hidden) ? 8 : -1;
        if (poll(fds, sig_pipe[0] >= 0 ? 2 : 1, timeout) > 0 && (fds[1].revents & POLLIN)) {
            char buf[64];
            while (read(sig_pipe[0], buf, sizeof(buf)) > 0)
                ;
        }
    }

    LOG("exiting\n");
    shutdown_xcrt(&x);
    config_free(&x.cfg);
    config_free(&x.saved);
    return 0;
}
