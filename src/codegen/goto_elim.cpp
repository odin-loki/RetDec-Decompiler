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

// True if `s` or anything under it assigns one of the flag variables, and if
// so which. The flag set may be nested -- `if (c) goto L;` becomes `if (c)
// { _flagL = 1; }` -- and only looking at the top level of a block meant such
// a goto produced no guard at all: the statements it was meant to skip ran
// unconditionally and the flag was dead.
static std::string flagSetIn(const CStmt* s, const std::unordered_map<std::string, std::string>& flagNames)
{
	if (!s) return {};
	if (s->kind == CStmt::Kind::Assign && s->lhs && s->lhs->kind == CExpr::Kind::Var)
	{
		for (const auto& [label, fname]: flagNames)
			if (s->lhs->varName == fname) return fname;
	}
	for (const auto& c: s->children)
	{
		std::string f = flagSetIn(c.get(), flagNames);
		if (!f.empty()) return f;
	}
	return {};
}

// Rewrite a statement tree:
//   - Replace `goto L` with `_flagL = 1` for labels in `toElim`
//   - Guard the statements between the goto and the label with `if (!_flagL)`
//   - Remove the Label node
//
// The label's position is what says where the guard ends, so labels survive
// until the enclosing Block is threaded below. They used to be replaced by an
// empty block on the way down, before any guard was built, so the guard was
// built from every remaining sibling -- including the statements *at and
// after* the label, which are the goto's destination and must always run. They
// were skipped exactly when the goto was taken.
static std::shared_ptr<CStmt> rewriteStmtTree(
        std::shared_ptr<CStmt> s,
        const std::unordered_set<std::string>& toElim,
        std::unordered_map<std::string, std::string>& flagNames) {

    if (!s) return s;

    // Replace goto → flag assignment.
    if (s->kind == CStmt::Kind::Goto && toElim.count(s->label)) {
        std::string flag = flagNames[s->label];
        // _flag = 1;
        return CStmt::assign(CExpr::var(flag), CExpr::lit("1"));
    }

    if (s->kind == CStmt::Kind::Block) {
		// Rewrite the children first, but leave eliminated labels in place:
		// this pass needs to see where they are.
		std::vector<std::shared_ptr<CStmt>> rewritten;
		rewritten.reserve(s->children.size());
		for (auto& c: s->children)
		{
			if (!c) continue;
			if (c->kind == CStmt::Kind::Label && toElim.count(c->label))
			{
				rewritten.push_back(c);
				continue;
			}
			rewritten.push_back(rewriteStmtTree(c, toElim, flagNames));
		}

		std::vector<std::shared_ptr<CStmt>> newChildren;
		std::vector<std::shared_ptr<CStmt>> guarded;
		std::string activeFlag;

		// Close the open guard, wrapping what has been collected for it.
		const auto closeGuard = [&]() {
			if (!guarded.empty())
			{
				auto guardBlock = CStmt::block();
				guardBlock->children = std::move(guarded);
				auto cond = CExpr::unop(CExpr::UnOpKind::Not, CExpr::var(activeFlag));
				auto ifStmt = CStmt::ifStmt(cond);
                ifStmt->children.push_back(guardBlock);
                newChildren.push_back(ifStmt);
			}
			guarded.clear();
			activeFlag.clear();
		};

		for (auto& child: rewritten)
		{
			if (!child) continue;

			// The label the flag was set for. Everything from here on runs
			// whatever the goto did, so the guard closes *before* it -- and
			// the label itself is dropped, its goto having become a flag.
			if (child->kind == CStmt::Kind::Label && toElim.count(child->label))
			{
				closeGuard();
				continue;
			}

			if (activeFlag.empty())
			{
				std::string setFlag = flagSetIn(child.get(), flagNames);
				newChildren.push_back(child);
				if (!setFlag.empty()) activeFlag = setFlag;
				continue;
			}

			guarded.push_back(child);
		}
		// A flag set with no label after it in this block: the destination is
		// in an enclosing statement, so the guard runs to the end of the block.
		closeGuard();

		s->children = std::move(newChildren);
		return s;
	}

	// Recurse into children.
	for (auto& c: s->children)
		c = rewriteStmtTree(c, toElim, flagNames);

	// An eliminated label whose parent is not a Block -- the sole body of an
	// if, say. There is nothing to guard, only the label to drop.
	if (s->kind == CStmt::Kind::Label && toElim.count(s->label))
	{
		return CStmt::block();
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
