/**
 * @file include/retdec/gui/model/tri_pane_sync.h
 * @brief Synchronisation logic of the tri-pane code view (Asm | IR | C):
 *        the line mapping between panes, deriving it from addresses found in
 *        the three texts, proportional scroll sync and back / forward
 *        navigation history.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_TRI_PANE_SYNC_H
#define RETDEC_GUI_MODEL_TRI_PANE_SYNC_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace retdec::gui::model {

/// Which pane of the tri-pane view.
enum class TriPane : uint8_t
{
	Asm,
	IR,
	C,
};

/**
 * A triple (asmLine, irLine, cLine) associates one assembly instruction with
 * its IR statement and the corresponding source line (1-based). Any index may
 * be -1 (no correspondence in that pane).
 */
struct LineMappingEntry
{
	int asmLine = -1;
	int irLine = -1;
	int cLine = -1;
	int line(TriPane p) const
	{
		return p == TriPane::Asm ? asmLine : p == TriPane::IR ? irLine : cLine;
	}
	friend bool operator==(const LineMappingEntry&, const LineMappingEntry&) = default;
};

/// Bidirectional mapping between assembly, IR and source lines.
class LineMapping {
public:
	void addEntry(int asmLine, int irLine, int cLine);
	void clear();
	bool isEmpty() const
	{
		return entries_.empty();
	}
	std::size_t size() const
	{
		return entries_.size();
	}
	const std::vector<LineMappingEntry>& entries() const
	{
		return entries_;
	}

	/// Entry whose asmLine == line, or nullptr (negative lines never match).
	const LineMappingEntry* byAsmLine(int line) const;
	const LineMappingEntry* byIrLine(int line) const;
	const LineMappingEntry* byCLine(int line) const;
	const LineMappingEntry* byLine(TriPane pane, int line) const;

private:
	std::vector<LineMappingEntry> entries_;
	std::map<int, std::size_t> asmIndex_, irIndex_, cIndex_;
};

/**
 * Address carried by one line of a pane, or nullopt:
 *  - Asm: the listing address ("0x401000:" / "401000:" prefix, else the
 *    first 0x token);
 *  - IR: an "; 0x…" comment, or "!insn.addr !N" resolved through the
 *    module's metadata (@p irMetadata: N -> address);
 *  - C: a "// 0x…" comment (retdec emits these per statement) or an
 *    "Address range: 0x… - 0x…" header (start address).
 */
std::optional<uint64_t>
lineAddress(TriPane pane, const std::string& line, const std::map<int, uint64_t>* irMetadata = nullptr);

/// IR metadata nodes of the form `!N = !{i64 <addr>}` (decimal or 0x).
std::map<int, uint64_t> parseIrAddressMetadata(const std::vector<std::string>& irLines);

/**
 * Builds a mapping by matching addresses: every asm line with an address is
 * paired with the first IR line and the first C line carrying the same
 * address (-1 when none). IR / C lines whose address has no asm line still
 * get an entry so that clicking them highlights the other pane.
 */
LineMapping buildLineMappingByAddress(
	const std::vector<std::string>& asmLines,
	const std::vector<std::string>& irLines,
	const std::vector<std::string>& cLines);

/// Maps one pane's scroll value to the same relative position in another.
int proportionalScroll(int value, int srcMax, int dstMax);

/// Back / forward history of (address, name) pairs, capped (oldest dropped).
class NavHistory {
public:
	struct Entry
	{
		uint64_t address = 0;
		std::string name;
	};
	explicit NavHistory(std::size_t cap = 32): cap_(cap) {}

	/// Records a new location: drops the forward part, appends, trims to the cap.
	void push(uint64_t address, std::string name);
	/// Moves back / forward; the new current entry, or nullopt at the end.
	std::optional<Entry> back();
	std::optional<Entry> forward();
	bool canGoBack() const
	{
		return index_ > 0;
	}
	bool canGoForward() const
	{
		return index_ + 1 < static_cast<int>(entries_.size());
	}
	void clear();
	std::size_t size() const
	{
		return entries_.size();
	}
	int index() const
	{
		return index_;
	}
	const std::vector<Entry>& entries() const
	{
		return entries_;
	}

private:
	std::vector<Entry> entries_;
	int index_ = -1;
	std::size_t cap_;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_TRI_PANE_SYNC_H
