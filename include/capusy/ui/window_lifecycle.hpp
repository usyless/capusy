#pragma once

#include <cstdint>
#include <optional>

namespace capusy::ui {

inline constexpr uint32_t DEFAULT_CARET_BLINK_MS = 530u;
inline constexpr uint32_t CARET_BLINK_INFINITE = 0xFFFFFFFFu;

[[nodiscard]] constexpr std::optional<uint32_t> caret_blink_interval_ms(uint32_t raw_blink_ms) noexcept {
    if (raw_blink_ms == CARET_BLINK_INFINITE) return std::nullopt;
    if (raw_blink_ms == 0u) return DEFAULT_CARET_BLINK_MS;
    return raw_blink_ms;
}

[[nodiscard]] constexpr bool wm_activate_is_active(uint16_t state, bool minimized) noexcept {
    return state != 0u && !minimized;
}

[[nodiscard]] constexpr bool window_can_render_live(bool window_visible, bool minimized, bool fading_out = false) noexcept {
    return window_visible && !minimized && !fading_out;
}

} // namespace capusy::ui
