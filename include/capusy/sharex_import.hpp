#pragma once

#include "history_store.hpp"
#include "string_helpers.hpp"

#include <glaze/glaze.hpp>
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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace capusy {

enum class ShareXHistoryFormat : uint8_t {
    Unknown = 0,
    Sqlite,
    Json,
};

struct ShareXImportOptions {
    /// When true, verifies each referenced file still exists on disk and records its file size.
    bool only_existing_files = false;
};

struct ShareXImportResult {
    ShareXHistoryFormat format = ShareXHistoryFormat::Unknown;
    uint64_t total_seen = 0;
    uint64_t imported = 0;
    uint64_t imported_images = 0;
    uint64_t imported_videos = 0;
    uint64_t duplicates_skipped = 0;
    uint64_t missing_skipped = 0;
    uint64_t non_media_skipped = 0;
    bool cancelled = false;
};

struct ShareXMediaInfo {
    CaptureKind kind = CaptureKind::Image;
    CaptureMode mode = CaptureMode::Region;
    FixedString<16> format;
};

namespace detail {

[[nodiscard]] constexpr bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr char ascii_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

[[nodiscard]] constexpr std::string_view trim_ascii(std::string_view s) noexcept {
    while (!s.empty() && is_ascii_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ascii_space(s.back())) s.remove_suffix(1);
    return s;
}

[[nodiscard]] constexpr bool parse_uint_digits(std::string_view s, size_t offset, size_t count, unsigned& out) noexcept {
    if (offset + count > s.size()) return false;
    unsigned v = 0;
    for (size_t i = 0; i < count; ++i) {
        const char c = s[offset + i];
        if (!is_ascii_digit(c)) return false;
        v = (v * 10u) + static_cast<unsigned>(c - '0');
    }
    out = v;
    return true;
}

[[nodiscard]] constexpr bool is_leap_year(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

[[nodiscard]] constexpr unsigned days_in_month(int year, unsigned month) noexcept {
    static constexpr std::array<unsigned, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return kDays[month - 1];
}

/// Howard Hinnant's civil-date to days-since-1970-01-01 conversion.
[[nodiscard]] constexpr int64_t days_from_civil(int64_t y, unsigned m, unsigned d) noexcept {
    y -= (m <= 2u) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - (era * 400));
    const unsigned doy = (((153u * (m > 2u ? (m - 3u) : (m + 9u))) + 2u) / 5u) + d - 1u;
    const unsigned doe = (yoe * 365u) + (yoe / 4u) - (yoe / 100u) + doy;
    return (era * 146097) + static_cast<int64_t>(doe) - 719468;
}

struct ShareXTagsJson {
    std::optional<std::string> WindowTitle;
    std::optional<std::string> ProcessName;
};

struct ShareXHistoryItemJson {
    std::optional<std::string> FileName;
    std::optional<std::string> FilePath;
    std::optional<std::string> DateTime;
    std::optional<std::string> Type;
    std::optional<ShareXTagsJson> Tags;
};

struct FileProbe {
    bool exists = false;
    uint64_t size_bytes = 0;
    int64_t modified_unix = 0;
};

[[nodiscard]] inline FileProbe probe_local_file(std::string_view utf8_path) {
    FileProbe out{};
    if (utf8_path.empty()) return out;
#ifdef _WIN32
    const std::wstring wpath = to_wide(utf8_path);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data)) return out;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return out;
    out.exists = true;
    out.size_bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32u) | static_cast<uint64_t>(data.nFileSizeLow);
    const uint64_t ft = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32u) |
                        static_cast<uint64_t>(data.ftLastWriteTime.dwLowDateTime);
    constexpr uint64_t kTicksPerSec = 10'000'000ull;
    constexpr uint64_t kEpochDiffSec = 11'644'473'600ull;
    const uint64_t secs = ft / kTicksPerSec;
    if (secs > kEpochDiffSec) {
        out.modified_unix = static_cast<int64_t>(secs - kEpochDiffSec);
    }
#else
    std::error_code ec;
    const std::filesystem::path p{std::u8string_view{reinterpret_cast<const char8_t*>(utf8_path.data()), utf8_path.size()}};
    if (!std::filesystem::is_regular_file(p, ec) || ec) return out;
    out.exists = true;
    out.size_bytes = std::filesystem::file_size(p, ec);
#endif
    return out;
}

