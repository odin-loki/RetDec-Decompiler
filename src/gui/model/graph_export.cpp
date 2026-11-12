/**
 * @file src/gui/model/graph_export.cpp
 * @brief SVG export of graph scenes, escaping helpers, file writing.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/graph_export.h"

#include <forge/os/fs.hpp>

#include <cmath>
#include <format>

namespace retdec::gui::model {

std::string graphXmlEscape(std::string_view s)
{
	std::string out;
	out.reserve(s.size());
	for (char c: s)
	{
		switch (c)
		{
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		case '\'': out += "&apos;"; break;
		default: out += c;
		}
	}
	return out;
}

std::string graphDotEscape(std::string_view s)
{
	std::string out;
	out.reserve(s.size());
	for (char c: s)
	{
		switch (c)
		{
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\n': out += "\\n"; break;
		case '\r': break;
		default: out += c;
		}
	}
	return out;
}

namespace {

std::string num(float v)
{
	// Two decimals, trailing zeros trimmed: compact and stable.
	std::string s = std::format("{:.2f}", double(v));
	while (!s.empty() && s.back() == '0')
		s.pop_back();
	if (!s.empty() && s.back() == '.') s.pop_back();
	if (s == "-0") s = "0";
	return s;
}

std::string fillAttr(const char* name, Rgba c)
{
	if (rgbaA(c) == 0) return std::format(" {}=\"none\"", name);
	std::string s = std::format(" {}=\"{}\"", name, rgbaCss(c));
	if (rgbaA(c) != 255) s += std::format(" {}-opacity=\"{}\"", name, num(float(rgbaA(c)) / 255.f));
	return s;
}

/// Path of a rectangle with only the top corners rounded (header stripes).
std::string topRoundedPath(const GRect& r, float rad)
{
	rad = std::min({rad, r.w * 0.5f, r.h});
	return std::format(
		"M{},{} L{},{} Q{},{} {},{} L{},{} Q{},{} {},{} L{},{} Z",
		num(r.x),
		num(r.bottom()),
		num(r.x),
		num(r.y + rad),
		num(r.x),
		num(r.y),
		num(r.x + rad),
		num(r.y),
		num(r.right() - rad),
		num(r.y),
		num(r.right()),
		num(r.y),
		num(r.right()),
		num(r.y + rad),
		num(r.right()),
		num(r.bottom()));
}

void writeText(std::string& out, const SceneText& t, float opacity)
{
	float x = t.box.x;
	const char* anchor = "start";
	if (t.align == SceneAlign::Center)
	{
		x = t.box.x + t.box.w * 0.5f;
		anchor = "middle";
	}
	else if (t.align == SceneAlign::Right)
	{
		x = t.box.right();
		anchor = "end";
	}
	const float y = t.box.y + t.box.h * 0.5f;
	out += std::format(
		"<text x=\"{}\" y=\"{}\" font-size=\"{}\" text-anchor=\"{}\" dominant-baseline=\"central\" "
		"font-family=\"{}\"{}{}>{}</text>\n",
		num(x),
		num(y),
		num(t.size),
		anchor,
		t.mono ? "Cascadia Code, Consolas, DejaVu Sans Mono, monospace" : "Open Sans, sans-serif",
		t.bold ? " font-weight=\"bold\"" : "",
		fillAttr("fill", rgbaFade(t.color, opacity)),
		graphXmlEscape(t.text));
}

} // namespace

std::string sceneToSvg(const GraphScene& scene, const SvgOptions& opt)
{
	const GRect b = scene.bounds.empty() ? GRect{0, 0, 1, 1} : scene.bounds;
	std::string out;
	out.reserve(256 + scene.nodes.size() * 400 + scene.edges.size() * 200);
	out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	out += std::format(
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{}\" height=\"{}\" viewBox=\"{} {} {} {}\">\n",
		num(std::ceil(b.w)),
		num(std::ceil(b.h)),
		num(b.x),
		num(b.y),
		num(b.w),
		num(b.h));
	out += std::format("<title>{}</title>\n", graphXmlEscape(opt.title));
	if (opt.background)
		out += std::format(
			"<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"{}/>\n",
			num(b.x),
			num(b.y),
			num(b.w),
			num(b.h),
			fillAttr("fill", scene.background));

	out += "<g id=\"regions\">\n";
	for (const SceneRegion& r: scene.regions)
	{
		if (!r.visible && !opt.includeHidden) continue;
		out += std::format(
			"<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"{}\"{}{} stroke-width=\"{}\"/>\n",
			num(r.rect.x),
			num(r.rect.y),
			num(r.rect.w),
			num(r.rect.h),
			num(r.radius),
			fillAttr("fill", r.fill),
			fillAttr("stroke", r.border),
			num(r.borderWidth));
		if (!r.label.empty())
			writeText(
				out,
				SceneText{
					r.label,
					{r.rect.x + 6.f, r.rect.y + 4.f, r.rect.w - 12.f, r.labelSize + 4.f},
					r.labelSize,
					r.labelColor,
					SceneAlign::Left,
					true,
					true},
				1.f);
	}
	out += "</g>\n<g id=\"edges\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n";
	for (const SceneEdge& e: scene.edges)
	{
		if ((!e.visible && !opt.includeHidden) || e.points.empty()) continue;
		std::string d = std::format("M{},{}", num(e.points[0].x), num(e.points[0].y));
		for (size_t i = 1; i + 2 < e.points.size(); i += 3)
			d += std::format(
				" C{},{} {},{} {},{}",
				num(e.points[i].x),
				num(e.points[i].y),
				num(e.points[i + 1].x),
				num(e.points[i + 1].y),
				num(e.points[i + 2].x),
				num(e.points[i + 2].y));
		const Rgba col = rgbaFade(e.color, e.opacity);
		std::string dash;
		if (e.dashed) dash = std::format(" stroke-dasharray=\"{} {}\"", num(e.dash * e.width), num(e.gap * e.width));
		out +=
			std::format("<path d=\"{}\"{} stroke-width=\"{}\"{}/>\n", d, fillAttr("stroke", col), num(e.width), dash);
		if (e.arrow && e.points.size() >= 4)
		{
			const GPoint tip = e.points.back();
			const GPoint from = e.points[e.points.size() - 4]; // the Qt items aimed the head from the edge start
			const ArrowHead a = arrowHead(
				tip,
				e.polyline.size() >= 2 ? e.polyline[e.polyline.size() - 2] : from,
				e.arrowLength,
				e.arrowHalfWidth);
			out += std::format(
				"<path d=\"M{},{} L{},{} L{},{} Z\"{} stroke-width=\"{}\"/>\n",
				num(a.tip.x),
				num(a.tip.y),
				num(a.left.x),
				num(a.left.y),
				num(a.right.x),
				num(a.right.y),
				fillAttr("stroke", col),
				num(e.width));
		}
	}
	out += "</g>\n<g id=\"nodes\">\n";
	for (const SceneNode& n: scene.nodes)
	{
		if (!n.visible && !opt.includeHidden) continue;
		out += std::format(
			"<g data-id=\"0x{:x}\"{}>\n",
			n.id,
			n.opacity < 1.f ? std::format(" opacity=\"{}\"", num(n.opacity)) : std::string());
		if (!n.tooltip.empty()) out += std::format("<title>{}</title>\n", graphXmlEscape(n.tooltip));
		const GRect& r = n.rect;
		if (n.shadow)
			out += std::format(
				"<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"{}\" fill=\"#000000\" "
				"fill-opacity=\"0.2\"/>\n",
				num(r.x + 3.f),
				num(r.y + 3.f),
				num(r.w),
				num(r.h),
				num(n.radius));
		out += std::format(
			"<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"{}\"{}{} stroke-width=\"{}\"{}/>\n",
			num(r.x),
			num(r.y),
			num(r.w),
			num(r.h),
			num(n.radius),
			fillAttr("fill", n.fill),
			fillAttr("stroke", n.border),
			num(n.borderWidth),
			n.dashedBorder ? " stroke-dasharray=\"5 3\"" : "");
		if (n.headerFill && n.headerHeight > 0.f)
			out += std::format(
				"<path d=\"{}\"{}/>\n",
				topRoundedPath({r.x, r.y, r.w, n.headerHeight}, n.radius),
				fillAttr("fill", n.headerFill));
		if (n.stripeFill && n.stripeWidth > 0.f)
			out += std::format(
				"<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\" rx=\"{}\"{}/>\n",
				num(r.x),
				num(r.y),
				num(n.stripeWidth),
				num(r.h),
				num(std::min(n.radius, n.stripeWidth)),
				fillAttr("fill", n.stripeFill));
		for (const SceneText& t: n.texts)
			writeText(out, t, 1.f);
		out += "</g>\n";
	}
	out += "</g>\n</svg>\n";
	return out;
}

std::expected<void, std::string> writeGraphExportFile(const std::filesystem::path& path, std::string_view text)
{
	const auto st = forge::os::fs::write_file_atomic(forge::os::fs::u8str(path), text);
	if (!st) return std::unexpected(st.err.what);
	return {};
}

} // namespace retdec::gui::model
