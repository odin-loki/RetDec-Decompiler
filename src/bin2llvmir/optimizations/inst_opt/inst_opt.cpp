#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt_ext.h"
/**
 * @file src/bin2llvmir/optimizations/inst_opt/inst_opt.cpp
 * @brief Optimize a single LLVM instruction.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdlib>

#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt.h"
#include "retdec/bin2llvmir/utils/debug.h"

using namespace llvm;
using namespace PatternMatch;

namespace retdec {
namespace bin2llvmir {
namespace inst_opt {

namespace {

bool instOptPatternTraceEnabled()
{
	const char* env = std::getenv("RETDEC_INST_OPT_PATTERN_TRACE");
	return env && *env && std::string(env) != "0";
}

bool instOptPatternTraceVerbose()
{
	const char* env = std::getenv("RETDEC_INST_OPT_PATTERN_TRACE");
	return env && env[0] == '2';
}

unsigned long instOptMaxPatterns()
{
	const char* env = std::getenv("RETDEC_INST_OPT_MAX_PATTERNS");
	if (env == nullptr || *env == '\0')
	{
		return 0; // no limit
	}
	char* end = nullptr;
	unsigned long v = std::strtoul(env, &end, 10);
	if (end == env)
	{
		return 0;
	}
	return v;
}

void traceInstOptPattern(const std::string& msg)
{
	if (!instOptPatternTraceEnabled())
	{
		return;
	}
	llvm::errs() << "[inst-opt-pattern] " << msg << "\n";
	llvm::errs().flush();
}

struct OptEntry
{
	const char* name;
	bool (*fn)(llvm::Instruction*);
};

} // anonymous namespace

/**
 * x = add y, 0
 *   =>
 * x = y
 *
 * x = add 0, y
 *   =>
 * x = y
 */
bool addZero(llvm::Instruction* insn)
{
	Value* val;
	uint64_t zero;

	if (!(match(insn, m_Add(m_Value(val), m_ConstantInt(zero)))
			|| match(insn, m_Add(m_ConstantInt(zero), m_Value(val)))))
	{
		return false;
	}
	if (zero != 0)
	{
		return false;
	}

	insn->replaceAllUsesWith(val);
	insn->eraseFromParent();
	return true;
}

/**
 * x = sub y, 0
 *   =>
 * x = use y
 */
bool subZero(llvm::Instruction* insn)
{
	uint64_t zero;

	if (!match(insn, m_Sub(m_Value(), m_ConstantInt(zero))))
	{
		return false;
	}
	if (zero != 0)
	{
		return false;
	}

	insn->replaceAllUsesWith(insn->getOperand(0));
	insn->eraseFromParent();
	return true;
}

/**
 * a = trunc i32 val to i8
 * b = zext i8 a to i32
 *   =>
 * b = and i32 val, 255
 *
 * a = trunc i32 val to i16
 * b = zext i16 a to i32
 *   =>
 * b = and i32 val, 65535
 */
bool truncZext(llvm::Instruction* insn)
{
	auto* zext = dyn_cast<ZExtInst>(insn);
	if (zext == nullptr)
	{
		return false;
	}
	auto* trunc = dyn_cast<TruncInst>(zext->getOperand(0));
	if (trunc == nullptr)
	{
		return false;
	}
	if (zext->getParent() == nullptr || trunc->getParent() == nullptr)
	{
		return false;
	}
	// Keep this fold strictly local to a single-use trunc->zext chain.
	// Broader rewrites in large lifted modules may trigger invalid IR.
	if (!trunc->hasOneUse() || trunc->getParent() != zext->getParent())
	{
		return false;
	}
	Value* val = trunc->getOperand(0);
	if (val == nullptr || !val->getType()->isIntegerTy(32))
	{
		return false;
	}
	Instruction* a = nullptr;

	if (trunc->getSrcTy()->isIntegerTy(32)
		&& trunc->getDestTy()->isIntegerTy(8)
		&& zext->getSrcTy()->isIntegerTy(8)
		&& zext->getDestTy()->isIntegerTy(32))
	{
		a = BinaryOperator::CreateAnd(
				val,
				ConstantInt::get(val->getType(), 255),
				"",
				zext);
	}
	else if (trunc->getSrcTy()->isIntegerTy(32)
			&& trunc->getDestTy()->isIntegerTy(16)
			&& zext->getSrcTy()->isIntegerTy(16)
			&& zext->getDestTy()->isIntegerTy(32))
	{
		a = BinaryOperator::CreateAnd(
				val,
				ConstantInt::get(val->getType(), 65535),
				"",
				zext);
	}
	if (a == nullptr)
	{
		return false;
	}

	a->takeName(zext);
	zext->replaceAllUsesWith(a);
	zext->eraseFromParent();
	if (trunc->user_empty())
	{
		trunc->eraseFromParent();
	}

	return true;
}

