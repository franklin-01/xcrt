// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

/* Compositors that xcrt replaces while it runs, instead of refusing to start:
 *  - xfwm4 (XFCE) has a built-in compositor, switched off through xfconf;
 *  - picom, compton and xcompmgr (common with LXQt, LXDE, Openbox and other
 *    window managers) run as their own process, which is stopped and later
 *    started again with the same command line.
 * A small watchdog process puts the previous compositor back as soon as xcrt is
 * gone, whether it exits cleanly, crashes or is killed. */

#include "xcrt.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define XFWM4_QUERY "xfconf-query -c xfwm4 -p /general/use_compositing"

/* Standalone compositors that are safe to stop and start again. */
static const char *const known_compositors[] = { "picom", "compton", "xcompmgr" };

/* signals the watchdog ignores; they must be reset before starting a compositor */
static const int watchdog_ignored[] = { SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGPIPE };

static bool   restore_xfwm4; /* turn xfwm4's compositor back on */
static char **restart_argv;  /* command line of a compositor to start again */
static char   restart_name[64];

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

static ssize_t read_proc(pid_t pid, const char *what, char *buf, size_t len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, what);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static bool is_known_compositor(const char *name)
{
    for (size_t i = 0; i < sizeof(known_compositors) / sizeof(known_compositors[0]); i++)
        if (!strcmp(name, known_compositors[i]))
            return true;
    return false;
}

/* Display number of a DISPLAY string (":7", ":7.0", "unix:7"), or -1. */
static int display_number(const char *s)
{
    const char *colon = s ? strrchr(s, ':') : NULL;
    return colon && colon[1] >= '0' && colon[1] <= '9' ? atoi(colon + 1) : -1;
}

/* xcompmgr does not publish _NET_WM_PID. Its selection window is named after
 * it, so look for the only process of this user with that name that is
 * connected to the same display; give up if there is more than one. */
static pid_t pid_by_name(Display *dpy, Window owner)
{
    char *name = NULL;
    if (!XFetchName(dpy, owner, &name) || !name)
        return 0;
    char wanted[64];
    snprintf(wanted, sizeof(wanted), "%s", name);
    XFree(name);
    if (!is_known_compositor(wanted))
        return 0;

    int dpy_num = display_number(DisplayString(dpy));
    DIR *proc = opendir("/proc");
    if (!proc)
        return 0;
    pid_t found = 0;
    int matches = 0;
    struct dirent *e;
    while ((e = readdir(proc))) {
        pid_t pid = atoi(e->d_name);
        char comm[64];
        if (pid <= 0 || read_proc(pid, "comm", comm, sizeof(comm)) <= 0)
            continue;
        comm[strcspn(comm, "\n")] = '\0';
        if (strcmp(comm, wanted))
            continue;
        /* other users' environments are unreadable, which also skips them */
        static char env[65536];
        ssize_t n = read_proc(pid, "environ", env, sizeof(env));
        for (char *p = env; n > 0 && p < env + n; p += strlen(p) + 1)
            if (!strncmp(p, "DISPLAY=", 8) && display_number(p + 8) == dpy_num) {
                found = pid;
                matches++;
                break;
            }
    }
    closedir(proc);
    if (matches > 1)
        LOG("%d processes named %s use this display; not stopping any of them\n", matches, wanted);
    return matches == 1 ? found : 0;
}

/* PID of the process owning the compositor selection, if it is one of the
 * known compositors, runs on this machine and belongs to this user. */
static pid_t known_compositor_pid(Display *dpy, Window owner)
{
    Atom pid_atom = XInternAtom(dpy, "_NET_WM_PID", True);
    pid_t pid = 0;
    if (pid_atom != None) {
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, owner, pid_atom, 0, 1, False, XA_CARDINAL, &type, &format, &n,
                               &after, &data) == Success && data) {
            if (type == XA_CARDINAL && format == 32 && n == 1)
                pid = (pid_t)*(unsigned long *)data;
            XFree(data);
        }
    }
    if (pid <= 0)
        pid = pid_by_name(dpy, owner);
    if (pid <= 0)
        return 0;

    /* a PID is only meaningful for a client on this machine */
    XTextProperty machine;
    if (XGetWMClientMachine(dpy, owner, &machine)) {
        char host[256] = "";
        gethostname(host, sizeof(host) - 1);
        bool same = machine.value && !strcmp((char *)machine.value, host);
        XFree(machine.value);
        if (!same)
            return 0;
    }

    char comm[64];
    if (read_proc(pid, "comm", comm, sizeof(comm)) <= 0)
        return 0;
    comm[strcspn(comm, "\n")] = '\0';
    bool known = is_known_compositor(comm);

    char path[64];
    struct stat st;
    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    if (!known || stat(path, &st) < 0 || st.st_uid != getuid())
        return 0;
    snprintf(restart_name, sizeof(restart_name), "%s", comm);
    return pid;
}

