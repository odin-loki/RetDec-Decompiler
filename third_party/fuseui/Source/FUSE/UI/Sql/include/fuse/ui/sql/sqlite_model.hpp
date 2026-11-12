// fuse_ui_sql: an widgets::ItemModel over a SQLite query (vendor/sqlite), for tableView / modelListView / proxies.
// Rows are fetched lazily in windows (LIMIT/OFFSET of the wrapped query) and cached, so a table view over a
// million-row table only ever reads the rows it shows. Single-threaded, like the UiContext that reads it.
#pragma once

#include "fuse/ui/widgets/widgets.hpp"

#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace fuse::ui::sql {

class SqliteModel {
public:
    SqliteModel();
    ~SqliteModel();
    SqliteModel(const SqliteModel&) = delete; ///< model() points at this object
    SqliteModel& operator=(const SqliteModel&) = delete;

    /// Open (or create) a database file; ":memory:" for an in-memory one. Closes any previous owned handle.
    bool open(std::string_view path);
    /// Use a handle owned by the caller (not closed by the model).
    void attach(sqlite3* db);
    sqlite3* handle() const { return m_db; }

    /// Run statements that return no rows (DDL, INSERT, transactions). Does not refresh the model.
    bool exec(std::string_view sql);
    /// Set the SELECT the model shows. Columns and headers come from the statement; the row count from
    /// SELECT count(*) over it. Returns false (and an empty model) on a SQL error.
    bool setQuery(std::string_view select);
    /// Order by a result column (0-based), applied around the query; ascending=false sorts descending.
    /// Pass col = ~0u to clear. Refreshes.
    void sortBy(u32 col, bool ascending);
    /// Re-count and drop the row cache (call after writes); then tableModelChanged() on the views.
    void refresh();

    u32 rows() const { return m_rows; }
    u32 columns() const { return u32(m_headers.size()); }
    std::string_view header(u32 col) const { return col < m_headers.size() ? std::string_view(m_headers[col]) : std::string_view{}; }
    std::string_view cell(u32 row, u32 col); ///< fetches the row's window on a cache miss
    const widgets::ItemModel& model() const { return m_model; }
    const std::string& lastError() const { return m_error; }

    /// Window size in rows (default 256). Smaller windows mean more queries while scrolling.
    void setWindow(u32 rowsPerFetch);
    u32 fetchCount() const { return m_fetches; } ///< window queries run so far (for tests / tuning)

private:
    bool fetch(u32 firstRow);
    std::string wrapped() const;
    sqlite3* m_db = nullptr;
    bool m_owned = false;
    std::string m_query, m_error;
    std::vector<std::string> m_headers;
    std::vector<std::string> m_cache; ///< m_cacheRows x columns
    u32 m_rows = 0, m_cacheFirst = 0, m_cacheRows = 0, m_window = 256, m_fetches = 0;
    u32 m_sortCol = ~0u;
    bool m_sortAsc = true;
    widgets::ItemModel m_model;
};

} // namespace fuse::ui::sql
