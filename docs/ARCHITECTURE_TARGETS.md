# RetDec — Architecture Targets Roadmap

Status of **native** decompilation targets as of Imortek **2.0.26**.
Capstone dispatch and ABI tables live under `src/capstone2llvmir/` and
`src/bin2llvmir/providers/abi/`. CLI `-a|--arch` accepts
`mips|mips64|pic32|arm|thumb|arm64|powerpc|powerpc64|x86|x86-64|riscv|riscv64|sparc|sparc64|sysz|s390x|xcore`
(`src/retdec-decompiler/retdec-decompiler.cpp`).

LLVM is pinned to **23.1.0** in `cmake/deps.cmake`
(`llvmorg-23.1.0` / `llvm-project-23.1.0.src.tar.xz`). Capstone is
**6.0.0-Alpha10**. Upstream LLVM targets are not RetDec lifters.

> **Honest status:** Compiler integer, FP, SIMD, atomics, and the AES/SHA/
> CRC32/PCLMUL analogues that Capstone actually enumerates now lift to LLVM
> IR at the same bar as x86-64 (Capstone translator + ABI + decoder + ELF
> `e_machine` + CLI). That bar is gcc/clang `-O0`/`-O1`/`-O2` on the default
> march — fused multiply-add, vector arith, compact encodings, channel I/O,
> string/MVC, VIS used by gcc `-mvis`, SSE4.1 insert/extract, BMI2 PDEP/PEXT,
> AVX 128-bit lane insert/extract, RISC-V Zba/Zbb/Zbs/Zcb/Zbkb/Zbc, AES-NI/
> VAES/SHA/CRC32/PCLMULQDQ, ARM/ARM64 crypto + `PMULL`, SPARC quad FP, PowerPC
> QPX `QV*`, SPE `EV*`, ARM64 SVE remainder, SME `FMOPA` (private `i2048`
> `ZA`), RISC-V RVV including segmented mem / widening FP / BF16 / `VAES*`,
> MSA including `FCLASS`/`FMADD`/`SRA*`, AVX-512 compute including `VPMOV*`/
> `VPERMI2*`/`VPCONFLICT*`, XOP `VFRCZ`/`VPCOM`, TBM, 3DNow core, MIPS DSP
> including `SHILO`/`SHILOV`, R6 `CMP_*_S/D`, Octeon `POP`/`CINS`, SPE
> `BRINC`, and `MFOCRF`/`MTOCRF`. Remaining specialized IDs (RVV SEW/LMUL
> switch, unprinted SASS opcodes) still become `__asm_*`.
> **SASS is not Production** — cubin/fatbin decode is a documented opcode
> subset, not a Capstone architecture.

---

## Summary

| Target | Status | Notes |
|--------|--------|-------|
| **x86 / x86-64** | Production | SSE/AVX/AVX2/AVX-512 compute including `VPMOV*`/`VPERMI2*`/`VPCONFLICT*` + AES-NI/SHA/CRC32/PCLMUL + FMA3 + F16C + XOP `VFRCZ`/`VPCOM` + TBM + 3DNow core. PadLock named helpers |
| **ARM / Thumb** | Production | VFP/NEON including VFMA, AES/SHA/CRC32, `VRINTR`/`VRINTX`/`CLREX`/`FLDM*X`, user-mode `MRS`/`MSR` |
| **ARM64** (AArch64) | Production | AES/SHA/CRC32/`PMULL`; leftover NEON; `BFM`/`SBFM`/`UBFM`/`FCCMPE`/`CLREX`; SVE remainder including gather/FFR; SME `FMOPA` |
| **MIPS 32/64 + PIC32** | Production | FPU; microMIPS compact + R6 including `CMP_*_S/D`; MSA; DSP packed integer on `AC0`–`AC3` including `SHILO`/`SHILOV`; Octeon `POP`/`CINS`/`EXTS`. No AES/SHA Capstone IDs |
| **PowerPC 32/64** | Production | VSX/Altivec + QPX `QV*` + SPE `EV*` + `BRINC` + `MFOCRF`/`MTOCRF` + ISEL + `XS*`/`XXSEL` + `LXVDSX` + FPSCR |
| **RISC-V** (RV32I / RV64I) | Production | I+C+M+F+D+A+CSR + Zb* + RVV including segmented mem, widening FP, BF16, `VAES*`/`VSHA2*`. SEW/LMUL unmapped |
| **SPARC** V8 32 / V9 64 | Production | IEEE FP including quad `F*Q` + `LDQ`/`STQ`; gcc `-mvis`; VIS3 `XMULX`; V8 `MULSCC`. No AES Capstone IDs |
| **SystemZ** | Production | s390x 64-bit. BFP + MVC + CS/LOCGR/LAN + VPERM/VESL/VGBM/VFEE/VSCEF + CPACF `KM`/`KIMD` named helpers |
| **XCore** | Production | XS1/XS2 32-bit only. Integer/control-flow + channel IN/OUT family (`xcore.chan.*`) + remaining Capstone IDs as named helpers |
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
  and Zba/Zbb/Zbs/Zcb/Zbkb/Zbc/Zicond (`ANDN`/`CLZ`/`CTZ`/`CPOP`/`MIN`/`SH*ADD`/`REV8`/`ORC.B`/
  `BCLR`/`BSET`/`BEXT`/`CZERO_EQZ`/`C.ZEXT`/`C.NOT`/`CLMUL`)
  are real IR. RVV types `V0`–`V31` privately as `<vscale x 4 x i32>`
  including mask logicals, unsigned compares, indexed/strided/segmented mem,
  widening FP, BF16, `VAES*`/`VSHA2*`. Half-precision and SEW/LMUL switching
  stay unmapped.

