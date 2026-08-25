# RetDec Imortek — Master Engineering Review and Execution Plan

**Subject:** `github.com/odin-loki/RetDec-Decompiler` @ `ebf3627` · v2.0.20 (CHANGELOG says 2.0.21)
**Scope:** full tree — 504,305 LOC `src`+`include`, 185,893 LOC tests, 8,815 test cases, 97 modules, 215 scripts, 10 CI workflows
**Prepared:** August 2026
**Author:** Odin Loch

This document supersedes and consolidates the three earlier review parts. Everything is folded in, deduplicated, corrected, and reorganised into a single execution plan. Where the earlier parts contradicted each other, this document states the corrected position only.

---

## How to read this

| Part | What it covers |
|---|---|
| **1** | Evidence base — the measurements this whole document rests on |
| **2** | Blocking findings — must close before any external technical conversation |
| **3** | Architecture critique |
| **4** | Tests, benchmarks, and the reproducibility problem |
| **5** | The AI programme (Qwen 3.5 9B / llama.cpp) |
| **6** | LLVM + Clang to latest, and the `retdec.pointee` port |
| **7** | Core decompiler roadmap |
| **8** | Security, CI, build, and repository hygiene |
| **9** | Legal, commercial, and positioning |
| **10** | **The plan — start to finish, phased, with entry and exit gates** |
| **A** | Consolidated backlog, every item, by theme |
| **B** | Measurement reference |
| **C** | Dependency graph and critical path |

Backlog items use thematic IDs (`DOC-04`, `AI-31`, `LLVM-12`) rather than a single running number, because the ordering that matters is the dependency ordering in Part 10, not the order of discovery.

---

# PART 1 — Evidence base

Everything in this document derives from measurements taken from the repository itself. Most of them were taken by you and committed to `results/`. This part collects them in one place because the argument depends on them, and because several are currently scattered across files a reader will never open together.

## 1.1 Scale

| Metric | Value |
|---|---|
| `src` + `include` | 504,305 LOC |
| Tests | 185,893 LOC, 8,815 test cases |
| Modules under `src/` | 97 |
| Scripts | 215 |
| CI workflows | 10 |
| Public docs / internal docs | 26 / 25 |
| Distinct `RETDEC_*` environment variables | 100 |
| `getenv` call sites | 74 |
| Files mentioning `llvm::` | 367 |
| Git tags | **0** |

Largest modules: `llvmir2hll` 95,971 · `bin2llvmir` 41,589 · `fileformat` 33,831 · `capstone2llvmir` 29,258 · `fileinfo` 29,139 · `gui` 19,980.

## 1.2 The four numbers

These four measurements define the state of the product. All four are yours.

| Measurement | Value | Source |
|---|---|---|
| Recompile rate, **`--buildable`** | **1.000** (216/216) | `results/compare-fork-vs-stock-full.md` |
| Recompile rate, stock RetDec 5.0 | 0.000 (0/216) | same |
| Algorithm-recovery F1, **name-blind** | **0.056** (CI 0.034–0.083) | `results/algorithm-recovery-full-nameblind.json` |
| Algorithm-recovery F1, name-assisted | 1.000 | `results/algorithm-recovery-full.json` |

**Where each appears publicly:**

| Number | Value | Published where |
|---|---|---|
| Recompile, `--buildable` — your best result | 1.000 | **nowhere** |
| Recompile, default `.c` | 0.000 | `docs/BENCHMARKS_TABLE.md` |
| Algorithm F1, name-blind — the honest number | 0.056 | **nowhere** |
| Algorithm F1, name-assisted — formally withdrawn | 1.000 | README → `BENCHMARKS_TABLE.md`, **in bold** |

**The public documentation publishes the wrong number on both axes.** It publishes a score the repository has formally withdrawn and suppresses a score that beats the incumbent 216–0.

`results/algorithm-recovery-gate-finding.md` contains the sentence **"Do not advertise 1.0."** `docs/BENCHMARKS_TABLE.md`, linked from the README, advertises 1.0. `docs/CLAIMS.md` marks `C-ALGO-F1` as `withdrawn` and says *"Generate marketing copy from this table; do not treat README... as evidence."* The README was never regenerated.

This is not concealment — every honest number is committed, named, and reasoned about. The failure is that **the audit layer and the publication layer were never connected.** But an outside reader cannot distinguish "failed to propagate" from "chose not to," and will not extend the benefit of the doubt.

## 1.3 Detector capability, measured

| Corpus | n | mean F1 | 95% CI |
|---|---|---|---|
| Full stand-in, symbol names visible | 216 | 1.000 | — |
| Full stand-in, **name-blind** | 216 | **0.056** | 0.034 – 0.083 |
| CI core, name-blind | 9 | 0.126 | — |
| Per-optimisation, name-blind | — | O0 0.050 / O2 0.059 / O3 0.059 | — |
| **Adversarial** implementations | 18 | **0.111** | **0.000 – 0.278** |
| **Third-party (zlib 1.3.1)** | 2 | **0.000** | 0.000 – 0.000 |

The adversarial confidence interval **includes zero**. On zlib's `crc32.c` — a textbook algorithm you explicitly detect — F1 is **0.000**, and the crc+deflate run **timed out**.

Confidence calibration (`results/a4-calibration.md`, 160 detections against empty ground truth):

| Reported confidence | n | Empirical precision |
|---|---|---|
| 0.4 – 0.6 | 80 | **0.000** |
| 0.6 – 0.8 | 10 | **0.000** |
| 0.8 – 1.0 | 70 | **0.000** |

Seventy detections asserted at ≥0.80 confidence, every one wrong. Your own note: *"Detector constants were not fitted."*

## 1.4 Why the corpus produces 1.0

The "216-binary corpus" is **6 hand-written C files plus 61 generated ones**, each roughly 20–40 lines, compiled with gcc and clang at O0/O2/O3. In full, one of them:

```c
/* Algorithm recovery corpus: binary search */
static int binary_search(const int* a, int n, int target) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == target) return mid;
        if (a[mid] < target) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}
int main(void) { /* ... */ }
```

Ground truth: `{"functions": {"binary_search": ["BinarySearch", "Search"], "main": []}}`.

One non-`main` function, named after its own label, in an unstripped binary. Perfect F1 is not a result; it is a tautology. B6 correctly guarded the *filename* channel; the *symbol* channel remained and is doing all the work. `results/b7-name-evidence.md` documents the score decaying as each name channel was tagged: 0.332 → 0.237 → 0.107 → 0.056.

## 1.5 The benchmarks are not reproducible

`.gitignore` excludes the corpus binaries **and the ground truth**:

```
/tests/algorithm_recovery/ground_truth/corpus.json
/tests/algorithm_recovery/predictions/*.json
/tests/algorithm_recovery/*_corpus/*
/tests/decompilebench/corpus/*
```

`results/corpus-build-recipe.md`: *"No pinned compiler container digest is published. Docker is not available in this WSL environment."*

**A third party cannot reproduce a single number in `results/`.** For a company being valued on technical claims, unverifiable benchmarks are worse than absent ones.

And `results/algorithm-recovery-gate-finding.md` records that the CI gate `MIN_MEAN_F1=0.95` is still in force, passed via `--stem-fallback` *"so the 0.95 `mean_f1` gate matches the stem-era score it was written for."* The green badge is protecting a metric the repository disowned.

## 1.6 Performance is unmeasured

`docs/BENCHMARKS_TABLE.md` reports fork 1.492s vs stock 0.242s — apparently a 6× regression. `results/compare-fork-vs-stock-full.md` states the methodology: *"Fork is Debug `retdec-decompiler` on WSL"* versus *"Stock is `remnux/retdec` v5.0 **Release** Docker."*

Debug-on-WSL against Release-in-Docker is not a comparison. The real ratio is unknown in both directions.

There is a plausible mechanism for a genuine regression, and it is fixable. The detector idiom appears **117 times** as `for (uint32_t b = 0; b < fn.blockCount(); ++b)`:

```cpp
static bool hasImmediate(const SSAFunction& fn, uint64_t val) {
    for (block : fn) for (instr : block) for (use : instr->uses)
        if (value(use)->kind == Immediate && value(use)->imm == val) return true;
    return false;                                   // full function scan, per constant
}
static bool hasAnyImmediate(const SSAFunction& fn, const std::set<uint64_t>& vals) {
    for (uint64_t v : vals) if (hasImmediate(fn, v)) return true;    // × |vals|
}
```

AES alone costs ~24 full function scans. Multiply by 11 crypto detectors, then by the container / sort / algorithm / serial / pattern / concurrency detectors, then by every function.

## 1.7 Dead and unreachable code

| Module | LOC | External consumers |
|---|---|---|
| `src/opencl/` | 4,547 | **0** |
| `src/cuda_accel/` | ~5,000 | 1 (`alias_analysis/steensgaard.cpp`) |
| `src/kotlin_emitter/` | 1,647 | **0** |
| `src/cxx_backend/` | 971 | **0** |
| `src/fsharp_emitter/` | 777 | **0** |
| `src/vbnet_emitter/` | 733 | **0** |
| **Total** | **~13,700** | |

`cuda_accel` and `opencl` are the same nine components implemented twice (`cuda_context`/`ocl_context`, `cuda_steensgaard`/`ocl_steensgaard`, and so on) — two parallel GPU backends, neither integrated, one of them described as a capability in `docs/CUDA_CAPABILITIES.md`.

## 1.8 Documentation drift

| Claim | README says | Reality |
|---|---|---|
| Output languages | 11 languages listed | **Native path emits C only** |
| C++ output | listed as a language | the C writer with a `.cpp` extension |
| `retdec-qwen3-runner` | listed as a shipped artefact; `--help` example in the user manual | **no source directory, no CMake target** |
| CLI `--model` | documented alternative | **flag does not exist** |
| Differential gate | "neural edits pass compile, structural, and **differential** gates" | `C-NEURAL-DIFF` → **withdrawn**; the gate warns and skips |
| GUI AI panel | "There is no in-GUI AI chat panel in v3" | `AIAssistantPanel` is instantiated in `mainwindow.cpp` |
| GUI panels | 5 listed | 22 exist |
| ARM64 / Mach-O | listed as supported input | `ARCHITECTURE_TARGETS.md`: *"not production-ready end-to-end"* |
| MIT chain of title | `C-LICENCE` → `demonstrated` | **151 upstream files carry no Avast notice** |

`src/retdec-decompiler/output_lang.cpp`, in full:

```cpp
void applyNativeOutputLanguage(OutputLangId lang) {
    const char* hll = nativeTargetHllId(lang);
    llvmir2hll::setTargetHll(hll);
    if (lang != OutputLangId::C && lang != OutputLangId::Cpp) {
        Log::info() << "[output-lang] Native pipeline only supports C/C++ emitters today; "
                    << "using C backend for requested " << outputLangCliName(lang) << ".";
    } else if (lang == OutputLangId::Cpp) {
        Log::info() << "[output-lang] C++ output uses the C HLL writer (C++-styled "
                    << ".cpp extension); dedicated C++ writer pending.";
    }
}
```

`ls src/llvmir2hll/hll/hll_writers/` returns exactly one file: `c_hll_writer.cpp`.

The other languages are reachable only from `managed_decompiler.cpp`, where the output language is **determined by the input format**, not chosen: JavaClass/JAR/DEX/APK → java, CliAssembly → csharp, Pyc → python, Lua → lua, Wasm → wat. That is a round-trip decompiler for eight managed formats — a real feature — but it is not eleven output languages.

## 1.9 Chain of title

| Measurement | Count |
|---|---|
| Files attributing Avast | 1,441 |
| Files attributing Odin Loch | 1,673 |
| Files attributing **only** Odin Loch | 237 |
| Of those, in unambiguously upstream directories | **151** |

Examples, all stock RetDec: `bin2llvmir/optimizations/param_return/param_return.h`, `phi_remover/phi_remover.h`, `types_propagator/types_propagator.h`, `writer_bc/writer_bc.h`, `register_localization/register_localization.h`.

Your own `NOTICE` says the MIT notice *"must be retained in all copies and substantial portions."* On these 151 files it was not. And no CLA, DCO, or copyright assignment exists anywhere in the repository — zero matches in `CONTRIBUTING.md` or `.github/`.

## 1.10 What is genuinely good

Stating this plainly, because the rest of the document is critical and the balance matters.

- **`--buildable`: 216/216 vs stock's 0/216.** A total, measured win over the incumbent.
- **`docs/CLAIMS.md`** is internal-audit-grade work with a `demonstrated`/`unpublished`/`withdrawn`/`opt-in` status per claim. Rare at any company size.
- **`results/a4-calibration.md`, `b7-name-evidence.md`, `gate-finding.md`** — you computed the numbers that make your product look worst, committed them, and named them honestly.
- **8 managed formats** with source-language output. Broader than most single tools.
- **A 22-panel Qt GUI** with a plugin API, headless mode, batch decompilation, project files, and a real test suite.
- **The neural subsystem** is carefully built: GBNF-constrained decode, tree-sitter AST structural gate, deterministic seeds, fail-closed model allowlist, compile-time-gated mock, prompt-injection stripping, callee-first topological refinement, context-budget refusal, content-addressed cache.
- **`goto` baseline**: mean 1.44 across 27 samples, **zero at O0**.
- **Every dependency SHA-256 pinned.**
- **10 CI workflows** including sanitizers, fuzzing, and a filename-rename guard.

The instinct that produced the audit layer is the most valuable thing in this repository. It is rarer than the code and harder to acquire.
---

# PART 2 — Blocking findings

Nine findings that must close before any external technical conversation. Each is discoverable in under an hour by a competent reviewer, and each independently ends the conversation.

## B1 · The public documentation publishes the wrong numbers

Stated fully in §1.2. The fix is a text editor and two days.

| ID | Action |
|---|---|
| `DOC-01` | Publish **0.056** with its CI in `BENCHMARKS_TABLE.md` and the README. Delete 1.0 from every public surface. |
| `DOC-02` | Publish the **`--buildable` 216/216 vs stock 0/216** result as the headline benchmark. |
| `DOC-03` | Delete `mean_f1` (the stem/label-fallback variant) from all public docs. A metric with a filename fallback will be assumed to be the one you quoted. |
| `DOC-04` | Always report name-blind as the headline. Report name-assisted separately and explicitly as "symbolicated binaries" — it is a legitimate second operating mode, never the headline. |
| `CI-01` | Switch the CI gate to name-blind `mean_f1_raw` with a floor at current measured performance, ratcheting upward. |
| `CI-02` | Remove `--stem-fallback` from `run_algorithm_recovery_ci.sh`, then delete the code path entirely. A metric with a filename fallback should not exist in the binary. |
| `DOC-05` | **Generate the README's feature and benchmark sections from `CLAIMS.md`** in CI. Fail the build if any public doc asserts a claim whose register status is `withdrawn` or `unpublished`. This is the structural fix and it retires the entire class of defect permanently. |

## B2 · 151 upstream files carry no Avast copyright notice

| ID | Action |
|---|---|
| `LEG-01` | Diff the tree against `avast/retdec` v5.0 mechanically. Restore the dual header on every upstream-origin file:<br>`@copyright (c) 2017 Avast Software, licensed under the MIT license`<br>`@copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)` |
| `LEG-02` | CI gate: fail if any file under a known-upstream path lacks the Avast line. Ten lines of shell; closes the issue permanently. |
| `LEG-03` | Make `docs/PROVENANCE.md` file-level and machine-generated: origin (upstream / derived / original), LOC, evidence. |
| `LEG-04` | Two hours with an Australian IP solicitor before any pitch. |

Note that `docs/CLAIMS.md` marks `C-LICENCE` as `demonstrated` while the measurement contradicts it. **A claims register with an unverified entry is worse than no register**, because it converts a documentation gap into a documented false statement. `LEG-02` converts that row from `asserted` to `automated`.

## B3 · No CLA — the dual-licence model does not currently work

`LICENSE` says contributions are accepted under AGPL-3.0+. If someone contributes under AGPL only, **you cannot relicense their contribution commercially.** The entire commercial path depends on Imortek holding rights to 100% of the non-upstream code. One accepted PR from a stranger breaks it irreversibly.

| ID | Action |
|---|---|
| `LEG-05` | Adopt a CLA with an explicit outbound relicensing grant — Apache ICLA or Harmony CLA-Individual/CLA-Entity with the FSFE-style grant option. |
| `LEG-06` | Wire CLA-assistant as a required check on `pull_request`. |
| `LEG-07` | Audit history. If every commit is yours, assert it explicitly in `PROVENANCE.md` — "sole author, no third-party contributions to date" is a strong DD asset and free if true. |

## B4 · Confidence scores have measured precision of zero

§1.3. The mechanism is visible in `src/crypto_detect/aes_detect.cpp`: `hasSBox` returns true if *any* immediate anywhere in the function is in `{0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x52, 0x09, 0x6a, 0xd5}` — and `0x09` is the integer 9. `hasRcon` matches the powers of two. Combined with `hasRoundLoop` (≥4 XOR, ≥1 AND, ≥1 shift) you reach 0.55 and the tool emits:

```
// Cryptographic primitive: AES-CBC
// Usage: EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv);
```

A CRC32 loop, an FNV hash, a PRNG, or any bit-twiddling function containing the constant 9 triggers this. `detectMode` returns `CBC` whenever *any* XOR is present, so the mode string is effectively constant. And the emitted string is a **usage recommendation**: if a customer reads `EVP_aes_128_cbc()` out of your tool and the code was actually GCM, you have caused a security regression in their reimplementation.

| ID | Action |
|---|---|
| `DET-01` | Delete `emittedAnnotation`. It is computed and consumed nowhere in the tree — dead code that exists only to be screenshotted. |
| `DET-02` | Ship `confidence: null` with an `evidence: [...]` list until the constants are fitted. Evidence lists are honest; unfitted floats are not. |
| `DET-03` | Never emit a cryptographic mode unless the mode's structure is positively identified. `ECB` is not a safe default — emit `mode: unknown`. |

## B5 · A documented binary does not exist

`retdec-qwen3-runner` appears in `README.md` (three times, including a shipped-artefact list), `docs/WINDOWS_NATIVE_BUILD.md`, `docs/MINGW_CROSS_DEEP_DIVE.md`, and `docs/user_manual.md` **with a `--help` usage example**. There is no `src/qwen3_runner/`, no CMake target. The `--model` flag it points to as an alternative also does not exist.

