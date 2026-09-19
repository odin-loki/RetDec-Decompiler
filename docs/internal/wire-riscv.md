# Wire RISC-V (Capstone 6.0.0-Alpha10)

**Capstone RISC-V exists.** Pinned Capstone 6.0.0-Alpha10 (`cmake/deps.cmake`) defines:

- `CS_ARCH_RISCV`
- `CS_MODE_RISCV32` (`1 << 0`, same value as `CS_MODE_32`)
- `CS_MODE_RISCV64` (`1 << 1`, same value as `CS_MODE_64`)
- `CS_MODE_RISCVC` (`1 << 2`; Capstone 6 name `CS_MODE_RISCV_C`, aliased in `capstone6_compat.h`)
- Header `include/capstone/riscv.h` (`riscv_insn`, `riscv_reg`, `cs_riscv`)

Do not invent enum values. Include `<capstone/riscv.h>` (already done via `riscv_defs.h`).

These files are already in the tree. **Do not apply this document’s shared patches from the translator agent** — they are for the wiring agent.

## F / D / C

Capstone 5.0.9 **does not expand** compressed encodings to I IDs. `RISCV_printInst` has `uncompressInst` commented out (`TODO: RISCV compressed instructions`). C instructions arrive as `RISCV_INS_C_*`.

The translator **does** lift integer `C_*` IDs (`C_ADD`, `C_ADDI`, `C_MV`, `C_LI`, `C_LUI`, `C_J`, `C_JAL`, `C_JR`, `C_JALR`, `C_BEQZ`, `C_BNEZ`, `C_LW`/`C_SW`/`C_LD`/`C_SD` and SP variants, `C_*W`, `C_NOP`, `C_MUL`). Decoding them requires extra mode `CS_MODE_RISCVC`.

**F and D** (`FADD_S`/`FADD_D`, `FSUB_*`, `FMUL_*`, `FDIV_*`, fused `FMADD`/`FMSUB`/`FNMADD`/`FNMSUB`, `FSQRT`, `FSGNJ`/`FSGNJN`/`FSGNJX`, `FMIN`/`FMAX`, `FCLASS_S`/`FCLASS_D`, `FMV_W_X`/`FMV_X_W`/`FMV_D_X`/`FMV_X_D`, `FLW`/`FSW`/`FLD`/`FSD`, `FCVT_*`, `FEQ`/`FLT`/`FLE`, compressed `C_FLW`/`C_FSW`/`C_FLD`/`C_FSD` and SP variants) lift to LLVM float/double. Rounding-mode field on `cs_riscv` is ignored. `FCLASS` writes the unprivileged 10-bit class mask (`-inf` … `qNaN`) via bitcast + icmp/select (LLVM 23 has `Intrinsic::is_fpclass`, but the capstone2llvmir emulator does not lower it).

**M** (`MUL`/`MULH`/`MULHSU`/`MULHU`/`MULW`/`C_MUL`, `DIV`/`DIVU`/`REM`/`REMU` and W variants) lift to `mul`/`sdiv`/`udiv`/`srem`/`urem`. Division by zero and signed overflow use the RISC-V-defined results (not LLVM UB).

**A** (`AMOADD`/`AMOAND`/`AMOOR`/`AMOXOR`/`AMOSWAP` W/D with aq/rl/`aqrl`, `LR`/`SC` W/D, `AMOMAX`/`AMOMAXU`/`AMOMIN`/`AMOMINU` W/D with aq/rl/`aqrl`) lift to `atomicrmw` / atomic load+store (same exclusive-monitor omission as ARM LDXR/STXR in this tree). `Max`/`Min`/`UMax`/`UMin` match the signed and unsigned AMO min/max variants.

**CSR** (`CSRRW`/`CSRRS`/`CSRRC` and immediates): Capstone `RISCV_OP_CSR` encodings that map to a Capstone register (`FFLAGS`/`FRM`/`VL`/`VTYPE`/`VLENB`/`VXSAT`/`VXRM`) or a well-known `RISCV_SYSREG_*` (cycle/time/instret, sstatus/sie/…, mstatus/misa/…/mhartid) become load/store of that LLVM global. `FCSR` is composed from `FFLAGS|FRM` because encoding `0x3` collides with `RISCV_REG_SSP`. Unknown encodings stay named `__asm_*`. CSR numbers are never invented.

