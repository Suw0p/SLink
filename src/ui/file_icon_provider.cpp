// Windows Shell 系统图标查询与缓存实现，为本地路径、快捷方式和远端扩展名提供统一图标。
#include "ui/file_icon_provider.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <shellapi.h>
#include <shlobj_core.h>
#include <string>
#include <unordered_map>

namespace SLink::ui::file_icons {
namespace {

constexpr std::size_t kMaximumCachedFileExtensions = 256;
constexpr std::size_t kMaximumCachedPaths = 512;

std::unordered_map<std::wstring, int>& ExtensionCache() {
    static std::unordered_map<std::wstring, int> cache;
    return cache;
}

std::unordered_map<std::wstring, int>& PathCache() {
    static std::unordered_map<std::wstring, int> cache;
    return cache;
}

} // namespace

const SystemFileIcons& SharedSystemFileIcons() {
    static const SystemFileIcons icons = [] {
        SystemFileIcons result;
        SHFILEINFOW shell_info{};
        result.image_list = reinterpret_cast<HIMAGELIST>(
            SHGetFileInfoW(L"folder",
                           FILE_ATTRIBUTE_DIRECTORY,
                           &shell_info,
                           sizeof(shell_info),
                           SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
        result.folder = shell_info.iIcon;

        SHSTOCKICONINFO stock_file{sizeof(stock_file)};
        if (SUCCEEDED(SHGetStockIconInfo(
                SIID_DOCNOASSOC, SHGSI_SYSICONINDEX | SHGSI_SMALLICON, &stock_file))) {
            result.file = stock_file.iSysImageIndex;
        } else {
            shell_info = {};
            SHGetFileInfoW(L".file",
                           FILE_ATTRIBUTE_NORMAL,
                           &shell_info,
                           sizeof(shell_info),
                           SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
            result.file = shell_info.iIcon;
        }
        return result;
    }();
    return icons;
}

int IconForFileName(const std::wstring_view name) {
    const SystemFileIcons& icons = SharedSystemFileIcons();
    std::wstring extension = std::filesystem::path(name).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    if (extension.empty())
        return icons.file;

    auto& cache = ExtensionCache();
    if (const auto found = cache.find(extension); found != cache.end()) {
        return found->second;
    }
    if (cache.size() >= kMaximumCachedFileExtensions)
        return icons.file;

    SHFILEINFOW shell_info{};
    const DWORD_PTR result =
        SHGetFileInfoW(extension.c_str(),
                       FILE_ATTRIBUTE_NORMAL,
                       &shell_info,
                       sizeof(shell_info),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    const int icon = result != 0 ? shell_info.iIcon : icons.file;
    cache.emplace(std::move(extension), icon);
    return icon;
}

int IconForPath(const std::wstring_view path) {
    const SystemFileIcons& icons = SharedSystemFileIcons();
    if (path.empty())
        return icons.file;

    // FileUri deliberately stores local paths with generic '/' separators.
    // The Windows shell icon resolver does not resolve real .lnk files when
    // given that form (it returns no system-image-list index), so convert at
    // the Shell API boundary while keeping the application URI unchanged.
    auto native_path = std::filesystem::path(path).make_preferred().wstring();
    std::wstring key(native_path);
    std::ranges::transform(key, key.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    auto& cache = PathCache();
    if (const auto found = cache.find(key); found != cache.end()) {
        return found->second;
    }

    SHFILEINFOW shell_info{};
    const DWORD_PTR result =
        SHGetFileInfoW(native_path.c_str(),
                       0,
                       &shell_info,
                       sizeof(shell_info),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_ADDOVERLAYS);
    const int icon = result != 0 ? shell_info.iIcon : icons.file;
    if (cache.size() < kMaximumCachedPaths) {
        cache.emplace(std::move(key), icon);
    }
    return icon;
}

} // namespace SLink::ui::file_icons
