# Quick start (ten minutes)

Plan.md `REL-07`. Linux x86_64 can skip the build: download the tarball from
the [v2.0.21 GitHub Release](https://github.com/odin-loki/RetDec-Decompiler/releases/tag/v2.0.21).
`docker pull imortek/retdec` is still unpublished (`REL-02`). Windows NSIS/zip
are still building on that same tag workflow.

Buildable C (`.h`, `_stubs.c`, `.buildable.c`) is **on by default**.
Pass `--no-buildable` to skip sidecars.

## Linux tarball (GitHub Release)

```bash
curl -fL -O https://github.com/odin-loki/RetDec-Decompiler/releases/download/v2.0.21/retdec-2.0.21-linux-x64.tar.gz
tar xzf retdec-2.0.21-linux-x64.tar.gz
cd retdec-2.0.21-linux-x64
chmod +x install.sh uninstall.sh
./install.sh --user --add-path
retdec-decompiler --help
```

Run without installing: `export PATH="$(pwd)/bin:$PATH"`. AppImage was not
produced on this tag run. Cosign keyless bundles (`.sigstore.json`) attach
when `sign-release-sbom.yml` or a later installer job has signed the tarball.

## Docker (when the image is published)

```bash
docker pull imortek/retdec
docker run --rm -v "$PWD":/work imortek/retdec \
  analyse /work/sample.elf -o /work/sample.c
```

The in-tree `Dockerfile` installs the CLI on `PATH` as `retdec-decompiler`
and `analyse` (same argv). The image is not on Docker Hub yet.

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
