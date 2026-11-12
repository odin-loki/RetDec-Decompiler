// fuse_app extras (RetDec additions): one binary, one ctest gate per area (fuse_app_extra_<gate>, --gate=<name>):
//   close_request  AppWindow::setCloseRequestHandler: the hook vetoes a user close (the window stays and keeps
//                  presenting), allows it later; requestClose() bypasses it; no hook = the old behaviour
//   file_drop      AppWindow::setFileDropHandler / dropFiles (paths, position, hit-tested target node), refused
//                  without a hook; parseUriList (text/uri-list: CRLF, comments, percent-decoding, file://localhost,
//                  remote hosts and other schemes skipped)
//   light_theme    every widget-drawn colour of the shell widgets (menu bar + open menu, dock titles, tab bar,
//                  table header, splitter grip, scroll bars, code editor) follows a light style sheet: rendered
//                  headless with the CPU rasteriser, sampled, and written as PNG with --out=<dir>
#include "fuse/app/app.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
std::string g_out;

struct Virt {
    double t = 0.0;
    static double now(void* u) { return static_cast<Virt*>(u)->t; }
    static bool pump(void* u, app::Application&, f32 wait) {
        static_cast<Virt*>(u)->t += std::isinf(wait) ? 0.05 : std::min(double(wait), 0.05);
        return true;
    }
};

// ---- close_request ---------------------------------------------------------------------------------------------------
void gateCloseRequest() {
    Virt v;
    app::Application a{{.name = "gate_close_request", .loadSettings = false}};
    a.setClock({&v, &Virt::now});
    a.setEventSource({&v, &Virt::pump});
    auto& win = a.createWindow({.title = "close", .width = 200, .height = 100});
    w::label(win.ui(), win.ui().root(), "Unsaved changes");
    // no hook: a user close closes (the old WM_CLOSE / WM_DELETE_WINDOW behaviour)
    auto& plain = a.createWindow({.title = "plain", .width = 100, .height = 60});
    CHECK(plain.userCloseRequest() && plain.closing());
    a.step(0.f);
    CHECK(a.windowCount() == 1);

    int asked = 0;
    bool allow = false;
    win.setCloseRequestHandler([&](app::AppWindow& w2) {
        ++asked;
        CHECK(&w2 == &win);
        return allow;
    });
    a.step(0.f);
    const auto frames = win.framesPresented();
    CHECK(!win.userCloseRequest()); // vetoed: e.g. an "unsaved changes?" dialog opens instead
    CHECK(asked == 1 && !win.closing());
    a.step(0.f);
    CHECK(a.windowCount() == 1);
    CHECK(win.framesPresented() > frames); // the veto forces a frame (the dialog shows)
    allow = true;
    CHECK(win.userCloseRequest() && win.closing() && asked == 2);
    CHECK(win.userCloseRequest() && asked == 2); // already closing: the hook is not asked again
    a.step(0.f);
    CHECK(a.windowCount() == 0);

    // requestClose() (the app's own decision) never asks the hook
    auto& w3 = a.createWindow({.title = "direct", .width = 100, .height = 60});
    int asked3 = 0;
    w3.setCloseRequestHandler([&](app::AppWindow&) { ++asked3; return false; });
    w3.requestClose();
    CHECK(w3.closing() && asked3 == 0);
}

// ---- file_drop -------------------------------------------------------------------------------------------------------
void gateFileDrop() {
    // text/uri-list decoding
    const auto p = app::parseUriList("# comment\r\nfile:///home/user/a%20b.exe\r\nfile://localhost/tmp/x.bin\r\n"
                                     "http://example.com/y\r\nfile://otherhost/z\r\nfile:///caf%C3%A9/%E2%9C%93.elf\r\n"
                                     "\r\nfile:/single/slash\n");
    CHECK(p.size() == 4);
    if (p.size() == 4) {
        CHECK(p[0] == "/home/user/a b.exe");
        CHECK(p[1] == "/tmp/x.bin");
        CHECK(p[2] == "/caf\xC3\xA9/\xE2\x9C\x93.elf"); // UTF-8 survives percent-decoding
        CHECK(p[3] == "/single/slash");
    }
    CHECK(app::parseUriList("").empty() && app::parseUriList("file://remote/x\r\n").empty());

    Virt v;
    app::Application a{{.name = "gate_file_drop", .loadSettings = false}};
    a.setClock({&v, &Virt::now});
    a.setEventSource({&v, &Virt::pump});
    auto& win = a.createWindow({.title = "drop", .width = 300, .height = 200});
    UiContext& ui = win.ui();
    const NodeHandle zone = w::panel(ui, ui.root(), "zone");
    ui.setCss(zone, "height", "120px");
    a.step(0.f);
    CHECK(!win.acceptsFileDrops());
    CHECK(!win.dropFiles({"/x"}, {10, 10})); // refused without a hook
    std::vector<app::FileDrop> got;
    win.setFileDropHandler([&](app::AppWindow&, const app::FileDrop& d) { got.push_back(d); });
    CHECK(win.acceptsFileDrops());
    CHECK(!win.dropFiles({}, {10, 10})); // nothing to deliver
    const Rect zr = ui.rect(zone);
    CHECK(win.dropFiles({"/bin/ls", "/tmp/\xE2\x9C\x93"}, {zr.x + 5.f, zr.y + 5.f}));
    CHECK(got.size() == 1);
    if (got.size() == 1) {
        CHECK(got[0].paths.size() == 2 && got[0].paths[0] == "/bin/ls" && got[0].paths[1] == "/tmp/\xE2\x9C\x93");
        CHECK(got[0].pos.x == zr.x + 5.f && got[0].pos.y == zr.y + 5.f);
        CHECK(got[0].target == zone); // hit-tested node under the drop point
    }
    win.setFileDropHandler({});
    CHECK(!win.acceptsFileDrops() && !win.dropFiles({"/x"}, {1, 1}) && got.size() == 1);
}

