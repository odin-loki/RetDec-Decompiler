/**
 * @file src/container_detect/unordered_detect.cpp
 * @brief std::unordered_map / unordered_set detector — hash-table fingerprinting.
 *
 * ## Structural fingerprint
 *
 * libstdc++ std::unordered_map stores:
 *   - A flat singly-linked node list (all elements in one list, ordered by hash).
 *   - A bucket array of begin-iterators into the list, size = bucket_count.
 *
 * libc++ std::unordered_map (and MSVC STL) use a classic chained bucket array:
 *   - bucket_array[i] → first node in chain or nullptr.
 *   - node: next pointer + value_type.
 *
 * Signals detected in IR:
 *
 * ### Bucket array
 *   - A pointer array (block of pointer-typed Loads from an array base).
 *   - Its size (bucket_count) is typically a prime number or power of two.
 *
 * ### Hash computation
 *   - A Call to `__hash_code`, `std::hash<...>::operator()`, `_Hash_bytes`,
 *     `fnv1a`, `_M_hash_code` or similar.
 *   - Or inline: shift + xor + multiply chain on the key (FNV-1a / murmur).
 *
 * ### Modulo (bucket selection)
 *   Power-of-two bucket count:
 *     `bucket_idx = hash & (bucket_count - 1)` → And with (2^k − 1) constant.
 *   Arbitrary bucket count:
 *     `bucket_idx = hash % bucket_count`       → IRem or UDiv + Mul + Sub idiom.
 *
 * ### Chain traversal
 *   - A loop that loads `node->_M_next` and compares to nullptr.
 *
 * ## Confidence scoring
 *
 *   bucket array evidence      +0.30
 *   hash computation           +0.30
 *   modulo bucket selection    +0.25
 *   chain traversal            +0.15
 */

#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <cstdint>

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

// Hash computation: call to a recognisable hash function, or inline XOR+MUL chain.
static bool hasHashCompute(const ssa::SSAFunction& fn) {
    bool hasXor = false, hasMul = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Call) {
                const auto& cn = instr->calleeName;
                if (cn.find("hash")  != std::string::npos ||
                    cn.find("Hash")  != std::string::npos ||
                    cn.find("fnv")   != std::string::npos ||
                    cn.find("murmur")!= std::string::npos)
                    return true;
            }
            if (instr->op == ssa::IrInstr::Op::Xor) hasXor = true;
            if (instr->op == ssa::IrInstr::Op::Mul) hasMul = true;
        }
    }
    // Inline FNV-1a: xor + mul combination.
    return hasXor && hasMul;
}

// Bucket array: at least two consecutive Loads from a pointer base.
static bool hasBucketArray(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Load) >= 2;
}

// Chain traversal: a loop with Load + Compare (node->next == nullptr check).
static bool hasChainTraversal(const ssa::SSAFunction& fn) {
    bool backEdge = false;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (uint32_t succ : blk->succs)
            if (succ <= b) { backEdge = true; break; }
    }
    return backEdge &&
           countOp(fn, ssa::IrInstr::Op::Load)    >= 1 &&
           countOp(fn, ssa::IrInstr::Op::Compare) >= 1;
}

} // anonymous namespace

// ─── UnorderedMapDetector ────────────────────────────────────────────────────

bool UnorderedMapDetector::hasBucketModulo(const ssa::SSAFunction& fn) const {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::And) {
                for (const auto& u : instr->uses) {
                    const auto* iv = fn.value(u.valueId);
                    if (iv && iv->kind == ssa::ValueKind::Immediate) {
                        uint64_t c = iv->imm;
                        if (c > 0 && ((c + 1) & c) == 0) return true;
                    }
                }
            }
            if (instr->op == ssa::IrInstr::Op::Div) return true;
        }
    }
    return false;
}

bool UnorderedMapDetector::hasHashFunction(const ssa::SSAFunction& fn) const {
    return hasHashCompute(fn);
}

HashTableEvidence UnorderedMapDetector::analyseStructure(const ssa::SSAFunction& fn) const {
    HashTableEvidence ev;
    ev.hasBucketArray    = hasBucketArray(fn);
    ev.hasHashCompute    = hasHashFunction(fn);
    ev.hasModulo         = hasBucketModulo(fn);
    ev.hasChainTraversal = hasChainTraversal(fn);
    ev.found = ev.hasHashCompute && (ev.hasModulo || ev.hasBucketArray);
    ev.confidence = scoreEvidence(ev);
    return ev;
}

float UnorderedMapDetector::scoreEvidence(const HashTableEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasBucketArray)    s += 0.30f;
    if (ev.hasHashCompute)    s += 0.30f;
    if (ev.hasModulo)         s += 0.25f;
    if (ev.hasChainTraversal) s += 0.15f;
    return s > 1.0f ? 1.0f : s;
}

ContainerResult UnorderedMapDetector::detect(const ssa::SSAFunction& fn) const {
    ContainerResult result;
    result.kind = ContainerKind::UnorderedMap;

    auto ev = analyseStructure(fn);
    result.confidence = ev.confidence;

    if (ev.confidence < 0.10f) return result;

    result.emittedType = "std::unordered_map<int, int>";
    result.elementType.kind = RecoveredType::Kind::Int32;
    result.keyType.kind     = RecoveredType::Kind::Int32;

    if (ev.hasChainTraversal) {
        AccessPattern ap;
        ap.kind    = AccessKind::Iterate;
        ap.emitted = "for (auto& [k,v] : um)";
        result.accessPatterns.push_back(ap);
    }
    if (ev.hasHashCompute) {
        AccessPattern ap;
        ap.kind    = AccessKind::Lookup;
        ap.emitted = "um.find(key)";
        result.accessPatterns.push_back(ap);
    }
    if (ev.hasBucketArray) {
        AccessPattern ap;
        ap.kind    = AccessKind::Insert;
        ap.emitted = "um[key] = val";
        result.accessPatterns.push_back(ap);
    }

    return result;
}

} // namespace container_detect
} // namespace retdec
