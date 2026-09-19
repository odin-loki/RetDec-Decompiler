/**
 * @file src/capstone2llvmir/mips/mips.cpp
 * @brief MIPS implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cmath>
#include <iomanip>
#include <vector>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

#include "capstone2llvmir/mips/mips_impl.h"

namespace retdec {
namespace capstone2llvmir {

Capstone2LlvmIrTranslatorMips_impl::Capstone2LlvmIrTranslatorMips_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(CS_ARCH_MIPS, basic, extra, m)
{
	// This needs to be called from concrete's class ctor, not abstract's
	// class ctor, so that virtual table is properly initialized.
	initialize();
}

//
//==============================================================================
// Mode query & modification methods - from Capstone2LlvmIrTranslator.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorMips_impl::isAllowedBasicMode(cs_mode m)
{
	return m == CS_MODE_MIPS32
			|| m == CS_MODE_MIPS64
			|| m == CS_MODE_MIPS3
			|| m == CS_MODE_MIPS32R6;
}

bool Capstone2LlvmIrTranslatorMips_impl::isAllowedExtraMode(cs_mode m)
{
	return m == CS_MODE_LITTLE_ENDIAN
			|| m == CS_MODE_BIG_ENDIAN
			|| m == CS_MODE_MICRO;
}

uint32_t Capstone2LlvmIrTranslatorMips_impl::getArchByteSize()
{
	switch (_origBasicMode)
	{
		case CS_MODE_MIPS32:
		case CS_MODE_MIPS32R6:
		case CS_MODE_MIPS3:
			return 4;
		case CS_MODE_MIPS64:
			return 8;
		default:
		{
			throw GenericError("Unhandled mode in getArchByteSize().");
			break;
		}
	}
}

//
//==============================================================================
// Capstone related getters - from Capstone2LlvmIrTranslator.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorMips_impl::hasDelaySlot(uint32_t id) const
{
	return getDelaySlot(id);
}

bool Capstone2LlvmIrTranslatorMips_impl::hasDelaySlotTypical(uint32_t id) const
{
	return getDelaySlot(id) && !hasDelaySlotLikely(id);
}

bool Capstone2LlvmIrTranslatorMips_impl::hasDelaySlotLikely(uint32_t id) const
{
	static std::set<uint32_t> set =
	{
			MIPS_INS_BC1FL, MIPS_INS_BC1TL, MIPS_INS_BEQL, MIPS_INS_BNEL,
			MIPS_INS_BLEZL, MIPS_INS_BGTZL, MIPS_INS_BLTZL, MIPS_INS_BGEZL,
			MIPS_INS_BGEZALL, MIPS_INS_BLTZALL
	};
	return set.count(id);
}

/**
 * At the moment, all instructions here have delay slot of size 1.
 * If there are some instructions with different sized slots, we will need map.
 */
std::size_t Capstone2LlvmIrTranslatorMips_impl::getDelaySlot(uint32_t id) const
{
	static std::set<uint32_t> set =
	{
			// cond branch
			MIPS_INS_BC1F, MIPS_INS_BC1FL, MIPS_INS_BC1T, MIPS_INS_BC1TL,
			MIPS_INS_BEQ, MIPS_INS_BEQL, MIPS_INS_BNE, MIPS_INS_BNEL,
			MIPS_INS_BLEZ, MIPS_INS_BLEZL, MIPS_INS_BGTZ, MIPS_INS_BGTZL,
			MIPS_INS_BLTZ, MIPS_INS_BLTZL, MIPS_INS_BGEZ, MIPS_INS_BGEZL,
			MIPS_INS_BEQZ, MIPS_INS_BNEZ,
			MIPS_INS_ALIAS_BEQZ, MIPS_INS_ALIAS_BNEZ,
			// microMIPS 16-bit branches/jumps still have a delay slot.
			MIPS_INS_B16, MIPS_INS_BEQZ16, MIPS_INS_BNEZ16,
			MIPS_INS_JR16, MIPS_INS_JALRS16, MIPS_INS_JALRS,
			MIPS_INS_JRADDIUSP,
			// call
			MIPS_INS_BGEZAL, MIPS_INS_BGEZALL, MIPS_INS_BLTZAL,
			MIPS_INS_BLTZALL, MIPS_INS_JAL, MIPS_INS_JALR,
			MIPS_INS_BAL, MIPS_INS_ALIAS_BAL,
			// branch
			MIPS_INS_J, MIPS_INS_JR,
			MIPS_INS_B, MIPS_INS_ALIAS_B, MIPS_INS_ALIAS_JR,
	};
	// Compact R6 (BC, BALC, BEQC, BEQZC, JIC, JRC, JALRC, …) is intentionally
	// absent: those encodings have no delay slot.
	return set.count(id);
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorMips_impl::generateEnvironmentArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorMips_impl::generateDataLayout()
{
	switch (_basicMode)
	{
		case CS_MODE_MIPS32:
		case CS_MODE_MIPS32R6:
		case CS_MODE_MIPS3:
			_module->setDataLayout("e-p:32:32:32-f80:32:32");
			break;
		case CS_MODE_MIPS64:
			_module->setDataLayout("e-p:64:64:64-i8:8:32-i16:16:32-i64:64-n32:64-S128");
			break;
		default:
		{
			throw GenericError("Unhandled mode in generateDataLayout().");
			break;
		}
	}
}

void Capstone2LlvmIrTranslatorMips_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		createRegister(p.first, _regLt);
	}
}

uint32_t Capstone2LlvmIrTranslatorMips_impl::getCarryRegister()
{
	return MIPS_REG_INVALID;
}

/**
 * True when any operand names an MSA vector register. Modelled MSA
 * (AND_V/OR_V/XOR_V/NOR_V, ADDV/SUBV/MULV lanes, ILVEV/ILVOD/ILVL/ILVR,
 * PCKEV/PCKOD, SHF, SPLAT/SPLATI, ANDI_B/ORI_B/XORI_B/NORI_B, BSEL_V/BSELI,
 * CEQ/CEQI/CLE_S/CLT_S, MAX_S/MIN_S, SLL/SRL lanes, COPY_S/COPY_U,
 * INSERT/FILL/LDI, ADDS_S/U / SUBS_S/U, SAT_S/U, SLD/SLDI,
 * FADD_W/FSUB_W/FMUL_W/FDIV_W/FMAX_W/FMIN_W, LD/ST of the whole vector) is
 * i128; every other MSA id still goes to a pseudo-assembly call.
 */
bool Capstone2LlvmIrTranslatorMips_impl::hasMsaOperand(cs_mips* mi) const
{
	for (unsigned k = 0; k < mi->op_count; ++k)
	{
		auto& op = mi->operands[k];
		if (op.type == MIPS_OP_REG && MIPS_REG_W0 <= op.reg && op.reg <= MIPS_REG_W31)
		{
			return true;
		}
	}

	return false;
}

void Capstone2LlvmIrTranslatorMips_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_mips* mi = &d->mips;

	// Capstone 6 keeps the real id (SLL/ADDIU/BEQ) and puts nop/li/beqz in
	// alias_id. Prefer a dedicated alias translator so delay-slot nop stays
	// empty and li keeps two operands.
	std::size_t id = i->id;
	if (i->is_alias)
	{
		auto aIt = _i2fm.find(static_cast<std::size_t>(i->alias_id));
		if (aIt != _i2fm.end() && aIt->second != nullptr)
		{
			id = static_cast<std::size_t>(i->alias_id);
		}
	}

	// Capstone 6 gives MSA its own ids (`AND_V`, `LD_B`, `ORI_B`, …). The
	// modelled subset (128-bit bitwise, ADDV/SUBV/MULV lanes, ILV*/PCK*,
	// SHF/SPLAT/SPLATI/MAX_S, ANDI_B/ORI_B/XORI_B/NORI_B, BSEL_V/BSELI,
	// CEQ/CEQI/CLE_S/CLT_S, COPY_S/U, INSERT/FILL/LDI, ADDS_S/U / SUBS_S/U,
	// SAT_S/U, SLD/SLDI, FADD_W/FSUB_W/FMUL_W/FDIV_W/FMAX_W/FMIN_W,
	// LD_*/ST_* whole vector) is handled below; everything else with a W
	// register still goes to a pseudo-assembly call rather than a scalar
	// translator.
	if (hasMsaOperand(mi) && !isMsaModelledId(static_cast<uint32_t>(id)))
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto fIt = _i2fm.find(id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;
		(this->*f)(i, mi, irb);
	}
	else
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, mi, irb);
	}
}

//
//==============================================================================
// MIPS-specific methods.
//==============================================================================
//

llvm::Value* Capstone2LlvmIrTranslatorMips_impl::getCurrentPc(cs_insn* i)
{
	return getNextInsnAddress(i);
}

/**
 * MIPS specifications often says something like:
 * "The return link is the address of the second instruction following the,
 * branch, at which location execution continues after a procedure call."
 * This method returns this address as an LLVM @c ConstantInt.
 */
llvm::Value* Capstone2LlvmIrTranslatorMips_impl::getNextNextInsnAddress(cs_insn* i)
{
	return llvm::ConstantInt::get(getDefaultType(), i->address + (2 * i->size));
}

/**
 * @return @c Nullptr -- there is no value.
 *
 * @c Nullptr will cause all the consumers like @c storeRegisterUnpredictable()
 * not to generate any code that depends on unpredictable value.
 *
 * MIPS specifications says:
 * "... Software can never depend on results that are UNPREDICTABLE.
 * UNPREDICTABLE operations may cause a result to be generated or not. ..."
 *
 * Right now, we choose not to generate it. This may change in future.
 */
llvm::Value* Capstone2LlvmIrTranslatorMips_impl::getUnpredictableValue()
{
	return nullptr;
}

/// Capstone 6 reports paired doubles as D0–D15, 64-bit FPRs as D0_64–D31_64,
/// and N64 GPRs as ZERO_64 / AT_64 / …. Fold those onto the F / GPR ids
/// this translator already models.
static uint32_t canonicalizeMipsReg(uint32_t r)
{
	if (r >= MIPS_REG_D0 && r <= MIPS_REG_D15)
	{
		return MIPS_REG_F0 + 2 * (r - MIPS_REG_D0);
	}
	if (r >= MIPS_REG_D0_64 && r <= MIPS_REG_D31_64)
	{
		return MIPS_REG_F0 + (r - MIPS_REG_D0_64);
	}
	if (r >= MIPS_REG_A0_64 && r <= MIPS_REG_A3_64)
	{
		return MIPS_REG_A0 + (r - MIPS_REG_A0_64);
	}
	if (r >= MIPS_REG_S0_64 && r <= MIPS_REG_S7_64)
	{
		return MIPS_REG_S0 + (r - MIPS_REG_S0_64);
	}
	if (r >= MIPS_REG_T0_64 && r <= MIPS_REG_T9_64)
	{
		return MIPS_REG_T0 + (r - MIPS_REG_T0_64);
	}
	switch (r)
	{
		case MIPS_REG_ZERO_64: return MIPS_REG_ZERO;
		case MIPS_REG_AT_64: return MIPS_REG_AT;
		case MIPS_REG_V0_64: return MIPS_REG_V0;
		case MIPS_REG_V1_64: return MIPS_REG_V1;
		case MIPS_REG_K0_64: return MIPS_REG_K0;
		case MIPS_REG_K1_64: return MIPS_REG_K1;
		case MIPS_REG_GP_64: return MIPS_REG_GP;
		case MIPS_REG_SP_64: return MIPS_REG_SP;
		case MIPS_REG_FP_64: return MIPS_REG_FP;
		case MIPS_REG_RA_64: return MIPS_REG_RA;
		case MIPS_REG_HI0_64: return MIPS_REG_HI;
		case MIPS_REG_LO0_64: return MIPS_REG_LO;
		default: return r;
	}
}

static bool isCanonicalFpReg(uint32_t r)
{
	r = canonicalizeMipsReg(r);
	return MIPS_REG_F0 <= r && r <= MIPS_REG_F31;
}

uint32_t Capstone2LlvmIrTranslatorMips_impl::singlePrecisionToDoublePrecisionFpRegister(
		uint32_t r) const
{
	// Working with odd double reg (e.g. sdc1 $f21, -0x7ba3($v1)) may happen.
	// I have no idea why, and if this is ok, or it is simply caused by decoding
	// data. But it is a real example from real binary, IDA has the same thing.
	// Right now, we map odd numbers to even ones. But we would be able to
	// create their own double registers very easily.
	switch (r)
	{
		case MIPS_REG_F0: return MIPS_REG_FD0;
		case MIPS_REG_F1: return MIPS_REG_FD0;
		case MIPS_REG_F2: return MIPS_REG_FD2;
		case MIPS_REG_F3: return MIPS_REG_FD2;
		case MIPS_REG_F4: return MIPS_REG_FD4;
		case MIPS_REG_F5: return MIPS_REG_FD4;
		case MIPS_REG_F6: return MIPS_REG_FD6;
		case MIPS_REG_F7: return MIPS_REG_FD6;
		case MIPS_REG_F8: return MIPS_REG_FD8;
		case MIPS_REG_F9: return MIPS_REG_FD8;
		case MIPS_REG_F10: return MIPS_REG_FD10;
		case MIPS_REG_F11: return MIPS_REG_FD10;
		case MIPS_REG_F12: return MIPS_REG_FD12;
		case MIPS_REG_F13: return MIPS_REG_FD12;
		case MIPS_REG_F14: return MIPS_REG_FD14;
		case MIPS_REG_F15: return MIPS_REG_FD14;
		case MIPS_REG_F16: return MIPS_REG_FD16;
		case MIPS_REG_F17: return MIPS_REG_FD16;
		case MIPS_REG_F18: return MIPS_REG_FD18;
		case MIPS_REG_F19: return MIPS_REG_FD18;
		case MIPS_REG_F20: return MIPS_REG_FD20;
		case MIPS_REG_F21: return MIPS_REG_FD20;
		case MIPS_REG_F22: return MIPS_REG_FD22;
		case MIPS_REG_F23: return MIPS_REG_FD22;
		case MIPS_REG_F24: return MIPS_REG_FD24;
		case MIPS_REG_F25: return MIPS_REG_FD24;
		case MIPS_REG_F26: return MIPS_REG_FD26;
		case MIPS_REG_F27: return MIPS_REG_FD26;
		case MIPS_REG_F28: return MIPS_REG_FD28;
		case MIPS_REG_F29: return MIPS_REG_FD28;
		case MIPS_REG_F30: return MIPS_REG_FD30;
		case MIPS_REG_F31: return MIPS_REG_FD30;
		default:
			throw GenericError("Can not convert to double precision "
					"register.");
	}
}

llvm::Value* Capstone2LlvmIrTranslatorMips_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == MIPS_REG_INVALID)
	{
		return nullptr;
	}

	if (r == MIPS_REG_PC)
	{
		return getCurrentPc(_insn);
	}

	r = canonicalizeMipsReg(r);

	if (r == MIPS_REG_ZERO)
	{
		return llvm::ConstantInt::getSigned(getDefaultType(), 0);
	}

	if (cs_insn_group(_handle, _insn, MIPS_GRP_NOTFP64BIT)
			&& MIPS_REG_F0 <= r
			&& r <= MIPS_REG_F31)
	{
		r = singlePrecisionToDoublePrecisionFpRegister(r);
	}

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);

	return createLoad(irb, llvmReg);
}

llvm::Value* Capstone2LlvmIrTranslatorMips_impl::loadOp(
		cs_mips_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case MIPS_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case MIPS_OP_IMM:
		{
			auto* t = getDefaultType();
			return llvm::ConstantInt::get(t, llvm::APInt(t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm), false, /*implicitTrunc=*/true));
		}
		case MIPS_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
			auto* t = getDefaultType();
			llvm::Value* disp = llvm::ConstantInt::getSigned(t, op.mem.disp);

			llvm::Value* addr = nullptr;
			if (baseR == nullptr)
			{
				addr = disp;
			}
			else
			{
				if (op.mem.disp == 0)
				{
					addr = baseR;
				}
				else
				{
					disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
					addr = irb.CreateAdd(baseR, disp);
				}
			}

			if (lea)
			{
				return addr;
			}
			else
			{
				auto* lty = ty ? ty : t;
				return loadIntPtr(irb, addr, lty);
			}
		}
		case MIPS_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::StoreInst* Capstone2LlvmIrTranslatorMips_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == MIPS_REG_INVALID)
	{
		return nullptr;
	}
	// These registers should not be stored, or their store has no effect.
	//
	r = canonicalizeMipsReg(r);

	if (r == MIPS_REG_PC
			|| r == MIPS_REG_ZERO)
	{
		return nullptr;
	}

	if (cs_insn_group(_handle, _insn, MIPS_GRP_NOTFP64BIT)
			&& MIPS_REG_F0 <= r
			&& r <= MIPS_REG_F31)
	{
		r = singlePrecisionToDoublePrecisionFpRegister(r);
	}

	auto* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("storeRegister() unhandled reg.");
	}
	val = generateTypeConversion(irb, val, llvmReg->getValueType(),  llvmReg->getValueType()->isFloatingPointTy()? eOpConv::FPCAST_OR_BITCAST : ct);

	auto* s = irb.CreateStore(val, llvmReg);
	attachPointeeType(s, llvmReg->getValueType());
	return s;
}

/**
 * Store unpredictable value to register @a r.
 * No store is generated if unpredictable value is set to @c nullptr (see
 * @c getUnpredictableValue()).
 */
llvm::StoreInst* Capstone2LlvmIrTranslatorMips_impl::storeRegisterUnpredictable(
		uint32_t r,
		llvm::IRBuilder<>& irb)
{
	auto* u = getUnpredictableValue();
	return u ? storeRegister(r, u, irb) : nullptr;
}

/**
 * @a ct is used when storing a value to register with a different type.
 * When storing to memory, value type is used -- therefore it needs to be
 * converted to the desired type prior to @c storeOp() call.
 */
llvm::Instruction* Capstone2LlvmIrTranslatorMips_impl::storeOp(
		cs_mips_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case MIPS_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case MIPS_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
			auto* t = getDefaultType();
			llvm::Value* disp = llvm::ConstantInt::getSigned(t, op.mem.disp);

			llvm::Value* addr = nullptr;
			if (baseR == nullptr)
			{
				addr = disp;
			}
			else
			{
				if (op.mem.disp == 0)
				{
					addr = baseR;
				}
				else
				{
					disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
					addr = irb.CreateAdd(baseR, disp);
				}
			}

			return storeIntPtr(irb, val, addr, val->getType());
		}
		case MIPS_OP_IMM:
		case MIPS_OP_INVALID:
		default:
		{
			throw GenericError("should not be possible");
		}
	}
}

bool Capstone2LlvmIrTranslatorMips_impl::isFpInstructionVariant(cs_insn* i)
{
	auto& mi = i->detail->mips;
	if (mi.op_count == 0 || mi.operands[0].type != MIPS_OP_REG)
	{
		return false;
	}
	uint32_t r = canonicalizeMipsReg(mi.operands[0].reg);
	return MIPS_REG_F0 <= r && r <= MIPS_REG_F31;
}

bool Capstone2LlvmIrTranslatorMips_impl::isFccRegister(uint32_t r) const
{
	return MIPS_REG_FCC0 <= r && r <= MIPS_REG_FCC7;
}

bool Capstone2LlvmIrTranslatorMips_impl::isMsaBitwiseId(uint32_t id) const
{
	switch (id)
	{
		case MIPS_INS_AND_V:
		case MIPS_INS_OR_V:
		case MIPS_INS_XOR_V:
		case MIPS_INS_NOR_V:
			return true;
		default:
			return false;
	}
}

