/**
 * @file src/capstone2llvmir/sparc/sparc.cpp
 * @brief SPARC implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include "capstone2llvmir/sparc/sparc_impl.h"

namespace retdec {
namespace capstone2llvmir {

namespace {

// ICC/XCC packed as NZVC in bits 3..0 of SPARC_REG_ICC / SPARC_REG_XCC.
constexpr unsigned kIccN = 3;
constexpr unsigned kIccZ = 2;
constexpr unsigned kIccV = 1;
constexpr unsigned kIccC = 0;

const uint32_t kOutRegs[8] = {
		SPARC_REG_O0, SPARC_REG_O1, SPARC_REG_O2, SPARC_REG_O3,
		SPARC_REG_O4, SPARC_REG_O5, SPARC_REG_SP, SPARC_REG_O7
};
const uint32_t kInRegs[8] = {
		SPARC_REG_I0, SPARC_REG_I1, SPARC_REG_I2, SPARC_REG_I3,
		SPARC_REG_I4, SPARC_REG_I5, SPARC_REG_FP, SPARC_REG_I7
};

} // anonymous namespace

Capstone2LlvmIrTranslatorSparc_impl::Capstone2LlvmIrTranslatorSparc_impl(
		llvm::Module* m,
		cs_mode basic,
		cs_mode extra)
		:
		Capstone2LlvmIrTranslator_impl(CS_ARCH_SPARC, basic, extra, m)
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

bool Capstone2LlvmIrTranslatorSparc_impl::isAllowedBasicMode(cs_mode m)
{
	// Capstone 5 SPARC accepts only endian + CS_MODE_V9. CS_MODE_32/64 are
	// x86/PPC bits and make cs_open() return CS_ERR_MODE.
	return m == CS_MODE_LITTLE_ENDIAN;
}

bool Capstone2LlvmIrTranslatorSparc_impl::isAllowedExtraMode(cs_mode m)
{
	auto withoutV9 = static_cast<cs_mode>(m & ~CS_MODE_V9);
	return withoutV9 == CS_MODE_LITTLE_ENDIAN
			|| withoutV9 == CS_MODE_BIG_ENDIAN;
}

uint32_t Capstone2LlvmIrTranslatorSparc_impl::getArchByteSize()
{
	return isV9() ? 8 : 4;
}

//
//==============================================================================
// Capstone related getters - from Capstone2LlvmIrTranslator.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorSparc_impl::hasDelaySlot(uint32_t id) const
{
	return getDelaySlot(id);
}

bool Capstone2LlvmIrTranslatorSparc_impl::hasDelaySlotTypical(uint32_t id) const
{
	return getDelaySlot(id);
}

bool Capstone2LlvmIrTranslatorSparc_impl::hasDelaySlotLikely(uint32_t id) const
{
	// SPARC annul (`,a` / SPARC_HINT_A) is a per-instruction hint, not a
	// distinct Capstone id. The decoder API is id-only, so annulled slots
	// are not modelled as MIPS-likely here. See docs/internal/wire-sparc.md.
	(void)id;
	return false;
}

/**
 * Every SPARC control-transfer instruction has a one-instruction delay slot.
 * Annulled delay slots are not distinguished at this layer (see likely).
 */
std::size_t Capstone2LlvmIrTranslatorSparc_impl::getDelaySlot(uint32_t id) const
{
	switch (id)
	{
		case SPARC_INS_B:
		case SPARC_INS_FB:
		case SPARC_INS_BRGEZ:
		case SPARC_INS_BRGZ:
		case SPARC_INS_BRLEZ:
		case SPARC_INS_BRLZ:
		case SPARC_INS_BRNZ:
		case SPARC_INS_BRZ:
		case SPARC_INS_CALL:
		case SPARC_INS_JMP:
		case SPARC_INS_JMPL:
		case SPARC_INS_RETT:
		case SPARC_INS_RET:
		case SPARC_INS_RETL:
			return 1;
		default:
			return 0;
	}
}

//
//==============================================================================
// Pure virtual methods from Capstone2LlvmIrTranslator_impl
//==============================================================================
//

void Capstone2LlvmIrTranslatorSparc_impl::generateEnvironmentArchSpecific()
{
	// Nothing.
}

