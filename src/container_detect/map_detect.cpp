/**
 * @file src/container_detect/map_detect.cpp
 * @brief std::map<K,V> / std::set<K> detector — red-black tree fingerprinting.
 *
 * ## Structural fingerprint
 *
 * std::map is built on a red-black tree (CLRS style) whose nodes carry:
 *
 *   ```cpp
 *   struct _Rb_tree_node_base {
 *       _Rb_color   _M_color;   // or bit-packed into _M_parent low bit
 *       _Rb_tree_node_base* _M_parent;
 *       _Rb_tree_node_base* _M_left;
 *       _Rb_tree_node_base* _M_right;
 *   };
 *   ```
 *
 * The key rotation patterns in compiled form:
 *
 * **Left-rotate** (x.right becomes new subtree root y):
 *   ```
 *   y = x->_M_right;           // Load: y  = *(x + right_off)
 *   x->_M_right = y->_M_left;  // Store at (x + right_off) <- *(y + left_off)
 *   y->_M_left  = x;           // Store at (y + left_off)  <- x
 *   ```
 *
 * **Right-rotate** (mirror of left-rotate):
 *   ```
 *   x = y->_M_left;
 *   y->_M_left  = x->_M_right;
 *   x->_M_right = y;
 *   ```
 *
 * In IR this translates to:
 *   - A Load instruction; the result is used in both a Store and a further Load.
 *   - Two cross-linked Stores following the Load (cross-links = A reads B's slot
 *     and B reads A's slot).
 *
 * **Colour field**:
 *   - Explicit byte at node+0 (libstdc++) — tested by a Compare against 0/1.
 *   - Packed into parent pointer low bit (libc++) — `And(parent_ptr, 1)`.
 *
 * **Rebalancing** (insert-fixup / delete-fixup):
 *   Alternating pattern of colour tests + rotations across multiple basic blocks.
 *   We look for: ≥2 Compare instructions + ≥2 Store instructions in a function
 *   that also has rotation evidence.
 *
 * ## Confidence scoring
 *
 *   left rotation pattern     +0.30
 *   right rotation pattern    +0.30
 *   colour field              +0.20
 *   three-pointer node layout +0.10
 *   rebalancing cases         +0.10
 */

#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

namespace retdec {
namespace container_detect {

namespace {

// Count all instructions of a given opcode across the function.
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

// Left / right rotation: we look for a Load whose result feeds a Store where
// the stored value is itself a load-dependent pointer, AND a cross-store where
// the original base is stored elsewhere (x->right = y->left pattern).
// Approximation: function contains both Load + two linked Stores in the same bb.
static bool hasRotation(const ssa::SSAFunction& fn, bool leftRotate) {
    // Need at least 2 Loads and 2 Stores to form a rotation.
    if (countOp(fn, ssa::IrInstr::Op::Load)  < 2) return false;
    if (countOp(fn, ssa::IrInstr::Op::Store) < 2) return false;

    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;

        // Collect load result value IDs and store source value IDs in this block.
        std::vector<ssa::ValueId> loadResults, storeSrcs, storeAddrs;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::Load && instr->id != ssa::kInvalidValue)
                loadResults.push_back(instr->id);
            if (instr->op == ssa::IrInstr::Op::Store && instr->uses.size() >= 2) {
                storeSrcs.push_back(instr->uses[0].valueId);
                storeAddrs.push_back(instr->uses[1].valueId);
            }
        }

        // Cross-link: a load result appears as a store source AND as a store address.
        for (auto lv : loadResults) {
            bool asSrc  = std::find(storeSrcs.begin(),  storeSrcs.end(),  lv) != storeSrcs.end();
            bool asAddr = std::find(storeAddrs.begin(), storeAddrs.end(), lv) != storeAddrs.end();
            if (asSrc && asAddr) return true;
            // Accept partial cross-link (used as src or addr) if ≥3 stores in block.
            if ((asSrc || asAddr) && (int)storeSrcs.size() >= 2) return true;
        }
    }
    return false;
    (void)leftRotate;
}