bool Capstone2LlvmIrTranslatorMips_impl::isMsaModelledId(uint32_t id) const
{
	if (isMsaBitwiseId(id))
	{
		return true;
	}

	switch (id)
	{
		case MIPS_INS_ADDV_B:
		case MIPS_INS_ADDV_H:
		case MIPS_INS_ADDV_W:
		case MIPS_INS_ADDV_D:
		case MIPS_INS_ADDS_S_B:
		case MIPS_INS_ADDS_S_H:
		case MIPS_INS_ADDS_S_W:
		case MIPS_INS_ADDS_S_D:
		case MIPS_INS_ADDS_U_B:
		case MIPS_INS_ADDS_U_H:
		case MIPS_INS_ADDS_U_W:
		case MIPS_INS_ADDS_U_D:
		case MIPS_INS_SUBV_B:
		case MIPS_INS_SUBV_H:
		case MIPS_INS_SUBV_W:
		case MIPS_INS_SUBV_D:
		case MIPS_INS_SUBS_S_B:
		case MIPS_INS_SUBS_S_H:
		case MIPS_INS_SUBS_S_W:
		case MIPS_INS_SUBS_S_D:
		case MIPS_INS_SUBS_U_B:
		case MIPS_INS_SUBS_U_H:
		case MIPS_INS_SUBS_U_W:
		case MIPS_INS_SUBS_U_D:
		case MIPS_INS_MULV_B:
		case MIPS_INS_MULV_H:
		case MIPS_INS_MULV_W:
		case MIPS_INS_MULV_D:
		case MIPS_INS_ILVEV_B:
		case MIPS_INS_ILVEV_H:
		case MIPS_INS_ILVEV_W:
		case MIPS_INS_ILVEV_D:
		case MIPS_INS_ILVOD_B:
		case MIPS_INS_ILVOD_H:
		case MIPS_INS_ILVOD_W:
		case MIPS_INS_ILVOD_D:
		case MIPS_INS_ILVL_B:
		case MIPS_INS_ILVL_H:
		case MIPS_INS_ILVL_W:
		case MIPS_INS_ILVL_D:
		case MIPS_INS_ILVR_B:
		case MIPS_INS_ILVR_H:
		case MIPS_INS_ILVR_W:
		case MIPS_INS_ILVR_D:
		case MIPS_INS_PCKEV_B:
		case MIPS_INS_PCKEV_H:
		case MIPS_INS_PCKEV_W:
		case MIPS_INS_PCKEV_D:
		case MIPS_INS_PCKOD_B:
		case MIPS_INS_PCKOD_H:
		case MIPS_INS_PCKOD_W:
		case MIPS_INS_PCKOD_D:
		case MIPS_INS_ANDI_B:
		case MIPS_INS_ORI_B:
		case MIPS_INS_XORI_B:
		case MIPS_INS_NORI_B:
		case MIPS_INS_BSEL_V:
		case MIPS_INS_BSELI_B:
		case MIPS_INS_COPY_S_B:
		case MIPS_INS_COPY_S_H:
		case MIPS_INS_COPY_S_W:
		case MIPS_INS_COPY_S_D:
		case MIPS_INS_COPY_U_B:
		case MIPS_INS_COPY_U_H:
		case MIPS_INS_COPY_U_W:
		case MIPS_INS_INSERT_B:
		case MIPS_INS_INSERT_H:
		case MIPS_INS_INSERT_W:
		case MIPS_INS_INSERT_D:
		case MIPS_INS_FILL_B:
		case MIPS_INS_FILL_H:
		case MIPS_INS_FILL_W:
		case MIPS_INS_FILL_D:
		case MIPS_INS_LDI_B:
		case MIPS_INS_LDI_H:
		case MIPS_INS_LDI_W:
		case MIPS_INS_LDI_D:
		case MIPS_INS_CEQ_B:
		case MIPS_INS_CEQ_H:
		case MIPS_INS_CEQ_W:
		case MIPS_INS_CEQ_D:
		case MIPS_INS_CEQI_B:
		case MIPS_INS_CEQI_H:
		case MIPS_INS_CEQI_W:
		case MIPS_INS_CEQI_D:
		case MIPS_INS_CLE_S_B:
		case MIPS_INS_CLE_S_H:
		case MIPS_INS_CLE_S_W:
		case MIPS_INS_CLE_S_D:
		case MIPS_INS_CLT_S_B:
		case MIPS_INS_CLT_S_H:
		case MIPS_INS_CLT_S_W:
		case MIPS_INS_CLT_S_D:
		case MIPS_INS_FADD_W:
		case MIPS_INS_FSUB_W:
		case MIPS_INS_FMUL_W:
		case MIPS_INS_FDIV_W:
		case MIPS_INS_FMAX_W:
		case MIPS_INS_FMIN_W:
		case MIPS_INS_SHF_B:
		case MIPS_INS_SHF_H:
		case MIPS_INS_SHF_W:
		case MIPS_INS_SPLAT_B:
		case MIPS_INS_SPLAT_H:
		case MIPS_INS_SPLAT_W:
		case MIPS_INS_SPLAT_D:
		case MIPS_INS_SPLATI_B:
		case MIPS_INS_SPLATI_H:
		case MIPS_INS_SPLATI_W:
		case MIPS_INS_SPLATI_D:
		case MIPS_INS_MAX_S_B:
		case MIPS_INS_MAX_S_H:
		case MIPS_INS_MAX_S_W:
		case MIPS_INS_MAX_S_D:
		case MIPS_INS_MIN_S_B:
		case MIPS_INS_MIN_S_H:
		case MIPS_INS_MIN_S_W:
		case MIPS_INS_MIN_S_D:
		case MIPS_INS_SLL_B:
		case MIPS_INS_SLL_H:
		case MIPS_INS_SLL_W:
		case MIPS_INS_SLL_D:
		case MIPS_INS_SRL_B:
		case MIPS_INS_SRL_H:
		case MIPS_INS_SRL_W:
		case MIPS_INS_SRL_D:
		case MIPS_INS_SAT_S_B:
		case MIPS_INS_SAT_S_H:
		case MIPS_INS_SAT_S_W:
		case MIPS_INS_SAT_S_D:
		case MIPS_INS_SAT_U_B:
		case MIPS_INS_SAT_U_H:
		case MIPS_INS_SAT_U_W:
		case MIPS_INS_SAT_U_D:
		case MIPS_INS_SLD_B:
		case MIPS_INS_SLD_H:
		case MIPS_INS_SLD_W:
		case MIPS_INS_SLD_D:
		case MIPS_INS_SLDI_B:
		case MIPS_INS_SLDI_H:
		case MIPS_INS_SLDI_W:
		case MIPS_INS_SLDI_D:
		case MIPS_INS_LD_B:
		case MIPS_INS_LD_H:
		case MIPS_INS_LD_W:
		case MIPS_INS_LD_D:
		case MIPS_INS_ST_B:
		case MIPS_INS_ST_H:
		case MIPS_INS_ST_W:
		case MIPS_INS_ST_D:
			return true;
		default:
			return false;
	}
}

bool Capstone2LlvmIrTranslatorMips_impl::isOperandRegister(cs_mips_op& op)
{
	return op.type == MIPS_OP_REG;
}

bool Capstone2LlvmIrTranslatorMips_impl::isGeneralPurposeRegister(uint32_t r)
{
	r = canonicalizeMipsReg(r);
	switch (r)
	{
		case MIPS_REG_ZERO:
		case MIPS_REG_AT:
		case MIPS_REG_V0: case MIPS_REG_V1:
		case MIPS_REG_A0: case MIPS_REG_A1: case MIPS_REG_A2: case MIPS_REG_A3:
		case MIPS_REG_T0: case MIPS_REG_T1: case MIPS_REG_T2: case MIPS_REG_T3:
		case MIPS_REG_T4: case MIPS_REG_T5: case MIPS_REG_T6: case MIPS_REG_T7:
		case MIPS_REG_S0: case MIPS_REG_S1: case MIPS_REG_S2: case MIPS_REG_S3:
		case MIPS_REG_S4: case MIPS_REG_S5: case MIPS_REG_S6: case MIPS_REG_S7:
		case MIPS_REG_T8: case MIPS_REG_T9:
		case MIPS_REG_K0: case MIPS_REG_K1:
		case MIPS_REG_GP: case MIPS_REG_SP: case MIPS_REG_FP: case MIPS_REG_RA:
			return true;
		default:
			return false;
	}
}

// Compact R6 (and compact microMIPS R6) control: link is the next instruction,
// not the instruction after a delay slot.
static bool isCompactLinkId(uint32_t id)
{
	switch (id)
	{
		case MIPS_INS_BALC:
		case MIPS_INS_JALRC:
		case MIPS_INS_JIALC:
		case MIPS_INS_BEQZALC:
		case MIPS_INS_BNEZALC:
		case MIPS_INS_BGEZALC:
		case MIPS_INS_BLTZALC:
		case MIPS_INS_BGTZALC:
		case MIPS_INS_BLEZALC:
			return true;
		default:
			return false;
	}
}

//
//==============================================================================
// MIPS instruction translation methods.
//==============================================================================
//

/**
 * MIPS_INS_ADDI, MIPS_INS_ADDIU, MIPS_INS_ADD, MIPS_INS_ADDU,
 * MIPS_INS_ADDU16
 */
void Capstone2LlvmIrTranslatorMips_impl::translateAdd(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST, eOpConv::FPCAST_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);
	auto* add = op1->getType()->isFloatingPointTy()
			? irb.CreateFAdd(op1, op2)
			: irb.CreateAdd(op1, op2);
	storeOp(mi->operands[0], add, irb);
}

/**
 * MIPS_INS_ABS, MIPS_INS_ABS_S, MIPS_INS_ABS_D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateAbs(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	if (op1->getType()->isFloatingPointTy())
	{
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, op1->getType());
		storeOp(mi->operands[0], irb.CreateCall(f, {op1}), irb);
		return;
	}

	auto* zero = llvm::ConstantInt::get(op1->getType(), 0);
	auto* neg = irb.CreateSub(zero, op1);
	auto* val = irb.CreateSelect(irb.CreateICmpSLT(op1, zero), neg, op1);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_AND, MIPS_INS_ANDI, MIPS_INS_AND_V, MIPS_INS_AND16, MIPS_INS_ANDI16
 */
void Capstone2LlvmIrTranslatorMips_impl::translateAnd(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* a = irb.CreateAnd(op1, op2);
	storeOp(mi->operands[0], a, irb);
}

/**
 * MIPS_INS_BC1F, MIPS_INS_BC1FL
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBc1f(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, mi, irb);

	if (mi->op_count == 1)
	{
		op0 = loadRegister(MIPS_REG_FCC0, irb); // implied operand
		op1 = loadOpUnary(mi, irb);
	}
	else if (mi->op_count == 2)
	{
		std::tie(op0, op1) = loadOpBinary(mi, irb);
	}

	auto* c = irb.CreateICmpEQ(op0, llvm::ConstantInt::get(op0->getType(), 0));
	generateCondBranchFunctionCall(irb, c, op1);
}

/**
 * MIPS_INS_BC1T, MIPS_INS_BC1TL
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBc1t(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, mi, irb);

	if (mi->op_count == 1)
	{
		op0 = loadRegister(MIPS_REG_FCC0, irb); // implied operand
		op1 = loadOpUnary(mi, irb);
	}
	else if (mi->op_count == 2)
	{
		std::tie(op0, op1) = loadOpBinary(mi, irb);
	}

	auto* c = irb.CreateICmpNE(op0, llvm::ConstantInt::get(op0->getType(), 0));
	generateCondBranchFunctionCall(irb, c, op1);
}

/**
 * MIPS_INS_BGEZAL (and link), MIPS_INS_BGEZALL (and link likely -- executes
 * the delay slot only if the branch is taken).
 * Bodies are the same, but delay slot eecution differs:
 * - MIPS_INS_BGEZAL: delay slot always executed -> should be moved before jump.
 * - MIPS_INS_BGEZALL: delay slot execution only if jump taken -> should be
 *   moved to branch target.
 *
 * The same for:
 * MIPS_INS_BLTZAL, MIPS_INS_BLTZALL
 * Compact R6 *ALC forms (BEQZALC, BGEZALC, …): same compare, but the link
 * is the next instruction — they have no delay slot.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBcondal(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	std::tie(op0, op1) = loadOpBinary(mi, irb);
	auto* zero = llvm::ConstantInt::get(op0->getType(), 0);
	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case MIPS_INS_BGEZAL:
		case MIPS_INS_BGEZALL:
		case MIPS_INS_BGEZALC:
			cond = irb.CreateICmpSGE(op0, zero);
			break;
		case MIPS_INS_BLTZAL:
		case MIPS_INS_BLTZALL:
		case MIPS_INS_BLTZALC:
			cond = irb.CreateICmpSLT(op0, zero);
			break;
		case MIPS_INS_BGTZALC:
			cond = irb.CreateICmpSGT(op0, zero);
			break;
		case MIPS_INS_BLEZALC:
			cond = irb.CreateICmpSLE(op0, zero);
			break;
		case MIPS_INS_BEQZALC:
			cond = irb.CreateICmpEQ(op0, zero);
			break;
		case MIPS_INS_BNEZALC:
			cond = irb.CreateICmpNE(op0, zero);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateBcondal().");
	}

	llvm::IRBuilder<> bodyIrb(generateIfThen(cond, irb));

	llvm::Value* ra = isCompactLinkId(i->id) ? getNextInsnAddress(i) : getNextNextInsnAddress(i);
	storeRegister(MIPS_REG_RA, ra, bodyIrb);
	generateCallFunctionCall(bodyIrb, op1);
}

/**
 * MIPS_INS_RDHWR -- read a hardware register.
 *
 * `rdhwr rt, $29` reads hardware register 29, ULR ("user local"), which is the
 * thread pointer: 19,389 occurrences in the static parity corpus, every one of
 * them `$29`, and the single largest unmodelled pseudo-assembly call on any of
 * the five architectures.
 *
 * The trap is in how Capstone reports it. The hardware-register number comes
 * back as an ordinary GPR id, and MIPS_REG_29 is the same id as MIPS_REG_SP --
 * so a translation that took operand 1 at face value and loaded it would read
 * the stack pointer and produce something that looks entirely plausible. The
 * number is a selector into a different register file, and it is compared as a
 * number here rather than loaded.
 *
 * The other hardware registers ($0 CPUNum, $1 SYNCI_Step, $2 CC, $3 CCRes)
 * stay on the pseudo-assembly path: a cycle counter is not a value this
 * decompiler can model, and pretending otherwise would be worse than the call.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateRdhwr(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	if (mi->operands[0].type != MIPS_OP_REG || mi->operands[1].type != MIPS_OP_REG
		|| mi->operands[1].reg != MIPS_REG_29)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	storeOp(mi->operands[0], loadRegister(MIPS_REG_HWR_ULR, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_INS -- `ins rt, rs, pos, size`
 *
 * The other half of EXT: rt keeps its bits outside the field and takes the
 * bottom `size` bits of rs inside it. 2,580 occurrences in the static corpus,
 * and it is the only one of the pair that reads its destination.
 *
 *     rt = (rt & ~mask) | ((rs << pos) & mask)     mask = ((1<<size)-1) << pos
 */
void Capstone2LlvmIrTranslatorMips_impl::translateIns(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	op3 = loadOp(mi->operands[3], irb);

	// 32-bit registers only, for the reason EXT is: DINS is the 64-bit one.
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(op1->getType());
	if (ty == nullptr || ty->getBitWidth() != 32 || !llvm::isa<llvm::ConstantInt>(op2)
		|| !llvm::isa<llvm::ConstantInt>(op3))
	{
		translatePseudoAsmOp0FncOp1Op2Op3(i, mi, irb);
		return;
	}
	uint64_t pos = llvm::cast<llvm::ConstantInt>(op2)->getZExtValue();
	uint64_t size = llvm::cast<llvm::ConstantInt>(op3)->getZExtValue();
	unsigned bits = ty->getBitWidth();
	if (size == 0 || pos >= bits || size > bits - pos)
	{
		translatePseudoAsmOp0FncOp1Op2Op3(i, mi, irb);
		return;
	}

	uint64_t field = size >= 64 ? ~0ull : ((1ull << size) - 1);
	auto* mask = llvm::ConstantInt::get(ty, field << pos);
	llvm::Value* old = loadOp(mi->operands[0], irb);
	llvm::Value* res = irb.CreateOr(
		irb.CreateAnd(old, irb.CreateNot(mask)),
		irb.CreateAnd(irb.CreateShl(op1, llvm::ConstantInt::get(ty, pos)), mask));
	storeOp(mi->operands[0], res, irb);
}

/**
 * MIPS_INS_WSBH -- `wsbh rd, rt`, swap the bytes WITHIN each halfword.
 *
 * 1,890 occurrences, and it is half of how MIPS spells a byte swap: `wsbh`
 * followed by `rotr rd, rd, 16` is a 32-bit bswap, which is why it turns up in
 * every endian conversion in the corpus.
 *
 * Not llvm.bswap.i32: `0x11223344` becomes `0x22114433`, not `0x44332211`.
 * The intrinsic is the plausible wrong answer here.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateWsbh(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(op1->getType());
	// 32-bit registers only. WSBH is a word instruction and MIPS64 spells the
	// 64-bit one DSBH; swapping all four halfwords of a 64-bit register would
	// be that other instruction, which is the plausible wrong answer a
	// register-width implementation produces.
	if (ty == nullptr || ty->getBitWidth() != 32)
	{
		translatePseudoAsmOp0FncOp1(i, mi, irb);
		return;
	}

	auto* eight = llvm::ConstantInt::get(ty, 8);
	auto* m = llvm::ConstantInt::get(ty, 0x00ff00ffull);
	llvm::Value* res =
		irb.CreateOr(irb.CreateShl(irb.CreateAnd(op1, m), eight), irb.CreateAnd(irb.CreateLShr(op1, eight), m));
	storeOp(mi->operands[0], res, irb);
}

/**
 * MIPS_INS_TRUNC, MIPS_INS_ROUND, MIPS_INS_CEIL, MIPS_INS_FLOOR
 *
 * `trunc.w.d $f0, $f2` and its eleven siblings: convert a float or a double to
 * a 32- or 64-bit integer under a fixed rounding mode, and leave the integer
 * in an FP register as a bit pattern.
 *
 * All four ids were dispatched to translatePseudoAsmOp0FncOp1(), which emits
 * `__asm_trunc.w.d(double)` and stores the *double* it returns. Two things
 * were wrong with that and only one of them was the missing semantics: the
 * destination of a `.w` form holds a 32-bit integer and belongs in the
 * single-precision register, and loadRegister()/storeRegister() map every FP
 * operand of a double-format instruction to the FDn file, so the result was
 * landing in `fd0` where the next instruction would read `f0`.
 *
 * That is why the store here bypasses storeRegister() and goes through
 * getRegister() directly, the way translateCvt()'s `cvt.s.d` branch already
 * does: the automatic single-to-double mapping is right for the source and
 * wrong for the destination, and there is no per-operand way to ask for it.
 *
 * Rounding mode is the whole difference between the four, and it is in the
 * mnemonic: TRUNC is toward zero, which is what fptosi already does; ROUND is
 * to nearest **even**, not away from zero, so it is llvm.roundeven and not
 * llvm.round.
 *
 * Anything whose register widths do not match what the format asks for falls
 * back to the pseudo-asm call rather than being given an answer. That is not a
 * corner: on MIPS64 every FP register is 64 bits, so a `.s` source is not the
 * float the instruction reads and a `.w` destination is not the 32-bit slot it
 * writes. Six of the twelve forms translate (the four `.w.s`/`.w.d` pairs on
 * MIPS32, and `.l.d` on MIPS64) and six do not, and the ones that do not say
 * so in the output rather than answering at the wrong width.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateFpToInt(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	std::string mnem = i->mnemonic;
	// <op>.<result width>.<source format>, e.g. trunc.w.d
	unsigned dstBits = mnem.find(".w.") != std::string::npos ? 32 : (mnem.find(".l.") != std::string::npos ? 64 : 0);
	unsigned srcBits = mnem.size() >= 2 && mnem.compare(mnem.size() - 2, 2, ".s") == 0
						 ? 32
						 : (mnem.size() >= 2 && mnem.compare(mnem.size() - 2, 2, ".d") == 0 ? 64 : 0);

	switch (i->id)
	{
		case MIPS_INS_TRUNC_W_S:
		case MIPS_INS_ROUND_W_S:
		case MIPS_INS_CEIL_W_S:
		case MIPS_INS_FLOOR_W_S:
			dstBits = 32; srcBits = 32; break;
		case MIPS_INS_TRUNC_W_D:
		case MIPS_INS_ROUND_W_D:
		case MIPS_INS_CEIL_W_D:
		case MIPS_INS_FLOOR_W_D:
			dstBits = 32; srcBits = 64; break;
		case MIPS_INS_TRUNC_L_S:
		case MIPS_INS_ROUND_L_S:
		case MIPS_INS_CEIL_L_S:
		case MIPS_INS_FLOOR_L_S:
			dstBits = 64; srcBits = 32; break;
		case MIPS_INS_TRUNC_L_D:
		case MIPS_INS_ROUND_L_D:
		case MIPS_INS_CEIL_L_D:
		case MIPS_INS_FLOOR_L_D:
			dstBits = 64; srcBits = 64; break;
		default: break;
	}

	if (mi->operands[0].type != MIPS_OP_REG
		|| !isCanonicalFpReg(mi->operands[0].reg)
		|| mi->operands[1].type != MIPS_OP_REG
		|| !isCanonicalFpReg(mi->operands[1].reg))
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmOp0FncOp1(i, mi, irb);
		return;
	}
	if (dstBits == 0 || srcBits == 0)
	{
		translatePseudoAsmOp0FncOp1(i, mi, irb);
		return;
	}

	// The source goes through loadRegister(), which maps FP operands of a
	// double-format instruction to the FDn file. Whether that produced a value
	// of the width the mnemonic asks for is the question, so the loaded type is
	// what gets checked -- not the register id, which is the same either way.
	llvm::Value* src = loadRegister(mi->operands[1].reg, irb);
	if (src == nullptr || !src->getType()->isFloatingPointTy() || src->getType()->getPrimitiveSizeInBits() != srcBits)
	{
		translatePseudoAsmOp0FncOp1(i, mi, irb);
		return;
	}

	// The destination is decided by the RESULT width, not by the instruction's
	// format group, which is why this does not go through storeRegister():
	// that applies the same single-to-double mapping to the destination, and
	// `trunc.w.d $f0, $f2` would land a 32-bit integer in fd0 where the next
	// instruction reads f0.
	uint32_t dr = canonicalizeMipsReg(mi->operands[0].reg);
	llvm::Type* dstTy = getRegisterType(dr);
	if (dstTy == nullptr || dstTy->getPrimitiveSizeInBits() != dstBits)
	{
		uint32_t alt = singlePrecisionToDoublePrecisionFpRegister(dr);
		llvm::Type* altTy = getRegister(alt) ? getRegisterType(alt) : nullptr;
		if (altTy != nullptr && altTy->getPrimitiveSizeInBits() == dstBits)
		{
			dr = alt;
			dstTy = altTy;
		}
	}
	auto* dstReg = getRegister(dr);
	if (dstReg == nullptr || dstTy == nullptr || dstTy->getPrimitiveSizeInBits() != dstBits)
	{
		translatePseudoAsmOp0FncOp1(i, mi, irb);
		return;
	}

	switch (i->id)
	{
	case MIPS_INS_TRUNC_W_S:
	case MIPS_INS_TRUNC_W_D:
	case MIPS_INS_TRUNC_L_S:
	case MIPS_INS_TRUNC_L_D:
		break; // fptosi already truncates toward zero
	case MIPS_INS_ROUND_W_S:
	case MIPS_INS_ROUND_W_D:
	case MIPS_INS_ROUND_L_S:
	case MIPS_INS_ROUND_L_D:
		src = irb.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, src); break;
	case MIPS_INS_CEIL_W_S:
	case MIPS_INS_CEIL_W_D:
	case MIPS_INS_CEIL_L_S:
	case MIPS_INS_CEIL_L_D:
		src = irb.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, src); break;
	case MIPS_INS_FLOOR_W_S:
	case MIPS_INS_FLOOR_W_D:
	case MIPS_INS_FLOOR_L_S:
	case MIPS_INS_FLOOR_L_D:
		src = irb.CreateUnaryIntrinsic(llvm::Intrinsic::floor, src); break;
	default: throw GenericError("translateFpToInt(): unhandled instruction id");
	}

	llvm::Value* iv = generateFpToIntDefault(src, irb.getIntNTy(dstBits), irb);
	auto* st = irb.CreateStore(irb.CreateBitCast(iv, dstTy), dstReg);
	attachPointeeType(st, dstReg->getValueType());
}

/**
 * MIPS_INS_CVT
 */
