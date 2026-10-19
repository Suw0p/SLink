// Unix/Windows SFTP 初始目录规范化实现，包含远端 Windows Desktop 候选路径生成。
#include "app/remote_initial_directory.hpp"

#include <algorithm>
#include <cctype>

namespace SLink::app {
namespace {

std::string Slashes(std::string_view value) {
    std::string result(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string Join(std::string_view base, std::string_view child) {
    std::string result = Slashes(base);
    std::string suffix = Slashes(child);
    while (!suffix.empty() && suffix.front() == '/') {
        suffix.erase(suffix.begin());
    }
    if (!result.empty() && result.back() != '/') {
        result.push_back('/');
    }
    result += suffix;
    return result;
}

bool HasDrivePrefix(std::string_view path) noexcept {
    if (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 &&
           path[1] == ':';
}

} // namespace

bool IsWindowsSftpPath(const std::string_view path) noexcept {
    return HasDrivePrefix(path);
}

std::string ResolveConfiguredInitialPath(const std::string_view home,
                                         const std::string_view configured) {
    std::string path = Slashes(configured);
    if (path.empty() || path == "~") {
        return Slashes(home);
    }
    if (path.starts_with("~/")) {
        return Join(home, std::string_view(path).substr(2));
    }
    if (path.front() == '/' || HasDrivePrefix(path)) {
        return path;
    }
    return Join(home, path);
}

std::vector<std::string> WindowsDesktopCandidates(const std::string_view home) {
    return {
        Join(home, "Desktop"),
        Join(home, "OneDrive/Desktop"),
        Join(home, "\xE6\xA1\x8C\xE9\x9D\xA2"),
        Join(home, "OneDrive/\xE6\xA1\x8C\xE9\x9D\xA2"),
    };
}

} // namespace SLink::app
