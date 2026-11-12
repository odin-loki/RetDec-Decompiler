// UI6 Tier D (2) widget gates, one ctest entry per widget (--gate=<name>; no argument runs all):
//   font_picker  file_dialog  node_graph  curve_timeline  viewport  code_edit  datetime_edit  video
// Optional --out=<dir>: tier_d2.png (all widgets on one screen) and file_dialog.png.
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
std::string g_out;
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void step(UiContext& ui) { ui.update(1.f / 60.f); }
void click(UiContext& ui, Vec2 p) { ui.pointerMove(p); ui.pointerDown(p); ui.pointerUp(p); step(ui); }
void drag(UiContext& ui, Vec2 a, Vec2 b) {
    ui.pointerMove(a); ui.pointerDown(a); step(ui);
    ui.pointerMove({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}); step(ui);
    ui.pointerMove(b); step(ui);
    ui.pointerUp(b); step(ui);
}
void key(UiContext& ui, Key k, u8 mods = 0) { ui.keyDown(k, mods); ui.keyUp(k, mods); step(ui); }
void type(UiContext& ui, std::string_view s) { ui.textInput(s); step(ui); }

Image g_scene; // the "engine" image the viewport shows
std::vector<const Image*> g_images;
void shot(UiContext& ui, const char* name) {
    if (g_out.empty()) return;
    ui.update();
    Image img;
    img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    for (const Image* im : g_images) r.addImage(im);
    r.render(ui.drawList(), img);
    const std::string path = g_out + "/" + name;
    stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    std::printf("wrote %s\n", path.c_str());
}
NodeHandle column(UiContext& ui, NodeHandle parent, f32 width) {
    const NodeHandle c = w::panel(ui, parent);
    char b[32];
    std::snprintf(b, sizeof b, "%gpx", double(width));
    ui.setCss(c, "width", b);
    ui.setCss(c, "gap", "8px");
    return c;
}
struct Screen {
    UiContext ui{{.viewport = {1500, 900}, .font = &font()}};
    NodeHandle c1, c2, c3;
    Screen() {
        w::install(ui);
        ui.setCss(ui.root(), "flex-direction", "row");
        ui.setCss(ui.root(), "gap", "10px");
        ui.setCss(ui.root(), "padding", "10px");
        c1 = column(ui, ui.root(), 470.f);
        c2 = column(ui, ui.root(), 520.f);
        c3 = column(ui, ui.root(), 470.f);
    }
};

// ---- font picker ------------------------------------------------------------------------------------------------------
int fontPicker(Screen& s) {
    UiContext& ui = s.ui;
    const NodeHandle fp = w::fontPicker(ui, s.c1, {}, 16.f, "fonts");
    std::string fam;
    f32 size = 0.f;
    ui.connect(fp, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { fam = a.text; size = a.value; });
    step(ui);
    UI_CHECK_EQ(w::fontPickerFamily(ui, fp), std::string("Open Sans"));
    UI_CHECK(w::fontPickerSize(ui, fp) == 16.f);
    NodeHandle list = ui.firstChild(fp);
    ui.setFocus(list);
    key(ui, Key::Down);
    UI_CHECK_EQ(fam, std::string("Sans Serif"));
    NodeHandle spin = ui.nextSibling(ui.firstChild(ui.nextSibling(list)));
    ui.setFocus(spin);
    key(ui, Key::Up);
    key(ui, Key::Up);
    UI_CHECK(size == 18.f && w::fontPickerSize(ui, fp) == 18.f);
    const NodeHandle preview = ui.nextSibling(ui.nextSibling(list));
    UI_CHECK(ui.computed(preview).fontSize == 18.f);
    UI_CHECK(ui.text(preview).starts_with("Sans Serif - The quick"));
    return 0;
}

