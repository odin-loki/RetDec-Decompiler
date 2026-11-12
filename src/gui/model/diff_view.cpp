/**
 * @file src/gui/model/diff_view.cpp
 * @brief Side-by-side / unified rows, hunks and statistics of a diff.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/diff_view.h"

#include <algorithm>
#include <format>

namespace retdec::gui::model {

namespace {

DiffCell cellOf(const DiffOp& op, DiffCellKind kind, bool leftSide)
{
	DiffCell c;
	c.kind = kind;
	c.text = op.line;
	c.line = leftSide ? op.leftLine : op.rightLine;
	return c;
}

// Intra-line spans of a changed pair, or none when most of both lines changed
// (the pair is then two different lines, and marking characters is noise).
IntraLineDiff pairSpans(const std::string& a, const std::string& b)
{
	IntraLineDiff d = intraLineDiff(a, b);
	std::size_t changed = 0;
	for (const auto& s: d.left)
		changed += s.end - s.begin;
	for (const auto& s: d.right)
		changed += s.end - s.begin;
	const std::size_t total = a.size() + b.size();
	if (total == 0 || changed * 10 > total * kIntraLineMaxChangedTenths) return {};
	return d;
}

DiffCell placeholder()
{
	DiffCell c;
	c.kind = DiffCellKind::Placeholder;
	return c;
}

// Calls f(deletes, inserts) for every maximal run of non-equal ops and
// g(op) for every equal op, in order.
template <class Run, class Eq>
void forEachRun(const DiffResult& diff, Run&& run, Eq&& eq)
{
	const auto& ops = diff.ops;
	std::size_t i = 0;
	while (i < ops.size())
	{
		if (ops[i].kind == DiffOpKind::Equal)
		{
			eq(ops[i]);
			++i;
			continue;
		}
		std::vector<const DiffOp*> dels, ins;
		std::size_t j = i;
		for (; j < ops.size() && ops[j].kind != DiffOpKind::Equal; ++j)
			(ops[j].kind == DiffOpKind::Delete ? dels : ins).push_back(&ops[j]);
		run(dels, ins);
		i = j;
	}
}

} // namespace

std::vector<SideBySideRow> buildSideBySide(const DiffResult& diff, bool pairChanges)
{
	std::vector<SideBySideRow> rows;
	rows.reserve(diff.ops.size());
	if (!pairChanges)
	{
		for (const auto& op: diff.ops)
		{
			SideBySideRow r;
			switch (op.kind)
			{
			case DiffOpKind::Equal:
				r.left = cellOf(op, DiffCellKind::Equal, true);
				r.right = cellOf(op, DiffCellKind::Equal, false);
				break;
			case DiffOpKind::Delete:
				r.left = cellOf(op, DiffCellKind::Removed, true);
				r.right = placeholder();
				break;
			case DiffOpKind::Insert:
				r.left = placeholder();
				r.right = cellOf(op, DiffCellKind::Added, false);
				break;
			}
			rows.push_back(std::move(r));
		}
		return rows;
	}
	forEachRun(
		diff,
		[&](const std::vector<const DiffOp*>& dels, const std::vector<const DiffOp*>& ins) {
			const std::size_t n = std::max(dels.size(), ins.size());
			for (std::size_t k = 0; k < n; ++k)
			{
				SideBySideRow r;
				r.left = k < dels.size() ? cellOf(*dels[k], DiffCellKind::Removed, true) : placeholder();
				r.right = k < ins.size() ? cellOf(*ins[k], DiffCellKind::Added, false) : placeholder();
				if (k < dels.size() && k < ins.size())
				{
					auto intra = pairSpans(r.left.text, r.right.text);
					r.left.spans = std::move(intra.left);
					r.right.spans = std::move(intra.right);
				}
				rows.push_back(std::move(r));
			}
		},
		[&](const DiffOp& op) {
			SideBySideRow r;
			r.left = cellOf(op, DiffCellKind::Equal, true);
			r.right = cellOf(op, DiffCellKind::Equal, false);
			rows.push_back(std::move(r));
		});
	return rows;
}

std::vector<UnifiedRow> buildUnified(const DiffResult& diff)
{
	std::vector<UnifiedRow> rows;
	rows.reserve(diff.ops.size());
	auto rowOf = [](const DiffOp& op) {
		UnifiedRow r;
		r.kind = op.kind;
		r.text = op.line;
		r.leftLine = op.leftLine;
		r.rightLine = op.rightLine;
		return r;
	};
	forEachRun(
		diff,
		[&](const std::vector<const DiffOp*>& dels, const std::vector<const DiffOp*>& ins) {
			// A run shows its removed lines, then its added ones (as `diff -u`
			// does); the k-th removed and k-th added line carry intra-line spans.
			std::vector<std::size_t> delRows, insRows;
			for (const DiffOp* d: dels)
			{
				delRows.push_back(rows.size());
				rows.push_back(rowOf(*d));
			}
			for (const DiffOp* in: ins)
			{
				insRows.push_back(rows.size());
				rows.push_back(rowOf(*in));
			}
			for (std::size_t k = 0; k < std::min(delRows.size(), insRows.size()); ++k)
			{
				auto intra = pairSpans(rows[delRows[k]].text, rows[insRows[k]].text);
				rows[delRows[k]].spans = std::move(intra.left);
				rows[insRows[k]].spans = std::move(intra.right);
			}
		},
		[&](const DiffOp& op) { rows.push_back(rowOf(op)); });
	return rows;
}

std::vector<int> diffHunkStarts(const std::vector<SideBySideRow>& rows)
{
	std::vector<int> out;
	bool in = false;
	for (std::size_t i = 0; i < rows.size(); ++i)
	{
		if (rows[i].changed())
		{
			if (!in) out.push_back(static_cast<int>(i));
			in = true;
		}
		else
			in = false;
	}
	return out;
}

std::vector<int> diffHunkStarts(const std::vector<UnifiedRow>& rows)
{
	std::vector<int> out;
	bool in = false;
	for (std::size_t i = 0; i < rows.size(); ++i)
	{
		if (rows[i].kind != DiffOpKind::Equal)
		{
			if (!in) out.push_back(static_cast<int>(i));
			in = true;
		}
		else
			in = false;
	}
	return out;
}

std::string diffStatsText(const DiffResult& diff)
{
	if (diff.isEmpty()) return "No diff";
	const int sim = static_cast<int>(diff.similarity * 100.0);
	return std::format("+{}  -{}  {}% similar", diff.linesAdded, diff.linesRemoved, sim);
}

void DiffHunkCursor::reset(std::vector<int> starts)
{
	starts_ = std::move(starts);
	cur_ = -1;
}

std::optional<int> DiffHunkCursor::next()
{
	if (starts_.empty()) return std::nullopt;
	cur_ = (cur_ + 1) % static_cast<int>(starts_.size());
	return starts_[static_cast<std::size_t>(cur_)];
}

std::optional<int> DiffHunkCursor::prev()
{
	if (starts_.empty()) return std::nullopt;
	const int n = static_cast<int>(starts_.size());
	cur_ = cur_ < 0 ? n - 1 : (cur_ - 1 + n) % n;
	return starts_[static_cast<std::size_t>(cur_)];
}

} // namespace retdec::gui::model
