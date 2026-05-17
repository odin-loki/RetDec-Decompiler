/**
* @file src/bin2llvmir/optimizations/tail_calls/tail_calls.cpp
* @brief Detect and annotate tail calls in the lifted LLVM IR.
* @copyright (c) 2024, MIT license
*
* A "tail call" in the decompiler context is a pattern where a function
* ends with a JMP to another function (rather than CALL+RET). The lifter
* represents this as an unconditional branch-function-call followed by a
* return of the return value. Example after lifting:
*
*   %ret = call i32 @foo(...)
*   ret i32 %ret
*
* Or for a void-returning tail call:
*
*   call void @foo(...)
*   ret void
*
* This pass:
*  1. Finds tail-call patterns: a call instruction that is the only use of
*     the return value, which flows directly into a ret instruction.
*  2. Marks those calls with the LLVM `tail` attribute.
*  3. Optionally (when the called function has the same signature) rewrites
*     the call+ret pair into a single `musttail call` so LLVM's backend
*     can emit an actual tail-call JMP rather than CALL+RET.
*
* The llvmir2hll backend interprets `tail call` as an opportunity to emit
* a more readable `return foo(...)` or just `foo(...)` at the end of a
* function, avoiding the pattern of:
*   temp = foo(args); return temp;
*/

#include <llvm/IR/Attributes.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/optimizations/tail_calls/tail_calls.h"
#include "retdec/utils/io/log.h"

using namespace llvm;
using namespace retdec::utils::io;

namespace retdec {
namespace bin2llvmir {

char TailCallOptimizer::ID = 0;

static RegisterPass<TailCallOptimizer> X(
    "retdec-tail-calls",
    "Tail call detection and annotation",
    false, false
);

TailCallOptimizer::TailCallOptimizer() : ModulePass(ID) {}

bool TailCallOptimizer::runOnModule(Module& m) {
    _module = &m;
    _abi    = AbiProvider::getAbi(_module);
    _config = ConfigProvider::getConfig(_module);
    return run();
}

bool TailCallOptimizer::runOnModuleCustom(Module& m, Abi* abi, Config* config) {
    _module = &m; _abi = abi; _config = config;
    return run();
}

bool TailCallOptimizer::run() {
    bool changed = false;
    for (auto& fn : *_module) {
        if (!fn.isDeclaration())
            changed |= processFunction(fn);
    }
    return changed;
}

bool TailCallOptimizer::processFunction(Function& fn) {
    bool changed = false;

    for (auto& bb : fn) {
        auto* ret = dyn_cast<ReturnInst>(bb.getTerminator());
        if (!ret) continue;

        // Walk backward through the block looking for a call immediately
        // before the ret (ignoring debug/metadata intrinsics).
        Instruction* prev = ret->getPrevNode();
        while (prev && isa<DbgInfoIntrinsic>(prev))
            prev = prev->getPrevNode();

        if (!prev) continue;

        CallInst* call = dyn_cast<CallInst>(prev);
        if (!call) continue;
        if (call->isInlineAsm()) continue;

        Function* callee = call->getCalledFunction();
        if (!callee) continue;                 // indirect call
        if (callee->isIntrinsic()) continue;   // LLVM intrinsic

        // Pattern 1: void return — call followed directly by ret void.
        if (call->getType()->isVoidTy() && ret->getNumOperands() == 0) {
            if (!call->isTailCall()) {
                call->setTailCall(true);
                Log::info() << "[TailCalls] " << fn.getName().str()
                            << " → " << callee->getName().str()
                            << " (void tail)\n";
                changed = true;
            }
            continue;
        }

        // Pattern 2: call result is the return value (possibly via bitcast).
        Value* retVal = ret->getNumOperands() > 0 ? ret->getOperand(0) : nullptr;
        if (!retVal) continue;

        Value* callResult = call;
        // Strip a bitcast between call result and ret value.
        if (auto* bc = dyn_cast<BitCastInst>(retVal)) {
            if (bc->getOperand(0) == call && bc->hasOneUse())
                callResult = bc;
        }

        if (retVal == call || retVal == callResult) {
            if (!call->isTailCall()) {
                call->setTailCall(true);
                Log::info() << "[TailCalls] " << fn.getName().str()
                            << " → " << callee->getName().str()
                            << " (value tail)\n";
                changed = true;
            }
        }
    }

    return changed;
}

} // namespace bin2llvmir
} // namespace retdec
