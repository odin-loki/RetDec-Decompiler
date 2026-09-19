/**
 * @file src/capstone2llvmir/riscv/riscv.cpp
 * @brief RISC-V implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <cmath>

#include <llvm/IR/Intrinsics.h>

#include "capstone2llvmir/riscv/riscv_impl.h"

namespace retdec {
namespace capstone2llvmir {

namespace {

uint32_t csrLlvmRegId(uint16_t encoding)
{
	return static_cast<uint32_t>(RISCV_REG_ENDING) + 0x1000u + encoding;
}

} // namespace


Capstone2LlvmIrTranslatorRiscv_impl::Capstone2LlvmIrTranslatorRiscv_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(
				CS_ARCH_RISCV,
				basic,
				static_cast<cs_mode>(
						static_cast<unsigned>(extra)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBA)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBB)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBKB)
						| static_cast<unsigned>(CS_MODE_RISCV_ZBS)),
				m)
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
	unsigned ext = static_cast<unsigned>(CS_MODE_RISCVC)
			| static_cast<unsigned>(CS_MODE_RISCV_FD)
			| static_cast<unsigned>(CS_MODE_RISCV_A)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBA)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBB)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBKB)
			| static_cast<unsigned>(CS_MODE_RISCV_ZBS);
	auto stripped = static_cast<cs_mode>(static_cast<unsigned>(m) & ~ext);
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

	// Capstone 6 keeps the real id (ADDI/JAL/CSRRS) and puts nop/jal/frflags
	// in alias_id. Prefer a dedicated alias translator (e.g. nop vs addi x0).
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
	// Capstone 6: need_effective_addr means the IMM is still a displacement.
	// When the flag is clear, IMM is already the effective target
	// (beq +8 at 0x1000 arrives as 0x1008, not 8).
	if (i->detail && i->detail->riscv.need_effective_addr)
	{
		return irb.CreateAdd(pc, offset);
	}
	return offset;
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
		case RISCV_OP_CSR:
		{
			auto* v = loadCsr(op.csr, irb);
			return v ? v : llvm::UndefValue::get(ty ? ty : getDefaultType());
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

	uint64_t alias = i->is_alias ? i->alias_id : 0;
	bool isJ = i->id == RISCV_INS_C_J || i->id == RISCV_INS_ALIAS_J
			|| alias == RISCV_INS_ALIAS_J;
	bool isJalAlias = i->id == RISCV_INS_C_JAL || i->id == RISCV_INS_ALIAS_JAL
			|| alias == RISCV_INS_ALIAS_JAL;

	if (isJ)
	{
		EXPECT_IS_UNARY(i, ri, irb);
		offset = loadOp(ri->operands[0], irb);
	}
	else if (isJalAlias || ri->op_count == 1)
	{
		if (ri->op_count == 1)
		{
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

	uint64_t alias = i->is_alias ? i->alias_id : 0;
	bool isJr = i->id == RISCV_INS_C_JR || i->id == RISCV_INS_ALIAS_JR
			|| alias == RISCV_INS_ALIAS_JR;
	bool isJalrAlias = i->id == RISCV_INS_C_JALR || i->id == RISCV_INS_ALIAS_JALR
			|| alias == RISCV_INS_ALIAS_JALR;
	bool isRet = i->id == RISCV_INS_ALIAS_RET || alias == RISCV_INS_ALIAS_RET;

	if (ri->op_count < 3 && (isJr || isJalrAlias || isRet || ri->op_count <= 1))
	{
		if (ri->op_count == 0)
		{
			rs1 = RISCV_REG_RA;
			target = loadRegister(RISCV_REG_RA, irb);
		}
		else if (ri->op_count == 1)
		{
			target = loadOp(ri->operands[0], irb, nullptr, ri->operands[0].type == RISCV_OP_MEM);
			if (ri->operands[0].type == RISCV_OP_REG)
			{
				rs1 = ri->operands[0].reg;
			}
			else if (ri->operands[0].type == RISCV_OP_MEM)
			{
				rs1 = ri->operands[0].mem.base;
			}
		}
		else if (ri->operands[0].type == RISCV_OP_REG
				&& ri->operands[1].type == RISCV_OP_IMM)
		{
			rs1 = ri->operands[0].reg;
			op1 = loadOp(ri->operands[0], irb);
			op2 = loadOp(ri->operands[1], irb);
			op2 = generateTypeConversion(irb, op2, op1->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
			target = irb.CreateAdd(op1, op2);
		}
		else
		{
			EXPECT_IS_UNARY(i, ri, irb);
			target = loadOp(ri->operands[0], irb);
			if (ri->operands[0].type == RISCV_OP_REG)
			{
				rs1 = ri->operands[0].reg;
			}
		}
		if (isJalrAlias || (!isJr && !isRet && i->id == RISCV_INS_JALR))
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
 * Capstone 6 IMM is the effective target unless need_effective_addr is set.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateBranch(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	llvm::Value* cond = nullptr;
	llvm::Value* offset = nullptr;

	if (i->id == RISCV_INS_C_BEQZ
			|| i->id == RISCV_INS_C_BNEZ
			|| i->id == RISCV_INS_ALIAS_BEQZ
			|| i->id == RISCV_INS_ALIAS_BNEZ
			|| i->id == RISCV_INS_ALIAS_BLEZ
			|| i->id == RISCV_INS_ALIAS_BGEZ
			|| i->id == RISCV_INS_ALIAS_BLTZ
			|| i->id == RISCV_INS_ALIAS_BGTZ)
	{
		EXPECT_IS_BINARY(i, ri, irb);
		op0 = loadOp(ri->operands[0], irb);
		offset = loadOp(ri->operands[1], irb);
		auto* zero = llvm::ConstantInt::get(op0->getType(), 0);
		switch (i->id)
		{
			case RISCV_INS_C_BEQZ:
			case RISCV_INS_ALIAS_BEQZ:
				cond = irb.CreateICmpEQ(op0, zero);
				break;
			case RISCV_INS_C_BNEZ:
			case RISCV_INS_ALIAS_BNEZ:
				cond = irb.CreateICmpNE(op0, zero);
				break;
			case RISCV_INS_ALIAS_BLEZ:
				cond = irb.CreateICmpSLE(op0, zero);
				break;
			case RISCV_INS_ALIAS_BGEZ:
				cond = irb.CreateICmpSGE(op0, zero);
				break;
			case RISCV_INS_ALIAS_BLTZ:
				cond = irb.CreateICmpSLT(op0, zero);
				break;
			case RISCV_INS_ALIAS_BGTZ:
				cond = irb.CreateICmpSGT(op0, zero);
				break;
			default:
				throw GenericError("Unhandled insn ID in translateBranch().");
		}
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
 * LB/LBU/LH/LHU/LW/LWU/LD, C_LW/C_LWSP/C_LD/C_LDSP, Zcb C_LBU/C_LH/C_LHU
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
		case RISCV_INS_C_LBU:
			ty = irb.getInt8Ty();
			ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LH:
		case RISCV_INS_C_LH:
			ty = irb.getInt16Ty();
			ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
			break;
		case RISCV_INS_LHU:
		case RISCV_INS_C_LHU:
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
 * SB/SH/SW/SD, C_SW/C_SWSP/C_SD/C_SDSP, Zcb C_SB/C_SH
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateStore(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case RISCV_INS_SB:
		case RISCV_INS_C_SB:
			ty = irb.getInt8Ty();
			break;
		case RISCV_INS_SH:
		case RISCV_INS_C_SH:
			ty = irb.getInt16Ty();
			break;
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

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::amoAddress(
		cs_riscv_op& op,
		llvm::IRBuilder<>& irb)
{
	if (op.type == RISCV_OP_MEM)
	{
		auto* base = loadRegister(op.mem.base, irb);
		if (base == nullptr)
		{
			return llvm::ConstantInt::getSigned(getDefaultType(), op.mem.disp);
		}
		if (op.mem.disp == 0)
		{
			return base;
		}
		auto* disp = llvm::ConstantInt::getSigned(base->getType(), op.mem.disp);
		return irb.CreateAdd(base, disp);
	}
	return loadOp(op, irb, nullptr, /*lea=*/true);
}

