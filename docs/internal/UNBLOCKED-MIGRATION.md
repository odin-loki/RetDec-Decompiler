# Unblocked migration (2026-08-23)

User-scoped: unlock the Wave 5 hard blocks, move Clang/LLVM to latest
upstream, and **port LLVM 8 typed-pointer facts into RetDec-owned
metadata** so opaque pointers do not delete type recovery.

This file is the working plan. Inventories are from in-repo search
(`src/`, `include/`, `tests/` — not `deps/llvm/`). Do not invent APIs.

## Non-negotiables

- Never edit files under `deps/llvm/`. Pin changes are URL + SHA-256
  in `cmake/deps.cmake` only. One pin per commit.
- Do not bump LLVM until `retdec.pointee` is the source of truth for
  load/store/type recovery (MD first, typed-pointer fallback second).
- One algorithm / one family per commit. Public `IrInstr::Op` changes
  are explicitly scoped here and still get their own commit.
- Never delete, SKIP, or loosen a test. Official `MIN_MEAN_F1=0.95`
  stays. Name-blind product F1 stays 0.056 / ci-core 0.126.
- Default F5 decompile path stays unchanged unless the user opts in.
- Build after every C++ edit.

## Current LLVM pin (do not change in Track 1)

| Field | Value |
|-------|--------|
| URL | `https://github.com/avast/llvm/archive/a776c2a976ef64d9cd84d7ee71d0e4a04aa117a1.zip` |
| Commit | `a776c2a976ef64d9cd84d7ee71d0e4a04aa117a1` |
| SHA-256 | `b5879b30768135e5fce84ccd8be356d2c55c940ab32ceb22d278b228e88c4c60` |

Avast LLVM 8-era fork, not upstream 8. Clang is **not** a separate pin;
it lives in that monorepo. Host `clang-18` in toolchains is the
**compiler that builds RetDec**, not the IR library.

`llvm::` appears in **373** files (367 excluding docs). Older note
said 314. There is **no** `RETDEC_LLVM_NEXT` flag. Do not add one as
a drive-by.

Pass manager is **legacy only**: `llvm::legacy::PassManager` in
`src/retdec/retdec.cpp` (disassemble / decompile / decompileToLlvmIr),
`RegisterPass<>` (~40 sites), JSON names in
`src/retdec-decompiler/decompiler-config.json` and `profiles/*.json`.
Zero `PassBuilder` / new PM usage.

Target pin after MD lands: **latest upstream LLVM/Clang (~22.x)**,
one URL/SHA. Not LLVM 18.

---

## Track 1 — Port pointer metadata (LLVM 8, first)

Opaque pointers (mandatory LLVM 17) delete `T*` pointee types.
RetDec still reads them. Port = copy those facts into IR metadata
**while LLVM 8 still has them**.

### Existing hook (do not invent a new public MD API)

Same pattern as `insn.addr` / `x87.depth`:

| Kind | Set | Read |
|------|-----|------|
| `insn.addr` | `asm_inst_remover.cpp`, `phi_remover.cpp` | `jump_table_recovery.cpp`, `LLVMSupport::getInstAddress` |
| `x87.depth` | `x87_fpu_ext.cpp` | write-only today |
| `retdec.addr` | never set | orphan reader in `llvm_to_ssa.cpp` |
| **`retdec.pointee`** | `llvm_utils::setPointeeTypeMetadata` | `getPointeeTypeMetadata` / `pointeeType` |

Helpers live in existing `include/retdec/bin2llvmir/utils/llvm.h`
(type-utils module). Payload is an `MDString` of the LLVM type print
(`llvmObjToString`), parsed back with existing `stringToLlvmType`.

Named globals/stack already store types via
`common::Object::type.setLlvmIr` (`object.h`). That does **not**
cover arbitrary SSA pointer values. `IrModifier::getGlobalVariable(...,
preferredPointeeType)` covers globals only.

No TBAA / `!deref` attachment exists in RetDec IR today.

### Inventory that must eventually read MD first

| Category | Count | Hottest files |
|----------|-------|----------------|
| `getPointerElementType` | 25 | `inst_opt.cpp`, `simple_types.cpp`, `ir_modifier.cpp`, `entry_alloca.cpp`, `abi.cpp` |
| `PointerType::get(` | 78 | `capstone2llvmir` (x86 21), `param_return`, `llvm.cpp` |
| Pointer `getElementType` | ~20 | `llvmir2hll` converters, `param_return`, `config.cpp` |
| Implicit `CreateLoad` | 46 | all `capstone2llvmir` backends |
| Implicit `CreateStore` | 44 | same |
| Typed `CreateLoad(type, ptr)` | 1 | `struct_recovery.cpp:371` (already opaque-ready) |
| GEP create | 2 | `struct_recovery` |
| `PointerType::isValidElementType` | 4 | `ctypes2llvm`, `ir_modifier`, `llvm.cpp` |

