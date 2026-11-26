#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace capusy::ui {

struct FitBox {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

/// Strips extended-length Win32 path prefixes (`\\?\` / `\\?\UNC\`) before passing URLs to Media Foundation.
[[nodiscard]] inline std::wstring normalize_mf_source_path(std::wstring_view path) {
    if (path.starts_with(LR"(\\?\UNC\)")) {
        std::wstring out = LR"(\\)";
        out.append(path.substr(8));
        return out;
    }
    if (path.starts_with(LR"(\\?\)")) {
        return std::wstring{path.substr(4)};
    }
    return std::wstring{path};
}

/// Builds a safe synthetic `file:///media.<ext>` URL hint for `IMFMediaEngineEx::SetSourceFromByteStream`.
[[nodiscard]] inline std::wstring make_mf_extension_hint_url(std::wstring_view path) {
    std::wstring out = L"file:///media";
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring_view::npos && (slash == std::wstring_view::npos || dot > slash)) {
        const std::wstring_view ext = path.substr(dot + 1);
        if (!ext.empty() && ext.size() <= 16u) {
            bool ascii_alnum = true;
            for (const wchar_t c : ext) {
                const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
                if (!ok) {
                    ascii_alnum = false;
                    break;
                }
            }
            if (ascii_alnum) {
                out.push_back(L'.');
                for (const wchar_t c : ext) {
                    out.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c);
                }
            }
        }
    }
    return out;
}

/// Scales `(w, h)` proportionally so `max(w, h) <= max_px` without ever upscaling (`max_px == 0` keeps native size).
[[nodiscard]] inline std::pair<uint32_t, uint32_t> fit_scaled_size(uint32_t w, uint32_t h, uint32_t max_px) noexcept {
    if (w == 0 || h == 0) return {0u, 0u};
    const uint32_t longest = std::max(w, h);
    if (max_px == 0 || longest <= max_px) return {w, h};
    const double s = static_cast<double>(max_px) / static_cast<double>(longest);
    const uint32_t sw = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(w) * s)));
    const uint32_t sh = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(h) * s)));
    return {sw, sh};
}

/// Computes the centered preview rectangle in DIPs for media `(src_w, src_h)` fitted into `area`.
[[nodiscard]] inline FitBox compute_preview_box(
    uint32_t src_w, uint32_t src_h, uint32_t meta_w, uint32_t meta_h, float dpi_scale, FitBox area) noexcept {
    const float safe_scale = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    const float bw = static_cast<float>(std::max(1u, src_w));
    const float bh = static_cast<float>(std::max(1u, src_h));
    const float native = (meta_w > 0 && meta_h > 0) ? static_cast<float>(std::max(meta_w, meta_h)) : std::max(bw, bh);
    const float k = std::max({native, bw, bh}) / std::max(bw, bh) / safe_scale;
    const float nw = bw * k;
    const float nh = bh * k;
    const float avail_w = std::max(1.0f, area.width);
    const float avail_h = std::max(1.0f, area.height);
    const float s = std::min({1.0f, avail_w / nw, avail_h / nh});
    const float w = std::max(1.0f, std::round(nw * s * safe_scale)) / safe_scale;
    const float h = std::max(1.0f, std::round(nh * s * safe_scale)) / safe_scale;
    const float x = std::round((area.left + ((avail_w - w) * 0.5f)) * safe_scale) / safe_scale;
    const float y = std::round((area.top + ((avail_h - h) * 0.5f)) * safe_scale) / safe_scale;
    return FitBox{.left = x, .top = y, .width = w, .height = h};
}

/// Throttled seek state machine for Media Foundation video playback.
class SeekState {
public:
    static constexpr int64_t TIMEOUT_MS = 180;

    void reset() noexcept {
        in_flight_ = false;
        show_target_ = false;
        pending_.reset();
        started_ms_ = 0;
        target_s_ = 0.0;
    }

    [[nodiscard]] std::optional<double> request(double target_s, double duration_s, int64_t now_ms) noexcept {
        if (duration_s <= 0.0 || !std::isfinite(target_s)) return std::nullopt;
        const double clamped = std::clamp(target_s, 0.0, duration_s);
        target_s_ = clamped;
        show_target_ = true;

        if (in_flight_ && (now_ms - started_ms_ < TIMEOUT_MS)) {
            pending_ = clamped;
            return std::nullopt;
        }
        in_flight_ = true;
        started_ms_ = now_ms;
        pending_.reset();
        return clamped;
    }

    void cancel_current() noexcept {
        in_flight_ = false;
        show_target_ = false;
        pending_.reset();
    }

    [[nodiscard]] std::optional<double> on_seeked(int64_t now_ms) noexcept {
        in_flight_ = false;
        if (pending_) {
            const double next = *pending_;
            pending_.reset();
            in_flight_ = true;
            started_ms_ = now_ms;
            target_s_ = next;
            show_target_ = true;
            return next;
        }
        return std::nullopt;
    }

    void on_frame_presented(bool playing) noexcept {
        if (!in_flight_ && !pending_ && playing) {
            show_target_ = false;
        }
    }

    void settle_paused_display() noexcept {
        if (!in_flight_ && !pending_) {
            show_target_ = false;
        }
    }

    [[nodiscard]] double displayed_time(double engine_time_s, bool playing, double duration_s) const noexcept {
        if (show_target_ && (!playing || in_flight_ || pending_.has_value())) {
            return duration_s > 0.0 ? std::clamp(target_s_, 0.0, duration_s) : 0.0;
        }
        if (!std::isfinite(engine_time_s) || engine_time_s < 0.0) return 0.0;
        return duration_s > 0.0 ? std::clamp(engine_time_s, 0.0, duration_s) : engine_time_s;
    }

    [[nodiscard]] bool in_flight() const noexcept { return in_flight_; }
    [[nodiscard]] bool active() const noexcept { return in_flight_ || pending_.has_value(); }
    [[nodiscard]] int64_t started_ms() const noexcept { return started_ms_; }

private:
    bool in_flight_ = false;
    bool show_target_ = false;
    std::optional<double> pending_;
    int64_t started_ms_ = 0;
    double target_s_ = 0.0;
};

} // namespace capusy::ui
