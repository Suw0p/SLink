#pragma once

#include <windows.h>

namespace SLink::ui::theme {

// 全局深浅主题色板。低对比表面和中性选中态让内容成为视觉主体；强调色只用于
// 操作反馈、进度及焦点。所有自绘控件从当前 Palette 取色，运行时切换无需重启。
enum class Mode {
    Dark,
    Light,
};

struct Palette {
    COLORREF window;
    COLORREF panel;
    COLORREF panel_alt;
    COLORREF panel_hover;
    COLORREF selected_row;
    COLORREF border;
    COLORREF text;
    COLORREF muted_text;
    COLORREF accent;
};

inline constexpr Palette kDarkPalette{RGB(23, 23, 23),
                                      RGB(30, 30, 30),
                                      RGB(38, 38, 38),
                                      RGB(46, 46, 46),
                                      RGB(53, 56, 60),
                                      RGB(51, 51, 51),
                                      RGB(235, 235, 235),
                                      RGB(161, 161, 161),
                                      RGB(126, 153, 216)};
inline constexpr Palette kLightPalette{RGB(247, 247, 247),
                                       RGB(255, 255, 255),
                                       RGB(250, 250, 250),
                                       RGB(239, 240, 242),
                                       RGB(231, 233, 237),
                                       RGB(220, 221, 223),
                                       RGB(36, 36, 36),
                                       RGB(105, 105, 108),
                                       RGB(75, 106, 177)};

inline Mode current_mode = Mode::Dark;
inline COLORREF kWindowColor = kDarkPalette.window;
inline COLORREF kPanelColor = kDarkPalette.panel;
inline COLORREF kPanelAltColor = kDarkPalette.panel_alt;
inline COLORREF kPanelHoverColor = kDarkPalette.panel_hover;
inline COLORREF kSelectedRowColor = kDarkPalette.selected_row;
inline COLORREF kBorderColor = kDarkPalette.border;
inline COLORREF kTextColor = kDarkPalette.text;
inline COLORREF kMutedTextColor = kDarkPalette.muted_text;
inline COLORREF kAccentColor = kDarkPalette.accent;

inline void SetMode(const Mode mode) noexcept {
    current_mode = mode;
    const Palette& palette = mode == Mode::Light ? kLightPalette : kDarkPalette;
    kWindowColor = palette.window;
    kPanelColor = palette.panel;
    kPanelAltColor = palette.panel_alt;
    kPanelHoverColor = palette.panel_hover;
    kSelectedRowColor = palette.selected_row;
    kBorderColor = palette.border;
    kTextColor = palette.text;
    kMutedTextColor = palette.muted_text;
    kAccentColor = palette.accent;
}

[[nodiscard]] inline Mode CurrentMode() noexcept {
    return current_mode;
}
[[nodiscard]] inline bool IsDark() noexcept {
    return current_mode == Mode::Dark;
}

// 终端画布及覆盖其上的轻量控件共用这组颜色，避免浮层形成独立色块。
[[nodiscard]] inline COLORREF TerminalBackgroundColor() noexcept {
    return IsDark() ? kWindowColor : kPanelColor;
}

[[nodiscard]] inline COLORREF TerminalForegroundColor() noexcept {
    return kTextColor;
}

inline constexpr int kServerSidebarWidth = 196;
// 侧栏以既有宽度作为初始值；拖动时可缩至零并折叠，最大值用于避免挤占主工作区。
inline constexpr int kServerSidebarMaximumWidth = 420;
inline constexpr int kServerSidebarContentMinimumWidth = 280;
inline constexpr int kServerListItemHeight = 38;
inline constexpr int kServerSearchHeight = 30;
inline constexpr int kServerSidebarPadding = 8;
inline constexpr int kServerSearchInset = 12;
inline constexpr int kCompactControlGap = 5;

} // namespace SLink::ui::theme
