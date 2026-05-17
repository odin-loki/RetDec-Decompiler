/**
 * @file src/cil_reconstruct/cil_stack_sim.cpp
 * @brief CIL typed stack simulation.
 */

#include <memory>
#include "retdec/cil_reconstruct/cil_stack_sim.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace retdec {
namespace cil_reconstruct {

// ─── CilStackSimulator ────────────────────────────────────────────────────────

CilStackSimulator::CilStackSimulator(const Options& opts) : opts_(opts) {}

const BlockStackInfo& CilStackSimulator::blockInfo(uint32_t blockId) const {
    if (blockId >= blockInfos_.size()) {
        static BlockStackInfo empty;
        return empty;
    }
    return blockInfos_[blockId];
}

const StackState& CilStackSimulator::entryStack(uint32_t blockId) const {
    return blockInfo(blockId).entryStack;
}

const StackState& CilStackSimulator::instrStack(uint32_t blockId, uint32_t instrIdx) const {
    const auto& info = blockInfo(blockId);
    if (instrIdx >= info.instrStacks.size()) {
        return info.exitStack;
    }
    return info.instrStacks[instrIdx];
}

CilExprPtr CilStackSimulator::exprAt(uint32_t blockId, uint32_t instrIdx) const {
    const auto& state = instrStack(blockId, instrIdx);
    if (state.empty()) return nullptr;
    return state.back().expr;
}

// ─── Stack helpers ────────────────────────────────────────────────────────────

bool CilStackSimulator::pop(StackState& s, StackSlot& out) {
    if (s.empty()) return false;
    out = std::move(s.back());
    s.pop_back();
    return true;
}

bool CilStackSimulator::popN(StackState& s, int n, std::vector<StackSlot>& out) {
    if (static_cast<int>(s.size()) < n) return false;
    out.resize(n);
    for (int i = n - 1; i >= 0; --i) {
        out[i] = std::move(s.back());
        s.pop_back();
    }
    return true;
}

void CilStackSimulator::push(StackState& s, BcType t, CilExprPtr expr) {
    StackSlot slot;
    slot.type = std::move(t);
    slot.expr = std::move(expr);
    s.push_back(std::move(slot));
}

// ─── Type unification ─────────────────────────────────────────────────────────

BcType CilStackSimulator::unifyTypes(const BcType& a, const BcType& b) {
    if (a == b) return a;
    // Widen to System.Object for mismatched reference types
    if (a.isRef() && b.isRef()) return types::ClrObject();
    // Mixed primitive/ref: use Object
    return types::ClrObject();
}

StackState CilStackSimulator::meetStates(const StackState& a, const StackState& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.size() != b.size()) {
        // Stack depth mismatch — use the non-empty one (fallback)
        return a;
    }
    StackState result(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        result[i].type = unifyTypes(a[i].type, b[i].type);
        result[i].expr = a[i].expr; // Keep first predecessor's expr
    }
    return result;
}

// ─── Static stack effects ─────────────────────────────────────────────────────

