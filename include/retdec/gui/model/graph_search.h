/**
 * @file include/retdec/gui/model/graph_search.h
 * @brief Name filters and node search for the graph views.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_GRAPH_SEARCH_H
#define RETDEC_GUI_MODEL_GRAPH_SEARCH_H

#include "retdec/gui/model/graph_scene.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// Case-insensitive wildcard match of the whole string: '*' any run, '?' one character, "[abc]" a set
/// (QRegularExpression::wildcardToRegularExpression semantics, which the Qt call-graph filter used).
bool globMatch(std::string_view pattern, std::string_view text);

/// The call-graph name filter: empty matches everything; a pattern with a wildcard ('*', '?', '[') is a whole-string
/// glob as in Qt; plain text is a case-insensitive substring (Qt required "*main*" to find "main_loop").
bool nameFilterMatches(std::string_view filter, std::string_view name);

/// Case-insensitive substring.
bool containsNoCase(std::string_view haystack, std::string_view needle);

/// Ids of the visible scene nodes whose searchText (or first text line) matches `query`, in scene order.
/// Matching is nameFilterMatches(query, ...); a query starting with "0x" also matches node ids numerically
/// by prefix of their hex form.
std::vector<uint64_t> searchScene(const GraphScene& scene, std::string_view query);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_GRAPH_SEARCH_H
