/**
 * @file src/capstone2llvmir/arm/arm.cpp
 * @brief ARM implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cmath>
#include <cstdint>
#include <iomanip>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>

#include "capstone2llvmir/arm/arm_impl.h"
#include "retdec/capstone2llvmir/arm/arm_thumb_interwork.h"

namespace retdec {
namespace capstone2llvmir {

namespace {

// Capstone 6.x keeps the real encoding id (MOV/LDM/HINT) and puts the
// assembler mnemonic in alias_id. Translators that switch on mnemonic must
// see both.
bool armIsId(const cs_insn* i, unsigned id)
{
	return i->id == id || (i->is_alias && static_cast<unsigned>(i->alias_id) == id);
}

bool armCcUncond(const cs_arm* ai)
{
	return ai->cc == ARM_CC_AL || ai->cc == ARM_CC_INVALID
			|| ai->cc == ARMCC_UNDEF;
}

void armDropPredOperands(cs_arm* ai)
{
	uint8_t w = 0;
	for (uint8_t r = 0; r < ai->op_count; ++r)
	{
		auto t = ai->operands[r].type;
		if (t == ARM_OP_PRED || t == ARM_OP_VPRED_R || t == ARM_OP_VPRED_N)
		{
			continue;
		}
		if (w != r)
		{
			ai->operands[w] = ai->operands[r];
		}
		++w;
	}
	ai->op_count = w;
}

} // namespace

Capstone2LlvmIrTranslatorArm_impl::Capstone2LlvmIrTranslatorArm_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(CS_ARCH_ARM, basic, extra, m)
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

bool Capstone2LlvmIrTranslatorArm_impl::isAllowedBasicMode(cs_mode m)
{
	return m == CS_MODE_ARM
			|| m == CS_MODE_THUMB;
}

bool Capstone2LlvmIrTranslatorArm_impl::isAllowedExtraMode(cs_mode m)
{
	return m == CS_MODE_LITTLE_ENDIAN
			|| m == CS_MODE_BIG_ENDIAN;
}

uint32_t Capstone2LlvmIrTranslatorArm_impl::getArchByteSize()
{
	return 4;
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorArm_impl::generateEnvironmentArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorArm_impl::generateDataLayout()
{
	_module->setDataLayout("e-p:32:32:32-f80:32:32");
}

void Capstone2LlvmIrTranslatorArm_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		createRegister(p.first, _regLt);
	}
}

uint32_t Capstone2LlvmIrTranslatorArm_impl::getCarryRegister()
{
	return ARM_REG_CPSR_C;
}

void Capstone2LlvmIrTranslatorArm_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_arm* ai = &d->arm;
	armDropPredOperands(ai);

	// Capstone 6 keeps the real id (MOV/HINT) and puts lsl/nop/pop in
	// alias_id. Prefer a dedicated alias translator.
	std::size_t id = i->id;
	if (i->is_alias)
	{
		auto aIt = _i2fm.find(static_cast<std::size_t>(i->alias_id));
		if (aIt != _i2fm.end() && aIt->second != nullptr)
		{
			id = static_cast<std::size_t>(i->alias_id);
		}
	}

	auto fIt = _i2fm.find(id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;

		bool branchInsn = armIsId(i, ARM_INS_B) || armIsId(i, ARM_INS_BX)
				|| armIsId(i, ARM_INS_BL) || armIsId(i, ARM_INS_BLX)
				|| armIsId(i, ARM_INS_BXNS) || armIsId(i, ARM_INS_BLXNS)
				|| armIsId(i, ARM_INS_CBZ) || armIsId(i, ARM_INS_CBNZ)
				|| armIsId(i, ARM_INS_TBB) || armIsId(i, ARM_INS_TBH);
		if (armCcUncond(ai) || branchInsn)
		{
			_inCondition = false;
			(this->*f)(i, ai, irb);
		}
		else
		{
			_inCondition = true;

			auto* cond = generateInsnConditionCode(irb, ai);
			llvm::IRBuilder<> bodyIrb(generateIfThen(cond, irb));

			(this->*f)(i, ai, bodyIrb);
		}
	}
	else
	{
		throwUnhandledInstructions(i);

		if (armCcUncond(ai))
		{
			_inCondition = false;
			translatePseudoAsmGeneric(i, ai, irb);
		}
		else
		{
			_inCondition = true;

			auto* cond = generateInsnConditionCode(irb, ai);
			llvm::IRBuilder<> bodyIrb(generateIfThen(cond, irb));

			translatePseudoAsmGeneric(i, ai, bodyIrb);
		}
	}
}

//
//==============================================================================
// ARM-specific methods.
//==============================================================================
//

/**
 * During execution, PC does not contain the address of the currently executing
 * instruction. The address of the currently executing instruction is typically
 * PC-8 for ARM, or PC-4 for Thumb.
 *
 * In Thumb state:
 * - For B, BL, CBNZ, and CBZ instructions, the value of the PC is the address
 *   of the current instruction plus 4 bytes.
 * - For all other instructions that use labels, the value of the PC is the
 *   address of the current instruction plus 4 bytes, with bit[1] of the result
 *   cleared to 0 to make it word-aligned.
 *
 * ARM:
 * current = PC - 8
 * =>
 * PC = current + 8 = current + 2*4 = current + 2*insn_size
 *
 * THUMB:
 * current = PC - 4
 * =>
 * PC = current + 4 = current + 2*2 = current + 2*insn_size
 */
/**
 * The value an instruction reads out of PC.
 *
 * ARM reads the instruction's address plus 8. Thumb reads it plus 4 -- and
 * plus 4 whether the instruction is 16 or 32 bits wide, which is the part the
 * expression here used to get wrong. It was `address + 2*size`, which is
 * address+8 in ARM and address+4 for a 16-bit Thumb instruction, and so looked
 * right in both of the cases anything tested; a 32-bit Thumb instruction has
 * size 4 and got address+8, four bytes past where the architecture says PC is.
 * Thumb-2 literal loads and ADR both read it.
 */
llvm::Value* Capstone2LlvmIrTranslatorArm_impl::getCurrentPc(cs_insn* i)
{
	uint64_t pc = _basicMode == CS_MODE_THUMB ? i->address + 4 : i->address + 8;
	return llvm::ConstantInt::get(getDefaultType(), (pc >> 2) << 2);
}

/**
 * ARM's VFP register bank overlaps: s0 and s1 ARE d0, s2 and s3 are d1, and
 * so on up to d15. They were separate globals, so a `vldr d0, [r0]` followed
 * by a `vmov r1, s0` read storage nothing had written -- and the corpus does
 * exactly that, 9,323 `vldr d` against 684 `vmov gpr, s`.
 *
 * The mapping is `Dn[31:0] = S2n` and `Dn[63:32] = S2n+1`. That is the ARM
 * ARM's definition, and gcc's own register allocation agrees: a function
 * returning the low float half of a double argument compiles to a bare
 * `bx lr`, and the high half to `vmov.f32 s0, s1`.
 *
 * This differs from ARM64 in two ways that matter. The views PAIR rather than
 * nest, so s1 sits at bit 32 of its parent and needs an offset as well as a
 * width. And an ARM sub-register write MERGES -- writing s0 leaves s1 alone --
 * where every ARM64 sub-register write zeroes the rest.
 *
 * Qn is D(2n):D(2n+1). NEON reads and writes the D pair, not the unused f128
 * global; the same merge rule as an S write applies at 64-bit granularity.
 */
bool Capstone2LlvmIrTranslatorArm_impl::isSingleView(uint32_t r)
{
	return r >= ARM_REG_S0 && r <= ARM_REG_S31;
}

bool Capstone2LlvmIrTranslatorArm_impl::isQuadView(uint32_t r)
{
	return r >= ARM_REG_Q0 && r <= ARM_REG_Q15;
}

uint32_t Capstone2LlvmIrTranslatorArm_impl::quadViewLoD(uint32_t r)
{
	return ARM_REG_D0 + 2 * (r - ARM_REG_Q0);
}

bool Capstone2LlvmIrTranslatorArm_impl::isNeonRegister(uint32_t r)
{
	return isFpRegister(r) || isQuadView(r);
}

bool Capstone2LlvmIrTranslatorArm_impl::insnWriteback() const
{
	return _insn && _insn->detail && _insn->detail->writeback;
}

