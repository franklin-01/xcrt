#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Franklin Cordeiro
# Runs xcrt on a throwaway LXDE desktop inside Xephyr: Openbox, lxpanel and the
# pcmanfm desktop, with xcompmgr as the compositor (picom when xcompmgr is not
# installed). HOME and D-Bus are isolated in a temporary directory, so the real
# session and its settings are never touched. Build xcrt first (make).
# On Debian 13, pcmanfm 1.4.0 may crash on start (a GLib assertion, with or
# without xcrt); the rest of the session still runs.
#
#   lxde/try-in-xephyr.sh            # display :7, 1600x1000
#   lxde/try-in-xephyr.sh :8 1280x800
#
# Close the Xephyr window or press Ctrl+C to end it.

set -euo pipefail

DPY=${1:-:7}
SIZE=${2:-1600x1000}
HERE=$(cd "$(dirname "$0")" && pwd)
XCRT="$HERE/../xcrt"
SANDBOX=$(mktemp -d "${TMPDIR:-/tmp}/xcrt-lxde.XXXXXX")
LOG="${TMPDIR:-/tmp}/xcrt-lxde.log"
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
for cmd in Xephyr openbox dbus-run-session; do
    command -v "$cmd" >/dev/null || { echo "missing: $cmd" >&2; exit 1; }
done
if command -v xcompmgr >/dev/null; then
    COMPOSITOR="xcompmgr -c"
elif command -v picom >/dev/null; then
    COMPOSITOR="picom --backend xrender"
else
    echo "missing: xcompmgr (or picom)" >&2
    exit 1
fi

mkdir -p "$H" "$SANDBOX/run"
chmod 700 "$SANDBOX/run"

Xephyr "$DPY" -screen "$SIZE" -ac -br >/dev/null 2>&1 &
XEPHYR=$!
sleep 1

export HOME="$H" XDG_CONFIG_HOME="$H/.config" XDG_DATA_HOME="$H/.local/share" \
       XDG_CACHE_HOME="$H/.cache" XDG_RUNTIME_DIR="$SANDBOX/run" DISPLAY="$DPY" \
       XDG_CURRENT_DESKTOP=LXDE
unset DBUS_SESSION_BUS_ADDRESS SESSION_MANAGER XDG_SESSION_ID

echo "nested LXDE on $DPY (log: $LOG)"
echo "xcrt stops '$COMPOSITOR' while it runs and starts it again when it exits"

# the compositor runs first, as on a composited LXDE desktop; xcrt then takes over
dbus-run-session -- bash -c '
    openbox &
    sleep 1
    $1 &
    command -v lxpanel >/dev/null && lxpanel &
    command -v pcmanfm >/dev/null && pcmanfm --desktop &
    sleep 2
    "$2" &
    wait' _ "$COMPOSITOR" "$XCRT" > "$LOG" 2>&1 &
wait $! || true
