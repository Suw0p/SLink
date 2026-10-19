// 固定 5×4MB 日志轮转实现，文件创建、写入和重命名均限制在 logs 目录内。
#include "storage/rotating_log.h"

#ifndef _WIN32
#error RotatingLog is a Windows-only implementation
#endif

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>

namespace SLink::storage {
namespace {

std::string_view LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

std::string FormatLine(LogLevel level, std::string_view message) {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth
           << '-' << std::setw(2) << now.wDay << 'T' << std::setw(2) << now.wHour << ':'
           << std::setw(2) << now.wMinute << ':' << std::setw(2) << now.wSecond << '.'
           << std::setw(3) << now.wMilliseconds << "Z [" << LevelName(level) << "] " << message
           << '\n';
    return output.str();
}

std::filesystem::path ArchivePath(const std::filesystem::path& active, std::size_t index) {
    auto result = active;
    result += L"." + std::to_wstring(index);
    return result;
}

StorageStatus RotateFiles(const std::filesystem::path& active) {
    std::error_code error;
    const auto oldest = ArchivePath(active, RotatingLog::kTotalFileCount - 1);
    std::filesystem::remove(oldest, error);
    if (error) {
        return StorageStatus::Error(StorageErrc::io_error, L"Cannot remove oldest log archive");
    }
    for (std::size_t index = RotatingLog::kTotalFileCount - 1; index > 1; --index) {
        const auto source = ArchivePath(active, index - 1);
        const auto destination = ArchivePath(active, index);
        if (!std::filesystem::exists(source, error)) {
            if (error) {
                return StorageStatus::Error(StorageErrc::io_error, L"Cannot inspect log archive");
            }
            continue;
        }
        std::filesystem::rename(source, destination, error);
        if (error) {
            return StorageStatus::Error(StorageErrc::io_error, L"Cannot rotate log archive");
        }
    }
    if (std::filesystem::exists(active, error)) {
        if (error) {
            return StorageStatus::Error(StorageErrc::io_error, L"Cannot inspect active log");
        }
        std::filesystem::rename(active, ArchivePath(active, 1), error);
        if (error) {
            return StorageStatus::Error(StorageErrc::io_error, L"Cannot rotate active log");
        }
    }
    return StorageStatus::Ok();
}

} // namespace

struct RotatingLog::Impl {
    std::filesystem::path active;
    std::ofstream stream;
    std::uint64_t bytes{};
    std::mutex mutex;
};

RotatingLog::RotatingLog(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RotatingLog::~RotatingLog() {
    Flush();
}
RotatingLog::RotatingLog(RotatingLog&&) noexcept = default;
RotatingLog& RotatingLog::operator=(RotatingLog&& other) noexcept {
    if (this != &other) {
        Flush();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

StorageResult<std::unique_ptr<RotatingLog>> RotatingLog::Open(const AppPaths& paths,
                                                              std::filesystem::path filename) {
    if (filename.empty() || filename.is_absolute() || filename.has_parent_path() ||
        filename.filename() != filename || filename == L"." || filename == L"..") {
        return StorageResult<std::unique_ptr<RotatingLog>>::Failure(StorageStatus::Error(
            StorageErrc::invalid_argument, L"Log filename must be a single relative filename"));
    }
    auto active = (paths.logs_dir() / filename).lexically_normal();
    if (!paths.ContainsInternalPath(active)) {
        return StorageResult<std::unique_ptr<RotatingLog>>::Failure(StorageStatus::Error(
            StorageErrc::invalid_argument, L"Log path escapes the executable directory"));
    }

    std::error_code error;
    auto size =
        std::filesystem::exists(active, error) ? std::filesystem::file_size(active, error) : 0;
    if (error) {
        return StorageResult<std::unique_ptr<RotatingLog>>::Failure(
            StorageStatus::Error(StorageErrc::io_error, L"Cannot inspect active log"));
    }
    if (size >= kMaximumFileBytes) {
        if (size > kMaximumFileBytes) {
            std::filesystem::resize_file(active, kMaximumFileBytes, error);
            if (error) {
                return StorageResult<std::unique_ptr<RotatingLog>>::Failure(StorageStatus::Error(
                    StorageErrc::io_error, L"Cannot constrain oversized active log"));
            }
        }
        if (auto status = RotateFiles(active); !status) {
            return StorageResult<std::unique_ptr<RotatingLog>>::Failure(std::move(status));
        }
        size = 0;
    }

    auto impl = std::make_unique<Impl>();
    impl->active = std::move(active);
    impl->bytes = size;
    impl->stream.open(impl->active, std::ios::binary | std::ios::app);
    if (!impl->stream) {
        return StorageResult<std::unique_ptr<RotatingLog>>::Failure(
            StorageStatus::Error(StorageErrc::io_error, L"Cannot open active log"));
    }
    return StorageResult<std::unique_ptr<RotatingLog>>::Success(
        std::unique_ptr<RotatingLog>(new RotatingLog(std::move(impl))));
}

StorageStatus RotatingLog::Write(LogLevel level, std::string_view message) {
    if (!impl_) {
        return StorageStatus::Error(StorageErrc::io_error, L"Log is closed");
    }
    auto line = FormatLine(level, message);
    if (line.size() > kMaximumFileBytes) {
        return StorageStatus::Error(StorageErrc::invalid_argument,
                                    L"Single log entry exceeds 4 MB");
    }

    std::scoped_lock lock(impl_->mutex);
    if (impl_->bytes + line.size() > kMaximumFileBytes) {
        impl_->stream.flush();
        impl_->stream.close();
        if (auto status = RotateFiles(impl_->active); !status) {
            return status;
        }
        impl_->stream.open(impl_->active, std::ios::binary | std::ios::trunc);
        if (!impl_->stream) {
            return StorageStatus::Error(StorageErrc::io_error, L"Cannot create rotated active log");
        }
        impl_->bytes = 0;
    }
    impl_->stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (!impl_->stream) {
        return StorageStatus::Error(StorageErrc::io_error, L"Cannot write active log");
    }
    impl_->bytes += line.size();
    return StorageStatus::Ok();
}

StorageStatus RotatingLog::Flush() {
    if (!impl_)
        return StorageStatus::Ok();
    std::scoped_lock lock(impl_->mutex);
    impl_->stream.flush();
    return impl_->stream ? StorageStatus::Ok()
                         : StorageStatus::Error(StorageErrc::io_error, L"Cannot flush active log");
}

const std::filesystem::path& RotatingLog::active_path() const noexcept {
    return impl_->active;
}

} // namespace SLink::storage
