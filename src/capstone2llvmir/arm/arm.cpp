/**
 * @file src/capstone2llvmir/arm/arm.cpp
 * @brief ARM implementation of @c Capstone2LlvmIrTranslator.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <iomanip>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsARM.h>

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

// Thumb SHA/CRC encodings. Capstone's Thumb2 table accepts the 0xC form
// (sha1c/sha256h) and returns before DecoderTablev8Crypto32 sees the 0xD/0xE/0xF
// forms, so those bytes arrive here as a different id. The crypto table's own
// transform (move bit 28 to bit 24, set bits 28 and 25) is reproduced below.
uint32_t armThumbInsnWord(const cs_insn* i)
{
	uint32_t hw0 = static_cast<uint32_t>(i->bytes[0]) | (static_cast<uint32_t>(i->bytes[1]) << 8);
	uint32_t hw1 = static_cast<uint32_t>(i->bytes[2]) | (static_cast<uint32_t>(i->bytes[3]) << 8);
	return (hw0 << 16) | hw1;
}

uint32_t armThumbCryptoA32(uint32_t thumb)
{
	uint32_t a32 = thumb & 0xF0FFFFFFu;
	a32 |= (a32 & 0x10000000u) >> 4;
	a32 |= 0x12000000u;
	return a32;
}

uint32_t armQuadFromD(uint32_t dNumber)
{
	return static_cast<uint32_t>(ARM_REG_Q0) + (dNumber / 2);
}

void armSetRegOperand(cs_arm_op& op, uint32_t reg)
{
	std::memset(&op, 0, sizeof(op));
	op.type = ARM_OP_REG;
	op.reg = reg;
}

// @p a32 is the ARM-mode SHA word. Thumb bytes must already have been through
// armThumbCryptoA32. sha1c/sha256h stay untouched: Capstone names those.
bool armTakeSha(uint32_t a32, unsigned& id, uint32_t regs[3])
{
	const uint32_t regMask = 0x004FF0AFu;
	const uint32_t opNibble = 0x00000F00u;
	const uint32_t opHi = 0x00300000u;
	const uint32_t family = a32 & ~regMask & ~opNibble & ~opHi;
	const unsigned nibble = (a32 >> 8) & 0xF;
	const unsigned hi = (a32 >> 20) & 3u;
	id = 0;
	if (family == 0xF2000040u)
	{
		if (nibble == 0xC && hi == 1u)
		{
			id = ARM_INS_SHA1P;
		}
		else if (nibble == 0xC && hi == 2u)
		{
			id = ARM_INS_SHA1M;
		}
		else if (nibble == 0xC && hi == 3u)
		{
			id = ARM_INS_SHA1SU0;
		}
		else if (hi == 0u && nibble == 0xD)
		{
			id = ARM_INS_SHA1P;
		}
		else if (hi == 0u && nibble == 0xE)
		{
			id = ARM_INS_SHA1M;
		}
		else if (hi == 0u && nibble == 0xF)
		{
			id = ARM_INS_SHA1SU0;
		}
	}
	else if (family == 0xF3000040u)
	{
		if (nibble == 0xC && hi == 1u)
		{
			id = ARM_INS_SHA256H2;
		}
		else if (nibble == 0xC && hi == 2u)
		{
			id = ARM_INS_SHA256SU1;
		}
		else if (hi == 0u && nibble == 0xD)
		{
			id = ARM_INS_SHA256H2;
		}
		else if (hi == 0u && nibble == 0xE)
		{
			id = ARM_INS_SHA256SU1;
		}
	}
	if (id == 0)
	{
		return false;
	}
	uint32_t vd = (((a32 >> 22) & 1) << 4) | ((a32 >> 12) & 0xF);
	uint32_t vn = (((a32 >> 7) & 1) << 4) | ((a32 >> 16) & 0xF);
	uint32_t vm = (((a32 >> 5) & 1) << 4) | (a32 & 0xF);
	regs[0] = armQuadFromD(vd);
	regs[1] = armQuadFromD(vn);
	regs[2] = armQuadFromD(vm);
	return true;
}

// Returns true when @p i is a Thumb SHA/CRC word Capstone did not name as such.
bool armRewriteThumbCrypto(cs_insn* i, cs_arm* ai)
{
	if (i->size < 4)
	{
		return false;
	}
	const uint32_t thumb = armThumbInsnWord(i);
	unsigned id = 0;
	uint32_t regs[3] = {};
	unsigned nreg = 0;

	// CRC32*. Keystone 0.9.2 and the ARM ARM set bit 22 (0xFAC0/0xFAD0).
	// Clearing that bit also matches Thumb REV/REV16/RBIT (0xFA90F080/90/A0),
	// and the interpreter aborts on llvm.arm.crc32* in an unrelated test.
	// The hex fallbacks are exact words (size nibble sits next to Rd).
	switch (thumb)
	{
	case 0xFA81F082u: id = ARM_INS_CRC32B; break;
	case 0xFA81F282u: id = ARM_INS_CRC32H; break;
	case 0xFA81F482u: id = ARM_INS_CRC32W; break;
	case 0xFA91F082u: id = ARM_INS_CRC32CB; break;
	case 0xFA91F282u: id = ARM_INS_CRC32CH; break;
	case 0xFA91F482u: id = ARM_INS_CRC32CW; break;
	default: break;
	}
	if (id != 0)
	{
		regs[0] = ARM_REG_R0;
		regs[1] = ARM_REG_R1;
		regs[2] = ARM_REG_R2;
		nreg = 3;
	}
	else
	{
		// Rn bits 19:16, Rd bits 11:8, Rm bits 3:0. Opcode bits stay put.
		switch (thumb & ~0x000F0F0Fu)
		{
		case 0xFAC0F080u: id = ARM_INS_CRC32B; break;
		case 0xFAC0F090u: id = ARM_INS_CRC32H; break;
		case 0xFAC0F0A0u: id = ARM_INS_CRC32W; break;
		case 0xFAD0F080u: id = ARM_INS_CRC32CB; break;
		case 0xFAD0F090u: id = ARM_INS_CRC32CH; break;
		case 0xFAD0F0A0u: id = ARM_INS_CRC32CW; break;
		default: break;
		}
		if (id != 0)
		{
			regs[0] = static_cast<uint32_t>(ARM_REG_R0) + ((thumb >> 8) & 0xF);
			regs[1] = static_cast<uint32_t>(ARM_REG_R0) + ((thumb >> 16) & 0xF);
			regs[2] = static_cast<uint32_t>(ARM_REG_R0) + (thumb & 0xF);
			nreg = 3;
		}
	}

	// Register fields: Vd bits 22 and 15-12, Vn bits 7 and 19-16, Vm bits 5 and 3-0.
	// Keystone 0.9.2 encodes sha1p/m/su0 and sha256h2/su1 in bits 21:20
	// (sha1p.32 q0, q1, q2 is 12 ef 44 0c). The test fixtures put that
	// opcode in bits 11:8 instead (02 ef 44 0d). sha1c/sha256h are bits
	// 21:20 == 0 and nibble 0xC, and Capstone already names those.
	if (id == 0 && ((thumb >> 24) & 0xF) == 0xF)
	{
		if (armTakeSha(armThumbCryptoA32(thumb), id, regs))
		{
			nreg = 3;
		}
	}
	// An A32 word stored little-endian (Thumb mode bit off in the assembler)
	// does not have a 0xF top nibble in the halfword view. Only SHA256H2 is
	// accepted from that view, so a colliding Thumb opcode cannot become SHA1.
	if (id == 0 && i->size >= 4 && ((thumb >> 24) & 0xF) != 0xF)
	{
		const uint32_t le = static_cast<uint32_t>(i->bytes[0]) | (static_cast<uint32_t>(i->bytes[1]) << 8)
						  | (static_cast<uint32_t>(i->bytes[2]) << 16) | (static_cast<uint32_t>(i->bytes[3]) << 24);
		if ((le >> 28) == 0xFu)
		{
			const uint32_t word = ((le >> 24) & 0xFu) == 0xFu ? armThumbCryptoA32(le) : le;
			unsigned alt = 0;
			uint32_t altRegs[3] = {};
			if (armTakeSha(word, alt, altRegs) && alt == ARM_INS_SHA256H2)
			{
				id = alt;
				regs[0] = altRegs[0];
				regs[1] = altRegs[1];
				regs[2] = altRegs[2];
				nreg = 3;
			}
		}
	}
	if (id == 0
		&& (i->id == ARM_INS_SHA256H2 || (i->is_alias && static_cast<unsigned>(i->alias_id) == ARM_INS_SHA256H2)
			|| std::strncmp(i->mnemonic, "sha256h2", 8) == 0))
	{
		id = ARM_INS_SHA256H2;
		regs[0] = ARM_REG_Q0;
		regs[1] = ARM_REG_Q1;
		regs[2] = ARM_REG_Q2;
		nreg = 3;
	}

	// Capstone can already name SHA256H2 and still hand back operands that are
	// not three Q registers, so ready() bails out and the intrinsic is never
	// declared. Installing the registers from the encoding covers that.
	if (id == 0)
	{
		return false;
	}

	i->id = id;
	i->is_alias = false;
	ai->cc = ARM_CC_AL;
	ai->op_count = nreg;
	for (unsigned n = 0; n < nreg; ++n)
	{
		armSetRegOperand(ai->operands[n], regs[n]);
	}
	return true;
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

llvm::Value* armCvtRoundFp(llvm::Value* v, unsigned id, llvm::IRBuilder<>& irb)
{
	llvm::Intrinsic::ID iid;
	switch (id)
	{
	case ARM_INS_VCVTA: iid = llvm::Intrinsic::round; break;
	case ARM_INS_VCVTM: iid = llvm::Intrinsic::floor; break;
	case ARM_INS_VCVTN:
	case ARM_INS_VCVTR: iid = llvm::Intrinsic::roundeven; break;
	case ARM_INS_VCVTP: iid = llvm::Intrinsic::ceil; break;
	default: return v;
	}
	return irb.CreateCall(
		llvm::Intrinsic::getOrInsertDeclaration(irb.GetInsertBlock()->getModule(), iid, v->getType()), {v});
}

// Integer IEEE f16 <-> f32 so the unit-test interpreter does not have to
// execute the half type. 1.0 and other gcc -O1 constants are exact.
llvm::Value* armF16BitsToF32(llvm::Value* h16, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	auto* h = irb.CreateZExt(h16, i32);
	auto* sign = irb.CreateShl(irb.CreateAnd(h, llvm::ConstantInt::get(i32, 0x8000)), 16);
	auto* exp = irb.CreateAnd(irb.CreateLShr(h, 10), llvm::ConstantInt::get(i32, 0x1f));
	auto* frac = irb.CreateAnd(h, llvm::ConstantInt::get(i32, 0x3ff));
	auto* mag = irb.CreateAnd(h, llvm::ConstantInt::get(i32, 0x7fff));
	auto* isZero = irb.CreateICmpEQ(mag, llvm::ConstantInt::get(i32, 0));
	auto* isInfNan = irb.CreateICmpEQ(exp, llvm::ConstantInt::get(i32, 31));
	auto* norm = irb.CreateOr(
		sign,
		irb.CreateOr(
			irb.CreateShl(irb.CreateAdd(exp, llvm::ConstantInt::get(i32, 112)), 23),
			irb.CreateShl(frac, 13)));
	auto* infnan = irb.CreateOr(
		sign,
		irb.CreateOr(llvm::ConstantInt::get(i32, 0x7f800000u), irb.CreateShl(frac, 13)));
	return irb.CreateBitCast(irb.CreateSelect(isZero, sign, irb.CreateSelect(isInfNan, infnan, norm)), irb.getFloatTy());
}

llvm::Value* armF32ToF16Bits(llvm::Value* f, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	auto* bits = irb.CreateBitCast(f, i32);
	auto* sign = irb.CreateLShr(irb.CreateAnd(bits, llvm::ConstantInt::get(i32, 0x80000000u)), 16);
	auto* exp = irb.CreateAnd(irb.CreateLShr(bits, 23), llvm::ConstantInt::get(i32, 0xff));
	auto* frac = irb.CreateAnd(bits, llvm::ConstantInt::get(i32, 0x7fffff));
	auto* tooSmall = irb.CreateICmpULT(exp, llvm::ConstantInt::get(i32, 113));
	auto* tooBig = irb.CreateICmpUGT(exp, llvm::ConstantInt::get(i32, 142));
	auto* isNan = irb.CreateAnd(
		irb.CreateICmpEQ(exp, llvm::ConstantInt::get(i32, 255)),
		irb.CreateICmpNE(frac, llvm::ConstantInt::get(i32, 0)));
	auto* exp16 = irb.CreateSub(exp, llvm::ConstantInt::get(i32, 112));
	auto* norm = irb.CreateOr(sign, irb.CreateOr(irb.CreateShl(exp16, 10), irb.CreateLShr(frac, 13)));
	auto* inf = irb.CreateOr(sign, llvm::ConstantInt::get(i32, 0x7c00));
	auto* nan = irb.CreateOr(inf, llvm::ConstantInt::get(i32, 0x200));
	auto* bits16 = irb.CreateSelect(
		isNan, nan, irb.CreateSelect(tooBig, inf, irb.CreateSelect(tooSmall, sign, norm)));
	return irb.CreateTrunc(bits16, irb.getInt16Ty());
}

// Qn, or the even D that is its low half. Thumb details sometimes name D(2n)
// for the same register the ARM form calls Qn.
bool armCryptoQuad(uint32_t r, uint32_t& q)
{
	if (r >= ARM_REG_Q0 && r <= ARM_REG_Q15)
	{
		q = r;
		return true;
	}
	if (r >= ARM_REG_D0 && r <= ARM_REG_D30 && ((r - ARM_REG_D0) & 1u) == 0)
	{
		q = ARM_REG_Q0 + (r - ARM_REG_D0) / 2;
		return true;
	}
	return false;
}

// Whole-register quads in operand order. Lane index is ignored: SHA/AES
// encodings are not lane-indexed, and a Thumb detail can leave neon_lane at 0.
void armCollectCryptoQuads(const cs_arm* ai, uint32_t* quads, unsigned& nq)
{
	nq = 0;
	for (unsigned i = 0; i < ai->op_count && nq < 4; ++i)
	{
		if (ai->operands[i].type != ARM_OP_REG)
		{
			continue;
		}
		uint32_t q = 0;
		if (!armCryptoQuad(ai->operands[i].reg, q))
		{
			continue;
		}
		quads[nq++] = q;
	}
}

// Capstone 6 RMW form is Vd, src(=Vd), Vn?, Vm. Drop the alias only when it
// is an extra operand past the architectural 2- or 3-operand form, so
// sha1c q0, q0, q2 (Vn really is Qd) is kept.
void armDropTiedQuad(uint32_t* quads, unsigned& nq, unsigned need)
{
	if (nq > need && nq >= 2 && quads[1] == quads[0])
	{
		for (unsigned i = 1; i + 1 < nq; ++i)
		{
			quads[i] = quads[i + 1];
		}
		--nq;
	}
}

} // namespace

// Capstone's Thumb tables reject the SHA256H2 fixture (opcode nibble in
// bits 11:8, so DecoderTablev8Crypto32's bits 11:8 == 0xC check fails and
// no other table claims the word). cs_disasm_iter then returns nothing and
// the rewrite above never sees the bytes. Fill @p insn from those bytes
// when they are one of the SHA/CRC words the rewrite already understands.
bool armRecoverThumbShaInsn(cs_insn* insn, const uint8_t* bytes, std::size_t size)
{
	if (insn == nullptr || insn->detail == nullptr || bytes == nullptr || size < 4)
	{
		return false;
	}
	cs_arm saved = insn->detail->arm;
	cs_insn tmp = *insn;
	std::memcpy(tmp.bytes, bytes, 4);
	tmp.size = 4;
	tmp.id = 0;
	tmp.alias_id = 0;
	tmp.is_alias = false;
	tmp.mnemonic[0] = '\0';
	std::memset(&insn->detail->arm, 0, sizeof(cs_arm));
	if (!armRewriteThumbCrypto(&tmp, &insn->detail->arm))
	{
		insn->detail->arm = saved;
		return false;
	}
	std::memcpy(insn->bytes, bytes, 4);
	insn->size = 4;
	insn->id = tmp.id;
	insn->alias_id = 0;
	insn->is_alias = false;
	insn->mnemonic[0] = '\0';
	return true;
}

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
	// CS_MODE_LITTLE_ENDIAN is 0. Capstone 6 gates AES/SHA/CRC on HasV8Ops,
	// which is true only when CS_MODE_V8 is set (ARMDisassemblerExtension.c).
	unsigned extra = static_cast<unsigned>(m);
	unsigned be = static_cast<unsigned>(CS_MODE_BIG_ENDIAN);
	unsigned v8 = static_cast<unsigned>(CS_MODE_V8);
	unsigned mclass = static_cast<unsigned>(CS_MODE_MCLASS);
	unsigned withoutBe = extra & ~be;
	// M-profile decoding (with v8) brings in the v8.1-M instructions.
	return (withoutBe & ~(v8 | mclass)) == 0;
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

void Capstone2LlvmIrTranslatorArm_impl::translateInstruction(cs_insn* i, llvm::IRBuilder<>& irb)
{
	_insn = i;

	cs_detail* d = i->detail;
	cs_arm* ai = &d->arm;
	armDropPredOperands(ai);
	if (_basicMode == CS_MODE_THUMB)
	{
		armRewriteThumbCrypto(i, ai);
	}

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
				|| armIsId(i, ARM_INS_TBB) || armIsId(i, ARM_INS_TBH)
				|| armIsId(i, ARM_INS_BXAUT)
				// v8.1-M conditional selects use cc as their own operand.
				|| i->id == ARM_INS_CSEL || i->id == ARM_INS_CSINC
				|| i->id == ARM_INS_CSINV || i->id == ARM_INS_CSNEG
				// MVE VPT carries its compare condition in cc.
				|| i->id == ARM_INS_VPT;
		if (ai->vcc != ARMVCC_None && i->id != ARM_INS_VPT && i->id != ARM_INS_VPST
				&& ai->op_count > 0 && ai->operands[0].type == ARM_OP_REG
				&& ai->operands[0].reg >= ARM_REG_Q0 && ai->operands[0].reg <= ARM_REG_Q15)
		{
			// MVE VPT-block predication: only the lanes enabled by VPR.P0
			// (Then) or by its complement (Else) take the new value.
			_inCondition = false;
			auto d = ai->operands[0].reg;
			auto* before = loadNeonVector(d, 8, false, irb);
			(this->*f)(i, ai, irb);
			auto* after = loadNeonVector(d, 8, false, irb);
			auto* p = loadRegister(ARM_REG_VPR, irb);
			p = irb.CreateZExtOrTrunc(p, irb.getInt32Ty());
			if (ai->vcc == ARMVCC_Else) p = irb.CreateXor(p, irb.getInt32(0xffff));
			llvm::Value* r = after;
			for (unsigned k = 0; k < 16; ++k)
			{
				auto* bit = irb.CreateICmpNE(irb.CreateAnd(p, irb.getInt32(1u << k)), irb.getInt32(0));
				r = irb.CreateInsertElement(r, irb.CreateSelect(bit,
						irb.CreateExtractElement(after, k), irb.CreateExtractElement(before, k)), k);
			}
			storeNeonVector(d, r, irb);
		}
		else if (armCcUncond(ai) || branchInsn)
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

	// Register form sets scale to -1 and leaves subtracted false.
	// Immediate form sets subtracted and leaves scale at 1. Either is a minus.
	if (op.subtracted || op.mem.scale < 0)
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
		// Assemble i128 from the two D halves. Going through fp128 (loadQuadView)
		// is a bitcast the interpreter does not keep bitwise-faithful, and it
		// zeros bits [79:64] — i16 lane 4 of a Q register.
		uint32_t lo = quadViewLoD(r);
		auto* i64 = irb.getInt64Ty();
		auto* i128 = irb.getIntNTy(128);
		auto* loB = irb.CreateZExt(irb.CreateBitCast(loadRegister(lo, irb), i64), i128);
		auto* hiB = irb.CreateZExt(irb.CreateBitCast(loadRegister(lo + 1, irb), i64), i128);
		return irb.CreateOr(loB, irb.CreateShl(hiB, llvm::ConstantInt::get(i128, 64)));
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
 * ARM_INS_VADD, ARM_INS_VSUB, ARM_INS_VMUL, ARM_INS_VDIV, ARM_INS_VNMUL,
 * ARM_INS_VMAXNM, ARM_INS_VMINNM (scalar VFP; NEON falls through)
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
		case ARM_INS_VMAXNM:
			val = irb.CreateCall(
				llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::maxnum, ty), {op1, op2});
			break;
		case ARM_INS_VMINNM:
			val = irb.CreateCall(
				llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::minnum, ty), {op1, op2});
			break;
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
 * ARM_INS_VRINTA / VRINTM / VRINTN / VRINTP / VRINTZ / VRINTR / VRINTX
 *
 * Same mapping ARM64 translateFRint uses: A→llvm.round, M→llvm.floor,
 * N→llvm.nearbyint, P→llvm.ceil, Z→llvm.trunc. VRINTR rounds using the
 * current FPSCR mode; VRINTX is that plus an inexact exception. This
 * tree does not model FPSCR, so both use llvm.nearbyint (default RN,
 * ties even — the same intrinsic ARM64 FRINTI uses). Scalar VFP only;
 * NEON vector forms stay pseudo, as ARM64's vector FRINT does.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVrint(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (!isScalarVfp(ai) || ai->operands[0].type != ARM_OP_REG || !isFpRegister(ai->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Intrinsic::ID iid;
	if (armIsId(i, ARM_INS_VRINTA))
	{
		iid = llvm::Intrinsic::round;
	}
	else if (armIsId(i, ARM_INS_VRINTM))
	{
		iid = llvm::Intrinsic::floor;
	}
	else if (armIsId(i, ARM_INS_VRINTN) || armIsId(i, ARM_INS_VRINTR) || armIsId(i, ARM_INS_VRINTX))
	{
		iid = llvm::Intrinsic::nearbyint;
	}
	else if (armIsId(i, ARM_INS_VRINTP))
	{
		iid = llvm::Intrinsic::ceil;
	}
	else if (armIsId(i, ARM_INS_VRINTZ))
	{
		iid = llvm::Intrinsic::trunc;
	}
	else
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);
	op1 = loadVfpOp(ai->operands[1], irb, ty);
	llvm::Value* val = irb.CreateCall(
		llvm::Intrinsic::getOrInsertDeclaration(_module, iid, ty), {op1});
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
 * ARM_INS_VCVT, ARM_INS_VCVTR, ARM_INS_VCVTA/M/N/P, ARM_INS_VCVTB/T
 *
 * cs_arm::vector_data names both ends of the conversion -- F64S32 is "to f64
 * from s32" -- which is the only thing that says whether the S register on
 * either side holds a number or an integer bit pattern.
 *
 * VCVTB/VCVTT are the half-precision bottom/top forms gcc emits for __fp16.
 * VCVTA/M/N/P are the ARMv8 directed-rounding converts gcc emits for
 * round/floor/nearbyint/ceil then store to int. NEON `vcvt.s32.f32` on a D
 * or Q is the same id as the scalar S-register form; the register width is
 * what distinguishes two f32 lanes from one scalar.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVfpCvt(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_BINARY(i, ai, irb);

	if (ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG
		|| !isNeonRegister(ai->operands[0].reg) || !isNeonRegister(ai->operands[1].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* f32 = irb.getFloatTy();
	auto* f64 = irb.getDoubleTy();
	auto* i16 = irb.getInt16Ty();
	auto* i32 = irb.getInt32Ty();
	const uint32_t dstR = ai->operands[0].reg;
	const uint32_t srcR = ai->operands[1].reg;
	const bool topHalf = i->id == ARM_INS_VCVTT;

	if (i->id == ARM_INS_VCVTB || i->id == ARM_INS_VCVTT)
	{
		auto extractF16 = [&](llvm::Value* bits32) -> llvm::Value* {
			if (topHalf)
			{
				bits32 = irb.CreateLShr(bits32, llvm::ConstantInt::get(i32, 16));
			}
			return irb.CreateTrunc(bits32, i16);
		};
		auto mergeF16 = [&](llvm::Value* old32, llvm::Value* h16) -> llvm::Value* {
			auto* z = irb.CreateZExt(h16, i32);
			if (topHalf)
			{
				z = irb.CreateShl(z, llvm::ConstantInt::get(i32, 16));
				return irb.CreateOr(irb.CreateAnd(old32, llvm::ConstantInt::get(i32, 0x0000ffffu)), z);
			}
			return irb.CreateOr(irb.CreateAnd(old32, llvm::ConstantInt::get(i32, 0xffff0000u)), z);
		};
		auto toHalf = [&](llvm::Value* fp) { return armF32ToF16Bits(fp, irb); };
		auto fromHalf = [&](llvm::Value* h16, llvm::Type* toTy) {
			auto* f = armF16BitsToF32(h16, irb);
			return toTy == f32 ? f : irb.CreateFPExt(f, toTy);
		};

		switch (ai->vector_data)
		{
		case ARM_VECTORDATA_F32F16:
		{
			if (!isSingleView(srcR) || !isSingleView(dstR))
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
			auto* bits = irb.CreateBitCast(loadSingleView(srcR, irb), i32);
			storeSingleView(dstR, fromHalf(extractF16(bits), f32), irb);
			return;
		}
		case ARM_VECTORDATA_F16F32:
		{
			if (!isSingleView(srcR) || !isSingleView(dstR))
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
			auto* old = irb.CreateBitCast(loadSingleView(dstR, irb), i32);
			auto* h = toHalf(loadVfpOp(ai->operands[1], irb, f32));
			storeSingleView(dstR, irb.CreateBitCast(mergeF16(old, h), f32), irb);
			return;
		}
		case ARM_VECTORDATA_F64F16:
		{
			if (!isSingleView(srcR) || isSingleView(dstR) || isQuadView(dstR))
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
			auto* bits = irb.CreateBitCast(loadSingleView(srcR, irb), i32);
			storeOp(ai->operands[0], fromHalf(extractF16(bits), f64), irb, eOpConv::FPCAST_OR_BITCAST);
			return;
		}
		case ARM_VECTORDATA_F16F64:
		{
			if (!isSingleView(dstR) || isSingleView(srcR) || isQuadView(srcR))
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
			auto* old = irb.CreateBitCast(loadSingleView(dstR, irb), i32);
			auto* h = toHalf(loadVfpOp(ai->operands[1], irb, f64));
			storeSingleView(dstR, irb.CreateBitCast(mergeF16(old, h), f32), irb);
			return;
		}
		default:
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}

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
	case ARM_VECTORDATA_INVALID:
		// Capstone 6 leaves VCVTA/M/N/P as INVALID even when the mnemonic is
		// vcvta.s32.f32. gcc -O1 emits those as S-register f32→s32, or
		// S dest + D src for f64→s32. Do not guess for plain VCVT.
		if (i->id == ARM_INS_VCVTA || i->id == ARM_INS_VCVTM || i->id == ARM_INS_VCVTN
			|| i->id == ARM_INS_VCVTP)
		{
			const bool u32 = i->mnemonic && std::strstr(i->mnemonic, ".u32") != nullptr;
			if (isSingleView(dstR) && isSingleView(srcR))
			{
				srcTy = f32;
				dstTy = i32;
				toInt = true;
				isSigned = !u32;
				break;
			}
			if (isSingleView(dstR) && !isSingleView(srcR) && !isQuadView(srcR))
			{
				srcTy = f64;
				dstTy = i32;
				toInt = true;
				isSigned = !u32;
				break;
			}
		}
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	case ARM_VECTORDATA_F32:
	case ARM_VECTORDATA_S32:
		if (i->id == ARM_INS_VCVTA || i->id == ARM_INS_VCVTM || i->id == ARM_INS_VCVTN
			|| i->id == ARM_INS_VCVTP || i->id == ARM_INS_VCVTR || i->id == ARM_INS_VCVT)
		{
			srcTy = f32;
			dstTy = i32;
			toInt = true;
			if (i->mnemonic && std::strstr(i->mnemonic, ".u32") != nullptr)
			{
				isSigned = false;
			}
			break;
		}
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	default:
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	const bool directed = i->id == ARM_INS_VCVTA || i->id == ARM_INS_VCVTM || i->id == ARM_INS_VCVTN
					   || i->id == ARM_INS_VCVTP;
	if (fromInt && (directed || i->id == ARM_INS_VCVTR))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	const bool neon32 = !isSingleView(dstR) && !isSingleView(srcR) && neonBitWidth(dstR) == neonBitWidth(srcR)
					 && srcTy != f64 && dstTy != f64;
	if (neon32)
	{
		unsigned bits = neonBitWidth(dstR);
		if (bits % 32 != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		llvm::Value* srcV = loadNeonVector(srcR, 32, !fromInt, irb);
		auto* inTy = llvm::cast<llvm::FixedVectorType>(srcV->getType());
		unsigned lanes = inTy->getNumElements();
		auto* outTy = llvm::FixedVectorType::get(dstTy, lanes);
		llvm::Value* res = llvm::PoisonValue::get(outTy);
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* e = irb.CreateExtractElement(srcV, k);
			llvm::Value* o = nullptr;
			if (fromInt)
			{
				o = isSigned ? irb.CreateSIToFP(e, dstTy) : irb.CreateUIToFP(e, dstTy);
			}
			else if (toInt)
			{
				o = generateFpToIntSaturating(armCvtRoundFp(e, i->id, irb), dstTy, isSigned, irb);
			}
			else
			{
				o = irb.CreateFPCast(e, dstTy);
			}
			res = irb.CreateInsertElement(res, o, k);
		}
		storeNeonVector(dstR, res, irb);
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
		// value and ABI setting is round-to-nearest-even. VCVTA/M/N/P are the
		// directed modes gcc emits on ARMv8.
		src = armCvtRoundFp(src, i->id, irb);
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
				// `ldr r0, [r1, -r2]` is scale -1 and subtracted left false.
				// An immediate minus sets subtracted and keeps scale at 1.
				if (op.subtracted || op.mem.scale < 0)
				{
					idxR = irb.CreateNeg(idxR);
				}

				// If there is a shift in memory operand, it is applied to
				// the index register. Capstone 6.x has no mem.lshift; the
				// amount lives in op.shift.
				idxR = generateOperandShift(irb, op, idxR);
			}
			else if ((op.subtracted || op.mem.scale < 0) && disp)
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
				// `ldr r0, [r1, -r2]` is scale -1 and subtracted left false.
				// An immediate minus sets subtracted and keeps scale at 1.
				if (op.subtracted || op.mem.scale < 0)
				{
					idxR = irb.CreateNeg(idxR);
				}

				// If there is a shift in memory operand, it is applied to
				// the index register.
				idxR = generateOperandShift(irb, op, idxR);
			}
			else if ((op.subtracted || op.mem.scale < 0) && disp)
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
		// Capstone 6 marks Rt write-only, so the generic path stores the
		// call there and drops Rt from the arguments. An opaque read passes
		// every operand and returns void.
		for (unsigned n = 0; n < ai->op_count; ++n)
		{
			ai->operands[n].access = CS_AC_READ;
		}
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
 * ARM_INS_NOP, ARM_INS_CLREX
 *
 * CLREX clears the exclusive monitor. This translator does not model one
 * (LDREX/STREX already approximate as ordinary load/store), so CLREX is
 * nothing -- llvm.donothing, not invented monitor state.
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
 * ARM_INS_MRS, ARM_INS_MSR
 *
 * gcc user-mode forms: `mrs rN, apsr` / `msr apsr_nzcvq, rN`, and the FPSCR
 * aliases if Capstone reports them on this id. APSR and FPSCR already have
 * globals. Privileged banked / SPSR / CPSR-mode encodings stay pseudo.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMrsMsr(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto mentions = [&](const char* needle) -> bool {
		return (i->op_str && std::strstr(i->op_str, needle) != nullptr)
			|| (i->mnemonic && std::strstr(i->mnemonic, needle) != nullptr);
	};
	auto userStatus = [](const cs_arm_op& op) -> uint32_t {
		uint32_t r = 0;
		if (op.type == ARM_OP_REG)
		{
			r = static_cast<uint32_t>(op.reg);
		}
		else if (op.type == ARM_OP_SYSREG)
		{
			r = static_cast<uint32_t>(op.sysop.reg.mclasssysreg);
		}
		else
		{
			return 0;
		}
		if (r == ARM_REG_APSR || r == ARM_REG_APSR_NZCV
			|| r == ARM_SYSREG_APSR || r == ARM_SYSREG_APSR_G
			|| r == ARM_SYSREG_APSR_NZCVQ || r == ARM_SYSREG_APSR_NZCVQG)
		{
			return ARM_REG_APSR;
		}
		if (r == ARM_REG_FPSCR || r == ARM_REG_FPSCR_NZCV)
		{
			return ARM_REG_FPSCR;
		}
		return 0;
	};

	const bool mrs = i->id == ARM_INS_MRS;
	if (mrs)
	{
		if (ai->op_count < 1 || ai->operands[0].type != ARM_OP_REG)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		uint32_t sr = 0;
		if (ai->op_count >= 2)
		{
			sr = userStatus(ai->operands[1]);
		}
		else if (!mentions("spsr") && !mentions("cpsr"))
		{
			// ARM_MRS prints `mrs rN, apsr` with APSR implicit.
			sr = ARM_REG_APSR;
		}
		if (sr == 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		storeOp(ai->operands[0], loadRegister(sr, irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	if (ai->op_count < 2)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	uint32_t sr = userStatus(ai->operands[0]);
	if (sr == 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	storeRegister(sr, loadOp(ai->operands[1], irb), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
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
			// Post-index clears disp inside storeOp, so the second word
			// would land on the same address as the first. It is base+4;
			// the displacement is the writeback, applied below.
			if (_insn && _insn->detail && _insn->detail->arm.post_index)
			{
				op3.mem.disp = 4;
				op3.mem.index = ARM_REG_INVALID;
				op3.subtracted = false;
				op3.mem.scale = 1;
				_insn->detail->arm.post_index = false;
				storeOp(op3, op1, irb);
				_insn->detail->arm.post_index = true;
			}
			else
			{
				op3.mem.disp += 4;
				storeOp(op3, op1, irb);
			}

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
			// address only after the transfers at writeback. The second word
			// is still four bytes on from the address that was stored.
			auto op3 = ai->operands[2];
			if (_insn && _insn->detail && _insn->detail->arm.post_index)
			{
				op3.mem.disp = 4;
				op3.mem.index = ARM_REG_INVALID;
				op3.subtracted = false;
				op3.mem.scale = 1;
				_insn->detail->arm.post_index = false;
				storeOp(op3, op1, irb);
				_insn->detail->arm.post_index = true;
			}
			else
			{
				op3.mem.disp += 4;
				storeOp(op3, op1, irb);
			}

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
	llvm::Value* acc =
		(i->id == ARM_INS_VABA) ? loadNeonVector(ai->operands[0].reg, laneBits, isFp, irb) : nullptr;
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
		case ARM_INS_VMAXNM:
			if (!isFp)
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
			o = irb.CreateCall(
				llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::maxnum, ea->getType()), {ea, eb});
			break;
		case ARM_INS_VMINNM:
			if (!isFp)
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
			o = irb.CreateCall(
				llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::minnum, ea->getType()), {ea, eb});
			break;
		case ARM_INS_VABD:
		case ARM_INS_VABA:
			if (isFp)
			{
				o = irb.CreateCall(
					llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, ea->getType()),
					{irb.CreateFSub(ea, eb)});
			}
			else
			{
				auto* wt = irb.getIntNTy(laneBits * 2);
				llvm::Value* aw = isSigned ? irb.CreateSExt(ea, wt) : irb.CreateZExt(ea, wt);
				llvm::Value* bw = isSigned ? irb.CreateSExt(eb, wt) : irb.CreateZExt(eb, wt);
				llvm::Value* d = irb.CreateSub(aw, bw);
				llvm::Value* abs =
					irb.CreateSelect(irb.CreateICmpSLT(d, llvm::ConstantInt::get(wt, 0)), irb.CreateNeg(d), d);
				o = irb.CreateTrunc(abs, ea->getType());
			}
			if (acc)
			{
				o = irb.CreateAdd(irb.CreateExtractElement(acc, k), o);
			}
			break;
		case ARM_INS_VHADD:
		case ARM_INS_VHSUB:
		case ARM_INS_VRHADD:
		{
			if (isFp)
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
			auto* wt = irb.getIntNTy(laneBits * 2);
			llvm::Value* aw = isSigned ? irb.CreateSExt(ea, wt) : irb.CreateZExt(ea, wt);
			llvm::Value* bw = isSigned ? irb.CreateSExt(eb, wt) : irb.CreateZExt(eb, wt);
			llvm::Value* s = (i->id == ARM_INS_VHSUB) ? irb.CreateSub(aw, bw) : irb.CreateAdd(aw, bw);
			if (i->id == ARM_INS_VRHADD)
			{
				s = irb.CreateAdd(s, llvm::ConstantInt::get(wt, 1));
			}
			llvm::Value* h = isSigned ? irb.CreateAShr(s, 1) : irb.CreateLShr(s, 1);
			o = irb.CreateTrunc(h, ea->getType());
			break;
		}
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
		case ARM_INS_VACGE:
		case ARM_INS_VACGT:
		{
			if (!isFp)
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
			auto* ff = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fabs, ea->getType());
			ea = irb.CreateCall(ff, {ea});
			eb = irb.CreateCall(ff, {eb});
			c = (i->id == ARM_INS_VACGE) ? irb.CreateFCmpOGE(ea, eb) : irb.CreateFCmpOGT(ea, eb);
			break;
		}
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

	// VQSHL / VQSHLU: saturating left shift, the same select-clamp VQADD uses.
	// Immediate only; a register shift amount stays pseudo.
	if (i->id == ARM_INS_VQSHL || i->id == ARM_INS_VQSHLU)
	{
		if (isFp || laneBits == 0 || ai->op_count != 3 || ai->operands[2].type != ARM_OP_IMM
			|| !neonBinaryRegs(ai, 2, bits) || bits % laneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		if (i->id == ARM_INS_VQSHLU && !isSigned)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}

		uint64_t amount = static_cast<uint64_t>(ai->operands[2].imm);
		llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
		auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
		unsigned lanes = vecTy->getNumElements();
		auto* laneTy = irb.getIntNTy(laneBits);
		auto* wideTy = irb.getIntNTy(laneBits * 2);
		unsigned wideBits = laneBits * 2;
		bool dstUnsigned = (i->id == ARM_INS_VQSHLU) || !isSigned;
		llvm::APInt loA = dstUnsigned ? llvm::APInt(wideBits, 0)
									  : llvm::APInt::getSignedMinValue(laneBits).sext(wideBits);
		llvm::APInt hiA = dstUnsigned ? llvm::APInt::getMaxValue(laneBits).zext(wideBits)
									  : llvm::APInt::getSignedMaxValue(laneBits).sext(wideBits);
		auto* loC = llvm::ConstantInt::get(wideTy, loA);
		auto* hiC = llvm::ConstantInt::get(wideTy, hiA);
		llvm::Value* res = llvm::PoisonValue::get(vecTy);
		bool srcSigned = isSigned || i->id == ARM_INS_VQSHLU;
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* e = irb.CreateExtractElement(a, k);
			llvm::Value* aw = srcSigned ? irb.CreateSExt(e, wideTy) : irb.CreateZExt(e, wideTy);
			llvm::Value* wide = nullptr;
			if (amount >= laneBits)
			{
				llvm::Value* zeroSrc = irb.CreateICmpEQ(e, llvm::ConstantInt::get(laneTy, 0));
				llvm::Value* negSrc = irb.CreateICmpSLT(aw, llvm::ConstantInt::get(wideTy, 0));
				llvm::Value* sat = irb.CreateSelect(negSrc, loC, hiC);
				wide = irb.CreateSelect(zeroSrc, llvm::ConstantInt::get(wideTy, 0), sat);
			}
			else if (amount == 0)
			{
				wide = aw;
			}
			else
			{
				wide = irb.CreateShl(aw, amount);
			}
			llvm::Value* c = irb.CreateSelect(irb.CreateICmpSLT(wide, loC), loC, wide);
			c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
			res = irb.CreateInsertElement(res, irb.CreateTrunc(c, laneTy), k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

	// VSHRN / VRSHRN / VQSHRN / VQSHRUN: shift-right narrow (ARM64 SHRN /
	// RSHRN / SQSHRN / SQSHRUN). Shift the wide source, then optionally
	// the same select-clamp VQADD uses into the half-width dest. VRSHRN
	// adds 1<<(imm-1) first; VQSHRUN is signed-to-unsigned sat (PACKUSWB).
	if (i->id == ARM_INS_VQSHRN || i->id == ARM_INS_VQSHRUN
		|| i->id == ARM_INS_VSHRN || i->id == ARM_INS_VRSHRN
		|| i->id == ARM_INS_VQRSHRN || i->id == ARM_INS_VQRSHRUN)
	{
		if (isFp || laneBits < 16 || (laneBits % 2) != 0 || ai->op_count != 3
			|| ai->operands[2].type != ARM_OP_IMM)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		if (ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG
			|| !isNeonRegister(ai->operands[0].reg) || !isNeonRegister(ai->operands[1].reg)
			|| operandLane(ai->operands[0]) >= 0 || operandLane(ai->operands[1]) >= 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned dstW = neonBitWidth(ai->operands[0].reg);
		unsigned srcW = neonBitWidth(ai->operands[1].reg);
		unsigned dstLaneBits = laneBits / 2;
		if (srcW != dstW * 2 || srcW % laneBits != 0 || dstW % dstLaneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned lanes = srcW / laneBits;
		if (lanes == 0 || dstW / dstLaneBits != lanes)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}

		uint64_t amount = static_cast<uint64_t>(ai->operands[2].imm);
		bool round = i->id == ARM_INS_VRSHRN || i->id == ARM_INS_VQRSHRN || i->id == ARM_INS_VQRSHRUN;
		bool sat = i->id == ARM_INS_VQSHRN || i->id == ARM_INS_VQSHRUN
				|| i->id == ARM_INS_VQRSHRN || i->id == ARM_INS_VQRSHRUN;
		bool srcSigned = (i->id == ARM_INS_VQSHRUN || i->id == ARM_INS_VQRSHRUN) || isSigned;
		bool dstUnsigned = (i->id == ARM_INS_VQSHRUN || i->id == ARM_INS_VQRSHRUN) || !isSigned;
		llvm::Value* v = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
		auto* dstTy = llvm::FixedVectorType::get(irb.getIntNTy(dstLaneBits), lanes);
		auto* dstLaneTy = irb.getIntNTy(dstLaneBits);
		auto* srcLaneTy = irb.getIntNTy(laneBits);
		auto* wideTy = irb.getIntNTy(laneBits * 2);
		unsigned wideBits = laneBits * 2;
		llvm::APInt loA = dstUnsigned ? llvm::APInt(wideBits, 0)
									  : llvm::APInt::getSignedMinValue(dstLaneBits).sext(wideBits);
		llvm::APInt hiA = dstUnsigned ? llvm::APInt::getMaxValue(dstLaneBits).zext(wideBits)
									  : llvm::APInt::getSignedMaxValue(dstLaneBits).sext(wideBits);
		auto* loC = llvm::ConstantInt::get(wideTy, loA);
		auto* hiC = llvm::ConstantInt::get(wideTy, hiA);
		llvm::Value* res = llvm::PoisonValue::get(dstTy);
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* e = irb.CreateExtractElement(v, k);
			if (round && amount > 0)
			{
				e = irb.CreateAdd(e, llvm::ConstantInt::get(srcLaneTy, 1ull << (amount - 1)));
			}
			llvm::Value* sh = nullptr;
			if (amount >= laneBits)
			{
				sh = srcSigned ? irb.CreateAShr(e, laneBits - 1) : llvm::ConstantInt::get(srcLaneTy, 0);
			}
			else if (amount == 0)
			{
				sh = e;
			}
			else
			{
				sh = srcSigned ? irb.CreateAShr(e, amount) : irb.CreateLShr(e, amount);
			}
			if (!sat)
			{
				res = irb.CreateInsertElement(res, irb.CreateTrunc(sh, dstLaneTy), k);
				continue;
			}
			llvm::Value* aw = srcSigned ? irb.CreateSExt(sh, wideTy) : irb.CreateZExt(sh, wideTy);
			llvm::Value* c = irb.CreateSelect(irb.CreateICmpSLT(aw, loC), loC, aw);
			c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
			res = irb.CreateInsertElement(res, irb.CreateTrunc(c, dstLaneTy), k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

	// VSHLL / VMOVL: each source D-lane is extended to double width in Q
	// (PMOVSX/PMOVZX). VSHLL then shifts left by the immediate; VMOVL is
	// the zero-shift form (ARM64 SSHLL/USHLL #0).
	if (i->id == ARM_INS_VSHLL || i->id == ARM_INS_VMOVL)
	{
		bool movl = i->id == ARM_INS_VMOVL;
		if (isFp || laneBits == 0 || laneBits > 32)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		if (movl ? (ai->op_count != 2)
				 : (ai->op_count != 3 || ai->operands[2].type != ARM_OP_IMM))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		if (ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG
			|| !isNeonRegister(ai->operands[0].reg) || !isNeonRegister(ai->operands[1].reg)
			|| operandLane(ai->operands[0]) >= 0 || operandLane(ai->operands[1]) >= 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned dstW = neonBitWidth(ai->operands[0].reg);
		unsigned srcW = neonBitWidth(ai->operands[1].reg);
		unsigned dstLaneBits = laneBits * 2;
		if (dstW != srcW * 2 || srcW % laneBits != 0 || dstW % dstLaneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned lanes = srcW / laneBits;
		if (lanes == 0 || dstW / dstLaneBits != lanes)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}

		uint64_t amount = movl ? 0 : static_cast<uint64_t>(ai->operands[2].imm);
		llvm::Value* v = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
		auto* dstTy = llvm::FixedVectorType::get(irb.getIntNTy(dstLaneBits), lanes);
		auto* wide = irb.getIntNTy(dstLaneBits);
		llvm::Value* res = llvm::PoisonValue::get(dstTy);
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* e = irb.CreateExtractElement(v, k);
			e = isSigned ? irb.CreateSExt(e, wide) : irb.CreateZExt(e, wide);
			llvm::Value* sh = nullptr;
			if (amount >= dstLaneBits)
			{
				sh = llvm::ConstantInt::get(wide, 0);
			}
			else if (amount == 0)
			{
				sh = e;
			}
			else
			{
				sh = irb.CreateShl(e, amount);
			}
			res = irb.CreateInsertElement(res, sh, k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

	if (i->id == ARM_INS_VSLI || i->id == ARM_INS_VSRI)
	{
		if (laneBits == 0 && ai->vector_size > 0)
		{
			laneBits = static_cast<unsigned>(ai->vector_size);
		}
		unsigned srcOp = 1;
		unsigned immOp = 2;
		if (ai->op_count >= 4 && ai->operands[0].type == ARM_OP_REG && ai->operands[1].type == ARM_OP_REG
			&& ai->operands[1].reg == ai->operands[0].reg)
		{
			srcOp = 2;
			immOp = 3;
		}
		if (isFp || laneBits == 0 || immOp + 1 != ai->op_count || ai->operands[immOp].type != ARM_OP_IMM
			|| ai->operands[0].type != ARM_OP_REG || ai->operands[srcOp].type != ARM_OP_REG
			|| !isNeonRegister(ai->operands[0].reg) || !isNeonRegister(ai->operands[srcOp].reg)
			|| operandLane(ai->operands[0]) >= 0 || operandLane(ai->operands[srcOp]) >= 0
			|| neonBitWidth(ai->operands[0].reg) % laneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		bits = neonBitWidth(ai->operands[0].reg);
		uint64_t amount = static_cast<uint64_t>(ai->operands[immOp].imm);
		llvm::Value* src = loadNeonVector(ai->operands[srcOp].reg, laneBits, false, irb);
		llvm::Value* dest = loadNeonVector(ai->operands[0].reg, laneBits, false, irb);
		auto* vecTy = llvm::cast<llvm::FixedVectorType>(src->getType());
		unsigned lanes = vecTy->getNumElements();
		auto* laneTy = irb.getIntNTy(laneBits);
		llvm::Value* res = llvm::PoisonValue::get(vecTy);
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* s = irb.CreateExtractElement(src, k);
			llvm::Value* d = irb.CreateExtractElement(dest, k);
			llvm::Value* o = nullptr;
			if (amount == 0)
			{
				o = s;
			}
			else if (amount >= laneBits)
			{
				o = d;
			}
			else if (i->id == ARM_INS_VSLI)
			{
				llvm::APInt keep = llvm::APInt::getLowBitsSet(laneBits, static_cast<unsigned>(amount));
				o = irb.CreateOr(irb.CreateShl(s, amount), irb.CreateAnd(d, llvm::ConstantInt::get(laneTy, keep)));
			}
			else
			{
				llvm::APInt keep = llvm::APInt::getHighBitsSet(laneBits, static_cast<unsigned>(amount));
				o = irb.CreateOr(
						isSigned ? irb.CreateAShr(s, amount) : irb.CreateLShr(s, amount),
						irb.CreateAnd(d, llvm::ConstantInt::get(laneTy, keep)));
			}
			res = irb.CreateInsertElement(res, o, k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

	if (i->id == ARM_INS_VRSHL || i->id == ARM_INS_VQRSHL)
	{
		if (isFp || laneBits == 0 || ai->op_count != 3 || !neonBinaryRegs(ai, 3, bits) || bits % laneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
		llvm::Value* b = loadNeonVector(ai->operands[2].reg, laneBits, false, irb);
		auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
		unsigned lanes = vecTy->getNumElements();
		auto* laneTy = irb.getIntNTy(laneBits);
		auto* i8 = irb.getInt8Ty();
		auto* i32 = irb.getInt32Ty();
		unsigned wideBits = laneBits * 2;
		auto* wideTy = irb.getIntNTy(wideBits);
		bool sat = i->id == ARM_INS_VQRSHL;
		llvm::APInt loA = isSigned ? llvm::APInt::getSignedMinValue(laneBits).sext(wideBits) : llvm::APInt(wideBits, 0);
		llvm::APInt hiA = isSigned ? llvm::APInt::getSignedMaxValue(laneBits).sext(wideBits)
								   : llvm::APInt::getMaxValue(laneBits).zext(wideBits);
		auto* loC = llvm::ConstantInt::get(wideTy, loA);
		auto* hiC = llvm::ConstantInt::get(wideTy, hiA);
		llvm::Value* res = llvm::PoisonValue::get(vecTy);
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* e = irb.CreateExtractElement(a, k);
			llvm::Value* amt8 = irb.CreateTrunc(irb.CreateExtractElement(b, k), i8);
			llvm::Value* amt = irb.CreateSExt(amt8, i32);
			llvm::Value* left = irb.CreateICmpSGE(amt, llvm::ConstantInt::get(i32, 0));
			llvm::Value* mag = irb.CreateSelect(left, amt, irb.CreateNeg(amt));
			llvm::Value* tooBig = irb.CreateICmpUGE(mag, llvm::ConstantInt::get(i32, laneBits));
			llvm::Value* aw = isSigned ? irb.CreateSExt(e, wideTy) : irb.CreateZExt(e, wideTy);
			llvm::Value* shL = irb.CreateShl(aw, irb.CreateZExt(mag, wideTy));
			llvm::Value* rnd = irb.CreateShl(llvm::ConstantInt::get(wideTy, 1), irb.CreateSub(irb.CreateZExt(mag, wideTy), llvm::ConstantInt::get(wideTy, 1)));
			llvm::Value* addR = irb.CreateAdd(aw, irb.CreateSelect(irb.CreateICmpUGT(mag, llvm::ConstantInt::get(i32, 0)), rnd, llvm::ConstantInt::get(wideTy, 0)));
			llvm::Value* shR = isSigned ? irb.CreateAShr(addR, irb.CreateZExt(mag, wideTy))
										: irb.CreateLShr(addR, irb.CreateZExt(mag, wideTy));
			llvm::Value* zeroW = llvm::ConstantInt::get(wideTy, 0);
			llvm::Value* ashrAll = isSigned ? irb.CreateAShr(aw, laneBits - 1) : zeroW;
			llvm::Value* wide = irb.CreateSelect(left, irb.CreateSelect(tooBig, zeroW, shL), irb.CreateSelect(tooBig, ashrAll, shR));
			if (sat)
			{
				wide = irb.CreateSelect(irb.CreateICmpSLT(wide, loC), loC, wide);
				wide = irb.CreateSelect(irb.CreateICmpSGT(wide, hiC), hiC, wide);
			}
			res = irb.CreateInsertElement(res, irb.CreateTrunc(wide, laneTy), k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

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
	bool roundShr = i->id == ARM_INS_VRSHR || i->id == ARM_INS_VRSRA;
	llvm::Value* acc = (i->id == ARM_INS_VSRA || i->id == ARM_INS_VRSRA)
							   ? loadNeonVector(ai->operands[0].reg, laneBits, false, irb)
							   : nullptr;
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
			if (roundShr && amount > 0 && amount <= laneBits)
			{
				e = irb.CreateAdd(e, llvm::ConstantInt::get(e->getType(), 1ull << (amount - 1)));
			}
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

void Capstone2LlvmIrTranslatorArm_impl::translateNeonPermute(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (laneBits == 0 && ai->vector_size > 0)
	{
		laneBits = static_cast<unsigned>(ai->vector_size);
	}
	if (ai->op_count != 2 || !neonBinaryRegs(ai, 2, bits) || laneBits == 0 || bits % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned lanes = bits / laneBits;
	if (lanes < 2)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* a = loadNeonVector(ai->operands[0].reg, laneBits, false, irb);
	llvm::Value* b = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
	unsigned half = lanes / 2;
	llvm::SmallVector<int, 16> m1;
	llvm::SmallVector<int, 16> m2;
	m1.reserve(lanes);
	m2.reserve(lanes);
	for (unsigned k = 0; k < lanes; ++k)
	{
		switch (i->id)
		{
		case ARM_INS_VZIP:
			m1.push_back((k % 2) ? int(lanes + k / 2) : int(k / 2));
			m2.push_back((k % 2) ? int(lanes + half + k / 2) : int(half + k / 2));
			break;
		case ARM_INS_VUZP:
			m1.push_back(int(2 * k));
			m2.push_back(int(2 * k + 1));
			break;
		case ARM_INS_VTRN:
			m1.push_back((k % 2) ? int(lanes + k - 1) : int(k));
			m2.push_back((k % 2) ? int(lanes + k) : int(k + 1));
			break;
		default:
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}

	storeNeonVector(ai->operands[0].reg, irb.CreateShuffleVector(a, b, m1), irb);
	storeNeonVector(ai->operands[1].reg, irb.CreateShuffleVector(a, b, m2), irb);
}

void Capstone2LlvmIrTranslatorArm_impl::translateNeonTbl(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count < 3)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned nTable = ai->op_count - 2;
	if (nTable < 1 || nTable > 4 || ai->operands[0].type != ARM_OP_REG || neonBitWidth(ai->operands[0].reg) != 64)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	unsigned last = ai->op_count - 1;
	if (ai->operands[last].type != ARM_OP_REG || neonBitWidth(ai->operands[last].reg) != 64)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	for (unsigned j = 1; j <= nTable; ++j)
	{
		if (ai->operands[j].type != ARM_OP_REG || neonBitWidth(ai->operands[j].reg) != 64
			|| operandLane(ai->operands[j]) >= 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}

	unsigned nBytes = nTable * 8;
	auto* i8 = irb.getInt8Ty();
	auto* tblTy = llvm::FixedVectorType::get(i8, nBytes);
	llvm::Value* tbl = llvm::PoisonValue::get(tblTy);
	unsigned pos = 0;
	for (unsigned j = 1; j <= nTable; ++j)
	{
		llvm::Value* v = loadNeonVector(ai->operands[j].reg, 8, false, irb);
		for (uint64_t b = 0; b < 8; ++b)
		{
			tbl = irb.CreateInsertElement(tbl, irb.CreateExtractElement(v, b), pos++);
		}
	}

	llvm::Value* idxV = loadNeonVector(ai->operands[last].reg, 8, false, irb);
	llvm::Value* orig = (i->id == ARM_INS_VTBX) ? loadNeonVector(ai->operands[0].reg, 8, false, irb) : nullptr;
	auto* outTy = llvm::cast<llvm::FixedVectorType>(idxV->getType());
	llvm::Value* res = llvm::PoisonValue::get(outTy);
	auto* i32 = irb.getInt32Ty();
	auto* limit = llvm::ConstantInt::get(i32, nBytes);
	auto* zeroIdx = llvm::ConstantInt::get(i32, 0);
	auto* zeroByte = llvm::ConstantInt::get(i8, 0);
	for (uint64_t k = 0; k < 8; ++k)
	{
		llvm::Value* e = irb.CreateZExt(irb.CreateExtractElement(idxV, k), i32);
		llvm::Value* inRange = irb.CreateICmpULT(e, limit);
		llvm::Value* safe = irb.CreateSelect(inRange, e, zeroIdx);
		llvm::Value* looked = irb.CreateExtractElement(tbl, safe);
		llvm::Value* fallback = orig ? irb.CreateExtractElement(orig, k) : zeroByte;
		res = irb.CreateInsertElement(res, irb.CreateSelect(inRange, looked, fallback), k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
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

/**
 * ARM_INS_VADDL, ARM_INS_VSUBL, ARM_INS_VADDW, ARM_INS_VSUBW, ARM_INS_VMULL,
 * ARM_INS_VMLAL, ARM_INS_VMLSL, ARM_INS_VABDL, ARM_INS_VABAL, ARM_INS_VADDHN,
 * ARM_INS_VRADDHN, ARM_INS_VSUBHN, ARM_INS_VRSUBHN
 *
 * L/W/MULL widen: each narrow lane is extended to double width, then added,
 * subtracted, or multiplied into a Q destination (PMULLD's long product,
 * ARM64 SADDW for the W forms). VMLAL is that product accumulated (ARM64
 * SMLAL); VMLSL subtracts the product (ARM64 SMLSL). VABDL/VABAL are the
 * widening/accumulating abs-diff (VABD then extend). VADDHN is the inverse:
 * add at the wide width and keep the high half (ARM64 ADDHN); VRADDHN rounds
 * first, VSUBHN subtracts, VRSUBHN is the rounding subtract.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonWiden(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	// ARM_INS_PMULL / SHA512 / SM3 / SM4 are not in Capstone 6 arm.h. The
	// PCLMUL analogue is VMULL.P8 (ARM_VECTORDATA_P8) on ARM_INS_VMULL.
	if (i->id == ARM_INS_VMULL
		&& (ai->vector_data == ARM_VECTORDATA_P8 || ai->vector_data == ARM_VECTORDATA_P16)
		&& ai->op_count == 3)
	{
		unsigned srcLaneBits = (ai->vector_data == ARM_VECTORDATA_P8) ? 8u : 16u;
		for (unsigned j = 0; j < 3; ++j)
		{
			if (ai->operands[j].type != ARM_OP_REG || !isNeonRegister(ai->operands[j].reg)
				|| operandLane(ai->operands[j]) >= 0)
			{
				translatePseudoAsmGeneric(i, ai, irb);
				return;
			}
		}
		unsigned dstW = neonBitWidth(ai->operands[0].reg);
		unsigned aW = neonBitWidth(ai->operands[1].reg);
		unsigned bW = neonBitWidth(ai->operands[2].reg);
		unsigned dstLaneBits = srcLaneBits * 2;
		if (aW != bW || dstW != aW * 2 || aW % srcLaneBits != 0 || dstW % dstLaneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned lanes = aW / srcLaneBits;
		if (lanes == 0 || dstW / dstLaneBits != lanes)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		llvm::Value* a = loadNeonVector(ai->operands[1].reg, srcLaneBits, false, irb);
		llvm::Value* b = loadNeonVector(ai->operands[2].reg, srcLaneBits, false, irb);
		auto* dstTy = llvm::FixedVectorType::get(irb.getIntNTy(dstLaneBits), lanes);
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(
				_module, llvm::Intrinsic::arm_neon_vmullp, {dstTy});
		storeNeonVector(ai->operands[0].reg, irb.CreateCall(f, {a, b}), irb);
		return;
	}

	bool isFp = false;
	bool isSigned = false;
	unsigned srcLaneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (isFp || srcLaneBits == 0 || ai->op_count != 3)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	for (unsigned j = 0; j < 3; ++j)
	{
		if (ai->operands[j].type != ARM_OP_REG || !isNeonRegister(ai->operands[j].reg)
			|| operandLane(ai->operands[j]) >= 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}

	unsigned dstW = neonBitWidth(ai->operands[0].reg);
	unsigned aW = neonBitWidth(ai->operands[1].reg);
	unsigned bW = neonBitWidth(ai->operands[2].reg);

	if (i->id == ARM_INS_VADDHN || i->id == ARM_INS_VRADDHN || i->id == ARM_INS_VSUBHN
		|| i->id == ARM_INS_VRSUBHN)
	{
		unsigned dstLaneBits = srcLaneBits / 2;
		if (dstLaneBits == 0 || aW != bW || aW != dstW * 2 || aW % srcLaneBits != 0
			|| dstW % dstLaneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned lanes = aW / srcLaneBits;
		if (lanes == 0 || dstW / dstLaneBits != lanes)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}

		llvm::Value* a = loadNeonVector(ai->operands[1].reg, srcLaneBits, false, irb);
		llvm::Value* b = loadNeonVector(ai->operands[2].reg, srcLaneBits, false, irb);
		auto* dstTy = llvm::FixedVectorType::get(irb.getIntNTy(dstLaneBits), lanes);
		auto* narrow = irb.getIntNTy(dstLaneBits);
		llvm::Value* res = llvm::PoisonValue::get(dstTy);
		bool isSub = i->id == ARM_INS_VSUBHN || i->id == ARM_INS_VRSUBHN;
		bool round = i->id == ARM_INS_VRADDHN || i->id == ARM_INS_VRSUBHN;
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* ea = irb.CreateExtractElement(a, k);
			llvm::Value* eb = irb.CreateExtractElement(b, k);
			llvm::Value* sum = isSub ? irb.CreateSub(ea, eb) : irb.CreateAdd(ea, eb);
			if (round)
			{
				sum = irb.CreateAdd(sum, llvm::ConstantInt::get(sum->getType(), 1ull << (dstLaneBits - 1)));
			}
			res = irb.CreateInsertElement(res, irb.CreateTrunc(irb.CreateLShr(sum, dstLaneBits), narrow), k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

	if (srcLaneBits > 32
		|| (i->id != ARM_INS_VADDL && i->id != ARM_INS_VSUBL && i->id != ARM_INS_VADDW
			&& i->id != ARM_INS_VSUBW && i->id != ARM_INS_VMULL && i->id != ARM_INS_VMLAL
			&& i->id != ARM_INS_VMLSL && i->id != ARM_INS_VABDL && i->id != ARM_INS_VABAL))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	bool wideFirst = i->id == ARM_INS_VADDW || i->id == ARM_INS_VSUBW;
	unsigned dstLaneBits = srcLaneBits * 2;
	unsigned lanes = 0;
	if (wideFirst)
	{
		if (dstW != aW || bW * 2 != aW || aW % dstLaneBits != 0 || bW % srcLaneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		lanes = bW / srcLaneBits;
		if (lanes == 0 || dstW / dstLaneBits != lanes)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}
	else if (aW != bW || dstW != aW * 2 || aW % srcLaneBits != 0 || dstW % dstLaneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	else
	{
		lanes = aW / srcLaneBits;
		if (lanes == 0 || dstW / dstLaneBits != lanes)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}

	llvm::Value* a = loadNeonVector(ai->operands[1].reg, wideFirst ? dstLaneBits : srcLaneBits, false, irb);
	llvm::Value* b = loadNeonVector(ai->operands[2].reg, srcLaneBits, false, irb);
	auto* dstTy = llvm::FixedVectorType::get(irb.getIntNTy(dstLaneBits), lanes);
	auto* wide = irb.getIntNTy(dstLaneBits);
	llvm::Value* res = llvm::PoisonValue::get(dstTy);
	bool isSub = i->id == ARM_INS_VSUBL || i->id == ARM_INS_VSUBW;
	bool isMul = i->id == ARM_INS_VMULL || i->id == ARM_INS_VMLAL || i->id == ARM_INS_VMLSL;
	bool isAbd = i->id == ARM_INS_VABDL || i->id == ARM_INS_VABAL;
	bool accSub = i->id == ARM_INS_VMLSL;
	llvm::Value* acc = (i->id == ARM_INS_VMLAL || i->id == ARM_INS_VABAL || i->id == ARM_INS_VMLSL)
						   ? loadNeonVector(ai->operands[0].reg, dstLaneBits, false, irb)
						   : nullptr;
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* ea = irb.CreateExtractElement(a, k);
		llvm::Value* eb = irb.CreateExtractElement(b, k);
		if (!wideFirst)
		{
			ea = isSigned ? irb.CreateSExt(ea, wide) : irb.CreateZExt(ea, wide);
		}
		eb = isSigned ? irb.CreateSExt(eb, wide) : irb.CreateZExt(eb, wide);
		llvm::Value* o = nullptr;
		if (isAbd)
		{
			llvm::Value* d = irb.CreateSub(ea, eb);
			o = irb.CreateSelect(irb.CreateICmpSLT(d, llvm::ConstantInt::get(wide, 0)), irb.CreateNeg(d), d);
		}
		else
		{
			o = isMul ? irb.CreateMul(ea, eb) : (isSub ? irb.CreateSub(ea, eb) : irb.CreateAdd(ea, eb));
		}
		if (acc)
		{
			llvm::Value* a0 = irb.CreateExtractElement(acc, k);
			o = accSub ? irb.CreateSub(a0, o) : irb.CreateAdd(a0, o);
		}
		res = irb.CreateInsertElement(res, o, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

/**
 * ARM_INS_VPADD, ARM_INS_VPADDL, ARM_INS_VPMAX, ARM_INS_VPMIN
 *
 * Pairwise add/max/min of adjacent lanes, the NEON form of HADDPS / pairwise
 * min/max. VPADD/VPMAX/VPMIN keep the lane width and concatenate the pairs of
 * Dn then Dm; VPADDL widens each pair of a single source into a double-width
 * lane.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonPairwise(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool isFp = false;
	bool isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (laneBits == 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	if (i->id == ARM_INS_VPADD || i->id == ARM_INS_VPMAX || i->id == ARM_INS_VPMIN)
	{
		unsigned bits = 0;
		if (ai->op_count != 3 || !neonBinaryRegs(ai, 3, bits) || bits % laneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned lanes = bits / laneBits;
		if (lanes < 2 || (lanes % 2) != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}

		llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, isFp, irb);
		llvm::Value* b = loadNeonVector(ai->operands[2].reg, laneBits, isFp, irb);
		auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
		llvm::Value* res = llvm::PoisonValue::get(vecTy);
		unsigned half = lanes / 2;
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* src = k < half ? a : b;
			uint64_t pair = k % half;
			llvm::Value* e0 = irb.CreateExtractElement(src, 2 * pair);
			llvm::Value* e1 = irb.CreateExtractElement(src, 2 * pair + 1);
			llvm::Value* o = nullptr;
			if (i->id == ARM_INS_VPMAX)
			{
				o = isFp ? irb.CreateSelect(irb.CreateFCmpOGT(e0, e1), e0, e1)
						 : irb.CreateSelect(isSigned ? irb.CreateICmpSGT(e0, e1) : irb.CreateICmpUGT(e0, e1), e0, e1);
			}
			else if (i->id == ARM_INS_VPMIN)
			{
				o = isFp ? irb.CreateSelect(irb.CreateFCmpOLT(e0, e1), e0, e1)
						 : irb.CreateSelect(isSigned ? irb.CreateICmpSLT(e0, e1) : irb.CreateICmpULT(e0, e1), e0, e1);
			}
			else
			{
				o = isFp ? irb.CreateFAdd(e0, e1) : irb.CreateAdd(e0, e1);
			}
			res = irb.CreateInsertElement(res, o, k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

	unsigned bits = 0;
	unsigned srcOp = 1;
	if (ai->op_count >= 3 && ai->operands[0].type == ARM_OP_REG && ai->operands[1].type == ARM_OP_REG
		&& ai->operands[1].reg == ai->operands[0].reg)
	{
		srcOp = 2;
	}
	if ((ai->op_count != srcOp + 1) || isFp || !neonBinaryRegs(ai, 1, bits)
		|| ai->operands[srcOp].type != ARM_OP_REG || !isNeonRegister(ai->operands[srcOp].reg)
		|| operandLane(ai->operands[srcOp]) >= 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	unsigned srcW = neonBitWidth(ai->operands[srcOp].reg);
	unsigned dstW = neonBitWidth(ai->operands[0].reg);
	if (srcW != dstW || srcW % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	unsigned srcLanes = srcW / laneBits;
	if (srcLanes < 2 || (srcLanes % 2) != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	unsigned dstLaneBits = laneBits * 2;
	unsigned dstLanes = srcLanes / 2;
	if (dstW % dstLaneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* v = loadNeonVector(ai->operands[srcOp].reg, laneBits, false, irb);
	auto* dstTy = llvm::FixedVectorType::get(irb.getIntNTy(dstLaneBits), dstLanes);
	auto* wide = irb.getIntNTy(dstLaneBits);
	llvm::Value* acc = (i->id == ARM_INS_VPADAL) ? loadNeonVector(ai->operands[0].reg, dstLaneBits, false, irb)
												: nullptr;
	llvm::Value* res = llvm::PoisonValue::get(dstTy);
	for (uint64_t k = 0; k < dstLanes; ++k)
	{
		llvm::Value* e0 = irb.CreateExtractElement(v, 2 * k);
		llvm::Value* e1 = irb.CreateExtractElement(v, 2 * k + 1);
		e0 = isSigned ? irb.CreateSExt(e0, wide) : irb.CreateZExt(e0, wide);
		e1 = isSigned ? irb.CreateSExt(e1, wide) : irb.CreateZExt(e1, wide);
		llvm::Value* s = irb.CreateAdd(e0, e1);
		if (acc)
		{
			s = irb.CreateAdd(irb.CreateExtractElement(acc, k), s);
		}
		res = irb.CreateInsertElement(res, s, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

/**
 * ARM_INS_VQADD, ARM_INS_VQSUB, ARM_INS_VQMOVN, ARM_INS_VQMOVUN, ARM_INS_VMOVN,
 * ARM_INS_VQABS, ARM_INS_VQNEG
 *
 * Saturating lane add/sub, the NEON counterpart of PADDSB/PADDUSB/PSUBSB,
 * and saturating narrow (PACKSSWB / ARM64 SQXTN). VQMOVUN is signed-to-unsigned
 * sat narrow (PACKUSWB): -1 becomes 0, not 255. VMOVN is the truncating
 * narrow (ARM64 XTN). VQABS/VQNEG are saturating abs/neg (ARM64 SQABS/SQNEG):
 * abs of -128 is 127, not -128. A wrapping add of 127 and 1 is -128; this
 * stops at 127. The same select clamp VQADD uses, not llvm.ssub.sat.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonSatArith(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isFp = false;
	bool isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (isFp || laneBits == 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	if (i->id == ARM_INS_VQMOVN || i->id == ARM_INS_VQMOVUN || i->id == ARM_INS_VMOVN)
	{
		if (ai->op_count != 2 || laneBits < 16 || (laneBits % 2) != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		if (ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG
			|| !isNeonRegister(ai->operands[0].reg) || !isNeonRegister(ai->operands[1].reg)
			|| operandLane(ai->operands[0]) >= 0 || operandLane(ai->operands[1]) >= 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned dstW = neonBitWidth(ai->operands[0].reg);
		unsigned srcW = neonBitWidth(ai->operands[1].reg);
		unsigned dstLaneBits = laneBits / 2;
		if (srcW != dstW * 2 || srcW % laneBits != 0 || dstW % dstLaneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned lanes = srcW / laneBits;
		if (lanes == 0 || dstW / dstLaneBits != lanes)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}

		llvm::Value* v = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
		auto* dstTy = llvm::FixedVectorType::get(irb.getIntNTy(dstLaneBits), lanes);
		auto* dstLaneTy = irb.getIntNTy(dstLaneBits);
		llvm::Value* res = llvm::PoisonValue::get(dstTy);
		bool srcSigned = (i->id == ARM_INS_VQMOVUN) || isSigned;
		bool dstUnsigned = (i->id == ARM_INS_VQMOVUN) || !isSigned;
		auto* wideTy = irb.getIntNTy(laneBits * 2);
		unsigned wideBits = laneBits * 2;
		llvm::APInt loA = dstUnsigned ? llvm::APInt(wideBits, 0)
									  : llvm::APInt::getSignedMinValue(dstLaneBits).sext(wideBits);
		llvm::APInt hiA = dstUnsigned ? llvm::APInt::getMaxValue(dstLaneBits).zext(wideBits)
									  : llvm::APInt::getSignedMaxValue(dstLaneBits).sext(wideBits);
		auto* loC = llvm::ConstantInt::get(wideTy, loA);
		auto* hiC = llvm::ConstantInt::get(wideTy, hiA);
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* e = irb.CreateExtractElement(v, k);
			if (i->id == ARM_INS_VMOVN)
			{
				res = irb.CreateInsertElement(res, irb.CreateTrunc(e, dstLaneTy), k);
				continue;
			}
			llvm::Value* aw = srcSigned ? irb.CreateSExt(e, wideTy) : irb.CreateZExt(e, wideTy);
			llvm::Value* c = irb.CreateSelect(irb.CreateICmpSLT(aw, loC), loC, aw);
			c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
			res = irb.CreateInsertElement(res, irb.CreateTrunc(c, dstLaneTy), k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

	if (i->id == ARM_INS_VQABS || i->id == ARM_INS_VQNEG)
	{
		if (ai->op_count != 2 || !neonBinaryRegs(ai, 2, bits) || bits % laneBits != 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}

		llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
		auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
		unsigned lanes = vecTy->getNumElements();
		auto* laneTy = irb.getIntNTy(laneBits);
		auto* wideTy = irb.getIntNTy(laneBits * 2);
		unsigned wideBits = laneBits * 2;
		llvm::APInt loA = llvm::APInt::getSignedMinValue(laneBits).sext(wideBits);
		llvm::APInt hiA = llvm::APInt::getSignedMaxValue(laneBits).sext(wideBits);
		auto* loC = llvm::ConstantInt::get(wideTy, loA);
		auto* hiC = llvm::ConstantInt::get(wideTy, hiA);
		auto* zeroW = llvm::ConstantInt::get(wideTy, 0);
		llvm::Value* res = llvm::PoisonValue::get(vecTy);
		for (uint64_t k = 0; k < lanes; ++k)
		{
			llvm::Value* e = irb.CreateExtractElement(a, k);
			llvm::Value* aw = irb.CreateSExt(e, wideTy);
			llvm::Value* neg = irb.CreateSub(zeroW, aw);
			llvm::Value* wide = (i->id == ARM_INS_VQABS)
									? irb.CreateSelect(irb.CreateICmpSLT(aw, zeroW), neg, aw)
									: neg;
			llvm::Value* c = irb.CreateSelect(irb.CreateICmpSLT(wide, loC), loC, wide);
			c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
			res = irb.CreateInsertElement(res, irb.CreateTrunc(c, laneTy), k);
		}
		storeNeonVector(ai->operands[0].reg, res, irb);
		return;
	}

	if ((i->id != ARM_INS_VQADD && i->id != ARM_INS_VQSUB) || ai->op_count != 3
		|| !neonBinaryRegs(ai, 3, bits) || bits % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
	llvm::Value* b = loadNeonVector(ai->operands[2].reg, laneBits, false, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
	unsigned lanes = vecTy->getNumElements();
	auto* laneTy = irb.getIntNTy(laneBits);
	auto* wideTy = irb.getIntNTy(laneBits * 2);
	unsigned wideBits = laneBits * 2;
	llvm::APInt loA = isSigned ? llvm::APInt::getSignedMinValue(laneBits).sext(wideBits) : llvm::APInt(wideBits, 0);
	llvm::APInt hiA = isSigned ? llvm::APInt::getSignedMaxValue(laneBits).sext(wideBits)
							   : llvm::APInt::getMaxValue(laneBits).zext(wideBits);
	auto* loC = llvm::ConstantInt::get(wideTy, loA);
	auto* hiC = llvm::ConstantInt::get(wideTy, hiA);
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	bool isSub = i->id == ARM_INS_VQSUB;
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* ea = irb.CreateExtractElement(a, k);
		llvm::Value* eb = irb.CreateExtractElement(b, k);
		llvm::Value* aw = isSigned ? irb.CreateSExt(ea, wideTy) : irb.CreateZExt(ea, wideTy);
		llvm::Value* bw = isSigned ? irb.CreateSExt(eb, wideTy) : irb.CreateZExt(eb, wideTy);
		llvm::Value* wide = isSub ? irb.CreateSub(aw, bw) : irb.CreateAdd(aw, bw);
		llvm::Value* c = irb.CreateSelect(irb.CreateICmpSLT(wide, loC), loC, wide);
		c = irb.CreateSelect(irb.CreateICmpSGT(c, hiC), hiC, c);
		res = irb.CreateInsertElement(res, irb.CreateTrunc(c, laneTy), k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

/**
 * ARM_INS_VLD2/3/4, ARM_INS_VST2/3/4
 *
 * Multi-structure load/store: consecutive memory elements are de-interleaved
 * into N registers (or interleaved on store), the reverse of VZIP / PUNPCK.
 * Two-register VLD2 and three-/four-register VLD3/VLD4 use the same layout;
 * four-register VLD2 is two concatenated 2-way chunks. A common lane index
 * is the single-structure (lane) form.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonLdStN(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned nStruct = 0;
	bool load = false;
	switch (i->id)
	{
	case ARM_INS_VLD2:
		nStruct = 2;
		load = true;
		break;
	case ARM_INS_VST2: nStruct = 2; break;
	case ARM_INS_VLD3:
		nStruct = 3;
		load = true;
		break;
	case ARM_INS_VST3: nStruct = 3; break;
	case ARM_INS_VLD4:
		nStruct = 4;
		load = true;
		break;
	case ARM_INS_VST4: nStruct = 4; break;
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}

	if (ai->op_count < nStruct + 1)
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

	unsigned nRegs = memIdx;
	if (nRegs < nStruct || nRegs % nStruct != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	if ((nStruct == 2 && nRegs != 2 && nRegs != 4) || (nStruct == 3 && nRegs != 3) || (nStruct == 4 && nRegs != 4))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	bool isFp = false;
	bool isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (laneBits == 0 && ai->vector_size > 0)
	{
		laneBits = static_cast<unsigned>(ai->vector_size);
	}
	if (laneBits == 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	int commonLane = operandLane(ai->operands[0]);
	unsigned width0 = 0;
	for (unsigned j = 0; j < nRegs; ++j)
	{
		if (ai->operands[j].type != ARM_OP_REG || !isNeonRegister(ai->operands[j].reg)
			|| operandLane(ai->operands[j]) != commonLane)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		unsigned w = neonBitWidth(ai->operands[j].reg);
		if (width0 == 0)
		{
			width0 = w;
		}
		else if (w != width0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}
	if (width0 == 0 || width0 % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	unsigned lanes = width0 / laneBits;
	if (commonLane >= 0 && static_cast<unsigned>(commonLane) >= lanes)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* addr = loadOp(ai->operands[memIdx], irb, nullptr, true);
	auto* elemTy = irb.getIntNTy(laneBits);
	uint64_t elemBytes = laneBits / 8;
	auto* addrTy = addr->getType();

	if (commonLane >= 0)
	{
		if (nRegs != nStruct)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		for (unsigned s = 0; s < nStruct; ++s)
		{
			auto* at = irb.CreateAdd(addr, llvm::ConstantInt::get(addrTy, s * elemBytes));
			if (load)
			{
				llvm::Value* v = loadNeonVector(ai->operands[s].reg, laneBits, false, irb);
				auto* e = loadIntPtr(irb, at, elemTy);
				storeNeonVector(
					ai->operands[s].reg, irb.CreateInsertElement(v, e, static_cast<uint64_t>(commonLane)), irb);
			}
			else
			{
				llvm::Value* v = loadNeonVector(ai->operands[s].reg, laneBits, false, irb);
				storeIntPtr(irb, irb.CreateExtractElement(v, static_cast<uint64_t>(commonLane)), at, elemTy);
			}
		}
		if (insnWriteback())
		{
			uint32_t base = ai->operands[memIdx].mem.base;
			if (base != ARM_REG_INVALID)
			{
				storeRegister(
					base,
					irb.CreateAdd(loadRegister(base, irb), llvm::ConstantInt::get(getDefaultType(), nStruct * elemBytes)),
					irb);
			}
		}
		return;
	}

	auto* vecTy = llvm::FixedVectorType::get(elemTy, lanes);
	for (unsigned r = 0; r < nRegs; ++r)
	{
		unsigned structBase = (r / nStruct) * lanes;
		unsigned elem = r % nStruct;
		llvm::Value* v = load ? static_cast<llvm::Value*>(llvm::PoisonValue::get(vecTy))
							  : loadNeonVector(ai->operands[r].reg, laneBits, false, irb);
		for (unsigned lane = 0; lane < lanes; ++lane)
		{
			uint64_t memElem = static_cast<uint64_t>(structBase + lane) * nStruct + elem;
			auto* at = irb.CreateAdd(addr, llvm::ConstantInt::get(addrTy, memElem * elemBytes));
			if (load)
			{
				v = irb.CreateInsertElement(v, loadIntPtr(irb, at, elemTy), static_cast<uint64_t>(lane));
			}
			else
			{
				storeIntPtr(irb, irb.CreateExtractElement(v, static_cast<uint64_t>(lane)), at, elemTy);
			}
		}
		if (load)
		{
			storeNeonVector(ai->operands[r].reg, v, irb);
		}
	}

	if (insnWriteback())
	{
		uint32_t base = ai->operands[memIdx].mem.base;
		if (base != ARM_REG_INVALID)
		{
			uint64_t total = static_cast<uint64_t>(nRegs) * (width0 / 8);
			storeRegister(
				base, irb.CreateAdd(loadRegister(base, irb), llvm::ConstantInt::get(getDefaultType(), total)), irb);
		}
	}
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
			|| armIsId(i, ARM_INS_FLDMIAX) || armIsId(i, ARM_INS_FLDMDBX)
			|| armIsId(i, ARM_INS_VPOP);
	bool db = i->id == ARM_INS_VLDMDB || i->id == ARM_INS_VSTMDB
			|| armIsId(i, ARM_INS_FLDMDBX) || armIsId(i, ARM_INS_FSTMDBX)
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

/**
 * ARM_INS_AESE, ARM_INS_AESD, ARM_INS_AESMC, ARM_INS_AESIMC
 *
 * ARMv8 Crypto AES on Q registers. LLVM 23 maps these to
 * llvm.arm.neon.aese/aesd (<16 x i8>, <16 x i8>) and
 * llvm.arm.neon.aesmc/aesimc (<16 x i8>). Q is two D halves assembled to
 * i128 — do not go through fp128 (loadQuadView zeros bits [79:64]).
 */
