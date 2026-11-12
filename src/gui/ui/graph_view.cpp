/**
 * @file src/gui/ui/graph_view.cpp
 * @brief GraphView: draw-list painting, pan / zoom / fit, minimap, tooltip, hit testing, export.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/graph_view.h"

#include "retdec/gui/ui/empty_state.h"

#include <fuse/app/app.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>
#include <cmath>

namespace retdec::gui::ui {

using fuse::ui::Color;
using fuse::ui::DrawList;
using fuse::ui::Event;
using fuse::ui::EventType;
using fuse::ui::Key;
using fuse::ui::MouseButton;
using fuse::ui::NodeHandle;
using fuse::ui::Rect;
using fuse::ui::UiContext;
using fuse::ui::Vec2;
using model::GPoint;
using model::GRect;

Color graphColor(model::Rgba c, float opacity)
{
	return {float(model::rgbaR(c)) / 255.f, float(model::rgbaG(c)) / 255.f, float(model::rgbaB(c)) / 255.f,
		float(model::rgbaA(c)) / 255.f * opacity};
}

namespace {

constexpr std::string_view kClassName = "retdecgraphview";
constexpr float kMiniW = 160.f, kMiniH = 120.f, kMiniMargin = 8.f;
constexpr float kMinTextPx = 4.5f;

/// Live views by slot (user data = slot + 1). The UI is single-threaded.
std::vector<GraphView*>& views()
{
	static std::vector<GraphView*> v;
	return v;
}

struct PaintState
{
	std::optional<uint64_t> hovered, selected;
	const std::unordered_set<uint64_t>* highlight = nullptr;
	bool text = true;
};

Rect toRect(const GRect& r)
{
	return {r.x, r.y, r.w, r.h};
}

/// Outline of a rect as four (optionally dashed) lines.
void outlineDashed(DrawList& dl, const Rect& r, Color c, float width, float dash, float gap)
{
	const std::vector<GPoint> poly{{r.x, r.y}, {r.right(), r.y}, {r.right(), r.bottom()}, {r.x, r.bottom()}, {r.x, r.y}};
	for (const auto& [a, b]: model::dashPolyline(poly, dash, gap))
		dl.line({a.x, a.y}, {b.x, b.y}, c, width);
}

/// Paints a scene through `t`; scene point s lands at offset + t.toView(s). Items outside `clip` (context
/// coordinates) are skipped.
void paintScene(UiContext& ui, DrawList& dl, const model::GraphScene& sc, const model::ViewTransform& t, Vec2 offset,
	const Rect& clip, const PaintState& st, uint32_t nodeIdx)
{
	const float z = t.zoom;
	// SceneText faces: [mono][bold] (null = the view's own face).
	const NodeHandle view = ui.handleOf(nodeIdx);
	const fuse::text::Font* faces[2][2] = {{nullptr, styledFont(ui, view, false, true)},
		{styledFont(ui, view, true, false), styledFont(ui, view, true, true)}};
	const GRect visible = t.visibleScene(clip.w, clip.h);
	const GRect vis{visible.x + (clip.x - offset.x) / z, visible.y + (clip.y - offset.y) / z, visible.w, visible.h};
	auto V = [&](GPoint p) {
		const GPoint v = t.toView(p);
		return Vec2{offset.x + v.x, offset.y + v.y};
	};
	auto R = [&](const GRect& r) {
		const GRect v = t.toViewRect(r);
		return Rect{offset.x + v.x, offset.y + v.y, v.w, v.h};
	};

	// Regions
	for (const auto& reg: sc.regions)
	{
		if (!reg.visible || !reg.rect.intersects(vis)) continue;
		const Rect r = R(reg.rect);
		const float rad = reg.radius * z;
		dl.roundedRect(r, graphColor(reg.fill), {rad, rad, rad, rad}, graphColor(reg.border), std::max(1.f, reg.borderWidth * z));
		if (!reg.label.empty() && st.text && reg.labelSize * z >= kMinTextPx)
			ui.paintString(dl, reg.label, {r.x + 6.f * z, r.y + 2.f * z, r.w - 12.f * z, (reg.labelSize + 6.f) * z},
				graphColor(reg.labelColor), reg.labelSize * z, fuse::ui::TextAlignment::Start, nodeIdx);
	}

	// Edges
	std::vector<Vec2> pts;
	for (const auto& e: sc.edges)
	{
		if (!e.visible || e.polyline.size() < 2 || !e.bounds.intersects(vis)) continue;
		const bool hot = (st.hovered && (*st.hovered == e.from || *st.hovered == e.to))
			|| (st.selected && (*st.selected == e.from || *st.selected == e.to));
		// Translucent edges are pre-blended over the background: segment joints would otherwise double the alpha
		// and bead the line.
		const float a = float(model::rgbaA(e.color)) / 255.f * e.opacity;
		const Color raw = graphColor(e.color), bg = graphColor(sc.background);
		const Color c{raw.r * a + bg.r * (1.f - a), raw.g * a + bg.g * (1.f - a), raw.b * a + bg.b * (1.f - a), 1.f};
		const float w = std::max(0.75f, e.width * z * (hot ? 1.6f : 1.f));
		if (e.dashed)
		{
			std::vector<GPoint> vp;
			vp.reserve(e.polyline.size());
			for (const GPoint& p: e.polyline)
			{
				const Vec2 v = V(p);
				vp.push_back({v.x, v.y});
			}
			const float unit = std::max(1.f, e.width * z);
			for (const auto& [a, b]: model::dashPolyline(vp, e.dash * unit, e.gap * unit))
				dl.line({a.x, a.y}, {b.x, b.y}, c, w);
		}
		else
		{
			pts.clear();
			for (const GPoint& p: e.polyline)
				pts.push_back(V(p));
			for (size_t i = 1; i < pts.size(); ++i)
				dl.line(pts[i - 1], pts[i], c, w);
		}
		if (e.arrow)
		{
			const model::ArrowHead a = model::arrowHead(e.polyline.back(), e.polyline[e.polyline.size() - 2],
				e.arrowLength, e.arrowHalfWidth);
			const Vec2 tip = V(a.tip), l = V(a.left), r = V(a.right);
			dl.line(tip, l, c, w);
			dl.line(l, r, c, w);
			dl.line(r, tip, c, w);
		}
	}

	// Nodes
	const Color selCol = graphColor(model::catppuccin::blue);
	const Color hlCol = graphColor(model::catppuccin::yellow);
	for (const auto& n: sc.nodes)
	{
		if (!n.visible || !n.rect.intersects(vis)) continue;
		const Rect r = R(n.rect);
		const float rad = n.radius * z;
		const bool hovered = st.hovered && *st.hovered == n.id;
		const bool selected = st.selected && *st.selected == n.id;
		const float op = n.opacity;
		if (n.shadow) dl.roundedRect({r.x + 3.f * z, r.y + 3.f * z, r.w, r.h}, Color{0, 0, 0, 50.f / 255.f * op}, {rad, rad, rad, rad});
		const model::Rgba fill = hovered && n.hoverFill ? n.hoverFill : n.fill;
		dl.roundedRect(r, graphColor(fill, op), {rad, rad, rad, rad});
		if (n.headerFill && n.headerHeight > 0.f)
			dl.roundedRect({r.x, r.y, r.w, n.headerHeight * z}, graphColor(n.headerFill, op), {rad, rad, 0.f, 0.f});
		if (n.stripeFill && n.stripeWidth > 0.f)
		{
			const float sw = n.stripeWidth * z;
			dl.roundedRect({r.x, r.y, sw + rad * 0.5f, r.h}, graphColor(n.stripeFill, op), {rad, 0.f, 0.f, rad});
		}
		const model::Rgba border = hovered && n.hoverBorder ? n.hoverBorder : n.border;
		const float bw = std::max(1.f, (hovered ? n.borderWidth + 0.5f : n.borderWidth) * z);
		if (n.dashedBorder)
			outlineDashed(dl, r, graphColor(border, op), bw, 5.f * std::max(1.f, z), 3.f * std::max(1.f, z));
		else
			dl.roundedRect(r, Color{}, {rad, rad, rad, rad}, graphColor(border, op), bw);
		if (st.text)
			for (const auto& tx: n.texts)
			{
				const float px = tx.size * z;
				if (px < kMinTextPx) continue;
				const fuse::ui::TextAlignment al = tx.align == model::SceneAlign::Center ? fuse::ui::TextAlignment::Center
					: tx.align == model::SceneAlign::Right                               ? fuse::ui::TextAlignment::End
																						 : fuse::ui::TextAlignment::Start;
				ui.paintString(dl, tx.text, R(tx.box), graphColor(tx.color, op), px, al, nodeIdx, faces[tx.mono][tx.bold]);
			}
		if (st.highlight && st.highlight->count(n.id))
		{
			const float g = 3.f;
			dl.roundedRect({r.x - g, r.y - g, r.w + 2 * g, r.h + 2 * g}, Color{}, {rad + g, rad + g, rad + g, rad + g}, hlCol,
				2.f);
		}
		if (selected) dl.roundedRect(r, Color{}, {rad, rad, rad, rad}, selCol, std::max(1.5f, 2.5f * z));
	}
}

} // namespace

// ── Widget class glue ────────────────────────────────────────────────────────

struct GraphView::Impl
{
	static constexpr std::string_view kName = kClassName;
	static constexpr fuse::ui::A11yRole kRole = fuse::ui::A11yRole::Group;
	static constexpr bool kFocusable = true;
	static constexpr bool kInteractive = true;

	static GraphView* of(const UiContext& ui, NodeHandle n)
	{
		const uint64_t k = ui.alive(n) ? ui.userData(n) : 0;
		auto& v = views();
		if (k == 0 || k > v.size() || !v[k - 1] || v[k - 1]->m_node != n || &v[k - 1]->m_ui != &ui) return nullptr;
		return v[k - 1];
	}
	static fuse::ui::Size measure(UiContext&, NodeHandle, fuse::ui::Size) { return {320.f, 240.f}; }
	static void paint(const fuse::ui::PaintContext& pc, DrawList& dl)
	{
		if (GraphView* g = of(pc.ui, pc.node)) g->paint(pc, dl);
	}
	static bool onEvent(fuse::ui::EventContext& ec, const Event& e)
	{
		if (ec.phase != fuse::ui::Phase::Target) return false;
		GraphView* g = of(ec.ui, ec.node);
		return g && g->handle(e);
	}
};

GraphView::GraphView(UiContext& ui, NodeHandle parent, std::string_view id): m_ui(ui)
{
	if (ui.registry().find(kClassName) == 0xFFFFFFFFu) ui.registry().registerWidget<Impl>();
	auto made = ui.create(kClassName, parent, id);
	m_node = made ? *made : NodeHandle{};
	auto& v = views();
	size_t slot = v.size();
	for (size_t i = 0; i < v.size(); ++i)
		if (!v[i])
		{
			slot = i;
			break;
		}
	if (slot == v.size()) v.push_back(nullptr);
	v[slot] = this;
	m_slot = uint32_t(slot + 1);
	if (ui.alive(m_node))
	{
		ui.setUserData(m_node, m_slot);
		ui.setA11y(m_node, fuse::ui::A11yRole::Group, "Graph");
		ui.setCss(m_node, "flex-grow", "1");
		ui.setCss(m_node, "background", "#1e1e2e");
		m_menuConn = ui.connect(m_node, fuse::ui::SignalKind::Submitted,
			[this](UiContext&, NodeHandle, const fuse::ui::SignalArgs& a) {
				if (!m_menuPick) return;
				auto pick = std::move(m_menuPick);
				m_menuPick = nullptr;
				pick(int(a.value));
			});
	}
}

GraphView::~GraphView()
{
	auto& v = views();
	if (m_slot && m_slot <= v.size() && v[m_slot - 1] == this) v[m_slot - 1] = nullptr;
	if (m_tooltipTimer.valid()) m_ui.cancelTimer(m_tooltipTimer);
	if (m_menuConn.valid()) m_ui.disconnect(m_menuConn);
}

// ── Scene ────────────────────────────────────────────────────────────────────

void GraphView::setScene(model::GraphScene scene, bool fit)
{
	m_scene = std::move(scene);
	if (m_scene.bounds.empty() && !m_scene.nodes.empty()) m_scene.finalize();
	if (m_selected && !m_scene.node(*m_selected)) m_selected.reset();
	if (m_hovered && !m_scene.node(*m_hovered)) setHovered(std::nullopt);
	std::erase_if(m_highlight, [&](uint64_t id) { return !m_scene.node(id); });
	if (fit) fitGraph();
	m_ui.invalidate(m_node);
}

void GraphView::sceneChanged()
{
	if (m_hovered)
		if (const auto* n = m_scene.node(*m_hovered); !n || !n->visible) setHovered(std::nullopt);
	m_ui.invalidate(m_node);
}

void GraphView::clear()
{
	setScene({}, false);
	m_t = {};
	m_pendingFit = false;
	m_highlight.clear();
	m_selected.reset();
}

void GraphView::setEmptyText(std::string text)
{
	m_emptyText = std::move(text);
	m_ui.invalidate(m_node);
}

// ── View ─────────────────────────────────────────────────────────────────────

Rect GraphView::contentRect() const
{
	return m_ui.alive(m_node) ? m_ui.contentRect(m_node) : Rect{};
}

Rect GraphView::viewportRect() const
{
	return contentRect();
}

void GraphView::ensureFit()
{
	if (!m_pendingFit) return;
	const Rect c = contentRect();
	if (c.w <= 0.f || c.h <= 0.f) return;
	m_pendingFit = false;
	GRect b = m_scene.visibleBounds();
	b = b.empty() ? m_scene.bounds : b.adjusted(-20.f, -20.f, 20.f, 20.f);
	if (b.empty()) return;
	m_t = model::ViewTransform::fit(b, c.w, c.h, 20.f);
}

void GraphView::viewChanged()
{
	m_ui.invalidate(m_node);
	if (onViewChanged) onViewChanged();
}

void GraphView::fitGraph()
{
	m_pendingFit = true;
	ensureFit();
	viewChanged();
}

void GraphView::resetZoom()
{
	ensureFit();
	const Rect c = contentRect();
	const GPoint centre = m_t.toScene({c.w * 0.5f, c.h * 0.5f});
	m_t.zoom = 1.f;
	m_t.centerOn(centre, c.w, c.h);
	viewChanged();
}

void GraphView::zoomBy(float factor)
{
	const Rect c = contentRect();
	zoomAt(factor, {c.x + c.w * 0.5f, c.y + c.h * 0.5f});
}

void GraphView::zoomAt(float factor, Vec2 p)
{
	ensureFit();
	const Rect c = contentRect();
	m_t.zoomAt(factor, {p.x - c.x, p.y - c.y});
	viewChanged();
}

void GraphView::panBy(float dx, float dy)
{
	ensureFit();
	m_t.panByView(dx, dy);
	viewChanged();
}

bool GraphView::centerOn(uint64_t id)
{
	const model::SceneNode* n = m_scene.node(id);
	if (!n) return false;
	ensureFit();
	const Rect c = contentRect();
	m_t.centerOn(n->rect.center(), c.w, c.h);
	viewChanged();
	return true;
}

void GraphView::setTransform(const model::ViewTransform& t)
{
	m_pendingFit = false;
	m_t = t;
	viewChanged();
}

Vec2 GraphView::sceneToContext(GPoint p) const
{
	const Rect c = contentRect();
	const GPoint v = m_t.toView(p);
	return {c.x + v.x, c.y + v.y};
}

GPoint GraphView::contextToScene(Vec2 p) const
{
	const Rect c = contentRect();
	return m_t.toScene({p.x - c.x, p.y - c.y});
}

std::optional<Vec2> GraphView::nodeCenter(uint64_t id) const
{
	const model::SceneNode* n = m_scene.node(id);
	if (!n || !n->visible) return std::nullopt;
	return sceneToContext(n->rect.center());
}

// ── Selection / hover ────────────────────────────────────────────────────────

void GraphView::setSelectedNode(std::optional<uint64_t> id)
{
	if (id && !m_scene.node(*id)) id.reset();
	m_selected = id;
	m_ui.invalidate(m_node);
}

void GraphView::setHighlighted(const std::vector<uint64_t>& ids)
{
	m_highlight = {ids.begin(), ids.end()};
	m_ui.invalidate(m_node);
}

void GraphView::setHovered(std::optional<uint64_t> id)
{
	if (id == m_hovered) return;
	m_hovered = id;
	m_tooltipShown = false;
	if (m_tooltipTimer.valid())
	{
		m_ui.cancelTimer(m_tooltipTimer);
		m_tooltipTimer = {};
	}
	if (id)
		m_tooltipTimer = m_ui.startTimer(m_tooltipDelay, false, [this](UiContext&) {
			m_tooltipTimer = {};
			m_tooltipShown = true;
			m_ui.invalidate(m_node);
		});
	m_ui.invalidate(m_node);
	if (onHoverChanged) onHoverChanged(id);
}

std::string GraphView::tooltipText() const
{
	if (!m_hovered) return {};
	const model::SceneNode* n = m_scene.node(*m_hovered);
	return n ? n->tooltip : std::string();
}

void GraphView::setMinimapVisible(bool on)
{
	m_minimap = on;
	m_ui.invalidate(m_node);
}

Rect GraphView::minimapRect() const
{
	const Rect c = contentRect();
	if (!m_minimap || m_scene.empty() || c.w < kMiniW + 2 * kMiniMargin || c.h < kMiniH + 2 * kMiniMargin) return {};
	return {c.right() - kMiniW - kMiniMargin, c.y + kMiniMargin, kMiniW, kMiniH};
}

GRect GraphView::minimapScene(const Rect& mm, float& scale, Vec2& offset) const
{
	const GRect b = m_scene.bounds;
	const float pad = 4.f;
	scale = std::min((mm.w - 2 * pad) / std::max(1.f, b.w), (mm.h - 2 * pad) / std::max(1.f, b.h));
	offset = {mm.x + (mm.w - b.w * scale) * 0.5f - b.x * scale, mm.y + (mm.h - b.h * scale) * 0.5f - b.y * scale};
	return b;
}

void GraphView::centreFromMinimap(Vec2 p)
{
	const Rect mm = minimapRect();
	if (mm.empty()) return;
	float s = 1.f;
	Vec2 off;
	minimapScene(mm, s, off);
	const GPoint scenePt{(p.x - off.x) / s, (p.y - off.y) / s};
	const Rect c = contentRect();
	m_t.centerOn(scenePt, c.w, c.h);
	viewChanged();
}

fuse::ui::NodeHandle GraphView::openMenu(std::string_view items, Vec2 at, std::function<void(int)> onPick)
{
	m_menuPick = std::move(onPick);
	return fuse::ui::widgets::popupMenu(m_ui, m_node, items, at);
}

// ── Paint ────────────────────────────────────────────────────────────────────

void GraphView::paint(const fuse::ui::PaintContext& pc, DrawList& dl)
{
	ensureFit();
	const Rect c = pc.content;
	dl.pushClip(c);
	dl.rect(c, graphColor(m_scene.background));
	if (m_scene.empty())
	{
		if (!m_emptyText.empty())
			m_ui.paintString(dl, m_emptyText, c, graphColor(model::catppuccin::overlay0), 14.f,
				fuse::ui::TextAlignment::Center, pc.node.index);
		dl.popClip();
		return;
	}
	PaintState st{m_hovered, m_selected, &m_highlight, true};
	paintScene(m_ui, dl, m_scene, m_t, {c.x, c.y}, c, st, pc.node.index);

	// Minimap overlay: whole scene scaled, viewport indicator.
	if (const Rect mm = minimapRect(); !mm.empty())
	{
		dl.roundedRect(mm, Color{24.f / 255.f, 24.f / 255.f, 37.f / 255.f, 210.f / 255.f}, {4.f, 4.f, 4.f, 4.f},
			graphColor(model::catppuccin::surface1), 1.f);
		dl.pushClip(mm, 4.f);
		float s = 1.f;
		Vec2 off;
		minimapScene(mm, s, off);
		auto M = [&](const GRect& r) { return Rect{off.x + r.x * s, off.y + r.y * s, r.w * s, r.h * s}; };
		for (const auto& reg: m_scene.regions)
			if (reg.visible) dl.rect(M(reg.rect), graphColor(reg.fill));
		if (m_scene.edges.size() <= 4000)
			for (const auto& e: m_scene.edges)
				if (e.visible && !e.points.empty())
				{
					const GPoint a = e.points.front(), b = e.points.back();
					dl.line({off.x + a.x * s, off.y + a.y * s}, {off.x + b.x * s, off.y + b.y * s}, graphColor(e.color, 0.6f), 1.f);
				}
		for (const auto& n: m_scene.nodes)
			if (n.visible)
			{
				Rect r = M(n.rect);
				r.w = std::max(r.w, 1.5f);
				r.h = std::max(r.h, 1.5f);
				dl.rect(r, graphColor(n.border == model::catppuccin::surface1 ? model::catppuccin::overlay0 : n.border, n.opacity));
			}
		const GRect vs = m_t.visibleScene(c.w, c.h);
		dl.roundedRect(M(vs), Color{0x89 / 255.f, 0xb4 / 255.f, 0xfa / 255.f, 40.f / 255.f}, {},
			Color{0x89 / 255.f, 0xb4 / 255.f, 0xfa / 255.f, 200.f / 255.f}, 1.5f);
		dl.popClip();
	}

	// Tooltip
	if (tooltipVisible())
	{
		const std::string tip = tooltipText();
		if (!tip.empty())
		{
			std::vector<std::string_view> lines;
			for (size_t at = 0;;)
			{
				const size_t nl = tip.find('\n', at);
				lines.push_back(std::string_view(tip).substr(at, nl == std::string::npos ? std::string::npos : nl - at));
				if (nl == std::string::npos) break;
				at = nl + 1;
			}
			const float px = 12.f, lh = 16.f;
			float w = 0.f;
			for (auto l: lines)
				w = std::max(w, m_ui.measureString(l, px));
			w = std::min(w + 16.f, c.w - 8.f);
			const float h = std::min(lh * float(lines.size()) + 8.f, c.h - 8.f);
			const Vec2 p = m_ui.pointer();
			float x = p.x + 14.f, y = p.y + 18.f;
			if (x + w > c.right() - 4.f) x = std::max(c.x + 4.f, p.x - w - 8.f);
			if (y + h > c.bottom() - 4.f) y = std::max(c.y + 4.f, p.y - h - 8.f);
			dl.roundedRect({x, y, w, h}, graphColor(model::catppuccin::crust, 0.96f), {4.f, 4.f, 4.f, 4.f},
				graphColor(model::catppuccin::surface2), 1.f);
			dl.pushClip({x, y, w, h});
			for (size_t i = 0; i < lines.size(); ++i)
			{
				const float ly = y + 4.f + lh * float(i);
				if (ly + lh > y + h) break;
				m_ui.paintString(dl, lines[i], {x + 8.f, ly, w - 16.f, lh}, graphColor(model::catppuccin::text), px,
					fuse::ui::TextAlignment::Start, pc.node.index);
			}
			dl.popClip();
		}
	}
	dl.popClip();
}

// ── Input ────────────────────────────────────────────────────────────────────

bool GraphView::handle(const Event& e)
{
	const Rect c = contentRect();
	auto hit = [&](Vec2 p) -> std::optional<uint64_t> {
		if (!c.contains(p)) return std::nullopt;
		const int i = m_scene.nodeAt(m_t.toScene({p.x - c.x, p.y - c.y}));
		if (i < 0) return std::nullopt;
		return m_scene.nodes[size_t(i)].id;
	};
	switch (e.type)
	{
	case EventType::PointerDown:
	{
		ensureFit();
		m_last = e.pos;
		if (e.button == MouseButton::Middle)
		{
			m_drag = Drag::Pan;
			return true;
		}
		const Rect mm = minimapRect();
		if (e.button == MouseButton::Left && !mm.empty() && mm.contains(e.pos))
		{
			m_drag = Drag::Minimap;
			centreFromMinimap(e.pos);
			return true;
		}
		const std::optional<uint64_t> id = hit(e.pos);
		if (e.button == MouseButton::Right)
		{
			if (id)
			{
				setSelectedNode(id);
				if (onNodeContextMenu) onNodeContextMenu(*id, e.pos);
			}
			else if (onBackgroundContextMenu)
				onBackgroundContextMenu(e.pos);
			return true;
		}
		if (!id)
		{
			m_drag = Drag::Pan;
			m_lastClickNode.reset();
			setSelectedNode(std::nullopt);
			return true;
		}
		m_drag = Drag::None;
		// FuseUI counts the clicks (UiContext double-click settings); the second
		// press must land on the same graph node.
		const bool dbl = e.clicks == 2 && m_lastClickNode == id;
		m_lastClickNode = id;
		setSelectedNode(id);
		// Callbacks last: they may replace the scene.
		const uint64_t nodeId = *id;
		if (onNodeClicked) onNodeClicked(nodeId);
		if (dbl && onNodeActivated) onNodeActivated(nodeId);
		return true;
	}
	case EventType::PointerMove:
		if (m_drag == Drag::Pan)
		{
			const float dx = e.pos.x - m_last.x, dy = e.pos.y - m_last.y;
			m_last = e.pos;
			if (dx != 0.f || dy != 0.f) panBy(dx, dy);
			return true;
		}
		if (m_drag == Drag::Minimap)
		{
			centreFromMinimap(e.pos);
			return true;
		}
		{
			const Rect mm = minimapRect();
			setHovered(!mm.empty() && mm.contains(e.pos) ? std::nullopt : hit(e.pos));
			if (m_tooltipShown) m_ui.invalidate(m_node); // tooltip follows the pointer
		}
		return true;
	case EventType::PointerUp:
		m_drag = Drag::None;
		return true;
	case EventType::PointerLeave:
		if (m_drag == Drag::None) setHovered(std::nullopt);
		return false;
	case EventType::Wheel:
	{
		if (e.mods & fuse::ui::kModCtrl)
		{
			zoomAt(std::pow(1.18f, e.wheel.y), e.pos);
			return true;
		}
		const float step = 40.f;
		if (e.mods & fuse::ui::kModShift)
			panBy(e.wheel.y * step, 0.f);
		else
			panBy(e.wheel.x * step, e.wheel.y * step);
		return true;
	}
	case EventType::KeyDown:
		if (e.mods & fuse::ui::kModCtrl)
		{
			if (e.key == Key::F)
			{
				fitGraph();
				return true;
			}
			if (e.key == Key::Num0)
			{
				resetZoom();
				return true;
			}
			return false;
		}
		switch (e.key)
		{
		case Key::Left: panBy(40.f, 0.f); return true;
		case Key::Right: panBy(-40.f, 0.f); return true;
		case Key::Up: panBy(0.f, 40.f); return true;
		case Key::Down: panBy(0.f, -40.f); return true;
		case Key::Enter:
			if (m_selected && onNodeActivated)
			{
				const uint64_t id = *m_selected;
				onNodeActivated(id);
			}
			return true;
		case Key::Escape:
			m_highlight.clear();
			setSelectedNode(std::nullopt);
			return true;
		default: return false;
		}
	default: return false;
	}
}

// ── Export ───────────────────────────────────────────────────────────────────

fuse::ui::Image GraphView::renderImage(float scale)
{
	fuse::ui::Image img;
	GRect b = m_scene.bounds;
	if (b.empty()) return img;
	b = b.adjusted(-10.f, -10.f, 10.f, 10.f);
	constexpr float kMaxSide = 16384.f;
	scale = std::min(scale, kMaxSide / std::max(b.w, b.h));
	const int w = std::max(1, int(std::ceil(b.w * scale))), h = std::max(1, int(std::ceil(b.h * scale)));
	img.resize(w, h, fuse::ui::packPremultiplied(graphColor(m_scene.background)));
	model::ViewTransform t;
	t.zoom = scale;
	t.origin = {b.x, b.y};
	DrawList dl;
	const Rect clip{0.f, 0.f, float(w), float(h)};
	PaintState st;
	st.text = true;
	paintScene(m_ui, dl, m_scene, t, {0.f, 0.f}, clip, st, m_node.index);
	fuse::ui::CpuRasterizer r;
	r.setGlyphAtlas(m_ui.atlas().pixels(), m_ui.atlas().size());
	r.render(dl, img);
	return img;
}

std::expected<void, std::string> GraphView::exportPng(const std::filesystem::path& path, float scale)
{
	const fuse::ui::Image img = renderImage(scale);
	if (img.width <= 0) return std::unexpected("nothing to export");
	if (!fuse::app::writePng(img, path.string())) return std::unexpected("cannot write " + path.string());
	return {};
}

} // namespace retdec::gui::ui

namespace retdec::gui::ui {

const char* graphExportExtension(GraphExportFormat f)
{
	switch (f)
	{
	case GraphExportFormat::Svg: return "svg";
	case GraphExportFormat::Png: return "png";
	case GraphExportFormat::Dot: return "dot";
	}
	return "";
}

void graphSetShown(fuse::ui::UiContext& ui, fuse::ui::NodeHandle n, bool shown)
{
	if (ui.alive(n)) ui.set(n, fuse::ui::Prop::Display, fuse::ui::PropValue::ofBool(shown));
}

bool graphShown(const fuse::ui::UiContext& ui, fuse::ui::NodeHandle n)
{
	return ui.alive(n) && ui.computed(n).visible;
}

} // namespace retdec::gui::ui

namespace retdec::gui::ui {

SavePrompt::~SavePrompt()
{
	if (m_conn.valid()) m_ui.disconnect(m_conn);
}

void SavePrompt::open(std::string_view title, std::string_view filter, std::function<void(const std::string&)> onChosen)
{
	m_onChosen = std::move(onChosen);
	if (m_conn.valid()) m_ui.disconnect(m_conn);
	if (m_ui.alive(m_dialog)) m_ui.destroy(m_dialog);
	std::error_code ec;
	const std::string dir = std::filesystem::current_path(ec).generic_string();
	m_dialog = fuse::ui::widgets::fileDialog(m_ui, title, dir, filter, /*save=*/true);
	m_conn = m_ui.connect(m_dialog, fuse::ui::SignalKind::Submitted,
		[this](fuse::ui::UiContext&, fuse::ui::NodeHandle, const fuse::ui::SignalArgs& a) {
			const std::string path(a.text);
			if (!path.empty() && m_onChosen) m_onChosen(path);
		});
	fuse::ui::widgets::openDialog(m_ui, m_dialog);
}

} // namespace retdec::gui::ui

