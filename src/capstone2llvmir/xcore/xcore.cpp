/**
 * @file src/capstone2llvmir/xcore/xcore.cpp
 * @brief XCore implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <llvm/IR/Intrinsics.h>

#include "capstone2llvmir/xcore/xcore_impl.h"

namespace retdec {
namespace capstone2llvmir {

Capstone2LlvmIrTranslatorXcore_impl::Capstone2LlvmIrTranslatorXcore_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(CS_ARCH_XCORE, basic, extra, m)
{
	initialize();
}

//
//==============================================================================
// Mode query & modification methods - from Capstone2LlvmIrTranslator.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorXcore_impl::isAllowedBasicMode(cs_mode m)
{
	return m == CS_MODE_LITTLE_ENDIAN;
}

bool Capstone2LlvmIrTranslatorXcore_impl::isAllowedExtraMode(cs_mode m)
{
	// Capstone's official XCore tests open with CS_MODE_BIG_ENDIAN for the
	// 16-bit instruction word byte order. There is no 64-bit XCore mode.
	return m == CS_MODE_LITTLE_ENDIAN || m == CS_MODE_BIG_ENDIAN;
}

uint32_t Capstone2LlvmIrTranslatorXcore_impl::getArchByteSize()
{
	return 4;
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorXcore_impl::generateEnvironmentArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorXcore_impl::generateDataLayout()
{
	_module->setDataLayout("e-p:32:32:32-a:0:32-n32");
}

void Capstone2LlvmIrTranslatorXcore_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		createRegister(p.first, _regLt);
	}
}

uint32_t Capstone2LlvmIrTranslatorXcore_impl::getCarryRegister()
{
	return XCORE_REG_INVALID;
}

void Capstone2LlvmIrTranslatorXcore_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_xcore* xi = &d->xcore;

	auto fIt = _i2fm.find(i->id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;
		(this->*f)(i, xi, irb);
	}
	else
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, xi, irb);
	}
}

//
//==============================================================================
// XCore-specific methods.
//==============================================================================
//

llvm::Value* Capstone2LlvmIrTranslatorXcore_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == XCORE_REG_INVALID)
	{
		return nullptr;
	}

	if (r == XCORE_REG_PC)
	{
		return getThisInsnAddress(_insn);
	}

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);
	return createLoad(irb, llvmReg);
}

llvm::Value* Capstone2LlvmIrTranslatorXcore_impl::generateMemAddress(
		cs_xcore_op& op,
		llvm::IRBuilder<>& irb,
		unsigned scale)
{
	auto* t = getDefaultType();
	llvm::Value* base = loadRegister(op.mem.base, irb);
	if (base == nullptr)
	{
		base = llvm::ConstantInt::get(t, 0);
	}

	llvm::Value* index = loadRegister(op.mem.index, irb);
	llvm::Value* off = nullptr;
	if (index != nullptr)
	{
		off = index;
	}
	else
	{
		off = llvm::ConstantInt::getSigned(t, op.mem.disp);
	}

	int dir = op.mem.direct == 0 ? 1 : op.mem.direct;
	if (dir < 0)
	{
		off = irb.CreateNeg(off);
	}

	if (scale != 1)
	{
		off = irb.CreateMul(off, llvm::ConstantInt::get(off->getType(), scale));
	}

	return irb.CreateAdd(base, irb.CreateSExtOrTrunc(off, base->getType()));
}

llvm::Value* Capstone2LlvmIrTranslatorXcore_impl::loadOp(
		cs_xcore_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case XCORE_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case XCORE_OP_IMM:
		{
			auto* t = getDefaultType();
			return llvm::ConstantInt::get(t, llvm::APInt(t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm), false, /*implicitTrunc=*/true));
		}
		case XCORE_OP_MEM:
		{
			unsigned scale = 4;
			if (ty && ty->isIntegerTy(8))
			{
				scale = 1;
			}
			else if (ty && ty->isIntegerTy(16))
			{
				scale = 2;
			}
			auto* addr = generateMemAddress(op, irb, scale);
			if (lea)
			{
				return addr;
			}
			auto* lty = ty ? ty : getDefaultType();
			return loadIntPtr(irb, addr, lty);
		}
		case XCORE_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::StoreInst* Capstone2LlvmIrTranslatorXcore_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == XCORE_REG_INVALID || r == XCORE_REG_PC)
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

