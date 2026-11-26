#pragma once

#include "dirty_rects.hpp"

#include <capusy/common.hpp>

#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wil/com.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace capusy::ui {

/// Process-wide Direct3D11 / Direct2D 1.3 (ID2D1Factory7, ID2D1Device6) / DirectWrite 3 / WIC2 device.
/// Recreated wholesale on device loss or adapter changes; `epoch()` bumps so owners drop device-bound resources.
class Gfx {
public:
    bool init() {
        const D2D1_FACTORY_OPTIONS opts{};
        if (!d2d_factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory7), &opts, reinterpret_cast<void**>(&d2d_factory_)))) return false;
        if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(&dwrite_)))) return false;
        if (!wic_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)))) return false;
        return create_device(nullptr);
    }

    /// Drops the D3D/D2D GPU device when all windows are hidden to keep idle VRAM & RAM tiny.
    void release_device() noexcept {
        d2d_device_.reset();
        dxgi_device_.reset();
        if (d3d_ctx_) {
            d3d_ctx_->ClearState();
            d3d_ctx_->Flush();
        }
        d3d_ctx_.reset();
        d3d_.reset();
        dxgi_factory_.reset();
        ++epoch_;
        if (on_device_changed) on_device_changed();
    }

    bool ensure_device(HWND hint = nullptr) {
        if (ready() && !device_lost()) return true;
        if (!d2d_factory_ || !dwrite_ || !wic_) return init();
        return create_device(hint);
    }

    [[nodiscard]] ID2D1Factory7* d2d_factory() const noexcept { return d2d_factory_.get(); }
    [[nodiscard]] IDWriteFactory3* dwrite() const noexcept { return dwrite_.get(); }
    [[nodiscard]] IWICImagingFactory* wic() const noexcept { return wic_.get(); }
    [[nodiscard]] ID3D11Device* d3d() const noexcept { return d3d_.get(); }
    [[nodiscard]] ID3D11DeviceContext* d3d_context() const noexcept { return d3d_ctx_.get(); }
    [[nodiscard]] IDXGIFactory2* dxgi_factory() const noexcept { return dxgi_factory_.get(); }
    [[nodiscard]] IDXGIDevice* dxgi_device() const noexcept { return dxgi_device_.get(); }
    [[nodiscard]] ID2D1Device6* d2d_device() const noexcept { return d2d_device_.get(); }
    [[nodiscard]] uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] bool ready() const noexcept { return d2d_device_ != nullptr; }

    bool recover(HWND hint) { return create_device(hint); }

    [[nodiscard]] bool device_lost() const noexcept {
        return !d3d_ || FAILED(d3d_->GetDeviceRemovedReason());
    }

    [[nodiscard]] bool is_warp() const noexcept { return warp_; }

    bool refresh_dxgi_factory() noexcept {
        if (dxgi_factory_ && dxgi_factory_->IsCurrent()) return true;
        wil::com_ptr<IDXGIFactory2> fresh;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fresh))) || !fresh) return false;
        dxgi_factory_ = std::move(fresh);
        return true;
    }

    std::function<void()> on_device_changed;

