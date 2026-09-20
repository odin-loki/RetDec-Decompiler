# ARM64 (AArch64) integrator notes

64-bit only (`CS_ARCH_ARM64`, Capstone 6.0.0-Alpha10). AArch32 / Thumb is a different
front-end (`src/capstone2llvmir/arm/`). Do not edit `architecture.h`, the
decoder, or the CLI: `-a arm64` and `Architecture::isArm64()` already exist.

## Where it lives

| Piece | Path |
|-------|------|
| Lifter | `src/capstone2llvmir/arm64/` (`arm64.cpp`, `arm64_init.cpp`, `arm64_fp_ext.cpp`) |
| Public headers | `include/retdec/capstone2llvmir/arm64/` |
| ABI | `src/bin2llvmir/providers/abi/arm64.cpp` |
| Calling convention | `src/bin2llvmir/providers/calling_convention/arm64/arm64_conv.cpp` (X0–X7 args, V0–V7 FP, `_largeObjectsPassedByReference`) |
| Decoder | already branches on `isArm64()` — 4-byte alignment, Capstone `CS_ARCH_ARM64` |
| Syscalls | `src/bin2llvmir/optimizations/syscalls/arm64.cpp` matches `ARM64_INS_SVC` |

LLVM IR data layout emitted by the translator is ELF AArch64:

```
e-m:e-i64:64-i128:128-n32:64-S128
```

Windows objects want `m:w` (and COFF mangling). That is a module-level
property the fileformat / decoder side would have to set; this lifter does
not.

## Implemented ABI (AAPCS64 / Linux ELF)

This is what `AbiArm64` and `Arm64CallingConvention` model:

| Role | Register |
|------|----------|
| Integer / pointer args | **X0–X7** |
| Indirect result (large struct return) | **X8** |
| FP / SIMD args | V0–V7 |
| Integer result | X0 (X1 for 16-byte composites — CC currently lists X0 only) |
| Frame pointer | **X29** |
| Link register | **X30** (`ARM64_REG_LR`) |
| Stack pointer | **SP** (16-byte aligned) |
| Zero register | XZR / WZR (reads as 0, writes discarded) |
| Linux syscall number | **X8** |
| Linux syscall args / result | X0–X5 / X0 |

X8 is therefore two things at different times: the AAPCS64 indirect-result
pointer in ordinary calls, and the Linux syscall number around `svc #0`.
The ABI object records the syscall mapping; the calling-convention object
sets `_largeObjectsPassedByReference` rather than naming X8. Both are
correct for AAPCS64; a Windows-specific CC would still use X8 for the
indirect result and would **not** use X8 as a syscall number (see below).

Callee-saved (not recovered as a list in `AbiArm64`, but architecturally):
X19–X28, X29, SP, and V8–V15 (lower 64 bits). X16/X17 are IP0/IP1 (PLTs,
linker veneers).

## Windows ARM64 — not implemented as a second CC

AAPCS64 and the Windows ARM64 ABI share the integer argument and result
registers. They diverge in ways this tree cannot select without PE/COFF
detection in the decoder (out of scope here). Differences to keep in mind
when wiring PE ARM64:

| | AAPCS64 (what we implement) | Windows ARM64 |
|--|-----------------------------|---------------|
| Integer args | X0–X7 | X0–X7 |
| Indirect result | X8 | X8 |
| FP args | V0–V7, independent of GPRs | V0–V7, independent of GPRs |
| **X18** | platform register, caller-saved if unused | **reserved: TEB / PEB pointer**; must not be a temp |
| Variadic | remaining named args in registers, then stack | **all variadic args on the stack** (fixed args still in registers) |
| Home area | none | callee homes X0–X7 (64 bytes) at `[sp+0..+56]` |
| Unwind | DWARF / `.eh_frame` | ARM64 pdata / xdata (PE) |
| Syscall | `svc #0`, number in **X8** | `svc #imm` (SSN in the immediate) via ntdll; **not** Linux X8 |
| Data layout | `m:e` (ELF) | `m:w` (COFF) |

