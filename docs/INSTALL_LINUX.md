# Linux Install & Packaging

This guide covers **building**, **packaging**, and **installing** RetDec on
Linux (native or WSL). Version **2.0.22**. For the ten-minute download path see
[QUICKSTART.md](../QUICKSTART.md). For the full preset matrix see
[BUILD_REFERENCE.md](BUILD_REFERENCE.md). macOS packaging is
[releases/README.md](../releases/README.md) and `scripts/build-macos-installer.sh`.

**Related docs:** [scripts/README.md](../scripts/README.md) ·
[WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) (Windows counterpart)

---

## Prerequisites

Build the project first with CMake presets (Release recommended for distribution):

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build python3 perl \
  qt6-base-dev qt6-base-dev-tools libgl1-mesa-dev libxkbcommon-dev

# Presets need CMake 3.26+ (CMakePresets.json). Root CMakeLists.txt still
# declares cmake_minimum_required(VERSION 3.13). Ubuntu's cmake package may
# be older — use Kitware's kit or pip if `cmake --version` is below 3.26.

cmake --preset full-linux-release
cmake --build build/linux --parallel
```

Optional packaging tools:

| Tool | Purpose | Install |
|------|---------|---------|
| **tar** | Tarball (usually preinstalled) | — |
| **fpm** | `.deb` packages | `gem install --no-document fpm` |
| **FUSE / APPIMAGE_EXTRACT_AND_RUN** | AppImage | see [make-appimage.sh](../scripts/make-appimage.sh) |

---

## Script overview

| Script | Role |
|--------|------|
| [build-linux-installer.sh](../scripts/build-linux-installer.sh) | `cmake --install`, stage `dist/retdec-*-linux-x64/`, tarball; copies `install.sh` / `uninstall.sh` to `releases/linux/` |
| [build-all.sh](../scripts/build-all.sh) | Configure + build + package (one command) |
| [install-linux.sh](../scripts/install-linux.sh) | User wrapper: build+package or install from tarball |
| [make-appimage.sh](../scripts/make-appimage.sh) | Portable AppImage (only when `APPIMAGE=1`; often off in CI) |

Default paths (from [CMakePresets.json](../CMakePresets.json)):

| Path | Contents |
|------|----------|
| `build/linux/` | CMake binary directory |
| `install/linux/` | `cmake --install` prefix |
| `dist/` | Tarball, staging tree, optional AppImage / `.deb` |

---

## Executable bit (important)

Git on Windows does not always preserve the Unix executable bit. After checkout
on Linux or WSL, mark the entry scripts executable **once**:

```bash
chmod +x scripts/build-linux-installer.sh \
         scripts/install-linux.sh \
         scripts/make-appimage.sh
```

Tarballs produced by `build-linux-installer.sh` include `install.sh` and
`uninstall.sh` with `+x` set at pack time. If you copy a staging tree by hand,
run `chmod +x install.sh uninstall.sh` before executing them.

You can also invoke any script explicitly with Bash (no `+x` required):

```bash
bash scripts/build-linux-installer.sh
bash scripts/install-linux.sh --build
```

---

## Build & package (maintainers)

### Standard tarball

```bash
# After cmake --build build/linux
./scripts/build-linux-installer.sh
```

This runs `cmake --install build/linux --prefix install/linux`, then writes:

```
dist/retdec-<version>-linux-x64/
  bin/
  lib/              # lib64/ included when present
  share/retdec/
  install.sh
  uninstall.sh
  README
dist/retdec-<version>-linux-x64.tar.gz
```

### One-shot build + package

```bash
./scripts/build-all.sh
# or
./scripts/build-linux-installer.sh --build
# or
./scripts/install-linux.sh --build --build
```

Only `install.sh` and `uninstall.sh` are copied to `releases/linux/` (committed helpers). The tarball stays in `dist/` and is published by CI to GitHub Releases — it is not a Git LFS artefact.

### With AppImage

```bash
APPIMAGE=1 ./scripts/build-linux-installer.sh
# → dist/retdec-<version>-x86_64.AppImage
```

### With Debian package (optional)

When `fpm` is on `PATH`, a `.deb` is produced automatically. To require it:

```bash
FPM=1 ./scripts/build-linux-installer.sh
# → dist/retdec_<version>_amd64.deb  (files under /opt/retdec)
```

If `fpm` is missing, the builder prints install instructions and continues
(tarball still succeeds). Install fpm with:

```bash
sudo apt-get install -y ruby ruby-dev build-essential
gem install --no-document fpm
```

### Useful options

| Option | Description |
|--------|-------------|
| `--build-dir DIR` | CMake binary dir (default: `build/linux`) |
| `--install-dir DIR` | Install prefix (default: `install/linux`) |
| `--dist-dir DIR` | Output directory (default: `dist`) |
| `--version VER` | Override version string in filenames |
| `--preset PRESET` | Preset for `--build` (default: `full-linux-release`) |
| `--skip-install` | Re-stage an existing `install/linux` tree |
| `--dry-run` | Print commands only |

---

## End-user install from tarball

### Interactive (recommended)

```bash
tar xzf retdec-2.0.22-linux-x64.tar.gz
cd retdec-2.0.22-linux-x64
./install.sh
```

The installer asks for:

1. **Location** — `/opt/retdec` (sudo) or `$HOME/.local/retdec`
2. **PATH** — optional `~/.bashrc` snippet for `<prefix>/bin`

### Non-interactive examples

```bash
# User-local + PATH
./install.sh --user --add-path

