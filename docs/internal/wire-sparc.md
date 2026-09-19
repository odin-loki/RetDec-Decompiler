# Wire SPARC into RetDec

This file is the **only** place the SPARC translator is connected to existing
sources. The implementation and tests already exist as new files; apply the
hunks below in a follow-up commit. Do **not** invent Capstone IDs.

Public factory already exists:

```cpp
Capstone2LlvmIrTranslator::createSparc(llvm::Module* m, cs_mode extra);
```

It currently throws `GenericError("SPARC architecture is unimplemented.")`.

## Capstone 5.0.9 modes (do not pass CS_MODE_32 to cs_open)

`cs_open(CS_ARCH_SPARC, mode)` accepts only endian plus `CS_MODE_V9`.
`CS_MODE_32` / `CS_MODE_64` are x86/PPC bits and yield `CS_ERR_MODE`.

| User / decoder request | Capstone basic | Capstone extra |
|---|---|---|
| SPARC V8 32-bit | `CS_MODE_LITTLE_ENDIAN` (0) | `CS_MODE_BIG_ENDIAN` (typical) |
| SPARC V9 / SPARC64 | `CS_MODE_LITTLE_ENDIAN` (0) | extra \| `CS_MODE_V9` |

The impl ctor takes `(m, basic=0, extra)`. V9 is detected with `extra & CS_MODE_V9`.

## Delay-slot strategy

SPARC CTIs have a **one-instruction delay slot**, same as MIPS.

| API | SPARC behaviour |
|---|---|
| `hasDelaySlot` / `getDelaySlot` | `1` for `B`, `FB`, `BRcc`, `CALL`, `JMP`, `JMPL`, `RETT`, `RET`, `RETL` |
| `hasDelaySlotTypical` | same set — delay slot **always executed** |
| `hasDelaySlotLikely` | always `false` |

Annul (`,a` / `SPARC_HINT_A`) is a **hint on `cs_sparc`**, not a distinct
Capstone insn id. The decoder APIs take `uint32_t id` only (see
`Decoder::handleDelaySlotTypical` / `handleDelaySlotLikely`), so annulled
slots cannot be classified as MIPS-likely without a decoder change that
inspects `insn->detail->sparc.hint`. Production lifting therefore matches
non-annulled SPARC (the common case): execute the delay slot always, then
the branch/call/return. That is the MIPS typical-slot path.

`BPcc` is Capstone `SPARC_INS_B` with `SPARC_HINT_PT` / `SPARC_HINT_PN`.
Same translator as `Bicc`; `%xcc` vs `%icc` is `SPARC_REG_XCC` if present
as an operand, else `SPARC_REG_ICC`.

---

## 1. `src/capstone2llvmir/capstone2llvmir.cpp`

### Include

```diff
 #include "capstone2llvmir/powerpc/powerpc_impl.h"
+#include "capstone2llvmir/sparc/sparc_impl.h"
 #include "capstone2llvmir/x86/x86_impl.h"
```

### `createArch` — `CS_MODE_32` vs V9/64

Replace the SPARC case so 64-bit / V9 requests set `CS_MODE_V9` on extra.
Do **not** pass `CS_MODE_32` or `CS_MODE_64` through as Capstone basic mode.

```diff
 		case CS_ARCH_SPARC:
 		{
-			return createSparc(m, extra);
+			cs_mode e = extra;
+			if (basic == CS_MODE_64 || basic == CS_MODE_V9 || (extra & CS_MODE_V9))
+			{
+				e = static_cast<cs_mode>(e | CS_MODE_V9);
+			}
+			return createSparc(m, e);
 		}
```

`CS_MODE_32` and default (`0`) stay V8. `CS_MODE_64` and `CS_MODE_V9` select V9.

### `createSparc` body — stop throwing

