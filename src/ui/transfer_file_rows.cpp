#include "ui/transfer_file_rows.hpp"

#include "ui/local_shortcut.hpp"
#include "ui/win32_ui_internal.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cwchar>

namespace SLink::ui::detail {
namespace {

bool NameLess(const TransferFileRow& left, const TransferFileRow& right) {
    const int insensitive = _wcsicmp(left.name.c_str(), right.name.c_str());
    return insensitive != 0 ? insensitive < 0 : left.name < right.name;
}

} // namespace

std::wstring FromUtf8(const std::string_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring(input.begin(), input.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        input.data(),
                        static_cast<int>(input.size()),
                        result.data(),
                        length);
    return result;
}

std::wstring FileTimeText(const std::chrono::system_clock::time_point& value) {
    if (value == std::chrono::system_clock::time_point{}) {
        return L"—";
    }
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(value);
    std::tm local{};
    if (localtime_s(&local, &timestamp) != 0) {
        return L"—";
    }
    wchar_t buffer[32]{};
    if (wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M", &local) == 0) {
        return L"—";
    }
    return buffer;
}

std::wstring PermissionsText(const std::uint32_t permissions) {
    if (permissions == 0) {
        return L"—";
    }
    const std::uint32_t mode = permissions & 0777U;
    std::wstring text(9, L'-');
    static constexpr std::array<wchar_t, 3> symbols{L'r', L'w', L'x'};
    for (std::size_t index = 0; index < text.size(); ++index) {
        const std::uint32_t bit = 1U << (8U - static_cast<unsigned>(index));
        if ((mode & bit) != 0) {
            text[index] = symbols[index % symbols.size()];
        }
    }
    return text;
}

std::vector<TransferFileRow> PrepareDirectoryRows(const transfer::FileUri& location,
                                                  std::vector<transfer::DirectoryEntry> entries,
                                                  const bool show_hidden) {
    std::vector<TransferFileRow> rows;
    rows.reserve(std::min<std::size_t>(100000, entries.size()) + 1);
    if (location.parent() != location) {
        TransferFileRow parent;
        parent.name = L"..";
        parent.status = L"上级目录";
        parent.directory = true;
        parent.uri = location.parent();
        rows.push_back(std::move(parent));
    }
    for (auto& entry : entries) {
        if (!show_hidden && transfer::IsHiddenDirectoryEntry(entry)) {
            continue;
        }
        if (rows.size() >= 100000) {
            break;
        }
        TransferFileRow row;
        row.name = FromUtf8(entry.name);
        row.uri = std::move(entry.uri);
        row.local_shortcut = location.is_local() && local_shortcut::IsShortcutName(row.name);
        row.directory = entry.stat.kind == transfer::FileKind::Directory;
        switch (entry.stat.kind) {
        case transfer::FileKind::Directory:
            row.status = L"文件夹";
            break;
        case transfer::FileKind::Regular:
            row.status = row.local_shortcut ? L"快捷方式" : L"文件";
            break;
        case transfer::FileKind::Symlink:
            row.status = L"符号链接";
            break;
        case transfer::FileKind::Other:
            row.status = L"其他";
            break;
        }
        if (!row.directory) {
            row.size = FormatFileSize(entry.stat.size);
        }
        row.modified = FileTimeText(entry.stat.modified);
        row.modified_time = entry.stat.modified;
        row.permission_bits = entry.stat.permissions & 0777U;
        row.permissions = PermissionsText(entry.stat.permissions);
        row.owner = FromUtf8(entry.stat.owner);
        rows.push_back(std::move(row));
    }
    const std::size_t sort_begin = !rows.empty() && rows.front().name == L".." ? 1 : 0;
    std::stable_sort(rows.begin() + static_cast<std::ptrdiff_t>(sort_begin),
                     rows.end(),
                     [](const TransferFileRow& left, const TransferFileRow& right) {
                         if (left.directory != right.directory) {
                             return left.directory > right.directory;
                         }
                         return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
                     });
    return rows;
}

bool SameFileRows(const std::vector<TransferFileRow>& left,
                  const std::vector<TransferFileRow>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.name != b.name || a.status != b.status || a.size != b.size ||
            a.modified != b.modified || a.permissions != b.permissions || a.owner != b.owner ||
            a.modified_time != b.modified_time || a.uri != b.uri ||
            a.permission_bits != b.permission_bits || a.directory != b.directory ||
            a.local_shortcut != b.local_shortcut || a.transferable != b.transferable) {
            return false;
        }
    }
    return true;
}

void SortFileRows(std::vector<TransferFileRow>& rows,
                  const std::size_t column,
                  const bool ascending) {
    const std::size_t sort_begin = !rows.empty() && rows.front().name == L".." ? 1 : 0;
    std::stable_sort(
        rows.begin() + static_cast<std::ptrdiff_t>(sort_begin),
        rows.end(),
        [column, ascending](const TransferFileRow& left, const TransferFileRow& right) {
            // 文件夹始终在前；名称和修改时间的次序由表头控制。
            if (left.directory != right.directory) {
                return left.directory > right.directory;
            }
            bool less = false;
            bool greater = false;
            if (column == 3 && left.modified_time != right.modified_time) {
                less = left.modified_time < right.modified_time;
                greater = left.modified_time > right.modified_time;
            } else {
                less = NameLess(left, right);
                greater = NameLess(right, left);
            }
            return ascending ? less : greater;
        });
}

} // namespace SLink::ui::detail
