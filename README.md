# xcrt

A CRT filter for the whole X11 / XLibre desktop: tube curvature, scanlines,
phosphor mask, bloom, chromatic aberration, vignette, noise, flicker, hum bar
and phosphor persistence, with presets you can switch with a hotkey. Every
effect can be tuned live, from the `xcrt-gui` window or from the command line.

xcrt is a **compositor**: it redirects every window (XComposite), assembles the
screen into an OpenGL texture and runs the CRT shader on each monitor before
showing it. The cursor is drawn inside the scene too, so it follows the
curvature and clicks land exactly where they appear to.

## Compatibility

| Environment | Works? |
|---|---|
| WMs without a compositor: i3, bspwm, openbox, dwm, awesome, fluxbox, herbstluftwm, IceWM… | yes |
| XFCE (xfwm4) | yes; xcrt pauses the xfwm4 compositor by itself (see [XFCE](#xfce)) |
| LXQt, and any WM running picom or compton | yes; xcrt stops picom/compton and starts it again on exit (see [LXQt](#lxqt)) |
| marco and other compositors | yes, after turning their compositing off |
| Cinnamon | yes, through the `xcrt@xcrt` extension (see [Cinnamon](#cinnamon)) |
| GNOME, KDE (the WM is the compositor) | not yet; they need their own plugin |

Only one compositor can run at a time. If another one is active, xcrt says so
and exits. On Cinnamon, use the extension instead of the `xcrt` executable.

## Building

Dependencies (Debian / Ubuntu / Mint):

```sh
sudo apt install build-essential pkg-config xxd \
    libx11-dev libxext-dev libxcomposite-dev libxdamage-dev \
    libxfixes-dev libxrandr-dev libxi-dev libgl-dev
```

The graphical interface uses Python 3 with GTK 3 (preinstalled on Mint and Ubuntu):

```sh
sudo apt install python3-gi gir1.2-gtk-3.0
```

```sh
make
sudo make install        # optional: installs xcrt, xcrt-gui and the menu entry
```

## Usage

```sh
xcrt                     # default preset (pc)
xcrt -p green            # pick a preset
xcrt -k 50%              # half of the preset's curvature
xcrt --list-presets
```

Add `xcrt &` to `~/.xinitrc` or to your WM's autostart (i3: `exec --no-startup-id xcrt`).

| Shortcut | Action |
|---|---|
| `Super+Shift+F12` | toggle the effect |
| `Super+Shift+F11` | next preset |
| `Super+Shift+F10` | previous preset |
| `Super+Shift+Page_Up` | more curvature (+10%) |
| `Super+Shift+Page_Down` | less curvature (−10%) |
| `Super+Shift+Home` | curvature back to the configured value |

### Curvature intensity

The intensity multiplies the curvature of any preset: `0%` makes the screen
flat, `100%` is the preset's own curvature and the maximum is `300%`. It stays
in effect when you switch presets, and each preset keeps its shape (`trinitron`
stays curved horizontally only). The starting value comes from
`curvature_scale` in the config or from `-k/--curvature`.

## Tuning the effects

### Graphical interface

```sh
xcrt-gui                 # or ./gui/xcrt-gui straight from the project folder
```

While xcrt is running, the window shows the current preset and one control per
effect, grouped in tabs (Geometry, Lines & mask, Image, Phosphor and
Interference). Changes show up on screen immediately. **Save** writes the preset
to the config file, **Save as…** creates a new preset and **Discard** goes back
to the saved values. The window also follows changes made with hotkeys or from
the command line. If xcrt is not running, it offers a button to start it.

### Command line

`xcrt -s` sends a command to the running instance. It is meant for WM
bindings, scripts and the terminal:

```sh
xcrt -s set bloom 0.5        # any xcrt.conf parameter, on the current preset
xcrt -s set bloom +0.1       # relative change (also -0.1)
xcrt -s set scanline_strength 80%
xcrt -s set mask aperture    # none, aperture, slot, shadow
xcrt -s set tint #ffb000
xcrt -s reset                # drop unsaved changes to the current preset
xcrt -s save                 # write the current preset to the config
xcrt -s save my-crt          # save it as a new preset
xcrt -s curvature 150%       # curvature intensity (also +10%, -0.1, reset)
xcrt -s preset green
xcrt -s toggle               # also: on, off, next, prev, reload
xcrt --status                # print the state and every parameter
```

`save` only rewrites the `[preset name]` section and the `preset` and
`curvature_scale` keys of `[general]`; comments and the rest of the file are
kept.

Example for i3:

```
bindsym $mod+bracketright exec --no-startup-id xcrt -s curvature +10%
bindsym $mod+bracketleft  exec --no-startup-id xcrt -s curvature -10%
bindsym $mod+backslash    exec --no-startup-id xcrt-gui
```

Signals work too:

```sh
pkill -USR1 -x xcrt      # toggle
pkill -USR2 -x xcrt      # next preset
pkill -HUP  -x xcrt      # reload the config
```

If xcrt hangs or dies, X hands the windows back automatically and the screen
returns to normal.

## XFCE

xfwm4 has its own compositor. When xcrt starts on XFCE it switches that
compositor off (the same switch as *Window Manager Tweaks → Compositor*) and
switches it back on when xcrt exits. A small watchdog process, `xcrt-restore`,
does the restoring, so it also happens if xcrt crashes or is killed with
`kill -9`. If you had already turned the xfwm4 compositor off yourself, xcrt
leaves it off.

To start xcrt with the session:

```sh
sudo make install            # xcrt must be in PATH
make install-autostart       # adds ~/.config/autostart/xcrt.desktop
```

You can also add `xcrt` in *Settings → Session and Startup → Application
Autostart*. While xcrt runs, xfwm4's own compositor effects (shadows, window
opacity settings) are off.

To try it without touching your session, this script opens a throwaway XFCE
desktop (xfwm4, panel and desktop) inside Xephyr, with its own HOME, settings
and D-Bus:

```sh
xfce/try-in-xephyr.sh
```

## LXQt

LXQt has no compositor of its own: it runs a window manager (Openbox by
default) and usually picom for compositing. When xcrt starts and finds picom or
compton running, it stops it and starts it again with the same command line
once xcrt exits. As on XFCE, the `xcrt-restore` watchdog takes care of that even
if xcrt crashes or is killed. xcrt only ever stops processes named `picom` or
`compton` that belong to you; any other compositor is left alone, and xcrt
asks you to turn it off instead.

The same applies to Openbox, i3 and other window managers that run picom.

To start xcrt with the session, use `make install-autostart` (see [XFCE](#xfce))
or add `xcrt` in *LXQt Configuration Center → Session Settings → Autostart*.

To try it without touching your session, this script runs Openbox and picom
inside Xephyr (plus lxqt-panel and the pcmanfm-qt desktop when they are
installed):

```sh
lxqt/try-in-xephyr.sh
```

## Cinnamon

On Cinnamon, muffin is already the compositor. That is why there is the
`xcrt@xcrt` extension, which applies the same shader inside Cinnamon itself,
including the panel, menus and notifications.

```sh
make install-cinnamon        # copies it to ~/.local/share/cinnamon/extensions, no sudo
```

Then enable it in **System Settings → Extensions → xcrt – CRT filter**. The gear
button opens its settings: preset, curvature intensity and every effect, grouped
in tabs and applied immediately. Changing an effect switches the preset to
*Custom*.

| Shortcut (editable in the General tab) | Action |
|---|---|
| `Super+Shift+F12` | toggle the filter |
| `Super+Shift+F11` | next preset |
| `Super+Shift+Page_Up` / `Page_Down` | curvature ±10% |

Differences from the standalone compositor:

- There is no phosphor persistence, because a Cinnamon effect cannot read the previous frame.
- Bloom is computed in a single pass; it is lighter and slightly less smooth.
- While the filter is on, Cinnamon redraws the whole screen on every change (the
  curvature moves pixels around), which uses a bit more GPU.
- Fullscreen windows (games, videos) stay inside the filter; this can be turned
  off in the General tab.

To try it without touching your session, this script opens a throwaway Cinnamon
inside Xephyr, with its own HOME, settings and D-Bus:

```sh
cinnamon/try-in-xephyr.sh
```

If something goes wrong in your real session, `Super+Shift+F12` turns the filter
off. To remove the extension for good, run `make uninstall-cinnamon` and restart
Cinnamon with `Ctrl+Alt+Esc`.

## Presets

| Name | Look |
|---|---|
| `arcade` | strong curvature, slot mask, pronounced scanlines, bloom |
| `pc` | a 90s PC monitor, subtle enough to work with |
| `trinitron` | almost flat screen, aperture grille |
| `tv` | CRT TV: noise, jitter, hum bar, shadow mask |
| `green` | green phosphor terminal with persistence |
| `amber` | amber phosphor terminal |

## Configuration

Copy `xcrt.conf.example` to `~/.config/xcrt/xcrt.conf`. Every parameter and
hotkey is documented there. A `[preset name]` section changes a built-in preset
or creates a new one (`base = arcade` inherits the values of another).

## Testing without leaving your session

Run a nested X server with Xephyr:

```sh
Xephyr :5 -screen 1280x800 -ac -br &
DISPLAY=:5 ./xcrt -d &
DISPLAY=:5 ./gui/xcrt-gui &
DISPLAY=:5 xterm &       # or any other program
```

## Limitations

- The standalone compositor has no shadows, no `_NET_WM_WINDOW_OPACITY` transparency and no
  window animations (the Cinnamon extension keeps all of those).
- Fullscreen games go through the filter as well, which adds one frame of latency.
- With `--no-cursor`, the hardware cursor does not follow the curvature.

## License

Copyright (C) 2026 Franklin Cordeiro

xcrt is free software: you can redistribute it and/or modify it under the terms
of the [GNU General Public License](LICENSE) as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. It is distributed in the hope that it will be useful, but **without
any warranty**.

In short: anyone may use, study, modify and redistribute xcrt, keeping the
authorship notices. Whoever distributes a version (original or modified, as
source or compiled) must offer the corresponding source code under the same
license; closing the source is not allowed.

### Third-party credits

The noise function in the shaders is
[Hash without Sine](https://www.shadertoy.com/view/4djSRW) by David Hoskins,
under the MIT license. The full notice is in `shaders/crt.frag` and in
`cinnamon/xcrt@xcrt/crt.glsl`.
