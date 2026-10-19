#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace SLink {

// 跨模块使用的轻量领域模型。此文件不包含 Win32、libssh2 或数据库实现细节，适合
// 在 UI、应用协调层之间传递不可变快照。敏感凭据只通过 CredentialRef 间接引用。
using ServerId = std::uint64_t;
using SshSessionId = std::uint64_t;
using TransferPaneId = std::uint64_t;
using TransferId = std::uint64_t;

enum class AuthenticationKind {
    password,
    private_key,
    open_ssh_agent,
    keyboard_interactive,
};

struct CredentialRef {
    std::string id;
};

struct ServerProfile {
    ServerId id{};
    std::wstring name;
    std::wstring host;
    std::uint16_t port{22};
    std::wstring username;
    std::wstring initial_directory;
    AuthenticationKind authentication{AuthenticationKind::password};
    std::optional<CredentialRef> credential;
    std::wstring group;
};

struct RemoteDocument {
    std::string id;
    ServerId server_id{};
    std::string remote_path;
    std::filesystem::path cache_path;
    std::uint64_t remote_size{};
    std::int64_t remote_modified_unix{};
    std::string remote_hash;
    bool dirty{false};
    bool read_only{false};
};

} // namespace SLink