private:
    static wil::com_ptr<IDXGIAdapter1> window_adapter(IDXGIFactory2* f, HWND hwnd) noexcept {
        if (!f || !hwnd) return nullptr;
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        for (UINT i = 0;; ++i) {
            wil::com_ptr<IDXGIAdapter1> a;
            if (FAILED(f->EnumAdapters1(i, &a)) || !a) break;
            for (UINT j = 0;; ++j) {
                wil::com_ptr<IDXGIOutput> o;
                if (FAILED(a->EnumOutputs(j, &o)) || !o) break;
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == mon) return a;
            }
        }
        return nullptr;
    }

    bool create_device(HWND hint) {
        d2d_device_.reset();
        dxgi_device_.reset();
        if (d3d_ctx_) {
            d3d_ctx_->ClearState();
            d3d_ctx_->Flush();
        }
        d3d_ctx_.reset();
        d3d_.reset();
        dxgi_factory_.reset();

        wil::com_ptr<IDXGIFactory2> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        wil::com_ptr<IDXGIAdapter1> adapter = window_adapter(factory.get(), hint);
        if (!adapter) factory->EnumAdapters1(0, &adapter);

        static constexpr std::array<D3D_FEATURE_LEVEL, 4> levels = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        };
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = E_FAIL;
        warp_ = false;
        if (adapter) hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &d3d_, nullptr, &d3d_ctx_);
        if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &d3d_, nullptr, &d3d_ctx_);
        if (FAILED(hr)) {
            warp_ = true;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &d3d_, nullptr, &d3d_ctx_);
        }
        if (FAILED(hr)) return false;

        if (const auto mt = d3d_.try_query<ID3D10Multithread>()) {
            mt->SetMultithreadProtected(TRUE);
        }

        if (FAILED(d3d_->QueryInterface(IID_PPV_ARGS(&dxgi_device_)))) return false;
        if (const auto d1 = dxgi_device_.try_query<IDXGIDevice1>()) d1->SetMaximumFrameLatency(1);
        dxgi_factory_ = std::move(factory);

        if (FAILED(d2d_factory_->CreateDevice(dxgi_device_.get(), &d2d_device_))) return false;
        d2d_device_->SetMaximumTextureMemory(32ull << 20u);
        ++epoch_;
        if (on_device_changed) on_device_changed();
        return true;
    }

    wil::com_ptr<ID2D1Factory7> d2d_factory_;
    wil::com_ptr<IDWriteFactory3> dwrite_;
    wil::com_ptr<IWICImagingFactory> wic_;
    wil::com_ptr<ID3D11Device> d3d_;
    wil::com_ptr<ID3D11DeviceContext> d3d_ctx_;
    wil::com_ptr<IDXGIFactory2> dxgi_factory_;
    wil::com_ptr<IDXGIDevice> dxgi_device_;
    wil::com_ptr<ID2D1Device6> d2d_device_;
    bool warp_ = false;
    uint64_t epoch_ = 0;
};

/// Per-window swapchain composed through DirectComposition (premultiplied alpha so DWM backdrops & transparency work cleanly).
class WindowTarget {
public:
    WindowTarget() = default;
    WindowTarget(const WindowTarget&) = delete;
    WindowTarget& operator=(const WindowTarget&) = delete;
    WindowTarget(WindowTarget&&) = delete;
    WindowTarget& operator=(WindowTarget&&) = delete;
    ~WindowTarget() { release(); }

    bool attach(Gfx& gfx, HWND hwnd) noexcept {
        gfx_ = &gfx;
        hwnd_ = hwnd;
        return rebuild();
    }