llvm::Instruction* Capstone2LlvmIrTranslatorXcore_impl::storeOp(
		cs_xcore_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case XCORE_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case XCORE_OP_MEM:
		{
			unsigned scale = 4;
			if (val->getType()->isIntegerTy(8))
			{
				scale = 1;
			}
			else if (val->getType()->isIntegerTy(16))
			{
				scale = 2;
			}
			auto* addr = generateMemAddress(op, irb, scale);
			val = generateTypeConversion(irb, val, val->getType(), ct);
			return storeIntPtr(irb, val, addr, val->getType());
		}
		case XCORE_OP_IMM:
		case XCORE_OP_INVALID:
		default:
		{
			throw GenericError("storeOp() unhandled operand type.");
		}
	}
}

bool Capstone2LlvmIrTranslatorXcore_impl::isOperandRegister(cs_xcore_op& op)
{
	return op.type == XCORE_OP_REG;
}

llvm::Value* Capstone2LlvmIrTranslatorXcore_impl::generateShift(
		llvm::Value* val,
		llvm::Value* amount,
		bool arithmetic,
		bool left,
		llvm::IRBuilder<>& irb)
{
	auto* width = llvm::ConstantInt::get(amount->getType(), 32);
	auto* tooBig = irb.CreateICmpUGE(amount, width);
	auto* masked = irb.CreateAnd(amount, llvm::ConstantInt::get(amount->getType(), 31));
	llvm::Value* shifted = nullptr;
	if (left)
	{
		shifted = irb.CreateShl(val, masked);
	}
	else if (arithmetic)
	{
		shifted = irb.CreateAShr(val, masked);
	}
	else
	{
		shifted = irb.CreateLShr(val, masked);
	}
	auto* zero = llvm::ConstantInt::get(val->getType(), 0);
	if (arithmetic && !left)
	{
		auto* allOnes = llvm::ConstantInt::getSigned(val->getType(), -1);
		auto* fill = irb.CreateSelect(irb.CreateICmpSLT(val, zero), allOnes, zero);
		return irb.CreateSelect(tooBig, fill, shifted);
	}
	return irb.CreateSelect(tooBig, zero, shifted);
}

// Capstone's XCore printer fills a trailing MEM operand but often leaves
// op_count one short (set_mem_access(false, 0) does not increment).
static unsigned xcoreOpCount(const cs_xcore* xi)
{
	unsigned n = xi->op_count;
	if (n < 8 && xi->operands[n].type == XCORE_OP_MEM)
	{
		return n + 1;
	}
	return n;
}

llvm::Value* Capstone2LlvmIrTranslatorXcore_impl::generateMakeMask(
		llvm::Value* bits,
		llvm::IRBuilder<>& irb)
{
	auto* ty = bits->getType();
	auto* thirtyTwo = llvm::ConstantInt::get(ty, 32);
	auto* tooBig = irb.CreateICmpUGE(bits, thirtyTwo);
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* one = llvm::ConstantInt::get(ty, 1);
	auto* allOnes = llvm::ConstantInt::getSigned(ty, -1);
	auto* safeAmt = irb.CreateSelect(tooBig, zero, bits);
	auto* mask = irb.CreateSub(irb.CreateShl(one, safeAmt), one);
	return irb.CreateSelect(tooBig, allOnes, mask);
}

llvm::Value* Capstone2LlvmIrTranslatorXcore_impl::generateCrc(
		llvm::Value* crc,
		llvm::Value* data,
		llvm::Value* poly,
		unsigned bitCount,
		llvm::IRBuilder<>& irb)
{
	auto* one = llvm::ConstantInt::get(crc->getType(), 1);
	auto* thirtyOne = llvm::ConstantInt::get(crc->getType(), 31);
	for (unsigned n = 0; n < bitCount; ++n)
	{
		auto* msb = irb.CreateTrunc(irb.CreateLShr(crc, thirtyOne), irb.getInt1Ty());
		auto* dataBit = irb.CreateTrunc(data, irb.getInt1Ty());
		auto* mix = irb.CreateXor(msb, dataBit);
		crc = irb.CreateShl(crc, one);
		data = irb.CreateLShr(data, one);
		crc = irb.CreateSelect(mix, irb.CreateXor(crc, poly), crc);
	}
	return crc;
}

