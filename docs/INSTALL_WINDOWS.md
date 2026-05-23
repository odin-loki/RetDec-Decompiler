# Installing RetDec on Windows

This guide covers **end-user installation** after you (or CI) have built RetDec on Windows.
For compiling from source, see [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md).

Project home: [github.com/odin-loki/RetDec-Decompiler](https://github.com/odin-loki/RetDec-Decompiler)

---

## Quick paths

| Method | Admin? | PATH | Uninstaller |
|--------|--------|------|-------------|
| **NSIS setup.exe** | Yes | System PATH | Yes (Add/Remove Programs) |
| **Portable ZIP** | No | Manual | Delete folder |
| **Copy install script** | Optional | User PATH (`-AddToPath`) | Delete folder |

---

## 1. Build the installer artifacts (developers)

From **Developer PowerShell for VS 2022**, after a successful native configure/build:

```powershell
# Full pipeline: build targets, cmake --install, stage bundle, zip, NSIS (if makensis on PATH)
.\scripts\build-windows-installer.ps1

# Re-package an existing install tree (no rebuild)
.\scripts\build-windows-installer.ps1 -SkipBuild

# Custom version string for filenames / NSIS metadata
.\scripts\build-windows-installer.ps1 -SkipBuild -Version 5.0.0
```

### Outputs

| Artifact | Location |
|----------|----------|
| Staged bundle (NSIS input) | `dist\windows-bundle\` |
| Portable archive | `dist\retdec-<version>-windows-x64-portable.zip` |
| Graphical installer | `dist\retdec-<version>-windows-x64-setup.exe` (if NSIS installed) |

### Parameters (`build-windows-installer.ps1`)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-BuildDir` | `build\windows` | CMake build tree |
| `-InstallDir` | `install\windows` | `cmake --install` prefix |
| `-OutDir` | `dist` | ZIP and setup.exe output |
| `-BundleDir` | `dist\windows-bundle` | Staging layout for NSIS |
| `-SkipBuild` | off | Skip `cmake --build` / `--install` |
| `-Version` | from `CMakeLists.txt` | Package version (e.g. `5.0`) |
| `-QtRoot` | auto-detect | Qt kit root (e.g. `C:\Qt\6.11.0\msvc2022_64`) |

The bundle includes:

- `bin\*.exe` from the install tree (decompiler, GUI, fileinfo, utilities)
- `share\retdec\` support data
- Qt6 DLLs (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`, …) and `platforms\qwindows.dll`, `qoffscreen.dll`
- MSVC runtime DLLs when found under the Visual Studio redist directory

---

## 2. Install for end users

### Option A — NSIS installer (recommended)

```powershell
.\scripts\install-windows.ps1 -SetupExe dist\retdec-5.0-windows-x64-setup.exe
```

Or double-click `retdec-*-windows-x64-setup.exe`. The installer:

- Installs to `C:\Program Files\RetDec\` by default
- Adds `bin` to **system PATH** (requires the [EnVar NSIS plug-in](https://nsis.sourceforge.io/EnVar_plug-in) at build time)
- Creates Start Menu / desktop shortcuts for the GUI
- Registers an uninstall entry

### Option B — Portable ZIP

1. Extract `dist\retdec-<version>-windows-x64-portable.zip` to any folder (e.g. `%LOCALAPPDATA%\RetDec`).
2. Run `bin\retdec-decompiler.exe` or `bin\retdec-gui.exe` directly.
3. Optionally add `bin` to your user PATH:

```powershell
.\scripts\install-windows.ps1 -SourceDir dist\windows-bundle -Destination "$env:LOCALAPPDATA\RetDec" -AddToPath
```

### Option C — Copy from cmake install tree

```powershell
.\scripts\install-windows.ps1 -SourceDir install\windows -Destination "C:\Program Files\RetDec" -Force
```

Run PowerShell **as Administrator** when installing under `Program Files`.

---

## 3. NSIS prerequisites (for building setup.exe)

NSIS is **not** required for the portable ZIP. To produce `setup.exe`:

1. Install [NSIS 3.x](https://nsis.sourceforge.io/Download).
2. Install the [EnVar plug-in](https://nsis.sourceforge.io/EnVar_plug-in):
   - Copy `EnVar.dll` → `%ProgramFiles(x86)\NSIS\Plugins\x86-unicode\`
   - Copy `EnVar.nsh` → `%ProgramFiles(x86)\NSIS\Include\`
3. Ensure `makensis.exe` is on `PATH`.
4. Re-run `.\scripts\build-windows-installer.ps1`.

Manual compile:

```powershell
makensis /DVERSION=5.0 /DBUNDLE_DIR=..\..\dist\windows-bundle packaging\nsis\retdec.nsi
```

The NSIS script lives at `packaging\nsis\retdec.nsi`. An optional **“.exe file association”** section is included but commented out; uncomment `SEC_ASSOC` in the `.nsi` file to add “Open with RetDec GUI” for `.exe` files.

---

## 4. Verify installation

```powershell
& "C:\Program Files\RetDec\bin\retdec-decompiler.exe" --help
& "C:\Program Files\RetDec\bin\retdec-gui.exe" --version
```

Or from the repo after staging:

```powershell
.\scripts\install_smoke.ps1 -InstallDir install\windows
.\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows-bundle\bin
```

---

## 5. Uninstall

- **NSIS install:** Settings → Apps → RetDec → Uninstall, or run `C:\Program Files\RetDec\uninstall.exe`.
- **Portable / copy install:** Delete the install folder and remove its `bin` entry from PATH if you added one.

---

## Related scripts and docs

| File | Purpose |
|------|---------|
| `scripts\build-windows-installer.ps1` | Build, stage, zip, NSIS |
| `scripts\install-windows.ps1` | User-facing install helper |
| `scripts\windows_cmake_install.ps1` | `cmake --install` only |
| `scripts\windows_native_build.ps1` | Full native build + `dist\windows` staging |
| `scripts\bundle-windows.sh` | Linux/WSL cross-compile bundle (same NSIS layout) |
| `packaging\nsis\retdec.nsi` | NSIS installer definition |
| [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) | Build from source (MSVC + CUDA + Qt6) |
| [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md) | CLI-only cross-compile from WSL |
