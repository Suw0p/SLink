#pragma once

#include "transfer/file_uri.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace SLink::ui::detail {

// 前进/后退是单个文件窗格的交互状态；重放路径时不追加新历史。
class TransferPaneHistory {
public:
    using Request = std::function<bool(const transfer::FileUri&)>;

    void Reset(const transfer::FileUri& location);
    void Record(const transfer::FileUri& current, const transfer::FileUri& target);
    bool Navigate(bool forward, const Request& request);

private:
    std::vector<transfer::FileUri> locations_;
    std::size_t index_{};
    bool replaying_{};
};

} // namespace SLink::ui::detail