/**
 * a = xor x, x
 *   =>
 * a = 0
 */
bool xorXX(llvm::Instruction* insn)
{
	Value* op0;
	Value* op1;

	if (!(match(insn, m_Xor(m_Value(op0), m_Value(op1)))
			&& op0 == op1))
	{
		return false;
	}

	insn->replaceAllUsesWith(ConstantInt::get(insn->getType(), 0));
	insn->eraseFromParent();

	return true;
}

/**
 * a = load x
 * b = load x
 * c = xor a, b
 *   =>
 * c = 0
 */
bool xorLoadXX(llvm::Instruction* insn)
{
	Instruction* i1;
	Instruction* i2;

	if (!(match(insn, m_Xor(m_Instruction(i1), m_Instruction(i2)))
			&& isa<LoadInst>(i1)
			&& isa<LoadInst>(i2)))
	{
		return false;
	}
	LoadInst* l1 = cast<LoadInst>(i1);
	LoadInst* l2 = cast<LoadInst>(i2);
	if (l1->getPointerOperand() != l2->getPointerOperand())
	{
		return false;
	}

	insn->replaceAllUsesWith(ConstantInt::get(insn->getType(), 0));
	insn->eraseFromParent();
	if (l1->user_empty())
	{
		l1->eraseFromParent();
	}
	if (l2 != l1 && l2->user_empty())
	{
		l2->eraseFromParent();
	}

	return true;
}

/**
 * a = or x, x
 *   =>
 * a = x
 *
 * a = and x, x
 *   =>
 * a = x
 */
bool orAndXX(llvm::Instruction* insn)
{
	Value* op0;
	Value* op1;

	if (!(match(insn, m_Or(m_Value(op0), m_Value(op1)))
			|| match(insn, m_And(m_Value(op0), m_Value(op1)))))
	{
		return false;
	}
	if (op0 != op1)
	{
		return false;
	}

	insn->replaceAllUsesWith(op0);
	insn->eraseFromParent();

	return true;
}

/**
 * a = load x
 * b = load x
 * c = or a, b
 *   =>
 * c = 0
 *
 * a = load x
 * b = load x
 * c = and a, b
 *   =>
 * c = 0
 */
bool orAndLoadXX(llvm::Instruction* insn)
{
	Instruction* i1;
	Instruction* i2;

	if (!(match(insn, m_Or(m_Instruction(i1), m_Instruction(i2)))
			|| match(insn, m_And(m_Instruction(i1), m_Instruction(i2)))))
	{
		return false;
	}
	LoadInst* l1 = dyn_cast<LoadInst>(i1);
	LoadInst* l2 = dyn_cast<LoadInst>(i2);
	if (l1 == nullptr
			|| l2 == nullptr
			|| l1->getPointerOperand() != l2->getPointerOperand())
	{
		return false;
	}

	insn->replaceAllUsesWith(l1);
	insn->eraseFromParent();
	if (l2->user_empty())
	{
		l2->eraseFromParent();
	}

	return true;
}

/**
 * a = xor i1 x, y
 *   =>
 * a = icmp ne i1 x, y
 */
