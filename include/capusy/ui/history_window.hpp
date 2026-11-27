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
        async_drops_.clear();
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
        KillTimer(hwnd_, TIMER_RELEASE_TARGET);
        if (!sh_.gfx.ensure_device(hwnd_)) return;
        if (!target_.ready() && !target_.attach(sh_.gfx, hwnd_)) {
            if (sh_.on_visibility_changed) sh_.on_visibility_changed();
            return;
        }
        sh_.thumbs.start(hwnd_, WM_APP_THUMBS, 2);
        sh_.previews.start(hwnd_, WM_APP_THUMBS, 1);

        reload_entries(true);
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
        sync_preview_media();
        sync_caret_timer();
        anim_active_ = false;
        full_dirty_ = true;
        pending_dirty_.clear();
        render();
        ValidateRect(hwnd_, nullptr);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void hide() {
        if (!hwnd_ || dragging_out_) return;
        // Hide via SW_HIDE without cloaking so Windows plays its native DWM close animation,
        // and defer releasing the DComp swapchain until after the animation finishes.
        const bool was_visible = visible();
        if (was_visible) {
            ShowWindow(hwnd_, SW_HIDE);
        }
        KillTimer(hwnd_, TIMER_VIDEO);
        if (async_drops_.empty()) KillTimer(hwnd_, TIMER_DROP);
        KillTimer(hwnd_, TIMER_CARET);
        KillTimer(hwnd_, TIMER_SEARCH);
        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
        last_tooltip_dirty_.reset();
        raw_hover_id_ = IdNone;
        raw_hover_index_ = 0;
        set_search_focus(false);
        search_field_.clear_cached_layout();
        dragging_search_ = false;
        deferred_select_idx_ = -1;
        video_.shutdown();
        sh_.previews.clear();
        sh_.thumbs.clear();
        page_cache_.clear();
        page_cache_.shrink_to_fit();
        search_ids_.clear();
        search_ids_.shrink_to_fit();
        hits_.clear();
        hits_.shrink_to_fit();
        scratch_hits_.clear();
        scratch_hits_.shrink_to_fit();
        pending_dirty_.clear();
        pending_dirty_.shrink_to_fit();
        dirty_dips_scratch_.clear();
        dirty_dips_scratch_.shrink_to_fit();
        focused_row_.reset();
        total_count_ = 0;
        unfiltered_count_ = 0;
        if (was_visible) {
            SetTimer(hwnd_, TIMER_RELEASE_TARGET, 400, nullptr);
        } else {
            KillTimer(hwnd_, TIMER_RELEASE_TARGET);
            target_.release();
        }
        dragging_scroll_ = false;
        in_sizemove_ = false;
        minimized_ = false;
        scrubbing_video_ = false;
        drag_pending_ = false;
        anim_active_ = false;
        full_dirty_ = true;
        hover_.set(IdNone, 0, false);
        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
    }

    void reload_entries(bool keep_selection = true, bool sync_media = true) {
        if (hwnd_) KillTimer(hwnd_, TIMER_SEARCH);
        const int64_t prev_id = (keep_selection && focused_row_) ? focused_row_->entry.id : -1;
        const auto filter = current_filter_kind();
        unfiltered_count_ = sh_.history.count(filter).value_or(0);
        apply_search_filter(prev_id, keep_selection, sync_media);
    }

    void apply_search_filter(int64_t preferred_id = -1, bool keep_selection = true, bool sync_media = true) {
        if (hwnd_) KillTimer(hwnd_, TIMER_SEARCH);
        if (preferred_id < 0 && keep_selection && focused_row_) {
            preferred_id = focused_row_->entry.id;
        }
        const std::wstring q = lower(trim(search_field_.text));
        const auto filter = current_filter_kind();

        page_cache_.clear();
        page_clock_ = 0;
        search_ids_.clear();

        if (q.empty()) {
            search_active_ = false;
            search_query_utf8_.clear();
            search_ids_.shrink_to_fit();
            total_count_ = static_cast<int>(std::min<uint64_t>(unfiltered_count_, static_cast<uint64_t>(std::numeric_limits<int>::max())));
        } else {
            search_active_ = true;
            search_query_utf8_ = to_utf8(q);
            if (auto ids = sh_.history.search_ids(search_query_utf8_, filter)) {
                search_ids_ = std::move(*ids);
            }
            total_count_ = static_cast<int>(std::min<size_t>(search_ids_.size(), static_cast<size_t>(std::numeric_limits<int>::max())));
        }

        reconcile_selection(keep_selection, preferred_id);
        deferred_select_idx_ = -1;

        if (sync_media) sync_preview_media();
        clamp_scroll();
        invalidate();
    }

    void on_thumbs_ready() {
        if (!window_can_render_live(visible(), minimized_) || !target_.ready()) return;
        const bool thumbs_changed = sh_.thumbs.process(target_.dc());
        const bool previews_changed = sh_.previews.process(target_.dc());
        if (thumbs_changed) {
            invalidate_rect(list_rect_);
            if (preview_rect_.right > preview_rect_.left && preview_rect_.bottom > preview_rect_.top) {
                invalidate_rect(preview_rect_);
            }
        }
        if (previews_changed) {
            if (preview_rect_.right > preview_rect_.left && preview_rect_.bottom > preview_rect_.top) {
                invalidate_rect(preview_rect_);
            } else {
                invalidate();
            }
        }
    }

    void reset_gpu() {
        video_.reset_gpu();
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

private:
    static constexpr float HEADER_H = 54.0f;
    static constexpr float SIDEBAR_W = 336.0f;
    static constexpr float SIDEBAR_TOP_H = 86.0f;
    static constexpr float ROW_H = 78.0f;
    static constexpr float ROW_GAP = 6.0f;
    static constexpr int PAGE_SIZE = 128;
    static constexpr size_t MAX_CACHED_PAGES = 16;
    static constexpr UINT_PTR TIMER_VIDEO = 2;
    static constexpr UINT_PTR TIMER_DROP = 3;
    static constexpr UINT_PTR TIMER_CARET = 4;
    static constexpr UINT_PTR TIMER_TOOLTIP = 5;
    static constexpr UINT_PTR TIMER_SEARCH = 6;
    static constexpr UINT_PTR TIMER_RELEASE_TARGET = 7;

    struct RowCache {
        std::wstring wpath;
        std::wstring filename;
        std::wstring window_title;
        std::wstring process_name;
        std::wstring format;
        std::wstring title_line;
        std::wstring line2;
        FixedWString<64> ts;
    };

    struct RowItem {
        CaptureEntry entry;
        RowCache cache;
        bool cache_built = false;
    };

    struct CachedPage {
        int page_index = -1;
        std::vector<RowItem> rows;
        uint64_t last_used = 0;
    };

    enum class Filter : uint8_t { All = 0, Images, Videos };

    enum HitId : uint8_t {
        IdNone = 0,
        IdBtnFull,
        IdBtnRegion,
        IdBtnFullRecord,
        IdBtnRecord,
        IdBtnColorPicker,
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

    enum CtxCmd : uint16_t {
        CmdOpen = 1001,
        CmdCopy,
        CmdCopyPath,
        CmdExplorer,
        CmdRemoveHistory,
        CmdDeleteFile,
    };

    [[nodiscard]] std::optional<CaptureKind> current_filter_kind() const noexcept {
        if (filter_ == Filter::Images) return CaptureKind::Image;
        if (filter_ == Filter::Videos) return CaptureKind::Video;
        return std::nullopt;
    }

    static void build_row_cache(const CaptureEntry& e, RowCache& rc) {
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

        rc.line2.clear();
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
    }

    [[nodiscard]] const RowItem* row_at(int idx) const {
        if (idx < 0 || idx >= total_count_) return nullptr;
        const int page_idx = idx / PAGE_SIZE;
        const int local_idx = idx % PAGE_SIZE;

        for (CachedPage& p : page_cache_) {
            if (p.page_index == page_idx) {
                p.last_used = ++page_clock_;
                if (local_idx < 0 || std::cmp_greater_equal(local_idx, p.rows.size())) return nullptr;
                RowItem& item = p.rows[static_cast<size_t>(local_idx)];
                if (!item.cache_built) {
                    build_row_cache(item.entry, item.cache);
                    item.cache_built = true;
                }
                return &item;
            }
        }

        // Cache miss: fetch one page from SQLite
        const int offset = page_idx * PAGE_SIZE;
        std::optional<std::vector<CaptureEntry>> fetched;
        if (search_active_) {
            if (offset >= 0 && std::cmp_less(offset, search_ids_.size())) {
                const size_t n = std::min<size_t>(static_cast<size_t>(PAGE_SIZE), search_ids_.size() - static_cast<size_t>(offset));
                fetched = sh_.history.get_by_ids(std::span<const int64_t>{search_ids_.data() + offset, n});
            }
        } else {
            fetched = sh_.history.list_page(PAGE_SIZE, offset, current_filter_kind());
        }
        if (!fetched) return nullptr;

        CachedPage page;
        page.page_index = page_idx;
        page.last_used = ++page_clock_;
        page.rows.reserve(fetched->size());
        for (CaptureEntry& e : *fetched) {
            RowItem ri;
            ri.entry = std::move(e);
            page.rows.push_back(std::move(ri));
        }

        CachedPage* slot = nullptr;
        if (page_cache_.size() < MAX_CACHED_PAGES) {
            page_cache_.push_back(std::move(page));
            slot = &page_cache_.back();
        } else {
            const int focus_page = (selection_.focus >= 0) ? (selection_.focus / PAGE_SIZE) : -1;
            size_t victim = 0;
            uint64_t oldest = std::numeric_limits<uint64_t>::max();
            for (size_t i = 0; i < page_cache_.size(); ++i) {
                if (page_cache_[i].page_index == focus_page && page_cache_.size() > 1) continue;
                if (page_cache_[i].last_used < oldest) {
                    oldest = page_cache_[i].last_used;
                    victim = i;
                }
            }
            page_cache_[victim] = std::move(page);
            slot = &page_cache_[victim];
        }

        if (local_idx < 0 || std::cmp_greater_equal(local_idx, slot->rows.size())) return nullptr;
        RowItem& item = slot->rows[static_cast<size_t>(local_idx)];
        build_row_cache(item.entry, item.cache);
        item.cache_built = true;
        return &item;
    }

    [[nodiscard]] std::optional<int64_t> id_at(int idx) const {
        if (idx < 0 || idx >= total_count_) return std::nullopt;
        if (search_active_) return search_ids_[static_cast<size_t>(idx)];
        if (const RowItem* item = row_at(idx)) return item->entry.id;
        return std::nullopt;
    }

    [[nodiscard]] std::vector<int64_t> fetch_ids_range(int lo, int hi) const {
        std::vector<int64_t> out;
        if (total_count_ <= 0) return out;
        lo = std::clamp(lo, 0, total_count_ - 1);
        hi = std::clamp(hi, 0, total_count_ - 1);
        if (lo > hi) return out;
        if (search_active_) {
            out.assign(search_ids_.begin() + lo, search_ids_.begin() + hi + 1);
            return out;
        }
        if (auto ids = sh_.history.list_ids_range(hi - lo + 1, lo, current_filter_kind())) {
            out = std::move(*ids);
        }
        return out;
    }

    [[nodiscard]] int find_index_of_id(int64_t id) const {
        if (id < 0 || total_count_ <= 0) return -1;
        if (search_active_) {
            for (size_t i = 0; i < search_ids_.size(); ++i) {
                if (search_ids_[i] == id) return static_cast<int>(i);
            }
            return -1;
        }
        if (auto idx = sh_.history.find_row_index(id, current_filter_kind())) {
            if (*idx >= 0 && *idx < total_count_) return *idx;
        }
        return -1;
    }

    void refresh_focused_row() {
        if (selection_.focus < 0 || selection_.focus >= total_count_) {
            focused_row_.reset();
            return;
        }
        if (const RowItem* item = row_at(selection_.focus)) {
            focused_row_ = *item;
        } else {
            focused_row_.reset();
        }
    }

    void select_single_idx(int idx) {
        if (idx < 0 || idx >= total_count_) {
            selection_.clear();
            focused_row_.reset();
            sel_summary_dirty_ = true;
            return;
        }
        const RowItem* item = row_at(idx);
        if (!item) {
            selection_.clear();
            focused_row_.reset();
            sel_summary_dirty_ = true;
            return;
        }
        selection_.ids.clear();
        selection_.ids.insert(item->entry.id);
        selection_.focus = idx;
        selection_.anchor = idx;
        selection_.anchor_id = item->entry.id;
        focused_row_ = *item;
        sel_summary_dirty_ = true;
    }

    bool click_row_idx(int idx, bool ctrl, bool shift) {
        if (idx < 0 || idx >= total_count_) return false;
        const auto cid_opt = id_at(idx);
        if (!cid_opt) return false;
        const int64_t cid = *cid_opt;

        if (shift) {
            const int base = (selection_.anchor >= 0 && selection_.anchor < total_count_)
                ? selection_.anchor
                : ((selection_.focus >= 0 && selection_.focus < total_count_) ? selection_.focus : idx);
            if (!ctrl) selection_.ids.clear();
            const int lo = std::min(base, idx);
            const int hi = std::max(base, idx);
            const auto range_ids = fetch_ids_range(lo, hi);
            selection_.ids.reserve(selection_.ids.size() + range_ids.size());
            for (const int64_t rid : range_ids) selection_.ids.insert(rid);
            selection_.anchor = base;
            selection_.anchor_id = id_at(base).value_or(-1);
            selection_.focus = idx;
            refresh_focused_row();
            sel_summary_dirty_ = true;
            return false;
        }

        if (ctrl) {
            if (selection_.ids.erase(cid) > 0) {
                selection_.anchor = idx;
                selection_.anchor_id = cid;
                if (selection_.focus == idx) {
                    int next_focus = -1;
                    if (!selection_.ids.empty()) {
                        // Prefer a nearby selected row first before falling back to any selected row
                        const int search_lo = std::max(0, idx - PAGE_SIZE);
                        const int search_hi = std::min(total_count_ - 1, idx + PAGE_SIZE);
                        int best_dist = std::numeric_limits<int>::max();
                        for (int k = search_lo; k <= search_hi; ++k) {
                            const auto kid = id_at(k);
                            if (kid && selection_.ids.contains(*kid)) {
                                const int d = std::abs(k - idx);
                                if (d < best_dist) {
                                    best_dist = d;
                                    next_focus = k;
                                }
                            }
                        }
                        if (next_focus < 0) {
                            next_focus = find_index_of_id(*selection_.ids.begin());
                        }
                    }
                    selection_.focus = next_focus;
                }
            } else {
                selection_.ids.insert(cid);
                selection_.focus = idx;
                selection_.anchor = idx;
                selection_.anchor_id = cid;
            }
            refresh_focused_row();
            sel_summary_dirty_ = true;
            return false;
        }

        if (selection_.ids.contains(cid) && selection_.ids.size() > 1) {
            return true;
        }

        select_single_idx(idx);
        return false;
    }

    bool move_selection(int delta, bool shift) {
        if (total_count_ <= 0) return false;
        const int last = total_count_ - 1;
        int next = selection_.focus;
        if (next < 0 || next > last) {
            next = delta >= 0 ? 0 : last;
        } else {
            next = std::clamp(next + delta, 0, last);
        }

        if (shift) {
            const int base = (selection_.anchor >= 0 && selection_.anchor <= last)
                ? selection_.anchor
                : ((selection_.focus >= 0 && selection_.focus <= last) ? selection_.focus : next);
            const int lo = std::min(base, next);
            const int hi = std::max(base, next);
            selection_.ids.clear();
            const auto range_ids = fetch_ids_range(lo, hi);
            selection_.ids.reserve(range_ids.size());
            for (const int64_t rid : range_ids) selection_.ids.insert(rid);
            selection_.anchor = base;
            selection_.anchor_id = id_at(base).value_or(-1);
            selection_.focus = next;
            refresh_focused_row();
            sel_summary_dirty_ = true;
            return true;
        }

        const auto next_id = id_at(next);
        const bool changed = (selection_.focus != next || selection_.ids.size() != 1 || !next_id || !selection_.ids.contains(*next_id));
        select_single_idx(next);
        return changed;
    }

    void select_all_rows() {
        selection_.ids.clear();
        if (total_count_ <= 0) {
            selection_.clear();
            focused_row_.reset();
            sel_summary_dirty_ = true;
            return;
        }
        const auto all_ids = fetch_ids_range(0, total_count_ - 1);
        selection_.ids.reserve(all_ids.size());
        for (const int64_t id : all_ids) selection_.ids.insert(id);
        if (selection_.focus < 0 || selection_.focus >= total_count_) selection_.focus = 0;
        if (selection_.anchor < 0 || selection_.anchor >= total_count_) {
            selection_.anchor = selection_.focus;
            selection_.anchor_id = id_at(selection_.anchor).value_or(-1);
        }
        refresh_focused_row();
        sel_summary_dirty_ = true;
    }

    void reconcile_selection(bool keep_selection, int64_t preferred_focus_id = -1) {
        if (total_count_ <= 0) {
            selection_.clear();
            focused_row_.reset();
            sel_summary_dirty_ = true;
            return;
        }
        if (!keep_selection) {
            selection_.ids.clear();
            selection_.anchor_id = -1;
        } else if (!selection_.ids.empty()) {
            if (search_active_) {
                std::unordered_set<int64_t> visible;
                visible.reserve(search_ids_.size());
                for (const int64_t id : search_ids_) visible.insert(id);
                std::erase_if(selection_.ids, [&](int64_t id) { return !visible.contains(id); });
            } else {
                std::vector<int64_t> cur(selection_.ids.begin(), selection_.ids.end());
                if (auto kept = sh_.history.filter_existing_ids(cur, current_filter_kind())) {
                    selection_.ids = std::move(*kept);
                } else {
                    selection_.ids.clear();
                }
            }
        }

        selection_.focus = -1;
        if (preferred_focus_id >= 0) {
            selection_.focus = find_index_of_id(preferred_focus_id);
        }
        if (selection_.focus < 0 && !selection_.ids.empty()) {
            selection_.focus = find_index_of_id(*selection_.ids.begin());
        }
        selection_.focus = std::max(selection_.focus, 0);
        if (selection_.ids.empty()) {
            if (const auto fid = id_at(selection_.focus)) selection_.ids.insert(*fid);
        }

        selection_.anchor = -1;
        if (selection_.anchor_id >= 0) {
            selection_.anchor = find_index_of_id(selection_.anchor_id);
        }
        if (selection_.anchor < 0) {
            selection_.anchor = selection_.focus;
            selection_.anchor_id = id_at(selection_.anchor).value_or(-1);
        }
        refresh_focused_row();
        sel_summary_dirty_ = true;
    }

    [[nodiscard]] const CaptureEntry* selected_entry() const noexcept {
        return focused_row_ ? &focused_row_->entry : nullptr;
    }

    [[nodiscard]] size_t selection_count() const noexcept {
        return selection_.count();
    }

    [[nodiscard]] std::vector<CaptureEntry> collect_selected_entries() const {
        if (selection_.empty()) return {};
        if (selection_.count() == 1 && focused_row_ && selection_.contains(focused_row_->entry.id)) {
            return {focused_row_->entry};
        }
        std::vector<int64_t> ids(selection_.ids.begin(), selection_.ids.end());
        if (auto entries = sh_.history.get_by_ids_ordered(ids)) {
            return std::move(*entries);
        }
        return {};
    }

    [[nodiscard]] const SelectionSummary& selection_summary() const {
        if (sel_summary_dirty_) {
            sel_summary_dirty_ = false;
            if (selection_.empty()) {
                sel_summary_ = {};
            } else if (selection_.count() == 1 && focused_row_) {
                sel_summary_ = {
                    .count = 1,
                    .image_count = focused_row_->entry.kind == CaptureKind::Video ? 0u : 1u,
                    .video_count = focused_row_->entry.kind == CaptureKind::Video ? 1u : 0u,
                    .total_bytes = focused_row_->entry.size_bytes,
                };
            } else {
                std::vector<int64_t> ids(selection_.ids.begin(), selection_.ids.end());
                sel_summary_ = sh_.history.summarize_ids(ids).value_or(SelectionSummary{});
            }
        }
        return sel_summary_;
    }

    void schedule_search_update() {
        const std::wstring q = trim(search_field_.text);
        if (q.empty() || unfiltered_count_ <= 3000u || !hwnd_) {
            apply_search_filter();
            return;
        }
        SetTimer(hwnd_, TIMER_SEARCH, 75, nullptr);
        invalidate();
    }

    [[nodiscard]] static bool is_playable_video(const CaptureEntry& e) noexcept {
        return e.kind == CaptureKind::Video && !is_gif_capture(e.format, e.path);
    }

    void sync_preview_media() {
        if (!focused_row_) {
            video_.close();
            if (hwnd_) KillTimer(hwnd_, TIMER_VIDEO);
            return;
        }
        const CaptureEntry& e = focused_row_->entry;
        const std::wstring& wpath = focused_row_->cache.wpath;
        if (is_playable_video(e)) {
            if (visible() && sh_.gfx.d3d()) {
                video_.open(wpath, sh_.gfx.d3d(), sh_.gfx.epoch(), true);
                SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
            }
        } else {
            video_.close();
            if (hwnd_) KillTimer(hwnd_, TIMER_VIDEO);
            sh_.previews.request(static_cast<uint64_t>(e.id), wpath, 2048);
        }
    }

    struct HeaderBtn {
        int id = IdNone;
        wchar_t icon = 0;
        const wchar_t* label = L"";
        int style = 0;
        float width = 0.0f;
        bool update_dot = false;
    };

    struct HeaderLayout {
        std::array<HeaderBtn, 6> btns{};
        size_t btn_count = 0;
        std::array<FlowPlacement, 7> placements{};
        float height = HEADER_H;
    };

    [[nodiscard]] HeaderLayout header_layout(float total_w) const {
        HeaderLayout out;
        const auto add_btn = [&](int id, wchar_t icon, const wchar_t* label, int style, float min_w, bool update_dot = false) {
            const float bw = std::max(min_w, sh_.fonts.measure(label, Font::SmallSemi) + 24.0f + 28.0f);
            out.btns[out.btn_count++] = HeaderBtn{
                .id = id, .icon = icon, .label = label, .style = style, .width = bw, .update_dot = update_dot,
            };
        };

        const wchar_t* rec_lbl = sh_.recording_active ? L"Stop Rec" : L"Record";
        add_btn(IdBtnFull, glyph::monitor, L"Full Screen", 1, 112.0f);
        add_btn(IdBtnRegion, glyph::crop, L"Region", 0, 92.0f);
        if (!sh_.recording_active) {
            add_btn(IdBtnFullRecord, glyph::video, L"Full Rec", 0, 96.0f);
        }
        add_btn(IdBtnRecord, sh_.recording_active ? glyph::stop : glyph::video, rec_lbl, sh_.recording_active ? 2 : 0, 96.0f);
        add_btn(IdBtnColorPicker, glyph::eyedropper, L"Color", 0, 78.0f);
        add_btn(IdBtnSettings, glyph::settings, L"Settings", 3, 98.0f, sh_.has_update_available());

        constexpr float pad_x = 14.0f;
        constexpr float pad_y = 11.0f;
        constexpr float item_h = 32.0f;
        constexpr float gap_x = 8.0f;
        constexpr float gap_y = 8.0f;
        constexpr float brand_w = 126.0f;
        const float avail = std::max(1.0f, total_w - (pad_x * 2.0f));

        float total_single = brand_w;
        for (size_t i = 0; i < out.btn_count; ++i) {
            total_single += gap_x + out.btns[i].width;
        }
        const bool single_line = (total_single <= avail + 0.001f);

        std::array<FlowItem, 7> items; // NOLINT(cppcoreguidelines-pro-type-member-init)
        items[0] = {.width = brand_w, .height = item_h, .grow = single_line};
        for (size_t i = 0; i < out.btn_count; ++i) {
            items[1 + i] = {.width = out.btns[i].width, .height = item_h, .grow = false};
        }
        const size_t n = 1 + out.btn_count;
        const float flow_h = flow_layout_into(
            std::span<const FlowItem>{items.data(), n}, gap_x, gap_y, avail,
            std::span<FlowPlacement>{out.placements.data(), n});
        out.height = flow_h + (pad_y * 2.0f);
        return out;
    }

    [[nodiscard]] double max_scroll() const {
        const double hdr_h = static_cast<double>(header_layout(target_.size().width).height);
        const double list_h = std::max(0.0, static_cast<double>(target_.size().height) - hdr_h - SIDEBAR_TOP_H);
        const double total_h = (total_count_ <= 0) ? 0.0 : ((static_cast<double>(total_count_) * (ROW_H + ROW_GAP)) + 12.0);
        return std::max(0.0, total_h - list_h);
    }

    void clamp_scroll() {
        const double ms = max_scroll();
        target_scroll_ = std::clamp(target_scroll_, 0.0, ms);
        scroll_ = std::clamp(scroll_, 0.0, ms);
    }

    void ensure_row_visible(int idx) {
        if (idx < 0) return;
        const double hdr_h = static_cast<double>(header_layout(target_.size().width).height);
        const double list_h = std::max(1.0, static_cast<double>(target_.size().height) - hdr_h - SIDEBAR_TOP_H);
        const double top = 6.0 + (static_cast<double>(idx) * (ROW_H + ROW_GAP));
        const double bot = top + ROW_H;
        if (top < target_scroll_) {
            target_scroll_ = top - 6.0;
        } else if (bot > target_scroll_ + list_h) {
            target_scroll_ = bot - list_h + 6.0;
        }
        clamp_scroll();
        start_anim();
    }

    void sync_caret_timer() noexcept {
        if (!hwnd_) return;
        const auto blink_ms = caret_blink_interval_ms(GetCaretBlinkTime());
        const CaretDecision dec = caret_timer_decision(visible(), minimized_, false, window_active_, search_field_.focused, blink_ms);
        search_field_.caret_on = dec.visible;
        if (dec.run_timer && blink_ms) {
            SetTimer(hwnd_, TIMER_CARET, *blink_ms, nullptr);
        } else {
            KillTimer(hwnd_, TIMER_CARET);
        }
    }

    void set_search_focus(bool focus) {
        if (search_field_.focused == focus) return;
        search_field_.focused = focus;
        sync_caret_timer();
        if (search_box_rect_.right > search_box_rect_.left) {
            invalidate_rect(inset(search_box_rect_, -3.0f, -3.0f));
        } else {
            invalidate();
        }
    }

    void reset_caret_blink() noexcept {
        sync_caret_timer();
    }

    void start_anim() noexcept {
        if (!window_can_render_live(visible(), minimized_)) return;
        if (!anim_active_) last_anim_ = std::chrono::steady_clock::now();
        anim_active_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void queue_hover_dirty(std::vector<D2D1_RECT_F>& out, int id, int idx, bool& full_dirty) const {
        if (id == IdNone) return;
        if (id == IdScrollbar) {
            if (list_rect_.right > list_rect_.left && list_rect_.bottom > list_rect_.top) {
                out.push_back(D2D1::RectF(list_rect_.right - 14.0f, list_rect_.top, list_rect_.right, list_rect_.bottom));
            } else {
                full_dirty = true;
            }
            return;
        }
        bool found = false;
        const float pad = (id == IdVideoScrub) ? 10.0f : 3.0f;
        for (const Hit& h : hits_) {
            if (h.id == id && h.index == idx) {
                out.push_back(D2D1::RectF(h.r.left - pad, h.r.top - pad, h.r.right + pad, h.r.bottom + pad));
                found = true;
            }
        }
        if (!found) full_dirty = true;
    }

    bool step_anim() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_anim_).count(), 0.001f, 0.05f);
        last_anim_ = now;

        bool moving = hover_.step(dt);
        if (tooltip_.step(dt)) moving = true;
        const double d = target_scroll_ - scroll_;
        if (std::abs(d) < 0.5) {
            scroll_ = target_scroll_;
        } else {
            scroll_ += d * (1.0 - std::exp(-static_cast<double>(dt) * 22.0));
            if (std::abs(target_scroll_ - scroll_) < 0.5) {
                scroll_ = target_scroll_;
            } else {
                moving = true;
            }
        }
        return moving;
    }

    void render() {
        if (!window_can_render_live(visible(), minimized_) || rendering_) return;
        if (!target_.ready()) {
            if (!sh_.gfx.ensure_device(hwnd_) || !target_.rebuild()) return;
            full_dirty_ = true;
        }
        rendering_ = true;
        const auto clear_r = wil::scope_exit([&] { rendering_ = false; });

        const double prev_scroll = scroll_;
        if (dragging_scroll_ || std::abs(target_scroll_ - scroll_) >= 0.5) {
            full_dirty_ = true;
        }

        if (video_.needs_recover() && sh_.gfx.d3d() && video_.recover_if_needed(sh_.gfx.d3d(), sh_.gfx.epoch())) {
            SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
            full_dirty_ = true;
        }
        if (video_.active() && target_.dc() &&
            video_.tick(sh_.gfx.d3d(), sh_.gfx.epoch(), target_.dc()) == MediaTickStatus::FrameReady && !full_dirty_) {
            if (preview_rect_.right > preview_rect_.left) pending_dirty_.push_back(preview_rect_);
            if (video_bar_rect_.right > video_bar_rect_.left) pending_dirty_.push_back(video_bar_rect_);
        }

        dirty_dips_scratch_.clear();
        if (!full_dirty_) {
            dirty_dips_scratch_.swap(pending_dirty_);
            if (hover_.in_anim.value != hover_.in_anim.target) {
                queue_hover_dirty(dirty_dips_scratch_, hover_.id, hover_.index, full_dirty_);
            }
            if (hover_.prev_id != IdNone && hover_.out_anim.value != hover_.out_anim.target) {
                queue_hover_dirty(dirty_dips_scratch_, hover_.prev_id, hover_.prev_index, full_dirty_);
            }
        } else {
            pending_dirty_.clear();
        }

        const bool anim = step_anim();
        if (scroll_ != prev_scroll) {
            full_dirty_ = true;
        }

        std::optional<D2D1_RECT_F> curr_tip_dirty;
        if (tooltip_.visible()) {
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
            sh_.thumbs.reset_gpu();
            sh_.previews.reset_gpu();
            video_.reset_gpu();
            invalidate();
        }
    }

    void draw(Canvas& cv) {
        const D2D1_SIZE_F sz = target_.size();
        cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, sz.height), with_alpha(cv.theme().bg, cv.theme().window_bg_alpha));

        const HeaderLayout hl = header_layout(sz.width);
        draw_header(cv, sz, hl);

        const float body_top = hl.height;
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
            case IdBtnFullRecord:
            case IdBtnRecord:
            case IdBtnColorPicker:
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
                return idx >= 0 && idx < total_count_;
            case IdFooterInfo:
                return focused_row_.has_value();
            default:
                return false;
        }
    }

    bool build_tooltip(int id, int idx, std::wstring& title, std::wstring& subtitle) const {
        title.clear();
        subtitle.clear();
        const size_t sel_n = selection_count();
        switch (id) {
            case IdBtnFull:
                title = L"Capture full screen";
                subtitle = hotkey_subtitle(HotkeyAction::FullScreenshot);
                return true;
            case IdBtnRegion:
                title = L"Capture region";
                subtitle = hotkey_subtitle(HotkeyAction::RegionScreenshot);
                return true;
            case IdBtnFullRecord:
                title = L"Start full-screen recording (tray controls)";
                subtitle = hotkey_subtitle(HotkeyAction::FullRecord);
                return true;
            case IdBtnRecord:
                title = sh_.recording_active ? L"Stop recording" : L"Start region recording";
                subtitle = hotkey_subtitle(HotkeyAction::RegionRecord);
                return true;
            case IdBtnColorPicker:
                title = L"Pick screen color";
                subtitle = hotkey_subtitle(HotkeyAction::ColorPicker);
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
                if (const RowItem* item = row_at(idx)) {
                    const RowCache& rc = item->cache;
                    title = !rc.window_title.empty()
                        ? ulp::str::concat_strings(rc.window_title, L"  —  ", rc.filename)
                        : rc.filename;
                    subtitle = rc.wpath;
                    return !title.empty() || !subtitle.empty();
                }
                return false;
            case IdFooterInfo:
                if (sel_n > 1) {
                    title = ulp::str::concat_strings(format_count(sel_n).view(), L" captures selected");
                    subtitle = L"Ctrl+click to toggle, Shift+click for range, Esc to clear";
                    return true;
                }
                if (focused_row_) {
                    const RowCache& rc = focused_row_->cache;
                    title = !rc.window_title.empty()
                        ? ulp::str::concat_strings(rc.window_title, L"  —  ", rc.filename)
                        : rc.filename;
                    subtitle = rc.wpath;
                    return !title.empty() || !subtitle.empty();
                }
                return false;
            case IdActCopy: {
                if (sel_n > 1) {
                    title = ulp::str::concat_strings(L"Copy ", format_count(sel_n).view(), L" files (Ctrl+C)");
                    return true;
                }
                const CaptureEntry* e = selected_entry();
                title = (e && e->kind == CaptureKind::Video) ? L"Copy video file (Ctrl+C)" : L"Copy image (Ctrl+C)";
                return true;
            }
            case IdActOpen:
                if (sel_n > 1) {
                    title = sel_n > 8
                        ? L"Open first 8 selected items in default app (Enter)"
                        : ulp::str::concat_strings(L"Open ", format_count(sel_n).view(), L" items in default app (Enter)");
                } else {
                    title = L"Open in default app (Enter)";
                }
                return true;
            case IdActExplorer:
                title = sel_n > 1 ? L"Show selected files in Explorer" : L"Show in Explorer";
                return true;
            case IdActDelete:
                title = sel_n > 1
                    ? ulp::str::concat_strings(L"Move ", format_count(sel_n).view(), L" files to Recycle Bin & remove from history (Del)")
                    : L"Move file to Recycle Bin & remove from history (Del)";
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

    void draw_header(Canvas& cv, D2D1_SIZE_F sz, const HeaderLayout& hl) {
        if (cv.has_dirty_clip() && !cv.intersects_clip(D2D1::RectF(0.0f, 0.0f, sz.width, hl.height + 2.0f), 2.0f)) return;
        cv.fill(D2D1::RectF(0.0f, 0.0f, sz.width, hl.height), with_alpha(cv.theme().surface_lowest, 0.78f));
        cv.hline(0.0f, sz.width, hl.height, with_alpha(cv.theme().outline_variant, 0.65f));

        constexpr float pad_x = 14.0f;
        constexpr float pad_y = 11.0f;
        constexpr float item_h = 32.0f;

        const FlowPlacement& brand_p = hl.placements[0];
        cv.icon(glyph::camera, rect(pad_x + brand_p.x, pad_y + brand_p.y, 32.0f, item_h), cv.theme().primary, Font::IconLarge);
        cv.text(L"capusy", rect(pad_x + brand_p.x + 36.0f, pad_y + brand_p.y, 90.0f, item_h), Font::Title, cv.theme().on_surface);

        for (size_t i = 0; i < hl.btn_count; ++i) {
            const HeaderBtn& b = hl.btns[i];
            const FlowPlacement& p = hl.placements[1 + i];
            const D2D1_RECT_F r = rect(pad_x + p.x, pad_y + p.y, p.width, item_h);
            draw_button(cv, hover_.alpha(b.id), r, b.icon, b.label, b.id, 0, b.style);
            if (b.update_dot) {
                cv.fill_circle({.x = r.right - 7.0f, .y = r.top + 7.0f}, 4.0f, cv.theme().primary);
            }
        }
    }

    void draw_sidebar(Canvas& cv, D2D1_RECT_F area) {
        if (cv.has_dirty_clip() && !cv.intersects_clip(area, 2.0f)) return;
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

        const size_t sel_n = selection_count();
        FixedWString<48> cnt;
        if (sel_n > 1) {
            cnt.assign(format_count(sel_n));
            cnt.append(L" selected");
        } else if (search_active_ && std::cmp_not_equal(total_count_, unfiltered_count_)) {
            cnt.assign(format_count(static_cast<uint64_t>(total_count_)));
            cnt.append(L" / ");
            cnt.append(format_count(unfiltered_count_));
        } else {
            cnt.assign(format_count(static_cast<uint64_t>(total_count_)));
        }
        const float cnt_left = std::min(area.right - 20.0f, fx + 210.0f);
        cv.text(cnt, D2D1::RectF(cnt_left, fy, area.right - 12.0f, fy + 28.0f), Font::Caption,
                sel_n > 1 ? cv.theme().primary : cv.theme().outline, Align::Right);
        cv.hline(area.left, area.right, area.top + SIDEBAR_TOP_H - 1.0f, with_alpha(cv.theme().outline_variant, 0.45f));

        const D2D1_RECT_F list_r = D2D1::RectF(area.left, area.top + SIDEBAR_TOP_H, area.right, area.bottom);
        list_rect_ = list_r;
        cv.push_clip(list_r);

        if (total_count_ <= 0) {
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

        const auto step = static_cast<double>(ROW_H + ROW_GAP);
        const int first = std::max(0, static_cast<int>(std::floor((scroll_ - 6.0) / step)));
        const int last = std::min(total_count_ - 1, static_cast<int>(std::ceil((scroll_ + static_cast<double>(height(list_r))) / step)));
        const UINT thumb_px = static_cast<UINT>(std::lround(112.0f * target_.scale()));

        for (int i = first; i <= last; ++i) {
            const float ry = list_r.top + 6.0f + static_cast<float>((static_cast<double>(i) * step) - scroll_);
            const D2D1_RECT_F rr = rect(list_r.left + 8.0f, ry, width(list_r) - 20.0f, ROW_H);
            if (cv.has_dirty_clip() && !cv.intersects_clip(rr, 4.0f)) continue;
            const RowItem* item = row_at(i);
            if (!item) continue;
            const CaptureEntry& e = item->entry;
            const RowCache& rc = item->cache;
            const bool sel = selection_.contains(e.id);
            const bool active = (i == selection_.focus);
            const float ha = hover_.alpha(IdRow, i);

            const D2D1_COLOR_F bg = sel
                ? lerp_color(with_alpha(cv.theme().primary_container, 0.55f), with_alpha(cv.theme().primary_container, 0.72f), ha)
                : with_alpha(cv.theme().surface_high, 0.45f * ha);
            if (bg.a > 0.002f) cv.fill_round(rr, 10.0f, bg);
            if (sel) {
                const bool active_multi = active && sel_n > 1;
                cv.stroke_round(rr, 10.0f, with_alpha(cv.theme().primary, active_multi ? 0.95f : 0.75f), active_multi ? 1.5f : 1.2f);
            }

            // Thumbnail box
            const D2D1_RECT_F tr = rect(rr.left + 8.0f, rr.top + 8.0f, 84.0f, ROW_H - 16.0f);
            cv.fill_round(tr, 7.0f, cv.theme().surface_lowest);
            const auto tid = static_cast<uint64_t>(e.id);
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

            // Text info (zero allocations per frame once page row is cached)
            const float tx = tr.right + 10.0f;
            const float tw = std::max(40.0f, rr.right - tx - 8.0f);
            cv.text(rc.title_line, rect(tx, rr.top + 10.0f, tw, 20.0f), Font::SmallSemi, sel ? cv.theme().on_primary_container : cv.theme().on_surface);
            cv.text(rc.line2, rect(tx, rr.top + 32.0f, tw, 17.0f), Font::Caption, cv.theme().on_surface_variant);
            cv.text(rc.ts, rect(tx, rr.top + 51.0f, tw, 17.0f), Font::Caption, cv.theme().outline);

            cv.hit(rr, IdRow, i);
        }
        cv.pop_clip();

        // Scrollbar
        const double ms = max_scroll();
        if (ms > 0.0) {
            const float track_h = height(list_r) - 8.0f;
            const double total_h = (static_cast<double>(total_count_) * step) + 12.0;
            const float thumb_h = std::max(28.0f, static_cast<float>(static_cast<double>(track_h) * static_cast<double>(height(list_r)) / total_h));
            const float ty = list_r.top + 4.0f + static_cast<float>(static_cast<double>(track_h - thumb_h) * (scroll_ / ms));
            const float ha = dragging_scroll_ ? 1.0f : hover_.alpha(IdScrollbar);
            scroll_thumb_ = D2D1::RectF(list_r.right - lerp(6.0f, 8.0f, ha), ty, list_r.right - 2.0f, ty + thumb_h);
            cv.fill_round(scroll_thumb_, 3.0f, with_alpha(cv.theme().on_surface_variant, lerp(0.28f, 0.55f, ha)));
            cv.hit(D2D1::RectF(list_r.right - 12.0f, list_r.top, list_r.right, list_r.bottom), IdScrollbar);
        }
    }

    void draw_preview_pane(Canvas& cv, D2D1_RECT_F area) {
        if (cv.has_dirty_clip() && !cv.intersects_clip(area, 2.0f)) return;
        const CaptureEntry* e = selected_entry();
        if (!e) {
            cv.icon(glyph::photo, rect(area.left, area.top + (height(area) * 0.5f) - 42.0f, width(area), 36.0f), cv.theme().outline, Font::IconLarge);
            cv.text(L"Select a capture to preview", rect(area.left, area.top + (height(area) * 0.5f), width(area), 24.0f),
                    Font::Body, cv.theme().outline, Align::Center);
            return;
        }

        const size_t sel_n = selection_count();
        FixedWString<48> del_lbl{L"Move to Recycle Bin"};
        if (sel_n > 1) {
            del_lbl.append(L" (");
            del_lbl.append(format_count(sel_n));
            del_lbl.push_back(L')');
        }
        const float del_w = button_width(cv, del_lbl, true, 154.0f);
        const float exp_w = button_width(cv, L"Folder", true, 84.0f);
        const float open_w = button_width(cv, L"Open", true, 78.0f);
        const float copy_w = button_width(cv, L"Copy", true, 78.0f);

        Flow foot_actions{6.0f, 6.0f};
        foot_actions.add(copy_w, 32.0f, [&](D2D1_RECT_F r) {
            draw_button(cv, hover_.alpha(IdActCopy), r, glyph::copy, L"Copy", IdActCopy, 0, 1);
        });
        foot_actions.add(open_w, 32.0f, [&](D2D1_RECT_F r) {
            draw_button(cv, hover_.alpha(IdActOpen), r, glyph::open, L"Open", IdActOpen, 0, 0);
        });
        foot_actions.add(exp_w, 32.0f, [&](D2D1_RECT_F r) {
            draw_button(cv, hover_.alpha(IdActExplorer), r, glyph::folder, L"Folder", IdActExplorer, 0, 3);
        });
        foot_actions.add(del_w, 32.0f, [&](D2D1_RECT_F r) {
            draw_button(cv, hover_.alpha(IdActDelete), r, glyph::del, del_lbl, IdActDelete, 0, 2);
        });

        const float foot_inner_w = std::max(40.0f, width(area) - 36.0f - 22.0f);
        const float btns_nat_w = foot_actions.natural_width();
        const bool inline_footer = (foot_inner_w - btns_nat_w - 14.0f) >= 140.0f;
        const float foot_box_h = inline_footer ? 56.0f : (10.0f + 40.0f + 8.0f + foot_actions.height(foot_inner_w) + 10.0f);
        const float footer_h = foot_box_h + 12.0f;

        const bool playable_video = is_playable_video(*e);
        const float video_bar_h = playable_video ? 44.0f : 0.0f;
        const D2D1_RECT_F stage = D2D1::RectF(area.left + 18.0f, area.top + 18.0f, area.right - 18.0f, std::max(area.top + 60.0f, area.bottom - footer_h - video_bar_h - 12.0f));
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
                const D2D1_SIZE_U ps = tb->GetPixelSize();
                const uint32_t src_w = (e->width > 0 && e->height > 0) ? e->width : ps.width;
                const uint32_t src_h = (e->width > 0 && e->height > 0) ? e->height : ps.height;
                const FitBox fb = compute_preview_box(src_w, src_h, e->width, e->height, target_.scale(),
                                                      {.left = media_box.left, .top = media_box.top, .width = width(media_box), .height = height(media_box)});
                cv.bitmap(tb, rect(fb.left, fb.top, fb.width, fb.height));
            } else {
                cv.text(video_.failed() ? L"Video preview unavailable — double-click to open externally" : L"Loading video…",
                        media_box, Font::Body, cv.theme().outline, Align::Center);
            }
            cv.hit(stage, IdPreviewArea);

            // Video controls bar
            const D2D1_RECT_F vb = D2D1::RectF(area.left + 18.0f, stage.bottom + 8.0f, area.right - 18.0f, stage.bottom + 44.0f);
            video_bar_rect_ = vb;
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
            video_bar_rect_ = {};
            const auto tid = static_cast<uint64_t>(e->id);
            ID2D1Bitmap1* bmp = sh_.previews.get(tid);
            if (!bmp) {
                if (focused_row_) sh_.previews.request(tid, focused_row_->cache.wpath, 2048);
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

        float info_w = foot_inner_w;
        float info_bot = foot.top + 48.0f;
        if (inline_footer) {
            const float bx = foot.right - 10.0f - btns_nat_w;
            const float by = foot.top + ((height(foot) - 32.0f) * 0.5f);
            foot_actions.draw(bx, by, btns_nat_w);
            info_w = std::max(40.0f, bx - foot.left - 18.0f);
            info_bot = foot.bottom - 4.0f;
        } else {
            foot_actions.draw(foot.left + 11.0f, foot.top + 56.0f, foot_inner_w);
        }

        if (sel_n > 1) {
            const SelectionSummary& sum = selection_summary();
            FixedWString<64> top_line;
            top_line.assign(format_count(sel_n));
            top_line.append(L" captures selected");
            cv.text(top_line, rect(foot.left + 12.0f, foot.top + 8.0f, info_w, 20.0f), Font::SmallSemi, cv.theme().on_surface);

            std::wstring sub;
            if (sum.image_count > 0) {
                append_to(sub, format_count(sum.image_count).view(), sum.image_count == 1 ? L" image" : L" images");
            }
            if (sum.video_count > 0) {
                if (!sub.empty()) sub += L"  •  ";
                append_to(sub, format_count(sum.video_count).view(), sum.video_count == 1 ? L" video" : L" videos");
            }
            if (sum.total_bytes > 0) {
                if (!sub.empty()) sub += L"  •  ";
                append_to(sub, format_size(sum.total_bytes).view(), L" total");
            }
            cv.text(sub, rect(foot.left + 12.0f, foot.top + 28.0f, info_w, 18.0f), Font::Caption, cv.theme().on_surface_variant);
        } else if (focused_row_) {
            const RowCache& sel_rc = focused_row_->cache;
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
        }
        if (info_w > 40.0f) {
            cv.hit(D2D1::RectF(foot.left + 8.0f, foot.top + 4.0f, foot.left + 12.0f + info_w, info_bot), IdFooterInfo);
        }
    }

    // ------------------------------------------------------------------------------------------
    // Actions, Context Menu & OLE Drag-and-Drop
    // ------------------------------------------------------------------------------------------

    void open_entry(int idx) {
        if (const RowItem* item = row_at(idx)) {
            (void)open_file(item->cache.wpath);
        }
    }

    void open_selected_entries() {
        const auto items = collect_selected_entries();
        constexpr size_t MAX_OPEN = 8;
        for (size_t i = 0; i < items.size() && i < MAX_OPEN; ++i) {
            (void)open_file(to_wide(items[i].path));
        }
    }

    void copy_selected_entries() {
        const auto items = collect_selected_entries();
        if (items.empty()) return;
        std::vector<std::wstring> paths;
        paths.reserve(items.size());
        for (const auto& item : items) {
            paths.push_back(to_wide(item.path));
        }
        const bool single_image = (items.size() == 1 && items[0].kind == CaptureKind::Image);
        (void)copy_captures_to_clipboard(paths, single_image);
    }

    void reveal_selected_entries() {
        const auto items = collect_selected_entries();
        if (items.empty()) return;
        std::vector<std::wstring> paths;
        paths.reserve(items.size());
        for (const auto& item : items) {
            paths.push_back(to_wide(item.path));
        }
        (void)reveal_in_explorer(paths);
    }

    void delete_selected_immediate(bool delete_files) {
        const auto items = collect_selected_entries();
        if (items.empty()) return;
        std::vector<int64_t> ids;
        std::vector<std::wstring> paths;
        ids.reserve(items.size());
        paths.reserve(items.size());
        for (const auto& item : items) {
            ids.push_back(item.id);
            paths.push_back(to_wide(item.path));
        }
        const int next_anchor = std::max(0, selection_.focus);
        delete_entries_immediate(ids, paths, next_anchor, delete_files);
    }

    void delete_entries_immediate(std::span<const int64_t> ids, std::span<const std::wstring> paths, int first_idx, bool delete_files) {
        if (ids.empty()) return;
        if (video_.active()) {
            for (const auto& p : paths) {
                if (video_.path() == p) {
                    video_.close();
                    break;
                }
            }
        }
        if (delete_files) {
            (void)delete_files_to_recycle_bin(paths);
        }
        (void)sh_.history.remove_many(ids);
        for (const int64_t id : ids) {
            sh_.thumbs.evict(static_cast<uint64_t>(id));
            sh_.previews.evict(static_cast<uint64_t>(id));
        }

        reload_entries(false, false);
        if (total_count_ > 0) {
            const int next_sel = std::clamp(first_idx, 0, total_count_ - 1);
            select_single_idx(next_sel);
        }
        sync_preview_media();
        invalidate();
    }

    void show_context_menu(int idx) {
        const RowItem* clicked = row_at(idx);
        if (!clicked) return;
        const int64_t clicked_id = clicked->entry.id;
        if (!selection_.contains(clicked_id)) {
            select_single_idx(idx);
            sync_preview_media();
            render();
        }

        const size_t n = selection_count();
        if (n == 0) return;
        const CaptureEntry* primary = selected_entry();

        const wil::unique_hmenu menu{CreatePopupMenu()};
        if (n > 1) {
            const auto cnt = format_count(n);
            const std::wstring open_lbl = ulp::str::concat_strings(L"Open ", cnt.view(), L" Items in Default App");
            const std::wstring copy_lbl = ulp::str::concat_strings(L"Copy ", cnt.view(), L" Files");
            const std::wstring path_lbl = ulp::str::concat_strings(L"Copy ", cnt.view(), L" File Paths");
            const std::wstring rem_lbl = ulp::str::concat_strings(L"Remove ", cnt.view(), L" Items from History");
            const std::wstring del_lbl = ulp::str::concat_strings(L"Move ", cnt.view(), L" Files to Recycle Bin");
            AppendMenuW(menu.get(), MF_STRING, CmdOpen, open_lbl.c_str());
            AppendMenuW(menu.get(), MF_STRING, CmdCopy, copy_lbl.c_str());
            AppendMenuW(menu.get(), MF_STRING, CmdCopyPath, path_lbl.c_str());
            AppendMenuW(menu.get(), MF_STRING, CmdExplorer, L"Show in Explorer");
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu.get(), MF_STRING, CmdRemoveHistory, rem_lbl.c_str());
            AppendMenuW(menu.get(), MF_STRING, CmdDeleteFile, del_lbl.c_str());
        } else {
            AppendMenuW(menu.get(), MF_STRING, CmdOpen, L"Open in Default App");
            AppendMenuW(menu.get(), MF_STRING, CmdCopy, (!primary || primary->kind == CaptureKind::Image) ? L"Copy Image" : L"Copy Video File");
            AppendMenuW(menu.get(), MF_STRING, CmdCopyPath, L"Copy File Path");
            AppendMenuW(menu.get(), MF_STRING, CmdExplorer, L"Show in Explorer");
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu.get(), MF_STRING, CmdRemoveHistory, L"Remove from History");
            AppendMenuW(menu.get(), MF_STRING, CmdDeleteFile, L"Move to Recycle Bin");
        }
        SetMenuDefaultItem(menu.get(), CmdOpen, FALSE);

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, hwnd_, nullptr));
        PostMessageW(hwnd_, WM_NULL, 0, 0);

        switch (cmd) {
            case CmdOpen: open_selected_entries(); break;
            case CmdCopy: copy_selected_entries(); break;
            case CmdCopyPath: {
                const auto items = collect_selected_entries();
                std::wstring all;
                for (const auto& item : items) {
                    if (!all.empty()) all += L"\r\n";
                    all += to_wide(item.path);
                }
                (void)set_clipboard_text(hwnd_, all);
                break;
            }
            case CmdExplorer: reveal_selected_entries(); break;
            case CmdRemoveHistory: delete_selected_immediate(false); break;
            case CmdDeleteFile: delete_selected_immediate(true); break;
            default: break;
        }
    }

    void start_drag_out(int idx) {
        if (dragging_out_ || idx < 0 || idx >= total_count_) return;
        drag_pending_ = false;
        deferred_select_idx_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();

        const RowItem* row = row_at(idx);
        if (!row) return;
        std::vector<std::wstring> paths;
        if (selection_.contains(row->entry.id)) {
            const auto items = collect_selected_entries();
            paths.reserve(items.size());
            for (const auto& item : items) {
                paths.push_back(to_wide(item.path));
            }
        } else {
            paths.push_back(row->cache.wpath);
        }
        if (paths.empty()) return;

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

    void scrub_to_x(float x) {
        const float pad = 8.0f;
        const float l = scrub_rect_.left + pad;
        const float r = std::max(l + 4.0f, scrub_rect_.right - pad);
        const float ratio = std::clamp((x - l) / (r - l), 0.0f, 1.0f);
        video_.seek(static_cast<double>(ratio) * video_.duration());
        if (video_bar_rect_.right > video_bar_rect_.left) {
            invalidate_rect(video_bar_rect_);
        } else {
            invalidate();
        }
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
            case IdBtnFullRecord: if (sh_.on_toggle_full_record) sh_.on_toggle_full_record(); return;
            case IdBtnRecord: if (sh_.on_toggle_record) sh_.on_toggle_record(); return;
            case IdBtnColorPicker: if (sh_.on_color_picker) sh_.on_color_picker(); return;
            case IdBtnSettings: if (sh_.on_open_settings) sh_.on_open_settings(0); return;
            case IdFilterAll: filter_ = Filter::All; reload_entries(true); return;
            case IdFilterImages: filter_ = Filter::Images; reload_entries(true); return;
            case IdFilterVideos: filter_ = Filter::Videos; reload_entries(true); return;
            case IdActCopy: copy_selected_entries(); return;
            case IdActOpen: open_selected_entries(); return;
            case IdActExplorer: reveal_selected_entries(); return;
            case IdActDelete: delete_selected_immediate(true); return;
            case IdVideoPlay:
                if (!video_.active() || video_.failed()) {
                    sync_preview_media();
                    invalidate();
                } else {
                    video_.toggle_play();
                    if (video_.needs_timer()) SetTimer(hwnd_, TIMER_VIDEO, 16, nullptr);
                    if (video_bar_rect_.right > video_bar_rect_.left) {
                        invalidate_rect(video_bar_rect_);
                    } else {
                        invalidate();
                    }
                }
                return;
            case IdVideoMute:
                video_.toggle_mute();
                sh_.settings.video_muted = video_.is_muted();
                sh_.save();
                if (video_bar_rect_.right > video_bar_rect_.left) {
                    invalidate_rect(video_bar_rect_);
                } else {
                    invalidate();
                }
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
                const bool ctrl = GetKeyState(VK_CONTROL) < 0;
                const bool shift = GetKeyState(VK_SHIFT) < 0;
                const int prev_focus = selection_.focus;
                const bool deferred = click_row_idx(h->index, ctrl, shift);
                deferred_select_idx_ = deferred ? h->index : -1;
                if (selection_.focus != prev_focus) {
                    sync_preview_media();
                }
                if (!ctrl && !shift && h->index >= 0 && h->index < total_count_) {
                    drag_pending_ = true;
                    drag_idx_ = h->index;
                    drag_start_px_ = {.x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp)};
                    SetCapture(hwnd_);
                }
                invalidate();
                return;
            }
            case IdPreviewArea: {
                if (selection_.focus >= 0) {
                    drag_pending_ = true;
                    drag_idx_ = selection_.focus;
                    drag_start_px_ = {.x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp)};
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
            open_entry(selection_.focus);
        }
    }

    void on_rbutton_up(LPARAM lp) {
        const Hit* h = hit_test(hits_, to_dip(lp));
        if (!h) return;
        if (h->id == IdRow) {
            show_context_menu(h->index);
        } else if (h->id == IdPreviewArea && selection_.focus >= 0) {
            show_context_menu(selection_.focus);
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
            const double track_h = std::max(1.0, static_cast<double>(height(list_rect_) - 8.0f - height(scroll_thumb_)));
            const double rel = std::clamp((static_cast<double>(p.y) - static_cast<double>(scroll_drag_offset_) - static_cast<double>(list_rect_.top + 4.0f)) / track_h, 0.0, 1.0);
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
            bool tip_changed = false;
            if (tooltip_.on_hover_changed(hwnd_, TIMER_TOOLTIP, h ? &h->r : nullptr, has_tooltip(nid, nidx), true)) {
                std::wstring tip_title;
                std::wstring tip_sub;
                const bool ok = window_active_ && build_tooltip(nid, nidx, tip_title, tip_sub);
                tip_changed = tooltip_.show_now(ok, std::move(tip_title), std::move(tip_sub), true);
            }
            if (vis_changed || tooltip_.alpha.active()) {
                start_anim();
            } else if (tip_changed && window_can_render_live(visible(), minimized_)) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        if (h && h->id == IdSearchBox) {
            SetCursor(cursor_ibeam_);
        } else {
            SetCursor((h && h->id != IdPreviewArea && h->id != IdFooterInfo) ? cursor_hand_ : cursor_arrow_);
        }
        TRACKMOUSEEVENT tme{.cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd_};
        TrackMouseEvent(&tme);
    }

    void on_lbutton_up() {
        drag_pending_ = false;
        dragging_scroll_ = false;
        dragging_search_ = false;
        if (deferred_select_idx_ >= 0 && deferred_select_idx_ < total_count_) {
            const int idx = std::exchange(deferred_select_idx_, -1);
            const int prev_focus = selection_.focus;
            select_single_idx(idx);
            if (selection_.focus != prev_focus) sync_preview_media();
            invalidate();
        } else {
            deferred_select_idx_ = -1;
        }
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
            schedule_search_update();
        }
    }

    void on_key_down(UINT vk) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        if (ctrl && vk == 'F') {
            set_search_focus(true);
            search_field_.select_all();
            reset_caret_blink();
            invalidate();
            return;
        }
        if (vk == VK_UP || vk == VK_DOWN) {
            const int prev_focus = selection_.focus;
            if (move_selection(vk == VK_UP ? -1 : 1, shift)) {
                if (selection_.focus != prev_focus) sync_preview_media();
                ensure_row_visible(selection_.focus);
                invalidate();
            }
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
            if (vk == VK_RETURN) {
                apply_search_filter();
                if (!selection_.empty()) open_selected_entries();
                return;
            }
            bool handled = false;
            const bool changed = search_field_.on_key(hwnd_, vk, handled);
            if (handled) {
                reset_caret_blink();
                if (changed) {
                    schedule_search_update();
                } else {
                    invalidate();
                }
                return;
            }
            return;
        }
        if (ctrl && vk == 'A' && total_count_ > 0) {
            const int prev_focus = selection_.focus;
            select_all_rows();
            if (selection_.focus != prev_focus) sync_preview_media();
            invalidate();
            return;
        }
        if (vk == VK_ESCAPE) {
            if (selection_count() > 1 && selection_.focus >= 0 && selection_.focus < total_count_) {
                select_single_idx(selection_.focus);
                invalidate();
                return;
            }
            hide();
            return;
        }
        if (vk == VK_RETURN && !selection_.empty()) { open_selected_entries(); return; }
        if (vk == VK_DELETE && !selection_.empty()) { delete_selected_immediate(true); return; }
        if (ctrl && vk == 'C' && !selection_.empty()) { copy_selected_entries(); return; }
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
                minimized_ = (wp == SIZE_MINIMIZED);
                if (minimized_) {
                    KillTimer(hwnd, TIMER_VIDEO);
                    sync_caret_timer();
                } else if (visible()) {
                    (void)target_.resize(LOWORD(lp), HIWORD(lp));
                    clamp_scroll();
                    full_dirty_ = true;
                    pending_dirty_.clear();
                    render();
                    sync_caret_timer();
                    if (video_.needs_timer()) SetTimer(hwnd, TIMER_VIDEO, 16, nullptr);
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
            case WM_GETMINMAXINFO: {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
                const float s = (dpi > 0 ? static_cast<float>(dpi) : 96.0f) / 96.0f;
                mmi->ptMinTrackSize = {.x = static_cast<LONG>(std::lround(640.0f * s)), .y = static_cast<LONG>(std::lround(480.0f * s))};
                return 0;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(hwnd, &ps);
                render();
                EndPaint(hwnd, &ps);
                if (window_can_render_live(visible(), minimized_) && anim_active_) {
                    // Drain due timers while QS_PAINT is clear so VSync frames never starve timers.
                    MSG m;
                    while (PeekMessageW(&m, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE)) {
                        DispatchMessageW(&m);
                    }
                    if (window_can_render_live(visible(), minimized_) && anim_active_) InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_APP_THUMBS:
                on_thumbs_ready();
                return 0;
            case WM_APP_VIDEO:
                if (video_.process_events()) {
                    if (window_can_render_live(visible(), minimized_) && video_.needs_timer()) SetTimer(hwnd, TIMER_VIDEO, 16, nullptr);
                    if (preview_rect_.right > preview_rect_.left) invalidate_rect(preview_rect_);
                    if (video_bar_rect_.right > video_bar_rect_.left) invalidate_rect(video_bar_rect_);
                    if (preview_rect_.right <= preview_rect_.left) invalidate();
                }
                return 0;
            case WM_TIMER:
                if (wp == TIMER_VIDEO) {
                    if (!window_can_render_live(visible(), minimized_) || !video_.active()) {
                        KillTimer(hwnd, TIMER_VIDEO);
                    } else if (target_.ready()) {
                        const auto st = video_.tick(sh_.gfx.d3d(), sh_.gfx.epoch(), target_.dc());
                        if (st == MediaTickStatus::FrameReady) {
                            if (preview_rect_.right > preview_rect_.left) invalidate_rect(preview_rect_);
                            if (video_bar_rect_.right > video_bar_rect_.left) invalidate_rect(video_bar_rect_);
                        } else if (video_.is_playing() && video_bar_rect_.right > video_bar_rect_.left) {
                            invalidate_rect(video_bar_rect_);
                        }
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
                    if (async_drops_.empty()) {
                        KillTimer(hwnd, TIMER_DROP);
                        if (sh_.on_visibility_changed) sh_.on_visibility_changed();
                    }
                } else if (wp == TIMER_CARET) {
                    if (search_field_.focused && window_can_render_live(visible(), minimized_) && window_active_) {
                        search_field_.caret_on = !search_field_.caret_on;
                        if (search_box_rect_.right > search_box_rect_.left) {
                            invalidate_rect(inset(search_box_rect_, -2.0f, -2.0f));
                        } else {
                            invalidate();
                        }
                    } else {
                        KillTimer(hwnd, TIMER_CARET);
                    }
                } else if (wp == TIMER_SEARCH) {
                    KillTimer(hwnd, TIMER_SEARCH);
                    apply_search_filter();
                } else if (wp == TIMER_TOOLTIP) {
                    if (!window_can_render_live(visible(), minimized_) || !window_active_) {
                        tooltip_.clear(hwnd_, TIMER_TOOLTIP, false);
                        return 0;
                    }
                    std::wstring tip_title;
                    std::wstring tip_sub;
                    const bool ok = build_tooltip(raw_hover_id_, raw_hover_index_, tip_title, tip_sub);
                    if (tooltip_.on_timer(hwnd_, TIMER_TOOLTIP, ok, std::move(tip_title), std::move(tip_sub), true)) {
                        start_anim();
                    }
                } else if (wp == TIMER_RELEASE_TARGET) {
                    KillTimer(hwnd, TIMER_RELEASE_TARGET);
                    if (!visible()) {
                        target_.release();
                    }
                }
                return 0;
            case WM_MOUSEWHEEL: {
                dismiss_tooltip();
                const double delta = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0;
                target_scroll_ = std::clamp(target_scroll_ - (delta * 96.0), 0.0, max_scroll());
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
                window_active_ = wm_activate_is_active(LOWORD(wp), HIWORD(wp) != 0);
                if (!window_active_) dismiss_tooltip();
                sync_caret_timer();
                if (search_field_.focused && search_box_rect_.right > search_box_rect_.left) {
                    invalidate_rect(inset(search_box_rect_, -2.0f, -2.0f));
                }
                break;
            case WM_DESTROY:
                KillTimer(hwnd, TIMER_VIDEO);
                KillTimer(hwnd, TIMER_DROP);
                KillTimer(hwnd, TIMER_CARET);
                KillTimer(hwnd, TIMER_TOOLTIP);
                KillTimer(hwnd, TIMER_SEARCH);
                KillTimer(hwnd, TIMER_RELEASE_TARGET);
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
    bool search_active_ = false;
    std::string search_query_utf8_;
    uint64_t unfiltered_count_ = 0;
    int total_count_ = 0;
    std::vector<int64_t> search_ids_;
    mutable std::vector<CachedPage> page_cache_;
    mutable uint64_t page_clock_ = 0;
    std::optional<RowItem> focused_row_;
    mutable SelectionSummary sel_summary_{};
    mutable bool sel_summary_dirty_ = true;
    HistorySelection selection_;
    int deferred_select_idx_ = -1;

    std::vector<Hit> hits_;
    std::vector<Hit> scratch_hits_;
    HoverTracker hover_;
    int raw_hover_id_ = IdNone;
    int raw_hover_index_ = 0;
    bool window_active_ = true;
    HoverTooltip tooltip_;
    std::optional<D2D1_RECT_F> last_tooltip_dirty_;
    double scroll_ = 0.0;
    double target_scroll_ = 0.0;
    D2D1_RECT_F list_rect_{};
    D2D1_RECT_F scroll_thumb_{};
    D2D1_RECT_F preview_rect_{};
    D2D1_RECT_F video_bar_rect_{};
    D2D1_RECT_F scrub_rect_{};

    bool dragging_scroll_ = false;
    bool in_sizemove_ = false;
    bool minimized_ = false;
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
    std::vector<D2D1_RECT_F> pending_dirty_;
    std::vector<D2D1_RECT_F> dirty_dips_scratch_;
    std::chrono::steady_clock::time_point last_anim_{};
    HCURSOR cursor_arrow_ = nullptr;
    HCURSOR cursor_hand_ = nullptr;
    HCURSOR cursor_ibeam_ = nullptr;
};

} // namespace capusy::ui