CilStackSimulator::StackEffect CilStackSimulator::getStaticEffect(BcOpcode op) {
    using O = BcOpcode;
    switch (op) {
    // 0-pop, 0-push
    case O::DOTNET_NOP: case O::DOTNET_BREAK:
    case O::DOTNET_ENDFINALLY: case O::DOTNET_ENDFILTER:
        return {0, 0};
    // 0-pop, 1-push
    case O::DOTNET_LDARG_0: case O::DOTNET_LDARG_1:
    case O::DOTNET_LDARG_2: case O::DOTNET_LDARG_3:
    case O::DOTNET_LDARG_S: case O::DOTNET_LDARG:
    case O::DOTNET_LDLOC_0: case O::DOTNET_LDLOC_1:
    case O::DOTNET_LDLOC_2: case O::DOTNET_LDLOC_3:
    case O::DOTNET_LDLOC_S: case O::DOTNET_LDLOC:
    case O::DOTNET_LDNULL:
    case O::DOTNET_LDC_I4_M1: case O::DOTNET_LDC_I4_0: case O::DOTNET_LDC_I4_1:
    case O::DOTNET_LDC_I4_2:  case O::DOTNET_LDC_I4_3: case O::DOTNET_LDC_I4_4:
    case O::DOTNET_LDC_I4_5:  case O::DOTNET_LDC_I4_6: case O::DOTNET_LDC_I4_7:
    case O::DOTNET_LDC_I4_8:
    case O::DOTNET_LDC_I4_S: case O::DOTNET_LDC_I4:
    case O::DOTNET_LDC_I8: case O::DOTNET_LDC_R4: case O::DOTNET_LDC_R8:
    case O::DOTNET_LDSTR:
    case O::DOTNET_LDSFLD: case O::DOTNET_LDSFLDA:
    case O::DOTNET_ARGLIST:
        return {0, 1};
    // 1-pop, 0-push (stores, branches, throw)
    case O::DOTNET_STARG_S: case O::DOTNET_STARG:
    case O::DOTNET_STLOC_0: case O::DOTNET_STLOC_1:
    case O::DOTNET_STLOC_2: case O::DOTNET_STLOC_3:
    case O::DOTNET_STLOC_S: case O::DOTNET_STLOC:
    case O::DOTNET_STSFLD:
    case O::DOTNET_BRFALSE: case O::DOTNET_BRFALSE_S:
    case O::DOTNET_BRTRUE:  case O::DOTNET_BRTRUE_S:
    case O::DOTNET_SWITCH:
    case O::DOTNET_THROW:
    case O::DOTNET_POP:
    case O::DOTNET_STIND_REF: case O::DOTNET_STIND_I:
    case O::DOTNET_STIND_I1: case O::DOTNET_STIND_I2:
    case O::DOTNET_STIND_I4: case O::DOTNET_STIND_I8:
    case O::DOTNET_STIND_R4: case O::DOTNET_STIND_R8:
        return {1, 0};
    // 1-pop, 1-push (unary, conversions, address-of, etc.)
    case O::DOTNET_NEG: case O::DOTNET_NOT:
    case O::DOTNET_CONV_I1: case O::DOTNET_CONV_U1: case O::DOTNET_CONV_I2:
    case O::DOTNET_CONV_U2: case O::DOTNET_CONV_I4: case O::DOTNET_CONV_U4:
    case O::DOTNET_CONV_I8: case O::DOTNET_CONV_U8: case O::DOTNET_CONV_R4:
    case O::DOTNET_CONV_R8: case O::DOTNET_CONV_I: case O::DOTNET_CONV_U:
    case O::DOTNET_CONV_R_UN:
    case O::DOTNET_CONV_OVF_I1: case O::DOTNET_CONV_OVF_U1:
    case O::DOTNET_CONV_OVF_I2: case O::DOTNET_CONV_OVF_U2:
    case O::DOTNET_CONV_OVF_I4: case O::DOTNET_CONV_OVF_U4:
    case O::DOTNET_CONV_OVF_I8: case O::DOTNET_CONV_OVF_U8:
    case O::DOTNET_CONV_OVF_I1_UN: case O::DOTNET_CONV_OVF_U1_UN:
    case O::DOTNET_CONV_OVF_I2_UN: case O::DOTNET_CONV_OVF_U2_UN:
    case O::DOTNET_CONV_OVF_I4_UN: case O::DOTNET_CONV_OVF_U4_UN:
    case O::DOTNET_CONV_OVF_I8_UN: case O::DOTNET_CONV_OVF_U8_UN:
    case O::DOTNET_CONV_OVF_I: case O::DOTNET_CONV_OVF_U:
    case O::DOTNET_CONV_OVF_I_UN: case O::DOTNET_CONV_OVF_U_UN:
    case O::DOTNET_CASTCLASS: case O::DOTNET_ISINST:
    case O::DOTNET_UNBOX: case O::DOTNET_UNBOX_ANY:
    case O::DOTNET_BOX:
    case O::DOTNET_LDLEN:
    case O::DOTNET_LDOBJ: case O::DOTNET_CPOBJ:
    case O::DOTNET_REFANYTYPE: case O::DOTNET_REFANYVAL:
    case O::DOTNET_CKFINITE:
    case O::DOTNET_LDIND_I1: case O::DOTNET_LDIND_U1:
    case O::DOTNET_LDIND_I2: case O::DOTNET_LDIND_U2:
    case O::DOTNET_LDIND_I4: case O::DOTNET_LDIND_U4:
    case O::DOTNET_LDIND_I8: case O::DOTNET_LDIND_I:
    case O::DOTNET_LDIND_R4: case O::DOTNET_LDIND_R8: case O::DOTNET_LDIND_REF:
    case O::DOTNET_LDTOKEN:
    case O::DOTNET_LDVIRTFTN:
    case O::DOTNET_LDARGA_S: case O::DOTNET_LDARGA:
    case O::DOTNET_LDLOCA_S: case O::DOTNET_LDLOCA:
        return {1, 1};
    // 2-pop, 1-push (binary ops)
    case O::DOTNET_ADD: case O::DOTNET_SUB: case O::DOTNET_MUL:
    case O::DOTNET_DIV: case O::DOTNET_DIV_UN:
    case O::DOTNET_REM: case O::DOTNET_REM_UN:
    case O::DOTNET_AND: case O::DOTNET_OR: case O::DOTNET_XOR:
    case O::DOTNET_SHL: case O::DOTNET_SHR: case O::DOTNET_SHR_UN:
    case O::DOTNET_ADD_OVF: case O::DOTNET_ADD_OVF_UN:
    case O::DOTNET_MUL_OVF: case O::DOTNET_MUL_OVF_UN:
    case O::DOTNET_SUB_OVF: case O::DOTNET_SUB_OVF_UN:
    case O::DOTNET_CEQ: case O::DOTNET_CGT: case O::DOTNET_CGT_UN:
    case O::DOTNET_CLT: case O::DOTNET_CLT_UN:
    case O::DOTNET_BEQ: case O::DOTNET_BEQ_S:
    case O::DOTNET_BGE: case O::DOTNET_BGE_S: case O::DOTNET_BGE_UN: case O::DOTNET_BGE_UN_S:
    case O::DOTNET_BGT: case O::DOTNET_BGT_S: case O::DOTNET_BGT_UN: case O::DOTNET_BGT_UN_S:
    case O::DOTNET_BLE: case O::DOTNET_BLE_S: case O::DOTNET_BLE_UN: case O::DOTNET_BLE_UN_S:
    case O::DOTNET_BLT: case O::DOTNET_BLT_S: case O::DOTNET_BLT_UN: case O::DOTNET_BLT_UN_S:
    case O::DOTNET_BNE_UN: case O::DOTNET_BNE_UN_S:
        return {2, 1};
    // dup: 0-pop, 1-push (technically duplicates top, so 1 consumed + 2 pushed = net +1)
    case O::DOTNET_DUP:
        return {0, 1}; // we handle dup specially
    // 2-pop 0-push
    case O::DOTNET_STFLD: case O::DOTNET_STOBJ:
    case O::DOTNET_STELEM_REF: case O::DOTNET_STELEM_I:
    case O::DOTNET_STELEM_I1: case O::DOTNET_STELEM_I2:
    case O::DOTNET_STELEM_I4: case O::DOTNET_STELEM_I8:
    case O::DOTNET_STELEM_R4: case O::DOTNET_STELEM_R8:
    case O::DOTNET_INITBLK: case O::DOTNET_CPBLK:
        return {2, 0};
    // 1-pop 1-push (ldfld: pop obj, push field value)
    case O::DOTNET_LDFLD: case O::DOTNET_LDFLDA:
    case O::DOTNET_LDFTN: case O::DOTNET_LOCALLOC:
    case O::DOTNET_SIZEOF:
        return {1, 1};
    // 2-pop 1-push (ldelem)
    case O::DOTNET_LDELEM: case O::DOTNET_LDELEM_I1: case O::DOTNET_LDELEM_U1:
    case O::DOTNET_LDELEM_I2: case O::DOTNET_LDELEM_U2:
    case O::DOTNET_LDELEM_I4: case O::DOTNET_LDELEM_U4:
    case O::DOTNET_LDELEM_I8: case O::DOTNET_LDELEM_I:
    case O::DOTNET_LDELEM_R4: case O::DOTNET_LDELEM_R8: case O::DOTNET_LDELEM_REF:
    case O::DOTNET_LDELEMA:
        return {2, 1};
    // 3-pop 0-push (stelem with type)
    case O::DOTNET_STELEM:
        return {3, 0};
    // 1-pop 0-push (stfld requires obj+val = 2, but special)
    case O::DOTNET_INITOBJ:
        return {1, 0};
    // 1-pop 1-push (newarr: pop length, push array)
    case O::DOTNET_NEWARR:
        return {1, 1};
    // mkrefany: 1-pop 1-push
    case O::DOTNET_MKREFANY:
        return {1, 1};
    // ret: variable (0-pop if void, 1-pop if non-void)
    case O::DOTNET_RET:
        return {-1, 0};
    // rethrow: 0-pop 0-push (terminates block)
    case O::DOTNET_RETHROW:
        return {0, 0};
    // call/callvirt/newobj/calli: variable
    case O::DOTNET_CALL: case O::DOTNET_CALLVIRT:
    case O::DOTNET_CALLI: case O::DOTNET_NEWOBJ:
    case O::DOTNET_TAIL_CALL:
        return {-1, -1};
    // unconditional branches: 0-pop 0-push
    case O::DOTNET_BR: case O::DOTNET_BR_S:
    case O::DOTNET_LEAVE: case O::DOTNET_LEAVE_S:
        return {0, 0};
    default:
        return {0, 0};
    }
}

