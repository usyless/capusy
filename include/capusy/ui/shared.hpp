#pragma once

#include "canvas.hpp"
#include "thumbs.hpp"

#include <capusy/history_store.hpp>
#include <capusy/hotkeys.hpp>
#include <capusy/settings.hpp>
#include <capusy/update_logic.hpp>

#include <atomic>
#include <dwmapi.h>
#include <limits>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace capusy::ui {

inline constexpr UINT WM_APP_THUMBS = WM_APP + 10;
inline constexpr UINT WM_APP_CALL = WM_APP + 11;
inline constexpr UINT WM_APP_VIDEO = WM_APP + 12;
inline constexpr UINT WM_APP_CAPTURE_SAVED = WM_APP + 13;

/// Shared application state (UI thread only unless noted).
struct Shared {
    Gfx gfx;
    Fonts fonts;
    Settings settings;
    HistoryStore history;
    Thumbs thumbs{96, false};
    Thumbs previews{4, true};

    std::array<HotkeyStatus, HOTKEY_COUNT> hotkey_status{};
    std::atomic<HWND> call_hwnd{nullptr};
    bool recording_active = false;
    int background_jobs = 0;

    UpdateState update_state;
    bool update_checking = false;

    [[nodiscard]] bool has_update_available() const noexcept {
        return !update_state.latest_version.empty() && is_newer_version(update_state.latest_version, to_utf8(APP_VERSION));
    }

    std::function<void()> on_capture_full;
    std::function<void()> on_capture_region;
    std::function<void()> on_toggle_record;
    std::function<void()> on_open_history;
    std::function<void(int)> on_open_settings;
    std::function<void()> on_reload_hotkeys;
    std::function<void(bool)> on_suspend_hotkeys;
    std::function<void()> on_test_toast;
    std::function<void()> on_visibility_changed;
    std::function<void()> on_history_changed;
    std::function<void()> on_redraw_all;
    std::function<void(std::wstring_view)> on_update_balloon;

    void call(std::function<void()> fn) {
        const HWND hwnd = call_hwnd.load(std::memory_order_acquire);
        if (!hwnd) return;
        auto p = std::make_unique<std::function<void()>>(std::move(fn));
        const auto* const raw = p.release();
        if (!PostMessageW(hwnd, WM_APP_CALL, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    }

    void save() {
        (void)save_settings(settings);
    }
};

/// Title-bar/taskbar icons loaded at the window's current DPI.
struct WindowIcons {
    wil::unique_hicon big_icon;
    wil::unique_hicon small_icon;

    void apply(HWND hwnd, HINSTANCE inst, int res_id) noexcept {
        if (!hwnd) return;
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0) dpi = GetDpiForSystem();
        const auto load = [&](int cx_metric, int cy_metric) noexcept {
            return static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(res_id), IMAGE_ICON,
                                                 GetSystemMetricsForDpi(cx_metric, dpi),
                                                 GetSystemMetricsForDpi(cy_metric, dpi), LR_DEFAULTCOLOR));
        };
        wil::unique_hicon new_big{load(SM_CXICON, SM_CYICON)};
        wil::unique_hicon new_small{load(SM_CXSMICON, SM_CYSMICON)};
        if (new_big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(new_big.get()));
        if (new_small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(new_small.get()));
        if (new_big) big_icon = std::move(new_big);
        if (new_small) small_icon = std::move(new_small);
    }
};

inline void cloak_window(HWND hwnd, bool cloak) noexcept {
    if (!hwnd) return;
    const BOOL val = cloak ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &val, sizeof(val));
}

