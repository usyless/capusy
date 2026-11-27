#pragma once

#include <usylibpp/strings.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace capusy {

enum class HotkeyAction : size_t {
    FullScreenshot = 0,
    RegionScreenshot = 1,
    RegionRecord = 2,
    OpenHistory = 3,
    Count = 4,
};

inline constexpr size_t HOTKEY_COUNT = static_cast<size_t>(HotkeyAction::Count);

struct HotkeyBinding {
    bool enabled = true;
    uint32_t mods = 0;
    uint32_t vk = 0;

    constexpr bool operator==(const HotkeyBinding&) const noexcept = default;
};

enum class HotkeyState : uint8_t {
    Disabled = 0,
    Registered,
    RegisteredHook,   // intercepted via PrintScreen hook fallback while Windows holds VK_SNAPSHOT
    Unavailable,      // another application owns the combination
    ConflictInternal, // collides with another enabled capusy hotkey
};

struct HotkeyStatus {
    HotkeyState state = HotkeyState::Disabled;
    std::optional<HotkeyAction> conflict_with;
};

inline constexpr uint32_t VK_PRINTSCREEN_CODE = 0x2Cu;

[[nodiscard]] constexpr bool is_hotkey_active(HotkeyState s) noexcept {
    return s == HotkeyState::Registered || s == HotkeyState::RegisteredHook;
}

[[nodiscard]] constexpr const wchar_t* hotkey_action_label(HotkeyAction a) noexcept {
    switch (a) {
        case HotkeyAction::FullScreenshot: return L"Full-Screen Screenshot";
        case HotkeyAction::RegionScreenshot: return L"Region Screenshot";
        case HotkeyAction::RegionRecord: return L"Region Recording (Start / Stop)";
        case HotkeyAction::OpenHistory: return L"Open Screenshot History";
        case HotkeyAction::Count: break;
    }
    return L"Shortcut";
}

[[nodiscard]] constexpr const wchar_t* hotkey_action_caption(HotkeyAction a) noexcept {
    switch (a) {
        case HotkeyAction::FullScreenshot: return L"Captures all monitors immediately (fills gaps with black)";
        case HotkeyAction::RegionScreenshot: return L"Freezes all monitors, snaps to windows & elements, with drawing tools";
        case HotkeyAction::RegionRecord: return L"Select a region to record MP4 video; press again to stop";
        case HotkeyAction::OpenHistory: return L"Shows the screenshot history and preview window";
        case HotkeyAction::Count: break;
    }
    return L"";
}

[[nodiscard]] constexpr HotkeyBinding default_hotkey(HotkeyAction a) noexcept {
#ifdef _WIN32
    constexpr uint32_t kAltShift = MOD_ALT | MOD_SHIFT;
    switch (a) {
        case HotkeyAction::FullScreenshot: return {.enabled = true, .mods = kAltShift, .vk = '1'};
        case HotkeyAction::RegionScreenshot: return {.enabled = true, .mods = kAltShift, .vk = '2'};
        case HotkeyAction::RegionRecord: return {.enabled = true, .mods = kAltShift, .vk = '3'};
        case HotkeyAction::OpenHistory: return {.enabled = true, .mods = kAltShift, .vk = 'H'};
        case HotkeyAction::Count: break;
    }
#else
    (void)a;
#endif
    return {};
}

/// Keys that are sensible as standalone global hotkeys without requiring Ctrl/Alt/Shift/Win.
[[nodiscard]] constexpr bool is_standalone_hotkey_vk(uint32_t vk) noexcept {
    return vk == VK_PRINTSCREEN_CODE /* VK_SNAPSHOT / PrintScreen */ ||
           (vk >= 0x70 && vk <= 0x87) /* VK_F1..VK_F24 */ ||
           vk == 0x13 /* VK_PAUSE */ ||
           vk == 0x91 /* VK_SCROLL */;
}

/// True for generic or left/right modifier virtual-key codes.
[[nodiscard]] constexpr bool is_modifier_vk(uint32_t vk) noexcept {
    return vk == 0x10 /* VK_SHIFT */ || vk == 0x11 /* VK_CONTROL */ || vk == 0x12 /* VK_MENU */ ||
           vk == 0x5B /* VK_LWIN */ || vk == 0x5C /* VK_RWIN */ ||
           vk == 0xA0 /* VK_LSHIFT */ || vk == 0xA1 /* VK_RSHIFT */ ||
           vk == 0xA2 /* VK_LCONTROL */ || vk == 0xA3 /* VK_RCONTROL */ ||
           vk == 0xA4 /* VK_LMENU */ || vk == 0xA5 /* VK_RMENU */;
}

/// Normalises modifier flags to the four Win32 hotkey modifier bits (MOD_ALT|MOD_CONTROL|MOD_SHIFT|MOD_WIN).
[[nodiscard]] constexpr uint32_t normalize_hotkey_mods(uint32_t mods) noexcept {
    return mods & 0x0Fu;
}

/// Resolves modifier bits for a recorded keybind: preserves standalone keys (like PrintScreen and F1-F24)
/// with zero modifiers, while defaulting bare non-standalone keys to Alt.
[[nodiscard]] constexpr uint32_t resolve_recorded_hotkey_mods(uint32_t mods, uint32_t vk) noexcept {
    const uint32_t nm = normalize_hotkey_mods(mods);
    if (nm == 0u && !is_standalone_hotkey_vk(vk)) return 0x0001u /* MOD_ALT */;
    return nm;
}

[[nodiscard]] constexpr bool is_printscreen_hotkey(const HotkeyBinding& hb) noexcept {
    return hb.enabled && hb.vk == VK_PRINTSCREEN_CODE;
}

