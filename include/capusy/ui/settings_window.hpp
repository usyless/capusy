#pragma once

#include "canvas.hpp"
#include "shared.hpp"
#include "widgets.hpp"
#include "window_lifecycle.hpp"

#include <capusy/audio_capture.hpp>
#include <capusy/recorder.hpp>
#include <capusy/sharex_import.hpp>
#include <capusy/updater.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace capusy::ui {

/// Settings window modeled on usyseek::ui::SettingsWindow (Midnight Dark theme, Flow layout, AnimFloat switches/tabs).
class SettingsWindow {
public:
    explicit SettingsWindow(Shared& sh) : sh_{sh} {}
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    SettingsWindow(SettingsWindow&&) = delete;
    SettingsWindow& operator=(SettingsWindow&&) = delete;
    ~SettingsWindow() {
        stop_background_jobs();
        stop_recording_hotkey();
        app_icon_.reset();
        target_.release();
        if (hwnd_) DestroyWindow(hwnd_);
    }

    void stop_background_jobs() noexcept {
        update_job_.thread.request_stop();
        update_check_thread_.request_stop();
        sharex_job_.thread.request_stop();
        update_job_.thread = {};
        update_check_thread_ = {};
        sharex_job_.thread = {};
    }

    void maybe_auto_check_updates() {
        start_update_check(false);
    }

    bool create(HINSTANCE inst) {
        inst_ = inst;
        WNDCLASSEXW wc{.cbSize = sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &SettingsWindow::wndproc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = SETTINGS_CLASS;
        RegisterClassExW(&wc);

        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        const UINT sys_dpi = GetDpiForSystem();
        const float s = (sys_dpi > 0 ? static_cast<float>(sys_dpi) : 96.0f) / 96.0f;
        RECT rc{.left = 0, .top = 0, .right = static_cast<LONG>(840 * s), .bottom = static_cast<LONG>(680 * s)};
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, sys_dpi);

        hwnd_ = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP, SETTINGS_CLASS, L"capusy — Settings",
            style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, inst, this);
        if (!hwnd_) return false;