llvm::AtomicOrdering Capstone2LlvmIrTranslatorRiscv_impl::amoOrdering(unsigned id) const
{
	switch (id)
	{
		case RISCV_INS_AMOADD_W_AQ:
		case RISCV_INS_AMOADD_D_AQ:
		case RISCV_INS_AMOAND_W_AQ:
		case RISCV_INS_AMOAND_D_AQ:
		case RISCV_INS_AMOOR_W_AQ:
		case RISCV_INS_AMOOR_D_AQ:
		case RISCV_INS_AMOXOR_W_AQ:
		case RISCV_INS_AMOXOR_D_AQ:
		case RISCV_INS_AMOSWAP_W_AQ:
		case RISCV_INS_AMOSWAP_D_AQ:
		case RISCV_INS_AMOMAX_W_AQ:
		case RISCV_INS_AMOMAX_D_AQ:
		case RISCV_INS_AMOMAXU_W_AQ:
		case RISCV_INS_AMOMAXU_D_AQ:
		case RISCV_INS_AMOMIN_W_AQ:
		case RISCV_INS_AMOMIN_D_AQ:
		case RISCV_INS_AMOMINU_W_AQ:
		case RISCV_INS_AMOMINU_D_AQ:
		case RISCV_INS_LR_W_AQ:
		case RISCV_INS_LR_D_AQ:
		case RISCV_INS_SC_W_AQ:
		case RISCV_INS_SC_D_AQ:
			return llvm::AtomicOrdering::Acquire;
		case RISCV_INS_AMOADD_W_RL:
		case RISCV_INS_AMOADD_D_RL:
		case RISCV_INS_AMOAND_W_RL:
		case RISCV_INS_AMOAND_D_RL:
		case RISCV_INS_AMOOR_W_RL:
		case RISCV_INS_AMOOR_D_RL:
		case RISCV_INS_AMOXOR_W_RL:
		case RISCV_INS_AMOXOR_D_RL:
		case RISCV_INS_AMOSWAP_W_RL:
		case RISCV_INS_AMOSWAP_D_RL:
		case RISCV_INS_AMOMAX_W_RL:
		case RISCV_INS_AMOMAX_D_RL:
		case RISCV_INS_AMOMAXU_W_RL:
		case RISCV_INS_AMOMAXU_D_RL:
		case RISCV_INS_AMOMIN_W_RL:
		case RISCV_INS_AMOMIN_D_RL:
		case RISCV_INS_AMOMINU_W_RL:
		case RISCV_INS_AMOMINU_D_RL:
		case RISCV_INS_LR_W_RL:
		case RISCV_INS_LR_D_RL:
		case RISCV_INS_SC_W_RL:
		case RISCV_INS_SC_D_RL:
			return llvm::AtomicOrdering::Release;
		case RISCV_INS_AMOADD_W_AQ_RL:
		case RISCV_INS_AMOADD_D_AQ_RL:
		case RISCV_INS_AMOAND_W_AQ_RL:
		case RISCV_INS_AMOAND_D_AQ_RL:
		case RISCV_INS_AMOOR_W_AQ_RL:
		case RISCV_INS_AMOOR_D_AQ_RL:
		case RISCV_INS_AMOXOR_W_AQ_RL:
		case RISCV_INS_AMOXOR_D_AQ_RL:
		case RISCV_INS_AMOSWAP_W_AQ_RL:
		case RISCV_INS_AMOSWAP_D_AQ_RL:
		case RISCV_INS_AMOMAX_W_AQ_RL:
		case RISCV_INS_AMOMAX_D_AQ_RL:
		case RISCV_INS_AMOMAXU_W_AQ_RL:
		case RISCV_INS_AMOMAXU_D_AQ_RL:
		case RISCV_INS_AMOMIN_W_AQ_RL:
		case RISCV_INS_AMOMIN_D_AQ_RL:
		case RISCV_INS_AMOMINU_W_AQ_RL:
		case RISCV_INS_AMOMINU_D_AQ_RL:
		case RISCV_INS_LR_W_AQ_RL:
		case RISCV_INS_LR_D_AQ_RL:
		case RISCV_INS_SC_W_AQ_RL:
		case RISCV_INS_SC_D_AQ_RL:
			return llvm::AtomicOrdering::SequentiallyConsistent;
		default:
			return llvm::AtomicOrdering::Monotonic;
	}
}

