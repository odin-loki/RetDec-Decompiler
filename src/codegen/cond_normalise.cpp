/**
 * @file src/codegen/cond_normalise.cpp
 * @brief Condition normalisation pass.
 *
 * Applies algebraic rewrites to boolean/comparison expressions for readability.
 *
 * Rules applied bottom-up on the expression tree:
 *
 *   NOT(a >= b)  →  a < b
 *   NOT(a > b)   →  a <= b
 *   NOT(a == b)  →  a != b
 *   NOT(a != b)  →  a == b
 *   NOT(a <= b)  →  a > b
 *   NOT(a < b)   →  a >= b
 *   NOT(NOT(a))  →  a
 *   NOT(NOT(a))  →  a           (double negation)
 *
 * In boolean context (condition of if/while/for):
 *   (a != 0)     →  a
 *   (a == 0)     →  !a
 *   (0 != a)     →  a
 *   (0 == a)     →  !a
 *
 * Bit-test:
 *   (a & pow2) != 0  →  a & pow2   (already short, leave as is)
 *   ((a >> k) & 1) != 0  →  (a >> k) & 1
 */

#include <memory>
#include "retdec/codegen/codegen.h"

#include <cstdlib>
#include <cstring>

namespace retdec {
namespace codegen {

namespace {

// Returns true if the expression is the integer literal zero.
static bool isZero(const CExpr& e) {
    return e.kind == CExpr::Kind::Literal && e.literal == "0";
}

// Returns true if the expression is a power-of-two integer literal.
static bool isPow2Literal(const CExpr& e) {
    if (e.kind != CExpr::Kind::Literal) return false;
    int64_t v = 0;
    try { v = std::stoll(e.literal); } catch (...) { return false; }
    if (v <= 0) return false;
    return (v & (v - 1)) == 0;
}

// Flip a comparison operator to its negation.
static CExpr::BinOpKind flipCmpOp(CExpr::BinOpKind op) {
    using B = CExpr::BinOpKind;
    switch (op) {
    case B::Eq: return B::Ne;
    case B::Ne: return B::Eq;
    case B::Lt: return B::Ge;
    case B::Le: return B::Gt;
    case B::Gt: return B::Le;
    case B::Ge: return B::Lt;
    default:    return op;
    }
}

static bool isCmpOp(CExpr::BinOpKind op) {
    using B = CExpr::BinOpKind;
    return op == B::Eq || op == B::Ne ||
           op == B::Lt || op == B::Le ||
           op == B::Gt || op == B::Ge;
}

// Forward declaration.
static std::shared_ptr<CExpr> norm(std::shared_ptr<CExpr> e, bool boolCtx);

// Apply normalisation to all children of e, then to e itself.
static std::shared_ptr<CExpr> normChildren(std::shared_ptr<CExpr> e, bool boolCtx) {
    // We always normalise children with boolCtx=false unless e is a boolean op
    // whose children should also be in boolean context.
    bool childBool = (e->kind == CExpr::Kind::BinOp &&
                      (e->binOp == CExpr::BinOpKind::LAnd ||
                       e->binOp == CExpr::BinOpKind::LOr));
    for (auto& c : e->children)
        c = norm(c, childBool);
    return e;
}

static std::shared_ptr<CExpr> norm(std::shared_ptr<CExpr> e, bool boolCtx) {
    if (!e) return e;

    using K = CExpr::Kind;
    using B = CExpr::BinOpKind;
    using U = CExpr::UnOpKind;

    // Bottom-up: recurse first.
    e = normChildren(e, boolCtx);

    // ── NOT elimination ──────────────────────────────────────────────────────
    if (e->kind == K::UnOp && e->unOp == U::Not && !e->children.empty()) {
        auto& inner = e->children[0];

        // NOT(NOT(x)) → x
        if (inner->kind == K::UnOp && inner->unOp == U::Not)
            return inner->children[0];

        // NOT(cmp) → flipped cmp
        if (inner->kind == K::BinOp && isCmpOp(inner->binOp)) {
            inner->binOp = flipCmpOp(inner->binOp);
            return inner;
        }
    }

    // ── Boolean context: simplify (x != 0) and (x == 0) ─────────────────────
    if (boolCtx && e->kind == K::BinOp &&
        (e->binOp == B::Ne || e->binOp == B::Eq) &&
        e->children.size() == 2) {

        bool rightZero = isZero(*e->children[1]);
        bool leftZero  = isZero(*e->children[0]);

        if (e->binOp == B::Ne) {
            // (x != 0) → x,  (0 != x) → x
            if (rightZero) return e->children[0];
            if (leftZero)  return e->children[1];
        } else {
            // (x == 0) → !x,  (0 == x) → !x
            if (rightZero)
                return CExpr::unop(U::Not, e->children[0]);
            if (leftZero)
                return CExpr::unop(U::Not, e->children[1]);
        }
    }

    // ── Bit-test: (a & pow2) != 0 stays as (a & pow2), normalise to !!(x) ──
    // We keep it as-is (already minimal); the `!= 0` is stripped in bool ctx.

    return e;
}

} // anonymous namespace

std::shared_ptr<CExpr> CondNormaliser::normalise(
        std::shared_ptr<CExpr> expr, bool boolContext) const {
    return norm(std::move(expr), boolContext);
}

std::shared_ptr<CExpr> CondNormaliser::flipCmp(std::shared_ptr<CExpr> e) const {
    if (!e || e->kind != CExpr::Kind::BinOp) return e;
    if (!isCmpOp(e->binOp)) return e;
    e->binOp = flipCmpOp(e->binOp);
    return e;
}

bool CondNormaliser::isPowerOfTwo(std::shared_ptr<CExpr> e) const {
    return e && isPow2Literal(*e);
}

} // namespace codegen
} // namespace retdec
