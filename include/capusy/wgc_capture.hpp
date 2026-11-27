#pragma once

#include "common.hpp"
#include "ui/thumbs.hpp"

#include <usylibpp/ui/wgc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace capusy {

struct WgcMonitorRect {
    HMONITOR hmon = nullptr;
    RECT rect{}; // physical virtual-screen pixels
};

namespace wgc_detail = usylibpp::ui::wgc;

/// Captures all monitors in `monitors` in parallel using Windows.Graphics.Capture (borderless on Windows 11,
/// native DWM cursor & HDR-to-SDR tone mapping) and writes them into an opaque 32bpp BGRA raster spanning `bounds`.
/// Non-monitor dead space is guaranteed solid opaque black (0, 0, 0, 255).
inline std::optional<ui::Raster> wgc_capture_monitors_to_raster(
    std::span<const WgcMonitorRect> monitors, RECT bounds, bool include_cursor,
    ID3D11Device* shared_d3d = nullptr) {
    const int vw = bounds.right - bounds.left;
    const int vh = bounds.bottom - bounds.top;
    if (vw <= 0 || vh <= 0 || monitors.empty()) return std::nullopt;

    const wgc_detail::RoScope ro{RO_INIT_SINGLETHREADED};
    if (!ro.ok()) return std::nullopt;

    wgc_detail::WgcDevice dev;
    if (!dev.init(shared_d3d, false)) return std::nullopt;

    struct Slot {
        WgcMonitorRect mon{};
        wgc_detail::MonitorCaptureSession session;
        bool done = false;
    };
    std::vector<std::unique_ptr<Slot>> slots;
    slots.reserve(monitors.size());

    for (const WgcMonitorRect& m : monitors) {
        if (!m.hmon || m.rect.right <= m.rect.left || m.rect.bottom <= m.rect.top) continue;
        auto s = std::make_unique<Slot>();
        s->mon = m;
        if (s->session.start(dev.winrt_dev.get(), m.hmon, include_cursor, 1, 0)) {
            slots.push_back(std::move(s));
        }
    }
    if (slots.empty()) return std::nullopt;

    ui::Raster out;
    out.w = static_cast<UINT>(vw);
    out.h = static_cast<UINT>(vh);
    const size_t pixel_count = static_cast<size_t>(vw) * static_cast<size_t>(vh);
    out.pixels.resize(pixel_count * 4u);
    auto* dst32_all = reinterpret_cast<uint32_t*>(out.pixels.data());
    std::fill(dst32_all, dst32_all + pixel_count, 0xFF000000u);

    wil::com_ptr<ID3D11Texture2D> staging;
    UINT staging_w = 0;
    UINT staging_h = 0;

    auto ensure_staging = [&](UINT w, UINT h) noexcept -> bool {
        if (staging && staging_w >= w && staging_h >= h) return true;
        staging.reset();
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = w;
        sd.Height = h;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev.d3d->CreateTexture2D(&sd, nullptr, &staging)) || !staging) return false;
        staging_w = w;
        staging_h = h;
        return true;
    };

    const ULONGLONG deadline = GetTickCount64() + 1000u;
    size_t completed = 0;
    std::vector<HANDLE> wait_handles;
    wait_handles.reserve(slots.size());

    while (completed < slots.size()) {
        for (auto& s : slots) {
            if (s->done) continue;
            const bool got = s->session.consume_latest_frame([&](ID3D11Texture2D* tex, ABI::Windows::Graphics::SizeInt32 content_sz) noexcept {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                const UINT avail_w = (content_sz.Width > 0) ? std::min<UINT>(td.Width, static_cast<UINT>(content_sz.Width)) : td.Width;
                const UINT avail_h = (content_sz.Height > 0) ? std::min<UINT>(td.Height, static_cast<UINT>(content_sz.Height)) : td.Height;
                const int mw = s->mon.rect.right - s->mon.rect.left;
                const int mh = s->mon.rect.bottom - s->mon.rect.top;
                const int dst_x = s->mon.rect.left - bounds.left;
                const int dst_y = s->mon.rect.top - bounds.top;
                if (dst_x < 0 || dst_y < 0 || mw <= 0 || mh <= 0) return;

                const UINT cw = std::min<UINT>({avail_w, static_cast<UINT>(mw), static_cast<UINT>(vw - dst_x)});
                const UINT ch = std::min<UINT>({avail_h, static_cast<UINT>(mh), static_cast<UINT>(vh - dst_y)});
                if (cw == 0 || ch == 0 || !ensure_staging(cw, ch)) return;

                const D3D11_BOX box{.left = 0, .top = 0, .front = 0, .right = cw, .bottom = ch, .back = 1};
                dev.ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex, 0, &box);

                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(dev.ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                    const auto* src_base = static_cast<const uint8_t*>(mapped.pData);
                    const size_t dst_stride = static_cast<size_t>(vw) * 4u;
                    for (UINT y = 0; y < ch; ++y) {
                        const auto* srow = reinterpret_cast<const uint32_t*>(src_base + (static_cast<size_t>(y) * mapped.RowPitch));
                        auto* drow = reinterpret_cast<uint32_t*>(
                            out.pixels.data() + (static_cast<size_t>(dst_y + static_cast<int>(y)) * dst_stride) +
                            (static_cast<size_t>(dst_x) * 4u));
                        for (UINT x = 0; x < cw; ++x) {
                            drow[x] = srow[x] | 0xFF000000u;
                        }
                    }
                    dev.ctx->Unmap(staging.get(), 0);
                }
            });
            if (got) {
                s->done = true;
                s->session.stop();
                ++completed;
            }
        }
        if (completed >= slots.size()) break;

        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;

        wait_handles.clear();
        for (const auto& s : slots) {
            if (!s->done) {
                if (HANDLE h = s->session.event_handle()) wait_handles.push_back(h);
            }
        }
        if (wait_handles.empty()) break;

        const DWORD wait_ms = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 100u));
        const DWORD count = static_cast<DWORD>(std::min<size_t>(wait_handles.size(), MAXIMUM_WAIT_OBJECTS));
        (void)WaitForMultipleObjects(count, wait_handles.data(), FALSE, wait_ms);
    }

    for (const auto& s : slots) s->session.stop();
    slots.clear();
    staging.reset();
    dev.reset();
    if (completed == 0) return std::nullopt;
    return out;
}

