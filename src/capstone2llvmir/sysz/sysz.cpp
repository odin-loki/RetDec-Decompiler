/**
 * @file src/capstone2llvmir/sysz/sysz.cpp
 * @brief SystemZ implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/ADT/APInt.h>

#include "capstone2llvmir/sysz/sysz_impl.h"

namespace retdec {
namespace capstone2llvmir {

Capstone2LlvmIrTranslatorSysz_impl::Capstone2LlvmIrTranslatorSysz_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(
				CS_ARCH_SYSZ,
				basic,
				// basic is already CS_MODE_BIG_ENDIAN; adding it again
				// wraps the 1U<<31 flag to 0 (little-endian). Extra 0
				// keeps z/Architecture + all features (Capstone default).
				(extra == CS_MODE_BIG_ENDIAN)
						? CS_MODE_LITTLE_ENDIAN
						: extra,
				m)
{
	initialize();
}

//
//==============================================================================
// Mode query & modification methods - from Capstone2LlvmIrTranslator.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorSysz_impl::isAllowedBasicMode(cs_mode m)
{
	// Capstone 6 has no 31-bit / ESA-390 CS_MODE. Instruction bytes are
	// big-endian; CS_MODE_LITTLE_ENDIAN (0) is only the additive identity
	// for extra. Processor-generation bits live in extra.
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN;
}

bool Capstone2LlvmIrTranslatorSysz_impl::isAllowedExtraMode(cs_mode m)
{
	unsigned u = static_cast<unsigned>(m);
	unsigned arch = CS_MODE_SYSTEMZ_ARCH8 | CS_MODE_SYSTEMZ_ARCH9
			| CS_MODE_SYSTEMZ_ARCH10 | CS_MODE_SYSTEMZ_ARCH11
			| CS_MODE_SYSTEMZ_ARCH12 | CS_MODE_SYSTEMZ_ARCH13
			| CS_MODE_SYSTEMZ_ARCH14 | CS_MODE_SYSTEMZ_Z10
			| CS_MODE_SYSTEMZ_Z196 | CS_MODE_SYSTEMZ_ZEC12
			| CS_MODE_SYSTEMZ_Z13 | CS_MODE_SYSTEMZ_Z14
			| CS_MODE_SYSTEMZ_Z15 | CS_MODE_SYSTEMZ_Z16
			| CS_MODE_SYSTEMZ_GENERIC | CS_MODE_BIG_ENDIAN;
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN
			|| (u & ~arch) == 0;
}

uint32_t Capstone2LlvmIrTranslatorSysz_impl::getArchByteSize()
{
	return 8;
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorSysz_impl::generateEnvironmentArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorSysz_impl::generateDataLayout()
{
	_module->setDataLayout("E-p:64:64:64-i8:8:16-i16:16-i32:32-i64:64-n32:64");
}

void Capstone2LlvmIrTranslatorSysz_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		createRegister(p.first, _regLt);
	}

	// Capstone 6 reports 32-bit RR ops as R*L / R*H. Production is 64-bit
	// z/Architecture: those are overlays of R*D, not separate GPRs.
	auto* i64 = llvm::IntegerType::getInt64Ty(_module->getContext());
	for (uint32_t i = 0; i < 16; ++i)
	{
		auto* g = getRegister(SYSZ_REG_R0D + i);
		_capstone2LlvmRegs[SYSZ_REG_R0L + i] = g;
		_capstone2LlvmRegs[SYSZ_REG_R0H + i] = g;
		_reg2type[SYSZ_REG_R0L + i] = i64;
		_reg2type[SYSZ_REG_R0H + i] = i64;
	}
}

uint32_t Capstone2LlvmIrTranslatorSysz_impl::getCarryRegister()
{
	return SYSZ_REG_INVALID;
}

void Capstone2LlvmIrTranslatorSysz_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_sysz* si = &d->sysz;

	auto fIt = _i2fm.find(i->id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;
		(this->*f)(i, si, irb);
	}
	else
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, si, irb);
	}
}

//
//==============================================================================
// SystemZ-specific methods.
//==============================================================================
//

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == SYSZ_REG_INVALID)
	{
		return nullptr;
	}

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);
	return createLoad(irb, llvmReg);
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::generateMemAddress(
		cs_sysz_op& op,
		llvm::IRBuilder<>& irb)
{
	auto* t = getDefaultType();
	llvm::Value* addr = llvm::ConstantInt::getSigned(t, op.mem.disp);

	auto* baseR = loadAddrReg(op.mem.base, irb);
	if (baseR != nullptr)
	{
		addr = irb.CreateAdd(baseR, irb.CreateSExtOrTrunc(addr, baseR->getType()));
	}

	auto* idxR = loadAddrReg(op.mem.index, irb);
	if (idxR != nullptr)
	{
		addr = irb.CreateAdd(addr, irb.CreateSExtOrTrunc(idxR, addr->getType()));
	}

	return addr;
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadAddrReg(
		uint32_t r,
		llvm::IRBuilder<>& irb)
{
	// z/Architecture: GPR 0 as base/index contributes 0, not the register.
	if (r == SYSZ_REG_INVALID || r == SYSZ_REG_R0D
			|| r == SYSZ_REG_R0L || r == SYSZ_REG_R0H)
	{
		return nullptr;
	}
	return loadRegister(r, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadOp(
		cs_sysz_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case SYSZ_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			if (r == nullptr)
			{
				return llvm::UndefValue::get(ty ? ty : getDefaultType());
			}
			if (ty && r->getType() != ty)
			{
				eOpConv c = ty->isFloatingPointTy()
						? eOpConv::FPCAST_OR_BITCAST
						: eOpConv::SEXT_TRUNC_OR_BITCAST;
				if (!r->getType()->isIntegerTy() && !ty->isFloatingPointTy())
				{
					c = eOpConv::ZEXT_TRUNC_OR_BITCAST;
				}
				r = generateTypeConversion(irb, r, ty, c);
			}
			return r;
		}
		case SYSZ_OP_IMM:
		{
			auto* t = getDefaultType();
			return llvm::ConstantInt::get(t, llvm::APInt(t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm), false, /*implicitTrunc=*/true));
		}
		case SYSZ_OP_MEM:
		{
			auto* addr = generateMemAddress(op, irb);
			if (lea)
			{
				return addr;
			}
			auto* lty = ty ? ty : getDefaultType();
			return loadIntPtr(irb, addr, lty);
		}
		case SYSZ_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::StoreInst* Capstone2LlvmIrTranslatorSysz_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == SYSZ_REG_INVALID)
	{
		return nullptr;
	}

	auto* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("storeRegister() unhandled reg.");
	}
	if (ct == eOpConv::SEXT_TRUNC_OR_BITCAST && llvmReg->getValueType()->isFloatingPointTy())
	{
		ct = eOpConv::FPCAST_OR_BITCAST;
	}
	val = generateTypeConversion(irb, val, llvmReg->getValueType(), ct);

	auto* s = irb.CreateStore(val, llvmReg);
	attachPointeeType(s, llvmReg->getValueType());
	return s;
}

