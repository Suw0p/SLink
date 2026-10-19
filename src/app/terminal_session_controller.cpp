// SSH 终端会话实现：每个终端窗格建立独立 Reactor、SSH Session 和 Shell Channel，
// 输入、输出、保活及防抖后的 PTY resize 均在会话线程处理。
#include "app/terminal_session_controller.hpp"

#include "app/local_terminal_session.hpp"
#include "core/perf_trace.hpp"
#include "ssh/session.hpp"
#include "ui/terminal_control.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <ws2tcpip.h>

namespace SLink::app {
namespace {

constexpr std::size_t kMinimumPtyRows = 2;
constexpr std::size_t kMinimumPtyColumns = 8;
constexpr auto kKeepaliveInterval = std::chrono::seconds(60);
constexpr std::size_t kMaximumQueuedInput = 1U * 1024U * 1024U;

enum class PaneConnectionState {
    Connecting,
    Connected,
    DisconnectedReconnectable,
    Reconnecting,
    FailedNonRetryable,
    Closing,
};

std::wstring Utf8ToWide(std::string_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0) {
        return std::wstring(input.begin(), input.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        input.data(),
                        static_cast<int>(input.size()),
                        result.data(),
                        length);
    return result;
}

std::string WideToUtf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           input.data(),
                                           static_cast<int>(input.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) {
        return "Unknown error";
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        input.data(),
                        static_cast<int>(input.size()),
                        result.data(),
                        length,
                        nullptr,
                        nullptr);
    return result;
}

void FeedStatus(ui::TerminalControl* terminal, std::wstring_view text, bool error = false) {
    if (!terminal) {
        return;
    }
    std::string line = error ? "\r\n\x1b[31m" : "\r\n\x1b[36m";
    line += WideToUtf8(text);
    line += "\x1b[0m\r\n";
    terminal->FeedUtf8(line);
}

struct HostKeyDecision {
    std::mutex mutex;
    std::condition_variable changed;
    bool ready{};
    bool approved{};
};

struct PaneSession {
    PaneSession(ui::TerminalControl* value, storage::ServerProfile server_profile)
        : terminal(value), profile(std::move(server_profile)) {}
    ~PaneSession() {
        for (auto& bytes : input) {
            if (!bytes.empty()) {
                SecureZeroMemory(bytes.data(), bytes.size());
            }
        }
    }

    ui::TerminalControl* terminal{};
    storage::ServerProfile profile;
    std::atomic<PaneConnectionState> state{PaneConnectionState::Connecting};
    std::atomic_bool close_requested{};
    std::uint64_t generation{};
    std::mutex command_mutex;
    std::deque<std::string> input;
    std::size_t queued_input_bytes{};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> resize;
    std::unique_ptr<ssh::NetworkReactor> reactor;
    ssh::ReactorTaskHandle task;
};

bool IsNonRetryable(const Status& status) {
    return status.code() == ErrorCode::authentication_failed ||
           status.code() == ErrorCode::host_key_rejected ||
           status.code() == ErrorCode::invalid_argument || status.code() == ErrorCode::not_found;
}

Status CancelledStatus() {
    return {ErrorCode::cancelled, L"SSH terminal connection was cancelled."};
}

Result<std::shared_ptr<ssh::SshSession>>
ConnectSession(const ssh::SessionConfig& config,
               ssh::NetworkReactor& reactor,
               const ssh::CancellationToken& cancellation) {
    // DNS 可能返回 IPv4/IPv6 多个候选；在统一截止时间内依次尝试非阻塞 connect，
    // 每次等待同时监听 Reactor 唤醒事件，使关闭窗格可以立即终止连接尝试。
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(config.port);
    if (getaddrinfo(config.host.c_str(), service.c_str(), &hints, &addresses) != 0) {
        return Status{ErrorCode::network_error, L"无法解析 SSH 服务器地址。"};
    }
    struct AddressGuard {
        addrinfo* value{};
        ~AddressGuard() {
            if (value) {
                freeaddrinfo(value);
            }
        }
    } guard{addresses};

    const auto deadline = std::chrono::steady_clock::now() + config.connect_timeout;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        if (cancellation.is_cancelled()) {
            return CancelledStatus();
        }
        const SOCKET candidate =
            socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) {
            continue;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(candidate, FIONBIO, &nonblocking) != 0) {
            closesocket(candidate);
            continue;
        }
        const int connected =
            ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        bool ready = connected == 0;
        if (!ready && WSAGetLastError() == WSAEWOULDBLOCK) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining > std::chrono::milliseconds::zero()) {
                ready = reactor.wait_socket(static_cast<std::uintptr_t>(candidate),
                                            ssh::WaitDirection::write,
                                            remaining,
                                            cancellation);
            }
        }
        if (ready) {
            int socket_error = 0;
            int length = sizeof(socket_error);
            if (getsockopt(candidate,
                           SOL_SOCKET,
                           SO_ERROR,
                           reinterpret_cast<char*>(&socket_error),
                           &length) == 0 &&
                socket_error == 0) {
                return ssh::SshSession::adopt_connected_socket(
                    static_cast<std::uintptr_t>(candidate), true, config);
            }
        }
        closesocket(candidate);
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }
    return cancellation.is_cancelled()
               ? Result<std::shared_ptr<ssh::SshSession>>(CancelledStatus())
               : Result<std::shared_ptr<ssh::SshSession>>(
                     Status{ErrorCode::network_error, L"在连接超时前无法连接 SSH 服务器。"});
}