`docs/PROVENANCE.md` even lists it as *"Imortek-new | Support dirs; no C/C++ of their own"* — the docs know there is no code, and the user manual still tells people to run it.

`scripts/ci/check_doc_vs_code.py` exists but is a hardcoded allowlist of four tokens; it verifies that four named things exist and cannot detect a documented artefact that does not.

| ID | Action |
|---|---|
| `DOC-06` | Purge `retdec-qwen3-runner` and `--model` from all public docs. |
| `CI-03` | **Invert `check_doc_vs_code.py`**: extract every `retdec-*` token, every `--flag`, and every `RETDEC_*` variable appearing in public docs, then assert each resolves to a CMake target, a registered CLI option, or a `getenv` call. |
| `CI-04` | Assert every binary named in a release-artefact table corresponds to an `add_executable` target. |

## B6 · The incremental cache can return wrong results, and it is on by default

`computeFunctionBodyHash` hashes block count, instruction count, function name, per-block size, opcodes, operand *counts*, and callee names. **It does not hash constant operand values.**

Every crypto detector keys on immediates. `x ^= 0x63` and `x ^= 0x99` are both a two-operand `Xor` and hash identically. Analyse binary A containing an AES S-box constant, then binary B with a structurally identical function and different constants — **B receives A's AES detection from the cache.** `incrementalCacheEnabled()` returns `true` by default.

| ID | Action |
|---|---|
| `CACHE-01` | Hash constant operand values. One line; it is the whole bug. |
| `CACHE-02` | Include a detector-version token and the threshold-file hash in the key. Otherwise the cache silently defeats your own calibration work. |
| `CACHE-03` | Switch from FNV-1a-64 to SHA-256 or BLAKE3. |
| `CACHE-04` | HMAC the cache file or move it out of user-writable space. A user-writable JSON file that injects detections into a report is a supply-chain hole in the artefact itself. |
| `CACHE-05` | **Differential test**: analyse the corpus with cache on and cache off, assert byte-identical output, run in CI. Would have caught this immediately and catches every future cache bug free. |
| `CACHE-06` | Version the cache file format; invalidate on mismatch rather than misreading. |

## B7 · Native output supports one language; the README advertises eleven

§1.8.

| ID | Action |
|---|---|
| `DOC-07` | Rewrite the output-languages section as two input-keyed tables: "Native input → C" and "Managed input → source language, per format." Delete the eleven-language list. |
| `CLI-01` | Remove `cpp` from `--output-lang` until a real C++ writer exists, or rename the value so it cannot be misread. Shipping renamed C as "C++ output" gets found in a demo. |
| `DEAD-01` | Either wire `cxx_backend` in as a genuine second HLL writer — it is most of the way there — or delete it. |
| `DEAD-02` | Delete `kotlin_emitter`, `fsharp_emitter`, `vbnet_emitter` (3,157 LOC), or wire Kotlin in behind DEX metadata detection (`kotlin_metadata.cpp` suggests that was the plan) and delete the other two. |

## B8 · There are no releases

Zero git tags. `release-installers.yml` triggers on `tags: v*` and has never run. No published binaries for any platform. Version drift: CMake 2.0.20 · README 2.0.20 · `releases/VERSION` 2.0.20 · **CHANGELOG 2.0.21**.

To evaluate the tool a person must install CMake 3.26+, Qt 6, Perl, Python, and Ninja, run a large-file fetch script, and build LLVM from source. **The evaluation funnel has an impassable first step.**

| ID | Action |
|---|---|
| `REL-01` | Tag `v2.0.21`. Let the release workflow run. Fix what it breaks. |
| `REL-02` | **Ship a Docker image.** `docker run imortek/retdec analyse foo.elf` must work. Largest single reduction in evaluation friction and you already have a Dockerfile. |
| `REL-03` | Publish prebuilt binaries: Linux x86-64 (AppImage + tarball), Windows (NSIS + zip). |
| `REL-04` | Single source of truth for version — `CMakeLists.txt` — with `releases/VERSION` and the CHANGELOG heading generated from it, plus a CI drift check. |
| `REL-05` | Sign releases: cosign/Sigstore for artefacts, Authenticode for the Windows installer. Unsigned installers do not run inside government networks. |
| `REL-06` | Attach a CycloneDX SBOM to every release. |
| `REL-07` | `QUICKSTART.md`: `docker pull` → analyse a bundled sample → read output. Ten minutes, zero build. |

## B9 · AGPL is a procurement blocker for your stated buyer

A material fraction of Five Eyes and NATO-aligned organisations have blanket policies against AGPL specifically, because the network clause creates uncertainty about internal deployment and complicates the SBOM story — even where a commercial licence exists.

| ID | Action |
|---|---|
| `LEG-08` | Reconsider the open half. For an on-prem analysis binary sold to government, **GPL-3.0 + commercial** or **BSL 1.1 with a 4-year GPL conversion** fits procurement better. BSL keeps source-available (which agencies want for auditability) without the network clause. |
| `LEG-09` | `LICENSING_FAQ.md` written for a procurement officer, not a developer: "Can we run this air-gapped without disclosing anything? Yes, under the commercial licence, clause X." |
| `LEG-10` | Remove the price list from `LICENSE-COMMERCIAL`. Prices in git are prices you cannot negotiate. |
| `LEG-11` | CI licence-compliance gate: fail if Keystone (GPL-2.0) symbols appear in a commercial-package target. `NOTICE` promises this exclusion and nothing enforces it. |
| `LEG-12` | Qt LGPL compliance evidence: confirm dynamic linking and ship relink-capable artefacts with commercial packages. |
| `LEG-13` | Correct the LLVM licence line in `NOTICE`. It says "University of Illinois/NCSA"; LLVM relicensed to Apache-2.0-with-LLVM-exception at LLVM 9. Five-minute fix to a document an acquirer will read. |

---

# PART 3 — Architecture critique

## 3.1 Two IRs, and the new one is the weaker one

Upstream's pipeline is `capstone2llvmir → bin2llvmir (LLVM IR) → llvmir2hll → C`. The fork adds a second SSA IR in `src/ssa/` (1,177 LOC: Braun construction, dominator tree, phi placement, renaming, liveness) plus `src/retdec/llvm_to_ssa.cpp` (239 LOC) to bridge. **Every new detector runs on the second IR.**

Consequences:

- Detectors sit downstream of a lossy 239-line translation. Whatever it drops is invisible to every detector.
- You maintain two SSA implementations, and `RETDEC_SSA_BRAUN` toggles between two constructions *inside* the second one.
- `src/ssa/domtree.cpp` (220 lines) reimplements `llvm::DominatorTree`.
- You get none of LLVM's analyses. **`ScalarEvolution` would give you loop trip counts — exactly the signal that distinguishes AES's 10/12/14 rounds from a generic XOR loop.** You have it available and are not using it.
- **336 instances of `if (!x) continue;`** across the new modules: silent skips on null. If translation drops something, detectors under-report and nothing surfaces it.

| ID | Action |
|---|---|
| `SSA-01` | **Round-trip fidelity harness**: for each function assert every LLVM constant, call, block, and edge appears in the SSA form. Report a fidelity percentage per binary. Expectation: well below 100%, and it explains part of the 0.056. |
| `SSA-02` | Replace the 336 silent `continue`s with a diagnostic counter — `[analysis] skipped N instructions (M null values)` — so under-reporting becomes visible. |
| `SSA-03` | Port detectors to LLVM IR. Real cost (3–6 months), but it eliminates the second IR and hands you `ScalarEvolution`, `LoopInfo`, `MemorySSA`, `AliasAnalysis` free. **This is the correct answer** and Part 6 makes it cheaper. |
| `SSA-04` | If the dual IR survives: delete `src/ssa/domtree.cpp`, compute dominance in LLVM, carry it across; pick one SSA construction and delete the other. |

## 3.2 `decompile()` is 490 lines nested 14 deep

`src/retdec/retdec.cpp` lines 588–1079. Maximum tab depth in the file is 14. This block appears **four times verbatim**:

```cpp
const std::size_t before = semanticMap.count(item.fn->name())
    ? semanticMap.find(item.fn->name())->second.size() : 0;
analysis::appendXDetections(semanticMap, *item.fn);
const auto it = semanticMap.find(item.fn->name());
if (it != semanticMap.end() && it->second.size() > before) ++nX;
```

| ID | Action |
|---|---|
| `ARCH-01` | Introduce a `SemanticPass` interface — `name()`, `run(const SSAFunction&, SemanticDetectionMap&)`, `enabled(const Config&)` — registered in a vector. The four copy-pasted blocks become one loop and adding a detector stops requiring a `retdec.cpp` edit. |
| `ARCH-02` | Extract the analysis phase into `runSemanticAnalysis(Module&, Config&)` in its own TU. |
| `ARCH-03` | Split `semantic_recovery_export.cpp` (1,771 lines) per detector kind. |
| `ARCH-04` | CI gate via `clang-tidy readability-function-size`: no function over 100 lines, no nesting over 5. Grandfather existing violations into a shrink-only allow-list. |
| `ARCH-05` | Split `if_to_switch_optimizer.cpp` (8,246 lines). Upstream-derived, but it is a compile-time and comprehension problem regardless. |

## 3.3 ~13,700 LOC of dead modules, two of them marketed

§1.7.

| ID | Action |
|---|---|
| `DEAD-03` | **Delete `opencl` and `cuda_accel`**, move to a branch, note in `RESEARCH_FRONTIERS.md` that GPU acceleration was prototyped and parked. Costs nothing, removes 9.5k LOC of liability, entirely defensible. Alternative: integrate exactly one and publish a benchmark — an unbenchmarked accelerator is not an accelerator. |
| `DEAD-04` | Do not leave both in the tree while `CUDA_CAPABILITIES.md` describes them as capabilities. That is the pattern that reads as overstatement. |
| `DEAD-05` | Remove `src/experimental/` (one stub file) or document why it exists. |

## 3.4 LLVM 14 on a dead fork; support package from 2019

`cmake/deps.cmake` pins `avast/llvm@a776c2a` — an LLVM 8-era fork maintained by a company that abandoned the project in 2022. Full treatment in Part 6.

`SUPPORT_PKG_URL` pins `retdec-support_2019-03-08.tar.xz`. Your statically-linked-function identification has **no coverage of anything built since March 2019** — no modern glibc, no recent MSVC runtime, no Rust, no Go. Function identification is the difference between "40,000 lines of C" and "400 lines of your code plus glibc, zlib, and OpenSSL." It is the highest-value transformation a decompiler performs and yours is seven years stale. Treatment in Part 7.

Also stale: Keystone 0.9.2 (2019), Capstone 5.0.9, `CMAKE_CXX_STANDARD 17` while C++23 is written elsewhere, `cmake_minimum_required(3.13)` while presets require 3.26.

## 3.5 Configuration is 100 environment variables

74 `getenv` sites. These are not all diagnostics:

| Variable | Default | Effect |
|---|---|---|
| `RETDEC_PARALLEL_ANALYSIS` | on if `hw_concurrency() > 2` | parallel detector execution |
| `RETDEC_INCREMENTAL_CACHE` | **on** | serves cached detections |
| `RETDEC_SSA_BRAUN` | — | selects SSA construction |
| `RETDEC_TYPE_INFERENCE` | — | enables type inference |
| `RETDEC_USE_ANDERSEN` | — | selects alias analysis |
| `RETDEC_EMIT_BUILDABLE` | **off** | **your best feature** |
| `RETDEC_LIEF_SHADOW` | — | alternate loader |
| `RETDEC_NEURAL_*` | 30+ vars | the entire AI feature |

The output artefact does not record how it was produced. Two analysts run the same binary and get different results with no way to discover why. For an evidentiary artefact in a defence context, that is disqualifying.

| ID | Action |
|---|---|
| `CFG-01` | Promote every behavioural variable to a CLI flag and a `config.json` key. Keep env vars as deprecated overrides that warn. Reserve env-only for `*_DIAG` / `*_TRACE`. |
| `CFG-02` | **Make `--buildable` a first-class CLI flag and default it ON.** Highest-leverage one-line change available; your differentiator is currently opt-in and unadvertised. |
| `CFG-03` | `--dump-effective-config` printing every resolved setting and its provenance (default / env / flag / file). |
| `CFG-04` | **Stamp the effective configuration into `config.json` output** — tool version, git SHA, every non-default setting, detector threshold version, model SHA and sampler parameters if neural ran. |
| `CFG-05` | Reject unknown `RETDEC_*` variables with a warning listing near-matches. Silent typos in a 100-variable surface are inevitable. |
| `CFG-06` | Generate the env-var reference doc from source so it cannot drift. |
| `CFG-07` | **Deterministic output guarantee**: same input + same config + same version = byte-identical output, tested in CI. Currently violated by `CACHE-01` and by unrecorded configuration. |

## 3.6 Module and script sprawl

97 `src/` directories, several single-file (`concurrency_detect` 1,277 LOC in one cpp; `serial_detect` 1,984; `module_cluster` 1,033), each with a directory, a CMakeLists, and an install target. 215 scripts. 40 `RETDEC_ENABLE_*` toggles — 2⁴⁰ untested configurations.

| ID | Action |
|---|---|
| `ARCH-06` | Consolidate the detector modules into one `src/semantics/` library with subdirectories. 60 CMake targets that always build together should be one. |
| `ARCH-07` | Audit `scripts/`. Archive or delete one-shot investigation scripts. Every survivor gets a header: what it does, when to run it, what it outputs. |
| `ARCH-08` | Reduce the `RETDEC_ENABLE_*` matrix to configurations you actually test and ship. |
| `ARCH-09` | Move `RETDEC_AUDIT.md` (90 KB) and `CHANGELOG.md` (83 KB) under `docs/`, leave pointers. |
| `ARCH-10` | Consolidate 51 docs with substantial overlap. Every survivor gets an owner and a review date. |
| `ARCH-11` | Decide deliberately about `.cursorrules` and `docs/internal/` in a public repo. No shame in either, but they should be decisions. |

---

# PART 4 — Tests, benchmarks, and reproducibility

## 4.1 8,815 tests that cannot fail

`tests/crypto_detect/crypto_detect_test.cpp` is representative. Every test hand-builds an `SSAFunction`:

```cpp
static void addImmUse(SSAFunction& fn, IrInstr* instr, uint64_t imm) {
    IrValue* v = fn.allocValue(ValueKind::Immediate);
    v->imm = imm;
    Use u; u.valueId = v->id;
    instr->uses.push_back(u);
}
```

then asserts that a function containing `0x63` is detected as AES. It will be, because the code checks for `0x63`. The test restates the implementation. It cannot catch a false positive, cannot catch a false negative on a real binary, and cannot catch `llvm_to_ssa` dropping the immediate before the detector sees it.

Three negative tests exist in a 1,074-line file. `NegativeTest.GenericArithmeticNoFalsePositive` uses immediates `100..109` — chosen, consciously or not, to miss the constant set. Substitute `9` and add four XORs and it fails.

| ID | Action |
|---|---|
| `TEST-01` | **Real-binary ground truth.** Compile OpenSSL, libsodium, mbedTLS, BoringSSL, wolfSSL at O0–O3 with GCC and Clang. You know which functions are AES because you have the source. Thousands of labelled positives, free. |
| `TEST-02` | **Hard negatives.** The current set (*"unit conversion, clamp, lerp, date, BMI, PID, flags, IPv4"*) is too easy, and `b8-negative-corpus.md` admits it: *"loop-free numeric / clamp / flag / log-level programs, not parsers or network stacks."* You need zlib, LZ4, xxHash, CRC32 tables, PRNGs (Mersenne, PCG, xoshiro), base64, UTF-8 decoders, chess bitboards, image kernels — things that *look* like crypto and are not. |
| `TEST-03` | Report **precision, recall, and F1 per detector per optimisation level** with confidence intervals. Never a single fused number again. |
| `TEST-04` | Convert synthetic tests into **property** tests: output invariant under block reordering; invariant under SSA renaming; detection at O0 implies detection at O2 at ≥X%. |
| `TEST-05` | **Mutation testing** (`mull`). If you can flip a comparison in a detector and no test fails, that detector is untested. Target 60% mutation score on new modules — brutal at first and the most informative metric you can add. |
| `TEST-06` | `tests/regression/`: every reported false positive becomes a permanent test with the binary attached. |
| `TEST-07` | Populate `tests/crash_corpus/` — it exists and is empty. |

## 4.2 The corpus is toy, unpublished, and not held out

§1.4, §1.5. `results/corpus-build-recipe.md` also records that the "B11 holdout" *"is the B9 source set, not a third-party Debian holdout"* — i.e. not a holdout.

| ID | Action |
|---|---|
| `BENCH-01` | Commit the ground-truth JSON and a corpus manifest (source SHA-256 + compiler version + flags). If binaries are too large for git, commit a `make corpus` recipe reproducing them bit-for-bit. |
| `BENCH-02` | **Pin a compiler container digest** (`gcc:13.2.0@sha256:...`). Without it the corpus is not reproducible even by you on a different machine. |
| `BENCH-03` | Rebuild the corpus from **real software**: coreutils, busybox, sqlite, zlib, libpng, openssl, curl, plus Debian `-dbgsym` packages. Label from DWARF, strip, then measure. The only corpus whose numbers mean anything. |
| `BENCH-04` | True three-way split: **fit / validate / held-out**, split by *project* not by function. Never touch held-out until a release. |
| `BENCH-05` | Publish per-detector confusion matrices, not scalars. |
| `BENCH-06` | Fix the performance methodology: both sides Release, same container, same hardware, semantic export toggled off for a core comparison and on for a feature comparison. Publish both. |
| `BENCH-07` | Publish CI results to a public dashboard. "Here is our live benchmark page" is a far stronger DD answer than a JSON file in git. |

## 4.3 Fuzzing is a smoke test; sanitizers are thinner than labelled

`.github/workflows/fuzz-pr.yml`:

```
corpus="${BIN_DIR}/corpus_${name}"
mkdir -p "${corpus}"          # fresh, empty, every run
"${bin}" "${corpus}" -max_total_time=30 -max_len=65536
```

Seven targets × 30 seconds = **3.5 minutes total**, each from an empty corpus. Coverage-guided fuzzing from empty in 30 seconds does not get past a PE header.