Until a PE path sets a Windows CC, decompiled Windows ARM64 will look like
AAPCS64: X18 may be treated as a GPR, variadic calls will look like they
passed trailing args in Xn, and there is no home-area model. Integer
leaf functions that only use X0–X7 / X29 / X30 / SP still recover.

## Lifter coverage (compiler AArch64, Capstone 6.0.0-Alpha10)

`CS_ARCH_ARM64`. Capstone 6.x has no `ARM64_REG_Vn` token: **Qn is the 128-bit
SIMD parent** (`i128`); Bn/Hn/Sn/Dn are nested views. `capstone6_compat.h`
maps 5.x `ARM64_REG_Vn` and `ARM64_INS_BTI` onto `ARM64_REG_Qn` /
`ARM64_INS_ALIAS_BTI`. “Real IR” means LLVM `add`/`fadd`/`load`/`store`/
`atomicrmw`/`cmpxchg`/… with **no** `__asm_*` barrier. PAC/BTI are the
identity (NOP). SVE Z/P registers and the compiler subset (WHILE*, PTRUE, CNT*,
INC*/DEC*, LD1/ST1/LD1RQ/LDNT1/STNT1, integer/FP/logic/shift/permute/compare
on Z or P) are typed and translated privately. SME `ZA` is a private `i2048`
and `FMOPA` writes the low tile. Do not invent tokens.

