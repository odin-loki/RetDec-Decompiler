# RetDec — Architecture Targets Roadmap

Status of **native** decompilation targets as of Imortek **2.0.26**.
Capstone dispatch and ABI tables live under `src/capstone2llvmir/` and
`src/bin2llvmir/providers/abi/`. CLI `-a|--arch` accepts
`mips|mips64|pic32|arm|thumb|arm64|powerpc|powerpc64|x86|x86-64|riscv|riscv64|sparc|sparc64|sysz|s390x|xcore`
(`src/retdec-decompiler/retdec-decompiler.cpp`).

LLVM is pinned to **23.1.0** in `cmake/deps.cmake`
(`llvmorg-23.1.0` / `llvm-project-23.1.0.src.tar.xz`). Capstone is
**6.0.0-Alpha10**. Upstream LLVM targets are not RetDec lifters.

> **Honest status:** Compiler integer, FP, SIMD, and atomics for the CPU
> ISAs below lift to LLVM IR at the same bar as x86-64 (Capstone translator
> + ABI + decoder + ELF `e_machine` + CLI). That bar is gcc/clang `-O0`/`-O1`/`-O2`
> on the default march — fused multiply-add, vector arith, compact encodings,
> channel I/O, string/MVC, VIS used by gcc `-mvis`, plus SSE4.1 insert/extract,
> BMI2 PDEP/PEXT, AVX 128-bit lane insert/extract, and RISC-V Zba/Zbb/Zbs/Zcb/Zbkb
> (x86 BMI/POPCNT/BTS/MOVZX/PUNPCK analogues). Crypto AES/SHA stays
> `nullptr` on **every** ISA including x86 (`AESENC`). Leftover specialized
> IDs (SVE/SME, RVV register file, QPX, unprinted SASS opcodes) still
> become `__asm_*`. **SASS is not Production** — cubin/fatbin decode is a
> documented opcode subset, not a Capstone architecture.

---

## Summary

| Target | Status | Notes |
|--------|--------|-------|
| **x86 / x86-64** | Production | Default native path; integer + FP + SSE/AVX including VINSERTI128/VPSLLW/VPMULLD + BMI2 PDEP/PEXT. AES/SHA nullptr |
| **ARM / Thumb** | Production | VFP/NEON including VFMA, VMULL/VSHRN/VMOVL/VMLSL/VQABS, VLD2–4, VRINT*. Capstone 6 alias mismatches remain in some historical tests |
| **ARM64** (AArch64) | Production | AAPCS64; FMLA; CASP; INS; SQADD/SMLAL/SABD/SHSUB/UMLSL. SVE/AES/SHA leftover (AES matches x86) |
| **MIPS 32/64 + PIC32** | Production | FPU; microMIPS + compact R6; LWM/SWM; MSA ADDV/ADDS.S/CEQI/SAT.S/SLD |
| **PowerPC 32/64** | Production | Compiler FP + gcc -O2 VSX/Altivec including VPERM/XVMAXDP/XVRDPI/VRLW. QPX unmapped |
| **RISC-V** (RV32I / RV64I) | Production | I+C+M+F+D+A+CSR + Zba/Zbb/Zbs/Zcb/Zbkb/Zicond. RVV register file not typed |
| **SPARC** V8 32 / V9 64 | Production | IEEE FP + gcc `-mvis` including FPADD64/MOVR/UMULXHI/TA. Quad leftover |
| **SystemZ** | Production | s390x 64-bit. BFP + MVC + CS/LOCGR/LAN + VPERM/VESL/VGBM/VFEE/VSCEF |
| **XCore** | Production | XS1/XS2 32-bit only. Integer/control-flow + channel IN/OUT family (`xcore.chan.*`). CHKCT/FREET leftover |
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
  load/store. Fused `FMADD`/`FMSUB`/`FNMADD`/`FNMSUB`, `FSQRT`, `FSGNJ*`,
  `FMIN`/`FMAX`, `FMV_*`, `FCLASS`, `AMOMAX`/`AMOMIN` (signed/unsigned, aq/rl),
  and Zba/Zbb/Zbs/Zcb/Zicond (`ANDN`/`CLZ`/`CTZ`/`CPOP`/`MIN`/`SH*ADD`/`REV8`/`ORC.B`/
  `BCLR`/`BSET`/`BEXT`/`CZERO_EQZ`/`C.ZEXT`/`C.NOT`)
  are real IR. Half-precision and RVV stay unmapped (no V0..V31 LLVM types).

---

## ARM64 (AArch64)

- Capstone translator: `src/capstone2llvmir/arm64/`
- ABI: `src/bin2llvmir/providers/abi/arm64.cpp`
- CLI: `-a arm64` implies 64-bit (do not require a separate `-b 64`)
- Scalar FP, vector FADD/FSUB/FMUL/FDIV/FMLA/FMLS (`llvm.fma`), LD1–LD4/
  ST1–ST4, LD1R–LD4R, TBL/TBX, INS, FACGE/GT, MLA/MLS, SADDLP, FMAXV,
  SQADD/UQADD, SQXTN/UQXTN, ADDHN, SMULL2, SADDLV,
  LDXR/STXR, LSE LDADD/SWP/CAS/CASP (i128 `cmpxchg`, `CMPXCHG16B` analogue)
  are real IR. SVE/SME, AES/SHA, widening FMLAL remain unmapped.

---

## SPARC, SystemZ, and XCore

`createSparc` / `createSysz` / `createXcore` construct the in-tree
translators. Capstone support is **enabled** in `deps/capstone/CMakeLists.txt`
(`CAPSTONE_SPARC_SUPPORT`, `CAPSTONE_SYSTEMZ_SUPPORT`, `CAPSTONE_XCORE_SUPPORT`).

- SPARC: `-a sparc` / `-a sparc64`; ELF `EM_SPARC` / `EM_SPARC32PLUS` / `EM_SPARCV9`. IEEE FP loads/stores share Capstone `LD`/`ST`/`LDD`/`STD` (no `LDF` token). gcc `-mvis` packed ops including FPADD64/FPACKFIX + `CASA`/`POPC`/`LDSTUB`/`SWAP`/`MOVR`/`ASI_P` LDA/STA/`FNOR` are real IR.
- SystemZ: `-a sysz` / `-a s390x`; ELF `EM_S390`; 64-bit GPRs only (`SYSZ_REG_R0D` in Capstone 6). MVC/CLC, LM/STM, CS/CSG, LOCGR/RISBG/LAA/LAN, vector `VA`/`VPERM`/`VFMA`/`VSEL`/`VESL` are real IR.
- XCore: `-a xcore`; ELF `EM_XCORE`; 32-bit only. Channel `IN`/`OUT` family lifts to named `xcore.chan.*` helpers (x86 `INS`/`OUTS` parity).

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
