# Semantic Output: C vs C++ Semantics

RetDec decompiles native binaries to a **high-level language (HLL)**. Semantic
recovery (STL containers, algorithms, concurrency) runs **after** the main
llvmir2hll pass and annotates both the config sidecar and the emitted source.

## Default output is C

- `retdec-decompiler` defaults to **`--output-lang c`** (`.c` extension).
- The native pipeline sets llvmir2hll **`TargetHLL`** to `"c"` unless overridden.
- Config JSON stores the choice as **`decompParams.outputLang`** (`"c"` by default).

The emitted **source code** is C syntax (structs, pointers, no C++ templates).

## STL labels are recovery hints, not emitted types

Post-pipeline detectors identify **compiled C++ STL layouts** in binary code
(e.g. three-pointer `std::vector`, red-black tree `std::map`). Detection
records use **C++ STL names** in the `label` field because that is the most
recognizable name for what the binary was compiled from:

```json
{
  "kind": "container",
  "label": "std::vector<int32_t>",
  "confidence": 0.87,
  "detail": "std::vector (confidence=0.87) element=int32_t ...",
  "cHint": "vector_like_3ptr"
}
```

These labels describe **semantics recovered from the binary**, not the syntax
RetDec emits. With default C output, variables remain raw pointers/structs;
the STL name appears in comments and JSON only.

## `--output-lang c` vs `--output-lang cpp`

| Aspect | `c` (default) | `cpp` |
|--------|---------------|-------|
| Emitted syntax | C (`.c`) | C++ (`.cpp`) |
| `semanticDetections[].label` | C++ STL name (hint) | C++ STL name |
| `semanticDetections[].cHint` | Present for containers | Omitted |
| Source comment style | C-friendly layout + STL cross-ref | C++-style detection line |

### C output (default)

Config sidecar includes **`cHint`** for container detections, e.g.
`"vector_like_3ptr"`, `"map_like_rbtree"`, `"string_like_sso"`.

Comments injected above functions combine layout and STL hint:

```c
// [RetDec] vector-like container (3-pointer, elem 4 bytes; STL: std::vector<int32_t>)
void function_401000(void) {
    ...
}
```

### C++ output (`--output-lang cpp`)

No `cHint` field in JSON. Comments keep the C++-oriented form:

```cpp
// [RetDec] std::vector<int32_t> detected (confidence 0.87)
void function_401000(void) {
    ...
}
```

Other detection kinds (sort, algorithm, concurrency) use the same
`[RetDec] <label> detected (confidence …)` form in both modes.

## GUI

Settings → Decompiler → **Output language** passes `--output-lang` to the CLI.
The Problems dock reads `semanticDetections` from `.config.json` regardless of
output language.

## Future: C struct typedef emission

Planned follow-up: when `outputLang` is `c`, map `cHint` values to idiomatic C
typedefs instead of comments only, e.g.:

```c
typedef struct {
    void* begin;
    void* end;
    void* cap;
} retdec_vector_like_t;  /* elem size from recovery; STL: std::vector<T> */
```

Until that lands, **`cHint` + comments** are the C-facing recovery surface;
full type replacement in emitted code remains partial (see
[ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md)).

## Related files

| File | Role |
|------|------|
| `include/retdec/common/semantic_detection.h` | Detection record + comment formatting |
| `src/retdec/semantic_recovery_export.cpp` | JSON merge + comment injection |
| `include/retdec/container_detect/container_detect.h` | `ContainerResult::cHint()` |
| `src/serdes/function.cpp` | `semanticDetections` JSON serialization |
| `tests/decompiler/semantic_c_hint_test.py` | `cHint` schema validation |
