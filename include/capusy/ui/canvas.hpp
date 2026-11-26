#pragma once

#include "anim.hpp"
#include "fit.hpp"
#include "gfx.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <ranges>
#include <unordered_map>

namespace capusy::ui {

[[nodiscard]] inline D2D1_RECT_F lerp_rect(D2D1_RECT_F a, D2D1_RECT_F b, float t) noexcept {
    return {.left = lerp(a.left, b.left, t), .top = lerp(a.top, b.top, t), .right = lerp(a.right, b.right, t), .bottom = lerp(a.bottom, b.bottom, t)};
}

[[nodiscard]] inline float indicator_hover_fade(float coverage) noexcept {
    const float x = std::clamp((1.0f - coverage) * 4.0f, 0.0f, 1.0f);
    return x * x * (3.0f - (2.0f * x));
}

namespace glyph {
inline constexpr wchar_t settings = 0xE713;
inline constexpr wchar_t close = 0xE8BB;
inline constexpr wchar_t play = 0xF5B0;
inline constexpr wchar_t pause = 0xF8AE;
inline constexpr wchar_t stop = 0xE71A;
inline constexpr wchar_t folder = 0xE8B7;
inline constexpr wchar_t open = 0xE8A7;
inline constexpr wchar_t copy = 0xE8C8;
inline constexpr wchar_t check = 0xE73E;
inline constexpr wchar_t photo = 0xEB9F;
inline constexpr wchar_t video = 0xE714;
inline constexpr wchar_t camera = 0xE722;
inline constexpr wchar_t crop = 0xE7A8;
inline constexpr wchar_t clock = 0xE823;
inline constexpr wchar_t refresh = 0xE72C;
inline constexpr wchar_t del = 0xE74D;
inline constexpr wchar_t keyboard = 0xE765;
inline constexpr wchar_t warning = 0xE7BA;
inline constexpr wchar_t info = 0xE946;
inline constexpr wchar_t volume = 0xE767;
inline constexpr wchar_t mute = 0xE74F;
inline constexpr wchar_t cursor = 0xE8B0;
inline constexpr wchar_t pen = 0xEE56;
inline constexpr wchar_t highlighter = 0xE7E6;
inline constexpr wchar_t shapes = 0xF158;
inline constexpr wchar_t undo = 0xE7A7;
inline constexpr wchar_t redo = 0xE7A6;
inline constexpr wchar_t history = 0xE81C;
inline constexpr wchar_t monitor = 0xE7F4;
} // namespace glyph

enum class Font : uint8_t {
    Body,       // 13
    BodySemi,
    BodyWrap,
    Large,      // 15 semibold
    Small,      // 12
    SmallSemi,
    SmallWrap,
    Caption,    // 11
    Label,      // 11 bold
    Mono,       // 12
    MonoSmall,  // 11
    Title,      // 17 semibold
    Icon,       // 16
    IconSmall,  // 12
    IconLarge,  // 20
    Count,
};

enum class Align : uint8_t { Left, Center, Right };

class Fonts {
public:
    bool init(IDWriteFactory3* dw) {
        dw_ = dw;
        wil::com_ptr<IDWriteFontCollection> sys;
        if (dw_) dw_->GetSystemFontCollection(&sys);
        const std::wstring text = pick(sys.get(), L"Segoe UI Variable Text", L"Segoe UI");
        const std::wstring disp = pick(sys.get(), L"Segoe UI Variable Display", L"Segoe UI");
        const std::wstring mono = pick(sys.get(), L"Cascadia Mono", L"Consolas");
        const std::wstring icon = pick(sys.get(), L"Segoe Fluent Icons", L"Segoe MDL2 Assets");

        struct Def {
            Font f;
            const std::wstring* family;
            float size;
            DWRITE_FONT_WEIGHT weight;
            bool wrap;
            bool is_icon;
        };
        const std::array<Def, static_cast<size_t>(Font::Count)> defs = {{
            {.f = Font::Body, .family = &text, .size = 13.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::BodySemi, .family = &text, .size = 13.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::BodyWrap, .family = &text, .size = 13.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = true, .is_icon = false},
            {.f = Font::Large, .family = &disp, .size = 15.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::Small, .family = &text, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::SmallSemi, .family = &text, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::SmallWrap, .family = &text, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = true, .is_icon = false},
            {.f = Font::Caption, .family = &text, .size = 11.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::Label, .family = &text, .size = 11.0f, .weight = DWRITE_FONT_WEIGHT_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::Mono, .family = &mono, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::MonoSmall, .family = &mono, .size = 11.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = false},
            {.f = Font::Title, .family = &disp, .size = 17.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .wrap = false, .is_icon = false},
            {.f = Font::Icon, .family = &icon, .size = 16.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = true},
            {.f = Font::IconSmall, .family = &icon, .size = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = true},
            {.f = Font::IconLarge, .family = &icon, .size = 20.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .wrap = false, .is_icon = true},
        }};
        for (const auto& d : defs) {
            wil::com_ptr<IDWriteTextFormat> fmt;
            if (FAILED(dw->CreateTextFormat(d.family->c_str(), nullptr, d.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, d.size, L"en-us", &fmt))) return false;
            fmt->SetWordWrapping(d.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            fmt->SetParagraphAlignment(d.wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (!d.wrap) {
                wil::com_ptr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt.get(), &ellipsis))) {
                    const DWRITE_TRIMMING trim{.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER, .delimiter = 0, .delimiterCount = 0};
                    fmt->SetTrimming(&trim, ellipsis.get());
                }
            }
            if (!d.is_icon) center_cap_metrics(sys.get(), fmt.get(), *d.family, d.weight, d.size);
            formats_[static_cast<size_t>(d.f)] = std::move(fmt);
        }
        return true;
    }

