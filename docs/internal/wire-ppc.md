# PowerPC 32-bit and 64-bit — Production wiring

Status of the Capstone 6.0.0-Alpha10 → LLVM IR translator (`createPpc32` /
`createPpc64`) and the `bin2llvmir` ABI tables. Decoder, CLI, and
`architecture.h` are out of scope here.

Capstone 6 is opened as plain `CS_MODE_32`/`CS_MODE_64` plus endian.
`CS_MODE_PWR7` is a Capstone 6.0.0-Alpha10 trap: classic `fadd`/`fmul`/`lfd`
disassemble to nothing. VSX and Altivec that gcc `-O1` emits already decode
without a POWER* extra mode. Integer/control-flow encodings are unchanged.

Capstone 6 X-form (`lwzx`, `lfdx`, `lvx`, `stvx`, …) is often two operands:
dest + `PPC_OP_MEM` with `base` and `offset` (the old three-register form is
still accepted). `loadOp`/`storeOp` add `mem.offset`. RA=`r0` and Capstone's
`PPC_REG_ZERO` are the constant 0, not GPR0. 64-bit GPRs arrive as
`PPC_REG_X0..X31` and alias `R0..R31`. Indexed translators take the EA with
`lea=true` so they do not treat the MEM operand as a loaded value.

QPX is **not** translated. Capstone 6 still has `CS_MODE_QPX`, the `QVF*`
real instructions, and `PPC_REG_QF0..QF31` (compat: `PPC_REG_Q0`). The
old `PPC_INS_QVFAND` family is now `PPC_INS_ALIAS_QVF*` of `QVFLOGICAL`.
Those stay `nullptr`. `PPC_REG_VS0..VS63` were dropped; vs0–31 are
`PPC_REG_VSL0..VSL31`, vs32–63 are `PPC_REG_VSX32..VSX63` (aliased to
`V0..V31`).

## Production bar (this task)

Real LLVM IR (no `__asm_*` pseudo-call) for:

| Family | 32-bit (`CS_MODE_32`) | 64-bit (`CS_MODE_64`) |
|--------|----------------------|------------------------|
| `add` / `addi` / `addis` / `subf` / `and` / `or` / `xor` / `nand` | yes | yes (full GPR width) |
| `cmp` / `cmpi` / `cmpl` (+ `cmpw`/`cmpwi`/`cmplw`/`cmplwi`, `cmpd`/`cmpdi`/`cmpld`/`cmpldi`) | yes | yes; word forms narrow to i32; primary 4-op form honours `L` |
| `lwz`/`stw`/`lbz`/`stb`/`lhz`/`sth` | yes | yes (zero-extended into the 64-bit GPR) |
| `ld`/`std` | encodings do not exist | yes |
| `b` / `bl` / `blr` / `bctr` / `bc` | yes | yes |
| `mflr`/`mtlr`/`mfctr`/`mtctr` | yes | yes (LR/CTR are i64) |
| `rlwinm` / `rlwnm` / `rlwimi` | yes (32-bit rotate + mask) | yes (low word, result zero-extended) |
| `rldicl` / `rldicr` / `rldic` / `rldimi` / `rldcl` / `rldcr` | n/a | yes |
| compiler aliases `clrldi` / `rotldi` / `rotld` / `sldi` | n/a | yes |
| compiler FP `fadd`/`fadds`/`fmul`/`fmuls`/`fsub`/`fdiv`/`fcmpu`/`fcmpo`/`lfd`/`stfd`/`lfs`/`stfs` (+ indexed/`u`) | yes | yes |
| `lfiwax`/`lfiwzx`/`stfiwx` | yes | yes |
| VSX scalar gcc `-O1`: `xsadddp`/`xssubdp`/`xsmuldp`/`xsdivdp` (+ `*sp`), `xscmpudp`/`xscmpodp`, `lxsdx`/`stxsdx` | yes | yes |
| VSX move/logical: `xxlor`/`xxland`/`xxlxor`/`xxlnor`/…, `xvmovdp`/`xvmovsp` | yes | yes |
| `xxpermdi`, `xxspltw`, `lxvd2x`/`stxvd2x` | yes | yes |
| Altivec gcc `-O1`: `vand`/`vor`/`vxor`/`vandc`/`vnor`, `lvx`/`stvx`, `vspltisw`/`vspltish`/`vspltisb`, `vaddfp`/`vsubfp`, `vadduwm`/`vadduhm`/`vaddubm` | yes | yes |

## Mapped Capstone 6 IDs (this lift)

Compiler FP (already wired, kept): `PPC_INS_FADD`, `FADDS`, `FMUL`, `FMULS`,
`FSUB`, `FSUBS`, `FDIV`, `FDIVS`, `FCMPU`, `LFD`, `LFDU`, `LFDX`, `LFDUX`,
`STFD`, `STFDU`, `STFDX`, `STFDUX`, `LFS`/`STFS` family.

Added:

