# RetDec — Future Directions

This document consolidates the long-term research agenda and the engineering
roadmap for RetDec.  It is divided into three parts:

1. **Semantic Library Recovery** — recovering STL containers and algorithms
   from compiled binaries without source code.
2. **New Decompilation Targets** — additional architectures, binary formats,
   and output languages on the roadmap.
3. **Research Frontiers** — open problems, speculative ideas, and integration
   opportunities.

---

## Part 1 — Semantic Library Recovery

> *Recovering standard-library usage is the highest-leverage improvement
> available: STL code appears in virtually every non-trivial C++ binary, and
> recognising it collapses hundreds of lines of decompiled noise into a single
> semantically clear call.*

### The Core Problem

Signature-based matching (FLIRT-style) works for non-templated library
functions with stable binary representations.  It **fails** for STL because
templates are instantiated per type — `std::sort<int*>` and
`std::sort<std::string*>` compile to completely different machine-code
sequences with no shared binary signature.

The solution is **semantic matching**: abstract characterisations of *what*
an algorithm does rather than what its bytes look like.

---

### Algorithm Recovery

#### Sorting

| Algorithm | Structural Invariants |
|-----------|----------------------|
| Quicksort / Introsort | Two-index partition loop; two recursive calls on sub-ranges; heapsort fallback triggered by recursion depth counter; insertion sort switch-over below N≈16 |
| Merge sort | Two recursive half-calls; merge loop with three-way branch (left exhausted / right exhausted / compare); auxiliary buffer allocation |
| Heapsort | Heap-build phase (i = n/2 → 0, calling sift-down); sort phase (swap root with last, sift-down); child index arithmetic 2i+1, 2i+2 |
| Radix sort | No comparisons; byte/nibble masking and shifting; histogram accumulation; prefix-sum pass; redistribution scatter |

**Detection strategy**: characterise functions by loop nesting depth,
recursive call count, comparison-swap pairing, auxiliary memory usage, and
index arithmetic relationships.  Match against algorithm descriptors with
confidence thresholds to suppress false positives.

#### Searching

| Algorithm | Key Signal |
|-----------|-----------|
| Binary search | `(low + high) >> 1` midpoint; three-way branch: return / narrow left / narrow right; loop terminates when low ≥ high |
| Linear search (`std::find`) | Simple range loop with equality comparison and early exit |
| Interpolation search | Non-uniform midpoint arithmetic proportional to key distribution |

#### Graph Algorithms