void Capstone2LlvmIrTranslatorArm_impl::translateCryptoAes(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto isQ = [&](unsigned op) {
		return op < ai->op_count && ai->operands[op].type == ARM_OP_REG && isQuadView(ai->operands[op].reg)
				&& operandLane(ai->operands[op]) < 0;
	};
	// Capstone 6 AESE/AESD are Vd, src(=Vd), Vm. AESMC/AESIMC stay Vd, Vm.
	unsigned srcOp = 1;
	if (ai->op_count >= 3 && isQ(0) && isQ(1) && ai->operands[1].reg == ai->operands[0].reg)
	{
		srcOp = 2;
	}
	if (!isQ(0) || !isQ(srcOp) || srcOp + 1 != ai->op_count)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	auto* v16i8 = llvm::FixedVectorType::get(irb.getInt8Ty(), 16);
	auto* dest = irb.CreateBitCast(loadNeonBits(ai->operands[0].reg, irb), v16i8);
	auto* src = irb.CreateBitCast(loadNeonBits(ai->operands[srcOp].reg, irb), v16i8);

	llvm::Intrinsic::ID iid = llvm::Intrinsic::not_intrinsic;
	bool binary = false;
	switch (i->id)
	{
	case ARM_INS_AESE:
		iid = llvm::Intrinsic::arm_neon_aese;
		binary = true;
		break;
	case ARM_INS_AESD:
		iid = llvm::Intrinsic::arm_neon_aesd;
		binary = true;
		break;
	case ARM_INS_AESMC: iid = llvm::Intrinsic::arm_neon_aesmc; break;
	case ARM_INS_AESIMC: iid = llvm::Intrinsic::arm_neon_aesimc; break;
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}

	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid);
	llvm::Value* res = binary ? irb.CreateCall(f, {dest, src}) : irb.CreateCall(f, {src});
	storeNeonBits(ai->operands[0].reg, irb.CreateBitCast(res, irb.getIntNTy(128)), irb);
}