    bool rebuild() noexcept {
        if (!gfx_ || !hwnd_ || !gfx_->ensure_device(hwnd_)) {
            release();
            return false;
        }
        const bool keep_dcomp = (dcomp_ && dcomp_target_ && visual_ && epoch_ == gfx_->epoch());
        if (keep_dcomp) {
            release_swap_and_dc();
        } else {
            release();
        }
        if (FAILED(gfx_->d2d_device()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS, &dc_))) {
            release();
            return false;
        }

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        w_ = static_cast<UINT>(std::max(1L, rc.right - rc.left));
        h_ = static_cast<UINT>(std::max(1L, rc.bottom - rc.top));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = w_;
        desc.Height = h_;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (!gfx_->refresh_dxgi_factory()) {
            release();
            return false;
        }
        if (FAILED(gfx_->dxgi_factory()->CreateSwapChainForComposition(gfx_->d3d(), &desc, nullptr, &swap_))) {
            release();
            return false;
        }
        if (!keep_dcomp) {
            if (FAILED(DCompositionCreateDevice(gfx_->dxgi_device(), IID_PPV_ARGS(&dcomp_))) ||
                FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_)) ||
                FAILED(dcomp_->CreateVisual(&visual_)) ||
                FAILED(dcomp_target_->SetRoot(visual_.get()))) {
                release();
                return false;
            }
        }
        if (FAILED(visual_->SetContent(swap_.get()))) {
            release();
            return false;
        }
        dcomp_commit_pending_ = true;

        dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
        if (dpi_ <= 0.0f) dpi_ = 96.0f;
        if (!create_target()) {
            release();
            return false;
        }
        epoch_ = gfx_->epoch();
        return true;
    }

    void release() noexcept {
        release_swap_and_dc();
        dcomp_commit_pending_ = false;
        visual_.reset();
        dcomp_target_.reset();
        dcomp_.reset();
    }

    [[nodiscard]] bool ready() const noexcept { return dc_ && target_ && gfx_ && gfx_->ready() && epoch_ == gfx_->epoch(); }
    [[nodiscard]] ID2D1DeviceContext6* dc() const noexcept { return dc_.get(); }
    [[nodiscard]] float scale() const noexcept { return dpi_ / 96.0f; }
    [[nodiscard]] D2D1_SIZE_F size() const noexcept { return {.width = static_cast<float>(w_) / scale(), .height = static_cast<float>(h_) / scale()}; }
    [[nodiscard]] UINT pixel_width() const noexcept { return w_; }
    [[nodiscard]] UINT pixel_height() const noexcept { return h_; }
    [[nodiscard]] float dpi() const noexcept { return dpi_; }

    [[nodiscard]] ID2D1SolidColorBrush* solid_brush() noexcept {
        if (!solid_brush_ && dc_) {
            dc_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &solid_brush_);
        }
        return solid_brush_.get();
    }

    [[nodiscard]] ID2D1BitmapBrush1* bitmap_brush(ID2D1Bitmap1* bmp) noexcept {
        if (!bmp || !dc_) return nullptr;
        if (!bmp_brush_) {
            const auto bp = D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_LINEAR);
            dc_->CreateBitmapBrush(bmp, &bp, nullptr, &bmp_brush_);
        } else {
            bmp_brush_->SetBitmap(bmp);
        }
        return bmp_brush_.get();
    }

    [[nodiscard]] ID2D1Layer* clip_layer() noexcept {
        if (!clip_layer_ && dc_) dc_->CreateLayer(nullptr, &clip_layer_);
        return clip_layer_.get();
    }

    [[nodiscard]] ID2D1RoundedRectangleGeometry* round_clip_geom(D2D1_RECT_F r, float radius) noexcept {
        if (!gfx_ || !gfx_->d2d_factory()) return nullptr;
        const uint64_t ep = gfx_->epoch();
        if (clip_geom_ && clip_geom_epoch_ == ep && clip_radius_ == radius &&
            clip_rect_.left == r.left && clip_rect_.top == r.top &&
            clip_rect_.right == r.right && clip_rect_.bottom == r.bottom) {
            return clip_geom_.get();
        }
        clip_geom_.reset();
        if (FAILED(gfx_->d2d_factory()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &clip_geom_))) {
            return nullptr;
        }
        clip_rect_ = r;
        clip_radius_ = radius;
        clip_geom_epoch_ = ep;
        return clip_geom_.get();
    }

    bool resize(UINT w, UINT h) noexcept {
        if (w == 0 || h == 0) return ready();
        const auto dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        if (!ready()) return rebuild();
        if (w == w_ && h == h_ && dpi == dpi_) return true;
        dpi_ = dpi > 0.0f ? dpi : 96.0f;
        dc_->SetTarget(nullptr);
        target_.reset();
        if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return rebuild();
        w_ = w;
        h_ = h;
        dirty_tracker_.reset();
        return create_target();
    }

    bool sync_dpi() noexcept {
        if (!hwnd_ || !ready()) return ready();
        const auto dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        if (dpi <= 0.0f || dpi == dpi_) return true;
        dpi_ = dpi;
        dc_->SetTarget(nullptr);
        target_.reset();
        dirty_tracker_.reset();
        return create_target();
    }

    enum class PresentMode : uint8_t { Vsync, Immediate };

    template <typename DrawFn>
    bool render_frame(bool full_redraw, std::span<const D2D1_RECT_F> dirty_dips, PresentMode present_mode, DrawFn&& draw_fn) {
        if (!ready()) return false;
        const float s = scale();
        const UINT sync_interval = (present_mode == PresentMode::Vsync) ? 1u : 0u;

        curr_px_.clear();
        if (!full_redraw) {
            curr_px_.reserve(dirty_dips.size());
            for (const D2D1_RECT_F& r : dirty_dips) {
                const PixelRect px = dip_to_pixel_rect(r.left, r.top, r.right, r.bottom, s, w_, h_, 2.0f);
                if (!px.empty()) curr_px_.push_back(px);
            }
        }

        const FlipDirtyTracker::Plan p = dirty_tracker_.plan(full_redraw, curr_px_, w_, h_);
        if (p.skip) return true;

        present_rects_.clear();
        dc_->BeginDraw();
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        if (p.full) {
            dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
            draw_fn(std::nullopt, true);
        } else {
            present_rects_.reserve(p.render_rects.size());
            for (const PixelRect& pr : p.render_rects) {
                present_rects_.emplace_back(pr.left, pr.top, pr.right, pr.bottom);
                const D2D1_RECT_F clip = D2D1::RectF(
                    static_cast<float>(pr.left) / s,
                    static_cast<float>(pr.top) / s,
                    static_cast<float>(pr.right) / s,
                    static_cast<float>(pr.bottom) / s);
                dc_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
                draw_fn(clip, false);
                dc_->PopAxisAlignedClip();
            }
        }

        HRESULT hr = dc_->EndDraw();
        if (bmp_brush_) bmp_brush_->SetBitmap(nullptr);
        if (SUCCEEDED(hr)) {
            DXGI_PRESENT_PARAMETERS params{};
            if (!p.full) {
                params.DirtyRectsCount = static_cast<UINT>(present_rects_.size());
                params.pDirtyRects = present_rects_.data();
            }
            hr = swap_->Present1(sync_interval, 0, &params);
        }
        if (hr == DXGI_STATUS_OCCLUDED) {
            dirty_tracker_.reset();
            return true;
        }
        if (FAILED(hr)) return false;

        if (dcomp_commit_pending_ && dcomp_) {
            if (FAILED(dcomp_->Commit())) return false;
            dcomp_commit_pending_ = false;
        }

        dirty_tracker_.commit(full_redraw, p);
        return true;
    }

