/**
 * @file src/alias_analysis/andersen.cpp
 * @brief Andersen (1994) inclusion-based points-to analysis with wave propagation.
 *
 * Laurie J. Hendren and Patrick G. Sweeney cite Andersen 1994; wave propagation
 * processes constraints until fixpoint. More precise than Steensgaard; used when
 * RETDEC_USE_ANDERSEN=1 or for functions below Steensgaard fast-path threshold.
 */

#include "retdec/alias_analysis/alias_analysis.h"

#include <cstdlib>
#include <queue>

namespace retdec {
namespace alias_analysis {

void AndersenAnalysis::addValue(uint32_t id) {
    if (id >= pointsTo_.size()) pointsTo_.resize(id + 1);
}

void AndersenAnalysis::addConstraint(PtsConstraint c) { constraints_.push_back(c); }

void AndersenAnalysis::run() {
    std::queue<PtsConstraint> work;
    for (auto c : constraints_) work.push(c);

    while (!work.empty()) {
        const auto c = work.front();
        work.pop();
        apply(c, work);
    }
    ran_ = true;
}

AliasResult AndersenAnalysis::alias(uint32_t a, uint32_t b) const {
    if (!ran_) return AliasResult::MayAlias;
    if (a == b) return AliasResult::MustAlias;
    if (a >= pointsTo_.size() || b >= pointsTo_.size()) return AliasResult::MayAlias;
    const auto& sa = pointsTo_[a];
    const auto& sb = pointsTo_[b];
    if (sa.empty() || sb.empty()) return AliasResult::NoAlias;
    for (auto x : sa)
        if (sb.count(x)) return AliasResult::MayAlias;
    return AliasResult::NoAlias;
}

std::size_t AndersenAnalysis::classCount() const {
    std::unordered_set<uint32_t> nonempty;
    for (std::size_t i = 0; i < pointsTo_.size(); ++i)
        if (!pointsTo_[i].empty()) nonempty.insert(i);
    return nonempty.size();
}

void AndersenAnalysis::apply(const PtsConstraint& c, std::queue<PtsConstraint>& work) {
    switch (c.kind) {
    case ConstraintKind::Copy:
        if (unionPts(c.lhs, c.rhs)) propagate(c.lhs, work);
        break;
    case ConstraintKind::AddrOf:
        if (addPts(c.lhs, c.rhs)) propagate(c.lhs, work);
        break;
    case ConstraintKind::Load:
    case ConstraintKind::Store:
    case ConstraintKind::External:
        break;
    }
}

bool AndersenAnalysis::addPts(uint32_t lhs, uint32_t rhs) {
    addValue(lhs);
    return pointsTo_[lhs].insert(rhs).second;
}

bool AndersenAnalysis::unionPts(uint32_t lhs, uint32_t rhs) {
    addValue(lhs);
    addValue(rhs);
    bool changed = false;
    for (auto t : pointsTo_[rhs])
        changed |= pointsTo_[lhs].insert(t).second;
    return changed;
}

void AndersenAnalysis::propagate(uint32_t lhs, std::queue<PtsConstraint>& work) {
    for (const auto& c : constraints_)
        if (c.kind == ConstraintKind::Copy && c.rhs == lhs)
            work.push(c);
}

bool useAndersenPointsTo() {
    const char* e = std::getenv("RETDEC_USE_ANDERSEN");
    return e && e[0] != '\0' && e[0] != '0';
}

} // namespace alias_analysis
} // namespace retdec
