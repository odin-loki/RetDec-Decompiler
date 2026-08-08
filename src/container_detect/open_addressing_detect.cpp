/**
 * @file src/container_detect/open_addressing_detect.cpp
 * @brief Open-addressing hash table detector (linear probing).
 */

#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace container_detect {

namespace {

static int countOp(const ssa::SSAFunction& fn, ssa::IrInstr::Op op) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs)
            if (instr && instr->op == op) ++n;
    }
    return n;
}

static bool hasBackEdge(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (uint32_t s : blk->succs)
            if (s <= b) return true;
    }
    return false;
}

static bool hasInlineHash(const ssa::SSAFunction& fn) {
    bool hasXor = false, hasMul = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Xor) hasXor = true;
            if (instr->op == ssa::IrInstr::Op::Mul) hasMul = true;
        }
    }
    return hasXor && hasMul;
}

static bool hasHashSignal(const ssa::SSAFunction& fn) {
    if (hasInlineHash(fn)) return true;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = instr->calleeName;
            if (cn.find("hash") != std::string::npos
                || cn.find("strcmp") != std::string::npos)
                return true;
        }
    }
    return false;
}

static bool hasModuloIndex(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Div)
                return true;
        }
    }
    return false;
}

} // anonymous namespace

ContainerResult OpenAddressingDetector::detect(const ssa::SSAFunction& fn) const {
    ContainerResult result;
    result.kind = ContainerKind::Array;

    if (!hasBackEdge(fn))
        return result;

    const bool hash = hasHashSignal(fn);
    const bool mod  = hasModuloIndex(fn);
    const int cmps  = countOp(fn, ssa::IrInstr::Op::Compare);
    const int loads = countOp(fn, ssa::IrInstr::Op::Load);
    const int stores = countOp(fn, ssa::IrInstr::Op::Store);

    if (!hash || !mod || cmps < 1 || loads < 2)
        return result;

    float score = 0.0f;
    if (hash)              score += 0.35f;
    if (mod)               score += 0.30f;
    if (cmps >= 1)         score += 0.20f;
    if (loads >= 2)        score += 0.15f;
    if (stores >= 1)       score += 0.10f;

    result.confidence = score > 1.0f ? 1.0f : score;
    if (result.confidence < 0.55f)
        return result;

    result.emittedType = "open_addressing_hash_table";
    result.elementType.kind = RecoveredType::Kind::Int32;
    result.keyType.kind     = RecoveredType::Kind::Int32;
    return result;
}

} // namespace container_detect
} // namespace retdec