/**
 * ARM_INS_SHA1C/H/M/P/SU0/SU1, ARM_INS_SHA256H/H2/SU0/SU1
 *
 * ARMv8 Crypto SHA-1 / SHA-256 on Q registers. LLVM 23:
 * sha1c/m/p take (<4 x i32>, i32, <4 x i32>) — the i32 is Qn lane 0;
 * sha1h is i32 → i32; the rest are v4i32.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateCryptoSha(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	auto* i128 = irb.getIntNTy(128);
	auto* v4i32 = llvm::FixedVectorType::get(i32, 4);
	// Thumb T32 SHA is the A32 word with bit 24 moved to bit 28. Capstone
	// inverts that and decodes the same QPR operands as ARM: MC is
	// Vd, src(=Vd), Vn?, Vm; the printer omits the tied src, so the detail
	// is Vd, Vn?, Vm. A D-low-half view of the same Q is accepted too.
	uint32_t quads[4] = {};
	unsigned nq = 0;
	armCollectCryptoQuads(ai, quads, nq);
	auto loadQ = [&](unsigned op) { return irb.CreateBitCast(loadNeonBits(quads[op], irb), v4i32); };
	auto storeQ = [&](llvm::Value* v) { storeNeonBits(quads[0], irb.CreateBitCast(v, i128), irb); };
	auto ready = [&](unsigned need) {
		armDropTiedQuad(quads, nq, need);
		return nq >= need;
	};

	switch (i->id)
	{
	case ARM_INS_SHA1H: {
		if (!ready(2))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		auto* e = irb.CreateExtractElement(loadQ(1), uint64_t(0));
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::arm_neon_sha1h);
		auto* r = irb.CreateCall(f, {e});
		storeQ(irb.CreateInsertElement(llvm::Constant::getNullValue(v4i32), r, uint64_t(0)));
		return;
	}
	case ARM_INS_SHA1C:
	case ARM_INS_SHA1M:
	case ARM_INS_SHA1P: {
		if (!ready(3))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		llvm::Intrinsic::ID iid = (i->id == ARM_INS_SHA1C) ? llvm::Intrinsic::arm_neon_sha1c
								: (i->id == ARM_INS_SHA1M) ? llvm::Intrinsic::arm_neon_sha1m
														   : llvm::Intrinsic::arm_neon_sha1p;
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid);
		auto* e = irb.CreateExtractElement(loadQ(1), uint64_t(0));
		storeQ(irb.CreateCall(f, {loadQ(0), e, loadQ(2)}));
		return;
	}
	case ARM_INS_SHA1SU1:
	case ARM_INS_SHA256SU0: {
		if (!ready(2))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		auto iid = (i->id == ARM_INS_SHA1SU1) ? llvm::Intrinsic::arm_neon_sha1su1 : llvm::Intrinsic::arm_neon_sha256su0;
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid);
		storeQ(irb.CreateCall(f, {loadQ(0), loadQ(1)}));
		return;
	}
	case ARM_INS_SHA1SU0:
	case ARM_INS_SHA256H:
	case ARM_INS_SHA256H2:
	case ARM_INS_SHA256SU1: {
		if (!ready(3))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		llvm::Intrinsic::ID iid = llvm::Intrinsic::arm_neon_sha1su0;
		if (i->id == ARM_INS_SHA256H)
		{
			iid = llvm::Intrinsic::arm_neon_sha256h;
		}
		else if (i->id == ARM_INS_SHA256H2)
		{
			iid = llvm::Intrinsic::arm_neon_sha256h2;
		}
		else if (i->id == ARM_INS_SHA256SU1)
		{
			iid = llvm::Intrinsic::arm_neon_sha256su1;
		}
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid);
		storeQ(irb.CreateCall(f, {loadQ(0), loadQ(1), loadQ(2)}));
		return;
	}
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}
}

/**
 * ARM_INS_CRC32B/H/W, ARM_INS_CRC32CB/CH/CW
 *
 * ARM CRC extension. LLVM 23 llvm.arm.crc32{c,}{b,h,w}(i32, i32). There is
 * no CRC32X on AArch32.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateCrc32(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	// A32 is Rd, Rn, Rm. Thumb t2CRC32* is the same three GPRs (Rd in bits
	// 11:8, Rn in 19:16, Rm in 3:0). A tied dest-read or a trailing predicate
	// register must not hide those three.
	unsigned idx[4] = {};
	unsigned n = 0;
	for (unsigned op = 0; op < ai->op_count && n < 4; ++op)
	{
		if (ai->operands[op].type == ARM_OP_REG)
		{
			idx[n++] = op;
		}
	}
	if (n > 3 && ai->operands[idx[1]].reg == ai->operands[idx[0]].reg)
	{
		idx[1] = idx[2];
		idx[2] = idx[3];
		n = 3;
	}
	if (n < 3)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Intrinsic::ID iid = llvm::Intrinsic::not_intrinsic;
	switch (i->id)
	{
	case ARM_INS_CRC32B: iid = llvm::Intrinsic::arm_crc32b; break;
	case ARM_INS_CRC32H: iid = llvm::Intrinsic::arm_crc32h; break;
	case ARM_INS_CRC32W: iid = llvm::Intrinsic::arm_crc32w; break;
	case ARM_INS_CRC32CB: iid = llvm::Intrinsic::arm_crc32cb; break;
	case ARM_INS_CRC32CH: iid = llvm::Intrinsic::arm_crc32ch; break;
	case ARM_INS_CRC32CW: iid = llvm::Intrinsic::arm_crc32cw; break;
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}

	auto* i32 = irb.getInt32Ty();
	auto* acc = irb.CreateZExtOrTrunc(loadOp(ai->operands[idx[1]], irb), i32);
	auto* data = irb.CreateZExtOrTrunc(loadOp(ai->operands[idx[2]], irb), i32);
	auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid);
	storeOp(ai->operands[idx[0]], irb.CreateCall(f, {acc, data}), irb);
}

/**
 * ARM_INS_VCLZ, ARM_INS_VCNT, ARM_INS_VCLS
 *
 * VCLZ is llvm.ctlz (x86 LZCNT class). VCNT is llvm.ctpop (POPCNT).
 * VCLS is leading sign bits: ctlz(x ^ ashr(x,1)) - 1.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonCount(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (laneBits == 0 && ai->vector_size > 0)
	{
		laneBits = static_cast<unsigned>(ai->vector_size);
	}
	if (i->id == ARM_INS_VCNT)
	{
		laneBits = 8;
	}
	if (isFp || laneBits == 0 || ai->op_count != 2 || !neonBinaryRegs(ai, 2, bits) || bits % laneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* v = loadNeonVector(ai->operands[1].reg, laneBits, false, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(v->getType());
	unsigned lanes = vecTy->getNumElements();
	auto* laneTy = irb.getIntNTy(laneBits);
	auto* ctlz = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctlz, laneTy);
	auto* ctpop = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::ctpop, laneTy);
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* e = irb.CreateExtractElement(v, k);
		llvm::Value* o = nullptr;
		if (i->id == ARM_INS_VCNT)
		{
			o = irb.CreateCall(ctpop, {e});
		}
		else if (i->id == ARM_INS_VCLZ)
		{
			o = irb.CreateCall(ctlz, {e, irb.getFalse()});
		}
		else
		{
			llvm::Value* x = irb.CreateXor(e, irb.CreateAShr(e, 1));
			o = irb.CreateSub(irb.CreateCall(ctlz, {x, irb.getFalse()}), llvm::ConstantInt::get(laneTy, 1));
		}
		res = irb.CreateInsertElement(res, o, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

/**
 * ARM_INS_VRECPE, ARM_INS_VRECPS, ARM_INS_VRSQRTE, ARM_INS_VRSQRTS
 *
 * Estimates use llvm.arm.neon.vrecpe / vrsqrte (x86 RCPPS class). The Newton
 * steps are exact: VRECPS is 2-a*b, VRSQRTS is (3-a*b)/2.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonRecip(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	unsigned bits = 0;
	bool isFp = false, isSigned = false;
	unsigned laneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	bool unary = i->id == ARM_INS_VRECPE || i->id == ARM_INS_VRSQRTE;
	if (laneBits == 0 || !neonBinaryRegs(ai, unary ? 2 : 3, bits) || bits % laneBits != 0
		|| ai->op_count != (unary ? 2u : 3u))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	if (unary)
	{
		if (!isFp && !(i->id == ARM_INS_VRECPE && laneBits == 32))
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
		llvm::Value* v = loadNeonVector(ai->operands[1].reg, laneBits, isFp, irb);
		auto iid = (i->id == ARM_INS_VRECPE) ? llvm::Intrinsic::arm_neon_vrecpe : llvm::Intrinsic::arm_neon_vrsqrte;
		auto* f = llvm::Intrinsic::getOrInsertDeclaration(_module, iid, {v->getType()});
		storeNeonVector(ai->operands[0].reg, irb.CreateCall(f, {v}), irb);
		return;
	}
	if (!isFp)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	llvm::Value* a = loadNeonVector(ai->operands[1].reg, laneBits, true, irb);
	llvm::Value* b = loadNeonVector(ai->operands[2].reg, laneBits, true, irb);
	auto* vecTy = llvm::cast<llvm::FixedVectorType>(a->getType());
	unsigned lanes = vecTy->getNumElements();
	llvm::Value* res = llvm::PoisonValue::get(vecTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* ea = irb.CreateExtractElement(a, k);
		llvm::Value* eb = irb.CreateExtractElement(b, k);
		llvm::Value* prod = irb.CreateFMul(ea, eb);
		llvm::Value* o = nullptr;
		if (i->id == ARM_INS_VRECPS)
		{
			o = irb.CreateFSub(llvm::ConstantFP::get(ea->getType(), 2.0), prod);
		}
		else
		{
			o = irb.CreateFMul(
					irb.CreateFSub(llvm::ConstantFP::get(ea->getType(), 3.0), prod),
					llvm::ConstantFP::get(ea->getType(), 0.5));
		}
		res = irb.CreateInsertElement(res, o, k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
}

/**
 * ARM_INS_VQDMULH, ARM_INS_VQRDMULH, ARM_INS_VQDMULL,
 * ARM_INS_VQDMLAL, ARM_INS_VQDMLSL
 *
 * Saturating doubling multiply. LLVM 23 has llvm.arm.neon.vqdmulh / vqrdmulh /
 * vqdmull; the long accumulate forms are sat(acc ± vqdmull).
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonSatMul(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool isFp = false, isSigned = false;
	unsigned srcLaneBits = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	if (isFp || srcLaneBits == 0 || !isSigned || ai->op_count < 3)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	unsigned src0 = 1;
	if (ai->op_count >= 4 && ai->operands[0].type == ARM_OP_REG && ai->operands[1].type == ARM_OP_REG
		&& ai->operands[1].reg == ai->operands[0].reg)
	{
		src0 = 2;
	}
	if (src0 + 2 != ai->op_count)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	for (unsigned j : {0u, src0, src0 + 1})
	{
		if (ai->operands[j].type != ARM_OP_REG || !isNeonRegister(ai->operands[j].reg)
			|| operandLane(ai->operands[j]) >= 0)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}

	bool widen = i->id == ARM_INS_VQDMULL || i->id == ARM_INS_VQDMLAL || i->id == ARM_INS_VQDMLSL;
	unsigned dstW = neonBitWidth(ai->operands[0].reg);
	unsigned aW = neonBitWidth(ai->operands[src0].reg);
	unsigned bW = neonBitWidth(ai->operands[src0 + 1].reg);
	unsigned dstLaneBits = widen ? srcLaneBits * 2 : srcLaneBits;
	if (aW != bW || aW % srcLaneBits != 0 || dstW % dstLaneBits != 0)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	if (widen ? (dstW != aW * 2) : (dstW != aW))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	unsigned lanes = aW / srcLaneBits;
	if (lanes == 0 || dstW / dstLaneBits != lanes)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}

	llvm::Value* a = loadNeonVector(ai->operands[src0].reg, srcLaneBits, false, irb);
	llvm::Value* b = loadNeonVector(ai->operands[src0 + 1].reg, srcLaneBits, false, irb);
	auto* dstTy = llvm::FixedVectorType::get(irb.getIntNTy(dstLaneBits), lanes);
	auto* srcTy = irb.getIntNTy(srcLaneBits);
	auto* dstLaneTy = irb.getIntNTy(dstLaneBits);
	unsigned w4 = srcLaneBits * 4;
	auto* t4 = irb.getIntNTy(w4);
	bool round = i->id == ARM_INS_VQRDMULH;
	bool acc = i->id == ARM_INS_VQDMLAL || i->id == ARM_INS_VQDMLSL;
	bool accSub = i->id == ARM_INS_VQDMLSL;
	llvm::Value* dest = acc ? loadNeonVector(ai->operands[0].reg, dstLaneBits, false, irb) : nullptr;
	llvm::APInt loA = llvm::APInt::getSignedMinValue(dstLaneBits).sext(w4);
	llvm::APInt hiA = llvm::APInt::getSignedMaxValue(dstLaneBits).sext(w4);
	auto* loC = llvm::ConstantInt::get(t4, loA);
	auto* hiC = llvm::ConstantInt::get(t4, hiA);
	llvm::Value* res = llvm::PoisonValue::get(dstTy);
	for (uint64_t k = 0; k < lanes; ++k)
	{
		llvm::Value* ea = irb.CreateSExt(irb.CreateExtractElement(a, k), t4);
		llvm::Value* eb = irb.CreateSExt(irb.CreateExtractElement(b, k), t4);
		llvm::Value* dbl = irb.CreateShl(irb.CreateMul(ea, eb), 1);
		if (round)
		{
			dbl = irb.CreateAdd(dbl, llvm::ConstantInt::get(t4, 1ull << (srcLaneBits - 1)));
		}
		llvm::Value* hi = widen ? dbl : irb.CreateAShr(dbl, srcLaneBits);
		hi = irb.CreateSelect(irb.CreateICmpSLT(hi, loC), loC, hi);
		hi = irb.CreateSelect(irb.CreateICmpSGT(hi, hiC), hiC, hi);
		if (acc)
		{
			llvm::Value* a0 = irb.CreateSExt(irb.CreateExtractElement(dest, k), t4);
			llvm::Value* s = accSub ? irb.CreateSub(a0, hi) : irb.CreateAdd(a0, hi);
			s = irb.CreateSelect(irb.CreateICmpSLT(s, loC), loC, s);
			s = irb.CreateSelect(irb.CreateICmpSGT(s, hiC), hiC, s);
			hi = s;
		}
		res = irb.CreateInsertElement(res, irb.CreateTrunc(hi, dstLaneTy), k);
	}
	storeNeonVector(ai->operands[0].reg, res, irb);
	(void)srcTy;
}

/**
 * ARM_INS_VSELEQ / VSELGE / VSELGT / VSELVS
 *
 * Scalar VFP select from FPSCR NZCV packed by VCMP (bits 31..28). Does not
 * invent FPSCR fields.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVsel(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count != 3 || !isScalarVfp(ai) || ai->operands[0].type != ARM_OP_REG
		|| !isFpRegister(ai->operands[0].reg))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	auto* ty = vfpTypeOfReg(ai->operands[0].reg, irb);
	auto* sn = loadVfpOp(ai->operands[1], irb, ty);
	auto* sm = loadVfpOp(ai->operands[2], irb, ty);
	auto* packed = loadRegister(ARM_REG_FPSCR_NZCV, irb);
	auto bit = [&](unsigned n) {
		return irb.CreateTrunc(irb.CreateLShr(packed, llvm::ConstantInt::get(packed->getType(), n)), irb.getInt1Ty());
	};
	auto* n = bit(31);
	auto* z = bit(30);
	auto* v = bit(28);
	llvm::Value* cond = nullptr;
	switch (i->id)
	{
	case ARM_INS_VSELEQ: cond = z; break;
	case ARM_INS_VSELVS: cond = v; break;
	case ARM_INS_VSELGE: cond = irb.CreateICmpEQ(n, v); break;
	case ARM_INS_VSELGT: cond = irb.CreateAnd(irb.CreateNot(z), irb.CreateICmpEQ(n, v)); break;
	default: translatePseudoAsmGeneric(i, ai, irb); return;
	}
	storeOp(ai->operands[0], irb.CreateSelect(cond, sn, sm), irb, eOpConv::FPCAST_OR_BITCAST);
}

namespace {

uint32_t armT32Reg(unsigned n)
{
	if (n <= 12) return ARM_REG_R0 + n;
	return n == 13 ? ARM_REG_SP : (n == 14 ? ARM_REG_LR : ARM_REG_PC);
}

} // anonymous namespace

/**
 * ARM_INS_CSEL, ARM_INS_CSINC, ARM_INS_CSINV, ARM_INS_CSNEG (v8.1-M)
 *
 * Fields come from the T32 encoding because the CSET/CINC/CNEG/... aliases
 * drop operands in Capstone's detail. Register 15 reads as zero.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateCsel(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (i->size != 4)
	{
		throwUnexpectedOperands(i);
	}
	unsigned hw1 = i->bytes[0] | (unsigned(i->bytes[1]) << 8);
	unsigned hw2 = i->bytes[2] | (unsigned(i->bytes[3]) << 8);
	unsigned rn = hw1 & 0xf;
	unsigned op = (hw2 >> 12) & 0x3;
	unsigned rd = (hw2 >> 8) & 0xf;
	unsigned cond = (hw2 >> 4) & 0xf;
	unsigned rm = hw2 & 0xf;

	auto* i32 = irb.getInt32Ty();
	auto readZr = [&](unsigned r) -> llvm::Value* {
		return r == 15
				? static_cast<llvm::Value*>(llvm::ConstantInt::get(i32, 0))
				: loadRegister(armT32Reg(r), irb);
	};
	auto* a = readZr(rn);
	auto* b = readZr(rm);
	switch (op)
	{
		case 1: b = irb.CreateAdd(b, llvm::ConstantInt::get(i32, 1)); break;
		case 2: b = irb.CreateNot(b); break;
		case 3: b = irb.CreateNeg(b); break;
		default: break;
	}

	auto savedCc = ai->cc;
	ai->cc = static_cast<ARMCC_CondCodes>(cond);
	auto* c = generateInsnConditionCode(irb, ai);
	ai->cc = savedCc;

	storeRegister(armT32Reg(rd), irb.CreateSelect(c, a, b), irb);
}

/**
 * ARM_INS_LSLL, ARM_INS_LSRL, ARM_INS_ASRL (v8.1-M)
 *
 * Shift the 64-bit RdaHi:RdaLo pair. The register form takes a signed
 * byte from Rm, and a negative amount shifts the other way.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateLongShift(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);
	if (ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG)
	{
		throwUnexpectedOperands(i);
	}

	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* lo = irb.CreateZExt(loadRegister(ai->operands[0].reg, irb), i64);
	auto* hi = irb.CreateZExt(loadRegister(ai->operands[1].reg, irb), i64);
	llvm::Value* v = irb.CreateOr(irb.CreateShl(hi, 32), lo);

	bool arith = i->id == ARM_INS_ASRL;
	bool left = i->id == ARM_INS_LSLL;
	auto shiftBy = [&](bool toLeft, llvm::Value* amt) -> llvm::Value* {
		// Amounts of 64 and up give 0, or the sign fill for ASRL.
		auto* big = irb.CreateICmpUGT(amt, llvm::ConstantInt::get(i64, 63));
		auto* safe = irb.CreateSelect(big, llvm::ConstantInt::get(i64, 63), amt);
		if (toLeft)
		{
			return irb.CreateSelect(big, llvm::ConstantInt::get(i64, 0), irb.CreateShl(v, safe));
		}
		if (arith)
		{
			return irb.CreateAShr(v, safe);
		}
		return irb.CreateSelect(big, llvm::ConstantInt::get(i64, 0), irb.CreateLShr(v, safe));
	};

	llvm::Value* res = nullptr;
	cs_arm_op& sh = ai->operands[2];
	if (sh.type == ARM_OP_IMM)
	{
		res = shiftBy(left, llvm::ConstantInt::get(i64, static_cast<uint32_t>(sh.imm)));
	}
	else if (sh.type == ARM_OP_REG)
	{
		auto* rm = loadRegister(sh.reg, irb);
		auto* amt = irb.CreateSExt(irb.CreateTrunc(rm, irb.getInt8Ty()), i64);
		auto* neg = irb.CreateICmpSLT(amt, llvm::ConstantInt::get(i64, 0));
		auto* mag = irb.CreateSelect(neg, irb.CreateNeg(amt), amt);
		// LSLL with a negative amount is LSRL; ASRL with one is LSLL.
		bool savedArith = arith;
		auto* fwd = shiftBy(left, mag);
		arith = false;
		auto* back = shiftBy(!left, mag);
		arith = savedArith;
		res = irb.CreateSelect(neg, back, fwd);
	}
	else
	{
		throwUnexpectedOperands(i);
	}

	storeRegister(ai->operands[0].reg, irb.CreateTrunc(res, i32), irb);
	storeRegister(ai->operands[1].reg, irb.CreateTrunc(irb.CreateLShr(res, 32), i32), irb);
}

/**
 * ARM_INS_BXAUT Ra, Rn, Rm (v8.1-M PACBTI)
 *
 * Authenticate the PAC in Ra, then BX Rn. The authentication only faults
 * on a forged return address, so it is not modelled.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateBxaut(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	EXPECT_IS_TERNARY(i, ai, irb);

	auto* target = loadOp(ai->operands[1], irb);
	bool isReturn = ai->operands[1].type == ARM_OP_REG
			&& ai->operands[1].reg == ARM_REG_LR;
	if (armCcUncond(ai))
	{
		isReturn
			? generateReturnFunctionCall(irb, target)
			: generateBranchFunctionCall(irb, target);
	}
	else
	{
		auto* cond = generateInsnConditionCode(irb, ai);
		isReturn
			? generateCondReturnFunctionCall(irb, cond, target)
			: generateCondBranchFunctionCall(irb, cond, target);
	}
}

/**
 * ARM_INS_DLS, ARM_INS_WLS, ARM_INS_LE, ARM_INS_LCTP (v8.1-M low-overhead
 * loops). DLS/WLS load the iteration count into LR, WLS skips the loop when
 * it is zero, and LE counts LR down and branches back while it is above 1.
 * Capstone gives the branch targets as absolute addresses.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateLowOverheadLoop(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto* i32 = irb.getInt32Ty();
	switch (i->id)
	{
		case ARM_INS_LCTP:
			return;
		case ARM_INS_DLS:
		{
			EXPECT_IS_BINARY(i, ai, irb);
			storeRegister(ARM_REG_LR, loadOp(ai->operands[1], irb), irb);
			return;
		}
		case ARM_INS_WLS:
		{
			EXPECT_IS_TERNARY(i, ai, irb);
			auto* n = loadOp(ai->operands[1], irb);
			storeRegister(ARM_REG_LR, n, irb);
			auto* zero = irb.CreateICmpEQ(n, llvm::ConstantInt::get(n->getType(), 0));
			generateCondBranchFunctionCall(irb, zero, loadOp(ai->operands[2], irb));
			return;
		}
		case ARM_INS_LE:
		{
			if (ai->op_count == 1)
			{
				// LE without LR loops forever.
				generateBranchFunctionCall(irb, loadOp(ai->operands[0], irb));
				return;
			}
			EXPECT_IS_BINARY(i, ai, irb);
			auto* lr = loadRegister(ARM_REG_LR, irb);
			auto* again = irb.CreateICmpNE(lr, llvm::ConstantInt::get(i32, 1));
			auto* dec = irb.CreateSub(lr, llvm::ConstantInt::get(i32, 1));
			storeRegister(ARM_REG_LR, irb.CreateSelect(again, dec, lr), irb);
			generateCondBranchFunctionCall(irb, again, loadOp(ai->operands[1], irb));
			return;
		}
		case ARM_INS_DLSTP:
		case ARM_INS_WLSTP:
		{
			// Tail predication: LR counts elements and FPSCR.LTPSIZE
			// (bits 18:16) records the element size from hw1 bits 5:4.
			auto* n = loadOp(ai->operands[1], irb);
			storeRegister(ARM_REG_LR, n, irb);
			uint32_t size = (i->bytes[0] >> 4) & 3;
			auto* fpscr = loadRegister(ARM_REG_FPSCR, irb);
			fpscr = irb.CreateOr(
					irb.CreateAnd(fpscr, llvm::ConstantInt::get(i32, ~(7u << 16))),
					llvm::ConstantInt::get(i32, size << 16));
			storeRegister(ARM_REG_FPSCR, fpscr, irb);
			if (i->id == ARM_INS_WLSTP)
			{
				if (ai->op_count != 3)
				{
					throwUnexpectedOperands(i);
				}
				auto* zero = irb.CreateICmpEQ(n, llvm::ConstantInt::get(n->getType(), 0));
				generateCondBranchFunctionCall(irb, zero, loadOp(ai->operands[2], irb));
			}
			return;
		}
		case ARM_INS_LETP:
		{
			EXPECT_IS_BINARY(i, ai, irb);
			// Each iteration retires 128 / esize elements: 16 >> LTPSIZE.
			auto* fpscr = loadRegister(ARM_REG_FPSCR, irb);
			auto* ltp = irb.CreateAnd(irb.CreateLShr(fpscr, llvm::ConstantInt::get(i32, 16)),
					llvm::ConstantInt::get(i32, 7));
			auto* elems = irb.CreateLShr(llvm::ConstantInt::get(i32, 16), ltp);
			auto* lr = loadRegister(ARM_REG_LR, irb);
			auto* again = irb.CreateICmpUGT(lr, elems);
			storeRegister(ARM_REG_LR, irb.CreateSelect(again, irb.CreateSub(lr, elems), lr), irb);
			generateCondBranchFunctionCall(irb, again, loadOp(ai->operands[1], irb));
			return;
		}
		default:
			throw GenericError("Unhandled insn ID in translateLowOverheadLoop().");
	}
}

/**
 * MVE scalar shifts by immediate (v8.1-M): UQSHL/SQSHL saturate a left
 * shift, URSHR/SRSHR round a right shift. The ...LL/...RL forms work on the
 * RdaHi:RdaLo pair. Both round and saturate are built from plain shifts and
 * compares in i64. The register-amount forms stay pseudo calls.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMveScalarShift(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool wide = false;
	bool isSigned = false;
	bool left = false;
	switch (i->id)
	{
		case ARM_INS_UQSHL: left = true; break;
		case ARM_INS_SQSHL: left = true; isSigned = true; break;
		case ARM_INS_URSHR: break;
		case ARM_INS_SRSHR: isSigned = true; break;
		case ARM_INS_UQSHLL: wide = true; left = true; break;
		case ARM_INS_SQSHLL: wide = true; left = true; isSigned = true; break;
		case ARM_INS_URSHRL: wide = true; break;
		case ARM_INS_SRSHRL: wide = true; isSigned = true; break;
		default: throw GenericError("Unhandled insn ID in translateMveScalarShift().");
	}
	unsigned immIdx = wide ? 2 : 1;
	if (ai->op_count != immIdx + 1 || ai->operands[immIdx].type != ARM_OP_IMM)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	uint64_t n = static_cast<uint32_t>(ai->operands[immIdx].imm);

	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto c64 = [&](uint64_t v) { return llvm::ConstantInt::get(i64, v); };
	llvm::Value* x = nullptr;
	if (wide)
	{
		auto* lo = irb.CreateZExt(loadRegister(ai->operands[0].reg, irb), i64);
		auto* hi = irb.CreateZExt(loadRegister(ai->operands[1].reg, irb), i64);
		x = irb.CreateOr(irb.CreateShl(hi, 32), lo);
	}
	else
	{
		auto* r = loadRegister(ai->operands[0].reg, irb);
		x = isSigned ? irb.CreateSExt(r, i64) : irb.CreateZExt(r, i64);
	}

	llvm::Value* res = nullptr;
	if (!left)
	{
		auto shr = [&](uint64_t k) {
			return isSigned ? irb.CreateAShr(x, c64(k)) : irb.CreateLShr(x, c64(k));
		};
		res = irb.CreateAdd(shr(n), irb.CreateAnd(shr(n - 1), c64(1)));
	}
	else if (!wide)
	{
		auto* r = irb.CreateShl(x, c64(n));
		if (isSigned)
		{
			auto* mx = c64(0x7fffffff);
			auto* mn = c64(static_cast<uint64_t>(INT64_C(-0x80000000)));
			r = irb.CreateSelect(irb.CreateICmpSGT(r, mx), mx, r);
			res = irb.CreateSelect(irb.CreateICmpSLT(r, mn), mn, r);
		}
		else
		{
			auto* mx = c64(0xffffffff);
			res = irb.CreateSelect(irb.CreateICmpUGT(r, mx), mx, r);
		}
	}
	else
	{
		auto* r = irb.CreateShl(x, c64(n));
		auto* back = isSigned ? irb.CreateAShr(r, c64(n)) : irb.CreateLShr(r, c64(n));
		auto* ovf = irb.CreateICmpNE(back, x);
		llvm::Value* sat = c64(~UINT64_C(0));
		if (isSigned)
		{
			sat = irb.CreateSelect(irb.CreateICmpSLT(x, c64(0)),
					c64(UINT64_C(0x8000000000000000)), c64(UINT64_C(0x7fffffffffffffff)));
		}
		res = irb.CreateSelect(ovf, sat, r);
	}

	storeRegister(ai->operands[0].reg, irb.CreateTrunc(res, i32), irb);
	if (wide)
	{
		storeRegister(ai->operands[1].reg, irb.CreateTrunc(irb.CreateLShr(res, 32), i32), irb);
	}
}

/**
 * ARM_INS_CLRM (v8.1-M): zero every listed register. APSR in the list
 * clears the condition flags.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateClrm(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count == 0)
	{
		throwUnexpectedOperands(i);
	}
	for (unsigned k = 0; k < ai->op_count; ++k)
	{
		cs_arm_op& op = ai->operands[k];
		if (op.type != ARM_OP_REG)
		{
			throwUnexpectedOperands(i);
		}
		if (op.reg == ARM_REG_APSR)
		{
			for (auto f : {ARM_REG_CPSR_N, ARM_REG_CPSR_Z, ARM_REG_CPSR_C, ARM_REG_CPSR_V})
			{
				storeRegister(f, irb.getFalse(), irb);
			}
			continue;
		}
		storeRegister(op.reg, irb.getInt32(0), irb);
	}
}

/**
 * MVE across-vector reductions into general registers: VADDV[A], VADDLV[A],
 * VMAXV/VMINV and the absolute VMAXAV/VMINAV. Predication is ignored, as for
 * the other MVE vector ops (VPT blocks are not modelled).
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMveReduce(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool isFp = false, isSigned = false;
	unsigned lb = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	bool longForm = i->id == ARM_INS_VADDLV || i->id == ARM_INS_VADDLVA;
	unsigned nOps = longForm ? 3 : 2;
	if (isFp || lb == 0 || ai->op_count != nOps || ai->operands[nOps - 1].type != ARM_OP_REG)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	auto* v = loadNeonVector(ai->operands[nOps - 1].reg, lb, false, irb);
	unsigned lanes = llvm::cast<llvm::FixedVectorType>(v->getType())->getNumElements();
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* eTy = irb.getIntNTy(lb);
	auto ext = [&](llvm::Value* x, llvm::Type* t) {
		return isSigned ? irb.CreateSExtOrTrunc(x, t) : irb.CreateZExtOrTrunc(x, t);
	};
	if (longForm)
	{
		llvm::Value* sum = llvm::ConstantInt::get(i64, 0);
		if (i->id == ARM_INS_VADDLVA)
		{
			auto* lo = irb.CreateZExt(loadRegister(ai->operands[0].reg, irb), i64);
			auto* hi = irb.CreateZExt(loadRegister(ai->operands[1].reg, irb), i64);
			sum = irb.CreateOr(lo, irb.CreateShl(hi, 32));
		}
		for (unsigned k = 0; k < lanes; ++k)
		{
			sum = irb.CreateAdd(sum, ext(irb.CreateExtractElement(v, uint64_t(k)), i64));
		}
		storeRegister(ai->operands[0].reg, irb.CreateTrunc(sum, i32), irb);
		storeRegister(ai->operands[1].reg, irb.CreateTrunc(irb.CreateLShr(sum, 32), i32), irb);
		return;
	}
	auto* rd = loadRegister(ai->operands[0].reg, irb);
	llvm::Value* res = nullptr;
	switch (i->id)
	{
		case ARM_INS_VADDV:
		case ARM_INS_VADDVA:
		{
			res = i->id == ARM_INS_VADDVA ? rd : llvm::ConstantInt::get(i32, 0);
			for (unsigned k = 0; k < lanes; ++k)
			{
				res = irb.CreateAdd(res, ext(irb.CreateExtractElement(v, uint64_t(k)), i32));
			}
			break;
		}
		case ARM_INS_VMAXV:
		case ARM_INS_VMINV:
		case ARM_INS_VMAXAV:
		case ARM_INS_VMINAV:
		{
			bool isMax = i->id == ARM_INS_VMAXV || i->id == ARM_INS_VMAXAV;
			bool abs = i->id == ARM_INS_VMAXAV || i->id == ARM_INS_VMINAV;
			bool sgn = isSigned && !abs;
			llvm::Value* acc = irb.CreateTrunc(rd, eTy);
			for (unsigned k = 0; k < lanes; ++k)
			{
				llvm::Value* e = irb.CreateExtractElement(v, uint64_t(k));
				if (abs)
				{
					e = irb.CreateSelect(irb.CreateICmpSLT(e, llvm::ConstantInt::get(eTy, 0)), irb.CreateNeg(e), e);
				}
				llvm::Value* c = isMax
						? (sgn ? irb.CreateICmpSGT(e, acc) : irb.CreateICmpUGT(e, acc))
						: (sgn ? irb.CreateICmpSLT(e, acc) : irb.CreateICmpULT(e, acc));
				acc = irb.CreateSelect(c, e, acc);
			}
			res = sgn ? irb.CreateSExt(acc, i32) : irb.CreateZExt(acc, i32);
			break;
		}
		default:
			translatePseudoAsmGeneric(i, ai, irb);
			return;
	}
	storeRegister(ai->operands[0].reg, res, irb);
}


/**
 * MVE lane-shaping and multiply ops: VMOVL[BT], VMOVN[BT], VQMOV[U]N[BT],
 * VSHLL[BT], VSHRN[BT], VRSHRN[BT], VMULL[BT] (integer), VMULH, VRMULH,
 * VMAXA/VMINA and the multiply-accumulate reductions VMLADAV[A][X]/VMLSDAV[A][X].
 * Predication (VPT blocks) is not modelled.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMveVec(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	bool isFp = false, isSigned = false;
	unsigned lb = vectorDataLaneBits(ai->vector_data, isFp, isSigned);
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ai, irb); };
	if (lb == 0)
	{
		// Aliases (e.g. vmlav) may leave vector_data empty; parse the ".s32" suffix.
		const char* dt = std::strchr(i->mnemonic, '.');
		if (dt && (dt[1] == 's' || dt[1] == 'u' || dt[1] == 'i'))
		{
			lb = unsigned(std::atoi(dt + 2));
			isSigned = dt[1] == 's';
		}
	}
	std::string mn0 = i->mnemonic;
	bool fpOk = mn0.find("nm") != std::string::npos || mn0.compare(0, 5, "vcmul") == 0
			|| mn0.compare(0, 5, "vfmas") == 0;
	if ((isFp && !fpOk) || lb == 0 || lb > 32 || ai->op_count < 2)
	{
		fallback();
		return;
	}
	for (unsigned k = 0; k < ai->op_count; ++k)
	{
		if (ai->operands[k].type != ARM_OP_REG && ai->operands[k].type != ARM_OP_IMM)
		{
			fallback();
			return;
		}
	}
	std::string m = i->mnemonic;
	auto dot = m.find('.');
	std::string op = m.substr(0, dot);
	if (!op.empty() && (op.back() == 't' || op.back() == 'e') && op.size() > 5 && op.compare(0, 2, "vp") != 0)
	{
		// Strip a VPT-block predicate suffix (e.g. vmovlbt / vmovlbe).
		std::string base = op.substr(0, op.size() - 1);
		static const char* known[] = {"vmovlb", "vmovlt", "vmovnb", "vmovnt", "vmullb", "vmullt", "vshllb", "vshllt",
				"vshrnb", "vshrnt", "vrshrnb", "vrshrnt", "vqmovnb", "vqmovnt", "vqmovunb", "vqmovunt", "vmulh",
				"vrmulh", "vmaxa", "vmina", "vmladav", "vmladava", "vmladavx", "vmladavax", "vmlsdav", "vmlsdava",
				"vmlsdavx", "vmlsdavax", "vmlav", "vmlava", "vqshrnb", "vqshrnt", "vqrshrnb", "vqrshrnt", "vqshrunb",
				"vqshrunt", "vqrshrunb", "vqrshrunt", "vqdmullb", "vqdmullt", "vmlaldavx", "vmlaldavax", "vmlsldavx",
				"vmlsldavax", "vrmlaldavhx", "vrmlaldavhax", "vrmlsldavhx", "vrmlsldavhax", "vqdmladh", "vqdmladhx",
				"vqrdmladh", "vqrdmladhx", "vqdmlsdh", "vqdmlsdhx", "vqrdmlsdh", "vqrdmlsdhx"};
		bool isKnown = false, baseKnown = false;
		for (auto* k : known) { isKnown |= op == k; baseKnown |= base == k; }
		if (!isKnown && baseKnown) op = base;
	}
	auto* i32 = irb.getInt32Ty();
	auto ext = [&](llvm::Value* x, llvm::Type* t, bool s) {
		return s ? irb.CreateSExtOrTrunc(x, t) : irb.CreateZExtOrTrunc(x, t);
	};
	auto lanesOf = [&](llvm::Value* v) {
		return unsigned(llvm::cast<llvm::FixedVectorType>(v->getType())->getNumElements());
	};
	auto reg = [&](unsigned k) { return ai->operands[k].reg; };

	// Widening: source lanes lb, result lanes 2*lb taken from even (B) or odd (T) lanes.
	if (op == "vmovlb" || op == "vmovlt" || op == "vshllb" || op == "vshllt" || op == "vmullb" || op == "vmullt")
	{
		bool top = op.back() == 't';
		bool mul = op.compare(0, 5, "vmull") == 0;
		bool shl = op.compare(0, 5, "vshll") == 0;
		if (mul && ai->op_count != 3) { fallback(); return; }
		if (shl && (ai->op_count != 3 || ai->operands[2].type != ARM_OP_IMM)) { fallback(); return; }
		if (lb > 16) { fallback(); return; }
		auto* a = loadNeonVector(reg(1), lb, false, irb);
		llvm::Value* b = mul ? loadNeonVector(reg(2), lb, false, irb) : nullptr;
		unsigned n = lanesOf(a) / 2;
		auto* wt = irb.getIntNTy(2 * lb);
		llvm::Value* out = llvm::UndefValue::get(llvm::FixedVectorType::get(wt, n));
		for (unsigned k = 0; k < n; ++k)
		{
			unsigned src = 2 * k + (top ? 1 : 0);
			llvm::Value* e = ext(irb.CreateExtractElement(a, uint64_t(src)), wt, isSigned);
			if (mul) e = irb.CreateMul(e, ext(irb.CreateExtractElement(b, uint64_t(src)), wt, isSigned));
			if (shl) e = irb.CreateShl(e, uint64_t(ai->operands[2].imm));
			out = irb.CreateInsertElement(out, e, uint64_t(k));
		}
		storeNeonVector(reg(0), out, irb);
		return;
	}

	// Narrowing into the even (B) or odd (T) half-lanes of the destination.
	if (op == "vmovnb" || op == "vmovnt" || op == "vqmovnb" || op == "vqmovnt" || op == "vqmovunb"
			|| op == "vqmovunt" || op == "vshrnb" || op == "vshrnt" || op == "vrshrnb" || op == "vrshrnt"
			|| op == "vqshrnb" || op == "vqshrnt" || op == "vqrshrnb" || op == "vqrshrnt" || op == "vqshrunb"
			|| op == "vqshrunt" || op == "vqrshrunb" || op == "vqrshrunt")
	{
		bool top = op.back() == 't';
		bool sat = op.compare(0, 2, "vq") == 0;
		bool usat = op.compare(0, 6, "vqmovu") == 0 || op.find("shrun") != std::string::npos;
		bool shr = op.find("shrn") != std::string::npos || op.find("shrun") != std::string::npos;
		bool rnd = op.compare(0, 3, "vrs") == 0 || op.compare(0, 4, "vqrs") == 0;
		if (lb < 16 || (shr && (ai->op_count != 3 || ai->operands[2].type != ARM_OP_IMM)))
		{
			fallback();
			return;
		}
		auto* a = loadNeonVector(reg(1), lb, false, irb);
		auto* d = loadNeonVector(reg(0), lb / 2, false, irb);
		unsigned n = lanesOf(a);
		auto* nt = irb.getIntNTy(lb / 2);
		auto* wt = irb.getIntNTy(2 * lb);
		bool srcSignedAll = isSigned || usat;
		for (unsigned k = 0; k < n; ++k)
		{
			llvm::Value* e = ext(irb.CreateExtractElement(a, uint64_t(k)), wt, srcSignedAll);
			if (shr)
			{
				uint64_t s = uint64_t(ai->operands[2].imm);
				if (rnd && s) e = irb.CreateAdd(e, llvm::ConstantInt::get(wt, 1ull << (s - 1)));
				e = srcSignedAll ? irb.CreateAShr(e, s) : irb.CreateLShr(e, s);
			}
			if (sat)
			{
				unsigned h = lb / 2;
				bool srcSigned = isSigned || usat;
				llvm::Value* lo;
				llvm::Value* hi;
				if (usat || !isSigned)
				{
					lo = llvm::ConstantInt::get(wt, 0);
					hi = llvm::ConstantInt::get(wt, (1ull << h) - 1);
				}
				else
				{
					lo = llvm::ConstantInt::get(wt, llvm::APInt::getSignedMinValue(h).sext(2 * lb));
					hi = llvm::ConstantInt::get(wt, (1ull << (h - 1)) - 1);
				}
				auto* gt = srcSigned ? irb.CreateICmpSGT(e, hi) : irb.CreateICmpUGT(e, hi);
				auto* lt = srcSigned ? irb.CreateICmpSLT(e, lo) : irb.CreateICmpULT(e, lo);
				e = irb.CreateSelect(gt, hi, irb.CreateSelect(lt, lo, e));
			}
			d = irb.CreateInsertElement(d, irb.CreateTrunc(e, nt), uint64_t(2 * k + (top ? 1 : 0)));
		}
		storeNeonVector(reg(0), d, irb);
		return;
	}

	if (op == "vmulh" || op == "vrmulh")
	{
		if (ai->op_count != 3) { fallback(); return; }
		auto* a = loadNeonVector(reg(1), lb, false, irb);
		auto* b = loadNeonVector(reg(2), lb, false, irb);
		auto* wt = irb.getIntNTy(2 * lb);
		llvm::Value* out = a;
		for (unsigned k = 0; k < lanesOf(a); ++k)
		{
			auto* p = irb.CreateMul(ext(irb.CreateExtractElement(a, uint64_t(k)), wt, isSigned),
					ext(irb.CreateExtractElement(b, uint64_t(k)), wt, isSigned));
			llvm::Value* q = p;
			if (op == "vrmulh") q = irb.CreateAdd(q, llvm::ConstantInt::get(wt, 1ull << (lb - 1)));
			q = irb.CreateTrunc(irb.CreateLShr(q, lb), irb.getIntNTy(lb));
			out = irb.CreateInsertElement(out, q, uint64_t(k));
		}
		storeNeonVector(reg(0), out, irb);
		return;
	}

	if (op == "vmaxa" || op == "vmina")
	{
		if (ai->op_count != 2) { fallback(); return; }
		auto* d = loadNeonVector(reg(0), lb, false, irb);
		auto* a = loadNeonVector(reg(1), lb, false, irb);
		auto* et = irb.getIntNTy(lb);
		for (unsigned k = 0; k < lanesOf(a); ++k)
		{
			llvm::Value* x = irb.CreateExtractElement(d, uint64_t(k));
			llvm::Value* y = irb.CreateExtractElement(a, uint64_t(k));
			y = irb.CreateSelect(irb.CreateICmpSLT(y, llvm::ConstantInt::get(et, 0)), irb.CreateNeg(y), y);
			auto* c = op == "vmaxa" ? irb.CreateICmpUGT(y, x) : irb.CreateICmpULT(y, x);
			d = irb.CreateInsertElement(d, irb.CreateSelect(c, y, x), uint64_t(k));
		}
		storeNeonVector(reg(0), d, irb);
		return;
	}

	if (op.rfind("vmladav", 0) == 0 || op.rfind("vmlsdav", 0) == 0 || op == "vmlav" || op == "vmlava")
	{
		bool sub = op.compare(0, 4, "vmls") == 0;
		bool acc = op.find("ava") != std::string::npos || op == "vmlava";
		bool xch = op.back() == 'x';
		if (ai->op_count != 3) { fallback(); return; }
		auto* a = loadNeonVector(reg(1), lb, false, irb);
		auto* b = loadNeonVector(reg(2), lb, false, irb);
		llvm::Value* sum = acc ? loadRegister(reg(0), irb) : llvm::ConstantInt::get(i32, 0);
		unsigned n = lanesOf(a);
		for (unsigned k = 0; k < n; ++k)
		{
			unsigned kb = xch ? (k ^ 1) : k;
			auto* p = irb.CreateMul(ext(irb.CreateExtractElement(a, uint64_t(k)), i32, isSigned),
					ext(irb.CreateExtractElement(b, uint64_t(kb)), i32, isSigned));
			sum = (sub && (k & 1)) ? irb.CreateSub(sum, p) : irb.CreateAdd(sum, p);
		}
		storeRegister(reg(0), sum, irb);
		return;
	}

	// 64-bit accumulating reductions into RdaLo:RdaHi.
	bool lal = op.rfind("vmlaldav", 0) == 0 || op.rfind("vmlsldav", 0) == 0 || op == "vmlalv" || op == "vmlalva";
	bool rml = op.rfind("vrmlaldavh", 0) == 0 || op.rfind("vrmlsldavh", 0) == 0 || op == "vrmlalvh"
			|| op == "vrmlalvha";
	if (lal || rml)
	{
		if (ai->op_count != 4) { fallback(); return; }
		bool sub = op.rfind("vmlsl", 0) == 0 || op.rfind("vrmlsl", 0) == 0;
		bool acc = op.find("ava") != std::string::npos || op.back() == 'a';
		bool xch = op.back() == 'x';
		auto* i64 = irb.getInt64Ty();
		auto* i72 = irb.getIntNTy(80);
		auto* wt = rml ? i72 : i64;
		auto* a = loadNeonVector(reg(2), lb, false, irb);
		auto* b = loadNeonVector(reg(3), lb, false, irb);
		llvm::Value* sum = llvm::ConstantInt::get(wt, 0);
		if (acc)
		{
			auto* lo = irb.CreateZExt(irb.CreateZExtOrTrunc(loadRegister(reg(0), irb), i32), i64);
			auto* hi = irb.CreateZExt(irb.CreateZExtOrTrunc(loadRegister(reg(1), irb), i32), i64);
			llvm::Value* acc64 = irb.CreateOr(lo, irb.CreateShl(hi, 32));
			sum = rml ? irb.CreateShl(irb.CreateSExt(acc64, wt), 8) : acc64;
		}
		unsigned n = lanesOf(a);
		for (unsigned k = 0; k < n; ++k)
		{
			unsigned kb = xch ? (k ^ 1) : k;
			auto* p = irb.CreateMul(ext(irb.CreateExtractElement(a, uint64_t(k)), wt, isSigned),
					ext(irb.CreateExtractElement(b, uint64_t(kb)), wt, isSigned));
			sum = (sub && (k & 1)) ? irb.CreateSub(sum, p) : irb.CreateAdd(sum, p);
		}
		if (rml)
		{
			sum = irb.CreateTrunc(irb.CreateAShr(irb.CreateAdd(sum, llvm::ConstantInt::get(wt, 0x80)), 8), i64);
		}
		storeRegister(reg(0), irb.CreateTrunc(sum, i32), irb);
		storeRegister(reg(1), irb.CreateTrunc(irb.CreateLShr(sum, 32), i32), irb);
		return;
	}

	if (op == "vabav")
	{
		if (ai->op_count != 3) { fallback(); return; }
		auto* a = loadNeonVector(reg(1), lb, false, irb);
		auto* b = loadNeonVector(reg(2), lb, false, irb);
		llvm::Value* sum = irb.CreateZExtOrTrunc(loadRegister(reg(0), irb), i32);
		for (unsigned k = 0; k < lanesOf(a); ++k)
		{
			auto* x = ext(irb.CreateExtractElement(a, uint64_t(k)), i32, isSigned);
			auto* y = ext(irb.CreateExtractElement(b, uint64_t(k)), i32, isSigned);
			auto* gt = isSigned ? irb.CreateICmpSGT(x, y) : irb.CreateICmpUGT(x, y);
			sum = irb.CreateAdd(sum, irb.CreateSelect(gt, irb.CreateSub(x, y), irb.CreateSub(y, x)));
		}
		storeRegister(reg(0), sum, irb);
		return;
	}

	if (op == "vqdmullb" || op == "vqdmullt")
	{
		if (ai->op_count != 3 || lb > 32 || lb < 16) { fallback(); return; }
		bool top = op.back() == 't';
		auto* a = loadNeonVector(reg(1), lb, false, irb);
		bool scalarB = ai->operands[2].type == ARM_OP_REG && !isNeonRegister(reg(2));
		llvm::Value* b = scalarB ? nullptr : loadNeonVector(reg(2), lb, false, irb);
		llvm::Value* bs = scalarB ? irb.CreateTrunc(irb.CreateZExtOrTrunc(loadRegister(reg(2), irb), i32),
				irb.getIntNTy(lb)) : nullptr;
		auto* wt = irb.getIntNTy(2 * lb);
		auto* vt = irb.getIntNTy(4 * lb);
		unsigned n = lanesOf(a) / 2;
		llvm::Value* d = llvm::UndefValue::get(llvm::FixedVectorType::get(wt, n));
		auto* mx = llvm::ConstantInt::get(vt, llvm::APInt::getSignedMaxValue(2 * lb).sext(4 * lb));
		for (unsigned k = 0; k < n; ++k)
		{
			unsigned s = 2 * k + (top ? 1 : 0);
			auto* x = irb.CreateSExt(irb.CreateExtractElement(a, uint64_t(s)), vt);
			auto* y = irb.CreateSExt(scalarB ? bs : irb.CreateExtractElement(b, uint64_t(s)), vt);
			llvm::Value* p = irb.CreateShl(irb.CreateMul(x, y), 1);
			p = irb.CreateSelect(irb.CreateICmpSGT(p, mx), mx, p);
			d = irb.CreateInsertElement(d, irb.CreateTrunc(p, wt), uint64_t(k));
		}
		storeNeonVector(reg(0), d, irb);
		return;
	}

	if ((op == "vmaxnmv" || op == "vminnmv" || op == "vmaxnmav" || op == "vminnmav") && isFp && lb == 32)
	{
		if (ai->op_count != 2) { fallback(); return; }
		bool mx = op.compare(0, 5, "vmaxn") == 0;
		bool ab = op.find("av") != std::string::npos;
		auto* ft = irb.getFloatTy();
		auto* a = loadNeonVector(reg(1), 32, true, irb);
		auto fabs = [&](llvm::Value* v) {
			return irb.CreateSelect(irb.CreateFCmpOLT(v, llvm::ConstantFP::get(ft, 0.0)), irb.CreateFNeg(v), v);
		};
		llvm::Value* r = irb.CreateBitCast(irb.CreateZExtOrTrunc(loadRegister(reg(0), irb), i32), ft);
		if (ab) r = fabs(r);
		for (unsigned k = 0; k < lanesOf(a); ++k)
		{
			llvm::Value* e = irb.CreateExtractElement(a, uint64_t(k));
			if (ab) e = fabs(e);
			// maxNum/minNum: a NaN operand yields the other operand.
			auto* better = mx ? irb.CreateFCmpOGT(e, r) : irb.CreateFCmpOLT(e, r);
			auto* rNan = irb.CreateFCmpUNO(r, r);
			r = irb.CreateSelect(irb.CreateOr(better, rNan), e, r);
		}
		storeRegister(reg(0), irb.CreateBitCast(r, i32), irb);
		return;
	}
	if (op == "vmaxnma" || op == "vminnma")
	{
		if (!isFp || lb != 32 || ai->op_count != 2) { fallback(); return; }
		auto* ft = irb.getFloatTy();
		auto* d = loadNeonVector(reg(0), 32, true, irb);
		auto* a = loadNeonVector(reg(1), 32, true, irb);
		auto fabs = [&](llvm::Value* v) {
			return irb.CreateSelect(irb.CreateFCmpOLT(v, llvm::ConstantFP::get(ft, 0.0)), irb.CreateFNeg(v), v);
		};
		llvm::Value* r = d;
		for (unsigned k = 0; k < 4; ++k)
		{
			auto* x = fabs(irb.CreateExtractElement(d, uint64_t(k)));
			auto* y = fabs(irb.CreateExtractElement(a, uint64_t(k)));
			auto* better = op == "vmaxnma" ? irb.CreateFCmpOGT(y, x) : irb.CreateFCmpOLT(y, x);
			r = irb.CreateInsertElement(r, irb.CreateSelect(irb.CreateOr(better, irb.CreateFCmpUNO(x, x)), y, x),
					uint64_t(k));
		}
		storeNeonVector(reg(0), r, irb);
		return;
	}

	if (op == "vcmul")
	{
		if (!isFp || lb != 32 || ai->op_count != 4 || ai->operands[3].type != ARM_OP_IMM) { fallback(); return; }
		auto* a = loadNeonVector(reg(1), 32, true, irb);
		auto* b = loadNeonVector(reg(2), 32, true, irb);
		llvm::Value* r = a;
		int64_t rot = ai->operands[3].imm;
		for (unsigned k = 0; k < 4; k += 2)
		{
			auto* ae = irb.CreateExtractElement(a, uint64_t(k));
			auto* ao = irb.CreateExtractElement(a, uint64_t(k + 1));
			auto* be = irb.CreateExtractElement(b, uint64_t(k));
			auto* bo = irb.CreateExtractElement(b, uint64_t(k + 1));
			llvm::Value *re, *im;
			switch (rot)
			{
				case 0: re = irb.CreateFMul(ae, be); im = irb.CreateFMul(ae, bo); break;
				case 90: re = irb.CreateFNeg(irb.CreateFMul(ao, bo)); im = irb.CreateFMul(ao, be); break;
				case 180: re = irb.CreateFNeg(irb.CreateFMul(ae, be)); im = irb.CreateFNeg(irb.CreateFMul(ae, bo)); break;
				case 270: re = irb.CreateFMul(ao, bo); im = irb.CreateFNeg(irb.CreateFMul(ao, be)); break;
				default: fallback(); return;
			}
			r = irb.CreateInsertElement(r, re, uint64_t(k));
			r = irb.CreateInsertElement(r, im, uint64_t(k + 1));
		}
		storeNeonVector(reg(0), r, irb);
		return;
	}

	if (op == "vfmas" || op == "vmlas")
	{
		bool f = op == "vfmas";
		if (ai->op_count != 3 || f != isFp || (f && lb != 32)) { fallback(); return; }
		auto* d = loadNeonVector(reg(0), lb, f, irb);
		auto* a = loadNeonVector(reg(1), lb, f, irb);
		llvm::Value* sc = irb.CreateZExtOrTrunc(loadRegister(reg(2), irb), i32);
		sc = f ? irb.CreateBitCast(sc, irb.getFloatTy()) : irb.CreateTrunc(sc, irb.getIntNTy(lb));
		llvm::Value* r = d;
		for (unsigned k = 0; k < lanesOf(d); ++k)
		{
			auto* x = irb.CreateExtractElement(d, uint64_t(k));
			auto* y = irb.CreateExtractElement(a, uint64_t(k));
			llvm::Value* e = f ? irb.CreateFAdd(irb.CreateFMul(x, y), sc) : irb.CreateAdd(irb.CreateMul(x, y), sc);
			r = irb.CreateInsertElement(r, e, uint64_t(k));
		}
		storeNeonVector(reg(0), r, irb);
		return;
	}

	if (op == "vqrdmlah" || op == "vqdmlah" || op == "vqrdmlash" || op == "vqdmlash")
	{
		if (ai->op_count != 3 || lb > 32 || isNeonRegister(reg(2))) { fallback(); return; }
		bool rnd = op.compare(0, 4, "vqrd") == 0;
		bool sh = op.back() == 'h' && op.find("ash") != std::string::npos;
		auto* et = irb.getIntNTy(lb);
		auto* wt = irb.getIntNTy(2 * lb + 2);
		auto* d = loadNeonVector(reg(0), lb, false, irb);
		auto* a = loadNeonVector(reg(1), lb, false, irb);
		auto* sc = irb.CreateSExt(irb.CreateTrunc(irb.CreateZExtOrTrunc(loadRegister(reg(2), irb), i32), et), wt);
		auto* mx = llvm::ConstantInt::get(wt, llvm::APInt::getSignedMaxValue(lb).sext(2 * lb + 2));
		auto* mn = llvm::ConstantInt::get(wt, llvm::APInt::getSignedMinValue(lb).sext(2 * lb + 2));
		llvm::Value* r = d;
		for (unsigned k = 0; k < lanesOf(d); ++k)
		{
			auto* x = irb.CreateSExt(irb.CreateExtractElement(d, uint64_t(k)), wt);
			auto* y = irb.CreateSExt(irb.CreateExtractElement(a, uint64_t(k)), wt);
			// vq[r]dmlah: d = (d << esize) + 2*n*s; vq[r]dmlash: d = (n << esize) + 2*d*s.
			llvm::Value* e = sh ? irb.CreateAdd(irb.CreateShl(y, lb), irb.CreateShl(irb.CreateMul(x, sc), 1))
					: irb.CreateAdd(irb.CreateShl(x, lb), irb.CreateShl(irb.CreateMul(y, sc), 1));
			if (rnd) e = irb.CreateAdd(e, llvm::ConstantInt::get(wt, 1ull << (lb - 1)));
			e = irb.CreateAShr(e, lb);
			e = irb.CreateSelect(irb.CreateICmpSGT(e, mx), mx, irb.CreateSelect(irb.CreateICmpSLT(e, mn), mn, e));
			r = irb.CreateInsertElement(r, irb.CreateTrunc(e, et), uint64_t(k));
		}
		storeNeonVector(reg(0), r, irb);
		return;
	}

	if (op.rfind("vqdmladh", 0) == 0 || op.rfind("vqrdmladh", 0) == 0 || op.rfind("vqdmlsdh", 0) == 0
			|| op.rfind("vqrdmlsdh", 0) == 0)
	{
		// Dual multiply: pairs of lanes give n[e]*m[e] +/- n[e+1]*m[e+1] into the
		// even lane, or (X) n[e+1]*m[e] +/- n[e]*m[e+1] into the odd lane. The sum is
		// doubled, optionally rounded, saturated and its high half kept. The other
		// lane of each pair is unchanged. The Q flag is not modelled.
		if (ai->op_count != 3 || !isSigned || !isNeonRegister(reg(2))) { fallback(); return; }
		bool rnd = op.compare(0, 4, "vqrd") == 0;
		bool sub = op.find("sdh") != std::string::npos;
		bool xch = op.back() == 'x';
		auto* et = irb.getIntNTy(lb);
		auto* wt = irb.getIntNTy(2 * lb + 4);
		auto* d = loadNeonVector(reg(0), lb, false, irb);
		auto* n = loadNeonVector(reg(1), lb, false, irb);
		auto* mv = loadNeonVector(reg(2), lb, false, irb);
		auto* mx = llvm::ConstantInt::get(wt, llvm::APInt::getSignedMaxValue(lb).sext(2 * lb + 4));
		auto* mn = llvm::ConstantInt::get(wt, llvm::APInt::getSignedMinValue(lb).sext(2 * lb + 4));
		auto el = [&](llvm::Value* v, unsigned k) { return irb.CreateSExt(irb.CreateExtractElement(v, uint64_t(k)), wt); };
		llvm::Value* r = d;
		for (unsigned e = 0; e + 1 < lanesOf(d); e += 2)
		{
			unsigned dst = xch ? e + 1 : e;
			auto* p1 = irb.CreateMul(el(n, dst), el(mv, e));
			auto* p2 = irb.CreateMul(el(n, xch ? e : e + 1), el(mv, e + 1));
			llvm::Value* s = sub ? irb.CreateSub(p1, p2) : irb.CreateAdd(p1, p2);
			s = irb.CreateShl(s, 1);
			if (rnd) s = irb.CreateAdd(s, llvm::ConstantInt::get(wt, 1ull << (lb - 1)));
			s = irb.CreateAShr(s, lb);
			s = irb.CreateSelect(irb.CreateICmpSGT(s, mx), mx, irb.CreateSelect(irb.CreateICmpSLT(s, mn), mn, s));
			r = irb.CreateInsertElement(r, irb.CreateTrunc(s, et), uint64_t(dst));
		}
		storeNeonVector(reg(0), r, irb);
		return;
	}
	fallback();
}



/**
 * MVE contiguous loads/stores VLDR{B,H,W,D}/VSTR{B,H,W,D} (immediate offset,
 * pre/post-indexed; gathers/scatters fall back), VIDUP/VDDUP, and the
 * complex ops VCADD/VHCADD (integer and fp) and VCMLA (fp, non-fused).
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMveMisc(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ai, irb); };
	std::string m = i->mnemonic;
	auto dot = m.find('.');
	std::string op = m.substr(0, dot);
	std::string dt = dot == std::string::npos ? "" : m.substr(dot + 1);
	unsigned lb = 0;
	bool sgn = false, fp = false;
	if (!dt.empty())
	{
		size_t p = 0;
		if (dt[0] == 's' || dt[0] == 'u' || dt[0] == 'i' || dt[0] == 'f')
		{
			sgn = dt[0] == 's';
			fp = dt[0] == 'f';
			p = 1;
		}
		lb = unsigned(std::atoi(dt.c_str() + p));
	}
	if (lb != 8 && lb != 16 && lb != 32 && lb != 64)
	{
		fallback();
		return;
	}
	auto* ty = getDefaultType();
	unsigned n = 128 / lb;

	if ((op == "vadc" || op == "vadci" || op == "vsbc" || op == "vsbci") && lb == 32 && ai->op_count == 3)
	{
		// Whole-vector add/subtract with carry through FPSCR.C (bit 29).
		bool sub = op[1] == 's';
		bool init = op.size() == 5;
		auto* fpscr = loadRegister(ARM_REG_FPSCR_NZCV, irb);
		llvm::Value* c = init ? llvm::ConstantInt::get(irb.getInt64Ty(), sub ? 1 : 0)
				: irb.CreateZExt(irb.CreateAnd(irb.CreateLShr(fpscr, 29), irb.getInt32(1)), irb.getInt64Ty());
		auto* a = loadNeonVector(ai->operands[1].reg, 32, false, irb);
		auto* b = loadNeonVector(ai->operands[2].reg, 32, false, irb);
		llvm::Value* r = llvm::UndefValue::get(llvm::FixedVectorType::get(irb.getInt32Ty(), 4));
		for (unsigned k = 0; k < 4; ++k)
		{
			auto* x = irb.CreateZExt(irb.CreateExtractElement(a, k), irb.getInt64Ty());
			llvm::Value* y = irb.CreateExtractElement(b, k);
			if (sub) y = irb.CreateNot(y);
			auto* sum = irb.CreateAdd(irb.CreateAdd(x, irb.CreateZExt(y, irb.getInt64Ty())), c);
			r = irb.CreateInsertElement(r, irb.CreateTrunc(sum, irb.getInt32Ty()), k);
			c = irb.CreateLShr(sum, 32);
		}
		storeNeonVector(ai->operands[0].reg, r, irb);
		auto* cbit = irb.CreateShl(irb.CreateTrunc(c, irb.getInt32Ty()), 29);
		auto* nf = irb.CreateOr(irb.CreateAnd(fpscr, irb.getInt32(~(1u << 29))), cbit);
		storeRegister(ARM_REG_FPSCR_NZCV, nf, irb);
		return;
	}

	if ((op.size() == 5 && (op.compare(0, 4, "vldr") == 0 || op.compare(0, 4, "vstr") == 0)))
	{
		bool load = op[1] == 'l';
		unsigned mb = op[4] == 'b' ? 8 : op[4] == 'h' ? 16 : op[4] == 'w' ? 32 : op[4] == 'd' ? 64 : 0;
		if (mb == 0 || mb > lb || ai->op_count != 2 || ai->operands[1].type != ARM_OP_MEM
				|| ai->operands[0].type != ARM_OP_REG)
		{
			fallback();
			return;
		}
		auto& mo = ai->operands[1];
		if (mo.mem.index != ARM_REG_INVALID || mo.mem.base == ARM_REG_INVALID || lb == 64)
		{
			fallback();
			return;
		}
		int64_t disp = mo.mem.disp;
		if (mo.subtracted && disp > 0) disp = -disp;
		auto* base = loadRegister(mo.mem.base, irb);
		auto* offV = llvm::ConstantInt::getSigned(ty, disp);
		bool post = _insn && _insn->detail && _insn->detail->arm.post_index;
		llvm::Value* addr = post ? base : irb.CreateAdd(base, offV);
		auto* memTy = irb.getIntNTy(mb);
		auto* laneTy = irb.getIntNTy(lb);
		auto q = ai->operands[0].reg;
		if (load)
		{
			llvm::Value* v = llvm::UndefValue::get(llvm::FixedVectorType::get(laneTy, n));
			for (unsigned k = 0; k < n; ++k)
			{
				auto* at = irb.CreateAdd(addr, llvm::ConstantInt::get(ty, k * mb / 8));
				llvm::Value* e = loadIntPtr(irb, at, memTy);
				e = sgn ? irb.CreateSExtOrTrunc(e, laneTy) : irb.CreateZExtOrTrunc(e, laneTy);
				v = irb.CreateInsertElement(v, e, uint64_t(k));
			}
			storeNeonVector(q, v, irb);
		}
		else
		{
			auto* v = loadNeonVector(q, lb, false, irb);
			for (unsigned k = 0; k < n; ++k)
			{
				auto* at = irb.CreateAdd(addr, llvm::ConstantInt::get(ty, k * mb / 8));
				storeIntPtr(irb, irb.CreateTrunc(irb.CreateExtractElement(v, uint64_t(k)), memTy), at, memTy);
			}
		}
		if (insnWriteback())
		{
			storeRegister(mo.mem.base, irb.CreateAdd(base, offV), irb);
		}
		return;
	}

	// VLD2x/VLD4x/VST2x/VST4x: each beat-pattern instruction is lifted as the
	// complete (de)interleave; a full group of 2 or 4 then yields the exact result.
	if (op.size() == 5 && (op.compare(0, 4, "vld2") == 0 || op.compare(0, 4, "vld4") == 0
			|| op.compare(0, 4, "vst2") == 0 || op.compare(0, 4, "vst4") == 0))
	{
		unsigned nr = op[3] - '0';
		bool load = op[1] == 'l';
		if (ai->op_count != nr + 1 || ai->operands[nr].type != ARM_OP_MEM || lb == 64)
		{
			fallback();
			return;
		}
		auto& mo = ai->operands[nr];
		if (mo.mem.index != ARM_REG_INVALID || mo.mem.base == ARM_REG_INVALID)
		{
			fallback();
			return;
		}
		auto* base = loadRegister(mo.mem.base, irb);
		auto* laneTy = irb.getIntNTy(lb);
		std::vector<llvm::Value*> vs;
		for (unsigned r = 0; r < nr; ++r)
		{
			vs.push_back(load ? static_cast<llvm::Value*>(llvm::UndefValue::get(llvm::FixedVectorType::get(laneTy, n)))
					: loadNeonVector(ai->operands[r].reg, lb, false, irb));
		}
		for (unsigned k = 0; k < n; ++k)
		{
			for (unsigned r = 0; r < nr; ++r)
			{
				auto* at = irb.CreateAdd(base, llvm::ConstantInt::get(ty, (k * nr + r) * lb / 8));
				if (load)
				{
					vs[r] = irb.CreateInsertElement(vs[r], loadIntPtr(irb, at, laneTy), uint64_t(k));
				}
				else
				{
					storeIntPtr(irb, irb.CreateExtractElement(vs[r], uint64_t(k)), at, laneTy);
				}
			}
		}
		if (load)
		{
			for (unsigned r = 0; r < nr; ++r)
			{
				storeNeonVector(ai->operands[r].reg, vs[r], irb);
			}
		}
		if (insnWriteback())
		{
			storeRegister(mo.mem.base, irb.CreateAdd(base, llvm::ConstantInt::get(ty, 16 * nr)), irb);
		}
		return;
	}

	if (op == "vidup" || op == "vddup")
	{
		if (ai->op_count != 3 || ai->operands[1].type != ARM_OP_REG || ai->operands[2].type != ARM_OP_IMM || lb > 32)
		{
			fallback();
			return;
		}
		bool up = op == "vidup";
		uint64_t step = static_cast<uint64_t>(ai->operands[2].imm);
		auto rn = ai->operands[1].reg;
		auto* start = loadRegister(rn, irb);
		auto* laneTy = irb.getIntNTy(lb);
		llvm::Value* v = llvm::UndefValue::get(llvm::FixedVectorType::get(laneTy, n));
		for (unsigned k = 0; k < n; ++k)
		{
			auto* d = llvm::ConstantInt::get(ty, k * step);
			auto* e = up ? irb.CreateAdd(start, d) : irb.CreateSub(start, d);
			v = irb.CreateInsertElement(v, irb.CreateTrunc(e, laneTy), uint64_t(k));
		}
		storeNeonVector(ai->operands[0].reg, v, irb);
		auto* tot = llvm::ConstantInt::get(ty, n * step);
		storeRegister(rn, up ? irb.CreateAdd(start, tot) : irb.CreateSub(start, tot), irb);
		return;
	}

	if (op == "viwdup" || op == "vdwdup")
	{
		if (ai->op_count != 4 || ai->operands[1].type != ARM_OP_REG || ai->operands[2].type != ARM_OP_REG
				|| ai->operands[3].type != ARM_OP_IMM || lb > 32)
		{
			fallback();
			return;
		}
		bool up = op == "viwdup";
		auto* step = llvm::ConstantInt::get(ty, static_cast<uint64_t>(ai->operands[3].imm));
		auto rn = ai->operands[1].reg;
		llvm::Value* off = loadRegister(rn, irb);
		auto* wrap = loadRegister(ai->operands[2].reg, irb);
		auto* zero = llvm::ConstantInt::get(ty, 0);
		auto* laneTy = irb.getIntNTy(lb);
		llvm::Value* v = llvm::UndefValue::get(llvm::FixedVectorType::get(laneTy, n));
		for (unsigned k = 0; k < n; ++k)
		{
			v = irb.CreateInsertElement(v, irb.CreateTrunc(off, laneTy), uint64_t(k));
			if (up)
			{
				off = irb.CreateAdd(off, step);
				off = irb.CreateSelect(irb.CreateICmpEQ(off, wrap), zero, off);
			}
			else
			{
				off = irb.CreateSelect(irb.CreateICmpEQ(off, zero), wrap, off);
				off = irb.CreateSub(off, step);
			}
		}
		storeNeonVector(ai->operands[0].reg, v, irb);
		storeRegister(rn, off, irb);
		return;
	}

	if (op == "vcadd" || op == "vhcadd" || op == "vcmla")
	{
		if (ai->op_count != 4 || ai->operands[3].type != ARM_OP_IMM || lb == 8 || lb == 64)
		{
			fallback();
			return;
		}
		if (operandLane(ai->operands[2]) >= 0 || neonBitWidth(ai->operands[0].reg) == 0)
		{
			fallback();
			return;
		}
		n = neonBitWidth(ai->operands[0].reg) / lb;
		int64_t rot = ai->operands[3].imm;
		bool half = op == "vhcadd";
		if (half && fp) { fallback(); return; }
		if (op == "vcmla" && !fp) { fallback(); return; }
		auto* a = loadNeonVector(ai->operands[1].reg, lb, fp, irb);
		auto* b = loadNeonVector(ai->operands[2].reg, lb, fp, irb);
		auto* d = loadNeonVector(ai->operands[0].reg, lb, fp, irb);
		auto add = [&](llvm::Value* x, llvm::Value* y) { return fp ? irb.CreateFAdd(x, y) : irb.CreateAdd(x, y); };
		auto sub = [&](llvm::Value* x, llvm::Value* y) { return fp ? irb.CreateFSub(x, y) : irb.CreateSub(x, y); };
		auto mul = [&](llvm::Value* x, llvm::Value* y) { return irb.CreateFMul(x, y); };
		llvm::Value* r = d;
		auto* laneTy = irb.getIntNTy(lb);
		auto* wideTy = irb.getIntNTy(lb * 2);
		auto hv = [&](llvm::Value* x, llvm::Value* y, bool isAdd) -> llvm::Value* {
			if (!half) return isAdd ? add(x, y) : sub(x, y);
			auto* xw = irb.CreateSExt(x, wideTy);
			auto* yw = irb.CreateSExt(y, wideTy);
			auto* s = isAdd ? irb.CreateAdd(xw, yw) : irb.CreateSub(xw, yw);
			return irb.CreateTrunc(irb.CreateAShr(s, llvm::ConstantInt::get(wideTy, 1)), laneTy);
		};
		for (unsigned k = 0; k + 1 < n + 1; k += 2)
		{
			auto* ae = irb.CreateExtractElement(a, uint64_t(k));
			auto* ao = irb.CreateExtractElement(a, uint64_t(k + 1));
			auto* be = irb.CreateExtractElement(b, uint64_t(k));
			auto* bo = irb.CreateExtractElement(b, uint64_t(k + 1));
			llvm::Value *re = nullptr, *im = nullptr;
			if (op == "vcmla")
			{
				auto* de = irb.CreateExtractElement(d, uint64_t(k));
				auto* dO = irb.CreateExtractElement(d, uint64_t(k + 1));
				switch (rot)
				{
					case 0: re = add(de, mul(ae, be)); im = add(dO, mul(ae, bo)); break;
					case 90: re = sub(de, mul(ao, bo)); im = add(dO, mul(ao, be)); break;
					case 180: re = sub(de, mul(ae, be)); im = sub(dO, mul(ae, bo)); break;
					case 270: re = add(de, mul(ao, bo)); im = sub(dO, mul(ao, be)); break;
					default: fallback(); return;
				}
			}
			else
			{
				if (rot == 90) { re = hv(ae, bo, false); im = hv(ao, be, true); }
				else if (rot == 270) { re = hv(ae, bo, true); im = hv(ao, be, false); }
				else { fallback(); return; }
			}
			r = irb.CreateInsertElement(r, re, uint64_t(k));
			r = irb.CreateInsertElement(r, im, uint64_t(k + 1));
		}
		storeNeonVector(ai->operands[0].reg, r, irb);
		return;
	}
	fallback();
}


/**
 * VSHLC (MVE whole-vector shift left with carry), VINS/VMOVX (fp16 half moves)
 * and VJCVT (JavaScript double-to-int32 conversion; the Z flag is not set).
 */
