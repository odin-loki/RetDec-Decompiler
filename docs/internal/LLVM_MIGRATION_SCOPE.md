# LLVM migration scope (D1 / D4)

**Current as of v2.0.22: no bump unless explicitly tasked.**

The pin is already upstream `llvm-project` **23.1.0**. This file is the
policy for any *further* move. The Wave 5 plan that got here is historical:
[UNBLOCKED-MIGRATION.md](UNBLOCKED-MIGRATION.md).

## Pin (current — do not change until an explicitly scoped task says so)

From [`cmake/deps.cmake`](../../cmake/deps.cmake):

| Variable | Value |
|----------|--------|
| `LLVM_URL` | `https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.0/llvm-project-23.1.0.src.tar.xz` |
| Tag | `llvmorg-23.1.0` |
| `LLVM_ARCHIVE_SHA256` | `ab1f0e3ec52448c33e8782eaf0422504b87c7b016b22514653ee0d8fcee479ff` |

Upstream llvm-project (not the Avast LLVM 8 fork). Clang is not a separate pin.
Configure from the `llvm/` subdirectory (`SOURCE_SUBDIR` in `deps/llvm/CMakeLists.txt`).

## Policy

- Never edit `deps/llvm/`.
- One URL/SHA change in `cmake/deps.cmake` per commit, only as an explicitly
  scoped task. Snapshot `retdec.pointee` metadata before moving the URL.
- No drive-by bump. No `RETDEC_LLVM_NEXT` flag unless a later commit
  adds a real dual-build.

## Opaque pointers

LLVM 17 deletes pointee types from `ptr`. RetDec ports LLVM 8
`getPointerElementType()` into instruction metadata kind `retdec.pointee`
(same `setMetadata` pattern as `insn.addr`). Readers use
`llvm_utils::pointeeType` (MD first, typed-pointer fallback).

Counts (src/include/tests, not deps/llvm) — inventory from the migration
research, not a live census:

- 25 `getPointerElementType`
- 78 `PointerType::get(`
- 46 implicit `CreateLoad`, 44 implicit `CreateStore` (lifters)
- 373 files mention `llvm::`

## New pass manager

Still legacy: `llvm::legacy::PassManager` in `src/retdec/retdec.cpp`,
JSON pass names in `decompiler-config.json` / profiles. A new-PM rewrite is
the same class of work as a pin bump: not Track 1, not 2.0.22, not unless
tasked.

## Out of scope unless tasked

- No change to `cmake/deps.cmake`
- No edit under `deps/llvm/`
- No compile against a newer upstream LLVM
