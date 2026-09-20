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
  `MTHC1`, `RECIP_S`/`D`, `RSQRT_S`/`D`, `CLASS_S`/`CLASS_D`,
  `RINT_S`/`RINT_D`, `MAXA_S`/`D`/`MINA_S`/`D`, `CTC1`/`CFC1` (FCR0–31),
  `BC1EQZ`/`BC1NEZ`, `SELEQZ_S/D`/`SELNEZ_S/D`, `DMFC1`/`DMTC1`,
  `MADDF_S`/`D`/`MSUBF_S`/`D` (unsuffixed `MADDF`/`MSUBF` are Capstone
  macros onto `_D`; do not shadow), indexed COP1
  `LWXC1`/`LDXC1`/`LUXC1`/`SWXC1`/`SDXC1`/`SUXC1`
- DSP ASE packed integer (`gcc -mdsp`, Capstone 6 **suffixed** ids; do not
  nullptr-shadow unsuffixed macros onto `_PH`/`_W`/`_QB`). Lanes live in
  32-bit GPRs (QB=4×i8, PH=2×i16, W=i32). Accumulators are Capstone
  `AC0`–`AC3` typed **i64** (`AC0_64` canonicalizes to `AC0`). `DSPCCOND`
  is i32 (4-bit pick/compare mask).
  - arith: `ADDQ_PH`/`ADDQ_S_*`/`ADDQH*`/`ADDUH*`/`ADDU_S_*`/`SUBQ*`/
    `SUBQH*`/`SUBUH*`/`SUBU_S_*`/`ABSQ_S_*` (wrap / sat like MSA `ADDS_S`)
  - compare/pick: `CMP_EQ/LE/LT_PH`, `CMPU_*_QB`, `CMPGU_*_QB`,
    `CMPGDU_*_QB`, `PICK_PH/QB`, `REPL_*`/`REPLV_*`
  - shift: `SHLL*`/`SHLL_S*`/`SHRA*`/`SHRA_R*`/`SHRL*` and `*V` forms
  - mul/MAC: `DPA*_W_PH`/`DPAQ*`/`DPAU*`/`DPAX*`/`DPS*`/`MAQ_*`/
    `MULQ_S/RS_*`/`MULEQ_S_*`/`MULEU_S_*`/`MULSA*`/`MTHLIP` on `AC0–3`
  - extract/pack: `EXTR*`/`EXTRV*`/`EXTP*`/`PRECR*`/`PRECRQ*`/`PACKRL_PH`/
    `PRECEQ*`/`PRECEQU*`/`PRECEU*`/`APPEND`/`PREPEND`/`BALIGN`
  - indexed load: `LBUX`/`LHX`/`LWX`
  - misc: `INSV`, `MODSUB`, `RADDU_W_QB`, `RDDSP`/`WRDSP`, `BPOSGE32`
    (delay slot; branch if `DSPPOS >= 32`), `ADDSC`/`ADDWC`
