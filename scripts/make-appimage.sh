#!/usr/bin/env bash
# make-appimage.sh — Build a portable Linux AppImage for RetDec.
#
# Usage:
#   ./scripts/make-appimage.sh --install-dir <cmake-install-dir> \
#                              --out <retdec-x86_64.AppImage>
#
# Options:
#   --install-dir DIR   CMake install prefix (contains bin/, lib/, share/)
#   --out FILE          Output AppImage path (default: retdec-<version>-x86_64.AppImage)
#   --version VER       Version string embedded in AppImage (default: 0.1.0)
#   --appimage-tool P   Path to appimagetool binary (auto-downloaded if absent)
#   --linuxdeploy P     Path to linuxdeploy binary (auto-downloaded if absent)
#   --dry-run           Print what would be done without doing it
#
# Requires:
#   FUSE (for AppImage mounting; on CI: APPIMAGE_EXTRACT_AND_RUN=1)
#   linuxdeploy + linuxdeploy-plugin-qt (auto-downloaded from GitHub releases)
#   appimagetool (auto-downloaded from GitHub releases)

set -euo pipefail

INSTALL_DIR=""
OUT_FILE=""
VERSION="0.1.0"
APPIMAGE_TOOL=""
LINUXDEPLOY=""
DRY_RUN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-dir)    INSTALL_DIR="$2";    shift 2 ;;
        --out)            OUT_FILE="$2";        shift 2 ;;
        --version)        VERSION="$2";         shift 2 ;;
        --appimage-tool)  APPIMAGE_TOOL="$2";   shift 2 ;;
        --linuxdeploy)    LINUXDEPLOY="$2";     shift 2 ;;
        --dry-run)        DRY_RUN=1;            shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

[[ -n "$INSTALL_DIR" ]] || { echo "ERROR: --install-dir required" >&2; exit 1; }

OUT_FILE="${OUT_FILE:-retdec-${VERSION}-x86_64.AppImage}"

_run() {
    if [[ "$DRY_RUN" -eq 1 ]]; then echo "[dry-run] $*"; else "$@"; fi
}

# ─── Download helpers ─────────────────────────────────────────────────────────
_download_if_missing() {
    local _path="$1" _url="$2"
    if [[ ! -x "$_path" ]]; then
        echo "Downloading: $_url → $_path"
        _run curl -fsSL "$_url" -o "$_path"
        _run chmod +x "$_path"
    fi
}

TOOLS_DIR="${TOOLS_DIR:-/tmp/retdec-appimage-tools}"
_run mkdir -p "$TOOLS_DIR"

# linuxdeploy
if [[ -z "$LINUXDEPLOY" ]]; then
    LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"
    _download_if_missing "$LINUXDEPLOY" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fi

# linuxdeploy-plugin-qt (for Qt6 library bundling)
LINUXDEPLOY_PLUGIN_QT="$TOOLS_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"
_download_if_missing "$LINUXDEPLOY_PLUGIN_QT" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

# appimagetool
if [[ -z "$APPIMAGE_TOOL" ]]; then
    APPIMAGE_TOOL="$TOOLS_DIR/appimagetool-x86_64.AppImage"
    _download_if_missing "$APPIMAGE_TOOL" \
        "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
fi

# ─── AppDir structure ─────────────────────────────────────────────────────────
APPDIR="${APPDIR:-/tmp/RetDec.AppDir}"
_run rm -rf "$APPDIR"
_run mkdir -p "$APPDIR/usr"

echo "=== Populating AppDir ==="
_run cp -r "$INSTALL_DIR/." "$APPDIR/usr/"

# ─── AppStream / desktop / icon ───────────────────────────────────────────────
_run mkdir -p "$APPDIR/usr/share/applications"
_run mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
_run mkdir -p "$APPDIR/usr/share/metainfo"

# .desktop file
cat > "${DRY_RUN:+/dev/null}" <<'DESKTOP'
[Desktop Entry]
Name=RetDec
GenericName=Binary Decompiler
Comment=Retargetable Machine-Code Decompiler with AI assistance
Exec=retdec-gui %f
Icon=retdec
Terminal=false
Type=Application
Categories=Development;Debugger;
MimeType=application/x-executable;application/x-sharedlib;
DESKTOP

if [[ "$DRY_RUN" -eq 0 ]]; then
cat > "$APPDIR/usr/share/applications/retdec.desktop" <<'DESKTOP'
[Desktop Entry]
Name=RetDec
GenericName=Binary Decompiler
Comment=Retargetable Machine-Code Decompiler with AI assistance
Exec=retdec-gui %f
Icon=retdec
Terminal=false
Type=Application
Categories=Development;Debugger;
MimeType=application/x-executable;application/x-sharedlib;
DESKTOP
fi

# AppStream metainfo
if [[ "$DRY_RUN" -eq 0 ]]; then
cat > "$APPDIR/usr/share/metainfo/retdec.appdata.xml" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>retdec.desktop</id>
  <name>RetDec</name>
  <summary>Retargetable Machine-Code Decompiler</summary>
  <description>
    <p>
      RetDec is an open-source retargetable machine-code decompiler based on LLVM.
      This enhanced edition adds a Qt6 GUI, CUDA GPU acceleration, AI-assisted
      variable naming via Qwen3-Coder, and advanced semantic recovery for STL
      containers, cryptographic algorithms, and design patterns.
    </p>
  </description>
  <url type="homepage">https://github.com/odin-loki/RetDec-Decompiler</url>
  <releases>
    <release version="${VERSION}" date="$(date -u +%Y-%m-%d)"/>
  </releases>
</component>
XML
fi

# Placeholder icon (1x1 PNG; replace with real icon in production)
if [[ "$DRY_RUN" -eq 0 && ! -f "$APPDIR/usr/share/icons/hicolor/256x256/apps/retdec.png" ]]; then
    # Create a minimal valid PNG if ImageMagick is available; otherwise skip.
    if command -v convert >/dev/null 2>&1; then
        convert -size 256x256 xc:'#1e1e2e' -fill '#89b4fa' \
            -gravity Center -pointsize 80 -annotate 0 'R' \
            "$APPDIR/usr/share/icons/hicolor/256x256/apps/retdec.png" 2>/dev/null || true
    fi
fi

# ─── AppDir entry symlinks ─────────────────────────────────────────────────────
if [[ "$DRY_RUN" -eq 0 ]]; then
    ln -sf "usr/share/applications/retdec.desktop" "$APPDIR/retdec.desktop"
    ln -sf "usr/share/icons/hicolor/256x256/apps/retdec.png" "$APPDIR/retdec.png" 2>/dev/null || true
fi

# ─── Run linuxdeploy to bundle Qt6 libraries ──────────────────────────────────
echo ""
echo "=== Running linuxdeploy (Qt plugin) ==="
export APPIMAGE_EXTRACT_AND_RUN=1   # Avoid FUSE requirement in CI
export QT_SELECT=qt6

_run env \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    OUTPUT="$OUT_FILE" \
    "$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/retdec.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/retdec.png" \
    --plugin qt \
    --output appimage

echo ""
echo "=== AppImage created: $OUT_FILE ==="
echo "Test: APPIMAGE_EXTRACT_AND_RUN=1 ./${OUT_FILE} --self-test"
