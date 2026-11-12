/**
 * @file src/gui/ui/progress_panel.cpp
 * @brief Progress panel (FuseUI).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/progress_panel.h"

#include "retdec/gui/model/funclist_export.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using fuse::ui::Color;
using fuse::ui::NodeHandle;
using fuse::ui::Rect;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;
using model::StageState;

namespace {

constexpr std::string_view kWaterfallClass = "rd-waterfall";
constexpr std::string_view kStageIconClass = "rd-stageicon";
constexpr float kWaterfallRowH = 14.f;
constexpr float kWaterfallLabelW = 120.f;

const char* stateColor(StageState s)
{
	switch (s)
	{
	case StageState::Pending: return "#6c7086";
	case StageState::Running: return "#89b4fa";
	case StageState::Done: return "#a6e3a1";
	case StageState::Error: return "#f38ba8";
	}
	return "#cdd6f4";
}

// Stage state icon drawn with the draw list (the bundled font has no
// ○ ◉ ✓ ✗ glyphs); the node text keeps the character for accessibility.
// The colour comes from the node's `color`.
struct StageIcon
{
	static constexpr std::string_view kName = kStageIconClass;
	static constexpr fuse::ui::A11yRole kRole = fuse::ui::A11yRole::Image;
	static fuse::ui::Size measure(UiContext&, NodeHandle, fuse::ui::Size) { return {16.f, 16.f}; }
	static void paint(const fuse::ui::PaintContext& pc, fuse::ui::DrawList& dl)
	{
		const std::string_view t = pc.ui.text(pc.node);
		const Color col = pc.tint(pc.style.color);
		const float cx = pc.content.x + pc.content.w * 0.5f, cy = pc.content.y + pc.content.h * 0.5f;
		const float r = 5.f;
		const Rect circle{cx - r, cy - r, 2.f * r, 2.f * r};
		if (t == model::stageStateIcon(StageState::Pending))
			dl.roundedRect(circle, {}, {r, r, r, r}, col, 1.2f);
		else if (t == model::stageStateIcon(StageState::Running))
		{
			dl.roundedRect(circle, {}, {r, r, r, r}, col, 1.2f);
			dl.rect({cx - 2.5f, cy - 2.5f, 5.f, 5.f}, col, 2.5f);
		}
		else if (t == model::stageStateIcon(StageState::Done))
		{
			dl.line({cx - 4.5f, cy}, {cx - 1.5f, cy + 3.5f}, col, 1.8f);
			dl.line({cx - 1.5f, cy + 3.5f}, {cx + 4.5f, cy - 4.f}, col, 1.8f);
		}
		else
		{
			dl.line({cx - 4.f, cy - 4.f}, {cx + 4.f, cy + 4.f}, col, 1.8f);
			dl.line({cx - 4.f, cy + 4.f}, {cx + 4.f, cy - 4.f}, col, 1.8f);
		}
	}
};

void setEditText(UiContext& ui, NodeHandle edit, std::string_view text)
{
	ui.editBuffer(edit).setText(text);
	ui.editBuffer(edit).end(false);
	ui.setText(edit, text);
}

} // namespace

// Reports from the bridge, queued for the UI thread.
struct ProgressPanel::Queue
{
	std::mutex mutex;
	std::deque<std::function<void(ProgressPanel&)>> items;
	bool posted = false;
	UiContext* ui = nullptr;
	NodeHandle root;
	ProgressPanel* panel = nullptr;

	void push(std::function<void(ProgressPanel&)> f)
	{
		std::lock_guard lock(mutex);
		items.push_back(std::move(f));
		if (posted) return;
		posted = ui->post(&ProgressPanel::drainCommand, root, reinterpret_cast<fuse::ui::u64>(panel), 0);
	}
};

// ─── Waterfall widget ────────────────────────────────────────────────────────

fuse::ui::Size ProgressPanel::measureWaterfall(UiContext& ui, NodeHandle n, fuse::ui::Size)
{
	const auto* p = reinterpret_cast<const ProgressPanel*>(static_cast<std::uintptr_t>(ui.userData(n)));
	const std::size_t bars = p ? p->tracker_.waterfall().size() : 0;
	return {200.f, std::max(80.f, static_cast<float>(bars) * (kWaterfallRowH + 2.f) + 4.f)};
}

void ProgressPanel::paintWaterfall(const fuse::ui::PaintContext& pc, fuse::ui::DrawList& dl)
{
	UiContext& ui = pc.ui;
	const auto* p = reinterpret_cast<const ProgressPanel*>(static_cast<std::uintptr_t>(ui.userData(pc.node)));
	if (!p) return;
	const Rect c = pc.content;
	const auto bars = p->tracker_.waterfall();
	const int64_t maxEnd = p->tracker_.waterfallEnd();
	dl.pushClip(c);
	if (bars.empty() || maxEnd <= 0 || c.w <= 1.f)
	{
		ui.paintString(
			dl,
			"No stage has finished yet",
			c,
			pc.tint(Color::rgb(0x6c7086)),
			12.f,
			fuse::ui::TextAlignment::Center,
			pc.node.index);
		dl.popClip();
		return;
	}
	const float barArea = std::max(40.f, c.w - kWaterfallLabelW - 8.f - 60.f);
	const float scale = barArea / static_cast<float>(maxEnd);
	for (std::size_t i = 0; i < bars.size(); ++i)
	{
		const auto& b = bars[i];
		const float y = c.y + static_cast<float>(i) * (kWaterfallRowH + 2.f) + 2.f;
		ui.paintString(
			dl,
			b.label,
			{c.x + 2.f, y, kWaterfallLabelW - 4.f, kWaterfallRowH},
			pc.tint(Color::rgb(0x9a9cab)),
			10.f,
			fuse::ui::TextAlignment::Start,
			pc.node.index);
		const float x = c.x + kWaterfallLabelW + static_cast<float>(b.startMs) * scale;
		const float bw = std::max(2.f, static_cast<float>(b.durationMs) * scale);
		dl.rect({x, y + 1.f, bw, kWaterfallRowH - 2.f}, pc.tint(Color::rgb(b.color)), 2.f);
		ui.paintString(
			dl,
			model::formatStageMs(b.durationMs),
			{x + bw + 3.f, y, 80.f, kWaterfallRowH},
			pc.tint(Color::rgb(0xcdd6f4)),
			10.f,
			fuse::ui::TextAlignment::Start,
			pc.node.index);
	}
	dl.popClip();
}

// ═════════════════════════════════════════════════════════════════════════════

ProgressPanel::ProgressPanel(UiContext& ui, NodeHandle parent): ui_(ui)
{
	build(parent);
	queue_ = std::make_shared<Queue>();
	queue_->ui = &ui_;
	queue_->root = root_;
	queue_->panel = this;
	syncRows();
	syncSummary();
}

ProgressPanel::~ProgressPanel()
{
	subscription_.reset(); // no report reaches the queue after this
	if (tickRunning_) ui_.cancelTimer(tickTimer_);
	if (busyRunning_) ui_.cancelTimer(busyTimer_);
	if (ui_.alive(waterfall_)) ui_.setUserData(waterfall_, 0);
	if (ui_.alive(fileDlg_)) ui_.destroy(ui_.parent(fileDlg_));
	for (NodeHandle t: tooltips_)
		if (ui_.alive(t)) ui_.destroy(t);
	for (const auto& r: rows_)
		if (ui_.alive(r.errorTip)) ui_.destroy(r.errorTip);
	if (ui_.alive(root_)) ui_.destroy(root_); // posted drains check the root
}

void ProgressPanel::tooltip(NodeHandle n, std::string_view text)
{
	tooltips_.push_back(w::setTooltip(ui_, n, text));
}

void ProgressPanel::build(NodeHandle parent)
{
	if (ui_.registry().find("lineedit") == 0xFFFFFFFFu) w::registerTierA(ui_);
	w::registerTierB(ui_);
	if (ui_.registry().find(kWaterfallClass) == 0xFFFFFFFFu)
	{
		fuse::ui::WidgetClass cls;
		cls.name = std::string(kWaterfallClass);
		cls.role = fuse::ui::A11yRole::Image;
		cls.measure = &ProgressPanel::measureWaterfall;
		cls.paint = &ProgressPanel::paintWaterfall;
		ui_.registry().add(std::move(cls));
	}
	if (ui_.registry().find(kStageIconClass) == 0xFFFFFFFFu) ui_.registry().registerWidget<StageIcon>();

	root_ = w::panel(ui_, parent);
	ui_.addClass(root_, "rd-progress");
	ui_.setA11y(root_, fuse::ui::A11yRole::Group, kTitle);
	ui_.setCss(root_, "flex-grow", "1");
	ui_.setCss(root_, "padding", "0px");
	ui_.setCss(root_, "gap", "0px");

	auto frame = [&](NodeHandle n) {
		ui_.setCss(n, "background", "#2a2b32");
		ui_.setCss(n, "border-width", "1px");
		ui_.setCss(n, "border-color", "#474955");
		ui_.setCss(n, "border-radius", "4px");
	};

	// ── summary header ──────────────────────────────────────────────────────
	const NodeHandle summary = w::panel(ui_, root_);
	frame(summary);
	ui_.setCss(summary, "flex-direction", "row");
	ui_.setCss(summary, "align-items", "center");
	ui_.setCss(summary, "gap", "12px");
	ui_.setCss(summary, "padding", "6px 8px");
	ui_.setCss(summary, "flex-shrink", "0");
	overallBar_ = w::progressBar(ui_, summary, 0.f);
	ui_.setA11y(overallBar_, fuse::ui::A11yRole::ProgressBar, "Overall progress");
	ui_.setCss(overallBar_, "flex-grow", "3");
	ui_.setCss(overallBar_, "height", "12px");
	ui_.setCss(overallBar_, "border-radius", "6px");
	pctLabel_ = w::label(ui_, summary, "0%");
	ui_.setCss(pctLabel_, "width", "40px");
	ui_.setCss(pctLabel_, "text-align", "end");
	auto stat = [&](std::string_view icon, std::string_view text, std::string_view tip) {
		const NodeHandle i = w::label(ui_, summary, icon);
		ui_.setCss(i, "color", "#9a9cab");
		const NodeHandle l = w::label(ui_, summary, text);
		ui_.setCss(l, "flex-shrink", "0");
		tooltip(l, tip);
		return l;
	};
	fnCountLabel_ = stat("\xC6\x92", "0 fn", "Functions recovered");
	instrLabel_ = stat("#", "0 instr", "Instructions analysed");
	elapsedLabel_ = stat("\xCE\x94t", "0.0s", "Elapsed wall time");
	elapsedTipNode_ = tooltips_.back();
	ui_.setCss(elapsedLabel_, "width", "56px");

	// ── stages + timeline (stacked splitter) ────────────────────────────────
	const NodeHandle split = w::splitter(ui_, root_, true);
	ui_.setCss(split, "flex-grow", "1");
	const NodeHandle stagesPane = w::splitterPane(ui_, split);
	ui_.setCss(stagesPane, "flex-grow", "3");
	stagesBox_ = w::scrollArea(ui_, stagesPane);
	ui_.setCss(stagesBox_, "flex-grow", "1");
	ui_.setCss(stagesBox_, "padding", "4px");
	ui_.setCss(stagesBox_, "gap", "4px");

	const NodeHandle wfPane = w::splitterPane(ui_, split, 150.f);
	ui_.setCss(wfPane, "padding", "4px 4px 2px 4px");
	ui_.setCss(wfPane, "gap", "2px");
	const NodeHandle wfTitle = w::label(ui_, wfPane, "Stage Timeline");
	ui_.setCss(wfTitle, "color", "#9a9cab");
	ui_.setCss(wfTitle, "font-size", "12px");
	waterfall_ = ui_.create(ui_.registry().find(kWaterfallClass), wfPane);
	ui_.setUserData(waterfall_, static_cast<fuse::ui::u64>(reinterpret_cast<std::uintptr_t>(this)));
	ui_.setA11y(waterfall_, fuse::ui::A11yRole::Image, "Stage timeline");
	ui_.setCss(waterfall_, "flex-grow", "1");
	ui_.setCss(waterfall_, "background", "#1e1e2e");
	ui_.setCss(waterfall_, "border-radius", "4px");
	ui_.setCss(waterfall_, "overflow", "clip");

	// ── throughput ──────────────────────────────────────────────────────────
	const NodeHandle tp = w::panel(ui_, root_);
	frame(tp);
	ui_.setCss(tp, "flex-direction", "row");
	ui_.setCss(tp, "align-items", "center");
	ui_.setCss(tp, "gap", "16px");
	ui_.setCss(tp, "padding", "4px 8px");
	ui_.setCss(tp, "flex-shrink", "0");
	const NodeHandle tpTitle = w::label(ui_, tp, "Throughput:");
	ui_.setCss(tpTitle, "color", "#9a9cab");
	const NodeHandle fnL = w::label(ui_, tp, "Functions/s:");
	ui_.addClass(fnL, "dim");
	fnRateLabel_ = w::label(ui_, tp, "\xE2\x80\x94");
	ui_.setCss(fnRateLabel_, "color", "#89b4fa");
	const NodeHandle sp = w::panel(ui_, tp);
	ui_.setCss(sp, "flex-grow", "1");
	const NodeHandle inL = w::label(ui_, tp, "Instructions/s:");
	ui_.addClass(inL, "dim");
	instrRateLabel_ = w::label(ui_, tp, "\xE2\x80\x94");
	ui_.setCss(instrRateLabel_, "color", "#cba6f7");

	exportBtn_ = w::button(ui_, root_, "Export JSON");
	ui_.setCss(exportBtn_, "padding", "4px 10px");
	ui_.setCss(exportBtn_, "flex-shrink", "0");
	tooltip(exportBtn_, "Copy analysis stats as JSON to clipboard");
	ui_.connect(exportBtn_, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { exportClicked(); });
}

// ─── Rows ────────────────────────────────────────────────────────────────────

void ProgressPanel::addRowNodes(const std::string& name)
{
	Row r;
	r.name = name;
	StageRowNodes& n = r.nodes;
	n.row = w::panel(ui_, stagesBox_);
	ui_.setCss(n.row, "flex-direction", "row");
	ui_.setCss(n.row, "align-items", "center");
	ui_.setCss(n.row, "gap", "6px");
	ui_.setCss(n.row, "padding", "1px 2px");
	ui_.setCss(n.row, "flex-shrink", "0");
	n.icon = ui_.create(ui_.registry().find(kStageIconClass), n.row);
	ui_.setText(n.icon, model::stageStateIcon(StageState::Pending));
	ui_.setA11y(n.icon, fuse::ui::A11yRole::Image, model::stageStateName(StageState::Pending));
	n.name = w::label(ui_, n.row, name);
	ui_.setCss(n.name, "width", "172px");
	n.bar = w::progressBar(ui_, n.row, 0.f);
	ui_.setA11y(n.bar, fuse::ui::A11yRole::ProgressBar, name);
	ui_.setCss(n.bar, "flex-grow", "1");
	ui_.setCss(n.bar, "height", "6px");
	ui_.setCss(n.bar, "border-radius", "3px");
	n.time = w::label(ui_, n.row, "");
	ui_.setCss(n.time, "width", "64px");
	ui_.setCss(n.time, "text-align", "end");
	ui_.setCss(n.time, "color", "#9a9cab");
	ui_.setCss(n.time, "font-size", "12px");
	rows_.push_back(std::move(r));
}

void ProgressPanel::syncRows()
{
	const auto& stages = tracker_.stages();
	// Rows only grow (resetAll keeps the default stages, which come first).
	while (rows_.size() > stages.size())
	{
		ui_.destroy(rows_.back().nodes.row);
		if (ui_.alive(rows_.back().errorTip)) ui_.destroy(rows_.back().errorTip);
		rows_.pop_back();
	}
	for (std::size_t i = rows_.size(); i < stages.size(); ++i)
		addRowNodes(stages[i].name);
	for (std::size_t i = 0; i < stages.size(); ++i)
		syncRow(i);
	updateBusyTimer();
}

void ProgressPanel::syncRow(std::size_t i)
{
	const auto& s = tracker_.stages()[i];
	const StageRowNodes& n = rows_[i].nodes;
	ui_.setText(n.icon, model::stageStateIcon(s.state));
	ui_.setA11y(n.icon, fuse::ui::A11yRole::Image, model::stageStateName(s.state));
	ui_.setCss(n.icon, "color", stateColor(s.state));
	ui_.setCss(n.name, "color", s.state == StageState::Pending ? "#9a9cab" : s.state == StageState::Error ? "#f38ba8" : "#e6e6eb");
	ui_.setCss(n.bar, "background-end", s.state == StageState::Error ? "#f38ba8" : s.state == StageState::Done ? "#a6e3a1" : "#89b4fa");
	if (s.percent >= 0) ui_.setValue(n.bar, static_cast<float>(s.percent) / 100.f);
	ui_.setText(n.time, model::formatStageMs(s.elapsedMs));
	Row& row = rows_[i];
	if (s.state == StageState::Error && !s.message.empty())
	{
		if (ui_.alive(row.errorTip))
			ui_.setText(row.errorTip, s.message);
		else
			row.errorTip = w::setTooltip(ui_, n.time, s.message);
	}
	else if (ui_.alive(row.errorTip))
	{
		ui_.destroy(row.errorTip); // its hover handler checks alive()
		row.errorTip = {};
	}
}

void ProgressPanel::updateBusyTimer()
{
	bool busy = false;
	for (const auto& s: tracker_.stages())
		busy |= s.state == StageState::Running && s.percent < 0;
	if (busy && !busyRunning_)
	{
		busyRunning_ = true;
		busyTimer_ = ui_.startTimer(0.05f, true, [this](UiContext&) {
			busyPhase_ = std::fmod(busyPhase_ + 0.04f, 1.f);
			for (std::size_t i = 0; i < rows_.size(); ++i)
			{
				const auto& s = tracker_.stages()[i];
				if (s.state == StageState::Running && s.percent < 0) ui_.setValue(rows_[i].nodes.bar, busyPhase_);
			}
		});
	}
	else if (!busy && busyRunning_)
	{
		busyRunning_ = false;
		ui_.cancelTimer(busyTimer_);
	}
}

void ProgressPanel::syncSummary()
{
	const int pct = tracker_.overallPercent();
	ui_.setValue(overallBar_, static_cast<float>(pct) / 100.f);
	ui_.setText(pctLabel_, std::format("{}%", pct));
	ui_.setText(fnCountLabel_, std::format("{} fn", tracker_.functionCount()));
	ui_.setText(instrLabel_, model::formatInstructionCount(tracker_.instructionCount()));
	ui_.setText(fnRateLabel_, tracker_.functionRate() ? model::formatFunctionRate(*tracker_.functionRate()) : "\xE2\x80\x94");
	ui_.setText(
		instrRateLabel_,
		tracker_.instructionRate() ? model::formatInstructionRate(*tracker_.instructionRate()) : "\xE2\x80\x94");
	// The timeline grows with its bar count.
	const std::size_t bars = tracker_.waterfall().size();
	ui_.setCss(waterfall_, "min-height", std::format("{}px", std::max(80.f, static_cast<float>(bars) * (kWaterfallRowH + 2.f) + 4.f)));
	ui_.invalidate(waterfall_);
}

void ProgressPanel::syncElapsed()
{
	const int64_t ms = tracker_.elapsedMs();
	ui_.setText(elapsedLabel_, tracker_.started() && ms > 0 ? model::formatStageMs(ms) : "0.0s");
	budget_ = tracker_.running() ? model::budgetLevel(ms, budgetSecs_) : model::BudgetLevel::Normal;
	elapsedTip_ = model::budgetToolTip(budget_, budgetSecs_);
	ui_.setText(elapsedTipNode_, elapsedTip_.empty() ? std::string("Elapsed wall time") : elapsedTip_);
	switch (budget_)
	{
	case model::BudgetLevel::Exceeded: ui_.setCss(elapsedLabel_, "color", "#e64553"); break;
	case model::BudgetLevel::Warning: ui_.setCss(elapsedLabel_, "color", "#df8e1d"); break;
	default: ui_.setCss(elapsedLabel_, "color", "#e6e6eb"); break;
	}
}

// ─── Direct API ──────────────────────────────────────────────────────────────

void ProgressPanel::setStageState(std::string_view stageName, StageState state, int percent)
{
	tracker_.setStageState(stageName, state, percent);
	syncRows();
	syncSummary();
}

void ProgressPanel::setStageElapsed(std::string_view stageName, int64_t ms)
{
	if (!tracker_.setStageElapsed(stageName, ms)) return;
	syncRows();
	syncSummary();
}

void ProgressPanel::setFunctionCount(int count)
{
	tracker_.setFunctionCount(count);
	syncSummary();
}

void ProgressPanel::setInstructionCount(int64_t count)
{
	tracker_.setInstructionCount(count);
	syncSummary();
}

void ProgressPanel::setThroughput(double fnPerSec, double instrPerSec)
{
	tracker_.setThroughput(fnPerSec, instrPerSec);
	syncSummary();
}

void ProgressPanel::setOverallProgress(int percent)
{
	tracker_.setOverallProgress(percent);
	syncSummary();
	ui_.setText(pctLabel_, std::format("{}%", percent));
}

void ProgressPanel::setAnalysisBudgetSecs(int secs)
{
	budgetSecs_ = std::max(0, secs);
	syncElapsed();
}

void ProgressPanel::resetAll()
{
	tracker_.reset();
	if (tickRunning_)
	{
		ui_.cancelTimer(tickTimer_);
		tickRunning_ = false;
	}
	syncRows();
	syncSummary();
	syncElapsed();
}

// ─── Pipeline events ─────────────────────────────────────────────────────────

void ProgressPanel::stageStarted(std::string_view stageName)
{
	tracker_.stageStarted(stageName);
	syncRows();
	syncSummary();
	if (onStageStarted) onStageStarted(std::string(stageName));
}

void ProgressPanel::stageProgress(std::string_view stageName, int percent)
{
	tracker_.stageProgress(stageName, percent);
	syncRows();
	syncSummary();
}

void ProgressPanel::stageCompleted(std::string_view stageName, int64_t elapsedMs)
{
	tracker_.stageCompleted(stageName, elapsedMs);
	syncRows();
	syncSummary();
	if (onStageCompleted) onStageCompleted(std::string(stageName), elapsedMs);
}

void ProgressPanel::stageErrored(std::string_view stageName, std::string_view message)
{
	tracker_.stageErrored(stageName, message);
	syncRows();
	syncSummary();
}

void ProgressPanel::analysisStarted()
{
	tracker_.analysisStarted();
	if (!tickRunning_)
	{
		tickRunning_ = true;
		tickTimer_ = ui_.startTimer(1.f, true, [this](UiContext&) { tick(); });
	}
	syncElapsed();
}

void ProgressPanel::tick()
{
	if (!tracker_.running()) return;
	syncElapsed();
}

void ProgressPanel::analysisCompleted(int64_t totalMs)
{
	tracker_.analysisCompleted(totalMs);
	if (tickRunning_)
	{
		ui_.cancelTimer(tickTimer_);
		tickRunning_ = false;
	}
	syncSummary();
	ui_.setText(pctLabel_, "100%");
	syncElapsed();
	if (onAnalysisCompleted) onAnalysisCompleted(totalMs);
}

void ProgressPanel::analysisCancelled()
{
	tracker_.analysisCancelled();
	if (tickRunning_)
	{
		ui_.cancelTimer(tickTimer_);
		tickRunning_ = false;
	}
	syncRows();
	syncSummary();
	syncElapsed();
}

// ─── Bridge ──────────────────────────────────────────────────────────────────

void ProgressPanel::connectBridge(model::AnalysisBridge* bridge)
{
	subscription_.reset();
	if (!bridge) return;
	model::AnalysisBridge::Listener l;
	std::weak_ptr<Queue> wq = queue_;
	auto push = [wq](std::function<void(ProgressPanel&)> f) {
		if (auto q = wq.lock()) q->push(std::move(f));
	};
	l.stageStarted = [push](const std::string& s) { push([s](ProgressPanel& p) { p.stageStarted(s); }); };
	l.progressUpdated = [push](const std::string& s, int pct) { push([s, pct](ProgressPanel& p) { p.stageProgress(s, pct); }); };
	l.stageCompleted = [push](const std::string& s, int64_t ms) {
		push([s, ms](ProgressPanel& p) { p.stageCompleted(s, ms); });
	};
	l.stageErrored = [push](const std::string& s, const std::string& m) {
		push([s, m](ProgressPanel& p) { p.stageErrored(s, m); });
	};
	l.functionCountChanged = [push](int c) { push([c](ProgressPanel& p) { p.setFunctionCount(c); }); };
	l.instructionCountChanged = [push](int64_t c) { push([c](ProgressPanel& p) { p.setInstructionCount(c); }); };
	l.throughputChanged = [push](double f, double i) { push([f, i](ProgressPanel& p) { p.setThroughput(f, i); }); };
	l.analysisStarted = [push] { push([](ProgressPanel& p) { p.analysisStarted(); }); };
	l.analysisCompleted = [push](int64_t ms) { push([ms](ProgressPanel& p) { p.analysisCompleted(ms); }); };
	l.analysisCancelled = [push] { push([](ProgressPanel& p) { p.analysisCancelled(); }); };
	subscription_ = bridge->subscribe(std::move(l));
}

void ProgressPanel::drainCommand(UiContext& ui, NodeHandle n, fuse::ui::u64 a, fuse::ui::u64)
{
	if (!ui.alive(n)) return; // the panel is gone
	reinterpret_cast<ProgressPanel*>(a)->drainQueue();
}

void ProgressPanel::drainQueue()
{
	std::deque<std::function<void(ProgressPanel&)>> items;
	{
		std::lock_guard lock(queue_->mutex);
		items.swap(queue_->items);
		queue_->posted = false;
	}
	for (auto& f: items)
		f(*this);
}

// ─── Export ──────────────────────────────────────────────────────────────────

std::expected<void, std::string> ProgressPanel::exportJsonTo(const std::string& path) const
{
	return model::funclistWriteTextFile(path, exportJson());
}

void ProgressPanel::exportClicked()
{
	const std::string json = exportJson();
	if (clipboardWriter)
		clipboardWriter(json);
	else
		ui_.setClipboardText(json);
	if (onExportRequested) onExportRequested();

	if (ui_.alive(fileDlg_)) ui_.destroy(ui_.parent(fileDlg_));
	std::error_code ec;
	fileDlg_ = w::fileDialog(ui_, "Save progress", std::filesystem::current_path(ec).generic_string(), "*.json", true);
	const NodeHandle body = w::dialogBody(ui_, fileDlg_);
	NodeHandle row = ui_.firstChild(body);
	for (int i = 0; i < 2 && ui_.alive(row); ++i)
		row = ui_.nextSibling(row);
	if (ui_.alive(row))
	{
		const NodeHandle nameEdit = ui_.nextSibling(ui_.firstChild(row));
		if (ui_.alive(nameEdit)) setEditText(ui_, nameEdit, "progress.json");
	}
	ui_.connect(fileDlg_, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
		if (!a.text.empty()) (void)exportJsonTo(std::string(a.text));
	});
	w::openDialog(ui_, fileDlg_);
}

// ─── Test hooks ──────────────────────────────────────────────────────────────

ProgressPanel::StageRowNodes ProgressPanel::stageRow(std::string_view name) const
{
	for (const auto& r: rows_)
		if (r.name == name) return r.nodes;
	return {};
}

} // namespace retdec::gui::ui