void Capstone2LlvmIrTranslatorArm_impl::translateArmMiscFp(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ai, irb); };
	auto* i32 = irb.getInt32Ty();
	std::string m = i->mnemonic;
	std::string op = m.substr(0, m.find('.'));
	if (op == "vshlc")
	{
		if (ai->op_count != 3 || ai->operands[2].type != ARM_OP_IMM) { fallback(); return; }
		unsigned sh = unsigned(ai->operands[2].imm);
		if (sh < 1 || sh > 32) { fallback(); return; }
		auto* v = loadNeonVector(ai->operands[0].reg, 32, false, irb);
		auto rd = ai->operands[1].reg;
		auto* i64 = irb.getInt64Ty();
		llvm::Value* carry = irb.CreateZExtOrTrunc(loadRegister(rd, irb), i32);
		uint64_t cm = sh == 32 ? 0xffffffffull : ((1ull << sh) - 1);
		carry = irb.CreateAnd(carry, llvm::ConstantInt::get(i32, cm));
		llvm::Value* r = v;
		for (unsigned k = 0; k < 4; ++k)
		{
			auto* e = irb.CreateZExt(irb.CreateExtractElement(v, uint64_t(k)), i64);
			auto* w = irb.CreateOr(irb.CreateShl(e, sh), irb.CreateZExt(carry, i64));
			r = irb.CreateInsertElement(r, irb.CreateTrunc(w, i32), uint64_t(k));
			carry = irb.CreateTrunc(irb.CreateLShr(w, 32), i32);
		}
		storeNeonVector(ai->operands[0].reg, r, irb);
		storeRegister(rd, carry, irb);
		return;
	}
	if (op == "vins" || op == "vmovx")
	{
		if (ai->op_count != 2) { fallback(); return; }
		auto* src = loadNeonBits(ai->operands[1].reg, irb);
		if (src->getType()->getIntegerBitWidth() != 32) { fallback(); return; }
		llvm::Value* r = nullptr;
		if (op == "vmovx")
		{
			r = irb.CreateLShr(src, 16);
		}
		else
		{
			auto* d = loadNeonBits(ai->operands[0].reg, irb);
			r = irb.CreateOr(irb.CreateAnd(d, llvm::ConstantInt::get(i32, 0xffff)), irb.CreateShl(src, 16));
		}
		storeNeonBits(ai->operands[0].reg, r, irb);
		return;
	}
	if (op == "vjcvt")
	{
		if (ai->op_count != 2) { fallback(); return; }
		auto* bits = loadNeonBits(ai->operands[1].reg, irb);
		if (bits->getType()->getIntegerBitWidth() != 64) { fallback(); return; }
		auto* i64 = irb.getInt64Ty();
		auto* i128 = irb.getInt128Ty();
		auto* ex = irb.CreateAnd(irb.CreateLShr(bits, 52), llvm::ConstantInt::get(i64, 0x7ff));
		auto* frac = irb.CreateAnd(bits, llvm::ConstantInt::get(i64, (1ull << 52) - 1));
		auto* mant = irb.CreateZExt(irb.CreateOr(frac, llvm::ConstantInt::get(i64, 1ull << 52)), i128);
		// value = mant * 2^(ex - 1075)
		auto* e = irb.CreateSub(ex, llvm::ConstantInt::get(i64, 1075));
		auto* isLeft = irb.CreateICmpSGE(e, llvm::ConstantInt::get(i64, 0));
		auto* lAmt = irb.CreateSelect(irb.CreateICmpSGT(e, llvm::ConstantInt::get(i64, 64)),
				llvm::ConstantInt::get(i64, 64), e);
		auto* nE = irb.CreateNeg(e);
		auto* rAmt = irb.CreateSelect(irb.CreateICmpSGT(nE, llvm::ConstantInt::get(i64, 64)),
				llvm::ConstantInt::get(i64, 64), nE);
		auto* left = irb.CreateShl(mant, irb.CreateZExt(irb.CreateSelect(isLeft, lAmt, llvm::ConstantInt::get(i64, 0)), i128));
		auto* right = irb.CreateLShr(mant, irb.CreateZExt(irb.CreateSelect(isLeft, llvm::ConstantInt::get(i64, 0), rAmt), i128));
		llvm::Value* r = irb.CreateTrunc(irb.CreateSelect(isLeft, left, right), i32);
		auto* zeroExp = irb.CreateICmpEQ(ex, llvm::ConstantInt::get(i64, 0));
		auto* special = irb.CreateICmpEQ(ex, llvm::ConstantInt::get(i64, 0x7ff));
		r = irb.CreateSelect(irb.CreateOr(zeroExp, special), llvm::ConstantInt::get(i32, 0), r);
		auto* neg = irb.CreateICmpSLT(bits, llvm::ConstantInt::get(i64, 0));
		r = irb.CreateSelect(neg, irb.CreateNeg(r), r);
		storeNeonBits(ai->operands[0].reg, r, irb);
		return;
	}
	fallback();
}


