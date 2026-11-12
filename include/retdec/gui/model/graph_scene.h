/**
 * @file include/retdec/gui/model/graph_scene.h
 * @brief Drawable description of a laid-out graph, shared by the graph views and the exporters.
 *
 * The CFG and call-graph layouts produce a GraphScene: rounded node boxes with text lines, cubic-bezier edges with
 * arrowheads and dash patterns, and translucent regions (loop bodies, module clusters). The FuseUI GraphView paints
 * it, the SVG exporter writes it, and tests inspect it, so everything about the picture that is not pixels lives here.
 * Plain C++23, no UI types; scene units are logical pixels at zoom 1.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_GRAPH_SCENE_H
#define RETDEC_GUI_MODEL_GRAPH_SCENE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace retdec::gui::model {

// ── Geometry ─────────────────────────────────────────────────────────────────

struct GPoint
{
	float x = 0.f;
	float y = 0.f;
	friend bool operator==(const GPoint&, const GPoint&) = default;
};

struct GRect
{
	float x = 0.f;
	float y = 0.f;
	float w = 0.f;
	float h = 0.f;

	float right() const { return x + w; }
	float bottom() const { return y + h; }
	GPoint center() const { return {x + w * 0.5f, y + h * 0.5f}; }
	bool empty() const { return w <= 0.f || h <= 0.f; }
	bool contains(GPoint p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
	bool intersects(const GRect& o) const
	{
		return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
	}
	GRect adjusted(float l, float t, float r, float b) const { return {x + l, y + t, w - l + r, h - t + b}; }
	GRect united(const GRect& o) const;
	friend bool operator==(const GRect&, const GRect&) = default;
};

// ── Colour ───────────────────────────────────────────────────────────────────

/// 0xRRGGBBAA, straight alpha.
using Rgba = uint32_t;

constexpr Rgba rgbaHex(uint32_t rgb, uint8_t alpha = 0xFF)
{
	return (rgb << 8) | alpha;
}
constexpr uint8_t rgbaR(Rgba c) { return uint8_t(c >> 24); }
constexpr uint8_t rgbaG(Rgba c) { return uint8_t(c >> 16); }
constexpr uint8_t rgbaB(Rgba c) { return uint8_t(c >> 8); }
constexpr uint8_t rgbaA(Rgba c) { return uint8_t(c); }
constexpr Rgba rgbaWithAlpha(Rgba c, uint8_t a) { return (c & 0xFFFFFF00u) | a; }
/// Multiplies the alpha channel by f (0..1).
Rgba rgbaFade(Rgba c, float f);
/// "#rrggbb" (alpha dropped).
std::string rgbaCss(Rgba c);

/// Catppuccin Mocha, the palette the Qt graph panels painted with.
namespace catppuccin {
inline constexpr Rgba base = rgbaHex(0x1e1e2e);
inline constexpr Rgba mantle = rgbaHex(0x181825);
inline constexpr Rgba crust = rgbaHex(0x11111b);
inline constexpr Rgba surface0 = rgbaHex(0x313244);
inline constexpr Rgba surface1 = rgbaHex(0x45475a);
inline constexpr Rgba surface2 = rgbaHex(0x585b70);
inline constexpr Rgba overlay0 = rgbaHex(0x6c7086);
inline constexpr Rgba overlay2 = rgbaHex(0x9a9ebc);
inline constexpr Rgba text = rgbaHex(0xcdd6f4);
inline constexpr Rgba subtext0 = rgbaHex(0xa6adc8);
inline constexpr Rgba subtext1 = rgbaHex(0xbac2de);
inline constexpr Rgba red = rgbaHex(0xf38ba8);
inline constexpr Rgba green = rgbaHex(0xa6e3a1);
inline constexpr Rgba yellow = rgbaHex(0xf9e2af);
inline constexpr Rgba blue = rgbaHex(0x89b4fa);
inline constexpr Rgba mauve = rgbaHex(0xcba6f7);
inline constexpr Rgba peach = rgbaHex(0xfab387);
inline constexpr Rgba teal = rgbaHex(0x94e2d5);
inline constexpr Rgba pink = rgbaHex(0xf5c2e7);
inline constexpr Rgba flamingo = rgbaHex(0xf2cdcd);
inline constexpr Rgba rosewater = rgbaHex(0xf5e0dc);
} // namespace catppuccin

// ── Scene items ──────────────────────────────────────────────────────────────

enum class SceneAlign : uint8_t
{
	Left,
	Center,
	Right
};

/// One line of text inside a node, positioned in scene coordinates (vertically centred in `box`).
struct SceneText
{
	std::string text;
	GRect box;
	float size = 11.f; ///< pixel size at zoom 1
	Rgba color = catppuccin::text;
	SceneAlign align = SceneAlign::Left;
	bool bold = false;
	bool mono = false; ///< monospace (the view paints it with the "monospace" face; SVG font-family)
};

struct SceneNode
{
	uint64_t id = 0;
	int kind = 0; ///< owner-defined (block, chain, function, SCC, ...)
	GRect rect;
	float radius = 6.f;
	Rgba fill = catppuccin::surface0;
	Rgba border = catppuccin::surface1;
	float borderWidth = 1.5f;
	bool dashedBorder = false;
	Rgba hoverFill = 0;   ///< 0 = keep `fill` while hovered
	Rgba hoverBorder = 0; ///< 0 = keep `border` while hovered
	Rgba headerFill = 0;  ///< top stripe (0 = none), `headerHeight` tall, top corners rounded
	float headerHeight = 0.f;
	Rgba stripeFill = 0; ///< left stripe (0 = none), `stripeWidth` wide
	float stripeWidth = 0.f;
	bool shadow = true;
	float opacity = 1.f; ///< dimming (focus mode)
	bool visible = true;
	std::vector<SceneText> texts;
	std::string tooltip;    ///< hover text, lines separated by '\n'
	std::string searchText; ///< what search matches (name, address, ...)
};

struct SceneEdge
{
	uint64_t from = 0;
	uint64_t to = 0;
	int kind = 0;
	/// Cubic path: points[0] is the start, then (control1, control2, end) triples.
	std::vector<GPoint> points;
	Rgba color = catppuccin::subtext0;
	float width = 1.6f;
	bool dashed = false;
	float dash = 4.f; ///< dash / gap lengths in pen widths (Qt dash pattern units)
	float gap = 3.f;
	bool arrow = true;
	float arrowLength = 10.f;
	float arrowHalfWidth = 5.f;
	bool visible = true;
	float opacity = 1.f;

	// Filled in by GraphScene::finalize().
	std::vector<GPoint> polyline; ///< flattened path
	GRect bounds;
};

/// Translucent backdrop rectangle (loop body, module cluster), drawn under edges and nodes.
struct SceneRegion
{
	GRect rect;
	Rgba fill = 0;
	Rgba border = 0;
	float borderWidth = 2.f;
	float radius = 0.f;
	std::string label;
	Rgba labelColor = 0;
	float labelSize = 11.f;
	bool visible = true;
	int group = -1; ///< loop id / module id
};

struct GraphScene
{
	std::vector<SceneRegion> regions;
	std::vector<SceneEdge> edges;
	std::vector<SceneNode> nodes;
	Rgba background = catppuccin::base;
	GRect bounds; ///< everything, plus the margin passed to finalize()

	bool empty() const { return nodes.empty(); }
	void clear();
	/// Flattens edge paths, computes edge bounds, the scene bounds (+margin) and the id index. Call after building
	/// or moving items; visibility changes need no finalize.
	void finalize(float margin = 40.f);
	/// Index of the node with `id`, or -1.
	int findNode(uint64_t id) const;
	const SceneNode* node(uint64_t id) const;
	SceneNode* node(uint64_t id);
	/// Topmost visible node under `p`, or -1.
	int nodeAt(GPoint p) const;
	/// Bounds of the visible items (empty rect when nothing is visible).
	GRect visibleBounds() const;
	int visibleNodeCount() const;

private:
	std::unordered_map<uint64_t, int> m_index;
};

// ── Path helpers ─────────────────────────────────────────────────────────────

/// Point on a cubic bezier at t.
GPoint cubicPoint(GPoint p0, GPoint c1, GPoint c2, GPoint p1, float t);
/// Flattens a cubic path (start + triples) into a polyline, `segments` segments per curve.
std::vector<GPoint> flattenCubicPath(const std::vector<GPoint>& points, int segments = 16);
/// Splits a polyline into dash segments (dash / gap in the same units as the points).
std::vector<std::pair<GPoint, GPoint>> dashPolyline(const std::vector<GPoint>& polyline, float dash, float gap);
/// Arrowhead triangle (tip, left, right) for an edge arriving at `tip` from direction `from`.
struct ArrowHead
{
	GPoint tip, left, right;
};
ArrowHead arrowHead(GPoint tip, GPoint from, float length, float halfWidth);

// ── View transform (pan / zoom / fit) ────────────────────────────────────────

/// view = (scene - origin) * zoom, where `origin` is the scene point shown at the view's top-left.
struct ViewTransform
{
	float zoom = 1.f;
	GPoint origin;

	GPoint toView(GPoint s) const { return {(s.x - origin.x) * zoom, (s.y - origin.y) * zoom}; }
	GPoint toScene(GPoint v) const { return {v.x / zoom + origin.x, v.y / zoom + origin.y}; }
	GRect toViewRect(const GRect& r) const { return {(r.x - origin.x) * zoom, (r.y - origin.y) * zoom, r.w * zoom, r.h * zoom}; }
	/// The scene rectangle visible in a viewport of size w x h.
	GRect visibleScene(float w, float h) const { return {origin.x, origin.y, w / zoom, h / zoom}; }

	static constexpr float kMinZoom = 0.02f;
	static constexpr float kMaxZoom = 8.f;
	/// Fits `content` (grown by `pad`) into a viewport, centred, keeping the aspect ratio.
	static ViewTransform fit(const GRect& content, float viewW, float viewH, float pad = 20.f);
	/// Zoom by `factor`, keeping the scene point under view point `anchor` fixed.
	void zoomAt(float factor, GPoint anchor);
	/// Pans so scene point `p` is at the centre of a w x h viewport.
	void centerOn(GPoint p, float w, float h);
	void panByView(float dx, float dy) { origin.x -= dx / zoom; origin.y -= dy / zoom; }
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_GRAPH_SCENE_H
