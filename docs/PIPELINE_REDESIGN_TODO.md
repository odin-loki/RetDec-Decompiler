# Pipeline Redesign — Gap Checklist

Reconciled checklist for the 29-stage pipeline described in [pipeline_stage_map.md](pipeline_stage_map.md). The stage map marks many entries **Implemented** at the “RetDec has a hook / partial pass” level; this file tracks what still falls short of the **full** algorithms in the original design write-up.

**Related (not yet in-tree):** [DECOMPILATION_IMPROVEMENT_FRAMEWORK.md](DECOMPILATION_IMPROVEMENT_FRAMEWORK.md) — referenced by the stage map for file paths, tests, and `RETDEC_HEURISTIC_DIAG` / jump-table notes. Create that document when consolidating per-stage test fixtures and diagnostic hooks.

**Status legend**

| Status | Meaning |
|--------|---------|
| **hook** | Entry point or flag exists; algorithm is stubbed or delegates to legacy heuristics |
| **partial** | Substantial implementation; known gaps vs design doc |
| **full** | Matches design intent for typical inputs; only edge cases remain |

**Priority legend**

| Priority | Meaning |
|----------|---------|
| **P0** | Blocks correctness or trust on common binaries |
| **P1** | High leverage for output quality or analyst workflows |
| **P2** | Incremental improvement; safe to defer one release |
| **P3** | Research / polish; schedule when upstream deps land |

---

## Stage checklist

| Stage | Name | Status | Gap (vs full design) | Priority | Owner |
|-------|------|--------|------------------------|----------|-------|
| 1 | Signature-Lattice File Format Parsing | partial | Polyglot scoring, fault-tolerant field recovery not unified | P2 | format |
| 2 | Structural Entropy Packer Detection | hook | Byte-signature YARA still primary; entropy lattice not wired | P1 | cpdetect |
| 3 | Emulation-Bounded Unpacking | partial | `--try-emulation` logs/continues; full IR→emulate→dump path incomplete | P1 | unpackertool |
| 4 | Codegen Compiler Fingerprinting | partial | Prologue/epilogue + tail-call ratio scoring incomplete | P2 | cpdetect |
| 5 | Format-Specific Binding Resolution | partial | TLS/PLT/GOT coverage varies by format | P2 | bin2llvmir |
| 6 | Multi-Evidence Code vs Data Classification | partial | Exec flag + section name only; xref/vtable evidence missing | P1 | decoder |
| 7 | Semantics-First Instruction Decoding | full | `validateTranslationSemantics` on all archs | P3 | capstone2llvmir |
| 8 | Convergent Function Boundary Detection | partial | DEBUG > SYMBOL > CONFIG; no convergence voting | P2 | decoder |
| 9 | Eager Constraint CFG Construction | partial | Strict range check; lazy edges on some archs | P2 | decoder |
| 10 | DWARF/PDB Ground Truth Extraction | partial | DW_OP_reg; limited PDB type seeding | P1 | debugformat |
| 11 | Semantic Idiom Reconstruction | partial | abs/rotate/bswap/popcount; broader idiom catalog open | P2 | idioms |
| 12 | Reference-Anchored String Detection | partial | `getStringAtAddress`; weak xref clustering | P2 | fileformat |
| 13 | ABI-Specific RTTI/Vtable Reconstruction | partial | ABI validation; incomplete Itanium/MSVC edge cases | P2 | rtti-finder |
| 14 | Type-Seeding Symbol Demangling | partial | `getFunctionTypeFromDemangledName`; templates weak | P2 | demangler |
| 15 | Exception Handling Reconstruction | partial | PE .pdata + ELF .eh_frame; compact/unwind info gaps | P2 | decoder |
| 16 | Liveness-Pruned SSA Construction | full | LLVM mem2reg in default pipeline | P3 | llvm |
| 17 | ABI-Constrained Calling Convention Detection | partial | x86/x64 providers; arch coverage uneven | P2 | abi |
| 18 | ABI-Aware Variable Recovery (DVSA) | partial | Stack + ABI + debug; inter-procedural slots open | P1 | stack |
| 19 | Stratified Alias Analysis | partial | simple/basic/ext tiers; no whole-program Steensgaard in HLL path | P2 | llvmir2hll |
| 20 | Width-Seeded Type Inference | partial | Load/store width seeds; struct recovery incomplete | P1 | ir_modifier |
| 21 | Bounds-Check Guided VSA (Jump Tables) | partial | `tableSize` from icmp; symbolic bounds incomplete | P1 | decoder |
| 22 | Compiler-Aware SESE Structuring | hook | `getDetectedCompilerOrPacker` plumbed; SESE not compiler-tuned | P1 | llvmir2hll |
| 23 | E-Graph Expression Simplification | hook | Sub-optimizer rewrites; no e-graph saturation | P2 | optimizer |
| 24 | WHT MBA Deobfuscation | partial | Fixed rewrite set; WHT transform not implemented | P2 | mba |
| 25 | ABI-Filtered Dead Code Elimination | partial | `retdec-value-protect` before dse/bdce/adce; callee-saved gaps | P2 | bin2llvmir |
| 26 | Summary-Based Inter-Procedural Analysis | partial | FuncInfo/CallInfo + SCC; context sensitivity limited | P2 | llvmir2hll |
| 27 | Readability-Optimised C Code Generation | partial | Optim order + copy-prop; no readability cost model | P2 | llvmir2hll |
| 28 | Output Validation | full | Return/BreakOutsideLoop/NoGlobalVarDef validators | P3 | validator |
| 29 | Diagnostic Aggregation | partial | Meta-info + Log phases; unified diagnostic schema open | P1 | hll_writer |

---

## Design invariants (P6 — do not regress)

1. **Compiler output is not arbitrary** — flag deviations; do not silently approximate.
2. **Ground truth overrides inference** — DWARF/PDB/symbols/EH before heuristics.
3. **Approximations are explicit** — every approximation emits a diagnostic.
4. **Readability is first-class** — codegen optimises for human readability under correctness.

---

## Suggested work order

1. **P0/P1 analysis core:** Stages 6, 10, 18, 20, 21, 22, 29 — ground truth + structuring + diagnostics.
2. **P1 unpack/obfusc:** Stages 2, 3, 24 — entropy unpack + MBA.
3. **P2 extensibility:** Pass profiles ([profiles/README.md](../src/retdec-decompiler/profiles/README.md)), [pipeline_builder_schema.json](pipeline_builder_schema.json), plugin sample ([examples/decompiler_plugin](../examples/decompiler_plugin/)).
4. **P3 polish:** Stages 1, 4, 8, 11 — format/compilers/fingerprint refinements.

---

## Cross-references

- [pipeline_stage_map.md](pipeline_stage_map.md) — stage → source mapping
- [pipeline_builder_schema.json](pipeline_builder_schema.json) — custom pass-list JSON schema
- [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) — shippable Tiers 1–5
- [DECOMPILATION_IMPROVEMENT_FRAMEWORK.md](DECOMPILATION_IMPROVEMENT_FRAMEWORK.md) — *planned* per-stage tests and env diagnostics