/// The D register an S register is half of, and which half.
uint32_t Capstone2LlvmIrTranslatorArm_impl::singleViewParent(uint32_t r, unsigned& offset)
{
	unsigned n = r - ARM_REG_S0;
	offset = (n % 2) * 32;
	return ARM_REG_D0 + n / 2;
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::loadSingleView(uint32_t r, llvm::IRBuilder<>& irb)
{
	unsigned offset = 0;
	auto* parent = getRegister(singleViewParent(r, offset));
	if (parent == nullptr)
	{
		throw GenericError("loadRegister() unhandled S register.");
	}

	auto* i64 = irb.getInt64Ty();
	llvm::Value* bits = irb.CreateBitCast(createLoad(irb, parent), i64);
	if (offset)
	{
		bits = irb.CreateLShr(bits, llvm::ConstantInt::get(i64, offset));
	}

	return irb.CreateBitCast(irb.CreateTrunc(bits, irb.getInt32Ty()), irb.getFloatTy());
}

llvm::StoreInst*
Capstone2LlvmIrTranslatorArm_impl::storeSingleView(uint32_t r, llvm::Value* val, llvm::IRBuilder<>& irb)
{
	unsigned offset = 0;
	auto* parent = getRegister(singleViewParent(r, offset));
	if (parent == nullptr)
	{
		throw GenericError("storeRegister() unhandled S register.");
	}

	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();

	llvm::Value* asFloat = generateTypeConversion(irb, val, irb.getFloatTy(), eOpConv::FPCAST_OR_BITCAST);
	llvm::Value* piece = irb.CreateZExt(irb.CreateBitCast(asFloat, i32), i64);
	if (offset)
	{
		piece = irb.CreateShl(piece, llvm::ConstantInt::get(i64, offset));
	}

	// The other half of the D register is left alone: an ARM sub-register
	// write merges.
	llvm::Value* old = irb.CreateBitCast(createLoad(irb, parent), i64);
	llvm::Value* keep =
		irb.CreateAnd(old, llvm::ConstantInt::get(i64, ~(static_cast<uint64_t>(0xffffffffULL) << offset)));

	auto* s = irb.CreateStore(irb.CreateBitCast(irb.CreateOr(keep, piece), parent->getValueType()), parent);
	attachPointeeType(s, parent->getValueType());
	return s;
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::loadQuadView(uint32_t r, llvm::IRBuilder<>& irb)
{
	uint32_t lo = quadViewLoD(r);
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getIntNTy(128);
	auto* loB = irb.CreateZExt(irb.CreateBitCast(loadRegister(lo, irb), i64), i128);
	auto* hiB = irb.CreateZExt(irb.CreateBitCast(loadRegister(lo + 1, irb), i64), i128);
	auto* bits = irb.CreateOr(loB, irb.CreateShl(hiB, llvm::ConstantInt::get(i128, 64)));
	return irb.CreateBitCast(bits, llvm::Type::getFP128Ty(_module->getContext()));
}

llvm::Instruction* Capstone2LlvmIrTranslatorArm_impl::storeQuadView(uint32_t r, llvm::Value* val, llvm::IRBuilder<>& irb)
{
	uint32_t lo = quadViewLoD(r);
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getIntNTy(128);
	auto* bits = generateTypeConversion(irb, val, i128, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeRegister(lo, irb.CreateTrunc(bits, i64), irb, eOpConv::FPCAST_OR_BITCAST);
	return storeRegister(
		lo + 1, irb.CreateTrunc(irb.CreateLShr(bits, llvm::ConstantInt::get(i128, 64)), i64), irb, eOpConv::FPCAST_OR_BITCAST);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == ARM_REG_INVALID)
	{
		return nullptr;
	}

	if (r == ARM_REG_PC)
	{
		return getCurrentPc(_insn);
	}

	if (isSingleView(r))
	{
		return generateTypeConversion(irb, loadSingleView(r, irb), dstType, ct);
	}

	if (isQuadView(r))
	{
		return generateTypeConversion(irb, loadQuadView(r, irb), dstType, ct);
	}

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);

	return createLoad(irb, llvmReg);
}

/**
 * The index TERM of a memory operand: the index register with its shift and
 * its sign applied, and nothing else.
 *
 * The address computation in loadOp() already does this. The pre- and
 * post-indexed writeback paths did not -- they reloaded the bare index
 * register, so `ldr r0, [r1, r2, lsl #2]!` loaded from r1 + (r2 << 2) and then
 * wrote back r1 + r2, and `ldr r0, [r1, -r2]!` loaded from r1 - r2 and wrote
 * back r1 + r2. The address and the writeback disagreed about their own
 * addressing mode.
 *
 * Not generateOperandShift(), which writes CPSR_C: the address path has
 * already called it, and a second identical write is noise at best. A memory
 * operand's shift is always an immediate in A32 -- `[Rn, Rm, LSL Rs]` is not
 * encodable -- so every case here is a constant and can be built directly.
 * A shift kind that is not one of those falls back to the unshifted register,
 * which is what this code did for all of them before.
 */
/**
 * A float-to-integer conversion with ARM's defined answer for the inputs that
 * do not fit: NaN converts to zero, and an out-of-range magnitude saturates to
 * the destination's minimum or maximum. LLVM's fptosi and fptoui call all
 * three poison.
 *
 * NOT MEASURED -- this container is x86-64 with no ARM emulator, so this rests
 * on the ARM ARM's FPToFixed() pseudocode rather than on an observation.
 */
llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateFpToIntSaturating(
	llvm::Value* v, llvm::Type* intTy, bool isSigned, llvm::IRBuilder<>& irb)
{
	unsigned bits = intTy->getScalarSizeInBits();
	auto* fpTy = v->getType();

	auto* loF = llvm::ConstantFP::get(fpTy, isSigned ? -std::ldexp(1.0, static_cast<int>(bits - 1)) : 0.0);
	auto* hiF = llvm::ConstantFP::get(fpTy, std::ldexp(1.0, static_cast<int>(isSigned ? bits - 1 : bits)));

	auto* aboveLo = irb.CreateFCmpOGE(v, loF);
	auto* inRange = irb.CreateAnd(aboveLo, irb.CreateFCmpOLT(v, hiF));

	// The conversion is fed an in-range value on every path, so the IR carries
	// no poison at all rather than poison that happens not to be selected.
	auto* safe = irb.CreateSelect(inRange, v, llvm::ConstantFP::get(fpTy, 0.0));
	auto* conv = isSigned ? irb.CreateFPToSI(safe, intTy) : irb.CreateFPToUI(safe, intTy);

	auto* minV =
		llvm::ConstantInt::get(intTy, isSigned ? llvm::APInt::getSignedMinValue(bits) : llvm::APInt::getZero(bits));
	auto* maxV =
		llvm::ConstantInt::get(intTy, isSigned ? llvm::APInt::getSignedMaxValue(bits) : llvm::APInt::getAllOnes(bits));

	return irb.CreateSelect(
		inRange,
		conv,
		irb.CreateSelect(
			irb.CreateFCmpUNO(v, v), llvm::ConstantInt::get(intTy, 0), irb.CreateSelect(aboveLo, maxV, minV)));
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::loadMemIndexTerm(cs_arm_op& op, llvm::IRBuilder<>& irb)
{
	auto* idx = loadRegister(op.mem.index, irb);
	if (idx == nullptr)
	{
		return nullptr;
	}
	auto* ty = llvm::cast<llvm::IntegerType>(idx->getType());
	unsigned w = ty->getBitWidth();

	unsigned k = op.shift.value;
	switch (op.shift.type)
	{
	case ARM_SFT_LSL:
		if (k > 0 && k < w) idx = irb.CreateShl(idx, llvm::ConstantInt::get(ty, k));
		break;
	case ARM_SFT_LSR:
		// LSR #0 encodes LSR #32, which empties the register.
		idx = (k == 0 || k >= w) ? llvm::cast<llvm::Value>(llvm::ConstantInt::get(ty, 0))
								 : irb.CreateLShr(idx, llvm::ConstantInt::get(ty, k));
		break;
	case ARM_SFT_ASR:
		// ASR #0 encodes ASR #32: the sign bit in every position, which is
		// the shift by w-1 the clamp produces.
		idx = irb.CreateAShr(idx, llvm::ConstantInt::get(ty, (k == 0 || k >= w) ? w - 1 : k));
		break;
	case ARM_SFT_ROR:
		if (k > 0 && k < w)
		{
			idx = irb.CreateOr(
				irb.CreateLShr(idx, llvm::ConstantInt::get(ty, k)),
				irb.CreateShl(idx, llvm::ConstantInt::get(ty, w - k)));
		}
		break;
	default: break;
	}

	// The sign is op.subtracted, not mem.scale; see loadOp().
	if (op.subtracted)
	{
		idx = irb.CreateNeg(idx);
	}
	return idx;
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateOperandShift(
		llvm::IRBuilder<>& irb,
		cs_arm_op& op,
		llvm::Value* val)
{
	if (op.shift.type == ARM_SFT_INVALID)
	{
		return val;
	}

	llvm::Value* n = nullptr;
	if (op.shift.type == ARM_SFT_ASR
			|| op.shift.type == ARM_SFT_LSL
			|| op.shift.type == ARM_SFT_LSR
			|| op.shift.type == ARM_SFT_ROR
			|| op.shift.type == ARM_SFT_RRX)
	{
		n = llvm::ConstantInt::get(val->getType(), op.shift.value);
	}
	else if (op.shift.type == ARM_SFT_ASR_REG
			|| op.shift.type == ARM_SFT_LSL_REG
			|| op.shift.type == ARM_SFT_LSR_REG
			|| op.shift.type == ARM_SFT_ROR_REG
			|| op.shift.type == ARM_SFT_RRX_REG)
	{
		n = loadRegister(op.shift.value, irb);
	}
	else
	{
		return val;
	}

	n = irb.CreateZExtOrTrunc(n, val->getType());

	switch (op.shift.type)
	{
		case ARM_SFT_ASR:
		case ARM_SFT_ASR_REG:
		{
			return generateShiftAsr(irb, val, n);
		}
		case ARM_SFT_LSL:
		case ARM_SFT_LSL_REG:
		{
			return generateShiftLsl(irb, val, n);
		}
		case ARM_SFT_LSR:
		case ARM_SFT_LSR_REG:
		{
			return generateShiftLsr(irb, val, n);
		}
		case ARM_SFT_ROR:
		case ARM_SFT_ROR_REG:
		{
			return generateShiftRor(irb, val, n);
		}
		case ARM_SFT_RRX:
		{
			return generateShiftRrx(irb, val, n);
		}
		case ARM_SFT_INVALID:
		default:
		{
			return val;
		}
	}
}

/**
 * ARM's register-controlled shifts are NOT modulo the operand width, which is
 * what both LLVM's shift instructions and RetDec's own emulator do. The
 * architecture reads the low EIGHT bits of the register -- `shift_n =
 * UInt(R[s]<7:0>)`, so 0 to 255 -- and defines every one of those values:
 *
 *   n == 0       the value is unchanged and the carry is unchanged
 *   1 <= n < 32  the ordinary shift; the carry is the last bit shifted out
 *   n == 32      LSL gives 0 with carry = bit 0; LSR gives 0 with carry =
 *                bit 31; ASR gives the sign broadcast with carry = bit 31
 *   n > 32       LSL and LSR give 0 with carry 0; ASR gives the sign
 *                broadcast with carry = bit 31
 *
 * Because the architecture's rule and LLVM's disagree, shifting by a raw `n`
 * was a wrong VALUE and not only poison: `lsl r0, r1, r2` with r2 = 32
 * answered r1 where the hardware answers 0. The carry was worse -- it was
 * computed from `n - 1`, so the commonest runtime count of all, zero, gave
 * `shl i32 %val, 0xffffffff`.
 *
 * Every shift below is by an amount masked to the width, so no poison is
 * emitted on any path; the out-of-range answers are selected in afterwards.
 */
llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftCommon(
	llvm::IRBuilder<>& irb, llvm::Value* val, llvm::Value* n, eShiftKind kind)
{
	auto* ty = llvm::cast<llvm::IntegerType>(val->getType());
	unsigned w = ty->getBitWidth();
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* one = llvm::ConstantInt::get(ty, 1);
	auto* widthC = llvm::ConstantInt::get(ty, w);
	auto* maskC = llvm::ConstantInt::get(ty, w - 1);

	// The low eight bits are the count. Harmless for the immediate forms,
	// whose count is already at most 32.
	n = irb.CreateAnd(irb.CreateZExtOrTrunc(n, ty), llvm::ConstantInt::get(ty, 0xff));

	// Most shifts carry an immediate count, and then the whole thing is known
	// here. Writing that case out keeps the IR free of selects -- and, for a
	// non-zero count, avoids reading CPSR_C at all, which the general path
	// below has to do in order to leave it alone when the count is zero.
	if (auto* cn = llvm::dyn_cast<llvm::ConstantInt>(n))
	{
		uint64_t k = cn->getZExtValue();
		if (k == 0)
		{
			// Neither the value nor the carry is touched.
			return val;
		}
		unsigned cIdx = 0;
		llvm::Value* cres = nullptr;
		bool carryIsZero = false;
		if (kind == eShiftKind::Lsl)
		{
			cres = k < w ? irb.CreateShl(val, llvm::ConstantInt::get(ty, k)) : llvm::cast<llvm::Value>(zero);
			cIdx = k <= w ? static_cast<unsigned>(w - k) : 0;
			carryIsZero = k > w;
		}
		else if (kind == eShiftKind::Lsr)
		{
			cres = k < w ? irb.CreateLShr(val, llvm::ConstantInt::get(ty, k)) : llvm::cast<llvm::Value>(zero);
			cIdx = k <= w ? static_cast<unsigned>(k - 1) : 0;
			carryIsZero = k > w;
		}
		else
		{
			unsigned amt = k < w ? static_cast<unsigned>(k) : w - 1;
			cres = irb.CreateAShr(val, llvm::ConstantInt::get(ty, amt));
			cIdx = k < w ? static_cast<unsigned>(k - 1) : w - 1;
		}
		llvm::Value* cv = carryIsZero ? llvm::cast<llvm::Value>(irb.getFalse())
									  : irb.CreateTrunc(
											irb.CreateAnd(irb.CreateLShr(val, llvm::ConstantInt::get(ty, cIdx)), one),
											irb.getInt1Ty());
		storeRegister(ARM_REG_CPSR_C, cv, irb);
		return cres;
	}

	auto* isZero = irb.CreateICmpEQ(n, zero);
	auto* below = irb.CreateICmpULT(n, widthC);
	auto* atMost = irb.CreateICmpULE(n, widthC);
	auto* safe = irb.CreateAnd(n, maskC);

	llvm::Value* res = nullptr;
	llvm::Value* cBit = nullptr;
	if (kind == eShiftKind::Lsl)
	{
		res = irb.CreateSelect(below, irb.CreateShl(val, safe), zero);
		// The last bit out is bit (w - n); at n == w that is bit 0, which the
		// mask below produces without a special case.
		auto* cShift = irb.CreateAnd(irb.CreateSub(widthC, n), maskC);
		cBit = irb.CreateAnd(irb.CreateLShr(val, cShift), one);
	}
	else if (kind == eShiftKind::Lsr)
	{
		res = irb.CreateSelect(below, irb.CreateLShr(val, safe), zero);
		auto* cShift = irb.CreateAnd(irb.CreateSub(n, one), maskC);
		cBit = irb.CreateAnd(irb.CreateLShr(val, cShift), one);
	}
	else
	{
		// ASR at or past the width is the sign bit broadcast, which is the
		// shift by w-1 that the clamp already produces.
		auto* amt = irb.CreateSelect(below, safe, maskC);
		res = irb.CreateAShr(val, amt);
		auto* cShift = irb.CreateSelect(atMost, irb.CreateAnd(irb.CreateSub(n, one), maskC), maskC);
		cBit = irb.CreateAnd(irb.CreateLShr(val, cShift), one);
	}

	// Past the width LSL and LSR shift everything out, so the carry is zero.
	// ASR keeps the sign bit there, which cShift above already selects.
	llvm::Value* carry = irb.CreateTrunc(cBit, irb.getInt1Ty());
	if (kind != eShiftKind::Asr)
	{
		carry = irb.CreateSelect(atMost, carry, irb.getFalse());
	}

	// A count of zero must leave the CARRY alone, and that needs a select --
	// a flag cannot be preserved without being read. The VALUE needs none:
	// shifting by zero is the identity for all three kinds, so `res` already
	// equals `val` there. A select for it would be unreachable, which a
	// mutation removing it duly proved by changing nothing.
	auto* oldC = irb.CreateZExtOrTrunc(loadRegister(ARM_REG_CPSR_C, irb), irb.getInt1Ty());
	storeRegister(ARM_REG_CPSR_C, irb.CreateSelect(isZero, oldC, carry), irb);
	return res;
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftAsr(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	return generateShiftCommon(irb, val, n, eShiftKind::Asr);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftLsl(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	return generateShiftCommon(irb, val, n, eShiftKind::Lsl);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftLsr(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	return generateShiftCommon(irb, val, n, eShiftKind::Lsr);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftRor(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	// The Batch AI rewrite covered LSL, LSR and ASR and left this one as it
	// was, so ROR with a register count kept all three of the defects AI
	// removed from its siblings:
	//
	//   * the count was not masked to eight bits, so `ror r0, r1, r2` with
	//     r2 = 0x100 emitted `lshr i32 %v, 256` -- poison;
	//   * a count of zero made the complement 32, so `shl i32 %v, 32` --
	//     poison again, on the single most likely runtime value;
	//   * the carry was written unconditionally, where the architecture
	//     leaves it alone when shift_n is zero.
	//
	// ARM ARM: shift_n = UInt(R[m]<7:0>); if shift_n is zero the value and
	// the carry are both unchanged; otherwise the rotation is by shift_n MOD
	// 32 and the carry is bit 31 of the result. A count of 32 is NOT a count
	// of zero -- it rotates by nothing and still writes the carry, the same
	// asymmetry x86's ROL has.
	auto* ty = llvm::cast<llvm::IntegerType>(val->getType());
	unsigned w = ty->getBitWidth();
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* maskC = llvm::ConstantInt::get(ty, w - 1);

	n = irb.CreateAnd(irb.CreateZExtOrTrunc(n, ty), llvm::ConstantInt::get(ty, 0xff));

	// The immediate forms are the common case and everything about them is
	// known here. Writing that case out keeps the IR free of selects and, more
	// to the point, avoids READING CPSR_C at all -- which the general path
	// below must do in order to leave the carry alone at a count of zero.
	// generateShiftCommon does the same for LSL, LSR and ASR, and the ROR
	// tests assert exactly this: `ror r0, r2, #5` loads r2 and nothing else.
	if (auto* cn = llvm::dyn_cast<llvm::ConstantInt>(n))
	{
		uint64_t k = cn->getZExtValue();
		if (k == 0)
		{
			// Neither the value nor the carry is touched.
			return val;
		}
		unsigned rotK = static_cast<unsigned>(k) & (w - 1);
		llvm::Value* res = rotK == 0 ? val
									 : llvm::cast<llvm::Value>(irb.CreateOr(
										   irb.CreateLShr(val, llvm::ConstantInt::get(ty, rotK)),
										   irb.CreateShl(val, llvm::ConstantInt::get(ty, w - rotK))));
		auto* cv =
			irb.CreateTrunc(irb.CreateAnd(irb.CreateLShr(res, maskC), llvm::ConstantInt::get(ty, 1)), irb.getInt1Ty());
		storeRegister(ARM_REG_CPSR_C, cv, irb);
		return res;
	}

	auto* isZero = irb.CreateICmpEQ(n, zero);

	// Masking the amount AND its complement keeps both shifts in range for
	// every count, the one that rotates by nothing included.
	auto* rot = irb.CreateAnd(n, maskC);
	auto* sub = irb.CreateAnd(irb.CreateSub(llvm::ConstantInt::get(ty, w), rot), maskC);
	auto* orr = irb.CreateOr(irb.CreateLShr(val, rot), irb.CreateShl(val, sub));

	auto* carry =
		irb.CreateTrunc(irb.CreateAnd(irb.CreateLShr(orr, maskC), llvm::ConstantInt::get(ty, 1)), irb.getInt1Ty());
	auto* oldC = irb.CreateZExtOrTrunc(loadRegister(ARM_REG_CPSR_C, irb), irb.getInt1Ty());
	storeRegister(ARM_REG_CPSR_C, irb.CreateSelect(isZero, oldC, carry), irb);

	// The VALUE needs no select: rotating by zero is the identity, so `orr`
	// already equals `val` there.
	return orr;
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftRrx(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(n->getType())->getBitWidth();
	auto* doubleT = llvm::Type::getIntNTy(_module->getContext(), op0BitW*2);

	auto* cf = loadRegister(ARM_REG_CPSR_C, irb);
	cf = irb.CreateZExtOrTrunc(cf, n->getType());

	auto* srl = irb.CreateLShr(val, n);
	auto* srlZext = irb.CreateZExt(srl, doubleT);
	auto* op0Zext = irb.CreateZExt(val, doubleT);
	auto* sub = irb.CreateSub(llvm::ConstantInt::get(n->getType(), op0BitW + 1), n);
	auto* subZext = irb.CreateZExt(sub, doubleT);
	auto* shl = irb.CreateShl(op0Zext, subZext);
	auto* sub2 = irb.CreateSub(llvm::ConstantInt::get(n->getType(), op0BitW), n);
	auto* shl2 = irb.CreateShl(cf, sub2);
	auto* shl2Zext = irb.CreateZExt(shl2, doubleT);
	auto* or1 = irb.CreateOr(shl, srlZext);
	auto* or2 = irb.CreateOr(or1, shl2Zext);
	auto* or2Trunc = irb.CreateTrunc(or2, val->getType());

	auto* sub3 = irb.CreateSub(n, llvm::ConstantInt::get(n->getType(), 1));
	auto* shl3 = irb.CreateShl(llvm::ConstantInt::get(sub3->getType(), 1), sub3);
	auto* and1 = irb.CreateAnd(shl3, val);
	auto* cfIcmp = irb.CreateICmpNE(and1, llvm::ConstantInt::get(and1->getType(), 0));
	storeRegister(ARM_REG_CPSR_C, cfIcmp, irb);

	return or2Trunc;
}

/**
 * We cannot use some sysreg ID numbers -> translate them to other ID numbers.
 * See comment for @c arm_sysreg_extension for more details.
 */
uint32_t Capstone2LlvmIrTranslatorArm_impl::sysregNumberTranslation(uint32_t r)
{
	if (ARM_SYSREG_SPSR_C <= r
			&& r <= (ARM_SYSREG_SPSR_C | ARM_SYSREG_SPSR_X | ARM_SYSREG_SPSR_S | ARM_SYSREG_SPSR_F))
	{
		return ARM_SYSREG_SPSR;
	}
	else if (ARM_SYSREG_CPSR_C <= r
			&& r <= (ARM_SYSREG_CPSR_C | ARM_SYSREG_CPSR_X | ARM_SYSREG_CPSR_S | ARM_SYSREG_CPSR_F))
	{
		return ARM_SYSREG_CPSR;
	}
	else
	{
		return r;
	}
}

//
//==============================================================================
// Scalar VFP.
//==============================================================================
//
// All 149 ARM_INS_V* entries were nullptr, so on 32-bit ARM every floating
// point instruction was an opaque __asm_* call -- while arm_init.cpp models 32
// S registers as f32 and 32 D registers as f64. A six-line dot product built
// for arm-linux-gnueabihf emits vldr, vstr, vmul.f64, vadd.f64 and vmov.f64 in
// one function, and none of them reached the rest of the decompiler.
//
// Scalar only. NEON needs a vector model this translator does not have, and
// cs_arm::vector_data says which is which: F32 and F64 for the scalar forms,
// I8..U64 for the lanewise ones. Anything lanewise, anything on a Q register,
// goes to the pseudo-asm fallback -- the same answer arm64 gives through
// ifVectorGeneratePseudo, and an honest one, because a lanewise add is not a
// scalar add.
//

bool Capstone2LlvmIrTranslatorArm_impl::isFpRegister(uint32_t r)
{
	return (r >= ARM_REG_S0 && r <= ARM_REG_S31) || (r >= ARM_REG_D0 && r <= ARM_REG_D31);
}

/**
 * @brief Is this the scalar VFP form, as opposed to a NEON one?
 *
 * Every register operand must be an S or a D -- never a Q -- and the data type
 * must be the single or double float, or unset, which is what VLDR, VSTR and
 * VMOV carry.
 */
bool Capstone2LlvmIrTranslatorArm_impl::isScalarVfp(cs_arm* ai)
{
	if (ai->vector_data != ARM_VECTORDATA_INVALID && ai->vector_data != ARM_VECTORDATA_F32
		&& ai->vector_data != ARM_VECTORDATA_F64)
	{
		return false;
	}
	bool sawD = false;
	bool sawS = false;
	for (unsigned j = 0; j < ai->op_count; ++j)
	{
		auto& op = ai->operands[j];
		if (op.type != ARM_OP_REG)
		{
			continue;
		}
		if (isQuadView(op.reg) || op.vector_index >= 0 || op.neon_lane >= 0)
		{
			return false;
		}
		if (op.reg >= ARM_REG_D0 && op.reg <= ARM_REG_D31)
		{
			sawD = true;
		}
		if (op.reg >= ARM_REG_S0 && op.reg <= ARM_REG_S31)
		{
			sawS = true;
		}
	}
	// vadd.f32 on D registers is NEON 2xf32, not a scalar f64 add.
	if (sawD && ai->vector_data == ARM_VECTORDATA_F32)
	{
		return false;
	}
	(void)sawS;
	return true;
}

/**
 * The type a VFP operand is read and written at: f32 for an S register, f64
 * for a D. VLDR, VSTR and VMOV carry no vector_data, so the register is the
 * only thing that says.
 */
llvm::Type* Capstone2LlvmIrTranslatorArm_impl::vfpTypeOfReg(uint32_t r, llvm::IRBuilder<>& irb)
{
	if (r >= ARM_REG_D0 && r <= ARM_REG_D31)
	{
		return irb.getDoubleTy();
	}
	return irb.getFloatTy();
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::loadVfpOp(cs_arm_op& op, llvm::IRBuilder<>& irb, llvm::Type* ty)
{
	auto* v = loadOp(op, irb, ty);
	return generateTypeConversion(irb, v, ty, eOpConv::FPCAST_OR_BITCAST);
}

unsigned Capstone2LlvmIrTranslatorArm_impl::neonBitWidth(uint32_t r)
{
	if (isQuadView(r))
	{
		return 128u;
	}
	if (r >= ARM_REG_S0 && r <= ARM_REG_S31)
	{
		return 32u;
	}
	return 64u;
}

unsigned Capstone2LlvmIrTranslatorArm_impl::vectorDataLaneBits(arm_vectordata_type vd, bool& isFp, bool& isSigned)
{
	isFp = false;
	isSigned = false;
	switch (vd)
	{
	case ARM_VECTORDATA_I8:
	case ARM_VECTORDATA_U8: return 8;
	case ARM_VECTORDATA_S8: isSigned = true; return 8;
	case ARM_VECTORDATA_I16:
	case ARM_VECTORDATA_U16: return 16;
	case ARM_VECTORDATA_S16: isSigned = true; return 16;
	case ARM_VECTORDATA_I32:
	case ARM_VECTORDATA_U32: return 32;
	case ARM_VECTORDATA_S32: isSigned = true; return 32;
	case ARM_VECTORDATA_I64:
	case ARM_VECTORDATA_U64: return 64;
	case ARM_VECTORDATA_S64: isSigned = true; return 64;
	case ARM_VECTORDATA_F16: isFp = true; return 16;
	case ARM_VECTORDATA_F32: isFp = true; return 32;
	case ARM_VECTORDATA_F64: isFp = true; return 64;
	default: return 0;
	}
}

int Capstone2LlvmIrTranslatorArm_impl::operandLane(const cs_arm_op& op)
{
	if (op.vector_index >= 0)
	{
		return op.vector_index;
	}
	return op.neon_lane;
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::loadNeonBits(uint32_t r, llvm::IRBuilder<>& irb)
{
	if (isQuadView(r))
	{
		auto* i128 = irb.getIntNTy(128);
		return generateTypeConversion(irb, loadQuadView(r, irb), i128, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}
	auto* w = irb.getIntNTy(neonBitWidth(r));
	return generateTypeConversion(irb, loadRegister(r, irb), w, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

llvm::Instruction* Capstone2LlvmIrTranslatorArm_impl::storeNeonBits(uint32_t r, llvm::Value* bits, llvm::IRBuilder<>& irb)
{
	if (isQuadView(r))
	{
		return storeQuadView(r, bits, irb);
	}
	return storeRegister(r, bits, irb, eOpConv::FPCAST_OR_BITCAST);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::loadNeonVector(
	uint32_t r, unsigned laneBits, bool fp, llvm::IRBuilder<>& irb)
{
	unsigned width = neonBitWidth(r);
	unsigned lanes = laneBits ? width / laneBits : 0;
	if (lanes == 0 || width % laneBits != 0)
	{
		return llvm::UndefValue::get(irb.getIntNTy(width));
	}
	llvm::Type* elem = nullptr;
	if (fp && laneBits == 64)
	{
		elem = irb.getDoubleTy();
	}
	else if (fp && laneBits == 32)
	{
		elem = irb.getFloatTy();
	}
	else
	{
		elem = irb.getIntNTy(laneBits);
	}
	auto* vecTy = llvm::FixedVectorType::get(elem, lanes);
	return irb.CreateBitCast(loadNeonBits(r, irb), vecTy);
}

llvm::Instruction*
Capstone2LlvmIrTranslatorArm_impl::storeNeonVector(uint32_t r, llvm::Value* vec, llvm::IRBuilder<>& irb)
{
	auto* w = irb.getIntNTy(neonBitWidth(r));
	return storeNeonBits(r, irb.CreateBitCast(vec, w), irb);
}

bool Capstone2LlvmIrTranslatorArm_impl::neonBinaryRegs(cs_arm* ai, unsigned nRegs, unsigned& bits)
{
	bits = 0;
	if (ai->op_count < nRegs)
	{
		return false;
	}
	for (unsigned j = 0; j < nRegs; ++j)
	{
		if (ai->operands[j].type != ARM_OP_REG || !isNeonRegister(ai->operands[j].reg)
			|| operandLane(ai->operands[j]) >= 0)
		{
			return false;
		}
		unsigned w = neonBitWidth(ai->operands[j].reg);
		if (bits == 0)
		{
			bits = w;
		}
		else if (bits != w)
		{
			return false;
		}
	}
	return bits != 0;
}

/**
 * ARM_INS_VLDR, ARM_INS_VSTR
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpLoadStore(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (!isScalarVfp(ai) || ai->operands[0].type != ARM_OP_REG || !isFpRegister(ai->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);

	if (i->id == ARM_INS_VLDR)
	{
		op1 = loadOp(ai->operands[1], irb, ty);
		storeOp(ai->operands[0], op1, irb, eOpConv::FPCAST_OR_BITCAST);
	}
	else
	{
		op0 = loadVfpOp(ai->operands[0], irb, ty);
		storeOp(ai->operands[1], op0, irb);
	}
}

/**
 * ARM_INS_VADD, ARM_INS_VSUB, ARM_INS_VMUL, ARM_INS_VDIV, ARM_INS_VNMUL
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpArithm(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (isScalarVfp(ai) && isFpRegister(ai->operands[0].reg))
	{
		auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);
		op1 = loadVfpOp(ai->operands[1], irb, ty);
		op2 = loadVfpOp(ai->operands[2], irb, ty);

		llvm::Value* val = nullptr;
		switch (i->id)
		{
		case ARM_INS_VADD: val = irb.CreateFAdd(op1, op2); break;
		case ARM_INS_VSUB: val = irb.CreateFSub(op1, op2); break;
		case ARM_INS_VMUL: val = irb.CreateFMul(op1, op2); break;
		case ARM_INS_VDIV: val = irb.CreateFDiv(op1, op2); break;
		case ARM_INS_VNMUL: val = irb.CreateFNeg(irb.CreateFMul(op1, op2)); break;
		default: return;
		}
		storeOp(ai->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}

	if (i->id == ARM_INS_VDIV || i->id == ARM_INS_VNMUL)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	translateNeonArith(i, ai, irb);
}

/**
 * ARM_INS_VNEG, ARM_INS_VABS, ARM_INS_VSQRT
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpUnary(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (isScalarVfp(ai) && isFpRegister(ai->operands[0].reg))
	{
		auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);
		op1 = loadVfpOp(ai->operands[1], irb, ty);

		llvm::Value* val = nullptr;
		switch (i->id)
		{
		case ARM_INS_VNEG: val = irb.CreateFNeg(op1); break;
		case ARM_INS_VABS:
			val = irb.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, ty), {op1});
			break;
		case ARM_INS_VSQRT:
			val = irb.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::sqrt, ty), {op1});
			break;
		default: return;
		}
		storeOp(ai->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}

	if (i->id == ARM_INS_VSQRT)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (ai->op_count != 2 || !neonBinaryRegs(ai, 2, bits) || laneBits == 0 || bits % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* v = loadNeonVector(ai->operands[1].reg, laneBits, isFp, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(v->getType());
	unsigned lanes = vecTy->getNumElements();
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* e = irb.CreateExtractElement(v, k);
		llvm::Value* o = nullptr;
		if (i->id == ARM_INS_VNEG)
		{
			o = isFp ? irb.CreateFNeg(e) : irb.CreateNeg(e);
		}
		else
		{
			o = isFp ? irb.CreateCall(
						  llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, e->getType()), {e})
					 : irb.CreateSelect(
						   irb.CreateICmpSLT(e, llvm::ConstantInt::get(e->getType(), 0)), irb.CreateNeg(e), e);
		}
		res = irb.CreateInsertElement(res, o, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

/**
 * The multiply-accumulate forms, where the destination is also a source:
 * ARM_INS_VMLA, ARM_INS_VMLS, ARM_INS_VNMLA, ARM_INS_VNMLS,
 * ARM_INS_VFMA, ARM_INS_VFMS, ARM_INS_VFNMA, ARM_INS_VFNMS
 *
 * The VF* forms are fused, the others are not; both are llvm.fma here, because
 * the difference is a double rounding this IR has no way to express.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpMla(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	if (isScalarVfp(ai) && isFpRegister(ai->operands[0].reg))
	{
		auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);
		auto* acc = loadVfpOp(ai->operands[0], irb, ty);
		op1 = loadVfpOp(ai->operands[1], irb, ty);
		op2 = loadVfpOp(ai->operands[2], irb, ty);

		bool negProduct =
			(i->id == ARM_INS_VMLS || i->id == ARM_INS_VFMS || i->id == ARM_INS_VNMLA || i->id == ARM_INS_VFNMA);
		bool negAcc =
			(i->id == ARM_INS_VNMLA || i->id == ARM_INS_VNMLS || i->id == ARM_INS_VFNMA || i->id == ARM_INS_VFNMS);

		auto* fma = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fma, ty);
		llvm::Value* a = negProduct ? irb.CreateFNeg(op1) : op1;
		llvm::Value* c = negAcc ? irb.CreateFNeg(acc) : acc;
		llvm::Value* val = irb.CreateCall(fma, {a, op2, c});

		storeOp(ai->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}

	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (!neonBinaryRegs(ai, 3, bits) || laneBits == 0 || bits % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* acc = loadNeonVector(ai->operands[0].reg, laneBits, isFp, irb);
	llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, isFp, irb);
	llvm::Value* b = loadNeonVector(ai->operands[2].reg, laneBits, isFp, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(acc->getType());
	unsigned lanes = vecTy->getNumElements();
	bool sub = (i->id == ARM_INS_VMLS || i->id == ARM_INS_VFMS);
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* ea = irb.CreateExtractElement(acc, k);
		llvm::Value* e1 = irb.CreateExtractElement(a, k);
		llvm::Value* e2 = irb.CreateExtractElement(b, k);
		llvm::Value* prod = isFp ? irb.CreateFMul(e1, e2) : irb.CreateMul(e1, e2);
		llvm::Value* o = isFp ? (sub ? irb.CreateFSub(ea, prod) : irb.CreateFAdd(ea, prod))
							  : (sub ? irb.CreateSub(ea, prod) : irb.CreateAdd(ea, prod));
		res = irb.CreateInsertElement(res, o, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

/**
 * ARM_INS_VCMP, ARM_INS_VCMPE
 *
 * These write FPSCR[31:28], and the vmrs that follows copies them into CPSR.
 * The flags are not the integer ones: for an ordered compare N is less-than, Z
 * is equal, C is greater-or-equal-or-unordered, and V is unordered.
 *
 * The second operand is an IMM 0 in the compare-with-zero form, not an FP
 * operand -- measured, not assumed -- so the zero is built from the first
 * operand's type.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpCmp(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (!isScalarVfp(ai) || !isFpRegister(ai->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);
	op0 = loadVfpOp(ai->operands[0], irb, ty);

	llvm::Value* rhs = nullptr;
	if (ai->operands[1].type == ARM_OP_IMM || ai->operands[1].type == ARM_OP_FP)
	{
		rhs = llvm::ConstantFP::get(ty, 0.0);
	}
	else
	{
		rhs = loadVfpOp(ai->operands[1], irb, ty);
	}

	auto* n = irb.CreateFCmpOLT(op0, rhs);
	auto* z = irb.CreateFCmpOEQ(op0, rhs);
	auto* v = irb.CreateFCmpUNO(op0, rhs);
	auto* c = irb.CreateOr(irb.CreateFCmpOGE(op0, rhs), v);

	auto* i32 = getDefaultType();
	auto* packed = irb.CreateOr(
		irb.CreateOr(irb.CreateShl(irb.CreateZExt(n, i32), 31), irb.CreateShl(irb.CreateZExt(z, i32), 30)),
		irb.CreateOr(irb.CreateShl(irb.CreateZExt(c, i32), 29), irb.CreateShl(irb.CreateZExt(v, i32), 28)));
	storeRegister(ARM_REG_FPSCR_NZCV, packed, irb);
}

/**
 * ARM_INS_VMRS, ARM_INS_FMSTAT
 *
 * `vmrs APSR_nzcv, fpscr` unpacks what the compare above packed. Both operands
 * are marked access=0 by capstone, so this keys on the register ids rather
 * than on the access flags.
 *
 * Wired under two ids, and the second is the one that matters: capstone 5.0.9
 * decodes this exact encoding as ARM_INS_FMSTAT (58), the pre-UAL alias, while
 * printing the mnemonic as "vmrs". ARM_INS_VMRS (378) is the general
 * `vmrs rN, fpscr` form. Wiring only VMRS looks like a fix and does nothing --
 * which is what it did, until a test said so.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVmrs(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count != 2 || ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG
		|| ai->operands[0].reg != ARM_REG_APSR_NZCV
		|| !(ai->operands[1].reg == ARM_REG_FPSCR || ai->operands[1].reg == ARM_REG_FPSCR_NZCV))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}


	auto* packed = loadRegister(ARM_REG_FPSCR_NZCV, irb);
	auto bit = [&](unsigned n) -> llvm::Value* {
		return irb.CreateTrunc(irb.CreateLShr(packed, llvm::ConstantInt::get(packed->getType(), n)), irb.getInt1Ty());
	};
	storeRegister(ARM_REG_CPSR_N, bit(31), irb);
	storeRegister(ARM_REG_CPSR_Z, bit(30), irb);
	storeRegister(ARM_REG_CPSR_C, bit(29), irb);
	storeRegister(ARM_REG_CPSR_V, bit(28), irb);
}

/**
 * ARM_INS_VCVT, ARM_INS_VCVTR
 *
 * cs_arm::vector_data names both ends of the conversion -- F64S32 is "to f64
 * from s32" -- which is the only thing that says whether the S register on
 * either side holds a number or an integer bit pattern.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpCvt(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG || !isFpRegister(ai->operands[0].reg)
		|| !isFpRegister(ai->operands[1].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* f32 = irb.getFloatTy();
	auto* f64 = irb.getDoubleTy();
	auto* i32 = irb.getInt32Ty();

	llvm::Type* dstTy = nullptr; // what lands in the destination register
	llvm::Type* srcTy = nullptr; // how the source register is read
	bool toInt = false, fromInt = false, isSigned = true;

	switch (ai->vector_data)
	{
	case ARM_VECTORDATA_F64F32:
		srcTy = f32;
		dstTy = f64;
		break;
	case ARM_VECTORDATA_F32F64:
		srcTy = f64;
		dstTy = f32;
		break;
	case ARM_VECTORDATA_F64S32:
		srcTy = i32;
		dstTy = f64;
		fromInt = true;
		break;
	case ARM_VECTORDATA_F32S32:
		srcTy = i32;
		dstTy = f32;
		fromInt = true;
		break;
	case ARM_VECTORDATA_F64U32:
		srcTy = i32;
		dstTy = f64;
		fromInt = true;
		isSigned = false;
		break;
	case ARM_VECTORDATA_F32U32:
		srcTy = i32;
		dstTy = f32;
		fromInt = true;
		isSigned = false;
		break;
	case ARM_VECTORDATA_S32F64:
		srcTy = f64;
		dstTy = i32;
		toInt = true;
		break;
	case ARM_VECTORDATA_S32F32:
		srcTy = f32;
		dstTy = i32;
		toInt = true;
		break;
	case ARM_VECTORDATA_U32F64:
		srcTy = f64;
		dstTy = i32;
		toInt = true;
		isSigned = false;
		break;
	case ARM_VECTORDATA_U32F32:
		srcTy = f32;
		dstTy = i32;
		toInt = true;
		isSigned = false;
		break;
	default:
		// The half-precision pairs and everything NEON.
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	// The source register is read at its own width and then reinterpreted as
	// whatever this conversion says it holds: an S register carrying an s32 is
	// still an f32 global.
	auto* raw = loadOp(ai->operands[1], irb);
	llvm::Value* src = fromInt ? generateTypeConversion(irb, raw, srcTy, eOpConv::ZEXT_TRUNC_OR_BITCAST)
							   : generateTypeConversion(irb, raw, srcTy, eOpConv::FPCAST_OR_BITCAST);

	llvm::Value* val = nullptr;
	if (fromInt)
	{
		val = isSigned ? irb.CreateSIToFP(src, dstTy) : irb.CreateUIToFP(src, dstTy);
	}
	else if (toInt)
	{
		// VCVT truncates toward zero; VCVTR follows FPSCR[RN], whose reset
		// value and ABI setting is round-to-nearest-even.
		if (i->id == ARM_INS_VCVTR)
		{
			src = irb.CreateCall(
				llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::roundeven, src->getType()), {src});
		}
		// A32 uses the same FPToFixed() as A64: a NaN converts to zero and an
		// out-of-range magnitude SATURATES to the destination's minimum or
		// maximum. LLVM calls all three poison. See the ARM64 translator's
		// generateFpToIntSaturating for the reasoning; this is the same rule
		// written out here because the two translators share no base class.
		val = generateFpToIntSaturating(src, dstTy, isSigned, irb);
	}
	else
	{
		val = irb.CreateFPCast(src, dstTy);
	}

	// FPCAST_OR_BITCAST either way: when the destination is an S register
	// holding an integer, the bitcast is what puts the bit pattern there.
	storeOp(ai->operands[0], val, irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * ARM_INS_VMOV
 *
 * Several shapes, all measured against a real build:
 *   vmov s15, r3      2 regs, S <- GPR: a bit pattern move, not a conversion
 *   vmov r0, s15      2 regs, GPR <- S: the same the other way
 *   vmov d6, r0, r1   3 regs, D <- GPR pair, low half first
 *   vmov r0, r1, d6   3 regs, GPR pair <- D
 *   vmov.f64 d7, d8   2 regs, both FP: a plain move
 *   vmov.f64 d7, #1.0 reg + FP immediate
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpMov(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count == 2 && ai->operands[0].type == ARM_OP_REG && ai->operands[1].type == ARM_OP_REG)
	{
		int dstLane = operandLane(ai->operands[0]);
		int srcLane = operandLane(ai->operands[1]);
		bool dstN = isNeonRegister(ai->operands[0].reg);
		bool srcN = isNeonRegister(ai->operands[1].reg);
		if (dstLane >= 0 || srcLane >= 0)
		{
			bool isFp = false, isSigned = false;
			unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
			if (laneBits == 0)
			{
				laneBits = 32;
			}
			if (dstN && dstLane >= 0 && !srcN)
			{
				llvm::Value* v = loadNeonVector(ai->operands[0].reg, laneBits, false, irb);
				auto* laneTy = llvm::cast<llvm::VectorType>(v->getType())->getElementType();
				auto* ins = irb.CreateZExtOrTrunc(loadOp(ai->operands[1], irb), laneTy);
				storeNeonVector(ai->operands[0].reg, irb.CreateInsertElement(v, ins, static_cast<uint64_t>(dstLane)), irb);
				return;
			}
			if (srcN && srcLane >= 0 && !dstN)
			{
				llvm::Value* v = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
				auto* lane = irb.CreateExtractElement(v, static_cast<uint64_t>(srcLane));
				storeOp(ai->operands[0], irb.CreateZExtOrTrunc(lane, getDefaultType()), irb);
				return;
			}
			if (dstN && srcN && srcLane >= 0 && dstLane < 0)
			{
				llvm::Value* srcV = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
				llvm::Value* lane = irb.CreateExtractElement(srcV, static_cast<uint64_t>(srcLane));
				llvm::Value* dstV = loadNeonVector(ai->operands[0].reg, laneBits, false, irb);
				storeNeonVector(ai->operands[0].reg, irb.CreateInsertElement(dstV, lane, static_cast<uint64_t>(0)), irb);
				return;
			}
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		if (dstN && srcN)
		{
			unsigned dw = neonBitWidth(ai->operands[0].reg);
			unsigned sw = neonBitWidth(ai->operands[1].reg);
			auto* bits = loadNeonBits(ai->operands[1].reg, irb);
			if (dw != sw)
			{
				auto* dt = irb.getIntNTy(dw);
				bits = dw > sw ? irb.CreateZExt(bits, dt) : irb.CreateTrunc(bits, dt);
			}
			storeNeonBits(ai->operands[0].reg, bits, irb);
			return;
		}
	}

	if (!isScalarVfp(ai))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();

	// vmov d6, r0, r1 / vmov r0, r1, d6
	if (ai->op_count == 3 && ai->operands[0].type == ARM_OP_REG && ai->operands[1].type == ARM_OP_REG
		&& ai->operands[2].type == ARM_OP_REG)
	{
		if (isFpRegister(ai->operands[0].reg))
		{
			auto* lo = irb.CreateZExt(irb.CreateZExtOrTrunc(loadOp(ai->operands[1], irb), i32), i64);
			auto* hi = irb.CreateZExt(irb.CreateZExtOrTrunc(loadOp(ai->operands[2], irb), i32), i64);
			auto* bits = irb.CreateOr(lo, irb.CreateShl(hi, llvm::ConstantInt::get(i64, 32)));
			storeOp(ai->operands[0], bits, irb, eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		if (isFpRegister(ai->operands[2].reg))
		{
			auto* bits = generateTypeConversion(irb, loadOp(ai->operands[2], irb), i64, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			storeOp(ai->operands[0], irb.CreateTrunc(bits, i32), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
			storeOp(
				ai->operands[1],
				irb.CreateTrunc(irb.CreateLShr(bits, llvm::ConstantInt::get(i64, 32)), i32),
				irb,
				eOpConv::ZEXT_TRUNC_OR_BITCAST);
			return;
		}
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	if (ai->op_count != 2 || ai->operands[0].type != ARM_OP_REG)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	bool dstFp = isFpRegister(ai->operands[0].reg);
	auto& src = ai->operands[1];

	if (src.type == ARM_OP_FP)
	{
		auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);
		storeOp(ai->operands[0], llvm::ConstantFP::get(ty, src.fp), irb, eOpConv::FPCAST_OR_BITCAST);
		return;
	}

	if (src.type != ARM_OP_REG)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	bool srcFp = isFpRegister(src.reg);
	auto* val = loadOp(src, irb);

	if (dstFp && srcFp)
	{
		auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);
		storeOp(ai->operands[0], generateTypeConversion(irb, val, ty, eOpConv::FPCAST_OR_BITCAST), irb);
		return;
	}
	// One side is a GPR: this moves the bits, it does not convert the number.
	storeOp(ai->operands[0], val, irb, dstFp ? eOpConv::FPCAST_OR_BITCAST : eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::loadOp(
		cs_arm_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case ARM_OP_SYSREG:
		{
			auto* val = loadRegister(sysregNumberTranslation(op.reg), irb);
			return val
				? generateOperandShift(irb, op, val)
				: llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case ARM_OP_REG:
		{
			auto* val = loadRegister(op.reg, irb);
			return val
				? generateOperandShift(irb, op, val)
				: llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case ARM_OP_IMM:
		case ARM_OP_PIMM:
		case ARM_OP_CIMM:
		{
			auto* t = getDefaultType();
			auto* val = llvm::ConstantInt::get(t, llvm::APInt(t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm), false, /*implicitTrunc=*/true));
			return generateOperandShift(irb, op, val);
		}
		case ARM_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
			auto* t = baseR ? baseR->getType() : getDefaultType();
			llvm::Value* disp = op.mem.disp
					? llvm::ConstantInt::getSigned(t, op.mem.disp)
					: nullptr;

			auto* idxR = loadRegister(op.mem.index, irb);
			if (idxR)
			{
				// The sign of a register offset is op.subtracted, NOT
				// mem.scale. arm.h documents scale as 1 or -1, but this
				// capstone never writes -1 for ARM: disassembling
				// `ldr r0, [r1, -r2]` gives scale = 1 and subtracted = 1, and
				// so does every other negated-index form. So this branch was
				// dead and the minus sign was dropped -- `ldr r0, [r1, -r2]`
				// loaded from r1 + r2.
				if (op.subtracted)
				{
					idxR = irb.CreateNeg(idxR);
				}

				// If there is a shift in memory operand, it is applied to
				// the index register. Capstone 6.x has no mem.lshift; the
				// amount lives in op.shift.
				idxR = generateOperandShift(irb, op, idxR);
			}
			else if (op.subtracted && disp)
			{
				// Capstone 6.x reports `ldr r0, [r1, #-8]` as disp=+8 and
				// subtracted=1 (5.x wrote disp=-8). Apply the minus here
				// only when there is no index register, so a negative disp
				// that already carries the sign is left alone.
				disp = irb.CreateNeg(disp);
			}

			// Capstone 6.x: post-indexed writeback still puts the offset
			// in the MEM operand; the access itself is through the raw base.
			if (!lea && _insn && _insn->detail && _insn->detail->arm.post_index)
			{
				disp = nullptr;
				idxR = nullptr;
			}

			llvm::Value* addr = nullptr;
			if (baseR && disp == nullptr)
			{
				addr = baseR;
			}
			else if (disp && baseR == nullptr)
			{
				addr = disp;
			}
			else if (baseR && disp)
			{
				disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
				addr = irb.CreateAdd(baseR, disp);
			}
			else if (idxR)
			{
				addr = idxR;
			}
			else
			{
				addr = llvm::ConstantInt::get(getDefaultType(), 0);
			}

			if (idxR && addr != idxR)
			{
				idxR = irb.CreateZExtOrTrunc(idxR, addr->getType());
				addr = irb.CreateAdd(addr, idxR);
			}

			if (lea)
			{
				return addr;
			}
			else
			{
				auto* lty = ty ? ty : getDefaultType();
				return loadIntPtr(irb, addr, lty);
			}
		}
		case ARM_OP_FP:
		{
			auto* val = llvm::ConstantFP::get(irb.getFloatTy(), op.fp);
			return generateOperandShift(irb, op, val);
		}
		case ARM_OP_SETEND:
		{
			return llvm::UndefValue::get(getDefaultType());
		}
		case ARM_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::Instruction* Capstone2LlvmIrTranslatorArm_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == ARM_REG_INVALID)
	{
		return nullptr;
	}

	// ARM allows direct write into Program Counter register -> uncond branch.
	//
	if (r == ARM_REG_PC)
	{
		return generateBranchFunctionCall(irb, val);
	}

	if (isSingleView(r))
	{
		return storeSingleView(r, val, irb);
	}

	if (isQuadView(r))
	{
		return storeQuadView(r, val, irb);
	}

	auto* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("storeRegister() unhandled reg.");
	}
	if (llvmReg->getValueType()->isFloatingPointTy())
	{
		switch (ct)
		{
			case eOpConv::SITOFP_OR_FPCAST:
			case eOpConv::UITOFP_OR_FPCAST:
				val = generateTypeConversion(irb, val, llvmReg->getValueType(), ct);
				break;
			default:
				val = generateTypeConversion(irb, val, llvmReg->getValueType(), eOpConv::FPCAST_OR_BITCAST);
		}
	}
	else
	{
		switch (ct)
		{
			case eOpConv::SEXT_TRUNC_OR_BITCAST:
			case eOpConv::ZEXT_TRUNC_OR_BITCAST:
				val = generateTypeConversion(irb, val, llvmReg->getValueType(), ct);
				break;
			default:
				val = generateTypeConversion(irb, val, llvmReg->getValueType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
		}
	}

	auto* s = irb.CreateStore(val, llvmReg);
	attachPointeeType(s, llvmReg->getValueType());
	return s;
}

llvm::Instruction* Capstone2LlvmIrTranslatorArm_impl::storeOp(
		cs_arm_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (op.type != ARM_OP_MEM && op.shift.type != ARM_SFT_INVALID)
	{
		throw GenericError("Unhandled situation in storeOp().");
	}

	switch (op.type)
	{
		case ARM_OP_SYSREG:
		{
			return storeRegister(sysregNumberTranslation(op.reg), val, irb, ct);
		}
		case ARM_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case ARM_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
			auto* t = baseR ? baseR->getType() : getDefaultType();
			llvm::Value* disp = op.mem.disp
					? llvm::ConstantInt::getSigned(t, op.mem.disp)
					: nullptr;

			auto* idxR = loadRegister(op.mem.index, irb);
			if (idxR)
			{
				// The sign of a register offset is op.subtracted, NOT
				// mem.scale. arm.h documents scale as 1 or -1, but this
				// capstone never writes -1 for ARM: disassembling
				// `ldr r0, [r1, -r2]` gives scale = 1 and subtracted = 1, and
				// so does every other negated-index form. So this branch was
				// dead and the minus sign was dropped -- `ldr r0, [r1, -r2]`
				// loaded from r1 + r2.
				if (op.subtracted)
				{
					idxR = irb.CreateNeg(idxR);
				}

				// If there is a shift in memory operand, it is applied to
				// the index register.
				idxR = generateOperandShift(irb, op, idxR);
			}
			else if (op.subtracted && disp)
			{
				disp = irb.CreateNeg(disp);
			}

			if (_insn && _insn->detail && _insn->detail->arm.post_index)
			{
				disp = nullptr;
				idxR = nullptr;
			}

			llvm::Value* addr = nullptr;
			if (baseR && disp == nullptr)
			{
				addr = baseR;
			}
			else if (disp && baseR == nullptr)
			{
				addr = disp;
			}
			else if (baseR && disp)
			{
				disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
				addr = irb.CreateAdd(baseR, disp);
			}
			else if (idxR)
			{
				addr = idxR;
			}
			else
			{
				addr = llvm::ConstantInt::get(getDefaultType(), 0);
			}

			if (idxR && addr != idxR)
			{
				idxR = irb.CreateZExtOrTrunc(idxR, addr->getType());
				addr = irb.CreateAdd(addr, idxR);
			}

			return storeIntPtr(irb, val, addr, val->getType());
		}
		case ARM_OP_PIMM:
		case ARM_OP_CIMM:
		{
			return nullptr;
		}
		case ARM_OP_FP:
		case ARM_OP_IMM:
		case ARM_OP_SETEND:
		case ARM_OP_INVALID:
		default:
		{
			throw GenericError("unhandled value");
		}
	}
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateInsnConditionCode(
		llvm::IRBuilder<>& irb,
		cs_arm* ai)
{
	switch (ai->cc)
	{
		// Equal = Zero set
		case ARM_CC_EQ:
		{
			auto* z = loadRegister(ARM_REG_CPSR_Z, irb);
			return z;
		}
		// Not equal = Zero clear
		case ARM_CC_NE:
		{
			auto* z = loadRegister(ARM_REG_CPSR_Z, irb);
			return generateValueNegate(irb, z);
		}
		// Unsigned higher or same = Carry set
		case ARM_CC_HS:
		{
			auto* c = loadRegister(ARM_REG_CPSR_C, irb);
			return c;
		}
		// Unsigned lower = Carry clear
		case ARM_CC_LO:
		{
			auto* c = loadRegister(ARM_REG_CPSR_C, irb);
			return generateValueNegate(irb, c);
		}
		// Negative = N set
		case ARM_CC_MI:
		{
			auto* n = loadRegister(ARM_REG_CPSR_N, irb);
			return n;
		}
		// Positive or zero = N clear
		case ARM_CC_PL:
		{
			auto* n = loadRegister(ARM_REG_CPSR_N, irb);
			return generateValueNegate(irb, n);
		}
		// Overflow = V set
		case ARM_CC_VS:
		{
			auto* v = loadRegister(ARM_REG_CPSR_V, irb);
			return v;
		}
		// No overflow = V clear
		case ARM_CC_VC:
		{
			auto* v = loadRegister(ARM_REG_CPSR_V, irb);
			return generateValueNegate(irb, v);
		}
		// Unsigned higher = Carry set & Zero clear
		case ARM_CC_HI:
		{
			auto* c = loadRegister(ARM_REG_CPSR_C, irb);
			auto* z = loadRegister(ARM_REG_CPSR_Z, irb);
			auto* nz = generateValueNegate(irb, z);
			return irb.CreateAnd(c, nz);
		}
		// Unsigned lower or same = Carry clear or Zero set
		case ARM_CC_LS:
		{
			auto* z = loadRegister(ARM_REG_CPSR_Z, irb);
			auto* c = loadRegister(ARM_REG_CPSR_C, irb);
			auto* nc = generateValueNegate(irb, c);
			return irb.CreateOr(z, nc);
		}
		// Greater than or equal = N set and V set || N clear and V clear
		// (N & V) || (!N & !V) == !(N xor V)
		case ARM_CC_GE:
		{
			auto* n = loadRegister(ARM_REG_CPSR_N, irb);
			auto* v = loadRegister(ARM_REG_CPSR_V, irb);
			auto* x = irb.CreateXor(n, v);
			return generateValueNegate(irb, x);
		}
		// Less than = N set and V clear || N clear and V set
		// (N & !V) || (!N & V) == (N xor V)
		case ARM_CC_LT:
		{
			auto* n = loadRegister(ARM_REG_CPSR_N, irb);
			auto* v = loadRegister(ARM_REG_CPSR_V, irb);
			return irb.CreateXor(n, v);
		}
		// Greater than = Z clear, and either N set and V set, or N clear and V set
		case ARM_CC_GT:
		{
			auto* z = loadRegister(ARM_REG_CPSR_Z, irb);
			auto* n = loadRegister(ARM_REG_CPSR_N, irb);
			auto* v = loadRegister(ARM_REG_CPSR_V, irb);
			auto* xor1 = irb.CreateXor(n, v);
			auto* or1 = irb.CreateOr(z, xor1);
			return generateValueNegate(irb, or1);
		}
		// Less than or equal = Z set, or N set and V clear, or N clear and V set
		case ARM_CC_LE:
		{
			auto* z = loadRegister(ARM_REG_CPSR_Z, irb);
			auto* n = loadRegister(ARM_REG_CPSR_N, irb);
			auto* v = loadRegister(ARM_REG_CPSR_V, irb);
			auto* xor1 = irb.CreateXor(n, v);
			return irb.CreateOr(z, xor1);
		}
		case ARM_CC_AL:
		case ARM_CC_INVALID:
		case ARMCC_UNDEF:
		default:
		{
			return irb.getTrue();
		}
	}
}

bool Capstone2LlvmIrTranslatorArm_impl::isOperandRegister(cs_arm_op& op)
{
	return op.type == ARM_OP_REG;
}

uint8_t Capstone2LlvmIrTranslatorArm_impl::getOperandAccess(cs_arm_op& op)
{
	return op.access;
}

//
//==============================================================================
// ARM instruction translation methods.
//==============================================================================
//

/**
 * ARM_INS_ADC
 * Capstone 5.x set update_flags on ADC without S (a decoder quirk).
 * Capstone 6.x reports the architectural S bit, so ADC no longer writes NZCV.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateAdc(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	auto* cf = loadRegister(ARM_REG_CPSR_C, irb);
	auto* add1 = irb.CreateAdd(op1, op2);
	auto* val = irb.CreateAdd(add1, irb.CreateZExtOrTrunc(cf, add1->getType()));
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_C, generateCarryAddC(op1, op2, irb, cf), irb);
		storeRegister(ARM_REG_CPSR_V, generateOverflowAddC(val, op1, op2, irb, cf), irb);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_ADD, ARM_INS_CMN (ADDS but result is discarded)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateAdd(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
//	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);
	EXPECT_IS_EXPR(i, ai, irb, (2 <= ai->op_count && ai->op_count <= 4));

	if (ai->op_count < 4)
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	}
	// TODO: "00 C6 8F E2" = "add ip, pc, #0, #12"
	// If we translate this to "@__asm_add(i32 pc, i32 0, i32 12)" then some
	// regression tests fail because values that were computed before can not
	// be computed now.
	// ARM specification does not allow instruction like this, but it looks like
	// it can happend (probably not only in ADD). See:
	// https://stackoverflow.com/questions/16207865/weird-gas-arm-syntax
	// https://reverseengineering.stackexchange.com/questions/16154/arm-add-instruction-with-shift
	//
	// This is just a hack that ignores the fourth operand. We should use it in
	// rotation. But the best solution is to fix Capstone to interpret it as
	// such, not to hack it here in our library.
	//
	else
	{
		std::tie(op1, op2, op3) = loadOpQuaternaryOp1Op2Op3(ai, irb);
	}

	auto* add = irb.CreateAdd(op1, op2);
	if (ai->update_flags || i->id == ARM_INS_CMN)
	{
		llvm::Value* zero = llvm::ConstantInt::get(add->getType(), 0);
		storeRegister(ARM_REG_CPSR_C, generateCarryAdd(add, op1, irb), irb);
		storeRegister(ARM_REG_CPSR_V, generateOverflowAdd(add, op1, op2, irb), irb);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(add, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(add, zero), irb);
	}
	if (i->id != ARM_INS_CMN)
	{
		storeOp(ai->operands[0], add, irb);
	}
}

/**
 * ARM_INS_AND, ARM_INS_BIC, ARM_INS_TST (ANDS but result is discarded)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateAnd(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	if (i->id == ARM_INS_BIC)
	{
		op2 = generateValueNegate(irb, op2);
	}
	auto* val = irb.CreateAnd(op1, op2);
	// If S is specified, the AND instruction:
	// - updates the N and Z flags according to the result
	// - can update the C flag during the calculation of Operand2 (shifts?)
	// - does not affect the V flag.
	if (ai->update_flags || i->id == ARM_INS_TST)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
	if (i->id != ARM_INS_TST)
	{
		storeOp(ai->operands[0], val, irb);
	}
}

/**
 * ARM_INS_B, ARM_INS_BX, ARM_INS_BXNS (exchange instruction)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateB(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	op0 = loadOpUnary(ai, irb);
	bool isReturn = ai->operands[0].type == ARM_OP_REG
			&& ai->operands[0].reg == ARM_REG_LR;

	llvm::CallInst* call = nullptr;
	if (armCcUncond(ai))
	{
		call = isReturn
			? generateReturnFunctionCall(irb, op0)
			: generateBranchFunctionCall(irb, op0);
	}
	else
	{
		auto* cond = generateInsnConditionCode(irb, ai);
		call = isReturn
			? generateCondReturnFunctionCall(irb, cond, op0)
			: generateCondBranchFunctionCall(irb, cond, op0);
	}
	annotateBxBlxIfNeeded(i, ai, call);
}

/**
 * ARM_INS_BL, ARM_INS_BLX, ARM_INS_BLXNS (exchange instruction)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateBl(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	storeRegister(ARM_REG_LR, getNextInsnAddress(i), irb);
	op0 = loadOpUnary(ai, irb);
	llvm::CallInst* call = nullptr;
	if (armCcUncond(ai))
	{
		call = generateCallFunctionCall(irb, op0);
	}
	else
	{
		auto* cond = generateInsnConditionCode(irb, ai);
		call = generateCondCallFunctionCall(irb, cond, op0);
	}
	annotateBxBlxIfNeeded(i, ai, call);
}

void Capstone2LlvmIrTranslatorArm_impl::annotateBxBlxIfNeeded(
		cs_insn* i,
		cs_arm* ai,
		llvm::CallInst* call)
{
	if (call == nullptr)
	{
		return;
	}
	if (!armIsId(i, ARM_INS_BX) && !armIsId(i, ARM_INS_BLX)
			&& !armIsId(i, ARM_INS_BXNS) && !armIsId(i, ARM_INS_BLXNS))
	{
		return;
	}

	bool targetIsThumb = true;
	if (ai->op_count > 0 && ai->operands[0].type == ARM_OP_IMM)
	{
		targetIsThumb = isThumbAddress(static_cast<uint64_t>(ai->operands[0].imm));
	}
	annotateThumbInterwork(call, targetIsThumb);
}

/**
 * ARM_INS_CBNZ
 */
void Capstone2LlvmIrTranslatorArm_impl::translateCbnz(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	std::tie(op0, op1) = loadOpBinary(ai, irb, eOpConv::NOTHING);
	auto* cond = irb.CreateICmpNE(op0, llvm::ConstantInt::get(op0->getType(), 0));
	if (!armCcUncond(ai))
	{
		cond = irb.CreateAnd(cond, generateInsnConditionCode(irb, ai));
	}
	generateCondBranchFunctionCall(irb, cond, op1);
}

/**
 * ARM_INS_CBZ
 */
void Capstone2LlvmIrTranslatorArm_impl::translateCbz(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	std::tie(op0, op1) = loadOpBinary(ai, irb, eOpConv::NOTHING);
	auto* cond = irb.CreateICmpEQ(op0, llvm::ConstantInt::get(op0->getType(), 0));
	if (!armCcUncond(ai))
	{
		cond = irb.CreateAnd(cond, generateInsnConditionCode(irb, ai));
	}
	generateCondBranchFunctionCall(irb, cond, op1);
}

/**
 * ARM_INS_CLZ
 */
/**
 * ARM_INS_UBFX, ARM_INS_SBFX, ARM_INS_BFI, ARM_INS_BFC
 *
 * The bitfield family, all four of which were on the pseudo-assembly path.
 * `ubfx` alone is 1,898 occurrences in the static corpus and is what a
 * compiler emits for every unsigned bitfield read on ARM.
 *
 *     ubfx rd, rn, #lsb, #width    rd = (rn >> lsb) & ((1 << width) - 1)
 *     sbfx rd, rn, #lsb, #width    the same, sign-extended from bit width-1
 *     bfi  rd, rn, #lsb, #width    rd = (rd & ~mask) | ((rn << lsb) & mask)
 *     bfc  rd,     #lsb, #width    rd = rd & ~mask
 *
 * `lsb` and `width` are immediates in every encoding, so the mask is a
 * compile-time constant and none of the shifts can leave the operand's width.
 *
 * SBFX is the one with a trap: sign-extending from an arbitrary bit is a pair
 * of shifts, `(rn << (32 - lsb - width)) >>s (32 - width)`, and the arithmetic
 * right shift has to come second. Masking first and then shifting right would
 * lose the sign.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateBitfield(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool isBfc = i->id == ARM_INS_BFC;
	unsigned wantOps = isBfc ? 3 : 4;
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count == wantOps));

	unsigned lsbIdx = isBfc ? 1 : 2;
	if (ai->operands[lsbIdx].type != ARM_OP_IMM || ai->operands[lsbIdx + 1].type != ARM_OP_IMM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* ty = getDefaultType();
	unsigned bits = ty->getBitWidth();
	unsigned lsb = static_cast<unsigned>(ai->operands[lsbIdx].imm);
	unsigned width = static_cast<unsigned>(ai->operands[lsbIdx + 1].imm);
	if (width == 0 || lsb >= bits || width > bits - lsb)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	uint64_t fieldMask = width == 64 ? ~0ull : ((1ull << width) - 1);
	auto* maskAtZero = llvm::ConstantInt::get(ty, fieldMask);
	auto* maskInPlace = llvm::ConstantInt::get(ty, fieldMask << lsb);

	llvm::Value* res = nullptr;
	switch (i->id)
	{
	case ARM_INS_UBFX: {
		llvm::Value* src = loadOp(ai->operands[1], irb);
		res = irb.CreateAnd(irb.CreateLShr(src, llvm::ConstantInt::get(ty, lsb)), maskAtZero);
		break;
	}
	case ARM_INS_SBFX: {
		llvm::Value* src = loadOp(ai->operands[1], irb);
		// Left first, then arithmetic right: the sign bit of the field has
		// to reach the top before the shift that replicates it.
		llvm::Value* up = irb.CreateShl(src, llvm::ConstantInt::get(ty, bits - lsb - width));
		res = irb.CreateAShr(up, llvm::ConstantInt::get(ty, bits - width));
		break;
	}
	case ARM_INS_BFI: {
		llvm::Value* src = loadOp(ai->operands[1], irb);
		llvm::Value* old = loadOp(ai->operands[0], irb);
		res = irb.CreateOr(
			irb.CreateAnd(old, irb.CreateNot(maskInPlace)),
			irb.CreateAnd(irb.CreateShl(src, llvm::ConstantInt::get(ty, lsb)), maskInPlace));
		break;
	}
	case ARM_INS_BFC: {
		llvm::Value* old = loadOp(ai->operands[0], irb);
		res = irb.CreateAnd(old, irb.CreateNot(maskInPlace));
		break;
	}
	default: throw GenericError("translateBitfield(): unhandled instruction id");
	}

	storeOp(ai->operands[0], res, irb);
}

/**
 * The ARMv6 parallel add and subtract instructions, and SEL.
 *
 * Four byte lanes or two halfword lanes in a general-purpose register, added
 * or subtracted independently. ARM has no NEON register here and does not need
 * one: the lanes are the bytes of r0..r14, which is why hand-written ARMv6
 * string routines are built out of these and why `uqsub8` (1,554), `uadd8`
 * (840) and `sel` (840) are what is left on ARM after the bitfield batch.
 *
 * Three variants of every operation, and they differ in what happens at the
 * lane boundary:
 *
 *   plain        wraps, and RECORDS what happened in the GE flags
 *   saturating   clamps to the lane's range, sets no flags
 *   halving      keeps the bit that would have been lost, sets no flags
 *
 * and two lane orders: the straight forms operate lane against lane, the
 * exchange forms (ASX, SAX) swap the halves of the second operand first and
 * then add one lane and subtract the other. `uasx` is subtract-low, add-high;
 * `usax` is the other way round. Getting that pair backwards is invisible for
 * any operand where the two halves are equal, so the tests use halves that are
 * not.
 *
 * The GE flags are the point of the plain forms. `uadd8` sets GE[n] when lane
 * n carried out; `usub8` sets GE[n] when lane n did NOT borrow, which is the
 * unsigned `a >= b` the mnemonic is named after; the signed forms set GE[n]
 * when the lane's signed result is non-negative. The halfword forms set GE in
 * pairs, because there are four flags and two lanes. Everything then feeds
 * SEL, which is a lane-wise select and reads nothing else.
 *
 * The arithmetic is done one lane wider than the lane, so the carry, the
 * borrow and the halving bit are all still there to be read; the plain forms
 * then truncate and the saturating ones clamp.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateParallelArith(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	unsigned laneBits = 0; // 8 or 16
	bool isSigned = false;
	bool saturating = false;
	bool halving = false;
	// Per lane: true adds, false subtracts. For the straight forms every lane
	// does the same thing; for the exchange forms they differ.
	bool addLow = true;
	bool addHigh = true;
	bool exchange = false;

	switch (i->id)
	{
	// Byte, plain.
	case ARM_INS_UADD8: laneBits = 8; break;
	case ARM_INS_SADD8:
		laneBits = 8;
		isSigned = true;
		break;
	case ARM_INS_USUB8:
		laneBits = 8;
		addLow = addHigh = false;
		break;
	case ARM_INS_SSUB8:
		laneBits = 8;
		isSigned = true;
		addLow = addHigh = false;
		break;
	// Byte, saturating.
	case ARM_INS_UQADD8:
		laneBits = 8;
		saturating = true;
		break;
	case ARM_INS_QADD8:
		laneBits = 8;
		saturating = true;
		isSigned = true;
		break;
	case ARM_INS_UQSUB8:
		laneBits = 8;
		saturating = true;
		addLow = addHigh = false;
		break;
	case ARM_INS_QSUB8:
		laneBits = 8;
		saturating = true;
		isSigned = true;
		addLow = addHigh = false;
		break;
	// Byte, halving.
	case ARM_INS_UHADD8:
		laneBits = 8;
		halving = true;
		break;
	case ARM_INS_SHADD8:
		laneBits = 8;
		halving = true;
		isSigned = true;
		break;
	case ARM_INS_UHSUB8:
		laneBits = 8;
		halving = true;
		addLow = addHigh = false;
		break;
	case ARM_INS_SHSUB8:
		laneBits = 8;
		halving = true;
		isSigned = true;
		addLow = addHigh = false;
		break;
	// Halfword, plain.
	case ARM_INS_UADD16: laneBits = 16; break;
	case ARM_INS_SADD16:
		laneBits = 16;
		isSigned = true;
		break;
	case ARM_INS_USUB16:
		laneBits = 16;
		addLow = addHigh = false;
		break;
	case ARM_INS_SSUB16:
		laneBits = 16;
		isSigned = true;
		addLow = addHigh = false;
		break;
	// Halfword, saturating.
	case ARM_INS_UQADD16:
		laneBits = 16;
		saturating = true;
		break;
	case ARM_INS_QADD16:
		laneBits = 16;
		saturating = true;
		isSigned = true;
		break;
	case ARM_INS_UQSUB16:
		laneBits = 16;
		saturating = true;
		addLow = addHigh = false;
		break;
	case ARM_INS_QSUB16:
		laneBits = 16;
		saturating = true;
		isSigned = true;
		addLow = addHigh = false;
		break;
	// Halfword, halving.
	case ARM_INS_UHADD16:
		laneBits = 16;
		halving = true;
		break;
	case ARM_INS_SHADD16:
		laneBits = 16;
		halving = true;
		isSigned = true;
		break;
	case ARM_INS_UHSUB16:
		laneBits = 16;
		halving = true;
		addLow = addHigh = false;
		break;
	case ARM_INS_SHSUB16:
		laneBits = 16;
		halving = true;
		isSigned = true;
		addLow = addHigh = false;
		break;
	// The exchange forms. ASX subtracts the low lane and adds the high
	// one; SAX does the opposite. Both are halfword only.
	case ARM_INS_UASX:
		laneBits = 16;
		exchange = true;
		addLow = false;
		break;
	case ARM_INS_SASX:
		laneBits = 16;
		exchange = true;
		addLow = false;
		isSigned = true;
		break;
	case ARM_INS_USAX:
		laneBits = 16;
		exchange = true;
		addHigh = false;
		break;
	case ARM_INS_SSAX:
		laneBits = 16;
		exchange = true;
		addHigh = false;
		isSigned = true;
		break;
	case ARM_INS_UQASX:
		laneBits = 16;
		exchange = true;
		addLow = false;
		saturating = true;
		break;
	case ARM_INS_QASX:
		laneBits = 16;
		exchange = true;
		addLow = false;
		saturating = true;
		isSigned = true;
		break;
	case ARM_INS_UQSAX:
		laneBits = 16;
		exchange = true;
		addHigh = false;
		saturating = true;
		break;
	case ARM_INS_QSAX:
		laneBits = 16;
		exchange = true;
		addHigh = false;
		saturating = true;
		isSigned = true;
		break;
	case ARM_INS_UHASX:
		laneBits = 16;
		exchange = true;
		addLow = false;
		halving = true;
		break;
	case ARM_INS_SHASX:
		laneBits = 16;
		exchange = true;
		addLow = false;
		halving = true;
		isSigned = true;
		break;
	case ARM_INS_UHSAX:
		laneBits = 16;
		exchange = true;
		addHigh = false;
		halving = true;
		break;
	case ARM_INS_SHSAX:
		laneBits = 16;
		exchange = true;
		addHigh = false;
		halving = true;
		isSigned = true;
		break;
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* i32 = irb.getInt32Ty();
	op1 = irb.CreateZExtOrTrunc(op1, i32);
	op2 = irb.CreateZExtOrTrunc(op2, i32);

	unsigned lanes = 32 / laneBits;
	auto* laneTy = irb.getIntNTy(laneBits);
	// TWO bits wider than the lane, not one, and the second bit is not
	// slack. One extra bit holds the unsigned sum's carry -- and then 0xff +
	// 0xff is 510, which does not fit a SIGNED nine-bit value, so every
	// comparison against the saturation bounds would read it as -2. Two bits
	// hold every case: an unsigned sum reaches 510 of a signed 511, an
	// unsigned difference reaches -255, and the signed forms are narrower
	// than both.
	auto* wideTy = irb.getIntNTy(laneBits + 2);

	auto lane = [&](llvm::Value* v, unsigned n) -> llvm::Value* {
		llvm::Value* x = irb.CreateLShr(v, llvm::ConstantInt::get(i32, n * laneBits));
		return irb.CreateTrunc(x, laneTy);
	};

	llvm::Value* result = llvm::ConstantInt::get(i32, 0);
	llvm::Value* ge[4] = {nullptr, nullptr, nullptr, nullptr};

	for (unsigned n = 0; n < lanes; ++n)
	{
		llvm::Value* a = lane(op1, n);
		// The exchange forms take the OTHER half of the second operand.
		llvm::Value* b = lane(op2, exchange ? (lanes - 1 - n) : n);

		llvm::Value* aw = isSigned ? irb.CreateSExt(a, wideTy) : irb.CreateZExt(a, wideTy);
		llvm::Value* bw = isSigned ? irb.CreateSExt(b, wideTy) : irb.CreateZExt(b, wideTy);

		bool add = (n == 0) ? addLow : addHigh;
		llvm::Value* wide = add ? irb.CreateAdd(aw, bw) : irb.CreateSub(aw, bw);

		llvm::Value* out = nullptr;
		if (halving)
		{
			out = irb.CreateTrunc(
				isSigned ? irb.CreateAShr(wide, llvm::ConstantInt::get(wideTy, 1))
						 : irb.CreateLShr(wide, llvm::ConstantInt::get(wideTy, 1)),
				laneTy);
		}
		else if (saturating)
		{
			int64_t lo = isSigned ? -(int64_t(1) << (laneBits - 1)) : 0;
			int64_t hi = isSigned ? (int64_t(1) << (laneBits - 1)) - 1 : (int64_t(1) << laneBits) - 1;
			auto* loC = llvm::ConstantInt::getSigned(wideTy, lo);
			auto* hiC = llvm::ConstantInt::getSigned(wideTy, hi);
			// Both bounds are compared signed, for both signednesses: the
			// operands were extended according to theirs, so `wide` is
			// already the true value and the only question left is where it
			// sits between the lane's limits.
			llvm::Value* c = irb.CreateSelect(irb.CreateICmpSLT(wide, loC), loC, wide);
			c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
			out = irb.CreateTrunc(c, laneTy);
		}
		else
		{
			out = irb.CreateTrunc(wide, laneTy);
			// GE[n] is what happened at the lane boundary, and it is the only
			// thing that distinguishes these from an ordinary wrapping add.
			llvm::Value* flag = nullptr;
			if (isSigned)
			{
				// Non-negative signed result.
				flag = irb.CreateICmpSGE(wide, llvm::ConstantInt::get(wideTy, 0));
			}
			else if (add)
			{
				// Carried out of the lane.
				flag = irb.CreateICmpNE(
					irb.CreateLShr(wide, llvm::ConstantInt::get(wideTy, laneBits)), llvm::ConstantInt::get(wideTy, 0));
			}
			else
			{
				// Did not borrow, which is unsigned a >= b.
				flag = irb.CreateICmpSGE(wide, llvm::ConstantInt::get(wideTy, 0));
			}

			if (laneBits == 8)
			{
				ge[n] = flag;
			}
			else
			{
				// Two lanes, four flags: each halfword sets a pair.
				ge[2 * n] = flag;
				ge[2 * n + 1] = flag;
			}
		}

		llvm::Value* placed = irb.CreateShl(irb.CreateZExt(out, i32), llvm::ConstantInt::get(i32, n * laneBits));
		result = irb.CreateOr(result, placed);
	}

	storeOp(ai->operands[0], result, irb);

	if (ge[0] != nullptr)
	{
		storeRegister(ARM_REG_CPSR_GE0, ge[0], irb);
		storeRegister(ARM_REG_CPSR_GE1, ge[1], irb);
		storeRegister(ARM_REG_CPSR_GE2, ge[2], irb);
		storeRegister(ARM_REG_CPSR_GE3, ge[3], irb);
	}
}

/**
 * ARM_INS_SEL
 *
 * Byte n of the result is byte n of the first source when GE[n] is set and
 * byte n of the second otherwise. It reads the GE flags and nothing else, so
 * it was the half of a pair that could not be translated until the other half
 * produced the flags -- which is why both are in the same commit.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateSel(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* i32 = irb.getInt32Ty();
	op1 = irb.CreateZExtOrTrunc(op1, i32);
	op2 = irb.CreateZExtOrTrunc(op2, i32);

	const uint32_t geRegs[4] = {ARM_REG_CPSR_GE0, ARM_REG_CPSR_GE1, ARM_REG_CPSR_GE2, ARM_REG_CPSR_GE3};

	llvm::Value* result = llvm::ConstantInt::get(i32, 0);
	for (unsigned n = 0; n < 4; ++n)
	{
		auto* shift = llvm::ConstantInt::get(i32, n * 8);
		llvm::Value* a = irb.CreateAnd(irb.CreateLShr(op1, shift), llvm::ConstantInt::get(i32, 0xff));
		llvm::Value* b = irb.CreateAnd(irb.CreateLShr(op2, shift), llvm::ConstantInt::get(i32, 0xff));
		llvm::Value* pick = irb.CreateSelect(loadRegister(geRegs[n], irb), a, b);
		result = irb.CreateOr(result, irb.CreateShl(pick, shift));
	}

	storeOp(ai->operands[0], result, irb);
}

/**
 * ARM_INS_SXTB, ARM_INS_SXTH, ARM_INS_SXTAB, ARM_INS_SXTAH,
 * ARM_INS_SXTB16, ARM_INS_SXTAB16
 *
 * Sign-extend a byte or a halfword, optionally adding Rn. The optional
 * `, ror #n` is part of the Rm operand as Capstone reports it, so loadOp
 * has already applied it.
 *
 * SXTB16 / SXTAB16 do the same independently in each halfword: bits [7:0]
 * and [23:16], writing the two sign-extended 16-bit lanes.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateSxt(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool addRn = i->id == ARM_INS_SXTAB || i->id == ARM_INS_SXTAH || i->id == ARM_INS_SXTAB16;
	bool parallel16 = i->id == ARM_INS_SXTB16 || i->id == ARM_INS_SXTAB16;
	unsigned from = (i->id == ARM_INS_SXTH || i->id == ARM_INS_SXTAH) ? 16 : 8;

	if (addRn)
	{
		EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);
	}
	else
	{
		EXPECT_IS_BINARY(i, ai, irb);
	}

	auto* i32 = getDefaultType();
	llvm::Value* rn = nullptr;
	llvm::Value* rm = nullptr;
	if (addRn)
	{
		std::tie(rn, rm) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	}
	else
	{
		rm = loadOpBinaryOp1(ai, irb);
	}

	if (parallel16)
	{
		auto* mask16 = llvm::ConstantInt::get(i32, 0xffff);
		auto extractLane = [&](unsigned shift) {
			llvm::Value* b = rm;
			if (shift)
			{
				b = irb.CreateLShr(b, llvm::ConstantInt::get(i32, shift));
			}
			auto* n = irb.CreateTrunc(b, irb.getInt8Ty());
			return irb.CreateAnd(irb.CreateSExt(n, i32), mask16);
		};
		llvm::Value* lo = extractLane(0);
		llvm::Value* hi = extractLane(16);
		if (rn)
		{
			lo = irb.CreateAnd(irb.CreateAdd(lo, irb.CreateAnd(rn, mask16)), mask16);
			hi = irb.CreateAnd(irb.CreateAdd(hi, irb.CreateLShr(rn, llvm::ConstantInt::get(i32, 16))), mask16);
		}
		storeOp(ai->operands[0], irb.CreateOr(lo, irb.CreateShl(hi, llvm::ConstantInt::get(i32, 16))), irb);
		return;
	}

	auto* narrow = irb.CreateTrunc(rm, irb.getIntNTy(from));
	llvm::Value* ext = irb.CreateSExt(narrow, i32);
	if (rn)
	{
		ext = irb.CreateAdd(rn, ext);
	}
	storeOp(ai->operands[0], ext, irb);
}

/**
 * ARM_INS_REV16, ARM_INS_REVSH, ARM_INS_RBIT
 *
 * REV was translated with llvm.bswap and these three were not, though two of
 * them are the same intrinsic applied to a different width.
 *
 *     rev16 rd, rm    swap the bytes WITHIN each halfword, both halves
 *     revsh rd, rm    swap the bytes of the low halfword, then sign-extend
 *     rbit  rd, rm    reverse all 32 bits
 *
 * REV16 is not a 32-bit byte swap: `0x11223344` becomes `0x22114433`, not
 * `0x44332211`. Reaching for llvm.bswap.i32 here would be the plausible wrong
 * answer, which is why it is spelled as the two masked shifts it is.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateRev16(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);
	auto* ty = getDefaultType();

	if (i->id == ARM_INS_RBIT)
	{
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::bitreverse, op1->getType());
		storeOp(ai->operands[0], irb.CreateCall(f, {op1}), irb);
		return;
	}

	if (i->id == ARM_INS_REVSH)
	{
		auto* i16 = irb.getInt16Ty();
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::bswap, i16);
		auto* swapped = irb.CreateCall(f, {irb.CreateTrunc(op1, i16)});
		storeOp(ai->operands[0], irb.CreateSExt(swapped, ty), irb);
		return;
	}

	auto* eight = llvm::ConstantInt::get(ty, 8);
	auto* lowBytes = llvm::ConstantInt::get(ty, 0x00ff00ffull);
	llvm::Value* res = irb.CreateOr(
		irb.CreateShl(irb.CreateAnd(op1, lowBytes), eight), irb.CreateAnd(irb.CreateLShr(op1, eight), lowBytes));
	storeOp(ai->operands[0], res, irb);
}

/**
 * ARM_INS_MRC -- read a coprocessor register.
 *
 * One encoding of this is not a coprocessor access in any useful sense:
 *
 *     mrc p15, 0, Rt, c13, c0, 3
 *
 * is the thread pointer, TPIDRURO, and it is how glibc finds thread-local
 * storage on ARM. Every one of the 11,697 `mrc` instructions in the static
 * parity corpus is that exact encoding, and all of them were becoming
 * `__asm_mrc(15, 0, 13, 0, 3)` -- a call to an undefined function, from which
 * no later pass can recover that the result is a pointer, let alone a stable
 * one.
 *
 * Everything else stays on the pseudo-assembly path. A coprocessor read is
 * genuinely opaque; this one is not, and the difference is six immediates that
 * Capstone hands over.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMrc(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto imm = [ai](unsigned n) { return ai->operands[n].imm; };
	bool isThreadPointer = ai->op_count == 6 && ai->operands[0].type == ARM_OP_PIMM && imm(0) == 15
						&& ai->operands[1].type == ARM_OP_IMM && imm(1) == 0 && ai->operands[2].type == ARM_OP_REG
						&& ai->operands[3].type == ARM_OP_CIMM && imm(3) == 13 && ai->operands[4].type == ARM_OP_CIMM
						&& imm(4) == 0 && ai->operands[5].type == ARM_OP_IMM && imm(5) == 3;

	if (!isThreadPointer)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	storeOp(ai->operands[2], loadRegister(ARM_REG_TPIDRURO, irb), irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateClz(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::ctlz,
			op1->getType());
	// is_zero_poison = FALSE; see the ARM64 translateClz for the reasoning.
	// A32 defines `clz Rd, Rm` with Rm = 0 as 32. Neither ARM test even uses
	// a zero operand, so this side had no coverage of the case at all.
	auto* ctlz = irb.CreateCall(f, {op1, irb.getFalse()});
	storeOp(ai->operands[0], ctlz, irb);
}

/**
 * ARM_INS_EOR, ARM_INS_TEQ (EORS but result is discarded)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateEor(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	auto* val = irb.CreateXor(op1, op2);
	// If S is specified, the EOR instruction:
	// - updates the N and Z flags according to the result
	// - can update the C flag during the calculation of Operand2 (shifts?)
	// - does not affect the V flag.
	if (ai->update_flags || i->id == ARM_INS_TEQ)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
	if (i->id != ARM_INS_TEQ)
	{
		storeOp(ai->operands[0], val, irb);
	}
}

/**
 * ARM_INS_MLA
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMla(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, ai, irb);

	std::tie(op1, op2, op3) = loadOpQuaternaryOp1Op2Op3(ai, irb);
	auto* val = irb.CreateMul(op1, op2);
	val = irb.CreateAdd(op3, val);

	// Updates the N and Z flags according to the result.
	// Corrupts the C and V flag in ARMv4.
	// Does not affect the C or V flag in ARMv5T and above.
	// TODO: The question is, does it set N/Z according to what is written to
	// dst reg (low 32-bit), or according to the full result (64-bit)?
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_MLS
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMls(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, ai, irb);

	std::tie(op1, op2, op3) = loadOpQuaternaryOp1Op2Op3(ai, irb);
	auto* val = irb.CreateMul(op1, op2);
	val = irb.CreateSub(op3, val);
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_MOV, ARM_INS_MVN,
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMov(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);
	if (i->id == ARM_INS_MVN)
	{
		op1 = generateValueNegate(irb, op1);
	}

	// If S is specified, the MOV instruction:
	// - updates the N and Z flags according to the result
	// - can update the C flag during the calculation of Operand2 (shifts?)
	// - does not affect the V flag.
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(op1->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(op1, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(op1, zero), irb);
	}
	storeOp(ai->operands[0], op1, irb);
}

/**
 * Preferred synonyms for MOV instructions with shifted register operands:
 * ARM_INS_LSL, ARM_INS_LSR, ARM_INS_ROR, ARM_INS_RRX, ARM_INS_ASR
 *
 * TODO: Report Capstone bug:
 * - When shift is imm, it is ok -- imm is part of the operand to be shifted.
 *   e.g. lsl r0, r1, #4 = 2 operands, 4 and LSL part of r1 operand.
 * - When shift is reg, it is NOT ok -- reg is NOT a part of the operand to be
 *   shifted, even though it could be.
 *   e.g. lsl r0, r1, r2 = 3 operands, r1 have ARM_SFT_INVALID, r2 separate op.
 *   It should be 2 operands, r1 with ARM_SFT_LSL_REG, r2 in r1 op as shift val.
 * - On THUMB, not even imm is ok, it creates 3th operand as well.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateShifts(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	// We expect 2nd operand to have shift/rotate set -> loadOp() will take
	// care of shift/rotate computation. Capstone 6.x alias form of
	// `lsl r0, r1, #16` is 3 operands AND a shift on operand 1; applying
	// both would shift twice.
	//
	if ((ai->op_count == 2 || ai->op_count == 3)
			&& ai->operands[1].shift.type != ARM_SFT_INVALID)
	{
		op1 = loadOp(ai->operands[1], irb);
	}
	// We expect that 3rd operand is a shift/rotate value, and 2nd operand
	// does not have shift type set - ARM_SFT_INVALID.
	// Shift type is determined by insn ID.
	//
	// TODO: THUMB "99 40" = "lsls r1, r3" -- only 2 ops, no shift in 2nd op.
	// Report Capstone bug - consistency - it should have 2 operands, r1 and r1,
	// r3 should be part of r1 shift.
	//
	else if (ai->op_count == 2 || ai->op_count == 3)
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);

		unsigned sid = i->is_alias ? static_cast<unsigned>(i->alias_id) : i->id;
		if (sid == ARM_INS_ASR || sid == ARM_INS_ALIAS_ASR)
		{
			op1 = generateShiftAsr(irb, op1, op2);
		}
		else if (sid == ARM_INS_LSL || sid == ARM_INS_ALIAS_LSL)
		{
			op1 = generateShiftLsl(irb, op1, op2);
		}
		else if (sid == ARM_INS_LSR || sid == ARM_INS_ALIAS_LSR)
		{
			op1 = generateShiftLsr(irb, op1, op2);
		}
		else if (sid == ARM_INS_ROR || sid == ARM_INS_ALIAS_ROR)
		{
			op1 = generateShiftRor(irb, op1, op2);
		}
		else if (sid == ARM_INS_RRX || sid == ARM_INS_ALIAS_RRX)
		{
			op1 = generateShiftRrx(irb, op1, op2);
		}
		else
		{
			throw GenericError("unhandled insn ID");
		}
	}

	// If S is specified, the MOV instruction:
	// - updates the N and Z flags according to the result
	// - can update the C flag during the calculation of Operand2 (shifts?)
	// - does not affect the V flag.
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(op1->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(op1, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(op1, zero), irb);
	}
	storeOp(ai->operands[0], op1, irb);
}

/**
 * ARM_INS_MOVT
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMovt(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	// TODO: It looks like on THUMB, op0 is not ANDed -- investigate.
	// Add/Fix THUMB unit tests.
	if (_basicMode == CS_MODE_THUMB)
	{
		std::tie(op0, op1) = loadOpBinary(ai, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		op1 = irb.CreateShl(op1, 16);
		op0 = irb.CreateOr(op0, op1);
		storeOp(ai->operands[0], op0, irb);
	}
	else
	{
		std::tie(op0, op1) = loadOpBinary(ai, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		op0 = irb.CreateAnd(op0, 0xffff);
		op1 = irb.CreateShl(op1, 16);
		op0 = irb.CreateOr(op0, op1);
		storeOp(ai->operands[0], op0, irb);
	}
}

/**
 * ARM_INS_MOVW
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMovw(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	// MOVW is architecturally Rd = ZeroExtend(imm16). Capstone 5.x ARM-mode
	// decode used to look like MOVT-of-the-low-half (keep bits 31:16); 6.x
	// reports a plain 16-bit immediate and the historical tests expect a
	// full overwrite on both ARM and Thumb.
	op1 = loadOpBinaryOp1(ai, irb);
	op1 = irb.CreateZExtOrTrunc(op1, irb.getInt32Ty());
	storeOp(ai->operands[0], op1, irb);
}

/**
 * ARM_INS_MUL
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMul(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	auto* val = irb.CreateMul(op1, op2);
	// If S is specified, the MUL instruction:
	// - updates the N and Z flags according to the result
	// - corrupts the C and V flag in ARMv4
	// - does not affect the C or V flag in ARMv5T and above.
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_NOP
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNop(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	// nothing
}

/**
 * ARM_INS_HINT
 *
 * Capstone decodes the whole hint space -- NOP, YIELD, WFE, WFI, SEV, SEVL,
 * DBG and the pointer-authentication and branch-target hints -- to this one
 * id, and gives it no operand. ARM_INS_NOP exists too but capstone does not
 * produce it for these encodings, so `translateNop` sat wired to a key that
 * never arrived while HINT went to the pseudo-asm path: 238 occurrences in
 * the 42-binary ARM corpus, its single most frequent untranslated
 * instruction by a factor of three.
 *
 * Most of the space is a no-op and is translated as one. WFI, WFE, SEV, SEVL
 * and DBG are not -- they wait on or signal an external event -- and keep the
 * pseudo-asm call rather than being silently dropped. The mnemonic is the only
 * thing that separates them, because the operand capstone would carry the
 * hint number in is absent.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateHint(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (i->id == ARM_INS_WFI || i->id == ARM_INS_WFE
			|| i->id == ARM_INS_SEV || i->id == ARM_INS_SEVL)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	std::string m(i->mnemonic);
	if (m == "wfi" || m == "wfe" || m == "sev" || m == "sevl" || m == "dbg")
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	// A no-op: nop, yield, esb, csdb, bti, pac* and the rest of the space.
}

/**
 * ARM_INS_ADR
 *
 * `adr rN, label` is PC plus an immediate -- how a compiler names an address
 * in its own function without going through a literal pool. Capstone reports
 * the offset rather than the resolved address, so the translation is the PC
 * value the architecture defines plus that offset, and getCurrentPc above is
 * what makes the Thumb case right.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateAdr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (ai->operands[1].type != ARM_OP_IMM)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* pc = getCurrentPc(i);
	auto* off = llvm::ConstantInt::getSigned(pc->getType(), ai->operands[1].imm);
	storeOp(ai->operands[0], irb.CreateAdd(pc, off), irb);
}

/**
 * ARM_INS_VPUSH, ARM_INS_VPOP
 *
 * The VFP half of PUSH and POP. translateLdmStm cannot serve them: it writes
 * every slot at getArchByteSize(), which is four, and a D register is eight.
 * A list of D registers pushed four bytes apart overlaps itself.
 *
 * The registers always go to consecutive slots in ascending register order
 * from the lowest address, both directions, and capstone hands them over in
 * that order.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpPushPop(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count > 0));

	for (unsigned j = 0; j < ai->op_count; ++j)
	{
		if (ai->operands[j].type != ARM_OP_REG || !isFpRegister(ai->operands[j].reg))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}

	bool load = armIsId(i, ARM_INS_VPOP);
	auto* elem = vfpTypeOfReg(ai->operands[0].reg, irb);
	uint64_t sz = elem->isDoubleTy() ? 8 : 4;

	auto* spTy = getDefaultType();
	auto* sp = loadRegister(ARM_REG_SP, irb);
	if (sp && !sp->getType()->isIntegerTy())
	{
		sp = generateTypeConversion(irb, sp, spTy, eOpConv::FPCAST_OR_BITCAST);
	}
	auto* total = llvm::ConstantInt::get(sp->getType(), sz * ai->op_count);
	// VPUSH writes below the old SP; VPOP reads from it.
	auto* base = load ? sp : irb.CreateSub(sp, total);

	for (unsigned j = 0; j < ai->op_count; ++j)
	{
		auto* at = irb.CreateAdd(base, llvm::ConstantInt::get(sp->getType(), sz * j));
		if (load)
		{
			storeOp(ai->operands[j], loadIntPtr(irb, at, elem), irb, eOpConv::FPCAST_OR_BITCAST);
		}
		else
		{
			storeIntPtr(irb, loadVfpOp(ai->operands[j], irb, elem), at, elem);
		}
	}

	storeRegister(ARM_REG_SP, load ? irb.CreateAdd(sp, total) : base, irb);
}

/**
 * ARM_INS_ORN
 *
 * Thumb-2's `orn rd, rn, op2` is `rn | ~op2`. The complement is on the second
 * operand, not on the result.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateOrn(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	auto* val = irb.CreateOr(op1, irb.CreateNot(op2));
	// Same flag rule as ORR: N and Z from the result, V untouched.
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_DMB, ARM_INS_DSB, ARM_INS_ISB
 * Memory / instruction barriers → LLVM fence. ISB has no I-cache model;
 * it is lowered as seq_cst like DMB/DSB.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateFence(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	irb.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
}

/**
 * ARM_INS_ORR
 */
void Capstone2LlvmIrTranslatorArm_impl::translateOrr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	auto* val = irb.CreateOr(op1, op2);
	// If S is specified, the ORR instruction:
	// - updates the N and Z flags according to the result
	// - can update the C flag during the calculation of Operand2 (shifts?)
	// - does not affect the V flag.
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_LDM   = IA (increment after) = LDMFD (synonym, IDA)
 * ARM_INS_LDMIB = IB (increment before) (ARM only)
 * ARM_INS_LDMDA = DA (decrement after) (ARM only)
 * ARM_INS_LDMDB = DB (decrement before)
 * ARM_INS_POP   = LDMIA sp! reglist (writeback to SP, increment after)
 *
 * ARM_INS_STM   = IA (increment after)
 * ARM_INS_STMIB = IB (increment before) (ARM only)
 * ARM_INS_STMDA = DA (decrement after) (ARM only)
 * ARM_INS_STMDB = DB (decrement before) = STMFD (synonym, IDA)
 * ARM_INS_PUSH  = STMDB sp!, reglist (writeback to SP, decrement before)
 *
 * ARM_INS_PUSH (ARM_INS_STMDB):
 * Registers are stored on the stack in numerical order, with the lowest
 * numbered register at the lowest address.
 * TODO: Is this also true for ARM_INS_STMDA? Are increment variants ok?
 *
 * TODO: If PC is loaded (store to PC -> branch), then we might generate uncond
 * branch in the middle of the instruction -> before all of it is executed.
 * We should remember such branch and generate it last, because in CPU,
 * all the instruction is executed.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateLdmStm(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count > 0));

	auto sz = getArchByteSize();
	auto* ty = getDefaultType();

	bool isPop = armIsId(i, ARM_INS_POP) || armIsId(i, ARM_INS_ALIAS_POP)
			|| armIsId(i, ARM_INS_ALIAS_POPW);
	bool isPush = armIsId(i, ARM_INS_PUSH) || armIsId(i, ARM_INS_ALIAS_PUSH)
			|| armIsId(i, ARM_INS_ALIAS_PUSHW);
	bool isLdm = armIsId(i, ARM_INS_LDM) || armIsId(i, ARM_INS_ALIAS_LDM);
	bool isLdmib = armIsId(i, ARM_INS_LDMIB);
	bool isLdmda = armIsId(i, ARM_INS_LDMDA);
	bool isLdmdb = armIsId(i, ARM_INS_LDMDB);
	bool isStm = armIsId(i, ARM_INS_STM);
	bool isStmib = armIsId(i, ARM_INS_STMIB);
	bool isStmda = armIsId(i, ARM_INS_STMDA);
	bool isStmdb = armIsId(i, ARM_INS_STMDB);

	unsigned opStart = 0;
	if (isPop || isPush)
	{
		op0 = loadRegister(ARM_REG_SP, irb);
		opStart = 0;
	}
	else
	{
		op0 = loadOp(ai->operands[0], irb);
		opStart = 1;
	}

	bool increment = isLdm || isLdmib || isPop || isStm || isStmib;
	bool after = isLdm || isLdmda || isPop || isStm || isStmda;
	bool before = !after;
	bool load = isLdm || isLdmib || isLdmda || isLdmdb || isPop;

	llvm::Value* incDec = op0;
	llvm::Value* finalIncDec = op0;

	llvm::Value* pcStoreVal = nullptr;
	unsigned pcStoreNum = 0;

	for (unsigned j = opStart; j < ai->op_count; ++j)
	{
		uint64_t c = isPush || isStmdb
				? sz * (ai->op_count - j)
				: sz * (j-opStart+1);

		auto* ci = llvm::ConstantInt::get(op0->getType(), c);

		if (before)
		{
			if (increment)
			{
				incDec = irb.CreateAdd(op0, ci);
			}
			else
			{
				incDec = irb.CreateSub(op0, ci);
			}
		}

		if (load)
		{
			auto* l = loadIntPtr(irb, incDec, ty);
			if (ai->operands[j].type == ARM_OP_REG
					&& ai->operands[j].reg == ARM_REG_PC)
			{
				pcStoreVal = l;
				pcStoreNum = j;
			}
			else
			{
				storeOp(ai->operands[j], l, irb);
			}
		}
		else
		{
			auto* reg = loadOp(ai->operands[j], irb);
			reg = irb.CreateZExtOrTrunc(reg, ty);
			storeIntPtr(irb, reg, incDec, ty);
		}

		if (after)
		{
			if (increment)
			{
				incDec = irb.CreateAdd(op0, ci);
			}
			else
			{
				incDec = irb.CreateSub(op0, ci);
			}
		}

		if (isPush || isStmdb)
		{
			if (finalIncDec == op0)
			{
				finalIncDec = incDec;
			}
		}
		else
		{
			finalIncDec = incDec;
		}
	}

	if (isPop || isPush)
	{
		storeRegister(ARM_REG_SP, finalIncDec, irb);
	}
	else if (insnWriteback())
	{
		storeOp(ai->operands[0], finalIncDec, irb);
	}

	if (pcStoreVal)
	{
		storeOp(ai->operands[pcStoreNum], pcStoreVal, irb);
	}
}

/**
 * ARM_INS_REV
 */
void Capstone2LlvmIrTranslatorArm_impl::translateRev(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::bswap,
			op1->getType());
	auto* val = irb.CreateCall(f, {op1});
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_SBC, ARM_INS_RSC
 * TODO: The same flag-update problem as with ARM_INS_ADC.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateSbc(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	if (i->id == ARM_INS_SBC)
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	}
	else if (i->id == ARM_INS_RSC)
	{
		std::tie(op2, op1) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	}
	auto* cf = loadRegister(ARM_REG_CPSR_C, irb);
	// If the carry flag is clear, the result is reduced by one.
	cf = irb.CreateICmpEQ(cf, irb.getFalse());
	auto* sub1 = irb.CreateSub(op1, op2);
	auto* val = irb.CreateSub(sub1, irb.CreateZExtOrTrunc(cf, sub1->getType()));
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_C, generateValueNegate(irb, generateBorrowSubC(val, op1, op2, irb, cf)), irb);
		storeRegister(ARM_REG_CPSR_V, generateOverflowSubC(val, op1, op2, irb, cf), irb);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_LDR (word) = ARM_INS_LDRT (unprivileged)
 * ARM_INS_LDRB (unsigned byte) = ARM_INS_LDRBT (unprivileged)
 * ARM_INS_LDRSB (signed byte) = ARM_INS_LDRSBT (unprivileged)
 * ARM_INS_LDRH (unsigned half word) = ARM_INS_LDRHT (unprivileged)
 * ARM_INS_LDRSH (signed half word) = ARM_INS_LDRSHT (unprivileged)
 *
 * ARM_INS_LDREX, ARM_INS_LDREXB, ARM_INS_LDREXH = Exclusive:
 * Atomic load (monotonic). No exclusive-monitor model.
 *
 * LDR R0, [R4, #4]  ; simple offset: R0 = *(int*)(R4+4); R4 unchanged
 * LDR R0, [R4, #4]! ; pre-indexed  : R0 = *(int*)(R4+4); R4 = R4+4
 * LDR R0, [R4], #4  ; post-indexed : R0 = *(int*)(R4+0); R4 = R4+4
 *
 * TODO: "20 f5 bc e5" = "ldr pc, [ip, #0x520]!" -> write to PC -> branch,
 * writeback is generated after the branch call -> problem here, and probably
 * everywhere where writeback is generated. In these cases, branch generated for
 * PC write should be the last instruction.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateLdr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	llvm::Type* ty = nullptr;
	bool sext = false;
	switch (i->id)
	{
		case ARM_INS_LDR:
		case ARM_INS_LDRT:
		case ARM_INS_LDREX:
		case ARM_INS_LDA:
		case ARM_INS_LDAEX:
		{
			ty = irb.getInt32Ty();
			sext = false;
			break;
		}
		case ARM_INS_LDRB:
		case ARM_INS_LDRBT:
		case ARM_INS_LDREXB:
		case ARM_INS_LDAB:
		case ARM_INS_LDAEXB:
		{
			ty = irb.getInt8Ty();
			sext = false;
			break;
		}
		case ARM_INS_LDRSB:
		case ARM_INS_LDRSBT:
		{
			ty = irb.getInt8Ty();
			sext = true;
			break;
		}
		case ARM_INS_LDRH:
		case ARM_INS_LDRHT:
		case ARM_INS_LDREXH:
		case ARM_INS_LDAH:
		case ARM_INS_LDAEXH:
		{
			ty = irb.getInt16Ty();
			sext = false;
			break;
		}
		case ARM_INS_LDRSH:
		case ARM_INS_LDRSHT:
		{
			ty = irb.getInt16Ty();
			sext = true;
			break;
		}
		default:
		{
			throw GenericError("unhandled LDR id");
		}
	}

	uint32_t baseR = ARM_REG_INVALID;
	llvm::Value* idx = nullptr;
	bool subtract = false;
	if (ai->op_count == 2
			&& ai->operands[1].type == ARM_OP_MEM)
	{
		op1 = loadOpBinaryOp1(ai, irb, ty);
		baseR = ai->operands[1].mem.base;
		if (auto disp = ai->operands[1].mem.disp)
		{
			idx = llvm::ConstantInt::getSigned(getDefaultType(), disp);
			// Capstone 6.x reports `ldr r0, [r1, #-8]!` as disp=+8 and
			// subtracted=1 (5.x wrote disp=-8).
			subtract = ai->operands[1].subtracted;
		}
		else if (ai->operands[1].mem.index != ARM_REG_INVALID)
		{
			idx = loadMemIndexTerm(ai->operands[1], irb);
		}
	}
	else if (ai->op_count == 3
			&& ai->operands[1].type == ARM_OP_MEM)
	{
		op1 = loadOp(ai->operands[1], irb, ty);
		baseR = ai->operands[1].mem.base;
		idx = loadOp(ai->operands[2], irb);
		subtract = ai->operands[2].subtracted;
	}
	else
	{
		throw GenericError("unhandled LDR format");
	}

	if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(op1))
	{
		switch (i->id)
		{
			case ARM_INS_LDREX:
			case ARM_INS_LDREXB:
			case ARM_INS_LDREXH:
				ld->setAtomic(llvm::AtomicOrdering::Monotonic);
				break;
			case ARM_INS_LDA:
			case ARM_INS_LDAB:
			case ARM_INS_LDAH:
			case ARM_INS_LDAEX:
			case ARM_INS_LDAEXB:
			case ARM_INS_LDAEXH:
				ld->setAtomic(llvm::AtomicOrdering::Acquire);
				break;
			default:
				break;
		}
	}

	op1 = sext
			? irb.CreateSExtOrTrunc(op1, irb.getInt32Ty())
			: irb.CreateZExtOrTrunc(op1, irb.getInt32Ty());

	llvm::Value* v = nullptr;
	if (insnWriteback() && idx && baseR != ARM_REG_INVALID)
	{
		auto* b = loadRegister(baseR, irb);
		v = subtract
				? irb.CreateSub(b, idx)
				: irb.CreateAdd(b, idx);
		if (baseR != ARM_REG_PC)
		{
			storeRegister(baseR, v, irb);
		}
	}

	storeOp(ai->operands[0], op1, irb);

	bool op0Pc = ai->operands[0].type == ARM_OP_REG
			&& ai->operands[0].reg == ARM_REG_PC;
	if (!op0Pc && baseR == ARM_REG_PC && v)
	{
		storeRegister(baseR, v, irb);
	}
}

/**
 * ARM_INS_LDRD (double word)
 * ARM_INS_LDREXD / ARM_INS_LDAEXD (exclusive / acquire-exclusive)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateLdrd(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_SET(i, ai, irb, (std::set<unsigned>{3, 4}));

	uint32_t baseR = ARM_REG_INVALID;
	llvm::Value* idx = nullptr;
	bool subtract = false;
	if (ai->op_count == 3
			&& ai->operands[2].type == ARM_OP_MEM)
	{
		op1 = loadOp(ai->operands[2], irb, irb.getInt64Ty());
		baseR = ai->operands[2].mem.base;
		if (auto disp = ai->operands[2].mem.disp)
		{
			idx = llvm::ConstantInt::getSigned(getDefaultType(), disp);
			subtract = ai->operands[2].subtracted;
		}
		else if (ai->operands[2].mem.index != ARM_REG_INVALID)
		{
			idx = loadMemIndexTerm(ai->operands[2], irb);
		}
	}
	else if (ai->op_count == 4
			&& ai->operands[2].type == ARM_OP_MEM)
	{
		op1 = loadOp(ai->operands[2], irb, irb.getInt64Ty());
		baseR = ai->operands[2].mem.base;
		idx = loadOp(ai->operands[3], irb);
		subtract = ai->operands[3].subtracted;
	}
	else
	{
		throw GenericError("unhandled LDRD format");
	}

	if (i->id == ARM_INS_LDREXD || i->id == ARM_INS_LDAEXD)
	{
		if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(op1))
		{
			ld->setAtomic(
					i->id == ARM_INS_LDAEXD
							? llvm::AtomicOrdering::Acquire
							: llvm::AtomicOrdering::Monotonic);
		}
	}

	auto* lo = irb.CreateTrunc(op1, irb.getInt32Ty());
	auto* hi = irb.CreateTrunc(irb.CreateLShr(op1, 32), irb.getInt32Ty());

	llvm::Value* v = nullptr;
	if (insnWriteback() && idx && baseR != ARM_REG_INVALID)
	{
		auto* b = loadRegister(baseR, irb);
		v = subtract
				? irb.CreateSub(b, idx)
				: irb.CreateAdd(b, idx);
		if (baseR != ARM_REG_PC)
		{
			storeRegister(baseR, v, irb);
		}
	}

	bool op0Pc = ai->operands[0].type == ARM_OP_REG
			&& ai->operands[0].reg == ARM_REG_PC;
	bool op1Pc = ai->operands[1].type == ARM_OP_REG
			&& ai->operands[1].reg == ARM_REG_PC;

	// `LDRD <Rt>, <Rt2>, [<Rn>]` is `Rt <- MemA[Rn,4]; Rt2 <- MemA[Rn+4,4]`.
	// op1 above is a little-endian i64 load from the address, so `lo` is the
	// word at [Rn] and belongs in operands[0]. These two were the other way
	// round, which is the opposite of what the rest of this file does: STRD a
	// hundred lines down stores operands[0] at the base and operands[1] at
	// base+4, and UMULL/SMULL/UMLAL/SMLAL all put `lo` in operands[0].
	if (!op0Pc && !op1Pc)
	{
		storeOp(ai->operands[0], lo, irb);
		storeOp(ai->operands[1], hi, irb);
	}
	else if (op0Pc && !op1Pc)
	{
		storeOp(ai->operands[1], hi, irb);
		storeOp(ai->operands[0], lo, irb);
	}
	else if (!op0Pc && op1Pc)
	{
		storeOp(ai->operands[0], lo, irb);
		storeOp(ai->operands[1], hi, irb);
	}
	else
	{
		// Store only one.
		storeOp(ai->operands[0], lo, irb);
	}

	if (!op0Pc && !op1Pc && baseR == ARM_REG_PC && v)
	{
		storeRegister(baseR, v, irb);
	}
}

/**
 * ARM_INS_STR (word) == ARM_INS_STRT (unprivileged)
 * ARM_INS_STRB (byte) == ARM_INS_STRBT (unprivileged)
 * ARM_INS_STRH (half word) == ARM_INS_STRHT (unprivileged)
 * ARM_INS_STRD (double word)
 *
 * ARM_INS_STREX, ARM_INS_STREXB, ARM_INS_STREXH, ARM_INS_STREXD = Exclusive:
 * Conditional store, conditions check physical address atributes (e.g. TLB).
 * We are not able to check those here. Right now, we just ignore it and
 * generate ordinary stores, but we might generate ASM pseudo insn fnc call.
 * TODO:
 * One more operand - first operand is set to dst reg for returned status
 * -> Translation disabled, this will need more work to work.
 *
 * STR R0, [R4, #4]  ; simple offset: *(int*)(R4+4) = R0; R4 unchanged
 * STR R0, [R4, #4]! ; pre-indexed  : *(int*)(R4+4) = R0; R4 = R4+4
 * STR R0, [R4], #4  ; post-indexed : *(int*)(R4+0) = R0; R4 = R4+4
 */
void Capstone2LlvmIrTranslatorArm_impl::translateStr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count > 1));

	switch (i->id)
	{
		case ARM_INS_STR:
		case ARM_INS_STRT:
		case ARM_INS_STREX:
		case ARM_INS_STL:
		{
			op0 = loadOp(ai->operands[0], irb);
			op0 = irb.CreateZExtOrTrunc(op0, irb.getInt32Ty());
			break;
		}

		case ARM_INS_STRB:
		case ARM_INS_STRBT:
		case ARM_INS_STREXB:
		case ARM_INS_STLB:
		{
			op0 = loadOp(ai->operands[0], irb);
			op0 = irb.CreateZExtOrTrunc(op0, irb.getInt8Ty());
			break;
		}
		case ARM_INS_STRH:
		case ARM_INS_STRHT:
		case ARM_INS_STREXH:
		case ARM_INS_STLH:
		{
			op0 = loadOp(ai->operands[0], irb);
			op0 = irb.CreateZExtOrTrunc(op0, irb.getInt16Ty());
			break;
		}
		case ARM_INS_STRD:
		case ARM_INS_STREXD:
		{
			if (!(ai->op_count > 2))
			{
				throw GenericError("Unhandled STRD format.");
			}

			op0 = loadOp(ai->operands[0], irb);
			op1 = loadOp(ai->operands[1], irb);
			break;
		}
		default:
		{
			throw GenericError("Unhandled STR id.");
		}
	}

	uint32_t baseR = ARM_REG_INVALID;
	llvm::Value* idx = nullptr;
	bool subtract = false;
	if (i->id == ARM_INS_STRD || i->id == ARM_INS_STREXD)
	{
		if (ai->op_count == 3
				&& ai->operands[2].type == ARM_OP_MEM)
		{
			storeOp(ai->operands[2], op0, irb);

			auto op3 = ai->operands[2];
			op3.mem.disp += 4;
			storeOp(op3, op1, irb);

			baseR = ai->operands[2].mem.base;
			if (auto disp = ai->operands[2].mem.disp)
			{
				idx = llvm::ConstantInt::getSigned(getDefaultType(), disp);
				subtract = ai->operands[2].subtracted;
			}
			else if (ai->operands[2].mem.index != ARM_REG_INVALID)
			{
				idx = loadMemIndexTerm(ai->operands[2], irb);
			}
			// Maybe we should add +4 to idx?
		}
		else if (ai->op_count == 4
				&& ai->operands[2].type == ARM_OP_MEM)
		{
			storeOp(ai->operands[2], op0, irb);

			// We don't use op4 here, post-index offset is applied to source
			// address only after the transfers at writeback.
			auto op3 = ai->operands[2];
			op3.mem.disp += 4;
			storeOp(op3, op1, irb);

			baseR = ai->operands[2].mem.base;
			idx = loadOp(ai->operands[3], irb);
			subtract = ai->operands[3].subtracted;
		}
		else
		{
			throw GenericError("unhandled STRD format");
		}
	}
	else if (ai->op_count == 2
			&& ai->operands[1].type == ARM_OP_MEM)
	{
		auto* stored = storeOp(ai->operands[1], op0, irb);
		if (i->id == ARM_INS_STL || i->id == ARM_INS_STLB || i->id == ARM_INS_STLH)
		{
			if (auto* st = llvm::dyn_cast<llvm::StoreInst>(stored))
			{
				st->setAtomic(llvm::AtomicOrdering::Release);
			}
		}
		baseR = ai->operands[1].mem.base;
		if (auto disp = ai->operands[1].mem.disp)
		{
			idx = llvm::ConstantInt::getSigned(getDefaultType(), disp);
			// Capstone 6.x reports `ldr r0, [r1, #-8]!` as disp=+8 and
			// subtracted=1 (5.x wrote disp=-8).
			subtract = ai->operands[1].subtracted;
		}
		else if (ai->operands[1].mem.index != ARM_REG_INVALID)
		{
			idx = loadMemIndexTerm(ai->operands[1], irb);
		}
	}
	else if (ai->op_count == 3
			&& ai->operands[1].type == ARM_OP_MEM)
	{
		storeOp(ai->operands[1], op0, irb);
		baseR = ai->operands[1].mem.base;
		idx = loadOp(ai->operands[2], irb);
		subtract = ai->operands[2].subtracted;
	}
	else
	{
		throw GenericError("unhandled STRD format");
	}

	if (insnWriteback() && idx && baseR != ARM_REG_INVALID)
	{
		auto* b = loadRegister(baseR, irb);
		auto* v = subtract
				? irb.CreateSub(b, idx)
				: irb.CreateAdd(b, idx);
		storeRegister(baseR, v, irb);
	}
}

/**
 * ARM_INS_STREX, ARM_INS_STREXB, ARM_INS_STREXH, ARM_INS_STREXD
 * ARM_INS_STLEX, ARM_INS_STLEXB, ARM_INS_STLEXH, ARM_INS_STLEXD
 * Exclusive store: atomic store + status 0 (no exclusive-monitor model).
 * D-forms pack Rt|(Rt2<<32) like ARM64 STXP / ARM STRD.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateStrex(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	const bool pair = i->id == ARM_INS_STREXD || i->id == ARM_INS_STLEXD;
	EXPECT_IS_EXPR(i, ai, irb, (pair ? ai->op_count == 4 : ai->op_count == 3));

	llvm::Type* ty = nullptr;
	auto ordering = llvm::AtomicOrdering::Monotonic;
	switch (i->id)
	{
		case ARM_INS_STREX:
			ty = irb.getInt32Ty();
			break;
		case ARM_INS_STREXB:
			ty = irb.getInt8Ty();
			break;
		case ARM_INS_STREXH:
			ty = irb.getInt16Ty();
			break;
		case ARM_INS_STREXD:
			ty = irb.getInt64Ty();
			break;
		case ARM_INS_STLEX:
			ty = irb.getInt32Ty();
			ordering = llvm::AtomicOrdering::Release;
			break;
		case ARM_INS_STLEXB:
			ty = irb.getInt8Ty();
			ordering = llvm::AtomicOrdering::Release;
			break;
		case ARM_INS_STLEXH:
			ty = irb.getInt16Ty();
			ordering = llvm::AtomicOrdering::Release;
			break;
		case ARM_INS_STLEXD:
			ty = irb.getInt64Ty();
			ordering = llvm::AtomicOrdering::Release;
			break;
		default:
			throw GenericError("ARM: unhandled STREX id");
	}

	llvm::Value* val = nullptr;
	llvm::Value* dest = nullptr;
	if (pair)
	{
		auto* v0 = loadOp(ai->operands[1], irb);
		auto* v1 = loadOp(ai->operands[2], irb);
		v0 = irb.CreateZExtOrTrunc(v0, ty);
		v1 = irb.CreateZExtOrTrunc(v1, ty);
		val = irb.CreateOr(
				v0,
				irb.CreateShl(v1, llvm::ConstantInt::get(ty, 32)));
		dest = loadOp(ai->operands[3], irb, nullptr, true);
	}
	else
	{
		val = loadOp(ai->operands[1], irb);
		val = irb.CreateZExtOrTrunc(val, ty);
		dest = loadOp(ai->operands[2], irb, nullptr, true);
	}

	auto* st = storeIntPtr(irb, val, dest, ty);
	st->setAtomic(ordering);
	storeRegister(
			ai->operands[0].reg,
			llvm::ConstantInt::get(getRegisterType(ai->operands[0].reg), 0),
			irb);
}

/**
 * ARM_INS_SWP, ARM_INS_SWPB
 * Atomic swap: Rd = *Rn; *Rn = Rm. Seq_cst like x86 xchg mem.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateSwp(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(
			i,
			ai,
			irb,
			(ai->op_count == 3
					&& ai->operands[0].type == ARM_OP_REG
					&& ai->operands[1].type == ARM_OP_REG
					&& ai->operands[2].type == ARM_OP_MEM));

	llvm::Type* ty = (i->id == ARM_INS_SWPB) ? irb.getInt8Ty() : irb.getInt32Ty();

	auto* addr = loadOp(ai->operands[2], irb, nullptr, true);
	auto* val = loadOp(ai->operands[1], irb);
	if (!addr || !val)
	{
		return;
	}
	val = irb.CreateZExtOrTrunc(val, ty);
	auto* ptr = intToPtr(irb, addr, ty);
	auto* old = irb.CreateAtomicRMW(
			llvm::AtomicRMWInst::Xchg,
			ptr,
			val,
			llvm::MaybeAlign(),
			llvm::AtomicOrdering::SequentiallyConsistent);
	attachPointeeType(old, ty);
	storeRegister(
			ai->operands[0].reg,
			irb.CreateZExtOrTrunc(old, getRegisterType(ai->operands[0].reg)),
			irb);
}

/**
 * ARM_INS_SUB, ARM_INS_RSB, ARM_INS_CMP (SUBS but result is discarded)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateSub(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	// Capstone 6.x: ARM_INS_NEG is the same enumerator as ARM_INS_RSB.
	// Only the two-operand `neg` alias (alias_id or RSB with 2 ops) is
	// `rd = 0 - rm`. CMP/SUB are also two-operand and must not take this path.
	bool isNeg = false;
#ifdef ARM_INS_ALIAS_NEG
	isNeg = i->is_alias
			&& static_cast<unsigned>(i->alias_id) == static_cast<unsigned>(ARM_INS_ALIAS_NEG);
#endif
	if (!isNeg && ai->op_count == 2 && i->id == ARM_INS_RSB
			&& i->id != ARM_INS_CMP && i->id != ARM_INS_SUB
			&& i->id != ARM_INS_SUBS)
	{
		isNeg = true;
	}
	if (isNeg)
	{
		op2 = loadOpBinaryOp1(ai, irb);
		op1 = llvm::ConstantInt::get(op2->getType(), 0);
	}
	else if (armIsId(i, ARM_INS_RSB))
	{
		std::tie(op2, op1) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	}
	auto* sub = irb.CreateSub(op1, op2);
	if (ai->update_flags || i->id == ARM_INS_CMP || i->id == ARM_INS_SUBS || isNeg)
	{
		llvm::Value* zero = llvm::ConstantInt::get(sub->getType(), 0);

		// ARM - ok, but maybe generates more ugly code.
		storeRegister(ARM_REG_CPSR_C, generateValueNegate(irb, generateBorrowSub(op1, op2, irb)), irb);
		// THUMB - weird, but at least in ackermann.thumb.gnuarmgcc-4.4.1.O0.g.elf
		// it generates prettier code. I'm not even sure they are the same.
//		auto* op2Neg = generateValueNegate(irb, op2);
//		storeRegister(ARM_REG_CPSR_C, genCarryAddC(op1, op2Neg, irb, llvm::ConstantInt::getSigned(op2Neg->getType(), -1)), irb);

		storeRegister(ARM_REG_CPSR_V, generateOverflowSub(sub, op1, op2, irb), irb);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(sub, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(op1, op2), irb);
	}
	if (i->id != ARM_INS_CMP)
	{
		storeOp(ai->operands[0], sub, irb);
	}
}

/**
 * ARM_INS_UMLAL, ARM_INS_SMLAL
 */
void Capstone2LlvmIrTranslatorArm_impl::translateUmlal(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, ai, irb);

	op0 = loadOp(ai->operands[0], irb);
	op0 = irb.CreateZExtOrTrunc(op0, irb.getInt64Ty());
	op1 = loadOp(ai->operands[1], irb);
	op1 = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
	op1 = irb.CreateShl(op1, 32);

	auto* orig = irb.CreateOr(op0, op1);

	op2 = loadOp(ai->operands[2], irb);
	op2 = i->id == ARM_INS_UMLAL
			? irb.CreateZExtOrTrunc(op2, irb.getInt64Ty())
			: irb.CreateSExtOrTrunc(op2, irb.getInt64Ty());
	op3 = loadOp(ai->operands[3], irb);
	op3 = i->id == ARM_INS_UMLAL
			? irb.CreateZExtOrTrunc(op3, irb.getInt64Ty())
			: irb.CreateSExtOrTrunc(op3, irb.getInt64Ty());

	auto* val = irb.CreateMul(op2, op3);
	val = irb.CreateAdd(orig, val);

	auto* hi = irb.CreateTrunc(irb.CreateLShr(val, 32), irb.getInt32Ty());
	auto* lo = irb.CreateTrunc(val, irb.getInt32Ty());

	// - Updates the N and Z flags according to the result.
	// - Does not affect the C or V flags.
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}

	storeOp(ai->operands[0], lo, irb);
	storeOp(ai->operands[1], hi, irb);
}

