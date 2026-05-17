/**
 * @file src/codegen/expr_coalesce.cpp
 * @brief Expression coalescing and SSA-to-CExpr lowering.
 *
 * This translation unit contains:
 *   - CType::toString / bitWidth / isIntegral helpers
 *   - CExpr::toString (recursive printer with precedence)
 *   - ExprCoalescer::run — main entry point
 *   - ExprCoalescer::buildExpr — recursive expression builder
 *   - ExprCoalescer::nameForValue — C variable name generator
 */

#include <memory>
#include "retdec/codegen/codegen.h"
#include "retdec/ssa/ssa.h"
#include "retdec/dce/dce.h"

#include <cassert>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace retdec {
namespace codegen {

// ─── CType helpers ────────────────────────────────────────────────────────────

// Helper constructor for const-stripping clone.
CType::CType(const CType& o, bool) : CType(o) { isConst = false; }

std::string CType::toString() const {
    if (isConst) {
        CType copy(*this, false);
        return "const " + copy.toString();
    }
    switch (kind) {
    case Kind::Void:   return "void";
    case Kind::Bool:   return "_Bool";
    case Kind::Int8:   return "int8_t";
    case Kind::Int16:  return "int16_t";
    case Kind::Int32:  return "int32_t";
    case Kind::Int64:  return "int64_t";
    case Kind::UInt8:  return "uint8_t";
    case Kind::UInt16: return "uint16_t";
    case Kind::UInt32: return "uint32_t";
    case Kind::UInt64: return "uint64_t";
    case Kind::Float:  return "float";
    case Kind::Double: return "double";
    case Kind::Pointer:
        if (!children.empty())
            return children[0]->toString() + " *";
        return "void *";
    case Kind::Array:
        if (!children.empty()) {
            std::string s = children[0]->toString();
            if (arraySize > 0) s += "[" + std::to_string(arraySize) + "]";
            else                s += "[]";
            return s;
        }
        return "void[]";
    case Kind::Struct:
        return "struct " + name;
    case Kind::Union:
        return "union " + name;
    case Kind::FuncPtr:
        return "void (*)()";
    default:
        return "int";
    }
}

bool CType::isIntegral() const {
    return kind == Kind::Int8  || kind == Kind::Int16 ||
           kind == Kind::Int32 || kind == Kind::Int64 ||
           kind == Kind::UInt8 || kind == Kind::UInt16 ||
           kind == Kind::UInt32|| kind == Kind::UInt64 ||
           kind == Kind::Bool;
}

uint8_t CType::bitWidth() const {
    switch (kind) {
    case Kind::Bool:
    case Kind::Int8:   case Kind::UInt8:  return 8;
    case Kind::Int16:  case Kind::UInt16: return 16;
    case Kind::Float:  case Kind::Int32:  case Kind::UInt32: return 32;
    case Kind::Double: case Kind::Int64:  case Kind::UInt64:
    case Kind::Pointer:                   return 64;
    default: return 0;
    }
}

// ─── CExpr helpers ────────────────────────────────────────────────────────────

const char* binOpStr(CExpr::BinOpKind op) noexcept {
    using B = CExpr::BinOpKind;
    switch (op) {
    case B::Add: return "+";  case B::Sub: return "-";
    case B::Mul: return "*";  case B::Div: return "/";
    case B::Mod: return "%";  case B::And: return "&";
    case B::Or:  return "|";  case B::Xor: return "^";
    case B::Shl: return "<<"; case B::Shr: return ">>";
    case B::LAnd:return "&&"; case B::LOr: return "||";
    case B::Eq:  return "=="; case B::Ne:  return "!=";
    case B::Lt:  return "<";  case B::Le:  return "<=";
    case B::Gt:  return ">";  case B::Ge:  return ">=";
    case B::Assign: return "=";
    }
    return "?";
}

const char* unOpStr(CExpr::UnOpKind op) noexcept {
    using U = CExpr::UnOpKind;
    switch (op) {
    case U::Neg:    return "-";
    case U::Not:    return "!";
    case U::BitNot: return "~";
    case U::Deref:  return "*";
    case U::AddrOf: return "&";
    case U::PreInc: return "++";
    case U::PreDec: return "--";
    default:        return "";
    }
}

// Operator precedence (higher = tighter binding, following C standard).
int binOpPrec(CExpr::BinOpKind op) noexcept {
    using B = CExpr::BinOpKind;
    switch (op) {
    case B::Mul: case B::Div: case B::Mod: return 13;
    case B::Add: case B::Sub:              return 12;
    case B::Shl: case B::Shr:             return 11;
    case B::Lt:  case B::Le:
    case B::Gt:  case B::Ge:              return 10;
    case B::Eq:  case B::Ne:              return 9;
    case B::And:                           return 8;
    case B::Xor:                           return 7;
    case B::Or:                            return 6;
    case B::LAnd:                          return 5;
    case B::LOr:                           return 4;
    case B::Assign:                        return 2;
    }
    return 1;
}

std::string CExpr::toString(int outerPrec) const {
    using K = CExpr::Kind;
    switch (kind) {
    case K::Literal:
        return literal;

    case K::Var:
        return varName;

    case K::BinOp: {
        int myPrec = binOpPrec(binOp);
        std::string l = children[0]->toString(myPrec);
        std::string r = children[1]->toString(myPrec + 1); // left-assoc
        std::string s = l + " " + binOpStr(binOp) + " " + r;
        if (myPrec < outerPrec) s = "(" + s + ")";
        return s;
    }

    case K::UnOp: {
        using U = CExpr::UnOpKind;
        if (unOp == U::PostInc)
            return children[0]->toString(15) + "++";
        if (unOp == U::PostDec)
            return children[0]->toString(15) + "--";
        std::string s = std::string(unOpStr(unOp)) + children[0]->toString(14);
        if (14 < outerPrec) s = "(" + s + ")";
        return s;
    }

    case K::Cast: {
        std::string t = castType ? castType->toString() : "int";
        std::string s = "(" + t + ")" + children[0]->toString(14);
        if (14 < outerPrec) s = "(" + s + ")";
        return s;
    }

    case K::Call: {
        std::string s = callee + "(";
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (i) s += ", ";
            s += children[i]->toString(1);
        }
        s += ")";
        return s;
    }

    case K::Index:
        return children[0]->toString(15) + "[" + children[1]->toString(1) + "]";

    case K::Member: {
        std::string op = arrowAccess ? "->" : ".";
        return children[0]->toString(15) + op + fieldName;
    }

    case K::Ternary: {
        std::string s = children[0]->toString(3) + " ? " +
                        children[1]->toString(3) + " : " +
                        children[2]->toString(3);
        if (3 < outerPrec) s = "(" + s + ")";
        return s;
    }

    case K::Comma: {
        std::string s;
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (i) s += ", ";
            s += children[i]->toString(1);
        }
        if (1 < outerPrec) s = "(" + s + ")";
        return s;
    }
    }
    return "";
}

