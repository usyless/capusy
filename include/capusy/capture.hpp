#pragma once

#include "common.hpp"
#include "settings.hpp"
#include "ui/thumbs.hpp"
#include "wgc_capture.hpp"

#include <dwmapi.h>
#include <propidl.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace capusy {

struct MonitorInfo {
    HMONITOR hmon = nullptr;
    RECT rect{}; // physical virtual-screen pixels
    RECT work{};
    UINT dpi = 96;
    bool primary = false;
};

struct VirtualCapture {
    RECT bounds{}; // SM_XVIRTUALSCREEN..SM_CXVIRTUALSCREEN
    std::vector<MonitorInfo> monitors;
    ui::Raster raster; // 32bpp BGRA opaque, non-monitor blanks filled solid black (0,0,0,255)
};

/// Pure snap candidate rectangle (in virtual-screen physical pixels).
struct SnapWindow {
    RECT outer{};               // DWM extended frame bounds (visual window border)
    RECT client{};              // inner client area (may equal outer)
    std::vector<RECT> children; // visible child control rects inside this window
    int z_order = 0;            // 0 = frontmost
    WindowInfo info{};          // window title & executable name for history metadata
};

struct SnapTargets {
    std::vector<RECT> monitors;
    std::vector<RECT> work_areas;
    std::vector<SnapWindow> windows; // ordered front-to-back (z_order ascending)
    std::vector<LONG> x_guides;      // sorted unique vertical guide positions
    std::vector<LONG> y_guides;      // sorted unique horizontal guide positions
};

