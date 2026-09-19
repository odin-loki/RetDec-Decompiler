# Wire ARM / Thumb (32-bit) to Production

Capstone **5.0.9**. This note is for the integrator. The lifter/ABI work
in this change does **not** edit decoder, CLI, or `architecture.h`.

## What this change implemented

Integer compiler-used ARM and Thumb-2 now lift to real LLVM IR (not
`translatePseudoAsmGeneric`):

| Class | IDs now real IR |
| --- | --- |
| Data processing | AND, EOR, SUB, SUBS, SUBW, RSB, ADD, ADDW, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, ORN, MOV, MVN, BIC, NEG, shifts (LSL/LSR/ASR/ROR/RRX) |
| Load/store | LDR/STR/LDRB/STRB/LDRH/STRH and signed/unprivileged/exclusive/acquire variants, LDM/STM, PUSH/POP, PC-relative LDR |
| Branches / calls / returns | B, BL, BX, BLX, BXNS, BLXNS, CBZ, CBNZ, TBB, TBH; `BX LR` is a return |
| Multiply | MUL, MLA, MLS, UMULL/SMULL, UMLAL/SMLAL, SMULBB/BT/TB/TT, SMLABB/BT/TB/TT, SMULWB/WT, SMLAWB/WT |
| Extend | SXTB/SXTH/SXTAB/SXTAH/SXTB16/SXTAB16, UXTB/UXTH/UXTAB/UXTAH/UXTB16/UXTAB16 |
| Bitfield / pack / sat | UBFX/SBFX/BFI/BFC, PKHBT/PKHTB, SSAT/USAT, CLZ, REV/REV16/REVSH/RBIT |
| Thumb-2 control | IT (header is a no-op; predicated insns keep existing cond wrappers) |
| Hints / barriers | NOP/YIELD/PLD/PLI, HINT, DMB/DSB/ISB/DFB/ESB/TSB, CSDB |

Thumb interwork: `translateB`/`translateBl` now attach `arm.thumb_call`
metadata at lift time. `arm_thumb_interwork.cpp` uses Capstone 5.0.9
`ARM_INS_BX` / `ARM_INS_BLX` (the old hardcoded 14/13 were Capstone 4).

ABI (`AbiArm`): AAPCS return in `r0` (`_regFunctionReturnId`), GPRs
include `sp`/`lr`, NOP detection covers `HINT`/`YIELD`, `mov rN,rN`, and
the classic `andeq r0, r0, r0`.

Calling convention tables were already AAPCS-VFP complete in
`arm_conv.cpp` (not owned here).

## Remaining gaps vs x86-64 Production

Still **pseudo-asm** (or `nullptr` → generic pseudo):

- NEON integer/SIMD (`VADD` on Q, `VLD1`/`VST1`, most `V*` lane ops). Scalar VFP (`VLDR`/`VSTR`/`VADD.f32`/…) is already real IR.
- DSP dual-multiply (`SMLAD`, `SMUAD`, `SMUSD`, `UMAAL`, `USAD8`, …).
- Saturating scalar `QADD`/`QSUB`/`QDADD`/`QDSUB` and `SSAT16`/`USAT16`.
- Coprocessor `MCR`/`MRC` (except TPIDRURO), `MRS`/`MSR`, `SVC`/`UDF`/`BKPT`.
- Armv8-M `SG`, `SETPAN`, `TT*`, `BLXNS`/`BXNS` lift as ordinary call/branch (no security state).
- Table-branch decoder follow-through (TBB/TBH now emit a branch call; the decoder must treat them as switches).

End-to-end Production still needs the integrator files below: mode
switching after `BX`/`BLX`, ELF/PE ARM detection, and CLI `-a arm|thumb`
already exist but are not re-verified here.

## Files changed (this task)

- `src/capstone2llvmir/arm/arm.cpp`
- `src/capstone2llvmir/arm/arm_init.cpp`
- `src/capstone2llvmir/arm/arm_impl.h`
- `src/capstone2llvmir/arm/arm_thumb_interwork.cpp`
- `src/bin2llvmir/providers/abi/arm.cpp`
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

Existing `ARM_INS_USAT` now expects a saturated integer result (255 for
`usat r0, #8, r2` with `r2=0x5678`) instead of `__asm_usat`. `USAT16`
stays pseudo-asm.

## Verification

`arm.cpp`, `arm_init.cpp`, `arm_thumb_interwork.cpp`, `abi/arm.cpp`, and
`arm_tests.cpp` compile as C++ objects against LLVM **23.1.0** headers
(`getZero` / `getOrInsertDeclaration` / `MaybeAlign`). Linking
`retdec-tests-capstone2llvmir` and ctest were not run in this workspace:
`build/windows` currently has LLVM **8.0.0** headers in
`deps/install/llvm` (CMakeCache `LLVM_URL` still the Avast LLVM 8 zip).
Reconfigure from `cmake/deps.cmake` (`llvmorg-23.1.0`) before e2e ctest.
Do not ninja from a dirty ExternalProject stamp — it re-extracts LLVM 8.
