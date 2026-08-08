# Master Upgrade Plan — Enhanced Retargetable Decompiler

**Author:** Odin Loch
**Repo:** `github.com/odin-loki/RetDec-Decompiler`
**Base:** upstream RetDec v5.0 (upstream dormant since 2022)
**Primary executor:** Composer 2.5, Cursor slow pool
**Date:** 2026-08-01

Single source of truth. Supersedes all previous plans and notes.

---

## Contents

- Part 1 — How to use this document
- Part 2 — Current state audit
- Part 3 — Decision register
- Part 4 — Phase 0: Foundations
- Part 5 — Phase 1: Licensing
- Part 6 — Phase 2: Measurement
- Part 7 — Phase 3: Dependencies
- Part 8 — Phase 4: Neural refinement
- Part 9 — Phase 5: Algorithms
- Part 10 — Phase 6: Robustness
- Part 11 — Phase 7: Performance
- Part 12 — Phase 8: Product, docs, release
- Part 13 — Phase 9: Library adoption
- Part 14 — Composer operating manual
- Part 15 — Copy-paste prompt library
- Part 16 — Automation to reduce your workload
- Part 17 — Master sequence
- Part 18 — Risk register

---

## Part 1 — How to use this document

Every task carries an executor tag:

- **[C]** — Composer 2.5, unsupervised, under the Part 14 guardrails.
- **[C+]** — Composer with a tight spec; you review each diff.
- **[H]** — You, or a frontier model. Handing these to Composer costs days and produces plausible-looking wrong code.

The tags are the load-bearing part of this plan. A cheap model on a 1,500-file C++ tree succeeds or fails on scoping, not on capability.

Part 15 contains ready-to-paste prompts for every **[C]** and **[C+]** task, in order. That appendix is the actual labour saving — most days you should be copying a prompt, reading a diff, and moving on.

### The single most important instruction

**Ship at Step 26 of the master sequence, regardless of what is unfinished.** Everything after it is the next release. The most likely way this plan fails is not technical — it is treating a roadmap as a completion checklist and never shipping.

---

## Part 2 — Current state audit

Measured from the repository, not assumed.

### Scale

| Metric | Value |
|---|---|
| Repository size | 55 MB |
| Source files | ~1,500 `.cpp`, ~1,350 `.h` |
| Commits | 14 |
| Test files | 378 |
| GTest cases | ~6,800 |
| Registered ctest targets | 72 |
| `docs/` files | 28 |
| Largest module | `src/llvmir2hll/` (~95k lines, upstream) |

### Continuous integration

All five workflows are `workflow_dispatch` only. Nothing runs automatically except release-on-tag.

| Workflow | Current trigger |
|---|---|
| `ci-smoke.yml` | manual |
| `ctest-linux.yml` | manual |
| `ctest-windows.yml` | manual |
| `perf-nightly.yml` | manual |
| `release-installers.yml` | tags (correct) |

You have ~6,800 tests that nothing triggers.

### Dependency pins (`cmake/deps.cmake`)

| Dependency | Pinned | Current upstream |
|---|---|---|
| LLVM | `avast/llvm` @ `a776c2a` (LLVM 8 era) | 22.1.8 |
| Capstone | `5.0-rc2` | 5.0.9 stable |
| YARA | `v4.2.0-rc1` | 4.5.8 |
| yaramod | commit `aa06dd4` | v4.8.1 |
| Keystone | commit `d7ba8e3` | — |
| googletest | commit `90a443f` | current release |
| OpenSSL | 3.2.6 | current-ish |
| zlib | 1.3.1 | current |
| retdec-support | `2019-03-08` package | unchanged upstream |

Two release candidates are shipping in a product being offered to defence buyers.

### LLVM coupling

| Metric | Count |
|---|---|
| Files referencing `llvm::` | 314 |
| Distinct LLVM headers included | 116 |
| `ModulePass`/`FunctionPass` subclasses | 41 |
| `getPointerElementType` call sites | 25 |
| Implicit-type `CreateLoad`/`CreateGEP` | 32 |
| Files using legacy PassManager | 4 |

RetDec does not use LLVM codegen or backends at all — Capstone lifts, LLVM holds IR and runs cleanup passes (`instcombine` ×42, `simplifycfg` ×31, `mem2reg`, `gvn`, `sccp`, `licm`, `adce`, `jump-threading`, loop passes).

### Neural engine

`src/qwen3/` is 7,742 lines — attention, MoE, tokenizer, sampler, weights, CUDA kernel. Callers outside the module: two GUI files only (`ai_assistant_panel.cpp`, `settings_dialog.cpp`). It is not in the decompilation pipeline, and per your report it never worked. **Delete it.**

### Robustness

Fuzzers exist for managed formats only: `fuzz_wasm`, `fuzz_pyc`, `fuzz_jvm_class`, `fuzz_dex`. Nothing fuzzes PE, ELF, Mach-O, the unpacker, DWARF/PDB, or the Capstone lifting boundary — the surfaces that actually eat hostile input.

`scripts/run_asan.sh`, `run_valgrind.sh`, `run_coverage.sh` exist. No workflow calls them.

### Versioning

`CMakeLists.txt:8` reads `VERSION 5.0`, inherited verbatim from upstream. `release-installers.yml` defaults to `"5.0"`. Your build is indistinguishable from stock RetDec 5.0 by version string.

### Where you sit against the field

Published figures, for calibration rather than discouragement:

| Metric | Best in field |
|---|---|
| Pointer resolution, -O2 | Hex-Rays 40.6%; Ghidra 25.9%; angr 12.2% |
| Struct recovery, -O2 | Hex-Rays 17.1%; Ghidra 15.0%; Binary Ninja 14.8% |
| Re-executability, baseline | 22–26% (RetDec 5.0 is a named baseline) |
| Re-executability, LLM-refined | 43–50% |

Best-in-world struct recovery is 17%. Nobody has solved this. That is the strategically useful fact in this entire document: you are not entering a field where a mature answer exists.

---

## Part 3 — Decision register

Decisions already made in this project. Recorded so they stop being re-litigated.

