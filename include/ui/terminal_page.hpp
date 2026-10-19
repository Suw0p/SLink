#pragma once

#include <windows.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include "ui/view_state.hpp"

namespace SLink::ui {

class TerminalControl;

// 终端页面负责服务器侧栏、标签组和左右分屏的 UI 模型。每个标签拥有独立
// TerminalControl；拖到另一组标签栏表示合并，拖到终端内容区才创建左右分屏。
// 页面本身不建立 SSH 连接，只通过回调把连接和关闭请求交给应用控制器。
class TerminalPage final {
public:
    using ServerActivatedCallback =
        std::function<void(std::string_view server_id, std::size_t pane_index)>;
    using PaneClosingCallback = std::function<void(std::size_t pane_index)>;
    using PaneCreatedCallback = std::function<void(std::size_t pane_index, TerminalControl& pane)>;

    TerminalPage();
    ~TerminalPage();

    TerminalPage(const TerminalPage&) = delete;
    TerminalPage& operator=(const TerminalPage&) = delete;

    bool Create(HWND parent, HINSTANCE instance);
    void SetBounds(int x, int y, int width, int height);
    void Show(bool visible);
    void ApplyTheme();
    void AddServer(std::string server_id,
                   std::wstring display_name,
                   std::wstring tab_name,
                   bool fixed_first = false);
    void ClearServers();
    bool ActivateServer(std::string_view server_id);
    void SetPaneViewState(TerminalControl& pane, ViewState state);
    void SetServerActivatedCallback(ServerActivatedCallback callback);
    void SetPaneClosingCallback(PaneClosingCallback callback);
    void SetPaneCreatedCallback(PaneCreatedCallback callback);
    void AddHorizontalPane();
    void CloseActivePane();
    void FocusPane(std::size_t index);
    std::size_t active_pane_index() const noexcept;
    std::size_t pane_count() const noexcept;
    TerminalControl* pane_at(std::size_t index) noexcept;
    const TerminalControl* pane_at(std::size_t index) const noexcept;
    HWND hwnd() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::ui