## Extra required patches (not in the original list, but the tree will not link without them)

- `deps/capstone/CMakeLists.txt` — `-DCAPSTONE_RISCV_SUPPORT=ON` (default is ON via `CAPSTONE_ARCHITECTURE_DEFAULT`; set it explicitly).
- `src/bin2llvmir/CMakeLists.txt` — add `providers/abi/riscv.cpp` and `riscv64.cpp`.
- `include/retdec/fileformat/fftypes.h` — `Architecture::RISCV` so `elf_format.cpp` compiles.
- `include/retdec/bin2llvmir/providers/asm_instruction.h` — include `riscv_defs.h`.
- `include/retdec/bin2llvmir/providers/abi/abi.h` + `abi.cpp` — `isRiscv()` / `isRiscv64()` (query methods; `addAbi` is listed below).

---

## `src/capstone2llvmir/capstone2llvmir.cpp`

```diff
 #include "capstone2llvmir/arm/arm_impl.h"
 #include "capstone2llvmir/arm64/arm64_impl.h"
 #include "capstone2llvmir/mips/mips_impl.h"
 #include "capstone2llvmir/powerpc/powerpc_impl.h"
+#include "capstone2llvmir/riscv/riscv_impl.h"
 #include "capstone2llvmir/x86/x86_impl.h"
```

```diff
 		case CS_ARCH_PPC:
 		{
 			if (basic == CS_MODE_32) return createPpc32(m, extra);
 			if (basic == CS_MODE_64) return createPpc64(m, extra);
 			break;
 		}
+		case CS_ARCH_RISCV:
+		{
+			if (basic == CS_MODE_RISCV32) return createRiscv32(m, extra);
+			if (basic == CS_MODE_RISCV64) return createRiscv64(m, extra);
+			break;
+		}
 		case CS_ARCH_SPARC:
```

```diff
 	return std::make_unique<Capstone2LlvmIrTranslatorPowerpc_impl>(m, CS_MODE_QPX, extra);
 }

+std::unique_ptr<Capstone2LlvmIrTranslator> Capstone2LlvmIrTranslator::createRiscv32(
+		llvm::Module* m,
+		cs_mode extra)
+{
+	return std::make_unique<Capstone2LlvmIrTranslatorRiscv_impl>(m, CS_MODE_RISCV32, extra);
+}
+
+std::unique_ptr<Capstone2LlvmIrTranslator> Capstone2LlvmIrTranslator::createRiscv64(
+		llvm::Module* m,
+		cs_mode extra)
+{
+	return std::make_unique<Capstone2LlvmIrTranslatorRiscv_impl>(m, CS_MODE_RISCV64, extra);
+}
+
 std::unique_ptr<Capstone2LlvmIrTranslator> Capstone2LlvmIrTranslator::createSparc(
```

---

## `include/retdec/capstone2llvmir/capstone2llvmir.h`

```diff
 #include "retdec/capstone2llvmir/powerpc/powerpc_defs.h"
+#include "retdec/capstone2llvmir/riscv/riscv_defs.h"
 #include "retdec/capstone2llvmir/x86/x86_defs.h"
```

Insert the named ctors **before** `createSparc()`:

```diff
 		static std::unique_ptr<Capstone2LlvmIrTranslator> createPpcQpx(
 				llvm::Module* m,
 				cs_mode extra = CS_MODE_LITTLE_ENDIAN);
+		/**
+		 * Create RV32I translator with basic mode @c CS_MODE_RISCV32,
+		 * and extra mode @c extra (e.g. @c CS_MODE_RISCVC).
+		 */
+		static std::unique_ptr<Capstone2LlvmIrTranslator> createRiscv32(
+				llvm::Module* m,
+				cs_mode extra = CS_MODE_LITTLE_ENDIAN);
+		/**
+		 * Create RV64I translator with basic mode @c CS_MODE_RISCV64,
+		 * and extra mode @c extra (e.g. @c CS_MODE_RISCVC).
+		 */
+		static std::unique_ptr<Capstone2LlvmIrTranslator> createRiscv64(
+				llvm::Module* m,
+				cs_mode extra = CS_MODE_LITTLE_ENDIAN);
 		/**
 		 * Create SPARC translator with extra mode @c extra.
```

