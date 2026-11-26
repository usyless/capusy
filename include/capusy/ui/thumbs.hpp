#pragma once

#include "gfx.hpp"

#include <shobjidl.h>

#include <condition_variable>
#include <deque>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace capusy::ui {

/// A decoded 32bpp BGRA raster.
struct Raster {
    UINT w = 0;
    UINT h = 0;
    std::vector<uint8_t> pixels;
};

/// Decodes an image with WIC, scaled down to fit `max_px` on its longer side (0 = native size).
inline std::optional<Raster> decode_image(const std::wstring& path, UINT max_px, bool premultiplied,
                                          IWICImagingFactory* shared_wic = nullptr) {
    constexpr uint64_t MAX_DECODE_PIXELS = 1ull << 28u; // 1 GiB at 4 bytes/pixel
    wil::com_ptr<IWICImagingFactory> owned_wic;
    IWICImagingFactory* wic = shared_wic;
    if (!wic) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&owned_wic)))) return std::nullopt;
        wic = owned_wic.get();
    }
    wil::com_ptr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return std::nullopt;
    wil::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return std::nullopt;
    UINT w = 0;
    UINT h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) return std::nullopt;
    if (max_px == 0 && static_cast<uint64_t>(w) * h > MAX_DECODE_PIXELS) return std::nullopt;

    wil::com_ptr<IWICBitmapSource> src = frame;
    const UINT longest = std::max(w, h);
    if (max_px && longest > max_px) {
        const double s = static_cast<double>(max_px) / longest;
        const UINT sw = std::max(1u, static_cast<UINT>(std::lround(w * s)));
        const UINT sh = std::max(1u, static_cast<UINT>(std::lround(h * s)));
        wil::com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler))) return std::nullopt;
        if (FAILED(scaler->Initialize(src.get(), sw, sh, WICBitmapInterpolationModeHighQualityCubic))) {
            wil::com_ptr<IWICFormatConverter> pre;
            if (FAILED(wic->CreateFormatConverter(&pre)) ||
                FAILED(pre->Initialize(src.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
                FAILED(scaler->Initialize(pre.get(), sw, sh, WICBitmapInterpolationModeHighQualityCubic))) {
                return std::nullopt;
            }
        }
        src = scaler;
    }
    wil::com_ptr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return std::nullopt;
    const WICPixelFormatGUID fmt = premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA;
    if (FAILED(conv->Initialize(src.get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return std::nullopt;
    Raster r;
    if (FAILED(conv->GetSize(&r.w, &r.h)) || !r.w || !r.h) return std::nullopt;
    const uint64_t total_pixels = static_cast<uint64_t>(r.w) * r.h;
    if (total_pixels > MAX_DECODE_PIXELS) return std::nullopt;
    const size_t stride = static_cast<size_t>(r.w) * 4;
    const size_t byte_size = static_cast<size_t>(total_pixels) * 4;
    r.pixels.resize(byte_size);
    if (FAILED(conv->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(byte_size), r.pixels.data()))) return std::nullopt;
    return r;
}

/// Async thumbnail & preview decoder with bounded LRU GPU bitmap cache (reused from usyseek).
class Thumbs {
public:
    explicit Thumbs(size_t max_bitmaps = 96, bool full = false) : max_bitmaps_{max_bitmaps}, full_{full} {}
    Thumbs(const Thumbs&) = delete;
    Thumbs& operator=(const Thumbs&) = delete;
    Thumbs(Thumbs&&) = delete;
    Thumbs& operator=(Thumbs&&) = delete;
    ~Thumbs() { stop(); }

    void start(HWND notify, UINT msg, unsigned threads = 2) {
        if (!workers_.empty()) stop();
        {
            const std::scoped_lock lk{m_};
            notify_ = notify;
            msg_ = msg;
            stopping_ = false;
        }
        for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this](const std::stop_token& st) { worker(st); });
    }

    void stop() noexcept {
        {
            const std::scoped_lock lk{m_};
            stopping_ = true;
            notify_ = nullptr;
        }
        cv_.notify_all();
        workers_.clear();
    }

    void request(uint64_t id, const std::wstring& path, UINT px) {
        if (px == 0 || failed_.contains(id)) return;
        if (const auto it = bitmaps_.find(id); it != bitmaps_.end() && it->second.px >= px) return;
        {
            const std::scoped_lock lk{m_};
            if (stopping_) return;
            if (!pending_.insert(id).second) return;
            queue_.push_front({.id = id, .path = path, .px = px, .gen = clear_gen_});
            while (queue_.size() > (full_ ? 4u : 48u)) {
                pending_.erase(queue_.back().id);
                queue_.pop_back();
            }
        }
        cv_.notify_one();
    }

    ID2D1Bitmap1* get(uint64_t id, UINT* out_px = nullptr) {
        const auto it = bitmaps_.find(id);
        if (it == bitmaps_.end()) return nullptr;
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        if (out_px) *out_px = it->second.px;
        return it->second.bmp.get();
    }

    [[nodiscard]] bool failed(uint64_t id) const { return failed_.contains(id); }

    bool process(ID2D1DeviceContext* dc) {
        if (!dc) return false;
        done_scratch_.clear();
        {
            const std::scoped_lock lk{m_};
            done_scratch_.swap(done_);
        }
        for (auto& d : done_scratch_) {
            if (d.pixels.empty() || d.w == 0 || d.h == 0 || d.w > 16384u || d.h > 16384u ||
                d.pixels.size() < static_cast<size_t>(d.w) * d.h * 4u) {
                failed_.insert(d.id);
                continue;
            }
            wil::com_ptr<ID2D1Bitmap1> bmp;
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            const HRESULT hr = dc->CreateBitmap(D2D1::SizeU(d.w, d.h), d.pixels.data(), d.w * 4u, &props, &bmp);
            if (FAILED(hr)) {
                if (hr != D2DERR_RECREATE_TARGET && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
                    failed_.insert(d.id);
                }
                continue;
            }
            if (const auto existing = bitmaps_.find(d.id); existing != bitmaps_.end()) {
                if (d.px >= existing->second.px) {
                    existing->second.bmp = std::move(bmp);
                    existing->second.px = d.px;
                }
                lru_.splice(lru_.begin(), lru_, existing->second.lru);
            } else {
                lru_.push_front(d.id);
                bitmaps_[d.id] = {.bmp = std::move(bmp), .lru = lru_.begin(), .px = d.px};
                while (bitmaps_.size() > max_bitmaps_) {
                    bitmaps_.erase(lru_.back());
                    lru_.pop_back();
                }
            }
        }
        return !done_scratch_.empty();
    }

    void reset_gpu() noexcept {
        bitmaps_.clear();
        lru_.clear();
    }

    void evict(uint64_t id) {
        failed_.erase(id);
        if (const auto it = bitmaps_.find(id); it != bitmaps_.end()) {
            lru_.erase(it->second.lru);
            bitmaps_.erase(it);
        }
    }

    void clear() {
        {
            const std::scoped_lock lk{m_};
            ++clear_gen_;
            pending_.clear();
            queue_.clear();
            done_.clear();
        }
        reset_gpu();
        failed_.clear();
    }

private:
    size_t max_bitmaps_;
    bool full_;

    struct Req {
        uint64_t id = 0;
        std::wstring path;
        UINT px = 0;
        uint64_t gen = 0;
    };
    struct Done {
        uint64_t id = 0;
        UINT w = 0;
        UINT h = 0;
        UINT px = 0;
        std::vector<uint8_t> pixels;
    };
    struct Entry {
        wil::com_ptr<ID2D1Bitmap1> bmp;
        std::list<uint64_t>::iterator lru;
        UINT px = 0;
    };

    void worker(const std::stop_token& st) {
        const DWORD com_flags = (full_ ? static_cast<DWORD>(COINIT_MULTITHREADED) : static_cast<DWORD>(COINIT_APARTMENTTHREADED)) |
                                static_cast<DWORD>(COINIT_DISABLE_OLE1DDE);
        const ulp::win::COMWrapper<false> com{com_flags};
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        wil::com_ptr<IWICImagingFactory> wic;
        (void)CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        while (!st.stop_requested()) {
            Req r;
            {
                std::unique_lock lk{m_};
                cv_.wait(lk, [&] { return stopping_ || st.stop_requested() || !queue_.empty(); });
                if (stopping_ || st.stop_requested()) return;
                r = std::move(queue_.front());
                queue_.pop_front();
            }
            Done d{.id = r.id, .px = r.px};
            if (full_) {
                if (auto img = decode_image(r.path, r.px, true, wic.get())) {
                    d.w = img->w;
                    d.h = img->h;
                    d.pixels = std::move(img->pixels);
                }
            } else {
                // Fast WIC decode first for PNG/JPG; fall back to Windows Shell thumbnailer for MP4s.
                if (auto img = decode_image(r.path, r.px, true, wic.get())) {
                    d.w = img->w;
                    d.h = img->h;
                    d.pixels = std::move(img->pixels);
                } else {
                    load_shell_thumb(r, d);
                }
            }
            HWND notify_hwnd = nullptr;
            UINT notify_msg = 0;
            {
                const std::scoped_lock lk{m_};
                if (!stopping_ && r.gen == clear_gen_) {
                    pending_.erase(r.id);
                    done_.push_back(std::move(d));
                    notify_hwnd = notify_;
                    notify_msg = msg_;
                }
            }
            if (notify_hwnd && notify_msg) PostMessageW(notify_hwnd, notify_msg, 0, 0);
        }
    }

    static void load_shell_thumb(const Req& r, Done& d) {
        wil::com_ptr<IShellItemImageFactory> factory;
        if (FAILED(SHCreateItemFromParsingName(r.path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return;
        HBITMAP raw = nullptr;
        if (FAILED(factory->GetImage(SIZE{static_cast<LONG>(r.px), static_cast<LONG>(r.px)},
                                     static_cast<SIIGBF>(static_cast<unsigned>(SIIGBF_RESIZETOFIT) | static_cast<unsigned>(SIIGBF_BIGGERSIZEOK)), &raw)) || !raw) return;
        const wil::unique_hbitmap bmp{raw};
        BITMAP info{};
        if (!GetObjectW(bmp.get(), sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight == 0) return;
        const UINT w = static_cast<UINT>(info.bmWidth);
        const UINT h = info.bmHeight < 0 ? static_cast<UINT>(-info.bmHeight) : static_cast<UINT>(info.bmHeight);
        if (w > 16384u || h > 16384u) return;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = static_cast<LONG>(w);
        bi.bmiHeader.biHeight = -static_cast<LONG>(h);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        const wil::unique_hdc_window screen{GetDC(nullptr)};
        if (!screen) return;
        d.pixels.resize(static_cast<size_t>(w) * h * 4);
        if (GetDIBits(screen.get(), bmp.get(), 0, h, d.pixels.data(), &bi, DIB_RGB_COLORS) != static_cast<int>(h)) {
            d.pixels.clear();
            return;
        }
        bool any_alpha = false;
        for (size_t i = 3; i < d.pixels.size(); i += 4) {
            if (d.pixels[i]) { any_alpha = true; break; }
        }
        if (!any_alpha) {
            for (size_t i = 3; i < d.pixels.size(); i += 4) d.pixels[i] = 255;
        }
        d.w = w;
        d.h = h;
    }

    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Req> queue_;
    std::unordered_set<uint64_t> pending_;
    std::vector<Done> done_;
    std::vector<Done> done_scratch_;
    uint64_t clear_gen_ = 0;
    bool stopping_ = false;
    std::vector<std::jthread> workers_;

    std::unordered_map<uint64_t, Entry> bitmaps_;
    std::list<uint64_t> lru_;
    std::unordered_set<uint64_t> failed_;
};

} // namespace capusy::ui