inline void exclude_window_from_capture(HWND hwnd, bool exclude = true) noexcept {
    if (!hwnd) return;
    SetWindowDisplayAffinity(hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

/// Applies Midnight Dark DWM immersive frame, Mica/Acrylic backdrop, and rounded corners.
inline void apply_dark_window(HWND hwnd, int backdrop = DWMSBT_MAINWINDOW, bool round = true) noexcept {
    if (!hwnd) return;
    ulp::win::dark::allow_dark_mode_for_window(hwnd, true);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corner = round ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    const MARGINS m{.cxLeftWidth = -1, .cxRightWidth = -1, .cyTopHeight = -1, .cyBottomHeight = -1};
    DwmExtendFrameIntoClientArea(hwnd, &m);
}

inline bool open_file(const std::wstring& path) {
    SHELLEXECUTEINFOW sei{.cbSize = sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

inline bool reveal_in_explorer(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) return false;
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    return SUCCEEDED(hr);
}

/// Moves a file to the Windows Recycle Bin (falls back to std::filesystem::remove if Recycle Bin fails).
inline bool delete_file_to_recycle_bin(const std::wstring& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;
    std::wstring double_nul = path;
    double_nul.push_back(L'\0');
    double_nul.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = double_nul.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) return true;
    return fs::remove(path, ec);
}

inline std::optional<std::wstring> browse_folder_dialog(HWND owner, const wchar_t* title, const std::wstring& initial) {
    wil::com_ptr<IFileDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        return std::nullopt;
    }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    if (title) dlg->SetTitle(title);
    if (!initial.empty()) {
        wil::com_ptr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            dlg->SetFolder(item.get());
        }
    }
    if (FAILED(dlg->Show(owner))) return std::nullopt;
    wil::com_ptr<IShellItem> res;
    if (FAILED(dlg->GetResult(&res)) || !res) return std::nullopt;
    PWSTR p = nullptr;
    if (FAILED(res->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return std::nullopt;
    std::wstring out{p};
    CoTaskMemFree(p);
    return out;
}

// ---------------------------------------------------------------------------------------------
// OLE Drag & Drop + Clipboard (exact architecture reused from usyseek)
// ---------------------------------------------------------------------------------------------

inline bool set_data_bytes(IDataObject* obj, CLIPFORMAT cf, const void* data, size_t n) noexcept {
    if (!obj || !data || n == 0) return false;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!h) return false;
    void* p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return false;
    }
    std::memcpy(p, data, n);
    GlobalUnlock(h);
    FORMATETC fe{.cfFormat = cf, .ptd = nullptr, .dwAspect = DVASPECT_CONTENT, .lindex = -1, .tymed = TYMED_HGLOBAL};
    STGMEDIUM sm{};
    sm.tymed = TYMED_HGLOBAL;
    sm.hGlobal = h;
    if (SUCCEEDED(obj->SetData(&fe, &sm, TRUE))) return true;
    GlobalFree(h);
    return false;
}

inline wil::com_ptr<IDataObject> media_data_object(std::span<const std::wstring> paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    std::vector<std::wstring> found;
    pidls.reserve(paths.size());
    found.reserve(paths.size());
    for (const auto& p : paths) {
        if (PIDLIST_ABSOLUTE id = ILCreateFromPathW(p.c_str())) {
            pidls.push_back(id);
            found.push_back(p);
        }
    }
    const auto free_pidls = wil::scope_exit([&] {
        for (auto* id : pidls) ILFree(id);
    });
    if (pidls.empty()) return nullptr;

    PIDLIST_ABSOLUTE parent = ILCloneFull(pidls.front());
    if (!parent) return nullptr;
    const auto free_parent = wil::scope_exit([&] { ILFree(parent); });
    ILRemoveLastID(parent);
    bool same_parent = true;
    for (const auto* id : pidls) {
        PIDLIST_ABSOLUTE pp = ILCloneFull(id);
        if (!pp) return nullptr;
        ILRemoveLastID(pp);
        const bool eq = ILIsEqual(pp, parent) != FALSE;
        ILFree(pp);
        if (!eq) {
            same_parent = false;
            break;
        }
    }
    std::vector<PCUITEMID_CHILD> children;
    children.reserve(pidls.size());
    for (const auto* id : pidls) children.push_back(same_parent ? ILFindLastID(id) : reinterpret_cast<PCUITEMID_CHILD>(id));
    wil::com_ptr<IDataObject> obj;
    if (FAILED(SHCreateDataObject(same_parent ? parent : nullptr, static_cast<UINT>(children.size()), children.data(), nullptr, IID_PPV_ARGS(&obj)))) return nullptr;

    std::vector<uint8_t> buf(sizeof(DROPFILES));
    for (const auto& p : found) {
        const auto* b = reinterpret_cast<const uint8_t*>(p.c_str());
        buf.insert(buf.end(), b, b + ((p.size() + 1) * sizeof(wchar_t)));
    }
    buf.insert(buf.end(), sizeof(wchar_t), 0);
    DROPFILES df{};
    df.pFiles = sizeof(DROPFILES);
    df.fWide = TRUE;
    std::memcpy(buf.data(), &df, sizeof(df));
    set_data_bytes(obj.get(), CF_HDROP, buf.data(), buf.size());

    const DWORD effect = DROPEFFECT_COPY;
    set_data_bytes(obj.get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)), &effect, sizeof(effect));
    return obj;
}

