#pragma once

#include "canvas.hpp"
#include "flow.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace capusy::ui {

[[nodiscard]] inline float text_height(Canvas& cv, std::wstring_view s, Font f, float w, float min_h) {
    return std::max(min_h, cv.measure_height(s, f, w));
}

[[nodiscard]] inline float button_width(Canvas& cv, std::wstring_view label, bool has_icon, float min_w) {
    return std::max(min_w, cv.measure(label, Font::SmallSemi) + (has_icon ? 24.0f : 0.0f) + 28.0f);
}

inline void flow_text(Canvas& cv, Flow& flow, std::wstring_view s, Font f, D2D1_COLOR_F c, float avail, float line_h,
                      bool grow = false, Align a = Align::Left) {
    const float natural = cv.measure(s, f) + 4.0f;
    if (natural <= avail) {
        flow.add(natural, line_h, [&cv, text = std::wstring{s}, f, c, a](D2D1_RECT_F r) { cv.text(text, r, f, c, a); }, grow);
        return;
    }
    const float wrapped_h = text_height(cv, s, f, avail, line_h);
    flow.add(avail, wrapped_h, [&cv, text = std::wstring{s}, f, c, a](D2D1_RECT_F r) { cv.text_wrap(text, r, f, c, a); });
}

struct CardHeader {
    std::wstring_view title;
    std::wstring_view caption;
    D2D1_COLOR_F title_color;
    float side_w;
    float side_h;
    bool beside;
    float text_w;
    float title_h;
    float caption_h;
    float h;
};

[[nodiscard]] inline CardHeader make_header(Canvas& cv, float rw, std::wstring_view title, D2D1_COLOR_F title_color,
                                            std::wstring_view caption, float side_w = 0.0f, float side_h = 0.0f) {
    CardHeader hd{.title = title, .caption = caption, .title_color = title_color, .side_w = side_w, .side_h = side_h};
    const float beside_w = rw - side_w - (side_w > 0.0f ? 12.0f : 0.0f);
    hd.beside = side_w <= 0.0f || beside_w >= 200.0f;
    hd.text_w = hd.beside ? beside_w : rw;
    hd.title_h = text_height(cv, hd.title, Font::Large, hd.text_w, 22.0f);
    hd.caption_h = hd.caption.empty() ? 0.0f : text_height(cv, hd.caption, Font::Small, hd.text_w, 18.0f);
    hd.h = hd.title_h + hd.caption_h + (hd.beside ? 0.0f : 8.0f + side_h);
    return hd;
}

inline D2D1_RECT_F draw_header(Canvas& cv, float lx, float y, float rw, const CardHeader& hd) {
    cv.text_wrap(hd.title, rect(lx, y + 16.0f, hd.text_w, hd.title_h), Font::Large, hd.title_color);
    if (!hd.caption.empty()) {
        cv.text_wrap(hd.caption, rect(lx, y + 16.0f + hd.title_h, hd.text_w, hd.caption_h), Font::Small, cv.theme().on_surface_variant);
    }
    if (hd.beside) return rect(lx + rw - hd.side_w, y + 18.0f, hd.side_w, hd.side_h);
    return rect(lx, y + 16.0f + hd.title_h + hd.caption_h + 8.0f, hd.side_w, hd.side_h);
}

inline float card_begin(Canvas& cv, float x, float y, float w, float h, float card_gap = 14.0f) {
    const D2D1_RECT_F r = rect(x, y, w, h);
    const float crad = offset_radius(cv.theme().card_radius, 2.0f);
    cv.fill_round(r, crad, with_alpha(cv.theme().surface, cv.theme().card_alpha));
    cv.stroke_round(r, crad, with_alpha(cv.theme().outline_variant, 0.65f));
    return y + h + card_gap;
}

inline void draw_well(Canvas& cv, D2D1_RECT_F r, float rad, bool stroke = false, bool focused = false, float hover = 0.0f) {
    const float eff_rad = std::min(height(r) * 0.5f, rad);
    cv.fill_round(r, eff_rad, lerp_color(cv.theme().surface_low, cv.theme().surface_high, hover));
    if (stroke) cv.stroke_round(r, eff_rad, focused ? cv.theme().primary : cv.theme().outline_variant, focused ? 1.5f : 1.0f);
}

inline void draw_button_content(Canvas& cv, D2D1_RECT_F r, wchar_t icon_g, std::wstring_view label, D2D1_COLOR_F fg) {
    if (icon_g && !label.empty()) {
        const float tw = cv.measure(label, Font::SmallSemi);
        const float total = 18.0f + 6.0f + tw;
        const float sx = r.left + ((width(r) - total) * 0.5f);
        cv.icon(icon_g, rect(sx, r.top, 18.0f, height(r)), fg, Font::IconSmall);
        cv.text(label, rect(sx + 24.0f, r.top, tw + 4.0f, height(r)), Font::SmallSemi, fg);
    } else if (icon_g) {
        cv.icon(icon_g, r, fg, Font::IconSmall);
    } else {
        cv.text(label, r, Font::SmallSemi, fg, Align::Center);
    }
}

