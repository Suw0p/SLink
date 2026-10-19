// Direct2D/DirectWrite 保留式自绘元素实现，集中处理主题色、文本格式、图标、命中测试
// 与设备资源丢失恢复。
#include "ui/retained_ui.hpp"

#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <cmath>
#include <d2d1helper.h>
#include <utility>
#include <windowsx.h>

namespace SLink::ui::detail {
namespace {

[[nodiscard]] D2D1_COLOR_F ThemeColor(const COLORREF color, const float alpha = 1.0F) noexcept {
    return D2D1::ColorF(static_cast<float>(GetRValue(color)) / 255.0F,
                        static_cast<float>(GetGValue(color)) / 255.0F,
                        static_cast<float>(GetBValue(color)) / 255.0F,
                        alpha);
}

const D2D1_COLOR_F kOnline = D2D1::ColorF(0x43c26b);
const D2D1_COLOR_F kOffline = D2D1::ColorF(0x72869a);

template <typename T> void Release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

float Width(const D2D1_RECT_F rect) {
    return std::max(0.0F, rect.right - rect.left);
}

float Height(const D2D1_RECT_F rect) {
    return std::max(0.0F, rect.bottom - rect.top);
}

bool Interactive(UiRole role) {
    switch (role) {
    case UiRole::navigation:
    case UiRole::sidebar_item:
    case UiRole::tab:
    case UiRole::file_tab:
    case UiRole::tab_close:
    case UiRole::primary_button:
    case UiRole::ghost_button:
    case UiRole::icon_button:
    case UiRole::toggle:
    case UiRole::window_button:
    case UiRole::window_close_button:
        return true;
    default:
        return false;
    }
}

struct SharedRenderResources {
    ID2D1Factory* d2d_factory{};
    IDWriteFactory* dwrite_factory{};
    ID2D1DCRenderTarget* target{};
    ID2D1SolidColorBrush* brush{};

    ~SharedRenderResources() {
        Release(brush);
        Release(target);
        Release(dwrite_factory);
        Release(d2d_factory);
    }

    bool EnsureFactories() {
        if (!d2d_factory &&
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory))) {
            return false;
        }
        if (!dwrite_factory &&
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&dwrite_factory)))) {
            return false;
        }
        return true;
    }

    bool EnsureTarget() {
        if (target && brush) {
            return true;
        }
        ResetTarget();
        if (!EnsureFactories()) {
            return false;
        }
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0F,
            96.0F);
        if (FAILED(d2d_factory->CreateDCRenderTarget(&properties, &target))) {
            return false;
        }
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        if (FAILED(target->CreateSolidColorBrush(ThemeColor(theme::kTextColor), &brush))) {
            ResetTarget();
            return false;
        }
        return true;
    }

    void ResetTarget() noexcept {
        Release(brush);
        Release(target);
    }
};

SharedRenderResources& SharedResources() {
    static SharedRenderResources resources;
    return resources;
}

} // namespace

IDWriteFactory* SharedDWriteFactory() noexcept {
    auto& shared = SharedResources();
    return shared.EnsureFactories() ? shared.dwrite_factory : nullptr;
}

SharedDcRenderContext BindSharedDcRenderTarget(HDC dc, const RECT& bounds) noexcept {
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return {};
    }
    auto& shared = SharedResources();
    if (!shared.EnsureTarget() || FAILED(shared.target->BindDC(dc, &bounds))) {
        return {};
    }
    return {shared.target, shared.brush};
}

void ResetSharedDcRenderTarget() noexcept {
    SharedResources().ResetTarget();
}

D2D1_COLOR_F UiColor(const COLORREF color, const float alpha) noexcept {
    return D2D1::ColorF(static_cast<float>(GetRValue(color)) / 255.0F,
                        static_cast<float>(GetGValue(color)) / 255.0F,
                        static_cast<float>(GetBValue(color)) / 255.0F,
                        alpha);
}

UiSurface::~UiSurface() {
    Release(body_format_);
    Release(semibold_format_);
    Release(compact_tab_format_);
    Release(heading_format_);
    Release(caption_format_);
    dwrite_factory_ = nullptr;
    target_ = nullptr;
    brush_ = nullptr;
}

