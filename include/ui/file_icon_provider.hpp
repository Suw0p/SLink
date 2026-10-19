#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string_view>

namespace SLink::ui::file_icons {

// 复用 Windows 系统小图标列表。按扩展名获取远端文件图标，按完整路径获取本地文件和
// .lnk 目标图标；返回的 HIMAGELIST 归系统所有，调用方不得销毁。
struct SystemFileIcons {
    HIMAGELIST image_list{};
    int folder{};
    int file{};
};

const SystemFileIcons& SharedSystemFileIcons();
int IconForFileName(std::wstring_view name);
int IconForPath(std::wstring_view path);

} // namespace SLink::ui::file_icons
