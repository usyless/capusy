#pragma once

#include "canvas.hpp"
#include "shared.hpp"
#include "widgets.hpp"

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
        update_job_.thread = {};
        update_check_thread_ = {};
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
        if (!target_.ready() && !target_.attach(sh_.gfx, hwnd_)) return;

        sync_state(false);
        apply_dark_window(hwnd_, DWMSBT_MAINWINDOW, true);
        cloak_window(hwnd_, false);
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        else ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        anim_active_ = false;
        render();
        ValidateRect(hwnd_, nullptr);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void hide() {
        if (!hwnd_) return;
        stop_recording_hotkey();
        dragging_scroll_ = false;
        in_sizemove_ = false;
        dragging_slider_ = IdNone;
        anim_active_ = false;
        hover_.set(IdNone, 0, false);
        app_icon_.reset();
        target_.release();
        cloak_window(hwnd_, true);
        ShowWindow(hwnd_, SW_HIDE);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void reset_gpu() noexcept {
        app_icon_.reset();
        target_.release();
    }

    void invalidate() noexcept {
        if (visible()) InvalidateRect(hwnd_, nullptr, FALSE);
    }

private:
    static constexpr float TABS_H = 52.0f;
    static constexpr float PAD = 24.0f;
    static constexpr float CARD_GAP = 14.0f;

    enum HitId : int {
        IdNone = 0,
        IdTab0,
        IdTab1,
        IdTab2,
        IdToggleHotkey,
        IdRecordHotkey,
        IdResetHotkey,
        IdToggleStartup,
        IdToggleAutoCopy,
        IdToggleCaptureCursor,
        IdToggleSplitMonth,
        IdFmtPng,
        IdFmtJpeg,
        IdFmtBmp,
        IdFmtTiff,
        IdSliderImageQuality,
        IdBrowseSaveDir,
        IdOpenSaveDir,
        IdToggleToast,
        IdSliderToastSize,
        IdSliderToastTimeout,
        IdTestToast,
        IdFps30,
        IdFps60,
        IdBitrate6,
        IdBitrate12,
        IdBitrate20,
        IdToggleRecordCursor,
        IdToggleAutoUpdate,
        IdCheckUpdates,
        IdInstallUpdate,
        IdInstallUpdateDisabled,
        IdOpenReleaseUrl,
        IdScrollbar,
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
        save_dir_w_ = resolve_save_dir(sh_.settings).wstring();
        if (auto st = sh_.history.stats()) stats_ = *st;
        for (size_t i = 0; i < HOTKEY_COUNT; ++i) {
            sw_hotkeys_[i].set(sh_.settings.hotkey(static_cast<HotkeyAction>(i)).enabled ? 1.0f : 0.0f, animate);
        }
        sw_startup_.set(startup_enabled_ ? 1.0f : 0.0f, animate);
        sw_auto_copy_.set(sh_.settings.auto_copy_clipboard ? 1.0f : 0.0f, animate);
        sw_cap_cursor_.set(sh_.settings.capture_cursor ? 1.0f : 0.0f, animate);
        sw_split_month_.set(sh_.settings.split_by_month ? 1.0f : 0.0f, animate);
        sw_toast_.set(sh_.settings.show_corner_toast ? 1.0f : 0.0f, animate);
        sw_rec_cursor_.set(sh_.settings.record_cursor ? 1.0f : 0.0f, animate);
        sw_auto_update_.set(sh_.settings.auto_check_updates ? 1.0f : 0.0f, animate);
    }

    void start_anim() noexcept {
        if (!visible()) return;
        if (!anim_active_) last_anim_ = std::chrono::steady_clock::now();
        anim_active_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool step_anim() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_anim_).count(), 0.001f, 0.05f);
        last_anim_ = now;

        bool moving = hover_.step(dt);
        if (tab_anim_.step(dt, 20.0f, 0.004f)) moving = true;
        for (auto& sw : sw_hotkeys_) { if (sw.step(dt, 22.0f, 0.004f)) moving = true; }
        if (sw_startup_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_auto_copy_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_cap_cursor_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_split_month_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_toast_.step(dt, 22.0f, 0.004f)) moving = true;
        if (sw_rec_cursor_.step(dt, 22.0f, 0.004f)) moving = true;
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

    void stop_recording_hotkey() noexcept {
        if (!recording_action_) return;
        recording_action_.reset();
        record_conflict_msg_.clear();
        if (sh_.on_suspend_hotkeys) sh_.on_suspend_hotkeys(false);
    }

    [[nodiscard]] float max_scroll() const noexcept {
        const float view_h = std::max(0.0f, target_.size().height - TABS_H - 1.0f);
        return std::max(0.0f, content_h_ - view_h);
    }

    void render() {
        if (!visible() || rendering_) return;
        if (!target_.ready()) {
            if (!sh_.gfx.ensure_device(hwnd_) || !target_.rebuild()) return;
        }
        rendering_ = true;
        const auto guard = wil::scope_exit([&] { rendering_ = false; });

        const bool anim = step_anim();
        const auto mode = in_sizemove_ ? WindowTarget::PresentMode::Immediate : WindowTarget::PresentMode::Vsync;
        const bool ok = target_.render_frame(true, {}, mode, [&](std::optional<D2D1_RECT_F> clip, bool) {
            hits_.clear();
            Canvas cv{target_, sh_.fonts, hits_, default_theme(), clip};
            draw(cv);
        });
        anim_active_ = visible() && anim;
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

        if (tab_ == 0) y = draw_tab_general(cv, cx, y, cw);
        else if (tab_ == 1) y = draw_tab_toast_recording(cv, cx, y, cw);
        else y = draw_tab_about(cv, cx, y, cw);

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
    }

    void draw_tabs(Canvas& cv, D2D1_SIZE_F sz) {
        cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, TABS_H), with_alpha(cv.theme().surface_lowest, 0.78f));

        struct TabDef { wchar_t g; const wchar_t* label; int id; };
        static constexpr std::array<TabDef, 3> tabs = {{
            {.g = glyph::keyboard, .label = L"General & Hotkeys", .id = IdTab0},
            {.g = glyph::video, .label = L"Toast & Recording", .id = IdTab1},
            {.g = glyph::info, .label = L"About", .id = IdTab2},
        }};

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
            Flow keys{8.0f, 8.0f};
            keys.add(210.0f, 36.0f, [&](D2D1_RECT_F kr) {
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
            if (is_rec && !record_conflict_msg_.empty()) {
                warn_msg = record_conflict_msg_;
            } else if (!is_rec && hb.enabled) {
                if (st.state == HotkeyState::ConflictInternal && st.conflict_with) {
                    warn_msg = ulp::str::concat_strings(
                        format_hotkey(hb.mods, hb.vk), L" conflicts with '", hotkey_action_label(*st.conflict_with), L"'. Pick a unique shortcut.");
                } else if (st.state == HotkeyState::Unavailable) {
                    warn_msg = ulp::str::concat_strings(
                        format_hotkey(hb.mods, hb.vk), L" is already registered by another app. Pick a different shortcut.");
                }
            }
            const float warn_h = warn_msg.empty() ? 0.0f : error_row_height(cv, warn_msg, rw);

            const float keys_y = y + 16.0f + hd.h + 10.0f;
            const float warn_y = keys_y + keys.height(rw) + 8.0f;
            const float card_h = (warn_msg.empty() ? (keys_y + keys.height(rw)) : (warn_y + warn_h)) + 16.0f - y;
            const float next_y = card_begin(cv, x, y, w, card_h, 10.0f);

            const D2D1_RECT_F side = draw_header(cv, lx, y, rw, hd);
            draw_switch(cv, hover_.alpha(IdToggleHotkey, static_cast<int>(i)), sw_hotkeys_[i].value, side.right, side.top + 12.0f, IdToggleHotkey, static_cast<int>(i));
            keys.draw(lx, keys_y, rw);
            if (!warn_msg.empty()) draw_error_row(cv, warn_msg, lx, warn_y, rw, warn_h);
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
            const float line_y = r3_y + 40.0f;
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

            cv.hline(lx, lx + rw, line_y, with_alpha(cv.theme().outline_variant, 0.5f));
            cv.text(L"Capture save folder", rect(lx, dir_lbl_y, rw, 20.0f), Font::SmallSemi, cv.theme().on_surface_variant);

            const float well_w = std::max(120.0f, rw - browse_w - open_w - 16.0f);
            const D2D1_RECT_F well = rect(lx, dir_row_y, well_w, 34.0f);
            draw_well(cv, well, 9.0f, true);
            cv.text(save_dir_w, inset(well, 10.0f, 0.0f), Font::MonoSmall, cv.theme().on_surface);
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

        // 2. Region Screen Recording card
        {
            const CardHeader hd = make_header(cv, rw, L"Region Screen Recording", cv.theme().on_surface,
                                              L"Native Windows Media Foundation H.264 MP4 encoder");

            Flow fps_row{8.0f, 8.0f};
            flow_text(cv, fps_row, L"Frame rate:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
            fps_row.add(88.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdFps30), r, 0, L"30 FPS", IdFps30, 0, sh_.settings.record_fps == 30 ? 1 : 0);
            });
            fps_row.add(88.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdFps60), r, 0, L"60 FPS", IdFps60, 0, sh_.settings.record_fps == 60 ? 1 : 0);
            });

            Flow br_row{8.0f, 8.0f};
            flow_text(cv, br_row, L"Video quality:", Font::Body, cv.theme().on_surface, 130.0f, 32.0f);
            br_row.add(118.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdBitrate6), r, 0, L"Standard (6M)", IdBitrate6, 0, sh_.settings.record_bitrate_mbps == 6 ? 1 : 0);
            });
            br_row.add(108.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdBitrate12), r, 0, L"High (12M)", IdBitrate12, 0, sh_.settings.record_bitrate_mbps == 12 ? 1 : 0);
            });
            br_row.add(112.0f, 32.0f, [&](D2D1_RECT_F r) {
                draw_button(cv, hover_.alpha(IdBitrate20), r, 0, L"Ultra (20M)", IdBitrate20, 0, sh_.settings.record_bitrate_mbps == 20 ? 1 : 0);
            });

            const float r1_y = y + 16.0f + hd.h + 14.0f;
            const float r2_y = r1_y + fps_row.height(rw) + 12.0f;
            const float r3_y = r2_y + br_row.height(rw) + 14.0f;
            const float card_h = (r3_y + 34.0f + 16.0f) - y;
            const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

            (void)draw_header(cv, lx, y, rw, hd);
            fps_row.draw(lx, r1_y, rw);
            br_row.draw(lx, r2_y, rw);

            cv.text(L"Include mouse cursor in recordings", rect(lx, r3_y, rw - 60.0f, 32.0f), Font::Body, cv.theme().on_surface);
            draw_switch(cv, hover_.alpha(IdToggleRecordCursor), sw_rec_cursor_.value, lx + rw, r3_y + 16.0f, IdToggleRecordCursor);

            y = next_y;
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
        const float card_h = 260.0f;
        const float next_y = card_begin(cv, x, y, w, card_h, CARD_GAP);

        const D2D1_RECT_F icon_r = cv.snap(rect(x + ((w - 72.0f) * 0.5f), y + 28.0f, 72.0f, 72.0f));
        if (ID2D1Bitmap1* bmp = ensure_app_icon(cv)) cv.bitmap(bmp, icon_r);
        else cv.icon(glyph::camera, icon_r, cv.theme().primary, Font::IconLarge);

        cv.text(APP_NAME, rect(lx, y + 112.0f, rw, 26.0f), Font::Title, cv.theme().on_surface, Align::Center);
        const std::wstring ver = ulp::str::concat_strings(L"Version ", APP_VERSION, L"  •  Win32 + Direct2D 1.3 + SQLite");
        cv.text(ver, rect(lx, y + 140.0f, rw, 22.0f), Font::Body, cv.theme().on_surface_variant, Align::Center);

        cv.hline(lx, lx + rw, y + 178.0f, with_alpha(cv.theme().outline_variant, 0.5f));

        const std::wstring db_info = ulp::str::concat_strings(
            L"History database: ", format_count(stats_.total_count), L" captures (",
            format_count(stats_.image_count), L" images, ", format_count(stats_.video_count), L" videos)  •  ",
            format_size(stats_.db_bytes));
        cv.text(db_info, rect(lx, y + 196.0f, rw, 22.0f), Font::Small, cv.theme().on_surface_variant, Align::Center);
        cv.text(history_db_path().wstring(), rect(lx, y + 220.0f, rw, 20.0f), Font::MonoSmall, cv.theme().outline, Align::Center);
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
    // Input
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] D2D1_POINT_2F to_dip(LPARAM lp) const noexcept {
        const float s = target_.scale();
        return {.x = GET_X_LPARAM(lp) / s, .y = GET_Y_LPARAM(lp) / s};
    }

    void update_slider_from_x(int slider_id, float x) {
        const D2D1_RECT_F r = (slider_id == IdSliderToastSize)
            ? slider_size_r_
            : (slider_id == IdSliderImageQuality ? slider_quality_r_ : slider_timeout_r_);
        const float l = r.left + 8.0f;
        const float ri = std::max(l + 4.0f, r.right - 8.0f);
        const float k = std::clamp((x - l) / (ri - l), 0.0f, 1.0f);
        if (slider_id == IdSliderToastSize) {
            sh_.settings.toast_width_dip = std::round((160.0f + (k * 260.0f)) / 5.0f) * 5.0f;
        } else if (slider_id == IdSliderImageQuality) {
            sh_.settings.image_quality = std::clamp(static_cast<int>(std::lround((10.0f + (k * 90.0f)) / 5.0f)) * 5, 10, 100);
        } else {
            sh_.settings.toast_timeout_s = std::round((1.0f + (k * 14.0f)) * 2.0f) / 2.0f;
        }
        sh_.save();
        invalidate();
    }

    void on_lbutton_down(LPARAM lp) {
        const D2D1_POINT_2F p = to_dip(lp);
        const Hit* h = hit_test(hits_, p);
        if (!h) {
            stop_recording_hotkey();
            invalidate();
            return;
        }
        if (recording_action_ && h->id != IdRecordHotkey) stop_recording_hotkey();

        switch (h->id) {
            case IdTab0: case IdTab1: case IdTab2:
                tab_ = h->id - IdTab0;
                tab_anim_.set(static_cast<float>(tab_), true);
                scroll_ = target_scroll_ = 0.0f;
                if (tab_ == 2) {
                    if (auto st = sh_.history.stats()) stats_ = *st;
                }
                start_anim();
                return;
            case IdToggleHotkey: {
                const auto a = static_cast<HotkeyAction>(h->index);
                sh_.settings.hotkey(a).enabled = !sh_.settings.hotkey(a).enabled;
                sh_.save();
                if (sh_.on_reload_hotkeys) sh_.on_reload_hotkeys();
                sync_state(true);
                start_anim();
                return;
            }
            case IdRecordHotkey: {
                const auto a = static_cast<HotkeyAction>(h->index);
                if (recording_action_ && *recording_action_ == a) {
                    stop_recording_hotkey();
                } else {
                    recording_action_ = a;
                    record_conflict_msg_.clear();
                    if (sh_.on_suspend_hotkeys) sh_.on_suspend_hotkeys(true);
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
                start_anim();
                return;
            }
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
            case IdToggleToast:
                sh_.settings.show_corner_toast = !sh_.settings.show_corner_toast;
                sh_.save();
                sync_state(true);
                start_anim();
                return;
            case IdSliderToastSize:
            case IdSliderToastTimeout:
            case IdSliderImageQuality:
                dragging_slider_ = h->id;
                SetCapture(hwnd_);
                update_slider_from_x(h->id, p.x);
                return;
            case IdTestToast:
                if (sh_.on_test_toast) sh_.on_test_toast();
                return;
            case IdFps30: sh_.settings.record_fps = 30; sh_.save(); invalidate(); return;
            case IdFps60: sh_.settings.record_fps = 60; sh_.save(); invalidate(); return;
            case IdBitrate6: sh_.settings.record_bitrate_mbps = 6; sh_.save(); invalidate(); return;
            case IdBitrate12: sh_.settings.record_bitrate_mbps = 12; sh_.save(); invalidate(); return;
            case IdBitrate20: sh_.settings.record_bitrate_mbps = 20; sh_.save(); invalidate(); return;
            case IdToggleRecordCursor:
                sh_.settings.record_cursor = !sh_.settings.record_cursor;
                sh_.save();
                sync_state(true);
                start_anim();
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

    void on_key_down(UINT vk) {
        if (recording_action_) {
            if (vk == VK_ESCAPE) {
                stop_recording_hotkey();
                invalidate();
                return;
            }
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return;
            uint32_t mods = 0;
            if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
            if (mods == 0 && !is_standalone_hotkey_vk(vk)) mods = MOD_ALT;

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
            stop_recording_hotkey();
            if (sh_.on_reload_hotkeys) sh_.on_reload_hotkeys();
            sync_state(true);
            invalidate();
            return;
        }
        if (vk == VK_ESCAPE) hide();
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
                if (wp != SIZE_MINIMIZED && visible()) {
                    (void)target_.resize(LOWORD(lp), HIWORD(lp));
                    scroll_ = target_scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
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
                if (visible() && anim_active_) {
                    MSG m;
                    while (PeekMessageW(&m, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) {
                        DispatchMessageW(&m);
                    }
                    if (visible() && anim_active_) InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_MOUSEWHEEL: {
                const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f;
                target_scroll_ = std::clamp(target_scroll_ - (delta * 90.0f), 0.0f, max_scroll());
                start_anim();
                return 0;
            }
            case WM_LBUTTONDOWN: on_lbutton_down(lp); return 0;
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
                if (hover_.set(h ? h->id : IdNone, h ? h->index : 0, true)) start_anim();
                SetCursor(h ? cursor_hand_ : cursor_arrow_);
                TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (hover_.set(IdNone, 0, true)) start_anim();
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (msg == WM_SYSKEYDOWN && wp == VK_F4 && !recording_action_) break;
                on_key_down(static_cast<UINT>(wp));
                return 0;
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
    AnimFloat sw_startup_;
    AnimFloat sw_auto_copy_;
    AnimFloat sw_cap_cursor_;
    AnimFloat sw_split_month_;
    AnimFloat sw_toast_;
    AnimFloat sw_rec_cursor_;
    AnimFloat sw_auto_update_;

    bool startup_enabled_ = false;
    std::wstring save_dir_w_;
    HistoryStats stats_{};
    std::optional<HotkeyAction> recording_action_;
    std::wstring record_conflict_msg_;
    UpdateJob update_job_;
    std::jthread update_check_thread_;

    wil::com_ptr<ID2D1Bitmap1> app_icon_;
    uint64_t app_icon_epoch_ = 0;

    std::vector<Hit> hits_;
    HoverTracker hover_;
    float scroll_ = 0.0f;
    float target_scroll_ = 0.0f;
    float content_h_ = 0.0f;
    D2D1_RECT_F view_rect_{};
    D2D1_RECT_F scroll_thumb_{};
    D2D1_RECT_F slider_size_r_{};
    D2D1_RECT_F slider_timeout_r_{};
    D2D1_RECT_F slider_quality_r_{};
    bool dragging_scroll_ = false;
    bool in_sizemove_ = false;
    float scroll_drag_offset_ = 0.0f;
    int dragging_slider_ = IdNone;
    bool anim_active_ = false;
    bool rendering_ = false;
    std::chrono::steady_clock::time_point last_anim_{};
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
};

} // namespace capusy::ui
