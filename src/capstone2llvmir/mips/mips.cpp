/**
 * @file src/capstone2llvmir/mips/mips.cpp
 * @brief MIPS implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <vector>

#include <llvm/IR/Constants.h>
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

bool Capstone2LlvmIrTranslatorMips_impl::enableMipsR2ForDisasm()
{
	// CS_MODE_MIPS32 / CS_MODE_MIPS64 do not set FeatureMips32r2 or
	// FeatureMips64r2, so synci, ext, ins, wsbh, rotr, rdhwr, m*hc1, dext
	// and drotr fail to decode. OR the revision in only for a retry: leaving
	// it on also sets FeatureFP64Bit and steals COP1 from the MIPS32 table.
	cs_mode add = CS_MODE_LITTLE_ENDIAN;
	if (_basicMode == CS_MODE_MIPS32 || _basicMode == CS_MODE_MIPS3)
	{
		add = CS_MODE_MIPS32R2;
	}
	else if (_basicMode == CS_MODE_MIPS64)
	{
		add = CS_MODE_MIPS64R2;
	}
	else
	{
		return false;
	}

	return cs_option(_handle, CS_OPT_MODE, _basicMode + _extraMode + add) == CS_ERR_OK;
}

void Capstone2LlvmIrTranslatorMips_impl::restoreDisasmMode()
{
	cs_option(_handle, CS_OPT_MODE, _basicMode + _extraMode);
}

typename Capstone2LlvmIrTranslatorMips_impl::TranslationResult Capstone2LlvmIrTranslatorMips_impl::translate(
	const uint8_t* bytes,
	std::size_t size,
	retdec::common::Address a,
	llvm::IRBuilder<>& irb,
	std::size_t count,
	bool stopOnBranch)
{
	using Base = Capstone2LlvmIrTranslator_impl<cs_mips, cs_mips_op>;

	TranslationResult res;
	const uint8_t* p = bytes;
	std::size_t left = size;
	retdec::common::Address addr = a;

	while (left != 0 && (count == 0 || res.count < count))
	{
		std::size_t want = count == 0 ? 0 : count - res.count;
		TranslationResult part = Base::translate(p, left, addr, irb, want, stopOnBranch);
		if (part.count == 0)
		{
			// One R2 insn, then back to the base mode so the next COP1
			// still decodes from the non-FP64 table.
			if (!enableMipsR2ForDisasm())
			{
				break;
			}
			try
			{
				part = Base::translate(p, left, addr, irb, 1, stopOnBranch);
			}
			catch (...)
			{
				restoreDisasmMode();
				throw;
			}
			restoreDisasmMode();
		}
		if (part.count == 0)
		{
			break;
		}

		res.insns.splice(res.insns.end(), part.insns);
		res.count += part.count;
		res.size += part.size;
		if (part.size == 0 || part.size > left)
		{
			break;
		}
		p += part.size;
		left -= part.size;
		addr = retdec::common::Address(static_cast<uint64_t>(addr) + part.size);
		if (stopOnBranch && part.branchCall != nullptr)
		{
			res.branchCall = part.branchCall;
			res.inCondition = part.inCondition;
			break;
		}
	}

	return res;
}

typename Capstone2LlvmIrTranslatorMips_impl::TranslationResultOne Capstone2LlvmIrTranslatorMips_impl::translateOne(
	const uint8_t*& bytes, std::size_t& size, retdec::common::Address& a, llvm::IRBuilder<>& irb)
{
	using Base = Capstone2LlvmIrTranslator_impl<cs_mips, cs_mips_op>;

	TranslationResultOne res = Base::translateOne(bytes, size, a, irb);
	if (!res.failed())
	{
		return res;
	}
	if (!enableMipsR2ForDisasm())
	{
		return res;
	}
	try
	{
		res = Base::translateOne(bytes, size, a, irb);
	}
	catch (...)
	{
		restoreDisasmMode();
		throw;
	}
	restoreDisasmMode();
	return res;
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
			|| m == CS_MODE_MICRO
			|| m == CS_MODE_MIPS64R6
			|| m == CS_MODE_OCTEON
			|| m == CS_MODE_OCTEONP
			|| m == (CS_MODE_OCTEON | CS_MODE_OCTEONP)
			|| m == CS_MODE_MIPS16;
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
			// R6 BC1EQZ/BC1NEZ/BC2EQZ/BC2NEZ keep a delay slot (their
			// ...C forms on microMIPS R6 do not).
			MIPS_INS_BC1EQZ, MIPS_INS_BC1NEZ, MIPS_INS_BC2EQZ, MIPS_INS_BC2NEZ,
			MIPS_INS_ALIAS_BEQZ, MIPS_INS_ALIAS_BNEZ,
			// microMIPS 16-bit branches/jumps still have a delay slot.
			MIPS_INS_B16, MIPS_INS_BEQZ16, MIPS_INS_BNEZ16,
			MIPS_INS_JR16, MIPS_INS_JALRS16, MIPS_INS_JALRS,
			MIPS_INS_JRADDIUSP,
			// call
			MIPS_INS_BGEZAL, MIPS_INS_BGEZALL, MIPS_INS_BLTZAL,
			MIPS_INS_BLTZALL, MIPS_INS_JAL, MIPS_INS_JALR,
			MIPS_INS_BAL, MIPS_INS_ALIAS_BAL,
			MIPS_INS_BGEZALS, MIPS_INS_BLTZALS, MIPS_INS_JALS, MIPS_INS_JALX,
			MIPS_INS_BPOSGE32,
			MIPS_INS_BBIT0, MIPS_INS_BBIT032, MIPS_INS_BBIT1, MIPS_INS_BBIT132,
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
 * CEQ/CEQI/CLE_S/U/CLT_S/U, MAX_S/MIN_S, SLL/SRL lanes, COPY_S/COPY_U,
 * INSERT/FILL/LDI, ADDS_S/U / SUBS_S/U, SAT_S/U, SLD/SLDI, ADDVI/SUBVI,
 * BCLR/BSET/BNEG, BINSL/BINSR, BMNZ/BMZ, ADD_A/ASUB/AVE/AVER,
 * FADD_W/D / FSUB_W/D / FMUL_W/D / FDIV_W/D / FMAX_W/D / FMIN_W/D,
 * LD/ST of the whole vector) is i128; every other MSA id still goes to a
 * pseudo-assembly call.
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
	// CEQ/CEQI/CLE_S/U/CLT_S/U, COPY_S/U, INSERT/FILL/LDI, ADDS_S/U / SUBS_S/U,
	// SAT_S/U, SLD/SLDI, ADDVI/SUBVI, BCLR/BSET/BNEG, BINSL/BINSR, BMNZ/BMZ,
	// ADD_A/ASUB/AVE/AVER, ADDS_A, FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FMAX_A/FMIN_A,
	// FCEQ/FCLT/…, DOTP/DPADD/DPSUB, HADD/HSUB, DIV/MOD, MOVE_V, BNZ/BZ,
	// MUL_Q/MADD_Q/MSUB_Q, LD_*/ST_* whole vector) is handled below;
	// everything else with a W register still goes to a pseudo-assembly
	// call rather than a scalar translator.
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
		case MIPS_REG_AC0_64: return MIPS_REG_AC0;
		default: return r;
	}
}

static bool isCanonicalFpReg(uint32_t r)
{
	r = canonicalizeMipsReg(r);
	return MIPS_REG_F0 <= r && r <= MIPS_REG_F31;
}