---

## `src/capstone2llvmir/CMakeLists.txt`

```diff
 	powerpc/powerpc_init.cpp
 	powerpc/powerpc.cpp
+	riscv/riscv_init.cpp
+	riscv/riscv.cpp
 	x86/x86_init.cpp
```

---

## `tests/capstone2llvmir/CMakeLists.txt`

```diff
 	mips_tests.cpp
 	powerpc_tests.cpp
+	riscv_tests.cpp
 	x86_tests.cpp
```

---

## `include/retdec/common/architecture.h`

```diff
 		bool isPpc() const;
 		bool isPpc64() const;
+		bool isRiscv() const;
+		bool isRiscv64() const;
 		bool isEndianLittle() const;
```

```diff
 		void setIsX86();
 		void setIsPpc();
+		void setIsRiscv();
 		void setIsEndianLittle();
```

```diff
 		enum class eArch
 		{
 			UNKNOWN,
 			MIPS,
 			PIC32,
 			ARM,
 			X86,
 			PPC,
+			RISCV,
 		};
```

---

## `src/common/architecture.cpp`

```diff
 const std::string ARCH_PPC     = "powerpc";
 const std::string ARCH_PPC64   = "powerpc64";
+const std::string ARCH_RISCV   = "riscv";
+const std::string ARCH_RISCV64 = "riscv64";
```

```diff
 bool Architecture::isPpc() const          { return isArch(eArch::PPC); }
 bool Architecture::isPpc64() const        { return isPpc() && getBitSize() == 64; }
+bool Architecture::isRiscv() const        { return isArch(eArch::RISCV); }
+bool Architecture::isRiscv64() const      { return isRiscv() && getBitSize() == 64; }
 bool Architecture::isKnown() const        { return !isUnknown(); }
```

```diff
 void Architecture::setIsX86()            { setName(ARCH_x86); }
 void Architecture::setIsPpc()            { setName(ARCH_PPC); }
+void Architecture::setIsRiscv()          { setName(ARCH_RISCV); }
```

In `setArch()`, **before** the MIPS check is fine; must test `riscv64` before `riscv` if you also set bit-size from the name:

```diff
 	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_PPC))
 	{
 		_arch = eArch::PPC;
 	}
+	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_RISCV64))
+	{
+		_arch = eArch::RISCV;
+		_bitSize = 64;
+	}
+	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_RISCV))
+	{
+		_arch = eArch::RISCV;
+	}
 }
```

(`ARCH_RISCV` is a prefix of `ARCH_RISCV64`; the 64-bit name must be first.)

---

## `src/bin2llvmir/optimizations/decoder/decoder_init.cpp`

```diff
 	else if (a.isArm64())
 	{
 		arch = CS_ARCH_ARM64;
 		basicMode = CS_MODE_ARM;
 	}
+	else if (a.isRiscv())
+	{
+		arch = CS_ARCH_RISCV;
+		basicMode = a.isRiscv64() ? CS_MODE_RISCV64 : CS_MODE_RISCV32;
+		extraMode = static_cast<cs_mode>(extraMode | CS_MODE_RISCVC);
+	}
 	else
 	{
 		throw std::runtime_error("Unsupported architecture.");
 	}
```

---

## `src/bin2llvmir/providers/abi/abi.cpp` — `AbiProvider::addAbi`

Includes:

```diff
 #include "retdec/bin2llvmir/providers/abi/powerpc.h"
 #include "retdec/bin2llvmir/providers/abi/x86.h"
 #include "retdec/bin2llvmir/providers/abi/x64.h"
 #include "retdec/bin2llvmir/providers/abi/pic32.h"
+#include "retdec/bin2llvmir/providers/abi/riscv.h"
+#include "retdec/bin2llvmir/providers/abi/riscv64.h"
 #include "retdec/bin2llvmir/utils/llvm.h"
```

