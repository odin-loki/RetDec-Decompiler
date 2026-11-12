// fuse_app gates (plan UI5), one executable per gate (FUSE_APP_GATE):
//   idle_zero_frames — an idle app presents nothing for 10 virtual seconds and sleeps until a timer / input
//   multiwindow      — two windows update and present independently; closing the last one ends run()
//   hello_startup    — Application + window + document to the first presented frame, ≤ 150 ms (headless)
//   settings         — command line parsing, settings round trip, single-instance lock
#include "fuse/app/app.hpp"
#include "fuse/ui/doc/document.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace fuse::ui;
namespace app = fuse::app;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
/// Virtual clock + pump: "sleeping" advances time by the requested wait (capped), input is injected by the test.
struct Virt {
    double t = 0.0;
    double slept = 0.0;
    int pumps = 0;
    static double now(void* u) { return static_cast<Virt*>(u)->t; }
    static bool pump(void* u, app::Application&, f32 wait) {
        Virt& v = *static_cast<Virt*>(u);
        ++v.pumps;
        const double d = std::isinf(wait) ? 1.0 : std::min(double(wait), 1.0);
        v.t += d;
        v.slept += d;
        return true;
    }
};
void useVirtual(app::Application& a, Virt& v) {
    a.setClock({&v, &Virt::now});
    a.setEventSource({&v, &Virt::pump});
}
constexpr const char* kDoc = R"({ "fuseui": 1, "root": { "class": "panel", "id": "p", "children": [
    { "class": "label", "id": "l", "text": "n = {n}" }, { "class": "button", "id": "b", "text": "Go" } ] } })";

int idleZeroFrames() {
    app::Application a{{.name = "gate_idle", .loadSettings = false}};
    Virt v;
    useVirtual(a, v);
    auto& w = a.createWindow({.width = 200, .height = 100});
    fuse::ui::doc::Model m;
    m.set("n", 1.0);
    CHECK(w.loadText(kDoc, &m));
    a.step(0.f);
    CHECK(w.framesPresented() == 1);
    // ten idle seconds: no frames, every pump is a full sleep
    const u64 f0 = a.stats().frames;
    const double t0 = v.t;
    while (v.t - t0 < 10.0) a.step();
    CHECK(a.stats().frames == f0);
    CHECK(w.framesPresented() == 1);
    CHECK(std::isinf(a.nextWake()));
    // a binding change wakes exactly one frame
    m.set("n", 2.0);
    w.ui().invalidate(w.ui().root()); // model writes come from outside the UI: the host pokes the window
    a.step(0.f);
    CHECK(w.framesPresented() == 2);
    CHECK(w.ui().text(w.ui().find("l")) == "n = 2");
    // a 3 s one-shot timer: the loop sleeps ~3 s, then fires, and presents only if the timer changed the UI
    bool fired = false;
    w.ui().startTimer(3.f, false, [&](UiContext& c) { fired = true; c.setText(c.find("l"), "timer"); });
    const double t1 = v.t;
    CHECK(std::abs(a.nextWake() - 3.f) < 1e-3f);
    while (!fired) a.step();
    CHECK(v.t - t1 <= 3.5 && v.t - t1 >= 2.9);
    a.step(0.f);
    CHECK(w.framesPresented() == 3);
    // pointer hover over the button repaints (input wakes the loop)
    const Rect b = w.ui().rect(w.ui().find("b"));
    w.pointerMove({b.x + 2.f, b.y + 2.f});
    a.step(0.f);
    CHECK(w.framesPresented() == 4);
    const u64 f1 = w.framesPresented();
    for (int i = 0; i < 20; ++i) a.step();
    CHECK(w.framesPresented() == f1);
    // continuous policy presents every iteration
    w.setRedrawPolicy(app::RedrawPolicy::Continuous);
    for (int i = 0; i < 5; ++i) a.step(0.f);
    CHECK(w.framesPresented() == f1 + 5);
    return 0;
}

