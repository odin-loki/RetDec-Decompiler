// notes_app (plan UI5 sample): a split view with a note list (item model) and an editor, menu bar, settings store
// (notes persist in the app's settings.ini), autosave timer.
//   notes_app [--settings=path] [--screenshot=out.png]
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"

#include <string>
#include <vector>

namespace w = fuse::ui::widgets;
using namespace fuse::ui;

namespace {
struct NotesModel {
    std::vector<std::string>* titles;
    w::ItemModel model;
};
} // namespace

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "notes_app", .argc = argc, .argv = argv, .singleInstance = true}};
    if (!app.primary() && !app.args().has("screenshot")) return app.fail("already running");
    auto& win = app.createWindow({.title = "Notes", .width = 640, .height = 400});
    UiContext& ui = win.ui();
    auto& st = app.settings();

    std::vector<std::string> titles, bodies;
    const int n = int(st.number("notes.count", 0));
    for (int i = 0; i < n; ++i) {
        titles.emplace_back(st.get("notes." + std::to_string(i) + ".title", "Untitled"));
        bodies.emplace_back(st.get("notes." + std::to_string(i) + ".body"));
    }
    if (titles.empty()) {
        titles = {"Welcome", "Shopping", "Ideas"};
        bodies = {"Notes are kept in settings.ini.", "Milk, eggs, coffee", "A UI toolkit inside a game engine"};
    }
    NotesModel nm{&titles, {}};
    nm.model.user = &nm;
    nm.model.columns = 1;
    nm.model.rows = [](const void* u) { return u32(static_cast<const NotesModel*>(u)->titles->size()); };
    nm.model.data = [](const void* u, u32 r, u32, std::string&) { return std::string_view((*static_cast<const NotesModel*>(u)->titles)[r]); };

    ui.setCss(ui.root(), "padding", "0px");
    ui.setCss(ui.root(), "gap", "0px");
    const NodeHandle bar = w::menuBar(ui, ui.root(), "menubar");
    w::menuBarMenu(ui, bar, "File", "New note\nDelete note\n-\nQuit", "file");
    const NodeHandle split = w::splitter(ui, ui.root(), false, "split");
    ui.setCss(split, "flex-grow", "1");
    const NodeHandle left = w::splitterPane(ui, split, 180.f);
    const NodeHandle right = w::splitterPane(ui, split);
    ui.setCss(right, "padding", "8px");
    ui.setCss(right, "gap", "6px");
    const NodeHandle list = w::modelListView(ui, left, &nm.model, "list");
    ui.setCss(list, "flex-grow", "1");
    const NodeHandle title = w::lineEdit(ui, right, "", "title");
    const NodeHandle body = w::lineEdit(ui, right, "", "body");
    const NodeHandle status = w::label(ui, right, "", "status");

    struct State {
        fuse::app::Application& app;
        UiContext& ui;
        fuse::app::Settings& st;
        std::vector<std::string>& titles;
        std::vector<std::string>& bodies;
        NodeHandle list, title, body, status;
        int current = -1;
        bool dirty = false;
        void save() {
            st.set("notes.count", double(titles.size()));
            for (std::size_t i = 0; i < titles.size(); ++i) {
                st.set("notes." + std::to_string(i) + ".title", titles[i]);
                st.set("notes." + std::to_string(i) + ".body", bodies[i]);
            }
            st.save();
            dirty = false;
            ui.setText(status, "Saved " + std::to_string(titles.size()) + " notes");
        }
        void open(int i) {
            current = i;
            const bool ok = i >= 0 && i < int(titles.size());
            const std::string t = ok ? titles[std::size_t(i)] : "", b = ok ? bodies[std::size_t(i)] : "";
            ui.setText(title, t);
            ui.editBuffer(title).setText(t);
            ui.setText(body, b);
            ui.editBuffer(body).setText(b);
            ui.setValue(list, f32(i));
        }
        void menu(int item) {
            switch (item) {
            case 0: titles.emplace_back("New note"); bodies.emplace_back(); w::tableModelChanged(ui, list); open(int(titles.size()) - 1); dirty = true; break;
            case 1:
                if (current >= 0) {
                    titles.erase(titles.begin() + current);
                    bodies.erase(bodies.begin() + current);
                    w::tableModelChanged(ui, list);
                    open(std::min(current, int(titles.size()) - 1));
                    dirty = true;
                }
                break;
            case 3: app.quit(); break;
            default: break;
            }
        }
    } S{app, ui, st, titles, bodies, list, title, body, status};
    State* s = &S;
    ui.connect(list, SignalKind::ValueChanged, [s](UiContext&, NodeHandle, const SignalArgs& a) { s->open(int(a.value)); });
    ui.connect(title, SignalKind::TextChanged, [s](UiContext& c, NodeHandle, const SignalArgs& a) {
        if (s->current < 0) return;
        s->titles[std::size_t(s->current)] = std::string(a.text);
        w::tableModelChanged(c, s->list);
        s->dirty = true;
    });
    ui.connect(body, SignalKind::TextChanged, [s](UiContext&, NodeHandle, const SignalArgs& a) {
        if (s->current >= 0) { s->bodies[std::size_t(s->current)] = std::string(a.text); s->dirty = true; }
    });
    ui.connect(ui.find("file"), SignalKind::Submitted, [s](UiContext&, NodeHandle, const SignalArgs& a) { s->menu(int(a.value)); });
    // autosave: a 2 s repeating timer; the on-demand loop wakes only for it while idle
    ui.startTimer(2.f, true, [s](UiContext&) { if (s->dirty) s->save(); });
    S.open(0);

    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        app.step(0.f);
        const Rect r = ui.rect(list);
        win.click({r.x + 20.f, r.y + 34.f}); // second note
        app.step(0.f);
        win.ui().setFocus(body);
        win.key(Key::End);
        win.text(" and bread");
        app.step(0.f);
        win.ui().pointerMove({620.f, 390.f});
        app.step(0.f);
        if (!win.writePng(std::string(shot))) return app.fail("cannot write screenshot");
        if (S.dirty) S.save();
        std::printf("notes_app: %llu frames, %zu notes, wrote %.*s\n", static_cast<unsigned long long>(win.framesPresented()),
                    titles.size(), int(shot.size()), shot.data());
        return 0;
    }
    // a real OS window (X11 / Win32) with the Vulkan swapchain presenter; headless when there is no display
    fuse::app::NativePlatform native(app);
    const fuse::app::PresenterKind kind = native.show(win);
    static const char* const kKinds[] = {"headless", "vulkan", "gdi"};
    std::fprintf(stderr, "notes_app: presenter %s%s%.*s%s\n", kKinds[int(kind)], native.gpuName().empty() ? "" : " on ",
                 int(native.gpuName().size()), native.gpuName().data(), native.gpuDraws(win) ? " (GPU draw list)" : "");
    if (const std::string_view shot = app.args().get("window-shot"); !shot.empty()) {
        // smoke run: present a few frames into the OS window, read the window back, write it, exit
        for (int i = 0; i < 200 && win.framesPresented() < 3; ++i) {
            win.requestRedraw();
            app.step(0.02f);
        }
        fuse::app::AppWindow& shown = win;
        fuse::ui::Image img;
        if (!native.capture(shown, img)) return app.fail("cannot read the OS window back");
        const bool wrote = fuse::app::writePng(img, std::string(shot));
        return wrote ? 0 : app.fail("cannot write window shot");
    }
    const int code = app.run();
    if (S.dirty) S.save();
    return code;
}
