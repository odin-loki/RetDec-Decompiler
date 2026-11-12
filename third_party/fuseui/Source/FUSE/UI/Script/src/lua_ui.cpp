#include "fuse/ui/script/lua_ui.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdio>
#include <string>

namespace fuse::ui::script {

namespace {
LuaUi& self(lua_State* L) { return *static_cast<LuaUi*>(lua_touserdata(L, lua_upvalueindex(1))); }

NodeHandle nodeArg(lua_State* L, LuaUi& s, int idx) {
    if (lua_type(L, idx) == LUA_TSTRING) return s.context().find(lua_tostring(L, idx));
    if (lua_type(L, idx) != LUA_TNUMBER) return {};
    const NodeHandle h = LuaUi::unpack(lua_tonumber(L, idx));
    return s.context().alive(h) ? h : NodeHandle{};
}
void pushNode(lua_State* L, NodeHandle h) {
    if (h) lua_pushnumber(L, LuaUi::pack(h));
    else lua_pushnil(L);
}
void pushArgs(lua_State* L, const SignalArgs& a) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, a.value);
    lua_setfield(L, -2, "value");
    lua_pushboolean(L, a.flag);
    lua_setfield(L, -2, "flag");
    lua_pushlstring(L, a.text.data(), a.text.size());
    lua_setfield(L, -2, "text");
}

int l_find(lua_State* L) { pushNode(L, self(L).context().find(luaL_checkstring(L, 1))); return 1; }
int l_text(lua_State* L) {
    auto& s = self(L);
    const NodeHandle h = nodeArg(L, s, 1);
    if (!h) { lua_pushnil(L); return 1; }
    const std::string_view t = s.context().text(h);
    lua_pushlstring(L, t.data(), t.size());
    return 1;
}
int l_setText(lua_State* L) {
    auto& s = self(L);
    size_t n = 0;
    const char* t = luaL_checklstring(L, 2, &n);
    if (const NodeHandle h = nodeArg(L, s, 1)) {
        s.context().setText(h, {t, n});
        if (s.context().registry().get(s.context().classOf(h)).name == "lineedit") s.context().editBuffer(h).setText({t, n});
    }
    return 0;
}
int l_value(lua_State* L) {
    auto& s = self(L);
    const NodeHandle h = nodeArg(L, s, 1);
    if (!h) lua_pushnil(L);
    else lua_pushnumber(L, s.context().value(h));
    return 1;
}
int l_setValue(lua_State* L) {
    auto& s = self(L);
    const double v = luaL_checknumber(L, 2);
    if (const NodeHandle h = nodeArg(L, s, 1)) s.context().setValue(h, f32(v), lua_toboolean(L, 3));
    return 0;
}
int l_checked(lua_State* L) {
    auto& s = self(L);
    const NodeHandle h = nodeArg(L, s, 1);
    if (!h) lua_pushnil(L);
    else lua_pushboolean(L, s.context().checked(h));
    return 1;
}
int l_setChecked(lua_State* L) {
    auto& s = self(L);
    if (const NodeHandle h = nodeArg(L, s, 1)) s.context().setChecked(h, lua_toboolean(L, 2));
    return 0;
}
int l_setDisabled(lua_State* L) {
    auto& s = self(L);
    if (const NodeHandle h = nodeArg(L, s, 1)) s.context().setDisabled(h, lua_toboolean(L, 2));
    return 0;
}
int l_focus(lua_State* L) {
    auto& s = self(L);
    if (lua_gettop(L) == 0) { pushNode(L, s.context().focused()); return 1; }
    if (const NodeHandle h = nodeArg(L, s, 1)) s.context().setFocus(h);
    return 0;
}
int l_addClass(lua_State* L) {
    auto& s = self(L);
    const char* c = luaL_checkstring(L, 2);
    if (const NodeHandle h = nodeArg(L, s, 1)) s.context().addClass(h, c);
    return 0;
}
int l_removeClass(lua_State* L) {
    auto& s = self(L);
    const char* c = luaL_checkstring(L, 2);
    if (const NodeHandle h = nodeArg(L, s, 1)) s.context().removeClass(h, c);
    return 0;
}
int l_css(lua_State* L) {
    auto& s = self(L);
    const char* p = luaL_checkstring(L, 2);
    const char* v = luaL_checkstring(L, 3);
    const NodeHandle h = nodeArg(L, s, 1);
    lua_pushboolean(L, h && s.context().setCss(h, p, v));
    return 1;
}
int l_create(lua_State* L) {
    auto& s = self(L);
    const char* cls = luaL_checkstring(L, 1);
    NodeHandle parent = lua_isnoneornil(L, 2) ? s.context().root() : nodeArg(L, s, 2);
    if (!parent) return luaL_error(L, "ui.create: bad parent");
    const char* id = luaL_optstring(L, 3, "");
    auto n = s.context().create(cls, parent, id);
    if (!n) return luaL_error(L, "ui.create: unknown class '%s'", cls);
    pushNode(L, *n);
    return 1;
}
int l_destroy(lua_State* L) {
    auto& s = self(L);
    if (const NodeHandle h = nodeArg(L, s, 1)) s.context().destroy(h);
    return 0;
}
int l_on(lua_State* L) {
    auto& s = self(L);
    const NodeHandle h = nodeArg(L, s, 1);
    if (!h) return luaL_error(L, "ui.on: no such node");
    const std::string_view sig = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    SignalKind k;
    if (sig == "clicked") k = SignalKind::Clicked;
    else if (sig == "toggled") k = SignalKind::Toggled;
    else if (sig == "valueChanged") k = SignalKind::ValueChanged;
    else if (sig == "textChanged") k = SignalKind::TextChanged;
    else if (sig == "submitted") k = SignalKind::Submitted;
    else if (sig == "focusChanged") k = SignalKind::FocusChanged;
    else return luaL_error(L, "ui.on: unknown signal '%s'", sig.data());
    lua_pushvalue(L, 3);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s.m_refs.push_back(ref);
    LuaUi* p = &s;
    s.context().connect(h, k, [p, ref](UiContext&, NodeHandle n, const SignalArgs& a) { p->dispatch(ref, n, a); });
    return 0;
}
int l_set(lua_State* L) {
    auto& s = self(L);
    const char* name = luaL_checkstring(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) s.model().set(name, std::string_view(lua_tostring(L, 2)));
    else if (lua_type(L, 2) == LUA_TBOOLEAN) s.model().set(name, lua_toboolean(L, 2) ? 1.0 : 0.0);
    else s.model().set(name, double(luaL_checknumber(L, 2)));
    return 0;
}
int l_get(lua_State* L) {
    auto& s = self(L);
    const char* name = luaL_checkstring(L, 1);
    const u32 slot = s.model().find(name);
    if (slot == 0xFFFFFFFFu) lua_pushnil(L);
    else if (s.model().isString(slot)) {
        const auto t = s.model().stringAt(slot);
        lua_pushlstring(L, t.data(), t.size());
    } else lua_pushnumber(L, s.model().numberAt(slot));
    return 1;
}
int loadText(lua_State* L, LuaUi& s, std::string_view json, int parentIdx) {
    const NodeHandle parent = lua_isnoneornil(L, parentIdx) ? s.context().root() : nodeArg(L, s, parentIdx);
    auto d = doc::Document::load(s.context(), json, &s.model(), parent);
    if (!d) {
        const auto& e = d.error();
        return luaL_error(L, "fuseui: %s%s%s (line %d col %d)", e.path.c_str(), e.path.empty() ? "" : ": ",
                          e.message.c_str(), int(e.line), int(e.column));
    }
    LuaUi* p = &s;
    (*d)->onAction([p](std::string_view a, NodeHandle h, const SignalArgs& args) { p->action(a, h, args); });
    pushNode(L, (*d)->root());
    s.m_docs.push_back(std::move(*d));
    return 1;
}
int l_load(lua_State* L) {
    size_t n = 0;
    const char* t = luaL_checklstring(L, 1, &n);
    return loadText(L, self(L), {t, n}, 2);
}
int l_loadFile(lua_State* L) {
    std::string text;
    const char* path = luaL_checkstring(L, 1);
    if (!doc::readTextFile(path, text)) return luaL_error(L, "ui.loadFile: cannot read '%s'", path);
    return loadText(L, self(L), text, 2);
}
int l_refresh(lua_State* L) {
    u32 n = 0;
    for (auto& d : self(L).m_docs) n += d->refresh();
    lua_pushnumber(L, n);
    return 1;
}
} // namespace

