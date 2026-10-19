#pragma once

#include "storage/application_repository.h"
#include "storage/secure_buffer.h"
#include "storage/storage_status.h"
#include "storage/vault.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace SLink::app {

// 单条 Vault 密文内部使用的稳定明文容器类型；编码后的容器也只能写入 Vault，
// 禁止落入 state.db、日志或 UI 状态。
enum class StoredCredentialKind : std::uint8_t {
    Password = 1,
    PrivateKey = 2,
};

struct StoredCredentialView {
    StoredCredentialKind kind{StoredCredentialKind::Password};
    std::span<const std::byte> password;
    std::span<const std::byte> private_key;
    std::span<const std::byte> private_key_passphrase;
};

struct DecodedStoredCredential {
    StoredCredentialKind kind{StoredCredentialKind::Password};
    storage::SecureBuffer password;
    storage::SecureBuffer private_key;
    storage::SecureBuffer private_key_passphrase;
};

// v1 格式：magic[8]、小端 uint16 版本、uint8 类型、uint8 字段数，之后是若干
// (uint8 tag, 小端 uint32 长度, bytes) 字段。未知版本/标签、重复字段及越界长度均拒绝。
storage::StorageResult<storage::SecureBuffer>
EncodeStoredCredential(const StoredCredentialView& credential);
storage::StorageResult<DecodedStoredCredential>
DecodeStoredCredential(std::span<const std::byte> encoded);

using ServerProfilesChangedCallback = std::function<void()>;

// 显示同步模态服务器管理器。至少新增、编辑或删除过一条配置时返回 true；每次事务
// 提交后立即调用 on_profiles_changed，让所有页面无需等窗口关闭即可刷新服务器列表。
// 删除服务器资料时同时删除 credential_ref 对应 Vault 记录，活动会话由上层决定何时断开。
bool ShowServerManagerDialog(HWND owner,
                             storage::ApplicationRepository& repository,
                             storage::Vault* vault,
                             ServerProfilesChangedCallback on_profiles_changed = {});

} // namespace SLink::app
