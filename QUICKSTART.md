# Quick start (ten minutes)

Plan.md `REL-07`. Prebuilt binaries are on the
[v2.0.21 GitHub Release](https://github.com/odin-loki/RetDec-Decompiler/releases/tag/v2.0.21).
`docker pull imortek/retdec` is still unpublished (`REL-02`).

Buildable C (`.h`, `_stubs.c`, `.buildable.c`) is **on by default**.
Pass `--no-buildable` to skip sidecars.

## Linux tarball (GitHub Release)

```bash
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.21/retdec-2.0.21-linux-x64.tar.gz
tar xzf retdec-2.0.21-linux-x64.tar.gz
cd retdec-2.0.21-linux-x64
chmod +x install.sh uninstall.sh
./install.sh --user --add-path
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.21/fib_smoke
retdec-decompiler fib_smoke -o fib.c
```

`fib_smoke` is gcc -O1 of `tests/test_binaries/fib.c` (same flags as
`retdec-decompiler-fixture-fib`). Read `fib.c` and `fib.buildable.c`.
Run without installing: `export PATH="$(pwd)/bin:$PATH"`. The same Release has
`retdec-2.0.21-linux-x64.tar.gz.sigstore.json` and `fib_smoke.sigstore.json`.

## Linux AppImage

```bash
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.21/retdec-2.0.21-x86_64.AppImage
chmod +x retdec-2.0.21-x86_64.AppImage
APPIMAGE_EXTRACT_AND_RUN=1 ./retdec-2.0.21-x86_64.AppImage --help
```

The AppImage launches `retdec-gui`. `APPIMAGE_EXTRACT_AND_RUN=1` avoids FUSE.
A keyless bundle `retdec-2.0.21-x86_64.AppImage.sigstore.json` is on the Release.

## Windows (NSIS and zip)

From the same Release:

- `retdec-2.0.21-windows-x64-setup.exe` — NSIS installer
- `retdec-2.0.21-windows-x64-portable.zip` — portable tree

Keyless Sigstore bundles `retdec-2.0.21-windows-x64-portable.zip.sigstore.json`
and `retdec-2.0.21-windows-x64-setup.exe.sigstore.json` are on the Release.
Authenticode is not applied.

## Docker (when the image is public)

The GHCR image packs the Linux tarball and `/opt/retdec/share/fib_smoke`.
CI smoked `analyse` on that sample ([run 32835822135](https://github.com/odin-loki/RetDec-Decompiler/actions/runs/32835822135)).
Anonymous `docker pull ghcr.io/odin-loki/retdec:v2.0.21` still returns 401
until the package is set public in GitHub Packages. Docker Hub
`imortek/retdec` is unpublished.

When the GHCR package is public:

```bash
docker pull ghcr.io/odin-loki/retdec:v2.0.21
docker run --rm -v "$PWD":/work ghcr.io/odin-loki/retdec:v2.0.21 \
  analyse /opt/retdec/share/fib_smoke -o /work/fib.c
```

The in-tree `Dockerfile` installs the CLI on `PATH` as `retdec-decompiler`
and `analyse` (same argv).

## Local binary (this tree)

```bash
cmake --preset core-release
cmake --build build/linux --target retdec-decompiler retdec-decompiler-fixture-fib
DEC=./build/linux/src/retdec-decompiler/retdec-decompiler
FIB=./build/linux/tests/decompiler/fib_smoke
"$DEC" "$FIB" -o /tmp/fib.c
```

`fib_smoke` is compiled from `tests/test_binaries/fib.c` by the
`retdec-decompiler-fixture-fib` target. Read `/tmp/fib.c` and
`/tmp/fib.buildable.c`.

## What you should see

- Recovered C in the `-o` path (default `.c` still may not recompile)
- Buildable sidecar `*.buildable.c` next to it unless `--no-buildable`
- Config JSON next to the output (`.config.json`)

Numbers and honesty: [README.md](README.md) Results, [docs/CLAIMS.md](docs/CLAIMS.md).
