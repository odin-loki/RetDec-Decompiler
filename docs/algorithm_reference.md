# RetDec Algorithm Reference

Imortek **2.0.22**. Algorithms documented in **source file headers**. Fields
marked `-` are not stated in the header. Complexity numbers are copied from
those headers, not independently measured.

**Wiring:** files under `src/algo_recover`, `src/ipa`, `src/type_inference`,
`src/call_conv`, and `src/ssa` (rebuild) participate in **post-pipeline**
analysis in `src/retdec/retdec.cpp` (type inference only if
`RETDEC_TYPE_INFERENCE=1`). Files under `src/cfg_structure`, `src/var_recovery`,
`src/dce`, `src/alias_analysis`, and `src/codegen` belong to the parallel
SSA/codegen stack used by tests / `cxx_backend`, **not** the llvmir2hll C
writer. See [architecture.md](architecture.md) and
[pipeline_stage_map.md](pipeline_stage_map.md).

Do not treat detector rows as production-quality recovery. Name-blind
algorithm-recovery F1 on the 216-binary corpus is **0.056**
([BENCHMARKS.md](BENCHMARKS.md)).

| Algorithm | Citation | Source file | Complexity |
|-----------|----------|-------------|------------|
| FlagBundle analysis + SSAPass + SSAVerifier + SSAFunction impl | - | `src/ssa/flag_bundle.cpp` | - |
| SSA renaming pass (Cytron et al. algorithm with FlagBundle and MemRef) | Cytron et al. §4 | `src/ssa/ssa_rename.cpp` | - |
| Backward dataflow liveness analysis | Aho, Lam, Sethi, Ullman §9.2 | `src/ssa/liveness.cpp` | O(n × k × d) where n = number of blocks, k = number of variables, d = iterations until convergence |
| Liveness-pruned phi function placement | Cytron et al. | `src/ssa/phi_placement.cpp` | - |
| Lengauer-Tarjan dominator tree and dominance frontier computation | Lengauer & Tarjan, "A fast algorithm for finding dominators in a flowgraph" (TOPLAS 1979); Cooper, Harvey, Kennedy 2001 | `src/ssa/domtree.cpp` | O(n α(n)); dominance frontiers: O(n²) |
| Steensgaard (1996) unification-based alias analysis | Steensgaard, "Points-to Analysis in Almost Linear Time" (POPL 1996) | `src/alias_analysis/steensgaard.cpp` | O(n α(n)) |
| Exact stack frame alias analysis | - | `src/alias_analysis/stack_alias.cpp` | - |
| Pointer escape analysis for SSA functions | - | `src/alias_analysis/escape_analysis.cpp` | - |
| ABI parameter/return-type seeding, struct recovery, and TypeInferencePass | - | `src/type_inference/abi_seeder.cpp` | - |
| Phase 1: instruction-width extraction for SSA values | - | `src/type_inference/width_seeder.cpp` | - |
| Phase 2: union-find type propagation | - | `src/type_inference/type_propagation.cpp` | - |
| SESE region decomposition using DFS timestamps and post-dominator tree | - | `src/cfg_structure/sese_decomp.cpp` | O(1) containment checks |
| Recursive SESE-based CFG structurer and CfgStructurePass orchestrator | - | `src/cfg_structure/compiler_structurer.cpp` | - |
| Natural loop classification (while / for / do-while / infinite) | - | `src/cfg_structure/loop_recovery.cpp` | - |
| CFG reducibility check via DFS edge classification | - | `src/cfg_structure/irreducibility.cpp` | - |
| Post-dominator tree via Lengauer-Tarjan on the reversed CFG | - | `src/cfg_structure/post_domtree.cpp` | - |
| std::for_each detector — range loop with single call per element | - | `src/algo_recover/foreach_detect.cpp` | - |
| std::transform detector — source→destination one-to-one loop | - | `src/algo_recover/transform_detect.cpp` | - |
| AlgorithmDetector orchestrator + AlgorithmResult utilities | - | `src/algo_recover/algo_detector.cpp` | - |
| std::partition detector — converging index, standalone (not in sort) | - | `src/algo_recover/partition_detect.cpp` | - |
| Iterator pattern recovery — begin/end → range-based for | - | `src/algo_recover/iterator_recover.cpp` | - |
| std::find / std::find_if detector — equality compare + early exit | - | `src/algo_recover/find_detect.cpp` | - |
| std::accumulate / max_element / min_element detector | - | `src/algo_recover/accumulate_detect.cpp` | - |
| Binary search detector — midpoint + load + compare + bound update (no name hints) | - | `src/algo_recover/binary_search_detect.cpp` | - |
| Classic C idiom detectors (atoi, BFS, varint, …) | - | `src/algo_recover/idiom_detect.cpp` | - |
| ABI artifact detection: stack alignment, prologue/epilogue, shadow space, callee-save pairs, red zone | - | `src/dce/abi_artifact_marker.cpp` | - |
| C-semantic live root collection | - | `src/dce/live_root_collector.cpp` | - |
| Backward SSA def-use liveness propagation from C-semantic live roots | - | `src/dce/dead_propagation.cpp` | - |
| Forward reachability analysis to find unreachable basic blocks | - | `src/dce/unreachable_elim.cpp` | - |
| DcePass orchestrator + DeadCodeResult summary | - | `src/dce/dce_pass.cpp` | - |
| SCC-stratified inter-procedural type propagation | - | `src/ipa/ipa_propagation.cpp` | - |
| Per-function summary computation from intra-procedural analysis results | - | `src/ipa/function_summary.cpp` | - |
| IpaPass orchestrator + IpaResult summary | - | `src/ipa/ipa_pass.cpp` | - |
| Inline candidate identification | - | `src/ipa/inline_candidate.cpp` | - |
| Global variable type unification across all functions | - | `src/ipa/global_typing.cpp` | - |
| Call graph construction and Tarjan SCC decomposition | - | `src/ipa/call_graph.cpp` | - |
| Prologue pattern recognition for x86-64 SysV, x86-64 Win64, x86-32, AArch64, and ARM32 | - | `src/var_recovery/prologue_parser.cpp` | - |
| DVSA: Data-flow-driven Variable and Stack-slot Analysis | - | `src/var_recovery/dvsa.cpp` | - |
| Variable naming + VarRecoveryPass orchestration | - | `src/var_recovery/var_namer.cpp` | - |
| ABI-mandated frame region carving | - | `src/var_recovery/abi_regions.cpp` | - |

### Other detector libraries (headers, not this table’s original set)

These also run post-pipeline unless noted. They emit `semanticDetections`
hints; they do not rewrite native output into STL/C++.

| Area | Directory | What exists |
|------|-----------|-------------|
| Containers | `src/container_detect/` | vector, list, map, unordered_map, string, ring buffer, open-addressing |
| Sorts | `src/sort_detect/` | introsort, mergesort, heapsort, quicksort, bubblesort, radix, partition |
| Crypto | `src/crypto_detect/` | AES, SHA-1/256, ChaCha20, Salsa20, RSA/DH, RC4, MD5, CRC, HMAC, Poly1305, Blowfish, DES (and related enums) |
| Serialisation | `src/serial_detect/serial_detect.cpp` | Protobuf, FlatBuffers, JSON, XML detectors |
| Concurrency | `src/concurrency_detect/` | std::thread, pthread, Win32, atomics, spinlock, OpenMP, TBB |
| Design patterns | `src/pattern_detect/` | Singleton, Factory, and related heuristics |
| Compiler idioms (unwired) | `src/idiom_reconstruct/` | magic-number div/mod, abs, bit ops, SIMD memcpy — **not** linked from `decompile()`; bin2llvmir `retdec-idioms` is what runs |
