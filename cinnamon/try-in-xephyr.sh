#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Franklin Cordeiro
# Runs a throwaway Cinnamon inside Xephyr with the xcrt extension enabled.
# Everything (HOME, dconf, D-Bus, runtime dir) is isolated in a temporary
# directory, so the real desktop session and its settings are never touched.
#
#   cinnamon/try-in-xephyr.sh            # display :7, 1600x1000
#   cinnamon/try-in-xephyr.sh :8 1280x800
#
# It prints the command that opens the extension's settings window inside the
# nested session. Close the Xephyr window or press Ctrl+C to end it.

set -euo pipefail

DPY=${1:-:7}
SIZE=${2:-1600x1000}
HERE=$(cd "$(dirname "$0")" && pwd)
SANDBOX=$(mktemp -d "${TMPDIR:-/tmp}/xcrt-cinnamon.XXXXXX")
LOG="${TMPDIR:-/tmp}/xcrt-cinnamon.log"
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
    # gvfs and the document portal leave FUSE mounts in the runtime dir
    awk -v d="$SANDBOX" 'index($2, d) == 1 { print $2 }' /proc/mounts | while read -r m; do
        fusermount3 -u "$m" 2>/dev/null || fusermount -u "$m" 2>/dev/null || true
    done
    rm -rf "$SANDBOX"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

for cmd in Xephyr cinnamon dbus-run-session gsettings; do
    command -v "$cmd" >/dev/null || { echo "missing: $cmd" >&2; exit 1; }
done

mkdir -p "$H/.local/share/cinnamon/extensions" "$SANDBOX/run"
chmod 700 "$SANDBOX/run"
ln -s "$HERE/xcrt@xcrt" "$H/.local/share/cinnamon/extensions/xcrt@xcrt"

Xephyr "$DPY" -screen "$SIZE" -ac -br >/dev/null 2>&1 &
XEPHYR=$!
sleep 1

export HOME="$H" XDG_CONFIG_HOME="$H/.config" XDG_DATA_HOME="$H/.local/share" \
       XDG_CACHE_HOME="$H/.cache" XDG_RUNTIME_DIR="$SANDBOX/run" DISPLAY="$DPY"
unset DBUS_SESSION_BUS_ADDRESS SESSION_MANAGER XDG_SESSION_ID

echo "nested Cinnamon on $DPY (log: $LOG)"
echo "settings window:"
echo "  HOME=$H XDG_CONFIG_HOME=$H/.config XDG_DATA_HOME=$H/.local/share XDG_RUNTIME_DIR=$SANDBOX/run \\"
echo "  DISPLAY=$DPY DBUS_SESSION_BUS_ADDRESS=\$(cat $SANDBOX/bus) xlet-settings extension xcrt@xcrt"

# run in the background and wait, so Ctrl+C / SIGTERM reach the cleanup trap
dbus-run-session -- bash -c '
    echo "$DBUS_SESSION_BUS_ADDRESS" > "$1/bus"
    gsettings set org.cinnamon enabled-extensions "[\"xcrt@xcrt\"]"
    gsettings set org.cinnamon startup-animation false
    exec cinnamon --replace' _ "$SANDBOX" > "$LOG" 2>&1 &
wait $! || true
