#pragma once

#include "canvas.hpp"
#include "shared.hpp"

#include <capusy/color_logic.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>

namespace capusy::ui {

/// Non-activating corner preview thumbnail on the primary monitor.
/// Never steals focus, never blocks user input elsewhere, excludes itself from screen capture,
/// supports configurable width & auto-dismiss timeout, and releases GPU resources when hidden.
class ToastWindow {
public:
    explicit ToastWindow(Shared& sh) : sh_{sh} {}
    ToastWindow(const ToastWindow&) = delete;
    ToastWindow& operator=(const ToastWindow&) = delete;
    ToastWindow(ToastWindow&&) = delete;
    ToastWindow& operator=(ToastWindow&&) = delete;
    ~ToastWindow() {
        hide_immediate();
        if (hwnd_) DestroyWindow(hwnd_);
    }

    std::function<void(const std::wstring&)> on_clicked;

    bool create(HINSTANCE inst) {
        WNDCLASSEXW wc{.cbSize = sizeof(wc)};
        wc.lpfnWndProc = &ToastWindow::wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = TOAST_CLASS;
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            TOAST_CLASS, L"capusy preview", WS_POPUP,
            0, 0, 10, 10, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        exclude_window_from_capture(hwnd_, true);
        cloak_window(hwnd_, true);
        return true;
    }

    [[nodiscard]] bool visible() const noexcept { return shown_; }
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

    void show_capture(const std::wstring& path, const Raster* raster, uint32_t cap_w, uint32_t cap_h,
                      bool is_video, bool copied, std::wstring_view video_badge = L"MP4") {
        if (!hwnd_ || !sh_.settings.show_corner_toast) return;
        if (!sh_.gfx.ensure_device(hwnd_)) return;

        color_mode_ = false;
        path_ = path;
        is_video_ = is_video;
        video_badge_.assign(video_badge.empty() ? L"MP4" : video_badge);
        copied_ = copied;
        meta_w_ = cap_w;
        meta_h_ = cap_h;

        // Position on the PRIMARY monitor's bottom-right work area
        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{.cbSize = sizeof(mi)};
        GetMonitorInfoW(primary, &mi);
        UINT dx = 96;
        UINT dy = 96;
        if (FAILED(GetDpiForMonitor(primary, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
        const float s = static_cast<float>(dx) / 96.0f;

        const float w_dip = std::clamp(sh_.settings.toast_width_dip, 160.0f, 420.0f);
        const float aspect = (cap_w > 0 && cap_h > 0) ? (static_cast<float>(cap_h) / static_cast<float>(cap_w)) : 0.5625f;
        const float img_h_dip = std::clamp(w_dip * aspect, 84.0f, w_dip * 0.72f);
        const float h_dip = img_h_dip + 34.0f;

        const int px_w = static_cast<int>(std::lround(w_dip * s));
        const int px_h = static_cast<int>(std::lround(h_dip * s));
        const int margin = static_cast<int>(std::lround(16.0f * s));
        const int x = mi.rcWork.right - px_w - margin;
        const int y = mi.rcWork.bottom - px_h - margin;

        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, px_w, px_h, SWP_NOACTIVATE);
        if (!target_.ready()) {
            if (!target_.attach(sh_.gfx, hwnd_)) {
                if (sh_.on_visibility_changed) sh_.on_visibility_changed();
                return;
            }
        } else {
            (void)target_.resize(static_cast<UINT>(px_w), static_cast<UINT>(px_h));
        }

        // Upload thumbnail bitmap (downscaled if huge so VRAM footprint stays tiny)
        thumb_bmp_.reset();
        if (raster && raster->w > 0 && raster->h > 0 && raster->pixels.size() >= static_cast<size_t>(raster->w) * raster->h * 4u) {
            const uint32_t max_side = static_cast<uint32_t>(std::max<int>(px_w, px_h));
            if (raster->w <= max_side * 2u && raster->h <= max_side * 2u) {
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
                (void)target_.dc()->CreateBitmap(D2D1::SizeU(raster->w, raster->h), raster->pixels.data(), raster->w * 4u, &props, &thumb_bmp_);
            } else {
                // Fast box downsample on CPU for giant multi-monitor rasters
                const auto [sw, sh_px] = fit_scaled_size(raster->w, raster->h, std::max<uint32_t>(max_side, 512u));
                std::vector<uint8_t> small_px(static_cast<size_t>(sw) * sh_px * 4u);
                for (uint32_t yy = 0; yy < sh_px; ++yy) {
                    const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(yy) * raster->h) / sh_px);
                    const uint8_t* srow = raster->pixels.data() + (static_cast<size_t>(sy) * raster->w * 4u);
                    uint8_t* drow = small_px.data() + (static_cast<size_t>(yy) * sw * 4u);
                    for (uint32_t xx = 0; xx < sw; ++xx) {
                        const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(xx) * raster->w) / sw);
                        std::memcpy(drow + (static_cast<size_t>(xx) * 4u), srow + (static_cast<size_t>(sx) * 4u), 4u);
                    }
                }
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
                (void)target_.dc()->CreateBitmap(D2D1::SizeU(sw, sh_px), small_px.data(), sw * 4u, &props, &thumb_bmp_);
            }
        }