| # | Decision | Status |
|---|---|---|
| D1 | Inference backend is llama.cpp, not a hand-written engine | **Settled** |
| D2 | Model is Qwen3.5-9B GGUF, CPU-capable | **Settled** |
| D3 | `src/qwen3/` is deleted, not kept as a fallback backend | **Settled** |
| D4 | LLVM stays on the Avast LLVM 8 fork for this release cycle | **Settled** |
| D5 | Licence set condensed to LICENSE / LICENSE-AGPL / LICENSE-COMMERCIAL / NOTICE | **Settled** |
| D6 | Neural output is gated and never replaces the deterministic artefact | **Settled** |
| D7 | Product positioning: pseudocode decompiler, or specification-extraction tool? | **Settled — (b) specification-extraction** ([docs/internal/D7_DECISION.md](docs/internal/D7_DECISION.md)) |

### D7 is the decision that unblocks the most work

Your new analysis passes are wired as *post-decompile analysis* feeding the semantic export. They do not replace `llvmir2hll`. The C you emit still comes from upstream's 2019 backend, so every algorithm improvement in Part 9 lands in your semantic export, not in your pseudocode. Diffed against stock RetDec on a test binary, your C looks nearly identical.

Two products are possible:

**(a) A better pseudocode decompiler.** Requires rewiring emission through your own SSA/type/structuring stack, or adopting rellic (Part 13). Large. Competes with Hex-Rays and Ghidra on ground where they have a twenty-year head start, and where the ceiling is 17% anyway.

**(b) A specification-extraction tool that contains a decompiler.** Algorithm recovery plus the air-gapped local model are the product; pseudocode is a supporting artefact. No competitor ships a self-contained local model — every LLM-decompilation result in the literature assumes cloud API access, which is disqualifying for defence buyers.

**(b) is the defensible choice.** Versioning, README, whitepaper, benchmark emphasis, and demo script all depend on it. Write the answer in one sentence at the top of the README before starting Part 12.
---

## Part 4 — Phase 0: Foundations

Nothing else is safe until these exist.

### 0.1 Replace `.cursorrules` **[H]** — 10 minutes

Do this before giving Composer anything. The current file instructs the agent to never stop, never ask, and to mark failures and move on. Applied to a dependency migration, that means Composer hits a build break, marks it failed, and stacks six more changes on a broken tree. You lose the ability to bisect, and the characteristic recovery behaviour of a cheap model under autonomous-continuation instructions is to delete the failing test.

The replacement file is written and ready. Install it.

### 0.2 Turn CI on **[C+]** — 3 days

Tier by cost rather than disabling everything:

| Workflow | New trigger | Target runtime |
|---|---|---|
| `ci-smoke.yml` | every push | < 10 min |
| `ctest-linux.yml` | every PR to main | < 45 min with ccache |
| `ctest-windows.yml` | nightly cron | — |
| `perf-nightly.yml` | weekly cron | — |

Add `ccache` and a cached `deps/` prefix keyed on `cmake/deps.cmake` hash. The dependency build is what makes this heavy, and it only changes when you change a pin.

**Done when:** a PR containing a deliberately broken test gets a red check with nobody clicking anything.

### 0.3 Reproducible baseline **[C]** — 2 days

`docker/baseline.Dockerfile`: clean clone to working binary on Ubuntu 24.04, pinned compiler and CMake, zero manual steps.

The LLVM 8 fork is the most fragile thing in the tree. Once pins start moving you need a known-good image to diff against, and "it built last week" is not that.

**Done when:** it builds twice from a cleared cache.

### 0.4 Freeze the tag **[H]** — 5 minutes

```
git tag -a baseline-2026-08 -m "Frozen pre-upgrade baseline"
git push --tags
```

Every number you publish from here is a delta against this tag.

### 0.5 Delete the dead engine **[C]** — 1 hour

Remove `src/qwen3/`, `include/retdec/qwen3/`, `tests/qwen3/`, the GUI panel's dependency on it, and the CUDA/OpenCL build branches that exist only to serve it. Keep the CUDA toolchain detection — Part 8 may reuse it for llama.cpp's CUDA backend.

7,742 lines of non-working code is not an asset. It is a maintenance liability and a diligence question you would rather not answer.

---

## Part 5 — Phase 1: Licensing

Files are written. This phase is installation, not authorship.

### 5.1 Install the condensed set **[C]** — 1 hour

Run `install-licence-files.sh` from the repo root. It extracts the verbatim AGPL text from your existing `LICENSE` into `LICENSE-AGPL` rather than re-typing it, so the text stays byte-identical to what you have already shipped.

Result: `LICENSE` goes from ~1,000 lines to 26.

| File | Content |
|---|---|
| `LICENSE` | Dual-licence summary, 26 lines |
| `LICENSE-AGPL` | Verbatim AGPL-3.0, extracted |
| `LICENSE-COMMERCIAL` | Section 7 terms, condensed |
| `NOTICE` | Third-party attribution, deduplicated by licence |

### 5.2 Follow-ups the script lists **[C]**

- `install(FILES LICENSE LICENSE-AGPL LICENSE-COMMERCIAL NOTICE ...)` in `CMakeLists.txt`.
- Populate `share/licenses/` at install time from dependency source trees (Apache-2.0, MPL-2.0, Zlib, GPL-2.0 texts).
- One lineage sentence near the top of `README.md`.
- The old `LICENSE` had a UTF-8 BOM and double-encoded `§` characters rendering as `Â§`. The script strips the BOM; the new files are clean.
- Bank details were published in the old `LICENSE`. Removed.

### 5.3 The Keystone constraint **[H]** — packaging rule

Keystone is **GPL-2.0**, confirmed from upstream `COPYING`. It is gated behind `RETDEC_ENABLE_KEYSTONE` and links only into `src/capstone2llvmirtool/` and its tests — not the decompiler library, not the shipped binaries.

Rule: **`capstone2llvmirtool` is excluded from any distribution offered under the commercial licence.** Ship it separately under GPL-2.0, or not at all. Add a CI assertion that the commercial package manifest does not contain it.

### 5.4 Commercial terms review **[H]**

The condensed `LICENSE-COMMERCIAL` softened three clauses. Restore them only deliberately:

- Annual fee was 5–25% of *total annual income*. At the top brackets that asks a company for a quarter of its revenue. Now 5% of revenue attributable to the software.
- Mandatory open-sourcing of research and quarterly R&D reports were required *even under the commercial licence*. Those two clauses alone stop most enterprise and defence buyers at legal review — a commercial licence that still constrains their research is not signable.
- A 5% gross-profit royalty stacked on top of the annual fee. Removed.