    void clear_caches() noexcept {
        width_cache_.clear();
        wrapped_cache_.clear();
    }

    [[nodiscard]] IDWriteTextFormat* get(Font f) const noexcept { return formats_[static_cast<size_t>(f)].get(); }
    [[nodiscard]] IDWriteFactory3* dwrite() const noexcept { return dw_; }

    float measure(std::wstring_view s, Font f) {
        if (s.empty() || !dw_) return 0.0f;
        const uint64_t key = rapidhash(s.data(), s.size() * sizeof(wchar_t)) ^ (static_cast<uint64_t>(f) * 0x9e3779b97f4a7c15ull);
        if (const auto it = width_cache_.find(key); it != width_cache_.end()) return it->second;
        wil::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(dw_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), get(f), 100000.0f, 100.0f, &layout))) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        if (width_cache_.size() > 1024) width_cache_.clear();
        width_cache_.emplace(key, m.widthIncludingTrailingWhitespace);
        return m.widthIncludingTrailingWhitespace;
    }

    struct WrappedEntry {
        std::wstring text;
        Font font = Font::Body;
        float width = 0.0f;
        Align align = Align::Left;
        D2D1_SIZE_F metrics{};
        wil::com_ptr<IDWriteTextLayout> layout;
    };

    [[nodiscard]] WrappedEntry* wrapped(std::wstring_view s, Font f, float w) {
        if (s.empty() || !dw_) return nullptr;
        const float cw = w > 1.0f ? w : 1.0f;
        const auto wbits = std::bit_cast<uint32_t>(cw);
        const uint64_t key = rapidhash(s.data(), s.size() * sizeof(wchar_t)) ^
                             (static_cast<uint64_t>(f) * 0x9e3779b97f4a7c15ull) ^
                             (static_cast<uint64_t>(wbits) * 0xbf58476d1ce4e5b9ull);
        if (const auto it = wrapped_cache_.find(key); it != wrapped_cache_.end()) {
            WrappedEntry& e = it->second;
            if (e.text == s && e.font == f && e.width == cw) return &e;
        }
        wil::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(dw_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), get(f), cw, 10000.0f, &layout))) {
            return nullptr;
        }
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const DWRITE_TRIMMING no_trim{.granularity = DWRITE_TRIMMING_GRANULARITY_NONE, .delimiter = 0, .delimiterCount = 0};
        layout->SetTrimming(&no_trim, nullptr);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        if (wrapped_cache_.size() >= 256) wrapped_cache_.clear();
        auto [it, inserted] = wrapped_cache_.insert_or_assign(
            key, WrappedEntry{
                     .text = std::wstring{s},
                     .font = f,
                     .width = cw,
                     .align = Align::Left,
                     .metrics = D2D1_SIZE_F{.width = m.widthIncludingTrailingWhitespace, .height = m.height},
                     .layout = std::move(layout),
                 });
        return &it->second;
    }