[[nodiscard]] inline bool rect_contains_pt(const RECT& r, POINT pt) noexcept {
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

[[nodiscard]] inline bool rect_valid(const RECT& r) noexcept {
    return r.right > r.left && r.bottom > r.top;
}

[[nodiscard]] inline RECT rect_intersect(const RECT& a, const RECT& b) noexcept {
    RECT r{
        .left = std::max(a.left, b.left),
        .top = std::max(a.top, b.top),
        .right = std::min(a.right, b.right),
        .bottom = std::min(a.bottom, b.bottom),
    };
    if (r.right <= r.left || r.bottom <= r.top) return {};
    return r;
}

[[nodiscard]] inline int64_t rect_area(const RECT& r) noexcept {
    if (!rect_valid(r)) return 0;
    return static_cast<int64_t>(r.right - r.left) * static_cast<int64_t>(r.bottom - r.top);
}

/// Builds sorted unique X/Y magnetic snap guides from all collected windows and monitors.
inline void rebuild_snap_guides(SnapTargets& targets) {
    targets.x_guides.clear();
    targets.y_guides.clear();
    const auto add_rect = [&](const RECT& r) {
        if (!rect_valid(r)) return;
        targets.x_guides.push_back(r.left);
        targets.x_guides.push_back(r.right);
        targets.y_guides.push_back(r.top);
        targets.y_guides.push_back(r.bottom);
    };
    for (const RECT& m : targets.monitors) add_rect(m);
    for (const RECT& w : targets.work_areas) add_rect(w);
    // Top 20 frontmost windows provide crisp magnetic snap edges without cluttering every pixel.
    const size_t lim = std::min<size_t>(targets.windows.size(), 20u);
    for (size_t i = 0; i < lim; ++i) {
        add_rect(targets.windows[i].outer);
        add_rect(targets.windows[i].client);
        for (const RECT& c : targets.windows[i].children) add_rect(c);
    }
    std::ranges::sort(targets.x_guides);
    auto [ux_first, ux_last] = std::ranges::unique(targets.x_guides);
    targets.x_guides.erase(ux_first, ux_last);
    std::ranges::sort(targets.y_guides);
    auto [uy_first, uy_last] = std::ranges::unique(targets.y_guides);
    targets.y_guides.erase(uy_first, uy_last);
}

/// Returns the best hover-snap rectangle under `pt`, respecting Z-order occlusion.
/// Prefers the smallest visible child element inside the frontmost window under `pt`,
/// then its client area (if cursor is clearly inside), then the window's extended frame,
/// falling back to monitor work area / full monitor.
[[nodiscard]] inline std::optional<RECT> hit_snap_rect(const SnapTargets& targets, POINT pt) noexcept {
    for (const SnapWindow& win : targets.windows) {
        if (!rect_contains_pt(win.outer, pt)) continue;

        // Check smallest child control under cursor inside this frontmost window.
        std::optional<RECT> best_child;
        int64_t best_area = rect_area(win.outer);
        for (const RECT& c : win.children) {
            if (!rect_contains_pt(c, pt)) continue;
            const int64_t a = rect_area(c);
            if (a > 0 && a < best_area) {
                best_area = a;
                best_child = c;
            }
        }
        if (best_child) return best_child;

        // If the window has a distinct client area and cursor is well inside it (away from border/titlebar), offer outer or client:
        // Offer outer when near window border/titlebar, or if client == outer.
        if (rect_valid(win.client) && rect_contains_pt(win.client, pt) &&
            (win.client.top - win.outer.top >= 12) &&
            pt.x >= win.client.left + 8 && pt.x < win.client.right - 8 &&
            pt.y >= win.client.top + 8 && pt.y < win.client.bottom - 8) {
            // Only pick client if noticeably smaller than outer; otherwise outer is usually preferred.
            const int64_t ca = rect_area(win.client);
            const int64_t oa = rect_area(win.outer);
            if (ca > 0 && ca < (oa * 92) / 100) return win.client;
        }
        return win.outer;
    }

    for (const RECT& w : targets.work_areas) {
        if (rect_contains_pt(w, pt)) return w;
    }
    for (const RECT& m : targets.monitors) {
        if (rect_contains_pt(m, pt)) return m;
    }
    return std::nullopt;
}

/// Magnetically snaps `val` to the nearest guide within `threshold` pixels.
[[nodiscard]] inline LONG snap_coordinate(LONG val, std::span<const LONG> sorted_guides, LONG threshold, std::optional<LONG>& snapped_guide) noexcept {
    snapped_guide.reset();
    if (sorted_guides.empty() || threshold <= 0) return val;
    const auto it = std::ranges::lower_bound(sorted_guides, val);
    LONG best = val;
    LONG best_dist = threshold + 1;
    if (it != sorted_guides.end()) {
        const LONG d = std::abs(*it - val);
        if (d < best_dist) { best_dist = d; best = *it; }
    }
    if (it != sorted_guides.begin()) {
        const auto prev = std::prev(it);
        const LONG d = std::abs(*prev - val);
        if (d < best_dist) { best_dist = d; best = *prev; }
    }
    if (best_dist <= threshold) {
        snapped_guide = best;
        return best;
    }
    return val;
}

inline std::vector<MonitorInfo> enumerate_monitors() {
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hmon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
        MONITORINFO mi{.cbSize = sizeof(mi)};
        if (GetMonitorInfoW(hmon, &mi)) {
            UINT dx = 96;
            UINT dy = 96;
            if (FAILED(GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
            list->push_back(MonitorInfo{
                .hmon = hmon,
                .rect = mi.rcMonitor,
                .work = mi.rcWork,
                .dpi = dx,
                .primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0,
            });
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

/// Snapshots all visible top-level windows (front-to-back Z-order), client areas, child controls,
/// and monitor rects BEFORE showing the region overlay. Fast (< 3 ms).
inline SnapTargets enumerate_snap_targets(const std::vector<MonitorInfo>& monitors) {
    SnapTargets targets;
    RECT vbounds{};
    vbounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vbounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vbounds.right = vbounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vbounds.bottom = vbounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    for (const MonitorInfo& m : monitors) {
        targets.monitors.push_back(m.rect);
        targets.work_areas.push_back(m.work);
    }

    struct EnumCtx {
        SnapTargets* targets;
        DWORD self_pid;
        RECT vbounds;
        int z;
    };
    EnumCtx ctx{.targets = &targets, .self_pid = GetCurrentProcessId(), .vbounds = vbounds, .z = 0};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == c->self_pid) return TRUE;

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return TRUE;
        }

        // Skip desktop wallpaper / invisible shell hosts
        std::array<wchar_t, 64> cls{};
        const int clen = GetClassNameW(hwnd, cls.data(), static_cast<int>(cls.size()));
        if (clen > 0) {
            const std::wstring_view sv{cls.data(), static_cast<size_t>(clen)};
            if (sv == L"Progman" || sv == L"WorkerW" || sv == L"Windows.UI.Core.CoreWindow" ||
                sv == L"ApplicationManager_ImmersiveShellWindow" || sv == L"EdgeUiInputTopWndClass") {
                return TRUE;
            }
        }

        RECT outer{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &outer, sizeof(outer)))) {
            if (!GetWindowRect(hwnd, &outer)) return TRUE;
        }
        outer = rect_intersect(outer, c->vbounds);
        if ((outer.right - outer.left) < 12 || (outer.bottom - outer.top) < 12) return TRUE;

        // Skip layered fully transparent windows
        const auto ex = static_cast<ULONG_PTR>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        if ((ex & WS_EX_LAYERED) != 0u) {
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) {
                return TRUE;
            }
        }

        SnapWindow sw;
        sw.outer = outer;
        sw.z_order = c->z++;
        sw.info = query_window_info(hwnd);

        RECT cr{};
        if (GetClientRect(hwnd, &cr) && cr.right > cr.left && cr.bottom > cr.top) {
            POINT tl{.x = cr.left, .y = cr.top};
            POINT br{.x = cr.right, .y = cr.bottom};
            ClientToScreen(hwnd, &tl);
            ClientToScreen(hwnd, &br);
            sw.client = rect_intersect(RECT{.left = tl.x, .top = tl.y, .right = br.x, .bottom = br.y}, outer);
        } else {
            sw.client = outer;
        }

        // Collect visible child controls for the top 12 foreground windows to keep enumeration < 2ms.
        if (sw.z_order < 12) {
            struct ChildCtx {
                SnapWindow* sw;
                RECT clip;
            };
            ChildCtx cctx{.sw = &sw, .clip = outer};
            EnumChildWindows(hwnd, [](HWND ch, LPARAM clp) -> BOOL {
                const auto* cc = reinterpret_cast<const ChildCtx*>(clp);
                if (cc->sw->children.size() >= 48u) return FALSE;
                if (!IsWindowVisible(ch)) return TRUE;
                RECT r{};
                if (!GetWindowRect(ch, &r)) return TRUE;
                r = rect_intersect(r, cc->clip);
                const LONG w = r.right - r.left;
                const LONG h = r.bottom - r.top;
                if (w < 20 || h < 16) return TRUE;
                if (r.left == cc->clip.left && r.top == cc->clip.top && r.right == cc->clip.right && r.bottom == cc->clip.bottom) return TRUE;
                cc->sw->children.push_back(r);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&cctx));
        }

        c->targets->windows.push_back(std::move(sw));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    rebuild_snap_guides(targets);
    return targets;
}