[[nodiscard]] constexpr bool any_printscreen_hotkey(std::span<const HotkeyBinding> bindings) noexcept {
    for (const HotkeyBinding& hb : bindings) {
        if (is_printscreen_hotkey(hb)) return true;
    }
    return false;
}

/// True for PrintScreen bindings that Windows itself reserves by default (bare PrintScreen for
/// Snipping Tool when enabled, or Win+PrintScreen for Windows screenshot saving).
[[nodiscard]] constexpr bool is_windows_reserved_printscreen(uint32_t mods, uint32_t vk) noexcept {
    if (vk != VK_PRINTSCREEN_CODE) return false;
    const uint32_t nm = normalize_hotkey_mods(mods);
    return nm == 0u || (nm & 0x08u /* MOD_WIN */) != 0u;
}

/// Evaluates `HKCU\Control Panel\Keyboard\PrintScreenKeyForSnippingEnabled`.
/// Windows stores `0` when the built-in Snipping Tool binding is disabled and `1` when enabled.
/// When absent (`std::nullopt`), returns `false` so the user can explicitly write `0` to disable it.
[[nodiscard]] constexpr bool is_windows_printscreen_disabled_value(std::optional<uint32_t> reg_dword) noexcept {
    return reg_dword.has_value() && *reg_dword == 0u;
}

/// True when a PrintScreen hotkey that failed `RegisterHotKey` with `ERROR_HOTKEY_ALREADY_REGISTERED`
/// should fall back to capusy's targeted PrintScreen keyboard hook (only when the user has disabled
/// Windows' built-in Print Screen binding).
[[nodiscard]] constexpr bool should_use_printscreen_hook_fallback(const HotkeyBinding& hb, bool win_prtscn_disabled) noexcept {
    return win_prtscn_disabled && is_printscreen_hotkey(hb);
}

/// Returns the first earlier or peer action index whose enabled binding collides with `idx`, or `std::nullopt`.
[[nodiscard]] constexpr std::optional<HotkeyAction> find_internal_hotkey_conflict(
    std::span<const HotkeyBinding> bindings, size_t idx) noexcept {
    if (idx >= bindings.size()) return std::nullopt;
    const HotkeyBinding& target = bindings[idx];
    if (!target.enabled || target.vk == 0) return std::nullopt;
    const uint32_t target_mods = normalize_hotkey_mods(target.mods);
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (i == idx) continue;
        const HotkeyBinding& other = bindings[i];
        if (!other.enabled || other.vk == 0) continue;
        if (other.vk == target.vk && normalize_hotkey_mods(other.mods) == target_mods) {
            return static_cast<HotkeyAction>(i);
        }
    }
    return std::nullopt;
}

/// Checks whether a candidate `(mods, vk)` would collide with any OTHER enabled binding in `bindings`.
[[nodiscard]] constexpr std::optional<HotkeyAction> check_candidate_hotkey_conflict(
    std::span<const HotkeyBinding> bindings, size_t target_idx, uint32_t mods, uint32_t vk) noexcept {
    if (vk == 0) return std::nullopt;
    const uint32_t nm = normalize_hotkey_mods(mods);
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (i == target_idx) continue;
        const HotkeyBinding& other = bindings[i];
        if (!other.enabled || other.vk == 0) continue;
        if (other.vk == vk && normalize_hotkey_mods(other.mods) == nm) {
            return static_cast<HotkeyAction>(i);
        }
    }
    return std::nullopt;
}

#ifdef _WIN32
inline usylibpp::strings::FixedWString<96> format_hotkey(uint32_t mods, uint32_t vk, bool compact = false) noexcept {
    usylibpp::strings::FixedWString<96> t;
    const wchar_t* sep = compact ? L"+" : L" + ";
    if (mods & MOD_CONTROL) { t.append(L"Ctrl"); t.append(sep); }
    if (mods & MOD_ALT)     { t.append(L"Alt");  t.append(sep); }
    if (mods & MOD_SHIFT)   { t.append(L"Shift"); t.append(sep); }
    if (mods & MOD_WIN)     { t.append(L"Win");  t.append(sep); }

    if (vk == 0) {
        t.append(L"None");
        return t;
    }
    if (vk == VK_SNAPSHOT) {
        t.append(L"PrintScreen");
        return t;
    }
    if (vk == VK_SPACE) {
        t.append(L"Space");
        return t;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        t.push_back(L'F');
        t.append(usylibpp::strings::to_fixed_wstr<8>(vk - VK_F1 + 1));
        return t;
    }
    if (vk == VK_PAUSE) {
        t.append(L"Pause");
        return t;
    }
    if (vk == VK_INSERT) {
        t.append(L"Insert");
        return t;
    }
    if (vk == VK_DELETE) {
        t.append(L"Delete");
        return t;
    }
    if (vk == VK_HOME) {
        t.append(L"Home");
        return t;
    }
    if (vk == VK_END) {
        t.append(L"End");
        return t;
    }
    if (vk == VK_PRIOR) {
        t.append(L"PgUp");
        return t;
    }
    if (vk == VK_NEXT) {
        t.append(L"PgDn");
        return t;
    }

    std::array<wchar_t, 64> name; // NOLINT(cppcoreguidelines-pro-type-member-init)
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const LONG key_lp = static_cast<LONG>(scan << 16u);
    if (scan == 0 || GetKeyNameTextW(key_lp, name.data(), static_cast<int>(name.size())) == 0) {
        t.append(L"Key ");
        t.append(usylibpp::strings::to_fixed_wstr<16>(vk));
        return t;
    }
    t.append(name.data());
    return t;
}
#endif

} // namespace capusy
