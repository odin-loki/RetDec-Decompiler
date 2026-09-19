# RetDec — Architecture Targets Roadmap

Status of **native** decompilation targets as of Imortek **2.0.25**.
Capstone dispatch and ABI tables live under `src/capstone2llvmir/` and
`src/bin2llvmir/providers/abi/`. CLI `-a|--arch` accepts
`mips|mips64|pic32|arm|thumb|arm64|powerpc|powerpc64|x86|x86-64|riscv|riscv64|sparc|sparc64|sysz|s390x|xcore`
(`src/retdec-decompiler/retdec-decompiler.cpp`).

LLVM is pinned to **23.1.0** in `cmake/deps.cmake`
(`llvmorg-23.1.0` / `llvm-project-23.1.0.src.tar.xz`). Capstone is
**6.0.0-Alpha10**. Upstream LLVM targets are not RetDec lifters.

> **Honest status:** Compiler integer, FP, and mapped SIMD/atomics for the
> CPU ISAs below lift to LLVM IR (Capstone translator + ABI + decoder +
> ELF `e_machine` + CLI). Leftover specialized IDs (SVE, AES/SHA, VIS,
> XCore channel/event ops, unprinted SASS opcodes) still become
> `__asm_*`. **SASS is not Production** — cubin/fatbin decode is a
> documented opcode subset, not a Capstone architecture.

---

## Summary

| Target | Status | Notes |
|--------|--------|-------|
| **x86 / x86-64** | Production | Default native path; Capstone + ABI + decoder coverage |
| **ARM / Thumb** | Production | 32-bit + Thumb interwork; compiler VFP/NEON. Capstone 6 alias mismatches remain in some historical tests |
| **ARM64** (AArch64) | Production | AAPCS64; `-a arm64` is 64-bit. Scalar+vector FP; LSE; PAC/BTI are NOP. SVE/AES/SHA/FMLA leftover |
| **MIPS 32/64 + PIC32** | Production | Delay slots; FPU + integer MUL/DIV. Compact R6 / microMIPS leftover |
| **PowerPC 32/64** | Production | Compiler FP + gcc -O1 VSX/Altivec. QPX unmapped |
| **RISC-V** (RV32I / RV64I) | Production | `CS_ARCH_RISCV` + `CS_MODE_RISCVC`/`FD`/`A`. I+C+M+F+D+A+CSR. AMOMIN/MAX and fused FP leftover |
| **SPARC** V8 32 / V9 64 | Production | Mode 0 + extra `CS_MODE_V9`. Integer + IEEE FP. VIS/quad/ASI leftover |
| **SystemZ** | Production | s390x 64-bit only (no 31-bit Capstone mode). Integer + BFP + vector load/store |
| **XCore** | Production | XS1/XS2 32-bit only. Integer/control-flow. Channel/event/thread ops leftover |
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
- Compressed integer `C_*` via extra `CS_MODE_RISCVC`. M/F/D/A/CSR lift
  to LLVM `mul`/`sdiv`, `float`/`double`, `atomicrmw`, and CSR
  load/store. AMOMIN/MAX and fused FMADD stay unmapped.

---

## ARM64 (AArch64)

- Capstone translator: `src/capstone2llvmir/arm64/`
- ABI: `src/bin2llvmir/providers/abi/arm64.cpp`
- CLI: `-a arm64` implies 64-bit (do not require a separate `-b 64`)
- Scalar FP, vector FADD/FSUB/FMUL/FDIV, LD1/ST1, LDXR/STXR, LSE
  LDADD/SWP/CAS are real IR. SVE/SME, AES/SHA, FMLA/FMLS, pair-CAS
  (`CASP*`) remain unmapped or explicit nullptr.

---

## SPARC, SystemZ, and XCore

`createSparc` / `createSysz` / `createXcore` construct the in-tree
translators. Capstone support is **enabled** in `deps/capstone/CMakeLists.txt`
(`CAPSTONE_SPARC_SUPPORT`, `CAPSTONE_SYSTEMZ_SUPPORT`, `CAPSTONE_XCORE_SUPPORT`).

- SPARC: `-a sparc` / `-a sparc64`; ELF `EM_SPARC` / `EM_SPARC32PLUS` / `EM_SPARCV9`
- SystemZ: `-a sysz` / `-a s390x`; ELF `EM_S390`; 64-bit GPRs only (`SYSZ_REG_R0D` in Capstone 6)
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
  called or bundled. Printed NVIDIA encodings in-tree include EXIT, NOP,
  IADD3, FADD, IMAD, LDG/STG, BRA, S2R, MOV, IMAD.WIDE, SHFL, S2UR, LDCU.
  FMUL/FFMA/ISETP/SHL/SHR/LOP3 are **not** lifted (no printed encoding).

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
