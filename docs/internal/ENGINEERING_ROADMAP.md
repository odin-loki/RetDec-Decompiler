# RetDec — Engineering Roadmap

Shippable engineering tiers for RetDec. Research-only items live in
[RESEARCH_FRONTIERS.md](../RESEARCH_FRONTIERS.md).

---

## Tier 1 — Core stability & infrastructure

- Windows native build parity (MSVC preset, install smoke)
- GUI headless decompile path
- Managed format smoke tests (Java, Python, WASM fixtures)
- [x] Decompile profiles — `--profile fast|balanced|quality`, `src/retdec-decompiler/profiles/`
- [x] Unified CLI — `scripts/retdec_cli.py` (batch, diff, emit-json, watch, yara-bridge, export-intel)
- [x] Golden corpus — `tests/decompiler/corpus/` + `corpus_regression_test.py`
- [x] Perf benchmark CI — `scripts/perf_bench_ci.ps1`
- [x] Parallel post-pipeline analysis — `RETDEC_PARALLEL_ANALYSIS`, `ThreadPool` when ≥5 functions
- [x] Function analysis cache — `.retdec-fn-cache.json` sidecar beside output `.c`

## Tier 2 — Managed pipeline depth & differentiation

### Managed emitters

- JVM / DEX / Python / Lua / WASM emitters wired through `managed_decompiler`
- CLI parser + C# emitter integration for .NET assemblies
- Integration fixtures under `tests/managed_integration/`

### Differentiation (CLI / GUI / tests)

- [x] **`--output-lang`** on `retdec-decompiler` (`c|python|csharp|java|wat`; `cpp` rejected until LLVM-22 / CLI-01); persisted as `outputLang` in config JSON; native pipeline sets `TargetHLL` to C
- [x] **GUI preferred output language** — Settings → Decompiler combo → `buildDecompilerArguments()` passes `--output-lang`
- [x] **Semantic detection confidence** — `semanticDetections` in `.config.json` shown in Problems dock (info >0.8, warning >0.5, muted otherwise)
- [x] **Managed format smoke** — `tests/decompiler/managed_format_smoke_test.py` (skips missing fixtures)
- [x] **Semantic output clarity** — C vs C++ comment/`cHint` behavior documented in [SEMANTIC_OUTPUT.md](SEMANTIC_OUTPUT.md)

## Tier 3 — New targets & format polish ✅

### Format router (`managed_decompiler`)

| Input | Detection | Output |
|-------|-----------|--------|
| `.class` | `CAFEBABE` + class version | Java |
| `.jar` | ZIP central directory with `.class` entries | Java |
| `.dex` | `dex\n` magic | Java (DEX) |
| `.apk` | ZIP with `classes*.dex` | Java (APK) |
| `.dll` / `.exe` | PE + CLI COM descriptor (`PeReader::hasCLI`) | C# |
| `.pyc`, `.luac`, `.wasm` | existing magics | Python / Lua / WAT |

- Unknown-format hints logged before native pipeline fallback
- Managed routes log suggested output language (`--output-lang` is ignored for managed inputs)

### Auto unpack → decompile scripts

| Script | Purpose |
|--------|---------|
| [`scripts/unpack_and_decompile.sh`](../scripts/unpack_and_decompile.sh) | Linux / WSL / macOS |
| [`scripts/unpack_and_decompile.ps1`](../scripts/unpack_and_decompile.ps1) | Windows PowerShell |

Flow: optional `retdec-fileinfo` probe → `retdec-unpacker` (when packed) → `retdec-decompiler`.

Use `--keep-unpacked` / `-KeepUnpacked` to retain the intermediate `-unpacked` file.

### Architecture targets stub

See [ARCHITECTURE_TARGETS.md](ARCHITECTURE_TARGETS.md) for RISC-V, ARM64, and SASS status (not implemented; prerequisites listed).

### Tests

- `tests/decompiler/format_router_test.py` — byte-buffer unit tests for `detectManagedFormatFromBytes` via `format_router_probe`

---

## Tier 4 — Security & research workflows ✅

- [x] Cross-binary diff — `scripts/retdec_binary_diff.py`, `retdec_cli.py diff`
- [x] Threat intel export — `scripts/retdec_export_intel.py`, GUI **File → Export Threat Intel…**
- [x] YARA bridge stub — `scripts/yara_retdec_bridge.py`
- [x] Symbol server doc — [SYMBOL_SERVER.md](SYMBOL_SERVER.md)
- [x] Semantic recovery export — `semanticDetections` in config JSON, `[RetDec]` comments in `.c`/`.cpp`, GUI Problems dock; C output adds `cHint` + layout comments ([SEMANTIC_OUTPUT.md](SEMANTIC_OUTPUT.md))
- STL container / algorithm pattern matching in emitted output (partial — detections + C `cHint` comments exported; full C struct typedef / C++ type replacement ongoing — [SEMANTIC_OUTPUT.md](SEMANTIC_OUTPUT.md))

## Tier 5 — Architecture & extensibility ✅

- [x] [PIPELINE_REDESIGN_TODO.md](PIPELINE_REDESIGN_TODO.md) — 29-stage gap table (hook/partial/full, P0–P3)
- [x] Pass profile registry — [profiles/README.md](../src/retdec-decompiler/profiles/README.md) + [index.json](../src/retdec-decompiler/profiles/index.json) (`fast`, `balanced`, `quality`)
- [x] [pipeline_builder_schema.json](pipeline_builder_schema.json) + [validate_pipeline_json.py](../scripts/validate_pipeline_json.py)
- [x] Reference decompiler plugin sample — [examples/decompiler_plugin/](../examples/decompiler_plugin/)
- [x] [FORMAL_VERIFICATION_BRIDGE.md](FORMAL_VERIFICATION_BRIDGE.md) — Frama-C export path (documentation)
- [x] [validate_pipeline_test.py](../tests/decompiler/validate_pipeline_test.py) — CTest profile schema validation

```powershell
python scripts/validate_pipeline_json.py --all-profiles
retdec-decompiler --profile balanced -o out.c input.exe
python examples/decompiler_plugin/post_process_output.py out.c
```

## Backlog

- GUI tri-pane output language parity with CLI
- Batch / agent workflows (`scripts/example-coding-agent.sh`)

---

## Related docs

- [pipeline_stage_map.md](pipeline_stage_map.md) — stage → source mapping
- [PIPELINE_REDESIGN_TODO.md](PIPELINE_REDESIGN_TODO.md) — pipeline gap checklist

- [CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md) — GPU vs CPU-only components
- [ARCHITECTURE_TARGETS.md](ARCHITECTURE_TARGETS.md) — CPU / GPU architecture roadmap
- [future_directions.md](future_directions.md) — consolidated long-term agenda
- [RESEARCH_FRONTIERS.md](RESEARCH_FRONTIERS.md) — Tier 7 research only
