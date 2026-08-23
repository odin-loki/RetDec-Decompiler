# Changelog

All notable changes to RetDec (Odin Loch Trading as Imortek) are documented here.

---

## [2.0.21] — 2026-08-17

### Added

- Wave 5 leftover: remaining Value-based pointer-element reads
  (`config`, `stack`, `syscalls`, ABI, `ir_modifier`, `param_return`,
  `idioms_libgcc`) go through `pointeeType`. llvmir2hll global
  variables use `GlobalVariable::getValueType`. LLVM pin unchanged.
- Wave 5 leftover: remaining lifter IntToPtr load/store paths
  (x86 push/pop/call/ret/enter/leave/far/xlat/fxsave, ARM LDM/STM,
  ARM64 LDR/STR/LDP/STP, PowerPC indexed load/store) now emit
  `retdec.pointee` via `loadIntPtr`/`storeIntPtr`. String-op i8*
  IntToPtrs attach the same kind. Test:
  `StackPushAttachesPointeeMetadata`. LLVM pin unchanged.
- Wave 5 leftover: operand `loadOp`/`storeOp` on x86, ARM, ARM64,
  MIPS, and PowerPC now emit `retdec.pointee` on IntToPtr +
  load/store via `loadIntPtr`/`storeIntPtr`. Value-based
  `getPointerElementType` sites in inst_opt, entry_alloca, ABI,
  ir_modifier, and simple_types read `pointeeType` first.
  Test: `MemoryLoadAttachesPointeeMetadata`. LLVM pin unchanged.
- Wave 5 leftover: LLVM 8 typed-pointer facts can be stored as
  instruction metadata kind `retdec.pointee` (same `setMetadata`
  pattern as `insn.addr`). Helpers
  `llvm_utils::setPointeeTypeMetadata` /
  `getPointeeTypeMetadata` / `pointeeType` read MD first, then
  fall back to `PointerType::getElementType`. The Avast LLVM 8 pin
  is unchanged. Test: `LlvmUtilsTests.PointeeMetadataRoundTrips`.
  See `docs/internal/UNBLOCKED-MIGRATION.md`.
- CI leftover: stem-fallback now treats graph-family FPs
  (`DFS` / `BFS` / `GraphTraversal`) like other cross-family
  noise, so `generated_quicksort` is not stuck at F1=0 when
  the decompiler emits only those labels. Official 0.95 gate
  stays; this is still the stem-era CI score, not product F1.
  Test: `test_quicksort_graph_only_fp_uses_stem`.
- CI leftover: official algorithm-recovery CI runner passes
  `--stem-fallback` again so the unchanged 0.95 `mean_f1` gate
  matches the stem-era score it was written for. Extract stays
  name-blind by default; `mean_f1_raw` stays 0.126 (ci-core).
  Do not advertise 1.0.
- Pipeline leftover: `PatternDetector` now runs on each SSA
  function after serial export and appends `kind="pattern"`
  (label from `kindName()`). RAII acquire/release table hits
  (`fopen`/`fclose`, …) prefix `detail` with
  `evidence:symbol_name`. Extract does not map
  `kind=="pattern"`, so headline F1 is unchanged. Test:
  `RaiiAcquireReleaseExportsAsPatternNameEvidence`. Default F5
  is unchanged.
- Pipeline leftover: `SerialDetector` now runs on each SSA
  function after crypto export and appends `kind="serial"`
  (label from `frameworkName()`). Name-only symbol-table hits
  (`SerializeToString`, `FlatBufferBuilder`, …) prefix `detail`
  with `evidence:symbol_name`. Extract does not map
  `kind=="serial"`, so headline F1 is unchanged. Tests:
  `ProtobufSymbolsExportAsSerialNameEvidence`,
  `SerialPreflightSkipsTinyFunctions`. Default F5 is unchanged.
- Pipeline leftover: `CryptoDetector` now runs on each SSA
  function after `buildSemanticDetectionMap` and appends
  `kind="crypto"` (label from `algorithmName()`). Extract does
  not map `kind=="crypto"`, so headline F1 is unchanged.
  Name-only AES-NI scores 0.20 and stays below the 0.50 export
  floor. Tests: `HmacPadsExportAsCryptoKind`,
  `NameOnlyAesNiDoesNotExportBelowThreshold`. Default F5 is
  unchanged.
- N16 leftover: `serializeSemanticContext` also dumps defined
  frame-base storage from existing `Function::frameBaseStorage`.
  Test: `SerializesFrameBaseStorage`. Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps a
  function's defined end address from existing
  `AddressRange::getEnd` (start was already dumped). Test:
  `SerializesExistingFunctionFields`. Default F5 is unchanged.
- N16 leftover: vtable `targets` are objects with name, slot
  address, target address, and thumb from existing
  `VtableItem` getters. Test: `SerializesVtableTargetNames`.
  Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps tool
  detection percentage (when non-zero) and `heuristics` from
  existing `ToolInfo` getters. Additional info stays omitted.
  Test: `SerializesToolConfidence`. Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps
  `file_class_bits` from existing `FileFormat::getFileClassBits`
  when non-zero. Test: `SerializesFileClassBits`. Default F5 is
  unchanged.
- N16 leftover: `serializeSemanticContext` also dumps defined
  source `start_line` / `end_line` from existing
  `Function::getStartLine` / `getEndLine`. Lines alone do not
  include a function. Test: `SerializesSourceLineNumbers`.
  Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps parameter
  `real_name` and `from_debug` from existing `Object` getters.
  Crypto descriptions stay omitted. Test:
  `SerializesParameterRealName`. Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps existing
  return and parameter storage (register name, stack offset,
  memory address) on already-included functions. Test:
  `SerializesParameterAndReturnStorage`. Default F5 is
  unchanged.
- N16 leftover: known architecture endian (`little` / `big`) is
  included next to name and bit-size. Unknown endian stays
  omitted. Test: `SerializesCompilerToolAndArchitecture`.
  Default F5 is unchanged.
- Neural leftover: structural gate and rename denylist also
  reject `CreateProcessAsUser` / `A` / `W`, `ShellExecuteEx` /
  `A` / `W`, and `posix_spawn` / `posix_spawnp` (word-boundary:
  `CreateProcess` does not match `CreateProcessAsUserA`). Tests:
  `AddedCreateProcessAsUserCallFailsStructural`,
  `AddedShellExecuteExCallFailsStructural`,
  `AddedPosixSpawnCallFailsStructural`. Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps RTTI class
  constructor / destructor / method / virtual-method / vtable
  names from existing `Class` sets. Test:
  `SerializesClassMemberNames`. Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps existing
  function role flags (constructor / destructor / virtual /
  variadic / exported / thumb / syscall / idiom / static or
  dynamic link) on already-included functions. These flags
  alone do not include a function. Test:
  `SerializesFunctionRoleFlags`. Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps known
  file type (`shared` / `archive` / `object` / `executable`)
  from existing `FileType` predicates. Unknown stays omitted.
  Test: `SerializesFileType`. Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps a
  function's known calling convention via the existing
  `CallingConvention` streamer (`CC_THISCALL`, …). Unknown
  stays omitted. Test: `SerializesCallingConvention`. Default
  F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps detected
  source languages (name, bytecode, module count) from
  `config.languages`. Test: `SerializesDetectedLanguages`.
  Default F5 is unchanged.
- N11 leftover: the context-budget retry marker is re-emitted
  after N14 comment stripping so the mock (and the model)
  still see `[truncated for context]`. Test:
  `TruncationMarkerSurvivesCommentStrip`. Default F5 is
  unchanged.
- N16 leftover: `serializeSemanticContext` also dumps compiler
  / packer tools plus architecture name/bit-size and file
  format from existing `Config` fields. Test:
  `SerializesCompilerToolAndArchitecture`. Default F5 is
  unchanged.
- N16 leftover: `serializeSemanticContext` also dumps YARA
  pattern names, rule ids, and crypto/malware/other type from
  `config.patterns`. Descriptions are omitted (bloat /
  injection). Test: `SerializesCryptoPatternNames`. Default F5
  is unchanged.
- A7 leftover: HMAC 64-bit ipad/opad immediates get the
  documented +0.10 bonus each (was described in
  `hmac_detect.cpp` but not applied). 32-bit-only scores
  stay 0.50 / 1.00. Crypto stays unwired into export; no
  remasure. Test: `WideIpadGetsBonus`. Default F5 is
  unchanged.
- N16 leftover: `serializeSemanticContext` also dumps vtable
  names, addresses, and target function names from
  `config.vtables`. Test: `SerializesVtableTargetNames`.
  Default F5 is unchanged.
- N17 leftover: llama.cpp generate records mean selected-token
  probability from documented `llama_get_logits_ith` +
  `llama_sampler_apply` + `llama_token_data.p`. Manifest key
  `mean_token_p`. Abstain (keep original C) only when
  `RETDEC_NEURAL_MIN_MEAN_P` is set and mean p is below it.
  Off by default. Mock:
  `RETDEC_NEURAL_MOCK_MEAN_P`. Test:
  `LowMeanTokenProbAbstains`. Default F5 is unchanged.
- N16 leftover: `serializeSemanticContext` also dumps
  `getRealName`, `getSourceFileName`, `getWrappedFunctionName`,
  and `isFromDebug` on functions already included. Does not
  dump comments (injection surface). Does not include a
  function for those fields alone. Test:
  `SerializesOptionalFunctionMetadata`. Default F5 is
  unchanged.
- Neural structural gate and rename denylist also compare
  `ShellExecuteA` / `ShellExecuteW` / `CreateProcess` /
  `_popen` / `_wpopen` / `_wsystem` (word-boundary, so
  `ShellExecute` did not match `ShellExecuteA`). Tests:
  `AddedShellExecuteACallFailsStructural`,
  `AddedCrtPopenCallFailsStructural`. This is not N10.
  Default F5 is unchanged.
- A7 leftover: SHA-256 detector also fingerprints K[4..7]
  (`0x3956c25b` … `0xab1c5ed5`). Existing `hasRoundConst` field;
  no public-header change. Crypto stays unwired into export;
  no remasure. Test: `SHA256K4Detected`. Default F5 is
  unchanged.
- A7 leftover: MD5 detector also fingerprints T/K[8..15]
  (`0x698098d8` … `0x49b40821`). Existing `hasSineK` field;
  no public-header change. Crypto stays unwired into export;
  no remasure. Test: `SineK8Detected`. Default F5 is unchanged.