llvm::Type* Capstone2LlvmIrTranslatorRiscv_impl::amoAccessType(unsigned id, llvm::IRBuilder<>& irb)
{
	switch (id)
	{
		case RISCV_INS_AMOADD_D:
		case RISCV_INS_AMOADD_D_AQ:
		case RISCV_INS_AMOADD_D_RL:
		case RISCV_INS_AMOADD_D_AQ_RL:
		case RISCV_INS_AMOAND_D:
		case RISCV_INS_AMOAND_D_AQ:
		case RISCV_INS_AMOAND_D_RL:
		case RISCV_INS_AMOAND_D_AQ_RL:
		case RISCV_INS_AMOOR_D:
		case RISCV_INS_AMOOR_D_AQ:
		case RISCV_INS_AMOOR_D_RL:
		case RISCV_INS_AMOOR_D_AQ_RL:
		case RISCV_INS_AMOXOR_D:
		case RISCV_INS_AMOXOR_D_AQ:
		case RISCV_INS_AMOXOR_D_RL:
		case RISCV_INS_AMOXOR_D_AQ_RL:
		case RISCV_INS_AMOSWAP_D:
		case RISCV_INS_AMOSWAP_D_AQ:
		case RISCV_INS_AMOSWAP_D_RL:
		case RISCV_INS_AMOSWAP_D_AQ_RL:
		case RISCV_INS_AMOMAX_D:
		case RISCV_INS_AMOMAX_D_AQ:
		case RISCV_INS_AMOMAX_D_RL:
		case RISCV_INS_AMOMAX_D_AQ_RL:
		case RISCV_INS_AMOMAXU_D:
		case RISCV_INS_AMOMAXU_D_AQ:
		case RISCV_INS_AMOMAXU_D_RL:
		case RISCV_INS_AMOMAXU_D_AQ_RL:
		case RISCV_INS_AMOMIN_D:
		case RISCV_INS_AMOMIN_D_AQ:
		case RISCV_INS_AMOMIN_D_RL:
		case RISCV_INS_AMOMIN_D_AQ_RL:
		case RISCV_INS_AMOMINU_D:
		case RISCV_INS_AMOMINU_D_AQ:
		case RISCV_INS_AMOMINU_D_RL:
		case RISCV_INS_AMOMINU_D_AQ_RL:
		case RISCV_INS_LR_D:
		case RISCV_INS_LR_D_AQ:
		case RISCV_INS_LR_D_RL:
		case RISCV_INS_LR_D_AQ_RL:
		case RISCV_INS_SC_D:
		case RISCV_INS_SC_D_AQ:
		case RISCV_INS_SC_D_RL:
		case RISCV_INS_SC_D_AQ_RL:
			return irb.getInt64Ty();
		default:
			return irb.getInt32Ty();
	}
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::definedDivisor(
		llvm::Value* dividend,
		llvm::Value* divisor,
		bool isSigned,
		llvm::IRBuilder<>& irb)
{
	auto* ty = divisor->getType();
	auto* one = llvm::ConstantInt::get(ty, 1);
	if (isSigned)
	{
		unsigned bits = llvm::cast<llvm::IntegerType>(ty)->getBitWidth();
		auto* intMin = llvm::ConstantInt::get(ty, llvm::APInt::getSignedMinValue(bits));
		auto* minusOne = llvm::ConstantInt::getSigned(ty, -1);
		auto* overflow = irb.CreateAnd(
				irb.CreateICmpEQ(dividend, intMin),
				irb.CreateICmpEQ(divisor, minusOne));
		divisor = irb.CreateSelect(overflow, one, divisor);
	}
	return irb.CreateBinaryIntrinsic(llvm::Intrinsic::umax, divisor, one);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::fpToInt(
		llvm::Value* v,
		llvm::Type* intTy,
		bool isSigned,
		llvm::IRBuilder<>& irb)
{
	unsigned bits = intTy->getScalarSizeInBits();
	auto* fpTy = v->getType();
	llvm::Value* loF = nullptr;
	llvm::Value* hiF = nullptr;
	llvm::Value* oob = nullptr;
	if (isSigned)
	{
		loF = llvm::ConstantFP::get(fpTy, -std::ldexp(1.0, static_cast<int>(bits - 1)));
		hiF = llvm::ConstantFP::get(fpTy, std::ldexp(1.0, static_cast<int>(bits - 1)));
		oob = llvm::ConstantInt::get(intTy, llvm::APInt::getSignedMaxValue(bits));
	}
	else
	{
		loF = llvm::ConstantFP::get(fpTy, 0.0);
		hiF = llvm::ConstantFP::get(fpTy, std::ldexp(1.0, static_cast<int>(bits)));
		oob = llvm::ConstantInt::get(intTy, llvm::APInt::getMaxValue(bits));
	}
	auto* inRange = irb.CreateAnd(irb.CreateFCmpOGE(v, loF), irb.CreateFCmpOLT(v, hiF));
	auto* safe = irb.CreateSelect(inRange, v, llvm::ConstantFP::get(fpTy, 0.0));
	auto* conv = isSigned ? irb.CreateFPToSI(safe, intTy) : irb.CreateFPToUI(safe, intTy);
	return irb.CreateSelect(inRange, conv, oob);
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::fpIntrinsic(
		llvm::IRBuilder<>& irb,
		llvm::Intrinsic::ID id,
		llvm::ArrayRef<llvm::Value*> args)
{
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, id, args.front()->getType());
	return irb.CreateCall(f, args);
}

llvm::Type* Capstone2LlvmIrTranslatorRiscv_impl::fpTypeOfInsn(unsigned id, llvm::IRBuilder<>& irb) const
{
	switch (id)
	{
		case RISCV_INS_FMADD_D:
		case RISCV_INS_FMSUB_D:
		case RISCV_INS_FNMADD_D:
		case RISCV_INS_FNMSUB_D:
		case RISCV_INS_FSQRT_D:
		case RISCV_INS_FSGNJ_D:
		case RISCV_INS_FSGNJN_D:
		case RISCV_INS_FSGNJX_D:
		case RISCV_INS_FMIN_D:
		case RISCV_INS_FMAX_D:
		case RISCV_INS_FMV_D_X:
		case RISCV_INS_FMV_X_D:
		case RISCV_INS_FCLASS_D:
			return irb.getDoubleTy();
		default:
			return irb.getFloatTy();
	}
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::asFp(
		llvm::Value* v,
		llvm::Type* ty,
		llvm::IRBuilder<>& irb)
{
	if (v->getType() == ty)
	{
		return v;
	}
	if (v->getType()->isFloatingPointTy() && ty->isFloatingPointTy())
	{
		return irb.CreateFPCast(v, ty);
	}
	return generateTypeConversion(irb, v, ty, eOpConv::FPCAST_OR_BITCAST);
}

uint32_t Capstone2LlvmIrTranslatorRiscv_impl::csrOperandReg(uint16_t encoding) const
{
	switch (encoding)
	{
		case RISCV_SYSREG_FFLAGS: return RISCV_REG_FFLAGS;
		case RISCV_SYSREG_FRM: return RISCV_REG_FRM;
		case RISCV_SYSREG_VL: return RISCV_REG_VL;
		case RISCV_SYSREG_VTYPE: return RISCV_REG_VTYPE;
		case RISCV_SYSREG_VLENB: return RISCV_REG_VLENB;
		case RISCV_SYSREG_VXSAT: return RISCV_REG_VXSAT;
		case RISCV_SYSREG_VXRM: return RISCV_REG_VXRM;
		case RISCV_SYSREG_FCSR:
		case RISCV_SYSREG_CYCLE:
		case RISCV_SYSREG_TIME:
		case RISCV_SYSREG_INSTRET:
		case RISCV_SYSREG_CYCLEH:
		case RISCV_SYSREG_TIMEH:
		case RISCV_SYSREG_INSTRETH:
		case RISCV_SYSREG_SSTATUS:
		case RISCV_SYSREG_SIE:
		case RISCV_SYSREG_STVEC:
		case RISCV_SYSREG_SSCRATCH:
		case RISCV_SYSREG_SEPC:
		case RISCV_SYSREG_SCAUSE:
		case RISCV_SYSREG_STVAL:
		case RISCV_SYSREG_SIP:
		case RISCV_SYSREG_SATP:
		case RISCV_SYSREG_MSTATUS:
		case RISCV_SYSREG_MISA:
		case RISCV_SYSREG_MEDELEG:
		case RISCV_SYSREG_MIDELEG:
		case RISCV_SYSREG_MIE:
		case RISCV_SYSREG_MTVEC:
		case RISCV_SYSREG_MSCRATCH:
		case RISCV_SYSREG_MEPC:
		case RISCV_SYSREG_MCAUSE:
		case RISCV_SYSREG_MTVAL:
		case RISCV_SYSREG_MIP:
		case RISCV_SYSREG_MHARTID:
		case RISCV_SYSREG_MVENDORID:
		case RISCV_SYSREG_MARCHID:
		case RISCV_SYSREG_MIMPID:
			return csrLlvmRegId(encoding);
		default:
			return RISCV_REG_INVALID;
	}
}

llvm::Value* Capstone2LlvmIrTranslatorRiscv_impl::loadCsr(uint16_t encoding, llvm::IRBuilder<>& irb)
{
	if (encoding == RISCV_SYSREG_FCSR)
	{
		auto* flags = loadRegister(RISCV_REG_FFLAGS, irb);
		auto* frm = loadRegister(RISCV_REG_FRM, irb);
		auto* ty = getDefaultType();
		flags = irb.CreateZExtOrTrunc(flags, ty);
		frm = irb.CreateZExtOrTrunc(frm, ty);
		auto* fmask = llvm::ConstantInt::get(ty, 0x1f);
		auto* rmask = llvm::ConstantInt::get(ty, 0x7);
		return irb.CreateOr(
				irb.CreateAnd(flags, fmask),
				irb.CreateShl(irb.CreateAnd(frm, rmask), llvm::ConstantInt::get(ty, 5)));
	}
	auto r = csrOperandReg(encoding);
	if (r == RISCV_REG_INVALID || getRegister(r) == nullptr)
	{
		return nullptr;
	}
	return loadRegister(r, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::storeCsr(
		uint16_t encoding,
		llvm::Value* val,
		llvm::IRBuilder<>& irb)
{
	if (encoding == RISCV_SYSREG_FCSR)
	{
		auto* ty = getDefaultType();
		val = irb.CreateZExtOrTrunc(val, ty);
		storeRegister(RISCV_REG_FFLAGS, irb.CreateAnd(val, llvm::ConstantInt::get(ty, 0x1f)), irb);
		storeRegister(
				RISCV_REG_FRM,
				irb.CreateAnd(
						irb.CreateLShr(val, llvm::ConstantInt::get(ty, 5)),
						llvm::ConstantInt::get(ty, 0x7)),
				irb);
		return;
	}
	auto r = csrOperandReg(encoding);
	if (r == RISCV_REG_INVALID || getRegister(r) == nullptr)
	{
		return;
	}
	storeRegister(r, val, irb);
}

/**
 * MUL/MULH/MULHSU/MULHU/MULW, C_MUL.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateMul(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	bool word = i->id == RISCV_INS_MULW;
	if (word)
	{
		op1 = narrowToWord(irb, op1);
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
	}

	bool high = i->id == RISCV_INS_MULH || i->id == RISCV_INS_MULHU || i->id == RISCV_INS_MULHSU;
	unsigned half = op1->getType()->getIntegerBitWidth();
	auto* wide = irb.getIntNTy(half * 2);

	if (i->id == RISCV_INS_MULHU)
	{
		op1 = irb.CreateZExt(op1, wide);
		op2 = irb.CreateZExt(op2, wide);
	}
	else if (i->id == RISCV_INS_MULHSU)
	{
		op1 = irb.CreateSExt(op1, wide);
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
			? irb.CreateTrunc(irb.CreateLShr(mul, llvm::ConstantInt::get(wide, half)), halfTy)
			: irb.CreateTrunc(mul, halfTy);
	if (word)
	{
		res = widenFromWord(irb, res);
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * DIV/DIVU/REM/REMU and W variants. RISC-V names the zero-divisor and
 * signed-overflow results, so the IR never carries LLVM sdiv/udiv UB.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateDiv(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);

	bool word = i->id == RISCV_INS_DIVW || i->id == RISCV_INS_DIVUW
			|| i->id == RISCV_INS_REMW || i->id == RISCV_INS_REMUW;
	if (word)
	{
		op1 = narrowToWord(irb, op1);
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
	}

	bool isSigned = i->id == RISCV_INS_DIV || i->id == RISCV_INS_DIVW
			|| i->id == RISCV_INS_REM || i->id == RISCV_INS_REMW;
	bool remainder = i->id == RISCV_INS_REM || i->id == RISCV_INS_REMU
			|| i->id == RISCV_INS_REMW || i->id == RISCV_INS_REMUW;

	auto* ty = op1->getType();
	auto* zero = llvm::ConstantInt::get(ty, 0);
	auto* divZero = irb.CreateICmpEQ(op2, zero);
	auto* safe = definedDivisor(op1, op2, isSigned, irb);

	llvm::Value* arith = nullptr;
	llvm::Value* dzRes = nullptr;
	if (remainder)
	{
		arith = isSigned ? irb.CreateSRem(op1, safe) : irb.CreateURem(op1, safe);
		dzRes = op1;
	}
	else if (isSigned)
	{
		arith = irb.CreateSDiv(op1, safe);
		dzRes = llvm::ConstantInt::getSigned(ty, -1);
	}
	else
	{
		arith = irb.CreateUDiv(op1, safe);
		dzRes = llvm::ConstantInt::getAllOnesValue(ty);
	}

	llvm::Value* res = irb.CreateSelect(divZero, dzRes, arith);
	if (isSigned)
	{
		unsigned bits = llvm::cast<llvm::IntegerType>(ty)->getBitWidth();
		auto* intMin = llvm::ConstantInt::get(ty, llvm::APInt::getSignedMinValue(bits));
		auto* minusOne = llvm::ConstantInt::getSigned(ty, -1);
		auto* overflow = irb.CreateAnd(
				irb.CreateICmpEQ(op1, intMin),
				irb.CreateICmpEQ(op2, minusOne));
		auto* ovRes = remainder ? llvm::ConstantInt::get(ty, 0) : intMin;
		res = irb.CreateSelect(overflow, ovRes, res);
	}

	if (word)
	{
		res = widenFromWord(irb, res);
	}
	storeOp(ri->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFpArith(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	op1 = loadOp(ri->operands[1], irb);
	op2 = loadOp(ri->operands[2], irb);
	if (op1->getType() != op2->getType() && op2->getType()->isFloatingPointTy())
	{
		op1 = irb.CreateFPCast(op1, op2->getType());
	}
	else if (op1->getType() != op2->getType() && op1->getType()->isFloatingPointTy())
	{
		op2 = irb.CreateFPCast(op2, op1->getType());
	}

	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FADD_S:
		case RISCV_INS_FADD_D:
			res = irb.CreateFAdd(op1, op2);
			break;
		case RISCV_INS_FSUB_S:
		case RISCV_INS_FSUB_D:
			res = irb.CreateFSub(op1, op2);
			break;
		case RISCV_INS_FMUL_S:
		case RISCV_INS_FMUL_D:
			res = irb.CreateFMul(op1, op2);
			break;
		case RISCV_INS_FDIV_S:
		case RISCV_INS_FDIV_D:
			res = irb.CreateFDiv(op1, op2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpArith().");
	}
	storeOp(ri->operands[0], res, irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFpLoad(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FLW:
		case RISCV_INS_C_FLW:
		case RISCV_INS_C_FLWSP:
			ty = irb.getFloatTy();
			break;
		case RISCV_INS_FLD:
		case RISCV_INS_C_FLD:
		case RISCV_INS_C_FLDSP:
			ty = irb.getDoubleTy();
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpLoad().");
	}

	if (ri->operands[1].type == RISCV_OP_MEM
			|| (ri->op_count == 2 && ri->operands[1].type != RISCV_OP_REG))
	{
		op1 = loadOp(ri->operands[1], irb, ty);
	}
	else if (ri->op_count >= 3)
	{
		auto* base = loadOp(ri->operands[1], irb);
		auto* off = loadOp(ri->operands[2], irb);
		off = irb.CreateSExtOrTrunc(off, base->getType());
		op1 = loadIntPtr(irb, irb.CreateAdd(base, off), ty);
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	storeOp(ri->operands[0], op1, irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFpStore(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FSW:
		case RISCV_INS_C_FSW:
		case RISCV_INS_C_FSWSP:
			ty = irb.getFloatTy();
			break;
		case RISCV_INS_FSD:
		case RISCV_INS_C_FSD:
		case RISCV_INS_C_FSDSP:
			ty = irb.getDoubleTy();
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpStore().");
	}

	op0 = loadOp(ri->operands[0], irb);
	if (op0->getType() != ty)
	{
		if (op0->getType()->isFloatingPointTy() && ty->isFloatingPointTy())
		{
			op0 = irb.CreateFPCast(op0, ty);
		}
		else if (ty->isFloatingPointTy())
		{
			op0 = generateTypeConversion(irb, op0, ty, eOpConv::FPCAST_OR_BITCAST);
		}
		else
		{
			op0 = irb.CreateBitCast(op0, ty);
		}
	}

	if (ri->operands[1].type == RISCV_OP_MEM
			|| (ri->op_count == 2 && ri->operands[1].type != RISCV_OP_REG))
	{
		storeOp(ri->operands[1], op0, irb);
	}
	else if (ri->op_count >= 3)
	{
		auto* base = loadOp(ri->operands[1], irb);
		auto* off = loadOp(ri->operands[2], irb);
		off = irb.CreateSExtOrTrunc(off, base->getType());
		storeIntPtr(irb, op0, irb.CreateAdd(base, off), ty);
	}
	else
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFcvt(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	llvm::Value* res = nullptr;

	switch (i->id)
	{
		case RISCV_INS_FCVT_S_D:
			res = irb.CreateFPTrunc(op1, irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_D_S:
			res = irb.CreateFPExt(op1, irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_S_W:
			res = irb.CreateSIToFP(irb.CreateSExtOrTrunc(op1, irb.getInt32Ty()), irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_S_WU:
			res = irb.CreateUIToFP(irb.CreateZExtOrTrunc(op1, irb.getInt32Ty()), irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_D_W:
			res = irb.CreateSIToFP(irb.CreateSExtOrTrunc(op1, irb.getInt32Ty()), irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_D_WU:
			res = irb.CreateUIToFP(irb.CreateZExtOrTrunc(op1, irb.getInt32Ty()), irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_S_L:
			res = irb.CreateSIToFP(irb.CreateSExtOrTrunc(op1, irb.getInt64Ty()), irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_S_LU:
			res = irb.CreateUIToFP(irb.CreateZExtOrTrunc(op1, irb.getInt64Ty()), irb.getFloatTy());
			break;
		case RISCV_INS_FCVT_D_L:
			res = irb.CreateSIToFP(irb.CreateSExtOrTrunc(op1, irb.getInt64Ty()), irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_D_LU:
			res = irb.CreateUIToFP(irb.CreateZExtOrTrunc(op1, irb.getInt64Ty()), irb.getDoubleTy());
			break;
		case RISCV_INS_FCVT_W_S:
		case RISCV_INS_FCVT_W_D:
			res = widenFromWord(irb, fpToInt(op1, irb.getInt32Ty(), /*isSigned=*/true, irb));
			break;
		case RISCV_INS_FCVT_WU_S:
		case RISCV_INS_FCVT_WU_D:
			res = irb.CreateZExt(fpToInt(op1, irb.getInt32Ty(), /*isSigned=*/false, irb), getDefaultType());
			break;
		case RISCV_INS_FCVT_L_S:
		case RISCV_INS_FCVT_L_D:
			res = fpToInt(op1, irb.getInt64Ty(), /*isSigned=*/true, irb);
			break;
		case RISCV_INS_FCVT_LU_S:
		case RISCV_INS_FCVT_LU_D:
			res = fpToInt(op1, irb.getInt64Ty(), /*isSigned=*/false, irb);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFcvt().");
	}
	storeOp(ri->operands[0], res, irb, res->getType()->isFloatingPointTy()
			? eOpConv::FPCAST_OR_BITCAST
			: eOpConv::SEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFcmp(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}
	op1 = loadOp(ri->operands[1], irb);
	op2 = loadOp(ri->operands[2], irb);
	if (op1->getType() != op2->getType() && op1->getType()->isFloatingPointTy())
	{
		op2 = irb.CreateFPCast(op2, op1->getType());
	}

	llvm::Value* cmp = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FEQ_S:
		case RISCV_INS_FEQ_D:
			cmp = irb.CreateFCmpOEQ(op1, op2);
			break;
		case RISCV_INS_FLT_S:
		case RISCV_INS_FLT_D:
			cmp = irb.CreateFCmpOLT(op1, op2);
			break;
		case RISCV_INS_FLE_S:
		case RISCV_INS_FLE_D:
			cmp = irb.CreateFCmpOLE(op1, op2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFcmp().");
	}
	storeOp(ri->operands[0], irb.CreateZExt(cmp, getDefaultType()), irb);
}

/**
 * FMADD/FMSUB/FNMADD/FNMSUB S/D: rd = ±(rs1 * rs2) ± rs3.
 * Capstone operands follow assembly order: rd, rs1, rs2, rs3 [, rm].
 * rm lives in cs_riscv.rounding_mode and is ignored, same as FADD.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateFpFma(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 4)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* rs1 = asFp(loadOp(ri->operands[1], irb), ty, irb);
	auto* rs2 = asFp(loadOp(ri->operands[2], irb), ty, irb);
	auto* rs3 = asFp(loadOp(ri->operands[3], irb), ty, irb);

	llvm::Value* a = rs1;
	llvm::Value* c = rs3;
	switch (i->id)
	{
		case RISCV_INS_FMADD_S:
		case RISCV_INS_FMADD_D:
			break;
		case RISCV_INS_FMSUB_S:
		case RISCV_INS_FMSUB_D:
			c = irb.CreateFNeg(rs3);
			break;
		case RISCV_INS_FNMSUB_S:
		case RISCV_INS_FNMSUB_D:
			a = irb.CreateFNeg(rs1);
			break;
		case RISCV_INS_FNMADD_S:
		case RISCV_INS_FNMADD_D:
			a = irb.CreateFNeg(rs1);
			c = irb.CreateFNeg(rs3);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpFma().");
	}

	storeOp(ri->operands[0], fpIntrinsic(irb, llvm::Intrinsic::fma, {a, rs2, c}), irb,
			eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFsqrt(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* src = asFp(loadOp(ri->operands[1], irb), ty, irb);
	storeOp(ri->operands[0], fpIntrinsic(irb, llvm::Intrinsic::sqrt, {src}), irb,
			eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFsgnj(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* rs1 = asFp(loadOp(ri->operands[1], irb), ty, irb);
	auto* rs2 = asFp(loadOp(ri->operands[2], irb), ty, irb);
	unsigned bits = ty->isDoubleTy() ? 64u : 32u;
	auto* intTy = irb.getIntNTy(bits);
	auto* ia = irb.CreateBitCast(rs1, intTy);
	auto* ib = irb.CreateBitCast(rs2, intTy);
	auto* signMask = llvm::ConstantInt::get(intTy, llvm::APInt::getSignMask(bits));
	auto* magMask = llvm::ConstantInt::get(intTy, ~llvm::APInt::getSignMask(bits));

	llvm::Value* bitsOut = nullptr;
	switch (i->id)
	{
		case RISCV_INS_FSGNJ_S:
		case RISCV_INS_FSGNJ_D:
			bitsOut = irb.CreateOr(irb.CreateAnd(ia, magMask), irb.CreateAnd(ib, signMask));
			break;
		case RISCV_INS_FSGNJN_S:
		case RISCV_INS_FSGNJN_D:
			bitsOut = irb.CreateOr(
					irb.CreateAnd(ia, magMask),
					irb.CreateAnd(irb.CreateNot(ib), signMask));
			break;
		case RISCV_INS_FSGNJX_S:
		case RISCV_INS_FSGNJX_D:
			bitsOut = irb.CreateXor(ia, irb.CreateAnd(ib, signMask));
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFsgnj().");
	}
	storeOp(ri->operands[0], irb.CreateBitCast(bitsOut, ty), irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFminMax(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 3)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* rs1 = asFp(loadOp(ri->operands[1], irb), ty, irb);
	auto* rs2 = asFp(loadOp(ri->operands[2], irb), ty, irb);
	llvm::Intrinsic::ID iid = llvm::Intrinsic::minnum;
	switch (i->id)
	{
		case RISCV_INS_FMIN_S:
		case RISCV_INS_FMIN_D:
			iid = llvm::Intrinsic::minnum;
			break;
		case RISCV_INS_FMAX_S:
		case RISCV_INS_FMAX_D:
			iid = llvm::Intrinsic::maxnum;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFminMax().");
	}
	storeOp(ri->operands[0], fpIntrinsic(irb, iid, {rs1, rs2}), irb, eOpConv::FPCAST_OR_BITCAST);
}

/**
 * FCLASS_S / FCLASS_D: 10-bit mask in rd (unprivileged spec).
 * bit0 -inf, 1 neg normal, 2 neg subnormal, 3 -0, 4 +0, 5 pos subnormal,
 * 6 pos normal, 7 +inf, 8 sNaN, 9 qNaN.
 *
 * LLVM 23 has Intrinsic::is_fpclass, but llvmir-emul does not lower it, so
 * this is bitcast + icmp/select. Quiet vs signaling uses the fraction MSB.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateFclass(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	auto* ty = fpTypeOfInsn(i->id, irb);
	auto* src = asFp(loadOp(ri->operands[1], irb), ty, irb);
	unsigned nbits = ty->isDoubleTy() ? 64u : 32u;
	unsigned fracBits = ty->isDoubleTy() ? 52u : 23u;
	auto* intTy = irb.getIntNTy(nbits);
	auto* bits = irb.CreateBitCast(src, intTy);
	auto* zero = llvm::ConstantInt::get(intTy, 0);
	auto* signMask = llvm::ConstantInt::get(intTy, llvm::APInt::getSignMask(nbits));
	auto* fracMask = llvm::ConstantInt::get(intTy, llvm::APInt::getLowBitsSet(nbits, fracBits));
	auto* expMask = llvm::ConstantInt::get(
			intTy,
			llvm::APInt::getBitsSet(nbits, fracBits, nbits - 1));
	auto* qnanBit = llvm::ConstantInt::get(
			intTy,
			llvm::APInt::getOneBitSet(nbits, fracBits - 1));

	auto* sign = irb.CreateAnd(bits, signMask);
	auto* exp = irb.CreateAnd(bits, expMask);
	auto* frac = irb.CreateAnd(bits, fracMask);
	auto* qbit = irb.CreateAnd(bits, qnanBit);

	auto* isNeg = irb.CreateICmpNE(sign, zero);
	auto* isPos = irb.CreateICmpEQ(sign, zero);
	auto* exp0 = irb.CreateICmpEQ(exp, zero);
	auto* expMax = irb.CreateICmpEQ(exp, expMask);
	auto* frac0 = irb.CreateICmpEQ(frac, zero);
	auto* fracNz = irb.CreateICmpNE(frac, zero);
	auto* expNorm = irb.CreateAnd(irb.CreateICmpNE(exp, zero), irb.CreateICmpNE(exp, expMask));
	auto* isQuiet = irb.CreateICmpNE(qbit, zero);
	auto* isSig = irb.CreateICmpEQ(qbit, zero);

	auto* dstTy = getDefaultType();
	auto* z = llvm::ConstantInt::get(dstTy, 0);
	auto classBit = [&](llvm::Value* cond, unsigned b) -> llvm::Value* {
		return irb.CreateSelect(cond, llvm::ConstantInt::get(dstTy, 1ull << b), z);
	};

	llvm::Value* res = classBit(irb.CreateAnd(isNeg, irb.CreateAnd(expMax, frac0)), 0);
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, expNorm), 1));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, irb.CreateAnd(exp0, fracNz)), 2));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isNeg, irb.CreateAnd(exp0, frac0)), 3));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(exp0, frac0)), 4));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(exp0, fracNz)), 5));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, expNorm), 6));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(isPos, irb.CreateAnd(expMax, frac0)), 7));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(expMax, irb.CreateAnd(fracNz, isSig)), 8));
	res = irb.CreateOr(res, classBit(irb.CreateAnd(expMax, irb.CreateAnd(fracNz, isQuiet)), 9));
	storeOp(ri->operands[0], res, irb);
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateFmv(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = loadOp(ri->operands[1], irb);
	switch (i->id)
	{
		case RISCV_INS_FMV_W_X:
		{
			if (!op1->getType()->isIntegerTy())
			{
				op1 = irb.CreateBitCast(
						op1,
						irb.getIntNTy(op1->getType()->getPrimitiveSizeInBits()));
			}
			auto* bits = irb.CreateTrunc(op1, irb.getInt32Ty());
			storeOp(ri->operands[0], irb.CreateBitCast(bits, irb.getFloatTy()), irb,
					eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case RISCV_INS_FMV_X_W:
		{
			if (op1->getType()->isFloatingPointTy())
			{
				unsigned n = op1->getType()->getPrimitiveSizeInBits();
				op1 = irb.CreateBitCast(op1, irb.getIntNTy(n));
			}
			auto* bits = irb.CreateTrunc(op1, irb.getInt32Ty());
			storeOp(ri->operands[0], bits, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
			return;
		}
		case RISCV_INS_FMV_D_X:
		{
			if (!op1->getType()->isIntegerTy())
			{
				op1 = irb.CreateBitCast(
						op1,
						irb.getIntNTy(op1->getType()->getPrimitiveSizeInBits()));
			}
			auto* bits = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
			storeOp(ri->operands[0], irb.CreateBitCast(bits, irb.getDoubleTy()), irb,
					eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case RISCV_INS_FMV_X_D:
		{
			if (op1->getType()->isFloatingPointTy())
			{
				unsigned n = op1->getType()->getPrimitiveSizeInBits();
				op1 = irb.CreateBitCast(op1, irb.getIntNTy(n));
			}
			auto* bits = irb.CreateZExtOrTrunc(op1, irb.getInt64Ty());
			storeOp(ri->operands[0], bits, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
			return;
		}
		default:
			throw GenericError("Unhandled insn ID in translateFmv().");
	}
}

void Capstone2LlvmIrTranslatorRiscv_impl::translateAmo(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ri, irb);

	llvm::AtomicRMWInst::BinOp op;
	switch (i->id)
	{
		case RISCV_INS_AMOADD_W:
		case RISCV_INS_AMOADD_W_AQ:
		case RISCV_INS_AMOADD_W_RL:
		case RISCV_INS_AMOADD_W_AQ_RL:
		case RISCV_INS_AMOADD_D:
		case RISCV_INS_AMOADD_D_AQ:
		case RISCV_INS_AMOADD_D_RL:
		case RISCV_INS_AMOADD_D_AQ_RL:
			op = llvm::AtomicRMWInst::Add;
			break;
		case RISCV_INS_AMOAND_W:
		case RISCV_INS_AMOAND_W_AQ:
		case RISCV_INS_AMOAND_W_RL:
		case RISCV_INS_AMOAND_W_AQ_RL:
		case RISCV_INS_AMOAND_D:
		case RISCV_INS_AMOAND_D_AQ:
		case RISCV_INS_AMOAND_D_RL:
		case RISCV_INS_AMOAND_D_AQ_RL:
			op = llvm::AtomicRMWInst::And;
			break;
		case RISCV_INS_AMOOR_W:
		case RISCV_INS_AMOOR_W_AQ:
		case RISCV_INS_AMOOR_W_RL:
		case RISCV_INS_AMOOR_W_AQ_RL:
		case RISCV_INS_AMOOR_D:
		case RISCV_INS_AMOOR_D_AQ:
		case RISCV_INS_AMOOR_D_RL:
		case RISCV_INS_AMOOR_D_AQ_RL:
			op = llvm::AtomicRMWInst::Or;
			break;
		case RISCV_INS_AMOXOR_W:
		case RISCV_INS_AMOXOR_W_AQ:
		case RISCV_INS_AMOXOR_W_RL:
		case RISCV_INS_AMOXOR_W_AQ_RL:
		case RISCV_INS_AMOXOR_D:
		case RISCV_INS_AMOXOR_D_AQ:
		case RISCV_INS_AMOXOR_D_RL:
		case RISCV_INS_AMOXOR_D_AQ_RL:
			op = llvm::AtomicRMWInst::Xor;
			break;
		case RISCV_INS_AMOSWAP_W:
		case RISCV_INS_AMOSWAP_W_AQ:
		case RISCV_INS_AMOSWAP_W_RL:
		case RISCV_INS_AMOSWAP_W_AQ_RL:
		case RISCV_INS_AMOSWAP_D:
		case RISCV_INS_AMOSWAP_D_AQ:
		case RISCV_INS_AMOSWAP_D_RL:
		case RISCV_INS_AMOSWAP_D_AQ_RL:
			op = llvm::AtomicRMWInst::Xchg;
			break;
		case RISCV_INS_AMOMAX_W:
		case RISCV_INS_AMOMAX_W_AQ:
		case RISCV_INS_AMOMAX_W_RL:
		case RISCV_INS_AMOMAX_W_AQ_RL:
		case RISCV_INS_AMOMAX_D:
		case RISCV_INS_AMOMAX_D_AQ:
		case RISCV_INS_AMOMAX_D_RL:
		case RISCV_INS_AMOMAX_D_AQ_RL:
			op = llvm::AtomicRMWInst::Max;
			break;
		case RISCV_INS_AMOMAXU_W:
		case RISCV_INS_AMOMAXU_W_AQ:
		case RISCV_INS_AMOMAXU_W_RL:
		case RISCV_INS_AMOMAXU_W_AQ_RL:
		case RISCV_INS_AMOMAXU_D:
		case RISCV_INS_AMOMAXU_D_AQ:
		case RISCV_INS_AMOMAXU_D_RL:
		case RISCV_INS_AMOMAXU_D_AQ_RL:
			op = llvm::AtomicRMWInst::UMax;
			break;
		case RISCV_INS_AMOMIN_W:
		case RISCV_INS_AMOMIN_W_AQ:
		case RISCV_INS_AMOMIN_W_RL:
		case RISCV_INS_AMOMIN_W_AQ_RL:
		case RISCV_INS_AMOMIN_D:
		case RISCV_INS_AMOMIN_D_AQ:
		case RISCV_INS_AMOMIN_D_RL:
		case RISCV_INS_AMOMIN_D_AQ_RL:
			op = llvm::AtomicRMWInst::Min;
			break;
		case RISCV_INS_AMOMINU_W:
		case RISCV_INS_AMOMINU_W_AQ:
		case RISCV_INS_AMOMINU_W_RL:
		case RISCV_INS_AMOMINU_W_AQ_RL:
		case RISCV_INS_AMOMINU_D:
		case RISCV_INS_AMOMINU_D_AQ:
		case RISCV_INS_AMOMINU_D_RL:
		case RISCV_INS_AMOMINU_D_AQ_RL:
			op = llvm::AtomicRMWInst::UMin;
			break;
		default:
			translatePseudoAsmGeneric(i, ri, irb);
			return;
	}

	auto* elem = amoAccessType(i->id, irb);
	auto* addr = amoAddress(ri->operands[1], irb);
	auto* val = loadOp(ri->operands[2], irb);
	val = generateTypeConversion(irb, val, elem, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* ptr = intToPtr(irb, addr, elem);
	auto* old = irb.CreateAtomicRMW(op, ptr, val, llvm::MaybeAlign(), amoOrdering(i->id));
	attachPointeeType(old, elem);
	storeOp(ri->operands[0], old, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * LR.W/D: atomic load. No exclusive-monitor model (same as ARM LDXR).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateLr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	auto* elem = amoAccessType(i->id, irb);
	auto* addr = amoAddress(ri->operands[1], irb);
	auto* ld = loadIntPtr(irb, addr, elem);
	ld->setAtomic(amoOrdering(i->id));
	storeOp(ri->operands[0], ld, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
}

/**
 * SC.W/D: atomic store + status 0. No exclusive-monitor model (same as ARM STXR).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateSc(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ri, irb);
	auto* elem = amoAccessType(i->id, irb);
	auto* addr = amoAddress(ri->operands[1], irb);
	auto* val = loadOp(ri->operands[2], irb);
	val = generateTypeConversion(irb, val, elem, eOpConv::SEXT_TRUNC_OR_BITCAST);
	auto* st = storeIntPtr(irb, val, addr, elem);
	st->setAtomic(amoOrdering(i->id));
	storeOp(ri->operands[0], llvm::ConstantInt::get(getDefaultType(), 0), irb);
}

/**
 * CSRRW/CSRRS/CSRRC and immediates. Capstone SYSREG encodings only; unknown
 * encodings stay a named __asm_* call rather than inventing a CSR number.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateCsr(cs_insn* i, cs_riscv* ri, llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 1)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	uint16_t encoding = 0;
	bool haveCsr = false;
	int rdOp = -1;
	int srcOp = -1;
	for (int n = 0; n < ri->op_count; ++n)
	{
		auto& op = ri->operands[n];
		if (op.type == RISCV_OP_CSR)
		{
			encoding = op.csr;
			haveCsr = true;
		}
		else if (op.type == RISCV_OP_REG
				&& (op.reg == RISCV_REG_FFLAGS
						|| op.reg == RISCV_REG_FRM
						|| csrOperandReg(static_cast<uint16_t>(op.reg)) != RISCV_REG_INVALID))
		{
			encoding = static_cast<uint16_t>(op.reg);
			haveCsr = true;
		}
		else if (op.type == RISCV_OP_REG)
		{
			if (rdOp < 0)
			{
				rdOp = n;
			}
			else
			{
				srcOp = n;
			}
		}
		else if (op.type == RISCV_OP_IMM)
		{
			if (!haveCsr && n != 0)
			{
				encoding = static_cast<uint16_t>(op.imm);
				haveCsr = true;
			}
			else
			{
				srcOp = n;
			}
		}
	}

	uint64_t alias = i->is_alias ? i->alias_id : i->id;
	if (!haveCsr)
	{
		switch (alias)
		{
			case RISCV_INS_ALIAS_FRFLAGS:
			case RISCV_INS_ALIAS_FSFLAGS:
			case RISCV_INS_ALIAS_FSFLAGSI:
				encoding = RISCV_SYSREG_FFLAGS;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_FRRM:
			case RISCV_INS_ALIAS_FSRM:
			case RISCV_INS_ALIAS_FSRMI:
				encoding = RISCV_SYSREG_FRM;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_FRCSR:
			case RISCV_INS_ALIAS_FSCSR:
				encoding = RISCV_SYSREG_FCSR;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDCYCLE:
				encoding = RISCV_SYSREG_CYCLE;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDTIME:
				encoding = RISCV_SYSREG_TIME;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDINSTRET:
				encoding = RISCV_SYSREG_INSTRET;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDCYCLEH:
				encoding = RISCV_SYSREG_CYCLEH;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDTIMEH:
				encoding = RISCV_SYSREG_TIMEH;
				haveCsr = true;
				break;
			case RISCV_INS_ALIAS_RDINSTRETH:
				encoding = RISCV_SYSREG_INSTRETH;
				haveCsr = true;
				break;
			default:
				break;
		}
	}

	if (!haveCsr)
	{
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	if (csrOperandReg(encoding) == RISCV_REG_INVALID && encoding != RISCV_SYSREG_FCSR)
	{
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	llvm::Value* src = nullptr;
	if (srcOp >= 0)
	{
		src = loadOp(ri->operands[srcOp], irb);
	}
	else
	{
		src = llvm::ConstantInt::get(getDefaultType(), 0);
	}
	src = irb.CreateZExtOrTrunc(src, getDefaultType());

	bool isImm = i->id == RISCV_INS_CSRRWI || i->id == RISCV_INS_CSRRSI || i->id == RISCV_INS_CSRRCI
			|| i->id == RISCV_INS_ALIAS_CSRWI || i->id == RISCV_INS_ALIAS_CSRSI
			|| i->id == RISCV_INS_ALIAS_CSRCI
			|| alias == RISCV_INS_ALIAS_CSRWI || alias == RISCV_INS_ALIAS_CSRSI
			|| alias == RISCV_INS_ALIAS_CSRCI || alias == RISCV_INS_ALIAS_FSFLAGSI
			|| alias == RISCV_INS_ALIAS_FSRMI;
	bool isWrite = i->id == RISCV_INS_CSRRW || i->id == RISCV_INS_CSRRWI
			|| i->id == RISCV_INS_ALIAS_CSRW || i->id == RISCV_INS_ALIAS_CSRWI
			|| i->id == RISCV_INS_ALIAS_FSFLAGS || i->id == RISCV_INS_ALIAS_FSCSR
			|| i->id == RISCV_INS_ALIAS_FSRM || i->id == RISCV_INS_ALIAS_FSFLAGSI
			|| i->id == RISCV_INS_ALIAS_FSRMI
			|| alias == RISCV_INS_ALIAS_CSRW || alias == RISCV_INS_ALIAS_CSRWI
			|| alias == RISCV_INS_ALIAS_FSFLAGS || alias == RISCV_INS_ALIAS_FSCSR
			|| alias == RISCV_INS_ALIAS_FSRM || alias == RISCV_INS_ALIAS_FSFLAGSI
			|| alias == RISCV_INS_ALIAS_FSRMI;
	bool isSet = i->id == RISCV_INS_CSRRS || i->id == RISCV_INS_CSRRSI
			|| i->id == RISCV_INS_ALIAS_CSRS || i->id == RISCV_INS_ALIAS_CSRSI
			|| i->id == RISCV_INS_ALIAS_CSRR || i->id == RISCV_INS_ALIAS_FRFLAGS
			|| i->id == RISCV_INS_ALIAS_FRRM || i->id == RISCV_INS_ALIAS_FRCSR
			|| i->id == RISCV_INS_ALIAS_RDCYCLE || i->id == RISCV_INS_ALIAS_RDTIME
			|| i->id == RISCV_INS_ALIAS_RDINSTRET || i->id == RISCV_INS_ALIAS_RDCYCLEH
			|| i->id == RISCV_INS_ALIAS_RDTIMEH || i->id == RISCV_INS_ALIAS_RDINSTRETH
			|| alias == RISCV_INS_ALIAS_CSRS || alias == RISCV_INS_ALIAS_CSRSI
			|| alias == RISCV_INS_ALIAS_CSRR || alias == RISCV_INS_ALIAS_FRFLAGS
			|| alias == RISCV_INS_ALIAS_FRRM || alias == RISCV_INS_ALIAS_FRCSR
			|| alias == RISCV_INS_ALIAS_RDCYCLE || alias == RISCV_INS_ALIAS_RDTIME
			|| alias == RISCV_INS_ALIAS_RDINSTRET || alias == RISCV_INS_ALIAS_RDCYCLEH
			|| alias == RISCV_INS_ALIAS_RDTIMEH || alias == RISCV_INS_ALIAS_RDINSTRETH;
	bool isClear = i->id == RISCV_INS_CSRRC || i->id == RISCV_INS_CSRRCI
			|| i->id == RISCV_INS_ALIAS_CSRC || i->id == RISCV_INS_ALIAS_CSRCI
			|| alias == RISCV_INS_ALIAS_CSRC || alias == RISCV_INS_ALIAS_CSRCI;

	if (!isWrite && !isSet && !isClear)
	{
		isSet = true;
	}

	bool srcIsZero = false;
	if (srcOp < 0)
	{
		srcIsZero = true;
	}
	else if (isImm || ri->operands[srcOp].type == RISCV_OP_IMM)
	{
		srcIsZero = llvm::isa<llvm::ConstantInt>(src)
				&& llvm::cast<llvm::ConstantInt>(src)->isZero();
	}
	else if (ri->operands[srcOp].type == RISCV_OP_REG)
	{
		srcIsZero = ri->operands[srcOp].reg == RISCV_REG_X0;
	}

	bool rdIsZero = rdOp >= 0
			&& ri->operands[rdOp].type == RISCV_OP_REG
			&& ri->operands[rdOp].reg == RISCV_REG_X0;
	if (rdOp < 0)
	{
		rdIsZero = true;
	}

	llvm::Value* old = nullptr;
	if (!isWrite || !rdIsZero)
	{
		old = loadCsr(encoding, irb);
		if (old == nullptr)
		{
			translatePseudoAsmGeneric(i, ri, irb);
			return;
		}
		old = irb.CreateZExtOrTrunc(old, getDefaultType());
		if (rdOp >= 0)
		{
			storeOp(ri->operands[rdOp], old, irb);
		}
	}

	if (isWrite)
	{
		storeCsr(encoding, src, irb);
	}
	else if ((isSet || isClear) && !srcIsZero)
	{
		if (old == nullptr)
		{
			old = loadCsr(encoding, irb);
			if (old == nullptr)
			{
				translatePseudoAsmGeneric(i, ri, irb);
				return;
			}
			old = irb.CreateZExtOrTrunc(old, getDefaultType());
		}
		auto* next = isSet ? irb.CreateOr(old, src) : irb.CreateAnd(old, irb.CreateNot(src));
		storeCsr(encoding, next, irb);
	}
}

namespace {

llvm::Value* rotateLeft(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* amt)
{
	auto* ty = val->getType();
	unsigned w = ty->getIntegerBitWidth();
	amt = irb.CreateZExtOrTrunc(amt, ty);
	amt = irb.CreateAnd(amt, llvm::ConstantInt::get(ty, w - 1));
	auto* left = irb.CreateShl(val, amt);
	auto* ramt = irb.CreateAnd(
			irb.CreateSub(llvm::ConstantInt::get(ty, 0), amt),
			llvm::ConstantInt::get(ty, w - 1));
	return irb.CreateOr(left, irb.CreateLShr(val, ramt));
}

llvm::Value* rotateRight(
		llvm::IRBuilder<>& irb,
		llvm::Value* val,
		llvm::Value* amt)
{
	auto* ty = val->getType();
	unsigned w = ty->getIntegerBitWidth();
	amt = irb.CreateZExtOrTrunc(amt, ty);
	amt = irb.CreateAnd(amt, llvm::ConstantInt::get(ty, w - 1));
	auto* right = irb.CreateLShr(val, amt);
	auto* lamt = irb.CreateAnd(
			irb.CreateSub(llvm::ConstantInt::get(ty, 0), amt),
			llvm::ConstantInt::get(ty, w - 1));
	return irb.CreateOr(right, irb.CreateShl(val, lamt));
}

} // namespace

/**
 * Zbb ANDN/ORN/XNOR. RISC-V complements rs2: rd = rs1 & ~rs2 (ANDN),
 * rs1 | ~rs2 (ORN), rs1 ^ ~rs2 (XNOR). x86 BMI ANDN is the other way
 * around (~rs1 & rs2). Zcb C.NOT is rd = ~rd (unary compressed REG).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateAndn(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (i->id == RISCV_INS_C_NOT)
	{
		if (ri->op_count < 1)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, ri, irb);
			return;
		}
		unsigned src = ri->op_count >= 2 ? 1u : 0u;
		op1 = loadOp(ri->operands[src], irb);
		storeOp(ri->operands[0], irb.CreateNot(op1), irb);
		return;
	}

	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* n2 = irb.CreateNot(op2);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_ANDN:
			res = irb.CreateAnd(op1, n2);
			break;
		case RISCV_INS_ORN:
			res = irb.CreateOr(op1, n2);
			break;
		case RISCV_INS_XNOR:
			res = irb.CreateXor(op1, n2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateAndn().");
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zbb CLZ/CTZ/CPOP and W variants. llvm.ctlz/cttz with is_zero_poison=false
 * (zero input is defined: XLEN, or 32 for W). W forms use the low 32 bits
 * then sign-extend, same as translateOp32.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateBitCount(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	op1 = loadOp(ri->operands[1], irb);
	bool word = i->id == RISCV_INS_CLZW
			|| i->id == RISCV_INS_CTZW
			|| i->id == RISCV_INS_CPOPW;
	if (word)
	{
		op1 = narrowToWord(irb, op1);
	}
	else
	{
		op1 = irb.CreateZExtOrTrunc(op1, getDefaultType());
	}

	llvm::Intrinsic::ID iid = llvm::Intrinsic::ctlz;
	bool pop = false;
	switch (i->id)
	{
		case RISCV_INS_CLZ:
		case RISCV_INS_CLZW:
			iid = llvm::Intrinsic::ctlz;
			break;
		case RISCV_INS_CTZ:
		case RISCV_INS_CTZW:
			iid = llvm::Intrinsic::cttz;
			break;
		case RISCV_INS_CPOP:
		case RISCV_INS_CPOPW:
			iid = llvm::Intrinsic::ctpop;
			pop = true;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateBitCount().");
	}

	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid, op1->getType());
	llvm::Value* cnt = pop
			? irb.CreateCall(f, {op1})
			: irb.CreateCall(f, {op1, irb.getFalse()});
	if (word)
	{
		cnt = widenFromWord(irb, cnt);
	}
	storeOp(ri->operands[0], cnt, irb);
}

/**
 * Zbb MIN/MINU/MAX/MAXU: signed or unsigned select.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateMinMax(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::SEXT_TRUNC_OR_BITCAST);
	llvm::Value* cmp = nullptr;
	switch (i->id)
	{
		case RISCV_INS_MIN:
			cmp = irb.CreateICmpSLT(op1, op2);
			break;
		case RISCV_INS_MINU:
			cmp = irb.CreateICmpULT(op1, op2);
			break;
		case RISCV_INS_MAX:
			cmp = irb.CreateICmpSGT(op1, op2);
			break;
		case RISCV_INS_MAXU:
			cmp = irb.CreateICmpUGT(op1, op2);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateMinMax().");
	}
	storeOp(ri->operands[0], irb.CreateSelect(cmp, op1, op2), irb);
}

/**
 * Zbb ROL/ROR and W/immediate forms. W variants rotate i32 then sign-extend.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateRotate(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	bool word = i->id == RISCV_INS_ROLW
			|| i->id == RISCV_INS_RORW
			|| i->id == RISCV_INS_RORIW;
	bool left = i->id == RISCV_INS_ROL || i->id == RISCV_INS_ROLW;
	if (word)
	{
		op1 = narrowToWord(irb, op1);
		op2 = irb.CreateZExtOrTrunc(op2, irb.getInt32Ty());
	}
	llvm::Value* res = left ? rotateLeft(irb, op1, op2) : rotateRight(irb, op1, op2);
	if (word)
	{
		res = widenFromWord(irb, res);
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zba SH1ADD/SH2ADD/SH3ADD and *_UW, plus ADD.UW / ZEXT.W / C.ZEXT.W.
 * rd = rs2 + (zext?(rs1) << n). UW/ADD.UW/ZEXT.W zero-extend rs1[31:0] first.
 * Capstone 6 DETAIL_REAL reports C.ZEXT.W as a unary rd (alias ZEXT.W).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateShadd(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	bool zextw = i->id == RISCV_INS_ZEXT_W
			|| i->id == RISCV_INS_ALIAS_ZEXT_W
			|| i->id == RISCV_INS_C_ZEXT_W;
	if (ri->op_count < 1 || (ri->op_count < 2 && !zextw))
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned src = ri->op_count >= 2 ? 1u : 0u;
	op1 = loadOp(ri->operands[src], irb);
	op1 = irb.CreateZExtOrTrunc(op1, getDefaultType());
	llvm::Value* rs2 = nullptr;
	if (ri->op_count >= 3)
	{
		rs2 = loadOp(ri->operands[2], irb);
		rs2 = generateTypeConversion(irb, rs2, op1->getType(), eOpConv::SEXT_TRUNC_OR_BITCAST);
	}
	else
	{
		rs2 = llvm::ConstantInt::get(op1->getType(), 0);
	}

	bool uw = i->id == RISCV_INS_SH1ADD_UW
			|| i->id == RISCV_INS_SH2ADD_UW
			|| i->id == RISCV_INS_SH3ADD_UW
			|| i->id == RISCV_INS_ADD_UW
			|| i->id == RISCV_INS_ZEXT_W
			|| i->id == RISCV_INS_ALIAS_ZEXT_W
			|| i->id == RISCV_INS_C_ZEXT_W;
	if (uw)
	{
		op1 = irb.CreateZExt(narrowToWord(irb, op1), getDefaultType());
	}

	unsigned sh = 0;
	switch (i->id)
	{
		case RISCV_INS_SH1ADD:
		case RISCV_INS_SH1ADD_UW:
			sh = 1;
			break;
		case RISCV_INS_SH2ADD:
		case RISCV_INS_SH2ADD_UW:
			sh = 2;
			break;
		case RISCV_INS_SH3ADD:
		case RISCV_INS_SH3ADD_UW:
			sh = 3;
			break;
		case RISCV_INS_ADD_UW:
		case RISCV_INS_ZEXT_W:
		case RISCV_INS_ALIAS_ZEXT_W:
		case RISCV_INS_C_ZEXT_W:
			sh = 0;
			break;
		default:
			throw GenericError("Unhandled insn ID in translateShadd().");
	}
	if (sh != 0)
	{
		op1 = irb.CreateShl(op1, llvm::ConstantInt::get(op1->getType(), sh));
	}
	storeOp(ri->operands[0], irb.CreateAdd(rs2, op1), irb);
}

/**
 * Zbs BCLR/BSET/BINV/BEXT and *I. Index is rs2/imm masked to XLEN-1,
 * matching x86 BTR/BTS/BTC / BT (BEXT writes the bit to rd, not CF).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateZbs(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op2 = maskShiftAmount(irb, op1, op2);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_BCLR:
		case RISCV_INS_BCLRI:
		{
			auto* bit = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op2);
			res = irb.CreateAnd(op1, irb.CreateNot(bit));
			break;
		}
		case RISCV_INS_BSET:
		case RISCV_INS_BSETI:
		{
			auto* bit = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op2);
			res = irb.CreateOr(op1, bit);
			break;
		}
		case RISCV_INS_BINV:
		case RISCV_INS_BINVI:
		{
			auto* bit = irb.CreateShl(llvm::ConstantInt::get(op1->getType(), 1), op2);
			res = irb.CreateXor(op1, bit);
			break;
		}
		case RISCV_INS_BEXT:
		case RISCV_INS_BEXTI:
			res = irb.CreateAnd(
					irb.CreateLShr(op1, op2),
					llvm::ConstantInt::get(op1->getType(), 1));
			break;
		default:
			throw GenericError("Unhandled insn ID in translateZbs().");
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zba SLLI.UW: rd = zext(rs1[31:0]) << shamt (RV64).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateSlliUw(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	op1 = irb.CreateZExt(narrowToWord(irb, op1), getDefaultType());
	op2 = maskShiftAmount(irb, op1, op2);
	storeOp(ri->operands[0], irb.CreateShl(op1, op2), irb);
}

/**
 * Zicond CZERO.EQZ / CZERO.NEZ: rd = (rs2 == 0 / != 0) ? 0 : rs1.
 * Capstone has no CS_MODE_RISCV_ZICOND; the decoder enables it by default.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateCzero(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* zero = llvm::ConstantInt::get(op1->getType(), 0);
	llvm::Value* cond = nullptr;
	switch (i->id)
	{
		case RISCV_INS_CZERO_EQZ:
			cond = irb.CreateICmpEQ(op2, zero);
			break;
		case RISCV_INS_CZERO_NEZ:
			cond = irb.CreateICmpNE(op2, zero);
			break;
		default:
			throw GenericError("Unhandled insn ID in translateCzero().");
	}
	storeOp(ri->operands[0], irb.CreateSelect(cond, zero, op1), irb);
}

/**
 * Zbb ORC.B: each byte becomes 0x00 if it was zero, 0xFF otherwise.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateOrcB(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	auto* ty = getDefaultType();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	llvm::Value* res = llvm::ConstantInt::get(ty, 0);
	unsigned nbytes = getArchByteSize();
	for (unsigned b = 0; b < nbytes; ++b)
	{
		auto* byte = irb.CreateAnd(
				irb.CreateLShr(op1, llvm::ConstantInt::get(ty, b * 8)),
				llvm::ConstantInt::get(ty, 0xff));
		auto* nz = irb.CreateICmpNE(byte, llvm::ConstantInt::get(ty, 0));
		auto* fill = irb.CreateSelect(
				nz,
				llvm::ConstantInt::get(ty, 0xffull << (b * 8)),
				llvm::ConstantInt::get(ty, 0));
		res = irb.CreateOr(res, fill);
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zbb REV8: byte-swap XLEN.
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateRev8(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	op1 = irb.CreateZExtOrTrunc(op1, getDefaultType());
	op1 = irb.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, op1);
	storeOp(ri->operands[0], op1, irb);
}

/**
 * Zbkb BREV8: reverse the bits of each byte (bitreverse then bswap).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateBrev8(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ri, irb);
	op1 = loadOp(ri->operands[1], irb);
	auto* ty = getDefaultType();
	op1 = irb.CreateZExtOrTrunc(op1, ty);
	uint64_t m4 = isXlen64() ? 0x0F0F0F0F0F0F0F0Full : 0x0F0F0F0Full;
	uint64_t m2 = isXlen64() ? 0x3333333333333333ull : 0x33333333ull;
	uint64_t m1 = isXlen64() ? 0x5555555555555555ull : 0x55555555ull;
	auto* c4 = llvm::ConstantInt::get(ty, m4);
	auto* c2 = llvm::ConstantInt::get(ty, m2);
	auto* c1 = llvm::ConstantInt::get(ty, m1);
	auto* n4 = llvm::ConstantInt::get(ty, 4);
	auto* n2 = llvm::ConstantInt::get(ty, 2);
	auto* n1 = llvm::ConstantInt::get(ty, 1);
	op1 = irb.CreateOr(
			irb.CreateLShr(irb.CreateAnd(op1, irb.CreateShl(c4, n4)), n4),
			irb.CreateShl(irb.CreateAnd(op1, c4), n4));
	op1 = irb.CreateOr(
			irb.CreateLShr(irb.CreateAnd(op1, irb.CreateShl(c2, n2)), n2),
			irb.CreateShl(irb.CreateAnd(op1, c2), n2));
	op1 = irb.CreateOr(
			irb.CreateLShr(irb.CreateAnd(op1, irb.CreateShl(c1, n1)), n1),
			irb.CreateShl(irb.CreateAnd(op1, c1), n1));
	storeOp(ri->operands[0], op1, irb);
}

/**
 * Zbb SEXT.B/SEXT.H, ZEXT.H and Zcb C.SEXT.B/C.SEXT.H/C.ZEXT.H.
 * ZEXT.W / C.ZEXT.W is ADD.UW rd, rs, x0 (translateShadd).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translateSextZext(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	if (ri->op_count < 1)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, ri, irb);
		return;
	}

	unsigned src = ri->op_count >= 2 ? 1u : 0u;
	op1 = loadOp(ri->operands[src], irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_SEXT_B:
		case RISCV_INS_C_SEXT_B:
			res = irb.CreateSExt(irb.CreateTrunc(op1, irb.getInt8Ty()), getDefaultType());
			break;
		case RISCV_INS_SEXT_H:
		case RISCV_INS_C_SEXT_H:
			res = irb.CreateSExt(irb.CreateTrunc(op1, irb.getInt16Ty()), getDefaultType());
			break;
		case RISCV_INS_ZEXT_H:
		case RISCV_INS_C_ZEXT_H:
			res = irb.CreateZExt(irb.CreateTrunc(op1, irb.getInt16Ty()), getDefaultType());
			break;
		case RISCV_INS_ZEXT_W:
		case RISCV_INS_C_ZEXT_W:
			res = irb.CreateZExt(narrowToWord(irb, op1), getDefaultType());
			break;
		default:
			throw GenericError("Unhandled insn ID in translateSextZext().");
	}
	storeOp(ri->operands[0], res, irb);
}

/**
 * Zbkb PACK/PACKH/PACKW (x86 PUNPCKL* / MOVZX parity on GPRs).
 * PACK: rd = {rs2[XLEN/2-1:0], rs1[XLEN/2-1:0]}.
 * PACKH: rd[15:0] = {rs2[7:0], rs1[7:0]}, rest zero (zext).
 * PACKW (RV64): rd = sext_32({rs2[15:0], rs1[15:0]}).
 */
void Capstone2LlvmIrTranslatorRiscv_impl::translatePack(
		cs_insn* i,
		cs_riscv* ri,
		llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY_OR_TERNARY(i, ri, irb);
	std::tie(op1, op2) = loadAluSrc(ri, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	auto* ty = getDefaultType();
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case RISCV_INS_PACK:
		{
			unsigned halfBits = getArchByteSize() * 4;
			auto* halfTy = irb.getIntNTy(halfBits);
			auto* lo = irb.CreateZExt(irb.CreateTrunc(op1, halfTy), ty);
			auto* hi = irb.CreateShl(
					irb.CreateZExt(irb.CreateTrunc(op2, halfTy), ty),
					llvm::ConstantInt::get(ty, halfBits));
			res = irb.CreateOr(lo, hi);
			break;
		}
		case RISCV_INS_PACKH:
		{
			auto* lo = irb.CreateZExt(irb.CreateTrunc(op1, irb.getInt8Ty()), ty);
			auto* hi = irb.CreateShl(
					irb.CreateZExt(irb.CreateTrunc(op2, irb.getInt8Ty()), ty),
					llvm::ConstantInt::get(ty, 8));
			res = irb.CreateOr(lo, hi);
			break;
		}
		case RISCV_INS_PACKW:
		{
			auto* i32 = irb.getInt32Ty();
			auto* lo = irb.CreateZExt(irb.CreateTrunc(op1, irb.getInt16Ty()), i32);
			auto* hi = irb.CreateShl(
					irb.CreateZExt(irb.CreateTrunc(op2, irb.getInt16Ty()), i32),
					llvm::ConstantInt::get(i32, 16));
			res = widenFromWord(irb, irb.CreateOr(lo, hi));
			break;
		}
		default:
			throw GenericError("Unhandled insn ID in translatePack().");
	}
	storeOp(ri->operands[0], res, irb);
}

} // namespace capstone2llvmir
} // namespace retdec