// ─── Return type helpers ──────────────────────────────────────────────────────

BcType CilStackSimulator::methodReturnType(const BcInstruction& insn) {
    // If the instruction has a method operand, get return type
    for (const auto& op : insn.operands) {
        if (const auto* mr = std::get_if<BcMethodRef>(&op)) {
            (void)mr;
            // Without full type resolution, default to Object
            return types::ClrObject();
        }
    }
    return types::ClrObject();
}

BcType CilStackSimulator::fieldType(const BcInstruction& insn) {
    for (const auto& op : insn.operands) {
        if (const auto* fr = std::get_if<BcFieldRef>(&op)) {
            (void)fr;
            return types::ClrObject();
        }
    }
    return types::ClrObject();
}

// ─── Expression builders ──────────────────────────────────────────────────────

CilExprPtr CilStackSimulator::makeConst(int64_t v, BcType t) {
    ExprConst c; c.intVal = v; c.type = t;
    return std::make_shared<CilExpr>(std::move(c), t);
}

CilExprPtr CilStackSimulator::makeConst(double v) {
    ExprConst c; c.fltVal = v; c.isFloat = true; c.type = types::Double();
    return std::make_shared<CilExpr>(std::move(c), types::Double());
}

CilExprPtr CilStackSimulator::makeConst(const std::string& s) {
    ExprConst c; c.strVal = s; c.isString = true; c.type = types::ClrString();
    return std::make_shared<CilExpr>(std::move(c), types::ClrString());
}

CilExprPtr CilStackSimulator::makeNull() {
    return std::make_shared<CilExpr>(ExprNull{}, types::Null());
}

CilExprPtr CilStackSimulator::makeLocal(uint32_t idx, const std::string& name, BcType t) {
    return std::make_shared<CilExpr>(ExprLocal{idx, name, t}, t);
}

CilExprPtr CilStackSimulator::makeArg(uint32_t idx, const std::string& name, BcType t) {
    return std::make_shared<CilExpr>(ExprArg{idx, name, t}, t);
}

CilExprPtr CilStackSimulator::makeBinOp(BinOpKind op, CilExprPtr l, CilExprPtr r, BcType t) {
    ExprBinOp b{op, std::move(l), std::move(r), t};
    return std::make_shared<CilExpr>(std::move(b), t);
}

CilExprPtr CilStackSimulator::makeUnOp(UnOpKind op, CilExprPtr e, BcType t) {
    ExprUnOp u{op, std::move(e), t};
    return std::make_shared<CilExpr>(std::move(u), t);
}

CilExprPtr CilStackSimulator::makeCast(CilExprPtr e, BcType t) {
    ExprCast c{std::move(e), t};
    return std::make_shared<CilExpr>(std::move(c), t);
}

CilExprPtr CilStackSimulator::makeIsinst(CilExprPtr e, BcType t) {
    ExprIsinst ii{std::move(e), t};
    // isinst returns null or the cast object — same reference type
    return std::make_shared<CilExpr>(std::move(ii), t);
}

// ─── applyInstruction ─────────────────────────────────────────────────────────

