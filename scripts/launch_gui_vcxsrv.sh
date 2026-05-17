#!/usr/bin/env bash
# launch_gui_vcxsrv.sh — Launch RetDec GUI using VcXsrv X11 server on Windows.
#
# Prerequisites:
#   1. Install VcXsrv (Windows X server) from the publisher's download page.
#      (download VcXsrv-*.exe and run the installer)
#   2. Launch XLaunch from the Start menu:
#        - Display settings: "Multiple windows"
#        - Session type: "Start no client"
#        - Extra settings: CHECK "Disable access control"
#        - Click Finish
#   3. Run this script from any WSL terminal.
#
# @copyright (c) 2024 Odin Loch Trading as Imortek

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

GUI_BIN="${RETDEC_GUI_BIN:-}"
if [[ -z "$GUI_BIN" ]]; then
  for cand in \
    "$REPO_ROOT/build/linux/src/gui/retdec-gui" \
    "$REPO_ROOT/build/gui-only/src/gui/retdec-gui"; do
    if [[ -f "$cand" ]]; then GUI_BIN="$cand"; break; fi
  done
fi

if [[ -z "$GUI_BIN" || ! -f "$GUI_BIN" ]]; then
    echo "ERROR: retdec-gui not found. Build the GUI preset, then set RETDEC_GUI_BIN or use build/linux or build/gui-only." >&2
    echo "  Example: cmake --preset full-linux-debug && cmake --build build/linux --target retdec-gui" >&2
    exit 1
fi

# Detect Windows host IP for DISPLAY (needed when VcXsrv is running on Windows host)
WIN_IP=$(cat /etc/resolv.conf 2>/dev/null | grep nameserver | awk '{print $2}' | head -1)
if [[ -z "$WIN_IP" ]]; then
    WIN_IP="127.0.0.1"
fi

export DISPLAY="${WIN_IP}:0.0"
export LIBGL_ALWAYS_INDIRECT=0
unset WAYLAND_DISPLAY

echo "Using DISPLAY=$DISPLAY (VcXsrv on Windows host)"
echo ""

# Quick test that VcXsrv is accepting connections
if ! xset q &>/dev/null 2>&1; then
    echo "ERROR: Cannot connect to X server at $DISPLAY"
    echo ""
    echo "Make sure VcXsrv (XLaunch) is running on Windows with:"
    echo "  - 'Multiple windows' mode"
    echo "  - 'Disable access control' CHECKED"
    echo ""
    echo "Download VcXsrv from the publisher's site."
    exit 1
fi

echo "X server OK. Launching RetDec GUI..."
exec "$GUI_BIN"
