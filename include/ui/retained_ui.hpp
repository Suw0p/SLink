#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SLink::ui::detail {

// 页面导航、标签和按钮使用的轻量保留式自绘层。元素只保存几何、文本和状态，
// WM_PAINT 时统一通过 Direct2D/DirectWrite 绘制，不为每个按钮创建原生子窗口。
struct SharedDcRenderContext {
    ID2D1DCRenderTarget* target{};
    ID2D1SolidColorBrush* brush{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return target != nullptr && brush != nullptr;
    }
};

// 所有控件都在 UI 线程绘制，因此共享一个可重新绑定的 DC RenderTarget；这样无需为
// 每个页面和终端标签长期保留整窗 GPU Surface，降低空闲显存和工作集。
[[nodiscard]] IDWriteFactory* SharedDWriteFactory() noexcept;
[[nodiscard]] SharedDcRenderContext BindSharedDcRenderTarget(HDC dc, const RECT& bounds) noexcept;
void ResetSharedDcRenderTarget() noexcept;

enum class UiRole {
    label,
    heading,
    muted,
    navigation,
    sidebar_item,
    tab,
    file_tab,
    tab_close,
    primary_button,
    ghost_button,
    icon_button,
    toggle,
    panel,
    card,
    drop_target,
    tab_insert,
    separator,
    accent_line,
    status,
    window_button,
    window_close_button,
};

enum class UiIcon {
    none,
    terminal,
    transfer,
    edit,
    settings,
    connect,
    server,
    sidebar_toggle,
    close,
    plus,
    chevron,
    chevron_left,
    folder,
    refresh,
    minimize,
    maximize,
    restore,
};

enum class UiTextAlign {
    leading,
    center,
    trailing,
};

struct UiElement {
    int id{};
    UiRole role{UiRole::label};
    D2D1_RECT_F bounds{};
    std::wstring text;
    UiIcon icon{UiIcon::none};
    UiTextAlign alignment{UiTextAlign::leading};
    bool selected{};
    bool enabled{true};
    bool visible{true};
    bool status_online{};
    bool compact_text{};
};

class UiSurface final {
public:
    UiSurface() = default;
    ~UiSurface();

    UiSurface(const UiSurface&) = delete;
    UiSurface& operator=(const UiSurface&) = delete;

    bool Initialize(HWND window);
    void DpiChanged();
    void ResetDeviceResources();
    void Resize(UINT width, UINT height);

    void ClearElements();
    UiElement& Add(UiElement element);
    const std::vector<UiElement>& elements() const noexcept;
    // 小面积选中线由页面计时器更新，不参与控件布局或命中测试。
    void SetOverlayAccent(std::optional<D2D1_RECT_F> bounds) noexcept;

    void Paint(D2D1_COLOR_F clear_color);

    bool PointerMove(POINT point);
    void PointerLeave();
    bool PointerDown(POINT point);
    std::optional<int> PointerUp(POINT point);
    void CancelPointer();

    int hovered_id() const noexcept;
    int pressed_id() const noexcept;

private:
    bool EnsureDeviceResources(HDC dc, const RECT& bounds);
    bool EnsureTextFormats();
    const UiElement* HitTest(POINT point) const noexcept;
    void DrawElement(const UiElement& element);
    void DrawIcon(UiIcon icon, D2D1_RECT_F bounds, ID2D1Brush* brush);
    void DrawText(const UiElement& element, D2D1_RECT_F bounds, ID2D1Brush* brush);
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F color);

    HWND window_{};
    UINT dpi_{96};
    IDWriteFactory* dwrite_factory_{};
    ID2D1DCRenderTarget* target_{};
    ID2D1SolidColorBrush* brush_{};
    IDWriteTextFormat* body_format_{};
    IDWriteTextFormat* semibold_format_{};
    IDWriteTextFormat* compact_tab_format_{};
    IDWriteTextFormat* heading_format_{};
    IDWriteTextFormat* caption_format_{};
    std::vector<UiElement> elements_;
    std::optional<D2D1_RECT_F> overlay_accent_;
    int hovered_id_{};
    int pressed_id_{};
    bool tracking_mouse_{};
};

D2D1_COLOR_F UiColor(COLORREF color, float alpha = 1.0F) noexcept;

inline D2D1_RECT_F UiRect(float left, float top, float right, float bottom) noexcept {
    return D2D1::RectF(left, top, right, bottom);
}

} // namespace SLink::ui::detail