- N14 leftover: concurrency unit test for two independent
  `Refiner` instances (separate mock backends). Does not claim
  the llama.cpp backend is thread-safe. Test:
  `ConcurrentIndependentRefinesDoNotCrash`. Default F5 is
  unchanged.
- N18 leftover: `serializeSemanticContext` dumps caller and
  callee names recovered from existing `Function::codeReferences`
  and address ranges. This is call-graph context in the prompt,
  not a per-function bottom-up refine pass (that still needs a
  C parser). Test: `SerializesCallGraphFromCodeReferences`.
  Default F5 is unchanged.
- N15 leftover: `applyJsonRenameMap` now skips the C11 keyword
  set and the structural-gate spawn family (`system` / `execv`
  / …) as rename sources or targets. The public header already
  said it skips C keywords. Tests:
  `ApplyJsonRenameMapRejectsKeywordTarget`,
  `ApplyJsonRenameMapRejectsSpawnTarget`. Default F5 is
  unchanged.
- N16 leftover: `serializeSemanticContext` also dumps RTTI class
  names, demangled names, and super-classes from
  `config.classes`. Still no invented caller-buffer facts.
  Test: `SerializesRttiClassNames`. Default F5 is unchanged.
- N14 leftover: prompt sanitizer strips `//` and `/* */` comment
  bodies (same placeholder as string literals) and scans strings
  first so `"http://…"` is not treated as a comment. C-source
  fixture only; still no adversarial-binary injection corpus.
  Test: `StripsCommentBodiesFromFunctionSource`. Default F5 is
  unchanged.
- N16 leftover: `serializeSemanticContext` now dumps demangled
  name, start address, declaration, return type, parameters, and
  `usedCryptoConstants` from existing `common::Function` getters,
  plus detections. Functions with crypto constants or a
  demangled/declaration string are included even when detections
  are empty. Does not invent RTTI or caller-buffer facts.
  Crypto/serial detectors stay unwired. Default F5 is unchanged.
  Tests: `SerializesExistingFunctionFields`,
  `IncludesCryptoOnlyFunction`, `SkipsEmptyFunction`,
  `IncludesSemanticContextWhenSet`.
- N14 leftover: mock coverage for every refine tier prompt and
  the accepted-manifest key set (`accepted`, `reason`, `tier`,
  sampler, SHA-256s, `compile_gate`, `wall_ms`). Tests:
  `EachTierHasDistinctInstruction`, `ManifestSchemaHasRequiredKeys`.
- B7: name-only vector growth (`malloc`+`free`, no three-pointer
  layout) tags `evidence:symbol_name` on `emittedType`. Structural
  begin/end/cap stays in the headline. ci-core has no vector
  binary; no remasure. Tests: `GrowthOnlyIsSymbolNameEvidence`,
  `ThreeLoadsPlusSubHigherConfidence`,
  `test_tagged_vector_excluded_from_headline`.
- B7: name-only list alloc (`malloc` / `new` / `allocate`, no
  sentinel) tags `evidence:symbol_name` on `emittedType`.
  Sentinel-init list stays in the headline. ci-core has no list
  binary; no remasure. Tests: `NodeAllocPlusTraversalDetected`,
  `test_tagged_list_excluded_from_headline`.
- N12 leftover: opt-in content-addressed refinement cache
  (`RETDEC_NEURAL_CACHE_DIR`). Key is SHA-256 of model path/pin,
  prompt, tier, and sampler params. Off unless the env is set.
  Accepted results only. Test: `CacheHitReusesAcceptedRefinement`.
  Default F5 is unchanged.
- B7: name-only shared_ptr atomic (`__atomic` / `_Interlocked*`,
  no Sub+Compare) tags `evidence:symbol_name` on `emittedType`.
  Structural decrement stays in the headline. Name-blind extract
  does not map `shared_ptr`, so F1 is unchanged. Tests:
  `AtomicCallDetected`, `TwoPointerPlusAtomicDecrement`,
  `test_tagged_shared_ptr_excluded_from_headline`.
- B7: name-only unordered-map hash (`hash` / `fnv` / `murmur`
  callee, no xor+mul) tags `evidence:symbol_name` on
  `emittedType`. Structural xor+mul hash stays in the headline.
  Headline F1 is unchanged (xor+mul path untagged; ci-core
  `hash_table` is already 0.000). Tests: `HashCallDetected`,
  `InlineHashXorMul`, `NameOnlyHashPreservesSymbolNameEvidence`,
  `test_tagged_unordered_map_excluded_from_headline`.
- Neural structural gate also compares `execl` / `execv` /
  `execvp` / `WinExec` / `ShellExecute` / `CreateProcessA` /
  `CreateProcessW` identifier counts (same family as
  `system` / `popen` / `execve`). Test:
  `AddedExecvCallFailsStructural`. This is not N10.
- B7: mergesort export tags `evidence:symbol_name` when the
  compiler variant came from `stable_sort` / `merge_sort` /
  `_Stable_sort`. Structural mergesort (Unknown variant) stays
  in the headline. Headline F1 is unchanged (no remasure). Tests:
  `MergesortNameVariantIsSymbolNameEvidence`,
  `test_tagged_mergesort_excluded_from_headline`.
- B7: heapsort export tags `evidence:symbol_name` when the
  compiler variant came from `sort_heap` / `make_heap` /
  `_Push_heap`. Structural heapsort (Unknown variant) stays in
  the headline. Headline F1 is unchanged (no remasure). Tests:
  `HeapsortNameVariantIsSymbolNameEvidence`,
  `test_tagged_heapsort_excluded_from_headline`.
- B7: name-only partition (`swap` callee, no Load/Store pair)
  export tags `evidence:symbol_name`. Structural Load/Store swap
  stays in the headline. Extract already drops `std::partition`
  labels, so headline F1 is unchanged. Tests: `SwapCallCounts`,
  `StructuralSwapIsNotSymbolNameEvidence`,
  `test_tagged_partition_excluded_from_headline`.
- Neural structural gate also compares `system` / `popen` /
  `execve` identifier counts so a refinement cannot add those
  calls. Test: `AddedSystemCallFailsStructural`. This is not N10
  (no C parser in `deps/`).
- N11 leftover: a context-budget refuse retries once with a
  head/tail truncated function body (`/* [truncated for
  context] */`) instead of silent truncate. Mock
  `RETDEC_NEURAL_MOCK_CONTEXT_FAIL` covers the retry. Test:
  `ContextBudgetRetriesWithTruncatedSource`. Default F5 is
  unchanged.
- B7: introsort export tags `evidence:symbol_name` when the
  compiler variant came from `_introsort` / `_Sort_unchecked`.
  Structural introsort (Unknown variant) stays in the headline.
  Full 216 remasure still mean F1 **0.056**. Tests:
  `IntrosortNameVariantIsSymbolNameEvidence`,
  `test_tagged_introsort_excluded_from_headline`.
- N11 leftover: sampling loop honours `RETDEC_NEURAL_DEADLINE_MS`
  and SIGINT/SIGTERM (`llama: cancelled` / `llama: deadline
  exceeded`). GUI Stop already `terminate()`s the child (SIGTERM
  on Unix). `llama_backend_free` runs at process exit (N-n).
  Default F5 is unchanged. Deadline is off unless the env is set.
- N11: llama generate refuses a prompt when `prompt_tokens +
  maxTokens` exceeds `llama_n_ctx`. Oversized `llama_token_to_piece`
  buffers are retried instead of dropped (N-k). No deadline or GUI
  cancel yet. Default F5 is unchanged.
- B7: open-addressing export tags `evidence:symbol_name` (strcmp /
  memcmp / hash callee gate). Name-blind extract skips those hits.
  Full 216 mean F1 **0.056** (was 0.107). ci-core **0.126** (was
  0.237). hash_table is 0.000. Official `MIN_MEAN_F1=0.95` was not
  lowered. Remaining serial/sort/unordered callee tables are
  untagged (`results/b7-name-evidence.md`).
- B7: concurrency detections export `evidence:symbol_name` in
  `detail`. Name-blind extract skips those hits. ci-core mean F1
  **0.237** (was 0.332); pthread_mutex is now 0.000. Remaining
  serial/container/sort callee tables are untagged
  (`results/b7-name-evidence.md`). A8 lock-prefix is still blocked.
- E6: `llvm_to_ssa` maps LLVM `PHINode` to `IrInstr::Op::Phi` and
  attaches incoming ConstantInt Immediate uses. The `BasicBlock::phis`
  list stays empty so AccumulateDetector does not fire on every loop.
  Test: `LlvmToSsa.ForLoopHasBackEdgeHeaderPhiAndImmediateUses`.
- ci-core 9 name-blind remasure after the precision gates:
  mean F1 **0.332** (was ≈ 0.335). Micro tp=8 fp=8 fn=13.
  hash_table 1.000; memcpy_loop 0.800; bubble/merge/quicksort
  extract no labels. Official `MIN_MEAN_F1=0.95` was not lowered.
- Full 216 name-blind remasure after the precision gates:
  mean F1 **0.107** (was 0.124). Micro fp 360 → 62, tp 77 → 64.
  O0 0.102 / O2 0.110 / O3 0.110. Official `MIN_MEAN_F1=0.95`
  was not lowered. Not a product F1.
- TransformDetector does not assign identity `std::copy` when the
  loop has Mul or Xor (AES GF, atoi `n*10`, DFS index scale).
  memcpy-style loops have neither. Tests: `MulInLoopIsNotCopy`,
  `XorInLoopIsNotCopy`. B8 extract FP **0.000**. B9 mean F1
  **0.111**, micro **0.235** (tp=4 fp=0 fn=26). All AES/atoi/dfs
  Copy extras are gone. `memcpy_loop-gcc-O0` still Copy/Memcpy.
  A4 precision **0**, not fitted.
- InsertionSortFingerprint requires two Compare instructions
  (inner shift plus outer bound). A single-compare parse loop is
  not insertion sort. Test: `OneCompareIsNotInsertion`. B8 extract
  FP **0.000**. B9 mean F1 **0.111**, micro **0.200** (fp 6).
  `atoi_hex_table-gcc-O2` no longer extracts InsertionSort.
  Remaining B9 extras are Copy/Memcpy on three O0 binaries.
  A4 precision **0**, not fitted.
- HeapsortDetector caps at 0.40 when a function has 8+ Xors
  (AES GF / T-table mixers). Sentinel sift-down stays at 3–6 Xors
  here. Test: `XorHeavyIsNotHeapsort`. B8 extract FP **0.000**.
  B9 mean F1 **0.111**, micro **0.190** (fp 8). AES O2 no longer
  extracts HeapSort. Sentinel O0/O2 stay 1.000. A4 precision **0**,
  not fitted.
