#pragma once

#include <optional>
#include <string>
#include <utility>

namespace SLink {

// 核心层统一错误分类。消息用于面向用户或日志的补充说明，业务判断必须依赖 ErrorCode，
// 不应解析消息文本。
enum class ErrorCode {
    none,
    invalid_argument,
    not_found,
    permission_denied,
    io_error,
    unsupported,
    cancelled,
    conflict,
    authentication_failed,
    host_key_rejected,
    network_error,
    protocol_error,
};

class Status {
public:
    Status() = default;
    Status(ErrorCode code, std::wstring message) : code_(code), message_(std::move(message)) {}

    [[nodiscard]] static Status ok() {
        return {};
    }
    [[nodiscard]] bool is_ok() const noexcept {
        return code_ == ErrorCode::none;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return is_ok();
    }
    [[nodiscard]] ErrorCode code() const noexcept {
        return code_;
    }
    [[nodiscard]] const std::wstring& message() const noexcept {
        return message_;
    }

private:
    ErrorCode code_{ErrorCode::none};
    std::wstring message_;
};

template <typename T> class Result {
public:
    // Result 要么持有 T，要么持有失败 Status；调用 value() 前必须检查 bool/has_value。
    Result(T value) : value_(std::move(value)) {}
    Result(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return value_.has_value();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }
    [[nodiscard]] T& value() & {
        return *value_;
    }
    [[nodiscard]] const T& value() const& {
        return *value_;
    }
    [[nodiscard]] T&& value() && {
        return std::move(*value_);
    }
    [[nodiscard]] const Status& status() const noexcept {
        return status_;
    }

private:
    std::optional<T> value_;
    Status status_;
};

} // namespace SLink
