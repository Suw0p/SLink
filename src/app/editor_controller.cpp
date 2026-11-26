// 远程编辑协调器实现：按服务器串行、跨服务器并行执行文档操作，再把结果投递回 UI。
#include "app/editor_controller.hpp"
#include "app/editor_work_scheduler.hpp"
#include "editor/gb2312_codec.hpp"
#include "ui/file_operation_dialogs.hpp"

#ifndef _WIN32
#error EditorController is a Windows-only implementation
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <commctrl.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SLink::app {
namespace {

constexpr UINT kDispatchMessage = WM_APP + 0x534;
constexpr std::chrono::milliseconds kCheckpointDelay{400};
constexpr wchar_t kDispatchClass[] = L"SLink.EditorController.Dispatch.1";

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return L"Invalid UTF-8 text";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        length);
    return result;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) {
        throw std::invalid_argument("Text is not valid Unicode");
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        length,
                        nullptr,
                        nullptr);
    return result;
}

ui::EditorTextEncoding ToUiEncoding(editor::TextEncoding value) {
    switch (value) {
    case editor::TextEncoding::Utf8:
        return ui::EditorTextEncoding::Utf8;
    case editor::TextEncoding::Utf8Bom:
        return ui::EditorTextEncoding::Utf8Bom;
    case editor::TextEncoding::Unknown8Bit:
        return ui::EditorTextEncoding::Legacy8Bit;
    case editor::TextEncoding::Gb2312:
        return ui::EditorTextEncoding::Gb2312;
    }
    return ui::EditorTextEncoding::Legacy8Bit;
}

editor::TextEncoding ToDocumentEncoding(const ui::EditorTextEncoding value) {
    switch (value) {
    case ui::EditorTextEncoding::Utf8:
        return editor::TextEncoding::Utf8;
    case ui::EditorTextEncoding::Utf8Bom:
        return editor::TextEncoding::Utf8Bom;
    case ui::EditorTextEncoding::Legacy8Bit:
        return editor::TextEncoding::Unknown8Bit;
    case ui::EditorTextEncoding::Gb2312:
        return editor::TextEncoding::Gb2312;
    }
    return editor::TextEncoding::Utf8;
}

editor::EditorStatus CheckpointForSave(editor::RemoteDocument& document,
                                       ui::EditorSaveRequest& request) {
    auto status = document.UpdateText(request.content, ToDocumentEncoding(request.encoding));
    if (!status && status.code == editor::EditorErrc::Unsupported) {
        // 保存编码无法表示新字符时，远端文件保持不变；最新编辑仍要写入恢复缓存。
        // 保存请求已经取消之前防抖的缓存任务，所以在此同步补记。
        if (auto cached = document.UpdateText(std::move(request.content)); !cached)
            return cached;
    }
    return status;
}

ui::EditorEolStyle ToUiEol(editor::EolStyle value) {
    switch (value) {
    case editor::EolStyle::None:
        return ui::EditorEolStyle::None;
    case editor::EolStyle::Lf:
        return ui::EditorEolStyle::Lf;
    case editor::EolStyle::CrLf:
        return ui::EditorEolStyle::CrLf;
    case editor::EolStyle::Cr:
        return ui::EditorEolStyle::Cr;
    case editor::EolStyle::Mixed:
        return ui::EditorEolStyle::Mixed;
    }
    return ui::EditorEolStyle::None;
}

ui::EditorDocumentKind ToUiKind(editor::DocumentMode value) {
    switch (value) {
    case editor::DocumentMode::Editable:
        return ui::EditorDocumentKind::Text;
    case editor::DocumentMode::ReadOnlyLarge:
        return ui::EditorDocumentKind::ReadOnlyLarge;
    case editor::DocumentMode::ReadOnlyBinary:
        return ui::EditorDocumentKind::Binary;
    }
    return ui::EditorDocumentKind::Text;
}

std::wstring DocumentTitle(const transfer::FileUri& uri) {
    auto title = Utf8ToWide(uri.filename());
    return title.empty() ? L"(remote file)" : std::move(title);
}

std::wstring OperationMessage(std::wstring_view operation, std::wstring_view message) {
    std::wstring result(operation);
    if (!result.empty()) {
        result.append(L"\r\n\r\n");
    }
    result.append(message);
    return result;
}

std::vector<std::byte> ReadBinaryProbe(transfer::IFileEndpoint& endpoint,
                                       const transfer::FileUri& uri) {
    auto input = endpoint.open_read(uri);
    std::vector<std::byte> bytes;
    bytes.reserve(editor::RemoteEditorService::kBinaryProbeBytes);
    std::vector<std::byte> chunk(16 * 1024);
    while (bytes.size() < editor::RemoteEditorService::kBinaryProbeBytes) {
        const auto remaining = editor::RemoteEditorService::kBinaryProbeBytes - bytes.size();
        const auto read =
            input->read(std::span<std::byte>(chunk).first((std::min)(remaining, chunk.size())));
        if (read == 0) {
            break;
        }
        if (read > chunk.size() || read > remaining) {
            throw std::runtime_error("Endpoint returned an invalid binary probe length");
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
    }
    return bytes;
}

} // namespace

struct EditorController::Impl {
    struct DocumentState {
        ui::EditorDocumentId id{};
        transfer::FileUri uri;
        std::string lane;
        std::shared_ptr<transfer::IFileEndpoint> endpoint;
        std::unique_ptr<editor::RemoteDocument> document;
        std::optional<editor::ConflictSnapshot> conflict;
        // 仅 UI 线程访问，覆盖操作入队前可检查另存为目标是否已被打开。
        std::optional<transfer::FileUri> ui_save_as_target;
        std::mutex mutex;
        std::atomic<std::uint64_t> edit_generation{};
        std::atomic<bool> ui_dirty{};
        std::atomic<bool> closing{};
    };

    struct OpenPayload {
        std::shared_ptr<DocumentState> state;
        ui::EditorDocumentDescriptor descriptor;
        std::vector<std::byte> binary_probe;
    };

    struct PendingClose {
        std::shared_ptr<DocumentState> state;
        std::optional<ui::EditorSaveRequest> snapshot;
        bool completed{}; // 由 state->mutex 保护，任务取消后仍由退出协调器接管。
        CloseDecision decision{CloseDecision::KeepDraft};
        bool saved{}, save_conflict{};
    };

    ui::EditorPage& page;
    editor::RemoteEditorService service;
    EndpointResolver endpoint_resolver;
    FileActivationResolver file_activation_resolver;
    SaveAsTargetResolver save_as_target_resolver;
    CloseConfirmation close_confirmation;
    DirectoryActivationCallback directory_activation;
    ConflictDiffCallback conflict_diff;
    ErrorCallback error_callback;

    DWORD ui_thread_id{GetCurrentThreadId()};
    HWND dispatch_window{};
    std::unique_ptr<EditorWorkScheduler> scheduler;

    std::mutex completion_mutex;
    std::deque<std::function<void()>> completions;
    std::atomic<bool> stopping{};

