/**
* @file src/bin2llvmir/optimizations/jump_table_recovery/jump_table_recovery.cpp
* @brief Recover switch statements from indirect jump-table branches.
* @copyright (c) 2024, MIT license
*/

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include "retdec/bin2llvmir/optimizations/jump_table_recovery/jump_table_recovery.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/utils/io/log.h"

using namespace llvm;
using namespace retdec::utils::io;

namespace retdec {
namespace bin2llvmir {

namespace {

/// When set (non-empty and not "0"), emit warnings for jump-table recovery
/// heuristics (design invariant: approximations must be visible). Same family
/// as RETDEC_PARAM_RETURN_TRACE in param_return.
bool jumpTableHeuristicDiagEnabled()
{
	const char* env = std::getenv("RETDEC_HEURISTIC_DIAG");
	return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

} // namespace

char JumpTableRecovery::ID = 0;

static RegisterPass<JumpTableRecovery> X(
    "retdec-jump-table-recovery",
    "Recover switch statements from indirect jump tables",
    false, false
);

JumpTableRecovery::JumpTableRecovery() : ModulePass(ID) {}

bool JumpTableRecovery::runOnModule(Module& m) {
    _module = &m;
    _abi    = AbiProvider::getAbi(_module);
    _config = ConfigProvider::getConfig(_module);
    _image  = FileImageProvider::getFileImage(_module);
    return run();
}

bool JumpTableRecovery::runOnModuleCustom(Module& m, Abi* abi, Config* config,
                                           FileImage* image) {
    _module = &m; _abi = abi; _config = config; _image = image;
    return run();
}

bool JumpTableRecovery::run() {
    _statIndirectBrCandidates = 0;
    _statAnalyzeDispatchFail = 0;
    _statReadTableFail = 0;
    _statRecovered = 0;
    _statIbrSuccessorMismatch = 0;
    _statDefaultCaseFallback = 0;

    bool changed = false;
    for (auto& fn : *_module) {
        if (!fn.isDeclaration())
            changed |= tryRecoverFunction(fn);
    }

    if (jumpTableHeuristicDiagEnabled() && _statIndirectBrCandidates > 0) {
        Log::error() << Log::Warning << "[JumpTableRecovery] heuristic: module "
                     << "summary indirectbr_candidates=" << _statIndirectBrCandidates
                     << " analyze_dispatch_fail=" << _statAnalyzeDispatchFail
                     << " read_table_fail=" << _statReadTableFail
                     << " recovered_switch=" << _statRecovered
                     << " ibr_succ_mismatch=" << _statIbrSuccessorMismatch
                     << " default_case_fallback=" << _statDefaultCaseFallback
                     << "\n";
    }
    return changed;
}

bool JumpTableRecovery::tryRecoverFunction(Function& fn) {
    // Collect IndirectBrInsts first to avoid iterator invalidation.
    std::vector<IndirectBrInst*> ibrs;
    for (auto& bb : fn)
        if (auto* ibr = dyn_cast<IndirectBrInst>(bb.getTerminator()))
            ibrs.push_back(ibr);

    bool changed = false;
    for (auto* ibr : ibrs) {
        ++_statIndirectBrCandidates;
        changed |= tryRecoverIndirectBr(ibr);
    }
    return changed;
}

bool JumpTableRecovery::tryRecoverIndirectBr(IndirectBrInst* ibr) {
    Value*          switchVal = nullptr;
    int64_t         baseVal   = 0;
    uint64_t        numCases  = 0;
    GlobalVariable* tableGV   = nullptr;

    if (!analyzeDispatch(ibr, switchVal, baseVal, numCases, tableGV)) {
        ++_statAnalyzeDispatchFail;
        return false;
    }

    Function& fn = *ibr->getParent()->getParent();
    std::vector<BasicBlock*> targets;
    if (!readTable(tableGV, numCases, fn, targets))
    {
        ++_statReadTableFail;
        if (jumpTableHeuristicDiagEnabled())
        {
            Log::error() << Log::Warning << "[JumpTableRecovery] heuristic: "
                         << "readTable failed (could not map constant table "
                         << "entries to basic blocks) in " << fn.getName().str()
                         << "\n";
        }
        return false;
    }

    if (targets.empty()) return false;

    // indirectbr lists every possible successor; dense jump tables usually match.
    if (ibr->getNumDestinations() != targets.size()) {
        ++_statIbrSuccessorMismatch;
        Log::debug() << "[JumpTableRecovery] indirectbr successor count "
                     << ibr->getNumDestinations() << " != table slots "
                     << targets.size() << " in " << fn.getName().str()
                     << " (still recovering)\n";
        if (jumpTableHeuristicDiagEnabled()) {
            Log::error() << Log::Warning << "[JumpTableRecovery] heuristic: "
                         << "indirectbr successor count "
                         << ibr->getNumDestinations() << " != table slots "
                         << targets.size() << " in " << fn.getName().str()
                         << " (recovery still applied)\n";
        }
    }

    // Find the default block: the predecessor branch that bypasses the
    // dispatch via a bounds-check. Walk up from the dispatch block.
    // Heuristic: the block has exactly two preds, one of which is the
    // bounds-check block. Default = the other successor of the bounds check.
    BasicBlock* dispatchBB = ibr->getParent();
    BasicBlock* defaultBB  = nullptr;

    std::unordered_map<BasicBlock*, unsigned> defaultVotes;
    for (auto* pred : predecessors(dispatchBB)) {
        auto* br = dyn_cast<BranchInst>(pred->getTerminator());
        if (!br || !br->isConditional()) continue;
        BasicBlock* cand = nullptr;
        if (br->getSuccessor(0) == dispatchBB)
            cand = br->getSuccessor(1);
        else if (br->getSuccessor(1) == dispatchBB)
            cand = br->getSuccessor(0);
        if (cand) ++defaultVotes[cand];
    }
    unsigned bestVotes = 0;
    for (const auto& p : defaultVotes) {
        if (p.second > bestVotes) {
            bestVotes = p.second;
            defaultBB   = p.first;
        }
    }
    if (!defaultBB) {
        ++_statDefaultCaseFallback;
        defaultBB = targets[0]; // fallback
        Log::debug() << "[JumpTableRecovery] default case inferred as first target "
                     << "(no bounds-check pred) in " << fn.getName().str() << "\n";
        if (jumpTableHeuristicDiagEnabled()) {
            Log::error() << Log::Warning << "[JumpTableRecovery] heuristic: "
                         << "default switch case assumed to first table target "
                         << "(no bounds-check predecessor) in "
                         << fn.getName().str() << "\n";
        }
    }

    // Build the switch.
    IRBuilder<> builder(ibr);

    // Adjust switchVal by baseVal if needed.
    Value* switchKey = switchVal;
    if (baseVal != 0) {
        switchKey = builder.CreateSub(
            switchVal,
            ConstantInt::get(switchVal->getType(), baseVal),
            "jtbl_idx");
    }

    SwitchInst* sw = builder.CreateSwitch(switchKey, defaultBB,
                                           static_cast<unsigned>(numCases));
    for (uint64_t i = 0; i < targets.size(); ++i) {
        sw->addCase(
            ConstantInt::get(cast<IntegerType>(switchKey->getType()), i),
            targets[i]);
    }

    // Remove the indirectbr.
    ibr->eraseFromParent();

    ++_statRecovered;
    Log::info() << "[JumpTableRecovery] Recovered switch with "
                << targets.size() << " cases in "
                << fn.getName().str() << "\n";
    return true;
}

bool JumpTableRecovery::analyzeDispatch(IndirectBrInst*  ibr,
                                         Value*&          switchVal,
                                         int64_t&         baseVal,
                                         uint64_t&        numCases,
                                         GlobalVariable*& tableGV) {
    // Pattern:
    //   %addr = load …, …* %ptr
    //   indirectbr … %addr, [...]
    // or integer tables:
    //   %w = load i64, i64* %ptr
    //   %addr = inttoptr i64 %w to i8*
    //   indirectbr i8* %addr, [...]
    // where %ptr = GEP(tableGV, 0, %idx)

    Value* ptrVal = ibr->getAddress();
    if (auto* ip = dyn_cast<IntToPtrInst>(ptrVal)) {
        ptrVal = ip->getOperand(0);
    }
    if (auto* bc = dyn_cast<BitCastInst>(ptrVal)) {
        ptrVal = bc->getOperand(0);
    }
    auto* addr = dyn_cast<LoadInst>(ptrVal);
    if (!addr) return false;

    auto* gep = dyn_cast<GEPOperator>(addr->getPointerOperand());
    if (!gep) return false;
    // Reject fully constant GEPs — not a dynamic table dispatch.
    if (gep->hasAllConstantIndices()) return false;

    Value* tablePtr = gep->getPointerOperand();
    if (auto* ce = dyn_cast<ConstantExpr>(tablePtr)) {
        if (ce->getOpcode() == Instruction::BitCast ||
            ce->getOpcode() == Instruction::AddrSpaceCast) {
            tablePtr = ce->getOperand(0);
        }
    }
    // Peel (ptr + const) so the table global is reachable (e.g. tagged base offsets).
    if (auto* bo = dyn_cast<BinaryOperator>(tablePtr)) {
        if (bo->getOpcode() == Instruction::Add && bo->getType()->isPointerTy()) {
            if (isa<Constant>(bo->getOperand(1)) && !isa<UndefValue>(bo->getOperand(1))) {
                tablePtr = bo->getOperand(0);
            } else if (isa<Constant>(bo->getOperand(0))
                    && !isa<UndefValue>(bo->getOperand(0))) {
                tablePtr = bo->getOperand(1);
            }
        }
    }
    // Peel no-op pointer casts (bitcast / addrspacecast chains on the GEP base).
    for (;;) {
        Value* stripped = tablePtr->stripPointerCasts();
        if (stripped == tablePtr) break;
        tablePtr = stripped;
    }
    tableGV = dyn_cast<GlobalVariable>(tablePtr);
    if (!tableGV || !tableGV->isConstant()) return false;
    if (!tableGV->hasInitializer()) return false;

    auto* arrTy = dyn_cast<ArrayType>(tableGV->getInitializer()->getType());
    if (!arrTy) return false;

    numCases = arrTy->getNumElements();
    if (numCases == 0 || numCases > 4096) return false; // sanity

    // The GEP's last index is the switch variable (after normalisation).
    // Walk back through sub/add to find base and original value.
    Value* idx = gep->getOperand(gep->getNumOperands() - 1);
    baseVal = 0;

    if (auto* sub = dyn_cast<BinaryOperator>(idx)) {
        if (sub->getOpcode() == Instruction::Sub) {
            if (auto* ci = dyn_cast<ConstantInt>(sub->getOperand(1))) {
                baseVal   = ci->getSExtValue();
                switchVal = sub->getOperand(0);
                return true;
            }
        }
        if (sub->getOpcode() == Instruction::Add) {
            if (auto* ci = dyn_cast<ConstantInt>(sub->getOperand(1))) {
                baseVal   = -ci->getSExtValue();
                switchVal = sub->getOperand(0);
                return true;
            }
        }
    }

    switchVal = idx;
    return true;
}

bool JumpTableRecovery::readTable(GlobalVariable*          tableGV,
                                   uint64_t                 numCases,
                                   Function&                fn,
                                   std::vector<BasicBlock*>& targets) {
    Constant* initC = tableGV->getInitializer();
    auto*     arrTy = dyn_cast<ArrayType>(initC->getType());
    if (!arrTy) return false;
    const unsigned nElts = arrTy->getNumElements();
    if (nElts == 0 || numCases > nElts) return false;

    // Map insn.addr (any instruction) → its basic block so table targets can
    // match the entry PC of a block or a lifted instruction inside it.
    std::map<uint64_t, BasicBlock*> addrToBB;
    for (auto& bb : fn) {
        for (auto& inst : bb) {
            if (auto* mdn = inst.getMetadata("insn.addr")) {
                auto* ci = mdconst::dyn_extract<ConstantInt>(mdn->getOperand(0));
                if (ci) addrToBB[ci->getZExtValue()] = &bb;
            }
        }
    }

    for (uint64_t i = 0; i < numCases; ++i) {
        Constant* entry = nullptr;
        if (auto* ca = dyn_cast<ConstantArray>(initC)) {
            entry = ca->getOperand(static_cast<unsigned>(i));
        } else if (auto* cda = dyn_cast<ConstantDataArray>(initC)) {
            entry = cda->getElementAsConstant(static_cast<unsigned>(i));
        } else {
            return false;
        }

        // Entry is typically a BlockAddress or an inttoptr(const).
        if (auto* ba = dyn_cast<BlockAddress>(entry)) {
            targets.push_back(ba->getBasicBlock());
            continue;
        }

        // Try to resolve as a constant integer address.
        uint64_t addr = 0;
        if (auto* ci = dyn_cast<ConstantInt>(entry)) {
            addr = ci->getZExtValue();
        } else if (auto* ce = dyn_cast<ConstantExpr>(entry)) {
            if (ce->getOpcode() == Instruction::IntToPtr ||
                ce->getOpcode() == Instruction::BitCast) {
                if (auto* ci = dyn_cast<ConstantInt>(ce->getOperand(0)))
                    addr = ci->getZExtValue();
            }
        }

        if (addr == 0) return false;

        auto it = addrToBB.find(addr);
        if (it == addrToBB.end()) {
            // Fallback: use the nearest lower insn.addr (within 64 bytes).
            auto jt = addrToBB.upper_bound(addr);
            if (jt == addrToBB.begin()) return false;
            --jt;
            const uint64_t delta = addr - jt->first;
            if (delta > 64) return false;
            targets.push_back(jt->second);
            continue;
        }
        targets.push_back(it->second);
    }

    return !targets.empty();
}

} // namespace bin2llvmir
} // namespace retdec
