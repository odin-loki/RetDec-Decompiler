# Wire ARM / Thumb (32-bit) to Production

Capstone **6.0.0-Alpha10**. This note is for the integrator. The lifter/ABI work
in this change does **not** edit decoder, CLI, or `architecture.h`.

## What this change implemented

Integer compiler-used ARM and Thumb-2, scalar VFP, and compiler-used NEON now
lift to real LLVM IR (not `translatePseudoAsmGeneric`):

| Class | IDs now real IR |
| --- | --- |
| Data processing | AND, EOR, SUB, SUBS, SUBW, RSB, ADD, ADDW, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, ORN, MOV, MVN, BIC, shifts (LSL/LSR/ASR/ROR/RRX) |
| Load/store | LDR/STR/LDRB/STRB/LDRH/STRH and signed/unprivileged/exclusive/acquire variants, LDM/STM, PUSH/POP, PC-relative LDR |
| Branches / calls / returns | B, BL, BX, BLX, BXNS, BLXNS, CBZ, CBNZ, TBB, TBH; `BX LR` is a return |
| Multiply | MUL, MLA, MLS, UMULL/SMULL, UMLAL/SMLAL, SMULBB/BT/TB/TT, SMLABB/BT/TB/TT, SMULWB/WT, SMLAWB/WT, SMLAD/X, SMUAD/X, SMLSD/X, SMUSD/X, UMAAL, SMMUL/R, SMMLA/R, SMMLS/R, USAD8, USADA8 |
| Saturating scalar | QADD, QSUB, QDADD, QDSUB, SSAT, USAT, SSAT16, USAT16 |
| Extend | SXTB/SXTH/SXTAB/SXTAH/SXTB16/SXTAB16, UXTB/UXTH/UXTAB/UXTAH/UXTB16/UXTAB16 |
| Bitfield / pack | UBFX/SBFX/BFI/BFC, PKHBT/PKHTB, CLZ, REV/REV16/REVSH/RBIT |
| Thumb-2 control | IT (header is a no-op; predicated insns keep existing cond wrappers) |
| Hints / barriers | NOP/YIELD/PLD/PLI, HINT, DMB/DSB/ISB/DFB/ESB/TSB, CSDB |
| Scalar VFP | VLDR, VSTR, VADD/VSUB/VMUL/VDIV/VNMUL, VNEG/VABS/VSQRT, VMLA/VMLS/VFMA/…, VCMP/VCMPE, VMRS, VCVT/VCVTR, VCVTA/M/N/P (Capstone 6 leaves `vector_data` INVALID; inferred from S/D operands + mnemonic), VCVTB/T (IEEE f16 bit conversion; the interpreter cannot execute `half`), VRINTA/M/N/P/Z (same llvm.round/floor/nearbyint/ceil/trunc map as ARM64 FRINT*), VMOV (GPR↔FP and FP immediate), VPUSH/VPOP, VLDMIA/VLDMDB/VSTMIA/VSTMDB, VMSR |
| NEON (compiler-used) | VADD/VSUB/VMUL (integer and FP lanes on D/Q), VAND/VEOR/VORR/VBIC/VORN/VMVN/VBIT/VBIF/VBSL, VCEQ/VCGE/VCGT/VCLE/VCLT/VTST, VMAX/VMIN, VMAXNM/VMINNM (FP `llvm.maxnum`/`minnum`), VNEG/VABS (vector), VSHL/VSHR/VSRA, VSHLL (widen then shift, PMOVSX class), VQSHL/VQSHLU (saturating shift left), VDUP, VEXT, VSWP, VZIP/VUZP/VTRN, VTBL/VTBX, VREV16/32/64, VLD1/VST1, VLD2/VST2, VLD3/VST3, VLD4/VST4 (de-interleave / interleave), VMOV lane insert/extract, NEON `vcvt.s32.f32` on D/Q, VADDL/VSUBL (widen then add/sub), VADDW/VSUBW (ARM64 SADDW analogue), VMULL (widening mul), VADDHN (add then high-half narrow), VQMOVN (saturating narrow, PACKSSWB/SQXTN), VPADD/VPADDL (pairwise, HADDPS analogue), VPMAX/VPMIN (pairwise max/min), VQADD/VQSUB (PADDSB/PSUBSB analogue), VABD (absolute difference), VHADD/VRHADD (halving add, PAVGB-class) |

Thumb interwork: `translateB`/`translateBl` attach `arm.thumb_call`
metadata at lift time. `arm_thumb_interwork.cpp` uses Capstone 6
`ARM_INS_BX` / `ARM_INS_BLX`.

## Remaining gaps vs x86-64 Production

Still **pseudo-asm** (or `nullptr` → generic pseudo):