        shown_ = true;
        fading_out_ = false;
        hovered_ = false;
        elapsed_s_ = 0.0f;
        timeout_s_ = std::clamp(sh_.settings.toast_timeout_s, 1.0f, 15.0f);
        if (opacity_.value < 0.05f) opacity_.snap(0.0f);
        opacity_.set(1.0f, true);
        last_tick_ = std::chrono::steady_clock::now();
        full_dirty_ = true;
        last_drawn_alpha_ = -1.0f;
        last_drawn_bar_w_ = -1.0f;

        render();
        cloak_window(hwnd_, false);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, px_w, px_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(hwnd_, TIMER_ANIM, 16, nullptr);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void show_color(Rgb8 rgb, std::wstring_view hex_str, std::wstring_view rgb_str, bool copied) {
        if (!hwnd_ || !sh_.settings.show_corner_toast) return;
        if (!sh_.gfx.ensure_device(hwnd_)) return;

        color_mode_ = true;
        picked_rgb_ = rgb;
        color_hex_.assign(hex_str);
        color_rgb_.assign(rgb_str);
        path_.clear();
        is_video_ = false;
        copied_ = copied;
        thumb_bmp_.reset();

        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{.cbSize = sizeof(mi)};
        GetMonitorInfoW(primary, &mi);
        UINT dx = 96;
        UINT dy = 96;
        if (FAILED(GetDpiForMonitor(primary, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
        const float s = static_cast<float>(dx) / 96.0f;

        const float w_dip = std::clamp(sh_.settings.toast_width_dip, 210.0f, 320.0f);
        constexpr float h_dip = 70.0f;
        const int px_w = static_cast<int>(std::lround(w_dip * s));
        const int px_h = static_cast<int>(std::lround(h_dip * s));
        const int margin = static_cast<int>(std::lround(16.0f * s));
        const int x = mi.rcWork.right - px_w - margin;
        const int y = mi.rcWork.bottom - px_h - margin;

        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, px_w, px_h, SWP_NOACTIVATE);
        if (!target_.ready()) {
            if (!target_.attach(sh_.gfx, hwnd_)) {
                if (sh_.on_visibility_changed) sh_.on_visibility_changed();
                return;
            }
        } else {
            (void)target_.resize(static_cast<UINT>(px_w), static_cast<UINT>(px_h));
        }

        shown_ = true;
        fading_out_ = false;
        hovered_ = false;
        elapsed_s_ = 0.0f;
        timeout_s_ = std::clamp(sh_.settings.toast_timeout_s, 1.0f, 15.0f);
        if (opacity_.value < 0.05f) opacity_.snap(0.0f);
        opacity_.set(1.0f, true);
        last_tick_ = std::chrono::steady_clock::now();
        full_dirty_ = true;
        last_drawn_alpha_ = -1.0f;
        last_drawn_bar_w_ = -1.0f;

        render();
        cloak_window(hwnd_, false);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, px_w, px_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(hwnd_, TIMER_ANIM, 16, nullptr);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void hide_immediate() noexcept {
        if (!hwnd_) return;
        const bool was = shown_;
        KillTimer(hwnd_, TIMER_ANIM);
        shown_ = false;
        fading_out_ = false;
        hovered_ = false;
        opacity_.snap(0.0f);
        full_dirty_ = true;
        last_drawn_alpha_ = -1.0f;
        last_drawn_bar_w_ = -1.0f;
        path_.clear();
        path_.shrink_to_fit();
        thumb_bmp_.reset();
        target_.release();
        cloak_window(hwnd_, true);
        ShowWindow(hwnd_, SW_HIDE);
        if (was && sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void reset_gpu() noexcept {
        thumb_bmp_.reset();
        target_.release();
        full_dirty_ = true;
        last_drawn_alpha_ = -1.0f;
        last_drawn_bar_w_ = -1.0f;
    }

private:
    static constexpr UINT_PTR TIMER_ANIM = 1;

    bool step_anim() {
        if (!shown_) return false;
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_tick_).count(), 0.001f, 0.1f);
        last_tick_ = now;

        if (!hovered_ && !fading_out_) {
            elapsed_s_ += dt;
            if (elapsed_s_ >= timeout_s_) {
                fading_out_ = true;
                opacity_.set(0.0f, true);
            }
        }
        (void)opacity_.step(dt, fading_out_ ? 14.0f : 22.0f, 0.005f);
        if (fading_out_ && opacity_.value <= 0.01f) {
            hide_immediate();
            return false;
        }
        return shown_ && (!hovered_ || opacity_.active(0.005f));
    }

    void render(bool force_full = false) {
        if (!target_.ready()) return;
        const D2D1_SIZE_F sz = target_.size();
        const float alpha = std::clamp(opacity_.value, 0.0f, 1.0f);
        const float frac = std::clamp(1.0f - (elapsed_s_ / std::max(0.1f, timeout_s_)), 0.0f, 1.0f);
        const float bar_w = (sz.width - 20.0f) * frac;

        const bool need_full = force_full || full_dirty_ ||
                               std::abs(alpha - last_drawn_alpha_) > 0.001f ||
                               hovered_ != last_drawn_hovered_;
        if (!need_full && std::abs(bar_w - last_drawn_bar_w_) < 0.25f) {
            return;
        }

        const D2D1_RECT_F bar_dirty = D2D1::RectF(8.0f, sz.height - 8.0f, sz.width - 8.0f, sz.height - 1.0f);
        const std::span<const D2D1_RECT_F> dirty_span = need_full
            ? std::span<const D2D1_RECT_F>{}
            : std::span<const D2D1_RECT_F>{&bar_dirty, 1};

        std::vector<Hit> unused;
        const bool ok = target_.render_frame(need_full, dirty_span, WindowTarget::PresentMode::Immediate, [&](std::optional<D2D1_RECT_F> clip, bool) {
            Canvas cv{target_, sh_.fonts, unused, default_theme(), clip, alpha};
            const D2D1_RECT_F card = D2D1::RectF(1.0f, 1.0f, sz.width - 1.0f, sz.height - 1.0f);
            cv.fill_round(card, 12.0f, with_alpha(cv.theme().surface, 0.96f));
            cv.stroke_round(card, 12.0f, hovered_ ? cv.theme().primary : cv.theme().outline_variant, hovered_ ? 1.5f : 1.0f);

            if (color_mode_) {
                const D2D1_RECT_F sw = rect(10.0f, 10.0f, 44.0f, 44.0f);
                cv.fill_round(sw, 9.0f, hex(rgb_to_u32(picked_rgb_)));
                cv.stroke_round(sw, 9.0f, with_alpha(cv.theme().on_surface, 0.35f), 1.0f);

                const float tx = sw.right + 10.0f;
                const float tw = std::max(40.0f, sz.width - tx - 10.0f);
                FixedWString<48> line1{color_hex_.view()};
                if (copied_) line1.append(L"  •  Copied");
                cv.text(line1, rect(tx, 11.0f, tw, 22.0f), Font::BodySemi, cv.theme().on_surface);
                cv.text(color_rgb_, rect(tx, 33.0f, tw, 20.0f), Font::MonoSmall, cv.theme().on_surface_variant);
            } else {
                const D2D1_RECT_F img_r = D2D1::RectF(6.0f, 6.0f, sz.width - 6.0f, sz.height - 30.0f);
                cv.fill_round(img_r, 8.0f, cv.theme().surface_lowest);
                if (thumb_bmp_) {
                    cv.fill_bitmap_round(thumb_bmp_.get(), img_r, 8.0f, ThumbFit::Contain);
                } else {
                    cv.icon(is_video_ ? glyph::video : glyph::photo, img_r, cv.theme().primary, Font::IconLarge);
                }

                if (is_video_) {
                    const D2D1_RECT_F badge = rect(img_r.right - 46.0f, img_r.top + 6.0f, 40.0f, 18.0f);
                    cv.fill_round(badge, 5.0f, with_alpha(cv.theme().surface_lowest, 0.85f));
                    cv.text(video_badge_, badge, Font::Label, cv.theme().amber, Align::Center);
                }

                // Footer metadata
                FixedWString<64> meta;
                if (meta_w_ > 0 && meta_h_ > 0) {
                    meta.assign(to_fixed_wstr<16>(meta_w_));
                    meta.append(L"×");
                    meta.append(to_fixed_wstr<16>(meta_h_));
                } else {
                    meta.assign(L"Captured");
                }
                if (copied_) meta.append(L"  •  Copied");
                cv.text(meta, D2D1::RectF(10.0f, sz.height - 28.0f, sz.width - 10.0f, sz.height - 6.0f), Font::Caption, cv.theme().on_surface_variant);
            }

            // Auto-hide countdown bar
            if (bar_w > 1.0f) {
                cv.fill_round(rect(10.0f, sz.height - 5.0f, bar_w, 2.5f), 1.2f, with_alpha(cv.theme().primary, 0.85f));
            }
        });
        if (ok) {
            full_dirty_ = false;
            last_drawn_alpha_ = alpha;
            last_drawn_hovered_ = hovered_;
            last_drawn_bar_w_ = bar_w;
        }
    }

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<ToastWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // NEVER steal focus!
            case WM_ERASEBKGND: return 1;
            case WM_TIMER:
                if (wp == TIMER_ANIM) {
                    const bool keep_going = self->step_anim();
                    if (!keep_going) {
                        KillTimer(hwnd, TIMER_ANIM);
                    }
                    if (self->shown_) {
                        self->render();
                    }
                }
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                if (self->shown_) self->render();
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_MOUSEMOVE:
                if (!self->hovered_) {
                    self->hovered_ = true;
                    self->full_dirty_ = true;
                    if (self->fading_out_) {
                        self->fading_out_ = false;
                        self->opacity_.set(1.0f, true);
                        SetTimer(hwnd, TIMER_ANIM, 16, nullptr);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
                    TrackMouseEvent(&tme);
                }
                return 0;
            case WM_MOUSELEAVE:
                self->hovered_ = false;
                self->full_dirty_ = true;
                self->last_tick_ = std::chrono::steady_clock::now();
                if (self->shown_) {
                    SetTimer(hwnd, TIMER_ANIM, 16, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP: {
                const std::wstring p = self->path_;
                self->hide_immediate();
                if (self->on_clicked && !p.empty()) self->on_clicked(p);
                return 0;
            }
            case WM_RBUTTONUP:
                self->hide_immediate();
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    Shared& sh_;
    HWND hwnd_ = nullptr;
    WindowTarget target_;
    wil::com_ptr<ID2D1Bitmap1> thumb_bmp_;
    std::wstring path_;
    uint32_t meta_w_ = 0;
    uint32_t meta_h_ = 0;
    bool is_video_ = false;
    FixedWString<16> video_badge_{L"MP4"};
    bool color_mode_ = false;
    Rgb8 picked_rgb_{};
    FixedWString<16> color_hex_;
    FixedWString<32> color_rgb_;
    bool copied_ = false;
    bool shown_ = false;
    bool fading_out_ = false;
    bool hovered_ = false;
    float elapsed_s_ = 0.0f;
    float timeout_s_ = 4.0f;
    AnimFloat opacity_;
    std::chrono::steady_clock::time_point last_tick_{};
    bool full_dirty_ = true;
    float last_drawn_alpha_ = -1.0f;
    bool last_drawn_hovered_ = false;
    float last_drawn_bar_w_ = -1.0f;
};

} // namespace capusy::ui