bool xor_i1(llvm::Instruction* insn)
{
	Value* op0;
	Value* op1;

	if (!(match(insn, m_Xor(m_Value(op0), m_Value(op1)))
			&& insn->getType()->isIntegerTy(1)))
	{
		return false;
	}

	auto* cmp = CmpInst::Create(
			Instruction::ICmp,
			ICmpInst::ICMP_NE,
			op0,
			op1,
			"",
			insn);
	cmp->takeName(insn);
	insn->replaceAllUsesWith(cmp);
	insn->eraseFromParent();

	return true;
}

/**
 * a = and i1 x, y
 *   =>
 * a = icmp eq i1 x, y
 */
bool and_i1(llvm::Instruction* insn)
{
	Value* op0;
	Value* op1;

	if (!(match(insn, m_And(m_Value(op0), m_Value(op1)))
			&& insn->getType()->isIntegerTy(1)))
	{
		return false;
	}

	auto* cmp = CmpInst::Create(
			Instruction::ICmp,
			ICmpInst::ICMP_EQ,
			op0,
			op1,
			"",
			insn);
	cmp->takeName(insn);
	insn->replaceAllUsesWith(cmp);
	insn->eraseFromParent();

	return true;
}

/**
 * a = add x, c1
 * b = add a, c2
 *   =>
 * b = add x, (c1 + c2)
 */
bool addSequence(llvm::Instruction* insn)
{
	Value* val;
	ConstantInt* c1;
	ConstantInt* c2;

	if (!(match(insn, m_Add(
			m_Add(m_Value(val), m_ConstantInt(c1)),
			m_ConstantInt(c2)))))
	{
		return false;
	}

	Instruction* secondAdd = cast<Instruction>(insn->getOperand(0));
	insn->setOperand(0, val);
	insn->setOperand(1, ConstantInt::get(insn->getType(), c1->getValue() + c2->getValue()));

	if (secondAdd->user_empty())
	{
		secondAdd->eraseFromParent();
	}

	return true;
}

/**
 * cast1 fp/ptr to ...
 * ...
 * cast2 ... to fp/ptr
 *   =>
 * cast fp/ptr to fp/ptr
 *
 * Do not do this for integers. It is not always safe.
 * E.g. i32 -> i1 -> i32 is not the same as i32 -> i32.
 * It may not be safe for pointers and floats as well, but we leave it for now.
 */
llvm::Value* castSequence(llvm::CastInst* cast1, llvm::CastInst* cast2)
{
	if (cast1 == nullptr || cast2 == nullptr
			|| cast1->getParent() == nullptr || cast2->getParent() == nullptr)
	{
		return nullptr;
	}

	auto* src = cast1->getOperand(0);
	auto* srcTy = cast1->getSrcTy();
	auto* dstTy = cast2->getDestTy();

	Value* v = nullptr;

	if (srcTy->isPointerTy() && dstTy->isPointerTy())
	{
		v = srcTy != dstTy
				? CastInst::CreatePointerCast(src, dstTy, "", cast2)
				: src;
	}
	// float -> cast -> cast -> float
	else if (srcTy->isFloatingPointTy() && dstTy->isFloatingPointTy())
	{
		v = srcTy != dstTy
				? CastInst::CreateFPCast(src, dstTy, "", cast2)
				: src;
	}
	else
	{
		return nullptr;
	}

	cast2->replaceAllUsesWith(v);
	cast2->eraseFromParent();
	if (cast1->user_empty())
	{
		cast1->eraseFromParent();
	}
	return v;
}

/**
 * Find cast sequnces to try to optimize.
 */
llvm::Value* castSequenceFinder(llvm::Value* insn)
{
	auto* cast2 = dyn_cast<CastInst>(insn);
	auto* cast1 = cast2 ? dyn_cast<CastInst>(cast2->getOperand(0)) : nullptr;

	while (cast1)
	{
		if (auto* v = castSequence(cast1, cast2))
		{
			return v;
		}
		cast1 = dyn_cast<CastInst>(cast1->getOperand(0));
	}

	return nullptr;
}

/**
 * Apply cast optimization repeatedly until it can not be applied anymore.
 */
