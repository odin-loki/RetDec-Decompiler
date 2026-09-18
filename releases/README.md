# RetDec release artifacts

Install helpers live in git under `releases/linux/` and `releases/macos/`. Platform binaries
(`.zip`, `.exe`, `.tar.gz`) are built into `dist/` locally and published to
**[GitHub Releases](https://github.com/odin-loki/RetDec-Decompiler/releases)** only — they are not committed.

## Current version

See `VERSION` for the active package version and script paths.

| Platform | In git | On GitHub Releases |
|----------|--------|-------------------|
| Linux | `linux/install.sh`, `linux/uninstall.sh` | `retdec-*-linux-x64.tar.gz` + scripts; `retdec-*-x86_64.AppImage`; `fib_smoke` sample ELF |
| Windows | — (use `scripts/install-windows.ps1`) | `retdec-*-windows-x64-setup.exe`, `retdec-*-windows-x64-portable.zip`; `fib_smoke.exe` sample PE |
| macOS | `macos/install.sh`, `macos/uninstall.sh` | `retdec-*-macos-arm64.tar.gz` + scripts |

## CI and validation

- **ci-smoke**, **ctest-linux** and **ctest-macos** run on push and pull request to `main`; **ctest-windows** is nightly and `workflow_dispatch`.
- **perf-nightly** is schedule / manual.
- **release-installers** builds into `dist/` and uploads to GitHub Releases on version tags or manual dispatch.
- Run **`scripts/doctor.ps1`** or **`bash scripts/doctor.sh`** locally before a full build.

## Windows

**Build locally** (outputs under `dist/`):

```powershell
.\scripts\build-all.ps1
# or after an existing install tree:
.\scripts\build-windows-installer.ps1 -SkipBuild
```

**Install:**

```powershell
.\scripts\install-windows.ps1 -SetupExe dist\retdec-2.0.21-windows-x64-setup.exe
```

## Linux

**Build locally** (tarball in `dist/`, scripts synced to `releases/linux/`):

```bash
chmod +x scripts/build-all.sh scripts/build-linux-installer.sh
./scripts/build-all.sh
```

**Install from a release tarball:**

```bash
# Download retdec-2.0.21-linux-x64.tar.gz from GitHub Releases, then:
tar xzf retdec-2.0.21-linux-x64.tar.gz
cd retdec-2.0.21-linux-x64
./install.sh --user --add-path
```

Or copy `install.sh` / `uninstall.sh` from this repo and run them from inside an extracted tarball tree.

## macOS

**Build locally** (tarball in `dist/`, scripts synced to `releases/macos/`):

```bash
brew install ninja qt@6 pkg-config autoconf automake libtool
cmake --preset full-linux-release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
cmake --build build/linux --target install
./scripts/build-macos-installer.sh --skip-install --install-dir install/linux
```

**Install from a release tarball:**

```bash
# Download retdec-<version>-macos-arm64.tar.gz from GitHub Releases, then:
tar xzf retdec-<version>-macos-arm64.tar.gz
cd retdec-<version>-macos-arm64
./install.sh --user --add-path
```

The binaries are signed ad-hoc rather than with an Apple Developer ID, so
macOS quarantines them when a browser did the downloading. `install.sh`
removes `com.apple.quarantine` from what it installs; by hand that is
`xattr -dr com.apple.quarantine <extracted directory>`.

## GitHub Releases

CI workflow **`.github/workflows/release-installers.yml`** publishes all three platforms when you:

1. **Tag a release:** `git tag v2.0.22 && git push origin v2.0.22`
2. **Manual dispatch:** Actions → *release-installers* → Run workflow

After changing install/uninstall scripts locally, commit the updated `releases/linux/` and `releases/macos/` files.