// ---- file dialog -------------------------------------------------------------------------------------------------------
int fileDialog(Screen& s) {
    namespace fs = std::filesystem;
    UiContext& ui = s.ui;
    // unique per run: ctest -j runs this gate and the all-gates binary concurrently
    const fs::path base = fs::temp_directory_path() /
        ("fuse_ui_fd_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path root = base / "fuse_ui_file_dialog";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "levels" / "act1");
    fs::create_directories(root / "Textures");
    for (const char* f : {"menu.fuseui", "hud.fuseui", "readme.txt", "theme.fusestyle", "levels/act1/intro.fuseui", ".hidden.fuseui"})
        std::ofstream(root / f) << "x";
    const NodeHandle dlg = w::fileDialog(ui, "Open UI document", root.generic_string(), "*.fuseui;*.FUSESTYLE", false, "fd");
    std::string chosen = "unset";
    ui.connect(dlg, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { chosen = a.text; });
    w::openDialog(ui, dlg);
    step(ui);
    UI_CHECK_EQ(w::fileDialogEntryCount(ui, dlg), 5u); // folders first (case-insensitive), filtered files, no dotfiles
    UI_CHECK_EQ(w::fileDialogEntry(ui, dlg, 0), std::string("levels/"));
    UI_CHECK_EQ(w::fileDialogEntry(ui, dlg, 1), std::string("Textures/"));
    UI_CHECK_EQ(w::fileDialogEntry(ui, dlg, 2), std::string("hud.fuseui"));
    UI_CHECK_EQ(w::fileDialogEntry(ui, dlg, 4), std::string("theme.fusestyle"));
    shot(ui, "file_dialog.png");
    // enter a folder, accept a file by activation
    UI_CHECK(w::fileDialogActivate(ui, dlg, 0).empty());
    UI_CHECK(w::fileDialogDirectory(ui, dlg).ends_with("fuse_ui_file_dialog/levels"));
    UI_CHECK_EQ(w::fileDialogEntryCount(ui, dlg), 1u);
    w::fileDialogActivate(ui, dlg, 0);
    UI_CHECK(w::fileDialogDirectory(ui, dlg).ends_with("levels/act1"));
    const std::string got = w::fileDialogActivate(ui, dlg, 0);
    UI_CHECK(got.ends_with("levels/act1/intro.fuseui") && chosen == got);
    step(ui);
    UI_CHECK(!ui.computed(ui.parent(dlg)).visible);
    // keyboard path: reopen at the root, select with arrows, Enter on the list accepts
    w::fileDialogNavigate(ui, dlg, root.generic_string());
    w::openDialog(ui, dlg);
    step(ui);
    NodeHandle body = w::dialogBody(ui, dlg);
    NodeHandle list = ui.nextSibling(ui.firstChild(body));
    ui.setFocus(list);
    key(ui, Key::Down); key(ui, Key::Down); key(ui, Key::Down); key(ui, Key::Down); // -> menu.fuseui
    NodeHandle nameEdit = ui.nextSibling(ui.firstChild(ui.nextSibling(list)));
    UI_CHECK_EQ(std::string(ui.text(nameEdit)), std::string("menu.fuseui"));
    key(ui, Key::Enter);
    UI_CHECK(chosen.ends_with("/menu.fuseui"));
    // the Up button, typed names, a missing file is refused in open mode, accepted in save mode; Cancel reports ""
    w::fileDialogNavigate(ui, dlg, (root / "levels").generic_string());
    w::openDialog(ui, dlg);
    step(ui);
    click(ui, ctr(ui.rect(ui.firstChild(ui.firstChild(body)))));
    UI_CHECK(w::fileDialogDirectory(ui, dlg).ends_with("fuse_ui_file_dialog"));
    chosen = "unset";
    ui.setText(nameEdit, "nope.fuseui");
    NodeHandle ok = ui.firstChild(ui.nextSibling(ui.nextSibling(list)));
    click(ui, ctr(ui.rect(ok)));
    UI_CHECK_EQ(chosen, std::string("unset"));
    click(ui, ctr(ui.rect(ui.nextSibling(ok))));
    UI_CHECK(chosen.empty());
    const NodeHandle save = w::fileDialog(ui, "Save layout", root.generic_string(), "*.fuseui", true, "fdsave");
    std::string saved;
    ui.connect(save, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { saved = a.text; });
    w::openDialog(ui, save);
    step(ui);
    NodeHandle sbody = w::dialogBody(ui, save);
    NodeHandle sname = ui.nextSibling(ui.firstChild(ui.nextSibling(ui.nextSibling(ui.firstChild(sbody)))));
    ui.setFocus(sname);
    type(ui, "new_layout.fuseui");
    key(ui, Key::Enter);
    UI_CHECK(saved.ends_with("/new_layout.fuseui"));
    UI_CHECK(!w::fileDialogNavigate(ui, save, (root / "missing").generic_string()));
    fs::remove_all(base, ec);
    return 0;
}

// ---- node graph --------------------------------------------------------------------------------------------------------
int nodeGraph(Screen& s) {
    UiContext& ui = s.ui;
    const NodeHandle g = w::nodeGraph(ui, s.c2, "graph");
    ui.setCss(g, "height", "300px");
    const w::GraphPin tex[] = {{"uv", 1}}, texOut[] = {{"rgba", 2}, {"alpha", 3}};
    const w::GraphPin mulIn[] = {{"a", 2}, {"b", 3}}, mulOut[] = {{"out", 2}};
    const w::GraphPin outIn[] = {{"color", 2}, {"opacity", 3}};
    const w::GraphPin uvOut[] = {{"uv", 1}};
    const u32 nUv = w::nodeGraphAddNode(ui, g, "TexCoord", {10, 40}, {}, uvOut);
    const u32 nTex = w::nodeGraphAddNode(ui, g, "Sample Texture", {170, 20}, tex, texOut);
    const u32 nMul = w::nodeGraphAddNode(ui, g, "Multiply", {170, 150}, mulIn, mulOut);
    const u32 nOut = w::nodeGraphAddNode(ui, g, "Material Output", {340, 60}, outIn, {});
    u32 changes = 0;
    ui.connect(g, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs&) { ++changes; });
    step(ui);
    UI_CHECK(w::nodeGraphConnect(ui, g, nUv, 0, nTex, 0));
    UI_CHECK(!w::nodeGraphConnect(ui, g, nTex, 1, nOut, 0));   // alpha (3) -> color (2): type mismatch
    UI_CHECK(!w::nodeGraphConnect(ui, g, nTex, 0, nTex, 0));   // self
    // drag from an output pin to an input pin connects
    drag(ui, w::nodeGraphPinPos(ui, g, nTex, 0, true), w::nodeGraphPinPos(ui, g, nMul, 0, false));
    drag(ui, w::nodeGraphPinPos(ui, g, nTex, 1, true), w::nodeGraphPinPos(ui, g, nMul, 1, false));
    drag(ui, w::nodeGraphPinPos(ui, g, nMul, 0, true), w::nodeGraphPinPos(ui, g, nOut, 0, false));
    UI_CHECK_EQ(w::nodeGraphLinkCount(ui, g), 4u);
    UI_CHECK_EQ(changes, 4u);
    // a second link into an occupied input replaces the first
    UI_CHECK(w::nodeGraphConnect(ui, g, nTex, 0, nOut, 0));
    UI_CHECK_EQ(w::nodeGraphLinkCount(ui, g), 4u);
    UI_CHECK(w::nodeGraphConnect(ui, g, nMul, 0, nOut, 0));
    // cycles are refused: Multiply -> Output exists; Output has no outputs, so build a cycle attempt via pins of the same types
    const w::GraphPin passIn[] = {{"in", 2}}, passOut[] = {{"out", 2}};
    const u32 p1 = w::nodeGraphAddNode(ui, g, "Pass A", {10, 200}, passIn, passOut);
    const u32 p2 = w::nodeGraphAddNode(ui, g, "Pass B", {10, 250}, passIn, passOut);
    UI_CHECK(w::nodeGraphConnect(ui, g, p1, 0, p2, 0));
    UI_CHECK(!w::nodeGraphConnect(ui, g, p2, 0, p1, 0));
    const std::vector<u32> order = w::nodeGraphTopoOrder(ui, g);
    UI_CHECK_EQ(order.size(), 6u);
    auto at = [&](u32 n) { return std::find(order.begin(), order.end(), n) - order.begin(); };
    UI_CHECK(at(nUv) < at(nTex) && at(nTex) < at(nMul) && at(nMul) < at(nOut) && at(p1) < at(p2));
    // dragging a node's header moves it; selection + Delete removes it with its links
    const Vec2 before = w::nodeGraphNodePos(ui, g, p2);
    const Vec2 head{w::nodeGraphPinPos(ui, g, p2, 0, false).x + 40.f, w::nodeGraphPinPos(ui, g, p2, 0, false).y - 18.f};
    drag(ui, head, {head.x + 30.f, head.y + 10.f});
    const Vec2 after = w::nodeGraphNodePos(ui, g, p2);
    UI_CHECK(std::abs(after.x - before.x - 30.f) < 0.5f && std::abs(after.y - before.y - 10.f) < 0.5f);
    UI_CHECK_EQ(i32(ui.value(g)), i32(p2));
    key(ui, Key::Delete);
    UI_CHECK_EQ(w::nodeGraphNodeCount(ui, g), 5u);
    UI_CHECK_EQ(w::nodeGraphLinkCount(ui, g), 4u);
    w::nodeGraphRemoveNode(ui, g, p1);
    // pulling a link off an input and dropping it on empty space disconnects it
    const Vec2 in0 = w::nodeGraphPinPos(ui, g, nOut, 0, false);
    drag(ui, in0, {in0.x + 60.f, in0.y + 140.f});
    UI_CHECK_EQ(w::nodeGraphLinkCount(ui, g), 3u);
    UI_CHECK(w::nodeGraphConnect(ui, g, nMul, 0, nOut, 0));
    // wheel zoom keeps the point under the pointer fixed
    const Vec2 pinBefore = w::nodeGraphPinPos(ui, g, nUv, 0, true);
    ui.pointerMove(pinBefore);
    ui.wheel(pinBefore, {0.f, 1.f});
    step(ui);
    const Vec2 pinAfter = w::nodeGraphPinPos(ui, g, nUv, 0, true);
    UI_CHECK(std::abs(pinAfter.x - pinBefore.x) < 0.5f && std::abs(pinAfter.y - pinBefore.y) < 0.5f);
    ui.wheel(pinBefore, {0.f, -1.f});
    ui.setValue(g, -1.f);
    step(ui);
    return 0;
}

