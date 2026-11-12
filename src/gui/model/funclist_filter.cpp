/**
 * @file src/gui/model/funclist_filter.cpp
 * @brief Function list filtering / sorting.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/funclist_filter.h"

#include <algorithm>
#include <cctype>

namespace retdec::gui::model {

namespace {

char lower(char c)
{
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Parses a [...] class at glob[gi] (gi at '['); returns the index after ']' or
// npos when the bracket is unterminated (then '[' is a literal).
std::size_t matchClass(std::string_view glob, std::size_t gi, char c, bool& matched)
{
	std::size_t i = gi + 1;
	bool negate = false;
	if (i < glob.size() && (glob[i] == '!' || glob[i] == '^'))
	{
		negate = true;
		++i;
	}
	bool any = false;
	bool first = true;
	for (; i < glob.size(); ++i)
	{
		if (glob[i] == ']' && !first)
		{
			matched = negate ? !any : any;
			return i + 1;
		}
		first = false;
		char lo = glob[i];
		char hi = lo;
		if (i + 2 < glob.size() && glob[i + 1] == '-' && glob[i + 2] != ']')
		{
			hi = glob[i + 2];
			i += 2;
		}
		const char lc = lower(c);
		if ((lower(lo) <= lc && lc <= lower(hi)) || (lo <= c && c <= hi)) any = true;
	}
	return std::string_view::npos;
}

bool wildRec(std::string_view g, std::size_t gi, std::string_view t, std::size_t ti)
{
	while (gi < g.size())
	{
		const char gc = g[gi];
		if (gc == '*')
		{
			while (gi < g.size() && g[gi] == '*')
				++gi;
			if (gi == g.size()) return t.find('/', ti) == std::string_view::npos;
			for (std::size_t k = ti; k <= t.size(); ++k)
			{
				if (wildRec(g, gi, t, k)) return true;
				if (k < t.size() && t[k] == '/') break;
			}
			return false;
		}
		if (ti >= t.size()) return false;
		if (gc == '?')
		{
			if (t[ti] == '/') return false;
			++gi;
			++ti;
			continue;
		}
		if (gc == '[')
		{
			bool m = false;
			const std::size_t next = matchClass(g, gi, t[ti], m);
			if (next != std::string_view::npos)
			{
				if (!m) return false;
				gi = next;
				++ti;
				continue;
			}
		}
		if (lower(gc) != lower(t[ti])) return false;
		++gi;
		++ti;
	}
	return ti == t.size();
}

bool startsWith(std::string_view s, std::string_view p)
{
	return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

int ciCompare(std::string_view a, std::string_view b)
{
	const std::size_t n = std::min(a.size(), b.size());
	for (std::size_t i = 0; i < n; ++i)
	{
		const char x = lower(a[i]), y = lower(b[i]);
		if (x != y) return x < y ? -1 : 1;
	}
	if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
	return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
}

int patternCount(const PatternFlags& p)
{
	return int(p.stl) + int(p.crypto) + int(p.algo) + int(p.design) + int(p.library);
}

} // namespace

bool funclistWildcardMatch(std::string_view glob, std::string_view text)
{
	return wildRec(glob, 0, text, 0);
}

std::optional<uint64_t> funclistParseHex(std::string_view s)
{
	auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
	while (!s.empty() && blank(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && blank(s.back()))
		s.remove_suffix(1);
	if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.remove_prefix(2);
	if (s.empty() || s.size() > 16) return std::nullopt;
	uint64_t v = 0;
	for (char c: s)
	{
		int d;
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			return std::nullopt;
		v = (v << 4) | static_cast<uint64_t>(d);
	}
	return v;
}

bool FunctionFilter::accepts(const FunctionEntry& e) const
{
	if (addrLow != 0 && e.address < addrLow) return false;
	if (addrHigh != 0 && e.address > addrHigh) return false;
	if (e.confidence.composite() < minConfidence) return false;
	if (anyPattern())
	{
		const bool match = (stl && e.patterns.stl) || (crypto && e.patterns.crypto) || (algo && e.patterns.algo)
						|| (design && e.patterns.design) || (lib && e.patterns.library);
		if (!match) return false;
	}
	if (!classFilter.empty())
	{
		const std::string prefix = classFilter + "::";
		if (!startsWith(e.name, prefix) && !startsWith(e.rawName, prefix)) return false;
	}
	if (!nameGlob.empty())
	{
		if (!funclistWildcardMatch(nameGlob, e.name) && !funclistWildcardMatch(nameGlob, e.rawName)) return false;
	}
	return true;
}

bool functionLess(const FunctionEntry& a, const FunctionEntry& b, int col, bool demangle)
{
	switch (col)
	{
	case FnColAddress: return a.address < b.address;
	case FnColName: return ciCompare(functionDisplayName(a, demangle), functionDisplayName(b, demangle)) < 0;
	case FnColSize: return a.sizeBytes < b.sizeBytes;
	case FnColInstrs: return a.instrCount < b.instrCount;
	case FnColConfidence: return a.confidence.composite() < b.confidence.composite();
	case FnColPatterns: return patternCount(a.patterns) < patternCount(b.patterns);
	case FnColCC: return ciCompare(a.cc, b.cc) < 0;
	case FnColNotes: return ciCompare(functionCellText(a, FnColNotes), functionCellText(b, FnColNotes)) < 0;
	default: return false;
	}
}

// ─── FunctionListProxy ───────────────────────────────────────────────────────

FunctionListProxy::FunctionListProxy(const FunctionList* source): src_(source) {}

void FunctionListProxy::setNameGlob(std::string_view glob)
{
	filter_.nameGlob = std::string(glob);
	dirty_ = true;
}

void FunctionListProxy::setAddressRange(uint64_t low, uint64_t high)
{
	filter_.addrLow = low;
	filter_.addrHigh = high;
	dirty_ = true;
}

void FunctionListProxy::setMinConfidence(float minConf)
{
	filter_.minConfidence = minConf;
	dirty_ = true;
}

void FunctionListProxy::setPatternFilter(bool stl, bool crypto, bool algo, bool design, bool lib)
{
	filter_.stl = stl;
	filter_.crypto = crypto;
	filter_.algo = algo;
	filter_.design = design;
	filter_.lib = lib;
	dirty_ = true;
}

void FunctionListProxy::setClassFilter(std::string_view className)
{
	filter_.classFilter = std::string(className);
	dirty_ = true;
}

void FunctionListProxy::setFilter(FunctionFilter f)
{
	filter_ = std::move(f);
	dirty_ = true;
}

void FunctionListProxy::sortBy(int col, bool ascending)
{
	sortCol_ = col;
	asc_ = ascending;
	dirty_ = true;
}

void FunctionListProxy::setDemangleNames(bool on)
{
	demangle_ = on;
	dirty_ = true;
}

void FunctionListProxy::refresh() const
{
	if (!src_)
	{
		rows_.clear();
		return;
	}
	if (!dirty_ && seenRevision_ == src_->revision()) return;
	dirty_ = false;
	seenRevision_ = src_->revision();
	rows_.clear();
	const auto& fns = src_->functions();
	for (std::size_t i = 0; i < fns.size(); ++i)
		if (filter_.accepts(fns[i])) rows_.push_back(i);
	if (sortCol_ >= 0 && sortCol_ < FnColCount)
	{
		const int col = sortCol_;
		const bool asc = asc_;
		const bool dm = demangle_;
		std::stable_sort(rows_.begin(), rows_.end(), [&](std::size_t x, std::size_t y) {
			return asc ? functionLess(fns[x], fns[y], col, dm) : functionLess(fns[y], fns[x], col, dm);
		});
	}
}

std::size_t FunctionListProxy::rowCount() const
{
	refresh();
	return rows_.size();
}

std::size_t FunctionListProxy::sourceRow(std::size_t row) const
{
	refresh();
	return rows_[row];
}

int FunctionListProxy::rowForSource(std::size_t src) const
{
	refresh();
	for (std::size_t i = 0; i < rows_.size(); ++i)
		if (rows_[i] == src) return static_cast<int>(i);
	return -1;
}

int FunctionListProxy::rowForAddress(uint64_t address) const
{
	if (!src_) return -1;
	const int src = src_->rowForAddress(address);
	return src < 0 ? -1 : rowForSource(static_cast<std::size_t>(src));
}

const FunctionEntry& FunctionListProxy::entry(std::size_t row) const
{
	return src_->entry(sourceRow(row));
}

const std::vector<std::size_t>& FunctionListProxy::rows() const
{
	refresh();
	return rows_;
}

} // namespace retdec::gui::model
