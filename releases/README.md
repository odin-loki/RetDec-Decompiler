# RetDec release artifacts

Install helpers live in git under `releases/linux/`. Platform binaries
(`.zip`, `.exe`, `.tar.gz`) are built into `dist/` locally and published to
**[GitHub Releases](https://github.com/odin-loki/RetDec-Decompiler/releases)** only — they are not committed.

## Current version

See `VERSION` for the active package version and script paths.

| Platform | In git | On GitHub Releases |
|----------|--------|-------------------|
| Linux | `linux/install.sh`, `linux/uninstall.sh` | `retdec-*-linux-x64.tar.gz` + scripts |
| Windows | — (use `scripts/install-windows.ps1`) | `setup.exe`, portable `.zip` |

## CI and validation

- **ci-smoke**, **perf-nightly**, and **ctest-*** workflows are **manual-only** (Actions → Run workflow).
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
.\scripts\install-windows.ps1 -SetupExe dist\retdec-5.0-windows-x64-setup.exe
```

## Linux

**Build locally** (tarball in `dist/`, scripts synced to `releases/linux/`):

```bash
chmod +x scripts/build-all.sh scripts/build-linux-installer.sh
./scripts/build-all.sh
```

**Install from a release tarball:**

```bash
# Download retdec-5.0-linux-x64.tar.gz from GitHub Releases, then:
tar xzf retdec-5.0-linux-x64.tar.gz
cd retdec-5.0-linux-x64
./install.sh --user --add-path
```

Or copy `install.sh` / `uninstall.sh` from this repo and run them from inside an extracted tarball tree.

## GitHub Releases

CI workflow **`.github/workflows/release-installers.yml`** publishes both platforms when you:

1. **Tag a release:** `git tag v5.0 && git push origin v5.0`
2. **Manual dispatch:** Actions → *release-installers* → Run workflow

After changing install/uninstall scripts locally, commit updated `releases/linux/` files.
