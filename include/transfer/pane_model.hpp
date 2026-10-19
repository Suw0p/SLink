#pragma once

#include "transfer/file_uri.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SLink::transfer {

using TransferPaneId = std::uint64_t;

enum class TransferPaneKind {
    Local,
    Remote,
};

struct TransferPane {
    TransferPaneId id{};
    TransferPaneKind kind{TransferPaneKind::Local};
    std::string server_id;
    FileUri location;

    [[nodiscard]] bool closable() const noexcept {
        return kind == TransferPaneKind::Remote;
    }
};

struct PaneColumns {
    bool name{true};
    bool status{true};
    bool size{true};
    bool modified{true};
    bool permissions{true};
};

struct PanePlacement {
    TransferPaneId id{};
    float x{};
    float width{};
    PaneColumns columns;
};

class TransferPaneModel {
public:
    explicit TransferPaneModel(FileUri initial_local_location = FileUri::local("/"));

    [[nodiscard]] TransferPaneId local_pane_id() const noexcept;
    [[nodiscard]] TransferPaneId open_remote(std::string server_id, std::string initial_path = "/");
    [[nodiscard]] bool close(TransferPaneId id);
    [[nodiscard]] bool set_location(TransferPaneId id, FileUri location);
    [[nodiscard]] const std::vector<TransferPane>& panes() const noexcept {
        return panes_;
    }
    [[nodiscard]] const TransferPane* find(TransferPaneId id) const noexcept;

    // 每个打开会话都在同一水平行获得一个等宽窗格，不保存“几屏模式”或屏数开关；
    // 窗格数量完全由当前打开的标签决定。
    [[nodiscard]] std::vector<PanePlacement> layout(float available_width) const;
    [[nodiscard]] static PaneColumns columns_for_width(float pane_width) noexcept;

private:
    TransferPaneId next_id_{1};
    std::vector<TransferPane> panes_;
};

} // namespace SLink::transfer