// ---- light_theme -----------------------------------------------------------------------------------------------------
struct Rows {
    w::ItemModel model() const {
        w::ItemModel m;
        m.user = this;
        m.columns = 3;
        m.rows = [](const void*) { return 60u; };
        m.data = [](const void*, u32 r, u32 c, std::string& s) {
            s = c == 0 ? "0x" + std::to_string(0x401000 + r * 16) : c == 1 ? "mov" : "eax, ebx";
            return std::string_view(s);
        };
        m.header = [](const void*, u32 c) { return std::string_view(c == 0 ? "Address" : c == 1 ? "Mnemonic" : "Operands"); };
        return m;
    }
};

constexpr const char* kLight = R"css(
:root {
  --bg: #f5f5f7; --surface: #ffffff; --surface-hi: #e6e8ee; --border: #c8cad2;
  --text: #1d1e22; --text-dim: #6b6e78; --accent: #2f6fed; --accent-hi: #4a84f5;
}
listview, treeview, lineedit, spinbox, gridview, nodegraph, curveeditor, timeline, viewport, codeedit { background: #ffffff; }
docktitle { background: #e9eaef; color: var(--text); }
dockclose { color: #7a7d88; }
dockclose:hover { color: #d03030; }
splithandle { background-end: #b4b7c2; }
tab:selected { background: #ffffff; }
tabline { background: var(--border); }
tableheader { background: #eceef3; color: var(--text); border-color: #c8cad2; }
tablestripe { background: rgba(0, 0, 0, 0.03); }
scrolltrack { background: rgba(0, 0, 0, 0.06); }
scrollthumb { background: rgba(110, 114, 128, 0.55); }
scrollthumb:pressed { background: rgba(80, 84, 98, 0.75); }
codearea { background: #ffffff; }
codegutter { background: #f0f1f4; color: #9a9caa; }
codegutter:selected { color: #1d1e22; }
codeplain { color: #1d1e22; }
codekeyword { color: #0033b3; }
codestring { color: #067d17; }
codenumber { color: #1750eb; }
codecomment { color: #8c8c8c; }
codeselection { background: #cfe0ff; }
codecaret { background: #1d1e22; }
codecurrentline { background: rgba(0, 0, 0, 0.04); }
)css";

int lum(u32 px) { return int(px & 0xFF) * 3 + int((px >> 8) & 0xFF) * 6 + int((px >> 16) & 0xFF); } // x10
bool light(const Image& img, Vec2 p) {
    const int x = int(p.x), y = int(p.y);
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return false;
    const u32 px = img.at(x, y);
    std::printf("  sample (%d,%d) = #%02x%02x%02x\n", x, y, px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF);
    return lum(px) > 2000; // > ~200 / 255 grey
}

void gateLightTheme() {
    Virt v;
    app::Application a{{.name = "gate_light_theme", .loadSettings = false}};
    a.setClock({&v, &Virt::now});
    a.setEventSource({&v, &Virt::pump});
    auto& win = a.createWindow({.title = "light", .width = 900, .height = 560});
    UiContext& ui = win.ui();
    CHECK(ui.addStyleSheet(kLight).has_value());
    const NodeHandle root = ui.root();
    ui.setCss(root, "padding", "0px");
    ui.setCss(root, "gap", "0px");
    const NodeHandle bar = w::menuBar(ui, root, "menubar");
    const NodeHandle file = w::menuBarMenu(ui, bar, "File", "Open...\nSave\n-\nQuit", "file");
    w::menuBarMenu(ui, bar, "View", "Disassembly\nGraph", "view");
    const NodeHandle area = w::dockArea(ui, root, "area");
    ui.setCss(area, "flex-grow", "1");
    const NodeHandle left = w::dockWidget(ui, area, "Functions", w::DockSide::Left, "funcs");
    const NodeHandle lv = w::listView(ui, w::dockBody(ui, left), "main\nsub_401000\nsub_401200\nstart", 0, "fl");
    ui.setCss(lv, "flex-grow", "1");
    const NodeHandle bottom = w::dockWidget(ui, area, "Output", w::DockSide::Bottom, "out");
    const NodeHandle code = w::codeEdit(ui, w::dockBody(ui, bottom), "int main() {\n  return 42; // answer\n}", w::CodeLanguage::Cpp, "code");
    ui.setCss(code, "height", "90px");
    const NodeHandle centre = w::dockRegion(ui, area, w::DockSide::Center);
    const NodeHandle tabs = w::tabWidget(ui, centre, "Disassembly\nDecompiled C\nHex", "tabs");
    ui.setCss(tabs, "flex-grow", "1");
    const NodeHandle split = w::splitter(ui, w::tabPage(ui, tabs, 0), false, "split");
    ui.setCss(split, "flex-grow", "1");
    const NodeHandle p1 = w::splitterPane(ui, split, 520.f);
    const NodeHandle p2 = w::splitterPane(ui, split);
    Rows rows;
    const w::ItemModel model = rows.model();
    const NodeHandle table = w::tableView(ui, p1, &model, "table");
    ui.setCss(table, "flex-grow", "1");
    for (u32 c = 0; c < 3; ++c) w::setTableColumnWidth(ui, table, c, c == 0 ? 120.f : 150.f);
    const NodeHandle scroll = w::scrollArea(ui, p2, "scroll");
    ui.setCss(scroll, "flex-grow", "1");
    for (int i = 0; i < 30; ++i) w::label(ui, scroll, "Section " + std::to_string(i));
    a.step(0.f);
    // open the File menu so a popup menu is in the picture
    win.click({ui.rect(file).x + 5.f, ui.rect(file).y + 5.f});
    a.step(0.f);
    a.step(0.f);
    const Image& img = win.rasterize({});
    if (!g_out.empty()) {
        const std::string path = g_out + "/light_theme.png";
        CHECK(app::writePng(img, path));
        std::printf("light_theme: wrote %s\n", path.c_str());
    }

    const NodeHandle title = ui.firstChild(left);
    const Rect tr = ui.rect(title), tc = ui.contentRect(table), tb = ui.rect(w::tabWidgetBar(ui, tabs));
    const Rect cr = ui.contentRect(code);
    std::printf("light_theme samples:\n");
    CHECK(light(img, {tr.right() - tr.h - 6.f, tr.y + 2.f}));             // dock title strip
    CHECK(light(img, {tc.right() - 14.f, tc.y + 2.f}));                    // table header (right of the titles)
    CHECK(light(img, {tb.x + 3.f, tb.y + 3.f}));                           // selected tab
    CHECK(light(img, {cr.x + 2.f, cr.bottom() - 3.f}));                    // code gutter
    CHECK(light(img, {cr.right() - 20.f, cr.bottom() - 3.f}));             // code area
    CHECK(light(img, {ui.rect(bar).right() - 4.f, ui.rect(bar).y + 3.f})); // menu bar
    bool menuOpen = false;
    for (NodeHandle c = ui.firstChild(w::overlayLayer(ui)); ui.alive(c); c = ui.nextSibling(c)) {
        const NodeHandle panel = ui.firstChild(c);
        if (!ui.alive(panel)) continue;
        const Rect mr = ui.rect(panel);
        if (mr.w <= 0.f) continue;
        menuOpen = true;
        CHECK(light(img, {mr.right() - 6.f, mr.y + mr.h * 0.5f}));          // popup menu panel
    }
    CHECK(menuOpen);
    // the splitter grip and the table's scroll bar use the sheet's (mid-grey) colours, not the dark built-ins
    const Rect hr = ui.rect(ui.nextSibling(ui.firstChild(split)));
    const u32 grip = img.at(int(hr.x + hr.w * 0.5f), int(hr.y + hr.h * 0.5f));
    std::printf("  grip #%02x%02x%02x\n", grip & 0xFF, (grip >> 8) & 0xFF, (grip >> 16) & 0xFF);
    CHECK(lum(grip) > 1600);
    // no large dark area is left: pixels darker than 25 % grey are text / glyph strokes only
    int dark = 0;
    for (u32 px : img.pixels) dark += lum(px) < 640;
    std::printf("  dark pixels: %d of %zu\n", dark, img.pixels.size());
    CHECK(dark < int(img.pixels.size() / 20));
}

} // namespace

int main(int argc, char** argv) {
    std::string gate;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--gate=", 7) == 0) gate = argv[i] + 7;
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
    }
    struct G { const char* name; void (*fn)(); };
    static const G kGates[] = {{"close_request", gateCloseRequest}, {"file_drop", gateFileDrop}, {"light_theme", gateLightTheme}};
    bool ran = false;
    for (const G& g : kGates)
        if (gate.empty() || gate == g.name) {
            const int before = g_fail;
            g.fn();
            std::printf("%s: %s\n", g.name, g_fail == before ? "ok" : "FAILED");
            ran = true;
        }
    if (!ran) {
        std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str());
        return 2;
    }
    std::printf("fuse_app_extras: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
