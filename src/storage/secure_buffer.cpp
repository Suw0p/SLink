// SecureBuffer 的锁页分配、不可优化清零、移动和释放实现。
#include "storage/secure_buffer.h"

#ifndef _WIN32
#error SecureBuffer is a Windows-only implementation
#endif

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <new>
#include <utility>

namespace SLink::storage {

void SecureZero(std::span<std::byte> bytes) noexcept {
    if (!bytes.empty()) {
        SecureZeroMemory(bytes.data(), bytes.size());
    }
}

SecureBuffer::SecureBuffer(std::size_t size) : size_(size) {
    if (size == 0) {
        return;
    }
    data_ = static_cast<std::byte*>(
        VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (data_ == nullptr) {
        size_ = 0;
        throw std::bad_alloc();
    }
    locked_ = VirtualLock(data_, size_) != FALSE;
    if (!locked_) {
        SecureZeroMemory(data_, size_);
        VirtualFree(data_, 0, MEM_RELEASE);
        data_ = nullptr;
        size_ = 0;
        throw std::bad_alloc();
    }
}

SecureBuffer::~SecureBuffer() {
    release();
}

SecureBuffer::SecureBuffer(SecureBuffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      locked_(std::exchange(other.locked_, false)) {}

SecureBuffer& SecureBuffer::operator=(SecureBuffer&& other) noexcept {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        locked_ = std::exchange(other.locked_, false);
    }
    return *this;
}

void SecureBuffer::clear() noexcept {
    SecureZero(bytes());
}

void SecureBuffer::release() noexcept {
    if (data_ == nullptr) {
        return;
    }
    clear();
    if (locked_) {
        VirtualUnlock(data_, size_);
    }
    VirtualFree(data_, 0, MEM_RELEASE);
    data_ = nullptr;
    size_ = 0;
    locked_ = false;
}

} // namespace SLink::storage
