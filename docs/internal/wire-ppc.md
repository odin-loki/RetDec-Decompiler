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

QPX is translated for compiler-used `QVF*` arithmetic. Capstone 6 has
`CS_MODE_QPX`, the `QVF*` real instructions, and `PPC_REG_QF0..QF31`
(compat: `PPC_REG_Q0`) as i256 (`<4 x double>`). `QVFLOGICAL` is a 2-input
4-bit LUT (`TT` 0–15); `PPC_INS_ALIAS_QVFAND` and friends dispatch through
`alias_id`. `PPC_REG_VS0..VS63` were dropped; vs0–31 are
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
| compiler 64-bit GPR arith `divd`/`divdu`, `mulld`/`mulhd`/`mulhdu`, `sld`/`srd`/`srad`/`sradi`, `cntlzd`, `popcntd` | n/a | yes |
| compiler FP `fadd`/`fadds`/`fmul`/`fmuls`/`fsub`/`fdiv`/`fcmpu`/`fcmpo`/`lfd`/`stfd`/`lfs`/`stfs` (+ indexed/`u`) | yes | yes |
| `lfiwax`/`lfiwzx`/`stfiwx` | yes | yes |
| VSX scalar gcc `-O1`: `xsadddp`/`xssubdp`/`xsmuldp`/`xsdivdp` (+ `*sp`), `xscmpudp`/`xscmpodp`, `lxsdx`/`stxsdx` | yes | yes |
| VSX packed gcc `-O2`: `xvadddp`/`xvsubdp`/`xvmuldp`/`xvdivdp` (+ `*sp`) | yes | yes |
| VSX move/logical: `xxlor`/`xxland`/`xxlxor`/`xxlnor`/…, `xvmovdp`/`xvmovsp` | yes | yes |
| `xxpermdi`, `xxspltw`, `lxvd2x`/`stxvd2x` | yes | yes |
| Altivec gcc `-O1`: `vand`/`vor`/`vxor`/`vandc`/`vnor`, `lvx`/`stvx`, `vspltisw`/`vspltish`/`vspltisb`, `vaddfp`/`vsubfp`, `vadduwm`/`vadduhm`/`vaddubm` | yes | yes |
| Altivec gcc `-O2`: `vperm`, `vsel`, `vsldoi`, `vmrghw`/`vmrglw`, `vcmpequw`/`vcmpgtuw`, `vspltw`/`vsplth`/`vspltb`, `vsububm`/`vsubuhm`/`vsubuwm`, `lvsl`/`lvsr`, `vpkuhum`/`vpkuwum`, `vsl`/`vsr` | yes | yes |

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
| `PPC_INS_LXVD2X`, `LXVW4X` | `translateVecLoadIndexed` |
| `PPC_INS_LXVDSX` | `translateVecLoadIndexed` (splat 64-bit load) |
| `PPC_INS_STXVD2X`, `STXVW4X` | `translateVecStoreIndexed` |
| `PPC_INS_MFFS`, `MTFSF`, `MTFSFI`, `MTFSB0`, `MTFSB1`, `MCRFS` | `translateFpscr` (private `fpscr` i32) |
| `PPC_INS_MFVSCR`, `MTVSCR` | `translateVscr` (private `vscr` i32) |
| `PPC_INS_XXLOR`, `XXLAND`, `XXLXOR`, `XXLNOR`, `XXLANDC`, `XXLORC`, `XXLEQV`, `XXLNAND` | `translateVecLogical` |
| `PPC_INS_XVMOVDP`, `XVMOVSP` | `translateVsxMove` |
| `PPC_INS_XXPERMDI` | `translateXxpermdi` |
| `PPC_INS_XXSPLTW` | `translateXxspltw` |
| `PPC_INS_VAND`, `VANDC`, `VOR`, `VORC`, `VXOR`, `VNOR`, `VEQV`, `VNAND` | `translateVecLogical` |
| `PPC_INS_LVX`, `LVXL` | `translateVecLoadIndexed` |
| `PPC_INS_STVX`, `STVXL` | `translateVecStoreIndexed` |
| `PPC_INS_VSPLTISB`, `VSPLTISH`, `VSPLTISW` | `translateVecSplatImm` |
| `PPC_INS_VADDFP`, `VSUBFP` | `translateVecFpArith` |
| `PPC_INS_XVADDDP`, `XVSUBDP`, `XVMULDP`, `XVDIVDP` | `translateVecFpArith` (`<2 x double>`) |
| `PPC_INS_XVADDSP`, `XVSUBSP`, `XVMULSP`, `XVDIVSP` | `translateVecFpArith` (`<4 x float>`) |
| `PPC_INS_XVMADDADP`, `XVMADDASP`, `XVMADDMDP`, `XVMADDMSP` | `translateVecFpFma` (type-A dest addend vs type-M dest multiplier) |
| `PPC_INS_XVMSUBADP`, `XVMSUBASP`, `XVMSUBMDP`, `XVMSUBMSP` | `translateVecFpFma` |
| `PPC_INS_XVNMSUBADP`, `XVNMSUBASP`, `XVNMSUBMDP`, `XVNMSUBMSP` | `translateVecFpFma` |
| `PPC_INS_XVNMADDADP`, `XVNMADDASP`, `XVNMADDMDP`, `XVNMADDMSP` | `translateVecFpFma` (`-(product + addend)`, type-A vs type-M) |
| `PPC_INS_XVABSDP`, `XVABSSP`, `XVNEGDP`, `XVNEGSP` | `translateVecFpSign` (AND/XOR sign bit; x86 ANDPS/XORPS) |
| `PPC_INS_XVNABSDP`, `XVNABSSP` | `translateVecFpSign` (OR sign bit = `-abs`) |
| `PPC_INS_XVCPSGNDP`, `XVCPSGNSP` | `translateVecFpSign` (AND/XOR: magnitude of XB, sign of XA) |
| `PPC_INS_XVMAXDP`, `XVMINDP`, `XVMAXSP`, `XVMINSP` | `translateVecFpArith` (x86 MAXPD/MINPD `fcmp`+`select`) |
| `PPC_INS_VCFSX` | `translateVecFpConvert` (signed word → SP; x86 CVTDQ2PS). Capstone IMM is UIM; gcc `-O2` is UIM=0. Other UIM scales by `2^-UIM`. |
| `PPC_INS_VCFUX` | `translateVecFpConvert` (unsigned word → SP). UIM as `VCFSX`. |
| `PPC_INS_VCTUXS` | `translateVecFpConvert` (SP → unsigned word saturate, chop; x86 CVTTPS2DQ unsigned). UIM=0 in gcc `-O2`; other UIM scales by `2^UIM`. |
| `PPC_INS_VCTSXS` | `translateVecFpConvert` (SP → signed word saturate, chop). UIM as `VCTUXS`. |
| `PPC_INS_VRFIN`, `VRFIZ`, `VRFIP`, `VRFIM` | `translateVecFpRound` (nearest-even / trunc / ceil / floor; x86 ROUNDPS) |
| `PPC_INS_XVRDPI`, `XVRDPIC`, `XVRDPIM`, `XVRDPIP`, `XVRDPIZ` | `translateVecFpRound` (`<2 x double>`; nearbyint / nearbyint / floor / ceil / trunc) |
| `PPC_INS_VRLB`, `VRLH`, `VRLW` | `translateVecShift128` (per-element rotate; count is low bits of each `vrb` lane, Capstone `vrt,vra,vrb`) |
| `PPC_INS_VADDUBM`, `VADDUHM`, `VADDUWM` | `translateVecIntAdd` |
| `PPC_INS_VADDSBS`, `VADDSHS`, `VADDSWS`, `VADDUBS`, `VADDUHS`, `VADDUWS` | `translateVecIntAdd` (saturating select, PADDSB-class) |
| `PPC_INS_VSUBSBS`, `VSUBSHS`, `VSUBSWS`, `VSUBUBS`, `VSUBUHS`, `VSUBUWS` | `translateVecIntAdd` (saturating) |
| `PPC_INS_VPERM` | `translateVecPerm` |
| `PPC_INS_VSEL` | `translateVecSel` |
| `PPC_INS_VSLDOI` | `translateVecSldoi` |
| `PPC_INS_VSL`, `VSR` | `translateVecShift128` (whole-register logical shift 0–7) |
| `PPC_INS_VSLB`, `VSLH`, `VSLW`, `VSLD`, `VSRB`, `VSRH`, `VSRW`, `VSRD`, `VSRAB`, `VSRAH`, `VSRAW`, `VSRAD` | `translateVecShift128` (per-element; count masked to element width; `VSLD`/`VSRD` are 64-bit lanes) |
| `PPC_INS_VSPLTB`, `VSPLTH`, `VSPLTW` | `translateVecSplat` |
| `PPC_INS_VSUBUBM`, `VSUBUHM`, `VSUBUWM` | `translateVecIntAdd` (subtract) |
| `PPC_INS_LVSL`, `LVSR` | `translateVecLvsl` (permute control from `EA & 15`; no memory access) |
| `PPC_INS_VPKUHUM`, `VPKUWUM` | `translateVecPack` (modulo truncate) |
| `PPC_INS_VPKSHSS`, `VPKUHUS`, `VPKUWUS`, `VPKSHUS`, `VPKSWSS`, `VPKSWUS` | `translateVecPack` (signed/unsigned saturate; PACKSSWB / unsigned PACKUSWB-class) |
| `PPC_INS_VMRGHW`, `VMRGLW`, `VMRGHB`, `VMRGHH`, `VMRGLB`, `VMRGLH` | `translateVecMerge` (interleave high/low 8/16/32-bit lanes) |
| `PPC_INS_VCMPEQUW`, `VCMPGTUW` | `translateVecCmp` |
| `PPC_INS_VCMPEQUB`, `VCMPEQUH`, `VCMPEQUD`, `VCMPEQUQ` | `translateVecCmp` (byte/half/dword/quad equality mask) |
| `PPC_INS_ISEL`, `ALIAS_ISELLT`/`ISELGT`/`ISELEQ` | `translateIsel` (`rT = crb ? rA : rB`; CR bits are i1 globals) |
| `PPC_INS_CMPB` | `translateCmpb` (byte equality → `0xFF`/`0x00`; x86 PCMPEQB on GPRs) |
| `PPC_INS_DIVDE`, `DIVDEU` | `translateDivde` (i128 `RA||0` / `RB`; 64-bit quotient). Overflow forms `DIVDEO`/`DIVDEUO` stay unmapped. |
| `PPC_INS_VCLZB`, `VCLZH`, `VCLZW`, `VCLZD` | `translateVecClz` (`llvm.ctlz`, ARM VCLZ) |
| `PPC_INS_VAVGSB`, `VAVGSH`, `VAVGSW`, `VAVGUB`, `VAVGUH`, `VAVGUW` | `translateVecAvg` (rounding `(a+b+1)>>1`; x86 PAVGB) |
| `PPC_INS_LVEBX`, `LVEHX`, `LVEWX`, `STVEBX`, `STVEHX`, `STVEWX` | `translateVecElementLoadStore` (one BE element at `EA`; other lanes 0 on load) |
| `PPC_INS_XVRSQRTEDP`, `XVRSQRTESP`, `VRSQRTEFP` | `translateVecFpEstimate` (`1/sqrt`; no `llvm.ppc.vsx.xvrsqrtedp` on this LLVM) |
| `PPC_INS_VREFP`, `XVREDP`, `XVRESP` | `translateVecFpEstimate` (`1/x`) |
| `PPC_INS_XVSQRTDP`, `XVSQRTSP` | `translateVecFpEstimate` (per-lane `llvm.sqrt`) |
| `PPC_INS_VCMPGEFP`, `VCMPGTSB/D/H/W`, `VCMPGTUB/D/H` | `translateVecCmp` |
| `PPC_INS_VCMPBFP` | `translateVecCmp` (bounds: `0` / `0x80000000` / `0x40000000` / `0xC0000000`) |
| `PPC_INS_VADDUDM`, `VSUBUDM` | `translateVecIntAdd` (`<2 x i64>`) |
| `PPC_INS_VADDCUW`, `VSUBCUW` | `translateVecIntAdd` (unsigned carry/borrow-out as 0/1 words) |
| `PPC_INS_VRLD` | `translateVecShift128` (64-bit rotate lanes) |
| `PPC_INS_VSLO`, `VSRO` | `translateVecShift128` (octet shift from `VRB[121:124]`; x86 PSLLDQ) |
| `PPC_INS_VPOPCNTB/H/W/D` | `translateVecPopcnt` (`llvm.ctpop`; ARM VCNT) |
| `PPC_INS_VMAXFP`, `VMINFP` | `translateVecFpArith` |
| `PPC_INS_VMAXS*`, `VMAXU*`, `VMINS*`, `VMINU*` | `translateVecMinMax` (x86 PMAXSD/PMINUB) |
| `PPC_INS_VMADDFP`, `VNMSUBFP` | `translateVecFpFma` (`a*c±b`) |
| `PPC_INS_XVCVSPDP`, `XVCVDPSP` | `translateVecFpConvert` (even-word fpext / fptrunc) |
| `PPC_INS_XVCVSXWSP`, `XVCVUXWSP`, `XVCVSPSXWS`, `XVCVSPUXWS` | `translateVecFpConvert` (i32↔f32) |
| `PPC_INS_XVCVSXDDP`, `XVCVUXDDP`, `XVCVDPSXDS`, `XVCVDPUXDS` | `translateVecFpConvert` (i64↔f64) |
| `PPC_INS_XVCVDPSXWS`, `XVCVDPUXWS` | `translateVecFpConvert` (DP → i32 in even words) |
| `PPC_INS_XVCVSPSXDS`, `XVCVSPUXDS` | `translateVecFpConvert` (even SP → i64) |
| `PPC_INS_XVCVSXDSP`, `XVCVUXDSP` | `translateVecFpConvert` (i64 → SP in even words) |
| `PPC_INS_XVCVSXWDP`, `XVCVUXWDP` | `translateVecFpConvert` (even i32 → DP) |
| `PPC_INS_VEXPTEFP`, `VLOGEFP` | `translateVecFpEstimate` (per-lane `llvm.exp2` / `llvm.log2` on `<4 x float>`) |
| `PPC_INS_VMULE*`, `VMULO*`, `VMULUWM` | `translateVecMulWiden` (even/odd lane × wider dest; `VMULUWM` is 4×i32) |
| `PPC_INS_VMSUM*`, `VMHADDSHS`, `VMHRADDSHS`, `VMLADDUHM` | `translateVecMsum` (VA-form products+addend; `*S` saturate). No `VMSUMMHM` ID. |
| `PPC_INS_VPKPX` | `translateVecPack` (word xRGB8888 → 1-5-5-5) |
| `PPC_INS_VUPKHSB/LSB/HSH/LSH`, `VUPKHPX`, `VUPKLPX` | `translateVecUnpack` (sign-extend high/low half; pixel 1555→8888) |
| `PPC_INS_VSUM2SWS`, `VSUM4SBS`, `VSUM4SHS`, `VSUM4UBS`, `VSUMSWS` | `translateVecSum` (saturating horizontal sums) |
| `PPC_INS_XVTDIVDP/SP`, `XVTSQRTDP/SP` | `translateVecFpTest` (CR field, default CR6: EQ=all finite & ready, SO=!EQ). No FPSCR global. |
| `PPC_INS_XVRSPI*` | `translateVecFpRound` (SP analogue of `XVRDPI*`) |
| `PPC_INS_VCMPEQFP`, `VCMPGTFP`, `XVCMPEQDP`, `XVCMPEQSP`, `XVCMPGEDP`, `XVCMPGESP`, `XVCMPGTDP`, `XVCMPGTSP` | `translateVecCmp` (ordered FP compare → all-1s/0s lanes) |
| `PPC_INS_DIVD`, `DIVDU` | `translateDivw` (64-bit width) |
| `PPC_INS_MULLD` | `translateMullw` (64-bit width) |
| `PPC_INS_MULHD`, `MULHDU` | `translateMulhw` (high half of i128 product) |
| `PPC_INS_SLD` | `translateShiftLeft` (64-bit; count ≥ 64 → 0) |
| `PPC_INS_SRD` | `translateShiftRight` (64-bit; count ≥ 64 → 0) |
| `PPC_INS_SRAD`, `SRADI` | `translateSraw` (64-bit; CA as for `sraw`) |
| `PPC_INS_CNTLZD` | `translateCntlzw` (64-bit `ctlz`) |
| `PPC_INS_POPCNTD`, `POPCNTW` | `translatePopcnt` |
| `PPC_INS_QVFADD`, `QVFADDS`, `QVFSUB`, `QVFSUBS`, `QVFMUL`, `QVFMULS` | `translateQpxFpArith` (`<4 x double>`; `*S` via `<4 x float>` then fpext) |
| `PPC_INS_QVFMADD`, `QVFMADDS`, `QVFMSUB`, `QVFMSUBS`, `QVFNMADD`, `QVFNMADDS`, `QVFNMSUB`, `QVFNMSUBS` | `translateQpxFpFma` (fmul+fadd/fsub; FRT = FRA * FRC ± FRB) |
| `PPC_INS_QVFABS`, `QVFNEG`, `QVFNABS`, `QVFMR`, `QVFCPSGN` | `translateQpxFpSign` |
| `PPC_INS_QVFCMPEQ`, `QVFCMPGT`, `QVFCMPLT` | `translateQpxFpCmp` (1.0 / −1.0 lanes) |
| `PPC_INS_QVFSEL` | `translateQpxFpSel` (`FRA >= 0 ? FRC : FRB`) |
| `PPC_INS_QVLFDX`, `QVLFDUX`, `QVLFSX`, `QVLFSUX` (+ `*A` cache-hint) | `translateQpxLoadIndexed` (4× double / 4× float→double) |
| `PPC_INS_QVSTFDX`, `QVSTFDUX`, `QVSTFSX`, `QVSTFSUX` (+ `*A`) | `translateQpxStoreIndexed` |
| `PPC_INS_QVFCFID`, `QVFCFIDS`, `QVFCFIDU`, `QVFCFIDUS` | `translateQpxFpConvert` (per-lane sitofp/uitofp) |
| `PPC_INS_QVFCTID`, `QVFCTIDU`, `QVFCTIDUZ`, `QVFCTIDZ`, `QVFCTIW`, `QVFCTIWU`, `QVFCTIWUZ`, `QVFCTIWZ` | `translateQpxFpConvert` (Z = trunc; else roundeven) |
| `PPC_INS_QVFLOGICAL` + `PPC_INS_ALIAS_QVFAND`/`ANDC`/`CLR`/`CTFB`/`EQU`/`NAND`/`NOR`/`NOT`/`OR`/`ORC`/`SET`/`XOR` | `translateQpxLogical` (2-input 4-bit LUT) |
| `PPC_INS_QVALIGNI` | `translateQpxAlign` |
| `PPC_INS_QVESPLATI` | `translateQpxSplat` |
| `PPC_INS_QVFPERM` | `translateQpxPerm` (low 3 bits of each FRC slot) |
| `PPC_INS_QVGPCI` | `translateQpxGpci` (12-bit imm → 4×3-bit i64 slots) |
| `PPC_INS_QVFRE`, `QVFRES`, `QVFRIM`, `QVFRIN`, `QVFRIP`, `QVFRIZ`, `QVFRSP`, `QVFRSQRTE`, `QVFRSQRTES` | `translateQpxFpRound` (per-lane; no vector fneg) |
| `PPC_INS_QVFTSTNAN` | `translateQpxFpCmp` (`uno` → 1.0 / −1.0) |
| `PPC_INS_QVFXMUL`, `QVFXMADD`, `QVFXXMADD`, `QVFXXNPMADD`, `QVFXXCPNMADD` (+ `*S`) | `translateQpxCrossFma` (128-bit rotate; CPN = xor odd-slot signs) |
| `PPC_INS_QVLFCD*`, `QVLFCS*`, `QVLFIWAX`/`ZX`, `QVLPCL*`/`QVLPCR*` | `translateQpxLoadIndexed` (2-el / i32 convert / permute-control) |
| `PPC_INS_QVSTFCD*`, `QVSTFCS*`, `QVSTFIWX`, leftover `QVSTFD*I`/`QVSTFS*I` | `translateQpxStoreIndexed` |
| `PPC_INS_EVADDW`, `EVADDIW`, `EVSUBFW`, `EVSUBIFW` | `translateSpeArith` (per-word i32; `EVSUBFW` is rB−rA) |
| `PPC_INS_EVAND`, `EVANDC`, `EVOR`, `EVORC`, `EVXOR`, `EVNOR`, `EVEQV`, `EVNAND` | `translateSpeLogical` (full i64). `evnot` is not a Capstone ID; use `evnor rD,rA,rA` |
| `PPC_INS_EVABS`, `EVNEG`, `EVEXTSB`, `EVEXTSH`, `EVCNTLSW`, `EVCNTLZW`, `EVRNDW` | `translateSpeUnary` |
| `PPC_INS_EVCMPEQ`, `EVCMPGTS`, `EVCMPGTU`, `EVCMPLTS`, `EVCMPLTU` | `translateSpeCmp` (SPE CR: LT=both, GT=either, EQ=same, SO=0) |
| `PPC_INS_EVSLW`, `EVSLWI`, `EVSRWS`, `EVSRWU`, `EVSRWIS`, `EVSRWIU`, `EVRLW`, `EVRLWI` | `translateSpeShift` (per-word; count `& 31`) |
| `PPC_INS_EVSPLATI`, `EVSPLATFI` | `translateSpeSplat` (5-bit SIMM; FI = SIMM≪27) |
| `PPC_INS_EVMERGEHI`, `EVMERGELO`, `EVMERGEHILO`, `EVMERGELOHI` | `translateSpeMerge` |
| `PPC_INS_EVLDD`/`X`, `EVLDW`/`X`, `EVLDH`/`X`, `EVLWHE`/`X`, `EVLWHOU`/`X` | `translateSpeLoad` |
| `PPC_INS_EVLHHESPLAT`/`X`, `EVLHHOSSPLAT`/`X`, `EVLHHOUSPLAT`/`X`, `EVLWHOS`/`X`, `EVLWHSPLAT`/`X`, `EVLWWSPLAT`/`X` | `translateSpeLoad` |
| `PPC_INS_EVSTDD`/`X`, `EVSTDW`/`X`, `EVSTDH`/`X`, `EVSTWHE`/`X`, `EVSTWHO`/`X`, `EVSTWWE`/`X`, `EVSTWWO`/`X` | `translateSpeStore` (`WHO`=odd halfwords, `WWE`=high word, `WWO`=low word) |
| `PPC_INS_EVDIVWS`, `EVDIVWU` | `translateSpeDiv` (per-word; 0 divisor → 0; `INT_MIN/-1` stays `INT_MIN`) |
| `PPC_INS_MFSPEFSCR` / `MTSPEFSCR` + `PPC_INS_ALIAS_*` | `translateSpeFscr` (`PPC_REG_SPEFSCR` is i32) |
| `EVMHE*` / `EVMHO*` / `EVMWL*` / `EVMW*` / `EVMRA` / `EVADD*MIAAW` / `EVSUBF*MIAAW` | `translateSpeMac` → `ppc.spe.<mnem>` (no SPE ACC; `PPC_REG_ACC0..7` are Power10 MMA) |

