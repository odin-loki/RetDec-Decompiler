// Gate fuse_ui_style_cascade: specificity, source order, sheet order, inline wins, inheritance, pseudo-classes,
// combinators, theme tokens (var()), restyle on class / state change, diagnostics, computed-style dedup.
#include "test_util.hpp"

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {200, 200}});
    const u32 panel = ui.registry().find("panel"), label = ui.registry().find("label");
    std::vector<StyleDiagnostic> diags;
    auto r = ui.addStyleSheet(R"css(
:root { --accent: #ff0000; --pad: 7px; color: #00ff00; font-size: 20px; }
panel { background: #111111; padding: var(--pad); }
.a { background: #222222; }
panel.a { background: #333333; }
#x { background: #444444; }
.a .b { color: var(--accent); }
.a > label { font-size: 30px; }
panel:hover { border-color: #abcdef; }
.b { bogus-prop: 3; width: nonsense; }
.tok { --accent: #0000ff; }
.tok label { color: var(--accent); }
.fallback { color: var(--missing, #123456); }
)css", &diags);
    UI_CHECK(r.has_value());
    UI_CHECK_EQ(diags.size(), std::size_t(2)); // unknown property + bad value

    const NodeHandle p = ui.create(panel, ui.root());
    const NodeHandle pa = ui.create(panel, ui.root());
    ui.addClass(pa, "a");
    const NodeHandle px = ui.create(panel, ui.root(), "x");
    ui.addClass(px, "a");
    const NodeHandle lb = ui.create(label, pa);
    ui.addClass(lb, "b");
    const NodeHandle deep = ui.create(panel, pa);
    const NodeHandle deepLabel = ui.create(label, deep);
    const NodeHandle tok = ui.create(panel, ui.root());
    ui.addClass(tok, "tok");
    const NodeHandle tokLabel = ui.create(label, tok);
    const NodeHandle fb = ui.create(label, ui.root());
    ui.addClass(fb, "fallback");
    ui.update();

    UI_CHECK(ui.computed(p).background == Color::rgb(0x111111));
    UI_CHECK(ui.computed(pa).background == Color::rgb(0x333333)); // type+class beats class
    UI_CHECK(ui.computed(px).background == Color::rgb(0x444444)); // id beats all
    UI_CHECK_EQ(ui.computed(p).padding.l, 7.f);                  // var() token
    UI_CHECK(ui.computed(lb).color == Color::rgb(0xff0000));     // descendant + var
    UI_CHECK_EQ(ui.computed(lb).fontSize, 30.f);                  // child combinator
    UI_CHECK_EQ(ui.computed(deepLabel).fontSize, 20.f);           // not a direct child → inherited
    UI_CHECK(ui.computed(deepLabel).color == Color::rgb(0x00ff00)); // inherited from :root
    // Tokens are global (defined in :root); a custom property set in a non-root rule is ignored (scoped tokens deferred).
    UI_CHECK(ui.computed(tokLabel).color == Color::rgb(0xff0000));
    UI_CHECK(ui.computed(fb).color == Color::rgb(0x123456));     // var() fallback

    // inline wins over everything
    ui.setCss(px, "background", "#555555");
    ui.update();
    UI_CHECK(ui.computed(px).background == Color::rgb(0x555555));
    ui.clearInline(px);
    ui.update();
    UI_CHECK(ui.computed(px).background == Color::rgb(0x444444));

    // pseudo-class via state; class removal restyles descendants
    ui.setState(p, kStateHover, true);
    ui.update();
    UI_CHECK(ui.computed(p).borderColor == Color::rgb(0xabcdef));
    ui.removeClass(pa, "a");
    ui.update();
    UI_CHECK(ui.computed(lb).color == Color::rgb(0x00ff00));
    UI_CHECK_EQ(ui.computed(lb).fontSize, 20.f);

    // later sheet with equal specificity wins; computed styles are deduplicated
    ui.addStyleSheet("panel { background: #999999; }");
    ui.update();
    UI_CHECK(ui.computed(p).background == Color::rgb(0x999999));
    const u32 before = ui.styles().tableSize();
    for (int i = 0; i < 100; ++i) ui.create(panel, ui.root());
    ui.update();
    UI_CHECK(ui.styles().tableSize() <= before + 1);

    // syntax error fails the whole sheet
    std::vector<StyleDiagnostic> d2;
    UI_CHECK(!ui.addStyleSheet("panel { background: #fff", &d2).has_value());
    return finish("fuse_ui_style_cascade");
}
