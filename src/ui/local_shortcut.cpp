// .lnk 显示名、Shell Link 目标解析和“此电脑”虚拟入口识别实现。
#include "ui/local_shortcut.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

namespace SLink::ui::local_shortcut {
namespace {

bool IsThisPcShellTarget(std::wstring_view text) {
    std::wstring normalized(text);
    std::ranges::transform(normalized, normalized.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return normalized.find(L"20d04fe0-3aea-1069-a2d8-08002b30309d") != std::wstring::npos ||
           normalized.find(L"shell:mycomputerfolder") != std::wstring::npos ||
           normalized.find(L"mycomputerfolder") != std::wstring::npos || normalized == L"此电脑" ||
           normalized == L"计算机" || normalized == L"this pc" || normalized == L"computer";
}

} // namespace

bool IsShortcutName(const std::wstring_view name) {
    return name.size() > 4 &&
           _wcsicmp(std::wstring(name.substr(name.size() - 4)).c_str(), L".lnk") == 0;
}

std::wstring DisplayName(const std::wstring_view name) {
    return IsShortcutName(name) ? std::wstring(name.substr(0, name.size() - 4))
                                : std::wstring(name);
}

Target Resolve(const std::wstring& shortcut_path) {
    if (!IsShortcutName(shortcut_path)) {
        return {};
    }

    const auto native_shortcut_path =
        std::filesystem::path(shortcut_path).make_preferred().wstring();

    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.GetAddressOf())))) {
        return {};
    }
    Microsoft::WRL::ComPtr<IPersistFile> persistent_file;
    if (FAILED(link.As(&persistent_file)) ||
        FAILED(persistent_file->Load(native_shortcut_path.c_str(), STGM_READ))) {
        return {};
    }

    std::array<wchar_t, 32768> arguments{};
    if (SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))) &&
        IsThisPcShellTarget(arguments.data())) {
        return {TargetKind::this_pc, {}};
    }

    std::array<wchar_t, 32768> target_path{};
    WIN32_FIND_DATAW target_data{};
    if (SUCCEEDED(link->GetPath(
            target_path.data(), static_cast<int>(target_path.size()), &target_data, 0)) &&
        target_path.front() != L'\0') {
        if (IsThisPcShellTarget(target_path.data())) {
            return {TargetKind::this_pc, {}};
        }
        const DWORD attributes = GetFileAttributesW(target_path.data());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            return {(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? TargetKind::directory
                                                                 : TargetKind::file,
                    std::filesystem::path(target_path.data()).lexically_normal().generic_wstring()};
        }
    }

    PIDLIST_ABSOLUTE target_id = nullptr;
    if (SUCCEEDED(link->GetIDList(&target_id)) && target_id) {
        PWSTR parsing_name = nullptr;
        const HRESULT name_result =
            SHGetNameFromIDList(target_id, SIGDN_DESKTOPABSOLUTEPARSING, &parsing_name);
        CoTaskMemFree(target_id);
        if (SUCCEEDED(name_result) && parsing_name) {
            const bool is_this_pc = IsThisPcShellTarget(parsing_name);
            CoTaskMemFree(parsing_name);
            if (is_this_pc) {
                return {TargetKind::this_pc, {}};
            }
        } else if (parsing_name) {
            CoTaskMemFree(parsing_name);
        }
    }
    return {};
}

} // namespace SLink::ui::local_shortcut
