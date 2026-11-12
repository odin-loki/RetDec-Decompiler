// Native-window extras gate (RetDec additions), X11 only (exit 77 elsewhere / without a display), run under Xvfb:
//   * close request: a WM_DELETE_WINDOW from the window manager asks AppWindow's close-request hook; a veto keeps the
//     OS window and the AppWindow, an allow closes both
//   * punctuation keys: synthetic KeyPress events for , . / ; ' \ ` - = (and Shift+, / Shift+`) arrive as the new
//     fuse::ui::Key values, shifted ones by their unshifted key with kModShift
//   * file drop: a second X connection plays an XDND source (XdndEnter / XdndPosition / XdndDrop, answering the
//     XdndSelection conversion with a text/uri-list); the window answers XdndStatus (refused without a drop hook,
//     accepted with one), fetches the list, sends XdndFinished and delivers the paths + drop point to the hook
#include "fuse/app/native.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <dlfcn.h>
#endif

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

#if defined(_WIN32) || defined(__APPLE__)
int main() {
    std::printf("fuse_app_native_extras: skipped (X11 only)\n");
    return 77;
}
#else
namespace {
// The few Xlib entry points the test client needs (LP64 ABI layouts, as in native.cpp).
using Display = struct _XDisplay;
using Window = unsigned long;
using Atom = unsigned long;
struct KeyEv {
    int type; unsigned long serial; int send_event; Display* display; Window window, root, subwindow;
    unsigned long time; int x, y, x_root, y_root; unsigned state, keycode; int same_screen;
};
struct ClientEv { int type; unsigned long serial; int send_event; Display* display; Window window; Atom message_type; int format; long l[5]; };
struct SelReqEv { int type; unsigned long serial; int send_event; Display* display; Window owner, requestor; Atom selection, target, property; unsigned long time; };
struct SelEv { int type; unsigned long serial; int send_event; Display* display; Window requestor; Atom selection, target, property; unsigned long time; };
union XEv { int type; KeyEv key; ClientEv client; SelReqEv req; SelEv sel; long pad[24]; };
enum { KeyPress = 2, SelectionRequest = 30, SelectionNotify = 31, ClientMessage = 33 };

struct X {
    void* so = nullptr;
    Display* (*OpenDisplay)(const char*) = nullptr;
    int (*CloseDisplay)(Display*) = nullptr;
    int (*DefaultScreen)(Display*) = nullptr;
    Window (*RootWindow)(Display*, int) = nullptr;
    Window (*CreateSimpleWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = nullptr;
    Atom (*InternAtom)(Display*, const char*, int) = nullptr;
    int (*SendEvent)(Display*, Window, int, long, XEv*) = nullptr;
    int (*Flush)(Display*) = nullptr;
    int (*Sync)(Display*, int) = nullptr;
    int (*Pending)(Display*) = nullptr;
    int (*NextEvent)(Display*, XEv*) = nullptr;
    int (*SetSelectionOwner)(Display*, Atom, Window, unsigned long) = nullptr;
    int (*ChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int) = nullptr;
    unsigned char (*KeysymToKeycode)(Display*, unsigned long) = nullptr;
    int (*TranslateCoordinates)(Display*, Window, Window, int, int, int*, int*, Window*) = nullptr;
    bool load() {
        so = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(so, name));
            all = all && fn;
        };
        get(OpenDisplay, "XOpenDisplay"); get(CloseDisplay, "XCloseDisplay"); get(DefaultScreen, "XDefaultScreen");
        get(RootWindow, "XRootWindow"); get(CreateSimpleWindow, "XCreateSimpleWindow"); get(InternAtom, "XInternAtom");
        get(SendEvent, "XSendEvent"); get(Flush, "XFlush"); get(Sync, "XSync"); get(Pending, "XPending");
        get(NextEvent, "XNextEvent"); get(SetSelectionOwner, "XSetSelectionOwner"); get(ChangeProperty, "XChangeProperty");
        get(KeysymToKeycode, "XKeysymToKeycode"); get(TranslateCoordinates, "XTranslateCoordinates");
        return all;
    }
};