private:
    static std::wstring pick(IDWriteFontCollection* sys, const wchar_t* preferred, const wchar_t* fallback) {
        if (sys) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sys->FindFamilyName(preferred, &idx, &exists)) && exists) return preferred;
        }
        return fallback;
    }

    void center_cap_metrics(IDWriteFontCollection* sys, IDWriteTextFormat* fmt, const std::wstring& family, DWRITE_FONT_WEIGHT weight, float size) const noexcept {
        if (!sys || !fmt || !dw_) return;
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (FAILED(sys->FindFamilyName(family.c_str(), &idx, &exists)) || !exists) return;
        wil::com_ptr<IDWriteFontFamily> fam;
        wil::com_ptr<IDWriteFont> font;
        if (FAILED(sys->GetFontFamily(idx, &fam)) ||
            FAILED(fam->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font))) {
            return;
        }
        DWRITE_FONT_METRICS fm{};
        font->GetMetrics(&fm);
        if (fm.designUnitsPerEm == 0 || fm.capHeight == 0) return;

        wil::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(dw_->CreateTextLayout(L"H", 1, fmt, 1000.0f, 1000.0f, &layout))) return;
        DWRITE_LINE_METRICS lm{};
        UINT32 count = 0;
        if (FAILED(layout->GetLineMetrics(&lm, 1, &count)) || count == 0 || lm.height <= 0.0f) return;

        const float cap_h = static_cast<float>(fm.capHeight) * size / static_cast<float>(fm.designUnitsPerEm);
        if (lm.height <= cap_h) return;
        fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lm.height, (lm.height + cap_h) * 0.5f);
    }

    IDWriteFactory3* dw_ = nullptr;
    std::array<wil::com_ptr<IDWriteTextFormat>, static_cast<size_t>(Font::Count)> formats_;
    std::unordered_map<uint64_t, float> width_cache_;
    std::unordered_map<uint64_t, WrappedEntry> wrapped_cache_;
};

struct Hit {
    D2D1_RECT_F r;
    int id;
    int index;
};

inline bool contains(const D2D1_RECT_F& r, D2D1_POINT_2F p) noexcept {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}
inline D2D1_RECT_F rect(float x, float y, float w, float h) noexcept { return D2D1::RectF(x, y, x + w, y + h); }
inline D2D1_RECT_F inset(D2D1_RECT_F r, float dx, float dy) noexcept { return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy); }
inline float width(const D2D1_RECT_F& r) noexcept { return r.right - r.left; }
inline float height(const D2D1_RECT_F& r) noexcept { return r.bottom - r.top; }

/// Immediate-mode drawing helpers over one Direct2D device context, plus hit-region recording.
class Canvas {
public:
    Canvas(WindowTarget& target, Fonts& fonts, std::vector<Hit>& hits, const Theme& theme = default_theme(), // NOLINT(cppcoreguidelines-pro-type-member-init)
           std::optional<D2D1_RECT_F> dirty_clip = std::nullopt, float opacity = 1.0f) noexcept
        : target_{target}, dc_{target.dc()}, fonts_{fonts}, hits_{hits}, theme_{theme},
          dirty_clip_{dirty_clip}, opacity_{std::clamp(opacity, 0.0f, 1.0f)} {}

