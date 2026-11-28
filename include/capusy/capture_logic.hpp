#pragma once

#include "string_helpers.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace capusy {

enum class ImageFormat : uint8_t {
    Png = 0,
    Jpeg = 1,
    Bmp = 2,
    Tiff = 3,
};

[[nodiscard]] inline ImageFormat parse_image_format(std::string_view s) noexcept {
    if (s == "jpeg" || s == "jpg" || s == "JPEG" || s == "JPG") return ImageFormat::Jpeg;
    if (s == "bmp" || s == "BMP") return ImageFormat::Bmp;
    if (s == "tiff" || s == "tif" || s == "TIFF" || s == "TIF") return ImageFormat::Tiff;
    return ImageFormat::Png;
}

[[nodiscard]] inline std::string_view image_format_key(ImageFormat fmt) noexcept {
    switch (fmt) {
        case ImageFormat::Png: return "png";
        case ImageFormat::Jpeg: return "jpeg";
        case ImageFormat::Bmp: return "bmp";
        case ImageFormat::Tiff: return "tiff";
    }
    return "png";
}

[[nodiscard]] inline std::wstring_view image_format_ext(ImageFormat fmt) noexcept {
    switch (fmt) {
        case ImageFormat::Png: return L".png";
        case ImageFormat::Jpeg: return L".jpg";
        case ImageFormat::Bmp: return L".bmp";
        case ImageFormat::Tiff: return L".tiff";
    }
    return L".png";
}

[[nodiscard]] inline std::string_view image_format_label_utf8(ImageFormat fmt) noexcept {
    switch (fmt) {
        case ImageFormat::Png: return "PNG";
        case ImageFormat::Jpeg: return "JPEG";
        case ImageFormat::Bmp: return "BMP";
        case ImageFormat::Tiff: return "TIFF";
    }
    return "PNG";
}

[[nodiscard]] inline std::wstring_view image_format_label(ImageFormat fmt) noexcept {
    switch (fmt) {
        case ImageFormat::Png: return L"PNG";
        case ImageFormat::Jpeg: return L"JPEG";
        case ImageFormat::Bmp: return L"BMP";
        case ImageFormat::Tiff: return L"TIFF";
    }
    return L"PNG";
}

[[nodiscard]] inline bool image_format_supports_quality(ImageFormat fmt) noexcept {
    return fmt == ImageFormat::Jpeg;
}

struct CaptureDateTime {
    uint16_t year = 0;
    uint16_t month = 0;
    uint16_t day = 0;
    uint16_t hour = 0;
    uint16_t minute = 0;
    uint16_t second = 0;
    uint16_t millisecond = 0;
};

/// Formats a ShareX-compatible `YYYY-MM` subfolder name.
[[nodiscard]] inline FixedWString<8> format_year_month(uint16_t year, uint16_t month) noexcept {
    FixedWString<8> ym;
    ym.append(to_fixed_wstr<8>(year));
    ym.push_back(L'-');
    ym.push_back(static_cast<wchar_t>(L'0' + ((month / 10) % 10)));
    ym.push_back(static_cast<wchar_t>(L'0' + (month % 10)));
    return ym;
}

/// Pure folder builder taking explicit local date/time: returns `dir / YYYY-MM` when `split_by_month` is enabled, otherwise `dir`.
[[nodiscard]] inline std::filesystem::path resolve_active_capture_dir_for_time(
    const std::filesystem::path& dir, const CaptureDateTime& dt, bool split_by_month) {
    if (split_by_month) {
        const FixedWString<8> ym = format_year_month(dt.year, dt.month);
        return dir / std::wstring_view{ym};
    }
    return dir;
}

