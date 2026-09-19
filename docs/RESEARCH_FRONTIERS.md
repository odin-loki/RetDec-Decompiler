# RetDec — Research Frontiers (Tier 7)

Long-horizon research and speculative engineering. **Not scheduled for product
sprints. Not shipped at v2.0.22.**

For shippable work see [ENGINEERING_ROADMAP.md](internal/ENGINEERING_ROADMAP.md)
(Tiers 1–5). For what the product actually is — specification extraction,
buildable C, input-keyed outputs, Qt 6 GUI, optional llama.cpp, Linux / macOS /
Windows installers — see [README.md](../README.md) and [CLAIMS.md](CLAIMS.md).

Roadmap overlap (STL detectors vs reconstruction quality, architecture gaps):
[future_directions.md](future_directions.md).

---

## 1. Whole-program concurrency model

**Goal:** Infer lock ownership, shared-state protection, and data-race hints —
not just individual `pthread_mutex_lock` calls.

v2.0.22 already ships **primitive** concurrency detection (`src/concurrency_detect`:
mutex, thread, atomic). Reconstructing the *model* (which thread owns which
lock) is this research item.

| Approach | Notes |
|----------|-------|
| Lock graph | Nodes = mutexes + threads; edges = acquire/release pairs per function |
| Escape analysis on SSA | Track which heap objects flow into which lock regions |
| Happens-before from atomics | C++11 memory_order patterns → partial order |

**Open problem:** Cross-DLL locks, lock-free structures, and custom spinlocks without symbols.

**Success metric:** On a synthetic multi-threaded corpus, ≥80% of protected globals correctly attributed to a lock.

---

## 2. C++20 coroutine recovery

Compiled `co_await` lowers to a **state machine** with suspend/resume points and heap-allocated coroutine frames.

**Signals:**
- Repeated switch on a frame index field
- `operator new` for frame + destructor on final suspend
- Resume handle passed as first parameter

**Output target:** Reconstruct `co_await` / `co_yield` skeleton (even if types are wrong).

**Risk:** Compiler-specific (MSVC vs Clang) frame layouts; high false-positive rate without ABI docs.

**Not shipped.**

---

## 3. Cross-language FFI reconstruction

Modern binaries mix languages via stable ABIs:

| Boundary | Detection | Emission |
|----------|-----------|----------|
| Rust → C | `#[no_mangle]` symbols, panic hooks | Dual-file output: `main.rs` + `extern "C"` block |
| JNI | `JNI_OnLoad`, `JNIEnv*` first arg | Java stub + native `JNIEXPORT` |
| P/Invoke | `DllImport` metadata in CIL sidecar | C# + C split |
| Python C API | `PyObject*`, `PyArg_ParseTuple` | `.py` + `_native.c` |

**Requires:** Per-ABI marshalling tables and format-specific loaders (partially exists for managed paths). Dual-language output is **not** a v2.0.22 product feature. Native output stays **C**.

---

## 4. Learned decompilation

Train models on **(source, compiled, decompiled)** triples at multiple `-O` levels:

1. **Naming model** — function/variable names from IR + context.
2. **Structure model** — predict `if`/`while`/`for` from CFG + memory accesses (seq2seq on graph).
3. **Diff model** — patch-aware naming (“version bump changed bounds check here”).

**Shipped, separate, opt-in:** llama.cpp refine (`RETDEC_ENABLE_LLAMACPP`,
`RETDEC_NEURAL_REFINE`). Complements item 1; it is not this research programme.
There is no in-tree `src/qwen3` (`C-QWEN3-GPU` withdrawn). The neural
**differential** gate is not implemented.

**Data:** LLVM `-g` corpora, Compiler Explorer snapshots, self-hosted RetDec round-trips.

**Ethics:** Do not train on malware-only sets without balanced OSS baselines.

---

## 5. Binary similarity at scale

**Use cases:** “Find functions like this one” across terabytes; cluster malware families.