| Group | Capstone IDs | IR | Notes |
|-------|--------------|----|-------|
| ADD / ADDS / CMN | `ADD`, `ADDS`, `CMN` | real | shifted-reg, imm (`LSL #0/#12`), extend (`UXT*`/`SXT*`); vector `ADD` is lane-wise |
| SUB / SUBS / CMP | `SUB`, `SUBS`, `CMP` | real | same forms |
| AND / ORR / EOR | `AND`, `ANDS`, `ORR`, `EOR`, `BIC`, `ORN`, `EON`, `TST` | real | shifted-reg and logical-imm |
| MOV / MVN | `MOV`, `MVN`, `MOVZ`, `MOVN`, `MOVK` | real | `MOV` is often `ORR Xd, XZR, Xm` |
| LDR / STR | `LDR`, `LDRB`, `LDRH`, `LDRSB`, `LDRSH`, `LDRSW`, `STR`, `STRB`, `STRH`, `LDUR*`, `STUR*`, `LDTR*`, `STTR*` | real | unsigned imm, signed unscaled, reg-offset, pre/post WB |
| LDP / STP | `LDP`, `LDPSW`, `STP`, `LDNP`, `STNP` | real | compiler prologue `stp x29, x30, [sp, #-N]!` |
| Exclusive | `LDXR`, `LDXRB`, `LDXRH`, `LDAXR*`, `STXR`, `STXRB`, `STXRH`, `STLXR*`, `CLREX` | real / nothing | atomic load/store; STXR status is 0 (no exclusive-monitor model). `CLREX` is nop like ARM32 |
| B / B.cond | `B` | real | `cc` selects `__pseudo_branch` vs `__pseudo_cond_branch` |
| BL | `BL` | real | writes X30, `__pseudo_call` |
| BR | `BR` | real | `__pseudo_branch` (not a call; matches x86 `jmp`) |
| BLR | `BLR` | real | writes X30, `__pseudo_branch` (indirect) |
| RET | `RET` | real | default X30; `ret xn` allowed |
| CBZ / CBNZ | `CBZ`, `CBNZ` | real | |
| TBZ / TBNZ | `TBZ`, `TBNZ` | real | |
| ADR / ADRP | `ADR`, `ADRP` | real | Capstone already folds PC + imm |
| MUL / MADD / MSUB | `MUL`, `MADD`, `MSUB`, `MNEG` | real | Capstone reports `MUL` as its own id (alias of `MADD …, XZR`) |
| Scalar FP | `FADD`, `FSUB`, `FMUL`, `FNMUL`, `FDIV`, `FCMP`, `FCVT`, `FMOV` (Sn/Dn), `FMLA`, `FMLS` | real | Sn/Dn, `VAS` invalid; `FMLA`/`FMLS` are 3-operand fused (`llvm.fma`); 4-operand `FMADD`/`FMSUB` already real |
| SIMD lane FP | `FADD`, `FSUB`, `FMUL`, `FDIV`, `FMLA`, `FMLS` on `.2s`/`.4s`/`.2d` | real | `translateNeonFpLaneBinary` / `translateFMla`; `.8h`/`.4h` and by-element `vm.s[n]` stay pseudo |
| SIMD list | `LD1`, `ST1`, `LD2`/`ST2`, `LD3`/`ST3`, `LD4`/`ST4`, `LD1R`–`LD4R` | real | whole-register / D-form; lane-index LD/ST stay nullptr. LD2–4 de-interleave. `LD*R` replicate one loaded element (x86 MOVDDUP / VBROADCAST) |
| SIMD table | `TBL`, `TBX` | real | 1–4 table regs, 16B each; OOB is 0 (TBL) or dest (TBX) |
| SIMD FP compare | `FCMEQ`, `FCMGE`, `FCMGT`, `FCMLE`, `FCMLT`, `FACGE`, `FACGT` | real | `.2s`/`.4s`/`.2d`; `fcmp` + sext mask like SSE cmpps. `FACGE`/`FACGT` compare `fabs` of both lanes |
| SIMD insert / MAC | `INS`, `MLA`, `MLS` | real | `INS` merges a GPR or lane into Qn (x86 PINSR*). `MLA`/`MLS` are dest ± a*b; by-element stays pseudo |
| SIMD pairwise widen | `SADDLP`, `UADDLP` | real | adjacent pairs, sign/zero-extend, add into double-width lanes (x86 PHADD-class) |
| SIMD sat / widen | `SQADD`/`UQADD`/`SQSUB`/`UQSUB`, `SQXTN`/`UQXTN` (+2), `ADDHN`/`RADDHN` (+2), `SADDW2`/`UADDW2`, `SMULL2`/`UMULL2`, `SADDLV`/`UADDLV` | real | x86 PADDSB/PACKSSWB/PMOVSX parity; select-clamp like ARM32 VQADD, not `llvm.sadd.sat` |
| SIMD across FP | `FMAXV`, `FMINV` | real | across-vector FP min/max into scalar Sn/Dn; `.4h`/`.8h` stay pseudo |
| LSE atomics | `LDADD*`, `LDCLR*`, `LDEOR*`, `LDSET*`, `LDSMAX*`, `LDSMIN*`, `LDUMAX*`, `LDUMIN*`, `SWP*`, `CAS`/`CASA`/`CASL`/`CASAL` (+ B/H) | real | `atomicrmw` / `cmpxchg` |
| LSE pair-CAS | `CASP`, `CASPA`, `CASPL`, `CASPAL` | real | `cmpxchg` i128 (Xt pair) or i64 (Wt pair); Capstone lists Rs,Rs+1,Rt,Rt+1,[Xn] |
| SVC / BRK | `SVC`, `BRK` | `__asm_svc` / `__asm_brk` | same pattern as ARM32 `SVC`; SyscallFixer keys off `ARM64_INS_SVC` |
| PAC / BTI / HINT / PRFM | `PACIASP`, `AUTIASP`, `BTI` (`ARM64_INS_ALIAS_BTI`), `HINT`, `NOP`, `PRFM`, … | nothing | decoder `isNopInstruction` agrees |
| AES / SHA | `AESE`, `AESD`, `AESMC`, `AESIMC`, `SHA1C`/`H`/`M`/`P`/`SU0`/`SU1`, `SHA256H`/`H2`/`SU0`/`SU1` | real | `llvm.aarch64.crypto.*` (Qn is the 128-bit SIMD parent). Tests use `translate()`, not `emulate()` — llvmir-emul cannot lower these |
| CRC | `CRC32B`/`H`/`W`/`X`, `CRC32CB`/`CH`/`CW`/`CX` | real | `llvm.aarch64.crc32*` / `crc32c*` |
| Widening FMA | `FMLAL`, `FMLAL2`, `FMLSL`, `FMLSL2` | real | half→single `fpext` then `fadd`/`fsub` of the product; `*2` takes the upper source half |
| Polynomial | `PMULL`, `PMULL2`, `PMUL` | real | `llvm.aarch64.neon.pmull` / `pmull64` / `pmul`. x86 `PCLMULQDQ` counterpart |
| FP estimate / rint leftover | `FABD`, `FRECPE`, `FRECPS`, `FRECPX`, `FRSQRTE`, `FRSQRTS`, `FRINTI`, `FRINTX`, `URECPE`, `URSQRTE`, `FMAXNMP`, `FMAXNMV`, `FMINNMP`, `FMINNMV` | real | `FABD` is sign-bit-cleared `fsub` (no vector `fneg`). Estimates use `llvm.aarch64.neon.frecpe*` / `frsqrte*` (x86 RCPPS/RSQRTPS). `FRINTI`/`FRINTX` are `nearbyint`/`rint` |
| Compare / count leftover | `CMLE`, `CMLT`, `CLS` | real | `CMLE`/`CMLT` are `icmp sle/slt` + sext. `CLS` is `ctlz(x ^ ashr(x,1))-1` (ARM32 VCLS), scalar and vector |
| Lane shift leftover | `SHLL`/`2`, `SSHL`, `USHL`, `SLI`, `SRI`, `SRSHL`, `URSHL`, `SRSHR`, `URSHR`, `SSRA`, `USRA`, `SRSRA`, `URSRA`, `SSHLL`/`2`, `USHLL`/`2` | real | register-amount forms use `llvm.aarch64.neon.sshl*`. Immediate insert/accumulate is IR |
| Sat leftover | `SQDMULH`, `SQRDMULH`, `SQDMULL`/`2`, `SQDMLAL`/`2`, `SQDMLSL`/`2`, `SQRSHL`, `SQRSHRN`/`2`, `SQRSHRUN`/`2`, `UQRSHL`, `UQRSHRN`/`2`, `UQSHRN`/`2`, `SUQADD`, `USQADD` | real | doubling/round-sat via `llvm.aarch64.neon.sqdmulh*` / `sqdmull` / `suqadd`. Narrow rounding extends `translateNeonNarrow` |
| Abd / pairwise leftover | `SABA`, `SABAL`/`2`, `SADALP`, `UABA`, `UABAL`/`2`, `UADALP`, `SUBHN`/`2`, `RSUBHN`/`2` | real | dest += abs-diff or pairwise-widen; `SUBHN` is ADDHN with a subtract |
| SVE compiler subset | `WHILELT`/`LE`/`LO`/`LS`/`GE`/`GT`, `PTRUE`, `CNTB`/`H`/`W`/`D`, `CNTP`, `INCB`/`H`/`W`/`D`, `DECB`/`H`/`W`/`D`, `LD1B`/`H`/`W`/`D`, `ST1B`/`H`/`W`/`D`, `LD1RQ*`, `LDNT1B`/`H`/`W`/`D`, `STNT1B`/`H`/`W`/`D`, SVE `ADD`/`SUB`/`MUL` on Zn, `AND`/`ORR`/`EOR`/`ANDS`/`ORRS`/`EORS` on Z or P, `FADD`/`FSUB`/`FMUL`/`FDIV`/`FMLA`/`FMLS`/`FNEG`/`FABS`/`FADDA` on Z, `ASR`/`LSL`/`LSR` on Z, `DUP`/`INDEX`/`SEL`/`COMPACT`, `SUNPK*`/`UUNPK*`, `ZIP`/`UZP`/`TRN` on Z or P, `CMPEQ`/`NE`/`GE`/`GT`/`LT`/`LE`, `PFALSE`, `NOT` on P | real | Z0–Z31 are private `<16 x i8>` (SVE min VL / vscale=1). P0–P15 are `<16 x i1>`. Vector `fneg` is xor of the sign bit (llvmir-emul faults on `fneg` of a vector). Half/single/double FP on Z uses `<N x half/float/double>` |
| SVE mem / pred leftover | `LD1SB`/`SH`/`SW`, `LDNT1SB`/`SH`/`SW`, `LDFF1B`/`H`/`W`/`D`/`SB`/`SH`/`SW`, indexed `LD1W`/`ST1W` gather/scatter, `SETFFR`/`RDFFR`/`RDFFRS`/`WRFFR`, `PRFB`/`H`/`W`/`D`, `CLASTA`/`CLASTB`, `SPLICE`, `REV`/`REVB`/`REVH`/`REVW`/`REVD`, `SADDV`/`UADDV`/`UMAXV` on Z, SVE `FCMEQ`/`FCMGE`/`FCMGT`/`FCMLE`/`FCMLT` | real | Sign-extending loads widen packed mem elems. Gather/scatter when mem base/index is Z (per-lane). `ARM64_REG_FFR` is private `<16 x i1>` like P. Prefetch is NOP like `PRFM`. First-fault sets FFR to the governing pred (no fault model) |
| SVE remainder | remaining first-fault gather variants beyond `LDFF1*` already wired, structured `LD2`/`LD3`/`LD4` / `ST2`–`4` on Z, `LD1R*` broadcast, predicate `BRK*`, `WHILE*` wr variants | unmapped / generic pseudo | IDs exist in Capstone 6 but are outside the contiguous/indexed compiler subset typed at VL=128 |
| SME | `ARM64_REG_ZA` (private `i2048`), `ARM64_INS_FMOPA`, `ARM64_INS_ALIAS_SMSTART`/`SMSTOP` | real / NOP | ZA is typed privately as `i2048` (16×16 bytes at VL=128). `FMOPA` is a 4×4 (`.s`) or 2×2 (`.d`) outer product into the low tile. Slice regs `ZAB`/`ZAD`/`ZAH`/`ZAQ`/`ZAS` are not separately typed — `FMOPA` always writes `ZA`. Capstone 6 decodes `smstart`/`smstop` as `MSR` (id 754), not the alias; aliases are wired as NOP if they appear. No public-header change. |

