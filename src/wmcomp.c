// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

/* Window managers with a built-in compositor. xfwm4 (XFCE) composites by
 * default; instead of refusing to start, xcrt switches that compositor off
 * through xfconf while it runs. A small watchdog process turns it back on as
 * soon as xcrt is gone, whether it exits cleanly, crashes or is killed. */

#include "xcrt.h"

#include <X11/Xatom.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define XFWM4_QUERY "xfconf-query -c xfwm4 -p /general/use_compositing"

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

/* Name of the running window manager (_NET_SUPPORTING_WM_CHECK), or NULL. */
static char *wm_name(Display *dpy, Window root)
{
    Atom check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", True);
    Atom name = XInternAtom(dpy, "_NET_WM_NAME", True);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", True);
    if (check == None || name == None || utf8 == None)
        return NULL;

    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    Window wm = None;
    if (XGetWindowProperty(dpy, root, check, 0, 1, False, XA_WINDOW, &type, &format, &n, &after,
                           &data) == Success && data) {
        if (type == XA_WINDOW && n == 1)
            wm = *(Window *)data;
        XFree(data);
    }
    if (wm == None)
        return NULL;

    char *result = NULL;
    data = NULL;
    if (XGetWindowProperty(dpy, wm, name, 0, 256, False, utf8, &type, &format, &n, &after, &data) ==
            Success && data) {
        if (type == utf8 && format == 8)
            result = strndup((char *)data, n);
        XFree(data);
    }
    return result;
}

static bool xfwm4_compositing_on(void)
{
    FILE *f = popen(XFWM4_QUERY " 2>/dev/null", "r");
    if (!f)
        return false;
    char buf[16] = "";
    bool read_ok = fgets(buf, sizeof(buf), f) != NULL;
    return pclose(f) == 0 && read_ok && !strncmp(buf, "true", 4);
}

static bool set_xfwm4_compositing(bool on)
{
    return system(on ? XFWM4_QUERY " -s true" : XFWM4_QUERY " -s false") == 0;
}

/* Runs in a child process: blocks until xcrt exits (the pipe closes), waits
 * for its compositor selection to be released, then re-enables xfwm4's. */
static void watchdog(int fd, const char *display, const char *selection)
{
    /* a distinct name keeps `pkill -x xcrt` and its signals off this process */
    prctl(PR_SET_NAME, "xcrt-xfwm4");
    int ignored[] = { SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGPIPE };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        signal(ignored[i], SIG_IGN);

    char c;
    ssize_t r;
    do
        r = read(fd, &c, 1);
    while (r > 0 || (r < 0 && errno == EINTR));

    Display *dpy = XOpenDisplay(display);
    if (dpy) {
        Atom sel = XInternAtom(dpy, selection, False);
        for (int i = 0; i < 40 && XGetSelectionOwner(dpy, sel) != None; i++)
            sleep_ms(50);
        XCloseDisplay(dpy);
    }
    if (set_xfwm4_compositing(true))
        fprintf(stderr, "xcrt: xfwm4 compositor restored\n");
    else
        fprintf(stderr, "xcrt: could not turn the xfwm4 compositor back on; enable it in "
                        "Window Manager Tweaks > Compositor\n");
    _exit(0);
}

static bool start_watchdog(xcrt *x, const char *selection)
{
    int fds[2];
    if (pipe(fds) < 0)
        return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[1]);
        close(ConnectionNumber(x->dpy)); /* the child opens its own connection */
        watchdog(fds[0], DisplayString(x->dpy), selection);
    }
    /* keep the write end open for xcrt's whole life; children must not inherit it */
    close(fds[0]);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

bool wmcomp_pause(xcrt *x, const char *selection)
{
    char *name = wm_name(x->dpy, x->root);
    bool xfwm4 = name && !strcasecmp(name, "Xfwm4");
    DBG(x, "window manager: %s\n", name ? name : "(none)");
    free(name);
    if (!xfwm4 || !xfwm4_compositing_on())
        return true;

    /* the watchdog comes first, so the setting is restored even if pausing half-works */
    if (!start_watchdog(x, selection)) {
        LOG("could not start the xfwm4 watchdog\n");
        return false;
    }
    LOG("pausing the xfwm4 compositor while xcrt runs\n");
    if (!set_xfwm4_compositing(false)) {
        LOG("xfconf-query failed; turn off compositing in Window Manager Tweaks > Compositor\n");
        return false;
    }
    Atom sel = XInternAtom(x->dpy, selection, False);
    for (int i = 0; i < 60 && XGetSelectionOwner(x->dpy, sel) != None; i++)
        sleep_ms(50);
    return true;
}
