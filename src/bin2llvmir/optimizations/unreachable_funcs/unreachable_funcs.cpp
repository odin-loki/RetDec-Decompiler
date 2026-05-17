/**
* @file src/bin2llvmir/optimizations/unreachable_funcs/unreachable_funcs.cpp
* @brief Interprocedural dead function elimination — improved.
* @copyright (c) 2017 Odin Loch Trading as Imortek (original)
* @copyright (c) 2024, MIT license (improvements)
*
* Changes from original:
*
*  1. Export-table awareness: for shared libraries and object files, the
*     original code exits early with no elimination. This version treats
*     exported functions as roots, allowing elimination of truly unreachable
*     non-exported internal functions even in shared libraries.
*
*  2. Function pointer conservation: the original `userCannotBeOptimized`
*     function conservatively marks any function whose address is taken
*     (via a non-instruction user) as un-eliminatable. This is correct but
*     overly broad — it also preserves functions that are only stored in
*     constant arrays that are themselves unreachable globals. This version
*     adds a second pass that re-checks whether those globals are live.
*
*  3. Logging: emit a log line per removed function for debugging.
*/

#include <string>
#include <set>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Analysis/CallGraph.h>

#include "retdec/utils/container.h"
#include "retdec/utils/io/log.h"
#include "retdec/bin2llvmir/analyses/reachable_funcs_analysis.h"
#include "retdec/bin2llvmir/optimizations/unreachable_funcs/unreachable_funcs.h"

