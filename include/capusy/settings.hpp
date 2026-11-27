#pragma once

#include "capture_logic.hpp"
#include "common.hpp"
#include "hotkeys.hpp"
#include "startup_logic.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace capusy {

struct Settings {
    // Global hotkeys
    HotkeyBinding hk_full = default_hotkey(HotkeyAction::FullScreenshot);
    HotkeyBinding hk_region = default_hotkey(HotkeyAction::RegionScreenshot);
    HotkeyBinding hk_record = default_hotkey(HotkeyAction::RegionRecord);
    HotkeyBinding hk_history = default_hotkey(HotkeyAction::OpenHistory);

    // Capture behavior & output format
    std::string save_dir;
    bool split_by_month = true;         // Organize into YYYY-MM folders (ShareX-compatible)
    std::string image_format = "png";   // "png", "jpeg", "bmp", "tiff"
    int image_quality = 90;             // 10 .. 100 (applies to JPEG)
    bool auto_copy_clipboard = true;
    bool capture_cursor = false;
    bool region_instant_capture = true; // Capture immediately on click/release instead of opening annotation toolbar

    // Corner preview thumbnail toast (on primary monitor)
    bool show_corner_toast = true;
    float toast_timeout_s = 4.0f;  // 1.0 .. 15.0 seconds
    float toast_width_dip = 260.0f; // 160 .. 420 DIPs

    // Region screen recording (Windows native Media Foundation H.264 + AAC MP4)
    uint32_t record_fps = 30;            // 30 or 60
    uint32_t record_bitrate_mbps = 12;   // 6, 12, or 20
    bool record_cursor = true;
    bool record_desktop_audio = true;
    bool record_mic_audio = false;
    std::string record_desktop_device_id; // Empty = Windows default output device
    std::string record_mic_device_id;     // Empty = Windows default microphone device

    // History window audio state for reused VideoPlayer
    double video_volume = 1.0;
    bool video_muted = false;

    // Updates
    bool auto_check_updates = true;

    [[nodiscard]] ImageFormat resolved_image_format() const noexcept {
        return parse_image_format(image_format);
    }

    [[nodiscard]] HotkeyBinding& hotkey(HotkeyAction a) noexcept {
        switch (a) {
            case HotkeyAction::FullScreenshot: return hk_full;
            case HotkeyAction::RegionScreenshot: return hk_region;
            case HotkeyAction::RegionRecord: return hk_record;
            case HotkeyAction::OpenHistory: return hk_history;
            case HotkeyAction::Count: break;
        }
        return hk_full;
    }

    [[nodiscard]] const HotkeyBinding& hotkey(HotkeyAction a) const noexcept {
        switch (a) {
            case HotkeyAction::FullScreenshot: return hk_full;
            case HotkeyAction::RegionScreenshot: return hk_region;
            case HotkeyAction::RegionRecord: return hk_record;
            case HotkeyAction::OpenHistory: return hk_history;
            case HotkeyAction::Count: break;
        }
        return hk_full;
    }

    [[nodiscard]] std::array<HotkeyBinding, HOTKEY_COUNT> all_hotkeys() const noexcept {
        return {hk_full, hk_region, hk_record, hk_history};
    }
};

inline constexpr auto JSON_READ = glz::opts{.error_on_unknown_keys = false};
inline constexpr auto JSON_WRITE = glz::opts{.prettify = true};

template <typename T>
inline T load_json_file(const fs::path& p, bool* parse_ok = nullptr) {
    T value{};
    if (parse_ok) *parse_ok = true;
    if (auto bytes = ulp::fs::read_as_bytes(p)) {
        thread_local glz::context ctx{};
        const auto err = glz::read<JSON_READ>(value, *bytes, ctx);
        if (err && parse_ok) *parse_ok = false;
    }
    return value;
}

/// Atomically writes `data` to `p` via a per-thread temp file + MoveFileExW, retrying brief Windows sharing violations.
inline bool write_file_atomic(const fs::path& p, std::string_view data) {
    const fs::path tmp = ulp::str::concat_strings(p.wstring(), L".tmp.", to_fixed_wstr(GetCurrentProcessId()), L".", to_fixed_wstr(GetCurrentThreadId()));
    if (!ulp::fs::write(tmp, data)) return false;
    DWORD err = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) return true;
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION && err != ERROR_ACCESS_DENIED) break;
        Sleep(15);
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    SetLastError(err);
    return false;
}

template <typename T>
inline bool save_json_file(const fs::path& p, const T& value) {
    thread_local std::string out;
    thread_local glz::context ctx{};
    out.clear();
    if (glz::write<JSON_WRITE>(value, out, ctx)) return false;
    return write_file_atomic(p, std::string_view{out});
}

