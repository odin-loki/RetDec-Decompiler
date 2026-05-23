# RetDec — Research Frontiers (Tier 7)

Long-horizon research and speculative engineering. **Not scheduled for product sprints.**
For shippable work see [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) (Tiers 1–5).

---

## 1. Whole-program concurrency model

**Goal:** Infer lock ownership, shared-state protection, and data-race hints — not just individual `pthread_mutex_lock` calls.

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

---

## 3. Cross-language FFI reconstruction

Modern binaries mix languages via stable ABIs:

| Boundary | Detection | Emission |
|----------|-----------|----------|
| Rust → C | `#[no_mangle]` symbols, panic hooks | Dual-file output: `main.rs` + `extern "C"` block |
| JNI | `JNI_OnLoad`, `JNIEnv*` first arg | Java stub + native `JNIEXPORT` |
| P/Invoke | `DllImport` metadata in CIL sidecar | C# + C split |
| Python C API | `PyObject*`, `PyArg_ParseTuple` | `.py` + `_native.c` |

**Requires:** Per-ABI marshalling tables and format-specific loaders (partially exists for managed paths).

---

## 4. Learned decompilation

Train models on **(source, compiled, decompiled)** triples at multiple `-O` levels:

1. **Naming model** — function/variable names from IR + context (complements Qwen3 post-pass).
2. **Structure model** — predict `if`/`while`/`for` from CFG + memory accesses (seq2seq on graph).
3. **Diff model** — patch-aware naming (“version bump changed bounds check here”).

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
| GPU batch | Extend `retdec-gpu-scanner` to batched MinHash |

**Integration:** Index sidecar next to `.config.json`; GUI “Find similar” on selected function.

---

## 6. Self-hosting dogfood

Decompile **RetDec binaries with RetDec**, track:

- Functions lost / mis-structured
- STL recovery on our own C++ (`std::vector`, `std::map` in `llvmir2hll`)
- Performance regressions on real 10 MB+ `retdec-decompiler` PE

Publish a living **dogfood scorecard** in CI (informational, non-gating).

---

## 7. Sandboxed untrusted analysis

Malware RE should not run `fileinfo` / unpackers on the host without isolation.

**Options:**
- Windows Sandbox / Firejail wrapper scripts
- WASM sandbox for parsers only (not full decompile — too heavy)
- Remote worker queue (commercial tier)

---

## 8. Hardware trace integration

Intel PT / ARM CoreSight **executed-edge** annotations overlaid on CFG:

1. Import trace file (Intel PT decoder output)
2. Mark CFG edges as taken/not-taken/unknown
3. Gray out dead code in Assembly + CFG views

**Value:** Analysts see what actually ran, not all static paths.

---

## 9. Formal verification bridge

Export lifted logic to **Why3**, **Frama-C**, or **Boogie** for niche certification (avionics, medical firmware).

**Scope:** Single-function, bounded loops, no full heap reasoning.

**Long shot:** If STL recovery emits `std::vector<int>`, map to verified spec library.

---

## 10. Async / event-loop patterns

Beyond coroutines: Node-style callbacks, `.NET async/await` state machines, Qt signal/slot meta-object calls.

**Detection:** Repeated indirect calls through vtables + timer APIs + queue push/pop pairs.

---

## Priority for research investment

| Rank | Topic | Why |
|------|-------|-----|
| 1 | Learned naming (4) | Low infra; high analyst satisfaction |
| 2 | Binary similarity (5) | Malware + patch diff workflows |
| 3 | FFI reconstruction (3) | Growing mixed-language firmware |
| 4 | Concurrency model (1) | Hard but differentiating |
| 5 | Coroutines (2) | Niche until C++20 corpus grows |
| 6 | Formal bridge (9) | Commercial certification door |
| 7 | HW trace (8) | Needs hardware lab |
| 8 | Self-hosting (6) | Continuous quality signal |
| 9 | Sandbox (7) | Ops/security, not algorithm |
| 10 | Async patterns (10) | Research-grade open problem |

---

## References

- [future_directions.md](future_directions.md) — STL recovery, targets, performance
- [pipeline_stage_map.md](pipeline_stage_map.md) — stage inventory
- [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) — shippable Tiers 1–5
