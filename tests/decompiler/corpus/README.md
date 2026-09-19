# Golden decompiler corpus

Regression fixtures for `corpus_regression_test.py` (CTest:
`decompiler_corpus_regression`). This is a **small golden set** for the
v2.0.22 specification-extraction pipeline (native C plus a couple of
managed inputs). It is not the 216-binary algorithm-recovery stand-in and
not DecompileBench.

## Layout

| Path | Purpose |
|------|---------|
| `manifest.yaml` | Fixture metadata and expectations |
| `sources/` | Tiny C/Python sources compiled into binaries (`hello.c`, `vector_sort.c`, `hello.py`) |
| `bin/` | Native executables (generated; not committed) |
| `fixtures/` | Managed inputs (`minimal.wasm`) |

## Build fixtures

```powershell
.\scripts\decompiler\build_corpus_fixtures.ps1
```

```bash
./scripts/decompiler/build_corpus_fixtures.sh
```

CMake also builds native corpus binaries under
`${CMAKE_CURRENT_BINARY_DIR}/corpus_fixtures/` when tests are enabled
(`RETDEC_TESTS=ON`, as in `full-linux-debug` / `full-windows-debug`).

## Managed fixtures

- **WASM** — `fixtures/minimal.wasm` is an 8-byte valid module header (`\0asm\x01\0\0\0`). Output is input-keyed WAT, not C.
- **Python** — `hello.pyc` is generated from `sources/hello.py` via `py_compile` into the CMake `corpus_fixtures/` dir (or `bin/` via the offline scripts). If generation fails, the `hello_pyc` manifest entry is skipped (`skip_if_missing: true`).

## Run manually

After a preset build (`build/linux` on Linux/WSL/macOS, `build/windows` on
MSVC):

```bash
python tests/decompiler/corpus/corpus_regression_test.py \
  path/to/retdec-decompiler \
  tests/decompiler/corpus/manifest.yaml \
  build/tests/decompiler/corpus_fixtures \
  /tmp/corpus_out
```

Normal path: `ctest --preset full-linux-debug` (or `ctest --test-dir build/linux`).