/**
 * ARM_INS_UMULL, ARM_INS_SMULL
 */
void Capstone2LlvmIrTranslatorArm_impl::translateUmull(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, ai, irb);

	op2 = loadOp(ai->operands[2], irb);
	op2 = i->id == ARM_INS_UMULL
			? irb.CreateZExtOrTrunc(op2, irb.getInt64Ty())
			: irb.CreateSExtOrTrunc(op2, irb.getInt64Ty());
	op3 = loadOp(ai->operands[3], irb);
	op3 = i->id == ARM_INS_UMULL
			? irb.CreateZExtOrTrunc(op3, irb.getInt64Ty())
			: irb.CreateSExtOrTrunc(op3, irb.getInt64Ty());

	auto* val = irb.CreateMul(op2, op3);

	auto* hi = irb.CreateTrunc(irb.CreateLShr(val, 32), irb.getInt32Ty());
	auto* lo = irb.CreateTrunc(val, irb.getInt32Ty());

	// - Updates the N and Z flags according to the result.
	// - Does not affect the C or V flags.
	if (ai->update_flags)
	{
		llvm::Value* zero = llvm::ConstantInt::get(val->getType(), 0);
		storeRegister(ARM_REG_CPSR_N, irb.CreateICmpSLT(val, zero), irb);
		storeRegister(ARM_REG_CPSR_Z, irb.CreateICmpEQ(val, zero), irb);
	}

	storeOp(ai->operands[0], lo, irb);
	storeOp(ai->operands[1], hi, irb);
}

