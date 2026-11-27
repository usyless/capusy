#pragma once

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace capusy {

enum class CaptureKind : int32_t {
    Image = 0,
    Video = 1,
};

enum class CaptureMode : int32_t {
    Full = 0,
    Region = 1,
    Recording = 2,
};

struct CaptureEntry {
    int64_t id = 0;
    std::string path;         // UTF-8 full file path
    CaptureKind kind = CaptureKind::Image;
    CaptureMode mode = CaptureMode::Full;
    int64_t created_at = 0;   // unix seconds
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t size_bytes = 0;
    uint64_t duration_ms = 0;
    std::string window_title; // UTF-8 captured/foreground window title
    std::string process_name; // UTF-8 executable name (e.g. "Code.exe")
    std::string format;       // UTF-8 uppercase format tag (e.g. "PNG", "JPEG", "MP4")

    bool operator==(const CaptureEntry&) const = default;
};

/// Case-insensitive multi-token AND search across screenshot name/path, window title,
/// process name, format, capture mode, and formatted timestamps (display + ISO).
[[nodiscard]] inline bool matches_history_query_lower(std::wstring_view haystack_lower, std::wstring_view query_lower) noexcept {
    size_t pos = 0;
    while (pos < query_lower.size()) {
        while (pos < query_lower.size() && (query_lower[pos] == L' ' || query_lower[pos] == L'\t')) ++pos;
        if (pos >= query_lower.size()) break;
        const size_t start = pos;
        while (pos < query_lower.size() && query_lower[pos] != L' ' && query_lower[pos] != L'\t') ++pos;
        const std::wstring_view token = query_lower.substr(start, pos - start);
        if (!token.empty() && haystack_lower.find(token) == std::wstring_view::npos) {
            return false;
        }
    }
    return true;
}

struct HistoryStats {
    uint64_t total_count = 0;
    uint64_t image_count = 0;
    uint64_t video_count = 0;
    uint64_t db_bytes = 0;
};

/// Multi-select state for the history list (Ctrl toggle, Shift range, Ctrl+Shift additive range,
/// keyboard navigation, deferred single-select on multi-drag, and reconciliation across reloads).
struct HistorySelection {
    std::unordered_set<int64_t> ids;
    int focus = -1;
    int anchor = -1;
    int64_t anchor_id = -1;

    [[nodiscard]] bool empty() const noexcept { return ids.empty(); }
    [[nodiscard]] size_t count() const noexcept { return ids.size(); }
    [[nodiscard]] bool contains(int64_t id) const noexcept { return ids.contains(id); }

    void clear() noexcept {
        ids.clear();
        focus = -1;
        anchor = -1;
        anchor_id = -1;
    }

    void select_single(std::span<const CaptureEntry> entries, int idx) {
        if (idx < 0 || std::cmp_greater_equal(idx, entries.size())) {
            clear();
            return;
        }
        const int64_t id = entries[static_cast<size_t>(idx)].id;
        ids.clear();
        ids.insert(id);
        focus = idx;
        anchor = idx;
        anchor_id = id;
    }

    /// Handles a left-click on `idx`.
    /// Returns true if the click landed on an already-selected row in a multi-selection without
    /// modifiers (caller should defer collapsing to `idx` until mouse-up if no drag started).
    bool click(std::span<const CaptureEntry> entries, int idx, bool ctrl, bool shift) {
        if (idx < 0 || std::cmp_greater_equal(idx, entries.size())) return false;
        const int64_t cid = entries[static_cast<size_t>(idx)].id;

        if (shift) {
            const int base = (anchor >= 0 && std::cmp_less(anchor, entries.size()))
                ? anchor
                : ((focus >= 0 && std::cmp_less(focus, entries.size())) ? focus : idx);
            if (!ctrl) ids.clear();
            const int lo = std::min(base, idx);
            const int hi = std::max(base, idx);
            for (int k = lo; k <= hi; ++k) {
                ids.insert(entries[static_cast<size_t>(k)].id);
            }
            anchor = base;
            anchor_id = entries[static_cast<size_t>(base)].id;
            focus = idx;
            return false;
        }

        if (ctrl) {
            if (ids.erase(cid) > 0) {
                anchor = idx;
                anchor_id = cid;
                if (focus == idx) {
                    focus = find_closest_selected(entries, idx);
                }
            } else {
                ids.insert(cid);
                focus = idx;
                anchor = idx;
                anchor_id = cid;
            }
            return false;
        }

        if (ids.contains(cid) && ids.size() > 1) {
            return true;
        }

        select_single(entries, idx);
        return false;
    }

    /// Handles Up/Down keyboard navigation (`delta` = -1 or +1). Returns true if selection changed.
    bool move(std::span<const CaptureEntry> entries, int delta, bool shift) {
        if (entries.empty()) return false;
        const int last = static_cast<int>(entries.size()) - 1;
        int next = focus;
        if (next < 0 || next > last) {
            next = delta >= 0 ? 0 : last;
        } else {
            next = std::clamp(next + delta, 0, last);
        }

        if (shift) {
            const int base = (anchor >= 0 && anchor <= last)
                ? anchor
                : ((focus >= 0 && focus <= last) ? focus : next);
            const int lo = std::min(base, next);
            const int hi = std::max(base, next);
            ids.clear();
            for (int k = lo; k <= hi; ++k) {
                ids.insert(entries[static_cast<size_t>(k)].id);
            }
            anchor = base;
            anchor_id = entries[static_cast<size_t>(base)].id;
            focus = next;
            return true;
        }

        const bool changed = (focus != next || ids.size() != 1 || !ids.contains(entries[static_cast<size_t>(next)].id));
        select_single(entries, next);
        return changed;
    }

    void select_all(std::span<const CaptureEntry> entries) {
        ids.clear();
        ids.reserve(entries.size());
        for (const auto& e : entries) ids.insert(e.id);
        if (entries.empty()) {
            focus = -1;
            anchor = -1;
            anchor_id = -1;
        } else {
            if (focus < 0 || std::cmp_greater_equal(focus, entries.size())) focus = 0;
            if (anchor < 0 || std::cmp_greater_equal(anchor, entries.size())) {
                anchor = focus;
                anchor_id = entries[static_cast<size_t>(anchor)].id;
            }
        }
    }

    /// Reconciles selection against a newly filtered/reloaded `entries` list.
    void reconcile(std::span<const CaptureEntry> entries, bool keep_selection, int64_t preferred_focus_id = -1) {
        if (entries.empty()) {
            clear();
            return;
        }
        if (!keep_selection) {
            ids.clear();
            anchor_id = -1;
        } else if (!ids.empty()) {
            std::unordered_set<int64_t> visible;
            visible.reserve(entries.size());
            for (const auto& e : entries) visible.insert(e.id);
            std::erase_if(ids, [&](int64_t id) { return !visible.contains(id); });
        }

        focus = -1;
        if (preferred_focus_id >= 0) {
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].id == preferred_focus_id) {
                    focus = static_cast<int>(i);
                    break;
                }
            }
        }
        if (focus < 0 && !ids.empty()) {
            for (size_t i = 0; i < entries.size(); ++i) {
                if (ids.contains(entries[i].id)) {
                    focus = static_cast<int>(i);
                    break;
                }
            }
        }
        if (focus < 0) {
            focus = 0;
        }
        if (ids.empty()) {
            ids.insert(entries[static_cast<size_t>(focus)].id);
        }

        anchor = -1;
        if (anchor_id >= 0) {
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].id == anchor_id) {
                    anchor = static_cast<int>(i);
                    break;
                }
            }
        }
        if (anchor < 0) {
            anchor = focus;
            anchor_id = entries[static_cast<size_t>(anchor)].id;
        }
    }

    [[nodiscard]] std::vector<int> selected_indices(std::span<const CaptureEntry> entries) const {
        std::vector<int> out;
        out.reserve(ids.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            if (ids.contains(entries[i].id)) out.push_back(static_cast<int>(i));
        }
        return out;
    }

