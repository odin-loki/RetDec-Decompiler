// FUSE UI extras: several font faces, glyph fallback, placeholders, double clicks, multi-selection item views,
// spin box special-value text and the indeterminate progress bar. One binary, one ctest gate per area
// (fuse_ui_extra_<gate>, run with --gate=<name>):
//   font_faces             CSS font-family / font-weight pick registered faces per node (labels, lineedit, codeedit,
//                          table columns / cells); measurement uses the node's face; run-time registration and font
//                          search paths; one glyph atlas serves every face
//   glyph_fallback         check marks, arrows, geometric shapes and the ellipsis render in every bundled face through
//                          the bundled fallback chain (DejaVu Sans) and through UiContext::addFallbackFont
//   placeholder            lineedit / codeedit placeholder: muted colour, CSS placeholder-color, hidden on focus / text
//   double_click           DoubleClicked from press timing and distance (virtual clock), configurable, click counts
//   table_multiselect      Single (default) vs Extended selection with Ctrl / Shift clicks and keys, Ctrl+A, the
//                          selection API, scrollToRow, Activated on Enter / double click
//   spinbox_special        special-value text at the minimum
//   progress_indeterminate the busy bar animates on the context clock and stops when switched off
//   cooked_font_props      (with fuse_ui_doc) font-family / font-weight survive the style cooker; custom properties too
#include "test_util.hpp"

#if defined(FUSE_UI_EXTRAS_HAVE_DOC)
#include "fuse/ui/doc/cook.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::text;
namespace w = fuse::ui::widgets;

namespace {
double g_now = 0.0;
double fakeNow(void*) { return g_now; }

std::string utf8(char32_t cp) {
    std::string s;
    appendUtf8(s, cp);
    return s;
}

/// Glyph commands painted for node `n`.
std::vector<DrawCmd> glyphsOf(const UiContext& ui, NodeHandle n) {
    std::vector<DrawCmd> out;
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.op == DrawOp::Glyph && c.node == n.index) out.push_back(c);
    return out;
}

void settle(UiContext& ui) {
    for (int i = 0; i < 3; ++i) ui.update(1.f / 60.f);
}

// ---- font_faces ------------------------------------------------------------------------------------------------------
struct CellModel {
    std::vector<std::string> rows{"iiiiiiii", "MMMMMMMM"};
    w::ItemModel model() const {
        w::ItemModel m;
        m.user = this;
        m.columns = 3;
        m.rows = [](const void* u) { return u32(static_cast<const CellModel*>(u)->rows.size()); };
        m.data = [](const void* u, u32 r, u32, std::string&) { return std::string_view(static_cast<const CellModel*>(u)->rows[r]); };
        m.header = [](const void*, u32 c) { return std::string_view(c == 0 ? "col" : c == 1 ? "plain" : "cell"); };
        m.font = [](const void*, u32, u32 c) { return c == 2 ? w::CellFont{"monospace", 0} : w::CellFont{}; };
        return m;
    }
};

/// Horizontal span (first to last glyph x) of the glyphs painted in table cell (row, col).
f32 cellSpan(const UiContext& ui, NodeHandle table, u32 row, u32 col) {
    const Rect c = ui.contentRect(table);
    const f32 rh = ui.lineHeight(ui.computed(table).fontSize, table) + 6.f;
    f32 x0 = c.x;
    for (u32 k = 0; k < col; ++k) x0 += w::tableColumnWidth(ui, table, k);
    const f32 x1 = x0 + w::tableColumnWidth(ui, table, col);
    const f32 y0 = c.y + rh * f32(row + 1), y1 = y0 + rh; // + header row
    f32 lo = 1e9f, hi = -1e9f;
    int n = 0;
    for (const DrawCmd& g : glyphsOf(ui, table)) {
        const f32 cy = g.rect.y + g.rect.h * 0.5f;
        if (g.rect.x < x0 || g.rect.x >= x1 || cy < y0 || cy >= y1) continue;
        lo = std::min(lo, g.rect.x);
        hi = std::max(hi, g.rect.x);
        ++n;
    }
    return n >= 2 ? hi - lo : -1.f;
}

