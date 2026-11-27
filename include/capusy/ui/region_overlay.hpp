#pragma once

#include "canvas.hpp"
#include "shared.hpp"
#include "widgets.hpp"

#include <capusy/audio_capture.hpp>
#include <capusy/capture.hpp>
#include <capusy/color_logic.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace capusy::ui {

enum class OverlayMode : uint8_t {
    Screenshot = 0,
    RecordRegion = 1,
    ColorPicker = 2,
};

inline constexpr float HIGHLIGHTER_OPACITY = 0.24f;

enum class DrawTool : uint8_t {
    Select = 0,
    Pen,
    Highlighter,
    Rect,
    Ellipse,
    Line,
    Arrow,
};

/// Vector annotation stored in global virtual-screen physical pixel coordinates.
struct Annotation {
    DrawTool tool = DrawTool::Pen;
    D2D1_COLOR_F color = hex(0xff5c5c);
    float thickness = 4.0f; // physical pixels
    std::vector<D2D1_POINT_2F> points; // freehand points (Pen / Highlighter) or {start, end} (shapes)
};

/// Renders a single annotation onto `dc` shifted by `(-offset_x, -offset_y)` and divided by `scale`
/// (pass `scale = 1.0f` for 1:1 physical export, or `monitor_dpi / 96.0f` for DPI-aware overlay display).
inline void render_annotation(ID2D1DeviceContext6* dc, ID2D1Factory* factory, ID2D1SolidColorBrush* brush,
                              ID2D1StrokeStyle* round_stroke, const Annotation& a,
                              float offset_x, float offset_y, float scale) noexcept {
    if (!dc || !brush || a.points.empty()) return;
    const float inv_s = scale > 0.0f ? (1.0f / scale) : 1.0f;
    const auto to_local = [&](D2D1_POINT_2F p) noexcept -> D2D1_POINT_2F {
        return {.x = (p.x - offset_x) * inv_s, .y = (p.y - offset_y) * inv_s};
    };

    const float w = std::max(1.0f, a.thickness * inv_s);
    D2D1_COLOR_F col = a.color;
    if (a.tool == DrawTool::Highlighter) {
        col.a = HIGHLIGHTER_OPACITY;
    }
    brush->SetColor(col);

    switch (a.tool) {
        case DrawTool::Pen:
        case DrawTool::Highlighter: {
            const float stroke_w = (a.tool == DrawTool::Highlighter) ? (w * 3.2f) : w;
            if (a.points.size() == 1) {
                const D2D1_POINT_2F p = to_local(a.points[0]);
                dc->FillEllipse(D2D1::Ellipse(p, stroke_w * 0.5f, stroke_w * 0.5f), brush);
                break;
            }
            // Build a single unified PathGeometry (with quadratic midpoint smoothing) so Direct2D
            // widens and composites the whole stroke once — eliminating overlapping-circle beads
            // on translucent highlighter strokes and jagged polyline corners.
            wil::com_ptr<ID2D1Factory> fallback_factory;
            ID2D1Factory* fac = factory;
            if (!fac) {
                dc->GetFactory(&fallback_factory);
                fac = fallback_factory.get();
            }
            wil::com_ptr<ID2D1PathGeometry> path;
            wil::com_ptr<ID2D1GeometrySink> sink;
            if (fac && SUCCEEDED(fac->CreatePathGeometry(&path)) && path && SUCCEEDED(path->Open(&sink)) && sink) {
                sink->BeginFigure(to_local(a.points[0]), D2D1_FIGURE_BEGIN_HOLLOW);
                if (a.points.size() == 2) {
                    sink->AddLine(to_local(a.points[1]));
                } else {
                    for (size_t i = 1; i + 1 < a.points.size(); ++i) {
                        const D2D1_POINT_2F cur = to_local(a.points[i]);
                        const D2D1_POINT_2F next = to_local(a.points[i + 1]);
                        const D2D1_POINT_2F mid{.x = (cur.x + next.x) * 0.5f, .y = (cur.y + next.y) * 0.5f};
                        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(cur, mid));
                    }
                    sink->AddLine(to_local(a.points.back()));
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close())) {
                    dc->DrawGeometry(path.get(), brush, stroke_w, round_stroke);
                    break;
                }
            }
            for (size_t i = 1; i < a.points.size(); ++i) {
                dc->DrawLine(to_local(a.points[i - 1]), to_local(a.points[i]), brush, stroke_w, round_stroke);
            }
            break;
        }
        case DrawTool::Rect: {
            if (a.points.size() < 2) break;
            const D2D1_POINT_2F p0 = to_local(a.points.front());
            const D2D1_POINT_2F p1 = to_local(a.points.back());
            const D2D1_RECT_F r = D2D1::RectF(std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x), std::max(p0.y, p1.y));
            dc->DrawRectangle(r, brush, w, round_stroke);
            break;
        }
        case DrawTool::Ellipse: {
            if (a.points.size() < 2) break;
            const D2D1_POINT_2F p0 = to_local(a.points.front());
            const D2D1_POINT_2F p1 = to_local(a.points.back());
            const D2D1_POINT_2F c{.x = (p0.x + p1.x) * 0.5f, .y = (p0.y + p1.y) * 0.5f};
            const float rx = std::abs(p1.x - p0.x) * 0.5f;
            const float ry = std::abs(p1.y - p0.y) * 0.5f;
            dc->DrawEllipse(D2D1::Ellipse(c, rx, ry), brush, w, round_stroke);
            break;
        }
        case DrawTool::Line:
        case DrawTool::Arrow: {
            if (a.points.size() < 2) break;
            const D2D1_POINT_2F p0 = to_local(a.points.front());
            const D2D1_POINT_2F p1 = to_local(a.points.back());
            dc->DrawLine(p0, p1, brush, w, round_stroke);
            if (a.tool == DrawTool::Arrow) {
                const float dx = p1.x - p0.x;
                const float dy = p1.y - p0.y;
                const float len = std::hypot(dx, dy);
                if (len > 4.0f) {
                    const float ux = dx / len;
                    const float uy = dy / len;
                    const float head = std::max(10.0f * inv_s, w * 3.4f);
                    const float wing = head * 0.48f;
                    const D2D1_POINT_2F left_pt{.x = p1.x - (ux * head) + (uy * wing), .y = p1.y - (uy * head) - (ux * wing)};
                    const D2D1_POINT_2F right_pt{.x = p1.x - (ux * head) - (uy * wing), .y = p1.y - (uy * head) + (ux * wing)};
                    dc->DrawLine(p1, left_pt, brush, w, round_stroke);
                    dc->DrawLine(p1, right_pt, brush, w, round_stroke);
                }
            }
            break;
        }
        case DrawTool::Select:
            break;
    }
}

/// Per-monitor frozen-frame region capture & annotation overlay.
/// Uses ONE borderless topmost Direct2D window per HMONITOR (instead of one giant SM_XVIRTUALSCREEN window)
/// so mixed-DPI monitors render at crisp native DPI, never exceed GPU texture limits, and avoid wasting VRAM
/// on dead space — while seamlessly supporting cross-monitor drag selection and drawing via shared virtual coordinates.
class RegionOverlay {
public:
    explicit RegionOverlay(Shared& sh) : sh_{sh} {}
    RegionOverlay(const RegionOverlay&) = delete;
    RegionOverlay& operator=(const RegionOverlay&) = delete;
    RegionOverlay(RegionOverlay&&) = delete;
    RegionOverlay& operator=(RegionOverlay&&) = delete;
    ~RegionOverlay() { cancel(); }

    std::function<void(Raster&&, WindowInfo&&)> on_screenshot_captured;
    std::function<void(RECT, WindowInfo&&)> on_record_region_selected;
    std::function<void(Rgb8)> on_color_picked;
    std::function<void()> on_closed;

    [[nodiscard]] bool active() const noexcept { return !monitors_.empty(); }
    [[nodiscard]] OverlayMode mode() const noexcept { return mode_; }

    bool start(HINSTANCE inst, OverlayMode mode, bool include_cursor) {
        cancel();
        inst_ = inst;
        mode_ = mode;

        // 1. Ensure shared D3D11 device is warm so WGC reuses it directly, then snapshot foreground window,
        //    instant freezeframe & Z-ordered snap targets BEFORE creating any overlay windows!
        if (!sh_.gfx.ensure_device(nullptr)) {
            return false;
        }
        foreground_info_ = (mode_ == OverlayMode::ColorPicker) ? WindowInfo{} : foreground_window_info();
        const bool snap_cursor = (mode_ == OverlayMode::ColorPicker) ? false : include_cursor;
        auto cap = capture_virtual_screen(snap_cursor, sh_.gfx.d3d());
        if (!cap || cap->monitors.empty()) return false;
        snaps_ = (mode_ == OverlayMode::ColorPicker) ? SnapTargets{} : enumerate_snap_targets(cap->monitors);
        capture_ = std::move(*cap);
        ensure_stroke_style();

        // Register window class once
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{.cbSize = sizeof(wc)};
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = &RegionOverlay::wndproc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
            wc.lpszClassName = REGION_OVERLAY_CLASS;
            RegisterClassExW(&wc);
            registered = true;
        }

        cursor_cross_ = LoadCursorW(nullptr, IDC_CROSS);
        cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
        cursor_move_ = LoadCursorW(nullptr, IDC_SIZEALL);
        cursor_nwse_ = LoadCursorW(nullptr, IDC_SIZENWSE);
        cursor_nesw_ = LoadCursorW(nullptr, IDC_SIZENESW);
        cursor_ns_ = LoadCursorW(nullptr, IDC_SIZENS);
        cursor_we_ = LoadCursorW(nullptr, IDC_SIZEWE);
        current_cursor_ = cursor_cross_;

        // Reset session state
        instant_capture_ = (mode_ == OverlayMode::Screenshot) && sh_.settings.region_instant_capture;
        open_dropdown_ = AudioDropdown::None;
        if (mode_ == OverlayMode::RecordRegion) {
            desktop_devices_ = enumerate_audio_devices(AudioFlow::Render);
            mic_devices_ = enumerate_audio_devices(AudioFlow::Capture);
        } else {
            desktop_devices_.clear();
            mic_devices_.clear();
        }
        has_selection_ = false;
        selection_ = {};
        hover_snap_.reset();
        snap_guide_x_.reset();
        snap_guide_y_.reset();
        drag_kind_ = DragKind::None;
        tool_ = DrawTool::Select;
        annotations_.clear();
        redo_stack_.clear();
        active_annotation_.reset();
        hover_id_ = IdNone;
        hover_idx_ = 0;
        color_picker_open_ = false;
        annotation_dropper_active_ = false;
        sync_picker_hsv_from_current();
        tooltip_.clear(nullptr, 0, false);
        tooltip_hwnd_ = nullptr;

        POINT cur{};
        GetCursorPos(&cur);
        cursor_vpt_ = cur;
        hover_snap_ = (mode_ == OverlayMode::ColorPicker) ? std::nullopt : hit_snap_rect(snaps_, cur);

