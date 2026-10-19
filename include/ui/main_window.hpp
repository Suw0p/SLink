#pragma once

#include "ui/theme.hpp"

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SLink::ui {

class TerminalPage;
class TransferPage;
class EditorPage;
class PortForwardPage;

enum class MainPage {
    Terminal,
    Transfer,
    Editor,
    PortForward,
    Settings,
};

struct ServerSidebarItem {
    std::string id;
    std::wstring label;
    bool fixed_first{};
    std::optional<std::string> group_id;
};

struct ServerSidebarGroup {
    std::string id;
    std::wstring label;
    std::int32_t sort_order{};
};

class MainWindow final {
public:
    // 主窗口拥有全部页面和消息循环，但不持有 SSH/SFTP 业务会话；会话生命周期由
    // Application 协调，以保证窗口销毁前先停止后台回调。
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE instance, int show_command);
    int RunMessageLoop();

    HWND hwnd() const noexcept;
    MainPage active_page() const noexcept;
    void ShowPage(MainPage page);
    void SetServerManagerCallback(std::function<void()> callback);
    void SetServerSidebarItems(std::vector<ServerSidebarGroup> groups,
                               std::vector<ServerSidebarItem> servers);
    void SetCollapsedServerGroups(std::vector<std::string> group_ids,
                                  std::function<bool(const std::vector<std::string>&)> changed);
    bool SelectServerSidebarItem(std::string_view server_id);
    void SetThemePreference(theme::Mode selected,
                            std::function<bool(theme::Mode)> changed_callback);
    // 线程安全 UI 投递。回调所有权转移给窗口，并始终通过 PostMessage 在窗口线程执行。
    bool PostUiTask(std::function<void()> callback);

    TerminalPage& terminal_page() noexcept;
    TransferPage& transfer_page() noexcept;
    EditorPage& editor_page() noexcept;
    PortForwardPage& port_forward_page() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::ui
