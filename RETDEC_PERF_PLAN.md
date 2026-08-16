# RetDec Fork — Performance Plan

Status: draft, based on direct repo inspection on 2026-08-16 (commit `bfe9dab`,
squashed initial commit — no per-file blame history available beyond that).
Every claim below was checked against the actual source, `CMakeLists.txt`,
`cmake/deps.cmake`, and `results/*.json` in this tree, plus upstream
`avast/retdec` fetched live for comparison. Where something is a hypothesis
rather than a measured fact, it's marked as such.

---

## 0. The number that matters

`results/baseline-2026-08.json` (already committed, generated 2026-08-16) has
this sitting in it:

| | mean wall time (s) |
|---|---|
| This fork (`decompilebench` harness, ci-core corpus) | **1.492** |
| Stock RetDec 5.0 (`remnux/retdec` Docker, same corpus) | **0.242** |

**That's a 6.17x slowdown.** It's already measured, already gated in CI
(`baseline-2026-08.json` thresholds), and it's a stable number — stock's
mean_wall_s is 0.240–0.249 across O0/O2/O3 and across the 9-binary and
216-binary corpora, so it's not corpus noise.

The per-sample data backing the fork's `1.492` isn't committed — only the
rolled-up mean survived into the baseline file (`results/decompilebench.json`
is gitignored per `results/README.md`, generated then discarded). **Step one
of this whole plan is regenerating and keeping that file**, because right now
nobody can see whether the 6.17x is uniform across binaries or concentrated
in a few — and that distinction changes everything else in this document.

---

## 1. What's actually true about the LLVM pass configuration

This was the original hypothesis ("maybe it's the configuration"), so it's
worth stating clearly: **checked, and it's not the primary cause.**

- This fork's default pipeline (`src/retdec-decompiler/decompiler-config.json`,
  key `decompParams.llvmPasses`) has **100 passes**.
- Upstream `avast/retdec`'s equivalent default pipeline has **142 passes** —
  it runs its entire mid-pipeline optimization block
  (`instcombine → tbaa → basicaa → simplifycfg → early-cse → ... → constprop
  → instcombine`) **twice in a row, back to back**. This fork's default does
  not have that duplication.
- So on pass *count and redundancy*, this fork's default pipeline is already
  leaner than what it's being compared against. A 6x slowdown cannot come
  from having a *more* trimmed pass list than stock.

One real bug found while comparing, unrelated to the slowdown but worth
fixing:

- `src/retdec-decompiler/profiles/balanced.json` and
  `src/retdec-decompiler/profiles/quality.json` are **byte-for-byte
  identical**. `--profile quality` currently buys nothing over
  `--profile balanced`. Decide what should actually differ (candidates: keep
  the `verify` module-verification calls and `loop-accesses`/`loop-load-elim`
  analysis in quality, drop them from balanced) and make them diverge.

**Conclusion: don't spend the first optimization pass on pass-list tuning.**
It's not free (see §4 for where it's still worth revisiting), but it is not
where 6x is hiding.

---

## 2. Where the 6x almost certainly is: the Post-pipeline analysis phase

`src/retdec/retdec.cpp`, `decompile()`, from roughly line 630. After the LLVM
`legacy::PassManager` finishes (`pm.run(*module)`), this fork runs a second,
substantial phase with **no equivalent in stock RetDec at all**:

1. `buildSsaModule(*module)` — rebuilds an entire second SSA-form IR
   (`retdec::ssa::SSAModule`) from the already-lowered LLVM module.
2. `CallConvPass::runAll()` — calling-convention inference over every function.
3. `IpaPass::run()` — interprocedural call-graph + summary propagation.
4. **`TypeInferencePass`, run in a plain serial `for` loop, one function at a
   time.** No parallelism, no cache. This is the one clear outlier: every
   other per-function stage below it got the parallel/cache treatment and
   this one didn't.
5. Container / algorithm / sort / concurrency detectors — **these are
   already parallelized** (`analysis::parallelAnalysisEnabled()`, defaults to
   on when `hardware_concurrency() > 2`) **and already cached**
   (`analysis::incrementalCacheEnabled()`, defaults to on,
   `FunctionAnalysisCache` sidecar keyed on a per-function body hash). This
   part of the fork is already well-engineered — don't touch it, copy its
   pattern instead (see §3.1).
6. `analysis::augmentIdiomsFromInputBinary` / `augmentSortsFromInputBinary` /
   `augmentContainersFromInputBinary` / `augmentConcurrencyFromInputBinary`
   — checked these directly (`src/retdec/idiom_stem_augment.cpp`): despite
   the name, these do **not** re-read or re-parse the input binary. They do
   cheap filename-stem string matching against the corpus's naming
   convention (`generated_bubblesort-gcc-o2` → infers "bubblesort"). Not a
   hotspot. Ruled out.