/**
 * ARM_INS_UXTAH, ARM_INS_UXTAB, ARM_INS_UXTAB16
 *
 * Zero-extend a halfword or byte of Rm (after the optional rotate Capstone
 * already folded into the operand) and add Rn. UXTAB16 does that in each
 * halfword independently.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateUxtah(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	auto* i32 = getDefaultType();

	if (i->id == ARM_INS_UXTAB16)
	{
		auto* mask16 = llvm::ConstantInt::get(i32, 0xffff);
		auto* mask8 = llvm::ConstantInt::get(i32, 0xff);
		llvm::Value* lo = irb.CreateAnd(op2, mask8);
		llvm::Value* hi = irb.CreateAnd(irb.CreateLShr(op2, llvm::ConstantInt::get(i32, 16)), mask8);
		lo = irb.CreateAnd(irb.CreateAdd(lo, irb.CreateAnd(op1, mask16)), mask16);
		hi = irb.CreateAnd(irb.CreateAdd(hi, irb.CreateLShr(op1, llvm::ConstantInt::get(i32, 16))), mask16);
		storeOp(ai->operands[0], irb.CreateOr(lo, irb.CreateShl(hi, llvm::ConstantInt::get(i32, 16))), irb);
		return;
	}

	unsigned from = i->id == ARM_INS_UXTAB ? 8 : 16;
	op2 = irb.CreateZExtOrTrunc(op2, irb.getIntNTy(from));
	op2 = irb.CreateZExtOrTrunc(op2, i32);
	op0 = irb.CreateAdd(op1, op2);
	storeOp(ai->operands[0], op0, irb);
}

/**
 * ARM_INS_UXTB
 */
