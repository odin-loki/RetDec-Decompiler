#!/usr/bin/env bash
# bundle-windows.sh — Collect a cross-compiled Windows build into a self-contained
# directory ready for NSIS packaging or direct zip distribution.
#
# Usage:
#   ./scripts/bundle-windows.sh --build-dir <mingw-build-dir> --out-dir <bundle-dir>
#
# Options:
#   --build-dir DIR     Directory produced by cmake --build (contains bin/ etc.)
#   --out-dir DIR       Destination bundle directory (will be created / cleared)
#   --qt-root DIR       Root of a Qt6 Windows cross-build (optional;
#                       auto-detected from RETDEC_QT6_WIN_ROOT env var)
#   --mingw-sysroot DIR MinGW sysroot with runtime DLLs
#                       (default: /usr/x86_64-w64-mingw32)
#   --zip               Also produce <out-dir>.zip
#   --dry-run           Print what would be done without doing it
#
# Dependencies (on the Linux/WSL host):
#   x86_64-w64-mingw32-objdump  (binutils-mingw-w64-x86-64)
#   zip                          (optional, for --zip)

set -euo pipefail

# ─── Argument parsing ──────────────────────────────────────────────────────────
BUILD_DIR=""
OUT_DIR=""
QT_ROOT="${RETDEC_QT6_WIN_ROOT:-}"
MINGW_SYSROOT="/usr/x86_64-w64-mingw32"
DO_ZIP=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)   BUILD_DIR="$2";    shift 2 ;;
        --out-dir)     OUT_DIR="$2";      shift 2 ;;
        --qt-root)     QT_ROOT="$2";      shift 2 ;;
        --mingw-sysroot) MINGW_SYSROOT="$2"; shift 2 ;;
        --zip)         DO_ZIP=1;          shift ;;
        --dry-run)     DRY_RUN=1;         shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

[[ -n "$BUILD_DIR" ]] || { echo "ERROR: --build-dir is required" >&2; exit 1; }
[[ -n "$OUT_DIR"   ]] || { echo "ERROR: --out-dir is required"   >&2; exit 1; }

_run() {
    if [[ "$DRY_RUN" -eq 1 ]]; then echo "[dry-run] $*"; else "$@"; fi
}
_cp() { _run cp -v "$@"; }
_mkdir() { _run mkdir -p "$@"; }

echo "=== RetDec Windows Bundle ==="
echo "  build-dir:     $BUILD_DIR"
echo "  out-dir:       $OUT_DIR"
echo "  qt-root:       ${QT_ROOT:-(not set)}"
echo "  mingw-sysroot: $MINGW_SYSROOT"

# ─── Create output layout ──────────────────────────────────────────────────────
_mkdir "$OUT_DIR/bin"
_mkdir "$OUT_DIR/platforms"
_mkdir "$OUT_DIR/imageformats"
_mkdir "$OUT_DIR/share/retdec"

# ─── Copy RetDec binaries ──────────────────────────────────────────────────────
echo ""
echo "--- Copying RetDec binaries ---"
for _exe in "$BUILD_DIR"/bin/*.exe "$BUILD_DIR"/**/*.exe; do
    [[ -f "$_exe" ]] && _cp "$_exe" "$OUT_DIR/bin/" || true