    // Accessed on the UI thread only.
    ui::EditorDocumentId next_document_id{1};
    std::unordered_map<ui::EditorDocumentId, std::shared_ptr<DocumentState>> documents;
    std::unordered_map<std::string, ui::EditorDocumentId> uri_index;
    std::unordered_set<std::string> reopening_uris;
    std::unordered_map<std::string, ui::EditorDocumentId> reserved_save_targets;
    std::vector<std::shared_ptr<PendingClose>> pending_closes;
    bool preparing_shutdown{}; // UI 线程退出屏障，不能与普通编辑并行提交。

    Impl(ui::EditorPage& editor_page, const storage::AppPaths& paths, EndpointResolver resolver)
        : page(editor_page), service(paths), endpoint_resolver(std::move(resolver)) {
        if (!endpoint_resolver) {
            throw std::invalid_argument("EditorController requires an endpoint resolver");
        }
        if (page.hwnd() == nullptr ||
            GetWindowThreadProcessId(page.hwnd(), nullptr) != ui_thread_id) {
            throw std::invalid_argument(
                "EditorController must be created after EditorPage on its UI thread");
        }
        if (!paths.ContainsInternalPath(paths.editor_cache_dir()) ||
            !std::filesystem::is_directory(paths.editor_cache_dir())) {
            throw std::invalid_argument("Editor cache directory is not a valid AppPaths directory");
        }

        RegisterDispatchClass();
        dispatch_window = CreateWindowExW(0,
                                          kDispatchClass,
                                          L"",
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          HWND_MESSAGE,
                                          nullptr,
                                          GetModuleHandleW(nullptr),
                                          this);
        if (dispatch_window == nullptr) {
            throw std::runtime_error("Cannot create the editor UI dispatch window");
        }

        try {
            scheduler =
                std::make_unique<EditorWorkScheduler>(4, [this](std::exception_ptr failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const std::exception& error) {
                        ReportErrorAsync(L"编辑器后台任务", error.what());
                    } catch (...) {
                        ReportErrorAsync(L"编辑器后台任务", "Unknown background error");
                    }
                });
            BindPageCallbacks();
        } catch (...) {
            UnbindPageCallbacks();
            DestroyWindow(dispatch_window);
            dispatch_window = nullptr;
            throw;
        }
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        UnbindPageCallbacks();

        auto pending = pending_closes;
        pending.reserve(pending.size() + documents.size());
        for (const auto& [id, state] : documents) {
            if (state->closing.load(std::memory_order_acquire))
                continue; // pending_closes 已持有这份快照，不能在关闭后重复 UpdateText。
            auto item = std::make_shared<PendingClose>();
            item->state = state;
            if (state->ui_dirty.load(std::memory_order_acquire))
                item->snapshot = page.DocumentSnapshot(id);
            pending.push_back(std::move(item));
        }

        scheduler->Stop();
        for (const auto& item : pending)
            (void)CompleteClose(*item);
        pending_closes.clear();
        documents.clear();
        uri_index.clear();