        // 2. Create one native-DPI overlay HWND per physical monitor!
        HWND primary_or_cursor_hwnd = nullptr;
        monitors_.reserve(capture_->monitors.size());
        for (size_t i = 0; i < capture_->monitors.size(); ++i) {
            const MonitorInfo& mi = capture_->monitors[i];
            const int mw = mi.rect.right - mi.rect.left;
            const int mh = mi.rect.bottom - mi.rect.top;
            if (mw <= 0 || mh <= 0) continue;

            auto mw_ptr = std::make_unique<MonitorWindow>();
            mw_ptr->owner = this;
            mw_ptr->index = monitors_.size();
            mw_ptr->info = mi;

            mw_ptr->hwnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                REGION_OVERLAY_CLASS, L"capusy overlay",
                WS_POPUP,
                mi.rect.left, mi.rect.top, mw, mh,
                nullptr, nullptr, inst, mw_ptr.get());
            if (!mw_ptr->hwnd) {
                cancel();
                return false;
            }
            MonitorWindow& mw_ref = *monitors_.emplace_back(std::move(mw_ptr));
            if (!mw_ref.target.attach(sh_.gfx, mw_ref.hwnd)) {
                cancel();
                return false;
            }
            // Upload this monitor's slice of the frozen virtual capture as a native D2D bitmap!
            if (!upload_monitor_slice(mw_ref)) {
                cancel();
                return false;
            }
            if (!primary_or_cursor_hwnd || rect_contains_pt(mi.rect, cur)) {
                primary_or_cursor_hwnd = mw_ref.hwnd;
            }
        }

        // Render initial frozen frame before revealing so there is zero flash!
        render_all();
        for (auto& m : monitors_) {
            ShowWindow(m->hwnd, SW_SHOWNA);
            SetWindowPos(m->hwnd, HWND_TOPMOST, m->info.rect.left, m->info.rect.top,
                         m->info.rect.right - m->info.rect.left, m->info.rect.bottom - m->info.rect.top,
                         SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        if (primary_or_cursor_hwnd) {
            SetForegroundWindow(primary_or_cursor_hwnd);
            SetFocus(primary_or_cursor_hwnd);
        }
        return true;
    }

    void cancel() noexcept {
        if (monitors_.empty() && !capture_) return;
        const bool was_active = !monitors_.empty();
        if (GetCapture() != nullptr) {
            for (const auto& m : monitors_) {
                if (GetCapture() == m->hwnd) {
                    ReleaseCapture();
                    break;
                }
            }
        }
        for (auto& m : monitors_) {
            if (m->hwnd) {
                KillTimer(m->hwnd, TIMER_TOOLTIP);
                KillTimer(m->hwnd, TIMER_TOOLTIP_ANIM);
            }
            m->frozen_bmp.reset();
            m->target.release();
            if (m->hwnd) {
                DestroyWindow(m->hwnd);
                m->hwnd = nullptr;
            }
        }
        tooltip_.clear(nullptr, 0, false);
        tooltip_hwnd_ = nullptr;
        monitors_.clear();
        capture_.reset();
        snaps_ = {};
        foreground_info_ = {};
        annotations_.clear();
        redo_stack_.clear();
        active_annotation_.reset();
        round_stroke_.reset();
        picker_sv_bmp_.reset();
        picker_hue_bmp_.reset();
        picker_bmp_dc_ = nullptr;
        picker_sv_bmp_h_ = -1.0f;
        color_picker_open_ = false;
        annotation_dropper_active_ = false;
        has_selection_ = false;
        drag_kind_ = DragKind::None;
        open_dropdown_ = AudioDropdown::None;
        desktop_devices_.clear();
        mic_devices_.clear();
        if (was_active && on_closed) on_closed();
    }

private:
    static constexpr UINT_PTR TIMER_TOOLTIP = 1;
    static constexpr UINT_PTR TIMER_TOOLTIP_ANIM = 2;

    enum HitId : int {
        IdNone = 0,
        IdTool,
        IdColor,
        IdColorPicker,
        IdColorDropper,
        IdPickerPanel,
        IdPickerSV,
        IdPickerHue,
        IdThick,
        IdUndo,
        IdRedo,
        IdClear,
        IdCapture,
        IdCancel,
        IdToggleInstant,
        IdRecDesktopToggle,
        IdRecDesktopDrop,
        IdRecMicToggle,
        IdRecMicDrop,
        IdRecDropdownItem,
        IdRecFmtQuick,
    };

    enum class AudioDropdown : uint8_t {
        None = 0,
        Desktop,
        Mic,
    };

    enum class DragKind : uint8_t {
        None = 0,
        Creating,
        Moving,
        ResizeNW, ResizeN, ResizeNE, ResizeE, ResizeSE, ResizeS, ResizeSW, ResizeW,
        Drawing,
        PickingSV,
        PickingHue,
    };

    struct MonitorWindow {
        RegionOverlay* owner = nullptr;
        size_t index = 0;
        MonitorInfo info{};
        HWND hwnd = nullptr;
        WindowTarget target;
        wil::com_ptr<ID2D1Bitmap1> frozen_bmp;
        std::vector<Hit> hits;
    };

    static constexpr std::array<uint32_t, 8> PALETTE = {
        0xff5c5c, // red
        0xff9f43, // orange
        0xffd43b, // yellow
        0x51cf66, // green
        0x38d9a9, // teal
        0x4dabf7, // blue
        0xffffff, // white
        0x141218, // dark ink
    };
    static constexpr std::array<float, 4> THICKNESSES = {2.0f, 4.0f, 8.0f, 14.0f};

    void ensure_stroke_style() noexcept {
        if (round_stroke_ || !sh_.gfx.d2d_factory()) return;
        D2D1_STROKE_STYLE_PROPERTIES1 props = D2D1::StrokeStyleProperties1(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f,
            D2D1_STROKE_TRANSFORM_TYPE_NORMAL);
        (void)sh_.gfx.d2d_factory()->CreateStrokeStyle(&props, nullptr, 0, &round_stroke_);
    }

    bool upload_monitor_slice(MonitorWindow& mw) noexcept {
        if (!capture_ || !mw.target.dc()) return false;
        const int mw_px = mw.info.rect.right - mw.info.rect.left;
        const int mh_px = mw.info.rect.bottom - mw.info.rect.top;
        const int ox = mw.info.rect.left - capture_->bounds.left;
        const int oy = mw.info.rect.top - capture_->bounds.top;
        if (ox < 0 || oy < 0 || ox + mw_px > static_cast<int>(capture_->raster.w) ||
            oy + mh_px > static_cast<int>(capture_->raster.h)) {
            return false;
        }
        const size_t full_stride = static_cast<size_t>(capture_->raster.w) * 4u;
        const uint8_t* start_ptr = capture_->raster.pixels.data() + (static_cast<size_t>(oy) * full_stride) + (static_cast<size_t>(ox) * 4u);
        const float dpi = mw.target.dpi();
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            dpi, dpi);
        return SUCCEEDED(mw.target.dc()->CreateBitmap(
            D2D1::SizeU(static_cast<UINT>(mw_px), static_cast<UINT>(mh_px)),
            start_ptr, static_cast<UINT32>(full_stride), &props, &mw.frozen_bmp));
    }

    [[nodiscard]] size_t toolbar_monitor_index() const noexcept {
        if (monitors_.empty()) return 0;
        const POINT anchor = has_selection_
            ? POINT{.x = (selection_.left + selection_.right) / 2, .y = selection_.bottom}
            : POINT{.x = (monitors_[0]->info.rect.left + monitors_[0]->info.rect.right) / 2, .y = monitors_[0]->info.rect.top};
        for (size_t i = 0; i < monitors_.size(); ++i) {
            if (rect_contains_pt(monitors_[i]->info.rect, anchor)) return i;
        }
        // Fallback: monitor with largest overlap with selection
        if (has_selection_) {
            int64_t best_a = -1;
            size_t best_i = 0;
            for (size_t i = 0; i < monitors_.size(); ++i) {
                const int64_t a = rect_area(rect_intersect(selection_, monitors_[i]->info.rect));
                if (a > best_a) { best_a = a; best_i = i; }
            }
            return best_i;
        }
        return 0;
    }

    void render_all() {
        for (auto& m : monitors_) render_monitor(*m);
    }

    void render_monitor(MonitorWindow& mw) {
        if (!mw.target.ready()) return;
        const bool show_toolbar = has_selection_ && (drag_kind_ != DragKind::Creating) && (mw.index == toolbar_monitor_index());
        (void)mw.target.render_frame(true, {}, WindowTarget::PresentMode::Immediate, [&](std::optional<D2D1_RECT_F> clip, bool) {
            mw.hits.clear();
            Canvas cv{mw.target, sh_.fonts, mw.hits, default_theme(), clip};
            draw_monitor(mw, cv, show_toolbar);
        });
    }

    void draw_monitor(MonitorWindow& mw, Canvas& cv, bool show_toolbar) {
        const D2D1_SIZE_F sz = mw.target.size();
        const float s = mw.target.scale();
        const float inv_s = 1.0f / s;
        const float mon_x = static_cast<float>(mw.info.rect.left);
        const float mon_y = static_cast<float>(mw.info.rect.top);

        // 1. Frozen desktop background (1:1 crisp pixels)
        if (mw.frozen_bmp) {
            cv.dc()->DrawBitmap(mw.frozen_bmp.get(), D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }

        if (mode_ == OverlayMode::ColorPicker) {
            const bool cursor_here = rect_contains_pt(mw.info.rect, cursor_vpt_);
            if (cursor_here) {
                const std::wstring_view hint = L"Click to copy #HEX  •  Arrow keys to nudge  •  Esc to cancel";
                const float hw = std::min(sz.width - 32.0f, cv.measure(hint, Font::SmallSemi) + 28.0f);
                const float cy_dip = (static_cast<float>(cursor_vpt_.y) - mon_y) * inv_s;
                const float hy = (cy_dip < 96.0f) ? (sz.height - 50.0f) : 16.0f;
                const D2D1_RECT_F hr = rect((sz.width - hw) * 0.5f, hy, hw, 34.0f);
                cv.shadow(hr, 17.0f, 8.0f, 0.35f);
                cv.fill_round(hr, 17.0f, with_alpha(cv.theme().surface, 0.94f));
                cv.stroke_round(hr, 17.0f, with_alpha(cv.theme().outline_variant, 0.85f));
                cv.text(hint, hr, Font::SmallSemi, cv.theme().on_surface, Align::Center);

                draw_color_loupe(mw, cv);
            }
            return;
        }

        // Determine active spotlight rect (either selected region or hover snap target)
        const std::optional<RECT> spot = has_selection_ ? std::optional<RECT>{selection_} : hover_snap_;

        // 2. Dimmed scrim outside the spotlight region
        const D2D1_COLOR_F scrim_col = hex(0x09080c, 0.56f);
        if (spot && rect_valid(*spot)) {
            const D2D1_RECT_F sr = D2D1::RectF(
                (static_cast<float>(spot->left) - mon_x) * inv_s,
                (static_cast<float>(spot->top) - mon_y) * inv_s,
                (static_cast<float>(spot->right) - mon_x) * inv_s,
                (static_cast<float>(spot->bottom) - mon_y) * inv_s);
            const D2D1_RECT_F cl = D2D1::RectF(
                std::clamp(sr.left, 0.0f, sz.width),
                std::clamp(sr.top, 0.0f, sz.height),
                std::clamp(sr.right, 0.0f, sz.width),
                std::clamp(sr.bottom, 0.0f, sz.height));
            if (cl.right > cl.left && cl.bottom > cl.top) {
                if (cl.top > 0.0f) cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, cl.top), scrim_col);
                if (cl.bottom < sz.height) cv.fill(D2D1::RectF(0.0f, cl.bottom, sz.width, sz.height), scrim_col);
                if (cl.left > 0.0f) cv.fill(D2D1::RectF(0.0f, cl.top, cl.left, cl.bottom), scrim_col);
                if (cl.right < sz.width) cv.fill(D2D1::RectF(cl.right, cl.top, sz.width, cl.bottom), scrim_col);
            } else {
                cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), scrim_col);
            }

            // 3. Vector annotations clipped inside the selection
            if (has_selection_ && mode_ == OverlayMode::Screenshot && cl.right > cl.left && cl.bottom > cl.top) {
                cv.push_clip(cl);
                ID2D1SolidColorBrush* br = mw.target.solid_brush();
                ID2D1Factory* fac = sh_.gfx.d2d_factory();
                for (const Annotation& a : annotations_) {
                    render_annotation(cv.dc(), fac, br, round_stroke_.get(), a, mon_x, mon_y, s);
                }
                if (active_annotation_) {
                    render_annotation(cv.dc(), fac, br, round_stroke_.get(), *active_annotation_, mon_x, mon_y, s);
                }
                cv.pop_clip();
            }

            // 4. Crisp border & resize handles
            const D2D1_COLOR_F accent = (mode_ == OverlayMode::RecordRegion) ? hex(0xff5c5c) : cv.theme().primary;
            cv.stroke(sr, accent, has_selection_ ? 2.0f : 1.5f);

            if (has_selection_ && tool_ == DrawTool::Select) {
                draw_resize_handles(cv, sr, accent);
            }

            // 5. Dimension badge above top-left of selection (on the monitor showing its top-left)
            if (sr.left >= -40.0f && sr.left < sz.width && sr.top >= -40.0f && sr.top < sz.height) {
                const int rw_px = spot->right - spot->left;
                const int rh_px = spot->bottom - spot->top;
                FixedWString<48> badge;
                badge.assign(to_fixed_wstr<16>(rw_px));
                badge.append(L" × ");
                badge.append(to_fixed_wstr<16>(rh_px));
                if (!has_selection_) {
                    badge.append((mode_ == OverlayMode::Screenshot && instant_capture_) ? L"  (click to capture)" : L"  (click to snap)");
                }
                const float bw = cv.measure(badge, Font::MonoSmall) + 16.0f;
                const float bh = 24.0f;
                const float bx = std::clamp(sr.left, 8.0f, std::max(8.0f, sz.width - bw - 8.0f));
                const float by = (sr.top >= bh + 10.0f) ? (sr.top - bh - 6.0f) : std::min(sr.top + 8.0f, sz.height - bh - 8.0f);
                const D2D1_RECT_F br = rect(bx, by, bw, bh);
                cv.fill_round(br, 6.0f, with_alpha(cv.theme().surface_lowest, 0.90f));
                cv.stroke_round(br, 6.0f, with_alpha(accent, 0.65f));
                cv.text(badge, br, Font::MonoSmall, cv.theme().on_surface, Align::Center);
            }
        } else {
            cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), scrim_col);
        }

        // 6. Magnetic snap guide lines when dragging
        if (snap_guide_x_) {
            const float gx = (static_cast<float>(*snap_guide_x_) - mon_x) * inv_s;
            if (gx >= 0.0f && gx <= sz.width) cv.vline(gx, 0.0f, sz.height, with_alpha(cv.theme().mint, 0.85f));
        }
        if (snap_guide_y_) {
            const float gy = (static_cast<float>(*snap_guide_y_) - mon_y) * inv_s;
            if (gy >= 0.0f && gy <= sz.height) cv.hline(0.0f, sz.width, gy, with_alpha(cv.theme().mint, 0.85f));
        }

        // 7. Top hint banner before selection is made (on primary monitor)
        if (!has_selection_ && mw.info.primary) {
            if (mode_ == OverlayMode::RecordRegion) {
                const std::wstring_view hint = L"Click a window/element or drag to select recording region  •  Hold Alt to disable snap  •  Esc to cancel";
                const float hw = std::min(sz.width - 32.0f, cv.measure(hint, Font::SmallSemi) + 28.0f);
                const D2D1_RECT_F hr = rect((sz.width - hw) * 0.5f, 16.0f, hw, 34.0f);
                cv.shadow(hr, 17.0f, 8.0f, 0.35f);
                cv.fill_round(hr, 17.0f, with_alpha(cv.theme().surface, 0.94f));
                cv.stroke_round(hr, 17.0f, with_alpha(cv.theme().outline_variant, 0.85f));
                cv.text(hint, hr, Font::SmallSemi, cv.theme().on_surface, Align::Center);
            } else {
                const std::wstring_view hint = instant_capture_
                    ? L"Click a window or drag to capture  •  Hold Alt to disable snap  •  Esc to cancel"
                    : L"Click a window or drag to select & edit  •  Hold Alt to disable snap  •  Esc to cancel";
                const wchar_t* mode_lbl = instant_capture_ ? L"Edit Mode (E)" : L"Instant Mode (E)";
                const wchar_t mode_icon = instant_capture_ ? glyph::pen : glyph::camera;
                const float btn_w = button_width(cv, mode_lbl, true, 114.0f);
                const float text_w = cv.measure(hint, Font::SmallSemi);
                const float hw = std::min(sz.width - 32.0f, 16.0f + text_w + 12.0f + btn_w + 8.0f);
                const float hx = (sz.width - hw) * 0.5f;
                const D2D1_RECT_F hr = rect(hx, 16.0f, hw, 38.0f);
                cv.shadow(hr, 19.0f, 8.0f, 0.35f);
                cv.fill_round(hr, 19.0f, with_alpha(cv.theme().surface, 0.94f));
                cv.stroke_round(hr, 19.0f, with_alpha(cv.theme().outline_variant, 0.85f));
                const float avail_text = std::max(60.0f, hw - btn_w - 34.0f);
                cv.text(hint, rect(hx + 16.0f, hr.top, avail_text, height(hr)), Font::SmallSemi, cv.theme().on_surface);
                draw_button(cv, hover_id_ == IdToggleInstant ? 1.0f : 0.0f,
                            rect(hr.right - btn_w - 6.0f, hr.top + 5.0f, btn_w, 28.0f),
                            mode_icon, mode_lbl, IdToggleInstant, 0, instant_capture_ ? 0 : 1);
            }
        }

        // 8. Floating annotation & capture toolbar
        if (show_toolbar) {
            draw_toolbar(mw, cv, mon_x, mon_y, inv_s);
        }

        // 8b. Live eyedropper pixel loupe when picking an annotation color from the screen
        if (mode_ == OverlayMode::Screenshot && annotation_dropper_active_ &&
            drag_kind_ == DragKind::None && hover_id_ == IdNone &&
            rect_contains_pt(mw.info.rect, cursor_vpt_)) {
            draw_color_loupe(mw, cv);
        }

        // 9. Floating hover tooltip (topmost above toolbar)
        if (mw.hwnd == tooltip_hwnd_ && open_dropdown_ == AudioDropdown::None && !color_picker_open_ && drag_kind_ == DragKind::None) {
            draw_tooltip(cv, sz, tooltip_.anchor, tooltip_.title, tooltip_.subtitle, tooltip_.alpha.value);
        }
    }

    [[nodiscard]] bool has_tooltip(int id, int idx) const noexcept {
        if (open_dropdown_ != AudioDropdown::None || color_picker_open_) return false;
        switch (id) {
            case IdUndo:
            case IdRedo:
            case IdClear:
            case IdCapture:
            case IdCancel:
            case IdToggleInstant:
            case IdColorPicker:
            case IdColorDropper:
            case IdRecDesktopToggle:
            case IdRecDesktopDrop:
            case IdRecMicToggle:
            case IdRecMicDrop:
            case IdRecFmtQuick:
                return true;
            case IdTool:
                return idx >= 0 && idx < 7;
            case IdColor:
                return idx >= 0 && std::cmp_less(idx, PALETTE.size());
            case IdThick:
                return idx >= 0 && std::cmp_less(idx, THICKNESSES.size());
            default:
                return false;
        }
    }

    bool build_tooltip(int id, int idx, std::wstring& title, std::wstring& subtitle) const {
        title.clear();
        subtitle.clear();
        if (open_dropdown_ != AudioDropdown::None || color_picker_open_) return false;
        switch (id) {
            case IdToggleInstant:
                if (instant_capture_) {
                    title = L"Switch to Edit Mode (E)";
                    subtitle = L"Adjust region & annotate before saving";
                } else {
                    title = L"Switch to Instant Mode (E)";
                    subtitle = L"Capture immediately on click or drag release";
                }
                return true;
            case IdTool: {
                switch (static_cast<DrawTool>(idx)) {
                    case DrawTool::Select:
                        title = L"Select & Resize";
                        subtitle = L"Move or resize the capture region";
                        return true;
                    case DrawTool::Pen:
                        title = L"Pen";
                        subtitle = L"Freehand draw  •  Scroll to change size";
                        return true;
                    case DrawTool::Highlighter:
                        title = L"Highlighter";
                        subtitle = L"Translucent marker  •  Scroll to change size";
                        return true;
                    case DrawTool::Rect:
                        title = L"Rectangle";
                        subtitle = L"Drag to draw outline  •  Scroll to change size";
                        return true;
                    case DrawTool::Ellipse:
                        title = L"Oval / Ellipse";
                        subtitle = L"Drag to draw outline  •  Scroll to change size";
                        return true;
                    case DrawTool::Line:
                        title = L"Line";
                        subtitle = L"Drag to draw straight line  •  Scroll to change size";
                        return true;
                    case DrawTool::Arrow:
                        title = L"Arrow";
                        subtitle = L"Drag to draw arrow  •  Scroll to change size";
                        return true;
                    default:
                        return false;
                }
            }
            case IdUndo: title = L"Undo (Ctrl+Z)"; return true;
            case IdRedo: title = L"Redo (Ctrl+Y / Ctrl+Shift+Z)"; return true;
            case IdClear: title = L"Clear all annotations"; return true;
            case IdColorPicker:
                title = L"Custom color picker";
                subtitle = std::wstring{format_hex_rgb(u32_to_rgb(current_color_rgb_)).view()};
                return true;
            case IdColorDropper:
                title = annotation_dropper_active_ ? L"Cancel color dropper (Esc)" : L"Pick color from screen";
                subtitle = L"Click any pixel on screen to use its color";
                return true;
            case IdCapture:
                if (mode_ == OverlayMode::RecordRegion) {
                    title = L"Start recording (Enter)";
                } else {
                    title = L"Save screenshot (Enter)";
                    subtitle = L"Or double-click inside the selection";
                }
                return true;
            case IdCancel: title = L"Cancel (Esc)"; return true;
            case IdColor: {
                static constexpr std::array<const wchar_t*, 8> names = {
                    L"Red", L"Orange", L"Yellow", L"Green", L"Teal", L"Blue", L"White", L"Dark ink",
                };
                if (idx >= 0 && std::cmp_less(idx, names.size())) {
                    title = names[static_cast<size_t>(idx)];
                    return true;
                }
                return false;
            }
            case IdThick: {
                static constexpr std::array<const wchar_t*, 4> names = {
                    L"Thin (2 px)", L"Medium (4 px)", L"Thick (8 px)", L"Extra thick (14 px)",
                };
                if (idx >= 0 && std::cmp_less(idx, names.size())) {
                    title = names[static_cast<size_t>(idx)];
                    subtitle = L"Scroll wheel to adjust";
                    return true;
                }
                return false;
            }
            case IdRecDesktopToggle:
                title = sh_.settings.record_desktop_audio ? L"Mute desktop audio" : L"Record desktop audio";
                return true;
            case IdRecDesktopDrop:
                title = L"Select desktop audio device";
                subtitle = resolve_audio_device_label(sh_.settings.record_desktop_device_id, desktop_devices_, L"Default Output");
                return true;
            case IdRecMicToggle:
                title = sh_.settings.record_mic_audio ? L"Mute microphone" : L"Record microphone audio";
                return true;
            case IdRecMicDrop:
                title = L"Select microphone device";
                subtitle = resolve_audio_device_label(sh_.settings.record_mic_device_id, mic_devices_, L"Default Mic");
                return true;
            case IdRecFmtQuick:
                if (idx == 0) title = L"Record as MP4 (H.264/HEVC + AAC)";
                else if (idx == 1) title = L"Record as WMV (WMV9 + WMA)";
                else title = L"Record as animated GIF (no audio)";
                return true;
            default:
                return false;
        }
    }

    void start_tooltip_anim(HWND hwnd) noexcept {
        if (!hwnd) return;
        last_tooltip_anim_ = std::chrono::steady_clock::now();
        SetTimer(hwnd, TIMER_TOOLTIP_ANIM, 16, nullptr);
    }

    bool dismiss_tooltip() noexcept {
        const bool was_vis = tooltip_.visible();
        if (tooltip_hwnd_) {
            KillTimer(tooltip_hwnd_, TIMER_TOOLTIP);
            KillTimer(tooltip_hwnd_, TIMER_TOOLTIP_ANIM);
        }
        tooltip_.clear(nullptr, 0, false);
        return was_vis;
    }

    void draw_resize_handles(Canvas& cv, D2D1_RECT_F sr, D2D1_COLOR_F accent) const noexcept {
        const float mx = (sr.left + sr.right) * 0.5f;
        const float my = (sr.top + sr.bottom) * 0.5f;
        const std::array<D2D1_POINT_2F, 8> pts = {{
            {sr.left, sr.top}, {mx, sr.top}, {sr.right, sr.top}, {sr.right, my},
            {sr.right, sr.bottom}, {mx, sr.bottom}, {sr.left, sr.bottom}, {sr.left, my},
        }};
        for (const D2D1_POINT_2F p : pts) {
            cv.fill_circle(p, 4.5f, cv.theme().surface_lowest);
            cv.stroke_circle(p, 4.5f, accent, 1.8f);
        }
    }

    void draw_toolbar(MonitorWindow& mw, Canvas& cv, float mon_x, float mon_y, float inv_s) {
        const D2D1_SIZE_F sz = mw.target.size();
        const D2D1_RECT_F sr = D2D1::RectF(
            (static_cast<float>(selection_.left) - mon_x) * inv_s,
            (static_cast<float>(selection_.top) - mon_y) * inv_s,
            (static_cast<float>(selection_.right) - mon_x) * inv_s,
            (static_cast<float>(selection_.bottom) - mon_y) * inv_s);

        if (mode_ == OverlayMode::RecordRegion) {
            const RecordFormat fmt = sh_.settings.resolved_record_format();
            const bool audio_supported = record_format_supports_audio(fmt);
            const float rec_w = button_width(cv, L"Start Recording", true, 148.0f);
            const float cancel_w = button_width(cv, L"Cancel", true, 88.0f);
            const float pad = 12.0f;
            const float toggle_w = 108.0f;
            const float tw = std::min(sz.width - 24.0f, 460.0f);
            const float th = audio_supported ? 174.0f : 98.0f;
            const float tx = std::clamp(((sr.left + sr.right) * 0.5f) - (tw * 0.5f), 12.0f, std::max(12.0f, sz.width - tw - 12.0f));
            float ty = sr.bottom + 12.0f;
            if (ty + th > sz.height - 12.0f) ty = sr.top - th - 12.0f;
            ty = std::clamp(ty, 12.0f, std::max(12.0f, sz.height - th - 12.0f));

            const D2D1_RECT_F bar = rect(tx, ty, tw, th);
            cv.shadow(bar, 12.0f, 10.0f, 0.45f);
            cv.fill_round(bar, 12.0f, with_alpha(cv.theme().surface, 0.96f));
            cv.stroke_round(bar, 12.0f, cv.theme().outline_variant);

            const float inner_w = tw - (pad * 2.0f);

            // Row 0: Quick Format switch + live recording spec summary
            const float y0 = ty + 10.0f;
            static constexpr std::array<const wchar_t*, 3> fmt_lbls = {L"MP4", L"WMV", L"GIF"};
            float fx = tx + pad;
            for (int fi = 0; fi < 3; ++fi) {
                const bool active = (static_cast<int>(fmt) == fi);
                draw_button(cv, (hover_id_ == IdRecFmtQuick && hover_idx_ == fi) ? 1.0f : 0.0f,
                            rect(fx, y0, 52.0f, 28.0f), 0, fmt_lbls[static_cast<size_t>(fi)],
                            IdRecFmtQuick, fi, active ? 1 : 0);
                fx += 58.0f;
            }
            const float spec_l = fx + 4.0f;
            const float spec_r = tx + tw - pad;
            if (spec_r - spec_l >= 60.0f) {
                const FixedWString<80> spec = format_recording_spec_summary(
                    fmt, sh_.settings.resolved_record_codec(), sh_.settings.resolved_rate_control(),
                    sh_.settings.record_fps, sh_.settings.record_bitrate_mbps, sh_.settings.record_quality,
                    sh_.settings.record_scale_pct, sh_.settings.record_gif_colors);
                cv.text(spec, D2D1::RectF(spec_l, y0, spec_r, y0 + 28.0f), Font::Caption, cv.theme().on_surface_variant, Align::Right);
            }

            D2D1_RECT_F desk_drop{};
            D2D1_RECT_F mic_drop{};
            float div_y = y0 + 36.0f;

            if (audio_supported) {
                const float drop_w = std::max(110.0f, inner_w - toggle_w - 8.0f);
                const float drop_x = tx + pad + toggle_w + 8.0f;

                // Row 1: Desktop audio toggle + output device dropdown
                const float y1 = y0 + 36.0f;
                const bool desk_on = sh_.settings.record_desktop_audio;
                draw_button(cv, hover_id_ == IdRecDesktopToggle ? 1.0f : 0.0f,
                            rect(tx + pad, y1, toggle_w, 32.0f),
                            desk_on ? glyph::volume : glyph::mute, L"Desktop",
                            IdRecDesktopToggle, 0, desk_on ? 1 : 0);
                desk_drop = rect(drop_x, y1, drop_w, 32.0f);
                const std::wstring desk_lbl = resolve_audio_device_label(sh_.settings.record_desktop_device_id, desktop_devices_, L"Default Output");
                draw_dropdown_field(cv, hover_id_ == IdRecDesktopDrop ? 1.0f : 0.0f, desk_drop, desk_lbl,
                                    open_dropdown_ == AudioDropdown::Desktop, desk_on && !desktop_devices_.empty(),
                                    desktop_devices_.empty() ? IdNone : IdRecDesktopDrop);

                // Row 2: Microphone toggle + input device dropdown
                const float y2 = y1 + 38.0f;
                const bool mic_on = sh_.settings.record_mic_audio;
                draw_button(cv, hover_id_ == IdRecMicToggle ? 1.0f : 0.0f,
                            rect(tx + pad, y2, toggle_w, 32.0f),
                            mic_on ? glyph::mic : glyph::mute, L"Mic",
                            IdRecMicToggle, 0, mic_on ? 1 : 0);
                mic_drop = rect(drop_x, y2, drop_w, 32.0f);
                const std::wstring mic_lbl = resolve_audio_device_label(sh_.settings.record_mic_device_id, mic_devices_, L"Default Mic");
                draw_dropdown_field(cv, hover_id_ == IdRecMicDrop ? 1.0f : 0.0f, mic_drop, mic_lbl,
                                    open_dropdown_ == AudioDropdown::Mic, mic_on && !mic_devices_.empty(),
                                    mic_devices_.empty() ? IdNone : IdRecMicDrop);
                div_y = y2 + 40.0f;
            }

            // Divider + Final row: Start Recording + Cancel
            cv.hline(tx + pad, tx + tw - pad, div_y, with_alpha(cv.theme().outline_variant, 0.55f));
            const float y3 = div_y + 8.0f;
            draw_button(cv, hover_id_ == IdCapture ? 1.0f : 0.0f, rect(tx + pad, y3, rec_w, 32.0f),
                        glyph::video, L"Start Recording", IdCapture, 0, 2);
            if (!audio_supported) {
                const float mid_l = tx + pad + rec_w + 8.0f;
                const float mid_r = tx + tw - pad - cancel_w - 8.0f;
                if (mid_r - mid_l >= 60.0f) {
                    cv.text(L"No audio (GIF)", D2D1::RectF(mid_l, y3, mid_r, y3 + 32.0f), Font::Caption, cv.theme().outline, Align::Center);
                }
            }
            draw_button(cv, hover_id_ == IdCancel ? 1.0f : 0.0f, rect(tx + tw - pad - cancel_w, y3, cancel_w, 32.0f),
                        glyph::close, L"Cancel", IdCancel, 0, 3);

            // Open audio device dropdown popup (rendered last so it wins hit-testing)
            if (audio_supported && open_dropdown_ != AudioDropdown::None) {
                const bool is_desk = (open_dropdown_ == AudioDropdown::Desktop);
                const auto& devs = is_desk ? desktop_devices_ : mic_devices_;
                const std::string& cur_id = is_desk ? sh_.settings.record_desktop_device_id : sh_.settings.record_mic_device_id;
                std::vector<DropdownOption> opts;
                opts.reserve(devs.size() + 1);
                opts.push_back({
                    .label = resolve_audio_device_label("", devs, L"System Default"),
                    .selected = cur_id.empty(),
                    .index = -1,
                });
                for (size_t i = 0; i < devs.size(); ++i) {
                    opts.push_back({
                        .label = devs[i].name,
                        .selected = (!cur_id.empty() && cur_id == devs[i].id),
                        .index = static_cast<int>(i),
                    });
                }
                draw_dropdown_popup(cv, is_desk ? desk_drop : mic_drop, sz, opts, IdRecDropdownItem, hover_id_, hover_idx_);
            }
            return;
        }

        // Screenshot mode: 2-row compact toolbar (Row 1: Tools + Undo/Redo/Clear + Capture/Cancel, Row 2: Colors + Thickness)
        struct ToolBtn { DrawTool tool; wchar_t icon; const wchar_t* label; };
        static constexpr std::array<ToolBtn, 7> tools = {{
            {.tool = DrawTool::Select, .icon = glyph::cursor, .label = L""},
            {.tool = DrawTool::Pen, .icon = glyph::pen, .label = L""},
            {.tool = DrawTool::Highlighter, .icon = glyph::highlighter, .label = L""},
            {.tool = DrawTool::Rect, .icon = 0, .label = L"Rect"},
            {.tool = DrawTool::Ellipse, .icon = 0, .label = L"Oval"},
            {.tool = DrawTool::Line, .icon = 0, .label = L"Line"},
            {.tool = DrawTool::Arrow, .icon = 0, .label = L"Arrow"},
        }};

        const bool show_style_row = (tool_ != DrawTool::Select);
        const float cap_w = button_width(cv, L"Capture", true, 96.0f);
        float row1_w = 10.0f;
        for (const auto& tb : tools) row1_w += (tb.label[0] ? 48.0f : 34.0f) + 4.0f;
        row1_w += 10.0f + (32.0f * 3.0f) + (4.0f * 2.0f) + 10.0f + cap_w + 6.0f + 34.0f + 10.0f;
        const float row2_w = 14.0f + (static_cast<float>(PALETTE.size()) * 28.0f) + 4.0f + 30.0f + 4.0f + 30.0f + 18.0f + (static_cast<float>(THICKNESSES.size()) * 34.0f) + 10.0f;

        const float tw = std::min(sz.width - 24.0f, show_style_row ? std::max(row1_w, row2_w) : row1_w);
        const float th = show_style_row ? 88.0f : 48.0f;
        const float tx = std::clamp(((sr.left + sr.right) * 0.5f) - (tw * 0.5f), 12.0f, std::max(12.0f, sz.width - tw - 12.0f));
        float ty = sr.bottom + 12.0f;
        if (ty + th > sz.height - 12.0f) ty = sr.top - th - 12.0f;
        ty = std::clamp(ty, 12.0f, std::max(12.0f, sz.height - th - 12.0f));

        const D2D1_RECT_F bar = rect(tx, ty, tw, th);
        cv.shadow(bar, 12.0f, 10.0f, 0.45f);
        cv.fill_round(bar, 12.0f, with_alpha(cv.theme().surface, 0.96f));
        cv.stroke_round(bar, 12.0f, cv.theme().outline_variant);

        float x = tx + 10.0f;
        const float y1 = ty + 8.0f;
        for (size_t i = 0; i < tools.size(); ++i) {
            const auto& tb = tools[i];
            const float bw = tb.label[0] ? 48.0f : 34.0f;
            const bool active = (tool_ == tb.tool);
            const float ha = (hover_id_ == IdTool && hover_idx_ == static_cast<int>(i)) ? 1.0f : 0.0f;
            draw_button(cv, ha, rect(x, y1, bw, 32.0f), tb.icon, tb.label, IdTool, static_cast<int>(i), active ? 1 : 3);
            x += bw + 4.0f;
        }

        cv.vline(x + 2.0f, y1 + 4.0f, y1 + 28.0f, with_alpha(cv.theme().outline_variant, 0.7f));
        x += 10.0f;

        draw_button(cv, hover_id_ == IdUndo ? 1.0f : 0.0f, rect(x, y1, 32.0f, 32.0f), glyph::undo, L"", IdUndo, 0, 3);
        x += 36.0f;
        draw_button(cv, hover_id_ == IdRedo ? 1.0f : 0.0f, rect(x, y1, 32.0f, 32.0f), glyph::redo, L"", IdRedo, 0, 3);
        x += 36.0f;
        draw_button(cv, hover_id_ == IdClear ? 1.0f : 0.0f, rect(x, y1, 32.0f, 32.0f), glyph::del, L"", IdClear, 0, 3);
        x += 36.0f;

        cv.vline(x + 2.0f, y1 + 4.0f, y1 + 28.0f, with_alpha(cv.theme().outline_variant, 0.7f));
        x += 10.0f;

        draw_button(cv, hover_id_ == IdCapture ? 1.0f : 0.0f, rect(x, y1, cap_w, 32.0f), glyph::check, L"Capture", IdCapture, 0, 1);
        x += cap_w + 6.0f;
        draw_button(cv, hover_id_ == IdCancel ? 1.0f : 0.0f, rect(x, y1, 34.0f, 32.0f), glyph::close, L"", IdCancel, 0, 3);

        if (show_style_row) {
            const float y2 = ty + 48.0f;
            cv.hline(tx + 10.0f, tx + tw - 10.0f, ty + 44.0f, with_alpha(cv.theme().outline_variant, 0.55f));
            float cx = tx + 14.0f;
            bool in_palette = false;
            for (size_t i = 0; i < PALETTE.size(); ++i) {
                const D2D1_RECT_F sr_col = rect(cx, y2 + 4.0f, 24.0f, 24.0f);
                const D2D1_POINT_2F center{.x = cx + 12.0f, .y = y2 + 16.0f};
                const bool sel = (current_color_rgb_ == PALETTE[i]);
                if (sel) in_palette = true;
                cv.fill_circle(center, sel ? 9.0f : 7.5f, hex(PALETTE[i]));
                cv.stroke_circle(center, sel ? 10.5f : 8.0f, sel ? cv.theme().primary : with_alpha(cv.theme().on_surface, 0.35f), sel ? 2.0f : 1.0f);
                cv.hit(sr_col, IdColor, static_cast<int>(i));
                cx += 28.0f;
            }

            // Custom color picker button + screen color dropper button right next to the base colors
            cx += 4.0f;
            const D2D1_RECT_F pick_r = rect(cx, y2 + 2.0f, 30.0f, 28.0f);
            color_picker_anchor_ = pick_r;
            if (!in_palette) {
                const Rgb8 cur_rgb = u32_to_rgb(current_color_rgb_);
                cv.fill_round(pick_r, 8.0f, hex(current_color_rgb_));
                cv.stroke_round(pick_r, 8.0f, cv.theme().primary, 2.0f);
                cv.icon(glyph::palette, pick_r, is_light_rgb(cur_rgb) ? hex(0x141218) : hex(0xffffff), Font::IconSmall);
                cv.hit(pick_r, IdColorPicker);
            } else {
                draw_button(cv, hover_id_ == IdColorPicker ? 1.0f : 0.0f, pick_r, glyph::palette, L"", IdColorPicker, 0, color_picker_open_ ? 1 : 3);
            }
            cx += 34.0f;

            const D2D1_RECT_F drop_r = rect(cx, y2 + 2.0f, 30.0f, 28.0f);
            draw_button(cv, hover_id_ == IdColorDropper ? 1.0f : 0.0f, drop_r, glyph::eyedropper, L"", IdColorDropper, 0, annotation_dropper_active_ ? 1 : 3);
            cx += 30.0f;

            cv.vline(cx + 6.0f, y2 + 4.0f, y2 + 28.0f, with_alpha(cv.theme().outline_variant, 0.7f));
            cx += 18.0f;

            for (size_t i = 0; i < THICKNESSES.size(); ++i) {
                const D2D1_RECT_F tr = rect(cx, y2 + 2.0f, 30.0f, 28.0f);
                const bool sel = (thick_idx_ == i);
                if (sel) cv.fill_round(tr, 8.0f, cv.theme().primary_container);
                const float r = std::clamp(THICKNESSES[i] * 0.55f + 1.5f, 2.5f, 8.5f);
                cv.fill_circle({.x = cx + 15.0f, .y = y2 + 16.0f}, r, sel ? cv.theme().on_primary_container : cv.theme().on_surface);
                cv.hit(tr, IdThick, static_cast<int>(i));
                cx += 34.0f;
            }

            if (color_picker_open_) {
                draw_color_picker_popover(mw, cv, bar);
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    // Color Picker Popover & Magnified Screen Color Loupe
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] Rgb8 sample_screen_rgb(POINT vpt) const noexcept {
        if (!capture_) return {};
        const int rx = vpt.x - capture_->bounds.left;
        const int ry = vpt.y - capture_->bounds.top;
        if (const auto c = sample_bgra_pixel(capture_->raster.pixels, capture_->raster.w, capture_->raster.h, rx, ry)) {
            return *c;
        }
        return {};
    }

    void sync_picker_hsv_from_current() noexcept {
        const Hsv hsv = rgb_to_hsv(u32_to_rgb(current_color_rgb_));
        picker_h_ = hsv.h;
        picker_s_ = hsv.s;
        picker_v_ = hsv.v;
    }

    void set_current_color_rgb(uint32_t rgb) noexcept {
        current_color_rgb_ = rgb & 0xFFFFFFu;
        sync_picker_hsv_from_current();
    }

    void ensure_picker_bitmaps(ID2D1DeviceContext6* dc) noexcept {
        if (!dc) return;
        if (picker_bmp_dc_ != dc) {
            picker_sv_bmp_.reset();
            picker_hue_bmp_.reset();
            picker_sv_bmp_h_ = -1.0f;
            picker_bmp_dc_ = dc;
        }
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);

        if (!picker_hue_bmp_) {
            constexpr uint32_t hw = 256;
            std::array<uint8_t, hw * 4u> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
            for (uint32_t x = 0; x < hw; ++x) {
                const Rgb8 c = hsv_to_rgb((static_cast<float>(x) / static_cast<float>(hw - 1)) * 360.0f, 1.0f, 1.0f);
                buf[x * 4u + 0u] = c.b;
                buf[x * 4u + 1u] = c.g;
                buf[x * 4u + 2u] = c.r;
                buf[x * 4u + 3u] = 255;
            }
            (void)dc->CreateBitmap(D2D1::SizeU(hw, 1), buf.data(), hw * 4u, &props, &picker_hue_bmp_);
        }

        if (!picker_sv_bmp_ || std::abs(picker_sv_bmp_h_ - picker_h_) > 0.05f) {
            constexpr uint32_t sw = 64;
            constexpr uint32_t sh = 64;
            std::array<uint8_t, sw * sh * 4u> buf; // NOLINT(cppcoreguidelines-pro-type-member-init)
            for (uint32_t y = 0; y < sh; ++y) {
                const float v = 1.0f - (static_cast<float>(y) / static_cast<float>(sh - 1));
                for (uint32_t x = 0; x < sw; ++x) {
                    const float s = static_cast<float>(x) / static_cast<float>(sw - 1);
                    const Rgb8 c = hsv_to_rgb(picker_h_, s, v);
                    const size_t idx = (static_cast<size_t>(y) * sw + x) * 4u;
                    buf[idx + 0u] = c.b;
                    buf[idx + 1u] = c.g;
                    buf[idx + 2u] = c.r;
                    buf[idx + 3u] = 255;
                }
            }
            picker_sv_bmp_.reset();
            if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(sw, sh), buf.data(), sw * 4u, &props, &picker_sv_bmp_))) {
                picker_sv_bmp_h_ = picker_h_;
            }
        }
    }

    void draw_color_picker_popover(MonitorWindow& mw, Canvas& cv, D2D1_RECT_F toolbar_rect) {
        const D2D1_SIZE_F sz = mw.target.size();
        constexpr float pad = 12.0f;
        constexpr float sv_w = 204.0f;
        constexpr float sv_h = 126.0f;
        constexpr float hue_h = 14.0f;
        constexpr float info_h = 26.0f;
        constexpr float pop_w = sv_w + (pad * 2.0f);
        constexpr float pop_h = pad + sv_h + 10.0f + hue_h + 10.0f + info_h + pad;

        const float anchor_cx = (color_picker_anchor_.left + color_picker_anchor_.right) * 0.5f;
        const float px = std::clamp(anchor_cx - (pop_w * 0.5f), 12.0f, std::max(12.0f, sz.width - pop_w - 12.0f));
        float py = toolbar_rect.bottom + 8.0f;
        if (py + pop_h > sz.height - 12.0f) {
            py = toolbar_rect.top - pop_h - 8.0f;
        }
        py = std::clamp(py, 12.0f, std::max(12.0f, sz.height - pop_h - 12.0f));

        const D2D1_RECT_F panel = rect(px, py, pop_w, pop_h);
        picker_panel_rect_ = panel;
        cv.shadow(panel, 12.0f, 12.0f, 0.50f);
        cv.fill_round(panel, 12.0f, with_alpha(cv.theme().surface, 0.98f));
        cv.stroke_round(panel, 12.0f, cv.theme().outline_variant);
        cv.hit(panel, IdPickerPanel);

        ensure_picker_bitmaps(cv.dc());

        // 2D Saturation / Value square
        const D2D1_RECT_F sv_r = rect(px + pad, py + pad, sv_w, sv_h);
        picker_sv_rect_ = sv_r;
        if (picker_sv_bmp_) {
            cv.dc()->DrawBitmap(picker_sv_bmp_.get(), sv_r, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        cv.stroke(sv_r, with_alpha(cv.theme().outline_variant, 0.9f), 1.0f);
        const D2D1_POINT_2F sv_pt{
            .x = sv_r.left + (std::clamp(picker_s_, 0.0f, 1.0f) * width(sv_r)),
            .y = sv_r.top + ((1.0f - std::clamp(picker_v_, 0.0f, 1.0f)) * height(sv_r)),
        };
        cv.stroke_circle(sv_pt, 6.0f, hex(0x000000, 0.85f), 2.5f);
        cv.stroke_circle(sv_pt, 6.0f, hex(0xffffff), 1.5f);
        cv.hit(sv_r, IdPickerSV);

        // Horizontal Hue rainbow slider
        const D2D1_RECT_F hue_r = rect(px + pad, sv_r.bottom + 10.0f, sv_w, hue_h);
        picker_hue_rect_ = hue_r;
        if (picker_hue_bmp_) {
            cv.dc()->DrawBitmap(picker_hue_bmp_.get(), hue_r, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        cv.stroke(hue_r, with_alpha(cv.theme().outline_variant, 0.9f), 1.0f);
        const float hx = hue_r.left + ((std::clamp(picker_h_, 0.0f, 360.0f) / 360.0f) * width(hue_r));
        const D2D1_POINT_2F hue_pt{.x = hx, .y = (hue_r.top + hue_r.bottom) * 0.5f};
        cv.fill_circle(hue_pt, 7.0f, hex(rgb_to_u32(hsv_to_rgb(picker_h_, 1.0f, 1.0f))));
        cv.stroke_circle(hue_pt, 7.5f, hex(0x000000, 0.75f), 2.5f);
        cv.stroke_circle(hue_pt, 7.5f, hex(0xffffff), 1.5f);
        cv.hit(D2D1::RectF(hue_r.left - 4.0f, hue_r.top - 4.0f, hue_r.right + 4.0f, hue_r.bottom + 4.0f), IdPickerHue);

        // Live color swatch + HEX & RGB readout
        const float iy = hue_r.bottom + 10.0f;
        const Rgb8 rgb = u32_to_rgb(current_color_rgb_);
        const D2D1_RECT_F sw_r = rect(px + pad, iy, 28.0f, info_h);
        cv.fill_round(sw_r, 6.0f, hex(current_color_rgb_));
        cv.stroke_round(sw_r, 6.0f, with_alpha(cv.theme().on_surface, 0.35f), 1.0f);

        const FixedWString<16> hex_lbl = format_hex_rgb(rgb);
        const FixedWString<32> rgb_lbl = format_rgb_label(rgb);
        cv.text(hex_lbl, rect(sw_r.right + 8.0f, iy, 64.0f, info_h), Font::MonoSmall, cv.theme().on_surface);
        cv.text(rgb_lbl, D2D1::RectF(sw_r.right + 72.0f, iy, px + pop_w - pad, iy + info_h), Font::Caption, cv.theme().on_surface_variant, Align::Right);
    }

    void draw_color_loupe(MonitorWindow& mw, Canvas& cv) const noexcept {
        const D2D1_SIZE_F sz = mw.target.size();
        const float inv_s = 1.0f / mw.target.scale();
        const float cx = (static_cast<float>(cursor_vpt_.x) - static_cast<float>(mw.info.rect.left)) * inv_s;
        const float cy = (static_cast<float>(cursor_vpt_.y) - static_cast<float>(mw.info.rect.top)) * inv_s;

        const Rgb8 center_rgb = sample_screen_rgb(cursor_vpt_);

        // Subtle pixel-precision crosshair reticle around the sampled pixel (leaves center pixel unobscured)
        const D2D1_COLOR_F ret_outer = is_light_rgb(center_rgb) ? hex(0x000000, 0.85f) : hex(0xffffff, 0.90f);
        const float px_dip = std::max(1.0f, inv_s);
        cv.stroke(D2D1::RectF(cx - px_dip, cy - px_dip, cx + (px_dip * 2.0f), cy + (px_dip * 2.0f)), ret_outer, 1.2f);

        constexpr int half = 6; // 13 x 13 zoomed pixel grid
        constexpr int grid_n = (half * 2) + 1;
        constexpr float cell = 13.0f;
        constexpr float grid_sz = static_cast<float>(grid_n) * cell; // 169 DIPs
        constexpr float pad = 10.0f;
        constexpr float card_w = grid_sz + (pad * 2.0f); // 189 DIPs
        constexpr float footer_h = 34.0f;
        constexpr float card_h = pad + grid_sz + 8.0f + footer_h + pad; // 231 DIPs

        float lx = cx + 22.0f;
        float ly = cy + 22.0f;
        if (lx + card_w > sz.width - 10.0f) lx = cx - card_w - 22.0f;
        if (ly + card_h > sz.height - 10.0f) ly = cy - card_h - 22.0f;
        lx = std::clamp(lx, 10.0f, std::max(10.0f, sz.width - card_w - 10.0f));
        ly = std::clamp(ly, 10.0f, std::max(10.0f, sz.height - card_h - 10.0f));

        const D2D1_RECT_F card = rect(lx, ly, card_w, card_h);
        cv.shadow(card, 12.0f, 12.0f, 0.50f);
        cv.fill_round(card, 12.0f, with_alpha(cv.theme().surface_lowest, 0.96f));
        cv.stroke_round(card, 12.0f, with_alpha(cv.theme().outline_variant, 0.95f));

        const float gx0 = lx + pad;
        const float gy0 = ly + pad;
        for (int dy = -half; dy <= half; ++dy) {
            for (int dx = -half; dx <= half; ++dx) {
                const Rgb8 c = sample_screen_rgb({.x = cursor_vpt_.x + dx, .y = cursor_vpt_.y + dy});
                const float x0 = gx0 + (static_cast<float>(dx + half) * cell);
                const float y0 = gy0 + (static_cast<float>(dy + half) * cell);
                cv.fill(D2D1::RectF(x0, y0, x0 + cell, y0 + cell), hex(rgb_to_u32(c)));
            }
        }

        // Subtle pixel grid lines
        const D2D1_COLOR_F grid_line = hex(0x000000, 0.22f);
        for (int i = 1; i < grid_n; ++i) {
            const float off = static_cast<float>(i) * cell;
            cv.vline(gx0 + off, gy0, gy0 + grid_sz, grid_line);
            cv.hline(gx0, gx0 + grid_sz, gy0 + off, grid_line);
        }
        cv.stroke(rect(gx0, gy0, grid_sz, grid_sz), with_alpha(cv.theme().outline_variant, 0.9f), 1.0f);

        // Highlight the exact center pixel cell
        const D2D1_RECT_F center_cell = rect(gx0 + (static_cast<float>(half) * cell), gy0 + (static_cast<float>(half) * cell), cell, cell);
        cv.stroke(D2D1::RectF(center_cell.left - 1.0f, center_cell.top - 1.0f, center_cell.right + 1.0f, center_cell.bottom + 1.0f),
                  hex(0x000000, 0.85f), 1.5f);
        cv.stroke(center_cell, hex(0xffffff), 1.5f);

        // Bottom color swatch + RGB & #HEX readouts
        const float fy = gy0 + grid_sz + 8.0f;
        const D2D1_RECT_F sw_r = rect(gx0, fy + 2.0f, 30.0f, 30.0f);
        cv.fill_round(sw_r, 7.0f, hex(rgb_to_u32(center_rgb)));
        cv.stroke_round(sw_r, 7.0f, with_alpha(cv.theme().on_surface, 0.35f), 1.0f);

        const float tx = sw_r.right + 8.0f;
        const float tw = (lx + card_w - pad) - tx;
        const FixedWString<32> rgb_lbl = format_rgb_label(center_rgb);
        const FixedWString<16> hex_lbl = format_hex_rgb(center_rgb);
        cv.text(rgb_lbl, rect(tx, fy, tw, 17.0f), Font::MonoSmall, cv.theme().on_surface);
        cv.text(hex_lbl, rect(tx, fy + 17.0f, tw, 17.0f), Font::MonoSmall, cv.theme().primary);
    }

    void update_picker_sv(D2D1_POINT_2F dip) noexcept {
        const float rw = std::max(1.0f, width(picker_sv_rect_));
        const float rh = std::max(1.0f, height(picker_sv_rect_));
        picker_s_ = std::clamp((dip.x - picker_sv_rect_.left) / rw, 0.0f, 1.0f);
        picker_v_ = 1.0f - std::clamp((dip.y - picker_sv_rect_.top) / rh, 0.0f, 1.0f);
        current_color_rgb_ = rgb_to_u32(hsv_to_rgb(picker_h_, picker_s_, picker_v_));
    }

    void update_picker_hue(D2D1_POINT_2F dip) noexcept {
        const float rw = std::max(1.0f, width(picker_hue_rect_));
        picker_h_ = std::clamp((dip.x - picker_hue_rect_.left) / rw, 0.0f, 1.0f) * 360.0f;
        if (picker_h_ >= 360.0f) picker_h_ = 359.99f;
        current_color_rgb_ = rgb_to_u32(hsv_to_rgb(picker_h_, picker_s_, picker_v_));
    }

    void nudge_cursor(int dx, int dy) {
        if (!capture_) return;
        const LONG nx = std::clamp<LONG>(cursor_vpt_.x + dx, capture_->bounds.left, capture_->bounds.right - 1);
        const LONG ny = std::clamp<LONG>(cursor_vpt_.y + dy, capture_->bounds.top, capture_->bounds.bottom - 1);
        cursor_vpt_ = {.x = nx, .y = ny};
        SetCursorPos(nx, ny);
        render_all();
    }

    // ------------------------------------------------------------------------------------------
    // Export cropped region + annotations at 1:1 native physical resolution
    // ------------------------------------------------------------------------------------------

    std::optional<Raster> compose_final_raster() {
        if (!capture_ || !has_selection_ || !rect_valid(selection_)) return std::nullopt;
        auto base = crop_virtual_capture(*capture_, selection_);
        if (!base) return std::nullopt;
        if (annotations_.empty()) return base;

        // Render cropped base + all Direct2D vector annotations at 1:1 physical pixels (96 DPI).
        if (!sh_.gfx.ensure_device(nullptr)) return base;
        wil::com_ptr<ID2D1DeviceContext6> dc;
        if (FAILED(sh_.gfx.d2d_device()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) || !dc) return base;

        const D2D1_PIXEL_FORMAT pfmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        const auto target_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pfmt, 96.0f, 96.0f);
        wil::com_ptr<ID2D1Bitmap1> rt;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(base->w, base->h), nullptr, 0, &target_props, &rt)) || !rt) return base;

        const auto src_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        wil::com_ptr<ID2D1Bitmap1> src_bmp;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(base->w, base->h), base->pixels.data(), base->w * 4u, &src_props, &src_bmp)) || !src_bmp) {
            return base;
        }

        wil::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush)) || !brush) return base;

        dc->SetTarget(rt.get());
        dc->SetDpi(96.0f, 96.0f);
        dc->BeginDraw();
        dc->DrawBitmap(src_bmp.get(), D2D1::RectF(0.0f, 0.0f, static_cast<float>(base->w), static_cast<float>(base->h)));
        const float ox = static_cast<float>(selection_.left);
        const float oy = static_cast<float>(selection_.top);
        ID2D1Factory* fac = sh_.gfx.d2d_factory();
        for (const Annotation& a : annotations_) {
            render_annotation(dc.get(), fac, brush.get(), round_stroke_.get(), a, ox, oy, 1.0f);
        }
        const HRESULT end_hr = dc->EndDraw();
        dc->SetTarget(nullptr);
        if (FAILED(end_hr)) return base;

        // Read back GPU target to CPU BGRA buffer
        const auto cpu_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pfmt, 96.0f, 96.0f);
        wil::com_ptr<ID2D1Bitmap1> staging;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(base->w, base->h), nullptr, 0, &cpu_props, &staging)) || !staging) return base;
        if (FAILED(staging->CopyFromBitmap(nullptr, rt.get(), nullptr))) return base;

        D2D1_MAPPED_RECT mapped{};
        if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return base;
        const size_t row_bytes = static_cast<size_t>(base->w) * 4u;
        for (UINT y = 0; y < base->h; ++y) {
            std::memcpy(base->pixels.data() + (static_cast<size_t>(y) * row_bytes),
                        mapped.bits + (static_cast<size_t>(y) * mapped.pitch), row_bytes);
        }
        staging->Unmap();

        auto* px32 = reinterpret_cast<uint32_t*>(base->pixels.data());
        const size_t count = static_cast<size_t>(base->w) * base->h;
        for (size_t i = 0; i < count; ++i) px32[i] |= 0xFF000000u;
        return base;
    }

    void confirm_capture() {
        if (!has_selection_ || !rect_valid(selection_)) return;
        WindowInfo win_info = window_info_for_rect(snaps_, selection_, foreground_info_);
        if (mode_ == OverlayMode::RecordRegion) {
            const RECT r = selection_;
            auto cb = on_record_region_selected;
            cancel();
            if (cb) cb(r, std::move(win_info));
            return;
        }
        auto raster = compose_final_raster();
        auto cb = on_screenshot_captured;
        cancel();
        if (raster && cb) cb(std::move(*raster), std::move(win_info));
    }

    // ------------------------------------------------------------------------------------------
    // Input handling (global virtual-screen coordinates)
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] POINT to_virtual_px(const MonitorWindow& mw, LPARAM lp) const noexcept {
        return {
            .x = mw.info.rect.left + GET_X_LPARAM(lp),
            .y = mw.info.rect.top + GET_Y_LPARAM(lp),
        };
    }

    [[nodiscard]] D2D1_POINT_2F to_local_dip(const MonitorWindow& mw, LPARAM lp) const noexcept {
        const float inv_s = 1.0f / mw.target.scale();
        return {.x = static_cast<float>(GET_X_LPARAM(lp)) * inv_s, .y = static_cast<float>(GET_Y_LPARAM(lp)) * inv_s};
    }

    [[nodiscard]] DragKind hit_selection_handle(POINT pt, float scale) const noexcept {
        if (!has_selection_ || !rect_valid(selection_)) return DragKind::None;
        const LONG pad = static_cast<LONG>(std::lround(10.0f * scale));
        const LONG mx = (selection_.left + selection_.right) / 2;
        const LONG my = (selection_.top + selection_.bottom) / 2;
        const auto near_pt = [&](LONG x, LONG y) noexcept {
            return std::abs(pt.x - x) <= pad && std::abs(pt.y - y) <= pad;
        };
        if (near_pt(selection_.left, selection_.top)) return DragKind::ResizeNW;
        if (near_pt(selection_.right, selection_.top)) return DragKind::ResizeNE;
        if (near_pt(selection_.right, selection_.bottom)) return DragKind::ResizeSE;
        if (near_pt(selection_.left, selection_.bottom)) return DragKind::ResizeSW;
        if (near_pt(mx, selection_.top) || (std::abs(pt.y - selection_.top) <= pad && pt.x > selection_.left && pt.x < selection_.right)) return DragKind::ResizeN;
        if (near_pt(mx, selection_.bottom) || (std::abs(pt.y - selection_.bottom) <= pad && pt.x > selection_.left && pt.x < selection_.right)) return DragKind::ResizeS;
        if (near_pt(selection_.left, my) || (std::abs(pt.x - selection_.left) <= pad && pt.y > selection_.top && pt.y < selection_.bottom)) return DragKind::ResizeW;
        if (near_pt(selection_.right, my) || (std::abs(pt.x - selection_.right) <= pad && pt.y > selection_.top && pt.y < selection_.bottom)) return DragKind::ResizeE;
        if (rect_contains_pt(selection_, pt)) return DragKind::Moving;
        return DragKind::None;
    }

    void update_cursor(const MonitorWindow& mw, POINT vpt, const Hit* h) noexcept {
        HCURSOR cur = cursor_cross_;
        if (mode_ == OverlayMode::ColorPicker) {
            cur = cursor_cross_;
        } else if (h) {
            if (h->id == IdPickerSV) cur = cursor_cross_;
            else if (h->id == IdPickerPanel) cur = cursor_arrow_;
            else cur = cursor_hand_;
        } else if (annotation_dropper_active_) {
            cur = cursor_cross_;
        } else if (has_selection_) {
            if (tool_ != DrawTool::Select && rect_contains_pt(selection_, vpt)) {
                cur = cursor_cross_;
            } else if (tool_ == DrawTool::Select) {
                switch (hit_selection_handle(vpt, mw.target.scale())) {
                    case DragKind::ResizeNW:
                    case DragKind::ResizeSE: cur = cursor_nwse_; break;
                    case DragKind::ResizeNE:
                    case DragKind::ResizeSW: cur = cursor_nesw_; break;
                    case DragKind::ResizeN:
                    case DragKind::ResizeS: cur = cursor_ns_; break;
                    case DragKind::ResizeW:
                    case DragKind::ResizeE: cur = cursor_we_; break;
                    case DragKind::Moving: cur = cursor_move_; break;
                    default: cur = cursor_cross_; break;
                }
            }
        }
        current_cursor_ = cur;
        if (cur) SetCursor(cur);
    }

    void on_toolbar_click(int id, int idx) {
        switch (id) {
            case IdTool:
                tool_ = static_cast<DrawTool>(idx);
                annotation_dropper_active_ = false;
                if (tool_ == DrawTool::Select) color_picker_open_ = false;
                break;
            case IdColor:
                if (idx >= 0 && std::cmp_less(idx, PALETTE.size())) {
                    set_current_color_rgb(PALETTE[static_cast<size_t>(idx)]);
                }
                annotation_dropper_active_ = false;
                break;
            case IdColorPicker:
                color_picker_open_ = !color_picker_open_;
                if (color_picker_open_) {
                    annotation_dropper_active_ = false;
                    sync_picker_hsv_from_current();
                }
                break;
            case IdColorDropper:
                annotation_dropper_active_ = !annotation_dropper_active_;
                if (annotation_dropper_active_) {
                    color_picker_open_ = false;
                }
                break;
            case IdPickerPanel:
                return;
            case IdThick:
                if (idx >= 0 && std::cmp_less(idx, THICKNESSES.size())) thick_idx_ = static_cast<size_t>(idx);
                break;
            case IdUndo:
                if (!annotations_.empty()) {
                    redo_stack_.push_back(std::move(annotations_.back()));
                    annotations_.pop_back();
                }
                break;
            case IdRedo:
                if (!redo_stack_.empty()) {
                    annotations_.push_back(std::move(redo_stack_.back()));
                    redo_stack_.pop_back();
                }
                break;
            case IdClear:
                annotations_.clear();
                redo_stack_.clear();
                break;
            case IdCapture:
                confirm_capture();
                return;
            case IdCancel:
                cancel();
                return;
            case IdToggleInstant:
                instant_capture_ = !instant_capture_;
                break;
            case IdRecDesktopToggle:
                open_dropdown_ = AudioDropdown::None;
                sh_.settings.record_desktop_audio = !sh_.settings.record_desktop_audio;
                sh_.save();
                if (sh_.on_redraw_all) sh_.on_redraw_all();
                break;
            case IdRecDesktopDrop:
                open_dropdown_ = (open_dropdown_ == AudioDropdown::Desktop) ? AudioDropdown::None : AudioDropdown::Desktop;
                break;
            case IdRecMicToggle:
                open_dropdown_ = AudioDropdown::None;
                sh_.settings.record_mic_audio = !sh_.settings.record_mic_audio;
                sh_.save();
                if (sh_.on_redraw_all) sh_.on_redraw_all();
                break;
            case IdRecMicDrop:
                open_dropdown_ = (open_dropdown_ == AudioDropdown::Mic) ? AudioDropdown::None : AudioDropdown::Mic;
                break;
            case IdRecDropdownItem:
                if (open_dropdown_ == AudioDropdown::Desktop) {
                    if (idx < 0) sh_.settings.record_desktop_device_id.clear();
                    else if (std::cmp_less(idx, desktop_devices_.size())) sh_.settings.record_desktop_device_id = desktop_devices_[static_cast<size_t>(idx)].id;
                    sh_.settings.record_desktop_audio = true;
                    sh_.save();
                    if (sh_.on_redraw_all) sh_.on_redraw_all();
                } else if (open_dropdown_ == AudioDropdown::Mic) {
                    if (idx < 0) sh_.settings.record_mic_device_id.clear();
                    else if (std::cmp_less(idx, mic_devices_.size())) sh_.settings.record_mic_device_id = mic_devices_[static_cast<size_t>(idx)].id;
                    sh_.settings.record_mic_audio = true;
                    sh_.save();
                    if (sh_.on_redraw_all) sh_.on_redraw_all();
                }
                open_dropdown_ = AudioDropdown::None;
                break;
            case IdRecFmtQuick:
                open_dropdown_ = AudioDropdown::None;
                if (idx == 0) sh_.settings.record_format = "mp4";
                else if (idx == 1) sh_.settings.record_format = "wmv";
                else if (idx == 2) sh_.settings.record_format = "gif";
                sh_.save();
                if (sh_.on_redraw_all) sh_.on_redraw_all();
                break;
            default:
                break;
        }
        render_all();
    }

    void confirm_screen_color(POINT vpt) {
        const Rgb8 rgb = sample_screen_rgb(vpt);
        auto cb = on_color_picked;
        cancel();
        if (cb) cb(rgb);
    }

    void on_lbutton_down(MonitorWindow& mw, LPARAM lp) {
        const POINT vpt = to_virtual_px(mw, lp);
        cursor_vpt_ = vpt;
        if (mode_ == OverlayMode::ColorPicker) {
            confirm_screen_color(vpt);
            return;
        }

        const bool tip_vis = dismiss_tooltip();
        const D2D1_POINT_2F dip = to_local_dip(mw, lp);
        if (const Hit* h = hit_test(mw.hits, dip)) {
            if (h->id == IdPickerSV) {
                drag_kind_ = DragKind::PickingSV;
                SetCapture(mw.hwnd);
                update_picker_sv(dip);
                render_all();
                return;
            }
            if (h->id == IdPickerHue) {
                drag_kind_ = DragKind::PickingHue;
                SetCapture(mw.hwnd);
                update_picker_hue(dip);
                render_all();
                return;
            }
            on_toolbar_click(h->id, h->index);
            return;
        }

        if (color_picker_open_) {
            color_picker_open_ = false;
            render_all();
            return;
        }

        if (annotation_dropper_active_) {
            set_current_color_rgb(rgb_to_u32(sample_screen_rgb(vpt)));
            annotation_dropper_active_ = false;
            render_all();
            return;
        }

        if (open_dropdown_ != AudioDropdown::None) {
            open_dropdown_ = AudioDropdown::None;
            render_all();
            return;
        }

        SetCapture(mw.hwnd);

        if (has_selection_ && tool_ != DrawTool::Select && mode_ == OverlayMode::Screenshot) {
            drag_kind_ = DragKind::Drawing;
            Annotation a;
            a.tool = tool_;
            a.color = hex(current_color_rgb_);
            a.thickness = THICKNESSES[thick_idx_] * mw.target.scale();
            a.points.push_back(D2D1::Point2F(static_cast<float>(vpt.x), static_cast<float>(vpt.y)));
            active_annotation_ = std::move(a);
            render_all();
            return;
        }

        if (has_selection_ && tool_ == DrawTool::Select) {
            const DragKind dk = hit_selection_handle(vpt, mw.target.scale());
            if (dk != DragKind::None) {
                drag_kind_ = dk;
                drag_start_ = vpt;
                drag_orig_sel_ = selection_;
                if (tip_vis) render_monitor(mw);
                return;
            }
        }

        // Start a fresh selection (click without drag snaps to hover_snap_ on mouse-up!)
        drag_kind_ = DragKind::Creating;
        drag_start_ = vpt;
        click_snap_candidate_ = hover_snap_;
        has_selection_ = false;
        selection_ = {.left = vpt.x, .top = vpt.y, .right = vpt.x, .bottom = vpt.y};
        annotations_.clear();
        redo_stack_.clear();
        render_all();
    }

    void on_mouse_move(MonitorWindow& mw, LPARAM lp) {
        const POINT vpt = to_virtual_px(mw, lp);
        cursor_vpt_ = vpt;
        if (mode_ == OverlayMode::ColorPicker) {
            update_cursor(mw, vpt, nullptr);
            render_all();
            return;
        }
        const bool snap_on = (GetKeyState(VK_MENU) >= 0); // hold Alt to bypass magnetic snap
        const LONG snap_px = static_cast<LONG>(std::lround(10.0f * mw.target.scale()));

        if (drag_kind_ == DragKind::PickingSV) {
            update_picker_sv(to_local_dip(mw, lp));
            render_all();
            return;
        }
        if (drag_kind_ == DragKind::PickingHue) {
            update_picker_hue(to_local_dip(mw, lp));
            render_all();
            return;
        }

        if (drag_kind_ == DragKind::None) {
            const D2D1_POINT_2F dip = to_local_dip(mw, lp);
            const Hit* h = hit_test(mw.hits, dip);
            const int nid = h ? h->id : IdNone;
            const int nidx = h ? h->index : 0;
            const bool mon_changed = (tooltip_hwnd_ != mw.hwnd);
            bool need_render = (nid != hover_id_ || nidx != hover_idx_) || annotation_dropper_active_;
            if (need_render || (mon_changed && nid != IdNone)) {
                if (mon_changed && tooltip_hwnd_) {
                    KillTimer(tooltip_hwnd_, TIMER_TOOLTIP);
                    KillTimer(tooltip_hwnd_, TIMER_TOOLTIP_ANIM);
                    tooltip_.clear(nullptr, 0, false);
                    need_render = true;
                }
                tooltip_hwnd_ = mw.hwnd;
                tooltip_.on_hover_changed(mw.hwnd, TIMER_TOOLTIP, h, has_tooltip(nid, nidx), true);
                if (tooltip_.alpha.active()) start_tooltip_anim(mw.hwnd);
            }
            hover_id_ = nid;
            hover_idx_ = nidx;
            if (!has_selection_) {
                const auto next_snap = snap_on ? hit_snap_rect(snaps_, vpt) : std::nullopt;
                const bool changed = (next_snap.has_value() != hover_snap_.has_value()) ||
                                     (next_snap && hover_snap_ && (next_snap->left != hover_snap_->left || next_snap->top != hover_snap_->top ||
                                                                    next_snap->right != hover_snap_->right || next_snap->bottom != hover_snap_->bottom));
                if (changed) {
                    hover_snap_ = next_snap;
                    need_render = true;
                }
            }
            update_cursor(mw, vpt, h);
            TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = mw.hwnd};
            TrackMouseEvent(&tme);
            if (need_render) render_all();
            return;
        }

        snap_guide_x_.reset();
        snap_guide_y_.reset();

        if (drag_kind_ == DragKind::Drawing && active_annotation_) {
            const D2D1_POINT_2F pt = D2D1::Point2F(static_cast<float>(vpt.x), static_cast<float>(vpt.y));
            if (active_annotation_->tool == DrawTool::Pen || active_annotation_->tool == DrawTool::Highlighter) {
                const D2D1_POINT_2F prev = active_annotation_->points.back();
                if (std::hypot(pt.x - prev.x, pt.y - prev.y) >= 1.5f) {
                    active_annotation_->points.push_back(pt);
                }
            } else {
                if (active_annotation_->points.size() < 2) active_annotation_->points.push_back(pt);
                else active_annotation_->points.back() = pt;
            }
            render_all();
            return;
        }

        if (drag_kind_ == DragKind::Creating) {
            if (std::abs(vpt.x - drag_start_.x) >= 4 || std::abs(vpt.y - drag_start_.y) >= 4) {
                has_selection_ = true;
                LONG cx = vpt.x;
                LONG cy = vpt.y;
                if (snap_on) {
                    cx = snap_coordinate(cx, snaps_.x_guides, snap_px, snap_guide_x_);
                    cy = snap_coordinate(cy, snaps_.y_guides, snap_px, snap_guide_y_);
                }
                selection_ = RECT{
                    .left = std::min(drag_start_.x, cx),
                    .top = std::min(drag_start_.y, cy),
                    .right = std::max(drag_start_.x, cx),
                    .bottom = std::max(drag_start_.y, cy),
                };
                render_all();
            }
            return;
        }

        if (drag_kind_ == DragKind::Moving) {
            const LONG w = drag_orig_sel_.right - drag_orig_sel_.left;
            const LONG h = drag_orig_sel_.bottom - drag_orig_sel_.top;
            LONG nl = drag_orig_sel_.left + (vpt.x - drag_start_.x);
            LONG nt = drag_orig_sel_.top + (vpt.y - drag_start_.y);
            if (snap_on) {
                std::optional<LONG> gx;
                const LONG sl = snap_coordinate(nl, snaps_.x_guides, snap_px, gx);
                if (gx) { nl = sl; snap_guide_x_ = gx; }
                else {
                    const LONG sr = snap_coordinate(nl + w, snaps_.x_guides, snap_px, gx);
                    if (gx) { nl = sr - w; snap_guide_x_ = gx; }
                }
                std::optional<LONG> gy;
                const LONG st = snap_coordinate(nt, snaps_.y_guides, snap_px, gy);
                if (gy) { nt = st; snap_guide_y_ = gy; }
                else {
                    const LONG sb = snap_coordinate(nt + h, snaps_.y_guides, snap_px, gy);
                    if (gy) { nt = sb - h; snap_guide_y_ = gy; }
                }
            }
            selection_ = {.left = nl, .top = nt, .right = nl + w, .bottom = nt + h};
            render_all();
            return;
        }

        // Resize handles
        RECT r = drag_orig_sel_;
        const auto snap_x = [&](LONG v) { return snap_on ? snap_coordinate(v, snaps_.x_guides, snap_px, snap_guide_x_) : v; };
        const auto snap_y = [&](LONG v) { return snap_on ? snap_coordinate(v, snaps_.y_guides, snap_px, snap_guide_y_) : v; };
        switch (drag_kind_) {
            case DragKind::ResizeNW: r.left = snap_x(vpt.x); r.top = snap_y(vpt.y); break;
            case DragKind::ResizeN:  r.top = snap_y(vpt.y); break;
            case DragKind::ResizeNE: r.right = snap_x(vpt.x); r.top = snap_y(vpt.y); break;
            case DragKind::ResizeE:  r.right = snap_x(vpt.x); break;
            case DragKind::ResizeSE: r.right = snap_x(vpt.x); r.bottom = snap_y(vpt.y); break;
            case DragKind::ResizeS:  r.bottom = snap_y(vpt.y); break;
            case DragKind::ResizeSW: r.left = snap_x(vpt.x); r.bottom = snap_y(vpt.y); break;
            case DragKind::ResizeW:  r.left = snap_x(vpt.x); break;
            default: break;
        }
        selection_ = RECT{
            .left = std::min(r.left, r.right),
            .top = std::min(r.top, r.bottom),
            .right = std::max(r.left, r.right),
            .bottom = std::max(r.top, r.bottom),
        };
        if (selection_.right - selection_.left < 8) selection_.right = selection_.left + 8;
        if (selection_.bottom - selection_.top < 8) selection_.bottom = selection_.top + 8;
        render_all();
    }

    void on_lbutton_up(MonitorWindow& mw, LPARAM /*lp*/) {
        if (GetCapture() == mw.hwnd) ReleaseCapture();
        snap_guide_x_.reset();
        snap_guide_y_.reset();

        if (drag_kind_ == DragKind::Drawing) {
            if (active_annotation_) {
                annotations_.push_back(std::move(*active_annotation_));
                active_annotation_.reset();
                redo_stack_.clear();
            }
            drag_kind_ = DragKind::None;
            render_all();
            return;
        }

        if (drag_kind_ == DragKind::Creating) {
            if (!has_selection_ && click_snap_candidate_ && rect_valid(*click_snap_candidate_)) {
                // Single click selected the hovered window/element!
                selection_ = *click_snap_candidate_;
                has_selection_ = true;
            } else if (has_selection_ && ((selection_.right - selection_.left) < 4 || (selection_.bottom - selection_.top) < 4)) {
                has_selection_ = false;
            }
            drag_kind_ = DragKind::None;
            if (has_selection_ && mode_ == OverlayMode::Screenshot && instant_capture_) {
                confirm_capture();
                return;
            }
            render_all();
            return;
        }

        drag_kind_ = DragKind::None;
        render_all();
    }

    void on_key_down(UINT vk) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        if (mode_ == OverlayMode::ColorPicker) {
            if (vk == VK_ESCAPE) {
                cancel();
                return;
            }
            if (vk == VK_RETURN || vk == VK_SPACE) {
                confirm_screen_color(cursor_vpt_);
                return;
            }
            if (vk == VK_LEFT)  { nudge_cursor(-1, 0); return; }
            if (vk == VK_RIGHT) { nudge_cursor(1, 0); return; }
            if (vk == VK_UP)    { nudge_cursor(0, -1); return; }
            if (vk == VK_DOWN)  { nudge_cursor(0, 1); return; }
            return;
        }
        if (vk == VK_ESCAPE) {
            if (color_picker_open_) {
                color_picker_open_ = false;
                render_all();
                return;
            }
            if (annotation_dropper_active_) {
                annotation_dropper_active_ = false;
                render_all();
                return;
            }
            if (open_dropdown_ != AudioDropdown::None) {
                open_dropdown_ = AudioDropdown::None;
                render_all();
                return;
            }
            cancel();
            return;
        }
        if (annotation_dropper_active_) {
            if (vk == VK_LEFT)  { nudge_cursor(-1, 0); return; }
            if (vk == VK_RIGHT) { nudge_cursor(1, 0); return; }
            if (vk == VK_UP)    { nudge_cursor(0, -1); return; }
            if (vk == VK_DOWN)  { nudge_cursor(0, 1); return; }
        }
        if (vk == 'E' && !ctrl && !has_selection_ && mode_ == OverlayMode::Screenshot) {
            instant_capture_ = !instant_capture_;
            render_all();
            return;
        }
        if (vk == VK_RETURN && has_selection_ && open_dropdown_ == AudioDropdown::None) {
            confirm_capture();
            return;
        }
        if (ctrl && vk == 'Z') {
            on_toolbar_click(shift ? IdRedo : IdUndo, 0);
            return;
        }
        if (ctrl && vk == 'Y') {
            on_toolbar_click(IdRedo, 0);
            return;
        }
    }

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* mw = reinterpret_cast<MonitorWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            if (mw) mw->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(mw));
        }
        if (auto* mw = reinterpret_cast<MonitorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            if (msg == WM_NCDESTROY) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
            if (mw->owner) return mw->owner->handle(*mw, hwnd, msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handle(MonitorWindow& mw, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
                render_monitor(mw);
                ValidateRect(hwnd, nullptr);
                return 0;
            case WM_LBUTTONDOWN:
                on_lbutton_down(mw, lp);
                return 0;
            case WM_LBUTTONDBLCLK:
                if (open_dropdown_ == AudioDropdown::None && !hit_test(mw.hits, to_local_dip(mw, lp)) &&
                    has_selection_ && tool_ == DrawTool::Select && rect_contains_pt(selection_, to_virtual_px(mw, lp))) {
                    confirm_capture();
                }
                return 0;
            case WM_MOUSEMOVE:
                on_mouse_move(mw, lp);
                return 0;
            case WM_MOUSELEAVE:
                if (tooltip_hwnd_ == hwnd) {
                    hover_id_ = IdNone;
                    hover_idx_ = 0;
                    tooltip_.clear(hwnd, TIMER_TOOLTIP, true);
                    if (tooltip_.alpha.active()) start_tooltip_anim(hwnd);
                    render_monitor(mw);
                }
                return 0;
            case WM_TIMER:
                if (wp == TIMER_TOOLTIP) {
                    if (tooltip_hwnd_ != hwnd || open_dropdown_ != AudioDropdown::None || drag_kind_ != DragKind::None) {
                        KillTimer(hwnd, TIMER_TOOLTIP);
                        return 0;
                    }
                    std::wstring tip_title;
                    std::wstring tip_sub;
                    const bool ok = build_tooltip(hover_id_, hover_idx_, tip_title, tip_sub);
                    if (tooltip_.on_timer(hwnd, TIMER_TOOLTIP, ok, std::move(tip_title), std::move(tip_sub), true)) {
                        start_tooltip_anim(hwnd);
                        render_monitor(mw);
                    }
                    return 0;
                }
                if (wp == TIMER_TOOLTIP_ANIM) {
                    const auto now = std::chrono::steady_clock::now();
                    const float dt = std::clamp(std::chrono::duration<float>(now - last_tooltip_anim_).count(), 0.001f, 0.05f);
                    last_tooltip_anim_ = now;
                    const bool moving = tooltip_.step(dt);
                    render_monitor(mw);
                    if (!moving) KillTimer(hwnd, TIMER_TOOLTIP_ANIM);
                    return 0;
                }
                return 0;
            case WM_LBUTTONUP:
                on_lbutton_up(mw, lp);
                return 0;
            case WM_RBUTTONDOWN:
                dismiss_tooltip();
                if (mode_ == OverlayMode::ColorPicker) {
                    cancel();
                } else if (color_picker_open_) {
                    color_picker_open_ = false;
                    render_all();
                } else if (annotation_dropper_active_) {
                    annotation_dropper_active_ = false;
                    render_all();
                } else if (open_dropdown_ != AudioDropdown::None) {
                    open_dropdown_ = AudioDropdown::None;
                    render_all();
                } else if (has_selection_) {
                    has_selection_ = false;
                    annotations_.clear();
                    redo_stack_.clear();
                    tool_ = DrawTool::Select;
                    render_all();
                } else {
                    cancel();
                }
                return 0;
            case WM_MOUSEWHEEL: {
                const bool tip_vis = dismiss_tooltip();
                if (has_selection_ && tool_ != DrawTool::Select) {
                    const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                    if (delta > 0 && thick_idx_ + 1 < THICKNESSES.size()) ++thick_idx_;
                    else if (delta < 0 && thick_idx_ > 0) --thick_idx_;
                    render_all();
                } else if (tip_vis) {
                    render_monitor(mw);
                }
                return 0;
            }
            case WM_KEYDOWN: {
                const bool tip_vis = dismiss_tooltip();
                on_key_down(static_cast<UINT>(wp));
                if (tip_vis && active()) render_monitor(mw);
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    if (current_cursor_) SetCursor(current_cursor_);
                    return TRUE;
                }
                break;
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    Shared& sh_;
    HINSTANCE inst_ = nullptr;
    OverlayMode mode_ = OverlayMode::Screenshot;
    std::optional<VirtualCapture> capture_;
    SnapTargets snaps_;
    WindowInfo foreground_info_;
    std::vector<std::unique_ptr<MonitorWindow>> monitors_;
    wil::com_ptr<ID2D1StrokeStyle1> round_stroke_;

    bool has_selection_ = false;
    bool instant_capture_ = true;
    AudioDropdown open_dropdown_ = AudioDropdown::None;
    std::vector<AudioDeviceInfo> desktop_devices_;
    std::vector<AudioDeviceInfo> mic_devices_;
    RECT selection_{};
    std::optional<RECT> hover_snap_;
    std::optional<RECT> click_snap_candidate_;
    std::optional<LONG> snap_guide_x_;
    std::optional<LONG> snap_guide_y_;

    DragKind drag_kind_ = DragKind::None;
    POINT drag_start_{};
    RECT drag_orig_sel_{};

    DrawTool tool_ = DrawTool::Select;
    uint32_t current_color_rgb_ = PALETTE[0];
    float picker_h_ = 0.0f;
    float picker_s_ = 1.0f;
    float picker_v_ = 1.0f;
    bool color_picker_open_ = false;
    bool annotation_dropper_active_ = false;
    POINT cursor_vpt_{};
    D2D1_RECT_F color_picker_anchor_{};
    D2D1_RECT_F picker_panel_rect_{};
    D2D1_RECT_F picker_sv_rect_{};
    D2D1_RECT_F picker_hue_rect_{};
    ID2D1DeviceContext6* picker_bmp_dc_ = nullptr;
    wil::com_ptr<ID2D1Bitmap1> picker_sv_bmp_;
    wil::com_ptr<ID2D1Bitmap1> picker_hue_bmp_;
    float picker_sv_bmp_h_ = -1.0f;
    size_t thick_idx_ = 1;
    std::vector<Annotation> annotations_;
    std::vector<Annotation> redo_stack_;
    std::optional<Annotation> active_annotation_;

    int hover_id_ = IdNone;
    int hover_idx_ = 0;
    HWND tooltip_hwnd_ = nullptr;
    HoverTooltip tooltip_;
    std::chrono::steady_clock::time_point last_tooltip_anim_{};

    HCURSOR cursor_cross_ = nullptr;
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    HCURSOR cursor_move_ = nullptr;
    HCURSOR cursor_nwse_ = nullptr;
    HCURSOR cursor_nesw_ = nullptr;
    HCURSOR cursor_ns_ = nullptr;
    HCURSOR cursor_we_ = nullptr;
    HCURSOR current_cursor_ = nullptr;
};

} // namespace capusy::ui
