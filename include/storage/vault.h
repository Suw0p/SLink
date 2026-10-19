#pragma once

#include "storage/secure_buffer.h"
#include "storage/storage_status.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace SLink::storage {

// 便携凭据库。state.db 只保存 credential_ref，真正的密码、私钥和口令在 vault.db
// 中逐条加密。解密结果使用 SecureBuffer 承载，调用方使用后应尽快释放。
// 当前便携模式使用 data/vault.key 自动解锁，不依赖 Windows 凭据管理器或注册表。
class Vault {
public:
    static StorageResult<std::unique_ptr<Vault>> Open(const std::filesystem::path& vault_path);
    static bool IsSupported() noexcept;

    ~Vault();
    Vault(Vault&&) noexcept;
    Vault& operator=(Vault&&) noexcept;
    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    // 创建新凭据库；目标已存在时返回 already_exists，绝不覆盖已有密文。
    StorageStatus Initialize(std::span<const std::byte> master_password);
    StorageStatus Unlock(std::span<const std::byte> master_password);
    // 无交互打开便携凭据库；随机 256 位密钥保存在 vault.db 同目录的 vault.key。
    StorageStatus UnlockPortable();
    void Lock() noexcept;
    [[nodiscard]] bool is_unlocked() const noexcept;

    StorageStatus Put(std::string key, std::span<const std::byte> secret);
    StorageResult<SecureBuffer> Get(const std::string& key) const;
    StorageStatus Remove(const std::string& key);

private:
    struct Impl;
    explicit Vault(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::storage
