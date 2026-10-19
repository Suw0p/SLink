#pragma once

#include "transfer/file_uri.hpp"
#include "transfer/transfer.hpp"

#include <windows.h>
#include <optional>
#include <string>

namespace SLink::ui {

// 文件冲突对话框只收集单次策略。“全部应用”由调用页面缓存到当前批次；SHA-256
// 仅在用户主动勾选时启用，不在普通大小校验之外增加默认读取开销。
struct TransferConflictChoice {
    transfer::ConflictPolicy policy{transfer::ConflictPolicy::Overwrite};
    bool apply_all{};
    bool verify_sha256{};
};

std::optional<TransferConflictChoice> ShowTransferConflictDialog(HWND owner,
                                                                 const transfer::FileUri& source,
                                                                 const transfer::FileUri& target,
                                                                 std::wstring source_display,
                                                                 std::wstring target_display);

} // namespace SLink::ui