        icons_.apply(hwnd_, inst, 101);
        cloak_window(hwnd_, true);
        cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
        tab_anim_.snap(0.0f);
        sync_state(false);
        return true;
    }

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] bool visible() const noexcept { return hwnd_ && IsWindowVisible(hwnd_) != FALSE; }

    void show(int tab = -1) {
        if (!hwnd_) return;
        if (tab >= 0) {
            tab_ = std::clamp(tab, 0, 2);
            tab_anim_.snap(static_cast<float>(tab_));
            scroll_ = target_scroll_ = 0.0f;
        }
        if (!sh_.gfx.ensure_device(hwnd_)) return;
        if (!target_.ready() && !target_.attach(sh_.gfx, hwnd_)) {
            if (sh_.on_visibility_changed) sh_.on_visibility_changed();
            return;
        }

        sync_state(false);
        apply_dark_window(hwnd_, DWMSBT_MAINWINDOW, true);
        cloak_window(hwnd_, false);
        minimized_ = false;
        if (IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_RESTORE);
        } else {
            ShowWindow(hwnd_, SW_SHOWNORMAL);
        }
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        window_active_ = true;
        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
        last_tooltip_dirty_.reset();
        full_dirty_ = true;
        pending_dirty_.clear();
        anim_active_ = false;
        render();
        ValidateRect(hwnd_, nullptr);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void hide() {
        if (!hwnd_) return;
        stop_recording_hotkey();
        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
        last_tooltip_dirty_.reset();
        raw_hover_id_ = IdNone;
        raw_hover_index_ = 0;
        dragging_scroll_ = false;
        in_sizemove_ = false;
        minimized_ = false;
        dragging_slider_ = IdNone;
        open_audio_drop_ = AudioDropdown::None;
        anim_active_ = false;
        full_dirty_ = true;
        hover_.set(IdNone, 0, false);
        desktop_devices_.clear();
        desktop_devices_.shrink_to_fit();
        mic_devices_.clear();
        mic_devices_.shrink_to_fit();
        hits_.clear();
        hits_.shrink_to_fit();
        scratch_hits_.clear();
        scratch_hits_.shrink_to_fit();
        pending_dirty_.clear();
        pending_dirty_.shrink_to_fit();
        dirty_dips_scratch_.clear();
        dirty_dips_scratch_.shrink_to_fit();
        app_icon_.reset();
        target_.release();
        cloak_window(hwnd_, true);
        ShowWindow(hwnd_, SW_HIDE);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void reset_gpu() noexcept {
        app_icon_.reset();
        target_.release();
        full_dirty_ = true;
        pending_dirty_.clear();
    }

    void invalidate() noexcept {
        full_dirty_ = true;
        pending_dirty_.clear();
        if (window_can_render_live(visible(), minimized_)) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void invalidate_rect(D2D1_RECT_F r) {
        if (r.right <= r.left || r.bottom <= r.top) {
            invalidate();
            return;
        }
        if (!full_dirty_) pending_dirty_.push_back(r);
        if (window_can_render_live(visible(), minimized_)) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void sync_and_invalidate() {
        sync_state(false);
        invalidate();
    }

    void refresh_external_state(bool reload_if_changed = true) {
        const bool cur_startup = is_startup_enabled();
        const bool cur_prtscn = is_windows_printscreen_snipping_disabled();
        const bool prtscn_changed = (cur_prtscn != win_prtscn_disabled_);
        if (cur_startup == startup_enabled_ && !prtscn_changed) return;
        startup_enabled_ = cur_startup;
        win_prtscn_disabled_ = cur_prtscn;
        win_prtscn_err_.clear();
        sw_startup_.set(startup_enabled_ ? 1.0f : 0.0f, false);
        sw_win_prtscn_.set(win_prtscn_disabled_ ? 1.0f : 0.0f, false);
        if (prtscn_changed && reload_if_changed && sh_.on_reload_hotkeys) {
            sh_.on_reload_hotkeys();
        }
        invalidate();
    }

private:
    static constexpr float TABS_H = 52.0f;
    static constexpr float PAD = 24.0f;
    static constexpr float CARD_GAP = 14.0f;
    static constexpr UINT WM_APP_RECORDED_KEY = WM_APP + 30;
    static constexpr UINT_PTR TIMER_TOOLTIP = 1;

    enum HitId : uint8_t {
        IdNone = 0,
        IdTab0,
        IdTab1,
        IdTab2,
        IdToggleHotkey,
        IdRecordHotkey,
        IdResetHotkey,
        IdToggleWinPrtSc,
        IdOpenWinKeyboardSettings,
        IdToggleStartup,
        IdToggleAutoCopy,
        IdToggleCaptureCursor,
        IdToggleInstantRegion,
        IdToggleSplitMonth,
        IdFmtPng,
        IdFmtJpeg,
        IdFmtBmp,
        IdFmtTiff,
        IdSliderImageQuality,
        IdBrowseSaveDir,
        IdOpenSaveDir,
        IdSaveDirWell,
        IdShareXPathWell,
        IdBrowseShareX,
        IdImportShareX,
        IdToggleShareXExistingOnly,
        IdToggleToast,
        IdSliderToastSize,
        IdSliderToastTimeout,
        IdTestToast,
        IdRecFmtMp4,
        IdRecFmtWmv,
        IdRecFmtGif,
        IdRecCodecH264,
        IdRecCodecHevc,
        IdRecProfBase,
        IdRecProfMain,
        IdRecProfHigh,
        IdRecGifColors,
        IdToggleRecGifDither,
        IdRecFpsPreset,
        IdSliderRecFps,
        IdRecScalePreset,
        IdSliderRecScale,
        IdRecRcVbr,
        IdRecRcQuality,
        IdRecRcCbr,
        IdRecBitratePreset,
        IdSliderRecBitrate,
        IdSliderRecQuality,
        IdRecKeyframe,
        IdToggleRecHwAccel,
        IdToggleRecFragMp4,
        IdToggleRecordCursor,
        IdToggleRecDesktop,
        IdDropRecDesktop,
        IdToggleRecMic,
        IdDropRecMic,
        IdRecAudioDropItem,
        IdRecAudioBitrate,
        IdRecAudioRate44,
        IdRecAudioRate48,
        IdRecAudioMono,
        IdRecAudioStereo,
        IdToggleAutoUpdate,
        IdCheckUpdates,
        IdInstallUpdate,
        IdInstallUpdateDisabled,
        IdOpenReleaseUrl,
        IdScrollbar,
    };

    enum class AudioDropdown : uint8_t {
        None = 0,
        Desktop,
        Mic,
    };

    struct UpdateJob {
        std::mutex mx;
        std::wstring log;
        bool failed = false;
        uint64_t done = 0;
        uint64_t total = 0;
        bool busy = false;
        std::jthread thread;
    };

    struct JobView {
        std::wstring log;
        bool failed = false;
        uint64_t done = 0;
        uint64_t total = 0;
    };

    [[nodiscard]] JobView view_update_job() {
        const std::scoped_lock lk{update_job_.mx};
        return {.log = update_job_.log, .failed = update_job_.failed, .done = update_job_.done, .total = update_job_.total};
    }

    [[nodiscard]] JobView view_sharex_job() {
        const std::scoped_lock lk{sharex_job_.mx};
        return {.log = sharex_job_.log, .failed = sharex_job_.failed, .done = sharex_job_.done, .total = sharex_job_.total};
    }

    void report_sharex(std::wstring_view msg, bool failed = false) {
        {
            const std::scoped_lock lk{sharex_job_.mx};
            sharex_job_.log.assign(msg);
            sharex_job_.failed = failed;
        }
        sh_.call([this] { invalidate(); });
    }

    void report_sharex_progress(uint64_t done, uint64_t total, std::wstring_view msg) {
        {
            const std::scoped_lock lk{sharex_job_.mx};
            sharex_job_.done = done;
            sharex_job_.total = total;
            sharex_job_.log.assign(msg);
            sharex_job_.failed = false;
        }
        sh_.call([this] { invalidate(); });
    }

    [[nodiscard]] static std::wstring detect_default_sharex_history_path() {
        const std::wstring docs = known_folder(FOLDERID_Documents);
        if (docs.empty()) return {};
        const fs::path base = fs::path{docs} / L"ShareX";
        std::error_code ec;
        const fs::path db = base / L"History.db";
        if (fs::is_regular_file(db, ec) && !ec) return db.wstring();
        const fs::path legacy_json = base / L"History.json";
        if (fs::is_regular_file(legacy_json, ec) && !ec) return legacy_json.wstring();
        return {};
    }

    void report_update(std::wstring_view msg, bool failed = false) {
        {
            const std::scoped_lock lk{update_job_.mx};
            update_job_.log.assign(msg);
            update_job_.failed = failed;
        }
        sh_.call([this] { invalidate(); });
    }

    void report_update_progress(uint64_t done, uint64_t total) {
        {
            const std::scoped_lock lk{update_job_.mx};
            update_job_.done = done;
            update_job_.total = total;
        }
        sh_.call([this] { invalidate(); });
    }

    void sync_state(bool animate) {
        startup_enabled_ = is_startup_enabled();
        win_prtscn_disabled_ = is_windows_printscreen_snipping_disabled();
        save_dir_w_ = resolve_save_dir(sh_.settings).wstring();
        if (auto st = sh_.history.stats()) stats_ = *st;
        for (size_t i = 0; i < HOTKEY_COUNT; ++i) {
            sw_hotkeys_[i].set(sh_.settings.hotkey(static_cast<HotkeyAction>(i)).enabled ? 1.0f : 0.0f, animate);
        }
        sw_win_prtscn_.set(win_prtscn_disabled_ ? 1.0f : 0.0f, animate);
        sw_startup_.set(startup_enabled_ ? 1.0f : 0.0f, animate);
        sw_auto_copy_.set(sh_.settings.auto_copy_clipboard ? 1.0f : 0.0f, animate);
        sw_cap_cursor_.set(sh_.settings.capture_cursor ? 1.0f : 0.0f, animate);
        sw_instant_region_.set(sh_.settings.region_instant_capture ? 1.0f : 0.0f, animate);
        sw_split_month_.set(sh_.settings.split_by_month ? 1.0f : 0.0f, animate);
        sw_sharex_existing_only_.set(sharex_only_existing_ ? 1.0f : 0.0f, animate);
        if (!sharex_path_probed_) {
            sharex_path_w_ = detect_default_sharex_history_path();
            sharex_path_probed_ = true;
        }
        sw_toast_.set(sh_.settings.show_corner_toast ? 1.0f : 0.0f, animate);
        sw_rec_cursor_.set(sh_.settings.record_cursor ? 1.0f : 0.0f, animate);
        sw_rec_hw_accel_.set(sh_.settings.record_hw_accel ? 1.0f : 0.0f, animate);
        sw_rec_frag_mp4_.set(sh_.settings.record_fragmented_mp4 ? 1.0f : 0.0f, animate);
        sw_rec_gif_dither_.set(sh_.settings.record_gif_dither ? 1.0f : 0.0f, animate);
        sw_rec_desktop_.set(sh_.settings.record_desktop_audio ? 1.0f : 0.0f, animate);
        sw_rec_mic_.set(sh_.settings.record_mic_audio ? 1.0f : 0.0f, animate);
        sw_auto_update_.set(sh_.settings.auto_check_updates ? 1.0f : 0.0f, animate);
        if (!animate) {
            desktop_devices_ = enumerate_audio_devices(AudioFlow::Render);
            mic_devices_ = enumerate_audio_devices(AudioFlow::Capture);
            if (!hevc_probed_) {
                hevc_available_ = is_hevc_encoder_available(true);
                hevc_probed_ = true;
            }
        }
    }

    void start_anim() noexcept {
        if (!window_can_render_live(visible(), minimized_)) return;
        if (!anim_active_) last_anim_ = std::chrono::steady_clock::now();
        anim_active_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void queue_hit_dirty(std::vector<D2D1_RECT_F>& out, int id, int idx, bool& full_dirty) const {
        if (id == IdNone) return;
        if (id == IdScrollbar) {
            if (view_rect_.right > view_rect_.left && view_rect_.bottom > view_rect_.top) {
                out.push_back(D2D1::RectF(view_rect_.right - 14.0f, view_rect_.top, view_rect_.right, view_rect_.bottom));
            } else {
                full_dirty = true;
            }
            return;
        }
        bool found = false;
        const bool is_slider = (id == IdSliderImageQuality || id == IdSliderToastSize || id == IdSliderToastTimeout ||
                                id == IdSliderRecFps || id == IdSliderRecScale || id == IdSliderRecBitrate ||
                                id == IdSliderRecQuality);
        const float pad = is_slider ? 10.0f : 3.0f;
        for (const Hit& h : hits_) {
            if (h.id == id && h.index == idx) {
                out.push_back(D2D1::RectF(h.r.left - pad, h.r.top - pad, h.r.right + pad, h.r.bottom + pad));
                found = true;
            }
        }
        if (!found) full_dirty = true;
    }

    void queue_animating_switches(std::vector<D2D1_RECT_F>& out, bool& full_dirty) const {
        const auto check = [&](const AnimFloat& sw, int id, int idx = 0) {
            if (sw.value != sw.target) queue_hit_dirty(out, id, idx, full_dirty);
        };
        for (size_t i = 0; i < HOTKEY_COUNT; ++i) {
            check(sw_hotkeys_[i], IdToggleHotkey, static_cast<int>(i));
        }
        check(sw_win_prtscn_, IdToggleWinPrtSc);
        check(sw_startup_, IdToggleStartup);
        check(sw_auto_copy_, IdToggleAutoCopy);
        check(sw_cap_cursor_, IdToggleCaptureCursor);
        check(sw_instant_region_, IdToggleInstantRegion);
        check(sw_split_month_, IdToggleSplitMonth);
        check(sw_sharex_existing_only_, IdToggleShareXExistingOnly);
        check(sw_toast_, IdToggleToast);
        check(sw_rec_cursor_, IdToggleRecordCursor);
        check(sw_rec_hw_accel_, IdToggleRecHwAccel);
        check(sw_rec_frag_mp4_, IdToggleRecFragMp4);
        check(sw_rec_gif_dither_, IdToggleRecGifDither);
        check(sw_rec_desktop_, IdToggleRecDesktop);
        check(sw_rec_mic_, IdToggleRecMic);
        check(sw_auto_update_, IdToggleAutoUpdate);
    }

    bool step_anim() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_anim_).count(), 0.001f, 0.05f);
        last_anim_ = now;

        bool moving = hover_.step(dt);
        if (tooltip_.step(dt)) moving = true;
        if (tab_anim_.step(dt, 20.0f, 0.004f)) moving = true;
        for (auto& sw : sw_hotkeys_) { if (sw.step(dt, 22.0f, 0.004f)) moving = true; }
        if (sw_win_prtscn_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_startup_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_auto_copy_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_cap_cursor_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_instant_region_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_split_month_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_sharex_existing_only_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_toast_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_rec_cursor_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_rec_hw_accel_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_rec_frag_mp4_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_rec_gif_dither_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_rec_desktop_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_rec_mic_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_auto_update_.step(dt, 22.0f, 0.004f)) moving = true;
        const float d = target_scroll_ - scroll_;
        if (std::abs(d) < 0.5f) {
            scroll_ = target_scroll_;
        } else {
            scroll_ += d * (1.0f - std::exp(-dt * 22.0f));
            if (std::abs(target_scroll_ - scroll_) < 0.5f) {
                scroll_ = target_scroll_;
            } else {
                moving = true;
            }
        }
        return moving;
    }

    static LRESULT CALLBACK ll_record_keyboard_proc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION && record_hook_target_ != nullptr &&
            record_hook_target_->recording_action_.has_value() &&
            GetForegroundWindow() == record_hook_target_->hwnd_) {
            const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
            if (kb->vkCode == VK_SNAPSHOT) {
                const auto read_mods = [&] noexcept -> uint32_t {
                    uint32_t mods = 0;
                    if (GetAsyncKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
                    if ((kb->flags & LLKHF_ALTDOWN) != 0 || GetAsyncKeyState(VK_MENU) < 0) mods |= MOD_ALT;
                    if (GetAsyncKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
                    if (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
                    return mods;
                };
                if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
                    if (!record_hook_target_->pending_snapshot_down_) {
                        record_hook_target_->pending_snapshot_mods_ = read_mods();
                        record_hook_target_->pending_snapshot_down_ = true;
                    }
                    return 1;
                }
                if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
                    const uint32_t mods = record_hook_target_->pending_snapshot_down_
                        ? record_hook_target_->pending_snapshot_mods_
                        : read_mods();
                    record_hook_target_->pending_snapshot_down_ = false;
                    PostMessageW(record_hook_target_->hwnd_, WM_APP_RECORDED_KEY, VK_SNAPSHOT, static_cast<LPARAM>(mods));
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    void install_record_hook() noexcept {
        if (record_hook_) return;
        record_hook_target_ = this;
        record_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &SettingsWindow::ll_record_keyboard_proc, inst_, 0);
        if (!record_hook_) record_hook_target_ = nullptr;
    }

    void uninstall_record_hook() noexcept {
        if (record_hook_) {
            UnhookWindowsHookEx(record_hook_);
            record_hook_ = nullptr;
        }
        if (record_hook_target_ == this) record_hook_target_ = nullptr;
        pending_snapshot_down_ = false;
        pending_snapshot_mods_ = 0;
    }

    void start_recording_hotkey(HotkeyAction a) {
        recording_action_ = a;
        record_conflict_msg_.clear();
        pending_snapshot_down_ = false;
        pending_snapshot_mods_ = 0;
        if (sh_.on_suspend_hotkeys) sh_.on_suspend_hotkeys(true);
        install_record_hook();
    }

    void stop_recording_hotkey(bool resume_hotkeys = true) noexcept {
        uninstall_record_hook();
        if (!recording_action_) return;
        recording_action_.reset();
        record_conflict_msg_.clear();
        if (resume_hotkeys && sh_.on_suspend_hotkeys) sh_.on_suspend_hotkeys(false);
    }

    [[nodiscard]] float max_scroll() const noexcept {
        const float view_h = std::max(0.0f, target_.size().height - TABS_H - 1.0f);
        return std::max(0.0f, content_h_ - view_h);
    }

    void render() {
        if (!window_can_render_live(visible(), minimized_) || rendering_) return;
        if (!target_.ready()) {
            if (!sh_.gfx.ensure_device(hwnd_) || !target_.rebuild()) return;
            full_dirty_ = true;
        }
        rendering_ = true;
        const auto guard = wil::scope_exit([&] { rendering_ = false; });

        const float prev_scroll = scroll_;
        if (dragging_scroll_ || std::abs(target_scroll_ - scroll_) >= 0.5f) {
            full_dirty_ = true;
        }

        dirty_dips_scratch_.clear();
        if (!full_dirty_) {
            dirty_dips_scratch_.swap(pending_dirty_);
            if (hover_.in_anim.value != hover_.in_anim.target) {
                queue_hit_dirty(dirty_dips_scratch_, hover_.id, hover_.index, full_dirty_);
            }
            if (hover_.prev_id != IdNone && hover_.out_anim.value != hover_.out_anim.target) {
                queue_hit_dirty(dirty_dips_scratch_, hover_.prev_id, hover_.prev_index, full_dirty_);
            }
            if (tab_anim_.value != tab_anim_.target) {
                dirty_dips_scratch_.push_back(D2D1::RectF(0.0f, 0.0f, target_.size().width, TABS_H + 2.0f));
            }
            queue_animating_switches(dirty_dips_scratch_, full_dirty_);
        } else {
            pending_dirty_.clear();
        }

        const bool anim = step_anim();
        if (scroll_ != prev_scroll) {
            full_dirty_ = true;
        }

        std::optional<D2D1_RECT_F> curr_tip_dirty;
        if (open_audio_drop_ == AudioDropdown::None && tooltip_.visible()) {
            const TooltipLayout tl = measure_tooltip_layout(sh_.fonts, target_.size(), tooltip_.anchor, tooltip_.title, tooltip_.subtitle);
            if (tl.valid) curr_tip_dirty = tl.dirty;
        }
        if (!full_dirty_) {
            if (curr_tip_dirty) dirty_dips_scratch_.push_back(*curr_tip_dirty);
            if (last_tooltip_dirty_ && (!curr_tip_dirty || !rect_equal(*last_tooltip_dirty_, *curr_tip_dirty))) {
                dirty_dips_scratch_.push_back(*last_tooltip_dirty_);
            }
        }

        const bool request_full = std::exchange(full_dirty_, false);
        const auto mode = in_sizemove_ ? WindowTarget::PresentMode::Immediate : WindowTarget::PresentMode::Vsync;
        const bool ok = target_.render_frame(request_full, dirty_dips_scratch_, mode, [&](std::optional<D2D1_RECT_F> clip, bool full_frame) {
            if (full_frame) {
                hits_.clear();
                Canvas cv{target_, sh_.fonts, hits_, default_theme(), clip};
                draw(cv);
            } else {
                scratch_hits_.clear();
                Canvas cv{target_, sh_.fonts, scratch_hits_, default_theme(), clip};
                draw(cv);
            }
        });
        last_tooltip_dirty_ = curr_tip_dirty;
        anim_active_ = window_can_render_live(visible(), minimized_) && anim;
        if (!ok) {
            target_.release();
            sh_.gfx.recover(hwnd_);
            invalidate();
        }
    }

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), with_alpha(cv.theme().bg, cv.theme().window_bg_alpha));

        draw_tabs(cv, sz);

        const D2D1_RECT_F view = D2D1::RectF(0.0f, TABS_H + 1.0f, sz.width, sz.height);
        view_rect_ = view;
        cv.push_clip(view);

        const float cw = std::min(760.0f, sz.width - (PAD * 2.0f));
        const float cx = std::round((sz.width - cw) * 0.5f);
        float y = view.top + PAD - scroll_;

        if (tab_ == 0) {
            y = draw_tab_general(cv, cx, y, cw);
        } else if (tab_ == 1) {
            y = draw_tab_toast_recording(cv, cx, y, cw);
        } else {
            y = draw_tab_about(cv, cx, y, cw);
        }

        content_h_ = (y + scroll_ - view.top) + PAD;
        cv.pop_clip();

        const float ms = max_scroll();
        if (ms > 0.0f) {
            const float track_h = height(view) - 8.0f;
            const float thumb_h = std::max(28.0f, track_h * height(view) / std::max(1.0f, content_h_));
            const float ty = view.top + 4.0f + ((track_h - thumb_h) * (scroll_ / ms));
            const float ha = dragging_scroll_ ? 1.0f : hover_.alpha(IdScrollbar);
            scroll_thumb_ = D2D1::RectF(view.right - lerp(6.0f, 8.0f, ha), ty, view.right - 2.0f, ty + thumb_h);
            cv.fill_round(scroll_thumb_, 3.0f, with_alpha(cv.theme().on_surface_variant, lerp(0.28f, 0.55f, ha)));
            cv.hit(D2D1::RectF(view.right - 12.0f, view.top, view.right, view.bottom), IdScrollbar);
        }

        if (tab_ == 1 && open_audio_drop_ != AudioDropdown::None) {
            const bool is_desk = (open_audio_drop_ == AudioDropdown::Desktop);
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
            draw_dropdown_popup(cv, is_desk ? desktop_drop_r_ : mic_drop_r_, sz, opts, IdRecAudioDropItem, hover_.id, hover_.index);
        } else {
            draw_tooltip(cv, sz, tooltip_.anchor, tooltip_.title, tooltip_.subtitle, tooltip_.alpha.value);
        }
    }

    [[nodiscard]] bool has_tooltip(int id, int idx) const noexcept {
        if (open_audio_drop_ != AudioDropdown::None) return false;
        switch (id) {
            case IdTab0:
            case IdTab1:
            case IdTab2:
            case IdToggleWinPrtSc:
            case IdOpenWinKeyboardSettings:
            case IdToggleStartup:
            case IdToggleAutoCopy:
            case IdToggleCaptureCursor:
            case IdToggleInstantRegion:
            case IdToggleSplitMonth:
            case IdFmtPng:
            case IdFmtJpeg:
            case IdFmtBmp:
            case IdFmtTiff:
            case IdSliderImageQuality:
            case IdBrowseSaveDir:
            case IdOpenSaveDir:
            case IdSaveDirWell:
            case IdShareXPathWell:
            case IdBrowseShareX:
            case IdImportShareX:
            case IdToggleShareXExistingOnly:
            case IdToggleToast:
            case IdSliderToastSize:
            case IdSliderToastTimeout:
            case IdTestToast:
            case IdRecFmtMp4:
            case IdRecFmtWmv:
            case IdRecFmtGif:
            case IdRecCodecH264:
            case IdRecCodecHevc:
            case IdRecProfBase:
            case IdRecProfMain:
            case IdRecProfHigh:
            case IdRecGifColors:
            case IdToggleRecGifDither:
            case IdRecFpsPreset:
            case IdSliderRecFps:
            case IdRecScalePreset:
            case IdSliderRecScale:
            case IdRecRcVbr:
            case IdRecRcQuality:
            case IdRecRcCbr:
            case IdRecBitratePreset:
            case IdSliderRecBitrate:
            case IdSliderRecQuality:
            case IdRecKeyframe:
            case IdToggleRecHwAccel:
            case IdToggleRecFragMp4:
            case IdToggleRecordCursor:
            case IdToggleRecDesktop:
            case IdDropRecDesktop:
            case IdToggleRecMic:
            case IdDropRecMic:
            case IdRecAudioBitrate:
            case IdRecAudioRate44:
            case IdRecAudioRate48:
            case IdRecAudioMono:
            case IdRecAudioStereo:
            case IdToggleAutoUpdate:
            case IdCheckUpdates:
            case IdInstallUpdate:
            case IdInstallUpdateDisabled:
            case IdOpenReleaseUrl:
                return true;
            case IdToggleHotkey:
            case IdRecordHotkey:
            case IdResetHotkey:
                return idx >= 0 && std::cmp_less(idx, HOTKEY_COUNT);
            default:
                return false;
        }
    }

    bool build_tooltip(int id, int idx, std::wstring& title, std::wstring& subtitle) const {
        title.clear();
        subtitle.clear();
        if (open_audio_drop_ != AudioDropdown::None) return false;
        switch (id) {
            case IdTab0: title = L"Global hotkeys, startup, save folder, and image format"; return true;
            case IdTab1: title = L"Corner preview toast and screen recording"; return true;
            case IdTab2: title = L"App info, history database stats, and software updates"; return true;
            case IdToggleHotkey:
                if (idx >= 0 && std::cmp_less(idx, HOTKEY_COUNT)) {
                    const auto a = static_cast<HotkeyAction>(idx);
                    title = sh_.settings.hotkey(a).enabled ? L"Disable this hotkey" : L"Enable this hotkey";
                    return true;
                }
                return false;
            case IdRecordHotkey:
                if (idx >= 0 && std::cmp_less(idx, HOTKEY_COUNT)) {
                    const auto a = static_cast<HotkeyAction>(idx);
                    title = (recording_action_ && *recording_action_ == a)
                        ? L"Cancel shortcut recording (Esc)"
                        : L"Click then press a key combination";
                    return true;
                }
                return false;
            case IdResetHotkey:
                if (idx >= 0 && std::cmp_less(idx, HOTKEY_COUNT)) {
                    const HotkeyBinding def = default_hotkey(static_cast<HotkeyAction>(idx));
                    title = L"Reset to default shortcut";
                    const auto txt = format_hotkey(def.mods, def.vk);
                    subtitle.assign(txt.view());
                    return true;
                }
                return false;
            case IdToggleWinPrtSc: title = L"Toggle Windows Snipping Tool PrintScreen override"; return true;
            case IdOpenWinKeyboardSettings: title = L"Open Windows Settings → Accessibility → Keyboard"; return true;
            case IdToggleStartup: title = L"Launch capusy automatically in the system tray at sign-in"; return true;
            case IdToggleAutoCopy: title = L"Automatically copy new captures to the clipboard"; return true;
            case IdToggleCaptureCursor: title = L"Include the mouse pointer in screenshots"; return true;
            case IdToggleInstantRegion: title = L"Save region on release without opening annotation tools"; return true;
            case IdSaveDirWell:
                title = L"Capture save folder";
                subtitle = save_dir_w_;
                return !subtitle.empty();
            case IdBrowseSaveDir: title = L"Choose a folder for saved screenshots and recordings"; return true;
            case IdOpenSaveDir: title = L"Open save folder in Explorer"; return true;
            case IdShareXPathWell:
                title = L"Selected ShareX history file";
                subtitle = sharex_path_w_.empty() ? L"No file selected — click Browse to pick History.db or History.json" : sharex_path_w_;
                return true;
            case IdBrowseShareX:
                title = L"Browse for a ShareX History.db or legacy History.json file";
                return true;
            case IdImportShareX:
                title = sharex_job_.busy
                    ? L"Cancel ShareX history import"
                    : L"Import captures from the selected ShareX history file (existing entries are kept)";
                return true;
            case IdToggleShareXExistingOnly:
                title = L"Skip entries whose files are no longer on disk and record file sizes";
                return true;
            case IdToggleSplitMonth: title = L"Group saved files into YYYY-MM subfolders"; return true;
            case IdFmtPng: title = L"Lossless PNG (recommended)"; return true;
            case IdFmtJpeg: title = L"Lossy JPEG (smaller files, adjustable quality)"; return true;
            case IdFmtBmp: title = L"Uncompressed BMP (fastest write, largest files)"; return true;
            case IdFmtTiff: title = L"Lossless TIFF (archival quality)"; return true;
            case IdSliderImageQuality: title = L"Adjust JPEG compression quality (10%–100%)"; return true;
            case IdToggleToast: title = L"Show a non-activating corner preview after each capture"; return true;
            case IdSliderToastSize: title = L"Adjust corner preview width (160–420 px)"; return true;
            case IdSliderToastTimeout: title = L"Adjust how long the corner preview stays visible (1–15 s)"; return true;
            case IdTestToast: title = L"Preview the corner thumbnail notification right now"; return true;
            case IdRecFmtMp4: title = L"MP4 container with H.264/HEVC video and AAC audio (recommended)"; return true;
            case IdRecFmtWmv: title = L"WMV container with Windows Media Video 9 and WMA audio"; return true;
            case IdRecFmtGif: title = L"Animated GIF via WIC (no audio, loops infinitely)"; return true;
            case IdRecCodecH264: title = L"H.264 (AVC) — universal playback compatibility"; return true;
            case IdRecCodecHevc:
                title = L"HEVC (H.265) — smaller files at equivalent quality";
                subtitle = hevc_available_ ? L"HEVC encoder detected on this PC" : L"No HEVC encoder detected — falls back to H.264";
                return true;
            case IdRecProfBase: title = L"H.264 Baseline profile — maximum legacy compatibility"; return true;
            case IdRecProfMain: title = L"H.264 Main profile — balanced quality and compatibility"; return true;
            case IdRecProfHigh: title = L"H.264 High profile — sharpest text and best compression"; return true;
            case IdRecGifColors:
                title = ulp::str::concat_strings(L"Quantize each GIF frame to ", to_fixed_wstr<16>(idx), L" adaptive colors");
                return true;
            case IdToggleRecGifDither: title = L"Enable Floyd-Steinberg error-diffusion dithering for smoother gradients"; return true;
            case IdRecFpsPreset:
                title = ulp::str::concat_strings(L"Set target frame rate to ", to_fixed_wstr<16>(idx), L" FPS");
                return true;
            case IdSliderRecFps:
                title = (sh_.settings.resolved_record_format() == RecordFormat::Gif)
                    ? L"Adjust GIF recording frame rate (5–30 FPS)"
                    : L"Adjust video recording frame rate (5–60 FPS)";
                return true;
            case IdRecScalePreset:
                title = (idx == 100)
                    ? L"Record at native 100% pixel resolution"
                    : ulp::str::concat_strings(L"Downscale recorded output to ", to_fixed_wstr<16>(idx), L"% resolution");
                return true;
            case IdSliderRecScale: title = L"Adjust output resolution scale (25%–100%)"; return true;
            case IdRecRcVbr: title = L"Variable Bitrate — adapts to motion while targeting average bitrate"; return true;
            case IdRecRcQuality: title = L"Quality-based VBR — targets consistent visual fidelity (ideal for screen text)"; return true;
            case IdRecRcCbr: title = L"Constant Bitrate — strict steady bitrate for predictable file sizes"; return true;
            case IdRecBitratePreset:
                title = ulp::str::concat_strings(L"Set target video bitrate to ", to_fixed_wstr<16>(idx), L" Mbps");
                return true;
            case IdSliderRecBitrate: title = L"Fine-tune target video bitrate (1–50 Mbps)"; return true;
            case IdSliderRecQuality: title = L"Adjust quality target (10–100; higher = sharper, larger files)"; return true;
            case IdRecKeyframe:
                title = ulp::str::concat_strings(L"Insert a full keyframe every ", to_fixed_wstr<16>(idx), idx == 1 ? L" second" : L" seconds");
                return true;
            case IdToggleRecHwAccel: title = L"Prefer GPU hardware video encoding when available (disable to force software encoder)"; return true;
            case IdToggleRecFragMp4: title = L"Write fragmented MP4 (fMP4) chunks so recordings stay playable if interrupted"; return true;
            case IdToggleRecordCursor: title = L"Include the mouse pointer in screen recordings"; return true;
            case IdToggleRecDesktop: title = L"Capture system audio via WASAPI loopback"; return true;
            case IdDropRecDesktop:
                title = L"Select desktop audio output device";
                subtitle = resolve_audio_device_label(sh_.settings.record_desktop_device_id, desktop_devices_, L"System Default");
                return true;
            case IdToggleRecMic: title = L"Capture microphone audio alongside video"; return true;
            case IdDropRecMic:
                title = L"Select microphone input device";
                subtitle = resolve_audio_device_label(sh_.settings.record_mic_device_id, mic_devices_, L"System Default");
                return true;
            case IdRecAudioBitrate:
                title = ulp::str::concat_strings(L"Encode audio track at ", to_fixed_wstr<16>(idx), L" kbps");
                return true;
            case IdRecAudioRate44: title = L"Record audio at 44.1 kHz sample rate"; return true;
            case IdRecAudioRate48: title = L"Record audio at 48 kHz sample rate (standard for video)"; return true;
            case IdRecAudioMono: title = L"Downmix audio to 1 channel (Mono — compact for voice)"; return true;
            case IdRecAudioStereo: title = L"Record 2-channel Stereo audio"; return true;
            case IdToggleAutoUpdate: title = L"Check GitHub Releases automatically once every 24 hours"; return true;
            case IdCheckUpdates: title = L"Check GitHub Releases for a newer version now"; return true;
            case IdInstallUpdate:
                title = update_job_.busy ? L"Cancel update download" : L"Download, verify, and install the latest release";
                return true;
            case IdInstallUpdateDisabled: title = L"Stop recording before installing an update"; return true;
            case IdOpenReleaseUrl: title = L"Open the GitHub Releases page in your browser"; return true;
            default: return false;
        }
    }

    void dismiss_tooltip() {
        const bool was_vis = tooltip_.visible();
        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
        if (was_vis) {
            if (last_tooltip_dirty_) {
                invalidate_rect(*last_tooltip_dirty_);
            } else {
                invalidate();
            }
        }
    }

    void draw_tabs(Canvas& cv, D2D1_SIZE_F sz) {
        if (cv.has_dirty_clip() && !cv.intersects_clip(D2D1::RectF(0.0f, 0.0f, sz.width, TABS_H + 2.0f), 2.0f)) return;
        cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, TABS_H), with_alpha(cv.theme().surface_lowest, 0.78f));

        struct TabDef { wchar_t g; const wchar_t* label; int id; };
        static constexpr std::array<TabDef, 3> tabs = {{
            {.g = glyph::keyboard, .label = L"General & Hotkeys", .id = IdTab0},
            {.g = glyph::video, .label = L"Toast & Recording", .id = IdTab1},
            {.g = glyph::info, .label = L"About", .id = IdTab2},
        },};

        std::array<D2D1_RECT_F, 3> rects{};
        float x = PAD;
        for (size_t i = 0; i < tabs.size(); ++i) {
            const float tw = cv.measure(tabs[i].label, Font::BodySemi) + 46.0f;
            rects[i] = rect(x, 8.0f, tw, TABS_H - 16.0f);
            x += tw + 8.0f;
        }

        const float t = std::clamp(tab_anim_.value, 0.0f, 2.0f);
        const int i0 = std::clamp(static_cast<int>(std::floor(t)), 0, 2);
        const int i1 = std::clamp(i0 + 1, 0, 2);
        const float frac = t - static_cast<float>(i0);
        const float tab_rad = 10.0f;

        for (int i = 0; i < 3; ++i) {
            const float act = std::clamp(1.0f - std::abs(t - static_cast<float>(i)), 0.0f, 1.0f);
            if (const float ha = hover_.alpha(tabs[static_cast<size_t>(i)].id) * indicator_hover_fade(act); ha > 0.002f) {
                cv.fill_round(rects[static_cast<size_t>(i)], tab_rad, with_alpha(cv.theme().on_surface, 0.07f * ha));
            }
        }
        cv.fill_round(lerp_rect(rects[static_cast<size_t>(i0)], rects[static_cast<size_t>(i1)], frac), tab_rad, cv.theme().primary_container);

        for (int i = 0; i < 3; ++i) {
            const D2D1_RECT_F r = rects[static_cast<size_t>(i)];
            const float act = std::clamp(1.0f - std::abs(t - static_cast<float>(i)), 0.0f, 1.0f);
            const D2D1_COLOR_F fg = lerp_color(cv.theme().on_surface_variant, cv.theme().on_primary_container, act);
            cv.icon(tabs[static_cast<size_t>(i)].g, rect(r.left + 12.0f, r.top, 18.0f, height(r)), fg, Font::IconSmall);
            cv.text(tabs[static_cast<size_t>(i)].label, rect(r.left + 34.0f, r.top, width(r) - 42.0f, height(r)), Font::BodySemi, fg);
            cv.hit(r, tabs[static_cast<size_t>(i)].id);
        }
        cv.hline(0.0f, sz.width, TABS_H, with_alpha(cv.theme().outline_variant, 0.65f));
    }

    float draw_tab_general(Canvas& cv, float x, float y, float w) {
        const float lx = x + 22.0f;
        const float rw = w - 44.0f;

        // 1. Global Hotkeys card with collision checking
        for (size_t i = 0; i < HOTKEY_COUNT; ++i) {
            const auto action = static_cast<HotkeyAction>(i);
            const HotkeyBinding& hb = sh_.settings.hotkey(action);
            const HotkeyStatus& st = sh_.hotkey_status[i];
            const bool is_rec = (recording_action_ && *recording_action_ == action);

            const CardHeader hd = make_header(cv, rw, hotkey_action_label(action), cv.theme().on_surface,
                                              hotkey_action_caption(action), 44.0f, 24.0f);

            const FixedWString<96> hk_lbl = is_rec ? FixedWString<96>{L"Press shortcut…"} : format_hotkey(hb.mods, hb.vk);
            const wchar_t* rec_btn = is_rec ? L"Cancel" : L"Record";
            const float pill_w = std::max(210.0f, cv.measure(hk_lbl, Font::BodySemi) + 28.0f);
            Flow keys{8.0f, 8.0f};
            keys.add(pill_w, 36.0f, [&](D2D1_RECT_F kr) {
                cv.fill_round(kr, 9.0f, cv.theme().surface_lowest);
                cv.stroke_round(kr, 9.0f, is_rec ? cv.theme().amber : cv.theme().outline_variant, is_rec ? 1.5f : 1.0f);
                cv.text(hk_lbl, kr, Font::BodySemi, is_rec ? cv.theme().amber : cv.theme().primary, Align::Center);
            });
            keys.add(button_width(cv, rec_btn, true, 112.0f), 36.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdRecordHotkey, static_cast<int>(i)), r, glyph::keyboard, rec_btn, IdRecordHotkey, static_cast<int>(i), is_rec ? 1 : 0);
            });
            keys.add(button_width(cv, L"Reset", false, 78.0f), 36.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdResetHotkey, static_cast<int>(i)), r, 0, L"Reset", IdResetHotkey, static_cast<int>(i), 3);
            });

            std::wstring warn_msg;
            std::wstring info_msg;
            if (is_rec && !record_conflict_msg_.empty()) {
                warn_msg = record_conflict_msg_;
            } else if (!is_rec && hb.enabled) {
                if (st.state == HotkeyState::ConflictInternal && st.conflict_with) {
                    warn_msg = ulp::str::concat_strings(
                        format_hotkey(hb.mods, hb.vk), L" conflicts with '", hotkey_action_label(*st.conflict_with), L"'. Pick a unique shortcut.");
                } else if (st.state == HotkeyState::Unavailable) {
                    if (is_windows_reserved_printscreen(hb.mods, hb.vk) && !win_prtscn_disabled_) {
                        warn_msg = ulp::str::concat_strings(
                            format_hotkey(hb.mods, hb.vk),
                            L" is reserved by Windows. Enable 'Disable built-in Windows Print Screen binding' below to use it.");
                    } else if (hb.vk == VK_SNAPSHOT) {
                        warn_msg = ulp::str::concat_strings(
                            format_hotkey(hb.mods, hb.vk),
                            L" is already registered by another app or Windows. Pick a different shortcut or check Windows Keyboard Settings.");
                    } else {
                        warn_msg = ulp::str::concat_strings(
                            format_hotkey(hb.mods, hb.vk), L" is already registered by another app. Pick a different shortcut.");
                    }
                } else if (st.state == HotkeyState::RegisteredHook) {
                    if ((hb.mods & MOD_WIN) != 0) {
                        info_msg = L"Active via PrintScreen hook fallback (Windows reserves Win + PrintScreen).";
                    } else {
                        info_msg = L"Active via PrintScreen hook fallback until Windows releases PrintScreen (sign out/in or toggle in Windows Keyboard Settings for native registration).";
                    }
                }
            }
            const float warn_h = warn_msg.empty() ? 0.0f : error_row_height(cv, warn_msg, rw);
            const float info_h = (warn_msg.empty() && !info_msg.empty()) ? text_height(cv, info_msg, Font::Caption, rw, 18.0f) : 0.0f;

            const float keys_y = y + 16.0f + hd.h + 10.0f;
            const float note_y = keys_y + keys.height(rw) + 8.0f;
            const float note_h = warn_h > 0.0f ? warn_h : info_h;
            const float card_h = (note_h > 0.0f ? (note_y + note_h) : (keys_y + keys.height(rw))) + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, card_h, 10.0f);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_switch(cv, hover_.alpha(IdToggleHotkey, static_cast<int>(i)), sw_hotkeys_[i].value, side.right, side.top + 12.0f, IdToggleHotkey, static_cast<int>(i));
            keys.draw(lx, keys_y, rw);
            if (warn_h > 0.0f) {
                draw_error_row(cv, warn_msg, lx, note_y, rw, warn_h);
            } else if (info_h > 0.0f) {
                cv.text_wrap(info_msg, rect(lx, note_y, rw, info_h), Font::Caption, cv.theme().on_surface_variant);
            }
            y = next_y;
        }

        // 1b. Windows built-in Print Screen binding setting
        {
            const CardHeader hd = make_header(
                cv, rw,
                L"Disable built-in Windows Print Screen binding",
                cv.theme().on_surface,
                L"Turns off Windows' built-in Snipping Tool binding (HKCU\\Control Panel\\Keyboard) so PrintScreen or modifier + PrintScreen can be used as a capusy keybind.",
                44.0f, 24.0f);

            const std::wstring_view status_note = win_prtscn_disabled_
                ? L"Built-in Windows Print Screen binding is disabled. You can bind PrintScreen alone or with Ctrl, Alt, Shift, or Win."
                : L"Enable this if Windows Snipping Tool intercepts PrintScreen when you want to use it as a capusy keybind.";

            Flow actions{8.0f, 8.0f};
            actions.add(button_width(cv, L"Windows Keyboard Settings", true, 196.0f), 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdOpenWinKeyboardSettings), r, glyph::open, L"Windows Keyboard Settings", IdOpenWinKeyboardSettings, 0, 3);
            });

            const float note_y = y + 16.0f + hd.h + 10.0f;
            const float err_h = win_prtscn_err_.empty() ? 0.0f : error_row_height(cv, win_prtscn_err_, rw);
            const float note_h = err_h > 0.0f ? err_h : text_height(cv, status_note, Font::Caption, rw, 18.0f);
            const float actions_y = note_y + note_h + 10.0f;
            const float card_h = (actions_y + actions.height(rw) + 16.0f) - y;
            const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_switch(cv, hover_.alpha(IdToggleWinPrtSc), sw_win_prtscn_.value, side.right, side.top + 12.0f, IdToggleWinPrtSc);
            if (err_h > 0.0f) {
                draw_error_row(cv, win_prtscn_err_, lx, note_y, rw, err_h);
            } else {
                cv.text_wrap(status_note, rect(lx, note_y, rw, note_h), Font::Caption, cv.theme().on_surface_variant);
            }
            actions.draw(lx, actions_y, rw);
            y = next_y;
        }

        // 2. Startup & Capture Behavior card
        {
            const CardHeader hd = make_header(cv, rw, L"Startup & Capture", cv.theme().on_surface,
                                              L"Windows sign-in launch, clipboard copy, and save directory");
            const std::wstring_view save_dir_w = save_dir_w_;
            const float browse_w = button_width(cv, L"Browse", true, 98.0f);
            const float open_w = button_width(cv, L"Open", true, 84.0f);

            const float r1_y = y + 16.0f + hd.h + 12.0f;
            const float r2_y = r1_y + 38.0f;
            const float r3_y = r2_y + 38.0f;
            const float r4_y = r3_y + 38.0f;
            const float line_y = r4_y + 40.0f;
            const float dir_lbl_y = line_y + 12.0f;
            const float dir_row_y = dir_lbl_y + 22.0f;
            const float card_h = (dir_row_y + 36.0f + 16.0f) - y;
            const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

            (void)draw_header(cv, lx, y, rw, hd);

            cv.text(L"Open on PC startup", rect(lx, r1_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
            draw_switch(cv, hover_.alpha(IdToggleStartup), sw_startup_.value, lx + rw, r1_y + 16.0f, IdToggleStartup);

            cv.text(L"Copy screenshots to clipboard automatically", rect(lx, r2_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
            draw_switch(cv, hover_.alpha(IdToggleAutoCopy), sw_auto_copy_.value, lx + rw, r2_y + 16.0f, IdToggleAutoCopy);

            cv.text(L"Include mouse cursor in screenshots", rect(lx, r3_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
            draw_switch(cv, hover_.alpha(IdToggleCaptureCursor), sw_cap_cursor_.value, lx + rw, r3_y + 16.0f, IdToggleCaptureCursor);

            cv.text(L"Instant region screenshot (capture on release, skip editor)", rect(lx, r4_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
            draw_switch(cv, hover_.alpha(IdToggleInstantRegion), sw_instant_region_.value, lx + rw, r4_y + 16.0f, IdToggleInstantRegion);

            cv.hline(lx, lx + rw, line_y, with_alpha(cv.theme().outline_variant, 0.5f));
            cv.text(L"Capture save folder", rect(lx, dir_lbl_y, rw, 20.0f), Font::SmallSemi, cv.theme().on_surface_variant);

            const float well_w = std::max(120.0f, rw - browse_w - open_w - 16.0f);
            const D2D1_RECT_F well = rect(lx, dir_row_y, well_w, 34.0f);
            draw_well(cv, well, 9.0f, true);
            cv.text(save_dir_w, inset(well, 10.0f, 0.0f), Font::MonoSmall, cv.theme().on_surface);
            cv.hit(well, IdSaveDirWell);
            draw_button(cv, hover_.alpha(IdBrowseSaveDir), rect(lx + well_w + 8.0f, dir_row_y, browse_w, 34.0f), glyph::folder, L"Browse", IdBrowseSaveDir, 0, 0);
            draw_button(cv, hover_.alpha(IdOpenSaveDir), rect(lx + well_w + 8.0f + browse_w + 8.0f, dir_row_y, open_w, 34.0f), glyph::open, L"Open", IdOpenSaveDir, 0, 3);

            y = next_y;
        }

        // 3. Screenshot Format & Folder Organization card
        {
            const ImageFormat fmt = sh_.settings.resolved_image_format();
            const bool show_quality = image_format_supports_quality(fmt);
            const CardHeader hd = make_header(cv, rw, L"Screenshot Format & Folders", cv.theme().on_surface,
                                              L"Image container format, compression quality, and ShareX-style monthly folders");

            Flow fmt_row{8.0f, 8.0f};
            flow_text(cv, fmt_row, L"Image format:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
            fmt_row.add(84.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdFmtPng), r, 0, L"PNG", IdFmtPng, 0, fmt == ImageFormat::Png ? 1 : 0);
            });
            fmt_row.add(84.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdFmtJpeg), r, 0, L"JPEG", IdFmtJpeg, 0, fmt == ImageFormat::Jpeg ? 1 : 0);
            });
            fmt_row.add(84.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdFmtBmp), r, 0, L"BMP", IdFmtBmp, 0, fmt == ImageFormat::Bmp ? 1 : 0);
            });
            fmt_row.add(84.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdFmtTiff), r, 0, L"TIFF", IdFmtTiff, 0, fmt == ImageFormat::Tiff ? 1 : 0);
            });

            const float split_y = y + 16.0f + hd.h + 12.0f;
            const float sep_y = split_y + 40.0f;
            const float fmt_y = sep_y + 14.0f;
            const float fmt_h = fmt_row.height(rw);
            const float qual_y = fmt_y + fmt_h + 12.0f;
            const float qual_h = show_quality ? 34.0f : 22.0f;
            const float card_h = (qual_y + qual_h + 16.0f) - y;
            const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

            (void)draw_header(cv, lx, y, rw, hd);
            cv.text(L"Organize captures into YYYY-MM folders (ShareX style)", rect(lx, split_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
            draw_switch(cv, hover_.alpha(IdToggleSplitMonth), sw_split_month_.value, lx + rw, split_y + 16.0f, IdToggleSplitMonth);

            cv.hline(lx, lx + rw, sep_y, with_alpha(cv.theme().outline_variant, 0.5f));
            fmt_row.draw(lx, fmt_y, rw);

            if (show_quality) {
                FixedWString<48> qlbl{L"JPEG quality: "};
                qlbl.append(to_fixed_wstr<16>(sh_.settings.image_quality));
                qlbl.append(L"%");
                cv.text(qlbl, rect(lx, qual_y, 170.0f, 32.0f), Font::Body, cv.theme().on_surface);
                slider_quality_r_ = rect(lx + 180.0f, qual_y, std::max(100.0f, rw - 180.0f), 32.0f);
                const float qr = std::clamp(static_cast<float>(sh_.settings.image_quality - 10) / 90.0f, 0.0f, 1.0f);
                draw_slider(cv, hover_.alpha(IdSliderImageQuality), slider_quality_r_, qr, dragging_slider_ == IdSliderImageQuality, IdSliderImageQuality);
            } else {
                const wchar_t* note = (fmt == ImageFormat::Png)
                    ? L"PNG is lossless (default) — full pixel quality with compact compression."
                    : (fmt == ImageFormat::Bmp ? L"BMP is uncompressed lossless — fastest write, largest file size."
                                               : L"TIFF is lossless — full pixel quality for archival workflows.");
                cv.text(note, rect(lx, qual_y, rw, 22.0f), Font::Caption, cv.theme().on_surface_variant);
            }

            y = next_y;
        }

        // 4. Import History from ShareX card
        {
            const JobView sx_view = view_sharex_job();
            const CardHeader hd = make_header(
                cv, rw, L"Import History from ShareX", cv.theme().on_surface,
                L"Import screenshots and recordings from ShareX History.db (SQLite) or legacy History.json backups without overwriting existing entries");

            const float browse_w = button_width(cv, L"Browse", true, 98.0f);
            const wchar_t* imp_lbl = sharex_job_.busy ? L"Cancel" : L"Import";
            const float import_w = button_width(cv, imp_lbl, true, 102.0f);

            const float file_lbl_y = y + 16.0f + hd.h + 12.0f;
            const float file_row_y = file_lbl_y + 22.0f;
            const float opt_y = file_row_y + 42.0f;

            float cy = opt_y + 34.0f;
            const bool show_bar = sharex_job_.busy && sx_view.total > 0;
            const float bar_y = cy + 8.0f;
            if (show_bar) cy = bar_y + 8.0f;

            const float msg_y = cy + 8.0f;
            const float msg_h = sx_view.log.empty()
                ? 0.0f
                : (sx_view.failed ? error_row_height(cv, sx_view.log, rw) : text_height(cv, sx_view.log, Font::Caption, rw, 18.0f));
            if (msg_h > 0.0f) cy = msg_y + msg_h;

            const float card_h = (cy + 16.0f) - y;
            const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

            (void)draw_header(cv, lx, y, rw, hd);
            cv.text(L"ShareX history file (History.db or History.json)", rect(lx, file_lbl_y, rw, 20.0f), Font::SmallSemi, cv.theme().on_surface_variant);

            const float well_w = std::max(120.0f, rw - browse_w - import_w - 16.0f);
            const D2D1_RECT_F well = rect(lx, file_row_y, well_w, 34.0f);
            draw_well(cv, well, 9.0f, true);
            const std::wstring_view path_display = sharex_path_w_.empty()
                ? std::wstring_view{L"Select ShareX History.db or legacy History.json…"}
                : std::wstring_view{sharex_path_w_};
            cv.text(path_display, inset(well, 10.0f, 0.0f), Font::MonoSmall,
                    sharex_path_w_.empty() ? cv.theme().outline : cv.theme().on_surface);
            cv.hit(well, IdShareXPathWell);

            const D2D1_RECT_F browse_r = rect(lx + well_w + 8.0f, file_row_y, browse_w, 34.0f);
            if (sharex_job_.busy) {
                draw_disabled_button(cv, browse_r, glyph::folder, L"Browse");
            } else {
                draw_button(cv, hover_.alpha(IdBrowseShareX), browse_r, glyph::folder, L"Browse", IdBrowseShareX, 0, 0);
            }
            const D2D1_RECT_F import_r = rect(lx + well_w + 8.0f + browse_w + 8.0f, file_row_y, import_w, 34.0f);
            draw_button(cv, hover_.alpha(IdImportShareX), import_r,
                        sharex_job_.busy ? glyph::close : glyph::download, imp_lbl, IdImportShareX, 0,
                        sharex_job_.busy ? 2 : 1);

            cv.text(L"Only import files that still exist on disk", rect(lx, opt_y, rw - 60.0f, 32.0f), Font::Body,
                    sharex_job_.busy ? with_alpha(cv.theme().on_surface_variant, 0.6f) : cv.theme().on_surface);
            draw_switch(cv, sharex_job_.busy ? 0.0f : hover_.alpha(IdToggleShareXExistingOnly),
                        sw_sharex_existing_only_.value, lx + rw, opt_y + 16.0f,
                        sharex_job_.busy ? IdNone : IdToggleShareXExistingOnly);

            if (show_bar) {
                const float pct = std::clamp(static_cast<float>(sx_view.done) / static_cast<float>(sx_view.total), 0.0f, 1.0f);
                const D2D1_RECT_F trk = rect(lx, bar_y, rw, 6.0f);
                cv.fill_round(trk, 3.0f, with_alpha(cv.theme().outline_variant, 0.35f));
                if (pct > 0.0f) {
                    cv.fill_round(rect(lx, bar_y, std::max(6.0f, rw * pct), 6.0f), 3.0f, cv.theme().primary);
                }
            }
            if (msg_h > 0.0f) {
                if (sx_view.failed) {
                    draw_error_row(cv, sx_view.log, lx, msg_y, rw, msg_h);
                } else {
                    cv.text_wrap(sx_view.log, rect(lx, msg_y, rw, msg_h), Font::Caption, cv.theme().on_surface_variant);
                }
            }

            y = next_y;
        }
        return y;
    }

    float draw_tab_toast_recording(Canvas& cv, float x, float y, float w) {
        const float lx = x + 22.0f;
        const float rw = w - 44.0f;

        // 1. Corner Thumbnail Toast card
        {
            const CardHeader hd = make_header(cv, rw, L"Corner Preview Thumbnail", cv.theme().on_surface,
                                              L"Non-activating thumbnail in the corner of your primary monitor after capture (never steals focus)",
                                              44.0f, 24.0f);

            const float s1_y = y + 16.0f + hd.h + 14.0f;
            const float s2_y = s1_y + 44.0f;
            const float btn_y = s2_y + 46.0f;
            const float card_h = (btn_y + 34.0f + 16.0f) - y;
            const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_switch(cv, hover_.alpha(IdToggleToast), sw_toast_.value, side.right, side.top + 12.0f, IdToggleToast);

            // Size slider (160 .. 420 DIPs)
            FixedWString<48> sz_lbl{L"Thumbnail width: "};
            sz_lbl.append(to_fixed_wstr<16>(static_cast<int>(std::lround(sh_.settings.toast_width_dip))));
            sz_lbl.append(L" px");
            cv.text(sz_lbl, rect(lx, s1_y, 200.0f, 32.0f), Font::Body, cv.theme().on_surface);
            slider_size_r_ = rect(lx + 210.0f, s1_y, std::max(100.0f, rw - 210.0f), 32.0f);
            const float sz_ratio = std::clamp((sh_.settings.toast_width_dip - 160.0f) / (420.0f - 160.0f), 0.0f, 1.0f);
            draw_slider(cv, hover_.alpha(IdSliderToastSize), slider_size_r_, sz_ratio, dragging_slider_ == IdSliderToastSize, IdSliderToastSize);

            // Timeout slider (1.0 .. 15.0 seconds)
            FixedWString<48> to_lbl{L"Dismiss timeout: "};
            to_lbl.append(to_fixed_wstr<16>(static_cast<double>(sh_.settings.toast_timeout_s), 1));
            to_lbl.append(L" s");
            cv.text(to_lbl, rect(lx, s2_y, 200.0f, 32.0f), Font::Body, cv.theme().on_surface);
            slider_timeout_r_ = rect(lx + 210.0f, s2_y, std::max(100.0f, rw - 210.0f), 32.0f);
            const float to_ratio = std::clamp((sh_.settings.toast_timeout_s - 1.0f) / (15.0f - 1.0f), 0.0f, 1.0f);
            draw_slider(cv, hover_.alpha(IdSliderToastTimeout), slider_timeout_r_, to_ratio, dragging_slider_ == IdSliderToastTimeout, IdSliderToastTimeout);

            const float tw = button_width(cv, L"Test Corner Preview", true, 168.0f);
            draw_button(cv, hover_.alpha(IdTestToast), rect(lx, btn_y, tw, 34.0f), glyph::photo, L"Test Corner Preview", IdTestToast, 0, 0);
            y = next_y;
        }

        // 2. Region Screen Recording — Format & Video card
        {
            const RecordFormat fmt = sh_.settings.resolved_record_format();
            const VideoCodec codec = sh_.settings.resolved_record_codec();
            const H264Profile prof = sh_.settings.resolved_h264_profile();
            const RateControlMode rc = sh_.settings.resolved_rate_control();
            const bool is_mp4 = (fmt == RecordFormat::Mp4);
            const bool is_wmv = (fmt == RecordFormat::Wmv);
            const bool is_gif = (fmt == RecordFormat::Gif);
            const uint32_t eff_fps = effective_record_fps(sh_.settings.record_fps, fmt);

            const CardHeader hd = make_header(
                cv, rw, L"Screen Recording — Format & Video", cv.theme().on_surface,
                L"Container format, codec, frame rate, resolution scale, and compression quality");

            Flow fmt_row{8.0f, 8.0f};
            flow_text(cv, fmt_row, L"Output format:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
            fmt_row.add(78.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdRecFmtMp4), r, 0, L"MP4", IdRecFmtMp4, 0, is_mp4 ? 1 : 0);
            });
            fmt_row.add(78.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdRecFmtWmv), r, 0, L"WMV", IdRecFmtWmv, 0, is_wmv ? 1 : 0);
            });
            fmt_row.add(78.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdRecFmtGif), r, 0, L"GIF", IdRecFmtGif, 0, is_gif ? 1 : 0);
            });

            Flow codec_row{8.0f, 8.0f};
            Flow prof_row{8.0f, 8.0f};
            Flow gif_row{8.0f, 8.0f};
            std::wstring_view format_note;
            if (is_mp4) {
                flow_text(cv, codec_row, L"Video codec:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                codec_row.add(86.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecCodecH264), r, 0, L"H.264", IdRecCodecH264, 0, codec == VideoCodec::H264 ? 1 : 0);
                });
                codec_row.add(124.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecCodecHevc), r, 0, L"HEVC (H.265)", IdRecCodecHevc, 0, codec == VideoCodec::Hevc ? 1 : 0);
                });
                if (codec == VideoCodec::H264) {
                    flow_text(cv, prof_row, L"H.264 profile:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                    prof_row.add(88.0f, 32.0f, [&](D2D1_RECT_F r) {
                        draw_button(cv, hover_.alpha(IdRecProfBase), r, 0, L"Baseline", IdRecProfBase, 0, prof == H264Profile::Baseline ? 1 : 0);
                    });
                    prof_row.add(78.0f, 32.0f, [&](D2D1_RECT_F r) {
                        draw_button(cv, hover_.alpha(IdRecProfMain), r, 0, L"Main", IdRecProfMain, 0, prof == H264Profile::Main ? 1 : 0);
                    });
                    prof_row.add(78.0f, 32.0f, [&](D2D1_RECT_F r) {
                        draw_button(cv, hover_.alpha(IdRecProfHigh), r, 0, L"High", IdRecProfHigh, 0, prof == H264Profile::High ? 1 : 0);
                    });
                } else if (!hevc_available_) {
                    format_note = L"No HEVC encoder detected on this PC — recordings will automatically fall back to H.264.";
                }
            } else if (is_wmv) {
                format_note = L"Windows Media Video 9 (WMV3) + WMA audio inside a native ASF/WMV container.";
            } else {
                flow_text(cv, gif_row, L"GIF palette:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                for (const int c : {64, 128, 256}) {
                    const FixedWString<24> clbl = [&] {
                        FixedWString<24> t{to_fixed_wstr<8>(c)};
                        t.append(L" colors");
                        return t;
                    }();
                    gif_row.add(92.0f, 32.0f, [&, c, clbl](D2D1_RECT_F r) {
                        draw_button(cv, hover_.alpha(IdRecGifColors, c), r, 0, clbl, IdRecGifColors, c,
                                    std::cmp_equal(sh_.settings.record_gif_colors, c) ? 1 : 0);
                    });
                }
            }

            Flow fps_row{8.0f, 8.0f};
            flow_text(cv, fps_row, L"Frame rate:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
            const std::array<int, 4> fps_presets = is_gif ? std::array<int, 4>{10, 15, 20, 30} : std::array<int, 4>{15, 24, 30, 60};
            for (const int fp : fps_presets) {
                FixedWString<16> flbl{to_fixed_wstr<8>(fp)};
                flbl.append(L" FPS");
                fps_row.add(78.0f, 32.0f, [&, fp, flbl](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecFpsPreset, fp), r, 0, flbl, IdRecFpsPreset, fp,
                                std::cmp_equal(eff_fps, fp) ? 1 : 0);
                });
            }

            Flow scale_row{8.0f, 8.0f};
            flow_text(cv, scale_row, L"Resolution:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
            for (const int sc : {100, 75, 50}) {
                const wchar_t* slbl = (sc == 100) ? L"100% (Native)" : (sc == 75 ? L"75%" : L"50%");
                const float sw = (sc == 100) ? 118.0f : 68.0f;
                scale_row.add(sw, 32.0f, [&, sc, slbl](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecScalePreset, sc), r, 0, slbl, IdRecScalePreset, sc,
                                std::cmp_equal(sh_.settings.record_scale_pct, sc) ? 1 : 0);
                });
            }

            Flow rc_row{8.0f, 8.0f};
            Flow br_row{8.0f, 8.0f};
            Flow kf_row{8.0f, 8.0f};
            if (!is_gif) {
                flow_text(cv, rc_row, L"Rate control:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                rc_row.add(76.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecRcVbr), r, 0, L"VBR", IdRecRcVbr, 0, rc == RateControlMode::Vbr ? 1 : 0);
                });
                rc_row.add(86.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecRcQuality), r, 0, L"Quality", IdRecRcQuality, 0, rc == RateControlMode::Quality ? 1 : 0);
                });
                rc_row.add(76.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecRcCbr), r, 0, L"CBR", IdRecRcCbr, 0, rc == RateControlMode::Cbr ? 1 : 0);
                });

                if (rc != RateControlMode::Quality) {
                    flow_text(cv, br_row, L"Bitrate presets:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                    for (const int mb : {4, 8, 12, 20, 35}) {
                        FixedWString<16> blbl{to_fixed_wstr<8>(mb)};
                        blbl.append(L" Mbps");
                        br_row.add(78.0f, 32.0f, [&, mb, blbl](D2D1_RECT_F r) {
                            draw_button(cv, hover_.alpha(IdRecBitratePreset, mb), r, 0, blbl, IdRecBitratePreset, mb,
                                        std::cmp_equal(sh_.settings.record_bitrate_mbps, mb) ? 1 : 0);
                        });
                    }
                }

                flow_text(cv, kf_row, L"Keyframe every:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                for (const int ks : {1, 2, 5, 10}) {
                    FixedWString<16> klbl{to_fixed_wstr<8>(ks)};
                    klbl.append(L" s");
                    kf_row.add(64.0f, 32.0f, [&, ks, klbl](D2D1_RECT_F r) {
                        draw_button(cv, hover_.alpha(IdRecKeyframe, ks), r, 0, klbl, IdRecKeyframe, ks,
                                    std::cmp_equal(sh_.settings.record_keyframe_s, ks) ? 1 : 0);
                    });
                }
            }

            // Compute exact vertical layout
            float cy = y + 16.0f + hd.h + 14.0f;
            const float fmt_y = cy;
            cy += fmt_row.height(rw) + 10.0f;

            float codec_y = 0.0f;
            float prof_y = 0.0f;
            float gif_y = 0.0f;
            float gif_dither_y = 0.0f;
            float fmt_note_y = 0.0f;
            float fmt_note_h = 0.0f;

            if (is_mp4) {
                codec_y = cy;
                cy += codec_row.height(rw) + 10.0f;
                if (codec == VideoCodec::H264) {
                    prof_y = cy;
                    cy += prof_row.height(rw) + 10.0f;
                } else if (!format_note.empty()) {
                    fmt_note_y = cy;
                    fmt_note_h = text_height(cv, format_note, Font::Caption, rw, 18.0f);
                    cy += fmt_note_h + 10.0f;
                }
            } else if (is_wmv) {
                fmt_note_y = cy;
                fmt_note_h = text_height(cv, format_note, Font::Caption, rw, 18.0f);
                cy += fmt_note_h + 10.0f;
            } else {
                gif_y = cy;
                cy += gif_row.height(rw) + 10.0f;
                gif_dither_y = cy;
                cy += 36.0f;
            }

            const float sep1_y = cy + 2.0f;
            cy = sep1_y + 14.0f;

            const float fps_y = cy;
            cy += fps_row.height(rw) + 8.0f;
            const float fps_slider_y = cy;
            cy += 38.0f;

            const float scale_y = cy;
            cy += scale_row.height(rw) + 8.0f;
            const float scale_slider_y = cy;
            cy += 38.0f;

            float sep2_y = 0.0f;
            float rc_y = 0.0f;
            float br_y = 0.0f;
            float br_slider_y = 0.0f;
            float q_slider_y = 0.0f;
            float q_note_y = 0.0f;
            float q_note_h = 0.0f;
            float kf_y = 0.0f;
            float hw_y = 0.0f;
            float frag_y = 0.0f;
            const std::wstring_view q_note = L"Quality-based VBR adapts bitrate automatically — crisp text with compact static frames.";

            if (!is_gif) {
                sep2_y = cy + 2.0f;
                cy = sep2_y + 14.0f;

                rc_y = cy;
                cy += rc_row.height(rw) + 10.0f;

                if (rc == RateControlMode::Quality) {
                    q_slider_y = cy;
                    cy += 36.0f;
                    q_note_y = cy;
                    q_note_h = text_height(cv, q_note, Font::Caption, rw, 18.0f);
                    cy += q_note_h + 12.0f;
                } else {
                    br_y = cy;
                    cy += br_row.height(rw) + 8.0f;
                    br_slider_y = cy;
                    cy += 40.0f;
                }

                kf_y = cy;
                cy += kf_row.height(rw) + 12.0f;

                hw_y = cy;
                cy += 36.0f;
                if (is_mp4) {
                    frag_y = cy;
                    cy += 36.0f;
                }
            }

            const float sep3_y = cy + 2.0f;
            const float cur_y = sep3_y + 12.0f;
            const float card_h = (cur_y + 34.0f + 16.0f) - y;
            const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

            (void)draw_header(cv, lx, y, rw, hd);
            fmt_row.draw(lx, fmt_y, rw);
            if (is_mp4) {
                codec_row.draw(lx, codec_y, rw);
                if (codec == VideoCodec::H264) {
                    prof_row.draw(lx, prof_y, rw);
                } else if (fmt_note_h > 0.0f) {
                    cv.text_wrap(format_note, rect(lx, fmt_note_y, rw, fmt_note_h), Font::Caption, cv.theme().amber);
                }
            } else if (is_wmv) {
                cv.text_wrap(format_note, rect(lx, fmt_note_y, rw, fmt_note_h), Font::Caption, cv.theme().on_surface_variant);
            } else {
                gif_row.draw(lx, gif_y, rw);
                cv.text(L"Error-diffusion dithering (smoother gradients)", rect(lx, gif_dither_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
                draw_switch(cv, hover_.alpha(IdToggleRecGifDither), sw_rec_gif_dither_.value, lx + rw, gif_dither_y + 16.0f, IdToggleRecGifDither);
            }

            cv.hline(lx, lx + rw, sep1_y, with_alpha(cv.theme().outline_variant, 0.5f));
            fps_row.draw(lx, fps_y, rw);
            {
                FixedWString<48> fps_lbl{L"Target FPS: "};
                fps_lbl.append(to_fixed_wstr<8>(eff_fps));
                if (is_gif && sh_.settings.record_fps > RECORD_GIF_FPS_MAX) fps_lbl.append(L" (GIF max)");
                cv.text(fps_lbl, rect(lx, fps_slider_y, 180.0f, 32.0f), Font::Body, cv.theme().on_surface);
                slider_rec_fps_r_ = rect(lx + 190.0f, fps_slider_y, std::max(100.0f, rw - 190.0f), 32.0f);
                const auto max_fps_f = static_cast<float>(is_gif ? RECORD_GIF_FPS_MAX : RECORD_FPS_MAX);
                const float fps_ratio = std::clamp(
                    (static_cast<float>(eff_fps) - static_cast<float>(RECORD_FPS_MIN)) / (max_fps_f - static_cast<float>(RECORD_FPS_MIN)),
                    0.0f, 1.0f);
                draw_slider(cv, hover_.alpha(IdSliderRecFps), slider_rec_fps_r_, fps_ratio, dragging_slider_ == IdSliderRecFps, IdSliderRecFps);
            }

            scale_row.draw(lx, scale_y, rw);
            {
                FixedWString<48> sc_lbl{L"Output scale: "};
                sc_lbl.append(to_fixed_wstr<8>(sh_.settings.record_scale_pct));
                sc_lbl.append(L"%");
                cv.text(sc_lbl, rect(lx, scale_slider_y, 180.0f, 32.0f), Font::Body, cv.theme().on_surface);
                slider_rec_scale_r_ = rect(lx + 190.0f, scale_slider_y, std::max(100.0f, rw - 190.0f), 32.0f);
                const float sc_ratio = std::clamp(
                    static_cast<float>(sh_.settings.record_scale_pct - RECORD_SCALE_MIN_PCT) /
                        static_cast<float>(RECORD_SCALE_MAX_PCT - RECORD_SCALE_MIN_PCT),
                    0.0f, 1.0f);
                draw_slider(cv, hover_.alpha(IdSliderRecScale), slider_rec_scale_r_, sc_ratio, dragging_slider_ == IdSliderRecScale, IdSliderRecScale);
            }

            if (!is_gif) {
                cv.hline(lx, lx + rw, sep2_y, with_alpha(cv.theme().outline_variant, 0.5f));
                rc_row.draw(lx, rc_y, rw);
                if (rc == RateControlMode::Quality) {
                    FixedWString<48> qlbl{L"Quality target: "};
                    qlbl.append(to_fixed_wstr<8>(sh_.settings.record_quality));
                    qlbl.append(L" / 100");
                    cv.text(qlbl, rect(lx, q_slider_y, 180.0f, 32.0f), Font::Body, cv.theme().on_surface);
                    slider_rec_quality_r_ = rect(lx + 190.0f, q_slider_y, std::max(100.0f, rw - 190.0f), 32.0f);
                    const float q_ratio = std::clamp(
                        static_cast<float>(sh_.settings.record_quality - RECORD_QUALITY_MIN) /
                            static_cast<float>(RECORD_QUALITY_MAX - RECORD_QUALITY_MIN),
                        0.0f, 1.0f);
                    draw_slider(cv, hover_.alpha(IdSliderRecQuality), slider_rec_quality_r_, q_ratio, dragging_slider_ == IdSliderRecQuality, IdSliderRecQuality);
                    cv.text_wrap(q_note, rect(lx, q_note_y, rw, q_note_h), Font::Caption, cv.theme().on_surface_variant);
                } else {
                    br_row.draw(lx, br_y, rw);
                    FixedWString<48> blbl{L"Video bitrate: "};
                    blbl.append(to_fixed_wstr<8>(sh_.settings.record_bitrate_mbps));
                    blbl.append(L" Mbps");
                    cv.text(blbl, rect(lx, br_slider_y, 180.0f, 32.0f), Font::Body, cv.theme().on_surface);
                    slider_rec_bitrate_r_ = rect(lx + 190.0f, br_slider_y, std::max(100.0f, rw - 190.0f), 32.0f);
                    const float br_ratio = std::clamp(
                        static_cast<float>(sh_.settings.record_bitrate_mbps - RECORD_BITRATE_MIN_MBPS) /
                            static_cast<float>(RECORD_BITRATE_MAX_MBPS - RECORD_BITRATE_MIN_MBPS),
                        0.0f, 1.0f);
                    draw_slider(cv, hover_.alpha(IdSliderRecBitrate), slider_rec_bitrate_r_, br_ratio, dragging_slider_ == IdSliderRecBitrate, IdSliderRecBitrate);
                }

                kf_row.draw(lx, kf_y, rw);

                cv.text(L"Use hardware video encoder (GPU)", rect(lx, hw_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
                draw_switch(cv, hover_.alpha(IdToggleRecHwAccel), sw_rec_hw_accel_.value, lx + rw, hw_y + 16.0f, IdToggleRecHwAccel);

                if (is_mp4) {
                    cv.text(L"Crash-safe fragmented MP4 (resilient to power loss)", rect(lx, frag_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
                    draw_switch(cv, hover_.alpha(IdToggleRecFragMp4), sw_rec_frag_mp4_.value, lx + rw, frag_y + 16.0f, IdToggleRecFragMp4);
                }
            }

            cv.hline(lx, lx + rw, sep3_y, with_alpha(cv.theme().outline_variant, 0.5f));
            cv.text(L"Include mouse cursor in recordings", rect(lx, cur_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
            draw_switch(cv, hover_.alpha(IdToggleRecordCursor), sw_rec_cursor_.value, lx + rw, cur_y + 16.0f, IdToggleRecordCursor);

            y = next_y;
        }

        // 3. Region Screen Recording — Audio card
        {
            const RecordFormat fmt = sh_.settings.resolved_record_format();
            const bool audio_supported = record_format_supports_audio(fmt);
            const CardHeader hd = make_header(
                cv, rw, L"Screen Recording — Audio", cv.theme().on_surface,
                L"WASAPI desktop loopback, microphone mixing, and audio encoding quality");

            if (!audio_supported) {
                const std::wstring_view no_aud = L"Animated GIF recordings do not include audio tracks. Switch format to MP4 or WMV to record audio.";
                const float ny = y + 16.0f + hd.h + 12.0f;
                const float nh = text_height(cv, no_aud, Font::Caption, rw, 20.0f);
                const float card_h = (ny + nh + 16.0f) - y;
                const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);
                (void)draw_header(cv, lx, y, rw, hd);
                cv.text_wrap(no_aud, rect(lx, ny, rw, nh), Font::Caption, cv.theme().on_surface_variant);
                y = next_y;
            } else {
                Flow abr_row{8.0f, 8.0f};
                flow_text(cv, abr_row, L"Audio bitrate:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                for (const int kb : {96, 128, 160, 192}) {
                    FixedWString<16> klbl{to_fixed_wstr<8>(kb)};
                    klbl.append(L" kbps");
                    abr_row.add(84.0f, 32.0f, [&, kb, klbl](D2D1_RECT_F r) {
                        draw_button(cv, hover_.alpha(IdRecAudioBitrate, kb), r, 0, klbl, IdRecAudioBitrate, kb,
                                    std::cmp_equal(sh_.settings.record_audio_bitrate_kbps, kb) ? 1 : 0);
                    });
                }

                Flow asr_row{8.0f, 8.0f};
                flow_text(cv, asr_row, L"Sample rate:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                asr_row.add(92.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecAudioRate44), r, 0, L"44.1 kHz", IdRecAudioRate44, 0,
                                sh_.settings.record_audio_sample_rate == 44100u ? 1 : 0);
                });
                asr_row.add(92.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecAudioRate48), r, 0, L"48 kHz", IdRecAudioRate48, 0,
                                sh_.settings.record_audio_sample_rate == 48000u ? 1 : 0);
                });

                Flow ach_row{8.0f, 8.0f};
                flow_text(cv, ach_row, L"Channels:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
                ach_row.add(84.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecAudioMono), r, 0, L"Mono", IdRecAudioMono, 0,
                                sh_.settings.record_audio_channels == 1u ? 1 : 0);
                });
                ach_row.add(84.0f, 32.0f, [&](D2D1_RECT_F r) {
                    draw_button(cv, hover_.alpha(IdRecAudioStereo), r, 0, L"Stereo", IdRecAudioStereo, 0,
                                sh_.settings.record_audio_channels == 2u ? 1 : 0);
                });

                const float desk_y = y + 16.0f + hd.h + 14.0f;
                const float desk_drop_y = desk_y + 36.0f;
                const float mic_y = desk_drop_y + 44.0f;
                const float mic_drop_y = mic_y + 36.0f;
                const float sep_y = mic_drop_y + 44.0f;
                const float abr_y = sep_y + 14.0f;
                const float asr_y = abr_y + abr_row.height(rw) + 10.0f;
                const float ach_y = asr_y + asr_row.height(rw) + 10.0f;
                const float card_h = (ach_y + ach_row.height(rw) + 16.0f) - y;
                const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

                (void)draw_header(cv, lx, y, rw, hd);

                cv.text(L"Record desktop audio", rect(lx, desk_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
                draw_switch(cv, hover_.alpha(IdToggleRecDesktop), sw_rec_desktop_.value, lx + rw, desk_y + 16.0f, IdToggleRecDesktop);
                desktop_drop_r_ = rect(lx, desk_drop_y, rw, 34.0f);
                const std::wstring desk_lbl = resolve_audio_device_label(sh_.settings.record_desktop_device_id, desktop_devices_, L"System Default");
                draw_dropdown_field(cv, hover_.alpha(IdDropRecDesktop), desktop_drop_r_, desk_lbl,
                                    open_audio_drop_ == AudioDropdown::Desktop,
                                    sh_.settings.record_desktop_audio && !desktop_devices_.empty(),
                                    desktop_devices_.empty() ? IdNone : IdDropRecDesktop);

                cv.text(L"Record microphone audio", rect(lx, mic_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
                draw_switch(cv, hover_.alpha(IdToggleRecMic), sw_rec_mic_.value, lx + rw, mic_y + 16.0f, IdToggleRecMic);
                mic_drop_r_ = rect(lx, mic_drop_y, rw, 34.0f);
                const std::wstring mic_lbl = resolve_audio_device_label(sh_.settings.record_mic_device_id, mic_devices_, L"System Default");
                draw_dropdown_field(cv, hover_.alpha(IdDropRecMic), mic_drop_r_, mic_lbl,
                                    open_audio_drop_ == AudioDropdown::Mic,
                                    sh_.settings.record_mic_audio && !mic_devices_.empty(),
                                    mic_devices_.empty() ? IdNone : IdDropRecMic);

                cv.hline(lx, lx + rw, sep_y, with_alpha(cv.theme().outline_variant, 0.5f));
                abr_row.draw(lx, abr_y, rw);
                asr_row.draw(lx, asr_y, rw);
                ach_row.draw(lx, ach_y, rw);

                y = next_y;
            }
        }

        return y;
    }

    ID2D1Bitmap1* ensure_app_icon(Canvas& cv) {
        const uint64_t epoch = sh_.gfx.epoch();
        if (app_icon_ && app_icon_epoch_ == epoch) return app_icon_.get();
        app_icon_.reset();
        app_icon_epoch_ = epoch;
        IWICImagingFactory* wic = sh_.gfx.wic();
        if (!wic || !inst_) return nullptr;
        auto* hi = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 128, 128, LR_DEFAULTCOLOR));
        if (!hi) return nullptr;
        const wil::unique_hicon own{hi};
        wil::com_ptr<IWICBitmap> wic_bmp;
        if (FAILED(wic->CreateBitmapFromHICON(own.get(), &wic_bmp))) return nullptr;
        wil::com_ptr<IWICFormatConverter> conv;
        if (FAILED(wic->CreateFormatConverter(&conv))) return nullptr;
        if (FAILED(conv->Initialize(wic_bmp.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;
        (void)cv.dc()->CreateBitmapFromWicBitmap(conv.get(), nullptr, &app_icon_);
        return app_icon_.get();
    }

    float draw_tab_about(Canvas& cv, float x, float y, float w) {
        const float lx = x + 22.0f;
        const float rw = w - 44.0f;
        const std::wstring ver = ulp::str::concat_strings(L"Version ", APP_VERSION, L"  •  by usy");
        const std::wstring ai_title = L"AI-generated codebase";
        const std::wstring ai_desc = L"All source code in capusy was written by AI.";
        const float box_pad_x = 18.0f;
        const float box_inner_w = std::max(120.0f, rw - (box_pad_x * 2.0f));
        const float ai_title_h = text_height(cv, ai_title, Font::BodySemi, box_inner_w, 22.0f);
        const float ai_desc_h = text_height(cv, ai_desc, Font::Small, box_inner_w, 18.0f);
        const float box_h = 14.0f + ai_title_h + 4.0f + ai_desc_h + 14.0f;

        constexpr float icon_sz = 72.0f;
        const float icon_y = y + 28.0f;
        const float name_y = icon_y + icon_sz + 14.0f;
        const float ver_y = name_y + 28.0f;
        const float line_y = ver_y + 22.0f + 16.0f;
        const float box_y = line_y + 18.0f;
        const float db_info_y = box_y + box_h + 16.0f;
        const float db_path_y = db_info_y + 22.0f;
        const float card_h = (db_path_y + 20.0f + 20.0f) - y;
        const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

        const D2D1_RECT_F icon_r = cv.snap(rect(x + ((w - icon_sz) * 0.5f), icon_y, icon_sz, icon_sz));
        if (ID2D1Bitmap1* bmp = ensure_app_icon(cv)) {
            cv.bitmap(bmp, icon_r);
        } else {
            cv.icon(glyph::camera, icon_r, cv.theme().primary, Font::IconLarge);
        }

        cv.text(APP_NAME, rect(lx, name_y, rw, 26.0f), Font::Title, cv.theme().on_surface, Align::Center);
        cv.text(ver, rect(lx, ver_y, rw, 22.0f), Font::Body, cv.theme().on_surface_variant, Align::Center);
        cv.hline(lx, lx + rw, line_y, with_alpha(cv.theme().outline_variant, 0.5f));

        const D2D1_RECT_F box_r = rect(lx, box_y, rw, box_h);
        const float box_rad = offset_radius(cv.theme().card_radius, -2.0f);
        cv.fill_round(box_r, box_rad, with_alpha(cv.theme().surface_high, 0.55f));
        cv.stroke_round(box_r, box_rad, with_alpha(cv.theme().outline_variant, 0.45f));
        cv.text_wrap(ai_title, rect(lx + box_pad_x, box_y + 14.0f, box_inner_w, ai_title_h),
                     Font::BodySemi, cv.theme().on_surface, Align::Center);
        cv.text_wrap(ai_desc, rect(lx + box_pad_x, box_y + 14.0f + ai_title_h + 4.0f, box_inner_w, ai_desc_h),
                     Font::Small, cv.theme().on_surface_variant, Align::Center);

        const std::wstring db_info = ulp::str::concat_strings(
            L"History database: ", format_count(stats_.total_count), L" captures (",
            format_count(stats_.image_count), L" images, ", format_count(stats_.video_count), L" videos)  •  ",
            format_size(stats_.db_bytes));
        cv.text(db_info, rect(lx, db_info_y, rw, 22.0f), Font::Small, cv.theme().on_surface_variant, Align::Center);
        cv.text(history_db_path().wstring(), rect(lx, db_path_y, rw, 20.0f), Font::MonoSmall, cv.theme().outline, Align::Center);
        y = next_y;

        // Software Updates card (modeled on usyseek)
        {
            const JobView upd_view = view_update_job();
            const bool update_avail = sh_.has_update_available();
            const bool can_install = update_avail && !sh_.update_state.asset_url.empty();

            std::wstring caption;
            D2D1_COLOR_F title_col = cv.theme().on_surface;
            if (sh_.update_checking) {
                caption = L"Checking GitHub Releases…";
            } else if (update_avail) {
                overwrite_to(
                    caption,
                    L"New version ",
                    sh_.update_state.latest_tag.empty() ? "v" : "",
                    sh_.update_state.latest_tag.empty() ? sh_.update_state.latest_version : sh_.update_state.latest_tag,
                    L" is available (current: v", APP_VERSION, L").");
                title_col = cv.theme().primary;
            } else if (!sh_.update_state.last_error.empty()) {
                overwrite_to(caption, L"Last check failed: ", sh_.update_state.last_error);
                if (sh_.update_state.last_check_unix > 0) {
                    append_to(caption, L"  •  Checked ", format_ago(sh_.update_state.last_check_unix));
                }
            } else if (sh_.update_state.last_check_unix > 0) {
                overwrite_to(caption, L"Up to date  •  Last checked ", format_ago(sh_.update_state.last_check_unix));
            } else {
                caption = L"Checks github.com/usyless/capusy/releases once every 24 hours.";
            }

            const CardHeader hd = make_header(cv, rw, L"Software Updates", title_col, caption);

            const wchar_t* check_lbl = sh_.update_checking ? L"Checking…" : L"Check Now";
            std::wstring install_lbl = L"Download & Install";
            if (update_job_.busy) {
                install_lbl = upd_view.total > 0
                    ? ulp::str::concat_strings(L"Cancel  •  ", to_fixed_wstr(upd_view.done * 100 / upd_view.total), L"%")
                    : L"Cancel";
            } else if (update_avail) {
                overwrite_to(
                    install_lbl,
                    L"Download & Install ",
                    sh_.update_state.latest_tag.empty() ? "v" : "",
                    sh_.update_state.latest_tag.empty() ? sh_.update_state.latest_version : sh_.update_state.latest_tag);
            }

            Flow actions{8.0f, 8.0f};
            actions.add(button_width(cv, check_lbl, true, 126.0f), 34.0f, [&](D2D1_RECT_F r) {
                if (sh_.update_checking || update_job_.busy) {
                    draw_disabled_button(cv, r, glyph::refresh, check_lbl, IdNone);
                } else {
                    draw_button(cv, hover_.alpha(IdCheckUpdates), r, glyph::refresh, check_lbl, IdCheckUpdates, 0, 0);
                }
            });
            if (can_install || update_job_.busy) {
                actions.add(button_width(cv, install_lbl, true, 170.0f), 34.0f, [&](D2D1_RECT_F r) {
                    if (sh_.recording_active && !update_job_.busy) {
                        draw_disabled_button(cv, r, glyph::download, install_lbl, IdInstallUpdateDisabled);
                    } else {
                        draw_button(cv, hover_.alpha(IdInstallUpdate), r, update_job_.busy ? glyph::close : glyph::download,
                                    install_lbl, IdInstallUpdate, 0, update_job_.busy ? 2 : 1);
                    }
                });
            }
            actions.add(button_width(cv, L"View Releases", true, 134.0f), 34.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdOpenReleaseUrl), r, glyph::open, L"View Releases", IdOpenReleaseUrl, 0, 3);
            });

            std::wstring pkg_note;
            if (update_avail && !sh_.update_state.asset_name.empty()) {
                append_to(pkg_note, L"Selected package: ", sh_.update_state.asset_name);
                if (sh_.update_state.asset_size > 0) {
                    append_to(pkg_note, L" (", format_size(sh_.update_state.asset_size), L")");
                }
                pkg_note += tar_supports_7z()
                    ? L"  •  Windows tar supports .7z"
                    : L"  •  Using .zip for Windows compatibility";
            }

            std::wstring job_note = upd_view.log;
            if (update_job_.busy && upd_view.total > 0) {
                append_to(job_note, L"  ", format_size(upd_view.done), L" / ", format_size(upd_view.total));
            }
            const bool show_bar = update_job_.busy && upd_view.total > 0;
            std::wstring notes_txt;
            if (update_avail && !sh_.update_state.release_notes.empty()) {
                notes_txt = to_wide(sh_.update_state.release_notes);
                if (notes_txt.size() > 600) {
                    notes_txt.resize(600);
                    notes_txt += L'…';
                }
            }

            const float auto_y = y + 16.0f + hd.h + 12.0f;
            const float line2_y = auto_y + 38.0f;
            const float actions_y = line2_y + 12.0f;
            const float actions_h = actions.height(rw);
            float cy = actions_y + actions_h;

            const float pkg_y = cy + 8.0f;
            const float pkg_h = pkg_note.empty() ? 0.0f : text_height(cv, pkg_note, Font::Caption, rw, 18.0f);
            if (pkg_h > 0.0f) cy = pkg_y + pkg_h;

            const float bar_y = cy + 8.0f;
            if (show_bar) cy = bar_y + 6.0f;

            const float job_y = cy + 8.0f;
            const float job_h = job_note.empty() ? 0.0f : text_height(cv, job_note, Font::Caption, rw, 18.0f);
            if (job_h > 0.0f) cy = job_y + job_h;

            const float notes_y = cy + 12.0f;
            const float notes_inner_w = std::max(120.0f, rw - 24.0f);
            const float notes_txt_h = notes_txt.empty() ? 0.0f : text_height(cv, notes_txt, Font::Small, notes_inner_w, 18.0f);
            const float notes_h = notes_txt.empty() ? 0.0f : (20.0f + notes_txt_h);
            if (notes_h > 0.0f) cy = notes_y + notes_h;

            const float uh = cy + 18.0f - y;
            const float u_next_y = card_begin(cv, x, y, w, uh, CARD_GAP);
            (void)draw_header(cv, lx, y, rw, hd);

            cv.text(L"Check automatically (every 24h)", rect(lx, auto_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
            draw_switch(cv, hover_.alpha(IdToggleAutoUpdate), sw_auto_update_.value, lx + rw, auto_y + 16.0f, IdToggleAutoUpdate);

            cv.hline(lx, lx + rw, line2_y, with_alpha(cv.theme().outline_variant, 0.5f));
            actions.draw(lx, actions_y, rw);
            if (pkg_h > 0.0f) {
                cv.text_wrap(pkg_note, rect(lx, pkg_y, rw, pkg_h), Font::Caption, cv.theme().on_surface_variant);
            }
            if (show_bar) {
                const float pct = std::clamp(static_cast<float>(upd_view.done) / static_cast<float>(upd_view.total), 0.0f, 1.0f);
                const D2D1_RECT_F trk = rect(lx, bar_y, rw, 6.0f);
                cv.fill_round(trk, 3.0f, with_alpha(cv.theme().outline_variant, 0.35f));
                if (pct > 0.0f) {
                    cv.fill_round(rect(lx, bar_y, std::max(6.0f, rw * pct), 6.0f), 3.0f, cv.theme().primary);
                }
            }
            if (job_h > 0.0f) {
                cv.text_wrap(job_note, rect(lx, job_y, rw, job_h), Font::Caption,
                             upd_view.failed ? cv.theme().error : cv.theme().on_surface_variant);
            }
            if (notes_h > 0.0f) {
                const D2D1_RECT_F nr = rect(lx, notes_y, rw, notes_h);
                draw_well(cv, nr, 10.0f, true);
                cv.text_wrap(notes_txt, rect(nr.left + 12.0f, nr.top + 10.0f, notes_inner_w, notes_txt_h),
                             Font::Small, cv.theme().on_surface_variant);
            }
            y = u_next_y;
        }
        return y;
    }

    // ------------------------------------------------------------------------------------------
    // Auto-Update Check & Staged Install (identical workflow to usyseek)
    // ------------------------------------------------------------------------------------------

    void start_update_check(bool manual) {
        if (sh_.update_checking || update_job_.busy) return;
        if (!manual && !should_auto_check_updates(sh_.settings.auto_check_updates, sh_.update_state.last_check_unix, unix_now())) {
            return;
        }
        if (update_check_thread_.joinable()) update_check_thread_.join();
        sh_.update_checking = true;
        ++sh_.background_jobs;

        UpdateState st = sh_.update_state;
        st.last_check_unix = unix_now();
        (void)save_update_state(st);
        sh_.update_state.last_check_unix = st.last_check_unix;
        invalidate();

        update_check_thread_ = std::jthread([this, st = std::move(st)](const std::stop_token& stop) mutable {
            bool is_newer = false;
            const bool ok = check_latest_release(st, is_newer, stop);
            if (stop.stop_requested()) return;
            sh_.call([this, st = std::move(st), ok, is_newer] mutable {
                if (update_check_thread_.joinable()) update_check_thread_.join();
                sh_.update_checking = false;
                if (sh_.background_jobs > 0) --sh_.background_jobs;
                if (ok && is_newer) {
                    const std::string ver_label = st.latest_tag.empty() ? ulp::str::concat_strings("v", st.latest_version) : st.latest_tag;
                    if (st.notified_version != st.latest_version) {
                        st.notified_version = st.latest_version;
                        (void)save_update_state(st);
                        if (sh_.on_update_balloon) {
                            sh_.on_update_balloon(to_wide(ver_label));
                        }
                    }
                }
                sh_.update_state = std::move(st);
                if (sh_.on_visibility_changed) sh_.on_visibility_changed();
                if (sh_.on_redraw_all) sh_.on_redraw_all();
                invalidate();
            });
        });
    }

    void start_update_install() {
        if (update_job_.busy) {
            update_job_.thread.request_stop();
            return;
        }
        if (sh_.recording_active || !sh_.has_update_available() || sh_.update_state.asset_url.empty()) return;

        if (update_job_.thread.joinable()) update_job_.thread.join();
        {
            const std::scoped_lock lk{update_job_.mx};
            update_job_.log.clear();
            update_job_.failed = false;
            update_job_.done = 0;
            update_job_.total = 0;
        }
        update_job_.busy = true;
        ++sh_.background_jobs;
        invalidate();

        UpdateState st = sh_.update_state;
        const auto staged_root = std::make_shared<fs::path>();
        const auto stage_ok = std::make_shared<bool>(false);

        update_job_.thread = std::jthread([this, st = std::move(st), staged_root, stage_ok](const std::stop_token& stop) {
            try {
                std::wstring err;
                *stage_ok = download_and_stage_update(
                    st, *staged_root, err,
                    [this](std::wstring_view msg) { report_update(msg); },
                    [this](uint64_t done, uint64_t total) { report_update_progress(done, total); },
                    stop);
                report_update_progress(0, 0);
                if (!*stage_ok) {
                    report_update(err, !stop.stop_requested());
                }
            } catch (const std::exception& e) {
                *stage_ok = false;
                report_update_progress(0, 0);
                report_update(to_wide(e.what()), true);
            }

            sh_.call([this, staged_root, stage_ok] {
                if (update_job_.thread.joinable()) update_job_.thread.join();
                update_job_.busy = false;
                if (sh_.background_jobs > 0) --sh_.background_jobs;
                if (sh_.on_visibility_changed) sh_.on_visibility_changed();
                if (!*stage_ok) {
                    invalidate();
                    return;
                }
                const DWORD wait_pid = GetCurrentProcessId();
                bool uac_cancelled = false;
                std::wstring err;
                if (!launch_staged_updater(*staged_root, exe_dir(), wait_pid, true, uac_cancelled, err)) {
                    report_update(err, true);
                    return;
                }
                PostQuitMessage(0);
            });
        });
    }

    // ------------------------------------------------------------------------------------------
    // ShareX History Import (SQLite History.db & Legacy JSON)
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] std::wstring sharex_browse_initial_dir() const {
        std::error_code ec;
        if (!sharex_path_w_.empty()) {
            const fs::path parent = fs::path{sharex_path_w_}.parent_path();
            if (!parent.empty() && fs::is_directory(parent, ec) && !ec) return parent.wstring();
        }
        std::wstring docs = known_folder(FOLDERID_Documents);
        if (!docs.empty()) {
            const fs::path sx_dir = fs::path{docs} / L"ShareX";
            if (fs::is_directory(sx_dir, ec) && !ec) return sx_dir.wstring();
            return docs;
        }
        return {};
    }

    bool browse_sharex_history_file() {
        static constexpr std::array<COMDLG_FILTERSPEC, 4> kFilters = {{
            {.pszName = L"ShareX History (*.db; *.json)", .pszSpec = L"*.db;*.sqlite;*.sqlite3;*.json"},
            {.pszName = L"ShareX SQLite Database (*.db)", .pszSpec = L"*.db;*.sqlite;*.sqlite3"},
            {.pszName = L"ShareX Legacy JSON (*.json)", .pszSpec = L"*.json"},
            {.pszName = L"All Files (*.*)", .pszSpec = L"*.*"},
        },};
        if (auto picked = browse_file_dialog(hwnd_, L"Select ShareX History.db or History.json", sharex_browse_initial_dir(), kFilters)) {
            sharex_path_w_ = std::move(*picked);
            {
                const std::scoped_lock lk{sharex_job_.mx};
                sharex_job_.log.clear();
                sharex_job_.failed = false;
                sharex_job_.done = 0;
                sharex_job_.total = 0;
            }
            invalidate();
            return true;
        }
        return false;
    }

    void start_sharex_import() {
        if (sharex_job_.busy) {
            sharex_job_.thread.request_stop();
            return;
        }
        if (sharex_path_w_.empty() && !browse_sharex_history_file()) {
            return;
        }
        if (sharex_job_.thread.joinable()) sharex_job_.thread.join();

        const fs::path src_path{sharex_path_w_};
        const std::wstring filename = src_path.filename().wstring();
        const ShareXImportOptions opts{.only_existing_files = sharex_only_existing_};
        {
            const std::scoped_lock lk{sharex_job_.mx};
            sharex_job_.log = ulp::str::concat_strings(L"Importing ", filename, L"…");
            sharex_job_.failed = false;
            sharex_job_.done = 0;
            sharex_job_.total = 0;
        }
        sharex_job_.busy = true;
        ++sh_.background_jobs;
        invalidate();

        sharex_job_.thread = std::jthread([this, src_path, filename, opts](const std::stop_token& stop) { // NOLINT(bugprone-exception-escape)
            ShareXImportResult res{};
            std::string err;
            bool ok = false;
            try {
                ok = import_sharex_history(
                    src_path, sh_.history, opts, res, &err,
                    [this, &filename](uint64_t done, uint64_t total) {
                        std::wstring status = ulp::str::concat_strings(L"Importing ", filename, L"…");
                        if (total > 0) {
                            append_to(status, L" (", format_count(done).view(), L" / ", format_count(total).view(), L")");
                        }
                        report_sharex_progress(done, total, status);
                    },
                    stop);
            } catch (const std::exception& e) {
                ok = false;
                err = e.what();
            } catch (...) {
                ok = false;
                err = "Unexpected error during ShareX history import.";
            }

            sh_.call([this, ok, res, filename, err = std::move(err)] { // NOLINT(bugprone-exception-escape)
                if (sharex_job_.thread.joinable()) sharex_job_.thread.join();
                sharex_job_.busy = false;
                if (sh_.background_jobs > 0) --sh_.background_jobs;
                {
                    const std::scoped_lock lk{sharex_job_.mx};
                    sharex_job_.done = 0;
                    sharex_job_.total = 0;
                    if (ok) {
                        sharex_job_.log = format_sharex_import_summary(res, filename);
                        sharex_job_.failed = false;
                    } else {
                        sharex_job_.log = err.empty() ? L"ShareX history import failed." : to_wide(err);
                        sharex_job_.failed = true;
                    }
                }
                if (auto st = sh_.history.stats()) stats_ = *st;
                if (ok && res.imported > 0 && sh_.on_history_changed) {
                    sh_.on_history_changed();
                }
                if (sh_.on_visibility_changed) sh_.on_visibility_changed();
                invalidate();
            });
        });
    }

    // ------------------------------------------------------------------------------------------
    // Input
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] D2D1_POINT_2F to_dip(LPARAM lp) const noexcept {
        const float s = target_.scale();
        return {.x = GET_X_LPARAM(lp) / s, .y = GET_Y_LPARAM(lp) / s};
    }

    void update_slider_from_x(int slider_id, float x) {
        D2D1_RECT_F r{};
        switch (slider_id) {
            case IdSliderToastSize: r = slider_size_r_; break;
            case IdSliderToastTimeout: r = slider_timeout_r_; break;
            case IdSliderImageQuality: r = slider_quality_r_; break;
            case IdSliderRecFps: r = slider_rec_fps_r_; break;
            case IdSliderRecScale: r = slider_rec_scale_r_; break;
            case IdSliderRecBitrate: r = slider_rec_bitrate_r_; break;
            case IdSliderRecQuality: r = slider_rec_quality_r_; break;
            default: return;
        }
        const float l = r.left + 8.0f;
        const float ri = std::max(l + 4.0f, r.right - 8.0f);
        const float k = std::clamp((x - l) / (ri - l), 0.0f, 1.0f);
        switch (slider_id) {
            case IdSliderToastSize:
                sh_.settings.toast_width_dip = std::round((160.0f + (k * 260.0f)) / 5.0f) * 5.0f;
                break;
            case IdSliderImageQuality:
                sh_.settings.image_quality = std::clamp(static_cast<int>(std::lround((10.0f + (k * 90.0f)) / 5.0f)) * 5, 10, 100);
                break;
            case IdSliderToastTimeout:
                sh_.settings.toast_timeout_s = std::round((1.0f + (k * 14.0f)) * 2.0f) / 2.0f;
                break;
            case IdSliderRecFps: {
                const bool is_gif = (sh_.settings.resolved_record_format() == RecordFormat::Gif);
                const auto hi = static_cast<float>(is_gif ? RECORD_GIF_FPS_MAX : RECORD_FPS_MAX);
                const auto lo = static_cast<float>(RECORD_FPS_MIN);
                sh_.settings.record_fps = clamp_record_fps(static_cast<uint32_t>(std::lround(lo + (k * (hi - lo)))));
                break;
            }
            case IdSliderRecScale: {
                const auto lo = static_cast<float>(RECORD_SCALE_MIN_PCT);
                const auto span = static_cast<float>(RECORD_SCALE_MAX_PCT - RECORD_SCALE_MIN_PCT);
                const auto stepped = static_cast<uint32_t>(std::lround((lo + (k * span)) / 5.0f)) * 5u;
                sh_.settings.record_scale_pct = clamp_record_scale_pct(stepped);
                break;
            }
            case IdSliderRecBitrate: {
                const auto lo = static_cast<float>(RECORD_BITRATE_MIN_MBPS);
                const auto span = static_cast<float>(RECORD_BITRATE_MAX_MBPS - RECORD_BITRATE_MIN_MBPS);
                sh_.settings.record_bitrate_mbps = clamp_record_bitrate_mbps(static_cast<uint32_t>(std::lround(lo + (k * span))));
                break;
            }
            case IdSliderRecQuality: {
                const auto lo = static_cast<float>(RECORD_QUALITY_MIN);
                const auto span = static_cast<float>(RECORD_QUALITY_MAX - RECORD_QUALITY_MIN);
                const auto stepped = static_cast<uint32_t>(std::lround((lo + (k * span)) / 5.0f)) * 5u;
                sh_.settings.record_quality = clamp_record_quality(stepped);
                break;
            }
            default:
                return;
        }
        sh_.save();
        if (r.right > r.left && r.bottom > r.top) {
            invalidate_rect(D2D1::RectF(0.0f, r.top - 8.0f, target_.size().width, r.bottom + 8.0f));
        } else {
            invalidate();
        }
    }

    void on_lbutton_down(LPARAM lp) {
        const D2D1_POINT_2F p = to_dip(lp);
        const Hit* h = hit_test(hits_, p);
        if (!h) {
            open_audio_drop_ = AudioDropdown::None;
            stop_recording_hotkey();
            invalidate();
            return;
        }
        if (open_audio_drop_ != AudioDropdown::None &&
            h->id != IdRecAudioDropItem && h->id != IdDropRecDesktop && h->id != IdDropRecMic) {
            open_audio_drop_ = AudioDropdown::None;
        }
        if (recording_action_ && h->id != IdRecordHotkey) stop_recording_hotkey();

        switch (h->id) {
            case IdTab0: case IdTab1: case IdTab2:
                open_audio_drop_ = AudioDropdown::None;
                tab_ = h->id - IdTab0;
                tab_anim_.set(static_cast<float>(tab_), true);
                scroll_ = target_scroll_ = 0.0f;
                if (tab_ == 1) {
                    desktop_devices_ = enumerate_audio_devices(AudioFlow::Render);
                    mic_devices_ = enumerate_audio_devices(AudioFlow::Capture);
                } else if (tab_ == 2) {
                    if (auto st = sh_.history.stats()) stats_ = *st;
                }
                full_dirty_ = true;
                pending_dirty_.clear();
                start_anim();
                return;
            case IdToggleHotkey: {
                const auto a = static_cast<HotkeyAction>(h->index);
                sh_.settings.hotkey(a).enabled = !sh_.settings.hotkey(a).enabled;
                sh_.save();
                if (sh_.on_reload_hotkeys) sh_.on_reload_hotkeys();
                sync_state(true);
                full_dirty_ = true;
                pending_dirty_.clear();
                start_anim();
                return;
            }
            case IdRecordHotkey: {
                const auto a = static_cast<HotkeyAction>(h->index);
                if (recording_action_ && *recording_action_ == a) {
                    stop_recording_hotkey();
                } else {
                    start_recording_hotkey(a);
                }
                invalidate();
                return;
            }
            case IdResetHotkey: {
                const auto a = static_cast<HotkeyAction>(h->index);
                sh_.settings.hotkey(a) = default_hotkey(a);
                sh_.save();
                if (sh_.on_reload_hotkeys) sh_.on_reload_hotkeys();
                sync_state(true);
                full_dirty_ = true;
                pending_dirty_.clear();
                start_anim();
                return;
            }
            case IdToggleWinPrtSc: {
                const bool want_disable = !win_prtscn_disabled_;
                if (set_windows_printscreen_snipping_disabled(want_disable)) {
                    win_prtscn_err_.clear();
                    win_prtscn_disabled_ = is_windows_printscreen_snipping_disabled();
                    if (sh_.on_reload_hotkeys) sh_.on_reload_hotkeys();
                } else {
                    win_prtscn_err_ = ulp::str::concat_strings(
                        L"Could not update Windows Print Screen registry setting (",
                        to_wide(win32_error_utf8(GetLastError())), L").");
                }
                sync_state(true);
                full_dirty_ = true;
                pending_dirty_.clear();
                start_anim();
                return;
            }
            case IdOpenWinKeyboardSettings:
                (void)open_file(L"ms-settings:easeofaccess-keyboard");
                return;
            case IdToggleStartup:
                if (set_startup_enabled(!startup_enabled_)) {
                    startup_enabled_ = is_startup_enabled();
                }
                sync_state(true);
                start_anim();
                return;
            case IdToggleAutoCopy:
                sh_.settings.auto_copy_clipboard = !sh_.settings.auto_copy_clipboard;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdToggleCaptureCursor:
                sh_.settings.capture_cursor = !sh_.settings.capture_cursor;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdToggleInstantRegion:
                sh_.settings.region_instant_capture = !sh_.settings.region_instant_capture;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdToggleSplitMonth:
                sh_.settings.split_by_month = !sh_.settings.split_by_month;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdFmtPng: sh_.settings.image_format = "png"; sh_.save(); invalidate(); return;
            case IdFmtJpeg: sh_.settings.image_format = "jpeg"; sh_.save(); invalidate(); return;
            case IdFmtBmp: sh_.settings.image_format = "bmp"; sh_.save(); invalidate(); return;
            case IdFmtTiff: sh_.settings.image_format = "tiff"; sh_.save(); invalidate(); return;
            case IdBrowseSaveDir:
                if (auto dir = browse_folder_dialog(hwnd_, L"Choose capture save folder", resolve_save_dir(sh_.settings).wstring())) {
                    sh_.settings.save_dir = to_utf8(*dir);
                    save_dir_w_ = *dir;
                    sh_.save();
                    invalidate();
                }
                return;
            case IdOpenSaveDir:
                (void)open_file(resolve_save_dir(sh_.settings).wstring());
                return;
            case IdBrowseShareX:
                if (!sharex_job_.busy) (void)browse_sharex_history_file();
                return;
            case IdImportShareX:
                start_sharex_import();
                return;
            case IdToggleShareXExistingOnly:
                if (!sharex_job_.busy) {
                    sharex_only_existing_ = !sharex_only_existing_;
                    sync_state(true);
                    start_anim();
                }
                return;
            case IdToggleToast:
                sh_.settings.show_corner_toast = !sh_.settings.show_corner_toast;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdSliderToastSize:
            case IdSliderToastTimeout:
            case IdSliderImageQuality:
            case IdSliderRecFps:
            case IdSliderRecScale:
            case IdSliderRecBitrate:
            case IdSliderRecQuality:
                dragging_slider_ = h->id;
                SetCapture(hwnd_);
                update_slider_from_x(h->id, p.x);
                return;
            case IdTestToast:
                if (sh_.on_test_toast) sh_.on_test_toast();
                return;
            case IdRecFmtMp4:
                sh_.settings.record_format = "mp4";
                sh_.save();
                scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
                invalidate();
                return;
            case IdRecFmtWmv:
                sh_.settings.record_format = "wmv";
                sh_.save();
                scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
                invalidate();
                return;
            case IdRecFmtGif:
                open_audio_drop_ = AudioDropdown::None;
                sh_.settings.record_format = "gif";
                sh_.save();
                scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
                invalidate();
                return;
            case IdRecCodecH264: sh_.settings.record_codec = "h264"; sh_.save(); invalidate(); return;
            case IdRecCodecHevc: sh_.settings.record_codec = "hevc"; sh_.save(); invalidate(); return;
            case IdRecProfBase: sh_.settings.record_h264_profile = "baseline"; sh_.save(); invalidate(); return;
            case IdRecProfMain: sh_.settings.record_h264_profile = "main"; sh_.save(); invalidate(); return;
            case IdRecProfHigh: sh_.settings.record_h264_profile = "high"; sh_.save(); invalidate(); return;
            case IdRecGifColors:
                sh_.settings.record_gif_colors = normalize_gif_colors(static_cast<uint32_t>(std::max(0, h->index)));
                sh_.save();
                invalidate();
                return;
            case IdToggleRecGifDither:
                sh_.settings.record_gif_dither = !sh_.settings.record_gif_dither;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdRecFpsPreset:
                sh_.settings.record_fps = clamp_record_fps(static_cast<uint32_t>(std::max(0, h->index)));
                sh_.save();
                invalidate();
                return;
            case IdRecScalePreset:
                sh_.settings.record_scale_pct = clamp_record_scale_pct(static_cast<uint32_t>(std::max(0, h->index)));
                sh_.save();
                invalidate();
                return;
            case IdRecRcVbr: sh_.settings.record_rate_control = "vbr"; sh_.save(); invalidate(); return;
            case IdRecRcQuality: sh_.settings.record_rate_control = "quality"; sh_.save(); invalidate(); return;
            case IdRecRcCbr: sh_.settings.record_rate_control = "cbr"; sh_.save(); invalidate(); return;
            case IdRecBitratePreset:
                sh_.settings.record_bitrate_mbps = clamp_record_bitrate_mbps(static_cast<uint32_t>(std::max(0, h->index)));
                sh_.save();
                invalidate();
                return;
            case IdRecKeyframe:
                sh_.settings.record_keyframe_s = clamp_record_keyframe_s(static_cast<uint32_t>(std::max(0, h->index)));
                sh_.save();
                invalidate();
                return;
            case IdToggleRecHwAccel:
                sh_.settings.record_hw_accel = !sh_.settings.record_hw_accel;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdToggleRecFragMp4:
                sh_.settings.record_fragmented_mp4 = !sh_.settings.record_fragmented_mp4;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdRecAudioBitrate:
                sh_.settings.record_audio_bitrate_kbps = normalize_audio_bitrate_kbps(static_cast<uint32_t>(std::max(0, h->index)));
                sh_.save();
                invalidate();
                return;
            case IdRecAudioRate44: sh_.settings.record_audio_sample_rate = 44100u; sh_.save(); invalidate(); return;
            case IdRecAudioRate48: sh_.settings.record_audio_sample_rate = 48000u; sh_.save(); invalidate(); return;
            case IdRecAudioMono: sh_.settings.record_audio_channels = 1u; sh_.save(); invalidate(); return;
            case IdRecAudioStereo: sh_.settings.record_audio_channels = 2u; sh_.save(); invalidate(); return;
            case IdToggleRecordCursor:
                sh_.settings.record_cursor = !sh_.settings.record_cursor;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdToggleRecDesktop:
                sh_.settings.record_desktop_audio = !sh_.settings.record_desktop_audio;
                sh_.save();
                sync_state(true);
                invalidate_rect(inset(desktop_drop_r_, -4.0f, -4.0f));
                start_anim();
                return;
            case IdDropRecDesktop:
                desktop_devices_ = enumerate_audio_devices(AudioFlow::Render);
                open_audio_drop_ = (open_audio_drop_ == AudioDropdown::Desktop) ? AudioDropdown::None : AudioDropdown::Desktop;
                invalidate();
                return;
            case IdToggleRecMic:
                sh_.settings.record_mic_audio = !sh_.settings.record_mic_audio;
                sh_.save();
                sync_state(true);
                invalidate_rect(inset(mic_drop_r_, -4.0f, -4.0f));
                start_anim();
                return;
            case IdDropRecMic:
                mic_devices_ = enumerate_audio_devices(AudioFlow::Capture);
                open_audio_drop_ = (open_audio_drop_ == AudioDropdown::Mic) ? AudioDropdown::None : AudioDropdown::Mic;
                invalidate();
                return;
            case IdRecAudioDropItem:
                if (open_audio_drop_ == AudioDropdown::Desktop) {
                    if (h->index < 0) {
                        sh_.settings.record_desktop_device_id.clear();
                    } else if (std::cmp_less(h->index, desktop_devices_.size())) {
                        sh_.settings.record_desktop_device_id = desktop_devices_[static_cast<size_t>(h->index)].id;
                    }
                    sh_.settings.record_desktop_audio = true;
                    sh_.save();
                    sync_state(true);
                    start_anim();
                } else if (open_audio_drop_ == AudioDropdown::Mic) {
                    if (h->index < 0) {
                        sh_.settings.record_mic_device_id.clear();
                    } else if (std::cmp_less(h->index, mic_devices_.size())) {
                        sh_.settings.record_mic_device_id = mic_devices_[static_cast<size_t>(h->index)].id;
                    }
                    sh_.settings.record_mic_audio = true;
                    sh_.save();
                    sync_state(true);
                    start_anim();
                }
                open_audio_drop_ = AudioDropdown::None;
                invalidate();
                return;
            case IdToggleAutoUpdate:
                sh_.settings.auto_check_updates = !sh_.settings.auto_check_updates;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdCheckUpdates:
                start_update_check(true);
                return;
            case IdInstallUpdate:
                start_update_install();
                return;
            case IdOpenReleaseUrl: {
                const std::wstring url = sh_.update_state.release_url.empty()
                    ? std::wstring{GITHUB_RELEASES_PAGE_URL}
                    : to_wide(sh_.update_state.release_url);
                (void)open_file(url);
                return;
            }
            case IdScrollbar:
                dragging_scroll_ = true;
                SetCapture(hwnd_);
                scroll_drag_offset_ = p.y - scroll_thumb_.top;
                return;
            default:
                break;
        }
    }

    void on_key_down(UINT vk, std::optional<uint32_t> explicit_mods = std::nullopt) {
        if (recording_action_) {
            if (vk == VK_ESCAPE) {
                stop_recording_hotkey();
                invalidate();
                return;
            }
            if (is_modifier_vk(vk)) return;
            uint32_t mods = 0;
            if (explicit_mods) {
                mods = *explicit_mods;
            } else {
                if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
                if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
                if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
                if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
            }
            mods = resolve_recorded_hotkey_mods(mods, vk);

            // Internal collision check BEFORE applying!
            const auto all = sh_.settings.all_hotkeys();
            if (const auto clash = check_candidate_hotkey_conflict(all, static_cast<size_t>(*recording_action_), mods, vk)) {
                record_conflict_msg_ = ulp::str::concat_strings(
                    format_hotkey(mods, vk), L" is already assigned to '", hotkey_action_label(*clash), L"'. Press a different shortcut.");
                invalidate();
                return;
            }

            const HotkeyAction act = *recording_action_;
            sh_.settings.hotkey(act) = {.enabled = true, .mods = mods, .vk = vk};
            sh_.save();
            stop_recording_hotkey(false);
            if (sh_.on_reload_hotkeys) sh_.on_reload_hotkeys();
            sync_state(true);
            invalidate();
            return;
        }
        if (vk == VK_ESCAPE) {
            if (open_audio_drop_ != AudioDropdown::None) {
                open_audio_drop_ = AudioDropdown::None;
                invalidate();
                return;
            }
            hide();
        }
    }

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->handle(hwnd, msg, wp, lp);
    }

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_CLOSE: hide(); return 0;
            case WM_ENTERSIZEMOVE:
                in_sizemove_ = true;
                return 0;
            case WM_EXITSIZEMOVE:
                in_sizemove_ = false;
                invalidate();
                return 0;
            case WM_SIZE:
                minimized_ = (wp == SIZE_MINIMIZED);
                if (!minimized_ && visible()) {
                    (void)target_.resize(LOWORD(lp), HIWORD(lp));
                    scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
                    full_dirty_ = true;
                    pending_dirty_.clear();
                    render();
                }
                return 0;
            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                icons_.apply(hwnd, inst_, 101);
                (void)target_.sync_dpi();
                invalidate();
                return 0;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                render();
                EndPaint(hwnd, &ps);
                if (window_can_render_live(visible(), minimized_) && anim_active_) {
                    MSG m;
                    while (PeekMessageW(&m, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) {
                        DispatchMessageW(&m);
                    }
                    if (window_can_render_live(visible(), minimized_) && anim_active_) InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_MOUSEWHEEL: {
                dismiss_tooltip();
                open_audio_drop_ = AudioDropdown::None;
                const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f;
                target_scroll_ = std::clamp(target_scroll_ - (delta * 90.0f), 0.0f, max_scroll());
                start_anim();
                return 0;
            }
            case WM_LBUTTONDOWN: dismiss_tooltip(); on_lbutton_down(lp); return 0;
            case WM_LBUTTONUP:
                dragging_scroll_ = false;
                dragging_slider_ = IdNone;
                if (GetCapture() == hwnd) ReleaseCapture();
                invalidate();
                return 0;
            case WM_MOUSEMOVE: {
                const D2D1_POINT_2F p = to_dip(lp);
                if (dragging_slider_ != IdNone) {
                    update_slider_from_x(dragging_slider_, p.x);
                    return 0;
                }
                if (dragging_scroll_) {
                    const float track_h = std::max(1.0f, height(view_rect_) - 8.0f - height(scroll_thumb_));
                    const float rel = std::clamp((p.y - scroll_drag_offset_ - (view_rect_.top + 4.0f)) / track_h, 0.0f, 1.0f);
                    scroll_ = target_scroll_ = rel * max_scroll();
                    invalidate();
                    return 0;
                }
                const Hit* h = hit_test(hits_, p);
                const int nid = h ? h->id : IdNone;
                const int nidx = h ? h->index : 0;
                if (nid != raw_hover_id_ || nidx != raw_hover_index_) {
                    raw_hover_id_ = nid;
                    raw_hover_index_ = nidx;
                    const int vis_id = (nid == IdSaveDirWell || nid == IdShareXPathWell || nid == IdInstallUpdateDisabled) ? IdNone : nid;
                    const int vis_idx = (vis_id == IdNone) ? 0 : nidx;
                    const bool vis_changed = hover_.set(vis_id, vis_idx, true);
                    tooltip_.on_hover_changed(hwnd_, TIMER_TOOLTIP, h, has_tooltip(nid, nidx), true);
                    if (vis_changed || tooltip_.alpha.active()) start_anim();
                }
                SetCursor((h && h->id != IdSaveDirWell && h->id != IdShareXPathWell && h->id != IdInstallUpdateDisabled) ? cursor_hand_ : cursor_arrow_);
                TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE: {
                raw_hover_id_ = IdNone;
                raw_hover_index_ = 0;
                tooltip_.clear(hwnd_, TIMER_TOOLTIP, true);
                if (hover_.set(IdNone, 0, true) || tooltip_.alpha.active()) start_anim();
                return 0;
            }
            case WM_TIMER:
                if (wp == TIMER_TOOLTIP) {
                    if (!visible() || !window_active_ || open_audio_drop_ != AudioDropdown::None) {
                        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
                        return 0;
                    }
                    std::wstring tip_title;
                    std::wstring tip_sub;
                    const bool ok = build_tooltip(raw_hover_id_, raw_hover_index_, tip_title, tip_sub);
                    if (tooltip_.on_timer(hwnd_, TIMER_TOOLTIP, ok, std::move(tip_title), std::move(tip_sub), true)) {
                        start_anim();
                    }
                }
                return 0;
            case WM_ACTIVATE:
                window_active_ = (LOWORD(wp) != WA_INACTIVE);
                if (!window_active_) {
                    dismiss_tooltip();
                    if (recording_action_ || record_hook_) {
                        stop_recording_hotkey();
                        invalidate();
                    }
                } else {
                    refresh_external_state();
                }
                break;
            case WM_DESTROY:
                KillTimer(hwnd, TIMER_TOOLTIP);
                return 0;
            case WM_APP_RECORDED_KEY:
                if (recording_action_) {
                    on_key_down(static_cast<UINT>(wp), static_cast<uint32_t>(lp));
                }
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                dismiss_tooltip();
                if (msg == WM_SYSKEYDOWN && wp == VK_F4 && !recording_action_) break;
                on_key_down(static_cast<UINT>(wp));
                return 0;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                if (recording_action_ && wp == VK_SNAPSHOT) {
                    on_key_down(VK_SNAPSHOT);
                    return 0;
                }
                break;
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) return TRUE;
                break;
            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    Shared& sh_;
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    WindowIcons icons_;
    WindowTarget target_;

    int tab_ = 0;
    AnimFloat tab_anim_;
    std::array<AnimFloat, HOTKEY_COUNT> sw_hotkeys_{};
    AnimFloat sw_win_prtscn_;
    AnimFloat sw_startup_;
    AnimFloat sw_auto_copy_;
    AnimFloat sw_cap_cursor_;
    AnimFloat sw_instant_region_;
    AnimFloat sw_split_month_;
    AnimFloat sw_sharex_existing_only_;
    AnimFloat sw_toast_;
    AnimFloat sw_rec_cursor_;
    AnimFloat sw_rec_hw_accel_;
    AnimFloat sw_rec_frag_mp4_;
    AnimFloat sw_rec_gif_dither_;
    AnimFloat sw_rec_desktop_;
    AnimFloat sw_rec_mic_;
    AnimFloat sw_auto_update_;

    AudioDropdown open_audio_drop_ = AudioDropdown::None;
    std::vector<AudioDeviceInfo> desktop_devices_;
    std::vector<AudioDeviceInfo> mic_devices_;
    D2D1_RECT_F desktop_drop_r_{};
    D2D1_RECT_F mic_drop_r_{};
    bool hevc_probed_ = false;
    bool hevc_available_ = false;

    bool startup_enabled_ = false;
    bool win_prtscn_disabled_ = false;
    std::wstring win_prtscn_err_;
    std::wstring save_dir_w_;
    HistoryStats stats_{};
    std::optional<HotkeyAction> recording_action_;
    std::wstring record_conflict_msg_;
    HHOOK record_hook_ = nullptr;
    bool pending_snapshot_down_ = false;
    uint32_t pending_snapshot_mods_ = 0;
    static inline SettingsWindow* record_hook_target_ = nullptr;
    UpdateJob update_job_;
    std::jthread update_check_thread_;
    UpdateJob sharex_job_;
    std::wstring sharex_path_w_;
    bool sharex_path_probed_ = false;
    bool sharex_only_existing_ = false;

    wil::com_ptr<ID2D1Bitmap1> app_icon_;
    uint64_t app_icon_epoch_ = 0;

    std::vector<Hit> hits_;
    std::vector<Hit> scratch_hits_;
    HoverTracker hover_;
    int raw_hover_id_ = IdNone;
    int raw_hover_index_ = 0;
    bool window_active_ = true;
    HoverTooltip tooltip_;
    std::optional<D2D1_RECT_F> last_tooltip_dirty_;
    float scroll_ = 0.0f;
    float target_scroll_ = 0.0f;
    float content_h_ = 0.0f;
    D2D1_RECT_F view_rect_{};
    D2D1_RECT_F scroll_thumb_{};
    D2D1_RECT_F slider_size_r_{};
    D2D1_RECT_F slider_timeout_r_{};
    D2D1_RECT_F slider_quality_r_{};
    D2D1_RECT_F slider_rec_fps_r_{};
    D2D1_RECT_F slider_rec_scale_r_{};
    D2D1_RECT_F slider_rec_bitrate_r_{};
    D2D1_RECT_F slider_rec_quality_r_{};
    bool dragging_scroll_ = false;
    bool in_sizemove_ = false;
    bool minimized_ = false;
    float scroll_drag_offset_ = 0.0f;
    int dragging_slider_ = IdNone;
    bool anim_active_ = false;
    bool rendering_ = false;
    bool full_dirty_ = true;
    std::vector<D2D1_RECT_F> pending_dirty_;
    std::vector<D2D1_RECT_F> dirty_dips_scratch_;
    std::chrono::steady_clock::time_point last_anim_{};
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
};

} // namespace capusy::ui
