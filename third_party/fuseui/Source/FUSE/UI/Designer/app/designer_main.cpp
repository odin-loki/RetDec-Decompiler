// fuse_ui_designer: the UI designer prototype in an application window.
//   fuse_ui_designer [file.fuseui] [--save=out.fuseui] [--screenshot=out.png]
// Ctrl+Z / Ctrl+Y undo / redo; Enter in the palette adds a widget under the selection; click the canvas to select.
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/ui/designer/designer.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "fuse_ui_designer", .argc = argc, .argv = argv}};
    auto& win = app.createWindow({.title = "FUSE UI Designer", .width = 1280, .height = 760});
    fuse::ui::UiContext& ui = win.ui();
    ui.setCss(ui.root(), "padding", "0px");
    fuse::ui::designer::Designer d(ui);
    std::string file;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-') file = argv[i];
    if (!file.empty()) {
        std::string err;
        if (!d.openFile(file, &err)) return app.fail(err.c_str());
    }
    if (const std::string_view out = app.args().get("save"); !out.empty())
        if (!d.saveFile(std::string(out))) return app.fail("cannot save");
    if (const std::string_view shot = app.args().get("screenshot"); !shot.empty()) {
        app.step(0.016f);
        return win.writePng(std::string(shot)) ? 0 : app.fail("cannot write screenshot");
    }
    fuse::app::NativePlatform native(app);
    native.show(win);
    return app.run();
}