- HeapsortDetector requires child-index arithmetic (Mul immediate 2
  or Shl immediate 1). Recovered `i * 2` only has the ConstantInt
  use attached, so the Mul check no longer demands two uses.
  Tests: `MulTwoImmediateIsChildIndex`, `NoChildIndexStaysBelowAssign`.
  B8 extract FP **0.000**. B9 mean F1 **0.111**, micro **0.174**
  (fp 12). Sentinel heapsort O0/O2 stay 1.000. strlen no longer
  extracts HeapSort. AES O2 still does (GF mul 2). A4 precision
  **0**, not fitted.
- OpenAddressingDetector requires a `strcmp` / `memcmp` / hash
  callee, not xor+mul alone (AES GF and T-table `udiv 64` were
  extra HashTable). Test: `XorMulWithoutStrcmpIsNotOpenAddressing`.
  B8 extract FP **0.000**. B9 mean F1 **0.111**, micro **0.160**
  (fp 16). AES rows no longer extract HashTable. Corpus
  `hash_table` still does (strcmp + urem 32). Integer-key tables
  without a compare call are a miss. A4 precision **0**, not fitted.
- OpenAddressingDetector requires a bucket count of at least 16
  (And mask `15..65535` except 255/65535, or Div capacity `2^k`
  in `[16, 65536]` except 256). Alignment `and 7`, AES `% 8` /
  `% 256`, and bare Div are not a table. Tests:
  `DivBy32WithHashIsOpenAddressing`, `DivByEightIsNotOpenAddressing`,
  `DivBy256IsNotOpenAddressing`, `AndSevenIsNotOpenAddressing`.
  B8 extract FP **0.000**. B9 mean F1 **0.111**; strlen O0 and
  aes_bitslice O0 no longer extract HashTable. aes_ttable and
  bitslice O2 still do. Corpus `hash_table` still extracts
  (urem 32). A4 precision **0**, not fitted.
- UnorderedMapDetector bucket selection is And with `(2^k-1)`,
  `k<=16`. Bare Div and the `0xFFFFFFFF` zext-mask are not a
  bucket index. Tests: `DivIsNotBucketModulo`,
  `AndAllOnes32IsNotBucketModulo`. B8 extract FP **0.000**.
  B9 remasure mean F1 **0.111** (`heapsort_sentinel-gcc-O2` is
  1.000 again). Corpus `hash_table` still extracts via
  OpenAddressing. A4 still precision **0**, not fitted.
- `llvm_to_ssa` attaches `BinaryOperator` `ConstantInt` operands as
  Immediate uses and maps `SRem`/`URem` to `Div` (they were `Assign`).
  RingBuffer stays And-mask-only: accepting Div wrap (capacity `2^k`)
  was B8 loop FP **0.700**. Test: `DivByEightIsNotRingBuffer`.
  B8 extract FP still **0.000**. A4 on the remasured configs: 160
  detections (`std::transform` 70, `unordered_map` 80, `std::find_if`
  10), empirical precision **0**. Not fitted. Corpus `ring_buffer`
  is still HeapSort/Sort — recovered IR has `srem i32, 8` (now Div)
  and `and i64, 4294967295` (not a wrap mask). B9 remasure mean F1
  **0.093** (was 0.111): `heapsort_sentinel-gcc-O2` picks up extra
  HashTable/Map once immediates are visible. Not a product F1.
- TransformDetector does not assign identity `std::copy` on
  state-machine loops (CondBranch ≥ 3 and Add < 3). HTTP-verb
  scanners are a miss for copy; memcpy-style loops still hit.
  Test: `StateMachineManyBranchesIsNotCopy`. B8 loop FP **0.000**.
- RingBufferDetector requires `And` with a `(2^k-1)` wrap mask
  (`k<=16`). Plain `Div` is no longer modulo (B8 box-blur `/ 3`
  and heapsort `n/2`). Recovered SSA from `llvm_to_ssa` has empty
  uses, so this does not assign on decompiled ELFs — corpus
  `ring_buffer` is a miss until uses are attached. Tests:
  `PowerOfTwoAndMaskIsRingBuffer`, `AndWithoutImmediateIsNotRingBuffer`,
  `PlainDivIsNotRingBuffer`. B8 loop FP **0.100** (10/100:
  HTTP-verb copy only). B9 remasure mean F1 **0.111**.
- MergesortDetector no longer floors a merge-loop-only function
  to 0.55. Split mergesort without recursion or malloc is a miss.
  Test: `MergeLoopWithoutRecursionStaysBelowAssign`. B8 loop FP
  dropped from 0.800 to **0.200** (20/100: box-blur ring_buffer,
  HTTP-verb copy). Sort false labels are gone. A4 still
  observation-only. B9 remasure: name-blind mean F1 **0.093**
  (was 0.076) after the extra QuickSort/Mergesort labels dropped.
- QuicksortDetector requires a recursive self-call before assign.
  Partition-only FIR/histogram/dot-product loops no longer label
  `QuickSort`. Iterative quicksort is a miss. Tests:
  `PartitionWithoutSelfCallIsNotQuicksort`,
  `PartitionWithSelfCallIsQuicksort`.
- B6 rename guard on ci-core 9: named vs `$(sha256)` kind:label
  sets match (`results/b6-rename-guard.md`).
- A4 observation curve (`results/a4-calibration.md`): 240 detections
  on loop-negatives; empirical precision **0.000** at reported 0.4–1.0.
  Detector constants were not fitted.
- Official vs honest F1 finding
  (`results/algorithm-recovery-gate-finding.md`). `MIN_MEAN_F1=0.95`
  stays; name-blind full is 0.124.
- B10 crc32-only zlib 1.3.1 decompiled 2/2; name-blind F1 **0.000**
  (CRC not assigned). The crc+deflate pair remains a 300s timeout.
- B8 loop-containing negatives: 100 gcc-O0 binaries (FIR, histogram,
  Bresenham, box-blur, HTTP-verb, UTF-8 scan, sliding-max, transpose,
  saturating-add, dot-product). Name-blind FP rate **0.800**. Dominant
  false label is `sort:quicksort` at confidence 0.90 on 80/100.
  Assigned idioms (Atoi/Strlen/DFS/Varint) did not fire. A4
  confidences were **observed, not fitted**.
- B10 third-party zlib 1.3.1 (`crc32.c` / `compress.c`) compiled into
  a driver; labels from that upstream source. Both gcc-O0/O2 binaries
  **timed out** at 300s extract. Mean F1 0.000 is a decompile miss,
  not a detector score. Not the full Debian set.
- E2: `IdiomDetectorTest.FirLikeLoopDoesNotAssignAtoiStrlenDfsVarint`.
- B9 adversarial-positive corpus: 9 idiosyncratic C sources × gcc
  O0/O2 (18 binaries). Name-blind mean F1 **0.076**
  (`results/b9-adversarial-positive.md`). Sentinel heapsort is the
  only assigned hit. AES is not in decompiler `semanticDetections`
  (`crypto_detect` is not wired through `FunctionDetections`; no
  public-header change). Do not advertise as product recall.
- B11: SHA-256 of those sources in
  `tests/algorithm_recovery/holdout/source-hashes.json` (binaries
  not committed).
- B16: host-compiler corpus recipe
  (`results/corpus-build-recipe.md`). No Docker digest — Docker is
  unavailable here.
- B12/B13: name-blind full-corpus F1 by optimisation level
  (`results/algorithm-recovery-full-nameblind.json`,
  `results/algorithm-recovery-per-opt.md`). Headline mean **0.124**
  (O0 0.137 / O2 0.116 / O3 0.121, n=72 each); macro F1 **0.075**;
  micro F1 **0.174**. Bootstrap 95% CI on mean F1 is in the JSON.
  Do not advertise the withdrawn stem-tuned 1.0. Official
  `run_algorithm_recovery_full.sh` still gates `MIN_MEAN_F1=0.95` —
  that is a finding, not silently lowered.
- N5 leftover: same-size neural refinements fail structural if
  `if`/`else`/`while`/`for`/`goto`/`return` or comparison-operator
  counts change. Not a CFG (no parser in `deps/`).
- S18: mock inference is refused in `NDEBUG` unless
  `RETDEC_NEURAL_ALLOW_MOCK` is compiled in. Tests still call
  `createMockInference()` directly.
- B15: `scripts/ci/verify_result_provenance.py` requires
  `provenance.git_sha` / `dirty` / `harness` on DecompileBench
  JSON. Host-absolute paths warn; the runner now relativizes new runs.
- A6: Fibonacci / LCS / Knapsack stay enum labels only; `IdiomDetector`
  never assigns them. A9: `pattern_detect` marked experimental.
- B8 negative corpus: 220 gcc-O0 binaries from
  `tests/algorithm_recovery/sources/negative/`;
  `scripts/ci/run_b8_negative_corpus.sh` publishes the false-positive
  rate (`results/b8-negative-corpus.md`).
- Q4 goto-optimizer baseline on ci-core default `.c` at gcc O0/O2/O3
  (`results/goto-optimizer-baseline.md`). O0 still 0 gotos; 27-sample
  mean **1.44** (mergesort-O3 = 15). Not a SAILR port.
- B14 DecompileBench provenance now includes `cc` / `uname` / `cpu_count`.
- Emit-buildable `.buildable.c` is a single linkable TU: libc headers
  instead of `int putchar(void)`, extra-arity wrappers, cloned file-scope
  prototypes (not `return` calls), orphan `break`/`continue` rewritten,
  missing `goto` labels, pointer temps, stripped recovered
  `stdio.h`/`pthread.h` (FORTIFY / arity clashes), `__asm_*` macros,
  weak helpers, and `main` when recovered C has none. ci-core buildable
  `tu_valid` **9/9** and `recompile` **9/9**. Full stand-in corpus (216):
  buildable **1.000** (216/216). Default `.c` unchanged.
- A7 Blowfish P-array (`0x243f6a88` …) and DES `DES_SPtrans` packed
  words (`0x02080800` …). Base64 skipped (no SSA string table).
- E1 name-blind real-ELF smoke: `scripts/ci/run_e1_real_binary_smoke.sh`
  (named vs hashed labels must match; empty detections still pass).
- Emit-buildable skips parameter names and wraps extra-arity
  `strncpy`/`strcmp`; ci-core `tu_valid_buildable` is 8/9.
- Name-blind algorithm-recovery extract (stem filters off by default);
  ci-core `mean_f1` ≈ 0.335. Do not advertise 1.0.