void Capstone2LlvmIrTranslatorMips_impl::translateCvt(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	if (mi->operands[0].type != MIPS_OP_REG
			|| !isCanonicalFpReg(mi->operands[0].reg)
			|| mi->operands[1].type != MIPS_OP_REG
			|| !isCanonicalFpReg(mi->operands[1].reg))
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto r0 = canonicalizeMipsReg(mi->operands[0].reg);
	auto r1 = canonicalizeMipsReg(mi->operands[1].reg);

	// CVT.S.fmt
	//
	std::string mnem = i->mnemonic;
	if (mnem == "cvt.s.d" || i->id == MIPS_INS_CVT_S_D) // should be only on MIPS32
	{
		op1 = loadRegister(r1, irb);
		op1 = irb.CreateFPCast(op1, getRegisterType(r0));
		auto* dst = getRegister(r0);
		auto* st = irb.CreateStore(op1, dst);
		attachPointeeType(st, dst->getValueType());
	}
	else if (mnem == "cvt.s.w" // should be only on MIPS32
			|| mnem == "cvt.s.l" // should be only on MIPS64
			|| i->id == MIPS_INS_CVT_S_W
			|| i->id == MIPS_INS_CVT_S_L)
	{
		auto* op0Ty = getRegisterType(r0);
		op1 = loadRegister(r1, irb);
		auto* iTy = op1->getType()->isDoubleTy()
				? irb.getInt64Ty()
				: irb.getInt32Ty();
		op1 = irb.CreateBitCast(op1, iTy);
		op1 = irb.CreateSIToFP(op1, op0Ty);
		auto* dst = getRegister(r0);
		auto* st = irb.CreateStore(op1, dst);
		attachPointeeType(st, dst->getValueType());
	}
	// CVT.D.fmt
	//
	else if (mnem == "cvt.d.s" || i->id == MIPS_INS_CVT_D_S)
	{
		op1 = createLoad(irb, getRegister(r1));
		storeRegister(r0, op1, irb, eOpConv::SITOFP_OR_FPCAST);
	}
	else if (mnem == "cvt.d.w" // should be only on MIPS32
			|| mnem == "cvt.d.l" // should be only on MIPS64
			|| i->id == MIPS_INS_CVT_D_W
			|| i->id == MIPS_INS_CVT_D_L)
	{
		op1 = createLoad(irb, getRegister(r1));
		auto* iTy = op1->getType()->isDoubleTy()
				? irb.getInt64Ty()
				: irb.getInt32Ty();
		op1 = irb.CreateBitCast(op1, iTy);
		op1 = irb.CreateSIToFP(op1, irb.getDoubleTy());
		storeRegister(r0, op1, irb, eOpConv::SITOFP_OR_FPCAST);
	}
	// CVT.W.fmt
	//
	else if (mnem == "cvt.w.s" // should be only on MIPS32
			|| mnem == "cvt.w.d" // should be only on MIPS64
			|| i->id == MIPS_INS_CVT_W_S
			|| i->id == MIPS_INS_CVT_W_D)
	{
		op1 = loadRegister(r1, irb);
		// CVT.W.fmt rounds with the CURRENT rounding mode (FCSR.RM), whose
		// reset value and ABI default is nearest-even. A bare fptosi
		// truncates, which is TRUNC.W.fmt -- a different instruction that
		// exists alongside it. translateFpToInt, eighty lines up, already
		// case-splits TRUNC/ROUND/CEIL/FLOOR onto fptosi/roundeven/ceil/floor
		// and says in its comment that the rounding mode is the whole
		// difference between them; this one was missed.
		//
		// Both tests of it use 3.1415, on which truncating and rounding agree.
		op1 = irb.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, op1);
		// And the destination is a WORD whatever the source format is -- that
		// is what the W in CVT.W means; CVT.L.fmt is the 64-bit one. This
		// picked the width from the SOURCE, so cvt.w.d converted to 64 bits.
		auto* iTy = irb.getInt32Ty();
		op1 = generateFpToIntDefault(op1, iTy, irb);
		auto* iTy2 = getRegisterType(r0)->isDoubleTy()
				? irb.getInt64Ty()
				: irb.getInt32Ty();
		op1 = irb.CreateSExtOrTrunc(op1, iTy2);
		op1 = irb.CreateBitCast(op1, getRegisterType(r0));
		auto* dst = getRegister(r0);
		auto* st = irb.CreateStore(op1, dst);
		attachPointeeType(st, dst->getValueType());
	}
	else if (mnem == "cvt.l.s"
			|| mnem == "cvt.l.d"
			|| i->id == MIPS_INS_CVT_L_S
			|| i->id == MIPS_INS_CVT_L_D)
	{
		op1 = loadRegister(r1, irb);
		op1 = irb.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, op1);
		auto* iTy = irb.getInt64Ty();
		op1 = generateFpToIntDefault(op1, iTy, irb);
		op1 = irb.CreateBitCast(op1, irb.getDoubleTy());
		storeRegister(r0, op1, irb, eOpConv::FPCAST_OR_BITCAST);
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
}

/**
 * MIPS_INS_BEQ, MIPS_INS_BEQL (likely)
 * MIPS_INS_BNE, MIPS_INS_BNEL (likely)
 * Compact R6 (no delay slot): BEQC, BNEC, BGEC, BLTC, BGEUC, BLTUC
 */
void Capstone2LlvmIrTranslatorMips_impl::translateCondBranchTernary(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(mi, irb);
	op1 = irb.CreateZExtOrTrunc(op1, op0->getType());

	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case MIPS_INS_BEQ:
		case MIPS_INS_BEQL:
		case MIPS_INS_BEQC:
			cond = irb.CreateICmpEQ(op0, op1);
			break;
		case MIPS_INS_BNE:
		case MIPS_INS_BNEL:
		case MIPS_INS_BNEC:
			cond = irb.CreateICmpNE(op0, op1);
			break;
		case MIPS_INS_BGEC:
			cond = irb.CreateICmpSGE(op0, op1);
			break;
		case MIPS_INS_BLTC:
			cond = irb.CreateICmpSLT(op0, op1);
			break;
		case MIPS_INS_BGEUC:
			cond = irb.CreateICmpUGE(op0, op1);
			break;
		case MIPS_INS_BLTUC:
			cond = irb.CreateICmpULT(op0, op1);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateCondBranchTernary().");
	}

	generateCondBranchFunctionCall(irb, cond, op2);
}

/**
 * MIPS_INS_BLEZ, MIPS_INS_BLEZL (likely)
 * MIPS_INS_BGTZ, MIPS_INS_BGTZL (likely)
 * MIPS_INS_BLTZ, MIPS_INS_BLTZL (likely)
 * MIPS_INS_BGEZ, MIPS_INS_BGEZL (likely)
 * MIPS_INS_BEQZ
 * MIPS_INS_BNEZ
 * microMIPS 16-bit: BEQZ16, BNEZ16
 * Compact R6 (no delay slot): BEQZC, BNEZC, BGTZC, BLEZC, BGEZC, BLTZC
 */
void Capstone2LlvmIrTranslatorMips_impl::translateCondBranchBinary(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	std::tie(op0, op1) = loadOpBinary(mi, irb);
	auto* zero = llvm::ConstantInt::get(op0->getType(), 0);

	unsigned cid = i->id;
	if (i->is_alias)
	{
		unsigned a = static_cast<unsigned>(i->alias_id);
		if (a == static_cast<unsigned>(MIPS_INS_BEQZ)
#ifdef MIPS_INS_ALIAS_BEQZ
				|| a == static_cast<unsigned>(MIPS_INS_ALIAS_BEQZ)
#endif
				|| a == static_cast<unsigned>(MIPS_INS_BNEZ)
#ifdef MIPS_INS_ALIAS_BNEZ
				|| a == static_cast<unsigned>(MIPS_INS_ALIAS_BNEZ)
#endif
			)
		{
			cid = a;
		}
	}
	llvm::Value* cond = nullptr;
	switch (cid)
	{
		case MIPS_INS_BLEZ:
		case MIPS_INS_BLEZL:
		case MIPS_INS_BLEZC:
			cond = irb.CreateICmpSLE(op0, zero);
			break;
		case MIPS_INS_BGTZ:
		case MIPS_INS_BGTZL:
		case MIPS_INS_BGTZC:
			cond = irb.CreateICmpSGT(op0, zero);
			break;
		case MIPS_INS_BLTZ:
		case MIPS_INS_BLTZL:
		case MIPS_INS_BLTZC:
			cond = irb.CreateICmpSLT(op0, zero);
			break;
		case MIPS_INS_BGEZ:
		case MIPS_INS_BGEZL:
		case MIPS_INS_BGEZC:
			cond = irb.CreateICmpSGE(op0, zero);
			break;
		case MIPS_INS_BEQZ:
		case MIPS_INS_BEQZ16:
		case MIPS_INS_BEQZC:
#ifdef MIPS_INS_BEQZC16
		case MIPS_INS_BEQZC16:
#endif
#ifdef MIPS_INS_ALIAS_BEQZ
		case MIPS_INS_ALIAS_BEQZ:
#endif
			cond = irb.CreateICmpEQ(op0, zero);
			break;
		case MIPS_INS_BNEZ:
		case MIPS_INS_BNEZ16:
		case MIPS_INS_BNEZC:
#ifdef MIPS_INS_BNEZC16
		case MIPS_INS_BNEZC16:
#endif
#ifdef MIPS_INS_ALIAS_BNEZ
		case MIPS_INS_ALIAS_BNEZ:
#endif
			cond = irb.CreateICmpNE(op0, zero);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateCondBranchBinary().");
	}

	generateCondBranchFunctionCall(irb, cond, op1);
}

/**
 * MIPS_INS_BREAK
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBreak(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	// MIPS `break` is a software trap / breakpoint.  We model it as a
	// pseudo-asm call so that downstream passes can see it as an opaque
	// side-effect and remove it if desired (e.g. when it guards an
	// unreachable path after a bounds check).
	EXPECT_IS_EXPR(i, mi, irb, (mi->op_count < 3));

	if (mi->op_count == 0)
	{
		op0 = llvm::ConstantInt::get(getDefaultType(), 0);
	}
	else if (mi->op_count == 1)
	{
		op0 = loadOpUnary(mi, irb);
	}
	else if (mi->op_count == 2)
	{
		std::tie(op0, op1) = loadOpBinary(mi, irb);
	}

	op0 = irb.CreateZExtOrTrunc(op0, getDefaultType());
	if (op1)
	{
		op1 = irb.CreateZExtOrTrunc(op1, getDefaultType());

		llvm::Function* fnc = getPseudoAsmFunction(
				i,
				irb.getVoidTy(),
				llvm::ArrayRef<llvm::Type*>{op0->getType(), op1->getType()});
		irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0, op1});
	}
	else
	{
		llvm::Function* fnc = getPseudoAsmFunction(
				i,
				irb.getVoidTy(),
				llvm::ArrayRef<llvm::Type*>{op0->getType()});
		irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op0});
	}
}

/**
 * MIPS_INS_C_* / MIPS_INS_ALIAS_C_*  (c.cond.fmt)
 */
void Capstone2LlvmIrTranslatorMips_impl::translateC(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	uint32_t fcc = MIPS_REG_FCC0;
	if (mi->op_count == 3
			&& mi->operands[0].type == MIPS_OP_REG
			&& isFccRegister(mi->operands[0].reg))
	{
		fcc = mi->operands[0].reg;
		op0 = loadOp(mi->operands[1], irb);
		op1 = loadOp(mi->operands[2], irb);
	}
	else
	{
		std::tie(op0, op1) = loadOpBinary(mi, irb, eOpConv::THROW);
	}

	std::string mnem = i->mnemonic;
	llvm::Value* val = nullptr;

	// http://ti.ira.uka.de/TI-2/Mips/Befehlssatz.pdf
	// Capstone 6 splits each cond/fmt pair into its own id; the mnemonic
	// still names the comparison, and the alias ids share that mnemonic.
	//
	if (mnem == "c.f.s" || mnem == "c.f.d"
			|| mnem == "c.sf.s" || mnem == "c.sf.d"
			|| i->id == MIPS_INS_C_F_S || i->id == MIPS_INS_C_F_D
			|| i->id == MIPS_INS_C_SF_S || i->id == MIPS_INS_C_SF_D
			|| i->id == MIPS_INS_ALIAS_C_F_S || i->id == MIPS_INS_ALIAS_C_F_D
			|| i->id == MIPS_INS_ALIAS_C_SF_S || i->id == MIPS_INS_ALIAS_C_SF_D)
	{
		val = irb.getFalse(); // This is ok, I checked.
	}
	else if (mnem == "c.un.s" || mnem == "c.un.d"
			|| mnem == "c.ngle.s" || mnem == "c.ngle.d"
			|| i->id == MIPS_INS_C_UN_S || i->id == MIPS_INS_C_UN_D
			|| i->id == MIPS_INS_C_NGLE_S || i->id == MIPS_INS_C_NGLE_D
			|| i->id == MIPS_INS_ALIAS_C_UN_S || i->id == MIPS_INS_ALIAS_C_UN_D
			|| i->id == MIPS_INS_ALIAS_C_NGLE_S || i->id == MIPS_INS_ALIAS_C_NGLE_D)
	{
		val = irb.CreateFCmpUNO(op0, op1);
	}
	else if (mnem == "c.eq.s" || mnem == "c.eq.d"
			|| mnem == "c.seq.s" || mnem == "c.seq.d"
			|| mnem == "c.ngl.s" || mnem == "c.ngl.d"
			|| i->id == MIPS_INS_C_EQ_S || i->id == MIPS_INS_C_EQ_D
			|| i->id == MIPS_INS_C_SEQ_S || i->id == MIPS_INS_C_SEQ_D
			|| i->id == MIPS_INS_C_NGL_S || i->id == MIPS_INS_C_NGL_D
			|| i->id == MIPS_INS_ALIAS_C_EQ_S || i->id == MIPS_INS_ALIAS_C_EQ_D
			|| i->id == MIPS_INS_ALIAS_C_SEQ_S || i->id == MIPS_INS_ALIAS_C_SEQ_D
			|| i->id == MIPS_INS_ALIAS_C_NGL_S || i->id == MIPS_INS_ALIAS_C_NGL_D)
	{
		val = irb.CreateFCmpOEQ(op0, op1);
	}
	else if (mnem == "c.ueq.s" || mnem == "c.ueq.d"
			|| i->id == MIPS_INS_C_UEQ_S || i->id == MIPS_INS_C_UEQ_D
			|| i->id == MIPS_INS_ALIAS_C_UEQ_S || i->id == MIPS_INS_ALIAS_C_UEQ_D)
	{
		val = irb.CreateFCmpUEQ(op0, op1);
	}
	else if (mnem == "c.olt.s" || mnem == "c.olt.d"
			|| mnem == "c.lt.s" || mnem == "c.lt.d"
			|| mnem == "c.nge.s" || mnem == "c.nge.d"
			|| i->id == MIPS_INS_C_OLT_S || i->id == MIPS_INS_C_OLT_D
			|| i->id == MIPS_INS_C_LT_S || i->id == MIPS_INS_C_LT_D
			|| i->id == MIPS_INS_C_NGE_S || i->id == MIPS_INS_C_NGE_D
			|| i->id == MIPS_INS_ALIAS_C_OLT_S || i->id == MIPS_INS_ALIAS_C_OLT_D
			|| i->id == MIPS_INS_ALIAS_C_LT_S || i->id == MIPS_INS_ALIAS_C_LT_D
			|| i->id == MIPS_INS_ALIAS_C_NGE_S || i->id == MIPS_INS_ALIAS_C_NGE_D)
	{
		val = irb.CreateFCmpOLT(op0, op1);
	}
	else if (mnem == "c.ult.s" || mnem == "c.ult.d"
			|| i->id == MIPS_INS_C_ULT_S || i->id == MIPS_INS_C_ULT_D
			|| i->id == MIPS_INS_ALIAS_C_ULT_S || i->id == MIPS_INS_ALIAS_C_ULT_D)
	{
		val = irb.CreateFCmpULT(op0, op1);
	}
	else if (mnem == "c.ole.s" || mnem == "c.ole.d"
			|| mnem == "c.le.s" || mnem == "c.le.d"
			|| mnem == "c.ngt.s" || mnem == "c.ngt.d"
			|| i->id == MIPS_INS_C_OLE_S || i->id == MIPS_INS_C_OLE_D
			|| i->id == MIPS_INS_C_LE_S || i->id == MIPS_INS_C_LE_D
			|| i->id == MIPS_INS_C_NGT_S || i->id == MIPS_INS_C_NGT_D
			|| i->id == MIPS_INS_ALIAS_C_OLE_S || i->id == MIPS_INS_ALIAS_C_OLE_D
			|| i->id == MIPS_INS_ALIAS_C_LE_S || i->id == MIPS_INS_ALIAS_C_LE_D
			|| i->id == MIPS_INS_ALIAS_C_NGT_S || i->id == MIPS_INS_ALIAS_C_NGT_D)
	{
		val = irb.CreateFCmpOLE(op0, op1);
	}
	else if (mnem == "c.ule.s" || mnem == "c.ule.d"
			|| i->id == MIPS_INS_C_ULE_S || i->id == MIPS_INS_C_ULE_D
			|| i->id == MIPS_INS_ALIAS_C_ULE_S || i->id == MIPS_INS_ALIAS_C_ULE_D)
	{
		val = irb.CreateFCmpULE(op0, op1);
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	storeRegister(fcc, val, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_CLO
 */
void Capstone2LlvmIrTranslatorMips_impl::translateClo(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	op1 = narrowToWord(i, irb, op1);
	op1 = irb.CreateXor(op1, llvm::ConstantInt::getSigned(op1->getType(), -1));
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::ctlz,
			op1->getType());
	auto* ctlz = irb.CreateCall(f, {op1, irb.getTrue()});
	storeOp(mi->operands[0], ctlz, irb);
}

/**
 * MIPS_INS_CLZ
 */
void Capstone2LlvmIrTranslatorMips_impl::translateClz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	op1 = narrowToWord(i, irb, op1);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::ctlz,
			op1->getType());
	auto* ctlz = irb.CreateCall(f, {op1, irb.getTrue()});
	storeOp(mi->operands[0], ctlz, irb);
}

/**
 * MIPS_INS_DIV
 */
