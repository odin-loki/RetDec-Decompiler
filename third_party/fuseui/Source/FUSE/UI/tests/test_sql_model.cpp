// fuse_ui_sql gate: SqliteModel over an in-memory database with 200k rows behind a tableView.
//   - headers / column count come from the statement, rows from count(*)
//   - only the visible window is read: scrolling to the bottom costs a handful of window queries, not 200k reads
//   - header click -> sortBy (ORDER BY around the query), writes + refresh, NULL cells, SQL errors reported
// Optional --out=<dir>: sql_table.png.
#include "test_util.hpp"

#include "fuse/ui/sql/sqlite_model.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <chrono>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

int main(int argc, char** argv) {
    std::string out;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;

    sql::SqliteModel m;
    UI_CHECK(m.open(":memory:"));
    UI_CHECK(m.exec("CREATE TABLE assets(id INTEGER PRIMARY KEY, name TEXT, kind TEXT, size INTEGER, note TEXT);"));
    const char* kinds[] = {"mesh", "texture", "sound", "material", "script"};
    UI_CHECK(m.exec("BEGIN;"));
    {
        // a recursive CTE inserts 200k rows in one statement
        std::string ins = "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000) "
                          "INSERT INTO assets(id, name, kind, size, note) SELECT i, printf('asset_%06d', i), "
                          "CASE i % 5 WHEN 0 THEN 'mesh' WHEN 1 THEN 'texture' WHEN 2 THEN 'sound' WHEN 3 THEN 'material' "
                          "ELSE 'script' END, (i * 7919) % 100000, CASE WHEN i % 10 = 0 THEN NULL ELSE 'ok' END FROM n;";
        UI_CHECK(m.exec(ins));
    }
    UI_CHECK(m.exec("COMMIT;"));
    (void)kinds;

    UI_CHECK(m.setQuery("SELECT id, name, kind, size, note FROM assets"));
    UI_CHECK_EQ(m.rows(), 200000u);
    UI_CHECK_EQ(m.columns(), 5u);
    UI_CHECK_EQ(std::string(m.header(1)), std::string("name"));
    UI_CHECK_EQ(m.model().columns, 5u);

    UiContext ui{{.viewport = {900, 520}, .font = &font()}};
    w::install(ui);
    ui.setCss(ui.root(), "padding", "10px");
    w::label(ui, ui.root(), "assets.db  -  SELECT id, name, kind, size, note FROM assets");
    const NodeHandle tv = w::tableView(ui, ui.root(), &m.model(), "assets");
    ui.setCss(tv, "flex-grow", "1");
    ui.update(1.f / 60.f);
    UI_CHECK_EQ(std::string(m.cell(0, 1)), std::string("asset_000001"));
    UI_CHECK(m.cell(9, 4).empty()); // NULL -> empty
    const u32 first = m.fetchCount();
    UI_CHECK(first >= 1u && first <= 2u);

    // scroll to the bottom with End: the view reads only the last window
    ui.setFocus(tv);
    ui.keyDown(Key::End); ui.keyUp(Key::End);
    ui.update(1.f / 60.f);
    UI_CHECK(w::tableFirstVisible(ui, tv) > 199900u);
    UI_CHECK(m.fetchCount() - first <= 2u);
    UI_CHECK_EQ(std::string(m.cell(199999, 1)), std::string("asset_200000"));
    // page back up a few times: stays within the cached window mostly
    const u32 before = m.fetchCount();
    for (int i = 0; i < 5; ++i) { ui.keyDown(Key::PageUp); ui.keyUp(Key::PageUp); ui.update(1.f / 60.f); }
    UI_CHECK(m.fetchCount() - before <= 2u);

    // header click sorts through the model's sortBy
    ui.connect(tv, SignalKind::Toggled, [&m](UiContext& u, NodeHandle t, const SignalArgs& a) {
        m.sortBy(u32(a.value), a.flag);
        w::tableModelChanged(u, t);
    });
    m.sortBy(3, false);
    w::tableModelChanged(ui, tv);
    ui.keyDown(Key::Home); ui.keyUp(Key::Home);
    ui.update(1.f / 60.f);
    UI_CHECK_EQ(std::string(m.cell(0, 3)), std::string("99999"));
    m.sortBy(~0u, true);
    UI_CHECK_EQ(std::string(m.cell(0, 0)), std::string("1"));

    // filtered query + writes + refresh
    UI_CHECK(m.setQuery("SELECT id, name, kind, size FROM assets WHERE kind = 'material' AND size < 5000"));
    const u32 mats = m.rows();
    UI_CHECK(mats > 0u && mats < 20000u);
    UI_CHECK(m.exec("INSERT INTO assets(id, name, kind, size) VALUES (300000, 'hero_material', 'material', 1);"));
    m.refresh();
    UI_CHECK_EQ(m.rows(), mats + 1u);
    w::tableModelChanged(ui, tv);
    m.sortBy(3, true);
    UI_CHECK_EQ(std::string(m.cell(0, 1)), std::string("hero_material"));

    // errors are reported, the model empties
    sql::SqliteModel bad;
    UI_CHECK(bad.open(":memory:"));
    UI_CHECK(!bad.setQuery("SELECT nope FROM missing"));
    UI_CHECK(!bad.lastError().empty());
    UI_CHECK_EQ(bad.rows(), 0u);

    // timing: a full frame over the 200k-row model (only visible rows are read)
    m.setQuery("SELECT id, name, kind, size, note FROM assets");
    w::tableModelChanged(ui, tv);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) { ui.keyDown(Key::PageDown); ui.keyUp(Key::PageDown); ui.update(1.f / 60.f); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 50.0;
    std::printf("sql: 200k rows, %.3f ms per paged frame, %u window queries total\n", ms, m.fetchCount());

    if (!out.empty()) {
        m.sortBy(3, false);
        w::tableModelChanged(ui, tv);
        ui.keyDown(Key::Home); ui.keyUp(Key::Home);
        ui.update();
        Image img;
        img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
        CpuRasterizer r;
        r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r.render(ui.drawList(), img);
        const std::string path = out + "/sql_table.png";
        stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("wrote %s\n", path.c_str());
    }
    return finish("fuse_ui_sql_model");
}
