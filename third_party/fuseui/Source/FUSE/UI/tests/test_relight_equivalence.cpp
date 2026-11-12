// fuse_ui_relight_equivalence: Relight's overlay::Ui (the first client, plan UI port) and fuse::ui::immediate::ImmediateUi
// run the same menu under the same event script; widget rects, click results, focus and text-field values must match.
#include "test_util.hpp"

#include "fuse/relight/overlay/ui.hpp"
#include "fuse/ui/immediate.hpp"

namespace ov = fuse::relight::overlay;
namespace im = fuse::ui::immediate;
using namespace fuse::ui::test;

namespace {
struct Ev { int type, x, y, value; };
struct Log {
    std::vector<std::string> clicks;
    std::string field;
};

template <class U, class E, class R>
void menu(U& ui, Log& log, std::string& field, int& page) {
    ui.title("Relight");
    if (ui.button("p0", "Graphics", page == 0)) { page = 0; log.clicks.push_back("p0"); }
    ui.sameLine();
    if (ui.button("p1", "Audio", page == 1)) { page = 1; log.clicks.push_back("p1"); }
    ui.separator();
    for (int i = 0; i < 12 && ui.hasRoom(); ++i) {
        char id[8], lbl[16];
        std::snprintf(id, sizeof id, "o%d", i);
        std::snprintf(lbl, sizeof lbl, "Option %d", i);
        if (ui.button(id, lbl)) log.clicks.push_back(id);
        if (i % 3 == 0) ui.sameLine();
    }
    ui.spacing();
    ui.text("Name:");
    ui.sameLine();
    if (ui.textField("name", field, 12)) log.clicks.push_back("name-changed");
    (void)sizeof(E);
    (void)sizeof(R);
}

template <class U, class E, class R>
Log drive(const std::vector<Ev>& script, std::vector<R>& rects, std::vector<std::uint32_t>& focus) {
    U ui;
    Log log;
    std::string field = "abc";
    int page = 0;
    const char* ids[] = {"p0", "p1", "o0", "o1", "o5", "o11", "name"};
    auto pass = [&](const E* e) {
        ui.begin({10, 10, 300, 300}, 2, e);
        menu<U, E, R>(ui, log, field, page);
        ui.end();
    };
    pass(nullptr);
    for (const Ev& s : script) {
        E e{};
        e.type = decltype(e.type)(s.type);
        e.x = s.x;
        e.y = s.y;
        e.value = s.value;
        pass(&e);
        pass(nullptr);
        focus.push_back(ui.focused());
        for (const char* id : ids) {
            R r{};
            rects.push_back(ui.widgetRect(id, r) ? r : R{-1, -1, -1, -1});
            (void)id;
        }
    }
    log.field = field;
    return log;
}
} // namespace

int main() {
    // first, find widget centres from the reference
    ov::Ui probe;
    {
        Log l;
        std::string f = "abc";
        int p = 0;
        probe.begin({10, 10, 300, 300}, 2, nullptr);
        menu<ov::Ui, ov::InputEvent, ov::Rect>(probe, l, f, p);
        probe.end();
    }
    auto centre = [&](const char* id) {
        ov::Rect r{};
        probe.widgetRect(id, r);
        return std::pair<int, int>{r.x + r.w / 2, r.y + r.h / 2};
    };
    const auto [p1x, p1y] = centre("p1");
    const auto [o1x, o1y] = centre("o1");
    const auto [nx, ny] = centre("name");
    // MouseMove=1 MouseDown=2 MouseUp=3 Wheel=4 KeyDown=5 Char=6
    const std::vector<Ev> script = {
        {1, p1x, p1y, 0}, {2, p1x, p1y, 0}, {3, p1x, p1y, 0},          // click Audio
        {1, o1x, o1y, 0}, {2, o1x, o1y, 0}, {1, 1, 1, 0}, {3, 1, 1, 0}, // press, drag out, release: no click
        {2, o1x, o1y, 0}, {3, o1x, o1y, 0},                             // click Option 1
        {2, nx, ny, 0},   {3, nx, ny, 0},                               // focus field
        {6, 0, 0, 'x'},   {6, 0, 0, 'y'}, {5, 0, 0, ov::kVkBack}, {6, 0, 0, 'z'}, {5, 0, 0, ov::kVkReturn},
        {4, 0, 0, -1},    {5, 0, 0, ov::kVkEscape},
    };
    std::vector<ov::Rect> ra;
    std::vector<im::IRect> rb;
    std::vector<std::uint32_t> fa, fb;
    const Log a = drive<ov::Ui, ov::InputEvent, ov::Rect>(script, ra, fa);
    const Log b = drive<im::ImmediateUi, im::InputEvent, im::IRect>(script, rb, fb);
    UI_CHECK_EQ(ra.size(), rb.size());
    int rectMismatch = 0;
    for (std::size_t i = 0; i < std::min(ra.size(), rb.size()); ++i)
        if (ra[i].x != rb[i].x || ra[i].y != rb[i].y || ra[i].w != rb[i].w || ra[i].h != rb[i].h) {
            if (rectMismatch++ < 5)
                std::fprintf(stderr, "rect %zu: overlay (%d %d %d %d) immediate (%d %d %d %d)\n", i, ra[i].x, ra[i].y, ra[i].w,
                             ra[i].h, rb[i].x, rb[i].y, rb[i].w, rb[i].h);
        }
    UI_CHECK_EQ(rectMismatch, 0);
    UI_CHECK(fa == fb);
    UI_CHECK(a.clicks == b.clicks);
    UI_CHECK_EQ(a.field, b.field);
    std::string cl;
    for (auto& c : a.clicks) cl += c + " ";
    std::printf("clicks: %s field: %s\n", cl.c_str(), a.field.c_str());
    UI_CHECK(!a.clicks.empty());
    return finish("fuse_ui_relight_equivalence");
}
