#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt_ext.h"
/**
 * @file src/bin2llvmir/optimizations/inst_opt/inst_opt.cpp
 * @brief Optimize a single LLVM instruction.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdlib>

#include "retdec/bin2llvmir/optimizations/inst_opt/inst_opt.h"
#include "retdec/bin2llvmir/utils/debug.h"
#include "retdec/bin2llvmir/utils/llvm.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;
using namespace PatternMatch;

namespace retdec {
namespace bin2llvmir {
namespace inst_opt {

namespace {

void attachPointeeOnPointerCast(Value* v)
{
	auto* i = dyn_cast<Instruction>(v);
	if (!i)
	{
		return;
	}
	auto* pt = dyn_cast<PointerType>(i->getType());
	if (!pt)
	{
		return;
	}
	llvm_utils::setPointeeTypeMetadata(i, llvm_utils::pointeeType(i));
}

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
 * Do these two loads certainly read the same value?
 *
 * Same pointer is not enough. Anything between them that may write memory --
 * a store, a call, an atomic operation, a fence -- can change what the second
 * one sees, and a volatile or atomic load may not be duplicated or removed at
 * all. This answers no unless both loads are plain, read the same pointer at
 * the same type, and sit in one basic block with nothing that writes memory
 * between them.
 */
bool loadsReadSameValue(llvm::LoadInst* l1, llvm::LoadInst* l2)
{
	if (l1 == nullptr || l2 == nullptr)
	{
		return false;
	}
	if (l1 == l2)
	{
		return true;
	}
	if (!l1->isSimple() || !l2->isSimple())
	{
		return false;
	}
	if (l1->getPointerOperand() != l2->getPointerOperand() || l1->getType() != l2->getType())
	{
		return false;
	}
	if (l1->getParent() == nullptr || l1->getParent() != l2->getParent())
	{
		return false;
	}

	// Program order, not argument order: the caller does not know which of the
	// two comes first.
	llvm::Instruction* first = l1;
	llvm::Instruction* second = l2;
	for (llvm::Instruction& i: *l1->getParent())
	{
		if (&i == l2)
		{
			first = l2;
			second = l1;
			break;
		}
		if (&i == l1)
		{
			break;
		}
	}

	for (auto it = std::next(first->getIterator()); &*it != second; ++it)
	{
		if (it->mayWriteToMemory())
		{
			return false;
		}
	}
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
	if (!loadsReadSameValue(l1, l2))
	{
		return false;
	}

	insn->replaceAllUsesWith(ConstantInt::get(insn->getType(), 0));
	insn->eraseFromParent();
	// `xor a, a` is zero whatever a is, so the fold is sound even for one
	// volatile load used twice -- but the load itself still has to happen.
	if (l1->isSimple() && l1->user_empty())
	{
		l1->eraseFromParent();
	}
	if (l2 != l1 && l2->isSimple() && l2->user_empty())
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
 * c = a
 *
 * a = load x
 * b = load x
 * c = and a, b
 *   =>
 * c = a
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
	if (!loadsReadSameValue(l1, l2))
	{
		return false;
	}

	insn->replaceAllUsesWith(l1);
	insn->eraseFromParent();
	if (l2 != l1 && l2->isSimple() && l2->user_empty())
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
 * a = and i1 x, true
 *   =>
 * a = x
 *
 * a = and i1 x, false
 *   =>
 * a = false
 *
 * This used to rewrite `and i1 x, y` to `icmp eq i1 x, y` for any pair of
 * operands. Those are different functions: for x = y = 0 the AND is 0 and the
 * comparison is 1. A one-bit AND is not a comparison and there is no two-operand
 * icmp that spells it, so nothing is done unless one side is a constant, where
 * the answer is the other operand or false. The test that covered this picked
 * `and i1 %a, 1`, the single input pattern on which the old rewrite and the
 * truth agree, so it could not have failed.
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

	auto* c0 = dyn_cast<ConstantInt>(op0);
	auto* c1 = dyn_cast<ConstantInt>(op1);
	Value* replacement = nullptr;

	if (c0 && c0->isZero())
	{
		replacement = c0;
	}
	else if (c1 && c1->isZero())
	{
		replacement = c1;
	}
	else if (c0 && c0->isOne())
	{
		replacement = op1;
	}
	else if (c1 && c1->isOne())
	{
		replacement = op0;
	}
	else
	{
		return false;
	}

	insn->replaceAllUsesWith(replacement);
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

	// Reassociating does not preserve the wrap flags. `(x +nsw 127) +nsw 127`
	// on i8 is defined for x = -127 -- neither step overflows -- while
	// `x +nsw -2` for the same x does overflow, and a defined value would
	// become poison. The flags describe the original pair of additions, so
	// they do not survive the pair being replaced.
	if (auto* bo = dyn_cast<BinaryOperator>(insn))
	{
		bo->setHasNoSignedWrap(false);
		bo->setHasNoUnsignedWrap(false);
	}

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
/**
 * How many bits of information does a value of this type carry, for the
 * purpose of deciding whether a cast chain threw any of them away?
 *
 * For a pointer that is the pointer's width on this target; for a float it is
 * the whole representation, because narrowing a double to a float loses
 * exponent range as well as mantissa bits; for an integer it is the integer's
 * width.
 */
unsigned informationBits(llvm::Type* t, const llvm::DataLayout& dl)
{
	if (t->isPointerTy())
	{
		return dl.getPointerTypeSizeInBits(t);
	}
	return t->getPrimitiveSizeInBits();
}

/**
 * Does the chain of casts from `cast1` up to and including `cast2` pass every
 * value through unharmed?
 *
 * A cast pair may only be collapsed if nothing between its two ends was
 * narrower than the ends themselves. `double -> float -> double` has matching
 * ends and is not the identity: 1.0000000000000002 comes back as 1.0. Under
 * opaque pointers the pointer case is worse, because every `ptr` in address
 * space 0 is the same Type*, so `ptrtoint ptr to i64 / trunc to i32 /
 * inttoptr i32 to ptr` has ends that compare equal and a middle that threw
 * away the top half of the address.
 */
bool castChainPreservesValue(llvm::CastInst* cast1, llvm::CastInst* cast2, const llvm::DataLayout& dl)
{
	unsigned floor = std::min(informationBits(cast1->getSrcTy(), dl), informationBits(cast2->getDestTy(), dl));
	if (floor == 0)
	{
		return false; // a type we cannot size; do not guess
	}

	// Walk the operand chain from cast2 back down to cast1, looking at every
	// intermediate type the value was forced through.
	llvm::Value* v = cast2->getOperand(0);
	for (unsigned steps = 0; steps < 64; ++steps)
	{
		auto* c = dyn_cast<CastInst>(v);
		if (c == nullptr)
		{
			return false; // cast1 was never reached: not one chain
		}
		unsigned bits = informationBits(c->getDestTy(), dl);
		if (bits == 0 || bits < floor)
		{
			return false;
		}
		if (c == cast1)
		{
			return true;
		}
		v = c->getOperand(0);
	}
	return false;
}

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

	const llvm::DataLayout& dl = cast2->getModule()->getDataLayout();

	Value* v = nullptr;

	if (srcTy->isPointerTy() && dstTy->isPointerTy())
	{
		// An addrspacecast is a target-specific conversion, not a
		// reinterpretation, so two pointers from different address spaces are
		// not two ends of one chain. RetDec does produce non-zero address
		// spaces, for x86 fs:/gs:.
		if (srcTy->getPointerAddressSpace() != dstTy->getPointerAddressSpace())
		{
			return nullptr;
		}
		if (!castChainPreservesValue(cast1, cast2, dl))
		{
			return nullptr;
		}
		v = srcTy != dstTy
				? CastInst::CreatePointerCast(src, dstTy, "", cast2)
				: src;
		if (v != src)
		{
			attachPointeeOnPointerCast(v);
		}
	}
	// float -> cast -> cast -> float
	else if (srcTy->isFloatingPointTy() && dstTy->isFloatingPointTy())
	{
		if (!castChainPreservesValue(cast1, cast2, dl))
		{
			return nullptr;
		}
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
	// llvm_utils::createStoreInst always builds a plain store at the ABI
	// alignment for the type. Rebuilding a volatile store that way drops the
	// side effect, and rebuilding an under-aligned one claims an alignment the
	// pointer does not have.
	if (auto* si = dyn_cast<StoreInst>(insn))
	{
		if (!si->isSimple())
		{
			return false;
		}
	}

	Value* val;
	Value* op;
	if (match(insn, m_Store(m_Value(val), m_BitCast(m_Value(op)))))
	{
		;
	}
	else if (!match(insn, m_Store(m_Value(val), m_Value(op))))
	{
		return false;
	}
	auto* ptee = llvm_utils::pointeeType(op);
	if (!ptee
			|| ptee == val->getType()
			|| !ptee->isFirstClassType()
			|| ptee->isAggregateType()
			|| ptee->isPointerTy())
	{
		return false;
	}

	if (!BitCastInst::isBitCastable(val->getType(), ptee))
	{
		return false;
	}

	auto* conv = CastInst::CreateBitOrPointerCast(
			val,
			ptee,
			"",
			insn);
	auto* st = llvm_utils::createStoreInst(conv, op, insn);
	st->setAlignment(cast<StoreInst>(insn)->getAlign());

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
	// Same reason as storeToBitcastPointer: createLoadInst builds a plain
	// load, so a volatile or atomic read would come back as one the optimiser
	// is free to delete, duplicate or reorder.
	if (auto* li = dyn_cast<LoadInst>(insn))
	{
		if (!li->isSimple())
		{
			return false;
		}
	}

	Value* op;
	if (match(insn, m_Load(m_BitCast(m_Value(op)))))
	{
		;
	}
	else if (auto* li = dyn_cast<LoadInst>(insn))
	{
		op = li->getPointerOperand();
	}
	else
	{
		return false;
	}
	auto* ptee = llvm_utils::pointeeType(op);
	if (!ptee
			|| ptee == insn->getType()
			|| !ptee->isFirstClassType()
			|| ptee->isAggregateType())
	{
		return false;
	}

	if (!BitCastInst::isBitOrNoopPointerCastable(
			ptee,
			insn->getType(),
			insn->getModule()->getDataLayout()))
	{
		return false;
	}

	auto* l = llvm_utils::createLoadInst(op, ptee, "", insn);
	l->setAlignment(cast<LoadInst>(insn)->getAlign());
	auto* conv = CastInst::CreateBitOrPointerCast(l, insn->getType(), "", insn);
	insn->replaceAllUsesWith(conv);
	attachPointeeOnPointerCast(conv);

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
	// This runs once per instruction in the module, and the trace is off in
	// every build nobody has deliberately switched it on for. It used to read
	// the environment once per pattern per instruction -- 22 getenv calls each
	// time round -- and build two heap-allocated strings, including
	// getName().str(), whose only consumer was output that was not being
	// produced. The environment is read once per process now, and the strings
	// are built only when something is going to print them.
	static const bool traceOn = instOptPatternTraceEnabled();
	static const bool traceVerbose = instOptPatternTraceVerbose();
	static const unsigned long maxPatterns = instOptMaxPatterns();

	// Captured up front, and only when something will read them: a pattern
	// that fires erases `insn`, so after opt.fn returns there is nothing left
	// to ask for the name.
	std::string fnName;
	std::string opName;
	if (traceOn || traceVerbose)
	{
		fnName = insn && insn->getFunction() ? insn->getFunction()->getName().str() : "<unknown>";
		opName = insn ? insn->getOpcodeName() : "<null>";
	}

	unsigned long idx = 0;
	for (auto& opt : optimizations)
	{
		++idx;
		if (maxPatterns != 0 && idx > maxPatterns)
		{
			break;
		}
		if (traceVerbose)
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
			if (traceOn)
			{
				traceInstOptPattern(std::string("applied ") + opt.name + " in " + fnName + " on " + opName);
			}
			return true;
		}
	}
	return false;
}

} // namespace inst_opt
} // namespace bin2llvmir
} // namespace retdec
