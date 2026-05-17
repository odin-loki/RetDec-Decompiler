/**
 * @file src/codegen/loop_form.cpp
 * @brief Loop form selector: picks the most readable C loop form.
 *
 * Given a `StructNode` loop and pre-built CExpr for condition/init/incr,
 * produces the most appropriate C loop statement:
 *
 *   For   → `for (init; cond; incr) { body }`
 *   While → `while (cond) { body }`
 *   DoWhile → `do { body } while (cond);`
 *   Infinite → `while (1) { body }`
 *
 * Never emits `while (1) { if (!cond) break; }` when the condition is
 * directly available as a loop entry test.
 */

#include <memory>
#include "retdec/codegen/codegen.h"
#include "retdec/cfg_structure/cfg_structure.h"

namespace retdec {
namespace codegen {

std::shared_ptr<CStmt> LoopFormSelector::select(
        const cfg_structure::StructNode& loop,
        std::shared_ptr<CExpr>           cond,
        std::shared_ptr<CExpr>           init,
        std::shared_ptr<CExpr>           incr,
        std::shared_ptr<CStmt>           body) const {

    using NK = cfg_structure::StructNode::Kind;

    switch (loop.kind) {
    case NK::For: {
        if (cond && init && incr) {
            auto s = CStmt::forStmt(std::move(init), std::move(cond), std::move(incr));
            s->children.push_back(std::move(body));
            return s;
        }
        if (cond) {
            auto s = CStmt::whileStmt(std::move(cond));
            s->children.push_back(std::move(body));
            return s;
        }
        auto s = CStmt::whileStmt(CExpr::lit("1"));
        s->children.push_back(std::move(body));
        return s;
    }

    case NK::While: {
        if (cond) {
            auto s = CStmt::whileStmt(std::move(cond));
            s->children.push_back(std::move(body));
            return s;
        }
        auto s = CStmt::whileStmt(CExpr::lit("1"));
        s->children.push_back(std::move(body));
        return s;
    }

    case NK::DoWhile: {
        if (cond) {
            auto s = CStmt::doWhileStmt(std::move(cond));
            s->children.push_back(std::move(body));
            return s;
        }
        auto s = CStmt::doWhileStmt(CExpr::lit("1"));
        s->children.push_back(std::move(body));
        return s;
    }

    case NK::Infinite:
    default: {
        auto s = CStmt::whileStmt(CExpr::lit("1"));
        s->children.push_back(std::move(body));
        return s;
    }
    }
}

} // namespace codegen
} // namespace retdec