void Capstone2LlvmIrTranslatorMips_impl::translateDiv(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (isFpInstructionVariant(i))
	{
		EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SITOFP_OR_FPCAST);
		auto* div = irb.CreateFDiv(op1, op2);
		storeOp(mi->operands[0], div, irb);
	}
	else
	{
		// Pre-R6 `div rs, rt` writes LO/HI. R6 (and MIPS32R6 mode) replaced
		// HI/LO with `div rd, rs, rt` into a GPR. Same Capstone id; operand
		// count is what distinguishes them.
		EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

		// Capstone 6 often reports pre-R6 `div rs, rt` as three operands with
		// rd=$zero. That is still the HI/LO form, not R6 `div rd, rs, rt`.
		const bool r6Gpr = mi->op_count == 3
				&& mi->operands[0].type == MIPS_OP_REG
				&& canonicalizeMipsReg(mi->operands[0].reg) != MIPS_REG_ZERO;
		if (r6Gpr)
		{
			std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
			op1 = narrowToWord(i, irb, op1);
			op2 = narrowToWord(i, irb, op2);
			op2 = generateSafeDivisor(op1, op2, /*isSigned=*/true, irb);
			storeOp(mi->operands[0], irb.CreateSDiv(op1, op2), irb);
			return;
		}

		if (mi->op_count == 3)
		{
			op0 = loadOp(mi->operands[1], irb, nullptr, false);
			op1 = loadOp(mi->operands[2], irb, nullptr, false);
		}
		else
		{
			std::tie(op0, op1) = loadOpBinary(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
		}
		op0 = narrowToWord(i, irb, op0);
		op1 = narrowToWord(i, irb, op1);

		// MIPS says the result is UNPREDICTABLE for a zero divisor, and says
		// explicitly that NO arithmetic exception occurs. Those two together
		// are much weaker than LLVM's `sdiv X, 0`, which is IMMEDIATE
		// undefined behaviour: the architecture's own definition of
		// UNPREDICTABLE requires that the operation must not halt or hang the
		// processor, where immediate UB licenses the optimiser to delete the
		// surrounding code outright.
		//
		// So the fix is one select on the divisor and nothing more. Because
		// ANY value is architecturally permitted for LO and HI here, dividing
		// by a substituted 1 is already exact -- there is no answer to select
		// back in, unlike ARM64 where the architecture names one.
		//
		// This is not a theoretical input. GCC does guard MIPS divisions, but
		// it spells the guard `teq $rt, $zero, 7` -- and MIPS_INS_TEQ is
		// mapped to translateNop here, so the guard translates to literally
		// nothing and the division that follows is bare.
		//
		// The width comes from the operand, because DDIV routes to this same
		// function at 64 bits while DIV arrives narrowed to 32.
		op1 = generateSafeDivisor(op0, op1, /*isSigned=*/true, irb);

		auto* div = irb.CreateSDiv(op0, op1);
		storeRegister(MIPS_REG_LO, div, irb);
		auto* rem = irb.CreateSRem(op0, op1);
		storeRegister(MIPS_REG_HI, rem, irb);
	}
}

/**
 * A divisor LLVM can prove is neither zero nor the signed-overflow partner.
 *
 * `sdiv X, 0` and `sdiv INT_MIN, -1` are IMMEDIATE undefined behaviour in
 * LLVM -- not poison. The difference matters: poison is a bad value that
 * propagates, while immediate UB lets the optimiser delete the surrounding
 * code, which in a decompiler means deleting the path being read.
 *
 * MIPS calls the zero-divisor result UNPREDICTABLE and guarantees no
 * exception, so any value is a faithful model and the caller needs no second
 * select to restore an architectural answer. The width is taken from the
 * operand so DDIV gets 64-bit constants and DIV gets 32-bit ones.
 */
llvm::Value* Capstone2LlvmIrTranslatorMips_impl::generateSafeDivisor(
	llvm::Value* dividend, llvm::Value* divisor, bool isSigned, llvm::IRBuilder<>& irb)
{
	auto* ty = divisor->getType();
	unsigned bits = ty->getIntegerBitWidth();
	if (isSigned)
	{
		// The overflow pair relates the two operands, so it needs a select;
		// only the zero case can be stated as a bound on the divisor alone.
		auto* overflow = irb.CreateAnd(
			irb.CreateICmpEQ(dividend, llvm::ConstantInt::get(ty, llvm::APInt::getSignedMinValue(bits))),
			irb.CreateICmpEQ(divisor, llvm::ConstantInt::getSigned(ty, -1)));
		divisor = irb.CreateSelect(overflow, llvm::ConstantInt::get(ty, 1), divisor);
	}
	// umax rather than a select on "is it zero": the two compute the same number
	// -- umax(x, 1) is x for every non-zero x, since x as an UNSIGNED value is
	// then at least 1, and is 1 when x is zero -- but only umax states the bound
	// LOCALLY. A reader of `select(y == 0, 1, y)`, human or analysis, has to
	// correlate the condition with the arms to see the result is non-zero, and
	// DIV-01 below cannot. This is the same lesson SHIFT-01 taught on the shift
	// side, where the clamp-and-select idiom made the checker fire on the fix.
	return irb.CreateBinaryIntrinsic(llvm::Intrinsic::umax, divisor, llvm::ConstantInt::get(ty, 1));
}

/**
 * MIPS_INS_DIVU
 */
void Capstone2LlvmIrTranslatorMips_impl::translateDivu(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	const bool r6Gpr = mi->op_count == 3
			&& mi->operands[0].type == MIPS_OP_REG
			&& canonicalizeMipsReg(mi->operands[0].reg) != MIPS_REG_ZERO;
	if (r6Gpr)
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
		op1 = narrowToWord(i, irb, op1);
		op2 = narrowToWord(i, irb, op2);
		op2 = generateSafeDivisor(op1, op2, /*isSigned=*/false, irb);
		storeOp(mi->operands[0], irb.CreateUDiv(op1, op2), irb);
		return;
	}

	if (mi->op_count == 3)
	{
		op0 = loadOp(mi->operands[1], irb, nullptr, false);
		op1 = loadOp(mi->operands[2], irb, nullptr, false);
	}
	else
	{
		std::tie(op0, op1) = loadOpBinary(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	}
	op0 = narrowToWord(i, irb, op0);
	op1 = narrowToWord(i, irb, op1);
	// See translateDiv: UNPREDICTABLE is not immediate UB, and GCC's guard is
	// a trap this translator maps to a nop. Unsigned has no overflow case.
	op1 = generateSafeDivisor(op0, op1, /*isSigned=*/false, irb);

	auto* div = irb.CreateUDiv(op0, op1);
	storeRegister(MIPS_REG_LO, div, irb);
	auto* rem = irb.CreateURem(op0, op1);
	storeRegister(MIPS_REG_HI, rem, irb);
}

/**
 * MIPS_INS_MOD, MIPS_INS_MODU, MIPS_INS_DMOD, MIPS_INS_DMODU
 *
 * R6 remainder into a GPR. Pre-R6 remainder lives in HI after DIV/DIVU.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMod(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);

	bool isSigned = i->id == MIPS_INS_MOD || i->id == MIPS_INS_DMOD;
	op2 = generateSafeDivisor(op1, op2, isSigned, irb);
	auto* rem = isSigned ? irb.CreateSRem(op1, op2) : irb.CreateURem(op1, op2);
	storeOp(mi->operands[0], rem, irb);
}

/**
 * MIPS_INS_EXT
 */
void Capstone2LlvmIrTranslatorMips_impl::translateExt(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	op3 = loadOp(mi->operands[3], irb);

	// This is a special case, when EXT is used to remove sign bit ->  generate
	// absolute value. The problem is, that both ordinary integers and IEEE 754
	// floats have the sign bit at the same place, and that floats can be stored
	// into integer registers. Therefore, this operation may perform floating
	// point abs on an integer register holding floating point value.
	// Since we can not determine, what kind (int vs float) of abs is being
	// performed, we generate floating point variant because it is more general.
	//
	auto* op1Ty = llvm::dyn_cast<llvm::IntegerType>(op1->getType());
	if (op1Ty
			&& llvm::isa<llvm::ConstantInt>(op2)
			&& llvm::cast<llvm::ConstantInt>(op2)->isZero()
			&& llvm::isa<llvm::ConstantInt>(op3)
			&& (llvm::cast<llvm::ConstantInt>(op3)->getZExtValue() + 1)
				== op1Ty->getBitWidth())
	{
		auto* fTy = getFloatTypeFromByteSize(_module, op1Ty->getBitWidth() / 8);
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(
				_module,
				llvm::Intrinsic::fabs,
				fTy);
		op1 = irb.CreateBitCast(op1, fTy);
		llvm::Value* fabs = irb.CreateCall(f, {op1});
		fabs = irb.CreateBitCast(fabs, op1Ty);
		storeOp(mi->operands[0], fabs, irb);
		return;
	}

	// The general case, which everything but that one idiom fell through to:
	//
	//     ext rt, rs, pos, size    rt = (rs >> pos) & ((1 << size) - 1)
	//
	// pos and size are immediates in every encoding, so the mask is constant
	// and the shift cannot leave the operand's width. 4,389 occurrences in the
	// static corpus were reaching __asm_ext instead.
	// 32-bit registers only. EXT is a word instruction -- MIPS64 spells the
	// 64-bit one DEXT -- and on a 64-bit register file the result would also
	// have to be sign-extended as a word, which is a different question from
	// the one this answers.
	if (op1Ty && op1Ty->getBitWidth() == 32 && llvm::isa<llvm::ConstantInt>(op2) && llvm::isa<llvm::ConstantInt>(op3))
	{
		uint64_t pos = llvm::cast<llvm::ConstantInt>(op2)->getZExtValue();
		uint64_t size = llvm::cast<llvm::ConstantInt>(op3)->getZExtValue();
		unsigned bits = op1Ty->getBitWidth();
		if (size > 0 && pos < bits && size <= bits - pos)
		{
			uint64_t mask = size >= 64 ? ~0ull : ((1ull << size) - 1);
			auto* res = irb.CreateAnd(
				irb.CreateLShr(op1, llvm::ConstantInt::get(op1Ty, pos)), llvm::ConstantInt::get(op1Ty, mask));
			storeOp(mi->operands[0], res, irb);
			return;
		}
	}

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			getDefaultType(),
			llvm::ArrayRef<llvm::Type*>{
					op1->getType(),
					op2->getType(),
					op3->getType()});

	auto* c = irb.CreateCall(fnc, llvm::ArrayRef<llvm::Value*>{op1, op2, op3});
	storeOp(mi->operands[0], c, irb);
}

/**
 * MIPS_INS_SYNC, MIPS_INS_SYNCI
 * Memory / instruction barriers → LLVM fence. SYNCI has no I-cache model.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateFence(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	irb.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
}

/**
 * MIPS_INS_J, MIPS_INS_JR,
 * MIPS_INS_B, MIPS_INS_B16, MIPS_INS_JR16,
 * Compact R6 (no delay slot): MIPS_INS_BC, MIPS_INS_JRC
 */
void Capstone2LlvmIrTranslatorMips_impl::translateJ(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	op0 = loadOpUnary(mi, irb);
	generateBranchFunctionCall(irb, op0);
}

/**
 * MIPS_INS_JAL, MIPS_INS_JALR,
 * MIPS_INS_BAL, MIPS_INS_JALRS, MIPS_INS_JALRS16
 * Compact R6 (no delay slot): MIPS_INS_BALC, MIPS_INS_JALRC
 *
 * `jal` / `bal` / `jalr rs` link into `$ra`. `jalr rd, rs` links into `rd`.
 * The target is loaded before the link register is written so `jalr $ra`
 * (uncommon, architecturally UNPREDICTABLE when rs == rd) still reads the
 * old value rather than the return address just stored.
 * Compact forms link to the next instruction (`PC + size`), not `PC + 2*size`.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateJal(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, mi, irb);

	uint32_t link = MIPS_REG_RA;
	if (mi->op_count == 2)
	{
		if (mi->operands[0].type == MIPS_OP_REG)
		{
			link = mi->operands[0].reg;
		}
		op0 = loadOp(mi->operands[1], irb);
	}
	else
	{
		op0 = loadOpUnary(mi, irb);
	}

	llvm::Value* ra = isCompactLinkId(i->id) ? getNextInsnAddress(i) : getNextNextInsnAddress(i);
	storeRegister(link, ra, irb);
	generateCallFunctionCall(irb, op0);
}

/**
 * MIPS_INS_JIC — PC = GPR[rt] + offset, no delay slot.
 * MIPS_INS_JIALC — same target, plus $ra = next instruction.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateJic(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	std::tie(op0, op1) = loadOpBinary(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* tgt = irb.CreateAdd(op0, op1);
	if (i->id == MIPS_INS_JIALC)
	{
		storeRegister(MIPS_REG_RA, getNextInsnAddress(i), irb);
		generateCallFunctionCall(irb, tgt);
		return;
	}
	generateBranchFunctionCall(irb, tgt);
}

/**
 * MIPS_INS_LI16 — rd = imm. Not an ADD: Capstone reports two operands, so
 * translateAdd would do rd += imm.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateLi(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	storeOp(mi->operands[0], op1, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ADDIUPC — rt = PC + offset (byte offset; PC is this instruction).
 * MIPS_INS_AUIPC — rt = PC + (imm << 16)
 * MIPS_INS_ALUIPC — rt = (PC + (imm << 16)) & ~0xFFFF
 *
 * Capstone reports the unshifted 16-bit field for AUIPC/ALUIPC and the
 * already-scaled byte offset for ADDIUPC. It does not convert those to
 * absolute addresses (unlike compact branches).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateAddiupc(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	auto* pc = getThisInsnAddress(i);
	if (pc->getType() != op1->getType())
	{
		pc = irb.CreateZExtOrTrunc(pc, op1->getType());
	}
	if (i->id == MIPS_INS_AUIPC || i->id == MIPS_INS_ALUIPC)
	{
		op1 = irb.CreateShl(op1, llvm::ConstantInt::get(op1->getType(), 16));
	}
	auto* sum = irb.CreateAdd(pc, op1);
	if (i->id == MIPS_INS_ALUIPC)
	{
		sum = irb.CreateAnd(sum, llvm::ConstantInt::get(sum->getType(), ~uint64_t(0xFFFF)));
	}
	storeOp(mi->operands[0], sum, irb);
}

/**
 * MIPS_INS_AUI — rt = rs + (imm << 16)
 */
void Capstone2LlvmIrTranslatorMips_impl::translateAui(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);
	op2 = irb.CreateShl(op2, llvm::ConstantInt::get(op2->getType(), 16));
	storeOp(mi->operands[0], irb.CreateAdd(op1, op2), irb);
}

/**
 * MIPS_INS_ADDIUSP — SP += imm (Capstone reports the decoded immediate).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateAddiusp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	op0 = loadOpUnary(mi, irb);
	auto* sp = loadRegister(MIPS_REG_SP, irb);
	op0 = irb.CreateSExtOrTrunc(op0, sp->getType());
	storeRegister(MIPS_REG_SP, irb.CreateAdd(sp, op0), irb);
}

/**
 * MIPS_INS_JRADDIUSP — jump to $ra and SP += imm. Has a delay slot.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateJraddiusp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	op0 = loadOpUnary(mi, irb);
	auto* ra = loadRegister(MIPS_REG_RA, irb);
	auto* sp = loadRegister(MIPS_REG_SP, irb);
	op0 = irb.CreateSExtOrTrunc(op0, sp->getType());
	storeRegister(MIPS_REG_SP, irb.CreateAdd(sp, op0), irb);
	generateBranchFunctionCall(irb, ra);
}

/**
 * MIPS_INS_BITSWAP — reverse the bits of each byte of a 32-bit GPR.
 * Full i32 bitreverse followed by bswap is the per-byte reverse.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBitswap(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	auto* i32 = irb.getInt32Ty();
	op1 = irb.CreateZExtOrTrunc(op1, i32);
	op1 = irb.CreateUnaryIntrinsic(llvm::Intrinsic::bitreverse, op1);
	op1 = irb.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, op1);
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_ALIGN rd, rs, rt, bp — concatenate rs||rt and extract a word
 * at byte offset bp (0..3): rd = (rs << 8*bp) | (rt >> 8*(4-bp)).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateAlign(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, mi, irb);

	std::tie(op1, op2, op3) = loadOpQuaternaryOp1Op2Op3(mi, irb);
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(op1->getType());
	if (ty == nullptr || !llvm::isa<llvm::ConstantInt>(op3))
	{
		translatePseudoAsmOp0FncOp1Op2Op3(i, mi, irb);
		return;
	}
	uint64_t bp = llvm::cast<llvm::ConstantInt>(op3)->getZExtValue();
	if (bp > 3)
	{
		translatePseudoAsmOp0FncOp1Op2Op3(i, mi, irb);
		return;
	}
	auto* i32 = irb.getInt32Ty();
	op1 = irb.CreateZExtOrTrunc(op1, i32);
	op2 = irb.CreateZExtOrTrunc(op2, i32);
	llvm::Value* res = nullptr;
	if (bp == 0)
	{
		res = op2;
	}
	else
	{
		unsigned sh = static_cast<unsigned>(bp * 8);
		res = irb.CreateOr(
				irb.CreateShl(op1, llvm::ConstantInt::get(i32, sh)),
				irb.CreateLShr(op2, llvm::ConstantInt::get(i32, 32 - sh)));
	}
	storeOp(mi->operands[0], res, irb);
}

/**
 * MIPS_INS_LWPC — load word from PC + offset (byte offset, this instruction).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateLwpc(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	auto* pc = getThisInsnAddress(i);
	if (pc->getType() != op1->getType())
	{
		pc = irb.CreateZExtOrTrunc(pc, op1->getType());
	}
	auto* addr = irb.CreateAdd(pc, op1);
	auto* val = loadIntPtr(irb, addr, irb.getInt32Ty());
	storeOp(mi->operands[0], val, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_LWP — load rt and rt2 from mem and mem+4. Capstone lists both
 * destination registers plus a folded memory operand.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateLoadPair(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count != 3
			|| mi->operands[0].type != MIPS_OP_REG
			|| mi->operands[1].type != MIPS_OP_REG
			|| mi->operands[2].type != MIPS_OP_MEM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* addr = loadOp(mi->operands[2], irb, nullptr, /*lea=*/true);
	auto* i32 = irb.getInt32Ty();
	storeOp(mi->operands[0], loadIntPtr(irb, addr, i32), irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* addr2 = irb.CreateAdd(addr, llvm::ConstantInt::get(addr->getType(), 4));
	storeOp(mi->operands[1], loadIntPtr(irb, addr2, i32), irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SWP — store rt and rt2 to mem and mem+4.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateStorePair(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count != 3
			|| mi->operands[0].type != MIPS_OP_REG
			|| mi->operands[1].type != MIPS_OP_REG
			|| mi->operands[2].type != MIPS_OP_MEM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* addr = loadOp(mi->operands[2], irb, nullptr, /*lea=*/true);
	auto* i32 = irb.getInt32Ty();
	op0 = irb.CreateZExtOrTrunc(loadOp(mi->operands[0], irb), i32);
	op1 = irb.CreateZExtOrTrunc(loadOp(mi->operands[1], irb), i32);
	storeIntPtr(irb, op0, addr, i32);
	auto* addr2 = irb.CreateAdd(addr, llvm::ConstantInt::get(addr->getType(), 4));
	storeIntPtr(irb, op1, addr2, i32);
}

namespace {

bool collectMipsRegListAndMem(cs_mips* mi, std::vector<uint32_t>& regs, unsigned& memIdx)
{
	memIdx = ~0u;
	bool sawImm = false;
	for (unsigned k = 0; k < mi->op_count; ++k)
	{
		auto& op = mi->operands[k];
		if (op.type == MIPS_OP_REG)
		{
			regs.push_back(op.reg);
		}
		else if (op.type == MIPS_OP_MEM)
		{
			memIdx = k;
		}
		else if (op.type == MIPS_OP_IMM)
		{
			sawImm = true;
		}
	}
	return !sawImm && memIdx != ~0u && !regs.empty();
}

// 16-bit LWM16/SWM16: Capstone 6 expands rlist to REGs but folds SP+offset
// into MEM with base=INVALID and disp=24 (the SP enum), not the scaled
// displacement. Printer and the microMIPS spec use a 4-bit signed offset
// in bits 3:0, scale 4, base SP (non-R6).
bool mips16ReglistSpOffset(cs_insn* i, int64_t& off)
{
	if (i->size != 2)
	{
		return false;
	}
	unsigned bits = static_cast<unsigned>(i->bytes[0])
			| (static_cast<unsigned>(i->bytes[1]) << 8);
	int off4 = static_cast<int>(bits & 0xf);
	if (off4 & 8)
	{
		off4 -= 16;
	}
	off = static_cast<int64_t>(off4) << 2;
	return true;
}

llvm::Value* msaToI128(llvm::Value* v, llvm::IRBuilder<>& irb)
{
	auto* i128 = irb.getIntNTy(128);
	if (v->getType() == i128)
	{
		return v;
	}
	if (v->getType()->isFloatingPointTy())
	{
		v = irb.CreateBitCast(v, irb.getIntNTy(v->getType()->getPrimitiveSizeInBits()));
	}
	if (v->getType()->getPrimitiveSizeInBits() < 128)
	{
		return irb.CreateZExt(v, i128);
	}
	return irb.CreateTrunc(v, i128);
}

llvm::Value* msaPackedBinOp(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		llvm::Instruction::BinaryOps opc,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	return irb.CreateBitCast(irb.CreateBinOp(opc, v0, v1), irb.getIntNTy(128));
}

llvm::Value* msaPackedSatArith(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		bool isSigned,
		bool isSub,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* laneTy = irb.getIntNTy(elemBits);
	auto* wideTy = irb.getIntNTy(elemBits * 2);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	unsigned wideBits = elemBits * 2;
	llvm::APInt loA = isSigned ? llvm::APInt::getSignedMinValue(elemBits).sext(wideBits)
							   : llvm::APInt::getZero(wideBits);
	llvm::APInt hiA = isSigned ? llvm::APInt::getSignedMaxValue(elemBits).sext(wideBits)
							   : llvm::APInt::getMaxValue(elemBits).zext(wideBits);
	auto* loC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, loA));
	auto* hiC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, hiA));

	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* aw = isSigned ? irb.CreateSExt(v0, wideVec) : irb.CreateZExt(v0, wideVec);
	auto* bw = isSigned ? irb.CreateSExt(v1, wideVec) : irb.CreateZExt(v1, wideVec);
	auto* wide = isSub ? irb.CreateSub(aw, bw) : irb.CreateAdd(aw, bw);
	auto* c = irb.CreateSelect(irb.CreateICmpSLT(wide, loC), loC, wide);
	c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
	return irb.CreateBitCast(irb.CreateTrunc(c, vecTy), irb.getIntNTy(128));
}