```diff
 std::unique_ptr<Capstone2LlvmIrTranslator> Capstone2LlvmIrTranslator::createSparc(
 		llvm::Module* m,
 		cs_mode extra)
 {
-	throw GenericError("SPARC architecture is unimplemented.");
-	return nullptr;
+	// Capstone SPARC basic mode is 0. V9 lives in extra (CS_MODE_V9).
+	return std::make_unique<Capstone2LlvmIrTranslatorSparc_impl>(
+			m, CS_MODE_LITTLE_ENDIAN, extra);
 }
```

---

## 2. CMakeLists

### `src/capstone2llvmir/CMakeLists.txt`

Insert next to the other ISA translators (after powerpc, before sysz/x86):

```diff
 	powerpc/powerpc_init.cpp
 	powerpc/powerpc.cpp
+	sparc/sparc_init.cpp
+	sparc/sparc.cpp
 	sysz/sysz_init.cpp
```

If `sysz/` is not present in this tree, insert before `x86/x86_init.cpp`.

### `tests/capstone2llvmir/CMakeLists.txt`

```diff
 	powerpc_tests.cpp
+	sparc_tests.cpp
 	sysz_tests.cpp
```

### `src/bin2llvmir/CMakeLists.txt`

```diff
 	providers/abi/powerpc.cpp
 	providers/abi/powerpc64.cpp
+	providers/abi/sparc.cpp
+	providers/abi/sparc64.cpp
 	providers/abi/sysz.cpp
```

---

## 3. `include/retdec/common/architecture.h` — `eArch::SPARC`

Query methods (with the other `is*` declarations):

```diff
 		bool isPpc() const;
 		bool isPpc64() const;
+		bool isSparc() const;
+		bool isSparc64() const;
 		bool isEndianLittle() const;
```

Set methods:

```diff
 		void setIsPpc();
+		void setIsSparc();
+		void setIsSparc64();
 		void setIsEndianLittle();
```

Enum:

```diff
 		enum class eArch
 		{
 			UNKNOWN,
 			MIPS,
 			PIC32,
 			ARM,
 			X86,
 			PPC,
+			SPARC,
 		};
```

---

## 4. `src/common/architecture.cpp`

Anonymous names:

```diff
 const std::string ARCH_PPC     = "powerpc";
 const std::string ARCH_PPC64   = "powerpc64";
+const std::string ARCH_SPARC   = "sparc";
+const std::string ARCH_SPARC64 = "sparc64";
```

Queries / setters (next to PPC):

```diff
 bool Architecture::isPpc() const          { return isArch(eArch::PPC); }
 bool Architecture::isPpc64() const        { return isPpc() && getBitSize() == 64; }
+bool Architecture::isSparc() const        { return isArch(eArch::SPARC); }
+bool Architecture::isSparc64() const      { return isSparc() && getBitSize() == 64; }
```

```diff
 void Architecture::setIsPpc()            { setName(ARCH_PPC); }
+void Architecture::setIsSparc()          { setName(ARCH_SPARC); }
+void Architecture::setIsSparc64()        { setName(ARCH_SPARC64); setBitSize(64); }
```

`setArch()` — match `sparc64` first so the name is not swallowed by `"sparc"`:

```diff
 	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_PPC))
 	{
 		_arch = eArch::PPC;
 	}
+	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_SPARC64))
+	{
+		_arch = eArch::SPARC;
+		_bitSize = 64;
+	}
+	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_SPARC))
+	{
+		_arch = eArch::SPARC;
+	}
 }
```

---

## 5. `decoder_init.cpp` — `Decoder::initTranslator`

After the ARM64 branch, before `else throw`:

```diff
 	else if (a.isArm64())
 	{
 		arch = CS_ARCH_ARM64;
 		basicMode = CS_MODE_ARM;
 	}
+	else if (a.isSparc())
+	{
+		arch = CS_ARCH_SPARC;
+		// Capstone SPARC basic mode is 0. V9/64 is extra CS_MODE_V9.
+		basicMode = CS_MODE_LITTLE_ENDIAN;
+		if (a.getBitSize() == 64)
+		{
+			extraMode = static_cast<cs_mode>(extraMode | CS_MODE_V9);
+		}
+	}
 	else
 	{
 		throw std::runtime_error("Unsupported architecture.");
 	}
```

