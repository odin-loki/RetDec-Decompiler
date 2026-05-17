/**
 * @file src/codegen/goto_elim.cpp
 * @brief Goto elimination via Erosa-Hendren flag variables.
 *
 * For each remaining `goto` in the statement tree targeting label L:
 *   1. Count how many times the flag variable for L would appear (one per
 *      goto site + one guard per intervening block containing L).
 *   2. If uses <= kMaxFlagUses (2):
 *        Introduce `int _flagL = 0;` at function entry.
 *        Replace `goto L;` with `_flagL = 1;`.
 *        Wrap all statements between the goto and the label in
 *        `if (!_flagL) { ... }`.
 *        Remove the label statement.
 *   3. If uses > kMaxFlagUses → keep the `goto` (irreducible region).
 *
 * This is a simplified Erosa-Hendren approach sufficient for typical
 * decompiler output where gotos arise from irreducible CFG remnants.
 */

#include <memory>
#include "retdec/codegen/codegen.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace retdec {
namespace codegen {

namespace {

// Count goto statements in a subtree, grouped by target label.
static void countGotosInTree(const CStmt* s,
                              std::unordered_map<std::string, int>& counts) {
    if (!s) return;
    if (s->kind == CStmt::Kind::Goto)
        ++counts[s->label];
    for (auto& c : s->children)
        countGotosInTree(c.get(), counts);
}

// Count label occurrences in a subtree.
static void countLabelsInTree(const CStmt* s,
                               std::unordered_set<std::string>& found) {
    if (!s) return;
    if (s->kind == CStmt::Kind::Label)
        found.insert(s->label);
    for (auto& c : s->children)
        countLabelsInTree(c.get(), found);
}

// Rewrite a statement tree:
//   - Replace `goto L` with `_flagL = 1` for labels in `toElim`
//   - Remove Label nodes for labels in `toElim`
//   - Wrap sequences between goto and label in `if (!_flagL)` guards
//     (simplified: we wrap the entire block body after each target label in
//     an if-guard — this is safe and covers the common single-forward-goto case)
static std::shared_ptr<CStmt> rewriteStmtTree(
        std::shared_ptr<CStmt> s,
        const std::unordered_set<std::string>& toElim,
        std::unordered_map<std::string, std::string>& flagNames) {

    if (!s) return s;

    // Recurse first into children.
    for (auto& c : s->children)
        c = rewriteStmtTree(c, toElim, flagNames);

    // Replace goto → flag assignment.
    if (s->kind == CStmt::Kind::Goto && toElim.count(s->label)) {
        std::string flag = flagNames[s->label];
        // _flag = 1;
        return CStmt::assign(CExpr::var(flag), CExpr::lit("1"));
    }

    // Remove eliminated labels.
    if (s->kind == CStmt::Kind::Label && toElim.count(s->label)) {
        // Replace with a no-op block.
        return CStmt::block();
    }

    // For Block statements: wrap segments after an eliminated label.
    // Simplified: we already removed the label; the guard wrapping is handled
    // at the block level by rewriting children.
    // The simple approach: after rewriting children, scan for goto-flag-set
    // instructions and guard subsequent children with if(!flag).
    if (s->kind == CStmt::Kind::Block) {
        // Find flags set in this block and guard subsequent code.
        std::vector<std::shared_ptr<CStmt>> newChildren;
        std::unordered_set<std::string> activeFlags;

        for (std::size_t i = 0; i < s->children.size(); ++i) {
            auto& child = s->children[i];
            if (!child) continue;

            // Detect `_flagL = 1` assignments just inserted.
            bool isFlagSet = false;
            std::string setFlag;
            if (child->kind == CStmt::Kind::Assign && child->lhs &&
                child->lhs->kind == CExpr::Kind::Var) {
                for (auto& [label, fname] : flagNames) {
                    if (child->lhs->varName == fname) {
                        isFlagSet = true;
                        setFlag = fname;
                        break;
                    }
                }
            }

            if (isFlagSet) {
                newChildren.push_back(child);
                activeFlags.insert(setFlag);
            } else if (!activeFlags.empty()) {
                // Collect remaining children into a guarded block.
                auto guardBlock = CStmt::block();
                guardBlock->children.push_back(child);
                for (std::size_t j = i + 1; j < s->children.size(); ++j)
                    if (s->children[j])
                        guardBlock->children.push_back(s->children[j]);

                // Guard with !(_flag) for each active flag.
                // Use the first active flag for simplicity.
                std::string flagVar = *activeFlags.begin();
                auto cond = CExpr::unop(CExpr::UnOpKind::Not, CExpr::var(flagVar));
                auto ifStmt = CStmt::ifStmt(cond);
                ifStmt->children.push_back(guardBlock);
                newChildren.push_back(ifStmt);
                break;
            } else {
                newChildren.push_back(child);
            }
        }
        s->children = std::move(newChildren);
    }

    return s;
}

} // anonymous namespace

std::shared_ptr<CStmt> GotoEliminator::eliminate(
        std::shared_ptr<CStmt> body) const {

    if (!body) return body;

    // Step 1: count goto→label pairs.
    std::unordered_map<std::string, int> gotoCounts;
    countGotosInTree(body.get(), gotoCounts);

    if (gotoCounts.empty()) return body; // no gotos, nothing to do

    // Step 2: check labels exist in the tree (forward gotos only).
    std::unordered_set<std::string> definedLabels;
    countLabelsInTree(body.get(), definedLabels);

    // Step 3: decide which gotos to eliminate.
    // A label is eligible for elimination if:
    //   - It is defined in this tree (forward jump).
    //   - Number of goto sites <= kMaxFlagUses.
    //   (backward gotos typically become loops; they should not reach here)
    std::unordered_set<std::string> toElim;
    std::unordered_map<std::string, std::string> flagNames;

    for (auto& [label, cnt] : gotoCounts) {
        if (!definedLabels.count(label)) continue; // no label in tree
        if (cnt <= kMaxFlagUses) {
            toElim.insert(label);
            flagNames[label] = "_flag_" + label;
        }
    }

    if (toElim.empty()) return body; // all gotos are irreducible

    // Step 4: rewrite the tree.
    body = rewriteStmtTree(std::move(body), toElim, flagNames);

    // Step 5: prepend flag declarations to the function body Block.
    if (body->kind == CStmt::Kind::Block) {
        std::vector<std::shared_ptr<CStmt>> decls;
        for (auto& [label, fname] : flagNames) {
            if (toElim.count(label)) {
                auto decl = CStmt::declStmt(fname,
                                             CType::make(CType::Kind::Int32),
                                             CExpr::lit("0"));
                decls.push_back(std::move(decl));
            }
        }
        // Prepend decls.
        body->children.insert(body->children.begin(),
                               decls.begin(), decls.end());
    }

    return body;
}

void GotoEliminator::countGotos(const CStmt* s,
                                  std::unordered_map<std::string, GotoInfo>& info) const {
    if (!s) return;
    if (s->kind == CStmt::Kind::Goto)
        ++info[s->label].flagUseCount;
    for (auto& c : s->children)
        countGotos(c.get(), info);
}

std::shared_ptr<CStmt> GotoEliminator::rewrite(
        std::shared_ptr<CStmt> s,
        const std::unordered_set<std::string>& eliminate,
        std::unordered_map<std::string, std::string>& flags) const {
    return rewriteStmtTree(std::move(s), eliminate, flags);
}

} // namespace codegen
} // namespace retdec