bool CilStackSimulator::applyInstruction(
        const BcInstruction& insn,
        StackState& stack,
        const BcMethod& method,
        CilExprPtr& outExpr) const {
    using O = BcOpcode;
    outExpr = nullptr;

    auto getLocalType = [&](uint32_t idx) -> BcType {
        if (idx < method.locals.size()) return method.locals[idx].type;
        return types::ClrObject();
    };
    auto getLocalName = [&](uint32_t idx) -> std::string {
        if (idx < method.locals.size() && !method.locals[idx].name.empty())
            return method.locals[idx].name;
        return "loc" + std::to_string(idx);
    };
    auto getParamType = [&](uint32_t idx) -> BcType {
        if (!method.descriptor.params.empty() && idx < method.descriptor.params.size())
            return *method.descriptor.params[idx];
        return types::ClrObject();
    };
    auto getParamName = [&](uint32_t idx) -> std::string {
        if (idx < method.paramNames.size()) return method.paramNames[idx];
        return "arg" + std::to_string(idx);
    };

    auto getLocalIdx = [&](const BcInstruction& i, uint32_t defaultIdx) -> uint32_t {
        for (const auto& op : i.operands)
            if (const auto* lo = std::get_if<BcLocalOperand>(&op))
                return lo->index;
        return defaultIdx;
    };

    StackSlot s0, s1, s2;
    std::vector<StackSlot> args;

    switch (insn.opcode) {
    case O::DOTNET_NOP: case O::DOTNET_BREAK:
        break;

    // Load constant integers
    case O::DOTNET_LDC_I4_M1: push(stack, types::Int(), makeConst(-1, types::Int())); break;
    case O::DOTNET_LDC_I4_0:  push(stack, types::Int(), makeConst(0,  types::Int())); break;
    case O::DOTNET_LDC_I4_1:  push(stack, types::Int(), makeConst(1,  types::Int())); break;
    case O::DOTNET_LDC_I4_2:  push(stack, types::Int(), makeConst(2,  types::Int())); break;
    case O::DOTNET_LDC_I4_3:  push(stack, types::Int(), makeConst(3,  types::Int())); break;
    case O::DOTNET_LDC_I4_4:  push(stack, types::Int(), makeConst(4,  types::Int())); break;
    case O::DOTNET_LDC_I4_5:  push(stack, types::Int(), makeConst(5,  types::Int())); break;
    case O::DOTNET_LDC_I4_6:  push(stack, types::Int(), makeConst(6,  types::Int())); break;
    case O::DOTNET_LDC_I4_7:  push(stack, types::Int(), makeConst(7,  types::Int())); break;
    case O::DOTNET_LDC_I4_8:  push(stack, types::Int(), makeConst(8,  types::Int())); break;
    case O::DOTNET_LDC_I4_S:
    case O::DOTNET_LDC_I4: {
        int64_t v = 0;
        for (const auto& op : insn.operands)
            if (const auto* io = std::get_if<BcIntOperand>(&op)) { v = io->value; break; }
        push(stack, types::Int(), makeConst(v, types::Int()));
        break;
    }
    case O::DOTNET_LDC_I8: {
        int64_t v = 0;
        for (const auto& op : insn.operands)
            if (const auto* io = std::get_if<BcIntOperand>(&op)) { v = io->value; break; }
        push(stack, types::Long(), makeConst(v, types::Long()));
        break;
    }
    case O::DOTNET_LDC_R4: case O::DOTNET_LDC_R8:
        push(stack, types::Double(), makeConst(0.0));
        break;

    case O::DOTNET_LDNULL:
        push(stack, types::Null(), makeNull());
        break;

    case O::DOTNET_LDSTR: {
        std::string sv;
        for (const auto& op : insn.operands)
            if (const auto* so = std::get_if<BcStringOperand>(&op)) { sv = so->value; break; }
        push(stack, types::ClrString(), makeConst(sv));
        break;
    }

    // Load locals
    case O::DOTNET_LDLOC_0: { auto t = getLocalType(0); push(stack, t, makeLocal(0, getLocalName(0), t)); break; }
    case O::DOTNET_LDLOC_1: { auto t = getLocalType(1); push(stack, t, makeLocal(1, getLocalName(1), t)); break; }
    case O::DOTNET_LDLOC_2: { auto t = getLocalType(2); push(stack, t, makeLocal(2, getLocalName(2), t)); break; }
    case O::DOTNET_LDLOC_3: { auto t = getLocalType(3); push(stack, t, makeLocal(3, getLocalName(3), t)); break; }
    case O::DOTNET_LDLOC_S:
    case O::DOTNET_LDLOC: {
        uint32_t idx = getLocalIdx(insn, 0);
        auto t = getLocalType(idx);
        push(stack, t, makeLocal(idx, getLocalName(idx), t));
        break;
    }
    case O::DOTNET_LDLOCA_S:
    case O::DOTNET_LDLOCA: {
        uint32_t idx = getLocalIdx(insn, 0);
        auto t = getLocalType(idx);
        auto inner = makeLocal(idx, getLocalName(idx), t);
        ExprAddressOf ao{inner};
        push(stack, types::Long(), std::make_shared<CilExpr>(std::move(ao), types::Long()));
        break;
    }

    // Store locals
    case O::DOTNET_STLOC_0: case O::DOTNET_STLOC_1:
    case O::DOTNET_STLOC_2: case O::DOTNET_STLOC_3:
    case O::DOTNET_STLOC_S: case O::DOTNET_STLOC:
        if (!pop(stack, s0)) return false;
        outExpr = s0.expr; // the stored value is the "expression" of this stmt
        break;

    // Load args
    case O::DOTNET_LDARG_0: { auto t = getParamType(0); push(stack, t, makeArg(0, getParamName(0), t)); break; }
    case O::DOTNET_LDARG_1: { auto t = getParamType(1); push(stack, t, makeArg(1, getParamName(1), t)); break; }
    case O::DOTNET_LDARG_2: { auto t = getParamType(2); push(stack, t, makeArg(2, getParamName(2), t)); break; }
    case O::DOTNET_LDARG_3: { auto t = getParamType(3); push(stack, t, makeArg(3, getParamName(3), t)); break; }
    case O::DOTNET_LDARG_S:
    case O::DOTNET_LDARG: {
        uint32_t idx = getLocalIdx(insn, 0);
        auto t = getParamType(idx);
        push(stack, t, makeArg(idx, getParamName(idx), t));
        break;
    }
    case O::DOTNET_LDARGA_S:
    case O::DOTNET_LDARGA: {
        uint32_t idx = getLocalIdx(insn, 0);
        auto t = getParamType(idx);
        auto inner = makeArg(idx, getParamName(idx), t);
        ExprAddressOf ao{inner};
        push(stack, types::Long(), std::make_shared<CilExpr>(std::move(ao), types::Long()));
        break;
    }
    case O::DOTNET_STARG_S:
    case O::DOTNET_STARG:
        if (!pop(stack, s0)) return false;
        break;

    // Dup: peek top
    case O::DOTNET_DUP: {
        if (stack.empty()) return false;
        auto top = stack.back();
        if (top.expr) {
            ExprDup dup{top.expr};
            top.expr = std::make_shared<CilExpr>(std::move(dup), top.type);
        }
        stack.push_back(std::move(top));
        break;
    }
    case O::DOTNET_POP:
        if (!pop(stack, s0)) return false;
        break;

    // Binary arithmetic
    case O::DOTNET_ADD: case O::DOTNET_ADD_OVF: case O::DOTNET_ADD_OVF_UN:
    case O::DOTNET_SUB: case O::DOTNET_SUB_OVF: case O::DOTNET_SUB_OVF_UN:
    case O::DOTNET_MUL: case O::DOTNET_MUL_OVF: case O::DOTNET_MUL_OVF_UN:
    case O::DOTNET_DIV: case O::DOTNET_DIV_UN:
    case O::DOTNET_REM: case O::DOTNET_REM_UN:
    case O::DOTNET_AND: case O::DOTNET_OR:  case O::DOTNET_XOR:
    case O::DOTNET_SHL: case O::DOTNET_SHR: case O::DOTNET_SHR_UN:
    case O::DOTNET_CEQ: case O::DOTNET_CGT: case O::DOTNET_CGT_UN:
    case O::DOTNET_CLT: case O::DOTNET_CLT_UN: {
        if (!pop(stack, s1) || !pop(stack, s0)) return false;
        static const std::map<BcOpcode, BinOpKind> opMap{
            {O::DOTNET_ADD, BinOpKind::Add}, {O::DOTNET_SUB, BinOpKind::Sub},
            {O::DOTNET_MUL, BinOpKind::Mul}, {O::DOTNET_DIV, BinOpKind::Div},
            {O::DOTNET_DIV_UN, BinOpKind::DivUn}, {O::DOTNET_REM, BinOpKind::Rem},
            {O::DOTNET_AND, BinOpKind::And}, {O::DOTNET_OR, BinOpKind::Or},
            {O::DOTNET_XOR, BinOpKind::Xor}, {O::DOTNET_SHL, BinOpKind::Shl},
            {O::DOTNET_SHR, BinOpKind::Shr}, {O::DOTNET_SHR_UN, BinOpKind::ShrUn},
            {O::DOTNET_CEQ, BinOpKind::Eq}, {O::DOTNET_CGT, BinOpKind::Gt},
            {O::DOTNET_CGT_UN, BinOpKind::GtUn}, {O::DOTNET_CLT, BinOpKind::Lt},
            {O::DOTNET_CLT_UN, BinOpKind::LtUn},
            {O::DOTNET_ADD_OVF, BinOpKind::AddOvf}, {O::DOTNET_MUL_OVF, BinOpKind::MulOvf},
            {O::DOTNET_SUB_OVF, BinOpKind::SubOvf},
        };
        BcType resType = (insn.opcode == O::DOTNET_CEQ ||
                          insn.opcode == O::DOTNET_CGT || insn.opcode == O::DOTNET_CGT_UN ||
                          insn.opcode == O::DOTNET_CLT || insn.opcode == O::DOTNET_CLT_UN)
            ? types::Bool() : s0.type;
        auto it = opMap.find(insn.opcode);
        BinOpKind bop = (it != opMap.end()) ? it->second : BinOpKind::Add;
        auto expr = makeBinOp(bop, s0.expr, s1.expr, resType);
        push(stack, resType, expr);
        outExpr = expr;
        break;
    }

    // Unary
    case O::DOTNET_NEG:
        if (!pop(stack, s0)) return false;
        push(stack, s0.type, makeUnOp(UnOpKind::Neg, s0.expr, s0.type));
        break;
    case O::DOTNET_NOT:
        if (!pop(stack, s0)) return false;
        push(stack, s0.type, makeUnOp(UnOpKind::Not, s0.expr, s0.type));
        break;

    // Conversions
    case O::DOTNET_CONV_I4:
        if (!pop(stack, s0)) return false;
        push(stack, types::Int(), makeUnOp(UnOpKind::ConvI4, s0.expr, types::Int()));
        break;
    case O::DOTNET_CONV_I8:
        if (!pop(stack, s0)) return false;
        push(stack, types::Long(), makeUnOp(UnOpKind::ConvI8, s0.expr, types::Long()));
        break;
    case O::DOTNET_CONV_R4:
        if (!pop(stack, s0)) return false;
        push(stack, types::Float(), makeUnOp(UnOpKind::ConvR4, s0.expr, types::Float()));
        break;
    case O::DOTNET_CONV_R8:
        if (!pop(stack, s0)) return false;
        push(stack, types::Double(), makeUnOp(UnOpKind::ConvR8, s0.expr, types::Double()));
        break;
    case O::DOTNET_CONV_I1: case O::DOTNET_CONV_I2:
    case O::DOTNET_CONV_U1: case O::DOTNET_CONV_U2:
    case O::DOTNET_CONV_U4: case O::DOTNET_CONV_U8:
    case O::DOTNET_CONV_I:  case O::DOTNET_CONV_U:
    case O::DOTNET_CONV_R_UN:
    case O::DOTNET_CONV_OVF_I1: case O::DOTNET_CONV_OVF_U1:
    case O::DOTNET_CONV_OVF_I2: case O::DOTNET_CONV_OVF_U2:
    case O::DOTNET_CONV_OVF_I4: case O::DOTNET_CONV_OVF_U4:
    case O::DOTNET_CONV_OVF_I8: case O::DOTNET_CONV_OVF_U8:
    case O::DOTNET_CONV_OVF_I: case O::DOTNET_CONV_OVF_U:
    case O::DOTNET_CONV_OVF_I1_UN: case O::DOTNET_CONV_OVF_U1_UN:
    case O::DOTNET_CONV_OVF_I2_UN: case O::DOTNET_CONV_OVF_U2_UN:
    case O::DOTNET_CONV_OVF_I4_UN: case O::DOTNET_CONV_OVF_U4_UN:
    case O::DOTNET_CONV_OVF_I8_UN: case O::DOTNET_CONV_OVF_U8_UN:
    case O::DOTNET_CONV_OVF_I_UN:  case O::DOTNET_CONV_OVF_U_UN:
        if (!pop(stack, s0)) return false;
        push(stack, types::Int(), makeUnOp(UnOpKind::ConvI4, s0.expr, types::Int()));
        break;

    // Cast / type tests
    case O::DOTNET_CASTCLASS: case O::DOTNET_UNBOX: case O::DOTNET_UNBOX_ANY: {
        if (!pop(stack, s0)) return false;
        BcType t = types::ClrObject();
        for (const auto& op : insn.operands)
            if (const auto* to = std::get_if<BcTypeOperand>(&op)) { t = to->type; break; }
        push(stack, t, makeCast(s0.expr, t));
        break;
    }
    case O::DOTNET_ISINST: {
        if (!pop(stack, s0)) return false;
        BcType t = types::ClrObject();
        for (const auto& op : insn.operands)
            if (const auto* to = std::get_if<BcTypeOperand>(&op)) { t = to->type; break; }
        push(stack, t, makeIsinst(s0.expr, t));
        break;
    }
    case O::DOTNET_BOX: {
        if (!pop(stack, s0)) return false;
        BcType t = types::ClrObject();
        ExprBox eb{s0.expr, s0.type};
        push(stack, t, std::make_shared<CilExpr>(std::move(eb), t));
        break;
    }

    // Object creation
    case O::DOTNET_NEWOBJ: {
        // The operand is a BcMethodRef for the constructor (.ctor).
        // The constructor's parameter count is mr->descriptor.params.size()
        // (excluding the implicit 'this', which newobj allocates separately).
        int numParams = 0;
        BcType objType = types::ClrObject();
        std::string ctorClass, ctorSig;
        for (const auto& op : insn.operands)
            if (const auto* mr = std::get_if<BcMethodRef>(&op)) {
                numParams = static_cast<int>(mr->descriptor.params.size());
                ctorClass = mr->owner;
                ctorSig   = mr->descriptor.toString();
                if (!mr->owner.empty())
                    objType = types::Class(mr->owner);
                break;
            }
        std::vector<StackSlot> ctorArgs;
        popN(stack, numParams, ctorArgs);
        ExprNewobj en;
        en.className = std::move(ctorClass);
        en.ctorSig   = std::move(ctorSig);
        en.type      = objType;
        for (auto& a : ctorArgs)
            en.args.push_back(a.expr);
        push(stack, objType, std::make_shared<CilExpr>(std::move(en), objType));
        break;
    }
    case O::DOTNET_NEWARR: {
        if (!pop(stack, s0)) return false; // length
        BcType elemType = types::ClrObject();
        for (const auto& op : insn.operands)
            if (const auto* to = std::get_if<BcTypeOperand>(&op)) { elemType = to->type; break; }
        ExprNewarr na{elemType, s0.expr};
        auto t = types::Array(elemType);
        push(stack, t, std::make_shared<CilExpr>(std::move(na), t));
        break;
    }
    case O::DOTNET_INITOBJ: {
        if (!pop(stack, s0)) return false; // address
        break;
    }

    // Field access
    case O::DOTNET_LDFLD: {
        if (!pop(stack, s0)) return false;
        BcType ft = types::ClrObject();
        std::string cn, fn;
        for (const auto& op : insn.operands)
            if (const auto* fr = std::get_if<BcFieldRef>(&op)) {
                cn = fr->owner; fn = fr->name; break;
            }
        ExprField ef{s0.expr, cn, fn, ft};
        push(stack, ft, std::make_shared<CilExpr>(std::move(ef), ft));
        break;
    }
    case O::DOTNET_LDSFLD: {
        BcType ft = types::ClrObject();
        std::string cn, fn;
        for (const auto& op : insn.operands)
            if (const auto* fr = std::get_if<BcFieldRef>(&op)) {
                cn = fr->owner; fn = fr->name; break;
            }
        ExprSField esf{cn, fn, ft};
        push(stack, ft, std::make_shared<CilExpr>(std::move(esf), ft));
        break;
    }
    case O::DOTNET_STFLD: {
        if (!pop(stack, s1) || !pop(stack, s0)) return false;
        outExpr = s1.expr;
        break;
    }
    case O::DOTNET_STSFLD:
        if (!pop(stack, s0)) return false;
        outExpr = s0.expr;
        break;
    case O::DOTNET_LDFLDA: {
        if (!pop(stack, s0)) return false;
        ExprAddressOf ao{s0.expr};
        push(stack, types::Long(), std::make_shared<CilExpr>(std::move(ao), types::Long()));
        break;
    }
    case O::DOTNET_LDSFLDA: {
        ExprAddressOf ao{nullptr};
        push(stack, types::Long(), std::make_shared<CilExpr>(std::move(ao), types::Long()));
        break;
    }

    // Array access
    case O::DOTNET_LDELEM: case O::DOTNET_LDELEM_I1: case O::DOTNET_LDELEM_U1:
    case O::DOTNET_LDELEM_I2: case O::DOTNET_LDELEM_U2:
    case O::DOTNET_LDELEM_I4: case O::DOTNET_LDELEM_U4:
    case O::DOTNET_LDELEM_I8: case O::DOTNET_LDELEM_I:
    case O::DOTNET_LDELEM_R4: case O::DOTNET_LDELEM_R8: case O::DOTNET_LDELEM_REF:
    case O::DOTNET_LDELEMA: {
        if (!pop(stack, s1) || !pop(stack, s0)) return false;
        ExprLdelem le{s0.expr, s1.expr, types::ClrObject()};
        push(stack, types::ClrObject(),
             std::make_shared<CilExpr>(std::move(le), types::ClrObject()));
        break;
    }
    case O::DOTNET_STELEM: case O::DOTNET_STELEM_REF:
    case O::DOTNET_STELEM_I: case O::DOTNET_STELEM_I1: case O::DOTNET_STELEM_I2:
    case O::DOTNET_STELEM_I4: case O::DOTNET_STELEM_I8:
    case O::DOTNET_STELEM_R4: case O::DOTNET_STELEM_R8:
        if (!pop(stack, s2) || !pop(stack, s1) || !pop(stack, s0)) return false;
        break;
    case O::DOTNET_LDLEN: {
        if (!pop(stack, s0)) return false;
        push(stack, types::Int(), nullptr);
        break;
    }

    // Method calls
    case O::DOTNET_CALL:
    case O::DOTNET_CALLVIRT:
    case O::DOTNET_TAIL_CALL:
    case O::DOTNET_CALLI: {
        int numArgs = 0;
        bool hasObj = false;
        BcType retType = types::ClrObject();
        bool isVoid = false;
        std::string callClassName, callMethodName;
        for (const auto& op : insn.operands)
            if (const auto* mr = std::get_if<BcMethodRef>(&op)) {
                numArgs        = static_cast<int>(mr->descriptor.params.size());
                callClassName  = mr->owner;
                callMethodName = mr->name;
                // CALLVIRT and non-static CALL have an implicit 'this' on the stack.
                hasObj = (insn.opcode == O::DOTNET_CALLVIRT)
                         || (insn.opcode == O::DOTNET_CALL && mr->name != ".cctor");
                if (mr->descriptor.returnType)
                    retType = *mr->descriptor.returnType;
                isVoid = !mr->descriptor.returnType;
                break;
            }
        std::vector<StackSlot> callArgs;
        CilExprPtr obj;
        if (hasObj) {
            StackSlot objSlot;
            pop(stack, objSlot);
            obj = objSlot.expr;
        }
        popN(stack, numArgs, callArgs);
        if (!isVoid) {
            ExprCall ec;
            ec.className  = std::move(callClassName);
            ec.methodName = std::move(callMethodName);
            ec.isVirtual  = (insn.opcode == O::DOTNET_CALLVIRT);
            ec.obj        = obj;
            ec.retType    = retType;
            for (auto& a : callArgs)
                ec.args.push_back(a.expr);
            push(stack, retType,
                 std::make_shared<CilExpr>(std::move(ec), retType));
        }
        break;
    }

    // Return
    case O::DOTNET_RET: {
        bool isVoid = !method.descriptor.returnType ||
                       method.descriptor.returnType->isVoid();
        if (!isVoid && !stack.empty())
            if (!pop(stack, s0)) return false;
        break;
    }

    // Throw
    case O::DOTNET_THROW: case O::DOTNET_RETHROW:
        if (insn.opcode == O::DOTNET_THROW)
            if (!pop(stack, s0)) return false;
        break;

    // Branches (consume from stack per conditional variant)
    case O::DOTNET_BRTRUE: case O::DOTNET_BRTRUE_S:
    case O::DOTNET_BRFALSE: case O::DOTNET_BRFALSE_S:
        if (!pop(stack, s0)) return false;
        outExpr = s0.expr;
        break;
    case O::DOTNET_BEQ: case O::DOTNET_BEQ_S:
    case O::DOTNET_BNE_UN: case O::DOTNET_BNE_UN_S:
    case O::DOTNET_BGT: case O::DOTNET_BGT_S: case O::DOTNET_BGT_UN: case O::DOTNET_BGT_UN_S:
    case O::DOTNET_BGE: case O::DOTNET_BGE_S: case O::DOTNET_BGE_UN: case O::DOTNET_BGE_UN_S:
    case O::DOTNET_BLT: case O::DOTNET_BLT_S: case O::DOTNET_BLT_UN: case O::DOTNET_BLT_UN_S:
    case O::DOTNET_BLE: case O::DOTNET_BLE_S: case O::DOTNET_BLE_UN: case O::DOTNET_BLE_UN_S:
        if (!pop(stack, s1) || !pop(stack, s0)) return false;
        outExpr = makeBinOp(BinOpKind::Eq, s0.expr, s1.expr, types::Bool());
        break;

    case O::DOTNET_BR: case O::DOTNET_BR_S:
    case O::DOTNET_LEAVE: case O::DOTNET_LEAVE_S:
        // Clear stack on leave
        if (insn.opcode == O::DOTNET_LEAVE || insn.opcode == O::DOTNET_LEAVE_S)
            stack.clear();
        break;

    case O::DOTNET_SWITCH:
        if (!pop(stack, s0)) return false;
        break;

    // sizeof
    case O::DOTNET_SIZEOF:
        push(stack, types::UInt(), nullptr);
        break;

    // ldtoken
    case O::DOTNET_LDTOKEN: {
        ExprLdToken lt{"token"};
        push(stack, types::ClrObject(), std::make_shared<CilExpr>(std::move(lt), types::ClrObject()));
        break;
    }

    // localloc
    case O::DOTNET_LOCALLOC: {
        if (!pop(stack, s0)) return false;
        ExprLocAlloc la{s0.expr, types::Byte()};
        push(stack, types::Long(), std::make_shared<CilExpr>(std::move(la), types::Long()));
        break;
    }

    // indirect load/store (handled as generic)
    case O::DOTNET_LDIND_I1: case O::DOTNET_LDIND_U1:
    case O::DOTNET_LDIND_I2: case O::DOTNET_LDIND_U2:
    case O::DOTNET_LDIND_I4: case O::DOTNET_LDIND_U4:
    case O::DOTNET_LDIND_I8: case O::DOTNET_LDIND_I:
    case O::DOTNET_LDIND_R4: case O::DOTNET_LDIND_R8: case O::DOTNET_LDIND_REF: {
        if (!pop(stack, s0)) return false;
        ExprDeref d{s0.expr, types::ClrObject()};
        push(stack, types::ClrObject(), std::make_shared<CilExpr>(std::move(d), types::ClrObject()));
        break;
    }
    case O::DOTNET_STIND_REF: case O::DOTNET_STIND_I:
    case O::DOTNET_STIND_I1: case O::DOTNET_STIND_I2:
    case O::DOTNET_STIND_I4: case O::DOTNET_STIND_I8:
    case O::DOTNET_STIND_R4: case O::DOTNET_STIND_R8:
        if (!pop(stack, s1) || !pop(stack, s0)) return false;
        break;

    // endfinally/endfilter
    case O::DOTNET_ENDFINALLY: case O::DOTNET_ENDFILTER:
        if (insn.opcode == O::DOTNET_ENDFILTER)
            if (!pop(stack, s0)) return false;
        break;

    case O::DOTNET_LDFTN: {
        ExprLdFtn lf;
        for (const auto& op : insn.operands)
            if (const auto* mr = std::get_if<BcMethodRef>(&op)) {
                lf.className = mr->owner; lf.methodName = mr->name; break;
            }
        push(stack, types::ClrObject(), std::make_shared<CilExpr>(std::move(lf), types::ClrObject()));
        break;
    }
    case O::DOTNET_LDVIRTFTN: {
        if (!pop(stack, s0)) return false;
        ExprLdFtn lf; lf.isVirtual = true;
        push(stack, types::ClrObject(), std::make_shared<CilExpr>(std::move(lf), types::ClrObject()));
        break;
    }

    case O::DOTNET_LDOBJ: {
        if (!pop(stack, s0)) return false;
        BcType t = types::ClrObject();
        for (const auto& op : insn.operands)
            if (const auto* to = std::get_if<BcTypeOperand>(&op)) { t = to->type; break; }
        ExprDeref d{s0.expr, t};
        push(stack, t, std::make_shared<CilExpr>(std::move(d), t));
        break;
    }
    case O::DOTNET_STOBJ:
        if (!pop(stack, s1) || !pop(stack, s0)) return false;
        break;

    case O::DOTNET_CPOBJ:
        if (!pop(stack, s1) || !pop(stack, s0)) return false;
        break;
    case O::DOTNET_CPBLK: case O::DOTNET_INITBLK:
        if (!pop(stack, s2) || !pop(stack, s1) || !pop(stack, s0)) return false;
        break;

    case O::DOTNET_CKFINITE:
        if (!pop(stack, s0)) return false;
        push(stack, s0.type, s0.expr);
        break;

    case O::DOTNET_MKREFANY: {
        if (!pop(stack, s0)) return false;
        ExprMkRefAny mr{s0.expr, types::ClrObject()};
        push(stack, types::ClrObject(), std::make_shared<CilExpr>(std::move(mr), types::ClrObject()));
        break;
    }
    case O::DOTNET_REFANYVAL: {
        if (!pop(stack, s0)) return false;
        BcType t = types::ClrObject();
        ExprRefAnyVal rv{s0.expr, t};
        push(stack, t, std::make_shared<CilExpr>(std::move(rv), t));
        break;
    }
    case O::DOTNET_REFANYTYPE: {
        if (!pop(stack, s0)) return false;
        ExprRefAnyType rt{s0.expr};
        push(stack, types::ClrObject(), std::make_shared<CilExpr>(std::move(rt), types::ClrObject()));
        break;
    }

    case O::DOTNET_ARGLIST:
        push(stack, types::ClrObject(), std::make_shared<CilExpr>(ExprArgList{}, types::ClrObject()));
        break;

    default:
        break;
    }

    return true;
}

