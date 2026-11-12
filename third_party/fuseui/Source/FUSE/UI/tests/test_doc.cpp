// fuse_ui_doc_load: .fuseui JSON parser (valid / invalid / unicode), document build (classes, styles, properties,
// actions), bindings (one-way templates, two-way value / checked / text, conditional disabled / visible), refresh is
// incremental and allocation-free when nothing changed, hot reload keeps per-id state and focus, error paths.
#include "fuse/ui/doc/document.hpp"
#include "test_util.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

static std::atomic<unsigned long long> g_allocs{0};
void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::doc;

static void testJson() {
    auto v = parseJson(R"({"a": [1, -2.5e1, true, null, "x\u00e9\ud83d\ude00\n"], "b": {}})");
    UI_CHECK(v.has_value());
    if (v) {
        const JsonValue* a = v->get("a");
        UI_CHECK(a && a->isArray() && a->array.size() == 5);
        UI_CHECK_EQ(a->array[1].number, -25.0);
        UI_CHECK(a->array[2].boolean && a->array[3].isNull());
        UI_CHECK(a->array[4].string == "x\xC3\xA9\xF0\x9F\x98\x80\n");
        UI_CHECK(v->get("b") && v->get("b")->isObject());
    }
    for (const char* bad : {"{", "[1,,2]", "{\"a\" 1}", "01", "\"\\ud800\"", "tru", "{} x", "\"a\nb\""}) {
        auto e = parseJson(bad);
        UI_CHECK(!e.has_value());
    }
    auto e = parseJson("{\n  \"a\": 1,\n  \"b\": ?\n}");
    UI_CHECK(!e && e.error().line == 3 && e.error().column == 8);
    std::string deep(1000, '[');
    UI_CHECK(!parseJson(deep).has_value()); // depth limit, no stack overflow
}

static void testExpr() {
    Model m;
    m.set("a", 3.0);
    m.set("b.c", 4.0);
    auto e = Expr::compile("-(a + b.c) * 2 % 5 + (a < b.c && !0)", m);
    UI_CHECK(e.has_value());
    if (e) UI_CHECK_EQ(e->eval(m), std::fmod(-14.0, 5.0) + 1.0);
    UI_CHECK(!Expr::compile("a +", m).has_value());
    UI_CHECK(!Expr::compile("a $ b", m).has_value());
    auto t = Template::compile("x={a:2} {{lit}} {b.c}", m);
    UI_CHECK(t.has_value());
    std::string out;
    if (t) t->render(m, out);
    UI_CHECK(out == "x=3.00 {lit} 4");
    UI_CHECK(!Template::compile("{=a + 1}", m).has_value());
}