`PPC_INS_DIVDE` / `DIVDEU` exist in this Capstone 6 insn enum but stay unmapped
(extended-divide; not gcc `-O2` Altivec). There is no `PPC_INS_VCIPHER` /
`VCIPHERLAST` / `VSBOX` in this Capstone 6 header set.

SPE opens as `createPpc32` extra `CS_MODE_SPE|BE`. Capstone names the 64-bit
GPR view `PPC_REG_S0..S31` (private i64 overlay). SPE ops load/store `S`
when Capstone names `R`; the low 32 bits are also written back to `R`.
`R0..R31` stay i32 so existing 32-bit tests are unchanged. There is no
`PPC_INS_EVNOT`. Every Capstone `PPC_INS_EV*` ID in this header is mapped
(no remaining EV nullptr except `PPC_INS_INVALID`). `MFSPEFSCR`/`MTSPEFSCR`
are Capstone aliases of `mfspr`/`mtspr` SPR 512.

## Tests

`tests/capstone2llvmir/powerpc_tests.cpp` is instantiated with
`CS_MODE_32`, `CS_MODE_64`, `CS_MODE_QPX`, and `CS_MODE_SPE` (`createPpc32` /
`createPpc64` / `createPpcQpx` / `createPpc32`+`CS_MODE_SPE`, big-endian).
`ALL_MODES` skips QPX and SPE.