// ---- curve + timeline ---------------------------------------------------------------------------------------------------
int curveTimeline(Screen& s) {
    UiContext& ui = s.ui;
    const NodeHandle cv = w::curveEditor(ui, s.c2, 0.f, 1.f, 0.f, 1.f, "curve");
    ui.setCss(cv, "height", "170px");
    w::curveAddKey(ui, cv, 0.f, 0.f);
    w::curveAddKey(ui, cv, 1.f, 1.f);
    w::curveAddKey(ui, cv, 0.5f, 0.8f);
    UI_CHECK_EQ(w::curveKeyCount(ui, cv), 3u);
    UI_CHECK(w::curveKey(ui, cv, 1).x == 0.5f); // kept sorted
    UI_CHECK(std::abs(w::curveEvaluate(ui, cv, 0.5f) - 0.8f) < 1e-5f);
    UI_CHECK(w::curveEvaluate(ui, cv, -1.f) == 0.f && w::curveEvaluate(ui, cv, 2.f) == 1.f);
    // smooth = C1 hermite: continuous through the middle key, monotone here
    f32 prev = -1.f;
    for (int i = 0; i <= 100; ++i) { const f32 v = w::curveEvaluate(ui, cv, f32(i) / 100.f); UI_CHECK(v >= prev - 1e-4f); prev = v; }
    const NodeHandle step2 = w::curveEditor(ui, s.c3, 0.f, 4.f, 0.f, 1.f);
    w::curveAddKey(ui, step2, 0.f, 0.2f, w::CurveInterp::Step);
    w::curveAddKey(ui, step2, 2.f, 0.6f, w::CurveInterp::Linear);
    w::curveAddKey(ui, step2, 4.f, 1.0f);
    UI_CHECK(w::curveEvaluate(ui, step2, 1.9f) == 0.2f);
    UI_CHECK(std::abs(w::curveEvaluate(ui, step2, 3.f) - 0.8f) < 1e-5f);
    ui.destroy(step2);
    step(ui);
    // clicking empty space adds a key; dragging moves it but never past its neighbours; Delete removes it
    const Rect c = ui.contentRect(cv);
    click(ui, {c.x + c.w * 0.25f, c.y + c.h * 0.5f});
    UI_CHECK_EQ(w::curveKeyCount(ui, cv), 4u);
    UI_CHECK(std::abs(w::curveKey(ui, cv, 1).x - 0.25f) < 0.01f && std::abs(w::curveKey(ui, cv, 1).y - 0.5f) < 0.01f);
    drag(ui, {c.x + c.w * 0.25f, c.y + c.h * 0.5f}, {c.x + c.w * 0.9f, c.y + c.h * 0.3f});
    UI_CHECK(w::curveKey(ui, cv, 1).x < 0.5f && w::curveKey(ui, cv, 1).x > 0.49f);
    UI_CHECK(std::abs(w::curveKey(ui, cv, 1).y - 0.7f) < 0.01f);
    ui.setFocus(cv);
    key(ui, Key::Delete);
    UI_CHECK_EQ(w::curveKeyCount(ui, cv), 3u);

    const NodeHandle tl = w::timeline(ui, s.c2, 12.f, "timeline");
    const u32 tCam = w::timelineAddTrack(ui, tl, "Camera");
    const u32 tAnim = w::timelineAddTrack(ui, tl, "Hero anim");
    const u32 tAudio = w::timelineAddTrack(ui, tl, "Audio");
    w::timelineAddClip(ui, tl, tCam, 0.f, 3.f, "dolly in", Color::rgb(0x4D96FF));
    w::timelineAddClip(ui, tl, tCam, 3.f, 6.5f, "orbit", Color::rgb(0x6BCB77));
    const u32 walk = w::timelineAddClip(ui, tl, tAnim, 1.f, 4.f, "walk", Color::rgb(0xFFD93D));
    w::timelineAddClip(ui, tl, tAudio, 0.5f, 6.f, "theme.ogg", Color::rgb(0xC77DFF));
    step(ui);
    UI_CHECK(ui.rect(tl).h >= 22.f + 3 * 28.f);
    UI_CHECK_EQ(w::timelineActiveClips(ui, tl, 2.f), 3u);
    const Rect tc = ui.contentRect(tl);
    const f32 pps = 60.f, x0 = tc.x + 96.f;
    f32 scrubbed = -1.f;
    ui.connect(tl, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { scrubbed = a.value; });
    drag(ui, {x0 + 2.f * pps, tc.y + 10.f}, {x0 + 4.5f * pps, tc.y + 10.f});
    UI_CHECK(std::abs(ui.value(tl) - 4.5f) < 0.02f && std::abs(scrubbed - 4.5f) < 0.02f);
    // drag a clip with 0.5 s snapping
    w::timelineSetSnap(ui, tl, 0.5f);
    const f32 rowY = tc.y + 22.f + 28.f * 1.5f;
    drag(ui, {x0 + 2.f * pps, rowY}, {x0 + 3.3f * pps, rowY});
    const Vec2 wc = w::timelineClip(ui, tl, tAnim, walk);
    UI_CHECK(std::abs(wc.x - 2.5f) < 1e-4f && std::abs(wc.y - 5.5f) < 1e-4f);
    ui.setFocus(tl);
    key(ui, Key::Right);
    UI_CHECK(std::abs(ui.value(tl) - 5.f) < 1e-4f);
    key(ui, Key::End);
    UI_CHECK(ui.value(tl) == 12.f);
    ui.setValue(tl, 2.25f);
    return 0;
}

