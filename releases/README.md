# RetDec release artifacts

Published installers are kept in git (`releases/`, large binaries via **Git LFS**)
and attached to **[GitHub Releases](https://github.com/odin-loki/RetDec-Decompiler/releases)**.

## Current version

See `VERSION` for paths and the active package version.

| Platform | Artifact | Path |
|----------|----------|------|
| Windows | NSIS installer | `windows/retdec-5.0-windows-x64-setup.exe` |
| Windows | Portable zip | `windows/retdec-5.0-windows-x64-portable.zip` |
| Linux | Portable tarball | `linux/retdec-5.0-linux-x64.tar.gz` |
| Linux | Install helper | `linux/install.sh` |
| Linux | Uninstall helper | `linux/uninstall.sh` |

## CI and validation

- **ci-smoke**, **perf-nightly**, and **ctest-*** workflows are **manual-only** (Actions → Run workflow) so push status stays clean.
- **release-installers** publishes Windows/Linux packages on version tags or manual dispatch.
- Run **`scripts/doctor.ps1`** or **`bash scripts/doctor.sh`** locally before a full build.

## Windows

| File | Description |
|------|-------------|
| `windows/retdec-*-windows-x64-setup.exe` | NSIS graphical installer (PATH, shortcuts, uninstaller) |
| `windows/retdec-*-windows-x64-portable.zip` | Portable bundle — extract anywhere |

**Build locally:**

```powershell
.\scripts\build-all.ps1
# or after an existing install tree:
.\scripts\build-windows-installer.ps1 -SkipBuild
```

**Install:**

```powershell
.\scripts\install-windows.ps1 -SetupExe releases\windows\retdec-5.0-windows-x64-setup.exe
```

## Linux

| File | Description |
|------|-------------|
| `linux/retdec-*-linux-x64.tar.gz` | Portable package with `bin/`, `lib/`, `share/` |
| `linux/install.sh` | Copy tarball contents to `/opt/retdec` or `~/.local/retdec` |
| `linux/uninstall.sh` | Remove install and optional PATH snippet |

Linux **`.tar.gz`** installers are produced by the **release-installers** workflow (tag or manual dispatch) and by `scripts/build-linux-installer.sh` locally. An **AppImage** is optional — build with `scripts/make-appimage.sh` after `cmake --install`; it is not uploaded by default in CI.

**Build locally:**

```bash
chmod +x scripts/build-all.sh scripts/build-linux-installer.sh
./scripts/build-all.sh
```

**Install from tarball:**

```bash
tar xzf releases/linux/retdec-5.0-linux-x64.tar.gz
cd retdec-5.0-linux-x64
./install.sh --user --add-path
```

## GitHub Releases

CI workflow **`.github/workflows/release-installers.yml`** publishes both platforms when you:

1. **Tag a release:** `git tag v5.0 && git push origin v5.0`
2. **Manual dispatch:** Actions → *release-installers* → Run workflow
   - Set `version` (e.g. `5.0`)
   - Enable `skip_build` to upload existing `releases/` artifacts without rebuilding

Release assets:

- Windows: `setup.exe`, portable `.zip`
- Linux: `.tar.gz`, `install.sh`, `uninstall.sh`

After a local build, commit updated `releases/` files so `main` always carries the latest installers.
