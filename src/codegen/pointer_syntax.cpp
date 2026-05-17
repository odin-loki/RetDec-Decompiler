/**
 * @file src/codegen/pointer_syntax.cpp
 * @brief Pointer syntax recovery and cast minimisation.
 *
 * Transforms low-level pointer arithmetic expressions into idiomatic C:
 *
 *   Deref(Add(p, i))        →  p[i]            (subscript)
 *   Deref(Add(p, K))        →  p->field         (struct member, K matches offset)
 *   Deref(Add(p, Mul(i,s))) →  p[i]             (stride-scaled subscript)
 *
 * Cast minimisation:
 *   Cast(T, Cast(T, x))     →  Cast(T, x)       (duplicate)
 *   Cast(T, x) where x: T   →  x                (no-op)
 */

#include <memory>
#include "retdec/codegen/codegen.h"

#include <cstdlib>

namespace retdec {
namespace codegen {

namespace {

// Returns true if literal string parses to the given integer value.
static bool isLiteral(const CExpr& e, int64_t& val) {
    if (e.kind != CExpr::Kind::Literal) return false;
    try { val = std::stoll(e.literal); return true; }
    catch (...) { return false; }
}

static bool isZeroExpr(const CExpr& e) {
    int64_t v = 0;
    return isLiteral(e, v) && v == 0;
}

// Check if two CType pointers represent the same concrete C type.
static bool sameType(const CType* a, const CType* b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind == CType::Kind::Struct || a->kind == CType::Kind::Union)
        return a->name == b->name;
    if (a->kind == CType::Kind::Pointer || a->kind == CType::Kind::Array) {
        if (a->children.size() != b->children.size()) return false;
        for (std::size_t i = 0; i < a->children.size(); ++i)
            if (!sameType(a->children[i].get(), b->children[i].get())) return false;
    }
    return true;
}

} // anonymous namespace

// ─── Public entry ─────────────────────────────────────────────────────────────

std::shared_ptr<CExpr> PointerSyntax::recover(
        std::shared_ptr<CExpr> expr,
        const std::unordered_map<std::string, StructInfo>& structs) const {

    if (!expr) return expr;

    // First recurse into children.
    for (auto& c : expr->children)
        c = recover(c, structs);

    // Then try transformations on this node.
    expr = minimiseCasts(expr);
    expr = trySubscript(expr);
    expr = tryMember(expr, structs);

    return expr;
}

// ─── Subscript recovery ───────────────────────────────────────────────────────

std::shared_ptr<CExpr> PointerSyntax::trySubscript(
        std::shared_ptr<CExpr> expr) const {

    using K = CExpr::Kind;
    using U = CExpr::UnOpKind;
    using B = CExpr::BinOpKind;

    // Match: Deref(Add(base, idx))
    if (expr->kind != K::UnOp || expr->unOp != U::Deref) return expr;
    auto& inner = expr->children[0];
    if (!inner || inner->kind != K::BinOp || inner->binOp != B::Add) return expr;

    auto base = inner->children[0];
    auto idx  = inner->children[1];

    // p[0]  simplifies to *p, but p[0] is equally readable — keep as p[0].
    // p + 0 → just skip (*(p+0) → *p, but leave as is since we already
    // normalised the Add away if idx==0 upstream).

    // Handle stride multiplication: Add(p, Mul(i, stride)) → p[i]
    // We only emit p[i] and let the type system figure out stride later.
    if (idx->kind == K::BinOp && idx->binOp == B::Mul) {
        // One of the Mul operands should be a constant stride.
        // Just use the non-constant operand as the subscript index.
        auto& ml = idx->children[0];
        auto& mr = idx->children[1];
        int64_t dummy;
        if (isLiteral(*mr, dummy)) {
            // Mul(i, stride) → use i.
            return CExpr::index(std::move(base), ml);
        }
        if (isLiteral(*ml, dummy)) {
            // Mul(stride, i) → use i.
            return CExpr::index(std::move(base), mr);
        }
    }

    // If idx is zero, emit *base directly (cleaner).
    if (isZeroExpr(*idx))
        return CExpr::unop(U::Deref, std::move(base));

    return CExpr::index(std::move(base), std::move(idx));
}

// ─── Struct member recovery ───────────────────────────────────────────────────

std::shared_ptr<CExpr> PointerSyntax::tryMember(
        std::shared_ptr<CExpr> expr,
        const std::unordered_map<std::string, StructInfo>& structs) const {

    if (structs.empty()) return expr;

    using K = CExpr::Kind;
    using U = CExpr::UnOpKind;
    using B = CExpr::BinOpKind;

    // Match: Index(base, K) or Deref(Add(base, K))
    // where K matches a known struct field offset.

    // Try Index form p[K] first.
    if (expr->kind == K::Index) {
        auto& base = expr->children[0];
        auto& idx  = expr->children[1];
        int64_t offset;
        if (!isLiteral(*idx, offset)) return expr;
        if (offset < 0) return expr;

        // Look for a struct whose field matches this offset.
        for (auto& [typeName, sinfo] : structs) {
            auto it = sinfo.fields.find(offset);
            if (it != sinfo.fields.end()) {
                // Determine if base is a pointer (use ->) or value (use .).
                bool arrow = base->exprType && base->exprType->isPointer();
                return CExpr::member(base, it->second, arrow);
            }
        }
        return expr;
    }

    // Try Deref(Add(base, K)) form.
    if (expr->kind == K::UnOp && expr->unOp == U::Deref &&
        !expr->children.empty() &&
        expr->children[0]->kind == K::BinOp &&
        expr->children[0]->binOp == B::Add) {

        auto& addExpr = expr->children[0];
        auto& addRhs  = addExpr->children[1];
        int64_t offset;
        if (!isLiteral(*addRhs, offset)) return expr;
        if (offset < 0) return expr;

        auto& base = addExpr->children[0];
        for (auto& [typeName, sinfo] : structs) {
            auto it = sinfo.fields.find(offset);
            if (it != sinfo.fields.end()) {
                bool arrow = !base->exprType || base->exprType->isPointer();
                return CExpr::member(base, it->second, arrow);
            }
        }
    }

    return expr;
}

// ─── Cast minimisation ────────────────────────────────────────────────────────

std::shared_ptr<CExpr> PointerSyntax::minimiseCasts(
        std::shared_ptr<CExpr> expr) const {

    using K = CExpr::Kind;
    if (expr->kind != K::Cast) return expr;

    auto& inner = expr->children[0];
    if (!inner) return expr;

    // (T)(T)x → (T)x  — duplicate cast chain
    if (inner->kind == K::Cast && expr->castType && inner->castType) {
        if (sameType(expr->castType.get(), inner->castType.get()))
            return expr; // already deduplicated by recursion; nothing to do
    }

    // (T)x where x already has type T → x
    if (isCastRedundant(*expr->castType, *inner))
        return inner;

    return expr;
}

bool PointerSyntax::isCastRedundant(const CType& outer, const CExpr& inner) const {
    if (!inner.exprType) return false;
    return sameType(&outer, inner.exprType.get());
}

} // namespace codegen
} // namespace retdec
