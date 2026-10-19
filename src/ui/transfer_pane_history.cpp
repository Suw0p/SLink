#include "ui/transfer_pane_history.hpp"

#include <cstddef>

namespace SLink::ui::detail {

void TransferPaneHistory::Reset(const transfer::FileUri& location) {
    locations_.assign(1, location);
    index_ = 0;
    replaying_ = false;
}

void TransferPaneHistory::Record(const transfer::FileUri& current,
                                 const transfer::FileUri& target) {
    if (replaying_ || current == target) {
        return;
    }
    if (locations_.empty()) {
        locations_.push_back(current);
        index_ = 0;
    }
    if (index_ + 1 < locations_.size()) {
        locations_.erase(locations_.begin() + static_cast<std::ptrdiff_t>(index_ + 1),
                         locations_.end());
    }
    locations_.push_back(target);
    index_ = locations_.size() - 1;
}

bool TransferPaneHistory::Navigate(const bool forward, const Request& request) {
    if (locations_.empty()) {
        return false;
    }
    const std::size_t target =
        forward ? index_ + 1 : (index_ == 0 ? locations_.size() : index_ - 1);
    if (target >= locations_.size()) {
        return false;
    }
    replaying_ = true;
    bool requested{};
    try {
        requested = request(locations_[target]);
    } catch (...) {
        replaying_ = false;
        throw;
    }
    replaying_ = false;
    if (requested) {
        index_ = target;
    }
    return requested;
}

} // namespace SLink::ui::detail
