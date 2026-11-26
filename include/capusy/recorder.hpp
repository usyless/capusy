#pragma once

#include "capture.hpp"
#include "common.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace capusy {

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

/// Native Windows Media Foundation H.264 MP4 region screen recorder.
/// Zero external binaries; captures directly from a persistent DIB section into IMFSinkWriter.
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
    [[nodiscard]] RECT region() const noexcept { return region_; }

    [[nodiscard]] uint64_t elapsed_ms() const noexcept {
        if (!active_.load(std::memory_order_acquire)) return 0;
        const auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count());
    }

    bool start(RECT region, const fs::path& output_path, uint32_t fps = 30, uint32_t bitrate_mbps = 12, bool include_cursor = true) {
        if (active_.load(std::memory_order_acquire)) return false;
        stop_sync();

        // H.264 requires even width and even height (>= 16x16).
        uint32_t w = static_cast<uint32_t>(std::max<LONG>(0, region.right - region.left)) & ~1u;
        uint32_t h = static_cast<uint32_t>(std::max<LONG>(0, region.bottom - region.top)) & ~1u;
        if (w < 16u) w = 16u;
        if (h < 16u) h = 16u;
        region_ = RECT{.left = region.left, .top = region.top, .right = region.left + static_cast<LONG>(w), .bottom = region.top + static_cast<LONG>(h)};
        fps = (fps == 60u) ? 60u : 30u;
        const uint32_t bitrate = std::clamp(bitrate_mbps, 2u, 50u) * 1'000'000u;

        stop_requested_.store(false, std::memory_order_release);
        active_.store(true, std::memory_order_release);
        start_time_ = std::chrono::steady_clock::now();

        worker_ = std::jthread([this, r = region_, w, h, output_path, fps, bitrate, include_cursor] {
            RecordingResult res = run_worker(r, w, h, output_path, fps, bitrate, include_cursor);
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
        active_.store(false, std::memory_order_release);
    }

private:
    RecordingResult run_worker(RECT region, uint32_t w, uint32_t h, const fs::path& output_path,
                               uint32_t fps, uint32_t bitrate, bool include_cursor) {
        RecordingResult res;
        res.path = output_path;
        res.width = w;
        res.height = h;

        const ulp::win::COMWrapper<false> com{static_cast<DWORD>(COINIT_MULTITHREADED)};
        if (FAILED(MFStartup(MF_VERSION))) {
            res.error = "MFStartup failed";
            return res;
        }
        const auto mf_guard = wil::scope_exit([] { MFShutdown(); });

        std::error_code ec;
        if (output_path.has_parent_path()) fs::create_directories(output_path.parent_path(), ec);

        wil::com_ptr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs, 2)) || !attrs) {
            res.error = "MFCreateAttributes failed";
            return res;
        }
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

        wil::com_ptr<IMFSinkWriter> writer;
        if (FAILED(MFCreateSinkWriterFromURL(output_path.c_str(), nullptr, attrs.get(), &writer)) || !writer) {
            res.error = "Failed to create Media Foundation MP4 sink writer";
            return res;
        }

        // Output stream: H.264
        wil::com_ptr<IMFMediaType> out_type;
        if (FAILED(MFCreateMediaType(&out_type)) || !out_type) {
            res.error = "MFCreateMediaType (output) failed";
            return res;
        }
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out_type.get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(out_type.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        DWORD stream_idx = 0;
        if (FAILED(writer->AddStream(out_type.get(), &stream_idx))) {
            res.error = "Failed to add H.264 output stream";
            return res;
        }

        // Input stream: bottom-up 32bpp RGB32 (universally accepted by all HW & SW Windows H.264 MFTs)
        wil::com_ptr<IMFMediaType> in_type;
        if (FAILED(MFCreateMediaType(&in_type)) || !in_type) {
            res.error = "MFCreateMediaType (input) failed";
            return res;
        }
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in_type->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4u);
        MFSetAttributeSize(in_type.get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in_type.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(in_type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (FAILED(writer->SetInputMediaType(stream_idx, in_type.get(), nullptr))) {
            res.error = "Failed to configure RGB32 input media type";
            return res;
        }
        if (FAILED(writer->BeginWriting())) {
            res.error = "IMFSinkWriter::BeginWriting failed";
            return res;
        }

        // Persistent GDI capture DIB section (bottom-up: biHeight = +h, zero per-frame allocations)
        const wil::unique_hdc_window screen_dc{GetDC(nullptr)};
        const wil::unique_hdc mem_dc{screen_dc ? CreateCompatibleDC(screen_dc.get()) : nullptr};
        if (!screen_dc || !mem_dc) {
            res.error = "Failed to create capture DC";
            return res;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(w);
        bmi.bmiHeader.biHeight = static_cast<LONG>(h); // positive = bottom-up RGB32
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

        const DWORD frame_bytes = w * h * 4u;
        const LONGLONG frame_dur_100ns = 10'000'000ll / static_cast<LONGLONG>(fps);
        const auto frame_period = std::chrono::microseconds{1'000'000 / fps};
        const auto t0 = std::chrono::steady_clock::now();
        auto next_tick = t0;
        uint64_t frames_written = 0;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            const LONGLONG ts_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count() / 100ll;

            BitBlt(mem_dc.get(), 0, 0, static_cast<int>(w), static_cast<int>(h),
                   screen_dc.get(), region.left, region.top, SRCCOPY | CAPTUREBLT);
            if (include_cursor) {
                draw_cursor_into_dc(mem_dc.get(), region.left, region.top);
            }
            GdiFlush();

            // Capture first frame (flipped to top-down BGRA) for instant corner thumbnail toast
            if (!res.first_frame) {
                ui::Raster thumb;
                thumb.w = w;
                thumb.h = h;
                thumb.pixels.resize(frame_bytes);
                const size_t stride = static_cast<size_t>(w) * 4u;
                for (uint32_t y = 0; y < h; ++y) {
                    const uint8_t* src = static_cast<const uint8_t*>(bits) + (static_cast<size_t>(h - 1 - y) * stride);
                    uint8_t* dst = thumb.pixels.data() + (static_cast<size_t>(y) * stride);
                    std::memcpy(dst, src, stride);
                    for (uint32_t x = 0; x < w; ++x) dst[(x * 4u) + 3u] = 255;
                }
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
                    if (SUCCEEDED(writer->WriteSample(stream_idx, sample.get()))) {
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
    std::chrono::steady_clock::time_point start_time_{};
    RECT region_{};
    std::jthread worker_;
};

} // namespace capusy
