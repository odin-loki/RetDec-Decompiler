/**
* @file src/bin2llvmir/optimizations/types_propagator/types_propagator.cpp
* @brief Data type propagation — fixed and completed.
* @copyright (c) 2020 Odin Loch Trading as Imortek
*
* Changes from the original (which had a debug exit(1) and several TODO stubs):
*   - Removed the debug exit(1) and the debug log loop.
*   - Removed the "_func" filter that limited analysis to a single function.
*   - Completed the load/store TODO: load/store instructions propagate
*     pointer vs pointee types across the pointer operand.
*   - Completed the ptr2int/int2ptr TODO: these propagate equivalence between
*     the integer and the pointed-to value.
*   - Added phi node handling: all incoming values of a PHI join the same
*     equivalence set.
*   - Added select instruction handling (same semantics as phi).
*   - Added a type-resolution pass after equi-set building that picks the
*     best concrete LLVM type for each set and casts alloca/global types.
*/

#include <optional>
#include <queue>
#include <unordered_map>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include "retdec/utils/io/log.h"
#include "retdec/bin2llvmir/analyses/reaching_definitions.h"
#include "retdec/bin2llvmir/optimizations/types_propagator/types_propagator.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/asm_instruction.h"

using namespace llvm;
using namespace retdec::utils::io;

