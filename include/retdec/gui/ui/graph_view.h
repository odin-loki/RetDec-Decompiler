/**
 * @file include/retdec/gui/ui/graph_view.h
 * @brief Pan / zoom canvas that paints a model::GraphScene with FuseUI's draw list (CFG and call-graph panels).
 *
 * One custom widget class ("retdecgraphview") whose paint function draws the scene through the view transform:
 * regions, edges (flattened beziers, dash patterns, outlined arrowheads), nodes (shadow, body, header / side stripe,
 * border, text), hover / selection / search highlights, the minimap overlay and the hover tooltip. Only items
 * intersecting the viewport are emitted, and text below a readable pixel size is skipped, so graphs of a few
 * thousand nodes stay cheap to repaint.
 *
 * Input (as the Qt CFGView / CallGraphView):
 *   Ctrl+wheel        zoom about the pointer (x1.18 per notch)     wheel / Shift+wheel   scroll
 *   middle-drag       pan; left-drag on empty space pans too        Ctrl+F fit, Ctrl+0 100 %
 *   left click node   select + onNodeClicked; double click / Enter onNodeActivated
 *   right click       onNodeContextMenu / onBackgroundContextMenu  arrows pan, Escape clears selection / highlights
 *   minimap           click or drag to centre the view there
 *
 * Lifetime: destroy the view (or its panel) before its UiContext.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_GRAPH_VIEW_H
#define RETDEC_GUI_UI_GRAPH_VIEW_H

#include "retdec/gui/model/graph_scene.h"

#include <fuse/ui/ui.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace retdec::gui::ui {

class GraphView
{
public:
	GraphView(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, std::string_view id = {});
	~GraphView();
	GraphView(const GraphView&) = delete;
	GraphView& operator=(const GraphView&) = delete;

	fuse::ui::NodeHandle node() const { return m_node; }

	// ── Scene ────────────────────────────────────────────────────────────────
	/// Replaces the scene. `fit` fits it to the viewport (deferred until the view has a size).
	void setScene(model::GraphScene scene, bool fit = true);
	const model::GraphScene& scene() const { return m_scene; }
	/// Edit visibility / styles in place, then call sceneChanged() (finalize() first if geometry moved).
	model::GraphScene& mutableScene() { return m_scene; }
	void sceneChanged();
	void clear();

	// ── View ─────────────────────────────────────────────────────────────────
	void fitGraph();
	/// Zoom 1 (Qt resetTransform), keeping the view centre.
	void resetZoom();
	/// Zoom about the viewport centre.
	void zoomBy(float factor);
	/// Zoom about a point in context coordinates.
	void zoomAt(float factor, fuse::ui::Vec2 contextPoint);
	void panBy(float dx, float dy);
	/// Centres the view on a node (no zoom change). False when the node is not in the scene.
	bool centerOn(uint64_t nodeId);
	float zoom() const { return m_t.zoom; }
	const model::ViewTransform& transform() const { return m_t; }
	void setTransform(const model::ViewTransform& t);
	/// The canvas area in context coordinates (valid after an update).
	fuse::ui::Rect viewportRect() const;

	// ── Selection, hover, search highlight ───────────────────────────────────
	void setSelectedNode(std::optional<uint64_t> id);
	std::optional<uint64_t> selectedNode() const { return m_selected; }
	std::optional<uint64_t> hoveredNode() const { return m_hovered; }
	void setHighlighted(const std::vector<uint64_t>& ids);
	const std::unordered_set<uint64_t>& highlighted() const { return m_highlight; }

	// ── Minimap and tooltip ──────────────────────────────────────────────────
	void setMinimapVisible(bool on);
	bool minimapVisible() const { return m_minimap; }
	/// Minimap frame in context coordinates (empty when hidden or no scene).
	fuse::ui::Rect minimapRect() const;
	void setTooltipDelay(float seconds) { m_tooltipDelay = seconds; }
	bool tooltipVisible() const { return m_tooltipShown && m_hovered.has_value(); }
	std::string tooltipText() const;
	/// Text shown centred when the scene is empty (empty = nothing).
	void setEmptyText(std::string text);

	// ── Coordinates (tests, panels) ──────────────────────────────────────────
	fuse::ui::Vec2 sceneToContext(model::GPoint p) const;
	model::GPoint contextToScene(fuse::ui::Vec2 p) const;
	/// Centre of a visible node in context coordinates.
	std::optional<fuse::ui::Vec2> nodeCenter(uint64_t id) const;

	// ── Export ───────────────────────────────────────────────────────────────
	/// Rasterises the whole scene at `scale` (Qt exported at 2x), opaque background. Long sides are capped at 16384 px.
	fuse::ui::Image renderImage(float scale = 2.f);
	std::expected<void, std::string> exportPng(const std::filesystem::path& path, float scale = 2.f);

	// ── Context menus ────────────────────────────────────────────────────────
	/// Opens a popup menu (items '\n'-separated, "-" = separator) at a context point; `onPick(index)` runs when an
	/// item is chosen.
	fuse::ui::NodeHandle openMenu(std::string_view items, fuse::ui::Vec2 at, std::function<void(int)> onPick);

	// ── Events ───────────────────────────────────────────────────────────────
	std::function<void(uint64_t id)> onNodeClicked;
	std::function<void(uint64_t id)> onNodeActivated; ///< double click or Enter on the selection
	std::function<void(uint64_t id, fuse::ui::Vec2 at)> onNodeContextMenu;
	std::function<void(fuse::ui::Vec2 at)> onBackgroundContextMenu;
	std::function<void(std::optional<uint64_t> id)> onHoverChanged;
	std::function<void()> onViewChanged;

	void setDoubleClickInterval(double seconds) { m_doubleClick = seconds; }

	// Widget-class glue (internal).
	struct Impl;

private:
	friend struct Impl;
	void paint(const fuse::ui::PaintContext& pc, fuse::ui::DrawList& dl);
	bool handle(const fuse::ui::Event& e);
	void setHovered(std::optional<uint64_t> id);
	void ensureFit();
	void viewChanged();
	fuse::ui::Rect contentRect() const;
	model::GRect minimapScene(const fuse::ui::Rect& mm, float& scale, fuse::ui::Vec2& offset) const;
	void centreFromMinimap(fuse::ui::Vec2 p);

	fuse::ui::UiContext& m_ui;
	fuse::ui::NodeHandle m_node;
	model::GraphScene m_scene;
	model::ViewTransform m_t;
	bool m_pendingFit = false;
	bool m_minimap = true;
	std::optional<uint64_t> m_selected, m_hovered;
	std::unordered_set<uint64_t> m_highlight;
	std::string m_emptyText;

	// interaction
	enum class Drag : uint8_t
	{
		None,
		Pan,
		Minimap
	} m_drag = Drag::None;
	fuse::ui::Vec2 m_last;
	std::optional<uint64_t> m_lastClickNode;
	std::chrono::steady_clock::time_point m_lastClickTime;
	double m_doubleClick = 0.45;

	// tooltip
	fuse::ui::TimerHandle m_tooltipTimer;
	float m_tooltipDelay = 0.5f;
	bool m_tooltipShown = false;

	// menus
	std::function<void(int)> m_menuPick;
	fuse::ui::ConnectionHandle m_menuConn;

	uint32_t m_slot = 0;
};

/// Colour conversion model::Rgba -> FuseUI.
fuse::ui::Color toColor(model::Rgba c, float opacity = 1.f);

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_GRAPH_VIEW_H
