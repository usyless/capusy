#pragma once

#include "audio_capture.hpp"
#include "capture.hpp"
#include "common.hpp"
#include "recording_logic.hpp"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace capusy {

struct RecordingConfig {
    RecordFormat format = RecordFormat::Mp4;
    VideoCodec codec = VideoCodec::H264;
    H264Profile h264_profile = H264Profile::High;
    RateControlMode rate_control = RateControlMode::Vbr;
    uint32_t fps = 30;
    uint32_t bitrate_mbps = 12;
    uint32_t quality = 80;
    uint32_t scale_pct = 100;
    uint32_t keyframe_s = 2;
    bool hw_accel = true;
    bool fragmented_mp4 = false;
    uint32_t audio_bitrate_kbps = 192;
    uint32_t audio_sample_rate = 48000;
    uint32_t audio_channels = 2;
    uint32_t gif_colors = 256;
    bool gif_dither = false;
    bool include_cursor = true;
    bool record_desktop_audio = true;
    bool record_mic_audio = false;
    std::string desktop_device_id;
    std::string mic_device_id;
};

struct RecordingResult {
    bool ok = false;
    bool cancelled = false;
    fs::path path;
    std::string format_label;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t duration_ms = 0;
    uint64_t size_bytes = 0;
    std::optional<ui::Raster> first_frame; // top-down 32bpp BGRA thumbnail for corner toast
    std::string error;
};

/// Probes whether Windows Media Foundation has at least one HEVC (H.265) video encoder MFT available.
/// Safe to call from STA (UI thread) or MTA (worker threads).
[[nodiscard]] inline bool is_hevc_encoder_available(bool allow_hw = true) noexcept {
    bool com_inited = false;
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com_hr)) com_inited = true;
    const auto com_guard = wil::scope_exit([&] {
        if (com_inited) CoUninitialize();
    });

    if (FAILED(MFStartup(MF_VERSION))) return false;
    const auto mf_guard = wil::scope_exit([] { MFShutdown(); });

    MFT_REGISTER_TYPE_INFO out_info{};
    out_info.guidMajorType = MFMediaType_Video;
    out_info.guidSubtype = MFVideoFormat_HEVC;

    DWORD flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    if (allow_hw) flags |= MFT_ENUM_FLAG_HARDWARE;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &out_info, &activates, &count);
    if (SUCCEEDED(hr) && activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

/// Native Windows Media Foundation (MP4 H.264/HEVC + AAC, WMV9 + WMA) and WIC Animated GIF region screen recorder.
/// Zero external binaries; captures video via Windows.Graphics.Capture + Direct3D 11 GPU pipeline (zero-copy DXGI NV12
/// for hardware MP4 encoding, GPU-scaled staging readback for SW/WMV/GIF) and desktop/mic audio via WASAPI.
class RegionRecorder {
public:
    RegionRecorder() = default;
    RegionRecorder(const RegionRecorder&) = delete;
    RegionRecorder& operator=(const RegionRecorder&) = delete;
    RegionRecorder(RegionRecorder&&) = delete;
    RegionRecorder& operator=(RegionRecorder&&) = delete;
    ~RegionRecorder() { stop_sync(); }

    std::function<void(RecordingResult&&)> on_finished;

    [[nodiscard]] bool recording() const noexcept { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] bool desktop_audio_active() const noexcept { return desktop_audio_active_.load(std::memory_order_acquire); }
    [[nodiscard]] bool mic_audio_active() const noexcept { return mic_audio_active_.load(std::memory_order_acquire); }
    [[nodiscard]] RECT region() const noexcept { return region_; }

    [[nodiscard]] uint64_t elapsed_ms() const noexcept {
        if (!active_.load(std::memory_order_acquire)) return 0;
        const auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count());
    }

    bool start(RECT region, const fs::path& output_path, RecordingConfig cfg = {}) {
        if (active_.load(std::memory_order_acquire)) return false;
        stop_sync();

        cfg.fps = effective_record_fps(cfg.fps, cfg.format);
        cfg.bitrate_mbps = clamp_record_bitrate_mbps(cfg.bitrate_mbps);
        cfg.quality = clamp_record_quality(cfg.quality);
        cfg.scale_pct = clamp_record_scale_pct(cfg.scale_pct);
        cfg.keyframe_s = clamp_record_keyframe_s(cfg.keyframe_s);
        cfg.audio_bitrate_kbps = normalize_audio_bitrate_kbps(cfg.audio_bitrate_kbps);
        cfg.audio_sample_rate = normalize_audio_sample_rate(cfg.audio_sample_rate);
        cfg.audio_channels = normalize_audio_channels(cfg.audio_channels);
        cfg.gif_colors = normalize_gif_colors(cfg.gif_colors);

        const bool require_even = record_format_uses_mf(cfg.format);
        uint32_t src_w = static_cast<uint32_t>(std::max<LONG>(0, region.right - region.left));
        uint32_t src_h = static_cast<uint32_t>(std::max<LONG>(0, region.bottom - region.top));
        if (require_even) {
            src_w &= ~1u;
            src_h &= ~1u;
        }
        if (src_w < 16u) src_w = 16u;
        if (src_h < 16u) src_h = 16u;

        region_ = RECT{
            .left = region.left,
            .top = region.top,
            .right = region.left + static_cast<LONG>(src_w),
            .bottom = region.top + static_cast<LONG>(src_h),
        };
        const auto [out_w, out_h] = compute_scaled_video_dimensions(src_w, src_h, cfg.scale_pct, require_even);

        const bool want_audio = record_format_supports_audio(cfg.format);
        stop_requested_.store(false, std::memory_order_release);
        cancel_requested_.store(false, std::memory_order_release);
        desktop_audio_active_.store(want_audio && cfg.record_desktop_audio, std::memory_order_release);
        mic_audio_active_.store(want_audio && cfg.record_mic_audio, std::memory_order_release);
        active_.store(true, std::memory_order_release);
        start_time_ = std::chrono::steady_clock::now();

        worker_ = std::jthread([this, r = region_, src_w, src_h, out_w, out_h, output_path, cfg = std::move(cfg)] {
            RecordingResult res = (cfg.format == RecordFormat::Gif)
                ? run_gif_worker(r, src_w, src_h, out_w, out_h, output_path, cfg)
                : run_mf_worker(r, src_w, src_h, out_w, out_h, output_path, cfg);
            desktop_audio_active_.store(false, std::memory_order_release);
            mic_audio_active_.store(false, std::memory_order_release);
            active_.store(false, std::memory_order_release);
            if (on_finished) on_finished(std::move(res));
        });
        return true;
    }

    void request_stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
    }

    void request_cancel() noexcept {
        cancel_requested_.store(true, std::memory_order_release);
        stop_requested_.store(true, std::memory_order_release);
    }

    void stop_sync() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        desktop_audio_active_.store(false, std::memory_order_release);
        mic_audio_active_.store(false, std::memory_order_release);
        active_.store(false, std::memory_order_release);
    }