class AsyncDataObject final : public IDataObject, public IDataObjectAsyncCapability {
public:
    explicit AsyncDataObject(wil::com_ptr<IDataObject> inner) noexcept : inner_(std::move(inner)) {}
    AsyncDataObject(const AsyncDataObject&) = delete;
    AsyncDataObject& operator=(const AsyncDataObject&) = delete;
    AsyncDataObject(AsyncDataObject&&) = delete;
    AsyncDataObject& operator=(AsyncDataObject&&) = delete;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) noexcept override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDataObject)) {
            *ppv = static_cast<IDataObject*>(this);
        } else if (riid == __uuidof(IDataObjectAsyncCapability)) {
            *ppv = static_cast<IDataObjectAsyncCapability*>(this);
        }
        if (*ppv) {
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() noexcept override { return ++ref_count_; }
    IFACEMETHODIMP_(ULONG) Release() noexcept override {
        const ULONG count = --ref_count_;
        if (count == 0) delete this;
        return count;
    }

    IFACEMETHODIMP SetAsyncMode(BOOL fDoOpAsync) noexcept override {
        async_mode_ = fDoOpAsync ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }
    IFACEMETHODIMP GetAsyncMode(BOOL* pfIsOpAsync) noexcept override {
        if (!pfIsOpAsync) return E_POINTER;
        *pfIsOpAsync = async_mode_;
        return S_OK;
    }
    IFACEMETHODIMP StartOperation(IBindCtx*) noexcept override {
        in_operation_ = VARIANT_TRUE;
        return S_OK;
    }
    IFACEMETHODIMP InOperation(BOOL* pfInAsyncOp) noexcept override {
        if (!pfInAsyncOp) return E_POINTER;
        *pfInAsyncOp = in_operation_;
        return S_OK;
    }
    IFACEMETHODIMP EndOperation(HRESULT, IBindCtx*, DWORD) noexcept override {
        in_operation_ = VARIANT_FALSE;
        return S_OK;
    }

    [[nodiscard]] bool is_operation_active() const noexcept {
        return in_operation_.load(std::memory_order_acquire) != VARIANT_FALSE &&
               ref_count_.load(std::memory_order_acquire) > 1;
    }

    IFACEMETHODIMP GetData(FORMATETC* f, STGMEDIUM* m) noexcept override { return inner_ ? inner_->GetData(f, m) : E_UNEXPECTED; }
    IFACEMETHODIMP GetDataHere(FORMATETC* f, STGMEDIUM* m) noexcept override { return inner_ ? inner_->GetDataHere(f, m) : E_UNEXPECTED; }
    IFACEMETHODIMP QueryGetData(FORMATETC* f) noexcept override { return inner_ ? inner_->QueryGetData(f) : E_UNEXPECTED; }
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* a, FORMATETC* b) noexcept override { return inner_ ? inner_->GetCanonicalFormatEtc(a, b) : E_UNEXPECTED; }
    IFACEMETHODIMP SetData(FORMATETC* f, STGMEDIUM* m, BOOL r) noexcept override { return inner_ ? inner_->SetData(f, m, r) : E_UNEXPECTED; }
    IFACEMETHODIMP EnumFormatEtc(DWORD d, IEnumFORMATETC** e) noexcept override { return inner_ ? inner_->EnumFormatEtc(d, e) : E_UNEXPECTED; }
    IFACEMETHODIMP DAdvise(FORMATETC* f, DWORD a, IAdviseSink* s, DWORD* c) noexcept override { return inner_ ? inner_->DAdvise(f, a, s, c) : E_UNEXPECTED; }
    IFACEMETHODIMP DUnadvise(DWORD c) noexcept override { return inner_ ? inner_->DUnadvise(c) : E_UNEXPECTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA** e) noexcept override { return inner_ ? inner_->EnumDAdvise(e) : E_UNEXPECTED; }

