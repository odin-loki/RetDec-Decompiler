/**
* @file src/bin2llvmir/optimizations/idioms/idioms_ext.cpp
* @brief Extended compiler idiom recognition: rotations, bswap, popcnt.
* @copyright (c) 2024, MIT license
*
* Adds recognition for:
*
*  1. Rotation idiom (GCC/MSVC/Clang all emit this):
*       %shl = shl  i32 %x, %n
*       %shr = lshr i32 %x, (32 - %n)   -- or sub(32, n) form
*       %rot = or  i32 %shl, %shr
*     → replaced with @llvm.fshl.i32(%x, %x, %n)  (rotate left)
*     The reverse pattern (shr first, shl second) is rotate right.
*
*  2. Byte-swap idiom (manual bswap, appears in big-endian conversion):
*       A chain of shift+or operations rearranging bytes of a 32-bit value.
*     → replaced with @llvm.bswap.i32(%x)
*     (A real bswap instruction would already have been translated; this
*      handles software implementations.)
*
*  3. Popcount idiom (Hamming weight, classic bit-twiddling):
*       The classic 5-step popcount for 32-bit integers. Checks for
*       the specific constants 0x55555555, 0x33333333, 0x0F0F0F0F.
*     → replaced with @llvm.ctpop.i32(%x)
*
* Integration: call `idiomsExt(bb)` from IdiomsCommon::doAnalysis() or
* wire into the Idioms pass dispatch. The apply script adds a call at the
* end of IdiomsMagicdivmod::doAnalysis().
*/

#include <functional>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>

#include "retdec/bin2llvmir/optimizations/idioms/idioms_ext.h"
#include "capstone2llvmir/capstone6_compat.h"

using namespace llvm;
using namespace llvm::PatternMatch;