namespace retdec::gui::ui {

fuse::ui::NodeHandle graphToolbarRow(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent)
{
	const fuse::ui::NodeHandle t = fuse::ui::widgets::panel(ui, parent);
	ui.setCss(t, "flex-direction", "row");
	ui.setCss(t, "align-items", "center");
	ui.setCss(t, "gap", "6px");
	ui.setCss(t, "padding", "3px 6px");
	ui.setCss(t, "background", "#181825");
	return t;
}

fuse::ui::NodeHandle graphToolButton(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, std::string_view text)
{
	const fuse::ui::NodeHandle b = fuse::ui::widgets::button(ui, parent, text);
	ui.setCss(b, "padding", "2px 8px");
	ui.setCss(b, "font-size", "12px");
	ui.setCss(b, "background", "#313244");
	ui.setCss(b, "color", "#cdd6f4");
	ui.setCss(b, "border-color", "#45475a");
	ui.setCss(b, "border-width", "1px");
	return b;
}

fuse::ui::NodeHandle graphMutedLabel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, std::string_view text)
{
	const fuse::ui::NodeHandle l = fuse::ui::widgets::label(ui, parent, text);
	ui.setCss(l, "color", "#a6adc8");
	ui.setCss(l, "font-size", "12px");
	return l;
}


} // namespace retdec::gui::ui
