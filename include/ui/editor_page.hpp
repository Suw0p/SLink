#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ui/view_state.hpp"

namespace SLink::ui {

using EditorDocumentId = std::uint64_t;

enum class EditorTextEncoding {
    Utf8,
    Utf8Bom,
    Legacy8Bit,
    Gb2312,
};

enum class EditorEolStyle {
    None,
    Lf,
    CrLf,
    Cr,
    Mixed,
};

enum class EditorDocumentKind {
    Text,
    ReadOnlyLarge,
    Binary,
};

// 仅供 UI 使用的文档描述。应用层可由 editor::RemoteDocument 直接映射，Win32 页面
// 不持有远程端点、网络会话或恢复缓存的所有权。
struct EditorDocumentDescriptor {
    // 传 0 时由 EditorPage 分配仅在本次进程内有效的标识。
    EditorDocumentId id{};
    std::wstring title;
    // 文件所属服务器随文档传入，不能使用当前侧栏选择推断。
    std::string server_id;
    std::wstring remote_path;
    std::string content;
    EditorTextEncoding encoding{EditorTextEncoding::Utf8};
    EditorEolStyle eol{EditorEolStyle::None};
    EditorDocumentKind kind{EditorDocumentKind::Text};
    bool read_only{};
    bool dirty{};
};

struct EditorSaveRequest {
    EditorDocumentId id{};
    std::string content;
    EditorTextEncoding encoding{EditorTextEncoding::Utf8};
    EditorEolStyle eol{EditorEolStyle::None};
};

struct EditorFileEntry {
    std::wstring name;
    std::wstring size;
    std::wstring modified;
    bool directory{};
    std::wstring status;
    std::wstring permissions;
    std::wstring owner;
    std::wstring icon_path;
    bool local_shortcut{};
    bool placeholder{};
};

struct EditorServerOption {
    std::string id;
    std::wstring label;
    bool fixed_first{};
};

struct EditorFileActivation {
    std::string server_id;
    std::wstring directory;
    EditorFileEntry entry;
};

class EditorPage final {
public:
    using SaveRequestCallback = std::function<void(EditorSaveRequest)>;
    using DocumentActionCallback = std::function<void(EditorDocumentId)>;
    using DocumentChangedCallback = std::function<void(EditorDocumentId)>;
    using CloseRequestCallback = std::function<void(EditorDocumentId, bool dirty)>;
    using ActiveDocumentChangedCallback = std::function<void(EditorDocumentId)>;
    using FileActivationCallback = std::function<void(EditorFileActivation)>;
    using ServerChangedCallback = std::function<void(std::string)>;
    using PathChangedCallback = std::function<void(std::string, std::wstring)>;
    using HistoryNavigationCallback = std::function<void(bool forward)>;
    using ViewActionCallback = std::function<void(ViewRecoveryAction)>;

    EditorPage();
    ~EditorPage();

    EditorPage(const EditorPage&) = delete;
    EditorPage& operator=(const EditorPage&) = delete;

    bool Create(HWND parent, HINSTANCE instance);
    void SetBounds(int x, int y, int width, int height);
    void Show(bool visible);
    void ApplyTheme();
    // 共享服务器侧栏切换不会改变文件模型，但会改变页面的客户区起点和宽度。
    // 主窗口提交新边界后调用此方法，立即重新布局并刷新全部自绘子控件。
    void RefreshLayout();

    void SetServer(EditorServerOption server);
    void SetServers(std::vector<EditorServerOption> servers, std::string selected_server_id = {});
    void SetRemotePath(std::wstring remote_path);
    void SetFileEntries(std::vector<EditorFileEntry> entries);
    void SetDirectoryState(ViewState state);
    void SetViewActionCallback(ViewActionCallback callback);
    bool ActivateServer(std::string_view server_id);
    // 打开或更新文本/大文件文档并设为活动标签；只修改 UI 状态，不执行文件或网络 I/O。
    EditorDocumentId OpenDocument(EditorDocumentDescriptor document);

    // 打开只读十六进制预览。UI 只物化有界前缀，调用方无需传入整个二进制文件。
    EditorDocumentId OpenBinaryDocument(EditorDocumentDescriptor document,
                                        std::span<const std::byte> bytes);

    // Compatibility overload used by the initial shell.
    bool OpenDocument(std::wstring tab_title, const std::string& utf8_text, bool read_only);

    bool ActivateDocument(EditorDocumentId id);
    bool CloseDocument(EditorDocumentId id);
    bool SetDocumentContent(EditorDocumentId id,
                            std::string content,
                            EditorTextEncoding encoding,
                            EditorEolStyle eol,
                            EditorDocumentKind kind,
                            bool read_only,
                            bool dirty);
    void SetDocumentSaving(EditorDocumentId id, bool saving);
    void MarkDocumentSaved(EditorDocumentId id);
    void SetDocumentConflict(EditorDocumentId id, std::wstring message);
    void ClearDocumentConflict(EditorDocumentId id);

    [[nodiscard]] std::optional<std::string> DocumentText(EditorDocumentId id);
    [[nodiscard]] std::optional<EditorDocumentId> active_document() const noexcept;
    [[nodiscard]] std::size_t document_count() const noexcept;

    // 以下请求均在 UI 线程触发。处理器必须把网络/磁盘操作加入后台队列并立即返回，
    // 完成后再通过 SetDocumentContent、SetDocumentSaving、MarkDocumentSaved 或冲突
    // 接口把结果应用回页面，禁止在 Scintilla 通知回调中阻塞等待远端。
    void SetSaveRequestedCallback(SaveRequestCallback callback);
    void SetEncodingChangedCallback(SaveRequestCallback callback);
    void SetOverwriteRequestedCallback(SaveRequestCallback callback);
    void SetSaveAsRequestedCallback(SaveRequestCallback callback);
    void SetDiffRequestedCallback(DocumentActionCallback callback);
    void SetCloseRequestedCallback(CloseRequestCallback callback);
    void SetDocumentChangedCallback(DocumentChangedCallback callback);
    void SetActiveDocumentChangedCallback(ActiveDocumentChangedCallback callback);
    void SetFileActivationCallback(FileActivationCallback callback);
    void SetServerChangedCallback(ServerChangedCallback callback);
    void SetPathChangedCallback(PathChangedCallback callback);
    void SetHistoryNavigationCallback(HistoryNavigationCallback callback);

    void RequestSave();

    // Compatibility helpers apply to the currently active document.
    void ShowConflict(std::wstring message);
    void HideConflict();

    HWND editor_hwnd() const noexcept;
    HWND hwnd() const noexcept;
    bool scintilla_available() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::ui
