#pragma once

#include <cstddef>
#include <span>

namespace SLink::storage {

// 仅可移动的敏感数据缓冲区。内存按页分配并尝试锁定，Windows 无法锁页时构造失败；
// 释放、移动赋值和 clear 前都使用不可优化掉的方式清零原内容。
class SecureBuffer {
public:
    SecureBuffer() noexcept = default;
    explicit SecureBuffer(std::size_t size);
    ~SecureBuffer();

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    SecureBuffer(SecureBuffer&& other) noexcept;
    SecureBuffer& operator=(SecureBuffer&& other) noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::span<std::byte> bytes() noexcept {
        return {data_, size_};
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {data_, size_};
    }
    void clear() noexcept;

private:
    void release() noexcept;

    std::byte* data_{nullptr};
    std::size_t size_{0};
    bool locked_{false};
};

void SecureZero(std::span<std::byte> bytes) noexcept;

} // namespace SLink::storage