Unfuzzed, against 11,323 LOC of hand-written parsers plus the unpackers:

| Parser | LOC | Target |
|---|---|---|
| `lua_parser` | 1,448 | ❌ |
| `bc_module` (CIL) | 1,389 | ❌ |
| APK / JAR (zip) | — | ❌ |
| `pdbparser` | 2,783 | ❌ |
| **`unpackertool`** | **9,922** | ❌ |

The unpackers are the worst omission — they parse attacker-authored packed executables, the most hostile input the tool ever receives.

`sanitizers.yml` has one job, `asan-ubsan`, containing `-DRETDEC_USE_UNDEFINED_BEHAVIOR_SANITIZER=OFF`. **UBSan is disabled in the job named for it.** No TSan, no MSan, and the workflow runs on `schedule` only — not on PRs. Parallel analysis is on by default above two cores, so the concurrent path has never seen a thread sanitizer.

| ID | Action |
|---|---|
| `FUZZ-01` | **Apply to OSS-Fuzz.** Free, continuous, CPU-years instead of CPU-minutes, and acceptance is itself a DD credential. |
| `FUZZ-02` | Until then: persist the corpus via `actions/cache` so coverage accumulates; nightly runs at `-max_total_time=3600` per target. |
| `FUZZ-03` | Seed corpora from real files — `tests/test_binaries/`, the decompilebench corpus, small public samples. |
| `FUZZ-04` | Add targets for lua, CIL, zip/APK/JAR, PDB, and each unpacker plugin. |
| `SAN-01` | **Turn UBSan on.** If it fails, that is a finding, not a reason to disable it — triage and suppress specific checks. |
| `SAN-02` | Add a TSan job; run the corpus under it with `RETDEC_PARALLEL_ANALYSIS=1`. |
| `SAN-03` | Add MSan (needs instrumented libc++; worth it for the parsers). |
| `SAN-04` | Run ASan on **every PR**. Nightly-only sanitizers report bugs a week after the commit that caused them. |
| `FUZZ-05` | Resource-exhaustion suite: 4 GB input, 100k-block function, 10k-deep nesting, zip bomb, self-referential ELF sections. Assert graceful failure with bounded RSS. |

## 4.4 Missing CI coverage

| ID | Action |
|---|---|
| `CI-05` | macOS runner. You claim Mach-O support with no macOS CI. |
| `CI-06` | `aarch64` runner (GitHub provides them). You claim ARM support. |
| `CI-07` | Coverage workflow with a ratchet — `tools/dev/collect_coverage.sh` exists; wire it up and publish the badge. |
| `CI-08` | CodeQL. Free, and its absence is noticed. |
| `CI-09` | Pin GitHub Actions to commit SHAs rather than tags. Tag mutation is a real supply-chain vector and government reviewers check. |
| `CI-10` | Enable Dependabot and secret scanning. |
| `CI-11` | Pre-build LLVM as a container layer or GitHub Package. 360-minute timeouts are a velocity tax and an evaluation blocker. |
| `CI-12` | ABI-stability check (`abi-compliance-checker`) once the C ABI exists. |
| `CI-13` | Perf ratchet on analysis-phase wall time, Release build, fixed corpus. |
| `CI-14` | Gate per-detector precision — no detector ships below a measured floor. |
---

# PART 5 — The AI programme

**Model:** Qwen 3.5 9B · **Backend:** llama.cpp (pinned `b10451`) · **Subsystem:** `src/neural/` 3,448 LOC

## 5.0 What already exists

Stated first so nothing gets rebuilt. This subsystem is better engineered than most AI features bolted onto native tools.

| Capability | Where | State |
|---|---|---|
| llama.cpp backend + mock swap-in | `llama_inference.cpp`, `mock_inference.cpp` | working |
| Model SHA-256 allowlist, fails closed | `model_verify.cpp` (20 KB) | working, **list empty** |
| Deterministic sampling (seed 0 default) | `inference.h` | working |
| GBNF grammar-constrained decode | `llama_sampler_init_grammar`, `namingRenameMapGbnf()` | working, **one grammar** |
| Prompt-injection stripping (strings + comments) | `prompts.cpp:stripCStringLiterals` | working |
| Compile gate (`cc -fsyntax-only`) | `gates.cpp:spawnSyntaxOnlyCompiler` | working, **weakest link** |
| Structural gate via tree-sitter C AST | `gates.cpp:408+` | working |
| Spawn-family identifier rejection | `applyJsonRenameMap` (N15) | working |
| Context-budget refusal + head/tail retry | `llama_inference.cpp` (N11) | working |
| Mean selected-token probability + abstain | `GenerationResult::meanTokenProb` (N17) | working |
| Callee-before-caller topological refine | `orderFunctionsCalleeFirst` (N18) | working |
| Rich semantic context in prompt | `serializeSemanticContext` (N16) | working |
| Content-addressed refinement cache | `refiner.cpp` — model+prompt+tier+sampler | working, opt-in |
| KV prefix reuse within a function | `RETDEC_NEURAL_REUSE_KV` | working |
| MTP load flag | `mparams.load_mtp` | wired, **unmeasured** |
| GPU offload with CPU fallback | `n_gpu_layers` + capability probe | working |
| Deadline / SIGINT abort | `RETDEC_NEURAL_DEADLINE_MS` | working |
| Five refinement tiers | `RefinementTier` enum | **one-line prompts** |

**Missing:** batched decode (`n_seq_max` is 1), embeddings, tool use, fine-tuning, self-consistency, the differential gate (withdrawn), per-identifier confidence, and a real compile gate.

**The architectural fact this section turns on:** Part 6 links LLVM 22 **with Clang**. That gives you an in-process C frontend, which upgrades the weakest gate into the strongest and dissolves the sandbox problem. Do the migration first and the AI work gets easier, not harder.

---

## 5.1 Inference infrastructure

The backend decodes one sequence, one token, one function at a time. On a 40,000-function binary that is not a product.

| ID | Action |
|---|---|
| `AI-01` | **Batched multi-sequence decode.** Set `n_seq_max = 8–32`, build a `llama_batch` with one sequence per function. On CPU this is nearly free — batch-1 matmuls are memory-bandwidth-bound, so batch 16 costs ~1.5× the time for 16× the work. Expect **8–15× throughput**. `RETDEC_NEURAL_BATCH` was removed with the note *"do not restore without `n_seq_max>1` decode"* — this is that work. `BatchRefiner` (611 bytes, sequential) becomes real. |
| `AI-02` | **TU-wide prefix KV sharing.** The `serializeSemanticContext` blob is identical for every function in a binary and can be thousands of tokens. Decode it once into sequence 0, then `llama_memory_seq_cp` to every worker. Eliminates the dominant prefill cost. With `AI-01`, expect an order of magnitude. |
| `AI-03` | **Speculative decoding** with a 0.5–1.5B draft from the same family. Decompiled C is highly predictable — boilerplate declarations, repeated identifiers, formulaic control flow — so acceptance should be high. Expect **2–3×** on generation-bound tiers. |
| `AI-04` | **Measure MTP.** `load_mtp` is wired and never benchmarked. It overlaps with `AI-03`; measure both, pick per tier, publish. |
| `AI-05` | **Quantisation matrix** measured on **gate-pass rate**, not perplexity. Perplexity is the wrong objective; "fraction of refinements passing compile + structural + differential" is the right one. Benchmark Q8_0 / Q6_K / Q5_K_M / Q4_K_M / IQ4_XS, publish quality-vs-RAM-vs-speed, pick the default from the table. |
| `AI-06` | **`retdec-neural-server`** — persistent model held resident over a Unix socket, `--neural-server <sock>`. Model load dominates short runs. Essential for GUI latency and corpus-scale batch. |
| `AI-07` | **Adaptive context length.** `contextLen = 16384` is a hardcoded default. Size it from the largest function plus context, capped by available RAM. A 9B at long context spends more on KV than weights; get this wrong and you OOM on firmware. |
| `AI-08` | **Chunked refinement for oversized functions.** N11 currently *refuses* after one head/tail retry. Split at CFG basic-block boundaries, refine each chunk with surrounding declarations, stitch. Refusing means the functions that most need help get none. |
| `AI-09` | **Thread tuning.** Default `RETDEC_NEURAL_THREADS` to physical cores; make it interact correctly with `RETDEC_PARALLEL_ANALYSIS`. Two pools fighting is an invisible 2× loss. |
| `AI-10` | NUMA-aware `mmap` for server mode on multi-socket analysis boxes. |
| `AI-11` | **Vulkan / SYCL / ROCm** backends, not just CUDA. Government hardware is heterogeneous and often AMD or Intel. |
| `AI-12` | **Backend capability probe** into `config.json`: llama.cpp backend, quant, layers offloaded, effective context. Part of `CFG-04`. |

## 5.2 Prompting and structured output

Current tier prompts are single sentences (`"Suggest improved variable and function names only. Do not change logic."`). A 9B model needs far more scaffolding.

| ID | Action |
|---|---|
| `AI-13` | **Prompts out of C++ into versioned data files** — `support/prompts/<tier>.v3.md`, hashed into the cache key. A prompt is a tuned artefact; recompiling to change one is wrong and an unversioned prompt makes the cache incoherent. |
| `AI-14` | **Few-shot exemplars per tier**, drawn from your own corpus where the original source is known. Cheapest quality win available and typically worth more than a model upgrade. |
| `AI-15` | **GBNF for every structured output.** You have one grammar. Add: struct-layout proposals, type annotations, function summaries, algorithm ID with abstention, parameter direction, spec-extraction records. Grammar-constrained decode makes a 9B behave like something much larger on structured tasks because it cannot emit malformed output at all. |
| `AI-16` | **Kill `FullRewrite`.** "Rewrite for clarity while preserving semantics" is the tier most likely to silently change behaviour and the hardest to gate. Replace with a **structured edit script** — JSON `{kind, target, replacement}` operations against the AST, validated and applied by your code, not the model. Every edit becomes individually reviewable, revertible, and provably scoped. |
| `AI-17` | **Thinking mode per tier.** `thinkingMode` exists and is never differentiated. On for type inference, struct layout, algorithm ID (reasoning tasks); off for naming and comments (pattern tasks). Measure per tier rather than picking globally. |
| `AI-18` | **Self-consistency sampling.** For structured tiers, sample N=5 at temperature 0.6 and majority-vote per field — keep a rename only if ≥3 of 5 agree. A well-established 5–15 point gain on structured extraction, and it converts soft confidence into a hard explainable one. |
| `AI-19` | **Solve N17 the other way.** Per-identifier logprob is blocked on a llama.cpp token-level API. You do not need it: agreement fraction across independent samples (`AI-18`) is a *better* confidence signal, because it is calibrated against the task rather than the vocabulary. |
| `AI-20` | **Negative constraints in the grammar, not post-hoc.** C keywords and spawn-family identifiers are currently filtered in `applyJsonRenameMap` after generation. Push them into GBNF so they cannot be generated. |
| `AI-21` | **Include the disassembly** for hard functions. The recovered C has already lost information the model could use. Test whether it pays on the difficult tail. |
| `AI-22` | **Include callers' use-sites.** For parameter naming and types, how a value is *used* at the call site beats the callee body. `serializeSemanticContext` already emits `callers`/`callees`; add a short excerpt of each call expression. |
| `AI-23` | **Prompt-length budgeting per tier**, with measured token costs published. |
| `AI-24` | **A/B prompt harness** — `scripts/ci/run_prompt_ab.py`: two prompt versions over a fixed function set, gate-pass and edit-acceptance rates with bootstrap CIs. A day of work that pays back for years. |

## 5.3 Refinement tiers

Five is a start. The full set worth building, ordered by value per unit of risk. Each independently enableable and independently gated.

| ID | Tier | Verifier |
|---|---|---|
| `AI-25` | **Type inference** — propose concrete types for parameters, locals, returns | Check each proposal against observed access widths, arithmetic, and call-site types from `bin2llvmir`. Accept only where consistent. **Highest-value tier**: RetDec's weakest dimension is types, and the LLM is unusually good at "this `int32*` with stride 24 accessed at +0/+8/+16 is a struct of three 8-byte fields." |
| `AI-26` | **Struct layout recovery** — named struct with named fields from access offsets | Every observed offset lands inside a field, no overlaps, total size matches observed allocation. Transforms readability more than any other single edit. |
| `AI-27` | **Enum recovery** — name enums and members from surrounding context | The constant set is closed and all comparisons are equality. |
| `AI-28` | **Function summary** — one paragraph per function | Comment-only; cannot break the build. Cheap, safe, and the tier that makes a 40,000-function binary navigable. |
| `AI-29` | **Parameter direction and contract** — in/out/inout, nullable, size, ownership | Input to the formal-verification bridge and to harness generation. |
| `AI-30` | **Library-call identification** — "this inlined loop is `strlen`" | Cross-checked against the signature database (`CORE-30`). Agreement is strong signal; disagreement is a bug report for one of them. |
| `AI-31` | **Algorithm identification — second opinion, mandatory abstention** | Your heuristics measure 0.056 name-blind. An LLM reading recovered C is plausibly better, and the channels are independent. Report only where model and heuristic agree, or where the structured evidence list is verifiable. **Do not ship before `AI-52` exists** — an unmeasured second bad channel is worse than one measured bad channel. |
| `AI-32` | **Crypto identification** — model recognises a round function | Verifier checks the constant table in `.rodata` and the trip count. Two independent channels plus a hard verifier is how you get from 0.000 precision to defensible. |
| `AI-33` | **Error-handling / control-flow annotation** — error paths, cleanup blocks, goto unwinding, RAII-equivalents | Comment-only. Large readability gain at zero semantic risk. |
| `AI-34` | **Idiom lifting** (exists, underspecified) — manual loop → `memcpy`/`memset`/`strlen`, magic division → `/ n` | Each rewrite needs a mechanical verifier. Bounded, enumerated rewrites only — never "make this nicer." |
| `AI-35` | **Deobfuscation** — opaque predicates, flattening dispatch loops, string decryption, MBA identities | Verified by the symbolic executor (`CORE-13`). **High-value defence capability that no open tool does well.** |
| `AI-36` | **Vulnerability annotation** — unchecked lengths, off-by-one, unvalidated indices, sign confusion, UAF candidates | Emit as SARIF, explicitly as leads for a human, never as findings. |
| `AI-37` | **Harness generation** — libFuzzer/AFL++ harness plus stub environment for a recovered function | Lets the analyst differential-test their reimplementation against the original. Closes the loop on "specification extraction." |
| `AI-38` | **Reimplementation** — clean idiomatic C or Rust implementing the recovered *specification*, not a cleanup of decompiler output | Gated by differential execution (`AI-43`). **This is the actual product, delivered.** |
| `AI-39` | **Patch-diff explanation** — two binary versions in, semantic delta out | Enormous in patch analysis and vulnerability research; incumbents (BinDiff, Diaphora) are purely structural. |
| `AI-40` | **Firmware peripheral map** — identify MMIO accesses, name against a chip memory map / SVD | Defence firmware work lives here and nothing automates it. |
| `AI-41` | **Symbol-name recovery for stripped binaries** — distinct from general naming; call graph and string references as primary evidence, propagated bottom-up (N18 already gives the ordering) | |

## 5.4 Verification and gates — where the product actually is

An AI edit you cannot verify is a liability. An AI edit you *can* verify is a feature. This is the most important section in Part 5, and the current stack is one syntax check plus one AST check.

| ID | Action |
|---|---|
| `AI-42` | **Replace subprocess `cc -fsyntax-only` with in-process Clang.** Once LLVM 22 + Clang is the pin, `clang::CompilerInstance` gives you: real semantic analysis, not syntax; structured `Diagnostic` objects with source ranges instead of scraped text; no `fork`/`execvp` and therefore no sandbox problem; no dependence on whatever `gcc` the customer has; 10–100× lower latency, which matters when gating thousands of edits. **This single change upgrades the weakest gate into the strongest and removes a security surface. It is the highest-leverage item in this document.** |
| `AI-43` | **Implement the differential gate for real.** `C-NEURAL-DIFF` is `withdrawn` — the gate warns and skips — while the README claims it. Build it: compile original and refined to shared objects; generate typed inputs from recovered parameter types (fuzz-style plus edge cases: 0, 1, −1, INT_MAX, NULL, unaligned); execute both in a hardened sandbox (separate process, seccomp allowlist, no filesystem, no network, no fork, address-space cap, wall-clock cap, `setrlimit` everywhere); compare return values, output buffers, observable side effects; accept only on N/N agreement. Turns "an AI suggested a change" into "a change was empirically verified equivalent on N inputs" — a categorically different claim. |
| `AI-44` | **Full compile, then link.** `-fsyntax-only` → `-c` → link against the generated stubs. `RETDEC_EMIT_BUILDABLE` already achieves 216/216; route refinement through the same machinery so edits are gated at the *buildable* bar, not the *parseable* bar. |
| `AI-45` | **Symbolic equivalence gate.** For tractable functions, prove pre/post equivalence with CBMC or KLEE over the C, or SMT over the IR. `docs/FORMAL_VERIFICATION_BRIDGE.md` exists — connect it. **Verified refinement is a claim nobody else in this market can make.** |
| `AI-46` | **Extend the structural gate to invariants**, not just parseability: same set of called functions, same loop count, same return count, no new control-flow constructs, renames only where requested, no changed integer literals. An AST-diff classifier labelling each edit `rename`/`comment`/`reformat`/`semantic`, rejecting any `semantic` edit in a naming tier. |
| `AI-47` | **Metamorphic gates.** Refine twice with different seeds, require compatible results. Refine O0 and O2 builds of the same source, require consistent naming. Catches instability no single-run gate can see. |
| `AI-48` | **Type-consistency gate.** After a type edit, re-run `bin2llvmir` type propagation and check the proposal does not contradict observed access widths. The decompiler is the verifier for the model. |
| `AI-49` | **Per-edit provenance record**: tier, model SHA, prompt version, sample agreement fraction, gate results, diff hash. **The artefact must answer "which lines did the AI touch and why."** Mandatory for evidentiary use, and the feature that sells the AI to a sceptical government buyer. |
| `AI-50` | **Dual-artefact output, always.** `foo.c` (deterministic, no AI), `foo.refined.c` (AI-touched), `foo.refine.json` (edit log). Never overwrite the deterministic artefact. Make it a documented guarantee. |
| `AI-51` | **`--neural-dry-run`** emitting only the diff and edit log, applying nothing. |
| `AI-52` | **Gate-result telemetry** — pass/fail rates per tier per model per quantisation, collected locally. This is the data that says which tiers are ready to ship. |
| `AI-53` | **Remove `RETDEC_NEURAL_SKIP_COMPILE_GATE`**, or make setting it stamp `"gates_bypassed": true` at the top of the output C and in `config.json`. A silent gate bypass settable from the environment invalidates every claim in the whitepaper. |