Status RunTerminalSession(PaneSession& pane,
                          ssh::NetworkReactor& reactor,
                          const ssh::CancellationToken& cancellation,
                          const storage::ServerProfile& profile,
                          const std::filesystem::path& known_hosts,
                          const std::shared_ptr<ssh::IUiDispatcher>& ui_dispatcher,
                          const TerminalSessionController::HostKeyApproval& host_key_approval,
                          TerminalAuthentication authentication,
                          HWND owner,
                          bool reconnecting,
                          std::function<void(ui::ViewPhase)> notify_state) {
    // 阶段 1：建立 TCP 并推进非阻塞 SSH 握手。会话一经创建即绑定当前任务的取消等待器，
    // 后续所有 libssh2 EAGAIN 都由同一 Reactor 统一等待。
    FeedStatus(pane.terminal,
               (reconnecting ? L"正在重新连接 " : L"正在连接 ") + Utf8ToWide(profile.host) +
                   L" ...");

    ssh::SessionConfig config;
    config.host = profile.host;
    config.port = profile.port;
    auto connected = ConnectSession(config, reactor, cancellation);
    if (!connected) {
        FeedStatus(pane.terminal, connected.status().message(), true);
        return connected.status();
    }
    auto session = std::move(connected).value();
    if (auto status = reactor.bind_session_waiter(session, cancellation); !status) {
        FeedStatus(pane.terminal, status.message(), true);
        (void)session->disconnect();
        session.reset();
        return status;
    }

    while (!session->handshake_complete()) {
        if (cancellation.is_cancelled()) {
            (void)session->disconnect();
            session.reset();
            return CancelledStatus();
        }
        auto progress = session->advance_handshake();
        if (!progress) {
            FeedStatus(pane.terminal, progress.status().message(), true);
            (void)session->disconnect();
            session.reset();
            return progress.status();
        }
        if (progress.value().state == ssh::AsyncState::would_block) {
            (void)reactor.wait_socket(session->native_socket(),
                                      progress.value().wait_for,
                                      std::chrono::milliseconds(100),
                                      cancellation);
        }
    }

    // 阶段 2：在发送凭据前校验主机密钥。密钥变化立即阻断；首次连接把指纹投递给
    // UI 确认，Reactor 只等待决策对象，不直接创建窗口。
    auto inspected = session->inspect_known_host(known_hosts);
    if (!inspected) {
        FeedStatus(pane.terminal, inspected.status().message(), true);
        (void)session->disconnect();
        session.reset();
        return inspected.status();
    }
    if (inspected.value().state == ssh::HostKeyState::mismatch) {
        const Status changed{ErrorCode::host_key_rejected,
                             L"主机密钥已经变化，连接已阻止。\r\n服务器当前报告的 "
                             L"SHA-256 指纹：" +
                                 Utf8ToWide(inspected.value().sha256_fingerprint) +
                                 L"\r\n请通过可信渠道人工核对 data\\known_hosts。"};
        FeedStatus(pane.terminal, changed.message(), true);
        (void)session->disconnect();
        session.reset();
        return changed;
    }
    if (inspected.value().state == ssh::HostKeyState::not_found) {
        auto decision = std::make_shared<HostKeyDecision>();
        ui_dispatcher->post(
            [decision, profile, host_key = inspected.value(), host_key_approval, owner] {
                bool approved = false;
                if (IsWindow(owner) && host_key_approval) {
                    approved = host_key_approval(profile, host_key);
                }
                {
                    std::scoped_lock lock(decision->mutex);
                    decision->approved = approved;
                    decision->ready = true;
                }
                decision->changed.notify_all();
            });

        std::unique_lock lock(decision->mutex);
        while (!decision->ready && !cancellation.is_cancelled()) {
            decision->changed.wait_for(lock, std::chrono::milliseconds(100));
        }
        const bool approved = decision->ready && decision->approved;
        lock.unlock();
        if (!approved || cancellation.is_cancelled()) {
            const Status rejected{ErrorCode::host_key_rejected,
                                  L"首次连接的主机密钥未获确认，连接已取消。"};
            FeedStatus(pane.terminal, rejected.message(), true);
            (void)session->disconnect();
            session.reset();
            return rejected;
        }
        if (auto status = session->accept_host_key(known_hosts, false); !status) {
            FeedStatus(pane.terminal, status.message(), true);
            (void)session->disconnect();
            session.reset();
            return status;
        }
    }

    // 阶段 3：按配置执行单一认证方式。为适配 libssh2 暂时构造的 std::string 在调用后
    // 显式清零，随后无条件清空所有 SecureBuffer 和 keyboard-interactive 回调。
    Status authentication_status;
    switch (authentication.mode) {
    case TerminalAuthentication::Mode::password: {
        const auto secret_bytes = authentication.secret.bytes();
        std::string password(reinterpret_cast<const char*>(secret_bytes.data()),
                             secret_bytes.size());
        authentication_status =
            session->authenticate(ssh::PasswordAuth{authentication.username, password});
        if (!password.empty()) {
            SecureZeroMemory(password.data(), password.size());
        }
        break;
    }
    case TerminalAuthentication::Mode::private_key: {
        const auto key_bytes = authentication.private_key.bytes();
        const auto passphrase_bytes = authentication.private_key_passphrase.bytes();
        std::string private_key(reinterpret_cast<const char*>(key_bytes.data()), key_bytes.size());
        std::string passphrase(reinterpret_cast<const char*>(passphrase_bytes.data()),
                               passphrase_bytes.size());
        authentication_status = session->authenticate(
            ssh::PrivateKeyAuth{authentication.username, {}, private_key, passphrase});
        if (!private_key.empty()) {
            SecureZeroMemory(private_key.data(), private_key.size());
        }
        if (!passphrase.empty()) {
            SecureZeroMemory(passphrase.data(), passphrase.size());
        }
        break;
    }
    case TerminalAuthentication::Mode::open_ssh_agent:
        authentication_status = session->authenticate(ssh::AgentAuth{authentication.username});
        break;
    case TerminalAuthentication::Mode::keyboard_interactive:
        authentication_status = session->authenticate(
            ssh::KeyboardInteractiveAuth{authentication.username, authentication.keyboard_respond});
        break;
    }
    authentication.secret.clear();
    authentication.private_key.clear();
    authentication.private_key_passphrase.clear();
    authentication.keyboard_respond = {};
    if (!authentication_status) {
        FeedStatus(pane.terminal, authentication_status.message(), true);
        (void)session->disconnect();
        session.reset();
        return authentication_status;
    }

    if (auto status = session->configure_keepalive(kKeepaliveInterval, true); !status) {
        FeedStatus(pane.terminal, status.message(), true);
        (void)session->disconnect();
        session.reset();
        return status;
    }

    // 阶段 4：认证成功后启用 60 秒协议保活，并用 UI 已上报的最新尺寸创建 PTY。
    // 最小行列限制可避免窗口布局瞬间变窄时把 shell 提示符重排成逐字符输出。
    std::uint32_t initial_rows = 24;
    std::uint32_t initial_columns = 80;
    {
        std::scoped_lock lock(pane.command_mutex);
        if (pane.resize) {
            initial_rows = pane.resize->first;
            initial_columns = pane.resize->second;
            pane.resize.reset();
        }
    }
    initial_rows =
        std::max<std::uint32_t>(initial_rows, static_cast<std::uint32_t>(kMinimumPtyRows));
    initial_columns =
        std::max<std::uint32_t>(initial_columns, static_cast<std::uint32_t>(kMinimumPtyColumns));
    auto opened = session->open_shell("xterm-256color", initial_columns, initial_rows);
    if (!opened) {
        FeedStatus(pane.terminal, opened.status().message(), true);
        (void)session->disconnect();
        session.reset();
        return opened.status();
    }
    auto channel = std::move(opened).value();
    pane.state.store(PaneConnectionState::Connected, std::memory_order_release);
    notify_state(ui::ViewPhase::Ready);
    FeedStatus(pane.terminal, reconnecting ? L"重新连接成功。" : L"连接成功。");

    std::string pending_write;
    std::size_t write_offset = 0;
    std::array<std::byte, 64 * 1024> read_buffer{};
    auto next_keepalive = std::chrono::steady_clock::now() + kKeepaliveInterval;

    Status final_status = Status::ok();
    while (!cancellation.is_cancelled() && !pane.close_requested.load(std::memory_order_acquire) &&
           !channel->eof()) {
        ssh::WaitDirection write_wait = ssh::WaitDirection::none;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> resize;
        if (pending_write.empty()) {
            std::scoped_lock lock(pane.command_mutex);
            if (!pane.input.empty()) {
                pending_write = std::move(pane.input.front());
                pane.input.pop_front();
                pane.queued_input_bytes -= pending_write.size();
                write_offset = 0;
            }
            resize = std::exchange(pane.resize, std::nullopt);
        } else {
            std::scoped_lock lock(pane.command_mutex);
            resize = std::exchange(pane.resize, std::nullopt);
        }

        if (resize) {
            resize->first =
                std::max<std::uint32_t>(resize->first, static_cast<std::uint32_t>(kMinimumPtyRows));
            resize->second = std::max<std::uint32_t>(
                resize->second, static_cast<std::uint32_t>(kMinimumPtyColumns));
            if (auto status = channel->resize_pty(resize->second, resize->first); !status) {
                final_status = status;
                break;
            }
        }

        if (!pending_write.empty()) {
            const auto remaining = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(pending_write.data() + write_offset),
                pending_write.size() - write_offset);
            auto written = channel->write(remaining);
            if (!written) {
                final_status = written.status();
                break;
            }
            write_offset += written.value().transferred;
            if (written.value().transferred == 0) {
                write_wait = written.value().would_block;
                if (write_wait == ssh::WaitDirection::none) {
                    write_wait = ssh::WaitDirection::write;
                }
            }
            if (write_offset == pending_write.size()) {
                if (!pending_write.empty()) {
                    SecureZeroMemory(pending_write.data(), pending_write.size());
                }
                pending_write.clear();
                write_offset = 0;
            }
        }

        bool read_any = false;
        for (int stream = 0; stream < 2; ++stream) {
            for (;;) {
                auto read = channel->read(read_buffer, stream != 0);
                if (!read) {
                    final_status = read.status();
                    break;
                }
                if (read.value().transferred == 0) {
                    break;
                }
                read_any = true;
                pane.terminal->FeedTransportBytes(std::string_view(
                    reinterpret_cast<const char*>(read_buffer.data()), read.value().transferred));
                if (read.value().transferred < read_buffer.size()) {
                    break;
                }
            }
            if (!final_status) {
                break;
            }
        }
        if (!final_status) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_keepalive) {
            if (auto status = session->send_keepalive(); !status) {
                final_status = status;
                break;
            }
            next_keepalive = now + kKeepaliveInterval;
        }

        if (!read_any && (pending_write.empty() || write_wait != ssh::WaitDirection::none)) {
            const auto directions = ssh::WaitDirection::read | write_wait;
            const auto until_keepalive = std::chrono::duration_cast<std::chrono::milliseconds>(
                next_keepalive - std::chrono::steady_clock::now());
            const auto wait_duration = (std::min)(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(30)),
                (std::max)(until_keepalive, std::chrono::milliseconds(1)));
            (void)reactor.wait_socket(
                session->native_socket(), directions, wait_duration, cancellation, true);
        }
    }

    const bool closing = pane.close_requested.load(std::memory_order_acquire);
    if (cancellation.is_cancelled()) {
        final_status = CancelledStatus();
    } else if (closing) {
        final_status = Status::ok();
    } else if (final_status) {
        final_status = {ErrorCode::network_error, L"远端终端已关闭。"};
        FeedStatus(pane.terminal, final_status.message(), true);
    } else {
        FeedStatus(pane.terminal, final_status.message(), true);
    }
    channel.reset();

    (void)session->disconnect();
    session.reset();
    return final_status;
}

} // namespace

