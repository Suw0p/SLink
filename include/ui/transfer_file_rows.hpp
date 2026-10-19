#pragma once

#include "transfer/endpoint.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SLink::ui::detail {

// 目录表只持有一次枚举得到的快照；绘制和排序均不再访问文件端点。
struct TransferFileRow {
    std::wstring name;
    std::wstring status;
    std::wstring size;
    std::wstring modified;
    std::wstring permissions;
    std::wstring owner;
    std::chrono::system_clock::time_point modified_time{};
    transfer::FileUri uri;
    std::uint32_t permission_bits{};
    bool directory{};
    bool local_shortcut{};
    bool transferable{true};
};

[[nodiscard]] std::wstring FromUtf8(std::string_view input);
[[nodiscard]] std::wstring FileTimeText(const std::chrono::system_clock::time_point& value);
[[nodiscard]] std::wstring PermissionsText(std::uint32_t permissions);
[[nodiscard]] std::vector<TransferFileRow>
PrepareDirectoryRows(const transfer::FileUri& location,
                     std::vector<transfer::DirectoryEntry> entries,
                     bool show_hidden);
[[nodiscard]] bool SameFileRows(const std::vector<TransferFileRow>& left,
                                const std::vector<TransferFileRow>& right);
void SortFileRows(std::vector<TransferFileRow>& rows, std::size_t column, bool ascending);

} // namespace SLink::ui::detail
