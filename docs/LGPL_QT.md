# Qt 6 LGPL compliance evidence

Plan.md `LEG-12`. This file is the evidence that the GUI links Qt dynamically
and that a recipient can relink against a modified Qt.

## Dynamic linking

`src/gui/CMakeLists.txt` links `retdec-gui` and `retdec-gui-panels` to
`Qt6::Core`, `Qt6::Gui`, and `Qt6::Widgets` (optional `Qt6::Svg`). Those
imported targets are the Qt 6 **shared** libraries from the official Qt
distribution (`find_package(Qt6 …)`). The GUI is not built against a
static Qt archive in this tree.

Windows deploy copies the shared libraries next to the executable:

- POST_BUILD `windeployqt` on `retdec-gui` (`src/gui/CMakeLists.txt`)
- install script `src/gui/install-windeployqt-gui.cmake.in`
- `scripts/windows_native_build.ps1` stages Qt DLLs into `dist/windows/`

`platforms/qwindows.dll` / `qoffscreen.dll` sit beside the exe, which is
the shared-plugin layout, not a statically linked Qt.

Linux/macOS: the binary depends on distro or Qt-installer `.so` / `.dylib`
files (`macdeployqt` when building a bundle).

## Relink (LGPL §4)

A recipient who wants a modified Qt can:

1. Replace the shipped Qt shared libraries / plugins with their build
   (same SONAME / DLL names), or
2. Rebuild `retdec-gui` from this source against their Qt prefix
   (`Qt6_DIR` / `CMAKE_PREFIX_PATH`) using
   [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) or
   [BUILD_REFERENCE.md](BUILD_REFERENCE.md).

Object files for the GUI are produced by a normal CMake build; we do not
ship a static Qt. Relink is “rebuild the GUI target against replacement
Qt shared libraries.”

## What this does not claim

- Qt source is not vendored in `deps/`.
- A commercial installer must still include Qt LGPL notices and the
  corresponding Qt licence texts from the Qt installation used to build.
- `NOTICE` lists Qt 6 as LGPL/GPL/commercial.

## Residual

Confirm on a Release installer that `retdec-gui.exe` is not linked to a
static Qt (`dumpbin /dependents` should list `Qt6Core.dll` etc.). That
check is not yet a CI job.