/* The process's command line as a NULL-terminated argv. */
static char **process_argv(pid_t pid)
{
    static char buf[16384];
    ssize_t n = read_proc(pid, "cmdline", buf, sizeof(buf));
    if (n <= 0)
        return NULL;
    int argc = 0;
    for (ssize_t i = 0; i < n; i++)
        argc += buf[i] == '\0';
    char **argv = calloc(argc + 1, sizeof(char *));
    if (!argv)
        return NULL;
    char *p = buf;
    for (int i = 0; i < argc; i++) {
        argv[i] = strdup(p);
        p += strlen(p) + 1;
    }
    return argv;
}

static void restart_compositor(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid(); /* outlive the terminal or session that started xcrt */
        /* ignored signals survive exec; the compositor must die on SIGTERM again */
        for (size_t i = 0; i < sizeof(watchdog_ignored) / sizeof(watchdog_ignored[0]); i++)
            signal(watchdog_ignored[i], SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execvp(restart_argv[0], restart_argv);
        _exit(127);
    }
    if (pid > 0)
        fprintf(stderr, "xcrt: %s started again\n", restart_name);
    else
        fprintf(stderr, "xcrt: could not start %s again\n", restart_name);
}

/* Runs in a child process: blocks until xcrt exits (the pipe closes), waits
 * for its compositor selection to be released, then restores the previous
 * compositor. */
static void watchdog(int fd, const char *display, const char *selection)
{
    /* a distinct name keeps `pkill -x xcrt` and its signals off this process */
    prctl(PR_SET_NAME, "xcrt-restore");
    for (size_t i = 0; i < sizeof(watchdog_ignored) / sizeof(watchdog_ignored[0]); i++)
        signal(watchdog_ignored[i], SIG_IGN);

    char c;
    ssize_t r;
    do
        r = read(fd, &c, 1);
    while (r > 0 || (r < 0 && errno == EINTR));
    close(fd);

    Display *dpy = XOpenDisplay(display);
    if (dpy) {
        Atom sel = XInternAtom(dpy, selection, False);
        for (int i = 0; i < 40 && XGetSelectionOwner(dpy, sel) != None; i++)
            sleep_ms(50);
        XCloseDisplay(dpy);
    }
    if (restore_xfwm4) {
        if (set_xfwm4_compositing(true))
            fprintf(stderr, "xcrt: xfwm4 compositor restored\n");
        else
            fprintf(stderr, "xcrt: could not turn the xfwm4 compositor back on; enable it in "
                            "Window Manager Tweaks > Compositor\n");
    } else if (restart_argv) {
        restart_compositor();
    }
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
    Atom sel = XInternAtom(x->dpy, selection, False);
    char *name = wm_name(x->dpy, x->root);
    bool xfwm4 = name && !strcasecmp(name, "Xfwm4");
    DBG(x, "window manager: %s\n", name ? name : "(none)");
    free(name);

    pid_t pid = 0;
    if (xfwm4 && xfwm4_compositing_on()) {
        restore_xfwm4 = true;
    } else {
        Window owner = XGetSelectionOwner(x->dpy, sel);
        if (owner != None && (pid = known_compositor_pid(x->dpy, owner)) > 0)
            restart_argv = process_argv(pid);
        if (!restart_argv)
            return true; /* nothing xcrt knows how to replace */
    }

    /* the watchdog comes first, so the old compositor returns even if stopping half-works */
    if (!start_watchdog(x, selection)) {
        LOG("could not start the watchdog that restores the previous compositor\n");
        return false;
    }
    if (restore_xfwm4) {
        LOG("pausing the xfwm4 compositor while xcrt runs\n");
        if (!set_xfwm4_compositing(false)) {
            LOG("xfconf-query failed; turn off compositing in Window Manager Tweaks > Compositor\n");
            return false;
        }
    } else {
        LOG("stopping %s (pid %d) while xcrt runs; it will be started again afterwards\n", restart_name,
            (int)pid);
        if (kill(pid, SIGTERM) < 0) {
            LOG("could not stop %s: %s\n", restart_name, strerror(errno));
            return false;
        }
    }
    for (int i = 0; i < 60 && XGetSelectionOwner(x->dpy, sel) != None; i++)
        sleep_ms(50);
    return true;
}