inline void parse_tags_json(std::string_view tags_json, std::string& out_title, std::string& out_process, glz::context& ctx) {
    const std::string_view trimmed = trim_ascii(tags_json);
    if (trimmed.size() < 2 || trimmed.front() != '{') return;
    if (trimmed == "{}") return;

    ShareXTagsJson tags{};
    constexpr auto opts = glz::opts{.error_on_unknown_keys = false};
    if (glz::read<opts>(tags, trimmed, ctx)) return;
    if (tags.WindowTitle) {
        const std::string_view t = trim_ascii(*tags.WindowTitle);
        if (!t.empty()) out_title.assign(t);
    }
    if (tags.ProcessName) {
        const std::string_view p = trim_ascii(*tags.ProcessName);
        if (!p.empty()) out_process.assign(p);
    }
}

[[nodiscard]] inline bool read_file_bytes(const std::filesystem::path& path, std::string& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "Could not open ShareX history file.";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streampos end_pos = in.tellg();
    if (end_pos < 0) {
        if (error) *error = "Could not determine ShareX history file size.";
        return false;
    }
    const auto sz = static_cast<size_t>(end_pos);
    out.clear();
    out.resize(sz);
    in.seekg(0, std::ios::beg);
    if (sz > 0 && !in.read(out.data(), static_cast<std::streamsize>(sz))) {
        if (error) *error = "Failed to read ShareX history file.";
        return false;
    }
    return true;
}

} // namespace detail

/// Parses a ShareX ISO-8601 / .NET round-trip ("o") DateTime string into Unix seconds (UTC).
/// Supports fractional seconds (e.g. `.3725698`), `Z`, `±HH:MM`, `±HHMM`, and timezone-less local timestamps.
[[nodiscard]] inline std::optional<int64_t> parse_sharex_datetime_unix(std::string_view raw) noexcept {
    const std::string_view s = detail::trim_ascii(raw);
    if (s.size() < 10) return std::nullopt;
    if (s[4] != '-' || s[7] != '-') return std::nullopt;

    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!detail::parse_uint_digits(s, 0, 4, year) ||
        !detail::parse_uint_digits(s, 5, 2, month) ||
        !detail::parse_uint_digits(s, 8, 2, day)) {
        return std::nullopt;
    }
    if (month < 1u || month > 12u || day < 1u || day > detail::days_in_month(static_cast<int>(year), month)) {
        return std::nullopt;
    }

    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    size_t pos = 10;
    bool has_tz = false;
    int64_t tz_offset_s = 0;

    if (pos < s.size()) {
        if (s[pos] != 'T' && s[pos] != 't' && s[pos] != ' ') return std::nullopt;
        ++pos;
        if (pos + 8 > s.size() || s[pos + 2] != ':' || s[pos + 5] != ':') return std::nullopt;
        if (!detail::parse_uint_digits(s, pos, 2, hour) ||
            !detail::parse_uint_digits(s, pos + 3, 2, minute) ||
            !detail::parse_uint_digits(s, pos + 6, 2, second)) {
            return std::nullopt;
        }
        if (hour > 23u || minute > 59u || second > 60u) return std::nullopt;
        if (second == 60u) second = 59u;
        pos += 8;

        // Optional fractional seconds (e.g. .3725698)
        if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
            ++pos;
            const size_t frac_start = pos;
            while (pos < s.size() && detail::is_ascii_digit(s[pos])) ++pos;
            if (pos == frac_start) return std::nullopt;
        }

        // Optional timezone suffix: Z / z / +HH:MM / -HH:MM / +HHMM / -HHMM / +HH
        if (pos < s.size()) {
            const char tc = s[pos];
            if (tc == 'Z' || tc == 'z') {
                if (pos + 1 != s.size()) return std::nullopt;
                has_tz = true;
                tz_offset_s = 0;
            } else if (tc == '+' || tc == '-') {
                const int64_t sign = (tc == '+') ? 1 : -1;
                ++pos;
                unsigned tz_h = 0;
                unsigned tz_m = 0;
                if (!detail::parse_uint_digits(s, pos, 2, tz_h)) return std::nullopt;
                pos += 2;
                if (pos < s.size()) {
                    if (s[pos] == ':') ++pos;
                    if (!detail::parse_uint_digits(s, pos, 2, tz_m)) return std::nullopt;
                    pos += 2;
                }
                if (pos != s.size() || tz_h > 23u || tz_m > 59u) return std::nullopt;
                has_tz = true;
                tz_offset_s = sign * ((static_cast<int64_t>(tz_h) * 3600) + (static_cast<int64_t>(tz_m) * 60));
            } else {
                return std::nullopt;
            }
        }
    }

    if (!has_tz && s.size() > 10) {
#ifdef _WIN32
        if (year >= 1601u) {
            SYSTEMTIME local_st{};
            local_st.wYear = static_cast<WORD>(year);
            local_st.wMonth = static_cast<WORD>(month);
            local_st.wDay = static_cast<WORD>(day);
            local_st.wHour = static_cast<WORD>(hour);
            local_st.wMinute = static_cast<WORD>(minute);
            local_st.wSecond = static_cast<WORD>(second);
            SYSTEMTIME utc_st{};
            FILETIME utc_ft{};
            if (TzSpecificLocalTimeToSystemTime(nullptr, &local_st, &utc_st) &&
                SystemTimeToFileTime(&utc_st, &utc_ft)) {
                const uint64_t ft = (static_cast<uint64_t>(utc_ft.dwHighDateTime) << 32u) |
                                    static_cast<uint64_t>(utc_ft.dwLowDateTime);
                const int64_t secs = static_cast<int64_t>(ft / 10'000'000ull) - 11'644'473'600ll;
                return secs;
            }
        }
#endif
    }

    const int64_t days = detail::days_from_civil(static_cast<int64_t>(year), month, day);
    const int64_t civil_s = (days * 86400) + (static_cast<int64_t>(hour) * 3600) +
                            (static_cast<int64_t>(minute) * 60) + static_cast<int64_t>(second);
    return civil_s - tz_offset_s;
}

