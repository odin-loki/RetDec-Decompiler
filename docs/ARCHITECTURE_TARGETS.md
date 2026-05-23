# RetDec — Architecture Targets Roadmap

Status of additional **native** decompilation targets beyond the current
x86, x86-64, ARM, Thumb, ARM64 (partial), MIPS, and PowerPC support.

> **Honest status:** RISC-V, full ARM64, and SASS are **not implemented** in
> RetDec today. This document lists prerequisites and integration points so
> work can be scheduled without overstating current capability.

---

## Summary

| Target | Status | Notes |
|--------|--------|-------|
| **RISC-V** (RV32I / RV64I) | Not implemented | LLVM backend exists; RetDec lifter + ABI tables missing |
| **ARM64** (AArch64) | Partial / incomplete | Capstone + some ARM64 init in `bin2llvmir`; not production-ready end-to-end |
| **SASS** (NVIDIA GPU machine code) | Not implemented | No lifter; research path via `nvdisasm` pre-processing only |

---

## RISC-V (RV32I / RV64I)

### Current state

- No `bin2llvmir` architecture module for RISC-V.
- No RetDec config preset or `-a riscv` CLI switch.
- LLVM upstream includes RISC-V targets, but RetDec does not wire them through
  `bin2llvmir` → `llvmir2hll`.

### Prerequisites

1. **Capstone or LLVM MC disassembler** integration for RV32/RV64 with stable
   insn → semantic mapping.
2. **`bin2llvmir` lifter** — register model (x0 zero, PC-relative addressing,
   CSR handling policy), calling convention (RISC-V ABI document).
3. **Config / fileformat** — ELF RISC-V machine type detection (`EM_RISCV`).
4. **Tests** — minimal RV64 `hello` / `fib` ELFs in `tests/test_binaries/`.
5. **Documentation** — `-a riscv64` (or similar) in user manual.

### Suggested milestone order

1. Disassembly-only in fileinfo / capstone2llvmir stub
2. Lift to LLVM IR (no hll)
3. End-to-end C emission via existing `llvmir2hll`

---

## ARM64 (AArch64)

### Current state

- Capstone ARM64 support and initializer code exist under `capstone2llvmir/arm64/`.
- End-to-end decompilation quality and coverage lag x86 and 32-bit ARM.
- Mach-O ARM64 and PE ARM64 edge cases remain open.

### Prerequisites

1. **Complete `bin2llvmir` ARM64 lifter** — SIMD, atomics, BTI/PAC stubs (may degrade gracefully).
2. **Calling convention tables** — AAPCS64, Windows ARM64 ABI variants.
3. **Regression corpus** — iOS/macOS ARM64 ELFs/Mach-O, Linux aarch64 binaries.
4. **GUI / CLI** — ensure `-a arm64` path is tested in CI smoke tests.

### Gap vs “implemented”

Treat ARM64 as **in progress**: components exist, but Tier 3 product criteria
(stable smoke + docs + parity) are not met.

---

## SASS (NVIDIA Streaming Assembler)

### Current state

- No SASS lifter or emitter in RetDec.
- CUDA support today targets **PTX**-level recovery where embedded in binaries,
  not raw SASS machine code.

### Prerequisites

1. **External tool chain** — `nvdisasm` (CUDA toolkit) to disassemble SASS →
   human-readable listing; not a structured IR.
2. **Architecture research** — SASS varies by SM version (Maxwell, Pascal, Volta,
   Turing, Ampere, …); no stable cross-generation IR.
3. **Integration design** — pre-processor stage: SASS blob → text → (future)
   pattern-based struct recovery; unlikely to share `llvmir2hll`.
4. **Legal / distribution** — CUDA EULA for redistributing `nvdisasm` with RetDec packages.

### Recommended scope (if pursued)

- Document-only + optional plugin hook that shells out to `nvdisasm`
- Do **not** claim SASS decompilation in product marketing until a structured
  lift exists

See also [future_directions.md](future_directions.md) § GPU / CUDA and
[RESEARCH_FRONTIERS.md](RESEARCH_FRONTIERS.md).

---

## Cross-cutting dependencies

| Component | Relevance |
|-----------|-----------|
| `src/capstone2llvmir/` | New architecture front-ends |
| `src/bin2llvmir/` | Instruction → LLVM IR lifting |
| `include/retdec/config/` | Architecture name, bitness, endian |
| `src/fileformat/` | ELF/Mach-O/COFF machine enum detection |
| `tests/decompiler/` | Smoke + format router tests |
| CI presets | `full-linux-release`, `full-windows-release` |

---

## Related documents

- [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) — shippable tiers 1–5
- [future_directions.md](future_directions.md) — Part 2 new targets table
- [architecture.md](architecture.md) — current pipeline overview
