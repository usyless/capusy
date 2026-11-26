#pragma once

#include "canvas.hpp"
#include "shared.hpp"
#include "widgets.hpp"

#include <capusy/recorder.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace capusy::ui {

/// Subtle click-through red recording border + floating REC timer/Stop pill.
/// Both windows use `WDA_EXCLUDEFROMCAPTURE` so they never appear inside the recorded video.
class RecordingOverlay {
public:
    explicit RecordingOverlay(Shared& sh) : sh_{sh} {}
    RecordingOverlay(const RecordingOverlay&) = delete;
    RecordingOverlay& operator=(const RecordingOverlay&) = delete;
    RecordingOverlay(RecordingOverlay&&) = delete;
    RecordingOverlay& operator=(RecordingOverlay&&) = delete;
    ~RecordingOverlay() { hide(); }

    std::function<void()> on_stop_clicked;

    bool show(HINSTANCE inst, RECT region, const RegionRecorder* recorder) {
        hide();
        recorder_ = recorder;
        region_ = region;

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc_b{.cbSize = sizeof(wc_b)};
            wc_b.lpfnWndProc = &RecordingOverlay::border_wndproc;
            wc_b.hInstance = inst;
            wc_b.lpszClassName = REC_BORDER_CLASS;
            RegisterClassExW(&wc_b);

            WNDCLASSEXW wc_p{.cbSize = sizeof(wc_p)};
            wc_p.lpfnWndProc = &RecordingOverlay::pill_wndproc;
            wc_p.hInstance = inst;
            wc_p.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc_p.lpszClassName = REC_PILL_CLASS;
            RegisterClassExW(&wc_p);
            registered = true;
        }

        if (!sh_.gfx.ensure_device(nullptr)) return false;

        // 1. Click-through border window wrapping the recorded region by 3px
        constexpr int pad = 3;
        const int bx = region.left - pad;
        const int by = region.top - pad;
        const int bw = (region.right - region.left) + (pad * 2);
        const int bh = (region.bottom - region.top) + (pad * 2);

        border_hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
            REC_BORDER_CLASS, L"capusy rec border", WS_POPUP,
            bx, by, bw, bh, nullptr, nullptr, inst, this);
        if (border_hwnd_) {
            exclude_window_from_capture(border_hwnd_, true);
            if (border_target_.attach(sh_.gfx, border_hwnd_)) {
                render_border();
                ShowWindow(border_hwnd_, SW_SHOWNOACTIVATE);
            }
        }

        // 2. Floating REC + Stop pill placed below or above the region (never steals focus)
        const HMONITOR mon = MonitorFromRect(&region, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{.cbSize = sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        UINT dx = 96;
        UINT dy = 96;
        if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
        const float s = static_cast<float>(dx) / 96.0f;

        const int pw = static_cast<int>(std::lround(192.0f * s));
        const int ph = static_cast<int>(std::lround(40.0f * s));
        int px = ((region.left + region.right) - pw) / 2;
        int py = region.bottom + static_cast<int>(std::lround(10.0f * s));
        if (py + ph > mi.rcWork.bottom - 8) py = region.top - ph - static_cast<int>(std::lround(10.0f * s));
        px = std::clamp<int>(px, mi.rcWork.left + 8, std::max<int>(mi.rcWork.left + 8, mi.rcWork.right - pw - 8));
        py = std::clamp<int>(py, mi.rcWork.top + 8, std::max<int>(mi.rcWork.top + 8, mi.rcWork.bottom - ph - 8));

        pill_hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            REC_PILL_CLASS, L"capusy rec pill", WS_POPUP,
            px, py, pw, ph, nullptr, nullptr, inst, this);
        if (!pill_hwnd_) return false;
        exclude_window_from_capture(pill_hwnd_, true);
        if (!pill_target_.attach(sh_.gfx, pill_hwnd_)) {
            hide();
            return false;
        }
        render_pill();
        ShowWindow(pill_hwnd_, SW_SHOWNOACTIVATE);
        SetTimer(pill_hwnd_, 1, 250, nullptr);
        return true;
    }

    void hide() noexcept {
        if (pill_hwnd_) KillTimer(pill_hwnd_, 1);
        border_target_.release();
        pill_target_.release();
        if (border_hwnd_) { DestroyWindow(border_hwnd_); border_hwnd_ = nullptr; }
        if (pill_hwnd_) { DestroyWindow(pill_hwnd_); pill_hwnd_ = nullptr; }
        recorder_ = nullptr;
        hover_stop_ = false;
    }

    [[nodiscard]] bool visible() const noexcept { return pill_hwnd_ != nullptr; }

