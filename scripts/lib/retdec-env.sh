#!/usr/bin/env bash
# Source from other shell scripts:  source "$(dirname "$0")/lib/retdec-env.sh"
# Or:  source "${RETDEC_ROOT}/scripts/lib/retdec-env.sh"
#
# Canonical layout (CMakePresets base + superbuild):
#   <repo>/build/linux/   — non-Windows hosts (WSL, Linux, macOS)
#   <repo>/build/windows/ — Windows native MSVC
#   <repo>/install/linux/ , install/windows/ — matching install prefixes
#   MinGW cross (WSL): build/linux/mingw-w64-release , install/linux/mingw-w64-release

if [[ -z "${RETDEC_ROOT:-}" ]]; then
	_RETDEC_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	RETDEC_ROOT="$(cd "${_RETDEC_LIB_DIR}/../.." && pwd)"
	export RETDEC_ROOT
fi

# Default preset for helper scripts (override before sourcing).
export RETDEC_CMAKE_PRESET_LINUX_DEBUG="${RETDEC_CMAKE_PRESET_LINUX_DEBUG:-full-linux-debug}"
export RETDEC_CMAKE_PRESET_LINUX_REL="${RETDEC_CMAKE_PRESET_LINUX_REL:-full-linux-release}"
export RETDEC_CMAKE_PRESET_WINDOWS_REL="${RETDEC_CMAKE_PRESET_WINDOWS_REL:-full-windows-release}"

export RETDEC_BUILD_DEBUG="${RETDEC_ROOT}/build/linux"
export RETDEC_BUILD_RELEASE="${RETDEC_ROOT}/build/linux"
export RETDEC_BUILD_MINGW="${RETDEC_ROOT}/build/linux/mingw-w64-release"
export RETDEC_INSTALL_MINGW="${RETDEC_ROOT}/install/linux/mingw-w64-release"