bool UiSurface::Initialize(HWND window) {
    window_ = window;
    dpi_ = WindowDpi(window);
    dwrite_factory_ = SharedDWriteFactory();
    if (!dwrite_factory_) {
        return false;
    }
    return EnsureTextFormats();
}

void UiSurface::DpiChanged() {
    if (!window_) {
        return;
    }
    const UINT next_dpi = WindowDpi(window_);
    if (next_dpi == dpi_) {
        return;
    }
    dpi_ = next_dpi;
    Release(body_format_);
    Release(semibold_format_);
    Release(compact_tab_format_);
    Release(heading_format_);
    Release(caption_format_);
    ResetDeviceResources();
    EnsureTextFormats();
    InvalidateRect(window_, nullptr, FALSE);
}

void UiSurface::ResetDeviceResources() {
    target_ = nullptr;
    brush_ = nullptr;
    ResetSharedDcRenderTarget();
}

void UiSurface::Resize(const UINT, const UINT) {}

void UiSurface::ClearElements() {
    elements_.clear();
}

UiElement& UiSurface::Add(UiElement element) {
    elements_.push_back(std::move(element));
    return elements_.back();
}

const std::vector<UiElement>& UiSurface::elements() const noexcept {
    return elements_;
}

bool UiSurface::EnsureTextFormats() {
    if (body_format_ && semibold_format_ && compact_tab_format_ && heading_format_ &&
        caption_format_) {
        return true;
    }
    const float scale = static_cast<float>(dpi_) / 96.0F;
    const auto create =
        [&](const float size, const DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** format) {
            return dwrite_factory_->CreateTextFormat(L"Segoe UI",
                                                     nullptr,
                                                     weight,
                                                     DWRITE_FONT_STYLE_NORMAL,
                                                     DWRITE_FONT_STRETCH_NORMAL,
                                                     size * scale,
                                                     L"zh-CN",
                                                     format);
        };
    if (FAILED(create(13.5F, DWRITE_FONT_WEIGHT_NORMAL, &body_format_)) ||
        FAILED(create(13.5F, DWRITE_FONT_WEIGHT_SEMI_BOLD, &semibold_format_)) ||
        FAILED(create(12.5F, DWRITE_FONT_WEIGHT_MEDIUM, &compact_tab_format_)) ||
        FAILED(create(18.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD, &heading_format_)) ||
        FAILED(create(12.5F, DWRITE_FONT_WEIGHT_NORMAL, &caption_format_))) {
        return false;
    }
    for (IDWriteTextFormat* format :
         {body_format_, semibold_format_, compact_tab_format_, heading_format_, caption_format_}) {
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return true;
}

bool UiSurface::EnsureDeviceResources(HDC dc, const RECT& bounds) {
    if (!window_) {
        return false;
    }
    const SharedDcRenderContext context = BindSharedDcRenderTarget(dc, bounds);
    if (!context) {
        return false;
    }
    target_ = context.target;
    brush_ = context.brush;
    return true;
}

ID2D1SolidColorBrush* UiSurface::Brush(const D2D1_COLOR_F color) {
    if (brush_) {
        brush_->SetColor(color);
    }
    return brush_;
}

void UiSurface::Paint(const D2D1_COLOR_F clear_color) {
    PAINTSTRUCT paint{};
    BeginPaint(window_, &paint);
    RECT bounds{};
    GetClientRect(window_, &bounds);
    if (!EnsureDeviceResources(paint.hdc, bounds)) {
        EndPaint(window_, &paint);
        return;
    }

    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(clear_color);
    for (const UiElement& element : elements_) {
        if (element.visible && Width(element.bounds) > 0.0F && Height(element.bounds) > 0.0F) {
            DrawElement(element);
        }
    }
    if (overlay_accent_ && Width(*overlay_accent_) > 0.0F &&
        Height(*overlay_accent_) > 0.0F) {
        target_->FillRoundedRectangle(D2D1::RoundedRect(*overlay_accent_, 1.0F, 1.0F),
                                      Brush(ThemeColor(theme::kAccentColor)));
    }
    const HRESULT result = target_->EndDraw();
    target_ = nullptr;
    brush_ = nullptr;
    if (result == D2DERR_RECREATE_TARGET) {
        ResetSharedDcRenderTarget();
    }
    EndPaint(window_, &paint);
}

void UiSurface::SetOverlayAccent(std::optional<D2D1_RECT_F> bounds) noexcept {
    overlay_accent_ = std::move(bounds);
}

void UiSurface::DrawElement(const UiElement& element) {
    const D2D1_COLOR_F kPanel = ThemeColor(theme::kPanelColor);
    const D2D1_COLOR_F kPanelRaised = ThemeColor(theme::kPanelAltColor);
    const D2D1_COLOR_F kPanelHover = ThemeColor(theme::kPanelHoverColor);
    const D2D1_COLOR_F kTerminalSurface = kPanel;
    const D2D1_COLOR_F kBorder = ThemeColor(theme::kBorderColor);
    const D2D1_COLOR_F kBorderBright =
        ThemeColor(theme::IsDark() ? RGB(74, 74, 74) : RGB(195, 195, 195));
    const D2D1_COLOR_F kText = ThemeColor(theme::kTextColor);
    const D2D1_COLOR_F kTextStrong =
        ThemeColor(theme::IsDark() ? RGB(248, 248, 248) : RGB(24, 24, 24));
    const D2D1_COLOR_F kMuted = ThemeColor(theme::kMutedTextColor);
    const D2D1_COLOR_F kAccent = ThemeColor(theme::kAccentColor);
    const D2D1_COLOR_F kAccentHover =
        ThemeColor(theme::IsDark() ? RGB(144, 169, 226) : RGB(64, 94, 163));
    const D2D1_COLOR_F kAccentSoft = ThemeColor(theme::kSelectedRowColor);
    const bool hovered = element.enabled && element.id != 0 && element.id == hovered_id_;
    const bool pressed = element.enabled && element.id != 0 && element.id == pressed_id_;
    D2D1_COLOR_F text_color = element.enabled ? kText : kMuted;
    D2D1_COLOR_F fill = D2D1::ColorF(0, 0.0F);
    D2D1_COLOR_F border = D2D1::ColorF(0, 0.0F);
    float radius = 0.0F;
    float border_width = 0.0F;

    switch (element.role) {
    case UiRole::heading:
        text_color = kTextStrong;
        break;
    case UiRole::muted:
        text_color = kMuted;
        break;
    case UiRole::navigation:
        fill = element.selected ? kPanelHover : (hovered ? kPanelRaised : D2D1::ColorF(0, 0.0F));
        text_color = element.selected || hovered ? kTextStrong : kMuted;
        radius = 7.0F;
        break;
    case UiRole::sidebar_item:
        fill = element.selected ? kAccentSoft : (hovered ? kPanelHover : D2D1::ColorF(0, 0.0F));
        text_color = element.selected ? kTextStrong : kText;
        radius = 7.0F;
        break;
    case UiRole::tab:
        fill = element.selected ? kAccentSoft : (hovered ? kPanelHover : kPanel);
        border = element.selected ? kBorderBright : kBorder;
        border_width = element.selected ? 1.0F : 0.0F;
        text_color = element.selected ? kTextStrong : kMuted;
        radius = 7.0F;
        break;
    case UiRole::file_tab:
        fill = element.selected ? kTerminalSurface : (hovered ? kPanelHover : kPanel);
        border = kBorder;
        border_width = 0.0F;
        text_color = element.selected ? kTextStrong : kMuted;
        radius = 7.0F;
        break;
    case UiRole::tab_close:
        fill = pressed ? kAccentSoft : (hovered ? kPanelHover : D2D1::ColorF(0, 0.0F));
        text_color = hovered ? kTextStrong : kMuted;
        radius = 4.0F;
        break;
    case UiRole::primary_button:
        fill = pressed ? kAccent : (hovered ? kAccentHover : kAccent);
        text_color = theme::IsDark() ? kPanel : D2D1::ColorF(0xffffff);
        radius = 7.0F;
        break;
    case UiRole::ghost_button:
    case UiRole::icon_button:
        fill = pressed ? kAccentSoft : (hovered ? kPanelHover : kPanelRaised);
        border = hovered ? kBorderBright : kBorder;
        border_width = hovered ? 1.0F : 0.0F;
        radius = 7.0F;
        break;
    case UiRole::toggle:
        fill = element.selected ? kAccent : (hovered ? kPanelHover : kPanelRaised);
        border = element.selected ? kAccentHover : kBorder;
        border_width = 1.0F;
        radius = std::max(1.0F, Height(element.bounds) / 2.0F);
        break;
    case UiRole::window_button:
        fill = pressed ? kPanelRaised : (hovered ? kPanelHover : D2D1::ColorF(0, 0.0F));
        text_color = hovered ? kTextStrong : kMuted;
        break;
    case UiRole::window_close_button:
        fill = pressed ? D2D1::ColorF(0x9f1f17)
                       : (hovered ? D2D1::ColorF(0xc42b1c) : D2D1::ColorF(0, 0.0F));
        text_color = hovered || pressed ? kTextStrong : kMuted;
        break;
    case UiRole::panel:
        fill = kPanel;
        break;
    case UiRole::card:
        fill = kPanel;
        border = kBorder;
        border_width = 1.0F;
        radius = 12.0F;
        break;
    case UiRole::drop_target:
        fill = ThemeColor(theme::kAccentColor, 0.28F);
        border = kAccentHover;
        border_width = 2.0F;
        radius = 6.0F;
        break;
    case UiRole::tab_insert:
        fill = kAccentHover;
        radius = 1.0F;
        break;
    case UiRole::separator:
        fill = kBorder;
        break;
    case UiRole::accent_line:
        fill = kAccent;
        radius = 1.0F;
        break;
    case UiRole::status:
        text_color = element.status_online ? kOnline : kMuted;
        break;
    case UiRole::label:
        break;
    }

    if (fill.a > 0.0F) {
        if (radius > 0.0F) {
            target_->FillRoundedRectangle(D2D1::RoundedRect(element.bounds, radius, radius),
                                          Brush(fill));
        } else {
            target_->FillRectangle(element.bounds, Brush(fill));
        }
    }
    if (border_width > 0.0F) {
        target_->DrawRoundedRectangle(
            D2D1::RoundedRect(element.bounds, radius, radius), Brush(border), border_width);
    }
    if (element.selected && element.role == UiRole::file_tab) {
        target_->FillRectangle(UiRect(element.bounds.left,
                                      std::max(element.bounds.top, element.bounds.bottom - 9.0F),
                                      element.bounds.right,
                                      element.bounds.bottom),
                               Brush(kTerminalSurface));
    }
    if (element.role == UiRole::toggle) {
        const float height = Height(element.bounds);
        const float knob_radius = std::max(2.0F, height * 0.34F);
        const float knob_x = element.selected ? element.bounds.right - height / 2.0F
                                              : element.bounds.left + height / 2.0F;
        const float knob_y = (element.bounds.top + element.bounds.bottom) / 2.0F;
        const D2D1_COLOR_F knob =
            element.enabled ? (theme::IsDark() ? D2D1::ColorF(0xf3f3f3) : D2D1::ColorF(0xffffff))
                            : kMuted;
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, knob_y), knob_radius, knob_radius),
                             Brush(knob));
    }

    D2D1_RECT_F content = element.bounds;
    const float padding = 10.0F;
    if (element.icon != UiIcon::none) {
        const float icon_size = std::min(16.0F, std::max(10.0F, Height(content) - 14.0F));
        D2D1_RECT_F icon_rect{};
        if (element.alignment == UiTextAlign::center && element.text.empty()) {
            icon_rect = UiRect(content.left + (Width(content) - icon_size) / 2.0F,
                               content.top + (Height(content) - icon_size) / 2.0F,
                               content.left + (Width(content) + icon_size) / 2.0F,
                               content.top + (Height(content) + icon_size) / 2.0F);
        } else {
            icon_rect = UiRect(content.left + padding,
                               content.top + (Height(content) - icon_size) / 2.0F,
                               content.left + padding + icon_size,
                               content.top + (Height(content) + icon_size) / 2.0F);
            content.left = icon_rect.right + 7.0F;
        }
        DrawIcon(element.icon, icon_rect, Brush(text_color));
    } else {
        content.left += padding;
    }
    content.right -= padding;
    if (!element.text.empty()) {
        DrawText(element, content, Brush(text_color));
    }
}

