/**
 * @file src/gui/model/console_diagnostics.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/console_diagnostics.h"

#include "retdec/gui/model/code_address.h"
#include "retdec/gui/model/console_log.h"

#include <algorithm>
#include <cctype>

namespace retdec::gui::model {

std::string_view severityLabel(Severity s)
{
	switch (s)
	{
	case Severity::Info: return "Info";
	case Severity::Warning: return "Warning";
	case Severity::Error: return "Error";
	}
	return {};
}

std::string_view severityIcon(Severity s)
{
	switch (s)
	{
	case Severity::Info: return "\xE2\x84\xB9";    // ℹ
	case Severity::Warning: return "\xE2\x9A\xA0"; // ⚠
	case Severity::Error: return "\xE2\x9C\x96";   // ✖
	}
	return {};
}

DiagnosticsStore::DiagnosticsStore(std::size_t maxEntries): max_(std::max<std::size_t>(100, maxEntries)) {}

void DiagnosticsStore::add(Diagnostic d)
{
	while (entries_.size() >= max_)
		entries_.pop_front();
	entries_.push_back(std::move(d));
}

void DiagnosticsStore::clear()
{
	entries_.clear();
}

void DiagnosticsStore::setMaxEntries(std::size_t n)
{
	max_ = std::max<std::size_t>(100, n);
	while (entries_.size() > max_)
		entries_.pop_front();
}

namespace {
std::string lowerCopy(std::string_view s)
{
	std::string r(s);
	for (char& c: r)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return r;
}

bool passes(const Diagnostic& d, SeverityFilter filter, const std::string& lowerNeedle)
{
	if (!lowerNeedle.empty()) return lowerCopy(d.message).find(lowerNeedle) != std::string::npos;
	switch (filter)
	{
	case SeverityFilter::Info: return d.severity == Severity::Info;
	case SeverityFilter::Warnings: return d.severity == Severity::Warning;
	case SeverityFilter::Errors: return d.severity == Severity::Error;
	default: return true;
	}
}
} // namespace

std::vector<std::size_t> DiagnosticsStore::visibleRows(SeverityFilter filter, std::string_view text) const
{
	std::vector<std::size_t> out;
	out.reserve(entries_.size());
	const std::string needle = lowerCopy(text);
	for (std::size_t i = 0; i < entries_.size(); ++i)
		if (passes(entries_[i], filter, needle)) out.push_back(i);
	return out;
}

bool DiagnosticsStore::matches(std::size_t i, SeverityFilter filter, std::string_view text) const
{
	return i < entries_.size() && passes(entries_[i], filter, lowerCopy(text));
}

std::string DiagnosticsStore::tsv(const std::vector<std::size_t>& rows) const
{
	std::string out;
	for (std::size_t k = 0; k < rows.size(); ++k)
	{
		if (rows[k] >= entries_.size()) continue;
		const Diagnostic& d = entries_[rows[k]];
		if (!out.empty()) out += '\n';
		out += severityLabel(d.severity);
		out += '\t';
		out += d.source;
		out += '\t';
		out += d.message;
	}
	return out;
}

std::optional<Diagnostic> diagnosticFromLogLine(std::string_view line, std::string_view source)
{
	const ConsoleLineKind k = classifyConsoleLine(line);
	if (k != ConsoleLineKind::Warning && k != ConsoleLineKind::Error) return std::nullopt;
	Diagnostic d;
	d.severity = k == ConsoleLineKind::Error ? Severity::Error : Severity::Warning;
	d.message = std::string(line);
	d.source = std::string(source);
	d.address = parseFirstAddress(line).value_or(0);
	return d;
}

} // namespace retdec::gui::model
