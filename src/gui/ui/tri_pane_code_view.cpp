/**
 * @file src/gui/ui/tri_pane_code_view.cpp
 * @brief CodeView widget class, SyncedCodePane and TriPaneCodeView.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/ui/tri_pane_code_view.h"

#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>

namespace retdec::gui::ui {

namespace w = fuse::ui::widgets;
using namespace fuse::ui;
using model::TextPos;
using model::TextRange;

namespace {

std::size_t prevCp(std::string_view s, std::size_t c)
{
	if (!c) return 0;
	--c;
	while (c && (static_cast<unsigned char>(s[c]) & 0xC0) == 0x80)
		--c;
	return c;
}

std::size_t nextCp(std::string_view s, std::size_t c)
{
	if (c >= s.size()) return s.size();
	++c;
	while (c < s.size() && (static_cast<unsigned char>(s[c]) & 0xC0) == 0x80)
		++c;
	return c;
}

constexpr float kBar = 8.f;

} // namespace

CodePalette defaultCodePalette()
{
	CodePalette p;
	p.background = Color::rgb(0x1e1e2e);
	p.gutter = Color::rgb(0x181825);
	p.gutterText = Color::rgb(0x6c7086);
	p.gutterTextActive = Color::rgb(0x89b4fa);
	p.text = Color::rgb(0xcdd6f4);
	p.placeholder = Color::rgb(0x6c7086);
	p.currentLine = Color::rgb(0xffffff, 0.04f);
	p.highlightLine = Color::rgb(0x45475a, 160.f / 255.f);
	p.selection = Color::rgb(0x264f78);
	p.caret = Color::rgb(0xf5e0dc);
	p.scrollThumb = Color::rgb(0x8a8a96, 0.85f);
	using TK = model::TokenKind;
	auto set = [&](TK k, Color c) { p.tokens[static_cast<std::size_t>(k)] = c; };
	set(TK::Text, Color::rgb(0xcdd6f4));
	set(TK::Keyword, Color::rgb(0x89b4fa));      // blue
	set(TK::Type, Color::rgb(0xf9e2af));         // yellow
	set(TK::String, Color::rgb(0xa6e3a1));       // green
	set(TK::Number, Color::rgb(0xfab387));       // peach
	set(TK::Comment, Color::rgb(0x6c7086));      // overlay0
	set(TK::Preprocessor, Color::rgb(0xf38ba8)); // red
	set(TK::Register, Color::rgb(0xcba6f7));     // mauve
	set(TK::Mnemonic, Color::rgb(0x89b4fa));     // blue
	set(TK::Address, Color::rgb(0x6c7086));      // overlay0
	set(TK::Label, Color::rgb(0xf9e2af));        // yellow
	set(TK::SsaName, Color::rgb(0xcba6f7));      // mauve
	set(TK::Builtin, Color::rgb(0x94e2d5));      // teal
	p.console = {Color::rgb(0xcdd6f4), Color::rgb(0xf9e2af), Color::rgb(0xf38ba8), Color::rgb(0x89b4fa), Color::rgb(0xa6e3a1)};
	p.boldTokens[static_cast<std::size_t>(TK::Keyword)] = true;
	p.boldTokens[static_cast<std::size_t>(TK::Mnemonic)] = true;
	p.boldConsole[3] = p.boldConsole[4] = true; // banner, footer
	return p;
}

// ── Widget class glue ────────────────────────────────────────────────────────

struct CodeView::Glue
{
	static constexpr std::string_view kName = "retdec-codeview";
	static constexpr A11yRole kRole = A11yRole::TextField;
	static constexpr bool kFocusable = true;
	static constexpr bool kInteractive = true;

	static ComputedStyle defaults()
	{
		ComputedStyle s;
		s.grow = 1.f;
		s.shrink = 1.f;
		s.fontSize = 13.f;
		// Code, assembly, hex and console output: monospace, so columns line up.
		s.fontFamilyCount = u8(parseFontFamilies(kMonoFamily, s.fontFamily));
		s.minHeight = Length::px(0.f);
		s.minWidth = Length::px(0.f);
		return s;
	}

	static CodeView* self(const UiContext& ui, NodeHandle n)
	{
		return reinterpret_cast<CodeView*>(static_cast<std::uintptr_t>(ui.userData(n)));
	}

	static Size measure(UiContext&, NodeHandle, Size) { return {200.f, 120.f}; }

	static bool barGeom(const CodeView& v, Rect c, Rect& track, Rect& thumb)
	{
		const int count = v.lineCount(), vis = v.visibleLineCount();
		if (count <= vis) return false;
		track = {c.right() - kBar, c.y, kBar, c.h};
		const float tl = std::max(16.f, track.h * float(vis) / float(count));
		const int mx = std::max(1, v.scrollMax());
		thumb = {track.x, track.y + (track.h - tl) * float(v.first_) / float(mx), kBar, tl};
		return true;
	}

	static void paint(const PaintContext& pc, DrawList& dl)
	{
		CodeView* v = self(pc.ui, pc.node);
		if (!v) return;
		UiContext& ui = pc.ui;
		const CodePalette& pal = v->palette_;
		const Rect c = pc.content;
		const float lh = std::ceil(pc.style.fontSize * 1.35f), fs = pc.style.fontSize;
		if (v->pendingCenter_ >= 0 && c.h > 0.f)
		{
			const int line = v->pendingCenter_;
			v->pendingCenter_ = -1;
			v->first_ = std::clamp(line - v->visibleLineCount() / 2, 0, v->scrollMax());
		}
		v->first_ = std::clamp(v->first_, 0, v->scrollMax());
		dl.pushClip(c);
		dl.rect(c, pc.tint(pal.background));
		const float gw = v->gutterWidth();
		const bool focused = ui.focused() == pc.node;
		const int count = v->lineCount();
		const int vis = v->visibleLineCount() + 1;
		if (gw > 0.f) dl.rect({c.x, c.y, gw - 4.f, c.h}, pc.tint(pal.gutter));

		// Line backgrounds and gutter numbers.
		for (int r = 0; r < vis && v->first_ + r < count; ++r)
		{
			const int li = v->first_ + r;
			const float y = c.y + float(r) * lh;
			if (li == v->highlight_)
				dl.rect({c.x + gw, y, c.w - gw, lh}, pc.tint(pal.highlightLine));
			else if (focused && li == v->caret_.line)
				dl.rect({c.x + gw, y, c.w - gw, lh}, pc.tint(pal.currentLine));
			if (gw > 0.f)
			{
				char num[24];
				std::snprintf(num, sizeof num, "%d", li + 1);
				const bool active = li == v->highlight_ || (focused && li == v->caret_.line);
				ui.paintString(dl, num, {c.x, y, gw - 10.f, lh}, pc.tint(active ? pal.gutterTextActive : pal.gutterText), fs * 0.9f,
				               TextAlignment::End, pc.node.index);
			}
		}

		// Text area.
		const Rect ta{c.x + gw, c.y, std::max(0.f, c.w - gw), c.h};
		dl.pushClip(ta);
		const float x0 = ta.x + 2.f - v->scrollX_;
		const TextPos a = std::min(v->caret_, v->anchor_), b = std::max(v->caret_, v->anchor_);
		const bool hasSel = !(a == b);
		const fuse::text::Font* boldFace = styledFont(ui, pc.node, false, true);
		if (v->isEmpty() && !v->placeholder_.empty())
			ui.paintString(dl, v->placeholder_, {ta.x + 4.f, c.y, ta.w, lh}, pc.tint(pal.placeholder), fs, TextAlignment::Start, pc.node.index);
		for (int r = 0; r < vis && v->first_ + r < count; ++r)
		{
			const int li = v->first_ + r;
			const std::string_view L = v->line(li);
			const float y = c.y + float(r) * lh;
			if (hasSel && li >= a.line && li <= b.line)
			{
				const std::size_t sc = li == a.line ? std::min<std::size_t>(std::size_t(a.col), L.size()) : 0;
				const std::size_t ec = li == b.line ? std::min<std::size_t>(std::size_t(b.col), L.size()) : L.size();
				const float sx = x0 + ui.measureString(L.substr(0, sc), fs, pc.node);
				const float ex = x0 + ui.measureString(L.substr(0, ec), fs, pc.node) + (li != b.line ? 6.f : 0.f);
				dl.rect({sx, y, std::max(0.f, ex - sx), lh}, pc.tint(pal.selection));
			}
			// Bold runs use the bold face of the view's (monospace) family: the same
			// advances, so columns still line up.
			auto seg = [&](std::size_t from, std::size_t to, Color col, bool bold = false) {
				if (to <= from) return true;
				const float x = x0 + ui.measureString(L.substr(0, from), fs, pc.node);
				if (x > ta.right()) return false;
				const std::string_view part = L.substr(from, to - from);
				ui.paintString(dl, part, {x, y, ta.right() - x + 2000.f, lh}, pc.tint(col), fs, TextAlignment::Start, pc.node.index,
				               bold ? boldFace : nullptr);
				return true;
			};
			if (v->console_)
			{
				const std::size_t k = std::min<std::size_t>(4, std::size_t(v->console_->kind(std::size_t(li))));
				seg(0, L.size(), pal.console[k], pal.boldConsole[k]);
			}
			else
			{
				const auto& spans = v->tokens(li);
				std::size_t at = 0;
				for (const model::TokenSpan& s: spans)
				{
					if (!seg(at, s.start, pal.text)) break;
					if (!seg(s.start, s.start + s.length, pal.tokens[std::size_t(s.kind)], pal.boldTokens[std::size_t(s.kind)])) break;
					at = s.start + s.length;
				}
				seg(at, L.size(), pal.text);
			}
			if (focused && li == v->caret_.line)
			{
				const float cx = x0 + ui.measureString(L.substr(0, std::min<std::size_t>(std::size_t(v->caret_.col), L.size())), fs, pc.node);
				dl.rect({cx, y + 2.f, 1.5f, lh - 4.f}, pc.tint(pal.caret));
				ui.setTextCaret(pc.node, {cx, y + 2.f, 1.5f, lh - 4.f});
			}
		}
		dl.popClip();

		Rect tr, th;
		if (barGeom(*v, c, tr, th))
		{
			dl.rect(tr, pc.tint(Color::rgb(0x000000, 0.15f)), kBar * 0.5f);
			dl.rect(th, pc.tint(pal.scrollThumb), kBar * 0.5f);
		}
		dl.popClip();
	}

	static bool onEvent(EventContext& ec, const Event& e)
	{
		if (ec.phase != Phase::Target) return false;
		CodeView* v = self(ec.ui, ec.node);
		if (!v) return false;
		UiContext& ui = ec.ui;
		const Rect c = ui.contentRect(ec.node);
		const bool shift = e.mods & kModShift, ctrl = e.mods & kModCtrl;
		switch (e.type)
		{
		case EventType::PointerDown: {
			if (e.button == MouseButton::Right)
			{
				ui.setFocus(ec.node, FocusReason::Pointer);
				const TextPos p = v->hitTest(e.pos);
				const TextPos a = std::min(v->caret_, v->anchor_), b = std::max(v->caret_, v->anchor_);
				const bool inSel = !(a == b) && !(p < a) && p < b;
				if (!inSel)
				{
					const int old = v->caret_.line;
					v->caret_ = v->anchor_ = p;
					v->caretMoved(old, false);
				}
				const bool below = e.pos.y >= c.y + float(v->lineCount() - v->first_) * v->lineHeight();
				if (v->onContextMenu) v->onContextMenu(e.pos, below ? -1 : p.line);
				return true;
			}
			if (e.button != MouseButton::Left) return false;
			Rect tr, th;
			if (barGeom(*v, c, tr, th) && tr.contains(e.pos))
			{
				v->dragScroll_ = true;
				v->grab_ = th.contains(e.pos) ? e.pos.y - th.y : th.h * 0.5f;
				Event mv = e;
				mv.type = EventType::PointerMove;
				return onEvent(ec, mv);
			}
			const TextPos p = v->hitTest(e.pos);
			const int old = v->caret_.line;
			v->caret_ = p;
			if (!shift) v->anchor_ = p;
			v->dragging_ = true;
			// FuseUI's click counting (UiContext double-click settings).
			const bool dbl = e.clicks == 2 && v->lastClickLine_ == p.line;
			v->lastClickLine_ = p.line;
			v->invalidate();
			// A click always activates its line (even the current one).
			if (v->onCaretLineChanged) v->onCaretLineChanged(p.line);
			(void)old;
			if (dbl && v->onDoubleClicked) v->onDoubleClicked(p.line);
			return true;
		}
		case EventType::PointerMove: {
			if (v->dragScroll_)
			{
				Rect tr, th;
				if (!barGeom(*v, c, tr, th)) return true;
				const float f = std::clamp((e.pos.y - tr.y - v->grab_) / std::max(1.f, tr.h - th.h), 0.f, 1.f);
				v->setFirstVisibleLine(int(std::lround(f * float(v->scrollMax()))), true);
				return true;
			}
			if (!v->dragging_) return false;
			if (e.pos.y < c.y) v->setFirstVisibleLine(v->first_ - 1, true);
			else if (e.pos.y > c.bottom()) v->setFirstVisibleLine(v->first_ + 1, true);
			const int old = v->caret_.line;
			v->caret_ = v->hitTest(e.pos);
			v->caretMoved(old, true);
			return true;
		}
		case EventType::PointerUp:
			v->dragging_ = false;
			if (v->dragScroll_) v->invalidate();
			v->dragScroll_ = false;
			return true;
		case EventType::Wheel: {
			if (shift || e.wheel.x != 0.f)
			{
				const float d = e.wheel.x != 0.f ? e.wheel.x : e.wheel.y;
				v->scrollX_ = std::clamp(v->scrollX_ - d * 40.f, 0.f, 200000.f);
				v->invalidate();
				return true;
			}
			v->setFirstVisibleLine(v->first_ - int(std::lround(e.wheel.y * 3.f)), true);
			return true;
		}
		case EventType::KeyDown: break;
		default: return false;
		}

		const int count = v->lineCount();
		const std::string_view L = v->line(v->caret_.line);
		auto move = [&](TextPos p) {
			const int old = v->caret_.line;
			v->caret_ = p;
			if (!shift) v->anchor_ = p;
			v->ensureVisible(p.line);
			v->caretMoved(old, true);
			return true;
		};
		switch (e.key)
		{
		case Key::Up:
		case Key::Down:
		case Key::PageUp:
		case Key::PageDown: {
			if (ctrl && (e.key == Key::Up || e.key == Key::Down))
			{
				v->setFirstVisibleLine(v->first_ + (e.key == Key::Up ? -1 : 1), true);
				return true;
			}
			const int vis = v->visibleLineCount();
			const int d = e.key == Key::Up ? -1 : e.key == Key::Down ? 1 : (e.key == Key::PageUp ? -vis : vis);
			const int ln = std::clamp(v->caret_.line + d, 0, count - 1);
			return move({ln, std::min(v->caret_.col, int(v->line(ln).size()))});
		}
		case Key::Left:
			if (!shift && !(v->caret_ == v->anchor_)) return move(std::min(v->caret_, v->anchor_));
			if (v->caret_.col > 0) return move({v->caret_.line, int(prevCp(L, std::size_t(v->caret_.col)))});
			if (v->caret_.line > 0) return move({v->caret_.line - 1, int(v->line(v->caret_.line - 1).size())});
			return move(v->caret_);
		case Key::Right:
			if (!shift && !(v->caret_ == v->anchor_)) return move(std::max(v->caret_, v->anchor_));
			if (v->caret_.col < int(L.size())) return move({v->caret_.line, int(nextCp(L, std::size_t(v->caret_.col)))});
			if (v->caret_.line + 1 < count) return move({v->caret_.line + 1, 0});
			return move(v->caret_);
		case Key::Home: return move(ctrl ? TextPos{0, 0} : TextPos{v->caret_.line, 0});
		case Key::End:
			if (ctrl) return move({count - 1, int(v->line(count - 1).size())});
			return move({v->caret_.line, int(L.size())});
		case Key::A:
			if (!ctrl || shift) return false;
			v->selectAll();
			return true;
		case Key::C:
			if (!ctrl || shift) return false;
			return v->copySelection();
		default: return false;
		}
	}
};

void registerCodeViewClass(UiContext& ui)
{
	if (ui.registry().find(CodeView::Glue::kName) != 0xFFFFFFFFu) return;
	ui.registry().registerWidget<CodeView::Glue>();
}

// ── CodeView ─────────────────────────────────────────────────────────────────

CodeView::CodeView(UiContext& ui, NodeHandle parent, model::CodeLang lang): ui_(ui), lang_(lang), palette_(defaultCodePalette())
{
	ensureRetdecUiClasses(ui);
	auto made = ui.create(Glue::kName, parent);
	node_ = made ? *made : NodeHandle{};
	ui.setUserData(node_, static_cast<u64>(reinterpret_cast<std::uintptr_t>(this)));
	retokenize();
}

CodeView::~CodeView()
{
	if (ui_.alive(node_)) ui_.setUserData(node_, 0);
}

void CodeView::invalidate()
{
	ui_.invalidate(node_);
}

void CodeView::retokenize()
{
	tokens_.clear();
	if (console_ || lang_ == model::CodeLang::Plain) return;
	tokens_ = model::highlightLines(lang_, lines_);
}

const std::vector<model::TokenSpan>& CodeView::tokens(int i) const
{
	static const std::vector<model::TokenSpan> kNone;
	if (i < 0 || std::size_t(i) >= tokens_.size()) return kNone;
	return tokens_[std::size_t(i)];
}

void CodeView::setText(std::string_view text)
{
	console_ = nullptr;
	lines_ = model::splitLines(text);
	retokenize();
	caret_ = anchor_ = {};
	first_ = 0;
	scrollX_ = 0.f;
	highlight_ = -1;
	pendingCenter_ = -1;
	invalidate();
}

std::string CodeView::text() const
{
	if (console_) return console_->text();
	return model::joinLines(lines_);
}

bool CodeView::isEmpty() const
{
	if (console_) return console_->isEmpty();
	return lines_.size() == 1 && lines_.front().empty();
}

int CodeView::lineCount() const
{
	if (console_) return int(console_->lineCount());
	return int(lines_.size());
}

std::string_view CodeView::line(int i) const
{
	if (i < 0 || i >= lineCount()) return {};
	if (console_) return console_->line(std::size_t(i));
	return lines_[std::size_t(i)];
}

void CodeView::setConsoleBuffer(const model::ConsoleBuffer* buffer)
{
	console_ = buffer;
	consoleDropped_ = buffer ? buffer->droppedLines() : 0;
	if (buffer) lines_.assign(1, std::string());
	retokenize();
	caret_ = anchor_ = {};
	first_ = 0;
	invalidate();
}

void CodeView::contentChanged()
{
	if (console_)
	{
		const std::uint64_t dropped = console_->droppedLines();
		const int delta = int(std::min<std::uint64_t>(dropped - consoleDropped_, std::uint64_t(1) << 30));
		consoleDropped_ = dropped;
		if (delta > 0)
		{
			first_ = std::max(0, first_ - delta);
			caret_.line = std::max(0, caret_.line - delta);
			anchor_.line = std::max(0, anchor_.line - delta);
			if (highlight_ >= 0) highlight_ = highlight_ - delta >= 0 ? highlight_ - delta : -1;
		}
	}
	else
		retokenize();
	clampCaret();
	invalidate();
}

void CodeView::setLang(model::CodeLang lang)
{
	if (lang == lang_) return;
	lang_ = lang;
	retokenize();
	invalidate();
}

void CodeView::setPalette(const CodePalette& p)
{
	palette_ = p;
	invalidate();
}

void CodeView::setShowGutter(bool on)
{
	gutter_ = on;
	invalidate();
}

void CodeView::setPlaceholder(std::string_view text)
{
	placeholder_ = std::string(text);
	invalidate();
}

void CodeView::setFontSize(float px)
{
	if (px <= 0.f) return;
	ui_.setCss(node_, "font-size", std::format("{}px", px));
}

float CodeView::lineHeight() const
{
	return std::ceil(ui_.computed(node_).fontSize * 1.35f);
}

float CodeView::gutterWidth() const
{
	if (!gutter_) return 0.f;
	char b[24];
	std::snprintf(b, sizeof b, "%d", std::max(1, lineCount()));
	return ui_.measureString(b, ui_.computed(node_).fontSize * 0.9f, node_) + 18.f;
}

void CodeView::clampCaret()
{
	auto clampPos = [&](TextPos& p) {
		p.line = std::clamp(p.line, 0, std::max(0, lineCount() - 1));
		p.col = std::clamp(p.col, 0, int(line(p.line).size()));
	};
	clampPos(caret_);
	clampPos(anchor_);
	first_ = std::clamp(first_, 0, scrollMax());
}

std::string CodeView::selectedText() const
{
	if (caret_ == anchor_) return {};
	if (!console_) return model::textInRange(lines_, {anchor_, caret_});
	std::vector<std::string> tmp;
	tmp.reserve(std::size_t(lineCount()));
	for (int i = 0; i < lineCount(); ++i)
		tmp.emplace_back(line(i));
	return model::textInRange(tmp, {anchor_, caret_});
}

void CodeView::caretMoved(int oldLine, bool notify)
{
	invalidate();
	if (notify && caret_.line != oldLine && onCaretLineChanged) onCaretLineChanged(caret_.line);
}

void CodeView::setCaret(TextPos p, bool extend, bool notify)
{
	const int old = caret_.line;
	caret_ = p;
	if (!extend) anchor_ = p;
	clampCaret();
	caretMoved(old, notify);
}

void CodeView::setSelection(TextRange r)
{
	anchor_ = r.begin;
	caret_ = r.end;
	clampCaret();
	invalidate();
}

void CodeView::selectAll()
{
	anchor_ = {0, 0};
	caret_ = {lineCount() - 1, int(line(lineCount() - 1).size())};
	invalidate();
}

bool CodeView::copySelection()
{
	const std::string s = selectedText();
	if (s.empty()) return false;
	ui_.setClipboardText(s);
	return true;
}

void CodeView::highlightLine(int line)
{
	highlight_ = line < 0 || line >= lineCount() ? -1 : line;
	invalidate();
}

bool CodeView::find(std::string_view query, bool forward, bool caseSensitive)
{
	if (query.empty() || isEmpty()) return false;
	std::optional<TextRange> r;
	const model::FindOptions opt{forward, caseSensitive, true};
	if (console_)
	{
		std::vector<std::string> tmp;
		tmp.reserve(std::size_t(lineCount()));
		for (int i = 0; i < lineCount(); ++i)
			tmp.emplace_back(line(i));
		r = model::findText(tmp, query, selection(), opt);
	}
	else
		r = model::findText(lines_, query, selection(), opt);
	if (!r) return false;
	setSelection(*r);
	ensureVisible(r->end.line);
	if (r->end.line < first_ || r->end.line >= first_ + visibleLineCount()) centerOnLine(r->end.line);
	return true;
}

int CodeView::visibleLineCount() const
{
	const float h = ui_.contentRect(node_).h;
	const float lh = lineHeight();
	if (lh <= 0.f) return 1;
	return std::max(1, int(h / lh));
}

int CodeView::scrollMax() const
{
	return std::max(0, lineCount() - visibleLineCount());
}

void CodeView::setFirstVisibleLine(int line, bool notify)
{
	const int f = std::clamp(line, 0, scrollMax());
	if (f == first_) return;
	first_ = f;
	invalidate();
	if (notify && onScrolled) onScrolled(first_);
}

void CodeView::centerOnLine(int line)
{
	line = std::clamp(line, 0, std::max(0, lineCount() - 1));
	if (ui_.contentRect(node_).h <= 0.f)
	{
		pendingCenter_ = line;
		first_ = line;
		invalidate();
		return;
	}
	pendingCenter_ = -1;
	first_ = std::clamp(line - visibleLineCount() / 2, 0, scrollMax());
	invalidate();
}

void CodeView::ensureVisible(int line)
{
	const int vis = visibleLineCount();
	if (line < first_) setFirstVisibleLine(line, true);
	else if (line >= first_ + vis) setFirstVisibleLine(line - vis + 1, true);
}

void CodeView::scrollToEnd()
{
	if (ui_.contentRect(node_).h <= 0.f)
	{
		pendingCenter_ = std::max(0, lineCount() - 1);
		first_ = scrollMax();
		invalidate();
		return;
	}
	first_ = scrollMax();
	invalidate();
}

TextPos CodeView::hitTest(Vec2 p) const
{
	const Rect c = ui_.contentRect(node_);
	const float fs = ui_.computed(node_).fontSize;
	TextPos r;
	r.line = std::clamp(first_ + int(std::max(0.f, (p.y - c.y) / lineHeight())), 0, std::max(0, lineCount() - 1));
	const std::string_view L = line(r.line);
	const float x = p.x - (c.x + gutterWidth() + 2.f - scrollX_);
	std::size_t col = 0;
	float prevW = 0.f;
	while (col < L.size())
	{
		const std::size_t nx = nextCp(L, col);
		const float nw = ui_.measureString(L.substr(0, nx), fs, node_);
		if (x < (prevW + nw) * 0.5f) break;
		prevW = nw;
		col = nx;
	}
	r.col = int(col);
	return r;
}

// ── SyncedCodePane ───────────────────────────────────────────────────────────

SyncedCodePane::SyncedCodePane(UiContext& ui, NodeHandle parent, model::CodeLang lang, std::string_view title): ui_(ui), title_(title)
{
	ensureRetdecUiClasses(ui);
	root_ = w::panel(ui, parent);
	ui.setCss(root_, "flex-grow", "1");
	ui.setCss(root_, "gap", "0px");
	ui.setCss(root_, "min-width", "0px");
	ui.setCss(root_, "min-height", "0px");
	titleLabel_ = w::label(ui, root_, title);
	ui.addClass(titleLabel_, "retdec-pane-title");
	view_ = std::make_unique<CodeView>(ui, root_, lang);
	view_->setPlaceholder("No content loaded\xE2\x80\xA6");
	view_->onCaretLineChanged = [this](int line) {
		if (onLineActivated) onLineActivated(line + 1);
	};
	view_->onScrolled = [this](int first) {
		if (onVerticalScrollChanged) onVerticalScrollChanged(first);
	};
}

SyncedCodePane::~SyncedCodePane()
{
	view_.reset();
}

void SyncedCodePane::setContent(std::string_view text)
{
	view_->setText(text);
	clearHighlight();
}

void SyncedCodePane::setLang(model::CodeLang lang)
{
	view_->setLang(lang);
}

void SyncedCodePane::highlightLine(int line)
{
	if (line < 1)
	{
		clearHighlight();
		return;
	}
	view_->highlightLine(line - 1);
}

void SyncedCodePane::scrollToLine(int line)
{
	if (line < 1) return;
	view_->setCaret({line - 1, 0}, false, false);
	view_->centerOnLine(line - 1);
}

void SyncedCodePane::clearHighlight()
{
	view_->highlightLine(-1);
}

int SyncedCodePane::highlightedLine() const
{
	const int h = view_->highlightedLine();
	return h < 0 ? -1 : h + 1;
}

void SyncedCodePane::setTitle(std::string_view title)
{
	title_ = std::string(title);
	ui_.setText(titleLabel_, title);
}

// ── TriPaneCodeView ──────────────────────────────────────────────────────────

model::CodeLang toCodeLang(OutputLang lang)
{
	switch (lang)
	{
	case OutputLang::C: return model::CodeLang::C;
	case OutputLang::Cxx: return model::CodeLang::Cxx;
	case OutputLang::CSharp: return model::CodeLang::CSharp;
	case OutputLang::Java: return model::CodeLang::Java;
	case OutputLang::Python: return model::CodeLang::Python;
	case OutputLang::Lua: return model::CodeLang::Lua;
	}
	return model::CodeLang::C;
}

OutputLang outputLangFromSettingsString(std::string_view s)
{
	switch (model::codeLangFromSettingsString(s))
	{
	case model::CodeLang::Cxx: return OutputLang::Cxx;
	case model::CodeLang::CSharp: return OutputLang::CSharp;
	case model::CodeLang::Java: return OutputLang::Java;
	case model::CodeLang::Python: return OutputLang::Python;
	case model::CodeLang::Lua: return OutputLang::Lua;
	default: return OutputLang::C;
	}
}

TriPaneCodeView::TriPaneCodeView(UiContext& ui, NodeHandle parent): ui_(ui)
{
	handler_.handle = [this](EventContext& ec, const Event& e) { return handleEvent(ec, e); };
	root_ = createPanelRoot(ui, parent, &handler_, "retdec-tripane");

	// Toolbar.
	const NodeHandle tb = toolbarRow(ui, root_);
	backBtn_ = w::toolButton(ui, tb, "\u25C0"); // ◀
	fwdBtn_ = w::toolButton(ui, tb, "\u25B6");  // ▶
	w::setTooltip(ui, backBtn_, "Navigate back (Alt+\u2190)");
	w::setTooltip(ui, fwdBtn_, "Navigate forward (Alt+\u2192)");
	funcLabel_ = w::label(ui, tb, "No function loaded");
	ui.setCss(funcLabel_, "flex-grow", "1");
	w::label(ui, tb, "Lang:");
	langCombo_ = w::comboBox(ui, tb, "C\nC++\nC#\nJava\nPython\nLua", 0);
	w::setTooltip(ui, langCombo_,
	              "View-only syntax highlighting. To change decompiled output: Settings \u2192 Decompiler \u2192 Output "
	              "language, then re-decompile (F5).");
	findBtn_ = w::toolButton(ui, tb, "Find"); // Qt: ⌕ (U+2315), in none of the bundled faces
	w::setTooltip(ui, findBtn_, "Find in panes (Ctrl+F)");

	// Panes.
	const NodeHandle split = w::splitter(ui, root_);
	ui.setCss(split, "flex-grow", "1");
	ui.setCss(split, "min-height", "0px");
	asm_ = std::make_unique<SyncedCodePane>(ui, w::splitterPane(ui, split), model::CodeLang::Asm, "Assembly");
	ir_ = std::make_unique<SyncedCodePane>(ui, w::splitterPane(ui, split), model::CodeLang::IR, "SSA IR");
	c_ = std::make_unique<SyncedCodePane>(ui, w::splitterPane(ui, split), model::CodeLang::C, "Decompiled C");

	// Find bar.
	findBar_ = w::panel(ui, root_);
	ui.addClass(findBar_, "retdec-findbar");
	ui.setCss(findBar_, "flex-direction", "row");
	ui.setCss(findBar_, "align-items", "center");
	ui.setCss(findBar_, "flex-shrink", "0");
	const NodeHandle fl = w::label(ui, findBar_, "Find:");
	ui.addClass(fl, "retdec-muted");
	findEdit_ = w::lineEdit(ui, findBar_, "");
	ui.setCss(findEdit_, "flex-grow", "1");
	w::setPlaceholderText(ui, findEdit_, "Search all panes\u2026");
	findNext_ = w::toolButton(ui, findBar_, "\u25BC");  // ▼
	findPrev_ = w::toolButton(ui, findBar_, "\u25B2");  // ▲
	findClose_ = w::toolButton(ui, findBar_, "\u2715"); // ✕
	w::setTooltip(ui, findNext_, "Next match (Enter)");
	w::setTooltip(ui, findPrev_, "Previous match");
	w::setTooltip(ui, findClose_, "Close (Escape)");
	setShown(ui, findBar_, false);

	// Wiring.
	auto* self = this;
	ui.connect(backBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->navigateBack(); });
	ui.connect(fwdBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->navigateForward(); });
	ui.connect(findBtn_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->showFindBar(); });
	ui.connect(findNext_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->findNext(); });
	ui.connect(findPrev_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->findPrevious(); });
	ui.connect(findClose_, SignalKind::Clicked, [self](UiContext&, NodeHandle, const SignalArgs&) { self->hideFindBar(); });
	ui.connect(findEdit_, SignalKind::Submitted, [self](UiContext&, NodeHandle, const SignalArgs&) { self->findNext(); });
	ui.connect(langCombo_, SignalKind::ValueChanged, [self](UiContext&, NodeHandle, const SignalArgs& a) {
		self->setOutputLanguage(static_cast<OutputLang>(std::clamp(int(a.value), 0, 5)));
		if (self->onStatusMessage)
			self->onStatusMessage("Change in Settings \u2192 Decompiler \u2192 Output language, then re-decompile.", 4000);
	});

	asm_->onLineActivated = [self](int l) { self->onLineActivated(model::TriPane::Asm, l); };
	ir_->onLineActivated = [self](int l) { self->onLineActivated(model::TriPane::IR, l); };
	c_->onLineActivated = [self](int l) { self->onLineActivated(model::TriPane::C, l); };
	asm_->onVerticalScrollChanged = [self](int v) { self->onScrollChanged(model::TriPane::Asm, v); };
	ir_->onVerticalScrollChanged = [self](int v) { self->onScrollChanged(model::TriPane::IR, v); };
	c_->onVerticalScrollChanged = [self](int v) { self->onScrollChanged(model::TriPane::C, v); };

	updateNavButtons();
}

TriPaneCodeView::~TriPaneCodeView()
{
	asm_.reset();
	ir_.reset();
	c_.reset();
	releasePanelRoot(ui_, root_);
}

SyncedCodePane& TriPaneCodeView::pane(model::TriPane p)
{
	return p == model::TriPane::Asm ? *asm_ : p == model::TriPane::IR ? *ir_ : *c_;
}

std::string TriPaneCodeView::functionLabel() const
{
	return std::string(ui_.text(funcLabel_));
}

bool TriPaneCodeView::handleEvent(EventContext& ec, const Event& e)
{
	if (e.type != EventType::KeyDown) return false;
	if (ec.phase == Phase::Capture)
	{
		if (isKey(e, Key::F, kModCtrl))
		{
			showFindBar();
			return true;
		}
		if (isKey(e, Key::Left, kModAlt))
		{
			navigateBack();
			return true;
		}
		if (isKey(e, Key::Right, kModAlt))
		{
			navigateForward();
			return true;
		}
		if (isKey(e, Key::F3))
		{
			findNext();
			return true;
		}
		if (isKey(e, Key::F3, kModShift))
		{
			findPrevious();
			return true;
		}
		return false;
	}
	if (ec.phase == Phase::Bubble && isKey(e, Key::Escape) && findShown_)
	{
		hideFindBar();
		return true;
	}
	return false;
}

void TriPaneCodeView::loadFunction(uint64_t address, std::string_view asmText, std::string_view irText, std::string_view cText)
{
	syncingScroll_ = true;
	asm_->setContent(asmText);
	ir_->setContent(irText);
	c_->setContent(cText);
	syncingScroll_ = false;
	ui_.setText(funcLabel_, std::format("0x{:x}", address));
}

void TriPaneCodeView::setLineMapping(const model::LineMapping& mapping)
{
	mapping_ = mapping;
}

void TriPaneCodeView::buildLineMappingFromAddresses()
{
	mapping_ = model::buildLineMappingByAddress(asm_->editor().lines(), ir_->editor().lines(), c_->editor().lines());
}

void TriPaneCodeView::setOutputLanguage(OutputLang lang)
{
	outputLang_ = lang;
	c_->setLang(toCodeLang(lang));
	ui_.setValue(langCombo_, float(static_cast<int>(lang)), false);
	static constexpr std::string_view kTitles[] = {"Decompiled C",    "Decompiled C++",    "Decompiled C#",
	                                               "Decompiled Java", "Decompiled Python", "Decompiled Lua"};
	c_->setTitle(kTitles[static_cast<int>(lang)]);
}

void TriPaneCodeView::syncLanguageFromSettings(std::string_view settingsValue)
{
	setOutputLanguage(outputLangFromSettingsString(settingsValue));
}

void TriPaneCodeView::applyEditorFont(float px)
{
	asm_->setEditorFontSize(px);
	ir_->setEditorFontSize(px);
	c_->setEditorFontSize(px);
}

void TriPaneCodeView::navigateTo(uint64_t address)
{
	if (onAddressNavigated) onAddressNavigated(address);
}

void TriPaneCodeView::navigateBack()
{
	if (auto e = history_.back())
	{
		updateNavButtons();
		if (onAddressNavigated) onAddressNavigated(e->address);
	}
}

void TriPaneCodeView::navigateForward()
{
	if (auto e = history_.forward())
	{
		updateNavButtons();
		if (onAddressNavigated) onAddressNavigated(e->address);
	}
}

void TriPaneCodeView::updateNavButtons()
{
	ui_.setDisabled(backBtn_, !history_.canGoBack());
	ui_.setDisabled(fwdBtn_, !history_.canGoForward());
}

void TriPaneCodeView::onFunctionSelected(uint64_t address, std::string_view name)
{
	ui_.setText(funcLabel_, std::format("{}  @ 0x{:x}", name, address));
	history_.push(address, std::string(name));
	updateNavButtons();
}

void TriPaneCodeView::applyHighlight(int asmLine, int irLine, int cLine)
{
	asm_->highlightLine(asmLine);
	ir_->highlightLine(irLine);
	c_->highlightLine(cLine);
	if (asmLine > 0) asm_->scrollToLine(asmLine);
	if (irLine > 0) ir_->scrollToLine(irLine);
	if (cLine > 0) c_->scrollToLine(cLine);
}

void TriPaneCodeView::onLineActivated(model::TriPane pane, int line)
{
	if (mapping_.isEmpty()) return;
	if (const auto* e = mapping_.byLine(pane, line)) applyHighlight(e->asmLine, e->irLine, e->cLine);
}

void TriPaneCodeView::activateLine(model::TriPane p, int line)
{
	pane(p).editor().setCaret({line - 1, 0}, false, false);
	onLineActivated(p, line);
}

void TriPaneCodeView::onScrollChanged(model::TriPane src, int value)
{
	if (syncingScroll_) return;
	syncingScroll_ = true;
	const int srcMax = pane(src).editor().scrollMax();
	for (model::TriPane p: {model::TriPane::Asm, model::TriPane::IR, model::TriPane::C})
	{
		if (p == src) continue;
		CodeView& v = pane(p).editor();
		v.setFirstVisibleLine(model::proportionalScroll(value, srcMax, v.scrollMax()), false);
	}
	syncingScroll_ = false;
}

void TriPaneCodeView::showFindBar()
{
	findShown_ = true;
	setShown(ui_, findBar_, true);
	ui_.setFocus(findEdit_, FocusReason::Keyboard);
	ui_.editBuffer(findEdit_).selectAll();
}

void TriPaneCodeView::hideFindBar()
{
	findShown_ = false;
	setShown(ui_, findBar_, false);
	setLineEditText(ui_, findEdit_, "");
	asm_->clearHighlight();
	ir_->clearHighlight();
	c_->clearHighlight();
}

void TriPaneCodeView::setFindText(std::string_view text)
{
	setLineEditText(ui_, findEdit_, text);
}

void TriPaneCodeView::findNext()
{
	const std::string text = ui_.editBuffer(findEdit_).text();
	if (text.empty()) return;
	asm_->editor().find(text, true);
	ir_->editor().find(text, true);
	c_->editor().find(text, true);
}

void TriPaneCodeView::findPrevious()
{
	const std::string text = ui_.editBuffer(findEdit_).text();
	if (text.empty()) return;
	asm_->editor().find(text, false);
	ir_->editor().find(text, false);
	c_->editor().find(text, false);
}

void TriPaneCodeView::clear()
{
	asm_->setContent({});
	ir_->setContent({});
	c_->setContent({});
	mapping_.clear();
	history_.clear();
	updateNavButtons();
	ui_.setText(funcLabel_, "No function loaded");
	hideFindBar();
}

} // namespace retdec::gui::ui
