# Semantic Output: C vs C++ Semantics

RetDec Imortek **2.0.22** decompiles **native** binaries to C. Semantic
recovery (containers, algorithms, sorts, crypto, serialisation, concurrency,
design patterns) runs **after** llvmir2hll, rebuilds a lightweight SSA
module, and annotates the config sidecar and `// [RetDec]` comments. It does
**not** replace the C writer with idiomatic `std::vector` / `std::sort`.

Name-blind algorithm-recovery F1 on the 216-binary corpus is **0.056**
(ci-core 0.126). That is the headline quality number
([BENCHMARKS.md](BENCHMARKS.md)). Detectors exist; they are not
production-quality reconstruction.

Skip the whole block with `RETDEC_SKIP_SEMANTIC_RECOVERY=1`.

## Default output is C

- `retdec-decompiler` defaults to **`--output-lang c`** (`.c` extension).
- The native pipeline sets llvmir2hll **`TargetHLL`** to `"c"`.
- `--output-lang cpp` / `c++` / `cxx` is **rejected**
  (`src/retdec-decompiler/output_lang.cpp`). `src/cxx_backend/` is unwired.
- Config JSON stores `decompParams.outputLang` (`"c"` by default).

The emitted **source** is C syntax (structs, pointers, no templates).

`--buildable` / `RETDEC_EMIT_BUILDABLE` (default on) writes `.h`,
`_stubs.c`, and `.buildable.c` beside that `.c`. The default `.c` is
unchanged.

## STL labels are recovery hints, not emitted types

Post-pipeline detectors identify **compiled layouts** (for example a
three-pointer `std::vector`, red-black `std::map`). Records use C++ STL
names in `label` because that is the usual name for the compiled origin:

```json
{
  "kind": "container",
  "label": "std::vector<int32_t>",
  "confidence": 0.87,
  "detail": "std::vector (confidence=0.87) element=int32_t ...",
  "cHint": "vector_like_3ptr"
}
```

With default C output, variables remain raw pointers/structs; the STL name
appears in comments and JSON only.

## What actually runs (`src/retdec/retdec.cpp`)

| Detector | Directory | Examples of `kind` / labels |
|----------|-----------|-----------------------------|
| Containers | `src/container_detect/` | vector, list, map, unordered_map, string, ring buffer |
| `<algorithm>`-like loops | `src/algo_recover/` | transform, find, binary_search, partition, accumulate, for_each, copy, … |
| Sorts | `src/sort_detect/` | introsort, mergesort, heapsort, quicksort, bubblesort, radix |
| Concurrency | `src/concurrency_detect/` | std::thread, pthread, Win32, atomics, OpenMP, TBB |
| Crypto | `src/crypto_detect/` | AES, SHA-1/256, ChaCha20, RSA/DH, RC4, MD5, CRC, … |
| Serialisation | `src/serial_detect/` | Protobuf, FlatBuffers, JSON, XML |
| Design patterns | `src/pattern_detect/` | Singleton, Factory, … |
| Type inference stats | `src/type_inference/` | only if `RETDEC_TYPE_INFERENCE=1` |

`src/idiom_reconstruct/` is a separate library (**not** linked into
`decompile()`). Compiler idioms that do run are bin2llvmir `retdec-idioms`.

`src/module_cluster/` CMake/module splitting is **not** in this pass.

## Native vs managed `--output-lang`

| Aspect | Native `c` (only native writer) |
|--------|-----------------------------------|
| Emitted syntax | C (`.c`) |
| `semanticDetections[].label` | Often a C++ STL or algorithm name (hint) |
| `semanticDetections[].cHint` | Present for containers |
| Source comment style | C comments + STL/algorithm cross-ref |

Other CLI `--output-lang` values (`python`, `csharp`, `java`, `wat`) apply
to **managed** inputs via format-specific emitters, not the native LLVM
path. On native binaries those flags still select the C HLL writer (with a
log line). Managed JVM/DEX emit Java; `.pyc` Python; `.luac` Lua; `.wasm`
WAT; .NET CLI C#. F# / VB.NET / Kotlin emitters exist in-tree and are not
this dispatch.

### C comments (default)

Config sidecar includes **`cHint`** for container detections, e.g.
`"vector_like_3ptr"`, `"map_like_rbtree"`, `"string_like_sso"`.

```c
// [RetDec] vector-like container (3-pointer, elem 4 bytes; STL: std::vector<int32_t>)
void function_401000(void) {
    ...
}
```

## GUI

Settings → Decompiler → **Output language** passes `--output-lang` to the
CLI child. The Problems dock reads `semanticDetections` from `.config.json`.
Analysis-tab detector checkboxes are in-process GUI state; F5 still runs
`retdec-decompiler` (see `SettingsDialog::buildAnalysisTab` banner).

## Future: C struct typedef emission

Planned: map `cHint` values to idiomatic C typedefs instead of comments
only. Until that lands, **`cHint` + comments** are the C-facing recovery
surface; full type replacement in emitted code is not done.

## Related files

| File | Role |
|------|------|
| `include/retdec/common/semantic_detection.h` | Detection record + comment formatting |
| `src/retdec/semantic_recovery_export.cpp` | JSON merge + comment injection + buildable sidecars |
| `src/retdec/function_analysis_cache.cpp` | Per-function detector orchestration / cache |
| `include/retdec/container_detect/container_detect.h` | `ContainerResult::cHint()` |
| `src/serdes/function.cpp` | `semanticDetections` JSON serialization |
| `tests/decompiler/semantic_c_hint_test.py` | `cHint` schema validation |