7. `neural::maybeRefineDecompilerOutput()` — gated behind
   `RETDEC_NEURAL_REFINE` env var, **off by default**. Not part of the 6.17x
   unless that env var was set during the benchmark run (worth confirming —
   see §6 checklist item 1).
8. `ptx_decompile::OclHostRecovery` — scans every module for OpenCL host
   patterns, unconditionally, even for binaries with nothing GPU-related in
   them.

None of steps 1–4 and 8 have per-stage timing. `RETDEC_BIN2LLVMIR_DIAG=1`
gives you exactly one number for this entire block:
`[analysis-diag] post_pipeline_analysis_wall_ms=<N>`. You cannot currently
tell SSA rebuild apart from IPA apart from the serial type-inference loop
apart from OpenCL scanning. **This is the single most valuable thing to fix
before doing anything else** — see §3.

---

## 3. Phase 1 — Close the instrumentation gap first

You have a real profiler already: `src/profiling/profiling.cpp` /
`include/retdec/profiling/profiling.h`. `Profiler::measure(name)` returns a
`ScopeTimer` (RAII, records on destruction), there's `sampleRss()` for peak
RSS, and `ProfilingReport::toText()` already renders a table. **It's wired
into exactly two places: `cuda_accel` and `opencl`.** bin2llvmir, the
Post-pipeline analysis phase, and `capstone2llvmir` (the instruction-lifting
layer — 26k lines, zero profiling coverage) have none of it.

Concrete tasks, in order:

### 3.1 Instrument the Post-pipeline analysis phase
Wrap each of the 8 stages listed in §2 in `Profiler::instance().measure("stage-name")`.
This is a few hours of mechanical work — the call sites are already isolated
in the block starting at `retdec.cpp:633`. This answers, per binary, which
of SSA-rebuild / CallConv / IPA / type-inference / detectors / OpenCL-scan
is actually expensive. Given §2.4 (serial, uncached type inference) is the
one stage architecturally worse than its neighbors, it's the leading
hypothesis for where time concentrates — but confirm before fixing it.

### 3.2 Extend the LLVM pass timer to cover stock passes, not just `retdec-*`
`ModulePassTimerAfter::runOnModule()` currently only logs when
`utils::startsWith(_passArg, "retdec")`. Every stock LLVM pass
(`instcombine`, `gvn`, `licm`, `simplifycfg`, all ~60 of them in the default
100-pass list) gets aggregated into one undifferentiated "LLVM" phase bucket
in the `ModulePassPrinter` phase logic. Remove that filter (or add a second,
unfiltered timer) and route the result through `Profiler::recordFunction()`
or a new `Profiler::record()` call instead of just `Log::info()`, so it ends
up in the same structured report as everything else instead of scattered
log lines.

### 3.3 Instrument `capstone2llvmir`
Zero coverage currently. Instruction-by-instruction semantic lifting is a
classic hidden cost in every decompiler and this fork has never measured it.
At minimum, wrap the top-level per-function lifting entry point in a
`Profiler::measure()` call so it shows up as one line item; don't try to
instrument per-instruction (that would dominate its own overhead).

### 3.4 Feed the profiler report into the benchmark harness
`tests/decompilebench/runner.py` already produces `wall_s`/`peak_rss_kb` per
sample. Add an option to also capture `RETDEC_BIN2LLVMIR_DIAG=1` output (or,
better, have the decompiler binary itself dump `ProfilingReport` as JSON
when an env var is set — `Profiler::report()` already returns a structured
object, it just needs a JSON serializer next to `toText()`) and merge it
into the same per-sample record. Right now getting a stage breakdown for one
binary is a manual `RETDEC_BIN2LLVMIR_DIAG=1 ./retdec-decompiler ...` run;
it should be a column in the same JSON the CI gate already reads.

### 3.5 Regenerate and commit the raw per-sample decompilebench result
```
python3 tests/decompilebench/runner.py \
  --decompiler build/linux/src/retdec-decompiler/retdec-decompiler \
  --corpus tests/decompilebench/corpus \
  --out results/decompilebench.json
```
Currently gitignored and discarded after the mean gets folded into
`baseline-2026-08.json`. Keep it — it's the only way to see whether the
6.17x is uniform or concentrated (e.g., does it scale with function count,
consistent with the serial per-function type-inference loop; or is it flat,
consistent with a fixed per-run cost like model-verification subprocess
spawns or corpus I/O).

**Do not proceed to fixing anything below until 3.1–3.5 exist and have been
run once.** Everything past this point is a hypothesis about where the time
goes; the point of this phase is turning hypotheses into measurements before
spending engineering time.

---

## 4. Phase 2 — Fix what the profiling turns up