// ─── ExprCoalescer ────────────────────────────────────────────────────────────

namespace {

// Map SSA Op → BinOpKind (only for instructions that map directly).
static bool ssaOpToBinOp(ssa::IrInstr::Op op, CExpr::BinOpKind& out) {
    using O = ssa::IrInstr::Op;
    using B = CExpr::BinOpKind;
    switch (op) {
    case O::Add: out = B::Add; return true;
    case O::Sub: out = B::Sub; return true;
    case O::Mul: out = B::Mul; return true;
    case O::Div: out = B::Div; return true;
    case O::And: out = B::And; return true;
    case O::Or:  out = B::Or;  return true;
    case O::Xor: out = B::Xor; return true;
    case O::Shl: out = B::Shl; return true;
    case O::Shr: out = B::Shr; return true;
    case O::Sar: out = B::Shr; return true; // arithmetic shift → signed shr
    default: return false;
    }
}

static bool hasSideEffects(ssa::IrInstr::Op op) {
    using O = ssa::IrInstr::Op;
    return op == O::Call || op == O::Store;
}

// Collect all uses of a given value across the function.
// Returns the block ID of the sole use, or kInvalidBlock if zero or multiple.
static ssa::BlockId findSoleUseBlock(ssa::ValueId vid,
                                      const ssa::SSAFunction& fn,
                                      const dce::DeadCodeResult& dce) {
    ssa::BlockId soleBlock = ssa::kInvalidBlock;
    int count = 0;

    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (!dce.liveInstrs.empty() &&
                !dce.liveInstrs.count(instr->id))
                continue;
            for (const auto& use : instr->uses) {
                if (use.valueId == vid) {
                    ++count;
                    soleBlock = bid;
                    if (count > 1) return ssa::kInvalidBlock;
                }
            }
        }
    }
    return (count == 1) ? soleBlock : ssa::kInvalidBlock;
}

} // anonymous namespace

