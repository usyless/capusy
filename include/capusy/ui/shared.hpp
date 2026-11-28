#pragma once

#include "canvas.hpp"
#include "thumbs.hpp"

#include <capusy/history_store.hpp>
#include <capusy/hotkeys.hpp>
#include <capusy/settings.hpp>
#include <capusy/update_logic.hpp>

#include <usylibpp/ui/drag_drop.hpp>

#include <atomic>
#include <dwmapi.h>
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

    [[nodiscard]] bool has_update_available() const {
        return !update_state.latest_version.empty() && is_newer_version(update_state.latest_version, to_utf8(APP_VERSION));
    }

    std::function<void()> on_capture_full;
    std::function<void()> on_capture_region;
    std::function<void()> on_toggle_full_record;
    std::function<void()> on_toggle_record;
    std::function<void()> on_color_picker;
    std::function<void()> on_open_history;
    std::function<void()> on_open_folder;
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

    void save() const {
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

inline bool reveal_in_explorer(std::span<const std::wstring> paths) {
    if (paths.empty()) return false;
    if (paths.size() == 1) return reveal_in_explorer(paths[0]);

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& p : paths) {
        if (PIDLIST_ABSOLUTE id = ILCreateFromPathW(p.c_str())) {
            pidls.push_back(id);
        }
    }
    const auto free_pidls = wil::scope_exit([&] {
        for (auto* id : pidls) ILFree(id);
    });
    if (pidls.empty()) return false;
    if (pidls.size() == 1) {
        return SUCCEEDED(SHOpenFolderAndSelectItems(pidls[0], 0, nullptr, 0));
    }

    PIDLIST_ABSOLUTE parent = ILCloneFull(pidls.front());
    if (!parent) return false;
    const auto free_parent = wil::scope_exit([&] { ILFree(parent); });
    ILRemoveLastID(parent);

    std::vector<PCUITEMID_CHILD> children;
    children.reserve(pidls.size());
    for (const auto* id : pidls) {
        PIDLIST_ABSOLUTE pp = ILCloneFull(id);
        if (!pp) continue;
        ILRemoveLastID(pp);
        const bool same = ILIsEqual(pp, parent) != FALSE;
        ILFree(pp);
        if (same) children.push_back(ILFindLastID(id));
    }
    if (children.size() <= 1) {
        return SUCCEEDED(SHOpenFolderAndSelectItems(pidls.front(), 0, nullptr, 0));
    }
    return SUCCEEDED(SHOpenFolderAndSelectItems(parent, static_cast<UINT>(children.size()), children.data(), 0));
}

/// Moves one or more files to the Windows Recycle Bin in a single shell operation
/// (falls back to std::filesystem::remove per file if Recycle Bin fails).
inline bool delete_files_to_recycle_bin(std::span<const std::wstring> paths) {
    std::wstring double_nul;
    std::vector<std::wstring_view> existing;
    existing.reserve(paths.size());
    std::error_code ec;
    for (const auto& path : paths) {
        if (fs::exists(path, ec)) {
            existing.push_back(path);
            double_nul.append(path);
            double_nul.push_back(L'\0');
        }
    }
    if (existing.empty()) return true;
    double_nul.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = double_nul.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) return true;

    bool all_ok = true;
    for (const auto path : existing) {
        std::error_code rm_ec;
        if (fs::exists(path, rm_ec) && !fs::remove(path, rm_ec)) {
            all_ok = false;
        }
    }
    return all_ok;
}

/// Moves a file to the Windows Recycle Bin (falls back to std::filesystem::remove if Recycle Bin fails).
inline bool delete_file_to_recycle_bin(const std::wstring& path) {
    return delete_files_to_recycle_bin(std::span<const std::wstring>{&path, 1});
}

inline std::optional<std::wstring> browse_folder_dialog(HWND owner, const wchar_t* title, const std::wstring& initial) {
    wil::com_ptr<IFileDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        return std::nullopt;
    }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | static_cast<DWORD>(FOS_PICKFOLDERS) | static_cast<DWORD>(FOS_FORCEFILESYSTEM) | static_cast<DWORD>(FOS_PATHMUSTEXIST));
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

inline std::optional<std::wstring> browse_file_dialog(HWND owner, const wchar_t* title, const std::wstring& initial_dir,
                                                      std::span<const COMDLG_FILTERSPEC> filters = {}) {
    wil::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        return std::nullopt;
    }
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | static_cast<DWORD>(FOS_FORCEFILESYSTEM) | static_cast<DWORD>(FOS_FILEMUSTEXIST) | static_cast<DWORD>(FOS_PATHMUSTEXIST));
    }
    if (title) dlg->SetTitle(title);
    if (!filters.empty()) {
        (void)dlg->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
        (void)dlg->SetFileTypeIndex(1);
    }
    if (!initial_dir.empty()) {
        wil::com_ptr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial_dir.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
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
// OLE Drag & Drop + Clipboard (shared implementation in usylibpp::ui::drag_drop)
// ---------------------------------------------------------------------------------------------

using ulp::ui::AsyncDataObject;
using ulp::ui::media_data_object;
using ulp::ui::set_data_bytes;

inline wil::com_ptr<AsyncDataObject> drag_media(HWND hwnd, std::span<const std::wstring> paths) {
    return ulp::ui::drag_files(hwnd, paths);
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

inline bool copy_captures_to_clipboard(std::span<const std::wstring> paths, bool single_is_image, const Raster* raster = nullptr) {
    if (paths.empty()) return false;
    ClipboardPayload payload;
    payload.paths.assign(paths.begin(), paths.end());
    std::optional<Raster> decoded;
    if (paths.size() == 1 && single_is_image) {
        if (!raster) {
            decoded = decode_image(paths[0], 0, false);
            if (decoded) raster = &*decoded;
        }
        populate_clipboard_payload(payload, raster, paths.data());
    }
    const auto obj = clipboard_data_object(payload);
    if (!obj) return false;
    return SUCCEEDED(OleSetClipboard(obj.get()));
}

inline bool copy_capture_to_clipboard(const std::wstring& path, bool is_image, const Raster* raster = nullptr) {
    return copy_captures_to_clipboard(std::span<const std::wstring>{&path, 1}, is_image, raster);
}

} // namespace capusy::ui