/// Classifies a ShareX history item into a capusy image/video media descriptor.
/// Returns std::nullopt for non-media entries (text uploads, URLs, archives, documents, etc.).
[[nodiscard]] inline std::optional<ShareXMediaInfo> classify_sharex_media(
    std::string_view file_path, std::string_view sharex_type) noexcept {
    const size_t slash = file_path.find_last_of("\\/");
    const std::string_view base = (slash != std::string_view::npos) ? file_path.substr(slash + 1) : file_path;
    const size_t dot = base.find_last_of('.');

    FixedString<16> ext_lower;
    if (dot != std::string_view::npos && dot + 1 < base.size()) {
        const std::string_view raw_ext = base.substr(dot + 1);
        if (raw_ext.size() <= 12) {
            for (const char c : raw_ext) ext_lower.push_back(detail::ascii_lower(c));
        }
    }
    const std::string_view ext = ext_lower.view();

    // Known image extensions
    if (ext == "png") return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"PNG"}};
    if (ext == "jpg" || ext == "jpeg" || ext == "jfif" || ext == "jpe") {
        return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"JPEG"}};
    }
    if (ext == "bmp" || ext == "dib") {
        return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"BMP"}};
    }
    if (ext == "tif" || ext == "tiff") {
        return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"TIFF"}};
    }
    if (ext == "gif") {
        return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"GIF"}};
    }
    if (ext == "webp") return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"WEBP"}};
    if (ext == "avif") return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"AVIF"}};
    if (ext == "heic") return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"HEIC"}};
    if (ext == "heif") return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"HEIF"}};
    if (ext == "ico") return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"ICO"}};
    if (ext == "jxr" || ext == "wdp" || ext == "hdp") {
        return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = FixedString<16>{"JXR"}};
    }

    // Known video extensions (ShareX records screen recordings with Type="File" and .mp4/.webm/etc.)
    if (ext == "mp4" || ext == "m4v") {
        return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"MP4"}};
    }
    if (ext == "wmv" || ext == "asf") {
        return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"WMV"}};
    }
    if (ext == "webm") return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"WEBM"}};
    if (ext == "mkv") return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"MKV"}};
    if (ext == "avi") return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"AVI"}};
    if (ext == "mov") return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"MOV"}};
    if (ext == "flv") return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"FLV"}};
    if (ext == "mpg" || ext == "mpeg") {
        return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"MPEG"}};
    }
    if (ext == "ts" || ext == "m2ts") {
        return ShareXMediaInfo{.kind = CaptureKind::Video, .mode = CaptureMode::Recording, .format = FixedString<16>{"TS"}};
    }

    // Fallback: if ShareX explicitly tagged it as "Image", accept it with uppercase extension if present
    if (detail::iequals_ascii(detail::trim_ascii(sharex_type), "Image")) {
        FixedString<16> fmt;
        for (const char c : ext) fmt.push_back(detail::ascii_upper(c));
        return ShareXMediaInfo{.kind = CaptureKind::Image, .mode = CaptureMode::Region, .format = fmt};
    }

    return std::nullopt;
}

