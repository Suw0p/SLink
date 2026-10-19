// 文件传输窗格的纯数据模型与等宽布局计算，不包含 HWND 或持久化状态。
#include "transfer/pane_model.hpp"

#include <algorithm>
#include <stdexcept>

namespace SLink::transfer {

TransferPaneModel::TransferPaneModel(FileUri initial_local_location) {
    if (!initial_local_location.is_local()) {
        throw std::invalid_argument("The initial transfer pane must be local");
    }
    panes_.push_back({next_id_++, TransferPaneKind::Local, {}, std::move(initial_local_location)});
}

TransferPaneId TransferPaneModel::local_pane_id() const noexcept {
    return panes_.front().id;
}

TransferPaneId TransferPaneModel::open_remote(std::string server_id, std::string initial_path) {
    if (server_id.empty()) {
        throw std::invalid_argument("A remote pane requires a server id");
    }
    const auto id = next_id_++;
    panes_.push_back(
        {id, TransferPaneKind::Remote, server_id, FileUri::sftp(server_id, initial_path)});
    return id;
}

bool TransferPaneModel::close(const TransferPaneId id) {
    const auto found = std::ranges::find(panes_, id, &TransferPane::id);
    if (found == panes_.end() || !found->closable())
        return false;
    panes_.erase(found);
    return true;
}

bool TransferPaneModel::set_location(const TransferPaneId id, FileUri location) {
    const auto found = std::ranges::find(panes_, id, &TransferPane::id);
    if (found == panes_.end())
        return false;
    if (found->kind == TransferPaneKind::Local && !location.is_local())
        return false;
    if (found->kind == TransferPaneKind::Remote &&
        (!location.is_sftp() || location.authority() != found->server_id)) {
        return false;
    }
    found->location = std::move(location);
    return true;
}

const TransferPane* TransferPaneModel::find(const TransferPaneId id) const noexcept {
    const auto found = std::ranges::find(panes_, id, &TransferPane::id);
    return found == panes_.end() ? nullptr : &*found;
}

PaneColumns TransferPaneModel::columns_for_width(const float pane_width) noexcept {
    PaneColumns result;
    // Hide least important metadata first; filename and connection/status remain.
    result.permissions = pane_width >= 520.0F;
    result.modified = pane_width >= 390.0F;
    result.size = pane_width >= 280.0F;
    return result;
}

std::vector<PanePlacement> TransferPaneModel::layout(const float available_width) const {
    if (available_width < 0.0F) {
        throw std::invalid_argument("Available pane width cannot be negative");
    }
    const auto width = panes_.empty() ? 0.0F : available_width / static_cast<float>(panes_.size());
    std::vector<PanePlacement> result;
    result.reserve(panes_.size());
    for (std::size_t index = 0; index < panes_.size(); ++index) {
        result.push_back(
            {panes_[index].id, width * static_cast<float>(index), width, columns_for_width(width)});
    }
    return result;
}

} // namespace SLink::transfer