private:
    void render_border() {
        if (!border_target_.ready()) return;
        std::vector<Hit> unused;
        (void)border_target_.render_frame(true, {}, WindowTarget::PresentMode::Immediate, [&](std::optional<D2D1_RECT_F> clip, bool) {
            Canvas cv{border_target_, sh_.fonts, unused, default_theme(), clip};
            const D2D1_SIZE_F sz = border_target_.size();
            cv.stroke(D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), hex(0xff4d4f, 0.92f), 2.5f);
        });
    }

    void render_pill() {
        if (!pill_target_.ready()) return;
        (void)pill_target_.render_frame(true, {}, WindowTarget::PresentMode::Immediate, [&](std::optional<D2D1_RECT_F> clip, bool) {
            pill_hits_.clear();
            Canvas cv{pill_target_, sh_.fonts, pill_hits_, default_theme(), clip};
            const D2D1_SIZE_F sz = pill_target_.size();
            const D2D1_RECT_F r = D2D1::RectF(1.0f, 1.0f, sz.width - 1.0f, sz.height - 1.0f);
            cv.fill_round(r, 12.0f, with_alpha(cv.theme().surface_lowest, 0.94f));
            cv.stroke_round(r, 12.0f, with_alpha(hex(0xff5c5c), 0.70f), 1.2f);

            const uint64_t ms = recorder_ ? recorder_->elapsed_ms() : 0;
            const bool blink = ((ms / 500) % 2) == 0;
            cv.fill_circle({.x = 18.0f, .y = sz.height * 0.5f}, 4.5f, with_alpha(hex(0xff5c5c), blink ? 1.0f : 0.35f));

            FixedWString<32> lbl{L"REC  "};
            lbl.append(format_duration_ms(ms));
            cv.text(lbl, rect(30.0f, 0.0f, 82.0f, sz.height), Font::MonoSmall, cv.theme().on_surface);

            draw_button(cv, hover_stop_ ? 1.0f : 0.0f, rect(sz.width - 74.0f, 6.0f, 66.0f, sz.height - 12.0f),
                        glyph::stop, L"Stop", 1, 0, 2);
        });
    }

    static LRESULT CALLBACK border_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT) {
            if (auto* self = reinterpret_cast<RecordingOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                self->render_border();
            }
            ValidateRect(hwnd, nullptr);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK pill_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<RecordingOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_ERASEBKGND: return 1;
            case WM_TIMER:
                self->render_pill();
                return 0;
            case WM_PAINT:
                self->render_pill();
                ValidateRect(hwnd, nullptr);
                return 0;
            case WM_MOUSEMOVE: {
                const float inv_s = 1.0f / self->pill_target_.scale();
                const D2D1_POINT_2F p{.x = GET_X_LPARAM(lp) * inv_s, .y = GET_Y_LPARAM(lp) * inv_s};
                const bool hov = hit_test(self->pill_hits_, p) != nullptr;
                if (hov != self->hover_stop_) {
                    self->hover_stop_ = hov;
                    self->render_pill();
                }
                TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (self->hover_stop_) {
                    self->hover_stop_ = false;
                    self->render_pill();
                }
                return 0;
            case WM_LBUTTONDOWN: {
                const float inv_s = 1.0f / self->pill_target_.scale();
                const D2D1_POINT_2F p{.x = GET_X_LPARAM(lp) * inv_s, .y = GET_Y_LPARAM(lp) * inv_s};
                if (hit_test(self->pill_hits_, p) && self->on_stop_clicked) {
                    self->on_stop_clicked();
                }
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    Shared& sh_;
    const RegionRecorder* recorder_ = nullptr;
    RECT region_{};
    HWND border_hwnd_ = nullptr;
    HWND pill_hwnd_ = nullptr;
    WindowTarget border_target_;
    WindowTarget pill_target_;
    std::vector<Hit> pill_hits_;
    bool hover_stop_ = false;
};

} // namespace capusy::ui