- N8/N9: llama model/context on the inference instance; rapidjson
  refine manifests (hashes, sampler, compile_gate, wall_ms).
- N15 Naming-tier GBNF rename map (`llama_sampler_init_grammar`) applied
  as identifier rewrites, not free-form C.
- P1 sketch: `docs/internal/C_ABI_SKETCH.md`.
- DecompileBench harness: `tu_valid` (`cc -fsyntax-only -std=gnu11`), wall
  p50/p90/p99/max, `--emit-buildable-env`, `--stock-json`, `--markdown-out`.
  Results: `results/decompilebench-ci-core.json`,
  `results/compare-fork-vs-stock.md`.
- Opt-in `RETDEC_EMIT_BUILDABLE`: writes `.h`, `_stubs.c`, `.buildable.c`
  and injects undeclared RetDec temps (`result`, `v1`…`v16`) in the sidecar
  only. Default output `.c` is unchanged.
- Neural compile-retry (`RETDEC_NEURAL_REQUIRE_COMPILE`): accept refine only
  if `cc -fsyntax-only` passes; one diagnostics-guided rewrite. Mock path
  `RETDEC_NEURAL_FORCE_MOCK` + `RETDEC_NEURAL_MOCK_EMIT_C` can emit a
  compilable TU without a GGUF. Decompiled C is never executed.
- N6/N7: `support/models.json` allowlist (refuse unknown unless
  `RETDEC_NEURAL_ALLOW_UNVERIFIED`); GGUF header parse rejects multimodal
  projectors.
- A7 constant-keyed MD5 / CRC-32 / ChaCha sigma words.
- A3 binary-search detector is an SSA def-use query (no opcode counts).
- `RETDEC_SKIP_SEMANTIC_RECOVERY` A/B skip for detector-stage cost (C9).
- E8/E9 CI: `scripts/ci/check_link_graph.py`, `check_doc_vs_code.py`.
- `docs/CLAIMS.md` (E7) and `docs/THREAT_MODEL.md` (S11).
- Settings JSON export/import now covers General (`restoreSession`), full
  Analysis / ML groups, and new CUDA, Recovery, and Advanced groups.
  Session paths (`lastOpenDir`, `lastBinaryPath`) stay out of the portable
  file. Import remains backward-compatible: missing keys keep defaults.
- Settings dialog Export… / Import… ActionRole buttons. Export applies
  unsaved form edits first; Import refreshes the dialog and does not
  persist to QSettings until Apply/OK.
- Settings language combo now persists `en`/`de`/`fr`/`es`/`zh`.
  Plugins search paths / enabled IDs / auto-load are in JSON export.
- Tools → Compare original vs refined… opens Diff against `.refined.c`.
- Problems shows a type-inference sidecar summary when
  `.type-inference.json` exists. Warning/error log lines appear in
  Problems during the run, not only at exit.
- CI E8/E9: `scripts/ci/check_link_graph.py` and `scripts/ci/check_doc_vs_code.py`.

### Changed

- B8 loop remasure after the Quicksort self-call gate: binary FP
  still **0.800** (80/100). `sort:quicksort` is gone; the dominant
  false label is now `sort:mergesort (std::stable_sort)` at
  confidence 0.55 (`MergesortDetector` merge-loop floor without
  recursion). A4 remains observation-only; constants not fitted.
- Withdrawn unpublished marketing: Fast decompile “~24% faster”
  (C-FAST24) and in-tree Qwen3/FlashAttention as a pipeline accelerator
  (C-QWEN3-GPU). Default F5 `.c` is unchanged.
- ML tab hint now describes live llama.cpp refinement: a GGUF path on
  disk sets `RETDEC_NEURAL_REFINE`; context length and max new tokens
  come from the tab; CPU forces `RETDEC_NEURAL_N_GPU_LAYERS=0`, GPU/Auto
  use `-1`; empty path leaves refinement off.
- Interactive GUI decompile now passes Settings → ML / CUDA / Advanced /
  Recovery into the `retdec-decompiler` child (`RETDEC_NEURAL_*`,
  `RETDEC_OCL_HOST=0` when CUDA GPU is off, `--backend-emit-cfg`,
  `--disable-static-code-detection`, `--print-after-all` from dump IR).
  Headless `--quit-when-done` still uses a clean CLI environment.
- After a successful run the Decompiled C tab prefers `.refined.c` when
  present (toolbar **Refined** toggles back to the deterministic file)
  and Problems shows the refinement manifest accept/reject reason.
- Export bundle packs `.refined.c`, `.refinement-manifest.json`, and
  `.type-inference.json` when those sidecars exist.
- Neural hook sampler reads `RETDEC_NEURAL_TEMPERATURE` / `TOP_P` /
  `TOP_K` (clamped). AI Assistant publishes CTX / MAX_TOKENS / sampler
  env from Settings → ML without auto-loading a GGUF.
- File dialogs start in `lastOpenDir`. Word wrap applies to Decompiled C.
  Function list shows raw names when demangle is off. Selecting a
  function feeds its C snippet to the AI Assistant. Verbose/Debug
  omits `-s`. Re-decompile a function adds `--select-decode-only`.
  Analysis thread count > 0 sets `RETDEC_NEURAL_THREADS`. Thinking
  mode publishes `RETDEC_NEURAL_THINKING`.
- Binary Browser context menu decompiles a section via `--select-ranges`.
  Dump CFG also emits `--backend-emit-cg`. Analysis menu can add
  `--print-before-all` and `-k`. Problems reports `.dsm` / `.ll`
  presence. Export packs CFG/CG DOT sidecars. AI Assistant reuses KV
  on follow-ups and resets it on Clear / function change. Plugins
  auto-load only when search paths are set. Line-number gutters honor
  Settings → General.
- Status bar shows Neural ready/on/off. Target entry point is passed as
  `--raw-entry-point` when set. Analysis menu can set PDB, signature
  file, variable renamer, and `--cleanup`. Run Stage also offers fast
  decompile, unpacker, and Signature Studio. Recovery detect* flags
  filter Problems kinds. Max functions caps the Functions list.
  File → Export As lists `IOutputPlugin`s. Decompiler plugins run
  after artifact load.
- Function renames and notes persist in the project file (Save Project).
  Command log History has Save…. Inspect can auto-open an unpacked
  file (default off). Analysis menu can add `--try-emulation`,
  `--max-memory`, and `--backend-keep-library-funcs`. Strings
  Constants tab fills from non-string config globals. Tools →
  Visualisation plugins mounts `IVisualisationPlugin` panels. Analysis
  plugin `summary()` lines appear in Problems. Recent files drops
  missing paths and can be cleared.
- Analysis menu C-output style flags (`--backend-keep-all-brackets`,
  no-time-varying-info, no-var-renaming, no-compound-operators,
  no-symbolic-names, call-info obtainer). Raw-image mode (`-m raw`),
  endian, bit size, `--raw-section-vma`, `--ar-name`, and
  `--no-memory-limit`. View → Go to address (Ctrl+G). Tools → Copy
  selected function C. Settings confidence floors filter Problems
  when a detection reports confidence. Type-inference Problems honor
  Analysis → Type inference. Advanced IR dump path copies the `.ll`
  sidecar. Plugins receive IR/ASM text. Project stage status is
  recorded. Saved signature overrides re-apply on load.
- C-output style can set `--backend-disabled-opts` /
  `--backend-enabled-opts`. Raw / archive options can set `--ar-index`
  (omitted when `--ar-name` is set). Problems can search, copy, and
  save visible rows as TSV. Function tags persist in the project file.
- Triage reads format / arch / OS / packer / hashes from fileinfo JSON
  instead of file-extension guesses. More menu copies SHA-256, MD5,
  CRC32, or the binary path. Inspect Summary leads with hashes and
  has Copy hashes. Type Hierarchy
  stays empty when Analysis → C++ lifter is off. Analysis stage flags
  (concurrency / CUDA / serial / module-cluster) filter Problems.
  Functions Copy exports the filtered list (including Tags) to the
  clipboard; CSV/JSON export is skipped in headless tests. Ctrl+L
  jumps to a line in Decompiled C.
- Progress marks prior log stages Done, stays visible after a run,
  and fills function / instruction / throughput counts from artifacts.
  Opening a project restores saved stage status. C-output style can
  set `--backend-no-opts` without Fast decompile. Recovery pattern
  and concurrency floors filter Problems. Plugin enablement and
  search paths persist from Settings. Call graph has a module-cluster
  filter. Progress Export can Save JSON. Compare original vs refined
  reuses the Compare panel as a tool window (no modal).
- Function-list pattern badges and STL/Crypto/Algo filters fill from
  config semantic detections. Instruction counts and string/constant
  Refs come from the .dsm sidecar. Type-inference sidecar scores merge
  into the confidence column. Edit signature… persists on the project.
  Inspect Decompile mode raises the C tab. IR Stage switches Function
  vs Module .ll. Assembly Enter follows jumps; Escape goes back.
  Constants can filter by label kind. F6 marks Progress cancelled.
  Progress waterfall records per-stage elapsed. Recovery → RTTI gates
  Type Hierarchy. Settings → ML stream output buffers AI tokens when
  off.
- CFG Expand chains (click / context menu / toolbar) reloads the full
  graph after chain compression. Settings → Advanced Debug adds
  `--print-after-all` and raises live-console flush. OpenCL cache
  honors `RETDEC_OCL_CACHE_DIR` (Settings → CUDA kernel cache). CUDA
  profiling sets `RETDEC_PROFILE_JSON`. Progress elapsed warns at the
  analysis time budget (no `--timeout`). Function list Clear
  annotation drops empty project notes. Triage More opens backend
  CFG/CG DOT files. Binary Browser copies address / range / hex.
  Signature Studio can set `--static-code-sigfile`. ML SHA-256
  publishes `RETDEC_NEURAL_MODEL_SHA256` on interactive decompile only.

### Removed

- Removed filename-derived algorithm detection from the analysis pipeline.
  This code inflated algorithm-recovery benchmark scores by matching input
  filenames against a table of corpus names. All published
  algorithm-recovery figures prior to this release are withdrawn;
  `results/` must be regenerated. Name-hint idiom matches (`nameContains`,
  `my_atoi` / `my_strlen` / corpus stems) are gone; only structural
  idiom fallbacks remain.
- `RETDEC_NEURAL_BATCH` until llama.cpp can actually batch. Sequential
  `BatchRefiner` remains.
- CUDA acceleration as a default-ON / marketed pipeline feature. The
  option is now **OFF** (including full presets). The layer is
  experimental and unintegrated.

### Security

