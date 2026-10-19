#pragma once

#include "editor/remote_document.hpp"
#include "storage/app_paths.h"
#include "transfer/endpoint.hpp"
#include "ui/editor_page.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace SLink::app {

// 连接 Win32 编辑页面与 RemoteDocument 的应用层协调器。网络、缓存和哈希计算均在
// 单独工作线程执行，禁止阻塞 UI。控制器必须在 EditorPage 所在线程创建和销毁。
// 文档对象由控制器持有，页面只保存进程内 id 和显示内容。
class EditorController final {
public:
    using EndpointResolver =
        std::function<std::shared_ptr<transfer::IFileEndpoint>(const transfer::FileUri&)>;
    using FileActivationResolver =
        std::function<std::optional<transfer::FileUri>(const ui::EditorFileActivation&)>;
    using SaveAsTargetResolver =
        std::function<std::optional<transfer::FileUri>(const transfer::FileUri&)>;
    using CloseConfirmation = std::function<bool(const transfer::FileUri&)>;
    using DirectoryActivationCallback = std::function<void(const ui::EditorFileActivation&)>;
    using ConflictDiffCallback = std::function<void(
        ui::EditorDocumentId, const transfer::FileUri&, const editor::ConflictSnapshot&)>;
    using ErrorCallback =
        std::function<void(std::wstring_view operation, std::wstring_view message)>;

    EditorController(ui::EditorPage& page,
                     const storage::AppPaths& paths,
                     EndpointResolver endpoint_resolver);
    ~EditorController();

    EditorController(const EditorController&) = delete;
    EditorController& operator=(const EditorController&) = delete;

    // 异步打开本地或 SFTP URI；同一 URI 已打开时只激活原标签，不重复下载或建缓存。
    void Open(transfer::FileUri uri);

    // 枚举 editor_cache_dir 下的 dirty 恢复记录，并异步恢复当前可解析端点的文档。
    void RecoverUnsaved();

    // 文件会话重连后，将该服务器已打开文档从旧端点切换到新端点。
    // 调用在 UI 线程返回；实际绑定按编辑工作队列顺序异步完成。
    void RebindEndpoint(std::string server_id,
                        std::shared_ptr<transfer::IFileEndpoint> endpoint);

    // 除 EndpointResolver 在编辑工作线程执行外，下列 provider 均在 UI 线程调用。
    // UI provider 必须立即返回；EndpointResolver 可以等待已连接端点，但不能弹窗。
    void SetFileActivationResolver(FileActivationResolver resolver);
    void SetSaveAsTargetResolver(SaveAsTargetResolver resolver);
    void SetCloseConfirmation(CloseConfirmation confirmation);
    void SetDirectoryActivationCallback(DirectoryActivationCallback callback);
    void SetConflictDiffCallback(ConflictDiffCallback callback);
    void SetErrorCallback(ErrorCallback callback);

    [[nodiscard]] std::size_t document_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::app