/// Detects whether `file_path` is a ShareX SQLite `History.db` or legacy ShareX JSON history file
/// by inspecting magic bytes first and falling back to file extension.
[[nodiscard]] inline ShareXHistoryFormat detect_sharex_history_format(const std::filesystem::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    if (in) {
        std::array<char, 64> hdr{};
        in.read(hdr.data(), static_cast<std::streamsize>(hdr.size()));
        const auto n = static_cast<size_t>(in.gcount());
        static constexpr std::array<char, 16> kSqliteMagic = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', '\0'};
        if (n >= 16 && std::memcmp(hdr.data(), kSqliteMagic.data(), 16) == 0) {
            return ShareXHistoryFormat::Sqlite;
        }
        size_t pos = 0;
        if (n >= 3 &&
            static_cast<uint8_t>(hdr[0]) == 0xEFu &&
            static_cast<uint8_t>(hdr[1]) == 0xBBu &&
            static_cast<uint8_t>(hdr[2]) == 0xBFu) {
            pos = 3;
        }
        while (pos < n && detail::is_ascii_space(hdr[pos])) ++pos;
        if (pos < n && (hdr[pos] == '{' || hdr[pos] == '[')) {
            return ShareXHistoryFormat::Json;
        }
    }

    const std::wstring ext = file_path.extension().wstring();
    if (ext.size() <= 10) {
        FixedString<16> low;
        for (const wchar_t c : ext) {
            if (c > 0 && c < 128) low.push_back(detail::ascii_lower(static_cast<char>(c)));
        }
        if (low.view() == ".db" || low.view() == ".sqlite" || low.view() == ".sqlite3") {
            return ShareXHistoryFormat::Sqlite;
        }
        if (low.view() == ".json") {
            return ShareXHistoryFormat::Json;
        }
    }
    return ShareXHistoryFormat::Unknown;
}

/// Normalizes raw legacy ShareX JSON text (UTF-8 BOM, unwrapped comma-separated objects, optional trailing comma)
/// into a valid JSON array string ready for deserialization.
[[nodiscard]] inline std::string normalize_sharex_json_array(std::string_view raw) {
    if (raw.size() >= 3 &&
        static_cast<uint8_t>(raw[0]) == 0xEFu &&
        static_cast<uint8_t>(raw[1]) == 0xBBu &&
        static_cast<uint8_t>(raw[2]) == 0xBFu) {
        raw.remove_prefix(3);
    }
    while (!raw.empty() && (detail::is_ascii_space(raw.front()) || raw.front() == '\0')) raw.remove_prefix(1);
    while (!raw.empty() && (detail::is_ascii_space(raw.back()) || raw.back() == '\0')) raw.remove_suffix(1);
    if (raw.empty()) return "[]";

    if (raw.front() == '[') {
        return std::string{raw};
    }

    // Legacy ShareX History.json stores comma-separated objects without enclosing `[` `]`.
    // Strip any stray trailing comma left by an interrupted append.
    while (!raw.empty() && (raw.back() == ',' || detail::is_ascii_space(raw.back()))) {
        raw.remove_suffix(1);
    }
    if (raw.empty()) return "[]";

    std::string out;
    out.reserve(raw.size() + 2);
    out.push_back('[');
    out.append(raw);
    out.push_back(']');
    return out;
}

