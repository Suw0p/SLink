#pragma once

#include "core/status.hpp"
#include "ssh/session.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace SLink::ssh {

using ReactorTaskId = std::uint64_t;

class IUiDispatcher {
public:
    virtual ~IUiDispatcher() = default;
    // 实现通常通过 PostMessage 把回调交给 UI 线程；NetworkReactor 不会内联执行完成回调。
    virtual void post(std::function<void()> callback) = 0;
};

class FunctionUiDispatcher final : public IUiDispatcher {
public:
    explicit FunctionUiDispatcher(std::function<void(std::function<void()>)> post_function)
        : post_function_(std::move(post_function)) {}

    void post(std::function<void()> callback) override {
        post_function_(std::move(callback));
    }

private:
    std::function<void(std::function<void()>)> post_function_;
};

class CancellationToken {
public:
    [[nodiscard]] bool is_cancelled() const noexcept {
        return !state_ || state_->load(std::memory_order_acquire);
    }

private:
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state)
        : state_(std::move(state)) {}
    std::shared_ptr<std::atomic_bool> state_;
    friend class NetworkReactor;
};

class ReactorTaskHandle {
public:
    ReactorTaskHandle() = default;
    [[nodiscard]] ReactorTaskId id() const noexcept {
        return id_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return id_ != 0 && state_ != nullptr;
    }
    [[nodiscard]] bool cancelled() const noexcept {
        return state_ && state_->load(std::memory_order_acquire);
    }
    // 唤醒当前 socket 等待但不取消任务，用于终端按键、PTY resize 等带外事件。
    void notify() const;
    void cancel() const;

private:
    ReactorTaskHandle(ReactorTaskId id,
                      std::shared_ptr<std::atomic_bool> state,
                      std::function<void()> wake)
        : id_(id), state_(std::move(state)), wake_(std::move(wake)) {}

    ReactorTaskId id_{};
    std::shared_ptr<std::atomic_bool> state_;
    std::function<void()> wake_;
    friend class NetworkReactor;
};

struct ReactorTaskResult {
    ReactorTaskId id{};
    Status status;
    bool cancelled{};
};

using ReactorTask = std::function<Status(const CancellationToken&)>;
using ReactorCompletion = std::function<void(ReactorTaskResult)>;

// 单连接网络事件循环。提交到本 Reactor 的任务拥有其中全部 SshSession 对象，任何
// 任务都不得把带 libssh2 状态的活对象交给 UI。UI 只能接收经 Dispatcher 投递的快照。
// 每次只有一个任务访问会话，取消通过唤醒 socket 等待实现，不并发调用 libssh2。
class NetworkReactor final {
public:
    explicit NetworkReactor(std::shared_ptr<IUiDispatcher> ui_dispatcher);
    ~NetworkReactor();
    NetworkReactor(const NetworkReactor&) = delete;
    NetworkReactor& operator=(const NetworkReactor&) = delete;

    [[nodiscard]] ReactorTaskHandle submit(ReactorTask task, ReactorCompletion completion = {});
    void shutdown();

    [[nodiscard]] bool is_reactor_thread() const noexcept;
    [[nodiscard]] std::thread::id thread_id() const noexcept;
    [[nodiscard]] std::size_t pending_count() const noexcept;

    // 仅允许活动 Reactor 任务调用；同时轮询目标 socket 和内部唤醒 socket，因此取消
    // 或带外通知可以中断等待。
    [[nodiscard]] bool wait_socket(std::uintptr_t native_socket,
                                   WaitDirection directions,
                                   std::chrono::milliseconds timeout,
                                   const CancellationToken& cancellation,
                                   bool return_on_notification = false);

    // SshSession 建连的桥接器；每次 libssh2 返回 EAGAIN 后都在 Reactor 线程重新取 socket。
    [[nodiscard]] SocketWaitCallback
    make_socket_waiter(CancellationToken cancellation,
                       std::function<std::uintptr_t()> socket_provider);

    // 为当前任务安装可取消等待器。执行阻塞式封装的认证/SFTP 调用前必须先绑定。
    [[nodiscard]] Status bind_session_waiter(const std::shared_ptr<SshSession>& session,
                                             CancellationToken cancellation);

    // libssh2 的非阻塞 SFTP 调用会跨 EAGAIN 保存协议状态。操作中途打断会让共享通道
    // 停在半包状态并破坏下一次调用，因此该等待器允许当前协议操作收尾；任务随后仍会
    // 观察取消标记并丢弃结果。
    [[nodiscard]] Status
    bind_session_waiter_non_interruptible(const std::shared_ptr<SshSession>& session);

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace SLink::ssh
