/**
 * @file src/capstone2llvmir/arm/arm.cpp
 * @brief ARM implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <iomanip>

#include <llvm/IR/Intrinsics.h>

#include "capstone2llvmir/arm/arm_impl.h"

namespace retdec {
namespace capstone2llvmir {

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

	auto fIt = _i2fm.find(i->id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;

		bool branchInsn = i->id == ARM_INS_B || i->id == ARM_INS_BX
				|| i->id == ARM_INS_BL || i->id == ARM_INS_BLX
				|| i->id == ARM_INS_CBZ || i->id == ARM_INS_CBNZ;
		if (ai->cc == ARM_CC_AL || ai->cc == ARM_CC_INVALID || branchInsn)
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

		if (ai->cc == ARM_CC_AL || ai->cc == ARM_CC_INVALID)
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

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);

	return createLoad(irb, llvmReg);
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
		case ARM_SFT_RRX_REG:
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

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftAsr(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	auto* cfOp1 = irb.CreateSub(n, llvm::ConstantInt::get(n->getType(), 1));
	auto* cfShl = irb.CreateShl(llvm::ConstantInt::get(cfOp1->getType(), 1), cfOp1);
	auto* cfAnd = irb.CreateAnd(cfShl, val);
	auto* cfIcmp = irb.CreateICmpNE(cfAnd, llvm::ConstantInt::get(cfAnd->getType(), 0));
	storeRegister(ARM_REG_CPSR_C, cfIcmp, irb);

	return irb.CreateAShr(val, n);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftLsl(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	auto* cfOp1 = irb.CreateSub(n, llvm::ConstantInt::get(n->getType(), 1));
	auto* cfShl = irb.CreateShl(val, cfOp1);
	auto* cfIntT = llvm::cast<llvm::IntegerType>(cfShl->getType());
	auto* cfRightCount = llvm::ConstantInt::get(cfIntT, cfIntT->getBitWidth() - 1);
	auto* cfLow = irb.CreateLShr(cfShl, cfRightCount);
	storeRegister(ARM_REG_CPSR_C, cfLow, irb);

	return irb.CreateShl(val, n);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftLsr(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	auto* cfOp1 = irb.CreateSub(n, llvm::ConstantInt::get(n->getType(), 1));
	auto* cfShl = irb.CreateShl(llvm::ConstantInt::get(cfOp1->getType(), 1), cfOp1);
	auto* cfAnd = irb.CreateAnd(cfShl, val);
	auto* cfIcmp = irb.CreateICmpNE(cfAnd, llvm::ConstantInt::get(cfAnd->getType(), 0));
	storeRegister(ARM_REG_CPSR_C, cfIcmp, irb);

	return irb.CreateLShr(val, n);
}

llvm::Value* Capstone2LlvmIrTranslatorArm_impl::generateShiftRor(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* n)
{
	unsigned op0BitW = llvm::cast<llvm::IntegerType>(n->getType())->getBitWidth();

	auto* srl = irb.CreateLShr(val, n);
	auto* sub = irb.CreateSub(llvm::ConstantInt::get(n->getType(), op0BitW), n);
	auto* shl = irb.CreateShl(val, sub);
	auto* orr = irb.CreateOr(srl, shl);

	auto* cfSrl = irb.CreateLShr(orr, llvm::ConstantInt::get(orr->getType(), op0BitW - 1));
	auto* cfIcmp = irb.CreateICmpNE(cfSrl, llvm::ConstantInt::get(cfSrl->getType(), 0));
	storeRegister(ARM_REG_CPSR_C, cfIcmp, irb);

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
	for (unsigned j = 0; j < ai->op_count; ++j)
	{
		auto& op = ai->operands[j];
		if (op.type != ARM_OP_REG)
		{
			continue;
		}
		if (op.reg >= ARM_REG_Q0 && op.reg <= ARM_REG_Q15)
		{
			return false;
		}
		if (op.vector_index >= 0)
		{
			return false;
		}
	}
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

	if (!isScalarVfp(ai) || !isFpRegister(ai->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

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
}

/**
 * ARM_INS_VNEG, ARM_INS_VABS, ARM_INS_VSQRT
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpUnary(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (!isScalarVfp(ai) || !isFpRegister(ai->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

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

	if (!isScalarVfp(ai) || !isFpRegister(ai->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

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
		val = isSigned ? irb.CreateFPToSI(src, dstTy) : irb.CreateFPToUI(src, dstTy);
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
				if (op.mem.lshift > 0)
				{
					auto* lshift = llvm::ConstantInt::get(
							idxR->getType(),
							op.mem.lshift);
					idxR = irb.CreateShl(idxR, lshift);
				}

				// arm.h says this is only 1 || -1 -> ignore anything != -1.
				if (op.mem.scale == -1)
				{
					auto* scale = llvm::ConstantInt::getSigned(
							idxR->getType(),
							op.mem.scale);
					idxR = irb.CreateMul(idxR, scale);
				}

				// If there is a shift in memory operand, it is applied to
				// the index register.
				idxR = generateOperandShift(irb, op, idxR);
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
				if (op.mem.lshift >= 0)
				{
					auto* lshift = llvm::ConstantInt::get(
							idxR->getType(),
							op.mem.lshift);
					idxR = irb.CreateShl(idxR, lshift);
				}

				// arm.h says this is only 1 || -1 -> ignore anything != -1.
				if (op.mem.scale == -1)
				{
					auto* scale = llvm::ConstantInt::getSigned(
							idxR->getType(),
							op.mem.scale);
					idxR = irb.CreateMul(idxR, scale);
				}

				// If there is a shift in memory operand, it is applied to
				// the index register.
				idxR = generateOperandShift(irb, op, idxR);
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
		default:
		{
			throw GenericError("should not be possible");
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
 * TODO: Castone sets update_flags==true even when "adc", not "adcs".
 * Check once more and report as bug.
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
 * ARM_INS_B, ARM_INS_BX (exchange instruction)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateB(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	op0 = loadOpUnary(ai, irb);
	bool isReturn = ai->operands[0].type == ARM_OP_REG
			&& ai->operands[0].reg == ARM_REG_LR;

	if (ai->cc == ARM_CC_AL || ai->cc == ARM_CC_INVALID)
	{
		isReturn
			? generateReturnFunctionCall(irb, op0)
			: generateBranchFunctionCall(irb, op0);
	}
	else
	{
		auto* cond = generateInsnConditionCode(irb, ai);
		isReturn
			? generateCondReturnFunctionCall(irb, cond, op0)
			: generateCondBranchFunctionCall(irb, cond, op0);
	}
}

/**
 * ARM_INS_BL, ARM_INS_BLX (exchange instruction)
 */