The purpose of a commercial tier is to be signable. Aggressive is fine; unsignable is not.

Two hours with an IP solicitor on the dual-licence structure is cheap relative to what it protects, given the buyer profile. Get it blessed once, then stop thinking about it.

---

## Part 6 — Phase 2: Measurement

This converts claims into evidence. Do not skip it to reach the interesting parts.

### 6.1 External benchmark **[C+]** — 2 weeks

Build `tests/decompilebench/` around DecompileBench (arXiv 2505.11340), which measures recompilation success and coverage equivalence across optimisation levels.

Runner:
1. Pull the corpus.
2. Run your binary at `-O0`, `-O2`, `-O3`.
3. Record syntax validity, recompile success, coverage equivalence, wall time, peak RSS.
4. Emit `results/<git-sha>.json` on a stable schema.

Run **stock RetDec 5.0** through the identical harness. Without the two-column table you cannot demonstrate your fork adds anything.

### 6.2 Your own metric **[H]** — 2 weeks

Nobody measures algorithm recovery. That is why it is yours to define. Your `algo_recover`, `sort_detect`, `concurrency_detect`, `container_detect`, and `serial_detect` modules attempt something no competitor does.

- **Corpus:** 200+ binaries built from sources with known algorithmic content — sorts, hash tables, ring buffers, mutex and atomic patterns, serialisation formats — across `-O0/-O2/-O3`, GCC and Clang.
- **Ground truth:** JSON labels generated from source, never from the decompiler.
- **Metrics:** precision, recall, F1 per class, per optimisation level.

Write the metric definition down *before* running it. A metric defined after seeing results is worth nothing to a reviewer, and you will know it.

### 6.3 Publish the weak numbers too **[H]**

A table showing parity with stock RetDec on pseudocode, alongside a strong algorithm-recovery result, is a stronger artefact than a vague superiority claim. A quantified weakness you volunteered reads as rigour. An unquantified one a reviewer finds reads as something else.

**Done when:** `scripts/run_benchmarks.sh` produces both tables unattended from a clean checkout.

---

## Part 7 — Phase 3: Dependencies

Only now, with regressions detectable.

### 7.1 The bumps **[C]** — 1–2 weeks

| Dependency | From | To | Tag |
|---|---|---|---|
| Capstone | `5.0-rc2` | 5.0.9 | **[C]** |
| YARA | `v4.2.0-rc1` | 4.5.8 | **[C+]** |
| yaramod | `aa06dd4` | v4.8.1 | **[C+]** |
| googletest | `90a443f` | current | **[C]** |
| Keystone | `d7ba8e3` | a tag | **[C]** |
| OpenSSL | 3.2.6 | verify | **[C]** |
| zlib | 1.3.1 | none | — |

Procedure, one dependency per branch, never two:

```
cmake --preset <preset> && cmake --build build -j
ctest --test-dir build --output-on-failure
scripts/run_benchmarks.sh --compare baseline-2026-08
```

A non-zero benchmark delta in *either* direction stops the train. An unexplained improvement needs the same scrutiny as a regression — it usually means something is being skipped.

The two release candidates go first: cheapest fix, most visible in any review.

### 7.2 Regenerate `retdec-support` **[H]** — 1 week

The signature and type database is pinned to a `2019-03-08` snapshot. Seven years of compiler versions, runtime libraries, and PE ordinals are simply absent — a silent, ongoing accuracy loss on every modern binary.

Rebuild with `scripts/retdec-signature-from-library-creator.py` against current MSVC, GCC, Clang, and MinGW runtimes.

**This is probably the highest accuracy-per-effort item in the entire document.** It is a scripted regeneration, not research.

### 7.3 The LLVM wall **[H]** — deferred, with reason

Do not attempt in this cycle. Put `deps/llvm/` in `.cursorrules` as forbidden, because on a broad "modernise dependencies" prompt Composer will try.

The non-obvious reason for deferring: from LLVM 17, **opaque pointers** remove pointee types from the IR. For a compiler that is a simplification. For a decompiler it deletes exactly the information you are trying to recover. Migrating today would plausibly make your output *worse*.

That blocker lifts once Retypd lands (Part 9), because a constraint-based type system recovers types from use patterns rather than IR annotations. **Sequence: Retypd first, LLVM second.** Doing it the other way means fighting a regression you caused yourself.
---

## Part 8 — Phase 4: Neural refinement

The flagship differentiator.

### 8.1 Architecture

```
deps/llamacpp/                        pinned llama.cpp
include/retdec/neural/inference.h     Inference interface
include/retdec/neural/refiner.h       refinement pass
include/retdec/neural/gates.h         verification gates
src/neural/llama_inference.cpp        libllama backend
src/neural/mock_inference.cpp         deterministic test double
src/neural/refiner.cpp                orchestration
src/neural/prompts.cpp                prompt construction
src/neural/gates.cpp                  compile / differential / structural
tests/neural/                         unit + integration
```

Integration point: `src/retdec/retdec.cpp:955`, immediately after `decompileToLlvmIr(config, "retdec-llvmir2hll")` completes C emission.

The `Inference` interface exists for testability, not backend portability. Tests link the mock; production links llama.cpp. There is no second real backend.

### 8.2 The model **[H]** — decided

Qwen3.5-9B, GGUF, CPU-capable.

Supporting figures: on the Artificial Analysis Intelligence Index the 9B scores 32 against Qwen3 VL 8B's 17 — a large generational jump at the same size. At 4-bit it needs roughly 6 GB (the 4B roughly 3 GB). Context is 262,144 natively, extensible to 1M via YaRN, which matters when a prompt carries a function plus recovered types plus call-graph context.

Three practical constraints:

**Thinking mode will destroy CPU throughput.** These are reasoning models and they are verbose — the sub-10B models consumed 230–390M output tokens running the Intelligence Index, more than Gemini 3.1 Pro (57M) or GPT-5.1 high (69M). Qwen3.5 Small disables thinking by default. Keep it off for naming and commentary; consider enabling only for idiom recovery.

