// Gate fuse_ui_event_routing: capture → target → bubble order, propagation stop, click = press + release inside,
// pointer capture while pressed, hover chain, disabled, modal blocking, wheel, signals, cross-thread post().
#include "test_util.hpp"

#include <thread>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
std::string g_log;
bool g_stopAtMiddleCapture = false;

struct Recorder {
    static constexpr std::string_view kName = "recorder";
    static constexpr bool kInteractive = true;
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type != EventType::PointerDown) return false;
        const char ph = ec.phase == Phase::Capture ? 'C' : ec.phase == Phase::Target ? 'T' : 'B';
        g_log += std::string(ec.ui.idOf(ec.node)) + ":" + ph + " ";
        return g_stopAtMiddleCapture && ec.phase == Phase::Capture && ec.ui.idOf(ec.node) == "mid";
    }
};
} // namespace

int main() {
    UiContext ui({.viewport = {200, 200}});
    const u32 rec = ui.registry().registerWidget<Recorder>();
    const NodeHandle outer = ui.create(rec, ui.root(), "outer");
    ui.setCss(outer, "height", "200px");
    ui.setCss(outer, "padding", "20px");
    const NodeHandle mid = ui.create(rec, outer, "mid");
    ui.setCss(mid, "padding", "20px");
    ui.setCss(mid, "height", "100px");
    const NodeHandle inner = ui.create(rec, mid, "inner");
    ui.setCss(inner, "height", "30px");
    ui.update();

    UI_CHECK(ui.hitTest({50, 50}) == inner);
    UI_CHECK(ui.pointerDown({50, 50}));
    UI_CHECK_EQ(g_log, std::string("outer:C mid:C inner:T mid:B outer:B "));
    ui.pointerUp({50, 50});

    g_log.clear();
    g_stopAtMiddleCapture = true;
    ui.pointerDown({50, 50});
    UI_CHECK_EQ(g_log, std::string("outer:C mid:C "));
    ui.pointerUp({50, 50});
    g_stopAtMiddleCapture = false;

    // hover chain and pressed state
    ui.pointerMove({50, 50});
    ui.update();
    UI_CHECK(ui.states(inner) & kStateHover);
    UI_CHECK(ui.states(outer) & kStateHover);
    ui.pointerMove({5, 190});
    UI_CHECK(!(ui.states(inner) & kStateHover));
    UI_CHECK(ui.states(outer) & kStateHover);

    // click requires release inside the pressed widget
    int clicks = 0;
    ui.connect(inner, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    ui.pointerDown({50, 50});
    ui.pointerUp({50, 50});
    UI_CHECK_EQ(clicks, 1);
    ui.pointerDown({50, 50});
    ui.pointerMove({5, 190});
    UI_CHECK(ui.pressed() == inner); // capture while pressed
    ui.pointerUp({5, 190});
    UI_CHECK_EQ(clicks, 1);
    UI_CHECK(!(ui.states(inner) & kStatePressed));

    // disabled: no click
    ui.setDisabled(inner, true);
    ui.pointerDown({50, 50});
    ui.pointerUp({50, 50});
    UI_CHECK_EQ(clicks, 1);
    ui.setDisabled(inner, false);

    // disconnect
    const ConnectionHandle c = ui.connect(inner, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { clicks += 10; });
    ui.disconnect(c);
    ui.pointerDown({50, 50});
    ui.pointerUp({50, 50});
    UI_CHECK_EQ(clicks, 2);

    // modal: input outside the scope is swallowed
    const NodeHandle dialog = ui.create(rec, ui.root(), "dialog");
    ui.setCss(dialog, "height", "10px");
    ui.update();
    ui.setModal(dialog);
    ui.pointerDown({50, 50});
    ui.pointerUp({50, 50});
    UI_CHECK_EQ(clicks, 2);
    ui.setModal({});

    // destroyed node: stale handles are inert
    ui.destroy(mid);
    UI_CHECK(!ui.alive(inner));
    ui.update();
    UI_CHECK(ui.hitTest({50, 50}) == outer);

    // cross-thread commands are applied on the owner thread at update()
    const NodeHandle target = ui.create(ui.registry().find("panel"), ui.root(), "target");
    std::thread th[4];
    for (int t = 0; t < 4; ++t)
        th[t] = std::thread([&ui, target] {
            for (int i = 0; i < 25; ++i)
                while (!ui.post([](UiContext& u, NodeHandle n, u64 a, u64) { u.setUserData(n, u.userData(n) + a); }, target, 1)) {}
        });
    for (auto& t : th) t.join();
    ui.update();
    UI_CHECK_EQ(ui.userData(target), u64(100));
    return finish("fuse_ui_event_routing");
}