/// Captures the entire virtual screen across ALL monitors in parallel via Windows.Graphics.Capture
/// (borderless on Windows 11, native DWM cursor composition & HDR-to-SDR tone mapping).
/// Any dead space / blank gaps outside monitors are guaranteed filled with solid opaque black (0, 0, 0, 255).
/// Passing an existing multithread-protected `shared_d3d` avoids creating a temporary D3D11 device.
inline std::optional<VirtualCapture> capture_virtual_screen(bool include_cursor = false,
                                                            ID3D11Device* shared_d3d = nullptr) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return std::nullopt;

    auto monitors = enumerate_monitors();
    if (monitors.empty()) return std::nullopt;

    std::vector<WgcMonitorRect> wgc_mons;
    wgc_mons.reserve(monitors.size());
    for (const MonitorInfo& m : monitors) {
        if (m.hmon) {
            wgc_mons.push_back(WgcMonitorRect{.hmon = m.hmon, .rect = m.rect});
        }
    }
    if (wgc_mons.empty()) return std::nullopt;

    const RECT bounds{.left = vx, .top = vy, .right = vx + vw, .bottom = vy + vh};
    auto raster = wgc_capture_monitors_to_raster(wgc_mons, bounds, include_cursor, shared_d3d);
    if (!raster) return std::nullopt;

    VirtualCapture out;
    out.bounds = bounds;
    out.monitors = std::move(monitors);
    out.raster = std::move(*raster);
    return out;
}