Lifter pattern (typed-pointer dependent):

```
PointerType::get(lty, 0) → CreateIntToPtr → CreateLoad(addr)
```

`struct_recovery` is the only bin2llvmir pass already using
`CreateLoad(type, ptr)`.

### Track 1 commit slices (do not flatten)

1. Helpers + unit test (`set`/`get`/`pointeeType` fallback). **This slice.**
2. Writers: attach MD at every `IntToPtr` + implicit load/store in
   `capstone2llvmir` (one arch per commit if the diff is large).
3. Readers: `getPointerElementType` sites call `pointeeType` first
   (`simple_types`, `inst_opt`, `ir_modifier`, `entry_alloca`, `abi`).
4. `llvmir2hll` type/value converters.
5. Only then consider the LLVM URL bump.

---

## Track 2 — LLVM 8 → ~22 (after Track 1)

1. Full `ctest` green on the LLVM 8 pin with MD readers in place.
2. Inventory breaking APIs (`scripts/inventory_llvm_apis.sh`); do not
   guess symbols.
3. New pass manager: replace `RegisterPass` + `legacy::PassManager` +
   JSON name lookup. Keep profile JSON pass **names** stable if possible.
4. Typed `CreateLoad`/`CreateGEP`/`CreateStore` → explicit type operands
   (LLVM 15+). Lifters already have `lty` in hand.
5. One `cmake/deps.cmake` URL/SHA to upstream LLVM ~22 (with Clang).
6. Host toolchain Clang can move with the library pin; keep
   `clang-format-18` until a format-version commit is scoped.

Retypd (Part 9) still helps use-based recovery. It is **not** a
prerequisite for starting Track 1.

---

## Track 3 — Other hard blocks (parallel streams)

Each stream is its own commit family. Public headers only when listed.

### 3a SSA opcodes (`include/retdec/ssa/ssa.h` 214–224)

Current: `Assign, Add, Sub, Mul, Div, And, Or, Xor, Not, Neg, Shl,
Shr, Sar, Ror, Rol, Load, Store, Call, Ret, Branch, CondBranch,
Compare, FlagWrite, FlagRead, Phi, Undef`.

- `SRem`/`URem`/`FRem` map to `Op::Rem`; `AtomicRMW`/`CmpXchg`/`Fence`
  and atomic load/store map to `Op::Lock`.
- A8: lock-prefixed x86 ADD/XADD/AND/OR/XOR/`inc`/`dec`/`not` emit
  `atomicrmw`; `lock cmpxchg` mem emits `cmpxchg`. ARM64
  `ldxr`/`ldaxr`/`ldar` are atomic loads; `stxr`/`stlxr` are atomic
  stores (status 0, no exclusive monitor); `stlr` is release.
  `extractAtomics` accepts `Op::Lock`. Implicit `xchg mem` emits
  `atomicrmw xchg` (register–register xchg stays a plain swap).
  `lock cmpxchg8b`/`cmpxchg16b` emit `cmpxchg`; `stxp`/`stlxp` are
  wide atomic stores. `ldxp`/`ldaxp` pair loads are atomic
  (monotonic / acquire); ordinary `ldp` stays non-atomic.
  x86 `lfence`/`sfence`/`mfence` and ARM/ARM64 `dmb`/`dsb`/`isb`
  emit LLVM `fence` (`isb` is seq_cst; no I-cache model).
  ARM `ldrex*` loads are atomic; `strex`/`strexb`/`strexh`/`strexd`
  are atomic stores plus status 0. `ldaexd` is an acquire pair load;
  `stlexd` is a release pair store.
  ARM `swp`/`swpb` emit `atomicrmw xchg` (seq_cst).
  ARM `lda*`/`ldaex*` are acquire loads; `stl*` are release
  stores; `stlex*` are release exclusive stores plus status 0.
  `ldaexd`/`stlexd` are packed `i64` acquire-load / release-store.
  `lock bts`/`btr`/`btc` mem emit `atomicrmw` or/and/xor.
  MIPS `ll`/`lld` are atomic loads; `sc`/`scd` are atomic stores
  plus status 1.
  PowerPC `lwarx`/`ldarx` are atomic loads; `stwcx`/`stdcx` are
  atomic stores plus `CR0.EQ`.
  `lock sub` mem emits `atomicrmw sub` (`cmp` is not locked).
  MIPS `sync`/`synci` emit LLVM `fence` (`synci` has no I-cache model).
  PowerPC `sync`/`isync`/`lwsync`/`eieio`/`mbar`/`msync`/`tlbsync`/
  `ptesync` emit LLVM `fence` (no I-cache / TLB model).

