#pragma once

#include <windows.h>
#include "core/status.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace SLink::app {

// UI 回答与网络取消之间的单次交接。UI 没有处理消息或已经退出时，等待者
// 仍能独立结束；迟到的回答不会复活已取消认证，也不会保留明文密码。
class InteractiveAuthRequest final {
public:
    using Responses = std::vector<std::string>;
    using Cancelled = std::function<bool()>;

    ~InteractiveAuthRequest() {
        if (response_)
            Wipe(*response_);
    }

    void Complete(Result<Responses> response) {
        std::scoped_lock lock(mutex_);
        if (abandoned_ || response_) {
            Wipe(response);
            return;
        }
        response_.emplace(std::move(response));
        changed_.notify_all();
    }

    Result<Responses> Wait(const Cancelled& cancelled,
                           std::chrono::milliseconds timeout = std::chrono::minutes(2)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock lock(mutex_);
        for (;;) {
            if ((cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline) {
                abandoned_.store(true, std::memory_order_release);
                if (response_) {
                    Wipe(*response_);
                    response_.reset();
                }
                return Status{ErrorCode::cancelled, L"交互认证已取消或等待超时。"};
            }
            if (response_)
                return std::move(*response_);
            changed_.wait_for(lock, std::chrono::milliseconds(20));
        }
    }

    bool abandoned() const noexcept {
        return abandoned_.load(std::memory_order_acquire);
    }

private:
    static void Wipe(Result<Responses>& response) {
        if (response)
            for (auto& text : response.value())
                if (!text.empty())
                    SecureZeroMemory(text.data(), text.size());
    }
    std::atomic_bool abandoned_{};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Result<Responses>> response_;
};

} // namespace SLink::app