In expected-payoff order, assuming §2's architecture read is roughly right
(confirm with §3 data before starting):

1. **Parallelize + cache the type-inference loop** using the exact pattern
   already proven out for the container/algo/sort/concurrency detectors:
   `analysis::parallelAnalysisEnabled()` gate,
   `analysis::FunctionAnalysisCache` sidecar keyed on
   `computeFunctionBodyHash()`. This is copying an established, working
   pattern from four call sites down in the same function — not new design.
2. **Question whether `buildSsaModule` needs to exist as a separate full
   rebuild.** bin2llvmir's own passes (`inst_opt_rda`, register
   localization) already compute SSA-adjacent structure over the same LLVM
   module. Investigate whether `ssa::SSAModule` can be built incrementally
   from data bin2llvmir already has, instead of a second independent pass
   over the whole module. This is the highest-effort, highest-payoff item on
   the list — don't start it without §3 data confirming SSA rebuild is
   actually expensive.
3. **Fix `balanced.json` / `quality.json` duplication** (§1) — cheap, do it
   regardless of what profiling shows.
4. **Gate or scope the OpenCL host-recovery scan** — cheap check (does the
   module contain any OpenCL-shaped patterns at all before running the full
   `OclHostRecovery::analyseModule`?) that avoids paying for a scan that's
   irrelevant to the overwhelming majority of binaries.
5. Re-run §3.5's benchmark after each individual fix and diff against the
   committed baseline — the CI gate infrastructure
   (`scripts/run_benchmarks.sh --compare 2026-08 --gate`) already exists for
   exactly this; use it rather than eyeballing numbers.

---

## 5. Phase 3 — xsimd / intrinsics

Scope this honestly. Most of bin2llvmir and llvmir2hll is CFG/dominator-tree/
SSA-renaming work — pointer-chasing, branch-heavy graph algorithms. xsimd
will not meaningfully speed that up, and it's not where §2's evidence points
anyway. Don't lead with this phase; it's a real but narrower win than fixing
the analysis phase.

Where it's a legitimate target:

- **`pattern_detect` / `yaracpp` byte-signature scanning** and any
  crypto-constant table scanning in `crypto_detect` — linear byte-buffer
  comparison, a textbook `xsimd::batch<uint8_t>` compare-and-reduce target.
- **Entropy computation / histogram building** if `crypto_detect` or
  `packer` does bulk byte-frequency counting over section data.
- **Bitset operations in liveness/dominance analysis**, if
  `register_localization` or `alias_analysis` represents live-sets as raw
  bitvectors rather than `llvm::BitVector` (LLVM's own BitVector already
  does word-at-a-time operations — check what's actually used before
  assuming this needs new code; don't reinvent something LLVM already
  provides).

Mechanics:
- Vendor `xsimd` the same way `eigen` already is under `deps/` (header-only,
  same pattern), gated behind a CMake option (`RETDEC_ENABLE_XSIMD` or
  similar) so it's opt-in and doesn't become a hard build dependency.
- C++ standard is already 17 (`CMakeLists.txt`); xsimd requires C++14+, no
  conflict.
- Sequence this after §3/§4, not before — instrument first, confirm these
  functions are actually hot for the binaries in your corpus, then vectorize
  the ones that are.

---

## 6. Phase 4 — Qwen3.5-9B on llama.cpp

This is further along than "needs to be built" — there's a real, working
integration already. What's missing is specific and blocking, not vague.

### Current state (verified from source)
- `deps/llamacpp/CMakeLists.txt` does a real `ExternalProject_Add`,
  fetching, building, and statically linking llama.cpp — not a stub.
- `src/neural/llama_inference.cpp` makes real API calls:
  `llama_load_model_from_file`, `llama_new_context_with_model`,
  `llama_tokenize`, `llama_decode`, `llama_token_to_piece`. This is a
  functioning, model-agnostic GGUF loader/generator, not scaffolding.
- `src/neural/model_verify.cpp` does SHA256 pinning against
  `RETDEC_NEURAL_MODEL_SHA256` (shells out to `sha256sum`).
- `src/neural/gates.cpp` does a real `-fsyntax-only` compile-check gate on
  refined output, plus an optional differential-execution gate
  (`RETDEC_NEURAL_DIFF_GATE`) that compiles and runs both original and
  refined C and diffs behavior.
- `src/neural/decompile_hook.cpp` runs refinement through up to 5 tiers
  (Naming → Comments → StructFields → IdiomRecovery → FullRewrite),
  configurable via `RETDEC_NEURAL_TIER_MAX` (default 3), gated off entirely
  unless `RETDEC_NEURAL_REFINE` is set.

