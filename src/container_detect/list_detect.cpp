/**
 * @file src/container_detect/list_detect.cpp
 * @brief std::list<T> detector — circular doubly-linked list fingerprinting.
 *
 * ## Structural fingerprint
 *
 * libstdc++ std::list uses an intrusive sentinel node whose _next and _prev
 * pointers both point to itself in the empty-list initialisation:
 *
 *   ```cpp
 *   _M_header._M_next = &_M_header;  // circular sentinel
 *   _M_header._M_prev = &_M_header;
 *   ```
 *
 * In IR this produces:
 *   - A Store instruction where the source value is the address of the
 *     destination struct (self-referential write).
 *   - Another such Store for the sibling pointer (same struct, adjacent offset).
 *
 * Regular list nodes are heap-allocated:
 *   - malloc/new call with size = 2*pointer_width + sizeof(T).
 *   - Result pointer is written to two adjacent offsets (prev/next linkage) of
 *     neighbouring nodes.
 *
 * Iteration pattern:
 *   - A loop that loads `node->_next` into the induction variable and compares
 *     it to the sentinel address (equality or pointer comparison).
 *
 * Insert / erase:
 *   - Four pointer stores: update prev/next in the two surrounding nodes.
 *
 * ## Confidence scoring
 *
 *   sentinel self-referential init    +0.35
 *   node heap allocation              +0.25
 *   chain traversal loop              +0.25
 *   four-pointer update (insert/erase)+0.15
 */

#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace container_detect {

namespace {

// Count Store instructions in fn.
static int countStores(const ssa::SSAFunction& fn) {
    int n = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs)
            if (instr && instr->op == ssa::IrInstr::Op::Store) ++n;
    }
    return n;
}

// A self-referential store is a Store where the destination address and the
// stored value are derived from the same base register / phi-node.
// We approximate this by looking for ≥2 Store instructions in the function
// entry block where the stored value feeds another memory slot in the same block
// (short-distance address reuse within 2 instructions).
static bool hasSentinelInit(const ssa::SSAFunction& fn) {
    if (fn.blockCount() == 0) return false;
    const auto* entry = fn.block(0);
    if (!entry) return false;

    int selfRefStores = 0;
    for (std::size_t i = 0; i + 1 < entry->instrs.size(); ++i) {
        const auto* a = entry->instrs[i];
        const auto* b = entry->instrs[i + 1];
        if (!a || !b) continue;
        if (a->op == ssa::IrInstr::Op::Store &&
            b->op == ssa::IrInstr::Op::Store) {
            // If both stores have overlapping use-sets (both reference the same
            // base address value), it's a sentinel init.
            if (!a->uses.empty() && !b->uses.empty() &&
                a->uses[1].valueId == b->uses[1].valueId + 1) {
                ++selfRefStores;
            }
        }
    }
    return selfRefStores >= 1;
}

// Node heap allocation: a malloc/new call followed by a pointer store.
static bool hasNodeAlloc(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = instr->calleeName;
            if (cn == "malloc" || cn == "_Znwm" || cn == "operator new" ||
                cn.find("allocate") != std::string::npos)
                return true;
        }
    }
    return false;
}

// Chain traversal: a loop that loads a pointer from an offset (the _next slot)
// and compares it to another pointer (the sentinel or nullptr).
// In practice we detect: ≥1 Load + ≥1 Compare + ≥1 back-edge.
static bool hasChainTraversal(const ssa::SSAFunction& fn) {
    int loads = 0, compares = 0, backEdges = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Load)    ++loads;
            if (instr->op == ssa::IrInstr::Op::Compare) ++compares;
        }
        // Back-edge: a successor block with a smaller index.
        for (uint32_t succ : blk->succs)
            if (succ <= b) ++backEdges;
    }
    return loads >= 1 && compares >= 1 && backEdges >= 1;
}

// Four-pointer update: insert or erase modifies 4 pointer slots.
// Approximation: ≥4 Store instructions in the function.
static bool hasFourPtrUpdate(const ssa::SSAFunction& fn) {
    return countStores(fn) >= 4;
}

} // anonymous namespace

// ─── ListDetector ────────────────────────────────────────────────────────────

ListEvidence ListDetector::analyseStructure(const ssa::SSAFunction& fn) const {
    ListEvidence ev;
    ev.hasSentinelNode   = hasSentinelInit(fn);
    ev.hasNodeAlloc      = hasNodeAlloc(fn);
    ev.hasChainTraversal = hasChainTraversal(fn);
    ev.hasFourPtrUpdate  = hasFourPtrUpdate(fn);
    ev.found = ev.hasSentinelNode || (ev.hasNodeAlloc && ev.hasChainTraversal);
    ev.confidence = scoreEvidence(ev);
    return ev;
}

float ListDetector::scoreEvidence(const ListEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasSentinelNode)    s += 0.35f;
    if (ev.hasNodeAlloc)       s += 0.25f;
    if (ev.hasChainTraversal)  s += 0.25f;
    if (ev.hasFourPtrUpdate)   s += 0.15f;
    return s > 1.0f ? 1.0f : s;
}

ContainerResult ListDetector::detect(const ssa::SSAFunction& fn) const {
    ContainerResult result;
    result.kind = ContainerKind::List;

    auto ev = analyseStructure(fn);
    result.confidence = ev.confidence;

    if (ev.confidence < 0.10f) return result;

    result.emittedType = "std::list<int>";
    result.elementType.kind = RecoveredType::Kind::Int32;

    if (ev.hasChainTraversal) {
        AccessPattern ap;
        ap.kind    = AccessKind::Iterate;
        ap.emitted = "for (auto& e : lst)";
        result.accessPatterns.push_back(ap);
    }
    if (ev.hasNodeAlloc) {
        AccessPattern ap;
        ap.kind    = AccessKind::PushBack;
        ap.emitted = "lst.push_back(elem)";
        result.accessPatterns.push_back(ap);
    }
    if (ev.hasFourPtrUpdate) {
        AccessPattern ap;
        ap.kind    = AccessKind::Erase;
        ap.emitted = "lst.erase(it)";
        result.accessPatterns.push_back(ap);
    }

    return result;
}

} // namespace container_detect
} // namespace retdec