// 一个远程终端对应一个 SessionState 和独立 Reactor；本地 CMD 另存于 local_sessions。
// 映射只在 UI 线程修改，网络任务通过弱生命周期标记回传，关闭后到达的输出会被丢弃。
struct TerminalSessionController::Impl {
    HWND owner{};
    std::shared_ptr<ssh::IUiDispatcher> ui_dispatcher;
    std::filesystem::path known_hosts;
    AuthenticationProvider authentication_provider;
    HostKeyApproval host_key_approval;
    StateCallback state_callback;
    std::shared_ptr<int> callback_lifetime{std::make_shared<int>(0)};
    std::vector<std::unique_ptr<PaneSession>> sessions;
    std::vector<std::unique_ptr<LocalTerminalSession>> local_sessions;

    PaneSession* Find(ui::TerminalControl& terminal) const {
        const auto it =
            std::find_if(sessions.begin(), sessions.end(), [&terminal](const auto& value) {
                return value->terminal == &terminal;
            });
        return it == sessions.end() ? nullptr : it->get();
    }

    void NotifyState(PaneSession& pane, const ui::ViewPhase phase,
                     std::wstring message = {}) {
        if (state_callback) {
            state_callback(*pane.terminal, {phase, std::move(message), ui::ViewRecoveryAction::None});
        }
    }

