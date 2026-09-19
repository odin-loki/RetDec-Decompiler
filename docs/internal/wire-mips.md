# Integrator notes — MIPS 32 / MIPS 64 / PIC32

Status of the **compiler O32 / N64 subset** lifted to LLVM IR. PIC32 is not
a separate Capstone architecture: it uses the MIPS translator plus `AbiPic32`.
Do **not** edit decoder / CLI / CMakeLists / `architecture.h` for this work.

Factories already exist: `createMips32`, `createMips64`, `createMips3`,
`createMips32R6`. Extra mode `CS_MODE_MICRO` is allowed and is required to
decode 16-bit microMIPS encodings (PIC32MZ / XC32). Compact R6 encodings
decode under `createMips32R6` (Capstone 6). They have **no delay slot**.

---

## Status

| Target | Status | Notes |
|--------|--------|-------|
| **MIPS 32-bit (O32)** | **Production** | `CS_MODE_MIPS32`. Word ALU, loads/stores, branches/jumps with delay slots, `MULT`/`DIV`+`MFLO`/`MFHI`, COP1 FPU, `SYSCALL` as pseudo-call. |
| **MIPS 64-bit (N64)** | **Production** | `CS_MODE_MIPS64`. Same subset; word ops sign-extend into 64-bit GPRs; `LD`/`SD` and `D*`-prefixed doubleword ALU. |
| **PIC32** | **Production (ISA)** | Same MIPS32 translator. ABI in `pic32.cpp` matches `mips.cpp` for SP/zero/V0/A0–A3 plus GP/K0/K1 roles. Enable `CS_MODE_MICRO` for XC32/PIC32MZ compact 16-bit encodings. |
| **MIPS32R6 compact** | **Production (compiler subset)** | `createMips32R6`. Compact branches (`BC`, `BEQC`, `BEQZC`, `JIC`, …) emit `__pseudo_branch` / cond-branch IR. `getDelaySlot()` is **0**. |

Capstone 5.0.9 ORs `CS_MODE_32` into `CS_MODE_MIPS32R6`, then the disassembler
takes the `CS_MODE_32` branch and **clears** `FeatureMips32r6`. R6 encodings
often do not decode on that pin. Capstone 6 (this tree) does decode them.

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
  when Capstone reports those ids. Lane-wise `ADDV_B/H/W/D` and `SUBV_*`
  (x86 `PADDD` family) and whole-vector `LD_B/H/W/D` / `ST_*` (x86
  `MOVDQA`) are modelled the same way. Immediate byte logic `ANDI_B`/
  `ORI_B`/`XORI_B`/`NORI_B`, `COPY_S`/`COPY_U` (x86 `PEXTR`), `INSERT_*`
  (x86 `PINSR`), `FILL_*`/`LDI_*` splats, and packed single `FADD_W`/
  `FSUB_W`/`FMUL_W`/`FDIV_W`/`FMAX_W`/`FMIN_W` follow the x86 SIMD subset.
  Default `createMips32` does not enable an MSA extra mode; Capstone 6
  still decodes these encodings under `CS_MODE_MIPS32`. Unmodelled MSA
  stays `__asm_*`.
- `SYSCALL` → existing pseudo-assembly call

### microMIPS 16-bit (compiler / PIC32MZ) — real IR

Capstone operand layouts were checked on Capstone 6 (public `cs_mips` list,
not TableGen LLVM-op counts). 16-bit ALU that reports two GPRs is dest OP=
src (tied dest); three-operand `ADDU16`/`SUBU16`/`SLL16`/`SRL16` match the
32-bit helpers.

| ID | Translator | Capstone ops (live dump) |
|----|------------|--------------------------|
| `AND16` | `translateAnd` | 2 GPR (dest &= src) |
| `ANDI16` | `translateAnd` | dest, imm |
| `OR16` | `translateOr` | 2 GPR |
| `XOR16` | `translateXor` | 2 GPR |
| `NOT16` | `translateNot` | 2 GPR |
| `SLL16` / `SRL16` | `translateSll` / `translateSrl` | 3-op |
| `ADDU16` / `SUBU16` | `translateAdd` / `translateSub` | 3 GPR |
| `LI16` | `translateLi` | dest = imm (**not** `translateAdd`) |
| `LW16` / `LBU16` / `LHU16` | `translateLoadMemory` | rt, mem |
| `SW16` / `SB16` / `SH16` | `translateStoreMemory` | rt, mem |
| `B16` | `translateJ` | target IMM |
| `BEQZ16` / `BNEZ16` | `translateCondBranchBinary` | rs, target |
| `JR16` | `translateJ` | rs |
| `JALRS16` | `translateJal` | delayed link |
| `BREAK16` | `translateBreak` | |

