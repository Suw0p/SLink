// 独立 TCP 端口代理实现：每条活动规则拥有监听生命周期和连接转发工作，规则身份由
// 监听地址与端口共同确定，不复用终端 SSH 会话。
#include "app/port_forward_controller.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <list>
#include <thread>
#include <unordered_map>
#include <utility>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace SLink::app {
namespace {

constexpr std::size_t kRelayBufferSize = 32U * 1024U;
constexpr std::size_t kMaximumConnections = 24;
constexpr auto kConnectTimeout = std::chrono::seconds(10);

struct WinsockGuard {
    bool initialized{};
    WinsockGuard() {
        WSADATA data{};
        initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockGuard() {
        if (initialized)
            WSACleanup();
    }
};

struct SocketGuard {
    SOCKET value{INVALID_SOCKET};
    SocketGuard() = default;
    explicit SocketGuard(SOCKET socket) : value(socket) {}
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    SocketGuard(SocketGuard&& other) noexcept : value(std::exchange(other.value, INVALID_SOCKET)) {}
    SocketGuard& operator=(SocketGuard&& other) noexcept {
        if (this == &other)
            return *this;
        if (value != INVALID_SOCKET)
            closesocket(value);
        value = std::exchange(other.value, INVALID_SOCKET);
        return *this;
    }
    ~SocketGuard() {
        if (value != INVALID_SOCKET)
            closesocket(value);
    }
};

Result<SocketGuard> CreateListener(const TcpForwardSpec& spec) {
    SocketGuard listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.value == INVALID_SOCKET) {
        return Status{ErrorCode::network_error, L"无法创建本机监听套接字。"};
    }
    u_long nonblocking = 1;
    if (ioctlsocket(listener.value, FIONBIO, &nonblocking) != 0) {
        return Status{ErrorCode::network_error, L"无法启用非阻塞本机监听。"};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(spec.local_port);
    if (InetPtonA(AF_INET, spec.bind_address.c_str(), &address.sin_addr) != 1) {
        return Status{ErrorCode::invalid_argument, L"本机监听地址无效。"};
    }
    if (bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener.value, SOMAXCONN) != 0) {
        return Status{ErrorCode::network_error, L"本机监听地址不可用，端口可能已被占用。"};
    }
    return std::move(listener);
}

Result<SocketGuard> ConnectTarget(const TcpForwardSpec& spec, bool& connecting) {
    SocketGuard target(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (target.value == INVALID_SOCKET) {
        return Status{ErrorCode::network_error, L"无法创建目标连接套接字。"};
    }
    u_long nonblocking = 1;
    if (ioctlsocket(target.value, FIONBIO, &nonblocking) != 0) {
        return Status{ErrorCode::network_error, L"无法启用非阻塞目标连接。"};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(spec.target_port);
    if (InetPtonA(AF_INET, spec.target_address.c_str(), &address.sin_addr) != 1) {
        return Status{ErrorCode::invalid_argument, L"目标服务器 IP 无效。"};
    }
    const int result =
        connect(target.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result == 0) {
        connecting = false;
        return std::move(target);
    }
    const int error = WSAGetLastError();
    if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL) {
        return Status{ErrorCode::network_error, L"无法连接目标服务器。"};
    }
    connecting = true;
    return std::move(target);
}

struct RelayConnection {
    SocketGuard client;
    SocketGuard target;
    std::array<std::byte, kRelayBufferSize> client_to_target{};
    std::size_t client_to_target_begin{};
    std::size_t client_to_target_end{};
    std::array<std::byte, kRelayBufferSize> target_to_client{};
    std::size_t target_to_client_begin{};
    std::size_t target_to_client_end{};
    std::chrono::steady_clock::time_point connect_deadline;
    bool connecting{};
    bool client_read_closed{};
    bool target_read_closed{};
    bool client_write_closed{};
    bool target_write_closed{};
    bool failed{};

    RelayConnection(SocketGuard accepted, SocketGuard destination, bool connection_pending)
        : client(std::move(accepted)), target(std::move(destination)),
          connect_deadline(std::chrono::steady_clock::now() + kConnectTimeout),
          connecting(connection_pending) {}
};

Status
RunProxy(const TcpForwardSpec& spec, std::stop_token stop, const std::function<void()>& listening) {
    WinsockGuard winsock;
    if (!winsock.initialized) {
        return {ErrorCode::network_error, L"Windows Socket 初始化失败。"};
    }
    auto listener_result = CreateListener(spec);
    if (!listener_result)
        return listener_result.status();
    SocketGuard listener = std::move(listener_result).value();
    if (listening)
        listening();

    std::list<RelayConnection> connections;
    while (!stop.stop_requested()) {
        fd_set reads;
        fd_set writes;
        fd_set errors;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(listener.value, &reads);

        for (auto& connection : connections) {
            if (connection.connecting) {
                FD_SET(connection.target.value, &writes);
                FD_SET(connection.target.value, &errors);
                continue;
            }
            if (!connection.client_read_closed &&
                connection.client_to_target_begin == connection.client_to_target_end) {
                FD_SET(connection.client.value, &reads);
            }
            if (!connection.target_read_closed &&
                connection.target_to_client_begin == connection.target_to_client_end) {
                FD_SET(connection.target.value, &reads);
            }
            if (connection.client_to_target_begin < connection.client_to_target_end) {
                FD_SET(connection.target.value, &writes);
            }
            if (connection.target_to_client_begin < connection.target_to_client_end) {
                FD_SET(connection.client.value, &writes);
            }
            FD_SET(connection.client.value, &errors);
            FD_SET(connection.target.value, &errors);
        }

        timeval timeout{0, 50'000};
        const int selected = select(0, &reads, &writes, &errors, &timeout);
        if (selected == SOCKET_ERROR) {
            return {ErrorCode::network_error, L"端口代理的 socket 轮询失败。"};
        }

        if (FD_ISSET(listener.value, &reads)) {
            while (connections.size() < kMaximumConnections) {
                SocketGuard accepted(accept(listener.value, nullptr, nullptr));
                if (accepted.value == INVALID_SOCKET)
                    break;
                u_long nonblocking = 1;
                if (ioctlsocket(accepted.value, FIONBIO, &nonblocking) != 0)
                    continue;
                bool connecting = false;
                auto target = ConnectTarget(spec, connecting);
                if (!target)
                    continue;
                connections.emplace_back(
                    std::move(accepted), std::move(target).value(), connecting);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        for (auto& connection : connections) {
            if (FD_ISSET(connection.client.value, &errors) ||
                FD_ISSET(connection.target.value, &errors)) {
                connection.failed = true;
                continue;
            }
            if (connection.connecting) {
                if (now >= connection.connect_deadline) {
                    connection.failed = true;
                    continue;
                }
                if (!FD_ISSET(connection.target.value, &writes))
                    continue;
                int socket_error = 0;
                int length = sizeof(socket_error);
                if (getsockopt(connection.target.value,
                               SOL_SOCKET,
                               SO_ERROR,
                               reinterpret_cast<char*>(&socket_error),
                               &length) != 0 ||
                    socket_error != 0) {
                    connection.failed = true;
                    continue;
                }
                connection.connecting = false;
            }

            if (FD_ISSET(connection.client.value, &reads)) {
                const int received =
                    recv(connection.client.value,
                         reinterpret_cast<char*>(connection.client_to_target.data()),
                         static_cast<int>(connection.client_to_target.size()),
                         0);
                if (received > 0) {
                    connection.client_to_target_begin = 0;
                    connection.client_to_target_end = static_cast<std::size_t>(received);
                } else if (received == 0) {
                    connection.client_read_closed = true;
                } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    connection.failed = true;
                }
            }
            if (FD_ISSET(connection.target.value, &reads)) {
                const int received =
                    recv(connection.target.value,
                         reinterpret_cast<char*>(connection.target_to_client.data()),
                         static_cast<int>(connection.target_to_client.size()),
                         0);
                if (received > 0) {
                    connection.target_to_client_begin = 0;
                    connection.target_to_client_end = static_cast<std::size_t>(received);
                } else if (received == 0) {
                    connection.target_read_closed = true;
                } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    connection.failed = true;
                }
            }
            if (FD_ISSET(connection.target.value, &writes) &&
                connection.client_to_target_begin < connection.client_to_target_end) {
                const int sent =
                    send(connection.target.value,
                         reinterpret_cast<const char*>(connection.client_to_target.data() +
                                                       connection.client_to_target_begin),
                         static_cast<int>(connection.client_to_target_end -
                                          connection.client_to_target_begin),
                         0);
                if (sent > 0) {
                    connection.client_to_target_begin += static_cast<std::size_t>(sent);
                } else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
                    connection.failed = true;
                }
            }
            if (FD_ISSET(connection.client.value, &writes) &&
                connection.target_to_client_begin < connection.target_to_client_end) {
                const int sent =
                    send(connection.client.value,
                         reinterpret_cast<const char*>(connection.target_to_client.data() +
                                                       connection.target_to_client_begin),
                         static_cast<int>(connection.target_to_client_end -
                                          connection.target_to_client_begin),
                         0);
                if (sent > 0) {
                    connection.target_to_client_begin += static_cast<std::size_t>(sent);
                } else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
                    connection.failed = true;
                }
            }

            if (connection.client_read_closed &&
                connection.client_to_target_begin == connection.client_to_target_end &&
                !connection.target_write_closed) {
                shutdown(connection.target.value, SD_SEND);
                connection.target_write_closed = true;
            }
            if (connection.target_read_closed &&
                connection.target_to_client_begin == connection.target_to_client_end &&
                !connection.client_write_closed) {
                shutdown(connection.client.value, SD_SEND);
                connection.client_write_closed = true;
            }
        }
        connections.remove_if([](const RelayConnection& connection) {
            const bool drained =
                connection.client_to_target_begin == connection.client_to_target_end &&
                connection.target_to_client_begin == connection.target_to_client_end;
            return connection.failed ||
                   (drained && connection.client_read_closed && connection.target_read_closed);
        });
    }
    return {ErrorCode::cancelled, L"端口转发已停止。"};
}

} // namespace

