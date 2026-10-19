#pragma once

#include "storage/state_store.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SLink::storage {

enum class AuthenticationKind : std::uint8_t {
    Password,
    PrivateKey,
    OpenSshAgent,
    KeyboardInteractive,
};

struct ServerGroup {
    std::string id;
    std::string name;
    std::int32_t sort_order{};
    auto operator<=>(const ServerGroup&) const = default;
};

struct ServerProfile {
    std::string id;
    std::optional<std::string> group_id;
    std::string name;
    std::string host;
    std::uint16_t port{22};
    std::string username;
    // 留空时自动使用远端登录主目录；相对路径和 ~/... 均以 SFTP 返回的规范主目录解析。
    std::string initial_directory;
    AuthenticationKind authentication{AuthenticationKind::Password};
    // 指向 Vault 的不透明引用；本结构及 state.db 中绝不出现密码、私钥或私钥口令。
    std::optional<std::string> credential_ref;
    auto operator<=>(const ServerProfile&) const = default;
};

struct EditorRecoveryState {
    std::string document_id;
    std::string remote_uri;
    // 始终相对于 cache/editor；绝对路径及包含 ".." 的路径会被拒绝。
    std::string content_relative_path;
    std::string metadata_relative_path;
    bool dirty{true};
    auto operator<=>(const EditorRecoveryState&) const = default;
};

enum class TransferRecoveryState : std::uint8_t {
    Queued,
    Paused,
    Failed,
};

enum class TransferRecoveryConflictPolicy : std::uint8_t {
    Overwrite,
    Skip,
    AutoRename,
    Resume,
};

struct TransferRecoveryRecord {
    std::uint64_t id{};
    std::string source_uri;
    std::string target_uri;
    std::string effective_target_uri;
    std::uint64_t bytes_transferred{};
    std::uint64_t bytes_total{};
    TransferRecoveryState state{TransferRecoveryState::Queued};
    TransferRecoveryConflictPolicy conflict{TransferRecoveryConflictPolicy::Overwrite};
    bool move{};
    bool verify_sha256{};
    std::string error;
    auto operator<=>(const TransferRecoveryRecord&) const = default;
};

struct PortForwardRecord {
    std::uint16_t local_port{};
    std::string bind_address;
    std::string target_address;
    std::uint16_t target_port{};
    bool proxy_enabled{};
    bool firewall_enabled{};
    auto operator<=>(const PortForwardRecord&) const = default;
};

struct FirewallRuleRecord {
    std::string session_id;
    std::uint16_t local_port{};
    std::string display_name;
    auto operator<=>(const FirewallRuleRecord&) const = default;
};

// 非敏感应用状态的强类型持久化边界。
//
// 这里只保存服务器资料、设置、父传输任务恢复信息及端口转发配置；敏感凭据只能通过
// Vault 保存。远程文件面板属于会话状态，数据库刻意不提供对应表或 API，重启后必须
// 由用户重新打开。当前仍处于开发阶段，不保留旧版本 schema 迁移逻辑。
class ApplicationRepository {
public:
    static constexpr std::uint32_t kCurrentSchemaVersion = 1;

    static StorageResult<std::unique_ptr<ApplicationRepository>> Open(StateStore& store);

    StorageStatus UpsertServerGroup(const ServerGroup& group);
    StorageResult<std::optional<ServerGroup>> FindServerGroup(const std::string& id) const;
    StorageResult<std::vector<ServerGroup>> ListServerGroups() const;
    StorageStatus RemoveServerGroup(const std::string& id);
    StorageStatus RemoveServerGroupAndUngroup(const std::string& id);

    StorageStatus UpsertServerProfile(const ServerProfile& profile);
    StorageResult<std::optional<ServerProfile>> FindServerProfile(const std::string& id) const;
    StorageResult<std::vector<ServerProfile>> ListServerProfiles() const;
    StorageStatus RemoveServerProfile(const std::string& id);

    StorageStatus SetSetting(std::string key, std::string value);
    StorageResult<std::optional<std::string>> GetSetting(const std::string& key) const;
    StorageStatus RemoveSetting(const std::string& key);

    StorageStatus UpsertEditorRecovery(const EditorRecoveryState& state);
    StorageResult<std::vector<EditorRecoveryState>> ListEditorRecovery() const;
    StorageStatus RemoveEditorRecovery(const std::string& document_id);

    StorageStatus UpsertTransferRecovery(const TransferRecoveryRecord& record);
    StorageResult<std::vector<TransferRecoveryRecord>> ListTransferRecovery() const;
    StorageStatus RemoveTransferRecovery(std::uint64_t id);

    StorageStatus UpsertPortForward(const PortForwardRecord& record);
    StorageResult<std::vector<PortForwardRecord>> ListPortForwards() const;
    StorageStatus RemovePortForward(const std::string& bind_address, std::uint16_t local_port);

    StorageStatus UpsertFirewallRule(const FirewallRuleRecord& record);
    StorageResult<std::vector<FirewallRuleRecord>> ListFirewallRules() const;
    StorageStatus RemoveFirewallRule(const std::string& session_id, std::uint16_t local_port);

    StorageStatus Flush();

private:
    explicit ApplicationRepository(StateStore& store) : store_(store) {}
    StateStore& store_;
};

} // namespace SLink::storage
