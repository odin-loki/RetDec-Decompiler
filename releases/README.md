# RetDec release artifacts

Published installers and helper scripts tracked in git (large binaries via Git LFS).

## Windows

| File | Description |
|------|-------------|
| `windows/retdec-*-windows-x64-setup.exe` | NSIS graphical installer (PATH, shortcuts, uninstaller) |
| `windows/retdec-*-windows-x64-portable.zip` | Portable bundle — extract anywhere |

Build or refresh:

```powershell
.\scripts\build-windows-installer.ps1 -SkipBuild
```

Install:

```powershell
.\scripts\install-windows.ps1 -SetupExe releases\windows\retdec-5.0-windows-x64-setup.exe
```

## Linux

| File | Description |
|------|-------------|
| `linux/install.sh` | Copy tarball contents to `/opt/retdec` or `~/.local/retdec` |
| `linux/uninstall.sh` | Remove install and optional PATH snippet |
| `linux/retdec-*-linux-x64.tar.gz` | Full portable package (when built on Linux) |

Build on Linux:

```bash
./scripts/install-linux.sh --build --build
```

After a Linux build, `scripts/build-linux-installer.sh` copies fresh scripts and the tarball here.

## Version manifest

See `VERSION` for the current published package version and artifact paths.
