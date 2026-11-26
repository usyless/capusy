#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
    std::string path; // UTF-8 full file path
    CaptureKind kind = CaptureKind::Image;
    CaptureMode mode = CaptureMode::Full;
    int64_t created_at = 0; // unix seconds
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t size_bytes = 0;
    uint64_t duration_ms = 0;

    bool operator==(const CaptureEntry&) const = default;
};

struct HistoryStats {
    uint64_t total_count = 0;
    uint64_t image_count = 0;
    uint64_t video_count = 0;
    uint64_t db_bytes = 0;
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
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                path        TEXT NOT NULL UNIQUE,
                kind        INTEGER NOT NULL DEFAULT 0,
                mode        INTEGER NOT NULL DEFAULT 0,
                created_at  INTEGER NOT NULL,
                width       INTEGER NOT NULL DEFAULT 0,
                height      INTEGER NOT NULL DEFAULT 0,
                size_bytes  INTEGER NOT NULL DEFAULT 0,
                duration_ms INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS idx_captures_created_at ON captures(created_at DESC, id DESC);
        )sql";
        if (!exec_locked(INIT_SQL, error)) {
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
                "INSERT INTO captures(path, kind, mode, created_at, width, height, size_bytes, duration_ms) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                "ON CONFLICT(path) DO UPDATE SET "
                "kind=excluded.kind, mode=excluded.mode, created_at=excluded.created_at, "
                "width=excluded.width, height=excluded.height, size_bytes=excluded.size_bytes, "
                "duration_ms=excluded.duration_ms;",
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
            ? "SELECT id, path, kind, mode, created_at, width, height, size_bytes, duration_ms "
              "FROM captures WHERE kind = ?1 ORDER BY created_at DESC, id DESC LIMIT ?2;"
            : "SELECT id, path, kind, mode, created_at, width, height, size_bytes, duration_ms "
              "FROM captures ORDER BY created_at DESC, id DESC LIMIT ?1;";

        Stmt st;
        if (!prepare_locked(sql, st, error)) return std::nullopt;
        if (filter_kind) {
            sqlite3_bind_int(st.s, 1, static_cast<int>(*filter_kind));
            sqlite3_bind_int(st.s, 2, limit);
        } else {
            sqlite3_bind_int(st.s, 1, limit);
        }

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
            const auto* p_txt = reinterpret_cast<const char*>(sqlite3_column_text(st.s, 1));
            const int p_len = sqlite3_column_bytes(st.s, 1);
            if (p_txt && p_len > 0) e.path.assign(p_txt, static_cast<size_t>(p_len));
            e.kind = static_cast<CaptureKind>(sqlite3_column_int(st.s, 2));
            e.mode = static_cast<CaptureMode>(sqlite3_column_int(st.s, 3));
            e.created_at = sqlite3_column_int64(st.s, 4);
            e.width = static_cast<uint32_t>(sqlite3_column_int64(st.s, 5));
            e.height = static_cast<uint32_t>(sqlite3_column_int64(st.s, 6));
            e.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(st.s, 7));
            e.duration_ms = static_cast<uint64_t>(sqlite3_column_int64(st.s, 8));
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