    void OnAttemptComplete(ui::TerminalControl* terminal,
                           const std::uint64_t generation,
                           ssh::ReactorTaskResult result) {
        if (!terminal)
            return;
        auto* pane = Find(*terminal);
        if (!pane || pane->generation != generation ||
            pane->state.load(std::memory_order_acquire) == PaneConnectionState::Closing) {
            return;
        }
        if (result.cancelled)
            return;
        pane->state.store(IsNonRetryable(result.status)
                              ? PaneConnectionState::FailedNonRetryable
                              : PaneConnectionState::DisconnectedReconnectable,
                          std::memory_order_release);
        NotifyState(*pane,
                    IsNonRetryable(result.status) ? ui::ViewPhase::Error
                                                  : ui::ViewPhase::Disconnected,
                    result.status.message());
    }

    void StartAttempt(PaneSession& pane, const bool reconnecting) {
        if (!authentication_provider)
            return;
        auto authentication = authentication_provider(pane.profile);
        if (!authentication) {
            if (authentication.status().code() != ErrorCode::cancelled)
                FeedStatus(pane.terminal, authentication.status().message(), true);
            pane.state.store(PaneConnectionState::FailedNonRetryable, std::memory_order_release);
            NotifyState(pane, ui::ViewPhase::Error, authentication.status().message());
            return;
        }
        if (pane.reactor) {
            pane.reactor->shutdown();
            pane.reactor.reset();
        }
        pane.close_requested.store(false, std::memory_order_release);
        pane.state.store(reconnecting ? PaneConnectionState::Reconnecting
                                      : PaneConnectionState::Connecting,
                         std::memory_order_release);
        const std::uint64_t generation = ++pane.generation;
        NotifyState(pane, ui::ViewPhase::Connecting);
        const std::weak_ptr<int> weak_lifetime = callback_lifetime;
        auto notify_state = [this,
                             weak_lifetime,
                             terminal = pane.terminal,
                             generation](const ui::ViewPhase phase) {
            ui_dispatcher->post([this, weak_lifetime, terminal, generation, phase] {
                if (weak_lifetime.expired()) {
                    return;
                }
                if (auto* current = Find(*terminal); current && current->generation == generation) {
                    NotifyState(*current, phase);
                }
            });
        };
        try {
            pane.reactor = std::make_unique<ssh::NetworkReactor>(ui_dispatcher);
            auto* reactor = pane.reactor.get();
            auto authentication_data =
                std::make_shared<TerminalAuthentication>(std::move(authentication).value());
            auto* pane_pointer = &pane;
            pane.task = pane.reactor->submit(
                [pane_pointer,
                 reactor,
                 profile = pane.profile,
                 known_hosts = known_hosts,
                 dispatcher = ui_dispatcher,
                 approval = host_key_approval,
                 authentication = std::move(authentication_data),
                 owner = owner,
                 reconnecting,
                 notify_state = std::move(notify_state)](
                    const ssh::CancellationToken& cancellation) mutable {
                    return RunTerminalSession(*pane_pointer,
                                              *reactor,
                                              cancellation,
                                              profile,
                                              known_hosts,
                                              dispatcher,
                                              approval,
                                              std::move(*authentication),
                                              owner,
                                              reconnecting,
                                              notify_state);
                },
                [this, weak_lifetime, terminal = pane.terminal,
                 generation](ssh::ReactorTaskResult result) {
                    if (weak_lifetime.expired()) {
                        return;
                    }
                    OnAttemptComplete(terminal, generation, std::move(result));
                });
        } catch (const std::exception& error) {
            pane.state.store(PaneConnectionState::FailedNonRetryable, std::memory_order_release);
            NotifyState(pane, ui::ViewPhase::Error, L"无法启动 SSH 网络线程");
            FeedStatus(pane.terminal,
                       L"无法启动 SSH 网络线程：" + Utf8ToWide(error.what() ? error.what() : ""),
                       true);
        }
    }