- Crypto (`AES*`/`SHA*`) — same bar as x86 (`AESENC` is nullptr).
- CRC32* — same bar as x86 (`X86_INS_CRC32` is nullptr).
- Coprocessor `CDP`/`MCR`/`MRC` (except TPIDRURO)/`LDC`/`STC`, plus `MRS`/`MSR`.
- `BKPT`/`HLT`/`HVC`/`SVC`/`UDF` — debug/exception; leave as svc-style pseudo.
- Remaining saturating SIMD (`VQDMULH`, `VQDMLAL`, doubling), rounding-sat shifts (`VQRSHRN`/`VQRSHRUN`/`VRSHR`), MVE extras.
- Directed FP rounding that still uses current-mode FPSCR (`VRINTR`/`VRINTX`); `VRINTA`/`M`/`N`/`P`/`Z` now lift with the same intrinsics as x86 `ROUNDSS` and ARM64 `FRINT*`.
- Armv8-M `SG`, `SETPAN`, `TT*`, `BLXNS`/`BXNS` lift as ordinary call/branch (no security state).
- Table-branch decoder follow-through (TBB/TBH now emit a branch call; the decoder must treat them as switches).

ABI (`AbiArm`): AAPCS return in `r0` (`_regFunctionReturnId`), GPRs
include `sp`/`lr`, NOP detection covers `HINT`/`YIELD`, `mov rN,rN`, and
the classic `andeq r0, r0, r0`.

Calling convention tables were already AAPCS-VFP complete in
`arm_conv.cpp` (not owned here).

End-to-end Production still needs the integrator files below: mode
switching after `BX`/`BLX`, ELF/PE ARM detection, and CLI `-a arm|thumb`
already exist but are not re-verified here.

## Files changed (this task)

- `src/capstone2llvmir/arm/arm.cpp`
- `src/capstone2llvmir/arm/arm_init.cpp`
- `src/capstone2llvmir/arm/arm_impl.h`
- `tests/capstone2llvmir/arm_tests.cpp`
- `docs/internal/wire-arm.md` (this file)

## Files the integrator must touch (do not edit here)

| File | Why |
| --- | --- |
| `src/bin2llvmir/optimizations/decoder/arm.cpp` | Dry-run ARM vs Thumb, `insnWrittesPc`, NOP leftovers. TBB/TBH are now control-flow IDs — confirm switch recovery. |
| `src/bin2llvmir/optimizations/decoder/decoder.cpp` | Mode switch logging; jump-target walk. |
| `src/bin2llvmir/optimizations/decoder/decoder_init.cpp` | `CS_ARCH_ARM`, Thumb symbol/`isThumb()` jump targets. |
| `src/bin2llvmir/optimizations/decoder/jump_targets.cpp` | Thumb bit-0 address alignment. |
| `src/bin2llvmir/optimizations/decoder/patterns.cpp` | Thumb pattern comments / pseudo-call patterns. |
| `include/retdec/common/architecture.h` | Already has `isArm32` / `isThumb` / `isArm32OrThumb`. Only touch if a new ARM variant is added. |
| `src/retdec/retdec.cpp` or config init | Ensure `AbiArm` + `ArmCallingConvention` are selected for ARM32/Thumb modules. |
| `src/retdec-decompiler/retdec-decompiler.cpp` | CLI `-a arm\|thumb` is already accepted; document Production status. |
| `src/fileformat/` ELF/PE/Mach-O ARM | `EM_ARM`, Thumb bit in symbols, interworking. |
| `src/bin2llvmir/providers/calling_convention/arm/arm_conv.cpp` | Already AAPCS-VFP; integrator only if a new ABI variant (AAPCS-soft vs hard) is split. |
| `src/bin2llvmir/optimizations/syscalls/arm.cpp` | `SVC` still pseudo-asm at the lifter; syscall pass matches `ARM_INS_SVC`. |
| `docs/ARCHITECTURE_TARGETS.md` | Flip ARM/Thumb from Partial to Production once e2e corpus is green. |
| Integration tests under `tests/` (bin2llvmir / decompiler) | Real ARM/Thumb ELFs, not only capstone2llvmir unit tests. |

Call `patchBxBlxCalls()` from the ARM decoder after a function is lifted
if `insn.id` metadata is present; the lifter now also annotates at
`BX`/`BLX` emission, so the post-pass is backup.

## Unit tests added (this task)

Both `CS_MODE_ARM` and `CS_MODE_THUMB` are parameterized. New names:

