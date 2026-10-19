#pragma once

#include <windows.h>

namespace SLink::ui::sidebar {

// 三个主要模块共用的服务器侧栏自绘控件注册入口。侧栏的实时搜索、整行选择、
// “本地主机”和服务器管理入口由页面模型提供，控件只负责统一绘制与命中测试。
inline constexpr wchar_t kClassName[] = L"SLink.SidebarList";

bool RegisterClass(HINSTANCE instance);

// 重建服务器行时保存并恢复视口，不让旧选中项的自动滚动把列表拉走。
int ScrollOffset(HWND window) noexcept;
void SetScrollOffset(HWND window, int offset) noexcept;

} // namespace SLink::ui::sidebar
