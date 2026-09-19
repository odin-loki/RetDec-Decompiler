# Quick start (ten minutes)

Plan.md `REL-07`. Prebuilt binaries are on the
[v2.0.24 GitHub Release](https://github.com/odin-loki/RetDec-Decompiler/releases/tag/v2.0.24).
`docker pull imortek/retdec` is still unpublished (`REL-02`).

Buildable C (`.h`, `_stubs.c`, `.buildable.c`) is **on by default**.
Pass `--no-buildable` to skip sidecars.

## Linux tarball (GitHub Release)

```bash
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.24/retdec-2.0.24-linux-x64.tar.gz
tar xzf retdec-2.0.24-linux-x64.tar.gz
cd retdec-2.0.24-linux-x64
chmod +x install.sh uninstall.sh
./install.sh --user --add-path
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.24/fib_smoke
retdec-decompiler fib_smoke -o fib.c
```

`fib_smoke` is gcc -O1 of `tests/test_binaries/fib.c` (same flags as
`retdec-decompiler-fixture-fib`). Read `fib.c` and `fib.buildable.c`.
Run without installing: `export PATH="$(pwd)/bin:$PATH"`. The same Release has
`retdec-2.0.24-linux-x64.tar.gz.sigstore.json` and `fib_smoke.sigstore.json`.

## Linux AppImage (optional — often not on the tag)

`release-installers.yml` sets job env `APPIMAGE` to `0` unless you dispatch
with input `appimage: true` or set the repository variable `APPIMAGE`.
Tag releases therefore **often do not** include an AppImage. If the asset
is on the Release:

```bash
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.24/retdec-2.0.24-x86_64.AppImage
chmod +x retdec-2.0.24-x86_64.AppImage
APPIMAGE_EXTRACT_AND_RUN=1 ./retdec-2.0.24-x86_64.AppImage --help
```

The AppImage launches `retdec-gui`. `APPIMAGE_EXTRACT_AND_RUN=1` avoids FUSE.
When CI builds it, a keyless bundle
`retdec-2.0.24-x86_64.AppImage.sigstore.json` is uploaded too.
Otherwise use the Linux tarball, or dispatch
[appimage-from-release.yml](.github/workflows/appimage-from-release.yml)
to wrap an already-published tarball.

## Windows (NSIS and zip)

Intended names from `release-installers.yml` `windows-installer`
(that job resolves the version locally from the tag, dispatch input, or
`CMakeLists.txt` `VERSION`; it does **not** wait on the Ubuntu `release`
job):

- `retdec-2.0.24-windows-x64-setup.exe` — NSIS installer
- `retdec-2.0.24-windows-x64-portable.zip` — portable tree

Those are the artifacts a completed Windows job uploads, with matching
`.sigstore.json` bundles. Authenticode is not applied. A given run may
still be building; check the Release assets before assuming both files
are present.

After NSIS or unzipping the portable tree and putting `bin` on `PATH`:

```powershell
curl.exe -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.24/fib_smoke.exe
retdec-decompiler fib_smoke.exe -o fib.c
```

`fib_smoke.exe` is the MSVC fixture (same flags as
`retdec-decompiler-fixture-fib`). Keyless Sigstore bundles
`retdec-2.0.24-windows-x64-portable.zip.sigstore.json`,
`retdec-2.0.24-windows-x64-setup.exe.sigstore.json`, and
`fib_smoke.exe.sigstore.json` are uploaded when those artifacts exist.

## macOS tarball

```bash
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.24/retdec-2.0.24-macos-arm64.tar.gz
tar xzf retdec-2.0.24-macos-arm64.tar.gz
cd retdec-2.0.24-macos-arm64
chmod +x install.sh uninstall.sh
./install.sh --user --add-path
```

The archive contains `bin/`, `lib/`, `share/`, `RetDec.app`, and generated
`install.sh` / `uninstall.sh`. The package is ad-hoc signed, not notarised;
`install.sh` strips `com.apple.quarantine` from a browser download and says
so. Open the GUI with `open RetDec.app` from the extracted tree, or from
the prefix after install.

GitHub also uploads **loose** `install-macos.sh` and `uninstall-macos.sh`
so they do not collide with Linux `install.sh` / `uninstall.sh` on the
same Release. Prefer the copies **inside** the tarball when you already
extracted it.

`macos-latest` currently builds **arm64** (`uname -m`). An Intel
`macos-x86_64` tarball is only present if CI produced one.

## Docker (when the image is public)

The GHCR image packs the Linux tarball and `/opt/retdec/share/fib_smoke`.
CI smoked `analyse` on that sample ([run 32835822135](https://github.com/odin-loki/RetDec-Decompiler/actions/runs/32835822135)).
Anonymous `docker pull ghcr.io/odin-loki/retdec:v2.0.24` still returns 401
until the package is set public in GitHub Packages. Docker Hub
`imortek/retdec` is unpublished.

When the GHCR package is public:

```bash
docker pull ghcr.io/odin-loki/retdec:v2.0.24
docker run --rm -v "$PWD":/work ghcr.io/odin-loki/retdec:v2.0.24 \
  analyse /opt/retdec/share/fib_smoke -o /work/fib.c
```

The GHCR image is `Dockerfile.runtime` packing the Linux tarball (not the
360-minute in-tree `Dockerfile` LLVM rebuild). `analyse` is a shim for
`retdec-decompiler`. The LLVM `Dockerfile` is dispatch-only
([docker-publish.yml](.github/workflows/docker-publish.yml)).

## Local binary (this tree)

```bash
cmake --preset core-release
cmake --build build/linux --target retdec-decompiler retdec-decompiler-fixture-fib
DEC=./build/linux/src/retdec-decompiler/retdec-decompiler
FIB=./build/linux/tests/decompiler/fib_smoke
"$DEC" "$FIB" -o /tmp/fib.c
```

Presets need **CMake 3.26+** (`CMakePresets.json`). A raw
`cmake -S . -B build` configure is accepted by **CMake 3.13+**
(`CMakeLists.txt`). `fib_smoke` is compiled from
`tests/test_binaries/fib.c` by the `retdec-decompiler-fixture-fib` target.
Read `/tmp/fib.c` and `/tmp/fib.buildable.c`.

## What you should see

- Recovered C in the `-o` path
- Buildable sidecar `*.buildable.c` next to it unless `--no-buildable`
- Config JSON next to the output (`.config.json`)

On the 216-binary stand-in corpus, both default `.c` and `--buildable`
recompile are **216/216** (CC-01). That is a compile gate, not source
parity. Numbers and honesty: [README.md](README.md) Results,
[docs/CLAIMS.md](docs/CLAIMS.md).
