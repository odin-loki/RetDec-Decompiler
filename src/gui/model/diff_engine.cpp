/**
 * @file src/gui/model/diff_engine.cpp
 * @brief Line and intra-line diffs.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/diff_engine.h"

#include <algorithm>
#include <sstream>

namespace retdec::gui::model {

// ─── DiffResult ──────────────────────────────────────────────────────────────

std::string DiffResult::toUnifiedDiff(const std::string& leftName, const std::string& rightName, int context) const
{
	std::ostringstream os;
	os << "--- " << leftName << "\n";
	os << "+++ " << rightName << "\n";

	const int n = static_cast<int>(ops.size());
	int i = 0;
	while (i < n)
	{
		while (i < n && ops[static_cast<std::size_t>(i)].kind == DiffOpKind::Equal)
			++i;
		if (i >= n) break;

		const int start = std::max(0, i - context);
		int end = i;
		while (end < n)
		{
			if (ops[static_cast<std::size_t>(end)].kind != DiffOpKind::Equal)
				end++;
			else
			{
				int eq = 0;
				for (int j = end; j < n && ops[static_cast<std::size_t>(j)].kind == DiffOpKind::Equal; ++j)
					eq++;
				if (eq <= 2 * context)
					end += eq;
				else
				{
					end += context;
					break;
				}
			}
		}
		end = std::min(end, n);

		int leftStart = 1, leftLen = 0, rightStart = 1, rightLen = 0;
		for (int j = 0; j < start; ++j)
		{
			if (ops[static_cast<std::size_t>(j)].kind != DiffOpKind::Insert) leftStart++;
			if (ops[static_cast<std::size_t>(j)].kind != DiffOpKind::Delete) rightStart++;
		}
		for (int j = start; j < end; ++j)
		{
			if (ops[static_cast<std::size_t>(j)].kind != DiffOpKind::Insert) leftLen++;
			if (ops[static_cast<std::size_t>(j)].kind != DiffOpKind::Delete) rightLen++;
		}

		os << "@@ -" << leftStart << "," << leftLen << " +" << rightStart << "," << rightLen << " @@\n";
		for (int j = start; j < end; ++j)
		{
			const auto& op = ops[static_cast<std::size_t>(j)];
			char prefix = ' ';
			if (op.kind == DiffOpKind::Delete) prefix = '-';
			if (op.kind == DiffOpKind::Insert) prefix = '+';
			os << prefix << op.line << "\n";
		}
		i = end;
	}
	return os.str();
}

std::string DiffResult::toHtml() const
{
	std::ostringstream os;
	os << "<html><body><pre style='font-family:monospace'>\n";
	for (const auto& op: ops)
	{
		std::string cls;
		if (op.kind == DiffOpKind::Delete)
			cls = "background:#3d1212;color:#ff9999";
		else if (op.kind == DiffOpKind::Insert)
			cls = "background:#122d12;color:#99ff99";
		else
			cls = "background:#1e1e2e;color:#cdd6f4";
		const char prefix = op.kind == DiffOpKind::Delete ? '-' : op.kind == DiffOpKind::Insert ? '+' : ' ';
		std::string escaped;
		for (char c: op.line)
		{
			if (c == '<')
				escaped += "&lt;";
			else if (c == '>')
				escaped += "&gt;";
			else if (c == '&')
				escaped += "&amp;";
			else
				escaped += c;
		}
		os << "<span style='" << cls << "'>" << prefix << escaped << "</span>\n";
	}
	os << "</pre></body></html>\n";
	return os.str();
}

// ─── Line diff ───────────────────────────────────────────────────────────────

namespace {

std::vector<std::vector<int>> buildLcsTable(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
	const std::size_t n = a.size(), m = b.size();
	std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
	for (std::size_t i = 1; i <= n; ++i)
	{
		const auto& ai = a[i - 1];
		for (std::size_t j = 1; j <= m; ++j)
		{
			if (ai == b[j - 1])
				dp[i][j] = dp[i - 1][j - 1] + 1;
			else
				dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
		}
	}
	return dp;
}

DiffOp makeOp(DiffOpKind k, const std::string& line, int l, int r)
{
	DiffOp op;
	op.kind = k;
	op.line = line;
	op.leftLine = l;
	op.rightLine = r;
	return op;
}

void emitOpsFromLcs(
	const std::vector<std::vector<int>>& dp,
	const std::vector<std::string>& a,
	const std::vector<std::string>& b,
	std::vector<DiffOp>& out)
{
	std::vector<DiffOp> rev;
	std::size_t i = a.size(), j = b.size();
	while (i > 0 && j > 0)
	{
		if (a[i - 1] == b[j - 1])
		{
			rev.push_back(makeOp(DiffOpKind::Equal, a[i - 1], int(i - 1), int(j - 1)));
			--i;
			--j;
		}
		else if (dp[i - 1][j] >= dp[i][j - 1])
		{
			rev.push_back(makeOp(DiffOpKind::Delete, a[i - 1], int(i - 1), -1));
			--i;
		}
		else
		{
			rev.push_back(makeOp(DiffOpKind::Insert, b[j - 1], -1, int(j - 1)));
			--j;
		}
	}
	while (i > 0)
	{
		rev.push_back(makeOp(DiffOpKind::Delete, a[i - 1], int(i - 1), -1));
		--i;
	}
	while (j > 0)
	{
		rev.push_back(makeOp(DiffOpKind::Insert, b[j - 1], -1, int(j - 1)));
		--j;
	}
	out.reserve(out.size() + rev.size());
	for (auto it = rev.rbegin(); it != rev.rend(); ++it)
		out.push_back(std::move(*it));
}

bool lcsOpsBounded(const std::vector<std::string>& a, const std::vector<std::string>& b, std::vector<DiffOp>& out)
{
	if (a.empty() && b.empty()) return true;
	const std::size_t cells = (a.size() + 1u) * (b.size() + 1u);
	const bool overflows = b.size() != 0 && (a.size() + 1u) > kMaxDiffCells / (b.size() + 1u);
	if (!overflows && cells <= kMaxDiffCells)
	{
		auto dp = buildLcsTable(a, b);
		emitOpsFromLcs(dp, a, b, out);
		return true;
	}
	for (std::size_t i = 0; i < a.size(); ++i)
		out.push_back(makeOp(DiffOpKind::Delete, a[i], int(i), -1));
	for (std::size_t j = 0; j < b.size(); ++j)
		out.push_back(makeOp(DiffOpKind::Insert, b[j], -1, int(j)));
	return false;
}

} // namespace

DiffResult MyersDiff::diff(const std::vector<std::string>& left, const std::vector<std::string>& right)
{
	DiffResult result;
	if (!left.empty() || !right.empty())
	{
		std::size_t pre = 0;
		while (pre < left.size() && pre < right.size() && left[pre] == right[pre])
			++pre;
		std::size_t suf = 0;
		while (suf < left.size() - pre && suf < right.size() - pre
			   && left[left.size() - 1 - suf] == right[right.size() - 1 - suf])
			++suf;

		for (std::size_t i = 0; i < pre; ++i)
			result.ops.push_back(makeOp(DiffOpKind::Equal, left[i], int(i), int(i)));

		const std::vector<std::string> midL(
			left.begin() + static_cast<std::ptrdiff_t>(pre), left.end() - static_cast<std::ptrdiff_t>(suf));
		const std::vector<std::string> midR(
			right.begin() + static_cast<std::ptrdiff_t>(pre), right.end() - static_cast<std::ptrdiff_t>(suf));
		std::vector<DiffOp> mid;
		result.exact = lcsOpsBounded(midL, midR, mid);
		for (auto& op: mid)
		{
			if (op.leftLine >= 0) op.leftLine += static_cast<int>(pre);
			if (op.rightLine >= 0) op.rightLine += static_cast<int>(pre);
			result.ops.push_back(std::move(op));
		}

		for (std::size_t i = 0; i < suf; ++i)
		{
			const std::size_t li = left.size() - suf + i, ri = right.size() - suf + i;
			result.ops.push_back(makeOp(DiffOpKind::Equal, left[li], int(li), int(ri)));
		}
	}
	for (const auto& op: result.ops)
	{
		if (op.kind == DiffOpKind::Insert)
			result.linesAdded++;
		else if (op.kind == DiffOpKind::Delete)
			result.linesRemoved++;
		else
			result.linesEqual++;
	}
	const int total = result.linesAdded + result.linesRemoved + result.linesEqual;
	result.similarity = total > 0 ? static_cast<double>(result.linesEqual) / total : 1.0;
	return result;
}

std::vector<std::string> MyersDiff::splitLines(const std::string& text)
{
	std::vector<std::string> lines;
	std::istringstream ss(text);
	std::string line;
	while (std::getline(ss, line))
		lines.push_back(line);
	return lines;
}

DiffResult MyersDiff::diffText(const std::string& leftText, const std::string& rightText)
{
	return diff(splitLines(leftText), splitLines(rightText));
}

// ─── Intra-line diff ─────────────────────────────────────────────────────────

namespace {

// Byte offsets of each code point start, plus the end.
std::vector<std::size_t> codePoints(std::string_view s)
{
	std::vector<std::size_t> cps;
	std::size_t i = 0;
	while (i < s.size())
	{
		cps.push_back(i);
		const unsigned char c = static_cast<unsigned char>(s[i]);
		const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
		i = std::min(s.size(), i + len);
	}
	cps.push_back(s.size());
	return cps;
}

std::string_view cpAt(std::string_view s, const std::vector<std::size_t>& cps, std::size_t k)
{
	return s.substr(cps[k], cps[k + 1] - cps[k]);
}

void addSpan(std::vector<DiffSpan>& spans, std::size_t b, std::size_t e)
{
	if (b >= e) return;
	if (!spans.empty() && spans.back().end == b)
		spans.back().end = e;
	else
		spans.push_back({b, e});
}

} // namespace

IntraLineDiff intraLineDiff(std::string_view a, std::string_view b, std::size_t maxChars)
{
	IntraLineDiff out;
	const auto ca = codePoints(a), cb = codePoints(b);
	const std::size_t n = ca.size() - 1, m = cb.size() - 1;

	std::size_t pre = 0;
	while (pre < n && pre < m && cpAt(a, ca, pre) == cpAt(b, cb, pre))
		++pre;
	std::size_t suf = 0;
	while (suf < n - pre && suf < m - pre && cpAt(a, ca, n - 1 - suf) == cpAt(b, cb, m - 1 - suf))
		++suf;

	const std::size_t mn = n - pre - suf, mm = m - pre - suf;
	if (mn == 0 || mm == 0 || mn > maxChars || mm > maxChars)
	{
		addSpan(out.left, ca[pre], ca[n - suf]);
		addSpan(out.right, cb[pre], cb[m - suf]);
		return out;
	}

	// LCS over the middle code points.
	std::vector<std::vector<int>> dp(mn + 1, std::vector<int>(mm + 1, 0));
	for (std::size_t i = 1; i <= mn; ++i)
		for (std::size_t j = 1; j <= mm; ++j)
			dp[i][j] = cpAt(a, ca, pre + i - 1) == cpAt(b, cb, pre + j - 1) ? dp[i - 1][j - 1] + 1
																			 : std::max(dp[i - 1][j], dp[i][j - 1]);
	std::vector<bool> keepA(mn, false), keepB(mm, false);
	for (std::size_t i = mn, j = mm; i > 0 && j > 0;)
	{
		if (cpAt(a, ca, pre + i - 1) == cpAt(b, cb, pre + j - 1))
		{
			keepA[i - 1] = keepB[j - 1] = true;
			--i;
			--j;
		}
		else if (dp[i - 1][j] >= dp[i][j - 1])
			--i;
		else
			--j;
	}
	for (std::size_t k = 0; k < mn; ++k)
		if (!keepA[k]) addSpan(out.left, ca[pre + k], ca[pre + k + 1]);
	for (std::size_t k = 0; k < mm; ++k)
		if (!keepB[k]) addSpan(out.right, cb[pre + k], cb[pre + k + 1]);
	return out;
}

} // namespace retdec::gui::model