void UiSurface::DrawText(const UiElement& element, D2D1_RECT_F bounds, ID2D1Brush* brush) {
    IDWriteTextFormat* format = body_format_;
    if (element.role == UiRole::heading) {
        format = heading_format_;
    } else if (element.role == UiRole::navigation || element.role == UiRole::tab ||
               element.role == UiRole::file_tab || element.role == UiRole::primary_button) {
        format = element.role == UiRole::file_tab && element.compact_text ? compact_tab_format_
                                                                          : semibold_format_;
    } else if (element.role == UiRole::muted || element.role == UiRole::status) {
        format = caption_format_;
    }
    switch (element.alignment) {
    case UiTextAlign::leading:
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        break;
    case UiTextAlign::center:
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        break;
    case UiTextAlign::trailing:
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        break;
    }
    target_->DrawTextW(element.text.c_str(),
                       static_cast<UINT32>(element.text.size()),
                       format,
                       bounds,
                       brush,
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void UiSurface::DrawIcon(const UiIcon icon, const D2D1_RECT_F bounds, ID2D1Brush* brush) {
    const float left = bounds.left;
    const float top = bounds.top;
    const float right = bounds.right;
    const float bottom = bounds.bottom;
    const float cx = (left + right) / 2.0F;
    const float cy = (top + bottom) / 2.0F;
    const float stroke = 1.45F;
    const auto line = [&](float x1, float y1, float x2, float y2) {
        target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, stroke);
    };

    switch (icon) {
    case UiIcon::terminal:
        target_->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 2.0F, 2.0F), brush, stroke);
        line(left + 3.0F, top + 4.0F, left + 6.0F, cy);
        line(left + 6.0F, cy, left + 3.0F, bottom - 4.0F);
        line(left + 8.0F, bottom - 4.0F, right - 3.0F, bottom - 4.0F);
        break;
    case UiIcon::transfer:
        line(left + 1.0F, top + 5.0F, right - 3.0F, top + 5.0F);
        line(right - 6.0F, top + 2.0F, right - 3.0F, top + 5.0F);
        line(right - 3.0F, top + 5.0F, right - 6.0F, top + 8.0F);
        line(right - 1.0F, bottom - 5.0F, left + 3.0F, bottom - 5.0F);
        line(left + 6.0F, bottom - 8.0F, left + 3.0F, bottom - 5.0F);
        line(left + 3.0F, bottom - 5.0F, left + 6.0F, bottom - 2.0F);
        break;
    case UiIcon::edit:
        target_->DrawRectangle(
            UiRect(left + 1.5F, top + 3.5F, right - 3.5F, bottom - 1.5F), brush, stroke);
        line(left + 5.0F, bottom - 5.0F, right - 1.0F, top + 1.0F);
        line(right - 3.5F, top + 1.0F, right - 1.0F, top + 3.5F);
        break;
    case UiIcon::settings:
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 4.0F, 4.0F), brush, stroke);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 1.0F, 1.0F), brush, stroke);
        for (int index = 0; index < 8; ++index) {
            const float angle = static_cast<float>(index) * 3.14159265F / 4.0F;
            line(cx + std::cos(angle) * 5.0F,
                 cy + std::sin(angle) * 5.0F,
                 cx + std::cos(angle) * 7.0F,
                 cy + std::sin(angle) * 7.0F);
        }
        break;
    case UiIcon::connect:
        target_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx - 3.0F, cy), 3.5F, 3.5F), brush, stroke);
        target_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx + 3.0F, cy), 3.5F, 3.5F), brush, stroke);
        line(cx - 1.5F, cy, cx + 1.5F, cy);
        break;
    case UiIcon::server:
        target_->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 2.0F, 2.0F), brush, stroke);
        line(left + 2.5F, cy, right - 2.5F, cy);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 4.0F, top + 4.0F), 1.0F, 1.0F),
                             brush);
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 4.0F, bottom - 4.0F), 1.0F, 1.0F),
                             brush);
        break;
    case UiIcon::sidebar_toggle:
        target_->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 3.0F, 3.0F), brush, stroke);
        line(left + 6.0F, top + 1.0F, left + 6.0F, bottom - 1.0F);
        break;
    case UiIcon::close:
        line(left + 3.0F, top + 3.0F, right - 3.0F, bottom - 3.0F);
        line(right - 3.0F, top + 3.0F, left + 3.0F, bottom - 3.0F);
        break;
    case UiIcon::plus:
        line(cx, top + 2.0F, cx, bottom - 2.0F);
        line(left + 2.0F, cy, right - 2.0F, cy);
        break;
    case UiIcon::chevron:
        line(left + 4.0F, top + 3.0F, right - 4.0F, cy);
        line(right - 4.0F, cy, left + 4.0F, bottom - 3.0F);
        break;
    case UiIcon::chevron_left:
        line(right - 4.0F, top + 3.0F, left + 4.0F, cy);
        line(left + 4.0F, cy, right - 4.0F, bottom - 3.0F);
        break;
    case UiIcon::folder:
        target_->DrawRoundedRectangle(
            D2D1::RoundedRect(UiRect(left, top + 4.0F, right, bottom), 1.5F, 1.5F), brush, stroke);
        line(left + 1.0F, top + 4.0F, left + 6.0F, top + 4.0F);
        line(left + 2.0F, top + 2.0F, left + 6.0F, top + 2.0F);
        line(left + 6.0F, top + 2.0F, left + 8.0F, top + 4.0F);
        break;
    case UiIcon::refresh:
        line(cx - 5.0F, cy - 3.0F, cx - 5.0F, cy - 6.0F);
        line(cx - 5.0F, cy - 6.0F, cx - 2.0F, cy - 6.0F);
        line(cx + 5.0F, cy + 3.0F, cx + 5.0F, cy + 6.0F);
        line(cx + 5.0F, cy + 6.0F, cx + 2.0F, cy + 6.0F);
        target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 5.0F, 5.0F), brush, stroke);
        break;
    case UiIcon::minimize:
        line(left + 3.0F, cy, right - 3.0F, cy);
        break;
    case UiIcon::maximize:
        target_->DrawRectangle(
            UiRect(left + 2.5F, top + 2.5F, right - 2.5F, bottom - 2.5F), brush, stroke);
        break;
    case UiIcon::restore:
        target_->DrawRectangle(
            UiRect(left + 4.0F, top + 1.5F, right - 1.5F, bottom - 4.0F), brush, stroke);
        target_->DrawRectangle(
            UiRect(left + 1.5F, top + 4.0F, right - 4.0F, bottom - 1.5F), brush, stroke);
        break;
    case UiIcon::none:
        break;
    }
}