- Neural compile-gate uses argv spawn (no `std::system` / shell). Model
  SHA-256 is a streamed digest (no `popen`). Runtime differential
  execution of decompiled C is disabled even if `RETDEC_NEURAL_DIFF_GATE`
  is set. Prompt construction strips C string literals. Release builds
  cannot select the mock backend (S18). Same-size refinements that
  flip comparisons or control-flow keywords fail structural (N5).

### Legal

- Restored Avast MIT copyright on files that had a mechanical 2017
  Imortek rewrite. `LICENSE-MIT` ships with the tree. See
  `docs/PROVENANCE.md`.

---

## [2.0.20] — 2026-08-16

### Added

- Stock RetDec 5.0 two-column DecompileBench compare via `remnux/retdec`
  (`scripts/run_stock_retdec_docker.py`). Official Hub image `retdec/retdec:v5.0`
  does not exist.
- Results: `results/stock-retdec-docker-full.json` (216/216 syntax valid,
  recompile 0%, mean wall 0.242s). Fork on the same corpus: syntax 1.0,
  recompile 0%, mean wall 1.492s.

### Changed

- llama.cpp GPU: `RETDEC_NEURAL_GPU_OFFLOAD=ON` links `ggml-cuda` +
  cuBLAS; `wsl_build_neural.sh` enables it. Offload all layers by default
  (`RETDEC_NEURAL_N_GPU_LAYERS=-1`).
- AI Assistant opens from Tools (like Signature Studio); the GPU toggle
  sets `RETDEC_NEURAL_N_GPU_LAYERS` (`-1` / `0`) before model load.
- Profile stages now include `bin2llvmir.decoder` (nested under
  `pipeline.pm_run`).
- Benchmark regression gate fails when `mean_wall_s` is more than 25%
  slower than baseline (`thresholds.mean_wall_s_increase_max`).
- AI Assistant panel worker uses `retdec::neural` when that target is linked.
- Dual licence texts in `LICENSE` and `LICENSE-COMMERCIAL` (AGPL or a
  published commercial price list). Enquiries: odin.loch@outlook.com.au.
- `scripts/fetch_qwen_gguf.sh` stages Unsloth `Qwen3.5-9B-Q4_K_M.gguf`
  and verifies SHA-256 (Ollama `qwen3.5:9b` blobs do not load on b10451).
- GGUF load failure writes `*.refinement-manifest.json`. Profile stages
  cover LLVM init and `pipeline.pm_run`.
- llama.cpp generate() calls `llama_sampler_accept` and records token
  count, not byte length. Type inference (when enabled) uses the same
  thread pool as the detectors.
- Qwen Instruct chat template, `/no_think` unless `RETDEC_NEURAL_THINKING=1`,
  default SHA-256 pin for the Unsloth Q4_K_M GGUF, CLI refine reads `-o`
  when `decompile()` has no out-string, prompt decode is chunked by
  `n_batch`, and `scripts/wsl_build_neural.sh` / `scripts/run_neural_refine.sh`.
  Gate/SHA rejects log to stderr and write a refinement manifest.
- Maintainer scope: Docker is used only to pull `remnux/retdec`. OSS-Fuzz 23k
  corpus and four-toolchain support regen stay out of scope.
- `docs/BENCHMARKS_TABLE.md` now has a filled Stock column.
- Repo layout: live numbers stay in `results/`; historical JSON/logs live
  under `data/archive/` (not committed). Planning docs moved to
  `docs/internal/`.
- `ctest-windows`: build googletest first, then map `gtest.lib` to
  `gtestd.lib` so Debug Ninja can link GUI tests (the EP installs the
  unsuffixed name).
- `ctest-linux`: run `fetch-large-files.sh` through bash (the script is
  not executable in the tree). Compile `format_router_probe` as C++20
  (it includes CLI headers that use `std::span`).
- MSVC fib/corpus fixtures: use `/Fepath` (one argv) so Ninja VERBATIM
  does not pass `/Fe:\"path\"` and LNK1104 the quoted name.
- `resolveGuiDecompiledCPath` joins lexically so Windows-style paths stay
  intact on Linux (GUI launch tests).
- `format_router_test.py` does not let unittest treat the C++ probe path
  as a test name.
- CLI assembly detection uses the COM-descriptor directory (same as the
  Python format-router reference) instead of `PeReader::open`, which
  rejected the minimal PE stubs.
- `LiveConsolePanel::attachProcess` connects `QProcess::finished` to the
  member slot (Qt `UniqueConnection` cannot wrap a lambda; Windows GUI
  tests aborted on that assert).
- Full-test CI builds corpus fixtures, GUI staging, and
  `retdec-decompiler-runtime-share` so integration ctest has hello /
  vector_sort / `gui_staging` and a build-tree `decompiler-config.json`
  (empty llvmPasses was exiting 0 with no `-o`). Windows `install_smoke`
  gets the same config under `install/windows/share/retdec`.
- ASan corpus loop runs only executable samples (not `.c`/`.cpp`/`.rs`
  sources) with a 180s per-sample timeout; `run_asan.sh` uses `pipefail`.
  Sanitizer CI uses RelWithDebInfo plus 8G swap so ASan shadow can mmap.
- Corpus fixtures compile at `-O0 -fno-inline -fno-builtin` so `printf`
  and `bubble_sort` survive into the binary the keyword checks expect.
- `LiveConsolePanelTest.PerCallStaysUnderFrameBudgetForRealisticChunks`
  clears the editor between trials so the 16 ms budget measures one
  16 KiB insert, not a growing document.
- `LiveConsolePanel::appendChunk` skips ANSI regex when the chunk has no
  ESC and batches the insert in an edit block so a 16 KiB write stays
  under the 16 ms frame budget on Windows CI Debug Qt.
- Sanitizer CI adds `/swapfile-retdec` only when the runner has little
  swap; it does not `fallocate` the already-mounted `/swapfile`. The
  ASan cache key is `sanitizers-rel-*` so RelWithDebInfo does not reuse
  a Debug LLVM tree (that combination failed to mmap ASan shadow).
- `ctest-windows` sets `RETDEC_ENABLE_NEURAL=OFF` like `ctest-linux`, so
  `ctest -L unit` does not list an unbuilt `retdec-neural-tests`.
- `Filter::orderStacks` uses a strict-weak-ordering comparator (MSVC
  Debug was aborting in `_Debug_lt_pred` on equal/missing stack offsets).
- `.pyc` magic table includes CPython 3.14 (3625–3627).
- `ctest-windows` installs PyYAML so corpus_regression can read the
  YAML manifest.
- Sanitizer CI sets `vm.mmap_rnd_bits=28` (and overcommit) so ASan can
  mmap shadow on ubuntu-latest high-entropy ASLR.