Optional `.eh_frame` triple (same file, `initJumpTargetsEhFrame`):

```diff
 	else if (a.isPpc())
 		arch = llvm::Triple::ppc;
+	else if (a.isSparc() && a.getBitSize() == 64)
+		arch = llvm::Triple::sparcv9;
+	else if (a.isSparc())
+		arch = llvm::Triple::sparc;
```

---

## 6. AbiProvider — `src/bin2llvmir/providers/abi/abi.cpp`

Includes:

```diff
 #include "retdec/bin2llvmir/providers/abi/powerpc.h"
+#include "retdec/bin2llvmir/providers/abi/sparc.h"
+#include "retdec/bin2llvmir/providers/abi/sparc64.h"
 #include "retdec/bin2llvmir/providers/abi/x86.h"
```

`addAbi()`, after PPC, before x86-64:

```diff
 	else if (c->getConfig().architecture.isPpc())
 	{
 		auto p = _module2abi.emplace(m, std::make_unique<AbiPowerpc>(m, c));
 		return p.first->second.get();
 	}
+	else if (c->getConfig().architecture.isSparc64())
+	{
+		auto p = _module2abi.emplace(m, std::make_unique<AbiSparc64>(m, c));
+		return p.first->second.get();
+	}
+	else if (c->getConfig().architecture.isSparc())
+	{
+		auto p = _module2abi.emplace(m, std::make_unique<AbiSparc>(m, c));
+		return p.first->second.get();
+	}
 	else if (c->getConfig().architecture.isX86_64())
```

`AbiSparc` / `AbiSparc64` currently set `_defcc = CC_UNKNOWN`. A later
commit can add `CC_SPARC` / `CC_SPARC64` to `calling_convention.h` if a
dedicated convention class is written. Do not invent Capstone register IDs.

---

## 7. ELF `EM_SPARC` / `EM_SPARCV9` / `EM_SPARC32PLUS`

### `include/retdec/fileformat/fftypes.h`

```diff
 enum class Architecture
 {
 	UNKNOWN,
 	X86,
 	X86_64,
 	ARM,
 	POWERPC,
 	MIPS
+	,SPARC
 };
```

Prefer a trailing-comma style consistent with the file:

```diff
 	POWERPC,
-	MIPS
+	MIPS,
+	SPARC
 };
```

### `src/fileformat/file_format/elf/elf_format.cpp` — `ElfFormat::getTargetArchitecture`

```diff
 		case EM_PPC:
 		case EM_PPC64:
 			return Architecture::POWERPC;
+		case EM_SPARC:
+		case EM_SPARC32PLUS:
+		case EM_SPARCV9:
+			return Architecture::SPARC;
 		case EM_NONE:
```

Word size still comes from ELF class (`getWordLength()`): `EM_SPARCV9` is
typically 64-bit; `EM_SPARC` / `EM_SPARC32PLUS` are 32-bit.

### `src/bin2llvmir/optimizations/provider_init/provider_init.cpp`

```diff
 			case fileformat::Architecture::POWERPC: a.setIsPpc(); break;
 			case fileformat::Architecture::MIPS: a.setIsMips(); break;
+			case fileformat::Architecture::SPARC:
+				if (a.getBitSize() == 64) a.setIsSparc64();
+				else a.setIsSparc();
+				break;
 			default: break; // nothing
```

`setBitSize` is already called from `getWordLength()` just above this switch.

---

## 8. CLI `-a sparc|sparc64`

### `src/retdec-decompiler/retdec-decompiler.cpp`

Allowed names:

```diff
 		if (!(a == "mips" || a == "pic32" || a == "arm" || a == "thumb" || a == "arm64" || a == "powerpc" || a == "x86"
-			  || a == "x86-64"))
+			  || a == "x86-64" || a == "sparc" || a == "sparc64"))
```

After `setName(a)` for `sparc64`, `setArch()` sets `_bitSize = 64`.

Help text:

