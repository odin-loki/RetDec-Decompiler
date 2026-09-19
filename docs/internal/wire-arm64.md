# ARM64 (AArch64) integrator notes

64-bit only (`CS_ARCH_ARM64`, Capstone 5.0.9). AArch32 / Thumb is a different
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

## Lifter coverage (integer compiler subset)

Capstone 5.0.9 IDs. “Real IR” means LLVM `add`/`load`/`store`/`icmp`/…
with **no** `__asm_*` barrier. SIMD/SVE stay pseudo-asm unless a lane
translator already exists. PAC/BTI are modelled as the identity (NOP).

| Group | Capstone IDs | IR | Notes |
|-------|--------------|----|-------|
| ADD / ADDS / CMN | `ADD`, `ADDS`, `CMN` | real | shifted-reg, imm (`LSL #0/#12`), extend (`UXT*`/`SXT*`) |
| SUB / SUBS / CMP | `SUB`, `SUBS`, `CMP` | real | same forms |
| AND / ORR / EOR | `AND`, `ANDS`, `ORR`, `EOR`, `BIC`, `ORN`, `EON`, `TST` | real | shifted-reg and logical-imm |
| MOV / MVN | `MOV`, `MVN`, `MOVZ`, `MOVN`, `MOVK` | real | `MOV` is often `ORR Xd, XZR, Xm` |
| LDR / STR | `LDR`, `LDRB`, `LDRH`, `LDRSB`, `LDRSH`, `LDRSW`, `STR`, `STRB`, `STRH`, `LDUR*`, `STUR*`, `LDTR*`, `STTR*` | real | unsigned imm, signed unscaled, reg-offset, pre/post WB |
| LDP / STP | `LDP`, `LDPSW`, `STP`, `LDNP`, `STNP` | real | compiler prologue `stp x29, x30, [sp, #-N]!` |
| B / B.cond | `B` | real | `cc` selects `__pseudo_branch` vs `__pseudo_cond_branch` |
| BL | `BL` | real | writes X30, `__pseudo_call` |
| BR | `BR` | real | `__pseudo_branch` (not a call; matches x86 `jmp`) |
| BLR | `BLR` | real | writes X30, `__pseudo_branch` (indirect) |
| RET | `RET` | real | default X30; `ret xn` allowed |
| CBZ / CBNZ | `CBZ`, `CBNZ` | real | |
| TBZ / TBNZ | `TBZ`, `TBNZ` | real | |
| ADR / ADRP | `ADR`, `ADRP` | real | Capstone already folds PC + imm |
| MUL / MADD / MSUB | `MUL`, `MADD`, `MSUB`, `MNEG` | real | Capstone reports `MUL` as its own id (alias of `MADD …, XZR`) |
| SVC / BRK | `SVC`, `BRK` | `__asm_svc` / `__asm_brk` | same pattern as ARM32 `SVC`; SyscallFixer keys off `ARM64_INS_SVC` |
| PAC / BTI / HINT / PRFM | `PACIASP`, `AUTIASP`, `BTI`, `HINT`, `NOP`, `PRFM`, … | nothing | decoder `isNopInstruction` agrees |
| SIMD / SVE / AES / SHA / LSE pair-CAS | various | pseudo-asm or dedicated NEON helpers | not the integer production bar |

Shifts `LSL`/`LSR`/`ASR`/`ROR`, bitfield aliases `UBFX`/`SBFX`/`BFI`/…,
`CSEL`/`CSET`/`CINC`, and `SXT*`/`UXT*` are also real IR (compiler
integer, not listed in the must-lift table above). Canonical `UBFM` /
`SBFM` / `BFM` stay `nullptr`: Capstone 5 reports the aliases on compiled
code.

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