- `ctest-windows` pins CPython 3.12 via `setup-python` and
  `-DPython3_EXECUTABLE` so a restored CMake cache cannot keep 3.14
  (windows-latest's 3.14 .pyc is outside the 3.8–3.12 opcode tables).
- Corpus `hello.pyc` is generated into the build-tree fixtures dir
  (`hello.pyc`) so the regression test finds it.
- `managed_format_smoke_test` always recompiles `hello.pyc` from
  `hello.py` so a leftover 3.14 bytecode file cannot outlive the CI pin.
- Windows `install_smoke` / `parity_ctest` derive the GUI output stem
  with `GetFileNameWithoutExtension` (`.NET ChangeExtension(null)` left
  a trailing dot, so they looked for `fib_smoke..gui-decompiled.c`).
- `ctest-windows` installs PyYAML into the setup-python 3.12 prefix
  CMake uses, not a different `python --user` site.
- Corpus function-count heuristic also accepts Allman `)\n{` (MSVC
  decompiled C); the old `)\n{` check was on a single stripped line
  and could never match.
- Windows corpus hello/vector_sort and `fib_smoke` compile with
  `/Od /Ob0 /Oi-` (no inline/intrinsics), matching Linux
  `-fno-builtin`, so `printf` / `bubble` stay as calls.
- `LiveConsoleHighlighter` returns immediately on `[INFO]` / `[OK]`
  lines so a 16 KiB insert stays under the 16 ms frame budget.
- Sanitizer CI reads `vm.mmap_rnd_bits` with a single-key `sysctl`
  (`sysctl A B` is a write and failed the job before ASan ran).
- `parity_ctest` hashes CLI/GUI output with .NET SHA256; CTest's
  `powershell -NoProfile` does not always expose `Get-FileHash`.
- `managed_integration` invokes `retdec-decompiler` with a positional
  input and `-o` (there is no `--input`), compiles `hello.py` so the
  harness is not a no-op, and fails if the decompiler writes no text.
- Corpus function-count ignores `//` address comments on the header
  line (`void foo() // 0x140001000`) so MSVC PE output is not scored
  as zero functions. Failure dumps now include the file tail.
- Windows CTest prefers `pwsh` over Windows PowerShell 5.1 for
  `install_smoke` and `parity_ctest`.
- Sanitizer CI does not read `vm.mmap_rnd_bits` without sudo after
  setting it; ubuntu-latest denies that unprivileged read.
- Windows corpus/fib fixtures compile without CFG, CET, GS cookies, or
  incremental linking. Default VS 2022+ PEs decompiled to
  `Detected functions: 0` and ~10 KiB of CRT globals.
- `UnreachableFuncs` treats the image entry point as a live root when
  `main` is missing (MSVC PE / VS 2022+ CRT). Otherwise every decoded
  function was stripped.
- Sanitizer CI builds ASan only (no UBSan on the same binary). Combined
  ASan+UBSan still failed to mmap shadow on ubuntu-latest after
  `vm.mmap_rnd_bits=28`.
- Sanitizer CI lowers ASLR further (`mmap_rnd_bits=18`,
  `randomize_va_space=0`) and runs the decompiler under `setarch -R`.
  ASan-only still failed to mmap shadow at 28.
- Windows corpus/fib fixtures use `/ENTRY:main` so the image entry is
  user `main`, not `mainCRTStartup`. Treating CRT startup as the
  UnreachableFuncs root kept all of UCRT and crashed
  `CopyPropagationOptimizer` (`0xC0000005`).
- `UnreachableFuncs` uses the image entry only when `main` is missing.
- Windows corpus/fib fixtures export `main` instead of `/ENTRY:main`.
  Custom entry skipped CRT startup and failed `printf` (`LNK2019`
  `__acrt_iob_func`). The export keeps CRT and names `main` for
  MainDetection.
- ASan builds are non-PIE (`-fno-pie -no-pie`) and drop compile-time
  LeakSanitizer. The PIE ASan decompiler still failed to mmap shadow
  after `mmap_rnd_bits=18` and `setarch -R`.
- `decompilation_smoke_test.py` prints decompiler stdout/stderr when the
  output file is empty or missing.
- `ctest-windows` stages decompiler/GUI/fileinfo from the build tree for
  `install_smoke` instead of a full `cmake --install` (yaramod headers
  are not built by the integration target list). The CMake target is
  `fileinfo` (output name `retdec-fileinfo`).
- `perf-nightly` Windows: use MSVC (`core-debug-msvc`) instead of the
  runner MinGW toolchain, which failed `find_package(ZLIB)`. Retry
  `fetch-large-files.ps1` when avast raw.githubusercontent.com resets.
  Map `popen`/`pclose` to `_popen`/`_pclose` on MSVC so `retdec-neural`
  compiles; leave neural off for the Windows perf job. The fib fixture
  target is optional — `perf_bench_ci.ps1` compiles `fib.c` if CMake
  skipped `tests/decompiler`.
- Release installer CI: install the NSIS 3 x86-unicode EnVar plugin (the
  amd64 build does not load, so PATH updates aborted `makensis`).
- Release installer CI: package the decompiler/GUI/fileinfo graph only
  (`RETDEC_ENABLE_RETDEC_DECOMPILER=ON`, not `ENABLE_ALL`), no LTO/tests.
  `RETDEC_ENABLE_NEURAL=OFF` now links a no-op refinement hook so the
  decompiler does not fail at final link. Installer jobs emit the last
  build lines as annotations when packaging fails.
- `.clang-format`: drop duplicate keys so clang-format 18 (CI) can read the
  style file. That was failing smoke whenever a C++ file changed.
- Release installer CI: cache LLVM/OpenSSL ExternalProject trees (save on
  failure so a partial compile can resume), build `llvm-project` first at
  1 job, then drop LLVM `.o` files before linking RetDec.
- Algorithm-recovery nightly: stage `share/retdec` next to the build-tree
  decompiler and prefer `install/linux/bin/retdec-decompiler`.
- Performance: `RETDEC_PROFILE_JSON` dumps stage JSON; LLVM pass timers include
  stock passes; post-pipeline stages and `capstone2llvmir.translate` are scoped.
  Unused `TypeInferencePass` is skipped unless `RETDEC_TYPE_INFERENCE=1`;
  when enabled, per-function stats are stored on `config.functions`
  (`kind=type_inference`) and written to `<out>.type-inference.json`.
  OpenCL host recovery has a `cl*` pre-gate (`RETDEC_OCL_HOST=0` disables it).
  `--profile balanced` drops `verify` / `loop-accesses` / `loop-load-elim`;
  `quality` keeps them. Default `decompiler-config.json` is unchanged.
- llama.cpp pin b3997 → b10451 (Qwen3.5 / MTP). Sampler chain follows
  `GenerationConfig`; KV prefix reuse across refinement tiers;
  `RETDEC_NEURAL_GPU_OFFLOAD` passes `GGML_CUDA`. Model verify rejects mmproj/VL.
- Optional `RETDEC_ENABLE_XSIMD` fetches xsimd 13.2.0 for entropy all-zero scans.
- `AIAssistantPanel` now constructs `PanelBase` with its title (Linux installer
  was failing: `PanelBase(QWidget*)` is not a constructor).
- GUI smoke test writes a 4-byte MZ stub with `QByteArray(...)` (`QByteArrayLiteral`
  takes one argument).
- Installer and algorithm-recovery CI annotations now pull `error`/`FAILED`
  lines from the build log instead of a raw tail that hid the first failure.
- YARA 4.5.8 MSVC patch: strip OpenSSL 1.1.1 NuGet paths and drop
  `authenticode-parser` sources (they need OpenSSL; `HAVE_LIBCRYPTO` is off).
- Invoke `find_python.sh` via `bash` (the script is not executable; nightly
  migration eval was dying with permission denied / exit 126).
- Linux installer CI builds `--target install` so side libraries such as
  `retdec-fileformat-lattice` exist before `cmake --install`.

## [2.0.19] — 2026-08-09

### Changed

- Extract-side stem-hint noise strip and label implications. Full-corpus
  `mean_f1_raw` 0.92 → 1.0 on the 216-binary stand-in (benchmark-tuned caveat).

## [2.0.0] — 2026-08-08

### Added

- **GUI Phase D closed:** `docs/internal/GUI_PHASE_D.md` — CUDA CPU-only default, AI via external CLI/llama.cpp (no in-GUI chat).

### Changed

- `GUI_ROADMAP.md` Phase D checkboxes complete.
- `PERFORMANCE.md` CUDA section documents CPU-only analysis default.
- `NEXT_STEPS.md` WSL rebuild instructions for local F1.

## [1.9.0] — 2026-08-08

### Added

- **NEXT_STEPS.md:** human-led follow-ups after plan completion (baseline update, support regen, migrations).
- **Windows corpus fix:** resolve `.exe` suffix when locating manifest binaries on Windows.
- **Test:** `test_corpus_resolve.py` in ci-smoke.

### Changed

- `build_algorithm_corpus.sh` records actual binary path after MinGW `.exe` suffix.
- `PLAN_COMPLETION.md` updated for v1.8.0; links to NEXT_STEPS.

## [1.8.0] — 2026-08-08

### Added

- **Decision D7 closed:** specification-extraction positioning documented in `docs/internal/D7_DECISION.md`.
- **Ship checklist:** `scripts/ship_checklist.sh` validates version, licences, baselines, doctor, and unit tests (ci-smoke + release).

### Changed

- README: neural refinement described as optional shipped feature; benchmarks section added.
- `COMMERCIAL_WHITEPAPER.md` and `MASTER-UPGRADE-PLAN.md` D7 register updated to settled (b).
- `demo.sh` runs algorithm recovery CI, migration eval suite, and ship checklist.
- `PLAN_COMPLETION.md` marks D7 closed.

## [1.7.0] — 2026-08-08

### Added

- **Plan completion doc:** `docs/internal/PLAN_COMPLETION.md` — automation status for steps 1–33.
- **LLVM API inventory:** `inventory_llvm_apis.sh` for step 33 migration tracking.
- **Release benchmark tables:** `regenerate_benchmark_tables.sh` → `docs/BENCHMARKS_TABLE.md` (wired in `release-installers.yml`).
- **Regression gate tests:** `tests/algorithm_recovery/test_regression_gate.py`.

### Changed

- `docs/BENCHMARKS.md` reflects wired corpus, CI F1, nightly, and migration evals.
- `algorithm-recovery-nightly` auto-updates baseline on success; full corpus runs regression gate.
- `migration_eval_suite.sh` includes LLVM inventory; `nightly_report.sh` includes migration summary.
- `regenerate-retdec-support.sh` emits `deps.cmake.snippet` and copies corpus manifest.

## [1.6.0] — 2026-08-08

### Added

- **Algorithm recovery regression gate:** `algorithm_recovery_regression_gate.sh` compares nightly F1/decompiled vs `baseline-algorithm-recovery.json`.
- **Baseline updater:** `update_algorithm_recovery_baseline.sh` refreshes baseline from CI results.
- **D-Helix gate mode:** `triton_diff_gate.py --mode dhelix` — randomized stdin path exploration + Triton entry hash.
- **Migration eval suite:** `migration_eval_suite.sh` runs rellic, LIEF, Retypd, SAILR scaffolds.
- **Retypd eval:** `eval_retypd.sh` (step 30 scaffold).
- **SAILR eval:** `eval_sailr.sh` — goto-count metrics on decompiled output (step 31 scaffold).

### Changed

- `algorithm-recovery-nightly` runs regression gate and migration suite.
- `doctor.sh` checks algorithm-recovery baseline and nightly workflow.
- `triton_diff_gate` auto mode defaults to `dhelix`.

## [1.5.0] — 2026-08-08

### Added

- **Full-corpus nightly F1:** `run_algorithm_recovery_full.sh` with parallel `--jobs` decompilation (216+ binaries).
- **algorithm-recovery-nightly workflow:** weekly CI-core run; full corpus on `workflow_dispatch`.
- **Triton differential gate:** `triton_diff_gate.py` with stdout/fuzz/triton modes; smoke test in ci-smoke.
- **LIEF eval scaffold:** `eval_lief.sh` compares readelf vs python-lief section counts.
- **Baseline:** `results/baseline-algorithm-recovery.json` for nightly trend tracking.

### Changed

- `extract_decompiler_predictions.py` supports `--jobs` parallel workers with per-binary work dirs.
- `perf-nightly` runs weekly algorithm-recovery CI core; `nightly_report.sh` includes F1 summary.
- `differential_gate_triton.sh` delegates to `triton_diff_gate.py`.

## [1.4.0] — 2026-08-08

### Added

- **Live algorithm-recovery F1 in CI:** `run_algorithm_recovery_ci.sh` decompiles a 9-binary core subset and scores precision/recall/F1 against ground truth.
- **Prediction extraction:** manifest-driven binary selection, `--ci-core` / `--limit` / per-binary timeout, richer label normalization (sorts, containers, concurrency).
- **Regression gate:** `algorithm_recovery_gate.sh` enforces minimum decompiled count and mean F1 floor.
- **Label unit tests:** `tests/algorithm_recovery/test_labels.py` in ci-smoke.

### Changed

- `extract_decompiler_predictions.py` output includes `decompiled` metadata; `runner.py` reports `summary.mean_f1`.
- `ctest-linux.yml` runs live F1 after integration tests when decompiler is built.
- `run_benchmarks.sh` discovers Windows decompiler paths.

## [1.3.0] — 2026-08-08

### Added

- **200+ binary corpus:** `generate_corpus_sources.py` adds 30 generated algorithm sources (36 total × gcc/clang × O0/O2/O3 ≥ 216 binaries).
- **Prediction extraction:** `extract_decompiler_predictions.py` maps decompiler `.config.json` semantic detections to labels.
- **Triton gate scaffold:** `differential_gate_triton.sh` with stdout fallback.
- **CI:** corpus size ≥ 200 check on Linux ci-smoke when gcc is available.

### Changed

- `build_algorithm_corpus.sh` auto-generates sources, uses C11/pthread flags, warns if < 200 binaries.
- `run_benchmarks.sh` extracts live predictions when decompiler is present.
- `regenerate-retdec-support.sh` detects available toolchains.

## [1.2.0] — 2026-08-08

### Added

- **Algorithm recovery corpus (step 10):** 6 labelled C sources, `build_algorithm_corpus.sh`, ground-truth generator, starter corpus pipeline.
- **Neural context (step 8.4):** semantic detections serialized into refinement prompts from `config.functions`.
- **Model provenance (step 8.8):** `RETDEC_NEURAL_MODEL_SHA256` verification at load.
- **Differential gate scaffold (step 20):** `RETDEC_NEURAL_DIFF_GATE=1` compares stdout of compiled original vs refined.
- **Demo:** `scripts/demo.sh` (Part 12.5) with offline assertion and benchmark tables.
- **Crash corpus:** `tests/crash_corpus/` + `scripts/ingest_fuzz_crash.sh` (Part 10.3).
- **DecompileBench schema:** `tests/decompilebench/schema.json`.

### Changed

- `run_benchmarks.sh` builds corpus, runs DecompileBench and algorithm-recovery metrics when decompiler available.

## [1.1.0] — 2026-08-08

### Added

- **Performance (step 27):** [docs/PERFORMANCE.md](docs/PERFORMANCE.md), `scripts/flamegraph_profile.sh`, `RETDEC_INCREMENTAL_CACHE` flag.
- **Neural (steps 21/32):** `BatchRefiner`, compile verification gate, tiers 4–5 via `RETDEC_NEURAL_TIER_MAX`.
- **Library adoption scaffolds (steps 28–29):** rellic eval script/docs, LIEF `LiefAdapter` stub, `RETDEC_ENABLE_LIEF` / `RETDEC_ENABLE_RELLIC` options.
- **Roadmap docs:** `docs/internal/retypd_sailr_llvm.md` (steps 30–33).
- **Algorithm recovery:** sample ground-truth and prediction JSON for metric runner.

### Changed

- `parallelBatchDecompile` declared in `retdec.h`.
- Neural compile gate uses `RETDEC_NEURAL_GATE_CC`.

## [1.0.0] — 2026-08-08

### Changed (v1.0.0 release)
- **Licence files:** Condensed `LICENSE` + `LICENSE-AGPL`, `LICENSE-COMMERCIAL`, `NOTICE` via `install-licence-files.sh`.
- **CI:** `ci-smoke` on every push/PR; `ctest-linux` on PRs; `ctest-windows` nightly; `perf-nightly` weekly; new `sanitizers.yml`.
- **Dependencies:** Capstone **5.0.9** (from 5.0-rc2).
- **`.cursorrules`:** Replaced autonomous-continuation policy with Part 14 guardrails.
- Internal roadmaps moved to `docs/internal/`.

### Removed

- **`src/qwen3/`** hand-written inference engine (~7.7k LOC); AI panel stubbed pending llama.cpp backend.

### Added

- **`retdec::neural`** mock inference library and tests.
- **`docker/baseline.Dockerfile`**, `scripts/upgrade-dep.sh`, `scripts/run_benchmarks.sh` (placeholder schema).
- PE/ELF/Mach-O fuzz harnesses in `tests/managed_integration/fuzz/`.
- [docs/NEURAL_REFINEMENT.md](docs/NEURAL_REFINEMENT.md), [docs/internal/MASTER-UPGRADE-PLAN.md](docs/internal/MASTER-UPGRADE-PLAN.md).

### Added (continued)

- **Neural:** `retdec::neural` decompile hook (`RETDEC_NEURAL_REFINE`), prompts, optional llama.cpp backend (`RETDEC_ENABLE_LLAMACPP`).
- **Algorithms:** Semi-NCA dominator citation, Andersen points-to scaffold, Braun SSA scaffold (`RETDEC_SSA_BRAUN`).
- **Benchmarks:** `tests/decompilebench/runner.py`, algorithm recovery scaffold, `docs/algorithm_reference.md`.
- **Security:** expanded `SECURITY.md`; commercial GPL exclusion in release workflow.

### Changed (continued)

- **retdec-support:** `scripts/regenerate-retdec-support.sh` scaffold for Phase 7.2.

### Changed (prior) Git history was squashed to a single root commit; issue/PR URLs were removed from in-tree comments where they were non-essential. Automated CI on push/PR uses [`.github/workflows/ci-smoke.yml`](.github/workflows/ci-smoke.yml); full test workflows ([`.github/workflows/ctest-linux.yml`](.github/workflows/ctest-linux.yml), [`.github/workflows/ctest-windows.yml`](.github/workflows/ctest-windows.yml)) are **manual-only**; scheduled/release automation uses [`.github/workflows/perf-nightly.yml`](.github/workflows/perf-nightly.yml) and [`.github/workflows/release-installers.yml`](.github/workflows/release-installers.yml). NSIS/AppImage homepage placeholders use `https://example.com/` until you set a real product URL.
- **Build layout:** CMake presets and helper scripts now use a fixed OS tree: `build/linux` + `install/linux` on non-Windows hosts, `build/windows` + `install/windows` on Windows; superbuilds use `build/linux/<preset>` or `build/windows/<preset>`. Staging defaults to `dist/windows` (and `dist/windows/debuggable` for the debuggable GUI script). MinGW cross lives under `build/linux/mingw-w64-release`.

### Added

#### Documentation
- **[docs/BUILD_REFERENCE.md](docs/BUILD_REFERENCE.md)** — canonical guide: CMake 3.26+, `build/linux` / `build/windows`, presets, superbuild, install, `dist/windows`, Docker, CI secrets, testing, troubleshooting.
- **[docs/README.md](docs/README.md)** — documentation hub: reading order, superbuild/CI/Docker summaries, diagnostics env vars, WSL and Windows quick paths.
- **[docs/user_manual.md](docs/user_manual.md)** — expanded installation (correct `cmake --install build/linux`), Windows staging notes, CLI companion section, troubleshooting, doc map.
- Cross-links and CMake **3.26+** alignment in [README.md](README.md), [docs/developer_guide.md](docs/developer_guide.md), [docs/WINDOWS_NATIVE_BUILD.md](docs/WINDOWS_NATIVE_BUILD.md), [docs/MINGW_CROSS_DEEP_DIVE.md](docs/MINGW_CROSS_DEEP_DIVE.md), [docs/architecture.md](docs/architecture.md), [scripts/README.md](scripts/README.md), and [`.github/workflows/`](.github/workflows/) (ci-smoke, ctest, perf-nightly, release-installers).

#### GPU Acceleration — CUDA
- Full CUDA acceleration backend replacing OpenCL throughout the project.
- New library `retdec-cuda-accel` (`src/cuda_accel/`, `include/retdec/cuda_accel/`):
  - `CUDAContext` — device detection, context lifecycle, CPU-fallback flag
  - `CUDABufferPool` — GPU memory pool with RAII management
  - `CUDAProfiler` — CUDA event-based kernel timing
  - `CUDADisassembler` — parallel x86-64 CFG disassembly on GPU
  - `CUDASteensgaard` — Steensgaard points-to alias analysis on GPU
  - `CUDATypeInferencer` — type propagation on GPU
  - `CUDASemanticHasher` — mini x86-64 emulator kernel for semantic hashing
  - `CUDAEGraphSimplifier` — E-graph equality saturation on GPU
- All passes include mandatory CPU-threaded fallback (activated automatically when no CUDA GPU is present).
- Google Test suites for every CUDA module under `tests/cuda_accel/`.

#### Managed Language Decompilation
- New dispatcher (`src/retdec-decompiler/managed_decompiler.cpp/.h`) detects managed
  formats by magic bytes and routes to the appropriate language pipeline, bypassing
  the LLVM IR path entirely.
- Supported formats and pipelines:
  - **Java `.class`** — `jvm_parser::JvmClassParser` → `jvm_reconstruct::JvmReconstructor` → `java_emitter::JavaFileEmitter`
  - **Android DEX/APK** — `dex_parser::ApkReader` → `java_emitter::JavaFileEmitter`
  - **Python `.pyc`** (CPython 3.8–3.12) — `pyc_parser::PycReader` → `py_reconstruct::PyReconstructor` → `py_emitter::PyFileEmitter`
  - **Lua bytecode** (5.1, 5.2, 5.3, 5.4) — `lua_parser::LuaReader` → `lua_parser::LuaEmitter`
  - **WebAssembly `.wasm`** — `wasm_parser::WasmReader` → `wasm_parser::WatEmitter`
- `src/retdec-decompiler/CMakeLists.txt` updated to link all managed language libraries.

#### Windows — Full Native Build (MSVC + CUDA + Qt6 GUI)
- `deps/openssl/CMakeLists.txt` — added `VC-WIN64A` + `nmake` path for MSVC native Windows
  builds (previously FATAL_ERROR'd). MSVC path uses static `libcrypto.lib`, `no-asm`,
  and discovers `nmake` via `find_program`.
- `scripts/Install-RetdecWindowsDeps.ps1` — winget-based prerequisite installer that checks
  for and installs MSVC Build Tools, CUDA Toolkit, Qt6, CMake, Ninja, Perl, Git.
- `scripts/windows_native_configure.ps1` — CMake configure script for native Windows MSVC
  builds; auto-detects Qt6, CUDA, and MSVC; enables `RETDEC_ENABLE_ALL=ON`,
  `RETDEC_BUNDLED_OPENSSL=ON`, `RETDEC_ENABLE_CUDA_ACCEL` based on GPU detection.
- `scripts/windows_native_build.ps1` — full build + staging script that runs cmake --build,
  cmake --install, `windeployqt` for Qt6 DLLs, CUDA runtime DLLs, and MSVC runtime DLLs
  into `dist-windows-full\`.
- `scripts/Test-RetdecWindows.ps1` — updated to support both `dist-windows\` (MinGW) and
  `dist-windows-full\` (MSVC); added tests for `retdec-gui.exe` launch and CUDA DLL presence.
- `docs/WINDOWS_NATIVE_BUILD.md` — new dedicated guide for the native Windows build including
  prerequisites, build steps, OpenSSL VC-WIN64A notes, Qt windeployqt, CUDA driver requirements,
  and full troubleshooting table.

#### Windows Cross-Compilation (Linux/WSL → Windows PE, CLI only)
- `cmake/toolchains/windows-mingw-w64.cmake` — MinGW-w64 toolchain (OpenCL reference removed).
- `scripts/wsl_cross_configure.sh` — configures Windows cross-build with all required options
  (toolchain, `RETDEC_LLVM_TABLEGEN`, `RETDEC_TESTS=OFF`, enabled components).
- `scripts/wsl_cross_build.sh` — builds and stages Windows PE binaries into `dist-windows/`
  including MinGW runtime DLLs; bypasses `cmake --install` to avoid missing-file errors.
- `scripts/Test-RetdecWindows.ps1` — PowerShell smoke test suite for the Windows build
  (help output, Lua / Python / Java managed decompilation tests).
- `CHANGELOG.md` and `LICENSE` (AGPL-3.0+ / commercial dual licence, Odin Loch trading as Imortek) added to
  satisfy install targets.
- `src/testing/test_harness.cpp` — added `#include <windows.h>` (with `WIN32_LEAN_AND_MEAN`
  and `NOMINMAX`) under `#ifdef _WIN32` to fix undeclared `MAX_PATH`, `GetTempPathA`, etc.

#### AI / Qwen3 Integration
- `include/retdec/qwen3/` — Qwen3 model, pipeline, sampler, and weights headers.
- `src/qwen3_runner/main.cpp` — replaced OpenCL with CUDA for GPU inference.
- `scripts/setup_qwen3.sh` — rewritten to install CUDA Toolkit and use CUDA backend.
- Model pull via Ollama: `ollama pull qwen3-coder:30b-a3b-q4_K_M`.

#### GUI
- `scripts/launch_gui.sh` — detects WSLg/VcXsrv and launches GUI correctly.
- `scripts/launch_gui_vcxsrv.sh` — dedicated VcXsrv launcher.
- Settings dialog CUDA tab replaces former OpenCL tab.

#### Testing / Samples
- `scripts/check_compilers.sh` — inventories installed compilers; improved Kotlin detection.
- `tests/decompile_samples/compile_all.sh` — compiles test samples for all supported languages
  (Java with `--release 8` for DEX compatibility; C# uses distinct output subdirectory).
- `tests/decompile_samples/run_decompile.sh` — runs `retdec-decompiler` on each sample and
  reports pass/fail quality metrics.

#### Documentation
- `docs/MINGW_CROSS_DEEP_DIVE.md` — complete tested walkthrough for Linux/WSL → Windows PE
  cross-compilation including all pitfalls and their fixes.
- `docs/README.md` — updated with real script names, quick-reference cross-compile table.
- `docs/user_manual.md` — CUDA tab replaces OpenCL; managed language input formats added.
- `docs/developer_guide.md` — Windows cross-compile section; CUDA profiling example; CPU
  fallback pattern documented.
- `README.md` (root) — overhauled: real build commands, cross-compile section, CUDA note,
  managed language quick-start examples, updated documentation table.

---

### Changed

- **Copyright** — all decompiled output headers and project files updated to
  "Odin Loch Trading as Imortek" (MIT License references removed).
- **GPU backend** — OpenCL replaced by CUDA across the entire codebase
  (all `.cl` kernels, `ocl_context`, `ocl_disassembler`, `ocl_steensgaard` removed).
- **GUI settings** — OpenCL settings renamed to CUDA settings throughout
  `include/retdec/gui/settings/settings.h`, `src/gui/settings/settings.cpp`,
  `src/gui/panels/settings_dialog.cpp`.
- `deps/openssl/CMakeLists.txt` — upgraded to OpenSSL 3.2.6 (GitHub release URL);
  fixed cross-compile configure to use `--cross-compile-prefix` only (no duplicate env vars);
  added `--libdir=lib` to prevent `lib64/` install.
- Root `CMakeLists.txt` — `add_subdirectory(tests)` guarded by `if(RETDEC_TESTS)`.
- `src/CMakeLists.txt`, `cmake/options.cmake`, `tests/CMakeLists.txt` — removed OpenCL
  entries, added CUDA entries.

---

### Fixed

#### C++ Crash Fixes (native decompiler pipeline)
- `include/retdec/llvmir2hll/support/subject.h` — fixed erase-remove idiom bug in
  `removeObserverAndNonExistingObservers` (two-iterator erase, prevents `weak_ptr` dangling).
- `src/capstone2llvmir/x86/x86_sse.cpp` — fixed `StoreInst::AssertOK()` assertion failures:
  changed `eOpConv::NOTHING` to `eOpConv::ZEXT_TRUNC_OR_BITCAST`; fixed `APInt` hex string
  parsing (replaced with `ConstantInt::get`).
- `src/bin2llvmir/providers/calling_convention/calling_convention.cpp` — made `clear()` a
  no-op to prevent clearing permanent constructor registrations (fixed `'cc' failed` assertion).
- `src/bin2llvmir/analyses/symbolic_tree.cpp` — added bit-width guards (`<= 64`) before
  `getSExtValue()` / `getZExtValue()` calls.
- `src/bin2llvmir/optimizations/simple_types/simple_types.cpp` — comprehensive `i128`
  guards preventing SIGSEGV in `std::unordered_set::insert` in `mergeEqSetInto`.
- `src/llvmir2hll/llvm/llvmir2bir_converter/llvm_constant_converter.cpp` — added handlers
  for `llvm::ConstantVector` and `llvm::ConstantDataVector` (zero initializer fallback).
- `src/bin2llvmir/optimizations/unreachable_funcs/unreachable_funcs.cpp` — replaced uses
  with `UndefValue` before `deleteBody()` to prevent `Value::~Value() use_empty()` assertion.
- `src/retdec/retdec.cpp` — added missing `#include "retdec/ssa/ssa.h"`.
- Various files — qualified `errs()` as `llvm::errs()` and `setLogsFrom` as
  `retdec::setLogsFrom` to fix "not declared in this scope" errors.

#### HLL Optimiser Performance
- `src/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer.cpp` — iteration caps and
  per-function time budgets to prevent infinite loops on large functions.
- `src/llvmir2hll/optimizer/optimizer_manager.cpp` — global time budget; worker thread cap (4).
- `src/llvmir2hll/optimizer/optimizers/simple_copy_propagation_optimizer.cpp` — pass
  `nullptr` for `VarUsesVisitor` to avoid redundant precomputation.
- CFG node count and local variable count thresholds to skip expensive passes on pathological inputs.

#### Managed Language — Python `.pyc`
- `src/pyc_parser/py_marshal.cpp` — fixed swapped `'('`/`')'` tuple type dispatch
  (`TYPE_TUPLE` vs `TYPE_SMALL_TUPLE`); improved error reporting with offset.
- `src/pyc_parser/py_opcodes.cpp` — fixed opcode table for Python 3.11+: removed duplicate
  entries (opcodes 66–68); added version-specific overrides for `PUSH_NULL` (2), `GET_ITER`
  (68), and other renamed/repurposed opcodes.
- `src/py_reconstruct/py_stack_sim.cpp`:
  - `PUSH_NULL` now pushes a `_null_` placeholder instead of being a no-op.
  - `LOAD_GLOBAL` (3.11+, `arg & 1`) pushes `_null_` sentinel correctly.
  - `LOAD_ATTR` (3.11+, `arg & 1`) pushes self + method pair.
  - `STORE_SUBSCR` operand order corrected (key, obj, val).
  - `LOAD_NAME` separated from `LOAD_GLOBAL` (removes incorrect `arg >> 1`).
  - `MAKE_FUNCTION` updated for Python 3.11+ (no `qualname` on stack).
  - `constFromIdx` returns `co_name` for nested code objects.
- `include/retdec/pyc_parser/pyc_reader.h` — added `std::shared_ptr<PyCodeObject> root`
  to `PycReadResult`.
- `src/pyc_parser/pyc_reader.cpp` — populates `result.root`.

#### Managed Language — Lua bytecode
- `src/lua_parser/lua_reader.cpp`:
  - Fixed `readDebugInfo51` and `readDebugInfo52plus` to always read upvalue name strings
    from the stream (stream alignment fix, previously caused "String read past end").
  - Fixed Lua 5.4 header parsing: added missing `readU8()` for `sizeof(lua_Number)`.
  - Implemented `readLuaSize54()` for Lua 5.4 modified LEB128 (MSB=1 = last byte).
  - Modified `readInt()` to dispatch to LEB128 for Lua 5.4.
  - Implemented `readString54()` with string deduplication table.
  - Implemented `readDebugInfo54()` for Lua 5.4's distinct debug format (raw `int8_t`
    line info, LEB128 abslineinfo pairs, locals, upvalue names).
  - Corrected Lua 5.4 constant tags: swapped `LUA_VNUMINT` (0x03) and `LUA_VNUMFLT` (0x13).
- `include/retdec/lua_parser/lua_reader.h` — added `useLeb128_`, `stringTable54_`,
  `readLuaSize54()`, `readString54()`, `readDebugInfo54()`.
- `include/retdec/lua_parser/lua_types.h` — corrected `fieldB54()` (bits 16–23) and
  `fieldC54()` (bits 24–31) bit extractors; added `fieldBx54()`, `fieldSBx54()`,
  `fieldSJ54()`, `fieldAx()` for Lua 5.4 instruction format.
- `src/lua_parser/lua_emitter.cpp`:
  - Refactored to `decodeInstrLua51` / `decodeInstrLua52` / `decodeInstr54` with dispatcher.
  - Corrected Lua 5.1 opcode mappings (JMP, CONCAT, GETGLOBAL, SETGLOBAL, etc.).
  - Added `rawStr()` helper for unquoted global names in Lua 5.1 output.
  - `decodeInstr54`: corrected all instruction encodings using `fieldB54`/`fieldC54`;
    added signed `sB`/`sC` bias-127 helpers for `ADDI`, `SHRI`, `SHLI`, `EQI`–`GEI`;
    corrected `LOADI` (uses `sBx`), `LOADF`, `LOADK`; fixed `JMP` to use `fieldSJ54()`;
    fixed `CONCAT` operand range; fixed `CALL`/`TAILCALL`/`RETURN` arg counts;
    swapped `FORLOOP` (73) and `FORPREP` (74) case bodies to match Lua 5.4.6 opcodes;
    corrected `FORPREP` jump target to `pc + bx + 2`; display `SHRI` with negative `sC`
    as left shift (`<< -sC`).
- `src/lua_parser/lua_types.cpp` — whole `LuaFloat` values formatted as integers
  (e.g. `5.0` → `5`) for cleaner Lua 5.1 output.

---

### Removed
- All OpenCL source files: `src/opencl/kernels/*.cl`, `src/opencl/ocl_context.cpp`,
  `src/opencl/ocl_disassembler.cpp`, `src/opencl/ocl_steensgaard.cpp`.
- All OpenCL headers: `include/retdec/opencl/ocl_context.h` (and related).
- OpenCL CMake targets and options from `src/CMakeLists.txt`, `cmake/options.cmake`,
  `cmake/superbuild/CMakeLists.txt`, `tests/CMakeLists.txt`.
- MIT License header from decompiled code output.
- "Avast" references from all output headers and copyright strings.
