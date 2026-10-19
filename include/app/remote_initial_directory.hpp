#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace SLink::app {

// 远端初始目录解析工具。Unix 默认使用规范化登录主目录；Windows SFTP 会在 home
// 基础上尝试 Desktop/桌面候选。用户配置的绝对路径优先，相对路径基于 home 解析。
[[nodiscard]] bool IsWindowsSftpPath(std::string_view path) noexcept;
[[nodiscard]] std::string ResolveConfiguredInitialPath(std::string_view home,
                                                       std::string_view configured);
[[nodiscard]] std::vector<std::string> WindowsDesktopCandidates(std::string_view home);

} // namespace SLink::app