### 3b C parser → N10 → N18

`TREE_SITTER_C_URL` pins tree-sitter-c v0.24.2. `TREE_SITTER_URL` pins
tree-sitter v0.26.12 (ABI 15; MIN 13, so ABI 14 grammar is OK).
Fetched in `cmake/tree_sitter.cmake` (not under `deps/`). N10 walks
the C AST for the N5 counted set; parse failure falls back to the
keyword scan.

N18: callee-before-caller refine in `maybeRefineDecompilerOutput`
(topo order, address ties, `refined_callees` in the caller prompt).
`serializeSemanticContext` still dumps callers/callees from
`codeReferences`. Call-graph-only functions (no declaration /
crypto / detections) are included so those edges reach the prompt.

### 3c Neural leftovers

N16 peripheral dump in `serializeSemanticContext`: detection
`cHint`, pattern `endian`/`matches`, tool nibble counts.
Comments, locals, globals, descriptions, `cElemBytes`, and
`getAdditionalInfo()` stay omitted.

| Item | Status |
|------|--------|
| N17 mean selected-token `p` | Done (`inference.h` 39–42). Not per-identifier. |
| N17 per-id logprob | Blocked on llama.cpp token-level API at the pin. |
| N19 retrieval | Audit-only. No embedding corpus in `src/neural/`. |
| `RETDEC_NEURAL_BATCH` | Removed. `BatchRefiner` is sequential. Do not restore the env without `n_seq_max>1` decode. |

### 3d Architectures

- ARM64: lifter exists (`capstone2llvmir/arm64`, ~3k LOC) + partial
  `bin2llvmir` ABI/CC. Docs: not production e2e (SIMD, atomics,
  BTI/PAC, corpus).
- RISC-V: **no** lifter, no `-a riscv`.
- SPARC / SystemZ / XCore: `create*` stubs throw.

### 3e C ABI / Python

`docs/internal/C_ABI_SKETCH.md` is a sketch. No `libretdec` export.
P2 waits on P1.

### 3f A4 (can run now)

`scripts/ci/run_a4_calibration.py` + `results/a4-calibration.md`:
n=160, precision 0, `fitted=false`. Fitting = tune detector constants
on the B8 100-binary negative corpus. No new opcode.

### 3g Crypto leftovers (not Wave 5)

ECC / BLAKE2 / Poly1305 / Salsa20 remain enum-only. Do not stack more
SHA-256 K[] / MD5 T[] rows. Do not mix Castagnoli into IEEE CRC.

---

## Honesty

A 0.95 CI F1 pass is the **stem-era** gate, not product quality.
Name-blind full-216 mean F1 is **0.056**. Do not advertise 1.0.

N17 is mean selected-token probability for the whole generation.

---

## Done so far

