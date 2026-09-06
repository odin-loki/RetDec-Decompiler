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

// Resolve the (base register, offset) pair a memory operand names.  Returns
// false for anything that is not a MemRef, so an operand we cannot place inside
// a node can never be mistaken for one of its slots.
static bool memSlot(const ssa::SSAFunction& fn, ssa::ValueId id,
                    ssa::VarId& base, int64_t& off) {
    const auto* v = fn.value(id);
    if (!v || v->kind != ssa::ValueKind::MemRef) return false;
    if (v->memBaseReg == ssa::kInvalidVar) return false;
    base = v->memBaseReg;
    off  = v->memOffset;
    return true;
}

// The pre-SSA variable a value came from, or kInvalidVar.
static ssa::VarId varOf(const ssa::SSAFunction& fn, ssa::ValueId id) {
    const auto* v = fn.value(id);
    return v ? v->varId : ssa::kInvalidVar;
}

// Rotation, as the file header describes it:
//
//   left_rotate:  y = x->_M_right; x->_M_right = y->_M_left; y->_M_left  = x;
//   right_rotate: x = y->_M_left;  y->_M_left  = x->_M_right; x->_M_right = y;
//
// Three things have to line up, and all three are about *slots*, not about
// instruction counts:
//   1. a Load reads a child slot (base x, offset Oc);
//   2. that same slot is written back  -- `x->child = ...`;
//   3. the loaded pointer y is linked the other way round, `y->other = x`,
//      i.e. a Store whose address hangs off y and whose stored value is x.
//
// Step 3 is what makes the pair a *cross*-link, and its offset is what makes
// the direction real: libstdc++ lays a node out as _M_parent, _M_left,
// _M_right in ascending order, so a left rotation promotes the higher child
// into the lower slot (Os < Oc) and a right rotation does the mirror image
// (Os > Oc).  The two are therefore mutually exclusive on one pair of stores,
// which they were not before: the old code ignored leftRotate entirely (the
// `(void)leftRotate` sat after an unconditional return) and awarded +0.30
// twice for the same evidence.
//
// The old evidence was not evidence at all.  It collected `instr->id` -- an
// InstrId -- into a list it then searched for store operand ValueIds, two
// unrelated numbering spaces that both start at 0 and count up, so any function
// with a couple of loads and a couple of stores collided by accident.  Combined
// with the "partial cross-link" escape hatch (a single collision plus two
// stores anywhere in the block) that scored 0.60 on ordinary loop code, and
// with the colour-field and rebalancing heuristics on top it reached 1.00 --
// which, because container_detector.cpp keeps the maximum-confidence answer,
// silently suppressed every other detector.
static bool hasRotation(const ssa::SSAFunction& fn, bool leftRotate) {
    // Need at least 2 Loads and 2 Stores to form a rotation.
    if (countOp(fn, ssa::IrInstr::Op::Load)  < 2) return false;
    if (countOp(fn, ssa::IrInstr::Op::Store) < 2) return false;

    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;

        for (const auto* ld : blk->instrs) {
            if (!ld || ld->op != ssa::IrInstr::Op::Load) continue;
            // A Load defines its result in defValue; an unrenamed Load defines
            // nothing and cannot be the y of a rotation.
            if (ld->defValue == ssa::kInvalidValue) continue;
            if (ld->uses.empty()) continue;

            ssa::VarId xBase    = ssa::kInvalidVar;  // node being demoted (x)
            int64_t    childOff = 0;                 // child slot y was read from
            if (!memSlot(fn, ld->uses[0].valueId, xBase, childOff)) continue;

            const ssa::VarId yVar = varOf(fn, ld->defValue);  // promoted child (y)
            if (yVar == ssa::kInvalidVar) continue;

            bool slotRewritten = false;  // x->child = ...
            bool crossLinked   = false;  // y->other = x, on the far side of Oc

            for (const auto* st : blk->instrs) {
                if (!st || st->op != ssa::IrInstr::Op::Store) continue;
                // (value, address); a Store with one operand has no address.
                if (st->uses.size() < 2) continue;

                ssa::VarId sBase = ssa::kInvalidVar;
                int64_t    sOff  = 0;
                if (!memSlot(fn, st->uses[1].valueId, sBase, sOff)) continue;

                if (sBase == xBase && sOff == childOff) slotRewritten = true;

                if (sBase == yVar
                    && varOf(fn, st->uses[0].valueId) == xBase
                    && sOff != childOff) {
                    crossLinked = leftRotate ? (sOff < childOff)
                                             : (sOff > childOff);
                }
            }

            if (slotRewritten && crossLinked) return true;
        }
    }
    return false;
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