**Tier the models, don't pick one.** A 9B at Q4 on CPU runs roughly 5–10 tok/s. Across thousands of functions that is hours. Use 2B or 4B for tiers 1–3 and reserve 9B for hard cases. Both are GGUF through the same code path — a config switch, not a code change.

**These are multimodal models with separate mmproj vision files.** You do not want vision. Load the text-only GGUF and ignore mmproj, or you carry weight you never use. Note that no Qwen3.5 GGUF currently works in Ollama for exactly this reason — llama.cpp-compatible backends are required, which validates the D1 decision.

### 8.3 llama.cpp integration notes **[C+]** — 1 week

- **Licence:** MIT. Add to the existing MIT block in `NOTICE`; no new licence text required.
- **Pin a commit, not a branch.** llama.cpp breaks its C API regularly and without deprecation periods. Pin in `cmake/deps.cmake` like everything else and bump deliberately.
- **Link `libllama` directly**, not the server binary. In-process is the whole point.
- **`llama_backend_init` runs exactly once per process.** Guard with `std::once_flag`.
- **Silence llama.cpp logging** via `llama_log_set`. The decompiler owns stderr.
- **Clear KV state between functions.** Cross-function contamination in a refinement pass produces confident nonsense.
- **API volatility:** whenever the pin moves, diff `deps/llamacpp/include/llama.h` against the previous pin before touching anything else. Do not let Composer guess at replacement symbols — that is an escalation trigger.

### 8.4 Feed it context, not just C **[H]** design, **[C+]** build — 1 week

The published gains come from context. You have what a generic refiner does not: recovered types, `algo_recover` classifications, calling-convention decisions, CFG structure, demangled names, RTTI, DWARF/PDB where present.

Serialise all of it into the prompt. Refining bare C is the weak version and will underperform published results.

Budget the context. At 16k tokens you fit a function plus substantial context; at 262k you fit far more but pay for it in KV cache and latency. Start at 16k and measure.

### 8.5 Verified, not trusted **[H]** — 2 weeks

The critical design decision in this document. A model rewriting decompiler output can silently invent semantics, and in your market a confidently wrong function summary is worse than no summary.

Every neural edit passes gates before reaching the user:

| Gate | Check | On failure |
|---|---|---|
| **Compile** | Refined C compiles | Discard edit |
| **Structural** | CFG edit distance from original within threshold | Discard edit |
| **Differential** | Compile both, execute against identical inputs, compare observable behaviour | Discard edit |
| **Fallback** | — | Return deterministic output silently |

The D-Helix approach (USENIX Security 2024) is the reference for symbolic differential testing of decompiler output. Triton (Part 13) gives you most of it off the shelf.

Emit both artefacts:

```
output.c                      deterministic, untouched, auditable
output.refined.c              neural
refinement-manifest.json      every change, which gates passed, model identity
```

Defence customers need the deterministic artefact to remain provably untouched. This is not a nicety; it is what makes the feature acceptable rather than disqualifying.

### 8.6 Task tiers **[C+]** — 3 weeks

Whole-function rewriting is where hallucination lives. Ship in order:

| Tier | Task | Risk | Ship |
|---|---|---|---|
| 1 | Variable and function naming | Near zero | First |
| 2 | Comment and summary generation | Zero semantic | First |
| 3 | Struct field naming over recovered layout | Low | First |
| 4 | Idiom recovery (memcpy loop, strlen, hash) | Medium | After benchmarks |
| 5 | Full function rewriting | High | Flag, off by default |

**Tiers 1–3 deliver most of the perceived quality jump at almost none of the risk.** Ship those, benchmark, then decide about 4.

### 8.7 Offline is the headline **[C]** — 2 days

Every LLM-decompilation result in the literature assumes cloud API access. In-process, air-gapped, zero network is your differentiator, and it is a procurement checkbox rather than a marketing line.

Make it verifiable: a `--no-network` assertion, a `RETDEC_NEURAL_OFFLINE_ONLY` build mode compiling out every egress path, and a documented statement. llama.cpp performs no network I/O at inference, but assert it anyway so you can state it rather than claim it.

### 8.8 Model provenance **[H]** — 1 day

Pin the GGUF SHA-256 in shipped configs and verify at load. Document which weights, which quantisation, which licence. Verify the Qwen3.5 model licence for the specific release before shipping — Qwen models have generally been Apache-2.0, but confirm it.

Defence buyers ask about model supply chain. "We ship a GGUF from somewhere" is not an answer.

### 8.9 Performance **[C+]** — 1 week

Cold-loading weights per invocation will dominate wall time. Add a persistent model cache and a daemon mode. `use_mmap` keeps resident memory down when several processes share weights. Measure before it becomes a complaint.

---

## Part 9 — Phase 5: Algorithms

Ranked by value per unit of risk.

### Tier 1 — do these **[C+]** — 3–5 weeks total

**9.1 Dominator tree: Lengauer-Tarjan → Semi-NCA**
`src/ssa/domtree.cpp`. LLVM switched for good reasons: faster in practice, supports incremental updates. Self-contained, and your existing SSA verifier checks the result.

**9.2 SSA construction: Cytron → Braun et al. 2013**
`src/ssa/phi_placement.cpp`, `src/ssa/ssa_rename.cpp`. "Simple and Efficient Construction of SSA Form" builds SSA directly without a separate dominance-frontier pass — standard in modern JITs and lifters. Contained blast radius.

**9.3 Points-to: Steensgaard → Andersen with wave propagation**
`src/alias_analysis/steensgaard.cpp`. Steensgaard 1996 is almost-linear but the least precise points-to analysis in common use. In a decompiler, precision drives readability. Keep Steensgaard behind a flag as the large-binary fast path.

### Tier 2 — high value, high effort **[H]**

**9.4 Type inference: union-find → Retypd** — 3–6 months
`src/type_inference/`. Constraint-based, handles subtyping and polymorphism; Ghidra adopted it. Your largest capability gap, directly serving the specification-mining thesis, and the unblocker for the LLVM migration.

Scope expectations honestly: Retypd-based approaches scored 3.74% struct recovery at `-O2` in the SURE'25 evaluation, against Hex-Rays at 17.08%. Target "meaningfully better than union-find", not "solved". If the benchmark after three months shows a small gain, that is the expected outcome, not a failure — but it should inform whether months four through six are worth it.