- Integer ALU, word loads/stores, branches, LR/CTR, `rlwinm`, `cmp*` run in
  **32 and 64** (`ALL_MODES`).
- `ld`/`std`/`ldx`/`stdx`, the `rldicl` family, and compiler 64-bit GPR arith
  (`divd`/`mulld`/`sld`/`srad`/`cntlzd`/`popcntd`) are `ONLY_MODE_64`.
- Compiler FP and the Altivec/VSX encodings above (`vperm`/`vsel`/`vspltw`/
  `vsububm`/`lvsl`/`vpkuhum`/`vsl` included) are `ALL_MODES`.
- QPX `qvf*` / `qvlfdx` / `qvstfdx` family is `ONLY_MODE_QPX` (LLVM `qpx.s`
  encodings). Keystone `ks_open(KS_ARCH_PPC, KS_MODE_QPX|BE)` fails; the QPX
  fixture opens `KS_MODE_PPC32|BE`. `kstool ppc32be "qvfadd 0, 1, 2"` yields
  `10 01 10 2a`.
- SPE `ev*` is `ONLY_MODE_SPE` (`createPpc32` extra `CS_MODE_SPE|BE`).
  Keystone has no SPE mode; encodings are `kstool ppc32be` hex via
  `emulate_bin`.
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
- **64-bit FP args.** `PowerPC64CallingConvention` (`powerpc64_conv.cpp`)
  lists `f1`–`f13` (ELFv2). Vector args (Altivec `v2`–`v13`, VSX) still
  have no CC entries.
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

