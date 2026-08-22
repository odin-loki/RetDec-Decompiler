# Execution plan — remaining audit + benchmark + compilable neural

**Date:** 2026-08-22  
**Head:** after `6196d738` (Weeks 1–2 blocking set is done)  
**Hard stops:** no `deps/llvm/` bump; no test deletion; default F5 unchanged unless opt-in.

This is the working order. Later waves depend on earlier measurement.

---

## Already done (do not redo)

B1–B6, L1–L4, N1–N5, N13, S1–S5, T5, E11, E16, D1 (scope only), C9 note.

---

## Goal A — Honest fork vs stock numbers

Stock (`remnux/retdec` / RetDec 5.0): **syntax 1.0, recompile 0%, mean wall ~0.24s** (216 binaries).  
Fork baseline (pre-B1): **syntax 1.0, recompile 0%, mean wall ~1.49s**, algorithm F1 **1.000 (withdrawn)**.

We publish **four quality rates** and **tail latency**, never a single F1:

| Metric | How |
|--------|-----|
| `syntax_valid` | output `.c` exists and non-empty |
| `tu_valid` | `cc -fsyntax-only -std=c11 -w` (Q2) |
| `recompile_success` | `cc -o` links (Q1) |
| `coverage_equivalence` | existing runner hook |
| `mean/p50/p90/p99/max wall_s` | C10 |
| algorithm F1 | **after** B1; must survive rename-guard |

Compare **this fork** vs **stock RetDec 5.0** on the same corpus, same machine, same limit.

---

## Goal B — Neural emits compilable C

Deterministic analysis writes C. Neural may refine it. **Neither path executes that C** (N3).

1. **Deterministic `--emit-buildable` (Q1/Q9)**  
   Next to `<out>.c`: `<out>.h` (stdint/stddef + recovered typedefs), `<out>_stubs.c` (weak/empty stubs for undeclared calls), optional `<out>.build.json`.  
   Prepend `#include` so `cc -c` / `-fsyntax-only` can succeed.  
   Default **off** (`RETDEC_EMIT_BUILDABLE=1` or CLI) so F5/headless stay unchanged.

2. **Neural compilable pass**  
   After each accepted refine, run the existing argv compile-gate on the refined TU (with stubs if emit-buildable).  
   If it fails: one retry prompt that includes the compiler stderr; accept only if compile-gate passes.  
   Sidecar `.refined.c` is what the harness scores.  
   Opt-in: `RETDEC_NEURAL_REFINE=1` + model path. Cap tokens. No host execution.

3. **Benchmark neural vs deterministic vs stock**  
   Same N binaries: stock wall/quality; fork wall/quality; fork+neural `tu_valid` / `recompile` on `.refined.c`.  
   Neural wall reported separately (`neural_refine_wall_s`).

---

## Goal C — Close the 6× wall gap (without lying)

1. Measure detector-stage share (`analysis.detectors` in `RETDEC_PROFILE_JSON`) — C9.  
2. Add `RETDEC_SKIP_SEMANTIC_RECOVERY=1` for A/B timing only (not default).  
3. If detectors dominate, cheapen or gate them; do not silently drop correctness work from default F5 without a flag.  
4. Target: **≤2× stock mean wall** on ci-core (audit XVI.6 #8). First milestone: beat 1.492s and publish tails.

---

## Waves

### Wave 1 — Measure + harness (this session)

- [x] Fetch DecompileBench corpus (`ci-core` then `full` if time).
- [x] Build `retdec-decompiler`.
- [x] Runner: `tu_valid`, p50/p90/p99/max, stock compare table, refined-C scoring.
- [x] Run ci-core fork vs stock; write `results/compare-fork-vs-stock.md`.
- [x] Re-run algorithm-recovery name-blind: `mean_f1` ≈ 0.335 (not a product claim).
- [x] Profile one sample with `RETDEC_PROFILE_JSON` (C9). Detectors ~4–6% of wall.

### Wave 2 — Compilable C (deterministic + neural)

- [x] Implement emit-buildable sidecar writer (Q1/Q9).
- [x] Neural compile-retry; reject refinements that fail `-fsyntax-only`.
- [x] Unit tests: stub file exists; compile-gate rejects broken refine; mock can produce a compiling TU.
- [x] Neural smoke: mock path `RETDEC_NEURAL_FORCE_MOCK` + `RETDEC_NEURAL_MOCK_EMIT_C` emitted a `cc -fsyntax-only` TU (`NEURAL_REFINED_TU_VALID=1`). No GGUF in-tree.

### Wave 3 — Speed

- [x] Skip-semantic A/B env exists (`RETDEC_SKIP_SEMANTIC_RECOVERY`).
- [x] Detectors are not the 35× Debug/WSL vs stock Release gap (`pipeline.pm_run` dominates).
- [x] Re-run ci-core; update compare table. **No speed win to advertise.**

### Wave 4 — Audit months 1–6 (after numbers exist)

Priority inside the wave:

1. **A7** constant-keyed crypto (SHA/MD5/ChaCha/Blowfish P-array, DES SPtrans). Base64 skipped (no SSA string table). **A8** lock-prefix/ldxr: SSA `IrInstr::Op` has no lock/atomic, so not implemented.  
2. **E7** `docs/CLAIMS.md` register.  
3. **E8/E9** link-graph / doc-vs-code CI scripts.  
4. **N6–N9** model allowlist default, GGUF header parse, instance state, richer manifest.  
5. **N15** GBNF rename-map for Naming tier (if llama.cpp grammar API is at the pin).  
6. **A3** rewrite binary-search detector as a dataflow query **on existing SSA** (A1 LLVM move is blocked by the LLVM pin).  
7. **E1** 3–5 real-binary detector tests (O0), no name hints — `scripts/ci/run_e1_real_binary_smoke.sh`.  
8. **Q4** measure existing goto-optimizer baseline (no SAILR port yet).  
9. **P1** sketch only unless a tiny C ABI already exists.  
10. **S11** `docs/THREAT_MODEL.md`.

### Wave 5 — Do not start until explicitly unblocked

- **D2** LLVM 8→18 (forbidden by `.cursorrules`).  
- Full **T1/T2** ARM64/RISC-V lifters (weeks).  
- **P2** Python bindings (needs P1).  
- **X1–X8** research.  
- Physical move of 7k LOC GPU trees.  
- Re-enabling `RETDEC_NEURAL_BATCH`.

---

## Definition of done for *this* push

1. Plan file in-tree (this document).  
2. Harness reports tu_valid + tail latency + stock delta.  
3. Emit-buildable exists and is tested.  
4. Neural path can accept only compile-gate-passing C.  
5. At least one ci-core (or smaller) fork-vs-stock table checked in or generated.  
6. Changelog states withdrawn F1 and new metrics.  
7. No LLVM bump, no skipped tests, no default-ON neural, no executing decompiled C.