Query methods (next to `isPic32()`):

```diff
 bool Abi::isPic32() const
 {
 	return _config->getConfig().architecture.isPic32();
 }
+
+bool Abi::isRiscv() const
+{
+	return _config->getConfig().architecture.isRiscv();
+}
+
+bool Abi::isRiscv64() const
+{
+	return _config->getConfig().architecture.isRiscv64();
+}
```

And matching declarations in `include/retdec/bin2llvmir/providers/abi/abi.h`:

```diff
 		bool isPowerPC() const;
 		bool isPowerPC64() const;
 		bool isPic32() const;
+		bool isRiscv() const;
+		bool isRiscv64() const;
```

`addAbi` body — **test 64-bit first**:

```diff
 	else if (c->getConfig().architecture.isX86())
 	{
 		auto p = _module2abi.emplace(m, std::make_unique<AbiX86>(m, c));
 		return p.first->second.get();
 	}
+	else if (c->getConfig().architecture.isRiscv64())
+	{
+		auto p = _module2abi.emplace(m, std::make_unique<AbiRiscv64>(m, c));
+		return p.first->second.get();
+	}
+	else if (c->getConfig().architecture.isRiscv())
+	{
+		auto p = _module2abi.emplace(m, std::make_unique<AbiRiscv>(m, c));
+		return p.first->second.get();
+	}
 	// ...
```

---

## `src/fileformat/file_format/elf/elf_format.cpp`

`EM_RISCV` is already `243` in `deps/elfio/include/elfio/elf_types.hpp`.

`getRelocationMask` — no RISC-V reloc map yet; accept the machine and fall through empty:

```diff
 		case EM_PPC64:
 			maps.push_back(&powerpcRelocationMap);
 			maps.push_back(&powerpc64RelocationMap);
 			break;
+		case EM_RISCV:
+			break;
 		case EM_NONE:
```

`getBytesPerWord`:

```diff
 		case EM_PPC64:
 			return 8;
+		case EM_RISCV:
+			return (elfClass == ELFCLASS64) ? 8 : 4;
 		case EM_NONE:
 			return isWiiPowerPc() ? 4 : 0;
```

`getTargetArchitecture` (requires `Architecture::RISCV` in `fftypes.h`):

```diff
 		case EM_PPC64:
 			return Architecture::POWERPC;
+		case EM_RISCV:
+			return Architecture::RISCV;
 		case EM_NONE:
```

`include/retdec/fileformat/fftypes.h`:

```diff
 enum class Architecture
 {
 	UNKNOWN,
 	X86,
 	X86_64,
 	ARM,
 	POWERPC,
 	MIPS
+	,RISCV
 };
```

---

## `src/retdec-decompiler/retdec-decompiler.cpp`

`-a` parser:

```diff
 		if (!(a == "mips" || a == "pic32" || a == "arm" || a == "thumb" || a == "arm64" || a == "powerpc" || a == "x86"
-			  || a == "x86-64"))
+			  || a == "x86-64" || a == "riscv" || a == "riscv64"))
```

Help text:

```diff
-	[-a|--arch ARCH] Specify target architecture [mips|pic32|arm|thumb|arm64|powerpc|x86|x86-64].
+	[-a|--arch ARCH] Specify target architecture [mips|pic32|arm|thumb|arm64|powerpc|x86|x86-64|riscv|riscv64].
```

---

## `src/bin2llvmir/CMakeLists.txt` (required extra)

```diff
 	providers/abi/powerpc.cpp
 	providers/abi/powerpc64.cpp
+	providers/abi/riscv.cpp
+	providers/abi/riscv64.cpp
 	providers/abi/x64.cpp
```

## `deps/capstone/CMakeLists.txt` (required extra)

```diff
 		-DCAPSTONE_X86_SUPPORT=ON
 		-DCAPSTONE_ARM64_SUPPORT=ON
+		-DCAPSTONE_RISCV_SUPPORT=ON
 		# Disabled architectures.
```

## `include/retdec/bin2llvmir/providers/asm_instruction.h` (required extra)

