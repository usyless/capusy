#pragma once

#include "string_helpers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace capusy {

struct Rgb8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    constexpr bool operator==(const Rgb8&) const noexcept = default;
};

struct Hsv {
    float h = 0.0f; // [0, 360)
    float s = 0.0f; // [0, 1]
    float v = 0.0f; // [0, 1]
};

[[nodiscard]] constexpr uint32_t rgb_to_u32(Rgb8 c) noexcept {
    return (static_cast<uint32_t>(c.r) << 16u) |
           (static_cast<uint32_t>(c.g) << 8u) |
           static_cast<uint32_t>(c.b);
}

[[nodiscard]] constexpr Rgb8 u32_to_rgb(uint32_t rgb) noexcept {
    return {
        .r = static_cast<uint8_t>((rgb >> 16u) & 0xFFu),
        .g = static_cast<uint8_t>((rgb >> 8u) & 0xFFu),
        .b = static_cast<uint8_t>(rgb & 0xFFu),
    };
}

[[nodiscard]] constexpr bool is_light_rgb(Rgb8 c) noexcept {
    const uint32_t luma = (static_cast<uint32_t>(c.r) * 299u) +
                          (static_cast<uint32_t>(c.g) * 587u) +
                          (static_cast<uint32_t>(c.b) * 114u);
    return luma >= 145000u;
}

[[nodiscard]] inline Rgb8 hsv_to_rgb(float h, float s, float v) noexcept {
    const float cs = std::clamp(s, 0.0f, 1.0f);
    const float cv = std::clamp(v, 0.0f, 1.0f);
    float ch = std::fmod(h, 360.0f);
    if (ch < 0.0f) ch += 360.0f;

    const float c = cv * cs;
    const float hp = ch / 60.0f;
    const float x = c * (1.0f - std::abs(std::fmod(hp, 2.0f) - 1.0f));
    const float m = cv - c;

    float rf = 0.0f;
    float gf = 0.0f;
    float bf = 0.0f;
    if (hp < 1.0f)      { rf = c; gf = x; bf = 0.0f; }
    else if (hp < 2.0f) { rf = x; gf = c; bf = 0.0f; }
    else if (hp < 3.0f) { rf = 0.0f; gf = c; bf = x; }
    else if (hp < 4.0f) { rf = 0.0f; gf = x; bf = c; }
    else if (hp < 5.0f) { rf = x; gf = 0.0f; bf = c; }
    else                { rf = c; gf = 0.0f; bf = x; }

    const auto to_u8 = [](float f) noexcept -> uint8_t {
        return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(f * 255.0f)), 0, 255));
    };
    return {.r = to_u8(rf + m), .g = to_u8(gf + m), .b = to_u8(bf + m)};
}

[[nodiscard]] inline Hsv rgb_to_hsv(Rgb8 c) noexcept {
    const float r = static_cast<float>(c.r) / 255.0f;
    const float g = static_cast<float>(c.g) / 255.0f;
    const float b = static_cast<float>(c.b) / 255.0f;
    const float mx = std::max({r, g, b});
    const float mn = std::min({r, g, b});
    const float d = mx - mn;

    Hsv out{.h = 0.0f, .s = (mx > 0.0f) ? (d / mx) : 0.0f, .v = mx};
    if (d > 1e-6f) {
        if (mx == r) {
            out.h = 60.0f * std::fmod(((g - b) / d), 6.0f);
        } else if (mx == g) {
            out.h = 60.0f * (((b - r) / d) + 2.0f);
        } else {
            out.h = 60.0f * (((r - g) / d) + 4.0f);
        }
        if (out.h < 0.0f) out.h += 360.0f;
    }
    return out;
}

[[nodiscard]] inline FixedWString<16> format_hex_rgb(Rgb8 c) noexcept {
    static constexpr std::array<wchar_t, 16> digits = {
        L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7',
        L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F',
    };
    FixedWString<16> out;
    out.push_back(L'#');
    out.push_back(digits[(static_cast<unsigned>(c.r) >> 4u) & 0xFu]);
    out.push_back(digits[c.r & 0xFu]);
    out.push_back(digits[(static_cast<unsigned>(c.g) >> 4u) & 0xFu]);
    out.push_back(digits[c.g & 0xFu]);
    out.push_back(digits[(static_cast<unsigned>(c.b) >> 4u) & 0xFu]);
    out.push_back(digits[c.b & 0xFu]);
    return out;
}

[[nodiscard]] inline FixedWString<32> format_rgb_label(Rgb8 c) noexcept {
    FixedWString<32> out{L"RGB("};
    out.append(to_fixed_wstr<8>(static_cast<unsigned>(c.r)));
    out.append(L", ");
    out.append(to_fixed_wstr<8>(static_cast<unsigned>(c.g)));
    out.append(L", ");
    out.append(to_fixed_wstr<8>(static_cast<unsigned>(c.b)));
    out.push_back(L')');
    return out;
}

/// Samples a single pixel from a top-down 32bpp BGRA pixel buffer (returns `std::nullopt` if out of bounds).
[[nodiscard]] inline std::optional<Rgb8> sample_bgra_pixel(std::span<const uint8_t> pixels, uint32_t w, uint32_t h, int x, int y) noexcept {
    if (w == 0 || h == 0 || x < 0 || y < 0) return std::nullopt;
    const auto ux = static_cast<uint32_t>(x);
    const auto uy = static_cast<uint32_t>(y);
    if (ux >= w || uy >= h) return std::nullopt;
    const size_t idx = ((static_cast<size_t>(uy) * static_cast<size_t>(w)) + static_cast<size_t>(ux)) * 4u;
    if (idx + 3u >= pixels.size()) return std::nullopt;
    return Rgb8{.r = pixels[idx + 2u], .g = pixels[idx + 1u], .b = pixels[idx + 0u]};
}

} // namespace capusy