**9.5 Control-flow structuring: SESE+goto → SAILR** — 3–6 months
`src/cfg_structure/`. SAILR (USENIX Security 2024, now in angr) and the earlier DREAM line produce goto-free output.

*"Our output has no gotos"* is the strongest five-second demo line available to you. A reviewer sees it instantly without understanding a word of the algorithm. If you only do one Tier 2 item before shipping, this is the one with the better demo-to-effort ratio — though Retypd has the better long-term payoff.

### Tier 3 — the structural question

Covered in Decision D7. Everything in Tiers 1 and 2 improves your *semantic export*, not your pseudocode, until D7 is resolved and acted on.

---

## Part 10 — Phase 6: Robustness

Decompilers eat hostile input by definition. Table stakes for defence buyers, currently thin.

### 10.1 Extend fuzzing **[C]** — 1 week

Existing fuzzers cover managed formats only. Add harnesses for the surfaces that actually take adversarial input:

- PE, ELF, Mach-O, COFF parsers (`src/fileformat/`)
- The unpacker (`src/unpackertool/`)
- Debug info parsers (DWARF, PDB)
- The Capstone lifting boundary (`src/capstone2llvmir/`)

Malformed-binary parsing is where the exploitable bugs live. **FuzzTest** (Part 13) integrates with GTest so these land in the suite you already have.

### 10.2 Sanitizer CI **[C]** — 2 days

`run_asan.sh`, `run_valgrind.sh`, `run_coverage.sh` exist but nothing calls them. Add a weekly ASan + UBSan job over the sample corpus, and publish the coverage number.

### 10.3 Crash corpus **[C]** — 2 days

Every fuzzer crash becomes a permanent regression test in `tests/crash_corpus/`, wired into ctest. This is how a fuzzing programme compounds rather than repeating itself.

### 10.4 Timeouts and resource caps **[C+]** — 1 week

A decompiler that hangs on a hostile binary is a denial-of-service surface. Per-function and per-module wall-clock limits, memory ceilings, and graceful degradation to partial output rather than hanging or dying.

Add the same for the neural pass — an unbounded generation loop on a pathological function is the same failure with a bigger constant.
---

## Part 11 — Phase 7: Performance

### 11.1 Establish the profile **[C]** — 3 days

`perf-nightly.yml` exists and never runs. Turn it on, publish flame graphs. You cannot optimise what is not measured, and the distribution is usually more lopsided than expected.

### 11.2 Parallelism **[C+]** — 2 weeks

Function-level decompilation is embarrassingly parallel, and RetDec has historically been single-threaded through much of the pipeline. On a 16-core workstation this is likely the largest single wall-clock win available, and it costs far less than any algorithm change.

Watch the LLVM context: `LLVMContext` is not thread-safe. Either one context per worker, or parallelise only the post-LLVM stages first — which is the safer starting point and covers your new analysis modules and the neural pass.

### 11.3 Incremental decompilation **[H]** — 2 weeks

Cache per-function results keyed by content hash. Re-analysing a 200 MB binary after a small change should not be a full rerun. Directly serves the interactive GUI workflow, and makes the neural pass tolerable by only refining what changed.

### 11.4 Neural batching **[C+]** — 1 week

Batch prompts across functions rather than issuing one generation per function. llama.cpp supports batched decode, and it is the difference between hours and minutes on CPU.

---

## Part 12 — Phase 8: Product, docs, release

Blocked on Decision D7.

### 12.1 Single-source the version **[H]** decide, **[C]** execute — 3 days

Two clean options:

- **Lineage-preserving:** `5.1.0`, `5.2.0`… honest about the base, permanently anchored to a dormant project.
- **Clean break:** distinct product name, own semver from `1.0.0`.

**Recommendation:** clean break, conditional on committing to D7 option (b). The product you are selling is not a RetDec release.

Define the version once in `CMakeLists.txt` and derive everywhere — installers, GUI about box, `--version`, Doxygen, workflow defaults. Grep and eliminate every hardcoded `5.0`, including the `release-installers.yml` input default.

### 12.2 Split the docs tree **[C]** — 1 day

28 files mixing shipped documentation with working notes (`PIPELINE_REDESIGN_TODO.md`, `GUI_POLISH.md`, `GUI_ROADMAP.md`).

```
docs/            user- and integrator-facing
docs/internal/   roadmaps, TODOs, working notes
```

### 12.3 Documents, in priority order **[C+]** — 1 week

1. **`README.md`** — what it is, what it adds, what it does not do, lineage sentence, D7 answer in one line.
2. **`docs/algorithm_reference.md`** — the highest-value document you own. Your source files already carry excellent algorithm headers with literature citations; extract them mechanically into one table of algorithm, citation, source file, complexity. Ideal Composer task, and the result reads as serious engineering.
3. **`docs/BENCHMARKS.md`** — Phase 2 output, methodology, honest weaknesses.
4. **`docs/NEURAL_REFINEMENT.md`** — architecture, gates, offline guarantee, model provenance. This is the document that sells the product.
5. **`CHANGELOG.md`** — Keep a Changelog format, with the 12.1 decision applied.
6. **`SECURITY.md`** — currently 937 bytes. Defence buyers need a real disclosure process and a stated response window.
7. **`docs/COMMERCIAL_WHITEPAPER.md`** — rewrite last, once D7 is settled and benchmarks exist.

### 12.4 Release automation **[C]** — 2 days

`release-installers.yml` should take the version from the git tag, not a workflow input default. Manual version entry drifts.

Add to the release job: licence file installation check, commercial-package manifest assertion (no `capstone2llvmirtool`), model SHA verification, and benchmark table regeneration.

### 12.5 The demo script **[H]** — 1 day

Underrated. Write a five-minute scripted demo: one binary, deterministic output, refined output side by side, algorithm-recovery report, network interface disabled on camera. Rehearse it. Most technical products lose the room in the first ninety seconds of a live demo, not in the architecture review.

---

## Part 13 — Phase 9: Library adoption

Each of these removes work you would otherwise do yourself. Verify every licence against the specific release before committing.

### 13.1 Attack your weakest link first **[H] evaluate, [C+] adopt**

