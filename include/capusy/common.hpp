#pragma once

#include <usylibpp/usylibpp.hpp>

#include <windows.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <rapidhash.h>
#include <capusy_version.h>
#include "string_helpers.hpp"

namespace capusy {

namespace fs = std::filesystem;

inline constexpr const wchar_t* APP_NAME = L"capusy";
inline constexpr const wchar_t* APP_VERSION = CAPUSY_VERSION_WSTR;

inline constexpr const wchar_t* INSTANCE_MUTEX = L"Local\\capusy.instance";
inline constexpr const wchar_t* TRAY_CLASS = L"capusy.tray";
inline constexpr const wchar_t* HISTORY_CLASS = L"capusy.history";
inline constexpr const wchar_t* SETTINGS_CLASS = L"capusy.settings";
inline constexpr const wchar_t* TOAST_CLASS = L"capusy.toast";
inline constexpr const wchar_t* REGION_OVERLAY_CLASS = L"capusy.region_overlay";
inline constexpr const wchar_t* REC_BORDER_CLASS = L"capusy.rec_border";
inline constexpr const wchar_t* REC_PILL_CLASS = L"capusy.rec_pill";

inline UINT msg_open_history() noexcept {
    static const UINT m = RegisterWindowMessageW(L"capusy.open_history");
    return m;
}
inline UINT msg_open_settings() noexcept {
    static const UINT m = RegisterWindowMessageW(L"capusy.open_settings");
    return m;
}

inline std::wstring lower(std::wstring_view s) {
    std::wstring out{s};
    if (!out.empty()) {
        CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    return out;
}

inline std::wstring trim(std::wstring_view s) {
    const auto b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring_view::npos) return {};
    const auto e = s.find_last_not_of(L" \t\r\n");
    return std::wstring{s.substr(b, e - b + 1)};
}

inline uint64_t path_id(std::wstring_view path) {
    const std::wstring l = lower(path);
    return rapidhash(l.data(), l.size() * sizeof(wchar_t));
}

inline std::string win32_error_utf8(DWORD err) {
    if (err == 0) return "Success";
    LPWSTR buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string out;
    if (n > 0 && buf) {
        std::wstring_view sv{buf, n};
        while (!sv.empty() && (sv.back() == L'\r' || sv.back() == L'\n' || sv.back() == L' ')) sv.remove_suffix(1);
        out = to_utf8(sv);
        LocalFree(buf);
    }
    if (out.empty()) {
        out = ulp::str::concat_strings("Error ", to_fixed_str(err));
    }
    return out;
}

inline std::wstring_view format_count_to(uint64_t n, std::span<wchar_t> buf) noexcept {
    if (buf.empty()) return {};
    size_t pos = buf.size();
    unsigned group = 0;
    do {
        if (group == 3) {
            if (pos == 0) break;
            buf[--pos] = L',';
            group = 0;
        }
        if (pos == 0) break;
        buf[--pos] = static_cast<wchar_t>(L'0' + (n % 10));
        n /= 10;
        ++group;
    } while (n > 0);
    return {buf.data() + pos, buf.size() - pos};
}

inline FixedWString<32> format_count(uint64_t n) noexcept {
    std::array<wchar_t, 32> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
    return FixedWString<32>{format_count_to(n, buf)};
}

inline FixedWString<32> format_size(uint64_t bytes) noexcept {
    FixedWString<32> out;
    if (bytes >= (1ull << 30u)) {
        out = to_fixed_wstr<32>(static_cast<double>(bytes) / (1ull << 30u), 1);
        out.append(L" GB");
    } else if (bytes >= (1ull << 20u)) {
        out = to_fixed_wstr<32>(static_cast<double>(bytes) / (1ull << 20u), 1);
        out.append(L" MB");
    } else if (bytes >= 1024) {
        out = to_fixed_wstr<32>(static_cast<double>(bytes) / 1024.0, 0);
        out.append(L" KB");
    } else {
        out = to_fixed_wstr<32>(bytes);
        out.append(L" B");
    }
    return out;
}

inline FixedWString<32> format_duration_ms(uint64_t ms) noexcept {
    const uint64_t s = ms / 1000;
    FixedWString<32> out;
    const auto push2 = [&](uint64_t v) noexcept {
        out.push_back(static_cast<wchar_t>(L'0' + ((v / 10) % 10)));
        out.push_back(static_cast<wchar_t>(L'0' + (v % 10)));
    };
    if (s >= 3600) {
        out.assign(to_fixed_wstr<32>(s / 3600));
        out.push_back(L':');
        push2((s / 60) % 60);
    } else {
        out.assign(to_fixed_wstr<32>(s / 60));
    }
    out.push_back(L':');
    push2(s % 60);
    return out;
}

inline int64_t unix_now() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline FixedWString<32> format_timestamp_iso(int64_t unix_s) noexcept {
    FixedWString<32> out;
    if (unix_s <= 0) return out;
    const uint64_t ft = (static_cast<uint64_t>(unix_s) + 11'644'473'600ull) * 10'000'000ull;
    const FILETIME f{.dwLowDateTime = static_cast<DWORD>(ft), .dwHighDateTime = static_cast<DWORD>(ft >> 32u)};
    FILETIME local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&f, &local);
    FileTimeToSystemTime(&local, &st);

    const auto push2 = [&](WORD v) noexcept {
        out.push_back(static_cast<wchar_t>(L'0' + ((v / 10) % 10)));
        out.push_back(static_cast<wchar_t>(L'0' + (v % 10)));
    };
    out.append(to_fixed_wstr<8>(st.wYear));
    out.push_back(L'-');
    push2(st.wMonth);
    out.push_back(L'-');
    push2(st.wDay);
    out.push_back(L' ');
    push2(st.wHour);
    out.push_back(L':');
    push2(st.wMinute);
    out.push_back(L':');
    push2(st.wSecond);
    return out;
}

inline FixedWString<64> format_timestamp(int64_t unix_s) noexcept {
    FixedWString<64> out;
    if (unix_s <= 0) return out;
    const uint64_t ft = (static_cast<uint64_t>(unix_s) + 11'644'473'600ull) * 10'000'000ull;
    const FILETIME f{.dwLowDateTime = static_cast<DWORD>(ft), .dwHighDateTime = static_cast<DWORD>(ft >> 32u)};
    FILETIME local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&f, &local);
    FileTimeToSystemTime(&local, &st);

    std::array<wchar_t, 32> d_buf{};
    std::array<wchar_t, 16> t_buf{};
    const int dn = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"d MMM yyyy", d_buf.data(), static_cast<int>(d_buf.size()), nullptr);
    const int tn = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, L"HH:mm", t_buf.data(), static_cast<int>(t_buf.size()));
    if (dn > 1) out.append(std::wstring_view{d_buf.data(), static_cast<size_t>(dn - 1)});
    if (dn > 1 && tn > 1) out.append(L"  •  ");
    if (tn > 1) out.append(std::wstring_view{t_buf.data(), static_cast<size_t>(tn - 1)});
    return out;
}

