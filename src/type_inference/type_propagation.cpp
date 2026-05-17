/**
 * @file src/type_inference/type_propagation.cpp
 * @brief Phase 2: union-find type propagation.
 *
 * ## Union-find type classes
 *
 * Each SSA value belongs to an equivalence class (TypeClass).  Each class
 * carries a partial IrType.  When two classes are merged, their types are
 * joined using a lattice:
 *
 *   Unknown  ⊔  T      = T        (any known type beats Unknown)
 *   Pointer  ⊔  Pointer = Pointer (compatible, merge pointee info)
 *   Integer  ⊔  Integer = Integer (take the one from higher-priority source)
 *   Float    ⊔  Float   = Float
 *   T1       ⊔  T2      = T1      (conflict: keep higher-priority, log)
 *
 * Priority levels (higher wins on conflict):
 *   0 — Unknown
 *   1 — Width seeding (instruction-derived)
 *   2 — Instruction semantics (ADD → integer, etc.)
 *   3 — ABI calling convention
 *   4 — Demangled symbol type
 *
 * ## Constraint application
 *
 *   HasWidth(v, w)     → set class(v).type.width = w
 *   IsPointer(v)       → set class(v).type.kind = Pointer
 *   IsUnsigned(v)      → set signedness = Unsigned
 *   IsSigned(v)        → set signedness = Signed
 *   IsFloat(v, w)      → set kind = Float, width = w
 *   IsBool(v)          → set kind = Bool, width = 1
 *   SameWidth(a, b)    → unite classes, propagate width
 *   SameSign(a, b)     → unite classes for signedness
 *   IsStruct(v, sid)   → set kind = Pointer, pointeeKind = Struct, structId = sid
 *   IsArray(v, n, ew, ek) → set kind = Pointer, pointeeKind = Array
 *   ReturnType/ParamType  → apply the given IrType at priority 3
 *
 * ## IrType::toString()
 *
 * Returns a C-like type string for display:
 *   Integer(32, Signed)   → "int32_t"
 *   Integer(64, Unsigned) → "uint64_t"
 *   Pointer               → "void*"
 *   Float(32)             → "float"
 *   Float(64)             → "double"
 *   Bool                  → "bool"
 *   Vector(4, 32, Float)  → "float32x4_t"
 */

#include "retdec/type_inference/type_inference.h"
#include <algorithm>
#include <cassert>
#include <sstream>
#include <unordered_set>