# System-wide
sudo ./install.sh --prefix /opt/retdec --add-path

# Defaults only (no prompts, no PATH change)
./install.sh --yes --prefix /opt/retdec
```

### Wrapper script

```bash
./scripts/install-linux.sh --user --add-path dist/retdec-2.0.22-linux-x64.tar.gz
```

### Run without installing

```bash
tar xzf retdec-2.0.22-linux-x64.tar.gz
cd retdec-2.0.22-linux-x64
export PATH="$(pwd)/bin:$PATH"
retdec-decompiler --help
retdec-gui   # needs display / Qt platform plugin
```

### Smoke test (`fib_smoke`) and signatures

After `install.sh --add-path` (or exporting `PATH` to `<prefix>/bin`):

```bash
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.22/fib_smoke
retdec-decompiler fib_smoke -o fib.c
```

`fib_smoke` is gcc -O1 of `tests/test_binaries/fib.c` (same flags as the
`retdec-decompiler-fixture-fib` CMake target). The Release also has
`retdec-2.0.22-linux-x64.tar.gz.sigstore.json` and `fib_smoke.sigstore.json`
(keyless Sigstore). There is no Authenticode on Linux.

---

## Uninstall

From the extracted tarball directory (or any copy of `uninstall.sh`):

```bash
./uninstall.sh
# or
./uninstall.sh --prefix /opt/retdec
```

This removes the install tree and deletes matching `PATH` lines from
`~/.bashrc` (creates `~/.bashrc.bak` when using GNU sed).

---

## CI and GitHub Releases

- **Integration tests:** [`.github/workflows/ctest-linux.yml`](../.github/workflows/ctest-linux.yml)
  runs on **push and pull request to `main`** (and `workflow_dispatch`). It fetches
  large support files, pins CMake **3.31.6**, installs Qt6 and PyYAML, and runs
  headless GUI (`QT_QPA_PLATFORM=offscreen`) plus labelled ctest (CPU-only;
  `RETDEC_ENABLE_CUDA_ACCEL=OFF`).
- **Release packaging:** [`.github/workflows/release-installers.yml`](../.github/workflows/release-installers.yml)
  `linux-installer` job (`needs: release`) builds `dist/retdec-<ver>-linux-x64.tar.gz`
  and publishes it to
  [GitHub Releases](https://github.com/odin-loki/RetDec-Decompiler/releases).
  AppImage is optional (`appimage` dispatch input / `APPIMAGE` env, default off).
  Cosign writes `*.sigstore.json` next to the tarball.
- Tag: `git tag v2.0.22 && git push origin v2.0.22`. `skip_build` defaults to
  **false**; do not document uploading a local `dist/` as the release path.

Local one-shot packaging:

```bash
./scripts/build-all.sh
```

If you edited the generated install helpers, commit `releases/linux/install.sh`
and `uninstall.sh` (scripts only). Do not commit the tarball.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `Permission denied` running scripts | `chmod +x scripts/*.sh` or use `bash scripts/...` |
| `install/linux/bin` missing | Run `cmake --build build/linux` then re-run the installer script |
| GUI fails on headless SSH | `export QT_QPA_PLATFORM=offscreen` or use CLI tools only |
| AppImage FUSE error | `export APPIMAGE_EXTRACT_AND_RUN=1` |
| `.deb` not created | Install `fpm` or ignore (tarball is the primary artifact) |
| Shared libs not found after install | Use `install.sh --add-path` or set `LD_LIBRARY_PATH` to `<prefix>/lib` (RPATH should cover normal use) |