inline FixedWString<48> format_ago(int64_t unix_time) noexcept {
    if (unix_time <= 0) return FixedWString<48>{L"Never"};
    const int64_t d = std::max<int64_t>(0, unix_now() - unix_time);
    if (d < 60) return FixedWString<48>{L"Just now"};
    FixedWString<48> out;
    if (d < 3600) {
        out = to_fixed_wstr<48>(d / 60);
        out.append(L" min ago");
    } else if (d < 86400) {
        out = to_fixed_wstr<48>(d / 3600);
        out.append(L" h ago");
    } else {
        out = to_fixed_wstr<48>(d / 86400);
        out.append(L" d ago");
    }
    return out;
}

inline std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

inline const fs::path& exe_dir() {
    static const fs::path p = [] {
        std::wstring buf;
        size_t cap = MAX_PATH;
        for (;;) {
            bool done = false;
            buf.resize_and_overwrite(cap, [&](wchar_t* ptr, size_t n) noexcept {
                const DWORD len = GetModuleFileNameW(nullptr, ptr, static_cast<DWORD>(n));
                if (len < n) {
                    done = true;
                    return static_cast<size_t>(len);
                }
                return n;
            });
            if (done) break;
            cap *= 2;
        }
        return fs::path{buf}.parent_path();
    }();
    return p;
}