void Capstone2LlvmIrTranslatorArm_impl::translateBl(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, ai, irb);

	storeRegister(ARM_REG_LR, getNextInsnAddress(i), irb);
	op0 = loadOpUnary(ai, irb);
	if (ai->cc == ARM_CC_AL || ai->cc == ARM_CC_INVALID)
	{
		generateCallFunctionCall(irb, op0);
	}
	else
	{
		auto* cond = generateInsnConditionCode(irb, ai);
		generateCondBranchFunctionCall(irb, cond, op0);
	}
}

/**
 * ARM_INS_CBNZ
 */
void Capstone2LlvmIrTranslatorArm_impl::translateCbnz(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	std::tie(op0, op1) = loadOpBinary(ai, irb, eOpConv::NOTHING);
	auto* cond = irb.CreateICmpNE(op0, llvm::ConstantInt::get(op0->getType(), 0));
	if (ai->cc != ARM_CC_AL && ai->cc != ARM_CC_INVALID)
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
	if (ai->cc != ARM_CC_AL && ai->cc != ARM_CC_INVALID)
	{
		cond = irb.CreateAnd(cond, generateInsnConditionCode(irb, ai));
	}
	generateCondBranchFunctionCall(irb, cond, op1);
}

/**
 * ARM_INS_CLZ
 */
