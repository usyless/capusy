#pragma once

#include "string_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>

namespace capusy {

enum class RecordFormat : uint8_t {
    Mp4 = 0,
    Wmv = 1,
    Gif = 2,
};

enum class VideoCodec : uint8_t {
    H264 = 0,
    Hevc = 1,
};

enum class H264Profile : uint8_t {
    Baseline = 0,
    Main = 1,
    High = 2,
};

enum class RateControlMode : uint8_t {
    Vbr = 0,
    Quality = 1,
    Cbr = 2,
};

inline constexpr uint32_t RECORD_FPS_MIN = 5u;
inline constexpr uint32_t RECORD_FPS_MAX = 60u;
inline constexpr uint32_t RECORD_GIF_FPS_MAX = 30u;
inline constexpr uint32_t RECORD_BITRATE_MIN_MBPS = 1u;
inline constexpr uint32_t RECORD_BITRATE_MAX_MBPS = 50u;
inline constexpr uint32_t RECORD_QUALITY_MIN = 10u;
inline constexpr uint32_t RECORD_QUALITY_MAX = 100u;
inline constexpr uint32_t RECORD_SCALE_MIN_PCT = 25u;
inline constexpr uint32_t RECORD_SCALE_MAX_PCT = 100u;
inline constexpr uint32_t RECORD_KEYFRAME_MIN_S = 1u;
inline constexpr uint32_t RECORD_KEYFRAME_MAX_S = 10u;

[[nodiscard]] inline RecordFormat parse_record_format(std::string_view s) noexcept {
    if (s == "wmv" || s == "WMV" || s == "asf" || s == "ASF") return RecordFormat::Wmv;
    if (s == "gif" || s == "GIF") return RecordFormat::Gif;
    return RecordFormat::Mp4;
}

[[nodiscard]] inline std::string_view record_format_key(RecordFormat fmt) noexcept {
    switch (fmt) {
        case RecordFormat::Mp4: return "mp4";
        case RecordFormat::Wmv: return "wmv";
        case RecordFormat::Gif: return "gif";
    }
    return "mp4";
}

[[nodiscard]] inline std::wstring_view record_format_ext(RecordFormat fmt) noexcept {
    switch (fmt) {
        case RecordFormat::Mp4: return L".mp4";
        case RecordFormat::Wmv: return L".wmv";
        case RecordFormat::Gif: return L".gif";
    }
    return L".mp4";
}

[[nodiscard]] inline std::string_view record_format_label_utf8(RecordFormat fmt) noexcept {
    switch (fmt) {
        case RecordFormat::Mp4: return "MP4";
        case RecordFormat::Wmv: return "WMV";
        case RecordFormat::Gif: return "GIF";
    }
    return "MP4";
}

[[nodiscard]] inline std::wstring_view record_format_label(RecordFormat fmt) noexcept {
    switch (fmt) {
        case RecordFormat::Mp4: return L"MP4";
        case RecordFormat::Wmv: return L"WMV";
        case RecordFormat::Gif: return L"GIF";
    }
    return L"MP4";
}

[[nodiscard]] inline bool record_format_supports_audio(RecordFormat fmt) noexcept {
    return fmt != RecordFormat::Gif;
}

[[nodiscard]] inline bool record_format_uses_mf(RecordFormat fmt) noexcept {
    return fmt != RecordFormat::Gif;
}

[[nodiscard]] inline VideoCodec parse_video_codec(std::string_view s) noexcept {
    if (s == "hevc" || s == "HEVC" || s == "h265" || s == "H265" || s == "h.265" || s == "H.265") {
        return VideoCodec::Hevc;
    }
    return VideoCodec::H264;
}

[[nodiscard]] inline std::string_view video_codec_key(VideoCodec c) noexcept {
    switch (c) {
        case VideoCodec::H264: return "h264";
        case VideoCodec::Hevc: return "hevc";
    }
    return "h264";
}

[[nodiscard]] inline std::wstring_view video_codec_label(VideoCodec c) noexcept {
    switch (c) {
        case VideoCodec::H264: return L"H.264";
        case VideoCodec::Hevc: return L"HEVC (H.265)";
    }
    return L"H.264";
}

[[nodiscard]] inline std::wstring_view video_codec_short_label(VideoCodec c) noexcept {
    switch (c) {
        case VideoCodec::H264: return L"H.264";
        case VideoCodec::Hevc: return L"HEVC";
    }
    return L"H.264";
}

[[nodiscard]] inline H264Profile parse_h264_profile(std::string_view s) noexcept {
    if (s == "baseline" || s == "Baseline" || s == "base") return H264Profile::Baseline;
    if (s == "main" || s == "Main") return H264Profile::Main;
    return H264Profile::High;
}

[[nodiscard]] inline std::string_view h264_profile_key(H264Profile p) noexcept {
    switch (p) {
        case H264Profile::Baseline: return "baseline";
        case H264Profile::Main: return "main";
        case H264Profile::High: return "high";
    }
    return "high";
}

[[nodiscard]] inline std::wstring_view h264_profile_label(H264Profile p) noexcept {
    switch (p) {
        case H264Profile::Baseline: return L"Baseline";
        case H264Profile::Main: return L"Main";
        case H264Profile::High: return L"High";
    }
    return L"High";
}

[[nodiscard]] inline RateControlMode parse_rate_control(std::string_view s) noexcept {
    if (s == "quality" || s == "Quality" || s == "cqp" || s == "CQP") return RateControlMode::Quality;
    if (s == "cbr" || s == "CBR") return RateControlMode::Cbr;
    return RateControlMode::Vbr;
}

[[nodiscard]] inline std::string_view rate_control_key(RateControlMode m) noexcept {
    switch (m) {
        case RateControlMode::Vbr: return "vbr";
        case RateControlMode::Quality: return "quality";
        case RateControlMode::Cbr: return "cbr";
    }
    return "vbr";
}

[[nodiscard]] inline std::wstring_view rate_control_label(RateControlMode m) noexcept {
    switch (m) {
        case RateControlMode::Vbr: return L"VBR";
        case RateControlMode::Quality: return L"Quality";
        case RateControlMode::Cbr: return L"CBR";
    }
    return L"VBR";
}

[[nodiscard]] inline uint32_t clamp_record_fps(uint32_t fps) noexcept {
    return std::clamp(fps, RECORD_FPS_MIN, RECORD_FPS_MAX);
}

[[nodiscard]] inline uint32_t effective_record_fps(uint32_t fps, RecordFormat fmt) noexcept {
    const uint32_t hi = (fmt == RecordFormat::Gif) ? RECORD_GIF_FPS_MAX : RECORD_FPS_MAX;
    return std::clamp(fps, RECORD_FPS_MIN, hi);
}

[[nodiscard]] inline uint32_t clamp_record_bitrate_mbps(uint32_t mbps) noexcept {
    return std::clamp(mbps, RECORD_BITRATE_MIN_MBPS, RECORD_BITRATE_MAX_MBPS);
}

[[nodiscard]] inline uint32_t clamp_record_quality(uint32_t quality) noexcept {
    return std::clamp(quality, RECORD_QUALITY_MIN, RECORD_QUALITY_MAX);
}

[[nodiscard]] inline uint32_t clamp_record_scale_pct(uint32_t pct) noexcept {
    return std::clamp(pct, RECORD_SCALE_MIN_PCT, RECORD_SCALE_MAX_PCT);
}

[[nodiscard]] inline uint32_t clamp_record_keyframe_s(uint32_t seconds) noexcept {
    return std::clamp(seconds, RECORD_KEYFRAME_MIN_S, RECORD_KEYFRAME_MAX_S);
}

/// Snaps audio bitrate to a value natively supported by Windows Media Foundation AAC & WMA encoders.
[[nodiscard]] inline uint32_t normalize_audio_bitrate_kbps(uint32_t kbps) noexcept {
    if (kbps <= 112u) return 96u;
    if (kbps <= 144u) return 128u;
    if (kbps <= 176u) return 160u;
    return 192u;
}

[[nodiscard]] inline uint32_t normalize_audio_sample_rate(uint32_t hz) noexcept {
    return (hz <= 46000u) ? 44100u : 48000u;
}

[[nodiscard]] inline uint32_t normalize_audio_channels(uint32_t ch) noexcept {
    return (ch <= 1u) ? 1u : 2u;
}

[[nodiscard]] inline uint32_t normalize_gif_colors(uint32_t colors) noexcept {
    if (colors <= 96u) return 64u;
    if (colors <= 192u) return 128u;
    return 256u;
}

/// Computes scaled output video dimensions (enforcing >= 16x16 and optional even alignment for H.264/HEVC/WMV).
[[nodiscard]] inline std::pair<uint32_t, uint32_t> compute_scaled_video_dimensions(
    uint32_t src_w, uint32_t src_h, uint32_t scale_pct, bool require_even = true) noexcept {
    const uint32_t pct = clamp_record_scale_pct(scale_pct);
    uint32_t w = (pct == 100u)
        ? src_w
        : static_cast<uint32_t>(std::lround((static_cast<double>(src_w) * pct) / 100.0));
    uint32_t h = (pct == 100u)
        ? src_h
        : static_cast<uint32_t>(std::lround((static_cast<double>(src_h) * pct) / 100.0));
    if (require_even) {
        w &= ~1u;
        h &= ~1u;
    }
    if (w < 16u) w = 16u;
    if (h < 16u) h = 16u;
    return {w, h};
}

/// Computes GIF frame delay in centiseconds (1/100 s) for a nominal frame rate.
[[nodiscard]] inline uint16_t gif_frame_delay_cs(uint32_t fps) noexcept {
    const uint32_t clamped = effective_record_fps(fps, RecordFormat::Gif);
    const auto cs = static_cast<uint32_t>(std::lround(100.0 / static_cast<double>(clamped)));
    return static_cast<uint16_t>(std::clamp(cs, 2u, 100u));
}

/// Estimates a fallback average bitrate (in bps) for MF SinkWriter when operating in Quality rate-control mode.
[[nodiscard]] inline uint32_t estimate_quality_hint_bitrate_bps(uint32_t quality, uint32_t fallback_mbps) noexcept {
    const uint32_t q = clamp_record_quality(quality);
    const uint32_t scaled_mbps = std::clamp(2u + ((q * 38u) / 100u), 2u, 45u);
    return std::max(clamp_record_bitrate_mbps(fallback_mbps), scaled_mbps) * 1'000'000u;
}

/// Returns true if a history entry's format or file extension indicates an animated GIF recording.
[[nodiscard]] inline bool is_gif_capture(std::string_view format, std::string_view path) noexcept {
    if (format == "GIF" || format == "gif") return true;
    if (path.size() >= 4) {
        const std::string_view tail = path.substr(path.size() - 4);
        if (tail == ".gif" || tail == ".GIF" || tail == ".Gif") return true;
    }
    return false;
}

/// Formats a compact recording spec string for overlays (e.g. `MP4 • H.264 • 30 FPS • 12M`).
[[nodiscard]] inline FixedWString<80> format_recording_spec_summary(
    RecordFormat fmt, VideoCodec codec, RateControlMode rc, uint32_t fps,
    uint32_t bitrate_mbps, uint32_t quality, uint32_t scale_pct, uint32_t gif_colors) noexcept {
    FixedWString<80> s;
    s.append(record_format_label(fmt));
    if (fmt == RecordFormat::Mp4) {
        s.append(L" • ");
        s.append(video_codec_short_label(codec));
    }
    s.append(L" • ");
    s.append(to_fixed_wstr<8>(effective_record_fps(fps, fmt)));
    s.append(L" FPS");
    if (fmt == RecordFormat::Gif) {
        s.append(L" • ");
        s.append(to_fixed_wstr<8>(normalize_gif_colors(gif_colors)));
        s.append(L"c");
    } else if (rc == RateControlMode::Quality) {
        s.append(L" • Q");
        s.append(to_fixed_wstr<8>(clamp_record_quality(quality)));
    } else {
        s.append(L" • ");
        s.append(to_fixed_wstr<8>(clamp_record_bitrate_mbps(bitrate_mbps)));
        s.append(L"M");
    }
    if (const uint32_t sc = clamp_record_scale_pct(scale_pct); sc < 100u) {
        s.append(L" • ");
        s.append(to_fixed_wstr<8>(sc));
        s.append(L"%");
    }
    return s;
}

} // namespace capusy