```diff
 #include "retdec/capstone2llvmir/powerpc/powerpc_defs.h"
+#include "retdec/capstone2llvmir/riscv/riscv_defs.h"
 #include "retdec/capstone2llvmir/x86/x86_defs.h"
```

---

## Instruction table (`_i2fm`)

Lifted (RV32I / RV64I + integer C aliases):

| Capstone id | Translator |
|---|---|
| `LUI`, `C_LUI` | `translateLui` |
| `AUIPC` | `translateAuipc` |
| `JAL`, `C_J`, `C_JAL`, `ALIAS_J`, `ALIAS_JAL` | `translateJal` |
| `JALR`, `C_JR`, `C_JALR`, `ALIAS_JR`, `ALIAS_JALR`, `ALIAS_RET` | `translateJalr` |
| `BEQ` `BNE` `BLT` `BGE` `BLTU` `BGEU`, `C_BEQZ` `C_BNEZ`, `ALIAS_BEQZ`/`BNEZ`/`BLEZ`/`BGEZ`/`BLTZ`/`BGTZ` | `translateBranch` |
| `LB` `LBU` `LH` `LHU` `LW` `LWU` `LD`, `C_LW` `C_LWSP` `C_LD` `C_LDSP`, Zcb `C_LBU` `C_LH` `C_LHU` | `translateLoad` |
| `SB` `SH` `SW` `SD`, `C_SW` `C_SWSP` `C_SD` `C_SDSP`, Zcb `C_SB` `C_SH` | `translateStore` |
| `ADD` `ADDI`, `C_ADD` `C_ADDI` `C_ADDI16SP` `C_ADDI4SPN` | `translateAdd` |
| `SUB`, `C_SUB` | `translateSub` |
| `AND` `ANDI`, `C_AND` `C_ANDI` | `translateAnd` |
| `OR` `ORI`, `C_OR` | `translateOr` |
| `XOR` `XORI`, `C_XOR` | `translateXor` |
| `SLT` `SLTI` | `translateSlt` |
| `SLTU` `SLTIU` | `translateSltu` |
| `SLL` `SLLI`, `C_SLLI` | `translateSll` |
| `SRL` `SRLI`, `C_SRLI` | `translateSrl` |
| `SRA` `SRAI`, `C_SRAI` | `translateSra` |
| `ADDW` `SUBW` `SLLW` `SRLW` `SRAW` `ADDIW` `SLLIW` `SRLIW` `SRAIW`, `C_ADDIW` `C_ADDW` `C_SUBW` | `translateOp32` |
| `C_LI` | `translateLi` |
| `C_MV` | `translateMv` |
| `ECALL` | `translateEcall` (`__pseudo_call` to `pc+size`) |
| `FENCE` `FENCE_I` `FENCE_TSO` | `translateFence` |
| `EBREAK` `C_EBREAK` `C_NOP` `UNIMP` `C_UNIMP` `ALIAS_NOP` | `translateNop` |
| `MUL` `MULH` `MULHSU` `MULHU` `MULW` `C_MUL` | `translateMul` (`mul`, high half via 2×XLEN) |
| `DIV` `DIVU` `REM` `REMU` `DIVW` `DIVUW` `REMW` `REMUW` | `translateDiv` (defined /0 and `INT_MIN/-1`) |
| `FADD_S/D` `FSUB_S/D` `FMUL_S/D` `FDIV_S/D` | `translateFpArith` |
| `FMADD_S/D` `FMSUB_S/D` `FNMADD_S/D` `FNMSUB_S/D` | `translateFpFma` (`llvm.fma`; `rd = ±(rs1*rs2) ± rs3`) |
| `FSQRT_S/D` | `translateFsqrt` (`llvm.sqrt`) |
| `FSGNJ_S/D` `FSGNJN_S/D` `FSGNJX_S/D` | `translateFsgnj` (IEEE sign-bit deposit) |
| `FMIN_S/D` `FMAX_S/D` | `translateFminMax` (`llvm.minnum` / `llvm.maxnum`) |
| `FMV_W_X` `FMV_X_W` `FMV_D_X` `FMV_X_D` | `translateFmv` (bitcast GPR↔FPR) |
| `FLW` `FLD` `C_FLW` `C_FLWSP` `C_FLD` `C_FLDSP` | `translateFpLoad` |
| `FSW` `FSD` `C_FSW` `C_FSWSP` `C_FSD` `C_FSDSP` | `translateFpStore` |
| `FCVT_{S,D}_{W,WU,L,LU}` and reverse, `FCVT_S_D` `FCVT_D_S` | `translateFcvt` |
| `FEQ_S/D` `FLT_S/D` `FLE_S/D` | `translateFcmp` |
| `FCLASS_S` `FCLASS_D` | `translateFclass` (10-bit mask, bitcast + icmp/select) |
| `AMOADD/AND/OR/XOR/SWAP/MAX/MAXU/MIN/MINU` W/D + aq/rl/aqrl | `translateAmo` (`atomicrmw`) |
| `LR_W/D` + aq/rl/aqrl | `translateLr` (atomic load) |
| `SC_W/D` + aq/rl/aqrl | `translateSc` (atomic store, `rd=0`) |
| `CSRRW` `CSRRS` `CSRRC` `CSRRWI` `CSRRSI` `CSRRCI`, `ALIAS_CSRR`/`CSRW`/`CSRS`/`CSRC`/`CSRWI`/`CSRSI`/`CSRCI`, `ALIAS_FRFLAGS`/`FSFLAGS`/`FRRM`/`FSRM`/`FRCSR`/`FSCSR`, `ALIAS_RDCYCLE`/`RDTIME`/`RDINSTRET` (+H) | `translateCsr` |