std::string ExprCoalescer::nameForValue(uint32_t vid,
                                         const ssa::SSAFunction& fn) const {
    if (vid == ssa::kInvalidValue) return "?";
    // Look up the variable ID from the value.
    const auto* val = fn.value(vid);
    if (val && val->varId != ssa::kInvalidVar) {
        const std::string& n = fn.varName(val->varId);
        if (!n.empty()) return n + "_" + std::to_string(val->version);
    }
    return "v" + std::to_string(vid);
}

ExprCoalescer::Result ExprCoalescer::run(
        const ssa::SSAFunction& fn,
        const dce::DeadCodeResult& dce) const {

    Result res;

    // Step 1: For each defined value, determine:
    //   defBlock[vid]    — which block defines it
    //   defInstrIdx[vid] — index in that block's instrs list
    //   soleUseBlock[vid]— block of sole use (kInvalidBlock = 0 or 2+ uses)

    std::unordered_map<ssa::ValueId, ssa::BlockId>   defBlock;
    std::unordered_map<ssa::ValueId, std::size_t>     defInstrIdx;
    std::unordered_map<ssa::ValueId, ssa::BlockId>   soleUseBlock;
    std::unordered_map<ssa::ValueId, int>              useCount;

    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk) continue;
        for (std::size_t ii = 0; ii < blk->instrs.size(); ++ii) {
            const auto* instr = blk->instrs[ii];
            if (!instr) continue;
            if (!dce.liveInstrs.empty() &&
                !dce.liveInstrs.count(instr->id))
                continue;

            if (instr->defValue != ssa::kInvalidValue) {
                defBlock[instr->defValue]   = bid;
                defInstrIdx[instr->defValue]= ii;
            }
            for (const auto& use : instr->uses) {
                if (use.valueId == ssa::kInvalidValue) continue;
                int& cnt = useCount[use.valueId];
                ++cnt;
                if (cnt == 1) soleUseBlock[use.valueId] = bid;
                else          soleUseBlock.erase(use.valueId);
            }
        }
    }

    // Also check IrValue::Immediate — these have no defining instruction
    // and get a direct literal expr. No coalescing needed, just materialise.
    for (const auto& valPtr : fn.values()) {
        if (!valPtr) continue;
        if (valPtr->kind == ssa::ValueKind::Immediate) {
            res.valueExprs[valPtr->id] =
                CExpr::lit(std::to_string(valPtr->imm), valPtr->id);
        }
    }

    // Step 2: Mark inlineable values.
    std::unordered_set<ssa::ValueId> inlineable;

    for (auto& [vid, cnt] : useCount) {
        if (cnt != 1) continue;
        auto dbIt  = defBlock.find(vid);
        auto subIt = soleUseBlock.find(vid);
        if (dbIt == defBlock.end() || subIt == soleUseBlock.end()) continue;
        if (dbIt->second != subIt->second) continue; // different blocks

        const auto* blk = fn.block(dbIt->second);
        if (!blk) continue;
        std::size_t defIdx = defInstrIdx[vid];

        bool blocked = false;
        for (std::size_t ii = defIdx + 1; ii < blk->instrs.size(); ++ii) {
            const auto* instr = blk->instrs[ii];
            if (!instr) continue;
            bool usesVid = false;
            for (const auto& use : instr->uses)
                if (use.valueId == vid) { usesVid = true; break; }
            if (usesVid) break;
            if (hasSideEffects(instr->op)) { blocked = true; break; }
        }
        if (!blocked) inlineable.insert(vid);
    }

    res.inlinedValues = inlineable;

    // Step 3: Build CExpr for every live instruction def (non-Immediate values).
    std::unordered_set<ssa::ValueId> inProgress;

    for (uint32_t bid = 0; bid < fn.blockCount(); ++bid) {
        const auto* blk = fn.block(bid);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (!dce.liveInstrs.empty() &&
                !dce.liveInstrs.count(instr->id))
                continue;
            if (instr->defValue == ssa::kInvalidValue) continue;
            if (res.valueExprs.count(instr->defValue)) continue;

            auto e = buildExpr(instr->defValue, fn, res, inProgress);
            if (e) res.valueExprs[instr->defValue] = e;
        }
    }

    // Build variable name map for non-inlined values.
    for (const auto& valPtr : fn.values()) {
        if (!valPtr || valPtr->kind == ssa::ValueKind::Immediate) continue;
        if (inlineable.count(valPtr->id)) continue;
        std::string n = nameForValue(valPtr->id, fn);
        res.varNames[n] = n;
    }

    return res;
}