void Capstone2LlvmIrTranslatorArm_impl::translateClz(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	op1 = loadOpBinaryOp1(ai, irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::ctlz,
			op1->getType());
	auto* ctlz = irb.CreateCall(f, {op1, irb.getTrue()});
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
	// care of shift/rotate computation.
	//
	if (ai->op_count == 2 && ai->operands[1].shift.type != ARM_SFT_INVALID)
	{
		op1 = loadOpBinaryOp1(ai, irb);
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

		switch (i->id)
		{
			case ARM_INS_ASR: op1 = generateShiftAsr(irb, op1, op2); break;
			case ARM_INS_LSL: op1 = generateShiftLsl(irb, op1, op2); break;
			case ARM_INS_LSR: op1 = generateShiftLsr(irb, op1, op2); break;
			case ARM_INS_ROR: op1 = generateShiftRor(irb, op1, op2); break;
			case ARM_INS_RRX: op1 = generateShiftRrx(irb, op1, op2); break;
			default:
			{
				throw GenericError("unhandled insn ID");
			}
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

	// TODO: It looks like on THUMB, result is overwritten -- investigate.
	// Add/Fix THUMB unit tests.
	if (_basicMode == CS_MODE_THUMB)
	{
		op1 = loadOpBinaryOp1(ai, irb);
		op1 = irb.CreateZExtOrTrunc(op1, irb.getInt32Ty());
		storeOp(ai->operands[0], op1, irb);
	}
	else
	{
		std::tie(op0, op1) = loadOpBinary(ai, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		op0 = irb.CreateAnd(op0, 0xffff0000);
		op0 = irb.CreateOr(op0, op1);
		storeOp(ai->operands[0], op0, irb);
	}
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

	bool load = i->id == ARM_INS_VPOP;
	auto* elem = vfpTypeOfReg(ai->operands[0].reg, irb);
	uint64_t sz = elem->isDoubleTy() ? 8 : 4;

	auto* sp = loadRegister(ARM_REG_SP, irb);
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

	unsigned opStart = 0;
	if (i->id == ARM_INS_POP || i->id == ARM_INS_PUSH)
	{
		op0 = loadRegister(ARM_REG_SP, irb);
		opStart = 0;
	}
	else
	{
		op0 = loadOp(ai->operands[0], irb);
		opStart = 1;
	}

	bool increment = i->id == ARM_INS_LDM || i->id == ARM_INS_LDMIB || i->id == ARM_INS_POP
			|| i->id == ARM_INS_STM || i->id == ARM_INS_STMIB;
	bool after = i->id == ARM_INS_LDM || i->id == ARM_INS_LDMDA || i->id == ARM_INS_POP
			|| i->id == ARM_INS_STM || i->id == ARM_INS_STMDA;
	bool before = !after;
	bool load = i->id == ARM_INS_LDM || i->id == ARM_INS_LDMIB || i->id == ARM_INS_LDMDA
			|| i->id == ARM_INS_LDMDB || i->id == ARM_INS_POP;

	llvm::Value* incDec = op0;
	llvm::Value* finalIncDec = op0;

	llvm::Value* pcStoreVal = nullptr;
	unsigned pcStoreNum = 0;

	for (unsigned j = opStart; j < ai->op_count; ++j)
	{
		uint64_t c = i->id == ARM_INS_PUSH || i->id == ARM_INS_STMDB
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

		if (i->id == ARM_INS_PUSH || i->id == ARM_INS_STMDB)
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

	if (i->id == ARM_INS_POP || i->id == ARM_INS_PUSH)
	{
		storeRegister(ARM_REG_SP, finalIncDec, irb);
	}
	else if (ai->writeback)
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
		}
		else if (ai->operands[1].mem.index != ARM_REG_INVALID)
		{
			idx = loadRegister(ai->operands[1].mem.index, irb);
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
	if (ai->writeback && idx && baseR != ARM_REG_INVALID)
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
		}
		else if (ai->operands[2].mem.index != ARM_REG_INVALID)
		{
			idx = loadRegister(ai->operands[2].mem.index, irb);
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
	if (ai->writeback && idx && baseR != ARM_REG_INVALID)
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

	if (!op0Pc && !op1Pc)
	{
		storeOp(ai->operands[0], hi, irb);
		storeOp(ai->operands[1], lo, irb);
	}
	else if (op0Pc && !op1Pc)
	{
		storeOp(ai->operands[1], lo, irb);
		storeOp(ai->operands[0], hi, irb);
	}
	else if (!op0Pc && op1Pc)
	{
		storeOp(ai->operands[0], hi, irb);
		storeOp(ai->operands[1], lo, irb);
	}
	else
	{
		// Store only one.
		storeOp(ai->operands[0], hi, irb);
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
			}
			else if (ai->operands[2].mem.index != ARM_REG_INVALID)
			{
				idx = loadRegister(ai->operands[2].mem.index, irb);
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
		}
		else if (ai->operands[1].mem.index != ARM_REG_INVALID)
		{
			idx = loadRegister(ai->operands[1].mem.index, irb);
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

	if (ai->writeback && idx && baseR != ARM_REG_INVALID)
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

	if (i->id == ARM_INS_RSB)
	{
		std::tie(op2, op1) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	}
	else
	{
		std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	}
	auto* sub = irb.CreateSub(op1, op2);
	if (ai->update_flags || i->id == ARM_INS_CMP)
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
 * ARM_INS_UXTAH
 */
void Capstone2LlvmIrTranslatorArm_impl::translateUxtah(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ai, irb);

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(ai, irb, eOpConv::THROW);
	op2 = irb.CreateZExtOrTrunc(op2, irb.getInt16Ty());
	op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
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

} // namespace capstone2llvmir
} // namespace retdec