const UiElement* UiSurface::HitTest(const POINT point) const noexcept {
    for (auto iterator = elements_.rbegin(); iterator != elements_.rend(); ++iterator) {
        const UiElement& element = *iterator;
        if (!element.visible || !element.enabled || element.id == 0 || !Interactive(element.role)) {
            continue;
        }
        if (point.x >= element.bounds.left && point.x < element.bounds.right &&
            point.y >= element.bounds.top && point.y < element.bounds.bottom) {
            return &element;
        }
    }
    return nullptr;
}

bool UiSurface::PointerMove(const POINT point) {
    const UiElement* element = HitTest(point);
    const int next = element ? element->id : 0;
    if (!tracking_mouse_ && window_) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
        TrackMouseEvent(&tracking);
        tracking_mouse_ = true;
    }
    if (next == hovered_id_) {
        return false;
    }
    hovered_id_ = next;
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

void UiSurface::PointerLeave() {
    tracking_mouse_ = false;
    if (hovered_id_ != 0) {
        hovered_id_ = 0;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

bool UiSurface::PointerDown(const POINT point) {
    const UiElement* element = HitTest(point);
    pressed_id_ = element ? element->id : 0;
    if (pressed_id_ != 0) {
        SetCapture(window_);
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }
    return false;
}

std::optional<int> UiSurface::PointerUp(const POINT point) {
    const int pressed = pressed_id_;
    const UiElement* element = HitTest(point);
    pressed_id_ = 0;
    if (GetCapture() == window_) {
        ReleaseCapture();
    }
    InvalidateRect(window_, nullptr, FALSE);
    if (pressed != 0 && element && element->id == pressed) {
        return pressed;
    }
    return std::nullopt;
}

void UiSurface::CancelPointer() {
    pressed_id_ = 0;
    if (GetCapture() == window_) {
        ReleaseCapture();
    }
    InvalidateRect(window_, nullptr, FALSE);
}

int UiSurface::hovered_id() const noexcept {
    return hovered_id_;
}

int UiSurface::pressed_id() const noexcept {
    return pressed_id_;
}

} // namespace SLink::ui::detail