    void Disconnect(ui::TerminalControl& terminal) {
        core::PerfScope timing("terminal_disconnect");
        const auto local = std::find_if(
            local_sessions.begin(), local_sessions.end(), [&terminal](const auto& value) {
                return value->terminal() == &terminal;
            });
        if (local != local_sessions.end()) {
            (*local)->Stop();
            local_sessions.erase(local);
        }
        const auto it =
            std::find_if(sessions.begin(), sessions.end(), [&terminal](const auto& value) {
                return value->terminal == &terminal;
            });
        if (it == sessions.end()) {
            terminal.SetInputCallback({});
            terminal.SetResizeCallback({});
            return;
        }
        terminal.SetInputCallback({});
        terminal.SetResizeCallback({});
        auto& pane = **it;
        pane.state.store(PaneConnectionState::Closing, std::memory_order_release);
        pane.close_requested.store(true, std::memory_order_release);
        pane.task.notify();
        pane.task.cancel();
        if (pane.reactor) {
            pane.reactor->shutdown();
        }
        sessions.erase(it);
    }

    void Shutdown() {
        callback_lifetime.reset();
        while (!local_sessions.empty()) {
            auto session = std::move(local_sessions.back());
            local_sessions.pop_back();
            session->Stop();
        }
        while (!sessions.empty())
            Disconnect(*sessions.back()->terminal);
    }
};