/// Crops a sub-rectangle `crop_rect` (in virtual-screen coordinates) out of `cap`.
/// Areas outside `cap.bounds` remain opaque black.
inline std::optional<ui::Raster> crop_virtual_capture(const VirtualCapture& cap, RECT crop_rect) {
    const int cw = crop_rect.right - crop_rect.left;
    const int ch = crop_rect.bottom - crop_rect.top;
    if (cw <= 0 || ch <= 0 || cap.raster.pixels.empty()) return std::nullopt;

    ui::Raster out;
    out.w = static_cast<UINT>(cw);
    out.h = static_cast<UINT>(ch);
    const size_t pixel_count = static_cast<size_t>(cw) * static_cast<size_t>(ch);
    out.pixels.resize(pixel_count * 4u, 0);
    auto* dst32 = reinterpret_cast<uint32_t*>(out.pixels.data());
    for (size_t i = 0; i < pixel_count; ++i) dst32[i] = 0xFF000000u;

    const RECT isect = rect_intersect(crop_rect, cap.bounds);
    if (!rect_valid(isect)) return out;

    const int copy_w = isect.right - isect.left;
    const int copy_h = isect.bottom - isect.top;
    const int src_x0 = isect.left - cap.bounds.left;
    const int src_y0 = isect.top - cap.bounds.top;
    const int dst_x0 = isect.left - crop_rect.left;
    const int dst_y0 = isect.top - crop_rect.top;

    const size_t src_stride = static_cast<size_t>(cap.raster.w) * 4u;
    const size_t dst_stride = static_cast<size_t>(out.w) * 4u;
    const size_t row_bytes = static_cast<size_t>(copy_w) * 4u;
    for (int y = 0; y < copy_h; ++y) {
        const uint8_t* src = cap.raster.pixels.data() + (static_cast<size_t>(src_y0 + y) * src_stride) + (static_cast<size_t>(src_x0) * 4u);
        uint8_t* dst = out.pixels.data() + (static_cast<size_t>(dst_y0 + y) * dst_stride) + (static_cast<size_t>(dst_x0) * 4u);
        std::memcpy(dst, src, row_bytes);
    }
    return out;
}

/// Resolves the most relevant window title & process name for a captured region `r`.
/// Prefers the frontmost visible window containing the region's center, then largest overlap, falling back to `fallback`.
[[nodiscard]] inline WindowInfo window_info_for_rect(const SnapTargets& targets, RECT r, const WindowInfo& fallback = {}) {
    if (rect_valid(r)) {
        const POINT center{.x = (r.left + r.right) / 2, .y = (r.top + r.bottom) / 2};
        for (const SnapWindow& win : targets.windows) {
            if (rect_contains_pt(win.outer, center) && (!win.info.title.empty() || !win.info.process_name.empty())) {
                return win.info;
            }
        }
        int64_t best_area = 0;
        const WindowInfo* best_info = nullptr;
        for (const SnapWindow& win : targets.windows) {
            if (win.info.title.empty() && win.info.process_name.empty()) continue;
            const int64_t a = rect_area(rect_intersect(win.outer, r));
            if (a > best_area) {
                best_area = a;
                best_info = &win.info;
            }
        }
        if (best_info) return *best_info;
    }
    return fallback;
}

/// Generates a unique timestamped filename in `dir` (optionally nested in `YYYY-MM`) with an atomic sequence suffix
/// so unlimited rapid-fire screenshots never collide.
inline fs::path make_capture_path(const fs::path& dir, std::wstring_view ext, bool split_by_month = true,
                                  std::wstring_view prefix = {}) {
    static std::atomic<uint64_t> seq{0};
    const uint64_t n = ++seq;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const CaptureDateTime dt{
        .year = st.wYear,
        .month = st.wMonth,
        .day = st.wDay,
        .hour = st.wHour,
        .minute = st.wMinute,
        .second = st.wSecond,
        .millisecond = st.wMilliseconds,
    };
    return make_capture_path_for_time(dir, ext, dt, n, split_by_month, prefix);
}

