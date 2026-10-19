#pragma once

#include "core/status.hpp"
#include "ssh/network_reactor.hpp"
#include "ssh/reactor_endpoint.hpp"
#include "storage/application_repository.h"
#include "storage/secure_buffer.h"
#include "transfer/file_uri.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace SLink::app {

// 认证材料必须在调用 AsyncOpen 前由应用层准备好。控制器不会从 Reactor 线程弹出
// 交互窗口；SecureBuffer 中的敏感数据在认证尝试结束后立即清零。
struct FilePasswordAuthentication {
    storage::SecureBuffer password;
};

struct FilePrivateKeyAuthentication {
    storage::SecureBuffer public_key;
    storage::SecureBuffer private_key;
    storage::SecureBuffer passphrase;
};

struct FileAgentAuthentication {};

struct FileKeyboardInteractiveAuthentication {
    // 回调运行在 NetworkReactor 所在线程，只能使用 UI 预先收集的响应，禁止在此
    // 弹窗或同步等待 UI 线程，否则会阻塞同一连接上的全部 SFTP 操作。
    ssh::KeyboardResponseCallback respond;
};

using FileSessionAuthentication = std::variant<FilePasswordAuthentication,
                                               FilePrivateKeyAuthentication,
                                               FileAgentAuthentication,
                                               FileKeyboardInteractiveAuthentication>;

enum class FileHostKeyPolicy {
    // known_hosts 中不存在或指纹发生变化时拒绝连接，结果中会携带供 UI 展示的指纹。
    require_known,
    // 首次出现的主机密钥写入 known_hosts；已记录的密钥发生变化时仍然阻止连接，
    // 绝不静默替换，避免中间人攻击被自动接受。
    accept_new,
};

// 仅由 OpenDedicated 创建的独立传输连接实现。该接口刻意与 IFileEndpoint 分离，
// 防止本地端点及浏览/编辑端点被迫暴露 SSH 专用能力。
class IDedicatedFileEndpoint {
public:
    virtual ~IDedicatedFileEndpoint() = default;
    [[nodiscard]] virtual Status SendKeepalive() = 0;
};

struct FileSessionOpenResult {
    Status status;
    std::shared_ptr<ssh::ReactorEndpoint> endpoint;
    std::optional<ssh::HostKeyInfo> host_key;
    std::string initial_directory;

    [[nodiscard]] bool host_key_confirmation_required() const noexcept {
        return status.code() == ErrorCode::host_key_rejected && host_key &&
               host_key->state == ssh::HostKeyState::not_found;
    }
};

struct FileDirectoryListResult {
    Status status;
    transfer::FileUri directory;
    std::vector<transfer::DirectoryEntry> entries;
};

// 文件浏览和远程编辑的 SFTP 会话管理器。
//
// 每个服务器会话独占一个持久 NetworkReactor。AsyncOpen 会按 server-id 复用连接并
// 增加使用计数，因此每次成功打开都必须在面板/编辑器关闭时配对调用 Close。
// EndpointFor 只借用现有端点，不增加计数，调用方必须先持有一次成功的 AsyncOpen。
//
// ReactorEndpoint 的 shared_ptr 可以延长 Reactor 生命周期。应用退出时应先销毁传输、
// 编辑工作线程、流和所有端点副本，再销毁本控制器，确保连接及时释放。
class FileSessionController final {
public:
    using OpenCompletion = std::function<void(FileSessionOpenResult)>;
    using ListCompletion = std::function<void(FileDirectoryListResult)>;

    FileSessionController(std::shared_ptr<ssh::IUiDispatcher> ui_dispatcher,
                          std::filesystem::path known_hosts);
    ~FileSessionController();

    FileSessionController(const FileSessionController&) = delete;
    FileSessionController& operator=(const FileSessionController&) = delete;

    // completion 始终经 IUiDispatcher 回到 UI 线程。同一份服务器配置正在连接时，
    // 后续调用会合并到该连接尝试，未使用的重复凭据会立即销毁。
    [[nodiscard]] ssh::ReactorTaskHandle AsyncOpen(storage::ServerProfile profile,
                                                   FileSessionAuthentication authentication,
                                                   FileHostKeyPolicy host_key_policy,
                                                   OpenCompletion completion);

    // 释放一个成功 AsyncOpen 对应的使用计数；最后一个使用者关闭后立即移除端点，
    // 并在其 Reactor 线程执行断开操作。
    void Close(std::string_view server_id);

    // 连接已被网络错误证实不可用时，强制淘汰整个服务器会话。该操作不按
    // consumer 计数递减：死连接的所有借用者都必须在下次操作前重新 AsyncOpen。
    // 调用方只应在已经收到 I/O/网络/协议失败后使用，不能用于普通的面板关闭。
    void Invalidate(std::string_view server_id);

    [[nodiscard]] std::shared_ptr<ssh::ReactorEndpoint>
    EndpointFor(const transfer::FileUri& uri) const;

    // 创建独立的 TCP/SSH/SFTP/Reactor 栈，供传输连接池租用。最后一个 shared_ptr
    // 释放时连接自动断开；文件面板和编辑器仍使用 AsyncOpen/EndpointFor 的共享会话。
    [[nodiscard]] Result<std::shared_ptr<transfer::IFileEndpoint>>
    OpenDedicated(storage::ServerProfile profile,
                  FileSessionAuthentication authentication,
                  FileHostKeyPolicy host_key_policy);

    // 目录枚举在 NetworkReactor 上执行，只把不可变结果快照投递给 UI，因此 UI 线程
    // 不会直接接触 libssh2 句柄。
    [[nodiscard]] ssh::ReactorTaskHandle AsyncList(transfer::FileUri directory,
                                                   ListCompletion completion);

    [[nodiscard]] std::size_t consumer_count(std::string_view server_id) const noexcept;

    // 幂等关闭：取消待处理任务，在各自 Reactor 上断开 SFTP，并丢弃随后到达的 UI 回调。
    void Shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::app