inline Settings load_settings(bool* corrupt_recovered = nullptr) {
    std::error_code ec;
    const bool existed = fs::exists(settings_path(), ec);
    bool ok = true;
    auto s = load_json_file<Settings>(settings_path(), &ok);
    const bool corrupted = existed && !ok;
    if (corrupt_recovered) *corrupt_recovered = corrupted;
    if (s.save_dir.empty()) {
        s.save_dir = to_utf8(default_captures_dir().wstring());
    }
    s.image_format = std::string{image_format_key(parse_image_format(s.image_format))};
    s.image_quality = std::clamp(s.image_quality, 10, 100);
    s.toast_timeout_s = std::clamp(s.toast_timeout_s, 1.0f, 15.0f);
    s.toast_width_dip = std::clamp(s.toast_width_dip, 160.0f, 420.0f);
    if (s.record_fps != 30 && s.record_fps != 60) s.record_fps = 30;
    if (s.record_bitrate_mbps < 2 || s.record_bitrate_mbps > 50) s.record_bitrate_mbps = 12;
    if (!existed) save_json_file(settings_path(), s);
    return s;
}

inline bool save_settings(const Settings& s) {
    return save_json_file(settings_path(), s);
}

inline fs::path resolve_save_dir(const Settings& s) {
    fs::path d = s.save_dir.empty() ? default_captures_dir() : fs::path{to_wide(s.save_dir)};
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// ---------------------------------------------------------------------------------------------
// Windows Startup Toggle (identical architecture to usyseek)
// ---------------------------------------------------------------------------------------------

inline constexpr const wchar_t* STARTUP_RUN_SUBKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t* STARTUP_APPROVED_SUBKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
inline constexpr const wchar_t* STARTUP_VALUE_NAME = L"capusy";

/// True when `HKCU\...\CurrentVersion\Run\capusy` exists, points to this installation's
/// `capusy.exe` on disk, and is not disabled under `Explorer\StartupApproved\Run`.
inline bool is_startup_enabled() noexcept {
    try {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_RUN_SUBKEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return false;
        }
        std::array<wchar_t, MAX_PATH * 2> stack_buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
        auto sz = static_cast<DWORD>(stack_buf.size() * sizeof(wchar_t));
        DWORD type = 0;
        LSTATUS st = RegQueryValueExW(key, STARTUP_VALUE_NAME, nullptr, &type, reinterpret_cast<LPBYTE>(stack_buf.data()), &sz);
        std::wstring dyn_buf;
        std::wstring_view raw;
        if (st == ERROR_MORE_DATA && (type == REG_SZ || type == REG_EXPAND_SZ) && sz >= sizeof(wchar_t)) {
            dyn_buf.resize((sz + sizeof(wchar_t) - 1) / sizeof(wchar_t));
            st = RegQueryValueExW(key, STARTUP_VALUE_NAME, nullptr, &type, reinterpret_cast<LPBYTE>(dyn_buf.data()), &sz);
            if (st == ERROR_SUCCESS) raw = std::wstring_view{dyn_buf.data(), sz / sizeof(wchar_t)};
        } else if (st == ERROR_SUCCESS) {
            raw = std::wstring_view{stack_buf.data(), sz / sizeof(wchar_t)};
        }
        RegCloseKey(key);
        if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || raw.empty()) return false;

        std::wstring expanded;
        std::wstring_view cmd = raw;
        if (type == REG_EXPAND_SZ) {
            std::wstring nul_terminated;
            if (const size_t nul = raw.find(L'\0'); nul != std::wstring_view::npos) {
                nul_terminated.assign(raw.substr(0, nul));
            } else {
                nul_terminated.assign(raw);
            }
            const DWORD need = ExpandEnvironmentStringsW(nul_terminated.c_str(), nullptr, 0);
            if (need == 0) return false;
            expanded.resize(need);
            const DWORD written = ExpandEnvironmentStringsW(nul_terminated.c_str(), expanded.data(), need);
            if (written == 0 || written > need) return false;
            cmd = std::wstring_view{expanded.data(), written};
        }

        const StartupCommandCandidates cands = extract_startup_command_exe(cmd);
        if (cands.primary.empty()) return false;

        const fs::path expected = exe_dir() / L"capusy.exe";
        const auto matches_expected = [&](std::wstring_view candidate) -> bool {
            if (candidate.empty()) return false;
            std::error_code ec;
            const fs::path p{candidate};
            if (!fs::is_regular_file(p, ec) || ec) return false;
            return fs::equivalent(p, expected, ec) && !ec;
        };
        if (!matches_expected(cands.primary) && !matches_expected(cands.fallback)) return false;

        HKEY approved = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_APPROVED_SUBKEY, 0, KEY_READ, &approved) == ERROR_SUCCESS) {
            std::array<uint8_t, 32> stack_bin; // NOLINT(cppcoreguidelines-pro-type-member-init)
            auto bsz = static_cast<DWORD>(stack_bin.size());
            DWORD btype = 0;
            LSTATUS bst = RegQueryValueExW(approved, STARTUP_VALUE_NAME, nullptr, &btype, stack_bin.data(), &bsz);
            std::vector<uint8_t> dyn_bin;
            std::span<const uint8_t> bin;
            if (bst == ERROR_MORE_DATA && btype == REG_BINARY && bsz > 0) {
                dyn_bin.resize(bsz);
                bst = RegQueryValueExW(approved, STARTUP_VALUE_NAME, nullptr, &btype, dyn_bin.data(), &bsz);
                if (bst == ERROR_SUCCESS) bin = std::span<const uint8_t>{dyn_bin.data(), std::min<size_t>(bsz, dyn_bin.size())};
            } else if (bst == ERROR_SUCCESS && btype == REG_BINARY && bsz > 0) {
                bin = std::span<const uint8_t>{stack_bin.data(), std::min<size_t>(bsz, stack_bin.size())};
            }
            RegCloseKey(approved);
            if (bst == ERROR_SUCCESS && btype == REG_BINARY && !is_startup_approved_bytes(bin)) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline bool set_startup_enabled(bool enable) {
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, STARTUP_RUN_SUBKEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(st));
        return false;
    }
    if (enable) {
        const fs::path exe = exe_dir() / L"capusy.exe";
        const std::wstring cmd = ulp::str::concat_strings(L"\"", exe.wstring(), L"\"");
        st = RegSetValueExW(key, STARTUP_VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, STARTUP_VALUE_NAME);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(st));
        return false;
    }

    HKEY approved = nullptr;
    const LSTATUS open_st = RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_APPROVED_SUBKEY, 0, KEY_SET_VALUE, &approved);
    if (open_st == ERROR_SUCCESS) {
        LSTATUS del_st = RegDeleteValueW(approved, STARTUP_VALUE_NAME);
        RegCloseKey(approved);
        if (del_st == ERROR_FILE_NOT_FOUND) del_st = ERROR_SUCCESS;
        if (del_st != ERROR_SUCCESS) {
            SetLastError(static_cast<DWORD>(del_st));
            return false;
        }
    } else if (open_st != ERROR_FILE_NOT_FOUND) {
        SetLastError(static_cast<DWORD>(open_st));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Windows Built-in Print Screen Binding Toggle (HKCU\Control Panel\Keyboard)
// ---------------------------------------------------------------------------------------------

inline constexpr const wchar_t* KEYBOARD_CONTROL_PANEL_SUBKEY = L"Control Panel\\Keyboard";
inline constexpr const wchar_t* PRINTSCREEN_SNIPPING_VALUE_NAME = L"PrintScreenKeyForSnippingEnabled";

/// True when `HKCU\Control Panel\Keyboard\PrintScreenKeyForSnippingEnabled` is explicitly set to `0`,
/// disabling Windows' built-in Snipping Tool binding for the Print Screen key.
inline bool is_windows_printscreen_snipping_disabled() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KEYBOARD_CONTROL_PANEL_SUBKEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD val = 1;
    DWORD type = 0;
    DWORD sz = sizeof(val);
    const LSTATUS st = RegQueryValueExW(key, PRINTSCREEN_SNIPPING_VALUE_NAME, nullptr, &type,
                                        reinterpret_cast<LPBYTE>(&val), &sz);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_DWORD || sz != sizeof(val)) {
        return is_windows_printscreen_disabled_value(std::nullopt);
    }
    return is_windows_printscreen_disabled_value(static_cast<uint32_t>(val));
}

/// Writes `PrintScreenKeyForSnippingEnabled` (`0` to disable Windows' built-in Print Screen binding,
/// `1` to re-enable it) and broadcasts `WM_SETTINGCHANGE` so shell listeners can react.
inline bool set_windows_printscreen_snipping_disabled(bool disable) noexcept {
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, KEYBOARD_CONTROL_PANEL_SUBKEY, 0, nullptr, 0,
                                 KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(st));
        return false;
    }
    const DWORD val = disable ? 0u : 1u;
    st = RegSetValueExW(key, PRINTSCREEN_SNIPPING_VALUE_NAME, 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&val), sizeof(val));
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(st));
        return false;
    }
    DWORD_PTR ignored = 0;
    (void)SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                              reinterpret_cast<LPARAM>(KEYBOARD_CONTROL_PANEL_SUBKEY),
                              SMTO_ABORTIFHUNG, 500, &ignored);
    return true;
}

} // namespace capusy