int main() {
    testJson();
    testExpr();

    UiContext ui({.viewport = {400, 400}, .font = &font()});
    widgets::install(ui);
    std::string json;
    UI_CHECK(readTextFile(FUSE_UI_TEST_DATA "/settings.fuseui", json));
    Model model;
    model.set("player.health", 30.0);
    model.set("player.maxHealth", 40.0);
    model.set("player.name", std::string_view("Odin"));
    model.set("audio.volume", 0.25);
    auto loaded = Document::load(ui, json, &model);
    if (!loaded) {
        std::fprintf(stderr, "load failed: %s: %s\n", loaded.error().path.c_str(), loaded.error().message.c_str());
        return 1;
    }
    Document& d = **loaded;
    std::vector<std::string> actions;
    d.onAction([&actions](std::string_view a, NodeHandle, const SignalArgs&) { actions.emplace_back(a); });
    ui.update();

    UI_CHECK(ui.find("settings") == d.root());
    UI_CHECK(ui.hasClass(d.root(), "card"));
    UI_CHECK_EQ(ui.rect(d.root()).w, 320.f);
    UI_CHECK(ui.text(ui.find("hp")) == "HP 75%");
    UI_CHECK(ui.text(ui.find("who")) == "Player: Odin");
    UI_CHECK(ui.editBuffer(ui.find("nick")).text() == "Odin");
    UI_CHECK_EQ(ui.value(ui.find("vol")), 0.25f);
    UI_CHECK_EQ(ui.value(ui.find("quality")), 1.f);
    UI_CHECK(!ui.disabled(ui.find("save")));
    UI_CHECK(!ui.computed(ui.find("warn")).visible);
    UI_CHECK_EQ(d.bindingCount(), 7u);

    // one-way: model -> UI, incremental
    model.set("player.health", 10.0);
    UI_CHECK_EQ(d.refresh(), 1u);
    UI_CHECK(ui.text(ui.find("hp")) == "HP 25%");
    UI_CHECK_EQ(d.refresh(), 0u);
    model.set("busy", 1.0);
    d.refresh();
    UI_CHECK(ui.disabled(ui.find("save")));
    model.set("busy", 0.0);
    d.refresh();
    ui.update();

    // two-way: UI -> model
    UiTestDriver drv(ui);
    drv.click("mute");
    UI_CHECK_EQ(model.number("audio.mute"), 1.0);
    d.refresh();
    ui.update();
    UI_CHECK(ui.computed(ui.find("warn")).visible);
    ui.setFocus(ui.find("vol"), FocusReason::Keyboard);
    drv.key(Key::Home);
    UI_CHECK_EQ(model.number("audio.volume"), 0.0);
    d.refresh();
    UI_CHECK(ui.disabled(ui.find("save"))); // volume <= 0
    model.set("audio.volume", 0.5);
    d.refresh();
    ui.update();
    ui.setFocus(ui.find("nick"), FocusReason::Keyboard);
    drv.key(Key::End);
    drv.type("!");
    UI_CHECK(model.string("player.name") == "Odin!");
    d.refresh();
    UI_CHECK(ui.text(ui.find("who")) == "Player: Odin!");

    // actions
    ui.update();
    drv.click("save");
    UI_CHECK(actions.size() == 1 && actions[0] == "save");

    // steady state: refresh with no model change does not allocate
    ui.update();
    const auto before = g_allocs.load();
    for (int i = 0; i < 100; ++i) {
        d.refresh();
        ui.update(1.f / 60.f);
    }
    UI_CHECK_EQ(g_allocs.load() - before, 0ull);
    // value-change refresh of a numeric template reuses the scratch buffer
    model.set("player.health", 20.0);
    d.refresh();
    const auto before2 = g_allocs.load();
    model.set("player.health", 21.0);
    d.refresh();
    UI_CHECK_EQ(g_allocs.load() - before2, 0ull);

    // hot reload: unbound per-id state (combo selection) and focus survive; bound state follows the model
    ui.setValue(ui.find("quality"), 2.f);
    ui.setFocus(ui.find("quality"), FocusReason::Keyboard);
    std::string json2 = json;
    json2.replace(json2.find("\"Save\""), 6, "\"Apply\"");
    auto rr = d.reload(json2);
    UI_CHECK(rr.has_value());
    ui.update();
    UI_CHECK(ui.text(ui.find("save")) == "Apply");
    UI_CHECK_EQ(ui.value(ui.find("quality")), 2.f);
    UI_CHECK(ui.focused() == ui.find("quality"));
    UI_CHECK(ui.text(ui.find("who")) == "Player: Odin!");
    drv.click("save");
    UI_CHECK_EQ(actions.size(), 2u);

    // errors carry a node path
    auto bad = Document::load(ui, R"({"fuseui":1,"root":{"children":[{"class":"nope"}]}})", &model);
    UI_CHECK(!bad && bad.error().path == "root/children[0]");
    auto bad2 = Document::load(ui, R"({"fuseui":1,"root":{"class":"label","text":"{a +}"}})", &model);
    UI_CHECK(!bad2.has_value());
    auto bad3 = Document::load(ui, R"({"fuseui":2,"root":{}})", &model);
    UI_CHECK(!bad3.has_value());
    auto bad4 = Document::load(ui, R"({"fuseui":1,"root":{"class":"label","text":"{x}"}})", nullptr);
    UI_CHECK(!bad4.has_value());
    const u32 nodes = ui.nodeCount();
    loaded->reset();
    UI_CHECK(ui.nodeCount() < nodes); // destroying the document destroys its tree
    UI_CHECK(!ui.find("settings"));
    return finish("fuse_ui_doc_load");
}
