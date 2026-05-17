#!/usr/bin/env bash
# launch_gui.sh — Launch the RetDec GUI via WSLg
#
# Run this from Windows Terminal (WSL tab), NOT from Cursor's terminal.
# WSLg only creates the display socket during an interactive WSL session.
#
# Usage:
#   bash scripts/launch_gui.sh [--build]
#
# @copyright (c) 2024 Odin Loch Trading as Imortek

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/gui-only"
GUI_BIN="$BUILD_DIR/src/gui/retdec-gui"

# ── Optionally rebuild first ───────────────────────────────────────────────────
if [[ "${1:-}" == "--build" ]]; then
    echo ">>> Building retdec-gui..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DRETDEC_ENABLE_ALL=OFF \
        -DRETDEC_ENABLE_CUDA_ACCEL=ON
    cmake --build "$BUILD_DIR" --target retdec-gui --parallel "$(nproc)"
    echo ">>> Build done."
fi

# ── Verify binary exists ───────────────────────────────────────────────────────
if [[ ! -f "$GUI_BIN" ]]; then
    echo "ERROR: $GUI_BIN not found."
    echo "Run:  bash scripts/launch_gui.sh --build"
    exit 1
fi

# ── Set up display environment ─────────────────────────────────────────────────
# WSLg provides a Wayland compositor; fall back to X11/XCB if needed.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export DISPLAY="${DISPLAY:-:0}"

# Verify the Wayland socket exists (WSLg active check)
WAYLAND_SOCK="$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
if [[ -S "$WAYLAND_SOCK" ]]; then
    echo "[ok] WSLg Wayland socket found: $WAYLAND_SOCK"
else
    echo "WARNING: WSLg Wayland socket not found at $WAYLAND_SOCK"

    # Try VcXsrv: detect Windows host IP from /etc/resolv.conf
    WIN_IP=$(grep nameserver /etc/resolv.conf 2>/dev/null | awk '{print $2}' | head -1)
    if [[ -n "$WIN_IP" ]] && DISPLAY="$WIN_IP:0.0" xset q &>/dev/null 2>&1; then
        echo "[ok] VcXsrv detected at $WIN_IP:0.0 — using it."
        export DISPLAY="$WIN_IP:0.0"
        unset WAYLAND_DISPLAY
    else
        echo ""
        echo "No display found. Try one of:"
        echo "  1. Run 'wsl --shutdown' in PowerShell, reopen this terminal, try again."
        echo "  2. Install VcXsrv from the publisher's download page"
        echo "     then run XLaunch (Multiple windows + Disable access control),"
        echo "     then run: bash scripts/launch_gui_vcxsrv.sh"
        exit 1
    fi
fi

# ── Launch ─────────────────────────────────────────────────────────────────────
echo ""
echo "============================================================"
echo " Launching RetDec GUI"
echo "  Binary:   $GUI_BIN"
echo "  Display:  DISPLAY=$DISPLAY  WAYLAND=$WAYLAND_DISPLAY"
echo "============================================================"
echo ""

exec "$GUI_BIN" "$@"
