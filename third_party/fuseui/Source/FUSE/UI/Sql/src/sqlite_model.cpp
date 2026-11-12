#include "fuse/ui/sql/sqlite_model.hpp"

#include "sqlite3.h"

#include <algorithm>

namespace fuse::ui::sql {

namespace {
struct Stmt {
    sqlite3_stmt* s = nullptr;
    ~Stmt() { sqlite3_finalize(s); }
};
} // namespace

SqliteModel::SqliteModel() {
    m_model.user = this;
    m_model.columns = 0;
    m_model.rows = [](const void* u) { return static_cast<const SqliteModel*>(u)->rows(); };
    m_model.data = [](const void* u, u32 row, u32 col, std::string&) {
        return const_cast<SqliteModel*>(static_cast<const SqliteModel*>(u))->cell(row, col);
    };
    m_model.header = [](const void* u, u32 col) { return static_cast<const SqliteModel*>(u)->header(col); };
}

SqliteModel::~SqliteModel() {
    if (m_owned) sqlite3_close(m_db);
}

bool SqliteModel::open(std::string_view path) {
    if (m_owned) sqlite3_close(m_db);
    m_db = nullptr;
    m_owned = false;
    const std::string p(path);
    sqlite3* db = nullptr;
    if (sqlite3_open(p.c_str(), &db) != SQLITE_OK) {
        m_error = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        sqlite3_close(db);
        return false;
    }
    m_db = db;
    m_owned = true;
    return true;
}

void SqliteModel::attach(sqlite3* db) {
    if (m_owned) sqlite3_close(m_db);
    m_db = db;
    m_owned = false;
}

bool SqliteModel::exec(std::string_view sql) {
    if (!m_db) return false;
    const std::string s(sql);
    char* err = nullptr;
    if (sqlite3_exec(m_db, s.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        m_error = err ? err : "error";
        sqlite3_free(err);
        return false;
    }
    return true;
}

std::string SqliteModel::wrapped() const {
    std::string q = "SELECT * FROM (" + m_query + ")";
    if (m_sortCol != ~0u) q += " ORDER BY " + std::to_string(m_sortCol + 1) + (m_sortAsc ? " ASC" : " DESC");
    return q;
}

bool SqliteModel::setQuery(std::string_view select) {
    m_query.assign(select);
    while (!m_query.empty() && (m_query.back() == ';' || m_query.back() == ' ' || m_query.back() == '\n')) m_query.pop_back();
    m_headers.clear();
    m_sortCol = ~0u;
    m_rows = 0;
    m_cacheRows = 0;
    m_model.columns = 0;
    if (!m_db) { m_error = "no database"; return false; }
    Stmt st;
    if (sqlite3_prepare_v2(m_db, m_query.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
        m_error = sqlite3_errmsg(m_db);
        m_query.clear();
        return false;
    }
    const int n = sqlite3_column_count(st.s);
    for (int i = 0; i < n; ++i) m_headers.emplace_back(sqlite3_column_name(st.s, i));
    m_model.columns = u32(n);
    refresh();
    return true;
}

void SqliteModel::sortBy(u32 col, bool ascending) {
    m_sortCol = col < columns() ? col : ~0u;
    m_sortAsc = ascending;
    refresh();
}

void SqliteModel::refresh() {
    m_cacheRows = 0;
    m_rows = 0;
    if (!m_db || m_query.empty()) return;
    Stmt st;
    const std::string q = "SELECT count(*) FROM (" + m_query + ")";
    if (sqlite3_prepare_v2(m_db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) { m_error = sqlite3_errmsg(m_db); return; }
    if (sqlite3_step(st.s) == SQLITE_ROW) m_rows = u32(std::max<sqlite3_int64>(0, sqlite3_column_int64(st.s, 0)));
}

void SqliteModel::setWindow(u32 n) {
    m_window = std::max(1u, n);
    m_cacheRows = 0;
}

bool SqliteModel::fetch(u32 firstRow) {
    ++m_fetches;
    m_cacheFirst = firstRow;
    m_cacheRows = 0;
    const u32 cols = columns();
    m_cache.resize(std::size_t(m_window) * cols);
    Stmt st;
    const std::string q = wrapped() + " LIMIT ?1 OFFSET ?2";
    if (sqlite3_prepare_v2(m_db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) { m_error = sqlite3_errmsg(m_db); return false; }
    sqlite3_bind_int64(st.s, 1, m_window);
    sqlite3_bind_int64(st.s, 2, firstRow);
    while (m_cacheRows < m_window && sqlite3_step(st.s) == SQLITE_ROW) {
        for (u32 c = 0; c < cols; ++c) {
            const unsigned char* t = sqlite3_column_text(st.s, int(c));
            std::string& cell = m_cache[std::size_t(m_cacheRows) * cols + c];
            if (t) cell.assign(reinterpret_cast<const char*>(t), std::size_t(sqlite3_column_bytes(st.s, int(c))));
            else cell.clear(); // NULL
        }
        ++m_cacheRows;
    }
    return true;
}

std::string_view SqliteModel::cell(u32 row, u32 col) {
    if (row >= m_rows || col >= columns()) return {};
    if (row < m_cacheFirst || row >= m_cacheFirst + m_cacheRows) {
        // centre the window a little behind the request so scrolling up does not refetch immediately
        const u32 back = m_window / 4;
        if (!fetch(row > back ? row - back : 0)) return {};
        if (row >= m_cacheFirst + m_cacheRows) return {};
    }
    return m_cache[std::size_t(row - m_cacheFirst) * columns() + col];
}

} // namespace fuse::ui::sql
