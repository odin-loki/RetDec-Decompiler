/**
 * @file src/capstone2llvmir/riscv/riscv.cpp
 * @brief RISC-V implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "capstone2llvmir/riscv/riscv_impl.h"

namespace retdec {
namespace capstone2llvmir {

Capstone2LlvmIrTranslatorRiscv_impl::Capstone2LlvmIrTranslatorRiscv_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(CS_ARCH_RISCV, basic, extra, m)
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

bool Capstone2LlvmIrTranslatorRiscv_impl::isAllowedBasicMode(cs_mode m)
{
	return m == CS_MODE_RISCV32
			|| m == CS_MODE_RISCV64;
}

bool Capstone2LlvmIrTranslatorRiscv_impl::isAllowedExtraMode(cs_mode m)
{
	auto stripped = static_cast<cs_mode>(m & ~CS_MODE_RISCVC);
	return stripped == CS_MODE_LITTLE_ENDIAN
			|| stripped == CS_MODE_BIG_ENDIAN;
}

uint32_t Capstone2LlvmIrTranslatorRiscv_impl::getArchByteSize()
{
	return isXlen64() ? 8 : 4;
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorRiscv_impl::generateEnvironmentArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorRiscv_impl::generateDataLayout()
{
	if (isXlen64())
	{
		_module->setDataLayout("e-p:64:64:64-i8:8:32-i16:16:32-i64:64-n32:64-S128");
	}
	else
	{
		_module->setDataLayout("e-p:32:32:32-f80:32:32");
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		createRegister(p.first, _regLt);
	}
}

uint32_t Capstone2LlvmIrTranslatorRiscv_impl::getCarryRegister()
{
	return RISCV_REG_INVALID;
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_riscv* ri = &d->riscv;

	auto fIt = _i2fm.find(i->id);
	if (fIt != _i2fm.end() && fIt->second != nullptr)
	{
		auto f = fIt->second;
		(this->*f)(i, ri, irb);
	}
	else
	{
		throwUnhandledInstructions(i);
		translatePseudoAsmGeneric(i, ri, irb);
	}
}

//
//==============================================================================
// RISC-V-specific methods.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorRiscv_impl::isXlen64() const
{
	return _origBasicMode == CS_MODE_RISCV64;
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::getCurrentPc(cs_insn* i)
{
	return llvm::ConstantInt::get(getDefaultType(), i->address);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::pcRelativeTarget(
		cs_insn* i,
		llvm::Value* offset,
		llvm::IRBuilder<>& irb)
{
	auto* pc = getCurrentPc(i);
	offset = generateTypeConversion(irb, offset, pc->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	return irb.CreateAdd(pc, offset);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::maskShiftAmount(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* amount)
{
	unsigned mask = val->getType()->getIntegerBitWidth() - 1;
	amount = irb.CreateZExtOrTrunc(amount, val->getType());
	return irb.CreateAnd(amount, llvm::ConstantInt::get(val->getType(), mask));
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::narrowToWord(
		llvm::IRBuilder<>& irb,
		llvm::Value* val)
{
	return irb.CreateTrunc(val, irb.getInt32Ty());
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::widenFromWord(
		llvm::IRBuilder<>& irb,
		llvm::Value* val)
{
	return irb.CreateSExt(val, getDefaultType());
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == RISCV_REG_INVALID)
	{
		return nullptr;
	}

	if (r == RISCV_REG_PC)
	{
		auto* pc = getCurrentPc(_insn);
		return generateTypeConversion(irb, pc, dstType, ct);
	}

	if (r == RISCV_REG_X0)
	{
		auto* z = llvm::ConstantInt::get(getDefaultType(), 0);
		return generateTypeConversion(irb, z, dstType, ct);
	}

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);
	return createLoad(irb, llvmReg);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::loadOp(
		cs_riscv_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case RISCV_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case RISCV_OP_IMM:
		{
			auto* t = getDefaultType();
			return llvm::ConstantInt::getSigned(t, op.imm);
		}
		case RISCV_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
			auto* t = getDefaultType();
			llvm::Value* disp = llvm::ConstantInt::getSigned(t, op.mem.disp);

			llvm::Value* addr = nullptr;
			if (baseR == nullptr)
			{
				addr = disp;
			}
			else if (op.mem.disp == 0)
			{
				addr = baseR;
			}
			else
			{
				disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
				addr = irb.CreateAdd(baseR, disp);
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
		case RISCV_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::Instruction* Capstone2LlvmIrTranslatorRiscv_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == RISCV_REG_INVALID
			|| r == RISCV_REG_PC
			|| r == RISCV_REG_X0)
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

llvm::Instruction* Capstone2LlvmIrTranslatorRiscv_impl::storeOp(
		cs_riscv_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case RISCV_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case RISCV_OP_MEM:
		{
			auto* baseR = loadRegister(op.mem.base, irb);
			auto* t = getDefaultType();
			llvm::Value* disp = llvm::ConstantInt::getSigned(t, op.mem.disp);

			llvm::Value* addr = nullptr;
			if (baseR == nullptr)
			{
				addr = disp;
			}
			else if (op.mem.disp == 0)
			{
				addr = baseR;
			}
			else
			{
				disp = irb.CreateSExtOrTrunc(disp, baseR->getType());
				addr = irb.CreateAdd(baseR, disp);
			}

			return storeIntPtr(irb, val, addr, val->getType());
		}
		case RISCV_OP_IMM:
		case RISCV_OP_INVALID:
		default:
		{
			throw GenericError("should not be possible");
		}
	}
}

bool Capstone2LlvmIrTranslatorRiscv_impl::isOperandRegister(cs_riscv_op& op)
{
	return op.type == RISCV_OP_REG;
}

//
//==============================================================================
// RISC-V instruction translation methods.
//==============================================================================
//

/**
 * RISCV_INS_LUI, RISCV_INS_C_LUI
 * rd = sext(imm[31:12] << 12)
 * Capstone reports the 20-bit U-immediate, not the already-shifted value.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateLui(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	op1 = loadOp(ri->operands[1], irb);
	auto* i32 = irb.getInt32Ty();
	auto* u = irb.CreateShl(irb.CreateZExtOrTrunc(op1, i32), llvm::ConstantInt::get(i32, 12));
	storeOp(ri->operands[0], u, irb);
}

/**
 * RISCV_INS_AUIPC
 * rd = pc + sext(imm[31:12] << 12)
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateAuipc(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	op1 = loadOp(ri->operands[1], irb);
	auto* i32 = irb.getInt32Ty();
	auto* u = irb.CreateShl(irb.CreateZExtOrTrunc(op1, i32), llvm::ConstantInt::get(i32, 12));
	u = generateTypeConversion(irb, u, getDefaultType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* add = irb.CreateAdd(getCurrentPc(i), u);
	storeOp(ri->operands[0], add, irb);
}

/**
 * RISCV_INS_JAL, RISCV_INS_C_J, RISCV_INS_C_JAL
 * rd = pc+size; pc += offset. rd is x0 / omitted for C.J.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateJal(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	llvm::Value* offset = nullptr;
	uint32_t rd = RISCV_REG_X0;

	if (i->id == RISCV_INS_C_J)
	{
		EXPECT_IS_UNARY(i, ri, irb);
		offset = loadOp(ri->operands[0], irb);
	}
	else if (i->id == RISCV_INS_C_JAL)
	{
		EXPECT_IS_UNARY(i, ri, irb);
		offset = loadOp(ri->operands[0], irb);
		rd = RISCV_REG_RA;
	}
	else
	{
		EXPECT_IS_BINARY(i, ri, irb);
		if (ri->operands[0].type == RISCV_OP_REG)
		{
			rd = ri->operands[0].reg;
		}
		offset = loadOp(ri->operands[1], irb);
	}

	auto* target = pcRelativeTarget(i, offset, irb);
	if (rd != RISCV_REG_X0)
	{
		storeRegister(rd, getNextInsnAddress(i), irb);
		generateCallFunctionCall(irb, target);
	}
	else
	{
		generateBranchFunctionCall(irb, target);
	}
}

/**
 * RISCV_INS_JALR, RISCV_INS_C_JR, RISCV_INS_C_JALR
 * rd = pc+size; pc = (rs1 + imm) & ~1.
 * ret: rd=x0, rs1=ra.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateJalr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	llvm::Value* target = nullptr;
	uint32_t rd = RISCV_REG_X0;
	uint32_t rs1 = RISCV_REG_INVALID;

	if (i->id == RISCV_INS_C_JR || i->id == RISCV_INS_C_JALR)
	{
		EXPECT_IS_UNARY(i, ri, irb);
		target = loadOp(ri->operands[0], irb);
		if (ri->operands[0].type == RISCV_OP_REG)
		{
			rs1 = ri->operands[0].reg;
		}
		if (i->id == RISCV_INS_C_JALR)
		{
			rd = RISCV_REG_RA;
		}
	}
	else if (ri->op_count == 2 && ri->operands[1].type == RISCV_OP_MEM)
	{
		if (ri->operands[0].type == RISCV_OP_REG)
		{
			rd = ri->operands[0].reg;
		}
		rs1 = ri->operands[1].mem.base;
		target = loadOp(ri->operands[1], irb, nullptr, /*lea=*/true);
	}
	else
	{
		EXPECT_IS_TERNARY(i, ri, irb);
		if (ri->operands[0].type == RISCV_OP_REG)
		{
			rd = ri->operands[0].reg;
		}
		if (ri->operands[1].type == RISCV_OP_REG)
		{
			rs1 = ri->operands[1].reg;
		}
		op1 = loadOp(ri->operands[1], irb);
		op2 = loadOp(ri->operands[2], irb);
		op2 = generateTypeConversion(irb, op2, op1->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
		target = irb.CreateAdd(op1, op2);
	}

	auto* one = llvm::ConstantInt::get(target->getType(), 1);
	target = irb.CreateAnd(target, irb.CreateNot(one));

	if (rd != RISCV_REG_X0)
	{
		storeRegister(rd, getNextInsnAddress(i), irb);
		generateCallFunctionCall(irb, target);
	}
	else if (rs1 == RISCV_REG_RA)
	{
		generateReturnFunctionCall(irb, target);
	}
	else
	{
		generateBranchFunctionCall(irb, target);
	}
}