`B16` / `BEQZ16` / `BNEZ16` / `JR16` / `JALRS16` / `JRADDIUSP` **have a delay
slot** (`getDelaySlot() == 1`). Compact R6 does not.

### PC-relative / multi-reg — real IR

| ID | Translator | Notes |
|----|------------|-------|
| `ADDIUPC` / `ALIAS_LAPC` | `translateAddiupc` | `rt = PC + byte_offset` (Capstone reports offset, not absolute) |
| `AUIPC` | `translateAddiupc` | `rt = PC + (imm << 16)` |
| `ALUIPC` | `translateAddiupc` | `(PC + (imm << 16)) & ~0xFFFF` |
| `LWPC` | `translateLwpc` | word load from `PC + offset` |
| `ADDIUSP` | `translateAddiusp` | `SP += imm` |
| `JRADDIUSP` | `translateJraddiusp` | `SP += imm`; jump `$ra`; **delay slot** |
| `LWP` / `SWP` | `translateLoadPair` / `translateStorePair` | only if Capstone reports `reg, reg, mem`; else `__asm_*` |
| `LWM16` / `LWM32` / `SWM16` / `SWM32` | `translateLoadMultiple` / `translateStoreMultiple` | **Implement.** Capstone 6 expands `rlist` to `REG` + `is_reglist` (not a leftover IMM bitset). **LWM32** live dump, LE halfwords of yaml BE `23 20 5c 6d` → `20 23 6d 5c`: `REG s0..s7,fp,ra is_reglist=1` then `MEM base=zero disp=-0x393`. **LWM16** `00 45`: printer `lwm16 $s0, $ra, 0($sp)`; details `REG s0, ra is_reglist` but `MEM base=INVALID disp=24` (SP's enum, not the offset). **SWM16** `40 45`, same broken MEM. 16-bit forms therefore use spec `SP + SignExtend4(bits 3:0)<<2`; 32-bit uses Capstone MEM. Leftover IMM still `__asm_*`. |
| `AUI` | `translateAui` | `rt = rs + (imm << 16)` |
| `BITSWAP` | `translateBitswap` | per-byte bit reverse |
| `ALIGN` | `translateAlign` | 4-op `rd, rs, rt, bp` |

### Compact R6 — real IR, **no delay slot**

`getDelaySlot()` / `hasDelaySlot()` return 0 for all of these (they are not
in the delay-slot set). Compact-and-link (`BALC`, `JALRC`, `JIALC`, `*ALC`)
write `$ra = PC + size`, not `PC + 2*size`.

| ID | Translator |
|----|------------|
| `BC` / `JRC` / `JRC16` | `translateJ` |
| `BALC` / `JALRC` | `translateJal` |
| `JIC` / `JIALC` | `translateJic` (`GPR + offset`) |
| `BEQC` / `BNEC` / `BGEC` / `BLTC` / `BGEUC` / `BLTUC` | `translateCondBranchTernary` |
| `BEQZC` / `BNEZC` / `BGTZC` / `BLEZC` / `BGEZC` / `BLTZC` / `BEQZC16` / `BNEZC16` | `translateCondBranchBinary` |
| `BEQZALC` / `BNEZALC` / `BGEZALC` / `BLTZALC` / `BGTZALC` / `BLEZALC` | `translateBcondal` |
| `SELEQZ` / `SELNEZ` | `translateSeleqz` / `translateSelnez` (`rd = rs` or `0`; **no delay slot**) |
| `SEL_S` / `SEL_D` | `translateSel` (COP1: `fd = lsb(ft) ? fs : fd`) |

Delay slots: `getDelaySlot()` / `hasDelaySlotLikely()` in `mips.cpp`. The
**decoder** moves the slot instruction; do not change that list without a
decoder task. Typical (and microMIPS 16-bit delayed) branches have a
1-instruction slot; `*L` likely forms are marked separately.

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

## Still nullptr (do not fake)

**Overflow compact (not typical compiler ALU):** `BNVC`, `BOVC`.

**DSP / remaining MSA / COP0 / CACHE:**

- MSA beyond the modelled subset (`AND_V`/`OR_V`/`XOR_V`/`NOR_V`,
  `ADDV_*`/`SUBV_*`, `MULV_*`, `ILVEV_*`/`ILVOD_*`/`ILVL_*`/`ILVR_*`,
  `PCKEV_*`/`PCKOD_*`, `SHF_*`, `SPLAT_*`/`SPLATI_*`, `ANDI_B`/`ORI_B`/
  `XORI_B`/`NORI_B`, `BSEL_V`/`BSELI_B`, `CEQ_*`/`CEQI_*`/`CLE_S_*`/`CLT_S_*`,
  `MAX_S_*`/`MIN_S_*`, lane `SLL_*`/`SRL_*`, `COPY_S_*`/`COPY_U_*`,
  `INSERT_*`/`FILL_*`/`LDI_*`, saturating `ADDS_S_*`/`ADDS_U_*`/`SUBS_S_*`/
  `SUBS_U_*` (x86 PADDSB/PADDUSB/PSUBSB select-clamp), `SAT_S_*`/`SAT_U_*`,
  `SLD_*`/`SLDI_*` (PALIGNR-class; Capstone `SLD` is W, W, GPR),
  `FADD_W`/`FSUB_W`/`FMUL_W`/`FDIV_W`/`FMAX_W`/`FMIN_W`, `LD_*`/`ST_*`):
  remaining MSA (`FADD_D`, other compares/shuffles) stay on
  pseudo-assembly when a W register appears. Capstone 6 reports
  **suffixed** ids (`MULV_W`, `ORI_B`, `FADD_W`, `ANDI_B`, `ADDS_S_B`,
  `CEQI_B`, `SAT_S_B`, `SLD_B`, …). Unsuffixed names are Capstone 6
  compat macros onto `_W`/`_B`, so a leftover `{MIPS_INS_FADD, nullptr}`
  would shadow `FADD_W` (same lesson as `ADDV_B` / `MULV_W` / `NORI` onto
  `NORI_B` / `ADDS_S` onto `ADDS_S_W`). Do not wrap maps in
  `#ifdef MIPS_INS_ORI_B` — those enums are not macros.
- DSP / COP0 / TLB / `MFC0` / `CACHE`
- FPU control word `CFC1`/`CTC1` (FCSR); R6 FP `SELEQZ_S/D` / `CLASS`;
  paired-single `ADD_PS`

Unaligned `LWL`/`LWR`/`SWL`/`SWR` **are** modelled (including endianness);
they are beyond the O32/N64 compiler subset listed above but already lifted.

---

## Tests

Capstone 6 reports paired doubles as `D0`–`D15`, 64-bit FPRs as `D0_64`–
`D31_64`, and N64 GPRs as `ZERO_64` / `AT_64` / …. The translator folds
those onto `F0`–`F31` and the named 32-bit GPR ids before load/store.

`tests/capstone2llvmir/mips_tests.cpp` is instantiated for:

- `CS_MODE_MIPS32` and `CS_MODE_MIPS64` (default suite)
- big-endian fixture: `LWL`/`LWR`/`SWL`/`SWR`
- `CS_MODE_MICRO` fixture: `AND16`, `ADDU16`, `LW16`, `LWM16`/`LWM32`/`SWM16`
  via `emulate_bin` (Keystone 0.9.2 does not assemble microMIPS)
- `CS_MODE_MIPS32R6` fixture: `BEQZC`, `BC` (assert `getDelaySlot()==0`),
  `ADDIUPC`, `SELEQZ`/`SELNEZ`/`SEL_S` (delay slot still 0)
- default suite: MSA `ADDV_W` (lane wrap), `SUBV_W`, `LD_B`/`ST_B`

Integer `div`/`divu` go in as encodings (`emulate_bin`); Keystone expands the
mnemonic. Compact R6 delay-slot zeros are also asserted on the default
fixture (IDs only; no decode).
