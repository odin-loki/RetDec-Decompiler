// OS-window popups gate (plan UI5): with NativeOptions::osPopups a popup menu is its own borderless OS window (X11
// override-redirect / Win32 WS_POPUP + WS_EX_NOACTIVATE), like Qt's QMenu, so it can extend past the main window.
//   * the menu is left out of the main window's draw list and shown in exactly one popup window
//   * that window lies partly outside the main window (below it) and stays inside the screen
//   * its pixels (read back from the window system) equal the CPU raster of the popup's own draw list
//   * a click outside every app window closes the popup (pointer grab / deactivation)
//   * a real click posted to the popup window (XSendEvent / PostMessage) on an item that lies outside the main
//     window picks that item through the normal event pump, and the popup window goes away
// Exit 77 without a display. --out=<dir> writes os_popup.png (main window + popup composited at screen positions).
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_os_popups", .argc = argc, .argv = argv, .loadSettings = false}};
    const std::string out(a.args().get("out"));
    app::NativePlatform native(a, {.osPopups = true});
    if (!native.available()) {
        std::printf("fuse_app_os_popups: skipped (%.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 77;
    }
    auto& win = a.createWindow({.title = "fuse_app OS popups", .width = 320, .height = 160});
    UiContext& ui = win.ui();
    const NodeHandle btn = w::button(ui, ui.root(), "File", "file");
    native.show(win);
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    CHECK(ui.osPopups());

    f32 picked = -1.f;
    std::string pickedText;
    ui.connect(btn, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& s) { picked = s.value; pickedText = s.text; });
    std::string items;
    for (int i = 1; i <= 12; ++i) items += (i > 1 ? "\n" : "") + std::string("Item ") + std::to_string(i);
    const NodeHandle scope = w::popupMenu(ui, btn, items, {40.f, 60.f});
    win.requestRedraw();
    for (int i = 0; i < 40; ++i) a.step(0.005f);

    CHECK(ui.popupSurfaces().size() == 1);
    CHECK(native.popupCount(win) == 1);
    const Rect menu = ui.rect(ui.firstChild(scope));
    std::printf("  menu rect %.0f,%.0f %.0fx%.0f (window 320x160)\n", menu.x, menu.y, menu.w, menu.h);
    CHECK(menu.y + menu.h > 160.f); // extends past the main window
    const Vec2 origin = native.screenOrigin(win);
    const Rect scr = native.popupScreenRect(win, 0);
    std::printf("  window origin %.0f,%.0f; popup window %.0f,%.0f %.0fx%.0f\n", origin.x, origin.y, scr.x, scr.y, scr.w, scr.h);
    CHECK(scr.x == origin.x + menu.x && scr.y == origin.y + menu.y && scr.w == menu.w && scr.h == menu.h);

    // the popup window shows exactly the CPU raster of the popup draw list
    Image shown, ref;
    CHECK(native.capturePopup(win, 0, shown));
    {
        const auto& ps = ui.popupSurfaces()[0];
        u32 clear = 0xFF000000u;
        for (const DrawCmd& c : ps.draw.cmds())
            if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
        ref.resize(int(scr.w), int(scr.h), clear);
        CpuRasterizer rz = win.rasterizer();
        rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        rz.render(ps.draw, ref);
    }
    int maxErr = 255;
    if (shown.width == ref.width && shown.height == ref.height) {
        maxErr = 0;
        for (std::size_t i = 0; i < ref.pixels.size(); ++i)
            for (int c = 0; c < 3; ++c)
                maxErr = std::max(maxErr, std::abs(int((shown.pixels[i] >> (8 * c)) & 0xFF) - int((ref.pixels[i] >> (8 * c)) & 0xFF)));
    }
    std::printf("  popup window read-back %dx%d, max channel error vs CPU raster %d\n", shown.width, shown.height, maxErr);
    CHECK(maxErr <= 2);

    if (!out.empty()) { // composite of what the screen shows: main window + popup window at their screen offsets
        Image main;
        if (native.capture(win, main) && shown.width > 0) {
            const int w0 = std::max(main.width, int(menu.x + menu.w) + 20), h0 = std::max(main.height, int(menu.y + menu.h) + 20);
            Image comp;
            comp.resize(w0, h0, 0xFF403830u);
            for (int y = 0; y < main.height; ++y)
                for (int x = 0; x < main.width; ++x) comp.pixels[std::size_t(y) * std::size_t(w0) + std::size_t(x)] = main.at(x, y);
            for (int y = 0; y < shown.height; ++y)
                for (int x = 0; x < shown.width; ++x) {
                    const int dx = int(menu.x) + x, dy = int(menu.y) + y;
                    if (dx < w0 && dy < h0) comp.pixels[std::size_t(dy) * std::size_t(w0) + std::size_t(dx)] = shown.at(x, y);
                }
            app::writePng(comp, out + "/os_popup.png");
        }
    }

    // a real click on "Item 11" (outside the main window) through the popup window's event queue
    NodeHandle item;
    for (NodeHandle it = ui.firstChild(ui.firstChild(scope)); ui.alive(it); it = ui.nextSibling(it))
        if (ui.text(it) == "Item 11") item = it;
    CHECK(ui.alive(item));
    const Rect ir = ui.rect(item);
    CHECK(ir.y > 160.f);
    CHECK(native.postClick(native.popupWindow(win, 0), {ir.x + ir.w * 0.5f - menu.x, ir.y + ir.h * 0.5f - menu.y}));
    for (int i = 0; i < 200 && picked < 0.f; ++i) a.step(0.01f);
    for (int i = 0; i < 20; ++i) a.step(0.005f);
    std::printf("  picked %.0f \"%s\"; popup windows now %zu\n", picked, pickedText.c_str(), native.popupCount(win));
    CHECK(picked == 10.f && pickedText == "Item 11");
    CHECK(native.popupCount(win) == 0 && ui.popupSurfaces().empty());

    // Qt::Popup: a click anywhere outside the app's windows (X11: delivered to the grabbing popup with coordinates
    // outside it; Win32: captured / app deactivated) closes the popup without picking anything
    picked = -1.f;
    w::popupMenu(ui, btn, items, {40.f, 60.f});
    win.requestRedraw();
    for (int i = 0; i < 40; ++i) a.step(0.005f);
    CHECK(native.popupCount(win) == 1);
    CHECK(native.postClick(native.popupWindow(win, 0), {-30.f, 400.f}));
    for (int i = 0; i < 200 && native.popupCount(win) != 0; ++i) a.step(0.01f);
    std::printf("  outside click: popup windows now %zu, picked %.0f\n", native.popupCount(win), picked);
    CHECK(native.popupCount(win) == 0 && picked < 0.f);

    win.requestClose();
    a.step(0.f);
    std::printf("fuse_app_os_popups: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