private:
    [[nodiscard]] int find_closest_selected(std::span<const CaptureEntry> entries, int from_idx) const noexcept {
        if (ids.empty() || entries.empty()) return -1;
        int best = -1;
        int best_dist = std::numeric_limits<int>::max();
        for (size_t i = 0; i < entries.size(); ++i) {
            if (ids.contains(entries[i].id)) {
                const int d = std::abs(static_cast<int>(i) - from_idx);
                if (d < best_dist) {
                    best_dist = d;
                    best = static_cast<int>(i);
                }
            }
        }
        return best;
    }
};

/// Thread-safe SQLite (WAL) store for screenshot & recording history (modeled on usyseek::TagStore).
class HistoryStore {
public:
    HistoryStore() = default;
    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;
    HistoryStore(HistoryStore&&) = delete;
    HistoryStore& operator=(HistoryStore&&) = delete;
    ~HistoryStore() { close(); }

    bool open(const std::filesystem::path& db_path, std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        close_locked();

        std::error_code ec;
        if (db_path.has_parent_path()) {
            std::filesystem::create_directories(db_path.parent_path(), ec);
        }

        const auto u8 = db_path.u8string();
        const std::string utf8{reinterpret_cast<const char*>(u8.data()), u8.size()};
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(utf8.c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            if (error) *error = raw ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
            if (raw) sqlite3_close(raw);
            return false;
        }
        db_ = raw;
        path_ = db_path;
        sqlite3_busy_timeout(db_, 5000);

        static constexpr const char* INIT_SQL = R"sql(
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            CREATE TABLE IF NOT EXISTS captures (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                path         TEXT NOT NULL UNIQUE,
                kind         INTEGER NOT NULL DEFAULT 0,
                mode         INTEGER NOT NULL DEFAULT 0,
                created_at   INTEGER NOT NULL,
                width        INTEGER NOT NULL DEFAULT 0,
                height       INTEGER NOT NULL DEFAULT 0,
                size_bytes   INTEGER NOT NULL DEFAULT 0,
                duration_ms  INTEGER NOT NULL DEFAULT 0,
                window_title TEXT NOT NULL DEFAULT '',
                process_name TEXT NOT NULL DEFAULT '',
                format       TEXT NOT NULL DEFAULT ''
            );
            CREATE INDEX IF NOT EXISTS idx_captures_created_at ON captures(created_at DESC, id DESC);
        )sql";
        if (!exec_locked(INIT_SQL, error)) {
            close_locked();
            return false;
        }
        if (!migrate_schema_locked(error)) {
            close_locked();
            return false;
        }
        return true;
    }

    void close() noexcept {
        const std::scoped_lock lk{mx_};
        close_locked();
    }

    [[nodiscard]] bool opened() const noexcept {
        const std::scoped_lock lk{mx_};
        return db_ != nullptr;
    }

    /// Inserts (or updates on duplicate path) a capture entry and populates `entry.id`.
    bool insert(CaptureEntry& entry, std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;

        Stmt st;
        if (!prepare_locked(
                "INSERT INTO captures(path, kind, mode, created_at, width, height, size_bytes, duration_ms, window_title, process_name, format) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) "
                "ON CONFLICT(path) DO UPDATE SET "
                "kind=excluded.kind, mode=excluded.mode, created_at=excluded.created_at, "
                "width=excluded.width, height=excluded.height, size_bytes=excluded.size_bytes, "
                "duration_ms=excluded.duration_ms, window_title=excluded.window_title, "
                "process_name=excluded.process_name, format=excluded.format;",
                st, error)) {
            return false;
        }
        sqlite3_bind_text(st.s, 1, entry.path.data(), static_cast<int>(entry.path.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(st.s, 2, static_cast<int>(entry.kind));
        sqlite3_bind_int(st.s, 3, static_cast<int>(entry.mode));
        sqlite3_bind_int64(st.s, 4, entry.created_at);
        sqlite3_bind_int64(st.s, 5, static_cast<int64_t>(entry.width));
        sqlite3_bind_int64(st.s, 6, static_cast<int64_t>(entry.height));
        sqlite3_bind_int64(st.s, 7, static_cast<int64_t>(entry.size_bytes));
        sqlite3_bind_int64(st.s, 8, static_cast<int64_t>(entry.duration_ms));
        sqlite3_bind_text(st.s, 9, entry.window_title.data(), static_cast<int>(entry.window_title.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.s, 10, entry.process_name.data(), static_cast<int>(entry.process_name.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.s, 11, entry.format.data(), static_cast<int>(entry.format.size()), SQLITE_TRANSIENT);
        if (!step_done_locked(st.s, error)) return false;

        entry.id = sqlite3_last_insert_rowid(db_);
        if (entry.id == 0) {
            Stmt q;
            if (prepare_locked("SELECT id FROM captures WHERE path = ?1;", q, error)) {
                sqlite3_bind_text(q.s, 1, entry.path.data(), static_cast<int>(entry.path.size()), SQLITE_TRANSIENT);
                if (sqlite3_step(q.s) == SQLITE_ROW) {
                    entry.id = sqlite3_column_int64(q.s, 0);
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<std::vector<CaptureEntry>> list_recent(int limit = 1000, std::optional<CaptureKind> filter_kind = std::nullopt,
                                                                       std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;

        const char* sql = filter_kind
            ? "SELECT id, path, kind, mode, created_at, width, height, size_bytes, duration_ms, window_title, process_name, format "
              "FROM captures WHERE kind = ?1 ORDER BY created_at DESC, id DESC LIMIT ?2;"
            : "SELECT id, path, kind, mode, created_at, width, height, size_bytes, duration_ms, window_title, process_name, format "
              "FROM captures ORDER BY created_at DESC, id DESC LIMIT ?1;";

        Stmt st;
        if (!prepare_locked(sql, st, error)) return std::nullopt;
        if (filter_kind) {
            sqlite3_bind_int(st.s, 1, static_cast<int>(*filter_kind));
            sqlite3_bind_int(st.s, 2, limit);
        } else {
            sqlite3_bind_int(st.s, 1, limit);
        }

        const auto read_str = [](sqlite3_stmt* stmt, int col, std::string& dst) {
            const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            const int len = sqlite3_column_bytes(stmt, col);
            if (txt && len > 0) dst.assign(txt, static_cast<size_t>(len));
        };

        std::vector<CaptureEntry> out;
        for (;;) {
            const int rc = sqlite3_step(st.s);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            CaptureEntry e;
            e.id = sqlite3_column_int64(st.s, 0);
            read_str(st.s, 1, e.path);
            e.kind = static_cast<CaptureKind>(sqlite3_column_int(st.s, 2));
            e.mode = static_cast<CaptureMode>(sqlite3_column_int(st.s, 3));
            e.created_at = sqlite3_column_int64(st.s, 4);
            e.width = static_cast<uint32_t>(sqlite3_column_int64(st.s, 5));
            e.height = static_cast<uint32_t>(sqlite3_column_int64(st.s, 6));
            e.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(st.s, 7));
            e.duration_ms = static_cast<uint64_t>(sqlite3_column_int64(st.s, 8));
            read_str(st.s, 9, e.window_title);
            read_str(st.s, 10, e.process_name);
            read_str(st.s, 11, e.format);
            out.push_back(std::move(e));
        }
        return out;
    }

    bool remove(int64_t id, std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;
        Stmt st;
        if (!prepare_locked("DELETE FROM captures WHERE id = ?1;", st, error)) return false;
        sqlite3_bind_int64(st.s, 1, id);
        return step_done_locked(st.s, error);
    }

    bool remove_many(std::span<const int64_t> ids, std::string* error = nullptr) {
        if (ids.empty()) return true;
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;
        if (ids.size() == 1) {
            Stmt st;
            if (!prepare_locked("DELETE FROM captures WHERE id = ?1;", st, error)) return false;
            sqlite3_bind_int64(st.s, 1, ids[0]);
            return step_done_locked(st.s, error);
        }
        if (!exec_locked("BEGIN IMMEDIATE;", error)) return false;
        Stmt st;
        if (!prepare_locked("DELETE FROM captures WHERE id = ?1;", st, error)) {
            (void)exec_locked("ROLLBACK;", nullptr);
            return false;
        }
        for (const int64_t id : ids) {
            sqlite3_reset(st.s);
            sqlite3_clear_bindings(st.s);
            sqlite3_bind_int64(st.s, 1, id);
            if (!step_done_locked(st.s, error)) {
                (void)exec_locked("ROLLBACK;", nullptr);
                return false;
            }
        }
        return exec_locked("COMMIT;", error);
    }

    [[nodiscard]] std::optional<HistoryStats> stats(std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;
        HistoryStats out;
        const auto query_count = [&](const char* sql, uint64_t& dst) -> bool {
            Stmt st;
            if (!prepare_locked(sql, st, error)) return false;
            if (sqlite3_step(st.s) != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return false;
            }
            dst = static_cast<uint64_t>(sqlite3_column_int64(st.s, 0));
            return true;
        };
        if (!query_count("SELECT COUNT(*) FROM captures;", out.total_count)) return std::nullopt;
        if (!query_count("SELECT COUNT(*) FROM captures WHERE kind = 0;", out.image_count)) return std::nullopt;
        if (!query_count("SELECT COUNT(*) FROM captures WHERE kind = 1;", out.video_count)) return std::nullopt;

        std::error_code ec;
        const auto add_size = [&](const std::filesystem::path& p) {
            if (std::filesystem::is_regular_file(p, ec)) {
                const uint64_t sz = std::filesystem::file_size(p, ec);
                if (!ec) out.db_bytes += sz;
            }
        };
        add_size(path_);
        std::filesystem::path wal_path = path_;
        wal_path += L"-wal";
        add_size(wal_path);
        return out;
    }

private:
    struct Stmt {
        sqlite3_stmt* s = nullptr;
        Stmt() = default;
        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;
        Stmt(Stmt&&) = delete;
        Stmt& operator=(Stmt&&) = delete;
        ~Stmt() {
            if (s) sqlite3_finalize(s);
        }
    };

    bool migrate_schema_locked(std::string* error) {
        bool has_window_title = false;
        bool has_process_name = false;
        bool has_format = false;
        Stmt st;
        if (!prepare_locked("PRAGMA table_info(captures);", st, error)) return false;
        while (sqlite3_step(st.s) == SQLITE_ROW) {
            const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 1));
            if (!name) continue;
            const std::string_view col{name};
            if (col == "window_title") has_window_title = true;
            else if (col == "process_name") has_process_name = true;
            else if (col == "format") has_format = true;
        }
        if (!has_window_title && !exec_locked("ALTER TABLE captures ADD COLUMN window_title TEXT NOT NULL DEFAULT '';", error)) return false;
        if (!has_process_name && !exec_locked("ALTER TABLE captures ADD COLUMN process_name TEXT NOT NULL DEFAULT '';", error)) return false;
        if (!has_format && !exec_locked("ALTER TABLE captures ADD COLUMN format TEXT NOT NULL DEFAULT '';", error)) return false;
        return true;
    }

    void close_locked() noexcept {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        path_.clear();
    }

    bool require_open_locked(std::string* error) const {
        if (db_) return true;
        if (error) *error = "history database is closed";
        return false;
    }

    bool exec_locked(const char* sql, std::string* error) {
        char* msg = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &msg);
        if (rc == SQLITE_OK) return true;
        if (error) *error = msg ? msg : sqlite3_errmsg(db_);
        if (msg) sqlite3_free(msg);
        return false;
    }

    bool prepare_locked(const char* sql, Stmt& out, std::string* error) {
        if (sqlite3_prepare_v2(db_, sql, -1, &out.s, nullptr) == SQLITE_OK) return true;
        if (error) *error = sqlite3_errmsg(db_);
        return false;
    }

    bool step_done_locked(sqlite3_stmt* s, std::string* error) {
        const int rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) return true;
        if (error) *error = sqlite3_errmsg(db_);
        return false;
    }

    mutable std::mutex mx_;
    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
};

} // namespace capusy
