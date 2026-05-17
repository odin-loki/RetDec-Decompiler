/**
* @file src/bin2llvmir/optimizations/simple_types/simple_types_fp_ext.cpp
* @brief Supplemental type propagation for FP conversions and aggregates.
* @copyright (c) 2024, MIT license
*
* Fills the TODO stubs in simple_types.cpp for:
*   FPToUI, FPToSI  — propagate FP type to the source operand
*   UIToFP, SIToFP  — propagate FP type to the result
*   FPTrunc, FPExt  — propagate FP type between narrow/wide operands
*   FCmp            — both operands must be same float type
*   Select          — true/false values must be same type as result
*   ExtractValue    — result shares struct/array element type
*   InsertValue     — inserted value shares aggregate element type
*
* This is a standalone utility called from simple_types.cpp's processUse()
* after the existing switch chain. Wire it in by including the header and
* adding a call to `simpleTypesFpExt(user, toProcess)` at the TODO sites,
* or apply as a patch to simple_types.cpp using the sed commands in the
* apply script.
*
* Design: this uses the same toProcess stack that simple_types.cpp uses.
* "toProcess.push(v)" means: schedule value v for further type-set merging.
*/

#include <queue>

#include <llvm/IR/Instructions.h>

#include "retdec/bin2llvmir/optimizations/simple_types/simple_types_fp_ext.h"

namespace retdec {
namespace bin2llvmir {

/**
 * Handle FP-conversion and aggregate TODO stubs from simple_types.cpp.
 *
 * @param user   The LLVM instruction being processed as a "user" in the
 *               simple_types propagation loop.
 * @param toProcess  The std::stack<llvm::Value*> used by simple_types.cpp.
 *
 * @return true if any item was pushed to @a toProcess.
 */
bool simpleTypesFpExt(llvm::Instruction* user,
		std::queue<llvm::Value*>& toProcess) {
    if (!user) return false;

    unsigned opc = user->getOpcode();
    bool changed = false;

    //─────────────────────────────────────────────────────────────────────
    // FPToUI / FPToSI  :  (float_type) → integer_type
    //   The source operand is a float; propagate float type to it.
    //─────────────────────────────────────────────────────────────────────
    if (opc == llvm::Instruction::FPToUI || opc == llvm::Instruction::FPToSI) {
        llvm::Value* src = user->getOperand(0);
        if (llvm::isa<llvm::Instruction>(src)) {
            toProcess.push(src);
            changed = true;
        }
    }

    //─────────────────────────────────────────────────────────────────────
    // UIToFP / SIToFP  :  integer_type → (float_type)
    //   The result is a float; the conversion instruction itself is the
    //   typed value — push it so downstream stores pick up the float type.
    //─────────────────────────────────────────────────────────────────────
    else if (opc == llvm::Instruction::UIToFP || opc == llvm::Instruction::SIToFP) {
        toProcess.push(user);
        changed = true;
    }

    //─────────────────────────────────────────────────────────────────────
    // FPTrunc / FPExt  :  float_type ↔ float_type (narrower/wider)
    //   Propagate the float type from operand to result and vice versa.
    //─────────────────────────────────────────────────────────────────────
    else if (opc == llvm::Instruction::FPTrunc || opc == llvm::Instruction::FPExt) {
        llvm::Value* src = user->getOperand(0);
        if (llvm::isa<llvm::Instruction>(src)) {
            toProcess.push(src);
            changed = true;
        }
        toProcess.push(user);
        changed = true;
    }

    //─────────────────────────────────────────────────────────────────────
    // FCmp  :  compare two float operands, produce i1
    //   Both operands must be the same float type.
    //─────────────────────────────────────────────────────────────────────
    else if (opc == llvm::Instruction::FCmp) {
        llvm::Value* op0 = user->getOperand(0);
        llvm::Value* op1 = user->getOperand(1);
        if (llvm::isa<llvm::Instruction>(op0)) { toProcess.push(op0); changed = true; }
        if (llvm::isa<llvm::Instruction>(op1)) { toProcess.push(op1); changed = true; }
    }

    //─────────────────────────────────────────────────────────────────────
    // Select  :  op0=i1 cond, op1=true_val, op2=false_val
    //   op1 and op2 must be the same type as the select result.
    //─────────────────────────────────────────────────────────────────────
    else if (opc == llvm::Instruction::Select) {
        llvm::Value* tv = user->getOperand(1); // true value
        llvm::Value* fv = user->getOperand(2); // false value
        if (llvm::isa<llvm::Instruction>(tv)) { toProcess.push(tv); changed = true; }
        if (llvm::isa<llvm::Instruction>(fv)) { toProcess.push(fv); changed = true; }
        toProcess.push(user); // result has same type
        changed = true;
    }

    //─────────────────────────────────────────────────────────────────────
    // ExtractValue  :  result type = element type of aggregate
    //   Push the aggregate operand so its element types are propagated.
    //─────────────────────────────────────────────────────────────────────
    else if (opc == llvm::Instruction::ExtractValue) {
        llvm::Value* agg = user->getOperand(0);
        if (llvm::isa<llvm::Instruction>(agg)) { toProcess.push(agg); changed = true; }
        // result itself may feed further propagation
        toProcess.push(user);
        changed = true;
    }

    //─────────────────────────────────────────────────────────────────────
    // InsertValue  :  insert a typed value into an aggregate
    //   The inserted value (operand 1) shares type with the element slot.
    //─────────────────────────────────────────────────────────────────────
    else if (opc == llvm::Instruction::InsertValue) {
        llvm::Value* agg     = user->getOperand(0);
        llvm::Value* insVal  = user->getOperand(1);
        if (llvm::isa<llvm::Instruction>(agg))    { toProcess.push(agg);    changed = true; }
        if (llvm::isa<llvm::Instruction>(insVal)) { toProcess.push(insVal); changed = true; }
        toProcess.push(user);
        changed = true;
    }

    return changed;
}

} // namespace bin2llvmir
} // namespace retdec
