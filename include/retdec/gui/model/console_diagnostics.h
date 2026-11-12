/**
 * @file include/retdec/gui/model/console_diagnostics.h
 * @brief The Problems panel's model: a bounded list of model::Diagnostic with
 *        severity / text filtering, TSV export, and turning decompiler log
 *        lines into diagnostics.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_CONSOLE_DIAGNOSTICS_H
#define RETDEC_GUI_MODEL_CONSOLE_DIAGNOSTICS_H

#include "retdec/gui/model/types.h"

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// Severity filter of the Problems panel ("Show:" combo order).
enum class SeverityFilter : uint8_t
{
	All,
	Info,
	Warnings,
	Errors,
};

/// "Info" / "Warning" / "Error".
std::string_view severityLabel(Severity s);
/// Glyph shown before the label: ℹ ⚠ ✖.
std::string_view severityIcon(Severity s);

/// Bounded FIFO of diagnostics (oldest evicted at the cap).
class DiagnosticsStore {
public:
	explicit DiagnosticsStore(std::size_t maxEntries = 10'000);

	void add(Diagnostic d);
	void clear();
	std::size_t size() const { return entries_.size(); }
	const Diagnostic& at(std::size_t i) const { return entries_[i]; }

	/// Cap on retained entries (minimum 100). Shrinking evicts the oldest.
	void setMaxEntries(std::size_t n);
	std::size_t maxEntries() const { return max_; }

	/**
	 * Indices of the rows shown: while @p text is non-empty it replaces the
	 * severity filter (case-insensitive substring of the message); otherwise
	 * rows matching @p filter.
	 */
	std::vector<std::size_t> visibleRows(SeverityFilter filter, std::string_view text) const;
	/// Whether entry @p i passes the same filter as visibleRows.
	bool matches(std::size_t i, SeverityFilter filter, std::string_view text) const;

	/// "Severity\tSource\tMessage" lines for @p rows, '\n'-joined.
	std::string tsv(const std::vector<std::size_t>& rows) const;

private:
	std::deque<Diagnostic> entries_;
	std::size_t max_;
};

/**
 * Diagnostic for a warning / error line of tool output (console
 * classification), nullopt for other lines. The first 0x… address on the
 * line becomes the diagnostic's address.
 */
std::optional<Diagnostic> diagnosticFromLogLine(std::string_view line, std::string_view source = "decompiler log");

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_CONSOLE_DIAGNOSTICS_H