bool castSequenceWrapper(llvm::Instruction* insn)
{
	if (insn == nullptr || insn->getParent() == nullptr || !isa<CastInst>(insn))
	{
		return false;
	}

	bool changed = false;
	Value* v = insn;
	unsigned depth = 0;
	while (v && depth++ < 64)
	{
		v = castSequenceFinder(v);
		changed |= v != nullptr;
	}
	return changed;
}

/**
 * \code{.ll}
 * store float %val, float* bitcast (i32* @gv to float*)
 *   ==>
 * %conv = bitcast float %val to i32
 * store i32 %conv, i32* @gv
 * \endcode
 *
 * This is countering an undesirable LLVM instrcombine optimization
 * that is going the other way.
 */
bool storeToBitcastPointer(llvm::Instruction* insn)
{
	Value* val;
	Value* op;
	if (!match(insn, m_Store(m_Value(val), m_BitCast(m_Value(op))))
			|| !op->getType()->getPointerElementType()->isFirstClassType()
			|| op->getType()->getPointerElementType()->isAggregateType()
			|| op->getType()->getPointerElementType()->isPointerTy())
	{
		return false;
	}

	if (!BitCastInst::isBitCastable(
			val->getType(),
			op->getType()->getPointerElementType()))
	{
		return false;
	}

	auto* conv = CastInst::CreateBitOrPointerCast(
			val,
			op->getType()->getPointerElementType(),
			"",
			insn);
	new StoreInst(conv, op, insn);

	auto* bitcastI = dyn_cast<BitCastInst>(insn->getOperand(1));
	auto* bitcastCE = dyn_cast<ConstantExpr>(insn->getOperand(1));
	insn->eraseFromParent();
	if (bitcastI && bitcastI->use_empty())
	{
		bitcastI->eraseFromParent();
	}
	if (bitcastCE && bitcastCE->use_empty())
	{
		bitcastCE->destroyConstant();
	}

	return true;
}

/**
 * \code{.ll}
 * %1 = load float, float* bitcast (i32* @g to float*)
 *   ==>
 * %1 = load i32, i32* @g
 * %2 = bitcast i32 %1 to float
 *
 * %1 = load i8*, i8** bitcast (i32* @g to i8**)
 *   ==>
 * %1 = load i32, i32* @g
 * %2 = inttoptr i32 %1 to i8*
 * \endcode
 */
bool loadFromBitcastPointer(llvm::Instruction* insn)
{
	Value* op;
	if (!match(insn, m_Load(m_BitCast(m_Value(op))))
			|| !op->getType()->getPointerElementType()->isFirstClassType()
			|| op->getType()->getPointerElementType()->isAggregateType())
	{
		return false;
	}

	if (!BitCastInst::isBitOrNoopPointerCastable(
			op->getType()->getPointerElementType(),
			insn->getType(),
			insn->getModule()->getDataLayout()))
	{
		return false;
	}

	auto* l = new LoadInst(op, "", insn);
	l->setAlignment(cast<LoadInst>(insn)->getAlignment());
	auto* conv = CastInst::CreateBitOrPointerCast(l, insn->getType(), "", insn);
	insn->replaceAllUsesWith(conv);

	auto* bitcastI = dyn_cast<BitCastInst>(insn->getOperand(0));
	auto* bitcastCE = dyn_cast<ConstantExpr>(insn->getOperand(0));

	insn->eraseFromParent();
	if (bitcastI && bitcastI->use_empty())
	{
		bitcastI->eraseFromParent();
	}
	if (bitcastCE && bitcastCE->use_empty())
	{
		bitcastCE->destroyConstant();
	}

	return true;
}

/**
 * trunc T x to T  =>  x
 *
 * A no-op truncation (source == destination type) is invalid LLVM IR.
 * Replace it with the source value so later passes see valid IR.
 */
bool noopTrunc(llvm::Instruction* insn)
{
	auto* trunc = dyn_cast<TruncInst>(insn);
	if (trunc == nullptr)
	{
		return false;
	}
	if (trunc->getSrcTy() != trunc->getDestTy())
	{
		return false;
	}
	trunc->replaceAllUsesWith(trunc->getOperand(0));
	trunc->eraseFromParent();
	return true;
}

