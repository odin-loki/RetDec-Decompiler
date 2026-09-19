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
	EXPECT_IS_BINARY_OR_TERNARY(i, xi, irb);
	if (xi->op_count == 2 && xi->operands[1].type == XCORE_OP_MEM)
	{
		op1 = loadOp(xi->operands[1], irb, nullptr, /*lea=*/true);
		storeOp(xi->operands[0], op1, irb);
		return;
	}
	std::tie(op1, op2) = loadOpBinaryOrTernaryOp1Op2(xi, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* scaled = irb.CreateMul(op2, llvm::ConstantInt::get(op2->getType(), 4));
	storeOp(xi->operands[0], irb.CreateAdd(op1, scaled), irb);
}

void Capstone2LlvmIrTranslatorXcore_impl::translateLoad(
		cs_insn* i,
		cs_xcore* xi,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, xi, irb);

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

	if (xi->op_count == 2 && xi->operands[1].type == XCORE_OP_MEM)
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
	EXPECT_IS_BINARY_OR_TERNARY(i, xi, irb);

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

	if (xi->op_count == 2 && xi->operands[1].type == XCORE_OP_MEM)
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

} // namespace capstone2llvmir
} // namespace retdec