llvm::Instruction* Capstone2LlvmIrTranslatorSysz_impl::storeOp(
		cs_sysz_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case SYSZ_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case SYSZ_OP_MEM:
		{
			auto* addr = generateMemAddress(op, irb);
			if (ct == eOpConv::FPCAST_OR_BITCAST && val->getType()->isFloatingPointTy())
			{
				return storeIntPtr(irb, val, addr, val->getType());
			}
			val = generateTypeConversion(irb, val, val->getType(), ct);
			return storeIntPtr(irb, val, addr, val->getType());
		}
		case SYSZ_OP_IMM:
		case SYSZ_OP_INVALID:
		default:
		{
			throw GenericError("storeOp() unhandled operand type.");
		}
	}
}

bool Capstone2LlvmIrTranslatorSysz_impl::isOperandRegister(cs_sysz_op& op)
{
	return op.type == SYSZ_OP_REG;
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::extractLow32(
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	return irb.CreateTrunc(val, irb.getInt32Ty());
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::depositLow32(
		uint32_t r,
		llvm::Value* lo32,
		llvm::IRBuilder<>& irb)
{
	auto* dst = loadRegister(r, irb);
	if (dst == nullptr || dst->getType()->getIntegerBitWidth() <= 32)
	{
		storeRegister(r, lo32, irb);
		return lo32;
	}
	auto* hiMask = llvm::ConstantInt::get(dst->getType(), 0xFFFFFFFF00000000ull);
	auto* hi = irb.CreateAnd(dst, hiMask);
	auto* lo = irb.CreateZExt(lo32, dst->getType());
	auto* merged = irb.CreateOr(hi, lo);
	storeRegister(r, merged, irb);
	return merged;
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcSigned(
		llvm::Value* result,
		llvm::Value* overflow,
		llvm::IRBuilder<>& irb)
{
	auto* zero = llvm::ConstantInt::get(result->getType(), 0);
	auto* isZero = irb.CreateICmpEQ(result, zero);
	auto* isNeg = irb.CreateICmpSLT(result, zero);
	auto* ccLt = llvm::ConstantInt::get(irb.getInt8Ty(), 1);
	auto* ccGt = llvm::ConstantInt::get(irb.getInt8Ty(), 2);
	auto* ccEq = llvm::ConstantInt::get(irb.getInt8Ty(), 0);
	auto* ccOv = llvm::ConstantInt::get(irb.getInt8Ty(), 3);
	auto* signedCc = irb.CreateSelect(isZero, ccEq, irb.CreateSelect(isNeg, ccLt, ccGt));
	auto* cc = irb.CreateSelect(overflow, ccOv, signedCc);
	storeRegister(SYSZ_REG_CC, cc, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcLogical(
		llvm::Value* result,
		llvm::IRBuilder<>& irb)
{
	auto* zero = llvm::ConstantInt::get(result->getType(), 0);
	auto* isZero = irb.CreateICmpEQ(result, zero);
	auto* cc = irb.CreateSelect(
			isZero,
			llvm::ConstantInt::get(irb.getInt8Ty(), 0),
			llvm::ConstantInt::get(irb.getInt8Ty(), 1));
	storeRegister(SYSZ_REG_CC, cc, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::generateCondition(
		systemz_cc cc,
		llvm::IRBuilder<>& irb)
{
	auto* ccv = loadRegister(SYSZ_REG_CC, irb);
	ccv = irb.CreateZExtOrTrunc(ccv, irb.getInt32Ty());
	auto eq = [&](unsigned v) {
		return irb.CreateICmpEQ(ccv, irb.getInt32(v));
	};
	auto ne = [&](unsigned v) {
		return irb.CreateICmpNE(ccv, irb.getInt32(v));
	};

	switch (cc)
	{
		case SYSTEMZ_CC_INVALID:
			return irb.getTrue();
		case SYSTEMZ_CC_O:
			return eq(3);
		case SYSTEMZ_CC_H:
			return eq(2);
		case SYSTEMZ_CC_NLE:
			return irb.CreateOr(eq(2), eq(3));
		case SYSTEMZ_CC_L:
			return eq(1);
		case SYSTEMZ_CC_NHE:
			return irb.CreateOr(eq(1), eq(3));
		case SYSTEMZ_CC_LH:
			return irb.CreateOr(eq(1), eq(2));
		case SYSTEMZ_CC_NE:
			return ne(0);
		case SYSTEMZ_CC_E:
			return eq(0);
		case SYSTEMZ_CC_NLH:
			return irb.CreateOr(eq(0), eq(3));
		case SYSTEMZ_CC_HE:
			return irb.CreateOr(eq(0), eq(2));
		case SYSTEMZ_CC_NL:
			return ne(1);
		case SYSTEMZ_CC_LE:
			return irb.CreateOr(eq(0), eq(1));
		case SYSTEMZ_CC_NH:
			return ne(2);
		case SYSTEMZ_CC_NO:
			return ne(3);
		default:
			return irb.getTrue();
	}
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadBranchTarget(
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count == 0)
	{
		return llvm::UndefValue::get(getDefaultType());
	}
	unsigned idx = (si->op_count == 2) ? 1u : 0u;
	return loadOp(si->operands[idx], irb);
}

systemz_cc Capstone2LlvmIrTranslatorSysz_impl::conditionFromInsn(
		cs_insn* i,
		cs_sysz* si)
{
	if (si->cc != SYSTEMZ_CC_INVALID)
	{
		return si->cc;
	}

	switch (i->id)
	{
		case SYSZ_INS_JE:
		case SYSZ_INS_JZ:
		case SYSZ_INS_J_G_L_E:
		case SYSZ_INS_J_G_L_Z:
			return SYSTEMZ_CC_E;
		case SYSZ_INS_JH:
		case SYSZ_INS_JP:
		case SYSZ_INS_J_G_L_H:
		case SYSZ_INS_J_G_L_P:
			return SYSTEMZ_CC_H;
		case SYSZ_INS_JL:
		case SYSZ_INS_JM:
		case SYSZ_INS_J_G_L_L:
		case SYSZ_INS_J_G_L_M:
			return SYSTEMZ_CC_L;
		case SYSZ_INS_JO:
		case SYSZ_INS_J_G_L_O:
			return SYSTEMZ_CC_O;
		case SYSZ_INS_JNE:
		case SYSZ_INS_JNZ:
		case SYSZ_INS_J_G_L_NE:
		case SYSZ_INS_J_G_L_NZ:
			return SYSTEMZ_CC_NE;
		case SYSZ_INS_JHE:
		case SYSZ_INS_J_G_L_HE:
			return SYSTEMZ_CC_HE;
		case SYSZ_INS_JLE:
		case SYSZ_INS_J_G_L_LE:
			return SYSTEMZ_CC_LE;
		case SYSZ_INS_JLH:
		case SYSZ_INS_J_G_L_LH:
			return SYSTEMZ_CC_LH;
		case SYSZ_INS_JNL:
		case SYSZ_INS_JNM:
		case SYSZ_INS_J_G_L_NL:
		case SYSZ_INS_J_G_L_NM:
			return SYSTEMZ_CC_NL;
		case SYSZ_INS_JNH:
		case SYSZ_INS_JNP:
		case SYSZ_INS_J_G_L_NH:
		case SYSZ_INS_J_G_L_NP:
			return SYSTEMZ_CC_NH;
		case SYSZ_INS_JNLE:
		case SYSZ_INS_J_G_L_NLE:
			return SYSTEMZ_CC_NLE;
		case SYSZ_INS_JNHE:
		case SYSZ_INS_J_G_L_NHE:
			return SYSTEMZ_CC_NHE;
		case SYSZ_INS_JNLH:
		case SYSZ_INS_J_G_L_NLH:
			return SYSTEMZ_CC_NLH;
		case SYSZ_INS_JNO:
		case SYSZ_INS_J_G_L_NO:
			return SYSTEMZ_CC_NO;
		default:
			return SYSTEMZ_CC_INVALID;
	}
}

bool Capstone2LlvmIrTranslatorSysz_impl::isAlwaysCondition(systemz_cc cc, cs_insn* i)
{
	return cc == SYSTEMZ_CC_INVALID
			&& (i->id == SYSZ_INS_J
					|| i->id == SYSZ_INS_J_G_LU_
					|| i->id == SYSZ_INS_BR
					|| i->id == SYSZ_INS_BRC
					|| i->id == SYSZ_INS_BRCL
					|| i->id == SYSZ_INS_BCR);
}

//
//==============================================================================
// SystemZ instruction translation methods.
//==============================================================================
//

void Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb);
	depositLow32(si->operands[0].reg, extractLow32(op1, irb), irb);
	if (i->id == SYSZ_INS_LTR)
	{
		storeCcSigned(extractLow32(op1, irb), irb.getFalse(), irb);
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb);
	storeOp(si->operands[0], op1, irb);
	if (i->id == SYSZ_INS_LTGR)
	{
		storeCcSigned(op1, irb.getFalse(), irb);
	}
}

void Capstone2LlvmIrTranslatorSysz_impl::translateAdd32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = extractLow32(loadOp(si->operands[0], irb), irb);
	op1 = extractLow32(loadOp(si->operands[1], irb), irb);
	auto* add = irb.CreateAdd(op0, op1);
	depositLow32(si->operands[0].reg, add, irb);
	storeCcSigned(add, generateOverflowAdd(add, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateAdd64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	auto* add = irb.CreateAdd(op0, op1);
	storeOp(si->operands[0], add, irb);
	storeCcSigned(add, generateOverflowAdd(add, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateSub32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = extractLow32(loadOp(si->operands[0], irb), irb);
	op1 = extractLow32(loadOp(si->operands[1], irb), irb);
	auto* sub = irb.CreateSub(op0, op1);
	depositLow32(si->operands[0].reg, sub, irb);
	storeCcSigned(sub, generateOverflowSub(sub, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateSub64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	auto* sub = irb.CreateSub(op0, op1);
	storeOp(si->operands[0], sub, irb);
	storeCcSigned(sub, generateOverflowSub(sub, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLogical32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = extractLow32(loadOp(si->operands[0], irb), irb);
	op1 = extractLow32(loadOp(si->operands[1], irb), irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_NR:
			res = irb.CreateAnd(op0, op1);
			break;
		case SYSZ_INS_OR:
			res = irb.CreateOr(op0, op1);
			break;
		case SYSZ_INS_XR:
			res = irb.CreateXor(op0, op1);
			break;
		default:
			throw GenericError("Unhandled logical insn in translateLogical32().");
	}
	depositLow32(si->operands[0].reg, res, irb);
	storeCcLogical(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoad32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, irb.getInt32Ty());
	depositLow32(si->operands[0].reg, op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoad64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, irb.getInt64Ty());
	storeOp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateStore32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = extractLow32(loadOp(si->operands[0], irb), irb);
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, op0, addr, irb.getInt32Ty());
}

void Capstone2LlvmIrTranslatorSysz_impl::translateStore64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb);
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, op0, addr, irb.getInt64Ty());
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLa(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, nullptr, /*lea=*/true);
	// LA writes a 32-bit address into bits 32-63; bit 32 (the high bit of
	// that half) is forced to 0, matching z/Architecture LA.
	auto* addr32 = irb.CreateAnd(
			extractLow32(op1, irb),
			llvm::ConstantInt::get(irb.getInt32Ty(), 0x7FFFFFFF));
	depositLow32(si->operands[0].reg, addr32, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateBr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, si, irb);
	unsigned idx = (si->op_count == 2) ? 1u : 0u;
	auto& top = si->operands[idx];
	if (top.type == SYSZ_OP_REG && top.reg == SYSZ_REG_R0D)
	{
		return;
	}
	op0 = loadOp(top, irb);
	generateBranchFunctionCall(irb, op0);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateBrc(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, si, irb);
	auto cc = conditionFromInsn(i, si);
	op0 = loadBranchTarget(si, irb);
	if (isAlwaysCondition(cc, i))
	{
		generateBranchFunctionCall(irb, op0);
		return;
	}
	auto* cond = generateCondition(cc, irb);
	generateCondBranchFunctionCall(irb, cond, op0);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateBasr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	storeRegister(si->operands[0].reg, getNextInsnAddress(i), irb);
	auto& t = si->operands[1];
	if (t.type == SYSZ_OP_REG && (t.reg == SYSZ_REG_R0D || t.reg == SYSZ_REG_INVALID))
	{
		return;
	}
	op1 = loadOp(t, irb);
	generateCallFunctionCall(irb, op1);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateBrasl(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	storeRegister(si->operands[0].reg, getNextInsnAddress(i), irb);
	op1 = loadOp(si->operands[1], irb);
	generateCallFunctionCall(irb, op1);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcFp(
		llvm::Value* result,
		llvm::IRBuilder<>& irb)
{
	auto* zero = llvm::ConstantFP::get(result->getType(), 0.0);
	auto* isNan = irb.CreateFCmpUNO(result, result);
	auto* isZero = irb.CreateFCmpOEQ(result, zero);
	auto* isNeg = irb.CreateFCmpOLT(result, zero);
	auto* ccLt = llvm::ConstantInt::get(irb.getInt8Ty(), 1);
	auto* ccGt = llvm::ConstantInt::get(irb.getInt8Ty(), 2);
	auto* ccEq = llvm::ConstantInt::get(irb.getInt8Ty(), 0);
	auto* ccNan = llvm::ConstantInt::get(irb.getInt8Ty(), 3);
	auto* signedCc = irb.CreateSelect(isZero, ccEq, irb.CreateSelect(isNeg, ccLt, ccGt));
	storeRegister(SYSZ_REG_CC, irb.CreateSelect(isNan, ccNan, signedCc), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcFpCompare(
		llvm::Value* a,
		llvm::Value* b,
		llvm::IRBuilder<>& irb)
{
	auto* unord = irb.CreateFCmpUNO(a, b);
	auto* eq = irb.CreateFCmpOEQ(a, b);
	auto* lt = irb.CreateFCmpOLT(a, b);
	auto* ccLt = llvm::ConstantInt::get(irb.getInt8Ty(), 1);
	auto* ccGt = llvm::ConstantInt::get(irb.getInt8Ty(), 2);
	auto* ccEq = llvm::ConstantInt::get(irb.getInt8Ty(), 0);
	auto* ccUn = llvm::ConstantInt::get(irb.getInt8Ty(), 3);
	auto* ordered = irb.CreateSelect(eq, ccEq, irb.CreateSelect(lt, ccLt, ccGt));
	storeRegister(SYSZ_REG_CC, irb.CreateSelect(unord, ccUn, ordered), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeCcCompare(
		llvm::Value* a,
		llvm::Value* b,
		llvm::IRBuilder<>& irb)
{
	auto* eq = irb.CreateICmpEQ(a, b);
	auto* lt = irb.CreateICmpSLT(a, b);
	auto* cc = irb.CreateSelect(
			eq,
			llvm::ConstantInt::get(irb.getInt8Ty(), 0),
			irb.CreateSelect(
					lt,
					llvm::ConstantInt::get(irb.getInt8Ty(), 1),
					llvm::ConstantInt::get(irb.getInt8Ty(), 2)));
	storeRegister(SYSZ_REG_CC, cc, irb);
}

bool Capstone2LlvmIrTranslatorSysz_impl::isFpSingleInsn(unsigned id) const
{
	switch (id)
	{
		case SYSZ_INS_AEBR:
		case SYSZ_INS_AEB:
		case SYSZ_INS_SEBR:
		case SYSZ_INS_SEB:
		case SYSZ_INS_MEEBR:
		case SYSZ_INS_MEEB:
		case SYSZ_INS_DEBR:
		case SYSZ_INS_DEB:
		case SYSZ_INS_CEBR:
		case SYSZ_INS_CEB:
		case SYSZ_INS_LE:
		case SYSZ_INS_LEY:
		case SYSZ_INS_STE:
		case SYSZ_INS_STEY:
		case SYSZ_INS_LER:
			return true;
		default:
			return false;
	}
}

llvm::Type* Capstone2LlvmIrTranslatorSysz_impl::fpTypeForInsn(
		unsigned id,
		llvm::IRBuilder<>& irb) const
{
	return isFpSingleInsn(id) ? irb.getFloatTy() : irb.getDoubleTy();
}

llvm::Value* Capstone2LlvmIrTranslatorSysz_impl::loadFp(
		cs_sysz_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty)
{
	auto* v = loadOp(op, irb, ty);
	if (v->getType() == ty)
	{
		return v;
	}
	if (v->getType()->isFloatingPointTy())
	{
		return irb.CreateFPCast(v, ty);
	}
	return generateTypeConversion(irb, v, ty, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::storeFp(
		cs_sysz_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	storeOp(op, val, irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpArith(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op0 = loadFp(si->operands[0], irb, ty);
	op1 = loadFp(si->operands[1], irb, ty);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_AEBR:
		case SYSZ_INS_AEB:
		case SYSZ_INS_ADBR:
		case SYSZ_INS_ADB:
			res = irb.CreateFAdd(op0, op1);
			break;
		case SYSZ_INS_SEBR:
		case SYSZ_INS_SEB:
		case SYSZ_INS_SDBR:
		case SYSZ_INS_SDB:
			res = irb.CreateFSub(op0, op1);
			break;
		case SYSZ_INS_MEEBR:
		case SYSZ_INS_MEEB:
		case SYSZ_INS_MDBR:
		case SYSZ_INS_MDB:
			res = irb.CreateFMul(op0, op1);
			break;
		case SYSZ_INS_DEBR:
		case SYSZ_INS_DEB:
		case SYSZ_INS_DDBR:
		case SYSZ_INS_DDB:
			res = irb.CreateFDiv(op0, op1);
			break;
		default:
			throw GenericError("Unhandled FP arith insn.");
	}
	storeFp(si->operands[0], res, irb);
	storeCcFp(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpCompare(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op0 = loadFp(si->operands[0], irb, ty);
	op1 = loadFp(si->operands[1], irb, ty);
	storeCcFpCompare(op0, op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpLoad(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op1 = loadFp(si->operands[1], irb, ty);
	storeFp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpStore(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op0 = loadFp(si->operands[0], irb, ty);
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, op0, addr, ty);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateFpMove(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	auto* ty = fpTypeForInsn(i->id, irb);
	op1 = loadFp(si->operands[1], irb, ty);
	storeFp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLdeb(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadFp(si->operands[1], irb, irb.getFloatTy());
	storeFp(si->operands[0], irb.CreateFPExt(op1, irb.getDoubleTy()), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLedbr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadFp(si->operands[1], irb, irb.getDoubleTy());
	storeFp(si->operands[0], irb.CreateFPTrunc(op1, irb.getFloatTy()), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLogical64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_NGR:
			res = irb.CreateAnd(op0, op1);
			break;
		case SYSZ_INS_OGR:
			res = irb.CreateOr(op0, op1);
			break;
		case SYSZ_INS_XGR:
			res = irb.CreateXor(op0, op1);
			break;
		default:
			throw GenericError("Unhandled logical insn in translateLogical64().");
	}
	storeOp(si->operands[0], res, irb);
	storeCcLogical(res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateImm64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb);
	if (i->id == SYSZ_INS_LHI)
	{
		depositLow32(si->operands[0].reg, extractLow32(op1, irb), irb);
		return;
	}
	storeOp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateAddImm(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	if (i->id == SYSZ_INS_AHI)
	{
		op0 = extractLow32(loadOp(si->operands[0], irb), irb);
		op1 = extractLow32(loadOp(si->operands[1], irb), irb);
		auto* add = irb.CreateAdd(op0, op1);
		depositLow32(si->operands[0].reg, add, irb);
		storeCcSigned(add, generateOverflowAdd(add, op0, op1, irb), irb);
		return;
	}
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	auto* add = irb.CreateAdd(op0, op1);
	storeOp(si->operands[0], add, irb);
	storeCcSigned(add, generateOverflowAdd(add, op0, op1, irb), irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateCompare(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	bool is32 = (i->id == SYSZ_INS_CR || i->id == SYSZ_INS_CHI);
	op0 = loadOp(si->operands[0], irb);
	op1 = loadOp(si->operands[1], irb);
	if (is32)
	{
		op0 = extractLow32(op0, irb);
		op1 = extractLow32(op1, irb);
	}
	storeCcCompare(op0, op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateExtend32(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	bool isMem = (i->id == SYSZ_INS_LGF || i->id == SYSZ_INS_LLGF);
	bool isUnsigned = (i->id == SYSZ_INS_LLGFR || i->id == SYSZ_INS_LLGF);
	op1 = isMem
			? loadOp(si->operands[1], irb, irb.getInt32Ty())
			: extractLow32(loadOp(si->operands[1], irb), irb);
	auto* ext = isUnsigned
			? irb.CreateZExt(op1, irb.getInt64Ty())
			: irb.CreateSExt(op1, irb.getInt64Ty());
	storeOp(si->operands[0], ext, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateShift64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, si, irb);
	unsigned srcIdx = (si->op_count == 3) ? 1u : 0u;
	unsigned amtIdx = (si->op_count == 3) ? 2u : 1u;
	op0 = loadOp(si->operands[srcIdx], irb);
	auto* amt = loadOp(si->operands[amtIdx], irb, nullptr, /*lea=*/true);
	amt = irb.CreateZExtOrTrunc(amt, op0->getType());
	amt = irb.CreateAnd(amt, llvm::ConstantInt::get(op0->getType(), 63));
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case SYSZ_INS_SLLG:
			res = irb.CreateShl(op0, amt);
			break;
		case SYSZ_INS_SRLG:
			res = irb.CreateLShr(op0, amt);
			break;
		case SYSZ_INS_SRAG:
			res = irb.CreateAShr(op0, amt);
			break;
		default:
			throw GenericError("Unhandled shift insn in translateShift64().");
	}
	storeOp(si->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLay(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	bool lea = (si->operands[1].type == SYSZ_OP_MEM);
	op1 = loadOp(si->operands[1], irb, nullptr, lea);
	storeOp(si->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorLoad(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, irb.getInt128Ty());
	storeOp(si->operands[0], op1, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVectorStore(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op0 = loadOp(si->operands[0], irb, irb.getInt128Ty());
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, op0, addr, irb.getInt128Ty());
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVlr(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb, irb.getInt128Ty());
	storeOp(si->operands[0], op1, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVlrep(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, si, irb);
	unsigned bits = 64;
	switch (i->id)
	{
		case SYSZ_INS_VLREPB:
			bits = 8;
			break;
		case SYSZ_INS_VLREPH:
			bits = 16;
			break;
		case SYSZ_INS_VLREPF:
			bits = 32;
			break;
		case SYSZ_INS_VLREPG:
			bits = 64;
			break;
		case SYSZ_INS_VLREP:
			if (si->op_count >= 3 && si->operands[2].type == SYSZ_OP_IMM)
			{
				static const unsigned kM3Bits[4] = {8, 16, 32, 64};
				auto m = static_cast<unsigned>(si->operands[2].imm);
				if (m < 4)
				{
					bits = kM3Bits[m];
				}
			}
			break;
		default:
			break;
	}
	auto* elTy = irb.getIntNTy(bits);
	op1 = loadOp(si->operands[1], irb, elTy);
	auto* z = irb.CreateZExt(op1, irb.getInt128Ty());
	llvm::Value* r = z;
	for (unsigned sh = bits; sh < 128; sh += bits)
	{
		r = irb.CreateOr(r, irb.CreateShl(z, sh));
	}
	storeOp(si->operands[0], r, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVleg(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, si, irb);
	bool isF = (i->id == SYSZ_INS_VLEF);
	unsigned elemBits = isF ? 32u : 64u;
	unsigned idx = 0;
	if (si->op_count >= 3 && si->operands[2].type == SYSZ_OP_IMM)
	{
		idx = static_cast<unsigned>(si->operands[2].imm);
	}
	auto* vec = loadOp(si->operands[0], irb, irb.getInt128Ty());
	auto* el = loadOp(si->operands[1], irb, irb.getIntNTy(elemBits));
	el = irb.CreateZExt(el, irb.getInt128Ty());
	// Element 0 is the leftmost (high) bits of the 128-bit vector.
	unsigned nElem = 128 / elemBits;
	if (idx >= nElem)
	{
		idx = nElem - 1;
	}
	unsigned shift = (nElem - 1 - idx) * elemBits;
	llvm::APInt ones = llvm::APInt::getLowBitsSet(128, elemBits).shl(shift);
	auto* mask = llvm::ConstantInt::get(irb.getInt128Ty(), ones);
	auto* cleared = irb.CreateAnd(vec, irb.CreateNot(mask));
	auto* placed = irb.CreateShl(el, shift);
	storeOp(si->operands[0], irb.CreateOr(cleared, placed), irb,
			eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSysz_impl::translateVsteg(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, si, irb);
	bool isF = (i->id == SYSZ_INS_VSTEF);
	unsigned elemBits = isF ? 32u : 64u;
	unsigned idx = 0;
	if (si->op_count >= 3 && si->operands[2].type == SYSZ_OP_IMM)
	{
		idx = static_cast<unsigned>(si->operands[2].imm);
	}
	auto* vec = loadOp(si->operands[0], irb, irb.getInt128Ty());
	unsigned nElem = 128 / elemBits;
	if (idx >= nElem)
	{
		idx = nElem - 1;
	}
	unsigned shift = (nElem - 1 - idx) * elemBits;
	auto* el = irb.CreateTrunc(irb.CreateLShr(vec, shift), irb.getIntNTy(elemBits));
	auto* addr = generateMemAddress(si->operands[1], irb);
	storeIntPtr(irb, el, addr, irb.getIntNTy(elemBits));
}

} // namespace capstone2llvmir
} // namespace retdec
