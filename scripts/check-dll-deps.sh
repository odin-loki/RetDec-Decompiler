#!/usr/bin/env bash
# check-dll-deps.sh — Verify all DLL dependencies of Windows PE binaries are
# either bundled in the target directory or are known Windows system DLLs.
#
# Usage:
#   ./scripts/check-dll-deps.sh <dir-or-exe> [<dir-or-exe> ...]
#
# Exit code: 0 if all deps resolved, 1 if any unbundled non-system deps found.
#
# Example:
#   ./scripts/check-dll-deps.sh build/linux/mingw-w64-release/src/gui/retdec-gui.exe
#   ./scripts/check-dll-deps.sh bundle/
#
# Requires:
#   x86_64-w64-mingw32-objdump  (binutils-mingw-w64-x86-64)

set -euo pipefail

OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"

if ! command -v "$OBJDUMP" >/dev/null 2>&1; then
    echo "ERROR: $OBJDUMP not found. Install binutils-mingw-w64-x86-64." >&2
    exit 1
fi

if [[ $# -eq 0 ]]; then
    echo "Usage: $0 <dir-or-exe> [...]" >&2
    exit 1
fi

# ─── Known Windows system DLLs (always present on Windows, never bundle) ─────
declare -A _SYSTEM_DLLS=(
    [kernel32.dll]=1 [user32.dll]=1 [gdi32.dll]=1 [advapi32.dll]=1
    [shell32.dll]=1 [ole32.dll]=1 [oleaut32.dll]=1 [ntdll.dll]=1
    [msvcrt.dll]=1 [ws2_32.dll]=1 [comctl32.dll]=1 [comdlg32.dll]=1
    [winspool.drv]=1 [winmm.dll]=1 [setupapi.dll]=1 [cfgmgr32.dll]=1
    [imm32.dll]=1 [opengl32.dll]=1 [uxtheme.dll]=1 [version.dll]=1
    [mpr.dll]=1 [shlwapi.dll]=1 [bcrypt.dll]=1 [crypt32.dll]=1
    [netapi32.dll]=1 [userenv.dll]=1 [psapi.dll]=1 [dwmapi.dll]=1
    [d3d9.dll]=1 [d3d11.dll]=1 [dxgi.dll]=1 [dbghelp.dll]=1
    [rpcrt4.dll]=1 [secur32.dll]=1 [wldap32.dll]=1 [iphlpapi.dll]=1
    [mswsock.dll]=1 [wininet.dll]=1 [msimg32.dll]=1 [dnsapi.dll]=1
    [nsi.dll]=1 [msvcr140.dll]=1 [vcruntime140.dll]=1 [ucrtbase.dll]=1
    [api-ms-win-crt-runtime-l1-1-0.dll]=1
    [api-ms-win-crt-math-l1-1-0.dll]=1
    [api-ms-win-crt-stdio-l1-1-0.dll]=1
    [api-ms-win-crt-locale-l1-1-0.dll]=1
    [api-ms-win-crt-heap-l1-1-0.dll]=1
)

# ─── Collect all target files ─────────────────────────────────────────────────
_TARGET_FILES=()
for _arg in "$@"; do
    if [[ -f "$_arg" ]]; then
        _TARGET_FILES+=("$_arg")
    elif [[ -d "$_arg" ]]; then
        while IFS= read -r -d '' _f; do
            _TARGET_FILES+=("$_f")
        done < <(find "$_arg" \( -name '*.exe' -o -name '*.dll' \) -print0 2>/dev/null)
    else
        echo "WARNING: not a file or directory: $_arg" >&2
    fi
done

if [[ ${#_TARGET_FILES[@]} -eq 0 ]]; then
    echo "No PE files found in provided paths." >&2
    exit 0
fi

# ─── Build set of bundled DLL names ───────────────────────────────────────────
declare -A _BUNDLED
for _f in "${_TARGET_FILES[@]}"; do
    _name="$(basename "$_f" | tr '[:upper:]' '[:lower:]')"
    _BUNDLED[$_name]=1
done

# ─── Check each PE file ───────────────────────────────────────────────────────
_total_missing=0
for _pe in "${_TARGET_FILES[@]}"; do
    # Quick magic check — skip non-PE files
    _magic=$(xxd -l 2 "$_pe" 2>/dev/null | awk '{print $2$3}' || true)
    [[ "$_magic" != "4d5a" ]] && continue

    _imports=$("$OBJDUMP" -p "$_pe" 2>/dev/null \
        | grep 'DLL Name:' \
        | awk '{print tolower($3)}' \
        || true)

    _file_missing=0
    while IFS= read -r _dep; do
        [[ -z "$_dep" ]] && continue
        [[ -v _SYSTEM_DLLS[$_dep] ]] && continue
        [[ -v _BUNDLED[$_dep]     ]] && continue
        if [[ "$_file_missing" -eq 0 ]]; then
            echo "MISSING deps in $(basename "$_pe"):"
            _file_missing=1
        fi
        echo "  → $_dep"
        _total_missing=$((_total_missing + 1))
    done <<< "$_imports"
done

# ─── Summary ──────────────────────────────────────────────────────────────────
echo ""
if [[ "$_total_missing" -eq 0 ]]; then
    echo "✓ All DLL dependencies resolved (${#_TARGET_FILES[@]} PE files checked)."
    exit 0
else
    echo "✗ $_total_missing unresolved DLL dependencies found."
    exit 1
fi
