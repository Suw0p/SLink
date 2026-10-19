#pragma once

#include "storage/app_paths.h"
#include "transfer/endpoint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SLink::editor {

enum class EditorErrc {
    Ok = 0,
    InvalidArgument,
    NotFound,
    IoError,
    CorruptCache,
    Unsupported,
};

struct EditorStatus {
    EditorErrc code{EditorErrc::Ok};
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return code == EditorErrc::Ok;
    }
    explicit operator bool() const noexcept {
        return ok();
    }

    static EditorStatus Ok() {
        return {};
    }
    static EditorStatus Error(EditorErrc code, std::string message) {
        return {code, std::move(message)};
    }
};

template <typename T> class EditorResult {
public:
    static EditorResult Success(T value) {
        return EditorResult(std::move(value), EditorStatus::Ok());
    }
    static EditorResult Failure(EditorStatus status) {
        return EditorResult(std::nullopt, std::move(status));
    }

    [[nodiscard]] bool ok() const noexcept {
        return value_.has_value() && status_.ok();
    }
    explicit operator bool() const noexcept {
        return ok();
    }
    [[nodiscard]] const EditorStatus& status() const noexcept {
        return status_;
    }
    T& value() & {
        return value_.value();
    }
    const T& value() const& {
        return value_.value();
    }
    T&& value() && {
        return std::move(value_).value();
    }

private:
    EditorResult(T value, EditorStatus status)
        : value_(std::move(value)), status_(std::move(status)) {}
    EditorResult(std::nullopt_t, EditorStatus status)
        : value_(std::nullopt), status_(std::move(status)) {}

    std::optional<T> value_;
    EditorStatus status_;
};

enum class DocumentMode {
    Editable,
    ReadOnlyLarge,
    ReadOnlyBinary,
};

enum class TextEncoding {
    Utf8,
    Utf8Bom,
    Unknown8Bit,
    Gb2312,
};

enum class EolStyle {
    None,
    Lf,
    CrLf,
    Cr,
    Mixed,
};

struct RemoteRevision {
    std::uint64_t size{};
    std::chrono::system_clock::time_point modified{};
    std::uint32_t permissions{};
    // 打开文档时对实际读取字节计算的 SHA-256。即使远端改写后刻意保留大小和 mtime，
    // 保存前的冲突检查仍能发现内容变化。
    std::array<std::byte, 32> content_sha256{};
};

struct ConflictSnapshot {
    std::string local_text;
    std::string remote_text;
    TextEncoding remote_encoding{TextEncoding::Unknown8Bit};
    EolStyle remote_eol{EolStyle::None};
    bool remote_is_binary{false};
    bool remote_too_large{false};
    bool remote_missing{false};
    RemoteRevision remote_revision;
};

enum class SaveOutcome {
    Saved,
    Conflict,
};

enum class SaveMode {
    CheckForConflict,
    OverwriteRemote,
};

struct SaveResult {
    SaveOutcome outcome{SaveOutcome::Saved};
    std::optional<ConflictSnapshot> conflict;
};

struct RecoveryEntry {
    transfer::FileUri uri;
    std::filesystem::path content_path;
    std::filesystem::path metadata_path;
    TextEncoding encoding{TextEncoding::Utf8};
    EolStyle eol{EolStyle::None};
    RemoteRevision base_revision;
};

class RemoteDocument {
public:
    ~RemoteDocument();
    RemoteDocument(RemoteDocument&&) noexcept;
    RemoteDocument& operator=(RemoteDocument&&) noexcept;
    RemoteDocument(const RemoteDocument&) = delete;
    RemoteDocument& operator=(const RemoteDocument&) = delete;

    [[nodiscard]] const transfer::FileUri& uri() const noexcept;
    [[nodiscard]] DocumentMode mode() const noexcept;
    [[nodiscard]] TextEncoding encoding() const noexcept;
    [[nodiscard]] EolStyle eol() const noexcept;
    [[nodiscard]] const std::string& text() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] const std::filesystem::path& cache_path() const noexcept;

    // SSH/SFTP 断线重连后替换文档使用的端点。文本、dirty 状态、缓存和
    // 打开时的远端修订基线全部保留，下一次保存仍会执行冲突检查。
    EditorStatus RebindEndpoint(std::shared_ptr<transfer::IFileEndpoint> endpoint);

    // UI 每次编辑后调用（可在 UI 层防抖），同步更新程序目录内的恢复缓存及 dirty 元数据。
    EditorStatus UpdateText(std::string utf8_or_original_bytes,
                            std::optional<TextEncoding> encoding = std::nullopt);
    EditorResult<SaveResult> Save(SaveMode mode = SaveMode::CheckForConflict);
    // 在同一端点内原子写入另一个 URI。成功后同时更新文档 URI 和恢复缓存标识。
    EditorResult<SaveResult> SaveAs(transfer::FileUri target, bool replace_existing = false);

    // 干净文档关闭时删除缓存和元数据；未保存文档保留用于崩溃恢复。析构行为相同。
    EditorStatus Close();

private:
    friend class RemoteEditorService;
    struct Impl;
    explicit RemoteDocument(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// 远程编辑服务只负责文档内容、缓存、冲突检测和安全保存，不持有任何 Win32 控件。
// 保存流程会重新核对远端修订，并在同目录写临时文件、恢复权限后原子重命名，避免
// 连接中断时把原文件截断。内部缓存始终位于 EXE 相对的 cache/editor 目录。
class RemoteEditorService {
public:
    static constexpr std::uint64_t kEditableSizeLimit = 20ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kBinaryProbeBytes = 64ULL * 1024ULL;

    explicit RemoteEditorService(const storage::AppPaths& paths);

    EditorResult<std::unique_ptr<RemoteDocument>>
    Open(std::shared_ptr<transfer::IFileEndpoint> endpoint, transfer::FileUri uri) const;

    EditorResult<std::vector<RecoveryEntry>> RecoverableDocuments() const;
    EditorResult<std::unique_ptr<RemoteDocument>>
    Recover(std::shared_ptr<transfer::IFileEndpoint> endpoint, const RecoveryEntry& entry) const;

private:
    std::filesystem::path cache_dir_;
};

} // namespace SLink::editor