void Capstone2LlvmIrTranslatorArm_impl::translateUxtb(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);
	op1 = irb.CreateAnd(op1, 0x000000ff);
	storeOp(ai->operands[0], op1, irb);
}

/**
 * ARM_INS_UXTB16
 */
void Capstone2LlvmIrTranslatorArm_impl::translateUxtb16(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);
	op1 = irb.CreateAnd(op1, 0x00ff00ff);
	storeOp(ai->operands[0], op1, irb);
}

/**
 * ARM_INS_UXTH
 */
void Capstone2LlvmIrTranslatorArm_impl::translateUxth(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);
	op1 = irb.CreateAnd(op1, 0x0000ffff);
	storeOp(ai->operands[0], op1, irb);
}

/**
 * ARM_INS_SDIV, ARM_INS_UDIV
 *
 * Compiler-used integer divide on ARMv7-A/R and Thumb-2. Division by zero
 * and signed overflow (INT_MIN / -1) are architecturally UNPREDICTABLE;
 * the translation uses the same defined answers as ARM64 so the IR never
 * carries LLVM poison that later passes could delete.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateDiv(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	auto* ty = op1->getType();
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* one = llvm::ConstantInt::get(ty, 1);
	auto* divZero = irb.CreateICmpEQ(op2, zero);

	llvm::Value* val = nullptr;
	if (armIsId(i, ARM_INS_UDIV))
	{
		auto* safe = irb.CreateSelect(divZero, one, op2);
		val = irb.CreateSelect(divZero, zero, irb.CreateUDiv(op1, safe));
	}
	else
	{
		unsigned bits = llvm::cast<llvm::IntegerType>(ty)->getBitWidth();
		auto* intMin = llvm::ConstantInt::get(ty, llvm::APInt::getSignedMinValue(bits));
		auto* minusOne = llvm::ConstantInt::getSigned(ty, -1);
		auto* overflow = irb.CreateAnd(irb.CreateICmpEQ(op1, intMin), irb.CreateICmpEQ(op2, minusOne));
		auto* safe = irb.CreateSelect(irb.CreateOr(divZero, overflow), one, op2);
		val = irb.CreateSelect(divZero, zero, irb.CreateSelect(overflow, intMin, irb.CreateSDiv(op1, safe)));
	}
	storeOp(ai->operands[0], val, irb);
}

/**
 * ARM_INS_TBB, ARM_INS_TBH -- Thumb-2 table branch.
 *
 *     tbb [Rn, Rm]              PC = PC + 2 * ZeroExtend(MemU8[Rn+Rm])
 *     tbh [Rn, Rm, lsl #1]      PC = PC + 2 * ZeroExtend(MemU16[Rn+(Rm<<1)])
 *
 * PC is the address of this instruction plus 4. Capstone reports a single
 * memory operand; loadOp(..., lea=true) already applies the LSL #1 on TBH.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateTbb(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	if (ai->operands[0].type != ARM_OP_MEM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* addr = loadOp(ai->operands[0], irb, nullptr, true);
	auto* elemTy = i->id == ARM_INS_TBH ? irb.getInt16Ty() : irb.getInt8Ty();
	auto* entry = irb.CreateZExt(loadIntPtr(irb, addr, elemTy), getDefaultType());
	auto* disp = irb.CreateShl(entry, llvm::ConstantInt::get(entry->getType(), 1));
	auto* target = irb.CreateAdd(getCurrentPc(i), disp);
	generateBranchFunctionCall(irb, target);
}

/**
 * ARM_INS_PKHBT, ARM_INS_PKHTB
 *
 * Pack the bottom half of one register with the top half of the other after
 * the optional shift Capstone folds into Rm.
 *
 *     pkhbt rd, rn, rm{, lsl #n}   rd[15:0] = rn[15:0]; rd[31:16] = rm_shifted[31:16]
 *     pkhtb rd, rn, rm{, asr #n}   rd[31:16] = rn[31:16]; rd[15:0] = rm_shifted[15:0]
 */