**rellic** (Apache-2.0, Trail of Bits) — LLVM IR to C. A genuine alternative to `llvmir2hll`, which is your 2019 bottleneck and the reason your output resembles stock RetDec.

Read it even if you do not adopt it. It is the closest thing to a modern, maintained answer to the problem you are stuck on, and evaluating it is how you make D7 option (a) a real choice rather than a hypothetical one.

Companions in the same stack: **anvill** (lifted-binary IR with type recovery) and **remill** (instruction semantics to LLVM IR).

### 13.2 Binary parsing **[C+]**

- **LIEF** (Apache-2.0) — PE/ELF/Mach-O/DEX parsing *and modification*, actively maintained. RetDec's `fileformat` is large, aging, and does less. Adopt incrementally behind your existing interface rather than in one cut.
- **raw_pdb** (BSD-2) — fast, dependency-free PDB reader. PDB via LLVM is heavy by comparison.

### 13.3 Disassembly **[C+]**

- **Zydis** (MIT) — x86/x64 only, faster and more accurate than Capstone there, and tiny. Use as an x86 fast path, not a replacement.
- **bddisasm** (Apache-2.0, Bitdefender) — rigorous x86 decoder. Best used as a cross-check oracle in differential testing rather than in the main path.

### 13.4 Solving and verification **[H]**

For the differential gate, jump-table resolution, and MBA deobfuscation:

- **Bitwuzla** (MIT) — SMT specialised on bitvectors, which is what binary analysis is. Usually faster than Z3 on this workload.
- **Z3** (MIT) — broader, better documented, more examples. Start here, move to Bitwuzla if performance bites.
- **Triton** (Apache-2.0) — symbolic execution, taint tracking, semantics for x86/ARM. Gives you most of the D-Helix-style differential testing off the shelf, which is a large chunk of Part 8.5.

### 13.5 Semantic export **[C+]**

Your spec-extraction output needs a real schema, not ad-hoc JSON:

- **Cap'n Proto** (MIT) or **FlatBuffers** (Apache-2.0) — versioned, zero-copy, language-agnostic. Matters the moment a customer wants to consume your output from Python.

This is a small change that makes the product look like a platform rather than a tool.

### 13.6 Function similarity **[H]**

- **ONNX Runtime** (MIT) — run embedding models for binary function similarity alongside llama.cpp. Relevant to algorithm recovery: nearest-neighbour over a labelled corpus is a strong complement to your hand-written detectors, and may outperform them.

### 13.7 Quality of life **[C]**

- **spdlog** / **fmt** (MIT) — you have a lot of ad-hoc output formatting.
- **CLI11** (BSD-3) — argument parsing.
- **FuzzTest** (Apache-2.0) — property-based testing that integrates with GTest, so Part 10 lands in the suite you already have.

### 13.8 Licence hazards — do not link

| Library | Licence | Why it matters |
|---|---|---|
| Unicorn Engine | GPL-2.0 | Tempting for emulation-based unpacking; poisons the commercial licence |
| Miasm | GPL | Same |
| UPX | GPL | Same |
| Keystone | GPL-2.0 | Already in tree — see 5.3, tool-only, excluded from commercial packages |

Same category as Keystone: usable only in tools you ship separately, never linked into the product.

### 13.9 The tension worth naming

You said earlier you wanted fewer dependencies. This phase adds several. The distinction that resolves it:

**Reduce coupling, not dependency count.** Put your own interfaces in front of commodity infrastructure so you are not welded to anyone's API — that is what the LLVM 8 situation cost you. Then let the commodity below the interface be someone else's maintenance burden.

Writing correct disassembly, inference, SMT solving, or ELF parsing is not where your advantage lies. Every one of those you take on yourself is time not spent on algorithm recovery, which is the thing nobody else is doing.
---

## Part 14 — Composer operating manual

Process matters as much as plan. This section is what makes a cheap model on a large C++ tree viable rather than destructive.

### 14.1 The rules file

Already written. Install before anything else. Its four load-bearing sections:

**Hard boundaries** — never touch `deps/llvm/`, never attempt an LLVM bump, never modify `deps/` outside pinned URL and SHA lines, never change a public header to make an implementation compile, never edit unnamed files.

**Verification** — build after every C++ edit; on breakage stop and report the first error verbatim rather than attempting a third repair; never claim complete without pasting ctest output.

**Tests are sacred** — never delete, disable, `SKIP`, `DISABLED_`-prefix, or loosen a test. A failing test is a finding, and reporting it is a successful outcome.

**Honesty** — say when unsure and stop; never invent a file path, symbol, or API not seen in the repository; never claim a build passed without output.

The tests rule is the one that matters most. Test-deletion to reach green is the characteristic failure of cheap models under autonomous-continuation instructions, and it silently destroys the regression net this entire plan depends on.

### 14.2 Task sizing

- ✅ "Bump `CAPSTONE_URL` in `cmake/deps.cmake` to the 5.0.9 tag, update `CAPSTONE_ARCHIVE_SHA256` to that archive's real hash, rebuild, report the first error if any."
- ✅ "In `src/ssa/domtree.cpp`, replace the Lengauer-Tarjan `link`/`eval` implementation with Semi-NCA. Keep `include/retdec/ssa/ssa.h` byte-identical. All existing SSA tests must pass unchanged."
- ❌ "Modernise the dependencies."
- ❌ "Improve the type inference."

**If you cannot state the acceptance test in one sentence, the task is too big.**

### 14.3 Always supply the verification command

End every prompt with the exact command that proves success. Composer is markedly more reliable when the criterion is executable rather than described. `ctest -R ssa --output-on-failure` beats "make sure SSA still works" by a wide margin.

### 14.4 Context discipline

Do not let it grep the tree. `src/llvmir2hll/` alone is ~95k lines and will crowd out everything relevant. Name files explicitly. More than about eight files in context means it is an **[H]** task in disguise.

### 14.5 Escalation triggers

Take over when:

- The same build error survives two repair attempts.
- It proposes changing a public header to make an implementation compile.
- It suggests disabling, deleting, or skipping a test.
- A diff you scoped as mechanical exceeds ~400 lines.
- It edits files you did not name.
- It guesses at a llama.cpp API symbol rather than reading `llama.h`.

### 14.6 Cost model

The slow pool is fine. Your bottleneck is review time per diff, not model latency — you will spend far longer reading output than waiting for it.

