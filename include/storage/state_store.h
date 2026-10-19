#pragma once

#include "storage/storage_status.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace SLink::storage {

struct StateEntry {
    std::string key;
    std::vector<std::byte> value;
};

struct StateMutation {
    enum class Kind {
        Set,
        Remove
    } kind{Kind::Set};
    std::string name_space;
    std::string key;
    std::vector<std::byte> value;
};

// 带命名空间的非敏感键值存储，当前后端为程序目录内的 SQLite state.db。
// 密码、私钥和口令只能经 Vault 保存，禁止通过本接口写入。
class StateStore {
public:
    static StorageResult<std::unique_ptr<StateStore>>
    Open(const std::filesystem::path& database_path);

    ~StateStore();
    StateStore(StateStore&&) noexcept;
    StateStore& operator=(StateStore&&) noexcept;
    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;

    StorageStatus Set(std::string name_space, std::string key, std::span<const std::byte> value);
    StorageResult<std::vector<std::byte>> Get(const std::string& name_space,
                                              const std::string& key) const;
    StorageResult<std::vector<StateEntry>> List(const std::string& name_space) const;
    StorageStatus Remove(const std::string& name_space, const std::string& key);
    // 一次提交多个关联状态；任何一步失败都不保留部分修改。
    StorageStatus ApplyBatch(std::span<const StateMutation> mutations);
    StorageStatus Flush();

    // 用于诊断/测试实际启用的持久化后端，不应据此改变业务语义。
    [[nodiscard]] bool uses_sqlite() const noexcept;

private:
    struct Impl;
    explicit StateStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::storage
