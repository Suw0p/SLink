#pragma once

#include "transfer/endpoint.hpp"
#include "transfer/pane_model.hpp"
#include "transfer/transfer.hpp"
#include "ui/view_state.hpp"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SLink::ui {

using TransferPaneId = transfer::TransferPaneId;

struct TransferServerOption {
    std::string id;
    std::wstring label;
    bool fixed_first{};
};

class TransferPage final {
public:
    using DirectoryListCompletion =
        std::function<void(std::vector<transfer::DirectoryEntry>, std::wstring error)>;
    using AsyncDirectoryList = std::function<void(TransferPaneId,
                                                  std::shared_ptr<transfer::IFileEndpoint>,
                                                  transfer::FileUri,
                                                  DirectoryListCompletion)>;
    using RemoteEditCallback = std::function<void(const transfer::FileUri&)>;
    using PaneClosedCallback = std::function<void(TransferPaneId)>;
    using PaneNavigatedCallback = std::function<void(TransferPaneId, const transfer::FileUri&)>;
    using OpenRemoteSessionCallback = std::function<void(std::string)>;
    using StatusCallback = std::function<void(std::wstring)>;
    using PaneActionCallback = std::function<void(TransferPaneId, ViewRecoveryAction)>;

    TransferPage();
    ~TransferPage();

    TransferPage(const TransferPage&) = delete;
    TransferPage& operator=(const TransferPage&) = delete;

    bool Create(HWND parent, HINSTANCE instance);
    void SetBounds(int x, int y, int width, int height);
    void Show(bool visible);
    void ApplyTheme();
    TransferPaneId AddLocalPane(std::wstring title = L"本地主机");

    // 每次远程文件会话成功后追加一个窗格，并持有返回 id 直到该会话关闭。
    // 页面不持久化远程窗格，重启后仍只打开本地主机窗格。
    TransferPaneId AddRemotePane(std::wstring server_name, std::wstring initial_path = L"/");
    TransferPaneId AddRemotePane(std::wstring server_name,
                                 std::string server_id,
                                 std::shared_ptr<transfer::IFileEndpoint> endpoint,
                                 std::string initial_path = "/");
    TransferPaneId AddPendingRemotePane(std::wstring server_name, std::string server_id);
    bool ClosePane(TransferPaneId id);
    void ResetRemotePanes();

    // 窗格端点只用于目录快照；实际复制统一提交给 TransferManager，源/目标端点由
    // TransferManager 的 resolver 决定，避免浏览会话被传输线程并发使用。
    bool BindPaneEndpoint(TransferPaneId id,
                          std::shared_ptr<transfer::IFileEndpoint> endpoint,
                          transfer::FileUri location);
    void SetPaneViewState(TransferPaneId id, ViewState state);
    void SetPaneActionCallback(PaneActionCallback callback);
    bool NavigatePane(TransferPaneId id, transfer::FileUri location);
    bool RefreshPane(TransferPaneId id);
    bool HandleEnterKey(HWND focused_control);
    [[nodiscard]] std::optional<transfer::FileUri> pane_location(TransferPaneId id) const;

    // SFTP/libssh2 所有者必须安装此钩子，并在对应 NetworkReactor 上执行 list。
    // completion 可从任意线程触发，结果会通过 PostMessage 回到页面线程。未安装钩子
    // 时仅允许后台线程枚举本地目录，远程枚举会被明确拒绝，防止 libssh2 句柄逃离
    // 它的 Reactor 所在线程。
    void SetAsyncDirectoryList(AsyncDirectoryList callback);
    void SetTransferManager(std::shared_ptr<transfer::TransferManager> manager);
    void SetRemoteEditCallback(RemoteEditCallback callback);
    void SetPaneClosedCallback(PaneClosedCallback callback);
    void SetPaneNavigatedCallback(PaneNavigatedCallback callback);
    void SetServerOptions(std::vector<TransferServerOption> servers);
    bool ActivateServer(std::string_view server_id);
    void SetOpenRemoteSessionCallback(OpenRemoteSessionCallback callback);
    void SetStatusCallback(StatusCallback callback);
    std::size_t pane_count() const noexcept;
    [[nodiscard]] std::optional<TransferPaneId> pane_id_at(std::size_t index) const noexcept;
    HWND hwnd() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::ui