## 5.5 Embeddings and retrieval

llama.cpp exposes pooled embeddings. This unlocks a class of features unrelated to generation and arguably more valuable.

| ID | Action |
|---|---|
| `AI-54` | **Function embeddings** for every recovered function (from the C, the IR, or both), in a local vector index — flat below a million functions, HNSW above. |
| `AI-55` | **Library identification by nearest neighbour** against a corpus of known library functions built from source at multiple optimisation levels. **Same asset as the signature database (`CORE-30`) approached from the other side, and complementary** — signatures give exact matches, embeddings give fuzzy matches across compiler versions and opt levels where byte signatures fail. Together they are a real moat. |
| `AI-56` | **Cross-binary function matching** — diff two firmware versions by embedding similarity rather than structure. Attacks BinDiff's territory with a better primitive. |
| `AI-57` | **Corpus-scale semantic search** — "find every function in this 10,000-binary fleet that looks like a CRC." No good open equivalent. |
| `AI-58` | **Clone and vendored-code detection** within and across binaries. |
| `AI-59` | **Retrieval-augmented refinement** — before refining F, retrieve the k most similar functions from a corpus where the source *is* known, and include those (decompiled, original) pairs as dynamic few-shots. Highest-value use of retrieval here; makes a 9B perform far above its size on familiar code. N19 is currently "audit-only, no embedding corpus" — build the corpus. |
| `AI-60` | **Deduplicate refinement work** by embedding similarity, not just exact hash. Most functions in a real binary are repeated library code. |
| `AI-61` | **Triage ranking by distance from known-benign library code.** The unusual functions are the interesting ones. A ranked entry point into a huge binary is a genuine product feature. |

## 5.6 Agentic and tool-using analysis

Where a 9B model punches far above its weight: give it tools and let it gather evidence rather than guess from a static prompt.

| ID | Action |
|---|---|
| `AI-62` | **Expose a tool surface**, each against the C ABI (`CORE-36`): `disassemble(addr,n)`, `read_bytes`, `xrefs_to`, `xrefs_from`, `get_strings`, `get_type`, `get_function_c`, `get_callers`, `search_bytes`, `run_compile_gate`, `run_diff_gate`. |
| `AI-63` | **Bounded agent loop for hard functions.** Cheap single-shot tiers handle the easy 90%. For the residual — gate failures, low confidence — run an agent with a hard budget (~12 tool calls) fetching assembly, checking callers, reading referenced data, iterating against the compile gate. Every tool call GBNF-constrained so malformed calls are impossible. |
| `AI-64` | **Self-repair loop.** On compile-gate failure, feed the Clang diagnostics back (with source ranges — free once `AI-42` lands) and retry up to k times. Converts a large share of gate failures into passes; one of the highest value-per-line items here. |
| `AI-65` | **Whole-binary agentic triage** — "find the network parsing code in this firmware." The agent uses strings, imports, xrefs, and embeddings and reports a ranked answer with evidence. **This is the demo that sells the product.** |
| `AI-66` | **Interactive analyst agent in the GUI.** `AIAssistantPanel` already exists; this is what it should do — answers with clickable references into the listing. |
| `AI-67` | Multi-agent decomposition for large functions (structure → naming → types → reviewer). Only after the single-agent path is measured. |
| `AI-68` | **Tool-call audit log** into the artefact — every call, arguments, results. Non-negotiable for evidentiary use and it makes the agent debuggable. |

## 5.7 Training data and fine-tuning — the moat

Everything above uses an off-the-shelf model and is therefore not defensible against a competitor who also downloads Qwen. **This section is the moat**, and you are unusually well positioned because you already own the data-generation pipeline.

