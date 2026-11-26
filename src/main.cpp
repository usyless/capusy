// capusy.exe — lightweight, Per-Monitor DPI-aware Win32 + Direct2D 1.3 screenshot & region recording app.
// Stays tiny in the system tray, registers global hotkeys directly for zero-latency freezeframes and
// unthrottled rapid-fire captures, and releases GPU/UI resources whenever all windows are hidden.

#include <capusy/capture.hpp>
#include <capusy/recorder.hpp>
#include <capusy/ui/history_window.hpp>
#include <capusy/ui/recording_overlay.hpp>
#include <capusy/ui/region_overlay.hpp>
#include <capusy/ui/settings_window.hpp>
#include <capusy/ui/toast_window.hpp>

#include "resource.h"

#include <shellapi.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using namespace capusy;
using namespace capusy::ui;

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT TRAY_ID = 1;
constexpr int HOTKEY_BASE_ID = 100;
constexpr UINT_PTR TIMER_IDLE_TRIM = 50;

enum TrayMenuId : UINT {
    IdTrayFull = 200,
    IdTrayRegion,
    IdTrayRecord,
    IdTrayHistory,
    IdTraySettings,
    IdTrayQuit,
};

struct SaveJob {
    Raster raster;
    CaptureMode mode = CaptureMode::Full;
    fs::path save_dir;
    bool auto_copy = true;
};

class CapusyApp {
public:
    bool init(HINSTANCE inst) {
        inst_ = inst;
        sh_.settings = load_settings();
        (void)sh_.history.open(history_db_path());

        if (!sh_.gfx.init()) {
            MessageBoxW(nullptr, L"Failed to initialize Direct3D / Direct2D 1.3.", APP_NAME, MB_ICONERROR | MB_OK);
            return false;
        }
        if (!sh_.fonts.init(sh_.gfx.dwrite())) {
            MessageBoxW(nullptr, L"Failed to initialize DirectWrite fonts.", APP_NAME, MB_ICONERROR | MB_OK);
            return false;
        }

        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSEXW wc{.cbSize = sizeof(wc)};
        wc.lpfnWndProc = &CapusyApp::tray_wndproc;
        wc.hInstance = inst;
        wc.lpszClassName = TRAY_CLASS;
        RegisterClassExW(&wc);
        tray_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!tray_hwnd_) return false;
        sh_.call_hwnd.store(tray_hwnd_, std::memory_order_release);

        history_win_ = std::make_unique<HistoryWindow>(sh_);
        settings_win_ = std::make_unique<SettingsWindow>(sh_);
        toast_win_ = std::make_unique<ToastWindow>(sh_);
        region_overlay_ = std::make_unique<RegionOverlay>(sh_);
        rec_overlay_ = std::make_unique<RecordingOverlay>(sh_);

        if (!history_win_->create(inst) || !settings_win_->create(inst) || !toast_win_->create(inst)) {
            return false;
        }

        wire_callbacks();
        start_saver_worker();
        add_tray();
        register_hotkeys(true);

        // Start lean: drop initial GPU device until first UI window or capture opens.
        schedule_idle_trim();
        return true;
    }

    int run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    void shutdown() {
        sh_.call_hwnd.store(nullptr, std::memory_order_release);
        unregister_hotkeys();
        remove_tray();
        if (region_overlay_) region_overlay_->cancel();
        if (rec_overlay_) rec_overlay_->hide();
        recorder_.stop_sync();
        stop_saver_worker();
        sh_.thumbs.stop();
        sh_.previews.stop();
        if (toast_win_) toast_win_->hide_immediate();
        if (history_win_) history_win_->hide();
        if (settings_win_) settings_win_->hide();
        sh_.history.close();
        if (tray_hwnd_) {
            MSG leftover;
            while (PeekMessageW(&leftover, tray_hwnd_, WM_APP_CALL, WM_APP_CALL, PM_REMOVE)) {
                if (leftover.lParam) delete reinterpret_cast<std::function<void()>*>(leftover.lParam);
            }
            DestroyWindow(tray_hwnd_);
            tray_hwnd_ = nullptr;
        }
    }