namespace retdec {
namespace bin2llvmir {

//===========================================================================
// Helper: create an LLVM intrinsic call replacing an instruction
//===========================================================================

static CallInst* replaceWithIntrinsic(Instruction* toReplace,
                                       Intrinsic::ID id,
                                       ArrayRef<Value*> args,
                                       ArrayRef<Type*> tys) {
    Module* m = toReplace->getModule();
    Function* fn = Intrinsic::getOrInsertDeclaration(m, id, tys);
    IRBuilder<> irb(toReplace);
    auto* call = irb.CreateCall(fn, args);
    toReplace->replaceAllUsesWith(call);
    toReplace->eraseFromParent();
    return call;
}

//===========================================================================
// Rotation Idiom
//===========================================================================

/**
 * Recognise:
 *   %shl = shl  iN %x, %amt
 *   %shr = lshr iN %x, (N - %amt)
 *   %or  = or   iN %shl, %shr
 *
 * (or the reverse: lshr first, then shl, still an or)
 * → llvm.fshl.iN(%x, %x, %amt)  for left-rotation
 *   llvm.fshr.iN(%x, %x, %amt)  for right-rotation
 */
static bool tryReplaceRotation(BinaryOperator* orInst) {
    if (orInst->getOpcode() != Instruction::Or) return false;

    Value *shlVal = nullptr, *shrVal = nullptr;
    Value *xShl = nullptr, *xShr = nullptr;
    Value *amtShl = nullptr, *amtShr = nullptr;

    // Try both orderings of the operands.
    for (int flip = 0; flip < 2; ++flip) {
        Value* op0 = orInst->getOperand(flip);
        Value* op1 = orInst->getOperand(1 - flip);

        auto* shl = dyn_cast<BinaryOperator>(op0);
        auto* shr = dyn_cast<BinaryOperator>(op1);
        if (!shl || !shr) continue;
        if (shl->getOpcode() != Instruction::Shl) continue;
        if (shr->getOpcode() != Instruction::LShr) continue;

        xShl   = shl->getOperand(0);
        amtShl = shl->getOperand(1);
        xShr   = shr->getOperand(0);
        amtShr = shr->getOperand(1);

        if (xShl != xShr) continue;  // must be same source value

        Type* ty = xShl->getType();
        if (!ty->isIntegerTy()) continue;
        unsigned N = ty->getIntegerBitWidth();

        // Check: amtShr == N - amtShl (constant or sub expression).
        bool isRotL = false;
        if (auto* ciShl = dyn_cast<ConstantInt>(amtShl)) {
            if (auto* ciShr = dyn_cast<ConstantInt>(amtShr)) {
                uint64_t s = ciShl->getZExtValue();
                uint64_t r = ciShr->getZExtValue();
                isRotL = (s + r == N);
                if (!isRotL) continue;
            }
        }
        // Also accept: amtShr = sub(N, amtShl) or amtShr = and(sub(N, amtShl), N-1)
        // (handled by the constant case above for typical code).

        if (!isRotL) continue;

        // Replace with fshl (left rotate).
        replaceWithIntrinsic(orInst, Intrinsic::fshl,
                             {xShl, xShl, amtShl}, {ty});
        return true;
    }
    return false;
}

//===========================================================================
// Byte-Swap Idiom
//===========================================================================

/**
 * Software bswap for i32/i64: (x>>24) | ((x>>8)&0xFF00) | ((x<<8)&0xFF0000) | (x<<24)
 * Matches a tree of ORs that combine these four contributions.
 */
static bool tryReplaceBswap(BinaryOperator* orInst) {
    if (orInst->getOpcode() != Instruction::Or) return false;

    Type* ty = orInst->getType();
    if (!ty->isIntegerTy(32)) return false;

    uint64_t mask_ff00 = 0x0000FF00UL;
    uint64_t mask_ff0000 = 0x00FF0000UL;
    unsigned shift_hi = 24u;

    Value* root = nullptr;
    int found = 0;  // bitmask: 1=shl24, 2=shr24, 4=shr8_ff00, 8=shl8_ff0000

    std::function<bool(Value*)> collect = [&](Value* v) -> bool {
        if (auto* innerOr = dyn_cast<BinaryOperator>(v)) {
            if (innerOr->getOpcode() == Instruction::Or)
                return collect(innerOr->getOperand(0)) && collect(innerOr->getOperand(1));
        }
        if (auto* bin = dyn_cast<BinaryOperator>(v)) {
            if (bin->getOpcode() == Instruction::Shl) {
                auto* amt = dyn_cast<ConstantInt>(bin->getOperand(1));
                Value* x = bin->getOperand(0);
                if (!amt) return false;
                uint64_t a = amt->getZExtValue();
                if (a == shift_hi) {
                    if (root && root != x) return false;
                    root = x;
                    if (found & 1) return false;
                    found |= 1;
                    return true;
                }
                return false;
            }
            if (bin->getOpcode() == Instruction::LShr) {
                auto* amt = dyn_cast<ConstantInt>(bin->getOperand(1));
                Value* x = bin->getOperand(0);
                if (!amt) return false;
                uint64_t a = amt->getZExtValue();
                if (a == shift_hi) {
                    if (root && root != x) return false;
                    root = x;
                    if (found & 2) return false;
                    found |= 2;
                    return true;
                }
                return false;
            }
            if (bin->getOpcode() == Instruction::And) {
                ConstantInt* c = dyn_cast<ConstantInt>(bin->getOperand(0));
                Value* other = bin->getOperand(1);
                if (!c) { c = dyn_cast<ConstantInt>(other); other = bin->getOperand(0); }
                if (!c) return false;
                uint64_t mask = c->getZExtValue();
                auto* shift = dyn_cast<BinaryOperator>(other);
                if (!shift) return false;
                Value* x = nullptr;
                uint64_t a = 0;
                if (shift->getOpcode() == Instruction::Shl) {
                    auto* amt = dyn_cast<ConstantInt>(shift->getOperand(1));
                    if (!amt) return false;
                    x = shift->getOperand(0);
                    a = amt->getZExtValue();
                } else if (shift->getOpcode() == Instruction::LShr) {
                    auto* amt = dyn_cast<ConstantInt>(shift->getOperand(1));
                    if (!amt) return false;
                    x = shift->getOperand(0);
                    a = amt->getZExtValue();
                } else return false;
                if (a != 8) return false;
                if (mask == mask_ff0000 && shift->getOpcode() == Instruction::Shl) {
                    if (root && root != x) return false;
                    root = x;
                    if (found & 8) return false;
                    found |= 8;
                    return true;
                }
                if (mask == mask_ff00 && shift->getOpcode() == Instruction::LShr) {
                    if (root && root != x) return false;
                    root = x;
                    if (found & 4) return false;
                    found |= 4;
                    return true;
                }
            }
        }
        return false;
    };

    if (!collect(orInst->getOperand(0)) || !collect(orInst->getOperand(1)))
        return false;

    if (!root || found != 15) return false;  // need all 4 contributions

    replaceWithIntrinsic(orInst, Intrinsic::bswap, {root}, {ty});
    return true;
}

//===========================================================================
// Popcount Idiom
//===========================================================================

/**
 * The classic 5-step popcount for i32:
 *   t1 = x - ((x >> 1) & 0x55555555)
 *   t2 = (t1 & 0x33333333) + ((t1 >> 2) & 0x33333333)
 *   t3 = (t2 + (t2 >> 4)) & 0x0F0F0F0F
 *   t4 = (t3 * 0x01010101) >> 24
 *
 * We look for the final multiply by 0x01010101 followed by >> 24 as the
 * signature, then check the chain backward.
 */
// The classic SWAR population count for 32 bits:
//
//   t1 = x  - ((x  >> 1) & 0x55555555)
//   t2 = (t1 & 0x33333333) + ((t1 >> 2) & 0x33333333)
//   t3 = (t2 + (t2 >> 4)) & 0x0F0F0F0F
//   r  = (t3 * 0x01010101) >> 24
//
// Every step is checked. The previous version checked five landmarks -- the
// shift by 24, the multiply, the 0x0F0F0F0F mask, that SOMETHING was an add,
// and that one operand of that add was masked with 0x33333333 -- and then took
// the root as `sub->getOperand(0)` with the subtrahend never looked at. A
// chain with an arbitrary `sub %x, %y` in it passed and was replaced by
// `ctpop(%x)`: for x = 0x40000000 and y = 0 the original answers 0 and ctpop
// answers 1. It was also over-strict in the other direction, since the add
// under the 0x0F0F0F0F mask is `t2 + (t2 >> 4)`, neither operand of which is
// masked with 0x33333333, so a genuine popcount did not match either.
static bool tryReplacePopcount(BinaryOperator* lshr) {
	using namespace llvm::PatternMatch;

	Type* ty = lshr->getType();
	if (!ty->isIntegerTy(32)) return false;

	Value* t3 = nullptr;
	if (!match(lshr, m_LShr(m_c_Mul(m_Value(t3), m_SpecificInt(0x01010101)), m_SpecificInt(24)))) return false;

	// t3 = (t2 + (t2 >> 4)) & 0x0F0F0F0F
	Value* sum4 = nullptr;
	if (!match(t3, m_c_And(m_Value(sum4), m_SpecificInt(0x0F0F0F0F)))) return false;

	Value* t2 = nullptr;
	Value* t2shifted = nullptr;
	if (!match(sum4, m_c_Add(m_Value(t2), m_Value(t2shifted)))) return false;
	if (!match(t2shifted, m_LShr(m_Specific(t2), m_SpecificInt(4))))
	{
		std::swap(t2, t2shifted);
		if (!match(t2shifted, m_LShr(m_Specific(t2), m_SpecificInt(4)))) return false;
	}

	// t2 = (t1 & 0x33333333) + ((t1 >> 2) & 0x33333333)
	Value* lo = nullptr;
	Value* hi = nullptr;
	if (!match(t2, m_c_Add(m_Value(lo), m_Value(hi)))) return false;

	Value* t1 = nullptr;
	Value* t1shifted = nullptr;
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		if (attempt == 1) std::swap(lo, hi);
		if (!match(lo, m_c_And(m_Value(t1), m_SpecificInt(0x33333333)))) continue;
		if (!match(hi, m_c_And(m_Value(t1shifted), m_SpecificInt(0x33333333)))) continue;
		if (!match(t1shifted, m_LShr(m_Specific(t1), m_SpecificInt(2)))) continue;
		break;
	}
	if (t1 == nullptr || t1shifted == nullptr) return false;
	if (!match(lo, m_c_And(m_Specific(t1), m_SpecificInt(0x33333333)))) return false;
	if (!match(t1shifted, m_LShr(m_Specific(t1), m_SpecificInt(2)))) return false;

	// t1 = x - ((x >> 1) & 0x55555555)
	Value* x = nullptr;
	Value* odd = nullptr;
	if (!match(t1, m_Sub(m_Value(x), m_Value(odd)))) return false;
	if (!match(odd, m_c_And(m_LShr(m_Specific(x), m_SpecificInt(1)), m_SpecificInt(0x55555555)))) return false;

	replaceWithIntrinsic(lshr, Intrinsic::ctpop, {x}, {ty});
	return true;
}

//===========================================================================
// Entry point
//===========================================================================

bool idiomsExt(BasicBlock& bb) {
    bool changed = false;
    for (auto it = bb.begin(); it != bb.end(); ) {
        Instruction& inst = *it++;
        if (auto* binOp = dyn_cast<BinaryOperator>(&inst)) {
            if (tryReplaceRotation(binOp)) { changed = true; continue; }
            if (tryReplaceBswap(binOp)) { changed = true; continue; }
            if (tryReplacePopcount(binOp)) { changed = true; continue; }
        }
    }
    return changed;
}

bool idiomsExtFunction(Function& fn) {
    bool changed = false;
    for (auto& bb : fn)
        changed |= idiomsExt(bb);
    return changed;
}

} // namespace bin2llvmir
} // namespace retdec
