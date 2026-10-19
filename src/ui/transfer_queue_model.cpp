#include "ui/transfer_queue_model.hpp"

#include <utility>

namespace SLink::ui::detail {

std::string TransferQueueItemKey(transfer::TransferId job_id, transfer::TransferItemId item_id) {
    return std::to_string(job_id) + ":" + std::to_string(item_id);
}

TransferQueueTree
BuildTransferQueueTree(std::span<const transfer::TransferJob> jobs,
                       const QueueDetailLookup& details,
                       const std::unordered_set<transfer::TransferId>& expanded_jobs,
                       const std::unordered_set<std::string>& expanded_items) {
    TransferQueueTree tree;
    tree.rows.reserve(jobs.size());
    for (std::size_t job_index = 0; job_index < jobs.size(); ++job_index) {
        const auto& job = jobs[job_index];
        const bool job_expanded = expanded_jobs.contains(job.id);
        tree.job_rows[job.id] = tree.rows.size();
        tree.rows.push_back({job_index, 0, 0, job.source_is_directory, job_expanded, true, 0});
        if (!job.source_is_directory || !job_expanded) {
            continue;
        }
        const auto items = details(job.id);
        transfer::TransferItemId root_id{};
        for (const auto& item : items) {
            if (item.parent_id == 0) {
                root_id = item.id;
                break;
            }
        }
        if (root_id == 0) {
            continue;
        }
        std::unordered_map<transfer::TransferItemId,
                           std::vector<const transfer::TransferItemProgress*>>
            children;
        children.reserve(items.size());
        for (const auto& item : items) {
            if (item.id != root_id) {
                children[item.parent_id].push_back(&item);
            }
        }
        const auto append = [&](auto&& self,
                                transfer::TransferItemId parent_id,
                                std::size_t depth,
                                std::uint64_t ancestor_lines) -> void {
            const auto found = children.find(parent_id);
            if (found == children.end()) {
                return;
            }
            const auto& siblings = found->second;
            for (std::size_t index = 0; index < siblings.size(); ++index) {
                const auto& item = *siblings[index];
                const bool last = index + 1 == siblings.size();
                const auto child_found = children.find(item.id);
                const bool expandable =
                    item.directory && child_found != children.end() && !child_found->second.empty();
                const bool expanded =
                    expandable && expanded_items.contains(TransferQueueItemKey(job.id, item.id));
                tree.visible_item_rows[job.id][item.id] = tree.rows.size();
                if (item.state == transfer::TransferState::Running) {
                    tree.running_visible_items[job.id].insert(item.id);
                }
                tree.rows.push_back(
                    {job_index, item.id, depth, expandable, expanded, last, ancestor_lines});
                if (!expanded) {
                    continue;
                }
                std::uint64_t descendant_lines = ancestor_lines;
                if (depth > 0 && depth <= 64 && !last) {
                    descendant_lines |= std::uint64_t{1} << (depth - 1);
                }
                self(self, item.id, depth + 1, descendant_lines);
            }
        };
        append(append, root_id, 1, 0);
    }
    return tree;
}

} // namespace SLink::ui::detail
