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

        if (!cursor_arrow_) cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        if (!cursor_hand_) cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);

        if (!sh_.gfx.ensure_device(nullptr)) return false;

        // 1. Click-through border window wrapping the recorded region by 3px.
        // WS_EX_LAYERED | WS_EX_TRANSPARENT is required for true cross-process mouse click-through.
        constexpr int pad = 3;
        const int bx = region.left - pad;
        const int by = region.top - pad;
        const int bw = (region.right - region.left) + (pad * 2);
        const int bh = (region.bottom - region.top) + (pad * 2);

        border_hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
            REC_BORDER_CLASS, L"capusy rec border", WS_POPUP,
            bx, by, bw, bh, nullptr, nullptr, inst, this);
        if (border_hwnd_) {
            SetLayeredWindowAttributes(border_hwnd_, 0, 255, LWA_ALPHA);
            exclude_window_from_capture(border_hwnd_, true);
            if (border_target_.attach(sh_.gfx, border_hwnd_)) {
                render_border();
                ShowWindow(border_hwnd_, SW_SHOWNOACTIVATE);
            } else {
                DestroyWindow(border_hwnd_);
                border_hwnd_ = nullptr;
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

        const bool desk_on = recorder_ && recorder_->desktop_audio_active();
        const bool mic_on = recorder_ && recorder_->mic_audio_active();
        const float extra_w = (desk_on ? 18.0f : 0.0f) + (mic_on ? 18.0f : 0.0f) + ((desk_on || mic_on) ? 6.0f : 0.0f);
        const int pw = static_cast<int>(std::lround((192.0f + extra_w) * s));
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
        if (!pill_hwnd_) {
            hide();
            return false;
        }
        exclude_window_from_capture(pill_hwnd_, true);
        if (!pill_target_.attach(sh_.gfx, pill_hwnd_)) {
            hide();
            return false;
        }
        pill_full_dirty_ = true;
        last_pill_sec_ = std::numeric_limits<uint64_t>::max();
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
        pill_hits_.clear();
        pill_hits_.shrink_to_fit();
        pill_scratch_hits_.clear();
        pill_scratch_hits_.shrink_to_fit();
        recorder_ = nullptr;
        hover_stop_ = false;
        pill_full_dirty_ = true;
        last_pill_sec_ = std::numeric_limits<uint64_t>::max();
    }

    void reset_gpu() noexcept {
        border_target_.release();
        pill_target_.release();
        pill_full_dirty_ = true;
        last_pill_sec_ = std::numeric_limits<uint64_t>::max();
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

    void render_pill(bool force_full = false) {
        if (!pill_target_.ready()) return;
        const D2D1_SIZE_F sz = pill_target_.size();
        const uint64_t ms = recorder_ ? recorder_->elapsed_ms() : 0;
        const uint64_t sec = ms / 1000u;
        const bool blink = ((ms / 500u) % 2u) == 0u;
        const bool need_full = force_full || pill_full_dirty_;

        std::array<D2D1_RECT_F, 3> dirty_rects{};
        size_t dirty_n = 0;
        if (!need_full) {
            if (blink != last_pill_blink_) {
                dirty_rects[dirty_n++] = rect(10.0f, 0.0f, 20.0f, sz.height);
            }
            if (sec != last_pill_sec_) {
                dirty_rects[dirty_n++] = rect(28.0f, 0.0f, 86.0f, sz.height);
            }
            if (hover_stop_ != last_pill_hover_) {
                dirty_rects[dirty_n++] = rect(sz.width - 76.0f, 4.0f, 70.0f, sz.height - 8.0f);
            }
            if (dirty_n == 0) return;
        }

        const std::span<const D2D1_RECT_F> dirty_span = need_full
            ? std::span<const D2D1_RECT_F>{}
            : std::span<const D2D1_RECT_F>{dirty_rects.data(), dirty_n};

        const bool ok = pill_target_.render_frame(need_full, dirty_span, WindowTarget::PresentMode::Immediate, [&](std::optional<D2D1_RECT_F> clip, bool full_frame) {
            std::vector<Hit>& target_hits = full_frame ? pill_hits_ : pill_scratch_hits_;
            target_hits.clear();
            Canvas cv{pill_target_, sh_.fonts, target_hits, default_theme(), clip};
            const D2D1_RECT_F r = D2D1::RectF(1.0f, 1.0f, sz.width - 1.0f, sz.height - 1.0f);
            cv.fill_round(r, 12.0f, with_alpha(cv.theme().surface_lowest, 0.94f));
            cv.stroke_round(r, 12.0f, with_alpha(hex(0xff5c5c), 0.70f), 1.2f);

            cv.fill_circle({.x = 18.0f, .y = sz.height * 0.5f}, 4.5f, with_alpha(hex(0xff5c5c), blink ? 1.0f : 0.35f));

            FixedWString<32> lbl{L"REC  "};
            lbl.append(format_duration_ms(ms));
            cv.text(lbl, rect(30.0f, 0.0f, 82.0f, sz.height), Font::MonoSmall, cv.theme().on_surface);

            float ix = 112.0f;
            const float stop_x = sz.width - 74.0f;
            if (recorder_ && recorder_->desktop_audio_active() && ix + 16.0f <= stop_x - 4.0f) {
                cv.icon(glyph::volume, rect(ix, 0.0f, 16.0f, sz.height), cv.theme().primary, Font::IconSmall);
                ix += 18.0f;
            }
            if (recorder_ && recorder_->mic_audio_active() && ix + 16.0f <= stop_x - 4.0f) {
                cv.icon(glyph::mic, rect(ix, 0.0f, 16.0f, sz.height), cv.theme().mint, Font::IconSmall);
            }

            draw_button(cv, hover_stop_ ? 1.0f : 0.0f, rect(stop_x, 6.0f, 66.0f, sz.height - 12.0f),
                        glyph::stop, L"Stop", 1, 0, 2);
        });
        if (ok) {
            pill_full_dirty_ = false;
            last_pill_sec_ = sec;
            last_pill_blink_ = blink;
            last_pill_hover_ = hover_stop_;
        }
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
                self->render_pill(true);
                ValidateRect(hwnd, nullptr);
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT pt{};
                    bool hov = self->hover_stop_;
                    if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                        const float inv_s = 1.0f / self->pill_target_.scale();
                        const D2D1_POINT_2F p{.x = static_cast<float>(pt.x) * inv_s, .y = static_cast<float>(pt.y) * inv_s};
                        hov = hit_test(self->pill_hits_, p) != nullptr;
                    }
                    SetCursor(hov ? self->cursor_hand_ : self->cursor_arrow_);
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE: {
                const float inv_s = 1.0f / self->pill_target_.scale();
                const D2D1_POINT_2F p{.x = GET_X_LPARAM(lp) * inv_s, .y = GET_Y_LPARAM(lp) * inv_s};
                const bool hov = hit_test(self->pill_hits_, p) != nullptr;
                SetCursor(hov ? self->cursor_hand_ : self->cursor_arrow_);
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
    std::vector<Hit> pill_scratch_hits_;
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    bool hover_stop_ = false;
    bool pill_full_dirty_ = true;
    uint64_t last_pill_sec_ = std::numeric_limits<uint64_t>::max();
    bool last_pill_blink_ = false;
    bool last_pill_hover_ = false;
};

} // namespace capusy::ui