`nullptr` (pseudo-asm): half-precision `*_H`; `MRET`/`SRET`/`WFI`/`SFENCE_VMA`. Capstone 6.0.0-Alpha10 has no `RISCV_INS_URET`. Fused FP (`FMADD`/`FMSUB`/`FNMADD`/`FNMSUB`), `FSQRT`, `FSGNJ*`, `FMIN`/`FMAX`, `FCLASS_S`/`FCLASS_D`, `FMV_*` (W/X and D/X), and `AMOMAX`/`AMOMIN` (signed/unsigned, W/D + aq/rl) lift to real IR.

**Zba / Zbb / Zbs / Zicond** (x86 BMI/ABM/POPCNT / LEA / BTR/BTS/BTC parity) lift: `ANDN`/`ORN`/`XNOR` (`rs1 & ~rs2` — not x86 `~rs1 & rs2`); `CLZ`/`CTZ`/`CPOP` and W (`llvm.ctlz/cttz/ctpop`, W = low 32 then sext); `MIN`/`MINU`/`MAX`/`MAXU`; `ROL`/`ROR`/`RORI` and W; `ORC_B`; `REV8`; `BREV8`; `SEXT_B`/`SEXT_H`/`ZEXT_H`/`ZEXT_W`; `SH1ADD`/`SH2ADD`/`SH3ADD` and `*_UW`; `ADD_UW`; `SLLI_UW`; `BCLR`/`BCLRI`/`BSET`/`BSETI`/`BINV`/`BINVI`/`BEXT`/`BEXTI`; `CZERO_EQZ`/`CZERO_NEZ`. Extra Capstone modes `CS_MODE_RISCV_ZBA`/`ZBB`/`ZBKB`/`ZBS` are ORed in the translator ctor. There is no `CS_MODE_RISCV_ZICOND` token (decoder default-enables Zicond).

**Zbkb PACK** (x86 PUNPCKL* / MOVZX parity) lifts `PACK` (`rd = {rs2[XLEN/2-1:0], rs1[XLEN/2-1:0]}`), `PACKH` (`rd[15:0] = {rs2[7:0], rs1[7:0]}`, rest zero), and RV64 `PACKW` (`rd = sext_32({rs2[15:0], rs1[15:0]})`) via `translatePack`. Capstone 6 has no `RISCV_INS_C_MULW` (`C_MUL` and `MULW` already lift). Remaining Zbkb leftovers: `UNZIP`/`ZIP`/`XPERM4`/`XPERM8`.