| ID | translator |
|----|------------|
| `PPC_INS_FCMPO` | `translateFcmp` |
| `PPC_INS_LFIWAX`, `LFIWZX` | `translateLoadFloatAsInt` |
| `PPC_INS_STFIWX` | `translateStoreFloatAsInt` |
| `PPC_INS_XSADDDP`, `XSSUBDP`, `XSMULDP`, `XSDIVDP` | `translateFpArithm` |
| `PPC_INS_XSADDSP`, `XSSUBSP`, `XSMULSP`, `XSDIVSP` | `translateFpArithm` (round to single) |
| `PPC_INS_XSCMPUDP`, `XSCMPODP` | `translateFcmp` |
| `PPC_INS_LXSDX` | `translateLoadFloatIndexed` |
| `PPC_INS_STXSDX` | `translateStoreFloatIndexed` |
| `PPC_INS_LXVD2X` | `translateVecLoadIndexed` |
| `PPC_INS_STXVD2X` | `translateVecStoreIndexed` |
| `PPC_INS_XXLOR`, `XXLAND`, `XXLXOR`, `XXLNOR`, `XXLANDC`, `XXLORC`, `XXLEQV`, `XXLNAND` | `translateVecLogical` |
| `PPC_INS_XVMOVDP`, `XVMOVSP` | `translateVsxMove` |
| `PPC_INS_XXPERMDI` | `translateXxpermdi` |
| `PPC_INS_XXSPLTW` | `translateXxspltw` |
| `PPC_INS_VAND`, `VANDC`, `VOR`, `VORC`, `VXOR`, `VNOR`, `VEQV`, `VNAND` | `translateVecLogical` |
| `PPC_INS_LVX`, `LVXL` | `translateVecLoadIndexed` |
| `PPC_INS_STVX`, `STVXL` | `translateVecStoreIndexed` |
| `PPC_INS_VSPLTISB`, `VSPLTISH`, `VSPLTISW` | `translateVecSplatImm` |
| `PPC_INS_VADDFP`, `VSUBFP` | `translateVecFpArith` |
| `PPC_INS_VADDUBM`, `VADDUHM`, `VADDUWM` | `translateVecIntAdd` |

## Tests

`tests/capstone2llvmir/powerpc_tests.cpp` is instantiated with
`CS_MODE_32` and `CS_MODE_64` (`createPpc32` / `createPpc64`, big-endian).

- Integer ALU, word loads/stores, branches, LR/CTR, `rlwinm`, `cmp*` run in
  **both** modes (`ALL_MODES`).
- `ld`/`std`/`ldx`/`stdx` and the `rldicl` family are `ONLY_MODE_64`.
- Compiler FP and the Altivec/VSX encodings above are `ALL_MODES`.
- No tests were deleted, skipped, or `DISABLED_`.

## ABI (what the abi files claim)

### 32-bit SYSV (`AbiPowerpc`, `CC_POWERPC`)

Implemented in `src/bin2llvmir/providers/abi/powerpc.cpp` plus
`PowerPCCallingConvention` (not edited here):

- `r1` stack pointer.
- Integer args `r3`–`r10`; returns `r3`/`r4`.
- FP args `f1`–`f8`; FP return `f1`.
- Linux `sc`: number in `r0`, args `r3`–`r8`, result in `r3`.
- Link register is the hardware `LR` (not a GPR); `mflr`/`mtlr` lift it.

### 64-bit (`AbiPowerpc64`, `CC_POWERPC64`)

Same GPR roles (`r1` SP, `r3`–`r10` args, `r3` return, `LR`, Linux `sc` in
`r0`/`r3`–`r8`). One calling-convention object is used for both ELFv1 and
ELFv2.

## Remaining ABI gaps (honest)

These are **not** modelled. They live in calling-convention code this task
does not own, or require decoder/config work:

- **ELFv1 vs ELFv2.** No function descriptors (ELFv1 `.opd`). No ELFv2 `r12`
  for global-entry/PLT. TOC pointer `r2` is not an ABI-tracked register.
- **64-bit FP args.** `PowerPC64CallingConvention` lists `f1`–`f8`, `f10`,
  `f11` — it skips `f9` and stops short of ELFv2’s `f1`–`f13`.
- **AIX / Darwin / Windows PowerPC.** Not implemented. Syscall layout above
  is Linux.
- **Vararg / homogeneous aggregates / `va_list`.** Not specialised.
- **Vector args** (Altivec `v2`–`v13`, VSX). No CC entries.

## Remaining translator gaps (Capstone 6, not this lift)

`*Powerpc*` is 691 pass / 319 fail (1010 total). The production-bar families
above pass. The remainder is almost all pre-existing Capstone 6 decode vs
the tests’ Keystone strings:

- CTR decrement branches (`bdnz*` / `bdz*` / `bdnza` / `bdza`) — Capstone 6
  dropped `PPC_INS_BDNZA`/`BDZA`; `ppc_bc` is `pred_cr`/`crX`, not `PPC_OP_CRX`.
- Aliases: `li`/`lis`/`mr`/`not`/`mtcr`/`crnot`/`crmove`/`slwi`/`srwi`/`rotlw*`.
- Rotates/masks: `rlwinm`/`rlwnm`/`rldicl`/`rldicr`/`clrldi`/`rotldi`/`sldi`.
- Update-indexed (`lbzux`/`lwzux`/`stbux`/`stwux`, …) — EA is correct, RA
  write-back still looks for a REG operand that Capstone 6 folded into MEM.
- A few ALU (`subf`/`subfc` on 32-bit) and CR-logical (`crset`/`crclr`).

QPX stays `nullptr`. Packed VSX `xv*` other than the move aliases, SPE,
most remaining Altivec (`vperm`, `vsel`, saturating arithmetic) stay
pseudo-asm or `nullptr`.

On 64-bit specifically: `divd`/`divdu`, `mulld`/`mulhd`, `sld`/`srd`/`srad`,
`cntlzd`, `popcntd`. Compilers usually spell 64-bit shifts as `rldicl`/
`rldicr` (now lifted), so those nullptr entries are less common in C code
than the MD-form family was.

**FPR / VSL overlay:** `f0` is still its own `double` global (existing FP
tests). `VSL0` is a separate i128. A VSX op that names `VSL0` inserts the
high doubleword; it does not rewrite `F0`, so mixed FPR/VSX views of the
same hardware register can drift. gcc `-O1` scalar VSX typically names the
FPR (`xsadddp` on `f1`).
