/**
 * @file src/string_detect/string_table.cpp
 * @brief String pointer table detection.
 *
 * ## Algorithm
 *
 * Starting at `vma`, read consecutive pointer-width values.
 * For each value:
 *   - If it is a mapped address → candidate string pointer.
 *   - Attempt typeString() at that address.
 *   - If it produces a valid string → increment consecutive hit count.
 *   - If it produces no string → stop (end of table).
 *
 * Require at least `minEntries` consecutive hits.
 *
 * The key invariant: a genuine string table will have ALL pointers pointing
 * to valid strings.  A single invalid pointer (non-string data) ends the scan.
 *
 * ## Common patterns detected
 *
 *   - Error message tables: static const char* errors[] = { "foo", "bar", … }
 *   - Switch-dispatch string tables: labels for each case
 *   - Printf format string arrays
 *   - Enum-to-name tables
 */

#include "retdec/string_detect/string_detect.h"

#include <optional>
#include <vector>

namespace retdec {
namespace string_detect {

std::optional<StringTable> detectStringTable(const IBinaryView& view,
                                              uint64_t           vma,
                                              std::size_t        maxEntries)
{
    uint32_t ptrW = view.pointerWidth();
    if (ptrW != 4 && ptrW != 8) return std::nullopt;
    if (!view.isMapped(vma)) return std::nullopt;

    std::vector<uint64_t> targets;
    targets.reserve(16);

    uint64_t cursor = vma;
    for (std::size_t i = 0; i < maxEntries; ++i) {
        if (!view.isMapped(cursor)) break;

        uint64_t ptr = view.readPointer(cursor);
        if (ptr == 0) break;                    // null pointer = end of table
        if (!view.isMapped(ptr))  break;        // not a valid address
        if (!view.isDataSection(ptr) && !view.isCodeSection(ptr)) break;

        // Attempt string classification at ptr
        auto sl = typeString(view, ptr, 256);
        if (!sl || sl->charCount < 1) break;    // not a string → end of table

        targets.push_back(ptr);
        cursor += ptrW;
    }

    if (targets.size() < 2) return std::nullopt;  // require at least 2

    StringTable tbl;
    tbl.address  = vma;
    tbl.count    = targets.size();
    tbl.targets  = std::move(targets);
    tbl.ptrWidth = ptrW;
    return tbl;
}

} // namespace string_detect
} // namespace retdec
