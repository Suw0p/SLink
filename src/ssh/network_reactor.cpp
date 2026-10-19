// 单连接 Reactor 实现：串行执行网络任务，并用内部唤醒 socket 统一处理中止、带外输入
// 和 WinSock 就绪等待，避免 libssh2 被多线程并发调用。
#include "ssh/network_reactor.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace SLink::ssh {

void ReactorTaskHandle::notify() const {
    if (wake_)
        wake_();
}

void ReactorTaskHandle::cancel() const {
    if (!state_)
        return;
    state_->store(true, std::memory_order_release);
    if (wake_)
        wake_();
}

class NetworkReactor::Impl : public std::enable_shared_from_this<Impl> {
public:
    struct Item {
        ReactorTaskId id{};
        ReactorTask task;
        ReactorCompletion completion;
        std::shared_ptr<std::atomic_bool> cancelled;
    };

    explicit Impl(std::shared_ptr<IUiDispatcher> dispatcher)
        : ui_dispatcher(std::move(dispatcher)) {
        if (!ui_dispatcher) {
            throw std::invalid_argument("NetworkReactor requires a UI dispatcher");
        }
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed for NetworkReactor");
        }
        winsock_started = true;
        try {
            create_wake_pair();
        } catch (...) {
            if (wake_sender != INVALID_SOCKET) {
                closesocket(wake_sender);
                wake_sender = INVALID_SOCKET;
            }
            if (wake_receiver != INVALID_SOCKET) {
                closesocket(wake_receiver);
                wake_receiver = INVALID_SOCKET;
            }
            WSACleanup();
            winsock_started = false;
            throw;
        }
    }

    ~Impl() {
        if (wake_sender != INVALID_SOCKET)
            closesocket(wake_sender);
        if (wake_receiver != INVALID_SOCKET)
            closesocket(wake_receiver);
        if (winsock_started)
            WSACleanup();
    }

    void start() {
        // 构造线程必须等 worker_id 发布后才能返回，否则紧接着调用 is_reactor_thread
        // 或 submit/关闭时可能观察到尚未初始化的线程状态。
        auto self = shared_from_this();
        worker = std::thread([self = std::move(self)] {
            self->run();
        });
        std::unique_lock lock(mutex);
        started.wait(lock, [this] {
            return worker_started;
        });
    }

    void create_wake_pair() {
        wake_receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        wake_sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (wake_receiver == INVALID_SOCKET || wake_sender == INVALID_SOCKET) {
            throw std::runtime_error("Cannot create NetworkReactor wake sockets");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(wake_receiver, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
            0) {
            throw std::runtime_error("Cannot bind NetworkReactor wake socket");
        }
        int address_length = sizeof(address);
        if (getsockname(wake_receiver, reinterpret_cast<sockaddr*>(&address), &address_length) !=
                0 ||
            connect(wake_sender, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
                0) {
            throw std::runtime_error("Cannot connect NetworkReactor wake socket");
        }
        u_long nonblocking = 1;
        if (ioctlsocket(wake_receiver, FIONBIO, &nonblocking) != 0 ||
            ioctlsocket(wake_sender, FIONBIO, &nonblocking) != 0) {
            throw std::runtime_error("Cannot make NetworkReactor wake sockets non-blocking");
        }
    }

    void wake() noexcept {
        if (wake_sender == INVALID_SOCKET)
            return;
        const char byte = 1;
        const auto ignored = send(wake_sender, &byte, 1, 0);
        (void)ignored;
        queue_changed.notify_one();
    }

    void drain_wake_socket() noexcept {
        std::array<char, 128> buffer{};
        while (recv(wake_receiver, buffer.data(), static_cast<int>(buffer.size()), 0) > 0) {}
    }

    void dispatch_completion(Item item, Status status, bool was_cancelled) {
        if (!item.completion)
            return;
        ReactorTaskResult result{item.id, std::move(status), was_cancelled};
        auto completion = std::move(item.completion);
        ui_dispatcher->post(
            [completion = std::move(completion), result = std::move(result)]() mutable {
                completion(std::move(result));
            });
    }

    void run() {
        // 队列中的任务严格串行执行。锁只用于领取任务和更新 active_cancellation，
        // 实际网络操作及完成回调投递均在锁外，避免阻塞其他线程的 cancel/submit。
        {
            std::scoped_lock lock(mutex);
            worker_id = std::this_thread::get_id();
            worker_started = true;
        }
        started.notify_all();

        for (;;) {
            Item item;
            {
                std::unique_lock lock(mutex);
                queue_changed.wait(lock, [this] {
                    return stopping || !queue.empty();
                });
                if (stopping && queue.empty())
                    break;
                item = std::move(queue.front());
                queue.pop_front();
                active_cancellation = item.cancelled;
            }

            const CancellationToken token(item.cancelled);
            Status status;
            bool cancelled = token.is_cancelled();
            if (cancelled) {
                status = {ErrorCode::cancelled, L"Network task was cancelled."};
            } else {
                try {
                    status = item.task(token);
                } catch (const std::exception& error) {
                    std::string_view text(error.what() != nullptr ? error.what() : "");
                    status = {ErrorCode::io_error, std::wstring(text.begin(), text.end())};
                } catch (...) {
                    status = {ErrorCode::io_error, L"Unhandled exception in network reactor task."};
                }
                cancelled = token.is_cancelled();
                if (cancelled && status.is_ok()) {
                    status = {ErrorCode::cancelled, L"Network task was cancelled."};
                }
            }
            {
                std::scoped_lock lock(mutex);
                active_cancellation.reset();
            }
            try {
                dispatch_completion(std::move(item), std::move(status), cancelled);
            } catch (...) {
                // UI Dispatcher 异常不能杀死唯一网络所有者并遗留 SSH 句柄；Dispatcher
                // 本身应在抛出前记录失败。
            }
        }
    }

    ReactorTaskHandle submit(ReactorTask task, ReactorCompletion completion) {
        // 为每个任务分配独立原子取消状态，入队后唤醒 Reactor。返回句柄只持有弱唤醒
        // 回调，因此句柄晚于 Reactor 销毁也不会延长整个网络线程生命周期。
        if (!task)
            throw std::invalid_argument("Network reactor task cannot be empty");
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        ReactorTaskId id = 0;
        {
            std::scoped_lock lock(mutex);
            if (stopping)
                throw std::logic_error("NetworkReactor is shutting down");
            id = next_id++;
            queue.push_back({id, std::move(task), std::move(completion), cancellation});
        }
        const std::weak_ptr<Impl> weak = shared_from_this();
        wake();
        return ReactorTaskHandle(id, std::move(cancellation), [weak] {
            if (const auto self = weak.lock())
                self->wake();
        });
    }

    bool wait_socket(std::uintptr_t native_socket,
                     WaitDirection directions,
                     std::chrono::milliseconds timeout,
                     const CancellationToken& cancellation,
                     bool return_on_notification) {
        if (std::this_thread::get_id() != worker_id || cancellation.is_cancelled())
            return false;
        const auto socket_value = static_cast<SOCKET>(native_socket);
        if (socket_value == INVALID_SOCKET)
            return false;

        const auto deadline =
            std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
        for (;;) {
            if (cancellation.is_cancelled())
                return false;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto wait_ms = static_cast<int>(
                std::min<std::int64_t>(remaining.count(), std::numeric_limits<int>::max()));

            WSAPOLLFD descriptors[2]{};
            descriptors[0].fd = socket_value;
            if (directions == WaitDirection::none || has_direction(directions, WaitDirection::read))
                descriptors[0].events |= POLLRDNORM;
            if (directions == WaitDirection::none ||
                has_direction(directions, WaitDirection::write))
                descriptors[0].events |= POLLWRNORM;
            descriptors[1].fd = wake_receiver;
            descriptors[1].events = POLLRDNORM;

            const auto result = WSAPoll(descriptors, 2, wait_ms);
            if (result == SOCKET_ERROR || result == 0)
                return false;
            if ((descriptors[1].revents & POLLRDNORM) != 0) {
                drain_wake_socket();
                if (cancellation.is_cancelled())
                    return false;
                if (return_on_notification)
                    return false;
            }
            if ((descriptors[0].revents &
                 (POLLRDNORM | POLLWRNORM | POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
            }
        }
    }

    void shutdown() {
        // 先在锁内标记排队和活动任务取消，再通过 wake socket 中断 WSAPoll，最后 join。
        // 禁止从 Reactor 自身任务调用，以免线程等待自己造成死锁。
        {
            std::scoped_lock lock(mutex);
            if (!stopping) {
                stopping = true;
                for (auto& item : queue)
                    item.cancelled->store(true, std::memory_order_release);
                if (active_cancellation)
                    active_cancellation->store(true, std::memory_order_release);
            }
        }
        wake();
        if (worker.joinable()) {
            if (worker.get_id() == std::this_thread::get_id()) {
                throw std::logic_error("NetworkReactor cannot be destroyed from its own task");
            }
            worker.join();
        }
    }

    std::shared_ptr<IUiDispatcher> ui_dispatcher;
    mutable std::mutex mutex;
    std::condition_variable queue_changed;
    std::condition_variable started;
    std::deque<Item> queue;
    std::shared_ptr<std::atomic_bool> active_cancellation;
    std::thread worker;
    std::thread::id worker_id;
    bool worker_started{};
    bool stopping{};
    bool winsock_started{};
    ReactorTaskId next_id{1};
    SOCKET wake_receiver{INVALID_SOCKET};
    SOCKET wake_sender{INVALID_SOCKET};
};

NetworkReactor::NetworkReactor(std::shared_ptr<IUiDispatcher> ui_dispatcher)
    : impl_(std::make_shared<Impl>(std::move(ui_dispatcher))) {
    impl_->start();
}

NetworkReactor::~NetworkReactor() {
    if (!impl_)
        return;
    try {
        impl_->shutdown();
    } catch (...) {
        std::terminate();
    }
}

ReactorTaskHandle NetworkReactor::submit(ReactorTask task, ReactorCompletion completion) {
    return impl_->submit(std::move(task), std::move(completion));
}

void NetworkReactor::shutdown() {
    impl_->shutdown();
}

bool NetworkReactor::is_reactor_thread() const noexcept {
    return std::this_thread::get_id() == impl_->worker_id;
}

std::thread::id NetworkReactor::thread_id() const noexcept {
    return impl_->worker_id;
}

std::size_t NetworkReactor::pending_count() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->queue.size();
}

bool NetworkReactor::wait_socket(std::uintptr_t native_socket,
                                 WaitDirection directions,
                                 std::chrono::milliseconds timeout,
                                 const CancellationToken& cancellation,
                                 bool return_on_notification) {
    return impl_->wait_socket(
        native_socket, directions, timeout, cancellation, return_on_notification);
}

SocketWaitCallback
NetworkReactor::make_socket_waiter(CancellationToken cancellation,
                                   std::function<std::uintptr_t()> socket_provider) {
    if (!socket_provider)
        throw std::invalid_argument("Socket provider cannot be empty");
    const std::weak_ptr<Impl> weak = impl_;
    return [weak,
            cancellation = std::move(cancellation),
            socket_provider = std::move(socket_provider)](WaitDirection directions,
                                                          std::chrono::milliseconds timeout) {
        const auto self = weak.lock();
        return self &&
               self->wait_socket(socket_provider(), directions, timeout, cancellation, false);
    };
}

Status NetworkReactor::bind_session_waiter(const std::shared_ptr<SshSession>& session,
                                           CancellationToken cancellation) {
    if (!session) {
        return {ErrorCode::invalid_argument, L"Cannot bind a null SSH session."};
    }
    if (!is_reactor_thread()) {
        return {ErrorCode::invalid_argument,
                L"SSH waiter must be bound on the network reactor thread."};
    }
    const std::weak_ptr<SshSession> weak = session;
    return session->set_socket_waiter(make_socket_waiter(std::move(cancellation), [weak] {
        const auto locked = weak.lock();
        return locked ? locked->native_socket() : std::numeric_limits<std::uintptr_t>::max();
    }));
}

Status
NetworkReactor::bind_session_waiter_non_interruptible(const std::shared_ptr<SshSession>& session) {
    return bind_session_waiter(session,
                               CancellationToken(std::make_shared<std::atomic_bool>(false)));
}

} // namespace SLink::ssh
