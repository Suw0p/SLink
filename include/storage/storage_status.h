#pragma once

#include <optional>
#include <string>
#include <utility>

namespace SLink::storage {

// 存储层独立错误分类，避免 SQLite、Win32、libsodium 等后端错误码泄漏到上层。
enum class StorageErrc {
    ok = 0,
    invalid_argument,
    not_found,
    io_error,
    corrupt_data,
    unsupported,
    locked,
    authentication_failed,
    already_exists,
    backend_error,
};

struct StorageStatus {
    StorageErrc code{StorageErrc::ok};
    std::wstring message;

    [[nodiscard]] bool ok() const noexcept {
        return code == StorageErrc::ok;
    }
    explicit operator bool() const noexcept {
        return ok();
    }

    static StorageStatus Ok() {
        return {};
    }
    static StorageStatus Error(StorageErrc error, std::wstring detail) {
        return {error, std::move(detail)};
    }
};

template <typename T> class StorageResult {
public:
    // 成功时同时持有值和 Ok 状态；失败时只持有状态。读取 value 前必须检查 ok/bool。
    static StorageResult Success(T value) {
        return StorageResult(std::move(value), StorageStatus::Ok());
    }

    static StorageResult Failure(StorageStatus status) {
        return StorageResult(std::nullopt, std::move(status));
    }

    [[nodiscard]] bool ok() const noexcept {
        return value_.has_value() && status_.ok();
    }
    explicit operator bool() const noexcept {
        return ok();
    }
    [[nodiscard]] const StorageStatus& status() const noexcept {
        return status_;
    }

    T& value() & {
        return value_.value();
    }
    const T& value() const& {
        return value_.value();
    }
    T&& value() && {
        return std::move(value_).value();
    }

private:
    StorageResult(T value, StorageStatus status)
        : value_(std::move(value)), status_(std::move(status)) {}
    StorageResult(std::nullopt_t, StorageStatus status)
        : value_(std::nullopt), status_(std::move(status)) {}

    std::optional<T> value_;
    StorageStatus status_;
};

} // namespace SLink::storage
