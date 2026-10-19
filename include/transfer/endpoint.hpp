#pragma once

#include "transfer/file_uri.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace SLink::transfer {

enum class FileKind {
    Regular,
    Directory,
    Symlink,
    Other,
};

struct FileStat {
    FileKind kind{FileKind::Other};
    std::uint64_t size{};
    std::chrono::system_clock::time_point modified{};
    std::uint32_t permissions{};
    std::string owner;
};

struct DirectoryEntry {
    std::string name;
    FileUri uri;
    FileStat stat;
    bool hidden{};
};

// Windows 依赖文件属性标记隐藏项，Unix/SFTP 通常以点号名称表达隐藏项。文件浏览器
// 统一识别两种形式，使同一个“显示隐藏文件”开关在本地和远程端行为一致。
[[nodiscard]] inline bool IsHiddenDirectoryEntry(const DirectoryEntry& entry) noexcept {
    return entry.hidden || (!entry.name.empty() && entry.name.front() == '.');
}

// 统一的顺序读取流。实现可来自本地文件或 SFTP，但对象只能在其所属端点允许的
// 线程中使用；尤其不能把远程流交给 UI 线程直接读写。
class IReadStream {
public:
    virtual ~IReadStream() = default;
    [[nodiscard]] virtual std::size_t read(std::span<std::byte> output) = 0;
};

class IWriteStream {
public:
    virtual ~IWriteStream() = default;
    // 远程流通过此标记在协议包安全边界停止提交数据，调用方无需为了快速取消而
    // 人工拆小应用层缓冲区。
    virtual void set_cancellation_flag(std::shared_ptr<const std::atomic_bool>) {}
    virtual void write(std::span<const std::byte> input) = 0;
    virtual void flush() = 0;
};

// 本地文件系统与 SFTP 的统一端点接口。上层传输、浏览和编辑逻辑只依赖 FileUri，
// 不根据平台或协议分叉。远程实现通常具有 Reactor 线程亲和性，调用者必须遵守
// 创建端点的控制器所规定的线程/租约边界。
class IFileEndpoint {
public:
    virtual ~IFileEndpoint() = default;

    [[nodiscard]] virtual bool accepts(const FileUri& uri) const noexcept = 0;
    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual std::vector<DirectoryEntry> list(const FileUri& directory) = 0;
    [[nodiscard]] virtual FileStat stat(const FileUri& uri) = 0;
    [[nodiscard]] virtual bool exists(const FileUri& uri) = 0;
    [[nodiscard]] virtual std::unique_ptr<IReadStream> open_read(const FileUri& uri,
                                                                 std::uint64_t offset = 0) = 0;
    [[nodiscard]] virtual std::unique_ptr<IWriteStream>
    open_write(const FileUri& uri, std::uint64_t offset = 0, bool truncate = true) = 0;
    virtual void rename(const FileUri& from, const FileUri& to, bool replace) = 0;
    virtual void remove(const FileUri& uri, bool recursive) = 0;
    virtual void mkdir(const FileUri& uri, bool recursive) = 0;
    virtual void set_permissions(const FileUri& uri, std::uint32_t permissions) = 0;
};

} // namespace SLink::transfer