/**
 * icmp op T1 x, T2 y  (T1 != T2, both integers)  =>
 *   %widened = sext/zext narrower to wider type
 *   icmp op T_wide x', y'
 *
 * LLVM requires both operands to an ICmp to share the same type.
 * We sign-extend or zero-extend the narrower value to match the wider one.
 */
bool icmpTypeMismatch(llvm::Instruction* insn)
{
	auto* icmp = dyn_cast<ICmpInst>(insn);
	if (icmp == nullptr)
	{
		return false;
	}
	Value* lhs = icmp->getOperand(0);
	Value* rhs = icmp->getOperand(1);
	auto* lhsTy = dyn_cast<IntegerType>(lhs->getType());
	auto* rhsTy = dyn_cast<IntegerType>(rhs->getType());
	if (!lhsTy || !rhsTy || lhsTy == rhsTy)
	{
		return false;
	}
	// Widen the narrower operand to the wider type.
	unsigned lhsBits = lhsTy->getBitWidth();
	unsigned rhsBits = rhsTy->getBitWidth();
	bool isSigned = icmp->isSigned();
	if (lhsBits < rhsBits)
	{
		Value* extended = isSigned
			? CastInst::CreateSExtOrBitCast(lhs, rhsTy, "", icmp)
			: CastInst::CreateZExtOrBitCast(lhs, rhsTy, "", icmp);
		icmp->setOperand(0, extended);
	}
	else
	{
		Value* extended = isSigned
			? CastInst::CreateSExtOrBitCast(rhs, lhsTy, "", icmp)
			: CastInst::CreateZExtOrBitCast(rhs, lhsTy, "", icmp);
		icmp->setOperand(1, extended);
	}
	return true;
}

/**
 * Order here is important.
 * More specific patterns must go first, more general later.
 */
std::vector<OptEntry> optimizations =
{
		// IR sanitization: fix invalid instructions before LLVM verifier runs.
		{"noopTrunc", &noopTrunc},
		{"icmpTypeMismatch", &icmpTypeMismatch},
		// More specific patterns first.
		{"truncZext", &truncZext},
		{"addZero", &addZero},
		{"subZero", &subZero},
		{"xorLoadXX", &xorLoadXX},
		{"xorXX", &xorXX},
		{"xor_i1", &xor_i1},
		{"and_i1", &and_i1},
		{"orAndLoadXX", &orAndLoadXX},
		{"orAndXX", &orAndXX},
		{"addSequence", &addSequence},
		{"storeToBitcastPointer", &storeToBitcastPointer},
		{"loadFromBitcastPointer", &loadFromBitcastPointer},
		{"castSequenceWrapper", &castSequenceWrapper},
		// Extended patterns (inst_opt_ext.cpp)
		{"mulZero", &mulZero},
		{"orAllOnes", &orAllOnes},
		{"andZero", &andZero},
		{"subSelf", &subSelf},
		{"shiftByZero", &shiftByZero},
		{"selectSame", &selectSame},
		{"orAndSelf", &orAndSelf},
};

bool optimize(llvm::Instruction* insn)
{
	const std::string fnName = insn && insn->getFunction()
			? insn->getFunction()->getName().str()
			: "<unknown>";
	const std::string opName = insn ? insn->getOpcodeName() : "<null>";
	const auto maxPatterns = instOptMaxPatterns();
	unsigned long idx = 0;
	for (auto& opt : optimizations)
	{
		++idx;
		if (maxPatterns != 0 && idx > maxPatterns)
		{
			break;
		}
		if (instOptPatternTraceVerbose())
		{
			traceInstOptPattern(
				std::string("trying ")
				+ opt.name
				+ " in "
				+ fnName
				+ " on "
				+ opName
			);
		}
		if (opt.fn(insn))
		{
			traceInstOptPattern(
				std::string("applied ")
				+ opt.name
				+ " in "
				+ fnName
				+ " on "
				+ opName
			);
			return true;
		}
	}
	return false;
}

} // namespace inst_opt
} // namespace bin2llvmir
} // namespace retdec