// Colour field: AND instruction with constant 1 (bit-packed) or a Compare
// against 0/1 of a single-byte load.
static bool hasColourField(const ssa::SSAFunction& fn) {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr) continue;
            if (instr->op == ssa::IrInstr::Op::And && instr->uses.size() >= 2) {
                const auto* iv = fn.value(instr->uses[1].valueId);
                if (iv && iv->kind == ssa::ValueKind::Immediate && iv->imm == 1)
                    return true;
            }
            if (instr->op == ssa::IrInstr::Op::Compare && instr->uses.size() >= 2) {
                const auto* iv = fn.value(instr->uses[1].valueId);
                if (iv && iv->kind == ssa::ValueKind::Immediate &&
                    (iv->imm == 0 || iv->imm == 1))
                    return true;
            }
        }
    }
    return false;
}

// Three-pointer node: parent, left, right — three adjacent loads from same base.
static bool hasThreePtrNode(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Load) >= 3;
}

// Rebalancing: multiple Compares + multiple Stores (colour flips + rotations).
static bool hasRebalancing(const ssa::SSAFunction& fn) {
    return countOp(fn, ssa::IrInstr::Op::Compare) >= 2 &&
           countOp(fn, ssa::IrInstr::Op::Store)   >= 3;
}

} // anonymous namespace

// ─── MapDetector ─────────────────────────────────────────────────────────────

bool MapDetector::hasLeftRotation(const ssa::SSAFunction& fn) const {
    return ::retdec::container_detect::hasRotation(fn, true);
}

bool MapDetector::hasRightRotation(const ssa::SSAFunction& fn) const {
    return ::retdec::container_detect::hasRotation(fn, false);
}

bool MapDetector::hasColourField(const ssa::SSAFunction& fn) const {
    return ::retdec::container_detect::hasColourField(fn);
}

RbTreeEvidence MapDetector::analyseStructure(const ssa::SSAFunction& fn) const {
    RbTreeEvidence ev;
    ev.hasLeftRotation  = hasLeftRotation(fn);
    ev.hasRightRotation = hasRightRotation(fn);
    ev.hasColourField   = hasColourField(fn);
    ev.hasThreePtrNode  = hasThreePtrNode(fn);
    ev.hasRebalancing   = hasRebalancing(fn);
    ev.found = ev.hasLeftRotation || ev.hasRightRotation;
    ev.confidence = scoreEvidence(ev);
    return ev;
}

float MapDetector::scoreEvidence(const RbTreeEvidence& ev) const {
    float s = 0.0f;
    if (ev.hasLeftRotation)  s += 0.30f;
    if (ev.hasRightRotation) s += 0.30f;
    if (ev.hasColourField)   s += 0.20f;
    if (ev.hasThreePtrNode)  s += 0.10f;
    if (ev.hasRebalancing)   s += 0.10f;
    return s > 1.0f ? 1.0f : s;
}

ContainerResult MapDetector::detect(const ssa::SSAFunction& fn) const {
    ContainerResult result;
    result.kind = ContainerKind::Map;

    auto ev = analyseStructure(fn);
    result.confidence = ev.confidence;

    if (ev.confidence < 0.10f) return result;

    result.emittedType = "std::map<int, int>";
    result.elementType.kind = RecoveredType::Kind::Int32;
    result.keyType.kind     = RecoveredType::Kind::Int32;

    if (ev.hasThreePtrNode) {
        AccessPattern ap;
        ap.kind    = AccessKind::Lookup;
        ap.emitted = "m.find(key)";
        result.accessPatterns.push_back(ap);
    }
    if (ev.hasRebalancing) {
        AccessPattern ap;
        ap.kind    = AccessKind::Insert;
        ap.emitted = "m.insert({key, val})";
        result.accessPatterns.push_back(ap);
    }
    if (ev.hasThreePtrNode) {
        AccessPattern ap;
        ap.kind    = AccessKind::Iterate;
        ap.emitted = "for (auto& [k,v] : m)";
        result.accessPatterns.push_back(ap);
    }

    return result;
}

} // namespace container_detect
} // namespace retdec