private:
    ~AsyncDataObject() = default;
    wil::com_ptr<IDataObject> inner_;
    std::atomic<ULONG> ref_count_{1};
    std::atomic<BOOL> async_mode_{VARIANT_TRUE};
    std::atomic<BOOL> in_operation_{VARIANT_FALSE};
};

inline wil::com_ptr<AsyncDataObject> drag_media(HWND hwnd, std::span<const std::wstring> paths) {
    auto inner = media_data_object(paths);
    if (!inner) return nullptr;
    wil::com_ptr<AsyncDataObject> obj;
    obj.attach(new (std::nothrow) AsyncDataObject(std::move(inner)));
    if (!obj) return nullptr;
    DWORD effect = DROPEFFECT_NONE;
    (void)SHDoDragDrop(hwnd, static_cast<IDataObject*>(obj.get()), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    if (obj->is_operation_active()) return obj;
    return nullptr;
}

struct ClipboardPayload {
    std::vector<std::wstring> paths;
    std::vector<uint8_t> dib;
    std::vector<uint8_t> png;
};

inline void populate_clipboard_payload(ClipboardPayload& payload, const Raster* image, const std::wstring* png_file) {
    if (image && image->w && image->h) {
        const uint64_t total_pixels = static_cast<uint64_t>(image->w) * image->h;
        constexpr uint64_t MAX_DIB_PIXELS = 1ull << 28u;
        if (total_pixels <= MAX_DIB_PIXELS) {
            const size_t stride = static_cast<size_t>(image->w) * 4;
            const size_t byte_size = static_cast<size_t>(total_pixels) * 4;
            if (image->pixels.size() >= byte_size) {
                payload.dib.resize(sizeof(BITMAPINFOHEADER) + byte_size);
                BITMAPINFOHEADER bih{};
                bih.biSize = sizeof(BITMAPINFOHEADER);
                bih.biWidth = static_cast<LONG>(image->w);
                bih.biHeight = static_cast<LONG>(image->h); // bottom-up
                bih.biPlanes = 1;
                bih.biBitCount = 32;
                bih.biCompression = BI_RGB;
                bih.biSizeImage = static_cast<DWORD>(byte_size);
                std::memcpy(payload.dib.data(), &bih, sizeof(bih));
                for (UINT y = 0; y < image->h; ++y) {
                    std::memcpy(payload.dib.data() + sizeof(BITMAPINFOHEADER) + (stride * (image->h - 1 - y)),
                                image->pixels.data() + (stride * y), stride);
                }
            }
        }
    }
    if (png_file) {
        const bool is_png = png_file->size() >= 4 &&
                            _wcsicmp(png_file->c_str() + (png_file->size() - 4), L".png") == 0;
        if (is_png) {
            if (auto bytes = ulp::fs::read_as_bytes(*png_file); bytes && !bytes->empty() && bytes->size() < (128ull << 20u)) {
                payload.png.resize(bytes->size());
                std::memcpy(payload.png.data(), bytes->data(), bytes->size());
            }
        }
    }
}

inline wil::com_ptr<IDataObject> clipboard_data_object(const ClipboardPayload& payload) {
    auto obj = media_data_object(payload.paths);
    if (!obj) return nullptr;
    if (!payload.dib.empty()) set_data_bytes(obj.get(), CF_DIB, payload.dib.data(), payload.dib.size());
    if (!payload.png.empty()) set_data_bytes(obj.get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")), payload.png.data(), payload.png.size());
    return obj;
}

inline bool copy_capture_to_clipboard(const std::wstring& path, bool is_image, const Raster* raster = nullptr) {
    ClipboardPayload payload;
    payload.paths.push_back(path);
    std::optional<Raster> decoded;
    if (is_image) {
        if (!raster) {
            decoded = decode_image(path, 0, false);
            if (decoded) raster = &*decoded;
        }
        populate_clipboard_payload(payload, raster, &path);
    }
    auto obj = clipboard_data_object(payload);
    if (!obj) return false;
    return SUCCEEDED(OleSetClipboard(obj.get()));
}

} // namespace capusy::ui
