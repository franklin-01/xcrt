#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Franklin Cordeiro
# Runs xcrt on a throwaway XFCE desktop (xfwm4, panel and desktop) inside
# Xephyr. HOME, xfconf and D-Bus are isolated in a temporary directory, so the
# real session and its settings are never touched. Build xcrt first (make).
#
#   xfce/try-in-xephyr.sh            # display :7, 1600x1000
#   xfce/try-in-xephyr.sh :8 1280x800
#
# Close the Xephyr window or press Ctrl+C to end it.

set -euo pipefail

DPY=${1:-:7}
SIZE=${2:-1600x1000}
HERE=$(cd "$(dirname "$0")" && pwd)
XCRT="$HERE/../xcrt"
SANDBOX=$(mktemp -d "${TMPDIR:-/tmp}/xcrt-xfce.XXXXXX")
LOG="${TMPDIR:-/tmp}/xcrt-xfce.log"
H="$SANDBOX/home"
XEPHYR=

cleanup() {
    if [ -n "$XEPHYR" ]; then kill "$XEPHYR" 2>/dev/null || true; fi
    # everything the nested session started carries the sandbox HOME
    local p
    for p in $(pgrep -u "$(id -u)"); do
        if tr '\0' '\n' 2>/dev/null < "/proc/$p/environ" | grep -qx "HOME=$H"; then
            kill "$p" 2>/dev/null || true
        fi
    done
    sleep 1
    awk -v d="$SANDBOX" 'index($2, d) == 1 { print $2 }' /proc/mounts | while read -r m; do
        fusermount3 -u "$m" 2>/dev/null || fusermount -u "$m" 2>/dev/null || true
    done
    rm -rf "$SANDBOX"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

[ -x "$XCRT" ] || { echo "build xcrt first: make" >&2; exit 1; }
for cmd in Xephyr xfwm4 xfce4-panel xfconf-query dbus-run-session; do
    command -v "$cmd" >/dev/null || { echo "missing: $cmd" >&2; exit 1; }
done

mkdir -p "$H/.config/xfce4/xfconf/xfce-perchannel-xml" "$SANDBOX/run"
chmod 700 "$SANDBOX/run"
# start the panel with its default layout instead of the first-run dialog
if [ -f /etc/xdg/xfce4/panel/default.xml ]; then
    cp /etc/xdg/xfce4/panel/default.xml "$H/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml"
fi

Xephyr "$DPY" -screen "$SIZE" -ac -br >/dev/null 2>&1 &
XEPHYR=$!
sleep 1

export HOME="$H" XDG_CONFIG_HOME="$H/.config" XDG_DATA_HOME="$H/.local/share" \
       XDG_CACHE_HOME="$H/.cache" XDG_RUNTIME_DIR="$SANDBOX/run" DISPLAY="$DPY" \
       XDG_CURRENT_DESKTOP=XFCE
unset DBUS_SESSION_BUS_ADDRESS SESSION_MANAGER XDG_SESSION_ID

echo "nested XFCE on $DPY (log: $LOG)"
echo "xcrt pauses the xfwm4 compositor; check it with:"
echo "  DBUS_SESSION_BUS_ADDRESS=\$(cat $SANDBOX/bus) xfconf-query -c xfwm4 -p /general/use_compositing"

# xfwm4 starts composited, as on a normal XFCE desktop; xcrt then takes over
dbus-run-session -- bash -c '
    echo "$DBUS_SESSION_BUS_ADDRESS" > "$1/bus"
    xfwm4 &
    sleep 2
    xfce4-panel &
    command -v xfdesktop >/dev/null && xfdesktop &
    sleep 2
    "$2" &
    wait' _ "$SANDBOX" "$XCRT" > "$LOG" 2>&1 &
wait $! || true
