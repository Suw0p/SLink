// SFTP Adapter 到统一 IFileEndpoint 的薄封装，集中验证 server-id、路径和连接可用性。
#include "transfer/sftp_endpoint.hpp"

#include <stdexcept>

namespace SLink::transfer {

SftpEndpoint::SftpEndpoint(std::shared_ptr<ISftpAdapter> adapter) : adapter_(std::move(adapter)) {
    if (!adapter_) {
        throw std::invalid_argument("SftpEndpoint requires an injected adapter");
    }
    if (adapter_->server_id().empty()) {
        throw std::invalid_argument("SFTP adapter requires a server id");
    }
}

bool SftpEndpoint::accepts(const FileUri& uri) const noexcept {
    return uri.is_sftp() && uri.authority() == adapter_->server_id();
}

bool SftpEndpoint::available() const noexcept {
    return adapter_->connected();
}

void SftpEndpoint::require_available(const FileUri& uri) const {
    if (!accepts(uri)) {
        throw std::invalid_argument("SftpEndpoint received a URI for another server");
    }
    if (!available()) {
        throw std::runtime_error("SFTP session is not connected");
    }
}

std::vector<DirectoryEntry> SftpEndpoint::list(const FileUri& directory) {
    require_available(directory);
    auto result = adapter_->list(directory.path());
    // Adapters may leave URI construction to this endpoint.
    for (auto& entry : result) {
        entry.uri = directory.joined(entry.name);
    }
    return result;
}

FileStat SftpEndpoint::stat(const FileUri& uri) {
    require_available(uri);
    return adapter_->stat(uri.path());
}

bool SftpEndpoint::exists(const FileUri& uri) {
    require_available(uri);
    return adapter_->exists(uri.path());
}

std::unique_ptr<IReadStream> SftpEndpoint::open_read(const FileUri& uri,
                                                     const std::uint64_t offset) {
    require_available(uri);
    return adapter_->open_read(uri.path(), offset);
}

std::unique_ptr<IWriteStream>
SftpEndpoint::open_write(const FileUri& uri, const std::uint64_t offset, const bool truncate) {
    require_available(uri);
    return adapter_->open_write(uri.path(), offset, truncate);
}

void SftpEndpoint::rename(const FileUri& from, const FileUri& to, const bool replace) {
    require_available(from);
    require_available(to);
    adapter_->rename(from.path(), to.path(), replace);
}

void SftpEndpoint::remove(const FileUri& uri, const bool recursive) {
    require_available(uri);
    adapter_->remove(uri.path(), recursive);
}

void SftpEndpoint::mkdir(const FileUri& uri, const bool recursive) {
    require_available(uri);
    adapter_->mkdir(uri.path(), recursive);
}

void SftpEndpoint::set_permissions(const FileUri& uri, const std::uint32_t permissions) {
    require_available(uri);
    adapter_->set_permissions(uri.path(), permissions);
}

} // namespace SLink::transfer