inline const fs::path& data_dir() {
    static const fs::path p = [] {
        fs::path d = exe_dir() / L"data";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }();
    return p;
}

inline const fs::path& settings_path() {
    static const fs::path p = data_dir() / L"settings.json";
    return p;
}

inline const fs::path& history_db_path() {
    static const fs::path p = data_dir() / L"history.db";
    return p;
}

inline const fs::path& update_state_path() {
    static const fs::path p = data_dir() / L"update_state.json";
    return p;
}

struct WindowInfo {
    std::string title;
    std::string process_name;
};

/// Reads the title and executable filename of `hwnd` (returns empty fields if invalid or owned by this process).
inline WindowInfo query_window_info(HWND hwnd) {
    WindowInfo info;
    if (!hwnd) return info;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return info;

    std::array<wchar_t, 512> title_buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
    const int tlen = GetWindowTextW(hwnd, title_buf.data(), static_cast<int>(title_buf.size()));
    if (tlen > 0) {
        std::wstring_view sv{title_buf.data(), static_cast<size_t>(tlen)};
        while (!sv.empty() && (sv.front() == L' ' || sv.front() == L'\t' || sv.front() == L'\r' || sv.front() == L'\n')) sv.remove_prefix(1);
        while (!sv.empty() && (sv.back() == L' ' || sv.back() == L'\t' || sv.back() == L'\r' || sv.back() == L'\n')) sv.remove_suffix(1);
        if (!sv.empty()) info.title = to_utf8(sv);
    }

    if (const wil::unique_handle proc{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)}) {
        std::array<wchar_t, 512> exe_buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
        auto len = static_cast<DWORD>(exe_buf.size());
        if (QueryFullProcessImageNameW(proc.get(), 0, exe_buf.data(), &len) && len > 0) {
            std::wstring_view full{exe_buf.data(), len};
            const size_t slash = full.find_last_of(L"\\/");
            const std::wstring_view base = (slash != std::wstring_view::npos) ? full.substr(slash + 1) : full;
            if (!base.empty()) info.process_name = to_utf8(base);
        }
    }
    return info;
}

inline WindowInfo foreground_window_info() {
    HWND fg = GetForegroundWindow();
    if (!fg) return {};
    HWND root = GetAncestor(fg, GA_ROOTOWNER);
    if (root) fg = root;
    return query_window_info(fg);
}

inline fs::path default_captures_dir() {
    std::wstring pics = known_folder(FOLDERID_Pictures);
    fs::path d = pics.empty() ? (data_dir() / L"captures") : (fs::path{pics} / L"capusy");
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

/// RAII owner of a cross-process named Win32 mutex acquired via WaitForSingleObject.
struct NamedMutexLock {
    wil::unique_mutex_nothrow handle;
    bool owned = false;

    NamedMutexLock() = default;
    NamedMutexLock(const NamedMutexLock&) = delete;
    NamedMutexLock& operator=(const NamedMutexLock&) = delete;
    NamedMutexLock(NamedMutexLock&& o) noexcept
        : handle{std::move(o.handle)}, owned{std::exchange(o.owned, false)} {}
    NamedMutexLock& operator=(NamedMutexLock&& o) noexcept {
        if (this != &o) {
            release();
            handle = std::move(o.handle);
            owned = std::exchange(o.owned, false);
        }
        return *this;
    }
    ~NamedMutexLock() { release(); }

    bool try_lock(const wchar_t* name, DWORD timeout_ms = 0) noexcept {
        release();
        handle.reset(CreateMutexW(nullptr, FALSE, name));
        if (!handle) return false;
        const DWORD w = WaitForSingleObject(handle.get(), timeout_ms);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {
            owned = true;
            return true;
        }
        handle.reset();
        return false;
    }

    void release() noexcept {
        if (handle && owned) {
            ReleaseMutex(handle.get());
            owned = false;
        }
        handle.reset();
    }

    [[nodiscard]] explicit operator bool() const noexcept { return owned; }
};

} // namespace capusy
