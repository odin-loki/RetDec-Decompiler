/**
 * @file src/capstone2llvmir/arm64/arm64_sve2.cpp
 * @brief SVE/SVE2 lane-wise integer, widening, narrowing, bitwise-ternary,
 *        reversed-operand and predicate-logic instructions.
 * @copyright (c) 2026, MIT license
 *
 * Z registers are modelled at the SVE minimum VL of 128 bits (see
 * arm64.cpp), so every vector here is 128 bits. Lanes are computed one by
 * one in a wide integer and then truncated or saturated, which keeps the
 * emitted IR executable by llvmir-emul (no target intrinsics).
 */

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include "arm64_impl.h"

namespace retdec {
namespace capstone2llvmir {

namespace {

unsigned sve2ElemBits(const cs_arm64_op& op)
{
	unsigned e = static_cast<unsigned>(op.vas) & 0xff;
	return (e == 8 || e == 16 || e == 32 || e == 64) ? e : 0;
}

bool sve2IsZ(const cs_arm64_op& op)
{
	return op.type == ARM64_OP_REG && op.reg >= ARM64_REG_Z0 && op.reg <= ARM64_REG_Z31;
}

bool sve2IsP(const cs_arm64_op& op)
{
	return (op.type == ARM64_OP_REG || op.type == ARM64_OP_PRED)
			&& op.reg >= ARM64_REG_P0 && op.reg <= ARM64_REG_P15;
}

constexpr unsigned W = 256; // working width for lane arithmetic

} // anonymous namespace

/**
 * Lane-wise SVE2 integer families. Unhandled forms fall back to a pseudo call.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateSve2(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ai, irb); };
	auto* wt = irb.getIntNTy(W);
	auto vecOf = [&](uint32_t r, unsigned eb) -> llvm::Value* {
		auto* ty = llvm::FixedVectorType::get(irb.getIntNTy(eb), 128 / eb);
		return irb.CreateBitCast(loadRegister(r, irb), ty);
	};
	auto ext = [&](llvm::Value* v, bool s) -> llvm::Value* {
		return s ? irb.CreateSExt(v, wt) : irb.CreateZExt(v, wt);
	};
	auto cW = [&](int64_t v) -> llvm::Value* { return llvm::ConstantInt::get(wt, v, true); };
	// Saturate a W-bit value to an eb-bit signed/unsigned lane.
	auto sat = [&](llvm::Value* v, bool s, unsigned eb) -> llvm::Value* {
		llvm::APInt lo = s ? llvm::APInt::getSignedMinValue(eb).sext(W) : llvm::APInt(W, 0);
		llvm::APInt hi = s ? llvm::APInt::getSignedMaxValue(eb).sext(W) : llvm::APInt::getMaxValue(eb).zext(W);
		auto* l = llvm::ConstantInt::get(wt, lo);
		auto* h = llvm::ConstantInt::get(wt, hi);
		v = irb.CreateSelect(irb.CreateICmpSLT(v, l), l, v);
		v = irb.CreateSelect(irb.CreateICmpSGT(v, h), h, v);
		return irb.CreateTrunc(v, irb.getIntNTy(eb));
	};
	// Shift a W-bit value by a signed amount; negative shifts right, with
	// optional rounding.
	auto shiftBy = [&](llvm::Value* x, llvm::Value* sh, bool s, bool round) -> llvm::Value* {
		auto* lim = cW(200);
		auto* pos = irb.CreateICmpSGE(sh, cW(0));
		auto* l = irb.CreateSelect(irb.CreateICmpSGT(sh, lim), lim, sh);
		auto* nsh = irb.CreateNeg(sh);
		auto* n = irb.CreateSelect(irb.CreateICmpSGT(nsh, lim), lim, nsh);
		llvm::Value* xr = x;
		if (round)
		{
			auto* add = irb.CreateSelect(irb.CreateICmpEQ(n, cW(0)), cW(0),
					irb.CreateShl(cW(1), irb.CreateSub(n, cW(1))));
			xr = irb.CreateAdd(x, add);
		}
		auto* right = s ? irb.CreateAShr(xr, n) : irb.CreateLShr(xr, n);
		return irb.CreateSelect(pos, irb.CreateShl(x, l), right);
	};

	unsigned id = i->id;
	auto& o = ai->operands;
	unsigned n = ai->op_count;

	// ---- Predicate logic: NAND/NOR/ORN/BIC (+S) on P registers.
	if ((id == ARM64_INS_NAND || id == ARM64_INS_NANDS || id == ARM64_INS_NOR || id == ARM64_INS_NORS
			|| id == ARM64_INS_ORNS)
		&& n == 4 && sve2IsP(o[0]) && sve2IsP(o[1]) && sve2IsP(o[2]) && sve2IsP(o[3]))
	{
		auto* pg = loadRegister(o[1].reg, irb);
		auto* a = loadRegister(o[2].reg, irb);
		auto* b = loadRegister(o[3].reg, irb);
		llvm::Value* r = nullptr;
		if (id == ARM64_INS_NAND || id == ARM64_INS_NANDS) r = irb.CreateNot(irb.CreateAnd(a, b));
		else if (id == ARM64_INS_NOR || id == ARM64_INS_NORS) r = irb.CreateNot(irb.CreateOr(a, b));
		else r = irb.CreateOr(a, irb.CreateNot(b));
		r = irb.CreateAnd(r, pg);
		storeRegister(o[0].reg, r, irb);
		if (id == ARM64_INS_NANDS || id == ARM64_INS_NORS || id == ARM64_INS_ORNS)
		{
			sve2SetPredFlags(pg, r, irb);
		}
		return;
	}

	// ---- Bitwise ternary: BSL1N / BSL2N / NBSL (zdn, zdn, zm, zk).
	if ((id == ARM64_INS_BSL1N || id == ARM64_INS_BSL2N || id == ARM64_INS_NBSL)
		&& n == 4 && sve2IsZ(o[0]) && sve2IsZ(o[2]) && sve2IsZ(o[3]))
	{
		auto* d = vecOf(o[1].reg, 64);
		auto* m = vecOf(o[2].reg, 64);
		auto* k = vecOf(o[3].reg, 64);
		if (id == ARM64_INS_BSL1N) d = irb.CreateNot(d);
		if (id == ARM64_INS_BSL2N) m = irb.CreateNot(m);
		llvm::Value* r = irb.CreateOr(irb.CreateAnd(d, k), irb.CreateAnd(m, irb.CreateNot(k)));
		if (id == ARM64_INS_NBSL) r = irb.CreateNot(r);
		storeSveZ(o[0].reg, r, irb);
		return;
	}

	// ---- EORBT / EORTB: interleaving exclusive OR.
	if ((id == ARM64_INS_EORBT || id == ARM64_INS_EORTB) && n == 3
		&& sve2IsZ(o[0]) && sve2IsZ(o[1]) && sve2IsZ(o[2]) && sve2ElemBits(o[0]))
	{
		unsigned eb = sve2ElemBits(o[0]);
		unsigned lanes = 128 / eb;
		llvm::Value* d = vecOf(o[0].reg, eb);
		auto* a = vecOf(o[1].reg, eb);
		auto* b = vecOf(o[2].reg, eb);
		for (unsigned k = 0; k < lanes; k += 2)
		{
			unsigned dst = id == ARM64_INS_EORBT ? k : k + 1;
			unsigned src = id == ARM64_INS_EORBT ? k + 1 : k;
			d = irb.CreateInsertElement(d, irb.CreateXor(irb.CreateExtractElement(a, dst),
					irb.CreateExtractElement(b, src)), dst);
		}
		storeSveZ(o[0].reg, d, irb);
		return;
	}

	// ---- Predicated destructive ops with reversed or extra semantics:
	// <op> zdn, pg/m, zdn, zm|#imm
	{
		enum K { NONE, DIV, SUB, LSL, LSR, ASR, QSUB, HSUB, RSHL, QSHL, QRSHL, ASRD, CNOT };
		K kind = NONE;
		bool s = false, rev = true;
		switch (id)
		{
		case ARM64_INS_SDIVR: kind = DIV; s = true; break;
		case ARM64_INS_UDIVR: kind = DIV; break;
		case ARM64_INS_SUBR: kind = SUB; break;
		case ARM64_INS_LSLR: kind = LSL; break;
		case ARM64_INS_LSRR: kind = LSR; break;
		case ARM64_INS_ASRR: kind = ASR; s = true; break;
		case ARM64_INS_SQSUBR: kind = QSUB; s = true; break;
		case ARM64_INS_UQSUBR: kind = QSUB; break;
		case ARM64_INS_SHSUBR: kind = HSUB; s = true; break;
		case ARM64_INS_UHSUBR: kind = HSUB; break;
		case ARM64_INS_SRSHLR: kind = RSHL; s = true; break;
		case ARM64_INS_URSHLR: kind = RSHL; break;
		case ARM64_INS_SQSHLR: kind = QSHL; s = true; break;
		case ARM64_INS_UQSHLR: kind = QSHL; break;
		case ARM64_INS_SQRSHLR: kind = QRSHL; s = true; break;
		case ARM64_INS_UQRSHLR: kind = QRSHL; break;
		case ARM64_INS_ASRD: kind = ASRD; s = true; rev = false; break;
		case ARM64_INS_CNOT: kind = CNOT; rev = false; break;
		default: break;
		}
		if (kind != NONE)
		{
			unsigned eb = sve2ElemBits(o[0]);
			if (!sve2IsZ(o[0]) || eb == 0)
			{
				fallback();
				return;
			}
			const cs_arm64_op* pg = nullptr;
			const cs_arm64_op* a = nullptr; // first source (zdn)
			const cs_arm64_op* b = nullptr; // second source
			if (n == 4 && sve2IsP(o[1]))
			{
				pg = &o[1]; a = &o[2]; b = &o[3];
			}
			else if (n == 3 && kind == CNOT && sve2IsP(o[1]))
			{
				pg = &o[1]; a = &o[2]; b = &o[2];
			}
			else if (n == 3 && kind == SUB)
			{
				a = &o[1]; b = &o[2];
			}
			else
			{
				fallback();
				return;
			}
			unsigned lanes = 128 / eb;
			auto* et = irb.getIntNTy(eb);
			auto* va = vecOf(a->reg, eb);
			llvm::Value* vb = b->type == ARM64_OP_IMM ? nullptr : vecOf(b->reg, eb);
			llvm::Value* old = vecOf(o[0].reg, eb);
			llvm::Value* mask = pg ? svePredToLaneMask(loadRegister(pg->reg, irb), eb, irb) : nullptr;
			llvm::Value* r = old;
			for (unsigned k = 0; k < lanes; ++k)
			{
				llvm::Value* x = irb.CreateExtractElement(va, k);
				llvm::Value* y = vb ? irb.CreateExtractElement(vb, k) : llvm::ConstantInt::get(et, b->imm);
				if (rev) std::swap(x, y); // result = zm OP zdn
				llvm::Value* v = nullptr;
				switch (kind)
				{
				case DIV:
				{
					auto* z = irb.CreateICmpEQ(y, llvm::ConstantInt::get(et, 0));
					auto* safe = irb.CreateSelect(z, llvm::ConstantInt::get(et, 1), y);
					llvm::Value* q;
					if (s)
					{
						// INT_MIN / -1 wraps to INT_MIN.
						auto* ovf = irb.CreateAnd(irb.CreateICmpEQ(x, llvm::ConstantInt::get(et, llvm::APInt::getSignedMinValue(eb))),
								irb.CreateICmpEQ(y, llvm::ConstantInt::getSigned(et, -1)));
						safe = irb.CreateSelect(ovf, llvm::ConstantInt::get(et, 1), safe);
						q = irb.CreateSDiv(x, safe);
					}
					else q = irb.CreateUDiv(x, safe);
					v = irb.CreateSelect(z, llvm::ConstantInt::get(et, 0), q);
					break;
				}
				case SUB: v = irb.CreateSub(x, y); break;
				case LSL:
				case LSR:
				case ASR:
				{
					auto* big = irb.CreateICmpUGE(y, llvm::ConstantInt::get(et, eb));
					if (kind == ASR)
					{
						auto* amt = irb.CreateSelect(big, llvm::ConstantInt::get(et, eb - 1), y);
						v = irb.CreateAShr(x, amt);
					}
					else
					{
						auto* amt = irb.CreateSelect(big, llvm::ConstantInt::get(et, 0), y);
						v = kind == LSL ? irb.CreateShl(x, amt) : irb.CreateLShr(x, amt);
						v = irb.CreateSelect(big, llvm::ConstantInt::get(et, 0), v);
					}
					break;
				}
				case QSUB: v = sat(irb.CreateSub(ext(x, s), ext(y, s)), s, eb); break;
				case HSUB:
					v = irb.CreateTrunc(irb.CreateAShr(irb.CreateSub(ext(x, s), ext(y, s)), cW(1)), et);
					break;
				case RSHL:
				case QSHL:
				case QRSHL:
				{
					auto* sh = irb.CreateSExt(irb.CreateTrunc(y, irb.getInt8Ty()), wt);
					auto* w = shiftBy(ext(x, s), sh, s, kind != QSHL);
					v = kind == RSHL ? irb.CreateTrunc(w, et) : sat(w, s, eb);
					break;
				}
				case ASRD:
				{
					auto* xs = ext(x, true);
					auto* sh = cW(b->imm);
					auto* bias = irb.CreateSelect(irb.CreateICmpSLT(xs, cW(0)),
							irb.CreateSub(irb.CreateShl(cW(1), sh), cW(1)), cW(0));
					v = irb.CreateTrunc(irb.CreateAShr(irb.CreateAdd(xs, bias), sh), et);
					break;
				}
				case CNOT:
					v = irb.CreateZExt(irb.CreateICmpEQ(x, llvm::ConstantInt::get(et, 0)), et);
					break;
				default: break;
				}
				if (mask)
				{
					v = irb.CreateSelect(irb.CreateExtractElement(mask, k), v, irb.CreateExtractElement(old, k));
				}
				r = irb.CreateInsertElement(r, v, k);
			}
			storeSveZ(o[0].reg, r, irb);
			return;
		}
	}

	// ---- Widening bottom/top ops: Zd.W, Zn.N|W, Zm.N|#imm
	{
		enum K { NONE, ADD, SUB, ABD, MUL, QDMUL, PMUL, SHLL };
		K kind = NONE;
		bool s = false, acc = false, accSub = false, wideA = false, qacc = false;
		int selA = 0, selB = 0; // 0 bottom, 1 top
		std::string m = i->mnemonic;
		auto has = [&](const char* p) { return m.compare(0, std::strlen(p), p) == 0; };
		auto tail = [&](const char* t) {
			return m.size() >= std::strlen(t) && m.compare(m.size() - std::strlen(t), std::strlen(t), t) == 0;
		};
		struct Row { const char* pfx; K k; bool s; bool acc; bool accSub; bool wideA; bool qacc; };
		static const Row rows[] = {
			{"saddl", ADD, true, false, false, false, false}, {"uaddl", ADD, false, false, false, false, false},
			{"ssubl", SUB, true, false, false, false, false}, {"usubl", SUB, false, false, false, false, false},
			{"sabdl", ABD, true, false, false, false, false}, {"uabdl", ABD, false, false, false, false, false},
			{"sabal", ABD, true, true, false, false, false}, {"uabal", ABD, false, true, false, false, false},
			{"smull", MUL, true, false, false, false, false}, {"umull", MUL, false, false, false, false, false},
			{"smlal", MUL, true, true, false, false, false}, {"umlal", MUL, false, true, false, false, false},
			{"smlsl", MUL, true, true, true, false, false}, {"umlsl", MUL, false, true, true, false, false},
			{"sqdmull", QDMUL, true, false, false, false, false},
			{"sqdmlal", QDMUL, true, true, false, false, true}, {"sqdmlsl", QDMUL, true, true, true, false, true},
			{"pmull", PMUL, false, false, false, false, false},
			{"saddw", ADD, true, false, false, true, false}, {"uaddw", ADD, false, false, false, true, false},
			{"ssubw", SUB, true, false, false, true, false}, {"usubw", SUB, false, false, false, true, false},
			{"sshll", SHLL, true, false, false, false, false}, {"ushll", SHLL, false, false, false, false, false},
		};
		for (auto& rw : rows)
		{
			if (!has(rw.pfx)) continue;
			std::string rest = m.substr(std::strlen(rw.pfx));
			if (rest == "b") { selA = selB = 0; }
			else if (rest == "t") { selA = selB = 1; }
			else if (rest == "bt") { selA = 0; selB = 1; }
			else if (rest == "tb") { selA = 1; selB = 0; }
			else continue;
			kind = rw.k; s = rw.s; acc = rw.acc; accSub = rw.accSub; wideA = rw.wideA; qacc = rw.qacc;
			break;
		}
		(void) tail;
		if (kind != NONE)
		{
			unsigned wb = sve2ElemBits(o[0]);
			unsigned nb = wb / 2;
			if (n != 3 || !sve2IsZ(o[0]) || !sve2IsZ(o[1]) || nb < 8
				|| (kind == SHLL ? o[2].type != ARM64_OP_IMM : !sve2IsZ(o[2])))
			{
				fallback();
				return;
			}
			unsigned lanes = 128 / wb;
			auto* wty = irb.getIntNTy(wb);
			auto* va = vecOf(o[1].reg, wideA ? wb : nb);
			llvm::Value* vb = kind == SHLL ? nullptr : vecOf(o[2].reg, nb);
			int idx = (kind != SHLL) ? o[2].vector_index : -1;
			llvm::Value* d = vecOf(o[0].reg, wb);
			llvm::Value* r = d;
			for (unsigned k = 0; k < lanes; ++k)
			{
				llvm::Value* x = irb.CreateExtractElement(va, wideA ? k : 2 * k + selA);
				x = ext(x, s);
				llvm::Value* y = nullptr;
				if (vb)
				{
					unsigned bi = idx >= 0 ? unsigned(idx) : 2 * k + selB;
					y = ext(irb.CreateExtractElement(vb, bi), s);
				}
				llvm::Value* v = nullptr;
				switch (kind)
				{
				case ADD: v = irb.CreateAdd(x, y); break;
				case SUB: v = irb.CreateSub(x, y); break;
				case ABD:
				{
					auto* t = irb.CreateSub(x, y);
					v = irb.CreateSelect(irb.CreateICmpSLT(t, cW(0)), irb.CreateNeg(t), t);
					break;
				}
				case MUL: v = irb.CreateMul(x, y); break;
				case QDMUL: v = irb.CreateMul(irb.CreateMul(x, y), cW(2)); break;
				case PMUL:
				{
					v = cW(0);
					for (unsigned bit = 0; bit < nb; ++bit)
					{
						auto* on = irb.CreateAnd(irb.CreateLShr(y, cW(bit)), cW(1));
						auto* term = irb.CreateMul(irb.CreateShl(x, cW(bit)), on);
						v = irb.CreateXor(v, term);
					}
					break;
				}
				case SHLL: v = irb.CreateShl(x, cW(o[2].imm)); break;
				default: break;
				}
				if (kind == QDMUL)
				{
					v = ext(sat(v, true, wb), true);
				}
				if (acc)
				{
					auto* dk = ext(irb.CreateExtractElement(d, k), s || qacc);
					v = accSub ? irb.CreateSub(dk, v) : irb.CreateAdd(dk, v);
				}
				v = (kind == QDMUL) ? sat(v, true, wb) : irb.CreateTrunc(v, wty);
				r = irb.CreateInsertElement(r, v, k);
			}
			storeSveZ(o[0].reg, r, irb);
			return;
		}
	}

	// ---- Narrowing bottom/top ops: Zd.N, Zn.W[, Zm.W | #imm]
	{
		enum K { NONE, HN, SHRN, XTN };
		K kind = NONE;
		bool round = false, sub = false, srcS = false, dstS = false, satur = false;
		std::string m = i->mnemonic;
		bool top = !m.empty() && m.back() == 't';
		bool bot = !m.empty() && m.back() == 'b';
		std::string base = (top || bot) ? m.substr(0, m.size() - 1) : m;
		if (top || bot)
		{
			if (base == "addhn") kind = HN;
			else if (base == "raddhn") { kind = HN; round = true; }
			else if (base == "subhn") { kind = HN; sub = true; }
			else if (base == "rsubhn") { kind = HN; sub = true; round = true; }
			else if (base == "shrn") kind = SHRN;
			else if (base == "rshrn") { kind = SHRN; round = true; }
			else if (base == "sqshrn") { kind = SHRN; srcS = dstS = satur = true; }
			else if (base == "sqrshrn") { kind = SHRN; srcS = dstS = satur = round = true; }
			else if (base == "uqshrn") { kind = SHRN; satur = true; }
			else if (base == "uqrshrn") { kind = SHRN; satur = round = true; }
			else if (base == "sqshrun") { kind = SHRN; srcS = satur = true; }
			else if (base == "sqrshrun") { kind = SHRN; srcS = satur = round = true; }
			else if (base == "sqxtn") { kind = XTN; srcS = dstS = satur = true; }
			else if (base == "uqxtn") { kind = XTN; satur = true; }
			else if (base == "sqxtun") { kind = XTN; srcS = satur = true; }
		}
		if (kind != NONE)
		{
			unsigned nb = sve2ElemBits(o[0]);
			unsigned wb = nb * 2;
			bool okOps = sve2IsZ(o[0]) && sve2IsZ(o[1]) && nb >= 8 && nb <= 32
					&& ((kind == XTN && n == 2) || (kind == SHRN && n == 3 && o[2].type == ARM64_OP_IMM)
						|| (kind == HN && n == 3 && sve2IsZ(o[2])));
			if (!okOps)
			{
				fallback();
				return;
			}
			unsigned lanes = 128 / wb;
			auto* nty = irb.getIntNTy(nb);
			auto* va = vecOf(o[1].reg, wb);
			llvm::Value* vb = kind == HN ? vecOf(o[2].reg, wb) : nullptr;
			llvm::Value* r = top ? vecOf(o[0].reg, nb)
					: llvm::Constant::getNullValue(llvm::FixedVectorType::get(nty, 128 / nb));
			for (unsigned k = 0; k < lanes; ++k)
			{
				llvm::Value* x = ext(irb.CreateExtractElement(va, k), srcS);
				llvm::Value* v = nullptr;
				if (kind == HN)
				{
					auto* y = ext(irb.CreateExtractElement(vb, k), false);
					x = ext(irb.CreateExtractElement(va, k), false);
					llvm::Value* t = sub ? irb.CreateSub(x, y) : irb.CreateAdd(x, y);
					if (round) t = irb.CreateAdd(t, cW(int64_t(1) << (nb - 1)));
					v = irb.CreateTrunc(irb.CreateLShr(t, cW(nb)), nty);
				}
				else if (kind == SHRN)
				{
					auto* w = shiftBy(x, cW(-o[2].imm), srcS, round);
					v = satur ? sat(w, dstS, nb) : irb.CreateTrunc(w, nty);
				}
				else
				{
					v = sat(x, dstS, nb);
				}
				r = irb.CreateInsertElement(r, v, 2 * k + (top ? 1 : 0));
			}
			storeSveZ(o[0].reg, r, irb);
			return;
		}
	}

	// ---- ANDV / ORV / EORV: predicated bitwise reductions to a scalar.
	if ((id == ARM64_INS_ANDV || id == ARM64_INS_ORV || id == ARM64_INS_EORV)
		&& n == 3 && sve2IsP(o[1]) && sve2IsZ(o[2]) && sve2ElemBits(o[2]))
	{
		unsigned eb = sve2ElemBits(o[2]);
		auto* et = irb.getIntNTy(eb);
		auto* v = vecOf(o[2].reg, eb);
		auto* mask = svePredToLaneMask(loadRegister(o[1].reg, irb), eb, irb);
		llvm::Value* accv = id == ARM64_INS_ANDV ? llvm::ConstantInt::getAllOnesValue(et) : llvm::ConstantInt::get(et, 0);
		for (unsigned k = 0; k < 128 / eb; ++k)
		{
			auto* e = irb.CreateExtractElement(v, k);
			llvm::Value* t = id == ARM64_INS_ANDV ? irb.CreateAnd(accv, e)
					: id == ARM64_INS_ORV ? irb.CreateOr(accv, e) : irb.CreateXor(accv, e);
			accv = irb.CreateSelect(irb.CreateExtractElement(mask, k), t, accv);
		}
		storeRegister(o[0].reg, accv, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	fallback();
}

/**
 * NZCV after a predicate-setting SVE op (PTEST semantics against \p pg):
 * N = first active element true, Z = no active element true,
 * C = last active element not true, V = 0.
 */
void Capstone2LlvmIrTranslatorArm64_impl::sve2SetPredFlags(llvm::Value* pg, llvm::Value* r, llvm::IRBuilder<>& irb)
{
	auto* i1 = irb.getInt1Ty();
	llvm::Value* first = llvm::ConstantInt::getFalse(irb.getContext());
	llvm::Value* seen = llvm::ConstantInt::getFalse(irb.getContext());
	llvm::Value* any = llvm::ConstantInt::getFalse(irb.getContext());
	llvm::Value* last = llvm::ConstantInt::getFalse(irb.getContext());
	for (unsigned k = 0; k < 16; ++k)
	{
		auto* g = irb.CreateExtractElement(pg, k);
		auto* e = irb.CreateExtractElement(r, k);
		first = irb.CreateSelect(irb.CreateAnd(g, irb.CreateNot(seen)), e, first);
		seen = irb.CreateOr(seen, g);
		any = irb.CreateOr(any, irb.CreateAnd(g, e));
		last = irb.CreateSelect(g, e, last);
	}
	(void) i1;
	storeRegister(ARM64_REG_CPSR_N, first, irb);
	storeRegister(ARM64_REG_CPSR_Z, irb.CreateNot(any), irb);
	storeRegister(ARM64_REG_CPSR_C, irb.CreateNot(last), irb);
	storeRegister(ARM64_REG_CPSR_V, llvm::ConstantInt::getFalse(irb.getContext()), irb);
}


namespace {

/// Active element count of an SVE predicate pattern at VL=128.
unsigned sve2PatternCount(const cs_arm64* ai, unsigned lanes)
{
	for (unsigned j = 0; j < ai->op_count; ++j)
	{
		if (ai->operands[j].type != ARM64_OP_SVEPREDPAT) continue;
		unsigned pat = static_cast<unsigned>(ai->operands[j].imm);
		switch (pat)
		{
		case ARM64_SVEPREDPAT_POW2:
		{
			unsigned a = 1;
			while ((a << 1) <= lanes) a <<= 1;
			return a;
		}
		case ARM64_SVEPREDPAT_VL1: return lanes >= 1 ? 1 : 0;
		case ARM64_SVEPREDPAT_VL2: return lanes >= 2 ? 2 : 0;
		case ARM64_SVEPREDPAT_VL3: return lanes >= 3 ? 3 : 0;
		case ARM64_SVEPREDPAT_VL4: return lanes >= 4 ? 4 : 0;
		case ARM64_SVEPREDPAT_VL5: return lanes >= 5 ? 5 : 0;
		case ARM64_SVEPREDPAT_VL6: return lanes >= 6 ? 6 : 0;
		case ARM64_SVEPREDPAT_VL7: return lanes >= 7 ? 7 : 0;
		case ARM64_SVEPREDPAT_VL8: return lanes >= 8 ? 8 : 0;
		case ARM64_SVEPREDPAT_VL16: return lanes >= 16 ? 16 : 0;
		case ARM64_SVEPREDPAT_VL32:
		case ARM64_SVEPREDPAT_VL64:
		case ARM64_SVEPREDPAT_VL128:
		case ARM64_SVEPREDPAT_VL256: return 0;
		case ARM64_SVEPREDPAT_MUL4: return lanes - lanes % 4;
		case ARM64_SVEPREDPAT_MUL3: return lanes - lanes % 3;
		case ARM64_SVEPREDPAT_ALL: return lanes;
		default: return 0;
		}
	}
	return lanes;
}

} // anonymous namespace

/**
 * Second SVE/SVE2 batch: vector-length arithmetic, CPY/FCPY/FDUP/DUPM,
 * MOVPRFX, predicate counts and saturating inc/dec, LASTA/LASTB, INSR,
 * predicate partitioning (PUNPK, PFIRST, PNEXT, PTEST, PTRUES, BRK*),
 * WHILEHI/HS/RW/WR, CTERM, ADCL/SBCL, dot products and matrix multiplies
 * (NEON and SVE), clamps, MAD/MSB, quadword reductions, bit permutes,
 * HISTCNT/HISTSEG and MATCH/NMATCH.
 */
void Capstone2LlvmIrTranslatorArm64_impl::translateSve2b(cs_insn* i, cs_arm64* ai, llvm::IRBuilder<>& irb)
{
	auto fallback = [&]() { translatePseudoAsmGeneric(i, ai, irb); };
	auto& o = ai->operands;
	unsigned n = ai->op_count;
	unsigned id = i->id;
	auto* i1 = irb.getInt1Ty();
	auto vecOf = [&](uint32_t r, unsigned eb) -> llvm::Value* {
		auto* ty = llvm::FixedVectorType::get(irb.getIntNTy(eb), 128 / eb);
		return irb.CreateBitCast(loadRegister(r, irb), ty);
	};
	auto isV = [&](const cs_arm64_op& op) {
		return op.type == ARM64_OP_REG && ((op.reg >= ARM64_REG_Q0 && op.reg <= ARM64_REG_Q31)
				|| (op.reg >= ARM64_REG_D0 && op.reg <= ARM64_REG_D31));
	};
	// Lanes of a Z or NEON vector operand.
	auto opLanes = [&](const cs_arm64_op& op, unsigned eb) -> unsigned {
		unsigned l = (static_cast<unsigned>(op.vas) >> 8) & 0xff;
		return (sve2IsZ(op) || l == 0) ? 128 / eb : l;
	};
	auto loadVec = [&](const cs_arm64_op& op, unsigned eb) -> llvm::Value* {
		if (sve2IsZ(op)) return vecOf(op.reg, eb);
		return loadNeonVector(op.reg, eb, 128 / eb, irb);
	};
	auto storeVec = [&](const cs_arm64_op& op, llvm::Value* v, unsigned eb, unsigned lanes) {
		if (sve2IsZ(op))
		{
			storeSveZ(op.reg, v, irb);
			return;
		}
		auto* bits = irb.CreateBitCast(v, irb.getIntNTy(128));
		if (lanes * eb == 64)
		{
			// 64-bit arrangement: upper half is zeroed.
			bits = irb.CreateAnd(bits, llvm::ConstantInt::get(irb.getIntNTy(128), llvm::APInt::getLowBitsSet(128, 64)));
		}
		uint32_t q = o[0].reg;
		if (q >= ARM64_REG_D0 && q <= ARM64_REG_D31) q = ARM64_REG_Q0 + (q - ARM64_REG_D0);
		storeRegister(q, bits, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	};
	auto predLanes = [&](llvm::Value* p, unsigned eb) { return svePredToLaneMask(p, eb, irb); };
	auto lanesToPred = [&](llvm::Value* m, unsigned eb) { return sveIcmpToPred(m, eb, irb); };
	auto allTrue = [&]() -> llvm::Value* {
		return llvm::Constant::getAllOnesValue(llvm::FixedVectorType::get(i1, 16));
	};
	auto mkMask = [&](unsigned lanes) -> llvm::Value* {
		return llvm::Constant::getNullValue(llvm::FixedVectorType::get(i1, lanes));
	};
	auto opEb = [&](const cs_arm64_op& op) -> unsigned {
		unsigned e = sve2ElemBits(op);
		return e ? e : 8;
	};
	std::string ops = i->op_str;
	bool zeroing = ops.find("/z") != std::string::npos;

	switch (id)
	{
	// ---- Vector-length arithmetic (VL = SVL = 128 bits).
	case ARM64_INS_ADDVL:
	case ARM64_INS_ADDPL:
	case ARM64_INS_ADDSVL:
	case ARM64_INS_ADDSPL:
	{
		if (n != 3 || o[2].type != ARM64_OP_IMM) break;
		int64_t unit = (id == ARM64_INS_ADDVL || id == ARM64_INS_ADDSVL) ? 16 : 2;
		auto* v = loadRegister(o[1].reg, irb);
		storeRegister(o[0].reg, irb.CreateAdd(v, llvm::ConstantInt::get(v->getType(), o[2].imm * unit, true)), irb);
		return;
	}
	case ARM64_INS_RDVL:
	case ARM64_INS_RDSVL:
	{
		if (n != 2 || o[1].type != ARM64_OP_IMM) break;
		storeRegister(o[0].reg, irb.getInt64(uint64_t(o[1].imm * 16)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	// ---- CPY / FCPY / FDUP / DUPM / MOVPRFX.
	case ARM64_INS_CPY:
	case ARM64_INS_FCPY:
	case ARM64_INS_FDUP:
	case ARM64_INS_DUPM:
	case ARM64_INS_MOVPRFX:
	{
		if (n < 2 || !sve2IsZ(o[0])) break;
		unsigned eb = sve2ElemBits(o[0]);
		if (eb == 0) eb = 64; // unsized movprfx copies bytes
		unsigned lanes = 128 / eb;
		auto* et = irb.getIntNTy(eb);
		bool pred = sve2IsP(o[1]);
		const cs_arm64_op& src = o[pred ? 2 : 1];
		if (pred && n < 3) break;
		llvm::Value* scalar = nullptr;
		llvm::Value* srcVec = nullptr;
		if (src.type == ARM64_OP_IMM)
		{
			int64_t v = src.imm;
			if (src.shift.type == ARM64_SFT_LSL) v <<= src.shift.value;
			scalar = llvm::ConstantInt::get(et, uint64_t(v), true);
		}
		else if (src.type == ARM64_OP_FP)
		{
			double d = src.fp;
			if (eb == 64) scalar = irb.CreateBitCast(llvm::ConstantFP::get(irb.getDoubleTy(), d), et);
			else if (eb == 32) scalar = irb.CreateBitCast(llvm::ConstantFP::get(irb.getFloatTy(), d), et);
			else
			{
				llvm::APFloat f(d);
				bool lost = false;
				f.convert(llvm::APFloat::IEEEhalf(), llvm::APFloat::rmNearestTiesToEven, &lost);
				scalar = llvm::ConstantInt::get(et, f.bitcastToAPInt());
			}
		}
		else if (sve2IsZ(src))
		{
			srcVec = vecOf(src.reg, eb);
		}
		else if (src.type == ARM64_OP_REG)
		{
			llvm::Value* v = loadRegister(src.reg, irb);
			if (!v->getType()->isIntegerTy()) v = irb.CreateBitCast(v, irb.getIntNTy(v->getType()->getPrimitiveSizeInBits()));
			scalar = irb.CreateZExtOrTrunc(v, et);
		}
		else break;
		llvm::Value* val = srcVec ? srcVec : irb.CreateVectorSplat(lanes, scalar);
		if (pred)
		{
			auto* m = predLanes(loadRegister(o[1].reg, irb), eb);
			llvm::Value* other = zeroing ? llvm::Constant::getNullValue(val->getType()) : vecOf(o[0].reg, eb);
			val = irb.CreateSelect(m, val, other);
		}
		storeSveZ(o[0].reg, val, irb);
		return;
	}

	// ---- INCP / DECP and saturating predicate counts.
	case ARM64_INS_INCP:
	case ARM64_INS_DECP:
	case ARM64_INS_SQINCP:
	case ARM64_INS_SQDECP:
	case ARM64_INS_UQINCP:
	case ARM64_INS_UQDECP:
	{
		if (n < 2 || !sve2IsP(o[1])) break;
		unsigned peb = opEb(o[1]);
		auto* m = predLanes(loadRegister(o[1].reg, irb), peb);
		llvm::Value* cnt = irb.getInt64(0);
		for (unsigned k = 0; k < 128 / peb; ++k)
			cnt = irb.CreateAdd(cnt, irb.CreateZExt(irb.CreateExtractElement(m, k), irb.getInt64Ty()));
		bool dec = id == ARM64_INS_DECP || id == ARM64_INS_SQDECP || id == ARM64_INS_UQDECP;
		bool sat = id != ARM64_INS_INCP && id != ARM64_INS_DECP;
		bool sgn = id == ARM64_INS_SQINCP || id == ARM64_INS_SQDECP;
		return sve2IncDecBy(i, ai, cnt, dec, sat, sgn, irb);
	}
	case ARM64_INS_SQINCB: case ARM64_INS_SQINCH: case ARM64_INS_SQINCW: case ARM64_INS_SQINCD:
	case ARM64_INS_SQDECB: case ARM64_INS_SQDECH: case ARM64_INS_SQDECW: case ARM64_INS_SQDECD:
	case ARM64_INS_UQINCB: case ARM64_INS_UQINCH: case ARM64_INS_UQINCW: case ARM64_INS_UQINCD:
	case ARM64_INS_UQDECB: case ARM64_INS_UQDECH: case ARM64_INS_UQDECW: case ARM64_INS_UQDECD:
	{
		std::string m = i->mnemonic;
		char sz = m.empty() ? 'b' : m.back();
		unsigned eb = sz == 'h' ? 16 : sz == 'w' ? 32 : sz == 'd' ? 64 : 8;
		uint64_t mul = 1;
		for (unsigned j = 1; j < n; ++j)
			if (o[j].type == ARM64_OP_IMM && o[j].imm > 0) mul = uint64_t(o[j].imm);
		uint64_t c = uint64_t(sve2PatternCount(ai, 128 / eb)) * mul;
		bool dec = m.find("dec") != std::string::npos;
		bool sgn = m[0] == 's';
		return sve2IncDecBy(i, ai, irb.getInt64(c), dec, true, sgn, irb);
	}

	// ---- LASTA / LASTB.
	case ARM64_INS_LASTA:
	case ARM64_INS_LASTB:
	{
		if (n != 3 || !sve2IsP(o[1]) || !sve2IsZ(o[2])) break;
		unsigned eb = opEb(o[2]);
		unsigned lanes = 128 / eb;
		auto* m = predLanes(loadRegister(o[1].reg, irb), eb);
		auto* v = vecOf(o[2].reg, eb);
		// Index of the last active element, or -1.
		llvm::Value* last = irb.getInt32(-1);
		for (unsigned k = 0; k < lanes; ++k)
			last = irb.CreateSelect(irb.CreateExtractElement(m, k), irb.getInt32(k), last);
		llvm::Value* idx;
		if (id == ARM64_INS_LASTA)
		{
			auto* nx = irb.CreateAdd(last, irb.getInt32(1));
			idx = irb.CreateSelect(irb.CreateICmpEQ(nx, irb.getInt32(lanes)), irb.getInt32(0), nx);
		}
		else
		{
			idx = irb.CreateSelect(irb.CreateICmpSLT(last, irb.getInt32(0)), irb.getInt32(lanes - 1), last);
		}
		auto* e = irb.CreateExtractElement(v, idx);
		storeRegister(o[0].reg, e, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}
	case ARM64_INS_INSR:
	{
		if (n != 2 || !sve2IsZ(o[0])) break;
		unsigned eb = opEb(o[0]);
		unsigned lanes = 128 / eb;
		auto* v = vecOf(o[0].reg, eb);
		llvm::Value* x = loadRegister(o[1].reg, irb);
		if (!x->getType()->isIntegerTy()) x = irb.CreateBitCast(x, irb.getIntNTy(x->getType()->getPrimitiveSizeInBits()));
		x = irb.CreateZExtOrTrunc(x, irb.getIntNTy(eb));
		llvm::Value* r = v;
		for (unsigned k = lanes - 1; k > 0; --k)
			r = irb.CreateInsertElement(r, irb.CreateExtractElement(v, k - 1), k);
		r = irb.CreateInsertElement(r, x, uint64_t(0));
		storeSveZ(o[0].reg, r, irb);
		return;
	}

	// ---- Predicate partitioning.
	case ARM64_INS_PUNPKHI:
	case ARM64_INS_PUNPKLO:
	{
		if (n != 2 || !sve2IsP(o[0]) || !sve2IsP(o[1])) break;
		auto* p = loadRegister(o[1].reg, irb);
		llvm::Value* r = mkMask(16);
		unsigned base = id == ARM64_INS_PUNPKHI ? 8 : 0;
		for (unsigned k = 0; k < 8; ++k)
			r = irb.CreateInsertElement(r, irb.CreateExtractElement(p, base + k), 2 * k);
		storeRegister(o[0].reg, r, irb);
		return;
	}
	case ARM64_INS_PTEST:
	{
		if (n != 2 || !sve2IsP(o[0]) || !sve2IsP(o[1])) break;
		sve2SetPredFlags(loadRegister(o[0].reg, irb), loadRegister(o[1].reg, irb), irb);
		return;
	}
	case ARM64_INS_PTRUES:
	{
		translateSvePtrue(i, ai, irb);
		sve2SetPredFlags(allTrue(), loadRegister(o[0].reg, irb), irb);
		return;
	}
	case ARM64_INS_PFIRST:
	case ARM64_INS_PNEXT:
	{
		if (n != 3 || !sve2IsP(o[0]) || !sve2IsP(o[1]) || !sve2IsP(o[2])) break;
		unsigned eb = id == ARM64_INS_PFIRST ? 8 : opEb(o[0]);
		unsigned lanes = 128 / eb;
		auto* g = loadRegister(o[1].reg, irb);
		auto* gm = predLanes(g, eb);
		auto* dn = loadRegister(o[2].reg, irb);
		auto* dm = predLanes(dn, eb);
		if (id == ARM64_INS_PFIRST)
		{
			llvm::Value* seen = irb.getFalse();
			llvm::Value* r = dm;
			for (unsigned k = 0; k < lanes; ++k)
			{
				auto* gk = irb.CreateExtractElement(gm, k);
				auto* first = irb.CreateAnd(gk, irb.CreateNot(seen));
				r = irb.CreateInsertElement(r, irb.CreateOr(first, irb.CreateExtractElement(dm, k)), k);
				seen = irb.CreateOr(seen, gk);
			}
			auto* rp = lanesToPred(r, eb);
			storeRegister(o[0].reg, rp, irb);
			sve2SetPredFlags(g, rp, irb);
			return;
		}
		// PNEXT: next active element after the last true one in pdn.
		llvm::Value* last = irb.getInt32(-1);
		for (unsigned k = 0; k < lanes; ++k)
			last = irb.CreateSelect(irb.CreateAnd(irb.CreateExtractElement(dm, k), irb.CreateExtractElement(gm, k)),
					irb.getInt32(k), last);
		llvm::Value* found = irb.getFalse();
		llvm::Value* r = mkMask(lanes);
		for (unsigned k = 0; k < lanes; ++k)
		{
			auto* hit = irb.CreateAnd(irb.CreateAnd(irb.CreateExtractElement(gm, k),
					irb.CreateICmpSGT(irb.getInt32(k), last)), irb.CreateNot(found));
			r = irb.CreateInsertElement(r, hit, k);
			found = irb.CreateOr(found, hit);
		}
		auto* rp = lanesToPred(r, eb);
		storeRegister(o[0].reg, rp, irb);
		sve2SetPredFlags(g, rp, irb);
		return;
	}
	case ARM64_INS_BRKA: case ARM64_INS_BRKAS: case ARM64_INS_BRKB: case ARM64_INS_BRKBS:
	case ARM64_INS_BRKPA: case ARM64_INS_BRKPAS: case ARM64_INS_BRKPB: case ARM64_INS_BRKPBS:
	case ARM64_INS_BRKN: case ARM64_INS_BRKNS:
	{
		if (n < 3 || !sve2IsP(o[0]) || !sve2IsP(o[1]) || !sve2IsP(o[2])) break;
		bool flags = id == ARM64_INS_BRKAS || id == ARM64_INS_BRKBS || id == ARM64_INS_BRKPAS
				|| id == ARM64_INS_BRKPBS || id == ARM64_INS_BRKNS;
		auto* g = loadRegister(o[1].reg, irb);
		auto* pn = loadRegister(o[2].reg, irb);
		// Last active element of pn under g.
		llvm::Value* lastTrue = irb.getFalse();
		for (unsigned k = 0; k < 16; ++k)
			lastTrue = irb.CreateSelect(irb.CreateExtractElement(g, k), irb.CreateExtractElement(pn, k), lastTrue);
		llvm::Value* r = nullptr;
		if (id == ARM64_INS_BRKN || id == ARM64_INS_BRKNS)
		{
			if (n != 4) break;
			auto* pdm = loadRegister(o[3].reg, irb);
			r = irb.CreateSelect(lastTrue, pdm, llvm::Constant::getNullValue(pdm->getType()));
			storeRegister(o[0].reg, r, irb);
			if (flags) sve2SetPredFlags(allTrue(), r, irb);
			return;
		}
		bool after = id == ARM64_INS_BRKA || id == ARM64_INS_BRKAS || id == ARM64_INS_BRKPA || id == ARM64_INS_BRKPAS;
		bool prop = id == ARM64_INS_BRKPA || id == ARM64_INS_BRKPAS || id == ARM64_INS_BRKPB || id == ARM64_INS_BRKPBS;
		auto* src = prop ? (n == 4 ? loadRegister(o[3].reg, irb) : nullptr) : pn;
		if (!src) break;
		llvm::Value* brk = irb.getFalse();
		auto* old = loadRegister(o[0].reg, irb);
		r = mkMask(16);
		for (unsigned k = 0; k < 16; ++k)
		{
			auto* gk = irb.CreateExtractElement(g, k);
			auto* sk = irb.CreateAnd(gk, irb.CreateExtractElement(src, k));
			llvm::Value* e;
			if (after)
			{
				e = irb.CreateAnd(gk, irb.CreateNot(brk));
				brk = irb.CreateOr(brk, sk);
			}
			else
			{
				brk = irb.CreateOr(brk, sk);
				e = irb.CreateAnd(gk, irb.CreateNot(brk));
			}
			if (prop) e = irb.CreateAnd(e, lastTrue);
			llvm::Value* inact = (zeroing || prop) ? static_cast<llvm::Value*>(irb.getFalse()) : irb.CreateExtractElement(old, k);
			r = irb.CreateInsertElement(r, irb.CreateSelect(gk, e, inact), k);
		}
		storeRegister(o[0].reg, r, irb);
		if (flags) sve2SetPredFlags(g, r, irb);
		return;
	}

	// ---- WHILEHI/HS (descending) and WHILERW/WR (address conflicts).
	case ARM64_INS_WHILEHI:
	case ARM64_INS_WHILEHS:
	case ARM64_INS_WHILERW:
	case ARM64_INS_WHILEWR:
	{
		if (n != 3 || !sve2IsP(o[0])) break;
		unsigned eb = opEb(o[0]);
		unsigned lanes = 128 / eb;
		auto* a = loadRegister(o[1].reg, irb);
		auto* b = loadRegister(o[2].reg, irb);
		auto* ty = a->getType();
		llvm::Value* r = mkMask(lanes);
		if (id == ARM64_INS_WHILEHI || id == ARM64_INS_WHILEHS)
		{
			llvm::Value* on = irb.getTrue();
			llvm::Value* cur = a;
			for (int k = int(lanes) - 1; k >= 0; --k)
			{
				auto* c = id == ARM64_INS_WHILEHI ? irb.CreateICmpUGT(cur, b) : irb.CreateICmpUGE(cur, b);
				on = irb.CreateAnd(on, c);
				r = irb.CreateInsertElement(r, on, k);
				cur = irb.CreateSub(cur, llvm::ConstantInt::get(ty, 1));
			}
		}
		else
		{
			auto* diff = irb.CreateSub(b, a);
			llvm::Value* el;
			llvm::Value* all;
			if (id == ARM64_INS_WHILERW)
			{
				auto* ad = irb.CreateSelect(irb.CreateICmpSLT(diff, llvm::ConstantInt::get(ty, 0)), irb.CreateNeg(diff), diff);
				el = irb.CreateSDiv(ad, llvm::ConstantInt::get(ty, eb / 8));
				all = irb.CreateICmpEQ(el, llvm::ConstantInt::get(ty, 0));
			}
			else
			{
				el = irb.CreateSDiv(diff, llvm::ConstantInt::get(ty, eb / 8));
				all = irb.CreateICmpSLE(el, llvm::ConstantInt::get(ty, 0));
			}
			for (unsigned k = 0; k < lanes; ++k)
				r = irb.CreateInsertElement(r, irb.CreateOr(all, irb.CreateICmpSGT(el, llvm::ConstantInt::get(ty, k))), k);
		}
		auto* rp = lanesToPred(r, eb);
		storeRegister(o[0].reg, rp, irb);
		sve2SetPredFlags(allTrue(), rp, irb);
		return;
	}
	case ARM64_INS_CTERMEQ:
	case ARM64_INS_CTERMNE:
	{
		if (n != 2) break;
		auto* a = loadRegister(o[0].reg, irb);
		auto* b = loadRegister(o[1].reg, irb);
		auto* t = id == ARM64_INS_CTERMEQ ? irb.CreateICmpEQ(a, b) : irb.CreateICmpNE(a, b);
		auto* c = loadRegister(ARM64_REG_CPSR_C, irb);
		c = irb.CreateICmpNE(c, llvm::ConstantInt::get(c->getType(), 0));
		storeRegister(ARM64_REG_CPSR_N, t, irb);
		storeRegister(ARM64_REG_CPSR_V, irb.CreateAnd(irb.CreateNot(t), irb.CreateNot(c)), irb);
		return;
	}

	// ---- ADCLB/ADCLT/SBCLB/SBCLT.
	case ARM64_INS_ADCLB: case ARM64_INS_ADCLT: case ARM64_INS_SBCLB: case ARM64_INS_SBCLT:
	{
		if (n != 3 || !sve2IsZ(o[0]) || !sve2IsZ(o[1]) || !sve2IsZ(o[2])) break;
		unsigned eb = opEb(o[0]);
		unsigned lanes = 128 / eb;
		auto* w = irb.getIntNTy(eb * 2);
		auto* et = irb.getIntNTy(eb);
		auto* d = vecOf(o[0].reg, eb);
		auto* a = vecOf(o[1].reg, eb);
		auto* m = vecOf(o[2].reg, eb);
		bool top = id == ARM64_INS_ADCLT || id == ARM64_INS_SBCLT;
		bool sbc = id == ARM64_INS_SBCLB || id == ARM64_INS_SBCLT;
		llvm::Value* r = d;
		for (unsigned k = 0; k < lanes; k += 2)
		{
			llvm::Value* x = irb.CreateExtractElement(a, k + (top ? 1 : 0));
			if (sbc) x = irb.CreateNot(x);
			auto* cin = irb.CreateAnd(irb.CreateZExt(irb.CreateExtractElement(m, k + 1), w), llvm::ConstantInt::get(w, 1));
			auto* sum = irb.CreateAdd(irb.CreateAdd(irb.CreateZExt(irb.CreateExtractElement(d, k), w), irb.CreateZExt(x, w)), cin);
			r = irb.CreateInsertElement(r, irb.CreateTrunc(sum, et), k);
			r = irb.CreateInsertElement(r, irb.CreateTrunc(irb.CreateLShr(sum, llvm::ConstantInt::get(w, eb)), et), k + 1);
		}
		storeSveZ(o[0].reg, r, irb);
		return;
	}

	// ---- Dot products and 8-bit matrix multiplies (NEON and SVE).
	case ARM64_INS_SDOT: case ARM64_INS_UDOT: case ARM64_INS_USDOT: case ARM64_INS_SUDOT:
	{
		if (n != 3 || o[0].type != ARM64_OP_REG || o[1].type != ARM64_OP_REG || o[2].type != ARM64_OP_REG) break;
		bool z = sve2IsZ(o[0]);
		if (!z && !isV(o[0])) break;
		unsigned deb = sve2ElemBits(o[0]);
		unsigned neb = sve2ElemBits(o[1]);
		if (deb == 0 || neb == 0 || deb <= neb) break;
		unsigned ratio = deb / neb;
		unsigned lanes = opLanes(o[0], deb);
		bool sa = id == ARM64_INS_SDOT || id == ARM64_INS_SUDOT;
		bool sb = id == ARM64_INS_SDOT || id == ARM64_INS_USDOT;
		auto* wt = irb.getIntNTy(64);
		auto* d = loadVec(o[0], deb);
		auto* a = loadVec(o[1], neb);
		auto* b = loadVec(o[2], neb);
		int idx = o[2].vector_index;
		llvm::Value* r = d;
		for (unsigned k = 0; k < lanes; ++k)
		{
			llvm::Value* acc = irb.CreateSExt(irb.CreateExtractElement(d, k), wt);
			unsigned seg = (k * deb) / 128 * (128 / deb); // first lane of this 128-bit segment
			for (unsigned j = 0; j < ratio; ++j)
			{
				unsigned bl = idx >= 0 ? (seg + unsigned(idx)) * ratio + j : k * ratio + j;
				auto* x = irb.CreateExtractElement(a, k * ratio + j);
				auto* y = irb.CreateExtractElement(b, bl);
				acc = irb.CreateAdd(acc, irb.CreateMul(sa ? irb.CreateSExt(x, wt) : irb.CreateZExt(x, wt),
						sb ? irb.CreateSExt(y, wt) : irb.CreateZExt(y, wt)));
			}
			r = irb.CreateInsertElement(r, irb.CreateTrunc(acc, irb.getIntNTy(deb)), k);
		}
		storeVec(o[0], r, deb, lanes);
		return;
	}
	case ARM64_INS_SMMLA: case ARM64_INS_UMMLA: case ARM64_INS_USMMLA:
	{
		if (n != 3 || o[0].type != ARM64_OP_REG) break;
		bool z = sve2IsZ(o[0]);
		if (!z && !isV(o[0])) break;
		bool sa = id == ARM64_INS_SMMLA;
		bool sb = id == ARM64_INS_SMMLA || id == ARM64_INS_USMMLA;
		auto* d = loadVec(o[0], 32);
		auto* a = loadVec(o[1], 8);
		auto* b = loadVec(o[2], 8);
		llvm::Value* r = d;
		for (unsigned row = 0; row < 2; ++row)
			for (unsigned col = 0; col < 2; ++col)
			{
				llvm::Value* acc = irb.CreateExtractElement(d, row * 2 + col);
				for (unsigned k = 0; k < 8; ++k)
				{
					auto* x = irb.CreateExtractElement(a, row * 8 + k);
					auto* y = irb.CreateExtractElement(b, col * 8 + k);
					acc = irb.CreateAdd(acc, irb.CreateMul(sa ? irb.CreateSExt(x, irb.getInt32Ty()) : irb.CreateZExt(x, irb.getInt32Ty()),
							sb ? irb.CreateSExt(y, irb.getInt32Ty()) : irb.CreateZExt(y, irb.getInt32Ty())));
				}
				r = irb.CreateInsertElement(r, acc, row * 2 + col);
			}
		storeVec(o[0], r, 32, 4);
		return;
	}

	// ---- Clamps and multiply-add with the addend in the third source.
	case ARM64_INS_SCLAMP:
	case ARM64_INS_UCLAMP:
	{
		if (n != 3 || !sve2IsZ(o[0]) || !sve2IsZ(o[1]) || !sve2IsZ(o[2])) break;
		unsigned eb = opEb(o[0]);
		auto* d = vecOf(o[0].reg, eb);
		auto* lo = vecOf(o[1].reg, eb);
		auto* hi = vecOf(o[2].reg, eb);
		bool s = id == ARM64_INS_SCLAMP;
		auto* t = irb.CreateSelect(s ? irb.CreateICmpSLT(d, lo) : irb.CreateICmpULT(d, lo), lo, d);
		t = irb.CreateSelect(s ? irb.CreateICmpSGT(t, hi) : irb.CreateICmpUGT(t, hi), hi, t);
		storeSveZ(o[0].reg, t, irb);
		return;
	}
	case ARM64_INS_MAD:
	case ARM64_INS_MSB:
	{
		if (n != 4 || !sve2IsZ(o[0]) || !sve2IsP(o[1]) || !sve2IsZ(o[2]) || !sve2IsZ(o[3])) break;
		unsigned eb = opEb(o[0]);
		auto* d = vecOf(o[0].reg, eb);
		auto* m = vecOf(o[2].reg, eb);
		auto* aa = vecOf(o[3].reg, eb);
		auto* prod = irb.CreateMul(d, m);
		auto* res = id == ARM64_INS_MAD ? irb.CreateAdd(aa, prod) : irb.CreateSub(aa, prod);
		auto* mask = predLanes(loadRegister(o[1].reg, irb), eb);
		storeSveZ(o[0].reg, irb.CreateSelect(mask, res, d), irb);
		return;
	}

	// ---- Quadword reductions: one 128-bit segment at VL=128, so each
	// active lane passes through and inactive lanes take the identity.
	case ARM64_INS_ADDQV: case ARM64_INS_ANDQV: case ARM64_INS_EORQV: case ARM64_INS_ORQV:
	case ARM64_INS_SMAXQV: case ARM64_INS_SMINQV: case ARM64_INS_UMAXQV: case ARM64_INS_UMINQV:
	{
		if (n != 3 || !sve2IsP(o[1]) || !sve2IsZ(o[2])) break;
		unsigned eb = opEb(o[2]);
		auto* v = vecOf(o[2].reg, eb);
		auto* et = irb.getIntNTy(eb);
		llvm::APInt idv(eb, 0);
		if (id == ARM64_INS_ANDQV || id == ARM64_INS_UMINQV) idv = llvm::APInt::getAllOnes(eb);
		if (id == ARM64_INS_SMAXQV) idv = llvm::APInt::getSignedMinValue(eb);
		if (id == ARM64_INS_SMINQV) idv = llvm::APInt::getSignedMaxValue(eb);
		auto* idn = irb.CreateVectorSplat(128 / eb, llvm::ConstantInt::get(et, idv));
		auto* mask = predLanes(loadRegister(o[1].reg, irb), eb);
		auto* r = irb.CreateSelect(mask, v, idn);
		storeRegister(o[0].reg, irb.CreateBitCast(r, irb.getIntNTy(128)), irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
		return;
	}

	// ---- BDEP / BEXT / BGRP.
	case ARM64_INS_BDEP: case ARM64_INS_BEXT: case ARM64_INS_BGRP:
	{
		if (n != 3 || !sve2IsZ(o[0]) || !sve2IsZ(o[1]) || !sve2IsZ(o[2])) break;
		unsigned eb = opEb(o[0]);
		unsigned lanes = 128 / eb;
		auto* et = irb.getIntNTy(eb);
		auto* a = vecOf(o[1].reg, eb);
		auto* m = vecOf(o[2].reg, eb);
		llvm::Value* r = a;
		auto c = [&](uint64_t v) { return llvm::ConstantInt::get(et, v); };
		for (unsigned k = 0; k < lanes; ++k)
		{
			auto* x = irb.CreateExtractElement(a, k);
			auto* mk = irb.CreateExtractElement(m, k);
			llvm::Value* res = c(0);
			llvm::Value* pos = c(0);  // next result bit (low group)
			llvm::Value* pos2 = c(0); // next result bit (high group, BGRP)
			llvm::Value* srcPos = c(0); // BDEP: next source bit
			for (unsigned b = 0; b < eb; ++b)
			{
				auto* mb = irb.CreateTrunc(irb.CreateLShr(mk, c(b)), irb.getInt1Ty());
				if (id == ARM64_INS_BDEP)
				{
					auto* sb = irb.CreateAnd(irb.CreateLShr(x, srcPos), c(1));
					res = irb.CreateSelect(mb, irb.CreateOr(res, irb.CreateShl(sb, c(b))), res);
					srcPos = irb.CreateSelect(mb, irb.CreateAdd(srcPos, c(1)), srcPos);
				}
				else
				{
					auto* xb = irb.CreateAnd(irb.CreateLShr(x, c(b)), c(1));
					res = irb.CreateSelect(mb, irb.CreateOr(res, irb.CreateShl(xb, pos)), res);
					pos = irb.CreateSelect(mb, irb.CreateAdd(pos, c(1)), pos);
					(void) pos2;
				}
			}
			if (id == ARM64_INS_BGRP)
			{
				// Unselected bits go above the selected ones, in order.
				llvm::Value* hi = c(0);
				llvm::Value* hp = c(0);
				for (unsigned b = 0; b < eb; ++b)
				{
					auto* mb = irb.CreateTrunc(irb.CreateLShr(mk, c(b)), irb.getInt1Ty());
					auto* xb = irb.CreateAnd(irb.CreateLShr(x, c(b)), c(1));
					hi = irb.CreateSelect(mb, hi, irb.CreateOr(hi, irb.CreateShl(xb, hp)));
					hp = irb.CreateSelect(mb, hp, irb.CreateAdd(hp, c(1)));
				}
				auto* full = irb.CreateICmpEQ(pos, c(eb));
				auto* shifted = irb.CreateShl(hi, irb.CreateSelect(full, c(0), pos));
				res = irb.CreateSelect(full, res, irb.CreateOr(res, shifted));
			}
			r = irb.CreateInsertElement(r, res, k);
		}
		storeSveZ(o[0].reg, r, irb);
		return;
	}

	// ---- HISTCNT / HISTSEG / MATCH / NMATCH.
	case ARM64_INS_HISTCNT:
	{
		if (n != 4 || !sve2IsZ(o[0]) || !sve2IsP(o[1])) break;
		unsigned eb = opEb(o[0]);
		unsigned lanes = 128 / eb;
		auto* et = irb.getIntNTy(eb);
		auto* mask = predLanes(loadRegister(o[1].reg, irb), eb);
		auto* a = vecOf(o[2].reg, eb);
		auto* b = vecOf(o[3].reg, eb);
		llvm::Value* r = llvm::Constant::getNullValue(a->getType());
		for (unsigned k = 0; k < lanes; ++k)
		{
			llvm::Value* cnt = llvm::ConstantInt::get(et, 0);
			auto* ak = irb.CreateExtractElement(a, k);
			for (unsigned j = 0; j <= k; ++j)
			{
				auto* hit = irb.CreateAnd(irb.CreateExtractElement(mask, j), irb.CreateICmpEQ(irb.CreateExtractElement(b, j), ak));
				cnt = irb.CreateAdd(cnt, irb.CreateZExt(hit, et));
			}
			r = irb.CreateInsertElement(r, irb.CreateSelect(irb.CreateExtractElement(mask, k), cnt, llvm::ConstantInt::get(et, 0)), k);
		}
		storeSveZ(o[0].reg, r, irb);
		return;
	}
	case ARM64_INS_HISTSEG:
	{
		if (n != 3 || !sve2IsZ(o[0])) break;
		auto* a = vecOf(o[1].reg, 8);
		auto* b = vecOf(o[2].reg, 8);
		llvm::Value* r = a;
		for (unsigned k = 0; k < 16; ++k)
		{
			llvm::Value* cnt = irb.getInt8(0);
			auto* ak = irb.CreateExtractElement(a, k);
			for (unsigned j = 0; j < 16; ++j)
				cnt = irb.CreateAdd(cnt, irb.CreateZExt(irb.CreateICmpEQ(irb.CreateExtractElement(b, j), ak), irb.getInt8Ty()));
			r = irb.CreateInsertElement(r, cnt, k);
		}
		storeSveZ(o[0].reg, r, irb);
		return;
	}
	case ARM64_INS_MATCH:
	case ARM64_INS_NMATCH:
	{
		if (n != 4 || !sve2IsP(o[0]) || !sve2IsP(o[1]) || !sve2IsZ(o[2]) || !sve2IsZ(o[3])) break;
		unsigned eb = opEb(o[2]);
		unsigned lanes = 128 / eb;
		auto* g = loadRegister(o[1].reg, irb);
		auto* mask = predLanes(g, eb);
		auto* a = vecOf(o[2].reg, eb);
		auto* b = vecOf(o[3].reg, eb);
		llvm::Value* r = mkMask(lanes);
		for (unsigned k = 0; k < lanes; ++k)
		{
			llvm::Value* any = irb.getFalse();
			auto* ak = irb.CreateExtractElement(a, k);
			for (unsigned j = 0; j < lanes; ++j)
				any = irb.CreateOr(any, irb.CreateICmpEQ(irb.CreateExtractElement(b, j), ak));
			if (id == ARM64_INS_NMATCH) any = irb.CreateNot(any);
			r = irb.CreateInsertElement(r, irb.CreateAnd(any, irb.CreateExtractElement(mask, k)), k);
		}
		auto* rp = lanesToPred(r, eb);
		storeRegister(o[0].reg, rp, irb);
		sve2SetPredFlags(g, rp, irb);
		return;
	}
	default:
		break;
	}
	fallback();
}

/**
 * Add or subtract \p cnt (i64) to the destination of an SVE inc/dec:
 * a Z register (per lane), an X register, or a W register extended to X
 * (the `xd, pg, wd` / `xd, wd, pattern` saturating forms).
 */
void Capstone2LlvmIrTranslatorArm64_impl::sve2IncDecBy(cs_insn* i, cs_arm64* ai, llvm::Value* cnt,
		bool dec, bool sat, bool sgn, llvm::IRBuilder<>& irb)
{
	auto& o = ai->operands;
	auto* w = irb.getIntNTy(130);
	auto clampTo = [&](llvm::Value* v, unsigned bits, bool s) -> llvm::Value* {
		llvm::APInt lo = s ? llvm::APInt::getSignedMinValue(bits).sext(130) : llvm::APInt(130, 0);
		llvm::APInt hi = s ? llvm::APInt::getSignedMaxValue(bits).sext(130) : llvm::APInt::getMaxValue(bits).zext(130);
		auto* l = llvm::ConstantInt::get(w, lo);
		auto* h = llvm::ConstantInt::get(w, hi);
		v = irb.CreateSelect(irb.CreateICmpSLT(v, l), l, v);
		return irb.CreateSelect(irb.CreateICmpSGT(v, h), h, v);
	};
	auto* c = irb.CreateZExt(cnt, w);
	if (sve2IsZ(o[0]))
	{
		unsigned eb = sve2ElemBits(o[0]);
		if (eb == 0) eb = 8;
		unsigned lanes = 128 / eb;
		auto* ty = llvm::FixedVectorType::get(irb.getIntNTy(eb), lanes);
		auto* v = irb.CreateBitCast(loadRegister(o[0].reg, irb), ty);
		llvm::Value* r = v;
		for (unsigned k = 0; k < lanes; ++k)
		{
			auto* e = irb.CreateExtractElement(v, k);
			auto* x = sgn ? irb.CreateSExt(e, w) : irb.CreateZExt(e, w);
			x = dec ? irb.CreateSub(x, c) : irb.CreateAdd(x, c);
			if (sat) x = clampTo(x, eb, sgn);
			r = irb.CreateInsertElement(r, irb.CreateTrunc(x, irb.getIntNTy(eb)), k);
		}
		storeSveZ(o[0].reg, r, irb);
		return;
	}
	// Scalar: 32-bit operation when a W register is named (as the
	// destination or as the separate 32-bit source of the X form).
	unsigned bits = 64;
	bool sextResult = false;
	for (unsigned j = 0; j < ai->op_count; ++j)
	{
		if (o[j].type == ARM64_OP_REG && o[j].reg >= ARM64_REG_W0 && o[j].reg <= ARM64_REG_W30)
		{
			bits = 32;
			sextResult = j > 0 && sgn;
		}
	}
	auto* d = loadRegister(o[0].reg, irb);
	auto* nb = irb.CreateTrunc(d, irb.getIntNTy(bits));
	auto* x = sgn ? irb.CreateSExt(nb, w) : irb.CreateZExt(nb, w);
	x = dec ? irb.CreateSub(x, c) : irb.CreateAdd(x, c);
	if (sat) x = clampTo(x, bits, sgn);
	auto* r = irb.CreateTrunc(x, irb.getIntNTy(bits));
	llvm::Value* out = sextResult ? irb.CreateSExt(r, irb.getInt64Ty()) : irb.CreateZExt(r, irb.getInt64Ty());
	storeRegister(o[0].reg, out, irb, eOpConv::ZEXT_TRUNC_OR_BITCAST);
	(void) i;
}

} // namespace capstone2llvmir
} // namespace retdec
