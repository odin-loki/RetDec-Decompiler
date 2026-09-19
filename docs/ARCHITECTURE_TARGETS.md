# RetDec — Architecture Targets Roadmap

Status of **native** decompilation targets as of Imortek **2.0.22**.
Capstone dispatch and ABI tables live under `src/capstone2llvmir/` and
`src/bin2llvmir/providers/abi/`. CLI `-a|--arch` accepts
`mips|pic32|arm|thumb|arm64|powerpc|x86|x86-64` only
(`src/retdec-decompiler/retdec-decompiler.cpp`).

LLVM is pinned to **23.1.0** in `cmake/deps.cmake`
(`llvmorg-23.1.0` / `llvm-project-23.1.0.src.tar.xz`). Upstream LLVM
targets are not RetDec lifters.

> **Honest status:** RISC-V, full ARM64, SASS, SPARC, SystemZ, and XCore are
> **not** production decompilation targets today. Capstone `createSparc` /
> `createSysz` / `createXcore` throw `GenericError`. There is no RISC-V
> translator directory.

---

## Summary

| Target | Status | Notes |
|--------|--------|-------|
| **x86 / x86-64** | Production | Default native path; Capstone + ABI + decoder coverage |
| **ARM / Thumb / MIPS / PowerPC** | Partial | Lifter + ABI present (`arm/`, `mips/`, `powerpc/`); not the production e2e bar |
| **PIC32** | Partial | Listed on `-a`; MIPS-family ABI (`pic32.cpp`) |
| **ARM64** (AArch64) | Incomplete | `src/capstone2llvmir/arm64/` + `src/bin2llvmir/providers/abi/arm64.cpp`; not production-ready end-to-end |
| **RISC-V** (RV32I / RV64I) | Not implemented | No `capstone2llvmir/riscv`, no `-a riscv`, no `EM_RISCV` wiring in `src/fileformat/` |
| **SPARC** | **Not implemented** | `createArch(CS_ARCH_SPARC)` → `createSparc` throws `GenericError("SPARC architecture is unimplemented.")` |
| **SystemZ** | **Not implemented** | `createSysz` throws `GenericError("SystemZ architecture is unimplemented.")` |
| **XCore** | **Not implemented** | `createXcore` throws `GenericError("XCore architecture is unimplemented.")` |
| **SASS** (NVIDIA GPU machine code) | Not implemented | No lifter. PTX **text** parser exists in `src/ptx_decompile/` and is **not** a CLI input |

Managed bytecode (JVM, DEX, CIL, CPython, Lua, WASM) is a separate
dispatcher — [architecture.md](architecture.md) § Managed-Language Dispatch.
Those are not CPU ISA targets.

---

## RISC-V (RV32I / RV64I)

### Current state

- No `bin2llvmir` architecture module and no `src/capstone2llvmir/riscv/`.
- No RetDec config preset or `-a riscv` / `-a riscv64`.
- `src/fileformat/` has no `EM_RISCV` handling in this tree.
- LLVM 23 includes a RISC-V backend; RetDec does not lift RISC-V machine
  code through `bin2llvmir` → `llvmir2hll`.

### Prerequisites (if scheduled)

1. Capstone (or LLVM MC) insn → LLVM IR mapping for RV32/RV64.
2. `bin2llvmir` register model and calling convention (RISC-V psABI).
3. ELF `EM_RISCV` in fileformat / config.
4. Tests: minimal RV64 ELFs under `tests/`.
5. CLI `-a` token and docs.

Suggested order: disassembly-only → lift to LLVM IR → C emission via
existing llvmir2hll.

---

## ARM64 (AArch64)

### Current state

- Capstone translator: `src/capstone2llvmir/arm64/` (`arm64.cpp`,
  `arm64_init.cpp`, `arm64_fp_ext.cpp`).
- ABI: `src/bin2llvmir/providers/abi/arm64.cpp` (`CC_ARM64`, X0–X7 args,
  X8 syscall).
- CLI: `-a arm64` is accepted.
- End-to-end quality and coverage lag x86. Mach-O / PE ARM64 edge cases
  remain open. Treat as **in progress**, not Tier-3 production.

### Remaining work (not done)

1. SIMD / atomics / BTI/PAC coverage in the lifter (may degrade).
2. Windows ARM64 ABI variants vs AAPCS64.
3. Regression corpus (Linux aarch64, iOS/macOS Mach-O) in CI smoke.
4. Documented quality bar matching x86.

---

## SPARC, SystemZ, and XCore

`Capstone2LlvmIrTranslator::createArch` has cases for `CS_ARCH_SPARC`,
`CS_ARCH_SYSZ`, and `CS_ARCH_XCORE`. Those call `createSparc` /
`createSysz` / `createXcore`, which **throw** `GenericError`. There are no
implementation directories under `src/capstone2llvmir/`. Public header
methods are retained (API); they do not produce a translator. `-a` does
not list these names.

---

## SASS (NVIDIA Streaming Assembler)

### Current state

- No SASS lifter or emitter.
- `src/ptx_decompile/` recovers **PTX text** and CUDA/OpenCL **host API**
  patterns. PTX lifting is unit-tested and **not** a `retdec-decompiler`
  input. `CudaHostRecovery` is **not** called from `decompile()`;
  `OclHostRecovery` is. See [CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md).

### If SASS were pursued

1. External `nvdisasm` (CUDA toolkit) — listing, not IR.
2. SASS varies by SM generation; no stable cross-generation IR.
3. Unlikely to share llvmir2hll.
4. CUDA EULA constraints on redistributing `nvdisasm`.

Do **not** claim SASS decompilation until a structured lift exists.

---

## Cross-cutting dependencies

| Component | Relevance |
|-----------|-----------|
| `src/capstone2llvmir/` | ISA front-ends (`createArch`) |
| `src/bin2llvmir/` | Instruction → LLVM IR + ABI |
| `include/retdec/config/` | Architecture name, bitness, endian |
| `src/fileformat/` | ELF/Mach-O/COFF machine detection |
| `src/retdec-decompiler/` | `-a` allow-list |
| `tests/decompiler/`, `tests/capstone2llvmir/` | Smoke + translator tests |
| CI presets | `full-linux-release`, `full-windows-release` |

---

## Related documents

- [ENGINEERING_ROADMAP.md](internal/ENGINEERING_ROADMAP.md)
- [future_directions.md](future_directions.md)
- [architecture.md](architecture.md)