```diff
-	[-a|--arch ARCH] Specify target architecture [mips|pic32|arm|thumb|arm64|powerpc|x86|x86-64].
+	[-a|--arch ARCH] Specify target architecture [mips|pic32|arm|thumb|arm64|powerpc|x86|x86-64|sparc|sparc64].
```

`capstone2llvmirtool` already accepts `-a sparc` and extra `-e v9`. After
`createSparc` is wired it will lift. SPARC is big-endian; pass `-e big`
(and `-e v9` for V9, or combine via extra-mode OR in a later tool tweak).

---

## New files this wiring assumes

| File | Role |
|---|---|
| `include/retdec/capstone2llvmir/sparc/sparc.h` | public translator interface |
| `src/capstone2llvmir/sparc/sparc_impl.h` | impl |
| `src/capstone2llvmir/sparc/sparc.cpp` | integer / FP / CTI / SAVE lift |
| `src/capstone2llvmir/sparc/sparc_init.cpp` | regs + `_i2fm` (Capstone 6.0.0-Alpha10 IDs only) |
| `tests/capstone2llvmir/sparc_tests.cpp` | 32-bit and V9/64, no SKIP |
| `include/retdec/bin2llvmir/providers/abi/sparc.h` | V8 ABI |
| `include/retdec/bin2llvmir/providers/abi/sparc64.h` | V9 ABI |
| `src/bin2llvmir/providers/abi/sparc.cpp` | `%sp`, `%g0`, `%o0`–`%o5`, `%g1` syscall |
| `src/bin2llvmir/providers/abi/sparc64.cpp` | same register IDs, 64-bit config |

## Tests after wiring

```
ctest -R capstone2llvmir --output-on-failure
```

Filter SPARC:

```
./retdec-tests-capstone2llvmir --gtest_filter='*Sparc*'
```

Both `CS_MODE_32` and `CS_MODE_64` (`CS_MODE_V9`) parameterizations must run.
Do not SKIP or DISABLED-prefix any of them.

---

## Capstone 6.0.0-Alpha10 IDs (do not invent `SPARC_INS_*`)

`cs_open(CS_ARCH_SPARC, mode)` still accepts only endian plus `CS_MODE_V9`.
Do **not** pass `CS_MODE_32` to `cs_open`. V9 is extra `CS_MODE_V9`.
`createSparc` keeps basic mode `CS_MODE_LITTLE_ENDIAN` (0).

Capstone 6 dropped several Capstone 5 names. Compat aliases in
`src/capstone2llvmir/capstone6_compat.h` map tests onto real enumerators:

| Capstone 5 name | Capstone 6 token |
|---|---|
| `SPARC_INS_JMP` | `SPARC_INS_JMPL` |
| `SPARC_INS_CMP` | `SPARC_INS_ALIAS_CMP` (real lift is `SPARC_INS_SUBCC`) |
| `SPARC_INS_RET` / `RETL` | `SPARC_INS_ALIAS_RET` / `ALIAS_RETL` (encoding is `JMPL`) |
| `SPARC_INS_BRZ` / `BRNZ` / … | `SPARC_INS_ALIAS_BR*` (real id `SPARC_INS_BR`) |
| `SPARC_REG_XCC` | no register; XCC is `SPARC_CC_FIELD_XCC`. Flags packed in `SPARC_REG_ICC` bits 7:4 |
| assembler `ldf` | `SPARC_INS_LD` with `SPARC_REG_F*` dest |
| assembler `lddf` / `ldd` to `D*` | `SPARC_INS_LDD` |
| assembler `stf` | `SPARC_INS_ST` from `SPARC_REG_F*` |
| assembler `stdf` | `SPARC_INS_STD` from `D*` / even `F*` |
| assembler `ld`/`st` `%fsr` | `SPARC_INS_LD` / `SPARC_INS_ST` (implicit `%fsr` when no dest/src reg) |
| assembler `cas` | `SPARC_INS_ALIAS_CAS` (real id `SPARC_INS_CASA`) |
| assembler `casx` | `SPARC_INS_ALIAS_CASX` (real id `SPARC_INS_CASXA`) |
| assembler `fand` | `SPARC_INS_FAND` (64-bit VIS; single-width is `SPARC_INS_FANDS`) |
| assembler `fnot` | `SPARC_INS_FNOT1` / `SPARC_INS_FNOT2` (no `SPARC_INS_FNOT`) |
| assembler `fmul8x16` | `SPARC_INS_FMUL8X16` |
| assembler `fmul8sux16` | `SPARC_INS_FMUL8SUX16` |