        {
            std::scoped_lock lock(completion_mutex);
            completions.clear();
        }
        if (dispatch_window != nullptr) {
            DestroyWindow(dispatch_window);
            dispatch_window = nullptr;
        }
    }

    static LRESULT CALLBACK DispatchWindowProc(HWND window,
                                               UINT message,
                                               WPARAM wparam,
                                               LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == kDispatchMessage && self != nullptr) {
            self->DrainCompletions();
            return 0;
        }
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static void RegisterDispatchClass() {
        static std::once_flag once;
        static DWORD registration_error{};
        std::call_once(once, [] {
            WNDCLASSEXW type{};
            type.cbSize = sizeof(type);
            type.lpfnWndProc = &DispatchWindowProc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = kDispatchClass;
            if (RegisterClassExW(&type) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                registration_error = GetLastError();
            }
        });
        if (registration_error != ERROR_SUCCESS) {
            throw std::runtime_error("Cannot register the editor UI dispatch window");
        }
    }

    void BindPageCallbacks() {
        page.SetFileActivationCallback([this](ui::EditorFileActivation activation) {
            OnFileActivated(std::move(activation));
        });
        page.SetSaveRequestedCallback([this](ui::EditorSaveRequest request) {
            OnSave(std::move(request));
        });
        page.SetEncodingChangedCallback([this](ui::EditorSaveRequest request) {
            OnEncodingChanged(std::move(request));
        });
        page.SetOverwriteRequestedCallback([this](ui::EditorSaveRequest request) {
            OnOverwrite(std::move(request));
        });
        page.SetSaveAsRequestedCallback([this](ui::EditorSaveRequest request) {
            OnSaveAs(std::move(request));
        });
        page.SetDiffRequestedCallback([this](ui::EditorDocumentId id) {
            OnDiff(id);
        });
        page.SetChangesRequestedCallback([this](ui::EditorDocumentId id) {
            OnChanges(id);
        });
        page.SetCloseRequestedCallback([this](ui::EditorDocumentId id, bool dirty) {
            OnClose(id, dirty);
        });
        page.SetDocumentChangedCallback([this](ui::EditorDocumentId id) {
            OnDocumentChanged(id);
        });
    }

    void UnbindPageCallbacks() {
        page.SetFileActivationCallback({});
        page.SetSaveRequestedCallback({});
        page.SetEncodingChangedCallback({});
        page.SetOverwriteRequestedCallback({});
        page.SetSaveAsRequestedCallback({});
        page.SetDiffRequestedCallback({});
        page.SetChangesRequestedCallback({});
        page.SetCloseRequestedCallback({});
        page.SetDocumentChangedCallback({});
    }

    static std::string LaneFor(const transfer::FileUri& uri) {
        return uri.is_sftp() ? "sftp:" + uri.authority() : "local";
    }

    void
    Enqueue(std::string lane, std::function<void()> action, std::chrono::milliseconds delay = {}) {
        if (stopping.load(std::memory_order_acquire)) {
            return;
        }
        scheduler->Enqueue(std::move(lane), std::move(action), delay);
    }

    void PostToUi(std::function<void()> action) {
        if (stopping.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::scoped_lock lock(completion_mutex);
            if (stopping.load(std::memory_order_relaxed)) {
                return;
            }
            completions.push_back(std::move(action));
        }
        PostMessageW(dispatch_window, kDispatchMessage, 0, 0);
    }

    void DrainCompletions() {
        std::deque<std::function<void()>> ready;
        {
            std::scoped_lock lock(completion_mutex);
            ready.swap(completions);
        }
        for (auto& completion : ready) {
            try {
                completion();
            } catch (const std::exception& error) {
                ReportError(L"编辑器界面", Utf8ToWide(error.what()));
            } catch (...) {
                ReportError(L"编辑器界面", L"发生未知错误。");
            }
        }
    }

    bool IsCurrent(const std::shared_ptr<DocumentState>& state) const {
        const auto item = documents.find(state->id);
        return item != documents.end() && item->second == state;
    }

    void ReportError(std::wstring_view operation, std::wstring_view message) {
        if (error_callback) {
            error_callback(operation, message);
            return;
        }
        const auto text = OperationMessage(operation, message);
        ::SLink::ui::ShowMessageDialog(page.hwnd(), text.c_str(), L"SLink", MB_OK | MB_ICONERROR);
    }

    void ReportErrorAsync(std::wstring operation, std::string message) {
        PostToUi([this, operation = std::move(operation), message = std::move(message)] {
            ReportError(operation, Utf8ToWide(message));
        });
    }

    std::shared_ptr<transfer::IFileEndpoint> ResolveEndpoint(const transfer::FileUri& uri) {
        auto endpoint = endpoint_resolver(uri);
        if (!endpoint || !endpoint->accepts(uri) || !endpoint->available()) {
            throw std::runtime_error("No connected file endpoint accepts the requested URI");
        }
        return endpoint;
    }

    ui::EditorDocumentDescriptor Describe(const DocumentState& state,
                                          const editor::RemoteDocument& document,
                                          bool dirty = false) const {
        ui::EditorDocumentDescriptor result;
        const auto& document_uri = document.uri();
        result.id = state.id;
        result.title = DocumentTitle(document_uri);
        result.server_id = document_uri.is_sftp() ? document_uri.authority() : "local://host";
        result.remote_path = Utf8ToWide(document_uri.path());
        result.content = document.text();
        result.encoding = ToUiEncoding(document.encoding());
        result.eol = ToUiEol(document.eol());
        result.kind = ToUiKind(document.mode());
        result.read_only = document.mode() != editor::DocumentMode::Editable;
        result.dirty = dirty;
        return result;
    }

    void Open(transfer::FileUri uri, bool reopen_existing = false) {
        if (GetCurrentThreadId() != ui_thread_id) {
            PostToUi([this, uri = std::move(uri), reopen_existing]() mutable {
                Open(std::move(uri), reopen_existing);
            });
            return;
        }
        if (preparing_shutdown)
            return;
        const auto key = uri.to_string();
        if (reopening_uris.contains(key)) {
            return;
        }
        if (const auto found = uri_index.find(key); found != uri_index.end()) {
            const auto id = found->second;
            // 尚在首次加载时不重复排队；已显示的文件再次从列表打开才重新读取。
            if (!page.ActivateDocument(id) || !reopen_existing) {
                return;
            }
            const auto state = documents.find(id);
            if (state == documents.end() ||
                state->second->closing.load(std::memory_order_acquire)) {
                return;
            }
            if (page.DocumentSaving(id)) {
                ReportError(L"重新打开文件", L"文件正在保存，请等待保存结束后再重新打开。");
                return;
            }
            const bool discard_local_changes =
                state->second->ui_dirty.load(std::memory_order_acquire);
            if (discard_local_changes &&
                ::SLink::ui::ShowMessageDialog(
                    page.hwnd(),
                    L"此文件有未保存的本地修改。重新读取远端文件会丢弃这些修改，"
                    L"且不会上传或覆盖远端文件。\r\n\r\n确定丢弃本地修改并重新打开吗？",
                    L"重新打开文件",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return;
            }
            auto previous = state->second;
            previous->closing.store(true, std::memory_order_release);
            previous->edit_generation.fetch_add(1, std::memory_order_acq_rel);
            uri_index.erase(found);
            documents.erase(state);
            page.CloseDocument(id);
            reopening_uris.insert(key);
            Enqueue(previous->lane,
                    [this, previous, uri = std::move(uri), key, discard_local_changes]() mutable {
                        editor::EditorStatus closed;
                        {
                            std::scoped_lock lock(previous->mutex);
                            if (previous->document) {
                                closed = discard_local_changes
                                             ? previous->document->DiscardChanges()
                                             : previous->document->Close();
                            }
                        }
                        PostToUi([this, uri = std::move(uri), key, closed]() mutable {
                            reopening_uris.erase(key);
                            if (!closed) {
                                ReportError(L"重新打开文件", Utf8ToWide(closed.message));
                                return;
                            }
                            Open(std::move(uri));
                        });
                    });
            return;
        }

        if (reserved_save_targets.contains(key)) {
            ReportError(L"打开文件", L"此文件正在被另存为，请等待保存结束后再打开。");
            return;
        }

        auto state = std::make_shared<DocumentState>();
        state->id = next_document_id++;
        state->uri = std::move(uri);
        state->lane = LaneFor(state->uri);
        documents.emplace(state->id, state);
        uri_index.emplace(key, state->id);

        Enqueue(state->lane, [this, state] {
            try {
                auto endpoint = ResolveEndpoint(state->uri);
                auto opened = service.Open(endpoint, state->uri);
                if (!opened) {
                    throw std::runtime_error(opened.status().message);
                }

                auto payload = std::make_shared<OpenPayload>();
                payload->state = state;
                {
                    std::scoped_lock lock(state->mutex);
                    state->endpoint = std::move(endpoint);
                    state->document = std::move(opened).value();
                    state->ui_dirty.store(state->document->dirty(), std::memory_order_release);
                    payload->descriptor =
                        Describe(*state, *state->document, state->document->dirty());
                    if (state->document->mode() == editor::DocumentMode::ReadOnlyBinary) {
                        payload->binary_probe = ReadBinaryProbe(*state->endpoint, state->uri);
                    }
                }
                PostToUi([this, payload] {
                    if (!IsCurrent(payload->state)) {
                        return;
                    }
                    if (payload->descriptor.kind == ui::EditorDocumentKind::Binary) {
                        page.OpenBinaryDocument(std::move(payload->descriptor),
                                                payload->binary_probe);
                    } else {
                        page.OpenDocument(std::move(payload->descriptor));
                    }
                });
            } catch (const std::exception& error) {
                const auto message = std::string(error.what());
                PostToUi([this, state, message] {
                    if (IsCurrent(state)) {
                        uri_index.erase(state->uri.to_string());
                        documents.erase(state->id);
                    }
                    ReportError(L"打开远程文件", Utf8ToWide(message));
                });
            }
        });
    }

    void RecoverUnsaved() {
        Enqueue("control", [this] {
            auto entries = service.RecoverableDocuments();
            if (!entries) {
                ReportErrorAsync(L"恢复未保存文件", entries.status().message);
                return;
            }
            for (const auto& entry : entries.value()) {
                try {
                    auto endpoint = endpoint_resolver(entry.uri);
                    // Recovery is attempted incrementally as file sessions
                    // become available. Cache entries for other, currently
                    // disconnected servers remain untouched and are not
                    // reported as errors.
                    if (!endpoint || !endpoint->accepts(entry.uri) || !endpoint->available()) {
                        continue;
                    }
                    auto recovered = service.Recover(endpoint, entry);
                    if (!recovered) {
                        throw std::runtime_error(recovered.status().message);
                    }
                    auto document =
                        std::shared_ptr<editor::RemoteDocument>(std::move(recovered).value());
                    auto endpoint_copy = std::move(endpoint);
                    const auto uri = entry.uri;
                    PostToUi([this,
                              document = std::move(document),
                              endpoint = std::move(endpoint_copy),
                              uri] {
                        const auto key = uri.to_string();
                        if (const auto found = uri_index.find(key); found != uri_index.end()) {
                            page.ActivateDocument(found->second);
                            return;
                        }
                        auto state = std::make_shared<DocumentState>();
                        state->id = next_document_id++;
                        state->uri = uri;
                        state->lane = LaneFor(state->uri);
                        state->endpoint = endpoint;
                        state->document =
                            std::make_unique<editor::RemoteDocument>(std::move(*document));
                        state->ui_dirty.store(true, std::memory_order_release);
                        auto descriptor = Describe(*state, *state->document, true);
                        documents.emplace(state->id, state);
                        uri_index.emplace(key, state->id);
                        page.OpenDocument(std::move(descriptor));
                    });
                } catch (const std::exception& error) {
                    ReportErrorAsync(L"恢复未保存文件", error.what());
                }
            }
        });
    }

    void RebindEndpoint(std::string server_id, std::shared_ptr<transfer::IFileEndpoint> endpoint) {
        if (GetCurrentThreadId() != ui_thread_id) {
            PostToUi(
                [this, server_id = std::move(server_id), endpoint = std::move(endpoint)]() mutable {
                    RebindEndpoint(std::move(server_id), std::move(endpoint));
                });
            return;
        }
        if (server_id.empty() || !endpoint) {
            return;
        }

        // 一个端点可能对应多个已打开标签。在 UI 线程拍快照，再按文档
        // 工作队列顺序替换，使重连前已排队的缓存更新不会与端点切换并发。
        std::vector<std::shared_ptr<DocumentState>> matching;
        matching.reserve(documents.size());
        for (const auto& [id, state] : documents) {
            (void)id;
            if (state && state->uri.is_sftp() && state->uri.authority() == server_id &&
                !state->closing.load(std::memory_order_acquire)) {
                matching.push_back(state);
            }
        }
        for (const auto& state : matching) {
            Enqueue(state->lane, [this, state, endpoint] {
                editor::EditorStatus status;
                {
                    std::scoped_lock lock(state->mutex);
                    if (!state->document || state->closing.load(std::memory_order_acquire)) {
                        return;
                    }
                    status = state->document->RebindEndpoint(endpoint);
                    if (status) {
                        state->endpoint = endpoint;
                    }
                }
                if (!status) {
                    ReportErrorAsync(L"恢复远程编辑连接", status.message);
                }
            });
        }
    }

    void OnFileActivated(ui::EditorFileActivation activation) {
        if (activation.entry.directory || activation.entry.local_shortcut) {
            if (directory_activation) {
                directory_activation(activation);
            }
            return;
        }
        try {
            std::optional<transfer::FileUri> uri;
            if (file_activation_resolver) {
                uri = file_activation_resolver(activation);
            } else {
                auto directory = WideToUtf8(activation.directory);
                auto name = WideToUtf8(activation.entry.name);
                uri = transfer::FileUri::sftp(activation.server_id, directory).joined(name);
            }
            if (uri) {
                Open(std::move(*uri), true);
            }
        } catch (const std::exception& error) {
            ReportError(L"打开远程文件", Utf8ToWide(error.what()));
        }
    }

    void OnDocumentChanged(ui::EditorDocumentId id) {
        const auto found = documents.find(id);
        if (found == documents.end() || found->second->closing.load(std::memory_order_acquire) ||
            preparing_shutdown) {
            return;
        }
        auto state = found->second;
        state->ui_dirty.store(page.DocumentDirty(id), std::memory_order_release);
        const auto generation = state->edit_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        scheduler->EnqueueLatest(
            state->lane,
            "checkpoint:" + std::to_string(id),
            [this, state, id, generation] {
                if (state->closing.load(std::memory_order_acquire) ||
                    state->edit_generation.load(std::memory_order_acquire) != generation) {
                    return;
                }
                PostToUi([this, state, id, generation] {
                    if (state->closing.load(std::memory_order_acquire) ||
                        state->edit_generation.load(std::memory_order_acquire) != generation) {
                        return;
                    }
                    auto text = page.DocumentText(id);
                    if (!text) {
                        return;
                    }
                    scheduler->EnqueueLatest(
                        state->lane,
                        "checkpoint:" + std::to_string(id),
                        [this, state, generation, text = std::move(*text)]() mutable {
                            if (state->closing.load(std::memory_order_acquire) ||
                                state->edit_generation.load(std::memory_order_acquire) !=
                                    generation) {
                                return;
                            }
                            editor::EditorStatus status;
                            bool dirty = true;
                            {
                                std::scoped_lock lock(state->mutex);
                                if (!state->document) {
                                    return;
                                }
                                status = state->document->UpdateText(std::move(text));
                                dirty = state->document->dirty();
                            }
                            if (!status) {
                                ReportErrorAsync(L"保存编辑器恢复缓存", status.message);
                            } else {
                                PostToUi([this, state, generation, dirty] {
                                    if (!IsCurrent(state) || state->closing.load() ||
                                        state->edit_generation.load() != generation ||
                                        preparing_shutdown)
                                        return;
                                    state->ui_dirty.store(dirty);
                                    if (page.DocumentDirty(state->id) != dirty)
                                        page.SetDocumentDirty(state->id, dirty);
                                });
                            }
                        });
                });
            },
            kCheckpointDelay);
    }

    bool ReserveSaveTarget(const std::shared_ptr<DocumentState>& state,
                           const transfer::FileUri& target) {
        const auto key = target.to_string();
        const auto opened = uri_index.find(key);
        const auto reserved = reserved_save_targets.find(key);
        if ((opened != uri_index.end() && opened->second != state->id) ||
            (reserved != reserved_save_targets.end() && reserved->second != state->id) ||
            reopening_uris.contains(key)) {
            ReportError(L"另存为",
                        L"目标文件已在其他标签打开或正在处理，请先关闭该文件或选择其他名称。");
            return false;
        }
        reserved_save_targets[key] = state->id;
        return true;
    }

    void ReleaseSaveTarget(const std::shared_ptr<DocumentState>& state) {
        std::erase_if(reserved_save_targets, [&](const auto& item) {
            return item.second == state->id;
        });
    }

    void FinishSave(const std::shared_ptr<DocumentState>& state,
                    std::uint64_t generation,
                    const std::optional<ui::EditorDocumentDescriptor>& location = std::nullopt) {
        if (location) {
            page.SetDocumentLocation(
                state->id, location->title, location->server_id, location->remote_path);
        }
        state->ui_save_as_target.reset();
        page.ClearDocumentConflict(state->id);
        // 网络往返期间可以继续输入。旧保存结果只能确认旧版本，绝不能覆盖
        // 当前正文、设置新保存点，或清掉新编辑的 dirty/恢复缓存。
        if (state->edit_generation.load(std::memory_order_acquire) == generation) {
            state->ui_dirty.store(false, std::memory_order_release);
            page.MarkDocumentSaved(state->id);
        } else {
            page.SetDocumentSaving(state->id, false);
            OnDocumentChanged(state->id);
        }
    }

    void OnSave(ui::EditorSaveRequest request) {
        const auto found = documents.find(request.id);
        if (found == documents.end()) {
            return;
        }
        auto state = found->second;
        if (state->closing.load(std::memory_order_acquire)) {
            return;
        }
        const auto generation = state->edit_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        state->ui_dirty.store(true, std::memory_order_release);
        page.SetDocumentSaving(request.id, true);

        Enqueue(state->lane, [this, state, generation, request = std::move(request)]() mutable {
            editor::EditorStatus update;
            std::optional<editor::SaveResult> saved;
            editor::EditorStatus failure;
            {
                std::scoped_lock lock(state->mutex);
                if (!state->document || state->closing.load(std::memory_order_acquire)) {
                    return;
                }
                update = CheckpointForSave(*state->document, request);
                if (update) {
                    auto result = state->document->Save();
                    if (result) {
                        saved = std::move(result).value();
                        state->conflict = saved->conflict;
                    } else {
                        failure = result.status();
                    }
                }
            }

            PostToUi([this,
                      state,
                      generation,
                      update = std::move(update),
                      saved = std::move(saved),
                      failure = std::move(failure)] {
                if (!IsCurrent(state)) {
                    return;
                }
                if (!update) {
                    page.SetDocumentSaving(state->id, false);
                    ReportError(L"保存远程文件", Utf8ToWide(update.message));
                    return;
                }
                if (!saved) {
                    page.SetDocumentSaving(state->id, false);
                    ReportError(L"保存远程文件", Utf8ToWide(failure.message));
                    return;
                }
                if (saved->outcome == editor::SaveOutcome::Conflict) {
                    state->ui_save_as_target.reset();
                    page.SetDocumentSaving(state->id, false);
                    page.SetDocumentConflict(state->id,
                                             L"远端文件已变化；请选择查看差异、覆盖或另存为。");
                    return;
                }
                FinishSave(state, generation);
            });
        });
    }

    void OnEncodingChanged(ui::EditorSaveRequest request) {
        const auto found = documents.find(request.id);
        if (found == documents.end() || found->second->closing.load(std::memory_order_acquire)) {
            return;
        }
        auto state = found->second;
        const auto generation = state->edit_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        state->ui_dirty.store(true, std::memory_order_release);
        Enqueue(state->lane, [this, state, generation, request = std::move(request)]() mutable {
            editor::EditorStatus status;
            bool dirty = true;
            {
                std::scoped_lock lock(state->mutex);
                if (!state->document || state->closing.load(std::memory_order_acquire))
                    return;
                status = state->document->UpdateText(std::move(request.content),
                                                     ToDocumentEncoding(request.encoding));
                dirty = state->document->dirty();
            }
            if (!status)
                ReportErrorAsync(L"切换文件编码", status.message);
            else
                PostToUi([this, state, generation, dirty] {
                    if (!IsCurrent(state) || state->closing.load() ||
                        state->edit_generation.load() != generation || preparing_shutdown)
                        return;
                    state->ui_dirty.store(dirty);
                    page.SetDocumentDirty(state->id, dirty);
                });
        });
    }

    void OnOverwrite(ui::EditorSaveRequest request) {
        const auto found = documents.find(request.id);
        if (found == documents.end()) {
            return;
        }
        auto state = found->second;
        if (state->closing.load(std::memory_order_acquire) || page.DocumentSaving(state->id)) {
            return;
        }
        const auto target = state->ui_save_as_target;
        if (target && !ReserveSaveTarget(state, *target))
            return;
        const auto generation = state->edit_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        state->ui_dirty.store(true, std::memory_order_release);
        page.SetDocumentSaving(request.id, true);
        const auto old_uri = state->uri;
        Enqueue(state->lane,
                [this, state, old_uri, target, generation, request = std::move(request)]() mutable {
                    editor::EditorStatus update;
                    std::optional<editor::SaveResult> saved;
                    editor::EditorStatus failure;
                    std::optional<ui::EditorDocumentDescriptor> descriptor;
                    transfer::FileUri new_uri;
                    {
                        std::scoped_lock lock(state->mutex);
                        if (!state->document || state->closing.load(std::memory_order_acquire)) {
                            PostToUi([this, state] {
                                ReleaseSaveTarget(state);
                            });
                            return;
                        }
                        update = CheckpointForSave(*state->document, request);
                        if (update) {
                            editor::EditorResult<editor::SaveResult> result =
                                target ? state->document->SaveAs(*target, true)
                                       : state->document->Save(editor::SaveMode::OverwriteRemote);
                            if (result) {
                                saved = std::move(result).value();
                                state->conflict.reset();
                                new_uri = state->document->uri();
                                descriptor = Describe(*state, *state->document, false);
                            } else {
                                failure = result.status();
                            }
                        }
                    }
                    PostToUi([this,
                              state,
                              old_uri,
                              generation,
                              new_uri = std::move(new_uri),
                              descriptor = std::move(descriptor),
                              update = std::move(update),
                              saved = std::move(saved),
                              failure = std::move(failure)]() mutable {
                        ReleaseSaveTarget(state);
                        if (!IsCurrent(state)) {
                            return;
                        }
                        if (!update || !saved) {
                            page.SetDocumentSaving(state->id, false);
                            ReportError(L"覆盖远端文件",
                                        Utf8ToWide(!update ? update.message : failure.message));
                            return;
                        }
                        if (new_uri != old_uri) {
                            uri_index.erase(old_uri.to_string());
                            uri_index[new_uri.to_string()] = state->id;
                            state->uri = new_uri;
                        }
                        FinishSave(state, generation, descriptor);
                    });
                });
    }

    void OnSaveAs(ui::EditorSaveRequest request) {
        const auto found = documents.find(request.id);
        if (found == documents.end()) {
            return;
        }
        if (!save_as_target_resolver) {
            ReportError(L"远程另存为", L"尚未配置远程目标路径选择器，本次未写入远端。");
            return;
        }
        auto state = found->second;
        if (state->closing.load(std::memory_order_acquire) || page.DocumentSaving(state->id))
            return;
        auto target = save_as_target_resolver(state->uri);
        if (!target) {
            return;
        }
        if (!IsCurrent(state) || state->closing.load(std::memory_order_acquire) ||
            page.DocumentSaving(state->id) || !ReserveSaveTarget(state, *target)) {
            return;
        }
        const auto old_uri = state->uri;
        const auto generation = state->edit_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        state->ui_dirty.store(true, std::memory_order_release);
        page.SetDocumentSaving(request.id, true);
        Enqueue(state->lane,
                [this,
                 state,
                 old_uri,
                 generation,
                 target = std::move(*target),
                 request = std::move(request)]() mutable {
                    editor::EditorStatus update;
                    std::optional<editor::SaveResult> saved;
                    editor::EditorStatus failure;
                    std::optional<ui::EditorDocumentDescriptor> descriptor;
                    transfer::FileUri new_uri;
                    {
                        std::scoped_lock lock(state->mutex);
                        if (!state->document || state->closing.load(std::memory_order_acquire)) {
                            PostToUi([this, state] {
                                ReleaseSaveTarget(state);
                            });
                            return;
                        }
                        update = CheckpointForSave(*state->document, request);
                        if (update) {
                            auto result = state->document->SaveAs(target, false);
                            if (result) {
                                saved = std::move(result).value();
                                state->conflict = saved->conflict;
                                if (saved->outcome == editor::SaveOutcome::Saved) {
                                    new_uri = state->document->uri();
                                    descriptor = Describe(*state, *state->document, false);
                                }
                            } else {
                                failure = result.status();
                            }
                        }
                    }
                    PostToUi([this,
                              state,
                              old_uri,
                              generation,
                              selected_target = target,
                              new_uri = std::move(new_uri),
                              update = std::move(update),
                              saved = std::move(saved),
                              failure = std::move(failure),
                              descriptor = std::move(descriptor)]() mutable {
                        ReleaseSaveTarget(state);
                        if (!IsCurrent(state)) {
                            return;
                        }
                        if (!update || !saved) {
                            page.SetDocumentSaving(state->id, false);
                            ReportError(L"远程另存为",
                                        Utf8ToWide(!update ? update.message : failure.message));
                            return;
                        }
                        if (saved->outcome == editor::SaveOutcome::Conflict) {
                            state->ui_save_as_target = selected_target;
                            page.SetDocumentSaving(state->id, false);
                            page.SetDocumentConflict(
                                state->id, L"另存为目标已存在；请选择其他名称，或确认覆盖后重试。");
                            return;
                        }
                        uri_index.erase(old_uri.to_string());
                        uri_index[new_uri.to_string()] = state->id;
                        state->uri = new_uri;
                        FinishSave(state, generation, descriptor);
                    });
                });
    }

    static std::string ChangeReport(std::string original,
                                    editor::TextEncoding original_encoding,
                                    std::string current,
                                    editor::TextEncoding current_encoding) {
        const auto unicode = [](std::string& text, editor::TextEncoding encoding) {
            if (encoding == editor::TextEncoding::Unknown8Bit) {
                if (auto decoded = editor::DecodeGb2312(text))
                    text = std::move(*decoded);
                else
                    text = WideToUtf8(Utf8ToWide(text));
            }
        };
        unicode(original, original_encoding);
        unicode(current, current_encoding);
        constexpr std::size_t limit = 2U * 1024U * 1024U;
        bool truncated = false;
        const auto lines = [&](std::string_view text) {
            auto size = std::min(text.size(), limit);
            while (size < text.size() && size > 0 &&
                   (static_cast<unsigned char>(text[size]) & 0xc0U) == 0x80U)
                --size;
            truncated |= size != text.size();
            text = text.substr(0, size);
            std::vector<std::string_view> result;
            while (!text.empty() && result.size() < 100000) {
                const auto end = text.find('\n');
                auto line = text.substr(0, end);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                result.push_back(line);
                if (end == std::string_view::npos) {
                    text = {};
                    break;
                }
                text.remove_prefix(end + 1);
            }
            truncated |= !text.empty();
            return result;
        };
        const auto before = lines(original), after = lines(current);
        std::string output =
            "原版本：上次打开/成功保存的内容\n当前版本：本次编辑快照（不会写入原文件）\n"
            "- 原始行号 | 删除/修改前\n+ 当前行号 | 新增/修改后\n\n";
        const auto encoding_name = [](editor::TextEncoding encoding) {
            return encoding == editor::TextEncoding::Utf8      ? "UTF-8"
                   : encoding == editor::TextEncoding::Utf8Bom ? "UTF-8 BOM"
                   : encoding == editor::TextEncoding::Gb2312  ? "GB2312"
                                                               : "原始 8 位";
        };
        output += std::string("编码：") + encoding_name(original_encoding) + " → " +
                  encoding_name(current_encoding) + "\n\n";
        // 先剥离公共首尾；空白行被改成一个字符时，后续正文绝不能被当成差异。
        std::size_t prefix = 0, old_end = before.size(), new_end = after.size();
        while (prefix < old_end && prefix < new_end && before[prefix] == after[prefix])
            ++prefix;
        while (old_end > prefix && new_end > prefix && before[old_end - 1] == after[new_end - 1]) {
            --old_end;
            --new_end;
        }
        const int old_count = static_cast<int>(old_end - prefix);
        const int new_count = static_cast<int>(new_end - prefix);
        std::vector<char> edits;
        // Myers 最短编辑路径不依赖“最近的空行”猜测。限制距离、比较次数和轨迹内存，
        // 大面积重写时明确退化为中间差异块，而不是让 UI 等待无界 LCS。
        constexpr int maximum_distance = 256;
        constexpr std::size_t maximum_comparisons = 1000000;
        const int distance_limit = std::min(maximum_distance, old_count + new_count);
        const int offset = distance_limit + 1;
        std::vector<int> furthest(static_cast<std::size_t>(distance_limit * 2 + 3), 0);
        std::vector<std::vector<int>> trace;
        std::size_t comparisons = 0;
        int distance = -1;
        if (std::abs(old_count - new_count) <= distance_limit) {
            for (int d = 0; d <= distance_limit && comparisons < maximum_comparisons; ++d) {
                trace.push_back(furthest);
                for (int k = -d; k <= d && comparisons < maximum_comparisons; k += 2) {
                    ++comparisons;
                    int x =
                        k == -d || (k != d && furthest[offset + k - 1] < furthest[offset + k + 1])
                            ? furthest[offset + k + 1]
                            : furthest[offset + k - 1] + 1;
                    int y = x - k;
                    while (x < old_count && y < new_count && comparisons < maximum_comparisons) {
                        ++comparisons;
                        if (before[prefix + x] != after[prefix + y])
                            break;
                        ++x;
                        ++y;
                    }
                    furthest[offset + k] = x;
                    if (x == old_count && y == new_count) {
                        distance = d;
                        break;
                    }
                }
                if (distance >= 0)
                    break;
            }
        }
        if (distance >= 0) {
            int x = old_count, y = new_count;
            for (int d = distance; d >= 0; --d) {
                const auto& previous = trace[d];
                const int k = x - y;
                const int previous_k =
                    k == -d || (k != d && previous[offset + k - 1] < previous[offset + k + 1])
                        ? k + 1
                        : k - 1;
                const int previous_x = previous[offset + previous_k];
                const int previous_y = previous_x - previous_k;
                while (x > previous_x && y > previous_y) {
                    edits.push_back('=');
                    --x;
                    --y;
                }
                if (d > 0) {
                    if (x == previous_x) {
                        edits.push_back('+');
                        --y;
                    } else {
                        edits.push_back('-');
                        --x;
                    }
                }
            }
            std::reverse(edits.begin(), edits.end());
        } else {
            output += "[变更较大，以下按连续差异块显示；相同首尾已省略]\n";
            edits.insert(edits.end(), old_count, '-');
            edits.insert(edits.end(), new_count, '+');
        }
        const auto append = [&](char sign, std::size_t line, std::string_view value) {
            if (output.size() + value.size() + 32 > limit) {
                truncated = true;
                return false;
            }
            output += sign;
            output += " " + std::to_string(line + 1) + " | ";
            output += value;
            output += '\n';
            return true;
        };
        const bool changed = old_count != 0 || new_count != 0;
        std::size_t i = prefix, j = prefix;
        bool in_hunk = false;
        for (const char edit : edits) {
            if (edit == '=') {
                ++i;
                ++j;
                in_hunk = false;
                continue;
            }
            if (!in_hunk) {
                output += "@@ 原始行 " + std::to_string(i + 1) + " / 当前行 " +
                          std::to_string(j + 1) + " @@\n";
                in_hunk = true;
            }
            if (edit == '-') {
                if (!append('-', i, before[i]))
                    break;
                ++i;
            } else {
                if (!append('+', j, after[j]))
                    break;
                ++j;
            }
        }
        if (!changed)
            output += original == current && original_encoding == current_encoding
                          ? "没有内容差异。\n"
                          : "文本行相同；编码或换行符有变化。\n";
        if (truncated)
            output += "\n[差异视图已按大小/行数上限截断，原文件和编辑内容不受影响]\n";
        return output;
    }

    void OnChanges(ui::EditorDocumentId id) {
        const auto found = documents.find(id);
        if (found == documents.end() || found->second->closing.load() || preparing_shutdown)
            return;
        auto snapshot = page.DocumentSnapshot(id);
        if (!snapshot)
            return;
        auto state = found->second;
        Enqueue(state->lane, [this, state, snapshot = std::move(*snapshot)]() mutable {
            auto original = [&] {
                std::scoped_lock lock(state->mutex);
                return state->document->OriginalText();
            }();
            if (!original) {
                ReportErrorAsync(L"查看改动",
                                 "此恢复草稿缺少原始快照，无法准确比较；现有内容保持不变。请勿把服"
                                 "务器当前内容当作原版本。");
                return;
            }
            auto report = ChangeReport(std::move(original.value().text),
                                       original.value().encoding,
                                       std::move(snapshot.content),
                                       ToDocumentEncoding(snapshot.encoding));
            PostToUi([this, state, report = std::move(report)]() mutable {
                if (!IsCurrent(state) || state->closing.load() || preparing_shutdown)
                    return;
                if (!page.ShowChangesView(L"改动 · " + page.DocumentDisplayName(state->id),
                                          std::move(report)))
                    ReportError(L"查看改动", L"无法创建对比窗口，当前编辑内容保持不变。");
            });
        });
    }

    void OnDiff(ui::EditorDocumentId id) {
        const auto found = documents.find(id);
        if (found == documents.end()) {
            return;
        }
        auto state = found->second;
        std::optional<editor::ConflictSnapshot> snapshot;
        {
            std::scoped_lock lock(state->mutex);
            snapshot = state->conflict;
        }
        if (!snapshot) {
            return;
        }
        if (conflict_diff) {
            conflict_diff(id, state->uri, *snapshot);
            return;
        }
        std::wstring message = L"已检测到远端版本变化。";
        if (snapshot->remote_missing) {
            message = L"远端文件已被删除。";
        } else if (snapshot->remote_too_large) {
            message = L"远端版本超过 20 MB，未载入差异内容。";
        } else if (snapshot->remote_is_binary) {
            message = L"远端版本为二进制内容，不能进行文本差异比较。";
        } else {
            message.append(L"\r\n本地字符数：");
            message.append(std::to_wstring(snapshot->local_text.size()));
            message.append(L"\r\n远端字符数：");
            message.append(std::to_wstring(snapshot->remote_text.size()));
        }
        ::SLink::ui::ShowMessageDialog(
            page.hwnd(), message.c_str(), L"远端差异", MB_OK | MB_ICONINFORMATION);
    }

    static editor::EditorStatus CompleteClose(PendingClose& pending, bool close = true) try {
        std::scoped_lock lock(pending.state->mutex);
        if (pending.completed || !pending.state->document)
            return editor::EditorStatus::Ok();
        auto& document = *pending.state->document;
        if (pending.decision == CloseDecision::Discard) {
            auto status = document.DiscardChanges();
            pending.completed = status.ok();
            return status;
        }
        if (pending.snapshot) {
            auto status = document.UpdateText(pending.snapshot->content,
                                              ToDocumentEncoding(pending.snapshot->encoding));
            // GB2312 无法表达新字符不应妨碍保留 Unicode 恢复副本。
            if (!status && status.code == editor::EditorErrc::Unsupported)
                status = document.UpdateText(pending.snapshot->content);
            if (!status)
                return status;
        }
        if (pending.decision == CloseDecision::Save) {
            auto result = document.Save();
            if (!result)
                return result.status();
            pending.state->conflict = result.value().conflict;
            pending.save_conflict = result.value().outcome == editor::SaveOutcome::Conflict;
            if (pending.save_conflict)
                return editor::EditorStatus::Error(editor::EditorErrc::InvalidArgument,
                                                   "Remote file changed; saving was cancelled");
            pending.saved = true;
        }
        if (!close)
            return editor::EditorStatus::Ok();
        auto status = document.Close();
        pending.completed = status.ok();
        return status;
    } catch (const std::exception& error) {
        return editor::EditorStatus::Error(editor::EditorErrc::IoError, error.what());
    } catch (...) {
        return editor::EditorStatus::Error(editor::EditorErrc::IoError,
                                           "Cannot preserve editor snapshot");
    }

    CloseDecision ChooseClose(ui::EditorDocumentId id, const transfer::FileUri& uri) {
        if (close_confirmation)
            return close_confirmation(uri);
        const auto choice = ui::ShowUnsavedFileDialog(page.hwnd(), page.DocumentDisplayName(id));
        return choice == ui::UnsavedFileChoice::Save        ? CloseDecision::Save
               : choice == ui::UnsavedFileChoice::Discard   ? CloseDecision::Discard
               : choice == ui::UnsavedFileChoice::KeepDraft ? CloseDecision::KeepDraft
                                                            : CloseDecision::Cancel;
    }

    void OnClose(ui::EditorDocumentId id, bool dirty) {
        const auto found = documents.find(id);
        if (found == documents.end()) {
            page.CloseDocument(id);
            return;
        }
        auto state = found->second;
        if (state->closing.load(std::memory_order_acquire) || preparing_shutdown)
            return;
        const auto decision = dirty ? ChooseClose(state->id, state->uri) : CloseDecision::Discard;
        if (decision == CloseDecision::Cancel)
            return;

        auto pending = std::make_shared<PendingClose>();
        pending->state = state;
        pending->decision = decision;
        if (dirty && decision != CloseDecision::Discard)
            pending->snapshot = page.DocumentSnapshot(id);
        if (dirty && decision != CloseDecision::Discard && !pending->snapshot) {
            ReportError(L"保留未保存内容", L"无法读取正文快照，标签已保留。");
            return;
        }
        pending_closes.push_back(pending);
        state->closing.store(true, std::memory_order_release);
        state->edit_generation.fetch_add(1, std::memory_order_acq_rel);
        scheduler->CancelLatest("checkpoint:" + std::to_string(id));
        // 不先撤掉唯一的可见正文。临时只读保证提交快照与最终移除的是同一版本。
        page.SetDocumentClosing(id, true);
        if (decision == CloseDecision::Save)
            page.SetDocumentSaving(id, true);

        Enqueue(state->lane, [this, pending] {
            const auto status = CompleteClose(*pending);
            PostToUi([this, pending, status] {
                std::erase(pending_closes, pending);
                const auto& state = pending->state;
                if (status) {
                    uri_index.erase(state->uri.to_string());
                    documents.erase(state->id);
                    page.CloseDocument(state->id);
                } else {
                    state->closing.store(false, std::memory_order_release);
                    page.SetDocumentSaving(state->id, false);
                    page.SetDocumentClosing(state->id, false);
                    if (pending->save_conflict)
                        page.SetDocumentConflict(
                            state->id, L"远端文件已变化；关闭已取消，请查看差异、覆盖或另存为。");
                    else
                        ReportError(L"关闭文件失败，修改仍保留", Utf8ToWide(status.message));
                }
            });
        });
    }

    void PrepareForShutdown(std::function<void(bool)> completed) {
        if (preparing_shutdown) {
            completed(false);
            return;
        }
        preparing_shutdown = true;
        std::vector<std::shared_ptr<PendingClose>> snapshots;
        // 模态确认期间仍会消费异步完成消息，不能持有 unordered_map 的迭代器。
        std::vector<std::shared_ptr<DocumentState>> candidates;
        for (const auto& [id, state] : documents)
            candidates.push_back(state);
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a->id < b->id;
        });
        for (const auto& state : candidates) {
            if (!IsCurrent(state) || state->closing.load(std::memory_order_acquire))
                continue;
            const auto id = state->id;
            auto item = std::make_shared<PendingClose>();
            item->state = state;
            const bool dirty = page.DocumentDirty(id);
            item->decision = dirty ? ChooseClose(state->id, state->uri) : CloseDecision::Discard;
            if (item->decision == CloseDecision::Cancel) {
                preparing_shutdown = false;
                completed(false);
                return;
            }
            if (dirty && item->decision != CloseDecision::Discard) {
                item->snapshot = page.DocumentSnapshot(id);
                if (!item->snapshot) {
                    for (const auto& previous : snapshots)
                        page.SetDocumentClosing(previous->state->id, false);
                    preparing_shutdown = false;
                    ReportError(L"退出已取消", L"无法读取正文快照，窗口已保留。");
                    completed(false);
                    return;
                }
            }
            snapshots.push_back(std::move(item));
        }
        // 所有选择确认后才冻结正文；取消任何一个确认都不提前删除其他文件草稿。
        for (const auto& item : snapshots) {
            item->state->edit_generation.fetch_add(1);
            scheduler->CancelLatest("checkpoint:" + std::to_string(item->state->id));
            page.SetDocumentClosing(item->state->id, true);
        }
        // 先完成保存/保留草稿；这些操作失败时不执行其它文件的“放弃”。
        auto closing_snapshots = pending_closes;
        Enqueue("control",
                [this,
                 snapshots = std::move(snapshots),
                 closing_snapshots = std::move(closing_snapshots),
                 completed = std::move(completed)] {
                    auto status = editor::EditorStatus::Ok();
                    try {
                        for (const auto& item : snapshots) {
                            if (item->decision == CloseDecision::Discard)
                                continue;
                            const auto current = CompleteClose(*item, false);
                            if (!current && status)
                                status = current;
                        }
                        // 单标签关闭失败的 UI 回调可能尚未消费，不能因此放行退出。
                        // 该标签仍有 readonly UI 快照；其缓存失败由下面的检查直接报告。
                        for (const auto& item : closing_snapshots) {
                            const auto current = CompleteClose(*item, false);
                            if (!current && status)
                                status = current;
                        }
                        if (status) {
                            for (const auto& item : snapshots) {
                                if (item->decision != CloseDecision::Discard)
                                    continue;
                                const auto current = CompleteClose(*item);
                                if (!current) {
                                    status = current;
                                    break;
                                }
                            }
                        }
                    } catch (const std::exception& error) {
                        status =
                            editor::EditorStatus::Error(editor::EditorErrc::IoError, error.what());
                    }
                    PostToUi([this, snapshots, completed, status] {
                        preparing_shutdown = false;
                        for (const auto& item : snapshots) {
                            const auto& state = item->state;
                            if (item->completed) {
                                state->closing.store(true);
                                uri_index.erase(state->uri.to_string());
                                documents.erase(state->id);
                                page.CloseDocument(state->id);
                            } else {
                                if (item->saved) {
                                    state->ui_dirty.store(false);
                                    page.MarkDocumentSaved(state->id);
                                }
                                page.SetDocumentClosing(state->id, false);
                                if (item->save_conflict)
                                    page.SetDocumentConflict(
                                        state->id,
                                        L"远端文件已变化；退出已取消，请查看差异、覆盖或另存为。");
                            }
                        }
                        if (!status)
                            ReportError(L"退出已取消：文件操作未完成", Utf8ToWide(status.message));
                        completed(status.ok());
                    });
                });
    }
};