void gateFontFaces() {
    // run-time font files: search paths and Font::load
    UI_CHECK(!findFontFile("opensans/OpenSans.ttf").empty());
    UI_CHECK(!findFontFile("jetbrainsmono/JetBrainsMono-Bold.ttf").empty());
    UI_CHECK(findFontFile("no/such-font.ttf").empty());
    UI_CHECK(!Font::load("no/such-font.ttf").has_value());
    UI_CHECK(Font::load("jetbrainsmono/JetBrainsMono-Regular.ttf").has_value());
    const std::string bogus = "/nonexistent/fuse-fonts";
    addFontSearchPath(bogus);
    const auto paths = fontSearchPaths();
    UI_CHECK(!paths.empty() && paths.front() == bogus);
    UI_CHECK(Font::bundled().has_value()); // the compiled-in build-tree path still works

    UiContext ui{{.viewport = {700, 400}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    UI_CHECK_EQ(ui.registerBundledFonts(), 4u);
    const Font* def = ui.font();
    const Font* mono = ui.resolveFont("monospace");
    const Font* monoBold = ui.resolveFont("monospace", kFontWeightBold);
    const Font* sansBold = ui.resolveFont("sans-serif", kFontWeightBold);
    UI_CHECK(def == &fuse::ui::test::font()); // the desc font stays the default face
    UI_CHECK(mono && monoBold && sansBold && mono != def && monoBold != mono && sansBold != def && sansBold != monoBold);
    UI_CHECK(ui.resolveFont("\"JetBrains Mono\", serif") == mono);         // quoted family names, case-insensitive
    UI_CHECK(ui.resolveFont("jetbrains   MONO", 800) == monoBold);          // nearest heavier weight for bold asks
    UI_CHECK(ui.resolveFont("Unknown Family", kFontWeightBold) == sansBold); // default family's bold face
    UI_CHECK(ui.resolveFont("Unknown Family") == def);
    UI_CHECK(ui.resolveFont("Unknown, monospace") == mono);                 // first registered family in the list
    ui.setFontAlias("code", "JetBrains Mono");
    UI_CHECK(ui.resolveFont("code") == mono);

    // a monospace face: equal advances for narrow and wide letters
    const f32 wi = ui.measureString("iiiiiiii", 14.f, mono), wm = ui.measureString("MMMMMMMM", 14.f, mono);
    const f32 pi = ui.measureString("iiiiiiii", 14.f), pm = ui.measureString("MMMMMMMM", 14.f);
    UI_CHECK(std::abs(wi - wm) < 0.01f && pm > pi * 2.f);

    // per-node selection through CSS (sheet, inline, inheritance, class defaults)
    UI_CHECK(ui.addStyleSheet(".mono { font-family: 'JetBrains Mono', monospace; } .strong { font-weight: bold; }").has_value());
    const NodeHandle plain = w::label(ui, ui.root(), "iiiiiiii", "plain");
    const NodeHandle code = w::label(ui, ui.root(), "iiiiiiii", "code");
    ui.addClass(code, "mono");
    const NodeHandle strong = w::label(ui, ui.root(), "Bold text", "strong");
    ui.addClass(strong, "strong");
    const NodeHandle box = w::panel(ui, ui.root(), "box");
    ui.setCss(box, "font-family", "monospace");
    ui.setCss(box, "font-weight", "700");
    const NodeHandle inherited = w::label(ui, box, "iiiiiiii", "inherited");
    const NodeHandle edit = w::lineEdit(ui, ui.root(), "MMMM", "edit");
    ui.setCss(edit, "font-family", "monospace");
    const NodeHandle ce = w::codeEdit(ui, ui.root(), "int main() {}", w::CodeLanguage::Cpp, "ce");
    ui.setCss(ce, "height", "60px");
    settle(ui);
    UI_CHECK(ui.fontFor(plain) == def);
    UI_CHECK(ui.fontFor(code) == mono);
    UI_CHECK(ui.fontFor(strong) == sansBold);
    UI_CHECK(ui.fontFor(inherited) == monoBold);
    UI_CHECK(ui.fontFor(edit) == mono);
    UI_CHECK(ui.fontFor(ce) == mono); // codeedit is monospace by default
    UI_CHECK(readProp(ui.computed(strong), Prop::FontWeight).num == 700.f);
    // the laid-out label text and the label's measured width use its face
    UI_CHECK(std::abs(ui.textLayout(code)->width - wi) < 0.5f);
    UI_CHECK(std::abs(ui.textLayout(plain)->width - pi) < 0.5f);
    UI_CHECK(std::abs(ui.measureString("iiiiiiii", 14.f, code) - wi) < 0.01f);
    UI_CHECK(ui.rect(code).w >= std::floor(wi) && ui.rect(plain).w < ui.rect(code).w);
    // switching the family at run time relays out
    ui.removeClass(code, "mono");
    settle(ui);
    UI_CHECK(ui.fontFor(code) == def && std::abs(ui.textLayout(code)->width - pi) < 0.5f);
    ui.addClass(code, "mono");
    settle(ui);

    // one atlas, several faces: every label paints glyphs; different faces give different coverage
    UI_CHECK(!glyphsOf(ui, plain).empty() && !glyphsOf(ui, code).empty() && !glyphsOf(ui, strong).empty());
    UI_CHECK(!glyphsOf(ui, edit).empty() && !glyphsOf(ui, ce).empty());
    const auto gp = glyphsOf(ui, plain), gc = glyphsOf(ui, code);
    UI_CHECK(gp.size() == gc.size() && (gp[0].tw != gc[0].tw || gp[0].th != gc[0].th || gp.back().rect.x - gp[0].rect.x != gc.back().rect.x - gc[0].rect.x));

    // table: column font, per-cell font (ItemModel::font), the view's own font for the rest
    CellModel cm;
    const w::ItemModel model = cm.model();
    const NodeHandle table = w::tableView(ui, ui.root(), &model, "table");
    ui.setCss(table, "height", "120px");
    w::setTableColumnFont(ui, table, 0, "monospace");
    for (u32 c = 0; c < 3; ++c) w::setTableColumnWidth(ui, table, c, 140.f);
    settle(ui);
    const f32 s00 = cellSpan(ui, table, 0, 0), s10 = cellSpan(ui, table, 1, 0);
    const f32 s01 = cellSpan(ui, table, 0, 1), s11 = cellSpan(ui, table, 1, 1);
    const f32 s02 = cellSpan(ui, table, 0, 2), s12 = cellSpan(ui, table, 1, 2);
    std::printf("font_faces: column spans mono %.1f/%.1f plain %.1f/%.1f cell %.1f/%.1f\n", double(s00), double(s10),
                double(s01), double(s11), double(s02), double(s12));
    UI_CHECK(s00 > 0.f && std::abs(s00 - s10) <= 1.f);
    UI_CHECK(s01 > 0.f && s11 > s01 * 2.f);
    UI_CHECK(s02 > 0.f && std::abs(s02 - s12) <= 1.f);
    // CSS on the table node: every cell monospace
    ui.setCss(table, "font-family", "monospace");
    settle(ui);
    UI_CHECK(std::abs(cellSpan(ui, table, 0, 1) - cellSpan(ui, table, 1, 1)) <= 1.f);

    // owned registration from a file
    auto reg = ui.registerFontFile("My Mono", 400, "jetbrainsmono/JetBrainsMono-Regular.ttf");
    UI_CHECK(reg.has_value() && *reg && ui.resolveFont("my mono") == *reg);
    UI_CHECK(!ui.registerFontFile("Nope", 400, "no/such.ttf").has_value());

    // a context with no desc font gets the bundled regular face as default
    UiContext bare{{.viewport = {200, 100}}};
    UI_CHECK(bare.font() == nullptr);
    UI_CHECK(bare.registerBundledFonts() == 4u && bare.font() != nullptr && bare.font() != bare.resolveFont("monospace"));
}

// ---- glyph_fallback ------------------------------------------------------------------------------------------------
void gateGlyphFallback() {
    static constexpr char32_t kSymbols[] = {0x2713, 0x2717, 0x25CB, 0x25C9, 0x2192, 0x2026, 0x2190, 0x25B6, 0x25BC, 0x2022, 0x26A0};
    UiContext ui{{.viewport = {600, 200}}};
    w::install(ui);
    UI_CHECK_EQ(ui.registerBundledFonts(), 4u);
    std::vector<ShapedGlyph> g;
    for (const char* fam : {"sans-serif", "monospace"})
        for (u16 weight : {kFontWeightNormal, kFontWeightBold}) {
            const Font* f = ui.resolveFont(fam, weight);
            UI_CHECK(f != nullptr);
            if (!f) continue;
            for (char32_t cp : kSymbols) {
                shapeText(*f, utf8(cp), 14.f, g);
                const bool ok = g.size() == 1 && g[0].glyph != 0;
                if (!ok) std::printf("  %s/%u: U+%04X missing\n", fam, unsigned(weight), unsigned(cp));
                UI_CHECK(ok);
            }
        }
    // a label mixing them lays out with no .notdef and paints a glyph per symbol
    std::string s = "ok ";
    for (char32_t cp : kSymbols) { appendUtf8(s, cp); s += ' '; }
    const NodeHandle l = w::label(ui, ui.root(), s, "symbols");
    const NodeHandle m = w::label(ui, ui.root(), s, "symbols.mono");
    ui.setCss(m, "font-family", "monospace");
    settle(ui);
    for (NodeHandle n : {l, m}) {
        int notdef = 0;
        for (const auto& x : ui.textLayout(n)->glyphs) notdef += x.glyph == 0;
        UI_CHECK_EQ(notdef, 0);
        UI_CHECK(glyphsOf(ui, n).size() >= std::size(kSymbols) + 2);
    }

    // owned faces get the context's fallback chain (addFallbackFont), before or after registration
    UiContext own{{.viewport = {200, 100}}};
    auto sans = Font::bundled();
    UI_CHECK(sans.has_value());
    if (!sans) return;
    const Font* plain = own.registerFont("Plain", 400, std::move(*sans));
    UI_CHECK(plain && plain->faceFor(0x2713) == 0 && plain->glyphIndex(0x2713) == 0);
    auto dejavu = Font::load("dejavu/DejaVuSans.ttf");
    UI_CHECK(dejavu.has_value());
    if (!dejavu) return;
    own.addFallbackFont(std::move(*dejavu));
    UI_CHECK(plain->faceFor(0x2713) == 1 && plain->faceFor(U'A') == 0);
}

// ---- placeholder ---------------------------------------------------------------------------------------------------
void gatePlaceholder() {
    UiContext ui{{.viewport = {400, 300}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    const NodeHandle edit = w::lineEdit(ui, ui.root(), {}, "edit");
    const NodeHandle other = w::button(ui, ui.root(), "other", "other");
    const NodeHandle ce = w::codeEdit(ui, ui.root(), {}, w::CodeLanguage::Plain, "ce");
    ui.setCss(ce, "height", "60px");
    w::setPlaceholderText(ui, edit, "Filter functions");
    w::setPlaceholderText(ui, ce, "Paste code here");
    UI_CHECK(w::placeholderText(ui, edit) == "Filter functions");
    UI_CHECK(ui.text(edit).empty()); // the placeholder is not the text
    settle(ui);
    const Color text = ui.computed(edit).color;
    auto ph = glyphsOf(ui, edit);
    UI_CHECK(ph.size() >= 10);
    UI_CHECK(!ph.empty() && ph[0].fill.a < text.a * 0.6f && ph[0].fill.a > 0.f); // muted
    UI_CHECK(!glyphsOf(ui, ce).empty());
    // focused: no placeholder (caret only)
    fuse::ui::test::UiTestDriver d(ui);
    d.click("edit");
    UI_CHECK(ui.focused() == edit);
    UI_CHECK(glyphsOf(ui, edit).empty());
    d.type("abc");
    UI_CHECK(ui.text(edit) == "abc");
    auto typed = glyphsOf(ui, edit);
    UI_CHECK(typed.size() == 3 && typed[0].fill.a == text.a);
    d.click("other");
    UI_CHECK(glyphsOf(ui, edit).size() == 3); // has text: no placeholder
    d.click("edit");
    for (int i = 0; i < 3; ++i) d.key(Key::Backspace);
    d.click("other");
    UI_CHECK(ui.text(edit).empty() && glyphsOf(ui, edit).size() >= 10);
    // CSS-styleable colour
    UI_CHECK(ui.addStyleSheet("lineedit { placeholder-color: #ff0000; }").has_value());
    settle(ui);
    ph = glyphsOf(ui, edit);
    UI_CHECK(!ph.empty() && ph[0].fill.r > 0.99f && ph[0].fill.g < 0.01f && ph[0].fill.a > 0.99f);
    // code edit: hidden once it has focus
    d.click("ce");
    UI_CHECK(ui.focused() == ce && glyphsOf(ui, ce).size() < 5);
}

// ---- double_click ----------------------------------------------------------------------------------------------------
void gateDoubleClick() {
    UiContext ui{{.viewport = {400, 300}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    ui.setClock({nullptr, &fakeNow});
    g_now = 100.0;
    const NodeHandle a = w::button(ui, ui.root(), "A", "a");
    const NodeHandle b = w::button(ui, ui.root(), "B", "b");
    ui.setCss(a, "width", "120px");
    const NodeHandle row = w::panel(ui, ui.root(), "row");
    const NodeHandle lab = w::label(ui, row, "a label", "lab");
    int dblA = 0, dblB = 0, dblLab = 0, clicksA = 0;
    u8 lastClicks = 0;
    ui.connect(a, SignalKind::DoubleClicked, [&](UiContext&, NodeHandle, const SignalArgs& s) {
        ++dblA;
        UI_CHECK(s.event && s.event->type == EventType::DoubleClick && s.event->clicks == 2);
    });
    ui.connect(b, SignalKind::DoubleClicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++dblB; });
    ui.connect(lab, SignalKind::DoubleClicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++dblLab; });
    ui.connect(a, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++clicksA; });
    ui.connect(a, SignalKind::Event, [&](UiContext&, NodeHandle, const SignalArgs& s) {
        if (s.event && s.event->type == EventType::PointerDown) lastClicks = s.event->clicks;
    });
    settle(ui);
    fuse::ui::test::UiTestDriver d(ui);
    const Vec2 pa = d.center("a");
    auto clickAt = [&](Vec2 p, double t) { g_now = t; d.clickAt(p); };
    clickAt(pa, 100.0);
    UI_CHECK(dblA == 0 && lastClicks == 1);
    clickAt(pa, 100.3);
    UI_CHECK(dblA == 1 && lastClicks == 2 && clicksA == 2); // both presses still click
    clickAt(pa, 100.5); // triple click: no second DoubleClicked
    UI_CHECK(dblA == 1 && lastClicks == 3);
    clickAt(pa, 102.0);
    clickAt(pa, 102.7); // slower than 0.5 s
    UI_CHECK(dblA == 1 && lastClicks == 1);
    clickAt(pa, 104.0);
    clickAt({pa.x + 9.f, pa.y}, 104.1); // moved more than 4 px
    UI_CHECK(dblA == 1);
    clickAt(pa, 106.0);
    clickAt(d.center("b"), 106.1); // another node
    UI_CHECK(dblA == 1 && dblB == 0);
    ui.setDoubleClick({.interval = 1.f, .distance = 12.f});
    UI_CHECK(ui.doubleClick().interval == 1.f);
    clickAt(pa, 108.0);
    clickAt({pa.x + 9.f, pa.y}, 108.8);
    UI_CHECK(dblA == 2);
    // a non-interactive node gets it too (delivered to the clicked node)
    clickAt(d.center("lab"), 110.0);
    clickAt(d.center("lab"), 110.2);
    UI_CHECK(dblLab == 1);
    // right button double click is a series of its own
    g_now = 112.0;
    ui.pointerDown(pa, MouseButton::Right);
    ui.pointerUp(pa, MouseButton::Right);
    g_now = 112.1;
    ui.pointerDown(pa, MouseButton::Left);
    ui.pointerUp(pa, MouseButton::Left);
    UI_CHECK(dblA == 2);
    // the default clock is steady_clock
    UiContext plain{{.viewport = {10, 10}}};
    const double t0 = plain.now();
    UI_CHECK(t0 > 0.0 && plain.now() >= t0);
}

// ---- table_multiselect -----------------------------------------------------------------------------------------------
void gateTableMultiSelect() {
    UiContext ui{{.viewport = {500, 400}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    ui.setClock({nullptr, &fakeNow});
    g_now = 0.0;
    w::StringTableModel m("Address\tName");
    char buf[64];
    for (int i = 0; i < 100; ++i) {
        std::snprintf(buf, sizeof buf, "%08x\tfunc_%d", 0x401000 + i * 16, i);
        m.addRow(buf);
    }
    const NodeHandle t = w::tableView(ui, ui.root(), &m.model(), "table");
    ui.setCss(t, "height", "300px");
    settle(ui);
    const Rect c = ui.contentRect(t);
    const f32 rh = ui.lineHeight(ui.computed(t).fontSize, t) + 6.f;
    auto rowPos = [&](u32 r) { return Vec2{c.x + 40.f, c.y + rh * (f32(r - w::tableFirstVisible(ui, t)) + 1.5f)}; };
    auto press = [&](u32 r, u8 mods = 0) {
        g_now += 1.0; // never a double click
        ui.setModifiers(mods);
        const Vec2 p = rowPos(r);
        ui.pointerMove(p);
        ui.pointerDown(p);
        ui.pointerUp(p);
        ui.setModifiers(0);
        ui.update(1.f / 60.f);
    };
    auto rows = [&] { return w::tableSelectedRows(ui, t); };
    int selSignals = 0;
    ui.connect(t, SignalKind::SelectionChanged, [&](UiContext&, NodeHandle, const SignalArgs&) { ++selSignals; });

    // Single (default): Ctrl / Shift change nothing about the one selected row
    UI_CHECK(w::tableSelectionMode(ui, t) == w::SelectionMode::Single);
    press(3);
    UI_CHECK(ui.value(t) == 3.f && rows() == std::vector<u32>{3});
    press(5, kModCtrl);
    UI_CHECK(rows() == std::vector<u32>{5} && w::tableSelectedCount(ui, t) == 1);
    ui.keyDown(Key::A, kModCtrl);
    UI_CHECK(w::tableSelectedCount(ui, t) == 1);

    // Extended
    w::setTableSelectionMode(ui, t, w::SelectionMode::Extended);
    UI_CHECK(rows() == std::vector<u32>{5}); // the current row carries over
    press(2);
    UI_CHECK(rows() == std::vector<u32>{2});
    press(5, kModCtrl);
    UI_CHECK((rows() == std::vector<u32>{2, 5}) && ui.value(t) == 5.f);
    press(8, kModShift); // from the anchor (5), replacing
    UI_CHECK((rows() == std::vector<u32>{5, 6, 7, 8}));
    press(0, kModShift | kModCtrl); // adds 0..5
    UI_CHECK((rows() == std::vector<u32>{0, 1, 2, 3, 4, 5, 6, 7, 8}));
    press(5, kModCtrl); // toggles off
    UI_CHECK(!w::tableRowSelected(ui, t, 5) && w::tableRowSelected(ui, t, 4) && w::tableSelectedCount(ui, t) == 8);
    UI_CHECK(selSignals >= 5);
    // keys
    press(10);
    ui.keyDown(Key::Down, kModShift);
    ui.keyDown(Key::Down, kModShift);
    UI_CHECK((rows() == std::vector<u32>{10, 11, 12}) && ui.value(t) == 12.f);
    ui.keyDown(Key::Down, kModCtrl); // current only
    UI_CHECK((rows() == std::vector<u32>{10, 11, 12}) && ui.value(t) == 13.f);
    ui.keyDown(Key::Space, kModCtrl);
    UI_CHECK((rows() == std::vector<u32>{10, 11, 12, 13}));
    ui.keyDown(Key::Up);
    UI_CHECK(rows() == std::vector<u32>{12});
    ui.keyDown(Key::A, kModCtrl);
    UI_CHECK(w::tableSelectedCount(ui, t) == 100 && rows().size() == 100);
    // API
    w::tableClearSelection(ui, t);
    UI_CHECK(w::tableSelectedCount(ui, t) == 0);
    w::setTableRowSelected(ui, t, 42, true);
    w::setTableRowSelected(ui, t, 44, true);
    UI_CHECK((rows() == std::vector<u32>{42, 44}));
    w::setTableRowSelected(ui, t, 42, false);
    UI_CHECK(rows() == std::vector<u32>{44});
    w::tableSelectAll(ui, t);
    UI_CHECK(w::tableSelectedCount(ui, t) == 100);
    // the model shrinks: flags past the end go
    m.clear();
    for (int i = 0; i < 50; ++i) m.addRow("x\ty");
    w::tableModelChanged(ui, t);
    UI_CHECK(w::tableSelectedCount(ui, t) == 50);
    for (int i = 50; i < 100; ++i) m.addRow("x\ty");
    w::tableModelChanged(ui, t);
    settle(ui);

    // scrollToRow
    const u32 first0 = w::tableFirstVisible(ui, t);
    w::scrollToRow(ui, t, 80);
    const u32 f1 = w::tableFirstVisible(ui, t);
    const u32 vis = u32((c.h - rh) / rh);
    UI_CHECK(f1 != first0 && f1 <= 80 && 80 < f1 + vis);
    w::scrollToRow(ui, t, 50, w::ScrollHint::Top);
    UI_CHECK(w::tableFirstVisible(ui, t) == 50);
    w::scrollToRow(ui, t, 60, w::ScrollHint::Center);
    UI_CHECK(w::tableFirstVisible(ui, t) == 60 - vis / 2);
    w::scrollToRow(ui, t, 60); // already visible: no scroll
    UI_CHECK(w::tableFirstVisible(ui, t) == 60 - vis / 2);
    settle(ui);

    // activation: off -> DoubleClicked on the table; on -> Activated {row, column}
    int dbl = 0, act = 0, submitted = 0;
    f32 actRow = -1.f;
    i32 actCol = -2;
    ui.connect(t, SignalKind::DoubleClicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++dbl; });
    ui.connect(t, SignalKind::Activated, [&](UiContext&, NodeHandle, const SignalArgs& s) { ++act; actRow = s.value; actCol = s.index; });
    ui.connect(t, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs&) { ++submitted; });
    const u32 target = w::tableFirstVisible(ui, t) + 2;
    auto dblClick = [&](Vec2 p) {
        g_now += 5.0;
        for (int k = 0; k < 2; ++k) {
            ui.pointerMove(p);
            ui.pointerDown(p);
            ui.pointerUp(p);
            g_now += 0.1;
        }
        ui.update(1.f / 60.f);
    };
    dblClick(rowPos(target));
    UI_CHECK(dbl == 1 && act == 0);
    w::setTableActivation(ui, t, true);
    const Vec2 p2{c.x + w::tableColumnWidth(ui, t, 0) + 20.f, rowPos(target).y};
    dblClick(p2);
    UI_CHECK(act == 1 && actRow == f32(target) && actCol == 1 && dbl == 1);
    UI_CHECK(rows() == std::vector<u32>{target});
    ui.keyDown(Key::Enter);
    UI_CHECK(act == 2 && actCol == -1 && submitted == 1);

    // modelListView shares the machinery
    const NodeHandle lv = w::modelListView(ui, ui.root(), &m.model(), "lv");
    w::setTableSelectionMode(ui, lv, w::SelectionMode::Extended);
    w::tableSelectAll(ui, lv);
    UI_CHECK(w::tableSelectedCount(ui, lv) == 100);
}

// ---- spinbox_special -------------------------------------------------------------------------------------------------
void gateSpinBoxSpecial() {
    UiContext ui{{.viewport = {300, 100}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    const NodeHandle sp = w::spinBox(ui, ui.root(), 0.f, 10.f, 0.f, 1.f, "sp");
    std::string scratch;
    UI_CHECK(w::spinBoxText(ui, sp, scratch) == "0");
    w::setSpecialValueText(ui, sp, "All");
    UI_CHECK(w::specialValueText(ui, sp) == "All");
    UI_CHECK(w::spinBoxText(ui, sp, scratch) == "All");
    settle(ui);
    UI_CHECK(glyphsOf(ui, sp).size() == 3);
    ui.setValue(sp, 3.f);
    settle(ui);
    UI_CHECK(w::spinBoxText(ui, sp, scratch) == "3" && glyphsOf(ui, sp).size() == 1);
    ui.setValue(sp, 0.f);
    settle(ui);
    UI_CHECK(glyphsOf(ui, sp).size() == 3);
    UI_CHECK(ui.rect(sp).w >= std::floor(ui.measureString("All", ui.computed(sp).fontSize)) + 20.f);
    w::setSpecialValueText(ui, sp, {});
    UI_CHECK(w::spinBoxText(ui, sp, scratch) == "0");
}

// ---- progress_indeterminate ------------------------------------------------------------------------------------------
Rect chunkOf(const UiContext& ui, NodeHandle bar) {
    Rect r;
    for (const DrawCmd& c : ui.drawList().cmds())
        if (c.op == DrawOp::Rect && c.node == bar.index) r = c.rect; // the last rect is the chunk
    return r;
}
void gateProgressIndeterminate() {
    UiContext ui{{.viewport = {300, 100}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    ui.setClock({nullptr, &fakeNow});
    g_now = 0.0;
    const NodeHandle bar = w::progressBar(ui, ui.root(), 0.5f, "bar");
    ui.setCss(bar, "width", "200px");
    settle(ui);
    UI_CHECK(!ui.needsUpdate());
    const Rect det = chunkOf(ui, bar);
    w::setProgressIndeterminate(ui, bar, true);
    UI_CHECK(w::progressIndeterminate(ui, bar) && ui.hasClass(bar, "indeterminate"));
    ui.update(0.f);
    const Rect r0 = chunkOf(ui, bar);
    UI_CHECK(r0.w > 0.f && r0.w != det.w);
    g_now = 0.4;
    ui.update(1.f / 30.f); // the repaint timer fires
    UI_CHECK(ui.stats().repainted);
    const Rect r1 = chunkOf(ui, bar);
    UI_CHECK(r1.x != r0.x && r1.w == r0.w);
    UI_CHECK(ui.needsUpdate() && ui.nextWakeSeconds() <= 1.f / 30.f + 1e-4f);
    w::setProgressIndeterminate(ui, bar, false);
    for (int i = 0; i < 4; ++i) ui.update(1.f / 30.f);
    UI_CHECK(!ui.needsUpdate());
    UI_CHECK(chunkOf(ui, bar).w == det.w);
    // destroyed while busy: the timer stops by itself
    w::setProgressIndeterminate(ui, bar, true);
    ui.destroy(bar);
    for (int i = 0; i < 4; ++i) ui.update(1.f / 30.f);
    UI_CHECK(!ui.needsUpdate());
}

// ---- cooked_font_props -----------------------------------------------------------------------------------------------
void gateCookedFontProps() {
#if defined(FUSE_UI_EXTRAS_HAVE_DOC)
    const char* css = ":root { --mono: monospace; } .code { font-family: \"JetBrains Mono\", monospace; font-weight: bold; "
                      "placeholder-color: #808080; } .x { font-family: var(--mono); }";
    auto cooked = fuse::ui::doc::cookStyle(css);
    UI_CHECK(cooked.has_value());
    if (!cooked) return;
    auto sheet = fuse::ui::doc::loadCookedStyle(*cooked);
    UI_CHECK(sheet.has_value());
    if (!sheet) return;
    auto parsed = StyleSheet::parse(css);
    UI_CHECK(parsed.has_value() && parsed->rules().size() == sheet->rules().size());
    bool family = false, weight = false, custom = false, var = false;
    for (const StyleRule& r : sheet->rules())
        for (const Declaration& d : r.decls) {
            if (d.prop == Prop::FontFamily && d.raw.empty())
                family = d.value.identCount == 2 && d.value.idents[0] == fontFamilyHash("jetbrains mono") &&
                         d.value.idents[1] == fontFamilyHash("monospace");
            if (d.prop == Prop::FontFamily && !d.raw.empty()) var = true;
            if (d.prop == Prop::FontWeight) weight = d.value.num == 700.f;
            if (d.prop == Prop::Count) custom = d.customHash == hashId("--mono");
        }
    UI_CHECK(family && weight && custom && var);
    // the cooked sheet styles nodes like the parsed one
    UiContext ui{{.viewport = {200, 100}, .font = &fuse::ui::test::font()}};
    w::install(ui);
    ui.registerBundledFonts();
    ui.addStyleSheet(std::move(*sheet));
    const NodeHandle a = w::label(ui, ui.root(), "a", "a");
    ui.addClass(a, "code");
    const NodeHandle b = w::label(ui, ui.root(), "b", "b");
    ui.addClass(b, "x");
    settle(ui);
    UI_CHECK(ui.fontFor(a) == ui.resolveFont("monospace", 700) && ui.fontFor(b) == ui.resolveFont("monospace"));
#else
    std::printf("cooked_font_props: fuse_ui_doc not built\n");
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::string gate;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
    struct G { const char* name; void (*fn)(); };
    static const G kGates[] = {{"font_faces", gateFontFaces},
                               {"glyph_fallback", gateGlyphFallback},
                               {"placeholder", gatePlaceholder},
                               {"double_click", gateDoubleClick},
                               {"table_multiselect", gateTableMultiSelect},
                               {"spinbox_special", gateSpinBoxSpecial},
                               {"progress_indeterminate", gateProgressIndeterminate},
                               {"cooked_font_props", gateCookedFontProps}};
    bool ran = false;
    for (const G& g : kGates)
        if (gate.empty() || gate == g.name) {
            const int before = fuse::ui::test::failures();
            g.fn();
            std::printf("%s: %s\n", g.name, fuse::ui::test::failures() == before ? "ok" : "FAILED");
            ran = true;
        }
    if (!ran) {
        std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str());
        return 2;
    }
    return fuse::ui::test::finish("fuse_ui_extras");
}