void Capstone2LlvmIrTranslatorXcore_impl::translateDivRem(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb,
		bool isSigned,
		bool remainder)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(
			xi,
			irb,
			isSigned ? eOpConv::SEXT_TRUNC_OR_BITCAST : eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* ty = op1->getType();
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* one = llvm::ConstantInt::get(ty, 1);
	auto* divZero = irb.CreateICmpEQ(op2, zero);
	llvm::Value* val = nullptr;
	if (!isSigned)
	{
		auto* safe = irb.CreateSelect(divZero, one, op2);
		auto* q = irb.CreateUDiv(op1, safe);
		auto* r = irb.CreateURem(op1, safe);
		val = remainder ? r : q;
		val = irb.CreateSelect(divZero, zero, val);
	}
	else
	{
		unsigned bits = llvm::cast<llvm::IntegerType>(ty)->getBitWidth();
		auto* intMin = llvm::ConstantInt::get(ty, llvm::APInt::getSignedMinValue(bits));
		auto* minusOne = llvm::ConstantInt::getSigned(ty, -1);
		auto* overflow = irb.CreateAnd(
				irb.CreateICmpEQ(op1, intMin),
				irb.CreateICmpEQ(op2, minusOne));
		auto* safe = irb.CreateSelect(irb.CreateOr(divZero, overflow), one, op2);
		auto* q = irb.CreateSDiv(op1, safe);
		auto* r = irb.CreateSRem(op1, safe);
		val = remainder ? r : q;
		auto* ovVal = remainder ? zero : intMin;
		val = irb.CreateSelect(divZero, zero, irb.CreateSelect(overflow, ovVal, val));
	}
	storeOp(xi->operands[0], val, irb);
}

//
//==============================================================================
// XCore instruction translation methods.
//==============================================================================
//

