#pragma once

#include <sqlite3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace capusy {

enum class CaptureKind : uint8_t {
    Image = 0,
    Video = 1,
};

enum class CaptureMode : uint8_t {
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
        if (!token.empty() && !haystack_lower.contains(token)) {
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

struct BatchInsertResult {
    uint64_t inserted = 0;
    uint64_t inserted_images = 0;
    uint64_t inserted_videos = 0;
    uint64_t duplicates = 0;
};

struct SelectionSummary {
    uint64_t count = 0;
    uint64_t image_count = 0;
    uint64_t video_count = 0;
    uint64_t total_bytes = 0;
};

namespace detail {

[[nodiscard]] constexpr char hs_ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool hs_is_ascii(std::string_view s) noexcept {
    return std::ranges::all_of(s, [](char c) noexcept {
        return static_cast<unsigned char>(c) < 0x80u;
    });
}

/// Case-insensitive substring check for an already-lowercased ASCII needle against arbitrary UTF-8 haystack.
[[nodiscard]] inline bool hs_contains_ascii_ci(std::string_view hay, std::string_view needle_lower) noexcept {
    if (needle_lower.empty()) return true;
    if (needle_lower.size() > hay.size()) return false;
    const size_t last = hay.size() - needle_lower.size();
    const char first = needle_lower[0];
    for (size_t i = 0; i <= last; ++i) {
        if (hs_ascii_lower(hay[i]) != first) continue;
        size_t j = 1;
        while (j < needle_lower.size() && hs_ascii_lower(hay[i + j]) == needle_lower[j]) ++j;
        if (j == needle_lower.size()) return true;
    }
    return false;
}

/// Formats `unix_s` as local `"YYYY-MM-DD HH:MM:SS d mmm yyyy"` into a caller-provided stack buffer (lowercase, zero allocs).
[[nodiscard]] inline std::string_view hs_format_local_search_time(int64_t unix_s, std::span<char> buf) noexcept {
    if (unix_s <= 0 || buf.size() < 40) return {};
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
#ifdef _WIN32
    const uint64_t ft = (static_cast<uint64_t>(unix_s) + 11'644'473'600ull) * 10'000'000ull;
    const FILETIME f{.dwLowDateTime = static_cast<DWORD>(ft), .dwHighDateTime = static_cast<DWORD>(ft >> 32u)};
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&f, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    year = st.wYear;
    month = st.wMonth;
    day = st.wDay;
    hour = st.wHour;
    minute = st.wMinute;
    second = st.wSecond;
#else
    const int64_t z = (unix_s / 86400) + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - (era * 146097));
    const unsigned yoe = (doe - (doe / 1460u) + (doe / 36524u) - (doe / 146096u)) / 365u;
    const int64_t y = static_cast<int64_t>(yoe) + (era * 400);
    const unsigned doy = doe - ((365u * yoe) + (yoe / 4u) - (yoe / 100u));
    const unsigned mp = ((5u * doy) + 2u) / 153u;
    day = doy - (((153u * mp) + 2u) / 5u) + 1u;
    month = mp < 10u ? (mp + 3u) : (mp - 9u);
    year = static_cast<unsigned>(y + (month <= 2u ? 1 : 0));
    const int64_t rem = ((unix_s % 86400) + 86400) % 86400;
    hour = static_cast<unsigned>(rem / 3600);
    minute = static_cast<unsigned>((rem % 3600) / 60);
    second = static_cast<unsigned>(rem % 60);
#endif
    static constexpr std::array<const char*, 12> kMonths = {
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec",
    };
    size_t p = 0;
    const auto push2 = [&](unsigned v) noexcept {
        buf[p++] = static_cast<char>('0' + ((v / 10u) % 10u));
        buf[p++] = static_cast<char>('0' + (v % 10u));
    };
    const auto push4 = [&](unsigned v) noexcept {
        buf[p++] = static_cast<char>('0' + ((v / 1000u) % 10u));
        buf[p++] = static_cast<char>('0' + ((v / 100u) % 10u));
        buf[p++] = static_cast<char>('0' + ((v / 10u) % 10u));
        buf[p++] = static_cast<char>('0' + (v % 10u));
    };
    // ISO: YYYY-MM-DD HH:MM:SS
    push4(year);
    buf[p++] = '-';
    push2(month);
    buf[p++] = '-';
    push2(day);
    buf[p++] = ' ';
    push2(hour);
    buf[p++] = ':';
    push2(minute);
    buf[p++] = ':';
    push2(second);
    // Display: " d mmm yyyy"
    buf[p++] = ' ';
    if (day >= 10u) buf[p++] = static_cast<char>('0' + ((day / 10u) % 10u));
    buf[p++] = static_cast<char>('0' + (day % 10u));
    buf[p++] = ' ';
    if (month >= 1u && month <= 12u) {
        const char* m = kMonths[month - 1u];
        buf[p++] = m[0];
        buf[p++] = m[1];
        buf[p++] = m[2];
    }
    buf[p++] = ' ';
    push4(year);
    return {buf.data(), p};
}

/// UTF-8 case-insensitive substring check (zero-alloc ASCII fast path; Win32 Unicode fallback for non-ASCII needles).
[[nodiscard]] inline bool hs_field_contains(std::string_view hay, std::string_view token_lower, bool token_ascii) {
    if (token_lower.empty()) return true;
    if (hay.empty()) return false;
    if (token_ascii) return hs_contains_ascii_ci(hay, token_lower);
#ifdef _WIN32
    const auto to_lower_w = [](std::string_view u8, std::wstring& out) -> bool {
        const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
        if (n <= 0) return false;
        out.resize(static_cast<size_t>(n));
        if (MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), out.data(), n) <= 0) return false;
        CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
        return true;
    };
    thread_local std::wstring whay;
    thread_local std::wstring wtok;
    if (!to_lower_w(hay, whay) || !to_lower_w(token_lower, wtok)) return false;
    return whay.contains(wtok);
#else
    return hay.contains(token_lower);
#endif
}

/// Deterministic SQLite scalar function:
/// capusy_match_query(path, window_title, process_name, format, mode, created_at, query_lower_utf8) -> 0|1
inline void sqlite_match_query(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc < 7) {
        sqlite3_result_int(ctx, 1);
        return;
    }
    const auto arg_sv = [&](int idx) noexcept -> std::string_view {
        const auto* p = reinterpret_cast<const char*>(sqlite3_value_text(argv[idx]));
        const int n = sqlite3_value_bytes(argv[idx]);
        return (p && n > 0) ? std::string_view{p, static_cast<size_t>(n)} : std::string_view{};
    };