llvm::Value* msaPackedSatN(
		llvm::Value* a,
		unsigned elemBits,
		unsigned satBits,
		bool isSigned,
		llvm::IRBuilder<>& irb)
{
	if (satBits == 0)
	{
		satBits = 1;
	}
	if (satBits > elemBits)
	{
		satBits = elemBits;
	}

	unsigned n = 128 / elemBits;
	auto* laneTy = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* v = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	llvm::APInt loA;
	llvm::APInt hiA;
	if (satBits == elemBits)
	{
		loA = isSigned ? llvm::APInt::getSignedMinValue(elemBits) : llvm::APInt::getZero(elemBits);
		hiA = isSigned ? llvm::APInt::getSignedMaxValue(elemBits) : llvm::APInt::getMaxValue(elemBits);
	}
	else
	{
		loA = isSigned ? llvm::APInt::getSignedMinValue(satBits).sext(elemBits)
					   : llvm::APInt::getZero(elemBits);
		hiA = isSigned ? llvm::APInt::getSignedMaxValue(satBits).sext(elemBits)
					   : llvm::APInt::getMaxValue(satBits).zext(elemBits);
	}
	auto* loC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(laneTy, loA));
	auto* hiC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(laneTy, hiA));
	if (isSigned)
	{
		auto* c = irb.CreateSelect(irb.CreateICmpSLT(v, loC), loC, v);
		c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
		return irb.CreateBitCast(c, irb.getIntNTy(128));
	}
	auto* c = irb.CreateSelect(irb.CreateICmpUGT(v, hiC), hiC, v);
	return irb.CreateBitCast(c, irb.getIntNTy(128));
}

llvm::Value* msaSlide(
		llvm::Value* wd,
		llvm::Value* ws,
		llvm::Value* slide,
		unsigned elemBits,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* i128 = irb.getIntNTy(128);
	auto* wdV = msaToI128(wd, irb);
	auto* wsV = msaToI128(ws, irb);
	if (!slide->getType()->isIntegerTy())
	{
		return wdV;
	}
	if (slide->getType()->getIntegerBitWidth() != 32)
	{
		slide = irb.CreateZExtOrTrunc(slide, irb.getInt32Ty());
	}
	auto* mod = irb.CreateURem(slide, llvm::ConstantInt::get(slide->getType(), n));
	auto* modBits32 = irb.CreateMul(mod, llvm::ConstantInt::get(mod->getType(), elemBits));
	auto* modBits = irb.CreateZExt(modBits32, i128);
	auto* isZero = irb.CreateICmpEQ(modBits, llvm::ConstantInt::get(i128, 0));
	auto* fromWd = irb.CreateLShr(wdV, modBits);
	auto* shWs = irb.CreateSub(llvm::ConstantInt::get(i128, 128), modBits);
	auto* shWsSafe = irb.CreateSelect(isZero, llvm::ConstantInt::get(i128, 0), shWs);
	auto* fromWs = irb.CreateShl(wsV, shWsSafe);
	auto* mixed = irb.CreateOr(fromWd, fromWs);
	return irb.CreateSelect(isZero, wdV, mixed);
}

unsigned msaLaneBits(uint32_t id)
{
	switch (id)
	{
		case MIPS_INS_ADDV_B:
		case MIPS_INS_SUBV_B:
		case MIPS_INS_MULV_B:
		case MIPS_INS_ILVEV_B:
		case MIPS_INS_ILVOD_B:
		case MIPS_INS_ILVL_B:
		case MIPS_INS_ILVR_B:
		case MIPS_INS_PCKEV_B:
		case MIPS_INS_PCKOD_B:
		case MIPS_INS_SHF_B:
		case MIPS_INS_SPLAT_B:
		case MIPS_INS_SPLATI_B:
		case MIPS_INS_CEQ_B:
		case MIPS_INS_CLE_S_B:
		case MIPS_INS_CLT_S_B:
		case MIPS_INS_MAX_S_B:
		case MIPS_INS_MIN_S_B:
		case MIPS_INS_SLL_B:
		case MIPS_INS_SRL_B:
		case MIPS_INS_COPY_S_B:
		case MIPS_INS_COPY_U_B:
		case MIPS_INS_INSERT_B:
		case MIPS_INS_FILL_B:
		case MIPS_INS_LDI_B:
		case MIPS_INS_ADDS_S_B:
		case MIPS_INS_ADDS_U_B:
		case MIPS_INS_SUBS_S_B:
		case MIPS_INS_SUBS_U_B:
		case MIPS_INS_CEQI_B:
		case MIPS_INS_SAT_S_B:
		case MIPS_INS_SAT_U_B:
		case MIPS_INS_SLD_B:
		case MIPS_INS_SLDI_B:
			return 8;
		case MIPS_INS_ADDV_H:
		case MIPS_INS_SUBV_H:
		case MIPS_INS_MULV_H:
		case MIPS_INS_ILVEV_H:
		case MIPS_INS_ILVOD_H:
		case MIPS_INS_ILVL_H:
		case MIPS_INS_ILVR_H:
		case MIPS_INS_PCKEV_H:
		case MIPS_INS_PCKOD_H:
		case MIPS_INS_SHF_H:
		case MIPS_INS_SPLAT_H:
		case MIPS_INS_SPLATI_H:
		case MIPS_INS_CEQ_H:
		case MIPS_INS_CLE_S_H:
		case MIPS_INS_CLT_S_H:
		case MIPS_INS_MAX_S_H:
		case MIPS_INS_MIN_S_H:
		case MIPS_INS_SLL_H:
		case MIPS_INS_SRL_H:
		case MIPS_INS_COPY_S_H:
		case MIPS_INS_COPY_U_H:
		case MIPS_INS_INSERT_H:
		case MIPS_INS_FILL_H:
		case MIPS_INS_LDI_H:
		case MIPS_INS_ADDS_S_H:
		case MIPS_INS_ADDS_U_H:
		case MIPS_INS_SUBS_S_H:
		case MIPS_INS_SUBS_U_H:
		case MIPS_INS_CEQI_H:
		case MIPS_INS_SAT_S_H:
		case MIPS_INS_SAT_U_H:
		case MIPS_INS_SLD_H:
		case MIPS_INS_SLDI_H:
			return 16;
		case MIPS_INS_ADDV_D:
		case MIPS_INS_SUBV_D:
		case MIPS_INS_MULV_D:
		case MIPS_INS_ILVEV_D:
		case MIPS_INS_ILVOD_D:
		case MIPS_INS_ILVL_D:
		case MIPS_INS_ILVR_D:
		case MIPS_INS_PCKEV_D:
		case MIPS_INS_PCKOD_D:
		case MIPS_INS_SPLAT_D:
		case MIPS_INS_SPLATI_D:
		case MIPS_INS_CEQ_D:
		case MIPS_INS_CLE_S_D:
		case MIPS_INS_CLT_S_D:
		case MIPS_INS_MAX_S_D:
		case MIPS_INS_MIN_S_D:
		case MIPS_INS_SLL_D:
		case MIPS_INS_SRL_D:
		case MIPS_INS_COPY_S_D:
		case MIPS_INS_INSERT_D:
		case MIPS_INS_FILL_D:
		case MIPS_INS_LDI_D:
		case MIPS_INS_ADDS_S_D:
		case MIPS_INS_ADDS_U_D:
		case MIPS_INS_SUBS_S_D:
		case MIPS_INS_SUBS_U_D:
		case MIPS_INS_CEQI_D:
		case MIPS_INS_SAT_S_D:
		case MIPS_INS_SAT_U_D:
		case MIPS_INS_SLD_D:
		case MIPS_INS_SLDI_D:
			return 64;
		default:
			return 32;
	}
}

llvm::Value* msaPackedSminMax(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		bool isMax,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* cmp = isMax ? irb.CreateICmpSGT(v0, v1) : irb.CreateICmpSLT(v0, v1);
	return irb.CreateBitCast(irb.CreateSelect(cmp, v0, v1), irb.getIntNTy(128));
}