/// Encodes a 32bpp BGRA raster to the chosen WIC container format (PNG, JPEG, BMP, TIFF),
/// applying JPEG quality (10..100) when applicable and optionally returning file size in bytes.
inline bool save_raster_image(const ui::Raster& raster, const fs::path& path, ImageFormat format = ImageFormat::Png,
                              int quality = 90, uint64_t* out_size = nullptr, IWICImagingFactory* shared_wic = nullptr) {
    if (raster.w == 0 || raster.h == 0 || raster.pixels.size() < static_cast<size_t>(raster.w) * raster.h * 4u) {
        return false;
    }
    wil::com_ptr<IWICImagingFactory> owned_wic;
    IWICImagingFactory* wic = shared_wic;
    if (!wic) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&owned_wic)))) return false;
        wic = owned_wic.get();
    }

    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);

    GUID container = GUID_ContainerFormatPng;
    switch (format) {
        case ImageFormat::Png: container = GUID_ContainerFormatPng; break;
        case ImageFormat::Jpeg: container = GUID_ContainerFormatJpeg; break;
        case ImageFormat::Bmp: container = GUID_ContainerFormatBmp; break;
        case ImageFormat::Tiff: container = GUID_ContainerFormatTiff; break;
    }

    wil::com_ptr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    wil::com_ptr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(container, nullptr, &enc)) || FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;

    wil::com_ptr<IWICBitmapFrameEncode> frame;
    wil::com_ptr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;

    if (format == ImageFormat::Jpeg && props) {
        const float q = std::clamp(static_cast<float>(quality) / 100.0f, 0.1f, 1.0f);
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT var{};
        VariantInit(&var);
        var.vt = VT_R4;
        var.fltVal = q;
        (void)props->Write(1, &opt, &var);

        if (quality >= 90) {
            PROPBAG2 sub{};
            sub.pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
            VARIANT svar{};
            VariantInit(&svar);
            svar.vt = VT_UI1;
            svar.bVal = WICJpegYCrCbSubsampling444;
            (void)props->Write(1, &sub, &svar);
        }
    }

    if (FAILED(frame->Initialize(props.get()))) return false;
    if (FAILED(frame->SetSize(raster.w, raster.h))) return false;

    const WICPixelFormatGUID desired = (format == ImageFormat::Jpeg) ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    WICPixelFormatGUID negotiated = desired;
    if (FAILED(frame->SetPixelFormat(&negotiated))) return false;

    const UINT stride = raster.w * 4u;
    const UINT buf_size = static_cast<UINT>(static_cast<size_t>(stride) * raster.h);

    if (negotiated == GUID_WICPixelFormat32bppBGRA) {
        // Direct zero-copy write for 32bpp BGRA encoders (PNG, BMP, TIFF).
        if (FAILED(frame->WritePixels(raster.h, stride, buf_size, const_cast<BYTE*>(raster.pixels.data())))) return false;
    } else {
        // Convert on-the-fly from 32bpp BGRA to the encoder's negotiated format (e.g., 24bpp BGR for JPEG).
        wil::com_ptr<IWICBitmap> src_bmp;
        if (FAILED(wic->CreateBitmapFromMemory(raster.w, raster.h, GUID_WICPixelFormat32bppBGRA, stride, buf_size,
                                               const_cast<BYTE*>(raster.pixels.data()), &src_bmp))) {
            return false;
        }
        wil::com_ptr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(src_bmp.get(), negotiated, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            return false;
        }
        if (FAILED(frame->WriteSource(conv.get(), nullptr))) return false;
    }

    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;
    stream.reset();

    if (out_size) {
        *out_size = fs::file_size(path, ec);
        if (ec) *out_size = 0;
    }
    return true;
}

/// Encodes a 32bpp BGRA raster to PNG via WIC, optionally returning file size in bytes.
inline bool save_raster_png(const ui::Raster& raster, const fs::path& path, uint64_t* out_size = nullptr,
                            IWICImagingFactory* shared_wic = nullptr) {
    return save_raster_image(raster, path, ImageFormat::Png, 100, out_size, shared_wic);
}

} // namespace capusy