// ---- viewport ------------------------------------------------------------------------------------------------------------
int viewport(Screen& s) {
    UiContext& ui = s.ui;
    // a 320x180 "scene": sky gradient, ground, sun
    g_scene.resize(320, 180);
    for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) {
            u32 r = u32(40 + y / 3), gg = u32(80 + y / 2), b = u32(200 - y / 3);
            if (y > 120) { r = 50; gg = u32(110 + (x * 7 + y * 3) % 20); b = 50; }
            const int dx = x - 240, dy = y - 50;
            if (dx * dx + dy * dy < 400) { r = 255; gg = 220; b = 120; }
            g_scene.pixels[std::size_t(y) * 320 + std::size_t(x)] = 0xFF000000u | (b << 16) | (gg << 8) | r;
        }
    g_images.push_back(&g_scene);
    const NodeHandle vp = w::viewportWidget(ui, s.c3, u32(g_images.size() - 1), 320, 180, "viewport");
    ui.setCss(vp, "height", "200px");
    step(ui);
    const Rect c = ui.contentRect(vp), r = w::viewportImageRect(ui, vp);
    UI_CHECK(std::abs(r.w / r.h - 16.f / 9.f) < 0.01f);
    UI_CHECK(r.h <= c.h + 0.01f && r.w <= c.w + 0.01f && (std::abs(r.w - c.w) < 0.5f || std::abs(r.h - c.h) < 0.5f));
    std::vector<f32> kinds;
    const ConnectionHandle conn = ui.connect(vp, SignalKind::Event, [&](UiContext&, NodeHandle, const SignalArgs& a) { kinds.push_back(a.value); });
    // the image centre maps to (160, 90) in image pixels
    ui.pointerMove(ctr(r));
    step(ui);
    UI_CHECK(std::abs(w::viewportLastInput(ui, vp).pos.x - 160.f) < 0.6f && std::abs(w::viewportLastInput(ui, vp).pos.y - 90.f) < 0.6f);
    ui.pointerDown({r.x + r.w * 0.75f, r.y + r.h * 0.25f}, MouseButton::Right);
    UI_CHECK(w::viewportLastInput(ui, vp).type == EventType::PointerDown && w::viewportLastInput(ui, vp).button == MouseButton::Right);
    UI_CHECK(std::abs(w::viewportLastInput(ui, vp).pos.x - 240.f) < 0.6f && std::abs(w::viewportLastInput(ui, vp).pos.y - 45.f) < 0.6f);
    ui.pointerUp({r.x + r.w * 0.75f, r.y + r.h * 0.25f}, MouseButton::Right);
    ui.wheel(ctr(r), {0.f, 2.f});
    UI_CHECK(w::viewportLastInput(ui, vp).wheel.y == 2.f);
    ui.setFocus(vp);
    ui.keyDown(Key::Space);
    UI_CHECK(w::viewportLastInput(ui, vp).key == Key::Space);
    ui.keyUp(Key::Space);
    UI_CHECK(w::viewportInputCount(ui, vp) >= 6u && kinds.size() >= w::viewportInputCount(ui, vp)); // core also emits Event
    ui.disconnect(conn); // kinds goes out of scope
    // zero copy: the draw list references the image index; nothing was copied into the UI
    step(ui);
    bool found = false;
    for (const DrawCmd& d : ui.drawList().cmds()) found |= d.op == DrawOp::Image && d.image == u32(g_images.size() - 1) && d.node == vp.index;
    UI_CHECK(found);
    return 0;
}