TerminalSessionController::TerminalSessionController(
    HWND owner,
    std::shared_ptr<ssh::IUiDispatcher> ui_dispatcher,
    std::filesystem::path known_hosts,
    AuthenticationProvider authentication_provider,
    HostKeyApproval host_key_approval)
    : impl_(std::make_unique<Impl>()) {
    impl_->owner = owner;
    impl_->ui_dispatcher = std::move(ui_dispatcher);
    impl_->known_hosts = std::move(known_hosts);
    impl_->authentication_provider = std::move(authentication_provider);
    impl_->host_key_approval = std::move(host_key_approval);
}

TerminalSessionController::~TerminalSessionController() {
    Shutdown();
}

void TerminalSessionController::Connect(ui::TerminalControl& terminal,
                                        const storage::ServerProfile& profile) {
    impl_->Disconnect(terminal);
    if (!ssh::libssh2_enabled()) {
        FeedStatus(&terminal, L"此构建未启用 libssh2，无法建立真实 SSH 连接。", true);
        if (impl_->state_callback) {
            impl_->state_callback(terminal,
                                  {ui::ViewPhase::Error, L"未启用 SSH", ui::ViewRecoveryAction::None});
        }
        return;
    }
    if (!impl_->authentication_provider) {
        FeedStatus(&terminal, L"没有可用的安全认证提供器。", true);
        if (impl_->state_callback) {
            impl_->state_callback(terminal, {ui::ViewPhase::Error, L"无认证提供器",
                                             ui::ViewRecoveryAction::None});
        }
        return;
    }
    auto pane = std::make_unique<PaneSession>(&terminal, profile);
    auto* pane_pointer = pane.get();
    terminal.SetInputCallback([controller = impl_.get(),
                               pane_pointer](const ui::TerminalInputEvent event) {
        if (event.bytes.empty()) {
            return;
        }
        const auto state = pane_pointer->state.load(std::memory_order_acquire);
        if (state == PaneConnectionState::Closing ||
            state == PaneConnectionState::FailedNonRetryable ||
            (event.origin == ui::TerminalInputOrigin::Protocol &&
             state != PaneConnectionState::Connected)) {
            return;
        }
        bool overflow = false;
        {
            std::scoped_lock lock(pane_pointer->command_mutex);
            if (event.bytes.size() > kMaximumQueuedInput ||
                pane_pointer->queued_input_bytes > kMaximumQueuedInput - event.bytes.size()) {
                overflow = true;
            } else {
                pane_pointer->input.emplace_back(event.bytes);
                pane_pointer->queued_input_bytes += event.bytes.size();
            }
        }
        if (overflow) {
            FeedStatus(pane_pointer->terminal, L"断线输入缓存已达到 1 MiB，后续输入已拒绝。", true);
            return;
        }
        if (event.origin == ui::TerminalInputOrigin::User &&
            state == PaneConnectionState::DisconnectedReconnectable) {
            auto expected = PaneConnectionState::DisconnectedReconnectable;
            if (pane_pointer->state.compare_exchange_strong(
                    expected, PaneConnectionState::Reconnecting, std::memory_order_acq_rel)) {
                controller->StartAttempt(*pane_pointer, true);
                return;
            }
        }
        pane_pointer->task.notify();
    });
    terminal.SetResizeCallback([pane_pointer](std::size_t rows, std::size_t columns) {
        {
            std::scoped_lock lock(pane_pointer->command_mutex);
            pane_pointer->resize = {
                static_cast<std::uint32_t>(std::clamp<std::size_t>(rows, kMinimumPtyRows, 4096)),
                static_cast<std::uint32_t>(
                    std::clamp<std::size_t>(columns, kMinimumPtyColumns, 4096))};
        }
        pane_pointer->task.notify();
    });
    {
        std::scoped_lock lock(pane_pointer->command_mutex);
        pane_pointer->resize = {static_cast<std::uint32_t>(std::clamp<std::size_t>(
                                    terminal.rows(), kMinimumPtyRows, 4096)),
                                static_cast<std::uint32_t>(std::clamp<std::size_t>(
                                    terminal.columns(), kMinimumPtyColumns, 4096))};
    }

    impl_->sessions.push_back(std::move(pane));
    impl_->StartAttempt(*pane_pointer, false);
}

void TerminalSessionController::ConnectLocalCmd(ui::TerminalControl& terminal) {
    impl_->Disconnect(terminal);
    auto session = std::make_unique<LocalTerminalSession>(terminal);
    if (const Status status = session->Start(); !status) {
        FeedStatus(&terminal, status.message(), true);
        if (impl_->state_callback) {
            impl_->state_callback(terminal, {ui::ViewPhase::Error, status.message(),
                                             ui::ViewRecoveryAction::None});
        }
        return;
    }
    impl_->local_sessions.push_back(std::move(session));
    if (impl_->state_callback) {
        impl_->state_callback(terminal, {ui::ViewPhase::Ready, {}, ui::ViewRecoveryAction::None});
    }
}

void TerminalSessionController::Disconnect(ui::TerminalControl& terminal) {
    impl_->Disconnect(terminal);
}

void TerminalSessionController::Shutdown() {
    if (impl_) {
        impl_->Shutdown();
    }
}

void TerminalSessionController::SetStateCallback(StateCallback callback) {
    impl_->state_callback = std::move(callback);
}

} // namespace SLink::app
