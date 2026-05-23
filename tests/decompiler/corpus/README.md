# Golden decompiler corpus

Regression fixtures for `corpus_regression_test.py` (CTest: `decompiler_corpus_regression`).

## Layout

| Path | Purpose |
|------|---------|
| `manifest.yaml` | Fixture metadata and expectations |
| `sources/` | Tiny C/Python sources compiled into binaries |
| `bin/` | Native executables (generated; not committed) |
| `fixtures/` | Managed inputs (`minimal.wasm`, optional `hello.pyc`) |

## Build fixtures

```powershell
.\scripts\decompiler\build_corpus_fixtures.ps1
```

```bash
./scripts/decompiler/build_corpus_fixtures.sh
```

CMake also builds native corpus binaries under `${CMAKE_CURRENT_BINARY_DIR}/corpus_fixtures/` when tests are enabled.

## Managed fixtures

- **WASM** — `fixtures/minimal.wasm` is an 8-byte valid module header (`\0asm\x01\0\0\0`).
- **Python** — `fixtures/hello.pyc` is generated from `sources/hello.py` via `py_compile` when Python is available. If generation fails, the `hello_pyc` manifest entry is skipped (`skip_if_missing: true`).

## Run manually

```bash
python tests/decompiler/corpus/corpus_regression_test.py \
  path/to/retdec-decompiler \
  tests/decompiler/corpus/manifest.yaml \
  build/tests/decompiler/corpus_fixtures \
  /tmp/corpus_out
```