void Capstone2LlvmIrTranslatorSparc_impl::generateDataLayout()
{
	if (isV9())
	{
		_module->setDataLayout(isBigEndian()
				? "E-p:64:64:64-f80:32:32"
				: "e-p:64:64:64-f80:32:32");
	}
	else
	{
		_module->setDataLayout(isBigEndian()
				? "E-p:32:32:32-f80:32:32"
				: "e-p:32:32:32-f80:32:32");
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::generateRegisters()
{
	for (auto& p : _reg2type)
	{
		createRegister(p.first, _regLt);
	}
}

uint32_t Capstone2LlvmIrTranslatorSparc_impl::getCarryRegister()
{
	return SPARC_REG_INVALID;
}

void Capstone2LlvmIrTranslatorSparc_impl::translateInstruction(
		cs_insn* i,
		llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_sparc* si = &d->sparc;

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
// SPARC-specific methods.
//==============================================================================
//

bool Capstone2LlvmIrTranslatorSparc_impl::isV9() const
{
	return (_extraMode & CS_MODE_V9) != 0;
}

bool Capstone2LlvmIrTranslatorSparc_impl::isBigEndian() const
{
	return (_extraMode & CS_MODE_BIG_ENDIAN) != 0;
}

bool Capstone2LlvmIrTranslatorSparc_impl::isZeroRegister(uint32_t r) const
{
	return r == SPARC_REG_G0;
}

bool Capstone2LlvmIrTranslatorSparc_impl::isGeneralPurposeRegister(uint32_t r) const
{
	return (SPARC_REG_G0 <= r && r <= SPARC_REG_G7)
			|| (SPARC_REG_O0 <= r && r <= SPARC_REG_O5)
			|| r == SPARC_REG_O7
			|| r == SPARC_REG_SP
			|| (SPARC_REG_L0 <= r && r <= SPARC_REG_L7)
			|| (SPARC_REG_I0 <= r && r <= SPARC_REG_I5)
			|| r == SPARC_REG_I7
			|| r == SPARC_REG_FP;
}

llvm::Value* Capstone2LlvmIrTranslatorSparc_impl::loadRegister(
		uint32_t r,
		llvm::IRBuilder<>& irb,
		llvm::Type* dstType,
		eOpConv ct)
{
	if (r == SPARC_REG_INVALID)
	{
		return nullptr;
	}

	if (isZeroRegister(r))
	{
		auto* z = llvm::ConstantInt::get(getDefaultType(), 0);
		return generateTypeConversion(irb, z, dstType, ct == eOpConv::THROW
				? eOpConv::ZEXT_TRUNC_OR_BITCAST
				: ct);
	}

	llvm::Value* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("loadRegister() unhandled reg.");
	}

	llvmReg = generateTypeConversion(irb, llvmReg, dstType, ct);

	return createLoad(irb, llvmReg);
}

llvm::Value* Capstone2LlvmIrTranslatorSparc_impl::loadOpAddress(
		cs_sparc_op& op,
		llvm::IRBuilder<>& irb)
{
	if (op.type == SPARC_OP_MEM)
	{
		return loadOp(op, irb, getDefaultType(), /*lea=*/true);
	}
	return loadOp(op, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorSparc_impl::loadOp(
		cs_sparc_op& op,
		llvm::IRBuilder<>& irb,
		llvm::Type* ty,
		bool lea)
{
	switch (op.type)
	{
		case SPARC_OP_REG:
		{
			auto* r = loadRegister(op.reg, irb);
			return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
		case SPARC_OP_IMM:
		{
			auto* t = getDefaultType();
			return llvm::ConstantInt::get(t, llvm::APInt(
					t->getIntegerBitWidth(),
					static_cast<uint64_t>(op.imm),
					false,
					/*implicitTrunc=*/true));
		}
		case SPARC_OP_MEM:
		{
			auto* t = getDefaultType();
			llvm::Value* addr = nullptr;

			auto* baseR = loadRegister(op.mem.base, irb);
			auto* idxR = loadRegister(op.mem.index, irb);
			llvm::Value* disp = llvm::ConstantInt::getSigned(t, op.mem.disp);

			if (baseR != nullptr)
			{
				addr = baseR;
			}
			if (idxR != nullptr)
			{
				addr = addr ? irb.CreateAdd(addr, idxR) : idxR;
			}
			if (op.mem.disp != 0 || addr == nullptr)
			{
				if (addr)
				{
					disp = irb.CreateSExtOrTrunc(disp, addr->getType());
					addr = irb.CreateAdd(addr, disp);
				}
				else
				{
					addr = disp;
				}
			}

			if (lea)
			{
				return addr;
			}

			auto* lty = ty ? ty : t;
			return loadIntPtr(irb, addr, lty);
		}
		case SPARC_OP_INVALID:
		default:
		{
			return llvm::UndefValue::get(ty ? ty : getDefaultType());
		}
	}
}

llvm::StoreInst* Capstone2LlvmIrTranslatorSparc_impl::storeRegister(
		uint32_t r,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	if (r == SPARC_REG_INVALID || isZeroRegister(r))
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

llvm::Instruction* Capstone2LlvmIrTranslatorSparc_impl::storeOp(
		cs_sparc_op& op,
		llvm::Value* val,
		llvm::IRBuilder<>& irb,
		eOpConv ct)
{
	switch (op.type)
	{
		case SPARC_OP_REG:
		{
			return storeRegister(op.reg, val, irb, ct);
		}
		case SPARC_OP_MEM:
		{
			auto* addr = loadOp(op, irb, getDefaultType(), /*lea=*/true);
			return storeIntPtr(irb, val, addr, val->getType());
		}
		case SPARC_OP_IMM:
		case SPARC_OP_INVALID:
		default:
		{
			throw GenericError("should not be possible");
		}
	}
}

bool Capstone2LlvmIrTranslatorSparc_impl::isOperandRegister(cs_sparc_op& op)
{
	return op.type == SPARC_OP_REG;
}

/**
 * SPARC assembler order is @c rs1, rs2_or_imm [, rd]. Capstone fills detail
 * operands in print order, so dest is last when present.
 */
std::pair<llvm::Value*, llvm::Value*> Capstone2LlvmIrTranslatorSparc_impl::loadOpRs1Rs2(
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		return {
				llvm::ConstantInt::get(getDefaultType(), 0),
				llvm::ConstantInt::get(getDefaultType(), 0)};
	}

	auto* a = loadOp(si->operands[0], irb);
	auto* b = loadOp(si->operands[1], irb);
	if (a->getType() != b->getType())
	{
		b = generateTypeConversion(
				irb,
				b,
				a->getType(),
				eOpConv::SEXT_TRUNC_OR_BITCAST);
	}
	return {a, b};
}

cs_sparc_op* Capstone2LlvmIrTranslatorSparc_impl::destOperand(cs_sparc* si)
{
	if (si->op_count < 3)
	{
		return nullptr;
	}
	return &si->operands[si->op_count - 1];
}

void Capstone2LlvmIrTranslatorSparc_impl::storeIcc(
		llvm::Value* result,
		llvm::Value* op0,
		llvm::Value* op1,
		bool isSub,
		llvm::IRBuilder<>& irb)
{
	auto* ty = result->getType();
	auto* zero = llvm::ConstantInt::get(ty, 0);

	llvm::Value* n = irb.CreateICmpSLT(result, zero);
	llvm::Value* z = irb.CreateICmpEQ(result, zero);
	llvm::Value* v = isSub
			? generateOverflowSub(result, op0, op1, irb)
			: generateOverflowAdd(result, op0, op1, irb);
	llvm::Value* c = isSub
			? generateBorrowSub(op0, op1, irb)
			: generateCarryAdd(result, op0, irb);

	auto pack = [&](llvm::Value* nbit, llvm::Value* zbit, llvm::Value* vbit, llvm::Value* cbit)
	{
		auto* i32 = irb.getInt32Ty();
		auto zext = [&](llvm::Value* b)
		{
			return irb.CreateZExt(b, i32);
		};
		auto* packed = zext(cbit);
		packed = irb.CreateOr(packed, irb.CreateShl(zext(vbit), kIccV));
		packed = irb.CreateOr(packed, irb.CreateShl(zext(zbit), kIccZ));
		packed = irb.CreateOr(packed, irb.CreateShl(zext(nbit), kIccN));
		return packed;
	};

	if (isV9() && ty->getIntegerBitWidth() == 64)
	{
		auto* lo = irb.CreateTrunc(result, irb.getInt32Ty());
		auto* op0lo = irb.CreateTrunc(op0, irb.getInt32Ty());
		auto* op1lo = irb.CreateTrunc(op1, irb.getInt32Ty());
		auto* n32 = irb.CreateICmpSLT(lo, llvm::ConstantInt::get(irb.getInt32Ty(), 0));
		auto* z32 = irb.CreateICmpEQ(lo, llvm::ConstantInt::get(irb.getInt32Ty(), 0));
		auto* v32 = isSub
				? generateOverflowSub(lo, op0lo, op1lo, irb)
				: generateOverflowAdd(lo, op0lo, op1lo, irb);
		auto* c32 = isSub
				? generateBorrowSub(op0lo, op1lo, irb)
				: generateCarryAdd(lo, op0lo, irb);
		storeRegister(SPARC_REG_ICC, pack(n32, z32, v32, c32), irb);
		storeRegister(SPARC_REG_XCC, pack(n, z, v, c), irb);
	}
	else
	{
		auto* packed = pack(n, z, v, c);
		storeRegister(SPARC_REG_ICC, packed, irb);
		if (isV9())
		{
			storeRegister(SPARC_REG_XCC, packed, irb);
		}
	}
}

llvm::Value* Capstone2LlvmIrTranslatorSparc_impl::generateIccCondition(
		sparc_cc cc,
		uint32_t ccReg,
		llvm::IRBuilder<>& irb)
{
	auto* icc = loadRegister(ccReg, irb);
	auto* i32 = irb.getInt32Ty();
	icc = irb.CreateZExtOrTrunc(icc, i32);

	auto bit = [&](unsigned b)
	{
		auto* sh = irb.CreateLShr(icc, llvm::ConstantInt::get(i32, b));
		auto* masked = irb.CreateAnd(sh, llvm::ConstantInt::get(i32, 1));
		return irb.CreateICmpNE(masked, llvm::ConstantInt::get(i32, 0));
	};

	auto* n = bit(kIccN);
	auto* z = bit(kIccZ);
	auto* v = bit(kIccV);
	auto* c = bit(kIccC);
	auto* nv = irb.CreateXor(n, v);

	switch (cc)
	{
		case SPARC_CC_ICC_A:   return irb.getTrue();
		case SPARC_CC_ICC_N:   return irb.getFalse();
		case SPARC_CC_ICC_NE:  return irb.CreateNot(z);
		case SPARC_CC_ICC_E:   return z;
		case SPARC_CC_ICC_G:   return irb.CreateNot(irb.CreateOr(z, nv));
		case SPARC_CC_ICC_LE:  return irb.CreateOr(z, nv);
		case SPARC_CC_ICC_GE:  return irb.CreateNot(nv);
		case SPARC_CC_ICC_L:   return nv;
		case SPARC_CC_ICC_GU:  return irb.CreateAnd(irb.CreateNot(c), irb.CreateNot(z));
		case SPARC_CC_ICC_LEU: return irb.CreateOr(c, z);
		case SPARC_CC_ICC_CC:  return irb.CreateNot(c);
		case SPARC_CC_ICC_CS:  return c;
		case SPARC_CC_ICC_POS: return irb.CreateNot(n);
		case SPARC_CC_ICC_NEG: return n;
		case SPARC_CC_ICC_VC:  return irb.CreateNot(v);
		case SPARC_CC_ICC_VS:  return v;
		default:
			return irb.getTrue();
	}
}

uint32_t Capstone2LlvmIrTranslatorSparc_impl::conditionRegister(cs_sparc* si) const
{
	for (uint8_t k = 0; k < si->op_count; ++k)
	{
		if (si->operands[k].type == SPARC_OP_REG
				&& si->operands[k].reg == SPARC_REG_XCC)
		{
			return SPARC_REG_XCC;
		}
	}
	return SPARC_REG_ICC;
}

void Capstone2LlvmIrTranslatorSparc_impl::copyOutsToIns(llvm::IRBuilder<>& irb)
{
	llvm::Value* vals[8];
	for (unsigned k = 0; k < 8; ++k)
	{
		vals[k] = loadRegister(kOutRegs[k], irb);
	}
	for (unsigned k = 0; k < 8; ++k)
	{
		storeRegister(kInRegs[k], vals[k], irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::copyInsToOuts(llvm::IRBuilder<>& irb)
{
	llvm::Value* vals[8];
	for (unsigned k = 0; k < 8; ++k)
	{
		vals[k] = loadRegister(kInRegs[k], irb);
	}
	for (unsigned k = 0; k < 8; ++k)
	{
		storeRegister(kOutRegs[k], vals[k], irb);
	}
}

//
//==============================================================================
// SPARC instruction translation methods.
//==============================================================================
//

void Capstone2LlvmIrTranslatorSparc_impl::translateAdd(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
	auto* add = irb.CreateAdd(op0, op1);

	if (i->id == SPARC_INS_ADDX
			|| i->id == SPARC_INS_ADDXCC
			|| i->id == SPARC_INS_ADDXC
			|| i->id == SPARC_INS_ADDXCCC)
	{
		auto* icc = loadRegister(SPARC_REG_ICC, irb);
		auto* c = irb.CreateAnd(icc, llvm::ConstantInt::get(icc->getType(), 1));
		c = irb.CreateZExtOrTrunc(c, add->getType());
		add = irb.CreateAdd(add, c);
	}

	if (auto* d = destOperand(si))
	{
		storeOp(*d, add, irb);
	}

	if (i->id == SPARC_INS_ADDCC
			|| i->id == SPARC_INS_ADDXCC
			|| i->id == SPARC_INS_ADDXCCC)
	{
		storeIcc(add, op0, op1, /*isSub=*/false, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateAnd(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
	auto* a = irb.CreateAnd(op0, op1);
	if (auto* d = destOperand(si))
	{
		storeOp(*d, a, irb);
	}
	if (i->id == SPARC_INS_ANDCC)
	{
		storeIcc(a, op0, op1, /*isSub=*/false, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateOr(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
	auto* o = irb.CreateOr(op0, op1);
	if (auto* d = destOperand(si))
	{
		storeOp(*d, o, irb);
	}
	if (i->id == SPARC_INS_ORCC)
	{
		storeIcc(o, op0, op1, /*isSub=*/false, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateXor(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
	auto* x = irb.CreateXor(op0, op1);
	if (auto* d = destOperand(si))
	{
		storeOp(*d, x, irb);
	}
	if (i->id == SPARC_INS_XORCC)
	{
		storeIcc(x, op0, op1, /*isSub=*/false, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateSub(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
	auto* sub = irb.CreateSub(op0, op1);

	if (i->id == SPARC_INS_SUBX || i->id == SPARC_INS_SUBXCC)
	{
		auto* icc = loadRegister(SPARC_REG_ICC, irb);
		auto* c = irb.CreateAnd(icc, llvm::ConstantInt::get(icc->getType(), 1));
		c = irb.CreateZExtOrTrunc(c, sub->getType());
		sub = irb.CreateSub(sub, c);
	}

	if (auto* d = destOperand(si))
	{
		storeOp(*d, sub, irb);
	}

	if (i->id == SPARC_INS_SUBCC || i->id == SPARC_INS_SUBXCC)
	{
		storeIcc(sub, op0, op1, /*isSub=*/true, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateCmp(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
	auto* sub = irb.CreateSub(op0, op1);
	storeIcc(sub, op0, op1, /*isSub=*/true, irb);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateSethi(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	op0 = loadOp(si->operands[0], irb);
	auto* i32 = irb.getInt32Ty();
	op0 = irb.CreateZExtOrTrunc(op0, i32);
	// Capstone reports the 22-bit field. SETHI writes imm22 << 10.
	// V9 zeroes the high 32 bits of rd.
	auto* hi = irb.CreateShl(op0, llvm::ConstantInt::get(i32, 10));
	storeOp(si->operands[1], hi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateMov(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	op0 = loadOp(si->operands[0], irb);
	storeOp(si->operands[1], op0, irb);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateNop(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	(void)i;
	(void)si;
	(void)irb;
}

void Capstone2LlvmIrTranslatorSparc_impl::translateLoad(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	llvm::Type* ty = nullptr;
	eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
	switch (i->id)
	{
		case SPARC_INS_LDSB: ty = irb.getInt8Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDUB: ty = irb.getInt8Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDSH: ty = irb.getInt16Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDUH: ty = irb.getInt16Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDSW: ty = irb.getInt32Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LD:   ty = irb.getInt32Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDX:  ty = irb.getInt64Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		default:
			throw GenericError("Unhandled insn ID in translateLoad().");
	}

	auto* mem = loadOp(si->operands[0], irb, ty);
	storeOp(si->operands[1], mem, irb, ct);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateStore(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->op_count < 2)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case SPARC_INS_STB: ty = irb.getInt8Ty(); break;
		case SPARC_INS_STH: ty = irb.getInt16Ty(); break;
		case SPARC_INS_ST:  ty = irb.getInt32Ty(); break;
		case SPARC_INS_STX: ty = irb.getInt64Ty(); break;
		default:
			throw GenericError("Unhandled insn ID in translateStore().");
	}

	op0 = loadOp(si->operands[0], irb);
	op0 = irb.CreateZExtOrTrunc(op0, ty);
	storeOp(si->operands[1], op0, irb);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateB(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	llvm::Value* target = nullptr;
	for (int k = static_cast<int>(si->op_count) - 1; k >= 0; --k)
	{
		if (si->operands[k].type == SPARC_OP_IMM
				|| si->operands[k].type == SPARC_OP_MEM
				|| si->operands[k].type == SPARC_OP_REG)
		{
			if (si->operands[k].type == SPARC_OP_REG
					&& (si->operands[k].reg == SPARC_REG_ICC
						|| si->operands[k].reg == SPARC_REG_XCC))
			{
				continue;
			}
			target = loadOpAddress(si->operands[k], irb);
			break;
		}
	}
	if (target == nullptr)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	if (si->cc == SPARC_CC_ICC_A || si->cc == SPARC_CC_INVALID)
	{
		generateBranchFunctionCall(irb, target);
		return;
	}

	auto* cond = generateIccCondition(si->cc, conditionRegister(si), irb);
	generateCondBranchFunctionCall(irb, cond, target);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateCall(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	storeRegister(SPARC_REG_O7, getThisInsnAddress(i), irb);

	llvm::Value* target = nullptr;
	if (si->op_count >= 1)
	{
		target = loadOpAddress(si->operands[0], irb);
	}
	if (target == nullptr)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}
	generateCallFunctionCall(irb, target);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateJmpl(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	llvm::Value* target = nullptr;
	cs_sparc_op* rd = nullptr;

	if (si->op_count == 0)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	if (i->id == SPARC_INS_JMP)
	{
		target = loadOpAddress(si->operands[0], irb);
		generateBranchFunctionCall(irb, target);
		return;
	}

	// jmpl addr, rd  (print order: mem/addr then rd) or 3-op rs1,rs2,rd.
	if (si->op_count >= 2 && si->operands[si->op_count - 1].type == SPARC_OP_REG)
	{
		rd = &si->operands[si->op_count - 1];
	}

	if (si->op_count >= 3 && si->operands[0].type != SPARC_OP_MEM)
	{
		std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
		target = irb.CreateAdd(op0, op1);
	}
	else
	{
		target = loadOpAddress(si->operands[0], irb);
	}

	if (rd)
	{
		storeRegister(rd->reg, getThisInsnAddress(i), irb);
	}

	generateBranchFunctionCall(irb, target);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateRet(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	(void)si;
	uint32_t link = (i->id == SPARC_INS_RETL) ? SPARC_REG_O7 : SPARC_REG_I7;
	auto* t = loadRegister(link, irb);
	t = irb.CreateAdd(t, llvm::ConstantInt::get(t->getType(), 8));
	generateReturnFunctionCall(irb, t);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateRett(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	llvm::Value* target = nullptr;
	if (si->op_count >= 2 && si->operands[0].type != SPARC_OP_MEM)
	{
		std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
		target = irb.CreateAdd(op0, op1);
	}
	else if (si->op_count >= 1)
	{
		target = loadOpAddress(si->operands[0], irb);
	}
	if (target == nullptr)
	{
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	copyInsToOuts(irb);
	generateReturnFunctionCall(irb, target);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateSave(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	(void)i;
	llvm::Value* result = nullptr;
	cs_sparc_op* rd = nullptr;
	if (si->op_count >= 2)
	{
		std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
		result = irb.CreateAdd(op0, op1);
		rd = destOperand(si);
	}

	copyOutsToIns(irb);

	if (result && rd)
	{
		storeOp(*rd, result, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateRestore(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	(void)i;
	llvm::Value* result = nullptr;
	cs_sparc_op* rd = nullptr;
	if (si->op_count >= 2)
	{
		std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
		result = irb.CreateAdd(op0, op1);
		rd = destOperand(si);
	}

	copyInsToOuts(irb);

	if (result && rd)
	{
		storeOp(*rd, result, irb);
	}
}

} // namespace capstone2llvmir
} // namespace retdec
