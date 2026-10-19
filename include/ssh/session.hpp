#pragma once

#include "core/status.hpp"
#include "transfer/sftp_endpoint.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SLink::ssh {

enum class WaitDirection : std::uint8_t {
    none = 0,
    read = 1,
    write = 2,
};

constexpr WaitDirection operator|(WaitDirection left, WaitDirection right) noexcept {
    return static_cast<WaitDirection>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}
constexpr bool has_direction(WaitDirection value, WaitDirection flag) noexcept {
    return (static_cast<unsigned>(value) & static_cast<unsigned>(flag)) != 0;
}

enum class AsyncState {
    complete,
    would_block,
};

struct AsyncProgress {
    AsyncState state{AsyncState::complete};
    WaitDirection wait_for{WaitDirection::none};
};

enum class HostKeyState {
    matched,
    not_found,
    mismatch,
};

struct HostKeyInfo {
    HostKeyState state{HostKeyState::not_found};
    std::string algorithm;
    std::string sha256_fingerprint;
};

struct SessionConfig {
    std::string host;
    std::uint16_t port{22};
    std::chrono::milliseconds connect_timeout{10'000};
    std::chrono::milliseconds io_timeout{15'000};
};

struct PasswordAuth {
    std::string username;
    std::string password;
};

// 私钥字节直接来自加密 Vault，本层绝不写临时密钥文件；libssh2 可从私钥推导公钥时，
// public_key 允许为空。
struct PrivateKeyAuth {
    std::string username;
    std::string public_key;
    std::string private_key;
    std::string passphrase;
};

struct AgentAuth {
    std::string username;
};

struct KeyboardPrompt {
    std::string text;
    bool echo{};
};

using KeyboardResponseCallback = std::function<Result<std::vector<std::string>>(
    std::string_view name, std::string_view instruction, std::span<const KeyboardPrompt> prompts)>;

struct KeyboardInteractiveAuth {
    std::string username;
    KeyboardResponseCallback respond;
};

// 仅在会话所属线程、libssh2 返回 EAGAIN 后调用；Reactor 可注入 socket 轮询，
// 返回 false 表示超时或取消。
using SocketWaitCallback = std::function<bool(WaitDirection, std::chrono::milliseconds timeout)>;

class ShellChannel;
class Libssh2SftpAdapter;

// SshSession 及全部子 Channel/Adapter 具有严格单线程亲和性，必须在所属 Reactor 线程
// 创建和使用，防止并发进入同一个 LIBSSH2_SESSION。
class SshSession final : public std::enable_shared_from_this<SshSession> {
public:
    ~SshSession();
    SshSession(const SshSession&) = delete;
    SshSession& operator=(const SshSession&) = delete;

    // 创建并连接 WinSock TCP socket，然后初始化非阻塞 libssh2；SSH 握手由
    // advance_handshake() 分步推进。
    [[nodiscard]] static Result<std::shared_ptr<SshSession>> connect(SessionConfig config,
                                                                     SocketWaitCallback wait = {});

    // 测试/集成入口：接管已连接 WinSock SOCKET。take_ownership=true 时析构负责关闭。
    [[nodiscard]] static Result<std::shared_ptr<SshSession>>
    adopt_connected_socket(std::uintptr_t native_socket,
                           bool take_ownership,
                           SessionConfig config,
                           SocketWaitCallback wait = {});

    // 只推进一次非阻塞握手，不在内部等待；按返回的读写方向就绪后再次调用。
    [[nodiscard]] Result<AsyncProgress> advance_handshake();
    [[nodiscard]] WaitDirection blocked_directions() const;
    [[nodiscard]] bool handshake_complete() const noexcept;
    [[nodiscard]] bool authenticated() const noexcept;

    [[nodiscard]] Result<HostKeyInfo>
    inspect_known_host(const std::filesystem::path& known_hosts_file);
    // 持久化本次协商的主机密钥。只有 replace_changed=true 才允许替换不匹配记录，
    // 防止调用方无意间静默接受主机密钥变化。
    [[nodiscard]] Status accept_host_key(const std::filesystem::path& known_hosts_file,
                                         bool replace_changed);

    [[nodiscard]] Status authenticate(const PasswordAuth& auth);
    [[nodiscard]] Status authenticate(const PrivateKeyAuth& auth);
    [[nodiscard]] Status authenticate(const AgentAuth& auth);
    [[nodiscard]] Status authenticate(const KeyboardInteractiveAuth& auth);

    [[nodiscard]] Result<std::unique_ptr<ShellChannel>>
    open_shell(std::string terminal_type, std::uint32_t columns, std::uint32_t rows);

    // 返回的 SFTP adapter 仅能在会话所属线程使用；父会话关闭后自动报告不可用。
    [[nodiscard]] Result<std::shared_ptr<transfer::ISftpAdapter>>
    create_sftp_adapter(std::string server_id);

    // 配置/发送 SSH 协议保活，必须在所属 Reactor 调用。仅在空闲间隔到期后真正发包。
    [[nodiscard]] Status configure_keepalive(std::chrono::seconds interval, bool want_reply = true);
    [[nodiscard]] Status send_keepalive();

    [[nodiscard]] Status disconnect();
    [[nodiscard]] const SessionConfig& config() const noexcept;
    // 在 Reactor 任务之间替换 EAGAIN 等待器，使每个操作使用自己的取消令牌。
    [[nodiscard]] Status set_socket_waiter(SocketWaitCallback wait);
    // 仅供 NetworkReactor 轮询就绪；调用方不得关闭该值或直接进行 socket I/O。
    [[nodiscard]] std::uintptr_t native_socket() const noexcept;

private:
    class Impl;
    explicit SshSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class ShellChannel;
    friend class Libssh2SftpAdapter;
};

struct ChannelIo {
    std::size_t transferred{};
    bool eof{};
    WaitDirection would_block{WaitDirection::none};
};

class ShellChannel final {
public:
    ~ShellChannel();
    ShellChannel(const ShellChannel&) = delete;
    ShellChannel& operator=(const ShellChannel&) = delete;

    [[nodiscard]] Result<ChannelIo> read(std::span<std::byte> output, bool stderr_stream = false);
    [[nodiscard]] Result<ChannelIo> write(std::span<const std::byte> input);
    [[nodiscard]] Status resize_pty(std::uint32_t columns, std::uint32_t rows);
    [[nodiscard]] Status send_eof();
    [[nodiscard]] bool eof() const;

private:
    class Impl;
    explicit ShellChannel(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class SshSession;
};

// 运行时能力查询放在库内而非头文件宏，应用目标无需继承库的私有编译定义。
[[nodiscard]] bool libssh2_enabled() noexcept;

} // namespace SLink::ssh
