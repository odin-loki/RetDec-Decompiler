# Quick start (ten minutes)

Plan.md `REL-07`. The zero-build path is `docker pull` once `imortek/retdec`
is published (`REL-02`). Until then, use a local `retdec-decompiler` from a
preset build. There is no `analyse` wrapper yet; that name is the product
command, not a current binary.

## Docker (when the image is published)

```bash
docker pull imortek/retdec
docker run --rm -v "$PWD":/work imortek/retdec \
  retdec-decompiler /work/sample.elf -o /work/sample.c
```

Buildable C (`.h`, `_stubs.c`, `.buildable.c`) is **on by default**.
Pass `--no-buildable` to skip sidecars.

The in-tree `Dockerfile` installs the CLI on `PATH` as `retdec-decompiler`.

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