/// Continuous multi-monitor Windows.Graphics.Capture + D3D11 GPU crop/scale/color-conversion surface
/// for zero-copy hardware Media Foundation recording (BGRA -> NV12) and fast staging readback (SW/WMV/GIF).
class WgcRegionSurface {
public:
    WgcRegionSurface() = default;
    WgcRegionSurface(const WgcRegionSurface&) = delete;
    WgcRegionSurface& operator=(const WgcRegionSurface&) = delete;
    WgcRegionSurface(WgcRegionSurface&&) = delete;
    WgcRegionSurface& operator=(WgcRegionSurface&&) = delete;
    ~WgcRegionSurface() { shutdown(); }

    bool init(RECT region, std::span<const WgcMonitorRect> all_monitors,
              uint32_t src_w, uint32_t src_h, uint32_t out_w, uint32_t out_h,
              bool include_cursor, uint32_t target_fps, std::string& err) {
        shutdown();
        region_ = region;
        src_w_ = src_w;
        src_h_ = src_h;
        out_w_ = out_w;
        out_h_ = out_h;
        scaled_ = (src_w_ != out_w_ || src_h_ != out_h_);

        if (!dev_.init(nullptr, true)) {
            err = "Failed to initialize Direct3D 11 device for Windows.Graphics.Capture";
            return false;
        }

        // Composite source texture matching the exact selected virtual-screen region.
        D3D11_TEXTURE2D_DESC src_desc{};
        src_desc.Width = src_w_;
        src_desc.Height = src_h_;
        src_desc.MipLevels = 1;
        src_desc.ArraySize = 1;
        src_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        src_desc.SampleDesc.Count = 1;
        src_desc.Usage = D3D11_USAGE_DEFAULT;
        src_desc.BindFlags = static_cast<UINT>(D3D11_BIND_RENDER_TARGET) | static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE);
        if (FAILED(dev_.d3d->CreateTexture2D(&src_desc, nullptr, &src_tex_)) || !src_tex_) {
            err = "Failed to create WGC composite source texture";
            return false;
        }

