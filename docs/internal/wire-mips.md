# Integrator notes — MIPS 32 / MIPS 64 / PIC32

Status of the **compiler O32 / N64 subset** lifted to LLVM IR. PIC32 is not
a separate Capstone architecture: it uses the MIPS translator plus `AbiPic32`.
Do **not** edit decoder / CLI / CMakeLists / `architecture.h` for this work.

Factories already exist: `createMips32`, `createMips64`, `createMips3`,
`createMips32R6`. Extra mode `CS_MODE_MICRO` is allowed; compact encodings
are **not** modelled (see microMIPS below).

---

## Status

| Target | Status | Notes |
|--------|--------|-------|
| **MIPS 32-bit (O32)** | **Production** | `CS_MODE_MIPS32`. Word ALU, loads/stores, branches/jumps with delay slots, `MULT`/`DIV`+`MFLO`/`MFHI`, COP1 FPU, `SYSCALL` as pseudo-call. |
| **MIPS 64-bit (N64)** | **Production** | `CS_MODE_MIPS64`. Same subset; word ops sign-extend into 64-bit GPRs; `LD`/`SD` and `D*`-prefixed doubleword ALU. |
| **PIC32** | **Production (ISA)** | Same MIPS32 translator. ABI in `pic32.cpp` matches `mips.cpp` for SP/zero/V0/A0–A3 plus GP/K0/K1 roles. **microMIPS firmware is a gap**, not a fake translation. |

`createMips32R6` is wired. When Capstone reports three GPR operands, `div`/`mod`/
`mul`/`muh` (and unsigned / `D*` forms) write a GPR; pre-R6 two-operand `div`/
`mult` still write HI/LO. Compact R6 branches (`BC`, `BEQC`, `JIC`, …) stay on
the pseudo-assembly path and **must not** grow a delay slot.

Capstone 5.0.9 ORs `CS_MODE_32` into `CS_MODE_MIPS32R6`, then the disassembler
takes the `CS_MODE_32` branch and **clears** `FeatureMips32r6`. R6 encodings
often do not decode; the translator still implements the 3-operand forms.

---

## Production subset (real LLVM IR)

Integer / memory / control that a compiler emits for O32/N64:

- ALU: `ADD`/`ADDU`/`ADDI`/`ADDIU`, `SUB`/`SUBU`, `AND`/`ANDI`, `OR`/`ORI`,
  `XOR`/`XORI`, `NOR`, `SLT`/`SLTI`/`SLTU`/`SLTIU`, `LUI`
- Memory: `LB`/`LBU`/`LH`/`LHU`/`LW`/`LWU`, `SB`/`SH`/`SW`; `LD`/`SD` when
  `MODE_MIPS64`
- Control: `BEQ`/`BNE`/`B`, `J`/`JAL`/`JR`/`JALR` (including `jalr rd, rs`)
- Multiply/divide: `MULT`/`MULTU`/`DIV`/`DIVU` + `MFLO`/`MFHI` (pre-R6);
  R6 GPR `MUL`/`MULU`/`MUH`/`MUHU`/`DIV`/`DIVU`/`MOD`/`MODU`
- COP1 FPU (Capstone 6 split ids, not the old collapsed `MIPS_INS_C` /
  `MIPS_INS_CVT`): `ADD_S`/`ADD_D`/`FADD_D`, `SUB_S`/`SUB_D`/`FSUB_D`,
  `MUL_S`/`MUL_D`/`FMUL_D`, `DIV_S`/`DIV_D`/`FDIV_D`, `ABS_S`/`ABS_D`,
  `NEG_S`/`NEG_D`, `SQRT_S`/`SQRT_D`, `MOV_S`/`MOV_D`, `LWC1`/`LDC1`/
  `SWC1`/`SDC1`, all 16 `C_*.S`/`C_*.D` (and `ALIAS_C_*`), `CVT_*`,
  `TRUNC`/`ROUND`/`CEIL`/`FLOOR` `_W/_L` `_S/_D`, `MFC1`/`MTC1`/`MFHC1`/
  `MTHC1`, `RECIP_S`/`D`, `RSQRT_S`/`D`
- MSA bitwise used by compilers: `AND_V`/`OR_V`/`XOR_V`/`NOR_V` as i128
  when Capstone reports those ids. Default `createMips32` does not enable
  an MSA extra mode, so many MSA encodings still fall through to
  `__asm_*` (same as `ORI_B`).
- `SYSCALL` → existing pseudo-assembly call

