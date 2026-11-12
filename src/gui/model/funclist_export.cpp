/**
 * @file src/gui/model/funclist_export.cpp
 * @brief Function list CSV / JSON export.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/funclist_export.h"

#include <forge/os/os.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>

namespace retdec::gui::model {

namespace {

void escapeJson(std::string& o, const std::string& s)
{
	o += '"';
	for (unsigned char c: s)
	{
		switch (c)
		{
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\b': o += "\\b"; break;
		case '\f': o += "\\f"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20)
				o += std::format("\\u{:04x}", static_cast<unsigned>(c));
			else
				o += static_cast<char>(c);
		}
	}
	o += '"';
}

void numberJson(std::string& o, double d)
{
	if (!std::isfinite(d))
	{
		o += "null";
		return;
	}
	if (d == std::trunc(d) && std::fabs(d) < 1e15)
	{
		o += std::to_string(static_cast<int64_t>(d));
		return;
	}
	char buf[64];
	const auto r = std::to_chars(buf, buf + sizeof buf, d);
	o.append(buf, r.ptr);
}

void writeValue(std::string& o, const forge::json::value& v, int indent);

void writeIndent(std::string& o, int indent)
{
	o.append(static_cast<std::size_t>(indent) * 4u, ' ');
}

void writeArray(std::string& o, const forge::json::array& a, int indent)
{
	o += "[\n";
	for (std::size_t i = 0; i < a.size(); ++i)
	{
		writeIndent(o, indent + 1);
		writeValue(o, a[i], indent + 1);
		o += i + 1 < a.size() ? ",\n" : "\n";
	}
	writeIndent(o, indent);
	o += ']';
}

void writeObject(std::string& o, const forge::json::object& obj, int indent)
{
	std::vector<const std::pair<std::string, forge::json::value>*> members;
	members.reserve(obj.size());
	for (const auto& m: obj)
		members.push_back(&m);
	std::stable_sort(members.begin(), members.end(), [](auto* a, auto* b) { return a->first < b->first; });
	o += "{\n";
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		writeIndent(o, indent + 1);
		escapeJson(o, members[i]->first);
		o += ": ";
		writeValue(o, members[i]->second, indent + 1);
		o += i + 1 < members.size() ? ",\n" : "\n";
	}
	writeIndent(o, indent);
	o += '}';
}

void writeValue(std::string& o, const forge::json::value& v, int indent)
{
	if (v.is_null())
		o += "null";
	else if (v.is_bool())
		o += v.b() ? "true" : "false";
	else if (v.is_num())
		numberJson(o, v.num());
	else if (v.is_str())
		escapeJson(o, v.str());
	else if (v.is_arr())
		writeArray(o, v.arr(), indent);
	else
		writeObject(o, v.obj(), indent);
}

std::string joinTags(const std::vector<std::string>& tags, std::string_view sep)
{
	std::string out;
	for (std::size_t i = 0; i < tags.size(); ++i)
	{
		if (i) out += sep;
		out += tags[i];
	}
	return out;
}

} // namespace

std::string funclistIndentedJson(const forge::json::value& v)
{
	std::string o;
	writeValue(o, v, 0);
	o += '\n';
	return o;
}

std::string functionListCsv(std::span<const FunctionEntry* const> rows)
{
	std::string out = "Address,Name,SizeBytes,Instrs,Confidence,CC,STL,Crypto,Algo,Design,Lib,Notes,Tags\n";
	for (const FunctionEntry* p: rows)
	{
		const FunctionEntry& e = *p;
		out += std::format(
			"0x{:x},{},{},{},{:.3f},{},{},{},{},{},{},{},{}\n",
			e.address,
			e.name,
			e.sizeBytes,
			e.instrCount,
			static_cast<double>(e.confidence.composite()),
			e.cc,
			e.patterns.stl ? 1 : 0,
			e.patterns.crypto ? 1 : 0,
			e.patterns.algo ? 1 : 0,
			e.patterns.design ? 1 : 0,
			e.patterns.library ? 1 : 0,
			e.notes,
			joinTags(e.tags, ";"));
	}
	return out;
}

forge::json::value functionListJsonValue(std::span<const FunctionEntry* const> rows)
{
	using forge::json::array;
	using forge::json::object;
	array arr;
	for (const FunctionEntry* p: rows)
	{
		const FunctionEntry& e = *p;
		object conf{
			{"typeInference", static_cast<double>(e.confidence.typeInference)},
			{"structure", static_cast<double>(e.confidence.structure)},
			{"variable", static_cast<double>(e.confidence.variable)},
			{"algorithm", static_cast<double>(e.confidence.algorithm)},
		};
		object pats{
			{"stl", e.patterns.stl},
			{"crypto", e.patterns.crypto},
			{"algo", e.patterns.algo},
			{"design", e.patterns.design},
			{"library", e.patterns.library},
		};
		array tags;
		for (const auto& t: e.tags)
			tags.emplace_back(t);
		object obj{
			{"address", std::format("0x{:x}", e.address)},
			{"name", e.name},
			{"rawName", e.rawName},
			{"signature", e.signature},
			{"sizeBytes", e.sizeBytes},
			{"instrCount", e.instrCount},
			{"confidence", static_cast<double>(e.confidence.composite())},
			{"cc", e.cc},
			{"notes", e.notes},
			{"confidenceDetail", std::move(conf)},
			{"patterns", std::move(pats)},
			{"tags", std::move(tags)},
		};
		arr.emplace_back(std::move(obj));
	}
	return arr;
}

std::string functionListJson(std::span<const FunctionEntry* const> rows)
{
	return funclistIndentedJson(functionListJsonValue(rows));
}

std::expected<void, std::string> funclistWriteTextFile(const std::string& path, std::string_view text)
{
	auto st = forge::os::write_file(path, text);
	if (!st) return std::unexpected("Cannot open " + path + ": " + st.err.what);
	return {};
}

} // namespace retdec::gui::model
