#pragma once

// 开发构建使用的轻量计时器。只输出固定的操作名与耗时，不记录路径、凭据或终端内容。
#ifdef SLINK_ENABLE_PERF_TRACE

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace SLink::core {

inline void RecordPerfSample(const char* label, std::uint64_t microseconds) noexcept {
    struct Window {
        std::array<std::uint64_t, 256> values{};
        std::size_t count{};
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, Window> windows;
    try {
        std::scoped_lock lock(mutex);
        auto& window = windows[label];
        window.values[window.count++] = microseconds;
        if (window.count != window.values.size()) {
            return;
        }
        std::sort(window.values.begin(), window.values.end());
        const std::string message =
            std::string("SLinkPerf ") + label + " p50=" +
            std::to_string(window.values[window.values.size() / 2]) + "us p95=" +
            std::to_string(window.values[window.values.size() * 95 / 100]) + "us\n";
        OutputDebugStringA(message.c_str());
        window.count = 0;
    } catch (...) {
        // 诊断代码不能影响正常运行。
    }
}

class PerfScope final {
public:
    explicit PerfScope(const char* label) noexcept : label_(label) {}
    ~PerfScope() noexcept {
        const auto elapsed = std::chrono::steady_clock::now() - started_;
        RecordPerfSample(label_, static_cast<std::uint64_t>(
                                     std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                                         .count()));
    }

private:
    const char* label_;
    std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
};

} // namespace SLink::core

#else

namespace SLink::core {
class PerfScope final {
public:
    explicit PerfScope(const char*) noexcept {}
};
} // namespace SLink::core

#endif