        // Clear dead-space pixels to opaque black once.
        wil::com_ptr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(dev_.d3d->CreateRenderTargetView(src_tex_.get(), nullptr, &rtv)) && rtv) {
            constexpr std::array<float, 4> black = {0.0f, 0.0f, 0.0f, 1.0f};
            dev_.ctx->ClearRenderTargetView(rtv.get(), black.data());
        }

        // On Win11 24H2+, avoid waking DWM 240x/sec when recording at low FPS (e.g. 15-30 FPS).
        const int64_t min_interval_100ns = (target_fps > 0 && target_fps <= 30u)
            ? (10'000'000ll / static_cast<int64_t>(target_fps * 2u))
            : 0ll;

        for (const WgcMonitorRect& m : all_monitors) {
            if (!m.hmon) continue;
            const RECT isect{
                .left = std::max(region_.left, m.rect.left),
                .top = std::max(region_.top, m.rect.top),
                .right = std::min(region_.right, m.rect.right),
                .bottom = std::min(region_.bottom, m.rect.bottom),
            };
            if (isect.right <= isect.left || isect.bottom <= isect.top) continue;

            auto st = std::make_unique<MonitorStream>();
            st->src_x = static_cast<UINT>(isect.left - m.rect.left);
            st->src_y = static_cast<UINT>(isect.top - m.rect.top);
            st->dst_x = static_cast<UINT>(isect.left - region_.left);
            st->dst_y = static_cast<UINT>(isect.top - region_.top);
            st->copy_w = static_cast<UINT>(isect.right - isect.left);
            st->copy_h = static_cast<UINT>(isect.bottom - isect.top);

            if (st->session.start(dev_.winrt_dev.get(), m.hmon, include_cursor, 2, min_interval_100ns)) {
                streams_.push_back(std::move(st));
            }
        }

        if (streams_.empty()) {
            err = "Failed to start Windows.Graphics.Capture session for recording region";
            shutdown();
            return false;
        }
        return true;
    }

    void shutdown() noexcept {
        for (const auto& st : streams_) st->session.stop();
        streams_.clear();
        nv12_pool_.clear();
        vp_nv12_.reset();
        vp_nv12_enum_.reset();
        vp_nv12_in_view_.reset();
        vp_bgra_.reset();
        vp_bgra_enum_.reset();
        vp_bgra_in_view_.reset();
        vp_bgra_out_view_.reset();
        scaled_bgra_tex_.reset();
        staging_tex_.reset();
        video_ctx_.reset();
        video_dev_.reset();
        src_tex_.reset();
        std::vector<uint8_t>{}.swap(cpu_buf_);
        dev_.reset();
    }

    [[nodiscard]] ID3D11Device* d3d_device() const noexcept { return dev_.d3d.get(); }
    [[nodiscard]] DWORD frame_bytes() const noexcept { return out_w_ * out_h_ * 4u; }
    [[nodiscard]] const void* cpu_bits() const noexcept { return cpu_buf_.data(); }

    /// Waits briefly for each intersecting monitor to deliver its initial desktop frame so frame 0 is never black.
    void wait_initial_frames(const std::atomic<bool>& stop_requested, DWORD timeout_ms = 600u) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        std::vector<HANDLE> handles;
        handles.reserve(streams_.size());

        while (!stop_requested.load(std::memory_order_acquire)) {
            size_t ready_count = 0;
            for (const auto& st : streams_) {
                if (drain_stream(*st)) st->has_first_frame = true;
                if (st->has_first_frame) ++ready_count;
            }
            if (ready_count >= streams_.size()) break;

            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;

            handles.clear();
            for (const auto& st : streams_) {
                if (!st->has_first_frame) {
                    if (HANDLE h = st->session.event_handle()) handles.push_back(h);
                }
            }
            if (handles.empty()) break;

            const DWORD rem = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 50u));
            const DWORD cnt = static_cast<DWORD>(std::min<size_t>(handles.size(), MAXIMUM_WAIT_OBJECTS));
            (void)WaitForMultipleObjects(cnt, handles.data(), FALSE, rem);
        }
    }

    /// Drains any newly arrived frames from all monitor capture sessions onto `src_tex_` on the GPU.
    void update_from_monitors() noexcept {
        for (const auto& st : streams_) {
            (void)drain_stream(*st);
        }
    }

    /// Initializes the single-pass GPU BGRA -> NV12 + scaling VideoProcessor pipeline for zero-copy HW MF encoding.
    bool init_nv12_pipeline() {
        if (vp_nv12_) return true;
        if (!ensure_video_interfaces()) return false;

        if (!create_video_processor(DXGI_FORMAT_NV12, vp_nv12_enum_, vp_nv12_, vp_nv12_in_view_, true)) {
            return false;
        }

        // Pre-allocate a small ring of NV12 textures so steady-state recording has zero GPU allocations.
        nv12_pool_.clear();
        nv12_pool_.reserve(8);
        for (int i = 0; i < 6; ++i) {
            if (!add_nv12_pool_slot()) break;
        }
        return !nv12_pool_.empty();
    }

    /// Runs GPU BGRA -> NV12 (plus scaling if `scale_pct < 100`) and returns a free pooled `ID3D11Texture2D`.
    [[nodiscard]] wil::com_ptr<ID3D11Texture2D> render_nv12_frame() {
        if (!vp_nv12_ || !video_ctx_ || !vp_nv12_in_view_) return nullptr;

        const Nv12Slot* slot = acquire_free_nv12_slot();
        if (!slot) return nullptr;

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = vp_nv12_in_view_.get();

        if (FAILED(video_ctx_->VideoProcessorBlt(vp_nv12_.get(), slot->out_view.get(), 0, 1, &stream))) {
            return nullptr;
        }
        return slot->tex;
    }

    /// Ensures CPU BGRA staging pipeline is ready (uses GPU VideoProcessor scaling when `scaled_`, with CPU fallback).
    bool init_cpu_pipeline(std::string& err) {
        if (staging_tex_ && cpu_buf_.size() == frame_bytes()) return true;

        if (scaled_ && !vp_bgra_) {
            if (ensure_video_interfaces() &&
                create_video_processor(DXGI_FORMAT_B8G8R8A8_UNORM, vp_bgra_enum_, vp_bgra_, vp_bgra_in_view_, false)) {
                D3D11_TEXTURE2D_DESC od{};
                od.Width = out_w_;
                od.Height = out_h_;
                od.MipLevels = 1;
                od.ArraySize = 1;
                od.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                od.SampleDesc.Count = 1;
                od.Usage = D3D11_USAGE_DEFAULT;
                od.BindFlags = D3D11_BIND_RENDER_TARGET;
                if (SUCCEEDED(dev_.d3d->CreateTexture2D(&od, nullptr, &scaled_bgra_tex_)) && scaled_bgra_tex_) {
                    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
                    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                    if (FAILED(video_dev_->CreateVideoProcessorOutputView(
                            scaled_bgra_tex_.get(), vp_bgra_enum_.get(), &ovd, &vp_bgra_out_view_))) {
                        scaled_bgra_tex_.reset();
                        vp_bgra_.reset();
                        vp_bgra_enum_.reset();
                        vp_bgra_in_view_.reset();
                    }
                }
            }
        }

        const bool gpu_scaled = scaled_ && scaled_bgra_tex_ && vp_bgra_ && vp_bgra_out_view_;
        const UINT stg_w = (!scaled_ || gpu_scaled) ? out_w_ : src_w_;
        const UINT stg_h = (!scaled_ || gpu_scaled) ? out_h_ : src_h_;

        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = stg_w;
        sd.Height = stg_h;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_.d3d->CreateTexture2D(&sd, nullptr, &staging_tex_)) || !staging_tex_) {
            err = "Failed to create D3D11 staging texture";
            return false;
        }
        staging_uses_cpu_scale_ = scaled_ && !gpu_scaled;
        cpu_buf_.resize(frame_bytes());
        return true;
    }

    /// Reads the current composed GPU frame into `cpu_bits()` (opaque 32bpp top-down BGRA, `out_w × out_h`).
    bool readback_cpu_bgra() noexcept {
        if (!staging_tex_ || cpu_buf_.size() != frame_bytes()) return false;

        ID3D11Texture2D* read_src = src_tex_.get();
        if (scaled_ && !staging_uses_cpu_scale_ && vp_bgra_ && video_ctx_ && vp_bgra_in_view_ && vp_bgra_out_view_) {
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = vp_bgra_in_view_.get();
            if (SUCCEEDED(video_ctx_->VideoProcessorBlt(vp_bgra_.get(), vp_bgra_out_view_.get(), 0, 1, &stream))) {
                read_src = scaled_bgra_tex_.get();
            }
        }

        dev_.ctx->CopyResource(staging_tex_.get(), read_src);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(dev_.ctx->Map(staging_tex_.get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
            return false;
        }
        const auto unmap_guard = wil::scope_exit([&] { dev_.ctx->Unmap(staging_tex_.get(), 0); });

        const auto* src_base = static_cast<const uint8_t*>(mapped.pData);
        if (staging_uses_cpu_scale_) {
            wgc_detail::cpu_scale_bgra_bilinear(src_base, src_w_, src_h_, mapped.RowPitch,
                                                cpu_buf_.data(), out_w_, out_h_);
        } else {
            const size_t row_bytes = static_cast<size_t>(out_w_) * 4u;
            if (mapped.RowPitch == row_bytes) {
                std::memcpy(cpu_buf_.data(), src_base, frame_bytes());
            } else {
                for (UINT y = 0; y < out_h_; ++y) {
                    std::memcpy(cpu_buf_.data() + (static_cast<size_t>(y) * row_bytes),
                                src_base + (static_cast<size_t>(y) * mapped.RowPitch),
                                row_bytes);
                }
            }
        }
        return true;
    }

    /// Captures a one-shot `ui::Raster` thumbnail (`out_w × out_h`, opaque BGRA) from the current GPU frame.
    /// Works in both the zero-copy NV12 path (without keeping a permanent staging buffer) and the CPU readback path.
    [[nodiscard]] std::optional<ui::Raster> copy_first_frame() {
        if (staging_tex_ && cpu_buf_.size() == frame_bytes()) {
            ui::Raster thumb;
            thumb.w = out_w_;
            thumb.h = out_h_;
            thumb.pixels = cpu_buf_;
            force_opaque(thumb);
            return thumb;
        }

        // One-shot readback of `src_tex_` for HW NV12 mode (keeps zero steady-state staging RAM).
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = src_w_;
        sd.Height = src_h_;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        wil::com_ptr<ID3D11Texture2D> tmp;
        if (FAILED(dev_.d3d->CreateTexture2D(&sd, nullptr, &tmp)) || !tmp) return std::nullopt;

        dev_.ctx->CopyResource(tmp.get(), src_tex_.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(dev_.ctx->Map(tmp.get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) return std::nullopt;
        const auto unmap_guard = wil::scope_exit([&] { dev_.ctx->Unmap(tmp.get(), 0); });

        ui::Raster thumb;
        thumb.w = out_w_;
        thumb.h = out_h_;
        thumb.pixels.resize(frame_bytes());
        const auto* src_base = static_cast<const uint8_t*>(mapped.pData);
        if (scaled_) {
            wgc_detail::cpu_scale_bgra_bilinear(src_base, src_w_, src_h_, mapped.RowPitch,
                                                thumb.pixels.data(), out_w_, out_h_);
        } else {
            const size_t row_bytes = static_cast<size_t>(out_w_) * 4u;
            for (UINT y = 0; y < out_h_; ++y) {
                std::memcpy(thumb.pixels.data() + (static_cast<size_t>(y) * row_bytes),
                            src_base + (static_cast<size_t>(y) * mapped.RowPitch),
                            row_bytes);
            }
        }
        force_opaque(thumb);
        return thumb;
    }

private:
    struct MonitorStream {
        wgc_detail::MonitorCaptureSession session;
        UINT src_x = 0;
        UINT src_y = 0;
        UINT dst_x = 0;
        UINT dst_y = 0;
        UINT copy_w = 0;
        UINT copy_h = 0;
        bool has_first_frame = false;
    };

    struct Nv12Slot {
        wil::com_ptr<ID3D11Texture2D> tex;
        wil::com_ptr<ID3D11VideoProcessorOutputView> out_view;
        ULONG idle_refs = 2;
    };

    static void force_opaque(ui::Raster& r) noexcept {
        const size_t px_count = static_cast<size_t>(r.w) * r.h;
        auto* px32 = reinterpret_cast<uint32_t*>(r.pixels.data());
        for (size_t i = 0; i < px_count; ++i) px32[i] |= 0xFF000000u;
    }

    static ULONG current_ref_count(IUnknown* p) noexcept {
        if (!p) return 0;
        p->AddRef();
        return p->Release();
    }

    bool drain_stream(MonitorStream& st) noexcept {
        return st.session.consume_latest_frame([&](ID3D11Texture2D* tex, ABI::Windows::Graphics::SizeInt32 content_sz) noexcept {
            wgc_detail::gpu_copy_clamped(dev_.ctx.get(), src_tex_.get(), src_w_, src_h_,
                                         st.dst_x, st.dst_y, tex, content_sz,
                                         st.src_x, st.src_y, st.copy_w, st.copy_h);
        });
    }

    bool ensure_video_interfaces() noexcept {
        if (video_dev_ && video_ctx_) return true;
        if (!dev_.d3d || !dev_.ctx) return false;
        video_dev_ = dev_.d3d.try_query<ID3D11VideoDevice>();
        video_ctx_ = dev_.ctx.try_query<ID3D11VideoContext>();
        return video_dev_ && video_ctx_;
    }

    bool create_video_processor(DXGI_FORMAT out_fmt,
                                wil::com_ptr<ID3D11VideoProcessorEnumerator>& out_enum,
                                wil::com_ptr<ID3D11VideoProcessor>& out_vp,
                                wil::com_ptr<ID3D11VideoProcessorInputView>& out_in_view,
                                bool is_yuv_output) noexcept {
        out_enum.reset();
        out_vp.reset();
        out_in_view.reset();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = src_w_;
        desc.InputHeight = src_h_;
        desc.OutputWidth = out_w_;
        desc.OutputHeight = out_h_;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        if (FAILED(video_dev_->CreateVideoProcessorEnumerator(&desc, &out_enum)) || !out_enum) {
            return false;
        }
        UINT in_sup = 0;
        UINT out_sup = 0;
        if (FAILED(out_enum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &in_sup)) ||
            (in_sup & static_cast<UINT>(D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) == 0u ||
            FAILED(out_enum->CheckVideoProcessorFormat(out_fmt, &out_sup)) ||
            (out_sup & static_cast<UINT>(D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) == 0u) {
            out_enum.reset();
            return false;
        }

        if (FAILED(video_dev_->CreateVideoProcessor(out_enum.get(), 0, &out_vp)) || !out_vp) {
            out_enum.reset();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = 0;
        if (FAILED(video_dev_->CreateVideoProcessorInputView(src_tex_.get(), out_enum.get(), &ivd, &out_in_view)) || !out_in_view) {
            out_vp.reset();
            out_enum.reset();
            return false;
        }

        // Disable automatic sharpening/denoise filters so crisp desktop text stays untouched.
        video_ctx_->VideoProcessorSetStreamAutoProcessingMode(out_vp.get(), 0, FALSE);

        const RECT src_rect{.left = 0, .top = 0, .right = static_cast<LONG>(src_w_), .bottom = static_cast<LONG>(src_h_)};
        const RECT dst_rect{.left = 0, .top = 0, .right = static_cast<LONG>(out_w_), .bottom = static_cast<LONG>(out_h_)};
        video_ctx_->VideoProcessorSetStreamSourceRect(out_vp.get(), 0, TRUE, &src_rect);
        video_ctx_->VideoProcessorSetStreamDestRect(out_vp.get(), 0, TRUE, &dst_rect);
        video_ctx_->VideoProcessorSetOutputTargetRect(out_vp.get(), TRUE, &dst_rect);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
        in_cs.Usage = 0;
        in_cs.RGB_Range = 0;    // Full range sRGB 0..255
        in_cs.YCbCr_Matrix = 1; // BT.709
        in_cs.YCbCr_xvYCC = 0;
        in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        video_ctx_->VideoProcessorSetStreamColorSpace(out_vp.get(), 0, &in_cs);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
        out_cs.Usage = 0;
        out_cs.RGB_Range = 0;
        out_cs.YCbCr_Matrix = 1; // BT.709
        out_cs.YCbCr_xvYCC = 0;
        out_cs.Nominal_Range = is_yuv_output
            ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
            : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        video_ctx_->VideoProcessorSetOutputColorSpace(out_vp.get(), &out_cs);
        return true;
    }

    bool add_nv12_pool_slot() {
        if (!vp_nv12_enum_ || !video_dev_) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = out_w_;
        td.Height = out_h_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;

        Nv12Slot slot;
        if (FAILED(dev_.d3d->CreateTexture2D(&td, nullptr, &slot.tex)) || !slot.tex) return false;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(video_dev_->CreateVideoProcessorOutputView(
                slot.tex.get(), vp_nv12_enum_.get(), &ovd, &slot.out_view)) || !slot.out_view) {
            return false;
        }
        slot.idle_refs = current_ref_count(slot.tex.get());
        nv12_pool_.push_back(std::move(slot));
        return true;
    }

    Nv12Slot* acquire_free_nv12_slot() {
        const size_t n = nv12_pool_.size();
        for (size_t attempt = 0; attempt < n; ++attempt) {
            const size_t idx = (nv12_cursor_ + attempt) % n;
            Nv12Slot& s = nv12_pool_[idx];
            if (current_ref_count(s.tex.get()) <= s.idle_refs) {
                nv12_cursor_ = (idx + 1u) % n;
                return &s;
            }
        }
        if (nv12_pool_.size() < 16u && add_nv12_pool_slot()) {
            nv12_cursor_ = 0;
            return &nv12_pool_.back();
        }
        if (n == 0) return nullptr;
        Nv12Slot& fallback = nv12_pool_[nv12_cursor_];
        nv12_cursor_ = (nv12_cursor_ + 1u) % n;
        return &fallback;
    }

    RECT region_{};
    uint32_t src_w_ = 0;
    uint32_t src_h_ = 0;
    uint32_t out_w_ = 0;
    uint32_t out_h_ = 0;
    bool scaled_ = false;
    bool staging_uses_cpu_scale_ = false;

    wgc_detail::WgcDevice dev_;
    wil::com_ptr<ID3D11Texture2D> src_tex_;
    std::vector<std::unique_ptr<MonitorStream>> streams_;

    wil::com_ptr<ID3D11VideoDevice> video_dev_;
    wil::com_ptr<ID3D11VideoContext> video_ctx_;

    wil::com_ptr<ID3D11VideoProcessorEnumerator> vp_nv12_enum_;
    wil::com_ptr<ID3D11VideoProcessor> vp_nv12_;
    wil::com_ptr<ID3D11VideoProcessorInputView> vp_nv12_in_view_;
    std::vector<Nv12Slot> nv12_pool_;
    size_t nv12_cursor_ = 0;

    wil::com_ptr<ID3D11VideoProcessorEnumerator> vp_bgra_enum_;
    wil::com_ptr<ID3D11VideoProcessor> vp_bgra_;
    wil::com_ptr<ID3D11VideoProcessorInputView> vp_bgra_in_view_;
    wil::com_ptr<ID3D11VideoProcessorOutputView> vp_bgra_out_view_;
    wil::com_ptr<ID3D11Texture2D> scaled_bgra_tex_;

    wil::com_ptr<ID3D11Texture2D> staging_tex_;
    std::vector<uint8_t> cpu_buf_;
};

} // namespace capusy