private:
    void release_swap_and_dc() noexcept {
        if (bmp_brush_) bmp_brush_->SetBitmap(nullptr);
        bmp_brush_.reset();
        solid_brush_.reset();
        clip_layer_.reset();
        clip_geom_.reset();
        clip_geom_epoch_ = 0;
        dirty_tracker_.reset();
        if (dc_) dc_->SetTarget(nullptr);
        target_.reset();
        swap_.reset();
        dc_.reset();
    }

    bool create_target() noexcept {
        wil::com_ptr<IDXGISurface> surface;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.get(), &props, &target_))) return false;
        dc_->SetTarget(target_.get());
        dc_->SetDpi(dpi_, dpi_);
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return true;
    }

    Gfx* gfx_ = nullptr;
    HWND hwnd_ = nullptr;
    wil::com_ptr<ID2D1DeviceContext6> dc_;
    wil::com_ptr<ID2D1Bitmap1> target_;
    wil::com_ptr<ID2D1SolidColorBrush> solid_brush_;
    wil::com_ptr<ID2D1BitmapBrush1> bmp_brush_;
    wil::com_ptr<ID2D1Layer> clip_layer_;
    wil::com_ptr<ID2D1RoundedRectangleGeometry> clip_geom_;
    D2D1_RECT_F clip_rect_{};
    float clip_radius_ = -1.0f;
    uint64_t clip_geom_epoch_ = 0;
    wil::com_ptr<IDXGISwapChain1> swap_;
    wil::com_ptr<IDCompositionDevice> dcomp_;
    wil::com_ptr<IDCompositionTarget> dcomp_target_;
    wil::com_ptr<IDCompositionVisual> visual_;
    UINT w_ = 0, h_ = 0;
    float dpi_ = 96.0f;
    uint64_t epoch_ = 0;
    FlipDirtyTracker dirty_tracker_;
    std::vector<PixelRect> curr_px_;
    std::vector<RECT> present_rects_;
    bool dcomp_commit_pending_ = false;
};

} // namespace capusy::ui
