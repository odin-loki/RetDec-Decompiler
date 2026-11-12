/**
 * @file src/gui/model/cfg_graph.cpp
 * @brief CFG compression, loop detection, layout, routing, scene building and DOT export.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/cfg_graph.h"

#include "retdec/gui/model/graph_export.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <unordered_set>

namespace retdec::gui::model {

Rgba cfgEdgeColor(EdgeKind kind)
{
	switch (kind)
	{
	case EdgeKind::FallThrough: return palette::subtext0;
	case EdgeKind::TrueBranch: return palette::green;
	case EdgeKind::FalseBranch: return palette::red;
	case EdgeKind::BackEdge: return palette::peach;
	case EdgeKind::ExceptionEdge: return palette::mauve;
	case EdgeKind::UnresolvedIndirect: return palette::red;
	}
	return palette::subtext0;
}

bool cfgEdgeDashed(EdgeKind kind)
{
	return kind == EdgeKind::BackEdge || kind == EdgeKind::UnresolvedIndirect;
}

const char* edgeKindName(EdgeKind kind)
{
	switch (kind)
	{
	case EdgeKind::FallThrough: return "fall-through";
	case EdgeKind::TrueBranch: return "true";
	case EdgeKind::FalseBranch: return "false";
	case EdgeKind::BackEdge: return "back edge";
	case EdgeKind::ExceptionEdge: return "exception";
	case EdgeKind::UnresolvedIndirect: return "unresolved indirect";
	}
	return "edge";
}

float cfgBlockHeight(const BasicBlockData& b, int maxPreview)
{
	const int n = int(b.instrs.size());
	const int preview = std::min(n, maxPreview);
	const bool truncated = n > maxPreview;
	return cfg::kHeaderH + float(preview) * cfg::kInstrH + (truncated ? cfg::kInstrH : 0.f) + cfg::kFooterH;
}

std::string cfgBlockHeaderText(const BasicBlockData& b)
{
	std::string s = std::format("0x{:016x}", b.address);
	if (b.isEntry)
		s += "  [entry]";
	else if (b.isExit)
		s += "  [exit]";
	else if (b.isLoopHeader)
		s += "  [loop header]";
	return s;
}

bool isChainSummary(const BasicBlockData& b)
{
	return b.instrs.size() == 1 && b.instrs.front().text.starts_with("[chain:");
}

std::string cfgBlockTooltip(const BasicBlockData& b, int maxLines)
{
	std::string tip = hexAddress(b.address);
	if (b.endAddress > b.address) tip += std::format(" - {}", hexAddress(b.endAddress));
	if (b.isEntry) tip += "  [entry]";
	if (b.isExit) tip += "  [exit]";
	if (b.isLoopHeader) tip += "  [loop header]";
	if (isChainSummary(b))
	{
		tip += "\n" + b.instrs.front().text + "\nClick to expand chains";
		return tip;
	}
	tip += std::format("\n{} instruction{}", b.instrs.size(), b.instrs.size() == 1 ? "" : "s");
	if (b.loopId >= 0) tip += std::format(", loop {}", b.loopId);
	const int n = std::min<int>(int(b.instrs.size()), maxLines);
	for (int i = 0; i < n; ++i)
		tip += std::format("\n{}  {}", hexAddress(b.instrs[size_t(i)].address), b.instrs[size_t(i)].text);
	if (int(b.instrs.size()) > n) tip += std::format("\n… {} more", int(b.instrs.size()) - n);
	return tip;
}

// ── Chain compression ────────────────────────────────────────────────────────

CfgGraphData compressChains(const std::vector<BasicBlockData>& in, const std::vector<CFGEdgeData>& inEdges,
	std::unordered_map<uint64_t, std::vector<uint64_t>>* members)
{
	CfgGraphData out;
	std::unordered_map<uint64_t, std::vector<uint64_t>> succ, pred;
	std::unordered_map<uint64_t, const BasicBlockData*> byId;
	for (const auto& b: in)
		byId[b.id] = &b;
	for (const auto& e: inEdges)
	{
		succ[e.from].push_back(e.to);
		pred[e.to].push_back(e.from);
	}
	std::unordered_set<uint64_t> suppressible;
	for (const auto& b: in)
	{
		if (b.isEntry || b.isExit || b.isLoopHeader) continue;
		if (succ[b.id].size() == 1 && pred[b.id].size() == 1 && succ[b.id][0] != b.id) suppressible.insert(b.id);
	}

	std::unordered_set<uint64_t> visited;
	auto emitOutgoing = [&](uint64_t from, uint64_t as) {
		for (const auto& e: inEdges)
			if (e.from == from)
			{
				CFGEdgeData ne = e;
				ne.from = as;
				out.edges.push_back(ne);
			}
	};
	for (const auto& b: in)
	{
		if (visited.count(b.id) || suppressible.count(b.id)) continue;
		visited.insert(b.id);
		std::vector<uint64_t> chain{b.id};
		uint64_t cur = b.id;
		while (succ[cur].size() == 1 && suppressible.count(succ[cur][0]) && !visited.count(succ[cur][0])
			&& byId.count(succ[cur][0]))
		{
			cur = succ[cur][0];
			visited.insert(cur);
			chain.push_back(cur);
		}
		if (chain.size() > 1)
		{
			int instrCount = 0;
			for (uint64_t id: chain)
				instrCount += int(byId[id]->instrs.size());
			BasicBlockData compressed = b;
			compressed.instrs = {BlockInstr{b.address, std::format("[chain: {} instructions]", instrCount)}};
			const BasicBlockData& tail = *byId[cur];
			if (tail.endAddress > compressed.endAddress) compressed.endAddress = tail.endAddress;
			out.blocks.push_back(std::move(compressed));
			emitOutgoing(cur, b.id);
		}
		else
		{
			out.blocks.push_back(b);
			emitOutgoing(b.id, b.id);
		}
		if (members) (*members)[b.id] = chain;
	}
	// Suppressible nodes no head reached (a ring of in = out = 1 blocks) stay as they are.
	for (const auto& b: in)
	{
		if (visited.count(b.id)) continue;
		visited.insert(b.id);
		out.blocks.push_back(b);
		emitOutgoing(b.id, b.id);
		if (members) (*members)[b.id] = {b.id};
	}
	return out;
}

uint64_t cfgEntryBlock(const std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges)
{
	if (blocks.empty()) return 0;
	for (const auto& b: blocks)
		if (b.isEntry) return b.id;
	std::unordered_set<uint64_t> hasPred;
	for (const auto& e: edges)
		if (e.from != e.to) hasPred.insert(e.to);
	for (const auto& b: blocks)
		if (!hasPred.count(b.id)) return b.id;
	return blocks.front().id;
}

// ── Loops ────────────────────────────────────────────────────────────────────

std::vector<NaturalLoop> detectNaturalLoops(const std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges)
{
	std::vector<uint64_t> ids;
	std::unordered_map<uint64_t, size_t> order;
	for (const auto& b: blocks)
	{
		order[b.id] = ids.size();
		ids.push_back(b.id);
	}
	std::vector<EdgeRef> refs;
	for (const auto& e: edges)
		if (order.count(e.from) && order.count(e.to)) refs.push_back({e.from, e.to});
	const GraphAdjacency adj = GraphAdjacency::build(refs);
	const EdgeSet back = findBackEdges(ids, adj.succ, cfgEntryBlock(blocks, edges));

	std::map<size_t, NaturalLoop> byHeader; // header position -> loop
	// Sort back edges for determinism.
	std::vector<std::pair<uint64_t, uint64_t>> backList(back.begin(), back.end());
	std::sort(backList.begin(), backList.end(), [&](const auto& a, const auto& b) {
		return std::pair(order[a.second], order[a.first]) < std::pair(order[b.second], order[b.first]);
	});
	for (const auto& [tail, header]: backList)
	{
		NaturalLoop& loop = byHeader[order[header]];
		loop.header = header;
		loop.backEdges.push_back({tail, header});
		std::unordered_set<uint64_t> body(loop.body.begin(), loop.body.end());
		body.insert(header);
		std::vector<uint64_t> work;
		if (body.insert(tail).second) work.push_back(tail);
		while (!work.empty())
		{
			const uint64_t n = work.back();
			work.pop_back();
			for (uint64_t p: adj.predecessors(n))
				if (body.insert(p).second) work.push_back(p);
		}
		loop.body.assign(body.begin(), body.end());
		std::sort(loop.body.begin(), loop.body.end(), [&](uint64_t a, uint64_t b) { return order[a] < order[b]; });
	}
	std::vector<NaturalLoop> out;
	for (auto& [pos, loop]: byHeader)
		out.push_back(std::move(loop));
	return out;
}

int annotateLoops(std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges)
{
	for (const auto& b: blocks)
		if (b.loopId >= 0) return 0;
	const auto loops = detectNaturalLoops(blocks, edges);
	if (loops.empty()) return 0;
	std::unordered_map<uint64_t, BasicBlockData*> byId;
	for (auto& b: blocks)
		byId[b.id] = &b;
	// Assign larger loops first so inner (smaller) loops overwrite: innermost wins.
	std::vector<size_t> idx(loops.size());
	for (size_t i = 0; i < idx.size(); ++i)
		idx[i] = i;
	std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return loops[a].body.size() > loops[b].body.size(); });
	for (size_t i: idx)
	{
		for (uint64_t id: loops[i].body)
			if (auto it = byId.find(id); it != byId.end()) it->second->loopId = int(i);
		if (auto it = byId.find(loops[i].header); it != byId.end()) it->second->isLoopHeader = true;
	}
	return int(loops.size());
}

// ── Layout ───────────────────────────────────────────────────────────────────

namespace {

SceneNode makeBlockNode(const BasicBlockData& b, int maxPreview)
{
	SceneNode n;
	n.id = b.id;
	const bool chain = isChainSummary(b);
	n.kind = chain ? cfg::Chain : cfg::Block;
	n.rect = {0.f, 0.f, cfg::kBlockWidth, cfgBlockHeight(b, maxPreview)};
	n.radius = cfg::kRadius;
	n.fill = palette::surface0;
	n.hoverFill = palette::surface1;
	n.border = b.isLoopHeader ? palette::blue
		: b.isEntry           ? palette::green
		: b.isExit            ? palette::red
							  : palette::surface1;
	n.hoverBorder = (b.isLoopHeader || b.isEntry || b.isExit) ? 0 : palette::overlay2;
	n.borderWidth = 1.5f;
	n.dashedBorder = chain;
	n.headerFill = b.isLoopHeader ? withAlpha(palette::blue, 80)
		: b.isEntry               ? withAlpha(palette::green, 60)
		: b.isExit                ? withAlpha(palette::red, 60)
								  : palette::mantle;
	n.headerHeight = cfg::kHeaderH;
	n.tooltip = cfgBlockTooltip(b);
	n.searchText = hexAddress(b.address);
	for (const auto& ins: b.instrs)
	{
		n.searchText += '\n';
		n.searchText += ins.text;
	}
	return n;
}

void addBlockTexts(SceneNode& n, const BasicBlockData& b, int maxPreview)
{
	const GRect& r = n.rect;
	const float w = r.w - cfg::kPadX * 2.f;
	n.texts.push_back(SceneText{cfgBlockHeaderText(b), {r.x + cfg::kPadX, r.y, w, cfg::kHeaderH}, cfg::kTextSize,
		palette::subtext1, SceneAlign::Left, true, true});
	const int shown = std::min<int>(int(b.instrs.size()), maxPreview);
	for (int i = 0; i < shown; ++i)
		n.texts.push_back(SceneText{b.instrs[size_t(i)].text,
			{r.x + cfg::kPadX, r.y + cfg::kHeaderH + float(i) * cfg::kInstrH, w, cfg::kInstrH}, cfg::kTextSize,
			isChainSummary(b) ? palette::overlay2 : palette::text, SceneAlign::Left, false, true});
	if (int(b.instrs.size()) > maxPreview)
		n.texts.push_back(SceneText{std::format("  … {} more", int(b.instrs.size()) - maxPreview),
			{r.x + cfg::kPadX, r.y + cfg::kHeaderH + float(shown) * cfg::kInstrH, w, cfg::kInstrH}, cfg::kTextSize,
			palette::overlay0, SceneAlign::Left, false, true});
}

} // namespace

CfgLayout layoutCfg(const std::vector<BasicBlockData>& blocksIn, const std::vector<CFGEdgeData>& edgesIn,
	const CfgLayoutOptions& opt)
{
	CfgLayout out;
	if (blocksIn.empty()) return out;

	// 1. Loop annotation (data without loop ids) and chain compression.
	std::vector<BasicBlockData> blocks = blocksIn;
	if (opt.autoDetectLoops) annotateLoops(blocks, edgesIn);
	out.loops = detectNaturalLoops(blocks, edgesIn);
	std::vector<CFGEdgeData> edges = edgesIn;
	if (!opt.forceUncompressed && int(blocks.size()) > opt.compressThreshold)
	{
		out.compressed = true;
		CfgGraphData c = compressChains(blocks, edges, &out.chainMembers);
		blocks = std::move(c.blocks);
		edges = std::move(c.edges);
	}
	out.nodeCount = int(blocks.size());

	// 2. Layered layout.
	std::vector<uint64_t> ids;
	std::vector<GPoint> sizes;
	std::unordered_map<uint64_t, const BasicBlockData*> byId;
	ids.reserve(blocks.size());
	for (const auto& b: blocks)
	{
		if (byId.count(b.id)) continue; // duplicate ids: first wins
		byId[b.id] = &b;
		ids.push_back(b.id);
		sizes.push_back({cfg::kBlockWidth, cfgBlockHeight(b, opt.maxPreview)});
		out.addressOf[b.id] = b.address;
	}
	std::vector<EdgeRef> refs;
	for (const auto& e: edges)
		refs.push_back({e.from, e.to});
	out.entry = cfgEntryBlock(blocks, edges);
	LayeredOptions lo;
	lo.nodeGap = cfg::kNodeSpacingH - cfg::kBlockWidth;
	// Qt spaced level tops 140 px apart; for full (3-preview) blocks that is a 48 px gap.
	lo.rankGap = cfg::kLevelH - (cfg::kHeaderH + float(cfg::kMaxPreview) * cfg::kInstrH + cfg::kFooterH);
	lo.margin = cfg::kMargin;
	const LayeredLayout lay = layeredLayout(ids, sizes, refs, out.entry, lo);
	out.backEdges = lay.backEdges;

	// 3. Nodes.
	GraphScene& sc = out.scene;
	sc.background = palette::base;
	sc.nodes.reserve(ids.size());
	for (uint64_t id: ids)
	{
		const BasicBlockData& b = *byId[id];
		SceneNode n = makeBlockNode(b, opt.maxPreview);
		const auto p = lay.topLeft.find(id);
		n.rect.x = p != lay.topLeft.end() ? p->second.x : 0.f;
		n.rect.y = p != lay.topLeft.end() ? p->second.y : 0.f;
		addBlockTexts(n, b, opt.maxPreview);
		sc.nodes.push_back(std::move(n));
	}
	std::unordered_map<uint64_t, GRect> rectOf;
	for (const SceneNode& n: sc.nodes)
		rectOf[n.id] = n.rect;

	// 4. Edges: ports spread along the bottom / top edges, ordered by the other end's x.
	struct Routed
	{
		const CFGEdgeData* e;
		bool back;
	};
	std::vector<Routed> routed;
	for (const auto& e: edges)
	{
		if (!rectOf.count(e.from) || !rectOf.count(e.to)) continue;
		const bool back = lay.backEdges.count({e.from, e.to}) > 0 || e.kind == EdgeKind::BackEdge || e.from == e.to;
		routed.push_back({&e, back});
	}
	std::unordered_map<uint64_t, std::vector<size_t>> outPorts, inPorts;
	for (size_t i = 0; i < routed.size(); ++i)
	{
		if (routed[i].back) continue;
		outPorts[routed[i].e->from].push_back(i);
		inPorts[routed[i].e->to].push_back(i);
	}
	std::vector<float> fromX(routed.size()), toX(routed.size());
	for (size_t i = 0; i < routed.size(); ++i)
	{
		fromX[i] = rectOf[routed[i].e->from].center().x;
		toX[i] = rectOf[routed[i].e->to].center().x;
	}
	auto spread = [&](std::unordered_map<uint64_t, std::vector<size_t>>& ports, bool outgoing) {
		for (auto& [id, list]: ports)
		{
			const GRect r = rectOf[id];
			std::stable_sort(list.begin(), list.end(), [&](size_t a, size_t b) {
				const float ka = outgoing ? rectOf[routed[a].e->to].center().x : rectOf[routed[a].e->from].center().x;
				const float kb = outgoing ? rectOf[routed[b].e->to].center().x : rectOf[routed[b].e->from].center().x;
				if (ka != kb) return ka < kb;
				// true branch left of false branch when stacked
				return int(routed[a].e->kind == EdgeKind::FalseBranch) < int(routed[b].e->kind == EdgeKind::FalseBranch);
			});
			const float span = std::min(r.w * 0.6f, 36.f * float(list.size() - 1));
			for (size_t k = 0; k < list.size(); ++k)
			{
				const float x = list.size() == 1 ? r.center().x
												 : r.center().x - span * 0.5f + span * float(k) / float(list.size() - 1);
				(outgoing ? fromX : toX)[list[k]] = x;
			}
		}
	};
	spread(outPorts, true);
	spread(inPorts, false);

	sc.edges.reserve(routed.size());
	for (size_t i = 0; i < routed.size(); ++i)
	{
		const CFGEdgeData& e = *routed[i].e;
		const GRect fr = rectOf[e.from], tr = rectOf[e.to];
		SceneEdge se;
		se.from = e.from;
		se.to = e.to;
		const EdgeKind kind = routed[i].back ? EdgeKind::BackEdge : e.kind;
		se.kind = int(kind);
		se.color = cfgEdgeColor(kind);
		se.width = 1.6f;
		se.dashed = cfgEdgeDashed(kind);
		se.dash = 4.f;
		se.gap = 3.f;
		se.arrowLength = 10.f;
		se.arrowHalfWidth = 5.f;
		if (routed[i].back)
		{
			// Arc along the left of both blocks so it does not cross the forward edges.
			const GPoint from{fr.center().x, fr.bottom()}, to{tr.center().x, tr.y};
			const float leftX = std::min(fr.x, tr.x) - cfg::kBackEdgeArc;
			se.points = {from, {leftX, from.y + 30.f}, {leftX, to.y - 30.f}, to};
		}
		else
		{
			const GPoint from{fromX[i], fr.bottom()}, to{toX[i], tr.y};
			const float cy = (from.y + to.y) * 0.5f;
			se.points = {from, {from.x, cy}, {to.x, cy}, to};
		}
		sc.edges.push_back(std::move(se));
	}

	// 5. Loop regions, one per loop id, coloured from the Qt palette.
	if (opt.showLoopRegions)
	{
		static constexpr Rgba kLoopColors[] = {
			palette::blue, palette::mauve, palette::teal, palette::yellow, palette::peach, palette::green};
		std::map<int, GRect> loopRects;
		for (const auto& b: blocks)
		{
			if (b.loopId < 0) continue;
			const auto it = rectOf.find(b.id);
			if (it == rectOf.end()) continue;
			loopRects[b.loopId] = loopRects[b.loopId].united(it->second);
		}
		int ci = 0;
		for (const auto& [lid, r]: loopRects)
		{
			const Rgba c = kLoopColors[size_t(ci++) % std::size(kLoopColors)];
			SceneRegion reg;
			reg.rect = r.adjusted(-12.f, -12.f, 12.f, 12.f);
			reg.fill = withAlpha(c, 25);
			reg.border = withAlpha(c, 140);
			reg.borderWidth = 2.f;
			reg.radius = 4.f;
			reg.group = lid;
			sc.regions.push_back(reg);
		}
	}

	sc.finalize(cfg::kMargin);
	return out;
}

// ── DOT ──────────────────────────────────────────────────────────────────────

std::string cfgToDot(const std::vector<BasicBlockData>& blocks, const std::vector<CFGEdgeData>& edges, const std::string& title)
{
	std::string dot = std::format("digraph \"{}\" {{\n", dotEscape(title));
	dot += "  graph [bgcolor=\"#1e1e2e\" fontcolor=\"#cdd6f4\" fontname=\"Consolas\" splines=true];\n"
		   "  node [shape=box style=\"rounded,filled\" fillcolor=\"#313244\" color=\"#45475a\" fontcolor=\"#cdd6f4\""
		   " fontname=\"Consolas\" fontsize=10];\n"
		   "  edge [color=\"#a6adc8\"];\n\n";
	for (const auto& b: blocks)
	{
		std::string label = cfgBlockHeaderText(b) + "\n";
		for (const auto& ins: b.instrs)
			label += ins.text + "\n";
		std::string escaped = dotEscape(label);
		// left-justified lines: DOT "\l"
		std::string lj;
		for (size_t i = 0; i < escaped.size(); ++i)
		{
			if (escaped[i] == '\\' && i + 1 < escaped.size() && escaped[i + 1] == 'n')
			{
				lj += "\\l";
				++i;
			}
			else if (escaped[i] == '\\' && i + 1 < escaped.size())
			{
				lj += escaped[i];
				lj += escaped[++i];
			}
			else
				lj += escaped[i];
		}
		const char* color = b.isLoopHeader ? "#89b4fa" : b.isEntry ? "#a6e3a1" : b.isExit ? "#f38ba8" : "#45475a";
		dot += std::format("  \"0x{:x}\" [label=\"{}\" color=\"{}\"];\n", b.id, lj, color);
	}
	dot += "\n";
	for (const auto& e: edges)
	{
		dot += std::format("  \"0x{:x}\" -> \"0x{:x}\" [color=\"{}\"{}];\n", e.from, e.to, cssHex(cfgEdgeColor(e.kind)),
			cfgEdgeDashed(e.kind) ? " style=dashed" : "");
	}
	dot += "}\n";
	return dot;
}

} // namespace retdec::gui::model