**Zcb** (x86 MOVZX/MOVSX/NOT parity) lifts compressed `C_ZEXT_B` (`andi rd,rd,0xff` — `translateAnd`), `C_ZEXT_H` / `C_SEXT_B` / `C_SEXT_H` (`translateSextZext`), `C_ZEXT_W` (`translateShadd`, RV64 unary rd), `C_NOT` (`rd = ~rd`), and `C_LBU`/`C_LH`/`C_LHU`/`C_SB`/`C_SH`. Live dump: dest/src is the compressed GPR (`a0` = `x10`). There is no `CS_MODE_RISCV_ZCB` token; `RISCV_getFeatureBits` default-enables `FeatureStdExtZcb`. `CS_MODE_RISCV_C` is already required to decode 16-bit encodings. `C.SEXT.*`/`C.ZEXT.H` also need `CS_MODE_RISCV_ZBB`; `C.ZEXT.W` needs `ZBA` + RV64. Do not OR `CS_MODE_RISCV_ZCMP_ZCMT_ZCE` for Zcb (that token is Zcmp/Zcmt/Zce, not Zcb).

**RVV still untyped / skipped.** Capstone `riscv.h` already has `RISCV_INS_VADD_VV`, `RISCV_INS_VLE32_V`, `RISCV_INS_VSE32_V`, `RISCV_REG_V0`..`V31` (plus LMUL groups), and `CS_MODE_RISCV_V`. Those vector registers are **not** in `initializeRegTypeMap` (only `VL`/`VTYPE`/`VLENB`/`VXSAT`/`VXRM` CSRs are). Lifting even that compiler subset would add a new register file whose width is not in the header: i128 or scalable LLVM types would invent a SEW/LMUL-independent layout, and `vadd.vv`/`vle32`/`vse32` also need `vl`/`vtype`/masking. Do not invent `V0`..`V31` types until that file is a scoped task.

Capstone 6 auto-sync reports `id=ADDI/JAL/CSRRS` with `alias_id=ALIAS_NOP/JAL/FRFLAGS` and often alias operand layouts. The translator prefers `_i2fm[alias_id]`, requests `CS_OPT_DETAIL_REAL`, and treats control-flow IMM as an effective address unless `need_effective_addr` is set. Decoding F/D/A encodings requires extra `CS_MODE_RISCV_FD` and `CS_MODE_RISCV_A` (in addition to `CS_MODE_RISCVC`). Zba/Zbb/Zbkb/Zbs decode requires `CS_MODE_RISCV_ZBA`/`ZBB`/`ZBKB`/`ZBS` (now enabled by the RISC-V translator). `CLMUL`/`Zbc` stay leftover (`x86 PCLMULQDQ` is `nullptr`). Half-precision `*_H` FP, `MRET`/`SRET`/`WFI`, Zbkb `UNZIP`/`ZIP`/`XPERM4`/`XPERM8`, and RVV stay unmapped. Do not invent `V0`..`V31`.

## ABI (psABI)

- Args: `x10`–`x17` (`a0`–`a7`) — syscall args `a0`–`a6`, id `a7`
- Return: `x10` (`a0`)
- `ra` = `x1`, `sp` = `x2`, `s0`/`fp` = `x8`
- `x0` hardwired zero (load 0, store discarded)
- `_defcc = CC_UNKNOWN` until a `CC_RISCV` calling-convention class is wired

## Tests

`tests/capstone2llvmir/riscv_tests.cpp` uses `emulate_bin()` hex sequences (Keystone has no RISC-V). Instantiated for `CS_MODE_RISCV32` and `CS_MODE_RISCV64`. Named ctors are called with extra `CS_MODE_RISCVC | CS_MODE_RISCV_FD | CS_MODE_RISCV_A` so Capstone 6 actually decodes compressed, float, and AMO encodings. Coverage includes integer RV32I/RV64I plus `MUL`/`DIV`, `FADD.S`/`FLW`/`FSW`/`C.FLW`, fused `FMADD.S`, `FSQRT.S`, `FSGNJ.S`, `FMIN.S`, `FMV.W.X`, `FCLASS.S`/`FCLASS.D`, `AMOADD.W`/`AMOMIN.W`/`LR.W`/`SC.W`, and `CSRRS`/`CSRRW` of `fflags`.