namespace detail {

inline bool flush_import_batch(HistoryStore& store, std::vector<CaptureEntry>& batch,
                               ShareXImportResult& result, std::string* error) {
    if (batch.empty()) return true;
    BatchInsertResult br{};
    if (!store.insert_many_ignore(batch, &br, error)) return false;
    result.imported += br.inserted;
    result.imported_images += br.inserted_images;
    result.imported_videos += br.inserted_videos;
    result.duplicates_skipped += br.duplicates;
    batch.clear();
    return true;
}

inline void build_entry_from_raw(std::string_view file_path, std::string_view datetime_str, std::string_view type_str,
                                 std::string_view window_title, std::string_view process_name,
                                 const ShareXImportOptions& opts,
                                 ShareXImportResult& result, std::vector<CaptureEntry>& batch) {
    ++result.total_seen;
    const std::string_view trimmed_path = trim_ascii(file_path);
    if (trimmed_path.empty()) {
        ++result.non_media_skipped;
        return;
    }

    const auto media = classify_sharex_media(trimmed_path, type_str);
    if (!media) {
        ++result.non_media_skipped;
        return;
    }

    uint64_t size_bytes = 0;
    int64_t fallback_ts = 0;
    if (opts.only_existing_files) {
        const FileProbe probe = probe_local_file(trimmed_path);
        if (!probe.exists) {
            ++result.missing_skipped;
            return;
        }
        size_bytes = probe.size_bytes;
        fallback_ts = probe.modified_unix;
    }

    int64_t created_at = 0;
    if (const auto parsed = parse_sharex_datetime_unix(datetime_str)) {
        created_at = *parsed;
    } else if (fallback_ts > 0) {
        created_at = fallback_ts;
    }

    CaptureEntry entry;
    entry.path.assign(trimmed_path);
    entry.kind = media->kind;
    entry.mode = media->mode;
    entry.created_at = created_at;
    entry.size_bytes = size_bytes;
    if (!window_title.empty()) entry.window_title.assign(window_title);
    if (!process_name.empty()) entry.process_name.assign(process_name);
    entry.format.assign(media->format.view());
    batch.push_back(std::move(entry));
}

inline bool import_sharex_sqlite(
    const std::filesystem::path& file_path,
    HistoryStore& store,
    const ShareXImportOptions& opts,
    ShareXImportResult& result,
    const std::function<void(uint64_t done, uint64_t total)>& on_progress,
    const std::stop_token& stop,
    std::string* error) {

    const auto u8 = file_path.u8string();
    const std::string utf8{reinterpret_cast<const char*>(u8.data()), u8.size()};

    sqlite3* raw = nullptr;
    const int open_rc = sqlite3_open_v2(utf8.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (open_rc != SQLITE_OK || !raw) {
        if (error) *error = raw ? sqlite3_errmsg(raw) : sqlite3_errstr(open_rc);
        if (raw) sqlite3_close(raw);
        return false;
    }
    struct DbGuard {
        sqlite3* db = nullptr;
        explicit DbGuard(sqlite3* d) noexcept : db(d) {}
        DbGuard(const DbGuard&) = delete;
        DbGuard& operator=(const DbGuard&) = delete;
        DbGuard(DbGuard&&) = delete;
        DbGuard& operator=(DbGuard&&) = delete;
        ~DbGuard() {
            if (db) sqlite3_close(db);
        }
    };
    const DbGuard db_guard{raw};
    sqlite3_busy_timeout(raw, 5000);

    // Inspect History table columns to validate schema and support older/newer ShareX schemas.
    bool has_filepath = false;
    bool has_datetime = false;
    bool has_type = false;
    bool has_tags = false;
    {
        sqlite3_stmt* info = nullptr;
        if (sqlite3_prepare_v2(raw, "PRAGMA table_info(History);", -1, &info, nullptr) != SQLITE_OK) {
            if (error) *error = "Not a valid ShareX SQLite database.";
            return false;
        }
        while (sqlite3_step(info) == SQLITE_ROW) {
            const auto* col = reinterpret_cast<const char*>(sqlite3_column_text(info, 1));
            if (!col) continue;
            const std::string_view name{col};
            if (iequals_ascii(name, "FilePath")) {
                has_filepath = true;
            } else if (iequals_ascii(name, "DateTime")) {
                has_datetime = true;
            } else if (iequals_ascii(name, "Type")) {
                has_type = true;
            } else if (iequals_ascii(name, "Tags")) {
                has_tags = true;
            }
        }
        sqlite3_finalize(info);
    }
    if (!has_filepath) {
        if (error) *error = "Not a valid ShareX History.db (missing History table or FilePath column).";
        return false;
    }

    uint64_t total_rows = 0;
    {
        sqlite3_stmt* cnt = nullptr;
        if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM History;", -1, &cnt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(cnt) == SQLITE_ROW) {
                total_rows = static_cast<uint64_t>(std::max<sqlite3_int64>(0, sqlite3_column_int64(cnt, 0)));
            }
            sqlite3_finalize(cnt);
        }
    }
    if (on_progress) on_progress(0, total_rows);