private:
    void wire_callbacks() {
        sh_.gfx.on_device_changed = [this] {
            sh_.thumbs.reset_gpu();
            sh_.previews.reset_gpu();
            if (history_win_) history_win_->reset_gpu();
            if (settings_win_) settings_win_->reset_gpu();
            if (toast_win_) toast_win_->reset_gpu();
        };

        sh_.on_capture_full = [this] { trigger_full_screenshot(); };
        sh_.on_capture_region = [this] { trigger_region_screenshot(); };
        sh_.on_toggle_record = [this] { trigger_toggle_recording(); };
        sh_.on_open_history = [this] { history_win_->show(); };
        sh_.on_open_settings = [this](int tab) { settings_win_->show(tab); };
        sh_.on_reload_hotkeys = [this] { register_hotkeys(false); };
        sh_.on_suspend_hotkeys = [this](bool suspend) {
            if (suspend) unregister_hotkeys();
            else register_hotkeys(false);
        };
        sh_.on_test_toast = [this] {
            if (auto cap = capture_virtual_screen(false)) {
                toast_win_->show_capture(L"", &cap->raster, cap->raster.w, cap->raster.h, false, false);
            }
        };
        sh_.on_visibility_changed = [this] { schedule_idle_trim(); };

        toast_win_->on_clicked = [this](const std::wstring& path) {
            if (!path.empty()) (void)open_file(path);
        };

        region_overlay_->on_screenshot_captured = [this](Raster&& raster) {
            enqueue_save(std::move(raster), CaptureMode::Region);
        };
        region_overlay_->on_record_region_selected = [this](RECT region) {
            start_region_recording(region);
        };
        region_overlay_->on_closed = [this] { schedule_idle_trim(); };

        rec_overlay_->on_stop_clicked = [this] { stop_region_recording(); };

        recorder_.on_finished = [this](RecordingResult&& res) {
            sh_.call([this, res = std::move(res)] mutable {
                on_recording_finished(std::move(res));
            });
        };
    }

    // ------------------------------------------------------------------------------------------
    // Instant Unthrottled Capture Pipeline
    // ------------------------------------------------------------------------------------------

    void trigger_full_screenshot() {
        // Immediate capture with zero rate limits! Every press snapshots right now and saves asynchronously.
        auto cap = capture_virtual_screen(sh_.settings.capture_cursor);
        if (!cap) return;
        enqueue_save(std::move(cap->raster), CaptureMode::Full);
    }

    void trigger_region_screenshot() {
        if (region_overlay_->active()) {
            region_overlay_->cancel();
            return;
        }
        (void)region_overlay_->start(inst_, OverlayMode::Screenshot, sh_.settings.capture_cursor);
    }

    void trigger_toggle_recording() {
        if (recorder_.recording()) {
            stop_region_recording();
            return;
        }
        if (region_overlay_->active()) {
            region_overlay_->cancel();
            return;
        }
        (void)region_overlay_->start(inst_, OverlayMode::RecordRegion, false);
    }

    void start_region_recording(RECT region) {
        if (recorder_.recording()) return;
        const fs::path out = make_capture_path(resolve_save_dir(sh_.settings), L".mp4");
        if (!recorder_.start(region, out, sh_.settings.record_fps, sh_.settings.record_bitrate_mbps, sh_.settings.record_cursor)) {
            show_balloon(L"Failed to start region recording.", NIIF_ERROR);
            return;
        }
        sh_.recording_active = true;
        (void)rec_overlay_->show(inst_, recorder_.region(), &recorder_);
        if (history_win_->visible()) history_win_->invalidate();
    }

    void stop_region_recording() {
        if (!recorder_.recording()) return;
        recorder_.request_stop();
        rec_overlay_->hide();
    }

    void on_recording_finished(RecordingResult&& res) {
        sh_.recording_active = false;
        rec_overlay_->hide();
        if (!res.ok) {
            if (!res.error.empty()) show_balloon(to_wide(res.error), NIIF_ERROR);
            schedule_idle_trim();
            return;
        }

        CaptureEntry entry;
        entry.path = to_utf8(res.path.wstring());
        entry.kind = CaptureKind::Video;
        entry.mode = CaptureMode::Recording;
        entry.created_at = unix_now();
        entry.width = res.width;
        entry.height = res.height;
        entry.size_bytes = res.size_bytes;
        entry.duration_ms = res.duration_ms;
        (void)sh_.history.insert(entry);

        bool copied = false;
        if (sh_.settings.auto_copy_clipboard) {
            copied = copy_capture_to_clipboard(res.path.wstring(), false, nullptr);
        }
        if (history_win_->visible()) history_win_->reload_entries(false);
        if (sh_.settings.show_corner_toast) {
            toast_win_->show_capture(res.path.wstring(), res.first_frame ? &*res.first_frame : nullptr,
                                     res.width, res.height, true, copied);
        }
        schedule_idle_trim();
    }

    void start_saver_worker() {
        saver_stop_ = false;
        saver_thread_ = std::jthread([this](const std::stop_token& st) {
            const ulp::win::COMWrapper<false> com{static_cast<DWORD>(COINIT_MULTITHREADED)};
            wil::com_ptr<IWICImagingFactory> wic;
            (void)CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
            while (!st.stop_requested()) {
                SaveJob job;
                {
                    std::unique_lock lk{saver_mx_};
                    saver_cv_.wait(lk, [&] { return saver_stop_ || st.stop_requested() || !saver_queue_.empty(); });
                    if (saver_stop_ || st.stop_requested()) return;
                    job = std::move(saver_queue_.front());
                    saver_queue_.pop_front();
                }

                const fs::path path = make_capture_path(job.save_dir, L".png");
                uint64_t bytes = 0;
                if (!save_raster_png(job.raster, path, &bytes, wic.get())) continue;

                CaptureEntry entry;
                entry.path = to_utf8(path.wstring());
                entry.kind = CaptureKind::Image;
                entry.mode = job.mode;
                entry.created_at = unix_now();
                entry.width = job.raster.w;
                entry.height = job.raster.h;
                entry.size_bytes = bytes;
                (void)sh_.history.insert(entry);

                // Prepare clipboard DIB + PNG bytes off the UI thread so clipboard update is instant
                ClipboardPayload clip;
                if (job.auto_copy) {
                    const std::wstring wpath = path.wstring();
                    clip.paths.push_back(wpath);
                    populate_clipboard_payload(clip, &job.raster, &wpath);
                }

                sh_.call([this, path = path.wstring(), raster = std::move(job.raster),
                          clip = std::move(clip), auto_copy = job.auto_copy] mutable {
                    bool copied = false;
                    if (auto_copy && !clip.paths.empty()) {
                        if (auto obj = clipboard_data_object(clip)) {
                            copied = SUCCEEDED(OleSetClipboard(obj.get()));
                        }
                    }
                    if (history_win_->visible()) history_win_->reload_entries(false);
                    if (sh_.settings.show_corner_toast) {
                        toast_win_->show_capture(path, &raster, raster.w, raster.h, false, copied);
                    }
                    schedule_idle_trim();
                });
            }
        });
    }

    void stop_saver_worker() noexcept {
        {
            const std::scoped_lock lk{saver_mx_};
            saver_stop_ = true;
        }
        saver_cv_.notify_all();
        if (saver_thread_.joinable()) saver_thread_.join();
    }

    void enqueue_save(Raster&& raster, CaptureMode mode) {
        SaveJob job{
            .raster = std::move(raster),
            .mode = mode,
            .save_dir = resolve_save_dir(sh_.settings),
            .auto_copy = sh_.settings.auto_copy_clipboard,
        };
        {
            const std::scoped_lock lk{saver_mx_};
            saver_queue_.push_back(std::move(job));
        }
        saver_cv_.notify_one();
    }

    // ------------------------------------------------------------------------------------------
    // Global Hotkeys with Internal + OS Collision Checking
    // ------------------------------------------------------------------------------------------

    void unregister_hotkeys() noexcept {
        if (!tray_hwnd_) return;
        for (size_t i = 0; i < HOTKEY_COUNT; ++i) {
            UnregisterHotKey(tray_hwnd_, HOTKEY_BASE_ID + static_cast<int>(i));
        }
    }

    void register_hotkeys(bool notify_conflicts) {
        unregister_hotkeys();
        const auto all = sh_.settings.all_hotkeys();
        std::wstring conflicts;

        for (size_t i = 0; i < HOTKEY_COUNT; ++i) {
            const HotkeyBinding& hb = all[i];
            auto& st = sh_.hotkey_status[i];
            st = {};
            if (!hb.enabled || hb.vk == 0) {
                st.state = HotkeyState::Disabled;
                continue;
            }
            if (const auto clash = find_internal_hotkey_conflict(all, i)) {
                st.state = HotkeyState::ConflictInternal;
                st.conflict_with = clash;
                continue;
            }
            if (RegisterHotKey(tray_hwnd_, HOTKEY_BASE_ID + static_cast<int>(i), normalize_hotkey_mods(hb.mods) | MOD_NOREPEAT, hb.vk)) {
                st.state = HotkeyState::Registered;
            } else {
                st.state = HotkeyState::Unavailable;
                if (!conflicts.empty()) conflicts += L", ";
                conflicts += hotkey_action_label(static_cast<HotkeyAction>(i));
            }
        }
        if (notify_conflicts && !conflicts.empty()) {
            show_balloon(ulp::str::concat_strings(L"Some hotkeys are already in use by another app: ", conflicts, L". Open Settings to change them."), NIIF_WARNING);
        }
        if (settings_win_ && settings_win_->visible()) settings_win_->invalidate();
    }

    // ------------------------------------------------------------------------------------------
    // Idle Memory & Resource Trimming
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] bool any_ui_active() const noexcept {
        return (history_win_ && history_win_->visible()) ||
               (settings_win_ && settings_win_->visible()) ||
               (toast_win_ && toast_win_->visible()) ||
               (region_overlay_ && region_overlay_->active()) ||
               (rec_overlay_ && rec_overlay_->visible()) ||
               recorder_.recording() ||
               sh_.background_jobs > 0;
    }

    void schedule_idle_trim() noexcept {
        if (!tray_hwnd_) return;
        KillTimer(tray_hwnd_, TIMER_IDLE_TRIM);
        if (!any_ui_active()) {
            // Release GPU device + caches after 5s of full idle so memory footprint drops to minimal.
            SetTimer(tray_hwnd_, TIMER_IDLE_TRIM, 5000, nullptr);
        }
    }

    void perform_idle_trim() noexcept {
        if (any_ui_active()) return;
        sh_.thumbs.clear();
        sh_.previews.clear();
        sh_.fonts.clear_caches();
        sh_.gfx.release_device();
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }

    // ------------------------------------------------------------------------------------------
    // System Tray
    // ------------------------------------------------------------------------------------------

    NOTIFYICONDATAW tray_data() const noexcept {
        NOTIFYICONDATAW nid{.cbSize = sizeof(nid)};
        nid.hWnd = tray_hwnd_;
        nid.uID = TRAY_ID;
        return nid;
    }

    void load_tray_icon() noexcept {
        UINT dpi = GetDpiForWindow(tray_hwnd_);
        if (dpi == 0) dpi = GetDpiForSystem();
        wil::unique_hicon next{static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                             GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                             GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR))};
        if (next) {
            tray_icon_ = std::move(next);
            tray_icon_dpi_ = dpi;
        }
    }

    void add_tray() {
        NOTIFYICONDATAW nid = tray_data();
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_TRAY;
        load_tray_icon();
        nid.hIcon = tray_icon_.get();
        wcscpy_s(nid.szTip, L"capusy — Screenshot & Recorder");
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }

    void remove_tray() noexcept {
        if (!tray_hwnd_) return;
        NOTIFYICONDATAW nid = tray_data();
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    void show_balloon(const std::wstring& body, DWORD flags) noexcept {
        if (!tray_hwnd_) return;
        NOTIFYICONDATAW nid = tray_data();
        nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, APP_NAME);
        wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
        nid.dwInfoFlags = flags;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    std::wstring menu_label(const wchar_t* title, HotkeyAction a) const {
        std::wstring s{title};
        const auto idx = static_cast<size_t>(a);
        if (sh_.hotkey_status[idx].state == HotkeyState::Registered) {
            const auto& hb = sh_.settings.hotkey(a);
            append_to(s, L"\t", format_hotkey(hb.mods, hb.vk, true));
        }
        return s;
    }

    void show_tray_menu() {
        const wil::unique_hmenu menu{CreatePopupMenu()};
        const std::wstring header = ulp::str::concat_strings(APP_NAME, L"  v", APP_VERSION);
        AppendMenuW(menu.get(), MF_STRING | MF_DISABLED, 0, header.c_str());
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, IdTrayFull, menu_label(L"Full-Screen Screenshot", HotkeyAction::FullScreenshot).c_str());
        AppendMenuW(menu.get(), MF_STRING, IdTrayRegion, menu_label(L"Region Screenshot", HotkeyAction::RegionScreenshot).c_str());
        AppendMenuW(menu.get(), MF_STRING, IdTrayRecord,
                    menu_label(recorder_.recording() ? L"Stop Region Recording" : L"Region Recording", HotkeyAction::RegionRecord).c_str());
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, IdTrayHistory, menu_label(L"Open History", HotkeyAction::OpenHistory).c_str());
        AppendMenuW(menu.get(), MF_STRING, IdTraySettings, L"Settings");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, IdTrayQuit, L"Quit capusy");
        SetMenuDefaultItem(menu.get(), IdTrayHistory, FALSE);

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(tray_hwnd_);
        const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, tray_hwnd_, nullptr));
        PostMessageW(tray_hwnd_, WM_NULL, 0, 0);

        switch (cmd) {
            case IdTrayFull: trigger_full_screenshot(); break;
            case IdTrayRegion: trigger_region_screenshot(); break;
            case IdTrayRecord: trigger_toggle_recording(); break;
            case IdTrayHistory: history_win_->show(); break;
            case IdTraySettings: settings_win_->show(0); break;
            case IdTrayQuit: PostQuitMessage(0); break;
            default: break;
        }
    }

    static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<CapusyApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->handle_tray(hwnd, msg, wp, lp);
    }

    LRESULT handle_tray(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == taskbar_created_ && taskbar_created_) {
            add_tray();
            return 0;
        }
        if (msg == msg_open_history()) {
            history_win_->show();
            return 0;
        }
        if (msg == msg_open_settings()) {
            settings_win_->show(0);
            return 0;
        }
        switch (msg) {
            case WM_HOTKEY: {
                const int idx = static_cast<int>(wp) - HOTKEY_BASE_ID;
                if (idx == static_cast<int>(HotkeyAction::FullScreenshot)) trigger_full_screenshot();
                else if (idx == static_cast<int>(HotkeyAction::RegionScreenshot)) trigger_region_screenshot();
                else if (idx == static_cast<int>(HotkeyAction::RegionRecord)) trigger_toggle_recording();
                else if (idx == static_cast<int>(HotkeyAction::OpenHistory)) history_win_->show();
                return 0;
            }
            case WM_APP_CALL:
                if (lp) {
                    const std::unique_ptr<std::function<void()>> fn{reinterpret_cast<std::function<void()>*>(lp)};
                    if (*fn) (*fn)();
                }
                return 0;
            case WM_TIMER:
                if (wp == TIMER_IDLE_TRIM) {
                    KillTimer(hwnd, TIMER_IDLE_TRIM);
                    perform_idle_trim();
                }
                return 0;
            case WM_TRAY:
                switch (LOWORD(lp)) {
                    case NIN_SELECT:
                    case NIN_KEYSELECT:
                    case WM_LBUTTONDBLCLK:
                        history_win_->show();
                        break;
                    case WM_CONTEXTMENU:
                    case WM_RBUTTONUP:
                        show_tray_menu();
                        break;
                    case NIN_BALLOONUSERCLICK:
                        settings_win_->show(0);
                        break;
                    default:
                        break;
                }
                return 0;
            case WM_DPICHANGED:
            case WM_DISPLAYCHANGE: {
                const UINT dpi = GetDpiForWindow(hwnd);
                if (dpi != 0 && dpi != tray_icon_dpi_) {
                    wil::unique_hicon old = std::move(tray_icon_);
                    load_tray_icon();
                    if (tray_icon_) {
                        NOTIFYICONDATAW nid = tray_data();
                        nid.uFlags = NIF_ICON;
                        nid.hIcon = tray_icon_.get();
                        Shell_NotifyIconW(NIM_MODIFY, &nid);
                    } else {
                        tray_icon_ = std::move(old);
                    }
                }
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HINSTANCE inst_ = nullptr;
    HWND tray_hwnd_ = nullptr;
    UINT taskbar_created_ = 0;
    wil::unique_hicon tray_icon_;
    UINT tray_icon_dpi_ = 0;

    Shared sh_;
    RegionRecorder recorder_;
    std::unique_ptr<HistoryWindow> history_win_;
    std::unique_ptr<SettingsWindow> settings_win_;
    std::unique_ptr<ToastWindow> toast_win_;
    std::unique_ptr<RegionOverlay> region_overlay_;
    std::unique_ptr<RecordingOverlay> rec_overlay_;

    std::mutex saver_mx_;
    std::condition_variable saver_cv_;
    std::deque<SaveJob> saver_queue_;
    bool saver_stop_ = false;
    std::jthread saver_thread_;
};

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    struct OleScope {
        HRESULT hr = OleInitialize(nullptr);
        OleScope() = default;
        OleScope(const OleScope&) = delete;
        OleScope& operator=(const OleScope&) = delete;
        OleScope(OleScope&&) = delete;
        OleScope& operator=(OleScope&&) = delete;
        ~OleScope() {
            if (SUCCEEDED(hr)) OleUninitialize();
        }
    };
    const OleScope ole;
    ulp::win::dark::init_dark_mode();

    NamedMutexLock single_instance;
    if (!single_instance.try_lock(INSTANCE_MUTEX, 0)) {
        if (HWND existing = FindWindowW(TRAY_CLASS, nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(existing, msg_open_history(), 0, 0);
        }
        return 0;
    }

    const auto app = std::make_unique<CapusyApp>();
    if (!app->init(hInstance)) return 1;
    const int code = app->run();
    app->shutdown();
    return code;
}