/// style: 0=secondary, 1=primary, 2=danger, 3=ghost
inline void draw_button(Canvas& cv, float ha, D2D1_RECT_F r, wchar_t icon_g, std::wstring_view label, int id, int idx = 0, int style = 0) {
    const float rad = std::min(cv.theme().button_radius, height(r) * 0.5f);
    D2D1_COLOR_F fg = cv.theme().on_surface;
    D2D1_COLOR_F bg = lerp_color(cv.theme().surface_high, cv.theme().surface_highest, ha);
    D2D1_COLOR_F border = cv.theme().outline_variant;
    if (style == 1) {
        bg = lerp_color(cv.theme().primary, cv.theme().primary_hover, ha);
        fg = cv.theme().on_primary;
        border = bg;
    } else if (style == 2) {
        bg = lerp_color(with_alpha(cv.theme().error_container, 0.45f), with_alpha(cv.theme().error, 0.22f), ha);
        fg = cv.theme().error;
        border = with_alpha(cv.theme().error, 0.45f);
    } else if (style == 3) {
        bg = with_alpha(cv.theme().on_surface, 0.08f * ha);
        fg = lerp_color(cv.theme().on_surface_variant, cv.theme().on_surface, ha);
        border = hex(0, 0.0f);
    }
    if (bg.a > 0.0f) cv.fill_round(r, rad, bg);
    if (border.a > 0.0f && style != 1) cv.stroke_round(r, rad, border);
    draw_button_content(cv, r, icon_g, label, fg);
    cv.hit(r, id, idx);
}

inline void draw_disabled_button(Canvas& cv, D2D1_RECT_F r, wchar_t icon_g, std::wstring_view label, int id = 0, int idx = 0) {
    const float rad = std::min(cv.theme().button_radius, height(r) * 0.5f);
    cv.fill_round(r, rad, with_alpha(cv.theme().surface_high, 0.45f));
    cv.stroke_round(r, rad, with_alpha(cv.theme().outline_variant, 0.45f));
    draw_button_content(cv, r, icon_g, label, with_alpha(cv.theme().on_surface_variant, 0.5f));
    if (id != 0) cv.hit(r, id, idx);
}

inline void draw_switch(Canvas& cv, float ha, float t, float right_x, float cy, int id, int idx = 0) {
    const D2D1_RECT_F r = rect(right_x - 44.0f, cy - 12.0f, 44.0f, 24.0f);
    const float kx = lerp(r.left + 12.0f, r.right - 12.0f, t);
    const float kr = lerp(7.0f, 8.0f, t);
    const D2D1_COLOR_F off_track = lerp_color(cv.theme().surface_high, cv.theme().surface_highest, ha);
    cv.fill_round(r, 12.0f, lerp_color(off_track, cv.theme().primary, t));
    if (t < 1.0f) cv.stroke_round(r, 12.0f, with_alpha(cv.theme().outline, 1.0f - t));
    cv.fill_circle({.x = kx, .y = cy}, kr, lerp_color(cv.theme().outline, cv.theme().on_primary, t));
    cv.hit(r, id, idx);
}

inline void draw_slider(Canvas& cv, float ha, D2D1_RECT_F r, float ratio, bool dragging, int id, int idx = 0) {
    const float k = std::clamp(ratio, 0.0f, 1.0f);
    const float cy = (r.top + r.bottom) * 0.5f;
    const float pad_x = 8.0f;
    const float track_l = r.left + pad_x;
    const float track_r = std::max(track_l + 4.0f, r.right - pad_x);
    const float thumb_x = lerp(track_l, track_r, k);
    const D2D1_RECT_F track = D2D1::RectF(track_l, cy - 3.0f, track_r, cy + 3.0f);
    const D2D1_RECT_F filled = D2D1::RectF(track_l, cy - 3.0f, std::max(track_l + 3.0f, thumb_x), cy + 3.0f);

    cv.fill_round(track, 3.0f, cv.theme().surface_highest);
    if (thumb_x > track_l + 1.0f) cv.fill_round(filled, 3.0f, cv.theme().primary);
    const float kr = (dragging || ha > 0.05f) ? 8.0f : 6.5f;
    cv.fill_circle({.x = thumb_x, .y = cy}, kr, dragging ? cv.theme().primary_hover : cv.theme().primary);
    cv.stroke_circle({.x = thumb_x, .y = cy}, kr, cv.theme().on_primary, 1.5f);
    cv.hit(r, id, idx);
}

inline float error_row_height(Canvas& cv, std::wstring_view message, float row_w) {
    return text_height(cv, message, Font::Caption, std::max(40.0f, row_w - 22.0f), 20.0f);
}

