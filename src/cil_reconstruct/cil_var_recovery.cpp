/**
 * @file src/cil_reconstruct/cil_var_recovery.cpp
 * @brief CIL variable recovery.
 */

#include "retdec/cil_reconstruct/cil_var_recovery.h"

#include <algorithm>
#include <cassert>

namespace retdec {
namespace cil_reconstruct {

// ─── CilVarRecovery ───────────────────────────────────────────────────────────

CilVarRecovery::CilVarRecovery(const Options& opts) : opts_(opts) {}

// ─── buildLocals ─────────────────────────────────────────────────────────────

std::vector<CilLocalVar> CilVarRecovery::buildLocals(
        const BcMethod& method,
        const CilStackSimulator& sim,
        const BcCFG& cfg) const {
    std::vector<CilLocalVar> locals;
    locals.resize(method.locals.size());

    for (size_t i = 0; i < method.locals.size(); ++i) {
        CilLocalVar& v = locals[i];
        v.index = static_cast<uint32_t>(i);
        v.name  = method.locals[i].name;
        v.type  = method.locals[i].type;
        if (v.name.empty())
            v.name = "loc" + std::to_string(i);
        v.isTemp = (v.name.find("CS$") == 0 ||
                    v.name.find("<>") == 0 ||
                    v.name.find("V_") == 0);
    }

    // Count def/use sites
    for (uint32_t bi = 0; bi < cfg.blockCount(); ++bi) {
        const auto& blk = cfg.block(bi);
        for (const auto& insn : blk.instrs) {
            uint32_t localIdx = UINT32_MAX;
            bool isDef = false, isUse = false;
            switch (insn.opcode) {
            case BcOpcode::DOTNET_STLOC_0: localIdx = 0; isDef = true; break;
            case BcOpcode::DOTNET_STLOC_1: localIdx = 1; isDef = true; break;
            case BcOpcode::DOTNET_STLOC_2: localIdx = 2; isDef = true; break;
            case BcOpcode::DOTNET_STLOC_3: localIdx = 3; isDef = true; break;
            case BcOpcode::DOTNET_LDLOC_0: localIdx = 0; isUse = true; break;
            case BcOpcode::DOTNET_LDLOC_1: localIdx = 1; isUse = true; break;
            case BcOpcode::DOTNET_LDLOC_2: localIdx = 2; isUse = true; break;
            case BcOpcode::DOTNET_LDLOC_3: localIdx = 3; isUse = true; break;
            case BcOpcode::DOTNET_STLOC_S: case BcOpcode::DOTNET_STLOC:
                isDef = true;
                for (const auto& op : insn.operands)
                    if (const auto* lo = std::get_if<BcLocalOperand>(&op)) { localIdx = lo->index; break; }
                break;
            case BcOpcode::DOTNET_LDLOC_S: case BcOpcode::DOTNET_LDLOC:
            case BcOpcode::DOTNET_LDLOCA_S: case BcOpcode::DOTNET_LDLOCA:
                isUse = true;
                for (const auto& op : insn.operands)
                    if (const auto* lo = std::get_if<BcLocalOperand>(&op)) { localIdx = lo->index; break; }
                break;
            default: break;
            }

            if (localIdx < locals.size()) {
                if (isDef) {
                    ++locals[localIdx].defCount;
                    locals[localIdx].defBlocks.push_back(bi);
                }
                if (isUse) {
                    ++locals[localIdx].useCount;
                    locals[localIdx].useBlocks.push_back(bi);
                }
            }
        }
    }

    computeInlineability(locals, cfg);
    return locals;
}

void CilVarRecovery::computeInlineability(
        std::vector<CilLocalVar>& locals,
        const BcCFG& cfg) const {
    if (!opts_.inlineTemps) return;

    for (auto& v : locals) {
        // Inline if: single def, single use, same block, not pinned
        if (v.defCount == 1 && v.useCount == 1 && !v.isPinned &&
            !v.defBlocks.empty() && !v.useBlocks.empty() &&
            v.defBlocks[0] == v.useBlocks[0]) {
            v.isInlineable = true;
        }
    }
}

// ─── buildParams ─────────────────────────────────────────────────────────────

std::vector<CilParam> CilVarRecovery::buildParams(const BcMethod& method) const {
    std::vector<CilParam> params;
    size_t numParams = method.descriptor.params.size();
    params.resize(numParams);
    for (size_t i = 0; i < numParams; ++i) {
        params[i].index = static_cast<uint32_t>(i);
        params[i].type  = *method.descriptor.params[i];
        if (i < method.paramNames.size())
            params[i].name = method.paramNames[i];
        else
            params[i].name = "arg" + std::to_string(i);
    }
    return params;
}

// ─── convertInsn ─────────────────────────────────────────────────────────────

std::vector<CilStmt> CilVarRecovery::convertInsn(
        const BcInstruction& insn,
        uint32_t blockId,
        uint32_t insnIdx,
        const CilStackSimulator& sim,
        std::vector<CilLocalVar>& locals,
        const std::vector<CilParam>& params,
        StackState& workStack) const {
    std::vector<CilStmt> stmts;

    auto getLocalName = [&](uint32_t idx) -> std::string {
        if (idx < locals.size()) return locals[idx].name;
        return "loc" + std::to_string(idx);
    };
    auto getParamName = [&](uint32_t idx) -> std::string {
        if (idx < params.size()) return params[idx].name;
        return "arg" + std::to_string(idx);
    };
    auto getLocalIdx = [&](uint32_t def) -> uint32_t {
        for (const auto& op : insn.operands)
            if (const auto* lo = std::get_if<BcLocalOperand>(&op)) return lo->index;
        return def;
    };
    auto getArgIdx = [&](uint32_t def) -> uint32_t {
        for (const auto& op : insn.operands)
            if (const auto* lo = std::get_if<BcLocalOperand>(&op)) return lo->index;
        return def;
    };

    // Get the expression that was computed just before this instruction
    // (the top of the simulated stack before applying this instruction)
    auto getTopExpr = [&]() -> CilExprPtr {
        if (insnIdx > 0)
            return sim.exprAt(blockId, insnIdx - 1);
        auto& es = sim.entryStack(blockId);
        return es.empty() ? nullptr : es.back().expr;
    };
    // Get post-instruction expression
    auto getOutExpr = [&]() -> CilExprPtr {
        return sim.exprAt(blockId, insnIdx);
    };

    switch (insn.opcode) {
    case BcOpcode::DOTNET_NOP:
        break; // no statement

    // Stores to locals
    case BcOpcode::DOTNET_STLOC_0: case BcOpcode::DOTNET_STLOC_1:
    case BcOpcode::DOTNET_STLOC_2: case BcOpcode::DOTNET_STLOC_3:
    case BcOpcode::DOTNET_STLOC_S: case BcOpcode::DOTNET_STLOC: {
        uint32_t idx = 0;
        switch (insn.opcode) {
        case BcOpcode::DOTNET_STLOC_0: idx = 0; break;
        case BcOpcode::DOTNET_STLOC_1: idx = 1; break;
        case BcOpcode::DOTNET_STLOC_2: idx = 2; break;
        case BcOpcode::DOTNET_STLOC_3: idx = 3; break;
        default: idx = getLocalIdx(0); break;
        }

        CilExprPtr rhs = getTopExpr();
        if (idx < locals.size() && locals[idx].isInlineable && rhs) {
            // Inline: don't emit assignment, record expr for later use
            break;
        }

        CilStmt s;
        if (idx < locals.size() && locals[idx].defCount == 1 && opts_.emitVarDecls) {
            // First (and only) store: emit LocalDecl
            s.kind = StmtKind::LocalDecl;
            s.declType = (idx < locals.size()) ? locals[idx].type : types::ClrObject();
            CilExprPtr lhs = makeExprLocal(idx, getLocalName(idx),
                idx < locals.size() ? locals[idx].type : types::ClrObject());
            s.target = lhs;
            s.expr   = rhs;
        } else {
            // Subsequent store: emit Assign
            s.kind = StmtKind::Assign;
            CilExprPtr lhs = makeExprLocal(idx, getLocalName(idx),
                idx < locals.size() ? locals[idx].type : types::ClrObject());
            s.target = lhs;
            s.expr   = rhs;
        }
        stmts.push_back(std::move(s));
        break;
    }

    // Stores to args
    case BcOpcode::DOTNET_STARG_S: case BcOpcode::DOTNET_STARG: {
        uint32_t idx = getArgIdx(0);
        CilStmt s;
        s.kind   = StmtKind::Assign;
        s.target = makeExprArg(idx, getParamName(idx),
            idx < params.size() ? params[idx].type : types::ClrObject());
        s.expr   = getTopExpr();
        stmts.push_back(std::move(s));
        break;
    }

    // Field stores
    case BcOpcode::DOTNET_STFLD: case BcOpcode::DOTNET_STSFLD: {
        CilStmt s;
        s.kind   = StmtKind::Assign;
        s.target = nullptr; // target is the field access expr (from sim)
        s.expr   = getOutExpr();
        stmts.push_back(std::move(s));
        break;
    }

    // Array element store
    case BcOpcode::DOTNET_STELEM: case BcOpcode::DOTNET_STELEM_REF:
    case BcOpcode::DOTNET_STELEM_I: case BcOpcode::DOTNET_STELEM_I1:
    case BcOpcode::DOTNET_STELEM_I2: case BcOpcode::DOTNET_STELEM_I4:
    case BcOpcode::DOTNET_STELEM_I8: case BcOpcode::DOTNET_STELEM_R4:
    case BcOpcode::DOTNET_STELEM_R8: {
        CilStmt s;
        s.kind = StmtKind::Assign;
        s.expr = getOutExpr();
        stmts.push_back(std::move(s));
        break;
    }

    // Returns
    case BcOpcode::DOTNET_RET: {
        CilStmt s;
        s.kind = StmtKind::Return;
        bool isVoid = !insn.operands.empty() ? false :
            (!insn.operands.empty()); // heuristic
        s.expr = getTopExpr();
        stmts.push_back(std::move(s));
        break;
    }

    // Throw
    case BcOpcode::DOTNET_THROW: {
        CilStmt s;
        s.kind = StmtKind::Throw;
        s.expr = getTopExpr();
        stmts.push_back(std::move(s));
        break;
    }
    case BcOpcode::DOTNET_RETHROW: {
        CilStmt s;
        s.kind = StmtKind::Rethrow;
        stmts.push_back(std::move(s));
        break;
    }

    // Branches
    case BcOpcode::DOTNET_BRFALSE: case BcOpcode::DOTNET_BRFALSE_S:
    case BcOpcode::DOTNET_BRTRUE:  case BcOpcode::DOTNET_BRTRUE_S: {
        CilStmt s;
        s.kind = StmtKind::If;
        s.expr = getTopExpr();
        // branch target
        for (const auto& op : insn.operands)
            if (const auto* bb = std::get_if<BcBlockOperand>(&op)) {
                s.blockRef = bb->blockId;
                break;
            }
        stmts.push_back(std::move(s));
        break;
    }

    case BcOpcode::DOTNET_BEQ:   case BcOpcode::DOTNET_BEQ_S:
    case BcOpcode::DOTNET_BNE_UN:case BcOpcode::DOTNET_BNE_UN_S:
    case BcOpcode::DOTNET_BGT:   case BcOpcode::DOTNET_BGT_S:
    case BcOpcode::DOTNET_BGE:   case BcOpcode::DOTNET_BGE_S:
    case BcOpcode::DOTNET_BLT:   case BcOpcode::DOTNET_BLT_S:
    case BcOpcode::DOTNET_BLE:   case BcOpcode::DOTNET_BLE_S:
    case BcOpcode::DOTNET_BGT_UN: case BcOpcode::DOTNET_BGT_UN_S:
    case BcOpcode::DOTNET_BGE_UN: case BcOpcode::DOTNET_BGE_UN_S:
    case BcOpcode::DOTNET_BLT_UN: case BcOpcode::DOTNET_BLT_UN_S:
    case BcOpcode::DOTNET_BLE_UN: case BcOpcode::DOTNET_BLE_UN_S: {
        CilStmt s;
        s.kind = StmtKind::If;
        s.expr = getOutExpr(); // comparison expression built during sim
        for (const auto& op : insn.operands)
            if (const auto* bb = std::get_if<BcBlockOperand>(&op)) {
                s.blockRef = bb->blockId;
                break;
            }
        stmts.push_back(std::move(s));
        break;
    }

    case BcOpcode::DOTNET_BR: case BcOpcode::DOTNET_BR_S: {
        CilStmt s;
        s.kind = StmtKind::Goto;
        for (const auto& op : insn.operands)
            if (const auto* bb = std::get_if<BcBlockOperand>(&op)) {
                s.blockRef = bb->blockId;
                s.labelName = "L" + std::to_string(bb->blockId);
                break;
            }
        stmts.push_back(std::move(s));
        break;
    }

    case BcOpcode::DOTNET_LEAVE: case BcOpcode::DOTNET_LEAVE_S: {
        CilStmt s;
        s.kind = StmtKind::Leave;
        for (const auto& op : insn.operands)
            if (const auto* bb = std::get_if<BcBlockOperand>(&op)) {
                s.blockRef = bb->blockId;
                s.labelName = "L" + std::to_string(bb->blockId);
                break;
            }
        stmts.push_back(std::move(s));
        break;
    }

    case BcOpcode::DOTNET_SWITCH: {
        CilStmt s;
        s.kind = StmtKind::Switch;
        s.expr = getTopExpr();
        stmts.push_back(std::move(s));
        break;
    }

    case BcOpcode::DOTNET_ENDFINALLY: {
        CilStmt s;
        s.kind = StmtKind::EndFinally;
        stmts.push_back(std::move(s));
        break;
    }
    case BcOpcode::DOTNET_ENDFILTER: {
        CilStmt s;
        s.kind = StmtKind::EndFilter;
        s.expr = getTopExpr();
        stmts.push_back(std::move(s));
        break;
    }

    // Calls that produce values are inline expressions (handled by subsequent stloc)
    // Void calls generate ExprStmt
    case BcOpcode::DOTNET_CALL:
    case BcOpcode::DOTNET_CALLVIRT:
    case BcOpcode::DOTNET_CALLI:
    case BcOpcode::DOTNET_TAIL_CALL: {
        // If the call is not immediately followed by a stloc/pop,
        // or if it returns void, emit ExprStmt
        CilStmt s;
        s.kind = StmtKind::ExprStmt;
        s.expr = getOutExpr();
        if (s.expr) stmts.push_back(std::move(s));
        break;
    }

    case BcOpcode::DOTNET_INITOBJ: {
        CilStmt s;
        s.kind = StmtKind::ExprStmt;
        // default(T) for the address
        s.expr = nullptr;
        stmts.push_back(std::move(s));
        break;
    }

    default:
        // All other instructions (loads, arithmetic, etc.) leave values on the
        // stack for consumption by later instructions — no statement needed.
        break;
    }

    return stmts;
}

// ─── convertBlock ────────────────────────────────────────────────────────────

std::vector<CilStmt> CilVarRecovery::convertBlock(
        const BcBasicBlock& block,
        const CilStackSimulator& sim,
        std::vector<CilLocalVar>& locals,
        const std::vector<CilParam>& params) const {
    std::vector<CilStmt> stmts;
    StackState workStack = sim.entryStack(block.id);

    for (uint32_t i = 0; i < block.instrs.size(); ++i) {
        auto insn_stmts = convertInsn(block.instrs[i], block.id, i,
                                       sim, locals, params, workStack);
        stmts.insert(stmts.end(), insn_stmts.begin(), insn_stmts.end());
    }

    return stmts;
}

// ─── recover ─────────────────────────────────────────────────────────────────

CilRecoveredMethod CilVarRecovery::recover(
        const BcCFG& cfg,
        const BcMethod& method,
        const CilStackSimulator& sim) const {
    CilRecoveredMethod result;
    result.method = &method;

    result.locals = buildLocals(method, sim, cfg);
    result.params = buildParams(method);

    // Convert each block
    result.blocks.resize(cfg.blockCount());
    for (uint32_t bi = 0; bi < cfg.blockCount(); ++bi) {
        const auto& blk = cfg.block(bi);
        CilRecoveredBlock& rb = result.blocks[bi];
        rb.id    = bi;
        rb.succs = blk.succs;
        rb.isEHEntry = blk.isExceptionHandler;
        rb.isLoop    = blk.isLoopHeader;

        rb.stmts = convertBlock(blk, sim, result.locals, result.params);
    }

    return result;
}

CilExprPtr CilVarRecovery::buildLocalLhs(uint32_t localIdx,
        const std::vector<CilLocalVar>& locals) const {
    if (localIdx < locals.size()) {
        return makeExprLocal(localIdx, locals[localIdx].name, locals[localIdx].type);
    }
    return makeExprLocal(localIdx, "loc" + std::to_string(localIdx), types::ClrObject());
}

CilExprPtr CilVarRecovery::buildArgLhs(uint32_t argIdx,
        const std::vector<CilParam>& params) const {
    if (argIdx < params.size()) {
        return makeExprArg(argIdx, params[argIdx].name, params[argIdx].type);
    }
    return makeExprArg(argIdx, "arg" + std::to_string(argIdx), types::ClrObject());
}

} // namespace cil_reconstruct
} // namespace retdec