llvm::Value* msaIlvEvenOdd(
		llvm::Value* ws,
		llvm::Value* wt,
		unsigned elemBits,
		bool odd,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v0 = irb.CreateBitCast(msaToI128(ws, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(wt, irb), vecTy);
	std::vector<int> mask;
	mask.reserve(n);
	unsigned off = odd ? 1u : 0u;
	for (unsigned i = 0; i < n / 2; ++i)
	{
		mask.push_back(static_cast<int>(n + 2 * i + off));
		mask.push_back(static_cast<int>(2 * i + off));
	}
	return irb.CreateBitCast(irb.CreateShuffleVector(v0, v1, mask), irb.getIntNTy(128));
}

llvm::Value* msaIlvLeftRight(
		llvm::Value* ws,
		llvm::Value* wt,
		unsigned elemBits,
		bool left,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v0 = irb.CreateBitCast(msaToI128(ws, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(wt, irb), vecTy);
	std::vector<int> mask;
	mask.reserve(n);
	unsigned off = left ? n / 2 : 0u;
	for (unsigned i = 0; i < n / 2; ++i)
	{
		mask.push_back(static_cast<int>(n + off + i));
		mask.push_back(static_cast<int>(off + i));
	}
	return irb.CreateBitCast(irb.CreateShuffleVector(v0, v1, mask), irb.getIntNTy(128));
}

llvm::Value* msaPckEvenOdd(
		llvm::Value* ws,
		llvm::Value* wt,
		unsigned elemBits,
		bool odd,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v0 = irb.CreateBitCast(msaToI128(ws, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(wt, irb), vecTy);
	std::vector<int> mask;
	mask.reserve(n);
	unsigned off = odd ? 1u : 0u;
	for (unsigned i = 0; i < n / 2; ++i)
	{
		mask.push_back(static_cast<int>(n + 2 * i + off));
	}
	for (unsigned i = 0; i < n / 2; ++i)
	{
		mask.push_back(static_cast<int>(2 * i + off));
	}
	return irb.CreateBitCast(irb.CreateShuffleVector(v0, v1, mask), irb.getIntNTy(128));
}

enum class MsaCmpPred
{
	Eq,
	Sle,
	Slt
};

llvm::Value* msaPackedCmp(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		MsaCmpPred pred,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	llvm::Value* cmp = nullptr;
	switch (pred)
	{
		case MsaCmpPred::Eq:
			cmp = irb.CreateICmpEQ(v0, v1);
			break;
		case MsaCmpPred::Sle:
			cmp = irb.CreateICmpSLE(v0, v1);
			break;
		case MsaCmpPred::Slt:
			cmp = irb.CreateICmpSLT(v0, v1);
			break;
	}
	return irb.CreateBitCast(irb.CreateSExt(cmp, vecTy), irb.getIntNTy(128));
}

llvm::Value* msaPackedFbinW(
		llvm::Value* a,
		llvm::Value* b,
		llvm::Instruction::BinaryOps opc,
		llvm::IRBuilder<>& irb)
{
	auto* vecTy = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	return irb.CreateBitCast(irb.CreateBinOp(opc, v0, v1), irb.getIntNTy(128));
}

llvm::Value* msaPackedFminmaxW(
		llvm::Value* a,
		llvm::Value* b,
		bool isMax,
		llvm::IRBuilder<>& irb)
{
	auto* vecTy = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* cmp = isMax ? irb.CreateFCmpOGT(v0, v1) : irb.CreateFCmpOLT(v0, v1);
	return irb.CreateBitCast(irb.CreateSelect(cmp, v0, v1), irb.getIntNTy(128));
}

llvm::Value* msaPackedShift(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		llvm::Instruction::BinaryOps opc,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(elemTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* amt = irb.CreateAnd(
			v1,
			llvm::ConstantVector::getSplat(
					llvm::ElementCount::getFixed(n),
					llvm::ConstantInt::get(elemTy, elemBits - 1)));
	return irb.CreateBitCast(irb.CreateBinOp(opc, v0, amt), irb.getIntNTy(128));
}

} // namespace

/**
 * MIPS_INS_LWM16, MIPS_INS_LWM32 — Capstone 6 expands the microMIPS rlist
 * field into REG operands with is_reglist plus a folded MEM. Consecutive
 * words, first register at the effective address.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateLoadMultiple(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	std::vector<uint32_t> regs;
	unsigned memIdx = ~0u;
	if (!collectMipsRegListAndMem(mi, regs, memIdx))
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	int64_t off16 = 0;
	llvm::Value* addr = nullptr;
	if (mips16ReglistSpOffset(i, off16))
	{
		auto* sp = loadRegister(MIPS_REG_SP, irb);
		addr = irb.CreateAdd(sp, llvm::ConstantInt::getSigned(sp->getType(), off16));
	}
	else
	{
		addr = loadOp(mi->operands[memIdx], irb, nullptr, /*lea=*/true);
	}
	auto* i32 = irb.getInt32Ty();
	auto* four = llvm::ConstantInt::get(addr->getType(), 4);
	for (uint32_t r : regs)
	{
		storeRegister(r, loadIntPtr(irb, addr, i32), irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
		addr = irb.CreateAdd(addr, four);
	}
}

/**
 * MIPS_INS_SWM16, MIPS_INS_SWM32
 */
void Capstone2LlvmIrTranslatorMips_impl::translateStoreMultiple(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	std::vector<uint32_t> regs;
	unsigned memIdx = ~0u;
	if (!collectMipsRegListAndMem(mi, regs, memIdx))
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	int64_t off16 = 0;
	llvm::Value* addr = nullptr;
	if (mips16ReglistSpOffset(i, off16))
	{
		auto* sp = loadRegister(MIPS_REG_SP, irb);
		addr = irb.CreateAdd(sp, llvm::ConstantInt::getSigned(sp->getType(), off16));
	}
	else
	{
		addr = loadOp(mi->operands[memIdx], irb, nullptr, /*lea=*/true);
	}
	auto* i32 = irb.getInt32Ty();
	auto* four = llvm::ConstantInt::get(addr->getType(), 4);
	for (uint32_t r : regs)
	{
		llvm::Value* v = loadRegister(r, irb);
		storeIntPtr(irb, irb.CreateZExtOrTrunc(v, i32), addr, i32);
		addr = irb.CreateAdd(addr, four);
	}
}

/**
 * MIPS_INS_ADDV_B/H/W/D — lane-wise add of the 128-bit MSA register,
 * matching x86 PADDB/PADDW/PADDD/PADDQ. Not a 128-bit integer add.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaAddv(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedBinOp(op1, op2, msaLaneBits(i->id), llvm::Instruction::Add, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SUBV_B/H/W/D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSubv(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedBinOp(op1, op2, msaLaneBits(i->id), llvm::Instruction::Sub, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ADDS_S_B/H/W/D, MIPS_INS_ADDS_U_* — saturating lane add
 * (x86 PADDSB/PADDUSB). MIPS_INS_SUBS_S_* / SUBS_U_* — saturating sub
 * (PSUBSB/PSUBUSB). Same select clamp ARM64 SQADD uses, not llvm.sadd.sat.
 * Capstone 6 reports suffixed ids; unsuffixed ADDS_S is a compat macro onto
 * ADDS_S_W.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSatArith(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	bool isSigned = i->id == MIPS_INS_ADDS_S_B
			|| i->id == MIPS_INS_ADDS_S_H
			|| i->id == MIPS_INS_ADDS_S_W
			|| i->id == MIPS_INS_ADDS_S_D
			|| i->id == MIPS_INS_SUBS_S_B
			|| i->id == MIPS_INS_SUBS_S_H
			|| i->id == MIPS_INS_SUBS_S_W
			|| i->id == MIPS_INS_SUBS_S_D;
	bool isSub = i->id == MIPS_INS_SUBS_S_B
			|| i->id == MIPS_INS_SUBS_S_H
			|| i->id == MIPS_INS_SUBS_S_W
			|| i->id == MIPS_INS_SUBS_S_D
			|| i->id == MIPS_INS_SUBS_U_B
			|| i->id == MIPS_INS_SUBS_U_H
			|| i->id == MIPS_INS_SUBS_U_W
			|| i->id == MIPS_INS_SUBS_U_D;
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedSatArith(op1, op2, msaLaneBits(i->id), isSigned, isSub, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_MULV_B/H/W/D — lane-wise multiply, wrapping like x86 PMULLW/PMULLD.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaMulv(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedBinOp(op1, op2, msaLaneBits(i->id), llvm::Instruction::Mul, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ILVEV_B/H/W/D — interleave even lanes (x86 PUNPCKL analog).
 * MIPS_INS_ILVOD_* — interleave odd lanes (x86 PUNPCKH analog).
 * MIPS_INS_ILVL_* — interleave the high (left) half of each source.
 * MIPS_INS_ILVR_* — interleave the low (right) half of each source.
 *
 * ILVEV/ILVOD: wd[2*i] = wt[2*i+off], wd[2*i+1] = ws[2*i+off]
 * with off = 0 (even) or 1 (odd).
 * ILVL/ILVR:   wd[2*i] = wt[i+off],   wd[2*i+1] = ws[i+off]
 * with off = n/2 (left) or 0 (right).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaIlv(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	bool odd = i->id == MIPS_INS_ILVOD_B
			|| i->id == MIPS_INS_ILVOD_H
			|| i->id == MIPS_INS_ILVOD_W
			|| i->id == MIPS_INS_ILVOD_D;
	bool left = i->id == MIPS_INS_ILVL_B
			|| i->id == MIPS_INS_ILVL_H
			|| i->id == MIPS_INS_ILVL_W
			|| i->id == MIPS_INS_ILVL_D;
	bool right = i->id == MIPS_INS_ILVR_B
			|| i->id == MIPS_INS_ILVR_H
			|| i->id == MIPS_INS_ILVR_W
			|| i->id == MIPS_INS_ILVR_D;
	llvm::Value* res = (left || right)
			? msaIlvLeftRight(op1, op2, msaLaneBits(i->id), left, irb)
			: msaIlvEvenOdd(op1, op2, msaLaneBits(i->id), odd, irb);
	storeOp(mi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SHF_B/H/W — 4-element group shuffle by an 8-bit immediate,
 * matching x86 PSHUFD / PSHUFB-class. Each 2-bit field of the imm selects
 * a source lane inside the group: dst[4*g+j] = src[4*g + ((imm >> 2*j) & 3)].
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaShf(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	if (mi->operands[2].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned elemBits = msaLaneBits(i->id);
	unsigned n = 128 / elemBits;
	if (n % 4 != 0)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	op1 = loadOp(mi->operands[1], irb);
	uint8_t imm = static_cast<uint8_t>(mi->operands[2].imm);
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v = irb.CreateBitCast(msaToI128(op1, irb), vecTy);
	std::vector<int> mask;
	mask.reserve(n);
	unsigned groups = n / 4;
	for (unsigned g = 0; g < groups; ++g)
	{
		for (unsigned j = 0; j < 4; ++j)
		{
			mask.push_back(static_cast<int>(g * 4 + ((imm >> (2 * j)) & 3)));
		}
	}
	storeOp(
			mi->operands[0],
			irb.CreateBitCast(irb.CreateShuffleVector(v, v, mask), irb.getIntNTy(128)),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SPLAT_B/H/W/D — replicate lane ws[rt] (rt modulo lane count)
 * across wd. MIPS_INS_SPLATI_* is the same with an immediate index.
 * x86 analog: PSHUFLW / MOVDDUP of a selected lane.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSplat(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	unsigned n = 128 / elemBits;
	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	if (!op2->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	if (op2->getType()->getIntegerBitWidth() != 32)
	{
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
	}
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v = irb.CreateBitCast(msaToI128(op1, irb), vecTy);
	auto* idx = irb.CreateURem(op2, llvm::ConstantInt::get(op2->getType(), n));
	auto* elem = irb.CreateExtractElement(v, idx);
	storeOp(
			mi->operands[0],
			irb.CreateBitCast(irb.CreateVectorSplat(n, elem), irb.getIntNTy(128)),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_MAX_S_B/H/W/D — signed per-lane max (x86 PMAXSD family).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaMaxs(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedSminMax(op1, op2, msaLaneBits(i->id), true, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_MIN_S_B/H/W/D — signed per-lane min.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaMins(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedSminMax(op1, op2, msaLaneBits(i->id), false, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SLL_B/H/W/D — per-lane left shift (x86 PSLLW analog, but
 * MSA takes a per-element count modulo the lane width).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSll(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedShift(op1, op2, msaLaneBits(i->id), llvm::Instruction::Shl, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SRL_B/H/W/D — per-lane logical right shift.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSrl(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedShift(op1, op2, msaLaneBits(i->id), llvm::Instruction::LShr, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ANDI_B / ORI_B / XORI_B / NORI_B — byte logic of ws with a splat
 * immediate (x86 PAND/POR/PXOR + splat). Distinct from scalar ANDI/ORI/XORI:
 * every lane sees the same 8-bit immediate. NORI_B is NOT(ws OR splat).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaLogiB(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	if (mi->operands[2].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	if (!op2->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* elemTy = irb.getInt8Ty();
	auto* vecTy = llvm::FixedVectorType::get(elemTy, 16);
	auto* v = irb.CreateBitCast(msaToI128(op1, irb), vecTy);
	auto* imm8 = irb.CreateVectorSplat(16, irb.CreateZExtOrTrunc(op2, elemTy));
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case MIPS_INS_ANDI_B:
			res = irb.CreateAnd(v, imm8);
			break;
		case MIPS_INS_XORI_B:
			res = irb.CreateXor(v, imm8);
			break;
		case MIPS_INS_NORI_B:
			res = irb.CreateNot(irb.CreateOr(v, imm8));
			break;
		default:
			res = irb.CreateOr(v, imm8);
			break;
	}
	storeOp(
			mi->operands[0],
			irb.CreateBitCast(res, irb.getIntNTy(128)),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_PCKEV_B/H/W/D — pack even lanes of wt then ws (PACKUSWB-class layout,
 * without saturation). MIPS_INS_PCKOD_* packs the odd lanes.
 *
 * wd[i]     = wt[2*i+off]
 * wd[i+n/2] = ws[2*i+off]
 * with off = 0 (even) or 1 (odd).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaPck(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	bool odd = i->id == MIPS_INS_PCKOD_B
			|| i->id == MIPS_INS_PCKOD_H
			|| i->id == MIPS_INS_PCKOD_W
			|| i->id == MIPS_INS_PCKOD_D;
	storeOp(
			mi->operands[0],
			msaPckEvenOdd(op1, op2, msaLaneBits(i->id), odd, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_BSEL_V — bitwise select (PPC VSEL analog; x86 PBLENDVB is the
 * byte-MSB cousin). wd is the mask:
 *   wd = (ws AND wd) OR (wt AND NOT wd)
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaBsel(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(mi, irb);
	auto* mask = msaToI128(op0, irb);
	auto* ws = msaToI128(op1, irb);
	auto* wt = msaToI128(op2, irb);
	auto* res = irb.CreateOr(irb.CreateAnd(ws, mask), irb.CreateAnd(wt, irb.CreateNot(mask)));
	storeOp(mi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_CEQ_B/H/W/D — lane compare equal, all-1s mask (x86 PCMPEQB).
 * MIPS_INS_CEQI_B/H/W/D — same against a splat of the signed 5-bit immediate.
 * MIPS_INS_CLE_S_* / CLT_S_* — signed <= / <, same mask layout.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaCmp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	bool ceqi = i->id == MIPS_INS_CEQI_B
			|| i->id == MIPS_INS_CEQI_H
			|| i->id == MIPS_INS_CEQI_W
			|| i->id == MIPS_INS_CEQI_D;
	if (ceqi)
	{
		if (mi->op_count != 3 || mi->operands[2].type != MIPS_OP_IMM)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
		op1 = loadOp(mi->operands[1], irb);
		unsigned n = 128 / elemBits;
		llvm::APInt simm(5, static_cast<uint64_t>(mi->operands[2].imm), true);
		auto* lane = llvm::ConstantInt::get(irb.getIntNTy(elemBits), simm.sext(elemBits));
		op2 = irb.CreateBitCast(irb.CreateVectorSplat(n, lane), irb.getIntNTy(128));
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}

	MsaCmpPred pred = MsaCmpPred::Eq;
	switch (i->id)
	{
		case MIPS_INS_CLE_S_B:
		case MIPS_INS_CLE_S_H:
		case MIPS_INS_CLE_S_W:
		case MIPS_INS_CLE_S_D:
			pred = MsaCmpPred::Sle;
			break;
		case MIPS_INS_CLT_S_B:
		case MIPS_INS_CLT_S_H:
		case MIPS_INS_CLT_S_W:
		case MIPS_INS_CLT_S_D:
			pred = MsaCmpPred::Slt;
			break;
		default:
			pred = MsaCmpPred::Eq;
			break;
	}
	storeOp(
			mi->operands[0],
			msaPackedCmp(op1, op2, elemBits, pred, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SAT_S_B/H/W/D — saturate each signed lane to n+1 bits
 * (PACKSS-class clamp). MIPS_INS_SAT_U_* saturates unsigned to [0, 2^(n+1)-1].
 * Capstone reports `wd, ws, n` as W, W, IMM.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSat(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	if (mi->operands[2].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	bool isSigned = i->id == MIPS_INS_SAT_S_B
			|| i->id == MIPS_INS_SAT_S_H
			|| i->id == MIPS_INS_SAT_S_W
			|| i->id == MIPS_INS_SAT_S_D;
	unsigned satBits = static_cast<unsigned>(mi->operands[2].imm) + 1u;
	op1 = loadOp(mi->operands[1], irb);
	storeOp(
			mi->operands[0],
			msaPackedSatN(op1, msaLaneBits(i->id), satBits, isSigned, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SLD_B/H/W/D — slide wd||ws left by GPR rt lanes (x86 PALIGNR /
 * PSLLDQ-class). Syntax `sld.b $wd, $ws[$rt]`; Capstone operands are
 * W dest, W ws, GPR rt. wd is also the low source. SLDI_* takes an
 * immediate count instead of a GPR.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSld(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	op0 = loadOp(mi->operands[0], irb);
	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	if (!op2->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	storeOp(
			mi->operands[0],
			msaSlide(op0, op1, op2, msaLaneBits(i->id), irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_FADD_W / FSUB_W / FMUL_W / FDIV_W — MSA packed single
 * (x86 ADDPS/SUBPS/MULPS/DIVPS). FMAX_W / FMIN_W are MAXPS/MINPS: ordered
 * compare plus select, not llvm.maxnum. W registers are i128; bitcast to
 * <4 x float> is the whole conversion.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFbin(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	if (i->id == MIPS_INS_FMAX_W || i->id == MIPS_INS_FMIN_W)
	{
		storeOp(
				mi->operands[0],
				msaPackedFminmaxW(op1, op2, i->id == MIPS_INS_FMAX_W, irb),
				irb,
				eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	llvm::Instruction::BinaryOps opc = llvm::Instruction::FAdd;
	switch (i->id)
	{
		case MIPS_INS_FSUB_W:
			opc = llvm::Instruction::FSub;
			break;
		case MIPS_INS_FMUL_W:
			opc = llvm::Instruction::FMul;
			break;
		case MIPS_INS_FDIV_W:
			opc = llvm::Instruction::FDiv;
			break;
		default:
			opc = llvm::Instruction::FAdd;
			break;
	}
	storeOp(
			mi->operands[0],
			msaPackedFbinW(op1, op2, opc, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_BSELI_B — immediate-mask select (byte splat of i8):
 *   wd = (ws AND splat) OR (wd AND NOT splat)
 * Capstone 6's unsuffixed BSELI is a compat macro onto BSELI_B.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaBseli(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	if (mi->operands[2].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	op0 = loadOp(mi->operands[0], irb);
	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	if (!op2->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* elemTy = irb.getInt8Ty();
	auto* vecTy = llvm::FixedVectorType::get(elemTy, 16);
	auto* wd = irb.CreateBitCast(msaToI128(op0, irb), vecTy);
	auto* ws = irb.CreateBitCast(msaToI128(op1, irb), vecTy);
	auto* mask = irb.CreateVectorSplat(16, irb.CreateZExtOrTrunc(op2, elemTy));
	auto* res = irb.CreateOr(irb.CreateAnd(ws, mask), irb.CreateAnd(wd, irb.CreateNot(mask)));
	storeOp(mi->operands[0], irb.CreateBitCast(res, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_COPY_S_B/H/W/D — extract one lane to a GPR with sign-extend
 * (x86 PEXTR + MOVSX). COPY_U_* zero-extends (PEXTR + MOVZX).
 * Capstone reports the lane as a following IMM, not a vector_index field.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaCopy(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	if (mi->operands[2].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned elemBits = msaLaneBits(i->id);
	unsigned n = 128 / elemBits;
	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	if (!op2->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	if (op2->getType()->getIntegerBitWidth() != 32)
	{
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
	}
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v = irb.CreateBitCast(msaToI128(op1, irb), vecTy);
	auto* idx = irb.CreateURem(op2, llvm::ConstantInt::get(op2->getType(), n));
	auto* elem = irb.CreateExtractElement(v, idx);
	bool sign = i->id == MIPS_INS_COPY_S_B
			|| i->id == MIPS_INS_COPY_S_H
			|| i->id == MIPS_INS_COPY_S_W
			|| i->id == MIPS_INS_COPY_S_D;
	storeOp(
			mi->operands[0],
			elem,
			irb,
			sign ? eOpConv::SEXT_TRUNC_OR_BITCAST : eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_INSERT_B/H/W/D — insert the low bits of a GPR into one MSA lane
 * (x86 PINSR; ARM64 INS). Capstone reports `wd[n], rs` as W, IMM, GPR.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaInsert(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	if (mi->operands[1].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned elemBits = msaLaneBits(i->id);
	unsigned n = 128 / elemBits;
	op0 = loadOp(mi->operands[0], irb);
	op2 = loadOp(mi->operands[2], irb);
	if (!op2->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	unsigned laneIdx = static_cast<unsigned>(mi->operands[1].imm) % n;
	auto* laneTy = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* v = irb.CreateBitCast(msaToI128(op0, irb), vecTy);
	auto* lane = irb.CreateZExtOrTrunc(op2, laneTy);
	storeOp(
			mi->operands[0],
			irb.CreateBitCast(
					irb.CreateInsertElement(v, lane, static_cast<uint64_t>(laneIdx)),
					irb.getIntNTy(128)),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_FILL_B/H/W/D — splat the low bits of a GPR across wd
 * (x86 VPBROADCAST). FILL.D sign-extends a 32-bit GPR into each 64-bit lane.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFill(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	unsigned n = 128 / elemBits;
	op1 = loadOp(mi->operands[1], irb);
	if (!op1->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	auto* lane = irb.CreateSExtOrTrunc(op1, irb.getIntNTy(elemBits));
	storeOp(
			mi->operands[0],
			irb.CreateBitCast(irb.CreateVectorSplat(n, lane), irb.getIntNTy(128)),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_LDI_B/H/W/D — splat a signed 10-bit immediate across wd.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaLdi(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	if (mi->operands[1].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned elemBits = msaLaneBits(i->id);
	unsigned n = 128 / elemBits;
	op1 = loadOp(mi->operands[1], irb);
	if (!op1->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	auto* lane = irb.CreateSExtOrTrunc(op1, irb.getIntNTy(elemBits));
	storeOp(
			mi->operands[0],
			irb.CreateBitCast(irb.CreateVectorSplat(n, lane), irb.getIntNTy(128)),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SELEQZ — rd = (rt == 0) ? rs : 0
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSeleqz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* zero = llvm::ConstantInt::get(op1->getType(), 0);
	if (op2->getType() != op1->getType())
	{
		op2 = irb.CreateZExtOrTrunc(op2, op1->getType());
	}
	auto* val = irb.CreateSelect(irb.CreateICmpEQ(op2, llvm::ConstantInt::get(op2->getType(), 0)), op1, zero);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_SELNEZ — rd = (rt != 0) ? rs : 0
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSelnez(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* zero = llvm::ConstantInt::get(op1->getType(), 0);
	if (op2->getType() != op1->getType())
	{
		op2 = irb.CreateZExtOrTrunc(op2, op1->getType());
	}
	auto* val = irb.CreateSelect(irb.CreateICmpNE(op2, llvm::ConstantInt::get(op2->getType(), 0)), op1, zero);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_SEL_S, MIPS_INS_SEL_D — fd = (lsb(ft) == 1) ? fs : fd
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSel(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(mi, irb);
	llvm::Value* bits = op2;
	if (bits->getType()->isFloatingPointTy())
	{
		bits = irb.CreateBitCast(bits, irb.getIntNTy(bits->getType()->getPrimitiveSizeInBits()));
	}
	auto* lsb = irb.CreateTrunc(bits, irb.getInt1Ty());
	storeOp(mi->operands[0], irb.CreateSelect(lsb, op1, op0), irb);
}

/**
 * MIPS_INS_LB, MIPS_INS_LBU, MIPS_INS_LBU16,
 * MIPS_INS_LH, MIPS_INS_LHU, MIPS_INS_LHU16,
 * MIPS_INS_LW, MIPS_INS_LW16, MIPS_INS_LWU,
 * MIPS_INS_LD, MIPS_INS_LDC3,
 * MIPS_INS_LWC1, MIPS_INS_LDC1,
 * MIPS_INS_LD_B, MIPS_INS_LD_H, MIPS_INS_LD_W, MIPS_INS_LD_D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateLoadMemory(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	llvm::Type* ty = nullptr;
	eOpConv ct = eOpConv::THROW;

	switch (i->id)
	{
		case MIPS_INS_LB: ty = irb.getInt8Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LBU:
		case MIPS_INS_LBU16: ty = irb.getInt8Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LH: ty = irb.getInt16Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LHU:
		case MIPS_INS_LHU16: ty = irb.getInt16Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LW:
		case MIPS_INS_LW16:
		case MIPS_INS_LL: ty = irb.getInt32Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LWU: ty = irb.getInt32Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LD:
		case MIPS_INS_LLD: ty = irb.getInt64Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LDC3: ty = irb.getInt64Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LWC1: ty = irb.getFloatTy(); ct = eOpConv::FPCAST_OR_BITCAST; break;
		case MIPS_INS_LDC1: ty = irb.getDoubleTy(); ct = eOpConv::FPCAST_OR_BITCAST; break;
		case MIPS_INS_LD_B:
		case MIPS_INS_LD_H:
		case MIPS_INS_LD_W:
		case MIPS_INS_LD_D:
			ty = irb.getIntNTy(128);
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateLoadMemory().");
	}

	op1 = loadOp(mi->operands[1], irb, ty);
	if (i->id == MIPS_INS_LL || i->id == MIPS_INS_LLD)
	{
		if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(op1))
		{
			ld->setAtomic(llvm::AtomicOrdering::Monotonic);
		}
	}
	storeOp(mi->operands[0], op1, irb, ct);
}

/**
 * MIPS_INS_SB, MIPS_INS_SB16, MIPS_INS_SH, MIPS_INS_SH16,
 * MIPS_INS_SW, MIPS_INS_SW16, MIPS_INS_SD, MIPS_INS_SDC3,
 * MIPS_INS_SWC1, MIPS_INS_SDC1,
 * MIPS_INS_ST_B, MIPS_INS_ST_H, MIPS_INS_ST_W, MIPS_INS_ST_D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateStoreMemory(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case MIPS_INS_SB:
		case MIPS_INS_SB16: ty = irb.getInt8Ty(); break;
		case MIPS_INS_SH:
		case MIPS_INS_SH16: ty = irb.getInt16Ty(); break;
		case MIPS_INS_SW:
		case MIPS_INS_SW16:
		case MIPS_INS_SC: ty = irb.getInt32Ty(); break;
		case MIPS_INS_SD:
		case MIPS_INS_SCD: ty = irb.getInt64Ty(); break;
		case MIPS_INS_SDC3: ty = irb.getInt64Ty(); break;
		case MIPS_INS_SWC1: ty = irb.getFloatTy(); break;
		case MIPS_INS_SDC1: ty = irb.getDoubleTy(); break;
		case MIPS_INS_ST_B:
		case MIPS_INS_ST_H:
		case MIPS_INS_ST_W:
		case MIPS_INS_ST_D:
			ty = irb.getIntNTy(128);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateStoreMemory().");
	}

	op0 = loadOp(mi->operands[0], irb);
	if (ty->isFloatingPointTy())
	{
		// This is not exact, in 64-bit mode, only lower 32-bits of FPR should
		// be used -> truncate, not cast.
		op0 = irb.CreateFPCast(op0, ty);
	}
	else if (ty->isIntegerTy())
	{
		op0 = irb.CreateZExtOrTrunc(op0, ty);
	}
	else
	{
		throw GenericError("unhandled type");
	}
	auto* stored = storeOp(mi->operands[1], op0, irb);
	if (i->id == MIPS_INS_SC || i->id == MIPS_INS_SCD)
	{
		if (auto* st = llvm::dyn_cast<llvm::StoreInst>(stored))
		{
			st->setAtomic(llvm::AtomicOrdering::Monotonic);
		}
		// Success = 1. No exclusive-monitor model.
		storeOp(
				mi->operands[0],
				llvm::ConstantInt::get(getDefaultType(), 1),
				irb);
	}
}

/**
 * MIPS_INS_LWL, MIPS_INS_LWR, MIPS_INS_SWL, MIPS_INS_SWR,
 * MIPS_INS_LDL, MIPS_INS_LDR, MIPS_INS_SDL, MIPS_INS_SDR
 *
 * The unaligned pair. MIPS has no unaligned load, so a compiler that must read
 * a word from an address it cannot prove aligned emits two instructions, each
 * of which transfers the part of the word that lies on one side of the
 * containing aligned word's boundary:
 *
 *     lwl $2, 0($3)       big-endian        lwl $2, 3($3)    little-endian
 *     lwr $2, 3($3)                         lwr $2, 0($3)
 *
 * 12,442 occurrences in the static parity corpus across the four word forms --
 * the largest group left on MIPS and the largest specifiable group on any
 * architecture outside x86's AVX. The loads were
 * `translatePseudoAsmOp0FncOp1`, which at least returned a value; the stores
 * were `translatePseudoAsmFncOp0Op1`, which wrote no memory at all.
 *
 * Written against the containing ALIGNED word rather than as a run of byte
 * accesses. That is both what the hardware does -- one bus transaction -- and
 * the form from which a later pass can recognise the pair: two reads of the
 * same aligned words merging into one value.
 *
 * The shift the merge needs is `8 * (EA & 3)` on a big-endian MIPS and
 * `8 * (3 - (EA & 3))` on a little-endian one, and the other member of the
 * pair uses its complement. Every one of the eight cases is one of those two
 * shifts:
 *
 *     LWL   rt = (W << s)  | (rt & lowMask(s))
 *     LWR   rt = (W >> r)  | (rt & highMask(r))
 *     SWL   W  = (rt >> s) | (W  & highMask(s))
 *     SWR   W  = (rt << r) | (W  & lowMask(r))
 *
 * with `r = (bytes - 1) * 8 - s`. Endianness therefore enters in exactly one
 * place, and it is read from the translator's own Capstone mode rather than
 * assumed -- the corpus is big-endian `mips-linux-gnu` and this test suite is
 * little-endian, so an implementation that hard-codes either is wrong for the
 * other and there is no configuration in which both are exercised by accident.
 *
 * The masks are built at twice the width and truncated, because `highMask(0)`
 * is `~((1 << width) - 1)` and a shift equal to the operand width is poison in
 * LLVM. `EA & 3` is a run-time value, so the shifts are run-time too and the
 * case split cannot be done in C++.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateUnalignedMemory(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	unsigned bytes = 0;
	bool store = false;
	bool left = false;
	switch (i->id)
	{
	case MIPS_INS_LWL:
		bytes = 4;
		left = true;
		break;
	case MIPS_INS_LWR:
		bytes = 4;
		left = false;
		break;
	case MIPS_INS_SWL:
		bytes = 4;
		left = true;
		store = true;
		break;
	case MIPS_INS_SWR:
		bytes = 4;
		left = false;
		store = true;
		break;
	case MIPS_INS_LDL:
		bytes = 8;
		left = true;
		break;
	case MIPS_INS_LDR:
		bytes = 8;
		left = false;
		break;
	case MIPS_INS_SDL:
		bytes = 8;
		left = true;
		store = true;
		break;
	case MIPS_INS_SDR:
		bytes = 8;
		left = false;
		store = true;
		break;
	default: throw GenericError("Unhandled insn ID in translateUnalignedMemory().");
	}

	// The doubleword forms are MIPS64 only.
	if (bytes == 8 && getArchByteSize() < 8)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* ty = irb.getIntNTy(bytes * 8);
	auto* wide = irb.getIntNTy(bytes * 16);

	llvm::Value* ea = loadOp(mi->operands[1], irb, nullptr, /*lea=*/true);
	ea = irb.CreateZExtOrTrunc(ea, ty);

	llvm::Value* off = irb.CreateAnd(ea, llvm::ConstantInt::get(ty, bytes - 1));
	llvm::Value* aligned = irb.CreateAnd(ea, llvm::ConstantInt::get(ty, ~static_cast<uint64_t>(bytes - 1)));

	// s on a big-endian MIPS, its complement on a little-endian one.
	llvm::Value* s = irb.CreateShl(off, llvm::ConstantInt::get(ty, 3));
	if (getExtraMode() != CS_MODE_BIG_ENDIAN)
	{
		s = irb.CreateSub(llvm::ConstantInt::get(ty, (bytes - 1) * 8), s);
	}
	llvm::Value* r = irb.CreateSub(llvm::ConstantInt::get(ty, (bytes - 1) * 8), s);

	// `ones(n)` is the low n bits set. Built one width up, because a shift by
	// the whole width is poison in LLVM and n reaches the width here.
	auto ones = [&](llvm::Value* n) -> llvm::Value* {
		llvm::Value* w = irb.CreateShl(llvm::ConstantInt::get(wide, 1), irb.CreateZExt(n, wide));
		w = irb.CreateSub(w, llvm::ConstantInt::get(wide, 1));
		return irb.CreateTrunc(w, ty);
	};

	// lowMask(n) keeps the bottom n bits. highMask(n) keeps the TOP n, which is
	// `ones` of the complement -- not of n. Getting that wrong is the one
	// mistake here that still produces a plausible answer, because for the
	// middle alignments the two masks are the same size.
	auto lowMask = [&](llvm::Value* n) { return ones(n); };
	auto highMask = [&](llvm::Value* n) {
		return irb.CreateNot(ones(irb.CreateSub(llvm::ConstantInt::get(ty, bytes * 8), n)));
	};

	llvm::Value* rt = loadOp(mi->operands[0], irb);
	rt = irb.CreateZExtOrTrunc(rt, ty);

	if (store)
	{
		llvm::Value* mem = loadIntPtr(irb, aligned, ty);
		llvm::Value* merged = left ? irb.CreateOr(irb.CreateLShr(rt, s), irb.CreateAnd(mem, highMask(s)))
								   : irb.CreateOr(irb.CreateShl(rt, r), irb.CreateAnd(mem, lowMask(r)));
		storeIntPtr(irb, merged, aligned, ty);
	}
	else
	{
		llvm::Value* mem = loadIntPtr(irb, aligned, ty);
		llvm::Value* merged = left ? irb.CreateOr(irb.CreateShl(mem, s), irb.CreateAnd(rt, lowMask(s)))
								   : irb.CreateOr(irb.CreateLShr(mem, r), irb.CreateAnd(rt, highMask(r)));
		storeOp(mi->operands[0], merged, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	}
}

