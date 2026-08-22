# LLVM migration scope (D1)

Investigation only. **Do not bump LLVM** and **do not edit `deps/llvm/`**.

## Pin (current)

From [`cmake/deps.cmake`](../../cmake/deps.cmake):

| Variable | Value |
|----------|--------|
| `LLVM_URL` | `https://github.com/avast/llvm/archive/a776c2a976ef64d9cd84d7ee71d0e4a04aa117a1.zip` |
| Commit | `a776c2a976ef64d9cd84d7ee71d0e4a04aa117a1` |
| `LLVM_ARCHIVE_SHA256` | `b5879b30768135e5fce84ccd8be356d2c55c940ab32ceb22d278b228e88c4c60` |

This is the **Avast LLVM 8-era fork**, not upstream LLVM 8 or a current LTS.

## Policy

Repository-root [`.cursorrules`](../../.cursorrules) forbids an LLVM version bump in this repo until a migration is **explicitly scoped**. Composer / agent runs must not touch `deps/llvm/` or change the URL / SHA-256 pair above as a “drive-by” upgrade.

See also [MASTER-UPGRADE-PLAN.md](MASTER-UPGRADE-PLAN.md) Part 7.3 and [retypd_sailr_llvm.md](retypd_sailr_llvm.md) step 33.

## What a migration would need (research notes)

These are notes for a future, separately scoped project. None of this is scheduled here.

### New Pass Manager

The decompiler pipeline still uses the **legacy** pass manager:

- `llvm::legacy::PassManager` in `src/retdec/retdec.cpp` (lift, opt, and `decompileToLlvmIr`)
- `addPass(legacy::PassManagerBase& …)` next to the same file
- Named pass lists in `src/retdec-decompiler/decompiler-config.json` and `src/retdec-decompiler/profiles/*.json`

Modern LLVM defaults to the **new pass manager**. A bump would need a rewrite of pass registration, the profile JSON → pass construction path, and any `bin2llvmir` / `llvmir2hll` passes that assume `legacy::PassManager`.

### Opaque pointers (LLVM 15+, mandatory by LLVM 17)

From LLVM 17, **opaque pointers** remove pointee types from `ptr`. For a compiler that is a simplification. For this decompiler it deletes information the type-recovery and load/store passes still read from IR.

In-tree uses that a migration must inventory (non-exhaustive):

- `Type::getPointerElementType()` — heavy in `src/bin2llvmir/optimizations/simple_types/simple_types.cpp` and `inst_opt.cpp`
- `PointerType::get(elem, as)` constructions in `value_protect.cpp` and elsewhere
- Implicit-typed `CreateLoad` / `CreateGEP` call sites that LLVM 15+ reject

MASTER-UPGRADE-PLAN Part 7.3: migrating *before* a use-based type system (Retypd or equivalent) would plausibly make recovered C **worse**. Suggested sequence remains **Retypd (or equivalent) first, LLVM second**.

### Other migration work

- Inventory every `llvm::` use (~314 files per `retypd_sailr_llvm.md`); do not guess APIs
- One pass at a time behind a hypothetical `RETDEC_LLVM_NEXT` flag (not present today — do not add it in this pass)
- Keep `cmake/deps.cmake` as the single pin; one dependency bump per branch
- Full `ctest` green on the LLVM 8 baseline **before** any fork move
- rellic / anvill alignment is a *target* of a future bump, not a reason to start one now

## Out of scope for this document

- No change to `cmake/deps.cmake`
- No edit under `deps/llvm/`
- No `RETDEC_LLVM_NEXT` flag
- No attempt to compile against upstream LLVM