done
# Also check install tree
if [[ -d "$BUILD_DIR/install/bin" ]]; then
    for _exe in "$BUILD_DIR"/install/bin/*.exe; do
        [[ -f "$_exe" ]] && _cp "$_exe" "$OUT_DIR/bin/" || true
    done
fi

# ─── Copy MinGW runtime DLLs ──────────────────────────────────────────────────
echo ""
echo "--- Copying MinGW runtime DLLs ---"
MINGW_BIN="$MINGW_SYSROOT/bin"
if [[ ! -d "$MINGW_BIN" ]]; then
    # Ubuntu package layout: libs are at /usr/lib/gcc/x86_64-w64-mingw32/<ver>/
    MINGW_BIN=$(find /usr/lib/gcc/x86_64-w64-mingw32 -maxdepth 2 -name 'libstdc++*.dll' -printf '%h\n' 2>/dev/null | head -1 || true)
fi

_MINGW_RUNTIME_DLLS=(
    "libstdc++-6.dll"
    "libgcc_s_seh-1.dll"
    "libwinpthread-1.dll"
)
for _dll in "${_MINGW_RUNTIME_DLLS[@]}"; do
    _found=""
    for _dir in "$MINGW_BIN" "/usr/lib/gcc/x86_64-w64-mingw32" /usr/x86_64-w64-mingw32/lib; do
        if [[ -f "$_dir/$_dll" ]]; then
            _found="$_dir/$_dll"
            break
        fi
    done
    # Recursive search as fallback
    if [[ -z "$_found" ]]; then
        _found=$(find /usr -name "$_dll" 2>/dev/null | head -1 || true)
    fi
    if [[ -n "$_found" ]]; then
        _cp "$_found" "$OUT_DIR/bin/"
    else
        echo "WARNING: MinGW runtime DLL not found: $_dll" >&2
    fi
done

# ─── Copy Qt6 DLLs (if Qt cross-build root is available) ──────────────────────
echo ""
echo "--- Copying Qt6 DLLs ---"
if [[ -n "$QT_ROOT" && -d "$QT_ROOT" ]]; then
    _QT6_DLLS=(
        "Qt6Core.dll"
        "Qt6Gui.dll"
        "Qt6Widgets.dll"
        "Qt6Network.dll"
        "Qt6OpenGL.dll"
        "Qt6Charts.dll"
        "Qt6Svg.dll"
    )
    for _dll in "${_QT6_DLLS[@]}"; do
        _path=$(find "$QT_ROOT" -name "$_dll" 2>/dev/null | head -1 || true)
        if [[ -n "$_path" ]]; then
            _cp "$_path" "$OUT_DIR/bin/"
        else
            echo "  (skipping $_ — not found in QT_ROOT)" >&2
        fi
    done

    # Qt platform plugin: platforms/qwindows.dll
    for _plug_name in qwindows.dll qoffscreen.dll; do
        _plug=$(find "$QT_ROOT" -name "$_plug_name" 2>/dev/null | head -1 || true)
        if [[ -n "$_plug" ]]; then
            _cp "$_plug" "$OUT_DIR/platforms/"
        fi
    done

    # Image format plugins
    for _fmt in qjpeg.dll qpng.dll qgif.dll qico.dll qsvg.dll; do
        _plug=$(find "$QT_ROOT" -name "$_fmt" 2>/dev/null | head -1 || true)
        [[ -n "$_plug" ]] && _cp "$_plug" "$OUT_DIR/imageformats/" || true
    done
else
    echo "  Qt6 root not set or does not exist — skipping Qt DLL copy."
    echo "  Set RETDEC_QT6_WIN_ROOT or pass --qt-root to include Qt6 DLLs."
fi

# ─── Copy CUDA runtime DLLs (optional, NVIDIA only) ───────────────────────────
echo ""
echo "--- Copying CUDA runtime ---"
# cudart64_*.dll ships with the NVIDIA CUDA Toolkit for Windows.
# If RETDEC_CUDA_WIN_ROOT points to the CUDA toolkit root, we copy cudart.
CUDA_WIN_ROOT="${RETDEC_CUDA_WIN_ROOT:-}"
if [[ -n "$CUDA_WIN_ROOT" && -d "$CUDA_WIN_ROOT" ]]; then
    _cudart=$(find "$CUDA_WIN_ROOT" -name 'cudart64_*.dll' 2>/dev/null | head -1 || true)
    if [[ -n "$_cudart" ]]; then
        _cp "$_cudart" "$OUT_DIR/bin/"
        echo "  Bundled: $( basename "$_cudart" )"
    else
        echo "  cudart64_*.dll not found under $CUDA_WIN_ROOT"
    fi
else
    echo "  RETDEC_CUDA_WIN_ROOT not set — cudart64.dll not bundled."
    echo "  The system CUDA runtime (from the NVIDIA driver / CUDA Toolkit) will be used at runtime."
fi

# ─── Copy RetDec support data ──────────────────────────────────────────────────
echo ""
echo "--- Copying RetDec support data ---"
if [[ -d "$BUILD_DIR/install/share/retdec" ]]; then
    _run cp -rv "$BUILD_DIR/install/share/retdec" "$OUT_DIR/share/"
elif [[ -d "$BUILD_DIR/share/retdec" ]]; then
    _run cp -rv "$BUILD_DIR/share/retdec" "$OUT_DIR/share/"
fi

# ─── DLL dependency check ─────────────────────────────────────────────────────
echo ""
echo "--- DLL dependency check (objdump) ---"
OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
if command -v "$OBJDUMP" >/dev/null 2>&1; then
    # Collect all bundled DLLs and exe names for reference
    _bundled_names=()
    while IFS= read -r -d '' _f; do
        _bundled_names+=("$(basename "$_f" | tr '[:upper:]' '[:lower:]')")
    done < <(find "$OUT_DIR/bin" -name '*.dll' -o -name '*.exe' -print0 2>/dev/null)

    _missing_any=0
    while IFS= read -r -d '' _exe; do
        echo "  Checking: $(basename "$_exe")"
        # Extract DLL imports
        _imports=$("$OBJDUMP" -p "$_exe" 2>/dev/null | grep 'DLL Name:' | awk '{print tolower($3)}' || true)
        while IFS= read -r _dep; do
            [[ -z "$_dep" ]] && continue
            # Ignore known system DLLs (always present on Windows)
            case "$_dep" in
                kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|\
                ole32.dll|oleaut32.dll|ntdll.dll|msvcrt.dll|ws2_32.dll|\
                comctl32.dll|comdlg32.dll|winspool.drv|winmm.dll|\
                setupapi.dll|cfgmgr32.dll|imm32.dll|opengl32.dll|uxtheme.dll|\
                version.dll|mpr.dll|shlwapi.dll|bcrypt.dll|crypt32.dll|\
                netapi32.dll|userenv.dll|psapi.dll)
                    continue ;;
            esac
            _found_in_bundle=0
            for _b in "${_bundled_names[@]}"; do
                [[ "$_b" == "$_dep" ]] && { _found_in_bundle=1; break; }
            done
            if [[ "$_found_in_bundle" -eq 0 ]]; then
                echo "    WARNING: dependency not bundled: $_dep" >&2
                _missing_any=1
            fi
        done <<< "$_imports"
    done < <(find "$OUT_DIR/bin" -name '*.exe' -print0 2>/dev/null)

    if [[ "$_missing_any" -eq 0 ]]; then
        echo "  All EXE dependencies are accounted for."
    fi
else
    echo "  $OBJDUMP not found — skipping DLL dependency check."
fi

# ─── ZIP bundle (optional) ────────────────────────────────────────────────────
if [[ "$DO_ZIP" -eq 1 ]]; then
    echo ""
    echo "--- Creating zip archive ---"
    _zip_path="${OUT_DIR}.zip"
    if command -v zip >/dev/null 2>&1; then
        _run zip -r "$_zip_path" "$OUT_DIR"
        echo "  Created: $_zip_path"
    else
        echo "  WARNING: zip not found — skipping archive creation." >&2
    fi
fi

echo ""
echo "=== Bundle complete: $OUT_DIR ==="