/**
 * MIPS_INS_DSLL32, MIPS_INS_DSRL32, MIPS_INS_DSRA32, MIPS_INS_DROTR32
 *
 * The shift-by-more-than-31 forms. A MIPS64 shift immediate is five bits, so
 * shifting a doubleword by 32 or more needs a second opcode: `dsll32 rd, rt,
 * sa` shifts by sa + 32. The assembler hides this -- you write `dsll $2, $3,
 * 40` and get `dsll32 $2, $3, 8` -- and a translation that takes the reported
 * immediate at face value is off by exactly 32, which for a shift is the
 * difference between a value and zero.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateDoubleShift32(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	auto* ty = op1->getType();
	op2 = irb.CreateZExtOrTrunc(op2, ty);

	auto* amount = irb.CreateAdd(op2, llvm::ConstantInt::get(ty, 32));

	llvm::Value* res = nullptr;
	switch (i->id)
	{
	case MIPS_INS_DSLL32: res = irb.CreateShl(op1, amount); break;
	case MIPS_INS_DSRL32: res = irb.CreateLShr(op1, amount); break;
	case MIPS_INS_DSRA32: res = irb.CreateAShr(op1, amount); break;
	case MIPS_INS_DROTR32: res = generateRotateRight(irb, op1, amount); break;
	default: throw GenericError("Unhandled insn ID in translateDoubleShift32().");
	}

	storeOp(mi->operands[0], res, irb);
}

/**
 * A rotate right by a run-time amount, without the shift-by-the-whole-width
 * that is poison in LLVM.
 *
 * `x >> n | x << (width - n)` is the obvious form and it is wrong for n == 0:
 * the left shift is then by the whole width. `rotr rd, rt, 0` is a legal
 * encoding meaning "no rotation", and translateRotr() produced poison for it.
 * Masking the complement with width - 1 fixes both at once -- for n == 0 it
 * gives 0, so both halves are x and the OR is x -- and costs one `and`.
 */
llvm::Value*
Capstone2LlvmIrTranslatorMips_impl::generateRotateRight(llvm::IRBuilder<>& irb, llvm::Value* val, llvm::Value* amount)
{
	auto* ty = llvm::cast<llvm::IntegerType>(val->getType());
	unsigned width = ty->getBitWidth();

	amount = irb.CreateAnd(amount, llvm::ConstantInt::get(ty, width - 1));
	auto* complement =
		irb.CreateAnd(irb.CreateSub(llvm::ConstantInt::get(ty, width), amount), llvm::ConstantInt::get(ty, width - 1));

	return irb.CreateOr(irb.CreateLShr(val, amount), irb.CreateShl(val, complement));
}

/**
 * MIPS_INS_DEXT, MIPS_INS_DEXTM, MIPS_INS_DEXTU,
 * MIPS_INS_DINS, MIPS_INS_DINSM, MIPS_INS_DINSU
 *
 * The 64-bit bitfield read and write. Six opcodes rather than two, because
 * `pos` and `size` are six bits of encoding between them and cannot cover the
 * whole 0..63 x 1..64 range: DEXTM adds 32 to the size, DEXTU adds 32 to the
 * position, and DINSM and DINSU do the same for the write.
 *
 * Capstone does NOT apply those offsets. It reports the encoded fields with
 * only the `msbd + 1` arithmetic done, so
 *
 *     dextm $2, $3, 0, 33     arrives as    pos 0, size 1
 *     dextu $2, $3, 32, 8     arrives as    pos 0, size 8
 *
 * Taking its numbers at face value reads a one-bit field where a 33-bit one
 * was meant, and reads bit 0 where bit 32 was meant. Neither is a crash and
 * both look like a plausible bitfield, which is why the tests here use the M
 * and U forms with fields whose two readings cannot coincide.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateDoubleBitfield(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, mi, irb);

	if (mi->operands[2].type != MIPS_OP_IMM || mi->operands[3].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	uint64_t pos = static_cast<uint64_t>(mi->operands[2].imm);
	uint64_t size = static_cast<uint64_t>(mi->operands[3].imm);

	switch (i->id)
	{
	case MIPS_INS_DEXTM:
	case MIPS_INS_DINSM: size += 32; break;
	case MIPS_INS_DEXTU:
	case MIPS_INS_DINSU: pos += 32; break;
	default: break;
	}

	if (size == 0 || pos >= 64 || size > 64 - pos)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	op1 = loadOp(mi->operands[1], irb);
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(op1->getType());
	if (ty == nullptr || ty->getBitWidth() != 64)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	uint64_t mask = size >= 64 ? ~0ull : ((1ull << size) - 1);

	bool insert = i->id == MIPS_INS_DINS || i->id == MIPS_INS_DINSM || i->id == MIPS_INS_DINSU;

	if (insert)
	{
		// The destination keeps every bit outside the field.
		llvm::Value* dst = loadOp(mi->operands[0], irb);
		dst = irb.CreateZExtOrTrunc(dst, ty);
		auto* cleared = irb.CreateAnd(dst, llvm::ConstantInt::get(ty, ~(mask << pos)));
		auto* field =
			irb.CreateShl(irb.CreateAnd(op1, llvm::ConstantInt::get(ty, mask)), llvm::ConstantInt::get(ty, pos));
		storeOp(mi->operands[0], irb.CreateOr(cleared, field), irb);
	}
	else
	{
		auto* res =
			irb.CreateAnd(irb.CreateLShr(op1, llvm::ConstantInt::get(ty, pos)), llvm::ConstantInt::get(ty, mask));
		storeOp(mi->operands[0], res, irb);
	}
}

/**
 * MIPS_INS_DSBH, MIPS_INS_DSHD
 *
 * The doubleword byte-order instructions, and neither is a 64-bit byte swap.
 * DSBH swaps the two bytes WITHIN each of the four halfwords; DSHD reverses
 * the order of the four halfwords and leaves the bytes inside them alone.
 * `dsbh` then `dshd` is how the ISA spells a full 64-bit swap, which is the
 * same relationship `wsbh` then `rotr 16` has on the 32-bit side -- and the
 * same reason one llvm.bswap.i64 is the wrong answer for either of them
 * alone.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateDoubleByteSwap(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(op1->getType());
	if (ty == nullptr || ty->getBitWidth() != 64)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto bits = [&](uint64_t v) { return llvm::ConstantInt::get(ty, v); };

	llvm::Value* res = nullptr;
	if (i->id == MIPS_INS_DSBH)
	{
		res = irb.CreateOr(
			irb.CreateShl(irb.CreateAnd(op1, bits(0x00ff00ff00ff00ffull)), bits(8)),
			irb.CreateLShr(irb.CreateAnd(op1, bits(0xff00ff00ff00ff00ull)), bits(8)));
	}
	else
	{
		res = irb.CreateOr(
			irb.CreateOr(
				irb.CreateLShr(op1, bits(48)),
				irb.CreateAnd(irb.CreateLShr(op1, bits(16)), bits(0x00000000ffff0000ull))),
			irb.CreateOr(
				irb.CreateAnd(irb.CreateShl(op1, bits(16)), bits(0x0000ffff00000000ull)),
				irb.CreateShl(op1, bits(48))));
	}

	storeOp(mi->operands[0], res, irb);
}

/**
 * The shift amount a MIPS shift actually uses.
 *
 * `sllv rd, rt, rs` shifts by the low FIVE bits of rs and `dsllv` by the low
 * six; the hardware masks, and nothing here did. A shift by 33 on a 32-bit
 * register is a shift past the operand's width, which is poison in LLVM, so
 * `sllv` with any register value above 31 -- which a compiler emits whenever
 * the amount is computed rather than constant -- produced poison rather than
 * the rotation-free shift-by-1 the machine performs.
 *
 * The emulator cannot see this: getShiftAmount() masks an over-wide shift with
 * exactly the same `& (width - 1)`, so every emulation test passes either way.
 * The assertion is therefore on the IR.
 *
 * Masking by `width - 1` is right for both widths at once, because the width
 * here is the register width and MIPS masks by exactly the bits that index it.
 */
llvm::Value*
Capstone2LlvmIrTranslatorMips_impl::maskShiftAmount(llvm::IRBuilder<>& irb, llvm::Value* val, llvm::Value* amount)
{
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(val->getType());
	if (ty == nullptr || amount->getType() != ty)
	{
		return amount;
	}

	return irb.CreateAnd(amount, llvm::ConstantInt::get(ty, ty->getBitWidth() - 1));
}

/**
 * Is this a 32-bit WORD operation, on a register file that is 64 bits wide?
 *
 * On a 64-bit MIPS the instructions without the D are word operations whose
 * results are sign-extended into the 64-bit register. `addu $2, $3, $4` adds
 * the low halves and sign-extends; `daddu` adds the registers. They are
 * different instructions and both exist, so doing the word one at the register
 * width silently turns it into the other -- which, until the previous commit
 * added them, was the only 64-bit arithmetic this translator had.
 *
 * The rule was already in this file. translateMadd() and translateMsub() both
 * carry
 *
 *     // We operate on 0..31 bits even if on MIPS64.
 *
 * and truncate their operands. It was applied in those two translators and in
 * no other, and Batch F applied it again to EXT, INS and WSBH by restricting
 * them to 32-bit registers. This is the same rule for the rest of them.
 *
 * Not every instruction is on this list, and the ones that are not are not
 * oversights: AND, OR, XOR, NOR and their immediate forms, SLT and SLTU, MOVN
 * and MOVZ genuinely operate on the whole 64-bit register. SEB and SEH
 * sign-extend from bit 7 and bit 15, which gives the same answer at either
 * width. LUI already carries its own note saying it behaves as a 32-bit
 * instruction on MIPS64.
 */
bool Capstone2LlvmIrTranslatorMips_impl::isWordOperation(cs_insn* i)
{
	switch (i->id)
	{
	case MIPS_INS_ADD:
	case MIPS_INS_ADDI:
	case MIPS_INS_ADDIU:
	case MIPS_INS_ADDU:
	case MIPS_INS_ADDU16:
	case MIPS_INS_SUB:
	case MIPS_INS_SUBU:
	case MIPS_INS_SUBU16:
	case MIPS_INS_NEG:
	case MIPS_INS_NEGU:
	case MIPS_INS_SLL:
	case MIPS_INS_SLL16:
	case MIPS_INS_SLLV:
	case MIPS_INS_SRL:
	case MIPS_INS_SRL16:
	case MIPS_INS_SRLV:
	case MIPS_INS_SRA:
	case MIPS_INS_SRAV:
	case MIPS_INS_ROTR:
	case MIPS_INS_ROTRV:
	case MIPS_INS_AUI:
	case MIPS_INS_ALIGN:
	case MIPS_INS_BITSWAP:
	case MIPS_INS_MUL:
	case MIPS_INS_MULU:
	case MIPS_INS_MUH:
	case MIPS_INS_MUHU:
	case MIPS_INS_MULT:
	case MIPS_INS_MULTU:
	case MIPS_INS_DIV:
	case MIPS_INS_DIVU:
	case MIPS_INS_MOD:
	case MIPS_INS_MODU:
	case MIPS_INS_CLZ:
	case MIPS_INS_CLO: return true;
	default: return false;
	}
}

/**
 * The operand of a word instruction, narrowed to the word it operates on.
 *
 * A no-op on 32-bit MIPS and on any instruction that is not a word operation,
 * so it can be applied unconditionally at the top of a translator. The result
 * goes back through storeOp(), whose MIPS default is SEXT_TRUNC_OR_BITCAST --
 * so the sign-extension the architecture requires on the way back into the
 * 64-bit register is already there and needs no code.
 */
llvm::Value* Capstone2LlvmIrTranslatorMips_impl::narrowToWord(cs_insn* i, llvm::IRBuilder<>& irb, llvm::Value* val)
{
	if (val == nullptr || !isWordOperation(i) || !val->getType()->isIntegerTy(64))
	{
		return val;
	}

	return irb.CreateTrunc(val, irb.getInt32Ty());
}

/**
 * A float-to-integer conversion with the MIPS default result for the inputs
 * that do not fit.
 *
 * LLVM's fptosi calls a NaN, an infinity, or an out-of-range magnitude POISON.
 * MIPS signals Invalid Operation, and when that exception is not enabled --
 * which is the state ordinary code runs in -- the default result is
 * 2^(N-1) - 1. For a word destination that is 0x7fffffff.
 *
 * Note what this means and how it differs from its neighbours: EVERY bad input
 * gives the MAXIMUM, including a large NEGATIVE one and including -infinity.
 * ARM saturates toward the nearer end and sends NaN to zero; Power saturates
 * and sends NaN to the minimum; x86 sends everything to the integer indefinite
 * value. Four architectures in this tree, four different rules. Reusing a
 * neighbour's helper would produce a plausible number that is wrong in a way
 * nothing here would notice.
 *
 * NOT MEASURED: this container is x86-64 with no MIPS emulator, so this rests
 * on the MIPS64 manual's stated default result rather than on an observation.
 */
llvm::Value*
Capstone2LlvmIrTranslatorMips_impl::generateFpToIntDefault(llvm::Value* v, llvm::Type* intTy, llvm::IRBuilder<>& irb)
{
	unsigned bits = intTy->getScalarSizeInBits();
	auto* fpTy = v->getType();

	auto* loF = llvm::ConstantFP::get(fpTy, -std::ldexp(1.0, static_cast<int>(bits - 1)));
	auto* hiF = llvm::ConstantFP::get(fpTy, std::ldexp(1.0, static_cast<int>(bits - 1)));

	// Ordered comparisons, so a NaN fails them and takes the default with
	// everything else out of range.
	auto* inRange = irb.CreateAnd(irb.CreateFCmpOGE(v, loF), irb.CreateFCmpOLT(v, hiF));

	// The conversion is fed an in-range value on every path, so the IR carries
	// no poison at all rather than poison that happens not to be selected.
	auto* safe = irb.CreateSelect(inRange, v, llvm::ConstantFP::get(fpTy, 0.0));
	return irb.CreateSelect(
		inRange, irb.CreateFPToSI(safe, intTy), llvm::ConstantInt::get(intTy, llvm::APInt::getSignedMaxValue(bits)));
}

/**
 * MIPS_INS_LUI
 * This behaves like 32-bit MIPS instruction even on 64-bit MIPS.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateLui(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	// The doc comment above is right and the code implemented only half of
	// it. MIPS64 LUI is `GPR[rt] <- sign_extend(immediate || 0^16)`: the
	// 32-bit result is SIGN-extended into the 64-bit register.
	//
	// The CreateZExt here was not a widening at all -- loadOp materialises a
	// MIPS immediate at getDefaultType() already, which is i64 in MIPS64, so
	// the cast was a no-op on equal types and the shift then produced
	// 0x00000000_abcd0000 where the architecture produces 0xffffffff_abcd0000.
	// storeOp's SEXT_TRUNC_OR_BITCAST default could not rescue it either,
	// because by then the value was already the parent's width.
	//
	// Computing at i32 and letting that default do the widening is the same
	// shape the rest of the word family uses. The standard n64 constant idiom
	// `lui $2,0xffff; ori $2,$2,0x1234` was giving 0x00000000ffff1234.
	auto* i32 = irb.getInt32Ty();
	op1 = irb.CreateShl(irb.CreateZExtOrTrunc(op1, i32), llvm::ConstantInt::get(i32, 16));
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_MADD, MIPS_INS_MADDU
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMadd(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (isFpInstructionVariant(i))
	{
		return translateMaddf(i, mi, irb);
	}

	EXPECT_IS_BINARY(i, mi, irb);

	std::tie(op0, op1) = loadOpBinary(mi, irb, eOpConv::NOTHING);
	auto* hi = loadRegister(MIPS_REG_HI, irb);
	auto* lo = loadRegister(MIPS_REG_LO, irb);

	auto* i32Ty = irb.getInt32Ty();
	auto* i64Ty = irb.getInt64Ty();

	// We operate on 0..31 bits even if on MIPS64.
	//
	if (op0->getType() == i64Ty)
	{
		op0 = irb.CreateTrunc(op0, i32Ty);
	}
	if (op1->getType() == i64Ty)
	{
		op1 = irb.CreateTrunc(op1, i32Ty);
	}
	if (hi->getType() == i64Ty)
	{
		hi = irb.CreateTrunc(hi, i32Ty);
	}
	if (lo->getType() == i64Ty)
	{
		lo = irb.CreateTrunc(lo, i32Ty);
	}

	if (i->id == MIPS_INS_MADD)
	{
		op0 = irb.CreateSExtOrTrunc(op0, i64Ty);
		op1 = irb.CreateSExtOrTrunc(op1, i64Ty);
	}
	else if (i->id == MIPS_INS_MADDU)
	{
		op0 = irb.CreateZExtOrTrunc(op0, i64Ty);
		op1 = irb.CreateZExtOrTrunc(op1, i64Ty);
	}
	else
	{
		throw GenericError("translateMadd(): unhandled insn ID");
	}

	hi = irb.CreateZExt(hi, i64Ty);
	hi = irb.CreateShl(hi, 32);
	lo = irb.CreateZExt(lo, i64Ty);
	auto* hilo = irb.CreateOr(hi, lo);

	auto* mul = irb.CreateMul(op0, op1);
	auto* add = irb.CreateAdd(hilo, mul);

	lo = irb.CreateTrunc(add, i32Ty);
	storeRegister(MIPS_REG_LO, lo, irb);

	hi = irb.CreateLShr(add, 32);
	hi = irb.CreateTrunc(hi, i32Ty);
	storeRegister(MIPS_REG_HI, hi, irb);
}

void Capstone2LlvmIrTranslatorMips_impl::translateMaddf(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count == 3)
	{
		// R6 maddf.s fd, fs, ft  →  fd = fd + fs * ft
		op0 = loadOp(mi->operands[0], irb);
		op1 = loadOp(mi->operands[1], irb);
		op2 = loadOp(mi->operands[2], irb);
		storeOp(mi->operands[0], irb.CreateFAdd(op0, irb.CreateFMul(op1, op2)), irb);
		return;
	}

	EXPECT_IS_QUATERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	op3 = loadOp(mi->operands[3], irb);

	auto* mul = irb.CreateFMul(op2, op3);
	auto* add = irb.CreateFAdd(mul, op1);
	storeOp(mi->operands[0], add, irb);
}

/**
 * MIPS_INS_NEG, MIPS_INS_NEG_S, MIPS_INS_NEG_D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateNeg(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (i->id == MIPS_INS_NEG_S || i->id == MIPS_INS_NEG_D || isFpInstructionVariant(i))
	{
		EXPECT_IS_BINARY(i, mi, irb);
		op1 = loadOpBinaryOp1(mi, irb);
		storeOp(mi->operands[0], irb.CreateFNeg(op1), irb);
		return;
	}

	if (mi->op_count == 2
			&& isOperandRegister(mi->operands[0])
			&& isGeneralPurposeRegister(mi->operands[0].reg)
			&& isOperandRegister(mi->operands[1])
			&& isGeneralPurposeRegister(mi->operands[1].reg))
	{
		translateNegu(i, mi, irb);
	}
	else
	{
		translatePseudoAsmOp0FncOp1(i, mi, irb);
	}
}

/**
 * MIPS_INS_NEGU
 */
