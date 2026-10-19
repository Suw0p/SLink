#pragma once

#include "ui/retained_ui.hpp"
#include "ui/view_state.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace SLink::ui::detail {

inline constexpr int kTransferPaneTabBaseId = 3500;
inline constexpr int kTransferPaneTabCloseBaseId = 3700;

struct TransferSurfaceTab {
    std::wstring title;
    ViewPhase phase{ViewPhase::Idle};
    bool local{};
    bool selected{};
};

struct TransferSurfaceGroup {
    RECT bounds{};
    std::vector<std::size_t> tab_indexes;
};

struct TransferTabInsertion {
    float x{};
    float top{};
    float bottom{};
};

// 页面只生成状态快照；元素几何和样式在这里转换，绘制过程不接触连接或文件端点。
struct TransferSurfaceLayout {
    UINT dpi{96};
    int width{};
    int client_bottom{};
    int content_left{};
    int queue_y{};
    int queue_toolbar_height{};
    int queue_splitter_y{};
    int concurrency_label_x{};
    int concurrency_label_width{};
    RECT concurrency_frame{};
    std::vector<TransferSurfaceTab> tabs;
    std::vector<TransferSurfaceGroup> groups;
    std::optional<RECT> split_separator;
    std::optional<RECT> drop_zone;
    bool drop_center{};
    std::optional<TransferTabInsertion> insertion;
};

void BuildTransferPageSurface(UiSurface& surface, const TransferSurfaceLayout& layout);

} // namespace SLink::ui::detail
