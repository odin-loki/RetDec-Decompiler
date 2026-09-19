# RetDec release artifacts

Install helpers live in git under `releases/linux/` and `releases/macos/`. Platform binaries
(`.zip`, `.exe`, `.tar.gz`) are built into `dist/` by CI or a local installer script and published to
**[GitHub Releases](https://github.com/odin-loki/RetDec-Decompiler/releases)** only — they are not committed.

## Current version

See `VERSION` for the active package version and script paths. CMake `project(... VERSION 2.0.24)`.

| Platform | In git | On GitHub Releases |
|----------|--------|-------------------|
| Linux | `linux/install.sh`, `linux/uninstall.sh` | `retdec-*-linux-x64.tar.gz` + those scripts as `install.sh` / `uninstall.sh`; optional `retdec-*-x86_64.AppImage`; `fib_smoke` sample ELF + `*.sigstore.json` |
| Windows | — (use `scripts/install-windows.ps1`) | `retdec-*-windows-x64-setup.exe`, `retdec-*-windows-x64-portable.zip`; `fib_smoke.exe`; `*.sigstore.json`. No Authenticode. |
| macOS | `macos/install.sh`, `macos/uninstall.sh` | `retdec-*-macos-<arch>.tar.gz` (contains `bin/`, `lib/`, `share/`, `RetDec.app`, `install.sh`, `uninstall.sh`). Loose assets are `install-macos.sh` / `uninstall-macos.sh` so they do not overwrite Linux `install.sh`. |

## CI and validation

- **ci-smoke** runs on every push and pull request (all branches).
- **ctest-linux** and **ctest-macos** run on push and pull request to `main` (plus `workflow_dispatch`).
- **ctest-windows** is nightly (`0 4 * * *`) and `workflow_dispatch` only — not on push.
- **macos-package** is nightly / manual (installer rehearsal, no publish).
- **perf-nightly** is schedule / manual.
- **release-installers** builds into `dist/` and uploads to GitHub Releases on `v*` tags or manual dispatch. `skip_build` defaults to **false**; do not treat uploading a local `dist/` as the release path.
- **windows-installer** in that workflow resolves version locally (tag / dispatch input / `CMakeLists.txt`) and does **not** `needs: release`, so a queued Ubuntu runner cannot block the Windows zip. Linux and macOS jobs still `needs: release`.
- Run **`scripts/doctor.ps1`** or **`bash scripts/doctor.sh`** locally before a full build.

## Windows

NSIS `setup.exe` + portable zip. CI: `windows-latest`, `RETDEC_TESTS=OFF`, `CMAKE_BUILD_PARALLEL_LEVEL=1`, EnVar NSIS plugin, sigstore cosign.

**Build locally** (outputs under `dist/`):

```powershell
.\scripts\build-all.ps1
# or after an existing install tree:
.\scripts\build-windows-installer.ps1 -SkipBuild
```

**Install** (then put `bin` on PATH if you used the zip):

```powershell
.\scripts\install-windows.ps1 -SetupExe dist\retdec-2.0.24-windows-x64-setup.exe
curl.exe -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.24/fib_smoke.exe
retdec-decompiler fib_smoke.exe -o fib.c
```

## Linux

Tarball is the primary artefact. AppImage is optional (`APPIMAGE=1` locally, or the `appimage` dispatch input / `APPIMAGE` env on `release-installers`; often off).

**Build locally** (tarball in `dist/`, `install.sh` / `uninstall.sh` synced to `releases/linux/`):

```bash
chmod +x scripts/build-all.sh scripts/build-linux-installer.sh
./scripts/build-all.sh
```

**Install from a release tarball:**

```bash
# Download retdec-2.0.24-linux-x64.tar.gz from GitHub Releases, then:
tar xzf retdec-2.0.24-linux-x64.tar.gz
cd retdec-2.0.24-linux-x64
./install.sh --user --add-path
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.24/fib_smoke
retdec-decompiler fib_smoke -o fib.c
```

Or copy `install.sh` / `uninstall.sh` from this repo and run them from inside an extracted tarball tree.

## macOS

`macos-installer` produces `retdec-<ver>-macos-<arch>.tar.gz`. MAC-01 (`scripts/ci/check_macos_bundle.py`) checks that rpaths stay inside `RetDec.app`. The package is **ad-hoc signed, not notarised**.

**Build locally** (tarball in `dist/`, scripts synced to `releases/macos/`). Unix CMake presets install to `install/linux/`; pass that path through:

```bash
brew install ninja qt@6 pkg-config autoconf automake libtool
cmake --preset full-linux-release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
cmake --build build/linux --target install
./scripts/build-macos-installer.sh --skip-install --install-dir install/linux
```

**Install from a release tarball** (the archive’s own `install.sh`, not the loose GitHub asset named `install-macos.sh`):

```bash
# Download retdec-2.0.24-macos-arm64.tar.gz from GitHub Releases, then:
tar xzf retdec-2.0.24-macos-arm64.tar.gz
cd retdec-2.0.24-macos-arm64
./install.sh --user --add-path
```

`macos-latest` currently builds **arm64**. Intel Macs need a `macos-x86_64` tarball when CI produces one.

The binaries are signed ad-hoc rather than with an Apple Developer ID, so
macOS quarantines them when a browser did the downloading. `install.sh`
removes `com.apple.quarantine` from what it installs; by hand that is
`xattr -dr com.apple.quarantine <extracted directory>`.

## GitHub Releases

CI workflow **`.github/workflows/release-installers.yml`** publishes all three platforms when you:

1. **Tag a release:** `git tag v2.0.24 && git push origin v2.0.24`
2. **Manual dispatch:** Actions → *release-installers* → Run workflow

After changing install/uninstall scripts locally, commit the updated `releases/linux/` and `releases/macos/` files.