private:
    static std::vector<WgcMonitorRect> enumerate_wgc_monitors() {
        auto mons = enumerate_monitors();
        std::vector<WgcMonitorRect> out;
        out.reserve(mons.size());
        for (const MonitorInfo& m : mons) {
            if (m.hmon) out.push_back(WgcMonitorRect{.hmon = m.hmon, .rect = m.rect});
        }
        return out;
    }

    static UINT32 to_mf_h264_profile(H264Profile p) noexcept {
        switch (p) {
            case H264Profile::Baseline: return eAVEncH264VProfile_Base;
            case H264Profile::Main: return eAVEncH264VProfile_Main;
            case H264Profile::High: return eAVEncH264VProfile_High;
        }
        return eAVEncH264VProfile_High;
    }

    static UINT32 to_mf_rate_control(RateControlMode m) noexcept {
        switch (m) {
            case RateControlMode::Vbr: return eAVEncCommonRateControlMode_UnconstrainedVBR;
            case RateControlMode::Quality: return eAVEncCommonRateControlMode_Quality;
            case RateControlMode::Cbr: return eAVEncCommonRateControlMode_CBR;
        }
        return eAVEncCommonRateControlMode_UnconstrainedVBR;
    }

    static void apply_codec_api_best_effort(IMFSinkWriter* writer, DWORD stream_idx,
                                            const RecordingConfig& cfg, uint32_t fps, uint32_t bitrate) noexcept {
        wil::com_ptr<ICodecAPI> codec_api;
        if (FAILED(writer->GetServiceForStream(stream_idx, GUID_NULL, IID_PPV_ARGS(&codec_api))) || !codec_api) {
            return;
        }
        const auto set_u32 = [&](const GUID& prop, UINT32 val) noexcept {
            VARIANT v{};
            v.vt = VT_UI4;
            v.ulVal = val;
            (void)codec_api->SetValue(&prop, &v);
        };
        set_u32(CODECAPI_AVEncCommonRateControlMode, to_mf_rate_control(cfg.rate_control));
        if (cfg.rate_control == RateControlMode::Quality) {
            set_u32(CODECAPI_AVEncCommonQuality, cfg.quality);
        } else {
            set_u32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
        }
        set_u32(CODECAPI_AVEncMPVGOPSize, std::max<uint32_t>(1u, fps * cfg.keyframe_s));
    }

    static bool configure_video_stream(IMFSinkWriter* writer, uint32_t w, uint32_t h, uint32_t fps,
                                       const RecordingConfig& cfg, VideoCodec effective_codec, bool use_nv12_dxgi,
                                       DWORD& out_stream_idx, std::string& err) {
        const uint32_t target_bitrate = (cfg.rate_control == RateControlMode::Quality)
            ? estimate_quality_hint_bitrate_bps(cfg.quality, cfg.bitrate_mbps)
            : (cfg.bitrate_mbps * 1'000'000u);

        wil::com_ptr<IMFMediaType> out_type;
        if (FAILED(MFCreateMediaType(&out_type)) || !out_type) {
            err = "MFCreateMediaType (video output) failed";
            return false;
        }
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (cfg.format == RecordFormat::Wmv) {
            out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_WMV3);
        } else if (effective_codec == VideoCodec::Hevc) {
            out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
        } else {
            out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            out_type->SetUINT32(MF_MT_MPEG2_PROFILE, to_mf_h264_profile(cfg.h264_profile));
        }
        out_type->SetUINT32(MF_MT_AVG_BITRATE, target_bitrate);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (use_nv12_dxgi) {
            out_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            out_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            out_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            out_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        }
        MFSetAttributeSize(out_type.get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(out_type.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (FAILED(writer->AddStream(out_type.get(), &out_stream_idx))) {
            err = (cfg.format == RecordFormat::Wmv)
                ? "Failed to add WMV9 output stream"
                : (effective_codec == VideoCodec::Hevc ? "Failed to add HEVC output stream" : "Failed to add H.264 output stream");
            return false;
        }

        // Input stream: GPU NV12 surface (zero-copy HW path) or top-down 32bpp RGB32 (SW / WMV fallback).
        wil::com_ptr<IMFMediaType> in_type;
        if (FAILED(MFCreateMediaType(&in_type)) || !in_type) {
            err = "MFCreateMediaType (video input) failed";
            return false;
        }
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (use_nv12_dxgi) {
            in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
            in_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            in_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            in_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            in_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        } else {
            in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4u);
        }
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in_type.get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in_type.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(in_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        wil::com_ptr<IMFAttributes> enc_params;
        if (SUCCEEDED(MFCreateAttributes(&enc_params, 4)) && enc_params) {
            enc_params->SetUINT32(CODECAPI_AVEncCommonRateControlMode, to_mf_rate_control(cfg.rate_control));
            if (cfg.rate_control == RateControlMode::Quality) {
                enc_params->SetUINT32(CODECAPI_AVEncCommonQuality, cfg.quality);
            } else {
                enc_params->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, target_bitrate);
            }
            enc_params->SetUINT32(CODECAPI_AVEncMPVGOPSize, std::max<uint32_t>(1u, fps * cfg.keyframe_s));
        }

        if (FAILED(writer->SetInputMediaType(out_stream_idx, in_type.get(), enc_params.get()))) {
            // Some hardware or WMV MFTs reject CodecAPI attributes during SetInputMediaType; retry cleanly and apply best-effort.
            if (FAILED(writer->SetInputMediaType(out_stream_idx, in_type.get(), nullptr))) {
                err = use_nv12_dxgi ? "Failed to configure NV12 DXGI input media type" : "Failed to configure RGB32 input media type";
                return false;
            }
            apply_codec_api_best_effort(writer, out_stream_idx, cfg, fps, target_bitrate);
        }
        return true;
    }

    static bool configure_aac_audio_stream(IMFSinkWriter* writer, uint32_t bitrate_kbps, uint32_t sample_rate,
                                           uint32_t channels, DWORD& out_stream_idx,
                                           uint32_t& actual_sample_rate, uint32_t& actual_channels) {
        wil::com_ptr<IMFMediaType> out_type;
        if (FAILED(MFCreateMediaType(&out_type)) || !out_type) return false;
        const uint32_t bytes_per_sec = (bitrate_kbps * 1000u) / 8u;
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
        out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes_per_sec);
        out_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        out_type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

        if (FAILED(writer->AddStream(out_type.get(), &out_stream_idx))) return false;

        wil::com_ptr<IMFMediaType> in_type;
        if (FAILED(MFCreateMediaType(&in_type)) || !in_type) return false;
        const uint32_t block_align = channels * 2u;
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
        in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sample_rate * block_align);

        if (FAILED(writer->SetInputMediaType(out_stream_idx, in_type.get(), nullptr))) return false;
        actual_sample_rate = sample_rate;
        actual_channels = channels;
        return true;
    }

    static bool configure_wma_audio_stream(IMFSinkWriter* writer, uint32_t bitrate_kbps, uint32_t sample_rate,
                                           uint32_t channels, DWORD& out_stream_idx,
                                           uint32_t& actual_sample_rate, uint32_t& actual_channels) {
        const DWORD flags = (MFT_ENUM_FLAG_ALL & ~MFT_ENUM_FLAG_FIELDOFUSE) | MFT_ENUM_FLAG_SORTANDFILTER;
        const uint32_t target_bps = (bitrate_kbps * 1000u) / 8u;

        wil::com_ptr<IMFMediaType> best_type;
        uint64_t best_score = (std::numeric_limits<uint64_t>::max)();
        uint32_t best_sr = sample_rate;
        uint32_t best_ch = channels;

        const std::array<GUID, 2> subtypes = {MFAudioFormat_WMAudioV9, MFAudioFormat_WMAudioV8};
        for (const GUID& sub : subtypes) {
            wil::com_ptr<IMFCollection> coll;
            if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(sub, flags, nullptr, &coll)) || !coll) continue;
            DWORD count = 0;
            if (FAILED(coll->GetElementCount(&count))) continue;
            for (DWORD i = 0; i < count; ++i) {
                wil::com_ptr<IUnknown> unk;
                if (FAILED(coll->GetElement(i, &unk)) || !unk) continue;
                wil::com_ptr<IMFMediaType> mt;
                if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&mt))) || !mt) continue;
                UINT32 ch = 0;
                UINT32 sr = 0;
                UINT32 bps = 0;
                UINT32 bits = 16;
                (void)mt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                (void)mt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
                (void)mt->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
                (void)mt->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                if ((ch != 1u && ch != 2u) || sr == 0 || bits != 16u) continue;

                const uint64_t ch_pen = (ch == channels) ? 0ull : 10'000'000ull;
                const uint64_t sr_pen = (sr == sample_rate) ? 0ull : 1'000'000ull;
                const uint64_t bps_diff = (bps > target_bps) ? (bps - target_bps) : (target_bps - bps);
                const uint64_t score = ch_pen + sr_pen + bps_diff;
                if (score < best_score) {
                    best_score = score;
                    best_type = mt;
                    best_sr = sr;
                    best_ch = ch;
                }
            }
            if (best_type) break;
        }
        if (!best_type) return false;

        if (FAILED(writer->AddStream(best_type.get(), &out_stream_idx))) return false;

        wil::com_ptr<IMFMediaType> in_type;
        if (FAILED(MFCreateMediaType(&in_type)) || !in_type) return false;
        const uint32_t block_align = best_ch * 2u;
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, best_sr);
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, best_ch);
        in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
        in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, best_sr * block_align);

        if (FAILED(writer->SetInputMediaType(out_stream_idx, in_type.get(), nullptr))) return false;
        actual_sample_rate = best_sr;
        actual_channels = best_ch;
        return true;
    }

    static bool configure_audio_stream(IMFSinkWriter* writer, const RecordingConfig& cfg, DWORD& out_stream_idx,
                                       uint32_t& actual_sample_rate, uint32_t& actual_channels) {
        if (cfg.format == RecordFormat::Wmv) {
            return configure_wma_audio_stream(writer, cfg.audio_bitrate_kbps, cfg.audio_sample_rate,
                                              cfg.audio_channels, out_stream_idx, actual_sample_rate, actual_channels);
        }
        return configure_aac_audio_stream(writer, cfg.audio_bitrate_kbps, cfg.audio_sample_rate,
                                          cfg.audio_channels, out_stream_idx, actual_sample_rate, actual_channels);
    }

    static bool write_audio_chunk(IMFSinkWriter* writer, std::mutex& writer_mx, DWORD audio_stream_idx,
                                  const int16_t* pcm16, uint32_t frames, uint32_t sample_rate, uint32_t channels,
                                  uint64_t sample_offset_frames) {
        const DWORD bytes = frames * channels * sizeof(int16_t);
        wil::com_ptr<IMFMediaBuffer> buf;
        wil::com_ptr<IMFSample> sample;
        if (FAILED(MFCreateMemoryBuffer(bytes, &buf)) || !buf ||
            FAILED(MFCreateSample(&sample)) || !sample) {
            return false;
        }
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr)) || !dst) return false;
        std::memcpy(dst, pcm16, bytes);
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        sample->AddBuffer(buf.get());

        const LONGLONG ts_100ns = static_cast<LONGLONG>((sample_offset_frames * 10'000'000ull) / sample_rate);
        const LONGLONG dur_100ns = static_cast<LONGLONG>((static_cast<uint64_t>(frames) * 10'000'000ull) / sample_rate);
        sample->SetSampleTime(ts_100ns);
        sample->SetSampleDuration(dur_100ns);

        const std::scoped_lock lk{writer_mx};
        return SUCCEEDED(writer->WriteSample(audio_stream_idx, sample.get()));
    }

    RecordingResult run_mf_worker(RECT region, uint32_t src_w, uint32_t src_h, uint32_t w, uint32_t h,
                                  const fs::path& output_path, const RecordingConfig& cfg) {
        RecordingResult res;
        res.path = output_path;
        res.format_label = std::string{record_format_label_utf8(cfg.format)};
        res.width = w;
        res.height = h;

        const uint32_t fps = cfg.fps;

        const wgc_detail::RoScope ro{RO_INIT_MULTITHREADED};
        if (!ro.ok()) {
            res.error = "WinRT initialization failed";
            return res;
        }
        if (FAILED(MFStartup(MF_VERSION))) {
            res.error = "MFStartup failed";
            return res;
        }
        const auto mf_guard = wil::scope_exit([] { MFShutdown(); });

        std::error_code ec;
        if (output_path.has_parent_path()) fs::create_directories(output_path.parent_path(), ec);

        // Initialize Windows.Graphics.Capture + D3D11 device up front so HW MF can share the device via IMFDXGIDeviceManager.
        WgcRegionSurface surface;
        const auto wgc_mons = enumerate_wgc_monitors();
        if (!surface.init(region, wgc_mons, src_w, src_h, w, h, cfg.include_cursor, fps, res.error)) {
            return res;
        }

        wil::com_ptr<IMFDXGIDeviceManager> dxgi_mgr;
        bool dxgi_nv12_ready = false;
        if (cfg.hw_accel && cfg.format == RecordFormat::Mp4 && surface.d3d_device()) {
            UINT reset_token = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&reset_token, &dxgi_mgr)) && dxgi_mgr &&
                SUCCEEDED(dxgi_mgr->ResetDevice(surface.d3d_device(), reset_token)) &&
                surface.init_nv12_pipeline()) {
                dxgi_nv12_ready = true;
            } else {
                dxgi_mgr.reset();
            }
        }

        // Initialize requested WASAPI audio capture streams before creating the sink writer.
        WasapiCaptureStream desktop_cap;
        WasapiCaptureStream mic_cap;
        bool desktop_ok = cfg.record_desktop_audio && desktop_cap.open(AudioFlow::Render, cfg.desktop_device_id, cfg.audio_sample_rate);
        bool mic_ok = cfg.record_mic_audio && mic_cap.open(AudioFlow::Capture, cfg.mic_device_id, cfg.audio_sample_rate);
        bool has_audio = desktop_ok || mic_ok;

        wil::com_ptr<IMFSinkWriter> writer;
        DWORD video_stream_idx = 0;
        DWORD audio_stream_idx = 0;
        uint32_t actual_audio_sr = cfg.audio_sample_rate;
        uint32_t actual_audio_ch = cfg.audio_channels;
        bool using_dxgi_nv12 = false;

        auto try_init_writer = [&](VideoCodec eff_codec, bool use_hw, bool use_dxgi_nv12, bool with_audio) -> bool {
            writer.reset();
            using_dxgi_nv12 = false;
            wil::com_ptr<IMFAttributes> attrs;
            if (FAILED(MFCreateAttributes(&attrs, 4)) || !attrs) {
                res.error = "MFCreateAttributes failed";
                return false;
            }
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, use_hw ? TRUE : FALSE);
            attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            if (use_dxgi_nv12 && dxgi_mgr) {
                attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgi_mgr.get());
            }
            if (cfg.format == RecordFormat::Wmv) {
                attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_ASF);
            } else {
                attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                               cfg.fragmented_mp4 ? MFTranscodeContainerType_FMPEG4 : MFTranscodeContainerType_MPEG4);
            }

            if (FAILED(MFCreateSinkWriterFromURL(output_path.c_str(), nullptr, attrs.get(), &writer)) || !writer) {
                res.error = (cfg.format == RecordFormat::Wmv)
                    ? "Failed to create Media Foundation WMV sink writer"
                    : "Failed to create Media Foundation MP4 sink writer";
                return false;
            }
            if (!configure_video_stream(writer.get(), w, h, fps, cfg, eff_codec, use_dxgi_nv12, video_stream_idx, res.error)) {
                return false;
            }
            if (with_audio && !configure_audio_stream(writer.get(), cfg, audio_stream_idx, actual_audio_sr, actual_audio_ch)) {
                return false;
            }
            using_dxgi_nv12 = use_dxgi_nv12;
            return true;
        };

        auto init_with_fallbacks = [&](bool with_audio) -> bool {
            if (dxgi_nv12_ready && try_init_writer(cfg.codec, true, true, with_audio)) return true;
            if (try_init_writer(cfg.codec, cfg.hw_accel, false, with_audio)) return true;
            if (cfg.hw_accel && try_init_writer(cfg.codec, false, false, with_audio)) return true;
            if (cfg.format == RecordFormat::Mp4 && cfg.codec == VideoCodec::Hevc) {
                if (dxgi_nv12_ready && try_init_writer(VideoCodec::H264, true, true, with_audio)) return true;
                if (try_init_writer(VideoCodec::H264, cfg.hw_accel, false, with_audio)) return true;
                if (cfg.hw_accel && try_init_writer(VideoCodec::H264, false, false, with_audio)) return true;
            }
            return false;
        };

        if (has_audio) {
            if (!init_with_fallbacks(true)) {
                // Gracefully fall back to video-only if audio stream setup fails on this system.
                desktop_cap.close();
                mic_cap.close();
                desktop_ok = false;
                mic_ok = false;
                has_audio = false;
                res.error.clear();
                if (!init_with_fallbacks(false)) return res;
            }
        } else {
            if (!init_with_fallbacks(false)) return res;
        }

        if (!using_dxgi_nv12 && !surface.init_cpu_pipeline(res.error)) {
            return res;
        }

        if (has_audio && actual_audio_sr != cfg.audio_sample_rate) {
            if (desktop_ok) desktop_cap.retarget_sample_rate(actual_audio_sr);
            if (mic_ok) mic_cap.retarget_sample_rate(actual_audio_sr);
        }

        desktop_audio_active_.store(desktop_ok, std::memory_order_release);
        mic_audio_active_.store(mic_ok, std::memory_order_release);

        if (FAILED(writer->BeginWriting())) {
            res.error = "IMFSinkWriter::BeginWriting failed";
            return res;
        }

        // Wait for the initial WGC frame from each intersecting monitor before starting clocks & audio so frame 0 isn't black.
        surface.wait_initial_frames(stop_requested_, 600u);

        if (desktop_ok && !desktop_cap.start()) desktop_ok = false;
        if (mic_ok && !mic_cap.start()) mic_ok = false;
        desktop_audio_active_.store(desktop_ok, std::memory_order_release);
        mic_audio_active_.store(mic_ok, std::memory_order_release);

        const auto t0 = std::chrono::steady_clock::now();
        std::mutex writer_mx;
        std::jthread audio_thread;

        if (has_audio) {
            audio_thread = std::jthread([&, t0, desktop_ok, mic_ok, audio_stream_idx, actual_audio_sr, actual_audio_ch] {
                const ulp::win::COMWrapper<false> audio_com{static_cast<DWORD>(COINIT_MULTITHREADED)};
                const uint32_t chunk_frames = std::max<uint32_t>(256u, actual_audio_sr / 50u); // 20 ms chunks
                StereoFloatFifo desktop_fifo{actual_audio_sr};
                StereoFloatFifo mic_fifo{actual_audio_sr};
                std::vector<float> mix_a(static_cast<size_t>(chunk_frames) * 2u, 0.0f);
                std::vector<float> mix_b(static_cast<size_t>(chunk_frames) * 2u, 0.0f);
                std::vector<int16_t> pcm16(static_cast<size_t>(chunk_frames) * actual_audio_ch, 0);

                const uint64_t jitter_cushion = (static_cast<uint64_t>(actual_audio_sr) * 30ull) / 1000ull;  // 30 ms
                const uint64_t max_lag_pad = (static_cast<uint64_t>(actual_audio_sr) * 80ull) / 1000ull;     // 80 ms
                const size_t max_backlog = (static_cast<size_t>(actual_audio_sr) * 160u) / 1000u;            // 160 ms
                const size_t trim_target = (static_cast<size_t>(actual_audio_sr) * 40u) / 1000u;             // 40 ms

                auto last_desktop_pkt = std::chrono::steady_clock::time_point{};
                auto last_mic_pkt = std::chrono::steady_clock::time_point{};
                uint64_t audio_frames_written = 0;

                const auto emit_one_chunk = [&] {
                    std::span<const float> sa{};
                    std::span<const float> sb{};
                    if (desktop_ok) {
                        const size_t got = desktop_fifo.pop_frames(mix_a.data(), chunk_frames);
                        if (got < chunk_frames) {
                            std::fill(mix_a.begin() + static_cast<ptrdiff_t>(got * 2u), mix_a.end(), 0.0f);
                        }
                        sa = mix_a;
                    }
                    if (mic_ok) {
                        const size_t got = mic_fifo.pop_frames(mix_b.data(), chunk_frames);
                        if (got < chunk_frames) {
                            std::fill(mix_b.begin() + static_cast<ptrdiff_t>(got * 2u), mix_b.end(), 0.0f);
                        }
                        sb = mix_b;
                    }
                    if (actual_audio_ch == 1u) {
                        mix_stereo_to_mono_pcm16(sa, sb, pcm16);
                    } else {
                        mix_stereo_to_pcm16(sa, sb, pcm16);
                    }
                    (void)write_audio_chunk(writer.get(), writer_mx, audio_stream_idx, pcm16.data(),
                                            chunk_frames, actual_audio_sr, actual_audio_ch, audio_frames_written);
                    audio_frames_written += chunk_frames;
                };

                while (!stop_requested_.load(std::memory_order_acquire)) {
                    const auto now = std::chrono::steady_clock::now();
                    if (desktop_ok && desktop_cap.drain_into(desktop_fifo)) last_desktop_pkt = now;
                    if (mic_ok && mic_cap.drain_into(mic_fifo)) last_mic_pkt = now;
                    desktop_fifo.trim_excess(max_backlog, trim_target);
                    mic_fifo.trim_excess(max_backlog, trim_target);

                    const auto elapsed_us = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count());
                    const uint64_t target_frames = (static_cast<uint64_t>(elapsed_us) * actual_audio_sr) / 1'000'000ull;
                    const uint64_t emit_limit = (target_frames > jitter_cushion) ? (target_frames - jitter_cushion) : 0ull;

                    while (audio_frames_written + chunk_frames <= emit_limit) {
                        const bool desktop_recent = desktop_ok && last_desktop_pkt.time_since_epoch().count() != 0 &&
                                                    (now - last_desktop_pkt) < std::chrono::milliseconds{50};
                        const bool mic_recent = mic_ok && last_mic_pkt.time_since_epoch().count() != 0 &&
                                                (now - last_mic_pkt) < std::chrono::milliseconds{50};
                        const bool desktop_waiting = desktop_recent && desktop_fifo.frames_available() < chunk_frames;
                        const bool mic_waiting = mic_recent && mic_fifo.frames_available() < chunk_frames;
                        if ((desktop_waiting || mic_waiting) && (target_frames - audio_frames_written) < max_lag_pad) {
                            break;
                        }
                        emit_one_chunk();
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }

                // Final drain & flush up to recording end time
                if (desktop_ok) (void)desktop_cap.drain_into(desktop_fifo);
                if (mic_ok) (void)mic_cap.drain_into(mic_fifo);
                desktop_cap.stop();
                mic_cap.stop();

                const auto end_us = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
                const uint64_t final_target = std::max<uint64_t>(
                    chunk_frames,
                    (static_cast<uint64_t>(end_us) * actual_audio_sr) / 1'000'000ull);
                while (audio_frames_written + chunk_frames <= final_target) {
                    emit_one_chunk();
                }
                if (audio_frames_written == 0) {
                    emit_one_chunk();
                }
            });
        }

        const DWORD frame_bytes = surface.frame_bytes();
        const LONGLONG frame_dur_100ns = 10'000'000ll / static_cast<LONGLONG>(fps);
        const auto frame_period = std::chrono::microseconds{1'000'000 / fps};
        auto next_tick = t0;
        uint64_t frames_written = 0;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            const LONGLONG ts_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count() / 100ll;

            surface.update_from_monitors();

            wil::com_ptr<IMFMediaBuffer> buf;
            wil::com_ptr<IMFSample> sample;

            if (using_dxgi_nv12) {
                if (!res.first_frame) {
                    res.first_frame = surface.copy_first_frame();
                }
                if (auto nv12_tex = surface.render_nv12_frame()) {
                    if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_tex.get(), 0, FALSE, &buf)) && buf &&
                        SUCCEEDED(MFCreateSample(&sample)) && sample) {
                        DWORD contig_len = (w * h * 3u) / 2u;
                        if (const auto buf2d = buf.try_query<IMF2DBuffer>()) {
                            (void)buf2d->GetContiguousLength(&contig_len);
                        }
                        buf->SetCurrentLength(contig_len);
                        sample->AddBuffer(buf.get());
                        sample->SetSampleTime(ts_100ns);
                        sample->SetSampleDuration(frame_dur_100ns);
                        HRESULT wr_hr = E_FAIL;
                        {
                            const std::scoped_lock lk{writer_mx};
                            wr_hr = writer->WriteSample(video_stream_idx, sample.get());
                        }
                        if (SUCCEEDED(wr_hr)) {
                            ++frames_written;
                        }
                    }
                }
            } else if (surface.readback_cpu_bgra()) {
                if (!res.first_frame) {
                    res.first_frame = surface.copy_first_frame();
                }
                if (SUCCEEDED(MFCreateMemoryBuffer(frame_bytes, &buf)) && buf &&
                    SUCCEEDED(MFCreateSample(&sample)) && sample) {
                    BYTE* dst_ptr = nullptr;
                    if (SUCCEEDED(buf->Lock(&dst_ptr, nullptr, nullptr)) && dst_ptr) {
                        std::memcpy(dst_ptr, surface.cpu_bits(), frame_bytes);
                        buf->Unlock();
                        buf->SetCurrentLength(frame_bytes);
                        sample->AddBuffer(buf.get());
                        sample->SetSampleTime(ts_100ns);
                        sample->SetSampleDuration(frame_dur_100ns);
                        HRESULT wr_hr = E_FAIL;
                        {
                            const std::scoped_lock lk{writer_mx};
                            wr_hr = writer->WriteSample(video_stream_idx, sample.get());
                        }
                        if (SUCCEEDED(wr_hr)) {
                            ++frames_written;
                        }
                    }
                }
            }

            next_tick += frame_period;
            const auto after = std::chrono::steady_clock::now();
            if (next_tick > after) {
                std::this_thread::sleep_until(next_tick);
            } else {
                next_tick = after;
            }
        }

        if (audio_thread.joinable()) audio_thread.join();
        desktop_cap.close();
        mic_cap.close();

        const auto end_time = std::chrono::steady_clock::now();
        res.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end_time - t0).count());

        if (cancel_requested_.load(std::memory_order_acquire)) {
            writer.reset();
            fs::remove(output_path, ec);
            res.cancelled = true;
            res.ok = false;
            return res;
        }

        if (frames_written > 0 && SUCCEEDED(writer->Finalize())) {
            res.size_bytes = fs::file_size(output_path, ec);
            if (ec) res.size_bytes = 0;
            res.ok = true;
        } else {
            writer.reset();
            fs::remove(output_path, ec);
            res.error = frames_written == 0
                ? "No video frames were captured"
                : (cfg.format == RecordFormat::Wmv ? "Failed to finalize WMV file" : "Failed to finalize MP4 file");
        }
        return res;
    }

    RecordingResult run_gif_worker(RECT region, uint32_t src_w, uint32_t src_h, uint32_t w, uint32_t h,
                                   const fs::path& output_path, const RecordingConfig& cfg) {
        RecordingResult res;
        res.path = output_path;
        res.format_label = "GIF";
        res.width = w;
        res.height = h;

        const uint32_t fps = cfg.fps;
        const wgc_detail::RoScope ro{RO_INIT_MULTITHREADED};
        if (!ro.ok()) {
            res.error = "WinRT initialization failed for GIF recording";
            return res;
        }

        std::error_code ec;
        if (output_path.has_parent_path()) fs::create_directories(output_path.parent_path(), ec);

        wil::com_ptr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) {
            res.error = "Failed to initialize WIC for GIF recording";
            return res;
        }

        wil::com_ptr<IWICStream> stream;
        if (FAILED(wic->CreateStream(&stream)) || !stream ||
            FAILED(stream->InitializeFromFilename(output_path.c_str(), GENERIC_WRITE))) {
            res.error = "Failed to open GIF output file";
            return res;
        }

        wil::com_ptr<IWICBitmapEncoder> encoder;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder)) || !encoder ||
            FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
            res.error = "Failed to initialize WIC GIF encoder";
            return res;
        }

        // Write NETSCAPE2.0 infinite loop application extension
        wil::com_ptr<IWICMetadataQueryWriter> global_meta;
        if (SUCCEEDED(encoder->GetMetadataQueryWriter(&global_meta)) && global_meta) {
            std::array<BYTE, 11> app_id{'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
            PROPVARIANT pv_app{};
            pv_app.vt = VT_UI1 | VT_VECTOR;
            pv_app.caub.cElems = static_cast<ULONG>(app_id.size());
            pv_app.caub.pElems = app_id.data();

            std::array<BYTE, 4> loop_data{3, 1, 0, 0}; // 0 = loop infinitely
            PROPVARIANT pv_data{};
            pv_data.vt = VT_UI1 | VT_VECTOR;
            pv_data.caub.cElems = static_cast<ULONG>(loop_data.size());
            pv_data.caub.pElems = loop_data.data();

            (void)global_meta->SetMetadataByName(L"/appext/Application", &pv_app);
            (void)global_meta->SetMetadataByName(L"/appext/data", &pv_data);
        }

        WgcRegionSurface surface;
        const auto wgc_mons = enumerate_wgc_monitors();
        if (!surface.init(region, wgc_mons, src_w, src_h, w, h, cfg.include_cursor, fps, res.error) ||
            !surface.init_cpu_pipeline(res.error)) {
            return res;
        }
        surface.wait_initial_frames(stop_requested_, 600u);

        const auto t0 = std::chrono::steady_clock::now();
        const auto frame_period = std::chrono::microseconds{1'000'000 / fps};
        const uint16_t nominal_delay_cs = gif_frame_delay_cs(fps);
        const WICBitmapDitherType dither = cfg.gif_dither ? WICBitmapDitherTypeErrorDiffusion : WICBitmapDitherTypeNone;
        const UINT stride = w * 4u;
        const UINT frame_bytes = surface.frame_bytes();

        auto next_tick = t0;
        auto prev_capture_time = t0;
        uint64_t frames_written = 0;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const auto cap_time = std::chrono::steady_clock::now();
            surface.update_from_monitors();
            if (!surface.readback_cpu_bgra()) {
                next_tick += frame_period;
                const auto after = std::chrono::steady_clock::now();
                if (next_tick > after) std::this_thread::sleep_until(next_tick);
                else next_tick = after;
                continue;
            }
            if (!res.first_frame) {
                res.first_frame = surface.copy_first_frame();
            }

            uint16_t delay_cs = nominal_delay_cs;
            if (frames_written > 0) {
                const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cap_time - prev_capture_time).count();
                if (dt_ms > 0) {
                    delay_cs = static_cast<uint16_t>(std::clamp<int64_t>((dt_ms + 5) / 10, 2, 500));
                }
            }
            prev_capture_time = cap_time;

            wil::com_ptr<IWICBitmap> src_bmp;
            if (SUCCEEDED(wic->CreateBitmapFromMemory(
                    w, h, GUID_WICPixelFormat32bppBGR, stride, frame_bytes,
                    const_cast<BYTE*>(reinterpret_cast<const BYTE*>(surface.cpu_bits())), &src_bmp)) && src_bmp) {
                wil::com_ptr<IWICPalette> palette;
                wil::com_ptr<IWICFormatConverter> conv;
                wil::com_ptr<IWICBitmapFrameEncode> frame;
                if (SUCCEEDED(wic->CreatePalette(&palette)) && palette &&
                    SUCCEEDED(palette->InitializeFromBitmap(src_bmp.get(), cfg.gif_colors, FALSE)) &&
                    SUCCEEDED(wic->CreateFormatConverter(&conv)) && conv &&
                    SUCCEEDED(conv->Initialize(src_bmp.get(), GUID_WICPixelFormat8bppIndexed, dither,
                                               palette.get(), 0.0, WICBitmapPaletteTypeCustom)) &&
                    SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && frame &&
                    SUCCEEDED(frame->Initialize(nullptr))) {
                    (void)frame->SetSize(w, h);
                    WICPixelFormatGUID pf = GUID_WICPixelFormat8bppIndexed;
                    (void)frame->SetPixelFormat(&pf);
                    (void)frame->SetPalette(palette.get());

                    wil::com_ptr<IWICMetadataQueryWriter> fmeta;
                    if (SUCCEEDED(frame->GetMetadataQueryWriter(&fmeta)) && fmeta) {
                        PROPVARIANT pv_delay{};
                        pv_delay.vt = VT_UI2;
                        pv_delay.uiVal = delay_cs;
                        (void)fmeta->SetMetadataByName(L"/grctlext/Delay", &pv_delay);

                        PROPVARIANT pv_disp{};
                        pv_disp.vt = VT_UI1;
                        pv_disp.bVal = 1; // Do not dispose (full-frame overwrite)
                        (void)fmeta->SetMetadataByName(L"/grctlext/Disposal", &pv_disp);
                    }

                    if (SUCCEEDED(frame->WriteSource(conv.get(), nullptr)) && SUCCEEDED(frame->Commit())) {
                        ++frames_written;
                    }
                }
            }

            next_tick += frame_period;
            const auto after = std::chrono::steady_clock::now();
            if (next_tick > after) {
                std::this_thread::sleep_until(next_tick);
            } else {
                next_tick = after;
            }
        }

        const auto end_time = std::chrono::steady_clock::now();
        res.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end_time - t0).count());

        if (cancel_requested_.load(std::memory_order_acquire)) {
            encoder.reset();
            stream.reset();
            fs::remove(output_path, ec);
            res.cancelled = true;
            res.ok = false;
            return res;
        }

        if (frames_written > 0 && SUCCEEDED(encoder->Commit())) {
            encoder.reset();
            stream.reset();
            res.size_bytes = fs::file_size(output_path, ec);
            if (ec) res.size_bytes = 0;
            res.ok = true;
        } else {
            encoder.reset();
            stream.reset();
            fs::remove(output_path, ec);
            res.error = frames_written == 0 ? "No GIF frames were captured" : "Failed to finalize GIF file";
        }
        return res;
    }

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> desktop_audio_active_{false};
    std::atomic<bool> mic_audio_active_{false};
    std::chrono::steady_clock::time_point start_time_{};
    RECT region_{};
    std::jthread worker_;
};

} // namespace capusy