- `.cursorrules` + D4 + Part 7.3 + Wave 5 point at this file.
- `retdec.pointee` helpers + `LlvmUtilsTests.PointeeMetadataRoundTrips`.
- Lifter `loadOp`/`storeOp` on x86/ARM/ARM64/MIPS/PowerPC write MD.
  Tests: `MemoryLoadAttachesPointeeMetadata` /
  `MemoryStoreAttachesPointeeMetadata` on those arches, plus x86
  store / atomic RMW / cmpxchg, ARM `swp`, ARM64 `ldxr`, MIPS `ll`,
  and PowerPC `lwarx`. Exclusive stores (`stlxr`/`stxp`/`strex`/`sc`/
  `stwcx`) and x86 `lock not`/`sub`/`bts` also have MD tests.
  Remaining A8 lift paths (`ldrex`/`lda*`/`stl*`/`swpb`, `ldxp`/
  `ldaxp`, `lock btr`/`btc`/`inc`/`cmpxchg8b`/`16b`) have MD tests.
  `STREXD`/`STLEXD` pack `Rt|(Rt2<<32)` like ARM64 `stxp`; `LDAEXD`
  is an acquire pair load. ARM64 `ldaxr`/`ldar`/`stxr`/`stlr`/`stlxp`/
  `ldaxrb`/`stxrb`/`stlrb`, MIPS `lld`/`scd`, PowerPC `ldarx`/`stdcx`,
  and ARM `ldrexb`/`ldrexh` also have MD tests. Remaining ARM
  byte/half acquire/exclusive (`ldab`/`ldah`/`ldaexb`/`ldaexh`/
  `strexb`/`strexh`/`stlb`/`stlh`/`stlexb`/`stlexh`), ARM `ldrexd`,
  ARM64 remaining widths (`ldxrb`/`ldxrh`/`ldaxrh`/`ldarb`/`ldarh`/
  `stxrh`/`stlrh`/`stlxrb`/`stlxrh`), and x86 `lock dec`/`or`/
  `xor`/`and`/`xadd` also have MD tests. x86 string lifts (`stos`/
  `lods`/`movs`/`scas`/`cmps`, including `rep stos`/`rep movs`
  inttoptr) also have MD tests, plus remaining string-op widths
  (`stosb`/`stosw`/`lodsb`/`lodsw`/`movsb`/`scasb`/`cmpsb`/`rep stosb`)
  and stack/table memops (`pop`/`xlatb`/`call`/`leave`/`enter`/`ret`/
  `lds`). Remaining x86 `lcall`/`pusha`/`popa`/`pushf`/`popf`/
  `fxsave`/`fxrstor` and ARM `push`/`pop`/`ldm`/`stm` also have MD
  tests. ARM64 `ldp`/`stp` and PowerPC indexed `lwzx`/`stwx` also
  have MD tests. Remaining ARM `ldrd`/`strd`, x86 memory `ljmp`/`retf`,
  and PowerPC `lhbrx` also have MD tests. Remaining wired fences (`synci`,
  `mbar`/`msync`/`tlbsync`/`ptesync`) have emit-fence tests.
- Value-based readers (`inst_opt`, `entry_alloca`, ABI, `ir_modifier`,
  `simple_types`) call `pointeeType` first. `pointeeType` also uses
  `AllocaInst::getAllocatedType` and `GlobalVariable::getValueType`.
  `IrModifier::convertValueToType` attaches `retdec.pointee` on
  instruction `inttoptr` and pointer `bitcast`/`addrspacecast`
  (not `ConstantExpr`). Remaining pointer-cast writers
  (`entry_alloca`, `value_protect`, `phi_remover`, `inst_opt`,
  `phi_to_select`, `struct_recovery`) do the same. Remaining
  pass `LoadInst`/`StoreInst` writers attach MD on the access
  (`inst_opt` bitcast load/store, `IrModifier` aggregate reload,
  `struct_recovery`, `entry_alloca`, `stack`, `value_protect`,
  decoder, syscalls, `cond_branch_opt`). `param_return` / x87
  register GV loads stay on the alloca/GV typed-pointer fallback.
  `hasFunctionTypeOrPointer` consults `pointeeType` on return/arg
  slots before the Type* function-pointer check.
  ParamReturn indirect-call snapshots include the function-pointer
  bitcast MD from `convertValueToType`. llvmir2hll
  `determineVariableType` reads `retdec.pointee` on pointer-typed
  instructions (loads keep their LLVM type; MD there is the loaded
  type). Inlined pointer casts (`convertCastInstToExpression`,
  including always-inlined `bitcast` / `addrspacecast` and one-use
  `inttoptr`) use the same MD for the destination type.
  `convert(PointerType*)` stays Type*-only.
- **No** `cmake/deps.cmake` LLVM URL change.
- `IrInstr::Op::Rem` / `Op::Lock` appended before `Undef`.
  `SRem`/`URem`/`FRem` → `Rem`; atomics/fence → `Lock`.
- Ring-buffer wrap: `Rem` + capacity immediate (not `Div`).
- A8 lock-prefix / `ldxr` / `stlxr` / `stxp`: lifter emits LLVM
  atomics; detector reads `Op::Lock`.
- llvmir2hll empty-string GV uses `getValueType`.
- tree-sitter-c v0.24.2 and tree-sitter v0.26.12 URL/SHA in
  `cmake/deps.cmake`. Fetched via `cmake/tree_sitter.cmake`. N10 AST
  shape check in `gates.cpp` (N5 keyword scan is fallback). N18
  callee-before-caller refine in `decompile_hook.cpp`.
- Remaining Type*-only readers (no Value to thread):
  `simple_types` 1203–1204 / 1641 nested pointer-to-array,
  llvmir2hll `convert(PointerType*)` (Type*-only signature),
  `fileimage` `getConstant(Type*)` char-pointer identity.
- **No** `cmake/deps.cmake` LLVM URL change.