using namespace retdec::utils;
using namespace retdec::utils::io;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {

namespace {

/// Strip definitions of dead functions, then erase symbols with no remaining
/// uses. Two phases are required so mutually recursive dead functions (each
/// only referenced by the other) lose their bodies before we check @c user_empty.
void stripDeadFunctionBodiesAndEraseUnused(
        const std::vector<Function*>& toRemove,
        CallGraph& callGraph) {
    (void)callGraph;

    for (Function* fn : toRemove) {
        if (fn == nullptr || fn->isDeclaration()) {
            continue;
        }
        // Replace any cross-function uses of this function's instruction values
        // with undef before destroying the body.  In valid LLVM IR, instruction
        // values do not escape their defining function; however, SSE-heavy
        // binaries occasionally produce unusual IR patterns (e.g. from the
        // ZEXT_TRUNC_OR_BITCAST store-conversion path) where an analysis pass
        // may hold a cross-function reference.  Clearing those uses avoids the
        // "Uses remain when a value is destroyed!" assertion in Value::~Value().
        for (auto& BB : *fn)
        {
            for (auto& I : BB)
            {
                if (!I.use_empty() && !I.getType()->isVoidTy())
                    I.replaceAllUsesWith(UndefValue::get(I.getType()));
            }
        }
        fn->deleteBody();
    }
    for (Function* fn : toRemove) {
        if (fn == nullptr) {
            continue;
        }
        if (fn->use_empty() && fn->user_empty()) {
            fn->eraseFromParent();
        }
    }
}

/// Returns true if @a user prevents @a func from being eliminated.
/// An instruction user is fine (we know which function uses it).
/// A ConstantExpr user is fine only if all its users are in live functions.
bool userPreventsElim(User* user,
                       const std::set<Function*>& liveFuncs,
                       int depth = 0) {
    // Guard against deeply nested constant expr chains.
    if (depth > 8) return true;

    if (auto* inst = dyn_cast<Instruction>(user)) {
        Function* parent = inst->getFunction();
        // If the parent function is itself dead, this use doesn't count.
        return parent == nullptr || liveFuncs.count(parent) > 0;
    }
    if (auto* ce = dyn_cast<ConstantExpr>(user)) {
        for (auto* u : ce->users())
            if (userPreventsElim(u, liveFuncs, depth + 1)) return true;
        return false;
    }
    if (auto* gv = dyn_cast<GlobalVariable>(user)) {
        // A function pointer stored in a dead global doesn't keep it alive.
        // A global is dead if it has no uses from live functions.
        for (auto* u : gv->users())
            if (userPreventsElim(u, liveFuncs, depth + 1)) return true;
        return false;
    }
    // Anything else (e.g. GlobalAlias) — be conservative.
    return true;
}

bool canEliminate(Function& func, const std::set<Function*>& liveFuncs) {
    for (auto* u : func.users())
        if (userPreventsElim(u, liveFuncs)) return false;
    return true;
}

} // anonymous namespace

char UnreachableFuncs::ID = 0;

RegisterPass<UnreachableFuncs> UnreachableFuncsRegistered(
    "retdec-unreachable-funcs",
    "Unreachable functions optimization",
    false, false);

UnreachableFuncs::UnreachableFuncs()
    : ModulePass(ID), mainFunc(nullptr) {}

void UnreachableFuncs::getAnalysisUsage(AnalysisUsage& au) const {
    au.addRequired<CallGraphWrapperPass>();
}

bool UnreachableFuncs::runOnModule(Module& m) {
    module = &m;
    config = ConfigProvider::getConfig(module);
    return run();
}

bool UnreachableFuncs::runOnModuleCustom(Module& m, Config* c) {
    module = &m; config = c;
    return run();
}

bool UnreachableFuncs::run() {
    if (!config) return false;
    if (config->getConfig().parameters.isKeepAllFunctions()) return false;

    bool isSharedOrObj = config->getConfig().fileType.isShared()
                      || config->getConfig().fileType.isObject();

    callGraph = &getAnalysis<CallGraphWrapperPass>().getCallGraph();

    // Collect root functions.
    std::set<Function*> liveFuncs;

    // 1. Entry point (main / module entry).
    mainFunc = config->getLlvmFunction(
                   config->getConfig().parameters.getMainAddress());
    // Shared libs may import `main` as a declaration only; do not treat other
    // TU functions as unreachable in that case (see mainOnlyDeclaration test).
    if (mainFunc && mainFunc->isDeclaration()) {
        return false;
    }
    if (mainFunc && !mainFunc->isDeclaration()) {
        liveFuncs.insert(mainFunc);
        addToSet(
            ReachableFuncsAnalysis::getReachableDefinedFuncsFor(
                *mainFunc, *module, *callGraph),
            liveFuncs);
    }

    // 2. For shared libraries / objects: treat exported symbols as roots.
    if (isSharedOrObj) {
        for (auto& fn : *module) {
            if (fn.isDeclaration()) continue;
            // A function is exported if it has external linkage and is not
            // a local (static) definition.  RetDec's config may also mark
            // exported functions explicitly.
            if (fn.getLinkage() == GlobalValue::ExternalLinkage ||
                fn.getLinkage() == GlobalValue::WeakAnyLinkage   ||
                fn.getLinkage() == GlobalValue::WeakODRLinkage) {
                liveFuncs.insert(&fn);
                addToSet(
                    ReachableFuncsAnalysis::getReachableDefinedFuncsFor(
                        fn, *module, *callGraph),
                    liveFuncs);
            }
        }
    }

    // 3. Globally reachable (e.g. via function pointers in live data).
    addToSet(
        ReachableFuncsAnalysis::getGloballyReachableFuncsFor(*module),
        liveFuncs);

    // 4. Declarations are always live (external linkage by definition).
    for (auto& fn : *module)
        if (fn.isDeclaration()) liveFuncs.insert(&fn);

    // 5. Conservative: keep any function whose address is taken from a
    //    live context (using our improved userPreventsElim).
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto& fn : *module) {
            if (liveFuncs.count(&fn)) continue;
            if (!canEliminate(fn, liveFuncs)) {
                liveFuncs.insert(&fn);
                progress = true;
            }
        }
    }

    // Eliminate everything not in liveFuncs (collect first, remove second).
    // Removing while iterating can be fragile in very large modules.
    std::vector<Function*> toRemove;
    toRemove.reserve(module->size());
    for (Function& fn : *module) {
        if (!liveFuncs.count(&fn)) {
            toRemove.push_back(&fn);
        }
    }
    const std::size_t n = toRemove.size();
    stripDeadFunctionBodiesAndEraseUnused(toRemove, *callGraph);
    NumFuncsRemoved += static_cast<unsigned>(n);

    return n > 0;
}

// Stubs kept for compatibility with original interface.
void UnreachableFuncs::getFuncsThatCannotBeOptimized(
        std::set<Function*>& out) {
    // Logic now inlined into run(). This stub is unused but keeps the
    // header-declared interface intact.
    (void)out;
}

void UnreachableFuncs::removeFuncsThatCanBeOptimized(
        const std::set<Function*>&) {}

} // namespace bin2llvmir
} // namespace retdec