- `ARM_INS_ADDW_r_r_i`, `ARM_INS_SUBW_r_r_i`
- `ARM_INS_SDIV_r_r_r`, `ARM_INS_SDIV_by_zero_is_zero`, `ARM_INS_SDIV_int_min_over_minus_one`, `ARM_INS_SDIV_arm_bin`
- `ARM_INS_UDIV_r_r_r`, `ARM_INS_UDIV_arm_bin`
- `ARM_INS_TBB`, `ARM_INS_TBB_bin`, `ARM_INS_TBH`, `ARM_INS_TBH_bin`
- `ARM_INS_LDR_arm_pc_relative`, `ARM_INS_LDR_arm_pc_relative_bin`, `ARM_INS_LDR_thumb_pc_relative`
- `ARM_INS_BX_lr_is_a_return`
- `ARM_INS_SXTAB`, `ARM_INS_SXTAH`, `ARM_INS_UXTAB`
- `ARM_INS_PKHBT`, `ARM_INS_PKHTB`
- `ARM_INS_SSAT`, `ARM_INS_USAT_negative_saturates_to_zero`
- `ARM_INS_SMULBB`, `ARM_INS_SMLABB`
- `ARM_INS_NEG`
- `ARM_INS_IT_then_addeq_when_z_is_set`, `ARM_INS_IT_then_addeq_when_z_is_clear`
- `ARM_INS_BLX_register_is_annotated`
- `ARM_INS_VEXT_8`, `ARM_INS_VSWP`, `ARM_INS_VBSL`, `ARM_INS_VBIT`, `ARM_INS_VBIF`, `ARM_INS_VCGT_s32`, `ARM_INS_VCGE_s32`
- `ARM_INS_VZIP_8`, `ARM_INS_VUZP_8`, `ARM_INS_VTRN_8`
- `ARM_INS_VTBL_1reg`, `ARM_INS_VTBX_1reg`
- `ARM_INS_VCVTB_f32_f16`, `ARM_INS_VCVTB_f32_f16_thumb`, `ARM_INS_VCVTT_f16_f32`, `ARM_INS_VCVTT_f16_f32_thumb`, `ARM_INS_VCVTA_s32_f32`, `ARM_INS_VCVTA_s32_f32_thumb`
- `ARM_INS_VRINTA_f32`, `ARM_INS_VRINTA_f32_thumb`, `ARM_INS_VRINTM_f32`, `ARM_INS_VRINTM_f32_thumb`, `ARM_INS_VRINTN_f32`, `ARM_INS_VRINTN_f32_thumb`, `ARM_INS_VRINTP_f32`, `ARM_INS_VRINTP_f32_thumb`, `ARM_INS_VRINTZ_f32`, `ARM_INS_VRINTZ_f32_thumb`
- `ARM_INS_VCVT_s32_f32_neon`
- `ARM_INS_VADDL_u8`, `ARM_INS_VADDL_s8_is_signed`, `ARM_INS_VSUBL_s16`
- `ARM_INS_VPADD_i32`, `ARM_INS_VPADDL_s16`
- `ARM_INS_VQADD_s8_saturates`, `ARM_INS_VQADD_u8_saturates`
- `ARM_INS_VPMAX_u8`, `ARM_INS_VPMIN_u8`, `ARM_INS_VQSUB_s8_saturates`, `ARM_INS_VSHLL_s8`
- `ARM_INS_VADDHN_i16`, `ARM_INS_VMULL_s8`, `ARM_INS_VQMOVN_s16`, `ARM_INS_VADDW_s8`, `ARM_INS_VSUBW_s8`, `ARM_INS_VQSHL_s8_saturates`, `ARM_INS_VQSHLU_s8`, `ARM_INS_VABD_s8`, `ARM_INS_VHADD_s8`, `ARM_INS_VRHADD_u8`
- `ARM_INS_VLD2_32`, `ARM_INS_VST2_32`, `ARM_INS_VLD3_32`, `ARM_INS_VLD4_32`

x86 `PADDSB`/`PSUBSB` are mapped (`translateSseSaturatingArith`), so `VQADD`/`VQSUB` lift. x86 `ROUNDSS`/`ROUNDSD`/`ROUNDPS`/`ROUNDPD` (and 3-operand `VROUNDPS`/`VROUNDPD`) now lift, so ARM32 `VRINTA`/`M`/`N`/`P`/`Z` lift too. `AESENC` / `CRC32` stay nullptr, so AES/CRC stay nullptr. `VROUNDSS`/`VROUNDSD` stay nullptr (4-operand VEX). `VRINTR`/`VRINTX` stay nullptr (FPSCR-dynamic).

Existing `ARM_INS_USAT` now expects a saturated integer result (255 for
`usat r0, #8, r2` with `r2=0x5678`) instead of `__asm_usat`. `USAT16`
stays pseudo-asm.

## Verification

Build `tests-capstone2llvmir` after the C++ edits and run the ARM suite
(`--gtest_filter=InstantiateArmWithAllModes*`). Do not ninja from a dirty
ExternalProject stamp — it re-extracts LLVM 8.
