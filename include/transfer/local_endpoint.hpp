#pragma once

#include "transfer/endpoint.hpp"

namespace SLink::transfer {

// Windows 本地文件系统端点。负责长路径、Unicode、隐藏属性、快捷方式之外的基础文件
// 操作；UI 的 .lnk 展示和“此电脑”虚拟入口由上层处理，不混入传输协议。
class LocalEndpoint final : public IFileEndpoint {
public:
    [[nodiscard]] bool accepts(const FileUri& uri) const noexcept override;
    [[nodiscard]] bool available() const noexcept override {
        return true;
    }
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
};

} // namespace SLink::transfer