| Algorithm | Key Signal |
|-----------|-----------|
| BFS | Queue (FIFO) frontier; visited marker array; push to back / pop from front |
| DFS | Recursive structure (or explicit stack); visited marking; can be further classified as reachability / topological / SCC (Tarjan's low-link array) |
| Dijkstra's | Min-heap priority queue with decrease-key; relaxation step `dist[u] + w(u,v) < dist[v]`; settled-node set |

#### Numeric and Mathematical

| Algorithm | Key Signal |
|-----------|-----------|
| Fast exponentiation | Loop: right-shift exponent, test low bit, square base, conditionally multiply result |
| GCD (Euclidean) | Repeated modulo until one operand is zero |
| GCD (binary) | Factor-of-2 extraction; subtract smaller from larger |
| FFT (Cooley-Tukey) | Bit-reversal permutation; nested stage/butterfly loops; twiddle factors via trig or precomputed table; power-of-two size |

---

### Container Recovery

#### std::vector

**Internal layout**: `{begin*, end*, capacity_end*}`

- `push_back`: check `end < cap`, write to `*end`, increment `end`; on overflow, reallocate at 1.5× or 2×, move elements, update all three pointers.
- The growth-and-move pattern is the strongest identification signal.
- Element stride reveals the template type parameter `T`.

#### std::deque

**Internal layout**: block map (array of chunk pointers) + two-level index arithmetic.

- Detect via: `mapIndex = logicalIndex / blockSize`, `offset = logicalIndex % blockSize`, two levels of pointer dereference per access.
- Map reallocation when blocks overflow either end.

#### std::list

**Internal layout**: doubly-linked list with circular sentinel node.

- `sentinel->next` = first element; `sentinel->prev` = last element.
- Detect via: heap-allocated nodes with two pointer fields (prev/next); threading operations; sentinel detection.

#### std::map / std::set (Red-Black Tree)

**Node structure**: `{left*, right*, parent*, color, key[, value]}`

- Left/right rotations are unmistakable: a node moves to a child position, its child is pulled up, grandparent and sibling pointers are adjusted.
- Rebalancing follows the classic Cormen-style case analysis (colour checks + rotation calls).

#### std::unordered_map / std::unordered_set

**libstdc++ layout**: bucket array (pointers to singly-linked chains) + flat linked list of all nodes.

- Detect via: hash function call on key; modulo bucket count; chain traversal.
- Hash function identifies key type: identity for integers; length+data loop for strings.

#### std::priority_queue

Built on `push_heap` / `pop_heap`.

- `push_heap`: sift-up from last position using `(i−1)/2` parent arithmetic.
- `pop_heap`: swap root with last, sift-down using `2i+1`, `2i+2` children.
- Same arithmetic as heapsort but calling context (vector + heapify) distinguishes it.

#### std::string (SSO)

- Small String Optimisation branch: compare length against threshold (15 for libstdc++, 22 for libc++), branch between inline buffer and heap pointer.
- Detect via: three-field struct; SSO threshold check; `operator[]` accessing either inline or heap data.

#### std::shared_ptr / std::weak_ptr

- Two-pointer struct: object pointer + control block pointer.
- Control block: atomic strong count + weak count + deleter.
- Atomic increment on copy; atomic decrement with zero-check triggering deleter / freeing control block.

---

### Recovery Pipeline Architecture

```
Layer 1 — Structure Detection (bottom-up, per-function)
    ↓  candidate matches + confidence scores + inferred parameters
Layer 2 — Context Validation (whole-program cross-check)
    ↓  validated, high-confidence identifications
Layer 3 — Object Reconstruction (idiomatic STL emission)
    ↓  v.push_back(), v.size(), map.find(), etc.
```

**Template type recovery**: the comparator or hash function used in an
algorithm identifies the element type.  This flows back into container
declarations and call-site reconstruction.

---

## Part 2 — New Decompilation Targets

### Architecture Targets

| Architecture | Status | Notes |
|-------------|--------|-------|
| x86-64 | Implemented | Primary target |
| x86-32 | Implemented | — |
| ARM32 / Thumb | Implemented | — |
| AArch64 | Implemented | — |
| MIPS32/64 | Implemented | — |
| PowerPC 32/64 | Implemented | — |
| RISC-V (RV32I, RV64I) | Future | LLVM backend exists; new lifter front-end needed |
| LoongArch | Research | Chinese architecture gaining traction |
| WASM (binary) | Implemented | WAT emitter complete |
| PTX (NVIDIA virtual ISA) | Implemented | CUDA C lifter complete |
| SASS (NVIDIA machine code) | Research | Architecture-specific, generation-variant; `nvdisasm` as pre-processor |
| DXBC / DXIL (HLSL bytecode) | Future | DirectX shaders; useful for game RE |
| SPIR-V | Future | Vulkan/OpenCL shaders |

### Binary Format Targets

| Format | Status | Notes |
|--------|--------|-------|
| ELF (Linux) | Implemented | — |
| PE/PE+ (Windows) | Implemented | — |
| Mach-O (macOS/iOS) | Implemented | — |
| COFF / OMF | Implemented | — |
| .NET CIL / MSIL | Implemented | C# emitter complete |
| JVM bytecode / DEX | Implemented | Java + Kotlin emitters complete |
| Python `.pyc` | Implemented | Python emitter complete |
| Lua `.luac` | Implemented | Lua emitter complete |
| WebAssembly `.wasm` | Implemented | WAT emitter complete |
| Fat binary / Universal | Future | macOS multi-arch containers |
| APK (Android) | Partially | DEX parser exists; resource recovery future |
| IPA (iOS) | Research | Mach-O + embedded frameworks |
| Swift ABI | Future | Swift name mangling + metadata parsing |
| Rust ABI | Research | Rust panic/unwrap patterns; trait vtables |

### Output Language Targets

| Language | Status | Notes |
|----------|--------|-------|
| C | Implemented | Default output |
| C++ | Implemented | Class hierarchy, vtables, RTTI |
| F# | Implemented | Functional approximation |
| Visual Basic .NET | Implemented | — |
| Python | Implemented | From `.pyc` input |
| Lua | Implemented | From `.luac` input |
| WASM/WAT | Implemented | — |
| CUDA C | Implemented | From PTX input |
| Java | Implemented | From JVM bytecode |
| Kotlin | Implemented | From DEX/JVM, Kotlin metadata |
| C# | Implemented | From CIL |
| Rust | Future | Highly desirable; requires Rust-specific idiom recovery |
| Go | Research | Go ABI differs significantly; goroutine patterns |
| Swift | Research | ARC patterns, Swift runtime calls |
| JavaScript / TypeScript | Future | From WASM; useful for web targets |

---

## Part 3 — Research Frontiers

### CUDA and GPU Decompilation

**PTX** (Parallel Thread Execution) is NVIDIA's virtual ISA and is feasible
today.  PTX preserves enough structure — thread indexing via `%tid.x`/`%ctaid.x`,
memory space qualifiers (`.shared`, `.global`, `.local`), synchronisation
barriers (`bar.sync`) — to reconstruct `__global__`/`__device__` kernels,
`threadIdx`, `blockIdx`, `__shared__` declarations, and `__syncthreads()`.

**SASS** (Streaming Assembler, actual GPU machine code) is harder.
It is architecture-specific, generation-variant (Ampere differs from Hopper),
and incompletely documented.  `nvdisasm` can be used as a pre-processor to
convert SASS → PTX-like text.  Recommend treating SASS as a stretch goal
using `nvdisasm` as the first stage.

**Fat binaries / cubin**: PTX sections can be extracted directly from `.cubin`
and `.fatbin` files embedded in executables, bypassing SASS entirely.

### Machine Learning Integration

**AI-assisted naming**: use the Qwen3 model already integrated into the GUI to
suggest meaningful variable and function names based on decompiled context.
This requires fine-tuning or prompting with decompiler-specific examples.

**Semantic validation**: use the AI assistant to verify that reconstructed
code is semantically equivalent to the original.  Input: original binary
execution trace vs reconstructed binary execution trace; output: equivalence
judgement.

**Algorithm classification**: augment the structural detector (Part 1) with
a neural classifier trained on IR-level features.  Provide training data from
compiled open-source projects where ground truth is known.

**Confidence calibration**: train a regression model on pairs (structural
confidence score, ground truth label) to better calibrate detection thresholds.

### Structural Reconstruction

**Full project reconstruction** from a stripped binary is the most ambitious
target.  For well-structured binaries (PDB present, or compiled with `/Zi`),
accuracy is high.  For stripped, LTO'd binaries the hard problems are:

- Recovering which `.cpp` a function belonged to (almost impossible without
  debug info; approximate using call-graph clustering + string locality).
- Distinguishing inlined code from out-of-line calls (LTO aggressively inlines
  across translation units).
- Reconstructing public API boundaries between modules.

**Achievable target**: a CMake project where each module is a best-effort
grouping by call-graph community detection, headers are generated from
recovered type information, and the build system links everything correctly.
It will not replicate the original source tree but it will compile and produce
equivalent behaviour.

### Formal Verification Integration

Connect RetDec's output to a proof assistant (Coq, Lean4, or Isabelle) for
formal verification that the decompiled source is semantically equivalent to
the binary.  This is a long-term research direction — likely a decade of work
to achieve practically useful coverage — but has applications in:

- Safety-critical systems verification
- Certification of embedded firmware
- Legal/compliance analysis of closed-source software

**Starting point**: semantics of a subset of x86-64 + LLVM IR → Coq/Lean
translation, using existing work (CompCert, Vellvm).

### Exception Handling and Concurrency

**C++ exceptions across modules**: current EH reconstruction handles local
try/catch but cross-module exception propagation (through shared libraries with
different compilers' EH ABIs) is an open problem.

**Concurrency model reconstruction**: the current concurrency detector identifies
individual primitives (mutex, thread, atomic).  A higher-level goal is
reconstructing the *concurrency model*: which threads own which locks, which
shared state is protected by which mutex, and whether the program is
data-race-free.  This is equivalent to automatic lock/unlock inference, which
has active academic research.

**Async patterns**: modern code increasingly uses `co_await`/`co_yield` (C++20
coroutines), `async`/`await` (many languages), and continuation-passing.
Reconstructing these from compiled state machines is an open research problem.

### Cross-Language Decompilation

Modern software often mixes languages via FFI:
- Rust calling C, or C calling Rust
- Python C extensions
- JNI (Java calling C/C++)
- P/Invoke (C# calling native)

Detecting and correctly annotating these boundaries — and reconstructing the
correct dual-language output — requires understanding the calling conventions
and type marshalling of each interface.

### Binary Diffing and Vulnerability Research

Extend the DiffPanel to support:
- **Cross-binary diff**: compare two different binaries function-by-function
  to identify patches, version changes, or variants.
- **Vulnerability pattern matching**: use the AI assistant to identify
  historically known vulnerability patterns (buffer overflows, use-after-free,
  format string bugs) in decompiled output.
- **Symbolic execution integration**: connect RetDec's output to a symbolic
  executor (KLEE, angr) for automatic test case generation and vulnerability
  finding.

### Performance Scalability

Current bottleneck hypothesis: type inference + structuring ≈ 60% of total
analysis time.  The profiling harness (Task 58) provides infrastructure to
measure this precisely.

**Planned optimisations**:
1. Profile-guided optimisation (PGO): instrument → profile on a 50-binary
   corpus → optimise.
2. LTO across core + OpenCL boundary.
3. Hot-path function inlining hints for the inner SSA traversal loops.
4. Parallel structuring: functions are independent; run on all hardware threads.
5. Incremental analysis: cache per-function results; re-analyse only changed
   functions when the binary is updated.

**Scale target**: 10 MB binary in under 60 seconds on a 16-core workstation.
Current baseline (estimated): ~5 minutes for a 2 MB binary.

---

## Implementation Priority

Ordered by impact-to-effort ratio:

| Priority | Feature | Rationale |
|----------|---------|-----------|
| 1 | STL container recovery (vector, map, unordered_map) | Highest coverage; appears in nearly all C++ binaries |
| 2 | Sorting algorithm recovery (introsort, merge sort) | High frequency; dramatic readability improvement |
| 3 | RISC-V support | Growing embedded and server architecture |
| 4 | Rust output language | Increasingly requested; clean memory-safe target |
| 5 | AI-assisted variable naming | High usability impact; low engineering risk |
| 6 | SASS decompilation (via nvdisasm) | Extends CUDA support to compiled kernels |
| 7 | Cross-binary diff | Security research application |
| 8 | Formal verification bridge | Long-term; high value for certification use cases |
