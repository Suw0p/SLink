#include "app/editor_work_scheduler.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SLink::app {

struct EditorWorkScheduler::Impl {
    struct WorkItem {
        std::chrono::steady_clock::time_point due;
        std::uint64_t sequence{};
        std::string lane;
        std::string key;
        std::function<void()> action;
    };

    ErrorHandler on_error;
    std::vector<std::jthread> workers;
    std::mutex mutex;
    std::condition_variable ready;
    std::list<WorkItem> work;
    std::unordered_map<std::string, std::list<WorkItem>::iterator> latest;
    std::unordered_set<std::string> busy_lanes;
    std::uint64_t next_sequence{};
    bool stopping{};

    explicit Impl(std::size_t maximum_workers, ErrorHandler handler)
        : on_error(std::move(handler)) {
        if (maximum_workers == 0) {
            throw std::invalid_argument("Editor scheduler requires a worker");
        }
        workers.reserve(maximum_workers);
        for (std::size_t index = 0; index < maximum_workers; ++index) {
            try {
                workers.emplace_back([this](std::stop_token stop) {
                    WorkerLoop(stop);
                });
            } catch (const std::system_error&) {
                if (workers.empty()) {
                    throw;
                }
                break;
            }
        }
    }

    bool LaneAvailable(const std::string& lane) const {
        return !busy_lanes.contains(lane) && (lane != "control" || busy_lanes.empty()) &&
               (lane == "control" || !busy_lanes.contains("control"));
    }

    void WorkerLoop(std::stop_token stop) {
        for (;;) {
            std::function<void()> action;
            std::string lane;
            {
                std::unique_lock lock(mutex);
                for (;;) {
                    if (stop.stop_requested() || stopping) {
                        return;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    auto runnable = work.end();
                    std::optional<std::chrono::steady_clock::time_point> next_due;
                    for (auto item = work.begin(); item != work.end(); ++item) {
                        if (!LaneAvailable(item->lane)) {
                            continue;
                        }
                        if (item->due > now) {
                            if (!next_due || item->due < *next_due) {
                                next_due = item->due;
                            }
                            continue;
                        }
                        if (runnable == work.end() || item->due < runnable->due ||
                            (item->due == runnable->due && item->sequence < runnable->sequence)) {
                            runnable = item;
                        }
                    }
                    if (runnable == work.end()) {
                        if (next_due) {
                            ready.wait_until(lock, *next_due);
                        } else {
                            ready.wait(lock);
                        }
                        continue;
                    }
                    lane = std::move(runnable->lane);
                    action = std::move(runnable->action);
                    if (!runnable->key.empty())
                        latest.erase(runnable->key);
                    work.erase(runnable);
                    busy_lanes.insert(lane);
                    break;
                }
            }
            try {
                action();
            } catch (...) {
                try {
                    if (on_error) {
                        on_error(std::current_exception());
                    }
                } catch (...) {
                    // 错误汇报本身不得终止工作线程。
                }
            }
            {
                std::scoped_lock lock(mutex);
                busy_lanes.erase(lane);
            }
            ready.notify_all();
        }
    }

    void Stop() {
        std::list<WorkItem> cancelled;
        {
            std::scoped_lock lock(mutex);
            if (stopping) {
                return;
            }
            stopping = true;
            work.swap(cancelled);
            latest.clear();
        }
        for (auto& worker : workers) {
            worker.request_stop();
        }
        ready.notify_all();
        cancelled.clear(); // 捕获的快照/端点在锁外释放。
        for (auto& worker : workers) {
            worker.join();
        }
    }

    void Enqueue(std::string lane,
                 std::string key,
                 std::function<void()> action,
                 std::chrono::milliseconds delay) {
        if (!action)
            return;
        std::function<void()> replaced;
        {
            std::scoped_lock lock(mutex);
            if (stopping)
                return;
            const auto due = std::chrono::steady_clock::now() + delay;
            const auto found = key.empty() ? latest.end() : latest.find(key);
            if (found != latest.end()) {
                auto& item = *found->second;
                replaced = std::move(item.action);
                item.action = std::move(action);
                item.lane = std::move(lane);
                item.due = due;
                item.sequence = next_sequence++;
            } else {
                auto item = work.emplace(work.end(),
                                         due,
                                         next_sequence++,
                                         std::move(lane),
                                         std::move(key),
                                         std::move(action));
                if (!item->key.empty()) {
                    try {
                        latest.emplace(item->key, item);
                    } catch (...) {
                        replaced = std::move(item->action);
                        work.erase(item);
                        throw;
                    }
                }
            }
        }
        ready.notify_all();
    }
};

EditorWorkScheduler::EditorWorkScheduler(std::size_t maximum_workers, ErrorHandler on_error)
    : impl_(std::make_unique<Impl>(maximum_workers, std::move(on_error))) {}

EditorWorkScheduler::~EditorWorkScheduler() {
    impl_->Stop();
}

void EditorWorkScheduler::Enqueue(std::string lane,
                                  std::function<void()> action,
                                  std::chrono::milliseconds delay) {
    impl_->Enqueue(std::move(lane), {}, std::move(action), delay);
}

void EditorWorkScheduler::EnqueueLatest(std::string lane,
                                        std::string key,
                                        std::function<void()> action,
                                        std::chrono::milliseconds delay) {
    if (key.empty())
        throw std::invalid_argument("Coalesced editor task requires a key");
    impl_->Enqueue(std::move(lane), std::move(key), std::move(action), delay);
}

void EditorWorkScheduler::CancelLatest(const std::string& key) {
    std::function<void()> cancelled;
    {
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->latest.find(key);
        if (found == impl_->latest.end())
            return;
        cancelled = std::move(found->second->action);
        impl_->work.erase(found->second);
        impl_->latest.erase(found);
    }
    impl_->ready.notify_all();
}

std::size_t EditorWorkScheduler::pending_count() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->work.size();
}

void EditorWorkScheduler::Stop() {
    impl_->Stop();
}

std::size_t EditorWorkScheduler::worker_count() const noexcept {
    return impl_->workers.size();
}

} // namespace SLink::app
