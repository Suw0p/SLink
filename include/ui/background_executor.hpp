#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace SLink::ui::detail {

// UI 页面共享的小型工作池，用于本地目录枚举、图标解析等短任务。析构时停止接收任务
// 并清空尚未开始的队列。远程 libssh2 操作不得提交到这里。
class BackgroundExecutor final {
public:
    explicit BackgroundExecutor(const std::size_t worker_count = 2) {
        const std::size_t count = std::max<std::size_t>(1, worker_count);
        workers_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            workers_.emplace_back([this](const std::stop_token stop) {
                Run(stop);
            });
        }
    }

    ~BackgroundExecutor() {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
            tasks_.clear();
        }
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        changed_.notify_all();
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
