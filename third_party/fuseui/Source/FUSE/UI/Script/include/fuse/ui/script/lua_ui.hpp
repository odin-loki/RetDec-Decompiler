// FUSE UI Lua bindings (fuse_ui_script, plan §4.9): a global `ui` table over one UiContext and one binding Model.
//
//   local h = ui.find("save")            -- node handle (number) or nil
//   ui.setText(h, "Saved"); ui.text(h)
//   ui.setValue(h, 0.5); ui.value(h); ui.setChecked(h, true); ui.checked(h); ui.setDisabled(h, true)
//   ui.on("save", "clicked", function(h, args) ... end)   -- args = { value=, flag=, text= }
//   ui.set("player.health", 42); ui.get("player.health")  -- binding model
//   local root = ui.load(json_text)      -- loads a .fuseui document; its actions call ui.actions[name](h, args)
//   ui.loadFile("ui/main.fuseui"); ui.refresh()            -- re-evaluates bindings (also done by LuaUi::update)
//   ui.create("button", parentHandle, "id"); ui.destroy(h); ui.addClass(h, "primary"); ui.css(h, "width", "80px")
//
// Errors are raised as Lua errors (lua_error), never C++ exceptions. Handles are generation-checked: a stale handle is
// treated as nil by every getter and ignored by setters.
#pragma once

#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/ui.hpp"

#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace fuse::ui::script {

class LuaUi {
public:
    /// Installs the `ui` table into `L`. The LuaUi must outlive every call into it from `L`.
    LuaUi(lua_State* L, UiContext& ui, doc::Model& model);
    ~LuaUi();
    LuaUi(const LuaUi&) = delete;
    LuaUi& operator=(const LuaUi&) = delete;

    /// Per-frame: refreshes every loaded document's bindings.
    void update();
    /// Runs a chunk; on error returns false and fills `err`.
    bool run(std::string_view chunk, std::string* err = nullptr, const char* name = "=fuseui");

    UiContext& context() { return m_ui; }
    doc::Model& model() { return m_model; }
    lua_State* state() { return m_L; }
    const std::vector<std::unique_ptr<doc::Document>>& documents() const { return m_docs; }

    static double pack(NodeHandle h) { return double((u64(h.generation) << 32) | h.index); }
    static NodeHandle unpack(double d) {
        const u64 v = u64(d);
        return {u32(v & 0xFFFFFFFFu), u32(v >> 32)};
    }

    // implementation detail (C callbacks)
    std::vector<std::unique_ptr<doc::Document>> m_docs;
    std::vector<int> m_refs;
    void dispatch(int ref, NodeHandle h, const SignalArgs& a);
    void action(std::string_view name, NodeHandle h, const SignalArgs& a);

private:
    lua_State* m_L;
    UiContext& m_ui;
    doc::Model& m_model;
};

} // namespace fuse::ui::script
