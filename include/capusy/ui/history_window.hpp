#pragma once

#include "canvas.hpp"
#include "preview_math.hpp"
#include "shared.hpp"
#include "textfield.hpp"
#include "video_player.hpp"
#include "widgets.hpp"
#include "window_lifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace capusy::ui {

/// Screenshot & recording history window:
/// - Left scrollable list of captures stored in SQLite (newest first)
/// - Right preview pane (high-res WIC image preview + reused Media Foundation VideoPlayer for MP4s)
/// - Double-click to open in default app
/// - Right-click context menu to copy, reveal, remove, or delete file
/// - OLE async drag-and-drop OUT of the window (from either list or preview), matching usyseek
class HistoryWindow {
public:
    explicit HistoryWindow(Shared& sh) : sh_{sh} {}
    HistoryWindow(const HistoryWindow&) = delete;
    HistoryWindow& operator=(const HistoryWindow&) = delete;
    HistoryWindow(HistoryWindow&&) = delete;
    HistoryWindow& operator=(HistoryWindow&&) = delete;
    ~HistoryWindow() {
        video_.shutdown();
        target_.release();
        if (hwnd_) DestroyWindow(hwnd_);
    }

    bool create(HINSTANCE inst) {
        inst_ = inst;
        WNDCLASSEXW wc{.cbSize = sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &HistoryWindow::wndproc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = HISTORY_CLASS;
        RegisterClassExW(&wc);

        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        const UINT sys_dpi = GetDpiForSystem();
        const float s = (sys_dpi > 0 ? static_cast<float>(sys_dpi) : 96.0f) / 96.0f;
        RECT rc{.left = 0, .top = 0, .right = static_cast<LONG>(1040 * s), .bottom = static_cast<LONG>(680 * s)};
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, sys_dpi);

        hwnd_ = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP, HISTORY_CLASS, L"capusy — History",
            style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, inst, this);
        if (!hwnd_) return false;

        icons_.apply(hwnd_, inst, 101);
        cloak_window(hwnd_, true);
        video_.set_notify(hwnd_, WM_APP_VIDEO);
        video_.set_muted(sh_.settings.video_muted);

