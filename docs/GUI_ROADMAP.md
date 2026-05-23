# RetDec GUI Roadmap

Phased plan for GUI/product work. Update checkboxes as items ship.

## Phase A — Trustworthy IDE (complete)

- [x] GUI subprocess parity with CLI (file redirect, absolute paths, `parity_bench.ps1`)
- [x] Headless decompile for CI (`--headless-decompile`, `parity_ctest.ps1`)
- [x] Shared `buildDecompilerArguments()` / `decompiler_launch`
- [x] Cache reuse on re-open (`.gui-decompiled.c` + `.config.json`)
- [x] Fast LLVM preset (`llvm_passes_fast.json`)
- [x] **Artifact loader** — config + `.dsm` + `.ll` → Functions, Strings, Call graph, CFG, Assembly, IR
- [x] **Binary browser** — real PE sections from fileinfo `--verbose` JSON
- [x] **Inspect** — cache fileinfo JSON, absolute paths, `fileinfoReady` signal
- [x] **Decompiled C** — `setSourceFromPath()` (single read, async display)
- [x] Decompile progress from log tail (stage keywords → status bar)
- [x] Slim Problems dock tab (diagnostics without console noise)
- [x] README / in-app text aligned with v3 (AI panel removed, etc.)

## Phase B — Fast iteration (complete)

- [x] Output directory option (avoid OneDrive sync beside binary)
- [x] Function-scoped re-decompile (`Analysis → Re-decompile Selected Function`, `--select-functions`)
- [x] Batch decompile queue (File → Batch Decompile…, sequential runs, Stop cancels)
- [x] Export bundle (zip `.c`, config, log, command line)
- [x] Optional live console tail (rate-limited, opt-in)

## Phase C — CI / release

- [x] `tests/decompiler/` — fib fixture, CLI smoke, GUI headless, Windows parity ctest
- [x] `.github/workflows/ctest-windows.yml` (integration label)
- [x] Linux integration workflow (`.github/workflows/ctest-linux.yml`)
- [x] Perf benchmark trend in CI (`.github/workflows/perf-nightly.yml`, weekly + workflow_dispatch; `scripts/perf_bench_ci.ps1`)
- [x] Install smoke after `cmake --install`

## Phase D — Differentiation

- [ ] CUDA wiring into hot analysis paths (or document CPU-only)
- [x] Reference decompiler plugin sample — `examples/decompiler_plugin/`
- [ ] AI assistant: external Ollama HTTP vs in-process vs remove
- [x] Multi-language output picker in GUI — Settings → Decompiler → Preferred output language (`--output-lang`)

## Verification commands

```powershell
# Unit tests (headless)
$env:RETDEC_GUI_HEADLESS="1"
$env:QT_QPA_PLATFORM="offscreen"
build\windows\tests\gui\retdec-gui-tests.exe

# CLI vs GUI subprocess parity (~2 min on 1 MB binary)
.\scripts\parity_bench.ps1

# CTest integration (after full-windows-debug build)
ctest --test-dir build\windows -L integration --output-on-failure
```
