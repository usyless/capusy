#pragma once

#include "canvas.hpp"
#include "flow.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace capusy::ui {

class Flow {
public:
    Flow(float gap_x, float gap_y) noexcept : gap_x_{gap_x}, gap_y_{gap_y} {}

    void add(float w, float h, std::function<void(D2D1_RECT_F)> draw, bool grow = false) {
        items_.emplace_back(w, h, grow);
        draws_.push_back(std::move(draw));
    }

    void add_group(Flow inner, float avail, bool grow = false) {
        const float group_w = std::min(inner.natural_width(), avail);
        const float group_h = inner.height(group_w);
        add(group_w, group_h, [inner = std::move(inner)](D2D1_RECT_F r) { inner.draw(r.left, r.top, width(r)); }, grow);
    }

    [[nodiscard]] float natural_width() const noexcept {
        float total = 0.0f;
        for (const FlowItem& item : items_) total += item.width;
        return items_.empty() ? 0.0f : (total + (gap_x_ * static_cast<float>(items_.size() - 1)));
    }

    [[nodiscard]] float height(float avail) const {
        return flow_layout(items_, gap_x_, gap_y_, avail).height;
    }

    void draw(float x, float y, float avail) const {
        const FlowResult layout = flow_layout(items_, gap_x_, gap_y_, avail);
        for (size_t i = 0; i < items_.size(); ++i) {
            const FlowPlacement& p = layout.placements[i];
            draws_[i](rect(x + p.x, y + p.y, p.width, items_[i].height));
        }
    }

private:
    float gap_x_;
    float gap_y_;
    std::vector<FlowItem> items_;
    std::vector<std::function<void(D2D1_RECT_F)>> draws_;
};

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

} // namespace capusy::ui