namespace retdec {
namespace type_inference {

// ─── IrType::toString ─────────────────────────────────────────────────────────

std::string IrType::toString() const {
    switch (kind) {
    case TypeKind::Unknown: return "unknown";
    case TypeKind::Void:    return "void";
    case TypeKind::Bool:    return "bool";
    case TypeKind::Float:
        if (width == 32)  return "float";
        if (width == 64)  return "double";
        if (width == 80)  return "long double";
        return "float" + std::to_string(width) + "_t";
    case TypeKind::Integer: {
        std::string s = (sign == Signedness::Unsigned) ? "u" : "";
        if (width == 0)   return s + "int_t";
        return s + "int" + std::to_string(width) + "_t";
    }
    case TypeKind::Pointer:
        if (pointeeKind == TypeKind::Struct)
            return "struct_" + std::to_string(structId) + "*";
        if (pointeeKind == TypeKind::Unknown)  return "void*";
        return "ptr";
    case TypeKind::Struct:
        return "struct_" + std::to_string(structId);
    case TypeKind::Array:
        return "array[" + std::to_string(arrayCount) + "]";
    case TypeKind::Vector:
        return "vec" + std::to_string(laneWidth) + "x" + std::to_string(laneCount);
    case TypeKind::Function:
        return "fn*";
    }
    return "?";
}

// ─── StructLayout helpers ─────────────────────────────────────────────────────

bool StructLayout::hasField(uint32_t offset) const {
    for (auto& f : fields) if (f.offset == offset) return true;
    return false;
}
void StructLayout::addField(StructField f) {
    if (!hasField(f.offset)) fields.push_back(std::move(f));
}
const StructField* StructLayout::field(uint32_t offset) const {
    for (auto& f : fields) if (f.offset == offset) return &f;
    return nullptr;
}

// ─── Union-find helpers ───────────────────────────────────────────────────────

void TypePropagation::addValue(uint32_t id) {
    if (id >= parent_.size()) {
        parent_.resize(id + 1, UINT32_MAX);
        rank_.resize(id + 1, 0);
        classes_.resize(id + 1);
    }
    if (parent_[id] == UINT32_MAX) {
        parent_[id] = id;
        rank_[id]   = 0;
    }
}

uint32_t TypePropagation::find(uint32_t x) const {
    if (x >= parent_.size() || parent_[x] == UINT32_MAX) return x;
    if (parent_[x] != x) parent_[x] = find(parent_[x]);
    return parent_[x];
}

void TypePropagation::unite(uint32_t x, uint32_t y) {
    uint32_t rx = find(x);
    uint32_t ry = find(y);
    if (rx == ry) return;
    addValue(rx); addValue(ry);

    TypeClass merged;
    mergeTypes(merged, classes_[rx]);
    mergeTypes(merged, classes_[ry]);

    if (rank_[rx] < rank_[ry]) std::swap(rx, ry);
    parent_[ry] = rx;
    if (rank_[rx] == rank_[ry]) ++rank_[rx];
    classes_[rx] = merged;
}

void TypePropagation::mergeTypes(TypeClass& dst, const TypeClass& src) {
    if (src.priority < dst.priority) return;
    if (src.type.kind == TypeKind::Unknown) return;

    if (dst.type.kind == TypeKind::Unknown || src.priority > dst.priority) {
        dst.type     = src.type;
        dst.priority = src.priority;
        return;
    }

    // Same priority: reconcile
    // Width: take wider
    if (src.type.width > dst.type.width) dst.type.width = src.type.width;
    // Signedness: prefer known over unknown
    if (dst.type.sign == Signedness::Unknown) dst.type.sign = src.type.sign;
    // Kind: prefer non-unknown
    if (dst.type.kind == TypeKind::Unknown) dst.type.kind = src.type.kind;
    // Pointer → struct upgrade
    if (dst.type.kind == TypeKind::Pointer &&
        src.type.pointeeKind != TypeKind::Unknown)
        dst.type.pointeeKind = src.type.pointeeKind;
    if (src.type.structId != UINT32_MAX) dst.type.structId = src.type.structId;
}

void TypePropagation::addConstraint(TypeConstraint c) {
    addValue(c.lhsId);
    if (c.rhsId != UINT32_MAX) addValue(c.rhsId);
    constraints_.push_back(std::move(c));
}

// ─── Constraint application ───────────────────────────────────────────────────

void TypePropagation::applyConstraint(const TypeConstraint& c) {
    uint32_t lid = find(c.lhsId);
    addValue(lid);

    switch (c.kind) {
    case ConstraintKind::HasWidth:
        if (classes_[lid].type.width == 0) {
            classes_[lid].type.width = c.width;
            if (classes_[lid].priority < 1) classes_[lid].priority = 1;
        }
        break;

    case ConstraintKind::IsPointer:
        if (classes_[lid].type.kind == TypeKind::Unknown) {
            classes_[lid].type.kind = TypeKind::Pointer;
            if (classes_[lid].priority < 2) classes_[lid].priority = 2;
        }
        break;

    case ConstraintKind::IsUnsigned:
        classes_[lid].type.sign = Signedness::Unsigned;
        if (classes_[lid].type.kind == TypeKind::Unknown)
            classes_[lid].type.kind = TypeKind::Integer;
        if (classes_[lid].priority < 2) classes_[lid].priority = 2;
        break;

    case ConstraintKind::IsSigned:
        classes_[lid].type.sign = Signedness::Signed;
        if (classes_[lid].type.kind == TypeKind::Unknown)
            classes_[lid].type.kind = TypeKind::Integer;
        if (classes_[lid].priority < 2) classes_[lid].priority = 2;
        break;

    case ConstraintKind::IsFloat:
        classes_[lid].type.kind = TypeKind::Float;
        if (c.width > 0) classes_[lid].type.width = c.width;
        if (classes_[lid].priority < 2) classes_[lid].priority = 2;
        break;

    case ConstraintKind::IsBool:
        classes_[lid].type.kind  = TypeKind::Bool;
        classes_[lid].type.width = 1;
        if (classes_[lid].priority < 2) classes_[lid].priority = 2;
        break;

    case ConstraintKind::IsStruct:
        classes_[lid].type.kind        = TypeKind::Pointer;
        classes_[lid].type.pointeeKind = TypeKind::Struct;
        classes_[lid].type.structId    = c.structId;
        if (classes_[lid].priority < 2) classes_[lid].priority = 2;
        break;

    case ConstraintKind::IsArray:
        classes_[lid].type.kind       = TypeKind::Pointer;
        classes_[lid].type.pointeeKind= TypeKind::Array;
        classes_[lid].type.arrayCount = c.arrayCount;
        classes_[lid].type.elemWidth  = c.elemWidth;
        classes_[lid].type.elemKind   = c.elemKind;
        if (classes_[lid].priority < 2) classes_[lid].priority = 2;
        break;

    case ConstraintKind::SameWidth:
        if (c.rhsId != UINT32_MAX) {
            uint32_t rid = find(c.rhsId);
            uint16_t wA = classes_[lid].type.width;
            uint16_t wB = (rid < classes_.size()) ? classes_[rid].type.width : 0;
            uint16_t w  = std::max(wA, wB);
            if (w > 0) {
                classes_[lid].type.width = w;
                if (rid < classes_.size()) classes_[rid].type.width = w;
            }
        }
        break;

    case ConstraintKind::SameSign:
        if (c.rhsId != UINT32_MAX) {
            uint32_t rid = find(c.rhsId);
            addValue(rid);
            Signedness sA = classes_[lid].type.sign;
            Signedness sB = classes_[rid].type.sign;
            if (sA != Signedness::Unknown) classes_[rid].type.sign = sA;
            else if (sB != Signedness::Unknown) classes_[lid].type.sign = sB;
        }
        break;

    case ConstraintKind::ReturnType:
    case ConstraintKind::ParamType:
        // Apply the type from constraint with priority 3 (ABI).
        // Only apply as integer if the current type is not already a higher-
        // fidelity kind (e.g. Float set by isFloat constraint must not be
        // overridden here since the ParamType constraint carries no kind info).
        {
            if (classes_[lid].type.kind != TypeKind::Float &&
                classes_[lid].type.kind != TypeKind::Pointer &&
                classes_[lid].type.kind != TypeKind::Bool) {
                TypeClass tc;
                tc.type     = c.sign != Signedness::Unknown
                              ? IrType::integer(c.width, c.sign)
                              : (c.width > 0 ? IrType::integer(c.width) : IrType::unknown());
                tc.priority = 3;
                mergeTypes(classes_[lid], tc);
            }
        }
        break;
    }
}

// ─── Main run ─────────────────────────────────────────────────────────────────

void TypePropagation::run() {
    // Apply all constraints once; SameWidth/SameSign/unite propagate transitively
    for (auto& c : constraints_)
        applyConstraint(c);
}

uint32_t TypePropagation::findRoot(uint32_t id) const { return find(id); }

const IrType& TypePropagation::typeOf(uint32_t id) const {
    uint32_t root = find(id);
    if (root >= classes_.size()) return kUnknown;
    return classes_[root].type;
}

std::size_t TypePropagation::classCount() const {
    std::unordered_set<uint32_t> roots;
    for (uint32_t i = 0; i < parent_.size(); ++i)
        if (parent_[i] != UINT32_MAX) roots.insert(find(i));
    return roots.size();
}

} // namespace type_inference
} // namespace retdec