QPX compiler IDs in this Capstone (`QVF*` / `QVL*` / `QVST*` / `QVGPCI` /
`QVALIGNI` / `QVESPLATI`, including `QVFLOGICAL` aliases) are mapped. The
gcc `-mspe` EV* IDs in this Capstone header are all mapped.
AES (no `PPC_INS_VCIPHER` in this Capstone) stays unmapped.
`PPC_INS_DIVDE` / `DIVDEU` are mapped (`RA||0` / `RB`); overflow `DIVDEO` /
`DIVDEUO` are not. `PPC_INS_VRLD` is mapped.
Packed Altivec/VSX leftovers from the previous SIMD pass (`vmrgh*`/`vmrgl*`
byte/half, `vmule*`/`vmulo*`/`vmuluwm`, `vmsum*`, `vmhaddshs`/`vmhraddshs`/
`vmladduhm`, `vpkpx`, `vupk*`, `vsum*`, `vexptefp`/`vlogefp`, mixed-width
`xvcv*`, `xvtdiv*`/`xvtsqrt*`) are now mapped. Scalar VSX `xs*` used by
POWER8 gcc `-O2` (`xsabsdp`/`xsnabsdp`/`xsnegdp`/`xscpsgndp`, `xscv*`,
`xsmadd*`/`xsmsub*`/`xsnmadd*`/`xsnmsub*` A/M, `xsmaxdp`/`xsmindp`,
`xsrdpi*`/`xsredp`/`xsrsqrtedp`/`xssqrtdp`, `xstdivdp`/`xstsqrtdp`) and
the leftover `xx*` permute/select IDs (`xxmrghw`/`xxmrglw`/`xxsel`/
`xxsldwi`/`xxspltd`/`xxmrghd`/`xxmrgld`/`xxswapd`) are mapped. No remaining
`PPC_INS_V*` or packed `XV*` nullptrs, and no remaining leftover scalar
`XS*` / permute `XX*` compiler IDs. Cache/TLB (`dcbz`, `tlbie`, `slbia`),
privileged `mfmsr`/`mtmsr`, `dst`/`dss`, `attn`, and traps `tw`/`td` stay
unmapped by policy. `lxvdsx`/`lxvw4x`/`stxvw4x` reuse the indexed 128-bit load/store path
(`lxvdsx` splat-loads one doubleword). FPSCR (`mffs`/`mtfsf`/`mtfsfi`/
`mtfsb0`/`mtfsb1`/`mcrfs`) and VSCR (`mfvscr`/`mtvscr`) use private
`fpscr`/`vscr` i32 globals. SPE `BRINC` is mapped (`translateSpeBrinc`,
32-bit bit-reversed increment). No remaining SPE compiler ID in `_i2fm`.
`MFOCRF`/`MTOCRF` are mapped (`translateMfcr` / `translateMtcrf`; Capstone
reports the FXM slot as a CR field register). Same bar as x86 including
3DNow core. `vmrghw`/`vmrglw`/`vcmpequw` tests use
Capstone 6 decoder VX bits (`Inst{10-0}`), not GNU `VX=128/192/454`. Saturating
Altivec (`vadds*`/`vsubs*` plus unsigned `*ubs`/`*uhs`/`*uws`), saturating pack
(`vpkshss`/`vpkshus`/`vpkswss`/`vpkswus`/`vpkuhus`/`vpkuwus`), per-element
shifts (`vslb`/`vslh`/`vslw`/`vsld`, `vsr*` including `vsrd`, `vsra*` including
`vsrad`), per-element rotate (`vrlb`/`vrlh`/`vrlw`; Capstone `vrt,vra,vrb`),
FP vector compare (`vcmpeqfp`/`vcmpgtfp`/`xvcmpeqdp`/`xvcmpeqsp`/
`xvcmpge*`/`xvcmpgt*`), VSX packed FMA (`xvmadd*` / `xvmsub*` / `xvnmsub*` /
`xvnmadd*` A and M, SP and DP), VSX abs/neg/nabs/cpsgn (`xvabsdp`/`xvabssp`/
`xvnegdp`/`xvnegsp`/`xvnabsdp`/`xvnabssp`/`xvcpsgndp`/`xvcpsgnsp`), VSX min/max
(`xvmaxdp`/`xvmindp`/`xvmaxsp`/`xvminsp`), VSX DP round (`xvrdpi`/`xvrdpic`/
`xvrdpim`/`xvrdpip`/`xvrdpiz`), Altivec `vcfsx`/`vcfux`/`vctuxs`/`vctsxs`
(UIM=0; non-zero UIM scales by `2^±UIM`), and `vrfin`/`vrfiz`/`vrfip`/`vrfim`
are mapped.

**FPR / VSL overlay:** `f0` is still its own `double` global (existing FP
tests). `VSL0` is a separate i128. A VSX op that names `VSL0` inserts the
high doubleword; it does not rewrite `F0`, so mixed FPR/VSX views of the
same hardware register can drift. gcc `-O1` scalar VSX typically names the
FPR (`xsadddp` on `f1`).
