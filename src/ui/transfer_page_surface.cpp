#include "ui/transfer_page_surface.hpp"

#include "ui/win32_ui_internal.hpp"

#include <algorithm>

namespace SLink::ui::detail {

void BuildTransferPageSurface(UiSurface& surface, const TransferSurfaceLayout& layout) {
    surface.ClearElements();
    const float scale = static_cast<float>(layout.dpi) / 96.0F;
    const float strip_height = 34.0F * scale;
    const auto add_tab =
        [&](const std::size_t index, const float left, const float top, const float right) {
            if (index >= layout.tabs.size()) {
                return;
            }
            const auto& tab = layout.tabs[index];
            const float inset = 3.0F * scale;
            const float tab_bottom = top + strip_height - 1.0F * scale;
            surface.Add({
                .id = kTransferPaneTabBaseId + static_cast<int>(index),
                .role = UiRole::file_tab,
                .bounds = UiRect(left, top + inset, right, tab_bottom),
                .text = tab.title + (tab.phase == ViewPhase::Connecting ? L" · 连接中"
                                     : tab.phase == ViewPhase::Loading  ? L" · 加载中"
                                     : tab.phase == ViewPhase::Error    ? L" · 失败"
                                                                        : L""),
                .icon = tab.local ? UiIcon::folder : UiIcon::server,
                .alignment = UiTextAlign::leading,
                .selected = tab.selected,
                .compact_text = true,
            });
            if (!tab.local) {
                const float close_size = 18.0F * scale;
                surface.Add({
                    .id = kTransferPaneTabCloseBaseId + static_cast<int>(index),
                    .role = UiRole::tab_close,
                    .bounds = UiRect(right - close_size - 5.0F * scale,
                                     top + (strip_height - close_size) / 2.0F,
                                     right - 5.0F * scale,
                                     top + (strip_height + close_size) / 2.0F),
                    .icon = UiIcon::close,
                    .alignment = UiTextAlign::center,
                });
            }
        };
    for (const auto& group : layout.groups) {
        const RECT& bounds = group.bounds;
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
            group.tab_indexes.empty()) {
            continue;
        }
        const float left = static_cast<float>(bounds.left);
        const float top = static_cast<float>(bounds.top);
        const float right = static_cast<float>(bounds.right);
        surface.Add(
            {.role = UiRole::panel, .bounds = UiRect(left, top, right, top + strip_height)});
        float tab_left = left + 5.0F * scale;
        const float available = right - tab_left - 5.0F * scale;
        const float tab_width = std::clamp(available / static_cast<float>(group.tab_indexes.size()),
                                           78.0F * scale,
                                           147.0F * scale);
        for (const std::size_t index : group.tab_indexes) {
            if (tab_left >= right) {
                break;
            }
            const float tab_right = std::min(tab_left + tab_width, right);
            add_tab(index, tab_left, top, tab_right);
            tab_left = tab_right + 2.0F * scale;
        }
    }
    if (layout.split_separator) {
        const auto& separator = *layout.split_separator;
        const float x = static_cast<float>(separator.right);
        surface.Add({
            .role = UiRole::separator,
            .bounds = UiRect(x,
                             static_cast<float>(separator.top),
                             x + std::max(2.0F, 2.0F * scale),
                             static_cast<float>(separator.bottom)),
        });
    }
    if (layout.drop_zone) {
        const RECT& drop = *layout.drop_zone;
        surface.Add({
            .role = UiRole::drop_target,
            .bounds = UiRect(static_cast<float>(drop.left + 8),
                             static_cast<float>(drop.top + 8),
                             static_cast<float>(drop.right - 8),
                             static_cast<float>(drop.bottom - 8)),
            .text = layout.drop_center ? L"" : L"拖放到此处分屏",
            .alignment = UiTextAlign::center,
        });
    }
    if (layout.insertion) {
        const float half_width = std::max(1.0F, 1.5F * scale);
        const auto& insertion = *layout.insertion;
        surface.Add({
            .role = UiRole::tab_insert,
            .bounds = UiRect(insertion.x - half_width,
                             insertion.top + 2.0F * scale,
                             insertion.x + half_width,
                             insertion.bottom - 2.0F * scale),
        });
    }
    if (layout.queue_splitter_y > 0) {
        surface.Add({
            .role = UiRole::separator,
            .bounds =
                UiRect(static_cast<float>(layout.content_left),
                       static_cast<float>(layout.queue_splitter_y),
                       static_cast<float>(layout.width),
                       static_cast<float>(layout.queue_splitter_y) + std::max(2.0F, 2.0F * scale)),
        });
    }
    surface.Add({
        .role = UiRole::panel,
        .bounds = UiRect(static_cast<float>(layout.content_left),
                         static_cast<float>(layout.queue_y),
                         static_cast<float>(layout.width),
                         static_cast<float>(layout.queue_y + layout.queue_toolbar_height)),
    });
    surface.Add({
        .role = UiRole::card,
        .bounds = UiRect(static_cast<float>(layout.content_left),
                         static_cast<float>(layout.queue_y + layout.queue_toolbar_height),
                         static_cast<float>(layout.width) - 8.0F * scale,
                         static_cast<float>(layout.client_bottom) - 8.0F * scale),
    });
    surface.Add({
        .role = UiRole::muted,
        .bounds =
            UiRect(static_cast<float>(layout.concurrency_label_x),
                   static_cast<float>(layout.queue_y),
                   static_cast<float>(layout.concurrency_label_x + layout.concurrency_label_width),
                   static_cast<float>(layout.queue_y + layout.queue_toolbar_height)),
        .text = L"并发",
        .alignment = UiTextAlign::center,
    });
    surface.Add({
        .role = UiRole::card,
        .bounds = UiRect(static_cast<float>(layout.concurrency_frame.left),
                         static_cast<float>(layout.concurrency_frame.top),
                         static_cast<float>(layout.concurrency_frame.right),
                         static_cast<float>(layout.concurrency_frame.bottom)),
    });
}

} // namespace SLink::ui::detail
