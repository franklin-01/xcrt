#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Franklin Cordeiro
# Runs xcrt on a throwaway MATE desktop inside Xephyr: Marco with its built-in
# compositor on, plus mate-panel and the caja desktop when they are installed.
# HOME, dconf and D-Bus are isolated in a temporary directory, so the real
# session and its settings are never touched. Build xcrt first (make).
#
#   mate/try-in-xephyr.sh            # display :7, 1600x1000
#   mate/try-in-xephyr.sh :8 1280x800
#
# Close the Xephyr window or press Ctrl+C to end it.

set -euo pipefail

DPY=${1:-:7}
SIZE=${2:-1600x1000}
HERE=$(cd "$(dirname "$0")" && pwd)
XCRT="$HERE/../xcrt"
SANDBOX=$(mktemp -d "${TMPDIR:-/tmp}/xcrt-mate.XXXXXX")
LOG="${TMPDIR:-/tmp}/xcrt-mate.log"
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
for cmd in Xephyr marco gsettings dbus-run-session; do
    command -v "$cmd" >/dev/null || { echo "missing: $cmd" >&2; exit 1; }
done

mkdir -p "$H" "$SANDBOX/run"
chmod 700 "$SANDBOX/run"

Xephyr "$DPY" -screen "$SIZE" -ac -br >/dev/null 2>&1 &
XEPHYR=$!
sleep 1

export HOME="$H" XDG_CONFIG_HOME="$H/.config" XDG_DATA_HOME="$H/.local/share" \
       XDG_CACHE_HOME="$H/.cache" XDG_RUNTIME_DIR="$SANDBOX/run" DISPLAY="$DPY" \
       XDG_CURRENT_DESKTOP=MATE
unset DBUS_SESSION_BUS_ADDRESS SESSION_MANAGER XDG_SESSION_ID

echo "nested MATE on $DPY (log: $LOG)"
echo "xcrt pauses the Marco compositor; check it with:"
echo "  DBUS_SESSION_BUS_ADDRESS=\$(cat $SANDBOX/bus) gsettings get org.mate.Marco.general compositing-manager"

# Marco starts composited, as on a normal MATE desktop; xcrt then takes over
dbus-run-session -- bash -c '
    echo "$DBUS_SESSION_BUS_ADDRESS" > "$1/bus"
    gsettings set org.mate.Marco.general compositing-manager true
    marco &
    sleep 2
    command -v mate-panel >/dev/null && mate-panel &
    command -v caja >/dev/null && caja -n --force-desktop &
    sleep 2
    "$2" &
    wait' _ "$SANDBOX" "$XCRT" > "$LOG" 2>&1 &
wait $! || true