There is **no** `SPARC_INS_LDF` / `SPARC_INS_STF` / `SPARC_INS_LDDF` / `SPARC_INS_STDF` /
`SPARC_INS_LDFSR` / `SPARC_INS_CASX` / `SPARC_INS_FNOT` / `SPARC_INS_LDUW` / `SPARC_INS_STW`
in Capstone 6. `lduw`/`stw` are `SPARC_INS_LD` / `SPARC_INS_ST`.

### Mapped (real LLVM IR)

Integer: `ADD`/`ADDCC`/`ADDX*`, `SUB`/`SUBCC`/`SUBX*`, `AND`/`ANDN`/`AND*CC`,
`OR`/`ORN`/`OR*CC`, `XOR`/`XNOR`/`XOR*CC`, `SLL`/`SRL`/`SRA`/`SLLX`/`SRLX`/`SRAX`,
`SETHI`, `NOP`, `MOV`, `SMUL`/`UMUL`/`MULX`, `SDIV`/`UDIV`/`SDIVX`/`UDIVX`,
`RD`/`WR` (`%y`), `LD`/`LDSB`/`LDUB`/`LDSH`/`LDUH`/`LDSW`/`LDX`/`LDD`,
`ST`/`STB`/`STH`/`STX`/`STD`, `SAVE`/`RESTORE`, `CALL`, `JMPL`, `RETT`,
`B`, `BR`, `CMP` (alias of `SUBCC`).

FP: `FADDS`/`FADDD`/`FSUBS`/`FSUBD`/`FMULS`/`FMULD`/`FSMULD`/`FDIVS`/`FDIVD`,
`FCMPS`/`FCMPD`/`FCMPES`/`FCMPED`, `FMOVS`/`FMOVD`, `FNEGS`/`FNEGD`,
`FABSS`/`FABSD`, `FSQRTS`/`FSQRTD`, `FITOS`/`FITOD`/`FSTOI`/`FDTOI`/`FSTOD`/`FDTOS`,
`FXTOS`/`FXTOD`/`FSTOX`/`FDTOX`, `MOVDTOX`/`MOVXTOD`/`MOVWTOS`/`MOVSTOSW`/`MOVSTOUW`,
`FB`, `ld`/`ldd` to `F*`/`D*` (assembler `ldf`/`lddf`), `st`/`std` from `F*`/`D*`
(assembler `stf`/`stdf`), `ld`/`st` `%fsr`.

VIS (D* as i64 SIMD containers, `F*S` as i32): `FAND`/`FANDS`/`FOR`/`FORS`/`FXOR`/`FXORS`/
`FXNOR`/`FXNORS`/`FNOT1`/`FNOT2`/`FZERO`/`FONE`/`FSRC1`/`FSRC2`/`FANDNOT1`/`FANDNOT2`
and the `*S` single-width forms; `ALIGNADDR`/`ALIGNADDRL`/`FALIGNDATA` (GSR = `SPARC_REG_ASR19`);
`FCMPGT16`/`FCMPGT32`/`FCMPLE16`/`FCMPLE32`/`FCMPEQ16`/`FCMPEQ32`/`FCMPNE16`/`FCMPNE32`;
`FMUL8X16`/`FMUL8SUX16`/`FMUL8X16AL`/`FMUL8X16AU`; `FPACK16`/`FPACK32`;
`FPADD16`/`FPADD16S`/`FPADD32`/`FPADD32S`/`FPSUB16`/`FPSUB16S`/`FPSUB32`/`FPSUB32S`;
`FEXPAND`; `FPMERGE`; `PDIST`/`PDISTN`; `BSHUFFLE`; `ARRAY8`/`ARRAY16`/`ARRAY32`;
`EDGE8`/`EDGE16`/`EDGE32` and `L`/`N`/`LN`.