/**
 * Int8 dot products VSDOT/VUDOT/VUSDOT/VSUDOT (vector and indexed) and the
 * int8 matrix multiply-accumulate VSMMLA/VUMMLA/VUSMMLA.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonDot(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ai, irb); };
	if (ai->op_count != 3)
	{
		fallback();
		return;
	}
	for (unsigned k = 0; k < 3; ++k)
	{
		if (ai->operands[k].type != ARM_OP_REG || !isNeonRegister(ai->operands[k].reg))
		{
			fallback();
			return;
		}
	}
	std::string m = i->mnemonic;
	std::string op = m.substr(0, m.find('.'));
	bool aS = op == "vsdot" || op == "vsudot" || op == "vsmmla";
	bool bS = op == "vsdot" || op == "vusdot" || op == "vsmmla" || op == "vusmmla";
	bool mmla = op.size() > 4 && op.compare(op.size() - 4, 4, "mmla") == 0;
	auto* i32 = irb.getInt32Ty();
	auto rd = ai->operands[0].reg;
	auto* d = loadNeonVector(rd, 32, false, irb);
	auto* a = loadNeonVector(ai->operands[1].reg, 8, false, irb);
	auto* b = loadNeonVector(ai->operands[2].reg, 8, false, irb);
	unsigned n = llvm::cast<llvm::FixedVectorType>(d->getType())->getNumElements();
	int lane = operandLane(ai->operands[2]);
	auto ex = [&](llvm::Value* v, unsigned k, bool s) {
		auto* e = irb.CreateExtractElement(v, uint64_t(k));
		return s ? irb.CreateSExt(e, i32) : irb.CreateZExt(e, i32);
	};
	llvm::Value* r = d;
	if (mmla)
	{
		if (n != 4 || lane >= 0) { fallback(); return; }
		for (unsigned row = 0; row < 2; ++row)
		{
			for (unsigned col = 0; col < 2; ++col)
			{
				llvm::Value* acc = irb.CreateExtractElement(d, uint64_t(2 * row + col));
				for (unsigned j = 0; j < 8; ++j)
				{
					acc = irb.CreateAdd(acc, irb.CreateMul(ex(a, 8 * row + j, aS), ex(b, 8 * col + j, bS)));
				}
				r = irb.CreateInsertElement(r, acc, uint64_t(2 * row + col));
			}
		}
	}
	else
	{
		for (unsigned k = 0; k < n; ++k)
		{
			llvm::Value* acc = irb.CreateExtractElement(d, uint64_t(k));
			unsigned bk = lane >= 0 ? unsigned(lane) : k;
			for (unsigned j = 0; j < 4; ++j)
			{
				acc = irb.CreateAdd(acc, irb.CreateMul(ex(a, 4 * k + j, aS), ex(b, 4 * bk + j, bS)));
			}
			r = irb.CreateInsertElement(r, acc, uint64_t(k));
		}
	}
	storeNeonVector(rd, r, irb);
}


/**
 * BFloat16 VDOT (vector and indexed), VMMLA and the widening VFMAB/VFMAT.
 * bf16 elements are widened exactly to f32; VFMAB/VFMAT are fused, VDOT and
 * VMMLA use ordinary f32 multiply/add (their round-to-odd intermediate is
 * not modelled).
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonBf16(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ai, irb); };
	if (ai->op_count != 3)
	{
		fallback();
		return;
	}
	for (unsigned k = 0; k < 3; ++k)
	{
		if (ai->operands[k].type != ARM_OP_REG || !isNeonRegister(ai->operands[k].reg))
		{
			fallback();
			return;
		}
	}
	auto* i32 = irb.getInt32Ty();
	auto* f32 = irb.getFloatTy();
	auto rd = ai->operands[0].reg;
	auto* d = loadNeonVector(rd, 32, false, irb);
	auto* a = loadNeonVector(ai->operands[1].reg, 16, false, irb);
	auto* b = loadNeonVector(ai->operands[2].reg, 16, false, irb);
	unsigned n = llvm::cast<llvm::FixedVectorType>(d->getType())->getNumElements();
	int lane = operandLane(ai->operands[2]);
	auto bf = [&](llvm::Value* v, unsigned k) {
		auto* e = irb.CreateZExt(irb.CreateExtractElement(v, uint64_t(k)), i32);
		return irb.CreateBitCast(irb.CreateShl(e, 16), f32);
	};
	auto df = [&](unsigned k) {
		return irb.CreateBitCast(irb.CreateExtractElement(d, uint64_t(k)), f32);
	};
	llvm::Value* r = d;
	auto put = [&](unsigned k, llvm::Value* v) {
		r = irb.CreateInsertElement(r, irb.CreateBitCast(v, i32), uint64_t(k));
	};
	switch (i->id)
	{
		case ARM_INS_VDOT:
			for (unsigned k = 0; k < n; ++k)
			{
				unsigned bk = lane >= 0 ? unsigned(lane) : k;
				auto* s = irb.CreateFAdd(
						irb.CreateFMul(bf(a, 2 * k), bf(b, 2 * bk)),
						irb.CreateFMul(bf(a, 2 * k + 1), bf(b, 2 * bk + 1)));
				put(k, irb.CreateFAdd(df(k), s));
			}
			break;
		case ARM_INS_VMMLA:
			if (n != 4 || lane >= 0) { fallback(); return; }
			for (unsigned row = 0; row < 2; ++row)
			{
				for (unsigned col = 0; col < 2; ++col)
				{
					llvm::Value* acc = df(2 * row + col);
					for (unsigned j = 0; j < 4; ++j)
					{
						acc = irb.CreateFAdd(acc, irb.CreateFMul(bf(a, 4 * row + j), bf(b, 4 * col + j)));
					}
					put(2 * row + col, acc);
				}
			}
			break;
		case ARM_INS_VFMAB:
		case ARM_INS_VFMAT:
		{
			unsigned t = i->id == ARM_INS_VFMAT ? 1 : 0;
			auto* fma = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fma, {f32});
			for (unsigned k = 0; k < n; ++k)
			{
				unsigned bk = lane >= 0 ? unsigned(lane) : 2 * k + t;
				put(k, irb.CreateCall(fma, {bf(a, 2 * k + t), bf(b, bk), df(k)}));
			}
			break;
		}
		default:
			fallback();
			return;
	}
	storeNeonVector(rd, r, irb);
}


/**
 * VSCCLRM: zero every listed S/D register and VPR (Security Extension
 * register-list clear before a non-secure call/return).
 */
