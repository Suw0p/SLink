#pragma once

#include "core/status.hpp"
#include "ssh/network_reactor.hpp"
#include "storage/application_repository.h"
#include "storage/secure_buffer.h"
#include "ui/view_state.hpp"

#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <winsock2.h>

namespace SLink::ui {
class TerminalControl;
}

namespace SLink::app {

struct TerminalAuthentication {
    enum class Mode {
        password,
        private_key,
        open_ssh_agent,
        keyboard_interactive,
    };

    Mode mode{Mode::password};
    std::string username;
    // 密码认证使用 secret；私钥及口令放在专用锁定缓冲区。无论认证成功还是失败，
    // 所有缓冲区都会在本次认证尝试后清零。
    storage::SecureBuffer secret;
    storage::SecureBuffer private_key;
    storage::SecureBuffer private_key_passphrase;
    ssh::KeyboardResponseCallback keyboard_respond;
};

// 每个已连接终端窗格独占一个 Reactor。SshSession 和 ShellChannel 的完整生命周期
// 都属于 Reactor 任务；UI 只投递不可变的输入和尺寸快照，不跨线程操作 SSH 句柄。
// 本机 CMD 也通过相同控制器管理，但不需要 SSH 保活或主机密钥校验。
class TerminalSessionController final {
public:
    using AuthenticationProvider =
        std::function<Result<TerminalAuthentication>(const storage::ServerProfile&)>;
    using HostKeyApproval =
        std::function<bool(const storage::ServerProfile&, const ssh::HostKeyInfo&)>;
    using StateCallback = std::function<void(ui::TerminalControl&, ui::ViewState)>;

    TerminalSessionController(HWND owner,
                              std::shared_ptr<ssh::IUiDispatcher> ui_dispatcher,
                              std::filesystem::path known_hosts,
                              AuthenticationProvider authentication_provider,
                              HostKeyApproval host_key_approval);
    ~TerminalSessionController();

    TerminalSessionController(const TerminalSessionController&) = delete;
    TerminalSessionController& operator=(const TerminalSessionController&) = delete;

    void Connect(ui::TerminalControl& terminal, const storage::ServerProfile& profile);
    void ConnectLocalCmd(ui::TerminalControl& terminal);
    void Disconnect(ui::TerminalControl& terminal);
    void Shutdown();
    void SetStateCallback(StateCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::app
