#pragma once

#include "audio_capture.hpp"
#include "capture.hpp"
#include "common.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace capusy {

struct RecordingConfig {
    uint32_t fps = 30;
    uint32_t bitrate_mbps = 12;
    bool include_cursor = true;
    bool record_desktop_audio = true;
    bool record_mic_audio = false;
    std::string desktop_device_id;
    std::string mic_device_id;
};

struct RecordingResult {
    bool ok = false;
    fs::path path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t duration_ms = 0;
    uint64_t size_bytes = 0;
    std::optional<ui::Raster> first_frame; // top-down 32bpp BGRA thumbnail for corner toast
    std::string error;
};

/// Native Windows Media Foundation H.264 + AAC MP4 region screen recorder.
/// Zero external binaries; captures video via persistent DIB section and desktop/mic audio via WASAPI.
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

        // H.264 requires even width and even height (>= 16x16).
        uint32_t w = static_cast<uint32_t>(std::max<LONG>(0, region.right - region.left)) & ~1u;
        uint32_t h = static_cast<uint32_t>(std::max<LONG>(0, region.bottom - region.top)) & ~1u;
        if (w < 16u) w = 16u;
        if (h < 16u) h = 16u;
        region_ = RECT{.left = region.left, .top = region.top, .right = region.left + static_cast<LONG>(w), .bottom = region.top + static_cast<LONG>(h)};
        cfg.fps = (cfg.fps == 60u) ? 60u : 30u;
        cfg.bitrate_mbps = std::clamp(cfg.bitrate_mbps, 2u, 50u);

        stop_requested_.store(false, std::memory_order_release);
        desktop_audio_active_.store(cfg.record_desktop_audio, std::memory_order_release);
        mic_audio_active_.store(cfg.record_mic_audio, std::memory_order_release);
        active_.store(true, std::memory_order_release);
        start_time_ = std::chrono::steady_clock::now();

        worker_ = std::jthread([this, r = region_, w, h, output_path, cfg = std::move(cfg)] {
            RecordingResult res = run_worker(r, w, h, output_path, cfg);
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

    void stop_sync() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        desktop_audio_active_.store(false, std::memory_order_release);
        mic_audio_active_.store(false, std::memory_order_release);
        active_.store(false, std::memory_order_release);
    }

private:
    static bool configure_video_stream(IMFSinkWriter* writer, uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate,
                                       DWORD& out_stream_idx, std::string& err) {
        wil::com_ptr<IMFMediaType> out_type;
        if (FAILED(MFCreateMediaType(&out_type)) || !out_type) {
            err = "MFCreateMediaType (video output) failed";
            return false;
        }
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out_type.get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(out_type.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (FAILED(writer->AddStream(out_type.get(), &out_stream_idx))) {
            err = "Failed to add H.264 output stream";
            return false;
        }

        // Input stream: top-down 32bpp RGB32 (positive MF_MT_DEFAULT_STRIDE = top-down, universally accepted by HW & SW Windows H.264 MFTs)
        wil::com_ptr<IMFMediaType> in_type;
        if (FAILED(MFCreateMediaType(&in_type)) || !in_type) {
            err = "MFCreateMediaType (video input) failed";
            return false;
        }
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4u);
        MFSetAttributeSize(in_type.get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in_type.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(in_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (FAILED(writer->SetInputMediaType(out_stream_idx, in_type.get(), nullptr))) {
            err = "Failed to configure RGB32 input media type";
            return false;
        }
        return true;
    }

    static bool configure_audio_stream(IMFSinkWriter* writer, DWORD& out_stream_idx) {
        wil::com_ptr<IMFMediaType> out_type;
        if (FAILED(MFCreateMediaType(&out_type)) || !out_type) return false;
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_OUT_SAMPLE_RATE);
        out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AUDIO_OUT_CHANNELS);
        out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbps AAC-LC
        out_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        out_type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

        if (FAILED(writer->AddStream(out_type.get(), &out_stream_idx))) return false;

        wil::com_ptr<IMFMediaType> in_type;
        if (FAILED(MFCreateMediaType(&in_type)) || !in_type) return false;
        const uint32_t block_align = AUDIO_OUT_CHANNELS * 2u;
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_OUT_SAMPLE_RATE);
        in_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AUDIO_OUT_CHANNELS);
        in_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
        in_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUDIO_OUT_SAMPLE_RATE * block_align);

        return SUCCEEDED(writer->SetInputMediaType(out_stream_idx, in_type.get(), nullptr));
    }

    static bool write_audio_chunk(IMFSinkWriter* writer, std::mutex& writer_mx, DWORD audio_stream_idx,
                                  const int16_t* pcm16, uint32_t frames, uint64_t sample_offset_frames) {
        const DWORD bytes = frames * AUDIO_OUT_CHANNELS * sizeof(int16_t);
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

        const LONGLONG ts_100ns = static_cast<LONGLONG>((sample_offset_frames * 10'000'000ull) / AUDIO_OUT_SAMPLE_RATE);
        const LONGLONG dur_100ns = static_cast<LONGLONG>((static_cast<uint64_t>(frames) * 10'000'000ull) / AUDIO_OUT_SAMPLE_RATE);
        sample->SetSampleTime(ts_100ns);
        sample->SetSampleDuration(dur_100ns);

        const std::scoped_lock lk{writer_mx};
        return SUCCEEDED(writer->WriteSample(audio_stream_idx, sample.get()));
    }

    RecordingResult run_worker(RECT region, uint32_t w, uint32_t h, const fs::path& output_path,
                               const RecordingConfig& cfg) {
        RecordingResult res;
        res.path = output_path;
        res.width = w;
        res.height = h;

        const uint32_t fps = cfg.fps;
        const uint32_t bitrate = cfg.bitrate_mbps * 1'000'000u;

        const ulp::win::COMWrapper<false> com{static_cast<DWORD>(COINIT_MULTITHREADED)};
        if (FAILED(MFStartup(MF_VERSION))) {
            res.error = "MFStartup failed";
            return res;
        }
        const auto mf_guard = wil::scope_exit([] { MFShutdown(); });

        std::error_code ec;
        if (output_path.has_parent_path()) fs::create_directories(output_path.parent_path(), ec);

        // Initialize requested WASAPI audio capture streams before creating the MP4 sink writer.
        WasapiCaptureStream desktop_cap;
        WasapiCaptureStream mic_cap;
        bool desktop_ok = cfg.record_desktop_audio && desktop_cap.open(AudioFlow::Render, cfg.desktop_device_id);
        bool mic_ok = cfg.record_mic_audio && mic_cap.open(AudioFlow::Capture, cfg.mic_device_id);
        bool has_audio = desktop_ok || mic_ok;

        wil::com_ptr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs, 2)) || !attrs) {
            res.error = "MFCreateAttributes failed";
            return res;
        }
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

        wil::com_ptr<IMFSinkWriter> writer;
        DWORD video_stream_idx = 0;
        DWORD audio_stream_idx = 0;

        auto init_writer = [&](bool with_audio) -> bool {
            writer.reset();
            if (FAILED(MFCreateSinkWriterFromURL(output_path.c_str(), nullptr, attrs.get(), &writer)) || !writer) {
                res.error = "Failed to create Media Foundation MP4 sink writer";
                return false;
            }
            if (!configure_video_stream(writer.get(), w, h, fps, bitrate, video_stream_idx, res.error)) {
                return false;
            }
            if (with_audio && !configure_audio_stream(writer.get(), audio_stream_idx)) {
                return false;
            }
            return true;
        };

        if (has_audio) {
            if (!init_writer(true)) {
                // Gracefully fall back to video-only if AAC stream setup fails on this system.
                desktop_cap.close();
                mic_cap.close();
                desktop_ok = false;
                mic_ok = false;
                has_audio = false;
                res.error.clear();
                if (!init_writer(false)) return res;
            }
        } else {
            if (!init_writer(false)) return res;
        }

        desktop_audio_active_.store(desktop_ok, std::memory_order_release);
        mic_audio_active_.store(mic_ok, std::memory_order_release);

        if (FAILED(writer->BeginWriting())) {
            res.error = "IMFSinkWriter::BeginWriting failed";
            return res;
        }

        // Persistent GDI capture DIB section (top-down: biHeight = -h to match positive MF_MT_DEFAULT_STRIDE, zero per-frame allocations)
        const wil::unique_hdc_window screen_dc{GetDC(nullptr)};
        const wil::unique_hdc mem_dc{screen_dc ? CreateCompatibleDC(screen_dc.get()) : nullptr};
        if (!screen_dc || !mem_dc) {
            res.error = "Failed to create capture DC";
            return res;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(w);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(h); // negative = top-down RGB32
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        const wil::unique_hbitmap dib{CreateDIBSection(mem_dc.get(), &bmi, DIB_RGB_COLORS, &bits, nullptr, 0)};
        if (!dib || !bits) {
            res.error = "CreateDIBSection failed";
            return res;
        }
        const auto old_bmp = wil::SelectObject(mem_dc.get(), dib.get());

        if (desktop_ok && !desktop_cap.start()) desktop_ok = false;
        if (mic_ok && !mic_cap.start()) mic_ok = false;
        desktop_audio_active_.store(desktop_ok, std::memory_order_release);
        mic_audio_active_.store(mic_ok, std::memory_order_release);

        const auto t0 = std::chrono::steady_clock::now();
        std::mutex writer_mx;
        std::jthread audio_thread;

        if (has_audio) {
            audio_thread = std::jthread([&, t0, desktop_ok, mic_ok, audio_stream_idx] {
                const ulp::win::COMWrapper<false> audio_com{static_cast<DWORD>(COINIT_MULTITHREADED)};
                StereoFloatFifo desktop_fifo{AUDIO_OUT_SAMPLE_RATE};
                StereoFloatFifo mic_fifo{AUDIO_OUT_SAMPLE_RATE};
                std::array<float, AUDIO_CHUNK_FRAMES * AUDIO_OUT_CHANNELS> mix_a{};
                std::array<float, AUDIO_CHUNK_FRAMES * AUDIO_OUT_CHANNELS> mix_b{};
                std::array<int16_t, AUDIO_CHUNK_FRAMES * AUDIO_OUT_CHANNELS> pcm16{};

                constexpr uint64_t kJitterCushionFrames = 1440u; // 30 ms
                constexpr uint64_t kMaxLagBeforePad = 3840u;     // 80 ms
                constexpr size_t kMaxBacklogFrames = 7680u;      // 160 ms
                constexpr size_t kTrimTargetFrames = 1920u;      // 40 ms

                auto last_desktop_pkt = std::chrono::steady_clock::time_point{};
                auto last_mic_pkt = std::chrono::steady_clock::time_point{};
                uint64_t audio_frames_written = 0;

                const auto emit_one_chunk = [&] {
                    std::span<const float> sa{};
                    std::span<const float> sb{};
                    if (desktop_ok) {
                        const size_t got = desktop_fifo.pop_frames(mix_a.data(), AUDIO_CHUNK_FRAMES);
                        if (got < AUDIO_CHUNK_FRAMES) {
                            std::fill(mix_a.begin() + static_cast<ptrdiff_t>(got * AUDIO_OUT_CHANNELS), mix_a.end(), 0.0f);
                        }
                        sa = mix_a;
                    }
                    if (mic_ok) {
                        const size_t got = mic_fifo.pop_frames(mix_b.data(), AUDIO_CHUNK_FRAMES);
                        if (got < AUDIO_CHUNK_FRAMES) {
                            std::fill(mix_b.begin() + static_cast<ptrdiff_t>(got * AUDIO_OUT_CHANNELS), mix_b.end(), 0.0f);
                        }
                        sb = mix_b;
                    }
                    mix_stereo_to_pcm16(sa, sb, pcm16);
                    (void)write_audio_chunk(writer.get(), writer_mx, audio_stream_idx, pcm16.data(), AUDIO_CHUNK_FRAMES, audio_frames_written);
                    audio_frames_written += AUDIO_CHUNK_FRAMES;
                };

                while (!stop_requested_.load(std::memory_order_acquire)) {
                    const auto now = std::chrono::steady_clock::now();
                    if (desktop_ok && desktop_cap.drain_into(desktop_fifo)) last_desktop_pkt = now;
                    if (mic_ok && mic_cap.drain_into(mic_fifo)) last_mic_pkt = now;
                    desktop_fifo.trim_excess(kMaxBacklogFrames, kTrimTargetFrames);
                    mic_fifo.trim_excess(kMaxBacklogFrames, kTrimTargetFrames);

                    const auto elapsed_us = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count());
                    const uint64_t target_frames = (static_cast<uint64_t>(elapsed_us) * AUDIO_OUT_SAMPLE_RATE) / 1'000'000ull;
                    const uint64_t emit_limit = (target_frames > kJitterCushionFrames) ? (target_frames - kJitterCushionFrames) : 0ull;

                    while (audio_frames_written + AUDIO_CHUNK_FRAMES <= emit_limit) {
                        const bool desktop_recent = desktop_ok && last_desktop_pkt.time_since_epoch().count() != 0 &&
                                                    (now - last_desktop_pkt) < std::chrono::milliseconds{50};
                        const bool mic_recent = mic_ok && last_mic_pkt.time_since_epoch().count() != 0 &&
                                                (now - last_mic_pkt) < std::chrono::milliseconds{50};
                        const bool desktop_waiting = desktop_recent && desktop_fifo.frames_available() < AUDIO_CHUNK_FRAMES;
                        const bool mic_waiting = mic_recent && mic_fifo.frames_available() < AUDIO_CHUNK_FRAMES;
                        if ((desktop_waiting || mic_waiting) && (target_frames - audio_frames_written) < kMaxLagBeforePad) {
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
                    AUDIO_CHUNK_FRAMES,
                    (static_cast<uint64_t>(end_us) * AUDIO_OUT_SAMPLE_RATE) / 1'000'000ull);
                while (audio_frames_written + AUDIO_CHUNK_FRAMES <= final_target) {
                    emit_one_chunk();
                }
                if (audio_frames_written == 0) {
                    emit_one_chunk();
                }
            });
        }

        const DWORD frame_bytes = w * h * 4u;
        const LONGLONG frame_dur_100ns = 10'000'000ll / static_cast<LONGLONG>(fps);
        const auto frame_period = std::chrono::microseconds{1'000'000 / fps};
        auto next_tick = t0;
        uint64_t frames_written = 0;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            const LONGLONG ts_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count() / 100ll;

            BitBlt(mem_dc.get(), 0, 0, static_cast<int>(w), static_cast<int>(h),
                   screen_dc.get(), region.left, region.top, SRCCOPY | CAPTUREBLT);
            if (cfg.include_cursor) {
                draw_cursor_into_dc(mem_dc.get(), region.left, region.top);
            }
            GdiFlush();

            // Capture first frame (already top-down BGRA) for instant corner thumbnail toast
            if (!res.first_frame) {
                ui::Raster thumb;
                thumb.w = w;
                thumb.h = h;
                thumb.pixels.resize(frame_bytes);
                std::memcpy(thumb.pixels.data(), bits, frame_bytes);
                const size_t pixel_count = static_cast<size_t>(w) * h;
                auto* px32 = reinterpret_cast<uint32_t*>(thumb.pixels.data());
                for (size_t i = 0; i < pixel_count; ++i) px32[i] |= 0xFF000000u;
                res.first_frame = std::move(thumb);
            }

            wil::com_ptr<IMFMediaBuffer> buf;
            wil::com_ptr<IMFSample> sample;
            if (SUCCEEDED(MFCreateMemoryBuffer(frame_bytes, &buf)) && buf &&
                SUCCEEDED(MFCreateSample(&sample)) && sample) {
                BYTE* dst_ptr = nullptr;
                if (SUCCEEDED(buf->Lock(&dst_ptr, nullptr, nullptr)) && dst_ptr) {
                    std::memcpy(dst_ptr, bits, frame_bytes);
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

        if (frames_written > 0 && SUCCEEDED(writer->Finalize())) {
            res.size_bytes = fs::file_size(output_path, ec);
            if (ec) res.size_bytes = 0;
            res.ok = true;
        } else {
            writer.reset();
            fs::remove(output_path, ec);
            res.error = frames_written == 0 ? "No video frames were captured" : "Failed to finalize MP4 file";
        }
        return res;
    }

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> desktop_audio_active_{false};
    std::atomic<bool> mic_audio_active_{false};
    std::chrono::steady_clock::time_point start_time_{};
    RECT region_{};
    std::jthread worker_;
};

} // namespace capusy