- R6 address: `LSA`/`DLSA` (`rd = (rs << sa) + rt`)
- MSA bitwise used by compilers: `AND_V`/`OR_V`/`XOR_V`/`NOR_V` as i128
  when Capstone reports those ids. Lane-wise `ADDV_B/H/W/D` and `SUBV_*`
  (x86 `PADDD` family) and whole-vector `LD_B/H/W/D` / `ST_*` (x86
  `MOVDQA`) are modelled the same way. Immediate byte logic `ANDI_B`/
  `ORI_B`/  `XORI_B`/`NORI_B`, `COPY_S`/`COPY_U` (x86 `PEXTR`), `INSERT_*`
  (x86 `PINSR`), `FILL_*`/`LDI_*` splats, immediate `ADDVI_*`/`SUBVI_*`,
  bit `BCLR*`/`BSET*`/`BNEG*`, `BINSL*`/`BINSR*`, `BMNZ_V`/`BMZ_V`/`BMNZI_B`/
  `BMZI_B`, `ADD_A_*`/`ASUB_S/U_*`/`AVE_*`/`AVER_*`, unsigned compares
  `CLT_U_*`/`CLE_U_*` (and `CLTI_*`/`CLEI_*`), packed `FADD`/`FSUB`/
  `FMUL`/`FDIV`/`FMAX`/`FMIN`/`FMAX_A`/`FMIN_A` `_W`/`_D`, quiet FP
  compares `FCEQ`/`FCLT`/`FCLE`/`FCNE`/`FCAF`/`FCOR`/`FCUN`/`FCUEQ`/
  `FCULE`/`FCULT`/`FCUNE` `_W`/`_D`, `DOTP_S/U_*`/`DPADD_*`/`DPSUB_*`,
  `HADD_S/U_*`/`HSUB_*`, `DIV_S/U_*`/`MOD_S/U_*`, `MOVE_V`, `BNZ_*`/`BZ_*`
  (compare-to-zero + existing cond-branch helper), `MUL_Q_*`/`MADD_Q_*`/
  `MSUB_Q_*`/`MULR_Q_*`/`MADDR_Q_*`/`MSUBR_Q_*`, `ADDS_A_*`, `CFCMSA`/
  `CTCMSA` (private i32 `MSACSR`/`MSAIR` and the other MSA control regs),
  `FCLASS_*` (10-bit IEEE class mask: bit0 SNAN, 1 QNAN, 2 −Inf, 3 −norm,
  4 −sub, 5 −0, 6 +0, 7 +sub, 8 +norm, 9 +Inf), `FMADD`/`FMSUB`,
  `FEXDO`/`FEXUPL`/`FEXUPR`, `FFINT_*`/`FFQL`/`FFQR`, `FLOG2`/`FEXP2`
  (`ws * 2^wt`), `FRCP`/`FRINT`/`FRSQRT`/`FSQRT` (packed; `llvm.sqrt`/
  `log2`/`exp2`), signaling `FS*`, `FTINT`/`FTQ`/`FTRUNC`, `MADDV`/`MSUBV`,
  `MAXI`/`MINI`/`MAX_A`/`MIN_A`/`MAX_U`/`MIN_U`, `NLOC`/`NLZC`/`PCNT`,
  `INSVE`, `VSHF`, `SRA_*`/`SLLI_*`/`SRAI_*`/`SRLI_*`, rounding
  `SRAR_*`/`SRARI_*`/`SRLR_*`/`SRLRI_*` (add `1<<(sa-1)` when `sa!=0`),
  and mixed-sign saturating `SUBSUS_U_*`/`SUBSUU_S_*` follow the x86
  SIMD subset. Unsuffixed `SLLI`/`SRAI`/`SRLI` are Capstone 6 macros
  onto `_W`; scalar `translateSll`/`Sra`/`Srl` dispatch to the MSA
  helper when a W register is present.
  Scalar COP1 `FADD_D` stays `translateAdd` when no W register is present.
  Default `createMips32` does not enable an MSA extra mode; Capstone 6
  still decodes these encodings under `CS_MODE_MIPS32`. Unmodelled MSA
  stays `__asm_*`.
- `SYSCALL` → existing pseudo-assembly call
- R6 CRC ASE (x86 `CRC32` analogue): `CRC32B`/`CRC32H`/`CRC32W`/`CRC32D` and
  Castagnoli `CRC32CB`/`CH`/`CW`/`CD`. LLVM 23.1.0 has no `llvm.mips.crc32*`
  (`IntrinsicsMips.h` / `IntrinsicsMips.td`); lifted as the reflected bit-loop
  matching ARM CRC32 (poly `0xEDB88320` / `0x82F63B78`). Capstone 6
  `build/windows/deps/install/capstone/include/capstone/mips.h` has **no**
  `MIPS_INS_AES*` / `MIPS_INS_SHA*` / `MIPS_INS_CRYPTO_*` / `MIPS_INS_PCLMUL*`
  (grep AES/SHA256/CRYPTO/PCLMUL: no insn IDs).

### microMIPS 16-bit (compiler / PIC32MZ) — real IR

Capstone operand layouts were checked on Capstone 6 (public `cs_mips` list,
not TableGen LLVM-op counts). 16-bit ALU that reports two GPRs is dest OP=
src (tied dest); three-operand `ADDU16`/`SUBU16`/`SLL16`/`SRL16` match the
32-bit helpers.