// ---- code editor -----------------------------------------------------------------------------------------------------------
int codeEdit(Screen& s) {
    UiContext& ui = s.ui;
    const char* src = "-- lantern behaviour\nlocal M = {}\n\nfunction M.update(self, dt)\n    if self.on then\n        self.t = self.t + dt * 2.5\n    end\nend\n\nreturn M";
    const NodeHandle ed = w::codeEdit(ui, s.c1, src, w::CodeLanguage::Lua, "code");
    ui.setCss(ed, "height", "230px");
    step(ui);
    UI_CHECK_EQ(w::codeEditLineCount(ui, ed), 10u);
    UI_CHECK_EQ(w::codeEditTokenAt(ui, ed, 0, 3), u8(4));  // comment
    UI_CHECK_EQ(w::codeEditTokenAt(ui, ed, 1, 0), u8(1));  // local
    UI_CHECK_EQ(w::codeEditTokenAt(ui, ed, 5, 32), u8(3)); // 2.5
    UI_CHECK_EQ(w::codeEditTokenAt(ui, ed, 1, 6), u8(0));  // M
    std::string changed;
    ui.connect(ed, SignalKind::TextChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { changed = a.text; });
    // typing, auto-indent, tab
    ui.setFocus(ed);
    w::codeEditSetCaret(ui, ed, 4, 99); // end of "    if self.on then"
    key(ui, Key::Enter);
    u32 l, c;
    w::codeEditCaret(ui, ed, l, c);
    UI_CHECK(l == 5 && c == 4);
    key(ui, Key::Tab);
    type(ui, "print(\"tick\")");
    w::codeEditCaret(ui, ed, l, c);
    UI_CHECK(l == 5 && c == 21);
    UI_CHECK(changed.find("\n        print(\"tick\")\n") != std::string::npos);
    UI_CHECK_EQ(w::codeEditTokenAt(ui, ed, 5, 15), u8(2)); // string
    // selection + copy / paste, shift-arrows
    key(ui, Key::Home);
    key(ui, Key::End, kModShift);
    key(ui, Key::C, kModCtrl);
    key(ui, Key::Down);
    key(ui, Key::End);
    key(ui, Key::Enter);
    key(ui, Key::V, kModCtrl);
    UI_CHECK(w::codeEditText(ui, ed).find("print(\"tick\")\n        self.t = self.t + dt * 2.5\n        print(\"tick\")") != std::string::npos);
    // undo twice restores, redo reapplies
    const std::string afterPaste = w::codeEditText(ui, ed);
    key(ui, Key::Z, kModCtrl);
    key(ui, Key::Z, kModCtrl);
    UI_CHECK_EQ(w::codeEditLineCount(ui, ed), 11u);
    key(ui, Key::Y, kModCtrl);
    key(ui, Key::Y, kModCtrl);
    UI_CHECK_EQ(w::codeEditText(ui, ed), afterPaste);
    // backspace across a line join, delete of a selection
    w::codeEditSetCaret(ui, ed, 3, 0);
    key(ui, Key::Backspace);
    w::codeEditCaret(ui, ed, l, c);
    UI_CHECK(l == 2 && c == 0 && w::codeEditLineCount(ui, ed) == 11u);
    key(ui, Key::Z, kModCtrl);
    // a click places the caret
    const Rect r = ui.contentRect(ed);
    click(ui, {r.right() - 4.f, r.y + 2.f});
    w::codeEditCaret(ui, ed, l, c);
    UI_CHECK(l == 0 && c == 20);
    // selection painted for the screenshot
    w::codeEditSetCaret(ui, ed, 3, 9);
    for (int i = 0; i < 6; ++i) key(ui, Key::Right, kModShift);
    UI_CHECK(ui.role(ed) == A11yRole::TextField);
    UI_CHECK(ui.text(ed) == w::codeEditText(ui, ed));
    return 0;
}

