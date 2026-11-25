#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace SLink::ui::detail {

// 提交、取消和异常都通过最后一个 ticket 的析构归还计数，不能依赖任务手工调用 End。
class BackgroundActivity final {
    struct State {
        std::mutex mutex;
        std::condition_variable idle;
        std::size_t count{};
    };
    struct Ticket {
        std::shared_ptr<State> state;
        bool armed{};
        ~Ticket() {
            if (!armed)
                return;
            std::scoped_lock lock(state->mutex);
            if (--state->count == 0)
                state->idle.notify_all();
        }
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();

public:
    [[nodiscard]] std::shared_ptr<void> Track() {
        auto ticket = std::make_shared<Ticket>();
        ticket->state = state_;
        std::scoped_lock lock(state_->mutex);
        ++state_->count;
        ticket->armed = true;
        return ticket;
    }
    void WaitForIdle() const {
        std::unique_lock lock(state_->mutex);
        state_->idle.wait(lock, [&] {
            return state_->count == 0;
        });
    }
};

// UI 页面共享的小型工作池，用于本地目录枚举、图标解析等短任务。析构时停止接收任务
// 并清空尚未开始的队列。远程 libssh2 操作不得提交到这里。
class BackgroundExecutor final {
public:
    using ThreadFactory = std::function<std::jthread(std::function<void(std::stop_token)>)>;

    explicit BackgroundExecutor(const std::size_t worker_count = 2, ThreadFactory factory = {}) {
        const std::size_t count = std::max<std::size_t>(1, worker_count);
        workers_.reserve(count);
        try {
            for (std::size_t index = 0; index < count; ++index) {
                auto action = [this](const std::stop_token stop) {
                    Run(stop);
                };
                workers_.push_back(factory ? factory(action) : std::jthread(action));
            }
        } catch (...) {
            // 构造失败不会进入类析构；已启动的线程也必须先唤醒再 join。
            Stop();
            throw;
        }
    }

    ~BackgroundExecutor() {
        Stop();
    }

    void Stop() {
        std::deque<std::function<void()>> cancelled;
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
            cancelled.swap(tasks_);
        }
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        changed_.notify_all();
        // 先归还未开始任务的计数，再等待仍在执行的线程，不能倒过来互相等待。
        cancelled.clear();
        for (auto& worker : workers_)
            if (worker.joinable())
                worker.join();
        // 任务捕获的端点/RAII 守卫可能阻塞或获取其他锁，在队列锁外析构。
    }

    BackgroundExecutor(const BackgroundExecutor&) = delete;
    BackgroundExecutor& operator=(const BackgroundExecutor&) = delete;

    bool Submit(std::function<void()> task) {
        if (!task) {
            return false;
        }
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) {
                return false;
            }
            tasks_.push_back(std::move(task));
        }
        changed_.notify_one();
        return true;
    }

private:
    void Run(const std::stop_token stop) noexcept {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [this, stop] {
                    return stop.stop_requested() || stopping_ || !tasks_.empty();
                });
                if ((stop.stop_requested() || stopping_) && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try {
                task();
            } catch (...) {
                // 任务应自行上报错误；此处只作为最后边界，防止共享工作线程退出。
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::jthread> workers_;
    bool stopping_{};
};

} // namespace SLink::ui::detail