void Capstone2LlvmIrTranslatorXcore_impl::translateAdd(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], irb.CreateAdd(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateSub(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], irb.CreateSub(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateAnd(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], irb.CreateAnd(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateAndnot(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op0 = loadOp(xi->operands[0], irb);
	op1 = loadOp(xi->operands[1], irb);
	storeOp(xi->operands[0], irb.CreateAnd(op0, irb.CreateNot(op1)), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateOr(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], irb.CreateOr(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateXor(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], irb.CreateXor(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateNot(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op1 = loadOp(xi->operands[1], irb);
	storeOp(xi->operands[0], irb.CreateNot(op1), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateNeg(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op1 = loadOp(xi->operands[1], irb);
	storeOp(xi->operands[0], irb.CreateNeg(op1), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateMul(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], irb.CreateMul(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateShl(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], generateShift(op1, op2, false, true, irb), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateShr(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], generateShift(op1, op2, false, false, irb), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateAshr(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeOp(xi->operands[0], generateShift(op1, op2, true, false, irb), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateEq(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* cmp = irb.CreateICmpEQ(op1, op2);
	storeOp(xi->operands[0], irb.CreateZExt(cmp, getDefaultType()), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLss(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* cmp = irb.CreateICmpSLT(op1, op2);
	storeOp(xi->operands[0], irb.CreateZExt(cmp, getDefaultType()), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLsu(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, xi, irb);
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* cmp = irb.CreateICmpULT(op1, op2);
	storeOp(xi->operands[0], irb.CreateZExt(cmp, getDefaultType()), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateClz(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op1 = loadOp(xi->operands[1], irb);
	auto* isZero = irb.CreateICmpEQ(op1, llvm::ConstantInt::get(op1->getType(), 0));
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::ctlz,
			op1->getType());
	auto* ctlz = irb.CreateCall(f, {op1, irb.getFalse()});
	auto* thirtyTwo = llvm::ConstantInt::get(op1->getType(), 32);
	storeOp(xi->operands[0], irb.CreateSelect(isZero, thirtyTwo, irb.CreateZExtOrTrunc(ctlz, op1->getType())), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLdc(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op1 = loadOp(xi->operands[1], irb);
	storeOp(xi->operands[0], op1, irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLdaw(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	unsigned n = xcoreOpCount(xi);
	EXPECT_IS_EXPR(i, xi, irb, n == 2 || n == 3);
	unsigned scale = (i->id == XCORE_INS_LDA16) ? 2 : 4;
	if (n == 2 && xi->operands[1].type == XCORE_OP_MEM)
	{
		op1 = generateMemAddress(xi->operands[1], irb, scale);
		storeOp(xi->operands[0], op1, irb);
		return;
	}
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* scaled = irb.CreateMul(op2, llvm::ConstantInt::get(op2->getType(), scale));
	storeOp(xi->operands[0], irb.CreateAdd(op1, scaled), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLoad(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	unsigned n = xcoreOpCount(xi);
	EXPECT_IS_EXPR(i, xi, irb, n == 2 || n == 3);

	llvm::Type* ty = irb.getInt32Ty();
	eOpConv ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
	unsigned scale = 4;
	switch (i->id)
	{
		case XCORE_INS_LD8U:
			ty = irb.getInt8Ty();
			scale = 1;
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		case XCORE_INS_LD16S:
			ty = irb.getInt16Ty();
			scale = 2;
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		default:
			break;
	}

	if (n == 2 && xi->operands[1].type == XCORE_OP_MEM)
	{
		op1 = loadOp(xi->operands[1], irb, ty);
		storeOp(xi->operands[0], op1, irb, ct);
		return;
	}

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* off = irb.CreateMul(op2, llvm::ConstantInt::get(op2->getType(), scale));
	auto* addr = irb.CreateAdd(op1, off);
	auto* val = loadIntPtr(irb, addr, ty);
	storeOp(xi->operands[0], val, irb, ct);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateStore(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	unsigned n = xcoreOpCount(xi);
	EXPECT_IS_EXPR(i, xi, irb, n == 2 || n == 3);

	llvm::Type* ty = irb.getInt32Ty();
	unsigned scale = 4;
	switch (i->id)
	{
		case XCORE_INS_ST8:
			ty = irb.getInt8Ty();
			scale = 1;
			break;
		case XCORE_INS_ST16:
			ty = irb.getInt16Ty();
			scale = 2;
			break;
		default:
			break;
	}

	op0 = loadOp(xi->operands[0], irb);
	op0 = irb.CreateTrunc(op0, ty);

	if (n == 2 && xi->operands[1].type == XCORE_OP_MEM)
	{
		auto* addr = generateMemAddress(xi->operands[1], irb, scale);
		storeIntPtr(irb, op0, addr, ty);
		return;
	}

	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* off = irb.CreateMul(op2, llvm::ConstantInt::get(op2->getType(), scale));
	auto* addr = irb.CreateAdd(op1, off);
	storeIntPtr(irb, op0, addr, ty);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBu(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	op0 = loadOp(xi->operands[0], irb);
	generateBranchFunctionCall(irb, op0);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBau(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	op0 = loadOp(xi->operands[0], irb);
	generateBranchFunctionCall(irb, op0);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBru(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	op0 = loadOp(xi->operands[0], irb);
	auto* pc = getThisInsnAddress(i);
	generateBranchFunctionCall(irb, irb.CreateAdd(pc, irb.CreateSExtOrTrunc(op0, pc->getType())));
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBf(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op0 = loadOp(xi->operands[0], irb);
	op1 = loadOp(xi->operands[1], irb);
	auto* cond = irb.CreateICmpEQ(op0, llvm::ConstantInt::get(op0->getType(), 0));
	generateCondBranchFunctionCall(irb, cond, op1);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBt(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op0 = loadOp(xi->operands[0], irb);
	op1 = loadOp(xi->operands[1], irb);
	auto* cond = irb.CreateICmpNE(op0, llvm::ConstantInt::get(op0->getType(), 0));
	generateCondBranchFunctionCall(irb, cond, op1);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBl(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	storeRegister(XCORE_REG_LR, getNextInsnAddress(i), irb);
	op0 = loadOp(xi->operands[0], irb);
	generateCallFunctionCall(irb, op0);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBla(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	storeRegister(XCORE_REG_LR, getNextInsnAddress(i), irb);
	op0 = loadOp(xi->operands[0], irb);
	generateCallFunctionCall(irb, op0);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateRetsp(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, xi, irb);
	auto* sp = loadRegister(XCORE_REG_SP, irb);
	llvm::Value* words = llvm::ConstantInt::get(sp->getType(), 0);
	if (xi->op_count == 1)
	{
		words = loadOp(xi->operands[0], irb);
		words = irb.CreateSExtOrTrunc(words, sp->getType());
	}
	auto* bytes = irb.CreateMul(words, llvm::ConstantInt::get(sp->getType(), 4));
	storeRegister(XCORE_REG_SP, irb.CreateAdd(sp, bytes), irb);
	auto* lr = loadRegister(XCORE_REG_LR, irb);
	generateReturnFunctionCall(irb, lr);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateEntsp(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	auto* sp = loadRegister(XCORE_REG_SP, irb);
	auto* words = irb.CreateSExtOrTrunc(loadOp(xi->operands[0], irb), sp->getType());
	auto* bytes = irb.CreateMul(words, llvm::ConstantInt::get(sp->getType(), 4));
	auto* nsp = irb.CreateSub(sp, bytes);
	storeRegister(XCORE_REG_SP, nsp, irb);
	auto* lr = loadRegister(XCORE_REG_LR, irb);
	storeIntPtr(irb, lr, nsp, getDefaultType());
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBitrev(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op1 = loadOp(xi->operands[1], irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::bitreverse,
			op1->getType());
	storeOp(xi->operands[0], irb.CreateCall(f, {op1}), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateByterev(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op1 = loadOp(xi->operands[1], irb);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(
			_module,
			llvm::Intrinsic::bswap,
			op1->getType());
	storeOp(xi->operands[0], irb.CreateCall(f, {op1}), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateMkmsk(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	op1 = loadOp(xi->operands[1], irb);
	storeOp(xi->operands[0], generateMakeMask(op1, irb), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateSext(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, xi->op_count == 2 || xi->op_count == 3);
	unsigned valIdx = (xi->op_count == 3) ? 1 : 0;
	op0 = loadOp(xi->operands[valIdx], irb);
	op1 = loadOp(xi->operands[xi->op_count - 1], irb);
	auto* ty = op0->getType();
	auto* thirtyTwo = llvm::ConstantInt::get(ty, 32);
	auto* tooBig = irb.CreateICmpUGE(op1, thirtyTwo);
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* shiftAmt = irb.CreateSelect(tooBig, zero, irb.CreateSub(thirtyTwo, op1));
	auto* extended = irb.CreateAShr(irb.CreateShl(op0, shiftAmt), shiftAmt);
	storeOp(xi->operands[0], irb.CreateSelect(tooBig, op0, extended), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateZext(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, xi->op_count == 2 || xi->op_count == 3);
	unsigned valIdx = (xi->op_count == 3) ? 1 : 0;
	op0 = loadOp(xi->operands[valIdx], irb);
	op1 = loadOp(xi->operands[xi->op_count - 1], irb);
	storeOp(xi->operands[0], irb.CreateAnd(op0, generateMakeMask(op1, irb)), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateDiv(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	translateDivRem(i, xi, irb, i->id == XCORE_INS_DIVS, /*remainder=*/false);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateRem(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	translateDivRem(i, xi, irb, i->id == XCORE_INS_REMS, /*remainder=*/true);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLdap(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, xi, irb);
	cs_xcore_op* dest = nullptr;
	cs_xcore_op* src = nullptr;
	if (xi->op_count == 1)
	{
		src = &xi->operands[0];
	}
	else
	{
		dest = &xi->operands[0];
		src = &xi->operands[1];
	}
	op1 = loadOp(*src, irb);
	if (dest)
	{
		storeOp(*dest, op1, irb);
	}
	else
	{
		storeRegister(XCORE_REG_R11, op1, irb);
	}
}

void Capstone2LlvmIrTranslatorXcore_impl::translateExtsp(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	auto* sp = loadRegister(XCORE_REG_SP, irb);
	auto* words = irb.CreateSExtOrTrunc(loadOp(xi->operands[0], irb), sp->getType());
	auto* bytes = irb.CreateMul(words, llvm::ConstantInt::get(sp->getType(), 4));
	storeRegister(XCORE_REG_SP, irb.CreateSub(sp, bytes), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateExtdp(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	auto* dp = loadRegister(XCORE_REG_DP, irb);
	auto* words = irb.CreateSExtOrTrunc(loadOp(xi->operands[0], irb), dp->getType());
	auto* bytes = irb.CreateMul(words, llvm::ConstantInt::get(dp->getType(), 4));
	storeRegister(XCORE_REG_DP, irb.CreateSub(dp, bytes), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLadd(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, xi->op_count >= 5);
	op0 = loadOp(xi->operands[2], irb);
	op1 = loadOp(xi->operands[3], irb);
	op2 = loadOp(xi->operands[4], irb);
	auto* i64 = irb.getInt64Ty();
	auto* x = irb.CreateZExt(op0, i64);
	auto* y = irb.CreateZExt(op1, i64);
	auto* cin = irb.CreateZExt(
			irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), 1)),
			i64);
	auto* sum = irb.CreateAdd(irb.CreateAdd(x, y), cin);
	auto* d = irb.CreateTrunc(sum, getDefaultType());
	auto* e = irb.CreateTrunc(irb.CreateLShr(sum, 32), getDefaultType());
	storeOp(xi->operands[0], d, irb);
	storeOp(xi->operands[1], e, irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLsub(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, xi->op_count >= 5);
	op0 = loadOp(xi->operands[2], irb);
	op1 = loadOp(xi->operands[3], irb);
	op2 = loadOp(xi->operands[4], irb);
	auto* i64 = irb.getInt64Ty();
	auto* x = irb.CreateZExt(op0, i64);
	auto* y = irb.CreateZExt(op1, i64);
	auto* bin = irb.CreateZExt(
			irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), 1)),
			i64);
	auto* diff = irb.CreateSub(irb.CreateSub(x, y), bin);
	auto* d = irb.CreateTrunc(diff, getDefaultType());
	auto* borrow = irb.CreateTrunc(irb.CreateLShr(diff, 63), getDefaultType());
	storeOp(xi->operands[0], d, irb);
	storeOp(xi->operands[1], borrow, irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLmul(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NARY(i, xi, irb, 6);
	auto* i64 = irb.getInt64Ty();
	auto* x = irb.CreateZExt(loadOp(xi->operands[2], irb), i64);
	auto* y = irb.CreateZExt(loadOp(xi->operands[3], irb), i64);
	auto* a = irb.CreateZExt(loadOp(xi->operands[4], irb), i64);
	auto* b = irb.CreateZExt(loadOp(xi->operands[5], irb), i64);
	auto* acc = irb.CreateAdd(irb.CreateAdd(irb.CreateMul(x, y), a), b);
	storeOp(xi->operands[0], irb.CreateTrunc(irb.CreateLShr(acc, 32), getDefaultType()), irb);
	storeOp(xi->operands[1], irb.CreateTrunc(acc, getDefaultType()), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLdivu(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, xi->op_count >= 5);
	auto* i64 = irb.getInt64Ty();
	auto* hi = irb.CreateZExt(loadOp(xi->operands[2], irb), i64);
	auto* lo = irb.CreateZExt(loadOp(xi->operands[3], irb), i64);
	auto* z = irb.CreateZExt(loadOp(xi->operands[4], irb), i64);
	auto* divd = irb.CreateOr(irb.CreateShl(hi, 32), lo);
	auto* zero = llvm::ConstantInt::get(i64, 0);
	auto* one = llvm::ConstantInt::get(i64, 1);
	auto* divZero = irb.CreateICmpEQ(z, zero);
	auto* safe = irb.CreateSelect(divZero, one, z);
	auto* q = irb.CreateSelect(divZero, zero, irb.CreateUDiv(divd, safe));
	auto* r = irb.CreateSelect(divZero, zero, irb.CreateURem(divd, safe));
	storeOp(xi->operands[0], irb.CreateTrunc(q, getDefaultType()), irb);
	storeOp(xi->operands[1], irb.CreateTrunc(r, getDefaultType()), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateMacc(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, xi->op_count == 4 || xi->op_count == 6);
	unsigned xIdx = xi->op_count - 2;
	unsigned yIdx = xi->op_count - 1;
	auto* i64 = irb.getInt64Ty();
	auto* d = loadOp(xi->operands[0], irb);
	auto* e = loadOp(xi->operands[1], irb);
	auto* x = loadOp(xi->operands[xIdx], irb);
	auto* y = loadOp(xi->operands[yIdx], irb);
	llvm::Value* accHi = nullptr;
	llvm::Value* accLo = irb.CreateZExt(e, i64);
	llvm::Value* prod = nullptr;
	if (i->id == XCORE_INS_MACCS)
	{
		accHi = irb.CreateShl(irb.CreateSExt(d, i64), 32);
		prod = irb.CreateMul(irb.CreateSExt(x, i64), irb.CreateSExt(y, i64));
	}
	else
	{
		accHi = irb.CreateShl(irb.CreateZExt(d, i64), 32);
		prod = irb.CreateMul(irb.CreateZExt(x, i64), irb.CreateZExt(y, i64));
	}
	auto* acc = irb.CreateAdd(irb.CreateOr(accHi, accLo), prod);
	storeOp(xi->operands[0], irb.CreateTrunc(irb.CreateLShr(acc, 32), getDefaultType()), irb);
	storeOp(xi->operands[1], irb.CreateTrunc(acc, getDefaultType()), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateCrc32(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, xi->op_count == 3 || xi->op_count == 4);
	unsigned crcIdx = (xi->op_count == 4) ? 1 : 0;
	op0 = loadOp(xi->operands[crcIdx], irb);
	op1 = loadOp(xi->operands[xi->op_count - 2], irb);
	op2 = loadOp(xi->operands[xi->op_count - 1], irb);
	storeOp(xi->operands[0], generateCrc(op0, op1, op2, 32, irb), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateCrc8(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_EXPR(i, xi, irb, xi->op_count == 4 || xi->op_count == 5);
	op0 = loadOp(xi->operands[0], irb);
	auto* data = loadOp(xi->operands[xi->op_count - 2], irb);
	auto* poly = loadOp(xi->operands[xi->op_count - 1], irb);
	storeOp(xi->operands[0], generateCrc(op0, data, poly, 8, irb), irb);
	storeOp(xi->operands[1], irb.CreateShl(data, llvm::ConstantInt::get(data->getType(), 8)), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateBlat(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	storeRegister(XCORE_REG_LR, getNextInsnAddress(i), irb);
	llvm::Value* target = nullptr;
	if (xi->operands[0].type == XCORE_OP_MEM)
	{
		target = loadOp(xi->operands[0], irb);
	}
	else
	{
		auto* cp = loadRegister(XCORE_REG_CP, irb);
		auto* idx = irb.CreateSExtOrTrunc(loadOp(xi->operands[0], irb), cp->getType());
		auto* addr = irb.CreateAdd(
				cp,
				irb.CreateMul(idx, llvm::ConstantInt::get(cp->getType(), 4)));
		target = loadIntPtr(irb, addr, getDefaultType());
	}
	generateCallFunctionCall(irb, target);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateDcall(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	storeRegister(XCORE_REG_SCP, getNextInsnAddress(i), irb);
	generateCallFunctionCall(irb, getNextInsnAddress(i));
}

void Capstone2LlvmIrTranslatorXcore_impl::translateDret(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	generateReturnFunctionCall(irb, loadRegister(XCORE_REG_SCP, irb));
}

void Capstone2LlvmIrTranslatorXcore_impl::translateKcall(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, xi, irb);
	storeRegister(XCORE_REG_LR, getNextInsnAddress(i), irb);
	if (xi->op_count == 1)
	{
		storeRegister(XCORE_REG_ED, loadOp(xi->operands[0], irb), irb);
	}
	generateCallFunctionCall(irb, loadRegister(XCORE_REG_KEP, irb));
}

void Capstone2LlvmIrTranslatorXcore_impl::translateKret(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	auto* ksp = loadRegister(XCORE_REG_KSP, irb);
	storeRegister(XCORE_REG_SP, ksp, irb);
	generateReturnFunctionCall(irb, loadRegister(XCORE_REG_KEP, irb));
}

void Capstone2LlvmIrTranslatorXcore_impl::translateEcall(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	op0 = loadOp(xi->operands[0], irb);
	auto* zero = llvm::ConstantInt::get(op0->getType(), 0);
	auto* cond = (i->id == XCORE_INS_ECALLF)
			? irb.CreateICmpEQ(op0, zero)
			: irb.CreateICmpNE(op0, zero);
	generateCondCallFunctionCall(irb, cond, loadRegister(XCORE_REG_KEP, irb));
}

void Capstone2LlvmIrTranslatorXcore_impl::translateDentsp(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	auto* sp = loadRegister(XCORE_REG_SP, irb);
	auto* nsp = irb.CreateSub(sp, llvm::ConstantInt::get(sp->getType(), 4));
	storeRegister(XCORE_REG_SP, nsp, irb);
	storeIntPtr(irb, loadRegister(XCORE_REG_SCP, irb), nsp, getDefaultType());
}

void Capstone2LlvmIrTranslatorXcore_impl::translateDrestsp(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	auto* sp = loadRegister(XCORE_REG_SP, irb);
	storeRegister(XCORE_REG_SP, irb.CreateAdd(sp, llvm::ConstantInt::get(sp->getType(), 4)), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateKentsp(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	auto* sp = loadRegister(XCORE_REG_SP, irb);
	storeRegister(XCORE_REG_KSP, sp, irb);
	auto* words = irb.CreateSExtOrTrunc(loadOp(xi->operands[0], irb), sp->getType());
	auto* bytes = irb.CreateMul(words, llvm::ConstantInt::get(sp->getType(), 4));
	storeRegister(XCORE_REG_SP, irb.CreateSub(sp, bytes), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateKrestsp(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_NULLARY_OR_UNARY(i, xi, irb);
	auto* ksp = loadRegister(XCORE_REG_KSP, irb);
	llvm::Value* words = llvm::ConstantInt::get(ksp->getType(), 0);
	if (xi->op_count == 1)
	{
		words = irb.CreateSExtOrTrunc(loadOp(xi->operands[0], irb), ksp->getType());
	}
	auto* bytes = irb.CreateMul(words, llvm::ConstantInt::get(ksp->getType(), 4));
	storeRegister(XCORE_REG_SP, irb.CreateAdd(ksp, bytes), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateGet(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, xi, irb);
	if (xi->operands[0].type != XCORE_OP_REG || xi->operands[1].type != XCORE_OP_REG)
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	storeOp(xi->operands[0], loadOp(xi->operands[1], irb), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateSet(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, xi, irb);
	if (xi->operands[1].type == XCORE_OP_MEM)
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	if (xi->op_count == 1)
	{
		uint32_t dest = XCORE_REG_INVALID;
		if (i->detail != nullptr && i->detail->regs_write_count >= 1)
		{
			dest = i->detail->regs_write[0];
		}
		// SETSP/SETDP/SETCP print the dest as a literal, not an operand.
		if (dest == XCORE_REG_INVALID && i->op_str[0] != '\0')
		{
			if (i->op_str[0] == 's' && i->op_str[1] == 'p')
			{
				dest = XCORE_REG_SP;
			}
			else if (i->op_str[0] == 'd' && i->op_str[1] == 'p')
			{
				dest = XCORE_REG_DP;
			}
			else if (i->op_str[0] == 'c' && i->op_str[1] == 'p')
			{
				dest = XCORE_REG_CP;
			}
		}
		if (xi->operands[0].type != XCORE_OP_REG || dest == XCORE_REG_INVALID)
		{
			throwUnhandledInstructions(i);
			translatePseudoAsmGeneric(i, xi, irb);
			return;
		}
		storeRegister(dest, loadOp(xi->operands[0], irb), irb);
		return;
	}
	if (xi->operands[0].type != XCORE_OP_REG || xi->operands[1].type != XCORE_OP_REG)
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, xi, irb);
		return;
	}
	storeOp(xi->operands[0], loadOp(xi->operands[1], irb), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateGetsr(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY_OR_BINARY(i, xi, irb);
	cs_xcore_op* dest = nullptr;
	cs_xcore_op* maskOp = nullptr;
	if (xi->op_count == 1)
	{
		maskOp = &xi->operands[0];
	}
	else
	{
		dest = &xi->operands[0];
		maskOp = &xi->operands[1];
	}
	auto* ssr = loadRegister(XCORE_REG_SSR, irb);
	auto* mask = irb.CreateSExtOrTrunc(loadOp(*maskOp, irb), ssr->getType());
	auto* bits = irb.CreateAnd(ssr, mask);
	auto* val = irb.CreateZExt(
			irb.CreateICmpNE(bits, llvm::ConstantInt::get(ssr->getType(), 0)),
			getDefaultType());
	if (dest)
	{
		storeOp(*dest, val, irb);
	}
	else
	{
		storeRegister(XCORE_REG_R11, val, irb);
	}
}

void Capstone2LlvmIrTranslatorXcore_impl::translateSetsr(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	auto* ssr = loadRegister(XCORE_REG_SSR, irb);
	auto* mask = irb.CreateSExtOrTrunc(loadOp(xi->operands[0], irb), ssr->getType());
	storeRegister(XCORE_REG_SSR, irb.CreateOr(ssr, mask), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateClrsr(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_UNARY(i, xi, irb);
	auto* ssr = loadRegister(XCORE_REG_SSR, irb);
	auto* mask = irb.CreateSExtOrTrunc(loadOp(xi->operands[0], irb), ssr->getType());
	storeRegister(XCORE_REG_SSR, irb.CreateAnd(ssr, irb.CreateNot(mask)), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateNop(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
}

} // namespace capstone2llvmir
} // namespace retdec