struct PortForwardController::Impl {
    struct Job {
        TcpForwardSpec spec;
        std::jthread worker;
    };

    std::shared_ptr<ssh::IUiDispatcher> dispatcher;
    std::unordered_map<std::uint16_t, std::unique_ptr<Job>> jobs;

    void Post(UpdateCallback callback, PortForwardUpdate update) const {
        if (!dispatcher || !callback)
            return;
        dispatcher->post([callback = std::move(callback), update = std::move(update)]() mutable {
            callback(std::move(update));
        });
    }

    void Stop(std::uint16_t port) {
        const auto found = jobs.find(port);
        if (found == jobs.end())
            return;
        found->second->worker.request_stop();
        jobs.erase(found); // jthread joins before the job is destroyed.
    }

    void Shutdown() noexcept {
        while (!jobs.empty())
            Stop(jobs.begin()->first);
    }
};

PortForwardController::PortForwardController(std::shared_ptr<ssh::IUiDispatcher> ui_dispatcher)
    : impl_(std::make_unique<Impl>()) {
    impl_->dispatcher = std::move(ui_dispatcher);
}

PortForwardController::~PortForwardController() {
    Shutdown();
}

void PortForwardController::Start(TcpForwardSpec spec, UpdateCallback callback) {
    if (spec.local_port == 0 || spec.target_port == 0 || spec.bind_address.empty() ||
        spec.target_address.empty() || !callback) {
        return;
    }
    impl_->Stop(spec.local_port);
    callback({spec, PortForwardState::starting, L"正在启动本机监听..."});

    auto job = std::make_unique<Impl::Job>();
    job->spec = spec;
    auto* state = impl_.get();
    job->worker = std::jthread([state, spec, callback](std::stop_token stop) mutable {
        const auto listening = [state, spec, callback] {
            state->Post(callback,
                        {spec,
                         PortForwardState::running,
                         L"SLink.exe (PID " + std::to_wstring(GetCurrentProcessId()) + L")"});
        };
        const Status result = RunProxy(spec, stop, listening);
        if (result.code() == ErrorCode::cancelled) {
            state->Post(callback, {spec, PortForwardState::stopped, {}});
        } else if (!result) {
            state->Post(callback, {spec, PortForwardState::failed, result.message()});
        }
    });
    impl_->jobs.emplace(spec.local_port, std::move(job));
}

void PortForwardController::Stop(std::uint16_t local_port) {
    impl_->Stop(local_port);
}

void PortForwardController::Shutdown() noexcept {
    if (impl_)
        impl_->Shutdown();
}

} // namespace SLink::app
