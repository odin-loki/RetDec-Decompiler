/**
 * @file src/capstone2llvmir/sysz/sysz.cpp
 * @brief SystemZ implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "capstone2llvmir/sysz/sysz_impl.h"

namespace retdec {
namespace capstone2llvmir {

Capstone2LlvmIrTranslatorSysz_impl::Capstone2LlvmIrTranslatorSysz_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(CS_ARCH_SYSZ, basic, extra, m)
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
	// Capstone 5.0.9 has no 31-bit / ESA-390 mode. CS_MODE_LITTLE_ENDIAN is 0
	// (the default) and CS_MODE_BIG_ENDIAN is the documented SystemZ open mode.
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN;
}

bool Capstone2LlvmIrTranslatorSysz_impl::isAllowedExtraMode(cs_mode m)
{
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN;
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

	auto* baseR = loadRegister(op.mem.base, irb);
	if (baseR != nullptr)
	{
		addr = irb.CreateAdd(baseR, irb.CreateSExtOrTrunc(addr, baseR->getType()));
	}

	auto* idxR = loadRegister(op.mem.index, irb);
	if (idxR != nullptr)
	{
		addr = irb.CreateAdd(addr, irb.CreateSExtOrTrunc(idxR, addr->getType()));
	}

	return addr;
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
		case SYSZ_OP_ACREG:
		{
			auto* r = loadRegister(op.reg, irb);
			return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
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
		case SYSZ_OP_ACREG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case SYSZ_OP_MEM:
		{
			auto* addr = generateMemAddress(op, irb);
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
	return op.type == SYSZ_OP_REG || op.type == SYSZ_OP_ACREG;
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
		sysz_cc cc,
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
		case SYSZ_CC_INVALID:
			return irb.getTrue();
		case SYSZ_CC_O:
			return eq(3);
		case SYSZ_CC_H:
			return eq(2);
		case SYSZ_CC_NLE:
			return irb.CreateOr(eq(2), eq(3));
		case SYSZ_CC_L:
			return eq(1);
		case SYSZ_CC_NHE:
			return irb.CreateOr(eq(1), eq(3));
		case SYSZ_CC_LH:
			return irb.CreateOr(eq(1), eq(2));
		case SYSZ_CC_NE:
			return ne(0);
		case SYSZ_CC_E:
			return eq(0);
		case SYSZ_CC_NLH:
			return irb.CreateOr(eq(0), eq(3));
		case SYSZ_CC_HE:
			return irb.CreateOr(eq(0), eq(2));
		case SYSZ_CC_NL:
			return ne(1);
		case SYSZ_CC_LE:
			return irb.CreateOr(eq(0), eq(1));
		case SYSZ_CC_NH:
			return ne(2);
		case SYSZ_CC_NO:
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

sysz_cc Capstone2LlvmIrTranslatorSysz_impl::conditionFromInsn(
		cs_insn* i,
		cs_sysz* si)
{
	if (si->cc != SYSZ_CC_INVALID)
	{
		return si->cc;
	}

	switch (i->id)
	{
		case SYSZ_INS_JE:
		case SYSZ_INS_JZ:
		case SYSZ_INS_JGE:
		case SYSZ_INS_JGZ:
			return SYSZ_CC_E;
		case SYSZ_INS_JH:
		case SYSZ_INS_JP:
		case SYSZ_INS_JGH:
		case SYSZ_INS_JGP:
			return SYSZ_CC_H;
		case SYSZ_INS_JL:
		case SYSZ_INS_JM:
		case SYSZ_INS_JGL:
		case SYSZ_INS_JGM:
			return SYSZ_CC_L;
		case SYSZ_INS_JO:
		case SYSZ_INS_JGO:
			return SYSZ_CC_O;
		case SYSZ_INS_JNE:
		case SYSZ_INS_JNZ:
		case SYSZ_INS_JGNE:
		case SYSZ_INS_JGNZ:
			return SYSZ_CC_NE;
		case SYSZ_INS_JHE:
		case SYSZ_INS_JGHE:
			return SYSZ_CC_HE;
		case SYSZ_INS_JLE:
		case SYSZ_INS_JGLE:
			return SYSZ_CC_LE;
		case SYSZ_INS_JLH:
		case SYSZ_INS_JGLH:
			return SYSZ_CC_LH;
		case SYSZ_INS_JNL:
		case SYSZ_INS_JNM:
		case SYSZ_INS_JGNL:
		case SYSZ_INS_JGNM:
			return SYSZ_CC_NL;
		case SYSZ_INS_JNH:
		case SYSZ_INS_JNP:
		case SYSZ_INS_JGNH:
		case SYSZ_INS_JGNP:
			return SYSZ_CC_NH;
		case SYSZ_INS_JNLE:
		case SYSZ_INS_JGNLE:
			return SYSZ_CC_NLE;
		case SYSZ_INS_JNHE:
		case SYSZ_INS_JGNHE:
			return SYSZ_CC_NHE;
		case SYSZ_INS_JNLH:
		case SYSZ_INS_JGNLH:
			return SYSZ_CC_NLH;
		case SYSZ_INS_JNO:
		case SYSZ_INS_JGNO:
			return SYSZ_CC_NO;
		default:
			return SYSZ_CC_INVALID;
	}
}

bool Capstone2LlvmIrTranslatorSysz_impl::isAlwaysCondition(sysz_cc cc, cs_insn* i)
{
	return cc == SYSZ_CC_INVALID
			&& (i->id == SYSZ_INS_J
					|| i->id == SYSZ_INS_JG
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
}

void Capstone2LlvmIrTranslatorSysz_impl::translateLoadReg64(
		cs_insn* i,
		cs_sysz* si,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, si, irb);
	op1 = loadOp(si->operands[1], irb);
	storeOp(si->operands[0], op1, irb);
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
	if (top.type == SYSZ_OP_REG && top.reg == SYSZ_REG_0)
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
	if (t.type == SYSZ_OP_REG && (t.reg == SYSZ_REG_0 || t.reg == SYSZ_REG_INVALID))
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

} // namespace capstone2llvmir
} // namespace retdec
