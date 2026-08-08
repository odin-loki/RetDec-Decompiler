# RetDec Algorithm Reference

Algorithms documented in source file headers. Fields marked `-` are not stated in the header.

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