| ID | Translator | Capstone ops (live dump) |
|----|------------|--------------------------|
| `ADDIUR1SP` | `translateAddiur1sp` | `rd = SP + imm` (scaled by Capstone) |
| `ADDIUR2` / `ADDIUS5` | `translateAdd` | 3-op `rd = rs + imm` / 2-op `rd += imm` |
| `MOVEP` | `translateMovep` | 4 REG: `(rd, re) = (rs, rt)` |
| `LWXS` | `translateLwxs` | `rt = sext(Mem[base + (index << 2)])` |
| `BGEZALS` / `BLTZALS` | `translateBcondal` | **16-bit delay slot**; `$ra = PC + size + 2` |
| `JALS` | `translateJal` | same short-delay link |
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
| `DBITSWAP` | `translateBitswap` | same on i64 (Capstone 6 needs Mips64r6 to decode) |
| `LSA` / `DLSA` | `translateLsa` | `rd = (rs << sa) + rt`; Capstone IMM is the shift 1..4 |
| `ALIGN` / `DALIGN` | `translateAlign` | 4-op `rd, rs, rt, bp`; DALIGN is i64, bp 0..7 |
| `DAUI` / `DAHI` / `DATI` | `translateDaui` | `rt += imm << {16,32,48}`; Capstone 6 needs `CS_MODE_MIPS64R6` extra on `createMips64` |
| `LWPC` / `LWUPC` / `LDPC` | `translateLwpc` | PC + already-scaled byte offset; LWU zero-extends, LD is i64 |

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
| `SELEQZ_S` / `SELEQZ_D` / `SELNEZ_S` / `SELNEZ_D` | same helpers on FPR bit patterns |
| `BOVC` / `BNVC` | `translateBovc` (signed `rs+rt` overflow / not; **no delay slot**) |
| `SEL_S` / `SEL_D` | `translateSel` (COP1: `fd = lsb(ft) ? fs : fd`) |
| `CLASS_S` / `CLASS_D` | `translateClass` (10-bit IEEE mask into fd; unsuffixed `CLASS` is a compat macro onto `CLASS_D`) |
| `RINT_S` / `RINT_D` | `translateRint` (`llvm.nearbyint`; unsuffixed `RINT` → `RINT_D`) |
| `MAXA_S` / `MAXA_D` / `MINA_S` / `MINA_D` | `translateMaxa` (select original operand by `|x|`; unsuffixed macros → `_D`) |
| `BC1EQZ` / `BC1NEZ` / `BC1EQZC` / `BC1NEZC` | `translateBc1eqz` (FPR bits == 0 / != 0; **no delay slot**) |
| `CTC1` / `CFC1` | `translateCtc1` (GPR ↔ `FCR0`–`FCR31`; `FCR31` is FCSR) |

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

Requested leftover MSA / R6 compiler families from this batch are mapped
(no nullptr). Capstone 6 reports **suffixed** ids. Unsuffixed names are
compat macros onto `_W`/`_B`/`_D` — do not shadow them with a nullptr
(`MADDF`/`MSUBF` → `_D`). Do not wrap maps in `#ifdef MIPS_INS_*`.
Capstone 6 has no `MIPS_INS_ADD_PS`.

**Compiler leftovers (not DSP / COP0):**

- Traps: `TEQI`, `TGE`/`TGEI`/`TGEIU`/`TGEU`, `TLT`/`TLTI`/`TLTIU`/`TLTU`,
  `TNE`/`TNEI`
- `BTEQZ` / `BTNEZ`: Capstone 6 never reports these ids under
  `CS_MODE_MIPS32|MICRO` (full 16-bit scan) or a 32-bit first-halfword scan.
  No operand layout to lift.
- Octeon (need extra `CS_MODE_OCTEON`): `V3MULU`/`VMM0`/`VMULU`,
  `MTM0–2`/`MTP0–2`, `BBIT0`/`BBIT032`/`BBIT1`/`BBIT132`.
  Mapped: `POP`/`DPOP` (`llvm.ctpop`), `BADDU`, `CINS`/`CINS32`,
  `EXTS`/`EXTS32`.
- COP2/COP3: `BC2*`/`BC3*`, `DMFC2`/`DMTC2`/`MFC2`/`MTC2`,
  `LDC2`/`LWC2`/`LWC3`/`SDC2`/`SWC2`/`SWC3`
- Hints: `PAUSE`, `SSNOP`

**DSP leftovers / COP0 / CACHE (do not lift):**