/**
 * BEQ/BNE/BLT/BGE/BLTU/BGEU, C_BEQZ, C_BNEZ.
 * Capstone reports the PC-relative byte offset, not an absolute address.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateBranch(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	llvm::Value* cond = nullptr;
	llvm::Value* offset = nullptr;

	if (i->id == RISCV_INS_C_BEQZ || i->id == RISCV_INS_C_BNEZ)
	{
		EXPECT_IS_BINARY(i, ri, irb);
		op0 = loadOp(ri->operands[0], irb);
		offset = loadOp(ri->operands[1], irb);
		auto* zero = llvm::ConstantInt::get(op0->getType(), 0);
		cond = (i->id == RISCV_INS_C_BEQZ)
				? irb.CreateICmpEQ(op0, zero)
				: irb.CreateICmpNE(op0, zero);
	}
	else
	{
		EXPECT_IS_TERNARY(i, ri, irb);
		op0 = loadOp(ri->operands[0], irb);
		op1 = loadOp(ri->operands[1], irb);
		op1 = generateTypeConversion(irb, op1, op0->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
		offset = loadOp(ri->operands[2], irb);

		switch (i->id)
		{
			case RISCV_INS_BEQ: cond = irb.CreateICmpEQ(op0, op1); break;
			case RISCV_INS_BNE: cond = irb.CreateICmpNE(op0, op1); break;
			case RISCV_INS_BLT: cond = irb.CreateICmpSLT(op0, op1); break;
			case RISCV_INS_BGE: cond = irb.CreateICmpSGE(op0, op1); break;
			case RISCV_INS_BLTU: cond = irb.CreateICmpULT(op0, op1); break;
			case RISCV_INS_BGEU: cond = irb.CreateICmpUGE(op0, op1); break;
			default:
				throw GenericError("Unhandled insn ID in translateBranch().");
		}
	}

	generateCondBranchFunctionCall(irb, cond, pcRelativeTarget(i, offset, irb));
}

/**
 * LB/LBU/LH/LHU/LW/LWU/LD, C_LW/C_LWSP/C_LD/C_LDSP
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateLoad(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	llvm::Type* ty = nullptr;
	eOpConv ct = eOpConv::THROW;

	switch (i->id)
	{
		case RISCV_INS_LB:
			ty = irb.getInt8Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LBU:
			ty = irb.getInt8Ty();
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LH:
			ty = irb.getInt16Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LHU:
			ty = irb.getInt16Ty();
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LW:
		case RISCV_INS_C_LW:
		case RISCV_INS_C_LWSP:
			ty = irb.getInt32Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LWU:
			ty = irb.getInt32Ty();
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LD:
		case RISCV_INS_C_LD:
		case RISCV_INS_C_LDSP:
			ty = irb.getInt64Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateLoad().");
	}

	op1 = loadOp(ri->operands[1], irb, ty);
	storeOp(ri->operands[0], op1, irb, ct);
}

/**
 * SB/SH/SW/SD, C_SW/C_SWSP/C_SD/C_SDSP
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateStore(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case RISCV_INS_SB: ty = irb.getInt8Ty(); break;
		case RISCV_INS_SH: ty = irb.getInt16Ty(); break;
		case RISCV_INS_SW:
		case RISCV_INS_C_SW:
		case RISCV_INS_C_SWSP:
			ty = irb.getInt32Ty();
			break;
		case RISCV_INS_SD:
		case RISCV_INS_C_SD:
		case RISCV_INS_C_SDSP:
			ty = irb.getInt64Ty();
			break;
		default:
			throw GenericError("Unhandled insn ID in translateStore().");
	}

	op0 = loadOp(ri->operands[0], irb);
	op0 = irb.CreateZExtOrTrunc(op0, ty);
	storeOp(ri->operands[1], op0, irb);
}

std::pair<llvm::Value*, llvm::Value*> Capstone2LlvmIrTranslatorRiscv_impl::loadAluSrc(
		cs_riscv* ri,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (ri->op_count == 2)
	{
		// Compressed: rd is also the first source.
		auto* a = loadOp(ri->operands[0], irb);
		auto* b = loadOp(ri->operands[1], irb);
		b = generateTypeConversion(irb, b, a->getType(), ct);
		return {a, b};
	}

	auto* a = loadOp(ri->operands[1], irb);
	auto* b = loadOp(ri->operands[2], irb);
	b = generateTypeConversion(irb, b, a->getType(), ct);
	return {a, b};
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateAdd(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateAdd(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSub(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateSub(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateAnd(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateAnd(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateOr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateOr(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateXor(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	storeOp(ri->operands[0], irb.CreateXor(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSlt(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* slt = irb.CreateZExt(irb.CreateICmpSLT(op1, op2), getDefaultType());
	storeOp(ri->operands[0], slt, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSltu(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ult = irb.CreateZExt(irb.CreateICmpULT(op1, op2), getDefaultType());
	storeOp(ri->operands[0], ult, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSll(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = maskShiftAmount(irb, op1, op2);
	storeOp(ri->operands[0], irb.CreateShl(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSrl(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = maskShiftAmount(irb, op1, op2);
	storeOp(ri->operands[0], irb.CreateLShr(op1, op2), irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateSra(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = maskShiftAmount(irb, op1, op2);
	storeOp(ri->operands[0], irb.CreateAShr(op1, op2), irb);
}

/**
 * RV64I OP-32 / OP-IMM-32: ADDW, SUBW, SLLW, SRLW, SRAW,
 * ADDIW, SLLIW, SRLIW, SRAIW, C_ADDIW, C_ADDW, C_SUBW.
 * Compute in i32, sign-extend to XLEN.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateOp32(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	op1 = narrowToWord(irb, op1);
	op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());

	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_ADDW:
		case RISCV_INS_ADDIW:
		case RISCV_INS_C_ADDIW:
		case RISCV_INS_C_ADDW:
			res = irb.CreateAdd(op1, op2);
			break;
		case RISCV_INS_SUBW:
		case RISCV_INS_C_SUBW:
			res = irb.CreateSub(op1, op2);
			break;
		case RISCV_INS_SLLW:
		case RISCV_INS_SLLIW:
			op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), 31));
			res = irb.CreateShl(op1, op2);
			break;
		case RISCV_INS_SRLW:
		case RISCV_INS_SRLIW:
			op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), 31));
			res = irb.CreateLShr(op1, op2);
			break;
		case RISCV_INS_SRAW:
		case RISCV_INS_SRAIW:
			op2 = irb.CreateAnd(op2, llvm::ConstantInt::get(op2->getType(), 31));
			res = irb.CreateAShr(op1, op2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateOp32().");
	}

	storeOp(ri->operands[0], widenFromWord(irb, res), irb);
}

/**
 * RISCV_INS_C_LI: rd = imm
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateLi(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	storeOp(ri->operands[0], op1, irb);
}

/**
 * RISCV_INS_C_MV: rd = rs2
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateMv(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	storeOp(ri->operands[0], op1, irb);
}

/**
 * RISCV_INS_ECALL, RISCV_INS_C_EBREAK is not this.
 * Environment call → translator pseudo-call (continuation at pc+size).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateEcall(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	(void)ri;
	generateCallFunctionCall(irb, getNextInsnAddress(i));
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateNop(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	(void)i;
	(void)ri;
	(void)irb;
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFence(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	(void)i;
	(void)ri;
	irb.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
}

} // namespace capstone2llvmir
} // namespace retdec