Therefore: **make the diffs small.** Ten 40-line commits you review properly beat one 400-line commit you skim, and the bug is always in the skimmed one.

### 14.7 What to escalate to a frontier model

Reserve paid credits for five things:

1. Retypd (9.4)
2. SAILR (9.5)
3. The neural gate architecture (8.5)
4. rellic evaluation (13.1)
5. Any benchmark regression with no obvious cause

---

## Part 15 — Copy-paste prompt library

Paste these into Composer in order. Each is scoped to a single reviewable diff. This appendix is the labour saving: most days should be copy prompt, read diff, next.

### P1 — CI smoke

```
Modify .github/workflows/ci-smoke.yml so it runs on every push and pull
request, in addition to workflow_dispatch. Add ccache via
hendrikmuhs/ccache-action and cache the deps/ build prefix keyed on the
hash of cmake/deps.cmake. Do not change any build steps. Target under
10 minutes. Show me the diff only.
```

### P2 — ctest on PRs

```
Modify .github/workflows/ctest-linux.yml to run on pull_request to main,
keeping workflow_dispatch. Reuse the ccache and deps cache from
ci-smoke.yml. Do not change test selection. Show the diff only.
```

### P3 — Baseline image

```
Create docker/baseline.Dockerfile: Ubuntu 24.04, GCC 13, CMake 3.28,
Ninja, Python 3.12. Clone nothing — assume the repo is COPYed in. Build
the project with the default preset and run ctest. It must succeed from
a clean cache with zero manual steps. Show the file only.
```

### P4 — Delete the dead engine

```
Delete src/qwen3/, include/retdec/qwen3/, and tests/qwen3/. Remove all
references from CMakeLists.txt files, and from
src/gui/panels/ai_assistant_panel.cpp and
src/gui/panels/settings_dialog.cpp — the GUI AI panel should be stubbed
to report "no inference backend configured" rather than removed. Keep
the CUDAToolkit detection logic. Build must succeed. Paste the build
output.
```

### P5 — Licence files

```
Run install-licence-files.sh from the repo root with NEW pointing at the
directory holding the new licence files. Then add to CMakeLists.txt:
install(FILES LICENSE LICENSE-AGPL LICENSE-COMMERCIAL NOTICE
        DESTINATION share/doc/${PROJECT_NAME})
Show me the CMakeLists diff and confirm all four files exist at the repo
root.
```

### P6 — Capstone bump

```
In cmake/deps.cmake, change CAPSTONE_URL to the Capstone 5.0.9 release
archive and set CAPSTONE_ARCHIVE_SHA256 to that archive's real SHA-256
(download and compute it — do not guess). Change nothing else. Rebuild.
If the build fails, STOP and paste the first compiler error verbatim.
If it succeeds, run:
  ctest --test-dir build -R capstone --output-on-failure
and paste the output.
```

### P7 — YARA bump

```
Same procedure as the Capstone bump, for YARA: cmake/deps.cmake,
YARA_URL to the v4.5.8 release archive, YARA_ARCHIVE_SHA256 to its real
hash. YARA 4.5 changed some API surface since 4.2 — if src/ fails to
compile, STOP and paste the first error rather than attempting a fix.
```

### P8 — yaramod bump

```
Same procedure, for yaramod v4.8.1. Note yaramod depends on YARA, so do
this only after the YARA bump is merged and green.
```

### P9 — llama.cpp dependency

```
Add deps/llamacpp/CMakeLists.txt following the pattern of
deps/tlsh/CMakeLists.txt: FetchContent from the llama.cpp repo at a
pinned commit hash (not a branch), exposing target
retdec::deps::llamacpp. Build with LLAMA_BUILD_SERVER=OFF,
LLAMA_BUILD_EXAMPLES=OFF, LLAMA_BUILD_TESTS=OFF, LLAMA_CURL=OFF.
Register it in deps/CMakeLists.txt behind RETDEC_ENABLE_NEURAL,
defaulting ON. Do not write any C++ yet.
```

### P10 — Inference skeleton

```
Create src/neural/ with CMakeLists.txt producing retdec::neural, linking
retdec::deps::llamacpp. Add src/neural/mock_inference.cpp implementing
the Inference interface from include/retdec/neural/inference.h as a
deterministic test double: it matches prompt substrings against a rule
list and returns canned responses. Add tests/neural/mock_test.cpp
covering it. Do NOT implement the llama.cpp backend yet.
Verify: ctest --test-dir build -R neural --output-on-failure
```

### P11 — llama.cpp backend

```
Implement src/neural/llama_inference.cpp against the skeleton provided.
Before writing any code, open deps/llamacpp/include/llama.h at the
pinned commit and confirm every symbol in the file header's API list
exists with the signature used. If any symbol is missing or differs,
STOP and report which ones — do not substitute alternatives.
```

### P12 — Semi-NCA

```
In src/ssa/domtree.cpp, replace the Lengauer-Tarjan link/eval
implementation with Semi-NCA. Keep include/retdec/ssa/ssa.h
byte-identical — no public interface changes. Preserve the algorithm
reference comment at the top of the file, updating the citation to
Georgiadis & Tarjan / the LLVM Semi-NCA implementation. All existing SSA
tests must pass unchanged.
Verify: ctest --test-dir build -R ssa --output-on-failure
```

### P13 — Algorithm reference doc

```
Read the file-header comment block of every .cpp in src/ssa/,
src/alias_analysis/, src/type_inference/, src/cfg_structure/,
src/algo_recover/, src/dce/, src/ipa/, and src/var_recovery/. Build
docs/algorithm_reference.md as a single table: Algorithm | Citation |
Source file | Complexity. Extract only what the headers state. Do not
invent citations or complexity figures. If a header lacks a citation,
put a dash.
```

### P14 — Version single-sourcing

```
Grep the repository for hardcoded version string "5.0" outside
deps/ and .git/. List every occurrence with file and line before
changing anything. Then make CMakeLists.txt the single source, deriving
the version everywhere else including release-installers.yml. Show me
the list first and wait for confirmation before editing.
```

### P15 — Fuzz harnesses

