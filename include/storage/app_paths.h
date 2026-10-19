#pragma once

#include "storage/storage_status.h"

#include <filesystem>

namespace SLink::storage {

// 所有应用内部路径都从可执行文件所在目录推导。禁止回退到 AppData、注册表、当前
// 工作目录或 TEMP；程序目录不可写时应拒绝启动，确保便携数据不会污染其他目录。
class AppPaths {
public:
    static StorageResult<AppPaths> InitializeForCurrentExecutable();

    // 供便携启动器和确定性测试使用。传入值必须是绝对 EXE 文件名，但文件可不存在。
    static StorageResult<AppPaths>
    InitializeFromExecutablePath(const std::filesystem::path& executable_path);

    [[nodiscard]] const std::filesystem::path& executable_path() const noexcept {
        return executable_path_;
    }
    [[nodiscard]] const std::filesystem::path& application_dir() const noexcept {
        return application_dir_;
    }
    [[nodiscard]] const std::filesystem::path& data_dir() const noexcept {
        return data_dir_;
    }
    [[nodiscard]] const std::filesystem::path& cache_dir() const noexcept {
        return cache_dir_;
    }
    [[nodiscard]] const std::filesystem::path& editor_cache_dir() const noexcept {
        return editor_cache_dir_;
    }
    [[nodiscard]] const std::filesystem::path& logs_dir() const noexcept {
        return logs_dir_;
    }
    [[nodiscard]] const std::filesystem::path& crash_dir() const noexcept {
        return crash_dir_;
    }
    [[nodiscard]] std::filesystem::path state_database() const {
        return data_dir_ / L"state.db";
    }
    [[nodiscard]] std::filesystem::path vault_database() const {
        return data_dir_ / L"vault.db";
    }
    [[nodiscard]] std::filesystem::path known_hosts() const {
        return data_dir_ / L"known_hosts";
    }

    // 解析已存在的父目录和重解析点，确认内部路径最终仍位于 EXE 目录之下，防止通过
    // 符号链接或 ".." 把缓存、日志和数据库写到应用目录之外。
    [[nodiscard]] bool ContainsInternalPath(const std::filesystem::path& path) const;

private:
    std::filesystem::path executable_path_;
    std::filesystem::path application_dir_;
    std::filesystem::path data_dir_;
    std::filesystem::path cache_dir_;
    std::filesystem::path editor_cache_dir_;
    std::filesystem::path logs_dir_;
    std::filesystem::path crash_dir_;
};

} // namespace SLink::storage