/// Extracts a safe filename prefix from a window executable/process name (e.g. "Code.exe" -> "Code").
/// Strips any directory prefix, trailing ".exe" (case-insensitive), trims whitespace/dots,
/// replaces invalid Windows filename characters with '_', and falls back to "capusy" when empty.
[[nodiscard]] inline FixedWString<64> sanitize_capture_prefix(std::wstring_view raw) noexcept {
    if (const size_t slash = raw.find_last_of(L"\\/"); slash != std::wstring_view::npos) {
        raw.remove_prefix(slash + 1);
    }

    const auto is_ws = [](wchar_t c) noexcept {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };
    while (!raw.empty() && is_ws(raw.front())) raw.remove_prefix(1);
    while (!raw.empty() && is_ws(raw.back())) raw.remove_suffix(1);

    if (raw.size() >= 4) {
        const std::wstring_view tail = raw.substr(raw.size() - 4);
        const auto ascii_low = [](wchar_t c) noexcept -> wchar_t {
            return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
        };
        if (tail[0] == L'.' && ascii_low(tail[1]) == L'e' && ascii_low(tail[2]) == L'x' && ascii_low(tail[3]) == L'e') {
            raw.remove_suffix(4);
        }
    }

    while (!raw.empty() && (is_ws(raw.front()) || raw.front() == L'.')) raw.remove_prefix(1);
    while (!raw.empty() && (is_ws(raw.back()) || raw.back() == L'.')) raw.remove_suffix(1);

    if (raw.size() > FixedWString<64>::capacity()) {
        raw = raw.substr(0, FixedWString<64>::capacity());
        while (!raw.empty() && (is_ws(raw.back()) || raw.back() == L'.')) raw.remove_suffix(1);
    }

    FixedWString<64> out;
    bool has_valid_char = false;
    for (const wchar_t raw_c : raw) {
        if (raw_c < 0x20 || raw_c == L'<' || raw_c == L'>' || raw_c == L':' || raw_c == L'"' ||
            raw_c == L'/' || raw_c == L'\\' || raw_c == L'|' || raw_c == L'?' || raw_c == L'*') {
            out.push_back(L'_');
        } else {
            out.push_back(raw_c);
            has_valid_char = true;
        }
    }

    if (!has_valid_char || out.empty()) {
        out.assign(L"capusy");
    }
    return out;
}

/// Pure path builder taking explicit local date/time & sequence number (for deterministic testing).
[[nodiscard]] inline std::filesystem::path make_capture_path_for_time(const std::filesystem::path& dir, std::wstring_view ext,
                                                                      const CaptureDateTime& dt, uint64_t seq, bool split_by_month = true,
                                                                      std::wstring_view prefix = {}) {
    const FixedWString<64> clean_prefix = sanitize_capture_prefix(prefix);
    FixedWString<160> name;
    name.append(clean_prefix.view());
    name.push_back(L'_');
    const auto push2 = [&](uint16_t v) noexcept {
        name.push_back(static_cast<wchar_t>(L'0' + ((v / 10) % 10)));
        name.push_back(static_cast<wchar_t>(L'0' + (v % 10)));
    };
    const auto push3 = [&](uint16_t v) noexcept {
        name.push_back(static_cast<wchar_t>(L'0' + ((v / 100) % 10)));
        name.push_back(static_cast<wchar_t>(L'0' + ((v / 10) % 10)));
        name.push_back(static_cast<wchar_t>(L'0' + (v % 10)));
    };
    name.append(to_fixed_wstr<8>(dt.year));
    name.push_back(L'-');
    push2(dt.month);
    name.push_back(L'-');
    push2(dt.day);
    name.push_back(L'_');
    push2(dt.hour);
    name.push_back(L'-');
    push2(dt.minute);
    name.push_back(L'-');
    push2(dt.second);
    name.push_back(L'_');
    push3(dt.millisecond);
    name.push_back(L'_');
    name.append(to_fixed_wstr<16>(seq));
    name.append(ext);

    if (split_by_month) {
        const FixedWString<8> ym = format_year_month(dt.year, dt.month);
        return dir / std::wstring_view{ym} / std::wstring_view{name};
    }
    return dir / std::wstring_view{name};
}

} // namespace capusy