```
Following the pattern of tests/managed_integration/fuzz/fuzz_pyc.cpp,
add fuzz_pe.cpp, fuzz_elf.cpp, and fuzz_macho.cpp targeting the parsers
in src/fileformat/. Each takes raw bytes and must not crash on malformed
input. Register them in run_fuzzers.sh. Run each for 60 seconds and
report any crashes found — do not fix them, just report.
```

### P16 — Sanitizer workflow

```
Create .github/workflows/sanitizers.yml running weekly on cron: build
with -fsanitize=address,undefined and run scripts/run_asan.sh over
tests/decompile_samples. Upload the log as an artifact. Do not modify
run_asan.sh.
```

---

## Part 16 — Automation to reduce your workload

Build these early. Each one removes a recurring manual step.

### 16.1 `scripts/upgrade-dep.sh` **[C]**

One command per dependency bump: edit the pin, fetch, compute the real SHA-256, rebuild, run ctest, run benchmarks, report the delta, and either commit or revert. Turns a half-day of careful clicking into a command and a diff review.

### 16.2 `scripts/run_benchmarks.sh --compare <tag>` **[C+]**

Single entry point producing both tables and a delta against any tag. Called by CI, by dependency bumps, and by you.

### 16.3 Benchmark regression gate in CI **[C]**

Fail the PR if recompile success or coverage equivalence drops more than a set threshold against `baseline-2026-08`. This is what lets you accept Composer diffs without reading every line of generated analysis code.

### 16.4 `scripts/doctor.sh` extension **[C]**

You already have `doctor.sh`. Extend it to check: model file present and SHA matches, licence files present, no `capstone2llvmirtool` in a commercial package manifest, CI workflows enabled, dependency pins not release candidates. Run it in CI. It turns a checklist you would otherwise carry in your head into an assertion.

### 16.5 Nightly report **[C]**

One markdown file per night, committed to `docs/internal/nightly/`: benchmark numbers, coverage, fuzz crashes, build times, open regressions. Five minutes to read instead of five tools to check.

### 16.6 Task queue file **[C+]**

`docs/internal/backlog.md` with one line per task, its executor tag, its prompt reference from Part 15, and its state. Composer updates the state; you read the file. Replaces holding the plan in your head.

---

## Part 17 — Master sequence

| # | Task | Executor | Effort |
|---|---|---|---|
| 1 | Install `.cursorrules` | H | 10 min |
| 2 | Install licence files | C | 1 hr |
| 3 | Delete `src/qwen3/` | C | 1 hr |
| 4 | CI smoke on every push | C+ | 1 day |
| 5 | ctest on PRs | C+ | 2 days |
| 6 | Baseline Dockerfile | C | 2 days |
| 7 | Tag `baseline-2026-08` | H | 5 min |
| 8 | Decide D7, write it in README | H | — |
| 9 | DecompileBench harness | C+ | 2 wks |
| 10 | Algorithm-recovery metric | H | 2 wks |
| 11 | `upgrade-dep.sh` automation | C | 2 days |
| 12 | Capstone → 5.0.9 | C | 2 days |
| 13 | YARA → 4.5.8 | C+ | 3 days |
| 14 | yaramod → 4.8.1 | C+ | 2 days |
| 15 | googletest, Keystone, OpenSSL | C | 2 days |
| 16 | Regenerate `retdec-support` | H | 1 wk |
| 17 | llama.cpp dependency + mock | C | 1 wk |
| 18 | llama.cpp backend | C+ | 1 wk |
| 19 | Prompt construction with context | C+ | 1 wk |
| 20 | Verification gates | H | 2 wks |
| 21 | Neural tiers 1–3 | C+ | 3 wks |
| 22 | Offline assertion + provenance | C | 3 days |
| 23 | Fuzz harnesses + sanitizer CI | C | 1 wk |
| 24 | Tier 1 algorithms (9.1–9.3) | C+ | 4 wks |
| 25 | Version, docs, release automation | C/C+ | 2 wks |
| 26 | **SHIP** | H | — |
| 27 | Performance (11.1–11.4) | C+/H | 4 wks |
| 28 | rellic evaluation | H | 2 wks |
| 29 | LIEF adoption | C+ | 3 wks |
| 30 | Retypd | H | 3–6 mo |
| 31 | SAILR | H | 3–6 mo |
| 32 | Neural tiers 4–5 | H | 2 mo |
| 33 | LLVM migration | H | 6+ mo |

Steps 1–26 are a product. Steps 27–33 are a roadmap. **Ship at 26 regardless of what is unfinished.**

Rough calendar for 1–26: five to seven months at a sustainable pace, assuming this is not your only project.

---

## Part 18 — Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Neural pass hallucinates semantics | Fatal in defence use | Part 8.5 gates; deterministic artefact always primary and untouched |
| Composer deletes tests to reach green | Silent loss of regression net | `.cursorrules`; CI on every PR; benchmark gate |
| llama.cpp API break on pin bump | Build failure, wasted days | Pin commits; diff `llama.h` before any bump; escalation trigger 14.5 |
| LLVM 8 stops building on new toolchains | Project becomes unbuildable | Docker baseline buys time; Retypd unblocks migration |
| Retypd underdelivers | Months spent, small gain | Benchmark first; SURE'25 sets realistic expectations; reassess at month 3 |
| CPU inference too slow to be usable | Feature unusable on target hardware | Model tiering (2B/4B/9B); batching; incremental caching; measure at step 18 |
| Scope creep into infrastructure rewrite | Nothing ships | Ship at step 26; Part 13.9 on coupling vs dependency count |
| Benchmarks show pseudocode parity with stock RetDec | Positioning damage | Expected. D7 option (b) makes it irrelevant — lead with algorithm recovery and offline operation |
| D7 left undecided | Parts 12 and 13 stall | Decide at step 8, before any doc work |
| GPL contamination via Keystone or Unicorn | Commercial licence unenforceable | 5.3 packaging rule; 13.8 avoid list; CI manifest assertion |
| Too many parallel projects | This one never finishes | Not a technical risk, and the one most likely to actually decide the outcome |

---

## Final note

Two things in this document matter more than the rest.

**Step 26.** Everything before it is a shippable product; everything after is the next one. Plans of this size fail by never converging, not by being wrong.

**Decision D7.** One sentence, written in the README, that says what this product is. Almost every other choice here follows from it, and it costs you nothing but the willingness to close a door.
