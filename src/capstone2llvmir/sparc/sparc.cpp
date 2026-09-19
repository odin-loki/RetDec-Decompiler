/**
 * @file src/capstone2llvmir/sparc/sparc.cpp
 * @brief SPARC implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#include <cstring>

#include <llvm/IR/Intrinsics.h>

#include "capstone2llvmir/sparc/sparc_impl.h"

namespace retdec {
namespace capstone2llvmir {

namespace {

// ICC packed as NZVC in bits 3..0 of SPARC_REG_ICC. V9 XCC is bits 7..4
// of the same register (Capstone 6 has no SPARC_REG_XCC).
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
		case SPARC_INS_BR:
		case SPARC_INS_BRGEZ:
		case SPARC_INS_BRGZ:
		case SPARC_INS_BRLEZ:
		case SPARC_INS_BRLZ:
		case SPARC_INS_BRNZ:
		case SPARC_INS_BRZ:
		case SPARC_INS_CALL:
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

	// Capstone 6 auto-sync keeps the real id (JMPL/SUBCC/BR) and puts
	// retl/cmp/BRcc in alias_id. Prefer a dedicated alias translator.
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

bool Capstone2LlvmIrTranslatorSparc_impl::isFpSingleRegister(uint32_t r) const
{
	return SPARC_REG_F0 <= r && r <= SPARC_REG_F31;
}

bool Capstone2LlvmIrTranslatorSparc_impl::isFpDoubleRegister(uint32_t r) const
{
	return SPARC_REG_D0 <= r && r <= SPARC_REG_D31;
}

bool Capstone2LlvmIrTranslatorSparc_impl::isFccRegister(uint32_t r) const
{
	return SPARC_REG_FCC0 <= r && r <= SPARC_REG_FCC3;
}

bool Capstone2LlvmIrTranslatorSparc_impl::isGprPairRegister(uint32_t r) const
{
	return SPARC_REG_G0_G1 <= r && r <= SPARC_REG_O6_O7;
}

uint32_t Capstone2LlvmIrTranslatorSparc_impl::gprPairEven(uint32_t r) const
{
	static const uint32_t evens[] = {
			SPARC_REG_G0, SPARC_REG_G2, SPARC_REG_G4, SPARC_REG_G6,
			SPARC_REG_I0, SPARC_REG_I2, SPARC_REG_I4, SPARC_REG_I6,
			SPARC_REG_L0, SPARC_REG_L2, SPARC_REG_L4, SPARC_REG_L6,
			SPARC_REG_O0, SPARC_REG_O2, SPARC_REG_O4, SPARC_REG_O6,
	};
	if (!isGprPairRegister(r))
	{
		return r;
	}
	return evens[r - SPARC_REG_G0_G1];
}

uint32_t Capstone2LlvmIrTranslatorSparc_impl::nextGpr(uint32_t r) const
{
	return r + 1;
}

uint32_t Capstone2LlvmIrTranslatorSparc_impl::fccFromField(sparc_cc_field field) const
{
	switch (field)
	{
		case SPARC_CC_FIELD_FCC1: return SPARC_REG_FCC1;
		case SPARC_CC_FIELD_FCC2: return SPARC_REG_FCC2;
		case SPARC_CC_FIELD_FCC3: return SPARC_REG_FCC3;
		case SPARC_CC_FIELD_FCC0:
		default:
			return SPARC_REG_FCC0;
	}
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

	if (isGprPairRegister(r))
	{
		uint32_t even = gprPairEven(r);
		auto* hi = loadRegister(even, irb);
		auto* lo = loadRegister(nextGpr(even), irb);
		auto* i64 = irb.getInt64Ty();
		hi = irb.CreateZExt(irb.CreateZExtOrTrunc(hi, irb.getInt32Ty()), i64);
		lo = irb.CreateZExt(irb.CreateZExtOrTrunc(lo, irb.getInt32Ty()), i64);
		auto* packed = irb.CreateOr(
				irb.CreateShl(hi, llvm::ConstantInt::get(i64, 32)),
				lo);
		return generateTypeConversion(
				irb,
				packed,
				dstType,
				ct == eOpConv::THROW ? eOpConv::ZEXT_TRUNC_OR_BITCAST : ct);
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

	if (isGprPairRegister(r))
	{
		uint32_t even = gprPairEven(r);
		auto* i64 = irb.getInt64Ty();
		val = generateTypeConversion(irb, val, i64, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		auto* hi = irb.CreateTrunc(
				irb.CreateLShr(val, llvm::ConstantInt::get(i64, 32)),
				irb.getInt32Ty());
		auto* lo = irb.CreateTrunc(val, irb.getInt32Ty());
		storeRegister(even, hi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return storeRegister(nextGpr(even), lo, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}

	auto* llvmReg = getRegister(r);
	if (llvmReg == nullptr)
	{
		throw GenericError("storeRegister() unhandled reg.");
	}
	if (llvmReg->getValueType()->isFloatingPointTy()
			&& (ct == eOpConv::SEXT_TRUNC_OR_BITCAST
				|| ct == eOpConv::ZEXT_TRUNC_OR_BITCAST
				|| ct == eOpConv::THROW))
	{
		ct = eOpConv::FPCAST_OR_BITCAST;
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
		auto ct = (a->getType()->isFloatingPointTy() || b->getType()->isFloatingPointTy())
				? eOpConv::FPCAST_OR_BITCAST
				: eOpConv::SEXT_TRUNC_OR_BITCAST;
		b = generateTypeConversion(irb, b, a->getType(), ct);
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

cs_sparc_op* Capstone2LlvmIrTranslatorSparc_impl::fpDestOperand(cs_sparc* si)
{
	for (int k = static_cast<int>(si->op_count) - 1; k >= 0; --k)
	{
		if (si->operands[k].type != SPARC_OP_REG)
		{
			continue;
		}
		auto r = si->operands[k].reg;
		if (isFpSingleRegister(r) || isFpDoubleRegister(r) || isFccRegister(r)
				|| isGprPairRegister(r) || isGeneralPurposeRegister(r))
		{
			return &si->operands[k];
		}
	}
	return destOperand(si);
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
		auto* icc = pack(n32, z32, v32, c32);
		auto* xcc = pack(n, z, v, c);
		storeRegister(SPARC_REG_ICC, irb.CreateOr(icc, irb.CreateShl(xcc, 4)), irb);
	}
	else
	{
		auto* packed = pack(n, z, v, c);
		if (isV9())
		{
			packed = irb.CreateOr(packed, irb.CreateShl(packed, 4));
		}
		storeRegister(SPARC_REG_ICC, packed, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::storeFcc(
		llvm::Value* a,
		llvm::Value* b,
		uint32_t fccReg,
		llvm::IRBuilder<>& irb)
{
	// SPARC fcc: 0=eq, 1=lt, 2=gt, 3=unordered.
	auto* un = irb.CreateFCmpUNO(a, b);
	auto* lt = irb.CreateFCmpOLT(a, b);
	auto* gt = irb.CreateFCmpOGT(a, b);
	auto* i32 = irb.getInt32Ty();
	auto* three = llvm::ConstantInt::get(i32, 3);
	auto* two = llvm::ConstantInt::get(i32, 2);
	auto* one = llvm::ConstantInt::get(i32, 1);
	auto* zero = llvm::ConstantInt::get(i32, 0);
	llvm::Value* code = irb.CreateSelect(un, three, irb.CreateSelect(lt, one, irb.CreateSelect(gt, two, zero)));
	storeRegister(fccReg, code, irb);
}

llvm::Value* Capstone2LlvmIrTranslatorSparc_impl::generateIccCondition(
		sparc_cc cc,
		uint32_t ccReg,
		unsigned nibbleShift,
		llvm::IRBuilder<>& irb)
{
	auto* icc = loadRegister(ccReg, irb);
	auto* i32 = irb.getInt32Ty();
	icc = irb.CreateZExtOrTrunc(icc, i32);
	if (nibbleShift != 0)
	{
		icc = irb.CreateLShr(icc, llvm::ConstantInt::get(i32, nibbleShift));
	}

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

llvm::Value* Capstone2LlvmIrTranslatorSparc_impl::generateFccCondition(
		sparc_cc cc,
		uint32_t fccReg,
		llvm::IRBuilder<>& irb)
{
	auto* fcc = loadRegister(fccReg, irb);
	auto* i32 = irb.getInt32Ty();
	fcc = irb.CreateZExtOrTrunc(fcc, i32);
	auto* e = irb.CreateICmpEQ(fcc, llvm::ConstantInt::get(i32, 0));
	auto* l = irb.CreateICmpEQ(fcc, llvm::ConstantInt::get(i32, 1));
	auto* g = irb.CreateICmpEQ(fcc, llvm::ConstantInt::get(i32, 2));
	auto* u = irb.CreateICmpEQ(fcc, llvm::ConstantInt::get(i32, 3));

	switch (cc)
	{
		case SPARC_CC_FCC_A:   return irb.getTrue();
		case SPARC_CC_FCC_N:   return irb.getFalse();
		case SPARC_CC_FCC_U:   return u;
		case SPARC_CC_FCC_G:   return g;
		case SPARC_CC_FCC_UG:  return irb.CreateOr(u, g);
		case SPARC_CC_FCC_L:   return l;
		case SPARC_CC_FCC_UL:  return irb.CreateOr(u, l);
		case SPARC_CC_FCC_LG:  return irb.CreateOr(l, g);
		case SPARC_CC_FCC_NE:  return irb.CreateNot(e);
		case SPARC_CC_FCC_E:   return e;
		case SPARC_CC_FCC_UE:  return irb.CreateOr(u, e);
		case SPARC_CC_FCC_GE:  return irb.CreateOr(g, e);
		case SPARC_CC_FCC_UGE: return irb.CreateNot(l);
		case SPARC_CC_FCC_LE:  return irb.CreateOr(l, e);
		case SPARC_CC_FCC_ULE: return irb.CreateNot(g);
		case SPARC_CC_FCC_O:   return irb.CreateNot(u);
		default:
			return irb.getTrue();
	}
}

llvm::Value* Capstone2LlvmIrTranslatorSparc_impl::generateCondition(
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	if (si->cc >= SPARC_CC_FCC_BEGIN && si->cc < SPARC_CC_CPCC_BEGIN)
	{
		return generateFccCondition(si->cc, fccFromField(si->cc_field), irb);
	}

	unsigned shift = (si->cc_field == SPARC_CC_FIELD_XCC) ? 4u : 0u;
	return generateIccCondition(si->cc, SPARC_REG_ICC, shift, irb);
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
	if (i->id == SPARC_INS_ANDN || i->id == SPARC_INS_ANDNCC)
	{
		op1 = irb.CreateNot(op1);
	}
	auto* a = irb.CreateAnd(op0, op1);
	if (auto* d = destOperand(si))
	{
		storeOp(*d, a, irb);
	}
	if (i->id == SPARC_INS_ANDCC || i->id == SPARC_INS_ANDNCC)
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
	if (i->id == SPARC_INS_ORN || i->id == SPARC_INS_ORNCC)
	{
		op1 = irb.CreateNot(op1);
	}
	auto* o = irb.CreateOr(op0, op1);
	if (auto* d = destOperand(si))
	{
		storeOp(*d, o, irb);
	}
	if (i->id == SPARC_INS_ORCC || i->id == SPARC_INS_ORNCC)
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
	if (i->id == SPARC_INS_XNOR || i->id == SPARC_INS_XNORCC)
	{
		op1 = irb.CreateNot(op1);
	}
	auto* x = irb.CreateXor(op0, op1);
	if (auto* d = destOperand(si))
	{
		storeOp(*d, x, irb);
	}
	if (i->id == SPARC_INS_XORCC || i->id == SPARC_INS_XNORCC)
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

	cs_sparc_op* dst = &si->operands[si->op_count - 1];
	uint32_t dr = (dst->type == SPARC_OP_REG) ? dst->reg : SPARC_REG_INVALID;

	llvm::Type* ty = nullptr;
	eOpConv ct = eOpConv::SEXT_TRUNC_OR_BITCAST;
	switch (i->id)
	{
		case SPARC_INS_LDSB: ty = irb.getInt8Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDUB: ty = irb.getInt8Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDSH: ty = irb.getInt16Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDUH: ty = irb.getInt16Ty(); ct = eOpConv::ZEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDSW: ty = irb.getInt32Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LD:
			if (isFpSingleRegister(dr) || isFpDoubleRegister(dr))
			{
				// Integer memory model: load bits, then bitcast to FP.
				ty = isFpDoubleRegister(dr) ? static_cast<llvm::Type*>(irb.getInt64Ty())
						: static_cast<llvm::Type*>(irb.getInt32Ty());
				ct = eOpConv::THROW;
			}
			else
			{
				ty = irb.getInt32Ty();
				ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			}
			break;
		case SPARC_INS_LDX:  ty = irb.getInt64Ty(); ct = eOpConv::SEXT_TRUNC_OR_BITCAST; break;
		case SPARC_INS_LDD:
			if (isFpDoubleRegister(dr) || isFpSingleRegister(dr))
			{
				ty = irb.getInt64Ty();
				ct = eOpConv::THROW;
			}
			else
			{
				ty = irb.getInt64Ty();
				ct = eOpConv::ZEXT_TRUNC_OR_BITCAST;
			}
			break;
		default:
			throw GenericError("Unhandled insn ID in translateLoad().");
	}

	auto* mem = loadOp(si->operands[0], irb, ty);

	if ((i->id == SPARC_INS_LD || i->id == SPARC_INS_LDD)
			&& (isFpSingleRegister(dr) || isFpDoubleRegister(dr)))
	{
		llvm::Type* fpTy = (i->id == SPARC_INS_LDD || isFpDoubleRegister(dr))
				? static_cast<llvm::Type*>(irb.getDoubleTy())
				: static_cast<llvm::Type*>(irb.getFloatTy());
		mem = irb.CreateBitCast(mem, fpTy);
		storeOp(*dst, mem, irb, eOpConv::THROW);
		return;
	}

	if (i->id == SPARC_INS_LDD && isGeneralPurposeRegister(dr) && !isGprPairRegister(dr))
	{
		auto* hi = irb.CreateLShr(mem, llvm::ConstantInt::get(mem->getType(), 32));
		auto* lo = irb.CreateTrunc(mem, irb.getInt32Ty());
		hi = irb.CreateTrunc(hi, irb.getInt32Ty());
		storeRegister(dr, hi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		storeRegister(nextGpr(dr), lo, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	storeOp(*dst, mem, irb, ct);
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

	uint32_t sr = (si->operands[0].type == SPARC_OP_REG)
			? si->operands[0].reg
			: SPARC_REG_INVALID;

	llvm::Type* ty = nullptr;
	switch (i->id)
	{
		case SPARC_INS_STB: ty = irb.getInt8Ty(); break;
		case SPARC_INS_STH: ty = irb.getInt16Ty(); break;
		case SPARC_INS_ST:
			ty = isFpSingleRegister(sr) ? static_cast<llvm::Type*>(irb.getFloatTy())
					: static_cast<llvm::Type*>(irb.getInt32Ty());
			break;
		case SPARC_INS_STX: ty = irb.getInt64Ty(); break;
		case SPARC_INS_STD:
			if (isFpDoubleRegister(sr) || isFpSingleRegister(sr))
			{
				ty = irb.getDoubleTy();
			}
			else
			{
				ty = irb.getInt64Ty();
			}
			break;
		default:
			throw GenericError("Unhandled insn ID in translateStore().");
	}

	if (i->id == SPARC_INS_STD && isGeneralPurposeRegister(sr) && !isGprPairRegister(sr))
	{
		auto* hi = loadRegister(sr, irb);
		auto* lo = loadRegister(nextGpr(sr), irb);
		hi = irb.CreateZExt(irb.CreateZExtOrTrunc(hi, irb.getInt32Ty()), irb.getInt64Ty());
		lo = irb.CreateZExt(irb.CreateZExtOrTrunc(lo, irb.getInt32Ty()), irb.getInt64Ty());
		op0 = irb.CreateOr(irb.CreateShl(hi, llvm::ConstantInt::get(hi->getType(), 32)), lo);
		storeOp(si->operands[1], op0, irb);
		return;
	}

	op0 = loadOp(si->operands[0], irb);
	if (ty->isFloatingPointTy())
	{
		op0 = generateTypeConversion(irb, op0, ty, eOpConv::FPCAST_OR_BITCAST);
	}
	else if (op0->getType()->isIntegerTy())
	{
		op0 = irb.CreateZExtOrTrunc(op0, ty);
	}
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
						|| isFccRegister(si->operands[k].reg)))
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

	if (si->cc == SPARC_CC_ICC_A
			|| si->cc == SPARC_CC_FCC_A
			|| si->cc == SPARC_CC_INVALID)
	{
		generateBranchFunctionCall(irb, target);
		return;
	}

	auto* cond = generateCondition(si, irb);
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
		if (std::strcmp(i->mnemonic, "retl") == 0
				|| std::strcmp(i->mnemonic, "ret") == 0
				|| i->alias_id == SPARC_INS_ALIAS_RETL
				|| i->alias_id == SPARC_INS_ALIAS_RET)
		{
			translateRet(i, si, irb);
			return;
		}
		throwUnexpectedOperands(i);
		translatePseudoAsmGeneric(i, si, irb);
		return;
	}

	if (i->id == SPARC_INS_RET || i->id == SPARC_INS_RETL
			|| i->alias_id == SPARC_INS_ALIAS_RET
			|| i->alias_id == SPARC_INS_ALIAS_RETL)
	{
		translateRet(i, si, irb);
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

	uint32_t rdReg = rd ? rd->reg : SPARC_REG_G0;
	uint32_t base = SPARC_REG_INVALID;
	int64_t disp = 0;
	bool indexed = false;
	if (si->operands[0].type == SPARC_OP_MEM)
	{
		base = si->operands[0].mem.base;
		disp = si->operands[0].mem.disp;
		indexed = si->operands[0].mem.index != SPARC_REG_INVALID
				&& si->operands[0].mem.index != SPARC_REG_G0;
	}
	else if (si->operands[0].type == SPARC_OP_REG)
	{
		base = si->operands[0].reg;
		if (si->op_count >= 2 && si->operands[1].type == SPARC_OP_IMM)
		{
			disp = si->operands[1].imm;
		}
	}

	if (!indexed && rdReg == SPARC_REG_G0 && disp == 8
			&& (base == SPARC_REG_O7 || base == SPARC_REG_I7))
	{
		generateReturnFunctionCall(irb, target);
		return;
	}

	generateBranchFunctionCall(irb, target);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateRet(
		cs_insn* i,
		cs_sparc* si,
		llvm::IRBuilder<>& irb)
{
	(void)si;
	const bool retl = i->id == SPARC_INS_RETL
			|| i->alias_id == SPARC_INS_ALIAS_RETL
			|| std::strcmp(i->mnemonic, "retl") == 0;
	uint32_t link = retl ? SPARC_REG_O7 : SPARC_REG_I7;
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

void Capstone2LlvmIrTranslatorSparc_impl::translateShift(
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
	unsigned mask = (i->id == SPARC_INS_SLLX || i->id == SPARC_INS_SRLX || i->id == SPARC_INS_SRAX)
			? 63u : 31u;
	op1 = irb.CreateAnd(op1, llvm::ConstantInt::get(op1->getType(), mask));

	llvm::Value* r = nullptr;
	switch (i->id)
	{
		case SPARC_INS_SLL:
		case SPARC_INS_SLLX: r = irb.CreateShl(op0, op1); break;
		case SPARC_INS_SRL:
		case SPARC_INS_SRLX: r = irb.CreateLShr(op0, op1); break;
		case SPARC_INS_SRA:
		case SPARC_INS_SRAX: r = irb.CreateAShr(op0, op1); break;
		default:
			throw GenericError("Unhandled insn ID in translateShift().");
	}
	if (auto* d = destOperand(si))
	{
		storeOp(*d, r, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateMul(
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
	llvm::Value* result = nullptr;
	bool cc = (i->id == SPARC_INS_SMULCC || i->id == SPARC_INS_UMULCC);

	if (i->id == SPARC_INS_MULX)
	{
		result = irb.CreateMul(op0, op1);
	}
	else
	{
		auto* i32 = irb.getInt32Ty();
		auto* i64 = irb.getInt64Ty();
		auto* a32 = irb.CreateZExtOrTrunc(op0, i32);
		auto* b32 = irb.CreateZExtOrTrunc(op1, i32);
		llvm::Value* wide = nullptr;
		if (i->id == SPARC_INS_SMUL || i->id == SPARC_INS_SMULCC)
		{
			wide = irb.CreateMul(irb.CreateSExt(a32, i64), irb.CreateSExt(b32, i64));
		}
		else
		{
			wide = irb.CreateMul(irb.CreateZExt(a32, i64), irb.CreateZExt(b32, i64));
		}
		result = irb.CreateTrunc(wide, i32);
		auto* hi = irb.CreateTrunc(irb.CreateLShr(wide, llvm::ConstantInt::get(i64, 32)), i32);
		storeRegister(SPARC_REG_Y, hi, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		result = generateTypeConversion(irb, result, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}

	if (auto* d = destOperand(si))
	{
		storeOp(*d, result, irb);
	}
	if (cc)
	{
		storeIcc(result, op0, op1, /*isSub=*/false, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateDiv(
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
	llvm::Value* result = nullptr;
	bool cc = (i->id == SPARC_INS_SDIVCC || i->id == SPARC_INS_UDIVCC);

	if (i->id == SPARC_INS_SDIVX)
	{
		result = irb.CreateSDiv(op0, op1);
	}
	else if (i->id == SPARC_INS_UDIVX)
	{
		result = irb.CreateUDiv(op0, op1);
	}
	else
	{
		auto* i32 = irb.getInt32Ty();
		auto* i64 = irb.getInt64Ty();
		auto* y = loadRegister(SPARC_REG_Y, irb);
		y = irb.CreateZExt(irb.CreateZExtOrTrunc(y, i32), i64);
		auto* lo = irb.CreateZExt(irb.CreateZExtOrTrunc(op0, i32), i64);
		auto* dividend = irb.CreateOr(irb.CreateShl(y, llvm::ConstantInt::get(i64, 32)), lo);
		auto* divisor = irb.CreateZExtOrTrunc(op1, i32);
		if (i->id == SPARC_INS_SDIV || i->id == SPARC_INS_SDIVCC)
		{
			result = irb.CreateSDiv(dividend, irb.CreateSExt(divisor, i64));
		}
		else
		{
			result = irb.CreateUDiv(dividend, irb.CreateZExt(divisor, i64));
		}
		result = irb.CreateTrunc(result, i32);
		result = generateTypeConversion(irb, result, op0->getType(), eOpConv::ZEXT_TRUNC_OR_BITCAST);
	}

	if (auto* d = destOperand(si))
	{
		storeOp(*d, result, irb);
	}
	if (cc)
	{
		storeIcc(result, op0, op1, /*isSub=*/false, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateRd(
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

void Capstone2LlvmIrTranslatorSparc_impl::translateWr(
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

	// wr rs1, rs2_or_imm, %asr  →  asr = rs1 xor rs2
	if (si->op_count >= 3)
	{
		std::tie(op0, op1) = loadOpRs1Rs2(si, irb);
		storeOp(si->operands[si->op_count - 1], irb.CreateXor(op0, op1), irb);
	}
	else
	{
		op0 = loadOp(si->operands[0], irb);
		storeOp(si->operands[1], op0, irb);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateFpArith(
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
	llvm::Value* r = nullptr;
	switch (i->id)
	{
		case SPARC_INS_FADDS:
		case SPARC_INS_FADDD: r = irb.CreateFAdd(op0, op1); break;
		case SPARC_INS_FSUBS:
		case SPARC_INS_FSUBD: r = irb.CreateFSub(op0, op1); break;
		case SPARC_INS_FMULS:
		case SPARC_INS_FMULD: r = irb.CreateFMul(op0, op1); break;
		case SPARC_INS_FDIVS:
		case SPARC_INS_FDIVD: r = irb.CreateFDiv(op0, op1); break;
		default:
			throw GenericError("Unhandled insn ID in translateFpArith().");
	}
	if (auto* d = destOperand(si) ? destOperand(si) : fpDestOperand(si))
	{
		storeOp(*d, r, irb, eOpConv::FPCAST_OR_BITCAST);
	}
}

void Capstone2LlvmIrTranslatorSparc_impl::translateFpCmp(
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

	cs_sparc_op* aOp = &si->operands[0];
	cs_sparc_op* bOp = &si->operands[1];
	uint32_t fcc = SPARC_REG_FCC0;
	if (si->operands[0].type == SPARC_OP_REG && isFccRegister(si->operands[0].reg))
	{
		fcc = si->operands[0].reg;
		if (si->op_count < 3)
		{
			throwUnexpectedOperands(i);
			translatePseudoAsmGeneric(i, si, irb);
			return;
		}
		aOp = &si->operands[1];
		bOp = &si->operands[2];
	}
	else if (si->cc_field >= SPARC_CC_FIELD_FCC0 && si->cc_field <= SPARC_CC_FIELD_FCC3)
	{
		fcc = fccFromField(si->cc_field);
	}

	(void)i;
	auto* a = loadOp(*aOp, irb);
	auto* b = loadOp(*bOp, irb);
	if (a->getType() != b->getType() && a->getType()->isFloatingPointTy())
	{
		b = generateTypeConversion(irb, b, a->getType(), eOpConv::FPCAST_OR_BITCAST);
	}
	storeFcc(a, b, fcc, irb);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateFpUnary(
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
	cs_sparc_op* d = &si->operands[si->op_count - 1];
	llvm::Value* r = nullptr;

	switch (i->id)
	{
		case SPARC_INS_FMOVS:
		case SPARC_INS_FMOVD:
			r = op0;
			break;
		case SPARC_INS_FNEGS:
		case SPARC_INS_FNEGD:
			r = irb.CreateFNeg(op0);
			break;
		case SPARC_INS_FABSS:
		case SPARC_INS_FABSD:
		{
			auto* f = llvm::Intrinsic::getOrInsertDeclaration(
					_module, llvm::Intrinsic::fabs, {op0->getType()});
			r = irb.CreateCall(f, {op0});
			break;
		}
		case SPARC_INS_FSQRTS:
		case SPARC_INS_FSQRTD:
		{
			auto* f = llvm::Intrinsic::getOrInsertDeclaration(
					_module, llvm::Intrinsic::sqrt, {op0->getType()});
			r = irb.CreateCall(f, {op0});
			break;
		}
		case SPARC_INS_FITOS:
		{
			auto* bits = irb.CreateBitCast(op0, irb.getInt32Ty());
			r = irb.CreateSIToFP(bits, irb.getFloatTy());
			break;
		}
		case SPARC_INS_FITOD:
		{
			auto* bits = irb.CreateBitCast(op0, irb.getInt32Ty());
			r = irb.CreateSIToFP(bits, irb.getDoubleTy());
			break;
		}
		case SPARC_INS_FSTOI:
		{
			auto* si32 = irb.CreateFPToSI(op0, irb.getInt32Ty());
			r = irb.CreateBitCast(si32, irb.getFloatTy());
			break;
		}
		case SPARC_INS_FDTOI:
		{
			auto* si32 = irb.CreateFPToSI(op0, irb.getInt32Ty());
			r = irb.CreateBitCast(si32, irb.getFloatTy());
			break;
		}
		case SPARC_INS_FSTOD:
			r = irb.CreateFPExt(op0, irb.getDoubleTy());
			break;
		case SPARC_INS_FDTOS:
			r = irb.CreateFPTrunc(op0, irb.getFloatTy());
			break;
		default:
			throw GenericError("Unhandled insn ID in translateFpUnary().");
	}

	storeOp(*d, r, irb, eOpConv::FPCAST_OR_BITCAST);
}

void Capstone2LlvmIrTranslatorSparc_impl::translateBr(
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
	llvm::Value* target = loadOpAddress(si->operands[si->op_count - 1], irb);
	auto* zero = llvm::ConstantInt::get(op0->getType(), 0);
	llvm::Value* cond = nullptr;

	sparc_cc cc = si->cc;
	if (i->id == SPARC_INS_BRZ) cc = SPARC_CC_REG_Z;
	else if (i->id == SPARC_INS_BRLEZ) cc = SPARC_CC_REG_LEZ;
	else if (i->id == SPARC_INS_BRLZ) cc = SPARC_CC_REG_LZ;
	else if (i->id == SPARC_INS_BRNZ) cc = SPARC_CC_REG_NZ;
	else if (i->id == SPARC_INS_BRGZ) cc = SPARC_CC_REG_GZ;
	else if (i->id == SPARC_INS_BRGEZ) cc = SPARC_CC_REG_GEZ;

	switch (cc)
	{
		case SPARC_CC_REG_Z:   cond = irb.CreateICmpEQ(op0, zero); break;
		case SPARC_CC_REG_LEZ: cond = irb.CreateICmpSLE(op0, zero); break;
		case SPARC_CC_REG_LZ:  cond = irb.CreateICmpSLT(op0, zero); break;
		case SPARC_CC_REG_NZ:  cond = irb.CreateICmpNE(op0, zero); break;
		case SPARC_CC_REG_GZ:  cond = irb.CreateICmpSGT(op0, zero); break;
		case SPARC_CC_REG_GEZ: cond = irb.CreateICmpSGE(op0, zero); break;
		default:
			cond = irb.getTrue();
			break;
	}
	generateCondBranchFunctionCall(irb, cond, target);
}

} // namespace capstone2llvmir
} // namespace retdec
