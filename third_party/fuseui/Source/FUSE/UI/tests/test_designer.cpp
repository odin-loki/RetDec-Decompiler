// UI8 designer prototype gate (fuse_ui_designer_roundtrip):
//   - DesignDocument: load -> save -> load is a fixed point, and every member (bindings, "on" actions, items,
//     unknown keys) survives; the saved text loads in fuse_ui_doc
//   - edits: insert / set property / move / remove with undo / redo back to the identical text
//   - Designer UI: canvas click selects (and does not operate the widget), property edits rebuild the live preview,
//     palette inserts, outline follows the selection
// Optional --out=<dir>: designer.png.
#include "test_util.hpp"

#include "fuse/ui/designer/designer.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;
namespace dz = fuse::ui::designer;

namespace {
const char* kDoc = R"({
  "fuseui": 1,
  "style": ".settings { gap: 6px; padding: 12px; } .hp { color: #ffcc66; }",
  "root": {
    "class": "panel", "id": "settings", "classes": ["settings", "card"],
    "style": { "width": 320 },
    "children": [
      { "class": "label", "id": "hp", "classes": ["hp"], "text": "HP {player.health / player.maxHealth * 100:0}%" },
      { "class": "slider", "id": "vol", "min": 0, "max": 1, "value": "{=audio.volume}" },
      { "class": "checkbox", "id": "mute", "text": "Mute", "checked": "{=audio.mute}" },
      { "class": "combobox", "id": "quality", "items": ["Low", "High", "Ultra"], "selected": 1, "x-editor-note": "kept" },
      { "class": "button", "id": "save", "text": "Save", "disabled": "{busy}", "on": { "clicked": "save" } }
    ]
  }
})";
void step(UiContext& ui) { ui.update(1.f / 60.f); }
void click(UiContext& ui, Vec2 p) { ui.pointerMove(p); ui.pointerDown(p); ui.pointerUp(p); step(ui); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
} // namespace

