/**
 * @file src/gui/model/graph_scene.cpp
 * @brief GraphScene geometry: path flattening, dashes, arrowheads, bounds, hit testing, view transforms.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/graph_scene.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace retdec::gui::model {

GRect GRect::united(const GRect& o) const
{
	if (empty()) return o;
	if (o.empty()) return *this;
	const float x0 = std::min(x, o.x), y0 = std::min(y, o.y);
	return {x0, y0, std::max(right(), o.right()) - x0, std::max(bottom(), o.bottom()) - y0};
}

Rgba rgbaFade(Rgba c, float f)
{
	const float a = std::clamp(float(rgbaA(c)) * f, 0.f, 255.f);
	return rgbaWithAlpha(c, uint8_t(std::lround(a)));
}

std::string rgbaCss(Rgba c)
{
	return std::format("#{:02x}{:02x}{:02x}", rgbaR(c), rgbaG(c), rgbaB(c));
}

// ── Scene ────────────────────────────────────────────────────────────────────

void GraphScene::clear()
{
	regions.clear();
	edges.clear();
	nodes.clear();
	bounds = {};
	m_index.clear();
}

namespace {

GRect boundsOf(const std::vector<GPoint>& pts)
{
	if (pts.empty()) return {};
	float x0 = std::numeric_limits<float>::max(), y0 = x0;
	float x1 = std::numeric_limits<float>::lowest(), y1 = x1;
	for (const GPoint& p: pts)
	{
		x0 = std::min(x0, p.x);
		y0 = std::min(y0, p.y);
		x1 = std::max(x1, p.x);
		y1 = std::max(y1, p.y);
	}
	// Degenerate (axis-aligned) edges still get an area so they intersect the view.
	return {x0 - 1.f, y0 - 1.f, x1 - x0 + 2.f, y1 - y0 + 2.f};
}

} // namespace

void GraphScene::finalize(float margin)
{
	m_index.clear();
	m_index.reserve(nodes.size());
	GRect all;
	for (int i = 0; i < int(nodes.size()); ++i)
	{
		m_index[nodes[size_t(i)].id] = i;
		all = all.united(nodes[size_t(i)].rect);
	}
	for (SceneEdge& e: edges)
	{
		e.polyline = flattenCubicPath(e.points);
		e.bounds = boundsOf(e.polyline).adjusted(-e.arrowHalfWidth, -e.arrowHalfWidth, e.arrowHalfWidth, e.arrowHalfWidth);
		all = all.united(e.bounds);
	}
	for (const SceneRegion& r: regions)
		all = all.united(r.rect);
	bounds = all.empty() ? GRect{} : all.adjusted(-margin, -margin, margin, margin);
}

int GraphScene::findNode(uint64_t id) const
{
	const auto it = m_index.find(id);
	if (it != m_index.end() && it->second < int(nodes.size()) && nodes[size_t(it->second)].id == id) return it->second;
	// Index stale or not built: linear fallback.
	for (int i = 0; i < int(nodes.size()); ++i)
		if (nodes[size_t(i)].id == id) return i;
	return -1;
}

const SceneNode* GraphScene::node(uint64_t id) const
{
	const int i = findNode(id);
	return i < 0 ? nullptr : &nodes[size_t(i)];
}

SceneNode* GraphScene::node(uint64_t id)
{
	const int i = findNode(id);
	return i < 0 ? nullptr : &nodes[size_t(i)];
}

int GraphScene::nodeAt(GPoint p) const
{
	for (int i = int(nodes.size()); i-- > 0;)
	{
		const SceneNode& n = nodes[size_t(i)];
		if (n.visible && n.rect.contains(p)) return i;
	}
	return -1;
}

GRect GraphScene::visibleBounds() const
{
	GRect all;
	for (const SceneNode& n: nodes)
		if (n.visible) all = all.united(n.rect);
	for (const SceneRegion& r: regions)
		if (r.visible) all = all.united(r.rect);
	return all;
}

int GraphScene::visibleNodeCount() const
{
	return int(std::count_if(nodes.begin(), nodes.end(), [](const SceneNode& n) { return n.visible; }));
}

// ── Paths ────────────────────────────────────────────────────────────────────

GPoint cubicPoint(GPoint p0, GPoint c1, GPoint c2, GPoint p1, float t)
{
	const float u = 1.f - t;
	const float a = u * u * u, b = 3.f * u * u * t, c = 3.f * u * t * t, d = t * t * t;
	return {a * p0.x + b * c1.x + c * c2.x + d * p1.x, a * p0.y + b * c1.y + c * c2.y + d * p1.y};
}

std::vector<GPoint> flattenCubicPath(const std::vector<GPoint>& pts, int segments)
{
	std::vector<GPoint> out;
	if (pts.empty()) return out;
	segments = std::max(1, segments);
	out.reserve(1 + (pts.size() / 3) * size_t(segments));
	out.push_back(pts[0]);
	for (size_t i = 1; i + 2 < pts.size(); i += 3)
	{
		const GPoint p0 = out.back();
		for (int s = 1; s <= segments; ++s)
			out.push_back(cubicPoint(p0, pts[i], pts[i + 1], pts[i + 2], float(s) / float(segments)));
	}
	return out;
}

std::vector<std::pair<GPoint, GPoint>> dashPolyline(const std::vector<GPoint>& poly, float dash, float gap)
{
	std::vector<std::pair<GPoint, GPoint>> out;
	if (poly.size() < 2) return out;
	if (dash <= 0.f || gap <= 0.f)
	{
		for (size_t i = 1; i < poly.size(); ++i)
			out.emplace_back(poly[i - 1], poly[i]);
		return out;
	}
	bool on = true;
	float left = dash; // length remaining in the current dash / gap
	for (size_t i = 1; i < poly.size(); ++i)
	{
		GPoint a = poly[i - 1];
		const GPoint b = poly[i];
		float segLen = std::hypot(b.x - a.x, b.y - a.y);
		if (segLen <= 1e-6f) continue;
		const float dx = (b.x - a.x) / segLen, dy = (b.y - a.y) / segLen;
		while (segLen > 0.f)
		{
			const float step = std::min(left, segLen);
			const GPoint m{a.x + dx * step, a.y + dy * step};
			if (on) out.emplace_back(a, m);
			a = m;
			segLen -= step;
			left -= step;
			if (left <= 1e-6f)
			{
				on = !on;
				left = on ? dash : gap;
			}
		}
	}
	return out;
}

ArrowHead arrowHead(GPoint tip, GPoint from, float length, float halfWidth)
{
	float dx = tip.x - from.x, dy = tip.y - from.y;
	const float len = std::hypot(dx, dy);
	if (len < 1e-6f) return {tip, tip, tip};
	dx /= len;
	dy /= len;
	const GPoint base{tip.x - dx * length, tip.y - dy * length};
	const GPoint perp{-dy, dx};
	return {tip, {base.x + perp.x * halfWidth, base.y + perp.y * halfWidth},
		{base.x - perp.x * halfWidth, base.y - perp.y * halfWidth}};
}

// ── View transform ───────────────────────────────────────────────────────────

ViewTransform ViewTransform::fit(const GRect& content, float viewW, float viewH, float pad)
{
	ViewTransform t;
	if (content.empty() || viewW <= 0.f || viewH <= 0.f) return t;
	const GRect r = content.adjusted(-pad, -pad, pad, pad);
	t.zoom = std::clamp(std::min(viewW / r.w, viewH / r.h), kMinZoom, kMaxZoom);
	const GPoint c = r.center();
	t.centerOn(c, viewW, viewH);
	return t;
}

void ViewTransform::zoomAt(float factor, GPoint anchor)
{
	const GPoint s = toScene(anchor);
	zoom = std::clamp(zoom * factor, kMinZoom, kMaxZoom);
	origin = {s.x - anchor.x / zoom, s.y - anchor.y / zoom};
}

void ViewTransform::centerOn(GPoint p, float w, float h)
{
	origin = {p.x - w * 0.5f / zoom, p.y - h * 0.5f / zoom};
}

} // namespace retdec::gui::model