    const std::string_view query = arg_sv(6);
    if (query.empty()) {
        sqlite3_result_int(ctx, 1);
        return;
    }

    const std::string_view path = arg_sv(0);
    const std::string_view title = arg_sv(1);
    const std::string_view proc = arg_sv(2);
    const std::string_view fmt = arg_sv(3);
    const int mode = sqlite3_value_int(argv[4]);
    const int64_t created_at = sqlite3_value_int64(argv[5]);

    const std::string_view mode_str = (mode == static_cast<int>(CaptureMode::Full))
        ? std::string_view{"full screen"}
        : (mode == static_cast<int>(CaptureMode::Region) ? std::string_view{"region"} : std::string_view{"recording video"});

    std::array<char, 48> time_buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
    std::string_view time_str;
    bool time_ready = false;

    size_t pos = 0;
    while (pos < query.size()) {
        while (pos < query.size() && (query[pos] == ' ' || query[pos] == '\t')) ++pos;
        if (pos >= query.size()) break;
        const size_t start = pos;
        while (pos < query.size() && query[pos] != ' ' && query[pos] != '\t') ++pos;
        const std::string_view tok = query.substr(start, pos - start);
        if (tok.empty()) continue;

        const bool ascii = hs_is_ascii(tok);
        bool matched = hs_field_contains(path, tok, ascii) ||
                       hs_field_contains(title, tok, ascii) ||
                       hs_field_contains(proc, tok, ascii) ||
                       hs_field_contains(fmt, tok, ascii) ||
                       (ascii && hs_contains_ascii_ci(mode_str, tok));
        if (!matched && ascii) {
            if (!time_ready) {
                time_str = hs_format_local_search_time(created_at, time_buf);
                time_ready = true;
            }
            matched = hs_contains_ascii_ci(time_str, tok);
        }
        if (!matched) {
            sqlite3_result_int(ctx, 0);
            return;
        }
    }
    sqlite3_result_int(ctx, 1);
}

} // namespace detail

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
        focus = std::max(focus, 0);
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
            CREATE INDEX IF NOT EXISTS idx_captures_kind_created_at ON captures(kind, created_at DESC, id DESC);
        )sql";
        if (!exec_locked(INIT_SQL, error)) {
            close_locked();
            return false;
        }
        if (!migrate_schema_locked(error)) {
            close_locked();
            return false;
        }
        if (sqlite3_create_function_v2(db_, "capusy_match_query", 7, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                       nullptr, &detail::sqlite_match_query, nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error) *error = sqlite3_errmsg(db_);
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

    /// Inserts a batch of capture entries inside a single transaction, ignoring any paths that already exist.
    /// Existing entries are preserved untouched.
    bool insert_many_ignore(std::span<const CaptureEntry> entries, BatchInsertResult* result = nullptr, std::string* error = nullptr) {
        BatchInsertResult res{};
        if (entries.empty()) {
            if (result) *result = res;
            return true;
        }
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return false;

        if (!exec_locked("BEGIN IMMEDIATE;", error)) return false;
        Stmt st;
        if (!prepare_locked(
                "INSERT OR IGNORE INTO captures(path, kind, mode, created_at, width, height, size_bytes, duration_ms, window_title, process_name, format) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);",
                st, error)) {
            (void)exec_locked("ROLLBACK;", nullptr);
            return false;
        }

        for (const CaptureEntry& entry : entries) {
            sqlite3_reset(st.s);
            sqlite3_clear_bindings(st.s);
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
            if (!step_done_locked(st.s, error)) {
                (void)exec_locked("ROLLBACK;", nullptr);
                return false;
            }
            if (sqlite3_changes(db_) > 0) {
                ++res.inserted;
                if (entry.kind == CaptureKind::Video) {
                    ++res.inserted_videos;
                } else {
                    ++res.inserted_images;
                }
            } else {
                ++res.duplicates;
            }
        }

        if (!exec_locked("COMMIT;", error)) {
            (void)exec_locked("ROLLBACK;", nullptr);
            return false;
        }
        if (result) *result = res;
        return true;
    }

    [[nodiscard]] std::optional<std::vector<CaptureEntry>> list_recent(int limit = 1000, std::optional<CaptureKind> filter_kind = std::nullopt,
                                                                       std::string* error = nullptr) {
        return list_page(limit, 0, filter_kind, error);
    }

    /// Returns total capture count (optionally filtered by kind). Uses covering B-tree index.
    [[nodiscard]] std::optional<uint64_t> count(std::optional<CaptureKind> filter_kind = std::nullopt,
                                                std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;
        const char* sql = filter_kind
            ? "SELECT COUNT(*) FROM captures WHERE kind = ?1;"
            : "SELECT COUNT(*) FROM captures;";
        Stmt st;
        if (!prepare_locked(sql, st, error)) return std::nullopt;
        if (filter_kind) sqlite3_bind_int(st.s, 1, static_cast<int>(*filter_kind));
        if (sqlite3_step(st.s) != SQLITE_ROW) {
            if (error) *error = sqlite3_errmsg(db_);
            return std::nullopt;
        }
        return static_cast<uint64_t>(std::max<sqlite3_int64>(0, sqlite3_column_int64(st.s, 0)));
    }

    /// Fetches a paginated slice of captures ordered newest-first (`created_at DESC, id DESC`).
    [[nodiscard]] std::optional<std::vector<CaptureEntry>> list_page(int limit, int offset,
                                                                     std::optional<CaptureKind> filter_kind = std::nullopt,
                                                                     std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;
        if (limit <= 0) return std::vector<CaptureEntry>{};
        offset = std::max(0, offset);

        const char* sql = filter_kind
            ? "SELECT id, path, kind, mode, created_at, width, height, size_bytes, duration_ms, window_title, process_name, format "
              "FROM captures WHERE kind = ?1 ORDER BY created_at DESC, id DESC LIMIT ?2 OFFSET ?3;"
            : "SELECT id, path, kind, mode, created_at, width, height, size_bytes, duration_ms, window_title, process_name, format "
              "FROM captures ORDER BY created_at DESC, id DESC LIMIT ?1 OFFSET ?2;";

        Stmt st;
        if (!prepare_locked(sql, st, error)) return std::nullopt;
        if (filter_kind) {
            sqlite3_bind_int(st.s, 1, static_cast<int>(*filter_kind));
            sqlite3_bind_int(st.s, 2, limit);
            sqlite3_bind_int(st.s, 3, offset);
        } else {
            sqlite3_bind_int(st.s, 1, limit);
            sqlite3_bind_int(st.s, 2, offset);
        }

        std::vector<CaptureEntry> out;
        out.reserve(static_cast<size_t>(std::min(limit, 512)));
        for (;;) {
            const int rc = sqlite3_step(st.s);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            out.push_back(read_entry_row(st.s));
        }
        return out;
    }

    /// Fetches capture IDs for a contiguous slice `[offset, offset + limit)` in display order (`created_at DESC, id DESC`).
    [[nodiscard]] std::optional<std::vector<int64_t>> list_ids_range(int limit, int offset,
                                                                     std::optional<CaptureKind> filter_kind = std::nullopt,
                                                                     std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;
        if (limit <= 0) return std::vector<int64_t>{};
        offset = std::max(0, offset);

        const char* sql = filter_kind
            ? "SELECT id FROM captures WHERE kind = ?1 ORDER BY created_at DESC, id DESC LIMIT ?2 OFFSET ?3;"
            : "SELECT id FROM captures ORDER BY created_at DESC, id DESC LIMIT ?1 OFFSET ?2;";

        Stmt st;
        if (!prepare_locked(sql, st, error)) return std::nullopt;
        if (filter_kind) {
            sqlite3_bind_int(st.s, 1, static_cast<int>(*filter_kind));
            sqlite3_bind_int(st.s, 2, limit);
            sqlite3_bind_int(st.s, 3, offset);
        } else {
            sqlite3_bind_int(st.s, 1, limit);
            sqlite3_bind_int(st.s, 2, offset);
        }

        std::vector<int64_t> out;
        out.reserve(static_cast<size_t>(std::min(limit, 4096)));
        for (;;) {
            const int rc = sqlite3_step(st.s);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            out.push_back(sqlite3_column_int64(st.s, 0));
        }
        return out;
    }

    /// Searches all captures in SQLite (newest-first) matching multi-token AND `query_lower_utf8`.
    /// Returns matching capture IDs in display order (`created_at DESC, id DESC`).
    [[nodiscard]] std::optional<std::vector<int64_t>> search_ids(std::string_view query_lower_utf8,
                                                                 std::optional<CaptureKind> filter_kind = std::nullopt,
                                                                 std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;

        const char* sql = filter_kind
            ? "SELECT id FROM captures WHERE kind = ?1 AND "
              "capusy_match_query(path, window_title, process_name, format, mode, created_at, ?2) != 0 "
              "ORDER BY created_at DESC, id DESC;"
            : "SELECT id FROM captures WHERE "
              "capusy_match_query(path, window_title, process_name, format, mode, created_at, ?1) != 0 "
              "ORDER BY created_at DESC, id DESC;";

        Stmt st;
        if (!prepare_locked(sql, st, error)) return std::nullopt;
        if (filter_kind) {
            sqlite3_bind_int(st.s, 1, static_cast<int>(*filter_kind));
            sqlite3_bind_text(st.s, 2, query_lower_utf8.data(), static_cast<int>(query_lower_utf8.size()), SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_text(st.s, 1, query_lower_utf8.data(), static_cast<int>(query_lower_utf8.size()), SQLITE_TRANSIENT);
        }

        std::vector<int64_t> out;
        for (;;) {
            const int rc = sqlite3_step(st.s);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            out.push_back(sqlite3_column_int64(st.s, 0));
        }
        return out;
    }

    /// Fetches entries for the specified `ids` in the exact order given (missing IDs are omitted).
    [[nodiscard]] std::optional<std::vector<CaptureEntry>> get_by_ids(std::span<const int64_t> ids,
                                                                      std::string* error = nullptr) {
        if (ids.empty()) return std::vector<CaptureEntry>{};
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;

        std::unordered_map<int64_t, CaptureEntry> by_id;
        by_id.reserve(ids.size());

        constexpr size_t kChunk = 500;
        std::string sql;
        for (size_t off = 0; off < ids.size(); off += kChunk) {
            const size_t n = std::min(kChunk, ids.size() - off);
            sql.assign("SELECT id, path, kind, mode, created_at, width, height, size_bytes, duration_ms, window_title, process_name, format "
                       "FROM captures WHERE id IN (");
            for (size_t i = 0; i < n; ++i) {
                if (i > 0) sql.push_back(',');
                sql.push_back('?');
            }
            sql.append(");");

            Stmt st;
            if (!prepare_locked(sql.c_str(), st, error)) return std::nullopt;
            for (size_t i = 0; i < n; ++i) {
                sqlite3_bind_int64(st.s, static_cast<int>(i + 1), ids[off + i]);
            }
            for (;;) {
                const int rc = sqlite3_step(st.s);
                if (rc == SQLITE_DONE) break;
                if (rc != SQLITE_ROW) {
                    if (error) *error = sqlite3_errmsg(db_);
                    return std::nullopt;
                }
                CaptureEntry e = read_entry_row(st.s);
                const int64_t eid = e.id;
                by_id.emplace(eid, std::move(e));
            }
        }

        std::vector<CaptureEntry> out;
        out.reserve(by_id.size());
        for (const int64_t id : ids) {
            if (const auto it = by_id.find(id); it != by_id.end()) {
                out.push_back(it->second);
            }
        }
        return out;
    }

    /// Returns entries for `ids` ordered in display order (`created_at DESC, id DESC`).
    [[nodiscard]] std::optional<std::vector<CaptureEntry>> get_by_ids_ordered(std::span<const int64_t> ids,
                                                                              std::string* error = nullptr) {
        auto entries = get_by_ids(ids, error);
        if (!entries) return std::nullopt;
        std::ranges::sort(*entries, [](const CaptureEntry& a, const CaptureEntry& b) noexcept {
            if (a.created_at != b.created_at) return a.created_at > b.created_at;
            return a.id > b.id;
        });
        return entries;
    }

    /// Finds the exact 0-based row index of `id` in the unfiltered `ORDER BY created_at DESC, id DESC` list.
    /// Returns std::nullopt if `id` does not exist (or doesn't match `filter_kind`).
    [[nodiscard]] std::optional<int> find_row_index(int64_t id, std::optional<CaptureKind> filter_kind = std::nullopt,
                                                    std::string* error = nullptr) {
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;

        int64_t created_at = 0;
        int kind = 0;
        {
            Stmt q;
            if (!prepare_locked("SELECT created_at, kind FROM captures WHERE id = ?1;", q, error)) return std::nullopt;
            sqlite3_bind_int64(q.s, 1, id);
            const int rc = sqlite3_step(q.s);
            if (rc == SQLITE_DONE) return std::nullopt;
            if (rc != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            created_at = sqlite3_column_int64(q.s, 0);
            kind = sqlite3_column_int(q.s, 1);
        }
        if (filter_kind && kind != static_cast<int>(*filter_kind)) return std::nullopt;

        const char* sql = filter_kind
            ? "SELECT COUNT(*) FROM captures WHERE kind = ?1 AND (created_at > ?2 OR (created_at = ?2 AND id > ?3));"
            : "SELECT COUNT(*) FROM captures WHERE created_at > ?1 OR (created_at = ?1 AND id > ?2);";
        Stmt st;
        if (!prepare_locked(sql, st, error)) return std::nullopt;
        if (filter_kind) {
            sqlite3_bind_int(st.s, 1, static_cast<int>(*filter_kind));
            sqlite3_bind_int64(st.s, 2, created_at);
            sqlite3_bind_int64(st.s, 3, id);
        } else {
            sqlite3_bind_int64(st.s, 1, created_at);
            sqlite3_bind_int64(st.s, 2, id);
        }
        if (sqlite3_step(st.s) != SQLITE_ROW) {
            if (error) *error = sqlite3_errmsg(db_);
            return std::nullopt;
        }
        return static_cast<int>(sqlite3_column_int64(st.s, 0));
    }

    /// Computes image/video counts and total file size across an arbitrary set of selected capture IDs.
    [[nodiscard]] std::optional<SelectionSummary> summarize_ids(std::span<const int64_t> ids,
                                                                std::string* error = nullptr) {
        SelectionSummary sum{};
        if (ids.empty()) return sum;
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;

        constexpr size_t kChunk = 500;
        std::string sql;
        for (size_t off = 0; off < ids.size(); off += kChunk) {
            const size_t n = std::min(kChunk, ids.size() - off);
            sql.assign("SELECT COUNT(*), "
                       "COALESCE(SUM(CASE WHEN kind = 0 THEN 1 ELSE 0 END), 0), "
                       "COALESCE(SUM(CASE WHEN kind = 1 THEN 1 ELSE 0 END), 0), "
                       "COALESCE(SUM(size_bytes), 0) "
                       "FROM captures WHERE id IN (");
            for (size_t i = 0; i < n; ++i) {
                if (i > 0) sql.push_back(',');
                sql.push_back('?');
            }
            sql.append(");");

            Stmt st;
            if (!prepare_locked(sql.c_str(), st, error)) return std::nullopt;
            for (size_t i = 0; i < n; ++i) {
                sqlite3_bind_int64(st.s, static_cast<int>(i + 1), ids[off + i]);
            }
            if (sqlite3_step(st.s) != SQLITE_ROW) {
                if (error) *error = sqlite3_errmsg(db_);
                return std::nullopt;
            }
            sum.count += static_cast<uint64_t>(std::max<sqlite3_int64>(0, sqlite3_column_int64(st.s, 0)));
            sum.image_count += static_cast<uint64_t>(std::max<sqlite3_int64>(0, sqlite3_column_int64(st.s, 1)));
            sum.video_count += static_cast<uint64_t>(std::max<sqlite3_int64>(0, sqlite3_column_int64(st.s, 2)));
            sum.total_bytes += static_cast<uint64_t>(std::max<sqlite3_int64>(0, sqlite3_column_int64(st.s, 3)));
        }
        return sum;
    }

    /// Returns the subset of `ids` that currently exist in the database (optionally matching `filter_kind`).
    [[nodiscard]] std::optional<std::unordered_set<int64_t>> filter_existing_ids(
        std::span<const int64_t> ids, std::optional<CaptureKind> filter_kind = std::nullopt,
        std::string* error = nullptr) {
        std::unordered_set<int64_t> keep;
        if (ids.empty()) return keep;
        const std::scoped_lock lk{mx_};
        if (!require_open_locked(error)) return std::nullopt;

        keep.reserve(ids.size());
        constexpr size_t kChunk = 500;
        std::string sql;
        for (size_t off = 0; off < ids.size(); off += kChunk) {
            const size_t n = std::min(kChunk, ids.size() - off);
            sql.assign("SELECT id FROM captures WHERE ");
            if (filter_kind) sql.append("kind = ? AND ");
            sql.append("id IN (");
            for (size_t i = 0; i < n; ++i) {
                if (i > 0) sql.push_back(',');
                sql.push_back('?');
            }
            sql.append(");");

            Stmt st;
            if (!prepare_locked(sql.c_str(), st, error)) return std::nullopt;
            int bind_idx = 1;
            if (filter_kind) sqlite3_bind_int(st.s, bind_idx++, static_cast<int>(*filter_kind));
            for (size_t i = 0; i < n; ++i) {
                sqlite3_bind_int64(st.s, bind_idx++, ids[off + i]);
            }
            for (;;) {
                const int rc = sqlite3_step(st.s);
                if (rc == SQLITE_DONE) break;
                if (rc != SQLITE_ROW) {
                    if (error) *error = sqlite3_errmsg(db_);
                    return std::nullopt;
                }
                keep.insert(sqlite3_column_int64(st.s, 0));
            }
        }
        return keep;
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
    [[nodiscard]] static CaptureEntry read_entry_row(sqlite3_stmt* s) {
        const auto read_str = [](sqlite3_stmt* stmt, int col, std::string& dst) {
            const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            const int len = sqlite3_column_bytes(stmt, col);
            if (txt && len > 0) dst.assign(txt, static_cast<size_t>(len));
        };
        CaptureEntry e;
        e.id = sqlite3_column_int64(s, 0);
        read_str(s, 1, e.path);
        e.kind = static_cast<CaptureKind>(sqlite3_column_int(s, 2));
        e.mode = static_cast<CaptureMode>(sqlite3_column_int(s, 3));
        e.created_at = sqlite3_column_int64(s, 4);
        e.width = static_cast<uint32_t>(sqlite3_column_int64(s, 5));
        e.height = static_cast<uint32_t>(sqlite3_column_int64(s, 6));
        e.size_bytes = static_cast<uint64_t>(sqlite3_column_int64(s, 7));
        e.duration_ms = static_cast<uint64_t>(sqlite3_column_int64(s, 8));
        read_str(s, 9, e.window_title);
        read_str(s, 10, e.process_name);
        read_str(s, 11, e.format);
        return e;
    }

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
            if (col == "window_title") {
                has_window_title = true;
            } else if (col == "process_name") {
                has_process_name = true;
            } else if (col == "format") {
                has_format = true;
            }
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