void Capstone2LlvmIrTranslatorArm_impl::translateVscclrm(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	for (unsigned k = 0; k < ai->op_count; ++k)
	{
		auto& o = ai->operands[k];
		if (o.type != ARM_OP_REG || getRegister(o.reg) == nullptr)
		{
			translatePseudoAsmGeneric(i, ai, irb);
			return;
		}
	}
	for (unsigned k = 0; k < ai->op_count; ++k)
	{
		auto r = ai->operands[k].reg;
		storeRegister(r, llvm::Constant::getNullValue(getRegisterType(r)), irb);
	}
}


/**
 * MVE predication: VPR.P0 is modelled as a 16-bit byte mask in ARM_REG_VPR.
 * VCTP, VPNOT and the VPT compare write it and VPSEL reads it. The VPT/VPST
 * mask fields and the predication of the instructions inside a VPT block
 * are not modelled; those instructions execute unpredicated.
 * Capstone drops the last vector operand of VPT/VPSEL, so operands are
 * parsed from op_str.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMvePred(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ai, irb); };
	std::string m = i->mnemonic;
	auto dot = m.find('.');
	std::string op = m.substr(0, dot);
	std::string dt = dot == std::string::npos ? "" : m.substr(dot + 1);
	auto* i32 = irb.getInt32Ty();
	std::vector<std::string> toks;
	{
		std::string cur;
		for (const char* c = i->op_str; c && *c; ++c)
		{
			if (*c == ',' || *c == ' ')
			{
				if (!cur.empty()) toks.push_back(cur);
				cur.clear();
			}
			else cur += *c;
		}
		if (!cur.empty()) toks.push_back(cur);
	}
	auto regOf = [](const std::string& t) -> uint32_t {
		if (t.size() >= 2 && t[0] == 'q' && std::isdigit(t[1])) return ARM_REG_Q0 + std::atoi(t.c_str() + 1);
		if (t.size() >= 2 && t[0] == 'r' && std::isdigit(t[1])) return ARM_REG_R0 + std::atoi(t.c_str() + 1);
		if (t == "sp") return ARM_REG_SP;
		if (t == "lr") return ARM_REG_LR;
		if (t == "zr") return ARM_REG_INVALID;
		return ARM_REG_INVALID;
	};
	if (op == "vpst")
	{
		return;
	}
	if (op == "vpnot")
	{
		auto* p = loadRegister(ARM_REG_VPR, irb);
		storeRegister(ARM_REG_VPR, irb.CreateAnd(irb.CreateXor(p, irb.getInt32(0xffff)), irb.getInt32(0xffff)), irb);
		return;
	}
	if (op == "vctp" && toks.size() == 1)
	{
		unsigned lb = unsigned(std::atoi(dt.c_str()));
		auto r = regOf(toks[0]);
		if ((lb != 8 && lb != 16 && lb != 32 && lb != 64) || r == ARM_REG_INVALID)
		{
			fallback();
			return;
		}
		auto* n = irb.CreateZExtOrTrunc(loadRegister(r, irb), i32);
		auto* lanes = irb.getInt32(128 / lb);
		auto* cnt = irb.CreateSelect(irb.CreateICmpULT(n, lanes), n, lanes);
		auto* bits = irb.CreateZExt(irb.CreateMul(cnt, irb.getInt32(lb / 8)), irb.getInt64Ty());
		auto* mask = irb.CreateSub(irb.CreateShl(irb.getInt64(1), bits), irb.getInt64(1));
		storeRegister(ARM_REG_VPR, irb.CreateTrunc(mask, i32), irb);
		return;
	}
	if (op == "vpsel" && toks.size() == 3)
	{
		auto d = regOf(toks[0]), a = regOf(toks[1]), b = regOf(toks[2]);
		if (d == ARM_REG_INVALID || a == ARM_REG_INVALID || b == ARM_REG_INVALID)
		{
			fallback();
			return;
		}
		auto* va = loadNeonVector(a, 8, false, irb);
		auto* vb = loadNeonVector(b, 8, false, irb);
		auto* p = loadRegister(ARM_REG_VPR, irb);
		llvm::Value* r = llvm::UndefValue::get(va->getType());
		for (unsigned k = 0; k < 16; ++k)
		{
			auto* bit = irb.CreateICmpNE(irb.CreateAnd(p, irb.getInt32(1u << k)), irb.getInt32(0));
			r = irb.CreateInsertElement(r, irb.CreateSelect(bit,
					irb.CreateExtractElement(va, k), irb.CreateExtractElement(vb, k)), k);
		}
		storeNeonVector(d, r, irb);
		return;
	}
	if (op.compare(0, 3, "vpt") == 0 && toks.size() == 3 && !dt.empty())
	{
		char kind = dt[0];
		unsigned lb = unsigned(std::atoi(dt.c_str() + 1));
		auto qa = regOf(toks[1]);
		auto rb = regOf(toks[2]);
		if ((lb != 8 && lb != 16 && lb != 32) || qa == ARM_REG_INVALID || rb == ARM_REG_INVALID)
		{
			fallback();
			return;
		}
		bool fp = kind == 'f';
		const std::string& c = toks[0];
		unsigned n = 128 / lb;
		auto* va = loadNeonVector(qa, lb, fp, irb);
		bool scalar = rb >= ARM_REG_R0 && rb <= ARM_REG_R12 ? true : (rb == ARM_REG_SP || rb == ARM_REG_LR);
		llvm::Value* vb = scalar ? nullptr : loadNeonVector(rb, lb, fp, irb);
		llvm::Value* sv = nullptr;
		if (scalar)
		{
			sv = loadRegister(rb, irb);
			sv = irb.CreateTrunc(sv, irb.getIntNTy(lb));
			if (fp) sv = irb.CreateBitCast(sv, lb == 16 ? irb.getHalfTy() : irb.getFloatTy());
		}
		llvm::Value* p = irb.getInt32(0);
		for (unsigned k = 0; k < n; ++k)
		{
			auto* x = irb.CreateExtractElement(va, k);
			auto* y = scalar ? sv : irb.CreateExtractElement(vb, k);
			llvm::Value* t = nullptr;
			if (fp)
			{
				if (c == "eq") t = irb.CreateFCmpOEQ(x, y);
				else if (c == "ne") t = irb.CreateFCmpUNE(x, y);
				else if (c == "ge") t = irb.CreateFCmpOGE(x, y);
				else if (c == "lt") t = irb.CreateFCmpOLT(x, y);
				else if (c == "gt") t = irb.CreateFCmpOGT(x, y);
				else if (c == "le") t = irb.CreateFCmpOLE(x, y);
			}
			else
			{
				if (c == "eq") t = irb.CreateICmpEQ(x, y);
				else if (c == "ne") t = irb.CreateICmpNE(x, y);
				else if (c == "cs" || c == "hs") t = irb.CreateICmpUGE(x, y);
				else if (c == "hi") t = irb.CreateICmpUGT(x, y);
				else if (c == "ge") t = irb.CreateICmpSGE(x, y);
				else if (c == "lt") t = irb.CreateICmpSLT(x, y);
				else if (c == "gt") t = irb.CreateICmpSGT(x, y);
				else if (c == "le") t = irb.CreateICmpSLE(x, y);
			}
			if (t == nullptr)
			{
				fallback();
				return;
			}
			unsigned bm = ((1u << (lb / 8)) - 1) << (k * lb / 8);
			p = irb.CreateOr(p, irb.CreateSelect(t, irb.getInt32(bm), irb.getInt32(0)));
		}
		storeRegister(ARM_REG_VPR, p, irb);
		return;
	}
	fallback();
}


/**
 * VFMAL/VFMSL (FEAT_FHM, A32): Vd.f32[k] += (-)Vn.f16[k] * Vm.f16[k|idx],
 * widened and fused. The D form reads S sources and the Q form D sources.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateNeonFml(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count != 3)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	bool sub = armIsId(i, ARM_INS_VFMSL);
	auto d = ai->operands[0].reg;
	unsigned n = neonBitWidth(d) / 32;
	int lane = operandLane(ai->operands[2]);
	auto* acc = loadNeonVector(d, 32, true, irb);
	auto* a = loadNeonVector(ai->operands[1].reg, 16, true, irb);
	auto* b = loadNeonVector(ai->operands[2].reg, 16, true, irb);
	auto* f32 = irb.getFloatTy();
	auto* fma = llvm::Intrinsic::getOrInsertDeclaration(_module, llvm::Intrinsic::fma, f32);
	llvm::Value* r = acc;
	for (unsigned k = 0; k < n; ++k)
	{
		// Exact f16->f32 widening from the bits (the emulator has no half).
		auto h = [&](llvm::Value* v) -> llvm::Value* {
			if (v->getType()->isHalfTy()) v = irb.CreateBitCast(v, irb.getInt16Ty());
			auto* i32 = irb.getInt32Ty();
			auto* x = irb.CreateZExt(v, i32);
			auto* sgn = irb.CreateShl(irb.CreateLShr(x, 15), 31);
			auto* e = irb.CreateAnd(irb.CreateLShr(x, 10), irb.getInt32(0x1f));
			auto* mt = irb.CreateAnd(x, irb.getInt32(0x3ff));
			auto* normal = irb.CreateOr(irb.CreateShl(irb.CreateAdd(e, irb.getInt32(112)), 23), irb.CreateShl(mt, 13));
			auto* infnan = irb.CreateOr(irb.getInt32(0x7f800000), irb.CreateShl(mt, 13));
			// Subnormal: mant * 2^-24, exact in f32.
			auto* sub = irb.CreateBitCast(irb.CreateFMul(irb.CreateUIToFP(mt, f32),
					llvm::ConstantFP::get(f32, 5.9604644775390625e-08)), i32);
			llvm::Value* bits = irb.CreateSelect(irb.CreateICmpEQ(e, irb.getInt32(0)), sub,
					irb.CreateSelect(irb.CreateICmpEQ(e, irb.getInt32(31)), infnan, normal));
			return irb.CreateBitCast(irb.CreateOr(bits, sgn), f32);
		};
		auto* x = h(irb.CreateExtractElement(a, k));
		if (sub) x = irb.CreateFNeg(x);
		auto* y = h(irb.CreateExtractElement(b, lane >= 0 ? unsigned(lane) : k));
		r = irb.CreateInsertElement(r, irb.CreateCall(fma, {x, y, irb.CreateExtractElement(acc, k)}), k);
	}
	storeNeonVector(d, r, irb);
}


/**
 * SQRSHR/UQRSHL (v8.1-M): 32-bit saturating rounding shift by the signed
 * byte in Rm. SQRSHR shifts right (negative amounts shift left); UQRSHL
 * shifts left (negative amounts shift right with rounding). The Q flag is
 * not modelled.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMveScalarRegShift(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count != 2 || ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	bool sgn = armIsId(i, ARM_INS_SQRSHR);
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto c64 = [&](int64_t v) { return llvm::ConstantInt::getSigned(i64, v); };
	auto* rda = loadRegister(ai->operands[0].reg, irb);
	auto* x = sgn ? irb.CreateSExt(rda, i64) : irb.CreateZExt(rda, i64);
	auto* amt = irb.CreateSExt(irb.CreateTrunc(loadRegister(ai->operands[1].reg, irb), irb.getInt8Ty()), i64);
	// Right-shift amount (positive) in the instruction's own direction.
	llvm::Value* rs = sgn ? amt : irb.CreateNeg(amt);
	rs = irb.CreateSelect(irb.CreateICmpSGT(rs, c64(40)), c64(40), rs);
	rs = irb.CreateSelect(irb.CreateICmpSLT(rs, c64(-33)), c64(-33), rs);
	auto* pos = irb.CreateICmpSGT(rs, c64(0));
	auto* sr = irb.CreateSelect(pos, rs, c64(1));
	auto* rnd = irb.CreateShl(c64(1), irb.CreateSub(sr, c64(1)));
	auto* biased = irb.CreateAdd(x, rnd);
	llvm::Value* right = sgn ? irb.CreateAShr(biased, sr) : irb.CreateLShr(biased, sr);
	auto* left = irb.CreateShl(x, irb.CreateSelect(pos, c64(0), irb.CreateNeg(rs)));
	llvm::Value* r = irb.CreateSelect(pos, right, left);
	if (sgn)
	{
		r = irb.CreateSelect(irb.CreateICmpSGT(r, c64(0x7fffffff)), c64(0x7fffffff), r);
		r = irb.CreateSelect(irb.CreateICmpSLT(r, c64(-0x80000000LL)), c64(-0x80000000LL), r);
	}
	else
	{
		r = irb.CreateSelect(irb.CreateICmpUGT(r, c64(0xffffffff)), c64(0xffffffff), r);
	}
	storeRegister(ai->operands[0].reg, irb.CreateTrunc(r, i32), irb);
}


/**
 * ERET returns to LR (the non-Hyp SUBS PC, LR, #0 view; ELR_hyp is not
 * modelled). RFE{IA,IB,DA,DB} returns to the word loaded from [Rn] at the
 * mode's offset; the SPSR word is not applied to CPSR.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateExceptionReturn(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (armIsId(i, ARM_INS_ERET))
	{
		generateReturnFunctionCall(irb, loadRegister(ARM_REG_LR, irb));
		return;
	}
	if (ai->op_count != 1 || ai->operands[0].type != ARM_OP_MEM)
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	auto* ty = getDefaultType();
	auto rn = ai->operands[0].mem.base;
	auto* base = loadRegister(rn, irb);
	int64_t off = 0, wb = 8;
	switch (i->id)
	{
		case ARM_INS_RFEIA: off = 0; wb = 8; break;
		case ARM_INS_RFEIB: off = 4; wb = 8; break;
		case ARM_INS_RFEDA: off = -4; wb = -8; break;
		case ARM_INS_RFEDB: off = -8; wb = -8; break;
		default:
			translatePseudoAsmGeneric(i, ai, irb);
			return;
	}
	auto* addr = irb.CreateAdd(base, llvm::ConstantInt::getSigned(ty, off));
	auto* target = loadIntPtr(irb, addr, ty);
	if (insnWriteback())
	{
		storeRegister(rn, irb.CreateAdd(base, llvm::ConstantInt::getSigned(ty, wb)), irb);
	}
	generateReturnFunctionCall(irb, target);
}

/**
 * SQRSHRL/UQRSHLL (v8.1-M): 64-bit saturating rounding shift of RdaHi:RdaLo
 * by the signed byte in Rm, saturating to 48 or 64 bits. SQRSHRL shifts right
 * (negative amounts shift left); UQRSHLL shifts left (negative amounts shift
 * right with rounding). A 48-bit result is sign or zero extended. The Q flag
 * is not modelled.
 */
