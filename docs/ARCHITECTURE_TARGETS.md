# RetDec — Architecture Targets Roadmap

Status of **native** decompilation targets as of Imortek **2.0.22**.
Capstone dispatch and ABI tables live under `src/capstone2llvmir/` and
`src/bin2llvmir/providers/abi/`. CLI `-a|--arch` accepts
`mips|mips64|pic32|arm|thumb|arm64|powerpc|powerpc64|x86|x86-64|riscv|riscv64|sparc|sparc64|sysz|s390x|xcore`
(`src/retdec-decompiler/retdec-decompiler.cpp`).

LLVM is pinned to **23.1.0** in `cmake/deps.cmake`
(`llvmorg-23.1.0` / `llvm-project-23.1.0.src.tar.xz`). Upstream LLVM
targets are not RetDec lifters.

> **Honest status:** Integer/control-flow lifting for the CPU ISAs below is
> wired end-to-end (Capstone translator + ABI + decoder + ELF `e_machine` +
> CLI). SIMD/FP/atomics still fall back to pseudo-asm where Capstone IDs
> are unmapped. **SASS is not Production** — cubin/fatbin decode is a
> documented opcode subset, not a Capstone architecture.

---

## Summary

| Target | Status | Notes |
|--------|--------|-------|
| **x86 / x86-64** | Production | Default native path; Capstone + ABI + decoder coverage |
| **ARM / Thumb** | Production (integer) | `arm/` + Thumb interwork; 32-bit |
| **ARM64** (AArch64) | Production (integer) | `arm64/` + AAPCS64 ABI; `-a arm64` sets 64-bit |
| **MIPS 32/64 + PIC32** | Production (integer) | Delay slots; `AbiMips` / `AbiMips64` / `AbiPic32` |
| **PowerPC 32/64** | Production (integer) | `AbiPowerpc` / `AbiPowerpc64` |
| **RISC-V** (RV32I / RV64I) | Production (integer) | Capstone 5.0.9 `CS_ARCH_RISCV`; integer `C_*`; F/D/A/M/CSR → pseudo-asm |
| **SPARC** V8 32 / V9 64 | Production (integer) | Capstone basic mode 0; V9 is `CS_MODE_V9`. Typical delay slot (annul not classified as likely) |
| **SystemZ** | Production (integer, 64-bit) | s390x only. Capstone has no 31-bit / ESA-390 mode |
| **XCore** | Production (integer, 32-bit) | XS1/XS2. No 64-bit Capstone mode |
| **SASS** (NVIDIA GPU machine code) | Library + CLI probe, not Production | `src/sass_decode/` cubin/fatbin + SM_70/80 subset. **Not** a `-a` token. See [internal/wire-sass.md](internal/wire-sass.md) |

Managed bytecode (JVM, DEX, CIL, CPython, Lua, WASM) is a separate
dispatcher — [architecture.md](architecture.md) § Managed-Language Dispatch.
Those are not CPU ISA targets.

---

## RISC-V (RV32I / RV64I)

- Translator: `src/capstone2llvmir/riscv/`
- ABI: `AbiRiscv` / `AbiRiscv64` (psABI `a0`–`a7`)
- CLI: `-a riscv` / `-a riscv64`
- ELF: `EM_RISCV` (243)
- Compressed integer `C_*` via extra `CS_MODE_RISCVC`. F/D/A/M/CSR unmapped.

---

## ARM64 (AArch64)

- Capstone translator: `src/capstone2llvmir/arm64/`
- ABI: `src/bin2llvmir/providers/abi/arm64.cpp`
- CLI: `-a arm64` implies 64-bit (do not require a separate `-b 64`)
- SIMD / atomics / BTI/PAC may still degrade to pseudo-asm

---

## SPARC, SystemZ, and XCore

`createSparc` / `createSysz` / `createXcore` construct the in-tree
translators. Capstone support is **enabled** in `deps/capstone/CMakeLists.txt`
(`CAPSTONE_SPARC_SUPPORT`, `CAPSTONE_SYSZ_SUPPORT`, `CAPSTONE_XCORE_SUPPORT`).

- SPARC: `-a sparc` / `-a sparc64`; ELF `EM_SPARC` / `EM_SPARC32PLUS` / `EM_SPARCV9`
- SystemZ: `-a sysz` / `-a s390x`; ELF `EM_S390`; 64-bit GPRs only
- XCore: `-a xcore`; ELF `EM_XCORE`; 32-bit only

---

## SASS (NVIDIA Streaming Assembler)

### Current state

- `src/sass_decode/` loads standalone cubin (ELF `e_machine = 190` /
  `EM_CUDA`) and fatbin (`FATBIN_MAGIC 0xBA55ED50`).
- `retdec-decompiler` probes cubin/fatbin **before** the native LLVM
  pipeline and writes CUDA-C subset output. **Not** `-a sass`.
- `--bit-size 32|64` on that path is GPU pointer width; omitted → ELF class.
- **Not Production.** No public bit-accurate SASS ISA. `nvdisasm` is not
  called or bundled.

Do **not** claim SASS Production until encodings are complete without
nvdisasm and recovered kernels match the x86 quality bar.

---

## Cross-cutting dependencies

| Component | Relevance |
|-----------|-----------|
| `src/capstone2llvmir/` | ISA front-ends (`createArch`) |
| `src/bin2llvmir/` | Instruction → LLVM IR + ABI |
| `include/retdec/config/` | Architecture name, bitness, endian |
| `src/fileformat/` | ELF/Mach-O/COFF machine detection |
| `src/retdec-decompiler/` | `-a` allow-list + cubin probe |
| `tests/decompiler/`, `tests/capstone2llvmir/` | Smoke + translator tests |
| CI presets | `full-linux-release`, `full-windows-release` |

---

## Related documents

- [ENGINEERING_ROADMAP.md](internal/ENGINEERING_ROADMAP.md)
- [future_directions.md](future_directions.md)
- [architecture.md](architecture.md)
