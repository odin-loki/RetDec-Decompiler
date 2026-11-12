// Shared helpers for the FUSE UI gate tests: CHECK macros, a scripted-input driver (UiTestDriver: click / type /
// key by widget id, plan §8 "scripted input by widget id"), the bundled font and the sample widget tree.
#pragma once

#include "fuse/text/text.hpp"
#include "fuse/ui/ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace fuse::ui::test {

inline int& failures() {
    static int f = 0;
    return f;
}

#define UI_CHECK(cond)                                                                                                 \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++::fuse::ui::test::failures();                                                                            \
        }                                                                                                              \
    } while (0)
#define UI_CHECK_EQ(a, b)                                                                                              \
    do {                                                                                                               \
        const auto va_ = (a);                                                                                          \
        const auto vb_ = (b);                                                                                          \
        if (!(va_ == vb_)) {                                                                                           \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b);                    \
            ++::fuse::ui::test::failures();                                                                            \
        }                                                                                                              \
    } while (0)
#define UI_CHECK_RECT(r, X, Y, W, H)                                                                                   \
    do {                                                                                                               \
        const ::fuse::ui::Rect rr_ = (r);                                                                              \
        if (!(rr_.x == f32(X) && rr_.y == f32(Y) && rr_.w == f32(W) && rr_.h == f32(H))) {                             \
            std::fprintf(stderr, "%s:%d: rect %s = (%g %g %g %g), want (%g %g %g %g)\n", __FILE__, __LINE__, #r,       \
                         double(rr_.x), double(rr_.y), double(rr_.w), double(rr_.h), double(X), double(Y), double(W),  \
                         double(H));                                                                                   \
            ++::fuse::ui::test::failures();                                                                            \
        }                                                                                                              \
    } while (0)

inline int finish(const char* name) {
    if (failures()) {
        std::fprintf(stderr, "%s: %d failure(s)\n", name, failures());
        return 1;
    }
    std::printf("%s: OK\n", name);
    return 0;
}

inline const fuse::text::Font& font() {
    static fuse::text::Font f = [] {
        auto r = fuse::text::Font::bundled();
        if (!r) {
            std::fprintf(stderr, "bundled font missing\n");
            std::exit(2);
        }
        return std::move(*r);
    }();
    return f;
}

/// Scripted input by widget id.
class UiTestDriver {
public:
    explicit UiTestDriver(UiContext& ui) : m_ui(ui) {}
    Vec2 center(std::string_view id) const {
        const Rect r = m_ui.rect(m_ui.find(id));
        return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    }
    void click(std::string_view id) { clickAt(center(id)); }
    void clickAt(Vec2 p) {
        m_ui.pointerMove(p);
        m_ui.pointerDown(p);
        m_ui.pointerUp(p);
        m_ui.update(1.f / 60.f);
    }
    void hover(std::string_view id) {
        m_ui.pointerMove(center(id));
        m_ui.update(1.f / 60.f);
    }
    void type(std::string_view utf8) {
        m_ui.textInput(utf8);
        m_ui.update(1.f / 60.f);
    }
    void key(Key k, u8 mods = 0) {
        m_ui.keyDown(k, mods);
        m_ui.keyUp(k, mods);
        m_ui.update(1.f / 60.f);
    }

private:
    UiContext& m_ui;
};

/// The sample / golden widget tree (Tier A widgets on a card).
inline void buildSample(UiContext& ui) {
    using namespace widgets;
    ui.setCss(ui.root(), "padding", "24px");
    const NodeHandle card = panel(ui, ui.root(), "card");
    ui.addClass(card, "card");
    ui.setCss(card, "width", "440px");
    const NodeHandle t = label(ui, card, "FUSE UI \xE2\x80\x94 Tier A widgets", "title");
    ui.addClass(t, "title");
    label(ui, card, "Retained core, CSS-subset themes, CPU reference rasteriser.", "subtitle");
    ui.addClass(ui.find("subtitle"), "dim");
    const NodeHandle r1 = panel(ui, card, "row1");
    ui.addClass(r1, "row");
    const NodeHandle ok = button(ui, r1, "Apply", "apply");
    ui.addClass(ok, "primary");
    button(ui, r1, "Cancel", "cancel");
    toolButton(ui, r1, "Tool", "tool");
    link(ui, r1, "Open docs", "docs");
    separator(ui, card, "sep1");
    const NodeHandle r2 = panel(ui, card, "row2");
    ui.addClass(r2, "row");
    checkBox(ui, r2, "Shadows", true, "shadows");
    checkBox(ui, r2, "Bloom", false, "bloom");
    toggleSwitch(ui, r2, "V-Sync", true, "vsync");
    const NodeHandle r3 = panel(ui, card, "row3");
    ui.addClass(r3, "row");
    radioButton(ui, r3, "Low", false, "q.low");
    radioButton(ui, r3, "High", true, "q.high");
    radioButton(ui, r3, "Ultra", false, "q.ultra");
    const NodeHandle r4 = panel(ui, card, "row4");
    ui.addClass(r4, "row");
    label(ui, r4, "Exposure", "exposure.label");
    const NodeHandle s = slider(ui, r4, 0.f, 1.f, 0.62f, 0.f, "exposure");
    ui.setCss(s, "flex-grow", "1");
    dial(ui, r4, 0.f, 100.f, 35.f, "gain");
    const NodeHandle r5 = panel(ui, card, "row5");
    ui.addClass(r5, "row");
    spinBox(ui, r5, 0.f, 240.f, 144.f, 1.f, "fps");
    comboBox(ui, r5, "Vulkan\nCPU raster\nNull", 0, "backend");
    const NodeHandle le = lineEdit(ui, r5, "player_one", "name");
    ui.setCss(le, "flex-grow", "1");
    progressBar(ui, card, 0.7f, "progress");
    const NodeHandle r6 = panel(ui, card, "row6");
    ui.addClass(r6, "row");
    label(ui, r6, "Scroll", "scroll.label");
    const NodeHandle sb = scrollBar(ui, r6, false, "hscroll");
    ui.setCss(sb, "flex-grow", "1");
    ui.setValue(sb, 0.3f);
}

} // namespace fuse::ui::test

namespace fuse::ui::test {
inline void render(UiContext& ui, Image& img) {
    img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(ui.drawList(), img);
}
} // namespace fuse::ui::test