int main(int argc, char** argv) {
    std::string out;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;

    // ---- model round trip
    dz::DesignDocument d;
    std::string err;
    UI_CHECK(d.load(kDoc, &err));
    UI_CHECK_EQ(d.count(), 6u);
    const std::string s1 = d.save();
    dz::DesignDocument d2;
    UI_CHECK(d2.load(s1));
    UI_CHECK_EQ(d2.save(), s1); // fixed point
    for (const char* kept : {"{=audio.volume}", "\"clicked\": \"save\"", "\"Ultra\"", "x-editor-note", "HP {player.health", ".hp { color"})
        UI_CHECK(s1.find(kept) != std::string::npos);
    UI_CHECK_EQ(d.prop(d.findId("quality"), "items"), std::string("Low High Ultra"));
    UI_CHECK_EQ(d.prop(0, "style.width"), std::string("320"));
    {
        UiContext ui{{.viewport = {400, 300}, .font = &font()}};
        w::install(ui);
        doc::Model m;
        auto live = doc::Document::load(ui, d.previewText(), &m);
        UI_CHECK(doc::Document::load(ui, s1, &m).has_value() == false); // x-* keys are editor-only
        if (!live) std::printf("doc error: %s: %s\n", live.error().path.c_str(), live.error().message.c_str());
        UI_CHECK(live.has_value());
    }
    // edits + undo / redo
    const u32 lbl = d.insert(0, "label");
    UI_CHECK(lbl != dz::DesignDocument::kNone && d.node(lbl)->id() == "label1" && d.parentOf(lbl) == 0u);
    UI_CHECK(d.setProp(lbl, "text", [] { doc::JsonValue v; v.type = doc::JsonValue::Type::String; v.string = "Hello"; return v; }()));
    UI_CHECK(d.setProp(lbl, "style.padding", [] { doc::JsonValue v; v.type = doc::JsonValue::Type::String; v.string = "4px"; return v; }()));
    const u32 moved = d.move(lbl, -1);
    UI_CHECK(moved == lbl - 1u && d.node(moved)->id() == "label1");
    UI_CHECK(d.remove(d.findId("mute")));
    UI_CHECK(d.findId("mute") == dz::DesignDocument::kNone);
    UI_CHECK(!d.remove(0)); // root
    int undos = 0;
    while (d.undo()) ++undos;
    UI_CHECK_EQ(undos, 5);
    UI_CHECK_EQ(d.save(), s1);
    for (int i = 0; i < undos; ++i) UI_CHECK(d.redo());
    UI_CHECK(d.findId("mute") == dz::DesignDocument::kNone && d.prop(d.findId("label1"), "text") == "Hello");
    // reparent / insertAt / removeMany / style sheet
    {
        dz::DesignDocument m;
        UI_CHECK(m.load(R"({"fuseui":1,"root":{"class":"panel","id":"r","children":[
            {"class":"panel","id":"a","children":[{"class":"label","id":"a1"},{"class":"label","id":"a2"}]},
            {"class":"panel","id":"b"},{"class":"button","id":"c"}]}})"));
        const std::string base = m.save();
        UI_CHECK(m.reparent(m.findId("c"), m.findId("a"), 1) != dz::DesignDocument::kNone);
        UI_CHECK(m.parentOf(m.findId("c")) == m.findId("a") && m.childSlot(m.findId("c")) == 1u);
        // into a later sibling's subtree (the target's path shifts after the erase)
        UI_CHECK(m.reparent(m.findId("a"), m.findId("b"), 0) != dz::DesignDocument::kNone);
        UI_CHECK(m.parentOf(m.findId("a")) == m.findId("b") && m.childCount(m.findId("a")) == 3u);
        UI_CHECK(m.reparent(m.findId("b"), m.findId("a1"), 0) == dz::DesignDocument::kNone); // leaf
        UI_CHECK(m.reparent(m.findId("b"), m.findId("a"), 0) == dz::DesignDocument::kNone);  // own subtree
        UI_CHECK(m.reparent(0, m.findId("b"), 0) == dz::DesignDocument::kNone);              // root
        // same parent, later slot
        UI_CHECK(m.reparent(m.findId("a1"), m.findId("a"), 3) != dz::DesignDocument::kNone);
        UI_CHECK(m.childSlot(m.findId("a1")) == 2u);
        const u32 ins = m.insertAt(m.findId("b"), 0, "slider");
        UI_CHECK(ins != dz::DesignDocument::kNone && m.childSlot(ins) == 0u && m.parentOf(ins) == m.findId("b"));
        UI_CHECK(m.insertAt(m.findId("a2"), 0, "label") == dz::DesignDocument::kNone);
        UI_CHECK_EQ(m.removeMany({m.findId("a"), m.findId("a2"), m.findId("slider1"), 0}), 2u); // a2 under a; root skipped
        UI_CHECK(m.findId("a2") == dz::DesignDocument::kNone && m.findId("slider1") == dz::DesignDocument::kNone);
        UI_CHECK(m.setStyleSheet(".x { gap: 2px; }") && m.styleSheet() == ".x { gap: 2px; }");
        UI_CHECK(!m.setStyleSheet(".x { gap: 2px; }"));
        m.beginBatch();
        m.setProp(m.findId("b"), "text", [] { doc::JsonValue v; v.type = doc::JsonValue::Type::String; v.string = "t"; return v; }());
        m.setProp(m.findId("r"), "text", [] { doc::JsonValue v; v.type = doc::JsonValue::Type::String; v.string = "t"; return v; }());
        m.endBatch();
        int steps = 0;
        while (m.undo()) ++steps;
        UI_CHECK_EQ(steps, 7); // 3 reparents + insert + removeMany + style + one batch
        UI_CHECK_EQ(m.save(), base);
    }
    UI_CHECK(!d.load("{ \"root\": [1, 2] }", &err) && !err.empty());
    UI_CHECK(!d.load("{ \"fuseui\": 1, ", &err) && err.find("line") == 0);

    // ---- designer UI
    UiContext ui{{.viewport = {1280, 720}, .font = &font()}};
    ui.setCss(ui.root(), "padding", "0px");
    dz::Designer de(ui);
    UI_CHECK(de.open(kDoc));
    step(ui);
    const u32 saveIdx = de.document().findId("save");
    const NodeHandle saveBtn = de.canvasNode(saveIdx);
    UI_CHECK(ui.alive(saveBtn));
    UI_CHECK(ui.rect(saveBtn).w > 0.f);
    // the canvas is in design mode: clicking the button selects it, it does not click
    int clicked = 0;
    ui.connect(saveBtn, SignalKind::Clicked, [&clicked](UiContext&, NodeHandle, const SignalArgs&) { ++clicked; });
    click(ui, ctr(ui.rect(saveBtn)));
    UI_CHECK_EQ(de.selected(), saveIdx);
    UI_CHECK_EQ(clicked, 0);
    UI_CHECK(ui.hasClass(saveBtn, "dsel"));
    UI_CHECK(u32(ui.value(de.outline())) == saveIdx);
    UI_CHECK(w::propertyCount(ui, de.properties()) >= 10u);
    UI_CHECK_EQ(w::propertyValue(ui, de.properties(), 2), std::string("Save")); // text row
    // a property edit rebuilds the live preview
    UI_CHECK(de.setProperty("text", "Apply"));
    step(ui);
    UI_CHECK_EQ(std::string(ui.text(de.canvasNode(de.document().findId("save")))), std::string("Apply"));
    UI_CHECK(de.setProperty("width", "140px"));
    step(ui);
    UI_CHECK(std::abs(ui.rect(de.canvasNode(de.document().findId("save"))).w - 140.f) < 0.5f);
    // clicking a combobox selects it too (and does not open its popup)
    const NodeHandle combo = de.canvasNode(de.document().findId("quality"));
    const u32 rootKids = ui.childCount(ui.root());
    click(ui, ctr(ui.rect(combo)));
    UI_CHECK_EQ(de.selected(), de.document().findId("quality"));
    UI_CHECK_EQ(ui.childCount(ui.root()), rootKids); // no overlay / popup was created
    // palette: select the root panel and add a slider and a button via the palette list (Enter)
    de.select(0);
    UI_CHECK(de.addWidget("slider") != dz::DesignDocument::kNone);
    const NodeHandle pal = de.palette();
    ui.setFocus(pal);
    ui.setValue(pal, 2.f); // "button"
    ui.keyDown(Key::Enter); ui.keyUp(Key::Enter);
    step(ui);
    UI_CHECK(de.document().findId("button1") != dz::DesignDocument::kNone);
    UI_CHECK(de.document().findId("slider1") != dz::DesignDocument::kNone);
    UI_CHECK(ui.alive(de.canvasNode(de.document().findId("button1"))));
    // undo twice removes both, redo restores
    de.undo(); de.undo();
    UI_CHECK(de.document().findId("button1") == dz::DesignDocument::kNone && de.document().findId("slider1") == dz::DesignDocument::kNone);
    de.redo(); de.redo();
    UI_CHECK(de.document().findId("button1") != dz::DesignDocument::kNone);
    // the saved text is a valid document again and round-trips
    const std::string saved = de.text();
    dz::DesignDocument d3;
    UI_CHECK(d3.load(saved));
    UI_CHECK_EQ(d3.save(), saved);
    UI_CHECK(saved.find("\"text\": \"Apply\"") != std::string::npos);
    // ---- drag-to-place: drag the "save" button above the "hp" label
    {
        step(ui);
        const NodeHandle sb = de.canvasNode(de.document().findId("save"));
        const NodeHandle hp = de.canvasNode(de.document().findId("hp"));
        const Rect hr = ui.rect(hp);
        const Vec2 from = ctr(ui.rect(sb)), to{hr.x + 4.f, hr.y + 1.f};
        int clicks = 0;
        ui.connect(sb, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
        ui.pointerMove(from); ui.pointerDown(from);
        for (int k = 1; k <= 6; ++k) { ui.pointerMove({from.x + (to.x - from.x) * k / 6.f, from.y + (to.y - from.y) * k / 6.f}); step(ui); }
        UI_CHECK(de.dragging());
        ui.pointerUp(to); step(ui);
        UI_CHECK_EQ(clicks, 0);
        const u32 s2 = de.document().findId("save");
        UI_CHECK(de.document().parentOf(s2) == 0u && de.document().childSlot(s2) == 0u);
        UI_CHECK_EQ(de.selected(), s2);
        de.undo();
        UI_CHECK(de.document().childSlot(de.document().findId("save")) > 0u);
        de.redo();
    }
    // ---- palette drag: drag "checkbox" from the palette onto the canvas below the last child
    {
        const NodeHandle pal = de.palette();
        const Rect pr = ui.rect(pal);
        const Vec2 from{pr.x + 30.f, pr.y + 4.f + 16.f * 3.5f}; // 4th row ("checkbox"), rows ~16px
        ui.pointerMove(from); ui.pointerDown(from); ui.pointerUp(from); step(ui); // select the row first
        const std::string picked(w::listItem(ui, pal, u32(ui.value(pal))));
        const Rect rr = ui.rect(de.canvasNode(0));
        const Vec2 to{rr.x + rr.w * 0.5f, rr.y + rr.h - 3.f};
        ui.pointerDown(from);
        for (int k = 1; k <= 6; ++k) { ui.pointerMove({from.x + (to.x - from.x) * k / 6.f, from.y + (to.y - from.y) * k / 6.f}); step(ui); }
        ui.pointerUp(to); step(ui);
        const u32 added = de.selected();
        UI_CHECK(std::string(de.document().node(added)->cls()) == picked);
        UI_CHECK(de.document().parentOf(added) == 0u && de.document().childSlot(added) + 1u == de.document().childCount(0));
        UI_CHECK(ui.alive(de.canvasNode(added)));
    }
    // ---- multi-select: Ctrl+click two widgets, one property edit applies to both, one undo reverts both
    {
        const u32 vol = de.document().findId("vol"), hp = de.document().findId("hp");
        click(ui, ctr(ui.rect(de.canvasNode(vol))));
        const Vec2 p = ctr(ui.rect(de.canvasNode(hp)));
        ui.pointerMove(p);
        ui.setModifiers(kModCtrl);
        ui.pointerDown(p); ui.pointerUp(p); step(ui);
        ui.setModifiers(0);
        UI_CHECK_EQ(de.selection().size(), std::size_t(2));
        UI_CHECK(ui.hasClass(de.canvasNode(vol), "dsel") && ui.hasClass(de.canvasNode(hp), "dsel"));
        UI_CHECK(de.setProperty("margin", "4px"));
        UI_CHECK(de.document().prop(de.document().findId("vol"), "style.margin") == "4px");
        UI_CHECK(de.document().prop(de.document().findId("hp"), "style.margin") == "4px");
        UI_CHECK_EQ(de.selection().size(), std::size_t(2));
        de.undo();
        UI_CHECK(de.document().prop(de.document().findId("vol"), "style.margin").empty());
        UI_CHECK(de.document().prop(de.document().findId("hp"), "style.margin").empty());
        de.redo();
        // shift-click toggles one off again
        de.select(de.document().findId("vol"));
        de.select(de.document().findId("hp"), true);
        de.select(de.document().findId("hp"), true);
        UI_CHECK_EQ(de.selection().size(), std::size_t(1));
    }
    // ---- style sheet editing
    {
        const std::string css = de.document().styleSheet() + " .hp { color: #66ddff; font-size: 18px; }";
        w::codeEditSetText(ui, de.styleEditor(), css);
        ui.find("designer.applystyle");
        click(ui, ctr(ui.rect(ui.find("designer.applystyle"))));
        UI_CHECK_EQ(de.document().styleSheet(), css);
        UI_CHECK(de.text().find("#66ddff") != std::string::npos);
        UI_CHECK(ui.rect(de.canvasNode(de.document().findId("hp"))).h >= 18.f);
    }
    // select the edited button for the screenshot (plus the label, multi-select)
    de.select(de.document().findId("save"));
    de.select(de.document().findId("hp"), true);
    step(ui);

    // the side columns fill the body height
    UI_CHECK(ui.rect(ui.find("designer.outline")).y + ui.rect(ui.find("designer.outline")).h > ui.rect(ui.find("designer.body")).y + ui.rect(ui.find("designer.body")).h - 12.f);
    if (!out.empty()) {
        ui.update();
        Image img;
        img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
        CpuRasterizer r;
        r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r.render(ui.drawList(), img);
        const std::string path = out + "/designer.png";
        stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("wrote %s\n", path.c_str());
    }
    return finish("fuse_ui_designer_roundtrip");
}