/// IEEE-754 class mask used by R6 CLASS.fmt and MSA FCLASS.
/// Bit 0 SNAN, 1 QNAN, 2 −Inf, 3 −norm, 4 −sub, 5 −0,
/// 6 +0, 7 +sub, 8 +norm, 9 +Inf.
static llvm::Value* fpClassBits(llvm::Value* b, unsigned e, llvm::IRBuilder<>& irb)
{
	unsigned man = e == 64 ? 52 : 23;
	uint64_t expMask = e == 64 ? 0x7ffull : 0xffull;
	uint64_t manMask = e == 64 ? 0xfffffffffffffull : 0x7fffffull;
	uint64_t qnanBit = 1ull << (man - 1);
	auto* iTy = irb.getIntNTy(e);
	auto* sign = irb.CreateICmpSLT(b, llvm::ConstantInt::get(iTy, 0));
	auto* exp = irb.CreateAnd(irb.CreateLShr(b, man), llvm::ConstantInt::get(iTy, expMask));
	auto* frac = irb.CreateAnd(b, llvm::ConstantInt::get(iTy, manMask));
	auto* expMax = irb.CreateICmpEQ(exp, llvm::ConstantInt::get(iTy, expMask));
	auto* expZ = irb.CreateICmpEQ(exp, llvm::ConstantInt::get(iTy, 0));
	auto* fracZ = irb.CreateICmpEQ(frac, llvm::ConstantInt::get(iTy, 0));
	auto* fracNZ = irb.CreateNot(fracZ);
	auto* isNan = irb.CreateAnd(expMax, fracNZ);
	auto* isQ = irb.CreateICmpNE(
			irb.CreateAnd(frac, llvm::ConstantInt::get(iTy, qnanBit)),
			llvm::ConstantInt::get(iTy, 0));
	auto* isSnan = irb.CreateAnd(isNan, irb.CreateNot(isQ));
	auto* isQnan = irb.CreateAnd(isNan, isQ);
	auto* isInf = irb.CreateAnd(expMax, fracZ);
	auto* isZero = irb.CreateAnd(expZ, fracZ);
	auto* isSub = irb.CreateAnd(expZ, fracNZ);
	auto* isNorm = irb.CreateAnd(irb.CreateNot(expZ), irb.CreateNot(expMax));
	auto* pos = irb.CreateNot(sign);
	auto bit = [&](llvm::Value* cond, uint64_t v) {
		return irb.CreateSelect(cond, llvm::ConstantInt::get(iTy, v), llvm::ConstantInt::get(iTy, 0));
	};
	auto* m = bit(isSnan, 0x001);
	m = irb.CreateOr(m, bit(isQnan, 0x002));
	m = irb.CreateOr(m, bit(irb.CreateAnd(sign, isInf), 0x004));
	m = irb.CreateOr(m, bit(irb.CreateAnd(sign, isNorm), 0x008));
	m = irb.CreateOr(m, bit(irb.CreateAnd(sign, isSub), 0x010));
	m = irb.CreateOr(m, bit(irb.CreateAnd(sign, isZero), 0x020));
	m = irb.CreateOr(m, bit(irb.CreateAnd(pos, isZero), 0x040));
	m = irb.CreateOr(m, bit(irb.CreateAnd(pos, isSub), 0x080));
	m = irb.CreateOr(m, bit(irb.CreateAnd(pos, isNorm), 0x100));
	m = irb.CreateOr(m, bit(irb.CreateAnd(pos, isInf), 0x200));
	return m;
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
		case MIPS_INS_ADDVI_B:
		case MIPS_INS_ADDVI_H:
		case MIPS_INS_ADDVI_W:
		case MIPS_INS_ADDVI_D:
		case MIPS_INS_ADDV_B:
		case MIPS_INS_ADDV_H:
		case MIPS_INS_ADDV_W:
		case MIPS_INS_ADDV_D:
		case MIPS_INS_ADD_A_B:
		case MIPS_INS_ADD_A_H:
		case MIPS_INS_ADD_A_W:
		case MIPS_INS_ADD_A_D:
		case MIPS_INS_ASUB_S_B:
		case MIPS_INS_ASUB_S_H:
		case MIPS_INS_ASUB_S_W:
		case MIPS_INS_ASUB_S_D:
		case MIPS_INS_ASUB_U_B:
		case MIPS_INS_ASUB_U_H:
		case MIPS_INS_ASUB_U_W:
		case MIPS_INS_ASUB_U_D:
		case MIPS_INS_AVE_S_B:
		case MIPS_INS_AVE_S_H:
		case MIPS_INS_AVE_S_W:
		case MIPS_INS_AVE_S_D:
		case MIPS_INS_AVE_U_B:
		case MIPS_INS_AVE_U_H:
		case MIPS_INS_AVE_U_W:
		case MIPS_INS_AVE_U_D:
		case MIPS_INS_AVER_S_B:
		case MIPS_INS_AVER_S_H:
		case MIPS_INS_AVER_S_W:
		case MIPS_INS_AVER_S_D:
		case MIPS_INS_AVER_U_B:
		case MIPS_INS_AVER_U_H:
		case MIPS_INS_AVER_U_W:
		case MIPS_INS_AVER_U_D:
		case MIPS_INS_BCLR_B:
		case MIPS_INS_BCLR_H:
		case MIPS_INS_BCLR_W:
		case MIPS_INS_BCLR_D:
		case MIPS_INS_BCLRI_B:
		case MIPS_INS_BCLRI_H:
		case MIPS_INS_BCLRI_W:
		case MIPS_INS_BCLRI_D:
		case MIPS_INS_BSET_B:
		case MIPS_INS_BSET_H:
		case MIPS_INS_BSET_W:
		case MIPS_INS_BSET_D:
		case MIPS_INS_BSETI_B:
		case MIPS_INS_BSETI_H:
		case MIPS_INS_BSETI_W:
		case MIPS_INS_BSETI_D:
		case MIPS_INS_BNEG_B:
		case MIPS_INS_BNEG_H:
		case MIPS_INS_BNEG_W:
		case MIPS_INS_BNEG_D:
		case MIPS_INS_BNEGI_B:
		case MIPS_INS_BNEGI_H:
		case MIPS_INS_BNEGI_W:
		case MIPS_INS_BNEGI_D:
		case MIPS_INS_BINSL_B:
		case MIPS_INS_BINSL_H:
		case MIPS_INS_BINSL_W:
		case MIPS_INS_BINSL_D:
		case MIPS_INS_BINSLI_B:
		case MIPS_INS_BINSLI_H:
		case MIPS_INS_BINSLI_W:
		case MIPS_INS_BINSLI_D:
		case MIPS_INS_BINSR_B:
		case MIPS_INS_BINSR_H:
		case MIPS_INS_BINSR_W:
		case MIPS_INS_BINSR_D:
		case MIPS_INS_BINSRI_B:
		case MIPS_INS_BINSRI_H:
		case MIPS_INS_BINSRI_W:
		case MIPS_INS_BINSRI_D:
		case MIPS_INS_BMNZ_V:
		case MIPS_INS_BMNZI_B:
		case MIPS_INS_BMZ_V:
		case MIPS_INS_BMZI_B:
		case MIPS_INS_ADDS_S_B:
		case MIPS_INS_ADDS_S_H:
		case MIPS_INS_ADDS_S_W:
		case MIPS_INS_ADDS_S_D:
		case MIPS_INS_ADDS_U_B:
		case MIPS_INS_ADDS_U_H:
		case MIPS_INS_ADDS_U_W:
		case MIPS_INS_ADDS_U_D:
		case MIPS_INS_SUBVI_B:
		case MIPS_INS_SUBVI_H:
		case MIPS_INS_SUBVI_W:
		case MIPS_INS_SUBVI_D:
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
		case MIPS_INS_CLE_U_B:
		case MIPS_INS_CLE_U_H:
		case MIPS_INS_CLE_U_W:
		case MIPS_INS_CLE_U_D:
		case MIPS_INS_CLEI_S_B:
		case MIPS_INS_CLEI_S_H:
		case MIPS_INS_CLEI_S_W:
		case MIPS_INS_CLEI_S_D:
		case MIPS_INS_CLEI_U_B:
		case MIPS_INS_CLEI_U_H:
		case MIPS_INS_CLEI_U_W:
		case MIPS_INS_CLEI_U_D:
		case MIPS_INS_CLT_S_B:
		case MIPS_INS_CLT_S_H:
		case MIPS_INS_CLT_S_W:
		case MIPS_INS_CLT_S_D:
		case MIPS_INS_CLT_U_B:
		case MIPS_INS_CLT_U_H:
		case MIPS_INS_CLT_U_W:
		case MIPS_INS_CLT_U_D:
		case MIPS_INS_CLTI_S_B:
		case MIPS_INS_CLTI_S_H:
		case MIPS_INS_CLTI_S_W:
		case MIPS_INS_CLTI_S_D:
		case MIPS_INS_CLTI_U_B:
		case MIPS_INS_CLTI_U_H:
		case MIPS_INS_CLTI_U_W:
		case MIPS_INS_CLTI_U_D:
		case MIPS_INS_FADD_W:
		case MIPS_INS_FADD_D:
		case MIPS_INS_FSUB_W:
		case MIPS_INS_FSUB_D:
		case MIPS_INS_FMUL_W:
		case MIPS_INS_FMUL_D:
		case MIPS_INS_FDIV_W:
		case MIPS_INS_FDIV_D:
		case MIPS_INS_FMAX_W:
		case MIPS_INS_FMAX_D:
		case MIPS_INS_FMIN_W:
		case MIPS_INS_FMIN_D:
		case MIPS_INS_FMAX_A_W:
		case MIPS_INS_FMAX_A_D:
		case MIPS_INS_FMIN_A_W:
		case MIPS_INS_FMIN_A_D:
		case MIPS_INS_FCAF_W:
		case MIPS_INS_FCAF_D:
		case MIPS_INS_FCEQ_W:
		case MIPS_INS_FCEQ_D:
		case MIPS_INS_FCLE_W:
		case MIPS_INS_FCLE_D:
		case MIPS_INS_FCLT_W:
		case MIPS_INS_FCLT_D:
		case MIPS_INS_FCNE_W:
		case MIPS_INS_FCNE_D:
		case MIPS_INS_FCOR_W:
		case MIPS_INS_FCOR_D:
		case MIPS_INS_FCUEQ_W:
		case MIPS_INS_FCUEQ_D:
		case MIPS_INS_FCULE_W:
		case MIPS_INS_FCULE_D:
		case MIPS_INS_FCULT_W:
		case MIPS_INS_FCULT_D:
		case MIPS_INS_FCUNE_W:
		case MIPS_INS_FCUNE_D:
		case MIPS_INS_FCUN_W:
		case MIPS_INS_FCUN_D:
		case MIPS_INS_DOTP_S_H:
		case MIPS_INS_DOTP_S_W:
		case MIPS_INS_DOTP_S_D:
		case MIPS_INS_DOTP_U_H:
		case MIPS_INS_DOTP_U_W:
		case MIPS_INS_DOTP_U_D:
		case MIPS_INS_DPADD_S_H:
		case MIPS_INS_DPADD_S_W:
		case MIPS_INS_DPADD_S_D:
		case MIPS_INS_DPADD_U_H:
		case MIPS_INS_DPADD_U_W:
		case MIPS_INS_DPADD_U_D:
		case MIPS_INS_DPSUB_S_H:
		case MIPS_INS_DPSUB_S_W:
		case MIPS_INS_DPSUB_S_D:
		case MIPS_INS_DPSUB_U_H:
		case MIPS_INS_DPSUB_U_W:
		case MIPS_INS_DPSUB_U_D:
		case MIPS_INS_HADD_S_H:
		case MIPS_INS_HADD_S_W:
		case MIPS_INS_HADD_S_D:
		case MIPS_INS_HADD_U_H:
		case MIPS_INS_HADD_U_W:
		case MIPS_INS_HADD_U_D:
		case MIPS_INS_HSUB_S_H:
		case MIPS_INS_HSUB_S_W:
		case MIPS_INS_HSUB_S_D:
		case MIPS_INS_HSUB_U_H:
		case MIPS_INS_HSUB_U_W:
		case MIPS_INS_HSUB_U_D:
		case MIPS_INS_DIV_S_B:
		case MIPS_INS_DIV_S_H:
		case MIPS_INS_DIV_S_W:
		case MIPS_INS_DIV_S_D:
		case MIPS_INS_DIV_U_B:
		case MIPS_INS_DIV_U_H:
		case MIPS_INS_DIV_U_W:
		case MIPS_INS_DIV_U_D:
		case MIPS_INS_MOD_S_B:
		case MIPS_INS_MOD_S_H:
		case MIPS_INS_MOD_S_W:
		case MIPS_INS_MOD_S_D:
		case MIPS_INS_MOD_U_B:
		case MIPS_INS_MOD_U_H:
		case MIPS_INS_MOD_U_W:
		case MIPS_INS_MOD_U_D:
		case MIPS_INS_MOVE_V:
		case MIPS_INS_BNZ_B:
		case MIPS_INS_BNZ_H:
		case MIPS_INS_BNZ_W:
		case MIPS_INS_BNZ_D:
		case MIPS_INS_BNZ_V:
		case MIPS_INS_BZ_B:
		case MIPS_INS_BZ_H:
		case MIPS_INS_BZ_W:
		case MIPS_INS_BZ_D:
		case MIPS_INS_BZ_V:
		case MIPS_INS_MADD_Q_H:
		case MIPS_INS_MADD_Q_W:
		case MIPS_INS_MSUB_Q_H:
		case MIPS_INS_MSUB_Q_W:
		case MIPS_INS_MUL_Q_H:
		case MIPS_INS_MUL_Q_W:
		case MIPS_INS_ADDS_A_B:
		case MIPS_INS_ADDS_A_H:
		case MIPS_INS_ADDS_A_W:
		case MIPS_INS_ADDS_A_D:
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
		case MIPS_INS_SRA_B:
		case MIPS_INS_SRA_H:
		case MIPS_INS_SRA_W:
		case MIPS_INS_SRA_D:
		case MIPS_INS_SLLI_B:
		case MIPS_INS_SLLI_H:
		case MIPS_INS_SLLI_W:
		case MIPS_INS_SLLI_D:
		case MIPS_INS_SRAI_B:
		case MIPS_INS_SRAI_H:
		case MIPS_INS_SRAI_W:
		case MIPS_INS_SRAI_D:
		case MIPS_INS_SRLI_B:
		case MIPS_INS_SRLI_H:
		case MIPS_INS_SRLI_W:
		case MIPS_INS_SRLI_D:
		case MIPS_INS_SRARI_B:
		case MIPS_INS_SRARI_H:
		case MIPS_INS_SRARI_W:
		case MIPS_INS_SRARI_D:
		case MIPS_INS_SRAR_B:
		case MIPS_INS_SRAR_H:
		case MIPS_INS_SRAR_W:
		case MIPS_INS_SRAR_D:
		case MIPS_INS_SRLRI_B:
		case MIPS_INS_SRLRI_H:
		case MIPS_INS_SRLRI_W:
		case MIPS_INS_SRLRI_D:
		case MIPS_INS_SRLR_B:
		case MIPS_INS_SRLR_H:
		case MIPS_INS_SRLR_W:
		case MIPS_INS_SRLR_D:
		case MIPS_INS_SUBSUS_U_B:
		case MIPS_INS_SUBSUS_U_H:
		case MIPS_INS_SUBSUS_U_W:
		case MIPS_INS_SUBSUS_U_D:
		case MIPS_INS_SUBSUU_S_B:
		case MIPS_INS_SUBSUU_S_H:
		case MIPS_INS_SUBSUU_S_W:
		case MIPS_INS_SUBSUU_S_D:
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
		case MIPS_INS_FCLASS_W:
		case MIPS_INS_FCLASS_D:
		case MIPS_INS_FMADD_W:
		case MIPS_INS_FMADD_D:
		case MIPS_INS_FMSUB_W:
		case MIPS_INS_FMSUB_D:
		case MIPS_INS_FEXDO_H:
		case MIPS_INS_FEXDO_W:
		case MIPS_INS_FEXP2_W:
		case MIPS_INS_FEXP2_D:
		case MIPS_INS_FEXUPL_W:
		case MIPS_INS_FEXUPL_D:
		case MIPS_INS_FEXUPR_W:
		case MIPS_INS_FEXUPR_D:
		case MIPS_INS_FFINT_S_W:
		case MIPS_INS_FFINT_S_D:
		case MIPS_INS_FFINT_U_W:
		case MIPS_INS_FFINT_U_D:
		case MIPS_INS_FFQL_W:
		case MIPS_INS_FFQL_D:
		case MIPS_INS_FFQR_W:
		case MIPS_INS_FFQR_D:
		case MIPS_INS_FLOG2_W:
		case MIPS_INS_FLOG2_D:
		case MIPS_INS_FRCP_W:
		case MIPS_INS_FRCP_D:
		case MIPS_INS_FRINT_W:
		case MIPS_INS_FRINT_D:
		case MIPS_INS_FRSQRT_W:
		case MIPS_INS_FRSQRT_D:
		case MIPS_INS_FSQRT_W:
		case MIPS_INS_FSQRT_D:
		case MIPS_INS_FSAF_W:
		case MIPS_INS_FSAF_D:
		case MIPS_INS_FSEQ_W:
		case MIPS_INS_FSEQ_D:
		case MIPS_INS_FSLE_W:
		case MIPS_INS_FSLE_D:
		case MIPS_INS_FSLT_W:
		case MIPS_INS_FSLT_D:
		case MIPS_INS_FSNE_W:
		case MIPS_INS_FSNE_D:
		case MIPS_INS_FSOR_W:
		case MIPS_INS_FSOR_D:
		case MIPS_INS_FSUEQ_W:
		case MIPS_INS_FSUEQ_D:
		case MIPS_INS_FSULE_W:
		case MIPS_INS_FSULE_D:
		case MIPS_INS_FSULT_W:
		case MIPS_INS_FSULT_D:
		case MIPS_INS_FSUNE_W:
		case MIPS_INS_FSUNE_D:
		case MIPS_INS_FSUN_W:
		case MIPS_INS_FSUN_D:
		case MIPS_INS_FTINT_S_W:
		case MIPS_INS_FTINT_S_D:
		case MIPS_INS_FTINT_U_W:
		case MIPS_INS_FTINT_U_D:
		case MIPS_INS_FTQ_H:
		case MIPS_INS_FTQ_W:
		case MIPS_INS_FTRUNC_S_W:
		case MIPS_INS_FTRUNC_S_D:
		case MIPS_INS_FTRUNC_U_W:
		case MIPS_INS_FTRUNC_U_D:
		case MIPS_INS_MADDR_Q_H:
		case MIPS_INS_MADDR_Q_W:
		case MIPS_INS_MULR_Q_H:
		case MIPS_INS_MULR_Q_W:
		case MIPS_INS_MSUBR_Q_H:
		case MIPS_INS_MSUBR_Q_W:
		case MIPS_INS_MADDV_B:
		case MIPS_INS_MADDV_H:
		case MIPS_INS_MADDV_W:
		case MIPS_INS_MADDV_D:
		case MIPS_INS_MSUBV_B:
		case MIPS_INS_MSUBV_H:
		case MIPS_INS_MSUBV_W:
		case MIPS_INS_MSUBV_D:
		case MIPS_INS_MAXI_S_B:
		case MIPS_INS_MAXI_S_H:
		case MIPS_INS_MAXI_S_W:
		case MIPS_INS_MAXI_S_D:
		case MIPS_INS_MAXI_U_B:
		case MIPS_INS_MAXI_U_H:
		case MIPS_INS_MAXI_U_W:
		case MIPS_INS_MAXI_U_D:
		case MIPS_INS_MINI_S_B:
		case MIPS_INS_MINI_S_H:
		case MIPS_INS_MINI_S_W:
		case MIPS_INS_MINI_S_D:
		case MIPS_INS_MINI_U_B:
		case MIPS_INS_MINI_U_H:
		case MIPS_INS_MINI_U_W:
		case MIPS_INS_MINI_U_D:
		case MIPS_INS_MAX_A_B:
		case MIPS_INS_MAX_A_H:
		case MIPS_INS_MAX_A_W:
		case MIPS_INS_MAX_A_D:
		case MIPS_INS_MIN_A_B:
		case MIPS_INS_MIN_A_H:
		case MIPS_INS_MIN_A_W:
		case MIPS_INS_MIN_A_D:
		case MIPS_INS_MAX_U_B:
		case MIPS_INS_MAX_U_H:
		case MIPS_INS_MAX_U_W:
		case MIPS_INS_MAX_U_D:
		case MIPS_INS_MIN_U_B:
		case MIPS_INS_MIN_U_H:
		case MIPS_INS_MIN_U_W:
		case MIPS_INS_MIN_U_D:
		case MIPS_INS_NLOC_B:
		case MIPS_INS_NLOC_H:
		case MIPS_INS_NLOC_W:
		case MIPS_INS_NLOC_D:
		case MIPS_INS_NLZC_B:
		case MIPS_INS_NLZC_H:
		case MIPS_INS_NLZC_W:
		case MIPS_INS_NLZC_D:
		case MIPS_INS_PCNT_B:
		case MIPS_INS_PCNT_H:
		case MIPS_INS_PCNT_W:
		case MIPS_INS_PCNT_D:
		case MIPS_INS_INSVE_B:
		case MIPS_INS_INSVE_H:
		case MIPS_INS_INSVE_W:
		case MIPS_INS_INSVE_D:
		case MIPS_INS_VSHF_B:
		case MIPS_INS_VSHF_H:
		case MIPS_INS_VSHF_W:
		case MIPS_INS_VSHF_D:
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

// microMIPS *ALS / JALS: 16-bit delay slot, so $ra = PC + size + 2.
static bool isShortDelayLinkId(uint32_t id)
{
	switch (id)
	{
		case MIPS_INS_BGEZALS:
		case MIPS_INS_BLTZALS:
		case MIPS_INS_JALS:
			return true;
		default:
			return false;
	}
}

static llvm::Value* mipsLinkAddress(cs_insn* i, llvm::Type* ty)
{
	if (isCompactLinkId(i->id))
	{
		return llvm::ConstantInt::get(ty, i->address + i->size);
	}
	if (isShortDelayLinkId(i->id))
	{
		return llvm::ConstantInt::get(ty, i->address + i->size + 2);
	}
	return llvm::ConstantInt::get(ty, i->address + (2 * i->size));
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
	if (i->id == MIPS_INS_FADD_D && hasMsaOperand(mi))
	{
		translateMsaFbin(i, mi, irb);
		return;
	}

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
		case MIPS_INS_BGEZALS:
			cond = irb.CreateICmpSGE(op0, zero);
			break;
		case MIPS_INS_BLTZAL:
		case MIPS_INS_BLTZALL:
		case MIPS_INS_BLTZALC:
		case MIPS_INS_BLTZALS: cond = irb.CreateICmpSLT(op0, zero); break;
		case MIPS_INS_BGTZALC: cond = irb.CreateICmpSGT(op0, zero); break;
		case MIPS_INS_BLEZALC: cond = irb.CreateICmpSLE(op0, zero); break;
		case MIPS_INS_BEQZALC: cond = irb.CreateICmpEQ(op0, zero); break;
		case MIPS_INS_BNEZALC: cond = irb.CreateICmpNE(op0, zero); break;
		default: throw GenericError("Unhandled insn ID in translateBcondal().");
		}

		llvm::IRBuilder<> bodyIrb(generateIfThen(cond, irb));

		llvm::Value* ra = mipsLinkAddress(i, getDefaultType());
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
 * The trap is in how Capstone reports it. Capstone 5 put the hardware-register
 * number in an ordinary GPR id, and MIPS_REG_29 is the same id as MIPS_REG_SP --
 * so a translation that took operand 1 at face value and loaded it would read
 * the stack pointer and produce something that looks entirely plausible.
 * Capstone 6 prints three operands, `rt, hwr, sel`, and the selector is
 * MIPS_REG_HWR29 rather than GPR 29. Either way it is a selector into a
 * different register file, and it is compared as a number here rather than
 * loaded.
 *
 * The other hardware registers ($0 CPUNum, $1 SYNCI_Step, $2 CC, $3 CCRes)
 * stay on the pseudo-assembly path: a cycle counter is not a value this
 * decompiler can model, and pretending otherwise would be worse than the call.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateRdhwr(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	// 2: rt, $29 (Capstone 5). 3: rt, HWR29, sel (Capstone 6).
	if (mi->op_count != 2 && mi->op_count != 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	if (mi->operands[0].type != MIPS_OP_REG || mi->operands[1].type != MIPS_OP_REG)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	uint32_t hw = mi->operands[1].reg;
	bool ulr = hw == MIPS_REG_HWR29 || hw == MIPS_REG_29;
	if (!ulr)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	if (mi->op_count == 3 && (mi->operands[2].type != MIPS_OP_IMM || mi->operands[2].imm != 0))
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
	// Printed form is rt, rs, pos, size. The MC instruction also carries a
	// tied copy of rt; if that copy reaches the detail, it is a fifth operand
	// and the immediates are still at slots 2 and 3.
	if (mi->op_count != 4 && mi->op_count != 5)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

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
 * R6 CMP.cond.fmt — compare fs, ft and write all-0 / all-1 bits into fd.
 * Capstone 6 splits each cond/fmt into CMP_*_S / CMP_*_D (not unsuffixed CMP).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateR6Cmp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	if (op1 == nullptr || op2 == nullptr
			|| !op1->getType()->isFloatingPointTy()
			|| !op2->getType()->isFloatingPointTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	if (op1->getType() != op2->getType())
	{
		op2 = irb.CreateFPCast(op2, op1->getType());
	}

	llvm::Value* cmp = nullptr;
	switch (i->id)
	{
		case MIPS_INS_CMP_AF_S:
		case MIPS_INS_CMP_AF_D:
		case MIPS_INS_CMP_SAF_S:
		case MIPS_INS_CMP_SAF_D:
			cmp = irb.getFalse();
			break;
		case MIPS_INS_CMP_UN_S:
		case MIPS_INS_CMP_UN_D:
		case MIPS_INS_CMP_SUN_S:
		case MIPS_INS_CMP_SUN_D:
			cmp = irb.CreateFCmpUNO(op1, op2);
			break;
		case MIPS_INS_CMP_EQ_S:
		case MIPS_INS_CMP_EQ_D:
		case MIPS_INS_CMP_SEQ_S:
		case MIPS_INS_CMP_SEQ_D:
			cmp = irb.CreateFCmpOEQ(op1, op2);
			break;
		case MIPS_INS_CMP_UEQ_S:
		case MIPS_INS_CMP_UEQ_D:
		case MIPS_INS_CMP_SUEQ_S:
		case MIPS_INS_CMP_SUEQ_D:
			cmp = irb.CreateFCmpUEQ(op1, op2);
			break;
		case MIPS_INS_CMP_LT_S:
		case MIPS_INS_CMP_LT_D:
		case MIPS_INS_CMP_SLT_S:
		case MIPS_INS_CMP_SLT_D:
			cmp = irb.CreateFCmpOLT(op1, op2);
			break;
		case MIPS_INS_CMP_ULT_S:
		case MIPS_INS_CMP_ULT_D:
		case MIPS_INS_CMP_SULT_S:
		case MIPS_INS_CMP_SULT_D:
			cmp = irb.CreateFCmpULT(op1, op2);
			break;
		case MIPS_INS_CMP_LE_S:
		case MIPS_INS_CMP_LE_D:
		case MIPS_INS_CMP_SLE_S:
		case MIPS_INS_CMP_SLE_D:
			cmp = irb.CreateFCmpOLE(op1, op2);
			break;
		case MIPS_INS_CMP_ULE_S:
		case MIPS_INS_CMP_ULE_D:
		case MIPS_INS_CMP_SULE_S:
		case MIPS_INS_CMP_SULE_D:
			cmp = irb.CreateFCmpULE(op1, op2);
			break;
		default:
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}

	unsigned bits = op1->getType()->getPrimitiveSizeInBits();
	auto* all = irb.CreateSExt(cmp, irb.getIntNTy(bits));
	storeOp(mi->operands[0], irb.CreateBitCast(all, op1->getType()), irb);
}

/**
 * MIPS_INS_POP / MIPS_INS_DPOP — population count. POP is a word ctpop;
 * DPOP is the 64-bit form.
 */
void Capstone2LlvmIrTranslatorMips_impl::translatePop(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	op1 = narrowToWord(i, irb, op1);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::ctpop,
			op1->getType());
	storeOp(mi->operands[0], irb.CreateCall(f, {op1}), irb);
}

/**
 * MIPS_INS_BADDU — unsigned byte add: dest = (rs + rt) & 0xff.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBaddu(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(i, irb, op1);
	op2 = narrowToWord(i, irb, op2);
	auto* sum = irb.CreateAdd(op1, op2);
	storeOp(mi->operands[0], irb.CreateAnd(sum, llvm::ConstantInt::get(sum->getType(), 0xff)), irb);
}

/**
 * Cavium CINS / CINS32 / EXTS / EXTS32.
 * Capstone reports dest, src, p, len where width = len+1.
 * EXTS32 / CINS32 add 32 to the position.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateCnBitfield(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	op3 = loadOp(mi->operands[3], irb);
	auto* ty = llvm::dyn_cast<llvm::IntegerType>(op1->getType());
	if (ty == nullptr || !llvm::isa<llvm::ConstantInt>(op2) || !llvm::isa<llvm::ConstantInt>(op3))
	{
		translatePseudoAsmOp0FncOp1Op2Op3(i, mi, irb);
		return;
	}

	uint64_t pos = llvm::cast<llvm::ConstantInt>(op2)->getZExtValue();
	uint64_t lenm1 = llvm::cast<llvm::ConstantInt>(op3)->getZExtValue();
	if (i->id == MIPS_INS_CINS32 || i->id == MIPS_INS_EXTS32)
	{
		pos += 32;
	}
	uint64_t width = lenm1 + 1;
	unsigned bits = ty->getBitWidth();
	if (width == 0 || width > bits || pos >= bits || width > bits - pos)
	{
		translatePseudoAsmOp0FncOp1Op2Op3(i, mi, irb);
		return;
	}

	if (i->id == MIPS_INS_EXTS || i->id == MIPS_INS_EXTS32)
	{
		auto* sh = irb.CreateLShr(op1, llvm::ConstantInt::get(ty, pos));
		auto* field = irb.CreateTrunc(sh, irb.getIntNTy(static_cast<unsigned>(width)));
		storeOp(mi->operands[0], irb.CreateSExt(field, ty), irb);
		return;
	}

	uint64_t field = width >= 64 ? ~0ull : ((1ull << width) - 1);
	auto* mask = llvm::ConstantInt::get(ty, field << pos);
	llvm::Value* old = loadOp(mi->operands[0], irb);
	old = generateTypeConversion(irb, old, ty, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	llvm::Value* ins = irb.CreateAnd(irb.CreateShl(op1, llvm::ConstantInt::get(ty, pos)), mask);
	storeOp(mi->operands[0], irb.CreateOr(irb.CreateAnd(old, irb.CreateNot(mask)), ins), irb);
}

/**
 * Cavium BBIT0 / BBIT1 / BBIT032 / BBIT132.
 * Operand order from CS_MODE_OCTEON dump: rs, bit-index imm, branch target.
 * BBIT032 / BBIT132 add 32 to the bit index. Branch if the selected bit is
 * 0 (BBIT0/032) or 1 (BBIT1/132).
 */
/**
 * COP2 registers are implementation defined; keep each one as an i32 global
 * so BC2EQZ/BC2NEZ can test its bit 0 rather than becoming opaque calls.
 */
llvm::GlobalVariable* Capstone2LlvmIrTranslatorMips_impl::getCop2Reg(uint32_t reg)
{
	const char* rn = cs_reg_name(_handle, reg);
	std::string name = std::string("cop2_") + (rn ? rn : std::to_string(reg));
	if (auto* gv = _module->getGlobalVariable(name))
	{
		return gv;
	}
	auto* i32 = llvm::Type::getInt32Ty(_module->getContext());
	return new llvm::GlobalVariable(*_module, i32, false, llvm::GlobalValue::ExternalLinkage,
			llvm::ConstantInt::get(i32, 0), name);
}

/**
 * MIPS_INS_BC2EQZ, MIPS_INS_BC2NEZ (+ compact forms): branch on bit 0 of a
 * COP2 register.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBc2eqz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	if (mi->operands[0].type != MIPS_OP_REG)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	auto* gv = getCop2Reg(mi->operands[0].reg);
	llvm::Value* v = irb.CreateLoad(gv->getValueType(), gv);
	v = irb.CreateAnd(v, llvm::ConstantInt::get(v->getType(), 1));
	auto* z = llvm::ConstantInt::get(v->getType(), 0);
	bool isEq = i->id == MIPS_INS_BC2EQZ || i->id == MIPS_INS_BC2EQZC;
	auto* c = isEq ? irb.CreateICmpEQ(v, z) : irb.CreateICmpNE(v, z);
	op1 = loadOp(mi->operands[1], irb);
	generateCondBranchFunctionCall(irb, c, op1);
}

/**
 * MIPS_INS_SAA, MIPS_INS_SAAD (Octeon+): atomic memory[base] += rt,
 * 32-bit for SAA and 64-bit for SAAD. Nothing is written back to a GPR.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSaa(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	auto* ty = i->id == MIPS_INS_SAAD ? irb.getInt64Ty() : irb.getInt32Ty();
	auto* rt = irb.CreateZExtOrTrunc(loadOp(mi->operands[0], irb), ty);
	llvm::Value* addr = nullptr;
	if (mi->operands[1].type == MIPS_OP_MEM)
	{
		addr = loadRegister(mi->operands[1].mem.base, irb);
		if (mi->operands[1].mem.disp)
		{
			addr = irb.CreateAdd(addr, llvm::ConstantInt::getSigned(addr->getType(), mi->operands[1].mem.disp));
		}
	}
	else
	{
		addr = loadOp(mi->operands[1], irb);
	}
	auto* pt = llvm::PointerType::get(ty, 0);
	auto* p = irb.CreateIntToPtr(addr, pt);
	auto* old = irb.CreateLoad(ty, p);
	irb.CreateStore(irb.CreateAdd(old, rt), p);
}

/**
 * MIPS16 CMP rx, ry / CMPI rx, imm: T ($24) = rx XOR (ry | zero-extended imm).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMips16Cmp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	auto* a = loadOp(mi->operands[0], irb);
	llvm::Value* b = nullptr;
	if (mi->operands[1].type == MIPS_OP_IMM)
	{
		b = llvm::ConstantInt::get(a->getType(), static_cast<uint64_t>(mi->operands[1].imm) & 0xffff);
	}
	else
	{
		b = irb.CreateZExtOrTrunc(loadOp(mi->operands[1], irb), a->getType());
	}
	storeRegister(MIPS_REG_T8, irb.CreateXor(a, b), irb);
}

/**
 * MIPS16 BTEQZ/BTNEZ: branch when T ($24) is (not) zero. Capstone reports the
 * raw offset field, so the target is computed here: PC of the next
 * instruction plus the sign-extended halfword offset (8 bits, or 16 bits for
 * the EXTEND form). MIPS16 conditional branches have no delay slot.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMips16Bteqz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	int64_t off = 0;
	uint64_t raw = static_cast<uint64_t>(mi->operands[0].imm);
	if (i->size == 4)
	{
		off = static_cast<int16_t>(raw & 0xffff);
	}
	else
	{
		off = static_cast<int8_t>(raw & 0xff);
	}
	uint64_t target = i->address + i->size + static_cast<uint64_t>(off * 2);

	auto* t = loadRegister(MIPS_REG_T8, irb);
	auto* z = llvm::ConstantInt::get(t->getType(), 0);
	auto* c = i->id == MIPS_INS_BTEQZ ? irb.CreateICmpEQ(t, z) : irb.CreateICmpNE(t, z);
	generateCondBranchFunctionCall(irb, c, llvm::ConstantInt::get(getDefaultType(), target));
}

void Capstone2LlvmIrTranslatorMips_impl::translateBbit(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	auto* src = loadOp(mi->operands[0], irb);
	auto* bitV = loadOp(mi->operands[1], irb);
	llvm::Value* tgt = loadOp(mi->operands[2], irb);

	uint64_t bit = 0;
	if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(bitV))
	{
		bit = ci->getZExtValue();
	}
	if (i->id == MIPS_INS_BBIT032 || i->id == MIPS_INS_BBIT132)
	{
		bit += 32;
	}

	unsigned sw = src->getType()->getIntegerBitWidth();
	unsigned tw = sw < 64 ? 64 : sw;
	auto* ty = irb.getIntNTy(tw);
	src = irb.CreateZExtOrTrunc(src, ty);
	uint64_t maskBits = (bit < tw) ? (uint64_t(1) << bit) : 0;
	auto* masked = irb.CreateAnd(src, llvm::ConstantInt::get(ty, maskBits));
	auto* zero = llvm::ConstantInt::get(ty, 0);
	bool wantClear = i->id == MIPS_INS_BBIT0 || i->id == MIPS_INS_BBIT032;
	auto* cond = wantClear ? irb.CreateICmpEQ(masked, zero)
			: irb.CreateICmpNE(masked, zero);
	generateCondBranchFunctionCall(irb, cond, tgt);
}

/**
 * MIPS_INS_VMULU — Octeon unsigned multiply. Capstone reports a GPR dest:
 * dest = low half of zext(rs)*zext(rt).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateVmulu(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned half = op1->getType()->getIntegerBitWidth();
	auto* wide = irb.getIntNTy(half * 2);
	auto* mul = irb.CreateMul(irb.CreateZExt(op1, wide), irb.CreateZExt(op2, wide));
	storeOp(mi->operands[0], irb.CreateTrunc(mul, irb.getIntNTy(half)), irb);
}

/**
 * MIPS_INS_MTM0/1/2, MIPS_INS_MTP0/1/2 — move GPR into MPL/P.
 * CS_MODE_OCTEON dump: one GPR operand.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMtm(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, mi, irb);

	op0 = loadOpUnary(mi, irb);
	uint32_t dst = MIPS_REG_INVALID;
	switch (i->id)
	{
		case MIPS_INS_MTM0: dst = MIPS_REG_MPL0; break;
		case MIPS_INS_MTM1: dst = MIPS_REG_MPL1; break;
		case MIPS_INS_MTM2: dst = MIPS_REG_MPL2; break;
		case MIPS_INS_MTP0: dst = MIPS_REG_P0; break;
		case MIPS_INS_MTP1: dst = MIPS_REG_P1; break;
		case MIPS_INS_MTP2: dst = MIPS_REG_P2; break;
		default:
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}
	storeRegister(dst, op0, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_DIV
 */
void Capstone2LlvmIrTranslatorMips_impl::translateDiv(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (i->id == MIPS_INS_FDIV_D && hasMsaOperand(mi))
	{
		translateMsaFbin(i, mi, irb);
		return;
	}

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

	llvm::Value* ra = mipsLinkAddress(i, getDefaultType());
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
 * MIPS64R6: DAUI rt, rs, imm — rt = rs + (imm << 16)
 * DAHI rt, imm — rt += imm << 32 (Capstone repeats rt as rs)
 * DATI rt, imm — rt += imm << 48
 */
void Capstone2LlvmIrTranslatorMips_impl::translateDaui(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* i64 = irb.getInt64Ty();
	op1 = irb.CreateSExtOrTrunc(op1, i64);
	op2 = irb.CreateSExtOrTrunc(op2, i64);
	unsigned sh = 16;
	if (i->id == MIPS_INS_DAHI)
	{
		sh = 32;
	}
	else if (i->id == MIPS_INS_DATI)
	{
		sh = 48;
	}
	op2 = irb.CreateShl(op2, llvm::ConstantInt::get(i64, sh));
	storeOp(mi->operands[0], irb.CreateAdd(op1, op2), irb);
}

/**
 * MIPS_INS_ADDIUR1SP — rd = SP + imm (Capstone reports the scaled byte offset).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateAddiur1sp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	auto* sp = loadRegister(MIPS_REG_SP, irb);
	op1 = irb.CreateSExtOrTrunc(op1, sp->getType());
	storeOp(mi->operands[0], irb.CreateAdd(sp, op1), irb);
}

/**
 * MIPS_INS_MOVEP — (rd, re) = (rs, rt). Four REG operands.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMovep(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count != 4
			|| mi->operands[0].type != MIPS_OP_REG
			|| mi->operands[1].type != MIPS_OP_REG
			|| mi->operands[2].type != MIPS_OP_REG
			|| mi->operands[3].type != MIPS_OP_REG)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* a = loadRegister(mi->operands[2].reg, irb);
	auto* b = loadRegister(mi->operands[3].reg, irb);
	if (a == nullptr || b == nullptr)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	storeRegister(mi->operands[0].reg, a, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeRegister(mi->operands[1].reg, b, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_LWXS — lw xs: rt = sign_extend(Mem[base + (index << 2)]).
 * Capstone reports dest, index, base as three REGs.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateLwxs(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count != 3
			|| mi->operands[0].type != MIPS_OP_REG
			|| mi->operands[1].type != MIPS_OP_REG
			|| mi->operands[2].type != MIPS_OP_REG)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* idx = loadRegister(mi->operands[1].reg, irb);
	auto* base = loadRegister(mi->operands[2].reg, irb);
	if (idx == nullptr || base == nullptr)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	idx = generateTypeConversion(irb, idx, base->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* addr = irb.CreateAdd(base, irb.CreateShl(idx, llvm::ConstantInt::get(idx->getType(), 2)));
	storeOp(mi->operands[0], loadIntPtr(irb, addr, irb.getInt32Ty()), irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
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
 * MIPS_INS_DBITSWAP — the same on a 64-bit GPR.
 * Full-width bitreverse followed by bswap is the per-byte reverse.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBitswap(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	auto* ty = i->id == MIPS_INS_DBITSWAP ? irb.getInt64Ty() : irb.getInt32Ty();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	op1 = irb.CreateUnaryIntrinsic(llvm::Intrinsic::bitreverse, op1);
	op1 = irb.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, op1);
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_LSA / MIPS_INS_DLSA — rd = (rs << sa) + rt.
 * Capstone 6 reports the assembly shift (1..4), not the encoded sa-1 field.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateLsa(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count != 4 || mi->operands[3].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	if (!op1->getType()->isIntegerTy() || !op2->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	uint64_t sa = static_cast<uint64_t>(mi->operands[3].imm);
	if (sa < 1 || sa > 4)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	op2 = generateTypeConversion(irb, op2, op1->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* sh = irb.CreateShl(op1, llvm::ConstantInt::get(op1->getType(), sa));
	storeOp(mi->operands[0], irb.CreateAdd(sh, op2), irb);
}

/**
 * MIPS_INS_CLASS_S / MIPS_INS_CLASS_D — 10-bit IEEE class mask into fd.
 * Same bits as MSA FCLASS. Unsuffixed CLASS is a Capstone 6 macro onto CLASS_D.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateClass(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	if (!op1->getType()->isFloatingPointTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned e = op1->getType()->isDoubleTy() ? 64 : 32;
	auto* bits = irb.CreateBitCast(op1, irb.getIntNTy(e));
	storeOp(mi->operands[0], irb.CreateBitCast(fpClassBits(bits, e, irb), op1->getType()), irb);
}

/**
 * MIPS_INS_BC1EQZ / BC1NEZ / compact BC1EQZC / BC1NEZC —
 * branch if the FPR bit pattern is == 0 / != 0. No delay slot.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBc1eqz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op0 = loadOp(mi->operands[0], irb);
	op1 = loadOp(mi->operands[1], irb);
	unsigned e = 32;
	if (op0->getType()->isIntegerTy())
	{
		e = op0->getType()->getIntegerBitWidth();
	}
	else if (op0->getType()->isDoubleTy())
	{
		e = 64;
	}
	auto* bits = op0->getType()->isIntegerTy()
			? irb.CreateZExtOrTrunc(op0, irb.getIntNTy(e))
			: irb.CreateBitCast(op0, irb.getIntNTy(e));
	auto* z = llvm::ConstantInt::get(bits->getType(), 0);
	bool isEq = i->id == MIPS_INS_BC1EQZ || i->id == MIPS_INS_BC1EQZC;
	auto* c = isEq ? irb.CreateICmpEQ(bits, z) : irb.CreateICmpNE(bits, z);
	generateCondBranchFunctionCall(irb, c, op1);
}

/**
 * MIPS_INS_CTC1 — GPR → FCR (FCSR is FCR31). Inverse of CFC1.
 * Capstone 6 reports the control operand as FCR0–FCR31.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateCtc1(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	if (mi->operands[0].type != MIPS_OP_REG || mi->operands[1].type != MIPS_OP_REG)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	uint32_t gpr = canonicalizeMipsReg(mi->operands[0].reg);
	uint32_t fcr = canonicalizeMipsReg(mi->operands[1].reg);
	if (i->id == MIPS_INS_CFC1)
	{
		gpr = canonicalizeMipsReg(mi->operands[0].reg);
		fcr = canonicalizeMipsReg(mi->operands[1].reg);
	}
	if (fcr < MIPS_REG_FCR0 || fcr > MIPS_REG_FCR31)
	{
		if (fcr >= MIPS_REG_F0 && fcr <= MIPS_REG_F31)
		{
			fcr = MIPS_REG_FCR0 + (fcr - MIPS_REG_F0);
		}
		else
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
	}

	if (i->id == MIPS_INS_CFC1)
	{
		storeRegister(gpr, loadRegister(fcr, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	else
	{
		storeRegister(fcr, loadRegister(gpr, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
}

/**
 * MIPS_INS_RINT_S / MIPS_INS_RINT_D — round to integral valued FP.
 * Unsuffixed RINT is a Capstone 6 macro onto RINT_D.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateRint(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	if (!op1->getType()->isFloatingPointTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module, llvm::Intrinsic::nearbyint, {op1->getType()});
	storeOp(mi->operands[0], irb.CreateCall(f, {op1}), irb);
}

/**
 * MIPS_INS_MAXA_S/D, MINA_S/D — select the operand with larger/smaller
 * absolute value, keeping the original sign. Unsuffixed MAXA/MINA are
 * Capstone 6 macros onto the _D ids.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMaxa(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::THROW);
	if (!op1->getType()->isFloatingPointTy() || op1->getType() != op2->getType())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned e = op1->getType()->isDoubleTy() ? 64 : 32;
	auto* iTy = irb.getIntNTy(e);
	auto* a = irb.CreateBitCast(op1, iTy);
	auto* b = irb.CreateBitCast(op2, iTy);
	uint64_t mag = e == 64 ? 0x7fffffffffffffffULL : 0x7fffffffULL;
	auto* mask = llvm::ConstantInt::get(iTy, mag);
	auto* absA = irb.CreateAnd(a, mask);
	auto* absB = irb.CreateAnd(b, mask);
	bool isMin = i->id == MIPS_INS_MINA_S || i->id == MIPS_INS_MINA_D;
	auto* pick1 = isMin ? irb.CreateICmpULE(absA, absB) : irb.CreateICmpUGE(absA, absB);
	storeOp(mi->operands[0], irb.CreateSelect(pick1, op1, op2), irb);
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
	const bool isD = i->id == MIPS_INS_DALIGN;
	unsigned width = isD ? 64 : 32;
	if (bp > (isD ? 7u : 3u))
	{
		translatePseudoAsmOp0FncOp1Op2Op3(i, mi, irb);
		return;
	}
	auto* ity = irb.getIntNTy(width);
	op1 = irb.CreateZExtOrTrunc(op1, ity);
	op2 = irb.CreateZExtOrTrunc(op2, ity);
	llvm::Value* res = nullptr;
	if (bp == 0)
	{
		res = op2;
	}
	else
	{
		unsigned sh = static_cast<unsigned>(bp * 8);
		res = irb.CreateOr(
				irb.CreateShl(op1, llvm::ConstantInt::get(ity, sh)),
				irb.CreateLShr(op2, llvm::ConstantInt::get(ity, width - sh)));
	}
	storeOp(mi->operands[0], res, irb);
}

/**
 * MIPS_INS_LWPC — load word from PC + offset (byte offset, this instruction).
 * MIPS_INS_LWUPC — same address, zero-extend the word.
 * MIPS_INS_LDPC — load doubleword (Capstone already scales the offset).
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
	if (i->id == MIPS_INS_LDPC)
	{
		storeOp(mi->operands[0], loadIntPtr(irb, addr, irb.getInt64Ty()), irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
		return;
	}
	auto* val = loadIntPtr(irb, addr, irb.getInt32Ty());
	eOpConv ct = i->id == MIPS_INS_LWUPC ? eOpConv::ZEXT_TRUNC_OR_BITCAST : eOpConv::SEXT_TRUNC_OR_BITCAST;
	storeOp(mi->operands[0], val, irb, ct);
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

static llvm::Value* dspToI32(llvm::Value* v, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	if (v->getType() == i32)
	{
		return v;
	}
	if (v->getType()->isIntegerTy())
	{
		return irb.CreateZExtOrTrunc(v, i32);
	}
	return irb.CreateBitCast(v, i32);
}

static llvm::Value* dspPackedBin(
		llvm::Value* a,
		llvm::Value* b,
		unsigned bits,
		llvm::Instruction::BinaryOps opc,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 32 / bits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(bits), n);
	auto* v0 = irb.CreateBitCast(dspToI32(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(dspToI32(b, irb), vecTy);
	return irb.CreateBitCast(irb.CreateBinOp(opc, v0, v1), irb.getInt32Ty());
}

static llvm::Value* dspPackedSat(
		llvm::Value* a,
		llvm::Value* b,
		unsigned bits,
		bool isSigned,
		bool isSub,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 32 / bits;
	auto* laneTy = irb.getIntNTy(bits);
	auto* wideTy = irb.getIntNTy(bits * 2);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	unsigned wb = bits * 2;
	llvm::APInt loA = isSigned ? llvm::APInt::getSignedMinValue(bits).sext(wb) : llvm::APInt::getZero(wb);
	llvm::APInt hiA = isSigned ? llvm::APInt::getSignedMaxValue(bits).sext(wb)
							   : llvm::APInt::getMaxValue(bits).zext(wb);
	auto* loC = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, loA));
	auto* hiC = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, hiA));
	auto* v0 = irb.CreateBitCast(dspToI32(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(dspToI32(b, irb), vecTy);
	auto* aw = isSigned ? irb.CreateSExt(v0, wideVec) : irb.CreateZExt(v0, wideVec);
	auto* bw = isSigned ? irb.CreateSExt(v1, wideVec) : irb.CreateZExt(v1, wideVec);
	auto* w = isSub ? irb.CreateSub(aw, bw) : irb.CreateAdd(aw, bw);
	auto* c = irb.CreateSelect(irb.CreateICmpSLT(w, loC), loC, w);
	c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
	return irb.CreateBitCast(irb.CreateTrunc(c, vecTy), irb.getInt32Ty());
}

static llvm::Value* dspPackedHalve(
		llvm::Value* a,
		llvm::Value* b,
		unsigned bits,
		bool isSub,
		bool round,
		bool isSigned,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 32 / bits;
	auto* laneTy = irb.getIntNTy(bits);
	auto* wideTy = irb.getIntNTy(bits * 2);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	auto* v0 = irb.CreateBitCast(dspToI32(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(dspToI32(b, irb), vecTy);
	auto* aw = isSigned ? irb.CreateSExt(v0, wideVec) : irb.CreateZExt(v0, wideVec);
	auto* bw = isSigned ? irb.CreateSExt(v1, wideVec) : irb.CreateZExt(v1, wideVec);
	auto* w = isSub ? irb.CreateSub(aw, bw) : irb.CreateAdd(aw, bw);
	if (round)
	{
		w = irb.CreateAdd(
				w,
				llvm::ConstantVector::getSplat(
						llvm::ElementCount::getFixed(n),
						llvm::ConstantInt::get(wideTy, 1)));
	}
	w = isSigned ? irb.CreateAShr(w, 1ull) : irb.CreateLShr(w, 1ull);
	return irb.CreateBitCast(irb.CreateTrunc(w, vecTy), irb.getInt32Ty());
}

static llvm::Value* dspPackedAbs(llvm::Value* a, unsigned bits, llvm::IRBuilder<>& irb)
{
	unsigned n = 32 / bits;
	auto* laneTy = irb.getIntNTy(bits);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* v = irb.CreateBitCast(dspToI32(a, irb), vecTy);
	auto* zero = llvm::Constant::getNullValue(vecTy);
	auto* minv = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n),
			llvm::ConstantInt::get(laneTy, llvm::APInt::getSignedMinValue(bits)));
	auto* maxv = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n),
			llvm::ConstantInt::get(laneTy, llvm::APInt::getSignedMaxValue(bits)));
	auto* neg = irb.CreateSub(zero, v);
	auto* sat = irb.CreateSelect(irb.CreateICmpEQ(v, minv), maxv, neg);
	return irb.CreateBitCast(irb.CreateSelect(irb.CreateICmpSLT(v, zero), sat, v), irb.getInt32Ty());
}

static llvm::Value* dspPackedShift(
		llvm::Value* a,
		llvm::Value* sh,
		unsigned bits,
		bool left,
		bool arith,
		bool sat,
		bool round,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 32 / bits;
	auto* laneTy = irb.getIntNTy(bits);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* v = irb.CreateBitCast(dspToI32(a, irb), vecTy);
	sh = irb.CreateAnd(
			irb.CreateZExtOrTrunc(sh, laneTy),
			llvm::ConstantInt::get(laneTy, bits - 1));
	llvm::Value* shv = llvm::Constant::getNullValue(vecTy);
	for (unsigned i = 0; i < n; ++i)
	{
		shv = irb.CreateInsertElement(shv, sh, i);
	}
	if (left)
	{
		auto* r = irb.CreateShl(v, shv);
		if (sat)
		{
			auto* wideTy = irb.getIntNTy(bits * 2);
			auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
			auto* w = irb.CreateShl(irb.CreateSExt(v, wideVec), irb.CreateZExt(shv, wideVec));
			auto* lo = llvm::ConstantVector::getSplat(
					llvm::ElementCount::getFixed(n),
					llvm::ConstantInt::get(wideTy, llvm::APInt::getSignedMinValue(bits).sext(bits * 2)));
			auto* hi = llvm::ConstantVector::getSplat(
					llvm::ElementCount::getFixed(n),
					llvm::ConstantInt::get(wideTy, llvm::APInt::getSignedMaxValue(bits).sext(bits * 2)));
			auto* c = irb.CreateSelect(irb.CreateICmpSLT(w, lo), lo, w);
			c = irb.CreateSelect(irb.CreateICmpSGT(c, hi), hi, c);
			r = irb.CreateTrunc(c, vecTy);
		}
		return irb.CreateBitCast(r, irb.getInt32Ty());
	}
	if (round)
	{
		auto* one = llvm::ConstantVector::getSplat(
				llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(laneTy, 1));
		auto* shm1 = irb.CreateSub(shv, one);
		auto* bias = irb.CreateSelect(
				irb.CreateICmpEQ(shv, llvm::Constant::getNullValue(vecTy)),
				llvm::Constant::getNullValue(vecTy),
				irb.CreateShl(one, shm1));
		v = irb.CreateAdd(v, bias);
	}
	auto* r = arith ? irb.CreateAShr(v, shv) : irb.CreateLShr(v, shv);
	return irb.CreateBitCast(r, irb.getInt32Ty());
}

static llvm::Value* dspRepl(llvm::Value* v, unsigned bits, llvm::IRBuilder<>& irb)
{
	v = irb.CreateZExtOrTrunc(v, irb.getIntNTy(bits));
	unsigned n = 32 / bits;
	llvm::Value* r = llvm::ConstantInt::get(irb.getInt32Ty(), 0);
	for (unsigned i = 0; i < n; ++i)
	{
		auto* lane = irb.CreateZExt(v, irb.getInt32Ty());
		r = irb.CreateOr(r, irb.CreateShl(lane, i * bits));
	}
	return r;
}

static llvm::Value* dspCmpMask(
		llvm::Value* a,
		llvm::Value* b,
		unsigned bits,
		llvm::CmpInst::Predicate pred,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 32 / bits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(bits), n);
	auto* v0 = irb.CreateBitCast(dspToI32(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(dspToI32(b, irb), vecTy);
	auto* c = irb.CreateICmp(pred, v0, v1);
	llvm::Value* mask = llvm::ConstantInt::get(irb.getInt32Ty(), 0);
	for (unsigned i = 0; i < n; ++i)
	{
		auto* bit = irb.CreateZExt(irb.CreateExtractElement(c, i), irb.getInt32Ty());
		mask = irb.CreateOr(mask, irb.CreateShl(bit, i));
	}
	return mask;
}

static llvm::Value* dspPick(
		llvm::Value* rs,
		llvm::Value* rt,
		llvm::Value* cond,
		unsigned bits,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 32 / bits;
	auto* i32 = irb.getInt32Ty();
	rs = dspToI32(rs, irb);
	rt = dspToI32(rt, irb);
	cond = irb.CreateZExtOrTrunc(cond, i32);
	llvm::Value* r = llvm::ConstantInt::get(i32, 0);
	auto* mask = llvm::ConstantInt::get(i32, bits == 32 ? 0xffffffffu : (1u << bits) - 1);
	for (unsigned i = 0; i < n; ++i)
	{
		auto* bit = irb.CreateAnd(irb.CreateLShr(cond, i), llvm::ConstantInt::get(i32, 1));
		auto* takeRs = irb.CreateICmpNE(bit, llvm::ConstantInt::get(i32, 0));
		auto* a = irb.CreateAnd(irb.CreateLShr(rs, i * bits), mask);
		auto* b = irb.CreateAnd(irb.CreateLShr(rt, i * bits), mask);
		r = irb.CreateOr(r, irb.CreateShl(irb.CreateSelect(takeRs, a, b), i * bits));
	}
	return r;
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
		case MIPS_INS_ADDVI_B:
		case MIPS_INS_SUBV_B:
		case MIPS_INS_SUBVI_B:
		case MIPS_INS_ADD_A_B:
		case MIPS_INS_ASUB_S_B:
		case MIPS_INS_ASUB_U_B:
		case MIPS_INS_AVE_S_B:
		case MIPS_INS_AVE_U_B:
		case MIPS_INS_AVER_S_B:
		case MIPS_INS_AVER_U_B:
		case MIPS_INS_BCLR_B:
		case MIPS_INS_BCLRI_B:
		case MIPS_INS_BSET_B:
		case MIPS_INS_BSETI_B:
		case MIPS_INS_BNEG_B:
		case MIPS_INS_BNEGI_B:
		case MIPS_INS_BINSL_B:
		case MIPS_INS_BINSLI_B:
		case MIPS_INS_BINSR_B:
		case MIPS_INS_BINSRI_B:
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
		case MIPS_INS_CLE_U_B:
		case MIPS_INS_CLEI_S_B:
		case MIPS_INS_CLEI_U_B:
		case MIPS_INS_CLT_S_B:
		case MIPS_INS_CLT_U_B:
		case MIPS_INS_CLTI_S_B:
		case MIPS_INS_CLTI_U_B:
		case MIPS_INS_MAX_S_B:
		case MIPS_INS_MIN_S_B:
		case MIPS_INS_SLL_B:
		case MIPS_INS_SRL_B:
		case MIPS_INS_SRA_B:
		case MIPS_INS_SLLI_B:
		case MIPS_INS_SRAI_B:
		case MIPS_INS_SRLI_B:
		case MIPS_INS_SRARI_B:
		case MIPS_INS_SRAR_B:
		case MIPS_INS_SRLRI_B:
		case MIPS_INS_SRLR_B:
		case MIPS_INS_SUBSUS_U_B:
		case MIPS_INS_SUBSUU_S_B:
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
		case MIPS_INS_ADDS_A_B:
		case MIPS_INS_DIV_S_B:
		case MIPS_INS_DIV_U_B:
		case MIPS_INS_MOD_S_B:
		case MIPS_INS_MOD_U_B:
		case MIPS_INS_MADDV_B:
		case MIPS_INS_MSUBV_B:
		case MIPS_INS_MAXI_S_B:
		case MIPS_INS_MAXI_U_B:
		case MIPS_INS_MINI_S_B:
		case MIPS_INS_MINI_U_B:
		case MIPS_INS_MAX_A_B:
		case MIPS_INS_MIN_A_B:
		case MIPS_INS_MAX_U_B:
		case MIPS_INS_MIN_U_B:
		case MIPS_INS_NLOC_B:
		case MIPS_INS_NLZC_B:
		case MIPS_INS_PCNT_B:
		case MIPS_INS_INSVE_B:
		case MIPS_INS_VSHF_B:
			return 8;
		case MIPS_INS_ADDV_H:
		case MIPS_INS_ADDVI_H:
		case MIPS_INS_SUBV_H:
		case MIPS_INS_SUBVI_H:
		case MIPS_INS_ADD_A_H:
		case MIPS_INS_ASUB_S_H:
		case MIPS_INS_ASUB_U_H:
		case MIPS_INS_AVE_S_H:
		case MIPS_INS_AVE_U_H:
		case MIPS_INS_AVER_S_H:
		case MIPS_INS_AVER_U_H:
		case MIPS_INS_BCLR_H:
		case MIPS_INS_BCLRI_H:
		case MIPS_INS_BSET_H:
		case MIPS_INS_BSETI_H:
		case MIPS_INS_BNEG_H:
		case MIPS_INS_BNEGI_H:
		case MIPS_INS_BINSL_H:
		case MIPS_INS_BINSLI_H:
		case MIPS_INS_BINSR_H:
		case MIPS_INS_BINSRI_H:
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
		case MIPS_INS_CLE_U_H:
		case MIPS_INS_CLEI_S_H:
		case MIPS_INS_CLEI_U_H:
		case MIPS_INS_CLT_S_H:
		case MIPS_INS_CLT_U_H:
		case MIPS_INS_CLTI_S_H:
		case MIPS_INS_CLTI_U_H:
		case MIPS_INS_MAX_S_H:
		case MIPS_INS_MIN_S_H:
		case MIPS_INS_SLL_H:
		case MIPS_INS_SRL_H:
		case MIPS_INS_SRA_H:
		case MIPS_INS_SLLI_H:
		case MIPS_INS_SRAI_H:
		case MIPS_INS_SRLI_H:
		case MIPS_INS_SRARI_H:
		case MIPS_INS_SRAR_H:
		case MIPS_INS_SRLRI_H:
		case MIPS_INS_SRLR_H:
		case MIPS_INS_SUBSUS_U_H:
		case MIPS_INS_SUBSUU_S_H:
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
		case MIPS_INS_ADDS_A_H:
		case MIPS_INS_DIV_S_H:
		case MIPS_INS_DIV_U_H:
		case MIPS_INS_MOD_S_H:
		case MIPS_INS_MOD_U_H:
		case MIPS_INS_DOTP_S_H:
		case MIPS_INS_DOTP_U_H:
		case MIPS_INS_DPADD_S_H:
		case MIPS_INS_DPADD_U_H:
		case MIPS_INS_DPSUB_S_H:
		case MIPS_INS_DPSUB_U_H:
		case MIPS_INS_HADD_S_H:
		case MIPS_INS_HADD_U_H:
		case MIPS_INS_HSUB_S_H:
		case MIPS_INS_HSUB_U_H:
		case MIPS_INS_MUL_Q_H:
		case MIPS_INS_MADD_Q_H:
		case MIPS_INS_MSUB_Q_H:
		case MIPS_INS_MADDR_Q_H:
		case MIPS_INS_MULR_Q_H:
		case MIPS_INS_MSUBR_Q_H:
		case MIPS_INS_MADDV_H:
		case MIPS_INS_MSUBV_H:
		case MIPS_INS_MAXI_S_H:
		case MIPS_INS_MAXI_U_H:
		case MIPS_INS_MINI_S_H:
		case MIPS_INS_MINI_U_H:
		case MIPS_INS_MAX_A_H:
		case MIPS_INS_MIN_A_H:
		case MIPS_INS_MAX_U_H:
		case MIPS_INS_MIN_U_H:
		case MIPS_INS_NLOC_H:
		case MIPS_INS_NLZC_H:
		case MIPS_INS_PCNT_H:
		case MIPS_INS_INSVE_H:
		case MIPS_INS_VSHF_H:
		case MIPS_INS_FEXDO_H:
		case MIPS_INS_FTQ_H:
			return 16;
		case MIPS_INS_ADDV_D:
		case MIPS_INS_ADDVI_D:
		case MIPS_INS_SUBV_D:
		case MIPS_INS_SUBVI_D:
		case MIPS_INS_ADD_A_D:
		case MIPS_INS_ASUB_S_D:
		case MIPS_INS_ASUB_U_D:
		case MIPS_INS_AVE_S_D:
		case MIPS_INS_AVE_U_D:
		case MIPS_INS_AVER_S_D:
		case MIPS_INS_AVER_U_D:
		case MIPS_INS_BCLR_D:
		case MIPS_INS_BCLRI_D:
		case MIPS_INS_BSET_D:
		case MIPS_INS_BSETI_D:
		case MIPS_INS_BNEG_D:
		case MIPS_INS_BNEGI_D:
		case MIPS_INS_BINSL_D:
		case MIPS_INS_BINSLI_D:
		case MIPS_INS_BINSR_D:
		case MIPS_INS_BINSRI_D:
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
		case MIPS_INS_CLE_U_D:
		case MIPS_INS_CLEI_S_D:
		case MIPS_INS_CLEI_U_D:
		case MIPS_INS_CLT_S_D:
		case MIPS_INS_CLT_U_D:
		case MIPS_INS_CLTI_S_D:
		case MIPS_INS_CLTI_U_D:
		case MIPS_INS_MAX_S_D:
		case MIPS_INS_MIN_S_D:
		case MIPS_INS_SLL_D:
		case MIPS_INS_SRL_D:
		case MIPS_INS_SRA_D:
		case MIPS_INS_SLLI_D:
		case MIPS_INS_SRAI_D:
		case MIPS_INS_SRLI_D:
		case MIPS_INS_SRARI_D:
		case MIPS_INS_SRAR_D:
		case MIPS_INS_SRLRI_D:
		case MIPS_INS_SRLR_D:
		case MIPS_INS_SUBSUS_U_D:
		case MIPS_INS_SUBSUU_S_D:
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
		case MIPS_INS_ADDS_A_D:
		case MIPS_INS_DIV_S_D:
		case MIPS_INS_DIV_U_D:
		case MIPS_INS_MOD_S_D:
		case MIPS_INS_MOD_U_D:
		case MIPS_INS_DOTP_S_D:
		case MIPS_INS_DOTP_U_D:
		case MIPS_INS_DPADD_S_D:
		case MIPS_INS_DPADD_U_D:
		case MIPS_INS_DPSUB_S_D:
		case MIPS_INS_DPSUB_U_D:
		case MIPS_INS_HADD_S_D:
		case MIPS_INS_HADD_U_D:
		case MIPS_INS_HSUB_S_D:
		case MIPS_INS_HSUB_U_D:
		case MIPS_INS_FCLASS_D:
		case MIPS_INS_FMADD_D:
		case MIPS_INS_FMSUB_D:
		case MIPS_INS_FEXP2_D:
		case MIPS_INS_FEXUPL_D:
		case MIPS_INS_FEXUPR_D:
		case MIPS_INS_FFINT_S_D:
		case MIPS_INS_FFINT_U_D:
		case MIPS_INS_FFQL_D:
		case MIPS_INS_FFQR_D:
		case MIPS_INS_FLOG2_D:
		case MIPS_INS_FRCP_D:
		case MIPS_INS_FRINT_D:
		case MIPS_INS_FRSQRT_D:
		case MIPS_INS_FSQRT_D:
		case MIPS_INS_FSAF_D:
		case MIPS_INS_FSEQ_D:
		case MIPS_INS_FSLE_D:
		case MIPS_INS_FSLT_D:
		case MIPS_INS_FSNE_D:
		case MIPS_INS_FSOR_D:
		case MIPS_INS_FSUEQ_D:
		case MIPS_INS_FSULE_D:
		case MIPS_INS_FSULT_D:
		case MIPS_INS_FSUNE_D:
		case MIPS_INS_FSUN_D:
		case MIPS_INS_FTINT_S_D:
		case MIPS_INS_FTINT_U_D:
		case MIPS_INS_FTRUNC_S_D:
		case MIPS_INS_FTRUNC_U_D:
		case MIPS_INS_MADDV_D:
		case MIPS_INS_MSUBV_D:
		case MIPS_INS_MAXI_S_D:
		case MIPS_INS_MAXI_U_D:
		case MIPS_INS_MINI_S_D:
		case MIPS_INS_MINI_U_D:
		case MIPS_INS_MAX_A_D:
		case MIPS_INS_MIN_A_D:
		case MIPS_INS_MAX_U_D:
		case MIPS_INS_MIN_U_D:
		case MIPS_INS_NLOC_D:
		case MIPS_INS_NLZC_D:
		case MIPS_INS_PCNT_D:
		case MIPS_INS_INSVE_D:
		case MIPS_INS_VSHF_D:
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
		llvm::IRBuilder<>& irb,
		bool isUnsigned = false)
{
	unsigned n = 128 / elemBits;
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	llvm::Value* cmp = nullptr;
	if (isUnsigned)
	{
		cmp = isMax ? irb.CreateICmpUGT(v0, v1) : irb.CreateICmpULT(v0, v1);
	}
	else
	{
		cmp = isMax ? irb.CreateICmpSGT(v0, v1) : irb.CreateICmpSLT(v0, v1);
	}
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
	Slt,
	Ule,
	Ult
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
		case MsaCmpPred::Ule:
			cmp = irb.CreateICmpULE(v0, v1);
			break;
		case MsaCmpPred::Ult:
			cmp = irb.CreateICmpULT(v0, v1);
			break;
	}
	return irb.CreateBitCast(irb.CreateSExt(cmp, vecTy), irb.getIntNTy(128));
}

llvm::Value* msaPackedFbin(
		llvm::Value* a,
		llvm::Value* b,
		llvm::Instruction::BinaryOps opc,
		bool isDouble,
		llvm::IRBuilder<>& irb)
{
	auto* vecTy = isDouble
			? llvm::FixedVectorType::get(irb.getDoubleTy(), 2)
			: llvm::FixedVectorType::get(irb.getFloatTy(), 4);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	return irb.CreateBitCast(irb.CreateBinOp(opc, v0, v1), irb.getIntNTy(128));
}

llvm::Value* msaPackedFminmax(
		llvm::Value* a,
		llvm::Value* b,
		bool isMax,
		bool isDouble,
		llvm::IRBuilder<>& irb)
{
	auto* vecTy = isDouble
			? llvm::FixedVectorType::get(irb.getDoubleTy(), 2)
			: llvm::FixedVectorType::get(irb.getFloatTy(), 4);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* cmp = isMax ? irb.CreateFCmpOGT(v0, v1) : irb.CreateFCmpOLT(v0, v1);
	return irb.CreateBitCast(irb.CreateSelect(cmp, v0, v1), irb.getIntNTy(128));
}

llvm::Value* msaPackedFminmaxAbs(
		llvm::Value* a,
		llvm::Value* b,
		bool isMax,
		bool isDouble,
		llvm::IRBuilder<>& irb)
{
	unsigned n = isDouble ? 2 : 4;
	unsigned e = isDouble ? 64 : 32;
	auto* vecF = isDouble
			? llvm::FixedVectorType::get(irb.getDoubleTy(), n)
			: llvm::FixedVectorType::get(irb.getFloatTy(), n);
	auto* vecI = llvm::FixedVectorType::get(irb.getIntNTy(e), n);
	auto* mask = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n),
			llvm::ConstantInt::get(irb.getIntNTy(e), e == 64 ? 0x7fffffffffffffffull : 0x7fffffffull));
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecF);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecF);
	auto* a0 = irb.CreateBitCast(irb.CreateAnd(irb.CreateBitCast(v0, vecI), mask), vecF);
	auto* a1 = irb.CreateBitCast(irb.CreateAnd(irb.CreateBitCast(v1, vecI), mask), vecF);
	auto* cmp = isMax ? irb.CreateFCmpOGT(a0, a1) : irb.CreateFCmpOLT(a0, a1);
	return irb.CreateBitCast(irb.CreateSelect(cmp, v0, v1), irb.getIntNTy(128));
}

llvm::Value* msaPackedFcmp(
		llvm::Value* a,
		llvm::Value* b,
		llvm::CmpInst::Predicate pred,
		bool isDouble,
		llvm::IRBuilder<>& irb)
{
	unsigned n = isDouble ? 2 : 4;
	auto* fTy = isDouble
			? llvm::FixedVectorType::get(irb.getDoubleTy(), n)
			: llvm::FixedVectorType::get(irb.getFloatTy(), n);
	auto* iTy = llvm::FixedVectorType::get(irb.getIntNTy(isDouble ? 64 : 32), n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), fTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), fTy);
	auto* cmp = irb.CreateFCmp(pred, v0, v1);
	auto* ones = llvm::Constant::getAllOnesValue(iTy);
	auto* z = llvm::Constant::getNullValue(iTy);
	return irb.CreateBitCast(irb.CreateSelect(cmp, ones, z), irb.getIntNTy(128));
}

llvm::Value* msaPackedDotp(
		llvm::Value* ws,
		llvm::Value* wt,
		llvm::Value* acc,
		unsigned destBits,
		bool isSigned,
		int accOp,
		llvm::IRBuilder<>& irb)
{
	unsigned srcBits = destBits / 2;
	unsigned srcN = 128 / srcBits;
	unsigned destN = 128 / destBits;
	auto* srcTy = llvm::FixedVectorType::get(irb.getIntNTy(srcBits), srcN);
	auto* destTy = llvm::FixedVectorType::get(irb.getIntNTy(destBits), destN);
	auto* sv = irb.CreateBitCast(msaToI128(ws, irb), srcTy);
	auto* tv = irb.CreateBitCast(msaToI128(wt, irb), srcTy);
	std::vector<int> even;
	std::vector<int> odd;
	even.reserve(destN);
	odd.reserve(destN);
	for (unsigned i = 0; i < destN; ++i)
	{
		even.push_back(static_cast<int>(2 * i));
		odd.push_back(static_cast<int>(2 * i + 1));
	}
	auto* poison = llvm::PoisonValue::get(srcTy);
	auto* se = irb.CreateShuffleVector(sv, poison, even);
	auto* so = irb.CreateShuffleVector(sv, poison, odd);
	auto* te = irb.CreateShuffleVector(tv, poison, even);
	auto* to = irb.CreateShuffleVector(tv, poison, odd);
	auto* seW = isSigned ? irb.CreateSExt(se, destTy) : irb.CreateZExt(se, destTy);
	auto* soW = isSigned ? irb.CreateSExt(so, destTy) : irb.CreateZExt(so, destTy);
	auto* teW = isSigned ? irb.CreateSExt(te, destTy) : irb.CreateZExt(te, destTy);
	auto* toW = isSigned ? irb.CreateSExt(to, destTy) : irb.CreateZExt(to, destTy);
	auto* prod = irb.CreateAdd(irb.CreateMul(seW, teW), irb.CreateMul(soW, toW));
	if (accOp != 0 && acc != nullptr)
	{
		auto* dv = irb.CreateBitCast(msaToI128(acc, irb), destTy);
		prod = accOp > 0 ? irb.CreateAdd(dv, prod) : irb.CreateSub(dv, prod);
	}
	return irb.CreateBitCast(prod, irb.getIntNTy(128));
}

llvm::Value* msaPackedHadd(
		llvm::Value* ws,
		llvm::Value* wt,
		unsigned destBits,
		bool isSigned,
		bool isSub,
		llvm::IRBuilder<>& irb)
{
	unsigned srcBits = destBits / 2;
	unsigned srcN = 128 / srcBits;
	unsigned destN = 128 / destBits;
	auto* srcTy = llvm::FixedVectorType::get(irb.getIntNTy(srcBits), srcN);
	auto* destTy = llvm::FixedVectorType::get(irb.getIntNTy(destBits), destN);
	auto* sv = irb.CreateBitCast(msaToI128(ws, irb), srcTy);
	auto* tv = irb.CreateBitCast(msaToI128(wt, irb), srcTy);
	std::vector<int> even;
	std::vector<int> odd;
	even.reserve(destN);
	odd.reserve(destN);
	for (unsigned i = 0; i < destN; ++i)
	{
		even.push_back(static_cast<int>(2 * i));
		odd.push_back(static_cast<int>(2 * i + 1));
	}
	auto* poison = llvm::PoisonValue::get(srcTy);
	auto* wo = irb.CreateShuffleVector(sv, poison, odd);
	auto* te = irb.CreateShuffleVector(tv, poison, even);
	auto* woW = isSigned ? irb.CreateSExt(wo, destTy) : irb.CreateZExt(wo, destTy);
	auto* teW = isSigned ? irb.CreateSExt(te, destTy) : irb.CreateZExt(te, destTy);
	auto* r = isSub ? irb.CreateSub(woW, teW) : irb.CreateAdd(woW, teW);
	return irb.CreateBitCast(r, irb.getIntNTy(128));
}

llvm::Value* msaPackedDiv(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		bool isSigned,
		bool isMod,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(elemTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* one = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(elemTy, 1));
	auto* z = llvm::Constant::getNullValue(vecTy);
	auto* safe = irb.CreateSelect(irb.CreateICmpEQ(v1, z), one, v1);
	if (isSigned)
	{
		auto* smin = llvm::ConstantVector::getSplat(
				llvm::ElementCount::getFixed(n),
				llvm::ConstantInt::get(elemTy, llvm::APInt::getSignedMinValue(elemBits)));
		auto* neg1 = llvm::ConstantVector::getSplat(
				llvm::ElementCount::getFixed(n), llvm::ConstantInt::getSigned(elemTy, -1));
		auto* ov = irb.CreateAnd(irb.CreateICmpEQ(v0, smin), irb.CreateICmpEQ(v1, neg1));
		safe = irb.CreateSelect(ov, one, safe);
	}
	llvm::Value* r = isMod
			? (isSigned ? irb.CreateSRem(v0, safe) : irb.CreateURem(v0, safe))
			: (isSigned ? irb.CreateSDiv(v0, safe) : irb.CreateUDiv(v0, safe));
	return irb.CreateBitCast(r, irb.getIntNTy(128));
}

llvm::Value* msaPackedMulQ(
		llvm::Value* wd,
		llvm::Value* ws,
		llvm::Value* wt,
		unsigned elemBits,
		int accOp,
		llvm::IRBuilder<>& irb,
		bool round = false)
{
	unsigned n = 128 / elemBits;
	unsigned wideBits = elemBits * 2;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* wideTy = irb.getIntNTy(wideBits);
	auto* vecTy = llvm::FixedVectorType::get(elemTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(ws, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(wt, irb), vecTy);
	auto* a = irb.CreateSExt(v0, wideVec);
	auto* b = irb.CreateSExt(v1, wideVec);
	auto* prod = irb.CreateMul(a, b);
	auto* shAmt = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, elemBits - 1));
	if (accOp != 0)
	{
		auto* acc = irb.CreateSExt(irb.CreateBitCast(msaToI128(wd, irb), vecTy), wideVec);
		auto* accQ = irb.CreateShl(acc, shAmt);
		prod = accOp > 0 ? irb.CreateAdd(accQ, prod) : irb.CreateSub(accQ, prod);
	}
	if (round && elemBits > 1)
	{
		auto* rnd = llvm::ConstantVector::getSplat(
				llvm::ElementCount::getFixed(n),
				llvm::ConstantInt::get(wideTy, 1ull << (elemBits - 2)));
		prod = irb.CreateAdd(prod, rnd);
	}
	llvm::APInt loA = llvm::APInt::getSignedMinValue(elemBits).sext(wideBits).shl(elemBits - 1);
	llvm::APInt hiA = llvm::APInt::getSignedMaxValue(elemBits).sext(wideBits).shl(elemBits - 1);
	auto* loC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, loA));
	auto* hiC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, hiA));
	auto* c = irb.CreateSelect(irb.CreateICmpSLT(prod, loC), loC, prod);
	c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
	return irb.CreateBitCast(irb.CreateTrunc(irb.CreateAShr(c, shAmt), vecTy), irb.getIntNTy(128));
}

llvm::Value* msaPackedAddsA(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	unsigned wideBits = elemBits * 2;
	auto* laneTy = irb.getIntNTy(elemBits);
	auto* wideTy = irb.getIntNTy(wideBits);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* aw = irb.CreateSExt(v0, wideVec);
	auto* bw = irb.CreateSExt(v1, wideVec);
	auto* z = llvm::Constant::getNullValue(wideVec);
	auto* aAbs = irb.CreateSelect(irb.CreateICmpSLT(aw, z), irb.CreateSub(z, aw), aw);
	auto* bAbs = irb.CreateSelect(irb.CreateICmpSLT(bw, z), irb.CreateSub(z, bw), bw);
	auto* sum = irb.CreateAdd(aAbs, bAbs);
	llvm::APInt loA = llvm::APInt::getSignedMinValue(elemBits).sext(wideBits);
	llvm::APInt hiA = llvm::APInt::getSignedMaxValue(elemBits).sext(wideBits);
	auto* loC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, loA));
	auto* hiC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, hiA));
	auto* c = irb.CreateSelect(irb.CreateICmpSLT(sum, loC), loC, sum);
	c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
	return irb.CreateBitCast(irb.CreateTrunc(c, vecTy), irb.getIntNTy(128));
}

llvm::Value* msaPackedMaxA(
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
	auto* z = llvm::Constant::getNullValue(vecTy);
	auto* a0 = irb.CreateSelect(irb.CreateICmpSLT(v0, z), irb.CreateSub(z, v0), v0);
	auto* a1 = irb.CreateSelect(irb.CreateICmpSLT(v1, z), irb.CreateSub(z, v1), v1);
	auto* cmp = isMax ? irb.CreateICmpUGE(a0, a1) : irb.CreateICmpULE(a0, a1);
	return irb.CreateBitCast(irb.CreateSelect(cmp, v0, v1), irb.getIntNTy(128));
}

llvm::Value* msaPackedPopcount(
		llvm::Value* a,
		unsigned elemBits,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* ty = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(ty, n);
	auto* x = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto splat = [&](uint64_t v) {
		return llvm::ConstantVector::getSplat(
				llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(ty, v));
	};
	auto* m55 = splat(elemBits == 8 ? 0x55ull : elemBits == 16 ? 0x5555ull
			: elemBits == 32 ? 0x55555555ull : 0x5555555555555555ull);
	auto* m33 = splat(elemBits == 8 ? 0x33ull : elemBits == 16 ? 0x3333ull
			: elemBits == 32 ? 0x33333333ull : 0x3333333333333333ull);
	auto* m0f = splat(elemBits == 8 ? 0x0full : elemBits == 16 ? 0x0f0full
			: elemBits == 32 ? 0x0f0f0f0full : 0x0f0f0f0f0f0f0f0full);
	x = irb.CreateSub(x, irb.CreateAnd(irb.CreateLShr(x, splat(1)), m55));
	x = irb.CreateAdd(irb.CreateAnd(x, m33), irb.CreateAnd(irb.CreateLShr(x, splat(2)), m33));
	x = irb.CreateAnd(irb.CreateAdd(x, irb.CreateLShr(x, splat(4))), m0f);
	if (elemBits >= 16)
	{
		x = irb.CreateAdd(x, irb.CreateLShr(x, splat(8)));
	}
	if (elemBits >= 32)
	{
		x = irb.CreateAdd(x, irb.CreateLShr(x, splat(16)));
	}
	if (elemBits >= 64)
	{
		x = irb.CreateAdd(x, irb.CreateLShr(x, splat(32)));
	}
	x = irb.CreateAnd(x, splat((2ull * elemBits) - 1));
	return irb.CreateBitCast(x, irb.getIntNTy(128));
}

llvm::Value* msaPackedCtlz(
		llvm::Value* a,
		unsigned elemBits,
		bool ones,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* ty = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(ty, n);
	auto* x = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	if (ones)
	{
		x = irb.CreateNot(x);
	}
	for (unsigned s = 1; s < elemBits; s *= 2)
	{
		auto* sh = llvm::ConstantVector::getSplat(
				llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(ty, s));
		x = irb.CreateOr(x, irb.CreateLShr(x, sh));
	}
	auto* onesCnt = irb.CreateBitCast(msaPackedPopcount(irb.CreateBitCast(x, irb.getIntNTy(128)), elemBits, irb), vecTy);
	auto* width = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(ty, elemBits));
	return irb.CreateBitCast(irb.CreateSub(width, onesCnt), irb.getIntNTy(128));
}

llvm::Value* msaPackedFclass(llvm::Value* a, bool isDouble, llvm::IRBuilder<>& irb)
{
	unsigned e = isDouble ? 64 : 32;
	unsigned n = 128 / e;
	auto* iTy = irb.getIntNTy(e);
	auto* vecI = llvm::FixedVectorType::get(iTy, n);
	auto* bits = irb.CreateBitCast(msaToI128(a, irb), vecI);
	llvm::Value* out = llvm::Constant::getNullValue(vecI);
	for (unsigned i = 0; i < n; ++i)
	{
		auto* b = irb.CreateExtractElement(bits, i);
		out = irb.CreateInsertElement(out, fpClassBits(b, e, irb), i);
	}
	return irb.CreateBitCast(out, irb.getIntNTy(128));
}

llvm::Value* msaPackedVshf(
		llvm::Value* wd,
		llvm::Value* ws,
		llvm::Value* wt,
		unsigned elemBits,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* ty = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(ty, n);
	auto* d = irb.CreateBitCast(msaToI128(wd, irb), vecTy);
	auto* s = irb.CreateBitCast(msaToI128(ws, irb), vecTy);
	auto* t = irb.CreateBitCast(msaToI128(wt, irb), vecTy);
	llvm::Value* out = llvm::Constant::getNullValue(vecTy);
	auto* nC = llvm::ConstantInt::get(ty, n);
	auto* twoN = llvm::ConstantInt::get(ty, 2 * n);
	auto* z = llvm::ConstantInt::get(ty, 0);
	for (unsigned i = 0; i < n; ++i)
	{
		auto* k = irb.CreateExtractElement(t, i);
		auto* idx = irb.CreateURem(k, nC);
		auto* fromD = irb.CreateExtractElement(d, idx);
		auto* fromS = irb.CreateExtractElement(s, idx);
		auto* val = irb.CreateSelect(irb.CreateICmpUGE(k, nC), fromS, fromD);
		val = irb.CreateSelect(irb.CreateICmpUGE(k, twoN), z, val);
		out = irb.CreateInsertElement(out, val, i);
	}
	return irb.CreateBitCast(out, irb.getIntNTy(128));
}

llvm::Value* msaSplatImm(
		uint64_t imm,
		unsigned elemBits,
		bool isSigned,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	llvm::APInt bits = isSigned
			? llvm::APInt(5, imm, true).sext(elemBits)
			: llvm::APInt(elemBits, imm);
	auto* lane = llvm::ConstantInt::get(irb.getIntNTy(elemBits), bits);
	return irb.CreateBitCast(irb.CreateVectorSplat(n, lane), irb.getIntNTy(128));
}

enum class MsaBitOp
{
	Clr,
	Set,
	Neg
};

llvm::Value* msaPackedBit(
		llvm::Value* ws,
		llvm::Value* idx,
		unsigned elemBits,
		MsaBitOp op,
		bool idxIsImm,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(elemTy, n);
	auto* v = irb.CreateBitCast(msaToI128(ws, irb), vecTy);
	auto* maskAmt = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n),
			llvm::ConstantInt::get(elemTy, elemBits - 1));
	llvm::Value* amt = nullptr;
	if (idxIsImm)
	{
		amt = irb.CreateAnd(
				irb.CreateBitCast(msaSplatImm(
						llvm::cast<llvm::ConstantInt>(idx)->getZExtValue(),
						elemBits,
						false,
						irb), vecTy),
				maskAmt);
	}
	else
	{
		amt = irb.CreateAnd(irb.CreateBitCast(msaToI128(idx, irb), vecTy), maskAmt);
	}
	auto* one = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(elemTy, 1));
	auto* bit = irb.CreateShl(one, amt);
	llvm::Value* res = nullptr;
	switch (op)
	{
		case MsaBitOp::Clr:
			res = irb.CreateAnd(v, irb.CreateNot(bit));
			break;
		case MsaBitOp::Set:
			res = irb.CreateOr(v, bit);
			break;
		case MsaBitOp::Neg:
			res = irb.CreateXor(v, bit);
			break;
	}
	return irb.CreateBitCast(res, irb.getIntNTy(128));
}

llvm::Value* msaPackedBins(
		llvm::Value* wd,
		llvm::Value* ws,
		llvm::Value* idx,
		unsigned elemBits,
		bool fromLeft,
		bool idxIsImm,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(elemTy, n);
	auto* d = irb.CreateBitCast(msaToI128(wd, irb), vecTy);
	auto* s = irb.CreateBitCast(msaToI128(ws, irb), vecTy);
	auto* maskAmt = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n),
			llvm::ConstantInt::get(elemTy, elemBits - 1));
	llvm::Value* raw = nullptr;
	if (idxIsImm)
	{
		raw = irb.CreateBitCast(msaSplatImm(
				llvm::cast<llvm::ConstantInt>(idx)->getZExtValue(),
				elemBits,
				false,
				irb), vecTy);
	}
	else
	{
		raw = irb.CreateBitCast(msaToI128(idx, irb), vecTy);
	}
	auto* nbits = irb.CreateAdd(
			irb.CreateAnd(raw, maskAmt),
			llvm::ConstantVector::getSplat(
					llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(elemTy, 1)));
	auto* full = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(elemTy, elemBits));
	auto* ones = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(elemTy, -1));
	auto* isFull = irb.CreateICmpEQ(nbits, full);
	auto* safeN = irb.CreateSelect(
			isFull,
			llvm::ConstantVector::getSplat(
					llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(elemTy, 0)),
			nbits);
	llvm::Value* maskLow = nullptr;
	if (fromLeft)
	{
		maskLow = irb.CreateSelect(isFull, llvm::Constant::getNullValue(vecTy), irb.CreateLShr(ones, safeN));
	}
	else
	{
		maskLow = irb.CreateSelect(isFull, ones, irb.CreateNot(irb.CreateShl(ones, safeN)));
	}
	llvm::Value* fromWs = fromLeft ? irb.CreateAnd(s, irb.CreateNot(maskLow))
									: irb.CreateAnd(s, maskLow);
	llvm::Value* fromWd = fromLeft ? irb.CreateAnd(d, maskLow)
									: irb.CreateAnd(d, irb.CreateNot(maskLow));
	return irb.CreateBitCast(irb.CreateOr(fromWs, fromWd), irb.getIntNTy(128));
}

llvm::Value* msaPackedAbsAdd(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* vecTy = llvm::FixedVectorType::get(elemTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* z = llvm::Constant::getNullValue(vecTy);
	auto* aAbs = irb.CreateSelect(irb.CreateICmpSLT(v0, z), irb.CreateSub(z, v0), v0);
	auto* bAbs = irb.CreateSelect(irb.CreateICmpSLT(v1, z), irb.CreateSub(z, v1), v1);
	return irb.CreateBitCast(irb.CreateAdd(aAbs, bAbs), irb.getIntNTy(128));
}

llvm::Value* msaPackedAsub(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		bool isSigned,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* laneTy = irb.getIntNTy(elemBits);
	auto* wideTy = irb.getIntNTy(elemBits * 2);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* aw = isSigned ? irb.CreateSExt(v0, wideVec) : irb.CreateZExt(v0, wideVec);
	auto* bw = isSigned ? irb.CreateSExt(v1, wideVec) : irb.CreateZExt(v1, wideVec);
	auto* diff = irb.CreateSub(aw, bw);
	auto* z = llvm::Constant::getNullValue(wideVec);
	auto* abs = irb.CreateSelect(irb.CreateICmpSLT(diff, z), irb.CreateSub(z, diff), diff);
	return irb.CreateBitCast(irb.CreateTrunc(abs, vecTy), irb.getIntNTy(128));
}

llvm::Value* msaPackedAve(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		bool isSigned,
		bool round,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	auto* laneTy = irb.getIntNTy(elemBits);
	auto* wideTy = irb.getIntNTy(elemBits * 2);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* aw = isSigned ? irb.CreateSExt(v0, wideVec) : irb.CreateZExt(v0, wideVec);
	auto* bw = isSigned ? irb.CreateSExt(v1, wideVec) : irb.CreateZExt(v1, wideVec);
	auto* sum = irb.CreateAdd(aw, bw);
	if (round)
	{
		sum = irb.CreateAdd(sum, llvm::ConstantVector::getSplat(
				llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, 1)));
	}
	auto* one = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, 1));
	auto* avg = isSigned ? irb.CreateAShr(sum, one) : irb.CreateLShr(sum, one);
	return irb.CreateBitCast(irb.CreateTrunc(avg, vecTy), irb.getIntNTy(128));
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

llvm::Value* msaPackedShiftRnd(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		bool arith,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	unsigned wideBits = elemBits * 2;
	auto* elemTy = irb.getIntNTy(elemBits);
	auto* wideTy = irb.getIntNTy(wideBits);
	auto* vecTy = llvm::FixedVectorType::get(elemTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* amt = irb.CreateAnd(
			v1,
			llvm::ConstantVector::getSplat(
					llvm::ElementCount::getFixed(n),
					llvm::ConstantInt::get(elemTy, elemBits - 1)));
	auto* src = arith ? irb.CreateSExt(v0, wideVec) : irb.CreateZExt(v0, wideVec);
	auto* sh = irb.CreateZExt(amt, wideVec);
	auto* one = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, 1));
	auto* rnd = irb.CreateLShr(irb.CreateShl(one, sh), one);
	auto* sum = irb.CreateAdd(src, rnd);
	auto* shR = arith ? irb.CreateAShr(sum, sh) : irb.CreateLShr(sum, sh);
	return irb.CreateBitCast(irb.CreateTrunc(shR, vecTy), irb.getIntNTy(128));
}

llvm::Value* msaPackedSatMix(
		llvm::Value* a,
		llvm::Value* b,
		unsigned elemBits,
		bool susU,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 128 / elemBits;
	unsigned wideBits = elemBits * 2;
	auto* laneTy = irb.getIntNTy(elemBits);
	auto* wideTy = irb.getIntNTy(wideBits);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* wideVec = llvm::FixedVectorType::get(wideTy, n);
	auto* v0 = irb.CreateBitCast(msaToI128(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(msaToI128(b, irb), vecTy);
	auto* aw = irb.CreateZExt(v0, wideVec);
	auto* bw = susU ? irb.CreateSExt(v1, wideVec) : irb.CreateZExt(v1, wideVec);
	auto* diff = irb.CreateSub(aw, bw);
	llvm::APInt loA = susU ? llvm::APInt::getZero(wideBits)
			: llvm::APInt::getSignedMinValue(elemBits).sext(wideBits);
	llvm::APInt hiA = susU ? llvm::APInt::getMaxValue(elemBits).zext(wideBits)
			: llvm::APInt::getSignedMaxValue(elemBits).sext(wideBits);
	auto* loC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, loA));
	auto* hiC = llvm::ConstantVector::getSplat(
			llvm::ElementCount::getFixed(n), llvm::ConstantInt::get(wideTy, hiA));
	auto* c = irb.CreateSelect(irb.CreateICmpSLT(diff, loC), loC, diff);
	c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
	return irb.CreateBitCast(irb.CreateTrunc(c, vecTy), irb.getIntNTy(128));
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
	if (i->id == MIPS_INS_SUBSUS_U_B || i->id == MIPS_INS_SUBSUS_U_H
			|| i->id == MIPS_INS_SUBSUS_U_W || i->id == MIPS_INS_SUBSUS_U_D
			|| i->id == MIPS_INS_SUBSUU_S_B || i->id == MIPS_INS_SUBSUU_S_H
			|| i->id == MIPS_INS_SUBSUU_S_W || i->id == MIPS_INS_SUBSUU_S_D)
	{
		bool susU = i->id == MIPS_INS_SUBSUS_U_B || i->id == MIPS_INS_SUBSUS_U_H
				|| i->id == MIPS_INS_SUBSUS_U_W || i->id == MIPS_INS_SUBSUS_U_D;
		storeOp(
				mi->operands[0],
				msaPackedSatMix(op1, op2, msaLaneBits(i->id), susU, irb),
				irb,
				eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
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

	unsigned elemBits = msaLaneBits(i->id);
	bool isUnsigned = i->id == MIPS_INS_MAX_U_B || i->id == MIPS_INS_MAX_U_H
			|| i->id == MIPS_INS_MAX_U_W || i->id == MIPS_INS_MAX_U_D
			|| i->id == MIPS_INS_MAXI_U_B || i->id == MIPS_INS_MAXI_U_H
			|| i->id == MIPS_INS_MAXI_U_W || i->id == MIPS_INS_MAXI_U_D;
	bool isAbs = i->id == MIPS_INS_MAX_A_B || i->id == MIPS_INS_MAX_A_H
			|| i->id == MIPS_INS_MAX_A_W || i->id == MIPS_INS_MAX_A_D;
	bool isImm = i->id == MIPS_INS_MAXI_S_B || i->id == MIPS_INS_MAXI_S_H
			|| i->id == MIPS_INS_MAXI_S_W || i->id == MIPS_INS_MAXI_S_D
			|| i->id == MIPS_INS_MAXI_U_B || i->id == MIPS_INS_MAXI_U_H
			|| i->id == MIPS_INS_MAXI_U_W || i->id == MIPS_INS_MAXI_U_D;
	op1 = loadOp(mi->operands[1], irb);
	if (isImm)
	{
		if (mi->op_count != 3 || mi->operands[2].type != MIPS_OP_IMM)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
		op2 = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm), elemBits, !isUnsigned, irb);
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	auto* r = isAbs
			? msaPackedMaxA(op1, op2, elemBits, true, irb)
			: msaPackedSminMax(op1, op2, elemBits, true, irb, isUnsigned);
	storeOp(mi->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_MIN_S_B/H/W/D — signed per-lane min.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaMins(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	bool isUnsigned = i->id == MIPS_INS_MIN_U_B || i->id == MIPS_INS_MIN_U_H
			|| i->id == MIPS_INS_MIN_U_W || i->id == MIPS_INS_MIN_U_D
			|| i->id == MIPS_INS_MINI_U_B || i->id == MIPS_INS_MINI_U_H
			|| i->id == MIPS_INS_MINI_U_W || i->id == MIPS_INS_MINI_U_D;
	bool isAbs = i->id == MIPS_INS_MIN_A_B || i->id == MIPS_INS_MIN_A_H
			|| i->id == MIPS_INS_MIN_A_W || i->id == MIPS_INS_MIN_A_D;
	bool isImm = i->id == MIPS_INS_MINI_S_B || i->id == MIPS_INS_MINI_S_H
			|| i->id == MIPS_INS_MINI_S_W || i->id == MIPS_INS_MINI_S_D
			|| i->id == MIPS_INS_MINI_U_B || i->id == MIPS_INS_MINI_U_H
			|| i->id == MIPS_INS_MINI_U_W || i->id == MIPS_INS_MINI_U_D;
	op1 = loadOp(mi->operands[1], irb);
	if (isImm)
	{
		if (mi->op_count != 3 || mi->operands[2].type != MIPS_OP_IMM)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
		op2 = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm), elemBits, !isUnsigned, irb);
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	auto* r = isAbs
			? msaPackedMaxA(op1, op2, elemBits, false, irb)
			: msaPackedSminMax(op1, op2, elemBits, false, irb, isUnsigned);
	storeOp(mi->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SLL_B/H/W/D — per-lane left shift (x86 PSLLW analog, but
 * MSA takes a per-element count modulo the lane width).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSll(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	if (mi->op_count == 3 && mi->operands[2].type == MIPS_OP_IMM)
	{
		op1 = loadOp(mi->operands[1], irb);
		op2 = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm), elemBits, false, irb);
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	storeOp(
			mi->operands[0],
			msaPackedShift(op1, op2, elemBits, llvm::Instruction::Shl, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SRL_B/H/W/D / SRLI_* — per-lane logical right shift.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSrl(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	if (mi->op_count == 3 && mi->operands[2].type == MIPS_OP_IMM)
	{
		op1 = loadOp(mi->operands[1], irb);
		op2 = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm), elemBits, false, irb);
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	storeOp(
			mi->operands[0],
			msaPackedShift(op1, op2, elemBits, llvm::Instruction::LShr, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_SRA_B/H/W/D / SRAI_* — per-lane arithmetic right shift.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaSra(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	if (mi->op_count == 3 && mi->operands[2].type == MIPS_OP_IMM)
	{
		op1 = loadOp(mi->operands[1], irb);
		op2 = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm), elemBits, false, irb);
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	storeOp(
			mi->operands[0],
			msaPackedShift(op1, op2, elemBits, llvm::Instruction::AShr, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * SRAR/SRARI / SRLR/SRLRI — rounding right shift (add 1<<(sa-1) when sa!=0).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaRndShift(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	bool arith = i->id == MIPS_INS_SRAR_B || i->id == MIPS_INS_SRAR_H
			|| i->id == MIPS_INS_SRAR_W || i->id == MIPS_INS_SRAR_D
			|| i->id == MIPS_INS_SRARI_B || i->id == MIPS_INS_SRARI_H
			|| i->id == MIPS_INS_SRARI_W || i->id == MIPS_INS_SRARI_D;
	if (mi->op_count == 3 && mi->operands[2].type == MIPS_OP_IMM)
	{
		op1 = loadOp(mi->operands[1], irb);
		op2 = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm), elemBits, false, irb);
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	storeOp(
			mi->operands[0],
			msaPackedShiftRnd(op1, op2, elemBits, arith, irb),
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
	bool imm = i->id == MIPS_INS_CEQI_B
			|| i->id == MIPS_INS_CEQI_H
			|| i->id == MIPS_INS_CEQI_W
			|| i->id == MIPS_INS_CEQI_D
			|| i->id == MIPS_INS_CLEI_S_B
			|| i->id == MIPS_INS_CLEI_S_H
			|| i->id == MIPS_INS_CLEI_S_W
			|| i->id == MIPS_INS_CLEI_S_D
			|| i->id == MIPS_INS_CLEI_U_B
			|| i->id == MIPS_INS_CLEI_U_H
			|| i->id == MIPS_INS_CLEI_U_W
			|| i->id == MIPS_INS_CLEI_U_D
			|| i->id == MIPS_INS_CLTI_S_B
			|| i->id == MIPS_INS_CLTI_S_H
			|| i->id == MIPS_INS_CLTI_S_W
			|| i->id == MIPS_INS_CLTI_S_D
			|| i->id == MIPS_INS_CLTI_U_B
			|| i->id == MIPS_INS_CLTI_U_H
			|| i->id == MIPS_INS_CLTI_U_W
			|| i->id == MIPS_INS_CLTI_U_D;
	bool immUnsigned = i->id == MIPS_INS_CLEI_U_B
			|| i->id == MIPS_INS_CLEI_U_H
			|| i->id == MIPS_INS_CLEI_U_W
			|| i->id == MIPS_INS_CLEI_U_D
			|| i->id == MIPS_INS_CLTI_U_B
			|| i->id == MIPS_INS_CLTI_U_H
			|| i->id == MIPS_INS_CLTI_U_W
			|| i->id == MIPS_INS_CLTI_U_D;
	if (imm)
	{
		if (mi->op_count != 3 || mi->operands[2].type != MIPS_OP_IMM)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
		op1 = loadOp(mi->operands[1], irb);
		op2 = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm), elemBits, !immUnsigned, irb);
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
		case MIPS_INS_CLEI_S_B:
		case MIPS_INS_CLEI_S_H:
		case MIPS_INS_CLEI_S_W:
		case MIPS_INS_CLEI_S_D:
			pred = MsaCmpPred::Sle;
			break;
		case MIPS_INS_CLT_S_B:
		case MIPS_INS_CLT_S_H:
		case MIPS_INS_CLT_S_W:
		case MIPS_INS_CLT_S_D:
		case MIPS_INS_CLTI_S_B:
		case MIPS_INS_CLTI_S_H:
		case MIPS_INS_CLTI_S_W:
		case MIPS_INS_CLTI_S_D:
			pred = MsaCmpPred::Slt;
			break;
		case MIPS_INS_CLE_U_B:
		case MIPS_INS_CLE_U_H:
		case MIPS_INS_CLE_U_W:
		case MIPS_INS_CLE_U_D:
		case MIPS_INS_CLEI_U_B:
		case MIPS_INS_CLEI_U_H:
		case MIPS_INS_CLEI_U_W:
		case MIPS_INS_CLEI_U_D:
			pred = MsaCmpPred::Ule;
			break;
		case MIPS_INS_CLT_U_B:
		case MIPS_INS_CLT_U_H:
		case MIPS_INS_CLT_U_W:
		case MIPS_INS_CLT_U_D:
		case MIPS_INS_CLTI_U_B:
		case MIPS_INS_CLTI_U_H:
		case MIPS_INS_CLTI_U_W:
		case MIPS_INS_CLTI_U_D:
			pred = MsaCmpPred::Ult;
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
 * MIPS_INS_FADD_W/D / FSUB_W/D / FMUL_W/D / FDIV_W/D — MSA packed
 * single/double (x86 ADDPS/ADDPD). FMAX_W/D / FMIN_W/D are ordered
 * compare plus select, not llvm.maxnum. Scalar COP1 FADD_D keeps
 * translateAdd when no W register is present.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFbin(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	bool isDouble = i->id == MIPS_INS_FADD_D
			|| i->id == MIPS_INS_FSUB_D
			|| i->id == MIPS_INS_FMUL_D
			|| i->id == MIPS_INS_FDIV_D
			|| i->id == MIPS_INS_FMAX_D
			|| i->id == MIPS_INS_FMIN_D
			|| i->id == MIPS_INS_FMAX_A_D
			|| i->id == MIPS_INS_FMIN_A_D;
	if (i->id == MIPS_INS_FMAX_A_W || i->id == MIPS_INS_FMIN_A_W
			|| i->id == MIPS_INS_FMAX_A_D || i->id == MIPS_INS_FMIN_A_D)
	{
		storeOp(
				mi->operands[0],
				msaPackedFminmaxAbs(op1, op2, i->id == MIPS_INS_FMAX_A_W || i->id == MIPS_INS_FMAX_A_D, isDouble, irb),
				irb,
				eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	if (i->id == MIPS_INS_FMAX_W || i->id == MIPS_INS_FMIN_W
			|| i->id == MIPS_INS_FMAX_D || i->id == MIPS_INS_FMIN_D)
	{
		storeOp(
				mi->operands[0],
				msaPackedFminmax(op1, op2, i->id == MIPS_INS_FMAX_W || i->id == MIPS_INS_FMAX_D, isDouble, irb),
				irb,
				eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	llvm::Instruction::BinaryOps opc = llvm::Instruction::FAdd;
	switch (i->id)
	{
		case MIPS_INS_FSUB_W:
		case MIPS_INS_FSUB_D:
			opc = llvm::Instruction::FSub;
			break;
		case MIPS_INS_FMUL_W:
		case MIPS_INS_FMUL_D:
			opc = llvm::Instruction::FMul;
			break;
		case MIPS_INS_FDIV_W:
		case MIPS_INS_FDIV_D:
			opc = llvm::Instruction::FDiv;
			break;
		default:
			opc = llvm::Instruction::FAdd;
			break;
	}
	storeOp(
			mi->operands[0],
			msaPackedFbin(op1, op2, opc, isDouble, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ADDVI_B/H/W/D / SUBVI_* — lane add/sub of a zero-extended 5-bit
 * immediate (x86 PADDD + splat).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaAddvi(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	if (mi->operands[2].type != MIPS_OP_IMM)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned elemBits = msaLaneBits(i->id);
	op1 = loadOp(mi->operands[1], irb);
	op2 = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm), elemBits, false, irb);
	bool isSub = i->id == MIPS_INS_SUBVI_B
			|| i->id == MIPS_INS_SUBVI_H
			|| i->id == MIPS_INS_SUBVI_W
			|| i->id == MIPS_INS_SUBVI_D;
	storeOp(
			mi->operands[0],
			msaPackedBinOp(op1, op2, elemBits, isSub ? llvm::Instruction::Sub : llvm::Instruction::Add, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_BCLR/BSET/BNEG and *I — per-lane bit clear/set/toggle
 * (x86 PANDN / POR / PXOR of a one-hot mask). Index is wt[i] % bits
 * or an immediate.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaBit(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	bool isImm = i->id == MIPS_INS_BCLRI_B
			|| i->id == MIPS_INS_BCLRI_H
			|| i->id == MIPS_INS_BCLRI_W
			|| i->id == MIPS_INS_BCLRI_D
			|| i->id == MIPS_INS_BSETI_B
			|| i->id == MIPS_INS_BSETI_H
			|| i->id == MIPS_INS_BSETI_W
			|| i->id == MIPS_INS_BSETI_D
			|| i->id == MIPS_INS_BNEGI_B
			|| i->id == MIPS_INS_BNEGI_H
			|| i->id == MIPS_INS_BNEGI_W
			|| i->id == MIPS_INS_BNEGI_D;
	MsaBitOp op = MsaBitOp::Clr;
	if (i->id == MIPS_INS_BSET_B || i->id == MIPS_INS_BSET_H
			|| i->id == MIPS_INS_BSET_W || i->id == MIPS_INS_BSET_D
			|| i->id == MIPS_INS_BSETI_B || i->id == MIPS_INS_BSETI_H
			|| i->id == MIPS_INS_BSETI_W || i->id == MIPS_INS_BSETI_D)
	{
		op = MsaBitOp::Set;
	}
	else if (i->id == MIPS_INS_BNEG_B || i->id == MIPS_INS_BNEG_H
			|| i->id == MIPS_INS_BNEG_W || i->id == MIPS_INS_BNEG_D
			|| i->id == MIPS_INS_BNEGI_B || i->id == MIPS_INS_BNEGI_H
			|| i->id == MIPS_INS_BNEGI_W || i->id == MIPS_INS_BNEGI_D)
	{
		op = MsaBitOp::Neg;
	}

	if (isImm)
	{
		if (mi->op_count != 3 || mi->operands[2].type != MIPS_OP_IMM)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
		op1 = loadOp(mi->operands[1], irb);
		auto* idx = llvm::ConstantInt::get(irb.getInt64Ty(), static_cast<uint64_t>(mi->operands[2].imm));
		storeOp(mi->operands[0], msaPackedBit(op1, idx, elemBits, op, true, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(mi->operands[0], msaPackedBit(op1, op2, elemBits, op, false, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_BINSL/BINSR and *I — insert the leftmost / rightmost
 * (wt % bits)+1 bits of ws into wd (bit-field insert).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaBins(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	bool fromLeft = i->id == MIPS_INS_BINSL_B
			|| i->id == MIPS_INS_BINSL_H
			|| i->id == MIPS_INS_BINSL_W
			|| i->id == MIPS_INS_BINSL_D
			|| i->id == MIPS_INS_BINSLI_B
			|| i->id == MIPS_INS_BINSLI_H
			|| i->id == MIPS_INS_BINSLI_W
			|| i->id == MIPS_INS_BINSLI_D;
	bool isImm = i->id == MIPS_INS_BINSLI_B
			|| i->id == MIPS_INS_BINSLI_H
			|| i->id == MIPS_INS_BINSLI_W
			|| i->id == MIPS_INS_BINSLI_D
			|| i->id == MIPS_INS_BINSRI_B
			|| i->id == MIPS_INS_BINSRI_H
			|| i->id == MIPS_INS_BINSRI_W
			|| i->id == MIPS_INS_BINSRI_D;

	op0 = loadOp(mi->operands[0], irb);
	op1 = loadOp(mi->operands[1], irb);
	if (isImm)
	{
		if (mi->operands[2].type != MIPS_OP_IMM)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
		auto* idx = llvm::ConstantInt::get(irb.getInt64Ty(), static_cast<uint64_t>(mi->operands[2].imm));
		storeOp(mi->operands[0], msaPackedBins(op0, op1, idx, elemBits, fromLeft, true, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	op2 = loadOp(mi->operands[2], irb);
	storeOp(mi->operands[0], msaPackedBins(op0, op1, op2, elemBits, fromLeft, false, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_BMNZ_V / BMZ_V / BMNZI_B / BMZI_B — bitwise move if (not) zero.
 * BMNZ: wd = (ws AND wt) OR (wd AND NOT wt). BMZ inverts the mask.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaBmnz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	op0 = loadOp(mi->operands[0], irb);
	op1 = loadOp(mi->operands[1], irb);
	auto* wd = msaToI128(op0, irb);
	auto* ws = msaToI128(op1, irb);
	llvm::Value* mask = nullptr;
	if (i->id == MIPS_INS_BMNZI_B || i->id == MIPS_INS_BMZI_B)
	{
		if (mi->operands[2].type != MIPS_OP_IMM)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
		mask = msaSplatImm(static_cast<uint64_t>(mi->operands[2].imm) & 0xffu, 8, false, irb);
	}
	else
	{
		mask = msaToI128(loadOp(mi->operands[2], irb), irb);
	}

	bool ifZero = i->id == MIPS_INS_BMZ_V || i->id == MIPS_INS_BMZI_B;
	if (ifZero)
	{
		mask = irb.CreateNot(mask);
	}
	auto* res = irb.CreateOr(irb.CreateAnd(ws, mask), irb.CreateAnd(wd, irb.CreateNot(mask)));
	storeOp(mi->operands[0], res, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ADD_A_B/H/W/D — |ws| + |wt| per lane (wrapping).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaAddA(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedAbsAdd(op1, op2, msaLaneBits(i->id), irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ASUB_S/U_* — |ws - wt| (signed or unsigned absolute difference).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaAsub(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	bool isSigned = i->id == MIPS_INS_ASUB_S_B
			|| i->id == MIPS_INS_ASUB_S_H
			|| i->id == MIPS_INS_ASUB_S_W
			|| i->id == MIPS_INS_ASUB_S_D;
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedAsub(op1, op2, msaLaneBits(i->id), isSigned, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_AVE_S/U_* / AVER_S/U_* — (ws+wt)>>1, AVER adds 1 first
 * (ARM VHADD / VRHADD).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaAve(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	bool isSigned = i->id == MIPS_INS_AVE_S_B
			|| i->id == MIPS_INS_AVE_S_H
			|| i->id == MIPS_INS_AVE_S_W
			|| i->id == MIPS_INS_AVE_S_D
			|| i->id == MIPS_INS_AVER_S_B
			|| i->id == MIPS_INS_AVER_S_H
			|| i->id == MIPS_INS_AVER_S_W
			|| i->id == MIPS_INS_AVER_S_D;
	bool round = i->id == MIPS_INS_AVER_S_B
			|| i->id == MIPS_INS_AVER_S_H
			|| i->id == MIPS_INS_AVER_S_W
			|| i->id == MIPS_INS_AVER_S_D
			|| i->id == MIPS_INS_AVER_U_B
			|| i->id == MIPS_INS_AVER_U_H
			|| i->id == MIPS_INS_AVER_U_W
			|| i->id == MIPS_INS_AVER_U_D;
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedAve(op1, op2, msaLaneBits(i->id), isSigned, round, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_FCEQ_W/D / FCLT_* / FCLE_* / FCNE_* / FCAF_* / FCOR_* /
 * FCUN_* / FCUEQ_* / FCULE_* / FCULT_* / FCUNE_* — packed FP compare
 * to an all-1s/0s mask (x86 CMPPS).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFcmp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	bool isDouble = i->id == MIPS_INS_FCEQ_D
			|| i->id == MIPS_INS_FCLT_D
			|| i->id == MIPS_INS_FCLE_D
			|| i->id == MIPS_INS_FCNE_D
			|| i->id == MIPS_INS_FCAF_D
			|| i->id == MIPS_INS_FCOR_D
			|| i->id == MIPS_INS_FCUN_D
			|| i->id == MIPS_INS_FCUEQ_D
			|| i->id == MIPS_INS_FCULE_D
			|| i->id == MIPS_INS_FCULT_D
			|| i->id == MIPS_INS_FCUNE_D
			|| i->id == MIPS_INS_FSEQ_D
			|| i->id == MIPS_INS_FSLT_D
			|| i->id == MIPS_INS_FSLE_D
			|| i->id == MIPS_INS_FSNE_D
			|| i->id == MIPS_INS_FSAF_D
			|| i->id == MIPS_INS_FSOR_D
			|| i->id == MIPS_INS_FSUN_D
			|| i->id == MIPS_INS_FSUEQ_D
			|| i->id == MIPS_INS_FSULE_D
			|| i->id == MIPS_INS_FSULT_D
			|| i->id == MIPS_INS_FSUNE_D;
	llvm::CmpInst::Predicate pred = llvm::CmpInst::FCMP_OEQ;
	switch (i->id)
	{
		case MIPS_INS_FCLT_W:
		case MIPS_INS_FCLT_D:
		case MIPS_INS_FSLT_W:
		case MIPS_INS_FSLT_D:
			pred = llvm::CmpInst::FCMP_OLT;
			break;
		case MIPS_INS_FCLE_W:
		case MIPS_INS_FCLE_D:
		case MIPS_INS_FSLE_W:
		case MIPS_INS_FSLE_D:
			pred = llvm::CmpInst::FCMP_OLE;
			break;
		case MIPS_INS_FCNE_W:
		case MIPS_INS_FCNE_D:
		case MIPS_INS_FSNE_W:
		case MIPS_INS_FSNE_D:
			pred = llvm::CmpInst::FCMP_ONE;
			break;
		case MIPS_INS_FCAF_W:
		case MIPS_INS_FCAF_D:
		case MIPS_INS_FSAF_W:
		case MIPS_INS_FSAF_D:
			pred = llvm::CmpInst::FCMP_FALSE;
			break;
		case MIPS_INS_FCOR_W:
		case MIPS_INS_FCOR_D:
		case MIPS_INS_FSOR_W:
		case MIPS_INS_FSOR_D:
			pred = llvm::CmpInst::FCMP_ORD;
			break;
		case MIPS_INS_FCUN_W:
		case MIPS_INS_FCUN_D:
		case MIPS_INS_FSUN_W:
		case MIPS_INS_FSUN_D:
			pred = llvm::CmpInst::FCMP_UNO;
			break;
		case MIPS_INS_FCUEQ_W:
		case MIPS_INS_FCUEQ_D:
		case MIPS_INS_FSUEQ_W:
		case MIPS_INS_FSUEQ_D:
			pred = llvm::CmpInst::FCMP_UEQ;
			break;
		case MIPS_INS_FCULE_W:
		case MIPS_INS_FCULE_D:
		case MIPS_INS_FSULE_W:
		case MIPS_INS_FSULE_D:
			pred = llvm::CmpInst::FCMP_ULE;
			break;
		case MIPS_INS_FCULT_W:
		case MIPS_INS_FCULT_D:
		case MIPS_INS_FSULT_W:
		case MIPS_INS_FSULT_D:
			pred = llvm::CmpInst::FCMP_ULT;
			break;
		case MIPS_INS_FCUNE_W:
		case MIPS_INS_FCUNE_D:
		case MIPS_INS_FSUNE_W:
		case MIPS_INS_FSUNE_D:
			pred = llvm::CmpInst::FCMP_UNE;
			break;
		default:
			pred = llvm::CmpInst::FCMP_OEQ;
			break;
	}
	storeOp(
			mi->operands[0],
			msaPackedFcmp(op1, op2, pred, isDouble, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_DOTP_S/U_H/W/D — even/odd pair products summed into a
 * double-width lane. DPADD accumulates, DPSUB subtracts from wd.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaDotp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	bool isSigned = i->id == MIPS_INS_DOTP_S_H
			|| i->id == MIPS_INS_DOTP_S_W
			|| i->id == MIPS_INS_DOTP_S_D
			|| i->id == MIPS_INS_DPADD_S_H
			|| i->id == MIPS_INS_DPADD_S_W
			|| i->id == MIPS_INS_DPADD_S_D
			|| i->id == MIPS_INS_DPSUB_S_H
			|| i->id == MIPS_INS_DPSUB_S_W
			|| i->id == MIPS_INS_DPSUB_S_D;
	int accOp = 0;
	if (i->id == MIPS_INS_DPADD_S_H || i->id == MIPS_INS_DPADD_S_W || i->id == MIPS_INS_DPADD_S_D
			|| i->id == MIPS_INS_DPADD_U_H || i->id == MIPS_INS_DPADD_U_W || i->id == MIPS_INS_DPADD_U_D)
	{
		accOp = 1;
	}
	else if (i->id == MIPS_INS_DPSUB_S_H || i->id == MIPS_INS_DPSUB_S_W || i->id == MIPS_INS_DPSUB_S_D
			|| i->id == MIPS_INS_DPSUB_U_H || i->id == MIPS_INS_DPSUB_U_W || i->id == MIPS_INS_DPSUB_U_D)
	{
		accOp = -1;
	}
	op0 = accOp != 0 ? loadOp(mi->operands[0], irb) : nullptr;
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedDotp(op1, op2, op0, msaLaneBits(i->id), isSigned, accOp, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_HADD_S/U_H/W/D / HSUB_* — pairwise add/sub of adjacent
 * source lanes into a double-width dest (x86 PHADDW-class).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaHadd(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	bool isSigned = i->id == MIPS_INS_HADD_S_H
			|| i->id == MIPS_INS_HADD_S_W
			|| i->id == MIPS_INS_HADD_S_D
			|| i->id == MIPS_INS_HSUB_S_H
			|| i->id == MIPS_INS_HSUB_S_W
			|| i->id == MIPS_INS_HSUB_S_D;
	bool isSub = i->id == MIPS_INS_HSUB_S_H
			|| i->id == MIPS_INS_HSUB_S_W
			|| i->id == MIPS_INS_HSUB_S_D
			|| i->id == MIPS_INS_HSUB_U_H
			|| i->id == MIPS_INS_HSUB_U_W
			|| i->id == MIPS_INS_HSUB_U_D;
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedHadd(op1, op2, msaLaneBits(i->id), isSigned, isSub, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_DIV_S/U_B/H/W/D / MOD_S/U_* — per-lane integer divide/remainder.
 * Zero divisor is substituted (MIPS UNPREDICTABLE, not LLVM UB).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaDiv(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	bool isSigned = i->id == MIPS_INS_DIV_S_B
			|| i->id == MIPS_INS_DIV_S_H
			|| i->id == MIPS_INS_DIV_S_W
			|| i->id == MIPS_INS_DIV_S_D
			|| i->id == MIPS_INS_MOD_S_B
			|| i->id == MIPS_INS_MOD_S_H
			|| i->id == MIPS_INS_MOD_S_W
			|| i->id == MIPS_INS_MOD_S_D;
	bool isMod = i->id == MIPS_INS_MOD_S_B
			|| i->id == MIPS_INS_MOD_S_H
			|| i->id == MIPS_INS_MOD_S_W
			|| i->id == MIPS_INS_MOD_S_D
			|| i->id == MIPS_INS_MOD_U_B
			|| i->id == MIPS_INS_MOD_U_H
			|| i->id == MIPS_INS_MOD_U_W
			|| i->id == MIPS_INS_MOD_U_D;
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedDiv(op1, op2, msaLaneBits(i->id), isSigned, isMod, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_MOVE_V — 128-bit W-register copy.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaMoveV(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	storeOp(mi->operands[0], msaToI128(op1, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_BNZ_B/H/W/D/V / BZ_* — branch if any/all MSA lanes are
 * nonzero/zero. All element sizes are equivalent to a 128-bit
 * compare-to-zero plus the existing cond-branch helper.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaBz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op0 = loadOp(mi->operands[0], irb);
	op1 = loadOp(mi->operands[1], irb);
	auto* v = msaToI128(op0, irb);
	auto* z = llvm::ConstantInt::get(v->getType(), 0);
	bool isBnz = i->id == MIPS_INS_BNZ_B
			|| i->id == MIPS_INS_BNZ_H
			|| i->id == MIPS_INS_BNZ_W
			|| i->id == MIPS_INS_BNZ_D
			|| i->id == MIPS_INS_BNZ_V;
	auto* cond = isBnz ? irb.CreateICmpNE(v, z) : irb.CreateICmpEQ(v, z);
	generateCondBranchFunctionCall(irb, cond, op1);
}

/**
 * MIPS_INS_MUL_Q_H/W / MADD_Q_* / MSUB_Q_* — saturating Q15/Q31
 * fractional multiply (and accumulate).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaMulQ(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	int accOp = 0;
	if (i->id == MIPS_INS_MADD_Q_H || i->id == MIPS_INS_MADD_Q_W
			|| i->id == MIPS_INS_MADDR_Q_H || i->id == MIPS_INS_MADDR_Q_W)
	{
		accOp = 1;
	}
	else if (i->id == MIPS_INS_MSUB_Q_H || i->id == MIPS_INS_MSUB_Q_W
			|| i->id == MIPS_INS_MSUBR_Q_H || i->id == MIPS_INS_MSUBR_Q_W)
	{
		accOp = -1;
	}
	bool round = i->id == MIPS_INS_MADDR_Q_H || i->id == MIPS_INS_MADDR_Q_W
			|| i->id == MIPS_INS_MULR_Q_H || i->id == MIPS_INS_MULR_Q_W
			|| i->id == MIPS_INS_MSUBR_Q_H || i->id == MIPS_INS_MSUBR_Q_W;
	op0 = accOp != 0 ? loadOp(mi->operands[0], irb) : nullptr;
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedMulQ(op0, op1, op2, msaLaneBits(i->id), accOp, irb, round),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_ADDS_A_B/H/W/D — saturating |ws| + |wt|.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaAddsA(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(
			mi->operands[0],
			msaPackedAddsA(op1, op2, msaLaneBits(i->id), irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_FMADD_W/D / FMSUB_* — packed fused acc ± ws*wt.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFmadd(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	bool isDouble = i->id == MIPS_INS_FMADD_D || i->id == MIPS_INS_FMSUB_D;
	auto* vecTy = isDouble
			? llvm::FixedVectorType::get(irb.getDoubleTy(), 2)
			: llvm::FixedVectorType::get(irb.getFloatTy(), 4);
	op0 = loadOp(mi->operands[0], irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* acc = irb.CreateBitCast(msaToI128(op0, irb), vecTy);
	auto* a = irb.CreateBitCast(msaToI128(op1, irb), vecTy);
	auto* b = irb.CreateBitCast(msaToI128(op2, irb), vecTy);
	auto* prod = irb.CreateFMul(a, b);
	auto* r = (i->id == MIPS_INS_FMSUB_W || i->id == MIPS_INS_FMSUB_D)
			? irb.CreateFSub(acc, prod)
			: irb.CreateFAdd(acc, prod);
	storeOp(mi->operands[0], irb.CreateBitCast(r, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * Unary MSA FP: FCLASS bitmask, FSQRT/FRCP/FLOG2/FEXP2/FRINT/FRSQRT.
 * FCLASS bits: 0 SNAN, 1 QNAN, 2 -Inf, 3 -norm, 4 -sub, 5 -0,
 * 6 +0, 7 +sub, 8 +norm, 9 +Inf. FEXP2 is ws * 2^wt.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFunary(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	bool isExp2 = i->id == MIPS_INS_FEXP2_W || i->id == MIPS_INS_FEXP2_D;
	if (isExp2)
	{
		EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
	}
	else
	{
		EXPECT_IS_BINARY(i, mi, irb);
	}

	bool isDouble = i->id == MIPS_INS_FCLASS_D || i->id == MIPS_INS_FSQRT_D
			|| i->id == MIPS_INS_FRCP_D || i->id == MIPS_INS_FLOG2_D
			|| i->id == MIPS_INS_FEXP2_D || i->id == MIPS_INS_FRINT_D
			|| i->id == MIPS_INS_FRSQRT_D;
	op1 = loadOp(mi->operands[1], irb);
	if (i->id == MIPS_INS_FCLASS_W || i->id == MIPS_INS_FCLASS_D)
	{
		storeOp(mi->operands[0], msaPackedFclass(op1, isDouble, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	unsigned n = isDouble ? 2 : 4;
	auto* vecTy = isDouble
			? llvm::FixedVectorType::get(irb.getDoubleTy(), n)
			: llvm::FixedVectorType::get(irb.getFloatTy(), n);
	auto* v = irb.CreateBitCast(msaToI128(op1, irb), vecTy);
	auto* one = llvm::ConstantFP::get(isDouble ? irb.getDoubleTy() : irb.getFloatTy(), 1.0);
	auto recip = [&](llvm::Value* src) {
		llvm::Value* out = llvm::Constant::getNullValue(vecTy);
		for (unsigned k = 0; k < n; ++k)
		{
			out = irb.CreateInsertElement(
					out,
					irb.CreateFDiv(one, irb.CreateExtractElement(src, uint64_t(k))),
					uint64_t(k));
		}
		return out;
	};
	llvm::Value* r = v;
	if (i->id == MIPS_INS_FRCP_W || i->id == MIPS_INS_FRCP_D)
	{
		r = recip(v);
	}
	else if (i->id == MIPS_INS_FRINT_W || i->id == MIPS_INS_FRINT_D)
	{
		auto* iv = isDouble
				? irb.CreateFPToSI(v, llvm::FixedVectorType::get(irb.getInt64Ty(), 2))
				: irb.CreateFPToSI(v, llvm::FixedVectorType::get(irb.getInt32Ty(), 4));
		r = irb.CreateSIToFP(iv, vecTy);
	}
	else if (i->id == MIPS_INS_FEXP2_W || i->id == MIPS_INS_FEXP2_D)
	{
		op2 = loadOp(mi->operands[2], irb);
		auto* t = irb.CreateBitCast(msaToI128(op2, irb), vecTy);
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::exp2, {vecTy});
		r = irb.CreateFMul(v, irb.CreateCall(f, {t}));
	}
	else
	{
		llvm::Intrinsic::ID iid = llvm::Intrinsic::sqrt;
		if (i->id == MIPS_INS_FLOG2_W || i->id == MIPS_INS_FLOG2_D)
		{
			iid = llvm::Intrinsic::log2;
		}
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid, {vecTy});
		r = irb.CreateCall(f, {v});
		if (i->id == MIPS_INS_FRSQRT_W || i->id == MIPS_INS_FRSQRT_D)
		{
			r = recip(r);
		}
	}
	storeOp(mi->operands[0], irb.CreateBitCast(r, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * FEXDO / FEXUPL / FEXUPR — pack or expand FP lanes.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFex(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	bool isDo = i->id == MIPS_INS_FEXDO_H || i->id == MIPS_INS_FEXDO_W;
	if (isDo)
	{
		EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		if (i->id == MIPS_INS_FEXDO_W)
		{
			auto* srcTy = llvm::FixedVectorType::get(irb.getDoubleTy(), 2);
			auto* dstTy = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
			auto* ws = irb.CreateBitCast(msaToI128(op1, irb), srcTy);
			auto* wt = irb.CreateBitCast(msaToI128(op2, irb), srcTy);
			llvm::Value* out = llvm::Constant::getNullValue(dstTy);
			out = irb.CreateInsertElement(out, irb.CreateFPTrunc(irb.CreateExtractElement(ws, uint64_t(0)), irb.getFloatTy()), uint64_t(0));
			out = irb.CreateInsertElement(out, irb.CreateFPTrunc(irb.CreateExtractElement(wt, uint64_t(0)), irb.getFloatTy()), uint64_t(1));
			out = irb.CreateInsertElement(out, irb.CreateFPTrunc(irb.CreateExtractElement(ws, uint64_t(1)), irb.getFloatTy()), uint64_t(2));
			out = irb.CreateInsertElement(out, irb.CreateFPTrunc(irb.CreateExtractElement(wt, uint64_t(1)), irb.getFloatTy()), uint64_t(3));
			storeOp(mi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		auto* srcTy = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
		auto* dstTy = llvm::FixedVectorType::get(irb.getHalfTy(), 8);
		auto* ws = irb.CreateBitCast(msaToI128(op1, irb), srcTy);
		auto* wt = irb.CreateBitCast(msaToI128(op2, irb), srcTy);
		llvm::Value* out = llvm::Constant::getNullValue(dstTy);
		for (unsigned k = 0; k < 4; ++k)
		{
			out = irb.CreateInsertElement(out, irb.CreateFPTrunc(irb.CreateExtractElement(ws, k), irb.getHalfTy()), 2 * k);
			out = irb.CreateInsertElement(out, irb.CreateFPTrunc(irb.CreateExtractElement(wt, k), irb.getHalfTy()), 2 * k + 1);
		}
		storeOp(mi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	EXPECT_IS_BINARY(i, mi, irb);
	op1 = loadOp(mi->operands[1], irb);
	bool left = i->id == MIPS_INS_FEXUPL_W || i->id == MIPS_INS_FEXUPL_D;
	if (i->id == MIPS_INS_FEXUPL_D || i->id == MIPS_INS_FEXUPR_D)
	{
		auto* srcTy = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
		auto* dstTy = llvm::FixedVectorType::get(irb.getDoubleTy(), 2);
		auto* ws = irb.CreateBitCast(msaToI128(op1, irb), srcTy);
		unsigned off = left ? 2 : 0;
		llvm::Value* out = llvm::Constant::getNullValue(dstTy);
		out = irb.CreateInsertElement(out, irb.CreateFPExt(irb.CreateExtractElement(ws, uint64_t(off)), irb.getDoubleTy()), uint64_t(0));
		out = irb.CreateInsertElement(out, irb.CreateFPExt(irb.CreateExtractElement(ws, uint64_t(off + 1)), irb.getDoubleTy()), uint64_t(1));
		storeOp(mi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	auto* srcTy = llvm::FixedVectorType::get(irb.getHalfTy(), 8);
	auto* dstTy = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
	auto* ws = irb.CreateBitCast(msaToI128(op1, irb), srcTy);
	unsigned off = left ? 4 : 0;
	llvm::Value* out = llvm::Constant::getNullValue(dstTy);
	for (unsigned k = 0; k < 4; ++k)
	{
		out = irb.CreateInsertElement(out, irb.CreateFPExt(irb.CreateExtractElement(ws, off + k), irb.getFloatTy()), k);
	}
	storeOp(mi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * FFINT_S/U / FFQL / FFQR — integer or Q-format to float.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFfint(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	bool isDouble = i->id == MIPS_INS_FFINT_S_D || i->id == MIPS_INS_FFINT_U_D
			|| i->id == MIPS_INS_FFQL_D || i->id == MIPS_INS_FFQR_D;
	bool isU = i->id == MIPS_INS_FFINT_U_W || i->id == MIPS_INS_FFINT_U_D;
	if (i->id == MIPS_INS_FFINT_S_W || i->id == MIPS_INS_FFINT_U_W
			|| i->id == MIPS_INS_FFINT_S_D || i->id == MIPS_INS_FFINT_U_D)
	{
		unsigned n = isDouble ? 2 : 4;
		unsigned e = isDouble ? 64 : 32;
		auto* iTy = llvm::FixedVectorType::get(irb.getIntNTy(e), n);
		auto* fTy = isDouble
				? llvm::FixedVectorType::get(irb.getDoubleTy(), n)
				: llvm::FixedVectorType::get(irb.getFloatTy(), n);
		auto* v = irb.CreateBitCast(msaToI128(op1, irb), iTy);
		auto* r = isU ? irb.CreateUIToFP(v, fTy) : irb.CreateSIToFP(v, fTy);
		storeOp(mi->operands[0], irb.CreateBitCast(r, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	bool left = i->id == MIPS_INS_FFQL_W || i->id == MIPS_INS_FFQL_D;
	if (isDouble)
	{
		auto* srcTy = llvm::FixedVectorType::get(irb.getInt32Ty(), 4);
		auto* dstTy = llvm::FixedVectorType::get(irb.getDoubleTy(), 2);
		auto* ws = irb.CreateBitCast(msaToI128(op1, irb), srcTy);
		unsigned off = left ? 2 : 0;
		auto* scale = llvm::ConstantFP::get(irb.getDoubleTy(), 1.0 / 2147483648.0);
		llvm::Value* out = llvm::Constant::getNullValue(dstTy);
		for (unsigned k = 0; k < 2; ++k)
		{
			auto* q = irb.CreateSExt(irb.CreateExtractElement(ws, off + k), irb.getInt64Ty());
			out = irb.CreateInsertElement(out, irb.CreateFMul(irb.CreateSIToFP(q, irb.getDoubleTy()), scale), k);
		}
		storeOp(mi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	auto* srcTy = llvm::FixedVectorType::get(irb.getInt16Ty(), 8);
	auto* dstTy = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
	auto* ws = irb.CreateBitCast(msaToI128(op1, irb), srcTy);
	unsigned off = left ? 4 : 0;
	auto* scale = llvm::ConstantFP::get(irb.getFloatTy(), 1.0 / 32768.0);
	llvm::Value* out = llvm::Constant::getNullValue(dstTy);
	for (unsigned k = 0; k < 4; ++k)
	{
		auto* q = irb.CreateSExt(irb.CreateExtractElement(ws, off + k), irb.getInt32Ty());
		out = irb.CreateInsertElement(out, irb.CreateFMul(irb.CreateSIToFP(q, irb.getFloatTy()), scale), k);
	}
	storeOp(mi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * FTINT / FTRUNC / FTQ — float to integer or Q-format (toward zero).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaFtint(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	bool isTq = i->id == MIPS_INS_FTQ_H || i->id == MIPS_INS_FTQ_W;
	if (isTq)
	{
		EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		if (i->id == MIPS_INS_FTQ_W)
		{
			auto* srcTy = llvm::FixedVectorType::get(irb.getDoubleTy(), 2);
			auto* dstTy = llvm::FixedVectorType::get(irb.getInt32Ty(), 4);
			auto* scale = llvm::ConstantFP::get(irb.getDoubleTy(), 2147483648.0);
			auto* lo = llvm::ConstantInt::getSigned(irb.getInt64Ty(), -2147483647LL - 1);
			auto* hi = llvm::ConstantInt::getSigned(irb.getInt64Ty(), 2147483647LL);
			auto cvt = [&](llvm::Value* src, unsigned lane) {
				auto* w = irb.CreateFPToSI(irb.CreateFMul(irb.CreateExtractElement(src, lane), scale), irb.getInt64Ty());
				w = irb.CreateSelect(irb.CreateICmpSLT(w, lo), lo, w);
				w = irb.CreateSelect(irb.CreateICmpSGT(w, hi), hi, w);
				return irb.CreateTrunc(w, irb.getInt32Ty());
			};
			auto* ws = irb.CreateBitCast(msaToI128(op1, irb), srcTy);
			auto* wt = irb.CreateBitCast(msaToI128(op2, irb), srcTy);
			llvm::Value* out = llvm::Constant::getNullValue(dstTy);
			out = irb.CreateInsertElement(out, cvt(ws, 0), uint64_t(0));
			out = irb.CreateInsertElement(out, cvt(wt, 0), uint64_t(1));
			out = irb.CreateInsertElement(out, cvt(ws, 1), uint64_t(2));
			out = irb.CreateInsertElement(out, cvt(wt, 1), uint64_t(3));
			storeOp(mi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		auto* srcTy = llvm::FixedVectorType::get(irb.getFloatTy(), 4);
		auto* dstTy = llvm::FixedVectorType::get(irb.getInt16Ty(), 8);
		auto* scale = llvm::ConstantFP::get(irb.getFloatTy(), 32768.0);
		auto* lo = llvm::ConstantInt::getSigned(irb.getInt32Ty(), -32768);
		auto* hi = llvm::ConstantInt::getSigned(irb.getInt32Ty(), 32767);
		auto cvt = [&](llvm::Value* src, unsigned lane) {
			auto* w = irb.CreateFPToSI(irb.CreateFMul(irb.CreateExtractElement(src, lane), scale), irb.getInt32Ty());
			w = irb.CreateSelect(irb.CreateICmpSLT(w, lo), lo, w);
			w = irb.CreateSelect(irb.CreateICmpSGT(w, hi), hi, w);
			return irb.CreateTrunc(w, irb.getInt16Ty());
		};
		auto* ws = irb.CreateBitCast(msaToI128(op1, irb), srcTy);
		auto* wt = irb.CreateBitCast(msaToI128(op2, irb), srcTy);
		llvm::Value* out = llvm::Constant::getNullValue(dstTy);
		for (unsigned k = 0; k < 4; ++k)
		{
			out = irb.CreateInsertElement(out, cvt(ws, k), 2 * k);
			out = irb.CreateInsertElement(out, cvt(wt, k), 2 * k + 1);
		}
		storeOp(mi->operands[0], irb.CreateBitCast(out, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	EXPECT_IS_BINARY(i, mi, irb);
	op1 = loadOp(mi->operands[1], irb);
	bool isDouble = i->id == MIPS_INS_FTINT_S_D || i->id == MIPS_INS_FTINT_U_D
			|| i->id == MIPS_INS_FTRUNC_S_D || i->id == MIPS_INS_FTRUNC_U_D;
	bool isU = i->id == MIPS_INS_FTINT_U_W || i->id == MIPS_INS_FTINT_U_D
			|| i->id == MIPS_INS_FTRUNC_U_W || i->id == MIPS_INS_FTRUNC_U_D;
	unsigned n = isDouble ? 2 : 4;
	unsigned e = isDouble ? 64 : 32;
	auto* fTy = isDouble
			? llvm::FixedVectorType::get(irb.getDoubleTy(), n)
			: llvm::FixedVectorType::get(irb.getFloatTy(), n);
	auto* iTy = llvm::FixedVectorType::get(irb.getIntNTy(e), n);
	auto* v = irb.CreateBitCast(msaToI128(op1, irb), fTy);
	auto* r = isU ? irb.CreateFPToUI(v, iTy) : irb.CreateFPToSI(v, iTy);
	storeOp(mi->operands[0], irb.CreateBitCast(r, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MADDV / MSUBV — integer wd ± ws*wt.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaMaddv(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	unsigned elemBits = msaLaneBits(i->id);
	op0 = loadOp(mi->operands[0], irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* prod = msaPackedBinOp(op1, op2, elemBits, llvm::Instruction::Mul, irb);
	bool isSub = i->id == MIPS_INS_MSUBV_B || i->id == MIPS_INS_MSUBV_H
			|| i->id == MIPS_INS_MSUBV_W || i->id == MIPS_INS_MSUBV_D;
	storeOp(
			mi->operands[0],
			msaPackedBinOp(op0, prod, elemBits, isSub ? llvm::Instruction::Sub : llvm::Instruction::Add, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * NLOC / NLZC / PCNT — leading ones/zeros and popcount (bit-parallel).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaBitcount(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOp(mi->operands[1], irb);
	unsigned elemBits = msaLaneBits(i->id);
	llvm::Value* r = nullptr;
	if (i->id == MIPS_INS_PCNT_B || i->id == MIPS_INS_PCNT_H
			|| i->id == MIPS_INS_PCNT_W || i->id == MIPS_INS_PCNT_D)
	{
		r = msaPackedPopcount(op1, elemBits, irb);
	}
	else
	{
		bool ones = i->id == MIPS_INS_NLOC_B || i->id == MIPS_INS_NLOC_H
				|| i->id == MIPS_INS_NLOC_W || i->id == MIPS_INS_NLOC_D;
		r = msaPackedCtlz(op1, elemBits, ones, irb);
	}
	storeOp(mi->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * VSHF — 3-reg permute. Index k: wd if k<n, ws if n<=k<2n, else 0.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaVshf(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	op0 = loadOp(mi->operands[0], irb);
	op1 = loadOp(mi->operands[1], irb);
	op2 = loadOp(mi->operands[2], irb);
	storeOp(
			mi->operands[0],
			msaPackedVshf(op0, op1, op2, msaLaneBits(i->id), irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * INSVE — insert ws[0] into wd[n].
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaInsve(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	// Capstone 6: wd[n], ws[0] as W, IMM, W or W, IMM, W, IMM.
	if (mi->op_count < 3
			|| mi->operands[1].type != MIPS_OP_IMM
			|| mi->operands[2].type != MIPS_OP_REG)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned elemBits = msaLaneBits(i->id);
	unsigned n = 128 / elemBits;
	op0 = loadOp(mi->operands[0], irb);
	op2 = loadOp(mi->operands[2], irb);
	unsigned laneIdx = static_cast<unsigned>(mi->operands[1].imm) % n;
	unsigned srcIdx = 0;
	if (mi->op_count >= 4 && mi->operands[3].type == MIPS_OP_IMM)
	{
		srcIdx = static_cast<unsigned>(mi->operands[3].imm) % n;
	}
	auto* vecTy = llvm::FixedVectorType::get(irb.getIntNTy(elemBits), n);
	auto* wd = irb.CreateBitCast(msaToI128(op0, irb), vecTy);
	auto* ws = irb.CreateBitCast(msaToI128(op2, irb), vecTy);
	auto* lane = irb.CreateExtractElement(ws, static_cast<uint64_t>(srcIdx));
	storeOp(
			mi->operands[0],
			irb.CreateBitCast(irb.CreateInsertElement(wd, lane, static_cast<uint64_t>(laneIdx)), irb.getIntNTy(128)),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

/**
 * MIPS_INS_CFCMSA / CTCMSA — GPR ↔ MSACSR/MSAIR (typed i32).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMsaCsr(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	if (mi->operands[0].type != MIPS_OP_REG || mi->operands[1].type != MIPS_OP_REG)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	if (i->id == MIPS_INS_CFCMSA)
	{
		op1 = loadRegister(mi->operands[1].reg, irb);
		storeRegister(mi->operands[0].reg, op1, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	op1 = loadRegister(mi->operands[1].reg, irb);
	storeRegister(mi->operands[0].reg, op1, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
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
 * MIPS_INS_SELEQZ_S / SELEQZ_D — same on FPR bit patterns (not FCMP).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSeleqz(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::THROW);
	llvm::Value* bits = op2;
	if (bits->getType()->isFloatingPointTy())
	{
		bits = irb.CreateBitCast(bits, irb.getIntNTy(bits->getType()->getPrimitiveSizeInBits()));
	}
	else if (op1->getType()->isIntegerTy() && bits->getType() != op1->getType())
	{
		bits = irb.CreateZExtOrTrunc(bits, op1->getType());
	}
	auto* z = llvm::ConstantInt::get(bits->getType(), 0);
	auto* cond = irb.CreateICmpEQ(bits, z);
	llvm::Value* zero = op1->getType()->isFloatingPointTy()
			? static_cast<llvm::Value*>(llvm::ConstantFP::get(op1->getType(), 0.0))
			: llvm::ConstantInt::get(op1->getType(), 0);
	storeOp(mi->operands[0], irb.CreateSelect(cond, op1, zero), irb);
}

/**
 * MIPS_INS_SELNEZ — rd = (rt != 0) ? rs : 0
 * MIPS_INS_SELNEZ_S / SELNEZ_D — same on FPR bit patterns.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateSelnez(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::THROW);
	llvm::Value* bits = op2;
	if (bits->getType()->isFloatingPointTy())
	{
		bits = irb.CreateBitCast(bits, irb.getIntNTy(bits->getType()->getPrimitiveSizeInBits()));
	}
	else if (op1->getType()->isIntegerTy() && bits->getType() != op1->getType())
	{
		bits = irb.CreateZExtOrTrunc(bits, op1->getType());
	}
	auto* z = llvm::ConstantInt::get(bits->getType(), 0);
	auto* cond = irb.CreateICmpNE(bits, z);
	llvm::Value* zero = op1->getType()->isFloatingPointTy()
			? static_cast<llvm::Value*>(llvm::ConstantFP::get(op1->getType(), 0.0))
			: llvm::ConstantInt::get(op1->getType(), 0);
	storeOp(mi->operands[0], irb.CreateSelect(cond, op1, zero), irb);
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
		case MIPS_INS_LBE:
		case MIPS_INS_LB: ty = irb.getInt8Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LBU:
		case MIPS_INS_LBUE:
		case MIPS_INS_LBU16: ty = irb.getInt8Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LHE:
		case MIPS_INS_LH: ty = irb.getInt16Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LHU:
		case MIPS_INS_LHUE:
		case MIPS_INS_LHU16: ty = irb.getInt16Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case MIPS_INS_LW:
		case MIPS_INS_LWE:
		case MIPS_INS_LLE:
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
	if (i->id == MIPS_INS_LL || i->id == MIPS_INS_LLE || i->id == MIPS_INS_LLD)
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
		case MIPS_INS_SBE:
		case MIPS_INS_SB16: ty = irb.getInt8Ty(); break;
		case MIPS_INS_SH:
		case MIPS_INS_SHE:
		case MIPS_INS_SH16: ty = irb.getInt16Ty(); break;
		case MIPS_INS_SW:
		case MIPS_INS_SWE:
		case MIPS_INS_SCE:
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
 * Indexed COP1: LWXC1 / LDXC1 / LUXC1 / SWXC1 / SDXC1 / SUXC1.
 * Address is GPR[base] + GPR[index]. LUXC1/SUXC1 are the unaligned
 * double forms; IR width matches LDC1/SDC1.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateIndexedCop1(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	llvm::Value* addr = nullptr;
	if (mi->op_count >= 3
			&& mi->operands[1].type == MIPS_OP_REG
			&& mi->operands[2].type == MIPS_OP_REG)
	{
		auto* a = loadRegister(mi->operands[1].reg, irb);
		auto* b = loadRegister(mi->operands[2].reg, irb);
		if (a == nullptr || b == nullptr)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
		b = generateTypeConversion(irb, b, a->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
		addr = irb.CreateAdd(a, b);
	}
	else if (mi->operands[1].type == MIPS_OP_MEM)
	{
		addr = loadOp(mi->operands[1], irb, getDefaultType(), /*lea=*/true);
	}
	else
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	bool isStore = i->id == MIPS_INS_SWXC1
			|| i->id == MIPS_INS_SDXC1
			|| i->id == MIPS_INS_SUXC1;
	bool isDouble = i->id == MIPS_INS_LDXC1
			|| i->id == MIPS_INS_LUXC1
			|| i->id == MIPS_INS_SDXC1
			|| i->id == MIPS_INS_SUXC1;
	llvm::Type* ty = isDouble ? irb.getDoubleTy() : irb.getFloatTy();

	if (isStore)
	{
		op0 = loadOp(mi->operands[0], irb);
		if (op0->getType() != ty)
		{
			if (op0->getType()->isFloatingPointTy())
			{
				op0 = irb.CreateFPCast(op0, ty);
			}
			else
			{
				op0 = irb.CreateBitCast(
						irb.CreateZExtOrTrunc(op0, irb.getIntNTy(ty->getPrimitiveSizeInBits())),
						ty);
			}
		}
		storeIntPtr(irb, op0, addr, ty);
		return;
	}

	storeOp(mi->operands[0], loadIntPtr(irb, addr, ty), irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * MIPS_INS_BOVC / BNVC — compact R6 branch if signed rs+rt overflows
 * / does not overflow. No delay slot. Overflow is the usual
 * sign-bit test on (rs^sum) & (rt^sum), not llvm.sadd.with.overflow
 * (the emulator has no handler for that intrinsic).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateBovc(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);

	std::tie(op0, op1, op2) = loadOpTernary(mi, irb);
	if (!op0->getType()->isIntegerTy() || !op1->getType()->isIntegerTy())
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	op1 = generateTypeConversion(irb, op1, op0->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* sum = irb.CreateAdd(op0, op1);
	auto* both = irb.CreateAnd(irb.CreateXor(op0, sum), irb.CreateXor(op1, sum));
	auto* ov = irb.CreateICmpSLT(both, llvm::ConstantInt::get(op0->getType(), 0));
	auto* cond = i->id == MIPS_INS_BNVC ? irb.CreateNot(ov) : ov;
	generateCondBranchFunctionCall(irb, cond, op2);
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
 * Capstone 5 does NOT apply those offsets. It reports the encoded fields with
 * only the `msbd + 1` arithmetic done, and keeps the M/U opcode, so
 *
 *     dextm $2, $3, 0, 33     arrives as    pos 0, size 1
 *     dextu $2, $3, 32, 8     arrives as    pos 0, size 8
 *
 * Capstone 6's DecodeDEXT / DecodeDINS apply the +32 themselves and then
 * rename every form to DEXT or DINS. The switch below still adds 32 when the
 * opcode is the M or U form, and adds nothing when the id was already
 * collapsed and the immediate is the architectural one.
 *
 * Taking the Capstone 5 numbers at face value reads a one-bit field where a
 * 33-bit one was meant, and reads bit 0 where bit 32 was meant. Neither is a
 * crash and both look like a plausible bitfield, which is why the tests here
 * use the M and U forms with fields whose two readings cannot coincide.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateDoubleBitfield(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	// Four printed operands, or five when the tied destination is kept.
	if (mi->op_count != 4 && mi->op_count != 5)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

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
	case MIPS_INS_CLO:
	case MIPS_INS_POP:
	case MIPS_INS_BADDU: return true;
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
 * MIPS_INS_MFC1 — low 32 bits of the FPR into a GPR.
 * MIPS_INS_DMFC1 — full 64-bit FPR bits into a GPR (N64).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMfc1(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op1 = loadOpBinaryOp1(mi, irb);
	if (i->id == MIPS_INS_DMFC1)
	{
		if (op1->getType()->isFloatingPointTy())
		{
			unsigned e = op1->getType()->isDoubleTy() ? 64 : 32;
			op1 = irb.CreateBitCast(op1, irb.getIntNTy(e));
		}
		op1 = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
		storeOp(mi->operands[0], op1, irb);
		return;
	}
	auto* ty = op1->getType()->isDoubleTy()
			? irb.getInt64Ty()
			: irb.getInt32Ty();
	op1 = irb.CreateBitCast(op1, ty);
	op1 = irb.CreateZExtOrTrunc(op1, irb.getInt32Ty()); // even on 64-bit it takes only 0..31
	storeOp(mi->operands[0], op1, irb);
}

/**
 * MIPS_INS_MTC1 — GPR low 32 bits into a single FPR.
 * MIPS_INS_DMTC1 — GPR bits into a 64-bit FPR (N64).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateMtc1(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);

	op0 = loadOpBinaryOp0(mi, irb);
	if (i->id == MIPS_INS_DMTC1)
	{
		op0 = irb.CreateZExtOrTrunc(op0, irb.getInt64Ty());
		op0 = irb.CreateBitCast(op0, irb.getDoubleTy());
		storeOp(mi->operands[1], op0, irb);
		return;
	}
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
	// Printed order is GPR, FPR for both directions (mthc1's MC operands are
	// FPR, tied FPR, GPR, and the printer emits the GPR first). Capstone 6
	// names the FPR D0 or D0_64, not F0. Two operands, or three if the tied
	// FPR is kept.
	if (mi->op_count < 2 || mi->op_count > 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();

	cs_mips_op* gpr = nullptr;
	cs_mips_op* fpr = nullptr;
	for (unsigned k = 0; k < mi->op_count; ++k)
	{
		cs_mips_op& op = mi->operands[k];
		if (op.type != MIPS_OP_REG)
		{
			continue;
		}
		if (isCanonicalFpReg(op.reg))
		{
			if (fpr == nullptr)
			{
				fpr = &op;
			}
		}
		else if (gpr == nullptr)
		{
			gpr = &op;
		}
	}
	if (gpr == nullptr || fpr == nullptr)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	uint32_t fd = singlePrecisionToDoublePrecisionFpRegister(canonicalizeMipsReg(fpr->reg));

	if (i->id == MIPS_INS_MFHC1)
	{
		auto* bits = irb.CreateBitCast(loadRegister(fd, irb), i64);
		auto* hi = irb.CreateTrunc(irb.CreateLShr(bits, llvm::ConstantInt::get(i64, 32)), i32);
		storeOp(*gpr, hi, irb);
		return;
	}

	op0 = loadOp(*gpr, irb);
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
	if (i->id == MIPS_INS_FMUL_D && hasMsaOperand(mi))
	{
		translateMsaFbin(i, mi, irb);
		return;
	}

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
	if (hasMsaOperand(mi))
	{
		translateMsaSll(i, mi, irb);
		return;
	}

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
	if (hasMsaOperand(mi))
	{
		translateMsaSra(i, mi, irb);
		return;
	}

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
	if (hasMsaOperand(mi))
	{
		translateMsaSrl(i, mi, irb);
		return;
	}

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
	if (i->id == MIPS_INS_FSUB_D && hasMsaOperand(mi))
	{
		translateMsaFbin(i, mi, irb);
		return;
	}

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
 * MIPS_INS_CRC32B/H/W/D and CRC32CB/CH/CW/CD.
 *
 * MIPS R6 CRC ASE. LLVM 23.1.0 has no llvm.mips.crc32* (IntrinsicsMips.td /
 * IntrinsicsMips.h have no CRC entries), so this is the reflected bit-loop
 * that matches ARM CRC32 / CRC32C (poly 0xEDB88320 / 0x82F63B78). Capstone 6
 * DecodeCRC reports rt, rs, rt (tied dest/acc).
 */
void Capstone2LlvmIrTranslatorMips_impl::translateCrc32(
		cs_insn* i,
		cs_mips* mi,
		llvm::IRBuilder<>& irb)
{
	if (mi->op_count != 2 && mi->op_count != 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}

	unsigned nbits = 0;
	uint32_t polyBits = 0;
	switch (i->id)
	{
		case MIPS_INS_CRC32B: nbits = 8; polyBits = 0xEDB88320u; break;
		case MIPS_INS_CRC32H: nbits = 16; polyBits = 0xEDB88320u; break;
		case MIPS_INS_CRC32W: nbits = 32; polyBits = 0xEDB88320u; break;
		case MIPS_INS_CRC32D: nbits = 64; polyBits = 0xEDB88320u; break;
		case MIPS_INS_CRC32CB: nbits = 8; polyBits = 0x82F63B78u; break;
		case MIPS_INS_CRC32CH: nbits = 16; polyBits = 0x82F63B78u; break;
		case MIPS_INS_CRC32CW: nbits = 32; polyBits = 0x82F63B78u; break;
		case MIPS_INS_CRC32CD: nbits = 64; polyBits = 0x82F63B78u; break;
		default:
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}

	auto* i32 = irb.getInt32Ty();
	auto* wide = (nbits > 32) ? irb.getInt64Ty() : i32;
	llvm::Value* acc = irb.CreateZExtOrTrunc(loadOp(mi->operands[0], irb), wide);
	llvm::Value* data = irb.CreateZExtOrTrunc(loadOp(mi->operands[1], irb), wide);
	if (mi->op_count == 3)
	{
		acc = irb.CreateZExtOrTrunc(loadOp(mi->operands[2], irb), wide);
	}
	auto* one = llvm::ConstantInt::get(wide, 1);
	auto* poly = llvm::ConstantInt::get(wide, polyBits);
	for (unsigned n = 0; n < nbits; ++n)
	{
		auto* mix = irb.CreateTrunc(irb.CreateAnd(irb.CreateXor(acc, data), one), irb.getInt1Ty());
		acc = irb.CreateLShr(acc, one);
		data = irb.CreateLShr(data, one);
		acc = irb.CreateSelect(mix, irb.CreateXor(acc, poly), acc);
	}
	storeOp(mi->operands[0], irb.CreateZExtOrTrunc(acc, i32), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
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

static unsigned dspLaneBits(unsigned id)
{
	switch (id)
	{
		case MIPS_INS_ABSQ_S_QB:
		case MIPS_INS_ADDU_QB:
		case MIPS_INS_SUBU_QB:
		case MIPS_INS_ADDUH_QB:
		case MIPS_INS_ADDUH_R_QB:
		case MIPS_INS_ADDU_S_QB:
		case MIPS_INS_SUBUH_QB:
		case MIPS_INS_SUBUH_R_QB:
		case MIPS_INS_SUBU_S_QB:
		case MIPS_INS_CMPGDU_EQ_QB:
		case MIPS_INS_CMPGDU_LE_QB:
		case MIPS_INS_CMPGDU_LT_QB:
		case MIPS_INS_CMPGU_EQ_QB:
		case MIPS_INS_CMPGU_LE_QB:
		case MIPS_INS_CMPGU_LT_QB:
		case MIPS_INS_CMPU_EQ_QB:
		case MIPS_INS_CMPU_LE_QB:
		case MIPS_INS_CMPU_LT_QB:
		case MIPS_INS_PICK_QB:
		case MIPS_INS_REPL_QB:
		case MIPS_INS_REPLV_QB:
		case MIPS_INS_SHLL_QB:
		case MIPS_INS_SHLLV_QB:
		case MIPS_INS_SHRA_QB:
		case MIPS_INS_SHRA_R_QB:
		case MIPS_INS_SHRAV_QB:
		case MIPS_INS_SHRAV_R_QB:
		case MIPS_INS_SHRL_QB:
		case MIPS_INS_SHRLV_QB:
			return 8;
		case MIPS_INS_ABSQ_S_PH:
		case MIPS_INS_ADDQ_PH:
		case MIPS_INS_ADDQ_S_PH:
		case MIPS_INS_ADDQH_PH:
		case MIPS_INS_ADDQH_R_PH:
		case MIPS_INS_ADDU_S_PH:
		case MIPS_INS_SUBQ_PH:
		case MIPS_INS_SUBQ_S_PH:
		case MIPS_INS_SUBQH_PH:
		case MIPS_INS_SUBQH_R_PH:
		case MIPS_INS_SUBU_S_PH:
		case MIPS_INS_CMP_EQ_PH:
		case MIPS_INS_CMP_LE_PH:
		case MIPS_INS_CMP_LT_PH:
		case MIPS_INS_PICK_PH:
		case MIPS_INS_REPL_PH:
		case MIPS_INS_REPLV_PH:
		case MIPS_INS_SHLL_PH:
		case MIPS_INS_SHLL_S_PH:
		case MIPS_INS_SHLLV_PH:
		case MIPS_INS_SHLLV_S_PH:
		case MIPS_INS_SHRA_PH:
		case MIPS_INS_SHRA_R_PH:
		case MIPS_INS_SHRAV_PH:
		case MIPS_INS_SHRAV_R_PH:
		case MIPS_INS_SHRL_PH:
		case MIPS_INS_SHRLV_PH:
		case MIPS_INS_MULQ_S_PH:
		case MIPS_INS_MULQ_RS_PH:
		case MIPS_INS_ADDU_PH:
		case MIPS_INS_SUBU_PH:
		case MIPS_INS_MUL_PH:
		case MIPS_INS_MUL_S_PH:
			return 16;
		default:
			return 32;
	}
}

static llvm::Value* dspQ15Mul(
		llvm::Value* a,
		llvm::Value* b,
		unsigned bits,
		bool round,
		llvm::IRBuilder<>& irb)
{
	auto* wideTy = irb.getIntNTy(bits * 2);
	auto* aw = irb.CreateSExt(a, wideTy);
	auto* bw = irb.CreateSExt(b, wideTy);
	auto* p = irb.CreateMul(aw, bw);
	p = irb.CreateShl(p, 1);
	if (round)
	{
		p = irb.CreateAdd(p, llvm::ConstantInt::get(wideTy, 1ull << (bits - 1)));
	}
	auto* lo = llvm::ConstantInt::get(wideTy, llvm::APInt::getSignedMinValue(bits).sext(bits * 2));
	auto* hi = llvm::ConstantInt::get(wideTy, llvm::APInt::getSignedMaxValue(bits).sext(bits * 2));
	p = irb.CreateSelect(irb.CreateICmpSLT(p, lo), lo, p);
	p = irb.CreateSelect(irb.CreateICmpSGT(p, hi), hi, p);
	return irb.CreateTrunc(p, a->getType());
}

static llvm::Value* dspPackedQ15(
		llvm::Value* a,
		llvm::Value* b,
		unsigned bits,
		bool round,
		llvm::IRBuilder<>& irb)
{
	unsigned n = 32 / bits;
	auto* laneTy = irb.getIntNTy(bits);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, n);
	auto* v0 = irb.CreateBitCast(dspToI32(a, irb), vecTy);
	auto* v1 = irb.CreateBitCast(dspToI32(b, irb), vecTy);
	llvm::Value* r = llvm::PoisonValue::get(vecTy);
	for (unsigned i = 0; i < n; ++i)
	{
		auto* x = irb.CreateExtractElement(v0, i);
		auto* y = irb.CreateExtractElement(v1, i);
		r = irb.CreateInsertElement(r, dspQ15Mul(x, y, bits, round, irb), i);
	}
	return irb.CreateBitCast(r, irb.getInt32Ty());
}

static llvm::Value* dspLaneS16(llvm::Value* v, unsigned shift, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	v = irb.CreateLShr(dspToI32(v, irb), shift);
	v = irb.CreateTrunc(v, irb.getInt16Ty());
	return irb.CreateSExt(v, i32);
}

static llvm::Value* dspLaneU8(llvm::Value* v, unsigned shift, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	v = irb.CreateLShr(dspToI32(v, irb), shift);
	v = irb.CreateAnd(v, llvm::ConstantInt::get(i32, 0xff));
	return v;
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspArith(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	unsigned bits = dspLaneBits(i->id);
	if (i->id == MIPS_INS_ABSQ_S_PH || i->id == MIPS_INS_ABSQ_S_QB || i->id == MIPS_INS_ABSQ_S_W)
	{
		EXPECT_IS_BINARY(i, mi, irb);
		auto* src = loadOp(mi->operands[1], irb);
		storeOp(mi->operands[0], dspPackedAbs(src, bits, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	llvm::Value* r = nullptr;
	switch (i->id)
	{
		case MIPS_INS_ADDQ_PH:
		case MIPS_INS_SUBQ_PH:
			r = dspPackedBin(
					op1,
					op2,
					bits,
					i->id == MIPS_INS_SUBQ_PH ? llvm::Instruction::Sub : llvm::Instruction::Add,
					irb);
			break;
		case MIPS_INS_ADDU_QB:
		case MIPS_INS_ADDU_PH:
		case MIPS_INS_SUBU_QB:
		case MIPS_INS_SUBU_PH:
		case MIPS_INS_MUL_PH:
			// Modular lane arithmetic; the DSPControl overflow flags are not modelled.
			r = dspPackedBin(op1, op2, bits,
					i->id == MIPS_INS_MUL_PH ? llvm::Instruction::Mul
					: (i->id == MIPS_INS_SUBU_QB || i->id == MIPS_INS_SUBU_PH) ? llvm::Instruction::Sub
					: llvm::Instruction::Add,
					irb);
			break;
		case MIPS_INS_MUL_S_PH:
		{
			// Signed halfword products saturated to int16 (DSP R2); the
			// DSPControl ouflag bit is not modelled.
			auto* vecTy = llvm::FixedVectorType::get(irb.getInt16Ty(), 2);
			auto* wideVec = llvm::FixedVectorType::get(irb.getInt32Ty(), 2);
			auto* aw = irb.CreateSExt(irb.CreateBitCast(dspToI32(op1, irb), vecTy), wideVec);
			auto* bw = irb.CreateSExt(irb.CreateBitCast(dspToI32(op2, irb), vecTy), wideVec);
			auto* w = irb.CreateMul(aw, bw);
			auto* loC = llvm::ConstantInt::get(wideVec, -32768, true);
			auto* hiC = llvm::ConstantInt::get(wideVec, 32767, true);
			w = irb.CreateSelect(irb.CreateICmpSLT(w, loC), loC, w);
			w = irb.CreateSelect(irb.CreateICmpSGT(w, hiC), hiC, w);
			r = irb.CreateBitCast(irb.CreateTrunc(w, vecTy), irb.getInt32Ty());
			break;
		}
		case MIPS_INS_ADDQ_S_PH:
		case MIPS_INS_ADDQ_S_W:
		case MIPS_INS_SUBQ_S_PH:
		case MIPS_INS_SUBQ_S_W:
			r = dspPackedSat(op1, op2, bits, true, i->id == MIPS_INS_SUBQ_S_PH || i->id == MIPS_INS_SUBQ_S_W, irb);
			break;
		case MIPS_INS_ADDU_S_PH:
		case MIPS_INS_ADDU_S_QB:
		case MIPS_INS_SUBU_S_PH:
		case MIPS_INS_SUBU_S_QB:
			r = dspPackedSat(op1, op2, bits, false, i->id == MIPS_INS_SUBU_S_PH || i->id == MIPS_INS_SUBU_S_QB, irb);
			break;
		case MIPS_INS_ADDQH_PH:
		case MIPS_INS_ADDQH_W:
		case MIPS_INS_ADDQH_R_PH:
		case MIPS_INS_ADDQH_R_W:
		case MIPS_INS_ADDUH_QB:
		case MIPS_INS_ADDUH_R_QB:
		case MIPS_INS_SUBQH_PH:
		case MIPS_INS_SUBQH_W:
		case MIPS_INS_SUBQH_R_PH:
		case MIPS_INS_SUBQH_R_W:
		case MIPS_INS_SUBUH_QB:
		case MIPS_INS_SUBUH_R_QB:
		{
			bool isSub = i->id == MIPS_INS_SUBQH_PH || i->id == MIPS_INS_SUBQH_W
					|| i->id == MIPS_INS_SUBQH_R_PH || i->id == MIPS_INS_SUBQH_R_W
					|| i->id == MIPS_INS_SUBUH_QB || i->id == MIPS_INS_SUBUH_R_QB;
			bool rnd = i->id == MIPS_INS_ADDQH_R_PH || i->id == MIPS_INS_ADDQH_R_W
					|| i->id == MIPS_INS_ADDUH_R_QB || i->id == MIPS_INS_SUBQH_R_PH
					|| i->id == MIPS_INS_SUBQH_R_W || i->id == MIPS_INS_SUBUH_R_QB;
			bool isSigned = i->id != MIPS_INS_ADDUH_QB && i->id != MIPS_INS_ADDUH_R_QB
					&& i->id != MIPS_INS_SUBUH_QB && i->id != MIPS_INS_SUBUH_R_QB;
			r = dspPackedHalve(op1, op2, bits, isSub, rnd, isSigned, irb);
			break;
		}
		default:
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}
	storeOp(mi->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspShift(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned bits = dspLaneBits(i->id);
	bool left = false;
	bool arith = false;
	bool sat = false;
	bool round = false;
	switch (i->id)
	{
		case MIPS_INS_SHLL_PH:
		case MIPS_INS_SHLL_QB:
		case MIPS_INS_SHLLV_PH:
		case MIPS_INS_SHLLV_QB:
			left = true;
			break;
		case MIPS_INS_SHLL_S_PH:
		case MIPS_INS_SHLL_S_W:
		case MIPS_INS_SHLLV_S_PH:
		case MIPS_INS_SHLLV_S_W:
			left = true;
			sat = true;
			break;
		case MIPS_INS_SHRA_PH:
		case MIPS_INS_SHRA_QB:
		case MIPS_INS_SHRAV_PH:
		case MIPS_INS_SHRAV_QB:
			arith = true;
			break;
		case MIPS_INS_SHRA_R_PH:
		case MIPS_INS_SHRA_R_QB:
		case MIPS_INS_SHRA_R_W:
		case MIPS_INS_SHRAV_R_PH:
		case MIPS_INS_SHRAV_R_QB:
		case MIPS_INS_SHRAV_R_W:
			arith = true;
			round = true;
			break;
		case MIPS_INS_SHRL_PH:
		case MIPS_INS_SHRL_QB:
		case MIPS_INS_SHRLV_PH:
		case MIPS_INS_SHRLV_QB:
			break;
		default:
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}
	storeOp(
			mi->operands[0],
			dspPackedShift(op1, op2, bits, left, arith, sat, round, irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspCmp(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	unsigned bits = dspLaneBits(i->id);
	llvm::CmpInst::Predicate pred = llvm::CmpInst::ICMP_EQ;
	bool writeGpr = false;
	bool writeCcond = true;
	switch (i->id)
	{
		case MIPS_INS_CMP_EQ_PH:
		case MIPS_INS_CMPU_EQ_QB:
			pred = llvm::CmpInst::ICMP_EQ;
			break;
		case MIPS_INS_CMP_LE_PH:
			pred = llvm::CmpInst::ICMP_SLE;
			break;
		case MIPS_INS_CMP_LT_PH:
			pred = llvm::CmpInst::ICMP_SLT;
			break;
		case MIPS_INS_CMPU_LE_QB:
			pred = llvm::CmpInst::ICMP_ULE;
			break;
		case MIPS_INS_CMPU_LT_QB:
			pred = llvm::CmpInst::ICMP_ULT;
			break;
		case MIPS_INS_CMPGU_EQ_QB:
		case MIPS_INS_CMPGDU_EQ_QB:
			pred = llvm::CmpInst::ICMP_EQ;
			writeGpr = true;
			writeCcond = i->id == MIPS_INS_CMPGDU_EQ_QB;
			break;
		case MIPS_INS_CMPGU_LE_QB:
		case MIPS_INS_CMPGDU_LE_QB:
			pred = llvm::CmpInst::ICMP_ULE;
			writeGpr = true;
			writeCcond = i->id == MIPS_INS_CMPGDU_LE_QB;
			break;
		case MIPS_INS_CMPGU_LT_QB:
		case MIPS_INS_CMPGDU_LT_QB:
			pred = llvm::CmpInst::ICMP_ULT;
			writeGpr = true;
			writeCcond = i->id == MIPS_INS_CMPGDU_LT_QB;
			break;
		default:
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}
	auto* mask = dspCmpMask(op1, op2, bits, pred, irb);
	if (writeCcond)
	{
		storeRegister(MIPS_REG_DSPCCOND, mask, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	if (writeGpr)
	{
		storeOp(mi->operands[0], mask, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspPick(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* cond = loadRegister(MIPS_REG_DSPCCOND, irb);
	storeOp(
			mi->operands[0],
			dspPick(op1, op2, cond, dspLaneBits(i->id), irb),
			irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspRepl(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, mi, irb);
	auto* src = loadOp(mi->operands[1], irb);
	storeOp(mi->operands[0], dspRepl(src, dspLaneBits(i->id), irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspMul(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	bool round = i->id == MIPS_INS_MULQ_RS_PH || i->id == MIPS_INS_MULQ_RS_W;
	unsigned bits = dspLaneBits(i->id);
	llvm::Value* r = nullptr;
	switch (i->id)
	{
		case MIPS_INS_MULQ_S_PH:
		case MIPS_INS_MULQ_RS_PH:
		case MIPS_INS_MULQ_S_W:
		case MIPS_INS_MULQ_RS_W:
			r = dspPackedQ15(op1, op2, bits, round, irb);
			break;
		case MIPS_INS_MULEQ_S_W_PHL:
		case MIPS_INS_MULEQ_S_W_PHR:
		{
			unsigned sh = i->id == MIPS_INS_MULEQ_S_W_PHL ? 16 : 0;
			r = dspQ15Mul(dspLaneS16(op1, sh, irb), dspLaneS16(op2, sh, irb), 16, false, irb);
			r = irb.CreateShl(irb.CreateZExtOrTrunc(r, irb.getInt32Ty()), 16);
			break;
		}
		case MIPS_INS_MULEU_S_PH_QBL:
		case MIPS_INS_MULEU_S_PH_QBR:
		{
			unsigned base = i->id == MIPS_INS_MULEU_S_PH_QBL ? 16 : 0;
			auto* i32 = irb.getInt32Ty();
			auto* lo = irb.CreateMul(dspLaneU8(op1, base, irb), dspLaneU8(op2, base, irb));
			auto* hi = irb.CreateMul(dspLaneU8(op1, base + 8, irb), dspLaneU8(op2, base + 8, irb));
			auto* cap = llvm::ConstantInt::get(i32, 0xffff);
			lo = irb.CreateSelect(irb.CreateICmpUGT(lo, cap), cap, lo);
			hi = irb.CreateSelect(irb.CreateICmpUGT(hi, cap), cap, hi);
			r = irb.CreateOr(lo, irb.CreateShl(hi, 16));
			break;
		}
		default:
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}
	storeOp(mi->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspDot(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);
	auto* acc = loadOp(mi->operands[0], irb);
	auto* rs = loadOp(mi->operands[1], irb);
	auto* rt = loadOp(mi->operands[2], irb);
	auto* i64 = irb.getInt64Ty();
	acc = irb.CreateSExtOrTrunc(acc, i64);
	llvm::Value* p0 = nullptr;
	llvm::Value* p1 = nullptr;
	bool sub = false;
	switch (i->id)
	{
		case MIPS_INS_DPA_W_PH:
		case MIPS_INS_DPS_W_PH:
			p0 = irb.CreateMul(dspLaneS16(rs, 0, irb), dspLaneS16(rt, 0, irb));
			p1 = irb.CreateMul(dspLaneS16(rs, 16, irb), dspLaneS16(rt, 16, irb));
			sub = i->id == MIPS_INS_DPS_W_PH;
			break;
		case MIPS_INS_DPAX_W_PH:
		case MIPS_INS_DPSX_W_PH:
			p0 = irb.CreateMul(dspLaneS16(rs, 0, irb), dspLaneS16(rt, 16, irb));
			p1 = irb.CreateMul(dspLaneS16(rs, 16, irb), dspLaneS16(rt, 0, irb));
			sub = i->id == MIPS_INS_DPSX_W_PH;
			break;
		case MIPS_INS_DPAU_H_QBR:
		case MIPS_INS_DPSU_H_QBR:
			p0 = irb.CreateMul(dspLaneU8(rs, 0, irb), dspLaneU8(rt, 0, irb));
			p1 = irb.CreateMul(dspLaneU8(rs, 8, irb), dspLaneU8(rt, 8, irb));
			sub = i->id == MIPS_INS_DPSU_H_QBR;
			break;
		case MIPS_INS_DPAU_H_QBL:
		case MIPS_INS_DPSU_H_QBL:
			p0 = irb.CreateMul(dspLaneU8(rs, 16, irb), dspLaneU8(rt, 16, irb));
			p1 = irb.CreateMul(dspLaneU8(rs, 24, irb), dspLaneU8(rt, 24, irb));
			sub = i->id == MIPS_INS_DPSU_H_QBL;
			break;
		case MIPS_INS_DPAQ_S_W_PH:
		case MIPS_INS_DPAQX_S_W_PH:
		case MIPS_INS_DPAQ_SA_L_W:
		case MIPS_INS_DPAQX_SA_W_PH:
		case MIPS_INS_DPSQ_S_W_PH:
		case MIPS_INS_DPSQX_S_W_PH:
		case MIPS_INS_DPSQ_SA_L_W:
		case MIPS_INS_DPSQX_SA_W_PH:
		{
			if (i->id == MIPS_INS_DPAQ_SA_L_W || i->id == MIPS_INS_DPSQ_SA_L_W)
			{
				p0 = dspQ15Mul(dspToI32(rs, irb), dspToI32(rt, irb), 32, false, irb);
				p1 = llvm::ConstantInt::get(irb.getInt32Ty(), 0);
			}
			else
			{
				bool cross = i->id == MIPS_INS_DPAQX_S_W_PH || i->id == MIPS_INS_DPAQX_SA_W_PH
						|| i->id == MIPS_INS_DPSQX_S_W_PH || i->id == MIPS_INS_DPSQX_SA_W_PH;
				p0 = dspQ15Mul(
						dspLaneS16(rs, 0, irb),
						dspLaneS16(rt, cross ? 16 : 0, irb),
						16,
						false,
						irb);
				p1 = dspQ15Mul(
						dspLaneS16(rs, 16, irb),
						dspLaneS16(rt, cross ? 0 : 16, irb),
						16,
						false,
						irb);
			}
			sub = i->id == MIPS_INS_DPSQ_S_W_PH || i->id == MIPS_INS_DPSQX_S_W_PH
					|| i->id == MIPS_INS_DPSQ_SA_L_W || i->id == MIPS_INS_DPSQX_SA_W_PH;
			break;
		}
		case MIPS_INS_MAQ_S_W_PHL:
		case MIPS_INS_MAQ_SA_W_PHL:
			p0 = dspQ15Mul(dspLaneS16(rs, 16, irb), dspLaneS16(rt, 16, irb), 16, false, irb);
			p1 = llvm::ConstantInt::get(irb.getInt32Ty(), 0);
			break;
		case MIPS_INS_MAQ_S_W_PHR:
		case MIPS_INS_MAQ_SA_W_PHR:
			p0 = dspQ15Mul(dspLaneS16(rs, 0, irb), dspLaneS16(rt, 0, irb), 16, false, irb);
			p1 = llvm::ConstantInt::get(irb.getInt32Ty(), 0);
			break;
		case MIPS_INS_MULSA_W_PH:
			p0 = irb.CreateMul(dspLaneS16(rs, 16, irb), dspLaneS16(rt, 16, irb));
			p1 = irb.CreateNeg(irb.CreateMul(dspLaneS16(rs, 0, irb), dspLaneS16(rt, 0, irb)));
			break;
		case MIPS_INS_MULSAQ_S_W_PH:
			p0 = dspQ15Mul(dspLaneS16(rs, 16, irb), dspLaneS16(rt, 16, irb), 16, false, irb);
			p1 = irb.CreateNeg(dspQ15Mul(dspLaneS16(rs, 0, irb), dspLaneS16(rt, 0, irb), 16, false, irb));
			break;
		default:
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}
	auto* dot = irb.CreateAdd(irb.CreateSExt(p0, i64), irb.CreateSExt(p1, i64));
	auto* res = sub ? irb.CreateSub(acc, dot) : irb.CreateAdd(acc, dot);
	storeOp(mi->operands[0], res, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspExtr(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, mi, irb);
	auto* acc = irb.CreateSExtOrTrunc(loadOp(mi->operands[1], irb), irb.getInt64Ty());
	auto* sh = irb.CreateZExtOrTrunc(loadOp(mi->operands[2], irb), irb.getInt64Ty());
	sh = irb.CreateAnd(sh, llvm::ConstantInt::get(irb.getInt64Ty(), 31));
	bool rnd = i->id == MIPS_INS_EXTR_R_W || i->id == MIPS_INS_EXTRV_R_W
			|| i->id == MIPS_INS_EXTR_RS_W || i->id == MIPS_INS_EXTRV_RS_W;
	bool sat = i->id == MIPS_INS_EXTR_S_H || i->id == MIPS_INS_EXTRV_S_H
			|| i->id == MIPS_INS_EXTR_RS_W || i->id == MIPS_INS_EXTRV_RS_W;
	if (rnd)
	{
		auto* one = llvm::ConstantInt::get(irb.getInt64Ty(), 1);
		auto* bias = irb.CreateSelect(
				irb.CreateICmpEQ(sh, llvm::ConstantInt::get(irb.getInt64Ty(), 0)),
				llvm::ConstantInt::get(irb.getInt64Ty(), 0),
				irb.CreateShl(one, irb.CreateSub(sh, one)));
		acc = irb.CreateAdd(acc, bias);
	}
	auto* v = irb.CreateAShr(acc, sh);
	if (i->id == MIPS_INS_EXTR_S_H || i->id == MIPS_INS_EXTRV_S_H)
	{
		auto* lo = llvm::ConstantInt::get(irb.getInt64Ty(), -32768);
		auto* hi = llvm::ConstantInt::get(irb.getInt64Ty(), 32767);
		v = irb.CreateSelect(irb.CreateICmpSLT(v, lo), lo, v);
		v = irb.CreateSelect(irb.CreateICmpSGT(v, hi), hi, v);
	}
	else if (sat)
	{
		auto* lo = llvm::ConstantInt::get(irb.getInt64Ty(), static_cast<uint64_t>(INT32_MIN));
		auto* hi = llvm::ConstantInt::get(irb.getInt64Ty(), INT32_MAX);
		v = irb.CreateSelect(irb.CreateICmpSLT(v, lo), lo, v);
		v = irb.CreateSelect(irb.CreateICmpSGT(v, hi), hi, v);
	}
	storeOp(mi->operands[0], irb.CreateTrunc(v, irb.getInt32Ty()), irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspPrec(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	auto* rs = loadOp(mi->operands[mi->op_count >= 3 ? 1 : 1], irb);
	llvm::Value* rt = mi->op_count >= 3 ? loadOp(mi->operands[2], irb) : nullptr;
	rs = dspToI32(rs, irb);
	if (rt)
	{
		rt = dspToI32(rt, irb);
	}
	auto* i32 = irb.getInt32Ty();
	llvm::Value* r = nullptr;
	switch (i->id)
	{
		case MIPS_INS_PRECEQ_W_PHL:
			r = irb.CreateAnd(rs, llvm::ConstantInt::get(i32, 0xffff0000));
			break;
		case MIPS_INS_PRECEQ_W_PHR:
			r = irb.CreateShl(irb.CreateAnd(rs, llvm::ConstantInt::get(i32, 0xffff)), 16ull);
			break;
		case MIPS_INS_PRECEQU_PH_QBL:
		case MIPS_INS_PRECEQU_PH_QBLA:
		case MIPS_INS_PRECEU_PH_QBL:
		case MIPS_INS_PRECEU_PH_QBLA:
		{
			unsigned s0 = i->id == MIPS_INS_PRECEQU_PH_QBLA || i->id == MIPS_INS_PRECEU_PH_QBLA ? 16 : 16;
			unsigned s1 = i->id == MIPS_INS_PRECEQU_PH_QBLA || i->id == MIPS_INS_PRECEU_PH_QBLA ? 24 : 24;
			if (i->id == MIPS_INS_PRECEQU_PH_QBL || i->id == MIPS_INS_PRECEU_PH_QBL)
			{
				s0 = 16;
				s1 = 24;
			}
			bool q = i->id == MIPS_INS_PRECEQU_PH_QBL || i->id == MIPS_INS_PRECEQU_PH_QBLA;
			auto* lo = dspLaneU8(rs, s0, irb);
			auto* hi = dspLaneU8(rs, s1, irb);
			if (q)
			{
				lo = irb.CreateShl(lo, 7);
				hi = irb.CreateShl(hi, 7);
			}
			r = irb.CreateOr(lo, irb.CreateShl(hi, 16));
			break;
		}
		case MIPS_INS_PRECEQU_PH_QBR:
		case MIPS_INS_PRECEQU_PH_QBRA:
		case MIPS_INS_PRECEU_PH_QBR:
		case MIPS_INS_PRECEU_PH_QBRA:
		{
			unsigned s0 = 0;
			unsigned s1 = i->id == MIPS_INS_PRECEQU_PH_QBRA || i->id == MIPS_INS_PRECEU_PH_QBRA ? 8 : 8;
			bool q = i->id == MIPS_INS_PRECEQU_PH_QBR || i->id == MIPS_INS_PRECEQU_PH_QBRA;
			auto* lo = dspLaneU8(rs, s0, irb);
			auto* hi = dspLaneU8(rs, s1, irb);
			if (q)
			{
				lo = irb.CreateShl(lo, 7);
				hi = irb.CreateShl(hi, 7);
			}
			r = irb.CreateOr(lo, irb.CreateShl(hi, 16));
			break;
		}
		case MIPS_INS_PRECR_QB_PH:
		{
			auto* a = irb.CreateAnd(rs, llvm::ConstantInt::get(i32, 0xff));
			auto* b = irb.CreateAnd(irb.CreateLShr(rs, 16), llvm::ConstantInt::get(i32, 0xff));
			auto* c = irb.CreateAnd(rt, llvm::ConstantInt::get(i32, 0xff));
			auto* d = irb.CreateAnd(irb.CreateLShr(rt, 16), llvm::ConstantInt::get(i32, 0xff));
			r = irb.CreateOr(irb.CreateOr(c, irb.CreateShl(d, 8)), irb.CreateOr(irb.CreateShl(a, 16), irb.CreateShl(b, 24)));
			break;
		}
		case MIPS_INS_PRECRQ_PH_W:
			r = irb.CreateOr(irb.CreateLShr(rt, 16), irb.CreateAnd(rs, llvm::ConstantInt::get(i32, 0xffff0000)));
			break;
		case MIPS_INS_PRECRQ_QB_PH:
		{
			auto* a = irb.CreateAnd(irb.CreateLShr(rs, 8), llvm::ConstantInt::get(i32, 0xff));
			auto* b = irb.CreateAnd(irb.CreateLShr(rs, 24), llvm::ConstantInt::get(i32, 0xff));
			auto* c = irb.CreateAnd(irb.CreateLShr(rt, 8), llvm::ConstantInt::get(i32, 0xff));
			auto* d = irb.CreateAnd(irb.CreateLShr(rt, 24), llvm::ConstantInt::get(i32, 0xff));
			r = irb.CreateOr(irb.CreateOr(c, irb.CreateShl(d, 8)), irb.CreateOr(irb.CreateShl(a, 16), irb.CreateShl(b, 24)));
			break;
		}
		case MIPS_INS_PRECRQ_RS_PH_W:
		{
			auto* a = irb.CreateAdd(rs, llvm::ConstantInt::get(i32, 0x8000));
			auto* b = irb.CreateAdd(rt, llvm::ConstantInt::get(i32, 0x8000));
			r = irb.CreateOr(irb.CreateLShr(b, 16), irb.CreateAnd(a, llvm::ConstantInt::get(i32, 0xffff0000)));
			break;
		}
		case MIPS_INS_PRECRQU_S_QB_PH:
		{
			auto sat8 = [&](llvm::Value* h) {
				auto* cap = llvm::ConstantInt::get(i32, 0xff);
				return irb.CreateSelect(irb.CreateICmpUGT(h, cap), cap, irb.CreateAnd(h, cap));
			};
			auto* c0 = sat8(irb.CreateAnd(rt, llvm::ConstantInt::get(i32, 0xffff)));
			auto* c1 = sat8(irb.CreateLShr(rt, 16ull));
			auto* c2 = sat8(irb.CreateAnd(rs, llvm::ConstantInt::get(i32, 0xffff)));
			auto* c3 = sat8(irb.CreateLShr(rs, 16ull));
			r = irb.CreateOr(irb.CreateOr(c0, irb.CreateShl(c1, 8ull)), irb.CreateOr(irb.CreateShl(c2, 16ull), irb.CreateShl(c3, 24ull)));
			break;
		}
		case MIPS_INS_PRECR_SRA_PH_W:
		case MIPS_INS_PRECR_SRA_R_PH_W:
		{
			llvm::Value* sa = mi->op_count >= 4 ? loadOp(mi->operands[3], irb)
					: (mi->operands[2].type == MIPS_OP_IMM ? loadOp(mi->operands[2], irb) : llvm::ConstantInt::get(i32, 0));
			sa = irb.CreateAnd(irb.CreateZExtOrTrunc(sa, i32), llvm::ConstantInt::get(i32, 31));
			if (i->id == MIPS_INS_PRECR_SRA_R_PH_W)
			{
				auto* bias = irb.CreateSelect(
						irb.CreateICmpEQ(sa, llvm::ConstantInt::get(i32, 0)),
						llvm::ConstantInt::get(i32, 0),
						irb.CreateShl(llvm::ConstantInt::get(i32, 1), irb.CreateSub(sa, llvm::ConstantInt::get(i32, 1))));
				rs = irb.CreateAdd(rs, bias);
				rt = irb.CreateAdd(rt, bias);
			}
			auto* a = irb.CreateAnd(irb.CreateAShr(rs, sa), llvm::ConstantInt::get(i32, 0xffff));
			auto* b = irb.CreateAnd(irb.CreateAShr(rt, sa), llvm::ConstantInt::get(i32, 0xffff));
			r = irb.CreateOr(b, irb.CreateShl(a, 16));
			break;
		}
		case MIPS_INS_PACKRL_PH:
			r = irb.CreateOr(irb.CreateLShr(rs, 16), irb.CreateShl(rt, 16));
			break;
		case MIPS_INS_EXTP:
		case MIPS_INS_EXTPDP:
		case MIPS_INS_EXTPV:
		case MIPS_INS_EXTPDPV:
		{
			auto* acc = irb.CreateSExtOrTrunc(rs, irb.getInt64Ty());
			auto* pos = irb.CreateZExtOrTrunc(loadRegister(MIPS_REG_DSPPOS, irb), irb.getInt64Ty());
			llvm::Value* size = rt ? irb.CreateZExtOrTrunc(rt, irb.getInt64Ty())
					: llvm::ConstantInt::get(irb.getInt64Ty(), 32);
			auto* sh = irb.CreateSub(irb.CreateAdd(pos, llvm::ConstantInt::get(irb.getInt64Ty(), 1)), size);
			auto* mask = irb.CreateSub(irb.CreateShl(llvm::ConstantInt::get(irb.getInt64Ty(), 1), size), llvm::ConstantInt::get(irb.getInt64Ty(), 1));
			r = irb.CreateTrunc(irb.CreateAnd(irb.CreateLShr(acc, sh), mask), i32);
			if (i->id == MIPS_INS_EXTPDP || i->id == MIPS_INS_EXTPDPV)
			{
				storeRegister(
						MIPS_REG_DSPPOS,
						irb.CreateSub(irb.CreateTrunc(pos, getDefaultType()), irb.CreateTrunc(size, getDefaultType())),
						irb,
						eOpConv::ZEXT_TRUNC_OR_BITCAST);
			}
			break;
		}
		default:
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}
	storeOp(mi->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspIndexLoad(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	llvm::Value* addr = nullptr;
	if (mi->op_count == 2 && mi->operands[1].type == MIPS_OP_MEM)
	{
		addr = loadOp(mi->operands[1], irb, nullptr, /*lea=*/true);
	}
	else if (mi->op_count == 3)
	{
		auto* idx = loadOp(mi->operands[1], irb);
		auto* base = loadOp(mi->operands[2], irb);
		if (i->id == MIPS_INS_LHXS || i->id == MIPS_INS_LHUXS)
		{
			idx = irb.CreateShl(idx, 1);
		}
		addr = irb.CreateAdd(
				generateTypeConversion(irb, base, getDefaultType(), eOpConv::ZEXT_TRUNC_OR_BITCAST),
				generateTypeConversion(irb, idx, getDefaultType(), eOpConv::ZEXT_TRUNC_OR_BITCAST));
	}
	if (addr == nullptr)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	llvm::Type* ty = irb.getInt32Ty();
	eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
	if (i->id == MIPS_INS_LBUX)
	{
		ty = irb.getInt8Ty();
		ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
	}
	else if (i->id == MIPS_INS_LBX)
	{
		ty = irb.getInt8Ty();
	}
	else if (i->id == MIPS_INS_LHX || i->id == MIPS_INS_LHUX
			|| i->id == MIPS_INS_LHXS || i->id == MIPS_INS_LHUXS)
	{
		ty = irb.getInt16Ty();
		if (i->id == MIPS_INS_LHUX || i->id == MIPS_INS_LHUXS)
		{
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
		}
	}
	storeOp(mi->operands[0], loadIntPtr(irb, addr, ty), irb, ct);
}

void Capstone2LlvmIrTranslatorMips_impl::translateDspCtrl(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	switch (i->id)
	{
		case MIPS_INS_ADDSC:
		{
			EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
			std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			op1 = dspToI32(op1, irb);
			op2 = dspToI32(op2, irb);
			auto* sum = irb.CreateAdd(op1, op2);
			storeRegister(MIPS_REG_DSPCARRY, irb.CreateICmpULT(sum, op1), irb);
			storeOp(mi->operands[0], sum, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		case MIPS_INS_ADDWC:
		{
			EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
			std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			op1 = dspToI32(op1, irb);
			op2 = dspToI32(op2, irb);
			auto* c = irb.CreateZExt(loadRegister(MIPS_REG_DSPCARRY, irb), i32);
			storeOp(mi->operands[0], irb.CreateAdd(irb.CreateAdd(op1, op2), c), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		case MIPS_INS_INSV:
		{
			EXPECT_IS_BINARY(i, mi, irb);
			auto* dst = dspToI32(loadOp(mi->operands[0], irb), irb);
			auto* src = dspToI32(loadOp(mi->operands[1], irb), irb);
			auto* pos = irb.CreateZExtOrTrunc(loadRegister(MIPS_REG_DSPPOS, irb), i32);
			auto* sz = irb.CreateZExtOrTrunc(loadRegister(MIPS_REG_DSPSCOUNT, irb), i32);
			sz = irb.CreateAnd(sz, llvm::ConstantInt::get(i32, 31));
			pos = irb.CreateAnd(pos, llvm::ConstantInt::get(i32, 31));
			auto* ones = irb.CreateSub(irb.CreateShl(llvm::ConstantInt::get(i32, 1), sz), llvm::ConstantInt::get(i32, 1));
			auto* mask = irb.CreateShl(ones, pos);
			auto* ins = irb.CreateShl(irb.CreateAnd(src, ones), pos);
			storeOp(mi->operands[0], irb.CreateOr(irb.CreateAnd(dst, irb.CreateNot(mask)), ins), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		case MIPS_INS_MODSUB:
		{
			EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
			std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(mi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			op1 = dspToI32(op1, irb);
			op2 = dspToI32(op2, irb);
			auto* last = irb.CreateAnd(irb.CreateLShr(op2, 16), llvm::ConstantInt::get(i32, 0xff));
			auto* dec = irb.CreateSub(op1, irb.CreateAnd(op2, llvm::ConstantInt::get(i32, 0xff)));
			storeOp(
					mi->operands[0],
					irb.CreateSelect(irb.CreateICmpEQ(op1, llvm::ConstantInt::get(i32, 0)), last, dec),
					irb,
					eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		case MIPS_INS_RADDU_W_QB:
		{
			EXPECT_IS_BINARY(i, mi, irb);
			auto* src = dspToI32(loadOp(mi->operands[1], irb), irb);
			auto* s = dspLaneU8(src, 0, irb);
			s = irb.CreateAdd(s, dspLaneU8(src, 8, irb));
			s = irb.CreateAdd(s, dspLaneU8(src, 16, irb));
			s = irb.CreateAdd(s, dspLaneU8(src, 24, irb));
			storeOp(mi->operands[0], s, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		case MIPS_INS_RDDSP:
		{
			EXPECT_IS_BINARY(i, mi, irb);
			auto* maskV = irb.CreateZExtOrTrunc(loadOp(mi->operands[1], irb), i32);
			uint32_t mask = 0x3f;
			if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(maskV))
			{
				mask = static_cast<uint32_t>(ci->getZExtValue());
			}
			llvm::Value* r = llvm::ConstantInt::get(i32, 0);
			auto addField = [&](unsigned bit, uint32_t reg, unsigned sh) {
				if ((mask & (1u << bit)) == 0)
				{
					return;
				}
				r = irb.CreateOr(r, irb.CreateShl(irb.CreateZExtOrTrunc(loadRegister(reg, irb), i32), sh));
			};
			addField(0, MIPS_REG_DSPPOS, 0);
			addField(1, MIPS_REG_DSPSCOUNT, 7);
			addField(2, MIPS_REG_DSPEFI, 13);
			addField(3, MIPS_REG_DSPOUTFLAG, 16);
			addField(4, MIPS_REG_DSPCCOND, 24);
			addField(5, MIPS_REG_DSPCARRY, 14);
			storeOp(mi->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		case MIPS_INS_WRDSP:
		{
			EXPECT_IS_BINARY(i, mi, irb);
			auto* src = dspToI32(loadOp(mi->operands[0], irb), irb);
			auto* maskV = irb.CreateZExtOrTrunc(loadOp(mi->operands[1], irb), i32);
			uint32_t mask = 0x3f;
			if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(maskV))
			{
				mask = static_cast<uint32_t>(ci->getZExtValue());
			}
			auto writeField = [&](unsigned bit, uint32_t reg, unsigned sh, unsigned wbits) {
				if ((mask & (1u << bit)) == 0)
				{
					return;
				}
				auto* field = irb.CreateAnd(irb.CreateLShr(src, sh), llvm::ConstantInt::get(i32, (1u << wbits) - 1));
				storeRegister(reg, field, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			};
			writeField(0, MIPS_REG_DSPPOS, 0, 6);
			writeField(1, MIPS_REG_DSPSCOUNT, 7, 6);
			writeField(2, MIPS_REG_DSPEFI, 13, 1);
			writeField(3, MIPS_REG_DSPOUTFLAG, 16, 1);
			writeField(4, MIPS_REG_DSPCCOND, 24, 4);
			writeField(5, MIPS_REG_DSPCARRY, 14, 1);
			return;
		}
		case MIPS_INS_BPOSGE32:
		{
			auto* pos = loadRegister(MIPS_REG_DSPPOS, irb);
			pos = irb.CreateZExtOrTrunc(pos, i32);
			auto* cond = irb.CreateICmpUGE(pos, llvm::ConstantInt::get(i32, 32));
			llvm::Value* tgt = mi->op_count >= 1 ? loadOp(mi->operands[0], irb)
					: llvm::ConstantInt::get(getDefaultType(), i->address + 8);
			generateCondBranchFunctionCall(irb, cond, tgt);
			return;
		}
		case MIPS_INS_APPEND:
		case MIPS_INS_PREPEND:
		case MIPS_INS_BALIGN:
		{
			EXPECT_IS_BINARY_OR_TERNARY(i, mi, irb);
			auto* dst = dspToI32(loadOp(mi->operands[0], irb), irb);
			auto* src = dspToI32(loadOp(mi->operands[1], irb), irb);
			llvm::Value* sa = mi->op_count >= 3 ? loadOp(mi->operands[2], irb) : llvm::ConstantInt::get(i32, 0);
			sa = irb.CreateAnd(irb.CreateZExtOrTrunc(sa, i32), llvm::ConstantInt::get(i32, 31));
			llvm::Value* r = nullptr;
			if (i->id == MIPS_INS_APPEND)
			{
				auto* mask = irb.CreateSub(irb.CreateShl(llvm::ConstantInt::get(i32, 1), sa), llvm::ConstantInt::get(i32, 1));
				r = irb.CreateOr(irb.CreateShl(dst, sa), irb.CreateAnd(src, mask));
			}
			else if (i->id == MIPS_INS_PREPEND)
			{
				auto* left = irb.CreateSub(llvm::ConstantInt::get(i32, 32), sa);
				r = irb.CreateOr(irb.CreateShl(src, left), irb.CreateLShr(dst, sa));
			}
			else
			{
				auto* bits = irb.CreateShl(sa, 3);
				auto* left = irb.CreateSub(llvm::ConstantInt::get(i32, 32), bits);
				r = irb.CreateOr(irb.CreateShl(dst, bits), irb.CreateLShr(src, left));
			}
			storeOp(mi->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		case MIPS_INS_MTHLIP:
		{
			EXPECT_IS_BINARY(i, mi, irb);
			auto* acc = irb.CreateZExtOrTrunc(loadOp(mi->operands[0], irb), irb.getInt64Ty());
			auto* rs = irb.CreateZExtOrTrunc(loadOp(mi->operands[1], irb), irb.getInt64Ty());
			auto* lo = irb.CreateAnd(acc, llvm::ConstantInt::get(irb.getInt64Ty(), 0xffffffffull));
			auto* n = irb.CreateOr(irb.CreateShl(lo, 32), irb.CreateAnd(rs, llvm::ConstantInt::get(irb.getInt64Ty(), 0xffffffffull)));
			storeOp(mi->operands[0], n, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			auto* pos = loadRegister(MIPS_REG_DSPPOS, irb);
			storeRegister(
					MIPS_REG_DSPPOS,
					irb.CreateAdd(pos, llvm::ConstantInt::get(pos->getType(), 32)),
					irb);
			return;
		}
		case MIPS_INS_SHILO:
		case MIPS_INS_SHILOV:
		{
			EXPECT_IS_BINARY(i, mi, irb);
			auto* i64 = irb.getInt64Ty();
			auto* acc = loadOp(mi->operands[0], irb);
			acc = irb.CreateSExtOrTrunc(acc, i64);
			auto* sa6 = irb.CreateSExt(
					irb.CreateTrunc(loadOp(mi->operands[1], irb), irb.getIntNTy(6)),
					i64);
			auto* zero = llvm::ConstantInt::get(i64, 0);
			auto* maxSh = llvm::ConstantInt::get(i64, 63);
			auto* isNeg = irb.CreateICmpSLT(sa6, zero);
			auto* mag = irb.CreateSelect(isNeg, irb.CreateNeg(sa6), sa6);
			mag = irb.CreateSelect(irb.CreateICmpUGT(mag, maxSh), maxSh, mag);
			acc = irb.CreateSelect(isNeg, irb.CreateShl(acc, mag), irb.CreateLShr(acc, mag));
			storeOp(mi->operands[0], acc, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		default:
			translatePseudoAsmGeneric(i, mi, irb);
			return;
	}
}

/**
 * Conditional traps: TEQ/TNE/TGE/TGEU/TLT/TLTU and their immediate forms.
 * The trap itself is a pseudo call that only happens when the condition holds.
 */
void Capstone2LlvmIrTranslatorMips_impl::translateTrap(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count < 2)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	auto* a = loadOp(mi->operands[0], irb);
	auto* b = generateTypeConversion(irb, loadOp(mi->operands[1], irb), a->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case MIPS_INS_TEQ: case MIPS_INS_TEQI: cond = irb.CreateICmpEQ(a, b); break;
		case MIPS_INS_TNE: case MIPS_INS_TNEI: cond = irb.CreateICmpNE(a, b); break;
		case MIPS_INS_TGE: case MIPS_INS_TGEI: cond = irb.CreateICmpSGE(a, b); break;
		case MIPS_INS_TGEU: case MIPS_INS_TGEIU: cond = irb.CreateICmpUGE(a, b); break;
		case MIPS_INS_TLT: case MIPS_INS_TLTI: cond = irb.CreateICmpSLT(a, b); break;
		case MIPS_INS_TLTU: case MIPS_INS_TLTIU: cond = irb.CreateICmpULT(a, b); break;
		default: throw GenericError("unhandled trap id");
	}
	auto* fnc = getPseudoAsmFunction(i, irb.getVoidTy(), llvm::ArrayRef<llvm::Type*>{});
	llvm::IRBuilder<> body(generateIfThen(cond, irb));
	body.CreateCall(fnc);
}



/**
 * Paired-single (and MIPS-3D) operations: a 64-bit FPR holds two singles,
 * PU in bits 63..32 and PL in bits 31..0.
 * MIPS_INS_ADD_PS, MIPS_INS_SUB_PS, MIPS_INS_MUL_PS, MIPS_INS_ADDR_PS,
 * MIPS_INS_MULR_PS, MIPS_INS_PLL_PS, MIPS_INS_PLU_PS, MIPS_INS_PUL_PS,
 * MIPS_INS_PUU_PS, MIPS_INS_CVT_PS_S, MIPS_INS_CVT_S_PL, MIPS_INS_CVT_S_PU,
 * MIPS_INS_CVT_PS_PW, MIPS_INS_CVT_PW_PS
 */
void Capstone2LlvmIrTranslatorMips_impl::translatePairedSingle(cs_insn* i, cs_mips* mi, llvm::IRBuilder<>& irb)
{
	if (mi->op_count < 2 || mi->op_count > 3)
	{
		translatePseudoAsmGeneric(i, mi, irb);
		return;
	}
	for (unsigned k = 0; k < mi->op_count; ++k)
	{
		if (mi->operands[k].type != MIPS_OP_REG)
		{
			translatePseudoAsmGeneric(i, mi, irb);
			return;
		}
	}
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* f32 = irb.getFloatTy();
	auto bits = [&](unsigned k) -> llvm::Value* {
		auto* v = loadRegister(canonicalizeMipsReg(mi->operands[k].reg), irb);
		if (v->getType()->isDoubleTy()) return irb.CreateBitCast(v, i64);
		if (v->getType()->isFloatTy()) return irb.CreateZExt(irb.CreateBitCast(v, i32), i64);
		return irb.CreateZExtOrTrunc(v, i64);
	};
	auto pu = [&](llvm::Value* b) { return irb.CreateTrunc(irb.CreateLShr(b, 32), i32); };
	auto pl = [&](llvm::Value* b) { return irb.CreateTrunc(b, i32); };
	auto asF = [&](llvm::Value* w) { return irb.CreateBitCast(w, f32); };
	auto asW = [&](llvm::Value* f) { return irb.CreateBitCast(f, i32); };
	auto pack = [&](llvm::Value* hi, llvm::Value* lo) {
		return irb.CreateOr(irb.CreateShl(irb.CreateZExt(hi, i64), 32), irb.CreateZExt(lo, i64));
	};
	auto store = [&](llvm::Value* b64) {
		auto* rt = getRegisterType(canonicalizeMipsReg(mi->operands[0].reg));
		llvm::Value* v = b64;
		if (rt->isDoubleTy()) v = irb.CreateBitCast(b64, rt);
		else if (rt->isFloatTy()) v = irb.CreateBitCast(irb.CreateTrunc(b64, i32), rt);
		else v = irb.CreateZExtOrTrunc(b64, rt);
		storeRegister(canonicalizeMipsReg(mi->operands[0].reg), v, irb);
	};
	auto fbin = [&](llvm::Value* a, llvm::Value* b) -> llvm::Value* {
		switch (i->id)
		{
			case MIPS_INS_SUB_PS: return irb.CreateFSub(a, b);
			case MIPS_INS_MUL_PS:
			case MIPS_INS_MULR_PS: return irb.CreateFMul(a, b);
			default: return irb.CreateFAdd(a, b);
		}
	};

	switch (i->id)
	{
		case MIPS_INS_ADD_PS:
		case MIPS_INS_SUB_PS:
		case MIPS_INS_MUL_PS:
		{
			if (mi->op_count != 3) break;
			auto* a = bits(1);
			auto* b = bits(2);
			store(pack(asW(fbin(asF(pu(a)), asF(pu(b)))), asW(fbin(asF(pl(a)), asF(pl(b))))));
			return;
		}
		case MIPS_INS_ADDR_PS:
		case MIPS_INS_MULR_PS:
		{
			// Reduction: PL = fs.PU op fs.PL, PU = ft.PU op ft.PL.
			if (mi->op_count != 3) break;
			auto* a = bits(1);
			auto* b = bits(2);
			store(pack(asW(fbin(asF(pu(b)), asF(pl(b)))), asW(fbin(asF(pu(a)), asF(pl(a))))));
			return;
		}
		case MIPS_INS_PLL_PS:
		case MIPS_INS_PLU_PS:
		case MIPS_INS_PUL_PS:
		case MIPS_INS_PUU_PS:
		{
			if (mi->op_count != 3) break;
			auto* a = bits(1);
			auto* b = bits(2);
			bool hiFromU = i->id == MIPS_INS_PUL_PS || i->id == MIPS_INS_PUU_PS;
			bool loFromU = i->id == MIPS_INS_PLU_PS || i->id == MIPS_INS_PUU_PS;
			store(pack(hiFromU ? pu(a) : pl(a), loFromU ? pu(b) : pl(b)));
			return;
		}
		case MIPS_INS_CVT_PS_S:
		{
			// Sources are singles; this model keeps a single as its numeric
			// value in the register, so narrow it to float.
			if (mi->op_count != 3) break;
			auto single = [&](unsigned k) -> llvm::Value* {
				auto* v = loadRegister(canonicalizeMipsReg(mi->operands[k].reg), irb);
				if (v->getType()->isFloatingPointTy()) return asW(irb.CreateFPTrunc(v, f32));
				return irb.CreateTrunc(v, i32);
			};
			store(pack(single(1), single(2)));
			return;
		}
		case MIPS_INS_CVT_S_PL:
		case MIPS_INS_CVT_S_PU:
		{
			if (mi->op_count != 2) break;
			auto* a = bits(1);
			auto* f = asF(i->id == MIPS_INS_CVT_S_PU ? pu(a) : pl(a));
			auto* rt = getRegisterType(canonicalizeMipsReg(mi->operands[0].reg));
			storeRegister(canonicalizeMipsReg(mi->operands[0].reg), rt->isDoubleTy() ? irb.CreateFPExt(f, rt) : f, irb,
					eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case MIPS_INS_CVT_PS_PW:
		{
			if (mi->op_count != 2) break;
			auto* a = bits(1);
			store(pack(asW(irb.CreateSIToFP(pu(a), f32)), asW(irb.CreateSIToFP(pl(a), f32))));
			return;
		}
		case MIPS_INS_CVT_PW_PS:
		{
			if (mi->op_count != 2) break;
			auto* a = bits(1);
			auto cvt = [&](llvm::Value* w) {
				// FCSR.RM (nearest even by default); an invalid conversion
				// gives the legacy default 2^31-1.
				auto* f = asF(w);
				auto* r = irb.CreateCall(
						llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::roundeven, {f32}), {f});
				auto* ok = irb.CreateAnd(irb.CreateFCmpOGE(r, llvm::ConstantFP::get(f32, -2147483648.0)),
						irb.CreateFCmpOLT(r, llvm::ConstantFP::get(f32, 2147483648.0)));
				auto* safe = irb.CreateSelect(ok, r, llvm::ConstantFP::get(f32, 0.0));
				return irb.CreateSelect(ok, irb.CreateFPToSI(safe, i32), llvm::ConstantInt::get(i32, 0x7fffffff));
			};
			store(pack(cvt(pu(a)), cvt(pl(a))));
			return;
		}
		default: break;
	}
	translatePseudoAsmGeneric(i, mi, irb);
}

} // namespace capstone2llvmir
} // namespace retdec