Delay slots: `getDelaySlot()` / `hasDelaySlotLikely()` in `mips.cpp`. The
**decoder** moves the slot instruction; do not change that list without a
decoder task. Typical branches have a 1-instruction slot; `*L` likely forms
are marked separately.

---

## ABI

| File | Default CC | Syscall args | Special GPRs |
|------|------------|--------------|--------------|
| `abi/mips.cpp` | `CC_MIPS` (O32) | `A0`–`A3` | `SP`, `ZERO`, return/`syscall` `V0`; **GP, K0, K1** reserved |
| `abi/mips64.cpp` | `CC_MIPS64` (N64) | `A0`–`A3` plus `T0`–`T3` (`a4`–`a7`) | same GP/K0/K1 roles |
| `abi/pic32.cpp` | `CC_PIC32` | same as O32 | **must stay complete vs `mips.cpp`**: GP, K0, K1, SP, ZERO, V0, A0–A3 |

PIC32 extras (keep): `double` sized as `float` in `getTypeByteSize` /
`getTypeBitSize` (XC32 32-bit ABI). `getTypeBitSize` must call
`Abi::getTypeBitSize`, not `getTypeByteSize`.

There is no dedicated ABI member for GP/K0/K1; they are Capstone GPR ids.
The decoder seeds PIC32 `$gp` from the last store (`initializeGpReg_mips`).
`$k0`/`$k1` are XC32 interrupt scratch, not compiler temps.

---

## microMIPS gaps (do not fake)

`CS_MODE_MICRO` is a legal extra mode. Compact 16/32-bit encodings share
scalar Capstone ids **or** have their own (`AND16`, `B16`, `JR16`, …) and
are mapped to `nullptr` → generic `__asm_*` call.

**Not modelled** (non-exhaustive; all `nullptr` in `mips_init.cpp`):

- 16-bit ALU/branch: `AND16`, `ANDI16`, `OR16`, `XOR16`, `NOT16`, `SLL16`,
  `SRL16`, `ADDU16`, `SUBU16`, `B16`, `BEQZ16`, `BNEZ16`, `LI16`, `LW16`,
  `LBU16`, `LHU16`, `SW16`, `SB16`, `SH16`, `JR16`, `JALRS16`, `BREAK16`
- microMIPS multi-register and PC-relative: `LWM16`/`LWM32`, `SWM16`/`SWM32`,
  `LWP`/`SWP`, `ADDIUPC`, `LWPC`, `ALUIPC`, `AUIPC`, `ADDIUSP`, `JRADDIUSP`
- Compact (R6, no delay slot): `BC`, `BALC`, `BEQC`, `BNEC`, `BEQZC`,
  `BNEZC`, `JIC`, `JIALC`, `JRC`, `JALRC`, `BGEC`, `BLTC`, …

PIC32MZ and some XC32 objects are microMIPS. Until those ids are translated
for real, decompilation of compact code is **partial** even though the PIC32
**ABI** is Production. Do not alias `AND16` to `AND` without checking
operand layout and delay-slot rules.

---

## Out of production scope

- MSA beyond 128-bit bitwise (`AND_V`/`OR_V`/`XOR_V`/`NOR_V`): `LD_B`/
  `ST_B`, `ADDV_*`, `ORI_B`, FP MSA (`FADD_W`, …) stay on pseudo-assembly
  when a W register appears
- DSP / COP0 / TLB / `MFC0` / `CACHE`
- Unaligned `LWL`/`LWR`/`SWL`/`SWR` **are** modelled (including endianness);
  they are beyond the O32/N64 compiler subset listed above but already lifted
- FPU control word `CFC1`/`CTC1` (FCSR); R6 `SEL`/`CLASS`; paired-single
  `ADD_PS`

---

## Tests

Capstone 6 reports paired doubles as `D0`–`D15`, 64-bit FPRs as `D0_64`–
`D31_64`, and N64 GPRs as `ZERO_64` / `AT_64` / …. The translator folds
those onto `F0`–`F31` and the named 32-bit GPR ids before load/store.

`tests/capstone2llvmir/mips_tests.cpp` is instantiated for `CS_MODE_MIPS32`
and `CS_MODE_MIPS64`. A big-endian fixture covers `LWL`/`LWR`/`SWL`/`SWR` only.
Integer `div`/`divu` go in as encodings (`emulate_bin`); Keystone expands the
mnemonic. R6 encodings are not in the suite (Capstone 5.0.9 feature-bit quirk
above; Capstone 6 split ids are mapped).