std::shared_ptr<CExpr> ExprCoalescer::buildExpr(
        ssa::ValueId vid,
        const ssa::SSAFunction& fn,
        Result& res,
        std::unordered_set<ssa::ValueId>& inProgress) const {

    if (inProgress.count(vid))
        return CExpr::var(nameForValue(vid, fn), vid); // cycle guard

    auto cached = res.valueExprs.find(vid);
    if (cached != res.valueExprs.end())
        return cached->second;

    const auto* val = fn.value(vid);
    if (!val) return CExpr::var("v" + std::to_string(vid), vid);

    // Immediate values are already materialised.
    if (val->kind == ssa::ValueKind::Immediate)
        return CExpr::lit(std::to_string(val->imm), vid);

    // Undef → 0.
    if (val->kind == ssa::ValueKind::Undef)
        return CExpr::lit("0", vid);

    // FlagBundle / MemRef → emit as variable reference.
    if (val->kind == ssa::ValueKind::FlagBundle ||
        val->kind == ssa::ValueKind::MemRef)
        return CExpr::var(nameForValue(vid, fn), vid);

    // For Phi → variable reference (phi values become C variables).
    if (val->kind == ssa::ValueKind::Phi || val->defPhi)
        return CExpr::var(nameForValue(vid, fn), vid);

    // VirtualReg — find the defining instruction.
    if (!val->defInstr)
        return CExpr::var(nameForValue(vid, fn), vid);

    const ssa::IrInstr* def = val->defInstr;

    inProgress.insert(vid);

    std::shared_ptr<CExpr> result;
    using O = ssa::IrInstr::Op;

    // Helper: get the i-th use value expr.
    auto getUse = [&](std::size_t i) -> std::shared_ptr<CExpr> {
        if (i >= def->uses.size()) return CExpr::lit("0");
        ssa::ValueId uid = def->uses[i].valueId;
        return buildExpr(uid, fn, res, inProgress);
    };

    switch (def->op) {
    case O::Assign: {
        // Assign copies the first operand.
        result = getUse(0);
        break;
    }

    case O::Add: case O::Sub: case O::Mul: case O::Div:
    case O::And: case O::Or:  case O::Xor:
    case O::Shl: case O::Shr: case O::Sar: {
        CExpr::BinOpKind bop;
        ssaOpToBinOp(def->op, bop);
        auto lhs = getUse(0);
        auto rhs = getUse(1);
        result = CExpr::binop(bop, lhs, rhs);
        break;
    }

    case O::Neg:
        result = CExpr::unop(CExpr::UnOpKind::Neg, getUse(0));
        break;

    case O::Not:
        result = CExpr::unop(CExpr::UnOpKind::BitNot, getUse(0));
        break;

    case O::Load:
        result = CExpr::unop(CExpr::UnOpKind::Deref, getUse(0));
        break;

    case O::Compare: {
        // CMP/TEST: emit as a comparison expression. The actual condition
        // (ZF, SF, etc.) is consumed by the flag reader. For the defining
        // expression we emit: lhs - rhs (the value CMP computes conceptually).
        auto lhs = getUse(0);
        auto rhs = getUse(1);
        result = CExpr::binop(CExpr::BinOpKind::Sub, lhs, rhs);
        break;
    }

    case O::Call: {
        std::vector<std::shared_ptr<CExpr>> args;
        for (std::size_t i = 0; i < def->uses.size(); ++i)
            args.push_back(getUse(i));
        std::string fname = def->calleeName.empty() ? "fn_ptr" : def->calleeName;
        result = CExpr::call(fname, std::move(args));
        break;
    }

    case O::Ror: case O::Rol:
        // Rotate: emit as a pair of shifts ORed together.
        // (x >> k) | (x << (bitwidth - k))  — simplified.
        result = CExpr::var(nameForValue(vid, fn), vid);
        break;

    default:
        result = CExpr::var(nameForValue(vid, fn), vid);
        break;
    }

    inProgress.erase(vid);

    if (!result) result = CExpr::var(nameForValue(vid, fn), vid);
    result->ssaValueId = vid;
    return result;
}

} // namespace codegen
} // namespace retdec