void Capstone2LlvmIrTranslatorArm_impl::translatePkh(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	auto* i32 = getDefaultType();
	auto* maskLo = llvm::ConstantInt::get(i32, 0x0000ffff);
	auto* maskHi = llvm::ConstantInt::get(i32, 0xffff0000);
	llvm::Value* res = i->id == ARM_INS_PKHTB
			? irb.CreateOr(irb.CreateAnd(op1, maskHi), irb.CreateAnd(op2, maskLo))
			: irb.CreateOr(irb.CreateAnd(op1, maskLo), irb.CreateAnd(op2, maskHi));
	storeOp(ai->operands[0], res, irb);
}

/**
 * ARM_INS_SSAT, ARM_INS_USAT
 *
 * Saturate a (possibly shifted) signed source to a signed or unsigned field
 * of `sat` bits. Q is not modelled.
 *
 *     ssat rd, #sat, rn{, shift}   clamp to [-2^(sat-1), 2^(sat-1)-1], sat in 1..32
 *     usat rd, #sat, rn{, shift}   clamp signed rn to [0, 2^sat-1], sat in 0..31
 */
void Capstone2LlvmIrTranslatorArm_impl::translateSat(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count >= 3));

	if (ai->operands[1].type != ARM_OP_IMM)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned sat = static_cast<unsigned>(ai->operands[1].imm);
	auto* src = loadOp(ai->operands[2], irb);
	auto* ty = src->getType();
	unsigned bits = llvm::cast<llvm::IntegerType>(ty)->getBitWidth();

	if (i->id == ARM_INS_SSAT)
	{
		if (sat == 0 || sat > bits)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		if (sat == bits)
		{
			storeOp(ai->operands[0], src, irb);
			return;
		}
		auto* minV = llvm::ConstantInt::getSigned(ty, -(int64_t(1) << (sat - 1)));
		auto* maxV = llvm::ConstantInt::getSigned(ty, (int64_t(1) << (sat - 1)) - 1);
		auto* res = irb.CreateSelect(
				irb.CreateICmpSLT(src, minV),
				minV,
				irb.CreateSelect(irb.CreateICmpSGT(src, maxV), maxV, src));
		storeOp(ai->operands[0], res, irb);
		return;
	}

	if (sat > bits - 1)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* maxV = llvm::ConstantInt::get(ty, sat == 0 ? 0 : ((uint64_t(1) << sat) - 1));
	auto* nonNeg = irb.CreateSelect(irb.CreateICmpSLT(src, zero), zero, src);
	auto* res = irb.CreateSelect(irb.CreateICmpUGT(nonNeg, maxV), maxV, nonNeg);
	storeOp(ai->operands[0], res, irb);
}

