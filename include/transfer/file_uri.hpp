#pragma once

#include <compare>
#include <string>
#include <string_view>

namespace SLink::transfer {

// 跨本地文件系统和 SFTP 使用的规范资源标识。
// local:///D:/path 的 authority 为空；sftp://<server-id>/path 的 authority 是稳定
// 服务器配置 id，而不是可能变化的显示名称或主机地址。路径统一使用正斜杠序列化。
enum class FileScheme {
    Local,
    Sftp,
};

class FileUri {
public:
    FileUri() = default;
    FileUri(FileScheme scheme, std::string authority, std::string path);

    [[nodiscard]] static FileUri parse(std::string_view value);
    [[nodiscard]] static FileUri local(std::string_view path);
    [[nodiscard]] static FileUri sftp(std::string_view server_id, std::string_view path);

    [[nodiscard]] FileScheme scheme() const noexcept {
        return scheme_;
    }
    [[nodiscard]] const std::string& authority() const noexcept {
        return authority_;
    }
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }
    [[nodiscard]] bool is_local() const noexcept {
        return scheme_ == FileScheme::Local;
    }
    [[nodiscard]] bool is_sftp() const noexcept {
        return scheme_ == FileScheme::Sftp;
    }

    [[nodiscard]] FileUri parent() const;
    [[nodiscard]] FileUri joined(std::string_view child) const;
    [[nodiscard]] FileUri with_path(std::string_view path) const;
    [[nodiscard]] std::string filename() const;
    [[nodiscard]] std::string to_string() const;

    auto operator<=>(const FileUri&) const = default;

private:
    FileScheme scheme_{FileScheme::Local};
    std::string authority_;
    std::string path_{"/"};
};

} // namespace SLink::transfer
