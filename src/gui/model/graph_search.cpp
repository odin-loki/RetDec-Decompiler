/**
 * @file src/gui/model/graph_search.cpp
 * @brief Name filters and node search for the graph views.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/graph_search.h"

#include <algorithm>
#include <format>

namespace retdec::gui::model {

namespace {

char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

/// Matches one character class "[...]" starting at pattern[p] == '['. Returns the index after ']' or npos when the
/// class is unterminated (then '[' is literal).
size_t matchClass(std::string_view pat, size_t p, char c, bool& matched)
{
	size_t i = p + 1;
	bool negate = false;
	if (i < pat.size() && (pat[i] == '!' || pat[i] == '^'))
	{
		negate = true;
		++i;
	}
	bool any = false;
	bool first = true;
	for (; i < pat.size(); ++i)
	{
		if (pat[i] == ']' && !first) break;
		first = false;
		if (i + 2 < pat.size() && pat[i + 1] == '-' && pat[i + 2] != ']')
		{
			if (lower(c) >= lower(pat[i]) && lower(c) <= lower(pat[i + 2])) any = true;
			i += 2;
		}
		else if (lower(pat[i]) == lower(c))
			any = true;
	}
	if (i >= pat.size()) return std::string_view::npos;
	matched = any != negate;
	return i + 1;
}

} // namespace

bool graphGlobMatch(std::string_view pat, std::string_view s)
{
	size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
	while (i < s.size())
	{
		if (p < pat.size() && pat[p] == '*')
		{
			star = p++;
			mark = i;
			continue;
		}
		if (p < pat.size() && pat[p] == '[')
		{
			bool ok = false;
			const size_t next = matchClass(pat, p, s[i], ok);
			if (next != std::string_view::npos)
			{
				if (ok)
				{
					p = next;
					++i;
					continue;
				}
			}
			else if (lower(s[i]) == '[')
			{
				++p;
				++i;
				continue;
			}
		}
		else if (p < pat.size() && (pat[p] == '?' || lower(pat[p]) == lower(s[i])))
		{
			++p;
			++i;
			continue;
		}
		if (star == std::string_view::npos) return false;
		p = star + 1;
		i = ++mark;
	}
	while (p < pat.size() && pat[p] == '*')
		++p;
	return p == pat.size();
}

bool graphContainsNoCase(std::string_view hay, std::string_view needle)
{
	if (needle.empty()) return true;
	if (needle.size() > hay.size()) return false;
	return std::search(
			   hay.begin(),
			   hay.end(),
			   needle.begin(),
			   needle.end(),
			   [](char a, char b) { return lower(a) == lower(b); })
		!= hay.end();
}

bool graphNameFilterMatches(std::string_view filter, std::string_view name)
{
	if (filter.empty()) return true;
	if (filter.find_first_of("*?[") != std::string_view::npos) return graphGlobMatch(filter, name);
	return graphContainsNoCase(name, filter);
}

std::vector<uint64_t> graphSearchScene(const GraphScene& scene, std::string_view query)
{
	std::vector<uint64_t> out;
	if (query.empty()) return out;
	const bool hex = query.size() > 2 && query[0] == '0' && lower(query[1]) == 'x';
	for (const SceneNode& n: scene.nodes)
	{
		if (!n.visible) continue;
		std::string_view text = n.searchText;
		if (text.empty() && !n.texts.empty()) text = n.texts.front().text;
		bool hit = graphNameFilterMatches(query, text);
		if (!hit && hex)
		{
			const std::string idHex = std::format("0x{:x}", n.id);
			hit = idHex.size() >= query.size()
			   && std::equal(query.begin(), query.end(), idHex.begin(), [](char a, char b) {
					  return lower(a) == lower(b);
				  });
		}
		if (hit) out.push_back(n.id);
	}
	return out;
}

} // namespace retdec::gui::model