void pumpFor(app::Application& a, int steps) {
    for (int i = 0; i < steps; ++i) a.step(0.005f);
}
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_native_extras", .argc = argc, .argv = argv, .loadSettings = false}};
    app::NativePlatform native(a, {.vulkan = false});
    X x;
    if (!native.available() || !x.load()) {
        std::printf("fuse_app_native_extras: skipped (%.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 77;
    }
    Display* dpy = x.OpenDisplay(nullptr); // the "window manager" / drag source: a second client
    if (!dpy) return 77;
    const Window root = x.RootWindow(dpy, x.DefaultScreen(dpy));

    auto& win = a.createWindow({.title = "native extras", .width = 320, .height = 200});
    UiContext& ui = win.ui();
    const NodeHandle btn = w::button(ui, ui.root(), "focus", "btn");
    native.show(win);
    pumpFor(a, 20);
    const Window target = Window(native.nativeWindow(win));
    CHECK(target != 0);

    // ---- close request through WM_DELETE_WINDOW ----
    auto wmDelete = [&] {
        XEv e{};
        e.client.type = ClientMessage;
        e.client.window = target;
        e.client.message_type = x.InternAtom(dpy, "WM_PROTOCOLS", 0);
        e.client.format = 32;
        e.client.l[0] = long(x.InternAtom(dpy, "WM_DELETE_WINDOW", 0));
        x.SendEvent(dpy, target, 0, 0, &e);
        x.Flush(dpy);
    };
    int asked = 0;
    bool allow = false;
    win.setCloseRequestHandler([&](app::AppWindow&) { ++asked; return allow; });
    wmDelete();
    for (int i = 0; i < 200 && asked == 0; ++i) a.step(0.005f);
    pumpFor(a, 5);
    std::printf("  WM_DELETE_WINDOW (veto): asked %d, windows %u, native %llu\n", asked, a.windowCount(), native.nativeWindow(win));
    CHECK(asked == 1 && a.windowCount() == 1 && !win.closing() && native.nativeWindow(win) == target);

    // ---- punctuation keys ----
    std::vector<std::pair<Key, u8>> keys;
    ui.connect(btn, SignalKind::Event, [&keys](UiContext&, NodeHandle, const SignalArgs& s) {
        if (s.event && s.event->type == EventType::KeyDown) keys.emplace_back(s.event->key, s.event->mods);
    });
    ui.setFocus(btn, FocusReason::Keyboard);
    pumpFor(a, 2);
    struct K { unsigned long sym; unsigned state; Key want; u8 mods; };
    const K sent[] = {{',', 0, Key::Comma, 0}, {'.', 0, Key::Period, 0}, {'/', 0, Key::Slash, 0}, {';', 0, Key::Semicolon, 0},
                      {'\'', 0, Key::Quote, 0}, {'\\', 0, Key::Backslash, 0}, {'`', 0, Key::Grave, 0}, {'-', 0, Key::Minus, 0},
                      {'=', 0, Key::Equal, 0}, {',', 1, Key::Comma, kModShift}, {'`', 1 | 4, Key::Grave, kModShift | kModCtrl},
                      {'1', 1, Key::Num1, kModShift}};
    for (const K& k : sent) {
        XEv e{};
        e.key.type = KeyPress;
        e.key.window = target;
        e.key.root = root;
        e.key.state = k.state;
        e.key.keycode = x.KeysymToKeycode(dpy, k.sym);
        e.key.same_screen = 1;
        x.SendEvent(dpy, target, 0, 0, &e);
    }
    x.Flush(dpy);
    for (int i = 0; i < 200 && keys.size() < std::size(sent); ++i) a.step(0.005f);
    std::printf("  keys received %zu of %zu\n", keys.size(), std::size(sent));
    CHECK(keys.size() == std::size(sent));
    for (std::size_t i = 0; i < keys.size() && i < std::size(sent); ++i) {
        if (keys[i].first != sent[i].want || keys[i].second != sent[i].mods)
            std::printf("  key %zu: got %u mods %u, want %u mods %u\n", i, unsigned(keys[i].first), keys[i].second,
                        unsigned(sent[i].want), sent[i].mods);
        CHECK(keys[i].first == sent[i].want && keys[i].second == sent[i].mods);
    }

    // ---- XDND drop ----
    const Atom aEnter = x.InternAtom(dpy, "XdndEnter", 0), aPos = x.InternAtom(dpy, "XdndPosition", 0),
               aStatus = x.InternAtom(dpy, "XdndStatus", 0), aDrop = x.InternAtom(dpy, "XdndDrop", 0),
               aFinished = x.InternAtom(dpy, "XdndFinished", 0), aLeave = x.InternAtom(dpy, "XdndLeave", 0),
               aSel = x.InternAtom(dpy, "XdndSelection", 0), aCopy = x.InternAtom(dpy, "XdndActionCopy", 0),
               aUri = x.InternAtom(dpy, "text/uri-list", 0);
    const Window src = x.CreateSimpleWindow(dpy, root, 0, 0, 10, 10, 0, 0, 0);
    x.SetSelectionOwner(dpy, aSel, src, 0);
    auto send = [&](Atom type, long l1, long l2, long l3, long l4) {
        XEv e{};
        e.client.type = ClientMessage;
        e.client.window = target;
        e.client.message_type = type;
        e.client.format = 32;
        e.client.l[0] = long(src);
        e.client.l[1] = l1; e.client.l[2] = l2; e.client.l[3] = l3; e.client.l[4] = l4;
        x.SendEvent(dpy, target, 0, 0, &e);
        x.Flush(dpy);
    };
    const std::string uris = "file:///tmp/sample%20one.exe\r\nfile:///opt/bin/\xE2\x9C\x93.elf\r\n";
    struct SourceState { long status = -1, finished = -1, finishedAction = 0; int requests = 0; } st;
    auto source = [&] { // the drag source's event loop
        while (x.Pending(dpy)) {
            XEv e{};
            x.NextEvent(dpy, &e);
            if (e.type == ClientMessage && e.client.message_type == aStatus) st.status = e.client.l[1] & 1;
            else if (e.type == ClientMessage && e.client.message_type == aFinished) { st.finished = e.client.l[1] & 1; st.finishedAction = e.client.l[2]; }
            else if (e.type == SelectionRequest) {
                ++st.requests;
                const SelReqEv r = e.req;
                x.ChangeProperty(dpy, r.requestor, r.property, aUri, 8, 0, reinterpret_cast<const unsigned char*>(uris.data()), int(uris.size()));
                XEv n{};
                n.sel.type = SelectionNotify;
                n.sel.requestor = r.requestor;
                n.sel.selection = r.selection;
                n.sel.target = r.target;
                n.sel.property = r.property;
                n.sel.time = r.time;
                x.SendEvent(dpy, r.requestor, 0, 0, &n);
                x.Flush(dpy);
            }
        }
    };
    auto until = [&](auto done) {
        for (int i = 0; i < 400 && !done(); ++i) { a.step(0.005f); x.Sync(dpy, 0); source(); }
    };
    int rx = 0, ry = 0;
    Window child = 0;
    x.TranslateCoordinates(dpy, target, root, 60, 40, &rx, &ry, &child);
    const long posWord = (long(rx) << 16) | long(ry);

    // 1) no drop hook: the window refuses the drag
    send(aEnter, 5L << 24, long(aUri), 0, 0);
    send(aPos, 0, posWord, 0, long(aCopy));
    until([&] { return st.status >= 0; });
    std::printf("  XdndStatus without a hook: %ld\n", st.status);
    CHECK(st.status == 0);
    send(aLeave, 0, 0, 0, 0);

    // 2) with a hook: accepted, dropped, fetched, finished, delivered
    std::vector<app::FileDrop> drops;
    win.setFileDropHandler([&](app::AppWindow&, const app::FileDrop& d) { drops.push_back(d); });
    st = {};
    send(aEnter, 5L << 24, long(aUri), 0, 0);
    send(aPos, 0, posWord, 0, long(aCopy));
    until([&] { return st.status >= 0; });
    std::printf("  XdndStatus with a hook: %ld\n", st.status);
    CHECK(st.status == 1);
    send(aDrop, 0, 0 /*CurrentTime*/, 0, 0);
    until([&] { return st.finished >= 0 && !drops.empty(); });
    std::printf("  selection requests %d, XdndFinished accepted %ld (action %s), drops %zu\n", st.requests, st.finished,
                st.finishedAction == long(aCopy) ? "copy" : "?", drops.size());
    CHECK(st.requests == 1 && st.finished == 1 && st.finishedAction == long(aCopy));
    CHECK(drops.size() == 1);
    if (!drops.empty()) {
        const app::FileDrop& d = drops[0];
        for (const std::string& p : d.paths) std::printf("    dropped \"%s\"\n", p.c_str());
        std::printf("    at (%.0f, %.0f)\n", double(d.pos.x), double(d.pos.y));
        CHECK(d.paths.size() == 2 && d.paths[0] == "/tmp/sample one.exe" && d.paths[1] == "/opt/bin/\xE2\x9C\x93.elf");
        CHECK(d.pos.x == 60.f && d.pos.y == 40.f);
        CHECK(d.target == btn || ui.alive(d.target)); // hit-tested
    }

    // ---- close request allowed: the OS window and the AppWindow go away ----
    allow = true;
    wmDelete();
    for (int i = 0; i < 200 && a.windowCount() != 0; ++i) a.step(0.005f);
    std::printf("  WM_DELETE_WINDOW (allow): asked %d, windows %u\n", asked, a.windowCount());
    CHECK(asked == 2 && a.windowCount() == 0);

    x.CloseDisplay(dpy);
    std::printf("fuse_app_native_extras: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
#endif