        cursor_arrow_ = LoadCursorW(nullptr, IDC_ARROW);
        cursor_hand_ = LoadCursorW(nullptr, IDC_HAND);
        cursor_ibeam_ = LoadCursorW(nullptr, IDC_IBEAM);
        search_field_.placeholder = L"Search title, name, date…";
        return true;
    }

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] bool visible() const noexcept { return hwnd_ && IsWindowVisible(hwnd_) != FALSE; }

    void show() {
        if (!hwnd_) return;
        if (!sh_.gfx.ensure_device(hwnd_)) return;
        if (!target_.ready() && !target_.attach(sh_.gfx, hwnd_)) return;
        sh_.thumbs.start(hwnd_, WM_APP_THUMBS, 2);
        sh_.previews.start(hwnd_, WM_APP_THUMBS, 1);

        reload_entries(true);
        apply_dark_window(hwnd_, DWMSBT_MAINWINDOW, true);
        cloak_window(hwnd_, false);
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        else ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        window_active_ = true;
        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
        sync_preview_media();
        anim_active_ = false;
        full_dirty_ = true;
        render();
        ValidateRect(hwnd_, nullptr);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void hide() {
        if (!hwnd_ || dragging_out_) return;
        KillTimer(hwnd_, TIMER_VIDEO);
        KillTimer(hwnd_, TIMER_DROP);
        KillTimer(hwnd_, TIMER_CARET);
        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
        raw_hover_id_ = IdNone;
        raw_hover_index_ = 0;
        set_search_focus(false);
        dragging_search_ = false;
        video_.close();
        sh_.previews.clear();
        sh_.thumbs.clear();
        target_.release();
        dragging_scroll_ = false;
        in_sizemove_ = false;
        scrubbing_video_ = false;
        drag_pending_ = false;
        anim_active_ = false;
        hover_.set(IdNone, 0, false);
        cloak_window(hwnd_, true);
        ShowWindow(hwnd_, SW_HIDE);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void reload_entries(bool keep_selection = true) {
        const int64_t prev_id = (keep_selection && selected_idx_ >= 0 && std::cmp_less(selected_idx_, entries_.size()))
            ? entries_[static_cast<size_t>(selected_idx_)].id : -1;

        std::optional<CaptureKind> filter;
        if (filter_ == Filter::Images) filter = CaptureKind::Image;
        else if (filter_ == Filter::Videos) filter = CaptureKind::Video;

        if (auto list = sh_.history.list_recent(2500, filter)) {
            all_entries_ = std::move(*list);
        } else {
            all_entries_.clear();
        }

        // Precompute display strings and lowercased search haystack once so scrolling & typing stay instant.
        all_cache_.clear();
        all_cache_.reserve(all_entries_.size());
        for (const CaptureEntry& e : all_entries_) {
            RowCache rc;
            rc.wpath = to_wide(e.path);
            const size_t slash = rc.wpath.find_last_of(L"\\/");
            rc.filename = (slash != std::wstring::npos) ? rc.wpath.substr(slash + 1) : rc.wpath;
            rc.window_title = to_wide(e.window_title);
            rc.process_name = to_wide(e.process_name);
            if (!e.format.empty()) {
                rc.format = to_wide(e.format);
            } else if (e.kind == CaptureKind::Video) {
                rc.format = L"MP4";
            } else {
                const size_t dot = rc.filename.find_last_of(L'.');
                if (dot != std::wstring::npos && dot + 1 < rc.filename.size()) {
                    rc.format = rc.filename.substr(dot + 1);
                    CharUpperBuffW(rc.format.data(), static_cast<DWORD>(rc.format.size()));
                }
            }
            rc.title_line = rc.window_title.empty() ? rc.filename : rc.window_title;

            if (!rc.process_name.empty()) {
                rc.line2 = rc.process_name;
            }
            if (e.width > 0 && e.height > 0) {
                if (!rc.line2.empty()) rc.line2 += L"  •  ";
                append_to(rc.line2, to_fixed_wstr<16>(e.width), L"×", to_fixed_wstr<16>(e.height));
            }
            if (e.size_bytes > 0) {
                if (!rc.line2.empty()) rc.line2 += L"  •  ";
                rc.line2 += std::wstring_view{format_size(e.size_bytes)};
            }
            if (e.kind == CaptureKind::Video && e.duration_ms > 0) {
                if (!rc.line2.empty()) rc.line2 += L"  •  ";
                rc.line2 += std::wstring_view{format_duration_ms(e.duration_ms)};
            }
            rc.ts = format_timestamp(e.created_at);
            rc.iso_ts = format_timestamp_iso(e.created_at);

            std::wstring hay;
            hay.reserve(rc.filename.size() + rc.window_title.size() + rc.process_name.size() + rc.wpath.size() + 64);
            append_to(hay, rc.filename, L' ', rc.window_title, L' ', rc.process_name, L' ', rc.format, L' ',
                      rc.ts, L' ', rc.iso_ts, L' ', rc.wpath);
            if (e.mode == CaptureMode::Full) hay += L" full screen";
            else if (e.mode == CaptureMode::Region) hay += L" region";
            else hay += L" recording video";
            rc.search_lower = lower(hay);
            all_cache_.push_back(std::move(rc));
        }

        apply_search_filter(prev_id);
    }

    void apply_search_filter(int64_t preferred_id = -1) {
        if (preferred_id < 0 && selected_idx_ >= 0 && std::cmp_less(selected_idx_, entries_.size())) {
            preferred_id = entries_[static_cast<size_t>(selected_idx_)].id;
        }
        const std::wstring q = lower(trim(search_field_.text));

        entries_.clear();
        row_cache_.clear();
        if (q.empty()) {
            entries_ = all_entries_;
            row_cache_ = all_cache_;
        } else {
            entries_.reserve(all_entries_.size());
            row_cache_.reserve(all_cache_.size());
            for (size_t i = 0; i < all_entries_.size(); ++i) {
                if (matches_history_query_lower(all_cache_[i].search_lower, q)) {
                    entries_.push_back(all_entries_[i]);
                    row_cache_.push_back(all_cache_[i]);
                }
            }
        }

        selected_idx_ = -1;
        if (preferred_id >= 0) {
            for (size_t i = 0; i < entries_.size(); ++i) {
                if (entries_[i].id == preferred_id) {
                    selected_idx_ = static_cast<int>(i);
                    break;
                }
            }
        }
        if (selected_idx_ < 0 && !entries_.empty()) selected_idx_ = 0;

        sync_preview_media();
        clamp_scroll();
        invalidate();
    }

    void on_thumbs_ready() {
        if (!visible() || !target_.ready()) return;
        bool changed = false;
        if (sh_.thumbs.process(target_.dc())) changed = true;
        if (sh_.previews.process(target_.dc())) changed = true;
        if (changed) invalidate();
    }

    void reset_gpu() noexcept {
        video_.reset_gpu();
        target_.release();
    }

    void invalidate() noexcept {
        full_dirty_ = true;
        if (visible()) InvalidateRect(hwnd_, nullptr, FALSE);
    }

private:
    static constexpr float HEADER_H = 54.0f;
    static constexpr float SIDEBAR_W = 336.0f;
    static constexpr float SIDEBAR_TOP_H = 86.0f;
    static constexpr float ROW_H = 78.0f;
    static constexpr float ROW_GAP = 6.0f;
    static constexpr UINT_PTR TIMER_VIDEO = 2;
    static constexpr UINT_PTR TIMER_DROP = 3;
    static constexpr UINT_PTR TIMER_CARET = 4;
    static constexpr UINT_PTR TIMER_TOOLTIP = 5;

    struct RowCache {
        std::wstring wpath;
        std::wstring filename;
        std::wstring window_title;
        std::wstring process_name;
        std::wstring format;
        std::wstring title_line;
        std::wstring line2;
        FixedWString<64> ts;
        FixedWString<32> iso_ts;
        std::wstring search_lower;
    };

    enum class Filter : uint8_t { All = 0, Images, Videos };

    enum HitId : int {
        IdNone = 0,
        IdBtnFull,
        IdBtnRegion,
        IdBtnRecord,
        IdBtnSettings,
        IdSearchBox,
        IdSearchClear,
        IdFilterAll,
        IdFilterImages,
        IdFilterVideos,
        IdRow,
        IdScrollbar,
        IdPreviewArea,
        IdActCopy,
        IdActOpen,
        IdActExplorer,
        IdActDelete,
        IdVideoPlay,
        IdVideoMute,
        IdVideoScrub,
        IdFooterInfo,
    };

    enum CtxCmd : UINT {
        CmdOpen = 1001,
        CmdCopy,
        CmdCopyPath,
        CmdExplorer,
        CmdRemoveHistory,
        CmdDeleteFile,
    };

    [[nodiscard]] const CaptureEntry* selected_entry() const noexcept {
        if (selected_idx_ < 0 || std::cmp_greater_equal(selected_idx_, entries_.size())) return nullptr;
        return &entries_[static_cast<size_t>(selected_idx_)];
    }

    [[nodiscard]] static bool is_playable_video(const CaptureEntry& e) noexcept {
        return e.kind == CaptureKind::Video && !is_gif_capture(e.format, e.path);
    }

    void sync_preview_media() {
        const CaptureEntry* e = selected_entry();
        if (!e) {
            video_.close();
            if (hwnd_) KillTimer(hwnd_, TIMER_VIDEO);
            return;
        }
        const std::wstring wpath = to_wide(e->path);
        if (is_playable_video(*e)) {
            if (visible() && sh_.gfx.d3d()) {
                video_.open(wpath, sh_.gfx.d3d(), sh_.gfx.epoch(), true);
                SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
            }
        } else {
            video_.close();
            if (hwnd_) KillTimer(hwnd_, TIMER_VIDEO);
            sh_.previews.request(static_cast<uint64_t>(e->id), wpath, 2048);
        }
    }

    [[nodiscard]] float max_scroll() const noexcept {
        const float list_h = std::max(0.0f, target_.size().height - HEADER_H - SIDEBAR_TOP_H);
        const float total_h = entries_.empty() ? 0.0f : (static_cast<float>(entries_.size()) * (ROW_H + ROW_GAP) + 12.0f);
        return std::max(0.0f, total_h - list_h);
    }

    void clamp_scroll() noexcept {
        const float ms = max_scroll();
        target_scroll_ = std::clamp(target_scroll_, 0.0f, ms);
        scroll_ = std::clamp(scroll_, 0.0f, ms);
    }

    void ensure_row_visible(int idx) noexcept {
        if (idx < 0) return;
        const float list_h = std::max(1.0f, target_.size().height - HEADER_H - SIDEBAR_TOP_H);
        const float top = 6.0f + (static_cast<float>(idx) * (ROW_H + ROW_GAP));
        const float bot = top + ROW_H;
        if (top < target_scroll_) target_scroll_ = top - 6.0f;
        else if (bot > target_scroll_ + list_h) target_scroll_ = bot - list_h + 6.0f;
        clamp_scroll();
        start_anim();
    }

    void set_search_focus(bool focus) noexcept {
        if (search_field_.focused == focus) return;
        search_field_.focused = focus;
        search_field_.caret_on = focus;
        if (hwnd_) {
            if (focus) SetTimer(hwnd_, TIMER_CARET, 530, nullptr);
            else KillTimer(hwnd_, TIMER_CARET);
        }
        invalidate();
    }

    void reset_caret_blink() noexcept {
        search_field_.caret_on = true;
        if (hwnd_ && search_field_.focused) {
            SetTimer(hwnd_, TIMER_CARET, 530, nullptr);
        }
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
        if (tooltip_.step(dt)) moving = true;
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

    void render() {
        if (!visible() || rendering_) return;
        if (!target_.ready()) {
            if (!sh_.gfx.ensure_device(hwnd_) || !target_.rebuild()) return;
        }
        rendering_ = true;
        const auto clear_r = wil::scope_exit([&] { rendering_ = false; });

        const bool anim = step_anim();

        if (video_.needs_recover() && sh_.gfx.d3d()) {
            if (video_.recover_if_needed(sh_.gfx.d3d(), sh_.gfx.epoch())) {
                SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
            }
        }
        if (video_.active() && target_.dc()) {
            (void)video_.tick(sh_.gfx.d3d(), sh_.gfx.epoch(), target_.dc());
        }

        const auto mode = in_sizemove_ ? WindowTarget::PresentMode::Immediate : WindowTarget::PresentMode::Vsync;
        const bool ok = target_.render_frame(true, {}, mode, [&](std::optional<D2D1_RECT_F> clip, bool) {
            hits_.clear();
            Canvas cv{target_, sh_.fonts, hits_, default_theme(), clip};
            draw(cv);
        });
        full_dirty_ = false;
        anim_active_ = visible() && anim;
        if (!ok) {
            target_.release();
            sh_.gfx.recover(hwnd_);
            sh_.thumbs.reset_gpu();
            sh_.previews.reset_gpu();
            video_.reset_gpu();
            invalidate();
        }
    }

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), with_alpha(cv.theme().bg, cv.theme().window_bg_alpha));

        draw_header(cv, sz);

        const float body_top = HEADER_H;
        const float sidebar_w = std::min(SIDEBAR_W, sz.width * 0.45f);
        draw_sidebar(cv, D2D1::RectF(0.0f, body_top, sidebar_w, sz.height));
        cv.vline(sidebar_w, body_top, sz.height, with_alpha(cv.theme().outline_variant, 0.65f));
        draw_preview_pane(cv, D2D1::RectF(sidebar_w + 1.0f, body_top, sz.width, sz.height));
        draw_tooltip(cv, sz, tooltip_.anchor, tooltip_.title, tooltip_.subtitle, tooltip_.alpha.value);
    }

    [[nodiscard]] std::wstring hotkey_subtitle(HotkeyAction a) const {
        const auto idx = static_cast<size_t>(a);
        if (!is_hotkey_active(sh_.hotkey_status[idx].state)) return {};
        const auto& hb = sh_.settings.hotkey(a);
        const auto txt = format_hotkey(hb.mods, hb.vk);
        return std::wstring{txt.view()};
    }

    [[nodiscard]] bool has_tooltip(int id, int idx) const noexcept {
        switch (id) {
            case IdBtnFull:
            case IdBtnRegion:
            case IdBtnRecord:
            case IdBtnSettings:
            case IdSearchBox:
            case IdSearchClear:
            case IdFilterAll:
            case IdFilterImages:
            case IdFilterVideos:
            case IdActCopy:
            case IdActOpen:
            case IdActExplorer:
            case IdActDelete:
            case IdVideoPlay:
            case IdVideoMute:
            case IdVideoScrub:
                return true;
            case IdRow:
                return idx >= 0 && std::cmp_less(idx, row_cache_.size());
            case IdFooterInfo:
                return selected_idx_ >= 0 && std::cmp_less(selected_idx_, row_cache_.size());
            default:
                return false;
        }
    }

    bool build_tooltip(int id, int idx, std::wstring& title, std::wstring& subtitle) const {
        title.clear();
        subtitle.clear();
        switch (id) {
            case IdBtnFull:
                title = L"Capture full screen";
                subtitle = hotkey_subtitle(HotkeyAction::FullScreenshot);
                return true;
            case IdBtnRegion:
                title = L"Capture region";
                subtitle = hotkey_subtitle(HotkeyAction::RegionScreenshot);
                return true;
            case IdBtnRecord:
                title = sh_.recording_active ? L"Stop region recording" : L"Start region recording";
                subtitle = hotkey_subtitle(HotkeyAction::RegionRecord);
                return true;
            case IdBtnSettings:
                title = sh_.has_update_available() ? L"Settings (Update available)" : L"Open settings";
                return true;
            case IdSearchBox:
                title = L"Search captures (Ctrl+F)";
                subtitle = L"Window title, app, filename, or date";
                return true;
            case IdSearchClear:
                title = L"Clear search (Esc)";
                return true;
            case IdFilterAll:
                title = L"Show all captures";
                return true;
            case IdFilterImages:
                title = L"Show screenshots only";
                return true;
            case IdFilterVideos:
                title = L"Show recordings only";
                return true;
            case IdRow:
                if (idx >= 0 && std::cmp_less(idx, row_cache_.size())) {
                    const RowCache& rc = row_cache_[static_cast<size_t>(idx)];
                    title = !rc.window_title.empty()
                        ? ulp::str::concat_strings(rc.window_title, L"  —  ", rc.filename)
                        : rc.filename;
                    subtitle = rc.wpath;
                    return !title.empty() || !subtitle.empty();
                }
                return false;
            case IdFooterInfo:
                if (selected_idx_ >= 0 && std::cmp_less(selected_idx_, row_cache_.size())) {
                    const RowCache& rc = row_cache_[static_cast<size_t>(selected_idx_)];
                    title = !rc.window_title.empty()
                        ? ulp::str::concat_strings(rc.window_title, L"  —  ", rc.filename)
                        : rc.filename;
                    subtitle = rc.wpath;
                    return !title.empty() || !subtitle.empty();
                }
                return false;
            case IdActCopy: {
                const CaptureEntry* e = selected_entry();
                title = (e && e->kind == CaptureKind::Video) ? L"Copy video file (Ctrl+C)" : L"Copy image (Ctrl+C)";
                return true;
            }
            case IdActOpen:
                title = L"Open in default app (Enter)";
                return true;
            case IdActExplorer:
                title = L"Show in Explorer";
                return true;
            case IdActDelete:
                title = L"Delete file & remove from history (Del)";
                return true;
            case IdVideoPlay: {
                const bool playing = video_.is_scrubbing() ? video_.want_play() : video_.is_playing();
                title = playing ? L"Pause (Space)" : L"Play (Space)";
                return true;
            }
            case IdVideoMute:
                title = video_.is_muted() ? L"Unmute" : L"Mute";
                return true;
            case IdVideoScrub:
                title = L"Seek video";
                subtitle = L"Click or drag to scrub";
                return true;
            default:
                return false;
        }
    }

    void dismiss_tooltip() noexcept {
        const bool was_vis = tooltip_.visible();
        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
        if (was_vis) invalidate();
    }

    void draw_header(Canvas& cv, D2D1_SIZE_F sz) {
        cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, HEADER_H), with_alpha(cv.theme().surface_lowest, 0.78f));
        cv.hline(0.0f, sz.width, HEADER_H, with_alpha(cv.theme().outline_variant, 0.65f));

        cv.icon(glyph::camera, rect(14.0f, 11.0f, 32.0f, 32.0f), cv.theme().primary, Font::IconLarge);
        cv.text(L"capusy", rect(50.0f, 0.0f, 90.0f, HEADER_H), Font::Title, cv.theme().on_surface);

        // Action buttons on right
        const float set_w = button_width(cv, L"Settings", true, 98.0f);
        const wchar_t* rec_lbl = sh_.recording_active ? L"Stop Rec" : L"Record";
        const float rec_w = button_width(cv, rec_lbl, true, 96.0f);
        const float reg_w = button_width(cv, L"Region", true, 92.0f);
        const float full_w = button_width(cv, L"Full Screen", true, 112.0f);

        float rx = sz.width - 14.0f;
        rx -= set_w;
        const D2D1_RECT_F set_r = rect(rx, 11.0f, set_w, 32.0f);
        draw_button(cv, hover_.alpha(IdBtnSettings), set_r, glyph::settings, L"Settings", IdBtnSettings, 0, 3);
        if (sh_.has_update_available()) {
            cv.fill_circle({.x = set_r.right - 7.0f, .y = set_r.top + 7.0f}, 4.0f, cv.theme().primary);
        }
        rx -= 8.0f + rec_w;
        draw_button(cv, hover_.alpha(IdBtnRecord), rect(rx, 11.0f, rec_w, 32.0f), sh_.recording_active ? glyph::stop : glyph::video, rec_lbl, IdBtnRecord, 0, sh_.recording_active ? 2 : 0);
        rx -= 8.0f + reg_w;
        draw_button(cv, hover_.alpha(IdBtnRegion), rect(rx, 11.0f, reg_w, 32.0f), glyph::crop, L"Region", IdBtnRegion, 0, 0);
        rx -= 8.0f + full_w;
        draw_button(cv, hover_.alpha(IdBtnFull), rect(rx, 11.0f, full_w, 32.0f), glyph::monitor, L"Full Screen", IdBtnFull, 0, 1);
    }

    void draw_sidebar(Canvas& cv, D2D1_RECT_F area) {
        cv.fill(area, with_alpha(cv.theme().surface_low, 0.55f));

        // Search bar
        const float sx = area.left + 10.0f;
        const float sy = area.top + 8.0f;
        const float sw = std::max(80.0f, width(area) - 20.0f);
        const D2D1_RECT_F sbox = rect(sx, sy, sw, 32.0f);
        search_box_rect_ = sbox;
        draw_well(cv, sbox, 9.0f, true, search_field_.focused, hover_.alpha(IdSearchBox));
        cv.icon(glyph::search, rect(sbox.left + 8.0f, sbox.top, 18.0f, height(sbox)),
                search_field_.focused ? cv.theme().primary : cv.theme().outline, Font::IconSmall);
        const bool has_q = !search_field_.text.empty();
        const float clear_space = has_q ? 28.0f : 8.0f;
        search_text_rect_ = D2D1::RectF(sbox.left + 30.0f, sbox.top + 2.0f, sbox.right - clear_space, sbox.bottom - 2.0f);
        search_field_.draw(cv, search_text_rect_, Font::Small, cv.theme().on_surface, cv.theme().outline);
        cv.hit(sbox, IdSearchBox);
        if (has_q) {
            const D2D1_RECT_F cr = rect(sbox.right - 26.0f, sbox.top + 4.0f, 22.0f, 24.0f);
            draw_button(cv, hover_.alpha(IdSearchClear), cr, glyph::close, L"", IdSearchClear, 0, 3);
        }

        // Filter bar
        const float fy = area.top + 48.0f;
        const float fx = area.left + 10.0f;
        draw_button(cv, hover_.alpha(IdFilterAll), rect(fx, fy, 54.0f, 28.0f), 0, L"All", IdFilterAll, 0, filter_ == Filter::All ? 1 : 3);
        draw_button(cv, hover_.alpha(IdFilterImages), rect(fx + 58.0f, fy, 72.0f, 28.0f), 0, L"Images", IdFilterImages, 0, filter_ == Filter::Images ? 1 : 3);
        draw_button(cv, hover_.alpha(IdFilterVideos), rect(fx + 134.0f, fy, 72.0f, 28.0f), 0, L"Videos", IdFilterVideos, 0, filter_ == Filter::Videos ? 1 : 3);

        FixedWString<48> cnt;
        if (has_q && entries_.size() != all_entries_.size()) {
            cnt.assign(format_count(entries_.size()));
            cnt.append(L" / ");
            cnt.append(format_count(all_entries_.size()));
        } else {
            cnt.assign(format_count(entries_.size()));
        }
        cv.text(cnt, rect(area.right - 92.0f, fy, 80.0f, 28.0f), Font::Caption, cv.theme().outline, Align::Right);
        cv.hline(area.left, area.right, area.top + SIDEBAR_TOP_H - 1.0f, with_alpha(cv.theme().outline_variant, 0.45f));

        const D2D1_RECT_F list_r = D2D1::RectF(area.left, area.top + SIDEBAR_TOP_H, area.right, area.bottom);
        list_rect_ = list_r;
        cv.push_clip(list_r);

        if (entries_.empty()) {
            cv.icon(has_q ? glyph::search : glyph::history, rect(list_r.left, list_r.top + 80.0f, width(list_r), 36.0f), cv.theme().outline, Font::IconLarge);
            cv.text(has_q ? L"No matching captures" : L"No captures yet", rect(list_r.left, list_r.top + 120.0f, width(list_r), 24.0f),
                    Font::BodySemi, cv.theme().on_surface_variant, Align::Center);
            cv.text(has_q ? L"Try searching by window title, app name, filename, or date (e.g. 2026-11)"
                          : L"Press your screenshot hotkey or click Full Screen / Region",
                    rect(list_r.left + 20.0f, list_r.top + 146.0f, width(list_r) - 40.0f, 44.0f),
                    Font::SmallWrap, cv.theme().outline, Align::Center);
            cv.pop_clip();
            return;
        }

        const float step = ROW_H + ROW_GAP;
        const int first = std::max(0, static_cast<int>(std::floor((scroll_ - 6.0f) / step)));
        const int last = std::min(static_cast<int>(entries_.size()) - 1, static_cast<int>(std::ceil((scroll_ + height(list_r)) / step)));
        const UINT thumb_px = static_cast<UINT>(std::lround(112.0f * target_.scale()));

        for (int i = first; i <= last; ++i) {
            const CaptureEntry& e = entries_[static_cast<size_t>(i)];
            const float ry = list_r.top + 6.0f + (static_cast<float>(i) * step) - scroll_;
            const D2D1_RECT_F rr = rect(list_r.left + 8.0f, ry, width(list_r) - 20.0f, ROW_H);
            const bool sel = (i == selected_idx_);
            const float ha = hover_.alpha(IdRow, i);

            const D2D1_COLOR_F bg = sel
                ? lerp_color(with_alpha(cv.theme().primary_container, 0.55f), with_alpha(cv.theme().primary_container, 0.72f), ha)
                : with_alpha(cv.theme().surface_high, 0.45f * ha);
            if (bg.a > 0.002f) cv.fill_round(rr, 10.0f, bg);
            if (sel) cv.stroke_round(rr, 10.0f, with_alpha(cv.theme().primary, 0.75f), 1.2f);

            // Thumbnail box
            const RowCache& rc = row_cache_[static_cast<size_t>(i)];
            const D2D1_RECT_F tr = rect(rr.left + 8.0f, rr.top + 8.0f, 84.0f, ROW_H - 16.0f);
            cv.fill_round(tr, 7.0f, cv.theme().surface_lowest);
            const uint64_t tid = static_cast<uint64_t>(e.id);
            if (ID2D1Bitmap1* bmp = sh_.thumbs.get(tid)) {
                cv.fill_bitmap_round(bmp, tr, 7.0f, ThumbFit::Crop);
            } else {
                sh_.thumbs.request(tid, rc.wpath, thumb_px);
                cv.icon(e.kind == CaptureKind::Video ? glyph::video : glyph::photo, tr, cv.theme().outline, Font::Icon);
            }
            if (!rc.format.empty()) {
                const float bw = std::max(30.0f, cv.measure(rc.format, Font::Label) + 8.0f);
                const D2D1_RECT_F vb = rect(tr.right - bw - 3.0f, tr.bottom - 18.0f, bw, 15.0f);
                cv.fill_round(vb, 4.0f, with_alpha(cv.theme().surface_lowest, 0.85f));
                cv.text(rc.format, vb, Font::Label, e.kind == CaptureKind::Video ? cv.theme().amber : cv.theme().on_surface_variant, Align::Center);
            }

            // Text info (zero allocations per frame)
            const float tx = tr.right + 10.0f;
            const float tw = std::max(40.0f, rr.right - tx - 8.0f);
            cv.text(rc.title_line, rect(tx, rr.top + 10.0f, tw, 20.0f), Font::SmallSemi, sel ? cv.theme().on_primary_container : cv.theme().on_surface);
            cv.text(rc.line2, rect(tx, rr.top + 32.0f, tw, 17.0f), Font::Caption, cv.theme().on_surface_variant);
            cv.text(rc.ts, rect(tx, rr.top + 51.0f, tw, 17.0f), Font::Caption, cv.theme().outline);

            cv.hit(rr, IdRow, i);
        }
        cv.pop_clip();

        // Scrollbar
        const float ms = max_scroll();
        if (ms > 0.0f) {
            const float track_h = height(list_r) - 8.0f;
            const float total_h = static_cast<float>(entries_.size()) * step + 12.0f;
            const float thumb_h = std::max(28.0f, track_h * height(list_r) / total_h);
            const float ty = list_r.top + 4.0f + ((track_h - thumb_h) * (scroll_ / ms));
            const float ha = dragging_scroll_ ? 1.0f : hover_.alpha(IdScrollbar);
            scroll_thumb_ = D2D1::RectF(list_r.right - lerp(6.0f, 8.0f, ha), ty, list_r.right - 2.0f, ty + thumb_h);
            cv.fill_round(scroll_thumb_, 3.0f, with_alpha(cv.theme().on_surface_variant, lerp(0.28f, 0.55f, ha)));
            cv.hit(D2D1::RectF(list_r.right - 12.0f, list_r.top, list_r.right, list_r.bottom), IdScrollbar);
        }
    }

    void draw_preview_pane(Canvas& cv, D2D1_RECT_F area) {
        const CaptureEntry* e = selected_entry();
        if (!e) {
            cv.icon(glyph::photo, rect(area.left, area.top + (height(area) * 0.5f) - 42.0f, width(area), 36.0f), cv.theme().outline, Font::IconLarge);
            cv.text(L"Select a capture to preview", rect(area.left, area.top + (height(area) * 0.5f), width(area), 24.0f),
                    Font::Body, cv.theme().outline, Align::Center);
            return;
        }

        const bool playable_video = is_playable_video(*e);
        const float footer_h = 68.0f;
        const float video_bar_h = playable_video ? 44.0f : 0.0f;
        const D2D1_RECT_F stage = D2D1::RectF(area.left + 18.0f, area.top + 18.0f, area.right - 18.0f, area.bottom - footer_h - video_bar_h - 12.0f);
        preview_rect_ = stage;

        cv.fill_round(stage, 12.0f, with_alpha(cv.theme().surface_lowest, 0.85f));
        cv.stroke_round(stage, 12.0f, with_alpha(cv.theme().outline_variant, 0.55f));

        const D2D1_RECT_F media_box = inset(stage, 10.0f, 10.0f);
        if (playable_video) {
            if (video_.has_frame() && video_.bitmap()) {
                const FitBox fb = compute_preview_box(video_.native_width(), video_.native_height(), e->width, e->height, target_.scale(),
                                                      {.left = media_box.left, .top = media_box.top, .width = width(media_box), .height = height(media_box)});
                video_.set_output_size(static_cast<uint32_t>(std::lround(fb.width * target_.scale())),
                                       static_cast<uint32_t>(std::lround(fb.height * target_.scale())));
                cv.bitmap(video_.bitmap(), rect(fb.left, fb.top, fb.width, fb.height));
            } else if (ID2D1Bitmap1* tb = sh_.thumbs.get(static_cast<uint64_t>(e->id))) {
                cv.fill_bitmap_round(tb, media_box, 8.0f, ThumbFit::Contain);
            } else {
                cv.text(video_.failed() ? L"Video preview unavailable — double-click to open externally" : L"Loading video…",
                        media_box, Font::Body, cv.theme().outline, Align::Center);
            }
            cv.hit(stage, IdPreviewArea);

            // Video controls bar
            const D2D1_RECT_F vb = D2D1::RectF(area.left + 18.0f, stage.bottom + 8.0f, area.right - 18.0f, stage.bottom + 44.0f);
            cv.fill_round(vb, 10.0f, with_alpha(cv.theme().surface, 0.92f));
            cv.stroke_round(vb, 10.0f, with_alpha(cv.theme().outline_variant, 0.6f));

            draw_button(cv, hover_.alpha(IdVideoPlay), rect(vb.left + 6.0f, vb.top + 4.0f, 32.0f, 28.0f),
                        video_.is_playing() ? glyph::pause : glyph::play, L"", IdVideoPlay, 0, 3);
            draw_button(cv, hover_.alpha(IdVideoMute), rect(vb.right - 38.0f, vb.top + 4.0f, 32.0f, 28.0f),
                        video_.is_muted() ? glyph::mute : glyph::volume, L"", IdVideoMute, 0, 3);

            const double dur = video_.duration();
            const double cur = video_.current_time();
            const float ratio = dur > 0.0 ? static_cast<float>(std::clamp(cur / dur, 0.0, 1.0)) : 0.0f;

            FixedWString<48> time_lbl = format_duration_ms(static_cast<uint64_t>(cur * 1000.0));
            time_lbl.append(L" / ");
            time_lbl.append(format_duration_ms(static_cast<uint64_t>(dur * 1000.0)));
            const float time_w = 96.0f;
            cv.text(time_lbl, rect(vb.right - 44.0f - time_w, vb.top, time_w, height(vb)), Font::MonoSmall, cv.theme().on_surface_variant, Align::Right);

            scrub_rect_ = D2D1::RectF(vb.left + 46.0f, vb.top + 4.0f, vb.right - 52.0f - time_w, vb.bottom - 4.0f);
            if (width(scrub_rect_) > 30.0f) {
                draw_slider(cv, hover_.alpha(IdVideoScrub), scrub_rect_, ratio, scrubbing_video_, IdVideoScrub);
            }
        } else {
            const uint64_t tid = static_cast<uint64_t>(e->id);
            ID2D1Bitmap1* bmp = sh_.previews.get(tid);
            if (!bmp) {
                sh_.previews.request(tid, row_cache_[static_cast<size_t>(selected_idx_)].wpath, 2048);
                bmp = sh_.thumbs.get(tid);
            }
            if (bmp) {
                cv.fill_bitmap_round(bmp, media_box, 8.0f, ThumbFit::Contain);
            } else {
                const bool missing = sh_.previews.failed(tid);
                cv.text(missing ? L"Image file not found on disk" : L"Loading preview…", media_box, Font::Body, cv.theme().outline, Align::Center);
            }
            cv.hit(stage, IdPreviewArea);
        }

        // Bottom metadata & action footer
        const D2D1_RECT_F foot = D2D1::RectF(area.left + 18.0f, area.bottom - footer_h, area.right - 18.0f, area.bottom - 12.0f);
        cv.fill_round(foot, 10.0f, with_alpha(cv.theme().surface, 0.90f));
        cv.stroke_round(foot, 10.0f, with_alpha(cv.theme().outline_variant, 0.6f));

        const float del_w = button_width(cv, L"Delete", true, 84.0f);
        const float exp_w = button_width(cv, L"Folder", true, 84.0f);
        const float open_w = button_width(cv, L"Open", true, 78.0f);
        const float copy_w = button_width(cv, L"Copy", true, 78.0f);

        float bx = foot.right - 10.0f;
        const float by = foot.top + ((height(foot) - 32.0f) * 0.5f);
        bx -= del_w;
        draw_button(cv, hover_.alpha(IdActDelete), rect(bx, by, del_w, 32.0f), glyph::del, L"Delete", IdActDelete, 0, 2);
        bx -= 6.0f + exp_w;
        draw_button(cv, hover_.alpha(IdActExplorer), rect(bx, by, exp_w, 32.0f), glyph::folder, L"Folder", IdActExplorer, 0, 3);
        bx -= 6.0f + open_w;
        draw_button(cv, hover_.alpha(IdActOpen), rect(bx, by, open_w, 32.0f), glyph::open, L"Open", IdActOpen, 0, 0);
        bx -= 6.0f + copy_w;
        draw_button(cv, hover_.alpha(IdActCopy), rect(bx, by, copy_w, 32.0f), glyph::copy, L"Copy", IdActCopy, 0, 1);

        const float info_w = std::max(40.0f, bx - foot.left - 18.0f);
        const RowCache& sel_rc = row_cache_[static_cast<size_t>(selected_idx_)];
        std::wstring top_line;
        if (!sel_rc.window_title.empty()) {
            top_line = ulp::str::concat_strings(sel_rc.window_title, L"  —  ", sel_rc.filename);
        } else {
            top_line = sel_rc.filename;
        }
        cv.text(top_line, rect(foot.left + 12.0f, foot.top + 8.0f, info_w, 20.0f), Font::SmallSemi, cv.theme().on_surface);

        std::wstring sub;
        if (!sel_rc.process_name.empty()) {
            append_to(sub, sel_rc.process_name, L"  •  ");
        }
        sub += (e->mode == CaptureMode::Full ? L"Full Screen" : (e->mode == CaptureMode::Region ? L"Region" : L"Recording"));
        if (!sel_rc.format.empty()) {
            append_to(sub, L"  •  ", sel_rc.format);
        }
        if (e->width > 0 && e->height > 0) {
            append_to(sub, L"  •  ", to_fixed_wstr<16>(e->width), L"×", to_fixed_wstr<16>(e->height));
        }
        if (e->size_bytes > 0) {
            append_to(sub, L"  •  ", format_size(e->size_bytes));
        }
        if (!sel_rc.ts.empty()) {
            append_to(sub, L"  •  ", sel_rc.ts);
        }
        cv.text(sub, rect(foot.left + 12.0f, foot.top + 28.0f, info_w, 18.0f), Font::Caption, cv.theme().on_surface_variant);
        if (info_w > 40.0f) {
            cv.hit(D2D1::RectF(foot.left + 8.0f, foot.top + 4.0f, foot.left + 12.0f + info_w, foot.bottom - 4.0f), IdFooterInfo);
        }
    }

    // ------------------------------------------------------------------------------------------
    // Actions, Context Menu & OLE Drag-and-Drop
    // ------------------------------------------------------------------------------------------

    void open_entry(int idx) {
        if (idx < 0 || std::cmp_greater_equal(idx, entries_.size())) return;
        (void)open_file(to_wide(entries_[static_cast<size_t>(idx)].path));
    }

    void copy_entry(int idx) {
        if (idx < 0 || std::cmp_greater_equal(idx, entries_.size())) return;
        const CaptureEntry& e = entries_[static_cast<size_t>(idx)];
        (void)copy_capture_to_clipboard(to_wide(e.path), e.kind == CaptureKind::Image);
    }

    void delete_entry(int idx, bool delete_file) {
        if (idx < 0 || std::cmp_greater_equal(idx, entries_.size())) return;
        const CaptureEntry e = entries_[static_cast<size_t>(idx)];
        const std::wstring wpath = to_wide(e.path);
        if (e.kind == CaptureKind::Video && video_.path() == wpath) {
            video_.close();
        }
        if (delete_file) {
            (void)delete_file_to_recycle_bin(wpath);
        }
        (void)sh_.history.remove(e.id);
        sh_.thumbs.evict(static_cast<uint64_t>(e.id));
        sh_.previews.evict(static_cast<uint64_t>(e.id));

        const int next_sel = std::min(idx, static_cast<int>(entries_.size()) - 2);
        reload_entries(false);
        if (next_sel >= 0 && std::cmp_less(next_sel, entries_.size())) {
            selected_idx_ = next_sel;
            sync_preview_media();
        }
        invalidate();
    }

    void show_context_menu(int idx) {
        if (idx < 0 || std::cmp_greater_equal(idx, entries_.size())) return;
        selected_idx_ = idx;
        sync_preview_media();
        render();

        const CaptureEntry& e = entries_[static_cast<size_t>(idx)];
        const wil::unique_hmenu menu{CreatePopupMenu()};
        AppendMenuW(menu.get(), MF_STRING, CmdOpen, L"Open in Default App");
        AppendMenuW(menu.get(), MF_STRING, CmdCopy, e.kind == CaptureKind::Image ? L"Copy Image" : L"Copy Video File");
        AppendMenuW(menu.get(), MF_STRING, CmdCopyPath, L"Copy File Path");
        AppendMenuW(menu.get(), MF_STRING, CmdExplorer, L"Show in Explorer");
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu.get(), MF_STRING, CmdRemoveHistory, L"Remove from History");
        AppendMenuW(menu.get(), MF_STRING, CmdDeleteFile, L"Delete File & History");
        SetMenuDefaultItem(menu.get(), CmdOpen, FALSE);

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, nullptr));
        PostMessageW(hwnd_, WM_NULL, 0, 0);

        switch (cmd) {
            case CmdOpen: open_entry(idx); break;
            case CmdCopy: copy_entry(idx); break;
            case CmdCopyPath: (void)set_clipboard_text(hwnd_, to_wide(e.path)); break;
            case CmdExplorer: (void)reveal_in_explorer(to_wide(e.path)); break;
            case CmdRemoveHistory: delete_entry(idx, false); break;
            case CmdDeleteFile: delete_entry(idx, true); break;
            default: break;
        }
    }

    void start_drag_out(int idx) {
        if (dragging_out_ || idx < 0 || std::cmp_greater_equal(idx, entries_.size())) return;
        drag_pending_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();

        const std::array<std::wstring, 1> paths = {to_wide(entries_[static_cast<size_t>(idx)].path)};
        wil::com_ptr<AsyncDataObject> async_drop;
        {
            dragging_out_ = true;
            const auto guard = wil::scope_exit([&] { dragging_out_ = false; });
            async_drop = drag_media(hwnd_, paths);
        }
        if (async_drop) {
            async_drops_.push_back(std::move(async_drop));
            ++sh_.background_jobs;
            SetTimer(hwnd_, TIMER_DROP, 250, nullptr);
        }
        invalidate();
    }

    // ------------------------------------------------------------------------------------------
    // Input
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] D2D1_POINT_2F to_dip(LPARAM lp) const noexcept {
        const float s = target_.scale();
        return {.x = GET_X_LPARAM(lp) / s, .y = GET_Y_LPARAM(lp) / s};
    }

    void scrub_to_x(float x) noexcept {
        const float pad = 8.0f;
        const float l = scrub_rect_.left + pad;
        const float r = std::max(l + 4.0f, scrub_rect_.right - pad);
        const float ratio = std::clamp((x - l) / (r - l), 0.0f, 1.0f);
        video_.seek(static_cast<double>(ratio) * video_.duration());
        invalidate();
    }

    void on_lbutton_down(LPARAM lp) {
        const D2D1_POINT_2F p = to_dip(lp);
        const Hit* h = hit_test(hits_, p);
        if (!h) {
            set_search_focus(false);
            return;
        }
        if (h->id != IdSearchBox && h->id != IdSearchClear) {
            set_search_focus(false);
        }

        switch (h->id) {
            case IdSearchBox: {
                set_search_focus(true);
                reset_caret_blink();
                if (target_.ready()) {
                    std::vector<Hit> dummy;
                    Canvas cv{target_, sh_.fonts, dummy, default_theme(), std::nullopt};
                    search_field_.click(cv, p.x, GetKeyState(VK_SHIFT) < 0);
                }
                dragging_search_ = true;
                SetCapture(hwnd_);
                invalidate();
                return;
            }
            case IdSearchClear:
                search_field_.set({});
                reset_caret_blink();
                apply_search_filter();
                return;
            case IdBtnFull: if (sh_.on_capture_full) sh_.on_capture_full(); return;
            case IdBtnRegion: if (sh_.on_capture_region) sh_.on_capture_region(); return;
            case IdBtnRecord: if (sh_.on_toggle_record) sh_.on_toggle_record(); return;
            case IdBtnSettings: if (sh_.on_open_settings) sh_.on_open_settings(0); return;
            case IdFilterAll: filter_ = Filter::All; reload_entries(true); return;
            case IdFilterImages: filter_ = Filter::Images; reload_entries(true); return;
            case IdFilterVideos: filter_ = Filter::Videos; reload_entries(true); return;
            case IdActCopy: copy_entry(selected_idx_); return;
            case IdActOpen: open_entry(selected_idx_); return;
            case IdActExplorer:
                if (const auto* e = selected_entry()) (void)reveal_in_explorer(to_wide(e->path));
                return;
            case IdActDelete: delete_entry(selected_idx_, true); return;
            case IdVideoPlay:
                if (!video_.active() || video_.failed()) {
                    sync_preview_media();
                } else {
                    video_.toggle_play();
                    if (video_.needs_timer()) SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
                }
                invalidate();
                return;
            case IdVideoMute:
                video_.toggle_mute();
                sh_.settings.video_muted = video_.is_muted();
                sh_.save();
                invalidate();
                return;
            case IdVideoScrub:
                scrubbing_video_ = true;
                video_.begin_scrub();
                SetCapture(hwnd_);
                scrub_to_x(p.x);
                return;
            case IdScrollbar: {
                dragging_scroll_ = true;
                SetCapture(hwnd_);
                scroll_drag_offset_ = p.y - scroll_thumb_.top;
                return;
            }
            case IdRow: {
                selected_idx_ = h->index;
                sync_preview_media();
                drag_pending_ = true;
                drag_idx_ = h->index;
                drag_start_px_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                SetCapture(hwnd_);
                invalidate();
                return;
            }
            case IdPreviewArea: {
                if (selected_idx_ >= 0) {
                    drag_pending_ = true;
                    drag_idx_ = selected_idx_;
                    drag_start_px_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    SetCapture(hwnd_);
                }
                return;
            }
            default:
                break;
        }
    }

    void on_lbutton_dblclk(LPARAM lp) {
        const Hit* h = hit_test(hits_, to_dip(lp));
        if (!h) return;
        if (h->id == IdRow) {
            open_entry(h->index);
        } else if (h->id == IdPreviewArea) {
            open_entry(selected_idx_);
        }
    }

    void on_rbutton_up(LPARAM lp) {
        const Hit* h = hit_test(hits_, to_dip(lp));
        if (!h) return;
        if (h->id == IdRow) {
            show_context_menu(h->index);
        } else if (h->id == IdPreviewArea && selected_idx_ >= 0) {
            show_context_menu(selected_idx_);
        }
    }

    void on_mouse_move(LPARAM lp) {
        const D2D1_POINT_2F p = to_dip(lp);
        if (dragging_search_) {
            if (target_.ready()) {
                std::vector<Hit> dummy;
                Canvas cv{target_, sh_.fonts, dummy, default_theme(), std::nullopt};
                search_field_.click(cv, p.x, true);
                invalidate();
            }
            return;
        }
        if (scrubbing_video_) {
            scrub_to_x(p.x);
            return;
        }
        if (dragging_scroll_) {
            const float track_h = std::max(1.0f, height(list_rect_) - 8.0f - height(scroll_thumb_));
            const float rel = std::clamp((p.y - scroll_drag_offset_ - (list_rect_.top + 4.0f)) / track_h, 0.0f, 1.0f);
            scroll_ = target_scroll_ = rel * max_scroll();
            invalidate();
            return;
        }
        if (drag_pending_) {
            const int dx = std::abs(GET_X_LPARAM(lp) - drag_start_px_.x);
            const int dy = std::abs(GET_Y_LPARAM(lp) - drag_start_px_.y);
            if (dx >= GetSystemMetrics(SM_CXDRAG) || dy >= GetSystemMetrics(SM_CYDRAG)) {
                start_drag_out(drag_idx_);
                return;
            }
        }
        const Hit* h = hit_test(hits_, p);
        const int nid = h ? h->id : IdNone;
        const int nidx = h ? h->index : 0;
        if (nid != raw_hover_id_ || nidx != raw_hover_index_) {
            raw_hover_id_ = nid;
            raw_hover_index_ = nidx;
            const int vis_id = (nid == IdFooterInfo) ? IdNone : nid;
            const int vis_idx = (nid == IdFooterInfo) ? 0 : nidx;
            const bool vis_changed = hover_.set(vis_id, vis_idx, true);
            tooltip_.on_hover_changed(hwnd_, TIMER_TOOLTIP, h, has_tooltip(nid, nidx), true);
            if (vis_changed || tooltip_.alpha.active()) start_anim();
        }
        if (h && h->id == IdSearchBox) SetCursor(cursor_ibeam_);
        else SetCursor((h && h->id != IdPreviewArea && h->id != IdFooterInfo) ? cursor_hand_ : cursor_arrow_);
        TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd_};
        TrackMouseEvent(&tme);
    }

    void on_lbutton_up() {
        drag_pending_ = false;
        dragging_scroll_ = false;
        dragging_search_ = false;
        if (scrubbing_video_) {
            scrubbing_video_ = false;
            video_.end_scrub();
            if (video_.needs_timer()) SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
            invalidate();
        }
        if (GetCapture() == hwnd_) ReleaseCapture();
    }

    void on_char(wchar_t ch) {
        if (!search_field_.focused) return;
        if (ch < 0x20 || ch == 0x7F) return;
        const std::wstring_view sv{&ch, 1};
        if (search_field_.replace_selection(sv)) {
            reset_caret_blink();
            apply_search_filter();
        }
    }

    void on_key_down(UINT vk) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && vk == 'F') {
            set_search_focus(true);
            search_field_.select_all();
            reset_caret_blink();
            invalidate();
            return;
        }
        if (vk == VK_UP && selected_idx_ > 0) {
            --selected_idx_;
            sync_preview_media();
            ensure_row_visible(selected_idx_);
            invalidate();
            return;
        }
        if (vk == VK_DOWN && selected_idx_ + 1 < static_cast<int>(entries_.size())) {
            ++selected_idx_;
            sync_preview_media();
            ensure_row_visible(selected_idx_);
            invalidate();
            return;
        }
        if (search_field_.focused) {
            if (vk == VK_ESCAPE) {
                if (!search_field_.text.empty()) {
                    search_field_.set({});
                    apply_search_filter();
                } else {
                    set_search_focus(false);
                }
                return;
            }
            if (vk == VK_RETURN && selected_idx_ >= 0) {
                open_entry(selected_idx_);
                return;
            }
            bool handled = false;
            const bool changed = search_field_.on_key(hwnd_, vk, handled);
            if (handled) {
                reset_caret_blink();
                if (changed) apply_search_filter();
                else invalidate();
                return;
            }
        }
        if (vk == VK_ESCAPE) { hide(); return; }
        if (vk == VK_RETURN && selected_idx_ >= 0) { open_entry(selected_idx_); return; }
        if (vk == VK_DELETE && selected_idx_ >= 0) { delete_entry(selected_idx_, true); return; }
        if (ctrl && vk == 'C' && selected_idx_ >= 0) { copy_entry(selected_idx_); return; }
        if (vk == VK_SPACE && selected_entry() && is_playable_video(*selected_entry())) {
            if (!video_.active() || video_.failed()) {
                sync_preview_media();
            } else {
                video_.toggle_play();
                if (video_.needs_timer()) SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
            }
            invalidate();
        }
    }

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<HistoryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->handle(hwnd, msg, wp, lp);
    }

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_CLOSE:
                hide();
                return 0;
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
                    clamp_scroll();
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
                    // Drain due timers while QS_PAINT is clear so VSync frames never starve timers.
                    MSG m;
                    while (PeekMessageW(&m, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) {
                        DispatchMessageW(&m);
                    }
                    if (visible() && anim_active_) InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_APP_THUMBS:
                on_thumbs_ready();
                return 0;
            case WM_APP_VIDEO:
                if (video_.process_events()) {
                    if (video_.needs_timer()) SetTimer(hwnd, TIMER_VIDEO, 16, nullptr);
                    invalidate();
                }
                return 0;
            case WM_TIMER:
                if (wp == TIMER_VIDEO) {
                    if (!visible() || !video_.active()) {
                        KillTimer(hwnd, TIMER_VIDEO);
                    } else if (target_.ready()) {
                        const auto st = video_.tick(sh_.gfx.d3d(), sh_.gfx.epoch(), target_.dc());
                        if (st == MediaTickStatus::FrameReady || video_.is_playing()) invalidate();
                        if (!video_.needs_timer()) KillTimer(hwnd, TIMER_VIDEO);
                    }
                } else if (wp == TIMER_DROP) {
                    std::erase_if(async_drops_, [&](const wil::com_ptr<AsyncDataObject>& o) {
                        if (!o || !o->is_operation_active()) {
                            if (sh_.background_jobs > 0) --sh_.background_jobs;
                            return true;
                        }
                        return false;
                    });
                    if (async_drops_.empty()) KillTimer(hwnd, TIMER_DROP);
                } else if (wp == TIMER_CARET) {
                    if (search_field_.focused && visible()) {
                        search_field_.caret_on = !search_field_.caret_on;
                        invalidate();
                    } else {
                        KillTimer(hwnd, TIMER_CARET);
                    }
                } else if (wp == TIMER_TOOLTIP) {
                    if (!visible() || !window_active_) {
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
            case WM_MOUSEWHEEL: {
                dismiss_tooltip();
                const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f;
                target_scroll_ = std::clamp(target_scroll_ - (delta * 96.0f), 0.0f, max_scroll());
                start_anim();
                return 0;
            }
            case WM_LBUTTONDOWN: dismiss_tooltip(); on_lbutton_down(lp); return 0;
            case WM_LBUTTONDBLCLK: dismiss_tooltip(); on_lbutton_dblclk(lp); return 0;
            case WM_LBUTTONUP: on_lbutton_up(); return 0;
            case WM_RBUTTONUP: dismiss_tooltip(); on_rbutton_up(lp); return 0;
            case WM_MOUSEMOVE: on_mouse_move(lp); return 0;
            case WM_MOUSELEAVE: {
                raw_hover_id_ = IdNone;
                raw_hover_index_ = 0;
                tooltip_.clear(hwnd_, TIMER_TOOLTIP, true);
                if (hover_.set(IdNone, 0, true) || tooltip_.alpha.active()) start_anim();
                return 0;
            }
            case WM_ACTIVATE:
                window_active_ = (LOWORD(wp) != WA_INACTIVE);
                if (!window_active_) dismiss_tooltip();
                break;
            case WM_DESTROY:
                KillTimer(hwnd, TIMER_VIDEO);
                KillTimer(hwnd, TIMER_DROP);
                KillTimer(hwnd, TIMER_CARET);
                KillTimer(hwnd, TIMER_TOOLTIP);
                return 0;
            case WM_KEYDOWN: dismiss_tooltip(); on_key_down(static_cast<UINT>(wp)); return 0;
            case WM_CHAR: on_char(static_cast<wchar_t>(wp)); return 0;
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
    VideoPlayer video_;

    Filter filter_ = Filter::All;
    TextField search_field_;
    D2D1_RECT_F search_box_rect_{};
    D2D1_RECT_F search_text_rect_{};
    bool dragging_search_ = false;
    std::vector<CaptureEntry> all_entries_;
    std::vector<RowCache> all_cache_;
    std::vector<CaptureEntry> entries_;
    std::vector<RowCache> row_cache_;
    int selected_idx_ = -1;

    std::vector<Hit> hits_;
    HoverTracker hover_;
    int raw_hover_id_ = IdNone;
    int raw_hover_index_ = 0;
    bool window_active_ = true;
    HoverTooltip tooltip_;
    float scroll_ = 0.0f;
    float target_scroll_ = 0.0f;
    D2D1_RECT_F list_rect_{};
    D2D1_RECT_F scroll_thumb_{};
    D2D1_RECT_F preview_rect_{};
    D2D1_RECT_F scrub_rect_{};

    bool dragging_scroll_ = false;
    bool in_sizemove_ = false;
    float scroll_drag_offset_ = 0.0f;
    bool scrubbing_video_ = false;
    bool drag_pending_ = false;
    bool dragging_out_ = false;
    int drag_idx_ = -1;
    POINT drag_start_px_{};
    std::vector<wil::com_ptr<AsyncDataObject>> async_drops_;

    bool anim_active_ = false;
    bool rendering_ = false;
    bool full_dirty_ = true;
    std::chrono::steady_clock::time_point last_anim_{};
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    HCURSOR cursor_ibeam_ = nullptr;
};

} // namespace capusy::ui
