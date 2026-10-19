#pragma once

#include "storage/app_paths.h"
#include "storage/storage_status.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace SLink::storage {

struct CrashTimestamp {
    std::uint16_t year{};
    std::uint16_t month{};
    std::uint16_t day{};
    std::uint16_t hour{};
    std::uint16_t minute{};
    std::uint16_t second{};
    std::uint16_t millisecond{};
};

// 安装进程级未处理异常过滤器，只向 AppPaths 指定的便携 logs/crash 写入 minidump；
// 不回退到 TEMP、AppData、注册表或其他位置。崩溃路径验证失败时宁可不生成文件。
class CrashReporter final {
public:
    CrashReporter() = delete;

    // 对同一便携目录重复安装是幂等的；已经生效时拒绝切换到另一个目录。
    static StorageStatus Install(const AppPaths& paths);
    static void Uninstall() noexcept;

    [[nodiscard]] static bool IsInstalled() noexcept;

    // 纯函数辅助接口用于确定性边界测试；生产环境也通过同一逻辑构造 dump 路径。
    [[nodiscard]] static std::wstring BuildDumpFileName(const CrashTimestamp& timestamp,
                                                        std::uint32_t process_id,
                                                        std::uint32_t sequence);
    [[nodiscard]] static std::filesystem::path BuildDumpPath(const AppPaths& paths,
                                                             const CrashTimestamp& timestamp,
                                                             std::uint32_t process_id,
                                                             std::uint32_t sequence);
    [[nodiscard]] static bool IsAllowedDumpPath(const AppPaths& paths,
                                                const std::filesystem::path& candidate);
};

} // namespace SLink::storage