EditorController::EditorController(ui::EditorPage& page,
                                   const storage::AppPaths& paths,
                                   EndpointResolver endpoint_resolver)
    : impl_(std::make_unique<Impl>(page, paths, std::move(endpoint_resolver))) {}

EditorController::~EditorController() = default;

void EditorController::PrepareForShutdown(std::function<void(bool)> completed) {
    impl_->PrepareForShutdown(std::move(completed));
}

void EditorController::Open(transfer::FileUri uri) {
    impl_->Open(std::move(uri));
}

void EditorController::RecoverUnsaved() {
    impl_->RecoverUnsaved();
}

void EditorController::RebindEndpoint(std::string server_id,
                                      std::shared_ptr<transfer::IFileEndpoint> endpoint) {
    impl_->RebindEndpoint(std::move(server_id), std::move(endpoint));
}

void EditorController::SetFileActivationResolver(FileActivationResolver resolver) {
    impl_->file_activation_resolver = std::move(resolver);
}

void EditorController::SetSaveAsTargetResolver(SaveAsTargetResolver resolver) {
    impl_->save_as_target_resolver = std::move(resolver);
}

void EditorController::SetCloseConfirmation(CloseConfirmation confirmation) {
    impl_->close_confirmation = std::move(confirmation);
}

void EditorController::SetDirectoryActivationCallback(DirectoryActivationCallback callback) {
    impl_->directory_activation = std::move(callback);
}

void EditorController::SetConflictDiffCallback(ConflictDiffCallback callback) {
    impl_->conflict_diff = std::move(callback);
}

void EditorController::SetErrorCallback(ErrorCallback callback) {
    impl_->error_callback = std::move(callback);
}

std::size_t EditorController::document_count() const noexcept {
    return impl_->documents.size();
}

} // namespace SLink::app