- DSP remaining nullptr: unsuffixed `CMP`/`CMPI` (Capstone never reports
  those ids under MIPS32 / MIPS32R6 / MICRO). R6 `cmp.eq.s` is
  `MIPS_INS_CMP_EQ_S` (mapped, `translateR6Cmp`). `SHILO`/`SHILOV` are
  mapped. `BPOSGE32C` is unmapped (not in this Capstone header / not in
  `_i2fm`).
- COP0 / privileged: `MFC0`/`MTC0`/`DMFC0`/`DMTC0`, `CACHE`, `DI`/`EI`,
  `DERET`/`ERET`/`EHB`/`WAIT`, `TLBP`/`TLBR`/`TLBWI`/`TLBWR`,
  `BC0F`/`BC0FL`/`BC0T`/`BC0TL`, `SDBBP`/`SDBBP16`, `JALR_HB`/`JR_HB`

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
  `ADDIUPC`, `SELEQZ`/`SELNEZ`/`SEL_S` (delay slot still 0), R6 CRC
  `CRC32B`/`CRC32W`/`CRC32CB`, `CLASS_S`, `RINT_S`, `MAXA_S`/`MINA_S`,
  `BC1EQZ`/`BC1NEZ` (delay slot 0), `SELEQZ_S`/`SELNEZ_S`, `BOVC`/`BNVC`
- `CS_MODE_MICRO` fixture also: `ADDIUR1SP`/`ADDIUR2`/`ADDIUS5`, `MOVEP`,
  `LWXS`, `BGEZALS`/`BLTZALS`, `JALS`
- `createMips64` + `CS_MODE_MIPS64R6`: `DAUI`/`DAHI`/`DATI`/`DALIGN`,
  `LDPC`/`LWUPC`
- default suite: `LSA` (MIPS32), `DLSA` (MIPS64), `JALX`, `CFC1`/`CTC1`,
  `DMFC1`/`DMTC1`, indexed COP1 `LWXC1`/`LDXC1`/`LUXC1`/`SWXC1`/`SDXC1`/`SUXC1`
  (FCR load/store), MSA `ADDV_W` (lane wrap), `SUBV_W`, `LD_B`/`ST_B`,
  `ADDVI_W`/`SUBVI_W`, `BCLR_W`/`BSET_W`/`BNEG_W`/`BCLRI_W`, `BINSL_W`/
  `BINSR_W`, `BMNZ_V`/`BMZ_V`, `ADD_A_W`/`ASUB_S_W`/`ASUB_U_W`/`AVE_S_W`/
  `AVER_S_W`, `CLT_U_W`/`CLE_U_W`, MSA `FADD_D`/`FMAX_D`, `FMAX_A_W`/
  `FMIN_A_W`, `FCEQ_W`/`FCLT_W`, `DOTP_S_W`/`DPADD_S_W`, `HADD_S_W`/
  `HSUB_S_W`, `DIV_S_W`/`MOD_S_W`, `MOVE_V`, `MUL_Q_W`, `ADDS_A_W`,
  `CFCMSA`/`CTCMSA`, `BNZ_W`/`BZ_W`, `FCLASS_W`/`FMADD_W`/`FRCP_W`/
  `FSQRT_W`/`FSEQ_W`/`FFINT_S_W`/`FTINT_S_W`/`FEXUPR_D`/`MADDV_W`/
  `MAXI_S_W`/`MAX_A_W`/`MAX_U_W`/`NLZC_W`/`PCNT_W`/`NLOC_W`/`VSHF_W`/
  `INSVE_W`/`MULR_Q_W`/`FFQL_D`, DSP `ADDQ_PH`/`ADDU_S_QB`/`ADDQH_W`/
  `SUBQ_S_W`/`SHLL_PH`/`REPL_QB`/`PICK_PH`/`CMPGU_EQ_QB`/`CMP_EQ_PH`/
  `DPA_W_PH`/`EXTR_W`/`LBUX`/`LWX`/`ADDSC`/`ADDWC`/`MODSUB`/`INSV`/
  `BPOSGE32`/`RDDSP`/`APPEND`/`PREPEND`/`SHILO`/`SHILOV`

Integer `div`/`divu` go in as encodings (`emulate_bin`); Keystone expands the
mnemonic. Compact R6 delay-slot zeros are also asserted on the default
fixture (IDs only; no decode).