| ID | Action |
|---|---|
| `AI-69` | **Build the training-pair generator.** You have a decompiler, gcc, clang, and DWARF. For any project you can build, you get `(original source function, decompiled C function)` pairs **for free**, aligned via debug info. Pipeline: fetch → build with `-g` at O0/O1/O2/O3/Os → decompile → align via debug info → emit pairs. Run over Debian source packages and you have **millions of aligned pairs across compilers, optimisation levels, and architectures.** Nobody else in this market has this. A week to prototype. |
| `AI-70` | **Derive every tier's training set from it.** The original source is ground truth for naming, comments, types, struct layouts, summaries, and reimplementation — the exact targets of `AI-25` through `AI-41`. You are not labelling by hand; the compiler labelled it for you. |
| `AI-71` | **LoRA fine-tune Qwen 3.5 9B.** A domain adapter on a few hundred thousand pairs will beat the base model by a wide margin, because decompiled C is a peculiar dialect the base model has seen little of. Ship the adapter alongside the base GGUF. **A competitor can download Qwen; they cannot download your adapter.** |
| `AI-72` | **Per-tier adapters.** Small LoRAs are cheap to train and hot-swappable at runtime in llama.cpp. A naming adapter, a types adapter, a summary adapter will each outperform one general adapter. |
| `AI-73` | **Distil from a larger teacher** on the tasks where source alone is not the answer (summaries, explanations, struct naming). Standard, effective, and it makes your 9B behave like something much larger on your specific tasks. |
| `AI-74` | **Train on gate outcomes (rejection sampling / DPO).** You have an automatic reward signal: does the edit pass compile + structural + differential? Sample many candidates, keep passers, train, iterate. **Your gate stack *is* the reward model.** Very few people building on decompilers have this and you built it accidentally. |
| `AI-75` | **Hold out by project, not by function.** Functions from the same project leak heavily. Freeze projects never used in training and report only on them. (Do not repeat `BENCH-04`'s failure here, where it is far more consequential.) |
| `AI-76` | **Train the embedding model too** — contrastive on (same function, different opt level) positives and (different function) negatives. Same corpus, small extra cost, large gain for `AI-54`–`AI-61`. |
| `AI-77` | **Publish a benchmark and the corpus generator.** Attracts research attention, citations, and credibility, and costs nothing you are keeping — the *adapter* is the asset, not the recipe. How a small vendor gets taken seriously by a government technical evaluator. |
| `AI-78` | **Version and sign every adapter**: SHA-256 into the allowlist, training-data manifest, eval results per version. An unversioned model is an unauditable model. |
| `AI-79` | **Document training-data provenance and licensing.** If you train on GPL source, be clear-eyed and get advice. Genuinely unsettled area and exactly what DD asks about. Prefer permissive corpora for anything that ships. |

## 5.8 Evaluation

| ID | Action |
|---|---|
| `AI-80` | **Build the eval harness before building more tiers.** A few hundred functions with known source; per-tier metrics: naming accuracy vs the DWARF name, type match rate, gate pass rate, edit acceptance rate, semantic-change rate (should be zero for safe tiers), tokens per function, wall time per function. Nightly, versioned, bootstrap CIs. Without this every tier above is a guess. |
| `AI-81` | **Human evaluation set** — 20 functions, three reverse engineers, blind pairwise preference, deterministic vs refined. Expensive, slow, the only metric matching what a customer means by "better." Quarterly. |
| `AI-82` | **Ablation table** in the whitepaper: base vs +naming vs +types vs +full stack, on automatic and human metrics. Without an ablation the AI feature is unfalsifiable marketing. |

## 5.9 Security of the AI path

The existing posture is good — fail-closed allowlist, compile-time-gated mock, network override required. These harden the rest.

| ID | Action |
|---|---|
| `AI-83` | **Treat all binary-derived content as hostile.** `stripCStringLiterals` handles literals and comments. Also neutralise: identifier names lifted from symbol tables (a symbol can be named `ignore previous instructions`), section names, DWARF strings, import names, and YARA match text — all of which reach the prompt through `serializeSemanticContext`. Delimit untrusted spans explicitly and instruct the model that content inside them is data. |
| `AI-84` | **Bound every edit to the target function's AST subtree.** Enforce structurally in the applier, not by asking the model nicely. |
| `AI-85` | **Sandbox the differential gate hard.** It executes code derived from an attacker-supplied binary as transformed by a model. Single most dangerous thing in the product: separate process, seccomp-confined, no network, no filesystem, resource-capped, hard timeout. |
| `AI-86` | **Model supply chain**: signed manifests, not just SHA-256; verify GGUF architecture and quantisation match expectation; refuse unknown metadata keys; document offline verification. |
| `AI-87` | **HMAC the refinement cache** (same reasoning as `CACHE-04`). |
| `AI-88` | **Air-gap CI job** — run the whole neural path with networking disabled at the kernel level and assert success. `RETDEC_NO_NETWORK` exists; prove it. |
| `AI-89` | **Threat-model the AI path** in `docs/THREAT_MODEL.md`: prompt injection, model poisoning, cache poisoning, gate bypass, resource exhaustion, information leakage (prove nothing leaves the machine). |

## 5.10 Product surface

| ID | Action |
|---|---|
| `AI-90` | Every neural setting gets a CLI flag and a `config.json` key (`CFG-01`; neural is 30+ of the 100 env vars and the worst offender). |
| `AI-91` | **Populate `support/models.json`.** It is `{"models": []}`, so the feature refuses to load anything on first contact. Add real checkpoints with SHA-256s and publish where to get them. |
| `AI-92` | **`--neural-selftest`** — load, tokenize, generate one token, run each gate against a fixture, print a pass/fail table, exit. Every AI feature needs a hello-world. |
| `AI-93` | **`--neural-explain <function>`** — full prompt, raw generation, sample agreement, every gate result for one function. Indispensable for support and for your own debugging. |
| `AI-94` | **`--neural-budget <seconds|tokens>`** with graceful degradation — refine highest-value functions first (ranked by `AI-61`), stop when spent. |
| `AI-95` | **Progress reporting** — refined, accepted, rejected-by-gate, tokens, ETA. A four-hour silent run is unusable. |
| `AI-96` | **Neural results in the GUI** as an annotation layer with per-edit accept/reject and the evidence behind each. |
| `AI-97` | **Make the refusal message actionable** — print the computed SHA-256 of the supplied model and the exact JSON line to add. |
| `AI-98` | **End-to-end CI job** (nightly, small model) proving the whole neural path works. `tests/neural/mock_test.cpp` tests the scaffolding; nothing tests the real thing. |
| `AI-99` | **Document the hardware envelope** — RAM and wall time per 1,000 functions at each quantisation, CPU-only and GPU. Customers size machines before they buy. |
---

# PART 6 — LLVM and Clang to latest, and the `retdec.pointee` port

Your `docs/internal/UNBLOCKED-MIGRATION.md` is a good plan. This part extends it rather than replacing it. Its non-negotiables are correct and I would keep all of them: never edit `deps/llvm/`, one pin change per commit, metadata before bump, no test loosening, build after every C++ edit.

## 6.1 Why this is the critical path

Almost everything else in this document sits behind it:

- **Type recovery** is dead on arrival under opaque pointers unless the pointee facts are captured first — and its replacement (Retypd) is *better*, so this is an upgrade forced by the migration, not a cost of it.
- **Clang** gives you an in-process compile gate (`AI-42`), a real C parser, an AST semantic differ, and a mechanical correctness oracle (`CORE-04`).
- **`ScalarEvolution`, `LoopInfo`, `MemorySSA`, `AliasAnalysis`** are what the detectors need to stop being opcode counters.
- **Fourteen versions of `opt` pass improvements** improve output quality for free.

And the cost grows monotonically: 367 files touch `llvm::` today, against a target that keeps moving.

## 6.2 Track 1 — `retdec.pointee` as the source of truth

The design is right. LLVM 8 still has typed pointers, so snapshot the facts into instruction metadata **now, while the information exists**, and make every reader consult metadata first. `llvm_utils::pointeeType` already implements MD-first with typed-pointer fallback. What remains is coverage and durability.

Inventory (migration doc, cross-checked against the tree):

| Category | Count | Notes |
|---|---|---|
| `getPointerElementType` | 25 | readers need MD-first |
| `PointerType::get(` | 78 | 21 in the x86 lifter alone |
| Pointer `getElementType` | ~20 | `llvmir2hll` converters |
| Implicit `CreateLoad` | 46 | all lifter backends |
| Implicit `CreateStore` | 44 | same |
| Typed `CreateLoad(type, ptr)` | 1 | `struct_recovery.cpp:371` — already correct |
| `isValidElementType` | 4 | — |

| ID | Action |
|---|---|
| `LLVM-01` | **Complete writer coverage.** Every `IntToPtr` and implicit load/store in `capstone2llvmir` attaches `retdec.pointee`. One architecture per commit as planned. The lifter always has `lty` in hand, so this is mechanical — but it must be exhaustive, because a missing attachment becomes silent type loss after the bump. |
| `LLVM-02` | **Debug-build coverage assert.** Any `IntToPtr`, `Load`, or `Store` created without `retdec.pointee` triggers an assert naming the creation site. Run the corpus under it. This is how you find the sites the inventory missed, and it is far cheaper than discovering them as post-bump regressions. |
| `LLVM-03` | **Metadata-survival test.** Lift, run the full pass pipeline, assert every attached `retdec.pointee` is still present and still parses. **LLVM passes drop unknown metadata freely** — `SimplifyCFG`, `InstCombine`, and instruction replacement all will. This is the failure mode that costs weeks if found late. |
| `LLVM-04` | **Preserve across replacement.** Wherever RetDec does `replaceAllUsesWith` or rebuilds an instruction, propagate `retdec.pointee`. Add `replaceInstPreservingRetdecMD` and route all replacements through it. |
| `LLVM-05` | **Reconsider the payload format.** The plan uses an `MDString` of the printed type, parsed with `stringToLlvmType`. Works and reuses existing code, but it is slow (parse per read) and fragile across versions where type printing changes. Consider a structured `MDNode` (kind tag + size + element reference), or at minimum cache the parse per instruction. Measure first — if reads are rare, simpler is better. |
| `LLVM-06` | **Extend the scheme beyond pointee types.** You will need all of these after the bump and the information exists only now:<br>• `retdec.gep.srcty` — GEP source element type (mandatory operand in LLVM 15+)<br>• `retdec.alloca.ty` — pin it for safety across passes<br>• `retdec.call.fnty` — indirect-call function type, which opaque pointers delete and `param_return` needs<br>• `retdec.byval.ty` / `retdec.sret.ty` — ABI attribute types, now required explicit<br>• `retdec.global.ty` — global pointee for the `IrModifier::getGlobalVariable` path<br>• `retdec.deref.size` — observed access width |
| `LLVM-07` | **Snapshot the *evidence*, not only the conclusion.** For every load/store record access width and alignment as metadata. When type recovery is later rewritten (`CORE-02`), the evidence is what it needs — and it will have survived the bump. A small addition now that prevents a much larger loss later. |
| `LLVM-08` | **Document the metadata contract** — kinds, payloads, writers, readers, invariants, behaviour on absence. This becomes a load-bearing internal API; write it down once rather than inferring it from 90 call sites. |

## 6.3 Track 2 — the bump

| ID | Action |
|---|---|
| `LLVM-09` | **Inventory, do not guess.** Extend `scripts/inventory_llvm_apis.sh` to diff your used symbol set against the target's headers and emit a per-file work list. Guessing at breakage across 14 major versions is how migrations fail. |
| `LLVM-10` | **Hop, do not leap.** The current plan targets a single pin change to ~22. With 367 files touching `llvm::`, that is a branch you will abandon. Go **8 → 15** (opaque pointers available but optional, typed pointers still work) **→ 17** (opaque mandatory) **→ 22** (latest). Each hop is independently green and independently revertible. This is the one place I would soften the existing plan. |
| `LLVM-11` | **New pass manager as its own commit family**, doable on the old pin via the legacy bridge. ~40 `RegisterPass<>` sites, `legacy::PassManager` in three functions, JSON pass names in `decompiler-config.json` and `profiles/*.json`. **Keep the JSON names stable** so customer profiles do not break — map names to `PassBuilder` registrations through a table. |
| `LLVM-12` | **Explicit type operands everywhere**: `CreateLoad(ty, ptr)`, `CreateStore`, `CreateGEP(srcTy, ...)`, `CreateInBoundsGEP`. Mechanical once Track 1 has recorded the types. |
| `LLVM-13` | **Dual-pin CI matrix during the migration window** — build against both pins, run both suites, gate on both. Do not add a permanent `RETDEC_LLVM_NEXT` flag as a drive-by (the plan is right about that); a temporary matrix is worth its cost. |
| `LLVM-14` | **Adopt the analyses you reimplement**: `DominatorTree`, `LoopInfo`, `ScalarEvolution`, `MemorySSA`, `AliasAnalysis`, `DemandedBits`, `ValueTracking`, `LazyValueInfo`. `src/ssa/domtree.cpp` and `src/alias_analysis/steensgaard.cpp` become deletable. |
| `LLVM-15` | **Use `opt` pipelines as preprocessing.** Modern `InstCombine`, `SROA`, `GVN`, `EarlyCSE`, `Reassociate`, `SimplifyCFG` on lifted IR before `llvmir2hll` measurably improves output for free. Fourteen versions of improvements sit unused. |

## 6.4 Clang as a first-class asset

The migration doc treats Clang as *"not a separate pin; it lives in that monorepo."* That badly undersells it. **Linking Clang gives you five capabilities you currently lack or fake.**

| ID | Action |
|---|---|
| `LLVM-16` | **In-process compile gate** — see `AI-42`. Replaces `fork` + `execvp` + text scraping with `clang::CompilerInstance` and structured diagnostics. |
| `LLVM-17` | **A real C parser for the whole product.** You currently pin tree-sitter-c 0.24.2 plus the tree-sitter runtime specifically to walk the C AST in `gates.cpp` and `decompile_hook.cpp`. Clang's AST is strictly better — full semantic information, type resolution, scope handling — and deletes two dependencies. Keep tree-sitter only for error-tolerant parsing of *invalid* C, which is a legitimate reason; the structural gate should use Clang. |
| `LLVM-18` | **Round-trip verification.** Parse your own emitted C with Clang, lower it back to LLVM IR, compare against the IR you decompiled from. **A mechanical correctness oracle for the entire decompiler** — no human labelling, no corpus, no ground truth. Run it over every binary in CI and you have a decompiler-correctness metric nobody else publishes. *Quite possibly the best single idea in this document for core quality.* |
| `LLVM-19` | **Real semantic diff for the AI gates** — compare Clang ASTs of original and refined C to classify edits precisely. Far more reliable than tree-sitter node counting (`AI-46`). |
| `LLVM-20` | **Better `--buildable` output** — validate generated headers and stubs with Clang before writing, and emit fix-its when validation fails. Your best feature gets more robust. |
| `LLVM-21` | **Clang's tooling for free**: `clang-format` the output (you ship `.clang-format` already), `clang-tidy` it as a readability metric, `ASTMatchers` for declarative idiom recognition instead of hand-written matchers. |
| `LLVM-22` | **Emit C++ properly.** A real C++ backend (`src/cxx_backend/`, 971 LOC, zero consumers, while `--output-lang cpp` ships renamed C) becomes far more tractable with Clang available to validate what you emit. Resolves `DEAD-01`/`CLI-01` positively. |

## 6.5 Toolchain modernisation alongside

| ID | Action |
|---|---|
| `LLVM-23` | `CMAKE_CXX_STANDARD` 17 → 20 (or 23, given you write C++23 elsewhere). Ranges, concepts, `std::span`, `<bit>`, designated initialisers — large cleanup of the new modules. |
| `LLVM-24` | `cmake_minimum_required` 3.13 → 3.26 to match `CMakePresets.json`; delete the `CMAKE_POLICY_VERSION_MINIMUM=3.5` shims. |
| `LLVM-25` | Capstone 5.0.9 → latest. New architectures, new instruction coverage, bug fixes — directly improves lifting. |
| `LLVM-26` | Keystone 0.9.2 (2019) — bump or drop. It is GPL-2.0 and excluded from commercial packages anyway; re-examine whether keeping it earns its cost. |
| `LLVM-27` | `clang-format-18` → match the new host toolchain, as its own commit. |

---

# PART 7 — Core decompiler roadmap

## 7.1 Output quality — the thing customers actually read

`llvmir2hll` is 95,971 LOC, the largest module by 2.3×, and it is where LLVM IR becomes C. Investment has gone to detectors; the artefact customers look at is stock RetDec 2022.

The `goto` baseline is a genuine strength worth building on: mean **1.44** across 27 samples, **zero at O0**, with regressions clustering at O3 (`mergesort-gcc-O3`: 15; `binary_search-gcc-O2/O3`: 4 each).

| ID | Action |
|---|---|
| `CORE-01` | **Port SAILR structuring** (USENIX Security '24). Current state of the art in goto reduction, directly attacks your O2/O3 regressions. `docs/internal/retypd_sailr_llvm.md` already scopes it. |
| `CORE-02` | **Port Retypd type recovery.** Use-based inference over recorded access widths and constraints is strictly stronger than reading declared types, because declared types in lifted code were always a fiction. Enabled by `LLVM-07`. |
| `CORE-03` | **Evaluate Rellic** (`docs/internal/rellic_evaluation.md` exists) as an alternative backend, or adopt its structuring. |
| `CORE-04` | **The Clang round-trip oracle** as the primary correctness gate (`LLVM-18`). |
| `CORE-05` | **Variable scoping and lifetime analysis** — declare at first use in the tightest scope rather than all at function top. Enormous readability win, purely mechanical. |
| `CORE-06` | **Expression re-association for readability**, not optimisation. Pick the form a human wrote. |
| `CORE-07` | **Recover `for` from `while`+increment**; extend `switch` recovery beyond what `if_to_switch_optimizer.cpp` handles. |
| `CORE-08` | **Recover short-circuit `&&`/`||`** from compiler-generated branch structure. Very common, very ugly when missed. |
| `CORE-09` | **Recover ternaries** from diamond CFGs with a single phi. |
| `CORE-10` | **Recover compound assignment** (`x += y`) and increment/decrement forms. |
| `CORE-11` | **Struct and array access recovery** — `*(int*)(p + 24)` → `p->field` / `arr[6]`. Depends on `CORE-02`; it is the payoff. |
| `CORE-12` | **Signedness recovery** from comparison and division instructions. RetDec gets this wrong often and it silently changes what the reader believes the code does. |
| `CORE-13` | **Float and SIMD recovery.** Vector code currently decompiles to unreadable intrinsic soup. Recognise horizontal sums, saturating arithmetic, shuffles; emit readable equivalents or named helpers. |
| `CORE-14` | **A readability metric suite, gated in CI**: goto count, variable count, cyclomatic complexity delta vs source, max expression nesting, cast count, intrinsic-call count. You already gate gotos — generalise. "40% fewer gotos than stock, measured" survives DD. |
| `CORE-15` | **Human evaluation set** — 20 functions, three reverse engineers, blind rating of fork vs stock vs Ghidra vs Hex-Rays. The only measurement matching what a buyer means by "better output." |

## 7.2 Analysis capability

| ID | Action |
|---|---|
| `CORE-16` | **A real symbolic executor** over the lifted IR. Consolidate `mini_emu` and `llvmir-emul` into one and make it capable. Needed for deobfuscation (`AI-35`), opaque-predicate resolution, hard jump-table recovery, and the symbolic equivalence gate (`AI-45`). |
| `CORE-17` | **Indirect-call and vtable resolution** — combine RTTI (`src/rtti/`, `src/rtti-finder/`), the symbolic executor, and value-set analysis. Unresolved indirect calls are the largest single source of missing call-graph edges, and every downstream analysis suffers. |
| `CORE-18` | **Value-set analysis / abstract interpretation** for pointer and range recovery. Prerequisite for real struct recovery and bounds reasoning. |
| `CORE-19` | **Jump-table recovery hardening** across compilers and optimisation levels. Currently fragile, and it silently truncates the CFG when it fails. |
| `CORE-20` | **Exception-handling recovery** for Itanium ABI and SEH. `src/eh_reconstruct/` exists at 2,872 LOC; extend and test against real binaries. |
| `CORE-21` | **PIC and relocation correctness sweep** — a common source of wrong addresses in shared objects. |
| `CORE-22` | **TLS, thread-local, and per-CPU variable recovery.** |
| `CORE-23` | **Inline-assembly recovery** — recognise `asm` blocks and preserve them rather than lifting them into nonsense. |
| `CORE-24` | **Self-modifying and packed code** — extend `tryEmulationUnpacking`, and critically, fuzz the unpackers (`FUZZ-04`). |

## 7.3 Detector redesign

Part 2 `B4` covers the crypto specifics. The structural problem is deeper: every detector has the same shape — count opcodes, look for constants, add weighted floats, threshold. **That shape cannot express the evidence that actually identifies an algorithm.**

| ID | Action |
|---|---|
| `DET-04` | **Evidence graph per detection.** Nodes are observations (constant table at `0x4010a0`; loop with trip count 10; callee `_mm_aesenc`; xref from `.rodata`), edges are support relations, the verdict is a rule over the graph. Serialise the graph into the output. This is what `--explain` needs and what an analyst will trust. |
| `DET-05` | **Evidence classes with different epistemic weight**: `symbol` (certain), `data-fingerprint` (near-certain), `structural` (heuristic), `statistical` (weak). Never mix classes into one float. The B7 work discovered this need; formalise it. |
| `DET-06` | **Require conjunctive evidence across ≥2 classes** before emitting at a reportable confidence. |
| `DET-07` | **Add an `abstain` verdict**, distinct from "not detected." |
| `DET-08` | **Move detection to the region level**, not function level. Inlining at O2/O3 destroys function-level detection. |
| `DET-09` | **Data-flow-based detection** — taint from a candidate key buffer to a candidate ciphertext buffer through a candidate round function. What actually distinguishes AES from a hash. |
| `DET-10` | **Use `ScalarEvolution` for trip counts** (`LLVM-14`). Round count is the strongest single structural signal for crypto: AES 10/12/14, SHA-256 64, SHA-512 80, MD5 64, DES 16, ChaCha20 20, Blowfish 16. You cannot currently compute it. |
| `DET-11` | **Function-similarity channel** — hash functions from known libraries and match. Far higher precision than any heuristic; the technique that actually works in industry. Converges with `AI-55` and `CORE-30`. |
| `DET-12` | **Publish per-detector precision/recall on a real corpus and delete anything below a floor** (say 0.5). A detector with measured precision 0.000 is not a feature; it is a liability that will be quoted back at you. `pattern_detect` and the container detectors are the likely casualties. **Cutting them is a strength signal, not a weakness.** |
| `DET-13` | **Centralise the 39 hardcoded float thresholds** into one versioned `detector_thresholds.json`, loaded at runtime, with the fitting procedure documented. |
| `DET-14` | **`--calibrate` mode** re-fitting thresholds against a labelled corpus and writing the JSON. |
| `DET-15` | **Isotonic-regression-calibrated probabilities** once a labelled set exists. Textbook, and it turns a meaningless float into a real one. |
| `DET-16` | **`--explain <function>`** dumping the full evidence chain for one detection. What a reverse engineer actually needs, and no competitor offers it well. |
| `DET-17` | **Version the detection schema** (`schemaVersion: 1`) in `config.json`. |

### Crypto specifically

| ID | Action |
|---|---|
| `DET-18` | **Replace immediate-scanning with `.rodata` table fingerprinting** across all 11 detectors: AES S-box and Te0–Te3/Td0–Td3 T-tables, DES S-boxes and PC/IP tables, Blowfish P-array and S-boxes, SHA-256 K constants (64 words, unmistakable), MD5 T-table, CRC polynomial tables. Table matching is near-zero-FP; immediate matching is near-100%-FP. |
| `DET-19` | **Cross-reference confirmation** — table present in `.rodata` **and** loaded by the candidate function. |
| `DET-20` | **Detect AES-NI properly.** The intrinsic will not appear as a named call in a stripped binary — it appears as raw `AESENC`/`AESENCLAST`/`AESKEYGENASSIST` opcodes. Match at the Capstone layer, not the callee-name layer. |
| `DET-21` | **ARMv8 crypto extensions** (`AESE`, `AESMC`, `SHA256H`) — you claim ARM support. |
| `DET-22` | **Vectorised paths**: AVX2 ChaCha20, VAES, GHASH via `PCLMULQDQ`. Modern crypto libraries do not use the scalar path. |
| `DET-23` | **Distinguish implementation from use.** A function calling `EVP_EncryptUpdate` *uses* AES; a function containing the S-box *implements* it. Completely different findings, different output kinds. |
| `DET-24` | **Post-quantum primitives** — ML-KEM (Kyber), ML-DSA (Dilithium), SLH-DSA (SPHINCS+). Defence customers are actively inventorying PQC readiness and nobody's decompiler tells them where their crypto is. **Genuine differentiator with correct market timing.** |
| `DET-25` | **CBOM output in CycloneDX 1.6** — the standard added crypto-asset support for exactly this. A standards-compliant deliverable for exactly your customer. |
| `DET-26` | **Flag weak/deprecated primitives** — MD5, SHA-1, DES, RC4, ECB, hardcoded IVs. "Where is the broken crypto in this legacy binary" is a question agencies pay for. |
| `DET-27` | **Detect hardcoded keys and IVs** — high-entropy constant arrays flowing into key-schedule functions. |

### Other detectors

| ID | Action |
|---|---|
| `DET-28` | **Containers**: base detection on allocation-site + layout, not opcode counting. `std::vector` is three pointers with a specific growth pattern; `std::map` is a red-black tree with a sentinel; `std::unordered_map` is a bucket array plus a linked list. Investigate the `unordered_map<uint32_t,uint32_t>` detector first — 80 detections at precision 0.000, your worst offender by volume. |
| `DET-29` | **Short-circuit heuristics with RTTI and the demangler.** `_ZNSt6vectorIiSaIiEE9push_backERKi` tells you exactly what it is. Heuristics are a fallback for stripped binaries only, and must be *labelled* as such in the output. |
| `DET-30` | **Algorithms**: ground truth from libstdc++/libc++/MSVC STL at all opt levels; detect the actual `std::sort` shape (introsort = quicksort + heapsort at depth limit + insertion sort under 16); report complexity class as separate output. |
| `DET-31` | **Concurrency**: split into `symbol` (certain, confidence 1.0 or none) and `structural` (heuristic) channels. Fixed 0.75/0.70 confidences on symbol matches are not probabilistic. |
| `DET-32` | **Serialisation**: match generated-code shape (protobuf `_InternalParse`, varint decode loops, wire-type switch), not symbol names. |
| `DET-33` | **Design patterns**: consider dropping. GoF patterns are ambiguous even in source code with names. `C-A9` already calls it experimental; deleting is more defensible than defending. |
| `DET-34` | **`FunctionSummary` refactor.** One pass per function building a `flat_hash_set<uint64_t>` of immediates, an opcode histogram, a callee-name set, a back-edge flag. Every `hasImmediate` becomes O(1); every `countOp` an array index. Converts `O(detectors × constants × instructions)` to `O(instructions + detectors × constants)`. Large constant-factor win; ~100 lines. |
| `DET-35` | Replace `std::set<uint64_t>` for 12-element constant tables with a sorted array or fold into the summary set. |
| `DET-36` | Per-detector timing into `src/profiling/`; generate `DETECTOR_STAGE_COST.md` from measured data. |

## 7.4 Architecture and format coverage

| ID | Action |
|---|---|
| `CORE-25` | **Finish ARM64.** Documented as *"not production-ready end-to-end"* while the README lists it as supported. SIMD, atomics, BTI/PAC, and a real corpus. ARM64 is the majority of new devices; **highest-value architecture gap.** |
| `CORE-26` | **RISC-V.** No lifter, no `-a riscv`. Growing fast in defence and embedded; being early is a positioning advantage. |
| `CORE-27` | **Delete or implement SPARC / SystemZ / XCore.** They `throw GenericError` today. Stubs that throw are worse than absent entries. |
| `CORE-28` | **Rust as first-class** — panic-machinery recognition, trait-object and vtable layout, `Vec`/`String`/`Box` layout recovery, monomorphisation-aware naming. `RETDEC_USE_RUSTC_DEMANGLE` is a start, not support. |
| `CORE-29` | **Go as first-class** — `pclntab` gives full function names, file/line, and type metadata **even in stripped binaries**. An enormous amount of free ground truth almost nothing exploits. Plus the register ABI (Go 1.17+), goroutine detection, interface layout. |
| `CORE-30` | **Regenerate the signature database.** `retdec-support` is pinned to 2019-03-08. Build glibc (2.28→2.40), musl, MSVC runtimes (2015→2022), and the top ~300 vcpkg/conan packages at multiple opt levels; generate signatures with your existing `bin2pat`/`pat2yara`/`patterngen`. **Mechanical, scriptable, nobody is doing it for an open tool, and it is the highest-ROI non-AI item you have.** |
| `CORE-31` | **Publish the database as a versioned, separately-downloadable product with a documented update cadence.** Signature freshness is exactly what organisations pay an annual fee for — far more defensible than a heuristic. |
| `CORE-32` | **Version-level identification** — "this is zlib 1.2.11, which has CVE-2018-25032." |
| `CORE-33` | **Customer-side signature builder** — point it at their internal library corpus, generate private signatures. Creates switching costs. |
| `CORE-34` | **Swift** demangling plus metadata parsing. |
| `CORE-35` | **Firmware and bare-metal** — `--memory-map` for regions, peripheral naming from an SVD file, interrupt-vector recovery, linker-script-implied layout. **This is where defence work actually lives and Ghidra is mediocre at it.** |
| `CORE-36` | **Adopt LIEF** for PE/ELF/Mach-O (`docs/internal/lief_adoption.md`, `RETDEC_LIEF_SHADOW`). Deletes ~10k LOC of your highest-risk hand-written parsing and improves coverage. |
| `CORE-37` | **Modern PE features** — Control Flow Guard, CET, delay-load imports, ARM64EC, forwarded exports. |
| `CORE-38` | **DWARF 5 and split DWARF** in `debugformat`. |
| `CORE-39` | **PDB via LLVM's reader** (`RETDEC_USE_LLVM_PDB`) rather than the hand-written 2,783-line `pdbparser`. |
| `CORE-40` | **Symbol server integration** — Microsoft's public symbol server and `debuginfod`. Free high-quality ground truth for a huge fraction of real binaries. `docs/SYMBOL_SERVER.md` exists; connect it. |
| `CORE-41` | **Bounds-checked reader type** (`ByteReader` with `read<T>()` returning `std::optional`) routing all parsing. 138 raw `memcpy`/`reinterpret_cast` sites in the parsers is 138 opportunities. |
| `CORE-42` | **Hard resource budgets per parser** — max allocation, max recursion depth, max element count, wall-clock timeout — enforced centrally. |
| `CORE-43` | **Parse untrusted input in a separate process** with seccomp, or behind a clean error boundary that cannot corrupt the analysis process. |
| `CORE-44` | **Differential parser testing** against LIEF, `javap`, `wasm-objdump`, `dis`. Disagreements are bugs in one of them and you learn either way. |

## 7.5 Scale and performance

| ID | Action |
|---|---|
| `CORE-45` | Streaming / incremental analysis for huge binaries with resume from partial state. |
| `CORE-46` | Content-addressed **cross-binary** function cache — the same statically linked libc function appears in thousands of binaries; analyse once. (Fix `CACHE-01` first.) |
| `CORE-47` | Binary-level parallelism for corpus runs; make `parallelBatchDecompile` the default for directory input. |
| `CORE-48` | Memory-map inputs; set and CI-enforce a peak-RSS budget. Firmware images are large and OOM in the field is worse than slow. |
| `CORE-49` | `--timeout` per phase, not just globally. One pathological function should not kill a batch. |
| `CORE-50` | `--functions <file>` to analyse a named subset. |

## 7.6 Output artefacts and integration

| ID | Action |
|---|---|
| `INT-01` | **A stable C ABI** — `libretdec-semantics.so` with a versioned header. `docs/internal/C_ABI_SKETCH.md` is a sketch; no export exists. Prerequisite for everything below. |
| `INT-02` | **Python bindings** over that ABI. Every RE workflow in government is glued together with Python. |
| `INT-03` | **Ghidra extension.** Free tool, largest government user base by far, and analysts add tools rather than switching. Ship a `.zip` extension running `retdec-decompiler --semantic-json` and painting comments/bookmarks into the listing. **Highest-value integration by a wide margin.** |
| `INT-04` | **Binary Ninja plugin** (easiest API), then **IDA plugin** (largest paying base, hardest API). |
| `INT-05` | **SARIF 2.1.0 output.** Zero matches for "sarif" in the tree. Every government DevSecOps pipeline ingests it; each detection maps cleanly to a `result` with a `location` and `properties`. Two days of work for pipeline compatibility with everything. |
| `INT-06` | **CycloneDX SBOM of the analysed binary** — detected libraries, versions, crypto assets. "Point this at a legacy binary, get an SBOM" is a product on its own and a mandated deliverable under US EO 14028 and equivalents. |
| `INT-07` | **A versioned specification schema** — the actual claimed product. Per function: recovered signature, types, algorithm, pre/post conditions, side effects, concurrency behaviour, evidence. Publish the JSON Schema. |
| `INT-08` | **ACSL contract emission** feeding `FORMAL_VERIFICATION_BRIDGE.md`. |
| `INT-09` | **Semantic binary-diff format** so two versions compare at specification level, not byte level. BinDiff/Diaphora are the incumbents and are structural only. |
| `INT-10` | **NDJSON streaming results** and machine-readable progress on stderr (`--progress json`) so long runs are useful before completion and plugins do not parse log prose. |
| `INT-11` | **Stable, documented exit codes.** A caller currently cannot distinguish "failed to parse" from "parsed, found nothing." |
| `INT-12` | **`--profile {readable,buildable,spec}`** output profiles. |

## 7.7 GUI

19,980 LOC, 22 panels, a plugin interface (API 1.0), headless mode, batch decompilation, project files, export bundles, and a real test suite. Substantially undersold.

| ID | Action |
|---|---|
| `GUI-01` | Rewrite the README GUI section to list what actually ships (Call Graph, Type Hierarchy, Signature Studio, Diff, Binary Browser are all unmentioned). |
| `GUI-02` | **Document the plugin API publicly.** A versioned plugin interface with no documentation is a feature nobody will use. |
| `GUI-03` | **Interactive rename-and-propagate** — analyst renames a variable, the tool propagates through types and callers. The single feature that makes Hex-Rays worth its price and the clearest gap. |
| `GUI-04` | **Confidence-aware rendering** — low-confidence recovered code in a distinct style, hoverable evidence. Analysts need to know what to trust. |
| `GUI-05` | **Surface semantic detections as an annotation layer** with the evidence graph behind each. They currently land in `config.json` and comments; the GUI is where they become useful. |
| `GUI-06` | **In-process mode** once `INT-01` exists, for interactive latency. Subprocess is defensible for crash isolation but forces IPC over files and blocks incremental interaction. |

## 7.8 Managed decompilation — an unrecognised product

`managed_decompiler.cpp` handles JVM class/JAR, DEX, APK, CIL, PYC, Lua bytecode, and WASM — 8 formats with source-language output, broader than most single tools, buried in a README bullet.

| ID | Action |
|---|---|
| `MAN-01` | **Split it out and market it separately**: "one tool for Java, Android, .NET, Python, Lua, and WASM." Android and .NET analysis are large funded markets. |
| `MAN-02` | Benchmark honestly against JADX and Procyon (Java/Android), ILSpy and dnSpy (.NET), `uncompyle6`/`decompyle3` (Python), `wasm2c` (WASM). You will lose some; publishing the matrix anyway is what makes the wins credible. |
| `MAN-03` | **APK-level analysis**, not just DEX — manifest, permissions, native libs, resources. What an Android analyst actually needs. |
| `MAN-04` | **Kotlin from Kotlin-compiled DEX** via `kotlin_metadata.cpp`. The one credible use for the orphaned Kotlin emitter (`DEAD-02`). |

## 7.9 Code quality and hardening

| ID | Action |
|---|---|
| `QUAL-01` | `clang-tidy` in CI: `bugprone-*`, `cert-*`, `cppcoreguidelines-*`, `performance-*`, `readability-function-size`. New modules first, ratchet outward. |
| `QUAL-02` | Replace the 5 remaining `atoi`/`strcpy`/`sprintf` uses with `std::from_chars` / `std::format` / `std::string`. |
| `QUAL-03` | Replace the 37 raw `new` in new modules with `unique_ptr` / value semantics. |
| `QUAL-04` | Narrow or log the 2 `catch (...)` blocks. |
| `QUAL-05` | Audit the 2 `assert()` uses — compiled out in release; promote to runtime checks if they guard real invariants. |
| `QUAL-06` | `-Werror` on new modules, per-target so you need not fix upstream first. |
| `QUAL-07` | `-D_GLIBCXX_ASSERTIONS` and `-D_FORTIFY_SOURCE=3` on release builds. |
| `QUAL-08` | CFI and stack protector on release binaries; add `checksec` CI checks (RELRO, PIE, NX, canaries, fortified functions). |
| `QUAL-09` | Introduce a `Result`/`expected` error type. New modules currently use 172 bare `return false` and 8 `std::optional`, with no structured error channel. |
| `QUAL-10` | Audit thread safety of the parallel path — detectors write into shared maps. TSan under the corpus (`SAN-02`) will find this. |
---

# PART 8 — Security posture and supply chain

Separate from the code-quality items in §7.9, these are the things a government security reviewer will explicitly check for.

| ID | Action |
|---|---|
| `SEC-01` | Extend `SECURITY.md` with a coordinated-disclosure timeline and a PGP key. |
| `SEC-02` | Extend `docs/THREAT_MODEL.md` to cover the AI path explicitly (`AI-89`). |
| `SEC-03` | **Prompt injection is live and unaddressed** at the symbol-name level (`AI-83`). Strings from a hostile binary flow into the C, which flows into the prompt. |
| `SEC-04` | Pin GitHub Actions to commit SHAs (`CI-09`). |
| `SEC-05` | Sign releases; Authenticode for Windows (`REL-05`). |
| `SEC-06` | SBOM for your own build, attached to every release (`REL-06`). |
| `SEC-07` | **Reproducible builds.** "The binary you have is the source you audited" is a purchasing requirement in some programmes. |
| `SEC-08` | **CVE handling process** with published SLAs. Non-negotiable for government procurement. |
| `SEC-09` | **FIPS-boundary documentation.** You bundle OpenSSL 3.2.6; document whether it is FIPS-capable and how. |
| `SEC-10` | **Air-gapped deployment guide** — no network, no telemetry, offline signature updates via signed bundle. Test the whole posture (`AI-88`). |
| `SEC-11` | Consider **Common Criteria** or equivalent evaluation if deal size justifies it. Expensive and slow, and it locks out competitors who have not done it. |
| `SEC-12` | **Deterministic output guarantee**, tested (`CFG-07`). For evidentiary use this is a hard requirement, currently violated by `CACHE-01` and unrecorded configuration. |

---

# PART 9 — Commercial position

## 9.1 The valuation ledger

An acquirer values a codebase on **defensible capability**, not lines. The honest tally after this review:

**Assets**

| Asset | Evidence |
|---|---|
| `--buildable`: 216/216 vs stock 0/216 | `compare-fork-vs-stock-full.md` |
| 8 managed formats with source-language output | `managed_decompiler.cpp` |
| 22-panel Qt GUI, plugin API, headless mode | `src/gui/` |
| The audit apparatus | `CLAIMS.md`, `a4-calibration.md`, `b7-name-evidence.md` |
| 8,815 tests, 10 CI workflows | infrastructure that exists and runs |
| goto baseline 1.44 mean, 0 at O0 | `goto-optimizer-baseline.md` |
| Careful neural security posture | fail-closed allowlist, compile-time-gated mock |

**Liabilities**

| Liability | Evidence |
|---|---|
| Detectors: name-blind F1 0.056, 0.000 on zlib | §1.3 |
| ~13,700 LOC dead, two modules marketed | §1.7 |
| LLVM 8-era fork of a dead upstream; support pkg 2019 | §3.4 |
| 151 files chain-of-title defect; no CLA | §1.9 |
| Docs publish withdrawn numbers, hide real ones | §1.2 |
| Zero releases, zero tags, no binaries | `B8` |
| Benchmarks not reproducible by anyone | §1.5 |

## 9.2 The moat question

"A fork of a dormant MIT project plus heuristic detectors" is not a moat, and the 0.056 confirms it. Candidate moats, ranked by defensibility:

1. **A fresh, comprehensive signature database** (`CORE-30`, `CORE-31`). Data assets compound, are expensive to replicate, and support subscription pricing.
2. **The training-pair corpus and derived adapters** (`AI-69`–`AI-72`). Compiler-labelled data at industrial scale that you can produce and competitors cannot. **The single most defensible thing available to you.**
3. **`--buildable` output at scale.** If recompile rate on *real* binaries reaches any meaningful number, you own a capability no competitor has.
4. **PQC / crypto inventory as a compliance product** (`DET-24`, `DET-25`). Funded mandate, immature tooling, correct timing — but entirely dependent on fixing detection first.
5. **Plugin distribution into Ghidra/IDA/Binary Ninja** (`INT-03`, `INT-04`). Not a moat, but the channel that makes any of the above sellable.

Everything else is table stakes.

## 9.3 Positioning

| ID | Action |
|---|---|
| `POS-01` | **Lead with `--buildable`.** "Stock RetDec: 0 of 216 recompile. Ours: 216 of 216." True, measured, beats the incumbent absolutely, and currently in no public document. |
| `POS-02` | **Retire the algorithm-recovery pitch until the numbers justify it.** Selling a 0.056 detector as a capability is the fastest route to losing the room. Reposition as "experimental semantic annotation, symbolicated binaries only" and be visibly honest — that honesty buys credibility for the claims that hold. |
| `POS-03` | **Do not sell "a better decompiler."** You lose to Hex-Rays on quality and to Ghidra on price. Sell **buildable output + managed-format breadth + a semantic annotation layer delivered as a plugin into the customer's existing workflow.** |
| `POS-04` | **Publish the competitive matrix including the comparisons you lose** — Ghidra (free, NSA, huge plugin ecosystem, weak semantics), IDA Pro + Hex-Rays (expensive incumbent, gold-standard decompiler), Binary Ninja (modern, good API), angr/Rellic (research-grade). An honest matrix is more persuasive than a flattering one. |
| `POS-05` | **Record a 5-minute demo** against a real, recognisable binary. Show `--buildable` output compiling and running. That artefact does more work than the whitepaper. |
| `POS-06` | **10-page technical brief** for a non-specialist M&A partner: what it does, who buys it, what the moat is, what the risks are. Michael Quinn will not read 90 KB of `RETDEC_AUDIT.md`. |
| `POS-07` | **Rewrite `COMMERCIAL_WHITEPAPER.md`** to contain nothing that is not `demonstrated` in `CLAIMS.md`. Specifically check it does not describe CUDA acceleration as a capability. |
| `POS-08` | **`ROADMAP.md`** — public, dated, with the LLVM migration and recompile-rate targets on it. Roadmaps are how a buyer models future cost. |

## 9.4 Business model

| ID | Action |
|---|---|
| `BIZ-01` | **Three products, not one**: **Core** (decompiler + buildable, open) · **Signatures** (annual subscription — the real revenue) · **Enterprise** (plugins, support, private signature building, air-gapped deployment). |
| `BIZ-02` | Licence structure per `LEG-08`; procurement FAQ per `LEG-09`. |
| `BIZ-03` | **Prepare a DD data room now**: generated file-level `PROVENANCE.md`, `CLAIMS.md` with a verification column, SBOM, licence audit, security posture summary, and **this document with your responses**. Walking in with your own critique already answered is a strong position; being handed it by their engineer is a weak one. |
| `BIZ-04` | **`DUE_DILIGENCE.md`** pre-empting every finding here. An acquirer who finds a problem you already documented trusts you more; one who finds a problem you hid trusts nothing. |
| `BIZ-05` | **Extend `CLAIMS.md` with a `verification` column** — `automated` / `manual` / `asserted` — and attach a machine-checkable artefact (CI job name or results file) to every `demonstrated` row. Fail the build if the artefact is missing or stale. |

## 9.5 Speculative but genuinely valuable

| ID | Idea |
|---|---|
| `SPEC-01` | **PQC inventory product.** Scan a fleet of legacy binaries; output every cryptographic primitive, what breaks under a cryptographically-relevant quantum computer, and a migration priority. Defence and finance both have funded mandates and the tooling is immature. Your crypto detection, once `DET-18`–`DET-27` land, is 70% of the way there. **May be a bigger business than the decompiler.** |
| `SPEC-02` | **Firmware specialisation** (`CORE-35`). Bare-metal ARM/RISC-V, no OS, no symbols, memory-mapped peripherals. Ghidra is mediocre here and it is where defence work lives. |
| `SPEC-03` | **Semantic patch diffing** (`INT-09`, `AI-39`). "This patch changed the bounds check in the parser" is worth more than a byte diff. |
| `SPEC-04` | **Supply-chain identification** (`CORE-32`). "This firmware contains zlib 1.2.11 with CVE-2018-25032." Directly saleable, uses the database you should rebuild anyway. |
| `SPEC-05` | **Recovered-code test generation** (`AI-37`). Emit fuzzing harnesses so an analyst can differential-test their reimplementation against the original. |
| `SPEC-06` | **Lifting to a verifiable IR** — Boogie/Why3/Coq rather than C for high-assurance customers. Where "specification extraction" becomes literally true. |
| `SPEC-07` | **Corpus-scale semantic search** (`AI-57`). Enterprise-scale, nobody serves it well. |
| `SPEC-08` | **Interactive refinement loop** (`GUI-03`). What makes Hex-Rays worth its price. |
---

# PART 10 — The plan, start to finish

## 10.0 Structure

Seven phases. Each has an **entry condition**, a **workstream list**, and an **exit gate** that is objectively checkable. Do not start a phase until its entry condition holds; do not declare a phase done until its exit gate passes in CI.

The phases are sized for one person working seriously. If you have help, Phases 2–5 parallelise along the track lines; Phase 0 and 1 do not — they are short, sequential, and blocking.

```
Phase 0  Truth              3 days      docs only, zero code risk
Phase 1  Hygiene            2 weeks     legal, dead code, releases
Phase 2  Foundations        3 months    metadata port, AI throughput, corpus generator
Phase 3  Verification       3 months    Clang, real gates, eval harness, first adapter
Phase 4  Capability         4 months    LLVM latest, Retypd/SAILR, signatures, plugins
Phase 5  Product            4 months    tiers, agents, architectures, integrations
Phase 6  Ongoing            —           ratchets that never stop
```

Total to a genuinely sellable position: roughly **14 months**. To a position where you can survive technical due diligence without embarrassment: **Phase 0 + Phase 1, about two and a half weeks.**

---

## Phase 0 — Truth (3 days)

**Entry:** none. Start today.
**Risk:** zero. Every item is a documentation edit or a dead-code deletion.
**Why first:** these are the findings that end an external conversation, and none of them require engineering.

### Day 1 — the numbers

| Item | Action | Time |
|---|---|---|
| `DOC-01` | Publish 0.056 with its 95% CI in `BENCHMARKS_TABLE.md` and the README; delete 1.0 everywhere public | 1 h |
| `DOC-02` | Publish `--buildable` 216/216 vs stock 0/216 as the headline benchmark | 1 h |
| `DOC-03` | Delete the stem-fallback `mean_f1` from all public docs | 30 m |
| `DOC-04` | Reframe name-assisted as a labelled second mode, never the headline | 30 m |
| `BENCH-06` | Add a methodology note to every performance figure: Debug/WSL vs Release/Docker is not a comparison; mark the 6× as unmeasured | 30 m |

### Day 2 — the claims

| Item | Action | Time |
|---|---|---|
| `DOC-06` | Purge `retdec-qwen3-runner` and `--model` from README, user manual, Windows build doc, MinGW doc | 30 m |
| `DOC-07` | Rewrite output languages as two input-keyed tables; delete the eleven-language list | 1 h |
| — | Fix the differential-gate claim: compile gate = `-fsyntax-only`; structural = active; **differential = not implemented** | 15 m |
| `GUI-01` | Rewrite the GUI section to list the 22 panels that exist; remove the "no AI panel" line | 1 h |
| — | Add architecture-maturity labels to the input-format table: Production (x86, x86-64) / Partial (ARM, Thumb, MIPS, PowerPC) / Incomplete (ARM64) / Not implemented (SPARC, SystemZ, XCore) | 30 m |
| `LEG-13` | Correct the LLVM licence line in `NOTICE` (NCSA → Apache-2.0-with-LLVM-exception) | 10 m |
| `LEG-10` | Remove the price list from `LICENSE-COMMERCIAL` | 5 m |
| `POS-07` | Strip anything not `demonstrated` from `COMMERCIAL_WHITEPAPER.md` | 1 h |

### Day 3 — the smallest code changes and the register

| Item | Action | Time |
|---|---|---|
| `DET-01` | Delete `emittedAnnotation` — computed, consumed nowhere | 10 m |
| `REL-04` | Reconcile version numbers to one source of truth | 30 m |
| `BIZ-05` | Add a `verification` column to `CLAIMS.md`; mark `C-LICENCE` as `asserted` (it is not demonstrated) | 30 m |
| `BIZ-04` | Draft `DUE_DILIGENCE.md` covering every finding in this document with your response | 3 h |

**Exit gate:** No public document asserts anything `CLAIMS.md` marks `withdrawn`. No public document names a binary or flag that does not exist. The `--buildable` result appears above the fold in the README. Read the README end to end as a hostile reviewer and find nothing you would have to explain away.

---

## Phase 1 — Hygiene (2 weeks)

**Entry:** Phase 0 exit gate passes.
**Why now:** legal exposure and dead weight are the two things that convert "interesting" into "unfundable," and both are bounded, mechanical work.

### Track 1A — Legal (3 days)

| Item | Action |
|---|---|
| `LEG-01` | Mechanical diff against `avast/retdec` v5.0; restore the dual copyright header on all 151 files |
| `LEG-02` | CI gate: fail if any upstream-path file lacks the Avast line |
| `LEG-03` | Generate file-level `PROVENANCE.md` from the diff |
| `LEG-05`/`LEG-06` | Adopt a CLA with outbound relicensing grant; wire CLA-assistant as a required PR check |
| `LEG-07` | Assert sole authorship to date in `PROVENANCE.md` if true |
| `LEG-11` | CI licence gate: fail if Keystone symbols reach a commercial-package target |
| `LEG-12` | Document Qt LGPL compliance evidence |
| `LEG-04` | Book two hours with an Australian IP solicitor |

### Track 1B — Dead code (2 days)

| Item | Action | LOC removed |
|---|---|---|
| `DEAD-03` | Delete `src/opencl/` and `src/cuda_accel/` to a branch; note in `RESEARCH_FRONTIERS.md` | ~9,550 |
| `DEAD-02` | Delete `fsharp_emitter`, `vbnet_emitter`; keep `kotlin_emitter` only if `MAN-04` is on the plan | 1,510–3,157 |
| `DEAD-01` | Keep `cxx_backend` — Phase 4 `LLVM-22` makes it real. Mark it clearly as unwired. | — |
| `DEAD-05` | Remove `src/experimental/` or document it | — |
| `DOC-06`+ | Delete `CUDA_CAPABILITIES.md` or rewrite it as a parked-research note | — |
| `CLI-01` | Remove `cpp` from `--output-lang` until `LLVM-22` | — |

### Track 1C — Correctness (2 days)

| Item | Action |
|---|---|
| `CACHE-01` | Hash constant operand values in `computeFunctionBodyHash` |
| `CACHE-02` | Add detector-version and threshold-hash tokens to the cache key |
| `CACHE-03` | FNV-1a-64 → BLAKE3 |
| `CACHE-05` | **Differential test: cache-on vs cache-off must be byte-identical, gated in CI** |
| `CACHE-06` | Version the cache file format |
| `CFG-02` | **Make `--buildable` a CLI flag and default it ON** |

### Track 1D — Releases (3 days)

| Item | Action |
|---|---|
| `REL-02` | Docker image: `docker run imortek/retdec analyse foo.elf` |
| `REL-01` | Tag `v2.0.21`; run the release workflow; fix what breaks |
| `REL-03` | Publish Linux AppImage + tarball and Windows NSIS + zip |
| `REL-05` | Sign artefacts with cosign |
| `REL-07` | `QUICKSTART.md`: pull → analyse a bundled sample → read output, ten minutes, zero build |

### Track 1E — CI truth (2 days)

| Item | Action |
|---|---|
| `CI-01`/`CI-02` | Gate on name-blind `mean_f1_raw`; remove `--stem-fallback` from the CI script and then from the binary |
| `CI-03`/`CI-04` | Invert `check_doc_vs_code.py`; assert every documented token, flag, env var, and release artefact resolves to something real |
| `DOC-05` | Generate README feature and benchmark sections from `CLAIMS.md`; fail on `withdrawn` claims |
| `SAN-01`/`SAN-04` | Turn UBSan on; move ASan to every PR |
| `CI-09` | Pin GitHub Actions to SHAs |
| `CI-10` | Enable Dependabot and secret scanning |

**Exit gate:** `git tag` is non-empty. `docker run` works from a clean machine. Every upstream file carries the Avast notice and CI enforces it. A CLA check is required on PRs. Cache-on equals cache-off, byte for byte. `--buildable` is the default. CI gates the honest metric. The tree is ~11,000 LOC lighter.

**At this point you can safely put the repository in front of a technical due-diligence engineer.** Everything after this is building value, not removing risk.

---

## Phase 2 — Foundations (3 months)

**Entry:** Phase 1 exit gate.
**Shape:** four parallel tracks. A, B, and C are independent; D is small and opportunistic.

### Track 2A — LLVM metadata port

This is the critical path for everything in Phases 3–5. Start it first and keep it moving.

| Order | Item | Notes |
|---|---|---|
| 1 | `LLVM-02` | Debug-build coverage assert **first** — it tells you the true size of the job |
| 2 | `LLVM-01` | Writer coverage, one architecture per commit (x86 first — 21 of the 78 `PointerType::get` sites) |
| 3 | `LLVM-06` | Extend the scheme: `gep.srcty`, `call.fnty`, `byval.ty`, `sret.ty`, `global.ty`, `alloca.ty` |
| 4 | `LLVM-07` | **Snapshot access width and alignment as evidence** — Retypd needs it and it only exists now |
| 5 | `LLVM-03` | Metadata-survival test through the full pass pipeline |
| 6 | `LLVM-04` | `replaceInstPreservingRetdecMD`, routed everywhere |
| 7 | `LLVM-05` | Measure read frequency; restructure the payload only if it pays |
| 8 | `LLVM-08` | Document the metadata contract |
| 9 | `LLVM-09` | Extend the API inventory script for the LLVM 15 target |
| 10 | `LLVM-11` | New pass manager on the old pin via the legacy bridge; **keep JSON pass names stable** |
| 11 | `LLVM-10` | **Hop 1: LLVM 8 → 15** with the dual-pin CI matrix (`LLVM-13`) |

### Track 2B — AI throughput

Everything here is prerequisite to the AI being a product rather than a demo.

| Order | Item | Expected effect |
|---|---|---|
| 1 | `AI-91`, `AI-92`, `AI-97` | The feature works on first contact at all |
| 2 | `AI-01` | Batched multi-sequence decode — **8–15×** |
| 3 | `AI-02` | TU-wide prefix KV sharing — removes the dominant prefill cost |
| 4 | `AI-07`, `AI-09` | Adaptive context, thread tuning |
| 5 | `AI-13`, `AI-14` | Prompts to versioned files with few-shot exemplars |
| 6 | `AI-15`, `AI-20` | GBNF for every structured output; negative constraints in the grammar |
| 7 | `AI-16` | **Kill `FullRewrite`**; replace with structured edit scripts |
| 8 | `AI-18`, `AI-19` | Self-consistency sampling; agreement fraction as confidence — closes N17 |
| 9 | `AI-05` | Quantisation matrix measured on gate-pass rate |
| 10 | `AI-03`, `AI-04` | Speculative decoding and MTP, measured |
| 11 | `AI-06` | `retdec-neural-server` |

### Track 2C — The corpus

**Start this in week one. Everything downstream needs it and it is a week to prototype.**

| Order | Item |
|---|---|
| 1 | `AI-69` — the training-pair generator: fetch → build with `-g` at O0/O1/O2/O3/Os → decompile → align via DWARF → emit pairs |
| 2 | `BENCH-02` — pin a compiler container digest so anything built is reproducible |
| 3 | `BENCH-03` — real-software corpus: coreutils, busybox, sqlite, zlib, libpng, openssl, curl, plus Debian `-dbgsym` |
| 4 | `BENCH-01` — commit ground truth and the corpus manifest |
| 5 | `BENCH-04`, `AI-75` — **split by project, not by function**; freeze a true holdout |
| 6 | `TEST-01`, `TEST-02` — labelled crypto positives; hard negatives (zlib, LZ4, xxHash, CRC tables, PRNGs, base64, UTF-8, bitboards) |
| 7 | `TEST-03`, `BENCH-05` — per-detector precision/recall/F1 per optimisation level with CIs; confusion matrices |

### Track 2D — Opportunistic

| Item | Action |
|---|---|
| `DET-34`, `DET-35` | `FunctionSummary` refactor; measure Release-vs-Release properly and publish |
| `CI-13` | Perf ratchet on analysis wall time |
| `SAN-02` | TSan job; run the corpus with parallel analysis on |
| `FUZZ-01` | Apply to OSS-Fuzz (long lead time — file the application early) |
| `FUZZ-02`, `FUZZ-03`, `FUZZ-04` | Persist the fuzz corpus; seed from real files; add lua/CIL/zip/PDB/unpacker targets |
| `INT-05` | SARIF output — two days, unlocks pipeline compatibility |
| `CFG-01`, `CFG-03`, `CFG-04` | Configuration overhaul; stamp effective config into output |

**Exit gate:** Building on LLVM 15 with all tests green. Every `IntToPtr`/load/store carries `retdec.pointee` (assert-clean over the corpus) and it survives the pass pipeline. Neural throughput is ≥8× Phase 1 on a fixed workload. The pair generator has produced ≥100k aligned pairs from real software with a project-split holdout. Per-detector precision/recall published with CIs against the real corpus.

---

## Phase 3 — Verification (3 months)

**Entry:** Phase 2 exit gate — specifically LLVM 15 and the corpus.
**Theme:** turn "an AI suggested a change" into "a change was verified."

### Track 3A — LLVM 17 and Clang

| Order | Item |
|---|---|
| 1 | `LLVM-12` — explicit type operands everywhere (mechanical, Track 2A recorded the types) |
| 2 | `LLVM-10` — **Hop 2: LLVM 15 → 17**, opaque pointers mandatory |
| 3 | `LLVM-16` / `AI-42` — **link Clang; replace the compile gate with `clang::CompilerInstance`** |
| 4 | `LLVM-18` / `CORE-04` — **the round-trip oracle**: emitted C → Clang → IR → compare against decompiled IR, gated in CI |
| 5 | `LLVM-17` — Clang AST replaces tree-sitter for the structural gate |
| 6 | `LLVM-19` / `AI-46` — AST semantic diff classifying every edit |
| 7 | `LLVM-20` — Clang-validated `--buildable` headers and stubs |
| 8 | `LLVM-23`, `LLVM-24` | C++20, CMake 3.26 |

### Track 3B — The gate stack

| Order | Item |
|---|---|
| 1 | `AI-44` — full compile then link, via the `--buildable` machinery |
| 2 | `AI-64` — **self-repair loop**: feed Clang diagnostics back and retry. Large share of gate failures become passes |
| 3 | `AI-43` + `AI-85` — **implement the differential gate properly**, with a hard seccomp sandbox |
| 4 | `AI-47` — metamorphic gates (seed variation, O0-vs-O2 consistency) |
| 5 | `AI-48` — type-consistency gate against `bin2llvmir` propagation |
| 6 | `AI-49`, `AI-50`, `AI-51` — per-edit provenance, dual artefacts, dry-run |
| 7 | `AI-53` — remove or loudly stamp the gate-bypass escape hatch |
| 8 | `AI-52` — gate telemetry per tier/model/quant |
| 9 | `AI-83`, `AI-84`, `AI-87` — hostile-content handling at symbol level, AST-bounded edits, cache HMAC |

### Track 3C — Evaluation and the first adapter

| Order | Item |
|---|---|
| 1 | `AI-80` — **the eval harness. Build it before any new tier.** |
| 2 | `AI-70` — derive per-tier training sets from the corpus |
| 3 | `AI-71` — **first LoRA fine-tune of Qwen 3.5 9B** |
| 4 | `AI-78`, `AI-79` — version, sign, and document adapter provenance and licensing |
| 5 | `AI-82` — ablation table |
| 6 | `AI-98` — end-to-end neural CI job with a real small model |
| 7 | `AI-88` — air-gap CI job |

### Track 3D — Embeddings

| Order | Item |
|---|---|
| 1 | `AI-54` — function embeddings + local vector index |
| 2 | `AI-76` — contrastive training on (same function, different opt level) |
| 3 | `AI-55` — library identification by nearest neighbour |
| 4 | `AI-59` — **retrieval-augmented refinement** with dynamic few-shots from known-source neighbours |
| 5 | `AI-60`, `AI-61` — dedup by similarity; triage ranking |

**Exit gate:** Building on LLVM 17 with opaque pointers. The compile gate is in-process Clang. The round-trip oracle runs in CI over the full corpus and reports a correctness percentage. The differential gate is implemented, sandboxed, and its pass rate is published. The eval harness reports per-tier metrics nightly with CIs. One LoRA adapter measurably beats the base model on the harness. **`C-NEURAL-DIFF` moves from `withdrawn` to `demonstrated` with a CI artefact behind it.**

---

## Phase 4 — Capability (4 months)

**Entry:** Phase 3 exit gate.
**Theme:** the things that make the output good and the product distributable.

### Track 4A — LLVM 22 and the analysis upgrade

| Order | Item |
|---|---|
| 1 | `LLVM-10` — **Hop 3: LLVM 17 → 22** |
| 2 | `LLVM-14` — adopt `DominatorTree`, `LoopInfo`, `ScalarEvolution`, `MemorySSA`, `AliasAnalysis` |
| 3 | `SSA-04`, `LLVM-14` — delete `src/ssa/domtree.cpp` and `steensgaard.cpp` |
| 4 | `LLVM-15` — modern `opt` pipelines as preprocessing before `llvmir2hll` |
| 5 | `LLVM-25`, `LLVM-26`, `LLVM-27` — Capstone, Keystone, clang-format |

### Track 4B — Output quality

| Order | Item |
|---|---|
| 1 | `CORE-02` — **Retypd type recovery** on the evidence captured in `LLVM-07` |
| 2 | `CORE-01` — **SAILR structuring**; attacks the O2/O3 goto regressions |
| 3 | `CORE-11` — struct and array access recovery (the payoff from `CORE-02`) |
| 4 | `CORE-05`, `CORE-07`–`CORE-10`, `CORE-12` — scoping, `for`/`switch`/ternary/short-circuit/compound-assignment/signedness |
| 5 | `CORE-14` — readability metric suite gated in CI |
| 6 | `LLVM-22` / `DEAD-01` — a real C++ backend; resolves `CLI-01` positively |
| 7 | `ARCH-01`–`ARCH-05` — `SemanticPass` registry, TU splits, function-size gate |

### Track 4C — Detectors, rebuilt

| Order | Item |
|---|---|
| 1 | `SSA-01`, `SSA-02` — round-trip fidelity harness; visible skip diagnostics |
| 2 | `SSA-03` — port detectors to LLVM IR (now cheap: the analyses are there and the second IR has nothing left to offer) |
| 3 | `DET-04`–`DET-08` — evidence graphs, evidence classes, conjunctive requirement, abstain, region-level detection |
| 4 | `DET-10` — `ScalarEvolution` trip counts |
| 5 | `DET-18`–`DET-23` — crypto rebuilt on `.rodata` table fingerprinting, xref confirmation, AES-NI/ARMv8/vectorised paths, implementation-vs-use |
| 6 | `DET-28`, `DET-29`, `DET-30`, `DET-31`, `DET-32` — containers, RTTI short-circuit, algorithms, concurrency channels, serialisation |
| 7 | `DET-13`–`DET-17` — thresholds to JSON, `--calibrate`, isotonic calibration, `--explain`, schema version |
| 8 | `DET-12`, `DET-33` — **publish per-detector precision and delete everything below the floor** |

### Track 4D — Signatures and distribution

| Order | Item |
|---|---|
| 1 | `CORE-30` — **regenerate the signature database.** Highest-ROI non-AI item you have |
| 2 | `CORE-31` — publish it versioned, with a cadence, as a separate product |
| 3 | `CORE-32`, `CORE-33` — version-level ID; customer-side signature builder |
| 4 | `CORE-40` — symbol server + debuginfod |
| 5 | `INT-01` — **the stable C ABI** |
| 6 | `INT-02` — Python bindings |
| 7 | `INT-03` — **the Ghidra extension** |
| 8 | `INT-06`, `INT-07` — CycloneDX SBOM output; the versioned specification schema |
| 9 | `CI-12` — ABI stability check |

**Exit gate:** Building on LLVM 22. Retypd and SAILR landed with measured readability improvements published. Detectors run on LLVM IR; every shipped detector has published precision above the floor and everything below it is deleted. A fresh signature database ships as a versioned artefact. `libretdec-semantics.so` exists with a versioned header, and a Ghidra extension consumes it.

---

## Phase 5 — Product (4 months)

**Entry:** Phase 4 exit gate.
**Theme:** the tiers, the agents, and the coverage that make it sellable.

### Track 5A — Tiers, in dependency order

| Order | Item | Gate it needs |
|---|---|---|
| 1 | `AI-28` — function summary | none (comment-only) |
| 2 | `AI-33` — error-handling annotation | none |
| 3 | `AI-25` — type inference | `AI-48` |
| 4 | `AI-26` — struct layout | `AI-48` + `CORE-02` |
| 5 | `AI-27` — enum recovery | `AI-48` |
| 6 | `AI-29` — parameter direction and contracts | `AI-43` |
| 7 | `AI-30` — library-call ID | `CORE-30` |
| 8 | `AI-32` — crypto second opinion | `DET-18`, `AI-80` |
| 9 | `AI-31` — algorithm second opinion | `AI-80` (**do not ship earlier**) |
| 10 | `AI-34` — idiom lifting | `AI-43` |
| 11 | `AI-37` — harness generation | `AI-29` |
| 12 | `AI-38` — **reimplementation** | `AI-43` + `AI-45` |
| 13 | `AI-35` — deobfuscation | `CORE-16` |
| 14 | `AI-36` — vulnerability annotation | `INT-05` |
| 15 | `AI-39` — patch-diff explanation | `INT-09` |
| 16 | `AI-40` — firmware peripheral map | `CORE-35` |
| 17 | `AI-41` — symbol recovery for stripped binaries | `AI-71` |

### Track 5B — Agents

| Order | Item |
|---|---|
| 1 | `AI-62` — the tool surface over `INT-01` |
| 2 | `AI-63` — bounded agent loop for the hard residual |
| 3 | `AI-68` — tool-call audit log into the artefact |
| 4 | `AI-65` — **whole-binary agentic triage** — the demo that sells the product |
| 5 | `AI-66`, `AI-96`, `GUI-05` — the GUI analyst agent and the annotation layer |
| 6 | `AI-72`, `AI-73`, `AI-74` — per-tier adapters, distillation, **gate-outcome training (your gate stack is the reward model)** |

### Track 5C — Core capability

| Order | Item |
|---|---|
| 1 | `CORE-16` — the symbolic executor (consolidate `mini_emu` + `llvmir-emul`) |
| 2 | `AI-45` — symbolic equivalence gate on top of it |
| 3 | `CORE-17`, `CORE-18`, `CORE-19` — indirect calls, VSA, jump tables |
| 4 | `CORE-25` — **finish ARM64** |
| 5 | `CORE-28`, `CORE-29` — **Rust and Go as first-class** |
| 6 | `CORE-35` — firmware and bare-metal |
| 7 | `CORE-36` — LIEF adoption; `CORE-41`–`CORE-44` parser hardening |
| 8 | `CORE-20`–`CORE-24` — EH, PIC, TLS, inline asm, packed code |
| 9 | `CORE-26`, `CORE-27` — RISC-V; delete or implement the throwing stubs |

### Track 5D — Product surface

| Order | Item |
|---|---|
| 1 | `INT-04` — Binary Ninja, then IDA |
| 2 | `INT-09`, `INT-10`, `INT-11`, `INT-12` — semantic diff format, NDJSON streaming, exit codes, output profiles |
| 3 | `GUI-02`, `GUI-03`, `GUI-04`, `GUI-06` — plugin API docs, rename-and-propagate, confidence rendering, in-process mode |
| 4 | `MAN-01`–`MAN-04` — split out and market managed decompilation |
| 5 | `AI-93`, `AI-94`, `AI-95`, `AI-99` — `--neural-explain`, budgets, progress, hardware envelope |
| 6 | `DET-24`, `DET-25`, `DET-26`, `DET-27` — **PQC detection and CBOM output** |
| 7 | `SPEC-01` — package PQC inventory as its own product |

**Exit gate:** Ten or more tiers shipping with published per-tier eval numbers. Agentic triage demo working on a real firmware image. ARM64, Rust, and Go handled to a documented standard. Plugins for Ghidra and Binary Ninja published. PQC inventory + CBOM shipping.

---

## Phase 6 — Ongoing ratchets

Things that start once and never stop.

| Ratchet | Direction |
|---|---|
| Round-trip oracle correctness % | may only increase |
| Recompile rate on the real corpus | may only increase |
| Per-detector precision | may only increase; below-floor detectors are deleted |
| Name-blind F1 | may only increase |
| goto count / readability metrics | may only decrease |
| Analysis-phase wall time (Release) | may only decrease |
| Mutation score on new modules | may only increase (`TEST-05`) |
| Coverage | may only increase (`CI-07`) |
| Peak RSS on the corpus | may only increase within budget |
| Gate-pass rate per tier | tracked; regressions block release |
| Signature database | quarterly regeneration |
| Human eval (`AI-81`, `CORE-15`) | quarterly |
| OSS-Fuzz findings | zero open criticals at release |
| `CLAIMS.md` ↔ public docs | CI-enforced, always |

---

## 10.1 Critical path

```
LLVM-02 (assert) ──> LLVM-01 (writer coverage) ──> LLVM-06/07 (extend + evidence)
                                                        │
                                                        ▼
                                              LLVM-03/04 (survival)
                                                        │
                                    LLVM-11 (new PM) ───┤
                                                        ▼
                                              LLVM-10 hop 1: →15
                                                        │
                                              LLVM-12 (explicit types)
                                                        ▼
                                              LLVM-10 hop 2: →17
                                                        │
                        ┌───────────────────────────────┼──────────────────────┐
                        ▼                               ▼                      ▼
              LLVM-16/AI-42                    LLVM-18/CORE-04           LLVM-17/19
              (Clang compile gate)             (round-trip oracle)       (Clang AST)
                        │                               │                      │
              AI-64 (self-repair)              CORE-14 (readability)    AI-46 (semantic diff)
                        │                                                      │
              AI-43 (differential gate) ◄──────────────────────────────────────┘
                        │
                        ▼
              AI-74 (gate-outcome training)
                        │
              LLVM-10 hop 3: →22
                        │
        ┌───────────────┼───────────────┐
        ▼               ▼               ▼
   LLVM-14         CORE-02          SSA-03
   (analyses)      (Retypd)      (detectors→LLVM IR)
        │               │               │
   DET-10          CORE-11          DET-04..08
   (trip counts)   (struct access)  (evidence graphs)
        │               │               │
        └───────────────┴───────────────┘
                        ▼
                 DET-12 (publish precision, delete failures)
                        ▼
                 AI-25/26/27/31/32 (verified tiers)
```

**Parallel and independent of the above:**

```
AI-69 (pair generator) ──> AI-70 (tier training sets) ──> AI-71 (LoRA)
        │                                                      │
        ├──> AI-76 (embedding training) ──> AI-55 (library ID) │
        │                                        │             │
        ├──> BENCH-03 (real corpus) ──> AI-80 (eval harness) ◄─┘
        │                                        │
        └──> TEST-01/02 (labelled sets) ─────────┘

INT-01 (C ABI) ──> INT-02 (Python) ──> INT-03 (Ghidra) ──> INT-04 (BN, IDA)
        └──────────> AI-62 (tool surface) ──> AI-63 (agent) ──> AI-65 (triage demo)

CORE-30 (signatures) ──> CORE-32 (version ID) ──> SPEC-04 (supply chain)
        └──────────────> AI-30 (library-call tier)
```

## 10.2 If you only do three things

1. **Link Clang and make the compile gate real** (`AI-42` / `LLVM-16`). It upgrades the weakest link in the AI stack into the strongest, deletes a security surface, gives you structured diagnostics that make the self-repair loop trivial, and unlocks the round-trip oracle (`LLVM-18`) — a decompiler-correctness metric that requires no labels, no corpus, and no ground truth, and that nobody else publishes.

2. **Build the training-pair generator** (`AI-69`). Compiler-labelled data at industrial scale, which you can produce and your competitors cannot. Everything downstream — adapters, embeddings, evals, gate-outcome training — needs it. A week to prototype.

3. **Complete the pointee-metadata port and take the LLVM hops** (`LLVM-01`–`LLVM-10`). Not glamorous. But type recovery, structuring, the modern analyses, Clang, and the oracle all sit behind it, and every quarter you wait the 367 files touching `llvm::` drift further from a moving target.

## 10.3 If you only have three weeks

Phase 0 and Phase 1, in full. Two and a half weeks of documentation edits, legal cleanup, dead-code deletion, one cache bug, and a Docker image — and the repository goes from "would not survive due diligence" to "would."

Nothing else in this document is worth doing before those.
---

# APPENDIX A — Consolidated backlog index

Every item, by prefix, with its phase. 271 items.

| Prefix | Domain | Count | Where |
|---|---|---|---|
| `DOC` | Documentation truth | 7 | Part 2, Phase 0 |
| `LEG` | Legal, licensing, chain of title | 13 | Part 2, Phase 1 |
| `DEAD` | Dead-code removal | 5 | Parts 2–3, Phase 1 |
| `CACHE` | Cache correctness | 6 | Part 2, Phase 1 |
| `REL` | Releases and packaging | 7 | Part 2, Phase 1 |
| `CLI` | CLI surface | 1 | Part 2 |
| `CFG` | Configuration model | 7 | Part 3, Phase 2 |
| `ARCH` | Architecture and structure | 11 | Part 3, Phase 4 |
| `SSA` | The second IR | 4 | Part 3, Phase 4 |
| `TEST` | Test suite | 7 | Part 4, Phase 2 |
| `BENCH` | Benchmarks and corpus | 7 | Part 4, Phase 2 |
| `FUZZ` | Fuzzing | 5 | Part 4, Phase 2 |
| `SAN` | Sanitizers | 4 | Part 4, Phases 1–2 |
| `CI` | Continuous integration | 14 | Parts 2/4, Phases 1–2 |
| `AI` | The AI programme | 99 | Part 5, Phases 2–5 |
| `LLVM` | LLVM/Clang migration | 27 | Part 6, Phases 2–4 |
| `CORE` | Decompiler core | 50 | Part 7, Phases 4–5 |
| `DET` | Detector redesign | 36 | Parts 2/7, Phase 4 |
| `INT` | Integration and output formats | 12 | Part 7, Phases 4–5 |
| `GUI` | GUI | 6 | Part 7, Phase 5 |
| `MAN` | Managed decompilation | 4 | Part 7, Phase 5 |
| `QUAL` | Code quality and hardening | 10 | Part 7, ongoing |
| `SEC` | Security posture | 12 | Part 8, ongoing |
| `POS` | Positioning | 8 | Part 9, Phases 0–1 |
| `BIZ` | Business model and DD | 5 | Part 9, Phase 0 |
| `SPEC` | Speculative products | 8 | Part 9, Phase 5 |

## A.1 The twelve highest-leverage items

Ordered by (value × certainty) ÷ cost, not by phase.

| Rank | Item | Why |
|---|---|---|
| 1 | `DOC-01` + `DOC-02` | Two hours. Converts a repository that fails DD into one that passes it. |
| 2 | `AI-42` / `LLVM-16` | In-process Clang compile gate. Weakest link becomes strongest; a security surface disappears. |
| 3 | `AI-69` | The training-pair generator. A week to prototype, and it is the only defensible moat you can build. |
| 4 | `LLVM-18` / `CORE-04` | The round-trip oracle. A correctness metric with no labelling cost that nobody else publishes. |
| 5 | `CFG-02` | `--buildable` on by default. One line; makes your differentiator the default experience. |
| 6 | `CORE-30` | Regenerate the signature database. Mechanical, and the highest-ROI non-AI capability item. |
| 7 | `LLVM-02` | The debug coverage assert. Tells you the true size of the migration before you commit to it. |
| 8 | `AI-01` + `AI-02` | Batched decode + prefix sharing. An order of magnitude; makes the AI a product. |
| 9 | `CACHE-01` + `CACHE-05` | One-line bug fix plus the differential test that would have caught it and will catch the next one. |
| 10 | `REL-02` | Docker image. Removes the impassable first step in the evaluation funnel. |
| 11 | `INT-03` | Ghidra extension. Analysts add tools; they do not switch. |
| 12 | `LEG-01` + `LEG-05` | Chain of title and CLA. Two days that unblock the entire commercial model. |

## A.2 Deletions, consolidated

Doing all of these removes roughly 25,000 LOC and several liabilities.

| Target | LOC | Item |
|---|---|---|
| `src/opencl/` | 4,547 | `DEAD-03` |
| `src/cuda_accel/` | ~5,000 | `DEAD-03` |
| `src/fsharp_emitter/` | 777 | `DEAD-02` |
| `src/vbnet_emitter/` | 733 | `DEAD-02` |
| `src/kotlin_emitter/` (unless `MAN-04`) | 1,647 | `DEAD-02` |
| `src/ssa/domtree.cpp` | 220 | `LLVM-14` |
| `src/alias_analysis/steensgaard.cpp` | — | `LLVM-14` |
| `src/ssa/` entirely (after `SSA-03`) | 1,177 | `SSA-03` |
| `pdbparser` (after `CORE-39`) | 2,783 | `CORE-39` |
| `pe_format.cpp` + `elf_format.cpp` (after `CORE-36`) | ~7,200 | `CORE-36` |
| tree-sitter dependency (after `LLVM-17`) | dep | `LLVM-17` |
| `--stem-fallback` code path | — | `CI-02` |
| `emittedAnnotation` | — | `DET-01` |
| Below-floor detectors | ~3,700 | `DET-12`, `DET-33` |
| `src/experimental/` | — | `DEAD-05` |

## A.3 Things I would explicitly *not* do

Recorded so they are decided rather than forgotten.

- **Do not add a third IR.** `src/cfg/`, `src/cfg_structure/`, `src/mini_emu`, and `src/llvmir-emul` are already drifting toward one. Consolidate (`CORE-16`), do not multiply.
- **Do not add a permanent `RETDEC_LLVM_NEXT` flag.** Your migration doc is right. A temporary dual-pin CI matrix (`LLVM-13`) is fine; a permanent runtime toggle across two LLVM versions is not.
- **Do not ship `AI-31` (algorithm second opinion) before `AI-80`.** An unmeasured second bad channel is worse than one measured bad channel.
- **Do not defend `pattern_detect`.** GoF patterns are ambiguous in source code with names. Deleting is more defensible (`DET-33`).
- **Do not keep both GPU backends** while a doc describes them as capabilities. That specific combination is what reads as overstatement (`DEAD-04`).
- **Do not chase a general "make this nicer" AI tier.** Every AI edit needs a mechanical verifier or it is a liability (`AI-16`).
- **Do not quote a fused single-number metric again.** Per-detector, per-optimisation, with CIs (`TEST-03`).

---

# APPENDIX B — Measurement reference

Every figure in this document, with its source, for checking.

## B.1 Scale

| Measurement | Value | How obtained |
|---|---|---|
| `src`+`include` LOC | 504,305 | `find src include -name '*.cpp' -o -name '*.h' \| xargs wc -l` |
| Test LOC / cases | 185,893 / 8,815 | same over `tests/`; `grep -c '^TEST'` |
| `src/` modules | 97 | `ls src \| wc -l` |
| Scripts | 215 | `find scripts -type f \| wc -l` |
| CI workflows | 10 | `ls .github/workflows` |
| `RETDEC_*` env vars | 100 | `grep -rhoE 'RETDEC_[A-Z0-9_]+' src --include=*.cpp \| sort -u \| wc -l` |
| `getenv` sites | 74 | `grep -rn getenv src --include=*.cpp \| wc -l` |
| Files touching `llvm::` | 367 | migration doc, cross-checked |
| Git tags | 0 | `git tag \| wc -l` |

## B.2 Results files

| File | Key figure |
|---|---|
| `results/compare-fork-vs-stock-full.md` | `--buildable` 1.000 vs stock 0.000, both n=216 |
| `results/algorithm-recovery-full.json` | `mean_f1` 1.000 (name-assisted) |
| `results/algorithm-recovery-full-nameblind.json` | `mean_f1` 0.0559, CI95 0.0337–0.0828, n_boot 2000 |
| `results/algorithm-recovery-adversarial-b9.json` | 0.1111, CI95 0.0000–0.2778, n=18 |
| `results/algorithm-recovery-third-party-b10.json` | 0.0000, CI95 0.0–0.0, n=2 |
| `results/a4-calibration.md` | precision 0.000 in all three confidence bands |
| `results/b7-name-evidence.md` | decay 0.332 → 0.237 → 0.107 → 0.056 |
| `results/algorithm-recovery-gate-finding.md` | "Do not advertise 1.0"; `MIN_MEAN_F1=0.95` with `--stem-fallback` |
| `results/goto-optimizer-baseline.md` | mean 1.44 gotos, 0 at O0, worst `mergesort-gcc-O3` 15 |
| `results/b8-negative-corpus.md` | negatives are "not parsers or network stacks" |
| `results/corpus-build-recipe.md` | no pinned compiler digest; B11 holdout is the B9 source set |
| `docs/BENCHMARKS_TABLE.md` | `recompile_success_rate` 0.0; `mean_f1_raw` **1.0** in bold |

## B.3 Code measurements

| Measurement | Value | Location |
|---|---|---|
| `decompile()` length / max nesting | 490 lines / 14 tabs | `src/retdec/retdec.cpp:588–1079` |
| Largest file | 8,246 lines | `llvmir2hll/optimizer/optimizers/if_to_switch_optimizer.cpp` |
| Full-function scan loops | 117 | `grep -c 'for (uint32_t b = 0; b < fn.blockCount'` |
| Silent null skips | 336 | `if (!x) continue;` across new modules |
| Bare `return false` | 172 | new modules |
| `std::optional` | 8 | new modules |
| Structured error type | 0 | — |
| Magic float thresholds | 39 | new modules |
| Raw `new` | 37 | new modules |
| `atoi`/`strcpy`/`sprintf` | 5 | new modules |
| `memcpy`/`reinterpret_cast` in parsers | 138 | `fileformat`, `pelib`, new parsers |
| Files: Avast / Odin / Odin-only / Odin-only-in-upstream-dirs | 1,441 / 1,673 / 237 / **151** | copyright grep |
| Fuzz time per target | 30 s, empty corpus | `.github/workflows/fuzz-pr.yml` |
| UBSan | **OFF** in `asan-ubsan` | `sanitizers.yml:46` |
| `support/models.json` | `{"models": []}` | — |
| HLL writers | 1 (`c_hll_writer.cpp`) | `src/llvmir2hll/hll/hll_writers/` |
| Dependency pins | LLVM `avast/llvm@a776c2a`, Capstone 5.0.9, Keystone 0.9.2, OpenSSL 3.2.6, llama.cpp b10451, tree-sitter-c 0.24.2, support pkg 2019-03-08 | `cmake/deps.cmake` |

## B.4 Corrections from earlier review parts

For the record, since earlier drafts circulated:

| Earlier claim | Correction |
|---|---|
| "Nothing the decompiler emits compiles" | **False.** `--buildable` achieves 216/216; the 0.000 is the default `.c` path only. |
| "The fork is 6× slower than stock" | **Unmeasured.** Debug/WSL vs Release/Docker. Real ratio unknown. |
| "1,673 files have a chain-of-title defect" | **151.** Most Odin-attributed files correctly retain the Avast line alongside. |
| "There is no plugin API" | **A GUI plugin API exists** (`gui/settings/plugin_interface.h`, version 1.0). What is missing is a headless C ABI (`INT-01`). |

---

# Closing

The uncomfortable framing first, because it is the one an outside reviewer will arrive at: **this repository contains more demonstrated capability than it publishes, and less than it claims — simultaneously, on different axes.**

The `--buildable` result is a clean 216–0 win over the incumbent and appears in no public document. The algorithm-recovery F1 is 0.056 and appears in the README as 1.0. Both errors have the same root cause: an audit layer that works and a publication layer that was never wired to it. That is a much better problem than it looks, because three days with a text editor fixes most of it.

What survives as genuinely hard is a short list: the detector redesign, the LLVM migration, and the signature database. Only the third of those is on the critical path to something sellable, and it is mechanical work you can start this quarter.

Two things are worth saying plainly at the end.

The first is that **the instinct that produced `CLAIMS.md`, `gate-finding.md`, and `a4-calibration.md` is the most valuable thing in this repository.** You computed the numbers that make your own product look worst, committed them, and named them honestly. That is rarer than the code and much harder to acquire. It is also, mechanically, the reason this review could be as specific as it is — almost every finding here came from an artefact you built. Point that same instrument at the README and the publication problem disappears.

The second is about what to sell. Do not sell a better decompiler; you lose that comparison to Hex-Rays and lose the price comparison to Ghidra. Sell **buildable output, managed-format breadth, and a verified semantic layer delivered as a plugin into the workflow the analyst already has** — with the signature database as recurring revenue and the fine-tuned adapter as the thing a competitor cannot copy. That is a defensible story, it is mostly work you have already started, and the measurements to support it are within a quarter's reach.

Start with Phase 0. It takes three days and it changes the conversation.