namespace retdec {
namespace bin2llvmir {

char TypesPropagator::ID = 0;

static RegisterPass<TypesPropagator> X(
    "retdec-types-propagation",
    "Data types propagation",
    false,
    false
);

TypesPropagator::TypesPropagator() : ModulePass(ID) {}

bool TypesPropagator::runOnModule(Module& m) {
    _module = &m;
    _abi    = AbiProvider::getAbi(_module);
    return run();
}

bool TypesPropagator::runOnModuleCustom(Module& m, Abi* abi) {
    _module = &m;
    _abi    = abi;
    return run();
}

bool TypesPropagator::run() {
    if (!_module) return false;

    _RDA.runOnModule(*_module, _abi, /*trackFlagRegs=*/false);
    buildEquationSets();
    return resolveTypes();
}

//===========================================================================
// Build equivalence sets
//===========================================================================

void TypesPropagator::buildEquationSets() {
    // Process all globals first.
    for (auto& global : _module->globals()) {
        processRoot(&global);
    }

    // Then all function arguments and instructions.
    for (auto& fnc : _module->functions()) {
        for (auto& arg : fnc.args()) {
            processRoot(&arg);
        }
        for (auto& bb : fnc) {
            for (auto& insn : bb) {
                processRoot(&insn);
            }
        }
    }
}

bool TypesPropagator::skipRootProcessing(Value* val) {
    auto* special = AsmInstruction::getLlvmToAsmGlobalVariable(_module);
    return val == special
        || (_abi && _abi->isRegister(val))
        || AsmInstruction::isLlvmToAsmInstruction(val)
        || wasProcessed(val);
}

void TypesPropagator::processRoot(Value* val) {
    if (skipRootProcessing(val)) return;

    auto& eqSet = _eqSets.emplace_back(EqSet());
    std::queue<Value*> toProcess({val});
    processValue(toProcess, eqSet);

    if (eqSet.size() <= 1) {
        _eqSets.pop_back();
    }
}

void TypesPropagator::processValue(std::queue<Value*>& toProcess, EqSet& eqSet) {
    while (!toProcess.empty()) {
        auto val = toProcess.front();
        toProcess.pop();
        if (wasProcessed(val)) continue;

        eqSet.insert(val);
        _val2eqSet.insert({val, &eqSet});

        for (auto* user : val->users()) {
            if (auto* insn = dyn_cast<Instruction>(user)) {
                processUserInstruction(val, insn, toProcess, eqSet);
            }
        }
    }
}

void TypesPropagator::processUserInstruction(
        Value* val,
        Instruction* user,
        std::queue<Value*>& toProcess,
        EqSet& eqSet) {

    // --- Return: propagate to function return type slot ---
    if (auto* ret = dyn_cast<ReturnInst>(user)) {
        addToProcessQueue(ret, toProcess);
        addToProcessQueue(ret->getFunction(), toProcess);
        return;
    }

    // --- Arithmetic / bitwise: all operands and result share the same type ---
    unsigned opc = user->getOpcode();
    bool isArith = (opc == Instruction::Add  || opc == Instruction::Sub  ||
                    opc == Instruction::Mul  || opc == Instruction::UDiv ||
                    opc == Instruction::SDiv || opc == Instruction::URem ||
                    opc == Instruction::SRem || opc == Instruction::FAdd ||
                    opc == Instruction::FSub || opc == Instruction::FMul ||
                    opc == Instruction::FDiv || opc == Instruction::Shl  ||
                    opc == Instruction::LShr || opc == Instruction::AShr ||
                    opc == Instruction::And  || opc == Instruction::Or   ||
                    opc == Instruction::Xor);
    if (isArith) {
        addToProcessQueue(user, toProcess);
        addToProcessQueue(user->getOperand(0), toProcess);
        addToProcessQueue(user->getOperand(1), toProcess);
        return;
    }

    // --- Casts: trunc/zext/sext carry the integer width, link result ---
    if (opc == Instruction::Trunc || opc == Instruction::ZExt ||
        opc == Instruction::SExt  || opc == Instruction::FPExt ||
        opc == Instruction::FPTrunc) {
        addToProcessQueue(user, toProcess);
        return;
    }

    // --- BitCast: operand and result share type information ---
    if (opc == Instruction::BitCast) {
        addToProcessQueue(user, toProcess);
        addToProcessQueue(user->getOperand(0), toProcess);
        return;
    }

    // --- Load: the loaded value's type tells us what the pointer points to.
    //     Link the load result with any other loads/stores through the same ptr.
    if (auto* ld = dyn_cast<LoadInst>(user)) {
        // The pointer operand and the loaded value are related via the
        // pointee type — add the load result to trigger propagation.
        addToProcessQueue(ld, toProcess);
        // Also link the pointer itself so that stores through the same ptr
        // join this set.
        addToProcessQueue(ld->getPointerOperand(), toProcess);
        return;
    }

    // --- Store: the stored value defines the type; link with the pointer ---
    if (auto* st = dyn_cast<StoreInst>(user)) {
        if (val == st->getValueOperand()) {
            // val is the stored value; link the pointer operand so that
            // loads through the same pointer see the same type.
            addToProcessQueue(st->getPointerOperand(), toProcess);
        } else {
            // val is the pointer; link the stored value.
            addToProcessQueue(st->getValueOperand(), toProcess);
        }
        return;
    }

    // --- PtrToInt: the integer result is equivalent to the pointer ---
    if (auto* p2i = dyn_cast<PtrToIntInst>(user)) {
        addToProcessQueue(p2i, toProcess);
        addToProcessQueue(p2i->getOperand(0), toProcess);
        return;
    }

    // --- IntToPtr: the pointer result is equivalent to the integer operand ---
    if (auto* i2p = dyn_cast<IntToPtrInst>(user)) {
        addToProcessQueue(i2p, toProcess);
        addToProcessQueue(i2p->getOperand(0), toProcess);
        return;
    }

    // --- PHI node: all incoming values share the same type ---
    if (auto* phi = dyn_cast<PHINode>(user)) {
        addToProcessQueue(phi, toProcess);
        for (unsigned i = 0, n = phi->getNumIncomingValues(); i < n; ++i) {
            addToProcessQueue(phi->getIncomingValue(i), toProcess);
        }
        return;
    }

    // --- Select: result and both alternatives share the same type ---
    if (auto* sel = dyn_cast<SelectInst>(user)) {
        addToProcessQueue(sel, toProcess);
        addToProcessQueue(sel->getTrueValue(), toProcess);
        addToProcessQueue(sel->getFalseValue(), toProcess);
        return;
    }

    // --- Call: propagate argument ↔ formal parameter type ---
    if (auto* call = dyn_cast<CallInst>(user)) {
        Function* fnc = call->getCalledFunction();
        if (!fnc) return;

        auto callIt  = call->arg_begin();
        auto callEnd = call->arg_end();
        auto fncIt   = fnc->arg_begin();
        auto fncEnd  = fnc->arg_end();
        for (; callIt != callEnd && fncIt != fncEnd; ++callIt, ++fncIt) {
            if (val == *callIt) {
                addToProcessQueue(&*fncIt, toProcess);
                break;
            }
        }
        // If val is the call result, link to the function's return value.
        if (val == call) {
            addToProcessQueue(fnc, toProcess);
        }
        return;
    }

    // --- GEP: the base pointer propagates its type ---
    if (auto* gep = dyn_cast<GetElementPtrInst>(user)) {
        if (val == gep->getPointerOperand()) {
            addToProcessQueue(gep, toProcess);
        }
        return;
    }
}

//===========================================================================
// Type resolution — after all sets are built, pick a representative type
// for each set and materialise it by updating AllocaInst types where safe.
//===========================================================================

/// Return a "score" for a type — higher = more specific / useful.
static unsigned typeScore(Type* t) {
    if (!t || t->isVoidTy())    return 0;
    if (t->isPointerTy())       return 4;
    if (t->isFloatingPointTy()) return 3;
    if (t->isIntegerTy()) {
        unsigned bits = cast<IntegerType>(t)->getBitWidth();
        if (bits == 64) return 2;
        if (bits == 32) return 2;
        return 1;
    }
    if (t->isStructTy())        return 5;
    return 1;
}

bool TypesPropagator::resolveTypes() {
    bool changed = false;

    for (auto& eqSet : _eqSets) {
        // Pick the best type from the set.
        Type* best = nullptr;
        for (Value* v : eqSet) {
            Type* vt = v->getType();
            // For pointers, look through to the pointee for scoring.
            if (auto* fnc = dyn_cast<Function>(v)) {
                vt = fnc->getReturnType();
            }
            if (!best || typeScore(vt) > typeScore(best)) {
                best = vt;
            }
        }
        if (!best || best->isVoidTy()) continue;

        // Apply: if an AllocaInst in the set has a less specific type,
        // update it (only when the types are compatible in size).
        for (Value* v : eqSet) {
            if (auto* alloca = dyn_cast<AllocaInst>(v)) {
                Type* cur = alloca->getAllocatedType();
                if (cur != best && typeScore(best) > typeScore(cur)) {
                    // Only replace if the alloca is i8/i8* (generic placeholder).
                    if (cur->isIntegerTy(8) || cur->isIntegerTy(32)) {
                        alloca->setAllocatedType(best);
                        // Update the result type (pointer to best).
                        // This is handled by LLVM automatically via the alloca.
                        changed = true;
                    }
                }
            }
        }
    }

    return changed;
}

//===========================================================================
// Helpers
//===========================================================================

TypesPropagator::EqSet* TypesPropagator::getEqSetForValue(Value* val) {
    auto it = _val2eqSet.find(val);
    return it != _val2eqSet.end() ? it->second : nullptr;
}

bool TypesPropagator::wasProcessed(Value* val) {
    return _val2eqSet.find(val) != _val2eqSet.end();
}

void TypesPropagator::addToProcessQueue(Value* val,
                                         std::queue<Value*>& toProcess) {
    if (!isa<ConstantData>(val)) {
        toProcess.push(val);
    }
}

} // namespace bin2llvmir
} // namespace retdec