    std::string sql = "SELECT FilePath, ";
    sql += has_datetime ? "DateTime, " : "'', ";
    sql += has_type ? "Type, " : "'', ";
    sql += has_tags ? "Tags " : "'' ";
    sql += "FROM History;";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(raw, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(raw);
        return false;
    }
    struct StmtGuard {
        sqlite3_stmt* s = nullptr;
        explicit StmtGuard(sqlite3_stmt* st_in) noexcept : s(st_in) {}
        StmtGuard(const StmtGuard&) = delete;
        StmtGuard& operator=(const StmtGuard&) = delete;
        StmtGuard(StmtGuard&&) = delete;
        StmtGuard& operator=(StmtGuard&&) = delete;
        ~StmtGuard() {
            if (s) sqlite3_finalize(s);
        }
    };
    const StmtGuard st_guard{st};

    const auto col_sv = [](sqlite3_stmt* s, int col) noexcept -> std::string_view {
        const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(s, col));
        const int len = sqlite3_column_bytes(s, col);
        return (txt && len > 0) ? std::string_view{txt, static_cast<size_t>(len)} : std::string_view{};
    };

    constexpr size_t kBatchSize = 2000;
    std::vector<CaptureEntry> batch;
    batch.reserve(kBatchSize);
    std::string title_buf;
    std::string proc_buf;
    glz::context glz_ctx{};

    uint64_t processed = 0;
    for (;;) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            break;
        }
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            if (error) *error = sqlite3_errmsg(raw);
            return false;
        }

        const std::string_view fp = col_sv(st, 0);
        const std::string_view dt = col_sv(st, 1);
        const std::string_view typ = col_sv(st, 2);
        const std::string_view tags = col_sv(st, 3);

        title_buf.clear();
        proc_buf.clear();
        if (!tags.empty()) {
            parse_tags_json(tags, title_buf, proc_buf, glz_ctx);
        }

        build_entry_from_raw(fp, dt, typ, title_buf, proc_buf, opts, result, batch);
        ++processed;

        if (batch.size() >= kBatchSize && !flush_import_batch(store, batch, result, error)) {
            return false;
        }
        if (on_progress && (processed % 250u == 0u || processed == total_rows)) {
            on_progress(processed, total_rows);
        }
    }

    if (!flush_import_batch(store, batch, result, error)) return false;
    if (on_progress) on_progress(processed, total_rows);
    return true;
}

inline bool import_sharex_json(
    const std::filesystem::path& file_path,
    HistoryStore& store,
    const ShareXImportOptions& opts,
    ShareXImportResult& result,
    const std::function<void(uint64_t done, uint64_t total)>& on_progress,
    const std::stop_token& stop,
    std::string* error) {

    std::string raw;
    if (!read_file_bytes(file_path, raw, error)) return false;
    if (stop.stop_requested()) {
        result.cancelled = true;
        return true;
    }

    const std::string normalized = normalize_sharex_json_array(raw);
    raw.clear();
    raw.shrink_to_fit();

    std::vector<ShareXHistoryItemJson> items;
    glz::context glz_ctx{};
    constexpr auto glz_opts = glz::opts{.error_on_unknown_keys = false};
    if (const auto parse_err = glz::read<glz_opts>(items, normalized, glz_ctx)) {
        if (error) *error = "Failed to parse ShareX JSON history file.";
        return false;
    }

    // Guard against accidentally selecting a non-history ShareX JSON config (e.g. ApplicationConfig.json).
    if (!items.empty()) {
        bool looks_like_history = false;
        for (const auto& item : items) {
            if (item.FilePath.has_value() || item.FileName.has_value() || item.DateTime.has_value()) {
                looks_like_history = true;
                break;
            }
        }
        if (!looks_like_history) {
            if (error) *error = "This JSON file does not appear to be a ShareX history file.";
            return false;
        }
    }

    const uint64_t total = items.size();
    if (on_progress) on_progress(0, total);

    constexpr size_t kBatchSize = 2000;
    std::vector<CaptureEntry> batch;
    batch.reserve(std::min<size_t>(items.size(), kBatchSize));

    uint64_t processed = 0;
    for (const auto& item : items) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            break;
        }
        const std::string_view fp = item.FilePath ? std::string_view{*item.FilePath} : std::string_view{};
        const std::string_view dt = item.DateTime ? std::string_view{*item.DateTime} : std::string_view{};
        const std::string_view typ = item.Type ? std::string_view{*item.Type} : std::string_view{};
        std::string_view title;
        std::string_view proc;
        if (item.Tags) {
            if (item.Tags->WindowTitle) title = trim_ascii(*item.Tags->WindowTitle);
            if (item.Tags->ProcessName) proc = trim_ascii(*item.Tags->ProcessName);
        }

        build_entry_from_raw(fp, dt, typ, title, proc, opts, result, batch);
        ++processed;

        if (batch.size() >= kBatchSize && !flush_import_batch(store, batch, result, error)) {
            return false;
        }
        if (on_progress && (processed % 250u == 0u || processed == total)) {
            on_progress(processed, total);
        }
    }

    if (!flush_import_batch(store, batch, result, error)) return false;
    if (on_progress) on_progress(processed, total);
    return true;
}

} // namespace detail