// ─── Fixpoint loop ────────────────────────────────────────────────────────────

bool CilStackSimulator::runFixpoint(const BcCFG& cfg, const BcMethod& method) {
    uint32_t n = cfg.blockCount();
    blockInfos_.resize(n);

    // Initialize entry block
    if (n == 0) return true;

    // Seed the work-list
    std::queue<uint32_t> worklist;
    std::unordered_set<uint32_t> inQueue;
    worklist.push(0);
    inQueue.insert(0);

    // Entry for EH handler blocks: catch = [catchType], filter = [Object]
    for (const auto& eh : cfg.handlers()) {
        if (eh.handlerBlock < n) {
            StackState& hs = blockInfos_[eh.handlerBlock].entryStack;
            if (hs.empty()) {
                StackSlot slot;
                if (eh.isFinally || eh.isFault)
                    slot.type = types::ClrObject();
                else
                    slot.type = (eh.catchType.has_value() && !eh.catchType->isVoid())
                        ? *eh.catchType : types::ClrObject();
                slot.expr = makeNull(); // placeholder
                hs.push_back(std::move(slot));
                if (!inQueue.count(eh.handlerBlock)) {
                    worklist.push(eh.handlerBlock);
                    inQueue.insert(eh.handlerBlock);
                }
            }
        }
    }

    int iterations = 0;
    while (!worklist.empty() && iterations < opts_.maxIterations) {
        ++iterations;
        uint32_t bid = worklist.front();
        worklist.pop();
        inQueue.erase(bid);

        const BcBasicBlock& blk = cfg.block(bid);
        BlockStackInfo& info = blockInfos_[bid];

        StackState stack = info.entryStack;
        info.instrStacks.clear();
        info.instrStacks.reserve(blk.instrs.size());

        for (const auto& insn : blk.instrs) {
            CilExprPtr expr;
            if (!applyInstruction(insn, stack, method, expr)) {
                // Stack underflow — recover gracefully
            }
            info.instrStacks.push_back(stack);
        }
        info.exitStack = stack;

        // Propagate to successors
        for (uint32_t succ : blk.succs) {
            if (succ >= n) continue;
            BlockStackInfo& succInfo = blockInfos_[succ];
            StackState newEntry = meetStates(succInfo.entryStack, stack);
            if (newEntry != succInfo.entryStack) {
                succInfo.entryStack = std::move(newEntry);
                if (!inQueue.count(succ)) {
                    worklist.push(succ);
                    inQueue.insert(succ);
                }
            }
        }
    }

    return true;
}

bool CilStackSimulator::simulate(const BcCFG& cfg, const BcMethod& method) {
    valid_ = false;
    blockInfos_.clear();

    if (cfg.blockCount() == 0) {
        valid_ = true;
        return true;
    }

    if (!runFixpoint(cfg, method)) {
        error_ = "Stack simulation fixpoint failed";
        return false;
    }

    valid_ = true;
    return true;
}

} // namespace cil_reconstruct
} // namespace retdec