int multiwindow() {
    app::Application a{{.name = "gate_multi", .loadSettings = false}};
    Virt v;
    useVirtual(a, v);
    auto& w1 = a.createWindow({.title = "one", .width = 160, .height = 90});
    auto& w2 = a.createWindow({.title = "two", .width = 300, .height = 200});
    widgets::label(w1.ui(), w1.ui().root(), "first", "a");
    const NodeHandle btn = widgets::button(w2.ui(), w2.ui().root(), "second", "b");
    a.step(0.f);
    CHECK(w1.framesPresented() == 1 && w2.framesPresented() == 1);
    CHECK(w1.image().width == 160 && w2.image().width == 300 && w2.image().height == 200);
    CHECK(w1.id() != w2.id());
    // input in window 2 only repaints window 2
    const Rect r = w2.ui().rect(btn);
    w2.pointerMove({r.x + 3.f, r.y + 3.f});
    a.step(0.f);
    CHECK(w1.framesPresented() == 1 && w2.framesPresented() == 2);
    // resize forces a full frame of the new size
    w1.resize(200, 120);
    a.step(0.f);
    CHECK(w1.framesPresented() == 2 && w1.image().width == 200 && w1.image().height == 120);
    // closing windows: run() returns once the last one closes
    w1.requestClose();
    a.step(0.f);
    CHECK(a.windowCount() == 1);
    int clicks = 0;
    w2.ui().connect(btn, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) {
        if (++clicks == 1) a.window(0)->requestClose();
    });
    w2.click({r.x + 3.f, r.y + 3.f});
    CHECK(a.run() == 0);
    CHECK(a.windowCount() == 0 && clicks == 1);
    return 0;
}

int helloStartup() {
    using C = std::chrono::steady_clock;
    const auto t0 = C::now();
    app::Application a{{.name = "gate_hello", .loadSettings = false}};
    auto& w = a.createWindow({.title = "Hello", .width = 800, .height = 600});
    fuse::ui::doc::Model m;
    CHECK(w.loadText(kDoc, &m));
    a.step(0.f);
    const double ms = std::chrono::duration<double, std::milli>(C::now() - t0).count();
    std::printf("fuse_app_hello_startup: first frame after %.1f ms (headless CPU present)\n", ms);
    CHECK(w.framesPresented() == 1);
    CHECK(ms <= 150.0);
    // the presented image has the button face in it (not just the clear colour)
    const Rect b = w.ui().rect(w.ui().find("b"));
    CHECK(w.image().at(int(b.x + b.w * 0.5f), int(b.y + 2.f)) != 0xFF000000u);
    return 0;
}

int settings() {
    const char* argv[] = {"prog", "--name=Odin", "--verbose", "file.txt", "--n=2.5", "--", "--literal"};
    app::CommandLine cl(7, argv);
    CHECK(cl.get("name") == "Odin" && cl.has("verbose") && cl.get("verbose") == "1");
    CHECK(cl.number("n", 0) == 2.5 && cl.number("missing", 7) == 7);
    CHECK(cl.positional().size() == 2 && cl.positional()[0] == "file.txt" && cl.positional()[1] == "--literal");
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/fuse_app_gate/settings.ini";
    std::remove(path.c_str());
    {
        const std::string opt = "--settings=" + path;
        const char* av[] = {"prog", opt.c_str()};
        app::Application a{{.name = "gate_settings", .argc = 2, .argv = av}};
        CHECK(a.settings().path() == path && a.settings().size() == 0);
        a.settings().set("window.width", 1024.0);
        a.settings().set("note", "line1\nline2 = x\\y");
        a.settings().set("flag", 1.0);
    } // saved on exit
    app::Settings s;
    s.setPath(path);
    CHECK(s.load());
    CHECK(s.number("window.width") == 1024.0 && s.flag("flag") && s.get("note") == "line1\nline2 = x\\y");
    s.remove("flag");
    CHECK(s.dirty() && s.save() && !s.dirty());
    // single instance: a second Application with the same name is not primary until the first exits
    {
        app::Application one{{.name = "gate_single", .singleInstance = true, .loadSettings = false}};
        app::Application two{{.name = "gate_single", .singleInstance = true, .loadSettings = false}};
        CHECK(one.primary() && !two.primary());
    }
    app::Application three{{.name = "gate_single", .singleInstance = true, .loadSettings = false}};
    CHECK(three.primary());
    return 0;
}
} // namespace

int main() {
    const std::string g = FUSE_APP_GATE;
    if (g == "idle_zero_frames") idleZeroFrames();
    else if (g == "multiwindow") multiwindow();
    else if (g == "hello_startup") helloStartup();
    else if (g == "settings") settings();
    std::printf("fuse_app_%s: %s\n", g.c_str(), g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