    [[nodiscard]] ID2D1DeviceContext6* dc() const noexcept { return dc_; }
    [[nodiscard]] Fonts& fonts() const noexcept { return fonts_; }
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] float scale() const noexcept {
        const float s = target_.scale();
        return s > 0.0f ? s : 1.0f;
    }
    [[nodiscard]] float snap(float v) const noexcept {
        const float s = scale();
        return std::round(v * s) / s;
    }
    [[nodiscard]] D2D1_RECT_F snap(D2D1_RECT_F r) const noexcept {
        const float s = scale();
        const float l = std::round(r.left * s);
        const float t = std::round(r.top * s);
        const float ri = r.right > r.left ? std::max(l + 1.0f, std::round(r.right * s)) : std::round(r.right * s);
        const float b = r.bottom > r.top ? std::max(t + 1.0f, std::round(r.bottom * s)) : std::round(r.bottom * s);
        return D2D1::RectF(l / s, t / s, ri / s, b / s);
    }
    [[nodiscard]] std::pair<float, float> hairline(float v) const noexcept {
        const float s = scale();
        const float px = std::round(v * s);
        const float n = std::max(1.0f, std::round(s));
        return {px / s, n / s};
    }

    [[nodiscard]] bool intersects_clip(D2D1_RECT_F r, float pad = 0.0f) const noexcept {
        const float l = r.left - pad;
        const float t = r.top - pad;
        const float ri = r.right + pad;
        const float b = r.bottom + pad;
        if (dirty_clip_) {
            if (ri <= dirty_clip_->left || l >= dirty_clip_->right || b <= dirty_clip_->top || t >= dirty_clip_->bottom) {
                return false;
            }
        }
        if (clip_depth_ > 0) {
            const D2D1_RECT_F& c = current_clip();
            if (ri <= c.left || l >= c.right || b <= c.top || t >= c.bottom) {
                return false;
            }
        }
        return true;
    }

    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c) noexcept {
        c.a *= opacity_;
        ID2D1SolidColorBrush* b = target_.solid_brush();
        if (b && (!has_color_ || c.r != last_color_.r || c.g != last_color_.g || c.b != last_color_.b || c.a != last_color_.a)) {
            b->SetColor(c);
            last_color_ = c;
            has_color_ = true;
        }
        return b;
    }

    void hit(D2D1_RECT_F r, int id, int index = 0) {
        if (clip_depth_ > 0) {
            const D2D1_RECT_F& c = current_clip();
            r.left = std::max(r.left, c.left);
            r.top = std::max(r.top, c.top);
            r.right = std::min(r.right, c.right);
            r.bottom = std::min(r.bottom, c.bottom);
        }
        if (r.left < r.right && r.top < r.bottom) {
            hits_.emplace_back(r, id, index);
        }
    }

    void fill(D2D1_RECT_F r, D2D1_COLOR_F c) noexcept {
        if (!intersects_clip(r, 1.0f)) return;
        dc_->FillRectangle(r, brush(c));
    }
    void stroke(D2D1_RECT_F r, D2D1_COLOR_F c, float w = 1.0f) noexcept {
        if (!intersects_clip(r, (w * 0.5f) + 1.0f)) return;
        const float h = w * 0.5f;
        dc_->DrawRectangle(inset(r, h, h), brush(c), w);
    }
    void fill_round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) noexcept {
        if (!intersects_clip(r, 1.0f)) return;
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush(c));
    }
    void stroke_round(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float w = 1.0f) noexcept {
        if (!intersects_clip(r, (w * 0.5f) + 1.0f)) return;
        const float h = w * 0.5f;
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(inset(r, h, h), radius, radius), brush(c), w);
    }
    void fill_circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F col) noexcept {
        if (!intersects_clip(D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), 1.0f)) return;
        dc_->FillEllipse(D2D1::Ellipse(c, r, r), brush(col));
    }
    void stroke_circle(D2D1_POINT_2F c, float r, D2D1_COLOR_F col, float w = 1.0f) noexcept {
        if (!intersects_clip(D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), (w * 0.5f) + 1.0f)) return;
        dc_->DrawEllipse(D2D1::Ellipse(c, r, r), brush(col), w);
    }
    void hline(float x0, float x1, float y, D2D1_COLOR_F c) noexcept {
        const auto [top, thick] = hairline(y);
        fill(D2D1::RectF(snap(x0), top, snap(x1), top + thick), c);
    }
    void vline(float x, float y0, float y1, D2D1_COLOR_F c) noexcept {
        const auto [left, thick] = hairline(x);
        fill(D2D1::RectF(left, snap(y0), left + thick, snap(y1)), c);
    }

    void text(std::wstring_view s, D2D1_RECT_F r, Font f, D2D1_COLOR_F c, Align a = Align::Left) noexcept {
        if (s.empty() || !intersects_clip(r, 1.0f)) return;
        IDWriteTextFormat* fmt = fonts_.get(f);
        fmt->SetTextAlignment(text_alignment(a));
        dc_->DrawText(s.data(), static_cast<UINT32>(s.size()), fmt, r, brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void text_wrap(std::wstring_view s, D2D1_RECT_F r, Font f, D2D1_COLOR_F c, Align a = Align::Left) {
        if (s.empty()) return;
        Fonts::WrappedEntry* e = fonts_.wrapped(s, f, width(r));
        if (!e || !e->layout) return;
        const D2D1_RECT_F bounds = D2D1::RectF(
            r.left, r.top,
            r.left + std::max(width(r), e->metrics.width),
            r.top + std::max(height(r), e->metrics.height));
        if (!intersects_clip(bounds, 2.0f)) return;
        if (e->align != a) {
            e->layout->SetTextAlignment(text_alignment(a));
            e->align = a;
        }
        dc_->DrawTextLayout(D2D1::Point2F(r.left, r.top), e->layout.get(), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void icon(wchar_t g, D2D1_RECT_F r, D2D1_COLOR_F c, Font f = Font::Icon) noexcept {
        const std::array<wchar_t, 2> s = {g, 0};
        text(std::wstring_view{s.data(), 1}, r, f, c, Align::Center);
    }

    float measure(std::wstring_view s, Font f) { return fonts_.measure(s, f); }

    float measure_height(std::wstring_view s, Font f, float w) {
        const auto* e = fonts_.wrapped(s, f, w);
        return e ? e->metrics.height : 0.0f;
    }

    void fill_bitmap_round(ID2D1Bitmap1* bmp, D2D1_RECT_F tr, float radius, ThumbFit fit = ThumbFit::Crop) noexcept {
        if (!bmp || !intersects_clip(tr, 1.0f)) return;
        const D2D1_SIZE_F bs = bmp->GetSize();
        if (bs.width <= 0.0f || bs.height <= 0.0f) return;
        const D2D1_RECT_F box = snap(tr);
        const FitRect dest = fit_image(bs.width, bs.height, FitRect{.left = box.left, .top = box.top, .width = width(box), .height = height(box)}, fit);
        const D2D1_RECT_F drawn = fit == ThumbFit::Contain ? snap(rect(dest.left, dest.top, dest.width, dest.height)) : box;
        const float sx = fit == ThumbFit::Contain ? (width(drawn) / bs.width) : (dest.width / bs.width);
        const float sy = fit == ThumbFit::Contain ? (height(drawn) / bs.height) : sx;
        const float tx = fit == ThumbFit::Contain ? drawn.left : dest.left;
        const float ty = fit == ThumbFit::Contain ? drawn.top : dest.top;
        if (ID2D1BitmapBrush1* bb = target_.bitmap_brush(bmp)) {
            bb->SetOpacity(opacity_);
            bb->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(tx, ty));
            dc_->FillRoundedRectangle(D2D1::RoundedRect(drawn, radius, radius), bb);
        }
    }

    void bitmap(ID2D1Bitmap* bmp, D2D1_RECT_F dest,
                D2D1_INTERPOLATION_MODE mode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC) noexcept {
        if (!bmp) return;
        dest = snap(dest);
        if (!intersects_clip(dest, 1.0f)) return;
        dc_->DrawBitmap(bmp, &dest, opacity_, mode, nullptr, nullptr);
    }

    void push_clip(D2D1_RECT_F r) noexcept {
        dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        record_clip_rect(r);
    }
    void pop_clip() noexcept {
        dc_->PopAxisAlignedClip();
        if (clip_depth_ > 0) --clip_depth_;
    }

    void shadow(D2D1_RECT_F r, float radius, float spread = 10.0f, float alpha = 0.28f) noexcept {
        if (alpha <= 0.002f) return;
        const D2D1_RECT_F bounds = D2D1::RectF(r.left - (spread * 0.6f), r.top - (spread * 0.2f), r.right + (spread * 0.6f), r.bottom + spread);
        if (!intersects_clip(bounds, 1.0f)) return;
        constexpr int steps = 5;
        for (int i = steps; i >= 1; --i) {
            const float t = static_cast<float>(i) / steps;
            const float g = spread * t;
            fill_round(D2D1::RectF(r.left - (g * 0.6f), r.top - (g * 0.2f), r.right + (g * 0.6f), r.bottom + g), radius + g, hex(0x000000, alpha / steps * (1.2f - t)));
        }
    }

private:
    void record_clip_rect(D2D1_RECT_F r) noexcept {
        if (clip_depth_ > 0) {
            const D2D1_RECT_F& c = current_clip();
            r.left = std::max(r.left, c.left);
            r.top = std::max(r.top, c.top);
            r.right = std::min(r.right, c.right);
            r.bottom = std::min(r.bottom, c.bottom);
        }
        if (clip_depth_ < clips_.size()) {
            clips_[clip_depth_++] = r;
        } else {
            ++clip_depth_;
        }
    }

    [[nodiscard]] const D2D1_RECT_F& current_clip() const noexcept {
        return clips_[std::min(clip_depth_, clips_.size()) - 1];
    }

    static DWRITE_TEXT_ALIGNMENT text_alignment(Align a) noexcept {
        return a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING;
    }

    WindowTarget& target_;
    ID2D1DeviceContext6* dc_;
    Fonts& fonts_;
    std::vector<Hit>& hits_;
    const Theme& theme_;
    std::optional<D2D1_RECT_F> dirty_clip_;
    float opacity_ = 1.0f;
    D2D1_COLOR_F last_color_{};
    bool has_color_ = false;
    std::array<D2D1_RECT_F, 8> clips_; // NOLINT(cppcoreguidelines-pro-type-member-init)
    size_t clip_depth_ = 0;
};

inline const Hit* hit_test(std::span<const Hit> hits, D2D1_POINT_2F p) noexcept {
    for (const auto& h : std::views::reverse(hits)) {
        if (contains(h.r, p)) return &h;
    }
    return nullptr;
}

/// Tracks current and outgoing hover state with smooth exponential fades and zero idle repaints.
struct HoverTracker {
    int id = 0;
    int index = 0;
    int prev_id = 0;
    int prev_index = 0;
    AnimFloat in_anim;
    AnimFloat out_anim;

    bool set(int new_id, int new_index, bool animate) noexcept {
        if (new_id == id && new_index == index) return false;
        if (id != 0 && in_anim.value > 0.002f) {
            prev_id = id;
            prev_index = index;
            out_anim.snap(in_anim.value);
            out_anim.set(0.0f, animate);
        }
        id = new_id;
        index = new_index;
        in_anim.snap(0.0f);
        in_anim.set(new_id != 0 ? 1.0f : 0.0f, animate);
        if (!animate) {
            prev_id = 0;
            out_anim.snap(0.0f);
        }
        return true;
    }

    bool step(float dt) noexcept {
        bool moving = false;
        if (in_anim.step(dt, 34.0f, 0.008f)) moving = true;
        if (out_anim.step(dt, 28.0f, 0.008f)) moving = true;
        if (out_anim.value <= 0.008f) prev_id = 0;
        return moving;
    }

    [[nodiscard]] float alpha(int q_id, int q_index = 0) const noexcept {
        if (id == q_id && index == q_index) return in_anim.value;
        if (prev_id == q_id && prev_index == q_index) return out_anim.value;
        return 0.0f;
    }
};

} // namespace capusy::ui
