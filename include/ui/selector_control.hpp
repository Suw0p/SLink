#pragma once

#include <windows.h>

namespace SLink::ui::selector {

// 项目自绘的只读选择框。为便于替换 Win32 ComboBox，控件实现 CB_ADDSTRING、
// CB_GETCURSEL、CB_SETCURSEL 等常用消息，并以 CBN_SELCHANGE 通知父窗口。
inline constexpr wchar_t kClassName[] = L"SLink.Selector";

bool RegisterClass(HINSTANCE instance);

// 终端编码选择器使用画布配色绘制成轻量浮层；只影响指定实例，不改变认证方式、
// 冲突策略等普通选择器。
void SetTerminalOverlayStyle(HWND window, bool enabled);

} // namespace SLink::ui::selector
