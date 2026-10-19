#pragma once

#include <string>
#include <string_view>

namespace SLink::ui::local_shortcut {

// 本地 .lnk 的显示与导航辅助。DisplayName 隐藏资源管理器通常不展示的 .lnk 后缀；
// Resolve 区分普通文件、目录和“此电脑”虚拟目标，避免把虚拟入口当作磁盘路径。
enum class TargetKind {
    none,
    file,
    directory,
    this_pc,
};

struct Target {
    TargetKind kind{TargetKind::none};
    std::wstring path;
};

[[nodiscard]] bool IsShortcutName(std::wstring_view name);
[[nodiscard]] std::wstring DisplayName(std::wstring_view name);
[[nodiscard]] Target Resolve(const std::wstring& shortcut_path);

} // namespace SLink::ui::local_shortcut