void Capstone2LlvmIrTranslatorMips_impl::translateNegu(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	op1 = narrowToWord(i, irb, op1);
	auto* sub = irb.CreateSub(llvm::ConstantInt::get(op1->getType(), 0), op1);
	storeOp(mi->operands[0], sub, irb);
}

/**
 * MIPS_INS_NMADD -- this could be merged with translateMaddf().
 */
void Capstone2LlvmIrTranslatorMips_impl::translateNmadd(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	op3 = loadOp(mi->operands[3], irb);

	auto* mul = irb.CreateFMul(op2, op3);
	auto* add = irb.CreateFAdd(mul, op1);
	// Neg function call could be used here instead.
	auto* neg = irb.CreateFSub(llvm::ConstantFP::get(add->getType(), 0.0), add);
	storeOp(mi->operands[0], neg, irb);
}

/**
 * MIPS_INS_MAX, MIPS_INS_MAX_S, MIPS_INS_MAX_D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMax(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::THROW);
	if (op1->getType()->isFloatingPointTy())
	{
		storeOp(mi->operands[0], irb.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, op1, op2), irb);
		return;
	}
	auto* sge = irb.CreateICmpSGE(op1, op2);
	auto* val = irb.CreateSelect(sge, op1, op2);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_MFC1
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMfc1(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	auto* ty = op1->getType()->isDoubleTy()
			? irb.getInt64Ty()
			: irb.getInt32Ty();
	op1 = irb.CreateBitCast(op1, ty);
	op1 = irb.CreateZExtOrTrunc(op1, irb.getInt32Ty()); // even on 64-bit it takes only 0..31
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_MTC1
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMtc1(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op0 = loadOpBinaryOp0(mi, irb);
	op0 = irb.CreateZExtOrTrunc(op0, irb.getInt32Ty()); // even on 64-bit it takes only 0..31
	op0 = irb.CreateBitCast(op0, irb.getFloatTy());
	storeOp(mi->operands[1], op0, irb);
}

/**
 * MIPS_INS_MTHC1, MIPS_INS_MFHC1
 *
 * mtc1 and mfc1 move the LOW half of a 64-bit FPU register; these move the
 * high half, and a compiler emits them in pairs to get a double in and out of
 * the FPU without going through memory. Without them the pair was half a move
 * and an opaque call -- MTHC1 was the only instruction COV-01 found
 * untranslated in MIPS floating-point programs.
 *
 * The double lives in the FDn register that singlePrecisionToDoublePrecisionFpRegister
 * maps $fN onto, so this reads that register's bits, replaces one half, and
 * writes it back.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMthc1(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();

	if (mi->operands[1].type != MIPS_OP_REG)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	uint32_t fd = singlePrecisionToDoublePrecisionFpRegister(mi->operands[1].reg);

	if (i->id == MIPS_INS_MFHC1)
	{
		auto* bits = irb.CreateBitCast(loadRegister(fd, irb), i64);
		auto* hi = irb.CreateTrunc(irb.CreateLShr(bits, llvm::ConstantInt::get(i64, 32)), i32);
		storeOp(mi->operands[0], hi, irb);
		return;
	}

	op0 = loadOpBinaryOp0(mi, irb);
	op0 = irb.CreateZExtOrTrunc(op0, i32);

	auto* bits = irb.CreateBitCast(loadRegister(fd, irb), i64);
	auto* lo = irb.CreateAnd(bits, llvm::ConstantInt::get(i64, 0xffffffffULL));
	auto* hi = irb.CreateShl(irb.CreateZExt(op0, i64), llvm::ConstantInt::get(i64, 32));
	storeRegister(fd, irb.CreateBitCast(irb.CreateOr(lo, hi), irb.getDoubleTy()), irb);
}

/**
 * MIPS_INS_MFHI
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMfhi(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	auto* hi = loadRegister(MIPS_REG_HI, irb);
	storeOp(mi->operands[0], hi, irb);
}

/**
 * MIPS_INS_MFLO
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMflo(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	auto* lo = loadRegister(MIPS_REG_LO, irb);
	storeOp(mi->operands[0], lo, irb);
}

/**
 * MIPS_INS_MIN, MIPS_INS_MIN_S, MIPS_INS_MIN_D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMin(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::THROW);
	if (op1->getType()->isFloatingPointTy())
	{
		storeOp(mi->operands[0], irb.CreateBinaryIntrinsic(llvm::Intrinsic::minnum, op1, op2), irb);
		return;
	}
	auto* sle = irb.CreateICmpSLE(op1, op2);
	auto* val = irb.CreateSelect(sle, op1, op2);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_MOV, MIPS_INS_MOVE
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMov(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_MSUB, MIPS_INS_MSUBU
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsub(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (isFpInstructionVariant(i))
	{
		return translateMsubf(i, mi, irb);
	}

	EXPECT_IS_BINARY(i, mi, irb);

	std::tie(op0, op1) = loadOpBinary(mi, irb, eOpConv::NOTHING);
	auto* hi = loadRegister(MIPS_REG_HI, irb);
	auto* lo = loadRegister(MIPS_REG_LO, irb);

	auto* i32Ty = irb.getInt32Ty();
	auto* i64Ty = irb.getInt64Ty();

	// We operate on 0..31 bits even if on MIPS64.
	//
	if (op0->getType() == i64Ty)
	{
		op0 = irb.CreateTrunc(op0, i32Ty);
	}
	if (op1->getType() == i64Ty)
	{
		op1 = irb.CreateTrunc(op1, i32Ty);
	}
	if (hi->getType() == i64Ty)
	{
		hi = irb.CreateTrunc(hi, i32Ty);
	}
	if (lo->getType() == i64Ty)
	{
		lo = irb.CreateTrunc(lo, i32Ty);
	}

	if (i->id == MIPS_INS_MSUB)
	{
		op0 = irb.CreateSExtOrTrunc(op0, i64Ty);
		op1 = irb.CreateSExtOrTrunc(op1, i64Ty);
	}
	else if (i->id == MIPS_INS_MSUBU)
	{
		op0 = irb.CreateZExtOrTrunc(op0, i64Ty);
		op1 = irb.CreateZExtOrTrunc(op1, i64Ty);
	}
	else
	{
		throw GenericError("translateMsub(): unhandled insn ID");
	}

	hi = irb.CreateZExt(hi, i64Ty);
	hi = irb.CreateShl(hi, 32);
	lo = irb.CreateZExt(lo, i64Ty);
	auto* hilo = irb.CreateOr(hi, lo);

	auto* mul = irb.CreateMul(op0, op1);
	auto* sub = irb.CreateSub(hilo, mul);

	lo = irb.CreateTrunc(sub, i32Ty);
	storeRegister(MIPS_REG_LO, lo, irb);

	hi = irb.CreateLShr(sub, 32);
	hi = irb.CreateTrunc(hi, i32Ty);
	storeRegister(MIPS_REG_HI, hi, irb);
}

void Capstone2LlvmIrTranslatorMips_impl::translateMsubf(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count == 3)
	{
		// R6 msubf.s fd, fs, ft  →  fd = fd - fs * ft
		op0 = loadOp(mi->operands[0], irb);
		op1 = loadOp(mi->operands[1], irb);
		op2 = loadOp(mi->operands[2], irb);
		storeOp(mi->operands[0], irb.CreateFSub(op0, irb.CreateFMul(op1, op2)), irb);
		return;
	}

	EXPECT_IS_QUATERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	op3 = loadOp(mi->operands[3], irb);

	auto* mul = irb.CreateFMul(op2, op3);
	auto* sub = irb.CreateFSub(mul, op1);
	storeOp(mi->operands[0], sub, irb);
}

/**
 * MIPS_INS_NMSUB
 */
void Capstone2LlvmIrTranslatorMips_impl::translateNmsub(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	op3 = loadOp(mi->operands[3], irb);

	auto* mul = irb.CreateFMul(op2, op3);
	auto* sub = irb.CreateFSub(mul, op1);
	// Neg function call could be used here instead.
	auto* neg = irb.CreateFSub(llvm::ConstantFP::get(sub->getType(), 0.0), sub);
	storeOp(mi->operands[0], neg, irb);
}

/**
 * MIPS_INS_MTHI
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMthi(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	op0 = loadOpUnary(mi, irb);
	storeRegister(MIPS_REG_HI, op0, irb);
}

/**
 * MIPS_INS_MTLO
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMtlo(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	op0 = loadOpUnary(mi, irb);
	storeRegister(MIPS_REG_LO, op0, irb);
}

/**
 * MIPS_INS_MOVF
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMovf(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(mi, irb);
	auto* c = irb.CreateICmpEQ(op2, llvm::ConstantInt::get(op2->getType(), 0));
	auto* val = irb.CreateSelect(c, op1, op0);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_MOVN
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMovn(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(mi, irb);
	auto* e = irb.CreateICmpNE(op2, llvm::ConstantInt::get(op2->getType(), 0));
	auto* val = irb.CreateSelect(e, op1, op0);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_MOVT
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMovt(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(mi, irb);
	auto* c = irb.CreateICmpNE(op2, llvm::ConstantInt::get(op2->getType(), 0));
	auto* val = irb.CreateSelect(c, op1, op0);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_MOVZ
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMovz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(mi, irb);
	auto* e = irb.CreateICmpEQ(op2, llvm::ConstantInt::get(op2->getType(), 0));
	auto* val = irb.CreateSelect(e, op1, op0);
	storeOp(mi->operands[0], val, irb);
}

/**
 * MIPS_INS_MUL, MIPS_INS_MULU, MIPS_INS_MUH, MIPS_INS_MUHU,
 * MIPS_INS_DMUL, MIPS_INS_DMULU, MIPS_INS_DMUH, MIPS_INS_DMUHU
 *
 * Pre-R6 `mul` writes the low half of a word product to a GPR and leaves
 * HI/LO UNPREDICTABLE. R6 keeps that low-half form and adds `muh` (high
 * half) plus the unsigned pair; there is no HI/LO file in R6.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMul(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::THROW);
	if (op1->getType()->isFloatingPointTy())
	{
		auto* mul = irb.CreateFMul(op1, op2);
		storeOp(mi->operands[0], mul, irb);
		return;
	}

	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);

	bool high = i->id == MIPS_INS_MUH || i->id == MIPS_INS_MUHU
			|| i->id == MIPS_INS_DMUH || i->id == MIPS_INS_DMUHU;
	bool isUnsigned = i->id == MIPS_INS_MULU || i->id == MIPS_INS_MUHU
			|| i->id == MIPS_INS_DMULU || i->id == MIPS_INS_DMUHU;

	unsigned half = op1->getType()->getIntegerBitWidth();
	auto* wide = irb.getIntNTy(half * 2);
	if (isUnsigned)
	{
		op1 = irb.CreateZExt(op1, wide);
		op2 = irb.CreateZExt(op2, wide);
	}
	else
	{
		op1 = irb.CreateSExt(op1, wide);
		op2 = irb.CreateSExt(op2, wide);
	}

	auto* mul = irb.CreateMul(op1, op2);
	auto* halfTy = irb.getIntNTy(half);
	llvm::Value* res = high
			? irb.CreateTrunc(irb.CreateLShr(mul, half), halfTy)
			: irb.CreateTrunc(mul, halfTy);
	storeOp(mi->operands[0], res, irb);
	storeRegisterUnpredictable(MIPS_REG_HI, irb);
	storeRegisterUnpredictable(MIPS_REG_LO, irb);
}

/**
 * MIPS_INS_MULT, MIPS_INS_MULTU
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMult(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	std::tie(op0, op1) = loadOpBinary(mi, irb, eOpConv::THROW);

	// `mult` is a word multiply even on MIPS64: 32 x 32 into a 64-bit
	// product, whose two halves go into LO and HI SIGN-EXTENDED to the
	// register width. `dmult` is the doubleword one, 64 x 64 into 128.
	// Doing the word form at the register width makes it the doubleword
	// form, which is the instruction next to it in the table.
	op0 = narrowToWord(i, irb, op0);
	op1 = narrowToWord(i, irb, op1);

	unsigned half = op0->getType()->getIntegerBitWidth();
	auto* ty = irb.getIntNTy(half * 2);
	if (i->id == MIPS_INS_MULT || i->id == MIPS_INS_DMULT)
	{
		op0 = irb.CreateSExt(op0, ty);
		op1 = irb.CreateSExt(op1, ty);
	}
	else if (i->id == MIPS_INS_MULTU || i->id == MIPS_INS_DMULTU)
	{
		op0 = irb.CreateZExt(op0, ty);
		op1 = irb.CreateZExt(op1, ty);
	}
	else
	{
		throw GenericError("unhandled insn ID");
	}
	auto* mul = irb.CreateMul(op0, op1);
	auto* halfTy = irb.getIntNTy(half);
	// storeRegister's MIPS default is SEXT_TRUNC_OR_BITCAST, so each half
	// is sign-extended into a wider LO or HI, which is what the
	// architecture requires of the word form.
	auto* low = irb.CreateTrunc(mul, halfTy);
	storeRegister(MIPS_REG_LO, low, irb);
	auto* shift = irb.CreateLShr(mul, half);
	auto* high = irb.CreateTrunc(shift, halfTy);
	storeRegister(MIPS_REG_HI, high, irb);
}

/**
 * MIPS_INS_SQRT_S, MIPS_INS_SQRT_D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSqrt(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sqrt, op1->getType());
	storeOp(mi->operands[0], irb.CreateCall(f, {op1}), irb);
}

/**
 * MIPS_INS_RECIP_S, MIPS_INS_RECIP_D, MIPS_INS_RSQRT_S, MIPS_INS_RSQRT_D
 */
void Capstone2LlvmIrTranslatorMips_impl::translateRecip(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	if (i->id == MIPS_INS_RSQRT_S || i->id == MIPS_INS_RSQRT_D)
	{
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sqrt, op1->getType());
		op1 = irb.CreateCall(f, {op1});
	}
	auto* one = llvm::ConstantFP::get(op1->getType(), 1.0);
	storeOp(mi->operands[0], irb.CreateFDiv(one, op1), irb);
}

/**
 * MIPS_INS_NOP
 */
void Capstone2LlvmIrTranslatorMips_impl::translateNop(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	// Nothing.
}

/**
 * MIPS_INS_NOR, MIPS_INS_NORI
 */
void Capstone2LlvmIrTranslatorMips_impl::translateNor(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* o = irb.CreateOr(op1, op2);
	auto* x = irb.CreateXor(o, llvm::ConstantInt::getSigned(o->getType(), -1));
	storeOp(mi->operands[0], x, irb);
}

/**
 * MIPS_INS_NOT, MIPS_INS_NOT16
 */
void Capstone2LlvmIrTranslatorMips_impl::translateNot(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	op1 = irb.CreateNot(op1);
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_OR, MIPS_INS_ORI, MIPS_INS_OR16
 */
void Capstone2LlvmIrTranslatorMips_impl::translateOr(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* o = irb.CreateOr(op1, op2);
	storeOp(mi->operands[0], o, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ROTR, MIPS_INS_ROTRV
 */
void Capstone2LlvmIrTranslatorMips_impl::translateRotr(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);
	storeOp(mi->operands[0], generateRotateRight(irb, op1, op2), irb);
}

/**
 * MIPS_INS_SEB
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSeb(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	auto* ty = llvm::cast<llvm::IntegerType>(op1->getType());
	std::size_t shiftN = ty->getBitWidth() - 8;
	auto* shiftCi = llvm::ConstantInt::get(op1->getType(), shiftN);
	op1 = irb.CreateShl(op1, shiftCi);
	op1 = irb.CreateAShr(op1, shiftCi);
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_SEH
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSeh(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	auto* ty = llvm::cast<llvm::IntegerType>(op1->getType());
	std::size_t shiftN = ty->getBitWidth() - 16;
	auto* shiftCi = llvm::ConstantInt::get(op1->getType(), shiftN);
	op1 = irb.CreateShl(op1, shiftCi);
	op1 = irb.CreateAShr(op1, shiftCi);
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_SLL, MIPS_INS_SLLI, MIPS_INS_SLLV, MIPS_INS_SLL16
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSll(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);
	op2 = maskShiftAmount(irb, op1, op2);
	auto* shl = irb.CreateShl(op1, op2);
	storeOp(mi->operands[0], shl, irb);
}

/**
 * MIPS_INS_SLT, MIPS_INS_SLTI
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSlt(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* slt = irb.CreateICmpSLT(op1, op2);
	slt = irb.CreateZExt(slt, getDefaultType());
	storeOp(mi->operands[0], slt, irb);
}

/**
 * MIPS_INS_SLTU, MIPS_INS_SLTIU
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSltu(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ult = irb.CreateICmpULT(op1, op2);
	ult = irb.CreateZExt(ult, getDefaultType());
	storeOp(mi->operands[0], ult, irb);
}

/**
 * MIPS_INS_SRA, MIPS_INS_SRAI, MIPS_INS_SRAV
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSra(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);
	op2 = maskShiftAmount(irb, op1, op2);
	auto* sra = irb.CreateAShr(op1, op2);
	storeOp(mi->operands[0], sra, irb);
}

/**
 * MIPS_INS_SRL, MIPS_INS_SRLI, MIPS_INS_SRLV, MIPS_INS_SRL16
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSrl(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);
	op2 = maskShiftAmount(irb, op1, op2);
	auto* shr = irb.CreateLShr(op1, op2);
	storeOp(mi->operands[0], shr, irb);
}

/**
 * MIPS_INS_SUB, MIPS_INS_SUBU, MIPS_INS_SUBU16
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSub(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST, eOpConv::FPCAST_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);
	auto* sub = op1->getType()->isFloatingPointTy()
			? irb.CreateFSub(op1, op2)
			: irb.CreateSub(op1, op2);
	storeOp(mi->operands[0], sub, irb);
}

/**
 * MIPS_INS_SYSCALL
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSyscall(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, mi, irb);

	if (mi->op_count == 0)
	{
		op0 = llvm::ConstantInt::get(getDefaultType(), 0);
	}
	else if (mi->op_count == 1)
	{
		op0 = loadOpUnary(mi, irb);
	}

	op0 = irb.CreateZExtOrTrunc(op0, getDefaultType());

	llvm::Function* fnc = getPseudoAsmFunction(
			i,
			irb.getVoidTy(),
			llvm::ArrayRef<llvm::Type*>{op0->getType()});

	irb.CreateCall(fnc, {op0});
}

/**
 * MIPS_INS_XOR, MIPS_INS_XORI, MIPS_INS_XOR16
 */
void Capstone2LlvmIrTranslatorMips_impl::translateXor(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* x = irb.CreateXor(op1, op2);
	storeOp(mi->operands[0], x, irb);
}

/**
 * MIPS_INS_SNE, MIPS_INS_SNEI
 * op0 = (op1 != op2) ? 1 : 0
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSne(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ne = irb.CreateICmpNE(op1, op2);
	storeOp(mi->operands[0], ne, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SEQ, MIPS_INS_SEQI
 * op0 = (op1 != op2) ? 1 : 0
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSeq(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ne = irb.CreateICmpEQ(op1, op2);
	storeOp(mi->operands[0], ne, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

} // namespace capstone2llvmir
} // namespace retdec