/// Imports capture history from a ShareX `History.db` (SQLite) or legacy `History.json` file into `store`.
/// Duplicate paths already in `store` are skipped without overwriting existing entries.
inline bool import_sharex_history(
    const std::filesystem::path& file_path,
    HistoryStore& store,
    const ShareXImportOptions& opts,
    ShareXImportResult& result,
    std::string* error = nullptr,
    const std::function<void(uint64_t done, uint64_t total)>& on_progress = {},
    const std::stop_token& stop = {}) {

    result = {};
    if (!store.opened()) {
        if (error) *error = "capusy history database is not open.";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(file_path, ec) || ec) {
        if (error) *error = "Selected ShareX history file does not exist.";
        return false;
    }

    result.format = detect_sharex_history_format(file_path);
    if (result.format == ShareXHistoryFormat::Sqlite) {
        return detail::import_sharex_sqlite(file_path, store, opts, result, on_progress, stop, error);
    }
    if (result.format == ShareXHistoryFormat::Json) {
        return detail::import_sharex_json(file_path, store, opts, result, on_progress, stop, error);
    }
    if (error) *error = "Unsupported file format. Please select a ShareX History.db or legacy History.json file.";
    return false;
}

namespace detail {

[[nodiscard]] inline FixedWString<32> format_u64_commas(uint64_t n) noexcept {
    std::array<wchar_t, 32> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
    size_t pos = buf.size();
    unsigned group = 0;
    do {
        if (group == 3u) {
            if (pos == 0) break;
            buf[--pos] = L',';
            group = 0;
        }
        if (pos == 0) break;
        buf[--pos] = static_cast<wchar_t>(L'0' + (n % 10u));
        n /= 10u;
        ++group;
    } while (n > 0);
    return FixedWString<32>{std::wstring_view{buf.data() + pos, buf.size() - pos}};
}

} // namespace detail

/// Formats a user-facing summary of a completed ShareX history import.
[[nodiscard]] inline std::wstring format_sharex_import_summary(const ShareXImportResult& r, std::wstring_view filename) {
    std::wstring msg;
    if (r.cancelled) {
        append_to(msg, L"Import cancelled — ");
    }
    append_to(msg, L"Imported ", detail::format_u64_commas(r.imported).view(),
              r.imported == 1 ? L" capture" : L" captures");
    if (r.imported > 0) {
        append_to(msg, L" (", detail::format_u64_commas(r.imported_images).view(),
                  r.imported_images == 1 ? L" image, " : L" images, ",
                  detail::format_u64_commas(r.imported_videos).view(),
                  r.imported_videos == 1 ? L" video)" : L" videos)");
    }
    if (!filename.empty()) {
        append_to(msg, L" from ", filename);
    }
    const bool has_skips = (r.duplicates_skipped > 0 || r.missing_skipped > 0 || r.non_media_skipped > 0);
    if (has_skips) {
        msg += L"  •  Skipped: ";
        bool first = true;
        const auto add_skip = [&](uint64_t n, std::wstring_view label) {
            if (n == 0) return;
            if (!first) msg += L", ";
            append_to(msg, detail::format_u64_commas(n).view(), L" ", label);
            first = false;
        };
        add_skip(r.duplicates_skipped, L"already in history");
        add_skip(r.missing_skipped, L"missing files");
        add_skip(r.non_media_skipped, L"non-media");
    }
    return msg;
}

} // namespace capusy
