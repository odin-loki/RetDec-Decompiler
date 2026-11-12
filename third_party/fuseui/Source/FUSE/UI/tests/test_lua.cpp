// fuse_ui_lua_bindings: the `ui` Lua table — find / text / value / checked, signal handlers, model get / set, loading
// a .fuseui document whose actions dispatch to ui.actions, create / destroy, stale handles, Lua errors (no throw).
#include "fuse/ui/script/lua_ui.hpp"
#include "test_util.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {400, 400}, .font = &font()});
    widgets::install(ui);
    doc::Model model;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    {
        script::LuaUi lua(L, ui, model);
        std::string err;
        const bool ok = lua.run(R"(
            ui.set("player.health", 30); ui.set("player.maxHealth", 40); ui.set("player.name", "Odin")
            ui.set("audio.volume", 0.5)
            saves = 0
            ui.actions.save = function(h, args) saves = saves + 1; ui.set("busy", 1) end
            root = ui.loadFile(")" FUSE_UI_TEST_DATA R"(/settings.fuseui")
            assert(root == ui.find("settings"))
            assert(ui.text("hp") == "HP 75%", ui.text("hp"))
            assert(ui.value("quality") == 1)
            toggles = 0
            ui.on("mute", "toggled", function(h, a) toggles = toggles + 1; last = a.flag end)
            local b = ui.create("button", root, "extra")
            ui.setText(b, "Extra"); assert(ui.text("extra") == "Extra")
            ui.addClass(b, "primary"); assert(ui.css(b, "width", "90px"))
            ui.setValue("vol", 0.75, true)       -- emit: two-way writes the model
            assert(ui.get("audio.volume") == 0.75)
        )", &err);
        if (!ok) std::fprintf(stderr, "lua: %s\n", err.c_str());
        UI_CHECK(ok);
        ui.update();
        UiTestDriver drv(ui);
        drv.click("mute");
        drv.click("save");
        lua.update();
        UI_CHECK(lua.run(R"(
            assert(toggles == 1 and last == true)
            assert(saves == 1)
            assert(ui.get("audio.mute") == 1)
            local e = ui.find("extra"); ui.destroy(e)
            assert(ui.text(e) == nil)          -- stale handle reads as nil
            ui.setText(e, "x")                 -- and is ignored by setters
            assert(ui.find("extra") == nil)
            ui.set("player.health", 10); assert(ui.refresh() >= 1); assert(ui.text("hp") == "HP 25%")
        )", &err));
        if (!err.empty()) std::fprintf(stderr, "lua: %s\n", err.c_str());
        UI_CHECK(ui.disabled(ui.find("save"))); // busy = 1 from the action
        // errors become Lua errors with a message, never exceptions
        err.clear();
        UI_CHECK(!lua.run(R"(ui.load('{"fuseui":1,"root":{"class":"nope"}}'))", &err));
        UI_CHECK(err.find("unknown class") != std::string::npos);
        UI_CHECK(!lua.run(R"(ui.on("nope", "clicked", function() end))", &err));
        UI_CHECK(!lua.run(R"(ui.create("nope"))", &err));
        UI_CHECK_EQ(lua.documents().size(), 1u);
    }
    lua_close(L);
    return finish("fuse_ui_lua_bindings");
}
