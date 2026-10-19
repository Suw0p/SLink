#pragma once

#include "storage/app_paths.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace SLink::storage {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

// 便携滚动日志。单文件最多 4 MB，总计保留 5 个文件；轮转和 Flush 由内部互斥保护，
// 日志内容不得包含密码、私钥、口令或解密后的凭据数据。
class RotatingLog {
public:
    static constexpr std::uint64_t kMaximumFileBytes = 4ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kTotalFileCount = 5;

    static StorageResult<std::unique_ptr<RotatingLog>>
    Open(const AppPaths& paths, std::filesystem::path filename = L"SLink.log");

    ~RotatingLog();
    RotatingLog(RotatingLog&&) noexcept;
    RotatingLog& operator=(RotatingLog&&) noexcept;
    RotatingLog(const RotatingLog&) = delete;
    RotatingLog& operator=(const RotatingLog&) = delete;

    StorageStatus Write(LogLevel level, std::string_view message);
    StorageStatus Flush();
    [[nodiscard]] const std::filesystem::path& active_path() const noexcept;

private:
    struct Impl;
    explicit RotatingLog(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::storage
