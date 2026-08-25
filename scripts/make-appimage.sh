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

# linuxdeploy-plugin-qt (for Qt6 library bundling when retdec-gui is present)
LINUXDEPLOY_PLUGIN_QT="$TOOLS_DIR/linuxdeploy-plugin-qt-x86_64.AppImage"
if [[ -x "${INSTALL_DIR}/bin/retdec-gui" ]]; then
    _download_if_missing "$LINUXDEPLOY_PLUGIN_QT" \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
fi

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

if [[ -x "${INSTALL_DIR}/bin/retdec-gui" ]]; then
    APPIMAGE_EXEC="retdec-gui"
    APPIMAGE_TERMINAL="false"
else
    APPIMAGE_EXEC="retdec-decompiler"
    APPIMAGE_TERMINAL="true"
fi

if [[ "$DRY_RUN" -eq 0 ]]; then
cat > "$APPDIR/usr/share/applications/retdec.desktop" <<DESKTOP
[Desktop Entry]
Name=RetDec
GenericName=Binary Decompiler
Comment=Retargetable Machine-Code Decompiler
Exec=${APPIMAGE_EXEC} %f
Icon=retdec
Terminal=${APPIMAGE_TERMINAL}
Type=Application
Categories=Development;Debugger;
MimeType=application/x-executable;application/x-sharedlib;
DESKTOP
fi

# AppStream metainfo (filename must match the component id)
if [[ "$DRY_RUN" -eq 0 ]]; then
    rm -f "$APPDIR/usr/share/metainfo/retdec.appdata.xml"
cat > "$APPDIR/usr/share/metainfo/io.github.odin_loki.retdec.metainfo.xml" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>io.github.odin_loki.retdec</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>AGPL-3.0-or-later</project_license>
  <name>RetDec</name>
  <summary>Retargetable Machine-Code Decompiler</summary>
  <description>
    <p>
      RetDec is an open-source retargetable machine-code decompiler based on LLVM.
      This package ships the CLI and, when built, the Qt 6 GUI. CUDA/OpenCL
      acceleration is parked research, not a product feature.
    </p>
  </description>
  <launchable type="desktop-id">retdec.desktop</launchable>
  <url type="homepage">https://github.com/odin-loki/RetDec-Decompiler</url>
  <developer id="io.github.odin_loki">
    <name>Odin Loch trading as Imortek</name>
  </developer>
  <content_rating type="oars-1.1"/>
  <releases>
    <release version="${VERSION}" date="$(date -u +%Y-%m-%d)"/>
  </releases>
</component>
XML
fi

# Placeholder icon. linuxdeploy requires a file matching Icon= in the
# desktop entry. ImageMagick is optional; Python 3 writes a valid PNG.
_ensure_icon() {
    local _dest="$1"
    if command -v convert >/dev/null 2>&1; then
        convert -size 256x256 xc:'#1e1e2e' -fill '#89b4fa' \
            -gravity Center -pointsize 80 -annotate 0 'R' \
            "$_dest" 2>/dev/null && return 0
    fi
    if command -v magick >/dev/null 2>&1; then
        magick -size 256x256 xc:'#1e1e2e' -fill '#89b4fa' \
            -gravity Center -pointsize 80 -annotate 0 'R' \
            "$_dest" 2>/dev/null && return 0
    fi
    python3 - "$_dest" <<'PY'
import pathlib, struct, sys, zlib

def chunk(tag: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(tag + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

w = h = 256
rgb = bytes((0x1E, 0x1E, 0x2E))
raw = b"".join(b"\x00" + rgb * w for _ in range(h))
ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", ihdr)
    + chunk(b"IDAT", zlib.compress(raw, 9))
    + chunk(b"IEND", b"")
)
pathlib.Path(sys.argv[1]).write_bytes(png)
PY
}

if [[ "$DRY_RUN" -eq 0 ]]; then
    ICON_PNG="$APPDIR/usr/share/icons/hicolor/256x256/apps/retdec.png"
    if [[ ! -f "$ICON_PNG" ]]; then
        _ensure_icon "$ICON_PNG"
    fi
    mkdir -p "$APPDIR/usr/share/pixmaps"
    cp -f "$ICON_PNG" "$APPDIR/usr/share/pixmaps/retdec.png"
    cp -f "$ICON_PNG" "$APPDIR/retdec.png"
fi

# ─── AppDir entry symlinks ─────────────────────────────────────────────────────
if [[ "$DRY_RUN" -eq 0 ]]; then
    ln -sfn "usr/share/applications/retdec.desktop" "$APPDIR/retdec.desktop"
fi

# ─── Run linuxdeploy ──────────────────────────────────────────────────────────
echo ""
echo "=== Running linuxdeploy ==="
export APPIMAGE_EXTRACT_AND_RUN=1   # Avoid FUSE requirement in CI
export QT_SELECT=qt6

LINUXDEPLOY_ARGS=(
    --appdir "$APPDIR"
    --desktop-file "$APPDIR/usr/share/applications/retdec.desktop"
    --output appimage
)
if [[ -f "$APPDIR/usr/share/icons/hicolor/256x256/apps/retdec.png" ]]; then
    LINUXDEPLOY_ARGS+=(--icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/retdec.png")
fi
if [[ -x "${INSTALL_DIR}/bin/retdec-gui" ]]; then
    LINUXDEPLOY_ARGS+=(--plugin qt)
fi

_run env \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    OUTPUT="$OUT_FILE" \
    LDAI_OUTPUT="$OUT_FILE" \
    "$LINUXDEPLOY" \
    "${LINUXDEPLOY_ARGS[@]}"

echo ""
echo "=== AppImage created: $OUT_FILE ==="
echo "Test: APPIMAGE_EXTRACT_AND_RUN=1 ./${OUT_FILE} --self-test"