// ---- date-time edit ---------------------------------------------------------------------------------------------------------
int dateTimeEdit(Screen& s) {
    UiContext& ui = s.ui;
    const NodeHandle row = w::panel(ui, s.c3);
    ui.addClass(row, "row");
    w::label(ui, row, "Release");
    const NodeHandle dt = w::dateTimeEdit(ui, row, 2026, 1, 31, 23, 58, "release");
    step(ui);
    UI_CHECK_EQ(std::string(ui.text(dt)), std::string("2026-01-31 23:58"));
    UI_CHECK_EQ(w::dateTimeEditMinutes(ui, dt), i64(w::daysFromCivil(2026, 1, 31)) * 1440 + 23 * 60 + 58);
    UI_CHECK_EQ(i32(ui.value(dt)), w::daysFromCivil(2026, 1, 31));
    ui.setFocus(dt);
    key(ui, Key::Right); // month
    key(ui, Key::Up);    // Feb: day clamps to 28
    UI_CHECK_EQ(std::string(ui.text(dt)), std::string("2026-02-28 23:58"));
    key(ui, Key::Right); key(ui, Key::Right); key(ui, Key::Right); // minute
    UI_CHECK_EQ(w::dateTimeEditField(ui, dt), 4u);
    key(ui, Key::Up); key(ui, Key::Up); // carries into the next day
    UI_CHECK_EQ(std::string(ui.text(dt)), std::string("2026-03-01 00:00"));
    key(ui, Key::Down);
    UI_CHECK_EQ(std::string(ui.text(dt)), std::string("2026-02-28 23:59"));
    // typing digits fills a field and advances
    key(ui, Key::Left); key(ui, Key::Left); // day
    type(ui, "14");
    UI_CHECK_EQ(w::dateTimeEditField(ui, dt), 3u);
    type(ui, "0930");
    UI_CHECK_EQ(std::string(ui.text(dt)), std::string("2026-02-14 09:30"));
    // leap year handling on the year field
    w::setDateTimeEditValue(ui, dt, 2024, 2, 29, 12, 0);
    key(ui, Key::Home); // no-op
    for (int i = 0; i < 4; ++i) key(ui, Key::Left);
    key(ui, Key::Up);
    UI_CHECK_EQ(std::string(ui.text(dt)), std::string("2025-02-28 12:00"));
    // the calendar button opens a popup calendar; picking a day updates the field
    const Rect r = ui.contentRect(dt);
    click(ui, {r.right() - 8.f, r.y + r.h * 0.5f});
    NodeHandle cal = ui.focused();
    UI_CHECK(ui.alive(cal) && ui.role(cal) != A11yRole::None);
    key(ui, Key::Right);
    UI_CHECK_EQ(std::string(ui.text(dt)), std::string("2025-03-01 12:00"));
    key(ui, Key::Enter);
    step(ui);
    UI_CHECK(!w::popupOpen(ui, ui.root()) || ui.focused() == dt);
    int y, mo, d, hh, mm;
    w::dateTimeEditValue(ui, dt, y, mo, d, hh, mm);
    UI_CHECK(y == 2025 && mo == 3 && d == 1 && hh == 12 && mm == 0);
    w::setDateTimeEditValue(ui, dt, 2026, 10, 29, 23, 35);
    return 0;
}

