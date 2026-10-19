#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>

namespace SLink::app {

// 最多四个后台工作线程；同一服务器按入队到期顺序串行，不同服务器可并行。
// "control" 是缓存恢复专用的排他队列，避免恢复扫描与缓存写入并发。
class EditorWorkScheduler final {
public:
    using ErrorHandler = std::function<void(std::exception_ptr)>;

    explicit EditorWorkScheduler(std::size_t maximum_workers, ErrorHandler on_error);
    ~EditorWorkScheduler();
    EditorWorkScheduler(const EditorWorkScheduler&) = delete;
    EditorWorkScheduler& operator=(const EditorWorkScheduler&) = delete;

    void
    Enqueue(std::string lane, std::function<void()> action, std::chrono::milliseconds delay = {});
    void Stop();
    [[nodiscard]] std::size_t worker_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::app