inline void draw_error_row(Canvas& cv, std::wstring_view message, float x, float y, float row_w, float h) {
    cv.icon(glyph::warning, rect(x, y, 18.0f, 20.0f), cv.theme().error, Font::IconSmall);
    cv.text_wrap(message, rect(x + 22.0f, y, std::max(40.0f, row_w - 22.0f), h), Font::Caption, cv.theme().error);
}

struct DropdownOption {
    std::wstring label;
    bool selected = false;
    int index = 0;
};

inline void draw_dropdown_field(Canvas& cv, float ha, D2D1_RECT_F r, std::wstring_view label,
                                bool open, bool enabled, int id, int idx = 0) {
    const float rad = std::min(cv.theme().button_radius, height(r) * 0.5f);
    const D2D1_COLOR_F bg = enabled
        ? lerp_color(cv.theme().surface_low, cv.theme().surface_high, open ? 1.0f : ha)
        : with_alpha(cv.theme().surface_low, 0.55f);
    const D2D1_COLOR_F border = open
        ? cv.theme().primary
        : (enabled ? cv.theme().outline_variant : with_alpha(cv.theme().outline_variant, 0.45f));
    const D2D1_COLOR_F fg = enabled ? cv.theme().on_surface : with_alpha(cv.theme().on_surface_variant, 0.6f);
    const D2D1_COLOR_F chev = open ? cv.theme().primary : with_alpha(cv.theme().on_surface_variant, enabled ? 0.9f : 0.45f);

    cv.fill_round(r, rad, bg);
    cv.stroke_round(r, rad, border, open ? 1.4f : 1.0f);
    cv.text(label, D2D1::RectF(r.left + 10.0f, r.top, std::max(r.left + 10.0f, r.right - 26.0f), r.bottom), Font::Small, fg);
    cv.icon(open ? glyph::chevron_up : glyph::chevron_down, rect(r.right - 24.0f, r.top, 18.0f, height(r)), chev, Font::IconSmall);
    if (id != 0) cv.hit(r, id, idx);
}

inline void draw_dropdown_popup(Canvas& cv, D2D1_RECT_F anchor, D2D1_SIZE_F viewport,
                                std::span<const DropdownOption> opts, int item_id, int hover_id, int hover_idx) {
    if (opts.empty()) return;
    constexpr float item_h = 30.0f;
    constexpr float pad_y = 6.0f;
    const float max_h = std::max(item_h + (pad_y * 2.0f), viewport.height - 24.0f);
    const float content_h = (pad_y * 2.0f) + (static_cast<float>(opts.size()) * item_h);
    const float pop_h = std::min(max_h, content_h);
    float pop_w = std::max(width(anchor), 240.0f);
    for (const auto& o : opts) {
        pop_w = std::max(pop_w, cv.measure(o.label, Font::Small) + 46.0f);
    }
    pop_w = std::min(pop_w, std::max(160.0f, viewport.width - 24.0f));

    const float px = std::clamp(anchor.left, 12.0f, std::max(12.0f, viewport.width - pop_w - 12.0f));
    const float below_space = viewport.height - anchor.bottom - 16.0f;
    const float above_space = anchor.top - 16.0f;
    float py = anchor.bottom + 4.0f;
    if (below_space < pop_h && above_space > below_space) {
        py = anchor.top - pop_h - 4.0f;
    }
    py = std::clamp(py, 12.0f, std::max(12.0f, viewport.height - pop_h - 12.0f));

    const D2D1_RECT_F panel = rect(px, py, pop_w, pop_h);
    cv.shadow(panel, 10.0f, 10.0f, 0.48f);
    cv.fill_round(panel, 10.0f, with_alpha(cv.theme().surface, 0.98f));
    cv.stroke_round(panel, 10.0f, cv.theme().outline_variant, 1.0f);

    cv.push_clip(inset(panel, 2.0f, 2.0f));
    float iy = py + pad_y;
    for (const auto& o : opts) {
        if (iy >= panel.bottom - 4.0f) break;
        const D2D1_RECT_F row = rect(px + 6.0f, iy, pop_w - 12.0f, item_h);
        const bool hov = (hover_id == item_id && hover_idx == o.index);
        if (o.selected) {
            cv.fill_round(row, 7.0f, with_alpha(cv.theme().primary_container, hov ? 0.85f : 0.65f));
        } else if (hov) {
            cv.fill_round(row, 7.0f, with_alpha(cv.theme().on_surface, 0.08f));
        }
        if (o.selected) {
            cv.icon(glyph::check, rect(row.left + 6.0f, row.top, 18.0f, item_h), cv.theme().primary, Font::IconSmall);
        }
        const D2D1_COLOR_F tfg = o.selected ? cv.theme().on_primary_container : cv.theme().on_surface;
        cv.text(o.label, D2D1::RectF(row.left + 28.0f, row.top, row.right - 8.0f, row.bottom),
                o.selected ? Font::SmallSemi : Font::Small, tfg);
        cv.hit(row, item_id, o.index);
        iy += item_h;
    }
    cv.pop_clip();
}

} // namespace capusy::ui