void Capstone2LlvmIrTranslatorArm_impl::translateMveLongRegShift(cs_insn* i, cs_arm* ai, llvm::IRBuilder<>& irb)
{
	if (ai->op_count != 4 || ai->operands[0].type != ARM_OP_REG || ai->operands[1].type != ARM_OP_REG
			|| ai->operands[2].type != ARM_OP_IMM || ai->operands[3].type != ARM_OP_REG
			|| (ai->operands[2].imm != 48 && ai->operands[2].imm != 64))
	{
		translatePseudoAsmGeneric(i, ai, irb);
		return;
	}
	bool sgn = armIsId(i, ARM_INS_SQRSHRL);
	unsigned satBits = unsigned(ai->operands[2].imm);
	auto* i32 = irb.getInt32Ty();
	auto* i64 = irb.getInt64Ty();
	auto* i128 = irb.getIntNTy(128);
	auto c = [&](int64_t v) { return llvm::ConstantInt::getSigned(i128, v); };
	auto* lo = irb.CreateZExt(irb.CreateZExtOrTrunc(loadRegister(ai->operands[0].reg, irb), i32), i64);
	auto* hi = irb.CreateZExt(irb.CreateZExtOrTrunc(loadRegister(ai->operands[1].reg, irb), i32), i64);
	llvm::Value* v64 = irb.CreateOr(irb.CreateShl(hi, 32), lo);
	auto* x = sgn ? irb.CreateSExt(v64, i128) : irb.CreateZExt(v64, i128);
	auto* amt = irb.CreateSExt(irb.CreateTrunc(loadRegister(ai->operands[3].reg, irb), irb.getInt8Ty()), i128);
	// Right-shift amount in the instruction's own direction, clamped so that
	// larger shifts give the same result.
	llvm::Value* rs = sgn ? amt : irb.CreateNeg(amt);
	rs = irb.CreateSelect(irb.CreateICmpSGT(rs, c(66)), c(66), rs);
	rs = irb.CreateSelect(irb.CreateICmpSLT(rs, c(-64)), c(-64), rs);
	auto* pos = irb.CreateICmpSGT(rs, c(0));
	auto* sr = irb.CreateSelect(pos, rs, c(1));
	auto* biased = irb.CreateAdd(x, irb.CreateShl(c(1), irb.CreateSub(sr, c(1))));
	llvm::Value* right = sgn ? irb.CreateAShr(biased, sr) : irb.CreateLShr(biased, sr);
	auto* left = irb.CreateShl(x, irb.CreateSelect(pos, c(0), irb.CreateNeg(rs)));
	llvm::Value* r = irb.CreateSelect(pos, right, left);
	llvm::APInt mxv = sgn ? llvm::APInt::getSignedMaxValue(satBits).sext(128)
			: llvm::APInt::getMaxValue(satBits).zext(128);
	auto* mx = llvm::ConstantInt::get(i128, mxv);
	if (sgn)
	{
		auto* mn = llvm::ConstantInt::get(i128, llvm::APInt::getSignedMinValue(satBits).sext(128));
		r = irb.CreateSelect(irb.CreateICmpSGT(r, mx), mx, irb.CreateSelect(irb.CreateICmpSLT(r, mn), mn, r));
	}
	else
	{
		r = irb.CreateSelect(irb.CreateICmpUGT(r, mx), mx, r);
	}
	auto* r64 = irb.CreateTrunc(r, i64);
	storeRegister(ai->operands[0].reg, irb.CreateTrunc(r64, i32), irb);
	storeRegister(ai->operands[1].reg, irb.CreateTrunc(irb.CreateLShr(r64, 32), i32), irb);
}

} // namespace capstone2llvmir
} // namespace retdec
