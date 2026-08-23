# LLVM migration scope (D1 / D4)

Working plan: [`UNBLOCKED-MIGRATION.md`](UNBLOCKED-MIGRATION.md).
User-unblocked 2026-08-23.

## Pin (current — do not change until Track 1 MD is the source of truth)

From [`cmake/deps.cmake`](../../cmake/deps.cmake):

| Variable | Value |
|----------|--------|
| `LLVM_URL` | `https://github.com/avast/llvm/archive/a776c2a976ef64d9cd84d7ee71d0e4a04aa117a1.zip` |
| Commit | `a776c2a976ef64d9cd84d7ee71d0e4a04aa117a1` |
| `LLVM_ARCHIVE_SHA256` | `b5879b30768135e5fce84ccd8be356d2c55c940ab32ceb22d278b228e88c4c60` |

Avast LLVM 8-era fork. Clang is not a separate pin.

## Policy

- Never edit `deps/llvm/`.
- One URL/SHA change in `cmake/deps.cmake` per commit, only after
  `retdec.pointee` metadata is the source of truth.
- No drive-by bump. No `RETDEC_LLVM_NEXT` flag unless a later commit
  adds a real dual-build.

## Opaque pointers

LLVM 17 deletes pointee types from `ptr`. RetDec ports LLVM 8
`getPointerElementType()` into instruction metadata kind `retdec.pointee`
(same `setMetadata` pattern as `insn.addr`). Readers use
`llvm_utils::pointeeType` (MD first, typed-pointer fallback).

Counts (src/include/tests, not deps/llvm):

- 25 `getPointerElementType`
- 78 `PointerType::get(`
- 46 implicit `CreateLoad`, 44 implicit `CreateStore` (lifters)
- 373 files mention `llvm::`

## New pass manager

Still legacy: `llvm::legacy::PassManager` in `src/retdec/retdec.cpp`,
JSON pass names in `decompiler-config.json` / profiles. Rewrite is
Track 2, same branch family as the pin bump, not Track 1.

## Out of scope for Track 1

- No change to `cmake/deps.cmake`
- No edit under `deps/llvm/`
- No compile against upstream LLVM yet
