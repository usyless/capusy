#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace capusy::ui {

[[nodiscard]] inline D2D1_COLOR_F hex(uint32_t rgb, float a = 1.0f) noexcept {
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16u) & 0xFFu) / 255.0f,
        static_cast<float>((rgb >> 8u) & 0xFFu) / 255.0f,
        static_cast<float>(rgb & 0xFFu) / 255.0f,
        a);
}

[[nodiscard]] inline D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) noexcept {
    c.a = a;
    return c;
}

[[nodiscard]] inline D2D1_COLOR_F lerp_color(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) noexcept {
    const float k = std::clamp(t, 0.0f, 1.0f);
    return {
        .r = a.r + ((b.r - a.r) * k),
        .g = a.g + ((b.g - a.g) * k),
        .b = a.b + ((b.b - a.b) * k),
        .a = a.a + ((b.a - a.a) * k),
    };
}

/// Single default Midnight Dark theme matching usyseek's dark visual tokens.
struct Theme {
    D2D1_COLOR_F bg = hex(0x141218);
    D2D1_COLOR_F surface_lowest = hex(0x0f0d13);
    D2D1_COLOR_F surface_low = hex(0x1d1b20);
    D2D1_COLOR_F surface = hex(0x211f26);
    D2D1_COLOR_F surface_high = hex(0x2b2930);
    D2D1_COLOR_F surface_highest = hex(0x36343b);
    D2D1_COLOR_F on_surface = hex(0xe6e1e5);
    D2D1_COLOR_F on_surface_variant = hex(0xcac4d0);
    D2D1_COLOR_F outline = hex(0x938f99);
    D2D1_COLOR_F outline_variant = hex(0x444148);
    D2D1_COLOR_F primary = hex(0xa8c7fa);
    D2D1_COLOR_F primary_hover = hex(0xb8d4ff);
    D2D1_COLOR_F on_primary = hex(0x062e6f);
    D2D1_COLOR_F primary_container = hex(0x0842a0);
    D2D1_COLOR_F on_primary_container = hex(0xd3e3fd);
    D2D1_COLOR_F secondary_container = hex(0x004d67);
    D2D1_COLOR_F on_secondary_container = hex(0xc2e7ff);
    D2D1_COLOR_F error = hex(0xf2b8b5);
    D2D1_COLOR_F error_container = hex(0x8c1d18);
    D2D1_COLOR_F mint = hex(0x70d6b6);
    D2D1_COLOR_F amber = hex(0xffb951);
    D2D1_COLOR_F scrim = hex(0x000000, 0.55f);

    float window_bg_alpha = 0.90f;
    float card_alpha = 0.92f;
    float card_radius = 14.0f;
    float button_radius = 10.0f;
    float thumb_radius = 10.0f;
};

[[nodiscard]] inline const Theme& default_theme() noexcept {
    static const Theme t{};
    return t;
}

[[nodiscard]] inline float offset_radius(float r, float delta) noexcept {
    return r <= 0.0f ? 0.0f : std::max(0.0f, r + delta);
}

} // namespace capusy::ui
