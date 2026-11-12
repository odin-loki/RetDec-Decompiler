/**
 * @file include/retdec/gui/model/graph_export.h
 * @brief Text exports of graph scenes: SVG (what the view shows) and DOT helpers.
 *
 * PNG export rasterises the same scene in the view layer (GraphView::renderImage).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_GRAPH_EXPORT_H
#define RETDEC_GUI_MODEL_GRAPH_EXPORT_H

#include "retdec/gui/model/graph_scene.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace retdec::gui::model {

struct SvgOptions
{
	std::string title = "RetDec graph export";
	bool includeHidden = false; ///< export hidden (filtered) items too
	bool background = true;
};

/// A standalone SVG document of the scene's visible items: regions, edges (with dash patterns and arrowheads), nodes
/// (rounded boxes, header / side stripes, text). Uses the scene bounds as the view box.
std::string sceneToSvg(const GraphScene& scene, const SvgOptions& options = {});

/// XML text escaping (&, <, >, ", ').
std::string graphXmlEscape(std::string_view s);
/// DOT double-quoted string escaping (backslash, quote, newline -> \n).
std::string graphDotEscape(std::string_view s);

/// Writes `text` to `path` (UTF-8, binary). Error text on failure.
std::expected<void, std::string> writeGraphExportFile(const std::filesystem::path& path, std::string_view text);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_GRAPH_EXPORT_H
