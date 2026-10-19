#pragma once

#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>

namespace SLink::ui {

// 文件操作对话框只收集并验证用户输入，不执行文件 I/O；调用方负责在对应端点线程
// 应用权限、重命名或递归操作。
struct PermissionDialogResult {
    std::uint32_t permissions{};
    bool recursive{};
};

std::optional<PermissionDialogResult> ShowPermissionDialog(HWND owner,
                                                           std::wstring item_name,
                                                           std::uint32_t current_permissions,
                                                           bool directory);

std::optional<std::wstring>
ShowRenameDialog(HWND owner, std::wstring current_name, bool local_path);

} // namespace SLink::ui
