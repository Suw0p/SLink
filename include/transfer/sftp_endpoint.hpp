#pragma once

#include "transfer/endpoint.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace SLink::transfer {

// 由 SSH/网络层实现。Adapter 持有实际 SFTP 会话并执行全部 libssh2 调用；
// SftpEndpoint 只负责 FileUri 校验和协议无关适配，不隐式创建连接。
class ISftpAdapter {
public:
    virtual ~ISftpAdapter() = default;

    [[nodiscard]] virtual std::string_view server_id() const noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;
    [[nodiscard]] virtual std::string realpath(std::string_view path) = 0;
    [[nodiscard]] virtual std::vector<DirectoryEntry> list(std::string_view path) = 0;
    [[nodiscard]] virtual FileStat stat(std::string_view path) = 0;
    [[nodiscard]] virtual bool exists(std::string_view path) = 0;
    [[nodiscard]] virtual std::unique_ptr<IReadStream> open_read(std::string_view path,
                                                                 std::uint64_t offset) = 0;
    [[nodiscard]] virtual std::unique_ptr<IWriteStream>
    open_write(std::string_view path, std::uint64_t offset, bool truncate) = 0;
    virtual void rename(std::string_view from, std::string_view to, bool replace) = 0;
    virtual void remove(std::string_view path, bool recursive) = 0;
    virtual void mkdir(std::string_view path, bool recursive) = 0;
    virtual void set_permissions(std::string_view path, std::uint32_t permissions) = 0;
};

class SftpEndpoint final : public IFileEndpoint {
public:
    // adapter 及端点都继承所属 SshSession 的单线程亲和性。
    explicit SftpEndpoint(std::shared_ptr<ISftpAdapter> adapter);

    [[nodiscard]] bool accepts(const FileUri& uri) const noexcept override;
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::vector<DirectoryEntry> list(const FileUri& directory) override;
    [[nodiscard]] FileStat stat(const FileUri& uri) override;
    [[nodiscard]] bool exists(const FileUri& uri) override;
    [[nodiscard]] std::unique_ptr<IReadStream> open_read(const FileUri& uri,
                                                         std::uint64_t offset = 0) override;
    [[nodiscard]] std::unique_ptr<IWriteStream>
    open_write(const FileUri& uri, std::uint64_t offset = 0, bool truncate = true) override;
    void rename(const FileUri& from, const FileUri& to, bool replace) override;
    void remove(const FileUri& uri, bool recursive) override;
    void mkdir(const FileUri& uri, bool recursive) override;
    void set_permissions(const FileUri& uri, std::uint32_t permissions) override;

private:
    void require_available(const FileUri& uri) const;
    std::shared_ptr<ISftpAdapter> adapter_;
};

} // namespace SLink::transfer
