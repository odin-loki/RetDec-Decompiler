# E1 — real-binary detector tests

Goal: 3–5 tests that exercise detectors on **compiled** gcc `-O0` shapes,
without filename hints and without lowering thresholds.

## What exists today

| Path | What it does |
|------|----------------|
| `tests/algo_recover/` | C++ gtest. Builds `SSAFunction` in memory and calls `BinarySearchDetector`, `TransformDetector`, `AlgorithmDetector`, etc. CMake target: `retdec_algo_recover_tests`. |
| `tests/crypto_detect/` | Same pattern for crypto (in-memory SSA). No ELF load. |
| `tests/sort_detect/` | In-memory SSA, including `BubbleSortDetector`. Not an ELF loader. |
| `tests/algorithm_recovery/` | Python harness + label tests. **No `CMakeLists.txt`.** |
| `tests/e2e/` | **Does not exist.** |
| `tests/decompiler/` | Spawns `retdec-decompiler` (fib smoke, CLI). Needs the decompiler target, share/ config, and can time out. |

There is **no** public API that loads a compiled ELF and returns
`ssa::SSAFunction` for detectors. `AlgorithmDetector::detect` and
`IdiomDetector::detect` take `const ssa::SSAFunction&` only
(`include/retdec/algo_recover/algo_recover.h`).

Spawning `retdec-decompiler` from `tests/algo_recover` would pull in share/
layout, YARA/support files, and multi-minute first runs — not a lightweight
ctest. That path was **not** added.

## What was added instead

`tests/algo_recover/algo_recover_test.cpp` now has gcc `-O0` **IR-shape**
fixtures (generic function name `"f"`, no stem / filename):

1. `makeGccO0BinarySearchShape` — `binary_search.c`: `mid = lo + (hi - lo) / 2`,
   load at mid, compare vs invariant target, lo/hi updated from mid.
2. `makeGccO0BubblesortShape` — `bubblesort.c`: nested i/j loops, adjacent
   load/compare, two-store swap, `n-1-i` subtract. Asserts
   `BinarySearchDetector` stays below the existing 0.45 medium floor (no
   midpoint Shr/Div). Does not lower any threshold.
3. `makeGccO0MemcpyLoopShape` — `memcpy_loop.c`: indexed `dst[i] = src[i]`
   with Compare + index phi.

These encode the *shape* of the three ci-core ELFs. They are not a substitute
for decompiling those ELFs.

`AlgorithmDetector` still picks the highest-confidence registered detector.
On the overflow-safe binary-search fixture, `FindDetector` can outscore
`BinarySearchDetector`; the new tests call `BinarySearchDetector` /
`TransformDetector` directly so they do not depend on that race.

## Three binaries that MUST be used next

Stage or copy these into `tests/decompilebench/corpus/` (or point `--corpus`
at `tests/algorithm_recovery/corpus/` — the decompilebench names are often
WSL symlinks that Windows cannot open):

| Binary | Source | Detector to judge |
|--------|--------|-------------------|
| `binary_search-gcc-O0` | `tests/algorithm_recovery/sources/binary_search.c` | `algo_recover::BinarySearchDetector` / `kind` BinarySearch |
| `bubblesort-gcc-O0` | `tests/algorithm_recovery/sources/bubblesort.c` | `sort_detect::BubbleSortDetector` (not filename) |
| `memcpy_loop-gcc-O0` | `tests/algorithm_recovery/sources/memcpy_loop.c` | `TransformDetector` Copy / `std::copy` |

Do not use filename or stem fallback when scoring these three.

## Command to extract detections

Requires a **linked** `retdec-decompiler` and the three ELFs on disk.

```bash
# Prefer the in-tree ninja binary when it is a real ELF (not a 0-byte stub):
DEC=build/linux/src/retdec-decompiler/retdec-decompiler
# Fallback: DEC=build/linux/install/bin/retdec-decompiler

# On Windows, use the real ELF directory (not broken decompilebench symlinks):
CORPUS=tests/algorithm_recovery/corpus

python3 scripts/extract_decompiler_predictions.py \
  --decompiler "$DEC" \
  --corpus "$CORPUS" \
  --names binary_search-gcc-O0,bubblesort-gcc-O0,memcpy_loop-gcc-O0 \
  --no-stem-fallback \
  --timeout 120 \
  --out results/e1-three-binaries.json
```

`--no-stem-fallback` keeps labels out of the source-sidecar / filename path
(`labels_from_config(..., stem_fallback=False)`). Do not pass `--sources` for
this measurement.

Detections are read from each job’s `.config.json`
`functions[].semanticDetections[]` (`kind` + `label` + `confidence`).

Then inspect `predictions_raw` in the JSON (raw = no stem fallback). Expected
class names after mapping (existing script, unchanged thresholds):

- `binary_search-gcc-O0` → `BinarySearch` / `Search` if the algorithm label is
  `binary_search` at the configured confidence.
- `bubblesort-gcc-O0` → `BubbleSort` / `Sort` from **sort** detections.
- `memcpy_loop-gcc-O0` → `Memcpy` / `Copy` if the algorithm label is
  `std::copy`.

If `tests/decompilebench/corpus` is staged (`scripts/fetch_decompilebench_corpus.sh --profile ci-core`)
and the three names resolve to readable files, `--corpus tests/decompilebench/corpus` is equivalent.