Shifts `LSL`/`LSR`/`ASR`/`ROR`, bitfield aliases `UBFX`/`SBFX`/`BFI`/…,
canonical `UBFM`/`SBFM`/`BFM` (immr/imms: extract if imms>=immr else insert),
`FCCMP`/`FCCMPE` (`FCCMPE` shares `FCCMP` IR; FPSR Invalid-Op is not modelled),
`CSEL`/`CSET`/`CINC`, and `SXT*`/`UXT*` are also real IR (compiler
integer/FP, not listed in the must-lift table above). Capstone 6 may report
either the alias id or the real BFM/SBFM/UBFM id. Integer NEON (`ADD`/`SUB`/`MUL` `.4s`, `EXT`, `BSL`, compares, permutes)
already has a lane translator. `INS`, integer `MLA`/`MLS`, `SADDLP`/`UADDLP`,
and `FMAXV`/`FMINV` are the same model.

## Tests

`tests/capstone2llvmir/arm64_tests.cpp`. Production additions are the
`ARM64_INS_*_bin` / shifted-register / `SVC` / `BRK` cases at the bottom
of that file (assembled little-endian AArch64 words plus Keystone text).
Run:

```
ctest -R capstone2llvmir --output-on-failure
# or
retdec-tests-capstone2llvmir --gtest_filter='Capstone2LlvmIrTranslatorArm64Tests*'
```