---

## ARM64 (AArch64)

- Capstone translator: `src/capstone2llvmir/arm64/`
- ABI: `src/bin2llvmir/providers/abi/arm64.cpp`
- CLI: `-a arm64` implies 64-bit (do not require a separate `-b 64`)
- Scalar FP, vector FADD/FSUB/FMUL/FDIV/FMLA/FMLS (`llvm.fma`), LD1–LD4/
  ST1–ST4, LD1R–LD4R, TBL/TBX, INS, FACGE/GT, MLA/MLS, SADDLP, FMAXV,
  SQADD/UQADD, SQXTN/UQXTN, ADDHN, SMULL2, SADDLV,
  LDXR/STXR, LSE LDADD/SWP/CAS/CASP (i128 `cmpxchg`, `CMPXCHG16B` analogue),
  AES/SHA/CRC32, and `PMULL`/`PMULL2` (`llvm.aarch64.neon.pmull*`) are real IR.
  SVE compiler subset (`WHILE*`, `PTRUE`, `CNT*`, `LD1`/`ST1` B/H/W/D, `ADD`/
  `SUB`/`MUL`, `AND`/`ORR`/`EOR` on Z/P) types Z/P privately as `<16 x i8>`/
  `<16 x i1>`. SME `FMOPA` uses private `i2048` `ZA`. `BFM`/`SBFM`/`UBFM`
  and `FCCMPE` lift. Privileged `SYS`/`WFI`/`TLBI` stay `nullptr`.

---

## SPARC, SystemZ, and XCore

`createSparc` / `createSysz` / `createXcore` construct the in-tree
translators. Capstone support is **enabled** in `deps/capstone/CMakeLists.txt`
(`CAPSTONE_SPARC_SUPPORT`, `CAPSTONE_SYSTEMZ_SUPPORT`, `CAPSTONE_XCORE_SUPPORT`).

- SPARC: `-a sparc` / `-a sparc64`; ELF `EM_SPARC` / `EM_SPARC32PLUS` / `EM_SPARCV9`. IEEE FP loads/stores share Capstone `LD`/`ST`/`LDD`/`STD` (no `LDF` token). Quad `F*Q` on `Q0`–`Q15` (`fp128`). gcc `-mvis` packed ops including FPADD64/FPACKFIX + `CASA`/`POPC`/`LDSTUB`/`SWAP`/`MOVR`/`ASI_P` LDA/STA/`FNOR`/`XMULX` are real IR.
- SystemZ: `-a sysz` / `-a s390x`; ELF `EM_S390`; 64-bit GPRs only (`SYSZ_REG_R0D` in Capstone 6). MVC/CLC, LM/STM, CS/CSG, LOCGR/RISBG/LAA/LAN, vector `VA`/`VPERM`/`VFMA`/`VSEL`/`VESL`, CPACF `KM`/`KIMD` named helpers are real IR.
- XCore: `-a xcore`; ELF `EM_XCORE`; 32-bit only. Channel `IN`/`OUT` family plus remaining Capstone IDs lift to named `xcore.chan.*` / `xcore.thread.*` / `xcore.res.*` helpers (x86 `INS`/`OUTS` parity).

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
