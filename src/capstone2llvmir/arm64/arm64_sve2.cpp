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

} // namespace capstone2llvmir
} // namespace retdec