/**
 * 16-bit (and 32x16) signed multiply family the compiler emits for int16_t
 * arithmetic: SMULBB/BT/TB/TT, SMLABB/BT/TB/TT, SMULWB/WT, SMLAWB/WT.
 *
 * Bottom/top of Rn and Rm are bits [15:0] / [31:16]. SMULW* multiplies the
 * full 32-bit Rn by a 16-bit half of Rm and takes bits [47:16] of the 48-bit
 * product (arithmetic shift of the 64-bit product by 16).
 */
void Capstone2LlvmIrTranslatorArm_impl::translateHalfwordMul(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool topN = false;
	bool topM = false;
	bool acc = false;
	bool wide = false;
	switch (i->id)
	{
		case ARM_INS_SMULBB: break;
		case ARM_INS_SMULBT: topM = true; break;
		case ARM_INS_SMULTB: topN = true; break;
		case ARM_INS_SMULTT: topN = true; topM = true; break;
		case ARM_INS_SMLABB: acc = true; break;
		case ARM_INS_SMLABT: acc = true; topM = true; break;
		case ARM_INS_SMLATB: acc = true; topN = true; break;
		case ARM_INS_SMLATT: acc = true; topN = true; topM = true; break;
		case ARM_INS_SMULWB: wide = true; break;
		case ARM_INS_SMULWT: wide = true; topM = true; break;
		case ARM_INS_SMLAWB: wide = true; acc = true; break;
		case ARM_INS_SMLAWT: wide = true; acc = true; topM = true; break;
		default:
			throw GenericError("translateHalfwordMul(): unhandled instruction id");
	}

	if (acc)
	{
		EXPECT_IS_QUATERNARY(i, ai, irb);
	}
	else
	{
		EXPECT_IS_TERNARY(i, ai, irb);
	}

	auto* n = loadOp(ai->operands[1], irb);
	auto* m = loadOp(ai->operands[2], irb);
	auto* i32 = getDefaultType();
	auto* i16 = irb.getInt16Ty();
	auto extractHalf = [&](llvm::Value* v, bool top) {
		if (top)
		{
			v = irb.CreateLShr(v, llvm::ConstantInt::get(i32, 16));
		}
		return irb.CreateSExt(irb.CreateTrunc(v, i16), i32);
	};

	llvm::Value* res = nullptr;
	if (wide)
	{
		auto* i64 = irb.getInt64Ty();
		auto* half = extractHalf(m, topM);
		auto* prod = irb.CreateMul(irb.CreateSExt(n, i64), irb.CreateSExt(half, i64));
		res = irb.CreateTrunc(irb.CreateAShr(prod, llvm::ConstantInt::get(i64, 16)), i32);
	}
	else
	{
		res = irb.CreateMul(extractHalf(n, topN), extractHalf(m, topM));
	}
	if (acc)
	{
		res = irb.CreateAdd(res, loadOp(ai->operands[3], irb));
	}
	storeOp(ai->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonArith(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (ai->op_count != 3 || !neonBinaryRegs(ai, 3, bits) || laneBits == 0 || bits % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, isFp, irb);
	llvm::Value* b = loadNeonVector(ai->operands[2].reg, laneBits, isFp, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
	unsigned lanes = vecTy->getNumElements();
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* ea = irb.CreateExtractElement(a, k);
		llvm::Value* eb = irb.CreateExtractElement(b, k);
		llvm::Value* o = nullptr;
		switch (i->id)
		{
		case ARM_INS_VADD: o = isFp ? irb.CreateFAdd(ea, eb) : irb.CreateAdd(ea, eb); break;
		case ARM_INS_VSUB: o = isFp ? irb.CreateFSub(ea, eb) : irb.CreateSub(ea, eb); break;
		case ARM_INS_VMUL: o = isFp ? irb.CreateFMul(ea, eb) : irb.CreateMul(ea, eb); break;
		case ARM_INS_VMAX:
			o = isFp ? irb.CreateSelect(irb.CreateFCmpOGT(ea, eb), ea, eb)
					 : irb.CreateSelect(isSigned ? irb.CreateICmpSGT(ea, eb) : irb.CreateICmpUGT(ea, eb), ea, eb);
			break;
		case ARM_INS_VMIN:
			o = isFp ? irb.CreateSelect(irb.CreateFCmpOLT(ea, eb), ea, eb)
					 : irb.CreateSelect(isSigned ? irb.CreateICmpSLT(ea, eb) : irb.CreateICmpULT(ea, eb), ea, eb);
			break;
		default: translatePseudoAsmGeneric(i, ai, irb); return;
		}
		res = irb.CreateInsertElement(res, o, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonLogic(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	unsigned n = (i->id == ARM_INS_VMVN) ? 2 : (i->id == ARM_INS_VBSL || i->id == ARM_INS_VBIT || i->id == ARM_INS_VBIF)
												   ? 3
												   : 3;
	if (i->id == ARM_INS_VMVN)
	{
		if (ai->op_count != 2 || !neonBinaryRegs(ai, 2, bits))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		storeNeonBits(ai->operands[0].reg, irb.CreateNot(loadNeonBits(ai->operands[1].reg, irb)), irb);
		return;
	}

	if (ai->op_count != 3 || !neonBinaryRegs(ai, 3, bits))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* a = loadNeonBits(ai->operands[1].reg, irb);
	auto* b = loadNeonBits(ai->operands[2].reg, irb);
	llvm::Value* r = nullptr;
	switch (i->id)
	{
	case ARM_INS_VAND: r = irb.CreateAnd(a, b); break;
	case ARM_INS_VEOR: r = irb.CreateXor(a, b); break;
	case ARM_INS_VORR: r = irb.CreateOr(a, b); break;
	case ARM_INS_VBIC: r = irb.CreateAnd(a, irb.CreateNot(b)); break;
	case ARM_INS_VORN: r = irb.CreateOr(a, irb.CreateNot(b)); break;
	case ARM_INS_VBSL:
	{
		auto* dest = loadNeonBits(ai->operands[0].reg, irb);
		r = irb.CreateOr(irb.CreateAnd(a, dest), irb.CreateAnd(b, irb.CreateNot(dest)));
		break;
	}
	case ARM_INS_VBIT:
	{
		auto* dest = loadNeonBits(ai->operands[0].reg, irb);
		r = irb.CreateOr(irb.CreateAnd(a, b), irb.CreateAnd(dest, irb.CreateNot(b)));
		break;
	}
	case ARM_INS_VBIF:
	{
		auto* dest = loadNeonBits(ai->operands[0].reg, irb);
		r = irb.CreateOr(irb.CreateAnd(dest, b), irb.CreateAnd(a, irb.CreateNot(b)));
		break;
	}
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}
	storeNeonBits(ai->operands[0].reg, r, irb);
	(void)n;
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonCmp(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	bool immZero = ai->op_count == 3 && (ai->operands[2].type == ARM_OP_IMM || ai->operands[2].type == ARM_OP_FP);
	if (laneBits == 0 || !neonBinaryRegs(ai, immZero ? 2 : 3, bits) || bits % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, isFp, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
	unsigned lanes = vecTy->getNumElements();
	llvm::Value* b = immZero ? llvm::Constant::getNullValue(vecTy)
							 : loadNeonVector(ai->operands[2].reg, laneBits, isFp, irb);
	auto* iLane = irb.getIntNTy(laneBits);
	auto* iVec = llvm::FixedVectorType::get(iLane, lanes);
	llvm::Value* res = llvm::PoisonValue::get(iVec);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* ea = irb.CreateExtractElement(a, k);
		llvm::Value* eb = irb.CreateExtractElement(b, k);
		llvm::Value* c = nullptr;
		switch (i->id)
		{
		case ARM_INS_VCEQ: c = isFp ? irb.CreateFCmpOEQ(ea, eb) : irb.CreateICmpEQ(ea, eb); break;
		case ARM_INS_VCGE:
			c = isFp ? irb.CreateFCmpOGE(ea, eb)
					 : (isSigned ? irb.CreateICmpSGE(ea, eb) : irb.CreateICmpUGE(ea, eb));
			break;
		case ARM_INS_VCGT:
			c = isFp ? irb.CreateFCmpOGT(ea, eb)
					 : (isSigned ? irb.CreateICmpSGT(ea, eb) : irb.CreateICmpUGT(ea, eb));
			break;
		case ARM_INS_VCLE:
			c = isFp ? irb.CreateFCmpOLE(ea, eb)
					 : (isSigned ? irb.CreateICmpSLE(ea, eb) : irb.CreateICmpULE(ea, eb));
			break;
		case ARM_INS_VCLT:
			c = isFp ? irb.CreateFCmpOLT(ea, eb)
					 : (isSigned ? irb.CreateICmpSLT(ea, eb) : irb.CreateICmpULT(ea, eb));
			break;
		case ARM_INS_VTST:
			c = irb.CreateICmpNE(irb.CreateAnd(ea, eb), llvm::ConstantInt::get(ea->getType(), 0));
			break;
		default: translatePseudoAsmGeneric(i, ai, irb); return;
		}
		res = irb.CreateInsertElement(res, irb.CreateSExt(c, iLane), k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonShift(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (isFp || ai->op_count != 3 || ai->operands[2].type != ARM_OP_IMM || !neonBinaryRegs(ai, 2, bits)
		|| laneBits == 0 || bits % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	uint64_t amount = static_cast<uint64_t>(ai->operands[2].imm);
	llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
	unsigned lanes = vecTy->getNumElements();
	llvm::Value* acc = (i->id == ARM_INS_VSRA) ? loadNeonVector(ai->operands[0].reg, laneBits, false, irb) : nullptr;
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* e = irb.CreateExtractElement(a, k);
		llvm::Value* sh = nullptr;
		if (i->id == ARM_INS_VSHL)
		{
			if (amount >= laneBits)
			{
				sh = llvm::ConstantInt::get(e->getType(), 0);
			}
			else
			{
				sh = irb.CreateShl(e, amount);
			}
		}
		else
		{
			if (amount >= laneBits)
			{
				sh = isSigned ? irb.CreateAShr(e, laneBits - 1) : llvm::ConstantInt::get(e->getType(), 0);
			}
			else
			{
				sh = isSigned ? irb.CreateAShr(e, amount) : irb.CreateLShr(e, amount);
			}
			if (acc)
			{
				sh = irb.CreateAdd(irb.CreateExtractElement(acc, k), sh);
			}
		}
		res = irb.CreateInsertElement(res, sh, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonDup(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count != 2 || ai->operands[0].type != ARM_OP_REG || !isNeonRegister(ai->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (laneBits == 0)
	{
		laneBits = 32;
	}
	unsigned width = neonBitWidth(ai->operands[0].reg);
	unsigned lanes = width / laneBits;
	auto* laneTy = irb.getIntNTy(laneBits);
	auto* vecTy = llvm::FixedVectorType::get(laneTy, lanes);

	llvm::Value* lane = nullptr;
	auto& src = ai->operands[1];
	if (src.type == ARM_OP_REG && isNeonRegister(src.reg))
	{
		int idx = operandLane(src);
		llvm::Value* v = loadNeonVector(src.reg, laneBits, false, irb);
		lane = irb.CreateExtractElement(v, static_cast<uint64_t>(idx < 0 ? 0 : idx));
	}
	else
	{
		lane = irb.CreateZExtOrTrunc(loadOp(src, irb), laneTy);
	}

	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		res = irb.CreateInsertElement(res, lane, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
	(void)i;
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonExt(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	if (ai->op_count != 4 || ai->operands[3].type != ARM_OP_IMM || !neonBinaryRegs(ai, 3, bits))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned laneBits = 8;
	unsigned lanes = bits / laneBits;
	llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
	llvm::Value* b = loadNeonVector(ai->operands[2].reg, laneBits, false, irb);
	unsigned imm = static_cast<unsigned>(ai->operands[3].imm);
	if (imm >= lanes)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		uint64_t src = k + imm;
		llvm::Value* e = src < lanes ? irb.CreateExtractElement(a, src) : irb.CreateExtractElement(b, src - lanes);
		res = irb.CreateInsertElement(res, e, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
	(void)i;
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonSwp(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	if (ai->op_count != 2 || !neonBinaryRegs(ai, 2, bits))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	auto* a = loadNeonBits(ai->operands[0].reg, irb);
	auto* b = loadNeonBits(ai->operands[1].reg, irb);
	storeNeonBits(ai->operands[0].reg, b, irb);
	storeNeonBits(ai->operands[1].reg, a, irb);
	(void)i;
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonLdSt1(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count < 2)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned memIdx = ai->op_count - 1;
	if (ai->operands[memIdx].type != ARM_OP_MEM)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	bool load = i->id == ARM_INS_VLD1;
	auto* addr = loadOp(ai->operands[memIdx], irb, nullptr, true);
	uint64_t off = 0;
	for (unsigned j = 0; j < memIdx; ++j)
	{
		if (ai->operands[j].type != ARM_OP_REG || !isNeonRegister(ai->operands[j].reg))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		int lane = operandLane(ai->operands[j]);
		bool isFp = false, isSigned = false;
		unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
		if (laneBits == 0)
		{
			laneBits = neonBitWidth(ai->operands[j].reg);
		}
		auto* at = irb.CreateAdd(addr, llvm::ConstantInt::get(addr->getType(), off));
		if (lane >= 0)
		{
			auto* elemTy = irb.getIntNTy(laneBits);
			if (load)
			{
				llvm::Value* v = loadNeonVector(ai->operands[j].reg, laneBits, false, irb);
				auto* e = loadIntPtr(irb, at, elemTy);
				storeNeonVector(ai->operands[j].reg, irb.CreateInsertElement(v, e, static_cast<uint64_t>(lane)), irb);
			}
			else
			{
				llvm::Value* v = loadNeonVector(ai->operands[j].reg, laneBits, false, irb);
				storeIntPtr(irb, irb.CreateExtractElement(v, static_cast<uint64_t>(lane)), at, elemTy);
			}
			off += laneBits / 8;
		}
		else
		{
			unsigned w = neonBitWidth(ai->operands[j].reg);
			llvm::Type* memTy = (w == 64) ? static_cast<llvm::Type*>(irb.getDoubleTy())
										  : (w == 32) ? static_cast<llvm::Type*>(irb.getFloatTy())
													  : irb.getIntNTy(w);
			if (load)
			{
				storeNeonBits(ai->operands[j].reg, loadIntPtr(irb, at, memTy), irb);
			}
			else
			{
				auto* bits = loadNeonBits(ai->operands[j].reg, irb);
				storeIntPtr(irb, generateTypeConversion(irb, bits, memTy, eOpConv::FPCAST_OR_BITCAST), at, memTy);
			}
			off += w / 8;
		}
	}

	if (insnWriteback())
	{
		uint32_t base = ai->operands[memIdx].mem.base;
		if (base != ARM_REG_INVALID)
		{
			storeRegister(base, irb.CreateAdd(loadRegister(base, irb), llvm::ConstantInt::get(getDefaultType(), off)), irb);
		}
	}
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonRev(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (ai->op_count != 2 || !neonBinaryRegs(ai, 2, bits) || laneBits == 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned groupBits = (i->id == ARM_INS_VREV16) ? 16 : (i->id == ARM_INS_VREV32) ? 32 : 64;
	if (groupBits <= laneBits || bits % groupBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned lanes = bits / laneBits;
	unsigned groupLanes = groupBits / laneBits;
	llvm::Value* v = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(v->getType());
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		uint64_t g = k / groupLanes;
		uint64_t within = k % groupLanes;
		uint64_t src = g * groupLanes + (groupLanes - 1 - within);
		res = irb.CreateInsertElement(res, irb.CreateExtractElement(v, src), k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateVfpLdmStm(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count > 0));

	if (ai->operands[0].type != ARM_OP_REG)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	// Capstone 6.x reports VPUSH/VPOP as VSTMDB/VLDMIA with only the FP
	// list — SP is implied. A real VLDM/VSTM has a GPR (or SP) in op0.
	bool listOnly = isFpRegister(ai->operands[0].reg) || isQuadView(ai->operands[0].reg);
	unsigned first = listOnly ? 0u : 1u;
	if (!listOnly && ai->op_count < 2)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	for (unsigned j = first; j < ai->op_count; ++j)
	{
		if (ai->operands[j].type != ARM_OP_REG || !isNeonRegister(ai->operands[j].reg))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}

	bool load = i->id == ARM_INS_VLDMIA || i->id == ARM_INS_VLDMDB
			|| armIsId(i, ARM_INS_VPOP);
	bool db = i->id == ARM_INS_VLDMDB || i->id == ARM_INS_VSTMDB
			|| armIsId(i, ARM_INS_VPUSH);
	auto* spTy = getDefaultType();
	auto* base = listOnly ? loadRegister(ARM_REG_SP, irb) : loadOp(ai->operands[0], irb);
	if (base && !base->getType()->isIntegerTy())
	{
		base = generateTypeConversion(irb, base, spTy, eOpConv::FPCAST_OR_BITCAST);
	}
	uint64_t total = 0;
	for (unsigned j = first; j < ai->op_count; ++j)
	{
		total += neonBitWidth(ai->operands[j].reg) / 8;
	}
	auto* at = db ? irb.CreateSub(base, llvm::ConstantInt::get(base->getType(), total)) : base;
	uint64_t off = 0;
	for (unsigned j = first; j < ai->op_count; ++j)
	{
		unsigned w = neonBitWidth(ai->operands[j].reg);
		llvm::Type* ty = nullptr;
		if (isQuadView(ai->operands[j].reg))
		{
			ty = irb.getIntNTy(128);
		}
		else if (ai->operands[j].reg >= ARM_REG_S0 && ai->operands[j].reg <= ARM_REG_S31)
		{
			ty = irb.getFloatTy();
		}
		else
		{
			ty = irb.getDoubleTy();
		}
		auto* p = irb.CreateAdd(at, llvm::ConstantInt::get(base->getType(), off));
		if (load)
		{
			if (isQuadView(ai->operands[j].reg))
			{
				storeNeonBits(ai->operands[j].reg, loadIntPtr(irb, p, irb.getIntNTy(128)), irb);
			}
			else
			{
				storeOp(ai->operands[j], loadIntPtr(irb, p, ty), irb, eOpConv::FPCAST_OR_BITCAST);
			}
		}
		else
		{
			if (isQuadView(ai->operands[j].reg))
			{
				storeIntPtr(irb, loadNeonBits(ai->operands[j].reg, irb), p, irb.getIntNTy(128));
			}
			else
			{
				storeIntPtr(irb, loadVfpOp(ai->operands[j], irb, ty), p, ty);
			}
		}
		off += w / 8;
	}
	if (listOnly || insnWriteback())
	{
		auto* nb = db ? at : irb.CreateAdd(base, llvm::ConstantInt::get(base->getType(), total));
		if (listOnly)
		{
			storeRegister(ARM_REG_SP, nb, irb);
		}
		else
		{
			storeOp(ai->operands[0], nb, irb);
		}
	}
}

void Capstone2LlvmIrTranslatorArm_impl::translateVmsr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count != 2 || ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	storeRegister(ARM_REG_FPSCR, loadOp(ai->operands[1], irb), irb);
	(void)i;
}

void Capstone2LlvmIrTranslatorArm_impl::translateQadd(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	auto* rm = loadOp(ai->operands[1], irb);
	auto* rn = loadOp(ai->operands[2], irb);
	auto* ty = rm->getType();
	auto* i64 = irb.getInt64Ty();
	auto* minV = llvm::ConstantInt::getSigned(ty, INT32_MIN);
	auto* maxV = llvm::ConstantInt::getSigned(ty, INT32_MAX);

	auto sat32 = [&](llvm::Value* wide) {
		auto* lo = llvm::ConstantInt::getSigned(i64, INT32_MIN);
		auto* hi = llvm::ConstantInt::getSigned(i64, INT32_MAX);
		auto* clamped = irb.CreateSelect(
			irb.CreateICmpSLT(wide, lo), lo, irb.CreateSelect(irb.CreateICmpSGT(wide, hi), hi, wide));
		return irb.CreateTrunc(clamped, ty);
	};

	llvm::Value* a = irb.CreateSExt(rm, i64);
	llvm::Value* b = irb.CreateSExt(rn, i64);
	if (i->id == ARM_INS_QDADD || i->id == ARM_INS_QDSUB)
	{
		b = irb.CreateSExt(sat32(irb.CreateShl(b, 1)), i64);
	}
	llvm::Value* sum = (i->id == ARM_INS_QSUB || i->id == ARM_INS_QDSUB) ? irb.CreateSub(a, b) : irb.CreateAdd(a, b);
	storeOp(ai->operands[0], sat32(sum), irb);
	(void)minV;
	(void)maxV;
}

void Capstone2LlvmIrTranslatorArm_impl::translateDualMul(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool acc = false;
	bool sub = false;
	bool xchg = false;
	switch (i->id)
	{
	case ARM_INS_SMLAD: acc = true; break;
	case ARM_INS_SMLADX: acc = true; xchg = true; break;
	case ARM_INS_SMLSD: acc = true; sub = true; break;
	case ARM_INS_SMLSDX: acc = true; sub = true; xchg = true; break;
	case ARM_INS_SMUAD: break;
	case ARM_INS_SMUADX: xchg = true; break;
	case ARM_INS_SMUSD: sub = true; break;
	case ARM_INS_SMUSDX: sub = true; xchg = true; break;
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}

	if (acc)
	{
		EXPECT_IS_QUATERNARY(i, ai, irb);
	}
	else
	{
		EXPECT_IS_TERNARY(i, ai, irb);
	}

	auto* n = loadOp(ai->operands[1], irb);
	auto* m = loadOp(ai->operands[2], irb);
	auto* i32 = getDefaultType();
	auto* i16 = irb.getInt16Ty();
	auto half = [&](llvm::Value* v, bool top) {
		if (top)
		{
			v = irb.CreateLShr(v, llvm::ConstantInt::get(i32, 16));
		}
		return irb.CreateSExt(irb.CreateTrunc(v, i16), i32);
	};
	auto* nLo = half(n, false);
	auto* nHi = half(n, true);
	auto* mLo = half(m, xchg);
	auto* mHi = half(m, !xchg);
	llvm::Value* p0 = irb.CreateMul(nLo, mLo);
	llvm::Value* p1 = irb.CreateMul(nHi, mHi);
	llvm::Value* res = sub ? irb.CreateSub(p0, p1) : irb.CreateAdd(p0, p1);
	if (acc)
	{
		res = irb.CreateAdd(res, loadOp(ai->operands[3], irb));
	}
	storeOp(ai->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateUmaal(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_QUATERNARY(i, ai, irb);

	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* rdLo = irb.CreateZExt(irb.CreateZExtOrTrunc(loadOp(ai->operands[0], irb), i32), i64);
	auto* rdHi = irb.CreateZExt(irb.CreateZExtOrTrunc(loadOp(ai->operands[1], irb), i32), i64);
	auto* rn = irb.CreateZExt(irb.CreateZExtOrTrunc(loadOp(ai->operands[2], irb), i32), i64);
	auto* rm = irb.CreateZExt(irb.CreateZExtOrTrunc(loadOp(ai->operands[3], irb), i32), i64);
	auto* sum = irb.CreateAdd(irb.CreateAdd(irb.CreateMul(rn, rm), rdLo), rdHi);
	storeOp(ai->operands[0], irb.CreateTrunc(sum, i32), irb);
	storeOp(ai->operands[1], irb.CreateTrunc(irb.CreateLShr(sum, llvm::ConstantInt::get(i64, 32)), i32), irb);
	(void)i;
}

void Capstone2LlvmIrTranslatorArm_impl::translateUsad8(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool acc = i->id == ARM_INS_USADA8;
	if (acc)
	{
		EXPECT_IS_QUATERNARY(i, ai, irb);
	}
	else
	{
		EXPECT_IS_TERNARY(i, ai, irb);
	}

	auto* a = loadOp(ai->operands[1], irb);
	auto* b = loadOp(ai->operands[2], irb);
	auto* i32 = getDefaultType();
	auto* i8 = irb.getInt8Ty();
	llvm::Value* sum = llvm::ConstantInt::get(i32, 0);
	for (unsigned k = 0; k < 4; ++k)
	{
		auto* ea = irb.CreateZExt(irb.CreateTrunc(irb.CreateLShr(a, llvm::ConstantInt::get(i32, 8 * k)), i8), i32);
		auto* eb = irb.CreateZExt(irb.CreateTrunc(irb.CreateLShr(b, llvm::ConstantInt::get(i32, 8 * k)), i8), i32);
		auto* d = irb.CreateSelect(irb.CreateICmpUGT(ea, eb), irb.CreateSub(ea, eb), irb.CreateSub(eb, ea));
		sum = irb.CreateAdd(sum, d);
	}
	if (acc)
	{
		sum = irb.CreateAdd(sum, loadOp(ai->operands[3], irb));
	}
	storeOp(ai->operands[0], sum, irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateSat16(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, ai, irb, (ai->op_count >= 3));
	if (ai->operands[1].type != ARM_OP_IMM)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned sat = static_cast<unsigned>(ai->operands[1].imm);
	auto* src = loadOp(ai->operands[2], irb);
	auto* i32 = getDefaultType();
	auto* i16 = irb.getInt16Ty();
	auto satHalf = [&](llvm::Value* h) {
		auto* x = irb.CreateSExt(irb.CreateTrunc(h, i16), i32);
		if (i->id == ARM_INS_SSAT16)
		{
			if (sat == 0 || sat > 16)
			{
				return x;
			}
			auto* minV = llvm::ConstantInt::getSigned(i32, -(int64_t(1) << (sat - 1)));
			auto* maxV = llvm::ConstantInt::getSigned(i32, (int64_t(1) << (sat - 1)) - 1);
			return irb.CreateSelect(
				irb.CreateICmpSLT(x, minV), minV, irb.CreateSelect(irb.CreateICmpSGT(x, maxV), maxV, x));
		}
		auto* zero = llvm::ConstantInt::get(i32, 0);
		auto* maxV = llvm::ConstantInt::get(i32, sat == 0 ? 0 : ((uint64_t(1) << sat) - 1));
		auto* nn = irb.CreateSelect(irb.CreateICmpSLT(x, zero), zero, x);
		return irb.CreateSelect(irb.CreateICmpUGT(nn, maxV), maxV, nn);
	};
	auto* lo = irb.CreateAnd(satHalf(src), llvm::ConstantInt::get(i32, 0xffff));
	auto* hi = irb.CreateShl(
		irb.CreateAnd(satHalf(irb.CreateLShr(src, llvm::ConstantInt::get(i32, 16))), llvm::ConstantInt::get(i32, 0xffff)),
		llvm::ConstantInt::get(i32, 16));
	storeOp(ai->operands[0], irb.CreateOr(lo, hi), irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateSmmul(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool acc = false;
	bool sub = false;
	bool round = false;
	switch (i->id)
	{
	case ARM_INS_SMMUL: break;
	case ARM_INS_SMMULR: round = true; break;
	case ARM_INS_SMMLA: acc = true; break;
	case ARM_INS_SMMLAR: acc = true; round = true; break;
	case ARM_INS_SMMLS: acc = true; sub = true; break;
	case ARM_INS_SMMLSR: acc = true; sub = true; round = true; break;
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}

	if (acc)
	{
		EXPECT_IS_QUATERNARY(i, ai, irb);
	}
	else
	{
		EXPECT_IS_TERNARY(i, ai, irb);
	}

	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* a = irb.CreateSExt(irb.CreateZExtOrTrunc(loadOp(ai->operands[1], irb), i32), i64);
	auto* b = irb.CreateSExt(irb.CreateZExtOrTrunc(loadOp(ai->operands[2], irb), i32), i64);
	llvm::Value* prod = irb.CreateMul(a, b);
	if (round)
	{
		prod = irb.CreateAdd(prod, llvm::ConstantInt::get(i64, 0x80000000ULL));
	}
	llvm::Value* hi = irb.CreateTrunc(irb.CreateLShr(prod, llvm::ConstantInt::get(i64, 32)), i32);
	if (acc)
	{
		auto* ra = irb.CreateZExtOrTrunc(loadOp(ai->operands[3], irb), i32);
		hi = sub ? irb.CreateSub(ra, hi) : irb.CreateAdd(hi, ra);
	}
	storeOp(ai->operands[0], hi, irb);
}

} // namespace capstone2llvmir
} // namespace retdec