Atomics / barriers: `CASA`/`CASXA` (`cmpxchg`), `MEMBAR` (seq_cst fence), `STBAR` (release fence).

Delay slot stays 1 for `B`/`FB`/`BR`/`CALL`/`JMPL`/`RETT`/`RET`/`RETL`.
`hasDelaySlotLikely` remains false (annul is a hint, not an id).

Capstone 6 auto-sync reports `id=JMPL` for `ret`/`retl` with `alias_id=ALIAS_RET(L)`
and often zero alias operands. The translator prefers `_i2fm[alias_id]` and,
if `JMPL` still has `op_count==0`, dispatches on mnemonic/`alias_id` to
`translateRet` (`%o7+8` vs `%i7+8`). GPR even/odd pairs (`SPARC_REG_G2_G3`, …)
are composed from the two GPRs; they are not separate LLVM globals.
`ld`/`ldd` to `F*`/`D*` load integer bits then `bitcast` to float/double
(the emulator memory map is integer-typed). `st`/`std` from `F*`/`D*` bitcast
IEEE bits to i32/i64 before the integer store.

### Remaining gaps (pseudo-asm)

Quad (`FMOVQ`/`FADDq`/`FDMULQ`/`LDQ`/`STQ`/`FMOVRQ`/`Q0..` — Q regs exist in Capstone as
`SPARC_REG_Q0` but are not a mapped LLVM register file; skip unless a later commit
adds them),
ASI loads/stores other than primary/`omitted` (`LDA`/`STA`/`LDDA`/`STDA` with
`#ASI_P` / 0x80 *are* mapped, same as `LD`/`ST`/`LDD`/`STD`; `CASA`/`CASXA` *are*
mapped; `LDSTUB`/`SWAP` and ASI-primary/`omitted` `LDSTUBA`/`SWAPA` *are* mapped —
other ASI values stay pseudo),
`FLUSHW` (privileged window; `FLUSH` I-cache is a nop like x86 `PREFETCH` — I-cache is not modelled),
coprocessor `CB`, window state `DONE`/`RETRY`/`SAVED`/`RESTORED`,
privileged `RDPR`/`WRPR`, leftover VIS 3 (AES and anything still unmapped;
`TADDCC`/`TADDCCTV`/`TSUBCC`/`TSUBCCTV` *are* mapped as add/sub + ICC like `ADDCC`/`SUBCC`
(arithmetic NZVC only; tagged-overflow from operand[1:0] is not OR'd into `ICC.V`).
TV forms have no trap side effect (same IR as CC; `T`/`TA` are mapped as `__asm_t`/`__asm_ta` like ARM64 SVC — no throw);
`MULSCC` (V8 multiply-step: Y[0]/ICC-gated 33-bit add + 64-bit right shift of (rd:Y)) stays unmapped;
`MOVR`/`FMOVRS`/`FMOVRD` *are* mapped — integer/FP CMOV on `SPARC_CC_REG_*` in
`cs_sparc.cc`; aliases `ALIAS_MOVRZ`/`MOVRNZ`/`MOVRGZ`/`MOVRLEZ`/`MOVRLZ`/`MOVRGEZ`
dispatch to the same translator; `FNOR`/`FNORS` *are* mapped via `translateVisLogical`;
`FMEAN16` *is* mapped as signed 16-bit `(a+b+1)>>1` per lane;
`FPADD64`/`FPACKFIX`/`FMUL8ULX16`/`FMULD8SUX16`/`FMULD8ULX16`/`FCHKSM16`/`POPC`
*are* mapped). `FCHKSM16` is a wrapping 16-bit lane add of rs1/rs2 then
add-fold into `rd[15:0]` (no in-tree OSA helper). Annulled delay slots still
execute (decoder is id-only).
`F*`/`D*` are separate LLVM globals (no hardware even/odd aliasing); 64-bit VIS
rewrites even `F*` names to the overlapping `D*` container.