// ---- video -------------------------------------------------------------------------------------------------------------------
int video(Screen& s) {
    UiContext& ui = s.ui;
    const NodeHandle v = w::videoWidget(ui, s.c3, &w::testPatternSource(), "video");
    g_images.push_back(w::videoImage(ui, v));
    w::videoSetImageIndex(ui, v, u32(g_images.size() - 1));
    step(ui);
    UI_CHECK_EQ(w::videoDecodedFrames(ui, v), 1u);
    UI_CHECK(w::videoImage(ui, v)->width == 160 && w::videoImage(ui, v)->height == 90);
    const u32 px0 = w::videoImage(ui, v)->at(5, 70);
    w::videoPlay(ui, v, true);
    for (int i = 0; i < 30; ++i) ui.update(1.f / 30.f);
    UI_CHECK(w::videoFrame(ui, v) >= 28u && w::videoFrame(ui, v) <= 31u);
    UI_CHECK(w::videoDecodedFrames(ui, v) >= 29u);
    w::videoPlay(ui, v, false);
    const u32 f = w::videoFrame(ui, v);
    for (int i = 0; i < 10; ++i) ui.update(1.f / 30.f);
    UI_CHECK_EQ(w::videoFrame(ui, v), f);
    UI_CHECK(w::videoImage(ui, v)->at(5, 70) != px0 || w::videoImage(ui, v)->at(5, 62) != 0u);
    // scrubber seeks; Space toggles; decoding is lazy (seeking twice to the same frame decodes once)
    const Rect c = ui.contentRect(v);
    click(ui, {c.x + 30.f + (c.w - 100.f) * 0.5f, c.bottom() - 13.f});
    UI_CHECK(std::abs(i32(w::videoFrame(ui, v)) - 150) <= 1);
    const u32 dec = w::videoDecodedFrames(ui, v);
    w::videoSeek(ui, v, w::videoFrame(ui, v));
    UI_CHECK_EQ(w::videoDecodedFrames(ui, v), dec);
    ui.setFocus(v);
    key(ui, Key::Space);
    UI_CHECK(w::videoPlaying(ui, v));
    key(ui, Key::Space);
    UI_CHECK(!w::videoPlaying(ui, v));
    w::videoSeek(ui, v, 299);
    w::videoPlay(ui, v, true);
    ui.update(1.f / 30.f);
    ui.update(1.f / 30.f);
    UI_CHECK(w::videoFrame(ui, v) <= 2u); // loops
    w::videoPlay(ui, v, false);
    w::videoSeek(ui, v, 96);
    return 0;
}

int run(const std::string& gate) {
    Screen s;
    int rc = 0;
    auto want = [&](const char* g) { const bool w = gate.empty() || gate == g; if (w && gate.empty()) std::fprintf(stderr, "[gate] %s\n", g); return w; };
    if (want("code_edit")) rc |= codeEdit(s);
    if (want("font_picker")) rc |= fontPicker(s);
    if (want("node_graph")) rc |= nodeGraph(s);
    if (want("curve_timeline")) rc |= curveTimeline(s);
    if (want("viewport")) rc |= viewport(s);
    if (want("datetime_edit")) rc |= dateTimeEdit(s);
    if (want("video")) rc |= video(s);
    if (gate.empty()) shot(s.ui, "tier_d2.png");
    if (want("file_dialog")) rc |= fileDialog(s);
    return finish(gate.empty() ? "fuse_ui_tier_d2" : ("fuse_ui_widget_" + gate).c_str());
}
} // namespace

int main(int argc, char** argv) {
    std::string gate;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
    }
    return run(gate);
}