| Layer | Technique |
|-------|-----------|
| Fast filter | TLSH / ssdeep on function bytes |
| Structural | CFG edit distance, call-graph neighborhood hash |
| Semantic | Embedding of lifted IR snippets (Graph Neural Net) |
| GPU batch | Research extension of `retdec-gpu-scanner` (`src/utils/gpu_scanner.cu`) to batched MinHash |

`retdec-gpu-scanner` is **built and unit-tested only**. Nothing in `src/retdec`
calls it. `RETDEC_ENABLE_CUDA` defaults OFF. Do not advertise GPU scanning as
shipped. See [CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md).

**Integration (research):** Index sidecar next to `.config.json`; GUI “Find similar” on selected function.

---

## 6. Self-hosting dogfood

Decompile **RetDec binaries with RetDec**, track:

- Functions lost / mis-structured
- STL recovery on our own C++ (`std::vector`, `std::map` in `llvmir2hll`)
- Performance regressions on real 10 MB+ `retdec-decompiler` PE

Publish a living **dogfood scorecard** in CI (informational, non-gating).
**Not shipped** as a CI job at v2.0.22.

---

## 7. Sandboxed untrusted analysis

Malware RE should not run `fileinfo` / unpackers on the host without isolation.

**Options:**
- Windows Sandbox / Firejail wrapper scripts
- WASM sandbox for parsers only (not full decompile — too heavy)
- Remote worker queue (commercial tier)

**Not shipped.**

---

## 8. Hardware trace integration

Intel PT / ARM CoreSight **executed-edge** annotations overlaid on CFG:

1. Import trace file (Intel PT decoder output)
2. Mark CFG edges as taken/not-taken/unknown
3. Gray out dead code in Assembly + CFG views

**Value:** Analysts see what actually ran, not all static paths.

**Not shipped.**

---

## 9. Formal verification bridge

Export lifted logic to **Why3**, **Frama-C**, or **Boogie** for niche certification (avionics, medical firmware).

**Scope:** Single-function, bounded loops, no full heap reasoning.

**Long shot:** If STL recovery emits `std::vector<int>`, map to verified spec library.

**Not shipped.** Native output remains buildable C; that is not a proof.

---

## 10. Async / event-loop patterns

Beyond coroutines: Node-style callbacks, `.NET async/await` state machines, Qt signal/slot meta-object calls.

**Detection:** Repeated indirect calls through vtables + timer APIs + queue push/pop pairs.

**Not shipped.**

---

## Priority for research investment

| Rank | Topic | Why |
|------|-------|-----|
| 1 | Learned naming (4) | Complements opt-in llama.cpp; high analyst satisfaction |
| 2 | Binary similarity (5) | Malware + patch diff workflows |
| 3 | FFI reconstruction (3) | Growing mixed-language firmware |
| 4 | Concurrency model (1) | Hard but differentiating; primitives already ship |
| 5 | Coroutines (2) | Niche until C++20 corpus grows |
| 6 | Formal bridge (9) | Commercial certification door |
| 7 | HW trace (8) | Needs hardware lab |
| 8 | Self-hosting (6) | Continuous quality signal |
| 9 | Sandbox (7) | Ops/security, not algorithm |
| 10 | Async patterns (10) | Research-grade open problem |

---

## GPU acceleration (parked)

`src/cuda_accel/` and `src/opencl/` are **parked research**.
They are not on the product sprint. Default CMake is OFF; `src/retdec`
does not link them. Public docs: [CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md).

Phase 1 `DEAD-03` still wants the trees moved off `main` onto a research
branch. Until that deletion, treat the directories as archival source, not
as a capability. v2.0.22 installers (Linux, macOS, Windows) do **not**
require NVIDIA or OpenCL.

---

## References

- [future_directions.md](future_directions.md) — STL recovery, targets, performance
- [pipeline_stage_map.md](pipeline_stage_map.md) — stage inventory
- [ENGINEERING_ROADMAP.md](internal/ENGINEERING_ROADMAP.md) — shippable Tiers 1–5
- [CLAIMS.md](CLAIMS.md) — asserted / withdrawn / demonstrated claims
