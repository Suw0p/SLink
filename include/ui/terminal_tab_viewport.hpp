#pragma once

#include <algorithm>
#include <cstddef>

namespace SLink::ui {

// 只计算一组终端标签的可见窗口；隐藏标签仍保留自己的终端控件和连接。
struct TerminalTabViewport {
    std::size_t first{};
    std::size_t count{};
    int tab_width{};
    bool overflow{};

    [[nodiscard]] std::size_t LastStart(const std::size_t total) const noexcept {
        return total > count ? total - count : 0;
    }

    void Reveal(const std::size_t ordinal, const std::size_t total) noexcept {
        if (count == 0 || ordinal >= total) {
            return;
        }
        if (ordinal < first) {
            first = ordinal;
        } else if (ordinal >= first + count) {
            first = ordinal - count + 1;
        }
        first = std::min(first, LastStart(total));
    }
};

[[nodiscard]] inline TerminalTabViewport
ComputeTerminalTabViewport(const int inner_width,
                           const int minimum_tab_width,
                           const int maximum_tab_width,
                           const int gap,
                           const int arrow_pair_width,
                           const std::size_t total,
                           const std::size_t requested_first) noexcept {
    if (total == 0) {
        return {};
    }
    const int minimum = std::max(1, minimum_tab_width);
    const int spacing = std::max(0, gap);
    const int room = std::max(1, inner_width);
    const std::size_t full_capacity =
        static_cast<std::size_t>(std::max(0, room + spacing) / (minimum + spacing));
    const bool overflow = total > 1 && total > full_capacity;
    const int tab_room = std::max(1, room - (overflow ? std::max(0, arrow_pair_width) : 0));
    const std::size_t capacity =
        static_cast<std::size_t>(std::max(1, (tab_room + spacing) / (minimum + spacing)));
    const std::size_t count = overflow ? std::min(total, capacity) : total;
    const int gaps = spacing * static_cast<int>(count - 1);
    const int tab_width =
        std::clamp((tab_room - gaps) / static_cast<int>(count), 1, std::max(1, maximum_tab_width));
    return {std::min(requested_first, total - count), count, tab_width, overflow};
}

} // namespace SLink::ui