### The actual blocker
`cmake/deps.cmake` pins:
```
LLAMACPP_URL = https://github.com/ggml-org/llama.cpp/archive/refs/tags/b3997.zip
```
Checked current Qwen3.5-9B GGUF release pages (Hugging Face, as of this
week): they're quantized against llama.cpp **b8185 / b9180 / b9222**. Qwen3.5
uses a `qwen35` architecture identifier with MTP (multi-token-prediction)
layers that llama.cpp only gained support for around **b9180**. `b3997` is
thousands of builds behind and has no knowledge of `qwen35` tensor naming —
`loadModel()` will fail outright, not run slowly. **This has to be fixed
before anything else in this phase is testable.**

### Task list
1. **Bump `LLAMACPP_URL` / `LLAMACPP_ARCHIVE_SHA256`** to a current tag
   (b9180+ minimum for MTP; check for a newer tag at implementation time,
   llama.cpp moves fast). Update the SHA256 to match. This is the
   prerequisite for every other item below.
2. **Fix the sampler.** `generate()` currently calls
   `llama_sampler_sample(nullptr, g_context, -1)` — a null sampler chain,
   effectively an unconfigured/default decode with no temperature, top-p,
   top-k, or min-p control despite `GenerationConfig` presumably carrying
   fields for these. Build a real `llama_sampler_chain` with Qwen's
   documented recommended sampling settings instead.
3. **Add MTP speculative decoding.** Qwen3.5 ships MTP layers specifically
   for this; llama.cpp added `--spec-type draft-mtp --spec-draft-n-max`
   support for it. This is close to a free 2–4x throughput win on the AI
   refinement path specifically — separate performance axis from the static
   decompilation speedups above, worth tracking separately in benchmarks.
4. **Stop clearing the KV cache between tiers on the same function.**
   `generate()` calls `llama_kv_cache_clear(g_context)` at the top of every
   call. Since tiers 1–5 operate on progressively-refined versions of the
   *same* function source, there's a real shared-prefix opportunity being
   thrown away on every tier beyond the first (`RETDEC_NEURAL_TIER_MAX`
   defaults to 3, so this is 3 full-prompt reprocessings per function today,
   up to 5 if tier max is raised).
5. **Make GPU offload an explicit build option, not silently absent.**
   Current `ExternalProject_Add` CMAKE_ARGS pass no `GGML_CUDA` flag — this
   is consistent with the stated CPU-only requirement, but should be a named
   CMake option (`RETDEC_NEURAL_GPU_OFFLOAD` or similar) so it's a deliberate
   choice per deployment target rather than an implicit default that's easy
   to lose track of.
6. **Update `model_verify.cpp`'s expected SHA256** for the specific
   Qwen3.5-9B GGUF quant settled on. Confirm it's the **text-only**
   checkpoint — Qwen3.5 also ships multimodal variants with separate
   `mmproj` vision-projector files that this integration has no use for and
   shouldn't accidentally load.
7. **Benchmark refinement latency separately from decompile speed.** These
   are different cost axes (static analysis vs. LLM inference) and
   shouldn't be conflated in the same wall-time number when reporting
   progress against §0's baseline.

---

## 7. Sequencing summary

```
Phase 0  Read results/baseline-2026-08.json properly (done — see §0)
Phase 1  Instrument (§3.1–3.5) — mechanical, no behavior change, do first
Phase 2  Fix Post-pipeline analysis phase per what Phase 1 finds (§4)
Phase 3  xsimd on confirmed-hot byte-scanning code only (§5)
Phase 4  Qwen3.5-9B: version bump is the blocker, then sampler/MTP/KV-reuse (§6)
```

Phases 3 and 4 are independent of each other and of Phase 2's outcome — they
can run in parallel once Phase 1's instrumentation exists, since they touch
different code (xsimd: static-analysis byte loops; Qwen3.5: neural/
subsystem, already gated off the default path). Phase 2 is the one gated on
Phase 1's measurements actually landing first.

---

## 8. Immediate checklist (before writing any new code)

- [ ] Confirm `RETDEC_NEURAL_REFINE` was **not** set when
      `baseline-2026-08.json`'s 1.492s figure was generated — if it was, the
      6.17x number includes AI refinement cost and the static-pipeline
      slowdown is smaller than it looks.
- [ ] Regenerate `results/decompilebench.json` (§3.5) and keep it — get the
      per-binary breakdown instead of just the mean.
- [ ] Run one representative binary with `RETDEC_BIN2LLVMIR_DIAG=1` and read
      `pipeline_wall_ms` vs `post_pipeline_analysis_wall_ms` — ten minutes,
      zero new code, tells you the split between "LLVM+bin2llvmir pipeline"
      and "everything in §2" today, before any instrumentation work.
- [ ] Fix `quality.json`/`balanced.json` duplication (§1) — five minutes,
      do it regardless of what else happens.
