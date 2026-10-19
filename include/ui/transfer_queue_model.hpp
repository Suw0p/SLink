#pragma once

#include "transfer/transfer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SLink::ui::detail {

struct TransferQueueRow {
    std::size_t job_index{};
    transfer::TransferItemId item_id{};
    std::size_t depth{};
    bool expandable{};
    bool expanded{};
    bool last_sibling{true};
    std::uint64_t ancestor_lines{};

    [[nodiscard]] bool is_job() const noexcept {
        return item_id == 0;
    }
};

struct TransferQueueTree {
    std::vector<TransferQueueRow> rows;
    std::unordered_map<transfer::TransferId, std::size_t> job_rows;
    std::unordered_map<transfer::TransferId,
                       std::unordered_map<transfer::TransferItemId, std::size_t>>
        visible_item_rows;
    std::unordered_map<transfer::TransferId, std::unordered_set<transfer::TransferItemId>>
        running_visible_items;
};

using QueueDetailLookup =
    std::function<std::span<const transfer::TransferItemProgress>(transfer::TransferId)>;

[[nodiscard]] std::string TransferQueueItemKey(transfer::TransferId job_id,
                                               transfer::TransferItemId item_id);
[[nodiscard]] TransferQueueTree
BuildTransferQueueTree(std::span<const transfer::TransferJob> jobs,
                       const QueueDetailLookup& details,
                       const std::unordered_set<transfer::TransferId>& expanded_jobs,
                       const std::unordered_set<std::string>& expanded_items);

} // namespace SLink::ui::detail