LuaUi::LuaUi(lua_State* L, UiContext& ui, doc::Model& model) : m_L(L), m_ui(ui), m_model(model) {
    static const luaL_Reg fns[] = {
        {"find", l_find},         {"text", l_text},           {"setText", l_setText},
        {"value", l_value},       {"setValue", l_setValue},   {"checked", l_checked},
        {"setChecked", l_setChecked}, {"setDisabled", l_setDisabled}, {"focus", l_focus},
        {"addClass", l_addClass}, {"removeClass", l_removeClass}, {"css", l_css},
        {"create", l_create},     {"destroy", l_destroy},     {"on", l_on},
        {"set", l_set},           {"get", l_get},             {"load", l_load},
        {"loadFile", l_loadFile}, {"refresh", l_refresh},     {nullptr, nullptr}};
    lua_newtable(L);
    lua_pushlightuserdata(L, this);
    luaL_setfuncs(L, fns, 1);
    lua_newtable(L);
    lua_setfield(L, -2, "actions");
    lua_setglobal(L, "ui");
}

LuaUi::~LuaUi() {
    m_docs.clear();
    for (int r : m_refs) luaL_unref(m_L, LUA_REGISTRYINDEX, r);
}

void LuaUi::update() {
    for (auto& d : m_docs) d->refresh();
}

bool LuaUi::run(std::string_view chunk, std::string* err, const char* name) {
    if (luaL_loadbuffer(m_L, chunk.data(), chunk.size(), name) != LUA_OK || lua_pcall(m_L, 0, 0, 0) != LUA_OK) {
        if (err) *err = lua_tostring(m_L, -1) ? lua_tostring(m_L, -1) : "lua error";
        lua_pop(m_L, 1);
        return false;
    }
    return true;
}

void LuaUi::dispatch(int ref, NodeHandle h, const SignalArgs& a) {
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, ref);
    pushNode(m_L, h);
    pushArgs(m_L, a);
    if (lua_pcall(m_L, 2, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "fuse_ui_script: %s\n", lua_tostring(m_L, -1));
        lua_pop(m_L, 1);
    }
}

void LuaUi::action(std::string_view name, NodeHandle h, const SignalArgs& a) {
    lua_getglobal(m_L, "ui");
    lua_getfield(m_L, -1, "actions");
    lua_pushlstring(m_L, name.data(), name.size());
    lua_gettable(m_L, -2);
    if (!lua_isfunction(m_L, -1)) {
        lua_pop(m_L, 3);
        return;
    }
    pushNode(m_L, h);
    pushArgs(m_L, a);
    if (lua_pcall(m_L, 2, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "fuse_ui_script: action '%.*s': %s\n", int(name.size()), name.data(), lua_tostring(m_L, -1));
        lua_pop(m_L, 1);
    }
    lua_pop(m_L, 2);
}

} // namespace fuse::ui::script
